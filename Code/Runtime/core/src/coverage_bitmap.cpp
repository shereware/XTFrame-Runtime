#include "xt/coverage_bitmap.h"

#include <cmath>

#include "xt/geom.h"

/// See Docs/design/10-backends.md §10.11 item 2, Docs/tasks/T-660a.md item 2.
///
/// The shape is flattened exactly as SW's fillPath does (flattenPath, device transform, tolerance
/// kFlattenTolerance) and then moved by the integer bitmap origin. For a bitmap origin >= 0 and
/// coordinates below 2^24 that float subtraction is exact (the result is a multiple of the point's
/// own ulp and no larger than the point), so the rasterizer's row/column splits see the same
/// fractions SW's full-target rasterization sees and the coverage values come out the same.

namespace xt {

namespace {

/// Bitmap origins beyond this magnitude are refused, keeping every float -> int conversion and
/// the origin subtraction well inside float's exact-integer range.
constexpr float kMaxCoverageCoord = 8388608.0f;  // 2^23

/// Rasterizes the device-space contours already in `out.flatPoints`/`out.flatStarts` (moved into the
/// bitmap in place); the body shared by rasterizeCoverage and rasterizeCoverageContours.
bool rasterizeFlattened(FillRule rule, CoverageBitmap& out) {
    bool any = false;
    float minX = 0, minY = 0, maxX = 0, maxY = 0;
    for (const Vec2& p : out.flatPoints) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
            continue;
        }
        if (!any) {
            minX = maxX = p.x;
            minY = maxY = p.y;
            any = true;
            continue;
        }
        minX = std::fmin(minX, p.x);
        maxX = std::fmax(maxX, p.x);
        minY = std::fmin(minY, p.y);
        maxY = std::fmax(maxY, p.y);
    }
    if (!any || !(maxX > minX) || !(maxY > minY)) {
        return false;  // nothing finite, or a zero-area box.
    }
    if (std::fabs(minX) > kMaxCoverageCoord || std::fabs(maxX) > kMaxCoverageCoord ||
        std::fabs(minY) > kMaxCoverageCoord || std::fabs(maxY) > kMaxCoverageCoord) {
        return false;
    }
    const float x0 = std::floor(minX) - 1.0f;
    const float y0 = std::floor(minY) - 1.0f;
    const float x1 = std::ceil(maxX) + 1.0f;
    const float y1 = std::ceil(maxY) + 1.0f;
    if (x1 - x0 > static_cast<float>(kMaxCoverageBitmapSide) ||
        y1 - y0 > static_cast<float>(kMaxCoverageBitmapSide)) {
        return false;
    }
    const int32_t w = static_cast<int32_t>(x1 - x0);
    const int32_t h = static_cast<int32_t>(y1 - y0);

    for (Vec2& p : out.flatPoints) {
        p.x -= x0;
        p.y -= y0;
    }

    out.width = w;
    out.height = h;
    out.originX = static_cast<int32_t>(x0);
    out.originY = static_cast<int32_t>(y0);
    out.pixels.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0);

    // Same contour closing as SW's addClosedContours (Code/Runtime/sw/src/sw_renderer.cpp).
    CoverageRasterizer& raster = out.raster;
    raster.reset(w, h);
    const std::vector<Vec2>& pts = out.flatPoints;
    const std::vector<uint32_t>& starts = out.flatStarts;
    for (size_t i = 0; i < starts.size(); ++i) {
        const uint32_t start = starts[i];
        const uint32_t end = (i + 1 < starts.size()) ? starts[i + 1] : static_cast<uint32_t>(pts.size());
        if (end - start < 2) {
            continue;  // a single-point contour has no edges to fill.
        }
        for (uint32_t j = start; j + 1 < end; ++j) {
            raster.addLine(pts[j], pts[j + 1]);
        }
        raster.addLine(pts[end - 1], pts[start]);
    }
    uint8_t* const base = out.pixels.data();
    const size_t stride = static_cast<size_t>(w);
    uint64_t covered = 0;
    raster.sweep(rule, [&](int y, int xa, int xb, uint8_t coverage) {
        if (coverage != 0) {
            covered += static_cast<uint64_t>(xb - xa);
        }
        uint8_t* row = base + static_cast<size_t>(y) * stride;
        for (int x = xa; x < xb; ++x) {
            row[x] = coverage;
        }
    });
    out.coveredPixels = covered;
    return true;
}

}  // namespace

bool rasterizeCoverage(std::span<const PathVerb> verbs, std::span<const float> points,
                       const Mat2x3& deviceFromLocal, FillRule rule, CoverageBitmap& out) {
    out.flatPoints.clear();
    out.flatStarts.clear();
    out.flatClosed.clear();
    flattenPath(verbs, points, deviceFromLocal, kFlattenTolerance, out.flatPoints, out.flatStarts,
                out.flatClosed);
    return rasterizeFlattened(rule, out);
}

bool rasterizeCoverageContours(std::span<const Vec2> contours, std::span<const uint32_t> contourStarts,
                               FillRule rule, CoverageBitmap& out) {
    out.flatPoints.assign(contours.begin(), contours.end());
    out.flatStarts.assign(contourStarts.begin(), contourStarts.end());
    out.flatClosed.clear();
    return rasterizeFlattened(rule, out);
}

}  // namespace xt
