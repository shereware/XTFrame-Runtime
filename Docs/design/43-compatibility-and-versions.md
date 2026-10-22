# 43 · 相容性與版本（beta 前的保護機制）

> 2026-10-21 定案（規劃 session 第 0 波 0b；使用者：「需要讓使用者的工作不會因為後續改版就失效或表現不同行為」）。本章是第 2 波 B1～B8 的依據。40 §40.15 的三種版本號與相容性矩陣維持，本章把它落到檔案與程式。人看的版本在 `Docs/RELEASE-MAP.md`。

## 43.1 範圍與承諾

- **使用者的作品** = 來源格式：`.xt`／`.xts`／`.xtflow`、`<name>.xtui` 與 parts（`shared/`、`design/`、`.meta`、`manifest/assets.yaml`、`contracts/`、`conventions/`、`AGENTS.md`）、`.xteditor.json`、專案符號、匯入的 Claude Design 畫板。bundle 與 `.xtir` 是衍生物：編輯器永遠能由來源重編。
- **承諾（beta 起）**：(1) 任何 beta 起版本寫出的來源檔，之後所有版本都能開啟、自動遷移到最新格式、寫回；(2) 開啟後的**行為**與寫出時相同——改變既有行為只能以 bug 修正為由，且必須通過 §43.4 語料庫的 diff 核准並記入遷移說明；(3) 宿主拿到的 bundle：IR `major` 在 beta 期間不變，`minor` 只尾端追加，執行時讀它發佈以來的所有 `minor`。
- **不承諾**：未標為穩定的功能（§43.8 的「實驗」）、Pro／AI 功能、編輯器版面與 QSettings。

## 43.2 來源格式的 `version` 欄與「讀舊寫新」

| 檔 | 現況 | 定案 |
|---|---|---|
| `<name>.xtui` | `version: 1`，讀取檢查 | 維持；遷移登錄表的第一個客戶 |
| `.xtflow` | 寫 `version: 1`，讀不檢查 | 讀：缺 = 1；大於已知 → 拒絕並報「需要較新的 XTFrame」 |
| `.xteditor.json` | 無 | 加 `version: 1`；缺 = 1；只含檢視狀態，未知鍵保留原樣寫回 |
| `.meta`（素材、參考） | 無 | 加 `version: 1`；缺 = 1 |
| `manifest/assets.yaml` | `version: 1` | 維持 |
| `<xt>` 根節點 | 無 | `<xt version="1">`；缺 = 1；編譯器對大於已知的值報錯 |
| `.xts` | 無 | 不加（語言層由 §43.3 行為版本管） |
| `design/*.yaml`、`contracts/*.yaml`、`conventions/*` | 無 | 加 `version: 1`；缺 = 1。**例外**（2026-10-21 裁定，T-406）：`design/design.yaml` 的 `version` 是設計系統自己的版本（35 §35.2），其格式版用 `formatVersion`（缺 = 1；T-410 補） |
| `manifest.json`（bundle） | `version: 2` | 維持；加 §43.6 契約雜湊 |

規則：
- **讀舊寫新**：每種格式一個遷移序列 `migrate<Format>(json, fromVersion) → toVersion`，放 `Code/Runtime/bundle/src/migrations/`（R 層，編輯器與 `xtframe-pack` 共用）；開啟時逐步遷移到最新並在編輯器提示「專案已從格式 N 升到 M」（一次，記在 `.xteditor.json`）；寫回永遠最新。
- **拒絕未來**：版本大於已知 → 明確錯誤，不猜。
- **未知鍵保留**：JSON／YAML 讀取遇到未知鍵不丟（`.xtui`、`.xteditor.json`、`.meta`），寫回時原樣保留，讓新舊版本交替開啟不互相破壞。
- **round-trip 測試**（B5）：每種格式每個歷史版本一個凍結 fixture，開 → 存 → 與「同內容的最新版 fixture」只差 `version` 欄。

