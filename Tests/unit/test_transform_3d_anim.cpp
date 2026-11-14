#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/element_ref.h"
#include "xt/ir.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/property.h"
#include "xt/text_shaper.h"

/// T-702 (Docs/tasks/T-702.md; Docs/design/19-3d-space.md §19.3, §19.10): the 3D transform
/// components (TranslateZ, RotationX, RotationY) animate from `@keyframes`, `transition` and
/// `@timeline` like their 2D counterparts; a lone `translateZ()` from setStyle() takes effect; a
/// rejected IR says why (IrView::openError).

using namespace xt;

namespace {

compile::CompileResult compileRaw(const std::string& css, const std::string& body) {
    const std::string text = "<xt><head><style>" + css + "</style></head><body>" + body + "</body></xt>";
    const compile::SourceFile file{"transform_3d_anim.xt", text};
    return compile::compileDocumentFromStrings({&file, 1}, "transform_3d_anim.xt", compile::CompileOptions{});
}

struct Loaded {
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    Document* doc = nullptr;
    std::vector<uint8_t> ir;

    Loaded(const std::string& css, const std::string& body) {
        compile::CompileResult result = compileRaw(css, body);
        for (const compile::Diagnostic& d : result.diagnostics) {
            CAPTURE(d.message);
            CHECK(d.level != compile::Diagnostic::Level::Error);
        }
        REQUIRE(result.ok());
        ir = std::move(result.ir);
        CAPTURE(IrView::openError(IrBlob{ir.data(), ir.size()}));
        Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{ir.data(), ir.size()}, false);
        REQUIRE(handle.ok());
        doc = ctx.document(handle.value());
        REQUIRE(doc != nullptr);
        ctx.setViewport(Rect{0, 0, 400, 300}, 1.0f);
        ctx.update(0.0f);
    }

    const NodeCommon& common(const char* id) {
        ElementRef ref = doc->byId(id);
        REQUIRE(ref.valid());
        return doc->arena().common(ref.node);
    }
    float cssTranslateX(const char* id) {
        ElementRef ref = doc->byId(id);
        REQUIRE(ref.valid());
        return doc->arena().element(ref.node).cssTranslate.x;
    }
};

constexpr const char* kBox = "position:absolute;left:0px;top:0px;width:100px;height:50px;";

/// Which NodeCommon field a case reads.
enum class Field { TranslateZ, RotationX, RotationY };

float fieldOf(const NodeCommon& c, Field f) {
    switch (f) {
        case Field::TranslateZ:
            return c.translateZ;
        case Field::RotationX:
            return c.rotationX;
        case Field::RotationY:
            return c.rotationY;
    }
    return 0.0f;
}

struct KeyframeCase {
    const char* name;
    const char* from;  // declarations of the `from` stop
    const char* to;    // declarations of the `to` stop
    Field field;
    float end;  // the field's value at the `to` stop (`from` is 0)
};

const KeyframeCase kKeyframeCases[] = {
    {"translateZ", "transform: translateZ(0px)", "transform: translateZ(40px)", Field::TranslateZ, 40.0f},
    {"translate3d", "transform: translate3d(0px, 0px, 0px)", "transform: translate3d(10px, 0px, 40px)",
     Field::TranslateZ, 40.0f},
    {"rotateX", "transform: rotateX(0deg)", "transform: rotateX(60deg)", Field::RotationX, 60.0f},
    {"rotateY", "transform: rotateY(0deg)", "transform: rotateY(90deg)", Field::RotationY, 90.0f},
    {"translate with z", "translate: 0px 0px 0px", "translate: 0px 0px 40px", Field::TranslateZ, 40.0f},
    {"translateZ and rotateY", "transform: translateZ(0px) rotateY(0deg)",
     "transform: translateZ(40px) rotateY(90deg)", Field::RotationY, 90.0f},
};

}  // namespace

