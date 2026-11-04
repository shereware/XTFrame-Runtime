#include "xts_sections.h"

#include <algorithm>
#include <iterator>
#include <string_view>

namespace xt::compile {

namespace {

#include "xts_section_table.inc"

}  // namespace

bool isLayoutSectionProperty(std::string_view name) {
    return std::binary_search(std::begin(kLayoutSectionProps), std::end(kLayoutSectionProps), name);
}

bool isLayoutShorthandProperty(std::string_view name) {
    return std::binary_search(std::begin(kLayoutShorthandProps), std::end(kLayoutShorthandProps), name);
}

}  // namespace xt::compile
