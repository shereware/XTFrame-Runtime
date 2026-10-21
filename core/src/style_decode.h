#pragma once

#include <cstdint>

#include "xt/ir_pod.h"
#include "xt/node.h"
#include "xt/value.h"

/// IrDecl -> Value/enum-field decoding, the 05.4 initial-value table and the inherited-property
/// table. A private (core/src) header; the `styleprop` numbers below are the only copy -- the
/// compiler's xt::compile::styleprop (compile/src/xts/xts_props.h) names them through this header
/// (T-413). The initial values and the inherited column are still a separate hand-written copy of
/// compile/src/xts/xts_props.cpp's (see docs/design/05-style-and-css.md §5.4, docs/tasks/T-24.md).

namespace xt::style_decode {

/// Non-animatable StyleProp numbers (05.4), shared with the compiler (xt::compile::styleprop).
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
inline constexpr uint16_t Effect = 187;              ///< 18-effects.md §18.4 (T-51a/T-51b); ListRef-valued.
inline constexpr uint16_t BackfaceVisibility = 188;  ///< 05.5/19.10 (T-47a).
inline constexpr uint16_t TextRendering = 189;       ///< 23.4/T-101; auto | geometricPrecision.
inline constexpr uint16_t BackgroundGradient = 190;  ///< 05.5c (T-115); ListRef-valued.
inline constexpr uint16_t PointerWatch = 191;        ///< 27.2.5/08.9 (T-174); false | true.
/// 07 §7.8 (T-178): `text-effect`; ListRef into STYL `effects[]` (Outline/Glow/DropShadow entries).
inline constexpr uint16_t TextEffect = 192;
/// 28 §28.2 / 04 §4.10.2 (T-191): `clip-start` (seconds) and `clip-rate` of a `sync` clip.
inline constexpr uint16_t ClipStart = 193;
inline constexpr uint16_t ClipRate = 194;
/// 08 §8.9 (T-194): `focus-nav` (auto | explicit | none) and `layer` (normal | overlay).
inline constexpr uint16_t FocusNav = 195;
inline constexpr uint16_t Layer = 196;
/// 26 §26.3.4 / 05 §5.9.2 (T-199): the `border-image` nine-slice, compiled from the shorthand
/// `border-image: url(x.png) [<L> <R> <T> <B>] [fill] [stretch|repeat|round]` into four longhands:
///   197 `border-image-source`: StringRef, the image asset name or "none";
///   198 `border-image-slice`: four f32 (L, R, T, B) encoded like a Color value; a value >= 0 is
///       pixels of the source image, a negative value -f is the percentage f (0..1) of the source
///       width (L, R) or height (T, B); NaN in every slot = `auto` (the asset manifest's
///       `measured.slice`, else 0);
///   199 `border-image-repeat`: Enum 0 stretch, 1 repeat, 2 round (the edge and middle tiles);
///   200 `border-image-fill`: Enum 0 (border only) or 1 (`fill`: the middle tile is drawn too).
inline constexpr uint16_t BorderImageSource = 197;
inline constexpr uint16_t BorderImageSlice = 198;
inline constexpr uint16_t BorderImageRepeat = 199;
inline constexpr uint16_t BorderImageFill = 200;
/// 05 §5.9.8 (T-325): `outline-style`: Enum 0 none, 1 solid. Width / color / offset are the
/// animatable PropIds 129-131 (xt/property.h).
inline constexpr uint16_t OutlineStyle = 201;
/// 05 §5.9.8 (T-323): `text-transform`: Enum 0 none, 1 uppercase, 2 lowercase, 3 capitalize;
/// inherited, not animatable.
inline constexpr uint16_t TextTransform = 202;
/// 05 §5.9.8 (T-324): `text-decoration-line`: Enum payload = a set of kTextDecoration* bits
/// (xt/node.h), 0 = none. Color / thickness are the animatable
/// PropIds 132-133 (xt/property.h).
inline constexpr uint16_t TextDecorationLine = 203;
/// 05 §5.9.8 / 18 §18.12 (T-326): `mix-blend-mode`: Enum holding a BlendMode value (0 normal ..
/// 16 add; `additive` is an alias of `add`); not inherited, not animatable. Copied to
/// NodeCommon::blend, so the Painter's PushLayer(opacity, blend) rule (18 §18.1) covers Elements.
inline constexpr uint16_t MixBlendMode = 204;
/// 05 §5.9.8 (T-327): `background-repeat`: Enum, a kBackgroundRepeat* value (xt/node.h); 0 =
/// `stretch` (initial). Size / position are the animatable PropIds 134-137 (xt/property.h).
inline constexpr uint16_t BackgroundRepeat = 205;
/// 05 §5.9.8 (T-327): `image-rendering`: Enum 0 auto, 1 pixelated; inherited.
inline constexpr uint16_t ImageRendering = 206;
/// 05 §5.9.8 (T-332): `clip-path`: Enum, 0 = `none`, else 1 + the index into the IR `CLIP`
/// chunk's shape pool (or, with kRuntimeEffectOffsetBit, Document's runtime clip-shape pool). Not
/// inherited, not animatable.
inline constexpr uint16_t ClipPath = 207;
/// 05 §5.9.8 (T-331): `line-clamp` (alias `-webkit-line-clamp`): Enum payload = the line count,
/// 0 = `none`; not inherited. 160-207 is full (208-255 are the modifier / shape / rig PropIds), so
/// StyleProps continue at 256.
inline constexpr uint16_t LineClamp = 256;
/// 05 §5.9.8 (T-331): `font-variant-numeric`: Enum 0 normal, 1 tabular-nums; inherited.
inline constexpr uint16_t FontVariantNumeric = 257;
/// 05 §5.9.8 (T-333): `border-top/right/bottom/left-style` (the `border-style` shorthand's
/// longhands): Enum, a kBorderStyle* value (xt/node.h), 0 none, 1 solid (initial), 2 dashed, 3
/// dotted; not inherited, not animatable.
inline constexpr uint16_t BorderTopStyle = 258;
inline constexpr uint16_t BorderRightStyle = 259;
inline constexpr uint16_t BorderBottomStyle = 260;
inline constexpr uint16_t BorderLeftStyle = 261;
/// 05 §5.9.8 (T-336): `overflow-x` / `overflow-y`: Enum, an xt::Overflow (0 visible, 1 hidden, 2
/// scroll, 3 auto); not inherited, not animatable. `overflow` (168) sets both axes.
inline constexpr uint16_t OverflowX = 262;
inline constexpr uint16_t OverflowY = 263;
/// 06 §6.15 (T-338): `grid-template-columns` / `-rows`: Enum, 0 = `none`, else 1 + the IR `GRID`
/// chunk's track-list index; `grid-template-areas`: likewise into its area lists.
inline constexpr uint16_t GridTemplateColumns = 264;
inline constexpr uint16_t GridTemplateRows = 265;
inline constexpr uint16_t GridTemplateAreas = 266;
/// T-338: `justify-items`: Enum in AlignItems numbering (0 stretch, 1 start, 2 end, 3 center).
inline constexpr uint16_t JustifyItems = 267;
/// T-338: `grid-column-start` / `-end`, `grid-row-start` / `-end`: Enum, kGridLine* (xt/node.h).
inline constexpr uint16_t GridColumnStart = 268;
inline constexpr uint16_t GridColumnEnd = 269;
inline constexpr uint16_t GridRowStart = 270;
inline constexpr uint16_t GridRowEnd = 271;
/// T-338: `grid-area: <name> | auto`: StringRef, the area name (`auto` = the empty string).
inline constexpr uint16_t GridArea = 272;
/// 05 §5.9.9 (T-345): `ui-scale: auto | none | clamp(<min>, <max>)`: four f32 encoded like a
/// Color value -- (mode, min, max, 0) with mode a kUiScale* value (xt/node.h); inherited, not
/// animatable.
inline constexpr uint16_t UiScale = 273;
/// 05 §5.9.9 (T-345): `media-override: auto | show | hide`: Enum 0 / 1 / 2; inherited, not
/// animatable.
inline constexpr uint16_t MediaOverride = 274;
/// 05 §5.9.10 (T-376): `app-region: none | drag | no-drag`: Enum 0 / 1 / 2 (xt::AppRegion);
/// inherited, not animatable.
inline constexpr uint16_t AppRegion = 275;
/// 05 §5.9.11 (T-386): `hit-slop: <length>{1,4}`. In an IR of minor <= 7 only: four f32 encoded
/// like a Color value (top, right, bottom, left; px, `%` and `em` per xt::kHitSlopEmBias). Since
/// IR minor 8 (T-405) the compiler writes the four longhands below instead; StyleProp 276 remains
/// the computed-value id (ElementRef::computedStyleValue). Not inherited, not animatable.
inline constexpr uint16_t HitSlop = 276;
/// T-405 (02 §2.6, IR minor 8): one `hit-slop` side, a plain Length (Px, Percent or Em; >= 0).
/// Not source properties: only the `hit-slop` shorthand writes them.
inline constexpr uint16_t HitSlopTop = 277;
inline constexpr uint16_t HitSlopRight = 278;
inline constexpr uint16_t HitSlopBottom = 279;
inline constexpr uint16_t HitSlopLeft = 280;
static_assert(HitSlopLeft == kStylePropLast, "update kStylePropLast (ir_pod.h) and 02 §2.12");
}  // namespace styleprop

