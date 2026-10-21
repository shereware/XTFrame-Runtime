#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "test_temp.h"
#include "xt/bundle/bundle.h"
#include "xt/bundle/pack.h"
#include "xt/bundle/project_file.h"
#include "xt/bundle/ui_project.h"
#include "xt/compile/compile.h"
#include "xt/compile/utf8_path.h"
#include "xt/ir.h"
#include "xt/sw/sw_png.h"

/// UI Project (docs/design/30-ui-project.md §30.1/§30.2/§30.5, 22-bundle.md §22.10,
/// 26-layers-symbols-library.md §26.3.3, 12-compiler.md §12.7; docs/tasks/T-183.md):
/// `<name>.xtui` read/write (T-214), `ui-project.json`/`bundle.xtproj` compatibility, `--upgrade`, the asset
/// manifest (every validation rule), the manifest's new fields and what never reaches a bundle.

using namespace xt;
using namespace xt::bundle;

namespace {

namespace fs = std::filesystem;

std::string sampleFixture() { return std::string(XT_TEST_BUNDLE_DIR) + "/fixtures/sample"; }

/// A fresh temp directory `xt_uiproj_<name>` (removed first).
std::string freshDir(const std::string& name) {
    const fs::path dir = xt::test::tempDir() / ("xt_uiproj_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir.generic_string();
}

/// A writable copy of the `sample` bundle.xtproj fixture.
std::string copiedSample(const std::string& name) {
    const std::string dir = freshDir(name);
    std::error_code ec;
    fs::copy(sampleFixture(), dir, fs::copy_options::recursive, ec);
    REQUIRE(ec.value() == 0);
    return dir;
}

std::string readText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeText(const std::string& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    out << text;
}

/// Writes a `w` x `h` RGBA PNG (color type 6: has an alpha channel).
void writeRgbaPng(const std::string& path, int w, int h) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 200);
    REQUIRE(sw::writePng(path, pixels.data(), w, h, w * 4));
}

/// A minimal JPEG header: SOI, then a SOF0 frame header declaring `w` x `h`.
std::string jpegBytes(int w, int h) {
    std::string b;
    const unsigned char head[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0x00,
                                  0x00, 0xFF, 0xC0, 0x00, 0x11, 0x08};
    b.append(reinterpret_cast<const char*>(head), sizeof(head));
    b += static_cast<char>((h >> 8) & 0xFF);
    b += static_cast<char>(h & 0xFF);
    b += static_cast<char>((w >> 8) & 0xFF);
    b += static_cast<char>(w & 0xFF);
    b += std::string(10, '\0');
    return b;
}

UiProject loadProject(const std::string& dir) {
    std::string error;
    Result<UiProject> project = UiProject::load(dir, &error);
    REQUIRE_MESSAGE(project.ok(), error);
    return std::move(project.value());
}

const char* kFullProject = R"({
  "version": 1,
  "name": "Star UI",
  "id": "star-ui",
  "game": "Star Game",
  "spec": {
    "platforms": [ { "name": "pc", "resolutions": [[1920, 1080], [1280, 720]] } ],
    "safeArea": { "default": 0.03, "console": 0.06 },
    "input": { "methods": ["mouse", "gamepad"], "players": 2 },
    "languages": ["en", "zh-TW"],
    "fontFallback": { "zh-TW": ["Noto Sans TC", "Arial"] },
    "budget": { "frameMs": 1.5, "effectLayers": 24, "fillRate": 3 },
    "engine": { "name": "example-engine", "version": "5.7.1" },
    "notes": "kept"
  },
  "uis": [
    { "id": "hud", "doc": "ui/hud/hud.xt", "flow": "ui/hud/hud.xtflow", "size": [1920, 1080], "tags": ["ingame"], "fit": "contain" },
    { "id": "pause", "doc": "ui/pause/pause.xt" }
  ],
  "shared": { "assetRoot": "shared", "fontDirs": ["shared/fonts"], "styles": ["shared/base.xts"], "templateDirs": [],
              "symbols": "shared/symbols", "tokens": "shared/tokens.xts", "themes": "shared/themes",
              "strings": "shared/strings", "recipes": "shared/recipes" },
  "preview": { "dpr": 2 },
  "target": "desktop",
  "publish": { "outDir": "out", "languages": ["en"], "embedImages": false, "embedFonts": true, "thumbnails": [160, 90],
               "atlas": { "enabled": true, "maxSize": 2048, "padding": 4, "mipmaps": true } },
  "owner": "art team"
}
)";

bool hasDiagnostic(const PackReport& report, const std::string& needle, bool error) {
    for (const compile::Diagnostic& d : report.diagnostics) {
        const bool isError = d.level == compile::Diagnostic::Level::Error;
        if (isError == error && d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::vector<AssetIssue> issuesFor(const std::vector<AssetIssue>& all, const std::string& id) {
    std::vector<AssetIssue> out;
    for (const AssetIssue& issue : all) {
        if (issue.id == id) {
            out.push_back(issue);
        }
    }
    return out;
}

}  // namespace

// --- <name>.xtui ---

TEST_CASE("ui-project: load reads every field") {
    const std::string dir = freshDir("load");
    writeText(dir + "/Star UI.xtui", kFullProject);
    const UiProject p = loadProject(dir);
    CHECK_FALSE(p.legacy);
    CHECK(p.legacyKind == LegacyKind::kNone);
    CHECK(p.path() == dir + "/Star UI.xtui");
    CHECK(p.dir() == dir);
    CHECK(p.version == 1);
    CHECK(p.name == "Star UI");
    CHECK(p.id == "star-ui");
    CHECK(p.game == "Star Game");
    REQUIRE(p.spec.platforms.size() == 1);
    CHECK(p.spec.platforms[0].name == "pc");
    REQUIRE(p.spec.platforms[0].resolutions.size() == 2);
    CHECK(p.spec.platforms[0].resolutions[1].x == 1280);
    CHECK(p.spec.safeAreaDefault == doctest::Approx(0.03f));
    CHECK(p.spec.safeAreaConsole == doctest::Approx(0.06f));
    CHECK(p.spec.inputMethods == std::vector<std::string>{"mouse", "gamepad"});
    CHECK(p.spec.players == 2);
    CHECK(p.spec.languages == std::vector<std::string>{"en", "zh-TW"});
    REQUIRE(p.spec.fontFallback.size() == 1);
    CHECK(p.spec.fontFallback[0].lang == "zh-TW");
    CHECK(p.spec.fontFallback[0].families.size() == 2);
    CHECK(p.spec.budgetFrameMs == doctest::Approx(1.5f));
    CHECK(p.spec.budgetEffectLayers == 24);
    CHECK(p.spec.budgetFillRate == doctest::Approx(3.0f));
    CHECK(p.spec.engine == "example-engine");
    CHECK(p.spec.engineVersion == "5.7.1");
    REQUIRE(p.uis.size() == 2);
    CHECK(p.uis[0].fit == "contain");
    CHECK(p.uis[1].size.x == 1280);
    CHECK(p.shared.styles == std::vector<std::string>{"shared/base.xts"});
    CHECK(p.shared.tokens == "shared/tokens.xts");
    CHECK(p.shared.recipes == "shared/recipes");
    CHECK(p.previewDpr == 2.0f);
    CHECK(p.target == "desktop");
    CHECK(p.publish.outDir == "out");
    CHECK(p.publish.languages == std::vector<std::string>{"en"});
    CHECK(p.publish.embedFonts);
    CHECK(p.publish.thumbnail.x == 160);
    CHECK(p.publish.atlas.enabled);
    CHECK(p.publish.atlas.maxSize == 2048);
    CHECK(p.publish.atlas.padding == 4);
    CHECK(p.publish.atlas.mipmaps);
}

TEST_CASE("ui-project: save is byte-stable and keeps unknown keys") {
    const std::string dir = freshDir("save");
    writeText(dir + "/Star UI.xtui", kFullProject);
    const UiProject p = loadProject(dir);
    REQUIRE(p.save() == Status::kOk);
    const std::string first = readText(dir + "/Star UI.xtui");
    CHECK(first.find("\"owner\": \"art team\"") != std::string::npos);
    CHECK(first.find("\"notes\": \"kept\"") != std::string::npos);
    CHECK(first.back() == '\n');
    CHECK(first.find('\r') == std::string::npos);

    const UiProject again = loadProject(dir);
    REQUIRE(again.save() == Status::kOk);
    CHECK(readText(dir + "/Star UI.xtui") == first);
    CHECK(again.spec.players == 2);
    CHECK(again.uis[0].fit == "contain");
}

TEST_CASE("T-304: a retired publish target kind reads as dir and merges with a dir entry of the same path") {
    // The retired kind, spelled in two pieces (see ui_project.cpp).
    const std::string retired = "ce-dir";
    const std::string dir = freshDir("t304_targets");
    // The dir entry is older; the retired entry comes later and is newer.
    writeText(dir + "/x.xtui",
              R"({ "version": 1, "id": "x", "uis": [ { "id": "a", "doc": "a.xt" } ],
  "publish": { "targets": [
    { "kind": "dir", "path": "D:/MyGame/ui/game-ui", "lastPublished": "2026-09-01T08:00:00Z" },
    { "kind": "dir", "path": "out", "lastPublished": "2026-09-10T08:00:00Z" },
    { "kind": ")" +
                  retired +
                  R"(", "path": "D:/MyGame/ui/game-ui", "lastPublished": "2026-09-27T08:00:00Z" } ] } })");
    UiProject p = loadProject(dir);
    REQUIRE(p.publish.targets.size() == 2);
    CHECK(p.publish.targets[0].path == "out");
    CHECK(p.publish.targets[1].kind == kPublishKindDir);
    CHECK(p.publish.targets[1].path == "D:/MyGame/ui/game-ui");
    CHECK(p.publish.targets[1].lastPublished == "2026-09-27T08:00:00Z");
    REQUIRE(p.save() == Status::kOk);
    const std::string written = readText(dir + "/x.xtui");
    CHECK(written.find(retired) == std::string::npos);
    CHECK(written.find("\"kind\": \"dir\"") != std::string::npos);
    const UiProject again = loadProject(dir);
    REQUIRE(again.publish.targets.size() == 2);
    CHECK(again.publish.targets[1].kind == kPublishKindDir);
    CHECK(again.publish.targets[1].lastPublished == "2026-09-27T08:00:00Z");

    // The retired entry first and newer, the dir entry later and older: still one dir entry at the
    // later position, with the later time.
    const std::string dir2 = freshDir("t304_targets2");
    writeText(dir2 + "/x.xtui", R"({ "version": 1, "id": "x", "uis": [ { "id": "a", "doc": "a.xt" } ],
  "publish": { "targets": [
    { "kind": ")" + retired + R"(", "path": "D:/MyGame/ui/game-ui", "lastPublished": "2026-09-27T08:00:00Z" },
    { "kind": "dir", "path": "out", "lastPublished": "2026-09-10T08:00:00Z" },
    { "kind": "dir", "path": "D:/MyGame/ui/game-ui", "lastPublished": "2026-09-01T08:00:00Z" } ] } })");
    const UiProject p2 = loadProject(dir2);
    REQUIRE(p2.publish.targets.size() == 2);
    CHECK(p2.publish.targets[0].path == "out");
    CHECK(p2.publish.targets[1].kind == kPublishKindDir);
    CHECK(p2.publish.targets[1].path == "D:/MyGame/ui/game-ui");
    CHECK(p2.publish.targets[1].lastPublished == "2026-09-27T08:00:00Z");
}

