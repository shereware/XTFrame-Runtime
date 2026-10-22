#include <string>
#include <vector>

#include "doctest.h"
#include "fake_text_shaper.h"
#include "xt/text_layout.h"
#include "xt/text_shaper.h"

/// Tests for layoutText()/bidiReorder() (Docs/design/06-layout.md §6.8, Docs/tasks/T-28.md),
/// driven entirely by FakeTextShaper (Tests/unit/fake_text_shaper.h) -- no real font library
/// involved.

using namespace xt;

namespace {

constexpr float kSize = 10.0f;
constexpr float kAscent = 8.0f;   // FakeTextShaper metrics().ascent (0.8) * kSize
constexpr float kDescent = 2.0f;  // FakeTextShaper metrics().descent (0.2) * kSize
constexpr float kLineHeight = 14.0f;

ShapeParams makeShapeParams(TextDirection dir = TextDirection::Ltr,
                            WritingMode wm = WritingMode::HorizontalTb) {
    ShapeParams p;
    p.size = kSize;
    p.paragraphDirection = dir;
    p.writingMode = wm;
    return p;
}

TextLayoutParams makeLayoutParams(float availMain, TextDirection dir = TextDirection::Ltr,
                                  WritingMode wm = WritingMode::HorizontalTb) {
    TextLayoutParams p;
    p.availMain = availMain;
    p.fontSize = kSize;
    p.lineHeight = kLineHeight;
    p.ascent = kAscent;
    p.descent = kDescent;
    p.direction = dir;
    p.writingMode = wm;
    return p;
}

}  // namespace

TEST_CASE("layoutText: single line ASCII, one run") {
    FakeTextShaper shaper;
    ShapeResult sr;
    shaper.shape("hello world", makeShapeParams(), sr);
    REQUIRE(sr.runs.size() == 1);

    TextLayoutResult out;
    layoutText(sr, makeLayoutParams(1000.0f), out);

    REQUIRE(out.lines.size() == 1);
    REQUIRE(out.lines[0].runCount == 1);
    CHECK(out.runs.size() == 1);
    // 10 ASCII letters * (10*0.6) + 1 space * (10*0.3) = 60 + 3 = 63.
    CHECK(out.mainSize == doctest::Approx(63.0f));
    CHECK(out.lines[0].baseline == doctest::Approx(0 + (kLineHeight - (kAscent + kDescent)) / 2 + kAscent));
}

TEST_CASE("layoutText: wraps at a space, trailing space excluded from line length") {
    FakeTextShaper shaper;
    ShapeResult sr;
    shaper.shape("hello world", makeShapeParams(), sr);

    TextLayoutResult out;
    // "hello " (6*5 + 3 = 33) just barely fits; "world" (30) doesn't.
    layoutText(sr, makeLayoutParams(33.0f), out);

    REQUIRE(out.lines.size() == 2);
    CHECK(out.lines[0].rect.w == doctest::Approx(30.0f));  // "hello", space excluded
    CHECK(out.lines[1].rect.w == doctest::Approx(30.0f));  // "world"
}

TEST_CASE("layoutText: forced break at \\n") {
    FakeTextShaper shaper;
    ShapeResult sr;
    shaper.shape("ab\ncd", makeShapeParams(), sr);

    TextLayoutResult out;
    layoutText(sr, makeLayoutParams(1000.0f), out);

    REQUIRE(out.lines.size() == 2);
    CHECK(out.lines[0].rect.w == doctest::Approx(12.0f));  // "ab"
    CHECK(out.lines[1].rect.w == doctest::Approx(12.0f));  // "cd"
}

TEST_CASE("layoutText: CJK breaks after every character") {
    FakeTextShaper shaper;
    ShapeResult sr;
    // U+65E5 U+672C U+8A9E ("nihongo" in kanji), each 10px wide at size 10.
    shaper.shape("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", makeShapeParams(), sr);

    TextLayoutResult out;
    layoutText(sr, makeLayoutParams(10.0f), out);

    REQUIRE(out.lines.size() == 3);
    for (auto& line : out.lines) {
        CHECK(line.rect.w == doctest::Approx(10.0f));
    }
}

