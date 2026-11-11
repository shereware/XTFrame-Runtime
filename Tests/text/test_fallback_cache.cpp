#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "xt/text/ft_text_shaper.h"
#include "xt/text_shaper.h"

/// T-673: the per-shaper font fallback cache (codepoint -> font) and the normalized family-name
/// lookup must leave every shaping result exactly as it was. The expected values below were
/// produced by the code before T-673 (same fonts, same text) and are compared field by field:
/// run font, run byte range, glyph id, cluster and advances.

namespace {

std::string assetFont(const char* name) { return std::string(XT_ASSET_FONTS_DIR) + "/" + name; }
std::string testFont(const char* name) { return std::string(XT_TEST_FONTS_DIR) + "/" + name; }

/// Latin, CJK (Han, kana, fullwidth punctuation), emoji, a symbol, a rare CJK Extension B ideograph
/// no registered font has, and Latin again.
constexpr std::string_view kSample =
    "Hello \xE4\xB8\x96\xE7\x95\x8C\xEF\xBC\x8C\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88 "
    "\xF0\x9F\x98\x80 \xE2\x98\x83 \xF0\xA0\x80\x80 \xE1\xBA\x9E fin";

/// Registers a fixed font set: three Noto Sans styles and Noto Serif by family only, Noto Sans TC,
/// Noto Sans SC (subset) and Noto Emoji (subset) also as fallback fonts, in that order.
void registerFonts(xt::text::FtTextShaper& shaper) {
    REQUIRE(shaper.addFontFile(assetFont("NotoSans-Regular.ttf")));          // 0
    REQUIRE(shaper.addFontFile(assetFont("NotoSans-Bold.ttf")));             // 1
    REQUIRE(shaper.addFontFile(assetFont("NotoSans-Italic.ttf")));           // 2
    REQUIRE(shaper.addFontFile(assetFont("NotoSerif-Regular.ttf")));         // 3
    REQUIRE(shaper.addFontFile(testFont("NotoSansSC-Regular.subset.ttf")));  // 4
    REQUIRE(shaper.addFontFile(assetFont("NotoSansTC-Regular.ttf")));        // 5
    REQUIRE(shaper.addFontFile(testFont("NotoEmoji-Regular.subset.ttf")));   // 6
    shaper.addFallbackFont(4);
    shaper.addFallbackFont(5);
    shaper.addFallbackFont(6);
    REQUIRE(shaper.fontCount() == 7);
}

struct ExpectedRun {
    xt::FontId font;
    uint32_t byteStart, byteEnd, glyphCount;
};
struct ExpectedGlyph {
    uint32_t glyphId, cluster;
    float xAdvance;
};
struct Expected {
    std::vector<ExpectedRun> runs;
    std::vector<ExpectedGlyph> glyphs;
};

xt::ShapeResult shapeWith(xt::text::FtTextShaper& shaper, std::string_view text,
                          const std::vector<std::string_view>& families, uint16_t weight, bool italic) {
    xt::ShapeParams params;
    params.font = xt::FontRequest{std::span<const std::string_view>(families), weight, italic};
    params.size = 20.0f;
    xt::ShapeResult out;
    shaper.shape(text, params, out);
    return out;
}

void check(const char* name, const xt::ShapeResult& r, const Expected& e) {
    INFO(name);
    REQUIRE(r.runs.size() == e.runs.size());
    for (size_t i = 0; i < e.runs.size(); ++i) {
        CHECK(r.runs[i].font == e.runs[i].font);
        CHECK(r.runs[i].byteStart == e.runs[i].byteStart);
        CHECK(r.runs[i].byteEnd == e.runs[i].byteEnd);
        CHECK(r.runs[i].glyphCount == e.runs[i].glyphCount);
    }
    REQUIRE(r.glyphs.size() == e.glyphs.size());
    for (size_t i = 0; i < e.glyphs.size(); ++i) {
        CHECK(r.glyphs[i].glyphId == e.glyphs[i].glyphId);
        CHECK(r.glyphs[i].cluster == e.glyphs[i].cluster);
        CHECK(r.glyphs[i].xAdvance == e.glyphs[i].xAdvance);
        CHECK(r.glyphs[i].yAdvance == 0.0f);
    }
}

// Produced by the pre-T-673 code (kSample, the font set of registerFonts(), 20 px).
const Expected kSans400 = {
    {{0, 0, 6, 6},
     {4, 6, 12, 2},
     {5, 12, 15, 1},
     {5, 15, 24, 3},
     {0, 24, 25, 1},
     {6, 25, 29, 1},
     {0, 29, 30, 1},
     {5, 30, 33, 1},
     {0, 33, 34, 1},
     {0, 34, 39, 2},
     {0, 39, 46, 4}},
    {{43, 0, 14.8125f},  {72, 1, 11.28125f},     {79, 2, 5.15625f},   {79, 3, 5.15625f},
     {82, 4, 12.09375f}, {3, 5, 5.203125f},      {2, 6, 20.0f},       {10, 9, 20.0f},
     {20466, 12, 20.0f}, {1335, 15, 20.0f},      {1322, 18, 20.0f},   {1337, 21, 20.0f},
     {3, 24, 5.203125f}, {1, 25, 25.390625f},    {3, 29, 5.203125f},  {1025, 30, 20.0f},
     {3, 33, 5.203125f}, {0, 34, 12.0f},         {3, 38, 5.203125f},  {1660, 39, 14.1875f},
     {3, 42, 5.203125f}, {1654, 43, 12.046875f}, {81, 45, 12.359375f}}};
const Expected kSans700 = {
    {{1, 0, 6, 6},
     {4, 6, 12, 2},
     {5, 12, 15, 1},
     {5, 15, 24, 3},
     {1, 24, 25, 1},
     {6, 25, 29, 1},
     {1, 29, 30, 1},
     {5, 30, 33, 1},
     {1, 33, 34, 1},
     {1, 34, 39, 2},
     {1, 39, 46, 4}},
    {{43, 0, 15.296875f}, {72, 1, 11.9375f},      {79, 2, 5.953125f}, {79, 3, 5.953125f},
     {82, 4, 12.5f},      {3, 5, 5.203125f},      {2, 6, 20.0f},      {10, 9, 20.0f},
     {20466, 12, 20.0f},  {1335, 15, 20.0f},      {1322, 18, 20.0f},  {1337, 21, 20.0f},
     {3, 24, 5.203125f},  {1, 25, 25.390625f},    {3, 29, 5.203125f}, {1025, 30, 20.0f},
     {3, 33, 5.203125f},  {0, 34, 11.78125f},     {3, 38, 5.203125f}, {1660, 39, 15.265625f},
     {3, 42, 5.203125f},  {1654, 43, 13.703125f}, {81, 45, 13.0f}}};
const Expected kSansItalic = {
    {{2, 0, 6, 6},
     {4, 6, 12, 2},
     {5, 12, 15, 1},
     {5, 15, 24, 3},
     {2, 24, 25, 1},
     {6, 25, 29, 1},
     {2, 29, 30, 1},
     {5, 30, 33, 1},
     {2, 33, 34, 1},
     {2, 34, 39, 2},
     {2, 39, 46, 4}},
    {{43, 0, 13.59375f},  {72, 1, 9.984375f},     {79, 2, 5.15625f},   {79, 3, 5.15625f},
     {82, 4, 11.265625f}, {3, 5, 5.203125f},      {2, 6, 20.0f},       {10, 9, 20.0f},
     {20466, 12, 20.0f},  {1335, 15, 20.0f},      {1322, 18, 20.0f},   {1337, 21, 20.0f},
     {3, 24, 5.203125f},  {1, 25, 25.390625f},    {3, 29, 5.203125f},  {1025, 30, 20.0f},
     {3, 33, 5.203125f},  {0, 34, 12.0f},         {3, 38, 5.203125f},  {1978, 39, 13.578125f},
     {3, 42, 5.203125f},  {2192, 43, 11.515625f}, {81, 45, 11.578125f}}};
const Expected kSerifThenSans = {
    {{3, 0, 6, 6},
     {4, 6, 12, 2},
     {5, 12, 15, 1},
     {5, 15, 24, 3},
     {3, 24, 25, 1},
     {6, 25, 29, 1},
     {3, 29, 30, 1},
     {5, 30, 33, 1},
     {3, 33, 34, 1},
     {3, 34, 39, 2},
     {3, 39, 46, 4}},
    {{43, 0, 15.859375f}, {72, 1, 10.703125f},    {79, 2, 6.203125f}, {79, 3, 6.203125f},
     {82, 4, 11.546875f}, {3, 5, 5.203125f},      {2, 6, 20.0f},      {10, 9, 20.0f},
     {20466, 12, 20.0f},  {1335, 15, 20.0f},      {1322, 18, 20.0f},  {1337, 21, 20.0f},
     {3, 24, 5.203125f},  {1, 25, 25.390625f},    {3, 29, 5.203125f}, {1025, 30, 20.0f},
     {3, 33, 5.203125f},  {0, 34, 12.0f},         {3, 38, 5.203125f}, {1982, 39, 13.640625f},
     {3, 42, 5.203125f},  {2148, 43, 13.265625f}, {81, 45, 12.90625f}}};

const std::vector<std::string_view> kSans = {"Noto Sans"};
const std::vector<std::string_view> kSerifSans = {"Noto Serif", "noto sans"};

}  // namespace

