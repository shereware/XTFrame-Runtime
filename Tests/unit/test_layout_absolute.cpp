#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "doctest.h"
#include "fake_text_shaper.h"
#include "test_temp.h"
#include "xt/asset_resolver.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/layout.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/sw/sw_png.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// LayoutEngine absolute-positioning tests (Docs/design/06-layout.md §6.6): the four
/// left/top/right/bottom combinations, and a containing block that is an ancestor (not the DOM
/// parent). See Docs/tasks/T-25.md "測試".

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

namespace fs = std::filesystem;

/// T-89's text-child case: writes `source` to a temp `.xt` file and loads it with `shaper` -- like
/// test_layout_text_node.cpp's writeTempXt/loadTextDoc, since Docs/tasks/T-89.md's file list adds
/// no `.xt` file to Tests/layout/ for this one FakeTextShaper-only case (every other T-89 case is
/// NullTextShaper-driven and lives in absolute_flex_auto.xt above).
std::string writeTempXt(const std::string& name, const std::string& source) {
    fs::path dir = xt::test::tempDir() / "xt_t89_layout_absolute";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path path = dir / (name + ".xt");
    std::FILE* file = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&file, path.string().c_str(), "wb") != 0) {
        file = nullptr;
    }
#else
    file = std::fopen(path.string().c_str(), "wb");
#endif
    REQUIRE(file != nullptr);
    std::fwrite(source.data(), 1, source.size(), file);
    std::fclose(file);
    return path.string();
}

std::unique_ptr<Document> loadTempLayoutDoc(const std::string& name, const std::string& source,
                                            TextShaper* shaper, AssetResolver* assets) {
    std::string path = writeTempXt(name, source);
    CompileOptions options;
    CompileResult result = compileDocument(path, options);
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

TEST_CASE(
    "LayoutEngine: absolute_edges -- left/top, right/bottom, left+right, and the static-position fallback") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, xt::sw::decodeImageBytes);
    std::unique_ptr<Document> doc = loadLayoutDoc("absolute_edges.xt", &shaper, &assets);

    // #parent: position: relative, width 300, height 200, no padding/border -> its own padding box
    // is the containing block, cbW = 300, cbH = 200.
    const LayoutBox& c1 = doc->arena().element(doc->byId("c1").node).box;
    CHECK(c1.x == doctest::Approx(10));
    CHECK(c1.y == doctest::Approx(10));
    CHECK(c1.width == doctest::Approx(50));
    CHECK(c1.height == doctest::Approx(20));

    const LayoutBox& c2 = doc->arena().element(doc->byId("c2").node).box;
    CHECK(c2.x == doctest::Approx(250));  // 300 - 10 - 40
    CHECK(c2.y == doctest::Approx(175));  // 200 - 10 - 15
    CHECK(c2.width == doctest::Approx(40));
    CHECK(c2.height == doctest::Approx(15));

    const LayoutBox& c3 = doc->arena().element(doc->byId("c3").node).box;
    CHECK(c3.x == doctest::Approx(20));
    CHECK(c3.width == doctest::Approx(260));  // 300 - 20 - 20, widened by left+right
    CHECK(c3.y == doctest::Approx(5));
    CHECK(c3.height == doctest::Approx(10));

    const LayoutBox& c4 = doc->arena().element(doc->byId("c4").node).box;
    CHECK(c4.x == doctest::Approx(0));  // no left/right -> static-position fallback (0)
    CHECK(c4.width == doctest::Approx(80));
    CHECK(c4.y == doctest::Approx(0));
}

// T-74 part B (Docs/design/06-layout.md §6.6): an auto height follows the content (and `bottom`
// positions the box with that height), and an auto width shrink-wraps the widest in-flow child.
TEST_CASE("LayoutEngine: absolute_auto_size -- auto height/width follow the content") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, xt::sw::decodeImageBytes);
    std::unique_ptr<Document> doc = loadLayoutDoc("absolute_auto_size.xt", &shaper, &assets);

    // #cb: position: relative, 300x200 -> cbW = 300, cbH = 200.
    // #a: left/top, width 100, padding 10, two 20px-high children -> height 20 + 20 + 10 + 10.
    const LayoutBox& a = doc->arena().element(doc->byId("a").node).box;
    CHECK(a.x == doctest::Approx(10));
    CHECK(a.y == doctest::Approx(10));
    CHECK(a.width == doctest::Approx(100));
    CHECK(a.height == doctest::Approx(60));

    // #b: right/bottom, no width, padding 10, one 120x20 child -> 140x40, with its bottom-right
    // corner 20/30 from the containing block's.
    const LayoutBox& b = doc->arena().element(doc->byId("b").node).box;
    CHECK(b.width == doctest::Approx(140));
    CHECK(b.height == doctest::Approx(40));
    CHECK(b.x == doctest::Approx(140));  // 300 - 20 - 140
    CHECK(b.y == doctest::Approx(130));  // 200 - 30 - 40
}

