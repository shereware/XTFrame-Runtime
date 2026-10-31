#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "doctest.h"
#include "test_temp.h"
#include "xt/bundle/symbol_library.h"
#include "xt/bundle/symbol_lint.h"
#include "xt/bundle/ui_project.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/parse_xts.h"
#include "xt/compile/style_model.h"
#include "xt/compile/utf8_path.h"

/// T-498 (Docs/design/49-symbol-sets.md 49.2, 49.7, 49.8): the symbol library model of
/// xt/bundle/symbol_library.h against the preset library in Assets/Symbols. T-498b (49.2, 49.3):
/// multi-library mounts, the drag-in copy rule (useSymbols), isCopyStale and the lint helpers.

using namespace xt;
using namespace xt::bundle;
namespace fs = std::filesystem;

namespace {

const fs::path kPresetDir = fs::path(XT_TEST_BUNDLE_DIR) / ".." / ".." / "Assets" / "Symbols";
constexpr const char* kPresetGroup = "broadsheet-2026";

void writeTextT498(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

std::string readTextT498(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

fs::path freshDir(const char* name) {
    const fs::path dir = xt::test::tempDir() / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

/// A library with one group whose group.json is `groupJson` (index.json minimal and valid).
fs::path brokenLibrary(const char* name, const std::string& groupJson) {
    const fs::path dir = freshDir(name);
    writeTextT498(
        dir / "library.json",
        R"({ "version": 1, "id": "broken", "name": "Broken", "readOnly": false, "groups": ["g"] })");
    writeTextT498(dir / "groups" / "g" / "group.json", groupJson);
    writeTextT498(dir / "groups" / "g" / "index.json",
                  R"({ "version": 1, "tokens": "tokens.xts", "symbols": [ { "name": "A", "file": "A.xt",
                        "tokens": [ { "name": "--x", "default": "1px" } ], "fonts": [], "textures": [] } ] })");
    return dir;
}

size_t errorCount(const SymbolGroup& group) {
    size_t n = 0;
    for (const compile::Diagnostic& d : group.problems) {
        n += d.level == compile::Diagnostic::Level::Error ? 1 : 0;
    }
    return n;
}

}  // namespace

TEST_CASE("T-498 defaultGroupDir: the preset library's default group") {
    const std::string dir = defaultGroupDir(compile::genericPathToUtf8(kPresetDir));
    REQUIRE(!dir.empty());
    CHECK(dir.find("/groups/broadsheet-2026") == dir.size() - std::string("/groups/broadsheet-2026").size());
    CHECK(fs::is_regular_file(fs::path(dir) / "index.json"));
    CHECK(fs::is_regular_file(fs::path(dir) / "Button.xt"));
    CHECK(defaultGroupDir("").empty());
    CHECK(defaultGroupDir(compile::genericPathToUtf8(kPresetDir / "groups")).empty());  // No library.json.
    CHECK(defaultGroupDir(compile::genericPathToUtf8(kPresetDir / "nowhere")).empty());
}

TEST_CASE(
    "T-498 loadLibrary: the preset library, its group, 39 symbols, token defaults, fonts and textures") {
    SymbolLibrary library;
    REQUIRE(loadLibrary(kPresetDir, library) == Status::kOk);
    CHECK(library.id == "preset");
    CHECK(library.name == "XTFrame presets");
    CHECK(library.readOnly);
    CHECK(library.defaultGroup == kPresetGroup);
    REQUIRE(library.groups.size() == 1);
    const SymbolGroup& group = library.groups.front();
    CHECK(group.id == kPresetGroup);
    CHECK(group.name == "Broadsheet 2026");
    CHECK(group.source.kind == "preset");
    CHECK(group.source.set == "xtframe/broadsheet-2026");
    CHECK(group.source.setVersion == "2.0.0");
    CHECK(group.tokensFile == "tokens.xts");
    CHECK(group.themeFiles == std::vector<std::string>{"tokens-night.xts"});
    CHECK(group.thumbnail == "thumbnails/group.png");
    CHECK(group.problems.empty());
    CHECK(group.symbols.size() == 39);
    CHECK(fs::is_regular_file(fs::path(group.dir) / group.tokensFile));

    const SymbolEntry* button = findSymbol(library, "Button");
    REQUIRE(button != nullptr);
    CHECK(button == findSymbol(library, "Button", kPresetGroup));
    CHECK(findSymbol(library, "Button", "other") == nullptr);
    CHECK(findSymbol(library, "NoSuchSymbol") == nullptr);
    CHECK(button->file == "Button.xt");
    CHECK(button->thumbnail == "thumbnails/Button.png");
    CHECK(button->category == "form");
    CHECK(button->requiredStates == std::vector<std::string>{"Hover", "Pressed", "Focus", "Disabled"});
    CHECK(button->requiredSlots == std::vector<std::string>{"label"});
    REQUIRE(button->ports.size() == 1);
    CHECK(button->ports.front().name == "pressed");
    CHECK(button->ports.front().dir == "out");
    CHECK(button->timelines == std::vector<std::string>{"hover-in", "hover-out", "press"});
    CHECK(button->content.rfind("sha256:", 0) == 0);
    REQUIRE(!button->tokens.empty());
    bool accent = false;
    for (const TokenDefault& token : button->tokens) {
        CHECK(token.name.rfind("--", 0) == 0);
        CHECK(!token.value.empty());
        accent = accent || (token.name == "--color-accent" && token.value == "#1f1c18");
    }
    CHECK(accent);
    REQUIRE(!button->fonts.empty());
    REQUIRE(!button->textures.empty());
    for (const std::string& file : button->fonts) {
        CHECK(fs::is_regular_file(fs::path(group.dir) / file));
    }
    for (const std::string& file : button->textures) {
        CHECK(fs::is_regular_file(fs::path(group.dir) / file));
    }
    // The mobile/ and list/ subsets are entries of the same group.
    const SymbolEntry* joystick = findSymbol(library, "VirtualJoystick");
    REQUIRE(joystick != nullptr);
    CHECK(joystick->file == "mobile/VirtualJoystick.xt");
    CHECK(joystick->thumbnail == "thumbnails/VirtualJoystick.png");
    REQUIRE(findSymbol(library, "ListItem") != nullptr);
    CHECK(findSymbol(library, "ListItem")->file == "list/ListItem.xt");
}

TEST_CASE("T-498 loadLibrary: no library.json, broken library.json, a newer version") {
    SymbolLibrary library;
    CHECK(loadLibrary(fs::path(), library) == Status::kInvalidArgument);
    CHECK(loadLibrary(kPresetDir / "groups", library) == Status::kNotFound);
    const fs::path dir = freshDir("xt_t498_bad_library");
    writeTextT498(dir / "library.json", "not json");
    CHECK(loadLibrary(dir, library) == Status::kInvalidArgument);
    writeTextT498(dir / "library.json", R"({ "version": 1, "id": "Bad Id", "groups": [] })");
    CHECK(loadLibrary(dir, library) == Status::kInvalidArgument);
    writeTextT498(dir / "library.json", R"({ "version": 2, "id": "new", "groups": [] })");
    CHECK(loadLibrary(dir, library) == Status::kUnsupported);
    writeTextT498(dir / "library.json", R"({ "version": 1, "id": "empty", "groups": [] })");
    REQUIRE(loadLibrary(dir, library) == Status::kOk);
    CHECK(library.groups.empty());
    CHECK(library.defaultGroup.empty());
    CHECK(defaultGroupDir(compile::genericPathToUtf8(dir)).empty());
}