TEST_CASE("T-673 fallback cache: CJK, Latin, emoji and rare codepoints shape as before") {
    xt::text::FtTextShaper shaper;
    registerFonts(shaper);
    check("sans 400", shapeWith(shaper, kSample, kSans, 400, false), kSans400);
    check("sans 700", shapeWith(shaper, kSample, kSans, 700, false), kSans700);
    check("sans italic", shapeWith(shaper, kSample, kSans, 400, true), kSansItalic);
    check("serif, sans", shapeWith(shaper, kSample, kSerifSans, 400, false), kSerifThenSans);
}

TEST_CASE("T-673 fallback cache: a repeated shaping scans no fallback list") {
    xt::text::FtTextShaper shaper;
    registerFonts(shaper);
    check("sans 400 first", shapeWith(shaper, kSample, kSans, 400, false), kSans400);
    const uint64_t afterFirst = shaper.fallbackScanCount();
    CHECK(afterFirst > 0);
    check("sans 400 second", shapeWith(shaper, kSample, kSans, 400, false), kSans400);
    CHECK(shaper.fallbackScanCount() == afterFirst);
}

TEST_CASE("T-673 fallback cache: weights, styles and family chains do not leak into each other") {
    // The reverse order of the first case: each result is cached under its own key.
    xt::text::FtTextShaper shaper;
    registerFonts(shaper);
    for (int round = 0; round < 2; ++round) {
        check("serif, sans", shapeWith(shaper, kSample, kSerifSans, 400, false), kSerifThenSans);
        check("sans italic", shapeWith(shaper, kSample, kSans, 400, true), kSansItalic);
        check("sans 700", shapeWith(shaper, kSample, kSans, 700, false), kSans700);
        check("sans 400", shapeWith(shaper, kSample, kSans, 400, false), kSans400);
    }
    // The same family names in other string storage hit the same entries.
    const std::string name = "Noto Sans";
    const std::vector<std::string_view> copy = {std::string_view(name)};
    const uint64_t before = shaper.fallbackScanCount();
    check("sans 400 (copied names)", shapeWith(shaper, kSample, copy, 400, false), kSans400);
    CHECK(shaper.fallbackScanCount() == before);
}

