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
