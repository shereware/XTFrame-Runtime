#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "xt/text_shaper.h"
#include "xt/text_types.h"
#include "xt/types.h"

/// Pure, font-library-free line/column layout on top of a TextShaper::shape() result: greedy
/// line breaking, UAX#9 L2 visual reordering, text-align, text-overflow: ellipsis, and vertical
/// (column) writing modes. See Docs/design/07-text.md §7.2/§7.5, Docs/design/06-layout.md §6.8,
/// Docs/tasks/T-28.md.

namespace xt {

/// A shaped, positioned run of glyphs, ready to paint. See Docs/design/07-text.md §7.2.
struct GlyphRun {
    /// Font this run's glyphs are shaped/positioned in.
    FontId font = kInvalidFont;
    uint8_t bidiLevel = 0;  ///< UAX#9 embedding level of the source ShapedRun; odd = RTL
    uint8_t vertical = 0;   ///< 1 = upright vertical run (positions advance along y)
    uint8_t rotated = 0;    ///< 1 = horizontal run rotated 90deg cw for vertical writing-mode
    /// Glyph ids, in visual order.
    std::vector<uint32_t> glyphs;
    /// Relative to the text node's top-left. For horizontal runs this is the glyph's baseline
    /// origin; for vertical runs (upright or rotated) this is the origin on the column's center
    /// line.
    std::vector<Vec2> positions;
    /// Byte offset (into the source text) of this run's first byte.
    uint32_t byteStart = 0;
    /// Byte offset (into the source text) one past this run's last byte.
    uint32_t byteEnd = 0;
    Rect bounds{};  ///< this run's box within the text node, for hit-testing/selection
    /// T-178: TextRunInfo::tag of the source ShapedRun (the rich-text run index), or kNoRunTag.
    uint32_t tag = 0xFFFFFFFFu;
    /// T-178: font size (px) of this run; 0 = the parent element's font size.
    float fontSize = 0;
    /// T-178: per glyph (parallel to `glyphs`), the UTF-8 byte offset of the glyph's cluster;
    /// `text-reveal` hides glyphs whose cluster lies past the revealed prefix.
    std::vector<uint32_t> clusters;
};

/// "No rich-text run" value of GlyphRun::tag / TextRunInfo::tag.
inline constexpr uint32_t kNoRunTag = 0xFFFFFFFFu;

/// T-178 (Docs/design/07-text.md §7.8, 06-layout.md §6.8): per-ShapedRun metrics of a rich-text
/// layout. A line's height is the largest `lineHeight` of the runs on it and its baseline uses
/// the largest ascent/descent, per §6.8's "以行內最大字型度量".
struct TextRunInfo {
    float ascent = 0;          ///< px
    float descent = 0;         ///< px (same sign convention as TextLayoutParams::descent)
    float lineHeight = 0;      ///< px, already resolved
    float fontSize = 0;        ///< px; copied to GlyphRun::fontSize
    uint32_t tag = kNoRunTag;  ///< copied to GlyphRun::tag; runs with different tags never merge
};

/// One laid-out line (horizontal writing-mode) or column (vertical writing-mode).
struct TextLine {
    /// This line's/column's box, relative to the text node's top-left.
    Rect rect{};
    float baseline = 0;  ///< meaningless (0) for vertical writing-modes
    /// Index of this line's first run in TextLayoutResult::runs.
    uint32_t firstRun = 0;
    /// Number of consecutive runs (starting at `firstRun`) belonging to this line.
    uint32_t runCount = 0;
    /// Byte offset (into the source text) of this line's first byte.
    uint32_t byteStart = 0;
    /// Byte offset (into the source text) one past this line's last byte.
    uint32_t byteEnd = 0;
};

/// Input to layoutText(). See Docs/tasks/T-28.md.
struct TextLayoutParams {
    float availMain = 0;   ///< available main-axis length: width if horizontal, height if vertical
    float fontSize = 0;    ///< px
    float lineHeight = 0;  ///< px, already resolved
    float ascent = 0;      ///< px
    float descent = 0;     ///< px
    /// CSS text-align.
    TextAlign textAlign = TextAlign::Start;
    /// CSS white-space (subset).
    WhiteSpace whiteSpace = WhiteSpace::Normal;
    /// CSS text-overflow (subset).
    TextOverflow textOverflow = TextOverflow::Clip;
    /// Paragraph/base direction.
    TextDirection direction = TextDirection::Ltr;
    /// Horizontal vs. vertical writing mode.
    WritingMode writingMode = WritingMode::HorizontalTb;
    /// Advance (px) of a shaped U+0026's HORIZONTAL ELLIPSIS (U+2026) glyph, pre-shaped by the
    /// caller; <= 0 means ellipsis is unavailable and text-overflow: ellipsis has no effect.
    float ellipsisAdvance = 0;
    /// Glyph id of the pre-shaped ellipsis glyph (paired with `ellipsisAdvance`).
    uint32_t ellipsisGlyph = 0;
    /// Font the pre-shaped ellipsis glyph belongs to.
    FontId ellipsisFont = kInvalidFont;
    /// CSS `line-clamp` (05 §5.9.8, T-331): at most this many lines; 0 = none. When text
    /// follows the last kept line, that line ends in the ellipsis (same trimming as the nowrap
    /// `text-overflow: ellipsis` rule, at the line's visual end). Only with soft wrapping; the
    /// caller passes 1 for `nowrap` + `text-overflow: ellipsis` and 0 for any other `nowrap`.
    uint32_t lineClamp = 0;
    /// T-178: one entry per ShapeResult::runs[i] for rich text; empty = every run uses the
    /// uniform `fontSize`/`lineHeight`/`ascent`/`descent` above.
    std::span<const TextRunInfo> runInfo;
};

/// Output of layoutText().
struct TextLayoutResult {
    /// Every positioned glyph run, grouped by line (see TextLine::firstRun/runCount).
    std::vector<GlyphRun> runs;
    /// Laid-out lines (horizontal writing-mode) or columns (vertical writing-mode), in order.
    std::vector<TextLine> lines;
    float mainSize = 0;   ///< width if horizontal, height if vertical
    float crossSize = 0;  ///< height if horizontal, width if vertical
    /// Main-axis size the text would take with no wrapping (still honors forced breaks, i.e.
    /// breakAfter == 2): the longest such unwrapped segment.
    float intrinsicMain = 0;
};

/// Lays out already-shaped text (logical order) into lines/columns and positions every glyph.
/// Pure function: no side effects beyond `out`, no allocation outside of `out`'s containers, no
/// recursion. See Docs/design/06-layout.md §6.8.
void layoutText(const ShapeResult& shaped, const TextLayoutParams& params, TextLayoutResult& out);

/// UAX#9 rule L2: given the embedding level of each item in logical order, produces the
/// left-to-right visual order as indices into `levels`. From the highest level down to the
/// lowest odd level, each maximal run of items with level >= that level is reversed in place.
void bidiReorder(std::span<const uint8_t> levels, std::vector<uint32_t>& outVisualOrder);

}  // namespace xt