TEST_CASE("T-498 loadLibrary: a broken group.json is a problem of the group, not a failure") {
    SymbolLibrary library;
    SUBCASE("not JSON") {
        REQUIRE(loadLibrary(brokenLibrary("xt_t498_group_notjson", "{ this is not json"), library) ==
                Status::kOk);
        REQUIRE(library.groups.size() == 1);
        CHECK(errorCount(library.groups.front()) >= 1);
        CHECK(library.groups.front().name == "g");
        CHECK(library.groups.front().symbols.size() == 1);  // index.json was still read.
        CHECK(library.groups.front().tokensFile == "tokens.xts");
    }
    SUBCASE("no id") {
        REQUIRE(loadLibrary(brokenLibrary("xt_t498_group_noid", R"({ "version": 1, "name": "G" })"),
                            library) == Status::kOk);
        REQUIRE(library.groups.size() == 1);
        CHECK(errorCount(library.groups.front()) == 1);
        CHECK(library.groups.front().problems.front().message.find("'id'") != std::string::npos);
        CHECK(library.groups.front().name == "G");
    }
    SUBCASE("a token path that leaves the group") {
        REQUIRE(
            loadLibrary(
                brokenLibrary("xt_t498_group_escape",
                              R"({ "version": 1, "id": "g", "tokens": "../x", "themes": ["ok.xts", "/etc/x"],
                                               "thumbnail": "../x.png" })"),
                library) == Status::kOk);
        REQUIRE(library.groups.size() == 1);
        const SymbolGroup& group = library.groups.front();
        CHECK(group.tokensFile == "tokens.xts");  // Refused from group.json, taken from the index instead.
        CHECK(group.themeFiles == std::vector<std::string>{"ok.xts"});
        CHECK(group.thumbnail.empty());
        CHECK(errorCount(group) == 3);
    }
    SUBCASE("a missing group directory and a bad group id") {
        const fs::path dir = freshDir("xt_t498_group_missing");
        writeTextT498(dir / "library.json",
                      R"({ "version": 1, "id": "m", "groups": ["gone", "../up", "ok"] })");
        writeTextT498(dir / "groups" / "ok" / "group.json", R"({ "version": 1, "id": "ok" })");
        writeTextT498(dir / "groups" / "ok" / "index.json",
                      R"({ "symbols": [ { "name": "B", "file": "../B.xt" }, { "file": "C.xt" },
                                        { "name": "D", "file": "D.xt", "tokens": ["x", { "name": "--y", "default": "2" }],
                                          "fonts": ["fonts/f.ttf", "../f.ttf"] } ] })");
        REQUIRE(loadLibrary(dir, library) == Status::kOk);
        REQUIRE(library.groups.size() == 3);
        CHECK(library.groups[0].dir.empty());
        CHECK(errorCount(library.groups[0]) == 1);
        CHECK(library.groups[1].dir.empty());
        CHECK(errorCount(library.groups[1]) == 1);
        const SymbolGroup& ok = library.groups[2];
        REQUIRE(ok.symbols.size() == 1);  // B's file escapes, C has no name.
        CHECK(ok.symbols.front().name == "D");
        REQUIRE(ok.symbols.front().tokens.size() == 2);
        CHECK(ok.symbols.front().tokens[0].name == "--x");  // The old bare spelling.
        CHECK(ok.symbols.front().tokens[0].value.empty());
        CHECK(ok.symbols.front().tokens[1].value == "2");
        CHECK(ok.symbols.front().fonts == std::vector<std::string>{"fonts/f.ttf"});
        CHECK(errorCount(ok) == 3);
    }
}

TEST_CASE("T-498 saveLibraryJson: a read-only library is refused, a writable one round-trips") {
    SymbolLibrary preset;
    REQUIRE(loadLibrary(kPresetDir, preset) == Status::kOk);
    CHECK(saveLibraryJson(preset) == Status::kUnsupported);
    const std::string before = readTextT498(kPresetDir / "library.json");
    CHECK(!before.empty());

    const fs::path dir = freshDir("xt_t498_save");
    SymbolLibrary mine;
    mine.id = "mine";
    mine.name = "Mine";
    mine.dir = compile::genericPathToUtf8(dir);
    mine.defaultGroup = "b";
    for (const char* id : {"a", "b"}) {
        SymbolGroup group;
        group.id = id;
        mine.groups.push_back(std::move(group));
    }
    REQUIRE(saveLibraryJson(mine) == Status::kOk);
    SymbolLibrary loaded;
    REQUIRE(loadLibrary(dir, loaded) == Status::kOk);
    CHECK(loaded.id == "mine");
    CHECK(loaded.name == "Mine");
    CHECK_FALSE(loaded.readOnly);
    CHECK(loaded.defaultGroup == "b");
    REQUIRE(loaded.groups.size() == 2);
    CHECK(loaded.groups[0].id == "a");
    CHECK(loaded.groups[1].id == "b");
    CHECK(!loaded.groups[0].problems.empty());  // No directory yet.
    SymbolLibrary bad;
    CHECK(saveLibraryJson(bad) == Status::kInvalidArgument);
}

TEST_CASE("T-498 mountDefaults: the first mount copies the preset group, the second does not") {
    const fs::path userData = freshDir("xt_t498_mount");
    LibraryMounts mounts;
    REQUIRE(mountDefaults(mounts, kPresetDir, userData) == Status::kOk);
    REQUIRE(mounts.libraries.size() == 2);
    CHECK(mounts.libraries[0].id == "preset");
    CHECK(mounts.libraries[0].readOnly);
    const SymbolLibrary& mine = mounts.libraries[1];
    CHECK(mine.id == "mine");
    CHECK_FALSE(mine.readOnly);
    CHECK(mine.dir == compile::genericPathToUtf8(userData / "XTFrame" / "symbols"));
    CHECK(mine.defaultGroup == kPresetGroup);
    REQUIRE(mine.groups.size() == 1);
    CHECK(mine.groups.front().id == kPresetGroup);
    CHECK(mine.groups.front().problems.empty());
    CHECK(mine.groups.front().symbols.size() == 39);
    const fs::path copied = userData / "XTFrame" / "symbols" / "groups" / kPresetGroup;
    CHECK(fs::is_regular_file(copied / "Button.xt"));
    CHECK(fs::is_regular_file(copied / "mobile" / "VirtualJoystick.xt"));
    CHECK(fs::is_regular_file(copied / "thumbnails" / "Button.png"));
    CHECK(fs::is_regular_file(copied / "fonts" / "NotoSerif-Regular.ttf"));
    CHECK(readTextT498(copied / "tokens.xts") ==
          readTextT498(kPresetDir / "groups" / kPresetGroup / "tokens.xts"));
    CHECK(defaultGroupDir(mine.dir) == compile::genericPathToUtf8(copied));

    // The user edits the copy and deletes its themes: a second mount keeps both as they are.
    writeTextT498(copied / "Button.xt", "<xt>edited</xt>");
    std::error_code ec;
    fs::remove(copied / "tokens-night.xts", ec);
    LibraryMounts again;
    REQUIRE(mountDefaults(again, kPresetDir, userData) == Status::kOk);
    REQUIRE(again.libraries.size() == 2);
    CHECK(readTextT498(copied / "Button.xt") == "<xt>edited</xt>");
    CHECK_FALSE(fs::exists(copied / "tokens-night.xts"));
    CHECK(again.libraries[1].groups.size() == 1);
    // Mounting into a list that already holds them replaces, in front of the rest.
    SymbolLibrary other;
    other.id = "team";
    other.dir = "/team/symbols";
    again.libraries.push_back(other);
    REQUIRE(mountDefaults(again, kPresetDir, userData) == Status::kOk);
    REQUIRE(again.libraries.size() == 3);
    CHECK(again.libraries[0].id == "preset");
    CHECK(again.libraries[1].id == "mine");
    CHECK(again.libraries[2].id == "team");

    LibraryMounts none;
    CHECK(mountDefaults(none, kPresetDir / "groups", userData) == Status::kNotFound);
    CHECK(mountDefaults(none, kPresetDir, fs::path()) == Status::kInvalidArgument);
}

