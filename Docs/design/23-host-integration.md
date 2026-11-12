# 23 · 宿主整合

版本：0.3。狀態：設計定案（2026-09-23），任務單 T-97～T-103。

目的：讓任何遊戲引擎（本章不提任何引擎的專有 API）用最少的接點把 XTFrame 當「一張會動的 RGBA 貼圖 + 事件」接進去，而且只上傳有變化的區域。同時把宿主會碰到的介面凍結成一份清單。M11 的 bundle 是內容契約，本章是執行期契約。

不在本章：GL 後端直接畫進宿主 FBO 的路徑（10.3 已有，維持）、各宿主插件本身（各自的倉庫）。

**原則（2026-10-10 定案）**：本倉沒有宿主專屬功能。宿主友善 = 通用接點（目錄 bundle、可嵌入的 `EditorWorkspace`、本章與 13 章的宿主契約）；任何只為某個宿主存在的碼、格式、文件段落放在那個宿主的倉。各宿主的實作記錄（含 CryEngine 插件的 CE-nn）不在本倉文件，CE 的在 CE 插件倉 `docs/08-xtframe-integration-log.md`。

## 23.1 宿主看到的東西

```
每幀：
  host events ──pushInput──▶ Context ──update(dt)──▶ render(list, &damage)

> **寫入的生效時機（T-561，2026-10-31）**：`update(dt)` 內部順序是 輸入派送（hover 重判、事件路由；觸發器與 xt_flow 的**脈衝**邊在這裡同步執行）→ `ContextListener::onBeforeDocumentUpdate` → 各 Document 的樣式解算／animator／layout。`render` **不重解**樣式。所以宿主在 `update` 返回後、`render` 之前做的寫入（class、style、text、port）只標 dirty，**下一次 `update` 才生效**，transition 的起點時鐘也取下一幀；要與 xt_flow 同幀，必須在事件回呼內或 `onBeforeDocumentUpdate`／C ABI `xt_surface_set_update_hook` 內寫。值型訊號（xt_flow 按幀合併）本來就在 update 之後的 tick 送出，兩邊一致。
                                                        │
                        SwRenderer::render(list, ..., &damage) ──▶ RGBA 緩衝（宿主擁有）
                                                        │
                        for r in damage.rects: host.uploadRegion(texture, r, rgba)
```

- 宿主擁有像素緩衝（`SwTarget` 指向它），XTFrame 不配置貼圖、不知道貼圖是什麼。
- 宿主擁有時鐘（`update(dt)`）、輸入（`pushInput`）、資產（`AssetResolver`）、字型（`TextShaper`）。XTFrame 不開檔案、不讀系統時間、不碰執行緒。
- 一個 `Context` = 一張貼圖。多個 UI 疊在同一張貼圖是合法的（多個 Document 在同一 Context，`foregroundDocument` 收輸入）；要分開的貼圖就開多個 Context。

## 23.2 髒矩形（`DamageList`）

```cpp
namespace xt {
struct DamageList {
    std::vector<Rect> rects;   // 裝置像素，已裁到視口，互不重疊或少量重疊，最多 kMaxDamageRects（16）
    bool full = true;          // 整張都髒（第一幀、視口變動、dpr 變動、矩形太碎被合併成整張）
    void clear();
    Rect bounds() const;       // 所有矩形的聯集外框；full 時為視口
};
void Context::render(CommandList& out, DamageList* damage = nullptr);   // damage 非 null 時填
}
```

演算法（核心，與後端無關）：
1. `Context` 保留上一幀的 `CommandList`（swap，不複製）。
2. 對兩份命令清單逐條算雜湊（命令種類 + 參數 + 幾何位元組），從頭找第一個不同、從尾找最後一個不同，得到兩段變動區間 `[a, b)`（舊）與 `[a, c)`（新）。
3. 髒區 = 舊區間內每條命令的 `boundsHint` ∪ 新區間內每條命令的 `boundsHint`。圖層/效果/投影命令的 `boundsHint` 已是世界空間 AABB（09 章）；效果要外擴其 `outset`（模糊半徑等，09 §9.x 的 `effectOutset`）。
4. 矩形合併：超過 16 個時兩兩合併面積增量最小者，直到 16 個；若合併後總面積 > 視口 60%，`full = true`。
5. 視口或 dpr 改變、`reloadDocument`、第一幀：`full = true`。

沒有依賴 dirty 旗標：dirty 旗標是「哪些節點要重算」，髒矩形是「畫面哪裡變了」，兩者不同（一個動畫中的元素每幀重算但可能沒移動；一個沒重算的元素可能因祖先移動而變）。命令清單比較兩者都涵蓋。

成本：每幀兩份清單的雜湊，HUD demo 約 40 條命令，可忽略。

**修訂（2026-11-07 定案；T-639 調查 `Docs/reviews/2026-11-07-sw-render-perf.md`）**：

- **步驟 2 改為多區間**（T-644）：對兩份雜湊序列跑序列比對（Myers 或等價的 LCS 差異），得到多個變動區間而非「第一個到最後一個」一段；夾在兩個變動區間之間、雜湊未變的命令不算髒。既有的傳播規則不變（B6 的圖層內變動傳到整個圖層、clip-path、backdrop／mix-blend 讀背景者）。理由：game-ui hud 只有 2.5% 畫面在動，卻因三張靜態卡片夾在中間而髒 25.8%，並更容易越過 60% 門檻變整張。差異成本上限：命令數 n 的 O(nd)，d 為變動數；n > 4096 時退回單區間。
- **§23.3 SW 局部重繪補強**（T-642）：目前「命令觸到髒區就整條全尺寸光柵化，只在寫入時遮罩」。改為：每條命令的掃描與取樣範圍先與髒區（含效果外擴）相交；根層圖層／效果緩衝只配置髒區外擴效果讀取半徑的大小，而非整張；取消每幀整張的 255／0 遮罩與合併裁剪遮罩（改用矩形裁剪，只有旋轉／路徑裁剪才建遮罩）。輸出必須與整張重繪逐位元組相同（既有 `partial_*` 測試為閘門）。`xtframe-view --headless --partial` 讓 headless 也走髒區路徑以便量測。
- **合成核心**（T-643）：圖層 pop 時 opacity < 1 與非 Normal 混合走 SIMD 路徑（與現有純量結果逐位元組相同）；層緩衝只清實際寫入的列；float 層的 memset 只在需要時做。golden 不變。
- 不做／待裁（PENDING）：Layer 模式的靜態區段快取（23 §23.aa 的分帶只在 Direct 模式；fairy 58 → ~32 ms，但要定記憶體上限與快取語意）；定點 AVX2 影像取樣（改變像素，需 golden 裁決）；預縮放影像快取（不改像素，可排）。

**T-642 實作記錄（#608，2026-11-07）**：`sw_layer` 新 `Box`／`RectRegion`（髒區拆成互斥列帶的 x 區間，每幀重建、容量重用）／`WriteRegion`（盒＋可選區域；提供要掃的列與每 span 要寫的片段）；`setRootWriteMask` → `setRootRegion`，`popLayer`／`popEffectLayer`／`compositeSurface`／`popMask` 只寫區域的 run。255／0 髒遮罩與合併裁剪遮罩取消；整像素軸對齊 `PushClipRect` 不建遮罩（只盒、借父遮罩）；`paintContours` 只掃區域列、裁剪 run 逐片畫、漸層仍從 run 起點步進；影像／字形／quad／particle／mesh 全部收 `WriteRegion`；根層圖層／遮罩／逐像素效果（ColorAdjust、GradientMask、Shine、前置 BackdropBlur）緩衝裁到髒區外框。`xtframe-view --headless --partial`。**做不到髒區感知（像素會變）**：鄰域效果（Blur／Glow／DropShadow／Outline）保留整張緩衝、內容整個畫、只限制合成（box-blur 列累加、金字塔格、距離轉換都依緩衝範圍）；含巢狀 BackdropBlur 的根區段不限制；`PushMask` 緩衝仍整視口（配置未裁）。量測（釘單核）：npdo hud 局部 4.62 → 1.10 ms、settings 10.08 → 7.14（剩 ~5 ms 在 opacity < 1 的純量合成與整緩衝清除 → T-643）、game-ui hud 4.03 → 3.56（三張靜態投影卡被夾髒 → T-644）、pause 9.80 → 9.19；整張 3～7% 快。測試 `test_capi_partial.cpp` 三案（局部 vs 整張逐位元組、時間 ≤）、`test_sw_partial.cpp` 一案；core 2012、golden 173、capi 36；GCC sw 單元 73／73。**GCC 發現**：T-641 的 `test_capi_trim.cpp:60` `videoMb()` 非 Windows 未使用 → `-Werror=unused-function` 連結失敗（T-643 順手修）。

**T-644 實作記錄（#609，2026-11-07；一項裁決）**：`matchCommandHashes()`（`damage_internal.h`）先配頭尾、中段 Myers O((n+m)d) 回溯取配對；上限：任一清單 > 4096 條或編輯數 > 256 → 退回單區間。髒命令 = 未配對者，以及配對但外圍群組不同者（被包進／移出／移到別的群組；EndMaskContent 算一層）；B6／clip-path／BackdropBlur／mix-blend 規則改為逐變動區間跑；16 矩形合併與 60% 規則不動。**雜湊改為不含池位移**（PathRef verb／point、stop、dash、glyph、effect、instance、vertex、index 位移；池內容本來就有雜湊）——否則插入一條命令就讓之後全部位移改變、多區間失效；`hashCommandList` 同時是 GPU frame_builder 的效果快取鍵，改為純內容仍正確。量測：game-ui hud 髒區 24.6 → 7.5%（面板外 1.6%）、局部 3.55 → ~2.5 ms；npdo settings 不變（整面板淡入淡出是真變動）。**裁決**：≤ 5% 目標達不到是因為 HP 條每 0.4 s 縮一次、它所在的玩家面板有投影，B6 把整個投影範圍（9.2% 畫面）列髒——維持 B6 不放寬（投影內容 alpha 不變時可縮的想法記 PENDING），測試門檻改為整體 ≤ 8%、面板外 ≤ 2.5%。測試 `test_damage.cpp` 六案（含 200 組隨機 LCS 對照）、`capi partial 4`；core 2018、golden 173、capi 37。

