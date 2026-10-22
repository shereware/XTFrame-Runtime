# 12 · 離線編譯器與烘焙工具

## 12.1 `xt_compile` 函式庫

```cpp
namespace xt::compile {

struct Diagnostic { enum Level { Warning, Error } level; std::string file; int line, col; std::string message; };

struct CompileOptions {
    std::string rootDir;         // 解析相對路徑（link href、img src、anim src、@font-face src）
    std::vector<std::string> searchDirs;   // T-138：非空時 img/anim/@font-face 的相對路徑依序在這些目錄找，找不到才用 rootDir（22 §22.2 的 UI 目錄 → assetRoot → bundle 根）
    bool embedImages = false;    // true：圖片位元組內嵌到 ASST
    bool embedFonts  = false;
    bool strict      = false;    // 警告視為錯誤
    bool emitMeta    = true;
};

struct CompileResult {
    std::vector<uint8_t> ir;     // 空表示失敗
    std::vector<Diagnostic> diagnostics;
};

CompileResult compileDocument(std::string_view xtPath, const CompileOptions&);
// 單獨編譯一個 Lottie 為只含 SCEN+TLIN 與一個 <anim> 根元素的 IR（供 xtframe-view 直接預覽）
CompileResult compileLottie(std::string_view jsonPath, const CompileOptions&);
}
```

## 12.2 管線

```
1. parseXt(file)          → DomTree（元素、屬性、文字、<link>/<style> 蒐集）
2. 對每個 <link href> 與 <style>：parseXts(text) → RuleSet（規則、@keyframes、@font-face）
   順序：link 依出現順序，然後 style，全部串成一份 RuleSet；來源順序 = 串接後索引
3. 對每個 <anim src>：parseLottie(json) → SceneBuilder + TimelineBuilder（11）；相同 src 去重
3a. 對每個 <img src="*.svg">（T-110）：parseSvg(text) → SceneBuilder（靜態、無 TimelineBuilder），該 <img> 以與 <anim> 相同的方式引用場景（IrDocElement.scene），佈局尺寸 = viewBox（或 width/height 屬性）；相同 src 去重
4. @keyframes → TimelineBuilder（12.3）
5. 解析每個元素的 style 屬性 → inline Decl
6. 驗證：animation-name 可解析、transform 序列合法、文字/子元素互斥、資產存在（非內嵌時只檢查檔案存在並警告）
7. IrWriter 組裝 → bytes
```

所有解析器手寫，位於 `compile/parse_xt.cpp`、`compile/parse_xts.cpp`、`compile/parse_lottie.cpp`（後者用 nlohmann/json 取樹）。

### 12.2.1 SVG 靜態向量（T-110）

支援子集：`<svg viewBox width height>`、`<g transform>`、`<path d>`（M/L/H/V/C/S/Q/T/A/Z，大小寫）、`<rect>`（含 rx/ry）、`<circle>`、`<ellipse>`、`<line>`、`<polyline>`、`<polygon>`；`fill`/`stroke`（顏色、`none`、`url(#id)` 指向 `<linearGradient>`/`<radialGradient>` 的 `<stop>`）、`fill-opacity`/`stroke-opacity`/`opacity`、`fill-rule`、`stroke-width`/`stroke-linecap`/`stroke-linejoin`/`stroke-dasharray`；`transform`（matrix/translate/scale/rotate/skewX/skewY，可串接）；屬性與 `style="…"` 兩種寫法。`<defs>`（其中的 `<linearGradient>/<radialGradient>` 以 `id` 登記，**先收集整份文件的漸層再解析形狀**，所以 `url(#id)` 可指向文件任何位置、含 `<defs>` 內的；T-119 修正）、`<use href>`（展開一次）、`<title>`/`<desc>` 略過。不支援：`<text>`、`<image>`、`<filter>`、`<mask>`/`<clipPath>`、pattern、`<symbol>` 巢狀 viewBox、CSS `<style>` 區塊 → 該元素跳過並警告（`--strict` 時為錯誤）。橢圓弧 A 以 ≤ 90° 的貝茲段近似。輸出節點結構同 11.3.2（Group/Path），paint 同 11.3.3。

## 12.3 `@keyframes` → Timeline

```
@keyframes fade { from { opacity: 0 } 50% { opacity: 0.8; transform: scale(1.2) } to { opacity: 1 } }
```

