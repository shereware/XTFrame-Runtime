#include <memory>
#include <string>

#include "doctest.h"
#include "xt/asset_resolver.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// Document::hitTest() tests: fixtures compiled from tests/layout/hit_test_*.xt (reusing
/// XT_TEST_LAYOUT_DIR/loadLayoutDoc's pattern from test_layout_block.cpp), a 400x300 viewport,
/// one update(0). See docs/design/08-events.md §8.2, docs/tasks/T-32.md "測試".

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

}  // namespace

TEST_CASE("hitTest: two side-by-side divs, empty body area, empty-below-body area") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("hit_test_basic.xt", &shaper, &assets);

    ElementRef a = doc->byId("a");
    ElementRef b = doc->byId("b");
    ElementRef body = doc->byId("body");
    ElementRef root = doc->root();
    REQUIRE(a.valid());
    REQUIRE(b.valid());
    REQUIRE(body.valid());

    CHECK(doc->hitTest(Vec2{50, 25}) == a.node);
    CHECK(doc->hitTest(Vec2{150, 25}) == b.node);
    // Inside body's box (explicit height:50px; a/b are position:absolute, so they never
    // contribute to an auto body height, unlike this explicit one) but past both children (body
    // is a block box, auto width fills the viewport; a/b only cover x in [0, 200)).
    CHECK(doc->hitTest(Vec2{350, 25}) == body.node);
    // Below body's own box entirely: falls through to the `xt` root (docs/tasks/T-32.md "空白處
    // → 根").
    CHECK(doc->hitTest(Vec2{50, 200}) == root.node);
}

TEST_CASE("hitTest: nested child wins over parent; higher z-index wins among siblings") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("hit_test_nested_zindex.xt", &shaper, &assets);

    ElementRef parent = doc->byId("parent");
    ElementRef child = doc->byId("child");
    ElementRef low = doc->byId("low");
    ElementRef high = doc->byId("high");
    REQUIRE(parent.valid());
    REQUIRE(child.valid());
    REQUIRE(low.valid());
    REQUIRE(high.valid());

    // child (10,10)-(30,30) is nested inside parent (0,0)-(100,100).
    CHECK(doc->hitTest(Vec2{20, 20}) == child.node);
    CHECK(doc->hitTest(Vec2{5, 5}) == parent.node);

    // low (150,0)-(210,60) and high (170,20)-(230,80) overlap in (170,20)-(210,60); high has the
    // greater z-index.
    CHECK(doc->hitTest(Vec2{190, 40}) == high.node);
    // Outside the overlap, each still hits its own element.
    CHECK(doc->hitTest(Vec2{155, 5}) == low.node);
    CHECK(doc->hitTest(Vec2{225, 75}) == high.node);
}

TEST_CASE("hitTest: rotate(45deg) -- center hits, original corner does not") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("hit_test_transform.xt", &shaper, &assets);

    ElementRef box = doc->byId("box");
    REQUIRE(box.valid());

    // Unrotated box would span (100,100)-(140,140); its center (120,120) stays inside after a
    // 45deg rotation about the (default 50%/50%) transform-origin, but the original top-left
    // corner (100,100) rotates outside the shape.
    CHECK(doc->hitTest(Vec2{120, 120}) == box.node);
    CHECK(doc->hitTest(Vec2{100, 100}) != box.node);
}

TEST_CASE("hitTest: overflow:hidden clips a child positioned outside the parent's padding box") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("hit_test_overflow.xt", &shaper, &assets);

    ElementRef outer = doc->byId("outer");
    ElementRef inner = doc->byId("inner");
    REQUIRE(outer.valid());
    REQUIRE(inner.valid());

    // inner spans (120,0)-(170,50), entirely outside outer's (0,0)-(100,100) padding box: the
    // whole `outer` subtree is unreachable there.
    NodeId hit = doc->hitTest(Vec2{130, 10});
    CHECK(hit != inner.node);
    CHECK(hit != outer.node);
    // Inside outer's own box, outer itself still hits normally.
    CHECK(doc->hitTest(Vec2{10, 10}) == outer.node);
}

TEST_CASE("hitTest: pointer-events:none skips the element itself but not its descendants") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("hit_test_pointer_events.xt", &shaper, &assets);

    ElementRef parent = doc->byId("parent");
    ElementRef child = doc->byId("child");
    REQUIRE(parent.valid());
    REQUIRE(child.valid());

    CHECK(doc->hitTest(Vec2{30, 30}) == child.node);
    CHECK(doc->hitTest(Vec2{5, 5}) != parent.node);
}

