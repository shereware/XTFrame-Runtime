#include <memory>
#include <string>
#include <vector>

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

/// LayoutEngine flex tests: resolveFlexibleLengths (CSS Flexbox §9.7) unit tests, and flex-layout
/// fixtures compiled from Tests/layout/flex_*.xt via compileDocument, checked against a
/// hand-verified layout snapshot. See Docs/tasks/T-26.md, Docs/design/06-layout.md §6.9.

using namespace xt;
using namespace xt::compile;

namespace {

std::string layoutPath(const std::string& name) { return std::string(XT_TEST_LAYOUT_DIR) + "/" + name; }

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

std::string snapshotOf(Document& doc, AssetResolver* assets) {
    LayoutEngine engine;
    engine.init(doc.ir(), assets);
    return engine.snapshot(doc.arena(), doc.root().node);
}

const LayoutBox& boxOf(Document& doc, const char* id) {
    ElementRef ref = doc.byId(id);
    REQUIRE(ref.valid());
    return doc.arena().element(ref.node).box;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// resolveFlexibleLengths unit tests (CSS Flexbox §9.7).
// ---------------------------------------------------------------------------------------------

TEST_CASE("resolveFlexibleLengths: free > 0 distributes by flex-grow") {
    std::vector<FlexBasisInput> items = {
        FlexBasisInput{0, 1, 1, 0, kFlexNoMax},
        FlexBasisInput{0, 1, 1, 0, kFlexNoMax},
        FlexBasisInput{100, 0, 1, 0, kFlexNoMax},
    };
    std::vector<float> sizes;
    resolveFlexibleLengths(items, 300.0f, sizes);
    REQUIRE(sizes.size() == 3);
    CHECK(sizes[0] == doctest::Approx(150));
    CHECK(sizes[1] == doctest::Approx(150));
    CHECK(sizes[2] == doctest::Approx(100));
}

TEST_CASE("resolveFlexibleLengths: free < 0 shrinks by flex-shrink * basis") {
    std::vector<FlexBasisInput> items = {
        FlexBasisInput{200, 0, 1, 0, kFlexNoMax},
        FlexBasisInput{200, 0, 1, 0, kFlexNoMax},
        FlexBasisInput{200, 0, 1, 0, kFlexNoMax},
    };
    std::vector<float> sizes;
    resolveFlexibleLengths(items, -200.0f, sizes);
    REQUIRE(sizes.size() == 3);
    CHECK(sizes[0] == doctest::Approx(133.333f));
    CHECK(sizes[1] == doctest::Approx(133.333f));
    CHECK(sizes[2] == doctest::Approx(133.333f));
}

TEST_CASE("resolveFlexibleLengths: flex-shrink: 0 excludes an item from shrinking") {
    std::vector<FlexBasisInput> items = {
        FlexBasisInput{200, 0, 1, 0, kFlexNoMax},
        FlexBasisInput{200, 0, 0, 0, kFlexNoMax},  // flex-shrink: 0.
        FlexBasisInput{200, 0, 1, 0, kFlexNoMax},
    };
    std::vector<float> sizes;
    resolveFlexibleLengths(items, -200.0f, sizes);
    REQUIRE(sizes.size() == 3);
    CHECK(sizes[0] == doctest::Approx(100));
    CHECK(sizes[1] == doctest::Approx(200));
    CHECK(sizes[2] == doctest::Approx(100));
}

TEST_CASE("resolveFlexibleLengths: a max-width violation freezes and redistributes the rest") {
    std::vector<FlexBasisInput> items = {
        FlexBasisInput{50, 1, 1, 0, 120},  // max-width: 120px.
        FlexBasisInput{50, 1, 1, 0, kFlexNoMax},
        FlexBasisInput{50, 1, 1, 0, kFlexNoMax},
    };
    std::vector<float> sizes;
    resolveFlexibleLengths(items, 280.0f, sizes);
    REQUIRE(sizes.size() == 3);
    CHECK(sizes[0] == doctest::Approx(120));
    CHECK(sizes[1] == doctest::Approx(155));
    CHECK(sizes[2] == doctest::Approx(155));
}

TEST_CASE("resolveFlexibleLengths: free == 0 or empty items leaves basis / no output") {
    std::vector<FlexBasisInput> items = {FlexBasisInput{40, 1, 1, 0, kFlexNoMax}};
    std::vector<float> sizes;
    resolveFlexibleLengths(items, 0.0f, sizes);
    REQUIRE(sizes.size() == 1);
    CHECK(sizes[0] == doctest::Approx(40));

    std::vector<FlexBasisInput> empty;
    resolveFlexibleLengths(empty, 100.0f, sizes);
    CHECK(sizes.empty());
}

TEST_CASE("resolveFlexibleLengths: zero total weight leaves every item at its basis") {
    std::vector<FlexBasisInput> items = {
        FlexBasisInput{50, 0, 1, 0, kFlexNoMax},
        FlexBasisInput{50, 0, 1, 0, kFlexNoMax},
    };
    std::vector<float> sizes;
    resolveFlexibleLengths(items, 100.0f, sizes);  // grow > 0 but every grow weight is 0.
    REQUIRE(sizes.size() == 2);
    CHECK(sizes[0] == doctest::Approx(50));
    CHECK(sizes[1] == doctest::Approx(50));
}

// ---------------------------------------------------------------------------------------------
// Fixture tests.
// ---------------------------------------------------------------------------------------------

TEST_CASE("LayoutEngine: flex_row_basic -- widths stack left to right, height stretches") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, xt::sw::decodeImageBytes);
    std::unique_ptr<Document> doc = loadLayoutDoc("flex_row_basic.xt", &shaper, &assets);

