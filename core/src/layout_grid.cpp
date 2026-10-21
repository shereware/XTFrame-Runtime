#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "layout_common.h"
#include "xt/assert.h"
#include "xt/layout.h"
#include "xt/node.h"
#include "xt/property.h"

/// Grid layout, the fixed-track subset of docs/design/06-layout.md §6.15 (T-338):
/// LayoutEngine::layoutGrid / layoutGridChildren / gridContentWidth. Tracks are px / % / fr /
/// auto (`repeat()` is expanded by the compiler); items are placed by `grid-area` names or
/// `grid-column` / `grid-row` lines first, then row-major auto-placement (sparse, no `dense`);
/// `auto` tracks take their single-span items' max-content size (one measurement), `fr` tracks
/// share what the other tracks and the gaps leave. No `order`: items keep document order.

namespace xt {

namespace {

/// One axis of an item's requested placement: a definite start (0-based) or auto, and a span.
struct AxisPlacement {
    int start = -1;
    int span = 1;
    bool definite = false;
};

uint32_t lineNumber(uint32_t v) { return v & ~kGridLineSpan; }
bool isSpan(uint32_t v) { return (v & kGridLineSpan) != 0; }

/// CSS Grid §8.3's line pair resolution, restricted to the subset: two lines, a line and a span,
/// or auto (with a span).
AxisPlacement resolveAxis(uint32_t startLine, uint32_t endLine) {
    const bool startIsLine = startLine != kGridLineAuto && !isSpan(startLine);
    const bool endIsLine = endLine != kGridLineAuto && !isSpan(endLine);
    AxisPlacement p;
    if (startIsLine && endIsLine) {
        int a = static_cast<int>(startLine) - 1;
        int b = static_cast<int>(endLine) - 1;
        if (b < a) {
            std::swap(a, b);
        }
        p.start = a;
        p.span = std::max(1, b - a);
        p.definite = true;
    } else if (startIsLine) {
        p.start = static_cast<int>(startLine) - 1;
        p.span = isSpan(endLine) ? static_cast<int>(lineNumber(endLine)) : 1;
        p.definite = true;
    } else if (endIsLine) {
        const int end = static_cast<int>(endLine) - 1;
        const int span = isSpan(startLine) ? static_cast<int>(lineNumber(startLine)) : 1;
        p.start = std::max(0, end - span);
        p.span = std::max(1, end - p.start);
        p.definite = true;
    } else {
        p.span = isSpan(startLine) ? static_cast<int>(lineNumber(startLine))
                                   : (isSpan(endLine) ? static_cast<int>(lineNumber(endLine)) : 1);
    }
    p.span = std::max(1, std::min(p.span, static_cast<int>(kMaxGridTracks)));
    return p;
}

/// 06 §6.15: a placement past the explicit tracks (`count` > 0) is clamped to the last track.
void clampAxis(AxisPlacement& p, int count, bool& clamped) {
    if (count <= 0) {
        return;
    }
    if (p.span > count) {
        p.span = count;
        clamped = true;
    }
    if (p.definite && p.start + p.span > count) {
        p.start = count - p.span;
        clamped = true;
    }
}

/// An item's sizing state across the track passes (layoutGridChildren).
struct GridItem {
    NodeId node = kInvalidNode;
    Edges margin{};
    float maxContentW = -1;  // Outer max-content width, measured on demand (< 0 = not yet).
    float outerH = 0;        // Outer height measured at the cell width (row sizing).
};

/// Row-major occupancy of the cells placed so far; rows grow on demand.
struct Occupancy {
    int cols = 1;
    int rows = 0;
    std::vector<uint8_t> cells;

