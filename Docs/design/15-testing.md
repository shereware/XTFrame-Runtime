# 15 · 測試策略

測試框架：doctest（vendored）。所有測試以 `ctest` 執行。日常合併門檻是本機 `Tools/Scripts/local_ci.sh`（MSVC Debug/Release、Clang 語法檢查、WSL GCC）；GitHub Actions 矩陣（Windows MSVC、Ubuntu GCC/Clang、macOS Clang）保留為手動觸發，發佈前執行。

## 15.1 單元測試（`Tests/unit/`）

每個核心模組一個測試檔，命名 `test_<module>.cpp`。必要案例：

| 模組 | 必要案例 |
|---|---|
| easing | 5 個 CSS 關鍵字在 t=0, 0.25, 0.5, 0.75, 1 的值與瀏覽器參考值誤差 < 1e-4；steps 兩種 jump；Hold |
| track_eval | 邊界（t 小於首幀、大於末幀）、二分搜尋、快取索引、空間貝茲、各 ValueType 插值、離散退化 |
| animator | 層疊順序（transition < layer0 < layer1 < override）、同層 startClock 順序、Loop/PingPong/iterations、fill 四種、delay、負 speed、marker 觸發含回繞、finished 回呼 |
| ir_view | round-trip 位元組相同；截斷檔案、錯誤 magic、越界 offset 皆回傳 kInvalidIr 且不崩潰；未知 chunk 忽略 |
| selector_match | 每種選擇器與組合子、偽類、特異性排序、!important、inline |
| style_engine | 繼承、初始值、em 換算、transition 觸發與取代、animation 清單差異處理 |
| layout | 06.11 清單，以 JSON 快照比對 |
| text | 貪婪換行、CJK 斷行、ellipsis、text-align、line-height 基線 |
| hit_test | 旋轉元素命中、overflow hidden 裁切、z-index 順序、pointer-events none |
| events | 冒泡與 stopPropagation、hover 進出、click 需 down/up 同元素、focus/Tab 循環、checkbox 切換、wheel 滾動夾限 |
| geom | 扁平化容差、strokeToFill 對簡單線段的面積、dash 分段、trim 跨 1 回繞 |
| painter | 命令序列快照（文字化：每個命令一行）比對 |
| mini_css | 各值型別解析、錯誤輸入回傳 kInvalidArgument |
| compile | 每個解析器的正向與錯誤案例；診斷行列號正確；Lottie 每項功能至少一個最小 JSON |

## 15.2 IR round-trip

`Tests/ir/`：對每個 `tests/fixtures/*.xt` 與 `Tests/lottie/*.json` 編譯 → `IrView::open` → 以讀出資料重新 `IrWriter` → 位元組比對。此測試同時驗證讀寫器對稱。

## 15.3 黃金圖測試（`Tests/golden/`）

流程：

```
fixture (.xt 或 .json) + 參數 (width, height, dpr, time)
  → compile → Context(NullFontProvider 或 xt_text 內建測試字型) → update(time) → render → SwRenderer → PNG
  → 與 Tests/golden/expected/<name>.png 比對
```

- 比對規則：每通道差異 ≤ 2/255 的像素視為相同；差異像素比例 ≤ 0.5% 通過。失敗時輸出 `actual.png` 與 `diff.png` 到 `build/golden-out/`。
- 期望圖來源：UI 案例由 SW 光柵器首次產生後**人工檢視**確認再入庫；Lottie 案例以 lottie-web（headless Chrome，固定版本）於相同幀渲染產生，容差放寬到每通道 ≤ 8/255、比例 ≤ 2%（抗鋸齒差異）。
- 測試字型：`Tests/fonts/` 收錄一個 OFL 授權的拉丁字型與一個 CJK 子集字型（僅含測試用字），避免依賴系統字型。
- GL 後端：以離屏 FBO 渲染同一批 fixture，與 SW 輸出比對，容差每通道 ≤ 16/255、比例 ≤ 3%（MSAA 與 stencil 差異）。CI 用 Mesa llvmpipe（Linux）執行。
- 第一版最低案例數：UI 30 個、Lottie 20 個。
- 專案級黃金語料庫（整個 UI Project 打包後的截圖、manifest、連接點表與行為腳本）見 43 §43.4，位置 `Tests/corpus/`，以 `Tools/Scripts/corpus_check.sh` 比對，截圖容差同本節 SW 規則（T-401）。

