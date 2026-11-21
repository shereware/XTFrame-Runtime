# 07 · 文字、國際化與 TextShaper

## 7.1 目標與範圍

文字系統從第一版就必須正確處理：

| 需求 | 第一版 | 做法 |
|---|---|---|
| 多語言字型回退（一段文字混用拉丁、CJK、阿拉伯、emoji） | 是 | 逐碼點依字型覆蓋範圍切 run，走 `font-family` 清單再走系統回退 |
| 複雜文字 shaping（阿拉伯連字、天城文、泰文） | 是 | HarfBuzz |
| 雙向文字（阿拉伯、希伯來與拉丁混排；UAX#9） | 是 | SheenBidi（Apache-2.0）算 embedding level；核心依 L2 重排 |
| 段落方向 `direction: ltr / rtl` | 是 | 影響 bidi 基底方向與 `text-align: start/end` |
| Unicode 斷行（UAX#14） | 是 | libunibreak（無泰文/東南亞字典：無空白泰文以字素強制切，屬已知限制，見 16.3） |
| 直排（`writing-mode: vertical-rl / vertical-lr`） | 是，**限文字元素內部** | 行變成直欄；`text-orientation` 決定字形直立或旋轉 |
| IME 組字（中日韓輸入法、任何 composition） | 是 | Composition 事件（08.8） |
| 字素叢集（grapheme cluster）游標移動 | 是 | libunibreak 的 UAX#29 |
| 直排的**區塊**流向（整個佈局旋轉、logical properties） | 否，延後 | 見 16.3 |
| Ruby、text-emphasis、hyphenation、justify | 否，延後 | |

原則：**核心（`xt_core`）不含任何 Unicode 資料表**。所有需要資料表的工作（bidi、script、斷行、字素、字型回退、shaping）由 `TextShaper` 介面提供，`xt_text` 是參考實作；核心只做「已切好的 run 排成行、依 level 重排、產生字形位置」。引擎若有自己的文字堆疊，可實作 `TextShaper`。

其他章節出現的 `FontProvider` 一律指本章的 `TextShaper`。

## 7.2 介面

```cpp
namespace xt {

enum class TextDirection   : uint8_t { Ltr = 0, Rtl = 1 };
enum class WritingMode     : uint8_t { HorizontalTb = 0, VerticalRl = 1, VerticalLr = 2 };
enum class TextOrientation : uint8_t { Mixed = 0, Upright = 1, Sideways = 2 };

struct FontRequest {
    std::span<const std::string_view> families;   // 依優先序
    uint16_t weight;      // 100..900
    bool italic;
};

struct FontMetrics {
    float ascent, descent, lineGap;   // 以 1 em 為單位（乘 size 得 px），ascent 為正
    float unitsPerEm;
};

struct ShapedGlyph {
    uint32_t glyphId;
    float    xAdvance, yAdvance;   // px；水平 run 用 xAdvance，直立 run 用 yAdvance（正值向下）
    float    xOffset, yOffset;
    uint32_t cluster;              // 原字串 UTF-8 位元組偏移
    uint8_t  isSpace;              // 1 = 可折疊空白（U+0020、U+0009 等 White_Space 且非 NBSP）；行佈局據此忽略行尾空白，不依賴 glyphId
};

struct ShapedRun {
    FontId   font;
    uint8_t  bidiLevel;            // UAX#9 embedding level；奇數為 RTL
    uint8_t  vertical;             // 1 = 以直立方向 shaping（yAdvance 有效）
    uint8_t  rotated;              // 1 = 直排中順時針旋轉 90° 呈現的水平 run
    uint32_t byteStart, byteEnd;   // 原字串範圍（邏輯順序）
    uint32_t glyphStart, glyphCount;   // 指向 ShapeResult::glyphs；RTL run 的 glyph 已由 HarfBuzz 排成視覺順序
};

struct ShapeResult {
    std::vector<ShapedRun>   runs;          // 邏輯順序（依 byteStart 升冪）
    std::vector<ShapedGlyph> glyphs;
    std::vector<uint8_t>     breakAfter;    // 每個 UTF-8 位元組一格：1 = 此碼點後可斷行、2 = 必斷（\n）、0 = 不可
    std::vector<uint8_t>     graphemeStart; // 每個位元組一格：1 = 字素叢集起點
};

struct ShapeParams {
    FontRequest      font;
    float            size;            // px
    float            letterSpacing;   // px
    TextDirection    paragraphDirection;
    WritingMode      writingMode;
    TextOrientation  orientation;
    std::string_view lang;            // BCP 47，可空；影響 HarfBuzz 語言與 CJK 字形選擇
};

class TextShaper {
public:
    virtual ~TextShaper() = default;
    // 匹配：找不到時必須回傳某個可用字型；完全無字型時回傳 kInvalidFont，佈局視文字為 0 尺寸
    virtual FontId match(const FontRequest&) = 0;
    virtual FontMetrics metrics(FontId) = 0;
    // 一次完成：bidi → script 分段 → 字型回退分段 → shaping → 斷行機會 → 字素邊界
    virtual void shape(std::string_view utf8, const ShapeParams&, ShapeResult& out) = 0;
    // 字形外框（em 單位，y 向下），供 SW 後端與命中測試；格式依 03.4
    virtual bool glyphOutline(FontId, uint32_t glyphId,
                              std::vector<PathVerb>& outVerbs, std::vector<float>& outPoints) = 0;
    // 字形點陣（可選；回傳 false 表示不提供，後端改用外框）
    virtual bool glyphBitmap(FontId, uint32_t glyphId, float size, GlyphBitmap& out);   // 預設回傳 false
    // 字型註冊（可選；Document 載入 @font-face 資產時呼叫；預設回傳 false = 不支援）
    virtual bool addFontMemory(std::vector<uint8_t> bytes);
};

struct GlyphBitmap {
    int width, height;         // px
    int bearingX, bearingY;    // 左、上，px
    std::vector<uint8_t> alpha;   // 8-bit coverage，row-major
};

class NullTextShaper : public TextShaper { /* match 回傳 kInvalidFont，shape 輸出空結果 */ };
}
```

