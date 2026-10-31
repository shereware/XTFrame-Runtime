# 14 · 程式碼規範

本章為實作者（含 AI 子代理）的硬性規則。違反即退件。

## 14.1 語言與工具

- C++20，`-std=c++20`；MSVC 19.3x、Clang 18+（搭 libstdc++ 14）、GCC 12+ 皆須通過。
- 警告即錯誤：MSVC `/W4 /WX`，Clang/GCC `-Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion`。
- `xt_core`：`-fno-exceptions -fno-rtti`（MSVC `/EHs-c- /GR-`）。其他模組允許例外但公開介面不得拋出。
- 指定初始化子（`T{.a = 1}`）省略尾端成員時 GCC 12 會報 `-Wmissing-field-initializers`（Clang/MSVC 不會）：程式碼中一律完整列出成員，或改用建構函式；測試檔可在檔首以 `#if defined(__GNUC__) && !defined(__clang__)` 包住 `#pragma GCC diagnostic ignored`。
- 不得 include `<format>`、`<print>`（格式化用 `snprintf` 或 `std::to_string`）。`<chrono>` 可用（`perf.h`、gl、shell 已依賴）；它在 libstdc++ 14 會拉進 `<format>`，Clang 15 編不過，所以 CI 的 clang 腿用發行版的 `clang`（24.04 為 18）而不是 clang-15（2026-10-21 runtime 鏡像倉 CI 首跑的教訓）。
- `.clang-format` 於 repo 根：`BasedOnStyle: Google`，`IndentWidth: 4`，`ColumnLimit: 110`，`DerivePointerAlignment: false`，`PointerAlignment: Left`。提交前必須格式化。
- CMake：每個模組一個 `CMakeLists.txt`，目標名 `xt_core` 等，`target_include_directories(PUBLIC include)`。頂層選項 `XT_BUILD_GL`、`XT_BUILD_SHELL`、`XT_BUILD_TOOLS`、`XT_BUILD_TESTS`，預設 ON。
- 第三方：`Code/SDKs/` 內 vendored 單一標頭（doctest 2.4.11、stb_image / stb_image_write、nlohmann/json 3.11.3、bc7enc/bc7decomp/rgbcx（MIT／公有，`xt_bcn`，僅貼圖壓縮與 DDS 解碼）），每個附 `VERSION` 檔記錄來源 URL 與版本；SheenBidi（Apache-2.0，附 NOTICE）與 libunibreak（zlib）以 vendored 原始碼放 `Code/SDKs/`（僅 `xt_text` 使用）；FreeType/HarfBuzz/SDL2 以 `FetchContent` 固定版本標籤；cpp-httplib（MIT，單標頭，僅 `xt_editor_service` 的 MCP 伺服器使用）以 `FetchContent` + SHA-256 固定（`Tools/CMake/xt_httplib.cmake`）；**miniz 3.0.2**（MIT）以 `FetchContent` 固定版本＋SHA-256 取得（`Tools/CMake/xt_miniz.cmake`、`Code/SDKs/versions.cmake`）；`xt_history` 的 deflate（T-307）與 `xt_bundle` 的 `.xtsymbols`／`.xtb` zip 讀寫（store 模式，49 §49.4、22 §22.5）共用這一份（2026-10-31 審查 §7.2：T-500 曾改為 `Code/SDKs/miniz/` 的 `miniz.c`＋`miniz.h` 雙檔 vendored，不合「vendored 單一標頭或 FetchContent 固定版本」，退回 FetchContent；程式碼改回是後續任務）。**不得**依賴系統安裝的函式庫，不用 glad。
- **網路與 TLS（51 章，2026-10-27）**：編輯器層的 HTTPS 只經 `xt_net`（`Code/Editor/Net/`），實作為**作業系統原生**堆疊——Windows `winhttp.dll`、macOS `NSURLSession`（Foundation／Security，與 `dbghelp`、`ws2_32` 同屬 OS 元件，不算第三方）；Linux 是唯一例外：**執行期 `dlopen`** 發行版的 `libssl.so.3`／`libcrypto.so.3`（不是建置相依、不連結、找不到即功能退化並說明），這是「不得依賴系統安裝的函式庫」在 Linux TLS 上的明文例外。倉庫**不得**引入任何 TLS 函式庫（OpenSSL、mbedTLS、wolfSSL、BoringSSL…）作為 vendored 或 FetchContent；R 層（`Code/Runtime/`）不得做網路。

## 14.2 命名

