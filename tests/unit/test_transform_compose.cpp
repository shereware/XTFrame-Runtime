#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "doctest.h"
#include "mini_css.h"
#include "transform_compose.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/parse_xts.h"
#include "xt/compile/style_model.h"
#include "xt/property.h"
#include "xt/types.h"

/// Arbitrary-order `transform` sequences: compose to 4x4, decompose back to 03.3's components.
/// See docs/design/05-style-and-css.md §5.5, docs/tasks/T-120.md.

using namespace xt;

namespace {

constexpr float kDegToRad = 0.017453292519943295f;

const compile::StyleDecl* findDecl(const std::vector<compile::StyleDecl>& decls, uint16_t prop) {
    for (const compile::StyleDecl& d : decls) {
        if (d.prop == prop) {
            return &d;
        }
    }
    return nullptr;
}

std::vector<compile::StyleDecl> parseTransformDecl(const std::string& value, compile::DiagnosticSink& sink,
                                                   bool& ok) {
    compile::RuleSet rs;
    std::string src = "a { transform: " + value + "; }";
    ok = compile::parseXts(src, "t.xts", 1, sink, rs);
    if (rs.rules.empty()) {
        return {};
    }
    return rs.rules[0].decls;
}

/// Collects the components the compiler emitted for `value`. Works for both paths: the fast path
/// only emits the components the source names (the rest stay at their identity), while the
/// compose/decompose path always emits all ten.
bool componentsFromSource(const std::string& value, TransformComponents& out) {
    compile::DiagnosticSink sink;
    bool ok = false;
    std::vector<compile::StyleDecl> decls = parseTransformDecl(value, sink, ok);
    if (!ok) {
        return false;
    }
    out = TransformComponents{};
    // TranslateZ arrives as a Length on the fast path and as a Float on the composed one.
    auto scalar = [&](uint16_t p, float& dst) {
        const compile::StyleDecl* d = findDecl(decls, p);
        if (d != nullptr) {
            dst = d->value.kind == compile::StyleValueKind::Length ? d->value.len.value : d->value.f;
        }
    };
    scalar(prop::TranslateX, out.translateX);
    scalar(prop::TranslateY, out.translateY);
    scalar(prop::TranslateZ, out.translateZ);
    scalar(prop::RotationX, out.rotationX);
    scalar(prop::RotationY, out.rotationY);
    scalar(prop::Rotation, out.rotation);
    scalar(prop::SkewX, out.skewX);
    scalar(prop::SkewY, out.skewY);
    scalar(prop::ScaleX, out.scaleX);
    scalar(prop::ScaleY, out.scaleY);
    return true;
}

/// Element-wise |a - b| < eps over all 16 entries.
bool matrixClose(const Mat4& a, const Mat4& b, float eps) {
    for (int i = 0; i < 16; ++i) {
        if (std::fabs(a.m[i] - b.m[i]) >= eps) {
            return false;
        }
    }
    return true;
}

/// |a - b| < 1e-4, used instead of doctest::Approx because most expected values are 0 and
/// Approx's epsilon is relative.
bool close(float a, float b) { return std::fabs(a - b) < 1e-4f; }

}  // namespace

// T-120 test 2: the field report's `rotateY(18deg) translateZ(40px)` -- the translation follows
// the rotation, so it is not the same as writing the two the other way round.
TEST_CASE("transform compose: rotateY then translateZ moves along the rotated z axis") {
    TransformComponents c;
    REQUIRE(componentsFromSource("rotateY(18deg) translateZ(40px)", c));
    CHECK(close(c.translateX, 40.0f * std::sin(18.0f * kDegToRad)));
    CHECK(close(c.translateY, 0));
    CHECK(close(c.translateZ, 40.0f * std::cos(18.0f * kDegToRad)));
    CHECK(close(c.rotationY, 18));
    CHECK(close(c.rotationX, 0));
    CHECK(close(c.rotation, 0));
    CHECK(close(c.skewX, 0));
    CHECK(close(c.skewY, 0));
    CHECK(close(c.scaleX, 1));
    CHECK(close(c.scaleY, 1));
}

// T-120 test 3: the fixed order is unchanged -- the translation is along the unrotated z axis.
TEST_CASE("transform compose: translateZ then rotateY equals the fast path") {
    TransformComponents c;
    REQUIRE(componentsFromSource("translateZ(40px) rotateY(18deg)", c));
    CHECK(close(c.translateX, 0));
    CHECK(close(c.translateY, 0));
    CHECK(close(c.translateZ, 40));
    CHECK(close(c.rotationY, 18));
}