## 15.4 效能與配置測試（`Tests/perf/`）

- `evaluate` 1000 Track：Debug < 1 ms、Release < 0.2 ms（單次量測取 100 次中位數；CI 只驗證 Release 且門檻放寬 3 倍）。
- 每幀零配置：以替換的 `operator new` 計數，在 `update`+`render` 穩態（第 3 幀起）配置次數為 0。案例：靜態 UI 100 元素、Lottie 50 圖層播放、10 個 transition 同時進行。
- SW 光柵：1920×1080、200 個填充路徑 < 16 ms（Release，單執行緒）。

## 15.5 模糊測試

`Tests/fuzz/fuzz_ir.cpp`：libFuzzer 目標，輸入任意位元組給 `IrView::open` + `loadDocument` + 1 幀 update/render。CI 每次跑 60 秒。Windows 不執行。

## 15.6 範例即測試

`Examples/` 每個範例在 CI 以 `--headless --frames 10` 執行確保不崩潰。

- T-179：黃金圖事件腳本新增 `string <lang> <key> <text>`、`lang <tag>`；T-176 新增 `device`；T-172 新增 `play`；T-170 新增 `theme <name> <file.xts>`。

- T-190：`dev_check.sh` 新 `lottie` 套件（`xt_lottie_tests`：38 個 Lottie fixture 原生化 vs 黑盒三時刻比對）。

## 15.7 `gl-only` fixture（T-203 #201）

黃金 manifest 的 `gl-only` 標記：`xt_golden`（SW）跳過並印 `[SKIP] gl-only`，`xt_gl_tests` 負責比對；期望圖缺少時由 GL 測試寫出並判該次失敗。用於含 `custom()` 效果的 fixture。

## 15.6 審查修正鏈後的測試閘門（2026-10-07 審查修正鏈合併；T-267a #300、T-267b #324、T-267c #322、T-267d #309、T-263f #303）

