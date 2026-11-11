#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "xt/coverage_raster.h"
#include "xt/node.h"
#include "xt/types.h"

/// Path -> R8 coverage bitmap, rasterized with the software renderer's own flattening and
/// rasterizer so the GPU coverage atlas holds the exact coverage SW fills with.
/// See Docs/design/10-backends.md §10.11 item 2, Docs/tasks/T-660a.md item 2.

namespace xt {

/// Largest bitmap side rasterizeCoverage produces, in device pixels (§10.11 item 2; raised from
/// 1024 by the large-shape tiling supplement, T-672).
inline constexpr int32_t kMaxCoverageBitmapSide = 4096;

/// A rasterized coverage bitmap plus the scratch that produced it. Caller-owned and reusable: a
/// second rasterizeCoverage into the same object reuses every buffer's capacity.
struct CoverageBitmap {
    /// `width * height` coverage values (0..255), row-major, no row padding.
    std::vector<uint8_t> pixels;
    /// Bitmap width in pixels (includes the 1 px transparent border on each side).
    int32_t width = 0;
    /// Bitmap height in pixels (includes the 1 px transparent border on each side).
    int32_t height = 0;
    /// Device x of the bitmap's left column.
    int32_t originX = 0;
    /// Device y of the bitmap's top row.
    int32_t originY = 0;
    /// T-662 (§10.11 item 2, supplement 2): pixels the rasterizer gave a nonzero coverage, summed
    /// over the spans of every row while the bitmap was written -- the work the frame budget
    /// counts (a long thin diagonal has a large box but few of these).
    uint64_t coveredPixels = 0;

    /// Scratch: the flattened, bitmap-local contours.
    std::vector<Vec2> flatPoints;
    /// Scratch: contour starts into flatPoints (flattenPath's layout).
    std::vector<uint32_t> flatStarts;
    /// Scratch: flattenPath's per-contour closed flags (unused: fills close every contour).
    std::vector<uint8_t> flatClosed;
    /// Scratch: the rasterizer.
    CoverageRasterizer raster;
};

/// Rasterizes the path (`verbs`/`points` in the §3.4 layout, as CommandList pools hold it) under
/// `deviceFromLocal` into `out`: flattened with flattenPath at kFlattenTolerance (what SW's
/// fillPath uses without a 3D projection), every contour closed, rasterized by CoverageRasterizer
/// with `rule`. The bitmap covers the flattened shape's device box rounded outward plus a 1 px
/// transparent border on each side; `out.originX/originY` is its top-left device pixel. Each value
/// equals the coverage SW's span loop receives for that device pixel.
/// Returns false, leaving `out.pixels` untouched, when the shape has no finite point, its box has
/// zero width or height, or either bitmap side would exceed kMaxCoverageBitmapSide.
bool rasterizeCoverage(std::span<const PathVerb> verbs, std::span<const float> points,
                       const Mat2x3& deviceFromLocal, FillRule rule, CoverageBitmap& out);

/// T-661 (§10.11 item 3): rasterizes contours that are already flattened and in device space
/// (`contours`/`contourStarts` in flattenPath's layout, e.g. strokeToFill's output) into `out`
/// with `rule`, every contour closed -- the same box, border, origin and coverage values as
/// rasterizeCoverage after its flattening step (what SW's paintContours does with those contours).
/// Returns false under the same conditions as rasterizeCoverage.
bool rasterizeCoverageContours(std::span<const Vec2> contours, std::span<const uint32_t> contourStarts,
                               FillRule rule, CoverageBitmap& out);

}  // namespace xt