TEST_CASE("layoutText: single long word with no break opportunity forces a grapheme split") {
    FakeTextShaper shaper;
    ShapeResult sr;
    shaper.shape("aaaaaaaaaa", makeShapeParams(), sr);  // 10 chars, 6px each, no break points

    TextLayoutResult out;
    layoutText(sr, makeLayoutParams(25.0f), out);

    REQUIRE(out.lines.size() == 3);  // 4 chars (24px), 4 chars (24px), 2 chars (12px)
    CHECK(out.lines[0].rect.w == doctest::Approx(24.0f));
    CHECK(out.lines[1].rect.w == doctest::Approx(24.0f));
    CHECK(out.lines[2].rect.w == doctest::Approx(12.0f));
}

TEST_CASE("layoutText: text-align center/right, and start/end resolved via direction") {
    FakeTextShaper shaper;
    ShapeResult sr;
    shaper.shape("abcd", makeShapeParams(), sr);  // width 24
    float availMain = 100.0f;

    {
        TextLayoutParams p = makeLayoutParams(availMain);
        p.textAlign = TextAlign::Center;
        TextLayoutResult out;
        layoutText(sr, p, out);
        CHECK(out.lines[0].rect.x == doctest::Approx((availMain - 24.0f) / 2.0f));
    }
    {
        TextLayoutParams p = makeLayoutParams(availMain);
        p.textAlign = TextAlign::Right;
        TextLayoutResult out;
        layoutText(sr, p, out);
        CHECK(out.lines[0].rect.x == doctest::Approx(availMain - 24.0f));
    }
    {
        // start, LTR -> left (x == 0)
        TextLayoutParams p = makeLayoutParams(availMain, TextDirection::Ltr);
        p.textAlign = TextAlign::Start;
        TextLayoutResult out;
        layoutText(sr, p, out);
        CHECK(out.lines[0].rect.x == doctest::Approx(0.0f));
    }
    {
        // start, RTL -> right
        ShapeResult srRtl;
        shaper.shape("abcd", makeShapeParams(TextDirection::Rtl), srRtl);
        TextLayoutParams p = makeLayoutParams(availMain, TextDirection::Rtl);
        p.textAlign = TextAlign::Start;
        TextLayoutResult out;
        layoutText(srRtl, p, out);
        CHECK(out.lines[0].rect.x == doctest::Approx(availMain - 24.0f));
    }
    {
        // end, LTR -> right
        TextLayoutParams p = makeLayoutParams(availMain, TextDirection::Ltr);
        p.textAlign = TextAlign::End;
        TextLayoutResult out;
        layoutText(sr, p, out);
        CHECK(out.lines[0].rect.x == doctest::Approx(availMain - 24.0f));
    }
}

TEST_CASE("layoutText: bidi run order, LTR paragraph with an embedded RTL word") {
    // "abc XYZ def": codepoint indices X=4,Y=5,Z=6 -> rtl range [4,7).
    FakeTextShaper rtlShaper({{4, 7}});
    ShapeResult sr;
    rtlShaper.shape("abc XYZ def", makeShapeParams(TextDirection::Ltr), sr);
    REQUIRE(sr.runs.size() == 3);
    CHECK(sr.runs[0].bidiLevel == 0);
    CHECK(sr.runs[1].bidiLevel == 1);
    CHECK(sr.runs[2].bidiLevel == 0);

    TextLayoutResult out;
    layoutText(sr, makeLayoutParams(1000.0f), out);
    REQUIRE(out.lines[0].runCount == 3);
    const GlyphRun& middle = out.runs[out.lines[0].firstRun + 1];
    // The RTL run's glyphs are already in visual order (Z, Y, X).
    REQUIRE(middle.glyphs.size() == 3);
    CHECK(middle.glyphs[0] == uint32_t('Z'));
    CHECK(middle.glyphs[1] == uint32_t('Y'));
    CHECK(middle.glyphs[2] == uint32_t('X'));
}

