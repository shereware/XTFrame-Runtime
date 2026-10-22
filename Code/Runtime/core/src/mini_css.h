#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xt/ir_pod.h"  // IrAnimationSpec (05.6/17.6, T-123's `animation:` parsing).
#include "xt/node.h"    // EffectInstance/EffectKind (18-effects.md §18.2, T-51b's `effect:` parsing).
#include "xt/types.h"
#include "xt/value.h"

/// Runtime mini CSS declaration-list parser: `"prop: value; prop: value"` -> decoded Values.
/// A private (Code/Runtime/core/src) header used by element_ref.cpp (ElementRef::setStyle) and value_helpers'
/// xt::parseValue(). See Docs/design/13-api.md §13.3-13.4, Docs/design/05-style-and-css.md §5.3-5.5,
/// Docs/tasks/T-35.md.
///
/// The grammar is the compile-time `.xts` value grammar's (Code/Runtime/compile/src/xts/xts_values.cpp) subset
/// listed in T-35: lengths (px/%/em/auto/unitless 0), unitless floats, colors (#rgb/#rgba/#rrggbb/
/// #rrggbbaa, rgb()/rgba(), `transparent`, the 17 named colors), enum keywords, font-family lists
/// and `transform`/`transform-origin`/`perspective-origin` sequences (19.10 adds `translateZ`/
/// `translate3d`/`rotateX`/`rotateY` to `transform`, plus `perspective`/`backface-visibility` as
/// plain declarations -- T-47a). `transition` is recognized but rejected with
/// Status::kUnsupported (it is compile-time-only per 05.6); `animation` is parsed (T-123, with
/// the same value grammar as Code/Runtime/compile/src/xts/xts_values.cpp's parseAnimationItem); every shorthand other than
/// `transform`/`transform-origin`/`perspective-origin` (margin, padding, border, border-width,
/// border-radius, flex, gap, inset) is *not* part of this subset and parses as an invalid
/// declaration.
///
/// The value grammar is Code/Runtime/core/src/css_values.h, which the compiler's `.xts` parser shares (T-413);
/// core does not include Code/Runtime/compile/ headers, so the CSS-name/enum-keyword tables here are still an
/// independent, hand-written copy of Code/Runtime/compile/src/xts/xts_props.cpp's.

