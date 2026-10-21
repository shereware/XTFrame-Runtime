# 22 · Bundle：一款遊戲的全部 UI

版本：0.3。狀態：設計定案並實作完成（M11，T-90～T-96，2026-09-22）。實作時的偏差與後續缺口見 16 章 M11 段。

目的：把「一款遊戲的所有 UI」當成一個東西來編輯、發佈、瀏覽、載入。動機與定案見 16 章 M10 末段的目標情境：來源形態與發佈形態是同一個結構；發佈時每個 UI 各自一個 `.xtir`，共用資產去重，另有 manifest；宿主（引擎）與唯讀瀏覽器只看 manifest 就能列出、拖曳、以穩定 id 綁定，不需理解文件內部。

不在本章：宿主端的貼圖輸出與 dirty rects（13 章修訂，M12）、各宿主插件（各自的倉庫）。

## 22.1 兩個形態

| | 來源形態（source） | 發佈形態（published） |
|---|---|---|
| 是什麼 | 一個目錄，內含 `bundle.xtproj` 與各 UI 的 `.xt/.xts/.xtflow` 及共用資產原檔 | 同一目錄結構經 `xtframe-pack` 產出：`manifest.json`、每個 UI 一個 `.xtir`、去重後的資產、縮圖 |
| 誰用 | 編輯器、版本控制 | 宿主（`Context::loadBundle`）、瀏覽器 `xtframe-browse`、宿主插件的資產匯入 |
| 可否手改 | 可 | 不建議；重新 pack 即覆蓋 |
| 容器 | 目錄 | 目錄或 zip（`.xtb`，store 模式不壓縮，讓宿主可 mmap；zip 只是搬運方便，載入前解開或由 `BundleReader` 直讀） |

`bundle.xtproj` 取代 17.8 的專案檔定義（17.8 改為指向本章；單一 `.xt` 直接開啟 = 隱含只有一個 UI 的匿名 bundle，行為不變）。

## 22.2 來源形態：`bundle.xtproj`

```json
{
  "version": 2,
  "name": "MyGame UI",
  "id": "mygame",                       // bundle 穩定 id：小寫英數與 '-'，宿主用它區分多個 bundle
  "uis": [
    { "id": "hud",   "doc": "hud/hud.xt",   "flow": "hud/hud.xtflow", "size": [1280, 720], "tags": ["ingame"] },
    { "id": "pause", "doc": "menus/pause.xt", "size": [1280, 720] }
  ],
  "shared": { "assetRoot": "shared", "fontDirs": ["shared/fonts"], "styles": ["shared/base.xts"], "templateDirs": ["shared/fx"] },
  "preview": { "dpr": 1 },
  "target": "engine",
  "publish": { "embedImages": false, "embedFonts": false, "thumbnails": [320, 180] }
}
```

- `uis[].id` 是**穩定 id**：改檔名不改 id；manifest、宿主程式、流圖節點都只認 id。編輯器新增 UI 時由檔名產生，之後可改但會提示「宿主可能引用」。
- `doc` 相對於 bundle 根；`.xts` 由 `.xt` 內的 `<link>` 決定，不在這裡列。`shared.styles` 是每個 UI **隱含**先載入的樣式表（等同在每個 `.xt` 前面加 `<link>`），共用主題放這裡。
- 資產路徑解析順序：UI 自己的目錄 → `shared.assetRoot` → bundle 根。同名以近者為準；pack 時對同內容去重（見 22.3）。**T-138（2026-09-24 實戰發現）**：這條同時約束編譯期與執行期——`CompileOptions.searchDirs`（依序試的目錄清單；空時退回 `rootDir`）供 `compileDocumentFromStrings` 解析 `<img src>`/`<anim src>`/`@font-face src`，`FileAssetResolver` 接受多個根目錄依序試。來源形態 bundle 內的 `.xt` 一律寫相對於自己的路徑（匯入器的 `../shared/img/x`），三個目錄任一命中即可；編輯器與 `xtframe-view` 開 bundle 時必須用這個順序，否則 SVG 圖是編譯錯誤、整個 UI 開不了。
- `target: engine` 沿 17.8 語意（效果、3D、文字的「宿主需支援」提示）。
- `size` 是設計視口，也是縮圖與瀏覽器顯示的比例；宿主可以另給視口，佈局照 06 章。

## 22.3 發佈形態

```
<bundle>/
  manifest.json
  ui/<id>.xtir            // 每個 UI 一個；ASST 表指向 assets/ 的相對名稱（不內嵌，除非 publish.embed*）
  ui/<id>.xtflow          // 有流圖者一併帶出（宿主可選擇執行 xt_flow，或無視）
  assets/<hash16>.<ext>   // 去重後的圖片/字型/著色器；檔名 = 內容 BLAKE2/FNV-1a 64 位元的 16 進位 + 原副檔名
  thumbs/<id>.png         // publish.thumbnails 尺寸；由 SW 後端在 pack 時渲染第 0 幀
```