// T-120 test 4: a purely 2D sequence decomposes to translate/rotate only.
TEST_CASE("transform compose: rotate then translate is a 2D rotated translation") {
    TransformComponents c;
    REQUIRE(componentsFromSource("rotate(30deg) translate(10px, 0px)", c));
    CHECK(close(c.translateX, 10.0f * std::cos(30.0f * kDegToRad)));
    CHECK(close(c.translateY, 10.0f * std::sin(30.0f * kDegToRad)));
    CHECK(close(c.translateZ, 0));
    CHECK(close(c.rotation, 30));
    CHECK(close(c.rotationX, 0));
    CHECK(close(c.rotationY, 0));
    CHECK(close(c.scaleX, 1));
    CHECK(close(c.scaleY, 1));
    CHECK(close(c.skewX, 0));
}

// T-120 test 5: a uniform scale commutes with a rotation; a non-uniform one does not and shows up
// as a skew, which must still rebuild the original matrix.
TEST_CASE("transform compose: uniform scale commutes, non-uniform scale becomes a skew") {
    TransformComponents a;
    REQUIRE(componentsFromSource("scale(2) rotate(45deg)", a));
    CHECK(close(a.rotation, 45));
    CHECK(close(a.scaleX, 2));
    CHECK(close(a.scaleY, 2));
    CHECK(close(a.skewX, 0));

    TransformComponents b;
    REQUIRE(componentsFromSource("rotate(45deg) scale(2)", b));
    CHECK(close(b.rotation, 45));
    CHECK(close(b.scaleX, 2));
    CHECK(close(b.scaleY, 2));
    CHECK(close(b.skewX, 0));

    TransformComponents s;
    REQUIRE(componentsFromSource("scale(2, 1) rotate(45deg)", s));
    CHECK(std::fabs(s.skewX) > 1.0f);  // A real skew, not an identity one.
    TransformFn fns[2];
    fns[0].kind = TransformFn::Kind::Scale;
    fns[0].a = 2;
    fns[0].b = 1;
    fns[1].kind = TransformFn::Kind::RotateZ;
    fns[1].a = 45;
    CHECK(matrixClose(composeTransformComponents(s), composeTransformSequence(fns, 2), 1e-4f));
}

// T-120 test 6: 20 pseudo-random sequences round-trip through the decomposition.
TEST_CASE("transform compose: random sequences rebuild their own matrix") {
    uint32_t seed = 0x5eed1234u;
    auto next = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>((seed >> 8) & 0xFFFFu) / 65535.0f;
    };
    for (int iter = 0; iter < 20; ++iter) {
        std::vector<TransformFn> fns;
        int count = 2 + static_cast<int>(next() * 4.0f);
        for (int i = 0; i < count; ++i) {
            TransformFn f;
            // Kinds 0..5, but never a rotateX/rotateY together with a skew or a non-uniform scale
            // (that is exactly the 3D-shear case 05.5 rejects), so only one 3D rotation axis is
            // used per sequence and always before any 2D-only function.
            int pick = static_cast<int>(next() * 4.0f);
            if (pick == 0) {
                f.kind = TransformFn::Kind::Translate;
                f.a = next() * 100.0f - 50.0f;
                f.b = next() * 100.0f - 50.0f;
                f.c = next() * 100.0f - 50.0f;
            } else if (pick == 1) {
                f.kind = TransformFn::Kind::RotateZ;
                f.a = next() * 300.0f - 150.0f;
            } else if (pick == 2) {
                f.kind = TransformFn::Kind::Skew;
                f.a = next() * 60.0f - 30.0f;
            } else {
                f.kind = TransformFn::Kind::Scale;
                f.a = 0.3f + next() * 2.0f;
                f.b = 0.3f + next() * 2.0f;
            }
            fns.push_back(f);
        }
        // One 3D rotation in front, which keeps the sequence representable (03.3 applies the 3D
        // rotations outside the skew/scale).
        TransformFn r3;
        r3.kind = (iter % 2 == 0) ? TransformFn::Kind::RotateX : TransformFn::Kind::RotateY;
        r3.a = next() * 120.0f - 60.0f;
        fns.insert(fns.begin(), r3);

        Mat4 src = composeTransformSequence(fns.data(), fns.size());
        TransformComponents c;
        CAPTURE(iter);
        REQUIRE(decomposeTransform(src, c) == TransformDecomposeStatus::Ok);
        CHECK(matrixClose(composeTransformComponents(c), src, 1e-3f));
    }
}