**實作記錄（T-406）**：`xt/bundle/migrations.h` + `Code/Runtime/bundle/src/migrations/migrations.cpp`：各格式最新版常數（`kUiProjectFormatVersion`／`kFlowFormatVersion`（= `flow::kFlowFormatVersion`）／`kEditorStateFormatVersion`／`kMetaFormatVersion`／`kDesignYamlFormatVersion`，另 `kAssetsManifestVersion` 只檢查不遷移）、`MigrationTable{latest, steps, count}`（`steps[v]` = v → v+1）、`migrateWith()`、`migrate<UiProject|Flow|EditorState|Meta|DesignYaml>(json, from) → Result<int>`、`readFormatVersion()`（缺 = 1）、`migrateToLatest()`；五個序列目前皆空，框架以測試裡的假 v0→v1 驗證。逐檔：`.xtui` 缺 = 1、過新 `kUnsupported`、`migratedFrom` 記原版本，`shared`／`preview`／`publish` 也保留未知鍵；`.xtflow` 由 `FlowGraph::parse()` 自己檢查（xt_flow 不能連 xt_bundle）；`<xt version>` 由 `parseXt()` 檢查，`doc_emit` 不寫入 IR（有無此屬性 IR 相同），編輯器 `toXt()` 不主動補寫（缺 = 1 = 最新）；`.xteditor.json` 寫 `version`（舊的 `v` 讀為版本），未知鍵（頂層與 `view`／`timeline`／`vector` 內）原樣寫回，過新時不覆寫該檔；`.meta`（素材與參考）、`palette.yaml`／`tokens.yaml`／`type.yaml` 首鍵 `version`；`design.yaml` 的 `version` 是設計系統自己的版本（35 §35.2），**不**當格式版本；`contracts/`／`conventions/` 由 `conventionYamlText()` 寫回時補 `version: 1`、過新拒寫，`xtframe-pack` 讀動效規格時檢查並且不把 `version` 放進 manifest；`manifest/assets.yaml` 讀取時過新拒絕。編輯器開專案遷移後以 NoticeBar 提示一次（`EditorSession::projectMigrated`），寫回在下一次存檔。**T-410**：凍結 fixture 在 `Tests/roundtrip/<格式>/{v0,v1,latest}/`（xtui、xtflow、xteditor、meta、assets-manifest、xt、design、conventions；v0 = T-406 前無版本欄、`.xteditor.json` 為舊 `v`），`Tests/bundle/test_roundtrip.cpp` 與 `Tests/editor_service/test_service_roundtrip.cpp`（headless 開 v0／v1 專案複本 → `doc.save` + `project.set` → 與 latest 比對）的 doctest 套件 `roundtrip`（`dev_check.sh` 同名套件；`XT_ROUNDTRIP_UPDATE=1` 由 v1 重寫 latest），`design.yaml` 的格式版本 `formatVersion` 同時實作（缺 = 1、過新拒絕、寫在首鍵）。

## 43.3 行為版本 `spec.behavior`

- `<name>.xtui` 的 `spec.behavior`：整數，beta 起 = 1；缺 = 0（= beta 前的行為）。新專案寫最新；舊專案保留原值，專案面板可升級（升級前跑 §43.4 比對並顯示 diff）。
- 編譯器把它寫進 IR `META`（`behavior` 選用尾段，02 §2.10；值 0 時不寫，讓 minor ≤ 7 的讀取器仍能開；**值 ≥ 1 的檔只有 2.8 起的執行時能讀**——承諾自 2.8 起算），執行時與 flow 依它切換；bundle 內各 UI 跟專案同值。
- **旗標表**（每個整數值 = 之前全部旗標的集合；只收「曾經改變既有行為」的項目，不是功能開關）：

| 值 | 旗標 | 行為 | 來源 |
|---|---|---|---|
| 1 | `flow-signal-coalescing` | 值型文件連接點每幀合併一次推送；連續事件觸發器每幀以最後事件執行一次（27 §27.4.1） | T-389 |
| 0 | — | 逐事件同步推送（beta 前） | — |

