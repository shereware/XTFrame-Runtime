#include <cmath>
#include <cstring>
#include <memory>
#include <string>

#include "compile_check.h"
#include "doctest.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/damage.h"
#include "xt/document.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/property.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// T-725 (Docs/tasks/T-725.md; 19 §19.3, 10 §10.15): inside a `perspective` space an element whose
/// transforms are all 2D -- static, or driven by keyframes, a timeline or a transition -- is not a 3D
/// layer. The cause: `transform: translateX(..) translateY(..)` repeats the translate category, so
/// 05.5's compose path writes all ten components, the 3D ones (TranslateZ, RotationX, RotationY) as
/// zero; the keyframe compiler makes a track of each and the Animator turned every Element a PropId
/// 16-22 track writes to into a 3D layer, even one writing only zero (animator.cpp, step 5).

using namespace xt;

namespace {

constexpr float kView = 256.0f;

struct Loaded {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc;
};

/// `body` inside #stage (256 x 256 at the origin; `stageExtra` added to its layout rule).
std::string documentText(const std::string& body, const std::string& layout, const std::string& style,
                         const std::string& stageExtra = "perspective: 600px;",
                         const std::string& timelines = "", const std::string& bodyAttrs = "") {
    return "<xt version=\"2\"><body" + bodyAttrs + "><div id=\"stage\">" + body +
           "</div></body><layout>#stage { position: absolute; left: 0px; top: 0px; width: 256px; height: "
           "256px; " +
           stageExtra + " } " + layout + "</layout><style>" + style + "</style>" + timelines + "</xt>";
}

void load(Loaded& l, const std::string& text) {
    const compile::SourceFile file{"plane.xt", text};
    compile::CompileOptions options;
    options.strict = false;
    compile::CompileResult result = compile::compileDocumentFromStrings({&file, 1}, "plane.xt", options);
    test::requireCompiled(result);
    REQUIRE(Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &l.shaper, nullptr, l.doc) ==
            Status::kOk);
    l.doc->setViewport(Rect{0, 0, kView, kView});
}

bool is3D(Loaded& l, const char* id) {
    ElementRef ref = l.doc->byId(id);
    REQUIRE(ref.valid());
    return l.doc->arena().common(ref.node).is3D != 0;
}

bool isIdentity(const Mat4& m) {
    for (int i = 0; i < 16; ++i) {
        const float want = (i % 5 == 0) ? 1.0f : 0.0f;
        if (m.m[i] != want) {
            return false;
        }
    }
    return true;
}

/// The PushTransform3D commands of the frame that do not carry the identity: the 3D layers (the root
/// plane's own content is wrapped in identity ones, 19 §19.5 step 4).
size_t layerTransforms(Loaded& l) {
    CommandList list;
    l.doc->paint(list);
    size_t n = 0;
    for (const Command& c : list.commands) {
        if (c.kind == CmdKind::PushTransform3D && !isIdentity(list.transforms3[c.index].world3)) {
            ++n;
        }
    }
    return n;
}

const char* const kBoxA = "#a { position: absolute; left: 40px; top: 40px; width: 40px; height: 40px; } ";
const char* const kPaintA = "#a { background-color: #ff0000; } ";
const char* const kDrift =
    "@keyframes drift { 0% { transform: translateX(0px) translateY(0px); } 100% { transform: "
    "translateX(60px) "
    "translateY(-30px); } } ";

/// One element #a under the perspective root, updated at 0 and `t` seconds.
void loadA(Loaded& l, const std::string& layoutA, const std::string& styleA, float t = 0.5f,
           const std::string& timelines = "", const std::string& bodyAttrs = "") {
    load(l, documentText("<div id=\"a\"></div>", std::string(kBoxA) + layoutA, std::string(kPaintA) + styleA,
                         "perspective: 600px;", timelines, bodyAttrs));
    l.doc->update(0.0f);
    l.doc->update(t);
}

}  // namespace

