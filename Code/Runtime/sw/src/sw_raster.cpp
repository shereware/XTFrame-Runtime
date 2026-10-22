#include "sw_raster.h"

#include <algorithm>
#include <cmath>

/// See Docs/design/10-backends.md §10.2.1, Docs/tasks/T-11.md "光柵器（sw_raster.h/.cpp）".
///
/// addLine decomposes a line segment into per-(row, column) cell contributions by walking the
/// segment's parameter t in [0, 1] (clipped to the visible row range) and splitting at every row
/// and column boundary it crosses. Because y(t) and x(t) are affine (and y(t) is strictly
/// monotonic since dy != 0), this produces exactly one sub-segment per (row, column) cell the
/// line touches, in a single left-to-right-in-t pass; no separate direction bookkeeping is
/// needed. Per sub-segment: `cover = yEnd - yStart` (signed, 24.8) and
/// `area = cover * (fx1 + fx2)` (fx1/fx2 = cell-local x fractions), which is the ftgrays cell
/// formula (FreeType's gray_render_scanline: `ras.area += (x1 + x2) * delta`). The sweep formula
/// `coverage = winding * 2*ONE_PIXEL - area` (Docs/tasks/T-11.md) is paired with this convention.
///
/// `cover` is derived from a fixed-point (24.8, rounded once via lround) Y computed at every split
/// point `t`, and a cell's cover is the *integer* difference of consecutive fixed points -- not an
/// independently-rounded `lround(yEnd - yStart)` per cell (see Docs/tasks/T-11b.md). Rounding each
/// cell's cover on its own does not telescope: summing many independently-rounded deltas can drift
/// from the true (rounded-once) total by more than the +-0.5 unit any single rounding introduces,
/// which is what let closed contours built from many small flattened-curve segments leave nonzero
/// residual cover in a row. Fixed-point Y differences telescope exactly regardless of how many
/// intermediate split points there are, so a line's total cover contribution to a row always
/// equals lround(yRowExit) - lround(yRowEntry).

