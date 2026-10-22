#include <initializer_list>
#include <string>
#include <vector>

#include "../calc_emit.h"  // foldCalcToPx (T-330, internal).
#include "doctest.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/parse_xts.h"
#include "xt/compile/style_model.h"
#include "xt/node.h"
#include "xt/property.h"
#include "xts_props.h"  // styleprop::*, AnimDirection/AnimFillMode/AnimPlayState (internal).

/// Tests for per-property value parsing and shorthand expansion (Code/Runtime/compile/src/xts/xts_values.cpp).
/// See Docs/design/05-style-and-css.md §5.3-5.6, Docs/tasks/T-22.md.

using namespace xt;
using namespace xt::compile;

namespace {

const StyleDecl* findDecl(const std::vector<StyleDecl>& decls, uint16_t prop) {
    for (const StyleDecl& d : decls) {
        if (d.prop == prop) {
            return &d;
        }
    }
    return nullptr;
}

std::vector<StyleDecl> parseOneRule(const std::string& body, DiagnosticSink& sink, bool& ok) {
    RuleSet rs;
    std::string src = "a { " + body + " }";
    ok = parseXts(src, "t.xts", 1, sink, rs);
    if (rs.rules.empty()) {
        return {};
    }
    return rs.rules[0].decls;
}

}  // namespace

TEST_CASE("values: margin shorthand, 2-value CSS rule") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("margin: 1px 2px;", sink, ok);
    CHECK(ok);
    REQUIRE(decls.size() == 4);
    CHECK(findDecl(decls, prop::MarginTop)->value.len.value == doctest::Approx(1));
    CHECK(findDecl(decls, prop::MarginBottom)->value.len.value == doctest::Approx(1));
    CHECK(findDecl(decls, prop::MarginRight)->value.len.value == doctest::Approx(2));
    CHECK(findDecl(decls, prop::MarginLeft)->value.len.value == doctest::Approx(2));
}

TEST_CASE("values: border shorthand, width+color in either order") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("border: 2px red;", sink, ok);
    CHECK(ok);
    REQUIRE(decls.size() == 5);
    CHECK(findDecl(decls, prop::BorderTopWidth)->value.len.value == doctest::Approx(2));
    CHECK(findDecl(decls, prop::BorderColor)->value.color.r == doctest::Approx(1));

    DiagnosticSink sink2;
    bool ok2;
    auto decls2 = parseOneRule("border: red 2px;", sink2, ok2);
    CHECK(ok2);
    REQUIRE(decls2.size() == 5);
    CHECK(findDecl(decls2, prop::BorderLeftWidth)->value.len.value == doctest::Approx(2));
}

TEST_CASE("values: flex shorthand -- single number and 'none'") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("flex: 1;", sink, ok);
    CHECK(ok);
    REQUIRE(decls.size() == 3);
    CHECK(findDecl(decls, prop::FlexGrow)->value.f == doctest::Approx(1));
    CHECK(findDecl(decls, prop::FlexShrink)->value.f == doctest::Approx(1));
    CHECK(findDecl(decls, prop::FlexBasis)->value.len.unit == LengthUnit::Px);

    DiagnosticSink sink2;
    bool ok2;
    auto decls2 = parseOneRule("flex: none;", sink2, ok2);
    CHECK(ok2);
    REQUIRE(decls2.size() == 3);
    CHECK(findDecl(decls2, prop::FlexGrow)->value.f == doctest::Approx(0));
    CHECK(findDecl(decls2, prop::FlexShrink)->value.f == doctest::Approx(0));
    CHECK(findDecl(decls2, prop::FlexBasis)->value.len.unit == LengthUnit::Auto);
}

TEST_CASE("values: transform -- valid sequence expands to multiple decls") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("transform: translate(10px, 5%) rotate(45deg) scale(2);", sink, ok);
    CHECK(ok);
    REQUIRE(decls.size() == 5);
    const StyleDecl* tx = findDecl(decls, prop::TranslateX);
    REQUIRE(tx != nullptr);
    CHECK(tx->value.len.value == doctest::Approx(10));
    CHECK(tx->value.len.unit == LengthUnit::Px);
    const StyleDecl* ty = findDecl(decls, prop::TranslateY);
    REQUIRE(ty != nullptr);
    CHECK(ty->value.len.value == doctest::Approx(5));
    CHECK(ty->value.len.unit == LengthUnit::Percent);
    const StyleDecl* rot = findDecl(decls, prop::Rotation);
    REQUIRE(rot != nullptr);
    CHECK(rot->value.f == doctest::Approx(45));
    CHECK(findDecl(decls, prop::ScaleX)->value.f == doctest::Approx(2));
    CHECK(findDecl(decls, prop::ScaleY)->value.f == doctest::Approx(2));
    CHECK(findDecl(decls, prop::SkewX) == nullptr);
}

// 05.5 (T-120): an out-of-order sequence is no longer an error -- it composes to a 4x4 and
// decomposes back into the full component set. See Tests/unit/test_transform_compose.cpp.
TEST_CASE("values: transform -- out-of-order sequence composes and decomposes") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("transform: scale(2) translate(10px);", sink, ok);
    CHECK(ok);
    CHECK(sink.hasErrors() == false);
    REQUIRE(decls.size() == 10);
    // scale(2) translate(10px) == translate(20px) scale(2).
    CHECK(findDecl(decls, prop::TranslateX)->value.len.value == doctest::Approx(20));
    CHECK(findDecl(decls, prop::ScaleX)->value.f == doctest::Approx(2));
    CHECK(findDecl(decls, prop::ScaleY)->value.f == doctest::Approx(2));
}

// Docs/design/19-3d-space.md §19.10, Docs/tasks/T-47a.md.
TEST_CASE("values: transform -- 3D functions (translate3d/rotateX/rotateY) expand in order") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule(
        "transform: translate3d(1px, 2px, 3px) rotateX(10deg) rotateY(20deg) rotate(30deg) scale(2);", sink,
        ok);
    CHECK(ok);
    REQUIRE(decls.size() == 8);
    CHECK(decls[0].prop == prop::TranslateX);
    CHECK(decls[0].value.len.value == doctest::Approx(1));
    CHECK(decls[1].prop == prop::TranslateY);
    CHECK(decls[1].value.len.value == doctest::Approx(2));
    CHECK(decls[2].prop == prop::TranslateZ);
    CHECK(decls[2].value.len.value == doctest::Approx(3));
    CHECK(decls[3].prop == prop::RotationX);
    CHECK(decls[3].value.f == doctest::Approx(10));
    CHECK(decls[4].prop == prop::RotationY);
    CHECK(decls[4].value.f == doctest::Approx(20));
    CHECK(decls[5].prop == prop::Rotation);
    CHECK(decls[5].value.f == doctest::Approx(30));
    CHECK(decls[6].prop == prop::ScaleX);
    CHECK(decls[7].prop == prop::ScaleY);
}

// Docs/design/05-style-and-css.md §5.5's `none` clause, Docs/tasks/T-69.md.
TEST_CASE("values: transform -- `none` is every component at its identity") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("transform: none;", sink, ok);
    CHECK(ok);
    CHECK(sink.all().empty());  // 05.5: `none` is an explicit identity, so no warning.
    REQUIRE(decls.size() == 10);
    CHECK(findDecl(decls, prop::TranslateX)->value.len.value == doctest::Approx(0));
    CHECK(findDecl(decls, prop::TranslateX)->value.len.unit == LengthUnit::Px);
    CHECK(findDecl(decls, prop::TranslateY)->value.len.value == doctest::Approx(0));
    CHECK(findDecl(decls, prop::TranslateZ)->value.f == doctest::Approx(0));
    CHECK(findDecl(decls, prop::RotationX)->value.f == doctest::Approx(0));
    CHECK(findDecl(decls, prop::RotationY)->value.f == doctest::Approx(0));
    CHECK(findDecl(decls, prop::Rotation)->value.f == doctest::Approx(0));
    CHECK(findDecl(decls, prop::SkewX)->value.f == doctest::Approx(0));
    CHECK(findDecl(decls, prop::SkewY)->value.f == doctest::Approx(0));
    CHECK(findDecl(decls, prop::ScaleX)->value.f == doctest::Approx(1));
    CHECK(findDecl(decls, prop::ScaleY)->value.f == doctest::Approx(1));
}

// 05.5: `none` may not be mixed with transform functions.
TEST_CASE("values: transform -- `none` mixed with a function is an invalid value") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("transform: none rotate(10deg);", sink, ok);
    CHECK(decls.empty());
    CHECK(sink.all().empty() == false);
    CHECK(sink.all()[0].message.find("invalid value") != std::string::npos);
}

TEST_CASE("values: transform -- rotate3d/scale3d/matrix3d/perspective() are unsupported") {
    for (const char* fn : {"rotate3d(1, 0, 0, 45deg)", "scale3d(1, 1, 1)",
                           "matrix3d(1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1)", "perspective(500px)"}) {
        DiagnosticSink sink;
        bool ok;
        std::string body = std::string("transform: ") + fn + ";";
        auto decls = parseOneRule(body, sink, ok);
        CAPTURE(fn);
        CHECK(ok == false);
        CHECK(sink.hasErrors());
        CHECK(decls.empty());
    }
}

TEST_CASE("values: transform -- translateZ rejects percent") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("transform: translateZ(10%);", sink, ok);
    CHECK(ok == false);
    CHECK(sink.hasErrors());
    CHECK(decls.empty());
}

// 05.5 (T-120): likewise for the 3D functions -- Rz * Rx decomposes back to rotate/rotateX.
TEST_CASE("values: transform -- rotate before rotateX composes and decomposes") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("transform: rotate(30deg) rotateX(10deg);", sink, ok);
    CHECK(ok);
    CHECK(sink.hasErrors() == false);
    REQUIRE(decls.size() == 10);
    CHECK(findDecl(decls, prop::Rotation)->value.f == doctest::Approx(30).epsilon(1e-4));
    CHECK(findDecl(decls, prop::RotationX)->value.f == doctest::Approx(10).epsilon(1e-4));
}

TEST_CASE("values: perspective -- none/px/em") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("perspective: none;", sink, ok);
    CHECK(ok);
    REQUIRE(decls.size() == 1);
    CHECK(decls[0].value.len.unit == LengthUnit::Auto);

    DiagnosticSink sink2;
    bool ok2;
    auto decls2 = parseOneRule("perspective: 500px;", sink2, ok2);
    CHECK(ok2);
    REQUIRE(decls2.size() == 1);
    CHECK(decls2[0].value.len.value == doctest::Approx(500));
    CHECK(decls2[0].value.len.unit == LengthUnit::Px);

    DiagnosticSink sink3;
    bool ok3;
    auto decls3 = parseOneRule("perspective: 20em;", sink3, ok3);
    CHECK(ok3);
    REQUIRE(decls3.size() == 1);
    CHECK(decls3[0].value.len.value == doctest::Approx(20));
    CHECK(decls3[0].value.len.unit == LengthUnit::Em);
}

TEST_CASE("values: perspective-origin -- keyword and length forms") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("perspective-origin: left top;", sink, ok);
    CHECK(ok);
    REQUIRE(decls.size() == 2);
    CHECK(findDecl(decls, prop::PerspectiveOriginX)->value.len.value == doctest::Approx(0));
    CHECK(findDecl(decls, prop::PerspectiveOriginY)->value.len.value == doctest::Approx(0));

    DiagnosticSink sink2;
    bool ok2;
    auto decls2 = parseOneRule("perspective-origin: 25% 75%;", sink2, ok2);
    CHECK(ok2);
    REQUIRE(decls2.size() == 2);
    CHECK(findDecl(decls2, prop::PerspectiveOriginX)->value.len.value == doctest::Approx(25));
    CHECK(findDecl(decls2, prop::PerspectiveOriginY)->value.len.value == doctest::Approx(75));

    DiagnosticSink sink3;
    bool ok3;
    auto decls3 = parseOneRule("perspective-origin: 10px 20px;", sink3, ok3);
    CHECK(ok3);
    REQUIRE(decls3.size() == 2);
    CHECK(findDecl(decls3, prop::PerspectiveOriginX)->value.len.value == doctest::Approx(10));
    CHECK(findDecl(decls3, prop::PerspectiveOriginX)->value.len.unit == LengthUnit::Px);
    CHECK(findDecl(decls3, prop::PerspectiveOriginY)->value.len.value == doctest::Approx(20));
}

TEST_CASE("values: backface-visibility -- hidden") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("backface-visibility: hidden;", sink, ok);
    CHECK(ok);
    REQUIRE(decls.size() == 1);
    CHECK(decls[0].prop == styleprop::BackfaceVisibility);
    CHECK(decls[0].value.e == 1);
}

TEST_CASE("values: transform-origin -- one value defaults the second to 50%") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("transform-origin: left top;", sink, ok);
    CHECK(ok);
    REQUIRE(decls.size() == 2);
    CHECK(findDecl(decls, prop::AnchorX)->value.len.value == doctest::Approx(0));
    CHECK(findDecl(decls, prop::AnchorY)->value.len.value == doctest::Approx(0));

    DiagnosticSink sink2;
    bool ok2;
    auto decls2 = parseOneRule("transform-origin: 10px;", sink2, ok2);
    CHECK(ok2);
    REQUIRE(decls2.size() == 2);
    CHECK(findDecl(decls2, prop::AnchorX)->value.len.value == doctest::Approx(10));
    CHECK(findDecl(decls2, prop::AnchorY)->value.len.value == doctest::Approx(50));
    CHECK(findDecl(decls2, prop::AnchorY)->value.len.unit == LengthUnit::Percent);
}

TEST_CASE("values: transition list -- multiple items, prop/all, easing, delay") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("transition: opacity 200ms ease-in 50ms, all 1s;", sink, ok);
    CHECK(ok);
    REQUIRE(decls.size() == 1);
    const StyleDecl* d = findDecl(decls, styleprop::Transition);
    REQUIRE(d != nullptr);
    REQUIRE(d->value.transitions.size() == 2);
    const TransitionSpec& t0 = d->value.transitions[0];
    CHECK(t0.prop == prop::Opacity);
    CHECK(t0.duration == doctest::Approx(0.2f));
    CHECK(t0.delay == doctest::Approx(0.05f));
    CHECK(t0.easing.kind == EasingKind::CubicBezier);
    const TransitionSpec& t1 = d->value.transitions[1];
    CHECK(t1.prop == 0xFFFF);
    CHECK(t1.duration == doctest::Approx(1.0f));
    CHECK(t1.delay == doctest::Approx(0.0f));
}

// T-79 item 1: `transform` / `transform-origin` / `perspective-origin` each name several
// animatable PropIds, so a transition item over one expands into one spec per component instead of
// warning "unknown or non-animatable transition property".
TEST_CASE("values: transition -- transform and the origin shorthands expand per component") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("transition: transform 300ms ease-in 50ms;", sink, ok);
    CHECK(ok);
    CHECK(sink.warningCount() == 0);
    const StyleDecl* d = findDecl(decls, styleprop::Transition);
    REQUIRE(d != nullptr);
    const std::vector<TransitionSpec>& specs = d->value.transitions;
    REQUIRE(specs.size() == 10);
    const PropId expected[] = {prop::TranslateX, prop::TranslateY, prop::TranslateZ, prop::RotationX,
                               prop::RotationY,  prop::Rotation,   prop::SkewX,      prop::SkewY,
                               prop::ScaleX,     prop::ScaleY};
    for (size_t i = 0; i < specs.size(); ++i) {
        CAPTURE(i);
        CHECK(specs[i].prop == expected[i]);
        CHECK(specs[i].duration == doctest::Approx(0.3f));
        CHECK(specs[i].delay == doctest::Approx(0.05f));
        CHECK(specs[i].easing.kind == EasingKind::CubicBezier);
    }

    DiagnosticSink sink2;
    bool ok2;
    auto decls2 = parseOneRule("transition: transform-origin 1s, perspective-origin 2s;", sink2, ok2);
    CHECK(ok2);
    CHECK(sink2.warningCount() == 0);
    const StyleDecl* d2 = findDecl(decls2, styleprop::Transition);
    REQUIRE(d2 != nullptr);
    REQUIRE(d2->value.transitions.size() == 4);
    CHECK(d2->value.transitions[0].prop == prop::AnchorX);
    CHECK(d2->value.transitions[1].prop == prop::AnchorY);
    CHECK(d2->value.transitions[1].duration == doctest::Approx(1.0f));
    CHECK(d2->value.transitions[2].prop == prop::PerspectiveOriginX);
    CHECK(d2->value.transitions[3].prop == prop::PerspectiveOriginY);
    CHECK(d2->value.transitions[3].duration == doctest::Approx(2.0f));

    // A shorthand with no animatable components is still rejected, with its warning.
    DiagnosticSink sink3;
    bool ok3;
    parseOneRule("transition: margin 1s;", sink3, ok3);
    CHECK(sink3.warningCount() == 1);
}

