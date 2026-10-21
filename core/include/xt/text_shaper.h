#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xt/text_types.h"
#include "xt/types.h"

/// TextShaper interface and the structs it exchanges with callers: font matching/metrics
/// requests, shaping input/output, and glyph outline/bitmap queries. See
/// docs/design/07-text.md §7.2, docs/tasks/T-28.md. Core (`xt_core`) contains no Unicode data
/// tables; a real implementation (HarfBuzz/FreeType/SheenBidi/libunibreak based) lives in
/// `xt_text` (T-29+).

namespace xt {

// PathVerb is defined in xt/node.h (§3.4, glyph outline verb format). Forward-declared here
// (rather than including xt/node.h) to avoid a header cycle: xt/node.h includes
// xt/text_layout.h, which includes this header, for GlyphRun.
enum class PathVerb : uint8_t;

/// A font search request, most-preferred family first.
struct FontRequest {
    /// Candidate font family names, most-preferred first.
    std::span<const std::string_view> families;
    uint16_t weight = 400;  ///< 100..900
    /// Whether an italic/oblique style is requested.
    bool italic = false;
};

/// Font metrics in 1-em units (multiply by size to get px). ascent is positive.
struct FontMetrics {
    /// Distance from the baseline to the top of the font, positive.
    float ascent = 0;
    /// Distance from the baseline to the bottom of the font, typically negative.
    float descent = 0;
    /// Extra spacing between lines beyond ascent + |descent|.
    float lineGap = 0;
    /// Units per em used to scale the other fields (usually 1000 or 2048).
    float unitsPerEm = 1000;
    /// T-324 (05 §5.9.8): distance from the baseline down to the center of the font's underline,
    /// positive below the baseline; 0 = the font gives none (the Painter then uses 0.1 em).
    float underlineOffset = 0;
    /// T-324: the font's underline thickness; 0 = none given (`auto` = max(1px, 1/14 em) is used).
    float underlineThickness = 0;
};

/// One shaped glyph. Positions/advances in px. `cluster` is the UTF-8 byte offset into the
/// original string that produced this glyph (logical order, independent of how glyphs of an RTL
/// run are stored in ShapeResult::glyphs).
struct ShapedGlyph {
    /// Font-specific glyph index (not a Unicode codepoint).
    uint32_t glyphId = 0;
    /// Horizontal advance to the next glyph, in px.
    float xAdvance = 0;
    float yAdvance = 0;  ///< valid for vertical (upright) runs
    /// Horizontal offset applied to the glyph's drawn position, in px.
    float xOffset = 0;
    /// Vertical offset applied to the glyph's drawn position, in px.
    float yOffset = 0;
    /// UTF-8 byte offset into the original string that produced this glyph.
    uint32_t cluster = 0;
    /// 1 = collapsible white space (Unicode White_Space, excluding U+00A0/U+202F/U+2007); line
    /// layout uses this to drop trailing-line whitespace instead of relying on glyphId.
    uint8_t isSpace = 0;
};

/// A run of glyphs sharing font/bidi-level/orientation. `glyphStart`/`glyphCount` index into
/// ShapeResult::glyphs; for an RTL run (bidiLevel odd) those glyphs are stored in visual
/// (left-to-right rendering) order, i.e. reversed relative to logical/byte order.
struct ShapedRun {
    /// Font used to shape this run.
    FontId font = kInvalidFont;
    uint8_t bidiLevel = 0;  ///< UAX#9 embedding level; odd = RTL
    uint8_t vertical = 0;   ///< 1 = shaped top-to-bottom, yAdvance valid
    uint8_t rotated = 0;    ///< 1 = horizontal run rotated 90deg cw for vertical writing-mode
    /// UTF-8 byte offset of the run's start in the shaped string.
    uint32_t byteStart = 0;
    /// UTF-8 byte offset one past the run's end in the shaped string.
    uint32_t byteEnd = 0;
    /// Index of the run's first glyph in `ShapeResult::glyphs`.
    uint32_t glyphStart = 0;
    /// Number of glyphs in the run.
    uint32_t glyphCount = 0;
};

/// Output of TextShaper::shape().
struct ShapeResult {
    std::vector<ShapedRun> runs;  ///< logical order, ascending byteStart
    /// Glyphs for every run, indexed by each ShapedRun's glyphStart/glyphCount.
    std::vector<ShapedGlyph> glyphs;
    /// One entry per UTF-8 byte of the shaped text: 0 = no break, 1 = break allowed after the
    /// codepoint starting at this byte, 2 = must break (e.g. after '\n').
    std::vector<uint8_t> breakAfter;
    /// One entry per UTF-8 byte: 1 = a grapheme cluster starts at this byte.
    std::vector<uint8_t> graphemeStart;
};

/// CSS `text-rendering` (docs/design/23-host-integration.md §23.4, docs/tasks/T-101.md). `Auto`
/// lets a backend pick the fastest legible path (small sizes take the hinted bitmap path with
/// coverage gamma); `GeometricPrecision` forces the unhinted outline path with no gamma, so the
/// rendered shape matches the glyph outline exactly at any size.
enum class TextRendering : uint8_t {
    Auto = 0,                ///< Backend picks bitmap (small) or outline (large); gamma applied.
    GeometricPrecision = 1,  ///< Always the outline path, no hinting and no coverage gamma.
};

/// Input to TextShaper::shape().
struct ShapeParams {
    /// Font search request used to resolve the run's font (and fallback fonts).
    FontRequest font;
    /// Font size in px.
    float size = 16;
    /// Additional space inserted after each glyph, in px.
    float letterSpacing = 0;
    /// Base (paragraph) direction used to resolve UAX#9 bidi levels.
    TextDirection paragraphDirection = TextDirection::Ltr;
    /// Horizontal vs. vertical text layout mode.
    WritingMode writingMode = WritingMode::HorizontalTb;
    /// How characters are oriented within a vertical writing mode.
    TextOrientation orientation = TextOrientation::Mixed;
    /// CSS `text-rendering`. Shaping itself is unaffected; carried here so a caller that shapes
    /// and then rasterizes has the whole text configuration in one struct (T-101).
    TextRendering rendering = TextRendering::Auto;
    std::string_view lang;  ///< BCP 47, may be empty
    /// OpenType features to enable, a set of kShapeFeature* bits (05 §5.9.8, T-331); 0 = the
    /// font's defaults. A shaper that has no OpenType support (NullTextShaper) ignores it.
    uint8_t features = 0;
};

/// ShapeParams::features bit: `tnum` (CSS `font-variant-numeric: tabular-nums`). The other bits
/// are reserved (`lnum` / `onum` later).
inline constexpr uint8_t kShapeFeatureTnum = 1;

/// Glyph coverage bitmap (8-bit alpha), row-major. See docs/design/07-text.md §7.2/§7.4.
struct GlyphBitmap {
    /// Bitmap width in pixels.
    int width = 0;
    /// Bitmap height in pixels.
    int height = 0;
    int bearingX = 0;  ///< left
    int bearingY = 0;  ///< top
    /// Horizontal subpixel offset this bitmap was rasterized at, one of 0/0.25/0.5/0.75 px
    /// (T-101). `bearingX` is relative to the *floor* of the glyph's device-space x, so a caller
    /// blitting the bitmap never has to re-apply this; it is reported for cache/debug purposes.
    float originX = 0;
    /// Row-major 8-bit alpha coverage, `width * height` bytes.
    std::vector<uint8_t> alpha;
};

/// One registered font, as reported by `TextShaper::fonts()`. Debug/tooling only (the devtools
/// agent's `dump.fonts`, docs/design/20-agent-interface.md §20.5).
struct FontDesc {
    /// The id `match()`/`metrics()`/`glyphOutline()` take.
    FontId id = kInvalidFont;
    /// The family name the font file declares.
    std::string family;
    uint16_t weight = 400;  ///< 100..900
    /// Whether the face is italic/oblique.
    bool italic = false;
    /// True when the font is also a system-fallback candidate (07.3 step 3).
    bool fallback = false;
};

/// Text shaping/font query interface. Implementations are shared across a Context; a real
/// implementation must be re-entrant (§7.3: "all public methods hold the same mutex"), but that
/// requirement is on `xt_text`, not this interface. See docs/design/07-text.md §7.2.
class TextShaper {
public:
    virtual ~TextShaper() = default;