namespace xt::mini_css {

/// One item of an `animation` declaration (05.6/17.6, T-123). `spec.timeline` is left
/// kInvalidTimeline: only the caller (ElementRef::setStyle), which has the Document, can resolve
/// `name` against the IR's `@keyframes` table.
struct AnimationDecl {
    std::string name;
    IrAnimationSpec spec{};
    /// T-335: the points of a `linear()` timing function; `spec.easing` (LinearPoints) indexes
    /// this vector, and the caller re-points it into the Animator's easing point pool.
    std::vector<EasingPoint> easingPoints;
};

/// One parsed declaration. `prop` is either an animatable PropId (0-108, xt/property.h) or a
/// non-animatable StyleProp number (160-186, style_decode.h).
struct Decl {
    uint16_t prop = 0;
    Value value{};
    /// String-valued props only (`font-family`'s first family name, `background-image`'s asset
    /// name): the text the caller still has to intern. `value.sid` is kInvalidString until then.
    std::string text{};
    uint8_t important = 0;
    /// `effect` (StyleProp 187, Docs/design/18-effects.md §18.4, T-51b) only: the parsed
    /// sequence, with each entry's stopOffset/stopCount already rebased to index `effectStops`
    /// (not some external pool). Empty for every other prop, and for `effect: none`. The caller
    /// (ElementRef::setStyle) still has to append this into Document's runtime effect declaration
    /// pool and turn `value` into the resulting ListRef -- mirrors `text` above, which the caller
    /// also still has to intern.
    std::vector<EffectInstance> effects;
    std::vector<GradientStop> effectStops;
    /// `background-gradient` (StyleProp 190, Docs/design/05-style-and-css.md §5.5c, T-115) only:
    /// the parsed value. The caller (ElementRef::setStyle) still has to append it to Document's
    /// runtime gradient declaration pool and turn `value` into the resulting ListRef, like
    /// `effects` above. `kind == GradientKind::None` for every other prop and for `none`.
    ElementGradient gradient;
    /// `clip-path` (StyleProp 207, Docs/design/05-style-and-css.md §5.9.8, T-332) only: the parsed
    /// shape. The caller (ElementRef::setStyle) still has to write it into Document's runtime
    /// clip-shape pool and turn `value` into the resulting Enum, like `gradient` above.
    /// `kind == ClipShapeKind::None` for every other prop and for `none`.
    ElementClipShape clipShape;
    /// `animation` (StyleProp 181, Docs/design/05-style-and-css.md §5.6, T-123) only: the parsed
    /// list, in declaration order. Empty for every other prop and for `animation: none`. The
    /// caller (ElementRef::setStyle) still has to resolve each item's `name` to a TimelineId and
    /// append the result to Document's runtime animation pool, like `effects` above.
    std::vector<AnimationDecl> animations;
    /// T-170 (Docs/design/05-style-and-css.md §5.9.2): true for a `prop: var(--name[, fallback])`
    /// declaration. `text` then holds `--name` (for the caller to intern), `value` is unused, and
    /// `varFallback` holds the parsed fallback when `hasVarFallback` is true. Only a
    /// Float/Length/Color/Enum-valued property accepts a whole-value `var()`; `var()` nested
    /// inside a larger value is not part of this runtime subset (invalid declaration).
    bool varRef = false;
    /// T-170: the `var()` fallback; meaningful only when `hasVarFallback` is true.
    Value varFallback{};
    /// T-170: true when the `var()` carried a fallback.
    bool hasVarFallback = false;
};

/// Parses a `style`-attribute-shaped declaration list. Every successfully parsed declaration is
/// appended to `out` (a `transform` sequence appends one Decl per component PropId); a declaration
/// that fails to parse -- unknown property name, bad value, missing `:` -- is skipped.
/// Returns kInvalidArgument if any declaration was skipped, else kUnsupported if any was a
/// `transition` declaration, else kOk. (kInvalidArgument wins over kUnsupported when
/// both happen; T-35 does not say which, and the caller's contract only distinguishes "everything
/// applied" from "something did not".)
Status parseDeclarations(std::string_view css, std::vector<Decl>& out);

/// T-389 (tests): how often parseDeclarations() ran on this thread.
uint64_t parseDeclarationsCount();

/// Parses a single property's value text (no property name, no `;`), appending the resulting
/// Decl(s) to `out`. Same statuses as parseDeclarations, for that one declaration.
Status parseOne(uint16_t prop, std::string_view valueText, std::vector<Decl>& out);

/// T-170: types a design token's value text for storage: a color first, then a unitless number
/// (Float), then a non-`auto` length. Returns false (out untouched) when none of those fits; the
/// caller then keeps only the text and re-parses it per property (parseTokenText).
bool classifyTokenText(std::string_view text, Value& out);

/// T-170: parses a token's value text as property `prop`'s value. True only when it yields exactly
/// one plain Float/Length/Color/Enum Value for `prop` (no string to intern, no list, no `var()`).
bool parseTokenText(uint16_t prop, std::string_view text, Value& out);

/// CSS property name -> prop number. Returns prop::kInvalidProp for an unknown name and for every
/// shorthand this parser does not support; `transform`/`transform-origin`/`transition`/`animation`
/// return their own synthetic/StyleProp numbers (see kTransformProp below).
uint16_t findProp(std::string_view cssName);

/// Synthetic prop numbers for the multi-output properties (they never appear in a Decl).
inline constexpr uint16_t kTransformProp = 0xFFF0;
inline constexpr uint16_t kTransformOriginProp = 0xFFF1;
/// `perspective-origin` (05.5/19.10, T-47a); expands to PerspectiveOriginX/Y (107/108).
inline constexpr uint16_t kPerspectiveOriginProp = 0xFFF2;
/// `outline` (05 §5.9.8, T-325); expands to OutlineWidth/OutlineColor (129/130) and the StyleProp
/// `outline-style` (201).
inline constexpr uint16_t kOutlineProp = 0xFFF3;
/// `text-decoration` (05 §5.9.8, T-324); expands to the StyleProp `text-decoration-line` (203),
/// TextDecorationThickness (133) and, when given, TextDecorationColor (132).
inline constexpr uint16_t kTextDecorationProp = 0xFFF4;
/// `background-size` / `background-position` (05 §5.9.8, T-327); expand to BackgroundSizeX/Y
/// (134/135) and BackgroundPositionX/Y (136/137).
inline constexpr uint16_t kBackgroundSizeProp = 0xFFF5;
inline constexpr uint16_t kBackgroundPositionProp = 0xFFF6;
/// `translate` / `rotate` / `scale` (05 §5.9.8, T-329); expand onto the transform PropIds
/// (TranslateX/Y/Z, Rotation, ScaleX/Y), or, together with `transform` in one call, onto the
/// combined translate * rotate * scale * transform sequence.
inline constexpr uint16_t kTranslateProp = 0xFFF7;
inline constexpr uint16_t kRotateProp = 0xFFF8;
inline constexpr uint16_t kScaleProp = 0xFFF9;
/// `border-style` (05 §5.9.8, T-333); expands (1-4 values) to the StyleProps
/// `border-top/right/bottom/left-style` (258-261).
inline constexpr uint16_t kBorderStyleProp = 0xFFFA;
/// `grid-column` / `grid-row` (06 §6.15, T-338); expand to the line StyleProps 268/269 and
/// 270/271.
inline constexpr uint16_t kGridColumnProp = 0xFFFB;
inline constexpr uint16_t kGridRowProp = 0xFFFC;

}  // namespace xt::mini_css