TEST_CASE("values: animation list -- full keyword set") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("animation: spin 2s linear infinite alternate both paused;", sink, ok);
    CHECK(ok);
    REQUIRE(decls.size() == 1);
    const StyleDecl* d = findDecl(decls, styleprop::Animation);
    REQUIRE(d != nullptr);
    REQUIRE(d->value.animations.size() == 1);
    const AnimationSpec& a = d->value.animations[0];
    CHECK(a.name == "spin");
    CHECK(a.duration == doctest::Approx(2.0f));
    CHECK(a.easing.kind == EasingKind::Linear);
    CHECK(a.iterations == 0xFFFF);
    CHECK(a.direction == static_cast<uint8_t>(AnimDirection::Alternate));
    CHECK(a.fill == static_cast<uint8_t>(AnimFillMode::Both));
    CHECK(a.playState == static_cast<uint8_t>(AnimPlayState::Paused));
}

TEST_CASE("values: colors -- rgba with fractional alpha, #f00, #ff000080") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("color: rgba(255, 0, 0, .5);", sink, ok);
    CHECK(ok);
    const StyleDecl* c = findDecl(decls, prop::Color);
    REQUIRE(c != nullptr);
    CHECK(c->value.color.r == doctest::Approx(1));
    CHECK(c->value.color.a == doctest::Approx(0.5f));

    DiagnosticSink sink2;
    bool ok2;
    auto decls2 = parseOneRule("color: #f00;", sink2, ok2);
    CHECK(ok2);
    const StyleDecl* c2 = findDecl(decls2, prop::Color);
    REQUIRE(c2 != nullptr);
    CHECK(c2->value.color.r == doctest::Approx(1));
    CHECK(c2->value.color.g == doctest::Approx(0));
    CHECK(c2->value.color.a == doctest::Approx(1));

    DiagnosticSink sink3;
    bool ok3;
    auto decls3 = parseOneRule("color: #ff000080;", sink3, ok3);
    CHECK(ok3);
    const StyleDecl* c3 = findDecl(decls3, prop::Color);
    REQUIRE(c3 != nullptr);
    CHECK(c3->value.color.r == doctest::Approx(1));
    CHECK(c3->value.color.a == doctest::Approx(128.0f / 255.0f));
}

TEST_CASE("values: line-height unitless -> Em") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("line-height: 1.5;", sink, ok);
    CHECK(ok);
    const StyleDecl* lh = findDecl(decls, prop::LineHeight);
    REQUIRE(lh != nullptr);
    CHECK(lh->value.len.unit == LengthUnit::Em);
    CHECK(lh->value.len.value == doctest::Approx(1.5f));
}

TEST_CASE("values: font-family -- quoted and bare idents") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("font-family: \"Noto Sans\", sans-serif;", sink, ok);
    CHECK(ok);
    const StyleDecl* f = findDecl(decls, styleprop::FontFamily);
    REQUIRE(f != nullptr);
    REQUIRE(f->value.families.size() == 2);
    CHECK(f->value.families[0] == "Noto Sans");
    CHECK(f->value.families[1] == "sans-serif");
}

TEST_CASE("values: background-image -- url(...) and none") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("background-image: url(a.png);", sink, ok);
    CHECK(ok);
    const StyleDecl* b = findDecl(decls, styleprop::BackgroundImage);
    REQUIRE(b != nullptr);
    CHECK(b->value.str == "a.png");

    DiagnosticSink sink2;
    bool ok2;
    auto decls2 = parseOneRule("background-image: none;", sink2, ok2);
    CHECK(ok2);
    CHECK(findDecl(decls2, styleprop::BackgroundImage)->value.str == "none");
}

TEST_CASE("values: z-index -- negative unitless integer") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("z-index: -1;", sink, ok);
    CHECK(ok);
    const StyleDecl* z = findDecl(decls, styleprop::ZIndex);
    REQUIRE(z != nullptr);
    CHECK(z->value.f == doctest::Approx(-1));
}

TEST_CASE("parseValue: single non-shorthand value, no diagnostics") {
    StyleValue v;
    CHECK(parseValue(prop::Width, "10px", v));
    CHECK(v.kind == StyleValueKind::Length);
    CHECK(v.len.value == doctest::Approx(10));
    CHECK(v.len.unit == LengthUnit::Px);

    StyleValue v2;
    CHECK(parseValue(prop::Color, "#000", v2));
    CHECK(v2.color.r == doctest::Approx(0));

    StyleValue v3;
    CHECK(parseValue(prop::Width, "not-a-length", v3) == false);
}

// -------------------------------------------------------------------------------------------
// effect (Docs/design/18-effects.md §18.3-18.4, Docs/tasks/T-51a.md).
// -------------------------------------------------------------------------------------------

TEST_CASE("values: effect -- shadow() defaults and full arguments") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("effect: shadow(2px 4px #000);", sink, ok);
    CHECK(ok);
    const StyleDecl* d = findDecl(decls, styleprop::Effect);
    REQUIRE(d != nullptr);
    REQUIRE(d->value.effects.size() == 1);
    const EffectDeclEntry& e = d->value.effects[0];
    CHECK(e.kind == EffectKind::DropShadow);
    CHECK(e.params[0] == doctest::Approx(2));  // offsetX
    CHECK(e.params[1] == doctest::Approx(4));  // offsetY
    CHECK(e.params[2] == doctest::Approx(0));  // blur default
    CHECK(e.params[3] == doctest::Approx(0));  // spread default
    CHECK(e.params[4] == doctest::Approx(1));  // opacity default
    CHECK(e.colors[0].r == doctest::Approx(0));

    DiagnosticSink sink2;
    bool ok2;
    auto decls2 = parseOneRule("effect: shadow(2px 4px 6px 1px rgba(0,0,0,0.5) 0.8);", sink2, ok2);
    CHECK(ok2);
    const EffectDeclEntry& e2 = findDecl(decls2, styleprop::Effect)->value.effects[0];
    CHECK(e2.params[2] == doctest::Approx(6));     // blur
    CHECK(e2.params[3] == doctest::Approx(1));     // spread
    CHECK(e2.params[4] == doctest::Approx(0.8f));  // opacity
    CHECK(e2.colors[0].a == doctest::Approx(0.5f));
}

// T-111: the trailing `inset` keyword (18.4) sets params[5]; it is only valid last.
TEST_CASE("values: effect -- shadow() inset keyword") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("effect: shadow(2px 2px 4px 0 #000 0.5 inset);", sink, ok);
    CHECK(ok);
    const EffectDeclEntry& e = findDecl(decls, styleprop::Effect)->value.effects[0];
    CHECK(e.kind == EffectKind::DropShadow);
    CHECK(e.params[4] == doctest::Approx(0.5f));
    CHECK(e.params[5] == doctest::Approx(1));

    // Without the keyword, and with the keyword but no opacity.
    DiagnosticSink sink2;
    bool ok2;
    auto decls2 = parseOneRule("effect: shadow(2px 2px 4px 0 #000 0.5);", sink2, ok2);
    CHECK(ok2);
    CHECK(findDecl(decls2, styleprop::Effect)->value.effects[0].params[5] == doctest::Approx(0));

    DiagnosticSink sink3;
    bool ok3;
    auto decls3 = parseOneRule("effect: shadow(2px 2px #000 inset);", sink3, ok3);
    CHECK(ok3);
    const EffectDeclEntry& e3 = findDecl(decls3, styleprop::Effect)->value.effects[0];
    CHECK(e3.params[4] == doctest::Approx(1));  // opacity default
    CHECK(e3.params[5] == doctest::Approx(1));

    // `inset` anywhere but last is an error.
    DiagnosticSink sink4;
    bool ok4;
    auto decls4 = parseOneRule("effect: shadow(2px 2px inset 4px #000);", sink4, ok4);
    // Dropped with a warning, the same treatment as any other bad effect argument list.
    CHECK(ok4 == true);
    CHECK(findDecl(decls4, styleprop::Effect) == nullptr);
    CHECK(sink4.warningCount() > 0);
}

TEST_CASE("values: effect -- glow() defaults and inner") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("effect: glow(3px #fff);", sink, ok);
    CHECK(ok);
    const EffectDeclEntry& e = findDecl(decls, styleprop::Effect)->value.effects[0];
    CHECK(e.kind == EffectKind::Glow);
    CHECK(e.params[0] == doctest::Approx(3));  // radius
    CHECK(e.params[1] == doctest::Approx(1));  // intensity default
    CHECK(e.params[2] == doctest::Approx(0));  // inner default
    CHECK(e.colors[0].r == doctest::Approx(1));

    DiagnosticSink sink2;
    bool ok2;
    auto decls2 = parseOneRule("effect: glow(3px #fff 2 inner);", sink2, ok2);
    CHECK(ok2);
    const EffectDeclEntry& e2 = findDecl(decls2, styleprop::Effect)->value.effects[0];
    CHECK(e2.params[1] == doctest::Approx(2));
    CHECK(e2.params[2] == doctest::Approx(1));
}

TEST_CASE("values: effect -- blur() single and two lengths") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("effect: blur(3px);", sink, ok);
    CHECK(ok);
    const EffectDeclEntry& e = findDecl(decls, styleprop::Effect)->value.effects[0];
    CHECK(e.kind == EffectKind::Blur);
    CHECK(e.params[0] == doctest::Approx(3));
    CHECK(e.params[1] == doctest::Approx(3));

    DiagnosticSink sink2;
    bool ok2;
    auto decls2 = parseOneRule("effect: blur(3px 5px);", sink2, ok2);
    CHECK(ok2);
    const EffectDeclEntry& e2 = findDecl(decls2, styleprop::Effect)->value.effects[0];
    CHECK(e2.params[0] == doctest::Approx(3));
    CHECK(e2.params[1] == doctest::Approx(5));
}

TEST_CASE("values: effect -- color-adjust() partial arguments, any order") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("effect: color-adjust(hue-rotate(30deg) brightness(0.2));", sink, ok);
    CHECK(ok);
    const EffectDeclEntry& e = findDecl(decls, styleprop::Effect)->value.effects[0];
    CHECK(e.kind == EffectKind::ColorAdjust);
    CHECK(e.params[0] == doctest::Approx(0.2f));  // brightness
    CHECK(e.params[1] == doctest::Approx(0));     // contrast default
    CHECK(e.params[2] == doctest::Approx(0));     // saturate default
    CHECK(e.params[3] == doctest::Approx(30));    // hue-rotate
    CHECK(e.params[4] == doctest::Approx(1));     // opacity default
}

TEST_CASE("values: effect -- T-334 color-adjust() invert() and sepia()") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("effect: color-adjust(sepia(40%) invert(0.25));", sink, ok);
    CHECK(ok);
    const EffectDeclEntry& e = findDecl(decls, styleprop::Effect)->value.effects[0];
    CHECK(e.kind == EffectKind::ColorAdjust);
    CHECK(e.params[4] == doctest::Approx(1));      // opacity default
    CHECK(e.params[5] == doctest::Approx(0.25f));  // invert
    CHECK(e.params[6] == doctest::Approx(0.4f));   // sepia, 40%

    // Empty argument = 1; out-of-range amounts clamp to 0..1.
    DiagnosticSink sink2;
    bool ok2;
    auto decls2 = parseOneRule("effect: color-adjust(invert() sepia(250%));", sink2, ok2);
    CHECK(ok2);
    const EffectDeclEntry& e2 = findDecl(decls2, styleprop::Effect)->value.effects[0];
    CHECK(e2.params[5] == doctest::Approx(1));
    CHECK(e2.params[6] == doctest::Approx(1));
    DiagnosticSink sink3;
    bool ok3;
    auto decls3 = parseOneRule("effect: color-adjust(invert(-0.5) brightness(0.1));", sink3, ok3);
    CHECK(ok3);
    const EffectDeclEntry& e3 = findDecl(decls3, styleprop::Effect)->value.effects[0];
    CHECK(e3.params[5] == doctest::Approx(0));
    CHECK(e3.params[6] == doctest::Approx(0));  // sepia default (identity)
    CHECK(e3.params[0] == doctest::Approx(0.1f));
}

TEST_CASE("values: effect -- T-334 invert()/sepia() reject units, duplicates and two arguments") {
    const char* bad[] = {"effect: color-adjust(invert(10px));", "effect: color-adjust(sepia(1) sepia(0.5));",
                         "effect: color-adjust(invert(0.1 0.2));"};
    for (const char* text : bad) {
        CAPTURE(text);
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule(text, sink, ok);
        CHECK(findDecl(decls, styleprop::Effect) == nullptr);
    }
}

TEST_CASE("values: effect -- gradient-mask() stops and invert") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule(
        "effect: gradient-mask(linear, 0px 0px, 10px 10px, #fff 0%, #000 100%, invert);", sink, ok);
    CHECK(ok);
    const EffectDeclEntry& e = findDecl(decls, styleprop::Effect)->value.effects[0];
    CHECK(e.kind == EffectKind::GradientMask);
    CHECK(e.params[0] == doctest::Approx(0));
    CHECK(e.params[1] == doctest::Approx(0));
    CHECK(e.params[2] == doctest::Approx(10));
    CHECK(e.params[3] == doctest::Approx(10));
    CHECK(e.params[4] == doctest::Approx(0));  // linear
    CHECK(e.params[5] == doctest::Approx(1));  // invert
    REQUIRE(e.stops.size() == 2);
    CHECK(e.stops[0].offset == doctest::Approx(0));
    CHECK(e.stops[0].color.r == doctest::Approx(1));
    CHECK(e.stops[1].offset == doctest::Approx(1));
    CHECK(e.stops[1].color.r == doctest::Approx(0));
}

TEST_CASE("values: effect -- shine() defaults and full arguments") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("effect: shine(45deg 10px #fff);", sink, ok);
    CHECK(ok);
    const EffectDeclEntry& e = findDecl(decls, styleprop::Effect)->value.effects[0];
    CHECK(e.kind == EffectKind::Shine);
    CHECK(e.params[0] == doctest::Approx(0));     // position default
    CHECK(e.params[1] == doctest::Approx(10));    // width
    CHECK(e.params[2] == doctest::Approx(0.5f));  // softness default
    CHECK(e.params[3] == doctest::Approx(45));    // angle
    CHECK(e.params[4] == doctest::Approx(1));     // intensity default

    DiagnosticSink sink2;
    bool ok2;
    auto decls2 = parseOneRule("effect: shine(45deg 10px #fff 2 0.3);", sink2, ok2);
    CHECK(ok2);
    const EffectDeclEntry& e2 = findDecl(decls2, styleprop::Effect)->value.effects[0];
    CHECK(e2.params[4] == doctest::Approx(2));
    CHECK(e2.params[2] == doctest::Approx(0.3f));
    CHECK(e2.params[0] == doctest::Approx(0));  // position still defaults when omitted
}

TEST_CASE("values: effect -- shine() position sixth argument") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("effect: shine(20deg 30px #fff 1 0.5 0.75);", sink, ok);
    CHECK(ok);
    const EffectDeclEntry& e = findDecl(decls, styleprop::Effect)->value.effects[0];
    CHECK(e.kind == EffectKind::Shine);
    CHECK(e.params[0] == doctest::Approx(0.75f));  // position
    CHECK(e.params[1] == doctest::Approx(30));     // width
    CHECK(e.params[2] == doctest::Approx(0.5f));   // softness
    CHECK(e.params[3] == doctest::Approx(20));     // angle
    CHECK(e.params[4] == doctest::Approx(1));      // intensity
    CHECK_FALSE(sink.hasErrors());
}