- 規則：行為版本只在 §43.1 承諾範圍內的行為改變時 +1；每次 +1 都要有語料庫 diff、遷移說明、旗標表一列。實作上每個旗標是一個 `if (behavior >= N)`，不做旗標位元組合。
- **實作記錄（T-407）**：常數 `kBehaviorFlowSignalCoalescing = 1`、`kLatestBehavior = 1` 在 `Code/Runtime/core/include/xt/version.h`。流向：`.xtui` `spec.behavior`（`ProjectSpec::behavior`；非負整數，缺或其他型別 = 0；非 0 才寫回，寫在 `spec` 第一個鍵；`project.set` 驗證 0～最新）→ `xtframe-pack` 以 `CompileOptions::behavior` 編進每個 UI 的 IR `META.behavior`（`compileDocument`／`compileLottie`；0 不寫尾段）並寫 manifest `spec.behavior`（非 0 才寫；`BundleSpec::behavior` 讀回）→ `Document::behavior()`（= `ir().meta().behavior()`，無 META 或無尾段 = 0）。旗標 1 的三處：(b) `BindingRuntime::notify` 於 `behavior >= 1` 標 dirty，否則立即逐一求值（T-389 前）；(c) `TriggerRuntime::fire` 的連續事件（pointermove／scroll／drag.over，連同 T-394 的 pointerdrag）只在 `behavior >= 1` 保留到 `update()`；(a) flow：`coalescesSignals(state)` 以該端點 Document 的 behavior 決定值型輸出是否進批次（`load()` 文件與全域 flow 各 UI 各自判斷），`tick()` 只在有任一綁定文件 `behavior >= 1` 時整批（否則每次推送立即求值，T-389 前的 tick）。(d) `setStyle` 去重不屬旗標，兩版都生效。新專案寫最新：`createUiProject`（空白與範本）、`adoptAsUiProject`、`xtframe-import` 新建的專案；`upgradeToUiProject`、未命名專案（lone `.xt`）維持 0。編輯器：`PreviewSession::setBehavior()` 由 `EditorSession` 在切換專案（`setRootDir`）、暫存 UI 預覽、場景預覽與 Demo 的 `loadProjectRuntime` 帶入；UI Project 面板 Project 分頁加「Behavior version N」與「Upgrade to latest」（`EditorSession::upgradeProjectBehavior()` 寫回 `.xtui` 並重編）；按下先確認：說明旗標、提示「升級後請檢查動態行為」，若專案在本倉語料庫（往上找 `Tools/Scripts/corpus_check.sh`、比對 `Tests/corpus/*/project.json` 的 `source`）再列出 `bash Tools/Scripts/corpus_check.sh --project <name>`——編輯器不直接執行該腳本（需 Python 與 Release 工具、數分鐘）。測試：同一文件 behavior 0／1 的 100 次 PointerMove（flow 下游 100 次／1 次）、100 次 setPort（綁定 100 次／1 次）、100 次 on:pointermove，以及 T-389 前的斷言（pointer 埠與 format 節點不需 tick 即更新、variant class 不需 update 即切換、按下拖曳在幀內生效）作為 behavior 0 案例；T-389／T-394／T-399 依賴合併的既有案例改以 behavior 1 編譯。語料庫：四個專案的 `.xtui` 加 `"behavior": 1`；截圖、腳本紀錄與連接點表不變，`expected/manifest.json` 只多 `spec.behavior: 1`，且各 UI `.xtir` 的 `fileHash` 變（META 多 4 bytes 尾段）——非行為變更。

## 43.4 專案級語料庫（golden corpus）

