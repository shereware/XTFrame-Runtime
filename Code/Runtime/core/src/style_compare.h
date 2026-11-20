#pragma once

#include "xt/node.h"

/// T-753 (Docs/design/57-measurement.md §57.3 item 4, `redundantWork`): field-by-field equality of two
/// ComputedStyles, for counting the re-styled elements whose computed style did not change. Only called
/// while the PerfTracker's detail level is >= 1.

namespace xt {

/// True when every field of `a` equals the same field of `b` (floats with ==, the `base` values with
/// valueEquals()). Padding is never read.
bool sameComputedStyle(const ComputedStyle& a, const ComputedStyle& b);

}  // namespace xt
