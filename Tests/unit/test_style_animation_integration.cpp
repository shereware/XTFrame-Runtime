#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "xt/asset_resolver.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/event.h"
#include "xt/input.h"
#include "xt/node.h"
#include "xt/property.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// End-to-end style/animation integration (originally T-34, absorbed by T-35): state pseudo-class
/// -> transition, the CSS `animation` list diff and its events, re-targeting a transition
/// mid-flight, and setStyle() driving layout. See Docs/design/05-style-and-css.md §5.6-5.8,
/// Docs/design/04-timeline.md §4.6-4.7, Docs/tasks/T-35.md "整合測試".

using namespace xt;
using namespace xt::compile;

namespace {

std::string layoutPath(const std::string& name) { return std::string(XT_TEST_LAYOUT_DIR) + "/" + name; }

std::unique_ptr<Document> loadDoc(const std::string& name, TextShaper* shaper, AssetResolver* assets) {
    CompileOptions options;
    CompileResult result = compileDocument(layoutPath(name), options);
    for (const Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());

    std::unique_ptr<Document> doc;
    Status status = Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, shaper, assets, doc);
    REQUIRE(status == Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 300});
    doc->update(0.0f);
    return doc;
}

InputEvent pointerMove(Vec2 pos) {
    InputEvent ev;
    ev.kind = InputEvent::PointerMove;
    ev.pos = pos;
    return ev;
}

}  // namespace

TEST_CASE("integration: :hover starts an opacity transition, leaving reverses it") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("style_animation.xt", &shaper, &assets);

    ElementRef box = doc->byId("hoverBox");
    REQUIRE(box.valid());
    CHECK(box.property(prop::Opacity).f == doctest::Approx(1));

    doc->dispatchInput(pointerMove(Vec2{10, 10}));  // inside #hoverBox (0,0)-(60,30)
    REQUIRE(box.hasState(kStateHover));
    doc->update(0.0f);  // Resolves opacity: 0.5 and registers the 0.2s linear transition.
    CHECK(doc->animator().transitionCount() == 1);
    CHECK(box.property(prop::Opacity).f == doctest::Approx(1));

    doc->update(0.1f);  // Half way: 1 -> 0.5.
    CHECK(box.property(prop::Opacity).f == doctest::Approx(0.75f).epsilon(0.02));

    doc->update(0.2f);
    CHECK(box.property(prop::Opacity).f == doctest::Approx(0.5f));
    CHECK(doc->animator().transitionCount() == 0);

    doc->dispatchInput(pointerMove(Vec2{300, 250}));  // Leave.
    CHECK_FALSE(box.hasState(kStateHover));
    doc->update(0.2f);
    doc->update(0.3f);
    CHECK(box.property(prop::Opacity).f == doctest::Approx(0.75f).epsilon(0.02));
    doc->update(0.4f);
    CHECK(box.property(prop::Opacity).f == doctest::Approx(1));
}

// T-79 item 1: `transition: transform` expands into one spec per transform component, so a :hover
// rule that changes `rotateY` transitions instead of snapping.
TEST_CASE("integration: :hover starts a rotateY transition through `transition: transform`") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("style_animation.xt", &shaper, &assets);

    ElementRef box = doc->byId("tfBox");
    REQUIRE(box.valid());
    CHECK(box.property(prop::RotationY).f == doctest::Approx(0));

    doc->dispatchInput(pointerMove(Vec2{310, 210}));  // inside #tfBox (300,200)-(360,230)
    REQUIRE(box.hasState(kStateHover));
    doc->update(0.0f);  // Resolves rotateY(60deg) and registers the 0.2s linear transition.
    CHECK(doc->animator().transitionCount() == 1);
    CHECK(box.property(prop::RotationY).f == doctest::Approx(0));

    doc->update(0.1f);  // Half way: 0 -> 60.
    CHECK(box.property(prop::RotationY).f == doctest::Approx(30).epsilon(0.05));

    doc->update(0.2f);
    CHECK(box.property(prop::RotationY).f == doctest::Approx(60));
    CHECK(doc->animator().transitionCount() == 0);
}