**T-643 實作記錄（#611，2026-11-07）**：`SwSimdOps::spanLayerBlendF`／`spanLayerBlendU8`：`popLayer` 除 Normal＠opacity 1（既有 source-over 核心）外全走 SIMD——Normal＠opacity < 1 每向量一像素；16 種非 Normal 模式（含 Hue／Saturation／Color／Luminosity）四像素轉置成每通道一向量，步驟順序與 `blendPixel` 相同（含首元素 min／max 與 `setSat` 的穩定三元排序）；n%4 尾與無效模式走純量參考；RGBA8 讀寫用精確 /255 與一般 un-premultiply（`loadPixelU8`／`storePixelU8`，`vloadU8` 的 ×1/255 在 126 個位元組值差最後一位）。`xt/simd.h` 加 `cmpLt`／`cmpLe`／`cmpEq`／`maskAnd`／`maskOr`／`maskAndNot`／`sqrt`／`abs`／`transpose4`（SSE2／wasm／純量；wasm 未編譯）。**只清寫入列**：`acquire` 不清，`Target::rowDefined` 逐列旗標，`rowF`／`set` 首次觸列才清、`get` 未定義列讀透明；`popLayer`／`popMask` 跳過未定義列（唯一會改位元組的情況＝全不透明 Normal 到非預乘 RGBA8 根：alpha 0 像素色彩歸零，與零 source-over 相同，256×256 全組合驗證）；`topLayerSurface()` 先清未定義列。**效果 pass 零配置**：`fx::MathScratch`（max-filter 列／欄、距離轉換格、遮罩、欄位、降採樣模糊層）放 `EffectScratch`，`releaseScratch` 釋放；`BackdropPyramid` 保留緩衝；`EffectFrame` 依深度重用。Direct 模式 `clusters` 仍每幀重建（未動）。量測（釘單核）：npdo 進場 36.9 → 22.4 ms、閒置 21.4 → 18.8、settings 局部 7.3 → 3.7、hud 局部 1.1 → 0.88；pause（glow 為主）不變。測試 `test_sw_layer_simd.cpp` 四案（每模式 SIMD vs 純量逐位元組，opacity 五值、邊緣 alpha）、`test_zero_alloc.cpp` 效果 fixture 300 幀 0 配置；core 2016、golden 173、capi 36、perf 9；GCC sw 99／99。**GCC 發現**：`capi trim 2`（T-641）在非 Windows `privateMb()` 回 0 使比例恆為 1 而失敗——非 Windows 跳過或改用 heap 統計（T-648 順手或下個 capi 單）。

## 23.3 SW 局部重繪

```cpp
void SwRenderer::render(const CommandList& list, TextShaper*, AssetResolver*, const IrView* = nullptr,
                        PerfTracker* = nullptr, const DamageList* damage = nullptr);
```

- `damage == nullptr` 或 `damage->full`：行為不變（整張）。
- 否則：對每個矩形設 scissor，只清除與光柵化該矩形；圖層/效果緩衝仍以完整 `boundsHint` 配置（效果需要鄰域），但只合成回矩形內。命令清單仍完整走一遍（跳過與所有矩形不相交的命令：`boundsHint` 測試）。
- 正確性測試：局部重繪 N 幀後的緩衝與每幀整張重繪的緩衝位元組相同（黃金比對）。
- 宿主上傳：`for (Rect r : damage.rects) uploadRegion(r)`；`full` 時整張。`engine_integration` 印每幀上傳像素數與整張的比例。

GL 後端不做局部重繪（stencil-then-cover 整張畫本來就便宜）；`damage` 參數接受但忽略。

## 23.4 字型與文字品質

- **系統字型後備**（T-97）：`FtTextShaper::addSystemFonts()` 掃 Windows `%WINDIR%\Fonts`、Linux `/usr/share/fonts`、macOS `/System/Library/Fonts`（只載入 `.ttf/.otf`；T-97 先做成即時開檔，T-104 改惰性：掃描時只讀 `name` 表登記路徑、family、subfamily、PostScript 名，`match()` 命中或字形後備走訪到該字型時才 `FT_New_Face`，開過的快取）。`xtframe-view`、`xtframe-pack` 縮圖、`engine_integration`、編輯器預覽都呼叫；宿主可不呼叫而自己 `addFontMemory`（引擎通常自帶字型資產）。
- **小字點陣路徑**（T-101）：目前 SW 一律用外框光柵（`FT_LOAD_NO_HINTING` 外框 → 掃描線），小字偏糊、筆畫粗細不均——這就是使用者在 RmlUi 看到的觀感問題的同類。改為：字級 < 24 px（裝置像素）時用 FreeType 點陣（`FT_LOAD_TARGET_LIGHT`，只有垂直 hinting）、水平 1/4 px 子像素位置（4 個變體）、快取在 `GlyphAtlas`（7.4 已有，GL 在用），SW 以覆蓋率貼圖混合；≥ 24 px 維持外框。GL 已走 atlas，改成同一套載入旗標即可一致。
- **覆蓋率 gamma**：文字混合時對覆蓋率套 `pow(c, 1/1.2)`（深底淺字）/`pow(c, 1.2)`（淺底深字）——判斷依文字色與目前像素的亮度；可由 `text-rendering: geometricPrecision` 關閉（不做 gamma、不用點陣）。黃金圖會變，逐張重產並在回報列出。
- 不做：LCD 子像素彩色抗鋸齒（貼圖會縮放、旋轉）、stem darkening。

## 23.5 SW 效能（T-102）

目標：HUD demo 1280×720 Release `render` ≤ 5 ms（T-82 後 13.5 ms）。手段（依量測）：
- 內部像素改 **premultiplied float**：source-over 變成 `dst = src + dst * (1 - srcA)`，少一次除法與分支；輸出時才 unpremultiply 成 RGBA8（宿主要 premultiplied 時提供 `SwTarget.premultiplied = true` 直接輸出）。
- **SIMD**：SSE2 為基線（MSVC x64 預設可用），AVX2 以執行期偵測派發（`__cpuid`）；三個熱點：span 混合、高斯模糊的水平/垂直 pass、float→RGBA8 轉換。跨平台以 `xt/simd.h` 薄封裝（16 個函式內），無 SIMD 時退回純量。**三張表（純量／SSE2／AVX2）的輸出位元組必須相同**（2026-10-29 裁決，T-529）：AVX2 單元不得以 FMA 建（GCC／Clang 不給 `-mfma`；MSVC `/arch:AVX2` 下保持 `/fp:precise`，不開 contraction），因為融合乘加少一次捨入，在效果重的場景（challenge-menu、fairy-forest）會讓同一份黃金在有 AVX2 的機器上變成另一組位元組，而 AVX2 相對 SSE2 只快約 2%。`Tests/unit/test_simd.cpp` 以位元組相等斷言守住；wasm 的 SIMD128 後端（見 `claude/web-wasm` 的 52 §52.2）同受此規則約束。同一條規則推到全樹：頂層 CMake `-ffp-contract=off`（14 §14.10），否則 AArch64 的純量程式碼本身就會被融合。
- 局部重繪（23.3）在互動時額外省的部分不算在 5 ms 目標內（目標以整張重繪量）。

## 23.6 穩定介面清單（T-100，併回 13 章）

`<xt/xt.h>` 匯總以下標頭；0.3 起這些是**宿主契約**，major 版本內只加不改：

| 標頭 | 內容 |
|---|---|
| `xt/context.h` | `Context`、`ContextConfig`、`DocumentHandle`、`DamageList` |
| `xt/document.h`、`xt/element_ref.h` | `Document` 公開方法（13.2）、`ElementRef`（13.3）、事件監聽 |
| `xt/input.h` | `InputEvent`、`KeyCode`、`PointerButton`、`Cursor` |
| `xt/asset_resolver.h` | `AssetResolver`、`FileAssetResolver`、`ImageData` |
| `xt/text_shaper.h` | `TextShaper` 抽象（宿主自供字型時實作 `addFontMemory` 即可） |
| `xt/command_list.h` | `CommandList`（自製後端用） |
| `xt/perf.h` | `PerfTracker`、`FrameStats` |
| `xt/sw/sw_renderer.h` | `SwRenderer`、`SwTarget` |
| `xt/bundle/bundle.h` | `Bundle`、`loadUi`、`reloadUi` |
| `xt/flow/flow_runtime.h` | `FlowRuntime`（宿主想跑流圖時） |
| `xt/capi/xtframe.h` | C ABI（50 章；純 C99，`xt_bundle_*`／`xt_surface_*`；FFI 宿主如 Unity 的唯一接點；ABI 版本獨立於 major） |

規則：以上標頭不得包含 `*_internal.h`、不得暴露第三方型別；新增成員只能加在結尾且有預設值；刪除或改語意 → major +1。`Examples/engine_integration` 只允許包含這份清單的標頭（CI 用 grep 檢查）。

## 23.7 假宿主 `engine_integration`（T-99、T-100）

改成一個「像引擎」的完整範例：宿主擁有 RGBA 緩衝與一個假的 `Texture` 物件（記錄 `uploadRegion` 呼叫）、自己的事件結構轉成 `InputEvent`、自己的時鐘、bundle 載入與熱重載、每 120 幀印 `PerfSummary` 與上傳面積比例。README 依 23.6 的順序寫整合步驟，宿主插件照著抄。

## 23.8 圖片資產格式（宿主常問）

- 輸出畫面是每幀更新的動態 RGBA8 貼圖，與引擎的離線貼圖管線（tif→dds/BC）無關，也不能壓縮。
- UI 引用的圖片由 `AssetResolver` 載入、stb 解碼成 RGBA8、在軟體端取樣，從不成為 GPU 貼圖；因此 bundle 內維持 PNG/JPG，引擎插件的 resolver 直接讀 bundle 原始位元組，不經引擎的 dds 管線。載入時解碼一次並快取，每幀成本與檔案格式無關。
- 候選優化（未排程）：`xtframe-pack --raw` 把資產預解成 RGBA8 存放，載入零解碼。DDS/BC 只在未來的 GPU 直繪後端（10.3 路徑）才有意義。

> T-199（#197）後：`xtframe-pack --compress bc1|bc3|bc7` 直接輸出 DDS（BC1/BC3 FourCC 或 BC7 DX10，straight alpha，可選 mip 鏈），核心 GL 後端以 `glCompressedTexImage2D` 上傳、SW 解回 RGBA；宿主（CE）可直接拿 DDS 走自己的貼圖管線。

## 23.8b 引擎貼圖進 UI（T-150）

宿主把一張引擎貼圖交給 UI 取樣：文件寫 `<img src="engine://scene">`，宿主在該文件第一次 `update` 前呼叫 `bundle.assets().setExternalTexture("scene", handle, w, h, straightAlpha)`（handle 為 `xt_gpu` 裝置能取樣的貼圖 id；SW 路徑畫透明）。`xtframe-view --bind-texture scene=<png>` 可模擬。引擎插件以自己的貼圖包裝 API 取得 handle。

## 23.8c 第一幀成本（T-160 量測）

首次 `update` 約 6 ms（HarfBuzz 首次 shaping ~3.5 ms、延遲字型 `FT_New_Face` ~1 ms、佈局）；首次 `render` 含圖片解碼（bench 看板三張 PNG 約 28 ms）、貼圖上傳、字形 atlas。建議宿主在載入畫面期間先跑一次 `update(0)` + `render` 預熱（例如開啟後立即 tick 一次不顯示）。

## 23.9 取捨紀錄