TEST_CASE("ui-project: missing keys read as the §30.1 defaults") {
    const std::string dir = freshDir("defaults");
    writeText(dir + "/x.xtui", R"({ "version": 1, "id": "x", "uis": [ { "id": "a", "doc": "a.xt" } ] })");
    const UiProject p = loadProject(dir);
    CHECK(p.shared.assetRoot == "shared");
    CHECK(p.shared.symbols == "shared/symbols");
    CHECK(p.shared.tokens == "shared/tokens.xts");
    CHECK(p.shared.themes == "shared/themes");
    CHECK(p.shared.strings == "shared/strings");
    CHECK(p.spec.safeAreaDefault == doctest::Approx(0.025f));
    CHECK(p.spec.safeAreaConsole == doctest::Approx(0.05f));
    CHECK(p.target == "engine");
}

TEST_CASE("ui-project: load errors") {
    std::string error;
    CHECK(UiProject::load(freshDir("empty"), &error).status() == Status::kNotFound);

    const std::string badVersion = freshDir("badversion");
    writeText(badVersion + "/x.xtui", R"({ "version": 2, "id": "x", "uis": [] })");
    CHECK(UiProject::load(badVersion, &error).status() == Status::kUnsupported);

    const std::string noId = freshDir("noid");
    writeText(noId + "/x.xtui", R"({ "version": 1, "uis": [] })");
    CHECK(UiProject::load(noId, &error).status() == Status::kInvalidArgument);

    const std::string dup = freshDir("dup");
    writeText(
        dup + "/x.xtui",
        R"({ "version": 1, "id": "x", "uis": [ { "id": "a", "doc": "a.xt" }, { "id": "a", "doc": "b.xt" } ] })");
    CHECK(UiProject::load(dup, &error).status() == Status::kInvalidArgument);
    CHECK(error.find("duplicate") != std::string::npos);

    const std::string notJson = freshDir("notjson");
    writeText(notJson + "/x.xtui", "{ nope");
    CHECK(UiProject::load(notJson, &error).status() == Status::kIoError);

    // T-214: a file that is not a root file.
    writeText(notJson + "/a.xt", "<xt/>");
    CHECK(UiProject::load(notJson + "/a.xt", &error).status() == Status::kInvalidArgument);
}

TEST_CASE("T-214: more than one .xtui in a directory is an error") {
    const std::string dir = freshDir("two_xtui");
    const std::string text = R"({ "version": 1, "id": "x", "uis": [ { "id": "a", "doc": "a.xt" } ] })";
    writeText(dir + "/A.xtui", text);
    writeText(dir + "/B.xtui", text);
    std::string error;
    CHECK(UiProject::load(dir, &error).status() == Status::kInvalidArgument);
    CHECK(error.find("more than one .xtui") != std::string::npos);
    CHECK(findUiProjectFile(dir).empty());
    // Either file opens when given directly.
    REQUIRE(UiProject::load(dir + "/B.xtui", &error).ok());
}

TEST_CASE("T-214: the .xtui wins over ui-project.json and bundle.xtproj; a file path opens directly") {
    const std::string dir = copiedSample("xtui_order");
    writeText(dir + "/ui-project.json",
              R"({ "version": 1, "name": "Old", "id": "old", "uis": [ { "id": "a", "doc": "a.xt" } ] })");
    writeText(
        dir + "/New Name.xtui",
        R"({ "version": 1, "name": "New Name", "id": "new", "uis": [ { "id": "a", "doc": "a.xt" } ] })");
    LegacyKind kind = LegacyKind::kBundleXtproj;
    CHECK(findUiProjectFile(dir, &kind) == dir + "/New Name.xtui");
    CHECK(kind == LegacyKind::kNone);
    CHECK(loadProject(dir).id == "new");

    const UiProject viaJson = loadProject(dir + "/ui-project.json");
    CHECK(viaJson.id == "old");
    CHECK(viaJson.rootDir == dir);
    CHECK(viaJson.legacyKind == LegacyKind::kUiProjectJson);
    CHECK_FALSE(viaJson.legacy);

    const UiProject viaXtproj = loadProject(dir + "/bundle.xtproj");
    CHECK(viaXtproj.id == "mygame");
    CHECK(viaXtproj.legacyKind == LegacyKind::kBundleXtproj);
    CHECK(viaXtproj.legacy);

    const UiProject viaXtui = loadProject(dir + "/New Name.xtui");
    CHECK(viaXtui.id == "new");
    CHECK(viaXtui.legacyKind == LegacyKind::kNone);
    CHECK(viaXtui.rootDir == dir);
}

TEST_CASE("T-214: a ui-project.json upgrades in memory; saving writes <name>.xtui and keeps the old file") {
    const std::string dir = freshDir("legacy_json");
    const std::string old =
        R"({ "version": 1, "name": "Fall of Warlord", "id": "fow", "uis": [ { "id": "a", "doc": "a.xt" } ] })";
    writeText(dir + "/ui-project.json", old);
    UiProject p = loadProject(dir);
    CHECK(p.legacyKind == LegacyKind::kUiProjectJson);
    CHECK_FALSE(p.legacy);
    CHECK(p.path() == dir + "/Fall of Warlord.xtui");
    REQUIRE(p.save() == Status::kOk);
    CHECK(readText(dir + "/ui-project.json") == old);  // Untouched.
    CHECK(fs::is_regular_file(dir + "/Fall of Warlord.xtui"));
    const UiProject reread = loadProject(dir);  // The .xtui now wins.
    CHECK(reread.legacyKind == LegacyKind::kNone);
    CHECK(reread.id == "fow");
}

TEST_CASE("T-214: a bundle.xtproj upgrades in memory; saving writes <name>.xtui and keeps the old file") {
    const std::string dir = copiedSample("legacy_xtproj");
    const std::string old = readText(dir + "/bundle.xtproj");
    UiProject p = loadProject(dir);
    CHECK(p.legacyKind == LegacyKind::kBundleXtproj);
    CHECK(p.legacy);
    REQUIRE(p.save() == Status::kOk);
    CHECK(readText(dir + "/bundle.xtproj") == old);
    CHECK(fs::is_regular_file(dir + "/MyGame UI.xtui"));
    CHECK_FALSE(fs::exists(dir + "/ui-project.json"));
    CHECK(loadProject(dir).legacyKind == LegacyKind::kNone);
}

TEST_CASE("T-214: a directory with both legacy files reads ui-project.json first") {
    const std::string dir = copiedSample("legacy_both");
    writeText(dir + "/ui-project.json",
              R"({ "version": 1, "id": "json-wins", "uis": [ { "id": "a", "doc": "a.xt" } ] })");
    const UiProject p = loadProject(dir);
    CHECK(p.id == "json-wins");
    CHECK(p.legacyKind == LegacyKind::kUiProjectJson);
    REQUIRE(p.save() == Status::kOk);  // name empty -> <id>.xtui
    CHECK(fs::is_regular_file(dir + "/json-wins.xtui"));
    CHECK(fs::is_regular_file(dir + "/ui-project.json"));
    CHECK(fs::is_regular_file(dir + "/bundle.xtproj"));
}

TEST_CASE("T-214: renaming a project writes the new .xtui and deletes the old one") {
    const std::string dir = freshDir("rename");
    writeText(dir + "/Alpha.xtui",
              R"({ "version": 1, "name": "Alpha", "id": "alpha", "uis": [ { "id": "a", "doc": "a.xt" } ] })");
    writeText(dir + "/ui-project.json", R"({ "version": 1, "id": "old", "uis": [] })");
    UiProject p = loadProject(dir);
    p.name = "Beta";
    REQUIRE(p.save() == Status::kOk);
    CHECK_FALSE(fs::exists(dir + "/Alpha.xtui"));
    CHECK(fs::is_regular_file(dir + "/Beta.xtui"));
    CHECK(fs::is_regular_file(dir + "/ui-project.json"));  // Legacy files are never touched.
    p.name = "Gamma";
    REQUIRE(p.save() == Status::kOk);
    CHECK_FALSE(fs::exists(dir + "/Beta.xtui"));
    CHECK(loadProject(dir).name == "Gamma");

    // A case-only rename leaves exactly one file, spelled the new way.
    p.name = "gamma";
    REQUIRE(p.save() == Status::kOk);
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".xtui") {
            names.push_back(entry.path().filename().string());
        }
    }
    CHECK(names == std::vector<std::string>{"gamma.xtui"});
}

TEST_CASE("T-214: uiProjectFileName falls back to the id for unusable names") {
    CHECK(uiProjectFileName("Fall of Warlord", "fow") == "Fall of Warlord.xtui");
    CHECK(uiProjectFileName("", "fow") == "fow.xtui");
    CHECK(uiProjectFileName("a/b", "fow") == "fow.xtui");
    CHECK(uiProjectFileName("a\\b", "fow") == "fow.xtui");
    CHECK(uiProjectFileName("what?", "fow") == "fow.xtui");
    CHECK(uiProjectFileName("trailing.", "fow") == "fow.xtui");
    CHECK(uiProjectFileName(" lead", "fow") == "fow.xtui");
    CHECK(uiProjectFileName("..", "fow") == "fow.xtui");
    CHECK(uiProjectFileName("CON", "fow") == "fow.xtui");
    CHECK(uiProjectFileName("com1", "fow") == "fow.xtui");
    CHECK(uiProjectFileName("Console UI", "fow") == "Console UI.xtui");
    CHECK(uiProjectFileName("\xe6\x88\xb0", "fow") == "\xe6\x88\xb0.xtui");  // T-237: UTF-8 is kept.
    CHECK(uiProjectFileName("\xe6\x88", "fow") == "fow.xtui");               // Not well-formed UTF-8.
    CHECK(uiProjectFileName("\xc0\xaf", "fow") == "fow.xtui");               // Overlong '/'.
    CHECK(isUiProjectFilePath("a/b/Game.XTUI"));
    CHECK(isUiProjectFilePath("ui-project.json"));
    CHECK(isUiProjectFilePath("c:\\x\\bundle.xtproj"));
    CHECK_FALSE(isUiProjectFilePath("a/manifest.json"));
    CHECK_FALSE(isUiProjectFilePath("a/.xtui"));
}