TEST_CASE("layoutText: pure RTL paragraph, start align resolves to the right") {
    // No rtlRanges needed: with paragraphDirection == Rtl, the whole text already gets the base
    // (odd) level from FakeTextShaper.
    FakeTextShaper shaper;
    ShapeResult sr;
    shaper.shape("abcd", makeShapeParams(TextDirection::Rtl), sr);
    REQUIRE(sr.runs.size() == 1);
    CHECK(sr.runs[0].bidiLevel == 1);

    TextLayoutParams p = makeLayoutParams(100.0f, TextDirection::Rtl);
    p.textAlign = TextAlign::Start;
    TextLayoutResult out;
    layoutText(sr, p, out);
    CHECK(out.lines[0].rect.x == doctest::Approx(100.0f - 24.0f));
}

TEST_CASE("bidiReorder: UAX#9 L2 reference cases") {
    {
        std::vector<uint8_t> levels = {0, 1, 1, 0};
        std::vector<uint32_t> order;
        bidiReorder(levels, order);
        std::vector<uint32_t> expect = {0, 2, 1, 3};
        CHECK(order == expect);
    }
    {
        std::vector<uint8_t> levels = {1, 2, 2, 1};
        std::vector<uint32_t> order;
        bidiReorder(levels, order);
        std::vector<uint32_t> expect = {3, 1, 2, 0};
        CHECK(order == expect);
    }
    {
        std::vector<uint8_t> levels = {0, 1, 2, 1, 0};
        std::vector<uint32_t> order;
        bidiReorder(levels, order);
        std::vector<uint32_t> expect = {0, 3, 2, 1, 4};
        CHECK(order == expect);
    }
}

TEST_CASE("layoutText: an RTL run split across a wrap is reordered independently per line") {
    // Whole string is one RTL run (uniform level 1): "abcd efgh".
    FakeTextShaper shaper({{0, 9}});
    ShapeResult sr;
    shaper.shape("abcd efgh", makeShapeParams(TextDirection::Ltr), sr);
    REQUIRE(sr.runs.size() == 1);
    CHECK(sr.runs[0].bidiLevel == 1);

    TextLayoutResult out;
    // "abcd " (27, space excluded from length -> 24) fits in 30; adding 'e' (33) doesn't.
    layoutText(sr, makeLayoutParams(30.0f), out);
    REQUIRE(out.lines.size() == 2);
    REQUIRE(out.lines[0].runCount == 1);
    REQUIRE(out.lines[1].runCount == 1);

    const GlyphRun& line0 = out.runs[out.lines[0].firstRun];
    std::vector<uint32_t> expect0 = {' ', 'd', 'c', 'b', 'a'};
    CHECK(line0.glyphs == expect0);

    const GlyphRun& line1 = out.runs[out.lines[1].firstRun];
    std::vector<uint32_t> expect1 = {'h', 'g', 'f', 'e'};
    CHECK(line1.glyphs == expect1);
}

TEST_CASE("layoutText: ellipsis, LTR trims from the visual right end") {
    FakeTextShaper shaper;
    ShapeResult sr;
    shaper.shape("abcdefghij", makeShapeParams(), sr);  // 10 chars * 6px = 60

    TextLayoutParams p = makeLayoutParams(40.0f);
    p.whiteSpace = WhiteSpace::NoWrap;
    p.textOverflow = TextOverflow::Ellipsis;
    p.ellipsisAdvance = 6.0f;
    p.ellipsisGlyph = 0x2026;
    p.ellipsisFont = 1;
    TextLayoutResult out;
    layoutText(sr, p, out);

    REQUIRE(out.lines.size() == 1);
    CHECK(out.lines[0].rect.w <= 40.0f);
    REQUIRE(out.lines[0].runCount >= 1);
    const GlyphRun& lastRun = out.runs[out.lines[0].firstRun + out.lines[0].runCount - 1];
    CHECK(lastRun.glyphs.back() == 0x2026u);
}