- 位置 `Tests/corpus/<project>/`：`source/`（指向 `Examples/demos/<name>` 或複本）、`expected/`：每個 UI 的截圖（SW 路徑，固定視口與 dpr，固定幀序列如 0／30／120）、正規化後的 `manifest.json`（去時間戳與雜湊順序）、連接點表（`xtframe-pack --gen-ports` 的 JSON 形式：名稱、型別、方向、預設值）、行為腳本（`xtframe-view --event` 序列 + 每步截圖 + HostEvent／port 輸出紀錄）。
- 起始四個：`game-ui`（demos）、`challenge-menu`、`npdo`、`fairy-forest-menu`；外部宿主（Aux Duty）在自己倉跑同一套腳本，不收進主倉。
- 工具 `Tools/Scripts/corpus_check.sh`：全部比對，截圖容差同 golden 測試；更新需 `XT_CORPUS_UPDATE=1`，且 PR 說明必須有「行為變更：…」一段（`merge_task.sh` 檢查）。
- CI：A1 之後每個 PR 跑；是 §43.1 承諾 (2) 的執行者。
- 平台裁決（T-422，2026-10-21）：`expected/` 以 **Windows 系統字型**渲染（語料庫專案寫的是 Bahnschrift／Segoe UI／Palatino／Georgia），所以 ctest 只在 Windows 登記 `corpus_check`，且只在 Release／RelWithDebInfo（hosted runner 的 Debug 一跑超過 30 分鐘）；Linux／macOS 的渲染一致性由 `Tests/golden`（有容差）負責。`Tools/Scripts/dev_check.sh` 的 `corpus` 套件不受影響。**實作記錄（T-426）**：`project.json` 選填 `fonts`（expected 依賴的字族，四個專案各填 `.xts` `font-family` 第一順位實際解析到的字族；challenge-menu 填 `Bahnschrift`，因 `Bahnschrift SemiCondensed` 是可變字型 Bahnschrift 的具名實例、字族表沒有此名），`corpus_check.py` 開跑前以新增的 `xtframe-view --list-fonts`（`addSystemFonts()` 後字族名排序去重、一行一個）取字族清單，缺字族的專案預設 FAIL、`--skip-missing-fonts`（ctest 登記使用）時 SKIP 不計失敗，`dev_check.sh` 的 `corpus` 維持嚴格。**T-429**：同字族不同版本（Windows Server 2022 與 Windows 11 的 bahnschrift.ttf）字形邊緣不同，故 `fonts` 元素改為 `{"family", "sha256"}` 以字型檔雜湊釘住，`xtframe-view --list-fonts` 改印 `family<TAB>path`（`--list-fonts=names` 保留舊輸出），雜湊不符視同缺字型（同樣 FAIL／SKIP），雜湊由 `Tools/Scripts/corpus_fonts.py --update` 填入、`corpus_check.py --update` 會先呼叫它。
- **實作記錄（T-401，語料庫 v0）**：`Tests/corpus/<project>/` 放 `project.json`（`source` 相對倉庫根、`uis` 的 id 與視口、`dpr`、`frames`、`scripts`）、`scripts/<name>.json`（目標 UI、`printText`、每步 `frame` 與 `events`／`pointers`）與 `expected/`（`<ui>/frame-<n>.png`、`<ui>/script-<name>-step<k>.png`、`<ui>/script-<name>.log`、`manifest.json`、`ports.json`）。四個專案：game-ui（hud、pause）、fairy-forest-menu、challenge-menu、npdo（三個 UI，1920×1080 原尺寸），幀 0／30／120，SW、dpr 1，共 54 檔約 42 MB。`Tools/Scripts/corpus_check.sh`（包 `Tools/Scripts/corpus_check.py`，Python 標準庫）把來源複製到工作目錄再 `xtframe-pack --strict`（打包會寫回專案，故不在範例原地打包），每步一次 `xtframe-view --headless` 從頭跑；截圖以 golden 的 SW 規則比對（`XT_GOLDEN_CHANNEL`／`XT_GOLDEN_RATIO`），失敗產 `.diff.png`；其餘逐字。manifest 正規化：去 `builtAt` 與產品版本`bundle.xtframe`、鍵排序、含 path／id／name／file 的條目陣列排序，雜湊保留。連接點表來自新增的 `xtframe-view --dump-ports <file>`（`xtframe-pack --gen-ports` 只有 cpp／cs）；腳本紀錄取 flow 事件行、新增的 `--log-ports` 連接點事件行與 `--print-text`。CTest `corpus_check`、`dev_check.sh` 套件 `corpus`；`merge_task.sh` 在 PR 差異含 `Tests/corpus/*/expected/` 時要求 PR 說明有「行為變更：」一行。

