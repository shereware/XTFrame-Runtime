#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "font_subset.h"
#include "test_temp.h"
#include "xt/bundle/bundle.h"
#include "xt/bundle/project_file.h"
#include "xt/bundle/ui_project.h"
#include "xt/ir_pod.h"
#include "yaml_lite.h"

/// T-257 (Docs/tasks/T-257.md F13, F20, F6): the YAML-lite reader bounds its nesting, and the font
/// subsetter survives composite chains, huge cmap format 12 groups and a cmap offset that would
/// wrap a 32-bit sum. The fonts are written byte by byte here: just the `cmap`, `head`, `loca`,
/// `maxp` and `glyf` tables FontFile reads.

using namespace xt::bundle;

namespace {

std::string repeat(std::string_view text, int count) {
    std::string out;
    out.reserve(text.size() * static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        out += text;
    }
    return out;
}

bool yamlFails(const std::string& text, std::string& error) {
    nlohmann::json out;
    error.clear();
    return !detail::parseYamlLite(text, out, &error);
}

void put16(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

void put32(std::vector<uint8_t>& out, uint32_t v) {
    put16(out, v >> 16);
    put16(out, v & 0xFFFF);
}

constexpr uint32_t tag(const char (&name)[5]) {
    return (static_cast<uint32_t>(name[0]) << 24) | (static_cast<uint32_t>(name[1]) << 16) |
           (static_cast<uint32_t>(name[2]) << 8) | static_cast<uint32_t>(name[3]);
}

struct SfntTable {
    uint32_t tag = 0;
    std::vector<uint8_t> data;
};

/// An sfnt with the given tables (checksums left zero: FontFile never checks them).
std::vector<uint8_t> buildSfnt(const std::vector<SfntTable>& tables) {
    std::vector<uint8_t> out;
    put32(out, 0x00010000u);
    put16(out, static_cast<uint32_t>(tables.size()));
    put16(out, 0);
    put16(out, 0);
    put16(out, 0);
    uint32_t offset = 12 + 16 * static_cast<uint32_t>(tables.size());
    for (const SfntTable& t : tables) {
        put32(out, t.tag);
        put32(out, 0);
        put32(out, offset);
        put32(out, static_cast<uint32_t>(t.data.size()));
        offset += (static_cast<uint32_t>(t.data.size()) + 3u) & ~3u;
    }
    for (const SfntTable& t : tables) {
        out.insert(out.end(), t.data.begin(), t.data.end());
        while ((out.size() & 3) != 0) {
            out.push_back(0);
        }
    }
    return out;
}

struct Group {
    uint32_t start = 0;
    uint32_t end = 0;
    uint32_t gid = 0;
};

/// A cmap format 12 subtable.
std::vector<uint8_t> format12(const std::vector<Group>& groups) {
    std::vector<uint8_t> out;
    put16(out, 12);
    put16(out, 0);
    put32(out, 16 + 12 * static_cast<uint32_t>(groups.size()));
    put32(out, 0);
    put32(out, static_cast<uint32_t>(groups.size()));
    for (const Group& g : groups) {
        put32(out, g.start);
        put32(out, g.end);
        put32(out, g.gid);
    }
    return out;
}

/// A cmap table whose encoding records (all platform 3, encoding 10) point at `offsets`; the
/// subtable `sub` follows the records.
std::vector<uint8_t> cmapTable(const std::vector<uint32_t>& offsets, const std::vector<uint8_t>& sub) {
    std::vector<uint8_t> out;
    put16(out, 0);
    put16(out, static_cast<uint32_t>(offsets.size()));
    for (uint32_t offset : offsets) {
        put16(out, 3);
        put16(out, 10);
        put32(out, offset);
    }
    out.insert(out.end(), sub.begin(), sub.end());
    return out;
}

/// The offset of the subtable cmapTable() puts after `records` encoding records.
uint32_t subtableOffset(size_t records) { return 4 + 8 * static_cast<uint32_t>(records); }

/// A TrueType font with `glyphs` (each one's `glyf` bytes, empty for no outline) and `cmap`.
std::vector<uint8_t> buildFont(const std::vector<std::vector<uint8_t>>& glyphs,
                               const std::vector<uint8_t>& cmap) {
    std::vector<uint8_t> head(54, 0);
    head[51] = 1;  // indexToLocFormat: long offsets
    std::vector<uint8_t> maxp;
    put32(maxp, 0x00005000u);
    put16(maxp, static_cast<uint32_t>(glyphs.size()));
    std::vector<uint8_t> loca;
    std::vector<uint8_t> glyf;
    for (const std::vector<uint8_t>& g : glyphs) {
        put32(loca, static_cast<uint32_t>(glyf.size()));
        glyf.insert(glyf.end(), g.begin(), g.end());
    }
    put32(loca, static_cast<uint32_t>(glyf.size()));
    return buildSfnt({SfntTable{tag("cmap"), cmap}, SfntTable{tag("glyf"), glyf},
                      SfntTable{tag("head"), head}, SfntTable{tag("loca"), loca},
                      SfntTable{tag("maxp"), maxp}});
}

/// A simple glyph with no contours (10 bytes: the header only).
std::vector<uint8_t> simpleGlyph() { return std::vector<uint8_t>(10, 0); }

/// A composite glyph of `components` (byte arguments, no transform).
std::vector<uint8_t> compositeGlyph(const std::vector<uint16_t>& components) {
    std::vector<uint8_t> out;
    put16(out, 0xFFFF);  // numberOfContours = -1
    out.insert(out.end(), 8, 0);
    for (size_t i = 0; i < components.size(); ++i) {
        put16(out, i + 1 < components.size() ? 0x0020 : 0x0000);  // MORE_COMPONENTS
        put16(out, components[i]);
        out.push_back(0);
        out.push_back(0);
    }
    return out;
}

std::vector<uint8_t> cmapOf(const std::vector<Group>& groups) {
    return cmapTable({subtableOffset(1)}, format12(groups));
}

}  // namespace

TEST_CASE("T-257 F13: a YAML line with 1000 '[' is a nesting error") {
    std::string error;
    REQUIRE(yamlFails("key: " + repeat("[", 1000), error));
    CHECK(error == "line 1: nesting too deep");
}

TEST_CASE("T-257 F13: a YAML line with 200000 '[' or '{' fails without overflowing the stack") {
    std::string error;
    REQUIRE(yamlFails(repeat("[", 200000), error));
    CHECK(error == "line 1: nesting too deep");
    REQUIRE(yamlFails("key: " + repeat("{a: ", 200000), error));
    CHECK(error == "line 1: nesting too deep");
}

TEST_CASE("T-257 F13: block nesting 100 levels deep is a nesting error") {
    std::string text;
    for (int i = 0; i < 100; ++i) {
        text += std::string(static_cast<size_t>(i) * 2, ' ') + "k" + std::to_string(i) + ":\n";
    }
    text += std::string(200, ' ') + "leaf: 1\n";
    std::string error;
    REQUIRE(yamlFails(text, error));
    CHECK(error.find("nesting too deep") != std::string::npos);
}

TEST_CASE("T-257 F13: moderate YAML nesting still parses") {
    nlohmann::json out;
    std::string error;
    REQUIRE(detail::parseYamlLite(
        "key: " + repeat("[", 20) + "1" + repeat("]", 20) + "\nother:\n  a:\n    b: 2\n", out, &error));
    CHECK(out["other"]["a"]["b"] == 2);
}

namespace {

/// `levels` nested block mappings (the top-level one included), the innermost holding `leaf: 1`.
std::string blockNesting(int levels) {
    std::string text;
    for (int i = 0; i + 1 < levels; ++i) {
        text += std::string(static_cast<size_t>(i) * 2, ' ') + "k" + std::to_string(i) + ":\n";
    }
    return text + std::string(static_cast<size_t>(levels - 1) * 2, ' ') + "leaf: 1\n";
}

}  // namespace

TEST_CASE("T-257b: YAML nested exactly 64 deep parses and is destroyed; 65 is an error") {
    // In Debug on the test thread's default stack: the parse, and the nlohmann::json destructor
    // (recursive) of the 64-deep result.
    {
        nlohmann::json out;
        std::string error;
        CHECK_MESSAGE(
            detail::parseYamlLite("key: " + repeat("[", 64) + "1" + repeat("]", 64) + "\n", out, &error),
            error);
    }
    {
        nlohmann::json out;
        std::string error;
        CHECK_MESSAGE(detail::parseYamlLite(blockNesting(64), out, &error), error);
    }
    std::string error;
    REQUIRE(yamlFails("key: " + repeat("[", 65) + "1" + repeat("]", 65) + "\n", error));
    CHECK(error == "line 1: nesting too deep");
    REQUIRE(yamlFails(blockNesting(65), error));
    CHECK(error.find("nesting too deep") != std::string::npos);
}

TEST_CASE("T-257 F20: a 60000-glyph composite chain subsets without recursion") {
    constexpr uint16_t kGlyphs = 60000;
    std::vector<std::vector<uint8_t>> glyphs;
    glyphs.reserve(kGlyphs);
    glyphs.emplace_back();  // .notdef, no outline
    for (uint16_t i = 1; i + 1 < kGlyphs; ++i) {
        glyphs.push_back(compositeGlyph({static_cast<uint16_t>(i + 1)}));
    }
    glyphs.push_back(simpleGlyph());
    FontFile font;
    REQUIRE(font.parse(buildFont(glyphs, cmapOf({Group{0x41, 0x41, 1}}))));
    CHECK(font.outlineCount() == kGlyphs - 1u);

    std::vector<uint8_t> subset;
    REQUIRE(font.subset({0x41}, subset));
    FontFile reread;
    REQUIRE(reread.parse(subset));
    CHECK(reread.outlineCount() == kGlyphs - 1u);  // the whole chain is reachable from 'A'
}

TEST_CASE("T-257 F20: a self-referencing composite keeps itself and its other component") {
    const std::vector<std::vector<uint8_t>> glyphs = {
        {}, compositeGlyph({1, 2}), simpleGlyph(), simpleGlyph()};
    FontFile font;
    REQUIRE(font.parse(buildFont(glyphs, cmapOf({Group{0x41, 0x41, 1}}))));
    std::vector<uint8_t> subset;
    REQUIRE(font.subset({0x41}, subset));
    FontFile reread;
    REQUIRE(reread.parse(subset));
    CHECK(reread.outlineCount() == 2);  // glyphs 1 and 2; glyph 3 is unused
}

TEST_CASE("T-257 F20: format 12 groups spanning all of Unicode stop at the glyph count") {
    std::vector<Group> groups(2000, Group{0, 0x10FFFF, 1});
    const std::vector<std::vector<uint8_t>> glyphs = {{}, simpleGlyph(), simpleGlyph()};
    FontFile font;
    REQUIRE(font.parse(buildFont(glyphs, cmapOf(groups))));
    CHECK(font.hasGlyph(0));
    CHECK(font.hasGlyph(1));
    CHECK_FALSE(font.hasGlyph(2));
}

TEST_CASE("T-257 F6: a cmap encoding record offset near UINT32_MAX is skipped, not read") {
    const std::vector<uint8_t> sub = format12({Group{0x41, 0x41, 1}});
    const std::vector<std::vector<uint8_t>> glyphs = {{}, simpleGlyph()};

    FontFile onlyBad;
    CHECK_FALSE(onlyBad.parse(buildFont(glyphs, cmapTable({0xFFFFFFFEu}, sub))));

    FontFile badThenGood;
    REQUIRE(badThenGood.parse(buildFont(glyphs, cmapTable({0xFFFFFFFEu, subtableOffset(2)}, sub))));
    CHECK(badThenGood.hasGlyph(0x41));
}

// T-262c (Docs/tasks/T-262c.md): ~1100 nested arrays killed the process with a stack overflow.
// Located by bisection in this Debug build: nlohmann 3.11.3's parser and destructor are both
// iterative (100000 levels parse and destruct fine), but its copy constructor (overflow between 600
// and 1100 levels) and dump() (between 300 and 600) recurse, and our walk reaches both --
// UiProject::load copies every unknown key into the project's extra JSON and dumps it. Bundle::open
// itself survived 100000 levels, so the 1100-level .xtui case is the one that crashed here. Every
// JSON input is now rejected beyond 32 levels of nesting before any object is built.

namespace {

/// A fresh directory `xt_limits_<name>` under the test temp root (removed first).
std::filesystem::path freshDir(const std::string& name) {
    const std::filesystem::path dir = xt::test::tempDir() / ("xt_limits_" + name);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary);
    file << text;
}

/// `levels` nested arrays, `[[[...]]]`.
std::string nestedArrays(int levels) { return repeat("[", levels) + repeat("]", levels); }

/// A minimal valid manifest whose `extra` key holds `arrays` nested arrays, so the whole document
/// is `arrays + 1` levels deep.
std::string manifestWithNesting(int arrays) {
    return "{\"version\": 1, \"bundle\": {\"id\": \"deep\", \"irMajor\": " +
           std::to_string(static_cast<int>(xt::kIrFormatMajor)) +
           "}, \"uis\": [], \"extra\": " + nestedArrays(arrays) + "}";
}

}  // namespace

