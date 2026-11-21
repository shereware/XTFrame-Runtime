#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "font_name_table.h"  // Code/Runtime/text/src/font_name_table.h
#include "test_temp.h"
#include "xt/text/ft_text_shaper.h"
#include "xt/text_shaper.h"

/// T-853 (Docs/design/07-text.md §7.21.1-§7.21.3): face width (OS/2 usWidthClass trusted only when
/// the name says so, the `fvar` `wdth` range), legacy family names (nameID 1) and width-first face
/// selection. No test depends on an installed font and no font file is added to the repository:
/// the scan cases feed synthesized sfnt bytes (name / OS/2 / fvar only), the matching cases use
/// copies of the bundled Noto Sans faces (Assets/Fonts) whose `name` table is replaced and whose
/// OS/2 weight and width class are patched, written to the test's temp directory.

namespace {

namespace fs = std::filesystem;
using xt::text::FaceWidth;
using xt::text::FontNameInfo;
using xt::text::WidthWord;

std::string assetFont(const char* name) { return std::string(XT_ASSET_FONTS_DIR) + "/" + name; }

std::vector<uint8_t> readAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeAll(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void put16(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    b[off] = static_cast<uint8_t>((v >> 8) & 0xFF);
    b[off + 1] = static_cast<uint8_t>(v & 0xFF);
}
void put32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    put16(b, off, v >> 16);
    put16(b, off + 2, v & 0xFFFF);
}
uint32_t get32(const std::vector<uint8_t>& b, size_t off) {
    return (static_cast<uint32_t>(b[off]) << 24) | (static_cast<uint32_t>(b[off + 1]) << 16) |
           (static_cast<uint32_t>(b[off + 2]) << 8) | static_cast<uint32_t>(b[off + 3]);
}
uint32_t get16(const std::vector<uint8_t>& b, size_t off) {
    return (static_cast<uint32_t>(b[off]) << 8) | static_cast<uint32_t>(b[off + 1]);
}
uint32_t tagOf(const char* tag) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(tag[0])) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(tag[1])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(tag[2])) << 8) | static_cast<uint32_t>(static_cast<uint8_t>(tag[3]));
}

/// A `name` table (format 0) of Windows US-English records, ASCII values stored as UTF-16BE.
std::vector<uint8_t> nameTable(const std::map<uint16_t, std::string>& names) {
    const size_t count = names.size();
    std::vector<uint8_t> out(6 + 12 * count, 0);
    put16(out, 2, static_cast<uint32_t>(count));
    put16(out, 4, static_cast<uint32_t>(6 + 12 * count));
    std::vector<uint8_t> strings;
    size_t i = 0;
    for (const auto& [id, value] : names) {
        const size_t rec = 6 + 12 * i;
        put16(out, rec + 0, 3);       // platform: Windows
        put16(out, rec + 2, 1);       // encoding: Unicode BMP
        put16(out, rec + 4, 0x0409);  // language: US English
        put16(out, rec + 6, id);
        put16(out, rec + 8, static_cast<uint32_t>(value.size() * 2));
        put16(out, rec + 10, static_cast<uint32_t>(strings.size()));
        for (const char c : value) {
            strings.push_back(0);
            strings.push_back(static_cast<uint8_t>(c));
        }
        ++i;
    }
    out.insert(out.end(), strings.begin(), strings.end());
    return out;
}

/// An OS/2 table (version 4, 96 bytes): usWeightClass, usWidthClass, fsSelection.
std::vector<uint8_t> os2Table(uint16_t weight, uint16_t widthClass, uint16_t selection = 0x0040) {
    std::vector<uint8_t> out(96, 0);
    put16(out, 0, 4);
    put16(out, 4, weight);
    put16(out, 6, widthClass);
    put16(out, 62, selection);
    return out;
}

struct Axis {
    const char* tag;
    double min, def, max;
};

/// An `fvar` table with `axes` (axis records of 20 bytes) and no named instance.
std::vector<uint8_t> fvarTable(const std::vector<Axis>& axes) {
    std::vector<uint8_t> out(16 + 20 * axes.size(), 0);
    put16(out, 0, 1);
    put16(out, 4, 16);
    put16(out, 6, 2);
    put16(out, 8, static_cast<uint32_t>(axes.size()));
    put16(out, 10, 20);
    put16(out, 14, static_cast<uint32_t>(axes.size() * 4 + 4));
    for (size_t a = 0; a < axes.size(); ++a) {
        const size_t rec = 16 + 20 * a;
        put32(out, rec, tagOf(axes[a].tag));
        put32(out, rec + 4, static_cast<uint32_t>(static_cast<int32_t>(axes[a].min * 65536.0)));
        put32(out, rec + 8, static_cast<uint32_t>(static_cast<int32_t>(axes[a].def * 65536.0)));
        put32(out, rec + 12, static_cast<uint32_t>(static_cast<int32_t>(axes[a].max * 65536.0)));
    }
    return out;
}

