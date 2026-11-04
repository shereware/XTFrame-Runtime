#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "font_name_table.h"  // Code/Runtime/text/src/font_name_table.h
#include "test_temp.h"
#include "xt/text/ft_text_shaper.h"
#include "xt/text_layout.h"
#include "xt/text_shaper.h"

/// T-569 (Docs/design/07-text.md §7.20, report R4): `.ttc`/`.otc` face enumeration, OS/2 weight
/// selection with the CSS Fonts 4 rule, variable `wght` instances, synthetic bold, the "first CJK
/// family is not installed" rule and kinsoku line breaking. The deterministic cases use the
/// repository's fonts (Assets/Fonts, Tests/fonts); the cases that need an installed system font
/// (Microsoft JhengHei, Noto Sans CJK TC, the Noto Sans TC variable font) print a SKIP line and
/// pass when the font is absent, so CI on other machines stays green.

namespace {

namespace fs = std::filesystem;

std::string assetFont(const char* name) { return std::string(XT_ASSET_FONTS_DIR) + "/" + name; }

std::vector<uint8_t> readAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

uint32_t be32(const std::vector<uint8_t>& b, size_t off) {
    return (uint32_t(b[off]) << 24) | (uint32_t(b[off + 1]) << 16) | (uint32_t(b[off + 2]) << 8) | b[off + 3];
}
uint16_t be16(const std::vector<uint8_t>& b, size_t off) { return uint16_t((b[off] << 8) | b[off + 1]); }
void put32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    b[off] = uint8_t(v >> 24);
    b[off + 1] = uint8_t(v >> 16);
    b[off + 2] = uint8_t(v >> 8);
    b[off + 3] = uint8_t(v);
}

/// A TrueType collection (`ttcf` 1.0) holding `fonts` in order: each font's table directory is
/// copied with its table offsets rewritten to the tables' new absolute positions.
std::vector<uint8_t> makeCollection(const std::vector<std::vector<uint8_t>>& fonts) {
    std::vector<uint8_t> out(12 + 4 * fonts.size(), 0);
    out[0] = 't';
    out[1] = 't';
    out[2] = 'c';
    out[3] = 'f';
    put32(out, 4, 0x00010000u);
    put32(out, 8, static_cast<uint32_t>(fonts.size()));
    for (size_t f = 0; f < fonts.size(); ++f) {
        const std::vector<uint8_t>& font = fonts[f];
        const uint16_t tables = be16(font, 4);
        const size_t dirSize = 12 + 16 * size_t(tables);
        const size_t dirAt = out.size();
        put32(out, 12 + 4 * f, static_cast<uint32_t>(dirAt));
        out.insert(out.end(), font.begin(), font.begin() + static_cast<std::ptrdiff_t>(dirSize));
        for (uint16_t t = 0; t < tables; ++t) {
            const size_t record = dirAt + 12 + 16 * size_t(t);
            const uint32_t offset = be32(font, 12 + 16 * size_t(t) + 8);
            const uint32_t length = be32(font, 12 + 16 * size_t(t) + 12);
            while (out.size() % 4 != 0) out.push_back(0);
            put32(out, record + 8, static_cast<uint32_t>(out.size()));
            out.insert(out.end(), font.begin() + offset, font.begin() + offset + length);
        }
    }
    return out;
}

xt::FontRequest req(const std::vector<std::string_view>& families, uint16_t weight = 400) {
    return xt::FontRequest{std::span<const std::string_view>(families), weight, false};
}

/// The glyph `utf8` (one character) shapes to with `request`, and the font it came from.
struct OneGlyph {
    xt::FontId font = xt::kInvalidFont;
    uint32_t glyph = 0;
    float advance = 0;
};
OneGlyph shapeOne(xt::text::FtTextShaper& shaper, const xt::FontRequest& request, std::string_view utf8) {
    xt::ShapeParams params;
    params.font = request;
    params.size = 48;
    xt::ShapeResult out;
    shaper.shape(utf8, params, out);
    OneGlyph g;
    if (!out.runs.empty() && !out.glyphs.empty()) {
        g.font = out.runs[0].font;
        g.glyph = out.glyphs[0].glyphId;
        g.advance = out.glyphs[0].xAdvance;
    }
    return g;
}