→ Timeline `duration = 1.0`，對每個出現過的 PropId 建一條 Track，`target = kSelfNode`：
- 關鍵幀時間 = 百分比 / 100。
- 某 PropId 在某個百分比未宣告時，該幀**不產生關鍵幀**（CSS 語意：在有宣告的幀之間插值）。`0%` 或 `100%` 缺少時，執行時以「動畫開始時的基底值」補：編譯器在該位置寫入特殊關鍵幀 `value.type = ValueType::Enum, e = 0xFFFFFFFF`（**Neutral 標記**），Animator 遇到時以該屬性當下基底值代入。求值規則：被插值區段的**任一端**為 Neutral 時整段輸出 Neutral（不寫入，保持基底），因此 Neutral 只影響其相鄰區段。
- 每幀的 easing 取該幀區塊內 `animation-timing-function` 宣告，否則取 `animation` 宣告的 easing（執行時由 AnimationSpec 覆蓋 Linear 佔位：編譯器寫 Linear，Animator 播放時若 Track 的 keyframe easing 為 Linear 且 AnimationSpec.easing 非 Linear，套用 AnimationSpec 的）。

## 12.4 `xtframe-compile` CLI

```
xtframe-compile <input.xt | input.json> -o <out.xtir> [--root DIR] [--embed-images] [--embed-fonts] [--strict] [--no-meta]
```

結束碼：0 成功（可有警告）、1 有錯誤、2 參數錯誤。診斷輸出格式：`file:line:col: warning|error: message`。

## 12.5 `xtframe-bake` CLI

```
xtframe-bake <in.xtir> -o <out.xtcl> --fps N [--width W --height H] [--dpr R] [--range start,end]
```

流程：載入 IR → `Context` 用 `NullFontProvider`（文字警告）→ 設定視口 → 對每幀 `update(1/N)` + `render()` → 序列化 CommandList。

`.xtcl` 格式：

```
Header: "XTCL", u16 major=1, u16 minor=0, u32 frameCount, f32 fps, f32 width, height, dpr
每幀:
  u32 frameIndex
  u32 payloadLength
  依 CommandList 欄位順序：每個 vector 為 u32 count + 原始位元組（結構以 09 定義的 struct 在 x86-64 的自然佈局序列化；
  規格：所有 struct 以 4 位元組對齊、無 vtable，序列化即 memcpy）
```

`xt_core` 提供 `BakedPlayer`：載入 `.xtcl`，`frame(i) → const CommandList&`。

## 12.5b 外部資產 `engine://`（T-150）

`<img src="engine://<name>">`：名稱原樣進 ASST、`embedded=0`、不 `resolveAsset`、不 embed（即使 `embedImages`）、不警告、不呼叫 `assetNameRewrite`、不走 SVG 匯入；同名共用一個 AssetId；`CompiledAsset::external = true`。用途：宿主執行期以 `AssetResolver::setExternalTexture` 綁定引擎貼圖（10 §10.4）。

## 12.5c `xtframe-view --fit`（T-156）

視窗模式：輸入為 `bundle.xtproj` 時取所選 UI 的 `fit` 與 `size` 當設計尺寸，`--fit native|stretch|contain|cover|center` 覆寫（非法值 exit 2）；其他輸入預設 native，指定 `--fit` 時以 `--width/--height` 為設計尺寸；headless 不受影響。`xt_shell` 非 native 時先在設計尺寸×DPR 的離屏 FBO 渲染（MSAA 給離屏目標，視窗不開），再 `glBlitFramebuffer` 到 `fitPlacement` 矩形、邊框清黑；指標/滾輪反算回設計座標，IME 候選框正向換算。

## 12.6 `xtframe-view` CLI

```
xtframe-view <file.xtir | file.json | file.xt> [--root DIR]
```

`.json`/`.xt` 時先在記憶體編譯。用 `xt_shell` 開視窗、`xt_gl` 渲染、`xt_text` 字型（自動註冊系統字型目錄：Windows `C:\Windows\Fonts`、macOS `/System/Library/Fonts`、Linux `/usr/share/fonts`，只掃描 `.ttf/.otf`）。快捷鍵：空白暫停/繼續、左右鍵逐幀、R 重載檔案。

0.4（T-125）：`--event "<frame> #<id>.<Port>"`（可重複；Click / PointerEnter / PointerLeave / Focus / Blur / Change=<value>，於該幀 update 前派發）、`--pointer "<frame> x y [move|down|up]"`、`--dump-flow-values` 與 `--dump-every N`（headless，印 `<node>.<port> = <value>`）、`--print-text` 可重複、`--screenshot` 在 headless 與相對路徑下亦寫出（失敗報錯）、`--help`。