/// A bare sfnt holding only `tables` (enough for the name-table scan, not for FreeType).
std::vector<uint8_t> sfnt(const std::map<std::string, std::vector<uint8_t>>& tables) {
    std::vector<uint8_t> out(12 + 16 * tables.size(), 0);
    put32(out, 0, 0x00010000u);
    put16(out, 4, static_cast<uint32_t>(tables.size()));
    size_t i = 0;
    for (const auto& [tag, bytes] : tables) {
        while (out.size() % 4 != 0) out.push_back(0);
        const size_t rec = 12 + 16 * i;
        put32(out, rec, tagOf(tag.c_str()));
        put32(out, rec + 8, static_cast<uint32_t>(out.size()));
        put32(out, rec + 12, static_cast<uint32_t>(bytes.size()));
        out.insert(out.end(), bytes.begin(), bytes.end());
        ++i;
    }
    return out;
}

/// Names of a synthesized face: nameID 1, 2, 16, 17 (16/17 left out when empty).
struct Names {
    std::string id1, id2, id16, id17;
};

std::map<uint16_t, std::string> nameMap(const Names& n) {
    std::map<uint16_t, std::string> names = {{1, n.id1}, {2, n.id2}, {6, "XtSynth-" + n.id2}};
    if (!n.id16.empty()) names[16] = n.id16;
    if (!n.id17.empty()) names[17] = n.id17;
    return names;
}

/// A copy of the bundled font `base` that FreeType opens as usual, with its `name` table replaced
/// by `names` (appended at the end of the file, the directory record repointed) and its OS/2
/// usWeightClass / usWidthClass set to `weight` / `widthClass`.
std::vector<uint8_t> patchedFace(const char* base, const Names& names, uint16_t weight, uint16_t widthClass) {
    std::vector<uint8_t> font = readAll(assetFont(base));
    REQUIRE(font.size() > 12);
    const std::vector<uint8_t> name = nameTable(nameMap(names));
    const uint32_t tables = get16(font, 4);
    bool renamed = false, patched = false;
    for (uint32_t t = 0; t < tables; ++t) {
        const size_t rec = 12 + 16 * static_cast<size_t>(t);
        const uint32_t tag = get32(font, rec);
        if (tag == tagOf("name")) {
            while (font.size() % 4 != 0) font.push_back(0);
            put32(font, rec + 8, static_cast<uint32_t>(font.size()));
            put32(font, rec + 12, static_cast<uint32_t>(name.size()));
            font.insert(font.end(), name.begin(), name.end());
            renamed = true;
        } else if (tag == tagOf("OS/2")) {
            const size_t at = get32(font, rec + 8);
            put16(font, at + 4, weight);
            put16(font, at + 6, widthClass);
            patched = true;
        }
    }
    REQUIRE(renamed);
    REQUIRE(patched);
    return font;
}

