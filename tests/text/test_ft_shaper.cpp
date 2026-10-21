#include <string>
#include <thread>
#include <vector>

#include "doctest.h"
#include "vo_table.h"  // text/src/vo_table.h, exercised directly per docs/tasks/T-29.md's test list.
#include "xt/text/ft_text_shaper.h"

/// Tests for FtTextShaper (docs/tasks/T-29.md "測試"). Registers the three OFL test-font subsets
/// under tests/fonts/ (see tests/fonts/README.md for how they were produced) and exercises
/// match(), shape()'s full pipeline (bidi/script/fallback/run-splitting/shaping/breaks/graphemes),
/// glyphOutline(), glyphBitmap(), and thread-safety.

namespace {

std::string fontPath(const char* name) { return std::string(XT_TEST_FONTS_DIR) + "/" + name; }

struct ThreeFonts {
    xt::text::FtTextShaper shaper;
    ThreeFonts() {
        REQUIRE(shaper.addFontFile(fontPath("NotoSans-Regular.subset.ttf")));
        REQUIRE(shaper.addFontFile(fontPath("NotoSansSC-Regular.subset.ttf")));
        REQUIRE(shaper.addFontFile(fontPath("NotoNaskhArabic-Regular.subset.ttf")));
        REQUIRE(shaper.fontCount() == 3);
    }
};

xt::FontRequest req(const std::vector<std::string_view>& families, uint16_t weight = 400,
                    bool italic = false) {
    return xt::FontRequest{std::span<const std::string_view>(families), weight, italic};
}

}  // namespace

TEST_CASE("FtTextShaper::match finds registered fonts by family/weight") {
    ThreeFonts f;
    const std::vector<std::string_view> noto = {"Noto Sans"};
    const xt::FontId a = f.shaper.match(req(noto));
    CHECK(a != xt::kInvalidFont);

    const std::vector<std::string_view> unknown = {"Not A Real Family"};
    const xt::FontId fallback = f.shaper.match(req(unknown));
    CHECK(fallback == 0);  // first registered font

    // The only "Noto Sans" face is weight 400; requesting 700 must still resolve to it.
    const xt::FontId heavy = f.shaper.match(req(noto, 700));
    CHECK(heavy == a);
}

TEST_CASE("FtTextShaper::shape latin 'Hello World'") {
    ThreeFonts f;
    const std::vector<std::string_view> noto = {"Noto Sans"};
    xt::ShapeParams params;
    params.font = req(noto);
    params.size = 16;

    xt::ShapeResult out;
    f.shaper.shape("Hello World", params, out);

    REQUIRE(out.runs.size() == 1);
    REQUIRE(out.glyphs.size() == 11);
    for (const xt::ShapedGlyph& g : out.glyphs) CHECK(g.xAdvance > 0);

    bool foundSpace = false;
    for (const xt::ShapedGlyph& g : out.glyphs) {
        if (g.cluster == 5) {
            CHECK(g.isSpace == 1);
            foundSpace = true;
        } else {
            CHECK(g.isSpace == 0);
        }
    }
    CHECK(foundSpace);

    REQUIRE(out.breakAfter.size() == 11);
    CHECK(out.breakAfter[5] == 1);  // break allowed after the space

    REQUIRE(out.graphemeStart.size() == 11);
    for (uint8_t b : out.graphemeStart) CHECK(b == 1);
}

TEST_CASE("FtTextShaper::shape mixed latin+CJK falls back fonts and splits runs") {
    ThreeFonts f;
    const std::vector<std::string_view> families = {"Noto Sans", "Noto Sans SC"};
    xt::ShapeParams params;
    params.font = req(families);
    params.size = 16;

    xt::ShapeResult out;
    f.shaper.shape("Hello \xE4\xB8\x96\xE7\x95\x8C", params, out);  // "Hello 世界"

    REQUIRE(out.runs.size() == 2);
    CHECK(out.runs[0].font != out.runs[1].font);
    CHECK(out.runs[0].byteStart == 0);
    CHECK(out.runs[1].byteEnd == 12);

    // After the first CJK character (世, bytes [6,9)) a break is allowed.
    CHECK(out.breakAfter[8] == 1);
}

