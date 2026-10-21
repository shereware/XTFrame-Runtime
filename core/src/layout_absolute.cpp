#include <algorithm>
#include <string_view>

#include "layout_common.h"
#include "xt/assert.h"
#include "xt/layout.h"
#include "xt/node.h"
#include "xt/property.h"

/// Absolute positioning (docs/design/06-layout.md §6.6): a single global top-down pass, run once
/// from LayoutEngine::layout after the in-flow tree (layout_block.cpp) is fully sized, so every
/// containing block candidate's width/height is already final -- see layout.h's
/// layoutAbsolutePass doc comment for why this isn't done inline in layoutBlock's own step 8.
/// See docs/tasks/T-25.md.

namespace xt {

namespace {

/// True when `node` has at least one child that takes part in its normal flow (an Element that is
/// neither `display: none`, a <camera>, nor absolutely positioned). T-74 part B uses it to pick
/// between the two shrink-to-fit width strategies: wrap the widest in-flow child, or (text only)
/// wrap the text.
bool hasInFlowElementChild(NodeArena& arena, NodeId node) {
    bool found = false;
    arena.forEachChild(node, [&](NodeId c) {
        if (found || arena.kind(c) != NodeKind::Element) {
            return;
        }
        const ElementData& cd = arena.element(c);
        if (cd.style.display == Display::None || cd.cameraNode != kInvalidNode ||
            cd.style.position == Position::Absolute) {
            return;
        }
        found = true;
    });
    return found;
}

}  // namespace

void LayoutEngine::resolveAbsoluteBox(NodeArena& arena, NodeId node, Rect cbPaddingAbs, int depth) {
    XT_ASSERT(static_cast<uint32_t>(depth) < kMaxElementDepth);
    ElementData& data = arena.element(node);
    const ComputedStyle& style = data.style;
    float cbW = cbPaddingAbs.w;
    float cbH = cbPaddingAbs.h;

    const auto [margin, padding, border] = resolveBoxEdges(style, cbW);

    Length widthLen = style.base[prop::Width].len;
    Length heightLen = style.base[prop::Height].len;
    Length leftLen = style.base[prop::Left].len;
    Length rightLen = style.base[prop::Right].len;
    Length topLen = style.base[prop::Top].len;
    Length bottomLen = style.base[prop::Bottom].len;
    bool widthAuto = widthLen.unit == LengthUnit::Auto;
    bool heightAuto = heightLen.unit == LengthUnit::Auto;
    bool leftAuto = leftLen.unit == LengthUnit::Auto;
    bool rightAuto = rightLen.unit == LengthUnit::Auto;
    bool topAuto = topLen.unit == LengthUnit::Auto;
    bool bottomAuto = bottomLen.unit == LengthUnit::Auto;

    Length mlLen = style.base[prop::MarginLeft].len;
    Length mrLen = style.base[prop::MarginRight].len;
    Length mtLen = style.base[prop::MarginTop].len;
    Length mbLen = style.base[prop::MarginBottom].len;
    float mL = (mlLen.unit == LengthUnit::Auto) ? 0.0f : margin.left;
    float mR = (mrLen.unit == LengthUnit::Auto) ? 0.0f : margin.right;
    float mT = (mtLen.unit == LengthUnit::Auto) ? 0.0f : margin.top;
    float mB = (mbLen.unit == LengthUnit::Auto) ? 0.0f : margin.bottom;

    // §6.10 intrinsic size, cached the same way layoutBlock does; used as the shrink-to-fit
    // fallback below (see the "uncovered cases" note in the T-25 report: a non-replaced,
    // non-flex element's shrink-to-fit width/height -- with no min/max-content measurement in
    // this engine -- degrades to 0). T-26 adds a `display: flex` special case (see `w` below).
    std::string_view tag = string(data.tag);
    bool replaced = isReplacedTag(tag);
    Vec2 intrinsic = replaced ? intrinsicSize(arena, node) : Vec2{0, 0};
    data.intrinsic = intrinsic;
    data.hasIntrinsic = replaced ? 1 : 0;
    data.box.intrinsicWidth = intrinsic.x;
    data.box.intrinsicHeight = intrinsic.y;

    // Height is computed before width (rather than the source order left/top/right/bottom
    // suggests) purely so the flex shrink-to-fit special case below (width) can pass an
    // already-known height in as the cross-axis available size. It does not otherwise depend on
    // width -- except in the content-height case (T-74, `heightDeterminate == false`), which is
    // finished off after the width below.
    //
    // §6.4/§6.6: height/top/bottom reference the containing block's padding box *height* (cbH),
    // which is always definite here, so resolveLength suffices (no resolveHeightLength dance).
    float h = 0;
    bool heightDeterminate = true;
    if (!heightAuto) {
        h = resolveLength(heightLen, cbH, 0);
    } else if (!topAuto && !bottomAuto) {
        float top = resolveLength(topLen, cbH, 0);
        float bottom = resolveLength(bottomLen, cbH, 0);
        h = cbH - top - bottom - mT - mB;
        if (h < 0.0f) {
            h = 0.0f;
        }
    } else if (replaced) {
        h = intrinsic.y;  // shrink-to-fit.
    } else {
        // T-74 part B: the height follows the content, laid out against the width resolved below.
        heightDeterminate = false;
    }
    if (heightDeterminate) {
        h = clampHeightMinMax(h, style.base[prop::MinHeight].len, style.base[prop::MaxHeight].len, cbH);
    }

    float w;
    // §6.6/§6.9 (T-89): an absolutely positioned `display: flex` container with `width: auto`
    // shrink-to-fits its main axis to its children -- Σ outer main-axis size + gaps (row), or max
    // outer cross size (column), per §6.6's flex-intrinsic-width rule -- which this engine can only
    // produce by actually running the flex algorithm (layoutFlex, via layoutChild) rather than the
    // generic `w = intrinsic.x` shrink-to-fit fallback (T-25's documented "uncovered cases": that
    // fallback has no min/max-content measurement and degrades non-replaced elements to 0).
    // Reaching this point already means width is auto and left+right are *not* both non-auto (that
    // combination took the branch above), so this covers every remaining absolute/auto-width flex
    // container -- not just the left-and-right-both-auto case (T-89's actual bug: a container with
    // e.g. `left` set and `right` auto used to fall through to the block-stacking shrink-to-fit
    // below instead, which lays out children as blocks and ignores flex-shrink). An absolutely
    // positioned flex container with an already-determined width runs the flex algorithm further
    // down instead (T-79 item 2).
    // §6.16 (T-328): `aspect-ratio` on the border box. A height fixed by `height` or top+bottom
    // gives an otherwise auto width; a width fixed any other way gives an auto height (below).
    const float ratio = style.base[prop::AspectRatio].f;
    const bool heightFromProps = !heightAuto || (!topAuto && !bottomAuto);
    const bool widthFromProps = !widthAuto || (!leftAuto && !rightAuto);
    const AspectMinMax aspectBounds = ratio > 0.0f ? aspectMinMax(style, cbW, cbH) : AspectMinMax{};
    bool flexShrinkToFit = style.display == Display::Flex && widthAuto;
    // Set once the element's children already carry their final boxes, so the block-stacking tail
    // below must not redo (and the auto-height pass must not repeat) the work.
    bool childrenLaidOut = false;
    float cursorY = 0;
    if (!widthAuto) {
        w = resolveLength(widthLen, cbW, 0);
    } else if (!leftAuto && !rightAuto) {
        float left = resolveLength(leftLen, cbW, 0);
        float right = resolveLength(rightLen, cbW, 0);
        w = cbW - left - right - mL - mR;
        if (w < 0.0f) {
            w = 0.0f;
        }
    } else if (ratio > 0.0f && heightFromProps) {
        w = resolveAspect(kUndefinedRef, h, ratio, aspectBounds).x;
    } else if (flexShrinkToFit) {
        // Measurement pass (§6.9 step 2's content-size fallback, run for the container itself):
        // an auto main axis makes layoutFlexChildren skip flex-grow/shrink (free forced to 0), so
        // this yields the *natural* (un-shrunk) main-axis sum -- exactly §6.6's flex-intrinsic-
        // width formula. The re-arrangement pass below (`flexArranged`, now unconditional for any
        // flex container) re-runs the real algorithm against the clamped width so flex-shrink can
        // still act when that natural sum overflows the containing block (T-89 test 5).
        layoutChild(arena, node, kUndefinedRef, cbH, heightDeterminate ? h : kUndefinedRef, depth + 1);
        w = data.box.width;
        if (!heightDeterminate) {
            h = data.box.height;  // Also shrink-to-fit: layoutFlex just measured it too.
            h = clampHeightMinMax(h, style.base[prop::MinHeight].len, style.base[prop::MaxHeight].len, cbH);
            heightDeterminate = true;
        }
        // §6.6: "结果夾在 [0, cbW]" -- the measured intrinsic width is clamped to the containing
        // block before the author's own min/max-width (applied just below, for every branch).
        // T-201: not a `white-space: nowrap` box, which cannot wrap narrower (see below).
        if (w > cbW && style.whiteSpace != WhiteSpace::NoWrap) {
            w = cbW;
        }
        if (w < 0.0f) {
            w = 0.0f;
        }
    } else if (replaced) {
        w = intrinsic.x;  // shrink-to-fit (see comment above).
    } else if (hasInFlowElementChild(arena, node)) {
        // T-201 (CSS 2.1 §10.3.7 shrink-to-fit, replacing T-74 part B's "widest child laid out
        // against the containing block", where an auto-width child filled it): the max-content
        // width, clamped to [0, cbW] -- except for `white-space: nowrap`, whose min-content width
        // is its max-content width (it cannot wrap narrower).
        w = maxContentWidth(arena, node, depth);
        if (w > cbW && style.whiteSpace != WhiteSpace::NoWrap) {
            w = cbW;
        }
        if (w < 0.0f) {
            w = 0.0f;
        }
    } else {
        // §6.8 / docs/tasks/T-30.md: a text-only element shrink-wraps to its text's unwrapped
        // main-axis size. Still 0 for a non-replaced element with no Text child (T-25's
        // documented "no min/max-content measurement" limitation).
        TextIntrinsic text = measureTextChildren(arena, node);
        float textContentW = style.writingMode == WritingMode::HorizontalTb ? text.main : text.width;
        w = textContentW > 0.0f ? textContentW + padding.left + padding.right + border.left + border.right
                                : 0.0f;
    }
    w = clampMinMax(w, style.base[prop::MinWidth].len, style.base[prop::MaxWidth].len, cbW);
    if (ratio > 0.0f && !heightFromProps) {
        // A shrink-to-fit width is automatic: a clamped height recomputes it once.
        const Vec2 size = resolveAspect(w, kUndefinedRef, ratio, aspectBounds, !widthFromProps);
        w = size.x;
        h = size.y;
        heightDeterminate = true;
    }

    float contentW = std::max(0.0f, w - padding.left - padding.right - border.left - border.right);

    // T-79 item 2 / T-89 (§6.9): every absolutely positioned `display: flex` container -- whether
    // its width was already determined (an explicit `width`, or left+right) or just measured and
    // clamped by `flexShrinkToFit` above -- arranges its children with the *final*, definite
    // content box via the real flex algorithm (including flex-grow/shrink); the block-stacking
    // tail below would lay them out as blocks instead. Re-running this for the `flexShrinkToFit`
    // case too (rather than trusting its measurement pass's un-shrunk child boxes) is what lets
    // flex-shrink act once the measured width was clamped down to cbW (T-89 test 5); when nothing
    // was clamped, `free` comes out to 0 and this pass reproduces the measurement pass's layout
    // exactly. An auto height is the flex container's own content size on whichever axis it is
    // (the cross axis in a row direction, the main axis in a column one), so it is resolved here
    // too, in place of the block-content measurement below.
    bool flexArranged = style.display == Display::Flex;
    if (flexArranged) {
        bool rowDir = style.flexDir == FlexDirection::Row || style.flexDir == FlexDirection::RowReverse;
        float contentH = heightDeterminate
                             ? std::max(0.0f, h - padding.top - padding.bottom - border.top - border.bottom)
                             : 0.0f;
        bool heightIsAxisAuto = !heightDeterminate;
        Vec2 finalSize = layoutFlexChildren(
            arena, node, rowDir ? contentW : contentH, rowDir ? contentH : contentW,
            /*mainAxisAuto=*/!rowDir && heightIsAxisAuto, /*crossAxisAuto=*/rowDir && heightIsAxisAuto,
            contentW, heightDeterminate ? contentH : kUndefinedRef, depth);
        if (heightIsAxisAuto) {
            contentH = rowDir ? finalSize.y : finalSize.x;
            h = contentH + padding.top + padding.bottom + border.top + border.bottom;
            h = clampHeightMinMax(h, style.base[prop::MinHeight].len, style.base[prop::MaxHeight].len, cbH);
            heightDeterminate = true;
        }
        childrenLaidOut = true;
    }

    // 06 §6.15 (T-338): an absolutely positioned grid container arranges its items against the
    // final width (an auto width was measured as its columns by maxContentWidth above); an auto
    // height is its rows.
    const bool gridArranged = style.display == Display::Grid;
    if (gridArranged) {
        const float contentH =
            heightDeterminate ? std::max(0.0f, h - padding.top - padding.bottom - border.top - border.bottom)
                              : 0.0f;
        const Vec2 finalSize =
            layoutGridChildren(arena, node, contentW, contentH, false, !heightDeterminate, depth);
        if (!heightDeterminate) {
            h = finalSize.y + padding.top + padding.bottom + border.top + border.bottom;
            h = clampHeightMinMax(h, style.base[prop::MinHeight].len, style.base[prop::MaxHeight].len, cbH);
            heightDeterminate = true;
        }
        childrenLaidOut = true;
    }

    // T-74 part B: an auto height that neither top+bottom nor an intrinsic size fixed follows the
    // content, measured with the width just resolved (and *before* `y` is computed from `bottom`,
    // which needs the final height).
    if (!heightDeterminate) {
        cursorY = layoutBlockChildren(arena, node, contentW, kUndefinedRef, depth);
        h = cursorY + padding.top + padding.bottom + border.top + border.bottom;
        h = clampHeightMinMax(h, style.base[prop::MinHeight].len, style.base[prop::MaxHeight].len, cbH);
        heightDeterminate = true;
        childrenLaidOut = true;
    }

    float x;
    if (!leftAuto) {
        x = resolveLength(leftLen, cbW, 0) + mL;
    } else if (!rightAuto) {
        x = cbW - resolveLength(rightLen, cbW, 0) - w - mR;
    } else {
        x = 0.0f;  // static-position fallback, §6.6 "第一版取 0".
    }

    float y;
    if (!topAuto) {
        y = resolveLength(topLen, cbH, 0) + mT;
    } else if (!bottomAuto) {
        y = cbH - resolveLength(bottomLen, cbH, 0) - h - mB;
    } else {
        y = 0.0f;
    }

    data.box.margin = margin;
    data.box.padding = padding;
    data.box.border = border;
    data.box.width = w;
    data.box.height = h;

    if (flexArranged || gridArranged) {
        // layoutFlexChildren / layoutGridChildren wrote every child's box above; only this box's own
        // scrollable content size is left: the items' extent (T-336, 06 §6.5), as in layoutFlex.
        storeScrollSize(arena, node, contentW,
                        std::max(0.0f, h - padding.top - padding.bottom - border.top - border.bottom));
    } else {
        float contentH = std::max(0.0f, h - padding.top - padding.bottom - border.top - border.bottom);
        if (!childrenLaidOut) {
            cursorY = layoutBlockChildren(arena, node, contentW, contentH, depth);
        }
        storeScrollSize(arena, node, contentW, cursorY > contentH ? cursorY : contentH);  // T-336.
    }

    arena.common(node).visible = 1;
    // x, y above are relative to the containing block's padding box; the caller (
    // layoutAbsoluteChildren) passes them in through cbPaddingAbs / *ContentAbs and performs the
    // conversion back to "relative to node's own DOM parent's content box" (LayoutBox's usual
    // convention), since node's containing block need not be its DOM parent (§6.6, see
    // "absolute_nested"). Storing the intermediate box-x/y here first keeps this function's return
    // surface simple (it only writes into `data.box`); see resolveAbsoluteBox's caller.
    data.box.x = x;
    data.box.y = y;
}

void LayoutEngine::layoutAbsoluteChildren(NodeArena& arena, NodeId node, NodeId cbNode, Rect cbPaddingAbs,
                                          float parentContentAbsX, float parentContentAbsY, int depth) {
    XT_ASSERT(static_cast<uint32_t>(depth) < kMaxElementDepth);
    arena.forEachChild(node, [&](NodeId c) {
        if (arena.kind(c) != NodeKind::Element) {
            return;
        }
        ElementData& cd = arena.element(c);
        if (cd.style.display == Display::None || cd.cameraNode != kInvalidNode) {
            return;  // Already zeroed + visible = 0 by the in-flow pass (layout_block.cpp).
        }

        if (cd.style.position == Position::Absolute) {
            // Sets cd.box fully, with box.x/y temporarily relative to the containing block's
            // padding box (cbPaddingAbs); converted to "relative to c's DOM parent's content box"
            // right below, matching every other LayoutBox's convention.
            resolveAbsoluteBox(arena, c, cbPaddingAbs, depth + 1);
            cd.box.x = cbPaddingAbs.x + cd.box.x - parentContentAbsX;
            cd.box.y = cbPaddingAbs.y + cd.box.y - parentContentAbsY;
        }

        float cAbsX = parentContentAbsX + cd.box.x;
        float cAbsY = parentContentAbsY + cd.box.y;
        Rect cContent = cd.box.contentBox();

        NodeId childCb = cbNode;
        Rect childCbPad = cbPaddingAbs;
        if (cd.style.position != Position::Static) {
            Rect cPad = cd.box.paddingBox();
            childCb = c;
            childCbPad = Rect{cAbsX + cPad.x, cAbsY + cPad.y, cPad.w, cPad.h};
        }
        layoutAbsoluteChildren(arena, c, childCb, childCbPad, cAbsX + cContent.x, cAbsY + cContent.y,
                               depth + 1);
    });
}

void LayoutEngine::layoutAbsolutePass(NodeArena& arena, NodeId root) {
    ElementData& rootData = arena.element(root);
    Rect pb = rootData.box.paddingBox();  // root's own border-box origin is (0, 0) by convention.
    Rect rootPaddingAbs{pb.x, pb.y, pb.w, pb.h};
    Rect content = rootData.box.contentBox();
    layoutAbsoluteChildren(arena, root, root, rootPaddingAbs, content.x, content.y, 0);
}

}  // namespace xt
