#include <vector>

#include "css_color.h"
#include "doctest.h"
#include "mini_css.h"
#include "style_decode.h"
#include "xt/property.h"
#include "xt/timeline.h"
#include "xt/types.h"
#include "xt/value.h"
#include "xt/value_helpers.h"

/// core's runtime mini CSS parser (core/src/mini_css.h): one case per value type, partial
/// application of a list with a bad declaration, the `transition`/`animation` rejection and
/// 13.4's parseValue(). See docs/design/05-style-and-css.md §5.3-5.5, docs/tasks/T-35.md.

using namespace xt;

namespace {

namespace sp = style_decode::styleprop;

std::vector<mini_css::Decl> parse(std::string_view css, Status expected) {
    std::vector<mini_css::Decl> decls;
    Status status = mini_css::parseDeclarations(css, decls);
    CHECK(status == expected);
    return decls;
}

}  // namespace

TEST_CASE("mini_css: length values (px, %, em, auto, unitless 0)") {
    std::vector<mini_css::Decl> decls =
        parse("width: 10px; height: 50%; margin-top: 2em; max-width: auto; padding-left: 0", Status::kOk);
    REQUIRE(decls.size() == 5);
    CHECK(decls[0].prop == prop::Width);
    CHECK(decls[0].value.type == ValueType::Length);
    CHECK(decls[0].value.len.value == doctest::Approx(10));
    CHECK(decls[0].value.len.unit == LengthUnit::Px);
    CHECK(decls[1].value.len.unit == LengthUnit::Percent);
    CHECK(decls[1].value.len.value == doctest::Approx(50));
    CHECK(decls[2].value.len.unit == LengthUnit::Em);
    CHECK(decls[3].value.len.unit == LengthUnit::Auto);
    CHECK(decls[4].value.len.unit == LengthUnit::Px);
    CHECK(decls[4].value.len.value == doctest::Approx(0));

    // A unitless non-zero length is not a length (05.3).
    parse("width: 10", Status::kInvalidArgument);
    // line-height is the one property where unitless means Em.
    std::vector<mini_css::Decl> lh = parse("line-height: 1.5", Status::kOk);
    REQUIRE(lh.size() == 1);
    CHECK(lh[0].value.len.unit == LengthUnit::Em);
    CHECK(lh[0].value.len.value == doctest::Approx(1.5f));
}

TEST_CASE("mini_css: float and enum values") {
    std::vector<mini_css::Decl> decls =
        parse("opacity: 0.5; z-index: 3; display: flex; font-weight: bold; cursor: pointer", Status::kOk);
    REQUIRE(decls.size() == 5);
    CHECK(decls[0].value.type == ValueType::Float);
    CHECK(decls[0].value.f == doctest::Approx(0.5f));
    CHECK(decls[1].prop == sp::ZIndex);
    CHECK(decls[1].value.f == doctest::Approx(3));
    CHECK(decls[2].prop == sp::Display);
    CHECK(decls[2].value.type == ValueType::Enum);
    CHECK(decls[2].value.e == 1);  // flex
    CHECK(decls[3].value.e == 700);
    CHECK(decls[4].value.e == 1);  // pointer

    std::vector<mini_css::Decl> weight = parse("font-weight: 300", Status::kOk);
    REQUIRE(weight.size() == 1);
    CHECK(weight[0].value.e == 300);
    parse("font-weight: 750", Status::kInvalidArgument);
    parse("display: sideways", Status::kInvalidArgument);
}

TEST_CASE("mini_css: color values") {
    std::vector<mini_css::Decl> decls =
        parse("color: #f00; background-color: #00ff0080; border-color: rgb(0, 0, 255)", Status::kOk);
    REQUIRE(decls.size() == 3);
    CHECK(decls[0].value.type == ValueType::Color);
    CHECK(decls[0].value.c.r == doctest::Approx(1));
    CHECK(decls[0].value.c.a == doctest::Approx(1));
    CHECK(decls[1].value.c.g == doctest::Approx(1));
    CHECK(decls[1].value.c.a == doctest::Approx(128.0f / 255.0f));
    CHECK(decls[2].value.c.b == doctest::Approx(1));

    std::vector<mini_css::Decl> more = parse(
        "color: rgba(255, 0, 0, 0.25); background-color: transparent; border-color: orange", Status::kOk);
    REQUIRE(more.size() == 3);
    CHECK(more[0].value.c.a == doctest::Approx(0.25f));
    CHECK(more[1].value.c.a == doctest::Approx(0));
    CHECK(more[2].value.c.g == doctest::Approx(165.0f / 255.0f));

    parse("color: #12345", Status::kInvalidArgument);
    parse("color: notacolor", Status::kInvalidArgument);
}

