#pragma once

#include <cstdint>

#include "xt/coverage_raster.h"

/// The software renderer's rasterizer is xt_core's CoverageRasterizer (Docs/tasks/T-660a.md: moved
/// to xt_core so the GPU coverage atlas rasterizes with the same code). These aliases keep the
/// renderer's existing names. See Docs/design/10-backends.md §10.2.1, §10.11 item 2.

namespace xt::sw {

/// Fixed-point scale used throughout: 24.8, i.e. one pixel == 256 subpixel units.
inline constexpr int32_t kOnePixel = kCoverageOnePixel;

/// One sparse-row cell; see xt::CoverageCell.
using Cell = CoverageCell;

/// The signed-area coverage rasterizer; see xt::CoverageRasterizer.
using Rasterizer = CoverageRasterizer;

}  // namespace xt::sw