TEST_CASE("T-498 reloadPreset: the preset copy replaces the edited group, the old one goes to .trash") {
    const fs::path userData = freshDir("xt_t498_reload");
    LibraryMounts mounts;
    REQUIRE(mountDefaults(mounts, kPresetDir, userData) == Status::kOk);
    const fs::path mineDir = userData / "XTFrame" / "symbols";
    const fs::path group = mineDir / "groups" / kPresetGroup;
    writeTextT498(group / "Button.xt", "<xt>edited</xt>");
    writeTextT498(group / "Extra.xt", "<xt>extra</xt>");

    REQUIRE(reloadPreset(mounts, kPresetGroup, false) == Status::kOk);
    CHECK(readTextT498(group / "Button.xt") ==
          readTextT498(kPresetDir / "groups" / kPresetGroup / "Button.xt"));
    CHECK_FALSE(fs::exists(group / "Extra.xt"));
    REQUIRE(fs::is_directory(mineDir / ".trash"));
    size_t trashed = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(mineDir / ".trash")) {
        CHECK(entry.path().filename().string().rfind(std::string(kPresetGroup) + "-", 0) == 0);
        CHECK(readTextT498(entry.path() / "Button.xt") == "<xt>edited</xt>");
        CHECK(fs::is_regular_file(entry.path() / "Extra.xt"));
        ++trashed;
    }
    CHECK(trashed == 1);
    const SymbolLibrary& mine = mounts.libraries[1];
    REQUIRE(mine.groups.size() == 1);
    CHECK(mine.groups.front().id == kPresetGroup);
    CHECK(mine.groups.front().symbols.size() == 39);
    CHECK(mine.groups.front().problems.empty());

    // A group the user deleted comes back; keepOld keeps the current one as <id>@<version>.
    std::error_code ec;
    fs::remove_all(group, ec);
    REQUIRE(reloadPreset(mounts, kPresetGroup, true) == Status::kOk);
    CHECK(fs::is_regular_file(group / "Button.xt"));
    writeTextT498(group / "Button.xt", "<xt>edited again</xt>");
    REQUIRE(reloadPreset(mounts, kPresetGroup, true) == Status::kOk);
    const fs::path kept = mineDir / "groups" / "broadsheet-2026@2.0.0";
    REQUIRE(fs::is_directory(kept));
    CHECK(readTextT498(kept / "Button.xt") == "<xt>edited again</xt>");
    CHECK(readTextT498(group / "Button.xt") ==
          readTextT498(kPresetDir / "groups" / kPresetGroup / "Button.xt"));
    REQUIRE(mounts.libraries[1].groups.size() == 2);
    CHECK(mounts.libraries[1].groups[0].id == kPresetGroup);
    CHECK(mounts.libraries[1].groups[1].id == "broadsheet-2026@2.0.0");
    CHECK(mounts.libraries[1].groups[1].problems.size() ==
          1);  // group.json still says broadsheet-2026 (a warning).
    CHECK(mounts.libraries[1].groups[1].problems.front().level == compile::Diagnostic::Level::Warning);
    CHECK(mounts.libraries[1].groups[1].symbols.size() == 39);
    const std::string json = readTextT498(mineDir / "library.json");
    CHECK(json.find("\"broadsheet-2026@2.0.0\"") != std::string::npos);
    CHECK(json.find("\"default\": \"broadsheet-2026\"") != std::string::npos);

    CHECK(reloadPreset(mounts, "no-such-group", false) == Status::kNotFound);
    CHECK(reloadPreset(mounts, "../x", false) == Status::kInvalidArgument);
    LibraryMounts readOnlyOnly;
    REQUIRE(loadLibrary(kPresetDir, readOnlyOnly.libraries.emplace_back()) == Status::kOk);
    CHECK(reloadPreset(readOnlyOnly, kPresetGroup, false) == Status::kNotFound);
}

// --- T-498b: mounts, the copy rule, staleness, lint helpers ---------------------------------------

namespace {

/// A one-group library at `dir` with the symbol `A` (file `A.xt`, tokens `--x`, no assets).
fs::path smallLibrary(const fs::path& dir, const char* id) {
    writeTextT498(dir / "library.json", std::string(R"({ "version": 1, "id": ")") + id +
                                            R"(", "name": "Small", "readOnly": false, "groups": ["g"] })");
    writeTextT498(dir / "groups" / "g" / "group.json",
                  R"({ "version": 1, "id": "g", "name": "G", "tokens": "tokens.xts" })");
    writeTextT498(dir / "groups" / "g" / "index.json",
                  R"({ "version": 1, "tokens": "tokens.xts", "symbols": [ { "name": "A", "file": "A.xt",
                        "tokens": [ { "name": "--x", "default": "1px" } ], "fonts": [], "textures": [] } ] })");
    writeTextT498(dir / "groups" / "g" / "tokens.xts", ":root { --x: 1px; }\n");
    writeTextT498(dir / "groups" / "g" / "A.xt", "<xt><symbol name=\"A\"><div/></symbol></xt>\n");
    return dir;
}

/// A blank UI Project (createUiProject without a template) in a fresh directory.
UiProject blankProjectT498b(const char* name) {
    const fs::path dir = freshDir(name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    UiProject project;
    std::string error;
    REQUIRE_MESSAGE(createUiProject(compile::genericPathToUtf8(dir), "Blank", "blank", "", &project,
                                    &error) == Status::kOk,
                    error);
    return project;
}

bool listsT498b(const std::vector<std::string>& list, std::string_view what) {
    for (const std::string& item : list) {
        if (item == what || item.rfind(std::string(what) + ":", 0) == 0 ||
            item.rfind(std::string(what) + " ->", 0) == 0) {
            return true;
        }
    }
    return false;
}

compile::RuleSet parseT498b(const fs::path& file) {
    compile::DiagnosticSink sink;
    compile::RuleSet rules;
    compile::parseXts(readTextT498(file), compile::genericPathToUtf8(file), 1, sink, rules);
    return rules;
}

}  // namespace