TEST_CASE("integration: addClass/removeClass play and stop a CSS animation, with its events") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("style_animation.xt", &shaper, &assets);

    ElementRef box = doc->byId("spinBox");
    REQUIRE(box.valid());
    CHECK(doc->arena().element(box.node).cssAnimations.empty());

    int iterations = 0;
    doc->addListener(box, EventType::AnimationIteration, [&iterations](Event&) { ++iterations; });

    CHECK(box.addClass("run"));
    doc->update(0.0f);
    const std::vector<uint32_t>& instances = doc->arena().element(box.node).cssAnimations;
    REQUIRE(instances.size() == 1);
    uint32_t id = instances[0];
    CHECK(doc->isPlaying(id));
    CHECK(box.property(prop::Opacity).f == doctest::Approx(1));

    doc->update(0.5f);
    CHECK(box.property(prop::Opacity).f == doctest::Approx(0.75f).epsilon(0.02));
    // `animation: spin 1s linear 0s infinite` -> one AnimationIteration per wrap (08.6).
    doc->update(1.5f);
    CHECK(iterations == 1);
    doc->update(2.5f);
    CHECK(iterations == 2);

    CHECK(box.removeClass("run"));
    doc->update(2.6f);
    CHECK(doc->arena().element(box.node).cssAnimations.empty());
    CHECK_FALSE(doc->isPlaying(id));
    int before = iterations;
    doc->update(4.0f);
    CHECK(iterations == before);
}

TEST_CASE("integration: setStyle relayouts and boundingBox follows") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("style_animation.xt", &shaper, &assets);

    ElementRef box = doc->byId("widthBox");
    REQUIRE(box.valid());
    CHECK(box.layoutBox().width == doctest::Approx(50));
    CHECK(box.boundingBox().w == doctest::Approx(50));

    CHECK(box.setStyle("width: 200px") == Status::kOk);
    doc->update(0.1f);
    CHECK(box.property(prop::Width).len.value == doctest::Approx(200));
    CHECK(box.layoutBox().width == doctest::Approx(200));
    CHECK(box.boundingBox().w == doctest::Approx(200));

    // Setting another property keeps the first one (the list is merged per prop).
    CHECK(box.setStyle("height: 40px") == Status::kOk);
    doc->update(0.2f);
    CHECK(box.layoutBox().width == doctest::Approx(200));
    CHECK(box.layoutBox().height == doctest::Approx(40));
    // Re-setting a property replaces it.
    CHECK(box.setStyle("width: 80px") == Status::kOk);
    doc->update(0.3f);
    CHECK(box.layoutBox().width == doctest::Approx(80));
}

