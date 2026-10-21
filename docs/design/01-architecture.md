# 01 · 架構與執行時管線

## 1.1 兩階段工作流

```
離線                                     執行時
──────────────────────────────           ─────────────────────────────────────────────
.xt   ─┐                                 .xtir ──► IrReader ──► Document
.xts  ─┼─► xt_compile ──► .xtir                     │
.json ─┘   (HTML/CSS/Lottie)                          ▼
                                          每幀：Context::update(dt)
                                            1. 事件分派、狀態偽類更新
                                            2. 樣式重新解析（僅 dirty 元素）
                                            3. Animator 評估所有 TimelineInstance
                                            4. 佈局（僅 dirty 子樹）
                                            5. 場景圖世界變換更新
                                          Context::render(CommandList&)
                                            6. 遍歷場景圖產生命令
                                                     │
                                                     ▼
                                          後端（xt_sw / xt_gl / 引擎自訂）
```

執行時**沒有任何文字格式解析器**。`xt_core` 只讀 `.xtir`。

## 1.2 核心物件關係

```
Context
 ├─ FontProvider*            （宿主注入，可為 NullFontProvider）
 ├─ AssetResolver*           （宿主注入，解析圖片位元組）
 ├─ Document[]               （可同時載入多份，各自獨立）
 │   ├─ IrBlob               （唯讀，Document 持有所有權或借用）
 │   ├─ NodeArena            （所有 Node 的 SoA 陣列，含 Element 與場景節點）
 │   ├─ StyleEngine          （選擇器匹配、層疊、計算值）
 │   ├─ Animator             （TimelineInstance 池、transition 池、屬性覆寫）
 │   ├─ LayoutEngine
 │   ├─ EventDispatcher
 │   └─ Painter              （場景圖 → CommandList）
 └─ Clock                    （由 update(dt) 累加，不讀系統時間）
```

## 1.3 Node 儲存：Structure of Arrays

所有節點放在 `NodeArena` 的平行陣列中，以 `NodeId`（`uint32_t`，索引）互相參照。理由：時間軸每幀寫入大量屬性，SoA 讓 Animator 直接以 `(NodeId, PropId)` 定址，不需要虛擬呼叫。

```cpp
struct NodeArena {
    // 拓撲
    std::vector<NodeKind>   kind;
    std::vector<NodeId>     parent;        // kInvalidNode 表示根
    std::vector<NodeId>     firstChild;
    std::vector<NodeId>     nextSibling;
    std::vector<NodeId>     lastChild;     // 加速 append
    // 通用屬性（每個節點都有）
    std::vector<NodeCommon> common;        // 見 03
    // 各種類專屬資料，以 kind 對應的索引查表
    std::vector<uint32_t>   kindIndex;     // 指向 paths / elements / texts / images 的索引
    std::vector<PathNodeData>    paths;
    std::vector<ElementData>     elements;
    std::vector<TextNodeData>    texts;
    std::vector<ImageNodeData>   images;
    // 髒旗標
    std::vector<uint8_t>    dirty;         // DirtyBits 位元遮罩
};
```

節點刪除採「墓碑 + 空閒清單」：`kind[id] = NodeKind::Free`，`freeList` 記錄可重用索引。第一版不做壓實。**因此 NodeId 的大小與父子順序無關**，任何需要「父先於子」的演算法必須做真正的前序走訪。

`NodeArena` 的走訪輔助（`traversePreorder/Postorder`）共用同一個內部堆疊，**不可重入**：走訪回呼中不得再呼叫 `markDirty`（其向下傳播也用該堆疊）或其他走訪。需要在走訪中修改髒旗標的引擎（StyleEngine、LayoutEngine）自備區域堆疊。

## 1.4 髒旗標

```cpp
enum DirtyBits : uint8_t {
    kDirtyStyle     = 1 << 0,  // 需重新解析計算樣式
    kDirtyLayout    = 1 << 1,  // 需重新佈局自身與子孫
    kDirtyTransform = 1 << 2,  // 世界變換需重算
    kDirtyPaint     = 1 << 3,  // 視覺屬性變了但不影響佈局
    kDirtyTree      = 1 << 4,  // 子節點增刪
};
```