/// A fresh, empty temp directory for one test's fonts.
fs::path freshDir(const char* name) {
    const fs::path dir = xt::test::tempDir() / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

struct FaceFile {
    const char* file;
    const char* base;
    Names names;
    uint16_t weight;
    uint16_t widthClass;
};

void writeFaces(const fs::path& dir, const std::vector<FaceFile>& faces) {
    for (const FaceFile& f : faces) writeAll(dir / f.file, patchedFace(f.base, f.names, f.weight, f.widthClass));
}

/// An `Arial`-like family: the four Narrow faces (nameID 1 `Xt Arial Narrow`, usWidthClass 3) sort
/// and register before the four normal ones, as ARIALN*.TTF do before arial*.ttf.
const std::vector<FaceFile> kNarrowFaces = {
    {"a_narrow.ttf", "NotoSans-Regular.ttf", {"Xt Arial Narrow", "Regular", "Xt Arial", "Narrow"}, 400, 3},
    {"b_narrow_bold.ttf", "NotoSans-Bold.ttf", {"Xt Arial Narrow", "Bold", "Xt Arial", "Narrow Bold"}, 700, 3},
    {"c_narrow_italic.ttf", "NotoSans-Italic.ttf", {"Xt Arial Narrow", "Italic", "Xt Arial", "Narrow Italic"}, 400, 3},
    {"d_narrow_bold_italic.ttf",
     "NotoSans-BoldItalic.ttf",
     {"Xt Arial Narrow", "Bold Italic", "Xt Arial", "Narrow Bold Italic"},
     700,
     3},
};
const std::vector<FaceFile> kNormalFaces = {
    {"e_regular.ttf", "NotoSans-Regular.ttf", {"Xt Arial", "Regular", "", ""}, 400, 5},
    {"f_bold.ttf", "NotoSans-Bold.ttf", {"Xt Arial", "Bold", "", ""}, 700, 5},
    {"g_italic.ttf", "NotoSans-Italic.ttf", {"Xt Arial", "Italic", "", ""}, 400, 5},
    {"h_bold_italic.ttf", "NotoSans-BoldItalic.ttf", {"Xt Arial", "Bold Italic", "", ""}, 700, 5},
};

/// The file name of the face `families`/`weight`/`italic` matches.
std::string matched(xt::text::FtTextShaper& shaper, std::string_view family, uint16_t weight, bool italic = false) {
    const std::vector<std::string_view> families = {family};
    const xt::FontRequest request{std::span<const std::string_view>(families), weight, italic};
    return fs::path(shaper.fontPath(shaper.match(request))).filename().generic_string();
}

/// The file name of the face shape() draws `utf8` (one character) with.
std::string shapedWith(xt::text::FtTextShaper& shaper, std::string_view family, uint16_t weight, bool italic,
                       std::string_view utf8) {
    const std::vector<std::string_view> families = {family};
    xt::ShapeParams params;
    params.font = xt::FontRequest{std::span<const std::string_view>(families), weight, italic};
    params.size = 32;
    xt::ShapeResult out;
    shaper.shape(utf8, params, out);
    if (out.runs.empty()) return std::string();
    return fs::path(shaper.fontPath(out.runs[0].font)).filename().generic_string();
}

}  // namespace

TEST_CASE("T-853 width words: whole words, case-insensitive, Semi/Extra/Ultra prefixes") {
    for (const char* name : {"Narrow", "Arial Narrow", "narrow bold", "Bold Condensed Italic", "Myriad Pro Cond",
                             "SemiCondensed", "Semi Condensed", "ExtraCondensed", "Ultra-Condensed", "UltraCompressed",
                             "Compressed", "Compact", "Light Semicondensed", "Bahnschrift SemiCondensed"}) {
        CAPTURE(name);
        CHECK(xt::text::widthWordIn(name) == WidthWord::kNarrow);
    }
    for (const char* name : {"Extended", "extended", "Expanded", "SemiExpanded", "Ultra Expanded", "Wide",
                             "Bold Extra-Wide", "UltraExpanded"}) {
        CAPTURE(name);
        CHECK(xt::text::widthWordIn(name) == WidthWord::kWide);
    }
    for (const char* name : {"", "Regular", "Light", "Bold Italic", "Semibold", "Semi", "Ultra", "Second", "Condor",
                             "Widescreen", "Narrowing", "BoldCond", "Comfortaa", "Semi Bold", "Extra Light"}) {
        CAPTURE(name);
        CHECK(xt::text::widthWordIn(name) == WidthWord::kNone);
    }
}