- **去重**：pack 時把每個 UI 引用到的資產依內容雜湊改名進 `assets/`，`.xtir` 的 `ASST.name` 改寫為 `assets/<hash>.<ext>`；同一張圖被三個 UI 用只存一份。原始邏輯名稱保留在 manifest 的 `assets` 表（除錯與瀏覽器顯示用）。
- **不合成單檔**（16 章已定案）：增量更新只換一個 `.xtir`、熱重載按 UI、依需求載入。
- `.xtir` 格式不動（02 章）；只是 `ASST` 一律外部引用。bundle/UI 對應只記在 manifest（T-91 時決定不動 IR）。

### manifest.json

```json
{
  "version": 1,
  "bundle": { "id": "mygame", "name": "MyGame UI", "builtAt": "2026-09-22T10:00:00Z", "xtframe": "0.3.0", "irMajor": 2 },
  "uis": [
    {
      "id": "hud", "name": "HUD", "file": "ui/hud.xtir", "flow": "ui/hud.xtflow", "thumb": "thumbs/hud.png",
      "size": [1280, 720], "tags": ["ingame"],
      "requires": { "effects": true, "space3d": true, "text": true },
      "assets": ["a1b2…png", "c3d4…ttf"],
      "ports": {
        "in":  [ { "name": "#hpText.setText", "type": "string" }, { "name": "#ammo.setProperty", "type": "number" } ],
        "out": [ { "name": "#fire.Click", "type": "pulse" }, { "name": "#hpBar.hover", "type": "bool" } ]
      }
    }
  ],
  "assets": [ { "file": "assets/a1b2…png", "kind": "image", "bytes": 12345, "sources": ["hud/img/bar.png"] } ]
}
```

- `ports` 由 17.6 的連接點反射產生（同一套程式碼），是宿主綁定的**契約**（`params` 為可選欄位，0.3 不產生；`setProperty` 的 `prop` 是邊的參數，不屬於連接點）：C++ 端執行期以 `xt::flow::HostPorts`（13 §13.6，T-144）綁定：`ports.setInput("#score.setText", value)`、`ports.addOutputListener("#fire.Click", fn)`；輸出埠名接受 manifest 的 `document.<port>` 與 `.xtflow` 的 `#document.<port>` 兩種寫法。連接點名稱穩定性等同元素 id 的穩定性——編輯器改元素 id 時比照 UI id 提示。
- `requires` 給宿主早期檢查（例如只做了貼圖路徑、不支援 3D 的宿主可以拒載或降級）。
- 版本：`xtframe` 與 `irMajor` 供宿主拒絕不相容的 bundle；bundle 自己不做語意版本，交給資產管線/版控。

### 22.3b 宿主自有容器（不在本倉；2026-10-10 去宿主化）

宿主若要把 bundle 目錄裝進自己的單檔容器（部署、輕度混淆），格式與讀寫都在宿主倉；本倉的 `Bundle::open`、編輯器與工具只認目錄 bundle，遇到不認得的檔案就是一般的「不是 bundle」錯誤，不辨識任何宿主的 magic（T-304 移除了舊的辨識碼）。原本寫在這裡的 CryEngine 容器規格搬到 CE 插件倉 `docs/08-xtframe-integration-log.md`。

外部資產（T-150）：`engine://` 資產不參與去重、不寫入 `assets/`、manifest `assets` 與 UI `assets` 不列，`--strict` 不報。

適配模式 `fit`（T-156 #157）：`bundle.xtproj` 與 manifest 的 `uis[]` 可選欄位 `"fit": "native"|"stretch"|"contain"|"cover"|"center"`，缺省 `native`（網頁語意：視口 = 螢幕，流式排版）；xtproj 為 native 時不寫出，manifest 永遠寫出；`UiInfo::fit`、`ProjectUi::fit`。宿主依它把設計框（`size`）放進視口；宿主無欄位時沿用自己的預設。匯入預設：Figma frame 與 Claude Design 畫板 → `contain`，其餘 `native`；重新匯入保留原值。UI 清單面板（編輯器）與瀏覽器各有 Fit 欄；`xtframe-view --fit` 可覆寫。

## 22.4 CLI：`xtframe-pack`

```
xtframe-pack <bundle-dir | bundle.xtproj> -o <out-dir | out.xtb> [--ui <id>]... [--strict] [--no-thumbs] [--embed]
```

- 對每個 UI 呼叫 `xt_compile`（12 章），共用樣式表依 22.2 隱含載入；去重資產；渲染縮圖（SW，`--no-thumbs` 略過，無 xt_sw 的建置也略過）；反射連接點；寫 manifest。
- `--ui` 只重 pack 指定的 UI 並**只更新** manifest 對應項與新增的資產（增量；不刪除孤兒資產，`--prune` 才刪）。
- 結束碼與診斷格式同 `xtframe-compile`。

## 22.5 宿主 API：`xt_bundle`（新函式庫）與 13 章新增

`xt_core` 零第三方相依，manifest 是 JSON，所以 bundle 讀取放在新的小函式庫 **`xt_bundle`**（`bundle/`，相依 `xt_core` + vendored nlohmann，比照 `xt_flow`）；核心只加一個不涉 JSON 的 `Context::reloadDocument`。