/// Total coverage of a glyph's bitmap at 48 px: grows with the stroke width.
uint64_t ink(xt::text::FtTextShaper& shaper, const OneGlyph& g) {
    xt::GlyphBitmap bitmap;
    if (!shaper.glyphBitmap(g.font, g.glyph, 48.0f, 0.0f, bitmap)) return 0;
    uint64_t sum = 0;
    for (uint8_t a : bitmap.alpha) sum += a;
    return sum;
}

uint64_t inkAt(xt::text::FtTextShaper& shaper, std::string_view family, uint16_t weight,
               std::string_view utf8) {
    const std::vector<std::string_view> families = {family};
    return ink(shaper, shapeOne(shaper, req(families, weight), utf8));
}

uint16_t weightOf(xt::text::FtTextShaper& shaper, xt::FontId id) {
    std::vector<xt::FontDesc> fonts;
    shaper.fonts(fonts);
    for (const xt::FontDesc& f : fonts) {
        if (f.id == id) return f.weight;
    }
    return 0;
}

std::string familyOf(xt::text::FtTextShaper& shaper, xt::FontId id) {
    std::vector<xt::FontDesc> fonts;
    shaper.fonts(fonts);
    for (const xt::FontDesc& f : fonts) {
        if (f.id == id) return f.family;
    }
    return std::string();
}

bool systemFontPresent(const char* path) {
    std::error_code ec;
    if (fs::exists(path, ec)) return true;
    std::printf("SKIP: %s is not installed\n", path);
    return false;
}

// The R4 sentence: 甲，乙：丙（丁）、戊。
const std::string kR4 =
    "\xE7\x94\xB2\xEF\xBC\x8C\xE4\xB9\x99\xEF\xBC\x9A\xE4\xB8\x99\xEF\xBC\x88\xE4\xB8\x81\xEF\xBC\x89"
    "\xE3\x80\x81\xE6\x88\x8A\xE3\x80\x82";
const char* const kGuo = "\xE5\x9C\x8B";  // 國

/// Every glyph of `text` comes from `family`, is not .notdef, has ink and is not a filled box.
void checkGlyphsDrawn(xt::text::FtTextShaper& shaper, const std::vector<std::string_view>& families,
                      std::string_view family, const std::string& text) {
    xt::ShapeParams params;
    params.font = req(families);
    params.size = 32;
    xt::ShapeResult out;
    shaper.shape(text, params, out);
    REQUIRE(out.glyphs.size() == 11);
    for (const xt::ShapedRun& run : out.runs) {
        CHECK(familyOf(shaper, run.font) == family);
        for (uint32_t i = run.glyphStart; i < run.glyphStart + run.glyphCount; ++i) {
            const xt::ShapedGlyph& g = out.glyphs[i];
            CAPTURE(g.cluster);
            CHECK(g.glyphId != 0);
            xt::GlyphBitmap bitmap;
            REQUIRE(shaper.glyphBitmap(run.font, g.glyphId, 32.0f, 0.0f, bitmap));
            size_t inked = 0, solid = 0;
            for (uint8_t a : bitmap.alpha) {
                inked += a > 0 ? 1 : 0;
                solid += a >= 250 ? 1 : 0;
            }
            CHECK(inked > 0);
            // A filled box (the R4 `。`) is solid over nearly its whole bitmap.
            CHECK(solid < bitmap.alpha.size() * 9 / 10);
        }
    }
}

}  // namespace