- 髒矩形用命令清單比較而不用節點 dirty 旗標：理由見 23.2；代價是每幀保留一份舊清單（HUD 約 10 KB）。
- 小字用點陣、大字用外框：兩套路徑的接縫（23.9 px ↔ 24 px 動畫縮放時會跳一下）接受，動畫中的文字通常不在這個字級邊界。
- premultiplied 改動會碰到 T-82 的黃金圖比對：容差內視為相同，超出的逐張說明，不默默重產。

## 23.10 宿主整合新增（2026-09-27 定案；依引擎 UI 系統對照與 27 章）

1. **註冊即生成節點**：宿主載入 bundle 時讀 `manifest.ports`，為每個 UI 生成宿主腳本系統的節點或元件（每個 port 一個，含 instanceId 埠，埠下拉列出 bundle 內 UI 與 port）；`xtframe-pack --gen-ports` 另產強型別標頭。
2. **多實例**：同一 UI 多個 surface（名牌類），事件帶 instanceId；Plate 沿用。
3. **清單、讀回、呼叫**：13.8.1。
4. **生命週期握手**：`requestHide` → outro → `OnHidden`；`OnShown`；系統事件（載入、暫停、斷線、切語言、平台）以 port 形式進全域流圖與 UI。
5. **世界座標**：宿主換算後 `setWorldAnchor`；提供「世界座標轉 UI 座標」宿主節點。
6. **輸入**：`setInputDevice`、手把與方向鍵焦點導覽（08.9）、多玩家、觸控；surface 疊層阻擋（`input: block|pass|watch`）。
7. **外部內容**：影片／序列由宿主播放器解碼並 `bindTexture`（引擎自己的影片播放器或作業系統的媒體框架）；場景視圖與引擎貼圖沿用 23.8b。
8. **在地化**：`ILocalizationProvider` 接引擎的在地化系統；切語言自動 `setLanguage`。
9. **音效與震動**：`sound(cue)` → 引擎音訊系統同名觸發執行；`haptic(cue)` → 手把震動。
10. **flow 在宿主執行**：宿主插件內建 `FlowRuntime`，載入 `bundle.xtflow` 與各 UI flow；宿主設定可關。
11. **熱載入**：22.10 契約；宿主編輯器窗格一鍵。
12. **自適應**：`setDevice` 由插件從引擎輸出解析度、平台 title-safe、輸入裝置取得；uiscale 規則（高度為準 1080 = 1.0，可改）。
13. **letterbox 與固定像素矩形**：以 `@media` 與安全區單位表達，不另做節點。

### 23.10.3 安全區與 uiscale 單一來源（2026-09-27 定案，對應 17.4.1 第 4～5 條）

宿主的 `setDevice` 安全區預設取 manifest `spec.safeArea`（來自 UI Project `spec.safeArea`，依平台），宿主的顯式設定（例如 cvar）才覆寫；主機平台由引擎 title-safe 取代。`uiscale` 規則兩端一致：邏輯高 1080 = 1.0（`setUiScale(outputHeight / 1080)`）。編輯器預覽用同一組數（17.4.1）。宿主以 `Bundle::spec()`（T-207）取值。

### 23.10.6 三層檔案規則的宿主端（2026-09-27 定案，對應 22 §22.11）

宿主執行時只讀 Bundle；宿主的編輯器窗格若存在，對使用者的動詞是「Open UI Project」與「Publish」，錯誤訊息指向「先發佈 UI Project」。宿主自己的容器格式（若有）只在宿主倉，本倉工具不讀不寫；宿主必須以本倉獨立編輯器發佈的目錄 bundle 做端到端驗證（開啟、port 節點、熱載入），目錄 bundle 是宿主的第一公民。

## 23.11 行動裝置宿主：Android 測試 app（2026-10-01 定案；使用者：「乾脆來弄手機支援，派代理創造一些手機用的 preset，隨便做個手機 app 傳過來給我測試」）

### 23.11.1 形態

- `Code/Runtime/platforms/android/`：SDL2 官方 Android 專案形態（`SDLActivity` Java 殼 + Gradle + NDK CMake），應用 id `org.xtframe.view`，名稱 **XTFrame View**。NDK 以本倉 `CMakeLists.txt` 建 `xt_core xt_compile xt_sw xt_bundle xt_flow xt_text xt_shell`（Android 上 `XT_SHELL_GL=OFF`：`xt_shell` 不連 `xt_gl`，`App` 走 **SW 呈現路徑**——`xt_sw` 算到 RGBA 緩衝，`SDL_Renderer` streaming texture 以 damage 矩形 `SDL_UpdateTexture` 後整張呈現；T-239 的 damage 邏輯可直接重用）。GLES 3.2 版的 `xt_gl` 為第二階段（T-242），到時 Android 切回 GL 路徑。
- 工具鏈：`C:\Android\Sdk`（NDK 27.3、platforms 34–36、build-tools 36、JDK 21）；Gradle wrapper 自動下載；輸出 debug 簽章 APK（`app-debug.apk`），由主模型以 SendUserFile 送給使用者。
- **內容**：APK `assets/` 內放 bundle 目錄（22 章目錄 bundle）；`AssetResolver` 以 `SDL_RWops`（`SDL_RWFromFile` 走 AAssetManager）讀取；`XT_BUNDLE` 環境／intent extra 可指定外部路徑供熱替換（第二階段：Wi‑Fi 從編輯器 Service 拉 bundle）。
- **輸入**：既有 `App` 的 SDL finger 路徑（27.7 手勢辨識：tap／hold／swipe／pinch）；多指；返回鍵 → `postSystemEvent(Back)`；文字輸入走 SDL 文字事件 + 系統鍵盤（`SDL_StartTextInput`）。
- **裝置**：視口 = 螢幕邏輯尺寸（`uiscale` = 螢幕高 ÷ `spec.logicalHeight`，直屏時以**寬**為準——UI Project `spec.uiscaleRule: height|width|shortEdge`，手機預設 `shortEdge`）；安全區 = Android `WindowInsets` 的 display cutout 與系統列（JNI 一次讀取，變動時重讀），對應 `setDevice` 的 safe-area；直橫屏切換 = 視口變更事件（既有 `@media` 比例規則接管）。
- **其他宿主能力**：haptic → `SDL_HapticRumble`／Java `Vibrator`（第二階段）；sound cue → 無（宿主回呼留空、log）；`engine://` 貼圖無；影片無（第二階段用 `MediaCodec`？先略）。
- **效能目標**：1080×2400 直屏 HUD 在中階手機 SW 路徑 ≤ 16 ms（靜態時 0），記錄實測；GLES 之後 ≤ 4 ms。
- **分層**：`Code/Runtime/platforms/android/` 屬 R（執行時展示 app，將開源，等同 `xtframe-view` 的手機版）。
- **實作記錄 T-240（#248，2026-10-01）**：`XT_SHELL_GL` 選項（預設 ON、Android 強制 OFF；桌面 `-DXT_SHELL_GL=OFF` 也可建 `xtframe-view`，另加 `--hidden`／`--stats`）；SW 呈現照 T-239：damage 矩形 `clearRects` + render 到 premul 緩衝，只 `SDL_UpdateTexture` damage 矩形，無 damage 的幀不算不傳。APK 含 arm64-v8a 與 x86_64，SDL2 2.30.9／FreeType／HarfBuzz 由 NDK 27.3 交叉編譯靜態連結；`assets/bundle/` 為 `mobile-hud`，由 Gradle 任務呼叫 host `xtframe-pack`（偏離單上的 CMake 自訂目標：資產合併須排在打包後）。安全區：JNI `XTFrameActivity.safeInsets()`（API 28+ cutout 與 systemWindowInsets 取大；cutout shortEdges 模式；尺寸變更後連讀 30 幀，平時每 30 幀讀一次）。uiscale 走 T-241 的 `uiscaleRule`／`UiScaleEdge`；橫向設計的 UI 自動轉橫屏。系統事件：返回鍵 → **`system.back`**（新隱含 port，flow System 節點加 `back` 輸出）、背景／前景 → `paused`／`resumed`；裝置 Handheld + Touch；sound／haptic／spawn／emit 只 log。換 bundle：`adb push` 到 `/sdcard/Android/data/org.xtframe.view/files/…` 後 `am start -S … --es XT_BUNDLE <路徑> --es XT_UI <名>`；`XT_STATS=0` 關效能 log。**為 NDK 所做的 R 層修改**：`xt::fromChars`（NDK libc++ 無 float `from_chars`）、AVX2／cpuid 只在 x86 編譯、`xt_shell` 改無 RTTI、xt_text 在 Android 掃 `/system/fonts`（Roboto 優先）、`xt_bundle::setBundleFileReader` hook、SDL Android 保留 joystick／HIDAPI／haptic。測試：`SdlRwAssetResolver`（`SDL_RWFromConstMem`）、SW 視窗幀對 SW headless 逐 byte 相同（GL 對 SW 只有 AA 邊緣與字形差異 < 1% 像素，無法達「容差 2」，改為手動比對）；`local_ci.sh` 加 `XT_CI_ANDROID=1` 選用階段。首次建置下載 SDK CMake 3.31.5、Gradle 8.10.2、AGP 8.7.3，冷建置約 33 分鐘。**手機實測數字未取得**（無 adb 裝置）；桌面 SW 代用：game-ui hud 1080p 動畫約 5 ms/幀（damage 約 10%），mobile-hud 1080×2400 靜態首幀後不再算圖。限制：一次只顯示一個 UI 與其 flow，無全域 flow、無 `ui#` 切換。 **T-240b（#249）**：使用者實機回報 `cannot load UI 'hud': kIoError`——`loadUi` 走 `Context::loadDocumentFile`（核心 fopen）而非 `setBundleFileReader` hook，APK 內的 `.xtir` 讀不到；改經 `detail::readFile` + `loadDocument`，與 `reloadUi` 同形，加虛擬 bundle 回歸測試。另：host `xtframe-pack` 的 `<exe>/symbols` 複本由建置時拷貝，T-241 之後要重建 pack 工具才有 `mobile/`。 **T-241b（#250）**：使用者實機回報搖桿頭與底盤、冷卻環與按鈕不同心——搖桿頭 `left/top` 未扣底盤 2 px 邊框（絕對定位原點是 padding box）偏 2 px，`.ring` 在 112 px 格子裡定在 0 偏 4 px；皆為製作座標錯誤，非 SW 算圖問題（桌面無頭算圖量測 37/37、38/38）。注意 `xtframe-pack` 只在 `shared/symbols/<Name>.xt` 缺檔時才複製標準符號，改庫後要刪專案裡的舊複本。

### 23.11.2 手機 preset（30 §30.4 擴充）