TEST_CASE("values: effect -- shine() position out of range is clamped and warns") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("effect: shine(20deg 30px #fff 1 0.5 1.5);", sink, ok);
    CHECK(ok);
    const EffectDeclEntry& e = findDecl(decls, styleprop::Effect)->value.effects[0];
    CHECK(e.params[0] == doctest::Approx(1));  // clamped to 1
    bool found = false;
    for (const Diagnostic& diag : sink.all()) {
        if (diag.message.find("position out of range") != std::string::npos) {
            found = true;
        }
    }
    CHECK(found);

    DiagnosticSink sink2;
    bool ok2;
    auto decls2 = parseOneRule("effect: shine(20deg 30px #fff 1 0.5 -0.5);", sink2, ok2);
    CHECK(ok2);
    const EffectDeclEntry& e2 = findDecl(decls2, styleprop::Effect)->value.effects[0];
    CHECK(e2.params[0] == doctest::Approx(0));  // clamped to 0
    bool found2 = false;
    for (const Diagnostic& diag : sink2.all()) {
        if (diag.message.find("position out of range") != std::string::npos) {
            found2 = true;
        }
    }
    CHECK(found2);
}

TEST_CASE("values: effect -- outline() defaults and full arguments") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("effect: outline(2px #f00);", sink, ok);
    CHECK(ok);
    const EffectDeclEntry& e = findDecl(decls, styleprop::Effect)->value.effects[0];
    CHECK(e.kind == EffectKind::Outline);
    CHECK(e.params[0] == doctest::Approx(2));  // radius
    CHECK(e.params[1] == doctest::Approx(0));  // softness default
    CHECK(e.params[2] == doctest::Approx(0));  // position default (outer)

    DiagnosticSink sink2;
    bool ok2;
    auto decls2 = parseOneRule("effect: outline(2px #f00 1px center);", sink2, ok2);
    CHECK(ok2);
    const EffectDeclEntry& e2 = findDecl(decls2, styleprop::Effect)->value.effects[0];
    CHECK(e2.params[1] == doctest::Approx(1));
    CHECK(e2.params[2] == doctest::Approx(1));  // center
}

TEST_CASE("values: effect -- backdrop-blur() defaults and full arguments") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("effect: backdrop-blur(12px);", sink, ok);
    CHECK(ok);
    const EffectDeclEntry& e = findDecl(decls, styleprop::Effect)->value.effects[0];
    CHECK(e.kind == EffectKind::BackdropBlur);
    CHECK(e.params[0] == doctest::Approx(12));  // radius
    CHECK(e.params[1] == doctest::Approx(0));   // saturation default
    CHECK(e.params[2] == doctest::Approx(0));   // brightness default

    DiagnosticSink sink2;
    bool ok2;
    auto decls2 = parseOneRule("effect: backdrop-blur(8px 0.4 -0.1);", sink2, ok2);
    CHECK(ok2);
    const EffectDeclEntry& e2 = findDecl(decls2, styleprop::Effect)->value.effects[0];
    CHECK(e2.params[0] == doctest::Approx(8));
    CHECK(e2.params[1] == doctest::Approx(0.4));
    CHECK(e2.params[2] == doctest::Approx(-0.1));
}

// Docs/design/18-effects.md §18.6: BackdropBlur only reads what was drawn before the node's
// own layer was opened, so it is meaningful only as the stack's first entry.
TEST_CASE("values: effect -- backdrop-blur() must come first") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("effect: blur(2px) backdrop-blur(8px);", sink, ok);
    CHECK(ok == false);
    CHECK(sink.hasErrors());
    bool found = false;
    for (const Diagnostic& diag : sink.all()) {
        if (diag.message.find("backdrop-blur must come first") != std::string::npos) {
            found = true;
        }
    }
    CHECK(found);

    // First is fine, whatever follows it.
    DiagnosticSink sink2;
    bool ok2;
    auto decls2 = parseOneRule("effect: backdrop-blur(8px) blur(2px);", sink2, ok2);
    CHECK(ok2);
    const StyleDecl* d = findDecl(decls2, styleprop::Effect);
    REQUIRE(d != nullptr);
    REQUIRE(d->value.effects.size() == 2);
    CHECK(d->value.effects[0].kind == EffectKind::BackdropBlur);
    CHECK(d->value.effects[1].kind == EffectKind::Blur);
}

TEST_CASE("values: effect -- sequence of multiple functions") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("effect: shadow(2px 4px 6px #0008) blur(3px);", sink, ok);
    CHECK(ok);
    const StyleDecl* d = findDecl(decls, styleprop::Effect);
    REQUIRE(d != nullptr);
    REQUIRE(d->value.effects.size() == 2);
    CHECK(d->value.effects[0].kind == EffectKind::DropShadow);
    CHECK(d->value.effects[1].kind == EffectKind::Blur);
}

TEST_CASE("values: effect -- none is an empty sequence") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("effect: none;", sink, ok);
    CHECK(ok);
    const StyleDecl* d = findDecl(decls, styleprop::Effect);
    REQUIRE(d != nullptr);
    CHECK(d->value.kind == StyleValueKind::EffectList);
    CHECK(d->value.effects.empty());
}

TEST_CASE("values: effect -- custom() parses the asset name and overrides (T-203)") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule(
        "effect: shadow(0 2px 4px #000) custom(\"ripple.xtfx\", amplitude 6, center 0.5 0.25, tint #ff0000);",
        sink, ok);
    CHECK(ok);
    CHECK_FALSE(sink.hasErrors());
    REQUIRE(decls.size() == 1);
    REQUIRE(decls[0].value.effects.size() == 2);
    const EffectDeclEntry& e = decls[0].value.effects[1];
    CHECK(e.kind == EffectKind::Custom);
    CHECK(e.customAsset == "ripple.xtfx");
    REQUIRE(e.customParams.size() == 3);
    CHECK(e.customParams[0].name == "amplitude");
    CHECK(e.customParams[0].count == 1);
    CHECK(e.customParams[0].value[0] == doctest::Approx(6));
    CHECK(e.customParams[1].count == 2);
    CHECK(e.customParams[1].value[1] == doctest::Approx(0.25));
    CHECK(e.customParams[2].isColor);
    CHECK(e.customParams[2].value[0] == doctest::Approx(1));
}

TEST_CASE("values: effect -- malformed custom() is a compile error (T-203)") {
    for (const char* text : {"effect: custom(ripple);", "effect: custom(\"r\" amplitude 1);",
                             "effect: custom(\"r\", amplitude);", "effect: custom(\"r\", a 1, a 2);"}) {
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule(text, sink, ok);
        CHECK_MESSAGE(sink.hasErrors(), text);
        CHECK(decls.empty());
    }
}

TEST_CASE("values: effect -- percent length is a compile error") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("effect: blur(10%);", sink, ok);
    CHECK(ok == false);
    bool found = false;
    for (const Diagnostic& diag : sink.all()) {
        if (diag.message.find("effect lengths do not accept percent") != std::string::npos) {
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("values: effect -- sequence longer than 16 is a compile error") {
    DiagnosticSink sink;
    bool ok;
    std::string body = "effect:";
    for (int i = 0; i < 17; ++i) {
        body += " blur(1px)";
    }
    body += ";";
    auto decls = parseOneRule(body, sink, ok);
    CHECK(ok == false);
    bool found = false;
    for (const Diagnostic& diag : sink.all()) {
        if (diag.message.find("at most 16") != std::string::npos) {
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("values: background-gradient -- linear, two stops, no positions") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("background-gradient: linear-gradient(90deg, #f00, #00f);", sink, ok);
    CHECK(ok);
    const StyleDecl* d = findDecl(decls, styleprop::BackgroundGradient);
    REQUIRE(d != nullptr);
    CHECK(d->value.kind == StyleValueKind::Gradient);
    const GradientDecl& g = d->value.gradient;
    CHECK(g.kind == GradientKind::Linear);
    CHECK(g.angle == doctest::Approx(90));
    REQUIRE(g.stops.size() == 2);
    CHECK(g.stops[0].offset == doctest::Approx(0));
    CHECK(g.stops[1].offset == doctest::Approx(1));
    CHECK(g.stops[0].color.r == doctest::Approx(1));
    CHECK(g.stops[1].color.b == doctest::Approx(1));
}

// T-160 (bench BUG-5): the bench's `#preview-tab` gradient, whose first stop is an 8-digit hex
// colour with alpha 0.
TEST_CASE("values: background-gradient -- 8-digit hex stops keep their alpha") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule(
        "background-gradient: linear-gradient(90deg, #1a261f00 0%, #2a3a30 40%, #3a5040 100%);", sink, ok);
    CHECK(ok);
    const StyleDecl* d = findDecl(decls, styleprop::BackgroundGradient);
    REQUIRE(d != nullptr);
    const GradientDecl& g = d->value.gradient;
    REQUIRE(g.stops.size() == 3);
    CHECK(g.stops[0].color.r == doctest::Approx(0x1a / 255.0f));
    CHECK(g.stops[0].color.g == doctest::Approx(0x26 / 255.0f));
    CHECK(g.stops[0].color.b == doctest::Approx(0x1f / 255.0f));
    CHECK(g.stops[0].color.a == doctest::Approx(0.0f));
    CHECK(g.stops[1].offset == doctest::Approx(0.4f));
    CHECK(g.stops[1].color.a == doctest::Approx(1.0f));
    CHECK(g.stops[2].color.g == doctest::Approx(0x50 / 255.0f));
}

TEST_CASE("values: background-gradient -- turn angle and explicit stop positions") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("background-gradient: linear-gradient(0.25turn, red 10%, blue 90%);", sink, ok);
    CHECK(ok);
    const StyleDecl* d = findDecl(decls, styleprop::BackgroundGradient);
    REQUIRE(d != nullptr);
    const GradientDecl& g = d->value.gradient;
    CHECK(g.kind == GradientKind::Linear);
    CHECK(g.angle == doctest::Approx(90));
    REQUIRE(g.stops.size() == 2);
    CHECK(g.stops[0].offset == doctest::Approx(0.1f));
    CHECK(g.stops[1].offset == doctest::Approx(0.9f));
}

TEST_CASE("values: background-gradient -- rad angle and evenly distributed middle stop") {
    DiagnosticSink sink;
    bool ok;
    auto decls =
        parseOneRule("background-gradient: linear-gradient(3.14159265rad, red, lime, blue);", sink, ok);
    CHECK(ok);
    const StyleDecl* d = findDecl(decls, styleprop::BackgroundGradient);
    REQUIRE(d != nullptr);
    const GradientDecl& g = d->value.gradient;
    CHECK(g.angle == doctest::Approx(180).epsilon(0.001));
    REQUIRE(g.stops.size() == 3);
    CHECK(g.stops[0].offset == doctest::Approx(0));
    CHECK(g.stops[1].offset == doctest::Approx(0.5f));
    CHECK(g.stops[2].offset == doctest::Approx(1));
}

TEST_CASE("values: background-gradient -- radial geometry") {
    DiagnosticSink sink;
    bool ok;
    auto decls =
        parseOneRule("background-gradient: radial-gradient(50% 50% at 30% 40%, #fff, #000);", sink, ok);
    CHECK(ok);
    const StyleDecl* d = findDecl(decls, styleprop::BackgroundGradient);
    REQUIRE(d != nullptr);
    const GradientDecl& g = d->value.gradient;
    CHECK(g.kind == GradientKind::Radial);
    CHECK(g.rx.value == doctest::Approx(50));
    CHECK(g.rx.unit == LengthUnit::Percent);
    CHECK(g.ry.value == doctest::Approx(50));
    CHECK(g.cx.value == doctest::Approx(30));
    CHECK(g.cy.value == doctest::Approx(40));
    REQUIRE(g.stops.size() == 2);
    CHECK(g.stops[0].color.r == doctest::Approx(1));
    CHECK(g.stops[1].color.r == doctest::Approx(0));
}

TEST_CASE("values: background-gradient -- none") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("background-gradient: none;", sink, ok);
    CHECK(ok);
    const StyleDecl* d = findDecl(decls, styleprop::BackgroundGradient);
    REQUIRE(d != nullptr);
    CHECK(d->value.kind == StyleValueKind::Gradient);
    CHECK(d->value.gradient.kind == GradientKind::None);
    CHECK(d->value.gradient.stops.empty());
}

TEST_CASE("values: background-gradient -- malformed values are dropped with a warning") {
    for (const char* body : {"background-gradient: linear-gradient(90deg #f00, #00f);",
                             "background-gradient: linear-gradient(90deg, #f00);",
                             "background-gradient: linear-gradient(#f00, #00f);",
                             "background-gradient: radial-gradient(50% 50% 30% 40%, #fff, #000);",
                             // T-201: conic-gradient exists; a stray token after its angle is malformed.
                             "background-gradient: conic-gradient(90deg 10%, #f00, #00f);"}) {
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule(body, sink, ok);
        CHECK(findDecl(decls, styleprop::BackgroundGradient) == nullptr);
        CHECK_FALSE(sink.all().empty());
    }
}

// ---------------------------------------------------------------------------------------------
// T-119 C2: `transition`/`animation` items are split on top-level commas only, so the commas
// inside `cubic-bezier()`/`steps()` no longer cut one item into several unparsable ones. The
// numbers inside them may be written without a leading zero (`.6`) or negative (`-.25`).
// ---------------------------------------------------------------------------------------------

TEST_CASE("values: T-119 animation with cubic-bezier(.6,0,.3,1) and forwards") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("animation: load 2.2s cubic-bezier(.6,0,.3,1) forwards;", sink, ok);
    CHECK(ok);
    CHECK(sink.all().empty());
    const StyleDecl* d = findDecl(decls, styleprop::Animation);
    REQUIRE(d != nullptr);
    REQUIRE(d->value.animations.size() == 1);
    const AnimationSpec& a = d->value.animations[0];
    CHECK(a.name == "load");
    CHECK(a.duration == doctest::Approx(2.2f));
    CHECK(a.easing.kind == EasingKind::CubicBezier);
    CHECK(a.easing.x1 == doctest::Approx(0.6f));
    CHECK(a.easing.y1 == doctest::Approx(0.0f));
    CHECK(a.easing.x2 == doctest::Approx(0.3f));
    CHECK(a.easing.y2 == doctest::Approx(1.0f));
    CHECK(a.fill == static_cast<uint8_t>(AnimFillMode::Forwards));
    CHECK(a.iterations == 1);
}

TEST_CASE("values: T-119 animation with a negative leading-dot bezier control point") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("animation: pop 1s cubic-bezier(-.25, 0, .5, 1.5);", sink, ok);
    CHECK(ok);
    CHECK(sink.all().empty());
    const StyleDecl* d = findDecl(decls, styleprop::Animation);
    REQUIRE(d != nullptr);
    REQUIRE(d->value.animations.size() == 1);
    const Easing& e = d->value.animations[0].easing;
    CHECK(e.kind == EasingKind::CubicBezier);
    CHECK(e.x1 == doctest::Approx(-0.25f));
    CHECK(e.y2 == doctest::Approx(1.5f));
}

TEST_CASE("values: T-119 two animation items, the first carrying a steps() easing") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("animation: tick 1s steps(4, start), fade 2s ease-in 0.5s;", sink, ok);
    CHECK(ok);
    CHECK(sink.all().empty());
    const StyleDecl* d = findDecl(decls, styleprop::Animation);
    REQUIRE(d != nullptr);
    REQUIRE(d->value.animations.size() == 2);
    CHECK(d->value.animations[0].name == "tick");
    CHECK(d->value.animations[0].easing.kind == EasingKind::Steps);
    CHECK(d->value.animations[0].easing.stepCount == 4);
    CHECK(d->value.animations[0].easing.stepsJumpEnd == 0);
    CHECK(d->value.animations[1].name == "fade");
    CHECK(d->value.animations[1].delay == doctest::Approx(0.5f));
}

// T-258b (Docs/reviews/2026-10-02-full-audit-raw/D.md D11): 0xFFFF stays the `infinite` sentinel.
TEST_CASE("values: T-258b an animation count of 65535 or more is clamped to 65534 with a warning") {
    for (const char* count : {"65535", "70000", "100000000000000000000"}) {
        CAPTURE(count);
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule(std::string("animation: spin 1s ") + count + ";", sink, ok);
        const StyleDecl* d = findDecl(decls, styleprop::Animation);
        REQUIRE(d != nullptr);
        REQUIRE(d->value.animations.size() == 1);
        CHECK(d->value.animations[0].iterations == 65534);
        CHECK(sink.warningCount() == 1);
    }
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("animation: spin 1s 65534;", sink, ok);
    CHECK(findDecl(decls, styleprop::Animation)->value.animations[0].iterations == 65534);
    CHECK(sink.all().empty());
}