// Docs/design/05-style-and-css.md §5.5's `none` clause, Docs/tasks/T-69.md.
TEST_CASE("integration: `transform: none` is the identity, resets a transform, and animates") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("transform_none.xt", &shaper, &assets);

    // A compiled `transform: none` rule leaves every component at its identity.
    ElementRef none = doc->byId("noneBox");
    REQUIRE(none.valid());
    CHECK(none.property(prop::TranslateX).f == doctest::Approx(0));
    CHECK(none.property(prop::TranslateY).f == doctest::Approx(0));
    CHECK(none.property(prop::TranslateZ).f == doctest::Approx(0));
    CHECK(none.property(prop::RotationX).f == doctest::Approx(0));
    CHECK(none.property(prop::RotationY).f == doctest::Approx(0));
    CHECK(none.property(prop::Rotation).f == doctest::Approx(0));
    CHECK(none.property(prop::SkewX).f == doctest::Approx(0));
    CHECK(none.property(prop::SkewY).f == doctest::Approx(0));
    CHECK(none.property(prop::ScaleX).f == doctest::Approx(1));
    CHECK(none.property(prop::ScaleY).f == doctest::Approx(1));

    // A runtime `setStyle("transform: none")` zeroes a transform the stylesheet set.
    ElementRef spun = doc->byId("spunBox");
    REQUIRE(spun.valid());
    CHECK(spun.property(prop::Rotation).f == doctest::Approx(30));
    CHECK(spun.property(prop::RotationY).f == doctest::Approx(20));
    CHECK(spun.setStyle("transform: none") == Status::kOk);
    doc->update(0.0f);
    CHECK(spun.property(prop::TranslateX).f == doctest::Approx(0));
    CHECK(spun.property(prop::TranslateY).f == doctest::Approx(0));
    CHECK(spun.property(prop::RotationY).f == doctest::Approx(0));
    CHECK(spun.property(prop::Rotation).f == doctest::Approx(0));
    CHECK(spun.property(prop::ScaleX).f == doctest::Approx(1));
    CHECK(spun.property(prop::ScaleY).f == doctest::Approx(1));

    // `@keyframes { 0% { transform: none } 100% { transform: rotateY(180deg) } }` over 2 s: the
    // `none` stop is a real 0-degree keyframe, so 1 s in the rotation is half way there.
    ElementRef flip = doc->byId("flipBox");
    REQUIRE(flip.valid());
    CHECK(flip.property(prop::RotationY).f == doctest::Approx(0));
    doc->update(1.0f);
    CHECK(flip.property(prop::RotationY).f == doctest::Approx(90).epsilon(0.05));
}

TEST_CASE("integration: a transition retargeted mid-flight continues from the current value") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("style_animation.xt", &shaper, &assets);

    ElementRef box = doc->byId("smoothBox");
    REQUIRE(box.valid());
    CHECK(box.setStyle("width: 150px") == Status::kOk);  // transition: width 0.1s linear

    float previous = box.property(prop::Width).len.value;
    float clock = 0.0f;
    float maxStep = 0.0f;
    for (int i = 0; i < 10; ++i) {
        clock += 0.01f;
        if (i == 4) {
            CHECK(box.setStyle("width: 60px") == Status::kOk);  // Retarget half way through.
        }
        doc->update(clock);
        float now = box.property(prop::Width).len.value;
        float step = now > previous ? now - previous : previous - now;
        maxStep = step > maxStep ? step : maxStep;
        previous = now;
    }
    // 50 -> 150 over 0.1s is 10px per 0.01s step; a discontinuity (a jump straight to the new
    // target) would be far larger than that.
    CHECK(maxStep < 20.0f);
    doc->update(clock + 0.2f);
    CHECK(box.property(prop::Width).len.value == doctest::Approx(60));
    CHECK(box.layoutBox().width == doctest::Approx(60));
}

// Docs/design/18-effects.md §18.4, Docs/tasks/T-51b.md: `animation: fx 1s` (T-51a's blur
// keyframes, Tests/layout/style_animation.xt's #fxBox) drives the assigned effect stack's
// paramIndex 1 (params[0], blur's radiusX) through the generic effectParam PropId machinery.
TEST_CASE("integration: `animation: fx 1s` linearly drives the effect's blur radius") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("style_animation.xt", &shaper, &assets);

    ElementRef fx = doc->byId("fxBox");
    REQUIRE(fx.valid());
    REQUIRE(fx.effectCount() == 1);
    CHECK(fx.effect(0).kind == EffectKind::Blur);

    doc->update(0.5f);
    CHECK(fx.property(effectParam(0, 1)).f == doctest::Approx(5.0f).epsilon(0.05));
}