標準符號庫新增 `mobile/` 子集（T-241）：`VirtualJoystick`（拖曳 → `axis:vec2 out`，回彈、死區、可固定或浮動）、`TouchButton`（大觸控面、按住連發 `repeat:event out`、冷卻環）、`DPad`、`SwipeCard`（左右滑 → `swipe:event out`，回彈動畫）、`BottomSheet`（上滑展開、拖曳關閉，狀態 Collapsed／Expanded）、`TabBar`（底部分頁，安全區內）、`TopBar`（狀態列避讓）、`PullIndicator`（下拉刷新）、`Toggle`／`Slider` 的觸控尺寸變體（≥ 44 px）、`Toast`（既有，底部安全區上方）、`PinchZoomView`（雙指縮放的容器，`scale:number in/out`）。全部依 30.4 契約規範（狀態、插槽、port、時間軸），`index.json` 以 `gen_symbols_index.py` 重生。裝置預設加 **Phone 20:9**（1080×2400，安全區上 4%／下 3%）與 **Tablet 16:10**；UI Project `spec.platforms` 範例 `mobile`。
- 範例專案 `Examples/demos/mobile-hud/`（UI Project）：直屏遊戲 HUD（搖桿、兩顆動作鈕、血條、彈藥、小地圖佔位、暫停鈕）+ 設定頁（BottomSheet、Toggle、Slider、TabBar）+ 一段 intro 時間軸；用的是 `mobile/` 符號與標準 token；打包成目錄 bundle 進 APK。
- **實作記錄 T-241（#247，2026-10-01）**：`Assets/Symbols/mobile/` 11 個符號（VirtualJoystick、TouchButton、DPad、SwipeCard、BottomSheet、TabBar、TopBar、PullIndicator、ToggleTouch、SliderTouch、PinchZoomView），token `--touch-min: 44px`／`--touch-lg: 88px`，`index.json` 分類 `mobile`；`xtframe-pack`、`xt-standard:` 與編輯器標準清單在根目錄找不到時再找 `mobile/`。裝置預設 Phone 20:9、Phone 19.5:9、Tablet 16:10；`spec.uiscaleRule` 讀寫（`.xtui`／manifest）、`bundle::uiScaleFor()` 供畫布、`xtframe-view` 共用，UI Project 面板下拉；`shell::App` 加 `UiScaleEdge`。範例 `mobile-hud` 嚴格打包零診斷、`xtframe-view` 1080×2400 可開。**受語言功能限制的近似**（待 C 之後補）：`draggable` 只有 `axis-x` → 搖桿只有水平軸、BottomSheet 以 swipe(down) 關閉；out port 不能驅動畫面綁定 → 搖桿頭用 `knob:vec2 in`、`active` 為 in port；觸發器拿不到指標座標 → Floating 模式只原地淡入；pinch 倍率不是事件值 → `scale` 為 in port；無預設值／自訂屬性 → 死區由宿主設、`repeat-ms` 固定 0.25 s；無 `extends` → ToggleTouch／SliderTouch 為複製；冷卻環改直向遮罩；畫布安全區疊圖與 lint 仍用統一比例。

## 23.x 宿主插件的文件生命週期守則（2026-10-02；T-260a #273、T-260b #277）

依 13 §13.9／§13.10 的語意，宿主遵守：

1. 在 `onHidden`／host event 內卸載一個面板並載入替代品是允許的：替代品拿到新 handle，從下一幀開始 update。
2. 使用 `BundleContext` 時，卸載前後呼叫 `removeUi`；全域 flow 的重載放在 callback 之外。
3. `reloadDocument`／`hotReload` 之後的 `loadGlobalFlow`／`loadUiFlow` 放在 callback 之外。
4. 自己持有、綁在某個面板上的 `FlowRuntime`：先載入新文件、對它載入 flow、再卸載舊文件。
5. 不要在同一個 callback 內「重載再卸載同一個 handle」並期待新文件出現。
6. 每幀由 handle 重新取 `Document*`，不跨幀保存指標。

## 23.y 審查修正鏈補記：damage、描邊、宿主升級注意事項（2026-10-07 審查修正鏈合併）

**damage（§23.2）**：`StrokePath` 的外擴 = `strokeOutset(width, cap, join, miterLimit) × 最大縮放`（0.5 × width × max(Miter 的 miterLimit, Square cap 的 √2, 1)；Painter 的節點包圍盒同一函式，round／bevel 接合比以前緊）；變更發生在仍開著的 `PushEffect` 內時，該效果的 `boundsHint` 也計入；`Context::render(out, &damage)` 只對自己上次產生、未被動過的清單比對（`frameSerial`、`structureChecked`），否則回 `full`；就地簡化的清單不是基準，要局部重繪用不改原清單的簡化（09 §9.8）；`commandBounds` 只接受已驗證的清單。

**宿主升級注意事項**（彙整自各 PR；每個宿主同步本倉版本時逐條檢查）：
1. 預建庫原始碼清單加 `Code/Runtime/bundle/src/file_status.cpp`；`ParticlesCmd`／`ImageQuadCmd`／`ImageMeshCmd` 位置式初始化與 `ShaderTable::copyTo(asset, source, out)` 的呼叫端要改。
2. 插件後端若渲染合併清單，須經 `sourceIr` 讀 `command.source`（貼圖鍵含來源的 `loadSerial`）；每板一份文件的模式不變。宿主的 `GpuDevice` 可覆寫 `maxTextureSize()`。
3. `lastFrameReusable` 每幀至多一次；靜態畫面上答 false 可能是重試到期。`invalidateAsset(id, const IrView*)`。
4. 每幀交回同一份未動的清單；要簡化用不改原清單的 overload。
5. `releaseSource`：插件自己開的 `IrView` 在釋放 blob 前先呼叫；要立刻丟貼圖就呼叫 `FrameBuilder::invalidateAllAssets()`；自建的 resolver 要 `setFileStatusFn`，否則 `revalidate()` 回 0；熱重載迴圈下常駐解碼 = 存活文件 + 閒置預算（預設 32 MiB，可設 0）；`resetAssetIds()` 或熱重載前取得的 `imageView()` 指標之後不可用（每幀重取）。
6. 跨多個 Context 共用一個 resolver 時，銷毀 Context 前先卸載其文件。
7. 文字視圖、組字字串、`<repeat>` 實例 StringId 的生命週期（07 §7.9、13 §13.11）；回收的實例取回模板文字。
8. 路徑一律 UTF-8，非合法 UTF-8 視為不存在；`ElementRef` 對回收中節點的行為。
9. `FlowRuntime`／`BundleContext` 的 load／unload 契約與序號存取器（13 §13.11）；文件生命週期守則（§23.x）。
10. 宿主部署的 `xtframe-pack` 不連結編輯器核心；CLI 烘焙改呼叫 `xtframe-editor --bake-vectors`。

## 23.z M31／M32 宿主升級注意事項（2026-10-15；基準：上次宿主同步 `0c6c6eae`（2026-10-07）→ main `0f9f4863`）

每個宿主同步本倉版本時逐條檢查（CE 對應單 CE-42）：

1. **IR 2.5 → 2.6**（T-335）：STYL 尾端新增 `easingPoints[]` 池、`LengthUnit::Rem = 14`、`EasingKind::LinearPoints = 4`。宿主必須重建；舊宿主讀含 `rem` 的檔案會 `kInvalidIr`，含 `linear()` 的檔案當一般 linear 播放。
2. **可選 IR 區塊** `CLIP`（T-332）與 `GRID`（T-338）：舊讀取器略過；新宿主不需處理，由核心消化。
3. **列舉與表格追加**（只追加，不改既有值）：StyleProp 201～207 與第二段 256～272；PropId 至 `kElementPropCount` = 144；`Overflow::Auto` = 3；`FlexWrap::WrapReverse` = 2；`Justify::Stretch` = 6；`Display::Grid` = 3；`MediaFeature` 9～12；`CalcOpKind` 7～8。宿主若有自己的名稱表（dump、devtools）要補。
4. **元素層級 `mix-blend-mode`**（T-326）：Element 現在也走 `PushLayer(opacity, blend)`；宿主 `GpuDevice` 的 Layer 合成須支援全部 16 種 + Add（場景節點本已使用；`FrameBuilder::compositeOnto` 以 Composite shader `mode` 參數處理，宿主只要支援既有 Composite shader）。
5. **`DeviceInfo.reducedMotion`**（T-330）：新欄位（結構變更，重建即可）；宿主應把 OS／引擎的「減少動態效果」設定以 `postSystemEvent("reducedMotion", "reduce"|"no-preference")` 餵入，並可讀 `system.reducedMotion` port。
6. **TextShaper 介面**（T-324／T-331）：`ShapeParams.features`（bit0 = tnum）與 `FontMetrics.underlineOffset/underlineThickness`；宿主自實作的 shaper 應填度量（不填則核心用 0.1 em 退回）並可忽略 features。
7. **命令清單**：結構不變。新用法：元素 `clip-path` 走既有 `PushClipPath`（stencil 深度 15 退包圍盒）；背景平鋪以多個 `ImageCmd`（上限 4096／元素）；outline／text-decoration 為 FillPath／FillRect；`image-rendering: pixelated` → `ImageCmd.smooth = 0`。宿主渲染器不需改。
8. **編輯器窗格（Sandbox 類宿主）**：`EditorSession::Mode` 已移除 → `EditorSession::Space { Canvas, Flow, Project, Demo }` + `animate()`（T-308、T-318a）；`Space::Design` 更名 `Space::Project`；`dock.quicklook` → `dock.project`（舊名別名一版）；命令 `space.*`；`EditorWorkspace` 公開 API 擴充（見 `editor_workspace.h` diff）；GL 預覽改離屏渲染 + 讀回（T-303，主視窗不再含 `QOpenGLWidget`；`--software-preview` 旗標）。窗格程式碼要跟著改名。
9. **預建庫**：新原始碼自動納入（`Code/Runtime/core/src/{clip_shape,layout_grid,text_case,css_color}.cpp`、`Code/Runtime/compile/src/calc_emit.cpp`、`Code/Editor/Core/src/grid_edit.cpp` 等）；`xtframe-pack` 介面不變，manifest 仍只記 irMajor。
10. **驗證**：宿主 selftest 應加：開一份含 `rem`、`linear()`、`clip-path`、`display: grid`、`mix-blend-mode: multiply` 的 UI 能載入並繪製；`postSystemEvent("reducedMotion","reduce")` 後 `@media (prefers-reduced-motion: reduce)` 規則生效。

## 23.aa 宿主直接繪製與場景 backdrop（2026-10-15 定案；使用者：「做，要做到比別人好，而且更高效」；T-343／T-344／CE-43）

**問題**：宿主若把 UI 先畫進獨立透明圖層再 alpha 合成（CE 現況），`mix-blend-mode`、`backdrop-blur` 看不到場景。UE（Slate 直接畫進 back buffer、`Background Blur` 抓場景色）、Unity（Overlay canvas 直接畫、`_CameraOpaqueTexture`）、Gameface（`backdrop-filter`）、Scaleform 都是「UI 直接畫在場景上」。本節定我們的做法，目標是**效果等同、成本更低**。

### 23.aa.1 兩種宿主模式

