#include <cstdio>
#include <cstring>
#include <type_traits>

#include "modifier_props.h"
#include "path_verbs.h"
#include "property_accept.h"
#include "xt/assert.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/property.h"
#include "xt/value.h"

/// Property table (03.9) and the (NodeId, PropId) read/write interface. See
/// Docs/design/03-scene-and-properties.md §3.9-3.11, Docs/tasks/T-05.md.

namespace xt {

namespace {

/// Decodes an effect-parameter PropId's effectIndex (Docs/design/18-effects.md §18.2). Uses 7
/// bits, not the 4 effectParam() encodes into, so a raw PropId built outside effectParam() with an
/// out-of-range index (e.g. 16) is preserved rather than silently wrapping back into 0-15 --
/// propertyDefined() relies on that to reject it (see test_property_access.cpp's "i = 16" case).
uint32_t rawEffectIndex(PropId prop) { return (prop >> 8) & 0x7Fu; }

/// Decodes an effect-parameter PropId's paramIndex (0-255; propertyDefined() checks it against
/// the actual defined set {0, 1..8, 16, 17, 18}).
uint32_t rawEffectParamIndex(PropId prop) { return prop & 0xFFu; }

/// Docs/design/18-effects.md §18.3's per-(kind, paramIndex) parameter name, or nullptr when kind
/// does not define paramIndex (the caller then falls back to the generic "param<p>" form).
const char* effectParamNameForKind(EffectKind kind, uint32_t paramIndex) {
    if (paramIndex == prop::kEffectParamEnabled) {
        return "enabled";
    }
    if (paramIndex == prop::kEffectParamColor0) {
        switch (kind) {
            case EffectKind::DropShadow:
            case EffectKind::Glow:
            case EffectKind::Shine:
            case EffectKind::Outline:
                return "color";
            default:
                return nullptr;  // 18.3: Blur/ColorAdjust/GradientMask/BackdropBlur define no colors.
        }
    }
    if (paramIndex == prop::kEffectParamOffset) {
        // 18.2: the composite Vec2 is specifically the DropShadow offset; params[0..1] mean
        // something else (radius/intensity, brightness/contrast, ...) for every other kind.
        return kind == EffectKind::DropShadow ? "offset" : nullptr;
    }
    if (paramIndex < 1 || paramIndex > 8) {
        return nullptr;  // paramIndex 17 (colors[1]): no kind defines a second color.
    }
    switch (kind) {
        case EffectKind::DropShadow: {
            static constexpr const char* kNames[] = {"offsetX", "offsetY", "blur",
                                                     "spread",  "opacity", "inset"};
            return paramIndex <= 6 ? kNames[paramIndex - 1] : nullptr;
        }
        case EffectKind::Glow: {
            static constexpr const char* kNames[] = {"radius", "intensity", "inner"};
            return paramIndex <= 3 ? kNames[paramIndex - 1] : nullptr;
        }
        case EffectKind::Blur: {
            static constexpr const char* kNames[] = {"radiusX", "radiusY"};
            return paramIndex <= 2 ? kNames[paramIndex - 1] : nullptr;
        }
        case EffectKind::ColorAdjust: {
            static constexpr const char* kNames[] = {"brightness", "contrast", "saturation", "hue",
                                                     "opacity",    "invert",   "sepia"};
            return paramIndex <= 7 ? kNames[paramIndex - 1] : nullptr;
        }
        case EffectKind::GradientMask: {
            static constexpr const char* kNames[] = {"p0x", "p0y", "p1x", "p1y", "radial", "invert"};
            return paramIndex <= 6 ? kNames[paramIndex - 1] : nullptr;
        }
        case EffectKind::Shine: {
            static constexpr const char* kNames[] = {"position", "width", "softness", "angle", "intensity"};
            return paramIndex <= 5 ? kNames[paramIndex - 1] : nullptr;
        }
        case EffectKind::Outline: {
            static constexpr const char* kNames[] = {"radius", "softness", "position"};
            return paramIndex <= 3 ? kNames[paramIndex - 1] : nullptr;
        }
        case EffectKind::BackdropBlur: {
            static constexpr const char* kNames[] = {"radius", "saturation", "brightness"};
            return paramIndex <= 3 ? kNames[paramIndex - 1] : nullptr;
        }
        default:
            return nullptr;  // None/Custom: no 18.3 table entry.
    }
}

/// T-225: propertyName() of PropId 208-231, indexed by prop - ModTrimStart.
constexpr const char* kModifierPropNames[] = {
    "ModTrimStart", "ModTrimEnd",  "ModTrimOffset",  "ModCopies",        "ModOffset",       "ModAnchor",
    "ModTranslate", "ModRotation", "ModScale",       "ModStartOpacity",  "ModEndOpacity",   "ModRadius",
    "ModAmount",    "ModSize",     "ModRidges",      "ModAngle",         "ModCenter",       "ModDetail",
    "ModSeed",      "ModSpeed",    "ModCorrelation", "ModTemporalPhase", "ModSpatialPhase", "ModEnabled",
};
static_assert(sizeof(kModifierPropNames) / sizeof(kModifierPropNames[0]) ==
              prop::ModEnabled - prop::ModTrimStart + 1);

/// T-225: propertyName() of PropId 232-245, indexed by prop - ShapeX.
constexpr const char* kShapePropNames[] = {
    "ShapeX",  "ShapeY", "ShapeW",  "ShapeH",      "ShapeRx",       "ShapeRy",    "ShapeCx",
    "ShapeCy", "ShapeR", "ShapeR2", "ShapePoints", "ShapeRotation", "ShapeRound", "ShapeRound2",
};
static_assert(sizeof(kShapePropNames) / sizeof(kShapePropNames[0]) == prop::ShapeRound2 - prop::ShapeX + 1);

/// T-243: propertyName() of PropId 246-255, indexed by prop - BoneLength.
constexpr const char* kRigPropNames[] = {
    "BoneLength",       "IkStrength",       "ConstraintMix",    "ConstraintParam0", "ConstraintParam1",
    "ConstraintParam2", "ConstraintParam3", "ConstraintParam4", "ConstraintParam5", "ConstraintEnabled",
};
static_assert(sizeof(kRigPropNames) / sizeof(kRigPropNames[0]) ==
              prop::ConstraintEnabled - prop::BoneLength + 1);

}  // namespace

bool propertyDefined(PropId prop) {
    if (isEffectProp(prop)) {
        if (rawEffectIndex(prop) > 15) {
            return false;
        }
        uint32_t p = rawEffectParamIndex(prop);
        return p == prop::kEffectParamEnabled || (p >= 1 && p <= 8) || p == prop::kEffectParamColor0 ||
               p == prop::kEffectParamColor1 || p == prop::kEffectParamOffset;
    }
    if (prop <= prop::AutoOrient) {
        return true;  // 0-14
    }
    if (prop >= prop::TranslateZ && prop <= prop::Orientation) {
        return true;  // 16-22
    }
    if (prop >= prop::CameraZoom && prop <= prop::CameraFar) {
        return true;  // 24-27
    }
    if (prop >= prop::PathData && prop <= prop::StrokeTaperEnd) {
        return true;  // 32-51 (T-233: 50-51 stroke taper)
    }
    if (prop >= prop::Width && prop <= prop::LetterSpacing) {
        return true;  // 64-93
    }
    if (prop >= prop::BackgroundColor && prop <= prop::Order) {
        // 96-142 (T-202: 110-128 particle fields; T-325: 129-131 outline; T-324: 132-133; T-327:
        // 134-137 background size / position; T-328: 138 aspect-ratio; T-333: 139-142 per-side
        // border colors; T-336: 143 order)
        return true;
    }
    if (prop >= prop::ModTrimStart && prop <= prop::ModEnabled) {
        return true;  // 208-231 (T-225)
    }
    if (prop >= prop::ShapeX && prop <= prop::ShapeRound2) {
        return true;  // 232-245 (T-225)
    }
    if (prop >= prop::BoneLength && prop <= prop::ConstraintEnabled) {
        return true;  // 246-255 (T-243)
    }
    return false;
}

namespace {

/// T-243 (32 §32.2.1): the transform props a Bone or Mesh takes: translate, rotate, scale and
/// anchor (no skew, no 3D).
bool isRigTransformProp(PropId prop) {
    return (prop >= prop::AnchorX && prop <= prop::Rotation) || (prop >= prop::Anchor && prop <= prop::Scale);
}

}  // namespace

bool propertyApplies(NodeKind kind, PropId prop) {
    if (!propertyDefined(prop)) {
        return false;
    }
    if (kind == NodeKind::Modifier) {
        // T-225 (31 §31.6.1): a modifier takes its own parameters, Opacity (ignored) and Visible.
        return prop == prop::Opacity || prop == prop::Visible ||
               (prop >= prop::ModTrimStart && prop <= prop::ModEnabled);
    }
    // T-243 (32 §32.2): a bone takes its transform, Visible and its length; a constraint only its
    // parameters; a mesh Opacity, Visible and its transform.
    if (kind == NodeKind::Bone) {
        return isRigTransformProp(prop) || prop == prop::Visible || prop == prop::BoneLength;
    }
    if (kind == NodeKind::Constraint) {
        return prop >= prop::IkStrength && prop <= prop::ConstraintEnabled;
    }
    if (kind == NodeKind::Mesh) {
        return prop == prop::Opacity || prop == prop::Visible || isRigTransformProp(prop);
    }
    if (prop >= prop::BoneLength && prop <= prop::ConstraintEnabled) {
        return false;  // 246-255: Bone / Constraint only (handled above).
    }
    if (isEffectProp(prop)) {
        // 18.2: every drawable kind (not Free, not Camera), regardless of the node's actual
        // effectCount -- the effectIndex range check happens in getProperty/setProperty instead.
        return kind != NodeKind::Free && kind != NodeKind::Camera;
    }
    if (prop <= prop::AutoOrient) {
        return kind != NodeKind::Free;
    }
    if (prop <= prop::Orientation) {
        return kind != NodeKind::Free;  // 16-22: every drawable kind, including Camera.
    }
    if (prop <= prop::CameraFar) {
        return kind == NodeKind::Camera;  // 24-27
    }
    if (prop <= prop::StrokeTaperEnd) {
        // T-225: a parametric Shape takes every Path paint prop, but not the geometry (`d`).
        return kind == NodeKind::Path || (kind == NodeKind::Shape && prop != prop::PathData);
    }
    if (prop >= prop::ModTrimStart && prop <= prop::ModEnabled) {
        return false;  // 208-231: Modifier only (handled above).
    }
    if (prop >= prop::ShapeX && prop <= prop::ShapeRound2) {
        return kind == NodeKind::Shape;  // 232-245
    }
    return kind == NodeKind::Element;  // 64-93/96-138
}

ValueType propertyType(PropId prop) {
    XT_ASSERT(propertyDefined(prop));
    if (isEffectProp(prop)) {
        uint32_t p = rawEffectParamIndex(prop);
        if (p == prop::kEffectParamColor0 || p == prop::kEffectParamColor1) {
            return ValueType::Color;
        }
        if (p == prop::kEffectParamOffset) {
            return ValueType::Vec2;
        }
        return ValueType::Float;  // kEffectParamEnabled (0) and params[0..7] (1..8).
    }
    switch (prop) {
        case prop::Opacity:
        case prop::AnchorX:
        case prop::AnchorY:
        case prop::TranslateX:
        case prop::TranslateY:
        case prop::ScaleX:
        case prop::ScaleY:
        case prop::Rotation:
        case prop::SkewX:
        case prop::SkewY:
            return ValueType::Float;
        case prop::Anchor:
        case prop::Translate:
        case prop::Scale:
            return ValueType::Vec2;
        case prop::Visible:
        case prop::AutoOrient:
            return ValueType::Enum;
        case prop::TranslateZ:
        case prop::AnchorZ:
        case prop::RotationX:
        case prop::RotationY:
        case prop::CameraZoom:
        case prop::CameraNear:
        case prop::CameraFar:
            return ValueType::Float;
        case prop::Translate3:
        case prop::Anchor3:
        case prop::Orientation:
        case prop::CameraTarget:
            return ValueType::Vec3;
        case prop::PathData:
            return ValueType::Path;
        case prop::FillColor:
        case prop::StrokeColor:
            return ValueType::Color;
        case prop::FillOpacity:
        case prop::StrokeOpacity:
        case prop::StrokeWidth:
        case prop::TrimStart:
        case prop::TrimEnd:
        case prop::TrimOffset:
        case prop::DashOffset:
        case prop::MiterLimit:
        case prop::StrokeTaperStart:  // T-233
        case prop::StrokeTaperEnd:
            return ValueType::Float;
        case prop::FillGradientP0:
        case prop::FillGradientP1:
        case prop::StrokeGradientP0:
        case prop::StrokeGradientP1:
            return ValueType::Vec2;
        case prop::FillGradientStops:
        case prop::StrokeGradientStops:
            return ValueType::Stops;
        case prop::DashArray:
            return ValueType::FloatArray;
        case prop::Width:
        case prop::Height:
        case prop::MinWidth:
        case prop::MaxWidth:
        case prop::MinHeight:
        case prop::MaxHeight:
        case prop::MarginTop:
        case prop::MarginRight:
        case prop::MarginBottom:
        case prop::MarginLeft:
        case prop::PaddingTop:
        case prop::PaddingRight:
        case prop::PaddingBottom:
        case prop::PaddingLeft:
        case prop::BorderTopWidth:
        case prop::BorderRightWidth:
        case prop::BorderBottomWidth:
        case prop::BorderLeftWidth:
        case prop::Left:
        case prop::Top:
        case prop::Right:
        case prop::Bottom:
        case prop::FlexBasis:
        case prop::RowGap:
        case prop::ColumnGap:
        case prop::FontSize:
        case prop::LineHeight:
        case prop::LetterSpacing:
        case prop::BorderRadiusTL:
        case prop::BorderRadiusTR:
        case prop::BorderRadiusBR:
        case prop::BorderRadiusBL:
        case prop::OutlineWidth:  // T-325
        case prop::OutlineOffset:
        case prop::TextDecorationThickness:  // T-324
        case prop::BackgroundSizeX:          // T-327
        case prop::BackgroundSizeY:
        case prop::BackgroundPositionX:
        case prop::BackgroundPositionY:
            return ValueType::Length;
        case prop::FlexGrow:
        case prop::FlexShrink:
        case prop::AspectRatio:  // T-328
        case prop::Order:        // T-336
        case prop::ImageOpacity:
        case prop::ScrollX:
        case prop::ScrollY:
        case prop::TextReveal:
        case prop::ParticleRate:
        case prop::ParticleBurst:
        case prop::ParticleLifetimeMin:
        case prop::ParticleLifetimeMax:
        case prop::ParticleSpeedMin:
        case prop::ParticleSpeedMax:
        case prop::ParticleDirection:
        case prop::ParticleSpread:
        case prop::ParticleGravity:
        case prop::ParticleDrag:
        case prop::ParticleSizeStart:
        case prop::ParticleSizeEnd:
        case prop::ParticleRotationMin:
        case prop::ParticleRotationMax:
        case prop::ParticleMax:
            return ValueType::Float;
        case prop::Perspective:
        case prop::PerspectiveOriginX:
        case prop::PerspectiveOriginY:
            return ValueType::Length;
        case prop::BackgroundColor:
        case prop::BorderColor:
        case prop::Color:
        case prop::ParticleColor0:
        case prop::ParticleColor1:
        case prop::ParticleColor2:
        case prop::ParticleColor3:
        case prop::OutlineColor:         // T-325
        case prop::TextDecorationColor:  // T-324
        case prop::BorderTopColor:       // T-333
        case prop::BorderRightColor:
        case prop::BorderBottomColor:
        case prop::BorderLeftColor:
            return ValueType::Color;
        case prop::ModAnchor:
        case prop::ModTranslate:
        case prop::ModCenter:
            return ValueType::Vec2;  // T-225
        default:
            if ((prop >= prop::ModTrimStart && prop <= prop::ModEnabled) ||
                (prop >= prop::ShapeX && prop <= prop::ShapeRound2)) {
                return ValueType::Float;  // T-225: every other modifier / shape parameter.
            }
            if (prop >= prop::BoneLength && prop <= prop::ConstraintEnabled) {
                return ValueType::Float;  // T-243
            }
            // Unreachable when propertyDefined(prop); Release build (XT_ASSERT is a no-op there)
            // falls back to Float conservatively.
            return ValueType::Float;
    }
}

uint8_t propertyDirtyBits(PropId prop) {
    XT_ASSERT(propertyDefined(prop));
    if (isEffectProp(prop)) {
        return static_cast<uint8_t>(kDirtyPaint);
    }
    switch (prop) {
        case prop::Opacity:
            return static_cast<uint8_t>(kDirtyPaint);
        case prop::AnchorX:
        case prop::AnchorY:
        case prop::TranslateX:
        case prop::TranslateY:
        case prop::ScaleX:
        case prop::ScaleY:
        case prop::Rotation:
        case prop::SkewX:
        case prop::SkewY:
        case prop::Anchor:
        case prop::Translate:
        case prop::Scale:
        case prop::Visible:
        case prop::AutoOrient:
        case prop::TranslateZ:
        case prop::AnchorZ:
        case prop::RotationX:
        case prop::RotationY:
        case prop::Translate3:
        case prop::Anchor3:
        case prop::Orientation:
        case prop::CameraZoom:
        case prop::CameraTarget:
        case prop::CameraNear:
        case prop::CameraFar:
        // T-243: a bone's length and the constraint parameters move the skeleton pose (T-244 adds
        // the skinning dirty flag).
        case prop::BoneLength:
        case prop::IkStrength:
        case prop::ConstraintMix:
        case prop::ConstraintParam0:
        case prop::ConstraintParam1:
        case prop::ConstraintParam2:
        case prop::ConstraintParam3:
        case prop::ConstraintParam4:
        case prop::ConstraintParam5:
        case prop::ConstraintEnabled:
            return static_cast<uint8_t>(kDirtyTransform);
        // §19.4/19.10 (T-47a): a perspective change can flip whether the element is a 3D space
        // root, which affects the whole subtree's world transforms, not just this node's.
        case prop::Perspective:
        case prop::PerspectiveOriginX:
        case prop::PerspectiveOriginY:
            return static_cast<uint8_t>(kDirtyTransform | kDirtyTree);
        case prop::Width:
        case prop::Height:
        case prop::MinWidth:
        case prop::MaxWidth:
        case prop::MinHeight:
        case prop::MaxHeight:
        case prop::MarginTop:
        case prop::MarginRight:
        case prop::MarginBottom:
        case prop::MarginLeft:
        case prop::PaddingTop:
        case prop::PaddingRight:
        case prop::PaddingBottom:
        case prop::PaddingLeft:
        case prop::BorderTopWidth:
        case prop::BorderRightWidth:
        case prop::BorderBottomWidth:
        case prop::BorderLeftWidth:
        case prop::Left:
        case prop::Top:
        case prop::Right:
        case prop::Bottom:
        case prop::FlexGrow:
        case prop::FlexShrink:
        case prop::FlexBasis:
        case prop::RowGap:
        case prop::ColumnGap:
        case prop::FontSize:
        case prop::LineHeight:
        case prop::LetterSpacing:
        case prop::AspectRatio:  // T-328 (06 §6.16).
        case prop::Order:        // T-336 (06 §6.9): item order, and with it paint / hit order.
        // T-32: ScrollX/Y feed layout_engine.cpp's applyTransforms (NodeCommon::translate), not
        // just paint, so a scroll offset write must trigger a relayout/transform pass like any
        // other L prop -- see Docs/tasks/T-32.md "把 ScrollX/ScrollY 的髒位改為 L".
        case prop::ScrollX:
        case prop::ScrollY:
            return static_cast<uint8_t>(kDirtyLayout);
        default:
            // Path props (32-51) and the remaining Element paint props (96-103) are all P.
            return static_cast<uint8_t>(kDirtyPaint);
    }
}

const char* propertyName(PropId prop) {
    if (isEffectProp(prop)) {
        if (!propertyDefined(prop)) {
            return "?";
        }
        // 18.2/18.3: the fully-resolved name ("effect[0].blur" for DropShadow's paramIndex 3)
        // needs the effect's actual EffectKind, which this context-free (PropId only) overload
        // has no way to look up (no NodeArena/NodeId here) -- Docs/tasks/T-50.md's "kind 未知" ->
        // "effect[<i>].param<p>" fallback, always taken by this function. Use the
        // propertyName(arena, node, prop) overload below for the fully-resolved name.
        //
        // Context is single-threaded (01.6), so a plain (non-thread_local) function-local static
        // buffer is safe here, matching command_list.cpp/painter.cpp's existing use of the same
        // pattern.
        static char buf[32];
        std::snprintf(buf, sizeof(buf), "effect[%u].param%u", rawEffectIndex(prop),
                      rawEffectParamIndex(prop));
        return buf;
    }
    switch (prop) {
        case prop::Opacity:
            return "Opacity";
        case prop::AnchorX:
            return "AnchorX";
        case prop::AnchorY:
            return "AnchorY";
        case prop::TranslateX:
            return "TranslateX";
        case prop::TranslateY:
            return "TranslateY";
        case prop::ScaleX:
            return "ScaleX";
        case prop::ScaleY:
            return "ScaleY";
        case prop::Rotation:
            return "Rotation";
        case prop::SkewX:
            return "SkewX";
        case prop::SkewY:
            return "SkewY";
        case prop::Anchor:
            return "Anchor";
        case prop::Translate:
            return "Translate";
        case prop::Scale:
            return "Scale";
        case prop::Visible:
            return "Visible";
        case prop::AutoOrient:
            return "AutoOrient";
        case prop::TranslateZ:
            return "TranslateZ";
        case prop::AnchorZ:
            return "AnchorZ";
        case prop::RotationX:
            return "RotationX";
        case prop::RotationY:
            return "RotationY";
        case prop::Translate3:
            return "Translate3";
        case prop::Anchor3:
            return "Anchor3";
        case prop::Orientation:
            return "Orientation";
        case prop::CameraZoom:
            return "CameraZoom";
        case prop::CameraTarget:
            return "CameraTarget";
        case prop::CameraNear:
            return "CameraNear";
        case prop::CameraFar:
            return "CameraFar";
        case prop::PathData:
            return "PathData";
        case prop::FillColor:
            return "FillColor";
        case prop::FillOpacity:
            return "FillOpacity";
        case prop::StrokeColor:
            return "StrokeColor";
        case prop::StrokeOpacity:
            return "StrokeOpacity";
        case prop::StrokeWidth:
            return "StrokeWidth";
        case prop::TrimStart:
            return "TrimStart";
        case prop::TrimEnd:
            return "TrimEnd";
        case prop::TrimOffset:
            return "TrimOffset";
        case prop::FillGradientP0:
            return "FillGradientP0";
        case prop::FillGradientP1:
            return "FillGradientP1";
        case prop::FillGradientStops:
            return "FillGradientStops";
        case prop::StrokeGradientP0:
            return "StrokeGradientP0";
        case prop::StrokeGradientP1:
            return "StrokeGradientP1";
        case prop::StrokeGradientStops:
            return "StrokeGradientStops";
        case prop::DashOffset:
            return "DashOffset";
        case prop::DashArray:
            return "DashArray";
        case prop::MiterLimit:
            return "MiterLimit";
        case prop::StrokeTaperStart:
            return "StrokeTaperStart";
        case prop::StrokeTaperEnd:
            return "StrokeTaperEnd";
        case prop::Width:
            return "Width";
        case prop::Height:
            return "Height";
        case prop::MinWidth:
            return "MinWidth";
        case prop::MaxWidth:
            return "MaxWidth";
        case prop::MinHeight:
            return "MinHeight";
        case prop::MaxHeight:
            return "MaxHeight";
        case prop::MarginTop:
            return "MarginTop";
        case prop::MarginRight:
            return "MarginRight";
        case prop::MarginBottom:
            return "MarginBottom";
        case prop::MarginLeft:
            return "MarginLeft";
        case prop::PaddingTop:
            return "PaddingTop";
        case prop::PaddingRight:
            return "PaddingRight";
        case prop::PaddingBottom:
            return "PaddingBottom";
        case prop::PaddingLeft:
            return "PaddingLeft";
        case prop::BorderTopWidth:
            return "BorderTopWidth";
        case prop::BorderRightWidth:
            return "BorderRightWidth";
        case prop::BorderBottomWidth:
            return "BorderBottomWidth";
        case prop::BorderLeftWidth:
            return "BorderLeftWidth";
        case prop::Left:
            return "Left";
        case prop::Top:
            return "Top";
        case prop::Right:
            return "Right";
        case prop::Bottom:
            return "Bottom";
        case prop::FlexGrow:
            return "FlexGrow";
        case prop::FlexShrink:
            return "FlexShrink";
        case prop::FlexBasis:
            return "FlexBasis";
        case prop::RowGap:
            return "RowGap";
        case prop::ColumnGap:
            return "ColumnGap";
        case prop::FontSize:
            return "FontSize";
        case prop::LineHeight:
            return "LineHeight";
        case prop::LetterSpacing:
            return "LetterSpacing";
        case prop::BackgroundColor:
            return "BackgroundColor";
        case prop::BorderColor:
            return "BorderColor";
        case prop::BorderRadiusTL:
            return "BorderRadiusTL";
        case prop::BorderRadiusTR:
            return "BorderRadiusTR";
        case prop::BorderRadiusBR:
            return "BorderRadiusBR";
        case prop::BorderRadiusBL:
            return "BorderRadiusBL";
        case prop::Color:
            return "Color";
        case prop::ImageOpacity:
            return "ImageOpacity";
        case prop::ScrollX:
            return "ScrollX";
        case prop::ScrollY:
            return "ScrollY";
        case prop::Perspective:
            return "Perspective";
        case prop::PerspectiveOriginX:
            return "PerspectiveOriginX";
        case prop::PerspectiveOriginY:
            return "PerspectiveOriginY";
        case prop::TextReveal:
            return "TextReveal";
        case prop::ParticleRate:
            return "ParticleRate";
        case prop::ParticleBurst:
            return "ParticleBurst";
        case prop::ParticleLifetimeMin:
            return "ParticleLifetimeMin";
        case prop::ParticleLifetimeMax:
            return "ParticleLifetimeMax";
        case prop::ParticleSpeedMin:
            return "ParticleSpeedMin";
        case prop::ParticleSpeedMax:
            return "ParticleSpeedMax";
        case prop::ParticleDirection:
            return "ParticleDirection";
        case prop::ParticleSpread:
            return "ParticleSpread";
        case prop::ParticleGravity:
            return "ParticleGravity";
        case prop::ParticleDrag:
            return "ParticleDrag";
        case prop::ParticleSizeStart:
            return "ParticleSizeStart";
        case prop::ParticleSizeEnd:
            return "ParticleSizeEnd";
        case prop::ParticleColor0:
            return "ParticleColor0";
        case prop::ParticleColor1:
            return "ParticleColor1";
        case prop::ParticleColor2:
            return "ParticleColor2";
        case prop::ParticleColor3:
            return "ParticleColor3";
        case prop::ParticleRotationMin:
            return "ParticleRotationMin";
        case prop::ParticleRotationMax:
            return "ParticleRotationMax";
        case prop::ParticleMax:
            return "ParticleMax";
        case prop::OutlineWidth:
            return "OutlineWidth";
        case prop::OutlineColor:
            return "OutlineColor";
        case prop::OutlineOffset:
            return "OutlineOffset";
        case prop::TextDecorationColor:
            return "TextDecorationColor";
        case prop::TextDecorationThickness:
            return "TextDecorationThickness";
        case prop::BackgroundSizeX:
            return "BackgroundSizeX";
        case prop::BackgroundSizeY:
            return "BackgroundSizeY";
        case prop::BackgroundPositionX:
            return "BackgroundPositionX";
        case prop::BackgroundPositionY:
            return "BackgroundPositionY";
        case prop::AspectRatio:
            return "AspectRatio";
        case prop::BorderTopColor:
            return "BorderTopColor";
        case prop::BorderRightColor:
            return "BorderRightColor";
        case prop::BorderBottomColor:
            return "BorderBottomColor";
        case prop::BorderLeftColor:
            return "BorderLeftColor";
        case prop::Order:
            return "Order";
        default:
            if (prop >= prop::ModTrimStart && prop <= prop::ModEnabled) {
                return kModifierPropNames[prop - prop::ModTrimStart];  // T-225
            }
            if (prop >= prop::ShapeX && prop <= prop::ShapeRound2) {
                return kShapePropNames[prop - prop::ShapeX];  // T-225
            }
            if (prop >= prop::BoneLength && prop <= prop::ConstraintEnabled) {
                return kRigPropNames[prop - prop::BoneLength];  // T-243
            }
            return "?";
    }
}

const char* propertyName(const NodeArena& arena, NodeId node, PropId prop) {
    if (!isEffectProp(prop) || !propertyDefined(prop)) {
        return propertyName(prop);
    }
    uint32_t effectIndex = rawEffectIndex(prop);
    uint32_t paramIndex = rawEffectParamIndex(prop);
    const NodeCommon& common = arena.common(node);
    if (effectIndex >= common.effectCount) {
        return propertyName(prop);
    }
    EffectKind kind = arena.effects()[common.effectOffset + effectIndex].kind;
    const char* name = effectParamNameForKind(kind, paramIndex);
    if (name == nullptr) {
        return propertyName(prop);
    }
    // Context is single-threaded (01.6); see propertyName(PropId)'s identical use of a plain
    // function-local static buffer just above.
    static char buf[40];
    std::snprintf(buf, sizeof(buf), "effect[%u].%s", effectIndex, name);
    return buf;
}

namespace {
struct NamedProp {
    const char* name;
    PropId id;
};

// Kept in PropId order; propertyByName() is a linear scan (small, non-hot-path table).
constexpr NamedProp kNamedProps[] = {
    {"Opacity", prop::Opacity},
    {"AnchorX", prop::AnchorX},
    {"AnchorY", prop::AnchorY},
    {"TranslateX", prop::TranslateX},
    {"TranslateY", prop::TranslateY},
    {"ScaleX", prop::ScaleX},
    {"ScaleY", prop::ScaleY},
    {"Rotation", prop::Rotation},
    {"SkewX", prop::SkewX},
    {"SkewY", prop::SkewY},
    {"Anchor", prop::Anchor},
    {"Translate", prop::Translate},
    {"Scale", prop::Scale},
    {"Visible", prop::Visible},
    {"AutoOrient", prop::AutoOrient},
    {"TranslateZ", prop::TranslateZ},
    {"AnchorZ", prop::AnchorZ},
    {"RotationX", prop::RotationX},
    {"RotationY", prop::RotationY},
    {"Translate3", prop::Translate3},
    {"Anchor3", prop::Anchor3},
    {"Orientation", prop::Orientation},
    {"CameraZoom", prop::CameraZoom},
    {"CameraTarget", prop::CameraTarget},
    {"CameraNear", prop::CameraNear},
    {"CameraFar", prop::CameraFar},
    {"PathData", prop::PathData},
    {"FillColor", prop::FillColor},
    {"FillOpacity", prop::FillOpacity},
    {"StrokeColor", prop::StrokeColor},
    {"StrokeOpacity", prop::StrokeOpacity},
    {"StrokeWidth", prop::StrokeWidth},
    {"TrimStart", prop::TrimStart},
    {"TrimEnd", prop::TrimEnd},
    {"TrimOffset", prop::TrimOffset},
    {"FillGradientP0", prop::FillGradientP0},
    {"FillGradientP1", prop::FillGradientP1},
    {"FillGradientStops", prop::FillGradientStops},
    {"StrokeGradientP0", prop::StrokeGradientP0},
    {"StrokeGradientP1", prop::StrokeGradientP1},
    {"StrokeGradientStops", prop::StrokeGradientStops},
    {"DashOffset", prop::DashOffset},
    {"DashArray", prop::DashArray},
    {"MiterLimit", prop::MiterLimit},
    {"StrokeTaperStart", prop::StrokeTaperStart},
    {"StrokeTaperEnd", prop::StrokeTaperEnd},
    {"Width", prop::Width},
    {"Height", prop::Height},
    {"MinWidth", prop::MinWidth},
    {"MaxWidth", prop::MaxWidth},
    {"MinHeight", prop::MinHeight},
    {"MaxHeight", prop::MaxHeight},
    {"MarginTop", prop::MarginTop},
    {"MarginRight", prop::MarginRight},
    {"MarginBottom", prop::MarginBottom},
    {"MarginLeft", prop::MarginLeft},
    {"PaddingTop", prop::PaddingTop},
    {"PaddingRight", prop::PaddingRight},
    {"PaddingBottom", prop::PaddingBottom},
    {"PaddingLeft", prop::PaddingLeft},
    {"BorderTopWidth", prop::BorderTopWidth},
    {"BorderRightWidth", prop::BorderRightWidth},
    {"BorderBottomWidth", prop::BorderBottomWidth},
    {"BorderLeftWidth", prop::BorderLeftWidth},
    {"Left", prop::Left},
    {"Top", prop::Top},
    {"Right", prop::Right},
    {"Bottom", prop::Bottom},
    {"FlexGrow", prop::FlexGrow},
    {"FlexShrink", prop::FlexShrink},
    {"FlexBasis", prop::FlexBasis},
    {"RowGap", prop::RowGap},
    {"ColumnGap", prop::ColumnGap},
    {"FontSize", prop::FontSize},
    {"LineHeight", prop::LineHeight},
    {"LetterSpacing", prop::LetterSpacing},
    {"BackgroundColor", prop::BackgroundColor},
    {"BorderColor", prop::BorderColor},
    {"BorderRadiusTL", prop::BorderRadiusTL},
    {"BorderRadiusTR", prop::BorderRadiusTR},
    {"BorderRadiusBR", prop::BorderRadiusBR},
    {"BorderRadiusBL", prop::BorderRadiusBL},
    {"Color", prop::Color},
    {"ImageOpacity", prop::ImageOpacity},
    {"ScrollX", prop::ScrollX},
    {"ScrollY", prop::ScrollY},
    {"Perspective", prop::Perspective},
    {"PerspectiveOriginX", prop::PerspectiveOriginX},
    {"PerspectiveOriginY", prop::PerspectiveOriginY},
    {"TextReveal", prop::TextReveal},
    {"ParticleRate", prop::ParticleRate},
    {"ParticleBurst", prop::ParticleBurst},
    {"ParticleLifetimeMin", prop::ParticleLifetimeMin},
    {"ParticleLifetimeMax", prop::ParticleLifetimeMax},
    {"ParticleSpeedMin", prop::ParticleSpeedMin},
    {"ParticleSpeedMax", prop::ParticleSpeedMax},
    {"ParticleDirection", prop::ParticleDirection},
    {"ParticleSpread", prop::ParticleSpread},
    {"ParticleGravity", prop::ParticleGravity},
    {"ParticleDrag", prop::ParticleDrag},
    {"ParticleSizeStart", prop::ParticleSizeStart},
    {"ParticleSizeEnd", prop::ParticleSizeEnd},
    {"ParticleColor0", prop::ParticleColor0},
    {"ParticleColor1", prop::ParticleColor1},
    {"ParticleColor2", prop::ParticleColor2},
    {"ParticleColor3", prop::ParticleColor3},
    {"ParticleRotationMin", prop::ParticleRotationMin},
    {"ParticleRotationMax", prop::ParticleRotationMax},
    {"ParticleMax", prop::ParticleMax},
    {"OutlineWidth", prop::OutlineWidth},
    {"OutlineColor", prop::OutlineColor},
    {"OutlineOffset", prop::OutlineOffset},
    {"TextDecorationColor", prop::TextDecorationColor},
    {"TextDecorationThickness", prop::TextDecorationThickness},
    {"BackgroundSizeX", prop::BackgroundSizeX},
    {"BackgroundSizeY", prop::BackgroundSizeY},
    {"BackgroundPositionX", prop::BackgroundPositionX},
    {"BackgroundPositionY", prop::BackgroundPositionY},
    {"AspectRatio", prop::AspectRatio},
    {"BorderTopColor", prop::BorderTopColor},
    {"BorderRightColor", prop::BorderRightColor},
    {"BorderBottomColor", prop::BorderBottomColor},
    {"BorderLeftColor", prop::BorderLeftColor},
    {"Order", prop::Order},
};
}  // namespace

PropId propertyByName(std::string_view name) {
    for (const NamedProp& entry : kNamedProps) {
        if (name == entry.name) {
            return entry.id;
        }
    }
    for (PropId p = prop::ModTrimStart; p <= prop::ModEnabled; ++p) {  // T-225
        if (name == kModifierPropNames[p - prop::ModTrimStart]) {
            return p;
        }
    }
    for (PropId p = prop::ShapeX; p <= prop::ShapeRound2; ++p) {  // T-225
        if (name == kShapePropNames[p - prop::ShapeX]) {
            return p;
        }
    }
    for (PropId p = prop::BoneLength; p <= prop::ConstraintEnabled; ++p) {  // T-243
        if (name == kRigPropNames[p - prop::BoneLength]) {
            return p;
        }
    }
    return prop::kInvalidProp;
}

namespace {

/// Element PropId (64-105) initial value, per Docs/design/05-style-and-css.md §5.4 and
/// Docs/design/03-scene-and-properties.md §3.9.
Value defaultElementValue(PropId prop) {
    switch (prop) {
        case prop::TranslateZ:
        case prop::AnchorZ:
        case prop::RotationX:
        case prop::RotationY:
            return Value::ofFloat(0);
        case prop::Translate3:
        case prop::Anchor3:
        case prop::Orientation:
            return Value::ofVec3(Vec3{0, 0, 0});
        case prop::Width:
        case prop::Height:
        case prop::MinWidth:
        case prop::MaxWidth:
        case prop::MinHeight:
        case prop::MaxHeight:
        case prop::Left:
        case prop::Top:
        case prop::Right:
        case prop::Bottom:
        case prop::FlexBasis:
            return Value::ofLength(0, LengthUnit::Auto);
        case prop::FontSize:
            return Value::ofLength(16, LengthUnit::Px);
        case prop::LineHeight:
            return Value::ofLength(1.2f, LengthUnit::Em);
        case prop::Perspective:
            return Value::ofLength(0, LengthUnit::Auto);  // `none` (19.4/19.10, T-47a).
        case prop::PerspectiveOriginX:
        case prop::PerspectiveOriginY:
            return Value::ofLength(50, LengthUnit::Percent);
        case prop::FlexGrow:
            return Value::ofFloat(0);
        case prop::FlexShrink:
            return Value::ofFloat(1);
        case prop::ImageOpacity:
        case prop::TextReveal:
            return Value::ofFloat(1);
        // T-202 (03 §3.12.5): particle defaults -- a plain white, upward, 1 s fade-out emitter.
        case prop::ParticleRate:
            return Value::ofFloat(10);
        case prop::ParticleLifetimeMin:
        case prop::ParticleLifetimeMax:
            return Value::ofFloat(1);
        case prop::ParticleSpeedMin:
        case prop::ParticleSpeedMax:
            return Value::ofFloat(100);
        case prop::ParticleDirection:
            return Value::ofFloat(-90);
        case prop::ParticleSizeStart:
        case prop::ParticleSizeEnd:
            return Value::ofFloat(8);
        // White fading out: the stops of the straight (1,1,1,1) -> (1,1,1,0) line.
        case prop::ParticleColor0:
            return Value::ofColor(Color{1, 1, 1, 1});
        case prop::ParticleColor1:
            return Value::ofColor(Color{1, 1, 1, 2.0f / 3.0f});
        case prop::ParticleColor2:
            return Value::ofColor(Color{1, 1, 1, 1.0f / 3.0f});
        case prop::ParticleColor3:
            return Value::ofColor(Color{1, 1, 1, 0});
        case prop::ParticleMax:
            return Value::ofFloat(256);
        case prop::BackgroundColor:
        case prop::BorderColor:
        // T-333: a side's own initial value is `border-color`'s; StyleEngine copies the element's
        // BorderColor into every side no rule declares, this is only the context-free fallback.
        case prop::BorderTopColor:
        case prop::BorderRightColor:
        case prop::BorderBottomColor:
        case prop::BorderLeftColor:
            return Value::ofColor(Color{0, 0, 0, 0});
        case prop::Color:
        // T-325: `outline-color`'s initial value is `color`; StyleEngine substitutes the element's
        // `color` when no rule declares it, this is only the context-free fallback.
        case prop::OutlineColor:
        // T-324: likewise `text-decoration-color`.
        case prop::TextDecorationColor:
            return Value::ofColor(Color{0, 0, 0, 1});
        case prop::TextDecorationThickness:
        case prop::BackgroundSizeX:  // T-327: `auto`.
        case prop::BackgroundSizeY:
            return Value::ofLength(0, LengthUnit::Auto);  // T-324: `auto`.
        case prop::BackgroundPositionX:                   // T-327: `0% 0%`.
        case prop::BackgroundPositionY:
            return Value::ofLength(0, LengthUnit::Percent);
        default:
            // Remaining Length props (margin/padding/border-width/gap/letter-spacing/radius) default
            // to 0px; remaining Float props (ScrollX/ScrollY) default to 0.
            if (propertyType(prop) == ValueType::Length) {
                return Value::ofLength(0, LengthUnit::Px);
            }
            return Value::ofFloat(0);
    }
}

}  // namespace

void initComputedStyleDefaults(ComputedStyle& style) {
    // T-119: the general PropIds (0-13) whose 05.4/03.9 initial value is not Value{}'s Float 0 --
    // Opacity, ScaleX and ScaleY are 1. They used to be left at 0 here, and style_engine.cpp
    // decides whether to call setProperty() by comparing the freshly cascaded value against the
    // element's *previous* ComputedStyle::base, so a declared `opacity: 0` (or `scale(0)`)
    // compared equal to that stale 0, setProperty() was skipped, and NodeCommon::opacity kept its
    // own default of 1 -- the element painted fully opaque. Every other general PropId's initial
    // value is 0, which Value{} already gives it.
    style.base[prop::Opacity] = Value::ofFloat(1);
    style.base[prop::ScaleX] = Value::ofFloat(1);
    style.base[prop::ScaleY] = Value::ofFloat(1);
    style.base[prop::AutoOrient] = Value::ofEnum(0);
    for (PropId p = prop::TranslateZ; p <= prop::Orientation; ++p) {
        style.base[p] = defaultElementValue(p);
    }
    for (PropId p = prop::Width; p <= prop::LetterSpacing; ++p) {
        style.base[p] = defaultElementValue(p);
    }
    for (PropId p = prop::BackgroundColor; p <= prop::Order; ++p) {
        style.base[p] = defaultElementValue(p);
    }
}

namespace {

/// T-255: a Path value's own verbs (PathVerb codes as floats, `stopCount` of them) are exact codes
/// whose operands add up to its `floatCount` points.
bool ownVerbsMatchPoints(const Value& value) {
    return floatVerbsMatchPoints(value.pathVerbs, value.stopCount, value.floatCount);
}

/// Path props 32-51 of a PathNodeData, or (T-225) the paint props 33-51 of a ShapeNodeData, which
/// has the same paint fields.
template <class PaintData>
void setPathProperty(PaintData& path, PropId prop, const Value& value) {
    switch (prop) {
        case prop::PathData: {
            if constexpr (std::is_same_v<PaintData, PathNodeData>) {
                // T-190 (03 §3.12.6): a value carrying its own verbs may change the topology (a
                // keyframe jump between paths of different shape); one without verbs must match
                // the node's point count and is ignored otherwise.
                bool sameVerbs = value.pathVerbs == nullptr;
                if (value.pathVerbs != nullptr && value.stopCount == path.verbs.size()) {
                    sameVerbs = true;
                    for (uint32_t i = 0; i < value.stopCount; ++i) {
                        if (static_cast<float>(static_cast<uint8_t>(path.verbs[i])) != value.pathVerbs[i]) {
                            sameVerbs = false;
                            break;
                        }
                    }
                }
                if (sameVerbs && value.floatCount == path.points.size()) {
                    std::memcpy(path.points.data(), value.floats, sizeof(float) * value.floatCount);
                } else if (value.pathVerbs != nullptr && ownVerbsMatchPoints(value)) {
                    // T-255: verbs whose operands do not add up to the points are ignored, never
                    // installed (the painter would read past the points).
                    path.verbs.resize(value.stopCount);
                    for (uint32_t i = 0; i < value.stopCount; ++i) {
                        path.verbs[i] = static_cast<PathVerb>(static_cast<uint8_t>(value.pathVerbs[i]));
                    }
                    path.points.assign(value.floats, value.floats + value.floatCount);
                }
            } else {
                XT_ASSERT(false && "unreachable: a Shape has no PathData");
            }
            break;
        }
        case prop::FillColor:
            path.fill.color = value.c;
            break;
        case prop::FillOpacity:
            path.fillOpacity = value.f;
            break;
        case prop::StrokeColor:
            path.stroke.color = value.c;
            break;
        case prop::StrokeOpacity:
            path.strokeOpacity = value.f;
            break;
        case prop::StrokeWidth:
            path.strokeStyle.width = value.f;
            break;
        case prop::TrimStart:
            path.trimStart = value.f;
            break;
        case prop::TrimEnd:
            path.trimEnd = value.f;
            break;
        case prop::TrimOffset:
            path.trimOffset = value.f;
            break;
        case prop::FillGradientP0:
            path.fill.p0 = value.v2;
            break;
        case prop::FillGradientP1:
            path.fill.p1 = value.v2;
            break;
        case prop::FillGradientStops:
            XT_ASSERT(value.stopCount == path.fill.stopCount);
            if (value.stopCount == path.fill.stopCount) {
                for (uint32_t i = 0; i < value.stopCount; ++i) {
                    path.stops[path.fill.stopOffset + i] = value.stops[i];
                }
            }
            break;
        case prop::StrokeGradientP0:
            path.stroke.p0 = value.v2;
            break;
        case prop::StrokeGradientP1:
            path.stroke.p1 = value.v2;
            break;
        case prop::StrokeGradientStops:
            XT_ASSERT(value.stopCount == path.stroke.stopCount);
            if (value.stopCount == path.stroke.stopCount) {
                for (uint32_t i = 0; i < value.stopCount; ++i) {
                    path.stops[path.stroke.stopOffset + i] = value.stops[i];
                }
            }
            break;
        case prop::DashOffset:
            path.strokeStyle.dashPhase = value.f;
            break;
        case prop::DashArray:
            path.dashes.assign(value.floats, value.floats + value.floatCount);
            path.strokeStyle.dashCount = static_cast<uint16_t>(value.floatCount);
            break;
        case prop::MiterLimit:
            path.strokeStyle.miterLimit = value.f;
            break;
        case prop::StrokeTaperStart:  // T-233
            path.strokeStyle.taperStart = value.f;
            break;
        case prop::StrokeTaperEnd:
            path.strokeStyle.taperEnd = value.f;
            break;
        default:
            XT_ASSERT(false && "unreachable: not a Path PropId");
            break;
    }
}

/// T-225: where a modifier PropId lives in ModifierNodeData for the modifier's kind -- a params
/// index (a Vec2 prop takes index and index + 1), one of the kMod* slots below, or -1 when the
/// kind does not use the prop.
constexpr int kModOpacityA = 100;
constexpr int kModOpacityB = 101;
constexpr int kModEnabledSlot = 102;

int modifierSlot(ModifierKind kind, PropId prop) {
    if (prop == prop::ModEnabled) {
        return kModEnabledSlot;
    }
    switch (kind) {
        case ModifierKind::Trim:
            switch (prop) {
                case prop::ModTrimStart:
                    return 0;
                case prop::ModTrimEnd:
                    return 1;
                case prop::ModTrimOffset:
                    return 2;
                default:
                    return -1;
            }
        case ModifierKind::Repeat:
            switch (prop) {
                case prop::ModCopies:
                    return 0;
                case prop::ModOffset:
                    return 1;
                case prop::ModAnchor:
                    return 2;
                case prop::ModTranslate:
                    return 4;
                case prop::ModRotation:
                    return 6;
                case prop::ModScale:
                    return 7;
                case prop::ModStartOpacity:
                    return kModOpacityA;
                case prop::ModEndOpacity:
                    return kModOpacityB;
                default:
                    return -1;
            }
        case ModifierKind::Round:
            return prop == prop::ModRadius ? 0 : -1;
        case ModifierKind::Pucker:
        case ModifierKind::Offset:
            return prop == prop::ModAmount ? 0 : -1;
        case ModifierKind::ZigZag:
            return prop == prop::ModSize ? 0 : (prop == prop::ModRidges ? 1 : -1);
        case ModifierKind::Twist:
            return prop == prop::ModAngle ? 0 : (prop == prop::ModCenter ? 1 : -1);
        case ModifierKind::Wiggle:
            switch (prop) {
                case prop::ModSize:
                    return 0;
                case prop::ModDetail:
                    return 1;
                case prop::ModSeed:
                    return 2;
                case prop::ModSpeed:
                    return 3;
                case prop::ModCorrelation:
                    return 4;
                case prop::ModTemporalPhase:
                    return 5;
                case prop::ModSpatialPhase:
                    return 6;
                default:
                    return -1;
            }
        default:
            return -1;  // Merge: no animatable parameters.
    }
}

Value getModifierProperty(const ModifierNodeData& m, PropId prop) {
    const int slot = modifierSlot(m.kind, prop);
    const bool vec2 = propertyType(prop) == ValueType::Vec2;
    if (slot < 0) {
        return vec2 ? Value::ofVec2(Vec2{}) : Value::ofFloat(0);
    }
    if (slot == kModEnabledSlot) {
        return Value::ofFloat(m.enabled != 0 ? 1.0f : 0.0f);
    }
    if (slot == kModOpacityA) {
        return Value::ofFloat(m.opacityA);
    }
    if (slot == kModOpacityB) {
        return Value::ofFloat(m.opacityB);
    }
    return vec2 ? Value::ofVec2(Vec2{m.params[slot], m.params[slot + 1]}) : Value::ofFloat(m.params[slot]);
}

void setModifierProperty(ModifierNodeData& m, PropId prop, const Value& value) {
    const int slot = modifierSlot(m.kind, prop);
    if (slot < 0) {
        return;  // A parameter this kind does not have: ignored (see prop::ModTrimStart).
    }
    if (slot == kModEnabledSlot) {
        m.enabled = value.f >= 0.5f ? 1 : 0;
    } else if (slot == kModOpacityA) {
        m.opacityA = value.f;
    } else if (slot == kModOpacityB) {
        m.opacityB = value.f;
    } else if (value.type == ValueType::Vec2) {
        m.params[slot] = value.v2.x;
        m.params[slot + 1] = value.v2.y;
    } else {
        m.params[slot] = value.f;
    }
}

/// T-225: the ShapeNodeData::params index of a shape PropId for the shape's kind, -1 if unused.
int shapeSlot(ShapeKind kind, PropId prop) {
    switch (kind) {
        case ShapeKind::Rect:
            switch (prop) {
                case prop::ShapeX:
                    return 0;
                case prop::ShapeY:
                    return 1;
                case prop::ShapeW:
                    return 2;
                case prop::ShapeH:
                    return 3;
                case prop::ShapeRx:
                    return 4;
                case prop::ShapeRy:
                    return 5;
                default:
                    return -1;
            }
        case ShapeKind::Ellipse:
            switch (prop) {
                case prop::ShapeCx:
                    return 0;
                case prop::ShapeCy:
                    return 1;
                case prop::ShapeRx:
                    return 2;
                case prop::ShapeRy:
                    return 3;
                default:
                    return -1;
            }
        case ShapeKind::Star:
            switch (prop) {
                case prop::ShapeCx:
                    return 0;
                case prop::ShapeCy:
                    return 1;
                case prop::ShapePoints:
                    return 2;
                case prop::ShapeR:
                    return 3;
                case prop::ShapeR2:
                    return 4;
                case prop::ShapeRotation:
                    return 5;
                case prop::ShapeRound:
                    return 6;
                case prop::ShapeRound2:
                    return 7;
                default:
                    return -1;
            }
        default:
            return -1;
    }
}

template <class PaintData>
Value getPathProperty(const PaintData& path, PropId prop) {
    switch (prop) {
        case prop::PathData:
            if constexpr (std::is_same_v<PaintData, PathNodeData>) {
                return Value::ofPath(path.points.data(), static_cast<uint32_t>(path.points.size()));
            } else {
                XT_ASSERT(false && "unreachable: a Shape has no PathData");
                return Value{};
            }
        case prop::FillColor:
            return Value::ofColor(path.fill.color);
        case prop::FillOpacity:
            return Value::ofFloat(path.fillOpacity);
        case prop::StrokeColor:
            return Value::ofColor(path.stroke.color);
        case prop::StrokeOpacity:
            return Value::ofFloat(path.strokeOpacity);
        case prop::StrokeWidth:
            return Value::ofFloat(path.strokeStyle.width);
        case prop::TrimStart:
            return Value::ofFloat(path.trimStart);
        case prop::TrimEnd:
            return Value::ofFloat(path.trimEnd);
        case prop::TrimOffset:
            return Value::ofFloat(path.trimOffset);
        case prop::FillGradientP0:
            return Value::ofVec2(path.fill.p0);
        case prop::FillGradientP1:
            return Value::ofVec2(path.fill.p1);
        case prop::FillGradientStops:
            return Value::ofStops(path.stops.data() + path.fill.stopOffset, path.fill.stopCount);
        case prop::StrokeGradientP0:
            return Value::ofVec2(path.stroke.p0);
        case prop::StrokeGradientP1:
            return Value::ofVec2(path.stroke.p1);
        case prop::StrokeGradientStops:
            return Value::ofStops(path.stops.data() + path.stroke.stopOffset, path.stroke.stopCount);
        case prop::DashOffset:
            return Value::ofFloat(path.strokeStyle.dashPhase);
        case prop::DashArray:
            return Value::ofFloatArray(path.dashes.data(), static_cast<uint32_t>(path.dashes.size()));
        case prop::MiterLimit:
            return Value::ofFloat(path.strokeStyle.miterLimit);
        case prop::StrokeTaperStart:  // T-233
            return Value::ofFloat(path.strokeStyle.taperStart);
        case prop::StrokeTaperEnd:
            return Value::ofFloat(path.strokeStyle.taperEnd);
        default:
            XT_ASSERT(false && "unreachable: not a Path PropId");
            return Value{};
    }
}

/// Type-appropriate default for an effect parameter (18.2's "effectIndex 不存在 -> get 回傳型別
/// 預設值" rule): 0 for Float, transparent for Color, (0, 0) for the composite Vec2.
Value defaultEffectParamValue(uint32_t paramIndex) {
    if (paramIndex == prop::kEffectParamColor0 || paramIndex == prop::kEffectParamColor1) {
        return Value::ofColor(Color{0, 0, 0, 0});
    }
    if (paramIndex == prop::kEffectParamOffset) {
        return Value::ofVec2(Vec2{0, 0});
    }
    return Value::ofFloat(0);
}

Value getEffectProperty(const NodeArena& arena, NodeId node, PropId prop) {
    uint32_t effectIndex = rawEffectIndex(prop);
    uint32_t paramIndex = rawEffectParamIndex(prop);
    const NodeCommon& common = arena.common(node);
    if (effectIndex >= common.effectCount) {
        return defaultEffectParamValue(paramIndex);
    }
    const EffectInstance& e = arena.effects()[common.effectOffset + effectIndex];
    if (paramIndex == prop::kEffectParamEnabled) {
        return Value::ofFloat(e.enabled != 0 ? 1.0f : 0.0f);
    }
    if (paramIndex >= 1 && paramIndex <= 8) {
        return Value::ofFloat(e.params[paramIndex - 1]);
    }
    if (paramIndex == prop::kEffectParamColor0) {
        return Value::ofColor(e.colors[0]);
    }
    if (paramIndex == prop::kEffectParamColor1) {
        return Value::ofColor(e.colors[1]);
    }
    XT_ASSERT(paramIndex == prop::kEffectParamOffset);
    return Value::ofVec2(Vec2{e.params[0], e.params[1]});
}

/// 18.2's "effectIndex 不存在 -> set 忽略" rule. T-211: a no-op in every build (no Debug assert) --
/// a keyframe track (`@keyframes` / `@timeline` `effect`) may address an effect the element's stack
/// does not have (no static `effect`, or a state that removed it); the arena latches it for a
/// one-shot kWarnEffectMissing document warning.
bool setEffectProperty(NodeArena& arena, NodeId node, PropId prop, const Value& value) {
    uint32_t effectIndex = rawEffectIndex(prop);
    uint32_t paramIndex = rawEffectParamIndex(prop);
    NodeCommon& common = arena.common(node);
    if (effectIndex >= common.effectCount) {
        arena.noteEffectMissingWritten();
        return false;
    }
    EffectInstance& e = arena.effects()[common.effectOffset + effectIndex];
    if (paramIndex == prop::kEffectParamEnabled) {
        const uint8_t v = static_cast<uint8_t>(value.f != 0 ? 1 : 0);
        const bool changed = e.enabled != v;
        e.enabled = v;
        return changed;
    }
    if (paramIndex >= 1 && paramIndex <= 8) {
        const bool changed = e.params[paramIndex - 1] != value.f;
        e.params[paramIndex - 1] = value.f;
        return changed;
    }
    if (paramIndex == prop::kEffectParamColor0) {
        const bool changed = !(e.colors[0] == value.c);
        e.colors[0] = value.c;
        return changed;
    }
    if (paramIndex == prop::kEffectParamColor1) {
        const bool changed = !(e.colors[1] == value.c);
        e.colors[1] = value.c;
        return changed;
    }
    XT_ASSERT(paramIndex == prop::kEffectParamOffset);
    const bool changed = e.params[0] != value.v2.x || e.params[1] != value.v2.y;
    e.params[0] = value.v2.x;
    e.params[1] = value.v2.y;
    return changed;
}

}  // namespace

Value modifierPropertyValue(const ModifierNodeData& m, PropId prop) { return getModifierProperty(m, prop); }

float shapeParamValue(ShapeKind kind, const float* params, PropId prop) {
    const int slot = shapeSlot(kind, prop);
    return slot >= 0 ? params[slot] : 0.0f;
}

namespace {

/// T-243 (32 §32.2.3): the ConstraintNodeData::params index of `prop` for a constraint of `kind`;
/// -1 for mix / enabled (not params) and for IkStrength on a non-Ik constraint.
int constraintParamSlot(ConstraintKind kind, PropId prop) {
    if (prop == prop::IkStrength) {
        return kind == ConstraintKind::Ik ? 0 : -1;
    }
    if (prop >= prop::ConstraintParam0 && prop <= prop::ConstraintParam5) {
        return prop - prop::ConstraintParam0;
    }
    return -1;
}

}  // namespace

float constraintPropertyValue(const ConstraintNodeData& c, PropId prop) {
    if (prop == prop::ConstraintMix) {
        return c.mix;
    }
    if (prop == prop::ConstraintEnabled) {
        return c.enabled != 0 ? 1.0f : 0.0f;
    }
    const int slot = constraintParamSlot(c.kind, prop);
    return slot >= 0 ? c.params[slot] : 0.0f;
}

void setConstraintPropertyValue(ConstraintNodeData& c, PropId prop, float value) {
    if (prop == prop::ConstraintMix) {
        c.mix = value;
        return;
    }
    if (prop == prop::ConstraintEnabled) {
        c.enabled = value >= 0.5f ? 1 : 0;
        return;
    }
    const int slot = constraintParamSlot(c.kind, prop);
    if (slot >= 0) {
        c.params[slot] = value;
    }
}

Value getProperty(const NodeArena& arena, NodeId node, PropId prop) {
    NodeKind kind = arena.kind(node);
    XT_ASSERT(propertyApplies(kind, prop));
    switch (prop) {
        case prop::Opacity:
            return Value::ofFloat(arena.common(node).opacity);
        case prop::AnchorX:
            if (kind == NodeKind::Element) {
                return Value::ofFloat(arena.element(node).cssAnchor.x);
            }
            return Value::ofFloat(arena.common(node).anchor.x);
        case prop::AnchorY:
            if (kind == NodeKind::Element) {
                return Value::ofFloat(arena.element(node).cssAnchor.y);
            }
            return Value::ofFloat(arena.common(node).anchor.y);
        case prop::Anchor:
            if (kind == NodeKind::Element) {
                return Value::ofVec2(arena.element(node).cssAnchor);
            }
            return Value::ofVec2(arena.common(node).anchor);
        case prop::TranslateX:
            if (kind == NodeKind::Element) {
                return Value::ofFloat(arena.element(node).cssTranslate.x);
            }
            return Value::ofFloat(arena.common(node).translate.x);
        case prop::TranslateY:
            if (kind == NodeKind::Element) {
                return Value::ofFloat(arena.element(node).cssTranslate.y);
            }
            return Value::ofFloat(arena.common(node).translate.y);
        case prop::Translate:
            if (kind == NodeKind::Element) {
                return Value::ofVec2(arena.element(node).cssTranslate);
            }
            return Value::ofVec2(arena.common(node).translate);
        case prop::ScaleX:
            return Value::ofFloat(arena.common(node).scale.x);
        case prop::ScaleY:
            return Value::ofFloat(arena.common(node).scale.y);
        case prop::Scale:
            return Value::ofVec2(arena.common(node).scale);
        case prop::Rotation:
            return Value::ofFloat(arena.common(node).rotation);
        case prop::SkewX:
            return Value::ofFloat(arena.common(node).skew.x);
        case prop::SkewY:
            return Value::ofFloat(arena.common(node).skew.y);
        case prop::Visible:
            return Value::ofEnum(arena.common(node).visible);
        case prop::AutoOrient:
            return Value::ofEnum(arena.common(node).autoOrient);
        case prop::TranslateZ:
            return Value::ofFloat(arena.common(node).translateZ);
        case prop::AnchorZ:
            return Value::ofFloat(arena.common(node).anchorZ);
        case prop::RotationX:
            return Value::ofFloat(arena.common(node).rotationX);
        case prop::RotationY:
            return Value::ofFloat(arena.common(node).rotationY);
        case prop::Translate3: {
            const NodeCommon& c = arena.common(node);
            if (kind == NodeKind::Element) {
                const ElementData& e = arena.element(node);
                return Value::ofVec3(Vec3{e.cssTranslate.x, e.cssTranslate.y, c.translateZ});
            }
            return Value::ofVec3(Vec3{c.translate.x, c.translate.y, c.translateZ});
        }
        case prop::Anchor3: {
            const NodeCommon& c = arena.common(node);
            if (kind == NodeKind::Element) {
                const ElementData& e = arena.element(node);
                return Value::ofVec3(Vec3{e.cssAnchor.x, e.cssAnchor.y, c.anchorZ});
            }
            return Value::ofVec3(Vec3{c.anchor.x, c.anchor.y, c.anchorZ});
        }
        case prop::Orientation:
            return Value::ofVec3(arena.common(node).orientation);
        case prop::CameraZoom:
            return Value::ofFloat(arena.camera(node).zoom);
        case prop::CameraTarget:
            return Value::ofVec3(arena.camera(node).target);
        case prop::CameraNear:
            return Value::ofFloat(arena.camera(node).nearZ);
        case prop::CameraFar:
            return Value::ofFloat(arena.camera(node).farZ);
        default:
            break;
    }
    if (isEffectProp(prop)) {
        return getEffectProperty(arena, node, prop);
    }
    if (kind == NodeKind::Path) {
        return getPathProperty(arena.path(node), prop);
    }
    if (kind == NodeKind::Modifier) {
        return getModifierProperty(arena.modifier(node), prop);  // T-225
    }
    if (kind == NodeKind::Bone) {
        return Value::ofFloat(arena.bone(node).length);  // T-243: BoneLength.
    }
    if (kind == NodeKind::Constraint) {
        return Value::ofFloat(constraintPropertyValue(arena.constraint(node), prop));  // T-243
    }
    if (kind == NodeKind::Shape) {
        const ShapeNodeData& s = arena.shape(node);
        if (prop >= prop::ShapeX) {
            const int slot = shapeSlot(s.kind, prop);
            return Value::ofFloat(slot >= 0 ? s.params[slot] : 0.0f);
        }
        return getPathProperty(s, prop);
    }
    if (kind == NodeKind::Element) {
        return arena.element(node).style.base[prop];
    }
    XT_ASSERT(false && "unreachable: not a defined PropId for kind");
    return Value{};
}

namespace {

/// T-211 (05 tail ruling, 06 §6.7): the (px, %) parts of a non-Float value written to an Element's
/// TranslateX/TranslateY. Length Px / Length Percent carry one unit; Vec2 is the mixed (px, %)
/// form a translate transition interpolates in (each part separately, summed at the fold).
bool elementTranslateParts(const Value& value, float& px, float& pct) {
    if (value.type == ValueType::Vec2) {
        px = value.v2.x;
        pct = value.v2.y;
        return true;
    }
    if (value.type == ValueType::Length && value.len.unit == LengthUnit::Px) {
        px = value.len.value;
        pct = 0;
        return true;
    }
    if (value.type == ValueType::Length && value.len.unit == LengthUnit::Percent) {
        px = 0;
        pct = value.len.value;
        return true;
    }
    return false;
}

}  // namespace

bool propertyAcceptsValue(NodeKind kind, PropId prop, ValueType type, LengthUnit unit) {
    if (!propertyDefined(prop)) {
        return false;
    }
    if (type == propertyType(prop)) {
        return true;
    }
    // T-211: what elementTranslateParts() takes.
    return kind == NodeKind::Element && (prop == prop::TranslateX || prop == prop::TranslateY) &&
           (type == ValueType::Vec2 ||
            (type == ValueType::Length && (unit == LengthUnit::Px || unit == LengthUnit::Percent)));
}

void setProperty(NodeArena& arena, NodeId node, PropId prop, const Value& value) {
    NodeKind kind = arena.kind(node);
    XT_ASSERT(propertyApplies(kind, prop));
    if (!propertyAcceptsValue(kind, prop, value)) {
        // T-257b: IrView::validate rejects timeline keyframes the property does not take (the
        // gate); a value that still gets here is ignored, the same in every build.
        return;
    }
    if (kind == NodeKind::Element && (prop == prop::TranslateX || prop == prop::TranslateY) &&
        value.type != ValueType::Float) {
        float px = 0, pct = 0;
        if (!elementTranslateParts(value, px, pct)) {
            return;  // Unreachable: propertyAcceptsValue() admits exactly these forms.
        }
        ElementData& e = arena.element(node);
        if (prop == prop::TranslateX) {
            e.cssTranslate.x = px;
            e.cssTranslatePct.x = pct;
        } else {
            e.cssTranslate.y = px;
            e.cssTranslatePct.y = pct;
        }
        arena.markDirty(node, propertyDirtyBits(prop));
        return;
    }
    switch (prop) {
        case prop::Opacity:
            arena.common(node).opacity = value.f;
            break;
        case prop::AnchorX:
            if (kind == NodeKind::Element) {
                arena.element(node).cssAnchor.x = value.f;
            } else {
                arena.common(node).anchor.x = value.f;
            }
            break;
        case prop::AnchorY:
            if (kind == NodeKind::Element) {
                arena.element(node).cssAnchor.y = value.f;
            } else {
                arena.common(node).anchor.y = value.f;
            }
            break;
        case prop::Anchor:
            if (kind == NodeKind::Element) {
                arena.element(node).cssAnchor = value.v2;
            } else {
                arena.common(node).anchor = value.v2;
            }
            break;
        case prop::TranslateX:
            if (kind == NodeKind::Element) {
                arena.element(node).cssTranslate.x = value.f;
                arena.element(node).cssTranslatePct.x = 0;
            } else {
                arena.common(node).translate.x = value.f;
            }
            break;
        case prop::TranslateY:
            if (kind == NodeKind::Element) {
                arena.element(node).cssTranslate.y = value.f;
                arena.element(node).cssTranslatePct.y = 0;
            } else {
                arena.common(node).translate.y = value.f;
            }
            break;
        case prop::Translate:
            if (kind == NodeKind::Element) {
                arena.element(node).cssTranslate = value.v2;
                arena.element(node).cssTranslatePct = Vec2{0, 0};
            } else {
                arena.common(node).translate = value.v2;
            }
            break;
        case prop::ScaleX:
            arena.common(node).scale.x = value.f;
            break;
        case prop::ScaleY:
            arena.common(node).scale.y = value.f;
            break;
        case prop::Scale:
            arena.common(node).scale = value.v2;
            break;
        case prop::Rotation:
            arena.common(node).rotation = value.f;
            break;
        case prop::SkewX:
            arena.common(node).skew.x = value.f;
            break;
        case prop::SkewY:
            arena.common(node).skew.y = value.f;
            break;
        case prop::Visible:
            arena.common(node).visible = static_cast<uint8_t>(value.e != 0 ? 1 : 0);
            break;
        case prop::AutoOrient: {
            NodeCommon& c = arena.common(node);
            c.autoOrient = static_cast<uint8_t>(value.e == 1 ? 1 : 0);
            if (c.autoOrient == 0) {
                c.orientPathDeg = 0;
            }
            break;
        }
        case prop::TranslateZ:
            arena.common(node).translateZ = value.f;
            break;
        case prop::AnchorZ:
            arena.common(node).anchorZ = value.f;
            break;
        case prop::RotationX:
            arena.common(node).rotationX = value.f;
            break;
        case prop::RotationY:
            arena.common(node).rotationY = value.f;
            break;
        case prop::Translate3: {
            NodeCommon& c = arena.common(node);
            if (kind == NodeKind::Element) {
                ElementData& e = arena.element(node);
                e.cssTranslate.x = value.v3.x;
                e.cssTranslate.y = value.v3.y;
                e.cssTranslatePct = Vec2{0, 0};
            } else {
                c.translate.x = value.v3.x;
                c.translate.y = value.v3.y;
            }
            c.translateZ = value.v3.z;
            break;
        }
        case prop::Anchor3: {
            NodeCommon& c = arena.common(node);
            if (kind == NodeKind::Element) {
                ElementData& e = arena.element(node);
                e.cssAnchor.x = value.v3.x;
                e.cssAnchor.y = value.v3.y;
            } else {
                c.anchor.x = value.v3.x;
                c.anchor.y = value.v3.y;
            }
            c.anchorZ = value.v3.z;
            break;
        }
        case prop::Orientation:
            arena.common(node).orientation = value.v3;
            break;
        case prop::CameraZoom:
            arena.camera(node).zoom = value.f;
            break;
        case prop::CameraTarget:
            arena.camera(node).target = value.v3;
            break;
        case prop::CameraNear:
            arena.camera(node).nearZ = value.f;
            break;
        case prop::CameraFar:
            arena.camera(node).farZ = value.f;
            break;
        default:
            if (isEffectProp(prop)) {
                if (!setEffectProperty(arena, node, prop, value)) {
                    return;  // T-545: unchanged (or missing effect) -> no dirty.
                }
            } else if (kind == NodeKind::Path) {
                setPathProperty(arena.path(node), prop, value);
            } else if (kind == NodeKind::Modifier) {
                setModifierProperty(arena.modifier(node), prop, value);  // T-225
            } else if (kind == NodeKind::Bone) {
                arena.bone(node).length = value.f;  // T-243: BoneLength.
            } else if (kind == NodeKind::Constraint) {
                setConstraintPropertyValue(arena.constraint(node), prop, value.f);  // T-243
            } else if (kind == NodeKind::Shape) {
                ShapeNodeData& s = arena.shape(node);
                if (prop >= prop::ShapeX) {
                    const int slot = shapeSlot(s.kind, prop);
                    if (slot >= 0) {
                        s.params[slot] = value.f;
                    }
                } else {
                    setPathProperty(s, prop, value);
                }
            } else if (kind == NodeKind::Element) {
                ComputedStyle& style = arena.element(node).style;
                style.base[prop] = value;
                // T-333 (05 §5.9.8): a side color written here (an animation, a transition, a
                // script) is the side's own until the next cascade commit recomputes the set.
                if (prop >= prop::BorderTopColor && prop <= prop::BorderLeftColor) {
                    style.borderColorSides =
                        static_cast<uint8_t>(style.borderColorSides | (1u << (prop - prop::BorderTopColor)));
                }
            } else {
                XT_ASSERT(false && "unreachable: not a defined PropId for kind");
            }
            break;
    }
    arena.markDirty(node, propertyDirtyBits(prop));
    // Docs/tasks/T-43b.md: a camera property change must also force its parent (the 3D space
    // root)'s subtree to recompute (kDirtyTransform only propagates to descendants, not ancestors,
    // so this needs an explicit extra markDirty on the parent).
    if (kind == NodeKind::Camera && prop >= prop::CameraZoom && prop <= prop::CameraFar) {
        NodeId parent = arena.parent(node);
        if (parent != kInvalidNode) {
            arena.markDirty(parent, static_cast<uint8_t>(kDirtyTransform));
        }
    }
}

}  // namespace xt
