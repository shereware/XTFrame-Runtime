#include <memory>
#include <string>

#include "doctest.h"
#include "xt/asset_resolver.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/layout.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/sw/sw_png.h"
#include "xt/text_shaper.h"
#include "xt/types.h"
#include "xt/value.h"

/// LayoutEngine tests: resolveLength unit resolution, and block-flow fixtures compiled from
/// tests/layout/*.xt via compileDocument, checked against a hand-verified layout snapshot. See
/// docs/tasks/T-25.md "測試", docs/design/06-layout.md §6.4-6.5, §6.7, §6.10.

using namespace xt;
using namespace xt::compile;

namespace {

std::string layoutPath(const std::string& name) { return std::string(XT_TEST_LAYOUT_DIR) + "/" + name; }

/// Compiles `name` (tests/layout/<name>), loads it into a Document, sets a 400x300 viewport and
/// runs one update(0). `assets` must outlive the returned Document (LayoutEngine only borrows it).
std::unique_ptr<Document> loadLayoutDoc(const std::string& name, TextShaper* shaper, AssetResolver* assets) {
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

/// A fresh LayoutEngine sharing the Document's IrView/assets, for snapshot() only (it does not
/// depend on any per-Document dirty/runtime state).
std::string snapshotOf(Document& doc, AssetResolver* assets) {
    LayoutEngine engine;
    engine.init(doc.ir(), assets);
    return engine.snapshot(doc.arena(), doc.root().node);
}

}  // namespace

TEST_CASE("resolveLength: Px, Percent (defined/undefined reference), Auto") {
    CHECK(resolveLength(Length{42, LengthUnit::Px}, 100, 0) == doctest::Approx(42));
    CHECK(resolveLength(Length{50, LengthUnit::Percent}, 200, 0) == doctest::Approx(100));
    CHECK(resolveLength(Length{50, LengthUnit::Percent}, kUndefinedRef, 7) == doctest::Approx(7));
    CHECK(resolveLength(Length{0, LengthUnit::Auto}, 200, 12) == doctest::Approx(12));
}

TEST_CASE("LayoutEngine: block_stack -- three divs stack vertically with margin") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, xt::sw::decodeImageBytes);
    std::unique_ptr<Document> doc = loadLayoutDoc("block_stack.xt", &shaper, &assets);

    ElementRef a = doc->byId("a");
    ElementRef b = doc->byId("b");
    ElementRef c = doc->byId("c");
    REQUIRE(a.valid());
    REQUIRE(b.valid());
    REQUIRE(c.valid());

    const LayoutBox& ba = doc->arena().element(a.node).box;
    const LayoutBox& bb = doc->arena().element(b.node).box;
    const LayoutBox& bc = doc->arena().element(c.node).box;
    CHECK(ba.y == doctest::Approx(5));
    CHECK(ba.height == doctest::Approx(20));
    CHECK(bb.y == doctest::Approx(35));
    CHECK(bb.height == doctest::Approx(30));
    CHECK(bc.y == doctest::Approx(75));
    CHECK(bc.height == doctest::Approx(40));

    std::string snap = snapshotOf(*doc, &assets);
    CAPTURE(snap);
    CHECK(snap.find("\"id\":\"a\",\"x\":5,\"y\":5,\"w\":390,\"h\":20") != std::string::npos);
    CHECK(snap.find("\"id\":\"c\",\"x\":5,\"y\":75,\"w\":390,\"h\":40") != std::string::npos);
}

TEST_CASE("LayoutEngine: percent_width -- child % width/padding reference the parent content width") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, xt::sw::decodeImageBytes);
    std::unique_ptr<Document> doc = loadLayoutDoc("percent_width.xt", &shaper, &assets);

    ElementRef parent = doc->byId("parent");
    ElementRef child = doc->byId("child");
    REQUIRE(parent.valid());
    REQUIRE(child.valid());
    const LayoutBox& bp = doc->arena().element(parent.node).box;
    const LayoutBox& bc = doc->arena().element(child.node).box;
    CHECK(bp.width == doctest::Approx(200));
    CHECK(bc.width == doctest::Approx(100));        // 50% of 200
    CHECK(bc.padding.left == doctest::Approx(20));  // 10% of 200
    CHECK(bc.height == doctest::Approx(40));        // 0 content + 20 + 20 padding
    CHECK(bp.height == doctest::Approx(40));
}