TEST_CASE("T-262c: Bundle::open rejects a 1100-level manifest and the process lives") {
    const std::filesystem::path dir = freshDir("manifest_1100");
    writeText(dir / "manifest.json", nestedArrays(1100));
    std::string error;
    auto bundle = Bundle::open(dir.generic_string(), nullptr, &error);
    REQUIRE_FALSE(bundle.ok());
    CHECK(bundle.status() == xt::Status::kInvalidArgument);
    CHECK(error == "manifest nesting too deep");
}

TEST_CASE("T-262c: Bundle::open rejects 1100 nested arrays under a manifest key") {
    const std::filesystem::path dir = freshDir("manifest_key_1100");
    writeText(dir / "manifest.json", manifestWithNesting(1100));
    std::string error;
    auto bundle = Bundle::open(dir.generic_string(), nullptr, &error);
    REQUIRE_FALSE(bundle.ok());
    CHECK(bundle.status() == xt::Status::kInvalidArgument);
    CHECK(error == "manifest nesting too deep");
}

TEST_CASE("T-262c: a manifest 32 levels deep opens; 33 levels is rejected") {
    const std::filesystem::path dir = freshDir("manifest_32");
    writeText(dir / "manifest.json", manifestWithNesting(31));
    std::string error;
    auto bundle = Bundle::open(dir.generic_string(), nullptr, &error);
    CHECK(bundle.ok());
    CHECK(error.empty());

    writeText(dir / "manifest.json", manifestWithNesting(32));
    auto deeper = Bundle::open(dir.generic_string(), nullptr, &error);
    REQUIRE_FALSE(deeper.ok());
    CHECK(deeper.status() == xt::Status::kInvalidArgument);
    CHECK(error == "manifest nesting too deep");
}

TEST_CASE("T-262c: a 1100-level .xtui is rejected and the process lives") {
    const std::filesystem::path dir = freshDir("xtui_1100");
    writeText(dir / "Deep.xtui",
              "{\"version\": 1, \"id\": \"deep\", \"uis\": [], \"extra\": " + nestedArrays(1100) + "}");
    std::string error;
    xt::Result<UiProject> project = UiProject::load(dir.generic_string(), &error);
    REQUIRE_FALSE(project.ok());
    CHECK(project.status() == xt::Status::kInvalidArgument);
    CHECK(error == (dir / "Deep.xtui").generic_string() + ": nesting too deep");
}

TEST_CASE("T-262c: a 1100-level bundle.xtproj is rejected and the process lives") {
    const std::filesystem::path dir = freshDir("xtproj_1100");
    writeText(dir / "bundle.xtproj", "{\"version\": 2, \"extra\": " + nestedArrays(1100) + "}");
    std::string error;
    xt::Result<ProjectFile> project = ProjectFile::load(dir.generic_string(), &error);
    REQUIRE_FALSE(project.ok());
    CHECK(project.status() == xt::Status::kInvalidArgument);
    CHECK(error == (dir / "bundle.xtproj").generic_string() + ": nesting too deep");
}
