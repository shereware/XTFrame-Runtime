# 00 · 總覽

> XTFrame 設計文件 · 版本 0.1 · 2026-09-14
> 本系列文件是實作的唯一依據。實作者（含 AI 子代理）不得自行更改此處定案的決策；遇到文件未涵蓋的情況，回報而非猜測。

## 0.0 名稱（2026-09-29 定案）

產品名 **XTFrame**（因與既有產品撞名，前一個名稱停用，樹內不保留、也不提供相容別名）。規則：

| 範圍 | 寫法 |
|---|---|
| 產品、CMake `project()`、視窗標題、`%LOCALAPPDATA%` 目錄、登錄 ProgID | `XTFrame`（ProgID `XTFrame.Project`） |
| 工具執行檔 | `xtframe-editor`、`xtframe-view`、`xtframe-pack`、`xtframe-import`、`xtframe-compile`、`xtframe-bake`、`xtframe-browse`、`xtframe-mcp`、`xtframe-gpu-probe`；腳本 `Tools/Scripts/xtframe-service-cli.sh` |
| 小寫識別（路徑、MIME、設計系統 id、暫存目錄） | `xtframe`（`ds/xtframe/`、`application/x-xtframe-ui`、`%TEMP%/xtframe-view`） |
| 巨集與環境變數 | `XTFRAME_*`（如 `XTFRAME_MCP_TOKEN`）、`XT_XTFRAME_*_EXE`；`XT_*` 選項不變 |
| i18n 檔 | `Code/Editor/I18n/xtframe_editor_<lang>.ts` |
| **不變** | 命名空間 `xt`、模組與標頭 `xt_*`／`xt/`、副檔名 `.xt` `.xts` `.xtir` `.xtcl` `.xtflow` `.xtfx` `.xtui`…、CE 插件 `CryXTUI`／`EditorXTUI` 與 cvar `xtui_*`、skill `xtui`／`xtui-editor`、容器 `.xtpack-ce`／`.xtsrc-ce` 與魔數 |

英文單字 extend／Extended（Unicode 屬性 `GBP_Extend`、`extendSelection` 等）不是品牌名，不改。git 歷史、既有 tag（v0.2.0～v0.7.0）不改寫。T-216（本倉）與 CE-36（CE 插件倉）實作；GitHub 倉庫改名為 `XTFrame` 與 `XTFrame-CRYENGINE`。

**實作記錄（T-216 #215，完整 CI PASS）**：14 個路徑 `git mv`、190 個檔案改內容（腳本替換 1164 處）；bundle manifest 欄位與 IR 產生器字串同步為 `bundle.xtframe`／`xtframe-compile <ver>`；黃金圖 `ui_3d_text_perspective`（畫出產品名）重生。樹內唯一殘留：`Tests/fuzz/crashes/ir-1.bin`（二進位 fuzz 重現檔，字串表內含舊產生器名，改動會破壞位移，保留）。GitHub 倉庫已於 2026-09-30 改名。設計系統 artifact 與藍圖 demo artifact 已以新名稱重新發佈。既有建置樹需重新 `cmake -S . -B build/vs2022-editor`（專案檔名已變）。

## 0.1 專案定位

**XTFrame** 是一個 MIT 授權的 C++20 函式庫，提供：

1. 以 HTML/CSS 子集描述的 UI 文件模型、樣式層疊、佈局、事件。
2. 以向量場景圖表示的圖形內容，支援 Lottie 動畫匯入。
3. 一套**統一的關鍵幀時間軸**，讓 CSS 屬性與向量屬性走同一種插值器與播放器。
4. 以**中間表示（IR）**為中心的工作流：離線編譯器把 HTML/CSS/Lottie 編譯成二進位 IR，執行時核心零渲染相依，輸出**繪製命令清單**交給任意後端。

它不是 RmlUI 加 ThorVG，而是參考兩者的架構與原始碼從零重寫。以下是刻意繼承與刻意放棄的部分：

| 來源 | 繼承 | 放棄 |
|---|---|---|
| RmlUI | 文件/元素樹、RCSS 式樣式層疊、事件冒泡、宿主提供渲染介面的概念 | 網格式渲染介面、內建 Lua、data binding、`decorator` 系統 |
| ThorVG | Paint/Shape/Scene 場景圖、路徑正規化、Lottie 圖層語意、SW 光柵器演算法 | 其 C API 的可變物件模型、執行時 JSON 解析、文字圖層 |

## 0.2 已定案的頂層決策

