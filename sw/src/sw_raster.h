#pragma once

#include <cstdint>
#include <vector>

#include "xt/node.h"
#include "xt/types.h"

/// Internal signed-area coverage rasterizer (FreeType ftgrays cell model). See
/// docs/design/10-backends.md §10.2.1, docs/tasks/T-11.md "光柵器（sw_raster.h/.cpp）".

namespace xt::sw {

/// Fixed-point scale used throughout: 24.8, i.e. one pixel == 256 subpixel units.
inline constexpr int32_t kOnePixel = 256;

/// One sparse-row cell, same semantics as FreeType's ftgrays: `cover` is the signed vertical
/// extent (24.8 fixed point) that line segments contributed while passing through this cell's
/// column on this row; `area` is `cover * (fx1 + fx2)`, where fx1/fx2 are the cell-local x
/// fractions (24.8, in [0, kOnePixel]) of each segment's entry/exit point. Multiple cells sharing
/// the same (row, x) are summed together at sweep time.
struct Cell {
    int32_t x;
    int32_t cover;
    int32_t area;
};

/// Accumulates transformed, device-space line segments into per-row sparse cell lists, then
/// sweeps each row left to right to produce per-pixel coverage spans (0..255).
class Rasterizer {
public:
    /// Clears all rows. Capacity of the row storage is retained when `width`/`height` are
    /// unchanged from the previous reset.
    void reset(int width, int height);

    /// Restricts the next `sweep` to rows in `[y0, y1)` (T-133: the caller already knows the clip
    /// box the spans would survive, and a row outside it produces nothing but discarded work).
    /// `reset` restores the full `[0, height)` range, so this must be called after it.
    void setSweepRows(int y0, int y1) {
        m_sweepY0 = y0;
        m_sweepY1 = y1;
    }

    /// Adds one line segment (already transformed to device space, i.e. xf and DPR applied).
    /// Rows outside [0, height) are clipped away (the sub-segment inside is kept, interpolated at
    /// the boundary); the cell's x index is clamped to [-1, width] so off-screen contributions
    /// still affect winding continuity without ever emitting a coverage value there.
    void addLine(Vec2 p0, Vec2 p1);

    /// Only rows in `[minRow(), maxRow()]` can hold cells; the range is empty when `minRow() >
    /// maxRow()` (nothing was added). Tracked by addLine so a sweep never walks the whole viewport
    /// for a small shape (docs/tasks/T-82.md).
    int minRow() const { return m_minRow; }
    /// Last row addLine touched; see minRow().
    int maxRow() const { return m_maxRow; }

    /// Sweeps every row: sorts and merges same-x cells, accumulates winding left to right, and
    /// invokes `span(y, x0, x1, coverage)` (0 <= x0 < x1 <= width, coverage in [0, 255]) for every
    /// non-empty span. Gaps between cells are filled with the running winding's coverage.
    /// Does not mutate the accumulated cells (safe to call once after a set of addLine calls).
    template <class SpanFn>
    void sweep(FillRule rule, SpanFn&& span) const {
        // Untouched rows produce no spans, so the sweep starts and stops at the rows addLine
        // actually wrote to, and the sorted copy of a row reuses one scratch buffer instead of
        // allocating per row (docs/tasks/T-82.md).
        const int first = m_minRow > m_sweepY0 ? m_minRow : m_sweepY0;
        const int last = m_maxRow < m_sweepY1 - 1 ? m_maxRow : m_sweepY1 - 1;
        for (int y = first; y <= last; ++y) {
            std::vector<Cell>& row = m_sortScratch;
            row.assign(m_rows[static_cast<size_t>(y)].begin(), m_rows[static_cast<size_t>(y)].end());
            sortAndMergeRow(row);
            int32_t winding = 0;
            int prevX = 0;
            for (const Cell& cell : row) {
                int gapEnd = cell.x < 0 ? 0 : (cell.x > m_width ? m_width : static_cast<int>(cell.x));
                if (gapEnd > prevX && winding != 0) {
                    span(y, prevX, gapEnd, mapCoverage(rule, winding));
                }
                if (gapEnd > prevX) {
                    prevX = gapEnd;
                }
                winding += cell.cover;
                if (cell.x >= 0 && cell.x < m_width) {
                    int64_t raw = static_cast<int64_t>(winding) * (2 * kOnePixel) - cell.area;
                    auto cellCoverage = static_cast<int32_t>(raw / (2 * kOnePixel));
                    span(y, static_cast<int>(cell.x), static_cast<int>(cell.x) + 1,
                         mapCoverage(rule, cellCoverage));
                    prevX = static_cast<int>(cell.x) + 1;
                }
            }
            if (m_width > prevX && winding != 0) {
                span(y, prevX, m_width, mapCoverage(rule, winding));
            }
        }
    }

    /// Capacity of one row's cell storage; exposed only for tests verifying that `reset` retains
    /// capacity when called again with the same dimensions.
    size_t rowCapacity(int row) const { return m_rows[static_cast<size_t>(row)].capacity(); }

    /// Sum of every accumulated cell's `cover` on row `y`, before sort/merge (summation is
    /// order-independent). For any closed contour (every addLine call paired so each subpath's
    /// last point connects back to its first), this must be exactly 0 for every row: a closed
    /// curve's net vertical crossing count, integrated over all x, is 0. Exposed only for tests
    /// asserting that invariant (docs/tasks/T-11b.md).
    int64_t debugRowCoverSum(int y) const {
        int64_t sum = 0;
        for (const Cell& cell : m_rows[static_cast<size_t>(y)]) {
            sum += cell.cover;
        }
        return sum;
    }

private:
    static void sortAndMergeRow(std::vector<Cell>& row);
    static uint8_t mapCoverage(FillRule rule, int32_t c);

    std::vector<std::vector<Cell>> m_rows;
    int m_width = 0;
    int m_height = 0;
    // Inclusive row range addLine has touched since the last reset; m_minRow > m_maxRow means none.
    int m_minRow = 0;
    int m_maxRow = -1;
    // Row window `sweep` is restricted to; reset() sets it back to the full target (setSweepRows).
    int m_sweepY0 = 0;
    int m_sweepY1 = 0;
    // Scratch for sweep's sorted copy of one row; mutable because sweep is const.
    mutable std::vector<Cell> m_sortScratch;
    /// addLine's split parameters and their fixed-point Y (T-239 item 9): reused across calls.
    std::vector<double> m_splitScratch;
    std::vector<int32_t> m_yFixedScratch;
};

}  // namespace xt::sw