TEST_CASE("values: T-258b steps(n) is clamped to 1..65535 with a warning") {
    struct Case {
        const char* text;
        uint16_t steps;
    };
    for (const Case& c : {Case{"steps(100000)", 65535}, Case{"steps(0)", 1}}) {
        CAPTURE(c.text);
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule(std::string("animation: tick 1s ") + c.text + ";", sink, ok);
        const StyleDecl* d = findDecl(decls, styleprop::Animation);
        REQUIRE(d != nullptr);
        REQUIRE(d->value.animations.size() == 1);
        CHECK(d->value.animations[0].easing.kind == EasingKind::Steps);
        CHECK(d->value.animations[0].easing.stepCount == c.steps);
        CHECK(sink.warningCount() == 1);
    }
}

TEST_CASE("values: T-258b font-weight beyond int is invalid, not undefined") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("font-weight: 3000000000;", sink, ok);
    CHECK(findDecl(decls, styleprop::FontWeight) == nullptr);
    CHECK(sink.warningCount() == 1);
}

TEST_CASE("values: T-119 transition with cubic-bezier() keeps one item per property") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("transition: opacity 300ms cubic-bezier(.4,0,.2,1), left 1s linear;", sink, ok);
    CHECK(ok);
    CHECK(sink.all().empty());
    const StyleDecl* d = findDecl(decls, styleprop::Transition);
    REQUIRE(d != nullptr);
    REQUIRE(d->value.transitions.size() == 2);
    CHECK(d->value.transitions[0].prop == prop::Opacity);
    CHECK(d->value.transitions[0].easing.kind == EasingKind::CubicBezier);
    CHECK(d->value.transitions[0].easing.x1 == doctest::Approx(0.4f));
    CHECK(d->value.transitions[1].prop == prop::Left);
    CHECK(d->value.transitions[1].easing.kind == EasingKind::Linear);
}

// ---------------------------------------------------------------------------------------------
// T-119 item 5: the per-side longhands of 05.4's `margin*`/`padding*`/`border-*-width` rows.
// 05.4 lists a single `border-color` (PropId 97) and no per-side border colors, so
// `border-left-color` and friends are deliberately not accepted.
// ---------------------------------------------------------------------------------------------

TEST_CASE("values: T-119 padding/margin/border-width side longhands") {
    struct Case {
        const char* decl;
        uint16_t prop;
    };
    const Case cases[] = {
        {"padding-top: 1px", prop::PaddingTop},
        {"padding-right: 2px", prop::PaddingRight},
        {"padding-bottom: 3px", prop::PaddingBottom},
        {"padding-left: 4px", prop::PaddingLeft},
        {"margin-top: 5px", prop::MarginTop},
        {"margin-right: 6px", prop::MarginRight},
        {"margin-bottom: 7px", prop::MarginBottom},
        {"margin-left: 8px", prop::MarginLeft},
        {"border-top-width: 9px", prop::BorderTopWidth},
        {"border-right-width: 10px", prop::BorderRightWidth},
        {"border-bottom-width: 11px", prop::BorderBottomWidth},
        {"border-left-width: 12px", prop::BorderLeftWidth},
    };
    for (const Case& c : cases) {
        CAPTURE(c.decl);
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule(std::string(c.decl) + ";", sink, ok);
        CHECK(ok);
        CHECK(sink.all().empty());
        REQUIRE(decls.size() == 1);
        CHECK(decls[0].prop == c.prop);
        CHECK(decls[0].value.kind == StyleValueKind::Length);
    }
}

TEST_CASE("values: T-119 per-side border colors are part of 05 since T-333") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("border-left-color: red;", sink, ok);
    CHECK(ok);
    CHECK(sink.all().empty());
    REQUIRE(decls.size() == 1);
    CHECK(decls[0].prop == prop::BorderLeftColor);
}

// T-325 (05 §5.9.8): the `outline-*` longhands and the `outline` shorthand.
TEST_CASE("values: T-325 outline longhands") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule(
        "outline-width: 3px; outline-color: #00ff00; outline-offset: -2px; outline-style: solid;", sink, ok);
    CHECK(ok);
    CHECK(sink.all().empty());
    REQUIRE(decls.size() == 4);
    const StyleDecl* w = findDecl(decls, prop::OutlineWidth);
    REQUIRE(w != nullptr);
    CHECK(w->value.kind == StyleValueKind::Length);
    CHECK(w->value.len.value == doctest::Approx(3));
    const StyleDecl* c = findDecl(decls, prop::OutlineColor);
    REQUIRE(c != nullptr);
    CHECK(c->value.kind == StyleValueKind::Color);
    CHECK(c->value.color.g == doctest::Approx(1));
    const StyleDecl* o = findDecl(decls, prop::OutlineOffset);
    REQUIRE(o != nullptr);
    CHECK(o->value.len.value == doctest::Approx(-2));
    const StyleDecl* s = findDecl(decls, styleprop::OutlineStyle);
    REQUIRE(s != nullptr);
    CHECK(s->value.kind == StyleValueKind::Enum);
    CHECK(s->value.e == 1u);
    CHECK(styleprop::OutlineStyle == 201);
    CHECK(prop::OutlineWidth == 129);
    CHECK(prop::OutlineColor == 130);
    CHECK(prop::OutlineOffset == 131);
}

TEST_CASE("values: T-325 outline-style takes only none | solid") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("outline-style: dashed;", sink, ok);
    CHECK(ok);
    CHECK(decls.empty());
    CHECK_FALSE(sink.all().empty());
}

TEST_CASE("values: T-325 outline shorthand, six forms") {
    struct Case {
        const char* decl;
        float width;     // Expected outline-width (px).
        uint32_t style;  // Expected outline-style.
        bool hasColor;   // Whether an outline-color declaration is written.
        float red;       // Expected outline-color red channel when written.
    };
    const Case cases[] = {
        {"outline: 2px solid red", 2, 1, true, 1}, {"outline: solid #0000ff 4px", 4, 1, true, 0},
        {"outline: red 1px solid", 1, 1, true, 1}, {"outline: none", 0, 0, false, 0},
        {"outline: 3px solid", 3, 1, false, 0},    {"outline: solid", 0, 1, false, 0},
    };
    for (const Case& c : cases) {
        CAPTURE(c.decl);
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule(std::string(c.decl) + ";", sink, ok);
        CHECK(ok);
        CHECK(sink.all().empty());
        REQUIRE(decls.size() == (c.hasColor ? 3u : 2u));
        const StyleDecl* w = findDecl(decls, prop::OutlineWidth);
        REQUIRE(w != nullptr);
        CHECK(w->value.len.value == doctest::Approx(c.width));
        const StyleDecl* s = findDecl(decls, styleprop::OutlineStyle);
        REQUIRE(s != nullptr);
        CHECK(s->value.e == c.style);
        const StyleDecl* col = findDecl(decls, prop::OutlineColor);
        CHECK((col != nullptr) == c.hasColor);
        if (col != nullptr) {
            CHECK(col->value.color.r == doctest::Approx(c.red));
        }
    }
}

TEST_CASE("values: T-325 outline shorthand rejects repeats and unknown parts") {
    const char* bad[] = {"outline: 2px 3px solid", "outline: solid solid", "outline: 2px dashed red",
                         "outline: red blue", "outline: 50% solid"};
    for (const char* body : bad) {
        CAPTURE(body);
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule(std::string(body) + ";", sink, ok);
        CHECK(decls.empty());
        CHECK_FALSE(sink.all().empty());
    }
}

TEST_CASE("values: T-325 transition accepts outline-width") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("transition: outline-width 0.1s, outline-color 0.2s;", sink, ok);
    CHECK(ok);
    CHECK(sink.all().empty());
    const StyleDecl* t = findDecl(decls, styleprop::Transition);
    REQUIRE(t != nullptr);
    REQUIRE(t->value.transitions.size() == 2);
    CHECK(t->value.transitions[0].prop == prop::OutlineWidth);
    CHECK(t->value.transitions[1].prop == prop::OutlineColor);
}

// ---------------------------------------------------------------------------------------------
// T-322: hsl()/hsla(), the CSS Color Level 4 named colours, rotateZ()/matrix(),
// step-start/step-end, transition-* longhands.
// ---------------------------------------------------------------------------------------------

namespace {

/// The color a one-declaration `color: <text>` rule parses to; fails the test when it does not.
Color parseColorValue(const std::string& text) {
    DiagnosticSink sink;
    bool ok = false;
    auto decls = parseOneRule("color: " + text + ";", sink, ok);
    CAPTURE(text);
    CHECK(ok);
    CHECK(sink.all().empty());
    const StyleDecl* c = findDecl(decls, prop::Color);
    REQUIRE(c != nullptr);
    return c->value.color;
}

void checkRgba(const Color& c, int r, int g, int b, float a) {
    CHECK(c.r == doctest::Approx(static_cast<float>(r) / 255.0f));
    CHECK(c.g == doctest::Approx(static_cast<float>(g) / 255.0f));
    CHECK(c.b == doctest::Approx(static_cast<float>(b) / 255.0f));
    CHECK(c.a == doctest::Approx(a));
}

/// True when `text` is rejected as a color (warned, nothing emitted).
bool colorRejected(const std::string& text) {
    DiagnosticSink sink;
    bool ok = false;
    auto decls = parseOneRule("color: " + text + ";", sink, ok);
    return decls.empty() && !sink.all().empty();
}

}  // namespace

TEST_CASE("T-322 values: hsl()/hsla() in the comma and the space syntax") {
    checkRgba(parseColorValue("hsl(120, 100%, 50%)"), 0, 255, 0, 1.0f);
    checkRgba(parseColorValue("hsla(240, 100%, 50%, 0.5)"), 0, 0, 255, 0.5f);
    checkRgba(parseColorValue("hsl(0 100% 50%)"), 255, 0, 0, 1.0f);
    // 0.5turn = 180deg; 25% lightness; alpha as a percentage after `/`.
    checkRgba(parseColorValue("hsl(0.5turn 100% 25% / 50%)"), 0, 128, 128, 0.5f);
    checkRgba(parseColorValue("hsl(3.14159265rad, 100%, 50%)"), 0, 255, 255, 1.0f);
    checkRgba(parseColorValue("hsla(60deg 100% 50% / 0.25)"), 255, 255, 0, 0.25f);
    // Hue wraps; saturation 0 is grey whatever the hue; the alias works with three arguments.
    checkRgba(parseColorValue("hsl(-120, 100%, 50%)"), 0, 0, 255, 1.0f);
    checkRgba(parseColorValue("hsla(300, 0%, 50%)"), 128, 128, 128, 1.0f);
    // Bad forms: saturation without %, mixed separators, a missing argument, an angle unit for s.
    CHECK(colorRejected("hsl(120, 100, 50%)"));
    CHECK(colorRejected("hsl(120 100% 50%, 0.5)"));
    CHECK(colorRejected("hsl(120, 100%, 50% / 0.5)"));
    CHECK(colorRejected("hsl(120, 100%)"));
    CHECK(colorRejected("hsl(120px 100% 50%)"));
}

TEST_CASE("T-322 values: the CSS named colour table, sampled, with its edges") {
    checkRgba(parseColorValue("aliceblue"), 0xF0, 0xF8, 0xFF, 1.0f);    // First entry.
    checkRgba(parseColorValue("yellowgreen"), 0x9A, 0xCD, 0x32, 1.0f);  // Last entry.
    checkRgba(parseColorValue("rebeccapurple"), 0x66, 0x33, 0x99, 1.0f);
    checkRgba(parseColorValue("darkslategrey"), 0x2F, 0x4F, 0x4F, 1.0f);
    checkRgba(parseColorValue("lightgoldenrodyellow"), 0xFA, 0xFA, 0xD2, 1.0f);
    checkRgba(parseColorValue("CornflowerBlue"), 0x64, 0x95, 0xED, 1.0f);  // Case-insensitive.
    checkRgba(parseColorValue("papayawhip"), 0xFF, 0xEF, 0xD5, 1.0f);
    checkRgba(parseColorValue("mediumspringgreen"), 0x00, 0xFA, 0x9A, 1.0f);
    checkRgba(parseColorValue("orange"), 0xFF, 0xA5, 0x00, 1.0f);  // One of the old 17.
    checkRgba(parseColorValue("grey"), 0x80, 0x80, 0x80, 1.0f);
    checkRgba(parseColorValue("transparent"), 0, 0, 0, 0.0f);
    // A prefix, an extension, before the first and after the last entry are all unknown.
    CHECK(colorRejected("aliceblu"));
    CHECK(colorRejected("aliceblues"));
    CHECK(colorRejected("aaa"));
    CHECK(colorRejected("zzz"));
    CHECK(colorRejected("currentcolor"));
}

TEST_CASE("T-322 values: transform rotateZ() is rotate()") {
    DiagnosticSink sink;
    bool ok = false;
    auto decls = parseOneRule("transform: translate(4px, 2px) rotateZ(30deg) scale(2);", sink, ok);
    CHECK(ok);
    CHECK(sink.all().empty());
    REQUIRE(findDecl(decls, prop::Rotation) != nullptr);
    CHECK(findDecl(decls, prop::Rotation)->value.f == doctest::Approx(30));
    CHECK(findDecl(decls, prop::ScaleX)->value.f == doctest::Approx(2));
    // Out of order it composes like rotate() does.
    DiagnosticSink sink2;
    auto composed = parseOneRule("transform: rotateZ(10deg) rotate(20deg);", sink2, ok);
    CHECK(ok);
    REQUIRE(findDecl(composed, prop::Rotation) != nullptr);
    CHECK(findDecl(composed, prop::Rotation)->value.f == doctest::Approx(30).epsilon(1e-4));
}

TEST_CASE("T-322 values: transform matrix() decomposes, a singular one is dropped") {
    DiagnosticSink sink;
    bool ok = false;
    // rotate(30deg) as a matrix, then a translation of (10, 20).
    auto decls = parseOneRule("transform: matrix(0.8660254, 0.5, -0.5, 0.8660254, 10, 20);", sink, ok);
    CHECK(ok);
    CHECK(sink.all().empty());
    REQUIRE(decls.size() == 10);
    CHECK(findDecl(decls, prop::TranslateX)->value.len.value == doctest::Approx(10));
    CHECK(findDecl(decls, prop::TranslateY)->value.len.value == doctest::Approx(20));
    CHECK(findDecl(decls, prop::Rotation)->value.f == doctest::Approx(30).epsilon(1e-4));
    CHECK(findDecl(decls, prop::SkewX)->value.f == doctest::Approx(0).epsilon(1e-4));
    CHECK(findDecl(decls, prop::ScaleX)->value.f == doctest::Approx(1).epsilon(1e-4));
    CHECK(findDecl(decls, prop::ScaleY)->value.f == doctest::Approx(1).epsilon(1e-4));

    // Scale and skew: matrix(2, 0, 1, 3, 0, 0) = skewX(atan(1/3)) scale(2, 3).
    DiagnosticSink sink2;
    auto skewed = parseOneRule("transform: matrix(2, 0, 1, 3, 0, 0);", sink2, ok);
    CHECK(ok);
    CHECK(findDecl(skewed, prop::ScaleX)->value.f == doctest::Approx(2).epsilon(1e-4));
    CHECK(findDecl(skewed, prop::ScaleY)->value.f == doctest::Approx(3).epsilon(1e-4));
    CHECK(findDecl(skewed, prop::SkewX)->value.f == doctest::Approx(18.4349488f).epsilon(1e-4));

    // Singular (ad - bc = 0): warned, the function dropped, the rest kept.
    DiagnosticSink sink3;
    auto singular = parseOneRule("transform: matrix(1, 2, 2, 4, 5, 6) rotate(15deg);", sink3, ok);
    CHECK(ok);
    CHECK_FALSE(sink3.hasErrors());
    REQUIRE(sink3.all().size() == 1);
    CHECK(sink3.all()[0].message.find("singular") != std::string::npos);
    REQUIRE(singular.size() == 10);
    CHECK(findDecl(singular, prop::Rotation)->value.f == doctest::Approx(15).epsilon(1e-4));
    CHECK(findDecl(singular, prop::TranslateX)->value.len.value == doctest::Approx(0));

    // Bad arguments (five numbers, a unit) are an invalid value as before.
    DiagnosticSink sink4;
    CHECK(parseOneRule("transform: matrix(1, 0, 0, 1, 0);", sink4, ok).empty());
    CHECK_FALSE(sink4.all().empty());
    DiagnosticSink sink5;
    CHECK(parseOneRule("transform: matrix(1, 0, 0, 1, 0px, 0);", sink5, ok).empty());
}

