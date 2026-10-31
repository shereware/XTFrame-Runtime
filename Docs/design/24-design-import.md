# 24 · 設計稿匯入：Figma 與 Claude Design

版本：0.4。狀態：Figma 前端（T-106～T-112、T-114、T-115、T-117、T-118）與 Claude Design 前端（T-113）已實作；實戰修訂 T-121/T-122 進行中。實作偏差見 16 章 M13 段與各任務回報。依據：`E:\coding3\_research\figma-import\01～05`、`E:\coding3\_research\claude-design-xtui\01-feasibility.md`（隔離於程式目錄之外的調查報告；本章只收結論）。

目的：美術在 Figma（或 Claude Design）做完的靜態版面，一個指令變成 bundle 來源形態（22 章），在編輯器裡接著做動畫、效果、連接點。單向匯入；再次匯入時顯示差異、不覆蓋手工修改。目標保真度：靜態遊戲 UI 七到八成（Figma）、八到九成（Claude Design）可直接用；對映不了的節點**整個點陣化成 PNG 當 `<img>`**並記進報告，永遠不靜默產生錯的樣式。

## 24.1 架構：取檔與轉換分離

```
Figma REST ──Tools/Scripts/figma_fetch.py──▶ <dump>/file.json + images.json + assets/…   （Python 標準庫，只有這一步碰網路與 token）
Claude Design canvas ──Artifact read──▶ <dump>/canvas.json + *.dc.html + _blob/…

<dump> ──xtframe-import（xt_import，C++）──▶ <bundle-dir>/bundle.xtproj + <ui>/<ui>.xt/.xts + shared/… + import-report.json
```

- **轉換器離線**：輸入是磁碟上的 JSON/HTML 與資產，可用 fixture 測試、可重跑、不需 token。核心規則（14 章）不變：`xt_import` 是獨立函式庫（`Code/Editor/Import/`，相依 `xt_core`、`xt_compile`、`xt_bundle`、vendored nlohmann；HTML 用自家寬鬆解析器，不引第三方）。
- 兩個來源共用中間表示 **`ImportNode`**（tag、id、classes、inline 宣告、children、assets、warnings）與後半段管線（樣式抽取、資產、bundle 寫出、報告）；只有前端（Figma JSON → ImportNode、`.dc.html` → ImportNode）不同。

## 24.2 Figma 前端（對映摘要；完整表見調查 03 章）

| Figma | 輸出 | 保真 |
|---|---|---|
| CANVAS 下每個頂層 FRAME | 一個 UI（`uis[].id` = frame 名稱 slug，`size` = frame 尺寸） | — |
| FRAME 有 auto-layout | `layout: flex` + direction/gap/padding/justify/align/wrap；HUG 不輸出尺寸、FILL → `flex-grow: 1`、FIXED → px | 高 |
| FRAME 無 auto-layout | `layout: absolute`，子元素依 `constraints` 換算 left/right/top/bottom/%（CENTER 用 `left: 50%` + `margin-left` 補償，不用 `translate(%)`） | 高 |
| `layoutPositioning: ABSOLUTE` 的子節點 | `position: absolute`，不參與 flex（**優先檢查**，漏判整個容器會錯） | 高 |
| GROUP | 無 opacity/blend/effect/mask/rotation 且父非 auto-layout → 刪除並上提子節點；否則保留 `<div>` | 高 |
| rotation | 需 `geometry=paths`：`relativeTransform = [[a,c,tx],[b,d,ty]]` = CSS `matrix(a,b,c,d,tx,ty)`，`θ = atan2(b, a)`（調查 03 §5.1 的負號是錯的，T-107 實測）→ `transform-origin: 0 0; transform: rotate(θ)`（含 skew 時分解 rotate/skewX/scale）；無 paths 時由 AABB 反推，45° 附近改點陣化 | 高/中 |
| RECTANGLE / ELLIPSE / LINE | `<div>`（+ `border-radius: 50%` / 高度 = strokeWeight） | 高 |
| VECTOR / STAR / POLYGON / BOOLEAN_OPERATION / TEXT_PATH | 0.3：PNG @2x `<img>`；T-110 後：SVG 靜態向量（`format=svg`） | 低→高 |
| TEXT | 單一樣式 `<span>`；有 `characterStyleOverrides` 時 `<div>` 內多個 `<span>` run（以 code point 走訪，非 UTF-16 索引） | 高/中 |
| fills | 反轉陣列順序；SOLID → `background-color`；LINEAR/RADIAL → `background-gradient`（05 §5.5c，T-115；之前只能點陣化）；ANGULAR/DIAMOND/PATTERN → 點陣化；IMAGE → `<img>` + `object-fit` 或 `background-image` | 高～低 |
| strokes | INSIDE → `border`；CENTER/OUTSIDE 在 absolute 中外擴框、在 flex 中當 INSIDE + warning；漸層/虛線 → 點陣化 | 高/中 |
| cornerRadius | `border-radius`（四角順序相同）；`cornerSmoothing` 忽略 | 高 |
| effects | DROP_SHADOW → `shadow()`；`offset=0 且 spread>0` → `glow()`（可關閉的 heuristic）；LAYER_BLUR → `blur()`（radius↔σ 以實測校正常數）；INNER_SHADOW → `shadow(... inset)`（T-111）；BACKGROUND_BLUR/TEXTURE/NOISE/PROGRESSIVE → 點陣化 + warning | 高～低 |
| opacity / blendMode | `opacity`；blend 依 09 章清單對映，不在清單者忽略 + warning | 高 |
| isMask | 矩形/圓形不透明 mask → `overflow: hidden` + `border-radius`；其他 → PNG 遮罩（LUMINANCE 離線轉 alpha） | 高/中 |
| COMPONENT_SET / INSTANCE | INSTANCE 直接展開 children（REST 已套用 override）；`componentId` 只用來把共通宣告抽成 `.c-<Set>` 與 `.c-<Set>--<prop>-<value>` modifier class | 高（視覺）/中（語意） |
| State variant + ON_HOVER 互動 | `.c-X:hover { 差異 }` + `transition:`（duration/easing 取自互動） | 中～高 |
| ON_CLICK NAVIGATE / OVERLAY / KEY_DOWN（含手把 device） | 寫進 `<ui>.xtflow` 的邊（`#id.Click` → 目標 UI id）供宿主/流圖用；不猜行為 | 中 |
| SMART_ANIMATE | 0.3 降級為 DISSOLVE + warning | 低 |
| 字型 | 依序：`--fonts-dir` 掃 name table 比對 `fontPostScriptName` → Google Fonts 下載（OFL）→ 列為 missing font；付費/系統字型不打包 | 中 |

