#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "fake_text_shaper.h"
#include "style_decode.h"  // styleprop::* (internal; core/src is a test-only include dir).
#include "test_temp.h"
#include "text_case.h"  // T-323 (internal).
#include "xt/asset_resolver.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/layout.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/types.h"

/// Text node layout tests (docs/design/06-layout.md §6.8, docs/tasks/T-30.md "測試"), driven
/// through the real pipeline (compileDocument -> Document -> update) with FakeTextShaper's
/// monospace metrics: an ASCII glyph advances 0.6em, a space 0.3em, a CJK codepoint 1.0em;
/// ascent 0.8em / descent 0.2em; the initial line-height (1.2em) is used throughout.
///
/// Fixtures are written to a temporary directory rather than tests/layout/ because
/// docs/tasks/T-30.md's file list adds no `.xt` files there.

using namespace xt;
using namespace xt::compile;

namespace {

namespace fs = std::filesystem;

/// Writes `source` to a uniquely named `.xt` file in the system temp directory and returns its
/// path. The file is left behind deliberately (a test run writes a handful of tiny files).
std::string writeTempXt(const std::string& name, const std::string& source) {
    fs::path dir = xt::test::tempDir() / "xt_t30_layout_text";
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

/// Compiles `source`, loads it with `shaper`, sets a 400x300 viewport and runs one update(0).
std::unique_ptr<Document> loadTextDoc(const std::string& name, const std::string& source,
                                      TextShaper& shaper) {
    std::string path = writeTempXt(name, source);
    CompileOptions options;
    CompileResult result = compileDocument(path, options);
    for (const Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());

    std::unique_ptr<Document> doc;
    Status status = Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &shaper, nullptr, doc);
    REQUIRE(status == Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 300});
    doc->update(0.0f);
    return doc;
}

/// The first Text child of `element`.
const TextNodeData& textOf(Document& doc, ElementRef element) {
    NodeId found = kInvalidNode;
    doc.arena().forEachChild(element.node, [&](NodeId child) {
        if (found == kInvalidNode && doc.arena().kind(child) == NodeKind::Text) {
            found = child;
        }
    });
    REQUIRE(found != kInvalidNode);
    return doc.arena().text(found);
}

std::string document(const std::string& css, const std::string& body) {
    return "<xt>\n<head>\n<style>\n" + css + "\n</style>\n</head>\n<body>\n" + body + "\n</body>\n</xt>\n";
}

}  // namespace

TEST_CASE("layoutTextNode: greedy wrapping into two lines, height = 2 x line-height") {
    FakeTextShaper shaper;
    // "hello world hello world": 4 x (5 x 6px) + 3 x 3px = 129px > 100px, so it wraps once (the
    // task sheet's own "hello world hello" is 96px, i.e. a single line at these metrics).
    std::unique_ptr<Document> doc = loadTextDoc(
        "wrap",
        document("#a { width: 100px; font-size: 10px; }", "<div id=\"a\">hello world hello world</div>"),
        shaper);
    ElementRef a = doc->byId("a");
    REQUIRE(a.valid());
    const TextNodeData& text = textOf(*doc, a);
    CHECK(text.lines.size() == 2);
    CHECK(text.height == doctest::Approx(24.0f));
    CHECK(text.width == doctest::Approx(100.0f));
    // The block's own auto height comes from the text (§6.8 "文字高度加入 cursorY").
    CHECK(doc->arena().element(a.node).box.height == doctest::Approx(24.0f));
    // Line 1 keeps three words (96px), line 2 the last one (30px).
    CHECK(text.lines[0].rect.w == doctest::Approx(96.0f));
    CHECK(text.lines[1].rect.w == doctest::Approx(30.0f));
}

