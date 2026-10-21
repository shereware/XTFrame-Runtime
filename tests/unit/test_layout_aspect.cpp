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
#include "xt/node_arena.h"
#include "xt/property.h"
#include "xt/sw/sw_png.h"
#include "xt/text_shaper.h"

/// T-328 (docs/design/06-layout.md §6.16): `aspect-ratio` in block, flex and absolute layout and on
/// replaced elements, checked against the hand-computed (Chrome-rule) snapshots next to each
/// fixture (tests/layout/aspect_ratio_*.json), plus resolveAspect() itself.

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

std::string snapshotOf(Document& doc, AssetResolver* assets) {
    LayoutEngine engine;
    engine.init(doc.ir(), assets);
    return engine.snapshot(doc.arena(), doc.root().node);
}

void checkFixture(const std::string& stem) {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, xt::sw::decodeImageBytes);
    std::unique_ptr<Document> doc = loadDoc(stem + ".xt", &shaper, &assets);
    const std::string expected = readFile(layoutPath(stem + ".json"));
    REQUIRE(!expected.empty());
    CHECK(snapshotOf(*doc, &assets) == expected);
}

}  // namespace

TEST_CASE("T-328: resolveAspect derives the auto axis once, clamps it, and transfers back once") {
    const AspectMinMax none{};
    Vec2 s = resolveAspect(160, kUndefinedRef, 16.0f / 9.0f, none);
    CHECK(s.x == doctest::Approx(160));
    CHECK(s.y == doctest::Approx(90));
    s = resolveAspect(kUndefinedRef, 50, 2, none);
    CHECK(s.x == doctest::Approx(100));
    CHECK(s.y == doctest::Approx(50));
    // Both definite, neither, or no ratio: unchanged.
    s = resolveAspect(100, 20, 2, none);
    CHECK(s.x == doctest::Approx(100));
    CHECK(s.y == doctest::Approx(20));
    s = resolveAspect(100, kUndefinedRef, 0, none);
    CHECK(s.y == doctest::Approx(kUndefinedRef));
    // A clamped derived axis leaves an author-definite axis alone ...
    AspectMinMax maxH{};
    maxH.maxH = 50;
    s = resolveAspect(400, kUndefinedRef, 2, maxH);
    CHECK(s.x == doctest::Approx(400));
    CHECK(s.y == doctest::Approx(50));
    // ... and recomputes an automatic one once, clamped to its own bounds.
    s = resolveAspect(400, kUndefinedRef, 2, maxH, true);
    CHECK(s.x == doctest::Approx(100));
    CHECK(s.y == doctest::Approx(50));
    maxH.minW = 150;
    s = resolveAspect(400, kUndefinedRef, 2, maxH, true);
    CHECK(s.x == doctest::Approx(150));
    CHECK(s.y == doctest::Approx(50));
}

TEST_CASE("T-328: block -- width given, height given, both auto, max-height transfer, both given") {
    checkFixture("aspect_ratio_block");
}

TEST_CASE("T-328: flex -- stretch wins over the ratio; a non-stretched item follows its main size") {
    checkFixture("aspect_ratio_flex");
}

TEST_CASE("T-328: absolute -- left+right gives the height, top+bottom the width, all four ignore it") {
    checkFixture("aspect_ratio_absolute");
}

TEST_CASE("T-328: img -- an explicit ratio overrides the natural one, auto keeps it") {
    checkFixture("aspect_ratio_img");
}

TEST_CASE("T-328: setStyle aspect-ratio relayouts; auto turns it off again") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, xt::sw::decodeImageBytes);
    std::unique_ptr<Document> doc = loadDoc("aspect_ratio_block.xt", &shaper, &assets);
    ElementRef el = doc->byId("b-width");
    REQUIRE(el.valid());
    CHECK(el.setStyle("aspect-ratio: 4 / 1") == Status::kOk);
    doc->update(0.0f);
    CHECK(doc->arena().element(el.node).box.height == doctest::Approx(40));
    CHECK(el.setStyle("aspect-ratio: 0") != Status::kOk);
    CHECK(el.setStyle("aspect-ratio: -2") != Status::kOk);
    CHECK(el.setStyle("aspect-ratio: auto") == Status::kOk);
    doc->update(0.0f);
    CHECK(doc->arena().element(el.node).box.height == doctest::Approx(0));
}