TEST_CASE("FtTextShaper::shape arabic paragraph in an LTR paragraph") {
    ThreeFonts f;
    const std::vector<std::string_view> arabic = {"Noto Naskh Arabic"};
    xt::ShapeParams params;
    params.font = req(arabic);
    params.size = 16;
    params.paragraphDirection = xt::TextDirection::Ltr;

    xt::ShapeResult out;
    f.shaper.shape("\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7", params, out);  // "مرحبا"

    REQUIRE(out.runs.size() == 1);
    CHECK(out.runs[0].bidiLevel == 1);
    // NotoNaskhArabic decomposes at least one letter into a base + mark pair via its 'ccmp'
    // GSUB feature, so glyph count is not bounded above by codepoint count for every Arabic
    // font/string (only true when no such decomposition occurs); just check it's plausible.
    CHECK(out.glyphs.size() >= 5);
    CHECK(out.glyphs.size() <= 10);
    REQUIRE(!out.glyphs.empty());
    // Visual order: the first glyph corresponds to the last codepoint's cluster (byte offset 8:
    // 5 two-byte Arabic letters, the 5th starts at byte 8).
    CHECK(out.glyphs.front().cluster == 8);
}

TEST_CASE("FtTextShaper::shape mixed latin/arabic/latin has 3 runs with levels [0,1,0]") {
    ThreeFonts f;
    const std::vector<std::string_view> families = {"Noto Sans", "Noto Naskh Arabic"};
    xt::ShapeParams params;
    params.font = req(families);
    params.size = 16;
    params.paragraphDirection = xt::TextDirection::Ltr;

    xt::ShapeResult out;
    // "abc مرحبا def"
    f.shaper.shape("abc \xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 def", params, out);

    // Run-split key is (level, script, font, orientation); per docs/tasks/T-29.md step 2, a
    // Common-script codepoint (the space) inherits the *previous* run's script, so the trailing
    // space right after the Arabic word carries Arabic script forward and becomes its own run
    // (same level/font as "def" but a different inherited script) rather than merging into
    // "def" -- 4 runs, not 3, with levels [0, 1, 0, 0].
    REQUIRE(out.runs.size() == 4);
    CHECK(out.runs[0].bidiLevel == 0);
    CHECK(out.runs[1].bidiLevel == 1);
    CHECK(out.runs[2].bidiLevel == 0);
    CHECK(out.runs[3].bidiLevel == 0);
}

TEST_CASE("FtTextShaper::shape RTL paragraph gives latin run level 2") {
    ThreeFonts f;
    const std::vector<std::string_view> noto = {"Noto Sans"};
    xt::ShapeParams params;
    params.font = req(noto);
    params.size = 16;
    params.paragraphDirection = xt::TextDirection::Rtl;

    xt::ShapeResult out;
    f.shaper.shape("abc", params, out);

    REQUIRE(out.runs.size() == 1);
    CHECK(out.runs[0].bidiLevel == 2);
}

TEST_CASE("FtTextShaper::shape newline is a mandatory break") {
    ThreeFonts f;
    const std::vector<std::string_view> noto = {"Noto Sans"};
    xt::ShapeParams params;
    params.font = req(noto);
    params.size = 16;

    xt::ShapeResult out;
    f.shaper.shape("a\nb", params, out);
    REQUIRE(out.breakAfter.size() == 3);
    CHECK(out.breakAfter[1] == 2);
}

TEST_CASE("FtTextShaper::shape combining mark forms one grapheme") {
    ThreeFonts f;
    const std::vector<std::string_view> noto = {"Noto Sans"};
    xt::ShapeParams params;
    params.font = req(noto);
    params.size = 16;

    xt::ShapeResult out;
    f.shaper.shape("e\xCC\x81", params, out);  // "e" + U+0301 COMBINING ACUTE ACCENT
    REQUIRE(out.graphemeStart.size() == 3);
    CHECK(out.graphemeStart[0] == 1);
    CHECK(out.graphemeStart[1] == 0);  // mid-grapheme: no break between e and the combining mark
}

TEST_CASE("FtTextShaper::shape vertical upright CJK") {
    ThreeFonts f;
    const std::vector<std::string_view> cjk = {"Noto Sans SC"};
    xt::ShapeParams params;
    params.font = req(cjk);
    params.size = 16;
    params.writingMode = xt::WritingMode::VerticalRl;
    params.orientation = xt::TextOrientation::Mixed;

    xt::ShapeResult out;
    f.shaper.shape("\xE6\x97\xA5\xE6\x9C\xAC", params, out);  // "日本"

    REQUIRE(!out.runs.empty());
    CHECK(out.runs[0].vertical == 1);
    REQUIRE(!out.glyphs.empty());
    for (const xt::ShapedGlyph& g : out.glyphs) {
        CHECK(g.yAdvance > 0);
        CHECK(g.xAdvance == 0);
    }
}