TEST_CASE("T-853 face width: usWidthClass only when the name says so") {
    FontNameInfo info;
    // Arial Narrow: nameID 1 carries the word, class 3.
    info.legacyFamily = "Arial Narrow";
    info.subfamily = "Regular";
    info.widthClass = 3;
    CHECK(xt::text::faceWidth(info).min == 750);
    CHECK(xt::text::faceWidth(info).max == 750);
    CHECK(xt::text::faceWidth(info).def == 750);
    // ... or only the subfamily (nameID 17) does.
    info.legacyFamily = "Myriad Pro";
    info.subfamily = "Bold Condensed";
    CHECK(xt::text::faceWidth(info).min == 750);
    // Comfortaa: no width word, class 3 or 7 -> normal width.
    info.legacyFamily = "Comfortaa";
    info.subfamily = "Light";
    CHECK(xt::text::faceWidth(info).min == 1000);
    info.widthClass = 7;
    info.subfamily = "Bold";
    CHECK(xt::text::faceWidth(info).max == 1000);
    // A width word with class 5 (or no OS/2): 75% for a narrow word, 125% for a wide one.
    info.legacyFamily = "Xt Condensed";
    info.subfamily = "Regular";
    info.widthClass = 5;
    CHECK(xt::text::faceWidth(info).min == 750);
    info.widthClass = 0;
    CHECK(xt::text::faceWidth(info).min == 750);
    info.legacyFamily = "Xt";
    info.subfamily = "Expanded";
    info.widthClass = 5;
    CHECK(xt::text::faceWidth(info).min == 1250);
    // The class -> percentage table, with a width word present.
    const uint16_t table[10] = {0, 500, 625, 750, 875, 1000, 1125, 1250, 1500, 2000};
    info.subfamily = "Condensed";
    for (uint16_t c = 1; c <= 9; ++c) {
        CAPTURE(c);
        info.widthClass = c;
        if (c != 5) CHECK(xt::text::faceWidth(info).min == table[c]);
    }
    // A `wdth` axis: the axis range, whatever the names say.
    FontNameInfo variable;
    variable.legacyFamily = "Bahnschrift";
    variable.subfamily = "Regular";
    variable.widthClass = 5;
    variable.variableWidth = true;
    variable.widthMin = 750;
    variable.widthDefault = 1000;
    variable.widthMax = 1000;
    const FaceWidth w = xt::text::faceWidth(variable);
    CHECK(w.min == 750);
    CHECK(w.def == 1000);
    CHECK(w.max == 1000);
}