佈局輸出（存於 `TextNodeData`）：

```cpp
struct GlyphRun {
    FontId   font;
    uint8_t  bidiLevel, vertical, rotated;
    std::vector<uint32_t> glyphs;
    std::vector<Vec2>     positions;   // 相對文字節點左上角的基線原點（直排時為欄中心線上的原點）
    uint32_t byteStart, byteEnd;
    Rect     bounds;                    // 此 run 在文字節點內的矩形（供命中與選取）
};
struct TextLine { Rect rect; float baseline; uint32_t firstRun, runCount; uint32_t byteStart, byteEnd; };
// TextNodeData 含 std::vector<GlyphRun> runs 與 std::vector<TextLine> lines
```

## 7.3 `xt_text` 實作要求

第三方：FreeType、HarfBuzz 以 FetchContent；SheenBidi（Apache-2.0）、libunibreak（zlib）vendored 原始碼於 `Code/SDKs/`（見 14.1）。字型 weight/italic 匹配第一版只用 FreeType 的 style flags（不讀 OS/2 usWeightClass）。

**`shape()` 流程**：

1. **Bidi**：SheenBidi 以 `paragraphDirection` 為基底方向算每個碼點的 level。直排時仍算 level（阿拉伯文在直排中以旋轉呈現，仍需正確連字順序）。
2. **Script 分段**：HarfBuzz `hb_unicode_script()`，Common/Inherited 繼承前一個 script。
3. **字型回退分段**：`@font-face` 註冊的字型（`addFontMemory`）同時進入回退清單（排在宿主 `addFallbackFont` 之前）；對每個碼點依 `families` 順序找第一個含此碼點的字型（`FT_Get_Char_Index != 0`）；都沒有則走系統回退清單（`addFallbackFont()` 註冊，預設空；`xt_shell` 依平台註冊常見 CJK / 阿拉伯 / emoji 字型路徑）；仍無則用 `families[0]` 的匹配結果（畫 .notdef）。
4. **run 切分**：(level, script, font, orientation) 任一改變即切。
5. **直排方向**（`writingMode != HorizontalTb`）：
   - `Upright`：所有 run `vertical = 1`，`HB_DIRECTION_TTB`，開 `vert`/`vrt2` 特性。
   - `Sideways`：所有 run `rotated = 1`，水平 shaping。
   - `Mixed`：碼點的 Unicode `Vertical_Orientation` 屬性為 U / Tu / Tr 者 upright，R 者 rotated。資料表由腳本從 `VerticalOrientation.txt` 生成 `vo_table.h`（放 `xt_text`，腳本放 `Tools/Scripts/gen_vo_table.py`）。