TEST_CASE("T-725 painter: 2D transforms under a perspective root are no 3D layer") {
    SUBCASE("static translate / rotate / scale / skew, fixed order and compose path") {
        Loaded l;
        loadA(l,
              "#a { transform: translateX(3px) translateY(4px) rotate(10deg) skewX(5deg) scale(1.2, 0.9); }",
              "");
        CHECK_FALSE(is3D(l, "a"));
        CHECK(layerTransforms(l) == 0u);
    }
    SUBCASE("keyframes translateX() translateY() (the compose path's zero 3D components)") {
        Loaded l;
        loadA(l, "", std::string("#a { animation: drift 1s linear 0s infinite; } ") + kDrift);
        CHECK_FALSE(is3D(l, "a"));
        CHECK(layerTransforms(l) == 0u);
    }
    SUBCASE("keyframes rotate, scale, skew") {
        Loaded l;
        loadA(
            l, "",
            "#a { animation: turn 1s linear 0s infinite; } @keyframes turn { from { transform: rotate(0deg) "
            "skewX(0deg) scale(1); } to { transform: rotate(90deg) skewX(10deg) scale(1.5); } }");
        CHECK_FALSE(is3D(l, "a"));
        CHECK(layerTransforms(l) == 0u);
    }
    SUBCASE("a timeline writing translateX() translateY()") {
        Loaded l;
        loadA(
            l, "", "", 0.5f,
            "<timelines>@timeline slide { #a { 0ms { transform: translateX(0px) translateY(0px); } 1000ms { "
            "transform: translateX(40px) translateY(20px); } } }</timelines>",
            " on:show=\"play(slide)\"");
        CHECK_FALSE(is3D(l, "a"));
        CHECK(layerTransforms(l) == 0u);
    }
    SUBCASE("a transition of a compose-path transform") {
        Loaded l;
        loadA(l, "#a { transform: translateX(0px) translateY(0px); }",
              "#a { transition: transform 1s linear; }");
        CHECK(l.doc->byId("a").setStyle("transform: translateX(30px) translateY(10px);") == Status::kOk);
        l.doc->update(0.75f);
        CHECK_FALSE(is3D(l, "a"));
        CHECK(layerTransforms(l) == 0u);
    }
}

TEST_CASE("T-725 painter: a 3D component keeps the 3D layer") {
    SUBCASE("static translateZ") {
        Loaded l;
        loadA(l, "#a { transform: translateZ(20px); }", "");
        CHECK(is3D(l, "a"));
        CHECK(layerTransforms(l) == 1u);
    }
    SUBCASE("static rotateX") {
        Loaded l;
        loadA(l, "#a { transform: rotateX(30deg); }", "");
        CHECK(is3D(l, "a"));
        CHECK(layerTransforms(l) == 1u);
    }
    SUBCASE("keyframes rotateY") {
        Loaded l;
        loadA(l, "",
              "#a { animation: flip 1s linear 0s infinite; } @keyframes flip { from { transform: "
              "rotateY(0deg); } "
              "to { transform: rotateY(60deg); } }");
        CHECK(is3D(l, "a"));
        CHECK(layerTransforms(l) == 1u);
    }
    SUBCASE("keyframes on the compose path with a translateZ") {
        Loaded l;
        loadA(l, "",
              "#a { animation: rise 1s linear 0s infinite; } @keyframes rise { from { transform: "
              "translateX(0px) "
              "translateZ(0px); } to { transform: translateX(10px) translateZ(40px); } }");
        CHECK(is3D(l, "a"));
        CHECK(layerTransforms(l) == 1u);
    }
    SUBCASE("keyframes whose 3D component is zero now but not later stay 3D from the first frame") {
        Loaded l;
        loadA(l, "",
              "#a { animation: flip 1s linear 0s infinite; } @keyframes flip { from { transform: "
              "rotateY(0deg); } "
              "to { transform: rotateY(60deg); } }",
              0.0f);
        CHECK(is3D(l, "a"));
    }
}

TEST_CASE("T-725 painter: a 2D element inside a 3D layer stays on that layer's plane") {
    Loaded l;
    load(l,
         documentText("<div id=\"tilt\"><div id=\"a\"></div></div>",
                      std::string("#tilt { position: absolute; left: 20px; top: 20px; width: 200px; height: "
                                  "200px; transform: rotateX(30deg); } ") +
                          kBoxA,
                      std::string(kPaintA) + "#a { animation: drift 1s linear 0s infinite; } " + kDrift));
    l.doc->update(0.0f);
    l.doc->update(0.5f);
    CHECK(is3D(l, "tilt"));
    CHECK_FALSE(is3D(l, "a"));
    CHECK(layerTransforms(l) == 1u);  // the tilted plane only
}