```cpp
namespace xt {                                    // core（13 章新增）
/// 同一 handle、同一視口換新 IR；輸入佇列與時鐘不動，文件狀態全部重置（0.3 不保留執行期值）。
Result<DocumentHandle> Context::reloadDocument(DocumentHandle, IrBlob, bool takeOwnership);
}

namespace xt::bundle {                            // xt_bundle
struct PortInfo { std::string name; std::string type; bool isInput; std::map<std::string, std::string> params; };
struct UiInfo {
    std::string id, name, file, flow, thumb;      // file/flow/thumb 相對於 bundle 根
    Vec2 size; std::vector<std::string> tags;
    bool requiresEffects = false, requiresSpace3d = false, requiresText = false;
    std::vector<std::string> assets;              // assets/<hash>.<ext>
    std::vector<PortInfo> ports;
    uint64_t fileHash = 0;                        // manifest 記錄的 .xtir 內容雜湊，reload() 比對用
};
class Bundle {
public:
    static Result<std::unique_ptr<Bundle>> open(std::string_view dir);   // 0.3 只支援目錄
    std::string_view id() const; std::string_view name() const;
    std::span<const UiInfo> uis() const;
    const UiInfo* ui(std::string_view id) const;
    AssetResolver& assets();                      // FileAssetResolver(dir, decode)；decode 由 open 的參數注入（同 FileAssetResolver）
    /// 重讀 manifest；回傳 fileHash 改變或新增的 UI id（熱重載用）。
    std::vector<std::string> reload();
};
Result<DocumentHandle> loadUi(Context&, Bundle&, std::string_view uiId);     // 讀 ui/<id>.xtir，資產走 bundle.assets()
Result<DocumentHandle> reloadUi(Context&, Bundle&, DocumentHandle, std::string_view uiId);  // 讀新 .xtir → Context::reloadDocument
}
```

- **熱重載**：宿主輪詢 `Bundle::reload()`（每 0.5 s 讀一次 manifest 就夠），對回傳的 id 呼叫 `reloadUi`。編輯器存檔即 `xtframe-pack --ui <id>` 到發佈目錄。流圖/宿主的連接點以名稱重綁，名稱消失者回報警告（`FlowRuntime::rebind(Document&)`）。
- `.xtb`（zip）**不在 0.3**：需要 vendored zip 讀取器（miniz 候選）；CE 匯入管線先解開成目錄。
- 沒有 bundle 的用法（單一 `.xtir`、`loadDocumentFile`）全部保留。

## 22.6 編輯器（17 章修訂）

- 開啟 `bundle.xtproj`：文件樹上方多一層「UI 清單」（id、名稱、縮圖、tags），切換 UI = 切換 `EditorSession` 的文件；未存檔的多個 UI 各自保留修改狀態。
- 共用樣式表在樣式規則面板顯示來源「shared」，可編輯（改到的是 `shared/base.xts`，影響全部 UI，面板提示）。
- 新增 UI：由範本（空白 / 複製現有）建立，寫進 `bundle.xtproj`。
- 「發佈」動作 = `xtframe-pack`（可設「存檔即發佈」，給熱重載用）。
- 連接點面板多一個「匯出契約」檢視：就是 manifest 的 `ports`，改 id 時警告。

## 22.7 唯讀瀏覽器 `xtframe-browse`

- 獨立 Qt 程式（`xt_editor_qt` 的預覽 widget + `Bundle`），開發佈形態。左：UI 清單（縮圖、名稱、tags、size、requires 圖示）；右：即時預覽（SW/GL）、連接點表、資產表。不能修改任何東西。
- 拖曳：把 UI 拖出時提供 MIME `application/x-xtframe-ui`（JSON：`{bundle, ui, file, ports}`）與純文字 `<bundle>/<ui>`；引擎編輯器的節點圖或場景元件接這個 MIME 即可。「複製 id」與「複製 C++ 片段」（`ctx.loadUi(bundle, "hud")`）按鈕。
- 同一套 widget 也是宿主編輯器窗格裡 UI 瀏覽器面板的內容（17.10）。

## 22.8 任務（16 章 M11）