6. **Shaping**：每個 run 一個 `hb_buffer`，設 direction（LTR / RTL / TTB）、script、language；`letterSpacing` 加到每個 advance。
7. **斷行**：libunibreak `set_linebreaks_utf8` → `breakAfter`（MUSTBREAK → 2、ALLOWBREAK → 1）。
8. **字素**：libunibreak `set_graphemebreaks_utf8` → `graphemeStart`。

**匹配**：依 `families` 順序找 family 名（大小寫不敏感）；同 family 內以 CSS Fonts Level 4 的 weight 距離規則挑最近，italic 先精確再退回。無匹配 → 第一個註冊的字型；無註冊 → `kInvalidFont`。

**字形外框**：FreeType `FT_Outline_Decompose`，conic 轉 cubic（2/3 提升），除以 unitsPerEm，y 翻轉。快取鍵 `(FontId, glyphId)`。

**點陣**：`FT_Render_Glyph(FT_RENDER_MODE_NORMAL)`，快取鍵 `(FontId, glyphId, round(size*4))`。

**執行緒安全**：所有公開方法持同一 `std::mutex`。

## 7.4 字形 Atlas（`xt_text::GlyphAtlas`，供 GL 後端）

- 單一 2048×2048 R8 紋理，shelf packing。滿了新開一張（最多 4 張），再滿則清空重建。
- 條目鍵同點陣快取。提供 `lookup(key) → {textureIndex, uv rect, bearing}` 與 `flushUploads(callback)`。
- 渲染尺寸 = `size * devicePixelRatio` 四捨五入到 0.25 px。

## 7.5 核心端的行佈局（詳見 06.8）

核心拿到 `ShapeResult` 後：以 `breakAfter` 與 run 的 advance 做貪婪斷行（邏輯順序）→ 每行依 bidi level 做 UAX#9 L2 重排（從最高 level 到最低的奇數 level，逐層反轉連續 run 的順序）→ 依 `text-align`（`start` / `end` 依段落方向解析）定位 → 直排時把「行」當「欄」：`vertical-rl` 第一欄在最右往左排，`vertical-lr` 相反；欄內 run 從上往下，rotated run 的字形以欄中心線為軸順時針轉 90°。

## 7.6 IME 與文字編輯

見 08.8。要點：組字中的字串以「composition 區段」插在游標處參與 shaping 與佈局，Painter 在其下畫底線；宿主透過 `Context::textInputRect()` 取得游標矩形以定位候選視窗；游標移動以 `graphemeStart` 為單位；RTL 段落中的游標視覺位置由 run 的 `bounds` 與 level 決定（邏輯游標 → 視覺座標）。

## 7.7 命中測試

點擊落在文字節點：以 `lines[].rect` 找行（直排找欄），再以 run 的 `bounds` 找 run，再以 glyph advance 累積找最近的字素邊界，回傳邏輯位元組偏移（給 input 的游標定位）。元素層級的命中仍以元素盒為準（08）。

## 7.8 富文本、文字效果與在地化（2026-09-27 定案；對應 25.5d、27.7、30）