TEST_CASE(
    "T-498b useSymbols: Button into a new project copies the three kinds of file and appends the tokens") {
    const fs::path userData = freshDir("xt_t498b_use_user");
    LibraryMounts mounts;
    REQUIRE(mountDefaults(mounts, kPresetDir, userData) == Status::kOk);
    const UiProject project = blankProjectT498b("xt_t498b_use");
    const fs::path root = compile::pathFromUtf8(project.rootDir);

    const std::string_view names[] = {"Button"};
    const UseReport report = useSymbols(mounts, project, names);
    REQUIRE(report.status == Status::kOk);
    CHECK(report.notFound.empty());
    CHECK(report.renamed.empty());
    CHECK(listsT498b(report.written, "shared/symbols/Button.xt"));
    CHECK(listsT498b(report.written, "shared/fonts/NotoSerifDisplay-ExtraCondensedBlack.ttf"));
    CHECK(listsT498b(report.written, "shared/fonts/NotoSerifDisplay-ExtraCondensedBold.ttf"));
    CHECK(listsT498b(report.written, "shared/textures/hatch-ink.png"));
    CHECK(listsT498b(report.written, "shared/textures/hatch-night.png"));  // T-498c: the night texture too.
    CHECK(listsT498b(report.written, "shared/tokens.xts"));
    CHECK(listsT498b(report.written, "shared/themes/night.xts"));
    CHECK(listsT498b(report.written, "shared/symbols/index.json"));
    CHECK(readTextT498(root / "shared" / "symbols" / "Button.xt") ==
          readTextT498(kPresetDir / "groups" / kPresetGroup / "Button.xt"));
    CHECK(readTextT498(root / "shared" / "fonts" / "NotoSerifDisplay-ExtraCondensedBold.ttf") ==
          readTextT498(kPresetDir / "groups" / kPresetGroup / "fonts" /
                       "NotoSerifDisplay-ExtraCondensedBold.ttf"));

    // The token sheet: one `/* added with ... */` section with the two @font-face blocks of the
    // copied fonts and Button's tokens, values from the group's tokens.xts, paths relative to it.
    const std::string tokens = readTextT498(root / "shared" / "tokens.xts");
    CHECK(tokens.find("/* added with Button from broadsheet-2026 */") != std::string::npos);
    CHECK(tokens.find("@font-face { font-family: \"Noto Serif Display ExtraCondensed\"; "
                      "src: \"fonts/NotoSerifDisplay-ExtraCondensedBlack.ttf\"; font-weight: 900; }") !=
          std::string::npos);
    CHECK(tokens.find("src: \"fonts/NotoSerif-Regular.ttf\"") == std::string::npos);  // Not one of Button's.
    CHECK(tokens.find("  --color-accent: #1f1c18;\n") != std::string::npos);
    CHECK(tokens.find("  --tex-hatch: url(\"textures/hatch-ink.png\");\n") != std::string::npos);
    CHECK(listsT498b(report.tokensAdded, "--color-accent"));
    CHECK(listsT498b(report.tokensAdded, "--tex-hatch"));
    CHECK(listsT498b(report.tokensAdded, "night:--color-accent"));
    const compile::RuleSet parsed = parseT498b(root / "shared" / "tokens.xts");
    CHECK(parsed.fontFaces.size() == 2);
    CHECK(parsed.tokens.size() == report.tokensAdded.size() - [&]() {
        size_t theme = 0;
        for (const std::string& t : report.tokensAdded) {
            if (t.rfind("night:", 0) == 0) ++theme;
        }
        return theme;
    }());
    // The night theme: Button's tokens with the theme's values, the night texture it names copied
    // and referenced from the theme file (T-498c: index.json's textures[] covers every theme file
    // of the group, 49 §49.12).
    const std::string night = readTextT498(root / "shared" / "themes" / "night.xts");
    CHECK(night.find("/* added with Button from broadsheet-2026 */") != std::string::npos);
    CHECK(night.find("  --color-accent: #d9d3c4;\n") != std::string::npos);
    CHECK(night.find("  --tex-hatch: url(\"../textures/hatch-night.png\");\n") != std::string::npos);
    CHECK(listsT498b(report.tokensAdded, "night:--tex-hatch"));
    CHECK_FALSE(listsT498b(report.skipped, "night:--tex-hatch"));
    CHECK(readTextT498(root / "shared" / "textures" / "hatch-night.png") ==
          readTextT498(kPresetDir / "groups" / kPresetGroup / "textures" / "hatch-night.png"));
    // The project index records the source.
    const std::string index = readTextT498(root / "shared" / "symbols" / "index.json");
    CHECK(index.find("\"name\": \"Button\"") != std::string::npos);
    CHECK(index.find("\"group\": \"broadsheet-2026\"") != std::string::npos);
    CHECK(index.find("\"set\": \"xtframe/broadsheet-2026\"") != std::string::npos);
    CHECK(index.find("\"sha256\": \"sha256:") != std::string::npos);
    CHECK_FALSE(isCopyStale(mounts, project, "Button"));

    // A second call changes nothing: every file is skipped, no token appended.
    const UseReport again = useSymbols(mounts, project, names);
    CHECK(again.status == Status::kOk);
    CHECK(again.written.empty());
    CHECK(again.tokensAdded.empty());
    CHECK(listsT498b(again.skipped, "shared/symbols/Button.xt"));
    CHECK(listsT498b(again.skipped, "shared/fonts/NotoSerifDisplay-ExtraCondensedBlack.ttf"));
    CHECK(readTextT498(root / "shared" / "tokens.xts") == tokens);
    CHECK(readTextT498(root / "shared" / "themes" / "night.xts") == night);

    // A second symbol only appends what is still missing (its own section).
    const std::string_view more[] = {"Toggle"};
    const UseReport toggle = useSymbols(mounts, project, more);
    CHECK(toggle.status == Status::kOk);
    CHECK(listsT498b(toggle.written, "shared/symbols/Toggle.xt"));
    CHECK_FALSE(listsT498b(toggle.tokensAdded, "--color-accent"));
    const std::string tokens2 = readTextT498(root / "shared" / "tokens.xts");
    CHECK(tokens2.rfind(tokens, 0) == 0);
    CHECK(tokens2.find("/* added with Toggle from broadsheet-2026 */") != std::string::npos);
    {
        // Toggle adds the body faces it needs; the display faces Button brought are not repeated.
        const compile::RuleSet both = parseT498b(root / "shared" / "tokens.xts");
        CHECK(both.fontFaces.size() > 2);
        size_t black = 0;
        for (const compile::FontFaceDef& face : both.fontFaces) {
            if (face.family == "Noto Serif Display ExtraCondensed" && face.weight == 900) ++black;
        }
        CHECK(black == 1);
    }

    // Not found, and a project symbol of the same name is never overwritten.
    const std::string_view none[] = {"NoSuchSymbol"};
    CHECK(useSymbols(mounts, project, none).notFound == std::vector<std::string>{"NoSuchSymbol"});
    writeTextT498(root / "shared" / "symbols" / "Slider.xt", "<xt>mine</xt>");
    const std::string_view slider[] = {"Slider"};
    const UseReport kept = useSymbols(mounts, project, slider);
    CHECK(kept.status == Status::kOk);
    CHECK(readTextT498(root / "shared" / "symbols" / "Slider.xt") == "<xt>mine</xt>");
    CHECK(listsT498b(kept.skipped, "shared/symbols/Slider.xt"));
    CHECK(isCopyStale(mounts, project, "Slider"));  // Differs from the library.
}