TEST_CASE("LayoutEngine: absolute_nested -- the containing block is an ancestor, not the DOM parent") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, xt::sw::decodeImageBytes);
    std::unique_ptr<Document> doc = loadLayoutDoc("absolute_nested.xt", &shaper, &assets);

    // #outer: position: relative, 300x200, padding 10 (border 0) -> padding box == border box
    // (300x200) since border is 0; #middle: static, padding 5, DOM parent of #inner.
    // #inner: left: 0, top: 0 -> absolute position (0, 0) within #outer's padding box. #outer's own
    // border-box origin is (0, 0) (it's body's only child, no margin); #middle's content box starts
    // at #outer's padding(10) + #middle's own padding(5) = (15, 15) from #outer's border-box
    // origin. #inner.box.x/y (relative to #middle's content box) = 0 - 15 = -15.
    const LayoutBox& inner = doc->arena().element(doc->byId("inner").node).box;
    CHECK(inner.x == doctest::Approx(-15));
    CHECK(inner.y == doctest::Approx(-15));
    CHECK(inner.width == doctest::Approx(20));
    CHECK(inner.height == doctest::Approx(20));
}

// T-79 item 2 (Docs/design/06-layout.md §6.6/§6.9): an absolutely positioned `display: flex` box
// whose width is already determined used to fall through to the block-stacking tail, so its
// children stacked vertically instead of flex-arranging.
TEST_CASE("LayoutEngine: absolute_flex -- a determined-width absolute flex container runs flex") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, xt::sw::decodeImageBytes);
    std::unique_ptr<Document> doc = loadLayoutDoc("absolute_flex.xt", &shaper, &assets);

    // #panel: explicit width 256, three 64px items with gap 16 -> laid out in a row at
    // 0 / 80 / 160, and an auto height following the (cross-axis) item height.
    const LayoutBox& panel = doc->arena().element(doc->byId("panel").node).box;
    CHECK(panel.x == doctest::Approx(20));
    CHECK(panel.y == doctest::Approx(10));
    CHECK(panel.width == doctest::Approx(256));
    CHECK(panel.height == doctest::Approx(32));
    const LayoutBox& i1 = doc->arena().element(doc->byId("i1").node).box;
    const LayoutBox& i2 = doc->arena().element(doc->byId("i2").node).box;
    const LayoutBox& i3 = doc->arena().element(doc->byId("i3").node).box;
    CHECK(i1.x == doctest::Approx(0));
    CHECK(i2.x == doctest::Approx(80));
    CHECK(i3.x == doctest::Approx(160));
    CHECK(i1.y == doctest::Approx(0));
    CHECK(i2.y == doctest::Approx(0));
    CHECK(i3.y == doctest::Approx(0));

    // #col: flex-direction: column with a definite 100x90 box -- the main axis is the height, so
    // the two 20px items stack with the 10px gap between them.
    const LayoutBox& c1 = doc->arena().element(doc->byId("c1").node).box;
    const LayoutBox& c2 = doc->arena().element(doc->byId("c2").node).box;
    CHECK(c1.x == doctest::Approx(0));
    CHECK(c1.y == doctest::Approx(0));
    CHECK(c2.x == doctest::Approx(0));
    CHECK(c2.y == doctest::Approx(30));

    // #stretch: width comes from left + right (300), height stays auto -> the row's cross size.
    const LayoutBox& stretch = doc->arena().element(doc->byId("stretch").node).box;
    CHECK(stretch.width == doctest::Approx(300));
    CHECK(stretch.height == doctest::Approx(25));
    const LayoutBox& s2 = doc->arena().element(doc->byId("s2").node).box;
    CHECK(s2.x == doctest::Approx(38));
    CHECK(s2.y == doctest::Approx(0));
}