`Context`／`FrameBuilder` 新增 `HostTargetMode`：
- `Layer`（現況，預設）：宿主綁一張透明 RT，UI 只重畫 damage，宿主每幀合成整層。混合與 backdrop 只看得到 UI 自己。
- `Direct`：宿主把**場景目標**（後製之後的 LDR 目標）綁為 `GpuFrame` 的目標 0 並呼叫 `submit`；UI 直接畫在場景上。此模式下 builder 不清除目標 0，`mix-blend-mode` 的 backdrop 副本與 `backdrop-blur` 的來源自然是場景。宿主在 23.aa.4 的時點每幀呼叫。

### 23.aa.2 分帶快取（beat UE：靜態 UI 不每幀重畫）

Direct 模式下場景每幀都變，UI 本該整個重畫。改為把繪製序切成**帶（band）**：元素依繪製序分成「需要場景的元素」S（`mix-blend-mode != normal`、含 `backdrop-blur` 的效果鏈、以及其子樹）與其餘；相鄰的「其餘」連成一帶。每帶畫進一張**帶圖層**（持久 RT，尺寸 = 該帶 bbox，premultiplied），只在該帶的 damage 非空時重畫；每幀的工作是依序：帶 0 的圖層以 SrcOver 貼到場景 → S 元素直接畫（含它的 backdrop 拷貝）→ 帶 1 貼上 → …。帶數上限 4（典型 HUD：玻璃面板 S 在最底、內容一帶、暈影 S 在最頂 = 2 帶）；超過上限或帶 bbox 總面積 > 2× 表面時退回「整個直接畫」（每幀全畫，仍正確）。`lastFrameReusable()` 在 Direct 模式的語意改為「所有帶圖層可重用」，宿主仍須每幀 `submit`（場景在變），但 submit 內容只有幾張 quad 與 S 元素。

### 23.aa.3 場景 backdrop 的取得（beat Gameface／UE：只拷需要的區域、一條共用模糊鏈）

- **區域拷貝**：S 元素需要的場景區域 = 其 bbox 外擴 `effectReadExpand`（3σ）；以既有 `Pass::copy` 從目標 0 拷貝到 transient，**不拷全螢幕**。多個 S 元素的區域若相交則合併成一張。
- **共用模糊金字塔**：一幀內所有 `backdrop-blur` 共用一條降解析度鏈：把合併區域先降到 1/2，再視最大 σ 降到 1/4、1/8（σ 在該層 ≤ 2 px 為止），每層一次 dual-Kawase；每個面板依自己的 σ 取最接近的層（線性混合相鄰兩層）再做自己的飽和度／亮度／色調。成本與面板數無關，只與區域面積有關；這就是 iOS 毛玻璃的做法，UE 的 `Background Blur` 是每個 widget 各抓各模糊。
- **降解析度模糊**也用於一般 `blur` 效果（18 §18.15；把 16 §16.3 的 T-151 待辦一併做）。
- **stencil**：直接畫在場景目標時目標可能沒有 stencil（CE 的 LDR 目標）。規則：S 元素與帶圖層都在自己的 transient／帶圖層內完成裁切（那裡有 stencil），最後以 Composite 貼到場景；場景目標本身只做 SrcOver 貼圖與 scissor，不需要 stencil。

### 23.aa.4 宿主介面（`GpuDevice` 追加，皆有預設實作）

- `virtual bool supportsDirectTarget() const { return false; }`：宿主宣告目標 0 可讀（可被 `Pass::copy` 當來源）。Direct 模式需要 true，否則 builder 退回 Layer 行為並 `lastError`。
- `virtual TextureHandle acquireBandTarget(uint16_t w, uint16_t h)`／`releaseBandTarget`：帶圖層；預設走 `acquireCachedTarget`（T-162 的持久目標），裝置不支援快取時退回整個直接畫。
- 宿主呼叫時點：場景後製完成、HDR → LDR 之後、宿主自己的其他 overlay（Scaleform／除錯文字）之前；多個 surface 依 z 順序各 `submit` 一次。
- 宿主若能提供「場景是否變動」（例如暫停選單下畫面靜止）可呼叫 `Context::setSceneStatic(bool)`：為 true 時 S 元素的 backdrop 拷貝與模糊結果也快取，整幀變成純 quad。

### 23.aa.5 編輯器與工具

- 展示空間（17 §17.31）：以 `editor-only` 子樹（03 §3.13）預先渲染成「場景」，再用 Direct 模式畫 UI——美術在編輯器裡看到的就是遊戲裡的混合與毛玻璃。畫布空間維持 Layer 模式（編輯時不需要）。
- `xtframe-view --scene <png>`：以一張圖當場景跑 Direct 模式，截圖用；`--scene-animate` 讓場景緩慢平移以驗證每幀 backdrop 更新。
- SW 後端（`xt_sw`）同樣實作 Direct（寫入宿主給的畫布）與分帶，供測試與 CPU 宿主。

### 23.aa.6 效能目標與驗收

1080p、三個玻璃面板（σ 8～12）+ 一個 `multiply` 暈影 + 靜態 HUD 內容，GL 後端（T-159 的 bench 機器）：靜態幀 GPU ≤ 0.25 ms、CPU build ≤ 0.1 ms；面板數 3 → 8 時 GPU 增量 ≤ 0.05 ms（共用金字塔）；與 Layer 模式相比靜態幀成本差 ≤ 30%。21 章 perf 計數新增 `bands`、`bandRedraws`、`backdropCopyPx`、`pyramidLevels`。golden：`gl`／`sw` 各一張 Direct 模式（場景圖 + 玻璃 + multiply + difference），SW／GL 對照。

### 23.aa.7 實作記錄（T-343，PR #376，2026-10-15，Fable）

帶標記 = `Command::flags[0] & kCommandSceneRead`（原 `_pad[3]` 更名，Painter 標在 blend ≠ normal 的 PushLayer 與首效果為 BackdropBlur 的 PushEffect；`.xtcl` 佈局不變）；`scanSceneRuns` 只在深度 0 切帶；`kMaxBands = 4`、帶面積 > 2× 表面退回全直接畫（`directFallback`）。`GpuDevice::supportsDirectTarget/acquireBandTarget/releaseBandTarget`（預設實作）；`FrameBuilder::build(..., damage)`；帶重用 = damage∩帶視窗為空 ∧ 命令數同 ∧ run 結構同 ∧ 視窗同。金字塔：level0 區域拷貝，每層 2×2 Downsample + 一次 Kawase（σ ≈ 2.3／5.1／10.5／21／42／85 px），面板以 `LevelMix` 取相鄰兩層；`EffectProgram` Downsample=14／Kawase=15／LevelMix=16，scaled transient。一般 blur σ > 4／8／16 → 1/2／1/4／1/8（SW 同算式；7 張 SW golden 重產，差 ≤ 12/255，`ui_fx_stack` 2 px SDF 門檻翻轉）。偏離：金字塔共用只在 Direct 深度 0（Layer 與巢狀各自建）；mix-blend 的 backdrop 拷貝仍每元素一張（讀的是合成當下）；`setSceneStatic` 只快取玻璃面板結果（mix-blend 每幀一拷貝 + 一 quad），SW 無 sceneStatic 快取。實測（RTX 4070 Ti，1080p，三玻璃 + 暈影）：靜態幀 GPU 0.171 ms、CPU build 0.008 ms、3 → 8 面板 +0.032 ms、Direct vs Layer 0.171 vs 0.170 ms——全部達 §23.aa.6。`xtframe-view --scene/--scene-animate/--perf`。截圖 `Docs/images/gallery/hud-overlay-scene-{static,animated}.png`。

## 23.ab 無邊框宿主（2026-10-19；外部需求：Aux Duty 桌面程式「不要 OS 的視窗框，拖曳用的框包含在整個 UI 裡」；T-376）

- **形態**：宿主以 borderless 建視窗，整個畫面由 XTFrame 繪製，含最上列的拖曳區與 `Min / Max / Close`。`xt_shell` 提供這套（13 §13.5 T-376）；用其他視窗系統的宿主自行對應（原則相同：OS hit-test 回「可拖曳／調整大小／一般」三類）。核心只加一個樣式屬性 `app-region`（05 §5.9.10），其餘都在 `xt_shell`。
- **hit-test 決策**（純函式，`xt_shell`）：(1) 視窗四邊與四角 `resizeBorder` 邏輯像素內 → 對應的 `Resize*`（最大化時不回 Resize）；(2) 否則 `Document::hitTest(windowToDesign(pos))` 命中的元素沿祖先找第一個非 `none` 的 `app-region`：`drag` → `Draggable`，其餘 → `Normal`；(3) 沒有文件或沒命中 → `Normal`。OS 的 Snap、Aero Shake、macOS 原生拖曳都因此保持正常。雙擊拖曳區 = 最大化／還原：**Windows 由 OS 處理**（`Draggable` 區是非客戶區，`WM_NCLBUTTONDBLCLK` 走 `DefWindowProc`，SDL 不會送 `SDL_MOUSEBUTTONDOWN`，宿主不可再判一次）；其他平台待實測，若 OS 不處理才由宿主以 `setEventHook` 判並呼叫 `toggleMaximize()`（2026-10-20 修正，Aux Duty XF-035）。
- **視窗控制**：UI 用 `emit(<port>)` 送 out 事件，宿主訂閱後呼叫 `App::minimize/toggleMaximize/close`。`xtframe-view --borderless` 的約定：out 事件 port 名 `winMinimize`、`winMaximize`、`winClose`。
- **Windows 體驗**：borderless 視窗預設無陰影無圓角 → `borderlessShadow` 以 DWM 取陰影；最大化不得蓋住工作列（SDL 已處理，`xtframe-view --borderless` 實測確認，若蓋住則在 `WM_GETMINMAXINFO` 處理，放同一個平台檔 `Code/Runtime/shell/src/platform_win.cpp`）。
- **工具**：`xtframe-view --borderless [--resize-border N]`；範例 `Examples/demos/borderless/`（頂端一條 `app-region: drag` 的標題列、三顆 `no-drag` 按鈕 `emit(winMinimize|winMaximize|winClose)`）。`--windowed-frame` 類的退路由宿主自己決定（不傳 `--borderless` 即是）。
- **Live resize（T-377，Aux Duty XF-040）**：拖曳邊框期間 OS 的 modal loop 讓 `SDL_PollEvent` 不返回，內容會到放開才重排。`xt_shell` 以 event watch 在 loop 內代為出幀（13 §13.5 T-377）；`WindowConfig::liveResize` 預設開。宿主只要知道：`onFrame` 可能在 `SDL_PollEvent` 之外被呼叫，仍在主執行緒、不重入。
- **預設 hit-test 只看 `currentDocument()`**（Aux Duty XF-038）：多文件或覆蓋層蓋在拖曳列上時以該文件為準；需要別的規則的宿主用 `setHitTest` 自己組合（`hitRegionFor` + `Document::hitTest` + `appRegionAt` 都是公開的）。`app-region` 的可拖曳面積是元素盒子，純文字元素要靠外層容器承接（XF-036）。
- **編輯器**：不受影響；展示空間不模擬視窗框。