- **runs**：一個文字節點可含多個 run，每個 run 帶樣式覆寫（`color`、`font-weight`、`font-style`、`font-size`、`text-effect`、`class`）與可選的行內物件（`<glyph>`、`<img inline>`：inline-block，基線對齊，尺寸依 `height` 或字高）。`TextNodeData` 新增 `runs[] { start, end, styleOverride, inlineNode }`；行佈局（06.8）以 run 為單位量測與換行；IME 編輯只作用於單一 run 的文字欄位。
- **連結**：`<a on:click="…">` 的 run 帶命中矩形清單（08.9），hover 時套 `:hover` 至該 run。
- **文字效果**：`text-effect: outline <w> <color> | glow <r> <color> | shadow <dx> <dy> <r> <color>`（可多個）；SW 與 GPU 於字形層實作（不走一般效果堆疊，避免每字一個 pass）；與 18 章效果可疊加。
- **`text-reveal`**：0..1 顯示比例（依字元或字素簇），可動畫；打字機對話框用。
- **`line-clamp: n`**、**`text-wrap: balance`**、**`font-variant-numeric: tabular-nums`**（0.6）。 **2026-10-14 M32 T-331**：`line-clamp` 與 `tabular-nums` 定義見 05 §5.9.8（`ShapeParams.features` 位旗標交給 shaper；`text-wrap: balance` 仍待排）。
- **在地化**：文字或屬性值以 `@key` 開頭 → 編譯進 `STRS` 表（12.7）；執行時由 `ILocalizationProvider::resolve(key, lang)` 取字串（13.8），未解析顯示鍵；`setLanguage` 觸發全文件重排與字型回退鏈重選（manifest `fonts.fallback[lang]`）；`:lang()` 選擇器依當前語言。
- **`uiscale`** 單位：`1uiscale = 1px × 全域縮放`（宿主 `setUiScale`），字級與間距用它即可支援無障礙放大。
- **編輯器**：雙擊就地編輯含 run 級樣式套用；語言下拉即時預覽；最長翻譯檢查與溢出 lint。

- T-179 回退鏈：依文件語言（無則元素 `lang`）選鏈，精確標籤優先、否則最長 BCP 47 前綴；鏈附加在元素 `font-family` 之後，接 `xt_text` 既有逐字碼回退。切語言：重解析鍵、全元素 `kDirtyStyle`、文字節點清 shaping 快取並 `kDirtyLayout`；宿主以 `setText/setAttribute` 覆寫過的不動。

- T-178 文字效果（每字形、只對點陣路徑）：Dilate = `max cov(p+d)·clamp(R+1−|d|)`，|d| ≤ ceil(R)；Blur = 高斯 σ = R/2、核半徑 ceil(3σ)；旋轉、投影、`geometricPrecision` 的 run 不畫效果；外框路徑轉換、烘焙格式、執行時 `setStyle` 尚未支援 `text-effect`；`glow` 強度固定 1。run 級 `:hover` 未做。

## 7.9 執行期文字與字型選擇（2026-10-07 審查修正鏈合併；T-270a #283、T-270b #286、T-270e #305）

- Text 節點由 `setText`／`bind:text` 設定的文字存於節點自有槽（id ≥ `kRuntimeTextBase` = 0xC0000000），原地覆寫；`ElementRef::text()`／`Document::string()` 的視圖只到該節點文字再改變或節點移除為止。此類 id 不是穩定鍵：節點移除後槽被別的節點重用。
- 整形快取以 id 為鍵但由 `shapedValid` 失效；layout 每次讀內容；Painter 不快取字形序列；GPU 不用 StringId；damage 雜湊取字形 id 與位置。
- 組字字串（`Event::text`、`InputEvent::composition`）在派送它的 `Context::update()` 結束時失效，監聽者要保留須複製。
- 字型家族只能從編譯清單選（05 §5.y）。

## 7.20 字型檔列舉與字重選擇（2026-11-01 定案；改進報告 R4；T-569）

- 目錄字型掃描接受 `.ttf`、`.otf`、`.ttc`、`.otc`；集合檔以 FreeType face index 逐字面列舉，每個字面各成一個候選（家族名取該字面的名稱表）。
- 字重選擇：同家族多個靜態檔依 OS/2 `usWeightClass` 建表，`font-weight` 取最接近者（CSS Fonts 4 的規則：400 以下往下找、以上往上找）；可變字型依 `wght` 軸設實例。要求 ≥ 600 而家族最重 < 600 時合成粗體（FreeType embolden，強度依差值）。
- 堆疊裡第一個 CJK 家族（名稱表含 CJK 或字面覆蓋 CJK 區段）未安裝時，import／view／pack 報 `missing font '<family>'`，不再靜默回退。
- 禁則：行首不得出現 `。，、」』）！？：；`（UAX#14 CL／NS 類），行尾不得出現 `「『（`；加測試 `甲。乙` 窄寬換行。