取檔（`Tools/Scripts/figma_fetch.py --file <key> --token $FIGMA_TOKEN [--ids …] -o <dump>`）：`GET /v1/files/:key?geometry=paths`、`GET /v1/files/:key/images`、需要點陣化的節點以 `GET /v1/images/:key?format=png&scale=2&use_absolute_bounds=true` 批次取（URL 14/30 天過期、速率限制緊 → 全部落地成檔案）。社群檔案要先 Duplicate 到自己的 Drafts 才有 file key（README 必寫）。**配額（2026-09-24 實測）**：`/v1/images` 匯出端點對個人帳號有硬性配額，約 150 次向量匯出後回 `429`、`Retry-After ≈ 4.5 天`；`figma_fetch.py` 遇到超過 5 分鐘的 Retry-After 直接放棄並保留已抓的（`images.json` 每批寫一次，`--skip-existing` 可續跑）。大檔的向量圖示要分多天抓，或請設計端在 Figma 內把圖示改成元件實例以減少唯一 id（alias 去重）。

## 24.3 Claude Design 前端

`.dc.html` = HTML + inline flex CSS 的受限子集：剝除 `<x-dc>`、`<helmet>`（其 `<style>` 的規則轉進 `.xts`）、`<script data-dc-script>`；根 `div` 的固定寬高 → UI `size`；inline `style` → 穩定 id 的 `.xts` 規則；`border: 1px solid` 丟掉 `solid`；`{{hole}}` 以 `data-props` 預設值展開；`<sc-for>/<sc-if>` 以預設資料展開一次；`<img src="/_blob/id">` → `shared/img/`；Google Fonts `<link>` → 下載 OFL 字型；`display: grid` 單列/單行退化為 flex，其餘 warning；inline `<svg>` 走 T-110；`backdrop-filter` warning；`<dc-import>` 設計系統元件拿不到 DOM → 由「XTFrame Design System」規範禁止使用。`canvas.json` 的 boards → `uis[]`。

**實戰修訂（2026-09-24，來自 `E:\coding3\_research\xtui-field-report-2026-09-23`）**：
- 元素的 `class` 屬性保留進 `classes`（`<helmet>` 的 class 規則才對得上、flow 才能 `toggleClass`）；去重產生的 `s-*` 附加在後。
- UI id：根元素的 `id` 屬性優先，其次 board `title` 的 slug；title 只當顯示名稱（manifest `name`）。
- 寫出前對每個 UI 編譯：任何 error → 匯入失敗並把 `檔名:行:欄: 訊息` 與來源節點 id 印到 stderr 與 `import-report.json.compileErrors[]`；warning 對應回節點 → 該節點降為 Medium 並記 warning。輸出先寫暫存目錄，全部通過才搬到目標（原子）。stderr 結尾印摘要：`N declarations dropped, M warnings — see import-report.json`。
- `{{hole}}` 用在顏色以外：不丟棄整個宣告——值以 `data-props` 預設展開；同時在報告 `bindings[]` 記 `{node, prop, hole}`，供 flow/宿主接線。
- `transform-style: preserve-3d`、`box-sizing: border-box` 靜默丟棄（本來就是預設）；`nav/header/footer/main/aside/article/figure` 當 `div` 別名，不扣保真。
- `xtframe-import dc --lint <dump>`：不寫檔，列出方言外寫法（行號、宣告、會得到的結果：dropped / degraded / invalid / fail）。
- 語法正規化（T-122）：`border: 1px solid rgba()`；單邊 `padding-*/margin-*/border-*-width`（直通）；`radial-gradient(circle|ellipse [<size>] at …)` → `rx ry at cx cy`；`repeating-linear-gradient` 展開成普通 stop（最多 32 個）；多層 `background` → 疊加子 `<div>`（由下而上，`position: absolute; inset: 0`，最上層才是原節點的內容）；多層 `box-shadow` → 多個 `shadow()`（≤ 16）；`text-shadow` → `shadow()`；`filter: drop-shadow()` → `shadow()`、`brightness()` → `color-adjust(brightness())`；`animation-*` longhand 合成簡寫；`font` 簡寫展開；`backdrop-filter: blur(r) [saturate(s)] [brightness(b)]` → `effect: backdrop-blur(r s' b')`（T-130；其他 backdrop 函式 warning）；`display: grid`（二維）仍 warning；`@keyframes` 區塊（T-132）：每個 keyframe 選擇器（`from`/`to`/`<pct>%`，逗號分隔的列表）的宣告同樣經 convertDeclaration——`filter`/`box-shadow`/`text-shadow`/`backdrop-filter` → `effect`、`var()` 展開、`background` 簡寫拆解、`transform` 直通；被 convert 丟棄的宣告照常 warning（附 keyframes 名稱）；其他 at-rule（`@font-face`、`@media`）仍原樣複製（`@media` 在 `.xts` 沒有意義，另 warning 一次）。
- **不做**：在 headless 瀏覽器執行 DC runtime 取渲染後 DOM（會引入瀏覽器相依，且 Claude Design 的 runtime 契約會變）；`<sc-for>`/`<sc-if>` 只以 `data-props` 預設資料展開一次。設計系統 README 明寫此限制。