**實作記錄 T-376**（2026-10-20）：`xt_shell` 依上文實作（`hitRegionFor`、`App::setHitTest`、視窗控制、`WindowConfig::borderless/borderlessShadow/resizeBorder`）；平台部分在 `Code/Runtime/shell/src/platform_win.cpp`（其他平台 `platform_other.cpp` 空實作）。Windows 11 實測（`xtframe-view --borderless Examples/demos/borderless/borderless.xt`，以 Win32 合成滑鼠／鍵盤）：標題列拖曳可移動、內容區拖曳不移動；WM_NCHITTEST 標題列 = HTCAPTION、按鈕與內容 = HTCLIENT、四邊四角 = 對應 HT*；右邊緣拖曳可調整大小；Max 鈕最大化／還原、Min 鈕最小化、Close 鈕結束（exit 0）；Win+← 貼齊左半（工作區內）、拖到螢幕頂端最大化、從最大化往下拖還原；DWM 陰影存在（淺色背景上視窗邊外約 50 px 漸暗）。偏離兩處：(1) SDL 2.30 的 borderless 視窗只有 `WS_POPUP | WS_MINIMIZEBOX`，沒有 `WS_THICKFRAME | WS_MAXIMIZEBOX` 時 OS 不依 hit-test 調整大小、也不最大化或 Snap，所以建視窗前設 SDL hint `SDL_BORDERLESS_RESIZABLE_STYLE=1`（SDL 仍以 `WM_NCCALCSIZE` 把非客戶區設為 0，不出現框）；(2) 加上該樣式後最大化**會蓋住工作列**（SDL 對 borderless 可調視窗的 `WM_GETMINMAXINFO` 回整個螢幕，實測 -7,-7,2055,1159 對工作區 0,0,2048,1104）。`WM_GETMINMAXINFO` 是 send 而非 post，`SDL_SetWindowsMessageHook` 只看得到 PeekMessage 取出的 posted 訊息，攔不到；改以 comctl32 `SetWindowSubclass` 在 SDL 的視窗程序之後把 `ptMaxPosition/ptMaxSize` 改為所在螢幕的工作區，實測最大化 = 0,0,2048,1104。另：`xtframe-view` 除了 flow `emit` 節點，也以 `ContextListener::onHostEvent` 接 UI 自己的 `emit(winMinimize)` 等（沒有 flow 時文件的 out 事件不會變成 flow event，範例靠這條）。

**實作記錄 T-377**（2026-10-20）：`WindowConfig::liveResize`（預設開）、`App::liveResizeWatch`（`SDL_AddEventWatch`，解構時 `SDL_DelEventWatch`）、`xtframe-view --no-live-resize`。**偏離任務單一處**：OS 的 modal size/move loop 是在 `SDL_PollEvent` 裡（`DefWindowProc` 處理 `WM_NCLBUTTONDOWN`）進入的，也就是在 `runFrame()` 之內，照單上「`m_inRun && !m_inFrame`」watch 在拖曳中永遠不會出幀；改為另記 `m_inPoll`（該幀正在 `pollEvents()`）——條件為 `m_inRun && !m_inWatch && (!m_inFrame || m_inPoll)`，並且只在主執行緒、只認本視窗；watch 出的幀**不再 poll**（在 modal loop 裡 `PeekMessage` 會搶走該 loop 的滑鼠訊息），排隊的事件等外層 poll 接著處理。`run()` 的 dt 改由同一個 `m_lastFrameTime`（每次 `runFrame()` 更新）量，watch 幀用掉的時間不會再算一次；`m_lastOnFrame` 在 `run()` 進入時複製一次（只有 `run()` 期間會用到）。SDL 2.30 在 `WM_ENTERSIZEMOVE` 後以 `USER_TIMER_MINIMUM` 計時器持續送 `EXPOSED`，所以拖曳停住不動時也照常出幀；尺寸未變的 `EXPOSED` 只重畫、不 `syncViewport()`（避免每次整張重畫）。副作用：一般情況下 poll 中收到的 `EXPOSED`／`SIZE_CHANGED` 也會多出一幀（無害）。Windows 11 實測（`xtframe-view --borderless --stats Examples/demos/borderless/borderless.xt`，120 Hz 螢幕，以 Win32 合成滑鼠拖右下角約 4.7 s）：拖曳中內容即時重排，每秒 16／121／97／118／119 幀（尺寸隨拖曳 1398×779 → 1473×816 → 1284×722 變化）；對照 `--no-live-resize`：整段拖曳只記到「64 frames in 5.24 s」，放開才重排。雙擊標題列：第一次最大化（工作區 0,0,2048,1104）、第二次還原，各切換一次——確認由 OS 處理且不會切兩次。測試 `Tests/unit/test_live_resize.cpp`（XT_BUILD_SHELL）：`run()` 外 push 不出幀；在 event hook（=poll 中，模擬 modal loop）裡 `SDL_SetWindowSize` 立即多出幀且 viewport 已是新尺寸；在 `onFrame` 裡 push 不重入；`liveResize=false` 不出幀但下一幀 poll 照常同步。

## 23.ac 以 `add_subdirectory` 引入時的建置選項（2026-10-20；Aux Duty XF-033／XF-034；T-382）

- `XT_RUNTIME_OUTPUT_DIR`（預設空 = CMake 預設）：所有 `xtframe-*` 工具目標的 `RUNTIME_OUTPUT_DIRECTORY`（含各組態）設為它，上層專案不必事後補設。
- FetchContent：首次下載 SDL2／FreeType／HarfBuzz 約數十分鐘；建議上層以 `FETCHCONTENT_BASE_DIR` 指到共用快取，或以 `FETCHCONTENT_SOURCE_DIR_<NAME>` 指向已存在的原始碼。`_deps` 目錄不可事後搬移（`gitinfo.txt` 比 `gitclone-lastrun.txt` 新會觸發重 clone）。

- **（2026-10-20 補，Aux Duty XF-045；T-383）** `XT_RUNTIME_OUTPUT_DIR` 在 `add_subdirectory(XTFrame)` 前後設定皆可（T-383 起以 `cmake_language(DEFER)` 在上層目錄結束時套用，`Tools/CMake/xt_runtime_output.cmake`）；值可含 `$<CONFIG>` 等生成式；狀態訊息不展開生成式（印字面）。符號規則 lint（XF-026）看不到宿主執行期 `addClass` 加的 class，訊息會註明；要抑制時把該 class 寫在任一 `<use symbol=X class="...">` 上即可。

## 23.ad 多視窗宿主（2026-10-20 定案；Aux Duty XF-046「工作區彈出到第二螢幕、第二螢幕看板、獨立設定視窗」；T-384a／T-384b）

- **原則**：核心不動（一個 `Context` 畫一張命令清單的規則不變）；多視窗 = `xt_shell` 的 `App` 管多個 `Window`，**每視窗一個 `Context` 與渲染器**。文件載到哪個視窗的 Context 就在哪個視窗畫；跨視窗搬文件 = 在另一視窗重新載入（port 值由宿主重送）。
- **共用與不共用**：共用 `FtTextShaper`（唯讀）、`AssetResolver`；GL context 不共享，字形圖集與貼圖快取每視窗一份（代價：同一字型在兩個視窗各上傳一次，可接受）。
- **事件**：SDL 事件依 windowID 分派；鍵盤焦點視窗由 SDL 決定；觸控手勢辨識逐視窗；gamepad 送焦點視窗。
- **關閉語意**：主視窗（id 0）關閉 = `run()` 結束，副視窗一併銷毀；副視窗關閉 → `windowClosedHandler(id)` 後銷毀，`run()` 繼續。宿主要「關閉前問一下」在 handler 之前以 `setEventHook` 攔 `SDL_WINDOWEVENT_CLOSE` 回 true。
- **節拍**：多視窗都開 vsync 會逐視窗等待疊加；建議副視窗 `vsync=false`，或全部關 vsync 並 `setTargetFps(60)`。
- **第二螢幕**：`displayBounds(i)` 取得顯示器矩形，`WindowConfig::x/y`（新增，預設 undefined）或建立後 `setBounds` 放置；`Window::displayIndex()` 查目前所在。
- **工具**：`xtframe-view --second-window <ui.xt|.xtir> [--second-display N]`：主視窗照舊，副視窗載入另一份文件。
- **實作順序**：T-384a 把 `App` 的逐視窗狀態抽成 `Window`（行為不變、測試全過）；T-384b 開 API、事件分派、工具與測試。
- **實作記錄（T-384a，2026-10-20）**：新檔 `Code/Runtime/shell/include/xt/shell/window.h`、`Code/Runtime/shell/src/window.cpp`。`FitMode`、`UiScaleEdge`、`HitRegion`、`WindowConfig` 移到 `window.h`（`app.h` 引入它，既有宿主不改）。`Window` 持有 SDL 視窗、GL context + `GlRenderer` + `GlyphAtlas`（SW：`SDL_Renderer`／texture／像素／damage）、`CommandList`／`DamageList`、`Context`、`currentDocument`、drawable 尺寸、fit 狀態與 FBO、`logicalHeight`／`uiScaleEdge`、scene、文字輸入狀態、hit-test、`GestureRecognizer`、stats 累計、live resize 的逐幀旗標（`m_inFrame/m_inPoll/m_inWatch/m_lastFrameTime`）。`App` 保留 SDL 初始化／關閉、`FtTextShaper`、`AssetResolver`、gamepad、事件迴圈與 `m_inRun`／`m_mainThread`／`m_lastOnFrame`，以 `m_windows[0]` 轉呼叫；新增 `App::mainWindow()`、`App::window(id)`。`Window::renderFrame(dt, swap, frameStart)` 為 `runFrame` 在 `onFrame` 之後的半段（`frameStart` 供 stats 計時）。每個視窗各自呼叫 `GlRenderer::init()`（既有流程，重載 GL 函式指標無害）。`xtframe-view --hidden --screenshot` 對 hud-overlay 與 borderless 與重構前逐 byte 相同。
- **實作記錄（T-384b，2026-10-20）**：簽名照 13 §13.5 T-384 段，`WindowId`／`kInvalidWindow`（`0xFFFFFFFF`）在 `window.h`；`WindowConfig::x/y` 加在結尾（預設 `SDL_WINDOWPOS_UNDEFINED`）。偏離／補充：(1) `Window` 另加 `damage()`（最近一幀的 `DamageList`，測試驗證兩視窗 damage 各自獨立用）；(2) `App::window(id)` 對不存在的 id 記 log 後 `abort()`，`findWindow` 回 null；(3) id 不重用（關掉 1 之後再開是 2）；(4) `Window::close()` 對副視窗 = `closeWindow(id)`（不呼叫 handler），主視窗照舊送 `SDL_QUIT`；(5) live resize 的 `m_inFrame/m_inPoll/m_inWatch` 從 `Window` 移回 `App`（一次 poll、一個幀迴圈涵蓋所有視窗），watch 以 windowID 找視窗只 `syncViewport` 該視窗，之後所有視窗各出一幀（OS modal loop 期間另一個視窗也不凍結）；(6) 事件 `windowID` 為 0 → 主視窗，非 0 但找不到或正在關閉 → 丟棄；(7) `closeWindow` 在幀內（poll／onFrame／listener）呼叫時延到幀迴圈結束才銷毀，幀外立即銷毀；(8) 每個 GL 相關的 `Window` 方法（`renderFrame`、`syncViewport`、`setFit`、`setScene`、`readPixels`、解構）先 `makeCurrent()`（已是目前 context 就不呼叫 `SDL_GL_MakeCurrent`）；建立視窗前把 `SDL_GL_MULTISAMPLE*` 重設為 0，避免前一個視窗的 MSAA 屬性殘留；(9) 文字輸入改由 `App::syncTextInput()` 在所有視窗出幀後處理一次（焦點視窗 = 有 `SDL_WINDOW_INPUT_FOCUS` 者，無則主視窗；焦點換視窗時對舊視窗補 `CompositionEnd`）；(10) stats：主視窗行不變（含 poll），副視窗行為 `xt-stats[id]:` 且只計自己的 onFrame＋update＋render＋swap。`xtframe-view --second-window` 的副視窗 `vsync=false`（本節「節拍」建議）、尺寸預設同主視窗、`--second-display N` 以 `displayBounds(N)` 置中、關閉時 stderr 印 `xtframe-view: window <id> closed`；`--hidden --screenshot` 有無副視窗截圖逐 byte 相同。**實測**（Debug、13600K、120 Hz 螢幕、主視窗 hud-overlay UI Project＋副視窗 `hud/hud.xt`，`--stats`）：單視窗 8.32 ms/frame（vsync 上限，120 fps）；兩視窗兩者皆 120 fps，主視窗 5.8～6.3 ms/frame（含 poll 與 vsync 等待），副視窗自身 2.0～2.3 ms/frame（render 1.15～1.2 ms，差額約 1 ms＝update＋context 切換＋非 vsync swap）；關掉副視窗後主視窗回到 8.31 ms/frame 續跑。單視窗時 render ms 顯示約 7.5 ms，推測是驅動在 GL 呼叫內等 vsync 背壓而非繪製成本（兩視窗時同一份 hud 只有約 1.1 ms）。