- **golden**：SW 對 SW 的比對為嚴格（色版差 ≤ 2／255、超出像素比 0），取代 §15.3 的「ratio ≤ 0.5 %」；lottie-web 與 GL 的比對維持 §15.3 容許度。可逐次調整：`XT_GOLDEN_RATIO`、`XT_GOLDEN_CHANNEL`（預設嚴格）；每張失敗的圖印 `<n> px differ, max channel diff <d>`，`XT_GOLDEN_VERBOSE` 對有差異但通過的圖也印。`lottie_ellipse_stroke`／`lottie_trim`／`lottie_gradient_stroke` 自 211fd93e（T-236 斜接上限修正）起過期，合併時重新產生。GPU 快照從不自動寫入：缺少即失敗，除非 `XT_GPU_GOLDEN_UPDATE=1`；不符時寫 `<name>.actual.txt`。畸形的 golden manifest 行即失敗。
- **GL**：`dev_check.sh` 匯出 `XT_REQUIRE_GL=1`，沒有 context 即失敗（`XT_ALLOW_NO_GL=1` 可改回跳過）；編輯器的 GL 預覽迴歸為 `gl` doctest 套件（`xt_editor_qt_gl_tests`、dev_check 套件 `editor_qt_gl`，無 offscreen）。
- **套件拆分**：`editor_qt` 排除 `gl` 與 `slow`；`editor_qt_slow`（`xt_editor_qt_slow_tests`：SW 預覽效能 smoke 等）；新增 `perf`（`xt_perf`：零配置斷言在 Debug 與 Release 皆成立，三個時間門檻只在 Release 斷言）；`layering`（`Tools/Scripts/check_layering.sh`：規則 1 include、2 link、3 `Code/Runtime/core/` 無 `<filesystem>`、4 每個頂層原始碼目錄都在 R／E／NEUTRAL 清單內，`tools/<name>/`、`platforms/<name>/` 在 R 或 E）；`ctest` 只跑命令列測試（1800 s 上限，建置它要跑的執行檔）。
- **隔離**：每個測試行程的暫存輸出在 `xt::test::tempDir()`／`tempRoot()`（`Tests/common/test_temp.h`）之下，通過時刪除、失敗時保留並印路徑；編輯器測試以 `QStandardPaths` 測試模式與暫存 INI `QSettings` 執行，不碰開發者的設定；dev_check 在建置樹取鎖（第二個執行者以 3 結束並印持有者）。
- **看門狗**：每個測試案例逾時即印 `TEST TIMEOUT: still running after N s: file(line): name` 並以 124 結束（`XT_TEST_CASE_TIMEOUT`，預設 600 s），所有套件啟用。
- **命令列測試**以 `xt_add_expect_exit_test(name CODE n [OUTPUT re] [REJECT re])` 檢查精確結束碼。
- **計時斷言**：Debug 不跑牆鐘斷言；反二次方守衛以 N 對 2N 的比值（或可計數時以配置／求值次數）斷言；其餘時間上限只在 Release 且放寬 10 倍。
- **dev_check 輸出**：逐一列出失敗案例與第一個失敗檢查；`may_fail` 案例另列為已知問題；零案例或非零結束碼即失敗。
- **IR 往返**（§15.2）：`xt_tests` 的「ir corpus: compile, read through IrView, rewrite, compare bytes」對每個語料樣本編譯、驗證、經公開讀取器與寫入器重建，須逐 byte 相同；已知不對稱者列於 `kKnownAsymmetric`（目前無）。
- **fuzz**（§15.5）：11 個 libFuzzer 目標（ir、xt、lottie、xtcl、xts、svg、xtfx、yaml、image、bundle_json、import；多格式目標以第一個 byte 選擇）；語料在建置時產生，缺任何 IR chunk 類型即建置失敗；已修的發現是每次執行前重放的迴歸檔，未修的放 `findings/` 並從 smoke 清單移除；`--fuzz` 每目標 60 s；完整閘門在 WSL GCC 與 MSVC SW shell 測試之後跑 20 s smoke。Editor Service 的 JSON 不 fuzz（需 Qt）。
- `local_ci.sh`：clang 階段加 `-Itests/common` 與兩個 define；`Code/Editor/DevTools/` 歸 E，`Code/Tools/Common/` 歸 R。

## 15.8 CI 閘門（T-402，44 §44.5.1 P0）

`.github/workflows/ci.yml` 在每個到 main 的 PR 與 main 的 push 上跑（另可 `workflow_dispatch`）；第一個 job `changes` 以 `git diff --name-only`（PR：base..head；push：before..after）判斷：改動全在 `Docs/**` 或 `*.md` 時輸出 `code=false`，`windows`／`linux`／`sanitizers`／`runtime-export` 仍執行並成功，但每個步驟以 `if:` 跳過（整個 job 被跳過時必要檢查不會回報）；手動執行、新分支或基準不明時一律 `code=true`。`docs-checks.yml` 不設路徑過濾，每個 PR／push 都跑。合併閘門＝下表「必要」各 job 全過（branch protection 依名稱勾選）。