**零成本前置（純文件）**：發佈一份「XTFrame Design System」artifact（README 方言規則 + `tokens.json`：色彩/字級/間距 token、允許的屬性清單、禁用 grid/bundle 元件），讓 Claude Design 從現在起產出接近 `.xt` 的畫板。

### 24.3c T-157 修訂（2026-09-25，實戰 bench 報告）

- **來源問題報告**：無對應開頭的結束標籤 → lint `degraded`（行號、標籤）；shell 下頂層元素 > 1 → `warning`（列各頂層 id 與行號；多出的元素以兄弟節點匯入，執行期合法但通常不是作者本意，03 §3.9b）。寫進 `import-report.json` 的 `sourceIssues[]`，計入 warnings；`--lint` 列出並計 finding。
- **`text-transform`**：核心無此屬性；匯入時把大小寫寫死進字元。inline 與 `<helmet>` 的單純 `tag`/`.class`/`#id` 規則皆適用，依 specificity 取規則、會繼承、`none` 重設；不支援的值或無法配對的選擇器（pseudo、組合子）一律 dropped（計入 `totals.dropped`、lint 列出）。限制：執行時切換 class 不改大小寫（原生支援記 16.3）。
- **`radial-gradient` 尺寸**：省略尺寸 = `farthest-corner`；ellipse 的 corner = 對應 side 橢圓邊長 × √2；circle 單一半徑（closest-side 取兩軸較小、farthest-side 取較大、corner 取圓心到該角距離）。ellipse 配百分比圓心輸出百分比（05 §5.5c 的 rx/ry 各相對寬/高）；circle 需元素宣告 px 盒尺寸才輸出 px，盒未知時輸出兩軸同百分比並 warning。
- 未納入 lint 的現況：`<helmet>` 規則的其他 warning（多層背景、`@media`）仍只進 stderr——保持。

## 24.4 共用後半段

1. **樣式抽取**：相同宣告集合出現 ≥ 2 次 → 抽成 class（Figma 的 `styles` 表與 component 名稱給 class 命名；否則 `s-<hash6>`）；單次出現的留在 `#id` 規則。目標：`.xts` 去重率讓 60 格背包只有一份格子樣式。
2. **id 穩定性**：節點 id 由來源 id（Figma `node id` / dc 的 `id` 屬性）雜湊成 `n-<hash8>`，再匯入時相同節點得到相同 id；名稱可讀的節點（有 `id`/`name`）優先用 slug。連接點名稱因此跨匯入穩定。
3. **資產**：圖片存 `shared/img/<hash>.<ext>`（去重與 22.3 同一套雜湊）；點陣化節點存 `shared/raster/<id>@2x.png` 並在 `.xt` 標 `data-import="raster"`；字型存 `shared/fonts/`。
4. **寫出**：`bundle.xtproj`（新建或合併：既有 UI 保留、新增 UI 加入）、每個 UI 一個目錄、`shared/base.xts`（抽出的共用 class）、`import-report.json`（每個節點：來源 id、輸出 id、保真等級、warnings、fallback 原因；總表：節點數、點陣化數、缺字型清單）。
5. **再次匯入**：以 `import-report.json` 的來源 id ↔ 輸出 id 對照；輸出 id 在既有 `.xt` 中已被手工修改（內容雜湊與上次匯入不同）→ 不覆蓋，寫進報告的 `conflicts`，並輸出 `<ui>.import.xt` 新版本供比對。`--force` 覆蓋。

## 24.4b 本機畫板（T-134；skill 形態已由 24.8 取代）

匯入器只讀畫板原始檔，因此畫板不一定來自 Claude Design：Claude Code 依設計系統 README 的方言自行寫出 `<dump>/project/canvas.json` + `<Name>.dc.html`（+ `ds/xtframe/tokens.json` 複本），走同一條 lint → 匯入 → 編譯 → 截圖 → flow 流程，就是「描述 → UI」。skill `xtui-author` 負責寫畫板並呼叫 `xtui-from-design` 的第 3～8 步；`xtui-from-design` 的輸入若是本機目錄（含 `project/canvas.json`）則跳過第 1～2 步。本機畫板的 `canvas.json` 至少含 `name`、`boards`（每板 `w/h/title`）、`order`、`designSystems: ["xtframe"]`；`title` 只是顯示名，UI id 取根元素 `id`。

## 24.5 前置能力（核心/編譯器，小任務）