    const LayoutBox& a = boxOf(*doc, "a");
    const LayoutBox& b = boxOf(*doc, "b");
    const LayoutBox& c = boxOf(*doc, "c");
    CHECK(a.x == doctest::Approx(0));
    CHECK(a.width == doctest::Approx(50));
    CHECK(a.height == doctest::Approx(100));
    CHECK(b.x == doctest::Approx(50));
    CHECK(b.width == doctest::Approx(60));
    CHECK(c.x == doctest::Approx(110));
    CHECK(c.width == doctest::Approx(70));
    CHECK(c.height == doctest::Approx(100));

    std::string snap = snapshotOf(*doc, &assets);
    CAPTURE(snap);
    CHECK(snap.find("\"id\":\"c\",\"x\":110,\"y\":0,\"w\":70,\"h\":100") != std::string::npos);
}

// T-74 (Docs/design/06-layout.md §6.4/§6.9): a column container's definite height is the % main
// reference for its items, and a stretched cross size is a definite height for the item's own
// descendants.
TEST_CASE("LayoutEngine: flex_percent_height -- % main size in a column, stretch as a definite height") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, xt::sw::decodeImageBytes);
    std::unique_ptr<Document> doc = loadLayoutDoc("flex_percent_height.xt", &shaper, &assets);

    // #col { height: 300px } -> #col-item { height: 50% } is 150 tall.
    CHECK(boxOf(*doc, "col-item").height == doctest::Approx(150));

    // #row { height: 120px; padding: 10px } -> content height 100; #row-item stretches to it, and
    // #row-child { height: 100% } fills the stretched item.
    CHECK(boxOf(*doc, "row-item").height == doctest::Approx(100));
    CHECK(boxOf(*doc, "row-child").height == doctest::Approx(100));
}

TEST_CASE("LayoutEngine: flex_grow -- two flex: 1 items split the free space, the fixed one stays") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, xt::sw::decodeImageBytes);
    std::unique_ptr<Document> doc = loadLayoutDoc("flex_grow.xt", &shaper, &assets);

    const LayoutBox& a = boxOf(*doc, "a");
    const LayoutBox& b = boxOf(*doc, "b");
    const LayoutBox& c = boxOf(*doc, "c");
    CHECK(a.x == doctest::Approx(0));
    CHECK(a.width == doctest::Approx(150));
    CHECK(b.x == doctest::Approx(150));
    CHECK(b.width == doctest::Approx(150));
    CHECK(c.x == doctest::Approx(300));
    CHECK(c.width == doctest::Approx(100));
}

TEST_CASE("LayoutEngine: flex_shrink -- items shrink by flex-shrink * basis, flex-shrink: 0 doesn't") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, xt::sw::decodeImageBytes);
    std::unique_ptr<Document> doc = loadLayoutDoc("flex_shrink.xt", &shaper, &assets);

    const LayoutBox& a1 = boxOf(*doc, "a1");
    const LayoutBox& a2 = boxOf(*doc, "a2");
    const LayoutBox& a3 = boxOf(*doc, "a3");
    CHECK(a1.width == doctest::Approx(133.333f));
    CHECK(a2.width == doctest::Approx(133.333f));
    CHECK(a3.width == doctest::Approx(133.333f));

    const LayoutBox& f1 = boxOf(*doc, "f1");
    const LayoutBox& f2 = boxOf(*doc, "f2");
    const LayoutBox& f3 = boxOf(*doc, "f3");
    CHECK(f1.width == doctest::Approx(100));
    CHECK(f2.width == doctest::Approx(200));  // flex-shrink: 0.
    CHECK(f3.width == doctest::Approx(100));
}

