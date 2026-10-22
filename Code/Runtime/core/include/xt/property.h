#pragma once

#include <cstdint>
#include <string_view>

#include "xt/types.h"
#include "xt/value.h"

/// PropId constants (Docs/design/03-scene-and-properties.md §3.9) and the property metadata
/// queries built on top of them: value type, applicability, dirty bits, name lookup, and
/// ComputedStyle default initialization. See Docs/tasks/T-05.md.

namespace xt {

/// Forward declaration only; fully defined in xt/node.h. propertyApplies() only needs the type,
/// not NodeKind's enumerators (node.h includes this header before defining NodeKind).
enum class NodeKind : uint8_t;

/// Forward declaration only; fully defined in xt/node.h.
struct ComputedStyle;

/// Forward declaration only; fully defined in xt/node_arena.h. Including node_arena.h here would
/// be circular (node_arena.h includes node.h, which includes this header).
class NodeArena;

namespace prop {

// General (PropId 0-13). Applies to every non-Free NodeKind.
inline constexpr PropId Opacity = 0;     ///< Node opacity, [0, 1].
inline constexpr PropId AnchorX = 1;     ///< Transform anchor point X, in the node's local units.
inline constexpr PropId AnchorY = 2;     ///< Transform anchor point Y, in the node's local units.
inline constexpr PropId TranslateX = 3;  ///< Translation X, in the node's local units.
inline constexpr PropId TranslateY = 4;  ///< Translation Y, in the node's local units.
inline constexpr PropId ScaleX = 5;      ///< Scale factor on the local X axis.
inline constexpr PropId ScaleY = 6;      ///< Scale factor on the local Y axis.
inline constexpr PropId Rotation = 7;    ///< Rotation in degrees.
inline constexpr PropId SkewX = 8;       ///< Skew angle on the local X axis, in degrees.
inline constexpr PropId SkewY = 9;       ///< Skew angle on the local Y axis, in degrees.
inline constexpr PropId Anchor = 10;     ///< Combined (AnchorX, AnchorY) as a Vec2 value.
inline constexpr PropId Translate = 11;  ///< Combined (TranslateX, TranslateY) as a Vec2 value.
inline constexpr PropId Scale = 12;      ///< Combined (ScaleX, ScaleY) as a Vec2 value.
inline constexpr PropId Visible = 13;    ///< Whether the node is rendered.
/// T-228 (31 §31.7.2): Enum{none = 0, path = 1}; `path` adds the tangent angle of the node's Vec2
/// Translate track (or TranslateX/TranslateY pair) to its rotation.
inline constexpr PropId AutoOrient = 14;

// 3D (PropId 16-22, gap at 15/23; Docs/design/19-3d-space.md §19.3). Applies to every
// non-Free NodeKind (Camera included, for Translate3/Anchor3/Orientation).
inline constexpr PropId TranslateZ = 16;   ///< Z translation, in the node's local units.
inline constexpr PropId AnchorZ = 17;      ///< Transform anchor point Z, in the node's local units.
inline constexpr PropId RotationX = 18;    ///< Rotation around the local X axis, in degrees.
inline constexpr PropId RotationY = 19;    ///< Rotation around the local Y axis, in degrees.
inline constexpr PropId Translate3 = 20;   ///< Combined (TranslateX, TranslateY, TranslateZ) as Vec3.
inline constexpr PropId Anchor3 = 21;      ///< Combined (AnchorX, AnchorY, AnchorZ) as Vec3.
inline constexpr PropId Orientation = 22;  ///< AE-style free-axis orientation, degrees (Vec3).
// 23 reserved.

// Camera (PropId 24-27, gap at 28-31). Applies only to NodeKind::Camera.
inline constexpr PropId CameraZoom = 24;    ///< Focal length, in px (19.4).
inline constexpr PropId CameraTarget = 25;  ///< Look-at target point, in the root's coordinates.
inline constexpr PropId CameraNear = 26;    ///< Near clip plane.
inline constexpr PropId CameraFar = 27;     ///< Far clip plane.
// 28-31 reserved.

inline constexpr PropId kGenericPropCount = 32;  ///< One past the general PropId range (0-31).

// Path (PropId 32-51). Applies only to NodeKind::Path (and, but for PathData, NodeKind::Shape).
inline constexpr PropId PathData = 32;             ///< The path's geometry (Value::Path).
inline constexpr PropId FillColor = 33;            ///< Fill paint color.
inline constexpr PropId FillOpacity = 34;          ///< Fill paint opacity, [0, 1].
inline constexpr PropId StrokeColor = 35;          ///< Stroke paint color.
inline constexpr PropId StrokeOpacity = 36;        ///< Stroke paint opacity, [0, 1].
inline constexpr PropId StrokeWidth = 37;          ///< Stroke width, in local units.
inline constexpr PropId TrimStart = 38;            ///< Path trim start, as a fraction of path length.
inline constexpr PropId TrimEnd = 39;              ///< Path trim end, as a fraction of path length.
inline constexpr PropId TrimOffset = 40;           ///< Path trim offset, as a fraction of path length.
inline constexpr PropId FillGradientP0 = 41;       ///< Fill gradient start point.
inline constexpr PropId FillGradientP1 = 42;       ///< Fill gradient end point.
inline constexpr PropId FillGradientStops = 43;    ///< Fill gradient color stops.
inline constexpr PropId StrokeGradientP0 = 44;     ///< Stroke gradient start point.
inline constexpr PropId StrokeGradientP1 = 45;     ///< Stroke gradient end point.
inline constexpr PropId StrokeGradientStops = 46;  ///< Stroke gradient color stops.
inline constexpr PropId DashOffset = 47;           ///< Dash pattern phase offset.
inline constexpr PropId DashArray = 48;            ///< Dash pattern segment lengths.
inline constexpr PropId MiterLimit = 49;           ///< Stroke miter limit.
inline constexpr PropId StrokeTaperStart = 50;     ///< T-233: start taper, fraction of arc length.
inline constexpr PropId StrokeTaperEnd = 51;       ///< T-233: end taper, fraction of arc length.

// Element (PropId 64-109, with a gap at 94-95). Applies only to NodeKind::Element.
inline constexpr PropId Width = 64;                ///< Box width.
inline constexpr PropId Height = 65;               ///< Box height.
inline constexpr PropId MinWidth = 66;             ///< Minimum box width.
inline constexpr PropId MaxWidth = 67;             ///< Maximum box width.
inline constexpr PropId MinHeight = 68;            ///< Minimum box height.
inline constexpr PropId MaxHeight = 69;            ///< Maximum box height.
inline constexpr PropId MarginTop = 70;            ///< Top margin.
inline constexpr PropId MarginRight = 71;          ///< Right margin.
inline constexpr PropId MarginBottom = 72;         ///< Bottom margin.
inline constexpr PropId MarginLeft = 73;           ///< Left margin.
inline constexpr PropId PaddingTop = 74;           ///< Top padding.
inline constexpr PropId PaddingRight = 75;         ///< Right padding.
inline constexpr PropId PaddingBottom = 76;        ///< Bottom padding.
inline constexpr PropId PaddingLeft = 77;          ///< Left padding.
inline constexpr PropId BorderTopWidth = 78;       ///< Top border width.
inline constexpr PropId BorderRightWidth = 79;     ///< Right border width.
inline constexpr PropId BorderBottomWidth = 80;    ///< Bottom border width.
inline constexpr PropId BorderLeftWidth = 81;      ///< Left border width.
inline constexpr PropId Left = 82;                 ///< `left` offset for position: absolute/relative.
inline constexpr PropId Top = 83;                  ///< `top` offset for position: absolute/relative.
inline constexpr PropId Right = 84;                ///< `right` offset for position: absolute/relative.
inline constexpr PropId Bottom = 85;               ///< `bottom` offset for position: absolute/relative.
inline constexpr PropId FlexGrow = 86;             ///< `flex-grow` weight.
inline constexpr PropId FlexShrink = 87;           ///< `flex-shrink` weight.
inline constexpr PropId FlexBasis = 88;            ///< `flex-basis` length.
inline constexpr PropId RowGap = 89;               ///< Flex/grid row gap.
inline constexpr PropId ColumnGap = 90;            ///< Flex/grid column gap.
inline constexpr PropId FontSize = 91;             ///< Font size.
inline constexpr PropId LineHeight = 92;           ///< Line height.
inline constexpr PropId LetterSpacing = 93;        ///< Additional space between glyphs.
inline constexpr PropId BackgroundColor = 96;      ///< Element background color.
inline constexpr PropId BorderColor = 97;          ///< Element border color.
inline constexpr PropId BorderRadiusTL = 98;       ///< Top-left border radius.
inline constexpr PropId BorderRadiusTR = 99;       ///< Top-right border radius.
inline constexpr PropId BorderRadiusBR = 100;      ///< Bottom-right border radius.
inline constexpr PropId BorderRadiusBL = 101;      ///< Bottom-left border radius.
inline constexpr PropId Color = 102;               ///< Text color.
inline constexpr PropId ImageOpacity = 103;        ///< Image opacity, [0, 1].
inline constexpr PropId ScrollX = 104;             ///< Horizontal scroll offset.
inline constexpr PropId ScrollY = 105;             ///< Vertical scroll offset.
inline constexpr PropId Perspective = 106;         ///< CSS `perspective`; Auto = `none` (19.4/19.10).
inline constexpr PropId PerspectiveOriginX = 107;  ///< CSS `perspective-origin` X (19.4/19.10).
inline constexpr PropId PerspectiveOriginY = 108;  ///< CSS `perspective-origin` Y (19.4/19.10).
/// CSS `text-reveal` (03 §3.12.6, 07 §7.8, T-178): Float 0..1, the fraction of the element's text
/// grapheme clusters that is drawn (typewriter). Initial value 1; not inherited; paint-only.
inline constexpr PropId TextReveal = 109;

// Particles (PropId 110-128; Docs/design/03-scene-and-properties.md §3.12.5, Docs/tasks/T-202.md).
// The numeric fields of a `<particles>` element, cascaded like any other Element paint property
// (`.xts` `particle-*`, `@timeline`, `bind:`); the element's NodeKind::Particles child mirrors them
// into its ParticleNodeData at every simulation step. Appended at the tail (T-202).
inline constexpr PropId ParticleRate = 110;         ///< Particles emitted per second (Float).
inline constexpr PropId ParticleBurst = 111;        ///< Particles emitted at once on play (Float).
inline constexpr PropId ParticleLifetimeMin = 112;  ///< Lifetime range start, seconds (Float).
inline constexpr PropId ParticleLifetimeMax = 113;  ///< Lifetime range end, seconds (Float).
inline constexpr PropId ParticleSpeedMin = 114;     ///< Initial speed range start, px/s (Float).
inline constexpr PropId ParticleSpeedMax = 115;     ///< Initial speed range end, px/s (Float).
inline constexpr PropId ParticleDirection = 116;    ///< Emission direction, degrees, -90 = up (Float).
inline constexpr PropId ParticleSpread = 117;       ///< Emission cone, degrees (Float).
inline constexpr PropId ParticleGravity = 118;      ///< Downward acceleration, px/s^2 (Float).
inline constexpr PropId ParticleDrag = 119;         ///< Velocity damping per second (Float).
inline constexpr PropId ParticleSizeStart = 120;    ///< Size at birth, px (Float).
inline constexpr PropId ParticleSizeEnd = 121;      ///< Size at death, px (Float).
/// Color over the particle's life as 4 evenly spaced stops (t = 0, 1/3, 2/3, 1); a two-color
/// `color="<a> <b>"` compiles to the stops of the straight a->b line (Color).
inline constexpr PropId ParticleColor0 = 122;
inline constexpr PropId ParticleColor1 = 123;       ///< See ParticleColor0.
inline constexpr PropId ParticleColor2 = 124;       ///< See ParticleColor0.
inline constexpr PropId ParticleColor3 = 125;       ///< See ParticleColor0.
inline constexpr PropId ParticleRotationMin = 126;  ///< Initial rotation range start, degrees (Float).
inline constexpr PropId ParticleRotationMax = 127;  ///< Initial rotation range end, degrees (Float).
inline constexpr PropId ParticleMax = 128;          ///< Live particle cap (Float; default 256).

// Outline (PropId 129-131; Docs/design/05-style-and-css.md §5.9.8, T-325): the `outline-*` focus
// ring, painted after the border outside the border box; paint-only (no layout, hit test or
// overflow effect). `outline-style` is StyleProp 201 (not animatable).
inline constexpr PropId OutlineWidth = 129;   ///< `outline-width` (Length, initial 0).
inline constexpr PropId OutlineColor = 130;   ///< `outline-color` (Color, initial = `color`).
inline constexpr PropId OutlineOffset = 131;  ///< `outline-offset` (Length, initial 0).

// Text decoration (PropId 132-133; Docs/design/05-style-and-css.md §5.9.8, T-324): the line's
// color and thickness; `text-decoration-line` is StyleProp 203 (not animatable). Drawn by the
// Painter's text branch as FillRects in the glyph layer, never through the effect chain.
inline constexpr PropId TextDecorationColor = 132;  ///< `text-decoration-color` (Color, initial = `color`).
/// `text-decoration-thickness` (Length; initial `auto` = LengthUnit::Auto, max(1px, 1/14 em)).
inline constexpr PropId TextDecorationThickness = 133;

// Background image placement (PropId 134-137; Docs/design/05-style-and-css.md §5.9.8, T-327).
// Read only when `background-repeat` (StyleProp 205) is not `stretch`. BackgroundSizeX/Y: a
// Length per axis, LengthUnit::Auto = the image's own size on that axis (aspect kept when only one
// axis is auto); the keywords `cover` / `contain` are LengthUnit::Auto with value 1 / 2 on both
// axes (Auto lengths never interpolate, so a keyword switches discretely). BackgroundPositionX/Y:
// a Length per axis, a percentage taken of (padding box - image size) as CSS defines it.
inline constexpr PropId BackgroundSizeX = 134;      ///< `background-size` horizontal (initial `auto`).
inline constexpr PropId BackgroundSizeY = 135;      ///< `background-size` vertical (initial `auto`).
inline constexpr PropId BackgroundPositionX = 136;  ///< `background-position` horizontal (initial 0%).
inline constexpr PropId BackgroundPositionY = 137;  ///< `background-position` vertical (initial 0%).

// Aspect ratio (PropId 138; Docs/design/06-layout.md §6.16, T-328): Float width / height, 0 =
// `auto` (a replaced element keeps its natural ratio, anything else has none); layout-affecting.
inline constexpr PropId AspectRatio = 138;  ///< `aspect-ratio` (Float w/h, initial 0 = auto).

// Per-side border colors (PropId 139-142; Docs/design/05-style-and-css.md §5.9.8, T-333): Color,
// paint-only. A side no rule declares follows the legacy single BorderColor (97), which stays the
// `border-color` one-value form; ComputedStyle::borderColorSides says which sides are their own.
inline constexpr PropId BorderTopColor = 139;     ///< `border-top-color` (initial = `border-color`).
inline constexpr PropId BorderRightColor = 140;   ///< `border-right-color`.
inline constexpr PropId BorderBottomColor = 141;  ///< `border-bottom-color`.
inline constexpr PropId BorderLeftColor = 142;    ///< `border-left-color`.

// Flex item order (PropId 143; Docs/design/05-style-and-css.md §5.9.8, 06 §6.9, T-336): Float
// holding an integer, initial 0; layout-affecting, not animatable (no transition / @keyframes).
inline constexpr PropId Order = 143;  ///< `order` (Float integer, initial 0).

inline constexpr PropId kElementPropCount = 144;  ///< One past the highest defined Element PropId.

// Shape modifiers (PropId 208-231; Docs/design/31-vector-authoring.md §31.6.1,
// T-225). Apply only to NodeKind::Modifier; each maps onto ModifierNodeData::params by the
// modifier's kind (a prop the kind does not use reads 0 and ignores writes). All Float except the
// three Vec2 props. PropId 160-207 stay unused: the compiler's StyleProp numbers (160-200,
// Code/Runtime/compile/src/xts/xts_props.h) share IrDecl's number space (T-225b).
inline constexpr PropId ModTrimStart = 208;      ///< Trim params[0], 0..1.
inline constexpr PropId ModTrimEnd = 209;        ///< Trim params[1], 0..1.
inline constexpr PropId ModTrimOffset = 210;     ///< Trim params[2], degrees.
inline constexpr PropId ModCopies = 211;         ///< Repeat params[0] (rounded when used).
inline constexpr PropId ModOffset = 212;         ///< Repeat params[1].
inline constexpr PropId ModAnchor = 213;         ///< Repeat params[2..3] (Vec2).
inline constexpr PropId ModTranslate = 214;      ///< Repeat params[4..5] (Vec2).
inline constexpr PropId ModRotation = 215;       ///< Repeat params[6], degrees.
inline constexpr PropId ModScale = 216;          ///< Repeat params[7] (uniform).
inline constexpr PropId ModStartOpacity = 217;   ///< Repeat opacityA.
inline constexpr PropId ModEndOpacity = 218;     ///< Repeat opacityB.
inline constexpr PropId ModRadius = 219;         ///< Round params[0].
inline constexpr PropId ModAmount = 220;         ///< Pucker (and baked Offset) params[0].
inline constexpr PropId ModSize = 221;           ///< ZigZag / Wiggle params[0].
inline constexpr PropId ModRidges = 222;         ///< ZigZag params[1].
inline constexpr PropId ModAngle = 223;          ///< Twist params[0], degrees.
inline constexpr PropId ModCenter = 224;         ///< Twist params[1..2] (Vec2).
inline constexpr PropId ModDetail = 225;         ///< Wiggle params[1].
inline constexpr PropId ModSeed = 226;           ///< Wiggle params[2].
inline constexpr PropId ModSpeed = 227;          ///< Wiggle params[3].
inline constexpr PropId ModCorrelation = 228;    ///< Wiggle params[4].
inline constexpr PropId ModTemporalPhase = 229;  ///< Wiggle params[5], degrees.
inline constexpr PropId ModSpatialPhase = 230;   ///< Wiggle params[6], degrees.
inline constexpr PropId ModEnabled = 231;        ///< ModifierNodeData::enabled (Float 0/1).

// Parametric shapes (PropId 232-245; §31.6.1, T-225). Apply only to
// NodeKind::Shape; each maps onto ShapeNodeData::params by the shape's kind. All Float.
inline constexpr PropId ShapeX = 232;         ///< Rect params[0].
inline constexpr PropId ShapeY = 233;         ///< Rect params[1].
inline constexpr PropId ShapeW = 234;         ///< Rect params[2].
inline constexpr PropId ShapeH = 235;         ///< Rect params[3].
inline constexpr PropId ShapeRx = 236;        ///< Rect params[4], Ellipse params[2].
inline constexpr PropId ShapeRy = 237;        ///< Rect params[5], Ellipse params[3].
inline constexpr PropId ShapeCx = 238;        ///< Ellipse / Star params[0].
inline constexpr PropId ShapeCy = 239;        ///< Ellipse / Star params[1].
inline constexpr PropId ShapeR = 240;         ///< Star params[3].
inline constexpr PropId ShapeR2 = 241;        ///< Star params[4].
inline constexpr PropId ShapePoints = 242;    ///< Star params[2] (rounded when used).
inline constexpr PropId ShapeRotation = 243;  ///< Star params[5], degrees.
inline constexpr PropId ShapeRound = 244;     ///< Star params[6], 0..1.
inline constexpr PropId ShapeRound2 = 245;    ///< Star params[7], 0..1.

// Rig (PropId 246-255; Docs/design/32-rigging-and-deformation.md §32.2.1/§32.2.3, T-243). All
// Float. BoneLength applies only to NodeKind::Bone, the rest only to NodeKind::Constraint (a
// parameter the constraint's kind does not use reads 0 and ignores writes).
inline constexpr PropId BoneLength = 246;         ///< BoneNodeData::length.
inline constexpr PropId IkStrength = 247;         ///< Ik params[0] (alias of ConstraintParam0 on Ik).
inline constexpr PropId ConstraintMix = 248;      ///< ConstraintNodeData::mix.
inline constexpr PropId ConstraintParam0 = 249;   ///< ConstraintNodeData::params[0].
inline constexpr PropId ConstraintParam1 = 250;   ///< params[1].
inline constexpr PropId ConstraintParam2 = 251;   ///< params[2].
inline constexpr PropId ConstraintParam3 = 252;   ///< params[3].
inline constexpr PropId ConstraintParam4 = 253;   ///< params[4].
inline constexpr PropId ConstraintParam5 = 254;   ///< params[5].
inline constexpr PropId ConstraintEnabled = 255;  ///< ConstraintNodeData::enabled (Float 0/1).
inline constexpr PropId kInvalidProp = 0xFFFF;    ///< Sentinel value for "no such property".

// Effect parameters (Docs/design/18-effects.md §18.2). A dynamic, per-effect-slot PropId range
// (0x8000+), not a fixed list like the ranges above: see effectParam()/isEffectProp() below.
inline constexpr PropId kEffectPropBase = 0x8000;  ///< Base of the effect-parameter PropId range.

/// paramIndex constants for effectParam()'s second argument (18.2). paramIndex 1-8 (EffectInstance
/// ::params[0..7]) have no named constants here; kind-specific meaning comes from 18.3's table.
inline constexpr uint32_t kEffectParamEnabled = 0;  ///< EffectInstance::enabled (Float 0/1).
inline constexpr uint32_t kEffectParamColor0 = 16;  ///< EffectInstance::colors[0].
inline constexpr uint32_t kEffectParamColor1 = 17;  ///< EffectInstance::colors[1].
inline constexpr uint32_t kEffectParamOffset = 18;  ///< Composite Vec2 (params[0], params[1]).

}  // namespace prop

/// Encodes an effect-parameter PropId (Docs/design/18-effects.md §18.2): effectIndex is the
/// effect's position in the node's effect stack (0-15), paramIndex selects which field (0 =
/// enabled, 1-8 = params[0..7], 16/17 = colors[0/1], 18 = the composite Vec2 offset from
/// params[0..1]; see the prop::kEffectParam* constants). Both arguments are masked to their
/// documented widths (4 and 8 bits) before encoding; callers must stay within 0-15/0-31 as
/// propertyDefined() requires -- this function does not itself validate the range.
constexpr PropId effectParam(uint32_t effectIndex, uint32_t paramIndex) {
    return static_cast<PropId>(prop::kEffectPropBase | ((effectIndex & 0xFu) << 8) | (paramIndex & 0xFFu));
}

/// True if prop falls in the effect-parameter PropId range (prop >= kEffectPropBase), regardless
/// of whether the encoded effectIndex/paramIndex is one of the ones propertyDefined() accepts.
inline bool isEffectProp(PropId prop) { return prop >= prop::kEffectPropBase; }

/// Value type of a defined PropId. XT_ASSERT(propertyDefined(prop)) internally; call
/// propertyDefined() first when prop may be undefined.
ValueType propertyType(PropId prop);

/// True if prop is one of the defined PropId ranges: general 0-13, 3D 16-22, Camera 24-27, Path
/// 32-51, Element 64-93/96-128, Modifier 208-231, Shape 232-245, or an effect parameter (18.2:
/// prop >= kEffectPropBase, decoded effectIndex <= 15 and decoded paramIndex in {0, 1..8, 16, 17,
/// 18}).
bool propertyDefined(PropId prop);

/// True if kind owns prop: general (0-13) and 3D (16-22) props apply to every NodeKind other than
/// Free and Modifier, Camera props (24-27) only to NodeKind::Camera, Path props (32-51) only to
/// NodeKind::Path (33-51 also to NodeKind::Shape), Element props (64-128) only to
/// NodeKind::Element, Modifier props (208-231) only to NodeKind::Modifier (which also takes
/// Opacity and Visible, and nothing else), Shape props (232-245) only to NodeKind::Shape, effect
/// parameters (18.2) to every drawable kind (everything but Free, Camera and Modifier) regardless
/// of the node's actual effectCount. False for an undefined prop.
bool propertyApplies(NodeKind kind, PropId prop);

/// DirtyBits raised by writing prop, per the L/T/P column of Docs/design/03-scene-and-properties.md
/// §3.9: L -> kDirtyLayout, T -> kDirtyTransform, P -> kDirtyPaint. XT_ASSERT(propertyDefined(prop))
/// internally.
uint8_t propertyDirtyBits(PropId prop);

/// PropId name, e.g. "Opacity". Returns "?" for an undefined prop. For an effect-parameter PropId
/// (18.2), this context-free overload cannot know the effect's actual EffectKind, so it always
/// returns the generic "effect[<i>].param<p>" form -- use the (arena, node, prop) overload below
/// for the fully-resolved 18.3 name.
const char* propertyName(PropId prop);

/// Effect-aware property name (Docs/design/18-effects.md §18.2-18.3): for a PropId in the effect
/// range, looks up the actual EffectKind at (arena, node)'s effectIndex to resolve the per-kind
/// parameter name from 18.3's table, e.g. "effect[0].blur" for DropShadow's paramIndex 3. Falls
/// back to propertyName(prop)'s generic "effect[<i>].param<p>" form when effectIndex is out of
/// range for `node`, or when that kind does not define paramIndex. For a non-effect (or
/// undefined) PropId, behaves exactly like propertyName(prop).
const char* propertyName(const NodeArena& arena, NodeId node, PropId prop);

/// Linear search by name. Returns prop::kInvalidProp if not found.
PropId propertyByName(std::string_view name);

/// Initializes style.base[64..128] (the Element PropId range) to their Docs/design/03 and 05
/// initial values, with each entry's type set to propertyType(i). style.base[0..13] and the
/// undefined 94/95 gap are left default-constructed (unused; see ComputedStyle in xt/node.h).
void initComputedStyleDefaults(ComputedStyle& style);

/// Reads (node, prop)'s current value. XT_ASSERT(propertyApplies(arena.kind(node), prop))
/// internally. See Docs/design/03-scene-and-properties.md §3.11.
Value getProperty(const NodeArena& arena, NodeId node, PropId prop);

/// Writes value into (node, prop) and marks the node dirty with propertyDirtyBits(prop). The only
/// path Animator, StyleEngine and the public API may use to write node properties.
/// XT_ASSERT(propertyApplies(arena.kind(node), prop)) and XT_ASSERT(value.type == propertyType(prop))
/// internally. T-211 exception: an Element's TranslateX/TranslateY also accepts a Length (px or %
/// of its own border box) and a Vec2 (px, %) -- the mixed form translate transitions interpolate
/// in; a Float writes px and clears the % part.
void setProperty(NodeArena& arena, NodeId node, PropId prop, const Value& value);

}  // namespace xt