| 任務 | 內容 | 章節 |
|---|---|---|
| T-110 | SVG 靜態向量匯入：`<img src="x.svg">` 由 `xt_compile` 轉成靜態 SCEN（path/rect/circle/ellipse/line/polygon、fill/stroke/opacity、transform、viewBox；不做 text/filter/gradient 以外的 paint server）；SW/GL 既有場景繪製 | 12、11 |
| T-111 | `shadow(... inset)`：內陰影（反相 alpha 模糊，spread 為收縮），SW 與 GL | 18 |

## 24.6 任務（M13）

| 任務 | 內容 | 相依 |
|---|---|---|
| T-106 | `Tools/Scripts/figma_fetch.py`（標準庫 urllib、重試、速率限制、資產落地）；三個手寫的 Figma JSON fixture（wireframe 基本路徑、component/variant、HUD constraints+效果）進 `Tests/import/fixtures/figma/` | — |
| T-107 | `xt_import` 骨架：`ImportNode`、Figma 前端的節點/佈局/樣式（24.2 除資產、元件、互動）、`.xt/.xts` 寫出、報告；wireframe fixture 保真近 100% | T-106 |
| T-108 | 資產與 fallback：圖片 fill、點陣化規則、遮罩、字型策略；`shared/` 去重 | T-107 |
| T-109 | 元件/variant → class、樣式抽取去重、`:hover` + transition、互動 → `.xtflow` 邊 | T-107 |
| T-110 | SVG 靜態向量匯入（24.5） | — |
| T-111 | `shadow(... inset)`（24.5） | — |
| T-112 | `xtframe-import` CLI（`figma <dump>` / `dc <dump>`）、`bundle.xtproj` 合併、再次匯入差異、README；用 `04-samples` 的 D/C/B 三個檔實測（需使用者提供 token 與 Duplicate 後的 key）並把報告放進 `Docs/tasks/T-112-report.md` | T-108, T-109 |
| T-113 | Claude Design 前端：`.dc.html` 寬鬆解析、24.3 規則、`canvas.json`；XTFrame Design System artifact 文件 | T-107, T-110 |
| T-114 | 編輯器：File > Import…（呼叫 `xt_import`，顯示報告摘要、缺字型與衝突清單） | T-112 |

## 24.8 整合形態（2026-09-24 定案，M15）

使用者的要求：**一個 skill 通吃**——在 Claude Code 說「做一個 HUD」，或給一張 Claude Design 畫布，結果是合規的 XTFrame UI，直接進 xtui editor；不用知道方言、不用挑 skill、不用手動掛設計系統、不用自己跑工具。M13/M14 的三個 skill（`xtui-from-design`、`xtui-author`、`xtui-flow`）與「先去 Claude Design 掛設計系統 artifact」的前提**廢止**，收斂成：

1. **skill `xtui`**（`Skills/skills/xtui/`，安裝到 `~/.claude/skills/xtui/` 或 `<project>/.claude/skills/xtui/`）。`SKILL.md` 只放入口判斷與流程，細節放 `references/`，Claude Code 依需要載入（progressive disclosure）：
   - `references/dialect.md`：方言契約（原設計系統 README，**單一來源**；`design-system/README.md` 改為它的逐位元複本，CI 檢查一致）與 `references/tokens.json`。
   - `references/canvas.md`：Claude Design 畫布的兩種用法——(a) 在 Claude Code 內以 Design artifact 型別**新建畫布並寫畫板**（`Artifact quickstart intent:"design"` → `type_url` 建立 → 依該型別的指示寫 `.dc.html` 板），使用者可在 Claude Design 上看與改；(b) 讀回既有畫布（`list scope:"files"` + `read paths`）。兩者都落成本機 dump（24.4b 形狀）。
   - `references/verify.md`：lint → 匯入 → 編譯 → 截圖 → 開 editor 的固定流程、每一步的指令與失敗處置（含 hang 偵測）。
   - `references/flow.md`：互動（原 `xtui-flow`）。
   - `references/template.dc.html`：範本板。
   入口判斷（SKILL.md）：輸入是文字描述 → 寫本機畫板（預設；使用者要「在 Claude Design 上看」才走 canvas.md (a)）；是 `claude.ai/.../artifact/` URL → canvas.md (b)；是含 `project/canvas.json` 的目錄 → 直接驗證；是含 `bundle.xtproj` 的目錄 → 改畫板後重匯入。之後一律走 verify.md，**最後一步是開 xtui editor**（`xtframe-editor <bundle.xtproj>`，背景啟動；找不到就建置）。互動在使用者要求時於同一入口內做（flow.md），不是另一個 skill。合規由 skill 自己保證：lint 或編譯有錯就改畫板重跑（≤ 3 輪），不把方言問題丟給使用者。
2. **editor：File > Import from Claude Design…**（T-137）：與 Figma 匯入同一個對話框，來源切換；輸入是本機 dump 目錄（`project/canvas.json`），呼叫 `xt::import::importDc`；匯入後列出 UI 並可開啟。
3. 設計系統 artifact（https://claude.ai/artifact/VTFcdxxTBrbJHMRmvgoeog）保留為 Claude Design 使用者的參考，**不再是前提**；內容與 `references/dialect.md` 同步。
4. **未來（16.3）**：editor 與 Claude Design 畫布的雙向同步（editor 改動寫回畫板、畫板改動熱更新 editor）。本階段只做匯入。
5. plugin 打包（`claude plugin`）：待 skill 穩定後再做，不在 M15。

## 24.8b 兩個 skill 分工（2026-09-27 使用者定案，取代 24.9／29.7 的「skill 改走 Editor Service」）

