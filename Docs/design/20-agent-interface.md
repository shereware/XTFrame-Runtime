# 20 · 開發用 AI 接入點「提線」（Marionette，`xt_agent`，拋棄式）

版本：0.2.1（M10）。狀態：設計定案，未實作。

**代號**：提線（Marionette）。指令 `Tools/Scripts/xa.sh`。由主模型（Claude）自行維護與擴充：工作中覺得不夠用、不好用或想做實驗，直接改，不需另外請示。

**定位**：這是給「讀過本專案原始碼的 Claude Code」用的開發輔助，用來測試、除錯、製作 demo。使用者不是人類。開發期結束即丟棄。因此：

- 不做權限、不做沙箱、不做 token、不做唯讀模式、不限制檔案路徑，所有能力全部開放。
- 不寫使用說明、不寫 JSON Schema、不做 MCP。**原始碼就是文件**：接點名稱與引數順序直接照 C++ 的 `類別.方法`，要知道怎麼用就去讀標頭。
- 追求「最少程式碼暴露最多操作」：核心物件用一行一個方法的模板綁定，Qt 端用 `QObject` 的通用反射。
- 面向使用者的 AI 接點是另一件事，屆時重新設計（20.9）；本章的接點目錄可作為那時的起點。

## 20.1 形態

只有一個宿主：**編輯器行程本身**。

```
xtframe-editor [file.xt] --agent <port> [--headless]
```

- `--agent` 在 `127.0.0.1:<port>` 開一個極簡 HTTP 端點（`QTcpServer`，GUI 執行緒，單執行緒處理，無併發問題）。
- `--headless`（既有旗標，`QT_QPA_PLATFORM=offscreen`）= 無視窗常駐伺服器；不加 = 你眼前的編輯器，AI 的操作即時可見。
- 用戶端不需要另寫程式：`curl -s 127.0.0.1:<port> -d '<json>'`。附一個 `Tools/Scripts/xa.sh` 包裝省去引號（20.7）。
- 狀態（文件、undo 堆疊、預覽時間、流圖 runtime）留在行程內，跨多次呼叫持續存在。

建置：CMake 選項 `XT_BUILD_AGENT`（預設 OFF），preset `vs2022-agent`（= editor + agent，`build/vs2022-agent`）。全部程式碼放在 `Code/Editor/DevTools/agent/`（不在 `local_ci.sh` 的掃描與 merge gate 範圍內），編輯器 `main.cpp` 只有一個 `#ifdef XT_BUILD_AGENT` 的掛點。`Tools/Scripts/dev_check.sh --agent` 改用該 preset。

## 20.2 訊息

`POST /`，body 為一個請求或請求陣列（依序執行，遇錯不中斷）：

```json
{"op":"doc.setAttribute","args":["#inc","class","big"]}
[{"op":"cmd.SetStyle","args":["#inc","width","Normal","120px"]},{"op":"session.tick","args":[0.016]},{"op":"render.png","args":["C:/tmp/a.png"]}]
```

回應：`{"ok":true,"result":<任意 JSON>}` 或 `{"ok":false,"error":"kInvalidArgument","detail":"…"}`；陣列請求回陣列。例外一律接住轉成錯誤，行程不得因接入點的錯誤請求而崩潰（斷言失敗除外，那是要抓的 bug）。

`GET /` 回 `help`（見下）。

## 20.3 綁定機制（`Code/Editor/DevTools/agent/include/xt/agent/binder.h`，零 Qt）

```cpp
class Dispatcher {
public:
    template <class F> void bind(std::string name, const char* signature, F&& fn);   // fn 為任意可呼叫物；引數型別由模板推導，從 JSON 陣列逐一轉換
    nlohmann::json call(const nlohmann::json& request);     // 單一或陣列
    nlohmann::json help(std::string_view prefix = {}) const; // [{op, signature}]，signature 是巨集擷取的 C++ 宣告字串
};
#define XT_BIND(disp, name, expr) (disp).bind(name, #expr, expr)
```

型別轉換（`from_json`/`to_json` 特化，全部集中在 `conv.h`）：