TEST_CASE("ui-project: a bundle.xtproj directory reads as a legacy UI Project") {
    const UiProject p = loadProject(sampleFixture());
    CHECK(p.legacy);
    CHECK(p.legacyKind == LegacyKind::kBundleXtproj);
    CHECK(p.id == "mygame");
    REQUIRE(p.uis.size() == 2);
    CHECK(p.uis[0].flow == "hud/hud.xtflow");
    CHECK(p.shared.styles == std::vector<std::string>{"shared/base.xts"});
    CHECK(p.shared.themes == "themes");
    CHECK(p.shared.strings == "strings");
    CHECK(p.shared.tokens.empty());

    const ProjectFile back = p.toBundleProject();
    std::string error;
    const ProjectFile original = ProjectFile::load(sampleFixture(), &error).value();
    CHECK(back.id == original.id);
    CHECK(back.uis.size() == original.uis.size());
    CHECK(back.shared.styles == original.shared.styles);
    CHECK(back.shared.fontDirs == original.shared.fontDirs);
}

TEST_CASE("ui-project: toBundleProject prepends an existing shared.tokens once") {
    const std::string dir = freshDir("tokens");
    writeText(dir + "/x.xtui", R"({ "version": 1, "id": "x", "uis": [ { "id": "a", "doc": "a.xt" } ],
        "shared": { "styles": ["shared/base.xts"] } })");
    UiProject p = loadProject(dir);
    CHECK(p.toBundleProject().shared.styles == std::vector<std::string>{"shared/base.xts"});

    writeText(dir + "/shared/tokens.xts", ":root { --c: #fff; }\n");
    CHECK(p.toBundleProject().shared.styles ==
          std::vector<std::string>{"shared/tokens.xts", "shared/base.xts"});

    p.shared.styles.push_back("shared/tokens.xts");
    CHECK(p.toBundleProject().shared.styles ==
          std::vector<std::string>{"shared/base.xts", "shared/tokens.xts"});
}

// --- upgrade ---

TEST_CASE(
    "ui-project: --upgrade writes <name>.xtui, the directories, design/, AGENTS.md and the motion spec") {
    const std::string dir = copiedSample("upgrade");
    UpgradeReport report;
    std::string error;
    REQUIRE_MESSAGE(upgradeToUiProject(dir, &report, &error) == Status::kOk, error);

    CHECK(fs::is_regular_file(dir + "/bundle.xtproj"));  // Nothing deleted.
    CHECK(fs::is_regular_file(dir + "/MyGame UI.xtui"));
    CHECK_FALSE(fs::exists(dir + "/ui-project.json"));
    CHECK(fs::is_regular_file(dir + "/AGENTS.md"));
    for (const char* folder : {"conventions", "contracts", "manifest"}) {
        CHECK(fs::is_directory(dir + "/" + folder));
    }
    // T-305: design/ (with design/references/) replaces the top-level references/.
    CHECK_FALSE(fs::exists(dir + "/references"));
    CHECK(fs::is_directory(dir + "/design/references"));
    CHECK(fs::is_regular_file(dir + "/design/design.yaml"));
    CHECK(std::find(report.created.begin(), report.created.end(), "design/") != report.created.end());
    CHECK(readText(dir + "/contracts/motion-spec.yaml") == std::string(defaultMotionSpecYaml()));
    CHECK(readText(dir + "/AGENTS.md") == std::string(agentsTemplate()));
    CHECK(std::find(report.created.begin(), report.created.end(), "MyGame UI.xtui") != report.created.end());
    CHECK(std::find(report.created.begin(), report.created.end(), "contracts/motion-spec.yaml") !=
          report.created.end());

    const UiProject p = loadProject(dir);
    CHECK_FALSE(p.legacy);
    CHECK(p.id == "mygame");
    CHECK(p.name == "MyGame UI");
    CHECK(p.uis.size() == 2);
    CHECK(p.shared.themes == "themes");
    CHECK(p.shared.strings == "strings");
    CHECK(p.shared.tokens.empty());

    // A second upgrade refuses, and changes nothing.
    const std::string before = readText(dir + "/MyGame UI.xtui");
    CHECK(upgradeToUiProject(dir, nullptr, &error) == Status::kInvalidArgument);
    CHECK(readText(dir + "/MyGame UI.xtui") == before);
}

TEST_CASE("ui-project: --upgrade keeps an existing AGENTS.md and fails without a bundle.xtproj") {
    const std::string dir = copiedSample("upgrade_keep");
    writeText(dir + "/AGENTS.md", "mine\n");
    REQUIRE(upgradeToUiProject(dir) == Status::kOk);
    CHECK(readText(dir + "/AGENTS.md") == "mine\n");

    CHECK(upgradeToUiProject(freshDir("upgrade_none")) == Status::kNotFound);
}

TEST_CASE("ui-project: an upgraded project packs the same UIs and assets as its bundle.xtproj") {
    const std::string dir = copiedSample("upgrade_pack");
    const std::string outLegacy = freshDir("upgrade_pack_legacy");
    const std::string outProject = freshDir("upgrade_pack_project");
    PackOptions options;
    options.thumbnails = false;
    options.outDir = outLegacy;
    const PackReport legacy = pack(loadProject(dir), options);
    REQUIRE(legacy.ok);
    REQUIRE(upgradeToUiProject(dir) == Status::kOk);
    options.outDir = outProject;
    const PackReport upgraded = pack(loadProject(dir), options);
    REQUIRE(upgraded.ok);
    CHECK(upgraded.packedUis == legacy.packedUis);

    std::string error;
    auto a = Bundle::open(outLegacy, nullptr, &error);
    auto b = Bundle::open(outProject, nullptr, &error);
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    CHECK(a.value()->project() == nullptr);  // bundle.xtproj: no `project`.
    CHECK(a.value()->spec() == nullptr);     // ... and no `spec` (T-207).
    REQUIRE(b.value()->project() != nullptr);
    CHECK(b.value()->project()->name == "MyGame UI");
    CHECK(b.value()->project()->version == 1);
    CHECK(b.value()->project()->commit.empty());  // T-314: optional, absent by default.
    CHECK(b.value()->project()->checkpoint.empty());
    REQUIRE(a.value()->uis().size() == b.value()->uis().size());
    for (size_t i = 0; i < a.value()->uis().size(); ++i) {
        CHECK(a.value()->uis()[i].assets == b.value()->uis()[i].assets);
        CHECK(a.value()->uis()[i].fileHash == b.value()->uis()[i].fileHash);
    }
}

// --- measuring ---

TEST_CASE("assets: measureImage reads PNG, JPEG and SVG headers") {
    const std::string dir = freshDir("measure");
    writeRgbaPng(dir + "/a.png", 7, 5);
    const ImageMeasure png = measureImage(readText(dir + "/a.png"), ".png");
    CHECK(png.ok);
    CHECK(png.w == 7);
    CHECK(png.h == 5);
    CHECK(png.alpha);

    const ImageMeasure jpg = measureImage(jpegBytes(64, 32), ".jpg");
    CHECK(jpg.ok);
    CHECK(jpg.w == 64);
    CHECK(jpg.h == 32);
    CHECK_FALSE(jpg.alpha);

    const ImageMeasure svg = measureImage(
        R"(<?xml version="1.0"?><svg xmlns="http://www.w3.org/2000/svg" stroke-width="3" width="24px" height="12">)",
        ".svg");
    CHECK(svg.ok);
    CHECK(svg.w == 24);
    CHECK(svg.h == 12);
    CHECK(svg.alpha);

    const ImageMeasure viewBox = measureImage(R"(<svg viewBox="0 0 48 16"></svg>)", ".svg");
    CHECK(viewBox.ok);
    CHECK(viewBox.w == 48);
    CHECK(viewBox.h == 16);

    CHECK_FALSE(measureImage("not an image", ".png").ok);
    CHECK_FALSE(measureImage(R"(<svg width="50%" height="1em">)", ".svg").ok);
}

// --- assets.yaml ---

TEST_CASE("assets: assets.yaml round-trips, including unknown scalar keys and quoting") {
    const char* text = R"(# comment
version: 1
assets:
  - id: health-bar
    type: image
    file: shared/img/health bar.png
    source: "import: figma #12"
    status: approved
    license: CC-BY 4.0
    measured: { w: 64, h: 16, alpha: true, slice: [4, 4, 2, 2] }
    note: "keep: me"
  - id: icon
    type: vector
    file: shared/img/icon.svg
    source: manual
    measured:
      slice: 1 2 3 4
)";
    AssetsManifest m;
    std::string error;
    REQUIRE_MESSAGE(AssetsManifest::parse(text, m, &error), error);
    REQUIRE(m.assets.size() == 2);
    const AssetEntry& bar = m.assets[0];
    CHECK(bar.id == "health-bar");
    CHECK(bar.file == "shared/img/health bar.png");
    CHECK(bar.source == "import: figma #12");
    CHECK(bar.status == "approved");
    CHECK(bar.license == "CC-BY 4.0");
    CHECK(bar.measured.hasSize);
    CHECK(bar.measured.w == 64);
    CHECK(bar.measured.alpha);
    CHECK(bar.measured.hasSlice);
    CHECK(bar.measured.slice[2] == 2);
    REQUIRE(bar.extra.size() == 1);
    CHECK(m.assets[1].status == "draft");
    CHECK(m.assets[1].measured.hasSlice);
    CHECK(m.assets[1].measured.slice[3] == 4);
    CHECK_FALSE(m.assets[1].measured.hasSize);

    const std::string yaml = m.toYaml();
    AssetsManifest back;
    REQUIRE_MESSAGE(AssetsManifest::parse(yaml, back, &error), error);
    CHECK(back.toYaml() == yaml);
    CHECK(back.assets[0].source == "import: figma #12");
    CHECK(back.assets[0].extra == bar.extra);

    AssetsManifest empty;
    CHECK(empty.toYaml().find("assets: []") != std::string::npos);
    REQUIRE(AssetsManifest::parse(empty.toYaml(), back, &error));
    CHECK(back.assets.empty());

    CHECK_FALSE(AssetsManifest::parse("assets:\n  - id: a\n\tfile: x\n", back, &error));
    CHECK_FALSE(AssetsManifest::parse("assets: 3\n", back, &error));
    CHECK_FALSE(AssetsManifest::parse("assets: [1, 2\n", back, &error));
}