使用者：「我們得把技能分開，要給 Claude Design 用的得直接寫再轉換，要操作 editor 的去操作 editor」。

1. **`xtui`（Claude Design 用）**：維持 24.8 的形態——**直接寫畫板（`.dc.html`）再轉換**（`xtframe-import dc`），不經 Editor Service。負責：文字描述／畫布 URL／dump／bundle 目錄四種入口、方言合規、27.5 的 **`data-xt-*` 標籤字典全部**（符號、變體、插槽、狀態、觸發、時間軸、port、綁定、token、音效、錨定、角色、字形、富文本——轉換器 `dc_import` 要實作對應生成，不只 `data-xt-name`）、再匯入往返（24.10）、`--diff-out`。互動：**優先以標籤宣告**（`data-xt-trigger`／`data-xt-timeline`／`data-xt-bind`，轉成 `on:`／`@timeline`／`bind:`），`.xtflow` 只給跨 UI 或需要運算的情況（`references/flow.md` 保留）。結尾開編輯器（不變）。`references/` 加 `ui-project.md`：從 UI Project 讀 token／契約／慣性（`shared/tokens.xts`、`contracts/`、`AGENTS.md`）作為寫板時的來源，取代 skill 自帶 `tokens.json` 的優先權（自帶檔僅在沒有 UI Project 時用）。
2. **`xtui-editor`（操作編輯器用）**：新 skill（`Skills/skills/xtui-editor/`，鏡射 `.claude/skills/xtui-editor/`）：經 `xtframe-mcp`（執行中的編輯器）或 `--spawn-headless`／stdio 呼叫 Editor Service。負責：對**已開啟的 UI** 做改動——`layer.*`、`style.*`、`timeline.*`、`trigger.*`、`bind.*`、`symbol.*`、`state.*`、意圖 op、`scene.dump`／截圖／lint 回饋、checkpoint、13 情境契約（`session.setScenario`）、`import.reimport`。`references/`：`service.md`（自 `xtui/references/` 搬過去）、`ops.md`（op 分類與常用序列，指向 `Assets/Schemas/editor-service/`）、`scenarios.md`（13 情境何時用、預算）、`recipes.md`。不寫 `.xt`／`.xts` 檔、不寫畫板。
3. **入口判斷**：使用者要「做一個 UI」或給畫布 → `xtui`；使用者要「改編輯器裡的這個 UI」、提到面板／時間軸／符號／情境，或編輯器正在跑（`service.json` 存在）→ `xtui-editor`。兩個 skill 的 `description` 明確互斥；`xtui` 完成後的後續修改若編輯器已開，建議切到 `xtui-editor`。
4. `Tools/Scripts/check_skill_sync.sh` 同時檢查兩個 skill 的鏡射。M15 記憶（單一 skill）作廢。

## 24.7 取捨紀錄

- 取檔用 Python 而非 C++：避免在 `xt_core` 之外再引 HTTPS 相依；轉換器離線可測是更大的好處。
- INSTANCE 直接展開而不建元件系統：`.xt` 沒有元件概念；class 命名保留意圖已足夠，視覺一定正確。
- 對映不了就點陣化：報告可見、結果不錯、成本最低；SVG（T-110）之後向量類回到高保真。
- 再次匯入不做三方合併：只偵測衝突並並排輸出，手工決定。

## 24.9 AI 標籤字典、再匯入配對與 UI Project 輸出（2026-09-27 定案；對應 27.5、29.8、30）

- **標籤字典**（轉換器讀、不生成 flow）：`data-xt-editor-only`（→ `editor-only`，03 §3.13；2026-10-15）、`data-xt-name`、`data-xt-symbol`、`data-xt-variant`、`data-xt-slot`、`data-xt-state`、`data-xt-trigger`、`data-xt-timeline`、`data-xt-port`、`data-xt-bind`、`data-xt-token`、`data-xt-sound`、`data-xt-anchor`、`data-xt-role`、`data-xt-glyph`、`data-xt-rich`（表見 27.5）。未知標籤警告；標籤與推導衝突時標籤優先；每次新增標籤同步 skill `references/dialect.md`。
- **`data-xt-uiscale`（T-486，2026-10-26）**：放在板的根元素，值 `height`｜`width`｜`shortEdge`｜`fit`，匯入寫成 `<name>.xtui` 的 `spec.uiscaleRule`；重新匯入時板上有值就以板為準、沒有就保留專案現值。這是 agent 設定縮放規則的唯一正規途徑（ui-project.md 第 4 節：`<project>` 下的檔只經匯入改）；「適應任何視窗」的請求建議 `fit`（兩軸都保證設計可見，寬螢幕留邊、窄螢幕縮小）。
- **lint**：有觸發無時間軸、狀態無差異、port 未綁定、符號缺契約狀態、未用 token 的字面顏色、浮動元素在安全區外、文字可能溢出（最長翻譯）。
- **再匯入配對**：以 `data-xt-name`／id 為主鍵；無名時依 AutoGameUI 的訊號排序配對——幾何（位置、尺寸）> 渲染順序 > 階層 > 文字內容；配對結果帶 `matched | new | missing` 與信心值；保留編輯器端的覆寫、時間軸、觸發、port；配對失敗率 > 20% 退回單向匯入並提示（29.8）。
- **輸出進 UI Project**：符號與變體進 `shared/symbols/`，token 進 `shared/tokens.xts`（與 Claude Design design system tokens 互轉），資產進 `shared/img/` 並登記資產清單（狀態 `draft`），字串鍵進 `strings/`。
- ~~**skill `xtui`** 改走 Editor Service（無頭編輯器），不再直接寫檔~~ → 改為兩個 skill 分工（24.8b）：`xtui` 直接寫畫板再轉換、`xtui-editor` 操作編輯器；`xtui` 的 `references/` 指向 UI Project 的契約與慣性。

