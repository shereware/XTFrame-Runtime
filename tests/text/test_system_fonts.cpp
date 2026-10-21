#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "doctest.h"
#include "font_name_table.h"  // text/src/font_name_table.h
#include "test_temp.h"
#include "xt/text/ft_text_shaper.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

/// Tests for FtTextShaper::addSystemFonts() (docs/design/23-host-integration.md §23.4,
/// docs/tasks/T-97.md "測試", docs/tasks/T-104.md "測試"). These read the host's real font
/// directories, so they assert only what every supported platform guarantees: some fonts exist,
/// and the budget is respected.

TEST_CASE("addSystemFonts: registers the platform's fonts and reports how many") {
    xt::text::FtTextShaper shaper;
    CHECK(shaper.fontCount() == 0);

    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    const uint32_t added = shaper.addSystemFonts();
    const double elapsedMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("addSystemFonts: %u fonts in %.1f ms\n", added, elapsedMs);

    CHECK(added > 0);
    CHECK(shaper.fontCount() == added);
    // docs/tasks/T-104.md test 1: scanning registers fonts without opening any of them.
    CHECK(shaper.openedFontCount() == 0);
}

TEST_CASE("addSystemFonts: honours the budget") {
    xt::text::FtTextShaper shaper;
    const uint32_t added = shaper.addSystemFonts(3);
    CHECK(added <= 3);
    CHECK(shaper.fontCount() == added);
}

TEST_CASE("addFontDir: a missing directory registers nothing") {
    xt::text::FtTextShaper shaper;
    CHECK(shaper.addFontDir("/xt-no-such-font-directory", 16) == 0);
    CHECK(shaper.fontCount() == 0);
}

TEST_CASE("addSystemFonts: match() by family opens exactly that one deferred font") {
    xt::text::FtTextShaper shaper;
    REQUIRE(shaper.addSystemFonts() > 0);
    CHECK(shaper.openedFontCount() == 0);

#if defined(_WIN32)
    const char* wantedFamily = "Arial";
#elif defined(__APPLE__)
    const char* wantedFamily = "Helvetica";
#else
    const char* wantedFamily = "DejaVu Sans";
#endif
    const std::vector<std::string_view> families = {wantedFamily};
    const xt::FontId id =
        shaper.match(xt::FontRequest{std::span<const std::string_view>(families), 400, false});
    CHECK(id != xt::kInvalidFont);
    // docs/tasks/T-104.md test 2: a family match opens that one font, nothing else.
    CHECK(shaper.openedFontCount() == 1);
}

TEST_CASE("addSystemFonts: CJK fallback opens far fewer fonts than are registered") {
    xt::text::FtTextShaper shaper;
    const uint32_t added = shaper.addSystemFonts();
    REQUIRE(added > 0);

    // A Latin-only family request forces shape()'s font-fallback segmentation (07.3 step 3) to
    // walk the (lazily-opened) system-fallback list for the CJK characters -- addFontDir() already
    // registered every system font as a fallback candidate.
#if defined(_WIN32)
    const char* latinFamily = "Arial";
#elif defined(__APPLE__)
    const char* latinFamily = "Helvetica";
#else
    const char* latinFamily = "DejaVu Sans";
#endif
    const std::vector<std::string_view> families = {latinFamily};
    xt::ShapeParams params;
    params.font = xt::FontRequest{std::span<const std::string_view>(families), 400, false};
    params.size = 16;

    xt::ShapeResult out;
    // "Hello 世界" -- Latin text followed by two CJK Unified Ideographs the requested family
    // doesn't cover.
    shaper.shape("Hello \xE4\xB8\x96\xE7\x95\x8C", params, out);

    REQUIRE(out.runs.size() >= 1);
    std::printf("CJK fallback: opened %u of %u registered fonts\n", shaper.openedFontCount(), added);
    // docs/tasks/T-168.md point 4: a host with no CJK font at all (a bare WSL image) has to try --
    // and so open -- every registered font before giving up, which is not what this case measures.
    // The ideographs come back as .notdef (glyph 0), or not at all, exactly then.
    constexpr uint32_t kCjkByteStart = 6;  // "Hello " is 6 UTF-8 bytes.
    uint32_t cjkGlyphs = 0;
    bool cjkMissing = false;
    for (const xt::ShapedGlyph& glyph : out.glyphs) {
        if (glyph.cluster >= kCjkByteStart) {
            ++cjkGlyphs;
            cjkMissing = cjkMissing || glyph.glyphId == 0;
        }
    }
    if (cjkGlyphs == 0 || cjkMissing) {
        MESSAGE("skipped: no system font covers U+4E16/U+754C, so every registered font was tried");
        return;
    }
    // docs/tasks/T-104.md test 3: fallback opens well under the full registered set.
    CHECK(shaper.openedFontCount() < 50);
    CHECK(shaper.openedFontCount() < added);
}