// T-74 (docs/design/06-layout.md §6.4): a Percent height resolves against the containing block's
// *definite* content height, and behaves as `auto` when that height is content-based.
TEST_CASE("LayoutEngine: percent_height -- % heights reference a definite containing block height") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, xt::sw::decodeImageBytes);
    std::unique_ptr<Document> doc = loadLayoutDoc("percent_height.xt", &shaper, &assets);

    // body { height: 100% } fills the 400x300 viewport, so its children's % heights see 300.
    const LayoutBox& fill = doc->arena().element(doc->byId("fill").node).box;
    CHECK(fill.height == doctest::Approx(300));

    // #auto-parent has an auto (content) height -> #half-of-auto's `height: 50%` behaves as auto,
    // i.e. its own content height (a single 20px child), not 150.
    const LayoutBox& halfOfAuto = doc->arena().element(doc->byId("half-of-auto").node).box;
    CHECK(halfOfAuto.height == doctest::Approx(20));

    // #fixed { height: 200px } -> 50% of its content height, and min-height: 100% of it.
    const LayoutBox& half = doc->arena().element(doc->byId("half").node).box;
    CHECK(half.height == doctest::Approx(100));
    const LayoutBox& minBox = doc->arena().element(doc->byId("min").node).box;
    CHECK(minBox.y == doctest::Approx(100));
    CHECK(minBox.height == doctest::Approx(200));
}

TEST_CASE("LayoutEngine: margin_auto -- horizontal auto margins center the element") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, xt::sw::decodeImageBytes);
    std::unique_ptr<Document> doc = loadLayoutDoc("margin_auto.xt", &shaper, &assets);

    ElementRef a = doc->byId("a");
    REQUIRE(a.valid());
    const LayoutBox& box = doc->arena().element(a.node).box;
    CHECK(box.x == doctest::Approx(150));
    CHECK(box.width == doctest::Approx(100));
}

TEST_CASE("LayoutEngine: min_max -- max-width and min-height clamp the box") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, xt::sw::decodeImageBytes);
    std::unique_ptr<Document> doc = loadLayoutDoc("min_max.xt", &shaper, &assets);

    ElementRef a = doc->byId("a");
    REQUIRE(a.valid());
    const LayoutBox& box = doc->arena().element(a.node).box;
    CHECK(box.width == doctest::Approx(300));  // 500px clamped by max-width: 300px
    CHECK(box.height == doctest::Approx(50));  // 0 content clamped up by min-height: 50px
}

TEST_CASE("LayoutEngine: relative_offset -- a relative element's offset doesn't move its siblings") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, xt::sw::decodeImageBytes);
    std::unique_ptr<Document> doc = loadLayoutDoc("relative_offset.xt", &shaper, &assets);

    ElementRef a = doc->byId("a");
    ElementRef b = doc->byId("b");
    ElementRef c = doc->byId("c");
    REQUIRE(a.valid());
    REQUIRE(b.valid());
    REQUIRE(c.valid());
    const LayoutBox& ba = doc->arena().element(a.node).box;
    const LayoutBox& bb = doc->arena().element(b.node).box;
    const LayoutBox& bc = doc->arena().element(c.node).box;
    CHECK(ba.y == doctest::Approx(0));
    CHECK(bb.x == doctest::Approx(10));
    CHECK(bb.y == doctest::Approx(25));  // 20 (flow) + 5 (top)
    CHECK(bc.y == doctest::Approx(40));  // unaffected by b's offset
}

TEST_CASE("LayoutEngine: display_none -- a none element is skipped, later siblings shift up") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, xt::sw::decodeImageBytes);
    std::unique_ptr<Document> doc = loadLayoutDoc("display_none.xt", &shaper, &assets);

    ElementRef a = doc->byId("a");
    ElementRef b = doc->byId("b");
    ElementRef c = doc->byId("c");
    REQUIRE(a.valid());
    REQUIRE(b.valid());
    REQUIRE(c.valid());
    CHECK(doc->arena().common(b.node).visible == 0);
    const LayoutBox& bb = doc->arena().element(b.node).box;
    CHECK(bb.width == doctest::Approx(0));
    CHECK(bb.height == doctest::Approx(0));
    const LayoutBox& bc = doc->arena().element(c.node).box;
    CHECK(bc.y == doctest::Approx(10));
}

TEST_CASE(
    "LayoutEngine: img_intrinsic -- one side given follows the aspect ratio, none given uses pixel size") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, xt::sw::decodeImageBytes);
    std::unique_ptr<Document> doc = loadLayoutDoc("img_intrinsic.xt", &shaper, &assets);

    ElementRef scaled = doc->byId("scaled");
    ElementRef natural = doc->byId("natural");
    REQUIRE(scaled.valid());
    REQUIRE(natural.valid());
    const LayoutBox& bs = doc->arena().element(scaled.node).box;
    const LayoutBox& bn = doc->arena().element(natural.node).box;
    CHECK(bs.width == doctest::Approx(100));
    CHECK(bs.height == doctest::Approx(100));  // pix.png is 2x2 -> 1:1 ratio
    CHECK(bn.width == doctest::Approx(2));
    CHECK(bn.height == doctest::Approx(2));
}

