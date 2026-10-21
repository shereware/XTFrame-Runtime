#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "layout_common.h"
#include "xt/assert.h"
#include "xt/layout.h"
#include "xt/node.h"
#include "xt/property.h"

/// Flex layout (docs/design/06-layout.md §6.9): resolveFlexibleLengths (CSS Flexbox §9.7, unit
/// tested standalone) and LayoutEngine::layoutFlex / layoutFlexChildren (the full single- and
/// multi-line algorithm: flex-basis, grow/shrink with min/max freezing, wrap, justify-content,
/// align-items/self, align-content, main-axis auto margins, gap, `*-reverse`, container auto
/// size). See docs/tasks/T-26.md.

namespace xt {

void resolveFlexibleLengths(const std::vector<FlexBasisInput>& items, float free,
                            std::vector<float>& outSizes) {
    const size_t n = items.size();
    outSizes.assign(n, 0.0f);
    if (n == 0) {
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        outSizes[i] = items[i].basis;
    }
    if (free == 0.0f) {
        return;
    }
    const bool growing = free > 0.0f;

    std::vector<uint8_t> frozen(n, 0);
    // At most n passes: each pass either terminates (no new violation) or freezes at least one
    // previously-unfrozen item, so there can be at most n passes that freeze something plus one
    // final non-freezing pass.
    for (size_t pass = 0; pass <= n; ++pass) {
        float sumWeight = 0.0f;
        float allocatedToFrozen = 0.0f;
        bool anyUnfrozen = false;
        for (size_t i = 0; i < n; ++i) {
            if (frozen[i] != 0) {
                allocatedToFrozen += outSizes[i] - items[i].basis;
            } else {
                anyUnfrozen = true;
                sumWeight += growing ? items[i].grow : items[i].shrink * items[i].basis;
            }
        }
        if (!anyUnfrozen || sumWeight <= 0.0f) {
            break;
        }
        const float remainingFree = free - allocatedToFrozen;

        bool violated = false;
        for (size_t i = 0; i < n; ++i) {
            if (frozen[i] != 0) {
                continue;
            }
            const float weight = growing ? items[i].grow : items[i].shrink * items[i].basis;
            const float target = items[i].basis + remainingFree * (weight / sumWeight);
            float clamped = target;
            if (clamped < items[i].minMain) {
                clamped = items[i].minMain;
            }
            if (clamped > items[i].maxMain) {
                clamped = items[i].maxMain;
            }
            outSizes[i] = clamped;
            if (clamped != target) {
                frozen[i] = 1;
                violated = true;
            }
        }
        if (!violated) {
            break;
        }
    }
}

namespace {

/// AlignSelf::Auto -> the container's align-items, otherwise unchanged. AlignItems and AlignSelf
/// enumerators intentionally line up (AlignSelf's non-Auto values are AlignItems's values shifted
/// by one, see xt/node.h), but this function spells the mapping out explicitly rather than relying
/// on that numeric coincidence.
AlignSelf resolveAlignSelf(AlignSelf self, AlignItems containerAlign) {
    if (self != AlignSelf::Auto) {
        return self;
    }
    switch (containerAlign) {
        case AlignItems::Stretch:
            return AlignSelf::Stretch;
        case AlignItems::FlexStart:
            return AlignSelf::FlexStart;
        case AlignItems::FlexEnd:
            return AlignSelf::FlexEnd;
        case AlignItems::Center:
            return AlignSelf::Center;
    }
    return AlignSelf::Stretch;
}

/// One flex item's per-axis state, accumulated across §6.9 steps 2-9. `main`/`cross` name the two
/// physical edges of each axis generically ([0] = main-start/cross-start i.e. left/top, [1] =
/// main-end/cross-end i.e. right/bottom); layoutFlexChildren maps these back to x/y using the
/// container's flex-direction.
struct FlexItem {
    NodeId node = kInvalidNode;
    float marginMain[2] = {0, 0};
    bool marginMainAuto[2] = {false, false};
    float marginCross[2] = {0, 0};
    bool marginCrossAuto[2] = {false, false};
    float basis = 0;  // hypotheticalMain: flexBasis clamped to [minMain, maxMain]
    float minMain = 0;
    float maxMain = kFlexNoMax;
    float grow = 0;
    float shrink = 1;
    float outerMain = 0;  // basis + non-auto main margins
    float mainSize = 0;   // resolved by resolveFlexibleLengths
    bool crossAuto = false;
    // §6.16 (T-328): `aspect-ratio` (w / h) when the item's cross size is auto and it is not
    // stretched, else 0: the cross size then follows the resolved main size.
    float ratio = 0;
    // §6.16: a stretched item with an auto cross size ignores its `aspect-ratio` (stretch wins);
    // its own layoutBlock / layoutFlex must not apply it either (LayoutEngine::m_aspectSuppressed).
    bool suppressAspect = false;
    AlignSelf alignSelf = AlignSelf::Stretch;
    float crossSize = 0;  // border-box cross size
    float outerCross = 0;
    float mainPos = 0;   // border-box main-start position, relative to content box main-start
    float crossPos = 0;  // border-box cross-start position, relative to content box cross-start
};

/// Distributes `free` (>= 0 only meaningfully; negative free is a no-op) across `n` gaps per one
/// of the six CSS `justify-content` / `align-content` "packing" keywords shared by both (align-
/// content's extra `stretch` value is handled by the caller, not here). Returns the position of
/// the first item ("start offset") and the additional spacing to insert *between* items (added on
/// top of `baseGap`, e.g. the `gap` property).
struct PackingResult {
    float startOffset = 0;
    float between = 0;
};

PackingResult packItems(Justify justify, float free, float baseGap, int n) {
    PackingResult result;
    result.between = baseGap;
    if (n <= 0) {
        return result;
    }
    float positiveFree = free > 0.0f ? free : 0.0f;
    switch (justify) {
        case Justify::FlexStart:
        case Justify::Stretch:  // T-338: `normal` / `stretch` packs like flex-start in flex.
            result.startOffset = 0.0f;
            break;
        case Justify::FlexEnd:
            result.startOffset = free;
            break;
        case Justify::Center:
            result.startOffset = free * 0.5f;
            break;
        case Justify::SpaceBetween:
            if (n == 1) {
                result.startOffset = 0.0f;
            } else {
                result.between += positiveFree / static_cast<float>(n - 1);
            }
            break;
        case Justify::SpaceAround: {
            float spacing = positiveFree / static_cast<float>(n);
            result.startOffset = spacing * 0.5f;
            result.between += spacing;
            break;
        }
        case Justify::SpaceEvenly: {
            float spacing = positiveFree / static_cast<float>(n + 1);
            result.startOffset = spacing;
            result.between += spacing;
            break;
        }
    }
    return result;
}

/// align-content packing: like packItems but keyed by AlignContent (seven values, the extra one
/// -- Stretch -- makes every line grow instead of repositioning, handled entirely by the caller).
PackingResult packLines(AlignContent align, float free, float baseGap, int n) {
    Justify justify;
    switch (align) {
        case AlignContent::FlexStart:
        case AlignContent::Stretch:
            justify = Justify::FlexStart;
            break;
        case AlignContent::FlexEnd:
            justify = Justify::FlexEnd;
            break;
        case AlignContent::Center:
            justify = Justify::Center;
            break;
        case AlignContent::SpaceBetween:
            justify = Justify::SpaceBetween;
            break;
        case AlignContent::SpaceAround:
            justify = Justify::SpaceAround;
            break;
        case AlignContent::SpaceEvenly:
        default:
            justify = Justify::SpaceEvenly;
            break;
    }
    return packItems(justify, free, baseGap, n);
}

/// T-336 (06 §6.9): puts `items` (document order) in `order` order -- a stable sort by each item's
/// `order` value. The permutation is cached on the container (ElementData::flexOrder) and sorted
/// again only when the items or one of their `order` values differ from the cached ones; while
/// every item has `order: 0` nothing is sorted and the cache stays empty.
void applyFlexOrder(NodeArena& arena, NodeId container, std::vector<FlexItem>& items) {
    ElementData::FlexOrderCache& cache = arena.element(container).flexOrder;
    bool any = false;
    for (const FlexItem& it : items) {
        any = any || arena.element(it.node).style.base[prop::Order].f != 0.0f;
    }
    if (!any) {
        cache.items.clear();
        cache.orders.clear();
        cache.permutation.clear();
        return;
    }
    const size_t n = items.size();
    bool same = cache.items.size() == n && cache.permutation.size() == n;
    for (size_t i = 0; same && i < n; ++i) {
        same = cache.items[i] == items[i].node &&
               cache.orders[i] == arena.element(items[i].node).style.base[prop::Order].f;
    }
    if (!same) {
        cache.items.resize(n);
        cache.orders.resize(n);
        cache.permutation.resize(n);
        for (size_t i = 0; i < n; ++i) {
            cache.items[i] = items[i].node;
            cache.orders[i] = arena.element(items[i].node).style.base[prop::Order].f;
            cache.permutation[i] = static_cast<uint32_t>(i);
        }
        const std::vector<float>& orders = cache.orders;
        std::stable_sort(cache.permutation.begin(), cache.permutation.end(),
                         [&orders](uint32_t a, uint32_t b) { return orders[a] < orders[b]; });
    }
    std::vector<FlexItem> sorted;
    sorted.reserve(n);
    for (uint32_t index : cache.permutation) {
        sorted.push_back(items[index]);
    }
    items.swap(sorted);
}

}  // namespace

Vec2 LayoutEngine::layoutFlexChildren(NodeArena& arena, NodeId node, float contentMain, float contentCross,
                                      bool mainAxisAuto, bool crossAxisAuto, float refW, float refH,
                                      int depth) {
    XT_ASSERT(static_cast<uint32_t>(depth) < kMaxElementDepth);
    const ComputedStyle& containerStyle = arena.element(node).style;
    const bool rowDir =
        containerStyle.flexDir == FlexDirection::Row || containerStyle.flexDir == FlexDirection::RowReverse;
    const bool reverse = containerStyle.flexDir == FlexDirection::RowReverse ||
                         containerStyle.flexDir == FlexDirection::ColumnReverse;
    const bool wrap = containerStyle.flexWrap != FlexWrap::NoWrap;
    // T-336: `wrap-reverse` swaps cross-start and cross-end. The lines are laid out as for `wrap`
    // with each item's cross margins swapped, then every cross position is mirrored in the
    // container's cross size: the line order, align-content and align-self start / end all flip.
    const bool wrapReverse = containerStyle.flexWrap == FlexWrap::WrapReverse;
    const AlignItems containerAlignItems = containerStyle.alignItems;

    const float mainGapPx =
        resolveLength(containerStyle.base[rowDir ? prop::ColumnGap : prop::RowGap].len, refW, 0);
    const float crossGapPx =
        resolveLength(containerStyle.base[rowDir ? prop::RowGap : prop::ColumnGap].len, refW, 0);

    // layoutChild for one item; a stretched item's `aspect-ratio` is suppressed for the call (§6.16).
    const auto layoutItem = [&](NodeId item, bool suppressAspect, float availW, float imposedH) {
        const NodeId previous = m_aspectSuppressed;
        if (suppressAspect) {
            m_aspectSuppressed = item;
        }
        layoutChild(arena, item, availW, refH, imposedH, depth + 1);
        m_aspectSuppressed = previous;
    };

    // Step 2: gather in-flow flex items (skip Text, display: none, position: absolute), in
    // document order, computing each one's flex base size / hypothetical main size / outer main.
    std::vector<FlexItem> items;
    // §6.8/docs/tasks/T-30.md: a Text child of a flex container is not a flex item (it has no
    // box of its own to flex); it is measured against the container's content size and its own
    // size contributes to an auto main/cross size below. `.xt` forbids mixing text and element
    // children, so in practice such a container has exactly this one child.
    float textW = 0.0f;
    float textH = 0.0f;
    arena.forEachChild(node, [&](NodeId c) {
        if (arena.kind(c) == NodeKind::Text) {
            float contentWidth = rowDir ? (mainAxisAuto ? kUndefinedRef : contentMain)
                                        : (crossAxisAuto ? kUndefinedRef : contentCross);
            float contentHeight = rowDir ? (crossAxisAuto ? kUndefinedRef : contentCross)
                                         : (mainAxisAuto ? kUndefinedRef : contentMain);
            float availMain =
                containerStyle.writingMode == WritingMode::HorizontalTb ? contentWidth : contentHeight;
            layoutTextNode(arena, c, containerStyle, availMain);
            arena.common(c).visible = 1;
            const TextNodeData& textData = arena.text(c);
            textW = std::max(textW, textData.width);
            textH = std::max(textH, textData.height);
            return;
        }
        if (arena.kind(c) != NodeKind::Element) {
            return;  // Scene subtree nodes aren't DOM children.
        }
        ElementData& cd = arena.element(c);
        // docs/design/19-3d-space.md §19.4: a <camera> element is treated as display:none.
        if (cd.style.display == Display::None || cd.cameraNode != kInvalidNode) {
            cd.box = LayoutBox{};
            arena.common(c).visible = 0;
            return;
        }
        arena.common(c).visible = 1;
        if (cd.style.position == Position::Absolute) {
            return;  // §6.6, resolved later by LayoutEngine::layoutAbsolutePass.
        }

        const ComputedStyle& s = cd.style;
        Edges margin = resolveEdgesPx(s.base[prop::MarginTop].len, s.base[prop::MarginRight].len,
                                      s.base[prop::MarginBottom].len, s.base[prop::MarginLeft].len, refW);
        Length mtLen = s.base[prop::MarginTop].len;
        Length mrLen = s.base[prop::MarginRight].len;
        Length mbLen = s.base[prop::MarginBottom].len;
        Length mlLen = s.base[prop::MarginLeft].len;

        FlexItem item;
        item.node = c;
        if (rowDir) {
            item.marginMain[0] = mlLen.unit == LengthUnit::Auto ? 0.0f : margin.left;
            item.marginMain[1] = mrLen.unit == LengthUnit::Auto ? 0.0f : margin.right;
            item.marginMainAuto[0] = mlLen.unit == LengthUnit::Auto;
            item.marginMainAuto[1] = mrLen.unit == LengthUnit::Auto;
            item.marginCross[0] = mtLen.unit == LengthUnit::Auto ? 0.0f : margin.top;
            item.marginCross[1] = mbLen.unit == LengthUnit::Auto ? 0.0f : margin.bottom;
            item.marginCrossAuto[0] = mtLen.unit == LengthUnit::Auto;
            item.marginCrossAuto[1] = mbLen.unit == LengthUnit::Auto;
        } else {
            item.marginMain[0] = mtLen.unit == LengthUnit::Auto ? 0.0f : margin.top;
            item.marginMain[1] = mbLen.unit == LengthUnit::Auto ? 0.0f : margin.bottom;
            item.marginMainAuto[0] = mtLen.unit == LengthUnit::Auto;
            item.marginMainAuto[1] = mbLen.unit == LengthUnit::Auto;
            item.marginCross[0] = mlLen.unit == LengthUnit::Auto ? 0.0f : margin.left;
            item.marginCross[1] = mrLen.unit == LengthUnit::Auto ? 0.0f : margin.right;
            item.marginCrossAuto[0] = mlLen.unit == LengthUnit::Auto;
            item.marginCrossAuto[1] = mrLen.unit == LengthUnit::Auto;
        }

        Length widthLen = s.base[prop::Width].len;
        Length heightLen = s.base[prop::Height].len;
        Length mainSizeLen = rowDir ? widthLen : heightLen;
        Length crossSizeLen = rowDir ? heightLen : widthLen;
        item.crossAuto = crossSizeLen.unit == LengthUnit::Auto;
        item.alignSelf = resolveAlignSelf(s.alignSelf, containerAlignItems);
        if (item.crossAuto && s.base[prop::AspectRatio].f > 0.0f) {
            if (item.alignSelf == AlignSelf::Stretch) {
                item.suppressAspect = true;  // §6.16: stretch wins, the ratio is ignored.
            } else {
                item.ratio = s.base[prop::AspectRatio].f;
                item.crossAuto = false;
            }
        }

        // §6.4 (T-74): a main-axis percentage references the container's content width in a row
        // container and its (possibly indefinite) content height in a column one; an indefinite
        // reference makes the percentage behave as `auto`, i.e. fall through to the content size.
        const float mainRef = rowDir ? refW : refH;
        auto resolveMain = [&](Length len, float& out) {
            if (len.unit == LengthUnit::Auto) {
                return false;
            }
            if (len.unit == LengthUnit::Percent && mainRef < 0.0f) {
                return false;
            }
            out = resolveLength(len, mainRef, 0);
            return true;
        };

        Length basisLen = s.base[prop::FlexBasis].len;
        float flexBasis = 0;
        if (resolveMain(basisLen, flexBasis)) {
            // flex-basis wins.
        } else if (resolveMain(mainSizeLen, flexBasis)) {
            // width (row) / height (column) is the flex base size.
        } else {
            // Content-size fallback (§6.9 step 2, T-26 simplification -- no text this task: see
            // the T-26 report's "uncovered cases"). Measures the child once with the main axis
            // left as undefined and reads back the resulting main-axis box size; this is an
            // approximation of true shrink-to-fit (a block child's own width: auto still *fills*
            // its available width per §6.5 step 2, so a plain <div> with no explicit size measures
            // as 0-wide here, not by its descendants' content).
            layoutItem(c, item.suppressAspect, rowDir ? kUndefinedRef : refW, kUndefinedRef);
            flexBasis = rowDir ? cd.box.width : cd.box.height;
        }

        Length minLen = rowDir ? s.base[prop::MinWidth].len : s.base[prop::MinHeight].len;
        Length maxLen = rowDir ? s.base[prop::MaxWidth].len : s.base[prop::MaxHeight].len;
        item.minMain = 0;
        resolveMain(minLen, item.minMain);
        item.maxMain = kFlexNoMax;
        resolveMain(maxLen, item.maxMain);

        float hypotheticalMain = flexBasis;
        if (hypotheticalMain < item.minMain) {
            hypotheticalMain = item.minMain;
        }
        if (hypotheticalMain > item.maxMain) {
            hypotheticalMain = item.maxMain;
        }
        item.basis = hypotheticalMain;
        item.grow = s.base[prop::FlexGrow].f;
        item.shrink = s.base[prop::FlexShrink].f;
        item.outerMain = item.basis + item.marginMain[0] + item.marginMain[1];
        if (wrapReverse) {
            std::swap(item.marginCross[0], item.marginCross[1]);
            std::swap(item.marginCrossAuto[0], item.marginCrossAuto[1]);
        }

        items.push_back(item);
    });
    // T-336 (06 §6.9): `order` -- before the lines are formed.
    applyFlexOrder(arena, node, items);

    // Step 3: break into lines. `wrap` is disabled (single line) when the main axis is auto (a
    // shrink-to-fit container has no fixed width to wrap against).
    std::vector<std::pair<size_t, size_t>> lines;
    if (!items.empty()) {
        if (!wrap || mainAxisAuto) {
            lines.emplace_back(0, items.size());
        } else {
            size_t lineStart = 0;
            float used = items[0].outerMain;
            for (size_t i = 1; i < items.size(); ++i) {
                float candidate = used + mainGapPx + items[i].outerMain;
                if (candidate > contentMain) {
                    lines.emplace_back(lineStart, i);
                    lineStart = i;
                    used = items[i].outerMain;
                } else {
                    used = candidate;
                }
            }
            lines.emplace_back(lineStart, items.size());
        }
    }

    // Steps 4-6: per line, resolve flexible main sizes, then measure each item's cross size, then
    // this line's cross size.
    std::vector<float> lineCrossSize(lines.size(), 0.0f);
    std::vector<float> lineUsedMain(lines.size(), 0.0f);  // Sum(outer main incl. gaps), post-freeze.
    for (size_t li = 0; li < lines.size(); ++li) {
        size_t lineStart = lines[li].first;
        size_t lineEnd = lines[li].second;
        size_t n = lineEnd - lineStart;

        std::vector<FlexBasisInput> basisInputs(n);
        float sumOuter = 0.0f;
        for (size_t i = 0; i < n; ++i) {
            const FlexItem& it = items[lineStart + i];
            basisInputs[i] = FlexBasisInput{it.basis, it.grow, it.shrink, it.minMain, it.maxMain};
            sumOuter += it.outerMain;
        }
        float gaps = n > 0 ? mainGapPx * static_cast<float>(n - 1) : 0.0f;
        float free = mainAxisAuto ? 0.0f : (contentMain - sumOuter - gaps);

        std::vector<float> sizes;
        resolveFlexibleLengths(basisInputs, free, sizes);

        float usedMain = gaps;
        for (size_t i = 0; i < n; ++i) {
            FlexItem& it = items[lineStart + i];
            it.mainSize = sizes[i];
            usedMain += it.mainSize + it.marginMain[0] + it.marginMain[1];

            // Step 5: cross-size measurement pass. The line's cross size isn't known yet, so the
            // cross axis is left undefined here; align-self: stretch items are re-measured in
            // step 9 once the line's final cross size is known.
            float measureAvailW = rowDir ? it.mainSize : kUndefinedRef;
            // T-74: the item's own Percent heights reference the container's content height
            // (refH); its resolved main size, in a column container, is a definite used height
            // imposed on it (so its descendants' Percent heights resolve against it too).
            float imposedH = rowDir ? kUndefinedRef : it.mainSize;
            if (rowDir && it.ratio > 0.0f) {
                // §6.16 (T-328): the cross size follows the final (post-freeze) main size, once;
                // only the cross axis is clamped (the main size is final). A column item's width
                // follows the imposed height inside layoutBlock / layoutFlex the same way.
                const AspectMinMax bounds = aspectMinMax(arena.element(it.node).style, refW, refH);
                imposedH = resolveAspect(it.mainSize, kUndefinedRef, it.ratio, bounds).y;
            }
            layoutItem(it.node, it.suppressAspect, measureAvailW, imposedH);
            ElementData& cd = arena.element(it.node);
            // The flex algorithm -- not the child's own width/height: auto rules -- determines the
            // used main size (e.g. flex-grow can grow an item past its own explicit width); force
            // it here rather than trusting whatever layoutChild computed.
            if (rowDir) {
                cd.box.width = it.mainSize;
                it.crossSize = cd.box.height;
            } else {
                cd.box.height = it.mainSize;
                it.crossSize = cd.box.width;
            }
            it.outerCross = it.crossSize + it.marginCross[0] + it.marginCross[1];
        }
        lineUsedMain[li] = usedMain;

        float maxOuterCross = 0.0f;
        for (size_t i = 0; i < n; ++i) {
            maxOuterCross = std::max(maxOuterCross, items[lineStart + i].outerCross);
        }
        lineCrossSize[li] = maxOuterCross;
    }
    // Step 6: a single line in a definite-cross-size container takes the full cross size.
    if (lines.size() == 1 && !crossAxisAuto) {
        lineCrossSize[0] = contentCross;
    }

    // Step 10 (cross): container's own auto cross size, computed before align-content redistributes
    // the (possibly now-negative) leftover space -- align-content only ever *adds* extra space to
    // an already-definite cross size.
    float sumLineCross = 0.0f;
    for (float lc : lineCrossSize) {
        sumLineCross += lc;
    }
    float crossGapsTotal = lines.empty() ? 0.0f : crossGapPx * static_cast<float>(lines.size() - 1);
    float finalCross = crossAxisAuto ? (sumLineCross + crossGapsTotal) : contentCross;

    // Step 7: align-content distributes multi-line leftover cross space (a no-op when the cross
    // axis is auto -- there's no "container" leftover to distribute against).
    std::vector<float> lineCrossStart(lines.size(), 0.0f);
    if (!lines.empty()) {
        float crossFree = crossAxisAuto ? 0.0f : (contentCross - sumLineCross - crossGapsTotal);
        if (containerStyle.alignContent == AlignContent::Stretch && crossFree > 0.0f && !crossAxisAuto) {
            float extraPerLine = crossFree / static_cast<float>(lines.size());
            for (float& lc : lineCrossSize) {
                lc += extraPerLine;
            }
            float cursor = 0.0f;
            for (size_t li = 0; li < lines.size(); ++li) {
                lineCrossStart[li] = cursor;
                cursor += lineCrossSize[li] + crossGapPx;
            }
        } else {
            PackingResult packing =
                packLines(containerStyle.alignContent, crossFree, crossGapPx, static_cast<int>(lines.size()));
            float cursor = packing.startOffset;
            for (size_t li = 0; li < lines.size(); ++li) {
                lineCrossStart[li] = cursor;
                cursor += lineCrossSize[li] + packing.between;
            }
        }
    }

    // Step 8 (main-axis position) + step 9 (cross-axis position, incl. align-self: stretch
    // re-measurement) + step 11 (*-reverse).
    for (size_t li = 0; li < lines.size(); ++li) {
        size_t lineStart = lines[li].first;
        size_t lineEnd = lines[li].second;
        size_t n = lineEnd - lineStart;

        int autoMarginCount = 0;
        for (size_t i = 0; i < n; ++i) {
            const FlexItem& it = items[lineStart + i];
            autoMarginCount += (it.marginMainAuto[0] ? 1 : 0) + (it.marginMainAuto[1] ? 1 : 0);
        }
        float leftover = mainAxisAuto ? 0.0f : (contentMain - lineUsedMain[li]);

        float startOffset;
        float between;
        float autoMarginShare = 0.0f;
        if (leftover > 0.0f && autoMarginCount > 0) {
            startOffset = 0.0f;
            between = mainGapPx;
            autoMarginShare = leftover / static_cast<float>(autoMarginCount);
        } else {
            PackingResult packing =
                packItems(containerStyle.justify, leftover, mainGapPx, static_cast<int>(n));
            startOffset = packing.startOffset;
            between = packing.between;
        }

        float mainFinalForReverse = mainAxisAuto ? lineUsedMain[li] : contentMain;

        float cursor = startOffset;
        for (size_t i = 0; i < n; ++i) {
            FlexItem& it = items[lineStart + i];
            float marginStart = it.marginMainAuto[0] ? autoMarginShare : it.marginMain[0];
            float marginEnd = it.marginMainAuto[1] ? autoMarginShare : it.marginMain[1];
            cursor += marginStart;
            it.mainPos = cursor;
            cursor += it.mainSize + marginEnd;
            if (i + 1 < n) {
                cursor += between;
            }

            if (reverse) {
                it.mainPos = mainFinalForReverse - it.mainPos - it.mainSize;
            }

            // Step 9: align-self (stretch re-measures the cross size against the final line size).
            AlignSelf align = it.alignSelf;
            float lineSize = lineCrossSize[li];
            if (align == AlignSelf::Stretch && it.crossAuto) {
                float target = lineSize - it.marginCross[0] - it.marginCross[1];
                if (target < 0.0f) {
                    target = 0.0f;
                }
                float measureAvailW = rowDir ? it.mainSize : target;
                // T-74: the stretched cross size (row) / the resolved main size (column) is a
                // definite used height imposed on the item.
                float imposedH = rowDir ? target : it.mainSize;
                layoutItem(it.node, it.suppressAspect, measureAvailW, imposedH);
                ElementData& cd = arena.element(it.node);
                if (rowDir) {
                    cd.box.width = it.mainSize;
                    cd.box.height = target;
                } else {
                    cd.box.height = it.mainSize;
                    cd.box.width = target;
                }
                it.crossSize = target;
                it.outerCross = it.crossSize + it.marginCross[0] + it.marginCross[1];
            }

            float crossPos;
            switch (align) {
                case AlignSelf::FlexEnd:
                    crossPos = lineCrossStart[li] + lineSize - it.outerCross + it.marginCross[0];
                    break;
                case AlignSelf::Center:
                    crossPos = lineCrossStart[li] + (lineSize - it.outerCross) * 0.5f + it.marginCross[0];
                    break;
                case AlignSelf::Stretch:
                case AlignSelf::FlexStart:
                case AlignSelf::Auto:
                default:
                    crossPos = lineCrossStart[li] + it.marginCross[0];
                    break;
            }
            if (wrapReverse) {
                crossPos = finalCross - crossPos - it.crossSize;  // T-336: mirrored cross axis.
            }
            it.crossPos = crossPos;

            ElementData& cd = arena.element(it.node);
            if (rowDir) {
                cd.box.x = it.mainPos;
                cd.box.y = it.crossPos;
            } else {
                cd.box.x = it.crossPos;
                cd.box.y = it.mainPos;
            }
        }
    }

    // Step 10 (main): container's own auto main size = the (single, since wrap is disabled when
    // mainAxisAuto) line's Sum(outer) + gaps.
    float finalMain = mainAxisAuto ? (lines.empty() ? 0.0f : lineUsedMain[0]) : contentMain;

    // A Text child's own size participates in the container's auto sizes (see the gather loop).
    if (mainAxisAuto) {
        finalMain = std::max(finalMain, rowDir ? textW : textH);
    }
    if (crossAxisAuto) {
        finalCross = std::max(finalCross, rowDir ? textH : textW);
    }

    return Vec2{finalMain, finalCross};
}

void LayoutEngine::layoutFlex(NodeArena& arena, NodeId node, float availW, float availH, float imposedH,
                              int depth) {
    XT_ASSERT(static_cast<uint32_t>(depth) < kMaxElementDepth);
    ElementData& data = arena.element(node);
    const ComputedStyle& style = data.style;
    // §6.4: margin/padding/border/width/min/max-width/flex-basis-along-a-row resolve their Percent
    // against the containing block's content *width* (refW), height/min/max-height against its
    // definite content *height* (refH, T-74); both match layoutBlock's identical convention.
    float refW = availW;
    float refH = availH;

    const auto [margin, padding, border] = resolveBoxEdges(style, refW);

    Length widthLen = style.base[prop::Width].len;
    Length heightLen = style.base[prop::Height].len;
    bool widthAuto = widthLen.unit == LengthUnit::Auto;
    // T-74, as in layoutBlock: "definite" (Px, or Percent against a definite refH) rather than
    // "not auto", plus an outer-imposed used height when the element's own height isn't definite.
    float definiteBorderBoxH = 0;
    bool heightDefinite = resolveDefiniteHeight(heightLen, refH, imposedH, definiteBorderBoxH);

    data.hasIntrinsic = 0;
    data.intrinsic = Vec2{0, 0};

    // Width: like layoutBlock, a definite width resolves directly; an auto width *fills* the
    // available width when one is given (matching normal block flow), and is otherwise "auto"
    // (isMainAuto candidate) -- this is the only route by which the shrink-to-fit case (an
    // absolutely positioned flex container measured via resolveAbsoluteBox's `layoutChild(...,
    // kUndefinedRef, ...)` call) reaches an undefined main/cross axis; see layout_absolute.cpp.
    float contentW;
    bool widthIsAxisAuto = false;
    if (!widthAuto) {
        float borderBoxW = resolveLength(widthLen, refW, 0);
        contentW = std::max(0.0f, borderBoxW - padding.left - padding.right - border.left - border.right);
    } else if (availW >= 0.0f) {
        Length mlLen = style.base[prop::MarginLeft].len;
        Length mrLen = style.base[prop::MarginRight].len;
        float mL = (mlLen.unit == LengthUnit::Auto) ? 0.0f : margin.left;
        float mR = (mrLen.unit == LengthUnit::Auto) ? 0.0f : margin.right;
        contentW =
            std::max(0.0f, availW - mL - mR - padding.left - padding.right - border.left - border.right);
    } else {
        widthIsAxisAuto = true;
        contentW = 0.0f;
    }
    if (!widthIsAxisAuto) {
        contentW =
            clampMinMax(contentW, style.base[prop::MinWidth].len, style.base[prop::MaxWidth].len, refW);
    }

    // §6.16 (T-328): `aspect-ratio` on the container's border box, as in layoutBlock: a definite
    // height with an auto width gives the width; a known width with an indefinite height gives the
    // height (a fill width is automatic, so a clamped height recomputes it once). Skipped while the
    // width is still a shrink-to-fit unknown.
    const float ratio = node == m_aspectSuppressed ? 0.0f : style.base[prop::AspectRatio].f;
    if (ratio > 0.0f && ((widthAuto && heightDefinite) || (!heightDefinite && !widthIsAxisAuto))) {
        const float extraW = padding.left + padding.right + border.left + border.right;
        const float extraH = padding.top + padding.bottom + border.top + border.bottom;
        const Vec2 size = containerAspectSize(style, ratio, refW, refH, extraW, extraH, heightDefinite,
                                              definiteBorderBoxH, contentW, widthAuto);
        contentW = std::max(0.0f, size.x - extraW);
        widthIsAxisAuto = false;
        definiteBorderBoxH = size.y;
        heightDefinite = true;
    }

    // Height: like layoutBlock, an indefinite height is always content-based (never fills availH).
    float contentH = 0.0f;
    bool heightIsAxisAuto = !heightDefinite;
    if (heightDefinite) {
        contentH =
            std::max(0.0f, definiteBorderBoxH - padding.top - padding.bottom - border.top - border.bottom);
    }

    bool rowDir = style.flexDir == FlexDirection::Row || style.flexDir == FlexDirection::RowReverse;
    bool mainAxisAuto = rowDir ? widthIsAxisAuto : heightIsAxisAuto;
    bool crossAxisAuto = rowDir ? heightIsAxisAuto : widthIsAxisAuto;
    float contentMain = rowDir ? contentW : contentH;
    float contentCross = rowDir ? contentH : contentW;
    // The single content-width reference passed to children (§6.4); undefined when the axis that
    // *is* the container's own content width isn't known yet.
    float childRefW = widthIsAxisAuto ? kUndefinedRef : contentW;
    // Likewise the content-height reference (§6.4, T-74): undefined while this container's own
    // height is content-based.
    float childRefH = heightIsAxisAuto ? kUndefinedRef : contentH;

    Vec2 finalSize = layoutFlexChildren(arena, node, contentMain, contentCross, mainAxisAuto, crossAxisAuto,
                                        childRefW, childRefH, depth);

    if (rowDir) {
        contentW = mainAxisAuto ? finalSize.x : contentW;
        contentH = crossAxisAuto ? finalSize.y : contentH;
    } else {
        contentH = mainAxisAuto ? finalSize.x : contentH;
        contentW = crossAxisAuto ? finalSize.y : contentW;
    }
    if (widthIsAxisAuto) {
        contentW =
            clampMinMax(contentW, style.base[prop::MinWidth].len, style.base[prop::MaxWidth].len, refW);
    }
    contentH =
        clampHeightMinMax(contentH, style.base[prop::MinHeight].len, style.base[prop::MaxHeight].len, refH);

    data.box.margin = margin;
    data.box.padding = padding;
    data.box.border = border;
    data.box.width = contentW + padding.left + padding.right + border.left + border.right;
    data.box.height = contentH + padding.top + padding.bottom + border.top + border.bottom;
    // T-336 (06 §6.5): the items' extent (it used to be the content box, so a flex container never
    // scrolled).
    storeScrollSize(arena, node, contentW, contentH);
}

}  // namespace xt
