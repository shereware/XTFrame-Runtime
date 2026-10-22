#include <algorithm>
#include <string_view>

#include "layout_common.h"
#include "xt/assert.h"
#include "xt/layout.h"
#include "xt/node.h"
#include "xt/property.h"

/// Block layout (Docs/design/06-layout.md §6.5) and the in-flow child-stacking helper shared with
/// the absolute pass's own children (layout_absolute.cpp). See Docs/tasks/T-25.md.

namespace xt {

void storeScrollSize(NodeArena& arena, NodeId node, float contentW, float minHeight) {
    Vec2 extent{contentW, minHeight};
    arena.forEachChild(node, [&](NodeId c) {
        if (arena.kind(c) == NodeKind::Text) {
            const TextNodeData& text = arena.text(c);
            extent.x = std::max(extent.x, text.width);
            extent.y = std::max(extent.y, text.height);
            return;
        }
        if (arena.kind(c) != NodeKind::Element) {
            return;
        }
        const ElementData& cd = arena.element(c);
        if (cd.style.display == Display::None || cd.cameraNode != kInvalidNode ||
            cd.style.position == Position::Absolute) {
            return;
        }
        const LayoutBox& b = cd.box;
        extent.x = std::max(extent.x, b.x + b.width + b.margin.right);
        extent.y = std::max(extent.y, b.y + b.height + b.margin.bottom);
    });
    LayoutBox& box = arena.element(node).box;
    box.scrollWidth = extent.x;
    box.scrollHeight = extent.y;
}

float LayoutEngine::maxContentWidth(NodeArena& arena, NodeId node, int depth) {
    XT_ASSERT(static_cast<uint32_t>(depth) < kMaxElementDepth);
    ElementData& data = arena.element(node);
    const ComputedStyle& style = data.style;
    const Edges padding =
        resolveEdgesPx(style.base[prop::PaddingTop].len, style.base[prop::PaddingRight].len,
                       style.base[prop::PaddingBottom].len, style.base[prop::PaddingLeft].len, kUndefinedRef);
    const Edges border = resolveEdgesPx(
        style.base[prop::BorderTopWidth].len, style.base[prop::BorderRightWidth].len,
        style.base[prop::BorderBottomWidth].len, style.base[prop::BorderLeftWidth].len, kUndefinedRef);
    const float extra = padding.left + padding.right + border.left + border.right;
    const Length minLen = style.base[prop::MinWidth].len;
    const Length maxLen = style.base[prop::MaxWidth].len;
    const Length widthLen = style.base[prop::Width].len;
    if (widthLen.unit != LengthUnit::Auto && widthLen.unit != LengthUnit::Percent) {
        const float borderBox = resolveLength(widthLen, kUndefinedRef, 0);
        return clampMinMax(std::max(0.0f, borderBox - extra), minLen, maxLen, kUndefinedRef) + extra;
    }
    float content = 0;
    if (isReplacedTag(string(data.tag))) {
        content = intrinsicSize(arena, node).x;
    } else if (style.display == Display::Grid) {
        content = gridContentWidth(arena, node, depth);  // 06 §6.15 (T-338): the columns.
    } else {
        const bool row = style.display == Display::Flex &&
                         (style.flexDir == FlexDirection::Row || style.flexDir == FlexDirection::RowReverse);
        float sum = 0;
        float widest = 0;
        int count = 0;
        arena.forEachChild(node, [&](NodeId c) {
            if (arena.kind(c) != NodeKind::Element) {
                return;
            }
            const ElementData& cd = arena.element(c);
            if (cd.style.display == Display::None || cd.cameraNode != kInvalidNode ||
                cd.style.position == Position::Absolute) {
                return;
            }
            const Length ml = cd.style.base[prop::MarginLeft].len;
            const Length mr = cd.style.base[prop::MarginRight].len;
            const float margins = (ml.unit == LengthUnit::Auto ? 0.0f : resolveLength(ml, kUndefinedRef, 0)) +
                                  (mr.unit == LengthUnit::Auto ? 0.0f : resolveLength(mr, kUndefinedRef, 0));
            const float outer = maxContentWidth(arena, c, depth + 1) + margins;
            sum += outer;
            widest = std::max(widest, outer);
            ++count;
        });
        if (row && count > 1) {
            sum += resolveLength(style.base[prop::ColumnGap].len, kUndefinedRef, 0) *
                   static_cast<float>(count - 1);
        }
        const TextIntrinsic text = measureTextChildren(arena, node);
        const float textW = style.writingMode == WritingMode::HorizontalTb ? text.main : text.width;
        content = std::max(row ? sum : widest, textW);
    }
    return clampMinMax(content, minLen, maxLen, kUndefinedRef) + extra;
}

float LayoutEngine::layoutBlockChildren(NodeArena& arena, NodeId node, float contentW, float contentH,
                                        int depth) {
    XT_ASSERT(static_cast<uint32_t>(depth) < kMaxElementDepth);
    float cursorY = 0;
    const ComputedStyle& ownStyle = arena.element(node).style;
    arena.forEachChild(node, [&](NodeId c) {
        if (arena.kind(c) == NodeKind::Text) {
            // §6.8: the available main size is this element's content width (horizontal writing
            // mode) or its content height (vertical; undefined -- i.e. an auto height -- means
            // unbounded). The text's own main-axis extent adds to the block's content height.
            float availMain = ownStyle.writingMode == WritingMode::HorizontalTb ? contentW : contentH;
            layoutTextNode(arena, c, ownStyle, availMain);
            arena.common(c).visible = 1;
            cursorY += arena.text(c).height;
            return;
        }
        if (arena.kind(c) != NodeKind::Element) {
            return;  // Scene subtree nodes aren't DOM children.
        }
        ElementData& cd = arena.element(c);
        // Docs/design/19-3d-space.md §19.4: a <camera> element is treated as display:none (no box,
        // no subtree layout), detected by ElementData::cameraNode rather than its style.
        if (cd.style.display == Display::None || cd.cameraNode != kInvalidNode) {
            cd.box = LayoutBox{};
            arena.common(c).visible = 0;
            return;
        }
        arena.common(c).visible = 1;
        if (cd.style.position == Position::Absolute) {
            return;  // §6.6, resolved later by LayoutEngine::layoutAbsolutePass.
        }

        // §6.4 (T-74): a child's Percent height resolves against this element's own content height,
        // and only when that height is definite (contentH == kUndefinedRef otherwise).
        layoutChild(arena, c, contentW, contentH, kUndefinedRef, depth + 1);

        Length mlLen = cd.style.base[prop::MarginLeft].len;
        Length mrLen = cd.style.base[prop::MarginRight].len;
        if (mlLen.unit == LengthUnit::Auto && mrLen.unit == LengthUnit::Auto) {
            cd.box.x = (contentW - cd.box.width) * 0.5f;
        } else {
            cd.box.x = cd.box.margin.left;
        }
        cd.box.y = cursorY + cd.box.margin.top;

        if (cd.style.position == Position::Relative) {
            Length leftLen = cd.style.base[prop::Left].len;
            Length rightLen = cd.style.base[prop::Right].len;
            Length topLen = cd.style.base[prop::Top].len;
            Length bottomLen = cd.style.base[prop::Bottom].len;
            if (leftLen.unit != LengthUnit::Auto) {
                cd.box.x += resolveLength(leftLen, contentW, 0);
            } else if (rightLen.unit != LengthUnit::Auto) {
                cd.box.x -= resolveLength(rightLen, contentW, 0);
            }
            // §6.4: top/bottom reference the containing block's content height; an indefinite one
            // makes a Percent offset resolve to 0 (resolveLength's undefined-reference fallback).
            if (topLen.unit != LengthUnit::Auto) {
                cd.box.y += resolveLength(topLen, contentH, 0);
            } else if (bottomLen.unit != LengthUnit::Auto) {
                cd.box.y -= resolveLength(bottomLen, contentH, 0);
            }
        }

        cursorY += cd.box.margin.top + cd.box.height + cd.box.margin.bottom;
    });
    return cursorY;
}

void LayoutEngine::layoutBlock(NodeArena& arena, NodeId node, float availW, float availH, float imposedH,
                               int depth) {
    XT_ASSERT(static_cast<uint32_t>(depth) < kMaxElementDepth);
    ElementData& data = arena.element(node);
    const ComputedStyle& style = data.style;
    // §6.4: width/min/max-width/padding/margin/left/right's % reference is the containing block's
    // content width, i.e. availW here (an undefined availW -- shouldn't normally happen for a
    // normal-flow call, body always receives the viewport width -- conservatively resolves % to 0
    // via resolveLength's undefined-reference fallback). height/min/max-height/top/bottom reference
    // the containing block's *definite* content height (availH, T-74); an undefined one turns a
    // Percent height into `auto` and a Percent min/max-height into "no bound".
    float refW = availW;
    float refH = availH;

    const auto [margin, padding, border] = resolveBoxEdges(style, refW);

    Length widthLen = style.base[prop::Width].len;
    Length heightLen = style.base[prop::Height].len;
    bool widthAuto = widthLen.unit == LengthUnit::Auto;
    // T-74: "definite" replaces the old "not auto" test -- a Percent height against an indefinite
    // containing block height behaves as `auto`. An `imposedH` (flex cross stretch / flex column
    // main size) only applies when the element's own height isn't definite by itself.
    float definiteBorderBoxH = 0;
    bool heightDefinite = resolveDefiniteHeight(heightLen, refH, imposedH, definiteBorderBoxH);

    // §6.10: img/anim intrinsic size, cached on ElementData for later tasks (e.g. T-26 flex-basis).
    std::string_view tag = string(data.tag);
    bool replaced = isReplacedTag(tag);
    Vec2 intrinsic = replaced ? intrinsicSize(arena, node) : Vec2{0, 0};
    data.intrinsic = intrinsic;
    data.hasIntrinsic = replaced ? 1 : 0;
    data.box.intrinsicWidth = intrinsic.x;
    data.box.intrinsicHeight = intrinsic.y;

    float contentW;
    float contentH = 0;
    bool contentHFixed = false;  // true once contentH below is already final (aspect-ratio case).

    // §6.16 (T-328): an explicit `aspect-ratio` sizes the border box (this engine's sizing box); a
    // replaced element's `auto` keeps its natural ratio, which sizes the content box (§6.10).
    const float extraW = padding.left + padding.right + border.left + border.right;
    const float extraH = padding.top + padding.bottom + border.top + border.bottom;
    // A stretched flex item ignores its explicit ratio (§6.16, m_aspectSuppressed).
    const float explicitRatio = node == m_aspectSuppressed ? 0.0f : style.base[prop::AspectRatio].f;
    const bool natural = replaced && intrinsic.x > 0.0f && intrinsic.y > 0.0f;
    const float ratio = explicitRatio > 0.0f ? explicitRatio : (natural ? intrinsic.x / intrinsic.y : 0.0f);
    const bool widthFromRatio = ratio > 0.0f && widthAuto && heightDefinite;

    if (!widthAuto) {
        float borderBoxW = resolveLength(widthLen, refW, 0);
        contentW = std::max(0.0f, borderBoxW - extraW);
    } else if (widthFromRatio) {
        contentW = 0.0f;  // Derived from the definite height below.
    } else if (natural) {
        contentW = intrinsic.x;  // §6.10: neither side given -> the natural size.
    } else {
        Length mlLen = style.base[prop::MarginLeft].len;
        Length mrLen = style.base[prop::MarginRight].len;
        float mL = (mlLen.unit == LengthUnit::Auto) ? 0.0f : margin.left;
        float mR = (mrLen.unit == LengthUnit::Auto) ? 0.0f : margin.right;
        if (availW < 0.0f) {
            // No available width at all: this is a shrink-to-fit measurement (an absolutely
            // positioned box, or a flex item's content-size flex basis). T-201: the max-content
            // width -- its text's unwrapped size (§6.8/docs/tasks/T-30.md) or its widest in-flow
            // child's, a flex row child's summed -- like CSS's `width: auto` flex item.
            contentW = maxContentWidth(arena, node, depth) - extraW;
        } else {
            contentW = availW - mL - mR - extraW;
        }
        if (contentW < 0.0f) {
            contentW = 0.0f;
        }
    }
    if (!widthFromRatio) {
        contentW =
            clampMinMax(contentW, style.base[prop::MinWidth].len, style.base[prop::MaxWidth].len, refW);
    }

    if (ratio > 0.0f && (widthFromRatio || !heightDefinite)) {
        // §6.16 / §6.10: one axis definite, the other auto -> the other follows the ratio (both
        // auto -> the width is the definite axis). min/max-width/height bound the content box.
        const float boxW = explicitRatio > 0.0f ? extraW : 0.0f;
        const float boxH = explicitRatio > 0.0f ? extraH : 0.0f;
        const AspectMinMax bounds = aspectMinMaxPlusBox(style, refW, refH, boxW, boxH);
        Vec2 size;
        if (widthFromRatio) {
            const float definiteH =
                clampHeightMinMax(std::max(0.0f, definiteBorderBoxH - extraH),
                                  style.base[prop::MinHeight].len, style.base[prop::MaxHeight].len, refH);
            size = resolveAspect(kUndefinedRef, definiteH + boxH, ratio, bounds);
        } else if (natural && explicitRatio <= 0.0f && widthAuto && contentW == intrinsic.x &&
                   clampHeightMinMax(intrinsic.y, style.base[prop::MinHeight].len,
                                     style.base[prop::MaxHeight].len, refH) == intrinsic.y) {
            size = Vec2{intrinsic.x, intrinsic.y};  // §6.10: the natural size, exactly.
        } else {
            // A fill/stretch or natural width is automatic: a clamped height recomputes it once.
            size = resolveAspect(contentW + boxW, kUndefinedRef, ratio, bounds, widthAuto);
        }
        contentW = std::max(0.0f, size.x - boxW);
        contentH = std::max(0.0f, size.y - boxH);
        contentHFixed = true;
    }

    // This element's *own* content height, when definite: §6.8's available main size for a vertical
    // writing-mode Text child, and (T-74) the % reference for every child's own height. An auto
    // (content-based) height stays undefined -- it isn't known yet here.
    float definiteContentH = kUndefinedRef;
    if (contentHFixed) {
        definiteContentH = contentH;
    } else if (heightDefinite) {
        definiteContentH =
            std::max(0.0f, definiteBorderBoxH - padding.top - padding.bottom - border.top - border.bottom);
    }

    float cursorY = layoutBlockChildren(arena, node, contentW, definiteContentH, depth);

    if (!contentHFixed) {
        contentH = heightDefinite ? definiteContentH : cursorY;
    }
    contentH =
        clampHeightMinMax(contentH, style.base[prop::MinHeight].len, style.base[prop::MaxHeight].len, refH);

    data.box.margin = margin;
    data.box.padding = padding;
    data.box.border = border;
    data.box.width = contentW + padding.left + padding.right + border.left + border.right;
    data.box.height = contentH + padding.top + padding.bottom + border.top + border.bottom;
    // T-336 (06 §6.5): the horizontal extent too (it used to be the content width).
    storeScrollSize(arena, node, contentW, cursorY > contentH ? cursorY : contentH);
}

}  // namespace xt
