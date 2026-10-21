#include "xts_props.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>

#include "css_values.h"  // core/src: iequals (T-421).
#include "xt/assert.h"
#include "xt/node.h"

namespace xt::compile {

namespace {

using xt::prop::Anchor;
using xt::prop::AnchorX;
using xt::prop::AnchorY;
using xt::prop::BackgroundColor;
using xt::prop::BorderBottomWidth;
using xt::prop::BorderColor;
using xt::prop::BorderLeftWidth;
using xt::prop::BorderRadiusBL;
using xt::prop::BorderRadiusBR;
using xt::prop::BorderRadiusTL;
using xt::prop::BorderRadiusTR;
using xt::prop::BorderRightWidth;
using xt::prop::BorderTopWidth;
using xt::prop::Bottom;
using xt::prop::Color;
using xt::prop::ColumnGap;
using xt::prop::FlexBasis;
using xt::prop::FlexGrow;
using xt::prop::FlexShrink;
using xt::prop::FontSize;
using xt::prop::Height;
using xt::prop::ImageOpacity;
using xt::prop::Left;
using xt::prop::LetterSpacing;
using xt::prop::LineHeight;
using xt::prop::MarginBottom;
using xt::prop::MarginLeft;
using xt::prop::MarginRight;
using xt::prop::MarginTop;
using xt::prop::MaxHeight;
using xt::prop::MaxWidth;
using xt::prop::MinHeight;
using xt::prop::MinWidth;
using xt::prop::Opacity;
using xt::prop::PaddingBottom;
using xt::prop::PaddingLeft;
using xt::prop::PaddingRight;
using xt::prop::PaddingTop;
using xt::prop::Perspective;
using xt::prop::PerspectiveOriginX;
using xt::prop::PerspectiveOriginY;
using xt::prop::Right;
using xt::prop::Rotation;
using xt::prop::RowGap;
using xt::prop::ScaleX;
using xt::prop::ScaleY;
using xt::prop::SkewX;
using xt::prop::SkewY;
using xt::prop::TextReveal;
using xt::prop::Top;
using xt::prop::Translate;
using xt::prop::TranslateX;
using xt::prop::TranslateY;
using xt::prop::Width;

// ------------------------------------------------------------------------------------------
// CSS name -> PropId/StyleProp table. docs/design/05-style-and-css.md §5.4.
// ------------------------------------------------------------------------------------------

// clang-format off
constexpr PropInfo kProps[] = {
    // Animatable (PropId) properties.
    {"opacity", Opacity, StyleValueKind::Float, false, true},
    {"width", Width, StyleValueKind::Length, false, true},
    {"height", Height, StyleValueKind::Length, false, true},
    {"min-width", MinWidth, StyleValueKind::Length, false, true},
    {"max-width", MaxWidth, StyleValueKind::Length, false, true},
    {"min-height", MinHeight, StyleValueKind::Length, false, true},
    {"max-height", MaxHeight, StyleValueKind::Length, false, true},
    {"margin-top", MarginTop, StyleValueKind::Length, false, true},
    {"margin-right", MarginRight, StyleValueKind::Length, false, true},
    {"margin-bottom", MarginBottom, StyleValueKind::Length, false, true},
    {"margin-left", MarginLeft, StyleValueKind::Length, false, true},
    {"padding-top", PaddingTop, StyleValueKind::Length, false, true},
    {"padding-right", PaddingRight, StyleValueKind::Length, false, true},
    {"padding-bottom", PaddingBottom, StyleValueKind::Length, false, true},
    {"padding-left", PaddingLeft, StyleValueKind::Length, false, true},
    {"border-top-width", BorderTopWidth, StyleValueKind::Length, false, true},
    {"border-right-width", BorderRightWidth, StyleValueKind::Length, false, true},
    {"border-bottom-width", BorderBottomWidth, StyleValueKind::Length, false, true},
    {"border-left-width", BorderLeftWidth, StyleValueKind::Length, false, true},
    {"left", Left, StyleValueKind::Length, false, true},
    {"top", Top, StyleValueKind::Length, false, true},
    {"right", Right, StyleValueKind::Length, false, true},
    {"bottom", Bottom, StyleValueKind::Length, false, true},
    {"flex-grow", FlexGrow, StyleValueKind::Float, false, true},
    {"flex-shrink", FlexShrink, StyleValueKind::Float, false, true},
    {"flex-basis", FlexBasis, StyleValueKind::Length, false, true},
    {"row-gap", RowGap, StyleValueKind::Length, false, true},
    {"column-gap", ColumnGap, StyleValueKind::Length, false, true},
    {"font-size", FontSize, StyleValueKind::Length, true, true},
    {"line-height", LineHeight, StyleValueKind::Length, true, true},
    {"letter-spacing", LetterSpacing, StyleValueKind::Length, true, true},
    {"background-color", BackgroundColor, StyleValueKind::Color, false, true},
    {"border-color", BorderColor, StyleValueKind::Color, false, true},
    // T-333 (05 §5.9.8): per-side colors; `border-color` with 2-4 values expands to them (one
    // value stays the legacy BorderColor above).
    {"border-top-color", xt::prop::BorderTopColor, StyleValueKind::Color, false, true},
    {"border-right-color", xt::prop::BorderRightColor, StyleValueKind::Color, false, true},
    {"border-bottom-color", xt::prop::BorderBottomColor, StyleValueKind::Color, false, true},
    {"border-left-color", xt::prop::BorderLeftColor, StyleValueKind::Color, false, true},
    {"border-top-left-radius", BorderRadiusTL, StyleValueKind::Length, false, true},
    {"border-top-right-radius", BorderRadiusTR, StyleValueKind::Length, false, true},
    {"border-bottom-right-radius", BorderRadiusBR, StyleValueKind::Length, false, true},
    {"border-bottom-left-radius", BorderRadiusBL, StyleValueKind::Length, false, true},
    {"color", Color, StyleValueKind::Color, true, true},
    {"image-opacity", ImageOpacity, StyleValueKind::Float, false, true},
    {"perspective", Perspective, StyleValueKind::Length, false, true},
    // 07.8 / 03.12.6 (T-178): typewriter reveal fraction, 0..1; not inherited.
    {"text-reveal", TextReveal, StyleValueKind::Float, false, true},
    // T-202 (03 §3.12.5, 05 §5.9.1): the `<particles>` fields as `particle-*` longhands (one per
    // PropId, animatable); the element's attributes compile to the same declarations
    // (doc_emit.cpp). The range pairs and the color stops also have shorthands below.
    {"particle-rate", xt::prop::ParticleRate, StyleValueKind::Float, false, true},
    {"particle-burst", xt::prop::ParticleBurst, StyleValueKind::Float, false, true},
    {"particle-lifetime-min", xt::prop::ParticleLifetimeMin, StyleValueKind::Float, false, true},
    {"particle-lifetime-max", xt::prop::ParticleLifetimeMax, StyleValueKind::Float, false, true},
    {"particle-speed-min", xt::prop::ParticleSpeedMin, StyleValueKind::Float, false, true},
    {"particle-speed-max", xt::prop::ParticleSpeedMax, StyleValueKind::Float, false, true},
    {"particle-direction", xt::prop::ParticleDirection, StyleValueKind::Float, false, true},
    {"particle-spread", xt::prop::ParticleSpread, StyleValueKind::Float, false, true},
    {"particle-gravity", xt::prop::ParticleGravity, StyleValueKind::Float, false, true},
    {"particle-drag", xt::prop::ParticleDrag, StyleValueKind::Float, false, true},
    {"particle-size-start", xt::prop::ParticleSizeStart, StyleValueKind::Float, false, true},
    {"particle-size-end", xt::prop::ParticleSizeEnd, StyleValueKind::Float, false, true},
    {"particle-color-0", xt::prop::ParticleColor0, StyleValueKind::Color, false, true},
    {"particle-color-1", xt::prop::ParticleColor1, StyleValueKind::Color, false, true},
    {"particle-color-2", xt::prop::ParticleColor2, StyleValueKind::Color, false, true},
    {"particle-color-3", xt::prop::ParticleColor3, StyleValueKind::Color, false, true},
    {"particle-rotation-min", xt::prop::ParticleRotationMin, StyleValueKind::Float, false, true},
    {"particle-rotation-max", xt::prop::ParticleRotationMax, StyleValueKind::Float, false, true},
    {"particle-max", xt::prop::ParticleMax, StyleValueKind::Float, false, true},
    // `<min> [<max>]` (one value: both) and `<c>` / `<a> <b>` / four stops (xts_values.cpp).
    {"particle-lifetime", kShorthandProp, StyleValueKind::Shorthand, false, true},
    {"particle-speed", kShorthandProp, StyleValueKind::Shorthand, false, true},
    {"particle-size", kShorthandProp, StyleValueKind::Shorthand, false, true},
    {"particle-rotation", kShorthandProp, StyleValueKind::Shorthand, false, true},
    {"particle-color", kShorthandProp, StyleValueKind::Shorthand, false, true},
    // T-325 (05 §5.9.8): the focus ring outside the border box; paint-only. `outline-style` is a
    // StyleProp below; the `outline` shorthand is `<width> <style> <color>` in any order.
    {"outline-width", xt::prop::OutlineWidth, StyleValueKind::Length, false, true},
    {"outline-color", xt::prop::OutlineColor, StyleValueKind::Color, false, true},
    {"outline-offset", xt::prop::OutlineOffset, StyleValueKind::Length, false, true},
    {"outline", kShorthandProp, StyleValueKind::Shorthand, false, false},
    // T-324 (05 §5.9.8): text decoration; paint-only, drawn in the glyph layer. The line set is a
    // StyleProp below; the `text-decoration` shorthand is `<line…> [<color>] [<thickness>]`.
    {"text-decoration-color", xt::prop::TextDecorationColor, StyleValueKind::Color, false, true},
    {"text-decoration-thickness", xt::prop::TextDecorationThickness, StyleValueKind::Length, false, true},
    {"text-decoration", kShorthandProp, StyleValueKind::Shorthand, false, false},
    // T-327 (05 §5.9.8): background image placement, read only when `background-repeat` is not
    // `stretch`. Each is a two-PropId shorthand (BackgroundSizeX/Y, BackgroundPositionX/Y), like
    // `perspective-origin`.
    {"background-size", kShorthandProp, StyleValueKind::Shorthand, false, true},
    {"background-position", kShorthandProp, StyleValueKind::Shorthand, false, true},
    // T-328 (06 §6.16): `auto | <w> / <h> | <number>` as one Float w/h (0 = auto); layout.
    {"aspect-ratio", xt::prop::AspectRatio, StyleValueKind::Float, false, true},

    // T-190 (03 §3.12.6, 11 §11.9): vector-content properties of an inline `<svg>`'s nodes, only
    // accepted in `@timeline` keys (isVectorProp). `d` takes `path("<svg path>")` (or a bare
    // string); `path-trim` is `<start> <end> [<offset>]` (fractions, offset in turns).
    {"d", xt::prop::PathData, StyleValueKind::String, false, true},
    {"fill", xt::prop::FillColor, StyleValueKind::Color, false, true},
    {"fill-opacity", xt::prop::FillOpacity, StyleValueKind::Float, false, true},
    {"stroke", xt::prop::StrokeColor, StyleValueKind::Color, false, true},
    {"stroke-opacity", xt::prop::StrokeOpacity, StyleValueKind::Float, false, true},
    {"stroke-width", xt::prop::StrokeWidth, StyleValueKind::Length, false, true},
    {"stroke-dashoffset", xt::prop::DashOffset, StyleValueKind::Length, false, true},
    // T-236: a dash array never interpolates (@timeline keys hold) and takes no transition.
    {"stroke-dasharray", xt::prop::DashArray, StyleValueKind::FloatArray, false, true},
    {"path-trim-start", xt::prop::TrimStart, StyleValueKind::Float, false, true},
    {"path-trim-end", xt::prop::TrimEnd, StyleValueKind::Float, false, true},
    {"path-trim-offset", xt::prop::TrimOffset, StyleValueKind::Float, false, true},
    // T-233 (31 §31.4.7): stroke taper, fractions of each contour's arc length (attribute form:
    // `xt-taper="<start> <end>"`). The width profile (`xt-width-profile`) is not animatable.
    {"stroke-taper-start", xt::prop::StrokeTaperStart, StyleValueKind::Float, false, true},
    {"stroke-taper-end", xt::prop::StrokeTaperEnd, StyleValueKind::Float, false, true},
    {"path-trim", kShorthandProp, StyleValueKind::Shorthand, false, true},

    // Shorthands and the multi-output `transform`/`transform-origin`/`perspective-origin`
    // properties (05.5/19.10, T-47a).
    {"margin", kShorthandProp, StyleValueKind::Shorthand, false, false},
    {"padding", kShorthandProp, StyleValueKind::Shorthand, false, false},
    {"border-width", kShorthandProp, StyleValueKind::Shorthand, false, false},
    {"border-radius", kShorthandProp, StyleValueKind::Shorthand, false, false},
    {"border", kShorthandProp, StyleValueKind::Shorthand, false, false},
    {"flex", kShorthandProp, StyleValueKind::Shorthand, false, false},
    {"gap", kShorthandProp, StyleValueKind::Shorthand, false, false},
    {"inset", kShorthandProp, StyleValueKind::Shorthand, false, false},
    {"transform", kShorthandProp, StyleValueKind::Shorthand, false, true},
    {"transform-origin", kShorthandProp, StyleValueKind::Shorthand, false, true},
    // T-329 (05 §5.9.8): independent transforms, expanded onto the existing transform PropIds.
    {"translate", kShorthandProp, StyleValueKind::Shorthand, false, true},
    {"rotate", kShorthandProp, StyleValueKind::Shorthand, false, true},
    {"scale", kShorthandProp, StyleValueKind::Shorthand, false, true},
    {"perspective-origin", kShorthandProp, StyleValueKind::Shorthand, false, true},

    // Non-animatable StyleProp (160-186, 188-189).
    {"display", styleprop::Display, StyleValueKind::Enum, false, false},
    {"position", styleprop::Position, StyleValueKind::Enum, false, false},
    {"flex-direction", styleprop::FlexDirection, StyleValueKind::Enum, false, false},
    {"flex-wrap", styleprop::FlexWrap, StyleValueKind::Enum, false, false},
    {"justify-content", styleprop::JustifyContent, StyleValueKind::Enum, false, false},
    {"align-items", styleprop::AlignItems, StyleValueKind::Enum, false, false},
    {"align-self", styleprop::AlignSelf, StyleValueKind::Enum, false, false},
    {"align-content", styleprop::AlignContent, StyleValueKind::Enum, false, false},
    {"overflow", styleprop::Overflow, StyleValueKind::Enum, false, false},
    // T-336 (05 §5.9.8): per-axis overflow; `overflow` with two values expands to them.
    {"overflow-x", styleprop::OverflowX, StyleValueKind::Enum, false, false},
    {"overflow-y", styleprop::OverflowY, StyleValueKind::Enum, false, false},
    // T-338 (06 §6.15): the grid subset. `grid-column` / `grid-row` are shorthands of the line
    // longhands; `grid-area` takes only an area name (or `auto`).
    {"grid-template-columns", styleprop::GridTemplateColumns, StyleValueKind::GridTracks, false, false},
    {"grid-template-rows", styleprop::GridTemplateRows, StyleValueKind::GridTracks, false, false},
    {"grid-template-areas", styleprop::GridTemplateAreas, StyleValueKind::GridAreas, false, false},
    {"justify-items", styleprop::JustifyItems, StyleValueKind::Enum, false, false},
    {"grid-column-start", styleprop::GridColumnStart, StyleValueKind::Enum, false, false},
    {"grid-column-end", styleprop::GridColumnEnd, StyleValueKind::Enum, false, false},
    {"grid-row-start", styleprop::GridRowStart, StyleValueKind::Enum, false, false},
    {"grid-row-end", styleprop::GridRowEnd, StyleValueKind::Enum, false, false},
    {"grid-area", styleprop::GridArea, StyleValueKind::String, false, false},
    {"grid-column", kShorthandProp, StyleValueKind::Shorthand, false, false},
    {"grid-row", kShorthandProp, StyleValueKind::Shorthand, false, false},
    // T-336 (06 §6.9): `order: <integer>` (PropId 143, Float); not animatable.
    {"order", xt::prop::Order, StyleValueKind::Float, false, false},
    {"z-index", styleprop::ZIndex, StyleValueKind::Float, false, false},
    {"visibility", styleprop::Visibility, StyleValueKind::Enum, true, false},
    {"pointer-events", styleprop::PointerEvents, StyleValueKind::Enum, true, false},
    {"font-family", styleprop::FontFamily, StyleValueKind::FontFamilyList, true, false},
    {"font-weight", styleprop::FontWeight, StyleValueKind::Enum, true, false},
    {"font-style", styleprop::FontStyle, StyleValueKind::Enum, true, false},
    {"text-align", styleprop::TextAlign, StyleValueKind::Enum, true, false},
    {"white-space", styleprop::WhiteSpace, StyleValueKind::Enum, true, false},
    {"text-overflow", styleprop::TextOverflow, StyleValueKind::Enum, false, false},
    {"text-transform", styleprop::TextTransform, StyleValueKind::Enum, true, false},  // T-323.
    // T-331 (05 §5.9.8): `line-clamp` (`none` or a count; the WebKit name is an alias) and
    // `font-variant-numeric`.
    {"line-clamp", styleprop::LineClamp, StyleValueKind::Enum, false, false},
    {"-webkit-line-clamp", styleprop::LineClamp, StyleValueKind::Enum, false, false},
    {"font-variant-numeric", styleprop::FontVariantNumeric, StyleValueKind::Enum, true, false},
    // T-345 (05 §5.9.9): the responsive exceptions, both inherited. `ui-scale` is parsed like a
    // (single-output) shorthand: a keyword or `clamp(<min>, <max>)`.
    {"ui-scale", styleprop::UiScale, StyleValueKind::Shorthand, true, false},
    {"media-override", styleprop::MediaOverride, StyleValueKind::Enum, true, false},
    {"app-region", styleprop::AppRegion, StyleValueKind::Enum, true, false},  // T-376 (05 §5.9.10).
    {"hit-slop", styleprop::HitSlop, StyleValueKind::Shorthand, false, false},  // T-386 (05 §5.9.11).
    {"mix-blend-mode", styleprop::MixBlendMode, StyleValueKind::Enum, false, false},   // T-326.
    {"cursor", styleprop::Cursor, StyleValueKind::Enum, true, false},
    {"object-fit", styleprop::ObjectFit, StyleValueKind::Enum, false, false},
    {"transition", styleprop::Transition, StyleValueKind::TransitionList, false, false},
    {"animation", styleprop::Animation, StyleValueKind::AnimationList, false, false},
    {"background-image", styleprop::BackgroundImage, StyleValueKind::String, false, false},
    // T-199 (05 §5.9.2, 26 §26.3.4): `border-image` and its longhands. `border-image-slice` is
    // parsed as a (single-output) shorthand: four numbers / percentages plus an optional `fill`.
    {"border-image", kShorthandProp, StyleValueKind::Shorthand, false, false},
    {"border-image-source", styleprop::BorderImageSource, StyleValueKind::String, false, false},
    {"border-image-slice", styleprop::BorderImageSlice, StyleValueKind::Shorthand, false, false},
    {"border-image-repeat", styleprop::BorderImageRepeat, StyleValueKind::Enum, false, false},
    {"border-image-fill", styleprop::BorderImageFill, StyleValueKind::Enum, false, false},
    {"outline-style", styleprop::OutlineStyle, StyleValueKind::Enum, false, false},  // T-325.
    // T-333 (05 §5.9.8): line styles per side; `border-style` is their 1-4 value shorthand.
    {"border-style", kShorthandProp, StyleValueKind::Shorthand, false, false},
    {"border-top-style", styleprop::BorderTopStyle, StyleValueKind::Enum, false, false},
    {"border-right-style", styleprop::BorderRightStyle, StyleValueKind::Enum, false, false},
    {"border-bottom-style", styleprop::BorderBottomStyle, StyleValueKind::Enum, false, false},
    {"border-left-style", styleprop::BorderLeftStyle, StyleValueKind::Enum, false, false},
    // T-324: not inherited, but drawn across descendant text (StyleEngine passes the set down).
    {"text-decoration-line", styleprop::TextDecorationLine, StyleValueKind::Enum, false, false},
    {"background-repeat", styleprop::BackgroundRepeat, StyleValueKind::Enum, false, false},  // T-327.
    {"image-rendering", styleprop::ImageRendering, StyleValueKind::Enum, true, false},
    {"direction", styleprop::Direction, StyleValueKind::Enum, true, false},
    {"writing-mode", styleprop::WritingMode, StyleValueKind::Enum, true, false},
    {"text-orientation", styleprop::TextOrientation, StyleValueKind::Enum, true, false},
    // `lang` is intentionally not registered: 05.4 says it comes from the `lang` HTML attribute,
    // not from CSS declarations (see T-22 report, uncovered case).
    {"backface-visibility", styleprop::BackfaceVisibility, StyleValueKind::Enum, false, false},
    {"text-rendering", styleprop::TextRendering, StyleValueKind::Enum, true, false},
    // `effect` (18-effects.md §18.4, T-51a): not inherited; `animatable = true` so
    // `transition: effect ...` is accepted by parseTransitionItem() (xts_values.cpp), which
    // requires `info->animatable` for anything other than `all` -- StyleEngine (T-51b) expands the
    // resulting `IrTransitionSpec.prop == 187` into per-parameter transitions.
    {"effect", styleprop::Effect, StyleValueKind::EffectList, false, true},
    // `background-gradient` (05.5c, T-115): not inherited and not animatable (0.4 does not
    // interpolate stops, so `transition`/`@keyframes` must not accept it).
    {"background-gradient", styleprop::BackgroundGradient, StyleValueKind::Gradient, false, false},
    // `clip-path` (05 §5.9.8, T-332): not inherited; not animatable in this first version (the
    // shape lives in the IR `CLIP` pool, the declaration is an index).
    {"clip-path", styleprop::ClipPath, StyleValueKind::ClipShape, false, false},
    // `pointer-watch` (27.2.5, 08.9, T-174): inherited like `pointer-events`, so a watch-only
    // card's children do not capture presses either.
    {"pointer-watch", styleprop::PointerWatch, StyleValueKind::Enum, true, false},
    // `text-effect` (07.8, T-178): `outline <w> <color> | glow <r> <color> |
    // shadow <dx> <dy> <r> <color>`, comma separated; inherited like CSS `text-shadow`; stored as
    // Outline/Glow/DropShadow entries of STYL effects[] (parseTextEffectList, xts_values.cpp).
    {"text-effect", styleprop::TextEffect, StyleValueKind::EffectList, true, false},
    // `clip-start` / `clip-rate` (28.2, 04.10.2, T-191): a `sync` clip's placement on its parent
    // timeline; local = (t - clip-start) * clip-rate. `clip-start` takes a time (`0.2s`, `200ms`)
    // or plain seconds.
    {"clip-start", styleprop::ClipStart, StyleValueKind::Float, false, false},
    {"clip-rate", styleprop::ClipRate, StyleValueKind::Float, false, false},
    // `focus-nav` (08.9, 27.7, T-194): inherited, so a container switches its whole subtree.
    // `layer: overlay` (08.9, T-194): not inherited (the subtree moves with its root).
    {"focus-nav", styleprop::FocusNav, StyleValueKind::Enum, true, false},
    {"layer", styleprop::Layer, StyleValueKind::Enum, false, false},
    // T-228 (31 §31.7.2): `auto-orient: none | path` (PropId 14); not inherited, not animatable.
    {"auto-orient", xt::prop::AutoOrient, StyleValueKind::Enum, false, false},
};
// clang-format on

struct EnumEntry {
    uint16_t prop;
    const char* name;
    uint32_t value;
};

// clang-format off
constexpr EnumEntry kEnums[] = {
    {styleprop::Display, "block", 0}, {styleprop::Display, "flex", 1}, {styleprop::Display, "none", 2},
    {styleprop::Display, "grid", 3},  // T-338 (06 §6.15).

    {styleprop::Position, "static", 0}, {styleprop::Position, "relative", 1},
    {styleprop::Position, "absolute", 2},

    {styleprop::FlexDirection, "row", 0}, {styleprop::FlexDirection, "column", 1},
    {styleprop::FlexDirection, "row-reverse", 2}, {styleprop::FlexDirection, "column-reverse", 3},

    {styleprop::FlexWrap, "nowrap", 0}, {styleprop::FlexWrap, "wrap", 1},
    {styleprop::FlexWrap, "wrap-reverse", 2},  // T-336.

    {styleprop::JustifyContent, "flex-start", 0}, {styleprop::JustifyContent, "flex-end", 1},
    {styleprop::JustifyContent, "center", 2}, {styleprop::JustifyContent, "space-between", 3},
    {styleprop::JustifyContent, "space-around", 4}, {styleprop::JustifyContent, "space-evenly", 5},

    {styleprop::AlignItems, "stretch", 0}, {styleprop::AlignItems, "flex-start", 1},
    {styleprop::AlignItems, "flex-end", 2}, {styleprop::AlignItems, "center", 3},

    {styleprop::AlignSelf, "auto", 0}, {styleprop::AlignSelf, "stretch", 1},
    {styleprop::AlignSelf, "flex-start", 2}, {styleprop::AlignSelf, "flex-end", 3},
    {styleprop::AlignSelf, "center", 4},

    {styleprop::AlignContent, "flex-start", 0}, {styleprop::AlignContent, "flex-end", 1},
    {styleprop::AlignContent, "center", 2}, {styleprop::AlignContent, "space-between", 3},
    {styleprop::AlignContent, "space-around", 4}, {styleprop::AlignContent, "space-evenly", 5},
    {styleprop::AlignContent, "stretch", 6},

    // T-338 (06 §6.15): `justify-items`, and the grid spellings `start` / `end` of the box
    // alignment values (after the flex names, so enumName() keeps printing those).
    {styleprop::JustifyItems, "stretch", 0}, {styleprop::JustifyItems, "start", 1},
    {styleprop::JustifyItems, "end", 2}, {styleprop::JustifyItems, "center", 3},
    {styleprop::AlignItems, "start", 1}, {styleprop::AlignItems, "end", 2},
    {styleprop::AlignSelf, "start", 2}, {styleprop::AlignSelf, "end", 3},
    {styleprop::JustifyContent, "start", 0}, {styleprop::JustifyContent, "end", 1},
    {styleprop::JustifyContent, "stretch", 6}, {styleprop::JustifyContent, "normal", 6},
    {styleprop::AlignContent, "normal", 6},
    {styleprop::AlignContent, "start", 0}, {styleprop::AlignContent, "end", 1},

    {styleprop::Overflow, "visible", 0}, {styleprop::Overflow, "hidden", 1},
    {styleprop::Overflow, "scroll", 2}, {styleprop::Overflow, "auto", 3},  // T-336: auto.
    {styleprop::OverflowX, "visible", 0}, {styleprop::OverflowX, "hidden", 1},
    {styleprop::OverflowX, "scroll", 2}, {styleprop::OverflowX, "auto", 3},
    {styleprop::OverflowY, "visible", 0}, {styleprop::OverflowY, "hidden", 1},
    {styleprop::OverflowY, "scroll", 2}, {styleprop::OverflowY, "auto", 3},

    {styleprop::Visibility, "visible", 0}, {styleprop::Visibility, "hidden", 1},

    {styleprop::PointerEvents, "auto", 0}, {styleprop::PointerEvents, "none", 1},

    {styleprop::FontWeight, "normal", 400}, {styleprop::FontWeight, "bold", 700},

    {styleprop::FontStyle, "normal", 0}, {styleprop::FontStyle, "italic", 1},

    {styleprop::TextAlign, "start", 0}, {styleprop::TextAlign, "end", 1},
    {styleprop::TextAlign, "left", 2}, {styleprop::TextAlign, "center", 3},
    {styleprop::TextAlign, "right", 4},

    {styleprop::WhiteSpace, "normal", 0}, {styleprop::WhiteSpace, "nowrap", 1},

    {styleprop::TextOverflow, "clip", 0}, {styleprop::TextOverflow, "ellipsis", 1},

    {styleprop::Cursor, "default", 0}, {styleprop::Cursor, "pointer", 1},
    {styleprop::Cursor, "text", 2}, {styleprop::Cursor, "move", 3},

    {styleprop::ObjectFit, "fill", 0}, {styleprop::ObjectFit, "contain", 1},
    {styleprop::ObjectFit, "cover", 2},

    // T-199: border-image-repeat / border-image-fill.
    {styleprop::BorderImageRepeat, "stretch", 0}, {styleprop::BorderImageRepeat, "repeat", 1},
    {styleprop::BorderImageRepeat, "round", 2},
    {styleprop::BorderImageFill, "false", 0}, {styleprop::BorderImageFill, "true", 1},

    // T-325: outline-style.
    {styleprop::OutlineStyle, "none", 0}, {styleprop::OutlineStyle, "solid", 1},

    // T-333: border-*-style (xt::kBorderStyle*).
    {styleprop::BorderTopStyle, "none", 0}, {styleprop::BorderTopStyle, "solid", 1},
    {styleprop::BorderTopStyle, "dashed", 2}, {styleprop::BorderTopStyle, "dotted", 3},
    {styleprop::BorderRightStyle, "none", 0}, {styleprop::BorderRightStyle, "solid", 1},
    {styleprop::BorderRightStyle, "dashed", 2}, {styleprop::BorderRightStyle, "dotted", 3},
    {styleprop::BorderBottomStyle, "none", 0}, {styleprop::BorderBottomStyle, "solid", 1},
    {styleprop::BorderBottomStyle, "dashed", 2}, {styleprop::BorderBottomStyle, "dotted", 3},
    {styleprop::BorderLeftStyle, "none", 0}, {styleprop::BorderLeftStyle, "solid", 1},
    {styleprop::BorderLeftStyle, "dashed", 2}, {styleprop::BorderLeftStyle, "dotted", 3},

    // T-323: text-transform.
    {styleprop::TextTransform, "none", 0}, {styleprop::TextTransform, "uppercase", 1},
    {styleprop::TextTransform, "lowercase", 2}, {styleprop::TextTransform, "capitalize", 3},
    // T-324: text-decoration-line's single keywords (bits; several keywords are OR-ed by
    // parseTextDecorationLine, xts_values.cpp).
    {styleprop::TextDecorationLine, "none", 0}, {styleprop::TextDecorationLine, "underline", 1},
    {styleprop::TextDecorationLine, "overline", 2}, {styleprop::TextDecorationLine, "line-through", 4},
    // T-327: background-repeat (`stretch` is XTFrame's own initial value) and image-rendering.
    {styleprop::BackgroundRepeat, "stretch", 0}, {styleprop::BackgroundRepeat, "no-repeat", 1},
    {styleprop::BackgroundRepeat, "repeat", 2}, {styleprop::BackgroundRepeat, "repeat-x", 3},
    {styleprop::BackgroundRepeat, "repeat-y", 4},
    {styleprop::ImageRendering, "auto", 0}, {styleprop::ImageRendering, "pixelated", 1},
    // T-331: line-clamp's keyword (a count is read as a number, xts_values.cpp) and
    // font-variant-numeric.
    {styleprop::LineClamp, "none", 0},
    {styleprop::FontVariantNumeric, "normal", 0}, {styleprop::FontVariantNumeric, "tabular-nums", 1},
    // T-345 (05 §5.9.9): media-override.
    {styleprop::MediaOverride, "auto", 0}, {styleprop::MediaOverride, "show", 1},
    {styleprop::MediaOverride, "hide", 2},
    // T-376 (05 §5.9.10): app-region.
    {styleprop::AppRegion, "none", 0}, {styleprop::AppRegion, "drag", 1}, {styleprop::AppRegion, "no-drag", 2},

    // T-326 (18 §18.12): mix-blend-mode, the BlendMode values in xt/node.h order; `additive` is
    // the older spelling of `add`.
    {styleprop::MixBlendMode, "normal", 0}, {styleprop::MixBlendMode, "multiply", 1},
    {styleprop::MixBlendMode, "screen", 2}, {styleprop::MixBlendMode, "overlay", 3},
    {styleprop::MixBlendMode, "darken", 4}, {styleprop::MixBlendMode, "lighten", 5},
    {styleprop::MixBlendMode, "color-dodge", 6}, {styleprop::MixBlendMode, "color-burn", 7},
    {styleprop::MixBlendMode, "hard-light", 8}, {styleprop::MixBlendMode, "soft-light", 9},
    {styleprop::MixBlendMode, "difference", 10}, {styleprop::MixBlendMode, "exclusion", 11},
    {styleprop::MixBlendMode, "hue", 12}, {styleprop::MixBlendMode, "saturation", 13},
    {styleprop::MixBlendMode, "color", 14}, {styleprop::MixBlendMode, "luminosity", 15},
    {styleprop::MixBlendMode, "add", 16}, {styleprop::MixBlendMode, "additive", 16},

    {styleprop::Direction, "ltr", 0}, {styleprop::Direction, "rtl", 1},

    {styleprop::WritingMode, "horizontal-tb", 0}, {styleprop::WritingMode, "vertical-rl", 1},
    {styleprop::WritingMode, "vertical-lr", 2},

    {styleprop::TextOrientation, "mixed", 0}, {styleprop::TextOrientation, "upright", 1},
    {styleprop::TextOrientation, "sideways", 2},

    {styleprop::BackfaceVisibility, "visible", 0}, {styleprop::BackfaceVisibility, "hidden", 1},

    {styleprop::TextRendering, "auto", 0}, {styleprop::TextRendering, "geometricPrecision", 1},

    {styleprop::PointerWatch, "false", 0}, {styleprop::PointerWatch, "true", 1},

    {styleprop::FocusNav, "auto", 0}, {styleprop::FocusNav, "explicit", 1},
    {styleprop::FocusNav, "none", 2},

    {styleprop::Layer, "normal", 0}, {styleprop::Layer, "overlay", 1},
    {xt::prop::AutoOrient, "none", 0}, {xt::prop::AutoOrient, "path", 1},
};
// clang-format on

/// T-225 (31 §31.6.1): the modifier (PropId 208-231) and parametric-shape (232-245) parameters of
/// inline `<svg>` content, accepted only where vector properties are (isVectorPropInfo). Kept out
/// of kProps because the names (`x`, `r`, `size`, ...) only mean these on svg content:
/// findPropById() never returns them (findSvgNodePropById() does). `width`/`height` resolve to the CSS
/// entries and svgNodeProp() maps them onto ShapeW/ShapeH for a Shape node. T-236: the Vec2 parameters
/// (ModAnchor, ModTranslate, ModCenter) take `<x> <y>` (StyleValueKind::Vec2).
constexpr PropInfo kSvgNodeProps[] = {
    {"trim-start", xt::prop::ModTrimStart, StyleValueKind::Float, false, true},
    {"trim-end", xt::prop::ModTrimEnd, StyleValueKind::Float, false, true},
    {"trim-offset", xt::prop::ModTrimOffset, StyleValueKind::Float, false, true},
    {"copies", xt::prop::ModCopies, StyleValueKind::Float, false, true},
    {"repeat-offset", xt::prop::ModOffset, StyleValueKind::Float, false, true},
    {"repeat-anchor", xt::prop::ModAnchor, StyleValueKind::Vec2, false, true},
    {"repeat-translate", xt::prop::ModTranslate, StyleValueKind::Vec2, false, true},
    {"repeat-rotation", xt::prop::ModRotation, StyleValueKind::Float, false, true},
    {"repeat-scale", xt::prop::ModScale, StyleValueKind::Float, false, true},
    {"opacity-start", xt::prop::ModStartOpacity, StyleValueKind::Float, false, true},
    {"opacity-end", xt::prop::ModEndOpacity, StyleValueKind::Float, false, true},
    {"radius", xt::prop::ModRadius, StyleValueKind::Float, false, true},
    {"amount", xt::prop::ModAmount, StyleValueKind::Float, false, true},
    {"size", xt::prop::ModSize, StyleValueKind::Float, false, true},
    {"ridges", xt::prop::ModRidges, StyleValueKind::Float, false, true},
    {"angle", xt::prop::ModAngle, StyleValueKind::Float, false, true},
    {"center", xt::prop::ModCenter, StyleValueKind::Vec2, false, true},
    {"detail", xt::prop::ModDetail, StyleValueKind::Float, false, true},
    {"seed", xt::prop::ModSeed, StyleValueKind::Float, false, true},
    {"speed", xt::prop::ModSpeed, StyleValueKind::Float, false, true},
    {"correlation", xt::prop::ModCorrelation, StyleValueKind::Float, false, true},
    {"temporal-phase", xt::prop::ModTemporalPhase, StyleValueKind::Float, false, true},
    {"spatial-phase", xt::prop::ModSpatialPhase, StyleValueKind::Float, false, true},
    {"enabled", xt::prop::ModEnabled, StyleValueKind::Float, false, true},
    {"x", xt::prop::ShapeX, StyleValueKind::Float, false, true},
    {"y", xt::prop::ShapeY, StyleValueKind::Float, false, true},
    {"rx", xt::prop::ShapeRx, StyleValueKind::Float, false, true},
    {"ry", xt::prop::ShapeRy, StyleValueKind::Float, false, true},
    {"cx", xt::prop::ShapeCx, StyleValueKind::Float, false, true},
    {"cy", xt::prop::ShapeCy, StyleValueKind::Float, false, true},
    {"r", xt::prop::ShapeR, StyleValueKind::Float, false, true},
    {"r2", xt::prop::ShapeR2, StyleValueKind::Float, false, true},
    {"points", xt::prop::ShapePoints, StyleValueKind::Float, false, true},
    {"rotation", xt::prop::ShapeRotation, StyleValueKind::Float, false, true},
    {"round", xt::prop::ShapeRound, StyleValueKind::Float, false, true},
    {"round2", xt::prop::ShapeRound2, StyleValueKind::Float, false, true},
    // T-243 (32 §32.2.1/§32.2.3): bone and constraint parameters. `strength` is IkStrength here;
    // svgNodeProp() maps it onto a Spring's ConstraintParam1 and `enabled` (ModEnabled above) onto
    // a constraint's ConstraintEnabled. The paramN entries come before the spring aliases so
    // findSvgNodePropById() names them `paramN`.
    {"length", xt::prop::BoneLength, StyleValueKind::Float, false, true},
    {"strength", xt::prop::IkStrength, StyleValueKind::Float, false, true},
    {"mix", xt::prop::ConstraintMix, StyleValueKind::Float, false, true},
    {"param0", xt::prop::ConstraintParam0, StyleValueKind::Float, false, true},
    {"param1", xt::prop::ConstraintParam1, StyleValueKind::Float, false, true},
    {"param2", xt::prop::ConstraintParam2, StyleValueKind::Float, false, true},
    {"param3", xt::prop::ConstraintParam3, StyleValueKind::Float, false, true},
    {"param4", xt::prop::ConstraintParam4, StyleValueKind::Float, false, true},
    {"param5", xt::prop::ConstraintParam5, StyleValueKind::Float, false, true},
    {"inertia", xt::prop::ConstraintParam0, StyleValueKind::Float, false, true},
    {"damping", xt::prop::ConstraintParam2, StyleValueKind::Float, false, true},
    {"gravity", xt::prop::ConstraintParam3, StyleValueKind::Float, false, true},
};

}  // namespace

const PropInfo* findProp(std::string_view cssName) {
    for (const PropInfo& info : kProps) {
        if (cssName == info.cssName) {
            return &info;
        }
    }
    for (const PropInfo& info : kSvgNodeProps) {
        if (cssName == info.cssName) {
            return &info;
        }
    }
    return nullptr;
}

bool isSvgNodePropInfo(const PropInfo& info) {
    return &info >= std::begin(kSvgNodeProps) && &info < std::end(kSvgNodeProps);
}

const PropInfo* findSvgNodePropById(uint16_t prop) {
    for (const PropInfo& info : kSvgNodeProps) {
        if (info.prop == prop) {
            return &info;
        }
    }
    return nullptr;
}

uint16_t svgNodeProp(uint16_t prop, NodeKind kind, ConstraintKind constraintKind) {
    if (kind == NodeKind::Constraint) {
        // T-243 (32 §32.2.3): `enabled` and, on a spring, `strength` (its params[1]).
        if (prop == xt::prop::ModEnabled) {
            return xt::prop::ConstraintEnabled;
        }
        if (prop == xt::prop::IkStrength && constraintKind == ConstraintKind::Spring) {
            return xt::prop::ConstraintParam1;
        }
        return prop;
    }
    if (kind == NodeKind::Shape) {
        if (prop == xt::prop::Width) {
            return xt::prop::ShapeW;
        }
        if (prop == xt::prop::Height) {
            return xt::prop::ShapeH;
        }
    }
    return prop;
}

const PropInfo* findPropById(uint16_t prop) {
    for (const PropInfo& info : kProps) {
        if (info.prop == prop && info.kind != StyleValueKind::Shorthand) {
            return &info;
        }
    }
    return nullptr;
}

const char* enumName(uint16_t prop, uint32_t value) {
    for (const EnumEntry& e : kEnums) {
        if (e.prop == prop && e.value == value) {
            return e.name;
        }
    }
    return nullptr;
}

bool enumValue(uint16_t prop, std::string_view name, uint32_t& out) {
    for (const EnumEntry& e : kEnums) {
        if (e.prop == prop && css_values::iequals(name, e.name)) {  // T-421 (05 §5.3).
            out = e.value;
            return true;
        }
    }
    return false;
}

namespace {

StyleValue floatValue(float f) {
    StyleValue v;
    v.kind = StyleValueKind::Float;
    v.f = f;
    return v;
}

StyleValue lengthValue(float f, LengthUnit unit) {
    StyleValue v;
    v.kind = StyleValueKind::Length;
    v.len = Length{f, unit};
    return v;
}

StyleValue colorValue(xt::Color c) {
    StyleValue v;
    v.kind = StyleValueKind::Color;
    v.color = c;
    return v;
}

StyleValue enumValueOf(uint32_t e) {
    StyleValue v;
    v.kind = StyleValueKind::Enum;
    v.e = e;
    return v;
}

StyleValue stringValue(std::string s) {
    StyleValue v;
    v.kind = StyleValueKind::String;
    v.str = std::move(s);
    return v;
}

}  // namespace

StyleValue initialValue(uint16_t prop) {
    switch (prop) {
        case Opacity:
            return floatValue(1);
        case AnchorX:
        case AnchorY:
            return lengthValue(50, LengthUnit::Percent);
        case Anchor:
            return lengthValue(50, LengthUnit::Percent);
        case TranslateX:
        case TranslateY:
        case Translate:
            return lengthValue(0, LengthUnit::Px);
        case ScaleX:
        case ScaleY:
            return floatValue(1);
        case Rotation:
        case SkewX:
        case SkewY:
            return floatValue(0);
        case Width:
        case Height:
        case MaxWidth:
        case MaxHeight:
        case Left:
        case Top:
        case Right:
        case Bottom:
        case FlexBasis:
            return lengthValue(0, LengthUnit::Auto);
        case MinWidth:
        case MinHeight:
        case MarginTop:
        case MarginRight:
        case MarginBottom:
        case MarginLeft:
        case PaddingTop:
        case PaddingRight:
        case PaddingBottom:
        case PaddingLeft:
        case BorderTopWidth:
        case BorderRightWidth:
        case BorderBottomWidth:
        case BorderLeftWidth:
        case RowGap:
        case ColumnGap:
        case LetterSpacing:
        case BorderRadiusTL:
        case BorderRadiusTR:
        case BorderRadiusBR:
        case BorderRadiusBL:
        case xt::prop::OutlineWidth:  // T-325
        case xt::prop::OutlineOffset:
            return lengthValue(0, LengthUnit::Px);
        case FlexGrow:
        case xt::prop::AspectRatio:  // T-328: `auto`.
            return floatValue(0);
        case FlexShrink:
            return floatValue(1);
        case FontSize:
            return lengthValue(16, LengthUnit::Px);
        case LineHeight:
            return lengthValue(1.2f, LengthUnit::Em);
        case BackgroundColor:
        case BorderColor:
        // T-333: a side's initial value is `border-color` (StyleEngine copies it into every side
        // no rule declares); `border-color`'s own initial value stands in here.
        case xt::prop::BorderTopColor:
        case xt::prop::BorderRightColor:
        case xt::prop::BorderBottomColor:
        case xt::prop::BorderLeftColor:
            return colorValue(xt::Color{0, 0, 0, 0});
        case Color:
        // T-325: `outline-color`'s initial value is the element's `color` (StyleEngine substitutes
        // it when no rule declares one); `color`'s own initial value stands in here.
        case xt::prop::OutlineColor:
        case xt::prop::TextDecorationColor:  // T-324: likewise.
            return colorValue(xt::Color{0, 0, 0, 1});
        case xt::prop::TextDecorationThickness:
        case xt::prop::BackgroundSizeX:  // T-327: `auto`.
        case xt::prop::BackgroundSizeY:
            return lengthValue(0, LengthUnit::Auto);  // T-324: `auto`.
        case xt::prop::BackgroundPositionX:           // T-327: `0% 0%`.
        case xt::prop::BackgroundPositionY:
            return lengthValue(0, LengthUnit::Percent);
        case ImageOpacity:
        case TextReveal:
            return floatValue(1);
        // T-202: particle field initial values (core/src/property_access.cpp's defaults).
        case xt::prop::ParticleRate:
            return floatValue(10);
        case xt::prop::ParticleLifetimeMin:
        case xt::prop::ParticleLifetimeMax:
            return floatValue(1);
        case xt::prop::ParticleSpeedMin:
        case xt::prop::ParticleSpeedMax:
            return floatValue(100);
        case xt::prop::ParticleDirection:
            return floatValue(-90);
        case xt::prop::ParticleSizeStart:
        case xt::prop::ParticleSizeEnd:
            return floatValue(8);
        case xt::prop::ParticleColor0:
            return colorValue(xt::Color{1, 1, 1, 1});
        case xt::prop::ParticleColor1:
            return colorValue(xt::Color{1, 1, 1, 2.0f / 3.0f});
        case xt::prop::ParticleColor2:
            return colorValue(xt::Color{1, 1, 1, 1.0f / 3.0f});
        case xt::prop::ParticleColor3:
            return colorValue(xt::Color{1, 1, 1, 0});
        case xt::prop::ParticleMax:
            return floatValue(256);
        case Perspective:
            return lengthValue(0, LengthUnit::Auto);  // `none` (05.5/19.10, T-47a).
        case PerspectiveOriginX:
        case PerspectiveOriginY:
            return lengthValue(50, LengthUnit::Percent);
        case xt::prop::AutoOrient:
            return enumValueOf(0);  // none
        case styleprop::Display:
            return enumValueOf(0);  // block
        case styleprop::Position:
            return enumValueOf(0);  // static
        case styleprop::FlexDirection:
            return enumValueOf(0);  // row
        case styleprop::FlexWrap:
            return enumValueOf(0);  // nowrap
        case styleprop::JustifyContent:
            return enumValueOf(6);  // stretch (`normal`; T-338)
        case styleprop::AlignItems:
            return enumValueOf(0);  // stretch
        case styleprop::AlignSelf:
            return enumValueOf(0);  // auto
        case styleprop::AlignContent:
            return enumValueOf(6);  // stretch
        case styleprop::Overflow:
        case styleprop::OverflowX:  // T-336.
        case styleprop::OverflowY:
            return enumValueOf(0);  // visible
        case xt::prop::Order:       // T-336.
            return floatValue(0);
        case styleprop::GridTemplateColumns:  // T-338: none.
        case styleprop::GridTemplateRows: {
            StyleValue v;
            v.kind = StyleValueKind::GridTracks;
            return v;
        }
        case styleprop::GridTemplateAreas: {
            StyleValue v;
            v.kind = StyleValueKind::GridAreas;
            return v;
        }
        case styleprop::JustifyItems:
            return enumValueOf(0);  // stretch (T-338)
        case styleprop::GridColumnStart:
        case styleprop::GridColumnEnd:
        case styleprop::GridRowStart:
        case styleprop::GridRowEnd:
            return enumValueOf(0);  // auto (T-338)
        case styleprop::GridArea:
            return stringValue("");  // auto (T-338)
        case styleprop::ZIndex:
            return floatValue(0);
        case styleprop::Visibility:
            return enumValueOf(0);  // visible
        case styleprop::PointerEvents:
            return enumValueOf(0);  // auto
        case styleprop::FontFamily: {
            StyleValue v;
            v.kind = StyleValueKind::FontFamilyList;
            v.families.emplace_back("sans-serif");
            return v;
        }
        case styleprop::FontWeight:
            return enumValueOf(400);
        case styleprop::FontStyle:
            return enumValueOf(0);  // normal
        case styleprop::TextAlign:
            return enumValueOf(0);  // start
        case styleprop::WhiteSpace:
            return enumValueOf(0);  // normal
        case styleprop::TextOverflow:
            return enumValueOf(0);  // clip
        case styleprop::Cursor:
            return enumValueOf(0);  // default
        case styleprop::ObjectFit:
            return enumValueOf(1);  // contain
        case styleprop::Transition: {
            StyleValue v;
            v.kind = StyleValueKind::TransitionList;
            return v;
        }
        case styleprop::Animation: {
            StyleValue v;
            v.kind = StyleValueKind::AnimationList;
            return v;
        }
        case styleprop::BackgroundImage:
            return stringValue("none");
        case styleprop::BorderImageSource:  // T-199.
            return stringValue("none");
        case styleprop::BorderImageSlice: {
            // `auto`: NaN in every slot (style_decode.h's encoding of StyleProp 198).
            StyleValue v;
            v.kind = StyleValueKind::Color;
            const float nan = std::numeric_limits<float>::quiet_NaN();
            v.color = xt::Color{nan, nan, nan, nan};
            return v;
        }
        case styleprop::BorderImageRepeat:
            return enumValueOf(0);  // stretch
        case styleprop::BorderImageFill:
            return enumValueOf(0);  // false
        case styleprop::OutlineStyle:
            return enumValueOf(0);       // none (T-325)
        case styleprop::BorderTopStyle:  // T-333: solid.
        case styleprop::BorderRightStyle:
        case styleprop::BorderBottomStyle:
        case styleprop::BorderLeftStyle:
            return enumValueOf(1);
        case styleprop::TextTransform:
            return enumValueOf(0);  // none (T-323)
        case styleprop::TextDecorationLine:
            return enumValueOf(0);  // none (T-324)
        case styleprop::MixBlendMode:
            return enumValueOf(0);  // normal (T-326)
        case styleprop::BackgroundRepeat:
            return enumValueOf(0);  // stretch (T-327)
        case styleprop::ImageRendering:
            return enumValueOf(0);  // auto (T-327)
        case styleprop::ClipPath: {
            StyleValue v;
            v.kind = StyleValueKind::ClipShape;
            return v;  // `none` (T-332): ClipShapeKind::None.
        }
        case styleprop::LineClamp:
            return enumValueOf(0);  // none (T-331)
        case styleprop::FontVariantNumeric:
            return enumValueOf(0);  // normal (T-331)
        case styleprop::UiScale: {
            StyleValue v;  // `auto` (T-345): mode 0.
            v.kind = StyleValueKind::Color;
            v.color = xt::Color{0, 0, 0, 0};
            return v;
        }
        case styleprop::MediaOverride:
            return enumValueOf(0);  // auto (T-345)
        case styleprop::AppRegion:
            return enumValueOf(0);  // none (T-376)
        case styleprop::HitSlop: {
            StyleValue v;  // 0 on every side (T-386).
            v.kind = StyleValueKind::Color;
            v.color = xt::Color{0, 0, 0, 0};
            return v;
        }
        case styleprop::Direction:
            return enumValueOf(0);  // ltr
        case styleprop::WritingMode:
            return enumValueOf(0);  // horizontal-tb
        case styleprop::TextOrientation:
            return enumValueOf(0);  // mixed
        case styleprop::Lang:
            return stringValue("");
        case styleprop::BackfaceVisibility:
            return enumValueOf(0);  // visible
        case styleprop::TextRendering:
            return enumValueOf(0);  // auto
        case styleprop::PointerWatch:
            return enumValueOf(0);  // false
        case styleprop::FocusNav:
            return enumValueOf(0);  // auto (T-194)
        case styleprop::Layer:
            return enumValueOf(0);  // normal (T-194)
        case styleprop::ClipStart: {
            StyleValue v;
            v.kind = StyleValueKind::Float;
            v.f = 0;
            return v;
        }
        case styleprop::ClipRate: {
            StyleValue v;
            v.kind = StyleValueKind::Float;
            v.f = 1;
            return v;
        }
        case xt::prop::PathData:
            return stringValue("");
        case xt::prop::FillColor:
        case xt::prop::StrokeColor: {
            StyleValue v;
            v.kind = StyleValueKind::Color;
            v.color = xt::Color{0, 0, 0, 1};
            return v;
        }
        case xt::prop::FillOpacity:
        case xt::prop::StrokeOpacity:
        case xt::prop::TrimEnd:
            return floatValue(1);
        case xt::prop::TrimStart:
        case xt::prop::TrimOffset:
        case xt::prop::StrokeTaperStart:  // T-233
        case xt::prop::StrokeTaperEnd:
            return floatValue(0);
        case xt::prop::StrokeWidth:
            return lengthValue(1, LengthUnit::Px);
        case xt::prop::DashOffset:
            return lengthValue(0, LengthUnit::Px);
        case xt::prop::DashArray: {
            StyleValue v;
            v.kind = StyleValueKind::FloatArray;
            return v;  // `none`: no dashes.
        }
        case styleprop::Effect:
        case styleprop::TextEffect: {
            StyleValue v;
            v.kind = StyleValueKind::EffectList;
            return v;  // `none` (18-effects.md §18.4, 07.8): empty effects list.
        }
        case styleprop::BackgroundGradient: {
            StyleValue v;
            v.kind = StyleValueKind::Gradient;
            return v;  // `none` (05.5c): GradientKind::None.
        }
        default:
            XT_ASSERT(false && "initialValue: unknown prop");
            return StyleValue{};
    }
}

bool appliesToSvgNode(uint16_t prop, NodeKind kind) {
    // T-243 (32 §32.2): as xt::propertyApplies() for Bone, Constraint and Mesh.
    if (kind == NodeKind::Bone || kind == NodeKind::Constraint || kind == NodeKind::Mesh) {
        return xt::propertyDefined(prop) && xt::propertyApplies(kind, prop);
    }
    if (prop >= xt::prop::BoneLength && prop <= xt::prop::ConstraintEnabled) {
        return false;
    }
    if (kind == NodeKind::Modifier) {
        // T-225: a modifier takes only its own parameters (and opacity, which it ignores).
        return prop == xt::prop::Opacity || (prop >= xt::prop::ModTrimStart && prop <= xt::prop::ModEnabled);
    }
    if (prop >= xt::prop::ShapeX && prop <= xt::prop::ShapeRound2) {
        return kind == NodeKind::Shape;  // T-225
    }
    switch (prop) {
        case xt::prop::Opacity:
        case xt::prop::AnchorX:
        case xt::prop::AnchorY:
        case xt::prop::TranslateX:
        case xt::prop::TranslateY:
        case xt::prop::ScaleX:
        case xt::prop::ScaleY:
        case xt::prop::Rotation:
        case xt::prop::SkewX:
        case xt::prop::SkewY:
        case xt::prop::Visible:  // T-229: cels (31 §31.7.5).
            return true;
        case xt::prop::PathData:
            return kind == NodeKind::Path;
        case xt::prop::FillColor:
        case xt::prop::FillOpacity:
        case xt::prop::StrokeColor:
        case xt::prop::StrokeOpacity:
        case xt::prop::StrokeWidth:
        case xt::prop::TrimStart:
        case xt::prop::TrimEnd:
        case xt::prop::TrimOffset:
        case xt::prop::DashOffset:
        case xt::prop::DashArray:         // T-236
        case xt::prop::StrokeTaperStart:  // T-233
        case xt::prop::StrokeTaperEnd:
            return kind == NodeKind::Path || kind == NodeKind::Shape;  // T-225: Shape paints too.
        default:
            return false;
    }
}

bool appliesToAnySvgNode(uint16_t prop) {
    return appliesToSvgNode(prop, NodeKind::Path) || appliesToSvgNode(prop, NodeKind::Shape) ||
           appliesToSvgNode(prop, NodeKind::Modifier) || appliesToSvgNode(prop, NodeKind::Bone) ||
           appliesToSvgNode(prop, NodeKind::Constraint) || appliesToSvgNode(prop, NodeKind::Mesh);
}

}  // namespace xt::compile
