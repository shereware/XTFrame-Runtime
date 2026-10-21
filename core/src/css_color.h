#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "xt/types.h"

/// CSS colour helpers shared by the compiler's `.xts` value parser (compile/src/xts/xts_values.cpp)
/// and the runtime mini CSS parser (core/src/mini_css.cpp), so both read named colours and `hsl()`
/// identically (docs/tasks/T-322.md items 1-2). A private (core/src) header, like
/// transform_compose.h. Pure functions: no allocation, no diagnostics.

namespace xt::css_color {

/// Number of entries in the CSS Color Level 4 named-colour table (`transparent` and
/// `currentcolor` are not part of it).
inline constexpr size_t kNamedColorCount = 148;

/// Looks `name` up in the CSS Color Level 4 named-colour table (ASCII case-insensitive, binary
/// search over the sorted table). Returns false for an unknown name; `out` is then untouched.
bool findNamedColor(std::string_view name, Color& out);

/// The `index`-th table entry (sorted by name), for tests; false past the end.
bool namedColorAt(size_t index, std::string_view& name, uint32_t& rgb);

/// CSS `hsl()` to sRGB: `hueDeg` in degrees (any value, wrapped into [0, 360)), `sat` and `light`
/// as fractions (clamped to [0, 1]), `alpha` clamped to [0, 1]. Each colour channel is rounded to
/// 8 bits, the precision of the hex and named forms; alpha is kept as is.
Color hslToColor(float hueDeg, float sat, float light, float alpha);

}  // namespace xt::css_color