| C++ | JSON |
|---|---|
| `editor::NodeId` | `"#id"`、`"n<index>.<gen>"`、`"@<ordinal>"`、或選擇器字串（取第一個命中）；輸出為 `{ref:"n12.3", tag, id}` |
| `const PropertyDesc&` | 屬性名稱字串 |
| `StyleState`、各 enum | 名稱字串 |
| `StyleValue` | 字串（原文，經 `parseStyleValue`）；輸出 `{text, important}` |
| `xt::Value` | 數字／字串／陣列；輸出帶 `type` |
| `Vec2/Vec3/Rect/Color/Mat2x3/Mat4` | 陣列 |
| `Status` | 名稱字串；非 `kOk` 轉為錯誤 |
| `std::span<T>`、`std::vector<T>`、`std::optional<T>` | 陣列／null |
| 核心 `xt::NodeId`、`ElementRef` | 數字；`ElementRef` 亦接受 `"#id"` |

新增一個接點 = `bindings_*.cpp` 裡多一行 `XT_BIND`。

## 20.4 接點目錄

物件前綴對應到行程內的實體；方法名與引數順序照標頭，除非下表另註。

| 前綴 | 實體 | 綁定範圍 |
|---|---|---|
| `doc.` | `editor::Document` | 全部公開方法：樹、屬性、文字、`toXt/toXts`、`sourceOf`、`style/cascadedStyle/setStyle`、`rulesMatching/specificity/ruleSource`、`keyframes/setKeyframes/removeKeyframes`、`reloadStylesheet`、`load/loadFromString` |
| `sheet.` | `editor::Stylesheet`（第一個引數為 fileId） | `rules`、`addRule`、`removeRule`、`setSelector`，以及讀寫某規則的宣告 |
| `cmd.` | `CommandStack` + 每個 `Command` 的建構 | `cmd.<CommandName>`（建構並 `session.push`）、`undo`、`redo`、`beginMacro/endMacro`、`list`、`clear` |
| `preview.` | `PreviewSession` | 全部公開方法 |
| `core.` | `xt::Document`（預覽中的核心文件） | `byId/query`、`ElementRef` 的全部方法（第一引數為元素）、`play/pause/seek/resume/setSpeed/animationTime/animationsOf`、`nodeToScreen/nodeToScreen3/cameraOf/setNodeProperty/setElementState`、`addListener`（事件寫入 `events` 環狀緩衝） |
| `ctx.` | `xt::Context` | `update`、`pushInput`、`setViewport`、`time`、`textInputRect` |
| `flow.` | `flow::FlowGraph` / `FlowRuntime` | `get/set`（JSON）、`parse`、`validate`、`values`、`edgeLastFired`、`connectionPoints`、`nodeTypeTable` |
| `session.` | `EditorSession` | `open/save/saveAs/path/isModified`、`selection/setSelection`、`mode/setMode`、`tick`、`restartDemo`、`flowErrors`、`recordKeyframes/playhead`、`quit`、`crash(kind)`（故意崩潰：`abort` / `av` / `terminate`，用來驗證崩潰紀錄真的會寫檔） |
| `fx.` | 效果輔助 | `EffectStack::parse/toText`、`effectParamTable`、模板 `list/load/apply` |
| `props.` | 反射表 | `table`、`find` |
| `compile.` | `xt::compile` | `compileDocument`、`compileDocumentFromStrings`、`parseStyleValue`、`parseEffectList`；回診斷 |
| `fs.` | 檔案系統 | `read/write/list/exists/remove/mkdir`，無路徑限制 |
| `time.` | 幀迴圈 | `pause`（停編輯器的 `QTimer`，時間只由 `session.tick` 推進）、`resume`、`step(n, dt)` |
| `events.` | 事件環狀緩衝 | `since(seq)`、`clear` |

## 20.5 傾印（除錯用，`dump.`）

全部直接讀內部狀態，輸出 JSON；為此允許 `Code/Editor/DevTools/agent` 引用 `Code/Runtime/core/src` 等內部標頭。

| 接點 | 內容 |
|---|---|
| `dump.tree` | 核心節點樹：`NodeId`、kind、對應元素/場景名、子節點 |
| `dump.node(node)` | `NodeArena` 該節點的全部欄位：local/world/world3、is3D、opacity、dirty 旗標、效果池 |
| `dump.props(node)` | 每個適用 PropId 的目前值、來源層（base/track/transition/override） |
| `dump.style(element)` | `ComputedStyle` 全欄位 |
| `dump.layout(element)` | `LayoutBox`、文字行與 run、字形數 |
| `dump.animator` | 所有實例：timeline、localT、speed、durationScale、狀態；`dump.timeline(id)` 傾印軌道與關鍵幀 |
| `dump.commands([filter])` | 目前幀的 `CommandList`：逐條（kind、bounds、paint 摘要）與統計 |
| `dump.ir` | 已編譯 IR 的區塊表與大小 |
| `dump.fonts` | 已載入字型、每個 run 實際使用的字型（含是否為系統後備） |

