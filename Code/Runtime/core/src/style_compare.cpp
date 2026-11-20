#include "style_compare.h"

#include "xt/value.h"

/// T-753: see style_compare.h.

namespace xt {

namespace {

/// Equal values; a float also equals itself as NaN (kAutoSlice is a NaN sentinel).
template <class T>
bool sameStyleField(const T& a, const T& b) {
    return a == b;
}
bool sameStyleField(float a, float b) { return a == b || (a != a && b != b); }

template <class T, size_t N>
bool sameStyleArray(const T (&a)[N], const T (&b)[N]) {
    for (size_t i = 0; i < N; ++i) {
        if (!sameStyleField(a[i], b[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace

// A field added to ComputedStyle must be compared below; this size (everything but `base`, 64-bit
// targets) changes with it and points here.
static_assert(sizeof(void*) != 8 || sizeof(ComputedStyle) - sizeof(ComputedStyle::base) == 376,
              "ComputedStyle changed: update sameComputedStyle()");

bool sameComputedStyle(const ComputedStyle& a, const ComputedStyle& b) {
#define XT_SAME(f)                   \
    if (!sameStyleField(a.f, b.f)) { \
        return false;                \
    }                                \
    static_assert(true)
    XT_SAME(display);
    XT_SAME(position);
    XT_SAME(flexDir);
    XT_SAME(flexWrap);
    XT_SAME(justify);
    XT_SAME(alignItems);
    XT_SAME(alignSelf);
    XT_SAME(alignContent);
    XT_SAME(overflowX);
    XT_SAME(overflowY);
    XT_SAME(zIndex);
    XT_SAME(visibility);
    XT_SAME(pointerEvents);
    XT_SAME(backfaceHidden);
    XT_SAME(worldLayer);
    XT_SAME(pointerWatch);
    XT_SAME(focusNav);
    XT_SAME(layerOverlay);
    XT_SAME(fontFamilyOffset);
    XT_SAME(fontFamilyCount);
    XT_SAME(fontWeight);
    XT_SAME(fontStyle);
    XT_SAME(textAlign);
    XT_SAME(whiteSpace);
    XT_SAME(textOverflow);
    XT_SAME(textTransform);
    XT_SAME(lineClamp);
    XT_SAME(fontVariantNumeric);
    XT_SAME(gridColumns);
    XT_SAME(gridRows);
    XT_SAME(gridAreas);
    XT_SAME(justifyItems);
    XT_SAME(gridAreaName);
    XT_SAME(uiScaleMode);
    XT_SAME(mediaOverride);
    XT_SAME(appRegion);
    XT_SAME(uiScaleMin);
    XT_SAME(uiScaleMax);
    XT_SAME(uiScaleEff);
    XT_SAME(cursor);
    XT_SAME(objectFit);
    XT_SAME(direction);
    XT_SAME(writingMode);
    XT_SAME(textOrientation);
    XT_SAME(textRendering);
    XT_SAME(lang);
    XT_SAME(transitionOffset);
    XT_SAME(transitionCount);
    XT_SAME(animationOffset);
    XT_SAME(animationCount);
    XT_SAME(effectOffset);
    XT_SAME(effectCount);
    XT_SAME(gradientOffset);
    XT_SAME(gradientCount);
    XT_SAME(textEffectOffset);
    XT_SAME(textEffectCount);
    XT_SAME(backgroundImage);
    XT_SAME(borderImage);
    XT_SAME(borderImageRepeat);
    XT_SAME(borderImageFill);
    XT_SAME(outlineStyle);
    XT_SAME(borderColorSides);
    XT_SAME(textDecorationLine);
    XT_SAME(backgroundRepeat);
    XT_SAME(imageRendering);
    XT_SAME(textDecorations);
    XT_SAME(inheritedDecorations);
    XT_SAME(inheritedDecorationColor);
    XT_SAME(inheritedDecorationThickness);
    XT_SAME(decoAscent);
    XT_SAME(decoDescent);
    XT_SAME(decoUnderlineOffset);
    XT_SAME(decoUnderlineThickness);
    XT_SAME(mixBlendMode);
    XT_SAME(clipShape);
    XT_SAME(resolvedFont);
    XT_SAME(lastAnimationOffset);
    XT_SAME(lastAnimationCount);
    XT_SAME(clipStart);
    XT_SAME(clipRate);
#undef XT_SAME
    if (!sameStyleArray(a.gridLines, b.gridLines) || !sameStyleArray(a.hitSlop, b.hitSlop) ||
        !sameStyleArray(a.borderImageSlice, b.borderImageSlice) ||
        !sameStyleArray(a.borderStyle, b.borderStyle) ||
        !sameStyleArray(a.animationLonghandOffset, b.animationLonghandOffset) ||  // T-778.
        !sameStyleArray(a.animationLonghandCount, b.animationLonghandCount) ||
        !sameStyleArray(a.transitionLonghandOffset, b.transitionLonghandOffset) ||
        !sameStyleArray(a.transitionLonghandCount, b.transitionLonghandCount)) {
        return false;
    }
    for (size_t i = 0; i < sizeof(a.base) / sizeof(a.base[0]); ++i) {
        const Value& x = a.base[i];
        const Value& y = b.base[i];
        // valueEquals(), with a NaN float equal to itself.
        const bool nanFloats =
            x.type == ValueType::Float && y.type == ValueType::Float && x.f != x.f && y.f != y.f;
        if (!nanFloats && !valueEquals(x, y)) {
            return false;
        }
    }
    return true;
}

}  // namespace xt
