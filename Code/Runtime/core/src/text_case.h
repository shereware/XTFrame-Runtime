#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "xt/text_types.h"

/// CSS `text-transform` case mapping (Docs/design/05-style-and-css.md §5.9.8, Docs/tasks/T-323.md
/// item 2). A private (Code/Runtime/core/src) header. The tables are the Unicode simple case mappings of
/// U+0000-U+017F (ASCII, Latin-1 Supplement, Latin Extended-A), written into this library so core
/// keeps zero dependencies; every other code point is left as it is. A mapping that would change
/// the UTF-8 length is left out (U+0130 to lower case, U+0131 and U+017F to upper case), so a
/// transformed string always has the length of its source and every byte offset into it -- style
/// runs, inline objects, `text-reveal` -- stays valid.

namespace xt::text_case {

/// Entries per table: U+0000-U+017F.
inline constexpr uint32_t kTableSize = 0x180;

/// The simple upper-case mapping of `cp`, or `cp` itself (outside the tables, or no mapping).
uint32_t toUpper(uint32_t cp);

/// The simple lower-case mapping of `cp`, or `cp` itself (outside the tables, or no mapping).
uint32_t toLower(uint32_t cp);

/// True when a word starts right after `cp` for `capitalize`: Unicode White_Space or ASCII
/// punctuation.
bool isWordSeparator(uint32_t cp);

/// Writes `text` with `transform` applied to `out` (replacing its contents); `out.size()` always
/// equals `text.size()`. `capitalize` upper-cases the code point at the start of each word (the
/// start of the text, or right after a separator) and leaves the rest as it is. Malformed UTF-8
/// bytes are copied unchanged.
void apply(std::string_view text, TextTransform transform, std::string& out);

}  // namespace xt::text_case