TEST_CASE("assets: checkAssets fills measured fields and applies every §26.3.3 rule") {
    const std::string dir = freshDir("check");
    writeRgbaPng(dir + "/img/ok.png", 20, 10);
    writeRgbaPng(dir + "/img/fill.png", 8, 6);
    writeText(dir + "/img/photo.jpg", jpegBytes(40, 30));
    const char* text = R"(assets:
  - id: fill
    type: image
    file: img/fill.png
    source: manual
  - id: size
    type: image
    file: img/ok.png
    source: manual
    measured: { w: 21, h: 10 }
  - id: alpha
    type: image
    file: img/photo.jpg
    source: manual
    measured: { alpha: true }
  - id: slice
    type: image
    file: img/ok.png
    source: manual
    measured: { slice: [10, 10, 2, 2] }
  - id: slice-neg
    type: image
    file: img/ok.png
    source: manual
    measured: { slice: [-1, 1, 1, 1] }
  - id: good-slice
    type: image
    file: img/ok.png
    source: manual
    status: approved
    measured: { w: 20, h: 10, alpha: true, slice: [4, 4, 2, 2] }
  - id: missing
    type: image
    file: img/nope.png
    source: manual
  - id: status
    type: image
    file: img/ok.png
    source: manual
    status: done
  - id: ai
    type: image
    file: img/ok.png
    source: ai:S3
  - id: fill
    type: image
    file: img/ok.png
    source: manual
  - type: image
    file: img/ok.png
)";
    AssetsManifest m;
    std::string error;
    REQUIRE_MESSAGE(AssetsManifest::parse(text, m, &error), error);
    const std::vector<std::string> referenced = {canonicalAssetPath(dir + "/img/ok.png")};
    bool filled = false;
    const std::vector<AssetIssue> issues = checkAssets(m, dir, &referenced, &filled);

    CHECK(filled);
    CHECK(m.assets[0].measured.hasSize);
    CHECK(m.assets[0].measured.w == 8);
    CHECK(m.assets[0].measured.h == 6);
    CHECK(m.assets[0].measured.alpha);
    // fill.png is not referenced: a warning, nothing else.
    auto fill = issuesFor(issues, "fill");
    CHECK(std::any_of(fill.begin(), fill.end(), [](const AssetIssue& i) {
        return !i.isError && i.message.find("not referenced") != std::string::npos;
    }));

    auto size = issuesFor(issues, "size");
    REQUIRE(size.size() == 1);
    CHECK(size[0].isError);
    CHECK(size[0].message.find("21x10") != std::string::npos);

    auto alpha = issuesFor(issues, "alpha");
    CHECK(std::any_of(alpha.begin(), alpha.end(), [](const AssetIssue& i) {
        return i.isError && i.message.find("alpha") != std::string::npos;
    }));

    auto slice = issuesFor(issues, "slice");
    REQUIRE(slice.size() == 1);
    CHECK(slice[0].message.find("stretchable") != std::string::npos);
    auto sliceNeg = issuesFor(issues, "slice-neg");
    REQUIRE(sliceNeg.size() == 1);
    CHECK(sliceNeg[0].message.find("negative") != std::string::npos);

    CHECK(issuesFor(issues, "good-slice").empty());

    auto missing = issuesFor(issues, "missing");
    REQUIRE(missing.size() == 1);
    CHECK(missing[0].isError);
    CHECK(missing[0].message.find("not found") != std::string::npos);

    auto status = issuesFor(issues, "status");
    REQUIRE(status.size() == 1);
    CHECK(status[0].message.find("done") != std::string::npos);

    auto ai = issuesFor(issues, "ai");
    REQUIRE(ai.size() == 1);
    CHECK(ai[0].message.find("license") != std::string::npos);

    CHECK(std::any_of(fill.begin(), fill.end(),
                      [](const AssetIssue& i) { return i.isError && i.message == "duplicate id"; }));
    auto noId = issuesFor(issues, "");
    REQUIRE(noId.size() == 1);
    CHECK(noId[0].message.find("no id") != std::string::npos);

    // Without a referenced list there is no unreferenced warning.
    AssetsManifest again;
    REQUIRE(AssetsManifest::parse(text, again, &error));
    const std::vector<AssetIssue> noRefs = checkAssets(again, dir, nullptr, nullptr);
    for (const AssetIssue& issue : noRefs) {
        CHECK(issue.message.find("not referenced") == std::string::npos);
    }
}

// --- pack ---

namespace {

/// The sample fixture upgraded to a UI Project, with `game` set, a string table and a theme in the
/// §30.1 `shared/` locations, reference/contract/recipe/sidecar files that must stay out of the
/// bundle, and a `manifest/assets.yaml` listing both images plus an unused one.
std::string makeUiProject(const std::string& name) {
    const std::string dir = copiedSample(name);
    REQUIRE(upgradeToUiProject(dir) == Status::kOk);
    UiProject p = loadProject(dir);
    p.game = "My Game";
    p.shared.themes = "shared/themes";
    p.shared.strings = "shared/strings";
    REQUIRE(p.save() == Status::kOk);
    writeText(dir + "/shared/themes/dark.xts", ":root { --bg: #000; }\n");
    writeText(dir + "/shared/strings/en.csv", "key,text\n");
    writeText(dir + "/shared/strings/fr.csv", "key,text\n");
    writeRgbaPng(dir + "/shared/img/unused.png", 3, 3);
    writeRgbaPng(dir + "/references/moodboard.png", 3, 3);
    writeText(dir + "/references/adr-001.md", "decision\n");
    writeText(dir + "/contracts/scenario.yaml", "name: s\n");
    writeText(dir + "/shared/recipes/pop.json", "{}\n");
    writeText(dir + "/hud/hud.xteditor.json", "{}\n");
    writeText(dir + "/conventions/naming.yaml", "ids: kebab\n");
    writeText(dir + "/manifest/assets.yaml", R"(assets:
  - id: a-hud
    type: image
    file: hud/img/a.png
    source: manual
    status: approved
  - id: a
    type: image
    file: shared/img/a.png
    source: manual
  - id: unused
    type: image
    file: shared/img/unused.png
    source: manual
)");
    return dir;
}

}  // namespace

TEST_CASE("pack: a UI Project adds project, motionSpec and assets[].manifest, and fills assets.yaml") {
    const std::string dir = makeUiProject("pack_full");
    const std::string out = freshDir("pack_full_out");
    PackOptions options;
    options.outDir = out;
    options.thumbnails = false;
    const PackReport report = pack(loadProject(dir), options);
    REQUIRE(report.ok);
    CHECK(report.assetsManifestUpdated);
    CHECK(hasDiagnostic(report, "assets: unused: 'shared/img/unused.png' is not referenced", false));
    CHECK_FALSE(hasDiagnostic(report, "assets: a-hud", false));

    // measured was written back.
    AssetsManifest written = AssetsManifest::load(dir + "/manifest/assets.yaml").value();
    REQUIRE(written.find("a-hud") != nullptr);
    CHECK(written.find("a-hud")->measured.hasSize);
    CHECK(written.find("a-hud")->measured.hasAlpha);
    CHECK(written.find("a-hud")->status == "approved");

    std::string error;
    auto opened = Bundle::open(out, nullptr, &error);
    REQUIRE_MESSAGE(opened.ok(), error);
    const Bundle& bundle = *opened.value();
    REQUIRE(bundle.project() != nullptr);
    CHECK(bundle.project()->name == "MyGame UI");
    CHECK(bundle.project()->game == "My Game");
    CHECK(bundle.project()->version == 1);
    CHECK(bundle.motionSpec().find("\"duration\"") != std::string_view::npos);
    CHECK(bundle.motionSpec().find("\"fast\":[100,200]") != std::string_view::npos);
    CHECK(bundle.motionSpec().find("\"spring\":\"off\"") != std::string_view::npos);
    CHECK(bundle.motionSpec().find("\"effect_layers_max\":24") != std::string_view::npos);
    REQUIRE(bundle.themes().size() == 1);
    CHECK(bundle.themes()[0].name == "dark");
    CHECK(bundle.strings().size() == 2);

    const std::string manifest = readText(out + "/manifest.json");
    CHECK(manifest.find("\"project\": {") < manifest.find("\"uis\": ["));
    CHECK(manifest.find("\"manifest\": {") != std::string::npos);
    CHECK(manifest.find("\"id\": \"a-hud\"") != std::string::npos);
    CHECK(manifest.find("\"id\": \"unused\"") == std::string::npos);
}

// T-262b (F10): an assets.yaml the writer cannot reproduce -- comments, structured values -- is
// left byte for byte as it is, with a warning, instead of being rewritten without them.
TEST_CASE("T-262b pack: assets.yaml with comments or structured values is not rewritten") {
    const std::string dir = makeUiProject("pack_assets_lossy");
    const std::string original = R"(# Hand-written notes about these assets.
assets:
  - id: a-hud
    type: image
    file: hud/img/a.png  # the HUD bar
    source: manual
    status: approved
    tags: [a, b]
  - id: a
    type: image
    file: shared/img/a.png
    source: manual
)";
    writeText(dir + "/manifest/assets.yaml", original);
    PackOptions options;
    options.outDir = freshDir("pack_assets_lossy_out");
    options.thumbnails = false;
    const PackReport report = pack(loadProject(dir), options);
    REQUIRE(report.ok);
    CHECK_FALSE(report.assetsManifestUpdated);
    CHECK(hasDiagnostic(report,
                        "assets.yaml not updated: it has comments or structured values the writer "
                        "cannot preserve",
                        false));
    CHECK(readText(dir + "/manifest/assets.yaml") == original);

    // Structured values alone (no comment) are enough.
    const std::string structured = R"(assets:
  - id: a-hud
    type: image
    file: hud/img/a.png
    source: manual
    provenance: {board: x, frame: y}
)";
    writeText(dir + "/manifest/assets.yaml", structured);
    const PackReport again = pack(loadProject(dir), options);
    REQUIRE(again.ok);
    CHECK_FALSE(again.assetsManifestUpdated);
    CHECK(readText(dir + "/manifest/assets.yaml") == structured);
}