**T-569 實作記錄（#542，2026-11-04；主模型裁決）**：`.ttc`／`.otc` 逐 face index 列舉，`--list-fonts` 維持 `family<TAB>path` 並在集合檔後加 `#index`（corpus 雜湊前去掉）；名稱表完整解碼 UTF-16 並優先美國英文紀錄；字重以 OS/2 範圍＋CSS Fonts 4 排名；可變字型 `wght` 另開實例（不列入 `fonts()`）；合成粗體強度「每 300 重量差 em/36、上限 500」（FreeType 預設 em/24 比真 Bold 粗太多；調後墨量與 Noto CJK Bold 相當）——接受；CJK 標點回退優先 CJK 字型。**F51 根因不在 libunibreak**：`ft_breaks.cpp` 把斷行值放在多位元組字元的最後一個位元組，而 `text_shaper.h` 約定與 `breakLines()` 讀首位元組，所有 CJK 字後的斷行機會都看不到——已改到首位元組，並加 `breakAfter==3` 禁則值（緊急斷行先退避、退不了讓標點懸出）。`font.missing` 的 fix 內容待定（裁決：fix = board hint，attr `style`，value 為去掉缺家族後的堆疊，T-575 文件化）。Golden：`ui_i18n_cjk_mixed_wrap`（正確斷行）、fairy-forest `demo3-menu` 四張（Italic 700 改合成粗體）；game-ui corpus 仍為 T-562 遺留。

**§7.2 補充：強制斷行字元與 TAB 不繪製（2026-11-20 裁決；T-801）**

起因：宿主經 port 傳入含 U+000A 的文字（兩段），在只註冊自家字型（沒有系統字型）的宿主上，斷行處畫出缺字方框。斷行本身是對的（§7.3 步驟 7、06 §6.8）；規格沒說斷行字元自己怎麼畫——補上：

1. **U+000A 一律斷行，不論 `white-space`**（06 §6.8 既有規則，維持）。這與 CSS 不同（CSS 的 `normal` 把換行摺成空白）——刻意保留：執行期文字（`setText`、`bind:text`、在地化字串）裡的換行就是作者要的換行，宿主不必為此設定樣式。`pre`／`pre-wrap`／`pre-line` 不新增。
2. **強制斷行字元不產生可見字形、寬度為 0**：U+000A、U+000B、U+000C、U+000D、U+0085、U+2028、U+2029。CR LF 算一次斷行（既有）。shaper 對這些碼位輸出該 run 字型的空白字形、advance 0、`isSpace = 1`；不查字型的 cmap、不走缺字後備。
3. **U+0009（TAB）當作一個空白**：輸出空白字形與空白的 advance、`isSpace = 1`。沒有定位點。
4. **行尾空白的判斷一律用 `ShapedGlyph::isSpace`**，不用「字形編號等於 0x20」（`text_layout.cpp` 的既有簡化，會把編號剛好是 32 的一般字形誤當空白）。
5. 來源檔（`.xt`）的文字仍在編譯期把 ASCII 空白摺成一個空白（`&#10;` 變空白）——不變。要在來源文字裡強制斷行，寫 `&#x2028;`（第 2 點之後不再畫出方框）。
6. 行為變更：以強制斷行結尾的行，行寬不再含缺字字形的寬度（置中、靠右的文字會有微小位移）；第 4 點可能讓極少數字型的行尾量測改變。