TEST_CASE("mini_css: font-family and background-image keep their text for interning") {
    std::vector<mini_css::Decl> decls =
        parse("font-family: \"My Font\", sans-serif; background-image: logo", Status::kOk);
    REQUIRE(decls.size() == 2);
    CHECK(decls[0].prop == sp::FontFamily);
    CHECK(decls[0].value.type == ValueType::ListRef);
    CHECK(decls[0].text == "My Font");
    CHECK(decls[1].prop == sp::BackgroundImage);
    CHECK(decls[1].value.type == ValueType::StringRef);
    CHECK(decls[1].text == "logo");

    parse("font-family: \"a\",", Status::kInvalidArgument);
}

TEST_CASE("mini_css: transform sequence and transform-origin") {
    std::vector<mini_css::Decl> decls =
        parse("transform: translate(1px, 2px) rotate(45deg) scale(2)", Status::kOk);
    REQUIRE(decls.size() == 5);
    CHECK(decls[0].prop == prop::TranslateX);
    CHECK(decls[0].value.len.value == doctest::Approx(1));
    CHECK(decls[1].prop == prop::TranslateY);
    CHECK(decls[2].prop == prop::Rotation);
    CHECK(decls[2].value.f == doctest::Approx(45));
    CHECK(decls[3].prop == prop::ScaleX);
    CHECK(decls[3].value.f == doctest::Approx(2));
    CHECK(decls[4].prop == prop::ScaleY);

    // 05.5 (T-120): any other order composes to a 4x4 and decomposes into all ten components.
    std::vector<mini_css::Decl> reordered = parse("transform: scale(2) rotate(45deg)", Status::kOk);
    CHECK(reordered.size() == 10);
    std::vector<mini_css::Decl> twice = parse("transform: rotate(45deg) rotate(10deg)", Status::kOk);
    REQUIRE(twice.size() == 10);
    CHECK(twice[5].prop == prop::Rotation);
    CHECK(twice[5].value.f == doctest::Approx(55).epsilon(1e-4));
    parse("transform: rotate(45)", Status::kInvalidArgument);             // Angles need `deg`.
    parse("transform: matrix(1,0,0,1,0px,0)", Status::kInvalidArgument);  // T-322: e/f are numbers.

    // 05.5's `none` clause (T-69): the empty sequence, i.e. every component at its identity.
    std::vector<mini_css::Decl> identity = parse("transform: none", Status::kOk);
    REQUIRE(identity.size() == 10);
    CHECK(identity[0].prop == prop::TranslateX);
    CHECK(identity[0].value.len.value == doctest::Approx(0));
    CHECK(identity[1].prop == prop::TranslateY);
    CHECK(identity[2].prop == prop::TranslateZ);
    CHECK(identity[2].value.f == doctest::Approx(0));
    CHECK(identity[3].prop == prop::RotationX);
    CHECK(identity[4].prop == prop::RotationY);
    CHECK(identity[4].value.f == doctest::Approx(0));
    CHECK(identity[5].prop == prop::Rotation);
    CHECK(identity[5].value.f == doctest::Approx(0));
    CHECK(identity[6].prop == prop::SkewX);
    CHECK(identity[7].prop == prop::SkewY);
    CHECK(identity[8].prop == prop::ScaleX);
    CHECK(identity[8].value.f == doctest::Approx(1));
    CHECK(identity[9].prop == prop::ScaleY);
    CHECK(identity[9].value.f == doctest::Approx(1));
    parse("transform: none rotate(10deg)", Status::kInvalidArgument);

    std::vector<mini_css::Decl> origin = parse("transform-origin: left top", Status::kOk);
    REQUIRE(origin.size() == 2);
    CHECK(origin[0].prop == prop::AnchorX);
    CHECK(origin[0].value.len.unit == LengthUnit::Percent);
    CHECK(origin[0].value.len.value == doctest::Approx(0));
    CHECK(origin[1].prop == prop::AnchorY);
    CHECK(origin[1].value.len.value == doctest::Approx(0));
}