TEST_CASE("T-322 values: step-start / step-end are steps(1, start|end)") {
    DiagnosticSink sink;
    bool ok = false;
    auto decls = parseOneRule("transition: opacity 1s step-start, width 2s step-end 1s;", sink, ok);
    CHECK(ok);
    CHECK(sink.all().empty());
    const StyleDecl* d = findDecl(decls, styleprop::Transition);
    REQUIRE(d != nullptr);
    REQUIRE(d->value.transitions.size() == 2);
    CHECK(d->value.transitions[0].easing.kind == EasingKind::Steps);
    CHECK(d->value.transitions[0].easing.stepCount == 1);
    CHECK(d->value.transitions[0].easing.stepsJumpEnd == 0);
    CHECK(d->value.transitions[1].easing.kind == EasingKind::Steps);
    CHECK(d->value.transitions[1].easing.stepCount == 1);
    CHECK(d->value.transitions[1].easing.stepsJumpEnd == 1);
    CHECK(d->value.transitions[1].delay == doctest::Approx(1));

    DiagnosticSink sink2;
    auto anim = parseOneRule("animation: spin 1s step-end infinite;", sink2, ok);
    CHECK(ok);
    const StyleDecl* a = findDecl(anim, styleprop::Animation);
    REQUIRE(a != nullptr);
    REQUIRE(a->value.animations.size() == 1);
    CHECK(a->value.animations[0].easing.kind == EasingKind::Steps);
    CHECK(a->value.animations[0].easing.stepsJumpEnd == 1);
}

// T-323 (05 §5.9.8): `text-transform`, an inherited, non-animatable Enum StyleProp.
TEST_CASE("values: T-323 text-transform keywords") {
    const char* const names[] = {"none", "uppercase", "lowercase", "capitalize"};
    for (uint32_t i = 0; i < 4; ++i) {
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule(std::string("text-transform: ") + names[i] + ";", sink, ok);
        CAPTURE(names[i]);
        CHECK(ok);
        CHECK(sink.all().empty());
        const StyleDecl* d = findDecl(decls, styleprop::TextTransform);
        REQUIRE(d != nullptr);
        CHECK(d->value.kind == StyleValueKind::Enum);
        CHECK(d->value.e == i);
    }
    CHECK(styleprop::TextTransform == 202);
    CHECK(styleprop::kLast >= styleprop::TextTransform);  // T-324 added 203, T-326 204 after it.
    const PropInfo* info = findProp("text-transform");
    REQUIRE(info != nullptr);
    CHECK(info->inherited);
    CHECK_FALSE(info->animatable);
}

TEST_CASE("values: T-323 text-transform rejects other values") {
    for (const char* body :
         {"text-transform: full-width;", "text-transform: uppercase lowercase;", "text-transform: 1px;"}) {
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule(body, sink, ok);
        CAPTURE(body);
        CHECK(decls.empty());
        CHECK_FALSE(sink.all().empty());
    }
}

// T-324 (05 §5.9.8): `text-decoration-*` and the `text-decoration` shorthand.
TEST_CASE("values: T-324 text-decoration longhands and numbering") {
    CHECK(styleprop::TextDecorationLine == 203);
    CHECK(prop::TextDecorationColor == 132);
    CHECK(prop::TextDecorationThickness == 133);
    CHECK(prop::kElementPropCount >= 134);  // 138 since T-327.
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule(
        "text-decoration-line: underline; text-decoration-color: #00ff00; text-decoration-thickness: 3px;",
        sink, ok);
    CHECK(ok);
    CHECK(sink.all().empty());
    REQUIRE(decls.size() == 3);
    const StyleDecl* l = findDecl(decls, styleprop::TextDecorationLine);
    REQUIRE(l != nullptr);
    CHECK(l->value.kind == StyleValueKind::Enum);
    CHECK(l->value.e == kTextDecorationUnderline);
    const StyleDecl* c = findDecl(decls, prop::TextDecorationColor);
    REQUIRE(c != nullptr);
    CHECK(c->value.color.g == doctest::Approx(1));
    const StyleDecl* t = findDecl(decls, prop::TextDecorationThickness);
    REQUIRE(t != nullptr);
    CHECK(t->value.kind == StyleValueKind::Length);
    CHECK(t->value.len.unit == LengthUnit::Px);
    CHECK(t->value.len.value == doctest::Approx(3));
}

TEST_CASE("values: T-324 text-decoration-line combines the three lines") {
    struct Case {
        const char* decl;
        uint32_t bits;
    };
    const Case cases[] = {
        {"text-decoration-line: none", 0},
        {"text-decoration-line: overline", kTextDecorationOverline},
        {"text-decoration-line: line-through", kTextDecorationLineThrough},
        {"text-decoration-line: underline line-through",
         kTextDecorationUnderline | kTextDecorationLineThrough},
        {"text-decoration-line: line-through overline underline", kTextDecorationAll},
    };
    for (const Case& c : cases) {
        CAPTURE(c.decl);
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule(std::string(c.decl) + ";", sink, ok);
        CHECK(sink.all().empty());
        REQUIRE(decls.size() == 1);
        CHECK(decls[0].value.e == c.bits);
    }
    const char* bad[] = {"text-decoration-line: underline underline", "text-decoration-line: none underline",
                         "text-decoration-line: blink", "text-decoration-line: 2px"};
    for (const char* body : bad) {
        CAPTURE(body);
        DiagnosticSink sink;
        bool ok;
        CHECK(parseOneRule(std::string(body) + ";", sink, ok).empty());
        CHECK_FALSE(sink.all().empty());
    }
}

TEST_CASE("values: T-324 text-decoration-thickness is auto or a length") {
    DiagnosticSink sink;
    bool ok;
    auto autoDecl = parseOneRule("text-decoration-thickness: auto;", sink, ok);
    REQUIRE(autoDecl.size() == 1);
    CHECK(autoDecl[0].value.len.unit == LengthUnit::Auto);
    auto em = parseOneRule("text-decoration-thickness: 0.1em;", sink, ok);
    REQUIRE(em.size() == 1);
    CHECK(em[0].value.len.unit == LengthUnit::Em);
    CHECK(sink.all().empty());
    const char* bad[] = {"text-decoration-thickness: 10%", "text-decoration-thickness: -1px"};
    for (const char* body : bad) {
        CAPTURE(body);
        DiagnosticSink badSink;
        CHECK(parseOneRule(std::string(body) + ";", badSink, ok).empty());
        CHECK_FALSE(badSink.all().empty());
    }
}

TEST_CASE("values: T-324 text-decoration shorthand, six forms") {
    struct Case {
        const char* decl;
        uint32_t bits;       // Expected text-decoration-line.
        bool autoThickness;  // Expected thickness `auto` (else `px` px).
        float px;
        bool hasColor;  // Whether a text-decoration-color declaration is written.
        float red;
    };
    const Case cases[] = {
        {"text-decoration: underline", kTextDecorationUnderline, true, 0, false, 0},
        {"text-decoration: line-through red", kTextDecorationLineThrough, true, 0, true, 1},
        {"text-decoration: underline overline #0000ff 2px",
         kTextDecorationUnderline | kTextDecorationOverline, false, 2, true, 0},
        {"text-decoration: 3px red underline", kTextDecorationUnderline, false, 3, true, 1},
        {"text-decoration: none", 0, true, 0, false, 0},
        {"text-decoration: overline auto", kTextDecorationOverline, true, 0, false, 0},
    };
    for (const Case& c : cases) {
        CAPTURE(c.decl);
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule(std::string(c.decl) + ";", sink, ok);
        CHECK(ok);
        CHECK(sink.all().empty());
        REQUIRE(decls.size() == (c.hasColor ? 3u : 2u));
        const StyleDecl* l = findDecl(decls, styleprop::TextDecorationLine);
        REQUIRE(l != nullptr);
        CHECK(l->value.e == c.bits);
        const StyleDecl* t = findDecl(decls, prop::TextDecorationThickness);
        REQUIRE(t != nullptr);
        CHECK((t->value.len.unit == LengthUnit::Auto) == c.autoThickness);
        if (!c.autoThickness) {
            CHECK(t->value.len.value == doctest::Approx(c.px));
        }
        const StyleDecl* col = findDecl(decls, prop::TextDecorationColor);
        CHECK((col != nullptr) == c.hasColor);
        if (col != nullptr) {
            CHECK(col->value.color.r == doctest::Approx(c.red));
        }
    }
    const char* bad[] = {"text-decoration: underline underline", "text-decoration: none underline",
                         "text-decoration: underline red blue", "text-decoration: underline 1px 2px",
                         "text-decoration: underline wavy"};
    for (const char* body : bad) {
        CAPTURE(body);
        DiagnosticSink sink;
        bool ok;
        CHECK(parseOneRule(std::string(body) + ";", sink, ok).empty());
        CHECK_FALSE(sink.all().empty());
    }
}

TEST_CASE("values: T-324 transition accepts text-decoration-color and -thickness") {
    DiagnosticSink sink;
    bool ok;
    auto decls =
        parseOneRule("transition: text-decoration-color 0.2s, text-decoration-thickness 0.1s;", sink, ok);
    CHECK(ok);
    CHECK(sink.all().empty());
    const StyleDecl* d = findDecl(decls, styleprop::Transition);
    REQUIRE(d != nullptr);
    CHECK(d->value.transitions.size() == 2);
}
// T-326 (18 §18.12): `mix-blend-mode`, a non-inherited, non-animatable Enum StyleProp whose value
// is a BlendMode (xt/node.h): the 16 CSS modes plus `add`, with `additive` as an alias of `add`.
TEST_CASE("values: T-326 mix-blend-mode keywords") {
    const char* const names[] = {"normal",     "multiply",    "screen",     "overlay",    "darken",
                                 "lighten",    "color-dodge", "color-burn", "hard-light", "soft-light",
                                 "difference", "exclusion",   "hue",        "saturation", "color",
                                 "luminosity", "add"};
    static_assert(sizeof(names) / sizeof(names[0]) == 17);
    for (uint32_t i = 0; i < 17; ++i) {
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule(std::string("mix-blend-mode: ") + names[i] + ";", sink, ok);
        CAPTURE(names[i]);
        CHECK(ok);
        CHECK(sink.all().empty());
        const StyleDecl* d = findDecl(decls, styleprop::MixBlendMode);
        REQUIRE(d != nullptr);
        CHECK(d->value.kind == StyleValueKind::Enum);
        CHECK(d->value.e == i);  // = the BlendMode value (xt/node.h): 0 Normal .. 16 Add.
    }
    {
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule("mix-blend-mode: additive;", sink, ok);
        CHECK(ok);
        const StyleDecl* d = findDecl(decls, styleprop::MixBlendMode);
        REQUIRE(d != nullptr);
        CHECK(d->value.e == 16u);  // BlendMode::Add
    }
    CHECK(styleprop::MixBlendMode == 204);
    CHECK(styleprop::kLast >= styleprop::MixBlendMode);  // T-327 took 205-206.
    const PropInfo* info = findProp("mix-blend-mode");
    REQUIRE(info != nullptr);
    CHECK_FALSE(info->inherited);
    CHECK_FALSE(info->animatable);
    CHECK(initialValue(styleprop::MixBlendMode).e == 0);
    {
        // CSS `plus-lighter` is the importer's business (it writes `add`); the compiler rejects it.
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule("mix-blend-mode: plus-lighter;", sink, ok);
        CHECK(findDecl(decls, styleprop::MixBlendMode) == nullptr);
        CHECK_FALSE(sink.all().empty());
    }
}

// T-327 (05 §5.9.8): background-size / background-position / background-repeat / image-rendering.
TEST_CASE("values: T-327 property numbers") {
    CHECK(prop::BackgroundSizeX == 134);
    CHECK(prop::BackgroundSizeY == 135);
    CHECK(prop::BackgroundPositionX == 136);
    CHECK(prop::BackgroundPositionY == 137);
    CHECK(prop::AspectRatio == 138);        // T-328 took the next PropId.
    CHECK(prop::kElementPropCount >= 139);  // 143 since T-333.
    CHECK(styleprop::BackgroundRepeat == 205);
    CHECK(styleprop::ImageRendering == 206);
    CHECK(styleprop::ClipPath == 207);                  // T-332 took the next StyleProp.
    CHECK(styleprop::kLastLow == styleprop::ClipPath);  // T-331 continued at 256.
}

TEST_CASE("values: T-327 background-size, every form") {
    struct Case {
        const char* decl;
        float x;
        LengthUnit ux;
        float y;
        LengthUnit uy;
    };
    const Case cases[] = {
        {"background-size: auto", 0, LengthUnit::Auto, 0, LengthUnit::Auto},
        {"background-size: cover", kBackgroundSizeCover, LengthUnit::Auto, kBackgroundSizeCover,
         LengthUnit::Auto},
        {"background-size: contain", kBackgroundSizeContain, LengthUnit::Auto, kBackgroundSizeContain,
         LengthUnit::Auto},
        {"background-size: 32px", 32, LengthUnit::Px, 0, LengthUnit::Auto},
        {"background-size: 50%", 50, LengthUnit::Percent, 0, LengthUnit::Auto},
        {"background-size: 16px 25%", 16, LengthUnit::Px, 25, LengthUnit::Percent},
        {"background-size: auto 40px", 0, LengthUnit::Auto, 40, LengthUnit::Px},
        {"background-size: 0 0", 0, LengthUnit::Px, 0, LengthUnit::Px},
    };
    for (const Case& c : cases) {
        CAPTURE(c.decl);
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule(std::string(c.decl) + ";", sink, ok);
        CHECK(ok);
        CHECK(sink.all().empty());
        REQUIRE(decls.size() == 2);
        const StyleDecl* x = findDecl(decls, prop::BackgroundSizeX);
        const StyleDecl* y = findDecl(decls, prop::BackgroundSizeY);
        REQUIRE(x != nullptr);
        REQUIRE(y != nullptr);
        CHECK(x->value.kind == StyleValueKind::Length);
        CHECK(x->value.len.value == doctest::Approx(c.x));
        CHECK(x->value.len.unit == c.ux);
        CHECK(y->value.len.value == doctest::Approx(c.y));
        CHECK(y->value.len.unit == c.uy);
    }
}

TEST_CASE("values: T-327 background-size rejects bad forms") {
    const char* bad[] = {"background-size: cover contain", "background-size: cover 10px",
                         "background-size: -4px", "background-size: 1px 2px 3px", "background-size: red"};
    for (const char* body : bad) {
        CAPTURE(body);
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule(std::string(body) + ";", sink, ok);
        CHECK(decls.empty());
        CHECK_FALSE(sink.all().empty());
    }
}

TEST_CASE("values: T-327 background-position, every form") {
    struct Case {
        const char* decl;
        float x;
        LengthUnit ux;
        float y;
        LengthUnit uy;
    };
    const Case cases[] = {
        {"background-position: left", 0, LengthUnit::Percent, 50, LengthUnit::Percent},
        {"background-position: center", 50, LengthUnit::Percent, 50, LengthUnit::Percent},
        {"background-position: right bottom", 100, LengthUnit::Percent, 100, LengthUnit::Percent},
        {"background-position: center top", 50, LengthUnit::Percent, 0, LengthUnit::Percent},
        {"background-position: 10px 20%", 10, LengthUnit::Px, 20, LengthUnit::Percent},
        {"background-position: -8px", -8, LengthUnit::Px, 50, LengthUnit::Percent},
        {"background-position: 25% 75%", 25, LengthUnit::Percent, 75, LengthUnit::Percent},
    };
    for (const Case& c : cases) {
        CAPTURE(c.decl);
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule(std::string(c.decl) + ";", sink, ok);
        CHECK(ok);
        CHECK(sink.all().empty());
        REQUIRE(decls.size() == 2);
        const StyleDecl* x = findDecl(decls, prop::BackgroundPositionX);
        const StyleDecl* y = findDecl(decls, prop::BackgroundPositionY);
        REQUIRE(x != nullptr);
        REQUIRE(y != nullptr);
        CHECK(x->value.len.value == doctest::Approx(c.x));
        CHECK(x->value.len.unit == c.ux);
        CHECK(y->value.len.value == doctest::Approx(c.y));
        CHECK(y->value.len.unit == c.uy);
    }
    const char* bad[] = {"background-position: top left", "background-position: 1px 2px 3px",
                         "background-position: middle"};
    for (const char* body : bad) {
        CAPTURE(body);
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule(std::string(body) + ";", sink, ok);
        CHECK(decls.empty());
        CHECK_FALSE(sink.all().empty());
    }
}