// T-262b (F10): a scalar-only assets.yaml is still filled in, also when it carries the header
// comment the writer itself emits (save() reproduces it).
TEST_CASE("T-262b pack: a scalar assets.yaml with only the writer's header is still written back") {
    const std::string dir = makeUiProject("pack_assets_scalar");
    const AssetsManifest manifest = AssetsManifest::load(dir + "/manifest/assets.yaml").value();
    REQUIRE(manifest.save(dir + "/manifest/assets.yaml") == Status::kOk);
    REQUIRE(readText(dir + "/manifest/assets.yaml").rfind("# ", 0) == 0);
    PackOptions options;
    options.outDir = freshDir("pack_assets_scalar_out");
    options.thumbnails = false;
    const PackReport report = pack(loadProject(dir), options);
    REQUIRE(report.ok);
    CHECK(report.assetsManifestUpdated);
    CHECK_FALSE(hasDiagnostic(report, "assets.yaml not updated", false));
    AssetsManifest written = AssetsManifest::load(dir + "/manifest/assets.yaml").value();
    REQUIRE(written.find("a-hud") != nullptr);
    CHECK(written.find("a-hud")->measured.hasSize);
}

TEST_CASE("T-207 pack: manifest spec carries spec.safeArea and the logical height") {
    const std::string dir = makeUiProject("pack_spec");
    Result<UiProject> loaded = UiProject::load(dir);
    REQUIRE(loaded.ok());
    UiProject project = loaded.value();
    project.spec.safeAreaDefault = 0.03f;
    project.spec.safeAreaConsole = 0.075f;
    REQUIRE(project.save() == Status::kOk);
    const std::string out = freshDir("pack_spec_out");
    PackOptions options;
    options.outDir = out;
    options.thumbnails = false;
    REQUIRE(pack(loadProject(dir), options).ok);

    const std::string manifest = readText(out + "/manifest.json");
    CHECK(manifest.find("\"project\": {") < manifest.find("\"spec\": {"));
    CHECK(manifest.find("\"spec\": {") < manifest.find("\"uis\": ["));
    CHECK(manifest.find("\"default\": 0.03,") != std::string::npos);
    CHECK(manifest.find("\"console\": 0.075") != std::string::npos);
    CHECK(manifest.find("\"logicalHeight\": 1080") != std::string::npos);

    std::string error;
    auto opened = Bundle::open(out, nullptr, &error);
    REQUIRE_MESSAGE(opened.ok(), error);
    const BundleSpec* spec = opened.value()->spec();
    REQUIRE(spec != nullptr);
    CHECK(spec->safeAreaDefault == doctest::Approx(0.03f));
    CHECK(spec->safeAreaConsole == doctest::Approx(0.075f));
    CHECK(spec->logicalHeight == doctest::Approx(1080.0f));
    // An incremental run keeps it (readManifest round trip).
    options.onlyUis = {opened.value()->uis().front().id};
    REQUIRE(pack(loadProject(dir), options).ok);
    CHECK(readText(out + "/manifest.json").find("\"logicalHeight\": 1080") != std::string::npos);
}

TEST_CASE("T-210 spec.logicalHeight: read, written only when set, packed into the manifest") {
    const std::string dir = makeUiProject("spec_logical_height");
    Result<UiProject> loaded = UiProject::load(dir);
    REQUIRE(loaded.ok());
    UiProject project = loaded.value();
    CHECK(project.spec.logicalHeight == doctest::Approx(1080.0f));  // Missing = the default.
    REQUIRE(project.save() == Status::kOk);
    CHECK(readText(dir + "/MyGame UI.xtui").find("logicalHeight") == std::string::npos);

    project.spec.logicalHeight = 720.0f;
    REQUIRE(project.save() == Status::kOk);
    const std::string text = readText(dir + "/MyGame UI.xtui");
    CHECK(text.find("\"logicalHeight\": 720") != std::string::npos);
    CHECK(text.find("\"safeArea\": {") < text.find("\"logicalHeight\""));
    Result<UiProject> reread = UiProject::load(dir);
    REQUIRE(reread.ok());
    CHECK(reread.value().spec.logicalHeight == doctest::Approx(720.0f));
    CHECK(reread.value().extraSpecJson.find("logicalHeight") == std::string::npos);

    const std::string out = freshDir("spec_logical_height_out");
    PackOptions options;
    options.outDir = out;
    options.thumbnails = false;
    REQUIRE(pack(loadProject(dir), options).ok);
    CHECK(readText(out + "/manifest.json").find("\"logicalHeight\": 720") != std::string::npos);
    std::string error;
    auto opened = Bundle::open(out, nullptr, &error);
    REQUIRE_MESSAGE(opened.ok(), error);
    REQUIRE(opened.value()->spec() != nullptr);
    CHECK(opened.value()->spec()->logicalHeight == doctest::Approx(720.0f));

    // A non-positive value reads as the default.
    std::string bad = readText(dir + "/MyGame UI.xtui");
    bad.replace(bad.find("\"logicalHeight\": 720"), std::string("\"logicalHeight\": 720").size(),
                "\"logicalHeight\": -5");
    {
        std::ofstream stream(dir + "/MyGame UI.xtui", std::ios::binary | std::ios::trunc);
        stream << bad;
    }
    Result<UiProject> negative = UiProject::load(dir);
    REQUIRE(negative.ok());
    CHECK(negative.value().spec.logicalHeight == doctest::Approx(1080.0f));
}

TEST_CASE("T-241 spec.uiscaleRule: read, written only when set, packed into the manifest") {
    // The rule names and the uiscale each one gives.
    UiScaleRule rule = UiScaleRule::Height;
    CHECK(parseUiScaleRule("shortEdge", rule));
    CHECK(rule == UiScaleRule::ShortEdge);
    CHECK(parseUiScaleRule("width", rule));
    CHECK(rule == UiScaleRule::Width);
    CHECK_FALSE(parseUiScaleRule("diagonal", rule));
    CHECK(rule == UiScaleRule::Width);
    CHECK(std::string(uiScaleRuleName(UiScaleRule::Height)) == "height");
    CHECK(std::string(uiScaleRuleName(UiScaleRule::ShortEdge)) == "shortEdge");
    CHECK(uiScaleFor(UiScaleRule::Height, 1080.0f, 2400.0f, 1080.0f) == doctest::Approx(2400.0f / 1080.0f));
    CHECK(uiScaleFor(UiScaleRule::Width, 1920.0f, 1080.0f, 1080.0f) == doctest::Approx(1920.0f / 1080.0f));
    CHECK(uiScaleFor(UiScaleRule::ShortEdge, 1080.0f, 2400.0f, 1080.0f) == doctest::Approx(1.0f));
    CHECK(uiScaleFor(UiScaleRule::ShortEdge, 2560.0f, 1600.0f, 1080.0f) ==
          doctest::Approx(1600.0f / 1080.0f));
    CHECK(uiScaleFor(UiScaleRule::Height, 1080.0f, 2400.0f, 0.0f) == doctest::Approx(1.0f));

    const std::string dir = makeUiProject("spec_uiscale_rule");
    Result<UiProject> loaded = UiProject::load(dir);
    REQUIRE(loaded.ok());
    UiProject project = loaded.value();
    CHECK(project.spec.uiscaleRule == UiScaleRule::Height);  // Missing = height.
    REQUIRE(project.save() == Status::kOk);
    CHECK(readText(dir + "/MyGame UI.xtui").find("uiscaleRule") == std::string::npos);

    project.spec.uiscaleRule = UiScaleRule::ShortEdge;
    REQUIRE(project.save() == Status::kOk);
    CHECK(readText(dir + "/MyGame UI.xtui").find("\"uiscaleRule\": \"shortEdge\"") != std::string::npos);
    Result<UiProject> reread = UiProject::load(dir);
    REQUIRE(reread.ok());
    CHECK(reread.value().spec.uiscaleRule == UiScaleRule::ShortEdge);
    CHECK(reread.value().extraSpecJson.find("uiscaleRule") == std::string::npos);

    const std::string out = freshDir("spec_uiscale_rule_out");
    PackOptions options;
    options.outDir = out;
    options.thumbnails = false;
    REQUIRE(pack(loadProject(dir), options).ok);
    CHECK(readText(out + "/manifest.json").find("\"uiscaleRule\": \"shortEdge\"") != std::string::npos);
    std::string error;
    auto opened = Bundle::open(out, nullptr, &error);
    REQUIRE_MESSAGE(opened.ok(), error);
    REQUIRE(opened.value()->spec() != nullptr);
    CHECK(opened.value()->spec()->uiscaleRule == UiScaleRule::ShortEdge);

    // The default is written to the manifest too (the host never has to guess), and an unknown
    // value in the .xtui reads as height.
    std::string text = readText(dir + "/MyGame UI.xtui");
    const std::string from = "\"uiscaleRule\": \"shortEdge\"";
    text.replace(text.find(from), from.size(), "\"uiscaleRule\": \"diagonal\"");
    {
        std::ofstream file(dir + "/MyGame UI.xtui", std::ios::binary | std::ios::trunc);
        file << text;
    }
    Result<UiProject> unknown = UiProject::load(dir);
    REQUIRE(unknown.ok());
    CHECK(unknown.value().spec.uiscaleRule == UiScaleRule::Height);
    REQUIRE(pack(loadProject(dir), options).ok);
    CHECK(readText(out + "/manifest.json").find("\"uiscaleRule\": \"height\"") != std::string::npos);
}