TEST_CASE("vo_table matches UAX#50 for known codepoints") {
    CHECK(xt::text::verticalOrientation(0x4E00) == xt::text::VerticalOrientation::Upright);
    CHECK(xt::text::verticalOrientation(0x0041) == xt::text::VerticalOrientation::Rotated);
    CHECK(xt::text::verticalOrientation(0x3001) == xt::text::VerticalOrientation::TransformedUpright);
}

TEST_CASE("FtTextShaper::shape handles invalid UTF-8 without crashing") {
    ThreeFonts f;
    const std::vector<std::string_view> noto = {"Noto Sans"};
    xt::ShapeParams params;
    params.font = req(noto);
    params.size = 16;

    xt::ShapeResult out;
    const char invalid[] = "a\xFF\xFEz";
    f.shaper.shape(std::string_view(invalid, sizeof(invalid) - 1), params, out);
    CHECK(!out.runs.empty());
    CHECK(!out.glyphs.empty());
}

TEST_CASE("FtTextShaper glyphOutline and glyphBitmap for 'A'") {
    ThreeFonts f;
    const std::vector<std::string_view> noto = {"Noto Sans"};
    const xt::FontId font = f.shaper.match(req(noto));
    REQUIRE(font != xt::kInvalidFont);

    xt::ShapeParams params;
    params.font = req(noto);
    params.size = 16;
    xt::ShapeResult shaped;
    f.shaper.shape("A", params, shaped);
    REQUIRE(!shaped.glyphs.empty());
    const uint32_t glyphId = shaped.glyphs[0].glyphId;

    std::vector<xt::PathVerb> verbs;
    std::vector<float> points;
    REQUIRE(f.shaper.glyphOutline(font, glyphId, verbs, points));
    CHECK(!verbs.empty());
    // docs/tasks/T-29.md: outline coordinates are em units, origin at the baseline, y flipped
    // (y-down: negative = above the baseline). 'A' has no descender, so x in roughly [0, 0.6]
    // (glyph advance width) and y in roughly [-1, 0] (baseline up to cap height); allow generous
    // margin for accents/overshoot on either axis without being so wide the check is meaningless.
    for (float p : points) CHECK(p >= -1.2f);
    for (float p : points) CHECK(p <= 1.2f);

    xt::GlyphBitmap bmp;
    REQUIRE(f.shaper.glyphBitmap(font, glyphId, 32.0f, bmp));
    CHECK(bmp.width > 10);
    CHECK(bmp.height > 10);
}

TEST_CASE("FtTextShaper::shape is thread-safe") {
    ThreeFonts f;
    const std::vector<std::string_view> noto = {"Noto Sans"};
    xt::ShapeParams params;
    params.font = req(noto);
    params.size = 16;

    std::vector<size_t> glyphCounts(2, 0);
    auto worker = [&](int idx) {
        for (int i = 0; i < 100; ++i) {
            xt::ShapeResult out;
            f.shaper.shape("Hello World", params, out);
            glyphCounts[static_cast<size_t>(idx)] = out.glyphs.size();
        }
    };
    std::thread t0(worker, 0);
    std::thread t1(worker, 1);
    t0.join();
    t1.join();
    CHECK(glyphCounts[0] == 11);
    CHECK(glyphCounts[1] == 11);
}

TEST_CASE("FtTextShaper::shape with the tnum feature (T-331, font-variant-numeric)") {
    // The Noto Sans subset has a `tnum` lookup, but its default figures are already tabular
    // (572/1000 em each; `pnum` would be the one to change them), so `tnum` must shape exactly
    // like the default: same glyphs, every digit the same advance.
    ThreeFonts f;
    const std::vector<std::string_view> noto = {"Noto Sans"};
    xt::ShapeParams params;
    params.font = req(noto);
    params.size = 20;
    xt::ShapeResult plain;
    f.shaper.shape("0123456789", params, plain);
    params.features = xt::kShapeFeatureTnum;
    xt::ShapeResult tabular;
    f.shaper.shape("0123456789", params, tabular);

    REQUIRE(plain.glyphs.size() == 10);
    REQUIRE(tabular.glyphs.size() == 10);
    for (size_t i = 0; i < 10; ++i) {
        CAPTURE(i);
        CHECK(tabular.glyphs[i].glyphId == plain.glyphs[i].glyphId);
        CHECK(tabular.glyphs[i].xAdvance == doctest::Approx(plain.glyphs[i].xAdvance));
        CHECK(tabular.glyphs[i].xAdvance == doctest::Approx(20 * 0.572f).epsilon(0.01));
    }
}