| 項目 | 規則 | 例 |
|---|---|---|
| 命名空間 | 小寫 | `xt`, `xt::detail`, `xt::compile` |
| 型別 | PascalCase | `NodeArena`, `CommandList` |
| 函式、方法 | camelCase | `setProperty`, `flattenPath` |
| 變數、參數 | camelCase | `nodeId` |
| 成員變數 | camelCase，無前綴（struct）；`m_` 前綴（class 私有） | `m_nodes` |
| 常數、enum 值 | `k` 前綴 PascalCase（常數）；enum class 值 PascalCase | `kInvalidNode`, `NodeKind::Path` |
| 巨集 | `XT_` 前綴 | `XT_ASSERT` |
| 檔案 | snake_case | `node_arena.h`, `path_trim.cpp` |

標頭用 `#pragma once`。公開標頭在 `include/xt/`，內部標頭在各模組 `src/`。

## 14.3 錯誤處理

```cpp
enum class Status : uint8_t {
    kOk = 0, kInvalidArgument, kInvalidIr, kNotFound, kOutOfRange, kUnsupported, kIoError, kInternal
};
template <class T> class Result {   // 不用 std::expected 以維持 MSVC 相容與零相依
public:
    static Result ok(T); static Result error(Status);
    bool ok() const; Status status() const; T& value(); const T& value() const;   // value() 在非 ok 時 XT_ASSERT
};
```

- 公開函式失敗回傳 `Status`/`Result`，不用例外、不用 `errno`、不印訊息。
- `XT_ASSERT(cond)`：Debug 中止並印檔案行號；Release 為 no-op。只在 `XT_ASSERT` 中使用的常數或變數必須標 `[[maybe_unused]]`，否則 Release 建置在 Clang 下觸發 `-Wunused-const-variable`。只用於「程式邏輯不可能」的情況，**不**用於驗證外部輸入（IR、使用者 API 參數）。
- 診斷訊息（編譯器）走 `Diagnostic` 結構，不用 `std::cerr`。

## 14.4 記憶體與所有權

- 禁止裸 `new/delete`。容器用 `std::vector`；需要多型所有權時用 `std::unique_ptr`。核心無 `std::shared_ptr`。
- 非擁有參照用 `std::span`、`std::string_view` 或裸指標（裸指標一律代表「非擁有、可為 null」）。
- 每幀路徑上不得配置（01.7）；測試 15.4 以自訂 allocator 計數驗證。
- 不用 `std::map`/`std::unordered_map` 於每幀路徑；查表用排序 vector + 二分搜尋或直接索引。

## 14.5 檔案組織範例（`xt_core`）

```
Code/Runtime/core/
├── include/xt/
│   ├── xt.h                 // 匯總
│   ├── types.h              // 03.1 基本型別、Status、Result
│   ├── context.h  document.h  element.h  command_list.h  font_provider.h  asset_resolver.h  input.h  value.h  version.h
└── src/
    ├── ir_view.cpp          // 02
    ├── node_arena.cpp       // 01.3, 03
    ├── property_access.cpp  // 03.11
    ├── easing.cpp  track_eval.cpp  animator.cpp   // 04
    ├── style_engine.cpp  selector_match.cpp       // 05
    ├── layout_block.cpp  layout_flex.cpp  layout_text.cpp  layout_absolute.cpp   // 06
    ├── event_dispatcher.cpp  hit_test.cpp         // 08
    ├── painter.cpp          // 09.3
    ├── geom_flatten.cpp  geom_stroke.cpp  geom_dash.cpp  path_trim.cpp   // 10.1, 03.8
    ├── command_simplifier.cpp  baked_player.cpp
    └── mini_css.cpp         // 13.3 setStyle 用
```

**檔案長度（2026-10-21，B17 C3／T-415）**：新檔案 ≤ 1,500 行；`Tests/` 底下的測試檔 ≤ 2,500 行。既有超過者列在豁免清單 `Tools/Scripts/file_length_allow.txt`（一行一個路徑），只減不增，拆檔後移除該行。`Tools/Scripts/check_file_length.sh` 檢查（`dev_check.sh` 套件 `shape`），清單外的超長檔即失敗；清單內已降到上限以下的項目只警告（過期項目）。不計第三方、建置目錄與生成檔。重複程式碼：`Tools/Scripts/dupcheck.py` 的組數記於 `Tools/Scripts/dupcheck_baseline.txt`，`Tools/Scripts/check_dupcheck.sh` 只在組數增加時警告、不擋；覆蓋率（`coverage.sh`）不進閘門。

## 14.6 註解與文件

- 每個公開標頭頂部一段用途說明；每個公開函式一行 `///` 摘要，參數語意若不明顯再補。
- 語言：程式碼、註解、識別字、提交訊息、README、API 文件、CLI 與診斷訊息一律**英文**。設計文件與任務單為繁體中文（台灣）。
- 演算法實作處註明對應設計文件章節，例如 `// 見 Docs/design/06-layout.md §6.9 步驟 4`。
- 不留 TODO；未實作的分支寫 `XT_ASSERT(false && "unsupported: ...")` 並在 16 章任務清單登記。