0.4（T-135）flow 自動載入：未給 `--flow` 時，若輸入 `.xt` 同目錄有同檔名的 `.xtflow`（匯入器的慣例 `<ui>/<ui>.xtflow`）就自動載入並印一行 `xtframe-view: loaded flow <path>`；`--no-flow` 關閉。輸入也可以是 `bundle.xtproj`：`--ui <id>` 選 UI（省略且只有一個 UI 時取它，多個時列出並以 exit 2 結束），`doc` 與 `flow` 欄位相對於 xtproj 所在目錄解析，`--root` 預設為該目錄。視窗模式的 R 重載同樣重載 flow。

0.4（T-139）視窗模式輸入轉發：`xtframe-view` 的快捷鍵（空白、←、→、R、Esc）只在 KeyDown 且**沒有文字焦點**（`Context::wantsTextInput()` 為 false）時被消耗，其餘所有輸入事件（指標移動/按下/放開、滾輪、其他鍵、文字輸入）一律推回 `Context` 給文件派發；`--dump-every N` 在視窗模式同樣印值；`bundle.xtproj` 的 `flow` 欄位為空時也套用同名 `.xtflow` 規則；T-20 起視窗模式把佇列整個抽走、從未轉發，所以 flow 與 `:hover` 在視窗裡沒有反應（headless 的 `--event/--pointer` 與編輯器預覽不受影響）。

T-172：`xtframe-view --play <timeline> [--seek <t>]`（headless 亦可）。

## 12.7 編輯器藍圖新增（2026-09-27 定案）

- **符號展開**：`<symbol>` 定義（本檔或 UI Project `shared/symbols/*.xt`）在解析後展開每個 `<use>`：內部元素 id 改為路徑 `Play/Bg`；`symbol(Name){}` 規則、`@timeline`、觸發、綁定依路徑改寫；`prop:/attr:` 覆寫成為該路徑元素的 `#id` 規則（特異性最高）；插槽替換；`requires` 契約驗證（缺狀態／插槽 → 錯誤）；循環引用 → 錯誤。IR 新區塊 `SYMB`：`{ symbol StringId, instanceNode, internalPaths[] }`。
- T-190：`.xtanim.json` 合併移到符號展開之前（步驟 3b）；內嵌 `<svg>`；`path-trim` 屬性。
- **`@timeline` 編譯**：每條軌道 → `@keyframes <timeline>__<path>`（時間正規化為百分比）+ 該元素 `animation` 項（`paused`），群組寫入 `TGRP` 區塊（4.10.1）；`sound{}` 進群組；`from "file#name"` 先載入 `.xtanim.json` 合併。
- **觸發／綁定／連接點**：解析 `on:`／`on-*`、`bind:`、`port:` → `TRIG`／`BIND`／`PORT` 區塊（3.12.3）；轉換器名稱 → 固定 op 編號；未知 → 錯誤；`if` 條件只接受比較類轉換器。
- **`<repeat>`**：模板子樹編譯一次，寫入 `RPT` 區塊（3.12.4）；模板內 id 為相對路徑。
- **在地化**：`@key` 字串進 `STRS` 區塊（key StringId 表）；`xtframe-pack` 檢查 `strings/<lang>.csv` 覆蓋率並 lint 未翻譯鍵。
- **`@media`**：條件式規則進 `MDIA` 區塊（T-176）；執行時評估；`calc()` 進 `CALC` 區塊。
- **`var()`**：步驟 2b（符號展開之後）`emitTokens` + `resolveRuleSetVars`；token 型別判定順序 Color → 無單位 Float → 非 auto Length → 文字；token 值進 `VARS` 區塊；規則保留引用（05 §5.9.3 的規則）；主題 = 另一組值（bundle `themes/`）。
- **資產清單驗證**（26.3.3）：宣告尺寸 = 實際、透明必有 alpha、九宮格中段可拉伸、引用存在、未引用警告。
- **`xtframe-pack`**：從 `<name>.xtui` 讀 UI 清單與發佈設定；`--atlas`（26.3.4）、`--compress bc7|bc3|none`、`--upgrade`（bundle.xtproj → <name>.xtui）、`--lang <list>`；輸出 manifest 新欄位見 22.10。
- **lint 類別**：`import`（24）、`tags`（24.9）、`contract`（符號 requires、port 綁定）、`spec`（節制規格）、`assets`、`i18n`、`safe-area`（浮動元素）。
- **`xtframe-import lottie`** 原生化（11.9）；`xtframe-import figma/dc` 輸出符號、變體、token 進 UI Project。