TEST_CASE("T-569 font_name_table: OS/2 weight class, CJK coverage, face count") {
    xt::text::FontNameInfo regular, bold, tc;
    REQUIRE(xt::text::readFontNameTable(assetFont("NotoSans-Regular.ttf"), regular));
    REQUIRE(xt::text::readFontNameTable(assetFont("NotoSans-Bold.ttf"), bold));
    REQUIRE(xt::text::readFontNameTable(assetFont("NotoSansTC-Regular.ttf"), tc));
    CHECK(regular.weightClass == 400);
    CHECK(bold.weightClass == 700);
    CHECK(tc.weightClass == 400);
    CHECK_FALSE(regular.cjk);
    CHECK(tc.cjk);
    CHECK(tc.family == "Noto Sans TC");
    CHECK(xt::text::fontFaceCount(assetFont("NotoSans-Regular.ttf")) == 1);
    CHECK(xt::text::fontFaceCount(assetFont("OFL-NotoSans.txt")) == 0);
    xt::text::FontNameInfo none;
    CHECK_FALSE(xt::text::readFontNameTable(assetFont("NotoSans-Regular.ttf"), none, 1));
}

TEST_CASE("T-569 collections: every face of a .ttc is a candidate with its face index") {
    const fs::path dir = xt::test::tempDir() / "xt_t569_ttc";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const std::vector<uint8_t> ttc =
        makeCollection({readAll(assetFont("NotoSans-Regular.ttf")), readAll(assetFont("NotoSans-Bold.ttf"))});
    const fs::path file = dir / "pair.ttc";
    {
        std::ofstream out(file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(ttc.data()), static_cast<std::streamsize>(ttc.size()));
    }
    CHECK(xt::text::fontFaceCount(file.generic_string()) == 2);
    {
        xt::text::FtTextShaper shaper;
        CHECK(shaper.addFontDir(dir.generic_string(), 16) == 2);
        std::vector<xt::FontDesc> fonts;
        shaper.fonts(fonts);
        REQUIRE(fonts.size() == 2);
        CHECK(fonts[0].family == "Noto Sans");
        CHECK(fonts[1].family == "Noto Sans");
        CHECK(fonts[0].weight == 400);
        CHECK(fonts[1].weight == 700);
        CHECK(shaper.fontFaceIndex(fonts[0].id) == 0);
        CHECK(shaper.fontFaceIndex(fonts[1].id) == 1);
        CHECK(fs::path(shaper.fontPath(fonts[1].id)) == file);
        // Face 1 really opens as the bold face: its strokes carry more ink.
        CHECK(inkAt(shaper, "Noto Sans", 700, "H") > inkAt(shaper, "Noto Sans", 400, "H"));
        CHECK(shaper.openedFontCount() == 2);
    }
    // A plain .ttf is not a collection face.
    xt::text::FtTextShaper single;
    CHECK(single.addFontDir(XT_ASSET_FONTS_DIR, 64) > 0);
    CHECK(single.fontFaceIndex(0) == -1);
    fs::remove_all(dir, ec);
}

TEST_CASE("T-569 weights: static faces are chosen by usWeightClass with the CSS Fonts 4 rule") {
    xt::text::FtTextShaper shaper;
    REQUIRE(shaper.addFontFile(assetFont("NotoSans-Regular.ttf")));
    REQUIRE(shaper.addFontFile(assetFont("NotoSans-Bold.ttf")));
    const std::vector<std::string_view> families = {"Noto Sans"};
    const auto chosen = [&](uint16_t weight) {
        return weightOf(shaper, shaper.match(req(families, weight)));
    };
    CHECK(chosen(100) == 400);  // nothing lighter: the lightest heavier face
    CHECK(chosen(300) == 400);
    CHECK(chosen(400) == 400);
    CHECK(chosen(500) == 400);  // 400..500: heavier up to 500 first (none), then lighter
    CHECK(chosen(600) == 700);  // above 500: heavier first
    CHECK(chosen(700) == 700);
    CHECK(chosen(900) == 700);  // the heaviest face is >= 600: no synthetic bold
    std::vector<xt::FontDesc> listed;
    shaper.fonts(listed);
    CHECK(listed.size() == 2);
}