TEST_CASE("integration: elements created at runtime pick up the stylesheet") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("style_animation.xt", &shaper, &assets);

    ElementRef host = doc->byId("host");
    REQUIRE(host.valid());
    ElementRef made = doc->createElement("div");
    CHECK(made.addClass("made"));
    doc->appendChild(host, made);
    doc->update(0.1f);
    CHECK(made.layoutBox().width == doctest::Approx(10));  // `.made { width: 10px; height: 10px }`
    CHECK(made.boundingBox().x == doctest::Approx(200));   // #host is at (200, 0).

    doc->remove(made);
    doc->update(0.2f);
    CHECK_FALSE(made.valid());
}

// Docs/tasks/T-70.md B: the compiler synthesizes a neutral keyframe (Docs/design/12-compiler.md
// §12.3) at 100% for every property the `@keyframes` block has no 100% stop for. Its value blob is
// the all-ones u32 whatever the track's ValueType is, so decoding it as the track's type used to
// produce a NaN Length that the Animator wrote straight into the NodeArena, and
// `ElementRef::property()` handed that NaN back at and past the span's boundary.
TEST_CASE("integration: a trailing neutral keyframe never writes NaN into the arena") {
    constexpr std::string_view kSource =
        "<xt>\n"
        "  <head>\n"
        "    <style>\n"
        "      #box {\n"
        "        position: absolute;\n"
        "        left: 0px;\n"
        "        top: 0px;\n"
        "        width: 100px;\n"
        "        height: 40px;\n"
        "        animation: grow 2s linear forwards;\n"
        "      }\n"
        "      @keyframes grow {\n"
        "        0% { width: 100px }\n"
        "        50% { width: 200px }\n"
        "      }\n"
        "    </style>\n"
        "  </head>\n"
        "  <body><div id=\"box\"></div></body>\n"
        "</xt>\n";

    SourceFile files[1] = {SourceFile{"neutral.xt", kSource}};
    CompileOptions options;
    CompileResult result = compileDocumentFromStrings(files, "neutral.xt", options);
    for (const Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());

    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &shaper, &assets, doc) ==
            Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 300});
    doc->update(0.0f);

    ElementRef box = doc->byId("box");
    REQUIRE(box.valid());
    std::vector<uint32_t> anims = doc->animationsOf(box);
    REQUIRE(anims.size() == 1);
    doc->pause(anims[0]);  // Scrub by hand, like the editor's timeline playhead does.

    // The compiled track is [0 -> 100px, 0.5 -> 200px, 1.0 -> neutral] in the timeline's own
    // (normalized) domain. 0.5..1.0 has a neutral end, so the whole span writes nothing and the
    // base style value (100px) stays -- finite, never the marker's bit pattern.
    doc->seek(anims[0], 0.75f);
    doc->update(0.0f);
    CHECK(std::isfinite(box.property(prop::Width).f));
    CHECK(box.property(prop::Width).f == doctest::Approx(100.0f));

    doc->seek(anims[0], 1.0f);
    doc->update(0.0f);
    CHECK(std::isfinite(box.property(prop::Width).f));
    CHECK(box.property(prop::Width).f == doctest::Approx(100.0f));

    // Exactly on the 50% stop, that stop's own value applies -- a neutral successor only makes the
    // span leading to it neutral.
    doc->seek(anims[0], 0.5f);
    doc->update(0.0f);
    CHECK(box.property(prop::Width).f == doctest::Approx(200.0f));

    // Back inside the neutral span: the last written value stays put, and is still finite.
    doc->seek(anims[0], 1.0f);
    doc->update(0.0f);
    CHECK(std::isfinite(box.property(prop::Width).f));
}

// Regression (reported from the HUD demo): an element with a 3D transform but no space root above
// it (no ancestor `perspective`, no <camera>) became a 3D layer anyway, got an identity `world`
// and jumped to the viewport origin. It must be drawn orthographically in the normal 2D chain.
TEST_CASE("3D transform without a space root: the element keeps its layout position") {
    static const char kSource[] = R"XT(<xt>
  <head><style>
    body { width: 400px; height: 300px; }
    #card { position: absolute; left: 100px; top: 50px; width: 200px; height: 80px;
            transform: rotateY(60deg); }
  </style></head>
  <body><div id="card"></div></body>