// T-120 test 7: a sequence that genuinely needs a 3D shear is a compile error naming it.
// `scale3d()` is unsupported syntax (19.10), and the sheet's `skewY` variant composes to the
// identity here (03.3's skew.y is Lottie's skew *axis*, not a y shear -- see skewDeg()), so the
// case is built with skewX: Rx(30) * shearX(30deg) * Rx(-30) tilts the shear out of the xy plane.
TEST_CASE("transform compose: a 3D shear is a compile error") {
    compile::DiagnosticSink sink;
    bool ok = false;
    std::vector<compile::StyleDecl> decls =
        parseTransformDecl("rotateX(30deg) skewX(30deg) rotateX(-30deg)", sink, ok);
    CHECK(ok == false);
    CHECK(sink.hasErrors());
    CHECK(decls.empty());
    REQUIRE(sink.all().empty() == false);
    CHECK(sink.all()[0].message.find("3D shear") != std::string::npos);
    // 05.5: the message carries the declaration text (the file name and line are sink fields).
    CHECK(sink.all()[0].message.find("skewX(30deg)") != std::string::npos);
    CHECK(sink.all()[0].file == "t.xts");
}

// 05.5: a `%`/`em` translate cannot be folded into a matrix at compile time.
TEST_CASE("transform compose: a non-px length in a reordered sequence is a compile error") {
    compile::DiagnosticSink sink;
    bool ok = false;
    std::vector<compile::StyleDecl> decls = parseTransformDecl("rotate(30deg) translate(10%, 0px)", sink, ok);
    CHECK(ok == false);
    CHECK(sink.hasErrors());
    CHECK(decls.empty());
    REQUIRE(sink.all().empty() == false);
    CHECK(sink.all()[0].message.find("must be px") != std::string::npos);
}

// 05.5: a decomposed skew beyond +/-89 degrees warns but still compiles.
TEST_CASE("transform compose: an extreme skew warns without failing") {
    compile::DiagnosticSink sink;
    bool ok = false;
    std::vector<compile::StyleDecl> decls = parseTransformDecl("scale(1, 0.005) skewX(89.9deg)", sink, ok);
    CHECK(ok);
    CHECK(sink.hasErrors() == false);
    CHECK(decls.size() == 10);
    REQUIRE(sink.all().empty() == false);
    CHECK(sink.all()[0].message.find("89") != std::string::npos);
}

// T-120 test 8: the runtime setStyle path decomposes the same way.
TEST_CASE("transform compose: mini_css agrees with the compiler") {
    std::vector<mini_css::Decl> decls;
    REQUIRE(mini_css::parseDeclarations("transform: rotateY(18deg) translateZ(40px)", decls) == Status::kOk);
    REQUIRE(decls.size() == 10);
    auto find = [&decls](uint16_t p) -> const mini_css::Decl* {
        for (const mini_css::Decl& d : decls) {
            if (d.prop == p) {
                return &d;
            }
        }
        return nullptr;
    };
    REQUIRE(find(prop::TranslateX) != nullptr);
    CHECK(close(find(prop::TranslateX)->value.len.value, 40.0f * std::sin(18.0f * kDegToRad)));
    CHECK(find(prop::TranslateX)->value.len.unit == LengthUnit::Px);
    CHECK(close(find(prop::TranslateZ)->value.f, 40.0f * std::cos(18.0f * kDegToRad)));
    CHECK(close(find(prop::RotationY)->value.f, 18));
    CHECK(close(find(prop::Rotation)->value.f, 0));
    CHECK(close(find(prop::ScaleX)->value.f, 1));

    // A 3D shear is rejected at runtime too (no diagnostic, just kInvalidArgument).
    std::vector<mini_css::Decl> bad;
    CHECK(mini_css::parseDeclarations("transform: rotateX(30deg) skewX(30deg) rotateX(-30deg)", bad) !=
          Status::kOk);
}

// Gimbal lock: rotateY = +/-90 puts the whole remaining rotation into Z, with rotateX at 0.
TEST_CASE("transform compose: gimbal lock keeps rotateX at zero") {
    TransformFn fns[2];
    fns[0].kind = TransformFn::Kind::RotateY;
    fns[0].a = 90;
    fns[1].kind = TransformFn::Kind::RotateX;
    fns[1].a = 25;
    TransformComponents c;
    REQUIRE(decomposeTransformSequence(fns, 2, c) == TransformDecomposeStatus::Ok);
    CHECK(close(c.rotationY, 90));
    CHECK(close(c.rotationX, 0));
    CHECK(matrixClose(composeTransformComponents(c), composeTransformSequence(fns, 2), 1e-3f));
}