TEST_CASE("layoutText: ellipsis, RTL paragraph trims from the visual left end") {
    FakeTextShaper shaper;  // paragraphDirection == Rtl already makes the whole run level 1
    ShapeResult sr;
    shaper.shape("abcdefghij", makeShapeParams(TextDirection::Rtl), sr);

    TextLayoutParams p = makeLayoutParams(40.0f, TextDirection::Rtl);
    p.whiteSpace = WhiteSpace::NoWrap;
    p.textOverflow = TextOverflow::Ellipsis;
    p.ellipsisAdvance = 6.0f;
    p.ellipsisGlyph = 0x2026;
    p.ellipsisFont = 1;
    TextLayoutResult out;
    layoutText(sr, p, out);

    REQUIRE(out.lines.size() == 1);
    CHECK(out.lines[0].rect.w <= 40.0f);
    REQUIRE(out.lines[0].runCount >= 1);
    const GlyphRun& firstRun = out.runs[out.lines[0].firstRun];
    CHECK(firstRun.glyphs.front() == 0x2026u);
}

TEST_CASE("layoutText: vertical-rl columns, CJK upright, first column at the right") {
    FakeTextShaper shaper;
    ShapeResult sr;
    // 7 CJK characters, upright, 10px each along the main (y) axis.
    shaper.shape(
        "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"
        "\xE3\x83\x86\xE3\x82\xAD\xE3\x82\xB9\xE3\x83\x88",
        makeShapeParams(TextDirection::Ltr, WritingMode::VerticalRl), sr);
    REQUIRE(sr.runs.size() == 1);
    CHECK(sr.runs[0].vertical == 1);
    CHECK(sr.runs[0].rotated == 0);

    TextLayoutParams p = makeLayoutParams(30.0f, TextDirection::Ltr, WritingMode::VerticalRl);
    TextLayoutResult out;
    layoutText(sr, p, out);

    REQUIRE(out.lines.size() == 3);  // ceil(7/3)
    float totalCross = static_cast<float>(out.lines.size()) * kLineHeight;
    CHECK(out.crossSize == doctest::Approx(totalCross));
    CHECK(out.lines[0].rect.x == doctest::Approx(totalCross - kLineHeight));  // first column: rightmost
    CHECK(out.lines[1].rect.x == doctest::Approx(totalCross - 2 * kLineHeight));
    CHECK(out.lines[2].rect.x == doctest::Approx(totalCross - 3 * kLineHeight));
}

TEST_CASE("layoutText: vertical-lr columns, first column at the left") {
    FakeTextShaper shaper;
    ShapeResult sr;
    shaper.shape(
        "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"
        "\xE3\x83\x86\xE3\x82\xAD\xE3\x82\xB9\xE3\x83\x88",
        makeShapeParams(TextDirection::Ltr, WritingMode::VerticalLr), sr);

    TextLayoutParams p = makeLayoutParams(30.0f, TextDirection::Ltr, WritingMode::VerticalLr);
    TextLayoutResult out;
    layoutText(sr, p, out);

    REQUIRE(out.lines.size() == 3);
    CHECK(out.lines[0].rect.x == doctest::Approx(0.0f));
    CHECK(out.lines[1].rect.x == doctest::Approx(kLineHeight));
    CHECK(out.lines[2].rect.x == doctest::Approx(2 * kLineHeight));
}

TEST_CASE("layoutText: vertical mode, rotated ASCII run advances along y via xAdvance") {
    FakeTextShaper shaper;
    ShapeResult sr;
    shaper.shape("ABC", makeShapeParams(TextDirection::Ltr, WritingMode::VerticalRl), sr);
    REQUIRE(sr.runs.size() == 1);
    CHECK(sr.runs[0].rotated == 1);
    CHECK(sr.runs[0].vertical == 0);

    TextLayoutParams p = makeLayoutParams(1000.0f, TextDirection::Ltr, WritingMode::VerticalRl);
    TextLayoutResult out;
    layoutText(sr, p, out);

    REQUIRE(out.lines.size() == 1);
    // 3 ASCII chars * (10*0.6) = 18, via xAdvance accumulated along the column's y axis.
    CHECK(out.lines[0].rect.h == doctest::Approx(18.0f));
    REQUIRE(out.runs.size() == 1);
    REQUIRE(out.runs[0].positions.size() == 3);
    CHECK(out.runs[0].positions[0].x == doctest::Approx(out.runs[0].positions[1].x));
    CHECK(out.runs[0].positions[1].y > out.runs[0].positions[0].y);
}