## 43.5 IR 2.8 整理（B8）

這一週（T-380～T-399）為了不升版用了借位，全部補正並升 `minor` 為 8：

| 借位 | 位置 | 2.8 做法 |
|---|---|---|
| T-380 `BindTarget::Port = 10`、T-385 `Disabled/Display = 11/12`、T-392 `Value/BuiltinState = 13/14` | BIND | 保留值；02 §2.s 表補齊；驗證器上限更新 |
| T-381 `IrPort._pad → entryType` | PORT | 正式欄位（已是）；02 表寫明 |
| T-383 `IrPortDefault` 的 List 文字、T-385 Struct 文字 | PORT defaults | 正式：`type = List/Struct` 時 `value` 前 4 bytes 為字串表 id；02 §2.s.2 寫明 |
| T-390 `IrTrigger._pad → button`、State 動作 `0x40` 旗標、If 的 `kIfTerms` step 序列 | TRIG | 正式欄位與旗標表；02 §2.v 寫明 |
| T-386 `hit-slop` 的 em 以負偏移編進 16 bytes | STYL | 改為 `LengthUnit` 正常編碼（em／%／px 各自單位），不再偏移 |
| T-391 PORT 尾端 `nested[]` 子表 | PORT | 維持尾段，但**舊執行時必須能忽略**：缺尾段 = 無巢狀；新執行時讀舊檔 = 無巢狀。02 章寫明；T-391「新 IR 舊執行時拒收」的驗證改為容忍 |
| T-392～T-399 的 enum 尾端追加（TriggerEvent 31～41、TriggerAction 17～18、SetSource 4～8、IfTerm 1～6、StyleProp 275～276） | 各處 | 保留；02 章表補齊 |

規則（之後一律）：`ir_pod.h` 的任何版面或列舉變更 → `kIrFormatMinor` +1、02 章同一提交、`Tests/ir/fixtures/v2.<minor>.xtir` 加一個凍結檔；`Tools/Scripts/check_ir_layout.sh` 比對 POD `sizeof`／列舉上限與 02 章表（CI）。舊 `minor` 讀取：缺尾段視為空；未知區塊型別忽略（02 §2.11 既有規則）。

- **實作記錄（T-405，IR 2.8）**：`kIrFormatMinor = 8`，POD 版面一個都沒變（79 項，02 §2.12）。02 章更新 §2.1（相容規則）、§2.2（minor 歷史）、新增 §2.6.2（StyleProp 273～280 與 `hit-slop`）、§2.10（META `behavior`）、§2.s.2（List／Struct 預設值文字）、新增 §2.s.3（PORT 完整版面、`entryType`、`nested[]` 容忍、BindTarget 1～14 表、ConverterArg 8／9）、新增 §2.v.1（`IrTrigger.button`、事件 0～41、動作 0～18、State `0x40`／`0x80`、If 旗標與 `kIfTerms` step 序列、IfTerm 0～6、SetSource 0～8）、新增 §2.12（版面與上限表）。`ir_pod.h` 新增上限常數 `kBindTargetLast`（驗證器改用它）、`kTriggerEventLast`、`kTriggerActionLast`、`kStylePropLast`（後三者超出不報錯）。`hit-slop`：編譯器與執行期 `setStyle` 改寫 StyleProp 277～280 的 Length（px／%／em，驗證器拒收其他單位）；276 只在 `IrView::formatMinor() < 8` 時以偏移解碼，2.8 檔裡出現則忽略；276 仍是計算值 id。PORT `nested[]`：取消「每個 List 欄位必須有描述」的拒收；缺描述的欄位在執行期無結構，`setPort`／`setPortText` 對其值略過（同未知欄位），巢狀 `<repeat>` 仍拒收。META `behavior`：選用 u32 尾段，0 時不寫，讓 2.8 檔維持 16 bytes 的 META 而可被 minor ≤ 7 的執行時開啟（舊讀取器要求剛好 16 bytes；behavior ≥ 1 的檔會被舊執行時拒收）。凍結 fixture：`Tests/ir/fixtures/v2.{5,6,7,8}.xtir` 由同一份 `v2_source.xt` 編成——2.5／2.6／2.7 用本倉 git 歷史在 `e5fb42e1^`／`a1ef2ede^`／`1f18224a^` 建的 `xtframe-compile`，2.8 用本版；另有 `v2.7-hitslop.xtir`（舊偏移編碼，與 2.8 長手寫法 hit test 一致）與 `v2.8-tail.xtir`（含 `nested[]` 尾段）。`1f18224a^`（2.7、T-391 前）建的 `xtframe-view --headless` 開啟 `v2.8.xtir` 與 `v2.8-tail.xtir` 成功，`v2.8` 截圖與 `v2.7` 逐位元組相同。`Tools/Scripts/check_ir_layout.sh`：docs-checks CI、`dev_check.sh` 套件 `ir`；`Tests/ir/layout.txt`。語料庫：截圖、紀錄與連接點表不變；四個專案的 `expected/manifest.json` 只有各 UI 的 `.xtir` `fileHash` 變了（標頭 minor 7 → 8 改變了位元組），已更新——非行為變更。