TEST_CASE("mini_css: a bad declaration is skipped, the rest still applies") {
    std::vector<mini_css::Decl> decls =
        parse("width: 10px; bogus: 1; height: ???; color: red;", Status::kInvalidArgument);
    // "color: ???" is a lexical error for the whole text, so use a lexable bad value instead.
    CHECK(decls.empty());

    decls = parse("width: 10px; bogus: 1; height: none; color: red", Status::kInvalidArgument);
    REQUIRE(decls.size() == 2);
    CHECK(decls[0].prop == prop::Width);
    CHECK(decls[1].prop == prop::Color);

    // Unsupported shorthands are just unknown property names here (T-35's subset).
    parse("margin: 4px", Status::kInvalidArgument);
    // Missing colon / empty value.
    parse("width 10px", Status::kInvalidArgument);
    parse("width:", Status::kInvalidArgument);
    // Empty declarations and trailing semicolons are not errors.
    parse(";; width: 1px ;", Status::kOk);
}

TEST_CASE("mini_css: !important and transition rejection") {
    std::vector<mini_css::Decl> decls = parse("width: 10px !important; height: 2px", Status::kOk);
    REQUIRE(decls.size() == 2);
    CHECK(decls[0].important == 1);
    CHECK(decls[1].important == 0);

    decls = parse("transition: opacity 0.2s", Status::kUnsupported);
    CHECK(decls.empty());
    // The other declarations of the same list still apply.
    decls = parse("width: 7px; transition: width 1s", Status::kUnsupported);
    REQUIRE(decls.size() == 1);
    CHECK(decls[0].prop == prop::Width);
    CHECK(decls[0].value.len.value == doctest::Approx(7));
}

TEST_CASE("mini_css: effect sequence, custom() rejection, none, and bad declarations") {
    std::vector<mini_css::Decl> decls =
        parse("effect: shadow(1px 2px 3px #f00) outline(2px #00f inner)", Status::kOk);
    REQUIRE(decls.size() == 1);
    CHECK(decls[0].prop == sp::Effect);
    CHECK(decls[0].value.type == ValueType::ListRef);
    REQUIRE(decls[0].effects.size() == 2);
    CHECK(decls[0].effects[0].kind == EffectKind::DropShadow);
    CHECK(decls[0].effects[0].params[0] == doctest::Approx(1));
    CHECK(decls[0].effects[0].params[1] == doctest::Approx(2));
    CHECK(decls[0].effects[0].params[2] == doctest::Approx(3));  // blur
    CHECK(decls[0].effects[0].colors[0].r == doctest::Approx(1));
    CHECK(decls[0].effects[1].kind == EffectKind::Outline);
    CHECK(decls[0].effects[1].params[0] == doctest::Approx(2));
    CHECK(decls[0].effects[1].params[2] == doctest::Approx(2));  // position: inner
    CHECK(decls[0].effects[1].colors[0].b == doctest::Approx(1));

    decls = parse("effect: none", Status::kOk);
    REQUIRE(decls.size() == 1);
    CHECK(decls[0].effects.empty());

    // 0.2 has no shader support: `custom()` is a distinct kUnsupported, same treatment as
    // `transition`/`animation` (18.4).
    parse("effect: custom(foo)", Status::kUnsupported);
    parse("effect: bogus(1px)", Status::kInvalidArgument);
    parse("effect: blur(10%)", Status::kInvalidArgument);  // Effect lengths don't accept percent.
}

// T-130: backdrop-blur() (18.4's kind 8), same acceptance as the compiler's parser -- including
// 18.6's "it must be the stack's first entry" rule, which here is a plain kInvalidArgument.
TEST_CASE("mini_css: backdrop-blur() arguments and ordering") {
    std::vector<mini_css::Decl> decls = parse("effect: backdrop-blur(8px 0.4 -0.1)", Status::kOk);
    REQUIRE(decls.size() == 1);
    REQUIRE(decls[0].effects.size() == 1);
    CHECK(decls[0].effects[0].kind == EffectKind::BackdropBlur);
    CHECK(decls[0].effects[0].params[0] == doctest::Approx(8));
    CHECK(decls[0].effects[0].params[1] == doctest::Approx(0.4f));
    CHECK(decls[0].effects[0].params[2] == doctest::Approx(-0.1f));

    decls = parse("effect: backdrop-blur(6px)", Status::kOk);
    REQUIRE(decls[0].effects.size() == 1);
    CHECK(decls[0].effects[0].params[1] == doctest::Approx(0));
    CHECK(decls[0].effects[0].params[2] == doctest::Approx(0));

    decls = parse("effect: backdrop-blur(6px) blur(2px)", Status::kOk);
    REQUIRE(decls[0].effects.size() == 2);
    CHECK(decls[0].effects[0].kind == EffectKind::BackdropBlur);

    parse("effect: blur(2px) backdrop-blur(6px)", Status::kInvalidArgument);
}