TEST_CASE("layoutTextNode: white-space: nowrap keeps one line") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc =
        loadTextDoc("nowrap",
                    document("#a { width: 100px; font-size: 10px; white-space: nowrap; }",
                             "<div id=\"a\">hello world hello world</div>"),
                    shaper);
    ElementRef a = doc->byId("a");
    REQUIRE(a.valid());
    const TextNodeData& text = textOf(*doc, a);
    CHECK(text.lines.size() == 1);
    CHECK(text.height == doctest::Approx(12.0f));
    CHECK(text.lines[0].rect.w == doctest::Approx(129.0f));
}

TEST_CASE("layoutTextNode: text-align: center centers the run in the content box") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc = loadTextDoc(
        "center",
        document("#a { width: 100px; font-size: 10px; text-align: center; }", "<div id=\"a\">hello</div>"),
        shaper);
    ElementRef a = doc->byId("a");
    REQUIRE(a.valid());
    const TextNodeData& text = textOf(*doc, a);
    REQUIRE(text.runs.size() == 1);
    // (100 - 30) / 2 = 35.
    CHECK(text.runs[0].positions[0].x == doctest::Approx(35.0f));
    CHECK(text.runs[0].bounds.x == doctest::Approx(35.0f));
    // Baseline: (12 - (8 + 2)) / 2 + 8 = 9.
    CHECK(text.runs[0].positions[0].y == doctest::Approx(9.0f));
}

TEST_CASE("layoutTextNode: shrink-to-fit absolute box takes the text's intrinsic main size") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc = loadTextDoc(
        "shrink", document("#a { position: absolute; font-size: 10px; }", "<div id=\"a\">hello world</div>"),
        shaper);
    ElementRef a = doc->byId("a");
    REQUIRE(a.valid());
    const TextNodeData& text = textOf(*doc, a);
    CHECK(text.intrinsicMain == doctest::Approx(63.0f));
    CHECK(doc->arena().element(a.node).box.width == doctest::Approx(63.0f));
    CHECK(text.lines.size() == 1);
}

TEST_CASE("layoutTextNode: a flex item's content flex basis is its text's intrinsic main size") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc =
        loadTextDoc("flexbasis",
                    document("#f { display: flex; } #i { font-size: 10px; flex-shrink: 0; }",
                             "<div id=\"f\"><div id=\"i\">hello world</div></div>"),
                    shaper);
    ElementRef item = doc->byId("i");
    REQUIRE(item.valid());
    CHECK(doc->arena().element(item.node).box.width == doctest::Approx(63.0f));
}

TEST_CASE("layoutTextNode: writing-mode: vertical-rl lays text out in columns") {
    FakeTextShaper shaper;
    // Three CJK codepoints (10px each) in a 20px-tall box: two columns, each 12px (line-height) wide.
    std::unique_ptr<Document> doc =
        loadTextDoc("vertical",
                    document("#a { width: 100px; height: 20px; font-size: 10px; writing-mode: vertical-rl; }",
                             "<div id=\"a\">\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E</div>"),
                    shaper);
    ElementRef a = doc->byId("a");
    REQUIRE(a.valid());
    const TextNodeData& text = textOf(*doc, a);
    CHECK(text.lines.size() == 2);
    CHECK(text.width == doctest::Approx(24.0f));
    CHECK(text.height == doctest::Approx(20.0f));
}

TEST_CASE("layoutTextNode: the shaping cache skips shape() on an unchanged relayout") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc = loadTextDoc(
        "cache",
        document("#a { width: 100px; font-size: 10px; }", "<div id=\"a\">hello world hello world</div>"),
        shaper);
    uint32_t callsAfterFirstLayout = shaper.shapeCalls;
    CHECK(callsAfterFirstLayout > 0);

    doc->setViewport(Rect{0, 0, 400, 300});  // Marks the whole tree kDirtyLayout, style untouched.
    doc->update(0.0f);
    CHECK(shaper.shapeCalls == callsAfterFirstLayout);

    ElementRef a = doc->byId("a");
    REQUIRE(a.valid());
    CHECK(textOf(*doc, a).lines.size() == 2);
}