// A mirroring sequence puts the flip into scaleY, keeping the rotation proper.
TEST_CASE("transform compose: a negative determinant flips scaleY") {
    TransformFn fns[2];
    fns[0].kind = TransformFn::Kind::Scale;
    fns[0].a = -1;
    fns[0].b = 1;
    fns[1].kind = TransformFn::Kind::RotateZ;
    fns[1].a = 20;
    TransformComponents c;
    REQUIRE(decomposeTransformSequence(fns, 2, c) == TransformDecomposeStatus::Ok);
    CHECK(c.scaleX > 0);
    CHECK(c.scaleY < 0);
    CHECK(matrixClose(composeTransformComponents(c), composeTransformSequence(fns, 2), 1e-4f));
}

// ---------------------------------------------------------------------------------------------
// T-329 (05 §5.9.8): `translate` / `rotate` / `scale` next to `transform` in one block compose as
// translate * rotate * scale * transform (CSS Transforms 2 §6).
// ---------------------------------------------------------------------------------------------

namespace {

std::vector<compile::StyleDecl> parseRuleBody(const std::string& body, compile::DiagnosticSink& sink,
                                              bool& ok) {
    compile::RuleSet rs;
    ok = compile::parseXts("a { " + body + " }", "t.xts", 1, sink, rs);
    if (rs.rules.empty()) {
        return {};
    }
    return rs.rules[0].decls;
}

TransformComponents componentsOf(const std::vector<compile::StyleDecl>& decls) {
    TransformComponents out;
    auto scalar = [&](uint16_t p, float& dst) {
        const compile::StyleDecl* d = findDecl(decls, p);
        if (d != nullptr) {
            dst = d->value.kind == compile::StyleValueKind::Length ? d->value.len.value : d->value.f;
        }
    };
    scalar(prop::TranslateX, out.translateX);
    scalar(prop::TranslateY, out.translateY);
    scalar(prop::TranslateZ, out.translateZ);
    scalar(prop::RotationX, out.rotationX);
    scalar(prop::RotationY, out.rotationY);
    scalar(prop::Rotation, out.rotation);
    scalar(prop::SkewX, out.skewX);
    scalar(prop::SkewY, out.skewY);
    scalar(prop::ScaleX, out.scaleX);
    scalar(prop::ScaleY, out.scaleY);
    return out;
}

}  // namespace

TEST_CASE("transform compose: T-329 an independent property alone passes straight through") {
    compile::DiagnosticSink sink;
    bool ok = false;
    std::vector<compile::StyleDecl> alone = parseRuleBody("rotate: 30deg;", sink, ok);
    REQUIRE(ok);
    REQUIRE(alone.size() == 1);  // Exactly what `transform: rotate(30deg)` writes, nothing composed.
    std::vector<compile::StyleDecl> viaTransform = parseRuleBody("transform: rotate(30deg);", sink, ok);
    REQUIRE(viaTransform.size() == 1);
    CHECK(alone[0].prop == viaTransform[0].prop);
    CHECK(alone[0].value.f == viaTransform[0].value.f);
    CHECK(sink.all().empty());
}

// The hand-computed case: M = T(10, 20) * R(90deg) * S(2) * T(5, 0).
//   T(5, 0) then S(2) moves the origin to (10, 0); R(90) turns that into (0, 10); T(10, 20) adds
//   (10, 20) -> translation (10, 30). The linear part is R(90) * 2 = [0 -2; 2 0].
TEST_CASE("transform compose: T-329 translate * rotate * scale * transform, hand-computed") {
    const Mat4 expected{{0, -2, 0, 10,  //
                         2, 0, 0, 30,   //
                         0, 0, 1, 0,    //
                         0, 0, 0, 1}};
    for (const char* body :
         {"translate: 10px 20px; rotate: 90deg; scale: 2; transform: translate(5px, 0px);",
          // Source order does not matter, only the property.
          "transform: translate(5px, 0px); scale: 2; rotate: 90deg; translate: 10px 20px;"}) {
        CAPTURE(body);
        compile::DiagnosticSink sink;
        bool ok = false;
        std::vector<compile::StyleDecl> decls = parseRuleBody(body, sink, ok);
        REQUIRE(ok);
        CHECK(sink.all().empty());
        CHECK(decls.size() == 10);  // One composed expansion replaced the four declarations.
        TransformComponents c = componentsOf(decls);
        CHECK(close(c.translateX, 10));
        CHECK(close(c.translateY, 30));
        CHECK(close(c.rotation, 90));
        CHECK(close(c.scaleX, 2));
        CHECK(close(c.scaleY, 2));
        CHECK(close(c.skewX, 0));
        CHECK(matrixClose(composeTransformComponents(c), expected, 1e-4f));
    }
}