TEST_CASE("font_name_table: addFontFile's reported family/PostScript match a direct name-table read") {
    static const char* const kFiles[] = {
        "NotoSans-Regular.subset.ttf",        "NotoSansSC-Regular.subset.ttf",
        "NotoNaskhArabic-Regular.subset.ttf", "NotoSansHebrew-Regular.subset.ttf",
        "NotoSansThai-Regular.subset.ttf",    "NotoEmoji-Regular.subset.ttf",
    };
    for (const char* name : kFiles) {
        const std::string path = std::string(XT_TEST_FONTS_DIR) + "/" + name;

        xt::text::FontNameInfo info;
        REQUIRE(xt::text::readFontNameTable(path, info));

        xt::text::FtTextShaper shaper;
        REQUIRE(shaper.addFontFile(path));
        std::vector<xt::FontDesc> descs;
        shaper.fonts(descs);
        REQUIRE(descs.size() == 1);
        CHECK(descs[0].family == info.family);
        // FontDesc doesn't carry PostScript name; readFontNameTable() itself is the only reader of
        // it in this test, so this just checks it parsed something plausible for every fixture.
        CHECK(!info.postScriptName.empty());
    }
}

TEST_CASE(
    "font files named in UTF-8 the ANSI code page can or cannot spell: name table, file, directory "
    "(T-261e)") {
    namespace fs = std::filesystem;
    const auto fromUtf8 = [](const std::string& s) {
        return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
    };
    const auto toUtf8 = [](const fs::path& p) {
        const std::u8string text = p.u8string();
        return std::string(reinterpret_cast<const char*>(text.data()), text.size());
    };
    const fs::path fixture = fs::path(XT_TEST_FONTS_DIR) / "NotoSans-Regular.subset.ttf";
    xt::text::FontNameInfo expected;
    REQUIRE(xt::text::readFontNameTable(fixture.generic_string(), expected));
    // Directory xt_t261e_字型; files 字型.ttf (CP950 spells it) and 글꼴.ttf (it does not).
    const fs::path dir = xt::test::tempDir() / fromUtf8("xt_t261e_\xE5\xAD\x97\xE5\x9E\x8B");
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    for (const std::string name : {"\xE5\xAD\x97\xE5\x9E\x8B.ttf", "\xEA\xB8\x80\xEA\xBC\xB4.ttf"}) {
        CAPTURE(name);
        const fs::path file = dir / fromUtf8(name);
        fs::copy_file(fixture, file);
        const std::string path = toUtf8(file);
        xt::text::FontNameInfo info;
        CHECK(xt::text::readFontNameTable(path, info));
        CHECK(info.family == expected.family);
        {
            xt::text::FtTextShaper shaper;
            CHECK(shaper.addFontFile(path));

            // The directory scan registers it deferred; a family match then opens it.
            xt::text::FtTextShaper scanned;
            CHECK(scanned.addFontDir(toUtf8(dir), 16) == 1);
            const std::vector<std::string_view> families = {expected.family};
            CHECK(scanned.match(xt::FontRequest{std::span<const std::string_view>(families), 400, false}) !=
                  xt::kInvalidFont);
            CHECK(scanned.openedFontCount() == 1);
        }  // The opened face holds the file; it is closed before the file is removed.
        fs::remove(file, ec);
        CHECK_FALSE(fs::exists(file));
    }
    fs::remove_all(dir, ec);
}