TEST_CASE("Document::hitTestText: nearest grapheme boundary in the clicked line") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc = loadTextDoc(
        "hit",
        document("#a { width: 100px; font-size: 10px; }", "<div id=\"a\">hello world hello world</div>"),
        shaper);
    ElementRef a = doc->byId("a");
    REQUIRE(a.valid());
    CHECK(doc->hitTestText(a, Vec2{0, 0}) == 0);
    // Boundary after the third glyph on line 1 (x = 18) -> byte offset 3.
    CHECK(doc->hitTestText(a, Vec2{18, 2}) == 3);
    // Second line (y = 18) starts at "world" -> byte offset 18.
    CHECK(doc->hitTestText(a, Vec2{0, 18}) == 18);
}

// ---- T-323 (05 §5.9.8): `text-transform` ----------------------------------------------------

namespace {

/// The shaped text of `text` as UTF-8: FakeTextShaper's glyphId is the code point, so this is the
/// string shape() was actually given (logical order; every test text here is LTR).
std::string shapedText(const TextNodeData& text) {
    std::string out;
    for (const ShapedGlyph& glyph : text.shaped.glyphs) {
        const uint32_t cp = glyph.glyphId;
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

/// UTF-8 byte length of `cp`.
size_t utf8Length(uint32_t cp) { return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4; }

std::string caseOf(std::string_view text, TextTransform transform) {
    std::string out;
    text_case::apply(text, transform, out);
    CHECK(out.size() == text.size());
    return out;
}

}  // namespace

TEST_CASE("text_case: tables keep the UTF-8 length; sizes and sampled entries") {
    uint32_t upper = 0;
    uint32_t lower = 0;
    for (uint32_t cp = 0; cp < text_case::kTableSize; ++cp) {
        CAPTURE(cp);
        CHECK(utf8Length(text_case::toUpper(cp)) == utf8Length(cp));
        CHECK(utf8Length(text_case::toLower(cp)) == utf8Length(cp));
        upper += text_case::toUpper(cp) != cp ? 1u : 0u;
        lower += text_case::toLower(cp) != cp ? 1u : 0u;
    }
    CHECK(text_case::kTableSize == 384);
    CHECK(upper == 119);
    CHECK(lower == 118);
    CHECK(text_case::toUpper('a') == 'A');
    CHECK(text_case::toLower('Z') == 'z');
    CHECK(text_case::toUpper(0xDF) == 0xDF);    // ß has no simple upper case.
    CHECK(text_case::toUpper(0xFF) == 0x178);   // ÿ -> Ÿ
    CHECK(text_case::toLower(0x178) == 0xFF);   // Ÿ -> ÿ
    CHECK(text_case::toUpper(0xB5) == 0x39C);   // µ -> Greek capital mu (two bytes both).
    CHECK(text_case::toUpper(0xF7) == 0xF7);    // ÷
    CHECK(text_case::toLower(0xD7) == 0xD7);    // ×
    CHECK(text_case::toUpper(0x101) == 0x100);  // ā -> Ā
    CHECK(text_case::toLower(0x141) == 0x142);  // Ł -> ł
    CHECK(text_case::toUpper(0x17E) == 0x17D);  // ž -> Ž
    CHECK(text_case::toUpper(0x138) == 0x138);  // ĸ: no upper case.
    CHECK(text_case::toUpper(0x149) == 0x149);  // ŉ: only a full mapping.
    // Left out because they would change the UTF-8 length.
    CHECK(text_case::toLower(0x130) == 0x130);  // İ (-> i)
    CHECK(text_case::toUpper(0x131) == 0x131);  // ı (-> I)
    CHECK(text_case::toUpper(0x17F) == 0x17F);  // ſ (-> S)
    // Outside the tables: unchanged.
    CHECK(text_case::toUpper(0x3C9) == 0x3C9);    // ω
    CHECK(text_case::toUpper(0x434) == 0x434);    // д
    CHECK(text_case::toLower(0x1E9E) == 0x1E9E);  // ẞ
}

TEST_CASE("text_case: apply() on mixed and malformed UTF-8") {
    // Latin-1 / Latin Extended-A in, Greek / Cyrillic / CJK / emoji untouched.
    const std::string mixed =
        "stra\xC3\x9F\x65 \xC3\xBF \xC3\xA9 \xC4\x81 \xC5\x91 \xCF\x89 \xD0\xB4 \xE6\x97\xA5 "
        "\xF0\x9F\x98\x80";
    CHECK(caseOf(mixed, TextTransform::Uppercase) ==
          "STRA\xC3\x9F\x45 \xC5\xB8 \xC3\x89 \xC4\x80 \xC5\x90 \xCF\x89 \xD0\xB4 \xE6\x97\xA5 "
          "\xF0\x9F\x98\x80");
    CHECK(caseOf("\xC3\x80\xC3\x89 \xC5\xB8 \xC5\x81 \xC4\xB0 \xC3\x97 ABC", TextTransform::Lowercase) ==
          "\xC3\xA0\xC3\xA9 \xC3\xBF \xC5\x82 \xC4\xB0 \xC3\x97 abc");
    CHECK(caseOf("Ab", TextTransform::None) == "Ab");
    // Malformed bytes are copied as they are, and the letters around them still map.
    CHECK(caseOf("a\xC3", TextTransform::Uppercase) == "A\xC3");
    CHECK(caseOf("a\x80z\xFF", TextTransform::Uppercase) == "A\x80Z\xFF");
    CHECK(caseOf("\xC0\x81x", TextTransform::Uppercase) == "\xC0\x81X");  // Overlong: not U+0041.
}

TEST_CASE("text_case: capitalize starts a word after white space or ASCII punctuation only") {
    CHECK(caseOf("hello wORLD", TextTransform::Capitalize) == "Hello WORLD");  // The rest is kept.
    CHECK(caseOf("foo-bar (x) a.b o'neil", TextTransform::Capitalize) == "Foo-Bar (X) A.B O'Neil");
    CHECK(caseOf("3rd \xC3\xB1u", TextTransform::Capitalize) == "3rd \xC3\x91u");  // A digit is a word start.
    // U+00A0 and U+3000 are white space; a CJK character or a Latin-1 letter is not a separator.
    CHECK(caseOf("a\xC2\xA0\x62 \xE3\x80\x80z \xE6\x97\xA5x \xC3\xA9x", TextTransform::Capitalize) ==
          "A\xC2\xA0\x42 \xE3\x80\x80Z \xE6\x97\xA5x \xC3\x89x");
    CHECK(caseOf("\ttab\nline", TextTransform::Capitalize) == "\tTab\nLine");
}

TEST_CASE("layoutTextNode: text-transform shapes the converted text, all four values") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc = loadTextDoc(
        "text_transform_values",
        document("div { font-size: 10px; } #n { text-transform: none; } #u { text-transform: uppercase; } "
                 "#l { text-transform: lowercase; } #c { text-transform: capitalize; }",
                 "<div id=\"n\">Abc d\xC3\xA9</div><div id=\"u\">abc d\xC3\xA9 \xC3\xBF \xCF\x89</div>"
                 "<div id=\"l\">ABC D\xC3\x89 \xC5\x81</div><div id=\"c\">abc d\xC3\xA9\x66-gh</div>"),
        shaper);
    CHECK(shapedText(textOf(*doc, doc->byId("n"))) == "Abc d\xC3\xA9");
    CHECK(shapedText(textOf(*doc, doc->byId("u"))) == "ABC D\xC3\x89 \xC5\xB8 \xCF\x89");
    CHECK(shapedText(textOf(*doc, doc->byId("l"))) == "abc d\xC3\xA9 \xC5\x82");
    CHECK(shapedText(textOf(*doc, doc->byId("c"))) == "Abc D\xC3\xA9\x66-Gh");
    // The source text is untouched: only what is shaped changes.
    CHECK(doc->byId("u").text() == "abc d\xC3\xA9 \xC3\xBF \xCF\x89");
    CHECK(doc->byId("u").computedStyleValue(style_decode::styleprop::TextTransform).e == 1u);
}

TEST_CASE("layoutTextNode: text-transform is inherited and `none` switches it off again") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc = loadTextDoc(
        "text_transform_inherit",
        document(
            ".u { text-transform: uppercase; } #r { text-transform: none; }",
            "<div class=\"u\"><div id=\"a\">abc</div><div id=\"r\">abc</div></div><div id=\"b\">abc</div>"),
        shaper);
    CHECK(shapedText(textOf(*doc, doc->byId("a"))) == "ABC");
    CHECK(doc->byId("a").computedStyleValue(style_decode::styleprop::TextTransform).e == 1u);
    CHECK(shapedText(textOf(*doc, doc->byId("r"))) == "abc");
    CHECK(shapedText(textOf(*doc, doc->byId("b"))) == "abc");
    CHECK(doc->byId("b").computedStyleValue(style_decode::styleprop::TextTransform).e == 0u);
}