TEST_CASE("T-345 spec: fit, uiscaleClamp, uiscaleDpiFloor and responsive read, written back, packed") {
    UiScaleRule rule = UiScaleRule::Height;
    CHECK(parseUiScaleRule("fit", rule));
    CHECK(rule == UiScaleRule::Fit);
    CHECK(std::string(uiScaleRuleName(UiScaleRule::Fit)) == "fit");
    // fit = min(outW / (logicalHeight x 16/9), outH / logicalHeight).
    CHECK(uiScaleFor(UiScaleRule::Fit, 1920.0f, 1080.0f, 1080.0f) == doctest::Approx(1.0f));
    CHECK(uiScaleFor(UiScaleRule::Fit, 3440.0f, 1440.0f, 1080.0f) == doctest::Approx(1440.0f / 1080.0f));
    CHECK(uiScaleFor(UiScaleRule::Fit, 1080.0f, 1920.0f, 1080.0f) == doctest::Approx(1080.0f / 1920.0f));
    CHECK(uiScaleFor(UiScaleRule::Fit, 1280.0f, 720.0f, 720.0f) == doctest::Approx(1.0f));
    CHECK(uiScaleFor(UiScaleRule::Fit, 1280.0f, 720.0f, 0.0f) == doctest::Approx(1.0f));

    const std::string dir = makeUiProject("spec_t345");
    Result<UiProject> loaded = UiProject::load(dir);
    REQUIRE(loaded.ok());
    UiProject project = loaded.value();
    CHECK_FALSE(project.spec.uiscaleLimits.clamped);
    CHECK(project.spec.uiscaleLimits.dpiFloor == 0.0f);
    CHECK_FALSE(project.spec.responsive.present);
    REQUIRE(project.save() == Status::kOk);
    std::string text = readText(dir + "/MyGame UI.xtui");
    CHECK(text.find("uiscaleClamp") == std::string::npos);
    CHECK(text.find("uiscaleDpiFloor") == std::string::npos);
    CHECK(text.find("responsive") == std::string::npos);

    project.spec.uiscaleRule = UiScaleRule::Fit;
    project.spec.uiscaleLimits.clamped = true;
    project.spec.uiscaleLimits.clampMin = 0.75f;
    project.spec.uiscaleLimits.clampMax = 1.5f;
    project.spec.uiscaleLimits.dpiFloor = 0.5f;
    project.spec.responsive.scale = "clamp(0.8, 1.2)";
    project.spec.responsive.visibility = "show";
    REQUIRE(project.save() == Status::kOk);
    text = readText(dir + "/MyGame UI.xtui");
    CHECK(text.find("\"uiscaleRule\": \"fit\"") != std::string::npos);
    CHECK(text.find("\"uiscaleDpiFloor\": 0.5") != std::string::npos);
    CHECK(text.find("\"scale\": \"clamp(0.8, 1.2)\"") != std::string::npos);
    Result<UiProject> reread = UiProject::load(dir);
    REQUIRE(reread.ok());
    const ProjectSpec& spec = reread.value().spec;
    CHECK(spec.uiscaleRule == UiScaleRule::Fit);
    CHECK(spec.uiscaleLimits.clamped);
    CHECK(spec.uiscaleLimits.clampMin == doctest::Approx(0.75f));
    CHECK(spec.uiscaleLimits.clampMax == doctest::Approx(1.5f));
    CHECK(spec.uiscaleLimits.dpiFloor == doctest::Approx(0.5f));
    CHECK(spec.responsive.present);
    CHECK(spec.responsive.scale == "clamp(0.8, 1.2)");
    CHECK(spec.responsive.position == "follow");
    CHECK(spec.responsive.visibility == "show");
    CHECK(reread.value().extraSpecJson.find("responsive") == std::string::npos);
    CHECK(reread.value().extraSpecJson.find("uiscaleClamp") == std::string::npos);

    // The manifest carries the rule, the clamp and the floor (never `responsive`).
    const std::string out = freshDir("spec_t345_out");
    PackOptions options;
    options.outDir = out;
    options.thumbnails = false;
    REQUIRE(pack(loadProject(dir), options).ok);
    const std::string manifest = readText(out + "/manifest.json");
    CHECK(manifest.find("\"uiscaleRule\": \"fit\"") != std::string::npos);
    CHECK(manifest.find("\"uiscaleDpiFloor\": 0.5") != std::string::npos);
    CHECK(manifest.find("responsive") == std::string::npos);
    std::string error;
    auto opened = Bundle::open(out, nullptr, &error);
    REQUIRE_MESSAGE(opened.ok(), error);
    REQUIRE(opened.value()->spec() != nullptr);
    const BundleSpec& packed = *opened.value()->spec();
    CHECK(packed.uiscaleRule == UiScaleRule::Fit);
    CHECK(packed.uiscaleLimits.clamped);
    CHECK(packed.uiscaleLimits.clampMin == doctest::Approx(0.75f));
    CHECK(packed.uiscaleLimits.clampMax == doctest::Approx(1.5f));
    CHECK(packed.uiscaleLimits.dpiFloor == doctest::Approx(0.5f));

    // An open upper bound is `null`; a malformed clamp reads as none.
    UiProject open = reread.value();
    open.spec.uiscaleLimits.clampMax = std::numeric_limits<float>::infinity();
    REQUIRE(open.save() == Status::kOk);
    text = readText(dir + "/MyGame UI.xtui");
    CHECK(text.find("null") != std::string::npos);
    Result<UiProject> openRead = UiProject::load(dir);
    REQUIRE(openRead.ok());
    CHECK(openRead.value().spec.uiscaleLimits.clamped);
    CHECK(std::isinf(openRead.value().spec.uiscaleLimits.clampMax));
    const size_t at = text.find("\"uiscaleClamp\"");
    REQUIRE(at != std::string::npos);
    const size_t end = text.find(']', at);
    text.replace(at, end + 1 - at, "\"uiscaleClamp\": [2, 1]");
    {
        std::ofstream file(dir + "/MyGame UI.xtui", std::ios::binary | std::ios::trunc);
        file << text;
    }
    Result<UiProject> bad = UiProject::load(dir);
    REQUIRE(bad.ok());
    CHECK_FALSE(bad.value().spec.uiscaleLimits.clamped);
}

TEST_CASE("pack: references, contracts, recipes, conventions and sidecars never reach the bundle") {
    const std::string dir = makeUiProject("pack_exclude");
    const std::string out = freshDir("pack_exclude_out");
    PackOptions options;
    options.outDir = out;
    options.thumbnails = false;
    REQUIRE(pack(loadProject(dir), options).ok);
    for (fs::recursive_directory_iterator it(out), end; it != end; ++it) {
        const std::string relative = fs::relative(it->path(), out).generic_string();
        const std::string top = relative.substr(0, relative.find('/'));
        CHECK_MESSAGE((top == "manifest.json" || top == "ui" || top == "assets" || top == "thumbs" ||
                       top == "themes" || top == "strings"),
                      relative);
        CHECK(it->path().filename() != "moodboard.png");
        CHECK(it->path().filename() != "motion-spec.yaml");
        CHECK(it->path().filename() != "assets.yaml");
        CHECK(it->path().extension() != ".md");
    }
    // unused.png is not referenced, so it is not in assets/ either: only the two used images.
    size_t assetFiles = 0;
    for (fs::directory_iterator it(out + "/assets"), end; it != end; ++it) {
        ++assetFiles;
    }
    CHECK(assetFiles == 1);  // hud/img/a.png and shared/img/a.png have the same bytes.
}

TEST_CASE("pack: publish.languages is the default --lang, and --lang still overrides it") {
    const std::string dir = makeUiProject("pack_lang");
    UiProject p = loadProject(dir);
    p.publish.languages = {"fr"};
    REQUIRE(p.save() == Status::kOk);
    PackOptions options;
    options.thumbnails = false;
    options.outDir = freshDir("pack_lang_out");
    REQUIRE(pack(loadProject(dir), options).ok);
    std::string error;
    auto bundle = Bundle::open(options.outDir, nullptr, &error);
    REQUIRE(bundle.ok());
    REQUIRE(bundle.value()->strings().size() == 1);
    CHECK(bundle.value()->strings()[0].lang == "fr");

    options.outDir = freshDir("pack_lang_out2");
    options.languages = {"en"};
    REQUIRE(pack(loadProject(dir), options).ok);
    bundle = Bundle::open(options.outDir, nullptr, &error);
    REQUIRE(bundle.ok());
    REQUIRE(bundle.value()->strings().size() == 1);
    CHECK(bundle.value()->strings()[0].lang == "en");
}

TEST_CASE("pack: an asset-manifest error fails the run; an incremental run skips the unreferenced check") {
    const std::string dir = makeUiProject("pack_asset_error");
    writeText(dir + "/manifest/assets.yaml", R"(assets:
  - id: a-hud
    type: image
    file: hud/img/a.png
    source: manual
    measured: { w: 999, h: 1 }
  - id: unused
    type: image
    file: shared/img/unused.png
    source: manual
)");
    PackOptions options;
    options.thumbnails = false;
    options.outDir = freshDir("pack_asset_error_out");
    const PackReport full = pack(loadProject(dir), options);
    CHECK_FALSE(full.ok);
    CHECK(hasDiagnostic(full, "assets: a-hud: declared size 999x1", true));
    CHECK(hasDiagnostic(full, "not referenced", false));

    options.onlyUis = {"hud"};
    const PackReport incremental = pack(loadProject(dir), options);
    CHECK_FALSE(hasDiagnostic(incremental, "not referenced", false));
}

TEST_CASE("pack: a malformed motion spec is a spec error") {
    const std::string dir = makeUiProject("pack_bad_motion");
    writeText(dir + "/contracts/motion-spec.yaml", "duration: { fast: [100, 200\n");
    PackOptions options;
    options.thumbnails = false;
    options.outDir = freshDir("pack_bad_motion_out");
    const PackReport report = pack(loadProject(dir), options);
    CHECK_FALSE(report.ok);
    CHECK(hasDiagnostic(report, "spec: malformed motion spec", true));
}

// --- T-200: deterministic fileHash ---

TEST_CASE("T-200: irFileHash ignores the META compile time") {
    compile::CompileOptions options;  // emitMeta = true: the compile time is written.
    std::vector<compile::SourceFile> files{
        {"a.xt", "<xt><body id=\"Root\"><div id=\"A\"></div></body></xt>"}};
    compile::CompileResult r = compile::compileDocumentFromStrings(files, "a.xt", options);
    REQUIRE(r.ok());
    Result<IrView> view = IrView::open(IrBlob{r.ir.data(), r.ir.size()});
    REQUIRE(view.ok());
    REQUIRE(view.value().meta().present());
    const size_t at =
        static_cast<size_t>(reinterpret_cast<const uint8_t*>(view.value().meta().meta()) - r.ir.data());
    // Another second: a different compile time and so a different META payload CRC.
    std::vector<uint8_t> later = r.ir;
    later[at + 8] ^= 0x5A;  // compileTimeUnix (IrMeta offset 8).
    later[at - 4] ^= 0x33;  // The chunk header's crc32.
    CHECK(later != r.ir);
    CHECK(irFileHash(later) == irFileHash(r.ir));
    std::vector<uint8_t> other = r.ir;
    other[at] ^= 1;  // sourceFile: real content.
    CHECK(irFileHash(other) != irFileHash(r.ir));
}

// --- T-200: symbol contracts ---