TEST_CASE("layoutText: NullTextShaper produces an empty ShapeResult and a 0-sized layout") {
    NullTextShaper shaper;
    ShapeResult sr;
    shaper.shape("hello", makeShapeParams(), sr);
    CHECK(sr.runs.empty());

    TextLayoutResult out;
    layoutText(sr, makeLayoutParams(1000.0f), out);
    CHECK(out.lines.empty());
    CHECK(out.mainSize == doctest::Approx(0.0f));
    CHECK(out.crossSize == doctest::Approx(0.0f));
}

namespace {

/// T-331: layout params with a pre-shaped ellipsis (6 px, FakeTextShaper's 0.6 em at 10 px) and
/// `line-clamp: n`.
TextLayoutParams clampParams(float availMain, uint32_t lineClamp, TextDirection dir = TextDirection::Ltr) {
    TextLayoutParams p = makeLayoutParams(availMain, dir);
    p.lineClamp = lineClamp;
    p.ellipsisAdvance = 6.0f;
    p.ellipsisGlyph = 0x2026;
    p.ellipsisFont = 1;
    return p;
}

/// Every glyph id of `line`, in visual order.
std::vector<uint32_t> lineGlyphs(const TextLayoutResult& out, const TextLine& line) {
    std::vector<uint32_t> glyphs;
    for (uint32_t r = line.firstRun; r < line.firstRun + line.runCount; ++r) {
        glyphs.insert(glyphs.end(), out.runs[r].glyphs.begin(), out.runs[r].glyphs.end());
    }
    return glyphs;
}

bool hasEllipsis(const TextLayoutResult& out) {
    for (const GlyphRun& run : out.runs) {
        for (uint32_t g : run.glyphs) {
            if (g == 0x2026u) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

// "aaa bbb ccc ddd eee" at 42 px: "aaa bbb " (42) | "ccc ddd " | "eee", every line 39 px wide
// without its trailing space.
TEST_CASE("layoutText: line-clamp 2 keeps two lines and ends the second in an ellipsis") {
    FakeTextShaper shaper;
    ShapeResult sr;
    shaper.shape("aaa bbb ccc ddd eee", makeShapeParams(), sr);

    TextLayoutResult full;
    layoutText(sr, makeLayoutParams(42.0f), full);
    REQUIRE(full.lines.size() == 3);

    TextLayoutResult out;
    layoutText(sr, clampParams(42.0f, 2), out);
    REQUIRE(out.lines.size() == 2);
    CHECK(out.crossSize == doctest::Approx(2 * kLineHeight));
    CHECK(out.intrinsicMain == doctest::Approx(full.intrinsicMain));
    // The first line is untouched.
    CHECK(lineGlyphs(out, out.lines[0]) == lineGlyphs(full, full.lines[0]));
    // "ccc ddd" (39) + the ellipsis does not fit 42: one "d" goes, the trailing space too.
    const std::vector<uint32_t> second = lineGlyphs(out, out.lines[1]);
    CHECK(second == std::vector<uint32_t>{'c', 'c', 'c', ' ', 'd', 'd', 0x2026u});
    CHECK(out.lines[1].rect.w == doctest::Approx(39.0f));
    CHECK(out.lines[1].rect.w <= 42.0f);
    const GlyphRun& last = out.runs[out.lines[1].firstRun + out.lines[1].runCount - 1];
    CHECK(last.positions.back().x == doctest::Approx(33.0f));  // At the line's visual end.
}

TEST_CASE("layoutText: line-clamp 1 puts the ellipsis right after the last visible glyph") {
    FakeTextShaper shaper;
    ShapeResult sr;
    shaper.shape("aaa bbb", makeShapeParams(), sr);

    // "aaa " | "bbb" at 30 px; the first line has room, so only its trailing space goes.
    TextLayoutResult out;
    layoutText(sr, clampParams(30.0f, 1), out);
    REQUIRE(out.lines.size() == 1);
    CHECK(lineGlyphs(out, out.lines[0]) == std::vector<uint32_t>{'a', 'a', 'a', 0x2026u});
    CHECK(out.lines[0].rect.w == doctest::Approx(24.0f));
    const GlyphRun& last = out.runs[out.lines[0].firstRun + out.lines[0].runCount - 1];
    CHECK(last.positions.back().x == doctest::Approx(18.0f));
    CHECK(out.crossSize == doctest::Approx(kLineHeight));
}

TEST_CASE("layoutText: line-clamp at or above the line count changes nothing") {
    FakeTextShaper shaper;
    ShapeResult sr;
    shaper.shape("aaa bbb ccc ddd eee", makeShapeParams(), sr);
    TextLayoutResult full;
    layoutText(sr, makeLayoutParams(42.0f), full);
    REQUIRE(full.lines.size() == 3);
    for (uint32_t n : {3u, 5u}) {
        CAPTURE(n);
        TextLayoutResult out;
        layoutText(sr, clampParams(42.0f, n), out);
        REQUIRE(out.lines.size() == 3);
        CHECK_FALSE(hasEllipsis(out));
        for (size_t i = 0; i < 3; ++i) {
            CHECK(lineGlyphs(out, out.lines[i]) == lineGlyphs(full, full.lines[i]));
            CHECK(out.lines[i].rect.w == doctest::Approx(full.lines[i].rect.w));
        }
        CHECK(out.crossSize == doctest::Approx(full.crossSize));
    }
}

TEST_CASE("layoutText: line-clamp in an RTL paragraph trims from the visual left end") {
    FakeTextShaper shaper;
    ShapeResult sr;
    shaper.shape("aaa bbb ccc ddd eee", makeShapeParams(TextDirection::Rtl), sr);

    TextLayoutResult out;
    layoutText(sr, clampParams(42.0f, 2, TextDirection::Rtl), out);
    REQUIRE(out.lines.size() == 2);
    const std::vector<uint32_t> second = lineGlyphs(out, out.lines[1]);
    REQUIRE(!second.empty());
    CHECK(second.front() == 0x2026u);
    CHECK(second.back() != 0x2026u);
    CHECK(out.lines[1].rect.w <= 42.0f);
    for (uint32_t g : lineGlyphs(out, out.lines[0])) {
        CHECK(g != 0x2026u);  // The first line has no ellipsis.
    }
}

TEST_CASE("layoutText: nowrap with line-clamp 1 (the caller's text-overflow: ellipsis case)") {
    FakeTextShaper shaper;
    ShapeResult sr;
    shaper.shape("ab\ncd", makeShapeParams(), sr);  // A forced break: two lines even with nowrap.

    TextLayoutParams p = clampParams(1000.0f, 0);
    p.whiteSpace = WhiteSpace::NoWrap;
    p.textOverflow = TextOverflow::Ellipsis;
    TextLayoutResult two;
    layoutText(sr, p, two);
    CHECK(two.lines.size() == 2);
    CHECK_FALSE(hasEllipsis(two));

    p.lineClamp = 1;
    TextLayoutResult one;
    layoutText(sr, p, one);
    REQUIRE(one.lines.size() == 1);
    CHECK(lineGlyphs(one, one.lines[0]).back() == 0x2026u);
}

TEST_CASE("layoutText: line-clamp without an ellipsis glyph still stops after n lines") {
    FakeTextShaper shaper;
    ShapeResult sr;
    shaper.shape("aaa bbb ccc ddd eee", makeShapeParams(), sr);
    TextLayoutParams p = clampParams(42.0f, 2);
    p.ellipsisAdvance = 0;
    TextLayoutResult out;
    layoutText(sr, p, out);
    CHECK(out.lines.size() == 2);
    CHECK_FALSE(hasEllipsis(out));
}