TEST_CASE("layoutTextNode: switching a class reshapes once; the transform is part of the cache key") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc = loadTextDoc(
        "text_transform_cache",
        document(
            "#a { font-size: 10px; } .u { text-transform: uppercase; } .l { text-transform: lowercase; }",
            "<div id=\"a\">Abc</div>"),
        shaper);
    ElementRef a = doc->byId("a");
    REQUIRE(a.valid());
    CHECK(shapedText(textOf(*doc, a)) == "Abc");
    CHECK(textOf(*doc, a).keyTransform == TextTransform::None);

    const uint32_t before = shaper.shapeCalls;
    a.addClass("u");
    doc->update(0.0f);
    CHECK(shaper.shapeCalls > before);
    CHECK(shapedText(textOf(*doc, a)) == "ABC");
    CHECK(textOf(*doc, a).keyTransform == TextTransform::Uppercase);

    // An unchanged relayout keeps the cache entry.
    const uint32_t afterSwitch = shaper.shapeCalls;
    doc->setViewport(Rect{0, 0, 400, 300});
    doc->update(0.0f);
    CHECK(shaper.shapeCalls == afterSwitch);
    CHECK(shapedText(textOf(*doc, a)) == "ABC");

    // Only the transform changes (same text, font, size...): the key still misses.
    a.removeClass("u");
    a.addClass("l");
    doc->update(0.0f);
    CHECK(shaper.shapeCalls > afterSwitch);
    CHECK(shapedText(textOf(*doc, a)) == "abc");
    a.removeClass("l");
    doc->update(0.0f);
    CHECK(shapedText(textOf(*doc, a)) == "Abc");
}

