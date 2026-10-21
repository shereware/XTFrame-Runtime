#pragma once

#include <cstdint>
#include <string_view>

#include "xt/compile/style_model.h"
#include "xt/node.h"  // ConstraintKind (T-243).
#include "xt/property.h"

/// CSS name <-> PropId/StyleProp table, enum value tables, initial values and the `inherited`
/// flag. See docs/design/05-style-and-css.md §5.4, docs/tasks/T-22.md.

namespace xt::compile {

/// StyleProp numbers, docs/design/05-style-and-css.md §5.4. Not animatable (PropId 0-159 covers
/// the animatable range instead; see core/include/xt/property.h).
namespace styleprop {
inline constexpr uint16_t Display = 160;
inline constexpr uint16_t Position = 161;
inline constexpr uint16_t FlexDirection = 162;
inline constexpr uint16_t FlexWrap = 163;
inline constexpr uint16_t JustifyContent = 164;
inline constexpr uint16_t AlignItems = 165;
inline constexpr uint16_t AlignSelf = 166;
inline constexpr uint16_t AlignContent = 167;
inline constexpr uint16_t Overflow = 168;
inline constexpr uint16_t ZIndex = 169;
inline constexpr uint16_t Visibility = 170;
inline constexpr uint16_t PointerEvents = 171;
inline constexpr uint16_t FontFamily = 172;
inline constexpr uint16_t FontWeight = 173;
inline constexpr uint16_t FontStyle = 174;
inline constexpr uint16_t TextAlign = 175;
inline constexpr uint16_t WhiteSpace = 176;
inline constexpr uint16_t TextOverflow = 177;
inline constexpr uint16_t Cursor = 178;
inline constexpr uint16_t ObjectFit = 179;
inline constexpr uint16_t Transition = 180;
inline constexpr uint16_t Animation = 181;
inline constexpr uint16_t BackgroundImage = 182;
inline constexpr uint16_t Direction = 183;
inline constexpr uint16_t WritingMode = 184;
inline constexpr uint16_t TextOrientation = 185;
inline constexpr uint16_t Lang = 186;
inline constexpr uint16_t Effect = 187;              ///< 18-effects.md §18.4 (T-51a).
inline constexpr uint16_t BackfaceVisibility = 188;  ///< 05.5/19.10 (T-47a).
inline constexpr uint16_t TextRendering = 189;       ///< 23.4/T-101; auto | geometricPrecision.
inline constexpr uint16_t BackgroundGradient = 190;  ///< 05.5c (T-115); ListRef-valued.
inline constexpr uint16_t PointerWatch = 191;        ///< 27.2.5/08.9 (T-174); false | true.
inline constexpr uint16_t TextEffect = 192;          ///< 07.8 (T-178); ListRef into STYL effects[].
inline constexpr uint16_t ClipStart = 193;           ///< 28.2/04.10.2 (T-191); seconds (`0.2s`).
inline constexpr uint16_t ClipRate = 194;            ///< 28.2/04.10.2 (T-191); a number.
inline constexpr uint16_t FocusNav = 195;            ///< 08.9 (T-194); auto | explicit | none.
inline constexpr uint16_t Layer = 196;               ///< 08.9 (T-194); normal | overlay.
/// T-199 (05 §5.9.2, 26 §26.3.4): `border-image` longhands; encodings in core/src/style_decode.h.
inline constexpr uint16_t BorderImageSource = 197;  ///< StringRef: asset name or "none".
inline constexpr uint16_t BorderImageSlice =
    198;  ///< 4 x f32 (Color-encoded): px, -fraction = %, NaN = auto.
inline constexpr uint16_t BorderImageRepeat = 199;  ///< stretch | repeat | round.
inline constexpr uint16_t BorderImageFill = 200;    ///< false | true.
inline constexpr uint16_t OutlineStyle = 201;       ///< 05 §5.9.8 (T-325); none | solid.
inline constexpr uint16_t TextTransform = 202;      ///< 05 §5.9.8 (T-323); inherited.
/// 05 §5.9.8 (T-324): none | [underline || overline || line-through], a set of bits 1/2/4
/// (xt::kTextDecoration*).
inline constexpr uint16_t TextDecorationLine = 203;
inline constexpr uint16_t MixBlendMode = 204;  ///< 18 §18.12 (T-326); a BlendMode value.
/// 05 §5.9.8 (T-327): stretch | no-repeat | repeat | repeat-x | repeat-y (xt::kBackgroundRepeat*).
inline constexpr uint16_t BackgroundRepeat = 205;
inline constexpr uint16_t ImageRendering = 206;  ///< 05 §5.9.8 (T-327); auto | pixelated; inherited.
/// 05 §5.9.8 (T-332): `none | inset() | circle() | ellipse() | polygon()`; Enum = 0 or 1 + the
/// `CLIP` chunk shape index; not inherited, not animatable.
inline constexpr uint16_t ClipPath = 207;
/// 05 §5.9.8 (T-331): none | <integer >= 1> (alias `-webkit-line-clamp`); Enum payload = the
/// count, 0 = none.
inline constexpr uint16_t LineClamp = 256;  ///< The first StyleProp past the rig PropIds.
/// 05 §5.9.8 (T-331): normal | tabular-nums; inherited.
inline constexpr uint16_t FontVariantNumeric = 257;
/// 05 §5.9.8 (T-333): `border-top/right/bottom/left-style` (`border-style` 1-4 values expands to
/// them): none | solid | dashed | dotted (xt::kBorderStyle*), initial solid; not inherited.
inline constexpr uint16_t BorderTopStyle = 258;
inline constexpr uint16_t BorderRightStyle = 259;
inline constexpr uint16_t BorderBottomStyle = 260;
inline constexpr uint16_t BorderLeftStyle = 261;
/// 05 §5.9.8 (T-336): `overflow-x` / `overflow-y` (visible | hidden | scroll | auto); `overflow`
/// with one value stays StyleProp 168 (both axes), with two values expands to these.
inline constexpr uint16_t OverflowX = 262;
inline constexpr uint16_t OverflowY = 263;
/// 06 §6.15 (T-338): grid. `grid-template-columns` / `-rows` (GridTracks: Enum 0 = none, else 1 +
/// the IR `GRID` track-list index), `grid-template-areas` (GridAreas: likewise into the area
/// lists), `justify-items` (AlignItems numbering), the four line longhands (Enum, xt::kGridLine*)
/// and `grid-area: <name> | auto` (String; `auto` = ""). None inherited, none animatable.
inline constexpr uint16_t GridTemplateColumns = 264;
inline constexpr uint16_t GridTemplateRows = 265;
inline constexpr uint16_t GridTemplateAreas = 266;
inline constexpr uint16_t JustifyItems = 267;
inline constexpr uint16_t GridColumnStart = 268;
inline constexpr uint16_t GridColumnEnd = 269;
inline constexpr uint16_t GridRowStart = 270;
inline constexpr uint16_t GridRowEnd = 271;
inline constexpr uint16_t GridArea = 272;
/// 05 §5.9.9 (T-345): `ui-scale: auto | none | clamp(<min>, <max>)`, four f32 encoded like a
/// Color value: (mode 0 auto / 1 none / 2 clamp, min, max, 0); inherited, not animatable.
inline constexpr uint16_t UiScale = 273;
/// 05 §5.9.9 (T-345): `media-override: auto | show | hide` (Enum 0 / 1 / 2); inherited, not
/// animatable.
inline constexpr uint16_t MediaOverride = 274;
/// 05 §5.9.10 (T-376): `app-region: none | drag | no-drag` (Enum 0 / 1 / 2); inherited, not
/// animatable.
inline constexpr uint16_t AppRegion = 275;
/// 05 §5.9.11 (T-386): `hit-slop: <length>{1,4}` (margin order), four f32 encoded like a Color
/// value (top, right, bottom, left; px, `%` and `em` per xt::kHitSlopEmBias); not inherited, not
/// animatable.
inline constexpr uint16_t HitSlop = 276;
/// StyleProps share IrDecl's number space with the animatable PropIds. The shape-modifier /
/// parametric-shape / rig PropIds take 208-255 (T-225b, T-243), so StyleProps fill 160-207
/// (kLastLow) and continue from kFirstHigh (256), below the effect PropIds (0x8000).
inline constexpr uint16_t kLastLow = ClipPath;
static_assert(kLastLow < xt::prop::ModTrimStart, "StyleProp numbers collide with modifier PropIds");
inline constexpr uint16_t kFirstHigh = 256;
static_assert(kFirstHigh > xt::prop::ConstraintEnabled, "StyleProp numbers collide with rig PropIds");
/// Highest StyleProp number.
inline constexpr uint16_t kLast = HitSlop;
static_assert(kLast < xt::prop::kEffectPropBase, "StyleProp numbers collide with effect PropIds");
}  // namespace styleprop

/// T-190 (03 §3.12.6, 11 §11.9): true for a vector-content property (PropId 32-51: `d`, `fill`,
/// `stroke`, `path-trim`, ...). These only apply to the Path/Group nodes of an inline `<svg>` and
/// are accepted only in `@timeline` keys; everywhere else they are dropped with a warning.
inline bool isVectorProp(uint16_t prop) {
    return prop >= xt::prop::PathData && prop <= xt::prop::StrokeTaperEnd;
}

/// T-190/T-218: the node-level properties an inline `<svg>` scene node of `kind` (Path or Group)
/// takes from an `@timeline` key or a scene-target rule (`#Icon/Blade { ... }`): opacity, the
/// transform components and transform-origin, and -- on a Path -- `d`, fill/stroke color and
/// opacity, stroke-width, stroke-dashoffset and path-trim. T-225: a Shape takes the same minus `d`
/// plus its shape parameters (232-245); a Modifier only opacity and its parameters (208-231).
bool appliesToSvgNode(uint16_t prop, NodeKind kind);

/// T-225: appliesToSvgNode() for some inline `<svg>` node kind (Path, Shape or Modifier) -- what
/// a scene-target rule may declare, since its targets' kinds are only known at runtime.
bool appliesToAnySvgNode(uint16_t prop);

/// A synthetic prop number for CSS names that are not a single value (shorthands, `transform`,
/// `transform-origin`): findProp() returns a PropInfo with this prop and kind == Shorthand; the
/// caller must not write a StyleDecl with this number.
inline constexpr uint16_t kShorthandProp = 0xFFFE;

/// `animation`'s direction/fill-mode/play-state keyword sets. Not part of 05.4's numbered table;
/// chosen conservatively in CSS keyword-list order (see T-22 report).
enum class AnimDirection : uint8_t { Normal = 0, Reverse = 1, Alternate = 2, AlternateReverse = 3 };
enum class AnimFillMode : uint8_t { None = 0, Forwards = 1, Backwards = 2, Both = 3 };
enum class AnimPlayState : uint8_t { Running = 0, Paused = 1 };

struct PropInfo {
    const char* cssName;
    uint16_t prop;
    StyleValueKind kind;
    bool inherited;
    bool animatable;
};

/// T-225 (31 §31.6.1): true for an entry of the modifier / parametric-shape name table
/// (`trim-end`, `copies`, `r`, `points`, ...; PropId 208-231 and 232-245).
bool isSvgNodePropInfo(const PropInfo& info);

/// isVectorProp() for a table entry, including the `path-trim` shorthand and (T-225) the
/// modifier / parametric-shape parameters.
inline bool isVectorPropInfo(const PropInfo& info) {
    return isVectorProp(info.prop) || std::string_view(info.cssName) == "path-trim" ||
           isSvgNodePropInfo(info);
}

/// Looks up a CSS property/shorthand name (e.g. "background-color", "margin"), then (T-225) the
/// modifier / parametric-shape names. Returns nullptr if unknown.
const PropInfo* findProp(std::string_view cssName);

/// T-225: the modifier / parametric-shape name-table entry of `prop` (PropId 208-231, 232-245),
/// nullptr if none. findPropById() does not look there (svg-only names).
const PropInfo* findSvgNodePropById(uint16_t prop);

/// T-225: the PropId an inline `<svg>` node of `kind` takes for `prop` -- CSS `width`/`height` are
/// a Shape's ShapeW/ShapeH (31 §31.6.1); T-243: on a Constraint `enabled` is ConstraintEnabled and
/// `strength` is ConstraintParam1 when `constraintKind` is Spring (IkStrength otherwise); every
/// other prop is returned unchanged.
uint16_t svgNodeProp(uint16_t prop, NodeKind kind, ConstraintKind constraintKind = ConstraintKind::Ik);

/// Looks up a resolved (non-shorthand) prop number's metadata. Returns nullptr if not a known
/// PropId/StyleProp.
const PropInfo* findPropById(uint16_t prop);

/// Enum keyword name for `prop`'s `value`. Returns nullptr if `prop` isn't an Enum property or
/// `value` has no keyword (e.g. an arbitrary font-weight number).
const char* enumName(uint16_t prop, uint32_t value);

/// Enum keyword -> value for `prop`. Returns false if `prop` isn't an Enum property or `name`
/// isn't one of its keywords.
bool enumValue(uint16_t prop, std::string_view name, uint32_t& out);

/// The property's initial value, docs/design/05-style-and-css.md §5.4. XT_ASSERT if `prop` is not
/// a known PropId/StyleProp.
StyleValue initialValue(uint16_t prop);

}  // namespace xt::compile