/// True if `prop` (a PropId 0-108, or a StyleProp 160-186/188) is an inherited CSS property per 05.4.
/// Unknown / undefined props (including the 94-95 gap and any general PropId other than the
/// animatable subset below) return false.
bool isInherited(uint16_t prop);

/// Resets `style`'s non-animatable fields (display..lang) to their 05.4 initial values. Does not
/// touch `style.base` (that's initComputedStyleDefaults(), xt/property.h) nor
/// transition/animation/lastAnimation offsets-counts (initial = 0,0, already the struct default).
void initNonAnimatableDefaults(ComputedStyle& style);

/// Applies one non-animatable Enum-valued StyleProp decl (display/position/.../object-fit/
/// direction/writing-mode/text-orientation) onto the matching ComputedStyle field. `enumValue` is
/// the raw IrDecl enum payload. Returns false if `prop` isn't one of those (the caller then knows
/// to handle it itself: visibility/pointer-events/font-weight ARE Enum too and handled here;
/// font-family/transition/animation/background-image/lang are ListRef/StringRef and handled by the
/// caller directly against the IR pools).
bool applyEnumDecl(ComputedStyle& style, uint16_t prop, uint32_t enumValue);

/// The general (transform-function) PropIds StyleEngine decodes from CSS: Opacity(0),
/// TranslateX/Y(3/4), ScaleX/Y(5/6), Rotation(7), SkewX/Y(8/9). AnchorX/Y(1/2) are decoded
/// separately into ElementData::originX/Y (Length, % preserved for layout, see docs/design/
/// 06-layout.md §6.7) rather than through this table. Returns Value{} (Float 0) for any other
/// prop (unreachable in practice; callers only query the props listed above).
Value initialGeneralValue(uint16_t prop);

/// Converts a CSS Length (px/em/percent/auto) to a plain px float for the general transform
/// PropIds above (whose runtime Value is ValueType::Float, see xt/property.h propertyType()).
/// Percent is not resolvable at style-resolve time (the layout box isn't known yet): returns false
/// and leaves outPx untouched -- an uncovered case (see docs/tasks/T-24 report); the caller keeps
/// the element's previous value. Auto also returns false (translate/scale/etc. never legally carry
/// Auto; defensive).
bool lengthToPx(const Length& len, float fontSizePx, float& outPx);

}  // namespace xt::style_decode