**§7.2 補充的實作記錄（T-801 #742，2026-11-20）**
- `FtTextShaper::shape`：強制斷行字元輸出 run 字型的 U+0020 字形（`hb_font_get_nominal_glyph`）、advance 0、不加 letter-spacing、offset 0、`isSpace = 1`；TAB 輸出空白字形與空白的 advance（直排用 v advance）、照常加 letter-spacing。這些碼位不參與字型後備：沿用前一個（沒有就後一個）非此類碼位的字型，整段都是這類字元時才查 U+0020 的後備。只在 shaping 之後替換字形；cluster、位元組位移、`breakAfter` 不變。
- `text_layout.cpp`：行尾空白修剪與 `VisEntry::space` 一律用 `isSpace`；line-clamp 修剪時不再重複扣行尾空白的 advance。
- **追加的行為變更（實作時才發現）**：真實字型的空白字形編號不是 32，所以以前以 HarfBuzz shaper 排版時**行尾空白從未被修剪**；現在會修剪——以空白結尾而換行的行變窄（置中、靠右時位置改變）。既有 golden 與 corpus 沒有任何一張因此改變。
- golden 腳本新增 `porttext` 指令（`port` 加上 `\n \r \t \\ \uXXXX` 跳脫）；新 golden `ui_text_hard_breaks`。
- 未涵蓋（裁決：接受）：字型沒有 U+0020 字形時，替換後的字形編號仍是 0、仍會畫出 .notdef——沒有空白字形的字型不是支援的對象。
- 未跑：capi、editor 各套件、gl。未編譯 GCC／Clang。合併時 rebase 後的組合未經 `merge_task.sh` 的建置檢查（代理第一次用了工作樹裡舊的腳本）——主模型另在 main 上重跑 core／text／golden 確認。

## 7.21 字型寬度與家族名稱（2026-11-22；§7.21.1～§7.21.3 定案，§7.21.4 以後為草案待使用者看過）

**實測到的問題（2026-11-22，`E:/coding3/xt-bench/font-axes/`；Release v0.19.2、Windows 系統字型）。**

| 寫法 | 畫出來的 | 原因 |
|---|---|---|
| `font-family: "Arial"` | **Arial Narrow**（寬度 264，Arial 應為 322） | 家族名只取 nameID 16（`ARIALN.TTF` 的 ID16 也是 `Arial`）；同家族同字重同斜體時取先註冊者，而路徑以位元組排序、`ARIALN.TTF` 在 `arial.ttf` 之前 |
| `font-family: "Arial"; font-weight: 700` | Arial Narrow Bold | 同上 |
| `font-family: "Myriad Pro"` | Myriad Pro Cond | 同上 |
| `"Arial Narrow"`、`"Segoe UI Semibold"`、`"Sitka Text"` | 落到下一個家族 | 這些是 nameID 1（舊式家族名）；每個字面只存一個家族字串（ID16 優先） |
| `"Bahnschrift SemiCondensed"`、`"Bahnschrift Condensed"` | 落到下一個家族 | 可變字型的具名寬度只存在 `fvar` 實例與 `STAT`；自家碼只接了 `wght` 一軸（FreeType 與 HarfBuzz 都支援任意軸） |

前兩類是缺陷（指定 Arial 卻畫成 Narrow），照 CSS 修正；第三類是功能。

### 7.21.1 字面的寬度

每個字面有一個**寬度**（百分比，CSS `font-stretch` 的尺度：50～200，100＝正常）：

- 可變字型有 `wdth` 軸：寬度是一個範圍（軸的 min～max），預設值是軸的 default。
- 靜態字面：OS/2 `usWidthClass` 對應 1→50、2→62.5、3→75、4→87.5、5→100、6→112.5、7→125、8→150、9→200——**但只有名稱也這麼說時才採信**：該字面的 nameID 17（沒有就 ID2）或 ID1 含寬度字（`Narrow`、`Condensed`、`Cond`、`Compressed`、`Compact`、`Extended`、`Expanded`、`Wide`，可帶 `Semi`／`Extra`／`Ultra` 前綴；不分大小寫、整字比對）時用 `usWidthClass`，否則一律 100。原因：`usWidthClass` 不可靠（Comfortaa 的 Light＝3、Bold＝7，其實都是正常寬度），照單全收會讓正常的字重被排除而改走合成粗體。名稱有寬度字而 `usWidthClass`＝5 的：`Narrow`／`Condensed` 類當 75、`Extended`／`Expanded`／`Wide` 類當 125。

掃描（`readFontNameTable`，不開 FreeType）因此多讀：OS/2 `usWidthClass`、nameID 1／2／17、`fvar` 的 `wdth` 軸。維持「掃描不開檔解析字形」。

### 7.21.2 家族名稱

一個字面可由兩個家族名找到：**排版家族**（nameID 16，沒有就 ID1；現行）與**舊式家族**（nameID 1，與排版家族不同時才登記）。`font-family` 比對兩者，不分大小寫。