TEST_CASE("values: T-327 background-repeat and image-rendering keywords") {
    const std::pair<const char*, uint32_t> repeats[] = {
        {"stretch", 0}, {"no-repeat", 1}, {"repeat", 2}, {"repeat-x", 3}, {"repeat-y", 4}};
    for (const auto& [name, value] : repeats) {
        CAPTURE(name);
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule(std::string("background-repeat: ") + name + ";", sink, ok);
        CHECK(sink.all().empty());
        const StyleDecl* d = findDecl(decls, styleprop::BackgroundRepeat);
        REQUIRE(d != nullptr);
        CHECK(d->value.kind == StyleValueKind::Enum);
        CHECK(d->value.e == value);
    }
    const std::pair<const char*, uint32_t> renderings[] = {{"auto", 0}, {"pixelated", 1}};
    for (const auto& [name, value] : renderings) {
        CAPTURE(name);
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule(std::string("image-rendering: ") + name + ";", sink, ok);
        CHECK(sink.all().empty());
        const StyleDecl* d = findDecl(decls, styleprop::ImageRendering);
        REQUIRE(d != nullptr);
        CHECK(d->value.e == value);
    }
    for (const char* body : {"background-repeat: space", "background-repeat: repeat no-repeat",
                             "image-rendering: crisp-edges"}) {
        CAPTURE(body);
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule(std::string(body) + ";", sink, ok);
        CHECK(decls.empty());
        CHECK_FALSE(sink.all().empty());
    }
    CHECK(initialValue(styleprop::BackgroundRepeat).e == 0u);  // stretch: the pre-T-327 drawing.
    CHECK(initialValue(styleprop::ImageRendering).e == 0u);
    CHECK(initialValue(prop::BackgroundSizeX).len.unit == LengthUnit::Auto);
    CHECK(initialValue(prop::BackgroundPositionY).len.unit == LengthUnit::Percent);
    CHECK(findProp("image-rendering")->inherited);
    CHECK_FALSE(findProp("background-repeat")->inherited);
}

TEST_CASE("values: T-327 transition accepts background-size and background-position") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("transition: background-size 0.1s, background-position 0.2s;", sink, ok);
    CHECK(ok);
    CHECK(sink.all().empty());
    const StyleDecl* t = findDecl(decls, styleprop::Transition);
    REQUIRE(t != nullptr);
    REQUIRE(t->value.transitions.size() == 4);
    CHECK(t->value.transitions[0].prop == prop::BackgroundSizeX);
    CHECK(t->value.transitions[1].prop == prop::BackgroundSizeY);
    CHECK(t->value.transitions[2].prop == prop::BackgroundPositionX);
    CHECK(t->value.transitions[3].prop == prop::BackgroundPositionY);
}

// T-328 (06 §6.16): `aspect-ratio: auto | <w> / <h> | <number>` is one Float w/h, `auto` = 0; a
// zero or negative ratio (or a unit) is dropped with a diagnostic.
TEST_CASE("values: T-328 aspect-ratio") {
    struct Case {
        const char* decl;
        float ratio;
    };
    const Case good[] = {{"aspect-ratio: 16/9", 16.0f / 9.0f},
                         {"aspect-ratio: 16 / 9", 16.0f / 9.0f},
                         {"aspect-ratio: 1.5", 1.5f},
                         {"aspect-ratio: auto", 0.0f}};
    for (const Case& c : good) {
        CAPTURE(c.decl);
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule(std::string(c.decl) + ";", sink, ok);
        REQUIRE(decls.size() == 1);
        CHECK(decls[0].prop == xt::prop::AspectRatio);
        CHECK(decls[0].value.kind == StyleValueKind::Float);
        CHECK(decls[0].value.f == doctest::Approx(c.ratio));
        CHECK(sink.all().empty());
    }
    const char* bad[] = {"aspect-ratio: 0",     "aspect-ratio: -1.5", "aspect-ratio: 16/0",
                         "aspect-ratio: -16/9", "aspect-ratio: 2px",  "aspect-ratio: 16/"};
    for (const char* body : bad) {
        CAPTURE(body);
        DiagnosticSink sink;
        bool ok;
        CHECK(parseOneRule(std::string(body) + ";", sink, ok).empty());
        CHECK_FALSE(sink.all().empty());
    }
}

// T-329 (05 §5.9.8): the independent transform properties. Alone, each writes the existing
// transform PropIds straight through (05.5's fixed-order path).
TEST_CASE("values: T-329 translate / rotate / scale syntax") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("translate: 10px 50%;", sink, ok);
    CHECK(ok);
    CHECK(sink.all().empty());
    REQUIRE(decls.size() == 2);
    CHECK(findDecl(decls, prop::TranslateX)->value.len.value == doctest::Approx(10));
    CHECK(findDecl(decls, prop::TranslateX)->value.len.unit == LengthUnit::Px);
    // 06 §6.14.1: the percent stays a Length for the post-layout fold (T-211).
    CHECK(findDecl(decls, prop::TranslateY)->value.len.value == doctest::Approx(50));
    CHECK(findDecl(decls, prop::TranslateY)->value.len.unit == LengthUnit::Percent);

    decls = parseOneRule("translate: 7px;", sink, ok);  // A missing y is 0.
    REQUIRE(decls.size() == 2);
    CHECK(findDecl(decls, prop::TranslateY)->value.len.value == doctest::Approx(0));

    decls = parseOneRule("translate: 1px 2em 3px;", sink, ok);
    REQUIRE(decls.size() == 3);
    CHECK(findDecl(decls, prop::TranslateY)->value.len.unit == LengthUnit::Em);
    CHECK(findDecl(decls, prop::TranslateZ) != nullptr);

    decls = parseOneRule("rotate: 45deg;", sink, ok);
    REQUIRE(decls.size() == 1);
    CHECK(findDecl(decls, prop::Rotation)->value.f == doctest::Approx(45));
    decls = parseOneRule("rotate: z -30deg;", sink, ok);
    REQUIRE(decls.size() == 1);
    CHECK(findDecl(decls, prop::Rotation)->value.f == doctest::Approx(-30));

    decls = parseOneRule("scale: 2;", sink, ok);
    REQUIRE(decls.size() == 2);
    CHECK(findDecl(decls, prop::ScaleX)->value.f == doctest::Approx(2));
    CHECK(findDecl(decls, prop::ScaleY)->value.f == doctest::Approx(2));
    decls = parseOneRule("scale: 2 0.5;", sink, ok);
    CHECK(findDecl(decls, prop::ScaleY)->value.f == doctest::Approx(0.5));
    CHECK(sink.all().empty());
}

TEST_CASE("values: T-329 none writes only the property's own components at identity") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("translate: none; rotate: none; scale: none;", sink, ok);
    CHECK(ok);
    CHECK(sink.all().empty());
    REQUIRE(decls.size() == 6);
    CHECK(findDecl(decls, prop::TranslateX)->value.len.value == doctest::Approx(0));
    CHECK(findDecl(decls, prop::TranslateY)->value.len.value == doctest::Approx(0));
    CHECK(findDecl(decls, prop::TranslateZ)->value.f == doctest::Approx(0));
    CHECK(findDecl(decls, prop::Rotation)->value.f == doctest::Approx(0));
    CHECK(findDecl(decls, prop::ScaleX)->value.f == doctest::Approx(1));
    CHECK(findDecl(decls, prop::ScaleY)->value.f == doctest::Approx(1));
    CHECK(findDecl(decls, prop::SkewX) == nullptr);
    CHECK(findDecl(decls, prop::RotationY) == nullptr);
}

TEST_CASE("values: T-329 rejected forms") {
    for (const char* body :
         {"rotate: x 10deg", "rotate: y 10deg", "rotate: 10", "rotate: z", "scale: 1px", "scale: 1 2 3",
          "translate: auto", "translate: 1px 2px 3px 4px", "translate: red"}) {
        CAPTURE(body);
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule(std::string(body) + ";", sink, ok);
        CHECK(decls.empty());
        CHECK_FALSE(sink.all().empty());
    }
    DiagnosticSink sink;
    bool ok;
    parseOneRule("rotate: x 10deg;", sink, ok);
    REQUIRE_FALSE(sink.all().empty());
    CHECK(sink.all()[0].level == Diagnostic::Level::Warning);  // 19 §19.10's rotateX(), not here.
    CHECK_FALSE(sink.hasErrors());
    parseOneRule("translate: 1px 2px 3%;", sink, ok);
    CHECK(sink.hasErrors());  // As translateZ(): z does not accept percent.
}

TEST_CASE("values: T-329 independent properties are animatable") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("transition: translate 0.1s, rotate 0.2s, scale 0.3s;", sink, ok);
    CHECK(ok);
    CHECK(sink.all().empty());
    const StyleDecl* t = findDecl(decls, styleprop::Transition);
    REQUIRE(t != nullptr);
    REQUIRE(t->value.transitions.size() == 6);
    CHECK(t->value.transitions[0].prop == prop::TranslateX);
    CHECK(t->value.transitions[2].prop == prop::TranslateZ);
    CHECK(t->value.transitions[3].prop == prop::Rotation);
    CHECK(t->value.transitions[5].prop == prop::ScaleY);
    CHECK_FALSE(findProp("translate")->inherited);
    CHECK(findProp("scale")->animatable);
}

// T-330 (05 §5.9.8): `min()` / `max()` / `clamp()` where `calc()` is -- Length longhands only, the
// arguments full calc expressions; min/max of n arguments are n - 1 Min/Max ops after the
// arguments, clamp(a, b, c) is `a b c Min Max` = max(a, min(b, c)). An all-px program folds at
// emit time (foldCalcToPx).
namespace {

std::vector<CalcOpKind> calcKinds(const StyleDecl& d) {
    std::vector<CalcOpKind> kinds;
    for (const CalcOpDecl& op : d.value.calc) {
        kinds.push_back(op.op);
    }
    return kinds;
}

}  // namespace

TEST_CASE("values: T-330 min(), max() and clamp() parse to Min/Max ops") {
    using K = CalcOpKind;
    struct Case {
        const char* decl;
        std::vector<CalcOpKind> ops;
    };
    const Case good[] = {
        {"width: min(100%, 300px)", {K::PushLength, K::PushLength, K::Min}},
        {"width: max(10px, 2em, 5vw)", {K::PushLength, K::PushLength, K::PushLength, K::Max, K::Max}},
        {"width: clamp(100px, 50%, 300px)", {K::PushLength, K::PushLength, K::PushLength, K::Min, K::Max}},
        // Arguments are expressions; calc() nests inside and the functions nest inside calc().
        {"width: min(100% - 20px, calc(2 * 100px))",
         {K::PushLength, K::PushLength, K::Sub, K::PushNumber, K::PushLength, K::Mul, K::Min}},
        {"width: calc(10px + max(1em, 4px) * 2)",
         {K::PushLength, K::PushLength, K::PushLength, K::Max, K::PushNumber, K::Mul, K::Add}},
        {"left: clamp(0px, var(--x, 4px), 1safe-left)",
         {K::PushLength, K::PushVar, K::PushLength, K::Min, K::Max}},
        {"width: min(10px)", {K::PushLength}},
    };
    for (const Case& c : good) {
        CAPTURE(c.decl);
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule(std::string(c.decl) + ";", sink, ok);
        REQUIRE(decls.size() == 1);
        CHECK(decls[0].value.kind == StyleValueKind::Calc);
        CHECK(calcKinds(decls[0]) == c.ops);
        CHECK(sink.all().empty());
    }
}

TEST_CASE("values: T-330 malformed min()/max()/clamp() and non-length properties are rejected") {
    struct Case {
        const char* decl;
        const char* reason;
    };
    const Case bad[] = {
        {"width: clamp(1px, 2px)", "exactly three arguments"},
        {"width: clamp(1px, 2px, 3px, 4px)", "exactly three arguments"},
        {"width: min(10px, 2)", "cannot mix numbers and lengths"},
        {"width: calc(10px, 20px)", "outside min(), max() or clamp()"},
        {"width: min(10px, )", "expected a number or a length"},
        {"opacity: min(1, 2)", "only supported on length properties"},
        {"width: min(10px, 20px) 5px", "invalid"},
        {"margin: min(4px, 2px)", "invalid"},
    };
    for (const Case& c : bad) {
        CAPTURE(c.decl);
        DiagnosticSink sink;
        bool ok;
        CHECK(parseOneRule(std::string(c.decl) + ";", sink, ok).empty());
        bool found = false;
        for (const Diagnostic& d : sink.all()) {
            CAPTURE(d.message);
            found = found || d.message.find(c.reason) != std::string::npos;
        }
        CHECK(found);
    }
}

TEST_CASE("values: T-330 an all-px min()/max()/clamp() folds to a constant") {
    struct Case {
        const char* decl;
        bool folds;
        float px;
    };
    const Case cases[] = {
        {"width: min(100px, 30px + 20px)", true, 50},
        {"width: max(10px, 4px * 3, 11px)", true, 12},
        {"width: clamp(100px, 500px, 300px)", true, 300},
        {"width: clamp(100px, 50px, 300px)", true, 100},
        {"width: calc(min(10px, 20px) / 2)", true, 5},
        {"width: clamp(100px, 50%, 300px)", false, 0},
        {"width: min(10px, 1em)", false, 0},
        {"width: min(10px, var(--x, 4px))", false, 0},
        {"width: calc(10px + 5px)", false, 0},  // A plain calc() keeps its CALC program (T-176).
    };
    for (const Case& c : cases) {
        CAPTURE(c.decl);
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule(std::string(c.decl) + ";", sink, ok);
        REQUIRE(decls.size() == 1);
        REQUIRE(decls[0].value.kind == StyleValueKind::Calc);
        float px = -1;
        CHECK(foldCalcToPx(decls[0].value.calc, px) == c.folds);
        if (c.folds) {
            CHECK(px == doctest::Approx(c.px));
        }
    }
}

// T-332 (05 §5.9.8): clip-path basic shapes.
namespace {

const StyleDecl* parseClip(const std::string& value, DiagnosticSink& sink, std::vector<StyleDecl>& decls) {
    bool ok;
    decls = parseOneRule("clip-path: " + value + ";", sink, ok);
    return findDecl(decls, styleprop::ClipPath);
}

}  // namespace

