#pragma once

#include <algorithm>

#include "xt/layout.h"
#include "xt/node.h"
#include "xt/property.h"

/// Box helpers shared by the block, flex, grid and absolute layout passes
/// (Code/Runtime/core/src/layout_{block,flex,grid,absolute}.cpp; T-414 C2b). Each of these used to be a private
/// copy in every pass; the arithmetic is unchanged, so the computed boxes are bit-identical.

namespace xt {

inline Edges resolveEdgesPx(const Length& t, const Length& r, const Length& b, const Length& l, float refW) {
    return Edges{resolveLength(t, refW, 0), resolveLength(r, refW, 0), resolveLength(b, refW, 0),
                 resolveLength(l, refW, 0)};
}

/// CSS-style clamp: max(min, min(value, max)); an Auto min/max length means "no bound" (min
/// defaults to 0 either way since Length{Auto} resolves to the 0 fallback via resolveLength).
inline float clampMinMax(float value, Length minLen, Length maxLen, float refW) {
    float clamped = value;
    if (maxLen.unit != LengthUnit::Auto) {
        float maxV = resolveLength(maxLen, refW, 0);
        if (clamped > maxV) {
            clamped = maxV;
        }
    }
    float minV = resolveLength(minLen, refW, 0);
    if (clamped < minV) {
        clamped = minV;
    }
    return clamped;
}

/// An element's margin, padding and border widths, every Percent against `refW` (§6.4).
struct BoxEdges {
    Edges margin;
    Edges padding;
    Edges border;
};

inline BoxEdges resolveBoxEdges(const ComputedStyle& style, float refW) {
    BoxEdges e;
    e.margin = resolveEdgesPx(style.base[prop::MarginTop].len, style.base[prop::MarginRight].len,
                              style.base[prop::MarginBottom].len, style.base[prop::MarginLeft].len, refW);
    e.padding = resolveEdgesPx(style.base[prop::PaddingTop].len, style.base[prop::PaddingRight].len,
                               style.base[prop::PaddingBottom].len, style.base[prop::PaddingLeft].len, refW);
    e.border =
        resolveEdgesPx(style.base[prop::BorderTopWidth].len, style.base[prop::BorderRightWidth].len,
                       style.base[prop::BorderBottomWidth].len, style.base[prop::BorderLeftWidth].len, refW);
    return e;
}

/// T-74: whether the border-box height is definite -- the element's own `height` against a definite
/// `refH`, or else an outer-imposed used height (`imposedH >= 0`: flex cross stretch / flex column
/// main size). The height lands in `outBorderBoxH` (0 when indefinite).
inline bool resolveDefiniteHeight(Length heightLen, float refH, float imposedH, float& outBorderBoxH) {
    outBorderBoxH = 0;
    bool heightDefinite = resolveHeightLength(heightLen, refH, outBorderBoxH);
    if (!heightDefinite && imposedH >= 0.0f) {
        outBorderBoxH = imposedH;
        heightDefinite = true;
    }
    return heightDefinite;
}

/// §6.16 (T-328): aspectMinMax(style, ...) moved out from the content box by `boxW` / `boxH` (an
/// unbounded max stays unbounded).
inline AspectMinMax aspectMinMaxPlusBox(const ComputedStyle& style, float refW, float refH, float boxW,
                                        float boxH) {
    AspectMinMax bounds = aspectMinMax(style, refW, refH);
    bounds.minW += boxW;
    bounds.minH += boxH;
    bounds.maxW = bounds.maxW < kFlexNoMax ? bounds.maxW + boxW : kFlexNoMax;
    bounds.maxH = bounds.maxH < kFlexNoMax ? bounds.maxH + boxH : kFlexNoMax;
    return bounds;
}

/// §6.16 (T-328): a flex or grid container's border box from `aspect-ratio` -- a definite height
/// (min/max-clamped) gives the width, otherwise the known content width gives the height.
inline Vec2 containerAspectSize(const ComputedStyle& style, float ratio, float refW, float refH, float extraW,
                                float extraH, bool heightDefinite, float definiteBorderBoxH, float contentW,
                                bool widthAuto) {
    const AspectMinMax bounds = aspectMinMaxPlusBox(style, refW, refH, extraW, extraH);
    if (heightDefinite) {
        const float h =
            clampHeightMinMax(std::max(0.0f, definiteBorderBoxH - extraH), style.base[prop::MinHeight].len,
                              style.base[prop::MaxHeight].len, refH);
        return resolveAspect(kUndefinedRef, h + extraH, ratio, bounds);
    }
    return resolveAspect(contentW + extraW, kUndefinedRef, ratio, bounds, widthAuto);
}

}  // namespace xt