## 14.7 提交

- Conventional prefix：`feat:`、`fix:`、`docs:`、`refactor:`、`test:`、`build:`。
- 一個提交一個任務（16 章的任務編號寫進訊息，例如 `feat(core): T-04 track evaluation`）。
- 提交前：全部測試通過、clang-format 無差異。合併門檻為 `Tools/Scripts/local_ci.sh`（MSVC Debug/Release + ctest、Clang 全源碼語法檢查、WSL GCC 建置）；GitHub Actions 只在發佈前手動觸發（私人倉庫的分鐘數計費）。

備註（2026-09-27）：`Tools/Scripts/dev_check.sh` 沒有 `compile` 套件，編譯器測試在 `core` 內；任務單請寫 `core`。

## 14.8 對 AI 子代理的額外規則

- 只實作指派的任務；不「順便」改動其他檔案。
- 規格未涵蓋的情況：在回報中列出，選擇最保守的行為（回傳 `kUnsupported` 或 `XT_ASSERT`），不自行擴充設計。
- 不引入未列於 14.1 的第三方相依。
- 不改動 `Docs/design/`。
- 回報格式：實作的檔案清單、測試結果原文、未涵蓋情況清單。

## 14.9 外部輸入規則（2026-10-02 全倉審查後定案）

第一次全倉審查（`Docs/reviews/2026-10-02-full-audit.md`）的多數 Critical／High 屬同幾類，定為規則：

1. **外部數字先過 `num_guard.h`**：來自檔案、綁定、宿主 API、服務參數的數字，在成為迴圈次數、配置大小、陣列索引或 float→int 轉換之前，必須經過 `finiteOr`／`clampFinite`／`to*Sat`。上限用具名常數，同一個上限只存在一處（執行期與編譯器共用）。
2. **遞迴下降必須有停止下降的深度上限**：只回報、繼續遞迴不算。匯入器的上限以 Debug、1 MB 堆疊實測決定（24 §24.12）。
3. **驗證端與使用端都要防守**：`IrView::validate` 拒絕壞檔，但執行期 API 能餵進同樣的值時，使用點也要夾限。
4. **不信任的 JSON 只用型別檢查過的存取函式**（`Code/Editor/Import/src/untrusted_input.h` 的 `member`／`str`／`flt`…）；`value()`／`get<T>()` 會丟例外。允許例外的層（E）的每個公開進入點以 `catchAll` 包住，沒有例外穿出公開介面（§14.3）。
5. **資料裡的路徑先 `resolveInside()`**：匯入資料（board、Figma dump、Lottie）給的路徑只在確認留在根目錄內之後才讀寫。
6. **callback 期間不改正在走訪的容器**：見 08／13 章的重入規則（T-260a 併入後補記）。
7. **換文件只有一個重設點、一個宣告點**：見 17 §17.22。

## 14.10 數字文字與路徑（2026-10-07 審查修正鏈合併；T-268 #323、T-261d～f）

- 數字文字不依賴行程 locale：解析走 `xt::fromChars`／`css_number::read`，輸出走 `xt::formatFloat`（`%g`）或 `xt::formatFixed`（`%.<n>f`）；進入檔案、IR、JSON 的文字不用 `strtof` 家族與 printf 家族的浮點轉換；每個 Qt 程式在建立 application 物件後把 `LC_NUMERIC` 固定為 "C"。
- 浮點運算不得 contraction（2026-10-29，T-529 之後的裁決）：全樹 `-ffp-contract=off`（頂層 CMake；MSVC 預設 `/fp:precise` 本來就不融合），因為 GCC／Clang 在基線就有 FMA 的目標（AArch64：Apple silicon、Android arm64）預設把 `a*b+c` 融合成一次捨入，SW 光柵的像素會與 x86 差一個位元；渲染輸出要在所有平台位元組相同（23 §23.5）。不得在任何目標加 `-ffast-math`／`/fp:fast`／`-mfma`。
- 路徑字串一律 UTF-8；`std::filesystem::path` 只經 `compile::pathFromUtf8`／`pathToUtf8`／`genericPathToUtf8` 建立與轉回；`Code/Runtime/core/` 不得 include `<filesystem>`（`Tools/Scripts/check_layering.sh` 規則 3）；測試不得直接呼叫 `temp_directory_path()`（`Tools/Scripts/check_test_temp.sh`）。
- CSS 值解析只有 core 一份：`Code/Runtime/core/src/css_values.{h,cpp}` 由執行期 `mini_css` 與編譯器 `xts_values*` 共用，兩邊只加各自的屬性表、診斷與 `.xts` 專屬語法，不再各寫一份（T-413）。
