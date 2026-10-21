#include "xt/style_engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

#include "camera.h"
#include "mini_css.h"
#include "modifier_props.h"
#include "selector_match.h"
#include "style_decode.h"
#include "xt/assert.h"
#include "xt/asset_resolver.h"
#include "xt/document.h"
#include "xt/effect_utils.h"
#include "xt/property.h"
#include "xt/text_shaper.h"

/// StyleEngine::resolve: selector matching, cascade, inheritance, em resolution, base-value
/// writes, transition registration and CSS `animation` play/stop. See docs/design/
/// 05-style-and-css.md §5.4, §5.7-5.8, docs/design/04-timeline.md §4.6, docs/tasks/T-24.md.

namespace xt {

namespace {

Easing toEasing(const IrEasing& e) { return easingFromIr(e); }

/// One matched declaration, ready for cascade sorting (05.7 step 2). Exactly one of `decl` (an IR
/// declaration: a stylesheet rule's or the element's `style` attribute's) and `runtime` (a
/// declaration set through ElementRef::setStyle(), T-35) is non-null.
struct Entry {
    bool important;
    uint32_t specificity;
    uint32_t order;  // Source order: rule index, then inline decls, then runtime decls.
    const IrDecl* decl;
    const RuntimeStyleDecl* runtime;
    // T-218: the rule's kIrSelSceneTarget part when it addresses the element's <svg> content
    // (then the declaration never reaches the element's own ComputedStyle).
    const IrSelPart* scene = nullptr;
    // T-345 (05 §5.9.9): the declaration comes from a rule inside an `@media` block (what
    // `media-override` keeps from switching `display`).
    bool fromMedia = false;
};

/// T-345 (05 §5.9.9): the factor an element's px lengths resolve with, from its `ui-scale`.
float effectiveUiScale(uint8_t mode, float lo, float hi, float uiScale, float dpr) {
    switch (mode) {
        case kUiScaleNone:
            return dpr > 0.0f ? dpr : 1.0f;
        case kUiScaleClamp:
            return std::min(std::max(uiScale, lo), hi);
        default:
            return uiScale;
    }
}

/// T-345: decodes a `ui-scale` declaration's (mode, min, max) into `style`; false (`style`
/// untouched) for a malformed payload.
bool applyUiScaleDecl(ComputedStyle& style, float mode, float lo, float hi) {
    if (mode == static_cast<float>(kUiScaleAuto) || mode == static_cast<float>(kUiScaleNone)) {
        style.uiScaleMode = static_cast<uint8_t>(mode);
        style.uiScaleMin = 0;
        style.uiScaleMax = 0;
        return true;
    }
    if (mode == static_cast<float>(kUiScaleClamp) && lo > 0.0f && hi >= lo && std::isfinite(hi)) {
        style.uiScaleMode = kUiScaleClamp;
        style.uiScaleMin = lo;
        style.uiScaleMax = hi;
        return true;
    }
    return false;
}

/// T-345 (05 §5.9.9): multiplies the px lengths the cascade left in `next` (design px: `em` /
/// `rem` were already turned into px against the unscaled font size) by the element's factor `s`.
/// font-size and translate are scaled where they are resolved; `calc()` programs carry the factor
/// in their own terms; `%`, viewport, safe-area and `uiscale` units are not scaled.
void scaleStylePx(ComputedStyle& next, Length& originX, Length& originY, float s) {
    auto scaleLength = [s](Value& v) {
        if (v.type == ValueType::Length && v.len.unit == LengthUnit::Px) {
            v.len.value *= s;
        }
    };
    for (PropId p = prop::Width; p <= prop::LetterSpacing; ++p) {
        if (p != prop::FontSize) {
            scaleLength(next.base[p]);
        }
    }
    for (PropId p = prop::BackgroundColor; p <= prop::Order; ++p) {
        if (p != prop::ScrollX && p != prop::ScrollY) {
            scaleLength(next.base[p]);
        }
    }
    if (originX.unit == LengthUnit::Px) {
        originX.value *= s;
    }
    if (originY.unit == LengthUnit::Px) {
        originY.value *= s;
    }
    if (next.inheritedDecorationThickness >= 0.0f) {
        next.inheritedDecorationThickness *= s;
    }
}

bool cascadeLess(const Entry& a, const Entry& b) {
    if (a.important != b.important) {
        return !a.important && b.important;  // non-important first
    }
    if (a.specificity != b.specificity) {
        return a.specificity < b.specificity;
    }
    return a.order < b.order;
}

/// Element PropId (64-105) Length decls: Em is resolved to px against the element's own font-size;
/// Px/Percent/Auto pass through unchanged (05.7 step 4).
Length decodeElementLength(const IrDecl& decl, float fontSizeSelfPx) {
    Length len{irValueF32(decl.value, 0), static_cast<LengthUnit>(decl.value[4])};
    if (len.unit == LengthUnit::Em) {
        return Length{len.value * fontSizeSelfPx, LengthUnit::Px};
    }
    return len;
}

/// T-335 (05 §5.9.3): a `rem` Length declaration resolved to px against the root element's font
/// size, at style time like `em`, so nothing downstream ever reads LengthUnit::Rem. Any other
/// declaration is returned as is; `scratch` holds the rewritten copy.
const IrDecl* resolveRemDecl(const IrDecl* decl, float rootFontPx, IrDecl& scratch) {
    if (decl == nullptr || static_cast<ValueType>(decl->valueType) != ValueType::Length ||
        static_cast<LengthUnit>(decl->value[4]) != LengthUnit::Rem) {
        return decl;
    }
    scratch = *decl;
    irSetValueF32(scratch.value, 0, irValueF32(decl->value, 0) * rootFontPx);
    scratch.value[4] = static_cast<uint8_t>(LengthUnit::Px);
    return &scratch;
}

/// resolveRemDecl() for a runtime declaration (a token's value can be in `rem`).
const RuntimeStyleDecl* resolveRemDecl(const RuntimeStyleDecl* decl, float rootFontPx,
                                       RuntimeStyleDecl& scratch) {
    if (decl == nullptr || decl->value.type != ValueType::Length || decl->value.len.unit != LengthUnit::Rem) {
        return decl;
    }
    scratch = *decl;
    scratch.value = Value::ofLength(decl->value.len.value * rootFontPx, LengthUnit::Px);
    return &scratch;
}

Color decodeColor(const IrDecl& decl) {
    return Color{irValueF32(decl.value, 0), irValueF32(decl.value, 1), irValueF32(decl.value, 2),
                 irValueF32(decl.value, 3)};
}

/// `background-image`'s decl carries a StringRef (asset name, or "none"); resolves the name to the
/// matching ASST Image entry's index (the AssetId convention used elsewhere, e.g.
/// document_load.cpp's `data-xt-asset` handling). No match (or "none") -> kInvalidAsset.
AssetId resolveBackgroundImageByName(const IrView& ir, std::string_view name) {
    if (name == "none") {
        return kInvalidAsset;
    }
    std::span<const IrAsset> assets = ir.assets().assets();
    for (uint32_t i = 0; i < assets.size(); ++i) {
        // Kind 0 = Image, 3 = AtlasImage (T-199: `atlas:<n>#<asset>`, docs/design/02 §2.5).
        if ((assets[i].kind == 0 || assets[i].kind == 3) && ir.string(assets[i].name) == name) {
            return i;
        }
    }
    return kInvalidAsset;
}

/// A compiled declaration's StringRef is an IR string id by construction (IrView validated it).
AssetId resolveBackgroundImageAsset(const IrView& ir, const IrDecl& decl) {
    return resolveBackgroundImageByName(ir, ir.string(irValueU32(decl.value, 0)));
}

/// T-270d: a runtime declaration's text value (setStyle / a style binding) through the
/// Document's string() (StyleEngine::stringOf, via `strings`): an IR id, a runtime-pool id or the
/// element's own runtime text slot.
std::string_view runtimeText(const SelectorMatchContext& strings, StringId id) {
    return strings.string != nullptr ? strings.string(strings.stringsCtx, id) : std::string_view{};
}

/// T-386 (05 §5.9.11): one encoded `hit-slop` side (xt::kHitSlopEmBias) -> ComputedStyle::hitSlop:
/// px and em in this element's ui-scale, a percentage kept as -fraction for the hit test.
float resolveHitSlopSide(float encoded, float fontSizeSelfPx, float pxScale) {
    if (!std::isfinite(encoded)) {
        return 0.0f;
    }
    if (encoded >= 0.0f) {
        return encoded * pxScale;
    }
    if (encoded <= -kHitSlopEmBias) {
        return (-encoded - kHitSlopEmBias) * fontSizeSelfPx * pxScale;
    }
    return encoded;
}

/// Applies one non-font-size declaration onto `next` (or, for transform-origin, onto
/// `originX`/`originY`, staged the same way `next.base` is and committed by the caller). Must run
/// after the font-size pass so `fontSizeSelfPx` is final.
void applyGeneralDecl(const IrView& ir, ComputedStyle& next, Length& originX, Length& originY,
                      const IrDecl& decl, float fontSizeSelfPx, float pxScale) {
    switch (decl.prop) {
        case prop::Opacity:
        case prop::ScaleX:
        case prop::ScaleY:
        case prop::Rotation:
        case prop::SkewX:
        case prop::SkewY:
            next.base[decl.prop] = Value::ofFloat(irValueF32(decl.value, 0));
            return;
        // §19.3 PropId 16-22 (TranslateZ..Orientation): no CSS 3D function syntax yet (T-47); a
        // decl carrying an already-encoded Float/Vec3 slot (as IrWriter's compiled inline decls
        // can) is copied straight through, same as the plain-float props above. See
        // docs/tasks/T-43c.md.
        case prop::TranslateZ:
        case prop::AnchorZ:
        case prop::RotationX:
        case prop::RotationY:
            next.base[decl.prop] = Value::ofFloat(irValueF32(decl.value, 0));
            return;
        case prop::AutoOrient:  // T-228 (31 §31.7.2): `auto-orient: none | path`.
            next.base[prop::AutoOrient] = Value::ofEnum(irValueU32(decl.value, 0) == 1 ? 1u : 0u);
            return;
        case prop::Translate3:
        case prop::Anchor3:
        case prop::Orientation:
            next.base[decl.prop] = Value::ofVec3(
                Vec3{irValueF32(decl.value, 0), irValueF32(decl.value, 1), irValueF32(decl.value, 2)});
            return;
        case prop::TranslateX:
        case prop::TranslateY: {
            Length len{irValueF32(decl.value, 0), static_cast<LengthUnit>(decl.value[4])};
            float px;
            if (len.unit == LengthUnit::Percent) {
                // T-211: kept as a Length; setProperty routes it to cssTranslatePct and the §6.7
                // fold resolves it against the element's own border box after layout.
                next.base[decl.prop] = Value::ofLength(len.value, LengthUnit::Percent);
            } else if (style_decode::lengthToPx(len, fontSizeSelfPx, px)) {
                // T-345: px / em are design px, scaled by the element's `ui-scale` factor.
                const bool scaled = len.unit == LengthUnit::Px || len.unit == LengthUnit::Em;
                next.base[decl.prop] = Value::ofFloat(scaled ? px * pxScale : px);
            }
            return;
        }
        case prop::AnchorX:
        case prop::AnchorY: {
            // transform-origin keeps `%` for layout (05.7 step 4) but, like every other length,
            // resolves `em` here: layout cannot (T-257b).
            Length len{irValueF32(decl.value, 0), static_cast<LengthUnit>(decl.value[4])};
            if (len.unit == LengthUnit::Em) {
                len = Length{len.value * fontSizeSelfPx, LengthUnit::Px};
            }
            (decl.prop == prop::AnchorX ? originX : originY) = len;
            return;
        }
        case prop::LineHeight:
            // Unitless/Em line-height stays Em (multiplied by font-size at layout time, 05.7 step 4).
            next.base[prop::LineHeight] =
                Value::ofLength(irValueF32(decl.value, 0), static_cast<LengthUnit>(decl.value[4]));
            return;
        default:
            break;
    }
    if (decl.prop >= prop::Width && decl.prop <= prop::LetterSpacing) {
        if (propertyType(decl.prop) == ValueType::Length) {
            Length len = decodeElementLength(decl, fontSizeSelfPx);
            next.base[decl.prop] = Value::ofLength(len.value, len.unit);
        } else {
            next.base[decl.prop] = Value::ofFloat(irValueF32(decl.value, 0));
        }
        return;
    }
    if (decl.prop >= prop::BackgroundColor && decl.prop <= prop::Order) {
        ValueType vt = propertyType(decl.prop);
        if (vt == ValueType::Color) {
            next.base[decl.prop] = Value::ofColor(decodeColor(decl));
        } else if (vt == ValueType::Length) {
            Length len = decodeElementLength(decl, fontSizeSelfPx);
            next.base[decl.prop] = Value::ofLength(len.value, len.unit);
        } else {
            next.base[decl.prop] = Value::ofFloat(irValueF32(decl.value, 0));
        }
        return;
    }
    if (decl.prop == style_decode::styleprop::ZIndex) {
        next.zIndex = irValueF32(decl.value, 0);
        return;
    }
    if (decl.prop == style_decode::styleprop::ClipStart) {  // T-191 (28.2).
        next.clipStart = irValueF32(decl.value, 0);
        return;
    }
    if (decl.prop == style_decode::styleprop::ClipRate) {
        const float rate = irValueF32(decl.value, 0);
        next.clipRate = rate > 0.0f ? rate : 1.0f;
        return;
    }
    if (decl.prop == style_decode::styleprop::FontFamily) {
        next.fontFamilyOffset = irValueU32(decl.value, 0);
        next.fontFamilyCount = irValueU32(decl.value, 1);
        return;
    }
    if (decl.prop == style_decode::styleprop::Transition) {
        next.transitionOffset = irValueU32(decl.value, 0);
        next.transitionCount = irValueU32(decl.value, 1);
        return;
    }
    if (decl.prop == style_decode::styleprop::Animation) {
        next.animationOffset = irValueU32(decl.value, 0);
        next.animationCount = irValueU32(decl.value, 1);
        return;
    }
    if (decl.prop == style_decode::styleprop::Effect) {
        // 18-effects.md §18.4/T-51b: same ListRef-into-a-pool encoding as Transition/Animation
        // above; the actual effect stack is assigned by StyleEngine::resolve right before commit.
        next.effectOffset = irValueU32(decl.value, 0);
        next.effectCount = irValueU32(decl.value, 1);
        return;
    }
    if (decl.prop == style_decode::styleprop::BackgroundGradient) {
        // 05.5c (T-115): same ListRef-into-a-pool encoding as `effect` above; the actual gradient
        // is decoded into ElementData by StyleEngine::resolve right before commit.
        next.gradientOffset = irValueU32(decl.value, 0);
        next.gradientCount = irValueU32(decl.value, 1);
        return;
    }
    if (decl.prop == style_decode::styleprop::TextEffect) {
        // T-178 (07 §7.8): ListRef into STYL `effects[]`, read by the Painter's text branch.
        next.textEffectOffset = irValueU32(decl.value, 0);
        next.textEffectCount = irValueU32(decl.value, 1);
        return;
    }
    if (decl.prop == style_decode::styleprop::BackgroundImage) {
        next.backgroundImage = resolveBackgroundImageAsset(ir, decl);
        return;
    }
    if (decl.prop == style_decode::styleprop::BorderImageSource) {  // T-199 (26.3.4).
        next.borderImage = resolveBackgroundImageAsset(ir, decl);
        return;
    }
    if (decl.prop == style_decode::styleprop::BorderImageSlice) {
        for (int i = 0; i < 4; ++i) {
            next.borderImageSlice[i] = irValueF32(decl.value, i);
        }
        return;
    }
    if (decl.prop == style_decode::styleprop::HitSlop) {  // T-386 (05 §5.9.11).
        for (int i = 0; i < 4; ++i) {
            next.hitSlop[i] = resolveHitSlopSide(irValueF32(decl.value, i), fontSizeSelfPx, pxScale);
        }
        return;
    }
    if (decl.prop == style_decode::styleprop::Lang) {
        // `lang` is resolved from the HTML attribute, not CSS (05.4); a stray decl is ignored.
        return;
    }
    if (decl.prop == style_decode::styleprop::GridArea) {
        // T-338 (06 §6.15): the area name's StringId (the empty string is `auto`).
        next.gridAreaName = static_cast<ValueType>(decl.valueType) == ValueType::StringRef
                                ? irValueU32(decl.value, 0)
                                : kInvalidString;
        return;
    }
    style_decode::applyEnumDecl(next, decl.prop, irValueU32(decl.value, 0));
}

/// applyGeneralDecl's counterpart for a runtime (setStyle) declaration, whose value is already a
/// decoded Value instead of an IrDecl's byte payload. Same 05.7 step 4 rules: Em resolves against
/// the element's own font-size, Percent is left for layout, an unresolvable Percent translate is
/// ignored. A value whose type does not match the property's is ignored (mini_css only ever
/// produces matching types; this is defensive).
void applyRuntimeDecl(const IrView& ir, const SelectorMatchContext& strings, ComputedStyle& next,
                      Length& originX, Length& originY, const RuntimeStyleDecl& decl, float fontSizeSelfPx,
                      float pxScale) {
    const Value& value = decl.value;
    switch (decl.prop) {
        case prop::Opacity:
        case prop::ScaleX:
        case prop::ScaleY:
        case prop::Rotation:
        case prop::SkewX:
        case prop::SkewY:
            if (value.type == ValueType::Float) {
                next.base[decl.prop] = value;
            }
            return;
        // §19.3 PropId 16-22 (TranslateZ..Orientation): no CSS 3D function syntax yet (T-47), but
        // a runtime decl (ElementRef::setStyle's test-only bypass, or a future `xt.setStyle3D`-
        // style API) can already carry an already-decoded Float/Vec3 value straight through, same
        // as the plain-float props above. See docs/tasks/T-43c.md.
        case prop::TranslateZ:
        case prop::AnchorZ:
        case prop::RotationX:
        case prop::RotationY:
            if (value.type == ValueType::Float) {
                next.base[decl.prop] = value;
            }
            return;
        case prop::AutoOrient:
            if (value.type == ValueType::Enum) {
                next.base[prop::AutoOrient] = Value::ofEnum(value.e == 1 ? 1u : 0u);
            }
            return;
        case prop::Translate3:
        case prop::Anchor3:
        case prop::Orientation:
            if (value.type == ValueType::Vec3) {
                next.base[decl.prop] = value;
            }
            return;
        case prop::TranslateX:
        case prop::TranslateY: {
            if (value.type != ValueType::Length) {
                return;
            }
            float px = 0;
            if (value.len.unit == LengthUnit::Percent) {
                next.base[decl.prop] = value;  // T-211: resolved by the §6.7 fold.
            } else if (style_decode::lengthToPx(value.len, fontSizeSelfPx, px)) {
                // T-345: px / em are design px, scaled by the element's `ui-scale` factor.
                const bool scaled = value.len.unit == LengthUnit::Px || value.len.unit == LengthUnit::Em;
                next.base[decl.prop] = Value::ofFloat(scaled ? px * pxScale : px);
            }
            return;
        }
        case prop::AnchorX:
        case prop::AnchorY:
            if (value.type == ValueType::Length) {
                // As applyGeneralDecl: `%` stays for layout, `em` resolves here (T-257b).
                const Length len = value.len.unit == LengthUnit::Em
                                       ? Length{value.len.value * fontSizeSelfPx, LengthUnit::Px}
                                       : value.len;
                (decl.prop == prop::AnchorX ? originX : originY) = len;
            }
            return;
        case prop::LineHeight:
            if (value.type == ValueType::Length) {
                next.base[prop::LineHeight] = value;  // Em stays Em (resolved at layout time).
            }
            return;
        default:
            break;
    }
    if ((decl.prop >= prop::Width && decl.prop <= prop::LetterSpacing) ||
        (decl.prop >= prop::BackgroundColor && decl.prop <= prop::Order)) {
        ValueType vt = propertyType(decl.prop);
        if (value.type != vt) {
            return;
        }
        if (vt == ValueType::Length && value.len.unit == LengthUnit::Em) {
            next.base[decl.prop] = Value::ofLength(value.len.value * fontSizeSelfPx, LengthUnit::Px);
        } else {
            next.base[decl.prop] = value;
        }
        return;
    }
    if (decl.prop == style_decode::styleprop::ZIndex) {
        if (value.type == ValueType::Float) {
            next.zIndex = value.f;
        }
        return;
    }
    if (decl.prop == style_decode::styleprop::ClipStart) {  // T-191 (28.2).
        if (value.type == ValueType::Float) {
            next.clipStart = value.f;
        }
        return;
    }
    if (decl.prop == style_decode::styleprop::ClipRate) {
        if (value.type == ValueType::Float && value.f > 0.0f) {
            next.clipRate = value.f;
        }
        return;
    }
    if (decl.prop == style_decode::styleprop::HitSlop) {  // T-386: mini_css encodes it as the IR does.
        if (value.type == ValueType::Color) {
            const float sides[4] = {value.c.r, value.c.g, value.c.b, value.c.a};
            for (int i = 0; i < 4; ++i) {
                next.hitSlop[i] = resolveHitSlopSide(sides[i], fontSizeSelfPx, pxScale);
            }
        }
        return;
    }
    if (decl.prop == style_decode::styleprop::FontFamily) {
        // ComputedStyle points at a run of the IR's font-family pool, so a runtime family name can
        // only be honored when that exact name already appears in some compiled font-family list
        // (see the T-35 report's uncovered cases). T-270d: a family only registered at runtime
        // needs a runtime family representation in ComputedStyle (reported, not designed here).
        std::span<const uint32_t> familyPool = ir.style().fontFamilies();
        for (uint32_t i = 0; i < familyPool.size(); ++i) {
            if (familyPool[i] == value.sid) {
                next.fontFamilyOffset = i;
                next.fontFamilyCount = 1;
                return;
            }
        }
        return;
    }
    if (decl.prop == style_decode::styleprop::BackgroundImage) {
        next.backgroundImage = resolveBackgroundImageByName(ir, runtimeText(strings, value.sid));
        return;
    }
    if (decl.prop == style_decode::styleprop::BorderImageSource) {  // T-199 (26.3.4).
        next.borderImage = resolveBackgroundImageByName(ir, runtimeText(strings, value.sid));
        return;
    }
    if (decl.prop == style_decode::styleprop::Effect) {
        // 18-effects.md §18.4/T-51b: ElementRef::setStyle/setEffects encode the declaration as a
        // ListRef whose `sid` is Document::declareRuntimeEffects()'s tagged offset
        // (kRuntimeEffectOffsetBit | index) and whose `floatCount` is the sequence's length (see
        // xt/document.h's declareRuntimeEffects doc comment; Value has no second u32 slot for a
        // ListRef the way an IrDecl's raw bytes do, so the count rides along in the otherwise-
        // unused floatCount field).
        if (value.type == ValueType::ListRef) {
            next.effectOffset = value.sid;
            next.effectCount = value.floatCount;
        }
        return;
    }
    if (decl.prop == style_decode::styleprop::Animation) {
        // 17.6 (T-123): ElementRef::setStyle encodes the declaration as a ListRef whose `sid` is
        // Document::declareRuntimeAnimations()' tagged offset (kRuntimeEffectOffsetBit | index)
        // and whose `floatCount` is the list's length -- same convention as `effect` above.
        if (value.type == ValueType::ListRef) {
            next.animationOffset = value.sid;
            next.animationCount = value.floatCount;
        }
        return;
    }
    if (decl.prop == style_decode::styleprop::BackgroundGradient) {
        // 05.5c (T-115): ElementRef::setStyle encodes the declaration as a ListRef whose `sid` is
        // Document::declareRuntimeGradient()'s tagged offset (kRuntimeEffectOffsetBit | index) and
        // whose `floatCount` is 1 (0 for `none`) -- same convention as `effect` above.
        if (value.type == ValueType::ListRef) {
            next.gradientOffset = value.sid;
            next.gradientCount = value.floatCount;
        }
        return;
    }
    if (decl.prop == style_decode::styleprop::Lang) {
        return;  // `lang` comes from the HTML attribute, not CSS (05.4).
    }
    if (value.type == ValueType::Enum) {
        style_decode::applyEnumDecl(next, decl.prop, value.e);
    }
}

// docs/design/19-3d-space.md §19.3's PropId 16-19 (TranslateZ/AnchorZ/RotationX/RotationY) are
// plain per-node floats exactly like the eight pre-19-3d-space entries below (property_access.cpp
// already commits them to NodeCommon), but were never added here -- so a CSS `transform:
// rotateY()`/`translateZ()` etc. on an Element correctly flipped `is3D` (19.3's derivation reads
// ComputedStyle::base directly) yet never actually reached NodeCommon, leaving
// world_transform.cpp's world3 composition untouched. Found while implementing T-48 (whose 3D
// golden fixtures/hit-test unit tests depend on these actually taking effect); fixed here since
// every other T-48 file depends on it. Translate3/Anchor3/Orientation (Vec3-typed, PropId 20-22)
// are left out of this fix -- no fixture in this task exercises them, and Vec3 diffing/
// registration in this same loop is untested territory best left to whichever task needs it.
constexpr PropId kGeneralAnimatableProps[] = {
    prop::Opacity, prop::TranslateX, prop::TranslateY, prop::ScaleX,  prop::ScaleY,    prop::Rotation,
    prop::SkewX,   prop::SkewY,      prop::TranslateZ, prop::AnchorZ, prop::RotationX, prop::RotationY};

/// True if `p` covers `prop` (explicit match, the `all` sentinel 0xFFFF, or -- 18-effects.md
/// §18.4/T-51b -- `transition: effect ...`'s spec.prop == 187 covering every effect-parameter
/// PropId, since `effect` is a dynamic list of PropIds rather than one of its own).
bool transitionCovers(const IrTransitionSpec& spec, PropId prop) {
    if (spec.prop == prop || spec.prop == 0xFFFFu) {
        return true;
    }
    return spec.prop == style_decode::styleprop::Effect && isEffectProp(prop);
}

/// `toFinal` is passed explicitly (rather than read from `next.base[prop]`, as an earlier revision
/// did) because it's out of range for an effect-parameter PropId (18-effects.md §18.4/T-51b):
/// `next.base` is sized for the general/Element PropIds (0-108) only, and effect parameter values
/// live in NodeArena::effects(), not ComputedStyle.
void maybeRegisterTransition(Animator& animator, const IrView& ir, const ComputedStyle& next, NodeId node,
                             PropId prop, const Value& fromFinal, const Value& toFinal, float clock,
                             bool firstResolve, NodeId target = kInvalidNode) {
    if (firstResolve || next.transitionCount == 0) {
        return;
    }
    ValueType vt = propertyType(prop);
    if (vt == ValueType::Enum || vt == ValueType::StringRef || vt == ValueType::ListRef) {
        return;  // Discrete types never transition (03.10).
    }
    std::span<const IrTransitionSpec> all = ir.style().transitions();
    if (uint64_t{next.transitionOffset} + next.transitionCount > all.size()) {
        return;  // Defensive; IrView::open already validated this range.
    }
    std::span<const IrTransitionSpec> specs = all.subspan(next.transitionOffset, next.transitionCount);
    for (const IrTransitionSpec& spec : specs) {
        if (transitionCovers(spec, prop)) {
            Transition tr;
            tr.node = node;
            tr.prop = prop;
            tr.from = fromFinal;
            tr.to = toFinal;
            tr.startClock = clock;
            tr.delay = spec.delay;
            tr.duration = spec.duration;
            tr.easing = toEasing(spec.easing);
            tr.target = target != kInvalidNode ? target : node;  // T-218: a scene node, or the element.
            animator.addTransition(tr);
            return;  // First matching entry wins (conservative choice; see T-24 report).
        }
    }
}

/// T-211 (05 tail ruling): a translate transition that involves a percent -- the current value has
/// a % part or the new base is a % -- runs in the mixed (px, %) Vec2 form, each part interpolated
/// separately and summed by the §6.7 fold. `fromFinal` must hold the current px (getProperty);
/// px-only transitions keep their Float ends unchanged.
void translateTransitionEnds(const ElementData& data, PropId p, Value& fromFinal, Value& toFinal) {
    const bool isX = p == prop::TranslateX;
    const float fromPct = isX ? data.cssTranslatePct.x : data.cssTranslatePct.y;
    const bool toPercent = toFinal.type == ValueType::Length && toFinal.len.unit == LengthUnit::Percent;
    if (fromPct == 0.0f && !toPercent) {
        return;
    }
    fromFinal = Value::ofVec2(Vec2{fromFinal.f, fromPct});
    toFinal = toPercent ? Value::ofVec2(Vec2{0, toFinal.len.value}) : Value::ofVec2(Vec2{toFinal.f, 0});
}

/// 05.7 step 6: dirty bits for the non-animatable fields that changed between `oldStyle` and
/// `next`. display/position/... affect layout; visibility/pointer-events/cursor/z-index affect
/// only paint (T-24.md).
void markNonAnimatableDirty(NodeArena& arena, NodeId node, const ComputedStyle& oldStyle,
                            const ComputedStyle& next) {
    bool layoutChanged = oldStyle.display != next.display || oldStyle.position != next.position ||
                         oldStyle.flexDir != next.flexDir || oldStyle.flexWrap != next.flexWrap ||
                         oldStyle.justify != next.justify || oldStyle.alignItems != next.alignItems ||
                         oldStyle.alignSelf != next.alignSelf || oldStyle.alignContent != next.alignContent ||
                         oldStyle.overflowX != next.overflowX || oldStyle.overflowY != next.overflowY ||
                         oldStyle.fontFamilyOffset != next.fontFamilyOffset ||
                         oldStyle.fontFamilyCount != next.fontFamilyCount ||
                         oldStyle.fontWeight != next.fontWeight || oldStyle.fontStyle != next.fontStyle ||
                         oldStyle.textAlign != next.textAlign || oldStyle.whiteSpace != next.whiteSpace ||
                         oldStyle.textOverflow != next.textOverflow || oldStyle.objectFit != next.objectFit ||
                         oldStyle.direction != next.direction || oldStyle.writingMode != next.writingMode ||
                         oldStyle.textOrientation != next.textOrientation || oldStyle.lang != next.lang;
    // T-338 (06 §6.15): the grid template, item placement and alignment.
    layoutChanged = layoutChanged || oldStyle.gridColumns != next.gridColumns ||
                    oldStyle.gridRows != next.gridRows || oldStyle.gridAreas != next.gridAreas ||
                    oldStyle.justifyItems != next.justifyItems ||
                    std::memcmp(oldStyle.gridLines, next.gridLines, sizeof(next.gridLines)) != 0 ||
                    oldStyle.gridAreaName != next.gridAreaName;
    // T-323 (05 §5.9.8): the text is converted before shaping, so a new transform reshapes it.
    layoutChanged = layoutChanged || oldStyle.textTransform != next.textTransform;
    // T-331 (05 §5.9.8): the line count and the OpenType features (part of the shaping key).
    layoutChanged = layoutChanged || oldStyle.lineClamp != next.lineClamp ||
                    oldStyle.fontVariantNumeric != next.fontVariantNumeric;
    bool paintChanged =
        oldStyle.visibility != next.visibility || oldStyle.pointerEvents != next.pointerEvents ||
        oldStyle.pointerWatch != next.pointerWatch || oldStyle.cursor != next.cursor ||
        oldStyle.layerOverlay != next.layerOverlay || oldStyle.zIndex != next.zIndex ||
        oldStyle.backgroundImage != next.backgroundImage || oldStyle.backfaceHidden != next.backfaceHidden ||
        oldStyle.borderImage != next.borderImage || oldStyle.borderImageRepeat != next.borderImageRepeat ||
        oldStyle.borderImageFill != next.borderImageFill ||
        std::memcmp(oldStyle.borderImageSlice, next.borderImageSlice, sizeof(next.borderImageSlice)) != 0 ||
        oldStyle.gradientOffset != next.gradientOffset || oldStyle.gradientCount != next.gradientCount ||
        oldStyle.clipShape != next.clipShape ||  // T-332: repaint and re-hit-test, no relayout.
        oldStyle.textEffectOffset != next.textEffectOffset ||
        oldStyle.textEffectCount != next.textEffectCount || oldStyle.outlineStyle != next.outlineStyle ||
        // T-333: which sides draw their own color, and each side's line style.
        oldStyle.borderColorSides != next.borderColorSides ||
        std::memcmp(oldStyle.borderStyle, next.borderStyle, sizeof(next.borderStyle)) != 0 ||
        // T-324: the decoration set and what the painter reads for it.
        oldStyle.textDecorationLine != next.textDecorationLine ||
        oldStyle.textDecorations != next.textDecorations ||
        oldStyle.inheritedDecorations != next.inheritedDecorations ||
        std::memcmp(&oldStyle.inheritedDecorationColor, &next.inheritedDecorationColor, sizeof(Color)) != 0 ||
        oldStyle.inheritedDecorationThickness != next.inheritedDecorationThickness ||
        oldStyle.decoAscent != next.decoAscent || oldStyle.decoDescent != next.decoDescent ||
        oldStyle.decoUnderlineOffset != next.decoUnderlineOffset ||
        oldStyle.decoUnderlineThickness != next.decoUnderlineThickness ||
        oldStyle.mixBlendMode != next.mixBlendMode ||  // T-326: a Layer appears / disappears.
        // T-327 (05 §5.9.8): background placement and image sampling.
        oldStyle.backgroundRepeat != next.backgroundRepeat || oldStyle.imageRendering != next.imageRendering;
    if (layoutChanged) {
        arena.markDirty(node, static_cast<uint8_t>(kDirtyLayout));
    }
    if (paintChanged) {
        arena.markDirty(node, static_cast<uint8_t>(kDirtyPaint));
    }
}

/// 05.7 step 6 (CSS `animation` diff): list-level compare -- any change to which decl range won
/// stops every previously-created instance and plays the new list from scratch.
void applyAnimationDiff(Animator& animator, const IrView& ir,
                        const std::vector<IrAnimationSpec>* runtimeAnimations, ElementData& data,
                        ComputedStyle& next, NodeId node, float clock,
                        const std::function<void(uint32_t, NodeId)>& onAnimationPlay) {
    bool unchanged = next.animationOffset == data.style.lastAnimationOffset &&
                     next.animationCount == data.style.lastAnimationCount;
    if (unchanged) {
        next.lastAnimationOffset = data.style.lastAnimationOffset;
        next.lastAnimationCount = data.style.lastAnimationCount;
        return;
    }
    for (uint32_t id : data.cssAnimations) {
        animator.stop(id);
    }
    data.cssAnimations.clear();

    // T-123: a runtime `animation` declaration (ElementRef::setStyle) tags its offset with
    // kRuntimeEffectOffsetBit and indexes Document's own pool instead of the IR's STYL one.
    std::span<const IrAnimationSpec> all = ir.style().animations();
    uint32_t offset = next.animationOffset;
    if ((offset & kRuntimeEffectOffsetBit) != 0) {
        offset &= ~kRuntimeEffectOffsetBit;
        all = runtimeAnimations != nullptr ? std::span<const IrAnimationSpec>(*runtimeAnimations)
                                           : std::span<const IrAnimationSpec>{};
    }
    if (static_cast<uint64_t>(offset) + next.animationCount <= all.size()) {
        std::span<const IrAnimationSpec> specs = all.subspan(offset, next.animationCount);
        for (const IrAnimationSpec& spec : specs) {
            // direction: 0 normal, 1 reverse, 2 alternate, 3 alternate-reverse (05.6/T-22).
            bool isAlternate = spec.direction == 2 || spec.direction == 3;
            bool isReverse = spec.direction == 1 || spec.direction == 3;

            PlayParams params;
            params.timeline = spec.timeline;
            params.localToArena = nullptr;
            params.localCount = 0;
            params.selfNode = node;
            params.delay = spec.delay;
            params.durationScale = spec.duration;
            params.segStart = 0;
            params.segEnd = -1;
            params.fill = static_cast<FillMode>(spec.fill);
            params.layer = 0;
            params.overrideEasing = toEasing(spec.easing);

            if (spec.iterations == 0) {
                params.loop = isAlternate ? LoopMode::PingPong : LoopMode::Loop;
                params.iterations = 0;
                params.speed = 1;
            } else {
                params.loop = LoopMode::Once;
                params.iterations = spec.iterations;
                params.speed = isReverse ? -1.0f : 1.0f;
            }

            uint32_t id = animator.play(params, clock);
            if (onAnimationPlay) {
                onAnimationPlay(id, node);
            }
            if (isReverse && spec.iterations != 0) {
                // "reverse 系列 -> speed = -1 並 seek 到尾" (T-24.md). localT is in the
                // timeline's own (unscaled) time domain, i.e. its segEnd == duration (the
                // compiler always builds @keyframes timelines with duration == 1, see
                // compile/src/style_emit.cpp emitKeyframesTimelines).
                float segEndSeconds = ir.timeline(spec.timeline).duration();
                animator.seek(id, segEndSeconds, clock);
            }
            if (spec.playState == 1 /* paused */) {
                animator.pause(id, clock);
            }
            data.cssAnimations.push_back(id);
        }
    }
    next.lastAnimationOffset = next.animationOffset;
    next.lastAnimationCount = next.animationCount;
}

/// Effect-parameter PropId's paramIndex list that's animatable (18-effects.md §18.2/§18.3):
/// params[0..7] (paramIndex 1-8), colors[0/1] (16/17) and the composite Vec2 offset (18).
/// paramIndex 0 (`enabled`) is deliberately excluded even though propertyType() reports it as
/// Float: 18.2 calls it out as "0 = enabled（Float 0/1，離散插值）" -- discrete, not a linear
/// tween -- unlike every other Float paramIndex here (see the T-51b report's uncovered case).
constexpr uint32_t kAnimatableEffectParams[] = {
    1, 2, 3, 4, 5, 6, 7, 8, prop::kEffectParamColor0, prop::kEffectParamColor1, prop::kEffectParamOffset};
constexpr size_t kAnimatableEffectParamCount =
    sizeof(kAnimatableEffectParams) / sizeof(kAnimatableEffectParams[0]);

/// Copies `count` gradient stops starting at `srcOffset` in `srcStops` into `scratchStops` at
/// `stopCursor`, advancing it. Returns false (leaving `stopCursor` untouched) if it would not fit
/// in `scratchStopsCap` or `srcOffset + count` is out of range of `srcStops` -- both defensive;
/// a validated IR or Document::declareRuntimeEffects()'s own bookkeeping should never hit this.
template <class SrcStop>
bool copyEffectStops(std::span<const SrcStop> srcStops, uint32_t srcOffset, uint32_t count,
                     GradientStop* scratchStops, uint32_t scratchStopsCap, uint32_t& stopCursor) {
    if (static_cast<uint64_t>(srcOffset) + count > srcStops.size() || stopCursor + count > scratchStopsCap) {
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const SrcStop& s = srcStops[srcOffset + i];
        scratchStops[stopCursor + i] = GradientStop{s.offset, s.color};
    }
    stopCursor += count;
    return true;
}

/// Resolves a ComputedStyle::effectOffset/effectCount pair (18-effects.md §18.4/T-51b) into the
/// actual effect sequence, ready for NodeArena::assignEffects() -- whose `stops` span must be
/// indexed relative to the returned `outEffects` (not to the source pool; see node_arena.h's
/// assignEffects() doc comment), hence the rebasing into `scratchEffects`/`scratchStops` here.
/// `offset`'s high bit (kRuntimeEffectOffsetBit, xt/node.h) selects Document's runtime effect
/// declaration pool (`runtimeEffects`/`runtimeStops`, StyleEngine::setRuntimeEffects) over the
/// compiled IR's STYL `effects[]`/`effectStops[]`. `scratchEffects`/`scratchStops` hold
/// `scratchEffectsCap` effects and `scratchStopsCap` stops (18.2's per-node/per-effect maximums).
/// Returns false (empty `outEffects`/`outStops`) when `count == 0`, the selected pool pointer is
/// null, any offset/count in the chain is out of range, or the sequence does not fit the scratch
/// arrays -- defensive for IR (IrView::open caps it at kMaxEffectSequence); a runtime declaration
/// (Document::declareRuntimeEffects()) may be any length.
bool resolveEffectSequence(const IrView& ir, const std::vector<EffectInstance>* runtimeEffects,
                           const std::vector<GradientStop>* runtimeStops, uint32_t offset, uint32_t count,
                           EffectInstance* scratchEffects, uint32_t scratchEffectsCap,
                           GradientStop* scratchStops, uint32_t scratchStopsCap,
                           std::span<const EffectInstance>& outEffects,
                           std::span<const GradientStop>& outStops) {
    outEffects = {};
    outStops = {};
    if (count == 0) {
        return true;
    }
    if (count > scratchEffectsCap) {
        return false;  // T-255: would overrun the caller's stack array.
    }
    uint32_t stopCursor = 0;
    if ((offset & kRuntimeEffectOffsetBit) != 0) {
        uint32_t idx = offset & ~kRuntimeEffectOffsetBit;
        if (runtimeEffects == nullptr || runtimeStops == nullptr ||
            static_cast<uint64_t>(idx) + count > runtimeEffects->size()) {
            return false;
        }
        std::span<const GradientStop> stopPool(*runtimeStops);
        for (uint32_t i = 0; i < count; ++i) {
            EffectInstance dst = (*runtimeEffects)[idx + i];
            uint32_t localCount = dst.stopCount;
            uint32_t localOffset = dst.stopOffset;
            uint32_t base = stopCursor;
            if (!copyEffectStops<GradientStop>(stopPool, localOffset, localCount, scratchStops,
                                               scratchStopsCap, stopCursor)) {
                return false;
            }
            dst.stopOffset = base;
            scratchEffects[i] = dst;
        }
    } else {
        std::span<const IrEffect> irEffects = ir.style().effects();
        std::span<const IrGradientStop> irStops = ir.style().effectStops();
        if (static_cast<uint64_t>(offset) + count > irEffects.size()) {
            return false;
        }
        for (uint32_t i = 0; i < count; ++i) {
            const IrEffect& src = irEffects[offset + i];
            EffectInstance dst{};
            dst.kind = static_cast<EffectKind>(src.kind);
            dst.enabled = src.enabled;
            dst.shader = src.shader;
            for (size_t p = 0; p < 8; ++p) {
                dst.params[p] = src.params[p];
            }
            dst.colors[0] = src.colors[0];
            dst.colors[1] = src.colors[1];
            uint32_t base = stopCursor;
            if (!copyEffectStops<IrGradientStop>(irStops, src.stopOffset, src.stopCount, scratchStops,
                                                 scratchStopsCap, stopCursor)) {
                return false;
            }
            dst.stopOffset = base;
            dst.stopCount = src.stopCount;
            scratchEffects[i] = dst;
        }
    }
    outEffects = std::span<const EffectInstance>(scratchEffects, count);
    outStops = std::span<const GradientStop>(scratchStops, stopCursor);
    return true;
}

/// 05.5c (T-115): decodes `next.gradientOffset/gradientCount` into `out`, the element's resolved
/// `background-gradient`. The offset's high bit (kRuntimeEffectOffsetBit, xt/node.h) selects
/// Document's runtime gradient declaration pool (StyleEngine::setRuntimeGradients) over the
/// compiled IR's STYL `gradientPool[]`, exactly as it does for `effect`. A count of 0, or an
/// out-of-range offset (defensive; a validated IR never has one), yields `GradientKind::None`.
void resolveBackgroundGradient(const IrView& ir, const std::vector<ElementGradient>* runtimeGradients,
                               const ComputedStyle& next, ElementGradient& out) {
    out.kind = GradientKind::None;
    out.stops.clear();
    if (next.gradientCount == 0) {
        return;
    }
    uint32_t offset = next.gradientOffset;
    if ((offset & kRuntimeEffectOffsetBit) != 0) {
        uint32_t idx = offset & ~kRuntimeEffectOffsetBit;
        if (runtimeGradients == nullptr || idx >= runtimeGradients->size()) {
            return;
        }
        out = (*runtimeGradients)[idx];
        return;
    }
    std::span<const IrGradient> pool = ir.style().gradients();
    if (offset >= pool.size()) {
        return;
    }
    const IrGradient& src = pool[offset];
    out.kind = static_cast<GradientKind>(src.kind);
    if (out.kind == GradientKind::Linear) {
        out.angle = src.params[0];
    } else if (out.kind == GradientKind::Conic) {  // T-201.
        out.angle = src.params[0];
        out.cx = Length{src.params[2], static_cast<LengthUnit>(src.units[2])};
        out.cy = Length{src.params[3], static_cast<LengthUnit>(src.units[3])};
    } else {
        out.rx = Length{src.params[0], static_cast<LengthUnit>(src.units[0])};
        out.ry = Length{src.params[1], static_cast<LengthUnit>(src.units[1])};
        out.cx = Length{src.params[2], static_cast<LengthUnit>(src.units[2])};
        out.cy = Length{src.params[3], static_cast<LengthUnit>(src.units[3])};
    }
    std::span<const IrGradientStop> stops = ir.style().effectStops();
    if (static_cast<uint64_t>(src.stopOffset) + src.stopCount > stops.size()) {
        out.kind = GradientKind::None;  // Defensive; a validated IR never has one.
        return;
    }
    for (uint32_t i = 0; i < src.stopCount; ++i) {
        GradientStop stop;
        stop.offset = stops[src.stopOffset + i].offset;
        stop.color = stops[src.stopOffset + i].color;
        out.stops.push_back(stop);
    }
}

/// 05 §5.9.8 (T-332): decodes `next.clipShape` into `out`, the element's resolved `clip-path`.
/// 0 is `none`; a value with kRuntimeEffectOffsetBit indexes Document's runtime clip-shape pool
/// (StyleEngine::setRuntimeClipShapes), any other value is 1 + the index into the compiled IR's
/// `CLIP` chunk. An out-of-range index (defensive; a validated IR never has one) yields None.
void resolveClipShape(const IrView& ir, const std::vector<ElementClipShape>* runtimeShapes,
                      const ComputedStyle& next, ElementClipShape& out) {
    out.kind = ClipShapeKind::None;
    out.params.clear();
    const uint32_t ref = next.clipShape;
    if (ref == 0) {
        return;
    }
    if ((ref & kRuntimeEffectOffsetBit) != 0) {
        const uint32_t idx = ref & ~kRuntimeEffectOffsetBit;
        if (runtimeShapes == nullptr || idx >= runtimeShapes->size()) {
            return;
        }
        out = (*runtimeShapes)[idx];
        return;
    }
    const ClipShapeChunk& clips = ir.clipShapes();
    const uint32_t idx = ref - 1;
    if (idx >= clips.shapes().size()) {
        return;
    }
    const IrClipShape& src = clips.shapes()[idx];
    std::span<const IrClipParam> pool = clips.params();
    if (static_cast<uint64_t>(src.paramOffset) + src.paramCount > pool.size()) {
        return;
    }
    out.kind = static_cast<ClipShapeKind>(src.kind);
    out.params.reserve(src.paramCount);
    for (uint32_t i = 0; i < src.paramCount; ++i) {
        const IrClipParam& p = pool[src.paramOffset + i];
        out.params.push_back(Length{p.value, static_cast<LengthUnit>(p.unit)});
    }
}

/// 06 §6.15 (T-338): decodes `next.gridColumns` / `gridRows` / `gridAreas` (0 = none, else 1 + a
/// `GRID` list index) into `out`. An out-of-range reference (defensive; a validated IR never has
/// one) decodes as `none`.
void resolveGrid(const IrView& ir, const ComputedStyle& next, ElementGrid& out) {
    out.columns.clear();
    out.rows.clear();
    out.areas.clear();
    out.areaRows = 0;
    out.areaCols = 0;
    const GridChunk& grid = ir.grid();
    const auto tracks = [&grid](uint32_t ref, std::vector<GridTrack>& dst) {
        if (ref == 0 || ref > grid.trackLists().size()) {
            return;
        }
        const IrGridList& list = grid.trackLists()[ref - 1];
        if (static_cast<uint64_t>(list.offset) + list.count > grid.tracks().size()) {
            return;
        }
        dst.reserve(list.count);
        for (uint32_t i = 0; i < list.count; ++i) {
            const IrGridTrack& t = grid.tracks()[list.offset + i];
            dst.push_back(GridTrack{static_cast<GridTrackKind>(t.kind), t.value});
        }
    };
    tracks(next.gridColumns, out.columns);
    tracks(next.gridRows, out.rows);
    const uint32_t areaRef = next.gridAreas;
    if (areaRef != 0 && areaRef <= grid.areaLists().size()) {
        const IrGridList& list = grid.areaLists()[areaRef - 1];
        if (static_cast<uint64_t>(list.offset) + list.count <= grid.areas().size()) {
            out.areaRows = list.rows;
            out.areaCols = list.cols;
            out.areas.reserve(list.count);
            for (uint32_t i = 0; i < list.count; ++i) {
                const IrGridArea& a = grid.areas()[list.offset + i];
                out.areas.push_back(GridArea{a.name, a.rowStart, a.rowEnd, a.colStart, a.colEnd});
            }
        }
    }
}

/// 18-effects.md §18.4's cascade paragraph (T-51b): whole-sequence replace, no inheritance;
/// when the new sequence's per-slot EffectKind and count exactly match the node's current one,
/// diffs each animatable parameter and registers a transition per docs/design's `transition`
/// rules (`effect` covers every effect parameter, see transitionCovers() above); otherwise
/// cancels every in-flight effect-parameter transition on `node` and replaces the stack outright.
void applyEffectCascade(Animator& animator, const IrView& ir, const ComputedStyle& next, NodeArena& arena,
                        NodeId node, const std::vector<EffectInstance>* runtimeEffects,
                        const std::vector<GradientStop>* runtimeStops, float clock, bool firstResolve) {
    constexpr uint32_t kMaxEffects = kMaxEffectSequence;
    constexpr uint32_t kMaxStops = kMaxEffects * 16;
    EffectInstance scratchEffects[kMaxEffects];
    GradientStop scratchStops[kMaxStops];
    std::span<const EffectInstance> newEffects;
    std::span<const GradientStop> newStops;
    if (!resolveEffectSequence(ir, runtimeEffects, runtimeStops, next.effectOffset, next.effectCount,
                               scratchEffects, kMaxEffects, scratchStops, kMaxStops, newEffects, newStops)) {
        return;  // Out-of-range decl (defensive); leave the node's current effect stack untouched.
    }
    // T-345 (05 §5.9.9): radii, offsets, widths and mask points are px of the element's plane --
    // scaled by its `ui-scale` factor like every other px length (effectToScreen with a uniform
    // scale). A custom() effect's parameters are its shader's own and stay as declared.
    EffectInstance scaledEffects[kMaxEffects];
    if (next.uiScaleEff != 1.0f && !newEffects.empty() && newEffects.size() <= kMaxEffects) {
        const float s = next.uiScaleEff;
        const Mat2x3 uniform{s, 0, 0, s, 0, 0};
        for (size_t i = 0; i < newEffects.size(); ++i) {
            scaledEffects[i] = newEffects[i].kind == EffectKind::Custom
                                   ? newEffects[i]
                                   : effectToScreen(newEffects[i], uniform);
        }
        newEffects = std::span<const EffectInstance>(scaledEffects, newEffects.size());
    }

    NodeCommon& common = arena.common(node);
    bool sameKindSeq = common.effectCount == newEffects.size();
    if (sameKindSeq && !newEffects.empty()) {
        std::span<const EffectInstance> curEffects =
            std::span<const EffectInstance>(arena.effects()).subspan(common.effectOffset, common.effectCount);
        for (size_t i = 0; i < newEffects.size(); ++i) {
            // T-208 (18 §18.13): two custom() effects are the same slot only for the same shader.
            if (curEffects[i].kind != newEffects[i].kind ||
                (newEffects[i].kind == EffectKind::Custom && curEffects[i].shader != newEffects[i].shader)) {
                sameKindSeq = false;
                break;
            }
        }
    }
    if (!sameKindSeq) {
        animator.cancelTransitionsForNodeRange(node, prop::kEffectPropBase, 0xFFFFu);
        arena.assignEffects(node, newEffects, newStops);
        return;
    }
    if (newEffects.empty()) {
        return;  // No effects before or after; nothing to do.
    }
    // Same-kind sequence: capture "from" finals before assignEffects() overwrites the pool in
    // place, then diff against the "to" finals to decide which parameters transition.
    Value fromValues[kMaxEffects][kAnimatableEffectParamCount];
    for (size_t i = 0; i < newEffects.size(); ++i) {
        for (size_t k = 0; k < kAnimatableEffectParamCount; ++k) {
            PropId p = effectParam(static_cast<uint32_t>(i), kAnimatableEffectParams[k]);
            fromValues[i][k] = getProperty(arena, node, p);
        }
    }
    arena.assignEffects(node, newEffects, newStops);
    for (size_t i = 0; i < newEffects.size(); ++i) {
        for (size_t k = 0; k < kAnimatableEffectParamCount; ++k) {
            PropId p = effectParam(static_cast<uint32_t>(i), kAnimatableEffectParams[k]);
            Value toFinal = getProperty(arena, node, p);
            if (!valueEquals(fromValues[i][k], toFinal)) {
                maybeRegisterTransition(animator, ir, next, node, p, fromValues[i][k], toFinal, clock,
                                        firstResolve);
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// T-170: design tokens (docs/design/05-style-and-css.md §5.9.2, 13-api.md §13.8.2).
// ---------------------------------------------------------------------------------------------

/// The Value type applyRuntimeDecl() expects for `p`, for the properties a `var()` may feed (the
/// plain Float/Length/Color/Enum ones). False for every list/string-valued property.
bool tokenTargetType(uint16_t p, ValueType& out) {
    switch (p) {
        case prop::Opacity:
        case prop::ScaleX:
        case prop::ScaleY:
        case prop::Rotation:
        case prop::SkewX:
        case prop::SkewY:
        case prop::TranslateZ:
        case prop::AnchorZ:
        case prop::RotationX:
        case prop::RotationY:
            out = ValueType::Float;
            return true;
        case prop::TranslateX:
        case prop::TranslateY:
        case prop::AnchorX:
        case prop::AnchorY:
        case prop::LineHeight:
            out = ValueType::Length;
            return true;
        // T-218: the vector properties of scene-target rules.
        case prop::FillColor:
        case prop::StrokeColor:
            out = ValueType::Color;
            return true;
        case prop::FillOpacity:
        case prop::StrokeOpacity:
        case prop::TrimStart:
        case prop::TrimEnd:
        case prop::TrimOffset:
        case prop::StrokeTaperStart:  // T-233
        case prop::StrokeTaperEnd:
            out = ValueType::Float;
            return true;
        case prop::StrokeWidth:
        case prop::DashOffset:
            out = ValueType::Length;
            return true;
        default:
            break;
    }
    if ((p >= prop::Width && p <= prop::LetterSpacing) || (p >= prop::BackgroundColor && p <= prop::Order)) {
        out = propertyType(p);
        return out == ValueType::Float || out == ValueType::Length || out == ValueType::Color;
    }
    namespace sp = style_decode::styleprop;
    if (p == sp::ZIndex || p == sp::ClipStart || p == sp::ClipRate) {  // T-191: clip-*.
        out = ValueType::Float;
        return true;
    }
    if (p < sp::Display || p > sp::BackgroundGradient || p == sp::FontFamily || p == sp::Transition ||
        p == sp::Animation || p == sp::BackgroundImage || p == sp::Lang || p == sp::Effect ||
        p == sp::BackgroundGradient) {
        return false;
    }
    out = ValueType::Enum;
    return true;
}

/// Fits a token's value to property `p`: the typed value when its type matches (a px Length and a
/// Float stand in for each other), else the source text re-parsed as `p`'s value.
bool coerceToken(uint16_t p, const Value* typed, std::string_view text, Value& out) {
    ValueType want = ValueType::Float;
    if (!tokenTargetType(p, want)) {
        return false;
    }
    if (typed != nullptr) {
        if (typed->type == want) {
            out = *typed;
            return true;
        }
        if (want == ValueType::Float && typed->type == ValueType::Length &&
            typed->len.unit == LengthUnit::Px) {
            out = Value::ofFloat(typed->len.value);
            return true;
        }
        if (want == ValueType::Length && typed->type == ValueType::Float) {
            out = Value::ofLength(typed->f, LengthUnit::Px);
            return true;
        }
    }
    Value parsed;
    if (!text.empty() && mini_css::parseTokenText(p, text, parsed) && parsed.type == want) {
        out = parsed;
        return true;
    }
    return false;
}

/// T-176: an IR `calc()` declaration (only IR declarations carry one; mini_css has no calc()).
bool isCalcEntry(const Entry& e) { return e.decl != nullptr && e.decl->valueType == kDeclCalc; }

/// T-176: a length the style pass cannot keep as-is for layout (it depends on the device).
bool isDeviceUnit(LengthUnit unit) {
    return static_cast<uint8_t>(unit) >= static_cast<uint8_t>(LengthUnit::Vw) && unit != LengthUnit::Rem;
}

/// T-176 (05.9.3): one `@media` condition against the viewport and the device.
bool mediaCondHolds(const IrMediaCond& cond, float width, float height, float aspect,
                    const DeviceInfo& device) {
    constexpr float kAspectEpsilon = 1e-4f;
    switch (static_cast<MediaFeature>(cond.feature)) {
        case MediaFeature::MinWidth:
            return width >= cond.value;
        case MediaFeature::MaxWidth:
            return width <= cond.value;
        case MediaFeature::MinHeight:
            return height >= cond.value;
        case MediaFeature::MaxHeight:
            return height <= cond.value;
        case MediaFeature::MinAspect:
            return aspect >= cond.value - kAspectEpsilon;
        case MediaFeature::MaxAspect:
            return aspect <= cond.value + kAspectEpsilon;
        case MediaFeature::Orientation:
            return (aspect >= 1.0f ? 0.0f : 1.0f) == cond.value;
        case MediaFeature::Platform:
            return static_cast<float>(device.platform) == cond.value;
        case MediaFeature::Input:
            return static_cast<float>(device.input) == cond.value;
        case MediaFeature::ReducedMotion:  // T-330.
            return (device.reducedMotion ? 1.0f : 0.0f) == cond.value;
        case MediaFeature::MinResolution:
            return device.dpr >= cond.value - kAspectEpsilon;
        case MediaFeature::MaxResolution:
            return device.dpr <= cond.value + kAspectEpsilon;
        case MediaFeature::Resolution:
            return device.dpr >= cond.value - kAspectEpsilon && device.dpr <= cond.value + kAspectEpsilon;
    }
    return false;
}

bool isVarEntry(const Entry& e) {
    return (e.decl != nullptr && e.decl->valueType == kDeclVarRef) ||
           (e.runtime != nullptr && e.runtime->varName != kInvalidString);
}

}  // namespace

std::string_view StyleEngine::stringOf(StringId id) const {
    if (m_doc != nullptr) {
        return m_doc->string(id);  // T-270d: the runtime text slots (kRuntimeTextBase) too.
    }
    if (id == kInvalidString) {
        return {};
    }
    if (id < kRuntimeStringBase) {
        return m_ir->string(id);
    }
    uint32_t index = id - kRuntimeStringBase;
    if (m_runtimeStrings == nullptr || index >= m_runtimeStrings->size()) {
        return {};
    }
    return (*m_runtimeStrings)[index];
}

bool StyleEngine::lookupToken(std::string_view name, uint16_t p, Value& out) const {
    // Most specific layer first; a layer whose value does not fit `p` does not hide the next one.
    for (const TokenMap* layer : {m_overrideTokens, m_themeTokens}) {
        if (layer == nullptr) {
            continue;
        }
        auto it = layer->find(name);
        if (it != layer->end() &&
            coerceToken(p, it->second.hasValue ? &it->second.value : nullptr, it->second.text, out)) {
            return true;
        }
    }
    for (const IrVar& var : m_ir->vars().tokens()) {
        if (m_ir->string(var.name) != name) {
            continue;
        }
        Value typed;
        std::string_view text;
        switch (static_cast<ValueType>(var.valueType)) {
            case ValueType::Float:
                typed = Value::ofFloat(irValueF32(var.value, 0));
                break;
            case ValueType::Length:
                typed = Value::ofLength(irValueF32(var.value, 0), static_cast<LengthUnit>(var.value[4]));
                break;
            case ValueType::Color:
                typed = Value::ofColor(Color{irValueF32(var.value, 0), irValueF32(var.value, 1),
                                             irValueF32(var.value, 2), irValueF32(var.value, 3)});
                break;
            default:
                text = m_ir->string(irValueU32(var.value, 0));
                return coerceToken(p, nullptr, text, out);
        }
        return coerceToken(p, &typed, text, out);
    }
    return false;
}

bool StyleEngine::tokenColor(std::string_view name, Color& out) const {
    Value value;
    if (m_ir == nullptr || !lookupToken(name, prop::Color, value) || value.type != ValueType::Color) {
        return false;
    }
    out = value.c;
    return true;
}

bool StyleEngine::lookupCalcToken(std::string_view name, Value& out) const {
    for (const TokenMap* layer : {m_overrideTokens, m_themeTokens}) {
        if (layer == nullptr) {
            continue;
        }
        auto it = layer->find(name);
        if (it != layer->end() && it->second.hasValue &&
            (it->second.value.type == ValueType::Float || it->second.value.type == ValueType::Length) &&
            !(it->second.value.type == ValueType::Length &&
              (it->second.value.len.unit == LengthUnit::Auto ||
               it->second.value.len.unit == LengthUnit::Calc))) {
            out = it->second.value;
            return true;
        }
    }
    for (const IrVar& var : m_ir->vars().tokens()) {
        if (m_ir->string(var.name) != name) {
            continue;
        }
        if (static_cast<ValueType>(var.valueType) == ValueType::Float) {
            out = Value::ofFloat(irValueF32(var.value, 0));
            return true;
        }
        if (static_cast<ValueType>(var.valueType) == ValueType::Length) {
            const auto unit = static_cast<LengthUnit>(var.value[4]);
            if (unit == LengthUnit::Auto || unit == LengthUnit::Calc) {
                return false;
            }
            out = Value::ofLength(irValueF32(var.value, 0), unit);
            return true;
        }
        return false;
    }
    return false;
}

bool StyleEngine::instantiateCalc(uint32_t index, float emPx, Length& out, bool& usedToken) {
    std::span<const IrCalc> calcs = m_ir->calcs().calcs();
    if (index >= calcs.size()) {
        return false;
    }
    std::span<const IrCalcOp> ops = m_ir->calcs().ops().subspan(calcs[index].opOffset, calcs[index].opCount);
    bool needsBuild = false;
    for (const IrCalcOp& op : ops) {
        const auto unit = static_cast<LengthUnit>(op.unit);
        needsBuild = needsBuild || static_cast<CalcOpKind>(op.op) == CalcOpKind::PushVar ||
                     (static_cast<CalcOpKind>(op.op) == CalcOpKind::PushLength &&
                      (unit == LengthUnit::Em || unit == LengthUnit::Rem ||
                       (m_calcScale != 1.0f && unit == LengthUnit::Px)));  // T-345: px is scaled.
    }
    if (!needsBuild) {
        out = Length{static_cast<float>(index), LengthUnit::Calc};
        return true;
    }
    // A pushed length: `em` (and T-335's `rem`) becomes px now, everything else is left for layout.
    // T-345 (05 §5.9.9): px / em / rem terms are design px in the element's `ui-scale` factor --
    // under `auto` as `uiscale` units (1uiscale = the Document's uiscale px, so the program stays
    // the same whatever the output size), otherwise as px times the factor.
    auto pushLength = [this, emPx](float value, LengthUnit unit) {
        IrCalcOp op{};
        op.op = static_cast<uint8_t>(CalcOpKind::PushLength);
        op.name = kInvalidString;
        float px = value;
        if (unit == LengthUnit::Em) {
            px = value * emPx;
        } else if (unit == LengthUnit::Rem) {
            px = value * m_rootFontPx;
        } else if (unit != LengthUnit::Px) {
            op.unit = static_cast<uint8_t>(unit);
            op.value = value;
            m_calcBuild.push_back(op);
            return;
        }
        if (m_calcScale == 1.0f) {
            op.unit = static_cast<uint8_t>(LengthUnit::Px);
            op.value = px;
        } else if (m_calcAuto) {
            op.unit = static_cast<uint8_t>(LengthUnit::UiScale);
            op.value = px;
        } else {
            op.unit = static_cast<uint8_t>(LengthUnit::Px);
            op.value = px * m_calcScale;
        }
        m_calcBuild.push_back(op);
    };
    auto pushNumber = [this](float value) {
        IrCalcOp op{};
        op.op = static_cast<uint8_t>(CalcOpKind::PushNumber);
        op.name = kInvalidString;
        op.value = value;
        m_calcBuild.push_back(op);
    };
    m_calcBuild.clear();
    for (const IrCalcOp& op : ops) {
        const auto kind = static_cast<CalcOpKind>(op.op);
        if (kind == CalcOpKind::PushLength) {
            pushLength(op.value, static_cast<LengthUnit>(op.unit));
            continue;
        }
        if (kind != CalcOpKind::PushVar) {
            IrCalcOp copy = op;
            copy.name = kInvalidString;
            m_calcBuild.push_back(copy);
            continue;
        }
        usedToken = true;
        Value token;
        if (lookupCalcToken(stringOf(op.name), token)) {
            if (token.type == ValueType::Float) {
                pushNumber(token.f);
            } else {
                pushLength(token.len.value, token.len.unit);
            }
        } else if (op.hasFallback != 0) {
            if (op.unit == kCalcUnitNumber) {
                pushNumber(op.value);
            } else {
                pushLength(op.value, static_cast<LengthUnit>(op.unit));
            }
        } else {
            return false;
        }
    }
    // Deduplicate: a program that resolves the same way every time reuses its pool entry.
    const size_t n = m_calcBuild.size();
    for (size_t i = 0; i < m_runtimeCalcs.size(); ++i) {
        const IrCalc& calc = m_runtimeCalcs[i];
        if (calc.opCount == n && std::memcmp(m_runtimeCalcOps.data() + calc.opOffset, m_calcBuild.data(),
                                             n * sizeof(IrCalcOp)) == 0) {
            out = Length{static_cast<float>(kRuntimeCalcBase + i), LengthUnit::Calc};
            return true;
        }
    }
    IrCalc calc{};
    calc.opOffset = static_cast<uint32_t>(m_runtimeCalcOps.size());
    calc.opCount = static_cast<uint32_t>(n);
    m_runtimeCalcOps.insert(m_runtimeCalcOps.end(), m_calcBuild.begin(), m_calcBuild.end());
    m_runtimeCalcs.push_back(calc);
    out = Length{static_cast<float>(kRuntimeCalcBase + m_runtimeCalcs.size() - 1), LengthUnit::Calc};
    return true;
}

uint32_t StyleEngine::updateMedia(NodeArena& arena, const LengthEnv& env, const DeviceInfo& device) {
    const MediaChunk& chunk = m_ir->media();
    if (chunk.rules().empty()) {
        return 0;
    }
    const float width = env.viewportW;
    const float height = env.viewportH;
    const float aspect = device.aspect > 0.0f ? device.aspect : (height > 0.0f ? width / height : 0.0f);
    std::span<const IrRule> rules = m_ir->style().rules();
    std::vector<uint8_t> next(rules.size(), 1);
    for (const IrMediaRule& entry : chunk.rules()) {
        const IrMedia& media = chunk.media()[entry.media];
        bool any = false;
        for (uint32_t q = 0; q < media.queryCount && !any; ++q) {
            const IrMediaQuery& query = chunk.queries()[media.queryOffset + q];
            bool all = true;
            for (uint32_t c = 0; c < query.condCount && all; ++c) {
                all = mediaCondHolds(chunk.conds()[query.condOffset + c], width, height, aspect, device);
            }
            any = all;
        }
        next[entry.rule] = any ? 1 : 0;
    }
    uint32_t changed = 0;
    if (!m_mediaEvaluated) {
        m_mediaEvaluated = true;
        m_ruleActive = std::move(next);
        return 0;
    }
    std::span<const IrSelPart> selParts = m_ir->style().selectorParts();
    std::span<const uint32_t> classIds = m_ir->style().classIds();
    // T-173's condition parts: attribute/`:lang()` text through the string pools, `:focus-within`
    // from the last resolve; child positions are computed on demand.
    SelectorMatchContext matchCtx;
    matchCtx.string = [](const void* self, StringId id) {
        return static_cast<const StyleEngine*>(self)->stringOf(id);
    };
    matchCtx.stringsCtx = this;
    matchCtx.customStateNames = m_ir->customStates().names();  // T-174: `:state()`.
    matchCtx.documentLang = m_documentLang;                    // T-179.
    if (m_usesFocusWithin) {
        matchCtx.focusWithin = &m_focusWithin;
    }
    for (uint32_t ri = 0; ri < rules.size(); ++ri) {
        if (next[ri] == m_ruleActive[ri]) {
            continue;
        }
        ++changed;
        for (NodeId node = 0; node < arena.capacity(); ++node) {
            if (!arena.alive(node) || arena.kind(node) != NodeKind::Element ||
                !matchRule(arena, *m_ir, node, rules[ri], selParts, classIds, matchCtx)) {
                continue;
            }
            arena.markDirty(node, static_cast<uint8_t>(kDirtyStyle));
            arena.traversePreorder(node, [&arena](NodeId n) {
                arena.markDirty(n, static_cast<uint8_t>(kDirtyStyle));
                return true;
            });
        }
    }
    m_ruleActive = std::move(next);
    return changed;
}

uint32_t StyleEngine::markDeviceUsersDirty(NodeArena& arena) {
    uint32_t count = 0;
    for (NodeId node = 0; node < m_deviceUsers.size(); ++node) {
        if (m_deviceUsers[node] == 0 || !arena.alive(node) || arena.kind(node) != NodeKind::Element) {
            continue;
        }
        ++count;
        arena.markDirty(node, static_cast<uint8_t>(kDirtyStyle));
        arena.traversePreorder(node, [&arena](NodeId n) {
            arena.markDirty(n, static_cast<uint8_t>(kDirtyStyle));
            return true;
        });
    }
    return count;
}

uint32_t StyleEngine::markTokenUsersDirty(NodeArena& arena) {
    uint32_t count = 0;
    for (NodeId node = 0; node < m_tokenUsers.size(); ++node) {
        if (m_tokenUsers[node] == 0 || !arena.alive(node) || arena.kind(node) != NodeKind::Element) {
            continue;
        }
        ++count;
        arena.markDirty(node, static_cast<uint8_t>(kDirtyStyle));
        arena.traversePreorder(node, [&arena](NodeId n) {
            arena.markDirty(n, static_cast<uint8_t>(kDirtyStyle));
            return true;
        });
    }
    return count;
}

void StyleEngine::resolveImageMeta(const IrView& ir, const ComputedStyle& next, ElementData& data) const {
    data.backgroundSubUv = Rect{0, 0, 1, 1};
    data.backgroundImageSize = Vec2{0, 0};
    data.borderImageSubUv = Rect{0, 0, 1, 1};
    if (m_assets == nullptr) {
        return;
    }
    if (next.backgroundImage != kInvalidAsset) {
        data.backgroundSubUv = m_assets->imageSubUv(next.backgroundImage, ir);
        // T-327: background-size / background-position need the image's own size.
        data.backgroundImageSize = m_assets->imageSize(next.backgroundImage, ir);
    }
    if (next.borderImage == kInvalidAsset) {
        return;
    }
    const AssetResolver::ImageMeta* meta = m_assets->imageMeta(next.borderImage, ir);
    if (meta != nullptr && meta->inAtlas) {
        data.borderImageSubUv = meta->subUv;
    }
    data.borderImageSize = m_assets->imageSize(next.borderImage, ir);
    for (int i = 0; i < 4; ++i) {
        const float declared = next.borderImageSlice[i];
        const float extent = i < 2 ? data.borderImageSize.x : data.borderImageSize.y;
        float px = 0.0f;
        if (declared != declared) {  // NaN: `auto`.
            px = (meta != nullptr && meta->hasSlice) ? meta->slice[i] : 0.0f;
        } else if (declared < 0.0f) {
            px = -declared * extent;
        } else {
            px = declared;
        }
        data.borderImageSlicePx[i] = px;
    }
}

void StyleEngine::init(const IrView& ir, Animator& animator, TextShaper* shaper) {
    m_ir = &ir;
    m_animator = &animator;
    m_shaper = shaper;

    // T-173: which condition-part features (xt/ir_pod.h's kIrSelCondition) the stylesheet uses.
    m_usesStructural = false;
    m_usesFocusWithin = false;
    m_focusWithinDeep = false;
    std::span<const IrSelPart> parts = ir.style().selectorParts();
    for (const IrRule& rule : ir.style().rules()) {
        bool inSubject = true;
        for (uint32_t i = 0; i < rule.selCount; ++i) {
            const IrSelPart& part = parts[rule.selOffset + i];
            if (part.combinator != kIrSelCondition) {
                inSubject = i == 0;
                continue;
            }
            const auto code = static_cast<IrSelCond>(part.classCount & ~kIrSelCondNegated);
            if (code == IrSelCond::NthChild || code == IrSelCond::FirstChild ||
                code == IrSelCond::LastChild) {
                m_usesStructural = true;
            } else if (code == IrSelCond::FocusWithin) {
                m_usesFocusWithin = true;
                m_focusWithinDeep = m_focusWithinDeep || !inSubject;
            }
        }
    }
    // T-176: until updateMedia() runs, an `@media` rule is inactive.
    m_ruleActive.assign(ir.style().rules().size(), 1);
    m_ruleIsMedia.assign(ir.style().rules().size(), 0);  // T-345: what `media-override` skips.
    for (const IrMediaRule& entry : ir.media().rules()) {
        m_ruleActive[entry.rule] = 0;
        m_ruleIsMedia[entry.rule] = 1;
    }
    m_mediaEvaluated = false;
}

bool StyleEngine::setScaleInputs(float uiScale, float dpr) {
    const float u = uiScale > 0.0f && std::isfinite(uiScale) ? uiScale : 1.0f;
    const float d = dpr > 0.0f && std::isfinite(dpr) ? dpr : 1.0f;
    const bool changed = u != m_uiScale || d != m_dpr;
    m_uiScale = u;
    m_dpr = d;
    return changed;
}

void StyleEngine::updateFocusWithin(NodeArena& arena) {
    // T-173 (`:focus-within`): recompute "the element or a descendant has kStateFocus" for every
    // node and mark the elements whose answer changed kDirtyStyle (ancestors follow automatically)
    // -- plus, when some selector tests it on an ancestor compound, the changed elements' whole
    // subtrees. Done here rather than at every focus-changing call site so that host setState(),
    // Document::setFocus() and subtree removal are all covered.
    const uint32_t capacity = arena.capacity();
    m_focusWithin.resize(capacity, 0);
    m_focusWithinNext.assign(capacity, 0);
    for (NodeId n = 0; n < capacity; ++n) {
        if (!arena.alive(n) || arena.kind(n) != NodeKind::Element ||
            (arena.element(n).stateBits & static_cast<uint16_t>(kStateFocus)) == 0) {
            continue;
        }
        for (NodeId a = n; a != kInvalidNode && m_focusWithinNext[a] == 0; a = arena.parent(a)) {
            m_focusWithinNext[a] = 1;
        }
    }
    for (NodeId n = 0; n < capacity; ++n) {
        if (m_focusWithin[n] == m_focusWithinNext[n] || !arena.alive(n) ||
            arena.kind(n) != NodeKind::Element) {
            continue;
        }
        arena.markDirty(n, static_cast<uint8_t>(kDirtyStyle));
        NodeId parent = arena.parent(n);
        const bool topmost = parent == kInvalidNode || m_focusWithin[parent] == m_focusWithinNext[parent];
        if (m_focusWithinDeep && topmost) {
            arena.traversePreorder(n, [&arena](NodeId d) {
                arena.markDirty(d, static_cast<uint8_t>(kDirtyStyle));
                return true;
            });
        }
    }
    m_focusWithin.swap(m_focusWithinNext);
}

uint32_t StyleEngine::resolve(NodeArena& arena, float clock) {
    XT_ASSERT(m_ir != nullptr && m_animator != nullptr);
    const IrView& ir = *m_ir;
    Animator& animator = *m_animator;

    std::span<const IrRule> rules = ir.style().rules();
    std::span<const IrSelPart> selParts = ir.style().selectorParts();
    std::span<const IrDecl> decls = ir.style().decls();
    std::span<const uint32_t> classIds = ir.style().classIds();
    std::span<const IrDecl> inlineDecls = ir.doc().inlineDecls();

    // T-38 (docs/design/01-architecture.md §1.7's per-frame zero-allocation goal): `entries` and
    // `stack` below used to be locals, reallocated on every resolve() call regardless of how many
    // (if any) elements were actually dirty -- `stack` in particular is pushed once per Element in
    // the whole tree just to walk down to the dirty ones, every single update(). A function-local
    // `static` keeps their capacity across calls, the same effect as promoting them to StyleEngine
    // members without exposing the file-local Entry type through style_engine.h. resolve() is not
    // re-entrant (see below) and Context is single-threaded (01.6), so a plain (non-thread_local)
    // static is safe here.
    static std::vector<Entry> entries;
    static std::vector<Entry> sceneEntries;  // T-218: the element's scene-target declarations.
    uint32_t processed = 0;

    // T-173: the condition-part inputs of selector matching (selector_match.h).
    SelectorMatchContext matchCtx;
    matchCtx.string = [](const void* self, StringId id) {
        return static_cast<const StyleEngine*>(self)->stringOf(id);
    };
    matchCtx.stringsCtx = this;
    matchCtx.customStateNames = m_ir->customStates().names();  // T-174: `:state()`.
    matchCtx.documentLang = m_documentLang;                    // T-179.
    if (m_usesFocusWithin) {
        updateFocusWithin(arena);
        matchCtx.focusWithin = &m_focusWithin;
    }
    if (m_usesStructural) {
        m_childIndex.assign(arena.capacity(), 0);
        matchCtx.childIndex = &m_childIndex;
    }

    // NodeId order is not a reliable parent-before-child order: NodeArena reuses freed slots, so a
    // child created after its parent was destroyed-and-recreated can end up with a lower NodeId
    // (see docs/tasks/T-24 report). Find each Element tree's root (parent == kInvalidNode) and walk
    // it with a real (non-recursive) parent-before-child DFS instead. This does NOT use
    // NodeArena::traversePreorder: that helper drives its iteration off NodeArena's own internal
    // scratch stack, and resolving an element below can itself call arena.markDirty(...,
    // kDirtyTransform) (directly for transform-origin, or via setProperty's propertyDirtyBits() for
    // any of the general transform PropIds) -- which clears and reuses that SAME scratch stack,
    // corrupting the outer traversal's pending-node list. `stack` here is StyleEngine::resolve's own
    // scratch, untouched by anything NodeArena does internally, so no such reentrancy is possible.
    // Pruning descent at the first non-Element node is safe: Elements never nest inside a Text node
    // or an instantiated `anim` scene's Group/Path subtree.
    static std::vector<NodeId> stack;
    for (NodeId root = 0; root < arena.capacity(); ++root) {
        if (!arena.alive(root) || arena.kind(root) != NodeKind::Element ||
            arena.parent(root) != kInvalidNode || (arena.common(root).flags2 & kTemplate) != 0) {
            continue;  // T-177: a detached `<repeat>` template is never styled.
        }
        stack.clear();
        stack.push_back(root);
        if (m_usesStructural) {
            m_childIndex[root] = 1;
        }
        // T-335: `rem` of this tree; replaced below once the root itself is (re)resolved. T-345:
        // the root's unscaled font size (`rem` lengths are scaled like px).
        m_rootFontPx = arena.element(root).styleResolvedOnce != 0
                           ? arena.element(root).style.base[prop::FontSize].len.value /
                                 arena.element(root).style.uiScaleEff
                           : 16.0f;
        while (!stack.empty()) {
            NodeId node = stack.back();
            stack.pop_back();
            if (arena.kind(node) != NodeKind::Element) {
                continue;  // Prune: this subtree can't contain further Elements.
            }
            uint32_t elementChildren = 0;
            for (NodeId c = arena.firstChild(node); c != kInvalidNode; c = arena.nextSibling(c)) {
                stack.push_back(c);
                // T-173: `:nth-child` position, fresh for every child before it is matched.
                if (m_usesStructural && arena.kind(c) == NodeKind::Element) {
                    m_childIndex[c] = ++elementChildren;
                }
            }
            if (!(arena.dirty(node) & static_cast<uint8_t>(kDirtyStyle))) {
                continue;  // Not dirty itself; its children were already pushed above.
            }
            ++processed;
            ElementData& data = arena.element(node);

            // 1. Collect matching declarations (rules + inline), 2. sort by (important, specificity,
            // source order).
            entries.clear();
            sceneEntries.clear();
            bool anyScene = false;
            for (uint32_t ri = 0; ri < rules.size(); ++ri) {
                const IrRule& rule = rules[ri];
                if (!ruleActive(ri) || !matchRule(arena, ir, node, rule, selParts, classIds, matchCtx)) {
                    continue;
                }
                const IrSelPart* scene =
                    rule.selCount >= 2 && selParts[rule.selOffset + 1].combinator == kIrSelSceneTarget
                        ? &selParts[rule.selOffset + 1]
                        : nullptr;
                anyScene = anyScene || scene != nullptr;
                const bool fromMedia = ri < m_ruleIsMedia.size() && m_ruleIsMedia[ri] != 0;  // T-345
                for (uint32_t di = 0; di < rule.declCount; ++di) {
                    const IrDecl& d = decls[rule.declOffset + di];
                    entries.push_back(
                        Entry{d.important != 0, rule.specificity, ri, &d, nullptr, scene, fromMedia});
                }
            }
            uint32_t inlineOrderBase = static_cast<uint32_t>(rules.size());
            for (uint32_t i = 0; i < data.inlineDeclCount; ++i) {
                const IrDecl& d = inlineDecls[data.inlineDeclOffset + i];
                entries.push_back(Entry{d.important != 0, 1u << 24, inlineOrderBase + i, &d, nullptr});
            }
            // T-35: runtime (setStyle) declarations cascade with inline specificity, ordered right
            // after the element's own inline decls, so they win over the `style` attribute.
            uint32_t runtimeOrderBase = inlineOrderBase + data.inlineDeclCount;
            for (size_t i = 0; i < data.runtimeDecls.size(); ++i) {
                const RuntimeStyleDecl& d = data.runtimeDecls[i];
                entries.push_back(Entry{d.important != 0, 1u << 24,
                                        runtimeOrderBase + static_cast<uint32_t>(i), nullptr, &d});
            }
            // T-170: a `var()` declaration becomes the token's value (a scratch runtime decl), else
            // its fallback, else -- a runtime one without a fallback -- nothing at all. The scratch
            // is reserved first so the Entry pointers into it stay valid.
            {
                size_t varCount = 0;
                size_t calcCount = 0;
                for (const Entry& e : entries) {
                    varCount += isVarEntry(e) ? 1u : 0u;
                    calcCount += isCalcEntry(e) ? 1u : 0u;
                }
                if (m_tokenUsers.size() <= node) {
                    m_tokenUsers.resize(std::max<size_t>(arena.capacity(), size_t{node} + 1u), 0);
                }
                if (m_deviceUsers.size() <= node) {
                    m_deviceUsers.resize(std::max<size_t>(arena.capacity(), size_t{node} + 1u), 0);
                }
                m_tokenUsers[node] = varCount != 0 ? 1 : 0;
                m_deviceUsers[node] = 0;
                if (varCount + calcCount != 0) {
                    m_varScratch.clear();
                    m_varScratch.reserve(varCount + calcCount);
                }
                // T-176: a `calc()` declaration becomes a scratch runtime decl carrying the IR
                // program index (LengthUnit::Calc); its `em`/`var()` terms are substituted below,
                // once the element's font-size is known.
                for (Entry& e : entries) {
                    if (!isCalcEntry(e)) {
                        continue;
                    }
                    RuntimeStyleDecl resolved;
                    resolved.prop = e.decl->prop;
                    resolved.important = e.decl->important;
                    resolved.value =
                        Value::ofLength(static_cast<float>(irValueU32(e.decl->value, 0)), LengthUnit::Calc);
                    m_varScratch.push_back(resolved);
                    e.decl = nullptr;
                    e.runtime = &m_varScratch.back();
                }
                if (varCount != 0) {
                    std::span<const IrDecl> fallbacks = ir.vars().fallbacks();
                    for (Entry& e : entries) {
                        if (!isVarEntry(e)) {
                            continue;
                        }
                        RuntimeStyleDecl resolved;
                        uint32_t irFallback = kNoVarFallback;
                        const Value* runtimeFallback = nullptr;
                        StringId name = kInvalidString;
                        if (e.decl != nullptr) {
                            resolved.prop = e.decl->prop;
                            name = irValueU32(e.decl->value, 0);
                            irFallback = irValueU32(e.decl->value, 1);
                        } else {
                            resolved.prop = e.runtime->prop;
                            name = e.runtime->varName;
                            runtimeFallback =
                                e.runtime->hasVarFallback != 0 ? &e.runtime->varFallback : nullptr;
                        }
                        if (lookupToken(stringOf(name), resolved.prop, resolved.value)) {
                            m_varScratch.push_back(resolved);
                            e.decl = nullptr;
                            e.runtime = &m_varScratch.back();
                        } else if (irFallback != kNoVarFallback && irFallback < fallbacks.size()) {
                            e.decl = &fallbacks[irFallback];
                            e.runtime = nullptr;
                        } else if (runtimeFallback != nullptr) {
                            resolved.value = *runtimeFallback;
                            m_varScratch.push_back(resolved);
                            e.decl = nullptr;
                            e.runtime = &m_varScratch.back();
                        } else {
                            e.decl = nullptr;
                            e.runtime = nullptr;
                        }
                    }
                    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                                 [](const Entry& e) {
                                                     return e.decl == nullptr && e.runtime == nullptr;
                                                 }),
                                  entries.end());
                }
            }
            // T-218: scene-target declarations (their var()/calc() already resolved above) leave the
            // element's own cascade; they are written to the <svg> nodes after step 5.
            if (anyScene) {
                for (const Entry& e : entries) {
                    if (e.scene != nullptr) {
                        sceneEntries.push_back(e);
                    }
                }
                entries.erase(std::remove_if(entries.begin(), entries.end(),
                                             [](const Entry& e) { return e.scene != nullptr; }),
                              entries.end());
                std::stable_sort(sceneEntries.begin(), sceneEntries.end(), cascadeLess);
            }
            std::stable_sort(entries.begin(), entries.end(), cascadeLess);

            // 3. Start from inherited/initial values.
            NodeId parentNode = arena.parent(node);
            const ComputedStyle* parentStyle = nullptr;
            if (parentNode != kInvalidNode && arena.kind(parentNode) == NodeKind::Element) {
                parentStyle = &arena.element(parentNode).style;
            }

            ComputedStyle next{};
            style_decode::initNonAnimatableDefaults(next);
            initComputedStyleDefaults(next);  // base[64..105] initial values (xt/property.h).
            for (PropId p : kGeneralAnimatableProps) {
                next.base[p] = style_decode::initialGeneralValue(p);
            }
            Length originX{50, LengthUnit::Percent};
            Length originY{50, LengthUnit::Percent};

            if (parentStyle != nullptr) {
                next.visibility = parentStyle->visibility;
                next.pointerEvents = parentStyle->pointerEvents;
                next.pointerWatch = parentStyle->pointerWatch;  // T-174: inherited (27.2.5).
                next.focusNav = parentStyle->focusNav;          // T-194: inherited (08.9).
                next.fontFamilyOffset = parentStyle->fontFamilyOffset;
                next.fontFamilyCount = parentStyle->fontFamilyCount;
                next.fontWeight = parentStyle->fontWeight;
                next.fontStyle = parentStyle->fontStyle;
                next.textAlign = parentStyle->textAlign;
                next.whiteSpace = parentStyle->whiteSpace;
                next.textTransform = parentStyle->textTransform;            // T-323: inherited (05 §5.9.8).
                next.imageRendering = parentStyle->imageRendering;          // T-327: inherited, as in CSS.
                next.fontVariantNumeric = parentStyle->fontVariantNumeric;  // T-331: inherited.
                next.cursor = parentStyle->cursor;
                next.direction = parentStyle->direction;
                next.writingMode = parentStyle->writingMode;
                next.textOrientation = parentStyle->textOrientation;
                next.appRegion = parentStyle->appRegion;  // T-376: inherited (05 §5.9.10).
                next.base[prop::FontSize] = parentStyle->base[prop::FontSize];
                next.base[prop::LineHeight] = parentStyle->base[prop::LineHeight];
                next.base[prop::LetterSpacing] = parentStyle->base[prop::LetterSpacing];
                next.base[prop::Color] = parentStyle->base[prop::Color];
                next.textEffectOffset = parentStyle->textEffectOffset;  // T-178: inherited.
                next.textEffectCount = parentStyle->textEffectCount;
            }
            // `lang`: HTML attribute, inherited when absent (05.4; not a CSS declaration).
            next.lang = parentStyle != nullptr ? parentStyle->lang : kInvalidString;
            for (const auto& attr : data.attributes) {
                if (stringOf(attr.first) == "lang") {  // T-270a: also a setAttribute()-interned name.
                    next.lang = attr.second;
                    break;
                }
            }

            // 3b. T-345 (05 §5.9.9): `ui-scale` and `media-override` (both inherited) before any
            // length: the element's factor `pxScale` multiplies every px / em / rem length below.
            if (parentStyle != nullptr) {
                next.uiScaleMode = parentStyle->uiScaleMode;
                next.uiScaleMin = parentStyle->uiScaleMin;
                next.uiScaleMax = parentStyle->uiScaleMax;
                next.mediaOverride = parentStyle->mediaOverride;
            }
            for (const Entry& e : entries) {
                const uint16_t p = e.decl != nullptr ? e.decl->prop : e.runtime->prop;
                if (p == style_decode::styleprop::UiScale) {
                    if (e.decl != nullptr) {
                        applyUiScaleDecl(next, irValueF32(e.decl->value, 0), irValueF32(e.decl->value, 1),
                                         irValueF32(e.decl->value, 2));
                    } else if (e.runtime->value.type == ValueType::Color) {
                        const Color& c = e.runtime->value.c;
                        applyUiScaleDecl(next, c.r, c.g, c.b);
                    }
                } else if (p == style_decode::styleprop::MediaOverride) {
                    const uint32_t v = e.decl != nullptr ? irValueU32(e.decl->value, 0)
                                       : e.runtime->value.type == ValueType::Enum ? e.runtime->value.e
                                                                                  : next.mediaOverride;
                    next.mediaOverride =
                        static_cast<uint8_t>(v <= kMediaOverrideHide ? v : next.mediaOverride);
                }
            }
            const float pxScale =
                effectiveUiScale(next.uiScaleMode, next.uiScaleMin, next.uiScaleMax, m_uiScale, m_dpr);
            next.uiScaleEff = pxScale;
            m_calcScale = pxScale;
            m_calcAuto = next.uiScaleMode == kUiScaleAuto;
            const float parentScale =
                parentStyle != nullptr && parentStyle->uiScaleEff > 0.0f ? parentStyle->uiScaleEff : 1.0f;
            if (parentStyle != nullptr && parentScale != 1.0f) {
                // The inherited px lengths back to design px (scaleStylePx() applies pxScale).
                for (PropId p : {prop::LineHeight, prop::LetterSpacing}) {
                    if (next.base[p].type == ValueType::Length && next.base[p].len.unit == LengthUnit::Px) {
                        next.base[p].len.value /= parentScale;
                    }
                }
            }

            // 4a. font-size first (em is resolved against the parent's font-size for this one prop).
            // T-345: in this element's scale (the parent's px font size times pxScale / its own).
            float parentFontSizePx = parentStyle != nullptr
                                         ? parentStyle->base[prop::FontSize].len.value / parentScale * pxScale
                                         : 16.0f * pxScale;
            float fontSizeSelfPx = parentFontSizePx;
            // T-335: the root's own `rem` font size is relative to the initial 16px (CSS Values 4).
            if (parentStyle == nullptr) {
                m_rootFontPx = 16.0f;
            }
            for (const Entry& e : entries) {
                Length len{};
                if (e.decl != nullptr) {
                    if (e.decl->prop != prop::FontSize) {
                        continue;
                    }
                    len = Length{irValueF32(e.decl->value, 0), static_cast<LengthUnit>(e.decl->value[4])};
                } else {
                    if (e.runtime->prop != prop::FontSize || e.runtime->value.type != ValueType::Length) {
                        continue;
                    }
                    len = e.runtime->value.len;
                }
                if (len.unit == LengthUnit::Rem) {  // T-335.
                    len = Length{len.value * m_rootFontPx, LengthUnit::Px};
                }
                // T-176: `font-size` is px after this pass, so a device unit or a `calc()` (whose
                // `em` is the parent's font size) is resolved here and re-resolved on a device
                // change (markDeviceUsersDirty()).
                if (isDeviceUnit(len.unit)) {
                    m_deviceUsers[node] = 1;
                }
                if (len.unit == LengthUnit::Calc) {
                    bool usedToken = false;
                    // T-345: the program's own terms carry pxScale, `em` is the unscaled parent size.
                    const bool ok = instantiateCalc(calcProgramIndex(len.value), parentFontSizePx / pxScale,
                                                    len, usedToken);
                    if (usedToken) {
                        m_tokenUsers[node] = 1;
                    }
                    if (!ok) {
                        continue;
                    }
                }
                float px;
                if (style_decode::lengthToPx(len, parentFontSizePx, px)) {
                    // T-345: a px (or rem, now px) size is design px; em already is in this scale.
                    fontSizeSelfPx = len.unit == LengthUnit::Px ? px * pxScale : px;
                }
            }
            next.base[prop::FontSize] = Value::ofLength(fontSizeSelfPx, LengthUnit::Px);
            // T-345: em resolves against the unscaled size; scaleStylePx() then applies pxScale.
            const float fontSizeDesignPx = fontSizeSelfPx / pxScale;
            if (parentStyle == nullptr) {
                m_rootFontPx = fontSizeDesignPx;  // T-335: what the whole tree's `rem` multiplies.
                // T-335: a new root font size reaches every `rem` user: the whole tree re-resolves
                // (marked while the root is still dirty, so the ancestor walk stops right here).
                if (data.styleResolvedOnce != 0 &&
                    data.style.base[prop::FontSize].len.value != fontSizeSelfPx) {
                    arena.traversePreorder(node, [&arena](NodeId n) {
                        arena.markDirty(n, static_cast<uint8_t>(kDirtyStyle));
                        return true;
                    });
                }
            }

            // 4b. Everything else (em now resolves against fontSizeSelfPx, rem against the root's).
            // T-345: both unscaled (fontSizeDesignPx); scaleStylePx() applies pxScale afterwards.
            for (const Entry& entry : entries) {
                // T-345 (05 §5.9.9): under `media-override: show | hide` an `@media` rule never
                // switches `display` (hide is applied below).
                if (entry.fromMedia && next.mediaOverride != kMediaOverrideAuto &&
                    (entry.decl != nullptr ? entry.decl->prop : entry.runtime->prop) ==
                        style_decode::styleprop::Display) {
                    continue;
                }
                IrDecl remDecl{};
                RuntimeStyleDecl remRuntime;
                Entry e = entry;
                e.decl = resolveRemDecl(entry.decl, m_rootFontPx, remDecl);
                e.runtime = resolveRemDecl(entry.runtime, m_rootFontPx, remRuntime);
                if (e.decl != nullptr) {
                    if (e.decl->prop == prop::FontSize) {
                        continue;
                    }
                    applyGeneralDecl(ir, next, originX, originY, *e.decl, fontSizeDesignPx, pxScale);
                } else {
                    if (e.runtime->prop == prop::FontSize) {
                        continue;
                    }
                    if (e.runtime->value.type == ValueType::Length &&
                        e.runtime->value.len.unit == LengthUnit::Calc) {
                        // T-176: substitute the program's `em` (own font size) and `var()` terms.
                        RuntimeStyleDecl inst = *e.runtime;
                        bool usedToken = false;
                        const bool ok = instantiateCalc(calcProgramIndex(e.runtime->value.len.value),
                                                        fontSizeDesignPx, inst.value.len, usedToken);
                        if (usedToken) {
                            m_tokenUsers[node] = 1;
                        }
                        if (ok) {
                            applyRuntimeDecl(ir, matchCtx, next, originX, originY, inst, fontSizeDesignPx,
                                             pxScale);
                        }
                        continue;
                    }
                    if (e.runtime->value.type == ValueType::Length &&
                        isDeviceUnit(e.runtime->value.len.unit) &&
                        (e.runtime->prop == prop::TranslateX || e.runtime->prop == prop::TranslateY)) {
                        m_deviceUsers[node] = 1;
                    }
                    applyRuntimeDecl(ir, matchCtx, next, originX, originY, *e.runtime, fontSizeDesignPx,
                                     pxScale);
                }
            }
            if (next.mediaOverride == kMediaOverrideHide) {
                next.display = Display::None;  // T-345: `media-override: hide`.
            }
            // T-325 (05 §5.9.8): `outline-color`'s initial value is the element's `color`; the
            // cascade has no `currentcolor`, so an element no rule gives an `outline-color` takes
            // its final `color` here.
            {
                bool outlineColorDeclared = false;
                for (const Entry& e : entries) {
                    const uint16_t p = e.decl != nullptr ? e.decl->prop : e.runtime->prop;
                    outlineColorDeclared = outlineColorDeclared || p == prop::OutlineColor;
                }
                if (!outlineColorDeclared) {
                    next.base[prop::OutlineColor] = next.base[prop::Color];
                }
            }
            // T-333 (05 §5.9.8): a side color is the side's own only when declared after the last
            // `border-color` (the one-value form writes the legacy BorderColor, which, like CSS's
            // shorthand, overrides earlier side declarations); every other side follows
            // BorderColor, copied in so getProperty() and transitions see the color drawn.
            {
                uint8_t sides = 0;
                for (const Entry& e : entries) {
                    const uint16_t p = e.decl != nullptr ? e.decl->prop : e.runtime->prop;
                    if (p == prop::BorderColor) {
                        sides = 0;
                    } else if (p >= prop::BorderTopColor && p <= prop::BorderLeftColor) {
                        sides = static_cast<uint8_t>(sides | (1u << (p - prop::BorderTopColor)));
                    }
                }
                next.borderColorSides = sides;
                for (PropId p = prop::BorderTopColor; p <= prop::BorderLeftColor; ++p) {
                    if ((sides & (1u << (p - prop::BorderTopColor))) == 0) {
                        next.base[p] = next.base[prop::BorderColor];
                    }
                }
            }
            // T-324 (05 §5.9.8): `text-decoration-color` defaults to `color` the same way.
            // `text-decoration-line` is not inherited, but a decoration is drawn across all
            // descendant text: pass the ancestors' set down with the nearest decorating
            // ancestor's color and thickness.
            {
                bool decorationColorDeclared = false;
                for (const Entry& e : entries) {
                    const uint16_t p = e.decl != nullptr ? e.decl->prop : e.runtime->prop;
                    decorationColorDeclared = decorationColorDeclared || p == prop::TextDecorationColor;
                }
                if (!decorationColorDeclared) {
                    next.base[prop::TextDecorationColor] = next.base[prop::Color];
                }
                if (parentStyle != nullptr && parentStyle->textDecorations != 0) {
                    next.inheritedDecorations = parentStyle->textDecorations;
                    // T-345: the parent's thickness back to design px (scaleStylePx() rescales it).
                    if (parentStyle->textDecorationLine != 0) {
                        next.inheritedDecorationColor = parentStyle->base[prop::TextDecorationColor].c;
                        const Length& t = parentStyle->base[prop::TextDecorationThickness].len;
                        next.inheritedDecorationThickness =
                            t.unit == LengthUnit::Px ? t.value / parentScale : -1.0f;
                    } else {
                        next.inheritedDecorationColor = parentStyle->inheritedDecorationColor;
                        next.inheritedDecorationThickness =
                            parentStyle->inheritedDecorationThickness >= 0.0f
                                ? parentStyle->inheritedDecorationThickness / parentScale
                                : -1.0f;
                    }
                }
                next.textDecorations =
                    static_cast<uint8_t>(next.textDecorationLine | next.inheritedDecorations);
            }
            // T-345 (05 §5.9.9): the px lengths in this element's scale.
            if (pxScale != 1.0f) {
                scaleStylePx(next, originX, originY, pxScale);
            }
            // 08.8 (T-35 "附帶修正"): an `input` never lays its text out vertically; a
            // `writing-mode` reaching one -- inherited or its own -- is ignored. The load-time
            // warning for the declared case is in document_load.cpp.
            if (next.writingMode != WritingMode::HorizontalTb && stringOf(data.tag) == "input") {
                next.writingMode = WritingMode::HorizontalTb;
            }

            // 5. Write base values, registering transitions for changed, interpolable props.
            bool firstResolve = data.styleResolvedOnce == 0;
            // T-345: a new `ui-scale` factor (a resized output, a new `ui-scale`) rescales every
            // length at once; nothing transitions to the rescaled values.
            const bool noTransition = firstResolve || data.style.uiScaleEff != pxScale;
            for (PropId p : kGeneralAnimatableProps) {
                if (!valueEquals(next.base[p], data.style.base[p])) {
                    Value fromFinal = getProperty(arena, node, p);
                    Value toFinal = next.base[p];
                    if (p == prop::TranslateX || p == prop::TranslateY) {
                        translateTransitionEnds(data, p, fromFinal, toFinal);
                    }
                    setProperty(arena, node, p, next.base[p]);
                    maybeRegisterTransition(animator, ir, next, node, p, fromFinal, toFinal, clock,
                                            noTransition);
                }
            }
            // T-228: auto-orient is a discrete Enum -- never transitioned.
            if (!valueEquals(next.base[prop::AutoOrient], data.style.base[prop::AutoOrient])) {
                setProperty(arena, node, prop::AutoOrient, next.base[prop::AutoOrient]);
            }
            for (PropId p = prop::Width; p <= prop::LetterSpacing; ++p) {
                if (!valueEquals(next.base[p], data.style.base[p])) {
                    Value fromFinal = getProperty(arena, node, p);
                    setProperty(arena, node, p, next.base[p]);
                    maybeRegisterTransition(animator, ir, next, node, p, fromFinal, next.base[p], clock,
                                            noTransition);
                }
            }
            // ScrollX/ScrollY are runtime state written by the wheel handler (T-32), never by the
            // cascade: they are carried over untouched so an unrelated re-resolve (e.g. a hover
            // passing through the container) does not reset the scroll position.
            next.base[prop::ScrollX] = data.style.base[prop::ScrollX];
            next.base[prop::ScrollY] = data.style.base[prop::ScrollY];
            for (PropId p = prop::BackgroundColor; p < prop::ScrollX; ++p) {
                if (!valueEquals(next.base[p], data.style.base[p])) {
                    Value fromFinal = getProperty(arena, node, p);
                    setProperty(arena, node, p, next.base[p]);
                    maybeRegisterTransition(animator, ir, next, node, p, fromFinal, next.base[p], clock,
                                            noTransition);
                }
            }
            // §19.4/19.10 (T-47a): perspective/perspective-origin, resolved to px by layout below.
            for (PropId p = prop::Perspective; p <= prop::PerspectiveOriginY; ++p) {
                if (!valueEquals(next.base[p], data.style.base[p])) {
                    Value fromFinal = getProperty(arena, node, p);
                    setProperty(arena, node, p, next.base[p]);
                    maybeRegisterTransition(animator, ir, next, node, p, fromFinal, next.base[p], clock,
                                            noTransition);
                }
            }
            // T-178 (07 §7.8): `text-reveal`, paint-only.
            if (!valueEquals(next.base[prop::TextReveal], data.style.base[prop::TextReveal])) {
                Value fromFinal = getProperty(arena, node, prop::TextReveal);
                setProperty(arena, node, prop::TextReveal, next.base[prop::TextReveal]);
                maybeRegisterTransition(animator, ir, next, node, prop::TextReveal, fromFinal,
                                        next.base[prop::TextReveal], clock, noTransition);
            }
            // T-202 (03 §3.12.5): the particle fields, paint-only like text-reveal; T-325 (05
            // §5.9.8): the outline width / color / offset right after them, paint-only too; T-324:
            // then the text-decoration color / thickness; T-327: then background size / position;
            // T-328: `aspect-ratio` (layout, through setProperty's kDirtyLayout); T-333: the
            // per-side border colors (paint-only).
            for (PropId p = prop::ParticleRate; p <= prop::BorderLeftColor; ++p) {
                if (!valueEquals(next.base[p], data.style.base[p])) {
                    Value fromFinal = getProperty(arena, node, p);
                    setProperty(arena, node, p, next.base[p]);
                    maybeRegisterTransition(animator, ir, next, node, p, fromFinal, next.base[p], clock,
                                            noTransition);
                }
            }
            // T-336 (05 §5.9.8): `order` is not animatable -- committed without a transition (layout
            // through setProperty's kDirtyLayout).
            if (!valueEquals(next.base[prop::Order], data.style.base[prop::Order])) {
                setProperty(arena, node, prop::Order, next.base[prop::Order]);
            }
            if (originX.value != data.originX.value || originX.unit != data.originX.unit ||
                originY.value != data.originY.value || originY.unit != data.originY.unit) {
                data.originX = originX;
                data.originY = originY;
                arena.markDirty(node, static_cast<uint8_t>(kDirtyTransform));
            }

            // 5b. Font matching (docs/tasks/T-30.md goal 1): resolvedFont =
            // shaper->match({font-family list, font-weight, font-style}); no shaper -> kInvalidFont
            // (Text nodes then lay out as 0-size, docs/design/07-text.md §7.2).
            next.resolvedFont = kInvalidFont;
            if (m_shaper != nullptr) {
                m_familyScratch.clear();
                std::span<const uint32_t> familyPool = ir.style().fontFamilies();
                for (uint32_t i = 0; i < next.fontFamilyCount; ++i) {
                    uint32_t index = next.fontFamilyOffset + i;
                    if (index < familyPool.size()) {
                        m_familyScratch.push_back(stringOf(familyPool[index]));  // T-270d.
                    }
                }
                FontRequest request;
                request.families = std::span<const std::string_view>(m_familyScratch);
                request.weight = next.fontWeight;
                request.italic = next.fontStyle == FontStyle::Italic;
                next.resolvedFont = m_shaper->match(request);
                // T-324 (07 §7.2): the decoration metrics the Painter's text branch reads (for the
                // element's own, inherited and rich-text run decorations alike).
                if (next.resolvedFont != kInvalidFont) {
                    const FontMetrics m = m_shaper->metrics(next.resolvedFont);
                    next.decoAscent = m.ascent;
                    next.decoDescent = m.descent;
                    next.decoUnderlineOffset = m.underlineOffset;
                    next.decoUnderlineThickness = m.underlineThickness;
                }
            }
            if (next.resolvedFont != data.style.resolvedFont) {
                arena.markDirty(node, static_cast<uint8_t>(kDirtyLayout));
            }

            // 5c. T-218 (31 31.5.1): scene-target rules -> the element's inline <svg> nodes, with
            // transitions from this cascade's `transition` list. Timeline tracks on the same
            // nodes are evaluated after the style pass, so an animation wins (04 4.7).
            if (!sceneEntries.empty() || !m_sceneOverrides.empty()) {
                m_sceneSet.clear();
                for (const Entry& entry : sceneEntries) {
                    IrDecl remDecl{};
                    RuntimeStyleDecl remRuntime;
                    Entry e = entry;
                    e.decl = resolveRemDecl(entry.decl, m_rootFontPx, remDecl);  // T-335.
                    e.runtime = resolveRemDecl(entry.runtime, m_rootFontPx, remRuntime);
                    const PropId p = e.decl != nullptr ? e.decl->prop : e.runtime->prop;
                    SceneOverrideDecl sd;
                    if (!decodeSceneValue(e.decl, e.runtime, p, fontSizeSelfPx, node, sd.value)) {
                        continue;
                    }
                    sd.targetKind = e.scene->classCount;
                    sd.target = e.scene->tag;
                    sd.prop = p;
                    sd.specificity = e.specificity;
                    sd.order = e.order;
                    sd.important = e.important;
                    m_sceneSet.push_back(sd);
                }
                applySceneOverrides(arena, node, data, next, clock, firstResolve);
            }

            // 6. animation diff, non-animatable dirty bits.
            applyAnimationDiff(animator, ir, m_runtimeAnimations, data, next, node, clock, onAnimationPlay);
            markNonAnimatableDirty(arena, node, data.style, next);

            // 6b. `effect` cascade (18-effects.md §18.4/T-51b): assigns the node's actual effect
            // stack from `next.effectOffset/effectCount`, run before commit per the design's
            // cascade paragraph.
            applyEffectCascade(animator, ir, next, arena, node, m_runtimeEffects, m_runtimeEffectStops, clock,
                               noTransition);

            // 6c. `background-gradient` (05.5c/T-115): decode the winning declaration into
            // ElementData, where Painter reads it (core has no IR access below the cascade).
            resolveBackgroundGradient(ir, m_runtimeGradients, next, data.backgroundGradient);
            // T-332 (05 §5.9.8): likewise the `clip-path` shape, read by the Painter, the hit test
            // and the paint-bounds code.
            resolveClipShape(ir, m_runtimeClipShapes, next, data.clipShape);
            // T-338 (06 §6.15): a grid container's template, read by layout_grid.cpp.
            if (next.display == Display::Grid || !data.grid.columns.empty() || !data.grid.rows.empty() ||
                !data.grid.areas.empty() || data.grid.areaRows != 0) {
                resolveGrid(ir, next, data.grid);
            }

            // 6d. T-199 (26.3.4): atlas cells of `background-image` / `border-image-source`, and
            // the nine-slice margins in source pixels (percentages against the source size,
            // `auto` from the resolver's manifest slice), so the Painter needs no resolver.
            resolveImageMeta(ir, next, data);

            // 7. Commit.
            data.style = next;
            data.styleResolvedOnce = 1;
            arena.clearDirtyBits(node, static_cast<uint8_t>(kDirtyStyle));

            // §19.3: Element is3D/backfaceHidden runtime derivation (docs/tasks/T-43c.md).
            {
                NodeCommon& common = arena.common(node);
                bool was = common.is3D != 0;
                const Value& orientation = next.base[prop::Orientation];
                bool now3D = next.base[prop::TranslateZ].f != 0.0f || next.base[prop::AnchorZ].f != 0.0f ||
                             next.base[prop::RotationX].f != 0.0f || next.base[prop::RotationY].f != 0.0f ||
                             orientation.v3.x != 0.0f || orientation.v3.y != 0.0f || orientation.v3.z != 0.0f;
                common.is3D = now3D ? 1 : 0;
                common.backfaceHidden = next.backfaceHidden;
                common.flags2 = next.pointerWatch != 0
                                    ? (common.flags2 | kPointerWatch)
                                    : (common.flags2 & ~static_cast<uint32_t>(kPointerWatch));
                common.flags2 = next.layerOverlay != 0  // T-194 (08.9).
                                    ? (common.flags2 | kLayerOverlay)
                                    : (common.flags2 & ~static_cast<uint32_t>(kLayerOverlay));
                // T-326 (18 §18.12): `mix-blend-mode` lands in the same NodeCommon::blend a scene
                // node carries, so Painter::enterNode's PushLayer(opacity, blend) rule applies.
                common.blend = next.mixBlendMode;
                if (was != now3D) {
                    arena.markDirty(node, static_cast<uint8_t>(kDirtyTransform | kDirtyTree));
                    arena.markDirtyAncestors(node, static_cast<uint8_t>(kDirtyTree),
                                             nearestSpaceRootOrDocRoot(arena, node));
                }
            }
        }
    }

    return processed;
}

// ---------------------------------------------------------------------------------------------
// T-218: scene-target rules (docs/design/31-vector-authoring.md §31.5.1).
// ---------------------------------------------------------------------------------------------

namespace {

/// The IR value of paint prop `prop` (33-51) of an IrPathData / IrShapeData (T-225: same field
/// names). False for a property the snapshot does not cover.
template <class PaintData>
bool irPaintBaseValue(const PaintData& pd, std::span<const float> floatPool, PropId prop,
                      SceneOverrideBase& out) {
    switch (prop) {
        case prop::FillColor:
            out.value = Value::ofColor(pd.fillColor);
            return true;
        case prop::StrokeColor:
            out.value = Value::ofColor(pd.strokeColor);
            return true;
        case prop::FillOpacity:
        case prop::StrokeOpacity:
            out.value = Value::ofFloat(1.0f);  // IrPathData has none; instantiateScene's default.
            return true;
        case prop::StrokeWidth:
            out.value = Value::ofFloat(pd.strokeWidth);
            return true;
        case prop::TrimStart:
            out.value = Value::ofFloat(pd.trimStart);
            return true;
        case prop::TrimEnd:
            out.value = Value::ofFloat(pd.trimEnd);
            return true;
        case prop::TrimOffset:
            out.value = Value::ofFloat(pd.trimOffset);
            return true;
        case prop::DashOffset:
            out.value = Value::ofFloat(pd.dashPhase);
            return true;
        case prop::StrokeTaperStart:  // T-233
            out.value = Value::ofFloat(pd.taperStart);
            return true;
        case prop::StrokeTaperEnd:
            out.value = Value::ofFloat(pd.taperEnd);
            return true;
        case prop::DashArray:  // T-236: the scene's floatPool outlives the override state.
            if (uint64_t{pd.dashOffset} + pd.dashCount > floatPool.size()) {
                return false;
            }
            out.value = Value::ofFloatArray(floatPool.data() + pd.dashOffset, pd.dashCount);
            return true;
        default:
            return false;
    }
}

/// The IR value of `prop` on local scene node `local` (what instantiateScene gave the node). False
/// for a property the snapshot does not cover.
bool irSceneBaseValue(const SceneChunk& scene, uint32_t local, PropId prop, SceneOverrideBase& out) {
    const IrSceneNode& sn = scene.nodes()[local];
    switch (prop) {
        case prop::Opacity:
            out.value = Value::ofFloat(sn.opacity);
            return true;
        case prop::AnchorX:
            out.value = Value::ofFloat(sn.anchor.x);
            return true;
        case prop::AnchorY:
            out.value = Value::ofFloat(sn.anchor.y);
            return true;
        case prop::TranslateX:
            out.value = Value::ofFloat(sn.translate.x);
            return true;
        case prop::TranslateY:
            out.value = Value::ofFloat(sn.translate.y);
            return true;
        case prop::ScaleX:
            out.value = Value::ofFloat(sn.scale.x);
            return true;
        case prop::ScaleY:
            out.value = Value::ofFloat(sn.scale.y);
            return true;
        case prop::Rotation:
            out.value = Value::ofFloat(sn.rotation);
            return true;
        case prop::SkewX:
            out.value = Value::ofFloat(sn.skew.x);
            return true;
        case prop::SkewY:
            out.value = Value::ofFloat(sn.skew.y);
            return true;
        default:
            break;
    }
    if (sn.kind == static_cast<uint8_t>(NodeKind::Modifier) && sn.kindIndex < scene.modifiers().size()) {
        // T-225: a modifier parameter.
        if (prop < prop::ModTrimStart || prop > prop::ModEnabled) {
            return false;
        }
        const IrModifierData& md = scene.modifiers()[sn.kindIndex];
        ModifierNodeData m;
        m.kind = static_cast<ModifierKind>(md.kind);
        m.mode = md.mode;
        m.enabled = md.enabled;
        for (int i = 0; i < 8; ++i) {
            m.params[i] = md.params[i];
        }
        m.opacityA = md.opacityA;
        m.opacityB = md.opacityB;
        out.value = modifierPropertyValue(m, prop);
        return true;
    }
    if (sn.kind == static_cast<uint8_t>(NodeKind::Bone) && sn.kindIndex < scene.bones().size()) {
        // T-243 (32 §32.2.1): a bone's length.
        if (prop != prop::BoneLength) {
            return false;
        }
        out.value = Value::ofFloat(scene.bones()[sn.kindIndex].length);
        return true;
    }
    if (sn.kind == static_cast<uint8_t>(NodeKind::Constraint) && sn.kindIndex < scene.constraints().size()) {
        // T-243 (32 §32.2.3): a constraint parameter.
        if (prop < prop::IkStrength || prop > prop::ConstraintEnabled) {
            return false;
        }
        const IrConstraintData& cd = scene.constraints()[sn.kindIndex];
        ConstraintNodeData c;
        c.kind = static_cast<ConstraintKind>(cd.kind);
        c.space = cd.space;
        c.enabled = cd.enabled;
        for (int i = 0; i < 8; ++i) {
            c.params[i] = cd.params[i];
        }
        c.mix = cd.mix;
        out.value = Value::ofFloat(constraintPropertyValue(c, prop));
        return true;
    }
    if (sn.kind == static_cast<uint8_t>(NodeKind::Shape) && sn.kindIndex < scene.shapes().size()) {
        // T-225: a shape parameter or one of its paint props.
        const IrShapeData& sd = scene.shapes()[sn.kindIndex];
        if (prop >= prop::ShapeX && prop <= prop::ShapeRound2) {
            out.value = Value::ofFloat(shapeParamValue(static_cast<ShapeKind>(sd.kind), sd.params, prop));
            return true;
        }
        return irPaintBaseValue(sd, scene.floatPool(), prop, out);
    }
    if (sn.kind != static_cast<uint8_t>(NodeKind::Path) || sn.kindIndex >= scene.paths().size()) {
        return false;
    }
    const IrPathData& pd = scene.paths()[sn.kindIndex];
    if (prop == prop::PathData) {
        std::span<const uint8_t> verbs = scene.verbPool().subspan(pd.verbOffset, pd.verbCount);
        out.verbs.assign(verbs.begin(), verbs.end());
        out.value = Value::ofPathWithVerbs(scene.floatPool().data() + pd.pointOffset, pd.pointCount,
                                           out.verbs.data(), pd.verbCount);
        return true;
    }
    return irPaintBaseValue(pd, scene.floatPool(), prop, out);
}

}  // namespace

bool StyleEngine::decodeSceneValue(const IrDecl* decl, const RuntimeStyleDecl* runtime, PropId prop,
                                   float fontPx, NodeId element, Value& out) {
    if (!propertyDefined(prop)) {
        return false;
    }
    const ValueType want = propertyType(prop);
    Value v;
    if (decl != nullptr) {
        switch (static_cast<ValueType>(decl->valueType)) {
            case ValueType::Float:
                v = Value::ofFloat(irValueF32(decl->value, 0));
                break;
            case ValueType::Color:
                v = Value::ofColor(decodeColor(*decl));
                break;
            case ValueType::Length:
                v = Value::ofLength(irValueF32(decl->value, 0), static_cast<LengthUnit>(decl->value[4]));
                break;
            case ValueType::Vec2:  // T-236: repeat-anchor / repeat-translate / center.
                v = Value::ofVec2(Vec2{irValueF32(decl->value, 0), irValueF32(decl->value, 1)});
                break;
            case ValueType::ListRef: {
                std::span<const float> pool = m_ir->style().floatPool();
                if (prop == prop::DashArray && want == ValueType::FloatArray) {
                    // T-236: `stroke-dasharray` (count 0 = none).
                    const uint32_t off = irValueU32(decl->value, 0), cnt = irValueU32(decl->value, 1);
                    if (uint64_t{off} + cnt > pool.size()) {
                        return false;  // IrView::open validated this; defensive.
                    }
                    out = Value::ofFloatArray(pool.data() + off, cnt);
                    return true;
                }
                if (prop != prop::PathData || want != ValueType::Path) {
                    return false;
                }
                const uint32_t po = irValueU32(decl->value, 0), pc = irValueU32(decl->value, 1);
                const uint32_t vo = irValueU32(decl->value, 2), vc = irValueU32(decl->value, 3);
                if (uint64_t{po} + pc > pool.size() || uint64_t{vo} + vc > pool.size()) {
                    return false;  // IrView::open validated this; defensive.
                }
                out = Value::ofPathWithVerbs(pool.data() + po, pc, pool.data() + vo, vc);
                return true;
            }
            default:
                return false;
        }
    } else if (runtime != nullptr) {
        v = runtime->value;
    } else {
        return false;
    }
    if (v.type == ValueType::Length) {
        Length len = v.len;
        if (len.unit == LengthUnit::Calc) {
            bool usedToken = false;
            if (!instantiateCalc(calcProgramIndex(len.value), fontPx, len, usedToken)) {
                return false;
            }
            if (usedToken) {
                m_tokenUsers[element] = 1;
            }
            m_deviceUsers[element] = 1;  // A calc() may hold device units; cheap to re-resolve.
        } else if (isDeviceUnit(len.unit)) {
            m_deviceUsers[element] = 1;
        }
        float px = 0;
        if (!style_decode::lengthToPx(len, fontPx, px)) {
            return false;  // `%` / `auto` mean nothing on an <svg> node.
        }
        v = Value::ofFloat(px);
    }
    if (v.type != want) {
        return false;
    }
    out = v;
    return true;
}

void StyleEngine::applySceneOverrides(NodeArena& arena, NodeId element, const ElementData& data,
                                      const ComputedStyle& next, float clock, bool firstResolve) {
    const SceneInstance* inst = nullptr;
    if (m_sceneInstances != nullptr && data.scene != kInvalidScene &&
        data.sceneInstanceIndex < m_sceneInstances->size()) {
        inst = &(*m_sceneInstances)[data.sceneInstanceIndex];
        if (inst->root == kInvalidNode || inst->scene != data.scene) {
            inst = nullptr;
        }
    }
    auto it = m_sceneOverrides.find(element);
    if (it != m_sceneOverrides.end() &&
        (inst == nullptr || it->second.sceneRoot != inst->root || it->second.scene != inst->scene)) {
        // The instance this state was written to is gone (element re-created, NodeId reused):
        // its nodes no longer exist, so there is nothing to restore.
        m_sceneOverrides.erase(it);
        it = m_sceneOverrides.end();
    }
    if (inst == nullptr || (m_sceneSet.empty() && it == m_sceneOverrides.end())) {
        return;
    }
    const SceneChunk& scene = m_ir->scene(inst->scene);
    std::span<const IrSceneNode> nodes = scene.nodes();
    const uint32_t count = static_cast<uint32_t>(std::min<size_t>(nodes.size(), inst->localToArena.size()));

    // Per (node, prop) winners: the set is in cascade order, so a later declaration replaces.
    m_sceneWinners.clear();
    for (const SceneOverrideDecl& d : m_sceneSet) {
        const auto kind = static_cast<IrSceneTarget>(d.targetKind);
        IrSvgTag wantTag = IrSvgTag::None;
        if (kind == IrSceneTarget::Tag) {
            wantTag = irSvgTagCode(stringOf(d.target));
            if (wantTag == IrSvgTag::None) {
                continue;
            }
        }
        for (uint32_t i = 1; i < count; ++i) {  // Local 0 is the scene root (the <svg> itself).
            const IrSceneNode& sn = nodes[i];
            const bool hit = kind == IrSceneTarget::Name  ? sn.name == d.target
                             : kind == IrSceneTarget::Tag ? sn.svgTag == static_cast<uint8_t>(wantTag)
                                                          : true;
            const NodeId n = inst->localToArena[i];
            if (!hit || n == kInvalidNode || !arena.alive(n) || !propertyApplies(arena.kind(n), d.prop)) {
                continue;
            }
            bool replaced = false;
            for (SceneOverrideWrite& w : m_sceneWinners) {
                if (w.node == n && w.prop == d.prop) {
                    w.value = d.value;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                m_sceneWinners.push_back(SceneOverrideWrite{n, d.prop, i, d.value});
            }
        }
    }
    if (it == m_sceneOverrides.end()) {
        if (m_sceneWinners.empty()) {
            return;
        }
        SceneOverrideState fresh;
        fresh.sceneRoot = inst->root;
        fresh.scene = inst->scene;
        it = m_sceneOverrides.emplace(element, std::move(fresh)).first;
    }
    SceneOverrideState& st = it->second;
    auto findBase = [&st](NodeId n, PropId p) -> SceneOverrideBase* {
        for (SceneOverrideBase& b : st.base) {
            if (b.node == n && b.prop == p) {
                return &b;
            }
        }
        return nullptr;
    };
    auto write = [&](NodeId n, PropId p, const Value& v) {
        const Value from = getProperty(arena, n, p);
        setProperty(arena, n, p, v);
        if (p != prop::PathData && p != prop::DashArray) {  // `d` and dash arrays jump (31 §31.5.1).
            maybeRegisterTransition(*m_animator, *m_ir, next, element, p, from, v, clock, firstResolve, n);
        }
    };

    // (node, prop) pairs no rule overrides any more go back to their IR value.
    for (const SceneOverrideWrite& prev : st.applied) {
        bool still = false;
        for (const SceneOverrideWrite& w : m_sceneWinners) {
            if (w.node == prev.node && w.prop == prev.prop) {
                still = true;
                break;
            }
        }
        SceneOverrideBase* base = still ? nullptr : findBase(prev.node, prev.prop);
        if (base != nullptr && arena.alive(prev.node)) {
            Value v = base->value;
            if (v.type == ValueType::Path) {
                v.pathVerbs = base->verbs.data();
            }
            write(prev.node, prev.prop, v);
        }
    }
    for (const SceneOverrideWrite& w : m_sceneWinners) {
        const SceneOverrideWrite* prev = nullptr;
        for (const SceneOverrideWrite& a : st.applied) {
            if (a.node == w.node && a.prop == w.prop) {
                prev = &a;
                break;
            }
        }
        if (prev != nullptr && valueEquals(prev->value, w.value)) {
            continue;
        }
        if (findBase(w.node, w.prop) == nullptr) {
            SceneOverrideBase base;
            base.node = w.node;
            base.prop = w.prop;
            if (irSceneBaseValue(scene, w.local, w.prop, base)) {
                st.base.push_back(std::move(base));
            }
        }
        write(w.node, w.prop, w.value);
    }
    st.applied = m_sceneWinners;
    if (st.applied.empty()) {
        m_sceneOverrides.erase(it);  // Everything restored; the snapshot is rebuilt when needed.
    }
}

}  // namespace xt