## 43.6 契約雜湊、alias 與廢止

- **契約雜湊**（40 §40.6.1）：`manifest.json` 每個 UI 加 `contract: "<sha256 前 16>"`，雜湊自連接點表（名稱、型別、方向、預設值）＋時間軸名＋狀態名的正規化 JSON；`xtframe-pack --gen-ports` 生成物帶同一雜湊；宿主載入時可比對。
- **alias**：語言關鍵字、屬性名、觸發名、op 名、符號名、port 名一旦進 beta 就**不刪**；改名 = 新名＋舊名 alias，alias 至少保留一個產品 MINOR；`--strict` 與 lint 對 alias 發 `deprecated: use <new>` 警告；29 章 op 廢止同規則（服務握手回 `deprecated` 清單）。
- **標準符號庫**：專案內是**複製**（現況，`xtframe-pack` 缺檔才補）；升級由 `project.syncSymbols` 類 op 以 diff 方式提示，不自動覆寫使用者改過的符號。活連結不做。
- **Editor Service 握手**（B4）：`initialize` 回 `{ "version": <服務版>, "format": {"xtui": N, "ir": "2.8"}, "behavior": N, "ops": [...], "deprecated": [...] }`；客戶端（skill、MCP）以此決定可用 op；`check_skill_sync` 檢查 skill 文件的 op 清單與 `deprecated` 一致。
- **實作記錄（T-408）**：(1) `bundle::contractHash(doc)` = `contractJson(doc)`（`{"ports":[{name,type,dir,default}],"timelines":[…],"states":[…]}`，單行無空白；port 依查找名 `qualifiedName` 排序，`type` 寫完整型別如 `enum(a|b)`、`list<number>`、`struct{hp:number}`、`list<struct{…}>`，`default` 為第一次 update 前的 `Document::portText()`（struct／list／event 為空字串）；時間軸 = TGRP 群組名、狀態 = STAT 自訂狀態名，各自排序去重）的 SHA-256 前 16 位小寫 hex。pack 在 `describeUi` 的第一次 update 前計算，寫進 manifest `uis[].contract`（`fileHash` 之後；增量打包沿用舊值）與 `PackReport::portTables[].contract`；`--gen-ports` 在每個 UI 命名空間寫 `inline constexpr char kContractHash[]`（C++）、`Ports.ContractHash`（C#）。`loadUi(ctx, bundle, id, expectContract = {})`：非空且與 manifest 不同（或 manifest 沒有）→ `kIncompatible`，不載入。(2) alias 登錄表 `xt/compile/aliases.h`（`AliasKind` Trigger／Action／Property／Converter／Binding，`nameAliases()`、`findNameAlias()`）：`pointerdown`→`press`、`pointerup`→`release`、`state-enter(<Name>)`→`state(<Name>).enter`、`state-exit(<Name>)`→`state(<Name>).exit`、`bind:state.Disabled`→`bind:disabled`；動作、屬性（`.xts` 宣告）、轉換器的查表失敗時也先查登錄表（目前無項目）。alias 照新名編譯；`strict` 或新選項 `CompileOptions::reportDeprecated`（編輯器預覽開啟，`lint.run` 規則 `compile.deprecated`）時每次使用發 `deprecated: use <新名> instead of <舊名>` 警告；**此警告不使 strict 失敗**（`DiagnosticSink::deprecatedCount()` 自警告數扣除，alias 在保留期內必須能編）。非 strict 且未開 reportDeprecated 時不發，既有輸出不變。`drag.drop`→`drop` 為既有同義寫法，未列入登錄表。(3) 標準符號：UI Project 只有「複製進專案」一種模式（`xtframe-pack`／`pullStandardSymbols` 缺檔才補、`symbol.instantiate` 已存在者優先，皆不覆寫）；`xt-standard:` 仍只給未命名專案（22 §22.12），不屬 UI Project 的活連結。`Assets/Symbols/index.json` 每個符號加 `content`（檔案 LF 正規化後的 `sha256:<hex>`）與 `previousContent`（舊版雜湊，`Tools/Scripts/gen_symbols_index.py` 在檔案變更時自動追加）。`bundle::diffStandardSymbols()` 與 op `project.syncSymbols {diff?}`（Read）：`shared.symbols` 內與標準符號同檔名的複本逐一比對，`current`（同文）／`outdated`（等於某個舊版：庫有新版、使用者沒改）／`modified`（不等於任何已知版本：使用者改過），附庫→複本的 unified diff（前後 2 行；任一檔超過 4000 行不附），只提示不寫檔。(4) `OpSpec::deprecated`／`replacedBy`：只在廢止的 op 上由 `opsJson()` 寫出 `deprecated: true`、`replacedBy`（其他 schema 不變），目前沒有廢止的 op；`initialize` 的 `deprecated` 清單留給 B4。