規則：
- 修改任何 Element 的樣式相關屬性 → `kDirtyStyle`，向上傳播到根（因為選擇器可能依賴子孫狀態，第一版保守處理）。
- Animator 寫入佈局屬性（`Width`、`Margin*` 等，見 03 表格的 L 欄）→ 該節點 `kDirtyLayout`，並向上傳播到最近的 `position: absolute` 或根。
- Animator 寫入變換屬性 → `kDirtyTransform`，向下傳播由世界變換更新階段處理。
- 其他屬性 → `kDirtyPaint`。**`worldOpacity` 快取（T-158 #159 修訂）**：世界變換更新階段除了 `kDirtyTransform`/`kDirtyTree` 之外，對帶 `kDirtyPaint` 的節點（以及父節點 `worldOpacity` 剛重算的子孫）也要重算 `worldOpacity`；否則只改 opacity 的動畫/`:hover`/class 切換會讓 `worldOpacity` 停在舊值（0 時整棵子樹被 Painter 與 hit test 跳過）。矩陣的重算條件不變。
- 向上傳播（Style、Layout）遇到**已帶有該旗標的祖先即停止**：旗標只在幀末由 `clearAllDirty` 整批清除，因此該祖先之上必已標記。`clearDirty(NodeId)` 只供測試使用，不得在幀中途對單一節點呼叫。

## 1.5 每幀順序（精確）

`Context::update(float dt)`：

1. `clock += dt`。
2. 處理宿主自上幀以來排隊的輸入事件（見 08）。事件處理可能改狀態偽類、觸發 C++ 回呼、呼叫 API 改屬性。
3. 對每份 Document：
   1. `StyleEngine::resolve()`：對 `kDirtyStyle` 的節點重新匹配與層疊，產出 `ComputedStyle`，寫入 Element 屬性的**基底值**；若某屬性有 `transition` 宣告且基底值改變，向 Animator 註冊 transition。
   2. `Animator::evaluate(clock)`：依 04 的層疊順序算出每個被動畫屬性的最終值，寫回 NodeArena，設定對應髒旗標。
   3. `LayoutEngine::layout()`：對有 `kDirtyLayout` 的子樹執行 06 的演算法，寫入 `ElementData::box`。
   4. 世界變換更新：深度優先遍歷有 `kDirtyTransform` 的子樹，`world = parentWorld * local`。
   5. 清除所有髒旗標。

`Context::render(CommandList& out)`：

6. `out.clear()`；每份 Document 依載入順序，Painter 深度優先遍歷場景圖，依 09 產生命令。

`update` 與 `render` 必須在同一執行緒依序呼叫。`render` 不改變任何狀態，可連續呼叫多次得到相同輸出（黃金圖測試依賴此性質）。

## 1.6 執行緒模型

- `Context` 及其所有物件**單執行緒**。宿主要在其他執行緒做事（例如 GL 後端在渲染執行緒消費 CommandList），由宿主自行複製 CommandList 或做同步。
- `CommandList` 是 POD 資料加幾個 `std::vector`，可被 `std::move` 到別的執行緒。
- `FontProvider` 可能被 `LayoutEngine` 在 `update` 中呼叫，也可能被後端在渲染時呼叫；其實作必須執行緒安全（`xt_text` 用 mutex）。

## 1.7 記憶體策略

- 每幀零堆疊配置目標：`CommandList` 的 vector 在 `clear()` 後保留容量；`Animator` 的暫存 vector 同樣保留。
- `NodeArena` 的 vector 只增不減。
- 字串：執行時不存 `std::string`，全部是指向 IR 字串表的 `StringId`；API 查詢時回傳 `std::string_view` 指向 IR 記憶體。因此 **Document 存活期間 IrBlob 必須存活**。
- 路徑資料：場景節點的路徑點存於 `PathNodeData::points`（`std::vector<float>`）；Animator 插值路徑時直接原地寫入，不配置。

## 1.8 錯誤處理

- 核心不用例外。所有可失敗的公開函式回傳 `xt::Result<T>` 或 `xt::Status`（見 14）。
- IR 載入做完整邊界檢查；任何越界或版本不符回傳 `Status::kInvalidIr`，Document 不建立。
- 執行時遇到不可能的狀態（例如 NodeId 越界）使用 `XT_ASSERT`，Release 建置下編譯為 `[[assume]]` 風格的 no-op。