### 12.7.1 實作記錄（T-199 #197）：`xtframe-pack` 素材旗標

- `--atlas`（或 `publish.atlas.enabled`）、`--atlas-scope ui|bundle`：MaxRects best-short-side-fit、不旋轉、二的冪頁 ≤ `maxSize`、cell 周圍 padding（有 mipmaps ×8）並邊緣外推，圖集後像素與原圖相同。候選：只被 `<img>`（非媒體）、`background-image`、stretch 的 `border-image` 用到的圖；排除行內文字圖、repeat/round 九宮格、影片／序列／外部、Lottie 場景圖。輸出 `atlas/<name>.png|.dds`（任一 cell 有變體時附 `@2x` 頁，1x 版面不變、無變體的 cell 最近鄰放大）；原圖不再寫出；第二趟重編給 `.xtir` `atlas:<name>#<asset>`。
- `--compress bc1|bc3|bc7|none`（預設 none）：寫 BC1/BC3 FourCC 或 BC7 DX10 DDS（straight alpha、可選 mip 鏈）；帶 alpha 的圖在 `bc1` 下自動改 BC3；`assets.yaml` 的 `compress:` 逐資產覆寫。
- `--font-subset`：手寫 TrueType 子集器——glyph id 不變（GSUB/GPOS/hmtx/post 原樣），丟棄未用 `glyf` 輪廓（追複合字），重建 `cmap`（format 4 + 12）自文件文字 + 所有 `strings/*.csv` + ASCII 0x20～0x7E；CFF 原樣複製；輸出 `fonts/<stem>.subset.ttf`；`i18n` lint 逐語言列出無字型覆蓋的碼位。限制：執行期才出現的文字（port／綁定）不在集合內。
- 增量 `--ui` 與 `--atlas`／`--font-subset` 併用會被拒（exit 2）。
- 修正：打包後 `background-image` 以前不會解析（宣告仍是來源名、ASST 已改寫），現經 `ensureImageAsset` 與最終名替換修好。

### 12.7.2 `xtframe-view --dump-commands`（T-202 #202）

headless 專用，印出最後一幀的 `dumpCommandList`（含 `DrawParticles`）。任務單原寫「無新旗標」，主模型決定保留此旗標。

### 12.7.3 T-204 修訂（#203）

- `--atlas --compress` 併用：第一趟不壓縮（`deferCompress()`），圖集候選以原名收集並打包；頁面寫出時壓縮成 `atlas/<n>.dds`（manifest `atlas[].format`）；`compressDeferred()` 在 `buildAtlases()` 後壓縮未進圖集的圖並同步 `m_finalNameOf`、`uis[].assets`、`@2x`、glyph 表，再第二趟重編。
- pack 的兩趟編譯都帶 §22.2 查找順序（UI 目錄 → assetRoot → 專案根），假的 "image asset not found" 警告消失；根因是 `pack()` 未設 `CompileOptions::searchDirs`（不是兩趟編譯），`rewriteAsset()` 有自己的退路所以以前仍能打包。副作用：只存在於專案根目錄的圖現在也會打包。

## 12.8 編譯器的輸入上限（2026-10-02 審查修正；T-257 #267、T-258b #272）

所有對外部輸入的遞迴下降都有「停止下降」的上限，不只是回報：

| 輸入 | 上限 | 超限時 |
|---|---|---|
| `.xt` 巢狀（`parse_xt.cpp`） | `xt::compile::kMaxXtDepth` = 256（公開常數） | 恰一個 error，中止解析、不產生樹 |
| 編輯器的 `.xt` 掃描器 | 同一常數 | 掃描失敗，`Document::loadFromString` 回 `kInvalidArgument` |
| HLSL 子集 | 括號／區塊巢狀 64、運算式 256 項、語法樹 256 層 | 診斷 `expression nesting too deep (max 64)`、`block nesting too deep (max 64)`、`expression too long (max 256 terms)`、`expression too deep (max 256 levels)` |
| YAML-lite | 64 | `nesting too deep` |
| 符號展開 | `kMaxExpandedNodes` 1000000 節點、`kMaxExpansionPathEntries` 1000000 條實例路徑 | error `symbol expansion exceeds 1000000 nodes`／`… instance paths`，編譯失敗 |
| 展開後樹深 | `kMaxElementDepth` 256（02 §2.w；T-255b） | error |
| `<xt-star points>` | 1024（與執行期同一常數） | 夾限＋warning；非有限是 error |
| `animation` 次數 | 有限次數最大 65534 | 夾限＋warning；`infinite` 不變 |
| `steps(n)` | 1..65535 | 夾限＋warning |