TEST_CASE("T-702: @keyframes with a 3D transform compile, load and sample like the 2D components") {
    for (const KeyframeCase& c : kKeyframeCases) {
        SUBCASE(c.name) {
            const std::string css = std::string("#a{") + kBox + "animation: k 1s linear forwards}" +
                                    "@keyframes k{from{" + c.from + "}to{" + c.to + "}}";
            Loaded l(css, R"(<div id="a"></div>)");
            CHECK(fieldOf(l.common("a"), c.field) == doctest::Approx(0.0f));
            l.ctx.update(0.5f);
            CHECK(fieldOf(l.common("a"), c.field) == doctest::Approx(c.end * 0.5f));
            CHECK(l.common("a").is3D == 1);
            l.ctx.update(0.25f);
            CHECK(fieldOf(l.common("a"), c.field) == doctest::Approx(c.end * 0.75f));
            l.ctx.update(1.0f);  // Past the end: `forwards` holds the last keyframe.
            CHECK(fieldOf(l.common("a"), c.field) == doctest::Approx(c.end));
            CHECK(l.common("a").is3D == 1);
        }
    }
}

TEST_CASE("T-702: translate3d's x track samples with the z track") {
    Loaded l(std::string("#a{") + kBox +
                 "animation: k 1s linear}"
                 "@keyframes k{from{transform: translate3d(0px, 0px, 0px)}"
                 "to{transform: translate3d(10px, 0px, 40px)}}",
             R"(<div id="a"></div>)");
    l.ctx.update(0.5f);
    CHECK(l.cssTranslateX("a") == doctest::Approx(5.0f));
    CHECK(l.common("a").translateZ == doctest::Approx(20.0f));
}

TEST_CASE("T-702: 3D @keyframes take easing and fill like 2D ones") {
    // ease-in on 0 -> 100 at p = 0.5 gives the same value on a 3D and a 2D track.
    Loaded l(
        std::string("#e{") + kBox +
            "animation: z 1s ease-in}"
            "#r{" +
            kBox +
            "animation: r 1s ease-in}"
            // Delayed by 1 s: fill none shows the element's own value, backwards the first key.
            "#n{" +
            kBox +
            "transform: translateZ(7px); animation: z 1s linear 1s}"
            "#b{" +
            kBox +
            "transform: translateZ(7px); animation: z 1s linear 1s backwards}"
            "#b2{" +
            kBox +
            "rotate: 7deg; animation: r 1s linear 1s backwards}"
            "@keyframes z{from{transform: translateZ(0px)}to{transform: translateZ(100px)}}"
            "@keyframes r{from{rotate: 0deg}to{rotate: 100deg}}",
        R"(<div id="e"></div><div id="r"></div><div id="n"></div><div id="b"></div><div id="b2"></div>)");
    l.ctx.update(0.5f);
    CHECK(l.common("e").translateZ == doctest::Approx(l.common("r").rotation));
    CHECK(l.common("e").translateZ > 25.0f);
    CHECK(l.common("e").translateZ < 40.0f);
    CHECK(l.common("n").translateZ == doctest::Approx(7.0f));
    CHECK(l.common("b").translateZ == doctest::Approx(0.0f));
    CHECK(l.common("b2").rotation == doctest::Approx(0.0f));
    l.ctx.update(1.0f);  // 0.5 s into the delayed runs.
    CHECK(l.common("n").translateZ == doctest::Approx(50.0f));
    CHECK(l.common("b").translateZ == doctest::Approx(50.0f));
    CHECK(l.common("b2").rotation == doctest::Approx(50.0f));
}

TEST_CASE("T-702: a transition on transform carries the 3D components") {
    Loaded l(std::string("#a{") + kBox + "transition: transform 1s linear}",
             R"(<div id="a"></div><div id="b"></div>)");
    REQUIRE(l.doc->byId("a").setStyle("transform: translateZ(40px) rotateX(20deg) rotateY(60deg)") ==
            Status::kOk);
    l.ctx.update(0.0f);  // The transition starts here.
    l.ctx.update(0.5f);
    CHECK(l.common("a").translateZ == doctest::Approx(20.0f));
    CHECK(l.common("a").rotationX == doctest::Approx(10.0f));
    CHECK(l.common("a").rotationY == doctest::Approx(30.0f));
    l.ctx.update(1.0f);
    CHECK(l.common("a").translateZ == doctest::Approx(40.0f));
    CHECK(l.common("a").rotationY == doctest::Approx(60.0f));
}

