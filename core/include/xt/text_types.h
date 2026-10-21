#pragma once

#include <cstdint>

/// Text-related enums shared by ComputedStyle, TextShaper and the line-layout algorithm.
/// See docs/design/07-text.md §7.2, docs/tasks/T-28.md.
///
/// TextAlign/WhiteSpace/TextOverflow are not yet defined elsewhere in core as of this task
/// (T-24, which adds style-side enums, is running in parallel and may add its own copies in
/// node.h). If T-24 already defines them by the time these branches merge, this file's copies
/// should be removed and callers should use T-24's instead; the numbering here matches what was
/// requested for T-28 so the two can be merged with a simple rename/dedupe.

namespace xt {

/// Paragraph/base direction. See docs/design/07-text.md §7.2.
enum class TextDirection : uint8_t {
    Ltr = 0,  ///< Left-to-right paragraph base direction.
    Rtl = 1,  ///< Right-to-left paragraph base direction.
};

/// See docs/design/07-text.md §7.2.
enum class WritingMode : uint8_t {
    HorizontalTb = 0,  ///< Horizontal lines, top-to-bottom block progression (default).
    VerticalRl = 1,    ///< Vertical lines/columns, columns flow right-to-left.
    VerticalLr = 2,    ///< Vertical lines/columns, columns flow left-to-right.
};

/// See docs/design/07-text.md §7.2.
enum class TextOrientation : uint8_t {
    Mixed =
        0,  ///< Glyph orientation follows Unicode Vertical_Orientation (upright or rotated per codepoint).
    Upright = 1,   ///< All glyphs drawn upright regardless of script.
    Sideways = 2,  ///< All glyphs rotated 90 degrees clockwise to run along the vertical line.
};

/// CSS text-align, resolved against `direction` for Start/End. See docs/tasks/T-28.md.
enum class TextAlign : uint8_t {
    Start = 0,   ///< Aligns to the line-start edge, resolved from `direction` (left for Ltr, right for Rtl).
    End = 1,     ///< Aligns to the line-end edge, resolved from `direction` (right for Ltr, left for Rtl).
    Left = 2,    ///< Always aligns to the left edge, regardless of direction.
    Center = 3,  ///< Centers the line.
    Right = 4,   ///< Always aligns to the right edge, regardless of direction.
};

/// CSS white-space (subset). See docs/tasks/T-28.md.
enum class WhiteSpace : uint8_t {
    Normal = 0,  ///< Collapses whitespace and wraps lines normally.
    NoWrap = 1,  ///< Collapses whitespace but never wraps onto a new line.
};

/// CSS text-overflow (subset). See docs/tasks/T-28.md.
enum class TextOverflow : uint8_t {
    Clip = 0,      ///< Clips overflowing text at the content box edge.
    Ellipsis = 1,  ///< Truncates overflowing text and appends an ellipsis.
};

/// CSS `text-transform` (StyleProp 202, docs/design/05-style-and-css.md §5.9.8, T-323): inherited,
/// not animatable. Applied to a text node's whole string before shaping; the case tables cover
/// ASCII, Latin-1 Supplement and Latin Extended-A only (core/src/text_case.h).
enum class TextTransform : uint8_t {
    None = 0,        ///< The text is shaped as written.
    Uppercase = 1,   ///< Every letter in the tables is upper-cased.
    Lowercase = 2,   ///< Every letter in the tables is lower-cased.
    Capitalize = 3,  ///< The first code point of each word is upper-cased; the rest is unchanged.
};

}  // namespace xt