TEST_CASE("values: T-332 clip-path property info, none and the four shapes") {
    const PropInfo* info = findProp("clip-path");
    REQUIRE(info != nullptr);
    CHECK_FALSE(info->inherited);
    CHECK_FALSE(info->animatable);
    CHECK(info->kind == StyleValueKind::ClipShape);
    CHECK(initialValue(styleprop::ClipPath).clipShape.kind == ClipShapeKind::None);

    DiagnosticSink sink;
    std::vector<StyleDecl> decls;
    const StyleDecl* none = parseClip("none", sink, decls);
    REQUIRE(none != nullptr);
    CHECK(none->value.kind == StyleValueKind::ClipShape);
    CHECK(none->value.clipShape.kind == ClipShapeKind::None);

    // inset: 2 insets (t/b, l/r) + `round` with 2 radii (tl/br, tr/bl), vertical = horizontal.
    const StyleDecl* inset = parseClip("inset(10px 20% round 8px 4px)", sink, decls);
    REQUIRE(inset != nullptr);
    const ElementClipShape& in = inset->value.clipShape;
    CHECK(in.kind == ClipShapeKind::Inset);
    REQUIRE(in.params.size() == kClipInsetParamCount);
    CHECK(in.params[0].value == doctest::Approx(10));
    CHECK(in.params[0].unit == LengthUnit::Px);
    CHECK(in.params[1].value == doctest::Approx(20));
    CHECK(in.params[1].unit == LengthUnit::Percent);
    CHECK(in.params[2].value == doctest::Approx(10));
    CHECK(in.params[3].value == doctest::Approx(20));
    CHECK(in.params[4].value == doctest::Approx(8));   // tl x
    CHECK(in.params[5].value == doctest::Approx(8));   // tl y
    CHECK(in.params[6].value == doctest::Approx(4));   // tr x
    CHECK(in.params[8].value == doctest::Approx(8));   // br x
    CHECK(in.params[10].value == doctest::Approx(4));  // bl x

    // inset with elliptical corners: `round <h> / <v>`.
    const StyleDecl* insetV = parseClip("inset(0 round 10px / 5px)", sink, decls);
    REQUIRE(insetV != nullptr);
    CHECK(insetV->value.clipShape.params[4].value == doctest::Approx(10));
    CHECK(insetV->value.clipShape.params[5].value == doctest::Approx(5));

    // circle: explicit radius and center; keyword radius; defaults.
    const StyleDecl* circle = parseClip("circle(40px at 25% 10px)", sink, decls);
    REQUIRE(circle != nullptr);
    const ElementClipShape& c = circle->value.clipShape;
    CHECK(c.kind == ClipShapeKind::Circle);
    REQUIRE(c.params.size() == kClipCircleParamCount);
    CHECK(c.params[0].value == doctest::Approx(40));
    CHECK(c.params[1].unit == LengthUnit::Percent);
    CHECK(c.params[1].value == doctest::Approx(25));
    CHECK(c.params[2].unit == LengthUnit::Px);
    const StyleDecl* far = parseClip("circle(farthest-side)", sink, decls);
    REQUIRE(far != nullptr);
    CHECK(far->value.clipShape.params[0].unit == LengthUnit::Auto);
    CHECK(far->value.clipShape.params[0].value == kClipRadiusFarthestSide);
    CHECK(far->value.clipShape.params[1].value == doctest::Approx(50));
    CHECK(far->value.clipShape.params[1].unit == LengthUnit::Percent);
    const StyleDecl* bare = parseClip("circle()", sink, decls);
    REQUIRE(bare != nullptr);
    CHECK(bare->value.clipShape.params[0].unit == LengthUnit::Auto);
    CHECK(bare->value.clipShape.params[0].value == kClipRadiusClosestSide);

    // ellipse.
    const StyleDecl* ellipse = parseClip("ellipse(50% closest-side at 10px 20px)", sink, decls);
    REQUIRE(ellipse != nullptr);
    const ElementClipShape& e = ellipse->value.clipShape;
    CHECK(e.kind == ClipShapeKind::Ellipse);
    REQUIRE(e.params.size() == kClipEllipseParamCount);
    CHECK(e.params[0].unit == LengthUnit::Percent);
    CHECK(e.params[1].unit == LengthUnit::Auto);
    CHECK(e.params[2].value == doctest::Approx(10));
    CHECK(e.params[3].value == doctest::Approx(20));

    // polygon: three or more vertices, px and % mixed.
    const StyleDecl* polygon = parseClip("polygon(50% 0%, 100% 100%, 0px 100px)", sink, decls);
    REQUIRE(polygon != nullptr);
    const ElementClipShape& p = polygon->value.clipShape;
    CHECK(p.kind == ClipShapeKind::Polygon);
    REQUIRE(p.params.size() == 6);
    CHECK(p.params[0].value == doctest::Approx(50));
    CHECK(p.params[4].unit == LengthUnit::Px);
    CHECK(p.params[5].value == doctest::Approx(100));
    CHECK(sink.all().empty());
}

TEST_CASE("values: T-332 clip-path rejects path(), url(), em, two-vertex polygons and keyframes") {
    struct Case {
        const char* value;
        const char* mention;
    };
    const Case cases[] = {
        {"path(\"M0 0 L10 10 Z\")", "path()"},
        {"url(#clip)", "path()"},
        {"circle(1em)", "clip-path"},
        {"polygon(0 0, 10px 10px)", "clip-path"},
        {"inset(1px 2px 3px 4px 5px)", "clip-path"},
        {"border-box", "clip-path"},
    };
    for (const Case& cs : cases) {
        CAPTURE(cs.value);
        DiagnosticSink sink;
        std::vector<StyleDecl> decls;
        CHECK(parseClip(cs.value, sink, decls) == nullptr);
        REQUIRE_FALSE(sink.all().empty());
        CHECK(sink.all()[0].message.find(cs.mention) != std::string::npos);
    }
    // Not animatable: a keyframe stop and a transition item naming it are dropped.
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("transition: clip-path 1s;", sink, ok);
    const StyleDecl* transition = findDecl(decls, styleprop::Transition);
    CHECK((transition == nullptr || transition->value.transitions.empty()));
    REQUIRE_FALSE(sink.all().empty());
    CHECK(sink.all()[0].message.find("non-animatable") != std::string::npos);
}

// T-331 (05 §5.9.8): line-clamp (alias -webkit-line-clamp) and font-variant-numeric.
TEST_CASE("values: T-331 property numbers") {
    CHECK(styleprop::kLastLow == 207);
    CHECK(styleprop::LineClamp == 256);
    CHECK(styleprop::FontVariantNumeric == 257);
    CHECK(styleprop::kFirstHigh == 256);
    CHECK(styleprop::kLast >= styleprop::FontVariantNumeric);  // T-333 took 258-261.
    const PropInfo* clamp = findProp("line-clamp");
    REQUIRE(clamp != nullptr);
    CHECK_FALSE(clamp->inherited);
    CHECK_FALSE(clamp->animatable);
    const PropInfo* alias = findProp("-webkit-line-clamp");
    REQUIRE(alias != nullptr);
    CHECK(alias->prop == styleprop::LineClamp);
    CHECK(findPropById(styleprop::LineClamp) == clamp);  // The standard name comes first.
    const PropInfo* numeric = findProp("font-variant-numeric");
    REQUIRE(numeric != nullptr);
    CHECK(numeric->inherited);
    CHECK_FALSE(numeric->animatable);
    CHECK(initialValue(styleprop::LineClamp).e == 0);
    CHECK(initialValue(styleprop::FontVariantNumeric).e == 0);
}

TEST_CASE("values: T-331 line-clamp takes none or an integer >= 1, under either name") {
    struct Case {
        const char* decl;
        uint32_t value;
    };
    const Case good[] = {
        {"line-clamp: none", 0},  {"line-clamp: 1", 1},         {"line-clamp: 3", 3},
        {"line-clamp: 1e2", 100}, {"-webkit-line-clamp: 2", 2}, {"-webkit-line-clamp: none", 0},
    };
    for (const Case& c : good) {
        CAPTURE(c.decl);
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule(std::string(c.decl) + ";", sink, ok);
        CHECK(ok);
        const StyleDecl* d = findDecl(decls, styleprop::LineClamp);
        REQUIRE(d != nullptr);
        CHECK(d->value.kind == StyleValueKind::Enum);
        CHECK(d->value.e == c.value);
    }
    const char* bad[] = {"line-clamp: 0",    "line-clamp: -1",  "line-clamp: 1.5",      "line-clamp: 2px",
                         "line-clamp: auto", "line-clamp: 2 3", "-webkit-line-clamp: 0"};
    for (const char* decl : bad) {
        CAPTURE(decl);
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule(std::string(decl) + ";", sink, ok);
        CHECK(findDecl(decls, styleprop::LineClamp) == nullptr);
        CHECK_FALSE(sink.all().empty());
    }
}

TEST_CASE("values: T-331 font-variant-numeric is normal or tabular-nums") {
    {
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule("font-variant-numeric: tabular-nums;", sink, ok);
        CHECK(ok);
        const StyleDecl* d = findDecl(decls, styleprop::FontVariantNumeric);
        REQUIRE(d != nullptr);
        CHECK(d->value.e == 1u);
    }
    {
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule("font-variant-numeric: normal;", sink, ok);
        const StyleDecl* d = findDecl(decls, styleprop::FontVariantNumeric);
        REQUIRE(d != nullptr);
        CHECK(d->value.e == 0u);
    }
    for (const char* decl :
         {"font-variant-numeric: proportional-nums;", "font-variant-numeric: slashed-zero;",
          "font-variant-numeric: tabular-nums slashed-zero;"}) {
        CAPTURE(decl);
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule(decl, sink, ok);
        CHECK(findDecl(decls, styleprop::FontVariantNumeric) == nullptr);
        CHECK_FALSE(sink.all().empty());
    }
}

// T-333 (05 §5.9.8): per-side `border-*-color`, `border-color` 1-4 values, `border-style`.
TEST_CASE("values: T-333 property numbers") {
    CHECK(prop::BorderTopColor == 139);
    CHECK(prop::BorderRightColor == 140);
    CHECK(prop::BorderBottomColor == 141);
    CHECK(prop::BorderLeftColor == 142);
    CHECK(prop::kElementPropCount >= 143);  // 144 since T-336.
    CHECK(styleprop::BorderTopStyle == 258);
    CHECK(styleprop::BorderRightStyle == 259);
    CHECK(styleprop::BorderBottomStyle == 260);
    CHECK(styleprop::BorderLeftStyle == 261);
    CHECK(styleprop::kLast >= styleprop::BorderLeftStyle);  // T-336 took 262-263.
}

TEST_CASE("values: T-333 per-side border color longhands") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule(
        "border-top-color: red; border-right-color: #00ff00; border-bottom-color: rgba(0, 0, 255, 0.5); "
        "border-left-color: transparent;",
        sink, ok);
    CHECK(ok);
    CHECK(sink.all().empty());
    REQUIRE(decls.size() == 4);
    const StyleDecl* top = findDecl(decls, prop::BorderTopColor);
    REQUIRE(top != nullptr);
    CHECK(top->value.kind == StyleValueKind::Color);
    CHECK(top->value.color.r == doctest::Approx(1));
    CHECK(findDecl(decls, prop::BorderRightColor)->value.color.g == doctest::Approx(1));
    CHECK(findDecl(decls, prop::BorderBottomColor)->value.color.a == doctest::Approx(0.5f));
    CHECK(findDecl(decls, prop::BorderLeftColor)->value.color.a == doctest::Approx(0));
    CHECK(findDecl(decls, prop::BorderColor) == nullptr);
}

TEST_CASE("values: T-333 border-color one value stays the single BorderColor") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("border-color: #ff0000;", sink, ok);
    CHECK(ok);
    CHECK(sink.all().empty());
    REQUIRE(decls.size() == 1);
    CHECK(decls[0].prop == prop::BorderColor);
    CHECK(decls[0].value.color.r == doctest::Approx(1));
}

TEST_CASE("values: T-333 border-color 2-4 values expand onto the sides (CSS 1-4 rule)") {
    struct Case {
        const char* decl;
        float red[4];  // Expected red channel of top, right, bottom, left.
    };
    // Colors with red = 1 (red), 0.2 (#333), 0.4 (#666), 0.6 (#999).
    const Case cases[] = {
        {"border-color: red #333333", {1, 0.2f, 1, 0.2f}},
        {"border-color: red #333333 #666666", {1, 0.2f, 0.4f, 0.2f}},
        {"border-color: red #333333 #666666 #999999", {1, 0.2f, 0.4f, 0.6f}},
        {"border-color: rgba(255, 0, 0, 1) #333333", {1, 0.2f, 1, 0.2f}},
    };
    const uint16_t sides[4] = {prop::BorderTopColor, prop::BorderRightColor, prop::BorderBottomColor,
                               prop::BorderLeftColor};
    for (const Case& c : cases) {
        CAPTURE(c.decl);
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule(std::string(c.decl) + ";", sink, ok);
        CHECK(ok);
        CHECK(sink.all().empty());
        REQUIRE(decls.size() == 4);
        CHECK(findDecl(decls, prop::BorderColor) == nullptr);
        for (int i = 0; i < 4; ++i) {
            const StyleDecl* d = findDecl(decls, sides[i]);
            REQUIRE(d != nullptr);
            CHECK(d->value.color.r == doctest::Approx(c.red[i]).epsilon(0.01));
        }
    }
}

TEST_CASE("values: T-333 border-color rejects five colors and non-colors") {
    const char* bad[] = {"border-color: red red red red red", "border-color: red 2px",
                         "border-color: red dashed"};
    for (const char* body : bad) {
        CAPTURE(body);
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule(std::string(body) + ";", sink, ok);
        CHECK(decls.empty());
        CHECK_FALSE(sink.all().empty());
    }
}

TEST_CASE("values: T-333 border-style 1-4 values and the longhands") {
    struct Case {
        const char* decl;
        uint32_t style[4];  // top, right, bottom, left (0 none, 1 solid, 2 dashed, 3 dotted).
    };
    const Case cases[] = {
        {"border-style: dashed", {2, 2, 2, 2}},
        {"border-style: dashed dotted", {2, 3, 2, 3}},
        {"border-style: solid none dotted", {1, 0, 3, 0}},
        {"border-style: none solid dashed dotted", {0, 1, 2, 3}},
    };
    const uint16_t sides[4] = {styleprop::BorderTopStyle, styleprop::BorderRightStyle,
                               styleprop::BorderBottomStyle, styleprop::BorderLeftStyle};
    for (const Case& c : cases) {
        CAPTURE(c.decl);
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule(std::string(c.decl) + ";", sink, ok);
        CHECK(ok);
        CHECK(sink.all().empty());
        REQUIRE(decls.size() == 4);
        for (int i = 0; i < 4; ++i) {
            const StyleDecl* d = findDecl(decls, sides[i]);
            REQUIRE(d != nullptr);
            CHECK(d->value.kind == StyleValueKind::Enum);
            CHECK(d->value.e == c.style[i]);
        }
    }
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("border-left-style: dotted; border-top-style: none;", sink, ok);
    CHECK(ok);
    REQUIRE(decls.size() == 2);
    CHECK(findDecl(decls, styleprop::BorderLeftStyle)->value.e == 3u);
    CHECK(findDecl(decls, styleprop::BorderTopStyle)->value.e == 0u);
    CHECK(initialValue(styleprop::BorderTopStyle).e == 1u);  // solid
}

TEST_CASE("values: T-333 border-style rejects other line styles and five values") {
    const char* bad[] = {"border-style: double", "border-style: solid solid solid solid solid",
                         "border-top-style: groove", "border-style: 2px"};
    for (const char* body : bad) {
        CAPTURE(body);
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule(std::string(body) + ";", sink, ok);
        CHECK(decls.empty());
        CHECK_FALSE(sink.all().empty());
    }
}

TEST_CASE("values: T-333 transition and keyframes accept the side colors") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("transition: border-top-color 0.2s, border-left-color 0.1s;", sink, ok);
    CHECK(ok);
    CHECK(sink.all().empty());
    const StyleDecl* t = findDecl(decls, styleprop::Transition);
    REQUIRE(t != nullptr);
    REQUIRE(t->value.transitions.size() == 2);
}

TEST_CASE("T-335 values: rem lengths, in longhands, shorthands and calc()") {
    DiagnosticSink sink;
    bool ok = false;
    auto decls = parseOneRule("width: 2rem; padding: 1rem 0.5rem; height: calc(1rem + 2px);", sink, ok);
    CHECK(ok);
    CHECK(sink.all().empty());
    const StyleDecl* w = findDecl(decls, prop::Width);
    REQUIRE(w != nullptr);
    CHECK(w->value.len.unit == LengthUnit::Rem);
    CHECK(w->value.len.value == doctest::Approx(2));
    const StyleDecl* pl = findDecl(decls, prop::PaddingLeft);
    REQUIRE(pl != nullptr);
    CHECK(pl->value.len.unit == LengthUnit::Rem);
    CHECK(pl->value.len.value == doctest::Approx(0.5f));
    REQUIRE(findDecl(decls, prop::Height) != nullptr);

    // translate accepts rem like em; the matrix path needs px, like em.
    DiagnosticSink sink2;
    auto tr = parseOneRule("transform: translate(1rem, 2px);", sink2, ok);
    CHECK(ok);
    REQUIRE(findDecl(tr, prop::TranslateX) != nullptr);
    CHECK(findDecl(tr, prop::TranslateX)->value.len.unit == LengthUnit::Rem);
    DiagnosticSink sink3;
    CHECK(parseOneRule("transform: rotate(10deg) translate(1rem);", sink3, ok).empty());
    CHECK(sink3.hasErrors());
}

// T-335 (05 §5.9.8): `linear(<stop>, ...)` easing (CSS Easing 1) in every easing position.
namespace {

/// The canonical points of `easing` as parsed from `transition: opacity 1s <easing>`; empty when
/// the declaration was rejected.
std::vector<EasingPoint> linearPointsOf(const std::string& easing) {
    DiagnosticSink sink;
    RuleSet rs;
    const std::string src = "a { transition: opacity 1s " + easing + "; }";
    parseXts(src, "t.xts", 1, sink, rs);
    if (rs.rules.empty()) {
        return {};
    }
    const StyleDecl* d = findDecl(rs.rules[0].decls, styleprop::Transition);
    if (d == nullptr || d->value.transitions.size() != 1) {
        return {};
    }
    const Easing& e = d->value.transitions[0].easing;
    if (e.kind != EasingKind::LinearPoints || size_t{e.pointOffset} + e.pointCount > rs.easingPoints.size()) {
        return {};
    }
    return std::vector<EasingPoint>(rs.easingPoints.begin() + e.pointOffset,
                                    rs.easingPoints.begin() + e.pointOffset + e.pointCount);
}

void checkLinear(const std::string& easing, std::initializer_list<EasingPoint> want) {
    CAPTURE(easing);
    const std::vector<EasingPoint> got = linearPointsOf(easing);
    REQUIRE(got.size() == want.size());
    size_t i = 0;
    for (const EasingPoint& w : want) {
        CAPTURE(i);
        CHECK(got[i].in == doctest::Approx(w.in));
        CHECK(got[i].out == doctest::Approx(w.out));
        ++i;
    }
}

}  // namespace