## 23.ae 內建 nlohmann 的 ABI 隔離（2026-10-21 定案；Aux Duty XF-089 ODR 崩潰；T-396）

XTFrame 內建的 `Code/SDKs/nlohmann/json.hpp`（3.11.3）以**自己的 ABI 標籤**編譯：所有引入它的目標定義 `NLOHMANN_JSON_NAMESPACE=nlohmann::xtframe_abi`（inline namespace，`nlohmann::json` 寫法不變），使符號與宿主自帶的任何 nlohmann（含 `JSON_NOEXCEPTION` 版）不同名，不再 ODR 衝突。公開標頭（`xt/bundle/design_system.h`、`xt/editor_service/*`）含它時也帶同一定義（以 `target_compile_definitions(... PUBLIC)` 傳遞）。宿主規則：不要把 XTFrame 的 `Code/SDKs/nlohmann` 目錄加進自己的 include 路徑；用自己的那份即可，兩者可同時存在。

實作記錄（T-396）：interface 目標 `xt_nlohmann`（`Tools/CMake/xt_nlohmann.cmake`）供所有含 nlohmann 的目標連結；3.11.3 的 `NLOHMANN_JSON_NAMESPACE_BEGIN`／`_END` 不讀 `NLOHMANN_JSON_NAMESPACE`，故三個巨集一併定義（`namespace nlohmann { inline namespace xtframe_abi {` ／ `} }`）；`Tests/unit/test_json_abi.cpp` 檢查型別名，`engine_integration --json-coexist` 在同一執行檔內以未加標籤、`JSON_NOEXCEPTION` 的私有副本與 XTFrame 的副本各自解析（ctest `example_engine_integration_json_coexist`）。

### 23.ad.1 多視窗第二輪（2026-10-21 定案；Aux Duty XF-091～097；T-398）

- **文件狀態快照（XF-097）**：`Document::snapshotState() → std::string`（JSON：所有 port 值、自訂狀態、`<input>` 文字與選取、焦點元素路徑 id、每個捲動容器的捲動位置、時間軸播放位置）與 `Document::restoreState(std::string_view) → Status`（載入後、第一幀前或任何時候套用；找不到的元素略過）。跨 Context 搬文件 = 在舊視窗 `snapshotState` → 卸載 → 新視窗載入 → `restoreState`。不做 `moveDocument`（Document 與 Context 的資源綁定太深）。`hotReload` 內部改用同一套。
- **全螢幕（XF-093）**：`WindowConfig::fullscreen` 改為 `enum class FullscreenMode { Off, Exclusive, Desktop }`（`bool` 寫法保留為 `Exclusive` 的相容別名）；`Desktop` = `SDL_WINDOW_FULLSCREEN_DESKTOP`（不切解析度）；`WindowConfig::minimizeOnFocusLoss = false`（`SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS`）；`Window::setFullscreen(FullscreenMode)`。
- **關閉流程統一（XF-095）**：`App::setWindowCloseRequestHandler(std::function<bool(WindowId)>)`——OS 關閉請求**與** `Window::close()`／`App::closeWindow()` 都先經它（回 false = 否決），通過後再 `windowClosedHandler` → 銷毀；主視窗同樣適用（否決即不結束 `run()`）。不必再攔 `SDL_WINDOWEVENT_CLOSE`。
- **焦點與前置（XF-091）**：`Window::raise()`（`SDL_RaiseWindow`）、`Window::hasInputFocus()`、`App::focusedWindow() → WindowId`（無則 `kInvalidWindow`）。
- **節拍（XF-092）**：`bool App::setTargetFps(int)`（主視窗 vsync 開時回 false 且不生效）、`bool App::targetFpsActive() const`。
- **顯示器（XF-096）**：`App::displayName(int) → std::string`、`App::setDisplayChangedHandler(std::function<void()>)`（`SDL_DISPLAYEVENT` 連線／斷線／方向變更時呼叫；宿主自行重新查詢）。
- **寫明（XF-094）**：`setLogicalHeight` 只設定 `uiscale` 係數；`px` 不隨之縮放（05 §5.9.9、25 §25.4.2 的 `ui-scale` 規則），要整體縮放用 `uiscale` 單位或 `--ui-scale`。
- **快照版本 2（2026-11-07 定案；XTAtrium AT-058b 幀一致缺口；T-650）**：JSON `version` 升 2（`restoreState` 仍接受 1）。新增：`"classes":{id:[class…]}`——有 id 元素的**執行期** class 集合（與 IR 靜態 class 的差異：執行期加的與移除的都列，格式 `{"add":[…],"remove":[…]}`），還原即套用（不觸發 class 相關 trigger 的再進入；樣式重算照常）；`"clock":{"seconds":<double 以 %.17g>,"frame":<uint64>}` 文件時鐘，還原時設回並讓時間軸位置以同一時鐘求值（消除末位浮點差）；`"shown":true|false` 文件是否已完成初次顯示。**首幀規則**：`restoreState` 在第一次 `update` 前套用且快照 `shown=true` → 文件視為已顯示：不發 OnShown／lifecycle shown、不重播 intro（`show` 觸發的時間軸不啟動）、時間軸只來自快照；`shown=false` 的快照照常走初次顯示。C ABI `xt_surface_load_state` 同規則（50 §50.y），宿主不必先做靜音 `update(0)`。

- **快照版本 3（2026-11-07 定案；XTAtrium：Sherefox debate 畫面六個 class 驅動的 CSS `animation`；T-651）**：`version` 升 3（讀 1／2／3）。新增 `"animations":[{id, index, name, elapsed, paused, ended, direction?}]`，內容與 T-616 `DocumentState.animations[]` 相同（元素 id、清單位置、名稱、自 delay 結束起的經過秒數、paused、ended）；還原在樣式重算讓動畫實例存在後（同 T-616 步驟 5b 於延續的時鐘下 `layoutIfDirty()`）以 `setElapsedSeconds` 靜默配對；快照未列而文件有的動畫實例照常從頭（類別剛套上的情況）。目的：CSS animation 是正式功能，快照必須涵蓋，作者不必知道「用了 CSS animation 存檔首幀會跳」。

- **快照版本 4（2026-11-07 定案；XTAtrium AT-067 兩個缺口；T-654）**：`version` 升 4（讀 1～4）。新增 (1) `"transitions":[{id, property, from, to, elapsed, duration, easing?}]`——進行中的 CSS `transition`（每個元素 id × 屬性一筆；`from`／`to` 以該屬性的序列化樣式值、`elapsed` 自開始起的秒數 `%.17g`），還原在樣式重算後靜默重建實例（不發 transition 事件、不重啟）；快照未列而文件有的 transition 照常。(2) `"pointer":{<player>:{x, y, buttons, hover, pressed, inside}}`——每個玩家的指標位置（文件座標）、按住的按鍵位元、hover 目標 id、按住中的目標 id、是否在視口內；還原直接設回指標狀態**不派送任何事件**（不發 PointerEnter／Leave，按住中的 Click 在之後的 PointerUp 正常成立）。目的：宿主續跑不需任何繞法（XTAtrium 目前以 load → 靜音 update → 重推指標 → 靜音 update 繞過）。

- **T-654 實作記錄（#617，2026-11-07；裁決與核准）**：`version:4`（讀 1～4）。`transitions[]` 依 (id, property) 排序；`property` 為 PropId 名；`from`／`to` 為 9 位有效數的樣式文字（數字、數字＋單位、`color(srgb r g b / a)`、vec2／vec3）；`elapsed` 自 delay 結束起（delay 中為負）；`easing` 只寫 linear／cubic-bezier／steps；不寫場景節點目標、效果參數、path／stops／calc 值；綁定 `tween` 也共用 animator 表所以一併存（還原取元素 CSS spec 的 delay／easing）。`pointer`：**裁決——08 章與執行期每文件只有一個指標狀態機（只有焦點是每玩家），所以 `pointer` 永遠一筆，以最後一次指標事件的玩家為鍵**；真正每玩家指標是 08 章的設計變更，不在本單（記 PENDING）；PR body 的「per-player」措辭以此為準。還原順序新增步驟 4b（焦點後、時間軸與樣式 pass 前）：`EventDispatcher::restorePointerState` 不派送，搬 Hover／Active 位元、設 pressed 鏈、按鍵、按下時間與位置、丟棄待定 drag；並在 Context 的 `m_hoverSurface`／`m_pointerCapture` 空閒或本文件為唯一文件時指向本文件（讓之後的 PointerUp 依 08.4 成立 Click）；transition 在步驟 5b 與動畫同處以 `Animator::addTransition` 靜默重建，起點回推到與存檔逐位元相同。測試 `test_state_snapshot_v4.cpp` 六案、capi `state 10`（flip 淡出中存→首幀前載→61 幀逐位元組）、`state 11`（按住存→載→up → Click 一次、Enter 零次）；core 2034、capi 54、golden 174、flow 192。