// T-111: shadow()'s trailing `inset` keyword (18.4), same acceptance as the compiler's parser.
TEST_CASE("mini_css: shadow() inset keyword") {
    std::vector<mini_css::Decl> decls = parse("effect: shadow(2px 2px 4px 0 #000 0.5 inset)", Status::kOk);
    REQUIRE(decls.size() == 1);
    REQUIRE(decls[0].effects.size() == 1);
    CHECK(decls[0].effects[0].kind == EffectKind::DropShadow);
    CHECK(decls[0].effects[0].params[4] == doctest::Approx(0.5f));
    CHECK(decls[0].effects[0].params[5] == doctest::Approx(1));

    decls = parse("effect: shadow(2px 2px 4px 0 #000 0.5)", Status::kOk);
    REQUIRE(decls.size() == 1);
    CHECK(decls[0].effects[0].params[5] == doctest::Approx(0));

    decls = parse("effect: shadow(2px 2px #000 inset)", Status::kOk);
    REQUIRE(decls.size() == 1);
    CHECK(decls[0].effects[0].params[4] == doctest::Approx(1));
    CHECK(decls[0].effects[0].params[5] == doctest::Approx(1));

    parse("effect: shadow(2px 2px inset 4px #000)", Status::kInvalidArgument);
}

TEST_CASE("mini_css: shine() position sixth argument") {
    std::vector<mini_css::Decl> decls = parse("effect: shine(20deg 30px #fff 1 0.5 0.75)", Status::kOk);
    REQUIRE(decls.size() == 1);
    REQUIRE(decls[0].effects.size() == 1);
    CHECK(decls[0].effects[0].kind == EffectKind::Shine);
    CHECK(decls[0].effects[0].params[0] == doctest::Approx(0.75f));  // position
    CHECK(decls[0].effects[0].params[1] == doctest::Approx(30));     // width
    CHECK(decls[0].effects[0].params[2] == doctest::Approx(0.5f));   // softness
    CHECK(decls[0].effects[0].params[3] == doctest::Approx(20));     // angle
    CHECK(decls[0].effects[0].params[4] == doctest::Approx(1));      // intensity

    // Omitted -> default 0.
    decls = parse("effect: shine(20deg 30px #fff)", Status::kOk);
    REQUIRE(decls.size() == 1);
    REQUIRE(decls[0].effects.size() == 1);
    CHECK(decls[0].effects[0].params[0] == doctest::Approx(0));

    // Out of range -> clamped (this runtime parser has no DiagnosticSink to warn through).
    decls = parse("effect: shine(20deg 30px #fff 1 0.5 1.5)", Status::kOk);
    REQUIRE(decls.size() == 1);
    REQUIRE(decls[0].effects.size() == 1);
    CHECK(decls[0].effects[0].params[0] == doctest::Approx(1));

    decls = parse("effect: shine(20deg 30px #fff 1 0.5 -0.5)", Status::kOk);
    REQUIRE(decls.size() == 1);
    REQUIRE(decls[0].effects.size() == 1);
    CHECK(decls[0].effects[0].params[0] == doctest::Approx(0));
}

TEST_CASE("parseValue: 13.4's single-value entry point") {
    Result<Value> width = parseValue(prop::Width, "12px");
    REQUIRE(width.ok());
    CHECK(width.value().type == ValueType::Length);
    CHECK(width.value().len.value == doctest::Approx(12));

    Result<Value> color = parseValue(prop::Color, "#ff0000");
    REQUIRE(color.ok());
    CHECK(color.value().c.r == doctest::Approx(1));

    Result<Value> opacity = parseValue(prop::Opacity, "0.5");
    REQUIRE(opacity.ok());
    CHECK(opacity.value().f == doctest::Approx(0.5f));

    CHECK(parseValue(prop::Width, "wide").status() == Status::kInvalidArgument);
    CHECK(parseValue(prop::PathData, "12px").status() == Status::kInvalidArgument);  // Not a CSS prop.
    CHECK(parseValue(sp::Transition, "opacity 1s").status() == Status::kUnsupported);
    CHECK(parseValue(sp::FontFamily, "serif").status() == Status::kUnsupported);  // Needs interning.
}

TEST_CASE("makeFloat/makeVec2/makeColor/makeLength/makeEnum") {
    CHECK(makeFloat(2.5f).f == doctest::Approx(2.5f));
    CHECK(makeVec2(1, 2).v2 == Vec2{1, 2});
    CHECK(makeColor(1, 0, 0, 1).c == Color{1, 0, 0, 1});
    CHECK(makeLength(4, LengthUnit::Percent).len.unit == LengthUnit::Percent);
    CHECK(makeEnum(3).e == 3);
    CHECK(makeEnum(3).type == ValueType::Enum);
}