</xt>
)XT";
    SourceFile files[1] = {SourceFile{"flat3d.xt", kSource}};
    CompileOptions options;
    CompileResult result = compileDocumentFromStrings(files, "flat3d.xt", options);
    REQUIRE(result.ok());
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &shaper, &assets, doc) ==
            Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 300});
    doc->update(0.0f);

    const Rect box = doc->byId("card").boundingBox();
    // rotateY(60deg) about the center: half the width (cos 60 = 0.5), same center, same y.
    CHECK(box.x == doctest::Approx(150.0f).epsilon(0.01));
    CHECK(box.w == doctest::Approx(100.0f).epsilon(0.01));
    CHECK(box.y == doctest::Approx(50.0f));
    CHECK(box.h == doctest::Approx(80.0f));
}

// T-160 (bench BUG-6, `#boot-scan`): a CSS animation that only moves an element through
// `transform: translateY(...)` writes ElementData::cssTranslate, which 06.7 folds into
// NodeCommon::translate after layout. Nothing else on the page relayouts, so before the fix the
// fold never ran again and the element stayed at its first frame's position (off-screen for the
// bench's sweep line, which starts at translateY(-20px)).
TEST_CASE("integration: a transform-only animation moves the element without a relayout") {
    static const char kSource[] = R"XT(<xt>
  <head><style>
    body { width: 400px; height: 300px; }
    #bar { position: absolute; left: 0px; top: 10px; width: 400px; height: 3px;
           animation: sweep 1s linear 0s 1 normal both; }
    @keyframes sweep {
      from { transform: translateY(-20px); }
      to { transform: translateY(180px); }
    }
  </style></head>
  <body><div id="bar"></div></body>
</xt>
)XT";
    SourceFile files[1] = {SourceFile{"sweep.xt", kSource}};
    CompileOptions options;
    CompileResult result = compileDocumentFromStrings(files, "sweep.xt", options);
    REQUIRE(result.ok());
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &shaper, &assets, doc) ==
            Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 300});
    doc->update(0.0f);
    ElementRef bar = doc->byId("bar");
    REQUIRE(bar.valid());
    CHECK(bar.boundingBox().y == doctest::Approx(-10.0f));

    doc->update(0.5f);  // Half way: translateY(80px).
    CHECK(bar.boundingBox().y == doctest::Approx(90.0f));
    doc->update(1.0f);
    CHECK(bar.boundingBox().y == doctest::Approx(190.0f));
}

// T-211 (05 tail ruling, 06 §6.7): a percent translate resolves against the element's own border
// box after layout; a px <-> % transition interpolates each part and sums them.
namespace {

std::unique_ptr<Document> loadSource(const char* source, NullTextShaper& shaper, FileAssetResolver& assets) {
    SourceFile files[1] = {SourceFile{"pct.xt", source}};
    CompileOptions options;
    CompileResult result = compileDocumentFromStrings(files, "pct.xt", options);
    for (const Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &shaper, &assets, doc) ==
            Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 300});
    doc->update(0.0f);
    return doc;
}

}  // namespace

TEST_CASE("integration: translate(-50%, -50%) centers on the element's own border box") {
    static const char kSource[] = R"XT(<xt>
  <head><style>
    body { width: 400px; height: 300px; }
    #c { position: absolute; left: 200px; top: 150px; width: 100px; height: 40px;
         transform: translate(-50%, -50%); }
    #mix { position: absolute; left: 0px; top: 0px; width: 80px; height: 20px;
           transform: translate(25%, 6px); }
  </style></head>
  <body><div id="c"></div><div id="mix"></div></body>