- T-170：dc 匯入對 12 個純屬性的整值 `var(--x)` 保留引用並輸出 `:root { --x: … }` 區塊；其餘展開如前；figma 不動。

- T-176 後 `@media` 在 `.xts` 有意義；dc 匯入的「@media has no meaning」警告文字待改。

- T-187：`xt_writer` 對 `@` 開頭的文字與 `placeholder`／`alt`／`title` 寫 `\@`（只限可在地化屬性）；dc 的 `@media` 診斷改為 `info: @media block kept`（`kInfoPrefix` 資訊級不計入 warningCount；skill 的 `references/verify.md` 兩份仍寫舊文字，待同步）。

## 24.10 T-197 實作記錄（#200）：再匯入配對、三方合併、止損、變更清單

- **配對**（`Code/Editor/Import/src/reimport_match.{h,cpp}`）：第一輪輸出 id 相同（`data-xt-name` slug 或 `id`）或新節點 sourceId 等於上次報告記錄者 → 信心 1、`by: name`；第二輪計分——幾何 0.5（(IoU + 尺寸比)/2）、渲染順序 0.2（1 − 正規化排名差）、階層 0.2（(父已配對 + 標籤路徑 LCS)/2）、文字 0.1（1 − 編輯距離/較長者）；閾值 0.6，分數由高到低貪婪配對；幾何取自兩側在記憶體編譯 + NullTextShaper 排版的 border box，無法排版的訊號剔除、餘權重等比放大。dc 前端 `data-xt-name` 優先於 `id`。
- **合併**（`mergeUi`）——**衝突改為屬性層級**（取代 §24.4 第 5 條的整 UI 擋下）：以 `import-report.json` 新增的 `nodes[].base{text, decls, attrs}` 為基準做三方合併：本地 = 基準 → 取新值；新值 = 基準 → 保留本地；兩邊都改 → 寫 `conflicts[{prop, base, local, incoming}]` 並保留本地。sidecar `overrides` 列的屬性視同本地已改。一律沿用舊值：`on:*`、`port:*`、`bind:*`、`port`、`expose`；無 import 標記的規則（`@timeline`、狀態規則）、編輯器加的 head link／style、編輯器新增節點全部保留。設計稿已無的節點標 `data-xt-missing="1"`，`--prune` 刪除。`:root` token 依 `uis[].tokens` 逐條合併。有衝突的屬性下次基準沿用舊 base，衝突會持續出現直到一方讓步或 `--force`（整份覆寫，連 on:/port:/timeline 都不保留）。部分匯入時報告保留其他 UI 的 rows。新舊皆有但無 id 的子元素以新結構為準。
- **止損**：比例 =（本次新遺失 + 信心 < 0.7 的訊號配對）/ 上次匯入產生的舊節點數（已標 missing 者不計入分母，否則永遠無法 prune）；> 20% 不合併，寫 `<ui>.import.xt/.xts`，報告 `reimport[]` 帶 `fallback: "one-way"`，CLI exit 3。低信心門檻取 0.7 而非 0.75：內容相同只位移的節點最高約 0.745。
- **`--diff-out f.json`**：`{tokens:[{ui, name, old, new}], texts:[{ui, name, source, old, new}], colors:[{ui, name, source, prop, old, new}]}`；顏色類 = 名稱含 color 以及 background／fill／stroke。
- **服務與編輯器**：op `import.reimport {source, ui?, prune?, force?}`（destructive，`force` 需 `confirm: true`；執行前存檔、完成後 reload）；UI Project 面板「Re-import…」共用 `service::reimportDesign()`，結果樹狀列配對與信心；圖層面板 `data-xt-missing` 灰顯。
- **舊報告（無 values）**：節點 self-hash 未變則新值全勝，已改則每個不同屬性算衝突；token 保留本地。
- **skill**：canvas.md (c) 往返流程與 diff-out、verify.md、dialect.md 加 `data-xt-name`；鏡射到 `.claude/skills/xtui/`。
- **發現**：對只有 shared class、無自己 `#id` 規則的節點改樣式時，`setStyle` 把規則寫進第一個 link（`shared/base.xts`）而非 UI 自己的 `.xts`——T-204（#203）已修（17 §17.8b.3）。

### 24.8b.1 T-206 實作記錄（#205）：兩個 skill 的最終形態