TEST_CASE("T-569 synthetic bold: a family whose heaviest face is < 600 is emboldened at >= 600") {
    xt::text::FtTextShaper shaper;
    REQUIRE(shaper.addFontFile(assetFont("NotoSansTC-Regular.ttf")));
    const std::vector<std::string_view> families = {"Noto Sans TC"};
    const OneGlyph w300 = shapeOne(shaper, req(families, 300), kGuo);
    const OneGlyph w400 = shapeOne(shaper, req(families, 400), kGuo);
    const OneGlyph w700 = shapeOne(shaper, req(families, 700), kGuo);
    const OneGlyph w900 = shapeOne(shaper, req(families, 900), kGuo);
    REQUIRE(w400.glyph != 0);
    CHECK(w300.font == w400.font);  // 300 falls back to the only (400) face, unemboldened
    CHECK(w700.font != w400.font);  // a synthetic-bold instance
    CHECK(w700.glyph == w400.glyph);
    const uint64_t i300 = ink(shaper, w300), i400 = ink(shaper, w400), i700 = ink(shaper, w700),
                   i900 = ink(shaper, w900);
    std::printf("Noto Sans TC (bundled) ink 300/400/700/900: %llu %llu %llu %llu\n",
                static_cast<unsigned long long>(i300), static_cast<unsigned long long>(i400),
                static_cast<unsigned long long>(i700), static_cast<unsigned long long>(i900));
    CHECK(i300 == i400);
    CHECK(i700 > i400 * 11 / 10);
    CHECK(i900 > i700);
    CHECK(w700.advance > w400.advance);  // HarfBuzz synthetic bold widens the advance as well
    // The instance is not listed as a font of its own.
    std::vector<xt::FontDesc> listed;
    shaper.fonts(listed);
    CHECK(listed.size() == 1);
    // Outlines are emboldened too (SW / GPU outline paths).
    std::vector<xt::PathVerb> verbs400, verbs700;
    std::vector<float> points400, points700;
    REQUIRE(shaper.glyphOutline(w400.font, w400.glyph, verbs400, points400));
    REQUIRE(shaper.glyphOutline(w700.font, w700.glyph, verbs700, points700));
    CHECK(points400 != points700);
}

TEST_CASE("T-569 R4 sentence: every glyph is drawn from the bundled Noto Sans TC (deterministic)") {
    xt::text::FtTextShaper shaper;
    REQUIRE(shaper.addFontDir(XT_ASSET_FONTS_DIR, 64) > 0);
    checkGlyphsDrawn(shaper, {"Noto Sans TC", "sans-serif"}, "Noto Sans TC", kR4);
}

TEST_CASE("T-569 R4 sentence: 'Microsoft JhengHei' stack draws no empty or box glyph (Windows)") {
    if (!systemFontPresent("C:/Windows/Fonts/msjh.ttc")) return;
    xt::text::FtTextShaper shaper;
    REQUIRE(shaper.addSystemFonts() > 0);
    std::vector<xt::FontDesc> fonts;
    shaper.fonts(fonts);
    bool listed = false;
    for (const xt::FontDesc& f : fonts) {
        const std::string path = shaper.fontPath(f.id);
        if (f.family == "Microsoft JhengHei" && path.size() >= 8 &&
            path.substr(path.size() - 8) == "msjh.ttc" && shaper.fontFaceIndex(f.id) == 0) {
            listed = true;
        }
    }
    CHECK(listed);
    checkGlyphsDrawn(shaper, {"Microsoft JhengHei", "Segoe UI", "sans-serif"}, "Microsoft JhengHei", kR4);
    CHECK(shaper.missingCjkFamily({"Microsoft JhengHei", "Segoe UI", "sans-serif"}).empty());
}

TEST_CASE("T-569 Noto Sans CJK TC 300/400/700: stroke width grows monotonically (installed)") {
    if (!systemFontPresent("C:/Windows/Fonts/NotoSansCJKtc-Regular.otf")) return;
    xt::text::FtTextShaper shaper;
    REQUIRE(shaper.addSystemFonts() > 0);
    const uint64_t i300 = inkAt(shaper, "Noto Sans CJK TC", 300, kGuo);
    const uint64_t i400 = inkAt(shaper, "Noto Sans CJK TC", 400, kGuo);
    const uint64_t i700 = inkAt(shaper, "Noto Sans CJK TC", 700, kGuo);
    std::printf("Noto Sans CJK TC ink 300/400/700: %llu %llu %llu\n", static_cast<unsigned long long>(i300),
                static_cast<unsigned long long>(i400), static_cast<unsigned long long>(i700));
    CHECK(i300 > 0);
    CHECK(i300 < i400);
    CHECK(i400 < i700);
}