    void ensureRows(int n) {
        if (n > rows) {
            rows = n;
            cells.resize(static_cast<size_t>(rows) * static_cast<size_t>(cols), 0);
        }
    }
    bool fits(int r, int c, int rs, int cs) {
        if (c < 0 || c + cs > cols) {
            return false;
        }
        ensureRows(r + rs);
        for (int y = r; y < r + rs; ++y) {
            for (int x = c; x < c + cs; ++x) {
                if (cells[static_cast<size_t>(y) * static_cast<size_t>(cols) + static_cast<size_t>(x)] != 0) {
                    return false;
                }
            }
        }
        return true;
    }
    void mark(int r, int c, int rs, int cs) {
        ensureRows(r + rs);
        for (int y = r; y < r + rs; ++y) {
            for (int x = std::max(0, c); x < std::min(cols, c + cs); ++x) {
                cells[static_cast<size_t>(y) * static_cast<size_t>(cols) + static_cast<size_t>(x)] = 1;
            }
        }
    }
};

/// The kind of track `i` of an axis: the explicit list, else `auto` (implicit tracks, and the
/// columns / rows `grid-template-areas` adds beyond the template).
GridTrack trackAt(const std::vector<GridTrack>& tracks, int i) {
    return i < static_cast<int>(tracks.size()) ? tracks[static_cast<size_t>(i)] : GridTrack{};
}

/// Whether track `t` is sized by its items' content: `auto`, and -- against an indefinite size --
/// `%` (behaves as auto) and `fr` (sized from the content, CSS Grid §12.7.1).
bool contentSized(const GridTrack& t, bool indefinite) {
    return t.kind == GridTrackKind::Auto ||
           (indefinite && (t.kind == GridTrackKind::Percent || t.kind == GridTrackKind::Fr));
}

/// 06 §6.15's track sizing for one axis: px / % directly, `auto` = `contribution` (the largest
/// outer size of the track's single-span items), `fr` = a share of `avail` minus the other tracks
/// and the gaps (0 when that is negative). `avail < 0` is an indefinite size: `%` behaves as
/// `auto` and the `fr` tracks are sized from their content (the largest contribution / fr, times
/// each track's fr).
void sizeTracks(const std::vector<GridTrack>& tracks, int count, float avail, float gap,
                const std::vector<float>& contribution, std::vector<float>& out) {
    out.assign(static_cast<size_t>(count), 0.0f);
    const bool indefinite = avail < 0.0f;
    float fixed = gap * static_cast<float>(std::max(0, count - 1));
    float sumFr = 0.0f;
    float frUnitFromContent = 0.0f;
    for (int i = 0; i < count; ++i) {
        const GridTrack t = trackAt(tracks, i);
        const float content = contribution[static_cast<size_t>(i)];
        switch (t.kind) {
            case GridTrackKind::Px:
                out[static_cast<size_t>(i)] = t.value;
                break;
            case GridTrackKind::Percent:
                out[static_cast<size_t>(i)] = indefinite ? content : t.value * avail * 0.01f;
                break;
            case GridTrackKind::Fr:
                sumFr += t.value;
                if (indefinite && t.value > 0.0f) {
                    frUnitFromContent = std::max(frUnitFromContent, content / t.value);
                }
                continue;  // Sized below; not part of the fixed sum.
            case GridTrackKind::Auto:
            default:
                out[static_cast<size_t>(i)] = content;
                break;
        }
        fixed += out[static_cast<size_t>(i)];
    }
    if (sumFr <= 0.0f) {
        return;
    }
    // CSS Grid §12.7.1: flex factors summing to less than 1 take only that fraction of the space.
    const float unit = indefinite ? frUnitFromContent : std::max(0.0f, avail - fixed) / std::max(sumFr, 1.0f);
    for (int i = 0; i < count; ++i) {
        const GridTrack t = trackAt(tracks, i);
        if (t.kind == GridTrackKind::Fr) {
            out[static_cast<size_t>(i)] = unit * t.value;
        }
    }
}

/// CSS Grid §12.8 (T-338): with `justify-content` / `align-content` `normal` / `stretch` and a
/// definite size, the space the tracks (fr ones included) leave is shared equally by the `auto`
/// tracks (and the `%` ones that behave as auto -- none on a definite axis).
void stretchAutoTracks(const std::vector<GridTrack>& tracks, int count, float avail, float gap,
                       std::vector<float>& sizes) {
    if (avail < 0.0f || count <= 0) {
        return;
    }
    float used = gap * static_cast<float>(count - 1);
    int autos = 0;
    for (int i = 0; i < count; ++i) {
        used += sizes[static_cast<size_t>(i)];
        autos += trackAt(tracks, i).kind == GridTrackKind::Auto ? 1 : 0;
    }
    const float free = avail - used;
    if (autos == 0 || free <= 0.0f) {
        return;
    }
    const float share = free / static_cast<float>(autos);
    for (int i = 0; i < count; ++i) {
        if (trackAt(tracks, i).kind == GridTrackKind::Auto) {
            sizes[static_cast<size_t>(i)] += share;
        }
    }
}

/// `justify-content` / `align-content` over the tracks (06 §6.15: only when the container is
/// larger than the tracks): the offset of the first track and the extra space between tracks.
void distributeTracks(Justify justify, float free, int count, float& start, float& between) {
    start = 0.0f;
    between = 0.0f;
    if (free <= 0.0f || count <= 0) {
        return;
    }
    switch (justify) {
        case Justify::FlexEnd:
            start = free;
            break;
        case Justify::Center:
            start = free * 0.5f;
            break;
        case Justify::SpaceBetween:
            between = count > 1 ? free / static_cast<float>(count - 1) : 0.0f;
            break;
        case Justify::SpaceAround:
            between = free / static_cast<float>(count);
            start = between * 0.5f;
            break;
        case Justify::SpaceEvenly:
            between = free / static_cast<float>(count + 1);
            start = between;
            break;
        case Justify::FlexStart:
        default:
            break;
    }
}

Justify alignContentAsJustify(AlignContent align) {
    switch (align) {
        case AlignContent::FlexEnd:
            return Justify::FlexEnd;
        case AlignContent::Center:
            return Justify::Center;
        case AlignContent::SpaceBetween:
            return Justify::SpaceBetween;
        case AlignContent::SpaceAround:
            return Justify::SpaceAround;
        case AlignContent::SpaceEvenly:
            return Justify::SpaceEvenly;
        case AlignContent::Stretch:  // `auto` rows already took the space (stretchAutoTracks).
            return Justify::Stretch;
        case AlignContent::FlexStart:
        default:
            return Justify::FlexStart;
    }
}

/// An item's block-axis alignment: its own `align-self`, else the container's `align-items`, in
/// AlignItems terms.
AlignItems itemAlign(AlignSelf self, AlignItems containerAlign) {
    switch (self) {
        case AlignSelf::Stretch:
            return AlignItems::Stretch;
        case AlignSelf::FlexStart:
            return AlignItems::FlexStart;
        case AlignSelf::FlexEnd:
            return AlignItems::FlexEnd;
        case AlignSelf::Center:
            return AlignItems::Center;
        case AlignSelf::Auto:
        default:
            return containerAlign;
    }
}

float alignOffset(AlignItems align, float free) {
    switch (align) {
        case AlignItems::FlexEnd:
            return free;
        case AlignItems::Center:
            return free * 0.5f;
        default:
            return 0.0f;
    }
}

/// The span of tracks [start, start + span) plus the gaps between them, from the track
/// positions (`pos`) and sizes.
float spanSize(const std::vector<float>& pos, const std::vector<float>& size, int start, int span) {
    const size_t a = static_cast<size_t>(start);
    const size_t b = static_cast<size_t>(start + span - 1);
    return pos[b] + size[b] - pos[a];
}

}  // namespace

/// Placement (06 §6.15) of `node`'s in-flow children: fills `items` (document order) and the
/// column / row counts. Shared by layoutGridChildren and gridContentWidth.
void LayoutEngine::placeGridItems(NodeArena& arena, NodeId node, std::vector<GridItemPlacement>& items,
                                  int& numCols, int& numRows) {
    const ElementData& container = arena.element(node);
    const ElementGrid& grid = container.grid;
    const int explicitCols = static_cast<int>(std::max<size_t>(grid.columns.size(), grid.areaCols));
    const int explicitRows = static_cast<int>(std::max<size_t>(grid.rows.size(), grid.areaRows));
    bool clamped = false;

    items.clear();
    arena.forEachChild(node, [&](NodeId c) {
        if (arena.kind(c) != NodeKind::Element) {
            return;
        }
        const ElementData& cd = arena.element(c);
        if (cd.style.display == Display::None || cd.cameraNode != kInvalidNode ||
            (arena.common(c).flags2 & kTemplate) != 0 || cd.style.position == Position::Absolute) {
            return;
        }
        const ComputedStyle& s = cd.style;
        GridItemPlacement item;
        item.node = c;
        const GridArea* area = nullptr;
        if (s.gridAreaName != kInvalidString) {
            for (const GridArea& a : grid.areas) {
                if (a.name == s.gridAreaName) {
                    area = &a;
                    break;
                }
            }
        }
        AxisPlacement row;
        AxisPlacement col;
        if (area != nullptr) {
            row = AxisPlacement{area->rowStart, area->rowEnd - area->rowStart, true};
            col = AxisPlacement{area->colStart, area->colEnd - area->colStart, true};
        } else {
            col = resolveAxis(s.gridLines[0], s.gridLines[1]);
            row = resolveAxis(s.gridLines[2], s.gridLines[3]);
        }
        clampAxis(col, explicitCols, clamped);
        clampAxis(row, explicitRows, clamped);
        item.rowStart = row.start;
        item.rowSpan = row.span;
        item.rowDefinite = row.definite;
        item.colStart = col.start;
        item.colSpan = col.span;
        item.colDefinite = col.definite;
        items.push_back(item);
    });

    // Columns: the explicit ones, or -- without any -- as many as the definite placements and
    // the widest auto span need (at least one).
    numCols = explicitCols;
    if (numCols == 0) {
        numCols = 1;
        for (const GridItemPlacement& it : items) {
            numCols = std::max(numCols, it.colDefinite ? it.colStart + it.colSpan : it.colSpan);
        }
        numCols = std::min(numCols, static_cast<int>(kMaxGridTracks));
    }
    for (GridItemPlacement& it : items) {
        if (it.colSpan > numCols) {
            it.colSpan = numCols;
            clamped = true;
        }
        if (it.colDefinite && it.colStart + it.colSpan > numCols) {
            it.colStart = numCols - it.colSpan;
            clamped = true;
        }
    }

    Occupancy occ;
    occ.cols = numCols;
    occ.ensureRows(std::max(explicitRows, 1));
    // 1. Fully definite items.
    for (GridItemPlacement& it : items) {
        if (it.rowDefinite && it.colDefinite) {
            occ.mark(it.rowStart, it.colStart, it.rowSpan, it.colSpan);
            it.placed = true;
        }
    }
    // 2. Items locked to a row: the first column in that row (after the previous such item in
    //    the same row) where they fit; none fits -> column 0, overlapping.
    std::vector<int> rowCursor;
    for (GridItemPlacement& it : items) {
        if (it.placed || !it.rowDefinite) {
            continue;
        }
        if (static_cast<int>(rowCursor.size()) <= it.rowStart) {
            rowCursor.resize(static_cast<size_t>(it.rowStart) + 1, 0);
        }
        int c = rowCursor[static_cast<size_t>(it.rowStart)];
        while (c + it.colSpan <= numCols && !occ.fits(it.rowStart, c, it.rowSpan, it.colSpan)) {
            ++c;
        }
        if (c + it.colSpan > numCols) {
            c = 0;
        }
        it.colStart = c;
        rowCursor[static_cast<size_t>(it.rowStart)] = c + it.colSpan;
        occ.mark(it.rowStart, c, it.rowSpan, it.colSpan);
        it.placed = true;
    }
    // 3. The rest, row-major with a sparse cursor (it never moves back; no `dense`).
    int cursorRow = 0;
    int cursorCol = 0;
    for (GridItemPlacement& it : items) {
        if (it.placed) {
            continue;
        }
        if (it.colDefinite) {
            if (it.colStart < cursorCol) {
                ++cursorRow;
            }
            int r = cursorRow;
            while (!occ.fits(r, it.colStart, it.rowSpan, it.colSpan)) {
                ++r;
            }
            cursorRow = r;
            cursorCol = it.colStart + it.colSpan;
            it.rowStart = r;
        } else {
            int r = cursorRow;
            int c = cursorCol;
            for (;;) {
                if (c + it.colSpan > numCols) {
                    ++r;
                    c = 0;
                    continue;
                }
                if (occ.fits(r, c, it.rowSpan, it.colSpan)) {
                    break;
                }
                ++c;
            }
            it.rowStart = r;
            it.colStart = c;
            cursorRow = r;
            cursorCol = c + it.colSpan;
        }
        occ.mark(it.rowStart, it.colStart, it.rowSpan, it.colSpan);
        it.placed = true;
    }
    numRows = std::max(explicitRows, 0);
    for (const GridItemPlacement& it : items) {
        numRows = std::max(numRows, it.rowStart + it.rowSpan);
    }
    if (clamped) {
        m_gridPlacementClamped = true;  // Document warns once (kWarnGridPlacementClamped).
    }
}

float LayoutEngine::gridOuterMaxContentWidth(NodeArena& arena, NodeId item, int depth) {
    const ComputedStyle& s = arena.element(item).style;
    const Length ml = s.base[prop::MarginLeft].len;
    const Length mr = s.base[prop::MarginRight].len;
    const float margins = (ml.unit == LengthUnit::Auto ? 0.0f : resolveLength(ml, kUndefinedRef, 0)) +
                          (mr.unit == LengthUnit::Auto ? 0.0f : resolveLength(mr, kUndefinedRef, 0));
    return maxContentWidth(arena, item, depth + 1) + margins;
}

float LayoutEngine::gridContentWidth(NodeArena& arena, NodeId node, int depth) {
    XT_ASSERT(static_cast<uint32_t>(depth) < kMaxElementDepth);
    std::vector<GridItemPlacement> items;
    int numCols = 0;
    int numRows = 0;
    placeGridItems(arena, node, items, numCols, numRows);
    const ElementData& data = arena.element(node);
    const float gap = resolveLength(data.style.base[prop::ColumnGap].len, kUndefinedRef, 0);
    std::vector<float> contribution(static_cast<size_t>(numCols), 0.0f);
    for (const GridItemPlacement& it : items) {
        if (it.colSpan == 1 && contentSized(trackAt(data.grid.columns, it.colStart), true)) {
            float& slot = contribution[static_cast<size_t>(it.colStart)];
            slot = std::max(slot, gridOuterMaxContentWidth(arena, it.node, depth));
        }
    }
    std::vector<float> sizes;
    sizeTracks(arena.element(node).grid.columns, numCols, kUndefinedRef, gap, contribution, sizes);
    float sum = gap * static_cast<float>(std::max(0, numCols - 1));
    for (float s : sizes) {
        sum += s;
    }
    return sum;
}

Vec2 LayoutEngine::layoutGridChildren(NodeArena& arena, NodeId node, float contentW, float contentH,
                                      bool widthAuto, bool heightAuto, int depth) {
    XT_ASSERT(static_cast<uint32_t>(depth) < kMaxElementDepth);
    const ComputedStyle& cs = arena.element(node).style;

    // Text children are not grid items here (as in flex): measured against the content box, their
    // size feeds the auto sizes. `.xt` forbids mixing text and element children.
    float textW = 0.0f;
    float textH = 0.0f;
    arena.forEachChild(node, [&](NodeId c) {
        if (arena.kind(c) == NodeKind::Text) {
            const float availMain = cs.writingMode == WritingMode::HorizontalTb
                                        ? (widthAuto ? kUndefinedRef : contentW)
                                        : (heightAuto ? kUndefinedRef : contentH);
            layoutTextNode(arena, c, cs, availMain);
            arena.common(c).visible = 1;
            textW = std::max(textW, arena.text(c).width);
            textH = std::max(textH, arena.text(c).height);
            return;
        }
        if (arena.kind(c) != NodeKind::Element) {
            return;
        }
        ElementData& cd = arena.element(c);
        // A <camera> and a `<repeat>` template are display: none (as in layoutChild).
        if (cd.style.display == Display::None || cd.cameraNode != kInvalidNode ||
            (arena.common(c).flags2 & kTemplate) != 0) {
            cd.box = LayoutBox{};
            arena.common(c).visible = 0;
            return;
        }
        arena.common(c).visible = 1;  // Absolute ones are sized by layoutAbsolutePass.
    });

    std::vector<GridItemPlacement> items;
    int numCols = 0;
    int numRows = 0;
    placeGridItems(arena, node, items, numCols, numRows);
    const ElementGrid& grid = arena.element(node).grid;
    const float refW = widthAuto ? kUndefinedRef : contentW;
    const float refH = heightAuto ? kUndefinedRef : contentH;
    const float colGap = resolveLength(cs.base[prop::ColumnGap].len, refW, 0);
    const float rowGap = resolveLength(cs.base[prop::RowGap].len, refH, 0);

    std::vector<GridItem> state(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        const ComputedStyle& s = arena.element(items[i].node).style;
        state[i].node = items[i].node;
        // Percent margins reference the grid area's inline size in CSS; the container's content
        // width is the stand-in here (auto margins count as 0).
        state[i].margin = resolveEdgesPx(s.base[prop::MarginTop].len, s.base[prop::MarginRight].len,
                                         s.base[prop::MarginBottom].len, s.base[prop::MarginLeft].len, refW);
    }

    // --- Columns ---
    std::vector<float> colContribution(static_cast<size_t>(numCols), 0.0f);
    for (size_t i = 0; i < items.size(); ++i) {
        const GridItemPlacement& it = items[i];
        if (it.colSpan == 1 && contentSized(trackAt(grid.columns, it.colStart), widthAuto)) {
            state[i].maxContentW = gridOuterMaxContentWidth(arena, it.node, depth);
            float& slot = colContribution[static_cast<size_t>(it.colStart)];
            slot = std::max(slot, state[i].maxContentW);
        }
    }
    std::vector<float> colSize;
    sizeTracks(grid.columns, numCols, widthAuto ? kUndefinedRef : contentW, colGap, colContribution, colSize);
    if (cs.justify == Justify::Stretch) {
        stretchAutoTracks(grid.columns, numCols, widthAuto ? kUndefinedRef : contentW, colGap, colSize);
    }
    float colsTotal = colGap * static_cast<float>(std::max(0, numCols - 1));
    for (float s : colSize) {
        colsTotal += s;
    }
    const float finalW = widthAuto ? colsTotal : contentW;
    float colStart = 0.0f;
    float colBetween = 0.0f;
    distributeTracks(cs.justify, finalW - colsTotal, numCols, colStart, colBetween);
    std::vector<float> colPos(static_cast<size_t>(numCols), 0.0f);
    {
        float cursor = colStart;
        for (int i = 0; i < numCols; ++i) {
            colPos[static_cast<size_t>(i)] = cursor;
            cursor += colSize[static_cast<size_t>(i)] + colGap + colBetween;
        }
    }

    // Lays out item `i` in a cell `cellW` wide (`cellH` high, or kUndefinedRef while the rows are
    // still being sized): width per justify-items, height stretched per align-items once the cell
    // height is known.
    const auto layoutInCell = [&](size_t i, float cellW, float cellH) {
        const NodeId c = state[i].node;
        const ComputedStyle& s = arena.element(c).style;
        const Edges& m = state[i].margin;
        const bool widthIsAuto = s.base[prop::Width].len.unit == LengthUnit::Auto;
        float availW = cellW;
        if (widthIsAuto && cs.justifyItems != AlignItems::Stretch) {
            if (state[i].maxContentW < 0.0f) {
                state[i].maxContentW = gridOuterMaxContentWidth(arena, c, depth);
            }
            // Shrink-to-fit: the max-content width, at most the cell (minus the margins).
            availW = std::min(state[i].maxContentW, cellW);
        }
        float imposedH = kUndefinedRef;
        float px = 0.0f;
        const bool heightDefinite = resolveHeightLength(s.base[prop::Height].len, cellH, px);
        const AlignItems align = itemAlign(s.alignSelf, cs.alignItems);
        // CSS Box Alignment: an item with an `aspect-ratio` is not stretched by `normal`; the
        // subset treats every stretch that way (the ratio then gives the height).
        if (cellH >= 0.0f && !heightDefinite && align == AlignItems::Stretch &&
            !(s.base[prop::AspectRatio].f > 0.0f)) {
            imposedH = std::max(0.0f, cellH - m.top - m.bottom);
        }
        layoutChild(arena, c, availW, cellH, imposedH, depth + 1);
    };

    // --- Rows ---
    std::vector<float> rowContribution(static_cast<size_t>(numRows), 0.0f);
    bool rowsNeedContent = false;
    for (int r = 0; r < numRows; ++r) {
        rowsNeedContent = rowsNeedContent || contentSized(trackAt(grid.rows, r), heightAuto);
    }
    if (rowsNeedContent) {
        for (size_t i = 0; i < items.size(); ++i) {
            const GridItemPlacement& it = items[i];
            if (it.rowSpan != 1 || !contentSized(trackAt(grid.rows, it.rowStart), heightAuto)) {
                continue;
            }
            layoutInCell(i, spanSize(colPos, colSize, it.colStart, it.colSpan), kUndefinedRef);
            const LayoutBox& b = arena.element(it.node).box;
            float& slot = rowContribution[static_cast<size_t>(it.rowStart)];
            slot = std::max(slot, b.height + state[i].margin.top + state[i].margin.bottom);
        }
    }
    std::vector<float> rowSize;
    sizeTracks(grid.rows, numRows, heightAuto ? kUndefinedRef : contentH, rowGap, rowContribution, rowSize);
    if (cs.alignContent == AlignContent::Stretch) {
        stretchAutoTracks(grid.rows, numRows, heightAuto ? kUndefinedRef : contentH, rowGap, rowSize);
    }
    float rowsTotal = rowGap * static_cast<float>(std::max(0, numRows - 1));
    for (float s : rowSize) {
        rowsTotal += s;
    }
    const float finalH = heightAuto ? rowsTotal : contentH;
    float rowStart = 0.0f;
    float rowBetween = 0.0f;
    distributeTracks(alignContentAsJustify(cs.alignContent), finalH - rowsTotal, numRows, rowStart,
                     rowBetween);
    std::vector<float> rowPos(static_cast<size_t>(numRows), 0.0f);
    {
        float cursor = rowStart;
        for (int i = 0; i < numRows; ++i) {
            rowPos[static_cast<size_t>(i)] = cursor;
            cursor += rowSize[static_cast<size_t>(i)] + rowGap + rowBetween;
        }
    }

    // --- Items in their cells ---
    for (size_t i = 0; i < items.size(); ++i) {
        const GridItemPlacement& it = items[i];
        // The area spans its tracks and the gaps between them (content distribution widens the
        // gaps, CSS Box Alignment §5.3).
        const float cellW = spanSize(colPos, colSize, it.colStart, it.colSpan);
        const float cellH = spanSize(rowPos, rowSize, it.rowStart, it.rowSpan);
        layoutInCell(i, cellW, cellH);
        ElementData& cd = arena.element(it.node);
        const Edges& m = state[i].margin;
        const AlignItems align = itemAlign(cd.style.alignSelf, cs.alignItems);
        const float freeX = cellW - cd.box.width - m.left - m.right;
        const float freeY = cellH - cd.box.height - m.top - m.bottom;
        cd.box.x = colPos[static_cast<size_t>(it.colStart)] + m.left + alignOffset(cs.justifyItems, freeX);
        cd.box.y = rowPos[static_cast<size_t>(it.rowStart)] + m.top + alignOffset(align, freeY);
    }

    return Vec2{std::max(finalW, widthAuto ? textW : 0.0f), std::max(finalH, heightAuto ? textH : 0.0f)};
}

void LayoutEngine::layoutGrid(NodeArena& arena, NodeId node, float availW, float availH, float imposedH,
                              int depth) {
    XT_ASSERT(static_cast<uint32_t>(depth) < kMaxElementDepth);
    ElementData& data = arena.element(node);
    const ComputedStyle& style = data.style;
    // The container's own box, as layoutFlex sizes it (§6.4 references, T-74 definiteness).
    const float refW = availW;
    const float refH = availH;
    const auto [margin, padding, border] = resolveBoxEdges(style, refW);
    const float extraW = padding.left + padding.right + border.left + border.right;
    const float extraH = padding.top + padding.bottom + border.top + border.bottom;

    const Length widthLen = style.base[prop::Width].len;
    const bool widthAuto = widthLen.unit == LengthUnit::Auto;
    float definiteBorderBoxH = 0;
    bool heightDefinite =
        resolveDefiniteHeight(style.base[prop::Height].len, refH, imposedH, definiteBorderBoxH);
    data.hasIntrinsic = 0;
    data.intrinsic = Vec2{0, 0};

    float contentW = 0.0f;
    bool widthIsAxisAuto = false;
    if (!widthAuto) {
        contentW = std::max(0.0f, resolveLength(widthLen, refW, 0) - extraW);
    } else if (availW >= 0.0f) {
        const Length mlLen = style.base[prop::MarginLeft].len;
        const Length mrLen = style.base[prop::MarginRight].len;
        const float mL = mlLen.unit == LengthUnit::Auto ? 0.0f : margin.left;
        const float mR = mrLen.unit == LengthUnit::Auto ? 0.0f : margin.right;
        contentW = std::max(0.0f, availW - mL - mR - extraW);
    } else {
        widthIsAxisAuto = true;  // Shrink-to-fit measurement: the columns' own size.
    }
    if (!widthIsAxisAuto) {
        contentW =
            clampMinMax(contentW, style.base[prop::MinWidth].len, style.base[prop::MaxWidth].len, refW);
    }

    // §6.16 (T-328): `aspect-ratio` on the border box, as in layoutFlex.
    const float ratio = node == m_aspectSuppressed ? 0.0f : style.base[prop::AspectRatio].f;
    if (ratio > 0.0f && ((widthAuto && heightDefinite) || (!heightDefinite && !widthIsAxisAuto))) {
        const Vec2 size = containerAspectSize(style, ratio, refW, refH, extraW, extraH, heightDefinite,
                                              definiteBorderBoxH, contentW, widthAuto);
        contentW = std::max(0.0f, size.x - extraW);
        widthIsAxisAuto = false;
        definiteBorderBoxH = size.y;
        heightDefinite = true;
    }

    float contentH = heightDefinite ? std::max(0.0f, definiteBorderBoxH - extraH) : 0.0f;
    const Vec2 finalSize =
        layoutGridChildren(arena, node, contentW, contentH, widthIsAxisAuto, !heightDefinite, depth);
    if (widthIsAxisAuto) {
        contentW =
            clampMinMax(finalSize.x, style.base[prop::MinWidth].len, style.base[prop::MaxWidth].len, refW);
    }
    if (!heightDefinite) {
        contentH = finalSize.y;
    }
    contentH =
        clampHeightMinMax(contentH, style.base[prop::MinHeight].len, style.base[prop::MaxHeight].len, refH);

    data.box.margin = margin;
    data.box.padding = padding;
    data.box.border = border;
    data.box.width = contentW + extraW;
    data.box.height = contentH + extraH;
    storeScrollSize(arena, node, contentW, contentH);
}

}  // namespace xt