| 任務 | 內容 | 相依 |
|---|---|---|
| T-90 | `xt_bundle` 骨架：`bundle.xtproj` v2 讀寫（`ProjectFile`）、隱含 bundle（單一 `.xt`）、manifest 讀取（`Bundle::open/uis/ui/assets/reload`）、單元測試 | — |
| T-91 | `xt_compile`：`CompileOptions.impliedStylesheets`（共用樣式表隱含 `<link>`）、`META` 的 `bundle`/`ui` 鍵、`CompileOptions.assetNameRewrite` 回呼（pack 用來改寫 `ASST.name`） | T-90 |
| T-92 | `xtframe-pack`：對每個 UI 編譯、資產去重、縮圖（SW）、連接點反射（連結 `xt_flow`）、manifest、`--ui` 增量、`--prune` | T-91 |
| T-93 | 核心 `Context::reloadDocument`；`xt_bundle::loadUi/reloadUi`；`FlowRuntime::rebind`；`engine_integration` 範例改用 bundle + 熱重載輪詢 | T-92 |
| T-94 | 編輯器：開 `bundle.xtproj`、UI 清單面板、切換 UI（多文件的 `EditorSession`）、共用樣式表來源標示、發佈動作與「存檔即發佈」 | T-90, T-92 |
| T-95 | `xtframe-browse`：清單、預覽、連接點與資產表、拖曳 MIME、複製 id / C++ 片段 | T-93 |
| T-96 | HUD demo 改成 bundle 範例（加暫停選單 UI，共用主題 `shared/base.xts`），README 與 CHANGELOG | T-94, T-95 |

## 22.9 已決定的取捨（2026-09-22）

- zip 直讀不在 0.3（見 22.5）。
- `reloadUi` 0.3 全部重置，不保留執行期值；熱重載的目的主要是看外觀。
- 連接點反射留在 `xt_flow`，`xtframe-pack` 連結 `xt_flow`；不動核心。
- manifest 解析放 `xt_bundle`，核心不碰 JSON。

## 22.10 UI Project 與 bundle 的關係（2026-09-27 定案；對應 30 章）

- **來源形態**改為 UI Project（`ui-project.json` 為根；`bundle.xtproj` 相容讀取並可 `--upgrade`）。bundle 仍是發佈形態，內容只含執行期需要的。
- **manifest 新欄位**（T-170 已做 `tokens`（自 `shared.styles` 取）與 `themes[]`（`{name, file}`，來自 `<root>/themes/*.xts`，複製到 `<out>/themes/`，`--prune` 涵蓋；API `Bundle::tokens/themes`、`loadThemes(ctx, bundle)`、`parseThemeTokens`））、`strings[lang]`（`@key` 表檔）、`fonts.fallback[lang]`（回退鏈）、`glyphs[device]`（按鍵字形集）、`symbols`（符號來源表摘要，供瀏覽器）、`ports`（連接點表，供宿主生成節點與標頭）、`assets.manifest`（26.3.3 子集：尺寸、alpha、九宮格）、`atlas[]`、`flows.global`（`bundle.xtflow`；T-193 已做：來源在專案根或 `shared/`，複製到 `<out>/bundle.xtflow`）、`motionSpec`（節制規格摘要，供宿主 lint 與減少動態設定）。
- **熱載入契約**：可不重建 Context 的變更 = manifest、`tokens/themes`、`strings`、`fonts`、`glyphs`、符號定義、`.xtir` 重編（保留 port 值、自訂狀態、焦點與播放頭）；結構不相容（id 消失）時回報並整體重載。
- **不進 bundle**：`references/`、`contracts/`（除 motionSpec 摘要）、`recipes/`、sidecar、審批狀態、截圖。
- `xtframe-browse` 顯示 UI Project 的規格、token、慣性、符號庫（含契約）與 bundle 內容。

- T-179：`strings/<lang>.csv`（UTF-8 可含 BOM、RFC 4180、兩欄、可選 `key,text` 標頭）→ manifest `"strings": {lang: path}`、`"fonts": {"fallback": {lang: [families]}}`；`parseStringsCsv`、`BundleLocalizationProvider`（`load/loadFile/addCsv/set/languages`；缺項依標籤前綴回退 `zh-TW → zh`）；`Bundle::strings/fontFallbacks/languageCoverage/localizationKeys`；`xtframe-pack --lang a,b`（`--prune` 涵蓋 `strings/`；`i18n` lint：未翻譯、未用、重複、缺表；壞表為錯誤）；`xtframe-view --lang`；瀏覽器狀態列顯示各語言覆蓋率。

- T-183：manifest `"project": {name, game, version}` 緊接 `bundle` 之後（只在由 `ui-project.json` 打包時）；`"motionSpec"` 最後（`contracts/motion-spec.yaml` 整份轉 JSON；壞檔為 `spec:` 錯誤）；`assets[].manifest {id, w, h, alpha, slice}`（22.10 的 `assets.manifest` 以逐列形式實作）；themes／strings 來自 `shared.themes`／`shared.strings`；`publish.languages` 為 `--lang` 預設、`publish.outDir` 為 `-o` 預設；`references/`、`contracts/`（除 motionSpec）、`recipes/`、`conventions/`、`assets.yaml`、sidecar 不打包；API `Bundle::project()`、`motionSpec()`。

### 22.10.1 實作記錄（T-199 #197、T-200 #198）：manifest 新欄位