#if defined(_WIN32)
// T-261f item 5: a non-ASCII font path is read through an FT_Stream over openFileUtf8()'s FILE*.
// That happens when a deferred font (addFontDir()) is opened -- addFontFile() reads the whole file
// into memory and uses no stream. The stream's close callback runs when FT_Open_Face fails
// (FreeType closes a stream it was given on every failure after accepting it) and when the face is
// destroyed with the shaper: no file handle is left behind either way.
TEST_CASE("deferred fonts with a non-ASCII name: the stream's file handle is released (T-261f)") {
    namespace fs = std::filesystem;
    const auto fromUtf8 = [](const std::string& s) {
        return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
    };
    const auto toUtf8 = [](const fs::path& p) {
        const std::u8string text = p.u8string();
        return std::string(reinterpret_cast<const char*>(text.data()), text.size());
    };
    const auto handleCount = []() {
        DWORD count = 0;
        GetProcessHandleCount(GetCurrentProcess(), &count);
        return count;
    };
    const fs::path fixture = fs::path(XT_TEST_FONTS_DIR) / "NotoSans-Regular.subset.ttf";
    xt::text::FontNameInfo expected;
    REQUIRE(xt::text::readFontNameTable(fixture.generic_string(), expected));
    const fs::path dir = xt::test::tempDir() / "xt_t261f_font_stream";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "broken");
    fs::create_directories(dir / "good");
    // broken/글꼴.ttf: the fixture with its `head` table record renamed, so the name table still
    // reads (the directory scan registers it) but FreeType cannot open the face; good/字型.ttf: whole.
    const fs::path broken = dir / "broken" / fromUtf8("\xEA\xB8\x80\xEA\xBC\xB4.ttf");
    const fs::path good = dir / "good" / fromUtf8("\xE5\xAD\x97\xE5\x9E\x8B.ttf");
    {
        std::ifstream in(fixture, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        REQUIRE(bytes.size() > 12);
        const size_t tables = static_cast<size_t>((static_cast<unsigned char>(bytes[4]) << 8) |
                                                  static_cast<unsigned char>(bytes[5]));
        bool renamed = false;
        for (size_t i = 0; i < tables && 12 + i * 16 + 4 <= bytes.size(); ++i) {
            char* tag = bytes.data() + 12 + i * 16;
            if (std::string(tag, 4) == "head") {
                tag[0] = tag[1] = tag[2] = tag[3] = 'x';
                renamed = true;
            }
        }
        REQUIRE(renamed);
        std::ofstream out(broken, std::ios::binary | std::ios::trunc);
        REQUIRE(out.write(bytes.data(), static_cast<std::streamsize>(bytes.size())));
    }
    fs::copy_file(fixture, good);
    const std::vector<std::string_view> families = {expected.family};
    const xt::FontRequest request{std::span<const std::string_view>(families), 400, false};
    // Scans `sub`, matches the family (which opens the deferred font) and destroys the shaper; true
    // when the font was opened.
    const auto scanAndOpen = [&](const fs::path& sub) {
        xt::text::FtTextShaper shaper;
        if (shaper.addFontDir(toUtf8(sub), 16) != 1) {
            return false;
        }
        shaper.match(request);
        return shaper.openedFontCount() == 1;
    };
    // Once each first, so that whatever a first open sets up for good is not counted.
    CHECK_FALSE(scanAndOpen(dir / "broken"));
    CHECK(scanAndOpen(dir / "good"));

    DWORD before = handleCount();
    int opened = 0;
    for (int i = 0; i < 100; ++i) {
        opened += scanAndOpen(dir / "broken") ? 1 : 0;
    }
    CHECK(opened == 0);
    CHECK(handleCount() == before);

    before = handleCount();
    opened = 0;
    for (int i = 0; i < 100; ++i) {
        opened += scanAndOpen(dir / "good") ? 1 : 0;
    }
    CHECK(opened == 100);
    CHECK(handleCount() == before);
    // Nothing holds either file: both can be removed.
    CHECK(fs::remove(broken, ec));
    CHECK(fs::remove(good, ec));
    fs::remove_all(dir, ec);
}
#endif