TEST_CASE("T-335 values: linear() stop syntax (six forms)") {
    // 1. Two bare outputs.
    checkLinear("linear(0, 1)", {{0, 0}, {1, 1}});
    // 2. A bare middle output is spread evenly.
    checkLinear("linear(0, 0.25, 1)", {{0, 0}, {0.5f, 0.25f}, {1, 1}});
    // 3. One input percentage.
    checkLinear("linear(0, 0.5 25%, 1)", {{0, 0}, {0.25f, 0.5f}, {1, 1}});
    // 4. Two input percentages expand to two points.
    checkLinear("linear(0, 0.5 25% 75%, 1)", {{0, 0}, {0.25f, 0.5f}, {0.75f, 0.5f}, {1, 1}});
    // 5. A decreasing input is raised to the largest before it.
    checkLinear("linear(0, 0.3 60%, 0.6 40%, 1)", {{0, 0}, {0.6f, 0.3f}, {0.6f, 0.6f}, {1, 1}});
    // 6. Overshoot outputs, negative values and explicit end inputs.
    checkLinear("linear(-0.2 0%, 1.2 50%, 1 100%)", {{0, -0.2f}, {0.5f, 1.2f}, {1, 1}});
    // Invalid: one stop, none, three percentages, a unit on the output, no comma.
    for (const char* bad :
         {"linear(0)", "linear()", "linear(0 10% 20% 30%, 1)", "linear(0px, 1)", "linear(0 1)"}) {
        CAPTURE(bad);
        CHECK(linearPointsOf(bad).empty());
    }
    // The bare keyword is still CSS `linear`.
    DiagnosticSink sink;
    bool ok = false;
    auto decls = parseOneRule("transition: opacity 1s linear;", sink, ok);
    const StyleDecl* d = findDecl(decls, styleprop::Transition);
    REQUIRE(d != nullptr);
    CHECK(d->value.transitions[0].easing.kind == EasingKind::Linear);
}

TEST_CASE("T-335 values: linear() in animation, @keyframes and @timeline share the RuleSet pool") {
    DiagnosticSink sink;
    RuleSet rs;
    const char* src =
        "@keyframes k { from { width: 0px; animation-timing-function: linear(0, 0.5, 1) } "
        "to { width: 10px } }\n"
        "a { animation: k 1s linear(0, 1 50%, 1) infinite; }\n"
        "@timeline t { #a { 0s { width: 0px } 1s linear(0, 0.9 10%, 1) { width: 5px } } }";
    CHECK(parseXts(src, "t.xts", 1, sink, rs));
    CHECK_FALSE(sink.hasErrors());
    REQUIRE(rs.keyframes.size() == 1);
    const Easing& kf = rs.keyframes[0].stops[0].timing;
    CHECK(rs.keyframes[0].stops[0].hasTiming);
    CHECK(kf.kind == EasingKind::LinearPoints);
    CHECK(kf.pointOffset == 0);
    CHECK(kf.pointCount == 3);
    REQUIRE(rs.rules.size() == 1);
    const StyleDecl* a = findDecl(rs.rules[0].decls, styleprop::Animation);
    REQUIRE(a != nullptr);
    const Easing& an = a->value.animations[0].easing;
    CHECK(an.kind == EasingKind::LinearPoints);
    CHECK(an.pointOffset == 3);
    CHECK(an.pointCount == 3);
    REQUIRE(rs.timelines.size() == 1);
    REQUIRE(rs.timelines[0].tracks.size() == 1);
    const TimelineKey& key = rs.timelines[0].tracks[0].keys[1];
    CHECK(key.hasEasing);
    CHECK(key.easing.kind == EasingKind::LinearPoints);
    CHECK(key.easing.pointOffset == 6);
    REQUIRE(rs.easingPoints.size() == 9);
    CHECK(rs.easingPoints[7].in == doctest::Approx(0.1f));
    CHECK(rs.easingPoints[7].out == doctest::Approx(0.9f));
}

// T-336 (05 §5.9.8): `overflow: auto`, `overflow-x` / `overflow-y`, flex `wrap-reverse`, `order`.
TEST_CASE("values: T-336 property numbers") {
    CHECK(styleprop::OverflowX == 262);
    CHECK(styleprop::OverflowY == 263);
    CHECK(styleprop::kLast >= styleprop::OverflowY);  // T-338 took 264-272.
    CHECK(prop::Order == 143);
    CHECK(prop::kElementPropCount == 144);
    const PropInfo* order = findProp("order");
    REQUIRE(order != nullptr);
    CHECK_FALSE(order->animatable);
    CHECK_FALSE(order->inherited);
    const PropInfo* ox = findProp("overflow-x");
    REQUIRE(ox != nullptr);
    CHECK(ox->prop == styleprop::OverflowX);
    CHECK_FALSE(ox->inherited);
}

TEST_CASE("values: T-336 overflow takes auto; one value stays the shorthand, two set the axes") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("overflow: auto;", sink, ok);
    CHECK(ok);
    CHECK(sink.all().empty());
    REQUIRE(decls.size() == 1);
    CHECK(decls[0].prop == styleprop::Overflow);
    CHECK(decls[0].value.e == 3);

    decls = parseOneRule("overflow: hidden scroll;", sink, ok);
    CHECK(sink.all().empty());
    REQUIRE(decls.size() == 2);
    CHECK(findDecl(decls, styleprop::OverflowX)->value.e == 1);
    CHECK(findDecl(decls, styleprop::OverflowY)->value.e == 2);
    CHECK(findDecl(decls, styleprop::Overflow) == nullptr);

    decls = parseOneRule("overflow-x: auto; overflow-y: visible;", sink, ok);
    CHECK(sink.all().empty());
    REQUIRE(decls.size() == 2);
    CHECK(findDecl(decls, styleprop::OverflowX)->value.e == 3);
    CHECK(findDecl(decls, styleprop::OverflowY)->value.e == 0);

    for (const char* bad : {"overflow: clip;", "overflow: auto auto auto;", "overflow-y: 2px;"}) {
        DiagnosticSink s2;
        CAPTURE(bad);
        decls = parseOneRule(bad, s2, ok);
        CHECK(decls.empty());
        CHECK_FALSE(s2.all().empty());
    }
}

TEST_CASE("values: T-336 flex-wrap wrap-reverse") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("flex-wrap: wrap-reverse;", sink, ok);
    CHECK(ok);
    CHECK(sink.all().empty());
    REQUIRE(decls.size() == 1);
    CHECK(decls[0].prop == styleprop::FlexWrap);
    CHECK(decls[0].value.e == 2);
}

TEST_CASE("values: T-336 order is an integer and is not animatable") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("order: -3;", sink, ok);
    CHECK(ok);
    CHECK(sink.all().empty());
    REQUIRE(decls.size() == 1);
    CHECK(decls[0].prop == prop::Order);
    CHECK(decls[0].value.kind == StyleValueKind::Float);
    CHECK(decls[0].value.f == doctest::Approx(-3));
    for (const char* bad : {"order: 1.5;", "order: 2px;", "order: auto;"}) {
        DiagnosticSink s2;
        CAPTURE(bad);
        decls = parseOneRule(bad, s2, ok);
        CHECK(decls.empty());
        CHECK_FALSE(s2.all().empty());
    }
    DiagnosticSink s3;
    decls = parseOneRule("transition: order 1s;", s3, ok);
    CHECK_FALSE(s3.all().empty());  // Not an animatable property.
}

// T-338 (06 §6.15): the grid subset.
TEST_CASE("values: T-338 property numbers and display: grid") {
    CHECK(styleprop::GridTemplateColumns == 264);
    CHECK(styleprop::GridTemplateRows == 265);
    CHECK(styleprop::GridTemplateAreas == 266);
    CHECK(styleprop::JustifyItems == 267);
    CHECK(styleprop::GridColumnStart == 268);
    CHECK(styleprop::GridColumnEnd == 269);
    CHECK(styleprop::GridRowStart == 270);
    CHECK(styleprop::GridRowEnd == 271);
    CHECK(styleprop::GridArea == 272);
    CHECK(styleprop::kLast >= styleprop::GridArea);  // T-345 took 273-274.
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("display: grid;", sink, ok);
    REQUIRE(findDecl(decls, styleprop::Display) != nullptr);
    CHECK(findDecl(decls, styleprop::Display)->value.e == static_cast<uint32_t>(Display::Grid));
}

TEST_CASE("values: T-338 grid-template-columns / -rows, every track form and repeat()") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule(
        "grid-template-columns: 10px 25% 2fr auto 0 repeat(2, 1fr 5px); grid-template-rows: none;", sink, ok);
    CHECK(sink.all().empty());
    const StyleDecl* cols = findDecl(decls, styleprop::GridTemplateColumns);
    REQUIRE(cols != nullptr);
    REQUIRE(cols->value.kind == StyleValueKind::GridTracks);
    const std::vector<GridTrack>& t = cols->value.gridTracks;
    REQUIRE(t.size() == 9);
    CHECK((t[0].kind == GridTrackKind::Px && t[0].value == doctest::Approx(10)));
    CHECK((t[1].kind == GridTrackKind::Percent && t[1].value == doctest::Approx(25)));
    CHECK((t[2].kind == GridTrackKind::Fr && t[2].value == doctest::Approx(2)));
    CHECK(t[3].kind == GridTrackKind::Auto);
    CHECK((t[4].kind == GridTrackKind::Px && t[4].value == doctest::Approx(0)));
    CHECK((t[5].kind == GridTrackKind::Fr && t[7].kind == GridTrackKind::Fr));
    CHECK((t[6].kind == GridTrackKind::Px && t[8].value == doctest::Approx(5)));
    const StyleDecl* rows = findDecl(decls, styleprop::GridTemplateRows);
    REQUIRE(rows != nullptr);
    CHECK(rows->value.kind == StyleValueKind::GridTracks);
    CHECK(rows->value.gridTracks.empty());
}

TEST_CASE("values: T-338 grid-template-areas builds rectangles, a non-rectangle drops the declaration") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("grid-template-areas: \"a a b\" \"c ... b\";", sink, ok);
    CHECK(sink.all().empty());
    const StyleDecl* areas = findDecl(decls, styleprop::GridTemplateAreas);
    REQUIRE(areas != nullptr);
    CHECK(areas->value.gridAreaRows == 2);
    CHECK(areas->value.gridAreaCols == 3);
    REQUIRE(areas->value.gridAreas.size() == 3);
    const GridAreaDecl& a = areas->value.gridAreas[0];
    CHECK((a.name == "a" && a.rowStart == 0 && a.rowEnd == 1 && a.colStart == 0 && a.colEnd == 2));
    const GridAreaDecl& b = areas->value.gridAreas[1];
    CHECK((b.name == "b" && b.rowStart == 0 && b.rowEnd == 2 && b.colStart == 2 && b.colEnd == 3));
    const GridAreaDecl& c = areas->value.gridAreas[2];
    CHECK((c.name == "c" && c.rowStart == 1 && c.colStart == 0 && c.colEnd == 1));

    for (const char* bad : {"\"a b\" \"b a\"", "\"a a\" \"a .\"", "\"a . a\""}) {
        CAPTURE(bad);
        DiagnosticSink s;
        auto d = parseOneRule(std::string("grid-template-areas: ") + bad + ";", s, ok);
        CHECK(findDecl(d, styleprop::GridTemplateAreas) == nullptr);
        REQUIRE_FALSE(s.all().empty());
        CHECK(s.all()[0].message.find("rectangle") != std::string::npos);
    }
    DiagnosticSink s;
    auto d = parseOneRule("grid-template-areas: \"a b\" \"c\";", s, ok);  // Ragged rows.
    CHECK(findDecl(d, styleprop::GridTemplateAreas) == nullptr);
    CHECK_FALSE(s.all().empty());
}

TEST_CASE("values: T-338 grid lines, the grid-column / grid-row shorthands and grid-area") {
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("grid-column: 2 / span 3; grid-row: span 2; grid-area: hero;", sink, ok);
    CHECK(sink.all().empty());
    CHECK(findDecl(decls, styleprop::GridColumnStart)->value.e == 2u);
    CHECK(findDecl(decls, styleprop::GridColumnEnd)->value.e == (kGridLineSpan | 3u));
    CHECK(findDecl(decls, styleprop::GridRowStart)->value.e == (kGridLineSpan | 2u));
    CHECK(findDecl(decls, styleprop::GridRowEnd)->value.e == kGridLineAuto);
    CHECK(findDecl(decls, styleprop::GridArea)->value.str == "hero");

    decls =
        parseOneRule("grid-row-start: 3; grid-row-end: auto; grid-column: 1 / 4; grid-area: auto;", sink, ok);
    CHECK(sink.all().empty());
    CHECK(findDecl(decls, styleprop::GridRowStart)->value.e == 3u);
    CHECK(findDecl(decls, styleprop::GridRowEnd)->value.e == kGridLineAuto);
    CHECK(findDecl(decls, styleprop::GridColumnEnd)->value.e == 4u);
    CHECK(findDecl(decls, styleprop::GridArea)->value.str.empty());

    decls = parseOneRule(
        "justify-items: center; align-items: start; align-self: end; justify-content: end; "
        "align-content: start;",
        sink, ok);
    CHECK(sink.all().empty());
    CHECK(findDecl(decls, styleprop::JustifyItems)->value.e == 3u);
    CHECK(findDecl(decls, styleprop::AlignItems)->value.e == 1u);
    CHECK(findDecl(decls, styleprop::AlignSelf)->value.e == 3u);
    CHECK(findDecl(decls, styleprop::JustifyContent)->value.e == 1u);
    CHECK(findDecl(decls, styleprop::AlignContent)->value.e == 0u);
}

TEST_CASE("values: T-338 values outside the grid subset are rejected with a warning") {
    struct Case {
        const char* decl;
        const char* mention;
    };
    const Case cases[] = {
        {"grid-template-columns: repeat(auto-fill, 100px);", "subset"},
        {"grid-template-columns: repeat(auto-fit, minmax(100px, 1fr));", "subset"},
        {"grid-template-columns: minmax(10px, 1fr) 1fr;", "subset"},
        {"grid-template-columns: [left] 1fr [right];", "subset"},
        {"grid-template-rows: max-content;", "subset"},
        {"grid-template-columns: 2em;", "grid-template-columns"},
        {"grid-template-columns: repeat(0, 1fr);", "grid-template-columns"},
        {"grid-column-start: -1;", "subset"},
        {"grid-column: main-start / main-end;", "not supported"},
        {"grid-row: 1 / 2 / 3;", "grid-row"},
        {"grid-area: 1 / 2 / 3 / 4;", "grid-area"},
        {"justify-items: baseline;", "justify-items"},
    };
    for (const Case& cs : cases) {
        CAPTURE(cs.decl);
        DiagnosticSink sink;
        bool ok;
        auto decls = parseOneRule(cs.decl, sink, ok);
        CHECK(findDecl(decls, styleprop::GridTemplateColumns) == nullptr);
        CHECK(findDecl(decls, styleprop::GridTemplateRows) == nullptr);
        CHECK(findDecl(decls, styleprop::GridColumnStart) == nullptr);
        CHECK(findDecl(decls, styleprop::GridRowStart) == nullptr);
        CHECK(findDecl(decls, styleprop::GridArea) == nullptr);
        CHECK(findDecl(decls, styleprop::JustifyItems) == nullptr);
        REQUIRE_FALSE(sink.all().empty());
        CHECK(sink.all()[0].message.find(cs.mention) != std::string::npos);
    }
    // Over the track cap (1000 after repeat()).
    DiagnosticSink sink;
    bool ok;
    auto decls = parseOneRule("grid-template-columns: repeat(600, 1fr 1fr);", sink, ok);
    CHECK(findDecl(decls, styleprop::GridTemplateColumns) == nullptr);
}