| 項目 | 決定 |
|---|---|
| 語言 | C++20，核心不用例外、不用 RTTI |
| 授權 | MIT |
| 建置 | CMake ≥ 3.24，所有第三方以 `FetchContent` 或 vendored 單一標頭引入，不允許系統全域安裝相依 |
| 使用場景 | 嵌入引擎與獨立桌面應用兩者都支援；核心不擁有視窗 |
| 渲染 | IR 為中心；`xt_core` 零渲染相依；附 SW 光柵器與 OpenGL 3.3 兩個參考後端 |
| UI 格式 | HTML/CSS 子集（副檔名 `.xt` / `.xts`），純 C++ API，第一版無腳本語言 |
| 動畫來源 | Lottie JSON（離線編譯）、CSS `transition`、CSS `@keyframes`、程式 API |
| 時間單位 | 秒，`float` |
| 座標系 | 左上為原點，x 向右，y 向下，單位為邏輯像素（`float`） |
| 顏色 | `float` RGBA，範圍 0..1，**非預乘**存放；後端自行預乘 |
| 命名空間 | `xt` |
| 產出語言 | 程式碼、註解、識別字、提交訊息、README、API 文件、CLI 與診斷訊息**一律英文**；設計文件與任務單為內部規格，使用繁體中文 |
| 國際化 | 第一版即支援多語言字型回退、HarfBuzz shaping、UAX#9 雙向文字、UAX#14 斷行、文字元素內直排、IME 組字（見 07、08.8） |

- **本倉沒有宿主專屬功能（2026-10-10）**：宿主友善 = 通用接點（目錄 bundle、可嵌入的 `EditorWorkspace`、13／23 章的宿主契約）。任何只為某個宿主（CryEngine 或之後的引擎）存在的碼、格式、UI 命令、文件段落都放在那個宿主的倉；本倉文件只在「相關專案」提到宿主名。

## 0.3 模組一覽

```
xtframe/
├── Code/Runtime/core/       xt_core     IR 載入、場景圖、屬性、時間軸、樣式解析、佈局、事件、命令清單輸出（零相依）
├── Code/Runtime/compile/    xt_compile  HTML/CSS/Lottie 解析器 → IR（相依：nlohmann/json）
├── Code/Runtime/text/       xt_text     FontProvider 參考實作（相依：FreeType、HarfBuzz）
├── Code/Runtime/sw/         xt_sw       軟體光柵器後端
├── Code/Runtime/gl/         xt_gl       OpenGL 3.3 core profile 後端（自帶最小 GL 函式載入器，無 glad）
├── Code/Runtime/shell/      xt_shell    SDL2 視窗與主迴圈包裝，給獨立桌面應用
├── tools/
│   ├── xtframe-compile       CLI：.xt/.xts/.json → .xtir
│   ├── xtframe-bake          CLI：.xtir → 逐幀烘焙命令串流
│   └── xtframe-view          CLI：用 shell + gl 打開 .xtir 預覽
├── tests/                   doctest 單元測試 + 黃金圖測試
├── examples/
└── Docs/design/             本文件
```

相依關係為單向：`core` ← `text`、`sw`、`gl`、`compile` ← `shell`、`tools`。`core` 不 include 任何其他模組。

## 0.4 文件目錄