- `atlas[{name, file, variants, format, w, h, entries[{asset, x, y, w, h, rotated:false}]}]`、`assets[].atlas`（歸屬頁）、`assets[].format`（png／bc1／bc3／bc7）、`assets[].variants.2x`（`img@2x.png` → `assets/<hash>@2x.<ext>`）。`Bundle::open` 以 `setImageMeta` 登記 cell；`Bundle::assetTable()`／`atlases()`。
- `symbols[]`（有符號時才寫，位於 `fonts` 後、`flows` 前）：`{name, source, requires{states, slots}, stateGroups[{name, states}], slots, ports[{name, type, dir}]}`；來源為專案符號目錄（`shared.symbols`，檔名排序）再各 UI 文件，同名以先定義者為準。API：`Bundle::symbolContracts()`、`readSymbolContracts(xt, sourceName)`、`symbolContractSummary()`（C++ 欄位 `slotNames`，因 `slots` 與 Qt 巨集衝突；JSON 鍵仍是 `slots`）。
- `bundle::irFileHash(span<const uint8_t>)`：FNV-1a 64，將 META `compileTimeUnix` 與 META 區塊標頭 CRC 歸零後計算；`packUi` 與圖集重寫皆用它，修掉 `test_ui_project` 的 flaky；`.xtir` 位元組本身仍含時間，未加 `--deterministic`。

### 22.10.2 T-204（#203）：manifest `glyphs`

- 掃描 `<assetRoot>/glyphs/<device>/<key>.<ext>` 再 `<root>/glyphs/...`，同 device/key 先找到者為準；device 只接受 mouse、gamepad、touch、keyboard（其他目錄警告後略過）；只收點陣圖，`@2x` 隨 1x 打包；**glyph 圖不進圖集**（執行期依名稱解析，`Document::resolveGlyphs` 只比對 kind 0 名稱）。
- 形狀：`"glyphs": {"<device>": {"<key>": "assets/<hash>.png|.dds"}}`，依 device、key 排序，位於 `fonts` 後、`symbols` 前。
- API：`GlyphInfo{device, key, asset}`、`Bundle::glyphs()`、`Bundle::glyphAsset(device, key)`（`reload()` 一併更新）。UI 仍需在 IR ASST 引用該圖（如 `<img>`）才畫得出來。

> 2026-09-27（17.4.1）：manifest 新增 `spec.safeArea`（來自 `ui-project.json:spec.safeArea`）與 `spec.logicalHeight`（預設 1080），供宿主 `setDevice` 預設值；T-207（#207）已寫：位置在 `project` 之後、`uis` 之前，只在從 ui-project.json 打包時寫；`BundleSpec{safeAreaDefault, safeAreaConsole, logicalHeight}`、`Bundle::spec()`。T-210（#209）：`logicalHeight` 改從 `ui-project.json:spec.logicalHeight` 取（`ProjectSpec::logicalHeight`，缺省 1080，只在 ≠ 1080 時寫出、位於 `safeArea` 之後；非正數或非有限值讀成 1080）。

## 22.11 三層檔案規則：UI Project → Bundle → UI（2026-09-27 定案）

使用者：「每個地方寫的跟讀的都不一樣，統一一下規則……編輯器沒有『建立 UI Project』和『另存 UI Project』……初次使用的人一定搞不清楚它怎麼運作」。現況：獨立編輯器開 `.xt` 無專案、Publish 去向由隱藏設定決定；宿主窗格「Import bundle…」實際要來源 `bundle.xtproj`；宿主執行時只吃 `manifest.json` 目錄；skill 產出舊格式 `bundle.xtproj`。以下為唯一規則，所有端點與文件用語一致：

### 22.11.1 詞彙（固定，UI 與文件都用這三個詞）

| 層 | 是什麼 | 檔案 | 誰讀 | 誰寫 |
|---|---|---|---|---|
| **UI Project** | 來源，唯一可編輯的東西 | 資料夾，根檔 `ui-project.json`（30 章）；內含 `<ui>/<ui>.xt` `.xts` `.xtflow`、`shared/`、`contracts/`… | 編輯器、宿主編輯器窗格、`xtframe-pack`、`xtframe-view`（自動打包）、`xtframe-browse`、skill | 編輯器、skill `xtui`、`xtframe-pack --upgrade` |
| **UI** | 一個畫面 | `<ui>/<ui>.xt`（+ `.xts`、`.xtflow`、sidecar） | 同上 | 編輯器 |
| **Bundle** | 發佈產物，唯讀 | 目錄：`manifest.json` + `ui/*.xtir` + `assets/` + `strings/` + `themes/` + `bundle.xtflow`… | 宿主執行時、`xtframe-view`、`xtframe-browse` | `xtframe-pack`（= 編輯器 Publish、宿主窗格 Publish、skill verify） |

