#pragma once

#include <cstddef>
#include <string_view>

#include "xt/style_template.h"

/// T-477: the halves of StyleTemplate that ElementRef::setStyleTemplate() (element_ref.cpp) uses;
/// implemented in style_template.cpp.

namespace xt {

/// Reads the hole numbers of `text`, a completion of `t` (its literals, in order, around one CSS
/// number per hole, as the setStyle() tokenizer reads them), into `numbers` (t.holeCount()
/// entries). False when `text` has another shape or a number is outside its hole's domain.
bool readStyleTemplateNumbers(const StyleTemplate& t, std::string_view text, float* numbers);

/// Declaration `decl` of `t` with the hole numbers written into its fields.
void fillStyleTemplateValue(const StyleTemplate& t, size_t decl, const float* numbers, Value& out);

}  // namespace xt