namespace xt::sw {

namespace {

/// Coordinate-plus-derivative for one axis of a line segment: value = origin + t * delta.
struct AxisMap {
    double origin;
    double delta;
    double at(double t) const { return origin + t * delta; }
};

}  // namespace

void Rasterizer::reset(int width, int height) {
    m_sweepY0 = 0;
    m_sweepY1 = height;
    if (width != m_width || height != m_height) {
        m_rows.assign(height > 0 ? static_cast<size_t>(height) : size_t{0}, std::vector<Cell>{});
        m_width = width;
        m_height = height;
        m_minRow = 0;
        m_maxRow = -1;
        return;
    }
    // Only the rows the previous pass touched can hold cells, so only those need clearing
    // (Docs/tasks/T-82.md).
    for (int y = m_minRow; y <= m_maxRow; ++y) {
        m_rows[static_cast<size_t>(y)].clear();
    }
    m_minRow = 0;
    m_maxRow = -1;
}

void Rasterizer::addLine(Vec2 p0, Vec2 p1) {
    if (m_width <= 0 || m_height <= 0) {
        return;
    }
    // T-258a: a non-finite endpoint drops the segment (a NaN used to wrap the row / column loops).
    if (!std::isfinite(p0.x) || !std::isfinite(p0.y) || !std::isfinite(p1.x) || !std::isfinite(p1.y)) {
        return;
    }

    // §10.1 74: convert to 24.8 fixed point by scaling by ONE_PIXEL (kept as double here; the
    // per-cell cover/area values are rounded to int32 once a cell's final contribution is known).
    // The deltas stay float differences (as they always were) unless that overflows.
    const float dxf = p1.x - p0.x;
    const float dyf = p1.y - p0.y;
    const double dx = std::isfinite(dxf) ? static_cast<double>(dxf) : static_cast<double>(p1.x) - p0.x;
    const double dy = std::isfinite(dyf) ? static_cast<double>(dyf) : static_cast<double>(p1.y) - p0.y;
    AxisMap ax{static_cast<double>(p0.x) * kOnePixel, dx * kOnePixel};
    AxisMap ay{static_cast<double>(p0.y) * kOnePixel, dy * kOnePixel};

    if (ay.delta == 0.0) {
        return;  // horizontal segments cross no scanline and contribute no winding.
    }

    const double heightFx = static_cast<double>(m_height) * kOnePixel;

    // Visible t-range: the portion of the segment whose y lies in [0, heightFx].
    double tBoundA = (0.0 - ay.origin) / ay.delta;
    double tBoundB = (heightFx - ay.origin) / ay.delta;
    double tLo = std::max(0.0, std::min(tBoundA, tBoundB));
    double tHi = std::min(1.0, std::max(tBoundA, tBoundB));
    if (tLo >= tHi) {
        return;  // entirely above or below the target.
    }

    // T-239 (item 9): member scratch, so a path's thousands of addLine calls allocate nothing.
    std::vector<double>& ts = m_splitScratch;
    ts.clear();
    ts.push_back(tLo);
    ts.push_back(tHi);

    const double yLoV = ay.at(tLo);
    const double yHiV = ay.at(tHi);
    const double yMin = std::min(yLoV, yHiV);
    const double yMax = std::max(yLoV, yHiV);
    const auto rowFirst = static_cast<int64_t>(std::floor(yMin / kOnePixel)) + 1;
    const auto rowLast = static_cast<int64_t>(std::ceil(yMax / kOnePixel)) - 1;
    for (int64_t r = rowFirst; r <= rowLast; ++r) {
        double boundaryY = static_cast<double>(r) * kOnePixel;
        double t = (boundaryY - ay.origin) / ay.delta;
        if (t > tLo && t < tHi) {
            ts.push_back(t);
        }
    }

    if (ax.delta != 0.0) {
        const double xLoV = ax.at(tLo);
        const double xHiV = ax.at(tHi);
        const double xMin = std::min(xLoV, xHiV);
        const double xMax = std::max(xLoV, xHiV);
        // T-258a: only the column boundaries in [-1, width] are split at -- cells outside are
        // clamped to -1 / width anyway -- so a huge x extent no longer means one cell per column.
        const double colLimit = static_cast<double>(m_width);
        const auto colFirst =
            static_cast<int64_t>(std::clamp(std::floor(xMin / kOnePixel) + 1.0, -1.0, colLimit));
        const auto colLast =
            static_cast<int64_t>(std::clamp(std::ceil(xMax / kOnePixel) - 1.0, -1.0, colLimit));
        for (int64_t c = colFirst; c <= colLast; ++c) {
            double boundaryX = static_cast<double>(c) * kOnePixel;
            double t = (boundaryX - ax.origin) / ax.delta;
            if (t > tLo && t < tHi) {
                ts.push_back(t);
            }
        }
    }

    std::sort(ts.begin(), ts.end());

    // Merge split points that coincide (within float noise): row/col boundary crossings can land
    // on top of tLo/tHi or each other, and keeping both as distinct cells would make the cover
    // split below depend on which near-duplicate happens to be first.
    ts.erase(std::unique(ts.begin(), ts.end(), [](double a, double b) { return b - a < 1e-9; }), ts.end());
    if (ts.size() < 2) {
        return;
    }

    // Fixed-point (24.8) Y at every split point, rounded exactly once (see the file-level comment
    // above): a cell's cover is the integer difference of consecutive entries, so summing all of
    // this line's cells telescopes exactly to yFixed.back() - yFixed.front() no matter how many
    // cells the line was split into.
    //
    // At the segment's true endpoints (t == 0 or t == 1) the Y is taken directly from p0.y/p1.y
    // rather than through `ay.at(t)` (== ay.origin + t * ay.delta): floating-point addition is not
    // exactly associative, so `origin + 1.0*delta` can differ from `p1.y * kOnePixel` in the low
    // bit even though they are mathematically equal. Two contour edges sharing a vertex (this
    // segment's p1 == the next segment's p0) must round that shared Y to the *same* integer, or the
    // per-row cover sum stops telescoping to 0 across the contour; using the direct multiply on
    // both sides guarantees bit-identical input to lround.
    std::vector<int32_t>& yFixed = m_yFixedScratch;
    yFixed.resize(ts.size());
    for (size_t i = 0; i < ts.size(); ++i) {
        const double t = ts[i];
        double y;
        if (t <= 0.0) {
            y = static_cast<double>(p0.y) * kOnePixel;
        } else if (t >= 1.0) {
            y = static_cast<double>(p1.y) * kOnePixel;
        } else {
            y = ay.at(t);
        }
        yFixed[i] = static_cast<int32_t>(std::lround(y));
    }

    // T-258a: x is clamped to [-2, width + 1] px. Every sub-segment inside the target already lies
    // within it; one outside lands in the clamped column -1 / width, whose area is never swept.
    const double xClampLo = -2.0 * kOnePixel;
    const double xClampHi = (static_cast<double>(m_width) + 1.0) * kOnePixel;
    for (size_t i = 0; i + 1 < ts.size(); ++i) {
        const double tA = ts[i];
        const double tB = ts[i + 1];
        const double xa = std::clamp(ax.at(tA), xClampLo, xClampHi);
        const double xb = std::clamp(ax.at(tB), xClampLo, xClampHi);
        const double ymid = (ay.at(tA) + ay.at(tB)) * 0.5;
        const double xmid = (xa + xb) * 0.5;

        auto row = static_cast<int64_t>(std::floor(ymid / kOnePixel));
        row = std::clamp<int64_t>(row, 0, m_height - 1);  // guard against float rounding at edges.

        auto trueCol = static_cast<int64_t>(std::floor(xmid / kOnePixel));
        int32_t clampedCol = static_cast<int32_t>(std::clamp<int64_t>(trueCol, -1, m_width));

        const double fx1 = xa - static_cast<double>(trueCol) * kOnePixel;
        const double fx2 = xb - static_cast<double>(trueCol) * kOnePixel;
        const int32_t cover = yFixed[i + 1] - yFixed[i];
        const double area = (fx1 + fx2) * static_cast<double>(cover);

        Cell cell{};
        cell.x = clampedCol;
        cell.cover = cover;
        cell.area = static_cast<int32_t>(std::lround(area));
        m_rows[static_cast<size_t>(row)].push_back(cell);
        const int rowI = static_cast<int>(row);
        if (m_minRow > m_maxRow) {
            m_minRow = rowI;
            m_maxRow = rowI;
        } else {
            m_minRow = std::min(m_minRow, rowI);
            m_maxRow = std::max(m_maxRow, rowI);
        }
    }
}

void Rasterizer::sortAndMergeRow(std::vector<Cell>& row) {
    std::sort(row.begin(), row.end(), [](const Cell& a, const Cell& b) { return a.x < b.x; });
    size_t writeIdx = 0;
    for (size_t readIdx = 0; readIdx < row.size(); ++readIdx) {
        if (writeIdx > 0 && row[writeIdx - 1].x == row[readIdx].x) {
            row[writeIdx - 1].cover += row[readIdx].cover;
            row[writeIdx - 1].area += row[readIdx].area;
        } else {
            row[writeIdx] = row[readIdx];
            ++writeIdx;
        }
    }
    row.resize(writeIdx);
}

uint8_t Rasterizer::mapCoverage(FillRule rule, int32_t c) {
    if (rule == FillRule::NonZero) {
        int32_t v = std::min<int32_t>(255, std::abs(c));
        return static_cast<uint8_t>(v);
    }
    // EvenOdd: fold into a 512-wide triangle wave, then clamp to 255.
    uint32_t m = static_cast<uint32_t>(c) & 511u;
    int32_t v = m > 256u ? static_cast<int32_t>(512u - m) : static_cast<int32_t>(m);
    return static_cast<uint8_t>(std::min(255, v));
}

}  // namespace xt::sw