- SVG 數字：`inf`／`nan`（`opacity="nan"`、`viewBox="0 0 inf inf"`、`<xt-mesh width="inf">`）視為解析失敗，走既有「不是數字」的路徑。
- `<glyph>` 或攤平的富文本內的 `<symbol>`：節點截斷改走 `truncateNodes()`，同時移除指向被截掉節點的 `symbolDefs` 項。
- 字型子集器：composite 改用工作清單（不遞迴）；cmap format 12 只走實際存在的 glyph；cmap 子表位移檢查不再 32-bit 溢位。
- Lottie 側的上限見 11 §11.10；匯入器見 24 §24.12。

## 12.9 審查修正鏈補記（2026-10-07 審查修正鏈合併；T-257b #325、T-262c #321、T-265a #294、T-273b #287、T-273c #307、T-261e #292、T-261f #310）

- 每個上限都以其數值端到端驗證（T-257b）；HLSL 巢狀以「層數」計；HTML 攤平保留標籤配對（攤平區段內未關閉的非自閉標籤會吞掉外層的關閉標籤，為既定行為）；編譯器與原生 Lottie 匯入共用同一個 parent 鏈走訪器（19 章）。
- **JSON 巢狀上限 32**：manifest、`.xtui`、`ui-project.json`、`bundle.xtproj`、`.xtflow`、`.xtanim.json`、效果模板、編輯器 sidecar、編輯器讀的 Lottie，超過 32 層在建立任何物件前拒絕（`manifest nesting too deep` 等）；`assets.yaml` 維持 64 層 YAML 上限。溢位的來源是 nlohmann 的複製建構與 `dump()`（解析器與解構為迭代）。
- 新公開 `xt::compile::parseWeightRow()`（`WeightRowText`、`WeightColumnForm`、`WeightRowError`）：編輯器與編譯器共用同一個權重列解析（mesh 列只有 `p` 欄；骨骼索引須在 `bones` 內；每欄總和 > 0）。
- **發佈時的資產命名**：`CompileOptions::publishAssetNames`（pack 開啟；預覽與 `xtframe-compile` 關）：來源在 project 外（`compile::assetLeavesProject()`：`compile::isMachinePath()`——有根名／根目錄或任何平台的 `<letter>:`、`//`、`\\` 前綴——或相對路徑經 `..` 逃出根）的資產，文件內的所有參照（`<img src>`、`<sequence src>`、`<particles texture>`、命名圖片的宣告）改寫為其 ASST 名稱，與 ASST 項共用同一個 StringId；內嵌者用檔名（撞名 `-2`、`-3`）。`AssetBuilder::referencedByName` 為寫入器中繼資料，IR 格式不變。不會變成 bundle 檔的外部參照寫為其最後一段路徑（打包時找不到的資產、來源檔表內的 `<link href>`、`<anim src>` 與 svg `<img src>` 的 scene／timeline 名、幀資料夾的 `<sequence src>`）；外部 SVG 內的 `<xt-mesh href>` 以 SVG 所在目錄解析並照一般圖片打包。影片未定（22 §22.14）。「project 內／外」只有一個定義：`compile::pathInsideRoot`／`referencePath`，編輯器與服務共用（Windows 不分大小寫）。
- **路徑一律 UTF-8**：`xtframe-import`、`xtframe-pack`、`-compile`、`-bake`、`-mcp`、`xtframe-editor` 的無視窗模式以 `wmain` + `xt::tools::Utf8Args`（`Code/Tools/Common/console_utf8.h`）讀取；`xtframe-view` 與含 GL 的 `-gpu-probe` 經 SDL2main 的 `wmain`。Windows 上不是合法 UTF-8 的路徑一律視為不存在（無 ANSI 退路）。CLI 工具把主控台輸出切成 UTF-8（只在原本不是時），正常結束時還原（`atexit`）。`xtframe-editor` 不再用 `QCoreApplication::arguments()`（Qt 5.12 在 `wmain` 行程以 `fromLocal8Bit` 解碼）。`xtframe-compile` 連結 `xt_bundle`（標準符號目錄的提示：`note: the project's standard symbols are not checked in; open the project in xtframe-editor once, or run xtframe-pack on it, to fetch them into <dir>`；`xtframe-view` 同）。
- `xtframe-editor --bake-vectors` 印出 `vector.bakedFrames`／`vector.unbakedModifier` 的略過訊息（31 §31.y）。