</xt>
)XT";
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadSource(kSource, shaper, assets);
    ElementRef c = doc->byId("c");
    REQUIRE(c.valid());
    CHECK(c.boundingBox().x == doctest::Approx(150.0f));
    CHECK(c.boundingBox().y == doctest::Approx(130.0f));
    // getProperty keeps reporting the px part only.
    CHECK(c.property(prop::TranslateX).f == doctest::Approx(0.0f));

    ElementRef mix = doc->byId("mix");
    CHECK(mix.boundingBox().x == doctest::Approx(20.0f));
    CHECK(mix.boundingBox().y == doctest::Approx(6.0f));

    // The percent follows the box: a new width re-resolves it at the next fold.
    REQUIRE(c.setStyle("width: 200px") == Status::kOk);
    doc->update(0.0f);
    CHECK(c.boundingBox().x == doctest::Approx(100.0f));
}

TEST_CASE("integration: @keyframes with % translate animate against the box") {
    static const char kSource[] = R"XT(<xt>
  <head><style>
    body { width: 400px; height: 300px; }
    #s { position: absolute; left: 10px; top: 0px; width: 60px; height: 20px;
         animation: slide 1s linear 0s 1 normal both; }
    @keyframes slide {
      from { transform: translateX(0%); }
      to { transform: translateX(100%); }
    }
  </style></head>
  <body><div id="s"></div></body>
</xt>
)XT";
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadSource(kSource, shaper, assets);
    ElementRef s = doc->byId("s");
    REQUIRE(s.valid());
    CHECK(s.boundingBox().x == doctest::Approx(10.0f));
    doc->update(0.5f);
    CHECK(s.boundingBox().x == doctest::Approx(40.0f));
    doc->update(1.0f);
    CHECK(s.boundingBox().x == doctest::Approx(70.0f));
}

TEST_CASE("integration: a px -> % translate transition sums the interpolated parts") {
    static const char kSource[] = R"XT(<xt>
  <head><style>
    body { width: 400px; height: 300px; }
    #t { position: absolute; left: 100px; top: 0px; width: 80px; height: 20px;
         transform: translateX(20px); transition: transform 1s linear; }
    #t.on { transform: translateX(-50%); }
  </style></head>
  <body><div id="t"></div></body>
</xt>
)XT";
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadSource(kSource, shaper, assets);
    ElementRef t = doc->byId("t");
    REQUIRE(t.valid());
    CHECK(t.boundingBox().x == doctest::Approx(120.0f));
    REQUIRE(t.addClass("on"));
    doc->update(0.0f);
    doc->update(0.5f);  // 10px + (-25% of 80) = -10.
    CHECK(t.boundingBox().x == doctest::Approx(90.0f));
    doc->update(1.0f);  // -50% of 80.
    CHECK(t.boundingBox().x == doctest::Approx(60.0f));
    REQUIRE(t.removeClass("on"));
    doc->update(1.0f);
    doc->update(1.5f);  // Back half way: 10px - 20px.
    CHECK(t.boundingBox().x == doctest::Approx(90.0f));
    doc->update(2.0f);
    CHECK(t.boundingBox().x == doctest::Approx(120.0f));
}

TEST_CASE("compile: adjacent translate keyframes mixing px and % are an error") {
    static const char kSource[] = R"XT(<xt>
  <head><style>
    #s { width: 60px; height: 20px; animation: bad 1s; }
    @keyframes bad {
      from { transform: translateX(10px); }
      to { transform: translateX(50%); }
    }
  </style></head>
  <body><div id="s"></div></body>
</xt>
)XT";
    SourceFile files[1] = {SourceFile{"bad.xt", kSource}};
    CompileOptions options;
    CompileResult result = compileDocumentFromStrings(files, "bad.xt", options);
    CHECK_FALSE(result.ok());
    bool found = false;
    for (const Diagnostic& d : result.diagnostics) {
        if (d.level == Diagnostic::Level::Error &&
            d.message.find("adjacent keyframes must use the same unit") != std::string::npos) {
            found = true;
        }
    }
    CHECK(found);
}