- **專案檔副檔名 `.xtui`**（使用者 2026-09-27：「附檔名直接用 json 很掉漆」）：UI Project 的根檔是 `<name>.xtui`（內容 JSON，格式同 30 §30.6），例如 `Fall of Warlord.xtui`；一個資料夾只允許一個 `.xtui`；可關聯到編輯器雙擊開啟；bundle 的 `manifest.json` 不改（執行時輸出，不是人碰的檔）。`ui-project.json`（T-198～T-213 的過渡命名）與 `bundle.xtproj` 一樣降為**舊格式只讀升級**：開到即記憶體升級，存檔寫 `<name>.xtui`，舊檔原樣保留。sidecar `<ui>.xteditor.json` 不改（內部檔，使用者裁決）。
  **T-214（#213）實作**：`UiProject::load(path)` 接受目錄或檔案——目錄：唯一的 `*.xtui`（不分大小寫；多於一個 → `kInvalidArgument`）→ `ui-project.json`（`legacyKind=kUiProjectJson`）→ `bundle.xtproj`（`kBundleXtproj`，`legacy=true`）；給檔案就讀該檔（明確指定舊檔優先於旁邊的 `.xtui`）。檔名 `uiProjectFileName(name, id)`：name 空、含路徑分隔或 `<>:"|?*`、控制字元、**非 ASCII**、前後空白、結尾 `.`、`.`／`..`、Windows 裝置名 → 改用 `<id>.xtui`（非 ASCII 退回 id 的原因：bundle 程式庫以窄字元 `fopen`，**待補 UTF-8 路徑（`std::filesystem::u8path`）後放寬**）。`save()` 寫 `path()` 後刪同目錄其他所有 `*.xtui`（只改大小寫時先 rename）；舊格式永不寫不刪，存檔成功後清 `legacyKind`。API：`LegacyKind`、`legacyKind`、`path()`（`.xtui`）、`dir()`、`fileName()`、`kUiProjectExtension`、`kLegacyUiProjectFileName`、`isUiProjectFilePath()`、`findUiProjectFile()`、`hasUiProjectFile()`（`.xtui` 或 `ui-project.json`）；`upgradeToUiProject` 在已有 `.xtui`／`ui-project.json` 時拒絕。範例：`examples/demos/game-ui/XTFrame Game UI demo.xtui`、`examples/ui-project-starter/Starter UI Project.xtui`。編輯器：Open 篩選 `*.xtui ui-project.json bundle.xtproj *.xt`；Recent 記 `.xtui`；面板舊格式提示「Read from %1 (old format); saving writes %2」；`xtframe-editor --register-file-type`（HKCU `Software\Classes`，`.xtui` → `XTFrame.Project`，不呼叫 SHChangeNotify）；服務 `project.create/saveAs/get` 的 `path` = `.xtui`、加 `dir`、`get` 加 `legacyFile`；匯入器輸出 `<name>.xtui`（舊 `ui-project.json` 目錄旁邊新增）。
- `bundle.xtproj`：**舊格式，只讀不寫**。任何端點開到它 → 以 `UiProject::load` 升級成記憶體中的 UI Project；存檔時寫 `ui-project.json` 並把 `bundle.xtproj` 留在原地（不刪、不再更新，下一版移除讀取）。
- 一個 `.xt` 單獨開啟 = **未命名 UI Project**（記憶體中，id 取檔名）；「Save」仍只存 `.xt`；要發佈或加第二個 UI 必須先「Save As UI Project…」落地。
- Bundle 永遠是**完整**的（`manifest.json` 列出全部 UI）；「Publish Current UI」是對同一個 bundle 的增量更新（`xtframe-pack --ui`），不會產生只有一個 UI 的 bundle。

### 22.11.2 各端點的讀與寫

- **獨立編輯器**（17 §17.1.1）：File → **New UI Project…**（精靈：名稱、id、位置、範本 = `examples/ui-project-starter` 或空白）、Open…（`ui-project.json`／`bundle.xtproj`／`.xt`）、Save、Save As…、**Save As UI Project…**、**Publish…**（對話框：目標資料夾，預設 `<project>/publish/`，記在 `ui-project.json:publish.outDir`；最近的發佈目標記在 `publish.targets[]`；沒有任何宿主專屬的發佈類型——要發到引擎專案就選那個目錄；T-304 移除了 `ce-dir` 類型與「Publish to CryEngine project…」）。T-212（#211）已實作，細節 17 §17.1.1.1。Bundle 選單的「Output directory」與「Publish on save」併入 Publish 對話框（publish on save = 對最後一次目標）。標題列 `<project name> — <ui id>`；UI Project 面板頂端顯示專案路徑與最後發佈位置／時間；首次啟動顯示 **Welcome 頁**（New／Open／Recent + 三層圖一段話）。
- **`xtframe-pack <UI Project 目錄> -o <bundle 目錄>`**（既有）。
- **`xtframe-view`**：開 bundle 目錄、`manifest.json`、`.xtir`、`.xt`、UI Project（自動打包到 `%TEMP%`）。T-213（#212）：UI Project 目錄／`ui-project.json`／`bundle.xtproj`（記憶體升級）→ 打包到 `%TEMP%/xtframe-view/<id>-<路徑 FNV64>`（無縮圖、關 atlas、不回寫 assets.yaml、prune），印 `packed <dir> -> <bundle>`，再當 Bundle 開（套 manifest 的 flow／fit／size／`spec.safeArea`／`logicalHeight`）；多 UI 未給 `--ui` 列出 id 後 exit 2；R 重新打包；`--project <dir>` 為別名（保留 T-193 載入全部 UI + 全域流圖，改自打包後 manifest；不可與位置參數同給）。限制：自動打包不複製標準符號庫進來源（連到來源沒有的標準符號要先 `xtframe-pack`）；xtframe-view 的 resolver 不登記 atlas meta，`--atlas` 打的 bundle 圖可能不顯示。
- **宿主執行時與宿主編輯器窗格**（各宿主倉）：執行時只讀 Bundle，宿主若另有容器則目錄 bundle 與容器在功能上必須完全等價，不得有只支援容器的路徑；錯誤訊息指向「'x' is not a published bundle (no manifest.json) — publish the UI Project first」。窗格的動詞是 **「Open UI Project…」**（接受舊格式升級）與 **「Publish…」**（目錄 bundle 一定可選）。CryEngine 的落地細節在 CE 插件倉 `docs/08-xtframe-integration-log.md`。
- **skill `xtui`**：輸出 UI Project（`ui-project.json`），驗證與開編輯器都用它；`xtui-editor` 經服務，`project.*` op 提供 New／Save As／Publish（29 §29.10 補三個 op：`project.create`、`project.saveAs`、`project.publish`）。
- **文件用語**：22 章標題「bundle」指發佈產物；凡指來源一律寫 UI Project；`bundle.xtproj` 只出現在「舊格式」段落。