TEST_CASE("T-673 fallback cache: results stay correct after the full table is cleared") {
    xt::text::FtTextShaper shaper;
    registerFonts(shaper);
    check("sans 400 warm", shapeWith(shaper, kSample, kSans, 400, false), kSans400);
    // 5,000 distinct ideographs: more than the 4,096-entry capacity, so the table is cleared and
    // refilled at least once.
    std::string many;
    for (uint32_t cp = 0x7600; cp < 0x7600 + 5000; ++cp) {
        many.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        many.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        many.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    const uint64_t before = shaper.fallbackScanCount();
    (void)shapeWith(shaper, many, kSans, 400, false);
    CHECK(shaper.fallbackScanCount() - before == 5000);
    check("sans 400 after clear", shapeWith(shaper, kSample, kSans, 400, false), kSans400);
    check("sans 700 after clear", shapeWith(shaper, kSample, kSans, 700, false), kSans700);
}

TEST_CASE("T-673 fallback cache: a font registered later clears the cache") {
    xt::text::FtTextShaper shaper;
    REQUIRE(shaper.addFontFile(assetFont("NotoSans-Regular.ttf")));
    // No CJK font yet: the ideograph falls back to match() = Noto Sans (.notdef). The two
    // codepoints are separate runs either way (Latin, then Han).
    const std::string_view text = "A\xE4\xB8\x96";
    xt::ShapeResult first = shapeWith(shaper, text, kSans, 400, false);
    REQUIRE(first.runs.size() == 2);
    CHECK(first.runs[0].font == 0);
    CHECK(first.runs[1].font == 0);
    REQUIRE(first.glyphs.size() == 2);
    CHECK(first.glyphs[1].glyphId == 0);
    std::ifstream in(testFont("NotoSansSC-Regular.subset.ttf"), std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(shaper.addFontMemory(std::move(bytes)));
    xt::ShapeResult second = shapeWith(shaper, text, kSans, 400, false);
    REQUIRE(second.runs.size() == 2);
    CHECK(second.runs[0].font == 0);
    CHECK(second.runs[1].font == 1);
    REQUIRE(second.glyphs.size() == 2);
    CHECK(second.glyphs[1].glyphId != 0);
}

TEST_CASE("T-673 fallback cache: system fonts, timing and digest (prints only)") {
    // Not a gate: it prints the time of a first and a second shaping pass over a mixed-script
    // corpus with the platform's system fonts, and a digest of every result, so a run before and
    // after a change can be compared on the same machine.
    xt::text::FtTextShaper shaper;
    const uint32_t added = shaper.addSystemFonts();
    if (added == 0) {
        std::printf("SKIP: no system fonts\n");
        return;
    }
    std::vector<std::string> corpus;
    auto addRange = [&corpus](uint32_t lo, uint32_t hi, uint32_t step) {
        std::string s;
        for (uint32_t cp = lo; cp <= hi; cp += step) {
            if (cp < 0x80) {
                s.push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
                s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
                s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
                s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }
        corpus.push_back(std::move(s));
    };
    addRange(0x20, 0x7E, 1);          // ASCII
    addRange(0xA0, 0x17F, 1);         // Latin-1, Latin Extended-A
    addRange(0x370, 0x3FF, 1);        // Greek
    addRange(0x400, 0x4FF, 1);        // Cyrillic
    addRange(0x590, 0x5FF, 1);        // Hebrew
    addRange(0x600, 0x6FF, 1);        // Arabic
    addRange(0xE00, 0xE7F, 1);        // Thai
    addRange(0x2000, 0x27BF, 3);      // punctuation, symbols, dingbats
    addRange(0x3000, 0x30FF, 1);      // CJK punctuation, kana
    addRange(0x4E00, 0x9FFF, 37);     // CJK ideographs, sampled
    addRange(0xAC00, 0xD7A3, 97);     // Hangul, sampled
    addRange(0xFF00, 0xFFEF, 1);      // fullwidth forms
    addRange(0x1F300, 0x1FAFF, 7);    // emoji, sampled
    addRange(0x20000, 0x2A6DF, 997);  // CJK Extension B, sampled
    const std::vector<std::vector<std::string_view>> chains = {
        {"Noto Sans"}, {"Segoe UI"}, {"Microsoft JhengHei", "Arial"}, {"No Such Family"}, {"serif"}};
    uint64_t digest = 1469598103934665603ull;
    auto mix = [&digest](uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            digest ^= (v >> (i * 8)) & 0xFF;
            digest *= 1099511628211ull;
        }
    };
    double passMs[2] = {0, 0};
    for (int pass = 0; pass < 2; ++pass) {
        const auto t0 = std::chrono::steady_clock::now();
        for (const auto& chain : chains) {
            for (const uint16_t weight : {uint16_t(400), uint16_t(700)}) {
                for (const bool italic : {false, true}) {
                    for (const std::string& text : corpus) {
                        const xt::ShapeResult r = shapeWith(shaper, text, chain, weight, italic);
                        if (pass != 0) continue;
                        for (const xt::ShapedRun& run : r.runs) {
                            mix(run.font);
                            mix(run.byteStart);
                            mix(run.glyphCount);
                        }
                        for (const xt::ShapedGlyph& g : r.glyphs) {
                            uint32_t adv = 0;
                            std::memcpy(&adv, &g.xAdvance, sizeof adv);
                            mix(g.glyphId);
                            mix(g.cluster);
                            mix(adv);
                        }
                    }
                }
            }
        }
        passMs[pass] =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    std::printf("T-673 system fonts (%u): first pass %.1f ms, second pass %.1f ms, digest %016llx\n", added,
                passMs[0], passMs[1], static_cast<unsigned long long>(digest));
}
