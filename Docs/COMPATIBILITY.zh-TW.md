# XTFrame 相容性聲明（beta）

第 1 版（2026-10-21），自第一個 beta 版起適用。英文版：[`COMPATIBILITY.md`](COMPATIBILITY.md)。規範來源是
`Docs/design/43-compatibility-and-versions.md`（§43.1、§43.3、§43.7）；兩者不一致時以 43 章為準，並修正本頁。

本頁說明 beta 期間更新 XTFrame 後什麼保證繼續可用、什麼還可能改變，以及遇到差異時怎麼回報。

## 1. 什麼是您的作品

您的作品是**來源檔**：`.xt`、`.xts`、`.xtflow`；專案檔 `<name>.xtui` 與其 parts（`shared/`、`design/`、`.meta`、`manifest/assets.yaml`、`contracts/`、`conventions/`、`AGENTS.md`）；`.xteditor.json`；專案符號；匯入的 Claude Design 畫板。

bundle 與 `.xtir` 是**衍生物**：編輯器永遠能由來源重新編出。

## 2. 三條承諾

自第一個 beta 版起：

1. **beta 起任何版本寫出的來源檔，之後所有版本都能開啟**、自動遷移到最新格式，並能寫回。
2. **開啟後的行為與寫出時相同。**改變既有行為只能以修正 bug 為由，且必須先通過專案語料庫比對（diff 經審閱核准）並記入遷移說明。刻意的行為改變以專案的行為版本把關（第 6 節）。
3. **bundle 在宿主中繼續可用。**beta 期間 IR `major` 版本不變；`minor` 只在尾端追加；執行時能讀它發佈以來的所有 `minor`。

## 3. 不承諾的範圍

- 標為**實驗**的功能（第 5 節；編輯器與本頁都會標示）。
- Pro 與 AI 功能。
- 編輯器的視窗版面與儲存的設定（QSettings）。

## 4. beta 期間凍結的項目

| 項目 | 凍結範圍 | 可變方式 |
|---|---|---|
| 來源格式 | 第 1 節所列全部來源檔的欄位與語意 | 加欄位並附遷移；不刪除、不改語意 |
| 語言 | `.xt` 元素與屬性、`.xts` 屬性與值集合、觸發名與動作、綁定來源與轉換器、保留名、port 型別 | 只增加；改變行為走行為版本（第 6 節） |
| flow | 節點型別與參數、文件連接點名稱 | 只增加；改名保留 alias |
| IR | major 版本 2；各 chunk 的版面 | `minor` 在尾端追加 |
| bundle manifest | manifest 第 2 版的欄位 | 加欄位 |
| 執行時 C++ API | `Docs/design/13-*.md` §13.6 的清單 | 在尾端增加，並附預設實作 |
| Editor Service | op 名稱與參數 | 增加 op；廢止的名稱以 alias 繼續可用 |
| skills | `xtui`／`xtui-editor` 兩個 skill 對外的說明 | 跟著 op 走 |

## 5. 實驗功能

以下功能不在上述承諾內。使用它們的檔案仍可開啟，但格式與行為可能在 beta 版本之間改變。編輯器在操作這些功能的地方顯示「**實驗**」標籤；點標籤會開啟本頁。

「實驗項」欄照錄 43 §43.7 原文（`Tools/Scripts/check_experimental_sync.sh` 逐字比對，並比對 id 與 `Code/Editor/Qt/src/experimental_features.h`、英文版的表）。

| id | 實驗項（43 §43.7 原文） | 編輯器標示位置 |
|---|---|---|
| `rigging` | 32 章綁定與變形 | 屬性面板的「Rig」群組 |
| `modifier-runtime` | 31 章修改器的執行期部分 | 屬性面板的「Modifiers」群組（堆疊中有執行期修改器時） |
| `overlay-nesting` | `layer: overlay` 的巢狀順序 | 不標示（單一屬性值） |
| `depth-layering-3d` | 3D UI 深度分層 | 3D 面板 |
| `media-axis` | 媒體軸 | `<video>`／`<sequence>` 的屬性面板「Clip」群組 |
| `window-snapshot` | 多視窗的 `snapshotState` JSON 內容（版本 1 可變） | 不標示（宿主 API，編輯器無此設定） |
| `wgpu-preview` | 編輯器的 WebGPU 預覽 | 偏好設定的「預覽」頁 |

說明：執行期修改器指編輯時未烘焙的修改器（31 章）；媒體軸指影片與圖片序列片段及其與時間軸的同步；多視窗快照即 `Document::snapshotState()` 的 JSON。

## 6. 行為版本（`spec.behavior`）

UI Project 在 `<name>.xtui` 的 `spec.behavior`（整數）記錄它製作時的行為。第一個 beta 寫 1；沒有這個值代表 0，也就是 beta 前的行為。新專案寫最新值；既有專案保留原值，直到您升級（UI Project 面板 Project 分頁的「Upgrade to latest」，按下會先說明改變內容）。編輯器的「關於」視窗顯示本版本知道的最新值與目前開啟專案的值。

編譯器把這個值寫進每個 UI 的 IR（`META` chunk 的選用尾段）。值為 0 時不寫，讓舊的執行時仍能開啟；**值 ≥ 1 的檔案需要能讀 IR 2.8 以上的執行時**，因此行為版本的承諾自 IR 2.8 起算。bundle 內每個 UI 都跟專案同值。

每個值包含它之前的所有旗標。本表只收「改變既有行為」的項目，不收新功能：

| 值 | 旗標 | 行為 | 來源 |
|---|---|---|---|
| 1 | `flow-signal-coalescing` | 值型文件連接點每幀合併推送一次；連續事件觸發器（指標移動、捲動、拖曳經過）每幀以最後一個事件執行一次 | T-389 |
| 0 | — | 每個事件逐一同步推送（beta 前） | — |

行為版本只在承諾範圍內的行為改變時 +1；每次 +1 都附語料庫 diff、遷移說明與本表一列。

## 7. 回報行為差異

更新後檔案的外觀或行為有差異時，請附上：

1. **版本**：編輯器的 Help > About XTFrame >「Copy version info」（產品、Qt、IR 格式與行為版本）；執行時檢視器則附 `xtframe-view --version` 的輸出（其中 `renderers:` 一行列出編進來的 renderer，例如 `renderers: sw gl wgpu`），並註明執行時用的 `--renderer`。
2. **專案的行為版本**：`<name>.xtui` 的 `spec.behavior`（或 UI Project 面板 Project 分頁的「Behavior version」一行），並說明是否升級過。
3. **最後一個表現正常的版本**（若知道）。
4. **能重現差異的最小專案或檔案**，以及您預期看到的結果。

實驗功能（第 5 節）的差異歡迎回饋，但不屬於相容性 bug。