// 05.5c (T-115): the runtime parser mirrors the compiler's `background-gradient` grammar.
TEST_CASE("mini_css: background-gradient linear/radial, none and bad values") {
    std::vector<mini_css::Decl> decls =
        parse("background-gradient: linear-gradient(90deg, #f00, #00f)", Status::kOk);
    REQUIRE(decls.size() == 1);
    CHECK(decls[0].prop == sp::BackgroundGradient);
    CHECK(decls[0].value.type == ValueType::ListRef);
    CHECK(decls[0].gradient.kind == GradientKind::Linear);
    CHECK(decls[0].gradient.angle == doctest::Approx(90));
    REQUIRE(decls[0].gradient.stops.size() == 2);
    CHECK(decls[0].gradient.stops[0].offset == doctest::Approx(0));
    CHECK(decls[0].gradient.stops[1].offset == doctest::Approx(1));

    decls = parse("background-gradient: linear-gradient(0.25turn, red 10%, blue 90%)", Status::kOk);
    REQUIRE(decls.size() == 1);
    CHECK(decls[0].gradient.angle == doctest::Approx(90));
    REQUIRE(decls[0].gradient.stops.size() == 2);
    CHECK(decls[0].gradient.stops[0].offset == doctest::Approx(0.1f));
    CHECK(decls[0].gradient.stops[1].offset == doctest::Approx(0.9f));

    decls = parse("background-gradient: radial-gradient(50% 50% at 30% 40%, #fff, #000)", Status::kOk);
    REQUIRE(decls.size() == 1);
    CHECK(decls[0].gradient.kind == GradientKind::Radial);
    CHECK(decls[0].gradient.rx.value == doctest::Approx(50));
    CHECK(decls[0].gradient.rx.unit == LengthUnit::Percent);
    CHECK(decls[0].gradient.cx.value == doctest::Approx(30));
    CHECK(decls[0].gradient.cy.value == doctest::Approx(40));

    decls = parse("background-gradient: none", Status::kOk);
    REQUIRE(decls.size() == 1);
    CHECK(decls[0].gradient.kind == GradientKind::None);
    CHECK(decls[0].gradient.stops.empty());

    parse("background-gradient: linear-gradient(90deg #f00, #00f)", Status::kInvalidArgument);
    parse("background-gradient: linear-gradient(90deg, #f00)", Status::kInvalidArgument);
    parse("background-gradient: radial-gradient(50% 50% 30% 40%, #fff, #000)", Status::kInvalidArgument);
}

// T-160 (bench BUG-5): 8-digit hex stops keep their alpha in the runtime parser too.
TEST_CASE("mini_css: background-gradient 8-digit hex stop alpha") {
    std::vector<mini_css::Decl> decls = parse(
        "background-gradient: linear-gradient(90deg, #1a261f00 0%, #2a3a30 40%, #3a5040 100%)", Status::kOk);
    REQUIRE(decls.size() == 1);
    REQUIRE(decls[0].gradient.stops.size() == 3);
    CHECK(decls[0].gradient.stops[0].color.r == doctest::Approx(0x1a / 255.0f));
    CHECK(decls[0].gradient.stops[0].color.a == doctest::Approx(0.0f));
    CHECK(decls[0].gradient.stops[1].color.a == doctest::Approx(1.0f));
}