TEST_CASE("LayoutEngine: flex_basis_minmax -- a max-width freeze redistributes to the other items") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, xt::sw::decodeImageBytes);
    std::unique_ptr<Document> doc = loadLayoutDoc("flex_basis_minmax.xt", &shaper, &assets);

    const LayoutBox& a = boxOf(*doc, "a");
    const LayoutBox& b = boxOf(*doc, "b");
    const LayoutBox& c = boxOf(*doc, "c");
    CHECK(a.width == doctest::Approx(120));
    CHECK(a.x == doctest::Approx(0));
    CHECK(b.width == doctest::Approx(155));
    CHECK(b.x == doctest::Approx(120));
    CHECK(c.width == doctest::Approx(155));
    CHECK(c.x == doctest::Approx(275));
}

TEST_CASE("LayoutEngine: flex_wrap -- five 150px items wrap 2/2/1, align-content: space-between") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, xt::sw::decodeImageBytes);
    std::unique_ptr<Document> doc = loadLayoutDoc("flex_wrap.xt", &shaper, &assets);

    const LayoutBox& a = boxOf(*doc, "a");
    const LayoutBox& b = boxOf(*doc, "b");
    const LayoutBox& c = boxOf(*doc, "c");
    const LayoutBox& d = boxOf(*doc, "d");
    const LayoutBox& e = boxOf(*doc, "e");
    CHECK(a.x == doctest::Approx(0));
    CHECK(a.y == doctest::Approx(0));
    CHECK(b.x == doctest::Approx(150));
    CHECK(b.y == doctest::Approx(0));
    CHECK(c.x == doctest::Approx(0));
    CHECK(c.y == doctest::Approx(80));
    CHECK(d.x == doctest::Approx(150));
    CHECK(d.y == doctest::Approx(80));
    CHECK(e.x == doctest::Approx(0));
    CHECK(e.y == doctest::Approx(160));
}

TEST_CASE("LayoutEngine: flex_justify -- the six justify-content keywords") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, xt::sw::decodeImageBytes);
    std::unique_ptr<Document> doc = loadLayoutDoc("flex_justify.xt", &shaper, &assets);

    CHECK(boxOf(*doc, "s1").x == doctest::Approx(0));
    CHECK(boxOf(*doc, "s2").x == doctest::Approx(80));
    CHECK(boxOf(*doc, "s3").x == doctest::Approx(160));

    CHECK(boxOf(*doc, "e1").x == doctest::Approx(160));
    CHECK(boxOf(*doc, "e2").x == doctest::Approx(240));
    CHECK(boxOf(*doc, "e3").x == doctest::Approx(320));

    CHECK(boxOf(*doc, "c1").x == doctest::Approx(80));
    CHECK(boxOf(*doc, "c2").x == doctest::Approx(160));
    CHECK(boxOf(*doc, "c3").x == doctest::Approx(240));

    CHECK(boxOf(*doc, "b1").x == doctest::Approx(0));
    CHECK(boxOf(*doc, "b2").x == doctest::Approx(160));
    CHECK(boxOf(*doc, "b3").x == doctest::Approx(320));

    CHECK(boxOf(*doc, "r1").x == doctest::Approx(26.6667f));
    CHECK(boxOf(*doc, "r2").x == doctest::Approx(160));
    CHECK(boxOf(*doc, "r3").x == doctest::Approx(293.333f).epsilon(0.001));

    CHECK(boxOf(*doc, "v1").x == doctest::Approx(40));
    CHECK(boxOf(*doc, "v2").x == doctest::Approx(160));
    CHECK(boxOf(*doc, "v3").x == doctest::Approx(280));
}

TEST_CASE("LayoutEngine: flex_align -- align-items four keywords, align-self overrides") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, xt::sw::decodeImageBytes);
    std::unique_ptr<Document> doc = loadLayoutDoc("flex_align.xt", &shaper, &assets);

    CHECK(boxOf(*doc, "s1").height == doctest::Approx(100));
    CHECK(boxOf(*doc, "s1").y == doctest::Approx(0));
    CHECK(boxOf(*doc, "s2").height == doctest::Approx(100));

    CHECK(boxOf(*doc, "fs1").y == doctest::Approx(0));
    CHECK(boxOf(*doc, "fs2").y == doctest::Approx(0));
    CHECK(boxOf(*doc, "selfOverride").y == doctest::Approx(30));  // align-self: center.

    CHECK(boxOf(*doc, "fe1").y == doctest::Approx(80));
    CHECK(boxOf(*doc, "fe2").y == doctest::Approx(60));

    CHECK(boxOf(*doc, "ce1").y == doctest::Approx(40));
    CHECK(boxOf(*doc, "ce2").y == doctest::Approx(30));
}