TEST_CASE("T-853 scan: nameID 1/2/16/17, usWidthClass and the wdth axis after wght") {
    const std::vector<uint8_t> bytes = sfnt({
        {"name", nameTable({{1, "Xt Scan Narrow"}, {2, "Bold"}, {6, "XtScan-NarrowBold"}, {16, "Xt Scan"},
                            {17, "Narrow Bold"}})},
        {"OS/2", os2Table(700, 3)},
        {"fvar", fvarTable({{"wght", 300, 400, 700}, {"wdth", 75, 100, 100}})},
    });
    FontNameInfo info;
    REQUIRE(xt::text::readFontNameTable(std::span<const uint8_t>(bytes), info));
    CHECK(info.family == "Xt Scan");
    CHECK(info.subfamily == "Narrow Bold");
    CHECK(info.legacyFamily == "Xt Scan Narrow");
    CHECK(info.legacySubfamily == "Bold");
    CHECK(info.postScriptName == "XtScan-NarrowBold");
    CHECK(info.weightClass == 700);
    CHECK(info.widthClass == 3);
    CHECK(info.variableWeight);
    CHECK(info.weightMin == 300);
    CHECK(info.weightMax == 700);
    CHECK(info.variableWidth);  // the `wght` axis came first; the scan does not stop there
    CHECK(info.widthMin == 750);
    CHECK(info.widthDefault == 1000);
    CHECK(info.widthMax == 1000);

    // The same bytes read from a file.
    const fs::path dir = freshDir("xt_t853_scan");
    writeAll(dir / "scan.ttf", bytes);
    FontNameInfo fromFile;
    REQUIRE(xt::text::readFontNameTable((dir / "scan.ttf").generic_string(), fromFile));
    CHECK(fromFile.legacyFamily == "Xt Scan Narrow");
    CHECK(fromFile.widthClass == 3);
    CHECK(fromFile.variableWidth);
    CHECK(fromFile.widthMin == 750);

    // No nameID 16/17: family and subfamily are the legacy pair; no fvar: no wdth axis.
    const std::vector<uint8_t> plain =
        sfnt({{"name", nameTable({{1, "Xt Plain"}, {2, "Italic"}})}, {"OS/2", os2Table(400, 5, 0x0001)}});
    FontNameInfo p;
    REQUIRE(xt::text::readFontNameTable(std::span<const uint8_t>(plain), p));
    CHECK(p.family == "Xt Plain");
    CHECK(p.legacyFamily == "Xt Plain");
    CHECK(p.subfamily == "Italic");
    CHECK(p.legacySubfamily == "Italic");
    CHECK(p.widthClass == 5);
    CHECK(p.italic);
    CHECK_FALSE(p.variableWidth);
    // A truncated buffer is not a font.
    FontNameInfo none;
    CHECK_FALSE(xt::text::readFontNameTable(std::span<const uint8_t>(plain.data(), 10), none));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("T-853 matching: a family with normal and narrow faces gives the normal ones") {
    const fs::path dir = freshDir("xt_t853_mixed");
    writeFaces(dir, kNarrowFaces);
    writeFaces(dir, kNormalFaces);
    xt::text::FtTextShaper shaper;
    REQUIRE(shaper.addFontDir(dir.generic_string(), 64) == 8);
    CHECK(matched(shaper, "Xt Arial", 400) == "e_regular.ttf");
    CHECK(matched(shaper, "xt arial", 400) == "e_regular.ttf");
    CHECK(matched(shaper, "Xt Arial", 700) == "f_bold.ttf");
    CHECK(matched(shaper, "Xt Arial", 900) == "f_bold.ttf");
    CHECK(matched(shaper, "Xt Arial", 400, true) == "g_italic.ttf");
    CHECK(matched(shaper, "Xt Arial", 700, true) == "h_bold_italic.ttf");
    // The legacy family name finds the narrow faces only.
    CHECK(matched(shaper, "Xt Arial Narrow", 400) == "a_narrow.ttf");
    CHECK(matched(shaper, "XT ARIAL NARROW", 700) == "b_narrow_bold.ttf");
    CHECK(matched(shaper, "Xt Arial Narrow", 400, true) == "c_narrow_italic.ttf");
    CHECK(matched(shaper, "Xt Arial Narrow", 700, true) == "d_narrow_bold_italic.ttf");
    // shape()'s font-fallback segmentation goes through the same match.
    CHECK(shapedWith(shaper, "Xt Arial", 400, false, "H") == "e_regular.ttf");
    CHECK(shapedWith(shaper, "Xt Arial", 700, true, "H") == "h_bold_italic.ttf");
    CHECK(shapedWith(shaper, "Xt Arial Narrow", 400, false, "H") == "a_narrow.ttf");
    // --list-fonts: the legacy name of a face whose typographic family differs.
    std::vector<xt::FontDesc> fonts;
    shaper.fonts(fonts);
    REQUIRE(fonts.size() == 8);
    for (const xt::FontDesc& f : fonts) {
        const std::string file = fs::path(shaper.fontPath(f.id)).filename().generic_string();
        CAPTURE(file);
        CHECK(f.family == "Xt Arial");
        CHECK(shaper.fontLegacyFamily(f.id) == (file[0] <= 'd' ? "Xt Arial Narrow" : ""));
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("T-853 matching: a family of narrow faces only still gives them") {
    const fs::path dir = freshDir("xt_t853_narrow_only");
    writeFaces(dir, kNarrowFaces);
    xt::text::FtTextShaper shaper;
    REQUIRE(shaper.addFontDir(dir.generic_string(), 64) == 4);
    CHECK(matched(shaper, "Xt Arial", 400) == "a_narrow.ttf");
    CHECK(matched(shaper, "Xt Arial", 700) == "b_narrow_bold.ttf");
    CHECK(matched(shaper, "Xt Arial", 400, true) == "c_narrow_italic.ttf");
    CHECK(matched(shaper, "Xt Arial Narrow", 700, true) == "d_narrow_bold_italic.ttf");
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("T-853 matching: faces registered from memory follow the same rules") {
    xt::text::FtTextShaper shaper;
    for (const std::vector<FaceFile>* set : {&kNarrowFaces, &kNormalFaces}) {
        for (const FaceFile& f : *set) {
            REQUIRE(shaper.addFontMemory(patchedFace(f.base, f.names, f.weight, f.widthClass)));
        }
    }
    const auto weightAt = [&](std::string_view family, uint16_t weight, bool italic) {
        const std::vector<std::string_view> families = {family};
        return shaper.match(xt::FontRequest{std::span<const std::string_view>(families), weight, italic});
    };
    // Registration order: ids 0..3 narrow, 4..7 normal.
    CHECK(weightAt("Xt Arial", 400, false) == 4);
    CHECK(weightAt("Xt Arial", 700, false) == 5);
    CHECK(weightAt("Xt Arial", 400, true) == 6);
    CHECK(weightAt("Xt Arial Narrow", 400, false) == 0);
    CHECK(weightAt("Xt Arial Narrow", 700, true) == 3);
    CHECK(shaper.fontLegacyFamily(1) == "Xt Arial Narrow");
    CHECK(shaper.fontLegacyFamily(5) == "");
}

TEST_CASE("T-853 matching: 'X Semibold' at 400 gives Semibold; a typographic family beats a legacy one") {
    const fs::path dir = freshDir("xt_t853_semibold");
    writeFaces(dir, {
                        // Registered first: another family whose legacy name collides with "Xt Segoe".
                        {"a_collide.ttf", "NotoSans-Bold.ttf", {"Xt Segoe", "Regular", "Xt Other", "Black"}, 900, 5},
                        {"b_regular.ttf", "NotoSans-Regular.ttf", {"Xt Segoe", "Regular", "", ""}, 400, 5},
                        {"c_semibold.ttf",
                         "NotoSans-Bold.ttf",
                         {"Xt Segoe Semibold", "Regular", "Xt Segoe", "Semibold"},
                         600,
                         5},
                        {"d_semibold_italic.ttf",
                         "NotoSans-BoldItalic.ttf",
                         {"Xt Segoe Semibold", "Italic", "Xt Segoe", "Semibold Italic"},
                         600,
                         5},
                        {"e_bold.ttf", "NotoSans-Bold.ttf", {"Xt Segoe", "Bold", "", ""}, 700, 5},
                    });
    xt::text::FtTextShaper shaper;
    REQUIRE(shaper.addFontDir(dir.generic_string(), 64) == 5);
    CHECK(matched(shaper, "Xt Segoe Semibold", 400) == "c_semibold.ttf");
    CHECK(matched(shaper, "Xt Segoe Semibold", 700) == "c_semibold.ttf");
    CHECK(matched(shaper, "Xt Segoe Semibold", 400, true) == "d_semibold_italic.ttf");
    // "Xt Segoe" is the typographic family of b..e and the legacy family of a: b..e win.
    CHECK(matched(shaper, "Xt Segoe", 400) == "b_regular.ttf");
    CHECK(matched(shaper, "Xt Segoe", 600) == "c_semibold.ttf");
    CHECK(matched(shaper, "Xt Segoe", 900) == "e_bold.ttf");
    CHECK(matched(shaper, "Xt Other", 400) == "a_collide.ttf");
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("T-853 matching: an untrusted usWidthClass does not exclude a weight (Comfortaa)") {
    const fs::path dir = freshDir("xt_t853_comfy");
    writeFaces(dir, {
                        {"a_bold.ttf", "NotoSans-Bold.ttf", {"Xt Comfy", "Bold", "", ""}, 700, 7},
                        {"b_light.ttf", "NotoSans-Regular.ttf", {"Xt Comfy", "Light", "", ""}, 300, 3},
                        {"c_regular.ttf", "NotoSans-Regular.ttf", {"Xt Comfy", "Regular", "", ""}, 400, 5},
                    });
    xt::text::FtTextShaper shaper;
    REQUIRE(shaper.addFontDir(dir.generic_string(), 64) == 3);
    CHECK(matched(shaper, "Xt Comfy", 300) == "b_light.ttf");
    CHECK(matched(shaper, "Xt Comfy", 400) == "c_regular.ttf");
    CHECK(matched(shaper, "Xt Comfy", 700) == "a_bold.ttf");
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("T-853 matching: a wdth range containing 100% beats a static narrow face") {
    // Scan-only faces (FreeType cannot open them; opening the match is expected to fail and log).
    const fs::path dir = freshDir("xt_t853_wdth");
    writeAll(dir / "a_condensed.ttf",
             sfnt({{"name", nameTable({{1, "Xt Var Condensed"}, {2, "Regular"}, {16, "Xt Var"}, {17, "Condensed"}})},
                   {"OS/2", os2Table(400, 3)}}));
    writeAll(dir / "b_variable.ttf", sfnt({{"name", nameTable({{1, "Xt Var"}, {2, "Regular"}})},
                                           {"OS/2", os2Table(400, 5)},
                                           {"fvar", fvarTable({{"wdth", 50, 50, 100}})}}));
    {
        xt::text::FtTextShaper shaper;
        REQUIRE(shaper.addFontDir(dir.generic_string(), 64) == 2);
        CHECK(shaper.fontLegacyFamily(0) == "Xt Var Condensed");
        CHECK(shaper.fontLegacyFamily(1) == "");
        CHECK(matched(shaper, "Xt Var", 400) == "b_variable.ttf");
    }
    {
        xt::text::FtTextShaper shaper;
        REQUIRE(shaper.addFontDir(dir.generic_string(), 64) == 2);
        CHECK(matched(shaper, "Xt Var Condensed", 400) == "a_condensed.ttf");
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}