## 20.6 畫面與 Qt 反射

| 接點 | 內容 |
|---|---|
| `render.png(path, [w, h, dpr])` | SW 渲染目前幀存 PNG（AI 用讀圖工具看） |
| `render.node(path, node, [pad])` | 裁到節點（含效果擴張） |
| `render.diff(a, b, out)` | 差異像素數、最大通道差、diff 圖 |
| `qt.tree([root])` | widget 樹：路徑、類別、`objectName`、幾何、可見、文字/標題 |
| `qt.grab(path, file)` | 任一 widget（`window`、面板標題或 `qt.tree` 的路徑）`grab()` 存 PNG |
| `qt.get/set(path, property[, value])` | 任意 `Q_PROPERTY` |
| `qt.invoke(path, method, args…)` | `QMetaObject::invokeMethod`（slot 與 `Q_INVOKABLE`） |
| `qt.action(text)` | 依文字觸發選單/工具列 `QAction` |
| `qt.mouse(path, action, x, y[, button, modifiers])`、`qt.key(path, key[, text, modifiers])`、`qt.wheel`、`qt.drag(path, x0,y0,x1,y1, steps)` | 對 widget 送合成事件（`QTest` 式的 `QApplication::sendEvent`） |
| `qt.processEvents([ms])` | 讓事件迴圈轉一下 |

widget 路徑：`window` 為主視窗；其餘為 `類別名#objectName` 以 `/` 串接，`objectName` 缺的以同類別的序號代替。實作時替各面板與主要子 widget 補上 `objectName`。

## 20.7 我怎麼用它

```
Tools/Scripts/xa.sh start [file.xt] [--window]     # 背景啟動（預設 headless），記下 port 到 build/vs2022-agent/agent.port
Tools/Scripts/xa.sh <op> [args…]                   # 每個引數若是合法 JSON 就當 JSON，否則當字串
Tools/Scripts/xa.sh -f batch.json                  # 送整批
Tools/Scripts/xa.sh stop
```

典型迴圈：`start` → `time.pause` → 一批修改 → `session.tick` → `render.png` / `dump.*` → 讀圖與 JSON → 再改。除錯面板時加 `--window`，用 `qt.grab` 看面板、`qt.mouse/key/drag` 操作。Demo 製作：把一批請求存成 `demos/<name>.json`，可重播。

驗收清單（原本要人手操作的那份）改由我透過接入點先跑一遍，再請使用者看成品。

## 20.8 任務（併入 16 章 M10）

| 任務 | 內容 |
|---|---|
| T-71a | `Code/Editor/DevTools/agent`：`Dispatcher`/`XT_BIND`/`conv.h`、`doc.`/`sheet.`/`cmd.`/`props.`/`fx.`/`compile.`/`fs.` 綁定、binder 的單元測試、CMake 選項與 preset、`dev_check.sh --agent` |
| T-71b | `preview.`/`core.`/`ctx.`/`flow.`/`events.` 綁定、`dump.*`、`render.*` |
| T-72 | 編輯器宿主：`--agent` HTTP 端點、`session.`/`time.`、`qt.*` 通用反射與合成輸入、各面板補 `objectName`、`Tools/Scripts/xa.sh` |
| T-73 | 我親自用接入點跑完驗收清單並修掉發現的問題；做出一個新 demo（效果、3D、流圖），批次檔入庫 |

T-71b 與 T-72 都只依賴 T-71a，可平行。這是拋棄式開發工具：任務單只規定接點與檔案位置，實作細節由實作者自行決定（14.8 的「無設計空間」規則對 `Code/Editor/DevTools/` 不適用）。

## 20.9 之後的使用者版（不在 0.2.1）

面向使用者的 AI 協作接點到時另立章節重新設計，需要處理本章刻意忽略的事：協定標準化（MCP）、工具粒度與上下文成本、權限與路徑沙箱、唯讀模式、歷史標記與可中斷、說明與 schema、決定性重播、多用戶端。本章的接點目錄與綁定機制可沿用為底層。