## 43.7 凍結清單（0d；人看版在 RELEASE-MAP §7）

| 項 | 凍結範圍 | 可變方式 |
|---|---|---|
| 來源格式 | §43.2 全部檔的欄位與語意 | 加欄位 + 遷移；不刪、不改語意 |
| 語言 | `.xt` 元素與屬性、`.xts` 屬性與值集合、觸發名與動作、綁定來源與轉換器、保留名、port 型別 | 只加；改行為走 §43.3 |
| flow | 節點型別與參數、文件連接點名 | 只加；alias |
| IR | major 2；各 chunk 版面 | minor 尾端追加（§43.5） |
| bundle manifest | version 2 的欄位 | 加欄位 |
| 執行時 C++ | 13 §13.6 清單 | 尾端加、預設實作 |
| Editor Service | op 名與參數 | 加 op；廢止走 alias |
| skills | `xtui`／`xtui-editor` 的對外說明 | 跟 op 走 |

**實驗**（不在承諾內，UI 與文件標「實驗」）：32 章綁定與變形、31 章修改器的執行期部分、`layer: overlay` 的巢狀順序、3D UI 深度分層、媒體軸、多視窗的 `snapshotState` JSON 內容（版本 1 可變）。

## 43.8 beta 相容聲明（C4 的骨架）

一頁：§43.1 三條承諾、§43.7 的凍結與實驗兩表、「遇到行為差異怎麼回報」（附 `xtframe-view --version` 與專案的 `spec.behavior`）。

**記錄（T-416，2026-10-21）**：`Docs/COMPATIBILITY.md`（英文）與 `Docs/COMPATIBILITY.zh-TW.md` 已入倉並列入 `Tools/Export/runtime.lst`；編輯器以 `ExperimentalBadge`（清單 `Code/Editor/Qt/src/experimental_features.h`，`Tools/Scripts/check_experimental_sync.sh` 比對本節實驗表）標示 Rig、Modifiers（有執行期修改器時）、3D 面板與 `<video>`／`<sequence>` 的 Clip 群組，`layer: overlay` 巢狀順序與 `snapshotState` 無編輯器介面故不標；About 顯示 IR 與行為版本（最新／本專案）、相容性連結與「Copy version info」。