TEST_CASE("T-200: readSymbolContracts reads requires, state groups, slots and ports") {
    const char* xt = R"XT(<xt><head>
<symbol name="Button" requires="states: Hover, Pressed; slots: label" state-groups="Level: Low|Ok; Mode: A|B">
  <div id="Bg" port:label="string" port:clicked="event out"><slot name="label"></slot></div>
  <slot name="icon"></slot>
</symbol>
<symbol name="Plain"><div id="X"></div></symbol>
</head><body id="Root"></body></xt>)XT";
    const std::vector<SymbolContract> contracts = readSymbolContracts(xt, "shared/symbols/button.xt");
    REQUIRE(contracts.size() == 2);
    const SymbolContract& b = contracts[0];
    CHECK(b.name == "Button");
    CHECK(b.source == "shared/symbols/button.xt");
    CHECK(b.requiredStates == std::vector<std::string>{"Hover", "Pressed"});
    CHECK(b.requiredSlots == std::vector<std::string>{"label"});
    REQUIRE(b.stateGroups.size() == 2);
    CHECK(b.stateGroups[0].name == "Level");
    CHECK(b.stateGroups[0].states == std::vector<std::string>{"Low", "Ok"});
    CHECK(b.slotNames == std::vector<std::string>{"label", "icon"});
    REQUIRE(b.ports.size() == 2);
    CHECK(b.ports[0].name == "label");
    CHECK(b.ports[0].type == "string");
    CHECK(b.ports[0].dir == "in");
    CHECK(b.ports[1].dir == "out");
    CHECK(symbolContractSummary(b) ==
          "states: Hover, Pressed; slots: label; group Level: Low|Ok; group Mode: A|B; ports: label string, "
          "clicked event out");
    CHECK(symbolContractSummary(contracts[1]).empty());
    CHECK(readSymbolContracts("<xt><body", "bad.xt").empty());
}

TEST_CASE("T-319: readSymbolContracts reads a symbol file without <body>") {
    // A standard / new symbol file: a <head> with the definition, no <body>.
    const char* xt = R"XT(<xt>
  <head>
    <symbol name="HealthBar" state-groups="Level: Normal|Low" requires="states: Normal, Low; slots: label">
      <div id="Box" port:hp="number in" port:hit="event in" on:port(hit)="play(hit)">
        <span id="Label"><slot name="label">HP</slot></span>
      </div>
    </symbol>
  </head>
</xt>)XT";
    const std::vector<SymbolContract> contracts = readSymbolContracts(xt, "symbols/HealthBar.xt");
    REQUIRE(contracts.size() == 1);
    const SymbolContract& c = contracts[0];
    CHECK(c.name == "HealthBar");
    CHECK(c.requiredStates == std::vector<std::string>{"Normal", "Low"});
    CHECK(c.requiredSlots == std::vector<std::string>{"label"});
    REQUIRE(c.stateGroups.size() == 1);
    CHECK(c.stateGroups[0].states == std::vector<std::string>{"Normal", "Low"});
    CHECK(c.slotNames == std::vector<std::string>{"label"});
    REQUIRE(c.ports.size() == 2);
    CHECK(c.ports[0].name == "hp");
    CHECK(c.ports[0].type == "number");
    CHECK(c.ports[1].name == "hit");
    CHECK(c.ports[1].type == "event");
    // A top-level definition (no <head>, no <body>) is read too.
    const std::vector<SymbolContract> top = readSymbolContracts(
        R"XT(<xt><symbol name="Dot" requires="states: On"><div id="D"></div></symbol></xt>)XT",
        "symbols/Dot.xt");
    REQUIRE(top.size() == 1);
    CHECK(top[0].requiredStates == std::vector<std::string>{"On"});
}

TEST_CASE("T-200: pack writes manifest symbols[] and Bundle::symbolContracts reads them") {
    const std::string dir = freshDir("symbols");
    auto write = [&](const std::string& rel, const std::string& text) {
        fs::create_directories((fs::path(dir) / rel).parent_path());
        std::ofstream(fs::path(dir) / rel, std::ios::binary) << text;
    };
    write("Sym.xtui", R"JSON({ "version": 1, "name": "Sym", "id": "sym",
  "uis": [ { "id": "hud", "doc": "ui/hud.xt", "size": [320, 200] } ],
  "shared": { "styles": [], "symbols": "shared/symbols" } })JSON");
    write("shared/symbols/b_button.xt",
          R"XT(<xt><head><style>symbol(Button) { #Bg:hover { opacity: 0.5; } }</style>
<symbol name="Button" requires="states: Hover; slots: label"><div id="Bg"><slot name="label"></slot></div></symbol>
</head><body></body></xt>)XT");
    write(
        "shared/symbols/a_badge.xt",
        R"XT(<xt><head><symbol name="Badge" state-groups="Tone: Good|Bad"><div id="Dot" port:n="number"></div></symbol>
</head><body></body></xt>)XT");
    write("ui/hud.xt", R"XT(<xt><head><symbol name="Local"><div id="L"></div></symbol></head>
<body id="Root"><use symbol="Local" id="One"></use></body></xt>)XT");
    std::string error;
    Result<UiProject> project = UiProject::load(dir, &error);
    REQUIRE_MESSAGE(project.ok(), error);
    PackOptions options;
    options.thumbnails = false;
    options.outDir = dir + "/out";
    const PackReport report = pack(project.value(), options);
    for (const auto& d : report.diagnostics) {
        INFO(d.message);
    }
    REQUIRE(report.ok);
    auto bundle = Bundle::open(options.outDir, nullptr, &error);
    REQUIRE_MESSAGE(bundle.ok(), error);
    std::span<const SymbolContract> symbols = bundle.value()->symbolContracts();
    REQUIRE(symbols.size() == 3);
    CHECK(symbols[0].name == "Badge");  // The library by file name, then the UI documents.
    CHECK(symbols[0].source == "shared/symbols/a_badge.xt");
    REQUIRE(symbols[0].stateGroups.size() == 1);
    CHECK(symbols[0].stateGroups[0].states == std::vector<std::string>{"Good", "Bad"});
    REQUIRE(symbols[0].ports.size() == 1);
    CHECK(symbols[0].ports[0].type == "number");
    CHECK(symbols[1].name == "Button");
    CHECK(symbols[1].requiredStates == std::vector<std::string>{"Hover"});
    CHECK(symbols[1].requiredSlots == std::vector<std::string>{"label"});
    CHECK(symbols[2].name == "Local");
    CHECK(symbols[2].source == "ui/hud.xt");
    const std::string manifest = readText(options.outDir + "/manifest.json");
    CHECK(manifest.find("\"symbols\": [") != std::string::npos);
}

TEST_CASE("ui-project: T-237 a project named and stored under UTF-8 (測試專案/) saves, reopens and packs") {
    using compile::genericPathToUtf8;
    using compile::pathFromUtf8;
    std::error_code ec;
    const fs::path base = xt::test::tempDir() / "xt_uiproj_t237_utf8";
    fs::remove_all(base, ec);
    const std::string dir = genericPathToUtf8(base) + "/測試專案";
    const std::string name = "測試專案 UI";
    UiProject created;
    std::string error;
    REQUIRE_MESSAGE(createUiProject(dir, name, "utf8test", "", &created, &error) == Status::kOk, error);
    CHECK(created.fileName() == name + ".xtui");
    const std::string xtuiPath = dir + "/" + name + ".xtui";
    CHECK(fs::is_regular_file(pathFromUtf8(xtuiPath), ec));
    CHECK(fs::is_regular_file(pathFromUtf8(dir + "/main/main.xt"), ec));

    // Saved with a change, reopened from the directory and from the root file.
    created.game = "遊戲";
    REQUIRE(created.save(&error) == Status::kOk);
    Result<UiProject> byDir = UiProject::load(dir, &error);
    REQUIRE_MESSAGE(byDir.ok(), error);
    CHECK(byDir.value().name == name);
    CHECK(byDir.value().game == "遊戲");
    CHECK(fs::equivalent(pathFromUtf8(byDir.value().path()), pathFromUtf8(xtuiPath), ec));
    Result<UiProject> byFile = UiProject::load(xtuiPath, &error);
    REQUIRE_MESSAGE(byFile.ok(), error);
    CHECK(byFile.value().name == name);
    REQUIRE(byFile.value().ui("main") != nullptr);

    // xtframe-pack's library path: compile, write the bundle, open it again.
    PackOptions options;
    options.outDir = dir + "/out";
    const PackReport report = pack(byDir.value(), options);
    for (const compile::Diagnostic& d : report.diagnostics) {
        const std::string line = d.file + ": " + d.message;
        MESSAGE(line);
    }
    REQUIRE(report.ok);
    CHECK(report.packedUis.size() == 1);
    CHECK(fs::is_regular_file(pathFromUtf8(options.outDir + "/manifest.json"), ec));
    CHECK(fs::is_regular_file(pathFromUtf8(options.outDir + "/thumbs/main.png"), ec));  // stb, UTF-8 path.
    auto opened = Bundle::open(options.outDir, nullptr, &error);
    CHECK_MESSAGE(opened.ok(), error);
    fs::remove_all(base, ec);
}

// --- T-258a: numbers from the project file are bounded (docs/reviews/2026-10-02-full-audit-raw F14, F24) ---

TEST_CASE("T-258a F14: a UI size outside 1..16384 is a load error; thumbnails are clamped to 0..1024") {
    for (const char* size : {"[1e12, 720]", "[40000, 40000]", "[0, 720]", "[1280, -1]", "[16385, 1]"}) {
        CAPTURE(size);
        const std::string uis =
            std::string(R"("uis": [ { "id": "a", "doc": "a.xt", "size": )") + size + " } ]";
        const std::string dir = freshDir("bad_size");
        writeText(dir + "/x.xtui", R"({ "version": 1, "id": "x", )" + uis + " }");
        std::string error;
        CHECK(UiProject::load(dir, &error).status() == Status::kInvalidArgument);
        CHECK(error.find("'size'") != std::string::npos);
        const std::string legacy = freshDir("bad_size_xtproj");
        writeText(legacy + "/bundle.xtproj", R"({ "version": 2, "id": "x", )" + uis + " }");
        error.clear();
        CHECK(ProjectFile::load(legacy, &error).status() == Status::kInvalidArgument);
        CHECK(error.find("'size'") != std::string::npos);
    }
    const std::string uis = R"("uis": [ { "id": "a", "doc": "a.xt", "size": [16384, 1] } ])";
    const std::string publish = R"("publish": { "thumbnails": [5000, 1000000000] })";
    const std::string dir = freshDir("edge_size");
    writeText(dir + "/x.xtui", R"({ "version": 1, "id": "x", )" + uis + ", " + publish + " }");
    const UiProject p = loadProject(dir);
    CHECK(p.uis[0].size.x == 16384);
    CHECK(p.publish.thumbnail.x == 1024);
    CHECK(p.publish.thumbnail.y == 1024);
    const std::string legacy = freshDir("edge_size_xtproj");
    writeText(legacy + "/bundle.xtproj",
              R"({ "version": 2, "id": "x", )" + uis + R"(, "publish": { "thumbnails": [-5, 90] } })");
    std::string error;
    Result<ProjectFile> file = ProjectFile::load(legacy, &error);
    REQUIRE_MESSAGE(file.ok(), error);
    CHECK(file.value().publish.thumbnail.x == 0);
    CHECK(file.value().publish.thumbnail.y == 90);
}

