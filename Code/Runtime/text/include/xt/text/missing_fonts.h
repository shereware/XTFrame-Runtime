#pragma once

#include <string>
#include <vector>

#include "xt/text_shaper.h"

namespace xt {
class Document;
}

namespace xt::text {

/// T-569 (Docs/design/07-text.md §7.20, report R4): for every text node of `doc` with visible text,
/// the `font-family` list its parent element resolved, and of each list the first CJK family when
/// it is not one of `shaper`'s registered families (TextShaper::fonts(); a family is CJK by its name
/// or its FontDesc::cjk coverage, font_name_table.h firstMissingCjkFamily()). Distinct, in node
/// order. A shaper that lists no fonts (NullTextShaper) reports nothing.
/// Needs resolved styles: call it after the document's first update. xtframe-view and xtframe-pack
/// print each one as `missing font '<family>'`.
std::vector<std::string> missingCjkFamilies(const Document& doc, const TextShaper& shaper);

}  // namespace xt::text
