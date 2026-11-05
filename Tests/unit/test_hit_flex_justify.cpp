#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "fake_text_shaper.h"
#include "test_temp.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/element_ref.h"
#include "xt/node_arena.h"
#include "xt/types.h"

/// T-622: the hit test must use the same final box the Painter draws -- a flex row's
/// `justify-content` offset and the items' padding included. FakeTextShaper metrics: an ASCII
/// glyph advances 0.6em, so "TAB" at font-size 10px is 18px wide; with `padding: 0 18.5px` an
/// auto-width tab is 55px wide (the XTAtrium report's 964-1019 / 1024-1079 / 1084-1139).

using namespace xt;
using namespace xt::compile;

namespace {

namespace fs = std::filesystem;

std::string writeTempXt(const std::string& name, const std::string& source) {
    fs::path dir = xt::test::tempDir() / "xt_t622_hit_flex";
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

/// A 240px flex row at x = 899 holding three tabs (#t0..#t2), 5px apart, laid out in a 1280x720
/// viewport. `justify` is the row's justify-content; `tabCss` the tabs' extra declarations.
std::unique_ptr<Document> loadRow(const std::string& name, const std::string& justify,
                                  const std::string& tabCss, TextShaper& shaper) {
    const std::string source =
        "<xt>\n<head>\n<style>\n"
        "#row { display: flex; flex-direction: row; justify-content: " +
        justify +
        "; width: 240px; height: 30px; margin-left: 899px; column-gap: 5px; }\n"
        ".tab { font-size: 10px; padding: 0 18.5px; flex-shrink: 0; " +
        tabCss +
        " }\n"
        "</style>\n</head>\n<body>\n"
        "<div id=\"row\"><div id=\"t0\" class=\"tab\">TAB</div><div id=\"t1\" class=\"tab\">TAB</div>"
        "<div id=\"t2\" class=\"tab\">TAB</div></div>\n"
        "</body>\n</xt>\n";
    const std::string path = writeTempXt(name, source);
    CompileOptions options;
    CompileResult result = compileDocument(path, options);
    for (const Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    std::unique_ptr<Document> doc;
    const Status status =
        Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &shaper, nullptr, doc);
    REQUIRE(status == Status::kOk);
    doc->setViewport(Rect{0, 0, 1280, 720});
    doc->update(0.0f);
    return doc;
}

/// Checks, for every tab, that the painted rectangle (boundingBox: the border box through the
/// world transform the Painter draws with) is `expectedX[i]`..+`expectedW`, and that hitTest()
/// returns that tab just inside both edges and not just outside them (between tabs: the row).
void checkTabs(Document& doc, const std::vector<float>& expectedX, float expectedW) {
    const ElementRef row = doc.byId("row");
    REQUIRE(row.valid());
    for (size_t i = 0; i < expectedX.size(); ++i) {
        const ElementRef tab = doc.byId("t" + std::to_string(i));
        REQUIRE(tab.valid());
        const Rect painted = tab.boundingBox();
        CAPTURE(i);
        CAPTURE(painted.x);
        CAPTURE(painted.w);
        CHECK(painted.x == doctest::Approx(expectedX[i]));
        CHECK(painted.w == doctest::Approx(expectedW));
        const float y = painted.y + painted.h * 0.5f;
        CHECK(doc.hitTest(Vec2{painted.x + 0.5f, y}) == tab.node);
        CHECK(doc.hitTest(Vec2{painted.x + painted.w * 0.5f, y}) == tab.node);
        CHECK(doc.hitTest(Vec2{painted.x + painted.w - 0.5f, y}) == tab.node);
        CHECK(doc.hitTest(Vec2{painted.x - 0.5f, y}) != tab.node);
        CHECK(doc.hitTest(Vec2{painted.x + painted.w + 0.5f, y}) != tab.node);
    }
}

}  // namespace

TEST_CASE("T-622 hit test: flex-end auto-width tabs hit where they are painted") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc = loadRow("flex_end", "flex-end", "", shaper);
    // Free space 240 - (3 x 55 + 2 x 5) = 65: the report's 964 / 1024 / 1084.
    checkTabs(*doc, {964.0f, 1024.0f, 1084.0f}, 55.0f);
    const ElementRef row = doc->byId("row");
    CHECK(doc->hitTest(Vec2{950.0f, 15.0f}) == row.node);   // Left of the first tab: the row.
    CHECK(doc->hitTest(Vec2{1021.0f, 15.0f}) == row.node);  // The gap between t0 and t1.
}

TEST_CASE("T-622 hit test: flex-start auto-width tabs") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc = loadRow("flex_start", "flex-start", "", shaper);
    checkTabs(*doc, {899.0f, 959.0f, 1019.0f}, 55.0f);
}

TEST_CASE("T-622 hit test: center auto-width tabs") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc = loadRow("center", "center", "", shaper);
    checkTabs(*doc, {931.5f, 991.5f, 1051.5f}, 55.0f);
}

TEST_CASE("T-622 hit test: space-between auto-width tabs") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc = loadRow("space_between", "space-between", "", shaper);
    // Free space 65 split into two 32.5px extra gaps on top of the 5px column-gap.
    checkTabs(*doc, {899.0f, 991.5f, 1084.0f}, 55.0f);
}

TEST_CASE("T-622 hit test: fixed-width flex-end tabs (control)") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc =
        loadRow("fixed", "flex-end", "width: 50px; box-sizing: border-box;", shaper);
    // 240 - (3 x 50 + 2 x 5) = 80.
    checkTabs(*doc, {979.0f, 1034.0f, 1089.0f}, 50.0f);
}