TEST_CASE("LayoutEngine: anim_fit -- object-fit: contain scales/centers the scene root Group") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, xt::sw::decodeImageBytes);
    std::unique_ptr<Document> doc = loadLayoutDoc("anim_fit.xt", &shaper, &assets);

    ElementRef a = doc->byId("a");
    REQUIRE(a.valid());
    const ElementData& data = doc->arena().element(a.node);
    REQUIRE(data.sceneRoot != kInvalidNode);
    const NodeCommon& groupCommon = doc->arena().common(data.sceneRoot);
    // min_solid_anim.json's design size is 100x100; content box is 100x50 -> contain scale 0.5,
    // centered: tx = (100 - 100*0.5)/2 = 25, ty = (50 - 100*0.5)/2 = 0.
    CHECK(groupCommon.scale.x == doctest::Approx(0.5f));
    CHECK(groupCommon.scale.y == doctest::Approx(0.5f));
    CHECK(groupCommon.translate.x == doctest::Approx(25));
    CHECK(groupCommon.translate.y == doctest::Approx(0));
}

TEST_CASE("LayoutEngine: transform_origin -- anchor/translate follow transform-origin") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, xt::sw::decodeImageBytes);
    std::unique_ptr<Document> doc = loadLayoutDoc("transform_origin.xt", &shaper, &assets);

    ElementRef a = doc->byId("a");
    ElementRef b = doc->byId("b");
    REQUIRE(a.valid());
    REQUIRE(b.valid());
    const NodeCommon& ca = doc->arena().common(a.node);
    const NodeCommon& cb = doc->arena().common(b.node);
    // #a: transform-origin: 0 0 -> anchor (0,0), translate = box position unchanged.
    CHECK(ca.anchor.x == doctest::Approx(0));
    CHECK(ca.anchor.y == doctest::Approx(0));
    CHECK(ca.translate.x == doctest::Approx(0));
    CHECK(ca.translate.y == doctest::Approx(0));
    // #b: transform-origin: 50% 50% of a 40x20 box -> anchor (20, 10); box.y = 20 (after #a).
    CHECK(cb.anchor.x == doctest::Approx(20));
    CHECK(cb.anchor.y == doctest::Approx(10));
    CHECK(cb.translate.x == doctest::Approx(20));
    CHECK(cb.translate.y == doctest::Approx(30));
}

// §19.4/19.10 (T-47a): perspective/perspective-origin resolved to px, relative to the element's
// own border-box.
TEST_CASE("LayoutEngine: perspective -- perspective/perspective-origin resolved to px") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, xt::sw::decodeImageBytes);
    std::unique_ptr<Document> doc = loadLayoutDoc("perspective.xt", &shaper, &assets);

    ElementRef a = doc->byId("a");
    ElementRef b = doc->byId("b");
    REQUIRE(a.valid());
    REQUIRE(b.valid());
    // #a: perspective: 50% of a 200px-wide box -> 100px.
    CHECK(doc->arena().element(a.node).perspectivePx == doctest::Approx(100));
    // #b: perspective-origin: 25% 75% of a 200x100 box -> (50, 75).
    const Vec2& originB = doc->arena().element(b.node).perspectiveOriginPx;
    CHECK(originB.x == doctest::Approx(50));
    CHECK(originB.y == doctest::Approx(75));
}

TEST_CASE("LayoutEngine: nested_padding_border -- world translate accumulates padding+border per level") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, xt::sw::decodeImageBytes);
    std::unique_ptr<Document> doc = loadLayoutDoc("nested_padding_border.xt", &shaper, &assets);

    ElementRef l2 = doc->byId("l2");
    ElementRef l3 = doc->byId("l3");
    REQUIRE(l2.valid());
    REQUIRE(l3.valid());
    // #l2, #l3 use transform-origin: 0 0, so translate == box.x/y + parent padding+border offset.
    CHECK(doc->arena().common(l2.node).translate.x == doctest::Approx(12));  // l1 padding 10 + border 2
    CHECK(doc->arena().common(l2.node).translate.y == doctest::Approx(12));
    CHECK(doc->arena().common(l3.node).translate.x == doctest::Approx(6));  // l2 padding 5 + border 1
    CHECK(doc->arena().common(l3.node).translate.y == doctest::Approx(6));
}