// T-123: `animation` is no longer a kUnsupported declaration -- it parses with 05.6's grammar,
// leaving only the `@keyframes` name resolution to the caller (ElementRef::setStyle).
TEST_CASE("mini_css: animation list") {
    std::vector<mini_css::Decl> decls = parse("animation: spin 1s linear", Status::kOk);
    REQUIRE(decls.size() == 1);
    CHECK(decls[0].prop == sp::Animation);
    CHECK(decls[0].value.type == ValueType::ListRef);
    REQUIRE(decls[0].animations.size() == 1);
    CHECK(decls[0].animations[0].name == "spin");
    CHECK(decls[0].animations[0].spec.duration == doctest::Approx(1));
    CHECK(decls[0].animations[0].spec.easing.kind == static_cast<uint8_t>(EasingKind::Linear));
    CHECK(decls[0].animations[0].spec.iterations == 1);
    CHECK(decls[0].animations[0].spec.timeline == kInvalidTimeline);

    // Every optional component, in 05.6's order, plus a comma-separated second item.
    decls =
        parse("animation: pulse 250ms ease-in-out 1s infinite alternate both paused, spin 2s", Status::kOk);
    REQUIRE(decls.size() == 1);
    REQUIRE(decls[0].animations.size() == 2);
    const mini_css::AnimationDecl& first = decls[0].animations[0];
    CHECK(first.name == "pulse");
    CHECK(first.spec.duration == doctest::Approx(0.25));
    CHECK(first.spec.delay == doctest::Approx(1));
    CHECK(first.spec.easing.kind == static_cast<uint8_t>(EasingKind::CubicBezier));
    CHECK(first.spec.iterations == 0);  // `infinite`
    CHECK(first.spec.direction == 2);   // alternate
    CHECK(first.spec.fill == 3);        // both
    CHECK(first.spec.playState == 1);   // paused
    CHECK(decls[0].animations[1].name == "spin");

    decls = parse("animation: bounce 1s steps(4, start)", Status::kOk);
    REQUIRE(decls[0].animations.size() == 1);
    CHECK(decls[0].animations[0].spec.easing.kind == static_cast<uint8_t>(EasingKind::Steps));
    CHECK(decls[0].animations[0].spec.easing.stepCount == 4);
    CHECK(decls[0].animations[0].spec.easing.stepsJumpEnd == 0);

    // A `cubic-bezier()`'s own commas do not split the list (T-123): two items, not five.
    decls = parse("animation: slide 1s cubic-bezier(0.1, 0.2, 0.3, 0.4), spin 3s", Status::kOk);
    REQUIRE(decls[0].animations.size() == 2);
    CHECK(decls[0].animations[1].name == "spin");
    CHECK(decls[0].animations[1].spec.duration == doctest::Approx(3));
    CHECK(decls[0].animations[0].spec.easing.x1 == doctest::Approx(0.1f));
    CHECK(decls[0].animations[0].spec.easing.y2 == doctest::Approx(0.4f));

    // `none` is an empty list, not an error (it is how xt_flow's `stopAnimation` port clears one).
    decls = parse("animation: none", Status::kOk);
    REQUIRE(decls.size() == 1);
    CHECK(decls[0].prop == sp::Animation);
    CHECK(decls[0].animations.empty());

    parse("animation: spin", Status::kInvalidArgument);  // No duration.
    parse("animation: spin 1s bogus", Status::kInvalidArgument);
    parse("animation: 1s 2s", Status::kInvalidArgument);  // No name.

    // 13.4's parseValue() has no Document to resolve `@keyframes` names with.
    CHECK(parseValue(sp::Animation, "spin 1s").status() == Status::kUnsupported);
}

// ---------------------------------------------------------------------------------------------
// T-322: hsl(), the named colour table, rotateZ()/matrix(), step-start/end, transition-*.
// ---------------------------------------------------------------------------------------------

TEST_CASE("T-322 mini_css: hsl()/hsla() and the CSS named colours") {
    std::vector<mini_css::Decl> decls = parse(
        "color: hsl(120, 100%, 50%); background-color: hsla(0 100% 50% / 25%); border-color: hsl(0.5turn "
        "100% 25%)",
        Status::kOk);
    REQUIRE(decls.size() == 3);
    CHECK(decls[0].value.c.g == doctest::Approx(1));
    CHECK(decls[0].value.c.r == doctest::Approx(0));
    CHECK(decls[1].value.c.r == doctest::Approx(1));
    CHECK(decls[1].value.c.a == doctest::Approx(0.25f));
    CHECK(decls[2].value.c.g == doctest::Approx(128.0f / 255.0f));
    CHECK(decls[2].value.c.b == doctest::Approx(128.0f / 255.0f));

    decls =
        parse("color: RebeccaPurple; background-color: aliceblue; border-color: yellowgreen", Status::kOk);
    REQUIRE(decls.size() == 3);
    CHECK(decls[0].value.c.r == doctest::Approx(0x66 / 255.0f));
    CHECK(decls[0].value.c.b == doctest::Approx(0x99 / 255.0f));
    CHECK(decls[1].value.c.r == doctest::Approx(0xF0 / 255.0f));
    CHECK(decls[2].value.c.g == doctest::Approx(0xCD / 255.0f));

    parse("color: hsl(120, 100, 50%)", Status::kInvalidArgument);
    parse("color: hsl(120 100% 50%, 1)", Status::kInvalidArgument);
    parse("color: aliceblu", Status::kInvalidArgument);
}