TEST_CASE("transform compose: T-329 the independent rotate applies outside transform") {
    // R(90) * T(10, 0): the translation is rotated; T(10, 0) * R(90) would leave it at (10, 0).
    compile::DiagnosticSink sink;
    bool ok = false;
    TransformComponents c =
        componentsOf(parseRuleBody("rotate: 90deg; transform: translate(10px, 0px);", sink, ok));
    REQUIRE(ok);
    CHECK(close(c.translateX, 0));
    CHECK(close(c.translateY, 10));
    CHECK(close(c.rotation, 90));
}

TEST_CASE("transform compose: T-329 rotate and transform: rotate() add up") {
    compile::DiagnosticSink sink;
    bool ok = false;
    TransformComponents c = componentsOf(parseRuleBody("rotate: 10deg; transform: rotate(20deg);", sink, ok));
    REQUIRE(ok);
    CHECK(close(c.rotation, 30));
    CHECK(close(c.translateX, 0));
    CHECK(close(c.scaleX, 1));
}

TEST_CASE("transform compose: T-329 a combination in fixed order keeps a percent translate") {
    // translate(-50%, -50%) rotate(45deg) is already in 05.5's order: no matrix, so % survives (T-211).
    compile::DiagnosticSink sink;
    bool ok = false;
    std::vector<compile::StyleDecl> decls =
        parseRuleBody("transform: rotate(45deg); translate: -50% -50%;", sink, ok);
    REQUIRE(ok);
    CHECK(sink.all().empty());
    REQUIRE(decls.size() == 3);
    const compile::StyleDecl* tx = findDecl(decls, prop::TranslateX);
    REQUIRE(tx != nullptr);
    CHECK(tx->value.len.unit == LengthUnit::Percent);
    CHECK(close(tx->value.len.value, -50));
    CHECK(close(findDecl(decls, prop::Rotation)->value.f, 45));

    // Once a matrix is needed the percent cannot be folded at compile time: 05.5's error.
    compile::DiagnosticSink bad;
    parseRuleBody("translate: 50% 0px; transform: translate(10px, 0px);", bad, ok);
    CHECK(bad.hasErrors());
}

TEST_CASE("transform compose: T-329 mini_css combines the same way") {
    auto find = [](const std::vector<mini_css::Decl>& decls, uint16_t p) -> const mini_css::Decl* {
        for (const mini_css::Decl& d : decls) {
            if (d.prop == p) {
                return &d;
            }
        }
        return nullptr;
    };
    std::vector<mini_css::Decl> decls;
    REQUIRE(mini_css::parseDeclarations(
                "translate: 10px 20px; rotate: 90deg; scale: 2; transform: translate(5px, 0px)", decls) ==
            Status::kOk);
    CHECK(decls.size() == 10);
    CHECK(close(find(decls, prop::TranslateX)->value.len.value, 10));
    CHECK(close(find(decls, prop::TranslateY)->value.len.value, 30));
    CHECK(close(find(decls, prop::Rotation)->value.f, 90));
    CHECK(close(find(decls, prop::ScaleX)->value.f, 2));

    std::vector<mini_css::Decl> alone;
    REQUIRE(mini_css::parseDeclarations("rotate: z 15deg", alone) == Status::kOk);
    REQUIRE(alone.size() == 1);
    CHECK(alone[0].prop == prop::Rotation);
    CHECK(close(alone[0].value.f, 15));

    std::vector<mini_css::Decl> pct;
    REQUIRE(mini_css::parseDeclarations("translate: 25% 3px", pct) == Status::kOk);
    REQUIRE(pct.size() == 2);
    CHECK(find(pct, prop::TranslateX)->value.len.unit == LengthUnit::Percent);

    std::vector<mini_css::Decl> none;
    REQUIRE(mini_css::parseDeclarations("scale: none", none) == Status::kOk);
    REQUIRE(none.size() == 2);
    CHECK(close(find(none, prop::ScaleY)->value.f, 1));

    std::vector<mini_css::Decl> bad;
    CHECK(mini_css::parseDeclarations("rotate: x 15deg", bad) != Status::kOk);
    CHECK(mini_css::findProp("translate") == mini_css::kTranslateProp);
}