| 檔案 | 內容 |
|---|---|
| 00-overview.md | 本文件 |
| 01-architecture.md | 執行時管線、資料流、執行緒模型 |
| 02-ir-format.md | `.xtir` 二進位格式完整規格 |
| 03-scene-and-properties.md | 節點種類、屬性表、值型別、插值規則 |
| 04-timeline.md | 關鍵幀、緩動、時間軸、播放器、屬性層疊順序 |
| 05-style-and-css.md | 支援的 HTML/CSS 子集、選擇器、層疊演算法 |
| 06-layout.md | 佈局演算法（block、flex、absolute、文字） |
| 07-text.md | TextShaper 介面、國際化（bidi、直排、IME）、字型回退、字形快取 |
| 08-events.md | 指標/鍵盤事件、命中測試、冒泡、狀態偽類 |
| 09-command-list.md | 繪製命令清單格式（後端唯一介面） |
| 10-backends.md | SW 與 GL 後端規格 |
| 11-lottie-mapping.md | Lottie 子集及其到場景圖/時間軸的映射 |
| 12-compiler.md | 離線編譯器與烘焙工具 |
| 13-api.md | 公開 C++ API |
| 14-coding-standards.md | 程式碼規範、錯誤處理、記憶體策略 |
| 15-testing.md | 測試策略、黃金圖流程 |
| 16-roadmap.md | 里程碑與任務切割（含派工方式） |
| 17-editor.md | 編輯器（v1.0 定案 2026-09-27）：分層、文件模型（符號、時間軸、觸發、綁定）、預覽、畫布直接操作、面板、四模式、多語化、編輯器即 AI 伺服器、Qt 相容、可嵌入外殼 |
| 25-editor-blueprint.md | 編輯器藍圖總綱：人與情境、六個心智模型、自適應模型、核心新增表、宿主缺口、AI 接點、定案決定 |
| 26-layers-symbols-library.md | 圖層（名稱 = id、sidecar）、符號與實例（契約、覆寫、變體）、資產庫與素材管線 |
| 27-interaction.md | 狀態、觸發、資料綁定與轉換器、清單、flow、AI 標籤字典、音效 cue、遊戲 UI 互動（手把、拖放、疊層、多玩家） |
| 28-animation-editing.md | 具名時間軸 `@timeline`、片段、關鍵幀與緩動、帶動畫的效果、Lottie 原生化、時間軸面板、音效軌與影片片段 |
| 29-ai-endpoint.md | 成品 AI 接點：Editor Service、保護、視覺回饋、13 個情境契約、節制規格、配方庫、客戶端、Claude Design 止損 |
| 30-ui-project.md | UI Project（編輯端的核心容器；2026-10-14 重寫草案）：八個 part、跨 part 的身分與連結、標準符號庫、UI Project 空間與面板、現況歸位 |
| 31-vector-authoring.md | 向量動畫製作：向量圖層模型、筆／節點／參數形狀、布林、修改器（執行期 vs 烘焙）、morph、運動路徑、cels、Lottie 進出、`vector.*` op；M24～M27（定案 2026-09-30） |
| 32-rigging-and-deformation.md | 綁定與變形（定案 2026-10-01）：Bone／Skin（Rive 模型，頂點與兩把手各自權重；蒙皮空間 = 骨架群組空間）／IK、Transform、Spring 約束／求值順序（morph→蒙皮→修改器）／圖片網格 `<xt-mesh>` 與 `DrawImageMesh`／權重 `.xt` 可讀文字、IR 二進位／物理驅動雙向（§32.8）；單一 M27：T-243～T-248、CE-40 |
| 35-design-system.md | UI Project 的設計文件 part（原「設計系統」；2026-10-14 降為 30 章 part 細節）：三層（參考／決定／執行）、`design/` 目錄、條目與連結、色彩庫是參考知識而非變數、生成的權威文件 `DESIGN.md`、版本、`design.*` op 與 S0 四入口、設計系統模式 |
| 36-history.md | 歷史：存檔點與方案（定案 2026-10-11）：美術詞彙（存檔點、主線、方案、採用、比較、整理）↔ git；歷史面板與畫布牌子；AI 在方案上；底層 40 §40.3 |
| 40-product-blueprint-v2.md | 產品藍圖 v2（草案 2026-10-01）：專案系統與 meta、git 相容內建歷史與語意 diff／合併、AI 分支工作流與專案記憶、契約綁定與 flow 生成、3D UI 與世界整合、媒體、主機、商業層；排在 BACKLOG C 之後 |
| 47-symbol-visual-style.md | 標準符號庫的視覺風格：報紙／偵探（Broadsheet；2026-10-27）、newsprint v2（§47.13）、字型與 token |
| 48-editor-broadsheet-theme.md | 編輯器的 Broadsheet 主題（paper／night-desk、圖示調色；2026-10-27） |
| 49-symbol-sets.md | 符號庫、組與集（Photoshop 筆刷模型）：預設庫、掛載、`.xtsymbols` 匯入匯出、`xtframe-symbols`、`symbols.*` ops（2026-10-27） |
| 50-c-abi.md | C ABI 宿主契約 `xt_capi`（共享庫 `xtframe`）與 Unity 插件 `com.xtframe.runtime`（2026-10-28） |
| 51-builtin-ai.md | 內建 AI 代理與網路層（定案 2026-10-27）：`xt_net`（作業系統原生 TLS、`SecretStore`）、`xt_ai`（Anthropic／OpenAI 相容供應商、`AgentRunner`、工具 = Editor Service op）、AI 面板 (c) 模式、無頭 `--agent`、契約 `model`／`budget.tokens` 欄位；M41 T-515～T-517 |
| 53-flow-language.md | Flow 語言規格（T-559 整理，2026-10-31）：`.xtflow` 格式與反推 JSON Schema、型別、23 種節點目錄、連接點反射、規則與錯誤字串、宿主介面與 C ABI 對應、conformance 測試集、文件與實作不一致表 |
| 18-effects.md | 效果與 shader（0.2）：內建效果、模板、自訂 HLSL 片段、後端與引擎整合 |
| 19-3d-space.md | 3D 空間（0.2）：AE 式 2.5D 圖層、攝影機、深度排序、投影繪製 |

## 0.5 術語

> 編輯器介面用語（圖層、片段、關鍵幀、連接點…）的統一表在 `Docs/design/GLOSSARY.md`（2026-10-09 草案，待使用者審閱）；本節只列執行時核心名詞。

| 術語 | 定義 |
|---|---|
| **Document** | 一份 IR 載入後的執行時實體，含元素樹、實例化的場景、樣式表、時間軸 |
| **Node** | 場景圖中的一個節點；Element 是 Node 的一種 |
| **Element** | 有佈局盒、可被 CSS 選擇的節點 |
| **Scene** | IR 內的向量子樹模板（來自 Lottie 或手寫），可被多次實例化 |
| **Track** | 一個（節點, 屬性）對應的關鍵幀序列 |
| **Timeline** | 一組 Track 加上時長、標記 |
| **TimelineInstance** | 執行時正在播放的 Timeline，含起始時間、速度、循環模式 |
| **CommandList** | 一幀的繪製命令，核心輸出、後端輸入 |
| **IR** | `.xtir` 二進位中間表示 |