TEST_CASE("T-322 css_color: the named colour table is sorted, complete and searchable") {
    CHECK(css_color::kNamedColorCount == 148);
    std::string_view prev;
    for (size_t i = 0; i < css_color::kNamedColorCount; ++i) {
        std::string_view name;
        uint32_t rgb = 0;
        REQUIRE(css_color::namedColorAt(i, name, rgb));
        CAPTURE(name);
        CHECK(prev < name);
        Color c;
        REQUIRE(css_color::findNamedColor(name, c));
        CHECK(c.r == doctest::Approx(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f));
        CHECK(c.b == doctest::Approx(static_cast<float>(rgb & 0xFF) / 255.0f));
        prev = name;
    }
    std::string_view name;
    uint32_t rgb = 0;
    CHECK_FALSE(css_color::namedColorAt(css_color::kNamedColorCount, name, rgb));
    Color c;
    CHECK_FALSE(css_color::findNamedColor("", c));
    CHECK_FALSE(css_color::findNamedColor("transparent", c));  // Handled by the parsers themselves.
}

TEST_CASE("T-322 mini_css: rotateZ() and matrix()") {
    std::vector<mini_css::Decl> decls = parse("transform: rotateZ(45deg)", Status::kOk);
    REQUIRE(decls.size() == 1);
    CHECK(decls[0].prop == prop::Rotation);
    CHECK(decls[0].value.f == doctest::Approx(45));

    // matrix() always composes: all ten components.
    decls = parse("transform: matrix(1, 0, 0, 1, 12, -3)", Status::kOk);
    REQUIRE(decls.size() == 10);
    CHECK(decls[0].prop == prop::TranslateX);
    CHECK(decls[0].value.len.value == doctest::Approx(12));
    CHECK(decls[1].value.len.value == doctest::Approx(-3));
    CHECK(decls[5].prop == prop::Rotation);
    CHECK(decls[5].value.f == doctest::Approx(0));
    decls = parse("transform: matrix(0, 2, -2, 0, 0, 0)", Status::kOk);  // rotate(90deg) scale(2).
    REQUIRE(decls.size() == 10);
    CHECK(decls[5].value.f == doctest::Approx(90).epsilon(1e-4));
    CHECK(decls[8].value.f == doctest::Approx(2).epsilon(1e-4));
    CHECK(decls[9].value.f == doctest::Approx(2).epsilon(1e-4));

    // Singular: the function is dropped, the rest of the sequence still applies.
    decls = parse("transform: matrix(0, 0, 0, 0, 5, 5) rotate(20deg)", Status::kOk);
    REQUIRE(decls.size() == 10);
    CHECK(decls[0].value.len.value == doctest::Approx(0));
    CHECK(decls[5].value.f == doctest::Approx(20).epsilon(1e-4));
    parse("transform: matrix(1, 0, 0, 1)", Status::kInvalidArgument);
}

TEST_CASE("T-322 mini_css: step-start / step-end and the transition longhands") {
    std::vector<mini_css::Decl> decls = parse("animation: a 1s step-start, b 2s step-end", Status::kOk);
    REQUIRE(decls.size() == 1);
    REQUIRE(decls[0].animations.size() == 2);
    CHECK(decls[0].animations[0].spec.easing.kind == static_cast<uint8_t>(EasingKind::Steps));
    CHECK(decls[0].animations[0].spec.easing.stepCount == 1);
    CHECK(decls[0].animations[0].spec.easing.stepsJumpEnd == 0);
    CHECK(decls[0].animations[1].spec.easing.stepsJumpEnd == 1);

    // Like `transition`, the longhands are compile-time only (05.6): recognised, not applied.
    for (const char* css : {"transition-property: opacity", "transition-duration: 1s",
                            "transition-timing-function: step-end", "transition-delay: 2s"}) {
        CAPTURE(css);
        CHECK(parse(css, Status::kUnsupported).empty());
    }
}