TEST_CASE("LayoutEngine: flex_column -- column direction stacks by height, align-items: center") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, xt::sw::decodeImageBytes);
    std::unique_ptr<Document> doc = loadLayoutDoc("flex_column.xt", &shaper, &assets);

    const LayoutBox& a = boxOf(*doc, "a");
    const LayoutBox& b = boxOf(*doc, "b");
    const LayoutBox& c = boxOf(*doc, "c");
    CHECK(a.y == doctest::Approx(0));
    CHECK(a.x == doctest::Approx(70));
    CHECK(b.y == doctest::Approx(50));
    CHECK(b.x == doctest::Approx(50));
    CHECK(c.y == doctest::Approx(130));
    CHECK(c.x == doctest::Approx(25));
}

TEST_CASE("LayoutEngine: flex_gap -- row-gap/column-gap and align-content: stretch (default)") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, xt::sw::decodeImageBytes);
    std::unique_ptr<Document> doc = loadLayoutDoc("flex_gap.xt", &shaper, &assets);

    const LayoutBox& a = boxOf(*doc, "a");
    const LayoutBox& b = boxOf(*doc, "b");
    const LayoutBox& c = boxOf(*doc, "c");
    CHECK(a.x == doctest::Approx(0));
    CHECK(a.y == doctest::Approx(0));
    CHECK(b.x == doctest::Approx(170));  // 150 + 20px column-gap.
    CHECK(b.y == doctest::Approx(0));
    CHECK(c.x == doctest::Approx(0));
    CHECK(c.y == doctest::Approx(105));  // line 0 stretched to 95 + 10px row-gap.
}

TEST_CASE("LayoutEngine: flex_auto_margin -- margin-left: auto pushes the last item to the end") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, xt::sw::decodeImageBytes);
    std::unique_ptr<Document> doc = loadLayoutDoc("flex_auto_margin.xt", &shaper, &assets);

    const LayoutBox& a = boxOf(*doc, "a");
    const LayoutBox& b = boxOf(*doc, "b");
    const LayoutBox& c = boxOf(*doc, "c");
    CHECK(a.x == doctest::Approx(0));
    CHECK(b.x == doctest::Approx(80));
    CHECK(c.x == doctest::Approx(320));
    CHECK(c.x + c.width == doctest::Approx(400));
}

TEST_CASE("LayoutEngine: flex_reverse -- row-reverse and column-reverse flip the main axis") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, xt::sw::decodeImageBytes);
    std::unique_ptr<Document> doc = loadLayoutDoc("flex_reverse.xt", &shaper, &assets);

    CHECK(boxOf(*doc, "ra").x == doctest::Approx(320));
    CHECK(boxOf(*doc, "rb").x == doctest::Approx(260));
    CHECK(boxOf(*doc, "rc").x == doctest::Approx(160));

    CHECK(boxOf(*doc, "ca").y == doctest::Approx(220));
    CHECK(boxOf(*doc, "cb").y == doctest::Approx(160));
    CHECK(boxOf(*doc, "cc").y == doctest::Approx(60));
}

TEST_CASE("LayoutEngine: flex_auto_size -- an absolutely positioned flex container shrink-wraps") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, xt::sw::decodeImageBytes);
    std::unique_ptr<Document> doc = loadLayoutDoc("flex_auto_size.xt", &shaper, &assets);

    const LayoutBox& container = boxOf(*doc, "container");
    CHECK(container.width == doctest::Approx(300));  // 100 + 80 + 120.
    CHECK(container.x == doctest::Approx(0));
    CHECK(container.y == doctest::Approx(0));

    CHECK(boxOf(*doc, "a").x == doctest::Approx(0));
    CHECK(boxOf(*doc, "b").x == doctest::Approx(100));
    CHECK(boxOf(*doc, "c").x == doctest::Approx(180));
}

TEST_CASE("LayoutEngine: flex_nested -- a flex item that is itself a flex container") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, xt::sw::decodeImageBytes);
    std::unique_ptr<Document> doc = loadLayoutDoc("flex_nested.xt", &shaper, &assets);

    const LayoutBox& mid = boxOf(*doc, "mid");
    const LayoutBox& side = boxOf(*doc, "side");
    CHECK(mid.x == doctest::Approx(0));
    CHECK(mid.width == doctest::Approx(150));
    CHECK(mid.height == doctest::Approx(100));  // stretched by the outer container's align-items.
    CHECK(side.x == doctest::Approx(150));
    CHECK(side.height == doctest::Approx(100));

    const LayoutBox& inner1 = boxOf(*doc, "inner1");
    const LayoutBox& inner2 = boxOf(*doc, "inner2");
    CHECK(inner1.y == doctest::Approx(0));
    CHECK(inner1.width == doctest::Approx(150));  // stretched by mid's own align-items.
    CHECK(inner2.y == doctest::Approx(30));
    CHECK(inner2.width == doctest::Approx(150));
}