- 以排版家族命中：候選是該家族的全部字面（`"Arial"` → Arial、Arial Narrow、Arial Black…）。
- 以舊式家族命中：候選只有 ID1 等於該名的字面（`"Arial Narrow"` → 四個 Narrow；`"Segoe UI Semibold"` → Semibold 與 Semibold Italic）。在這個集合內照 §7.21.3 選——所以 `"Segoe UI Semibold"`＋`font-weight: 400` 得到 Semibold（集合裡最接近 400 的就是它），與瀏覽器在 Windows 上的結果一致。
- 兩個名字撞在一起時（某家族的 ID16 等於另一家族的 ID1）排版家族優先。
- `--list-fonts`、`FontDesc` 與「缺字型」的訊息仍以排版家族為主；舊式家族名另列（`--list-fonts` 加一欄或加註，格式由實作單定，corpus 雜湊前去掉的規則照 T-569）。

### 7.21.3 選字面的順序

候選集合內依序（CSS Fonts 4 §5.2 的順序）：

1. **寬度**：要求值目前恆為 100（沒有 `font-stretch` 屬性之前）。候選中有寬度範圍包含要求值的取之；否則要求 ≤ 100 時先取較窄中最接近的、再取較寬中最接近的，> 100 反之。**只留下寬度最合的那一組**再往下比。
2. **斜體**（現行）。
3. **字重**（現行 §7.20）。
4. 仍同分取先註冊者（現行）。

不合成寬度（沒有窄體就用正常體，不做水平壓縮）。可變字型的 `wdth` 軸在沒有 `font-stretch` 時設為 100 落在範圍內的值（Bahnschrift：100），等同現行的預設值。

**行為變更**：以系統字型或專案字型畫 `Arial`、`Myriad Pro`、`Bodoni MT`、`Goudy Old Style`、`Minion Pro` 等「同一排版家族含窄體檔」的家族時，從窄體改為正常體；`"Arial Narrow"` 等舊式家族名從回退改為命中。只裝了窄體的機器不變（寬度那一關沒有更合的候選）。

### 7.21.4 可變字型的具名寬度與光學尺寸（草案）

`STAT` 表對 `wdth` 軸列出的具名值（非 elidable 者）登記成家族別名 `"<排版家族> <值名>"`（Bahnschrift：`Bahnschrift SemiCondensed`＝87.5、`Bahnschrift Condensed`＝75）；命中別名＝命中該可變字面並把 `wdth` 釘在該值，字重仍由 `font-weight` 決定。`opsz` 軸的具名值同理（`Segoe UI Variable Display`／`Text`／`Small`、`Sitka Text`…；格式 2 的範圍值取 nominal）。沒有 `STAT` 的可變字型不產生別名。實例的鍵由 `(base, wght, embolden)` 擴成帶各軸座標；下游快取都以 FontId 為鍵，不必動。

### 7.21.5 `font-stretch` 屬性（草案）

新樣式屬性 `font-stretch`（別名 `font-width`；可繼承；StyleProp 第二段下一個空號）：九個關鍵字（`ultra-condensed`…`ultra-expanded`）或 50%～200% 的百分比；進 `FontRequest`、回退快取的鍵、解析與 run 層的請求，成為 §7.21.3 第 1 步的要求值；可變字型把 `wdth` 設成夾在軸範圍內的要求值。可動畫（數值內插）與否待定。匯入器：`font-stretch`、`font` 簡寫裡的寬度關鍵字改為保留。工作量與 T-331（`font-variant-numeric`）同級、但走字型匹配那條路，約 25～30 檔。

### 7.21.6 之後（草案，未排）

編輯器的字型選單改用執行期的掃描結果（含舊式家族名與別名；目前取自專案字型的檔名與 Qt 的家族清單，兩邊不一致）；`@font-face` 的 `font-family`／`font-weight`／`font-style` 描述子在執行期被忽略（家族名一律取字型自己的 name 表，`document_load.cpp:514-533`）——要不要讓描述子生效另議；`font-variation-settings`、`font-optical-sizing: auto` 不在本節。