TEST_CASE("T-725 hit test and bounding box: as with the perspective on a container around the 3D content") {
    const std::string body = "<div id=\"wrap\"><div id=\"tilt\"></div></div><div id=\"a\"></div>";
    const std::string layout =
        std::string(kBoxA) +
        "#wrap { position: absolute; left: 0px; top: 0px; width: 256px; height: 256px; } #tilt { position: "
        "absolute; left: 120px; top: 140px; width: 100px; height: 80px; transform: rotateX(40deg); } ";
    const std::string style =
        std::string(kPaintA) +
        "#tilt { background-color: #2040a0; } #a { animation: drift 1s linear 0s infinite; } " + kDrift;
    Loaded persp, wrapped;
    load(persp, documentText(body, layout, style, "perspective: 600px;"));
    load(wrapped, documentText(body, layout + "#wrap { perspective: 600px; } ", style, ""));
    for (Loaded* l : {&persp, &wrapped}) {
        l->doc->update(0.0f);
        l->doc->update(0.5f);
    }
    CHECK_FALSE(is3D(persp, "a"));
    const Rect bp = persp.doc->byId("a").boundingBox();
    const Rect bw = wrapped.doc->byId("a").boundingBox();
    CHECK(bp.x == doctest::Approx(bw.x));
    CHECK(bp.y == doctest::Approx(bw.y));
    CHECK(bp.w == doctest::Approx(bw.w));
    CHECK(bp.h == doctest::Approx(bw.h));
    // Half way: translate (30, -15) from (40, 40); the box's center is at (90, 45).
    CHECK(bw.x == doctest::Approx(70.0f));
    CHECK(bw.y == doctest::Approx(25.0f));
    CHECK(persp.doc->hitTest(Vec2{90, 45}) == persp.doc->byId("a").node);
    CHECK(wrapped.doc->hitTest(Vec2{90, 45}) == wrapped.doc->byId("a").node);
    // Where the box was before it moved: nothing there now.
    CHECK(persp.doc->hitTest(Vec2{45, 75}) != persp.doc->byId("a").node);
    // The tilted plane is hit the same way in both.
    const Vec2 tiltCenter = persp.doc->nodeToScreen(persp.doc->byId("tilt").node, Vec2{50, 40});
    CHECK(persp.doc->hitTest(tiltCenter) == persp.doc->byId("tilt").node);
    CHECK(wrapped.doc->hitTest(tiltCenter) == wrapped.doc->byId("tilt").node);
}

TEST_CASE("T-725 damage: a 2D element moving under a perspective root damages its screen position") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    Context ctx(ContextConfig{&shaper, &assets});
    const std::string text = documentText(
        "<div id=\"a\"></div><div id=\"far\"></div>",
        std::string(kBoxA) +
            "#far { position: absolute; left: 200px; top: 200px; width: 20px; height: 20px; } ",
        std::string(kPaintA) +
            "#far { background-color: #00ff00; } #a { animation: drift 1s linear 0s infinite; } " + kDrift);
    const compile::SourceFile file{"plane.xt", text};
    compile::CompileOptions options;
    compile::CompileResult result = compile::compileDocumentFromStrings({&file, 1}, "plane.xt", options);
    REQUIRE(result.ok());
    Result<DocumentHandle> loaded = ctx.loadDocument(IrBlob{result.ir.data(), result.ir.size()}, true);
    REQUIRE(loaded.ok());
    ctx.setViewport(Rect{0, 0, kView, kView}, 1.0f);
    CommandList list;
    DamageList damage;
    ctx.update(0.0f);
    ctx.render(list, &damage);
    ctx.update(0.25f);
    ctx.render(list, &damage);
    ctx.update(0.25f);  // 0.5 s: translate (30, -15)
    ctx.render(list, &damage);
    REQUIRE_FALSE(damage.full);
    REQUIRE_FALSE(damage.rects.empty());
    Document* doc = ctx.document(loaded.value());
    REQUIRE(doc != nullptr);
    const Rect now = doc->byId("a").boundingBox();
    const Rect before{55.0f, 32.5f, 40.0f, 40.0f};  // 0.25 s: translate (15, -7.5)
    bool coversNow = false;
    bool coversBefore = false;
    for (const Rect& r : damage.rects) {
        coversNow = coversNow || (r.x <= now.x && r.y <= now.y && r.x + r.w >= now.x + now.w &&
                                  r.y + r.h >= now.y + now.h);
        coversBefore = coversBefore || (r.x <= before.x && r.y <= before.y &&
                                        r.x + r.w >= before.x + before.w && r.y + r.h >= before.y + before.h);
        CHECK(intersect(r, Rect{200, 200, 20, 20}).w == 0);  // the still box is not damaged
    }
    CHECK(coversNow);
    CHECK(coversBefore);
}

TEST_CASE("T-725 held timeline member: all-identity 3D components never make it a 3D layer") {
    Loaded l;
    load(l,
         documentText("<div id=\"a\"></div>", kBoxA, std::string(kPaintA) + ".faded { opacity: 0.5; } ",
                      "perspective: 600px;",
                      "<timelines>@timeline slide { #a { 0ms { transform: translateX(0px) translateY(0px); } "
                      "500ms { transform: translateX(40px) translateY(20px); } } }</timelines>"));
    l.doc->update(0.0f);
    PlayOptions hold;
    hold.hold = true;
    REQUIRE(l.doc->playTimeline("", "slide", hold) == Status::kOk);
    for (float t : {0.25f, 0.6f, 0.7f, 0.8f}) {
        l.doc->update(t);
        CHECK_FALSE(is3D(l, "a"));
    }
    REQUIRE(l.doc->byId("a").addClass("faded"));  // a restyle while held
    l.doc->update(0.9f);
    l.doc->update(1.0f);
    CHECK_FALSE(is3D(l, "a"));
    CHECK(layerTransforms(l) == 0u);
    CHECK(l.doc->byId("a").property(prop::TranslateX).f == doctest::Approx(40.0f));
}