TEST_CASE("T-569 variable fonts: Noto Sans TC VF follows the wght axis (installed)") {
    if (!systemFontPresent("C:/Windows/Fonts/NotoSansTC-VF.ttf")) return;
    xt::text::FtTextShaper shaper;
    REQUIRE(shaper.addFontDir("C:/Windows/Fonts", xt::text::FtTextShaper::kMaxSystemFonts) > 0);
    const std::vector<std::string_view> families = {"Noto Sans TC"};
    const OneGlyph w300 = shapeOne(shaper, req(families, 300), kGuo);
    const OneGlyph w400 = shapeOne(shaper, req(families, 400), kGuo);
    const OneGlyph w700 = shapeOne(shaper, req(families, 700), kGuo);
    const OneGlyph w900 = shapeOne(shaper, req(families, 900), kGuo);
    const uint64_t i300 = ink(shaper, w300), i400 = ink(shaper, w400), i700 = ink(shaper, w700),
                   i900 = ink(shaper, w900);
    std::printf("Noto Sans TC VF ink 300/400/700/900: %llu %llu %llu %llu\n",
                static_cast<unsigned long long>(i300), static_cast<unsigned long long>(i400),
                static_cast<unsigned long long>(i700), static_cast<unsigned long long>(i900));
    CHECK(i300 < i400);
    CHECK(i400 < i700);
    CHECK(i700 < i900);
}

TEST_CASE("T-569 missing font: the first CJK family of a stack that is not installed") {
    using xt::text::FamilyPresence;
    const auto none = [](std::string_view) { return FamilyPresence::kAbsent; };
    CHECK(xt::text::firstMissingCjkFamily({"Microsoft JhengHei", "Segoe UI", "sans-serif"}, none) ==
          "Microsoft JhengHei");
    CHECK(xt::text::firstMissingCjkFamily({"Segoe UI", "sans-serif"}, none).empty());
    CHECK(xt::text::firstMissingCjkFamily({"sans-serif", "Noto Sans TC"}, none) == "Noto Sans TC");
    // A localized family name is CJK by its characters (微軟正黑體).
    CHECK(xt::text::isCjkFamilyName("\xE5\xBE\xAE\xE8\xBB\x9F\xE6\xAD\xA3\xE9\xBB\x91\xE9\xAB\x94"));
    CHECK_FALSE(xt::text::isCjkFamilyName("Noto Sans"));
    CHECK(xt::text::splitFontFamilyList("'Microsoft JhengHei', \"Segoe UI\" , sans-serif") ==
          std::vector<std::string>{"Microsoft JhengHei", "Segoe UI", "sans-serif"});

    xt::text::FtTextShaper shaper;
    REQUIRE(shaper.addFontDir(XT_ASSET_FONTS_DIR, 64) > 0);
    CHECK(shaper.missingCjkFamily({"Xt No Such JhengHei", "Segoe UI", "sans-serif"}) ==
          "Xt No Such JhengHei");
    // An installed CJK family ends the search, so a later missing one is not reported.
    CHECK(shaper.missingCjkFamily({"Noto Sans TC", "Xt No Such JhengHei"}).empty());
    // An installed face with OS/2 CJK coverage counts as CJK whatever its name.
    CHECK(shaper.missingCjkFamily({"Noto Sans", "Noto Sans TC"}).empty());
    CHECK(shaper.missingCjkFamily({"Noto Sans", "sans-serif"}).empty());
}