TEST_CASE("T-702: a transition from a class change carries translateZ and rotateY") {
    Loaded l(std::string("#a{") + kBox +
                 "transition: transform 1s linear}"
                 ".up{transform: translateZ(30px) rotateY(80deg)}",
             R"(<div id="a"></div>)");
    REQUIRE(l.doc->byId("a").addClass("up"));
    l.ctx.update(0.0f);
    l.ctx.update(0.5f);
    CHECK(l.common("a").translateZ == doctest::Approx(15.0f));
    CHECK(l.common("a").rotationY == doctest::Approx(40.0f));
}

TEST_CASE("T-702: a transition back to no 3D transform leaves a 2D element") {
    Loaded l(std::string("#a{") + kBox + "transform: rotateY(40deg); transition: transform 0.5s linear}",
             R"(<div id="a"></div>)");
    CHECK(l.common("a").is3D == 1);
    REQUIRE(l.doc->byId("a").setStyle("transform: rotateY(0deg)") == Status::kOk);
    l.ctx.update(0.0f);
    l.ctx.update(0.25f);
    CHECK(l.common("a").rotationY == doctest::Approx(20.0f));
    CHECK(l.common("a").is3D == 1);
    l.ctx.update(0.5f);  // Ended: 19.3's next resolve restores is3D from the cascade.
    l.ctx.update(0.016f);
    CHECK(l.common("a").rotationY == doctest::Approx(0.0f));
    CHECK(l.common("a").is3D == 0);
}

TEST_CASE("T-702: @timeline tracks animate translateZ, translate3d, rotateX and rotateY") {
    Loaded l(
        std::string("#g{") + kBox +
            "}"
            "#h{" +
            kBox +
            "}"
            "@timeline tl{"
            "#g{0s{transform: translateZ(0px) rotateX(0deg)} 1s{transform: translateZ(40px) rotateX(30deg)}}"
            "#h{0s{transform: translate3d(0px, 0px, 0px) rotateY(0deg)}"
            " 2s{transform: translate3d(0px, 0px, -80px) rotateY(180deg)}}}",
        R"(<div id="g"></div><div id="h"></div>)");
    REQUIRE(l.doc->playTimeline("", "tl") == Status::kOk);
    l.ctx.update(0.0f);
    l.ctx.update(0.5f);
    CHECK(l.common("g").translateZ == doctest::Approx(20.0f));
    CHECK(l.common("g").rotationX == doctest::Approx(15.0f));
    CHECK(l.common("h").translateZ == doctest::Approx(-20.0f));
    CHECK(l.common("h").rotationY == doctest::Approx(45.0f));
    REQUIRE(l.doc->seekTimeline("", "tl", 1.5f) == Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.common("h").translateZ == doctest::Approx(-60.0f));
    CHECK(l.common("h").rotationY == doctest::Approx(135.0f));
    CHECK(l.common("h").is3D == 1);
}

TEST_CASE("T-702: a runtime animation from setStyle plays a 3D @keyframes") {
    Loaded l(std::string("#a{") + kBox +
                 "}"
                 "@keyframes k{from{transform: translateZ(0px)}to{transform: translateZ(40px)}}",
             R"(<div id="a"></div>)");
    REQUIRE(l.doc->byId("a").setStyle("animation: k 1s linear") == Status::kOk);
    l.ctx.update(0.0f);
    l.ctx.update(0.5f);
    CHECK(l.common("a").translateZ == doctest::Approx(20.0f));
}