### 22.11.3 宿主容器只屬於宿主（使用者 2026-09-27 定案；2026-10-10 去宿主化）

「確保引擎端能直接使用 editor 這邊導出的任何 UI，那個引擎專用的格式只在引擎的 editor plugin 建立或編輯過才使用。」

- **獨立編輯器發佈的目錄 bundle 是每個宿主的第一公民**：放到宿主載入 bundle 的位置即可開啟、註冊 port 節點、熱載入；不需要任何轉換。
- **宿主自己的容器格式只由宿主自己的工具產生**，且只對在宿主裡建立或編輯過的 UI Project；它是宿主內部的封裝與輕度混淆。本倉的編輯器、`xtframe-pack`、`xtframe-view` 不讀不寫、也不辨識它（22.9 的單向限制由宿主自己守）。
- 每個宿主的驗證必須包含「獨立編輯器 Publish → 複製目錄到宿主 → 宿主內開啟、port 節點、熱載入」一整條。

### 22.11.4 匯入器輸出 UI Project（T-213 #212）

`xt_writer::writeBundle` 改以 `UiProject::save` 寫 `ui-project.json`，不再產生 `bundle.xtproj`（`xtframe-import` 用法 `<project-dir>`，無 `--project` 旗標）。已有 `ui-project.json` → 合併（保留專案欄位與 UI 順序）；目錄只有舊 `bundle.xtproj` → 旁邊新增 `ui-project.json`，舊檔不動。巢狀匯入（`-o` 指到另一 UI Project 的子目錄）→ 自己的 `ui-project.json` 並沿用上層 `spec` 與 token 表（相對路徑如 `../shared/tokens.xts`），再匯入逐位元組一致。首次匯入板高 ≠ 1080 即寫 `spec.logicalHeight`（CLI 印 info）。「consider moving it」提示只在專案 token 表存在時才出。T-139 sibling `.xtflow` 慣例保留。`examples/demos/game-ui/bundle.xtproj` 已刪。

## 22.12 `xt-standard:` 連結 scheme（T-235 #232）

`<link rel="symbols" href="xt-standard:<Name>"/>` → `<standardSymbolDir>/<Name>.xt`（`<Name>` 不得含 `/ \ . :`；未設目錄為編譯錯誤）。目錄來源：編譯器 `CompileOptions::standardSymbolDir`；`xtframe-compile --standard-symbols DIR`（預設 `<exe>/symbols`）；pack 沿用 `PackOptions::standardSymbolDir`（此 scheme 不複製）；編輯器 `EditorSession::setStandardSymbolDir()`（預設 `<appDir>/symbols`，預覽與 publish 共用）。只給未命名專案用；具名專案一律複製到 `shared/symbols/` 並寫相對路徑（30 §30.9）。未命名專案連了 `xt-standard:` 時，標準 `tokens.xts` 作為隱含預設 token 層（不寫檔），存檔提示「Save As UI Project to copy standard symbols into the project」。

## 22.13 打包正確性（2026-10-02 審查修正；T-262a #270、T-258a #269、T-258b #272）