TEST_CASE("T-498b useSymbols: a font of the same name with other content is copied under a hashed name") {
    const fs::path userData = freshDir("xt_t498b_rename_user");
    LibraryMounts mounts;
    REQUIRE(mountDefaults(mounts, kPresetDir, userData) == Status::kOk);
    const UiProject project = blankProjectT498b("xt_t498b_rename");
    const fs::path root = compile::pathFromUtf8(project.rootDir);
    writeTextT498(root / "shared" / "fonts" / "NotoSerifDisplay-ExtraCondensedBlack.ttf",
                  "not the same font");
    writeTextT498(root / "shared" / "textures" / "hatch-ink.png", "not the same texture");

    const std::string_view names[] = {"Button"};
    const UseReport report = useSymbols(mounts, project, names);
    REQUIRE(report.status == Status::kOk);
    REQUIRE(report.renamed.size() == 2);
    const std::string fontLine = report.renamed[0];
    CHECK(fontLine.rfind("fonts/NotoSerifDisplay-ExtraCondensedBlack.ttf -> "
                         "shared/fonts/NotoSerifDisplay-ExtraCondensedBlack-",
                         0) == 0);
    const std::string renamedFont = fontLine.substr(fontLine.find("-> ") + 3);
    CHECK(renamedFont.size() ==
          std::string("shared/fonts/NotoSerifDisplay-ExtraCondensedBlack-").size() + 8 + 4);
    CHECK(fs::is_regular_file(root / compile::pathFromUtf8(renamedFont)));
    CHECK(readTextT498(root / "shared" / "fonts" / "NotoSerifDisplay-ExtraCondensedBlack.ttf") ==
          "not the same font");
    CHECK(report.renamed[1].rfind("textures/hatch-ink.png -> shared/textures/hatch-ink-", 0) == 0);
    const std::string renamedTexture = report.renamed[1].substr(report.renamed[1].find("-> ") + 3);
    // The appended lines reference the renamed files.
    const std::string tokens = readTextT498(root / "shared" / "tokens.xts");
    CHECK(tokens.find("src: \"" + renamedFont.substr(std::string("shared/").size()) + "\"") !=
          std::string::npos);
    CHECK(tokens.find("--tex-hatch: url(\"" + renamedTexture.substr(std::string("shared/").size()) +
                      "\");") != std::string::npos);
    CHECK(tokens.find("src: \"fonts/NotoSerifDisplay-ExtraCondensedBlack.ttf\"") == std::string::npos);
    // Idempotent: the hashed copies are found again and skipped.
    const UseReport again = useSymbols(mounts, project, names);
    CHECK(again.written.empty());
    CHECK(again.renamed.size() == 2);
    CHECK(listsT498b(again.skipped, renamedFont));
}

TEST_CASE("T-498b useSymbols: a name that is a path is refused as a whole; the group argument restricts") {
    const fs::path userData = freshDir("xt_t498b_refuse_user");
    LibraryMounts mounts;
    REQUIRE(mountDefaults(mounts, kPresetDir, userData) == Status::kOk);
    const UiProject project = blankProjectT498b("xt_t498b_refuse");
    const fs::path root = compile::pathFromUtf8(project.rootDir);
    for (const std::string_view bad :
         {"../Button", "sub/Button", "Button.xt", "xt-standard:Button", "..", ""}) {
        CAPTURE(bad);
        const std::string_view names[] = {"Toggle", bad};
        const UseReport report = useSymbols(mounts, project, names);
        CHECK(report.status == Status::kInvalidArgument);
        CHECK(report.written.empty());
        CHECK_FALSE(fs::exists(root / "shared" / "symbols" / "Toggle.xt"));
    }
    const std::string_view names[] = {"Button"};
    CHECK(useSymbols(mounts, project, names, "no-such-group").notFound == std::vector<std::string>{"Button"});
    CHECK(useSymbols(mounts, project, names, kPresetGroup).status == Status::kOk);
    CHECK(fs::is_regular_file(root / "shared" / "symbols" / "Button.xt"));
    CHECK_FALSE(isCopyStale(mounts, project, "Button"));
    writeTextT498(root / "shared" / "symbols" / "Button.xt",
                  readTextT498(root / "shared" / "symbols" / "Button.xt") + "\n");
    CHECK(isCopyStale(mounts, project, "Button"));
    CHECK_FALSE(isCopyStale(mounts, project, "Toggle"));        // No copy.
    CHECK_FALSE(isCopyStale(mounts, project, "NoSuchSymbol"));  // No library has it.
    CHECK_FALSE(isCopyStale(mounts, project, "../Button"));
}

TEST_CASE("T-498b mounts: mount, unmount, the persisted list and findSymbol across libraries") {
    const fs::path userData = freshDir("xt_t498b_mounts");
    const fs::path team = smallLibrary(freshDir("xt_t498b_team"), "team");
    LibraryMounts mounts;
    REQUIRE(mountDefaults(mounts, kPresetDir, userData) == Status::kOk);
    CHECK(mounts.libraries[0].scope == LibraryScope::Preset);
    CHECK(mounts.libraries[1].scope == LibraryScope::User);
    CHECK(loadMounts(userData, mounts) == Status::kNotFound);

    REQUIRE(mount(mounts, team) == Status::kOk);
    REQUIRE(mounts.libraries.size() == 3);
    CHECK(mounts.libraries[2].id == "team");
    CHECK(mounts.libraries[2].scope == LibraryScope::Extra);
    REQUIRE(mounts.entries.size() == 1);
    CHECK(mounts.entries[0].enabled);
    CHECK(mount(mounts, team) == Status::kOk);  // Again: replaced in place, listed once.
    CHECK(mounts.libraries.size() == 3);
    CHECK(mounts.entries.size() == 1);
    CHECK(mount(mounts, userData / "nowhere") == Status::kNotFound);

    const SymbolLibrary* library = nullptr;
    const SymbolGroup* group = nullptr;
    const SymbolEntry* a = findSymbol(mounts, "A", {}, &library, &group);
    REQUIRE(a != nullptr);
    CHECK(library->id == "team");
    CHECK(group->id == "g");
    const SymbolEntry* button = findSymbol(mounts, "Button", {}, &library, &group);
    REQUIRE(button != nullptr);
    CHECK(library->scope == LibraryScope::Preset);  // The first library in mount order wins.
    CHECK(findSymbol(mounts, "Button", "g") == nullptr);
    CHECK(findSymbol(mounts, "Nope") == nullptr);
    REQUIRE(standardGroup(mounts) != nullptr);
    CHECK(standardGroup(mounts)->id == kPresetGroup);
    CHECK(standardGroup(mounts)->dir == mounts.libraries[0].groups[0].dir);

    REQUIRE(saveMounts(userData, mounts) == Status::kOk);
    const std::string json = readTextT498(userData / "XTFrame" / "symbol-libraries.json");
    CHECK(json.find("\"version\": 1") != std::string::npos);
    CHECK(json.find("\"enabled\": true") != std::string::npos);
    LibraryMounts loaded;
    REQUIRE(mountDefaults(loaded, kPresetDir, userData) == Status::kOk);
    REQUIRE(loadMounts(userData, loaded) == Status::kOk);
    REQUIRE(loaded.libraries.size() == 3);
    CHECK(loaded.libraries[2].id == "team");
    CHECK(loaded.libraries[2].scope == LibraryScope::Extra);
    REQUIRE(loaded.entries.size() == 1);

    // Disabled: listed, not mounted. Unmounted: gone from both.
    loaded.entries[0].enabled = false;
    REQUIRE(saveMounts(userData, loaded) == Status::kOk);
    LibraryMounts disabled;
    REQUIRE(mountDefaults(disabled, kPresetDir, userData) == Status::kOk);
    REQUIRE(loadMounts(userData, disabled) == Status::kOk);
    CHECK(disabled.libraries.size() == 2);
    REQUIRE(disabled.entries.size() == 1);
    CHECK_FALSE(disabled.entries[0].enabled);
    CHECK(unmount(mounts, "team"));
    CHECK(mounts.libraries.size() == 2);
    CHECK(mounts.entries.empty());
    CHECK_FALSE(unmount(mounts, "team"));
    CHECK(findSymbol(mounts, "A") == nullptr);

    // A broken list file.
    writeTextT498(userData / "XTFrame" / "symbol-libraries.json", "[1, 2]");
    CHECK(loadMounts(userData, mounts) == Status::kInvalidArgument);
    writeTextT498(userData / "XTFrame" / "symbol-libraries.json", R"({ "version": 2, "libraries": [] })");
    CHECK(loadMounts(userData, mounts) == Status::kUnsupported);
    CHECK(saveMounts(fs::path(), mounts) == Status::kInvalidArgument);
}