- **T-651 實作記錄（#614，2026-11-07；一項核准）**：`version:3`（讀 1～3）；`animations[]` 放在 `timelines` 之後、`classes` 之前，`elapsed` 以 `%.17g`（delay 中為負）、`direction` 非 normal 才寫（讀端忽略）。配對在既有 T-616 步驟 5b（還原時鐘上 `layoutIfDirty()` 後依 (id, index, name) `setElapsedSeconds`，不發事件），快照 paused 而實例未 paused 時先 pause 再設經過秒數。**核准**：`Animator::elapsedSeconds`／`setElapsedSeconds` 改為 double（float 版在動畫起點早於存檔時鐘一半時 `startClock` 差 1 ulp，破壞逐位元組；C++ 簽名變更、非 C ABI；`updateParams` 仍用 float 助手，重設樣式行為不變）。測試 `test_state_snapshot_v3.cpp` 三案（六種樣態 30 幀逐位元組、未列動畫從 0、版本 1／2 載入、4 拒）、capi `state 9`（flip 新 UI `lit`：點擊後三個 class 驅動動畫，第 30 幀存、新 surface 首幀前載，首幀與後 60 幀逐位元組、shown 回呼 0）；core 2028、capi 48、golden 173。

- **T-650 實作記錄（#613，2026-11-07；三項裁決）**：`version:2`（讀 1／2，其他拒）。`classes`：只列執行期 class 與 IR 不同的有 id DOCT 元素（文字比對、排序）；還原以清單為準（列者 = IR + add − remove，未列的有 id 元素回 IR class），標子樹樣式髒；只在 `restoreState`（`applyState` 新步驟 2b，`exact` 時）套用，hotReload／`restoreRuntimeState` 保留新 IR 的 class；無 `classes` 欄位不動。`clock`：新 `Document::m_frame` 計數；在 `applyState` 前設回 `m_clock`／`m_frame`，且文件是 Context 唯一存活文件時一併設 `Context::m_clock`（之後各幀逐位元組相同）。`shown`＝`m_initialShown`；`shown` 且尚未 update → 設 `m_shownOnce`／`m_initialShown`，既有守門跳過 `lifecycle.shown`、OnShown／`xt_visibility_fn`、全域 `ui#.shown`、`show` 觸發器（intro）與 autofocus；`applyShow()` 也設 `m_initialShown`（停車 surface 的首次 `requestShow` 算初次顯示）。測試 core 三案、capi 三案（flip 點卡、intro 0.2 s、npdo rain 第 45 幀後 60 幀逐位元組）、flow 一案；core 2025、capi 47、flow 192、golden 173。**裁決**：(1) 多文件 Context 只設文件自己的時鐘，下次 update 被 Context 時鐘蓋回——接受（C ABI surface 皆單文件；編輯器多文件不保證逐位元組）；(2) 載入到執行中的 surface 會把 Context 時鐘倒退、進行中的 CSS transition 等時鐘追上——接受並記 50 §50.y；(3) CSS `animation` 項仍不在 JSON（T-616 的 `RuntimeState` 才有）——記 PENDING，XTAtrium 若有 CSS 動畫驅動的 UI 再補。

- **實作記錄（T-398，2026-10-21）**：簽名照本節。**core**：新檔 `Code/Runtime/core/src/document_state.{h,cpp}`（私有 `DocumentState` 與 `Document::collectState/missingState/applyState`），`Code/Runtime/core/src/mini_json.h`（`binding_runtime.cpp` 的小型 JSON 讀取器原樣搬出共用）。快照 JSON `{"version":1,"ports":{名:setPortText 文字},"states":{id:[狀態]},"inputs":{id:{value,cursor,anchor}},"focus":{玩家:id},"scroll":{id:[x,y]},"timelines":[{scope,name,t,rate,playing,reverse,loop,hold}]}`；port 值存成 `setPortText()` 吃的文字（數字 9 位有效、struct／list 為 JSON），還原即 `setPortText`（值有變時照常通知 listener、觸發 `port()` trigger，與宿主寫入相同）；Event port 與 repeat 實例 port 不存。還原順序 port → 自訂狀態 → input（`setEditValue`，不發 Change）→ 焦點 → 時間軸（快照外的群組釋放）→ 捲動（先 `layoutIfDirty()` 再夾限，不發 on:scroll）。`restoreState` 以快照為準：有 id 但快照未列的元素清自訂狀態與捲動、未列焦點的玩家清焦點；非快照（壞 JSON、非物件、version ≠ 1）回 `kInvalidArgument` 不套用。`snapshotState()` 非 const（struct／list 經 `getPort()` 會 intern 字串）。**偏離**：`hotReload` 共用 collect／missing／apply（新增帶過 input 值與捲動），但 port 仍用原本的 `adoptPorts`（型別直傳，不經文字），自訂狀態以非 exact 套用（行為與 T-200 相同）。**shell**：`WindowConfig::fullscreen` 型別為 `FullscreenOption`（包 `FullscreenMode`，可由 `bool` 隱式轉換＝相容別名；`explicit operator bool`）；GL 路徑原本忽略 `fullscreen`，現與 SW 一樣套用；另加 `Window::fullscreen()` 查詢。關閉：`App` 建構時設 `SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE=0`（否則否決最後一個視窗的 CLOSE 後 SDL 仍送 `SDL_QUIT`）；OS CLOSE、`Window::close()`、`closeWindow()` 同一路徑 `requestClose`；**行為變更**：`closeWindow(id)`／副視窗 `close()` 通過後也呼叫 `windowClosedHandler`（T-384b 原為不呼叫，兩個既有測試斷言隨之更新）；主視窗不呼叫 closed handler（`run()` 結束）；`SDL_QUIT`（訊號、平台結束指令、主視窗 `close()` 通過後推入的那個）不經 request handler。焦點以 `FOCUS_GAINED/LOST` 事件追蹤（隱藏視窗沒有 `SDL_WINDOW_INPUT_FOCUS`，測試以合成事件驗證）；gamepad／文字輸入仍用既有 `focusWindow()`。`setTargetFps(fps>0)` 在主視窗 vsync 開時回 false 不改設定；`targetFpsActive()` = 有目標且沒有開 vsync 的視窗。display handler 只對 CONNECTED／DISCONNECTED／ORIENTATION 呼叫（MOVED 不算），事件本身不再往下派。`xtframe-view --fullscreen desktop|exclusive`；`--second-window` 載入後對副視窗 `raise()`（非 `--hidden`）。實測（Windows 11，2048×1152）：`--fullscreen desktop` 視窗矩形 0,0,2048,1152；`--fullscreen exclusive` 正常開啟；`--second-window` 啟動後前景視窗為副視窗（標題 `… (2)`）。

### 23.10.7 flow 節點與 port 來源（2026-11-02 使用者定案；22 §22.15、53 §53.101）

14. **flow 節點**（manifest 欄位名 `flowInterface`，22 §22.15）：宿主腳本系統為每個帶 flow 的 UI 生成一個「`XTUI:<ui>:Flow`」節點：輸入 = Enable／Disable／Instance＋該 flow 實際接線的 system 事件（pulse：`Loading`、`Paused`、`Resumed`、`Disconnected`、`Back`；string：`Language`、`Platform`、`InputDevice`、`ReducedMotion`），輸出 = 每個 flow `emit`（pulse `<name>` 與 `<name>Value`）＋ `Instance`。宿主端的 port 清單（窗格、元件）依 `origin` 分組顯示：文件 port／觸發器 emit／flow emit／flow 寫入／system 事件，每列型別與來源，可直接複製節點型別名或建立節點。generated 的單 port 節點在 help 文字前加 `[flow]` 當該 port 由 flow 寫入；宿主對同一 port 寫入時 log 一次「also written by the UI's flow」。

## 23.af IR 3.0 宿主同步（2026-10-30；T-534；CE 側為 CE-64（雲端分支原寫 CE-62、原編號 §23.ab，主線 2026-11-04 移植時改號））

`IrEffect`／`EffectInstance` 由 80 bytes 擴為 144 bytes（`params[16]`、`colors[4]`；18 §18.2），效果參數 PropId 重編（float 1..16、color 32..35、vec2 整體 64..78），IR major 2 → 3。CE 端：`XUI2DResources.h` 的效果結構、`GpuDevice.cpp` 的 `XTUI_SAME_SIZE`／`XTUI_SAME_OFFSET` 斷言與 `SCustomEffectParams`（`params[16]`、`colors[16]`）、HLSL 包裝的 cbuffer 參數注入（最多 16 個 float 分量 + 4 色）；`.xtir` 讀取改認 major 3。沒有行為變更；Sandbox 以重新 pack 的 bundle 驗證既有場景畫面相同。

**宿主自訂 `GpuDevice` 後端的升級須知（2026-11-12；v0.15.2 之後的變更；唯讀檢查一個以 `FrameBuilder`＋自訂裝置整合、釘在 v0.15.0 的宿主所得）**

1. `gpu::DrawCmd` 尾端新增 `coverage`（R8 覆蓋率貼圖槽，T-660b）——結構大小改變；以 `static_assert` 鏡射此結構的宿主會在編譯期被擋下，須同步。`mipLod`（T-665）與 `boxShadowMulti`（T-683）佔用原有填充，不改變大小，斷言抓不到。
2. `Shading::BoxShadow`（T-668／T-680）與能力查詢（T-691，#652；10 §10.14）：`GpuDevice::supportsBoxShadowShading()` 預設 false。宿主在把 `Context` 接上自己的裝置時呼叫 `gpu::applyDeviceCapabilities(device, context)`（`xt/gpu/frame_builder.h`；裝置回 false 即 `setBoxShadowCommands(false)`，陰影走原本的效果路徑，畫面正確）。沒呼叫又沒實作者：模糊陰影不畫，`FrameBuilder::lastError` 報一次、`lastBuildStats().boxShadowSkipped` 計數。要省下離屏與模糊就實作該著色並回 true。
3. **覆蓋率 quad**（T-691 起）：`GpuDevice::supportsCoverageDraws()` 預設 false——未覆寫的裝置不再收到帶 `coverage` 的 draw，凹形／筆畫／大字／凹形裁切退回 10.11 之前的路徑（單取樣目標上這些邊緣沒有抗鋸齒，其餘正確；`lastBuildStats().coverageFallbacks` 計數）。要 1x 抗鋸齒就實作「著色 × R8 覆蓋率貼圖」並回 true。v0.15.2-code.N 期間（T-691 之前）的版本無此保護。
4. mip：`GpuDevice::requestMipChain` 的預設實作回「單層」，未覆寫的裝置不會收到帶 LOD 的 draw（拿不到 T-665 的品質，但不會出錯）。共用影像貼圖的四個介面皆有安全預設。
5. 不需宿主配合即生效：字型後援快取（T-673）、隱藏時釋放解碼影像（T-657）。wgpu 後端內部的改動（池、在途幀、預編、區塊範圍）與自訂裝置無關。

