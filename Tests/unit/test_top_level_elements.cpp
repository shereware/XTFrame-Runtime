#include <memory>
#include <string>

#include "doctest.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/node.h"
#include "xt/property.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// T-158: an element whose CSS `animation` fades its opacity in from 0 must become visible, also
/// when it is not the first element under <body> (the bench report: `#hud` next to
/// `#challenge-menu` stayed invisible). The animated value itself was always right; the cached
/// NodeCommon::worldOpacity was not refreshed (opacity only marks kDirtyPaint), so the Painter
/// culled the element. See Docs/tasks/T-158.md.

using namespace xt;
using namespace xt::compile;

namespace {

std::unique_ptr<Document> loadDoc(const char* name, TextShaper* shaper, AssetResolver* assets) {
    CompileOptions options;
    CompileResult result = compileDocument(std::string(XT_TEST_LAYOUT_DIR) + "/" + name, options);
    for (const Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, shaper, assets, doc) ==
            Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 300});
    return doc;
}

/// Same fixed step as xtframe-view --headless: frame 0 at clock 0, then 1/60 s per frame.
void runFrames(Document& doc, int frames) {
    for (int frame = 0; frame < frames; ++frame) {
        doc.update(static_cast<float>(frame) / 60.0f);
    }
}

/// Number of FillPath commands whose paint is opaque pure red (#hudChild's background).
int redFills(const CommandList& list) {
    int count = 0;
    for (const FillCmd& fill : list.fills) {
        const Color& c = fill.paint.color;
        if (fill.paint.kind == PaintKind::Solid && c.r == 1.0f && c.g == 0.0f && c.b == 0.0f) {
            ++count;
        }
    }
    return count;
}

}  // namespace

TEST_CASE("T-158: every element under <body> shares the one <body> below the document root") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("top_level_animation.xt", &shaper, &assets);
    // Document::root() is the IR's first element (<xt>); <body> is its child, and all three
    // top-level elements hang off that same <body>, so every root()-based walk reaches them.
    ElementRef root = doc->root();
    REQUIRE(root.valid());
    ElementRef body = doc->byId("menu").parent();
    REQUIRE(body.valid());
    CHECK(body.parent().node == root.node);
    for (const char* id : {"menu", "hud", "scan"}) {
        CAPTURE(id);
        ElementRef e = doc->byId(id);
        REQUIRE(e.valid());
        CHECK(e.parent().node == body.node);
    }
}

TEST_CASE("T-158: an opacity animation on a second top-level element runs and is painted") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("top_level_animation.xt", &shaper, &assets);
    ElementRef hud = doc->byId("hud");
    ElementRef hudChild = doc->byId("hudChild");
    ElementRef scan = doc->byId("scan");
    REQUIRE(hud.valid());
    REQUIRE(hudChild.valid());
    REQUIRE(scan.valid());

    runFrames(*doc, 1);  // Frame 0: `both` fill holds the first keyframe.
    CHECK(hud.property(prop::Opacity).f == doctest::Approx(0));
    CommandList first;
    doc->paint(first);
    CHECK(redFills(first) == 0);

    runFrames(*doc, 73);  // 1.2 s: past both animations' ends (900 ms, 1100 ms).
    CHECK(hud.property(prop::Opacity).f == doctest::Approx(1));
    CHECK(hud.computedStyleValue(prop::Opacity).f == doctest::Approx(1));
    CHECK(scan.property(prop::Opacity).f == doctest::Approx(0));
    const NodeArena& arena = doc->arena();
    CHECK(arena.common(hud.node).worldOpacity == doctest::Approx(1));
    CHECK(arena.common(hudChild.node).worldOpacity == doctest::Approx(1));

    CommandList last;
    doc->paint(last);
    CHECK(redFills(last) == 1);
}

TEST_CASE("T-158: mid-animation, the cached worldOpacity follows the animated opacity") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("top_level_animation.xt", &shaper, &assets);
    // Mid-way the cached worldOpacity of #hud's child must follow the animated value every frame.
    runFrames(*doc, 31);  // clock 0.5 s
    ElementRef hud = doc->byId("hud");
    ElementRef hudChild = doc->byId("hudChild");
    float opacity = hud.property(prop::Opacity).f;
    CHECK(opacity > 0.1f);
    CHECK(opacity < 1.0f);
    const NodeArena& arena = doc->arena();
    CHECK(arena.common(hud.node).worldOpacity == doctest::Approx(opacity));
    CHECK(arena.common(hudChild.node).worldOpacity == doctest::Approx(opacity));
}