TEST_CASE("T-498b mountProjectLibrary: <project>/shared/symbol-library/ mounts with the project") {
    const fs::path userData = freshDir("xt_t498b_project_user");
    LibraryMounts mounts;
    REQUIRE(mountDefaults(mounts, kPresetDir, userData) == Status::kOk);
    const UiProject project = blankProjectT498b("xt_t498b_project");
    CHECK(mountProjectLibrary(mounts, project) == Status::kNotFound);
    CHECK(mounts.libraries.size() == 2);
    const fs::path root = compile::pathFromUtf8(project.rootDir);
    smallLibrary(root / "shared" / "symbol-library", "own");
    REQUIRE(mountProjectLibrary(mounts, project) == Status::kOk);
    REQUIRE(mounts.libraries.size() == 3);
    CHECK(mounts.libraries[2].id == "own");
    CHECK(mounts.libraries[2].scope == LibraryScope::Project);
    CHECK(mounts.entries.empty());  // Never persisted.
    CHECK(findSymbol(mounts, "A") != nullptr);
    // Its symbols drag in like any other library's; the copy is then not stale.
    const std::string_view names[] = {"A"};
    const UseReport report = useSymbols(mounts, project, names);
    CHECK(report.status == Status::kOk);
    CHECK(listsT498b(report.written, "shared/symbols/A.xt"));
    CHECK(listsT498b(report.tokensAdded, "--x"));
    CHECK_FALSE(isCopyStale(mounts, project, "A"));
    CHECK(unmount(mounts, "own"));
    CHECK(mounts.libraries.size() == 2);
}

TEST_CASE("T-498b themeNameOf, sameAsDefaultTokens and loadGroup") {
    CHECK(themeNameOf("tokens-night.xts") == "night");
    CHECK(themeNameOf("themes/dark.xts") == "dark");
    CHECK(themeNameOf("tokens.xts") == "tokens");
    CHECK(themeNameOf("tokens-") == "tokens-");

    const fs::path dir = freshDir("xt_t498b_same");
    const fs::path group = kPresetDir / "groups" / kPresetGroup;
    writeTextT498(dir / "tokens.xts",
                  ":root { --color-bg: #e6e2d6; --safe-x: 32px; --color-text: #000; --tex-grain: "
                  "url(\"textures/grain-paper.png\"); }\n");
    const std::vector<TokenDefault> same = sameAsDefaultTokens(dir / "tokens.xts", group / "tokens.xts");
    REQUIRE(same.size() == 2);
    CHECK(same[0].name == "--color-bg");
    CHECK(same[0].value == "#e6e2d6");
    CHECK(same[1].name == "--tex-grain");
    CHECK(sameAsDefaultTokens(dir / "missing.xts", group / "tokens.xts").empty());

    SymbolGroup loaded;
    REQUIRE(loadGroup(group, loaded) == Status::kOk);
    CHECK(loaded.id == kPresetGroup);
    CHECK(loaded.tokensFile == "tokens.xts");
    CHECK(loaded.themeFiles == std::vector<std::string>{"tokens-night.xts"});
    CHECK(loaded.symbols.size() == 39);
    CHECK(loaded.problems.empty());
    CHECK(loadGroup(dir / "nowhere", loaded) == Status::kNotFound);
}

// --- T-501 (Docs/design/49-symbol-sets.md 49.1, 49.5, 49.6; Docs/tasks/T-501.md item 4) --------

namespace {

bool hasT501(const std::vector<std::string>& list, std::string_view what) { return listsT498b(list, what); }

}  // namespace

TEST_CASE("T-501 setGroupProblems: the persisted mark round-trips through loadLibrary and saveLibraryJson") {
    const fs::path userData = freshDir("xt_t501_problems_user");
    LibraryMounts mounts;
    REQUIRE(mountDefaults(mounts, kPresetDir, userData) == Status::kOk);
    const fs::path mine = userData / "XTFrame" / "symbols";
    SymbolLibrary library;
    REQUIRE(loadLibrary(mine, library) == Status::kOk);
    REQUIRE(library.groups.size() == 1);
    CHECK(library.groups.front().lint.errors == 0);
    CHECK(library.groups.front().lint.warnings == 0);

    CHECK(setGroupProblems(mine / "library.json", kPresetGroup, LintSummary{3, 1}) == Status::kOk);
    CHECK(setGroupProblems(mine, "not a group id!", LintSummary{1, 0}) == Status::kInvalidArgument);
    CHECK(setGroupProblems(mine / "nowhere.json", kPresetGroup, LintSummary{1, 0}) == Status::kNotFound);
    REQUIRE(loadLibrary(mine, library) == Status::kOk);
    CHECK(library.groups.front().lint.errors == 3);
    CHECK(library.groups.front().lint.warnings == 1);
    CHECK(readTextT498(mine / "library.json").find("\"problems\"") != std::string::npos);
    // saveLibraryJson keeps the mark; the other keys stay.
    CHECK(saveLibraryJson(library) == Status::kOk);
    REQUIRE(loadLibrary(mine, library) == Status::kOk);
    CHECK(library.groups.front().lint.errors == 3);
    CHECK(library.defaultGroup == kPresetGroup);
    // {0, 0} removes it (the directory form of the first argument).
    CHECK(setGroupProblems(mine, kPresetGroup, LintSummary{}) == Status::kOk);
    REQUIRE(loadLibrary(mine, library) == Status::kOk);
    CHECK(library.groups.front().lint.errors == 0);
    CHECK(readTextT498(mine / "library.json").find("\"problems\"") == std::string::npos);
}