- **增量打包對 atlas／font-subset bundle 一律拒絕**：目標 bundle 是以 `--atlas` 或 `--font-subset` 建的，`--ui`（編輯器的「Publish Current UI」）在寫任何東西之前失敗：`incremental pack is not supported for a bundle built with --atlas or --font-subset; run a full pack`。判斷依據是既有 bundle 的 manifest，不只是這次的選項。（編輯器對這類 project 改跑完整打包是 T-262b，尚未實作；目前使用者會看到這則錯誤。）
- **`--prune` 與失敗的 UI**：保留下來的 manifest 項從上一份 manifest 帶回它的資產列；`prune()` 計入每個 `ui.assets`；失敗的執行不刪任何東西。
- **`Bundle::reload()`** 與 `open()` 共用 `Bundle::commit()`：重新登記資產表、atlas 與 resolver 的圖片 meta，並忘掉已解碼的 atlas 頁（`FileAssetResolver::forgetFile(path)`）。已知限制：`reload()` 之前已解析過的資產 id 仍拿舊的 atlas 頁（需要 resolver 快取改以路徑為鍵，T-263b）。
- **刪除延後**：atlas、壓縮、字型子集三個 pass 要取代的檔案，等 `rewriteUis()` 對每個 UI 都成功之後才刪。
- **UI 自己的 flow 必須可讀且通過 `FlowGraph::parse`**，在寫出該 UI 的任何東西之前檢查（與全域 flow 相同）。空的 flow 是 `{"nodes": [], "edges": []}`，`{}` 不合法。
- **atlas 參數**：`maxSize` 64..16384（`kMinAtlasSize`／`kMaxAtlasSize`）、`padding` 0..64（`kMaxAtlasPadding`）。
- **超限圖片**：收資產時（含 `@2x`）讀標頭尺寸，超過 16384 px／64M 像素發 warning `image "<path>" is <w>x<h>; larger than 16384 px per side or 64M pixels is not decoded at run time`，打包照常成功。上限常數只存在 `sw/include/xt/sw/sw_png.h`。編譯器不檢查（每次重新編譯讀圖檔標頭不划算）。
- manifest 的 UI `size` 必須有限且每邊 1..16384（03 §3.13）。

## 22.14 審查修正鏈補記（2026-10-07 審查修正鏈合併；T-262b #313、T-262c #321、T-273b #287、T-273c #307、T-273d #312、T-263c #289）

- **manifest 路徑**：`Bundle::open` 拒絕任何路徑欄位的前導 `/`／`\`、第二字元 `:`、任一 `..` 段（`uis[].file/flow/thumb/assets[]`、`assets[].file/variants.2x`、`atlas[]`、`themes[]`、strings、`flows.global`、glyphs）：`kInvalidArgument`，`manifest path escapes the bundle: <value>`。`assets[].sources` 與 `symbols[].source` 不檢查（不會從 bundle 開啟）。JSON 巢狀上限 32（12 §12.9）。
- **原子寫入**：`.xtui`、`manifest.json`、`assets.yaml` 經 `<path>.tmp` 再 `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)`／`rename`；失敗移除暫存檔。
- **`assets.yaml` 回寫**：解析器遇到使用者註解（寫入器自己的 `# Asset manifest` 首行除外）、非純量值、非預期的 `measured` 形狀或未知頂層鍵即標 `hasUnsavableContent`；pack 與服務（`asset.import`、`asset.setStatus`，`manifestUpdated: false`）都不改寫該檔，警告 `assets.yaml not updated: it has comments or structured values the writer cannot preserve`。
- `system.back` 對 pulse 輸入發 Pulse（port 型別由 `nodePortInfo` 決定）。
- **編輯器單 UI 發佈**在 `publish.atlas.enabled` 開啟時改為整個 bundle 打包（訊息 `Atlas / font subsetting is on: the whole bundle was published`）；別的工具以 `--font-subset` 建的 bundle 仍得到 T-262a 的錯誤。
- **發佈時的資產名**（12 §12.9）：`manifest.assets[].sources` 列 project 外檔案的檔名、project 內的絕對參照相對於根；增量打包以 `assets[].file` 為鍵。打包的資產檔名為 `assets/<hash16>.<ext>`（ASCII），執行期的非 ASCII 只在 bundle 目錄。全面外洩測試：bundle 的每個檔案都不得含來源目錄的標記，例外清單只有影片的兩個檔。
- **影片（未定）**：bundle 不帶影片檔；`<video src>` 或 `engine://video:` 的來源在 project 外時，pack 每個不同路徑警告一次 `video "<path>" is outside the project; a bundle does not carry video files, so the published UI names it by this path`（`--strict` 下為錯誤、每個失敗的 UI 各一行）。bundle 是否帶影片、何種 kind、`src` 與鍵在發佈建置的語意，待與使用者討論（BACKLOG D）。
- `Bundle::reload()` 的限制（reload 前已解析的資產 id 仍拿舊 atlas 頁）寫在標頭；`Bundle::open` 設定 `setFileStatusFn(&xt::fileStatusUtf8)`；bundle 檔案讀取掛鉤收到 `joinPath(bundle.dir(), file)`，不經碼頁轉換；`bundle/src/file_status.cpp` 須在 CE 預建庫的原始碼清單內。

## 22.15 發佈時移除 `editor-only` 子樹（2026-10-15；03 §3.13，T-342）

`xtframe-pack` 與編輯器 Publish 在編譯每個 UI 前，先把帶 `editor-only` 屬性的元素連同子樹從文件移除，再編譯；只被這些子樹引用的素材不進 `assets/`，`manifest` 不列；移除的 id 若被流圖／綁定／`<use>` 引用 → lint 警告。縮圖（`thumbs/`）以移除後的文件渲染。