TEST_CASE("T-702: setStyle with only 3D transform functions matches the compiled style") {
    Loaded l(std::string("#s{") + kBox + "transform: translateZ(20px)}" + "#s3{" + kBox +
                 "transform: translate3d(4px, 0px, 20px)}" + "#se{" + kBox +
                 "font-size: 10px; transform: translateZ(2em)}",
             R"(<div id="s"></div><div id="s3"></div><div id="se"></div>)"
             R"(<div id="r"></div><div id="r3"></div><div id="re"></div>)");
    REQUIRE(l.doc->byId("r").setStyle("transform: translateZ(20px)") == Status::kOk);
    REQUIRE(l.doc->byId("r3").setStyle("transform: translate3d(4px, 0px, 20px)") == Status::kOk);
    REQUIRE(l.doc->byId("re").setStyle("font-size: 10px; transform: translateZ(2em)") == Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.common("s").translateZ == doctest::Approx(20.0f));
    CHECK(l.common("r").translateZ == doctest::Approx(l.common("s").translateZ));
    CHECK(l.common("r").is3D == 1);
    CHECK(l.common("r3").translateZ == doctest::Approx(l.common("s3").translateZ));
    CHECK(l.cssTranslateX("r3") == doctest::Approx(l.cssTranslateX("s3")));
    CHECK(l.common("re").translateZ == doctest::Approx(l.common("se").translateZ));
    // translateZ(2em) at font-size 10px is 20 px in both (the compiled decl used to read it as 2).
    CHECK(l.common("se").translateZ == doctest::Approx(20.0f));
}

TEST_CASE("T-702: still unsupported 3D forms stay compile errors") {
    // Not defined by 19 §19.10 / 06: rotate3d() and the other 3D-only functions, and a 3D
    // sequence out of 05.5's order.
    const char* bad[] = {
        "transform: rotate3d(0, 1, 0, 30deg)",
        "transform: scale3d(1, 1, 2)",
        "transform: matrix3d(1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1)",
        "transform: perspective(100px)",
    };
    for (const char* decl : bad) {
        CAPTURE(decl);
        compile::CompileResult r =
            compileRaw(std::string("@keyframes k{from{") + decl + "}to{" + decl + "}}#a{animation: k 1s}",
                       R"(<div id="a"></div>)");
        bool error = false;
        for (const compile::Diagnostic& d : r.diagnostics) {
            error = error || d.level == compile::Diagnostic::Level::Error;
        }
        CHECK(error);
    }
}

TEST_CASE("T-702: IrView::openError says why the validator rejects a blob") {
    compile::CompileResult r = compileRaw(
        std::string("#a{") + kBox + "animation: k 1s}" + "@keyframes k{from{opacity: 0}to{opacity: 1}}",
        R"(<div id="a"></div>)");
    REQUIRE(r.ok());
    CHECK(IrView::openError(IrBlob{r.ir.data(), r.ir.size()}).empty());

    // The same document with its opacity track retyped as a Length track: not a value Opacity takes.
    Result<IrView> view = IrView::open(IrBlob{r.ir.data(), r.ir.size()});
    REQUIRE(view.ok());
    std::vector<uint8_t> bad = r.ir;
    bool retyped = false;
    for (uint32_t i = 0; i < view.value().timelineCount(); ++i) {
        for (const IrTrack& t : view.value().timeline(i).tracks()) {
            if (t.prop == prop::Opacity) {
                const size_t at = static_cast<size_t>(reinterpret_cast<const uint8_t*>(&t) - r.ir.data());
                IrTrack edited = t;
                edited.type = static_cast<uint8_t>(ValueType::Length);
                std::memcpy(bad.data() + at, &edited, sizeof edited);
                retyped = true;
            }
        }
    }
    REQUIRE(retyped);
    CHECK_FALSE(IrView::open(IrBlob{bad.data(), bad.size()}).ok());
    const std::string why = IrView::openError(IrBlob{bad.data(), bad.size()});
    CAPTURE(why);
    CHECK(why.find("TLIN") != std::string::npos);
    CHECK(why.find("Opacity") != std::string::npos);

    // Not an .xtir at all; a wrong major keeps versionError()'s wording.
    const uint8_t junk[64] = {};
    CHECK_FALSE(IrView::openError(IrBlob{junk, sizeof junk}).empty());
    std::vector<uint8_t> major2 = r.ir;
    IrHeader header{};
    std::memcpy(&header, major2.data(), sizeof header);
    header.major = 2;
    std::memcpy(major2.data(), &header, sizeof header);
    CHECK(IrView::openError(IrBlob{major2.data(), major2.size()}) ==
          IrView::versionError(IrBlob{major2.data(), major2.size()}));
}