TEST_CASE(
    "T-501 copyGroup, renameGroup, deleteGroup: a preset group copied to my library, renamed, trashed") {
    const fs::path userData = freshDir("xt_t501_groups_user");
    LibraryMounts mounts;
    REQUIRE(mountDefaults(mounts, kPresetDir, userData) == Status::kOk);
    const fs::path mine = userData / "XTFrame" / "symbols";

    // The user library already holds `broadsheet-2026`: the copy gets `-2`.
    std::string targetId;
    REQUIRE(copyGroup(kPresetDir, kPresetGroup, mine, &targetId) == Status::kOk);
    CHECK(targetId == std::string(kPresetGroup) + "-2");
    CHECK(fs::is_regular_file(mine / "groups" / targetId / "Button.xt"));
    CHECK(fs::is_regular_file(mine / "groups" / targetId / "fonts" / "NotoSerif-Regular.ttf"));
    SymbolLibrary library;
    REQUIRE(loadLibrary(mine, library) == Status::kOk);
    REQUIRE(library.groups.size() == 2);
    CHECK(library.groups[1].id == targetId);
    CHECK(library.groups[1].name == "Broadsheet 2026");
    CHECK(library.groups[1].source.set == "xtframe/broadsheet-2026");  // The source is kept.
    CHECK(library.groups[1].symbols.size() == 39);
    CHECK(readTextT498(mine / "groups" / targetId / "group.json").find("\"id\": \"" + targetId + "\"") !=
          std::string::npos);
    // Into a read-only library: refused; a missing group: not found; a bad id: invalid.
    CHECK(copyGroup(mine, targetId, kPresetDir) == Status::kUnsupported);
    CHECK(copyGroup(kPresetDir, "no-such-group", mine) == Status::kNotFound);
    CHECK(copyGroup(kPresetDir, "Bad Id", mine) == Status::kInvalidArgument);

    // Rename: group.json's name only.
    CHECK(renameGroup(mine, targetId, "  My broadsheet ") == Status::kOk);
    REQUIRE(loadLibrary(mine, library) == Status::kOk);
    CHECK(library.groups[1].name == "My broadsheet");
    CHECK(library.groups[1].id == targetId);
    CHECK(renameGroup(mine, targetId, "   ") == Status::kInvalidArgument);
    CHECK(renameGroup(mine, "no-such-group", "x") == Status::kNotFound);
    CHECK(renameGroup(kPresetDir, kPresetGroup, "x") == Status::kUnsupported);

    // Delete: the directory moves to .trash/<id>-<seconds>/, library.json drops it.
    std::string trashed;
    CHECK(deleteGroup(mine, targetId, &trashed) == Status::kOk);
    CHECK(trashed.rfind(".trash/" + targetId + "-", 0) == 0);
    CHECK(fs::is_regular_file(mine / compile::pathFromUtf8(trashed) / "Button.xt"));
    CHECK_FALSE(fs::exists(mine / "groups" / targetId));
    REQUIRE(loadLibrary(mine, library) == Status::kOk);
    CHECK(library.groups.size() == 1);
    CHECK(deleteGroup(mine, targetId) == Status::kNotFound);
    CHECK(deleteGroup(kPresetDir, kPresetGroup) == Status::kUnsupported);
    // Deleting the default group clears `default`.
    CHECK(deleteGroup(mine, kPresetGroup) == Status::kOk);
    REQUIRE(loadLibrary(mine, library) == Status::kOk);
    CHECK(library.groups.empty());
    CHECK(library.defaultGroup.empty());
}

TEST_CASE("T-501 updateCopy: an edited project copy is overwritten, the tokens stay; the undo snapshot") {
    const fs::path userData = freshDir("xt_t501_update_user");
    LibraryMounts mounts;
    REQUIRE(mountDefaults(mounts, kPresetDir, userData) == Status::kOk);
    const UiProject project = blankProjectT498b("xt_t501_update");
    const fs::path root = compile::pathFromUtf8(project.rootDir);
    const std::string_view names[] = {"Button"};
    REQUIRE(useSymbols(mounts, project, names).status == Status::kOk);
    const std::string tokens = readTextT498(root / "shared" / "tokens.xts");
    const std::string night = readTextT498(root / "shared" / "themes" / "night.xts");
    const std::string original = readTextT498(root / "shared" / "symbols" / "Button.xt");

    // The project edits its copy; the token file gains a line of its own.
    writeTextT498(root / "shared" / "symbols" / "Button.xt", original + "<!-- mine -->\n");
    writeTextT498(root / "shared" / "tokens.xts", tokens + ":root { --color-accent: #ff0000; }\n");
    CHECK(isCopyStale(mounts, project, "Button"));

    const std::vector<ProjectFileState> snapshot = snapshotForUse(project, names);
    bool sawCopy = false;
    bool sawIndex = false;
    for (const ProjectFileState& state : snapshot) {
        if (state.path == "shared/symbols/Button.xt") {
            sawCopy = true;
            CHECK(state.existed);
            CHECK(state.content == original + "<!-- mine -->\n");
        }
        sawIndex = sawIndex || (state.path == "shared/symbols/index.json" && state.existed);
    }
    CHECK(sawCopy);
    CHECK(sawIndex);

    const UseReport report = updateCopy(mounts, project, "Button");
    REQUIRE(report.status == Status::kOk);
    CHECK(report.notFound.empty());
    CHECK(report.tokensAdded.empty());
    CHECK(hasT501(report.written, "shared/symbols/Button.xt"));
    CHECK(hasT501(report.written, "shared/symbols/index.json"));
    CHECK_FALSE(hasT501(report.written, "shared/tokens.xts"));
    CHECK_FALSE(hasT501(report.written, "shared/themes/night.xts"));
    CHECK(hasT501(report.skipped, "shared/fonts/NotoSerifDisplay-ExtraCondensedBlack.ttf"));
    CHECK(readTextT498(root / "shared" / "symbols" / "Button.xt") == original);
    CHECK(readTextT498(root / "shared" / "tokens.xts") == tokens + ":root { --color-accent: #ff0000; }\n");
    CHECK(readTextT498(root / "shared" / "themes" / "night.xts") == night);
    CHECK_FALSE(isCopyStale(mounts, project, "Button"));

    // The changes: Button.xt existed (before = the edited text), index.json existed.
    const std::vector<FileChange> changes = changesOfUse(snapshot, project, report);
    REQUIRE(changes.size() == report.written.size());
    CHECK(changes.front().path == "shared/symbols/Button.xt");
    CHECK(changes.front().existed);
    CHECK(changes.front().before == original + "<!-- mine -->\n");
    CHECK(changes.front().after == original);

    // Not found, and a bad name.
    CHECK(updateCopy(mounts, project, "NoSuchSymbol").notFound == std::vector<std::string>{"NoSuchSymbol"});
    CHECK(updateCopy(mounts, project, "../x").status == Status::kInvalidArgument);

    // A first use through the snapshot path: every written file is new (existed == false).
    const std::string_view toggle[] = {"Toggle"};
    const std::vector<ProjectFileState> before = snapshotForUse(project, toggle);
    const UseReport used = useSymbols(mounts, project, toggle);
    REQUIRE(used.status == Status::kOk);
    const std::vector<FileChange> fresh = changesOfUse(before, project, used);
    REQUIRE(fresh.size() == used.written.size());
    for (const FileChange& change : fresh) {
        if (change.path == "shared/symbols/Toggle.xt") {
            CHECK_FALSE(change.existed);
            CHECK(change.before.empty());
            CHECK(change.after == readTextT498(kPresetDir / "groups" / kPresetGroup / "Toggle.xt"));
        } else if (change.path == "shared/tokens.xts") {
            CHECK(change.existed);
            CHECK(change.before == tokens + ":root { --color-accent: #ff0000; }\n");
            CHECK(change.after.rfind(change.before, 0) == 0);
        }
    }
    CHECK(snapshotForUse(UiProject(), toggle).empty());
}