| job | 內容 | 必要 |
|---|---|---|
| `windows` | MSVC `vs2022` Debug＋Release 建置與 ctest | 必要 |
| `linux`（`g++-12`、`clang` 18 兩條腿） | `ninja-ci-debug`／`ninja-ci-release` 建置與 `xvfb-run` ctest | 必要 |
| `sanitizers` | clang Debug、`XT_SANITIZE=address,undefined`（preset `ninja-ci-asan`，GL／shell／編輯器關）；整棵樹的 ctest（xt_tests、flow、bundle、golden 容差不變、CLI 測試），`ASAN_OPTIONS=detect_leaks=1`、`UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`；標 `XT_SKIP_UNDER_ASAN` 的測試由 test preset 排除 | 必要 |
| `runtime-export` | `Tools/Scripts/export_runtime.sh --verify`：R 層匯出獨立建置與 ctest（T-400） | 必要 |
| `docs-checks` | `check_skill_sync`、`check_ops_doc`、`check_command_ops`、`check_symbols_index`、`check_test_temp`、`check_layering`、`check_public_headers`（無建置） | 必要 |
| `macos` | `ninja-ci-*` 建置與 ctest；`continue-on-error` | 非必要 |
| `fuzz` | §15.5 的 libFuzzer 60 s；只在 `workflow_dispatch` 跑 | 非必要 |
| （M34 T-441）`xt_wgpu_tests` | 併入 `windows`（D3D12，hosted runner `XT_WGPU_FALLBACK=1` 走 WARP）、`linux`（lavapipe）、`macos`（Metal）各腿；無 adapter 時 SKIP 回 0；`sanitizers` 腿不建 wgpu（10 §10.10.3） | 隨所屬腿 |

- `linux` 與 `sanitizers` 的 `runs-on` 讀 repository variable `XT_LINUX_RUNNER`（JSON 標籤陣列，例如 `["self-hosted","xtframe-linux"]`），未設即 `ubuntu-24.04`；self-hosted 時跳過 apt 安裝步驟。登錄步驟見 `Docs/ci/self-hosted-runner.md`。
- `XT_SANITIZE`（頂層 CMake 快取字串，逗號清單）把 `-fsanitize=<清單> -fno-sanitize=vptr -fno-omit-frame-pointer` 加到所有目標（含 FetchContent 相依）；只限 GCC／Clang，與 `XT_BUILD_FUZZ` 互斥。vptr 檢查關閉的理由同 §15.5（xt_core 無 RTTI）。
- `Tools/Scripts/local_ci.sh` 仍是本機閘門；CI 閘門不取代它。

## 15.9 閘門分級（使用者 2026-10-24 裁定：「只在打版本標時才跑完整閘門」）

- **每張任務單**：代理跑 `Tools/Scripts/dev_check.sh <targets> -- <suites>`（增量建置＋受影響套件＋headers／layering／shape），`merge_task.sh` 的 light gate 再做一次 rebase 檢查後合併。**不跑** `local_ci.sh`。
- **版號單**（RELEASE-MAP §4 第 1 步，T-205／T-238／T-460 這種）：合併前跑一次完整 `local_ci.sh --clean`（MSVC Debug／Release、Qt 編輯器、clang 全檔語法、Qt 5.12 標頭語法、WSL GCC、wgpu D3D12／Vulkan、fuzz smoke、SW shell），全綠才打 tag。
- **例外**：改動建置系統本身、搬目錄（B18 類）、換工具鏈（T-442 類）的單，也跑一次完整閘門。
- PR 觸發的 CI（§15.8）待 self-hosted runner 登錄後接手「每張單」那層；在那之前本機就是閘門。
- 後果：版號之間可能累積幾張只過 dev_check 的單；版號單的完整閘門抓到的問題開修正單（T-457 的模式）。
- T-470：Qt 5.12 標頭語法檢查抽成 `Tools/Scripts/qt512_syntax_check.sh`（`local_ci.sh` 以 `--all` 呼叫）；`dev_check.sh` 的 `qt512` 套件只查相對 `origin/main` 改動的 `Code/Editor/**/*.cpp`，要求 `editor_qt` 或目標 `xtframe-editor` 時自動帶入（`--no-qt512` 關閉），`qt512-all` 查全部。
