# 43 · 相容性與版本（beta 前的保護機制）

> 2026-10-21 定案（規劃 session 第 0 波 0b；使用者：「需要讓使用者的工作不會因為後續改版就失效或表現不同行為」）。本章是第 2 波 B1～B8 的依據。40 §40.15 的三種版本號與相容性矩陣維持，本章把它落到檔案與程式。人看的版本在 `docs/RELEASE-MAP.md`。

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
| `design/*.yaml`、`contracts/*.yaml`、`conventions/*` | 無 | 加 `version: 1`；缺 = 1 |
| `manifest.json`（bundle） | `version: 2` | 維持；加 §43.6 契約雜湊 |

規則：
- **讀舊寫新**：每種格式一個遷移序列 `migrate<Format>(json, fromVersion) → toVersion`，放 `bundle/src/migrations/`（R 層，編輯器與 `xtframe-pack` 共用）；開啟時逐步遷移到最新並在編輯器提示「專案已從格式 N 升到 M」（一次，記在 `.xteditor.json`）；寫回永遠最新。
- **拒絕未來**：版本大於已知 → 明確錯誤，不猜。
- **未知鍵保留**：JSON／YAML 讀取遇到未知鍵不丟（`.xtui`、`.xteditor.json`、`.meta`），寫回時原樣保留，讓新舊版本交替開啟不互相破壞。
- **round-trip 測試**（B5）：每種格式每個歷史版本一個凍結 fixture，開 → 存 → 與「同內容的最新版 fixture」只差 `version` 欄。

## 43.3 行為版本 `spec.behavior`

- `<name>.xtui` 的 `spec.behavior`：整數，beta 起 = 1；缺 = 0（= beta 前的行為）。新專案寫最新；舊專案保留原值，專案面板可升級（升級前跑 §43.4 比對並顯示 diff）。
- 編譯器把它寫進 IR `META`（`behavior` 欄；02 章 2.8 一併加），執行時與 flow 依它切換；bundle 內各 UI 跟專案同值。
- **旗標表**（每個整數值 = 之前全部旗標的集合；只收「曾經改變既有行為」的項目，不是功能開關）：

| 值 | 旗標 | 行為 | 來源 |
|---|---|---|---|
| 1 | `flow-signal-coalescing` | 值型文件連接點每幀合併一次推送；連續事件觸發器每幀以最後事件執行一次（27 §27.4.1） | T-389 |
| 0 | — | 逐事件同步推送（beta 前） | — |

- 規則：行為版本只在 §43.1 承諾範圍內的行為改變時 +1；每次 +1 都要有語料庫 diff、遷移說明、旗標表一列。實作上每個旗標是一個 `if (behavior >= N)`，不做旗標位元組合。

## 43.4 專案級語料庫（golden corpus）

- 位置 `tests/corpus/<project>/`：`source/`（指向 `examples/demos/<name>` 或複本）、`expected/`：每個 UI 的截圖（SW 路徑，固定視口與 dpr，固定幀序列如 0／30／120）、正規化後的 `manifest.json`（去時間戳與雜湊順序）、連接點表（`xtframe-pack --gen-ports` 的 JSON 形式：名稱、型別、方向、預設值）、行為腳本（`xtframe-view --event` 序列 + 每步截圖 + HostEvent／port 輸出紀錄）。
- 起始四個：`game-ui`（demos）、`challenge-menu`、`npdo`、`fairy-forest-menu`；外部宿主（Aux Duty）在自己倉跑同一套腳本，不收進主倉。
- 工具 `tools/corpus_check.sh`：全部比對，截圖容差同 golden 測試；更新需 `XT_CORPUS_UPDATE=1`，且 PR 說明必須有「行為變更：…」一段（`merge_task.sh` 檢查）。
- CI：A1 之後每個 PR 跑；是 §43.1 承諾 (2) 的執行者。

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

規則（之後一律）：`ir_pod.h` 的任何版面或列舉變更 → `kIrFormatMinor` +1、02 章同一提交、`tests/ir/fixtures/v2.<minor>.xtir` 加一個凍結檔；`tools/check_ir_layout.sh` 比對 POD `sizeof`／列舉上限與 02 章表（CI）。舊 `minor` 讀取：缺尾段視為空；未知區塊型別忽略（02 §2.11 既有規則）。

## 43.6 契約雜湊、alias 與廢止

- **契約雜湊**（40 §40.6.1）：`manifest.json` 每個 UI 加 `contract: "<sha256 前 16>"`，雜湊自連接點表（名稱、型別、方向、預設值）＋時間軸名＋狀態名的正規化 JSON；`xtframe-pack --gen-ports` 生成物帶同一雜湊；宿主載入時可比對。
- **alias**：語言關鍵字、屬性名、觸發名、op 名、符號名、port 名一旦進 beta 就**不刪**；改名 = 新名＋舊名 alias，alias 至少保留一個產品 MINOR；`--strict` 與 lint 對 alias 發 `deprecated: use <new>` 警告；29 章 op 廢止同規則（服務握手回 `deprecated` 清單）。
- **標準符號庫**：專案內是**複製**（現況，`xtframe-pack` 缺檔才補）；升級由 `project.syncSymbols` 類 op 以 diff 方式提示，不自動覆寫使用者改過的符號。活連結不做。
- **Editor Service 握手**（B4）：`initialize` 回 `{ "version": <服務版>, "format": {"xtui": N, "ir": "2.8"}, "behavior": N, "ops": [...], "deprecated": [...] }`；客戶端（skill、MCP）以此決定可用 op；`check_skill_sync` 檢查 skill 文件的 op 清單與 `deprecated` 一致。

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