TEST_CASE("T-258a F24: atlas maxSize is clamped to 64..16384 and padding to 0..64 on load") {
    auto atlasOf = [](const std::string& name, const std::string& atlas) {
        const std::string dir = freshDir(name);
        writeText(dir + "/x.xtui", R"({ "version": 1, "id": "x", "uis": [ { "id": "a", "doc": "a.xt" } ],
                                        "publish": { "atlas": )" +
                                       atlas + " } }");
        return loadProject(dir).publish.atlas;
    };
    const auto big = atlasOf("atlas_big", R"({ "maxSize": 100000, "padding": 300000000 })");
    CHECK(big.maxSize == 16384);
    CHECK(big.padding == 64);
    const auto small = atlasOf("atlas_small", R"({ "maxSize": 10, "padding": -3 })");
    CHECK(small.maxSize == 64);
    CHECK(small.padding == 0);
    const auto huge = atlasOf("atlas_huge", R"({ "maxSize": 18446744073709551615, "padding": 4 })");
    CHECK(huge.maxSize == 16384);
    CHECK(huge.padding == 4);
    const auto normal = atlasOf("atlas_normal", R"({ "maxSize": 2048, "padding": 4 })");
    CHECK(normal.maxSize == 2048);
    CHECK(normal.padding == 4);
}

TEST_CASE("T-314: pack writes project.commit and project.checkpoint when given and the bundle reads them") {
    const std::string dir = copiedSample("t314_provenance");
    REQUIRE(upgradeToUiProject(dir) == Status::kOk);
    const std::string out = freshDir("t314_provenance_out");
    PackOptions options;
    options.thumbnails = false;
    options.outDir = out;
    options.projectCommit = "0123456789abcdef0123456789abcdef01234567";
    options.projectCheckpoint = "start";
    REQUIRE(pack(loadProject(dir), options).ok);
    const std::string manifest = readText(out + "/manifest.json");
    CHECK(manifest.find("\"commit\": \"0123456789abcdef0123456789abcdef01234567\"") != std::string::npos);
    CHECK(manifest.find("\"checkpoint\": \"start\"") != std::string::npos);
    std::string error;
    auto b = Bundle::open(out, nullptr, &error);
    REQUIRE(b.ok());
    REQUIRE(b.value()->project() != nullptr);
    CHECK(b.value()->project()->commit == options.projectCommit);
    CHECK(b.value()->project()->checkpoint == "start");
}

TEST_CASE("T-407 spec.behavior: missing = 0, written only when set, packed into the manifest and META") {
    const std::string dir = makeUiProject("spec_behavior");
    Result<UiProject> loaded = UiProject::load(dir);
    REQUIRE(loaded.ok());
    UiProject project = loaded.value();
    CHECK(project.spec.behavior == 0);  // Missing = the pre-beta behavior.
    REQUIRE(project.save() == Status::kOk);
    CHECK(readText(dir + "/MyGame UI.xtui").find("\"behavior\"") == std::string::npos);

    // Every packed UI's IR carries the manifest's value; 0 leaves both out.
    const auto packAndCheck = [&](const std::string& outName, uint32_t expected) {
        const std::string out = freshDir(outName);
        PackOptions options;
        options.outDir = out;
        options.thumbnails = false;
        REQUIRE(pack(loadProject(dir), options).ok);
        const std::string manifest = readText(out + "/manifest.json");
        CHECK((manifest.find("\"behavior\"") != std::string::npos) == (expected != 0));
        std::string error;
        auto opened = Bundle::open(out, nullptr, &error);
        REQUIRE_MESSAGE(opened.ok(), error);
        REQUIRE(opened.value()->spec() != nullptr);
        CHECK(opened.value()->spec()->behavior == expected);
        int irs = 0;
        for (const auto& entry : fs::recursive_directory_iterator(fs::path(out))) {
            if (entry.path().extension() != ".xtir") {
                continue;
            }
            std::ifstream stream(entry.path(), std::ios::binary);
            const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)),
                                             std::istreambuf_iterator<char>());
            Result<IrView> view = IrView::open(IrBlob{bytes.data(), bytes.size()});
            REQUIRE(view.ok());
            CHECK(view.value().meta().behavior() == expected);
            ++irs;
        }
        CHECK(irs > 0);
    };
    packAndCheck("spec_behavior_out0", 0);

    project.spec.behavior = 1;
    REQUIRE(project.save() == Status::kOk);
    const std::string text = readText(dir + "/MyGame UI.xtui");
    CHECK(text.find("\"spec\": {\n    \"behavior\": 1,") != std::string::npos);
    Result<UiProject> reread = UiProject::load(dir);
    REQUIRE(reread.ok());
    CHECK(reread.value().spec.behavior == 1);
    CHECK(reread.value().extraSpecJson.find("behavior") == std::string::npos);
    packAndCheck("spec_behavior_out1", 1);

    // A negative or non-integer value reads as 0.
    std::string bad = text;
    bad.replace(bad.find("\"behavior\": 1"), std::string("\"behavior\": 1").size(), "\"behavior\": -3");
    writeText(dir + "/MyGame UI.xtui", bad);
    CHECK(UiProject::load(dir).value().spec.behavior == 0);
    bad.replace(bad.find("\"behavior\": -3"), std::string("\"behavior\": -3").size(), "\"behavior\": \"1\"");
    writeText(dir + "/MyGame UI.xtui", bad);
    CHECK(UiProject::load(dir).value().spec.behavior == 0);
}

TEST_CASE("T-407 spec.behavior: a new UI Project starts at the latest behavior version") {
    const std::string dir = freshDir("behavior_new_project");
    UiProject created;
    std::string error;
    REQUIRE_MESSAGE(createUiProject(dir, "Fresh", "fresh", "", &created, &error) == Status::kOk, error);
    CHECK(created.spec.behavior == kLatestBehavior);
    Result<UiProject> reread = UiProject::load(dir);
    REQUIRE(reread.ok());
    CHECK(reread.value().spec.behavior == kLatestBehavior);
    CHECK(readText(reread.value().path()).find("\"behavior\": 1") != std::string::npos);
}

TEST_CASE("T-408: diffStandardSymbols reports current / outdated / modified copies and writes nothing") {
    std::error_code ec;
    const fs::path base = xt::test::tempDir() / "xt_uiproj_t408_sync";
    fs::remove_all(base, ec);
    const fs::path library = base / "library";
    const fs::path project = base / "project";
    fs::create_directories(library / "mobile", ec);
    fs::create_directories(project / "shared" / "symbols", ec);
    const auto write = [](const fs::path& path, const std::string& text) {
        std::ofstream out(path, std::ios::binary);
        out << text;
    };
    // A is at v2 in the library; its v1 text hashes to the previousContent entry.
    write(library / "index.json",
          R"({"version": 1, "symbols": [
            {"name": "A", "file": "A.xt", "previousContent":
              ["sha256:d6591c8d3f1427fd7582bc5e85ccfc7a0e2777cdefb11f1d3fbf8112d93c7039"]},
            {"name": "B", "file": "B.xt", "previousContent": []},
            {"name": "C", "file": "mobile/C.xt"},
            {"name": "E", "file": "E.xt"}]})");
    write(library / "A.xt", "<symbol name=\"A\">\nv2\n</symbol>\n");
    write(library / "B.xt", "<symbol name=\"B\">\none\ntwo\nthree\n</symbol>\n");
    write(library / "mobile" / "C.xt", "<symbol name=\"C\"/>\n");
    write(library / "E.xt", "<symbol name=\"E\"/>\n");
    const std::string oldA = "<symbol name=\"A\">\r\nv1\r\n</symbol>\r\n";  // v1 with CRLF endings.
    const std::string editedB = "<symbol name=\"B\">\none\nTWO\nthree\n</symbol>\n";
    write(project / "shared" / "symbols" / "A.xt", oldA);
    write(project / "shared" / "symbols" / "B.xt", editedB);
    write(project / "shared" / "symbols" / "C.xt", "<symbol name=\"C\"/>\r\n");
    write(project / "shared" / "symbols" / "D.xt", "<symbol name=\"D\"/>\n");  // Not a standard one.

    UiProject ui;
    ui.rootDir = project.generic_string();
    ui.shared.symbols = "shared/symbols";
    std::vector<StandardSymbolDiff> rows;
    REQUIRE(diffStandardSymbols(ui, library.generic_string(), rows) == Status::kOk);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].name == "A");
    CHECK(rows[0].file == "shared/symbols/A.xt");
    CHECK(rows[0].standard == "A.xt");
    CHECK(rows[0].status == "outdated");
    CHECK(rows[0].diff ==
          "--- standard/A.xt\n+++ shared/symbols/A.xt\n@@ -1,3 +1,3 @@\n <symbol name=\"A\">\n-v2\n+v1\n "
          "</symbol>\n");
    CHECK(rows[1].name == "B");
    CHECK(rows[1].status == "modified");
    CHECK(rows[1].diff.find("\n-two\n+TWO\n") != std::string::npos);
    CHECK(rows[2].name == "C");
    CHECK(rows[2].standard == "mobile/C.xt");
    CHECK(rows[2].status == "current");
    CHECK(rows[2].diff.empty());

    // Nothing was overwritten.
    std::ifstream in(project / "shared" / "symbols" / "A.xt", std::ios::binary);
    const std::string afterA((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(afterA == oldA);
    CHECK(!fs::exists(project / "shared" / "symbols" / "E.xt", ec));

    // No library index: kNotFound.
    CHECK(diffStandardSymbols(ui, (base / "none").generic_string(), rows) == Status::kNotFound);
}