TEST_CASE(
    "T-501 saveAsGroup: project symbols become a lint-clean group with their tokens, fonts and textures") {
    const fs::path userData = freshDir("xt_t501_save_user");
    LibraryMounts mounts;
    REQUIRE(mountDefaults(mounts, kPresetDir, userData) == Status::kOk);
    const fs::path mine = userData / "XTFrame" / "symbols";
    const UiProject project = blankProjectT498b("xt_t501_save");
    const fs::path root = compile::pathFromUtf8(project.rootDir);
    const std::string_view names[] = {"Button", "Toggle"};
    REQUIRE(useSymbols(mounts, project, names).status == Status::kOk);
    // The project changes a value: the group takes the project's.
    const std::string tokens = readTextT498(root / "shared" / "tokens.xts");
    writeTextT498(root / "shared" / "tokens.xts", tokens + ":root { --color-focus: #00ff00; }\n");

    std::string error;
    CHECK(saveAsGroup(project, names, mine, "my-controls", "My controls", &error, &mounts) == Status::kOk);
    CHECK(error.empty());
    const fs::path group = mine / "groups" / "my-controls";
    CHECK(fs::is_regular_file(group / "Button.xt"));
    CHECK(fs::is_regular_file(group / "Toggle.xt"));
    CHECK(readTextT498(group / "Button.xt") == readTextT498(root / "shared" / "symbols" / "Button.xt"));
    CHECK(fs::is_regular_file(group / "fonts" / "NotoSerifDisplay-ExtraCondensedBlack.ttf"));
    CHECK(fs::is_regular_file(group / "textures" / "hatch-ink.png"));
    CHECK(fs::is_regular_file(group / "textures" / "hatch-night.png"));
    CHECK(fs::is_regular_file(group / "thumbnails" / "Button.png"));  // From the source group.
    const std::string groupTokens = readTextT498(group / "tokens.xts");
    CHECK(groupTokens.find("  --color-focus: #00ff00;\n") != std::string::npos);
    CHECK(groupTokens.find("  --color-accent: #1f1c18;\n") != std::string::npos);
    CHECK(groupTokens.find("  --tex-hatch: url(\"textures/hatch-ink.png\");\n") != std::string::npos);
    CHECK(groupTokens.find("@font-face { font-family: \"Noto Serif Display ExtraCondensed\"; "
                           "src: \"fonts/NotoSerifDisplay-ExtraCondensedBlack.ttf\"; font-weight: 900; }") !=
          std::string::npos);
    const std::string groupNight = readTextT498(group / "tokens-night.xts");
    CHECK(groupNight.find("  --tex-hatch: url(\"textures/hatch-night.png\");\n") != std::string::npos);
    CHECK(groupNight.find("  --color-accent: #d9d3c4;\n") != std::string::npos);

    SymbolLibrary library;
    REQUIRE(loadLibrary(mine, library) == Status::kOk);
    REQUIRE(library.groups.size() == 2);
    const SymbolGroup& saved = library.groups[1];
    CHECK(saved.id == "my-controls");
    CHECK(saved.name == "My controls");
    CHECK(saved.source.kind == "manual");
    CHECK(saved.tokensFile == "tokens.xts");
    CHECK(saved.themeFiles == std::vector<std::string>{"tokens-night.xts"});
    CHECK(saved.problems.empty());
    CHECK(saved.lint.errors == 0);  // The index-only lint, recorded in library.json.
    REQUIRE(saved.symbols.size() == 2);
    const SymbolEntry* button = findSymbol(library, "Button", "my-controls");
    REQUIRE(button != nullptr);
    CHECK(button->file == "Button.xt");
    CHECK(button->thumbnail == "thumbnails/Button.png");
    CHECK(button->requiredStates == std::vector<std::string>{"Hover", "Pressed", "Focus", "Disabled"});
    CHECK(button->requiredSlots == std::vector<std::string>{"label"});
    REQUIRE(button->ports.size() == 1);
    CHECK(button->ports.front().name == "pressed");
    CHECK(button->timelines == std::vector<std::string>{"hover-in", "hover-out", "press"});
    CHECK(button->description == findSymbol(mounts, "Button", kPresetGroup)->description);
    CHECK(!button->tokens.empty());
    CHECK(hasT501(button->fonts, "fonts/NotoSerifDisplay-ExtraCondensedBlack.ttf"));
    CHECK(hasT501(button->textures, "textures/hatch-ink.png"));
    CHECK(button->content.rfind("sha256:", 0) == 0);
    LintOptions indexOnly;
    indexOnly.indexOnly = true;
    const LintReport lint = lintGroup(group, indexOnly);
    for (const LintIssue& issue : lint.issues) {
        CHECK_MESSAGE(issue.level == LintLevel::Warning,
                      (issue.symbol + " " + issue.code + ": " + issue.message));
    }
    CHECK(lint.errors() == 0);

    // The new group is usable: another project pulls Button from it.
    LibraryMounts again;
    REQUIRE(mountDefaults(again, kPresetDir, userData) == Status::kOk);
    const UiProject other = blankProjectT498b("xt_t501_save_other");
    const std::string_view one[] = {"Button"};
    const UseReport report = useSymbols(again, other, one, "my-controls");
    CHECK(report.status == Status::kOk);
    CHECK(report.notFound.empty());
    CHECK(readTextT498(compile::pathFromUtf8(other.rootDir) / "shared" / "tokens.xts")
              .find("--color-focus: #00ff00;") != std::string::npos);

    // Refusals.
    CHECK(saveAsGroup(project, names, mine, "my-controls", "x", &error, &mounts) == Status::kInvalidArgument);
    CHECK(error.find("already") != std::string::npos);
    CHECK(saveAsGroup(project, names, mine, "Bad.Id", "x", &error) == Status::kInvalidArgument);
    // An empty `names`: an empty group (the editor's "New group"), even without a project.
    CHECK(saveAsGroup(UiProject(), {}, mine, "empty", "Empty group", &error) == Status::kOk);
    REQUIRE(loadLibrary(mine, library) == Status::kOk);
    REQUIRE(library.groups.size() == 3);
    CHECK(library.groups[2].id == "empty");
    CHECK(library.groups[2].name == "Empty group");
    CHECK(library.groups[2].symbols.empty());
    CHECK(library.groups[2].problems.empty());
    CHECK(deleteGroup(mine, "empty") == Status::kOk);
    const std::string_view missing[] = {"NoSuchSymbol"};
    CHECK(saveAsGroup(project, missing, mine, "missing", "x", &error) == Status::kNotFound);
    CHECK_FALSE(fs::exists(mine / "groups" / "missing"));
    CHECK(saveAsGroup(project, names, kPresetDir, "preset-copy", "x", &error) == Status::kUnsupported);
    CHECK(saveAsGroup(project, names, root / "nowhere", "x-y", "x", &error) == Status::kNotFound);
    // Without mounts, a token the project does not declare has no value: the lint says so.
    writeTextT498(root / "shared" / "symbols" / "Plain.xt",
                  "<xt><head><symbol name=\"Plain\"><div id=\"B\"/></symbol>"
                  "<style>symbol(Plain) { #B { color: var(--color-undeclared-x); } }</style></head></xt>\n");
    const std::string_view plain[] = {"Plain"};
    CHECK(saveAsGroup(project, plain, mine, "plain", "Plain", &error) == Status::kOk);
    REQUIRE(loadLibrary(mine, library) == Status::kOk);
    REQUIRE(library.groups.size() == 3);
    CHECK(library.groups[2].lint.errors > 0);
}