TEST_CASE("layoutTextNode: an editable input's value is never transformed") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc =
        loadTextDoc("text_transform_input",
                    document(".u { text-transform: uppercase; } input { width: 200px; height: 20px; }",
                             "<div class=\"u\"><input id=\"i\"/></div>"),
                    shaper);
    ElementRef input = doc->byId("i");
    REQUIRE(input.valid());
    REQUIRE(input.setAttribute("value", "abc \xC3\xA9") == Status::kOk);
    doc->update(0.0f);
    // Inherited all the same; the input skips it, so the value is shaped (and edited) as typed.
    CHECK(input.computedStyleValue(style_decode::styleprop::TextTransform).e == 1u);
    CHECK(shapedText(textOf(*doc, input)) == "abc \xC3\xA9");
    CHECK(input.text() == "abc \xC3\xA9");
}

TEST_CASE("layoutTextNode: rich text is transformed as one string; run offsets do not move") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc = loadTextDoc("text_transform_rich",
                                                document("p { font-size: 10px; text-transform: capitalize; }",
                                                         "<p id=\"p\">ab <b>cd\xC3\xA9</b> ef</p>"),
                                                shaper);
    const TextNodeData& text = textOf(*doc, doc->byId("p"));
    REQUIRE(!text.styleRuns.empty());
    // "ab cdé ef": the bold run "cdé" sits at bytes [3, 7) before and after the conversion.
    CHECK(text.styleRuns[0].start == 3);
    CHECK(text.styleRuns[0].end == 7);
    CHECK(shapedText(text) == "Ab Cd\xC3\xA9 Ef");
    CHECK(text.shaped.breakAfter.size() == std::string_view("ab cd\xC3\xA9 ef").size());
}