- **`xtui`**：`SKILL.md`、`references/{dialect.md, ui-project.md（新：偵測 <name>.xtui，把 shared/tokens.xts 轉成扁平 ds/project/tokens.json 以 --tokens 傳入；匯入器本身不讀 .xts）, tokens.json, template.dc.html, canvas.md, verify.md, flow.md}`；description 明寫「不用於改編輯器裡已開啟的 UI → 用 xtui-editor」。`dialect.md`／`verify.md` 的 `@media` 舊說法已同步（24.9 T-187 那條待辦結案）。
- **`xtui-editor`**：`SKILL.md`、`references/{service.md（自 xtui 搬來）, ops.md（67 個 op：66 + T-197 的 import.reimport；分讀／寫／意圖／回饋／bookkeeping）, scenarios.md（13 情境）, recipes.md（recipes/ 與 recipe.list 尚不存在，改指向意圖 op）}`；鏡射 `.claude/skills/xtui-editor/`；`Tools/Scripts/check_skill_sync.sh` 檢查兩者。
- `xtframe-import --symbols-dir`；lint 結果印出。
- **待決**：在 UI Project 內匯入時，產生的 `:root` 會與 `shared/tokens.xts` 重複宣告同一批 token（編譯器警告）。**裁決**：有 `<name>.xtui` 時，`:root` 只寫 `shared/tokens.xts` 沒有的 token（新 token 附警告提示搬到 shared）；既有者一律 `var()` 引用不重宣告——**T-208（#206）已實作**：在 `xt_writer::writeBundle()` 從輸出目錄往上找 `<name>.xtui`（第一個找到者），`shared.tokens` 空字串或讀取失敗時視為 `shared/tokens.xts`；`:root` 資料改存 `ImportUi::tokens`（`renderUi(..., sharedTokens)` 接在 `extraStyles` 之後輸出，無 UI Project 時輸出逐位元組不變）；新 token 印 `info: token --x not in shared/tokens.xts; consider moving it`（`WriteResult::notes`，CLI 印出，不進 `import-report.json`；編輯器匯入對話框與 service reimport 尚看不到）；驗證編譯與再匯入版面把 token 檔當隱含樣式表；使用者 token 檔有編譯錯誤則匯入被擋。再匯入同規則。
- 設計系統 artifact（https://claude.ai/artifact/VTFcdxxTBrbJHMRmvgoeog）的 README 已落後於 `dialect.md`，需重新發佈。

## 24.11 匯入的版面契約（2026-09-27 定案，對應 25 §25.4.1）

匯入（dc／figma）預設：根改為填滿視口；子元素依最近邊自動錨定到安全區（`data-xt-anchor` 優先；`--no-auto-anchor` 關閉）；板高寫入 `spec.logicalHeight`（專案未設時）；`fit` 不再預設 `contain`，改 `native`。再匯入沿用同規則並保留手動改過的錨定（24.10 三方合併）。T-210（#209）：`Code/Editor/Import/src/auto_anchor.{h,cpp}`（dc 與 figma 共用的 writer pass，盒以 layoutRects 量測）；`WriteOptions::autoAnchor`、`WriteResult::logicalHeightSet`（專案 logicalHeight 仍為 1080 時寫入第一張板高，CLI 印出）；規則細節與差異見 25 §25.4.1.1。

> T-209（#208）：`import-report.json` 加 `notes[]`（資訊行，如 `:root` token 提示；不計 warningCount；只含本次匯入）；匯入對話框「Notes」清單、UI Project 面板 Re-import 結果的「info」列；`import.reimport` 回傳加 `notes`。

> 22 §22.11：skill `xtui` 的產出改為 UI Project（`<name>.xtui`），不再寫 `bundle.xtproj`；verify.md 開編輯器用 `<name>.xtui`。T-213（#212）已實作：SKILL.md／verify.md／flow.md／canvas.md／ui-project.md 同步；規則見 22 §22.11.4。

## 24.12 匯入器的輸入上限與不信任輸入（2026-10-02 審查修正；T-257 #267、T-261a #275）

**深度上限**（以 Debug、預設 1 MB 堆疊實測：board 匯入在 125～150 層溢位、`figma::readFile` 在 100～125 層、原生 Lottie 在 170～180 層，所以不用 256）：

| 輸入 | 上限 | 超限時 |
|---|---|---|
| HTML board | 64 個未關閉元素 | 更深的標籤攤平到第 64 層，一則 source issue |
| Figma 節點 | 64 層 | 更深的子節點不讀，一則診斷 |
| Lottie | 見 11 §11.10 | |

**不信任的 JSON**（`canvas.json`、Figma dump、Lottie、上一次的 `import-report.json`）：

- 一律經 `Code/Editor/Import/src/untrusted_input.h` 的型別檢查存取函式；型別不對的成員當作不存在（用預設值）並記一則 source issue（例如 `board "x": "w" is not a number; ignored`）。
- 所有報告以 `error_handler_t::replace` 輸出：含無效 UTF-8 的字串（孤立 surrogate 的圖層名）以 U+FFFD 取代，不丟例外。
- `xt_import` 的每個公開進入點（`importDc`、`importFigma`、`writeBundle`、`importLottie`、`loadReimportBaseline`、`diffBundle`、`exportLottie`）以 `catchAll` 包住：漏掉的例外變成 `kInternal` 加 `internal error: …` 訊息，不會終止編輯器。

**資料裡的路徑**：board key、`<img src>`、`/_blob/` id、Figma 的 `images`／`rasters`／`vectors` 目錄都先過 `resolveInside(root, path)`：空、絕對、帶磁碟機、含 `..`、或正規化後離開根目錄者拒絕；blob id 必須是單一路徑段。被拒的項目當作缺少的資產並回報 `path outside the import dump: <value>`。

**`Tools/Scripts/figma_fetch.py` 下載**：只取 `https://`；以 64 KiB 分塊讀、上限 64 MiB、每次嘗試 120 秒期限；違反者不重試。

## 24.13 Node.js 版轉換器：同一份 C++ 編成 WebAssembly（2026-10-28 使用者定案；T-503）