// T-211: a keyframed `effect` on an element whose stack has no such effect -- no static `effect`
// (a CSS import animating filter/box-shadow from none), or a class that removed it mid-animation --
// used to trip the Debug assert in setEffectProperty(). The write is now a no-op in every build,
// reported once through Document::warnings().
TEST_CASE("integration: keyframes addressing a missing effect are a no-op with one warning") {
    static const char kSource[] = R"XT(<xt>
  <head><style>
    body { width: 400px; height: 300px; }
    #bare { width: 60px; height: 60px; animation: pulse 1s linear infinite; }
    #lit { width: 60px; height: 60px; effect: glow(0px #2f81f7 0.4); animation: pulse 1s linear infinite; }
    #lit.off { effect: none; }
    @keyframes pulse {
      from { effect: glow(0px #2f81f7 0.4); }
      to { effect: glow(8px #2f81f7 1); }
    }
  </style></head>
  <body><div id="bare"></div><div id="lit"></div></body>
</xt>
)XT";
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadSource(kSource, shaper, assets);
    doc->update(0.5f);
    ElementRef lit = doc->byId("lit");
    REQUIRE(lit.valid());
    REQUIRE(lit.addClass("off"));  // The stack goes away while the animation keeps running.
    doc->update(0.75f);
    doc->update(1.25f);

    CHECK(doc->arena().common(doc->byId("bare").node).effectCount == 0);
    CHECK(doc->arena().common(lit.node).effectCount == 0);
    size_t missing = 0;
    for (const std::string& w : doc->warnings()) {
        missing += w == kWarnEffectMissing ? 1u : 0u;
    }
    CHECK(missing == 1);
}

// T-329 (05 §5.9.8, 06 §6.14.1): `translate` with percentages folds against the element's own
// border box like transform's translate() (T-211); `rotate` next to `transform` composes
// outside it; `@keyframes` animate `scale` through ScaleX/ScaleY.
TEST_CASE("integration: independent translate / rotate / scale") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("independent_transforms.xt", &shaper, &assets);

    ElementRef center = doc->byId("center");
    REQUIRE(center.valid());
    CHECK(center.boundingBox().x == doctest::Approx(150.0f));  // 200 - 50% of 100
    CHECK(center.boundingBox().y == doctest::Approx(130.0f));  // 150 - 50% of 40
    ElementRef mixed = doc->byId("mixed");
    CHECK(mixed.boundingBox().x == doctest::Approx(20.0f));  // 25% of 80
    CHECK(mixed.boundingBox().y == doctest::Approx(6.0f));

    // rotate(90deg) * translate(10px, 0): the translation turns to +y.
    ElementRef combined = doc->byId("combined");
    REQUIRE(combined.valid());
    CHECK(combined.property(prop::Rotation).f == doctest::Approx(90));
    CHECK(combined.property(prop::TranslateX).f == doctest::Approx(0).epsilon(1e-4));
    CHECK(combined.property(prop::TranslateY).f == doctest::Approx(10));

    ElementRef grower = doc->byId("grower");
    REQUIRE(grower.valid());
    CHECK(grower.property(prop::ScaleX).f == doctest::Approx(1));
    doc->update(1.0f);  // Half way: scale 2 1.5.
    CHECK(grower.property(prop::ScaleX).f == doctest::Approx(2));
    CHECK(grower.property(prop::ScaleY).f == doctest::Approx(1.5));

    // setStyle takes the independent properties too (mini_css).
    REQUIRE(center.setStyle("translate: none") == Status::kOk);
    doc->update(1.0f);
    CHECK(center.boundingBox().x == doctest::Approx(200.0f));
    CHECK(center.boundingBox().y == doctest::Approx(150.0f));
    REQUIRE(center.setStyle("rotate: z 15deg") == Status::kOk);
    doc->update(1.0f);
    CHECK(center.property(prop::Rotation).f == doctest::Approx(15));
}