TEST_CASE("mini_css: T-327 background-size / -position / -repeat and image-rendering") {
    std::vector<mini_css::Decl> cover = parse("background-size: cover", Status::kOk);
    REQUIRE(cover.size() == 2);
    CHECK(cover[0].prop == prop::BackgroundSizeX);
    CHECK(cover[1].prop == prop::BackgroundSizeY);
    CHECK(cover[0].value.len.unit == LengthUnit::Auto);
    CHECK(cover[0].value.len.value == doctest::Approx(kBackgroundSizeCover));
    CHECK(cover[1].value.len.value == doctest::Approx(kBackgroundSizeCover));

    std::vector<mini_css::Decl> contain = parse("background-size: contain", Status::kOk);
    REQUIRE(contain.size() == 2);
    CHECK(contain[1].value.len.value == doctest::Approx(kBackgroundSizeContain));

    std::vector<mini_css::Decl> sized = parse("background-size: 12px", Status::kOk);
    REQUIRE(sized.size() == 2);
    CHECK(sized[0].value.len.unit == LengthUnit::Px);
    CHECK(sized[0].value.len.value == doctest::Approx(12));
    CHECK(sized[1].value.len.unit == LengthUnit::Auto);
    CHECK(sized[1].value.len.value == doctest::Approx(0));

    std::vector<mini_css::Decl> two = parse("background-size: 50% 2em", Status::kOk);
    REQUIRE(two.size() == 2);
    CHECK(two[0].value.len.unit == LengthUnit::Percent);
    CHECK(two[1].value.len.unit == LengthUnit::Em);

    std::vector<mini_css::Decl> pos = parse("background-position: right 10px", Status::kOk);
    REQUIRE(pos.size() == 2);
    CHECK(pos[0].prop == prop::BackgroundPositionX);
    CHECK(pos[0].value.len.value == doctest::Approx(100));
    CHECK(pos[0].value.len.unit == LengthUnit::Percent);
    CHECK(pos[1].prop == prop::BackgroundPositionY);
    CHECK(pos[1].value.len.value == doctest::Approx(10));

    std::vector<mini_css::Decl> rep =
        parse("background-repeat: repeat-y; image-rendering: pixelated", Status::kOk);
    REQUIRE(rep.size() == 2);
    CHECK(rep[0].prop == sp::BackgroundRepeat);
    CHECK(rep[0].value.e == 4u);
    CHECK(rep[1].prop == sp::ImageRendering);
    CHECK(rep[1].value.e == 1u);

    parse("background-size: cover contain", Status::kInvalidArgument);
    parse("background-size: -1px", Status::kInvalidArgument);
    parse("background-position: top left", Status::kInvalidArgument);
    parse("background-repeat: space", Status::kInvalidArgument);
}

// T-332 (05 §5.9.8): the runtime parser mirrors the compiler's `clip-path` grammar.
TEST_CASE("mini_css: clip-path shapes, none and bad values") {
    std::vector<mini_css::Decl> decls = parse("clip-path: circle(closest-side at 10px 50%)", Status::kOk);
    REQUIRE(decls.size() == 1);
    CHECK(decls[0].prop == sp::ClipPath);
    CHECK(decls[0].value.type == ValueType::Enum);
    CHECK(decls[0].clipShape.kind == ClipShapeKind::Circle);
    REQUIRE(decls[0].clipShape.params.size() == kClipCircleParamCount);
    CHECK(decls[0].clipShape.params[0].unit == LengthUnit::Auto);
    CHECK(decls[0].clipShape.params[0].value == kClipRadiusClosestSide);
    CHECK(decls[0].clipShape.params[1].value == doctest::Approx(10));
    CHECK(decls[0].clipShape.params[2].unit == LengthUnit::Percent);

    decls = parse("clip-path: inset(5% round 4px)", Status::kOk);
    REQUIRE(decls.size() == 1);
    CHECK(decls[0].clipShape.kind == ClipShapeKind::Inset);
    REQUIRE(decls[0].clipShape.params.size() == kClipInsetParamCount);
    CHECK(decls[0].clipShape.params[3].value == doctest::Approx(5));
    CHECK(decls[0].clipShape.params[11].value == doctest::Approx(4));

    decls = parse("clip-path: ellipse(40px 20px)", Status::kOk);
    REQUIRE(decls.size() == 1);
    CHECK(decls[0].clipShape.kind == ClipShapeKind::Ellipse);
    CHECK(decls[0].clipShape.params[2].value == doctest::Approx(50));

    decls = parse("clip-path: polygon(0 0, 100% 0, 100% 100%, 0 100%)", Status::kOk);
    REQUIRE(decls.size() == 1);
    CHECK(decls[0].clipShape.kind == ClipShapeKind::Polygon);
    CHECK(decls[0].clipShape.params.size() == 8);

    decls = parse("clip-path: none", Status::kOk);
    REQUIRE(decls.size() == 1);
    CHECK(decls[0].clipShape.kind == ClipShapeKind::None);
    CHECK(decls[0].value.e == 0);

    parse("clip-path: path(\"M0 0\")", Status::kInvalidArgument);
    parse("clip-path: url(#x)", Status::kInvalidArgument);
    parse("clip-path: circle(1em)", Status::kInvalidArgument);
    parse("clip-path: polygon(0 0, 1px 1px)", Status::kInvalidArgument);
}