**需求**：npm 技能套件（RELEASE-MAP 第 4 步的轉換器套件）在沒有對應平台執行檔的地方——雲端 session、沙箱、只有 Node 的機器——也要能跑 `xtframe-import dc`，讓 xtui 技能的 deliver mode 退場。

**取捨**：不做純 JavaScript 改寫。dc 路徑約 11,000 行 C++，而且寫出前的兩段仰賴執行時本體：每個 UI 以 `xt_compile` 編譯驗證（24.3），自動錨定（24.11、T-210）與再匯入配對（24.10）以 `layoutRects()`——編譯後由 `Document` 真的排版——量測盒子。JS 改寫等於永遠 `--no-auto-anchor` 且不驗證，輸出與桌機版不同，還要雙份維護。改用 **Emscripten 把現有程式編成 wasm**：單一程式碼來源，輸出逐位元相同。

**形態**：
- 建置：同一棵 CMake 樹經 `emcmake` 設定，`XT_BUILD_GL/GPU/WGPU/SHELL/TEXT/TESTS/EXAMPLES/EDITOR=OFF`，只建目標 `xtframe-import`；輸出 `bin/wasm32/<Config>/xtframe-import.js`（Emscripten 載入器）＋ `xtframe-import.wasm`（約 5 MB，Release）。平台標籤 `wasm32`（`xt_platform_tag`）。`XT_BUILD_TEXT=OFF` 不影響匯入輸出：寫出器的編譯驗證與量測本來就用 `NullTextShaper`。
- 連結旗標（`Code/Tools/CMakeLists.txt`）：`-sNODERAWFS=1`（直接讀寫宿主檔案系統的真實路徑，不經虛擬 FS 暫存）、`-sENVIRONMENT=node`、`-sMODULARIZE=1 -sEXPORT_NAME=createXtframeImport -sINVOKE_RUN=0 -sEXPORTED_RUNTIME_METHODS=callMain`（包裝器自己呼叫 main、拿回傳值）、`-sEXIT_RUNTIME=1`（`exit(n)` 傳到包裝器）、`-sALLOW_MEMORY_GROWTH=1`、`-sSTACK_SIZE=4MB`（24.12 的深度上限以 1 MB 堆疊實測；Emscripten 預設只有 64 KB）。
- 例外：`xt_enable_exceptions()` 在 Emscripten 下加 `-fwasm-exceptions`（編譯與連結；Node 17+ 原生支援）。允許例外的層照舊，`xt_core` 仍 `-fno-exceptions`。
- 警告：wasm32 是 32 位元目標，`begin() + uint32_t` 這類迭代器算術轉成 32 位元 `difference_type` 時觸發 `-Wsign-conversion`，約 50 處、在所有 64 位元 CI 腿都無事。`xt_set_warnings()` 只對 Emscripten 關掉這一個警告，其餘 `-Werror` 照舊；`file_bytes.h` 的 `streamoff → streamsize` 補明確轉型。`xt_sw` 的 AVX2 單元在 Emscripten 下不編（它回報 x86 處理器但沒有 AVX2）。
- 包裝：`Code/Tools/import-wasm/`（npm 套件目錄；`package.json`、`bin/xtframe-import.js`、`README.md`；`dist/` 由建置產生、不入倉）。`bin/xtframe-import.js` 載入 `dist/xtframe-import.js`，以 `noInitialRun` 建立模組後 `callMain(argv)`，退出碼從 `onExit`／`ExitStatus`／回傳值取得。命令列、寫出的檔案、診斷與退出碼與原生工具相同。
- 腳本：`Tools/Scripts/build_wasm_import.sh`（設定＋建置＋複製到 `dist/`；找 `emcmake`，否則 `EMSDK`／`EMSCRIPTEN_ROOT`）；`Tools/Scripts/check_wasm_import.sh`（原生 vs wasm：`--version`、22 張 dc fixture 板的 `dc` 轉換與兩張板的 `--lint`，比退出碼、stdout／stderr（輸出目錄名正規化）、整個專案目錄逐位元；`*.meta` 的隨機 UUID 行先剔除——原生版自己跑兩次也不同）。2026-10-28 實測：22 板全部一致。
- 套件：名稱 `@xtframe/import-wasm`（npm scope `@xtframe`，2026-10-26 使用者定案；2026-10-31 審查 §2.5 改正）；`private: true`——本倉不 `npm publish`，發佈由技能倉做；`version` 跟主倉版號，由版號單更新（RELEASE-MAP §4）；授權是編輯器 EULA（匯入器屬 E 層，非 MIT 執行時），`LICENSE` 由技能倉打包時從 `Legal/EULA.md` 複製。打包仍由 `shereware/xtframe-skills` 做（先跑 `build_wasm_import.sh` 再 `npm pack`），本目錄是唯一來源。
- CI：`ci.yml` 新增 `wasm` 工作（GitHub-hosted；setup-emsdk 固定 6.0.13、原生 Release 的 `xtframe-import`、`build_wasm_import.sh`、`check_wasm_import.sh`）。不在必要工作清單，由使用者決定是否加入分支保護。

**限制**：wasm 內沒有網路，但匯入器本來就不下載（字型由 `--fonts-dir` 提供）；啟動多一次 wasm 實例化（數十毫秒）；只支援 Node 18+（`engines`）。

**後續（未排）**：同法把 `xtframe-compile` 與 `xtframe-view --headless`（軟體渲染器）編成 wasm，verify.md 的編譯與截圖步驟就能在雲端跑，技能的 deliver mode 可以整個移除；屆時 `check_wasm_import.sh` 擴成三個工具。
