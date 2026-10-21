#include <cmath>
#include <memory>
#include <string>

#include "doctest.h"
#include "xt/asset_resolver.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/event.h"
#include "xt/input.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// Document::hitTest()/ElementRef::boundingBox() 3D tests: fixtures compiled from
/// tests/layout/hit_test_3d_*.xt, a 400x300 viewport, one update(0). See docs/design/
/// 19-3d-space.md §19.9, docs/tasks/T-48.md "單元測試".

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

InputEvent pointerDown(Vec2 pos) {
    InputEvent ev;
    ev.kind = InputEvent::PointerDown;
    ev.pos = pos;
    return ev;
}

}  // namespace

TEST_CASE("hitTest 3D: rotateY(60deg) card under perspective -- projected point hits, flat corner misses") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("hit_test_3d_card.xt", &shaper, &assets);

    ElementRef card = doc->byId("card");
    REQUIRE(card.valid());

    // The card's own local center (50, 50) sits at its transform-origin (anchor), which a pure
    // rotation leaves exactly at its unprojected screen position -- always inside the projected
    // card regardless of rotation angle.
    Vec2 centerScreen = doc->nodeToScreen(card.node, Vec2{50, 50});
    CHECK(doc->hitTest(centerScreen) == card.node);

    // The card's original (pre-rotation) flat top-left corner (100, 100) is not covered by the
    // rotated/projected shape (data-driven via the same boundingBox() this task adds, rather than
    // a hand-derived pixel: after a 60deg rotation the card's projected AABB is materially
    // narrower than its flat 100x100 box).
    Rect bbox = card.boundingBox();
    CHECK(bbox.w < 100.0f);
    bool cornerInsideBBox =
        100.0f >= bbox.x && 100.0f <= bbox.x + bbox.w && 100.0f >= bbox.y && 100.0f <= bbox.y + bbox.h;
    CHECK_FALSE(cornerInsideBBox);
    CHECK(doc->hitTest(Vec2{100, 100}) != card.node);
}

TEST_CASE(
    "hitTest 3D: boundingBox -- rotateY(60deg) narrows width; translateZ(100px)/perspective(200px) doubles "
    "size") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("hit_test_3d_card.xt", &shaper, &assets);

    ElementRef card = doc->byId("card");
    REQUIRE(card.valid());
    Rect bbox = card.boundingBox();
    CHECK(bbox.w < 100.0f);

    std::unique_ptr<Document> doc2 = loadDoc("hit_test_3d_scale.xt", &shaper, &assets);
    ElementRef box = doc2->byId("box");
    REQUIRE(box.valid());
    // §19.4's CSS perspective formula is a pure scale about perspective-origin for a translateZ-
    // only transform: at z = +100 with perspective(200px), scale = 1 / (1 - 100/200) = 2.
    Rect boxBBox = box.boundingBox();
    CHECK(boxBBox.w == doctest::Approx(100.0f).epsilon(0.02));
    CHECK(boxBBox.h == doctest::Approx(100.0f).epsilon(0.02));
}

TEST_CASE("hitTest 3D: two translateZ layers -- nearer wins in the overlap, backface-hidden never hits") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("hit_test_3d_layers.xt", &shaper, &assets);

    ElementRef back = doc->byId("back");
    ElementRef front = doc->byId("front");
    ElementRef bfHidden = doc->byId("bfHidden");
    ElementRef bfVisible = doc->byId("bfVisible");
    REQUIRE(back.valid());
    REQUIRE(front.valid());
    REQUIRE(bfHidden.valid());
    REQUIRE(bfVisible.valid());

    // #stage2's perspective-origin is pinned to its own top-left (0,0), so translateZ is a pure
    // scale about that corner: back (z=-100, perspective 400px) -> scale 0.8, projecting its flat
    // (50,0)-(130,80) rect to (40,0)-(104,64); front (z=+50) -> scale 1.142857, projecting its
    // flat (80,0)-(160,80) rect to (91.43,0)-(182.86,91.43). #stage2 itself sits at screen (0,150).
    CHECK(doc->hitTest(Vec2{45, 160}) == back.node);    // Inside back only.
    CHECK(doc->hitTest(Vec2{150, 160}) == front.node);  // Inside front only.
    CHECK(doc->hitTest(Vec2{97, 180}) == front.node);   // Overlap: nearer (front) wins.

    // rotateY(180deg) on a flat (z=0-embedded) 2D child is a pure mirror about its own anchor, so
    // both cards project to exactly their own flat (unrotated) screen rect; only backfaceHidden
    // differs.
    CHECK(doc->hitTest(Vec2{50, 50}) != bfHidden.node);
    CHECK(doc->hitTest(Vec2{150, 50}) == bfVisible.node);
}

TEST_CASE("hitTest 3D: a button inside a rotated 3D layer is hit through the layer's own plane") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("hit_test_3d_button.xt", &shaper, &assets);

    ElementRef card = doc->byId("card");
    ElementRef btn = doc->byId("btn");
    REQUIRE(card.valid());
    REQUIRE(btn.valid());

    Vec2 btnCenterScreen = doc->nodeToScreen(btn.node, Vec2{20, 15});
    CHECK(doc->hitTest(btnCenterScreen) == btn.node);
}

TEST_CASE("hitTest: a document with no 3D roots is unaffected by the 3D hit-test branch") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("hit_test_basic.xt", &shaper, &assets);

    ElementRef a = doc->byId("a");
    ElementRef b = doc->byId("b");
    REQUIRE(a.valid());
    REQUIRE(b.valid());
    CHECK(doc->hitTest(Vec2{50, 25}) == a.node);
    CHECK(doc->hitTest(Vec2{150, 25}) == b.node);
}

TEST_CASE(
    "hitTest 3D / T-48b: a sorted section's own root-plane 2D content hits below its 3D layers; a "
    "button inside a 3D layer reports its own local coordinates") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("hit_test_3d_section2d.xt", &shaper, &assets);

    ElementRef cardA = doc->byId("cardA");
    ElementRef cardB = doc->byId("cardB");
    ElementRef btn = doc->byId("btn");
    ElementRef plainDiv = doc->byId("plainDiv");
    REQUIRE(cardA.valid());
    REQUIRE(cardB.valid());
    REQUIRE(btn.valid());
    REQUIRE(plainDiv.valid());

    // #cardA/#cardB are rotateY(180deg) (a pure mirror, no translateZ), so -- per the established
    // property already exercised by "two translateZ layers" above -- they project to exactly their
    // own flat screen rects: cardA (20,20)-(100,100), cardB (100,20)-(180,100). #plainDiv is
    // ordinary 2D content (no 3D transform) in the same sorted section (its wrapper, #wrap, is
    // #stage's only direct child and has3D because cardA/cardB are its descendants), painted below
    // both cards, at flat screen rect (60,60)-(160,160).
    CHECK(doc->hitTest(Vec2{140, 140}) == plainDiv.node);  // Outside both cards: the 2D content hits.
    CHECK(doc->hitTest(Vec2{70, 70}) == cardA.node);  // Inside cardA's overlap with plainDiv: the layer wins.

    Vec2 btnLocalPos{};
    doc->addListener(btn, EventType::PointerDown, [&](Event& ev) { btnLocalPos = ev.localPos; });
    Vec2 btnScreen = doc->nodeToScreen(btn.node, Vec2{15, 10});  // #btn is 30x20; its own center.
    doc->dispatchInput(pointerDown(btnScreen));
    CHECK(std::abs(btnLocalPos.x - 15.0f) < 0.5f);
    CHECK(std::abs(btnLocalPos.y - 10.0f) < 0.5f);
}