// T-331 (05 §5.9.8): line-clamp and font-variant-numeric through the real pipeline.
namespace {

/// The glyph ids of a laid-out text node, every run in order.
std::vector<uint32_t> laidOutGlyphs(const TextNodeData& text) {
    std::vector<uint32_t> glyphs;
    for (const GlyphRun& run : text.runs) {
        glyphs.insert(glyphs.end(), run.glyphs.begin(), run.glyphs.end());
    }
    return glyphs;
}

}  // namespace

TEST_CASE("layoutTextNode: line-clamp keeps n lines, ends in an ellipsis and is not inherited") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc = loadTextDoc(
        "line_clamp_basic",
        document(
            "div { font-size: 10px; width: 42px; } #c2 { line-clamp: 2; } #c9 { -webkit-line-clamp: 9; } "
            "#p { line-clamp: 1; }",
            "<div id=\"c2\">aaa bbb ccc ddd eee</div><div id=\"c9\">aaa bbb ccc ddd eee</div>"
            "<div id=\"p\"><div id=\"k\">aaa bbb ccc ddd eee</div></div>"),
        shaper);
    const TextNodeData& two = textOf(*doc, doc->byId("c2"));
    REQUIRE(two.lines.size() == 2);
    CHECK(two.height == doctest::Approx(24.0f));  // 2 x line-height (1.2em).
    CHECK(laidOutGlyphs(two).back() == 0x2026u);
    CHECK(doc->byId("c2").computedStyleValue(style_decode::styleprop::LineClamp).e == 2u);

    const TextNodeData& nine = textOf(*doc, doc->byId("c9"));  // The alias; more than the 3 lines.
    CHECK(nine.lines.size() == 3);
    for (uint32_t g : laidOutGlyphs(nine)) {
        CHECK(g != 0x2026u);
    }

    CHECK(doc->byId("k").computedStyleValue(style_decode::styleprop::LineClamp).e == 0u);
    CHECK(textOf(*doc, doc->byId("k")).lines.size() == 3);
}

TEST_CASE("layoutTextNode: nowrap + text-overflow: ellipsis wins over line-clamp") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc = loadTextDoc(
        "line_clamp_nowrap",
        document("div { font-size: 10px; width: 42px; white-space: nowrap; line-clamp: 3; } "
                 "#e { text-overflow: ellipsis; }",
                 "<div id=\"e\">aaa bbb ccc ddd eee</div><div id=\"c\">aaa bbb ccc ddd eee</div>"),
        shaper);
    const TextNodeData& ellipsis = textOf(*doc, doc->byId("e"));
    REQUIRE(ellipsis.lines.size() == 1);
    CHECK(laidOutGlyphs(ellipsis).back() == 0x2026u);
    CHECK(ellipsis.lines[0].rect.w <= 42.0f);
    // `nowrap` without the ellipsis: one line, clipped, the clamp has nothing to do.
    const TextNodeData& clipped = textOf(*doc, doc->byId("c"));
    REQUIRE(clipped.lines.size() == 1);
    for (uint32_t g : laidOutGlyphs(clipped)) {
        CHECK(g != 0x2026u);
    }
}