TEST_CASE("hitTest: display:none never hits; visibility:hidden skips self but not its children") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("hit_test_display.xt", &shaper, &assets);

    ElementRef none = doc->byId("none");
    ElementRef hidden = doc->byId("hidden");
    ElementRef hiddenChild = doc->byId("hiddenChild");
    REQUIRE(none.valid());
    REQUIRE(hidden.valid());
    REQUIRE(hiddenChild.valid());

    CHECK(doc->hitTest(Vec2{20, 20}) != none.node);
    // hiddenChild (0,60)-(10,70), in flow inside hidden's (0,60)-(40,100) content box.
    CHECK(doc->hitTest(Vec2{5, 65}) == hiddenChild.node);
    CHECK(doc->hitTest(Vec2{25, 75}) != hidden.node);
}

// T-332 (05 §5.9.8, 08 §8.2 2026-10-14 revision): point-in-shape before entering an element.
TEST_CASE("hitTest: clip-path shapes reject points outside them, subtree included") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("hit_test_clip_path.xt", &shaper, &assets);

    ElementRef inset = doc->byId("inset");
    ElementRef circle = doc->byId("circle");
    ElementRef ellipse = doc->byId("ellipse");
    ElementRef polygon = doc->byId("polygon");
    ElementRef child = doc->byId("child");
    ElementRef rect = doc->byId("rect");
    ElementRef rectchild = doc->byId("rectchild");
    REQUIRE(inset.valid());
    REQUIRE(circle.valid());
    REQUIRE(ellipse.valid());
    REQUIRE(polygon.valid());
    REQUIRE(child.valid());
    REQUIRE(rect.valid());
    REQUIRE(rectchild.valid());

    // inset(10px 20px round 30px): the rect is (20,10)-(80,90) with 30 px corners.
    CHECK(doc->hitTest(Vec2{50, 50}) == inset.node);
    CHECK(doc->hitTest(Vec2{10, 50}) != inset.node);  // Left of the inset edge.
    CHECK(doc->hitTest(Vec2{22, 12}) != inset.node);  // Inside the rect, outside the rounded corner.
    CHECK(doc->hitTest(Vec2{50, 12}) == inset.node);  // Top edge, between the corners.

    // circle(40px at 50% 50%): center (150, 50).
    CHECK(doc->hitTest(Vec2{150, 50}) == circle.node);
    CHECK(doc->hitTest(Vec2{150, 85}) == circle.node);  // 35 px below the center.
    CHECK(doc->hitTest(Vec2{105, 5}) != circle.node);   // The box corner.

    // ellipse(50px 20px): center (250, 50).
    CHECK(doc->hitTest(Vec2{295, 50}) == ellipse.node);  // Along the long axis.
    CHECK(doc->hitTest(Vec2{250, 65}) == ellipse.node);  // 15 px along the short axis.
    CHECK(doc->hitTest(Vec2{250, 25}) != ellipse.node);  // 25 px along the short axis.

    // polygon(50% 0%, 100% 100%, 0% 100%): a triangle with its apex at (350, 0). The child
    // (300,0)-(400,20) lies mostly outside it and is not hit there.
    CHECK(doc->hitTest(Vec2{350, 50}) == polygon.node);  // Inside the triangle, below the child.
    CHECK(doc->hitTest(Vec2{350, 15}) == child.node);    // Inside both: the child wins.
    CHECK(doc->hitTest(Vec2{305, 10}) != child.node);    // Outside the triangle: not the child...
    CHECK(doc->hitTest(Vec2{305, 10}) != polygon.node);  // ... nor the element itself.
    CHECK(doc->hitTest(Vec2{310, 90}) == polygon.node);

    // Combined with overflow: hidden, both must pass: inset(0 300px 0 0) keeps x in [0, 100).
    CHECK(doc->hitTest(Vec2{60, 170}) == rectchild.node);
    CHECK(doc->hitTest(Vec2{120, 170}) != rectchild.node);
    CHECK(doc->hitTest(Vec2{120, 170}) != rect.node);
    CHECK(doc->hitTest(Vec2{20, 200}) == rect.node);
}