// T-89 (Docs/design/06-layout.md §6.6's flex-intrinsic-width rule): an absolutely positioned
// `display: flex` container with `width: auto` shrink-to-fits its main axis to its children
// instead of collapsing to 0 -- the bug HUD's `#skills` (T-88) hit because it has `left` set and
// `right: auto`, which the old, narrower special case (only left+right both auto) did not cover.
TEST_CASE(
    "LayoutEngine: absolute_flex_auto -- an auto-width absolute flex container shrink-wraps "
    "its children (row, padding, column, `left`-only positioning)") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, xt::sw::decodeImageBytes);
    std::unique_ptr<Document> doc = loadLayoutDoc("absolute_flex_auto.xt", &shaper, &assets);

    // #row: three 64x64 items, gap 16, no width/height -> 64*3 + 16*2 = 224 wide, 64 tall; items
    // keep their own 64px width (not shrunk).
    const LayoutBox& row = doc->arena().element(doc->byId("row").node).box;
    CHECK(row.width == doctest::Approx(224));
    CHECK(row.height == doctest::Approx(64));
    CHECK(doc->arena().element(doc->byId("ra").node).box.width == doctest::Approx(64));
    CHECK(doc->arena().element(doc->byId("rb").node).box.width == doctest::Approx(64));
    CHECK(doc->arena().element(doc->byId("rc").node).box.width == doctest::Approx(64));
    CHECK(doc->arena().element(doc->byId("rb").node).box.x == doctest::Approx(80));
    CHECK(doc->arena().element(doc->byId("rc").node).box.x == doctest::Approx(160));

    // #rowLeftOnly: T-89's actual regression -- `left` set, `right` auto (so left+right are *not*
    // both auto), yet width is still auto and must still shrink-wrap via the flex algorithm rather
    // than the block-stacking shrink-to-fit fallback (which would ignore flex-shrink and stack the
    // 64px items as blocks).
    const LayoutBox& rowLeftOnly = doc->arena().element(doc->byId("rowLeftOnly").node).box;
    CHECK(rowLeftOnly.x == doctest::Approx(8));
    CHECK(rowLeftOnly.width == doctest::Approx(224));
    CHECK(rowLeftOnly.height == doctest::Approx(64));
    CHECK(doc->arena().element(doc->byId("lc").node).box.x == doctest::Approx(160));

    // #rowPad: same three items, gap 16, padding 8 -> 224 + 8*2 = 240 wide.
    const LayoutBox& rowPad = doc->arena().element(doc->byId("rowPad").node).box;
    CHECK(rowPad.width == doctest::Approx(240));

    // #col: flex-direction: column, same three 64x64 items, gap 16 -> 64 wide (max outer cross),
    // 64*3 + 16*2 = 224 tall (main-axis sum).
    const LayoutBox& col = doc->arena().element(doc->byId("col").node).box;
    CHECK(col.width == doctest::Approx(64));
    CHECK(col.height == doctest::Approx(224));

    // #overflow: no positioned ancestor -> cbW = the 400px viewport. Three 150px items + gap 10*2
    // sum to 470, which overflows cbW, so the measured width is clamped to 400 and flex-shrink
    // (default shrink: 1, equal basis) then compresses each item to (400 - 20) / 3 = 126.667px.
    const LayoutBox& overflow = doc->arena().element(doc->byId("overflow").node).box;
    CHECK(overflow.width == doctest::Approx(400));
    const LayoutBox& oa = doc->arena().element(doc->byId("oa").node).box;
    const LayoutBox& ob = doc->arena().element(doc->byId("ob").node).box;
    const LayoutBox& oc = doc->arena().element(doc->byId("oc").node).box;
    CHECK(oa.width == doctest::Approx(126.6667f));
    CHECK(ob.width == doctest::Approx(126.6667f));
    CHECK(oc.width == doctest::Approx(126.6667f));
    CHECK(oa.x == doctest::Approx(0));
    CHECK(ob.x == doctest::Approx(oa.width + 10));
    CHECK(oc.x == doctest::Approx(ob.x + ob.width + 10));
}

// T-89: a flex item that is a single Text child (no width of its own) -- the container's
// shrink-to-fit width comes from the text's unwrapped main-axis advance (Docs/design/06-layout.md
// §6.8), verified with FakeTextShaper's fixed 0.6em-per-ASCII-glyph advance (`.xt` disallows
// mixing text and element children, so this is the one-item case of the §6.6 formula's "gap
// total" term, which is 0 for a single item).
TEST_CASE(
    "LayoutEngine: absolute_flex_auto_text -- an auto-width flex container shrink-wraps a "
    "text child") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc = loadTempLayoutDoc(
        "absolute_flex_auto_text",
        "<xt>\n<head>\n<style>\n"
        "#label { position: absolute; left: 4px; top: 4px; display: flex; font-size: 16px; }\n"
        "</style>\n</head>\n<body>\n<div id=\"label\">Hello</div>\n</body>\n</xt>\n",
        &shaper, nullptr);

    // "Hello" is 5 ASCII glyphs @ 0.6 * 16px = 9.6px each -> 48px unwrapped.
    const LayoutBox& label = doc->arena().element(doc->byId("label").node).box;
    CHECK(label.width == doctest::Approx(48.0f));
}