TEST_CASE("layoutTextNode: line-clamp switched at run time relayouts without reshaping") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc =
        loadTextDoc("line_clamp_switch",
                    document("#a { font-size: 10px; width: 42px; } .one { line-clamp: 1; }",
                             "<div id=\"a\">aaa bbb ccc ddd eee</div>"),
                    shaper);
    ElementRef a = doc->byId("a");
    REQUIRE(a.valid());
    CHECK(textOf(*doc, a).lines.size() == 3);
    const uint32_t before = shaper.shapeCalls;
    a.addClass("one");
    doc->update(0.0f);
    CHECK(textOf(*doc, a).lines.size() == 1);
    CHECK(shaper.shapeCalls == before);  // Only the line count changed: layoutText() alone.
    a.removeClass("one");
    doc->update(0.0f);
    CHECK(textOf(*doc, a).lines.size() == 3);
}

TEST_CASE("layoutTextNode: font-variant-numeric reaches ShapeParams::features and is inherited") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc = loadTextDoc(
        "font_variant_numeric",
        document(
            ".t { font-variant-numeric: tabular-nums; } #r { font-variant-numeric: normal; }",
            "<div class=\"t\"><div id=\"b\">123</div><div id=\"r\">456</div></div><div id=\"z\">789</div>"),
        shaper);
    CHECK(textOf(*doc, doc->byId("b")).keyFeatures == kShapeFeatureTnum);
    CHECK(doc->byId("b").computedStyleValue(style_decode::styleprop::FontVariantNumeric).e == 1u);
    CHECK(textOf(*doc, doc->byId("r")).keyFeatures == 0);
    CHECK(textOf(*doc, doc->byId("z")).keyFeatures == 0);
    CHECK(doc->byId("z").computedStyleValue(style_decode::styleprop::FontVariantNumeric).e == 0u);
    bool sawTnum = false;
    for (uint8_t features : shaper.featuresSeen) {
        sawTnum = sawTnum || features == kShapeFeatureTnum;
    }
    CHECK(sawTnum);
}

TEST_CASE("layoutTextNode: the features are part of the shaping cache key") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc =
        loadTextDoc("font_variant_numeric_cache",
                    document("#a { font-size: 10px; } .t { font-variant-numeric: tabular-nums; }",
                             "<div id=\"a\">1234</div>"),
                    shaper);
    ElementRef a = doc->byId("a");
    REQUIRE(a.valid());
    CHECK(textOf(*doc, a).keyFeatures == 0);

    const uint32_t before = shaper.shapeCalls;
    a.addClass("t");
    doc->update(0.0f);
    CHECK(shaper.shapeCalls > before);
    CHECK(shaper.featuresSeen.back() == kShapeFeatureTnum);
    CHECK(textOf(*doc, a).keyFeatures == kShapeFeatureTnum);

    const uint32_t afterSwitch = shaper.shapeCalls;
    doc->setViewport(Rect{0, 0, 400, 300});
    doc->update(0.0f);
    CHECK(shaper.shapeCalls == afterSwitch);  // Unchanged: the cache entry holds.

    a.removeClass("t");
    doc->update(0.0f);
    CHECK(shaper.shapeCalls > afterSwitch);
    CHECK(textOf(*doc, a).keyFeatures == 0);
}

TEST_CASE("layoutTextNode: a rich-text run overrides font-variant-numeric") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc =
        loadTextDoc("font_variant_numeric_run",
                    document("p { font-size: 10px; } .t { font-variant-numeric: tabular-nums; }",
                             "<p id=\"p\">ab <span class=\"t\">12</span> cd</p>"),
                    shaper);
    const TextNodeData& text = textOf(*doc, doc->byId("p"));
    REQUIRE(!text.styleRuns.empty());
    CHECK(text.keyFeatures == 0);  // The element's own text keeps the font's defaults...
    bool sawTnum = false;
    bool sawNone = false;
    for (uint8_t features : shaper.featuresSeen) {
        sawTnum = sawTnum || features == kShapeFeatureTnum;
        sawNone = sawNone || features == 0;
    }
    CHECK(sawTnum);  // ...and the run is shaped with `tnum`.
    CHECK(sawNone);
}
