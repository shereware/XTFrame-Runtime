#include <algorithm>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "doctest.h"
#include "xt/asset_resolver.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/layout.h"
#include "xt/node.h"
#include "xt/text_shaper.h"

/// T-338 (Docs/design/06-layout.md §6.15): the grid subset -- track sizing, areas, placement,
/// alignment -- against the hand-computed snapshots next to each fixture (Tests/layout/
/// grid_tracks.json, grid_areas.json, grid_align.json), the clamp warning and the runtime
/// (setStyle) side.

using namespace xt;
using namespace xt::compile;

namespace {

std::string layoutPath(const std::string& name) { return std::string(XT_TEST_LAYOUT_DIR) + "/" + name; }

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    std::string s = text.str();
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

std::unique_ptr<Document> loadDoc(const std::string& name, TextShaper* shaper, AssetResolver* assets) {
    CompileOptions options;
    CompileResult result = compileDocument(layoutPath(name), options);
    for (const Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, shaper, assets, doc) ==
            Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 300});
    doc->update(0.0f);
    return doc;
}

void checkFixture(const std::string& stem) {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc(stem + ".xt", &shaper, &assets);
    const std::string expected = readFile(layoutPath(stem + ".json"));
    REQUIRE(!expected.empty());
    LayoutEngine engine;
    engine.init(doc->ir(), &assets);
    CHECK(engine.snapshot(doc->arena(), doc->root().node) == expected);
}

const LayoutBox& boxOf(Document& doc, const char* id) {
    ElementRef ref = doc.byId(id);
    REQUIRE(ref.valid());
    return doc.arena().element(ref.node).box;
}

bool hasWarning(const Document& doc, std::string_view text) {
    return std::find(doc.warnings().begin(), doc.warnings().end(), text) != doc.warnings().end();
}

}  // namespace

TEST_CASE("T-338: grid tracks -- px / fr / auto, repeat() with % and fr, gaps, a negative remainder") {
    checkFixture("grid_tracks");
}

TEST_CASE("T-338: grid areas, a 2 x 2 span, sparse auto-placement, clamping, an absolute child") {
    checkFixture("grid_areas");
}

TEST_CASE("T-338: justify-items / align-items, justify-content / align-content") {
    checkFixture("grid_align");
}

TEST_CASE("T-338: normal / stretch content distribution stretches auto tracks, after the fr tracks") {
    checkFixture("grid_stretch");
}

TEST_CASE("T-338: a placement past the explicit tracks warns once") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("grid_areas.xt", &shaper, &assets);
    CHECK(hasWarning(*doc, kWarnGridPlacementClamped));
    const auto count = static_cast<size_t>(
        std::count(doc->warnings().begin(), doc->warnings().end(), kWarnGridPlacementClamped));
    doc->setViewport(Rect{0, 0, 420, 300});
    doc->update(0.1f);
    CHECK(std::count(doc->warnings().begin(), doc->warnings().end(), kWarnGridPlacementClamped) == count);
    CHECK(count == 1);

    std::unique_ptr<Document> clean = loadDoc("grid_tracks.xt", &shaper, &assets);
    CHECK_FALSE(hasWarning(*clean, kWarnGridPlacementClamped));
}

TEST_CASE("T-338: setStyle places an item by lines; templates and grid-area are compile-time only") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("grid_areas.xt", &shaper, &assets);
    // #k2 asks for both columns from line 1: the sparse cursor is already past column 1 (after #k),
    // so it goes to the next row, the implicit one.
    REQUIRE(doc->byId("k2").setStyle("grid-column: 1 / span 2") == Status::kOk);
    doc->update(0.1f);
    CHECK(boxOf(*doc, "k2").x == doctest::Approx(0));
    CHECK(boxOf(*doc, "k2").y == doctest::Approx(20));
    CHECK(boxOf(*doc, "k2").width == doctest::Approx(100));
    // Both lines definite: row 3, column 4 of the 4 x 3 grid.
    REQUIRE(doc->byId("a").setStyle("grid-row: 3; grid-column: 4") == Status::kOk);
    doc->update(0.2f);
    CHECK(boxOf(*doc, "a").x == doctest::Approx(150));
    CHECK(boxOf(*doc, "a").y == doctest::Approx(100));
    REQUIRE(doc->byId("span").setStyle("justify-items: center; align-items: end") == Status::kOk);
    doc->update(0.3f);
    CHECK(boxOf(*doc, "a").width == doctest::Approx(0));  // Shrink-to-fit an empty item.
    CHECK(boxOf(*doc, "a").x == doctest::Approx(175));
    CHECK(boxOf(*doc, "a").y == doctest::Approx(150));

    CHECK(doc->byId("span").setStyle("grid-template-columns: 1fr") == Status::kUnsupported);
    CHECK(doc->byId("a").setStyle("grid-area: m") == Status::kUnsupported);
    CHECK(doc->byId("a").setStyle("grid-column: -1") == Status::kInvalidArgument);
    // `display: grid` itself is a runtime value.
    REQUIRE(doc->byId("k").setStyle("display: grid") == Status::kOk);
    doc->update(0.4f);
    CHECK(doc->arena().element(doc->byId("k").node).style.display == Display::Grid);
}