TEST_CASE("T-569 kinsoku: break opportunities sit on each codepoint's first byte") {
    xt::text::FtTextShaper shaper;
    REQUIRE(shaper.addFontFile(assetFont("NotoSansTC-Regular.ttf")));
    const std::vector<std::string_view> families = {"Noto Sans TC"};
    xt::ShapeParams params;
    params.font = req(families);
    params.size = 20;
    xt::ShapeResult out;
    // 甲。乙 / 甲乙: libunibreak's UAX#14 classes are right (。 is CL); the break value of each
    // character is at its first byte, which is where text layout reads it.
    shaper.shape("\xE7\x94\xB2\xE3\x80\x82\xE4\xB9\x99", params, out);
    REQUIRE(out.breakAfter.size() == 9);
    CHECK(out.breakAfter[0] == 3);  // no break between 甲 and 。 -- not even an emergency one
    CHECK(out.breakAfter[3] == 1);  // a break after 。
    CHECK(out.breakAfter[1] == 0);
    CHECK(out.breakAfter[2] == 0);
    shaper.shape("\xE7\x94\xB2\xE4\xB9\x99", params, out);
    CHECK(out.breakAfter[0] == 1);
    // 「乙: no break after an opening bracket.
    shaper.shape("\xE3\x80\x8C\xE4\xB9\x99", params, out);
    CHECK(out.breakAfter[0] == 3);
}

TEST_CASE("T-569 kinsoku: 甲。乙 in a narrow box never starts a line with 。") {
    xt::text::FtTextShaper shaper;
    REQUIRE(shaper.addFontFile(assetFont("NotoSansTC-Regular.ttf")));
    const std::vector<std::string_view> families = {"Noto Sans TC"};
    xt::ShapeParams params;
    params.font = req(families);
    params.size = 20;
    const xt::FontMetrics metrics = shaper.metrics(shaper.match(req(families)));
    const auto layoutLines = [&](const std::string& text, float width) {
        xt::ShapeResult out;
        shaper.shape(text, params, out);
        xt::TextLayoutParams p;
        p.availMain = width;
        p.fontSize = 20;
        p.lineHeight = 24;
        p.ascent = metrics.ascent * 20;
        p.descent = metrics.descent * 20;
        xt::TextLayoutResult layout;
        xt::layoutText(out, p, layout);
        std::vector<std::string> lines;
        for (const xt::TextLine& line : layout.lines)
            lines.push_back(text.substr(line.byteStart, line.byteEnd - line.byteStart));
        return lines;
    };
    const std::string kJiaJuYi = "\xE7\x94\xB2\xE3\x80\x82\xE4\xB9\x99";  // 甲。乙
    const char* const kJu = "\xE3\x80\x82";                               // 。
    // 1.5 em: only one ideograph fits; 。 hangs after 甲 instead of opening the next line.
    std::vector<std::string> lines = layoutLines(kJiaJuYi, 30.0f);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "\xE7\x94\xB2\xE3\x80\x82");
    CHECK(lines[1] == "\xE4\xB9\x99");
    // 2.5 em: 甲。 fits, 乙 wraps.
    lines = layoutLines(kJiaJuYi, 50.0f);
    REQUIRE(lines.size() == 2);
    CHECK(lines[1] == "\xE4\xB9\x99");
    // The R4 sentence at every width from 1.5 to 5.5 em: no line starts with a closing mark and
    // no line ends with an opening bracket.
    const std::vector<std::string> startProhibited = {kJu, "\xEF\xBC\x8C", "\xE3\x80\x81", "\xEF\xBC\x89",
                                                      "\xEF\xBC\x9A"};
    for (float width = 30.0f; width <= 110.0f; width += 10.0f) {
        CAPTURE(width);
        lines = layoutLines(kR4, width);
        CHECK(lines.size() >= 2);
        for (size_t i = 0; i < lines.size(); ++i) {
            for (const std::string& mark : startProhibited) {
                CHECK(lines[i].rfind(mark, 0) != 0);
            }
            const std::string& line = lines[i];
            CHECK_FALSE((line.size() >= 3 && line.substr(line.size() - 3) == "\xEF\xBC\x88"));  // （
        }
    }
}