    /// Registers a font from an in-memory font file (the implementation takes ownership of the
    /// bytes). Returns false when this shaper cannot register fonts this way (the default) or the
    /// data could not be parsed. Not part of docs/design/07-text.md §7.2's interface listing: it
    /// is the one hook Document's `@font-face` loading needs (docs/tasks/T-30.md "字型資產"),
    /// added as a defaulted virtual so xt_core never has to know about xt_text.
    virtual bool addFontMemory(std::vector<uint8_t>) { return false; }

    /// Read-only inventory of every registered font, appended to `out` (not cleared). Debug/
    /// tooling hook for the devtools agent's `dump.fonts` (docs/design/20-agent-interface.md
    /// §20.5); the default implementation reports nothing.
    virtual void fonts(std::vector<FontDesc>& out) const { (void)out; }

    /// Finds the best matching font. Must return *some* usable font if any is registered; returns
    /// kInvalidFont only when no font at all is available (layout then treats the text as 0-size).
    virtual FontId match(const FontRequest& request) = 0;

    /// Metrics for a previously matched font (1-em units).
    virtual FontMetrics metrics(FontId font) = 0;

    /// Bidi -> script segmentation -> font-fallback segmentation -> shaping -> line-break
    /// opportunities -> grapheme boundaries, all in one call.
    virtual void shape(std::string_view utf8, const ShapeParams& params, ShapeResult& out) = 0;

    /// Glyph outline in em units (y down). Used by the SW backend and hit testing. Returns false
    /// if the glyph has no outline (e.g. space, or an unsupported font).
    virtual bool glyphOutline(FontId font, uint32_t glyphId, std::vector<PathVerb>& outVerbs,
                              std::vector<float>& outPoints) = 0;

    /// Optional pre-rendered glyph bitmap, rasterized at horizontal subpixel offset `subpixelX`
    /// (px, quantized by the implementation to one of 0/0.25/0.5/0.75). Default returns false (no
    /// bitmap available; backends fall back to the outline).
    virtual bool glyphBitmap(FontId, uint32_t, float, float, GlyphBitmap&) { return false; }

    /// `glyphBitmap` at subpixel offset 0. Kept for callers written before T-101 added the
    /// subpixel parameter; deliberately non-virtual, so an implementation only ever has to
    /// override the five-argument form.
    bool glyphBitmap(FontId font, uint32_t glyphId, float sizePx, GlyphBitmap& out) {
        return glyphBitmap(font, glyphId, sizePx, 0.0f, out);
    }
};

/// A TextShaper with no fonts: match() always returns kInvalidFont, shape() always produces an
/// empty ShapeResult. Used when no real text backend is wired in. See docs/design/07-text.md
/// §7.2.
class NullTextShaper : public TextShaper {
public:
    FontId match(const FontRequest& request) override;
    FontMetrics metrics(FontId font) override;
    void shape(std::string_view utf8, const ShapeParams& params, ShapeResult& out) override;
    bool glyphOutline(FontId font, uint32_t glyphId, std::vector<PathVerb>& outVerbs,
                      std::vector<float>& outPoints) override;
};

}  // namespace xt
