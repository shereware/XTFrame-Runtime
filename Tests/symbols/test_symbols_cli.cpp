#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "json.hpp"
#include "preview_render.h"
#include "test_temp.h"
#include "xt/bundle/symbol_library.h"
#include "xt/bundle/symbol_lint.h"
#include "xt/bundle/symbol_set.h"
#include "xt/compile/utf8_path.h"
#include "xt/sw/sw_png.h"
#include "xt/util/sha256.h"

/// T-499 (Docs/design/49-symbol-sets.md 49.4 to 49.7): the library behind `xtframe-symbols` --
/// lintGroup()/lintLibrary(), the previews and thumbnails, tokensJson(), exportGroup() and
/// importGroupDir(). The state goldens and the checks that moved into lint stay in test_symbols.cpp.

using namespace xt;

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string readText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void writeText(const fs::path& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    out << text;
}

/// Replaces the first `from` of `file` with `to`; REQUIREs it was there.
void patchFile(const fs::path& file, const std::string& from, const std::string& to) {
    std::string text = readText(file);
    const size_t at = text.find(from);
    REQUIRE_MESSAGE(at != std::string::npos, from);
    text.replace(at, from.size(), to);
    writeText(file, text);
}

/// Replaces every `from` of `file` with `to`.
void patchAll(const fs::path& file, const std::string& from, const std::string& to) {
    std::string text = readText(file);
    REQUIRE_MESSAGE(text.find(from) != std::string::npos, from);
    for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
    writeText(file, text);
}

/// A fresh directory under the test temp root.
fs::path freshDir(const std::string& name) {
    const fs::path dir = xt::test::tempDir() / ("xt_symbols_cli_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

/// A copy of the preset group under `<root>/<name>/broadsheet-2026` (the directory name is the id).
fs::path copiedGroup(const std::string& name) {
    const fs::path dir = freshDir(name) / "broadsheet-2026";
    std::error_code ec;
    fs::copy(XT_SYMBOLS_DIR, dir, fs::copy_options::recursive, ec);
    REQUIRE(ec.value() == 0);
    return dir;
}

/// An empty writable library `mine` under a fresh directory.
fs::path emptyLibrary(const std::string& name) {
    const fs::path dir = freshDir(name);
    bundle::SymbolLibrary library;
    library.id = "mine";
    library.name = "Mine";
    library.dir = compile::genericPathToUtf8(dir);
    REQUIRE(bundle::saveLibraryJson(library) == Status::kOk);
    return dir;
}

bool hasIssue(const bundle::LintReport& report, const std::string& symbol, const std::string& code,
              bundle::LintLevel level = bundle::LintLevel::Error) {
    return std::any_of(report.issues.begin(), report.issues.end(), [&](const bundle::LintIssue& issue) {
        return issue.symbol == symbol && issue.code == code && issue.level == level;
    });
}

std::string issuesText(const bundle::LintReport& report) {
    std::string text;
    for (const bundle::LintIssue& issue : report.issues) {
        text += issue.symbol + ": " + issue.code + ": " + issue.message + "\n";
    }
    return text;
}

/// Every file of the tree under `dir` -> sha256, relative '/' paths.
std::map<std::string, std::string> fileHashes(const fs::path& dir) {
    std::map<std::string, std::string> out;
    for (const fs::directory_entry& e : fs::recursive_directory_iterator(dir)) {
        if (e.is_regular_file()) {
            out[compile::genericPathToUtf8(e.path().lexically_relative(dir))] =
                util::sha256Hex(readText(e.path()));
        }
    }
    return out;
}

bool samePixels(const tools::symbols::Image& a, const std::vector<uint8_t>& b, int width, int height) {
    return a.width == width && a.height == height && a.rgba == b;
}

}  // namespace

TEST_CASE("symbols cli: the preset library lints without a finding") {
    for (const bool indexOnly : {false, true}) {
        bundle::LintOptions options;
        options.indexOnly = indexOnly;
        const std::vector<bundle::LintReport> reports = bundle::lintLibrary(XT_SYMBOLS_LIBRARY_DIR, options);
        REQUIRE(reports.size() == 1);
        CHECK(reports[0].groupId == "broadsheet-2026");
        CHECK(reports[0].symbols == 39);
        CHECK_MESSAGE(reports[0].issues.empty(), issuesText(reports[0]));
    }
    // A group directory is linted as one group.
    const std::vector<bundle::LintReport> group = bundle::lintLibrary(XT_SYMBOLS_DIR);
    REQUIRE(group.size() == 1);
    CHECK(group[0].errors() == 0);
    // Not a directory: one error, no crash.
    const bundle::LintReport missing = bundle::lintGroup(freshDir("missing") / "nothing");
    CHECK(hasIssue(missing, "", "group.notFound"));
    CHECK(missing.errors() == 1);
}

TEST_CASE("symbols cli: a missing thumbnail, a literal colour and a missing token default are reported") {
    {  // The thumbnail.
        const fs::path group = copiedGroup("lint_thumb");
        fs::remove(group / "thumbnails" / "Button.png");
        const bundle::LintReport report = bundle::lintGroup(group);
        CHECK_MESSAGE(hasIssue(report, "Button", "thumbnail.missing"), issuesText(report));
        CHECK(report.errors() == 1);
    }
    {  // A literal colour (the content hash of the index goes stale with it).
        const fs::path group = copiedGroup("lint_color");
        patchFile(group / "Button.xt", "<style>",
                  "<style>\n      symbol(Button) { #Box { color: #ff0000; } }");
        const bundle::LintReport report = bundle::lintGroup(group);
        CHECK_MESSAGE(hasIssue(report, "Button", "style.literalColor"), issuesText(report));
        CHECK(hasIssue(report, "Button", "index.content"));
        // --index-only does not scan the styles.
        bundle::LintOptions options;
        options.indexOnly = true;
        CHECK_FALSE(hasIssue(bundle::lintGroup(group, options), "Button", "style.literalColor"));
    }
    {  // A token the file reads but the token file does not declare.
        const fs::path group = copiedGroup("lint_token");
        patchFile(group / "tokens.xts", "  --color-accent: #1f1c18;\n", "");
        const bundle::LintReport report = bundle::lintGroup(group);
        CHECK_MESSAGE(hasIssue(report, "Button", "token.undeclared"), issuesText(report));
    }
    {  // A changed default in the token file against the index's.
        const fs::path group = copiedGroup("lint_default");
        patchFile(group / "tokens.xts", "--color-accent: #1f1c18;", "--color-accent: #1f1c19;");
        const bundle::LintReport report = bundle::lintGroup(group);
        CHECK_MESSAGE(hasIssue(report, "Button", "token.default"), issuesText(report));
    }
}

TEST_CASE("symbols cli: contract, timelines, listing and previews") {
    {  // A state the contract requires without a rule.
        const fs::path group = copiedGroup("lint_contract");
        patchAll(group / "Button.xt", ":active", ".was-active");
        const bundle::LintReport report = bundle::lintGroup(group);
        CHECK_MESSAGE(hasIssue(report, "Button", "contract.state"), issuesText(report));
    }
    {  // A timeline longer than 500 ms and one with an unconventional name.
        const fs::path group = copiedGroup("lint_timeline");
        patchFile(group / "Button.xt",
                  "0.15s linear { opacity: 1 } }\n        }\n        @timeline hover-out",
                  "0.9s linear { opacity: 1 } }\n        }\n        @timeline hover-out");
        patchFile(group / "Button.xt", "@timeline press {", "@timeline PressDown {");
        const bundle::LintReport report = bundle::lintGroup(group);
        CHECK_MESSAGE(hasIssue(report, "Button", "timeline.duration"), issuesText(report));
        CHECK(hasIssue(report, "Button", "timeline.name", bundle::LintLevel::Warning));
        CHECK(hasIssue(report, "Button", "index.timeline"));  // The index still lists `press`.
    }
    {  // A file the index does not list.
        const fs::path group = copiedGroup("lint_unlisted");
        fs::copy_file(group / "Button.xt", group / "Extra.xt");
        const bundle::LintReport report = bundle::lintGroup(group);
        CHECK_MESSAGE(hasIssue(report, "Extra", "index.unlisted"), issuesText(report));
    }
    {  // Without its preview a symbol is compiled through a synthetic one and stays clean.
        const fs::path group = copiedGroup("lint_synthetic");
        fs::remove(group / "previews" / "Button.preview.xt");
        fs::remove(group / "previews" / "Button.preview.json");
        const bundle::LintReport report = bundle::lintGroup(group);
        CHECK_MESSAGE(report.issues.empty(), issuesText(report));
    }
    {  // A preview that does not instantiate its symbol, and a malformed preview.json.
        const fs::path group = copiedGroup("lint_preview");
        patchFile(group / "previews" / "Toggle.preview.xt", "<use symbol=\"Toggle\"",
                  "<use symbol=\"Button\"");
        writeText(group / "previews" / "Slider.preview.json", "{ \"width\": 0, \"height\": 10 }");
        const bundle::LintReport report = bundle::lintGroup(group);
        CHECK_MESSAGE(hasIssue(report, "Slider", "preview.spec"), issuesText(report));
        CHECK_MESSAGE(
            (hasIssue(report, "Toggle", "preview.noInstance") || hasIssue(report, "Toggle", "compile.error")),
            issuesText(report));
    }
}

TEST_CASE("symbols cli: previews, the event script and the thumbnails") {
    std::vector<compile::Diagnostic> problems;
    const std::vector<bundle::PreviewSpec> specs = bundle::thumbnailSpecs(XT_SYMBOLS_DIR, {}, &problems);
    CHECK(problems.empty());
    REQUIRE(specs.size() == 39);
    CHECK(std::is_sorted(
        specs.begin(), specs.end(),
        [](const bundle::PreviewSpec& a, const bundle::PreviewSpec& b) { return a.name < b.name; }));
    const std::vector<bundle::PreviewSpec> two = bundle::thumbnailSpecs(XT_SYMBOLS_DIR, {"Slider", "Button"});
    REQUIRE(two.size() == 2);
    CHECK(two[0].name == "Button");
    CHECK(two[0].width == 320);
    CHECK(two[0].height == 200);
    CHECK(two[0].xtFile == "previews/Button.preview.xt");
    REQUIRE(two[1].name == "Slider");
    CHECK(bundle::previewEventScript(two[1]) ==
          "port Volume.min 0\nport Volume.max 100\nport Volume.value 35\nupdate 0.6\n");

    // A broken file is reported and left out.
    const fs::path group = copiedGroup("previews_bad");
    writeText(group / "previews" / "Button.preview.json", "{\"width\": 10}");
    std::vector<compile::Diagnostic> bad;
    CHECK(bundle::thumbnailSpecs(group, {"Button"}, &bad).empty());
    CHECK(bad.size() == 1);
    bundle::PreviewSpec spec;
    std::string error;
    CHECK(bundle::readPreviewSpec(group / "previews" / "Button.preview.json", spec, error) ==
          Status::kInvalidArgument);
    CHECK_FALSE(error.empty());
    CHECK(bundle::readPreviewSpec(group / "previews" / "Nope.preview.json", spec, error) ==
          Status::kNotFound);

    // A thumbnail is the normal-state golden of its symbol, pixel for pixel.
    for (const char* name : {"Button", "Modal", "DamageNumber", "Slider"}) {
        CAPTURE(name);
        const std::vector<bundle::PreviewSpec> one = bundle::thumbnailSpecs(XT_SYMBOLS_DIR, {name});
        REQUIRE(one.size() == 1);
        tools::symbols::Image image;
        std::string renderError;
        REQUIRE_MESSAGE(tools::symbols::renderPreview(XT_SYMBOLS_DIR, one[0], image, renderError),
                        renderError);
        std::vector<uint8_t> golden;
        int width = 0;
        int height = 0;
        REQUIRE(sw::readPng(
            (fs::path(XT_SYMBOLS_TEST_DIR) / "expected" / (std::string("sym_") + name + "_normal.png"))
                .string(),
            golden, width, height));
        CHECK(samePixels(image, golden, width, height));
    }

    // The group thumbnail: four cells of 160 x 100 in 320 x 200.
    std::vector<tools::symbols::Image> cells;
    for (const char* name : {"Button", "Toggle", "Slider", "Modal"}) {
        tools::symbols::Image image;
        REQUIRE(tools::symbols::readPng(
            fs::path(XT_SYMBOLS_DIR) / "thumbnails" / (std::string(name) + ".png"), image));
        cells.push_back(std::move(image));
    }
    tools::symbols::Image composite;
    REQUIRE(tools::symbols::composeGroupThumbnail(cells, 320, 200, composite));
    CHECK(composite.width == 320);
    CHECK(composite.height == 200);
    CHECK(composite.rgba.size() == 320u * 200u * 4u);
    tools::symbols::Image none;
    CHECK_FALSE(tools::symbols::composeGroupThumbnail({}, 320, 200, none));
    // The Modal quadrant (bottom right) carries its dark scrim; the Button quadrant (top left) its paper.
    const auto pixel = [&](int x, int y) {
        return composite.rgba[(static_cast<size_t>(y) * 320u + static_cast<size_t>(x)) * 4u];
    };
    CHECK(pixel(165, 105) < 120);
    CHECK(pixel(2, 2) > 200);

    // The preset group's thumbnails are what `xtframe-symbols thumbnails` writes.
    tools::symbols::Image shipped;
    REQUIRE(tools::symbols::readPng(fs::path(XT_SYMBOLS_DIR) / "thumbnails" / "group.png", shipped));
    CHECK(shipped.width == 320);
    CHECK(shipped.height == 200);
    CHECK(shipped.rgba == composite.rgba);
}

TEST_CASE("symbols cli: tokens-json is the skill's tokens.json") {
    std::string text;
    std::string error;
    REQUIRE_MESSAGE(bundle::tokensJson(XT_SYMBOLS_DIR, text, error) == Status::kOk, error);
    const json parsed = json::parse(text);
    CHECK(parsed["themes"].contains("base"));
    CHECK(parsed["themes"].contains("dark"));
    CHECK(parsed["themes"]["base"]["bg"] == "#e6e2d6");
    CHECK(parsed["themes"]["dark"]["bg"] == "#262320");
    CHECK(parsed["font"]["displayFamily"].get<std::string>().find("Noto Serif Display ExtraCondensed") !=
          std::string::npos);
    CHECK(parsed["space"].size() == 8);
    // The skill ships outside the runtime tree: compared when it is there.
    if (fs::exists(XT_SKILL_TOKENS_JSON)) {
        CHECK(text == readText(XT_SKILL_TOKENS_JSON));
    }
    // No token file: not found. No colours: invalid.
    CHECK(bundle::tokensJson(freshDir("tokens_none") / "none", text, error) == Status::kNotFound);
    const fs::path group = copiedGroup("tokens_nocolor");
    writeText(group / "tokens.xts", ":root { --space-1: 4px; }\n");
    CHECK(bundle::tokensJson(group, text, error) == Status::kInvalidArgument);
}

TEST_CASE("symbols cli: export then import round trips a group into a library") {
    const fs::path out = freshDir("export") / "set";
    const bundle::ExportReport exported = bundle::exportGroup(XT_SYMBOLS_DIR, out);
    REQUIRE_MESSAGE(exported.status == Status::kOk, exported.error);
    CHECK(exported.set.author == "xtframe");
    CHECK(exported.set.id == "broadsheet-2026");
    CHECK(exported.set.setVersion == "2.0.0");
    CHECK(exported.set.license == "CC0-1.0");
    CHECK(exported.set.fontsLicense == "OFL-1.1");
    CHECK(exported.set.thumbnail == "thumbnail.png");
    CHECK(fs::exists(out / "set.json"));
    CHECK(fs::exists(out / "thumbnail.png"));
    for (const char* file :
         {"index.json", "group.json", "tokens.xts", "tokens-night.xts", "Button.xt", "mobile/TouchButton.xt",
          "list/ListItem.xt", "fonts/NotoSerif-Bold.ttf", "textures/grain-paper.png", "thumbnails/group.png",
          "previews/Button.preview.json", "previews/night/Modal.preview.xt", "CHANGELOG.md"}) {
        CAPTURE(file);
        CHECK(fs::exists(out / "groups" / "broadsheet-2026" / file));
    }
    bundle::SetInfo info;
    std::string error;
    REQUIRE_MESSAGE(bundle::readSetInfo(out, info, error) == Status::kOk, error);
    CHECK(info.author == "xtframe");
    CHECK(info.groups == std::vector<std::string>{"broadsheet-2026"});
    CHECK(info.requiresXtframe.rfind(">=", 0) == 0);

    const fs::path library = emptyLibrary("export_lib");
    const bundle::ImportReport imported = bundle::importGroupDir(out, library);
    REQUIRE_MESSAGE(imported.status == Status::kOk, imported.error);
    REQUIRE(imported.groups.size() == 1);
    CHECK(imported.groups[0].action == bundle::ImportAction::Added);
    CHECK(imported.groups[0].targetId == "broadsheet-2026");
    CHECK(imported.groups[0].lint.errors() == 0);

    bundle::SymbolLibrary loaded;
    REQUIRE(bundle::loadLibrary(library, loaded) == Status::kOk);
    REQUIRE(loaded.groups.size() == 1);
    CHECK(loaded.groups[0].id == "broadsheet-2026");
    CHECK(loaded.groups[0].symbols.size() == 39);
    CHECK(loaded.groups[0].problems.empty());
    CHECK(loaded.groups[0].source.kind == "import");
    CHECK(loaded.groups[0].source.set == "xtframe/broadsheet-2026");
    CHECK(loaded.groups[0].source.setVersion == "2.0.0");
    CHECK(bundle::findSymbol(loaded, "Modal") != nullptr);
    CHECK(
        json::parse(readText(library / "groups" / "broadsheet-2026" / "group.json")).contains("importedAt"));

    // The files are the same bytes (group.json is rewritten with the source and the import time).
    const std::map<std::string, std::string> before = fileHashes(XT_SYMBOLS_DIR);
    const std::map<std::string, std::string> after = fileHashes(library / "groups" / "broadsheet-2026");
    CHECK(before.size() == after.size());
    for (const auto& [file, hash] : before) {
        CAPTURE(file);
        const auto it = after.find(file);
        REQUIRE(it != after.end());
        if (file != "group.json") {
            CHECK(it->second == hash);
        }
    }
    // The imported group lints like the original.
    const bundle::LintReport lint = bundle::lintGroup(library / "groups" / "broadsheet-2026");
    CHECK_MESSAGE(lint.issues.empty(), issuesText(lint));

    // --no-fonts leaves the fonts out; a non-empty output directory is refused.
    bundle::ExportOptions noFonts;
    noFonts.fonts = false;
    const fs::path out2 = freshDir("export_nofonts") / "set";
    REQUIRE(bundle::exportGroup(XT_SYMBOLS_DIR, out2, noFonts).status == Status::kOk);
    CHECK_FALSE(fs::exists(out2 / "groups" / "broadsheet-2026" / "fonts"));
    CHECK(fs::exists(out2 / "groups" / "broadsheet-2026" / "textures"));
    CHECK(bundle::exportGroup(XT_SYMBOLS_DIR, out2).status == Status::kInvalidArgument);
    CHECK(bundle::exportGroup(freshDir("export_none") / "nothing", freshDir("export_none2") / "o").status ==
          Status::kNotFound);
}

TEST_CASE("symbols cli: export needs an author and a license it can take") {
    const fs::path group = copiedGroup("export_infer");
    // No source set, no LICENSE: the author and the license are asked for.
    writeText(group / "group.json",
              "{ \"version\": 1, \"id\": \"broadsheet-2026\", \"name\": \"X\", \"tokens\": \"tokens.xts\" }");
    fs::remove(group / "LICENSE");
    const fs::path out = freshDir("export_infer_out") / "set";
    bundle::ExportReport report = bundle::exportGroup(group, out);
    CHECK(report.status == Status::kInvalidArgument);
    CHECK(report.error.find("author") != std::string::npos);
    bundle::ExportOptions options;
    options.author = "someone";
    report = bundle::exportGroup(group, out, options);
    CHECK(report.status == Status::kInvalidArgument);
    CHECK(report.error.find("license") != std::string::npos);
    CHECK_FALSE(fs::exists(out / "set.json"));
    options.license = "MIT";
    report = bundle::exportGroup(group, out, options);
    REQUIRE_MESSAGE(report.status == Status::kOk, report.error);
    CHECK(report.set.author == "someone");
    CHECK(report.set.license == "MIT");
    CHECK(report.set.setVersion == "1.0.0");
}

namespace {

/// An exported set of the preset group under a fresh directory.
fs::path exportedSet(const std::string& name) {
    const fs::path out = freshDir(name) / "set";
    const bundle::ExportReport report = bundle::exportGroup(XT_SYMBOLS_DIR, out);
    REQUIRE_MESSAGE(report.status == Status::kOk, report.error);
    return out;
}

size_t groupCount(const fs::path& library) {
    bundle::SymbolLibrary loaded;
    REQUIRE(bundle::loadLibrary(library, loaded) == Status::kOk);
    return loaded.groups.size();
}

}  // namespace

TEST_CASE("symbols cli: import refuses what leaves its group and writes nothing") {
    const fs::path library = emptyLibrary("traversal_lib");
    {  // A symbol file path with `..`.
        const fs::path set = exportedSet("traversal_file");
        patchFile(set / "groups" / "broadsheet-2026" / "index.json", "\"file\": \"Button.xt\"",
                  "\"file\": \"../../evil.xt\"");
        const bundle::ImportReport report = bundle::importGroupDir(set, library);
        CHECK(report.status == Status::kInvalidArgument);
        CHECK(report.error.find("leaves the group directory") != std::string::npos);
    }
    {  // A group id that is a path.
        const fs::path set = exportedSet("traversal_group");
        patchFile(set / "set.json", "\"groups\": [\n    \"broadsheet-2026\"\n  ]",
                  "\"groups\": [\"../broadsheet-2026\"]");
        CHECK(bundle::importGroupDir(set, library).status == Status::kInvalidArgument);
    }
    {  // A thumbnail path outside the set.
        const fs::path set = exportedSet("traversal_thumb");
        patchFile(set / "set.json", "\"thumbnail\": \"thumbnail.png\"",
                  "\"thumbnail\": \"../thumbnail.png\"");
        CHECK(bundle::importGroupDir(set, library).status == Status::kInvalidArgument);
    }
    {  // A font path, in the group.json's token files as well.
        const fs::path set = exportedSet("traversal_group_json");
        patchFile(set / "groups" / "broadsheet-2026" / "group.json", "\"tokens\": \"tokens.xts\"",
                  "\"tokens\": \"../../tokens.xts\"");
        CHECK(bundle::importGroupDir(set, library).status == Status::kInvalidArgument);
    }
    {  // A symbolic link in the set.
        const fs::path set = exportedSet("traversal_link");
        std::error_code ec;
        fs::create_symlink(fs::path(XT_SYMBOLS_DIR) / "Button.xt",
                           set / "groups" / "broadsheet-2026" / "Link.xt", ec);
        if (!ec) {
            const bundle::ImportReport report = bundle::importGroupDir(set, library);
            CHECK(report.status == Status::kInvalidArgument);
            CHECK(report.error.find("symbolic link") != std::string::npos);
        }
    }
    CHECK(groupCount(library) == 0);
    CHECK_FALSE(fs::exists(library / "groups" / "broadsheet-2026"));
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(library, ec)) {
        CHECK_MESSAGE((e.path().filename() == "library.json" || e.path().filename() == "groups"),
                      e.path().filename().string());
    }
}

TEST_CASE("symbols cli: import checks set.json") {
    const fs::path library = emptyLibrary("setjson_lib");
    const auto importWith = [&](const std::string& name, const std::string& from, const std::string& to) {
        const fs::path set = exportedSet(name);
        patchFile(set / "set.json", from, to);
        return bundle::importGroupDir(set, library);
    };
    CHECK(importWith("setjson_version", "\"version\": 1", "\"version\": 2").status == Status::kUnsupported);
    CHECK(importWith("setjson_requires", "\"xtframe\": \">=", "\"xtframe\": \">=99.").status ==
          Status::kUnsupported);
    CHECK(importWith("setjson_requires_form", "\"xtframe\": \">=", "\"xtframe\": \"^").status ==
          Status::kUnsupported);
    CHECK(importWith("setjson_license", "\"license\": \"CC0-1.0\"", "\"license\": \"\"").status ==
          Status::kInvalidArgument);
    CHECK(importWith("setjson_semver", "\"setVersion\": \"2.0.0\"", "\"setVersion\": \"two\"").status ==
          Status::kInvalidArgument);
    CHECK(importWith("setjson_id", "\"id\": \"broadsheet-2026\"", "\"id\": \"Bad Id\"").status ==
          Status::kInvalidArgument);
    CHECK(bundle::importGroupDir(freshDir("setjson_none") / "none", library).status == Status::kNotFound);
    CHECK(bundle::importGroupDir(exportedSet("setjson_nolib"), freshDir("setjson_nolib2") / "nolib").status ==
          Status::kNotFound);
    CHECK(groupCount(library) == 0);
}

TEST_CASE("symbols cli: import id rules (same, newer, older, other author, hand-made, --as)") {
    const fs::path library = emptyLibrary("rules_lib");
    const fs::path set = exportedSet("rules_set");
    const fs::path setJson = set / "set.json";
    const auto sourceVersion = [&](const std::string& group) {
        bundle::SymbolLibrary loaded;
        REQUIRE(bundle::loadLibrary(library, loaded) == Status::kOk);
        for (const bundle::SymbolGroup& g : loaded.groups) {
            if (g.id == group) {
                return g.source.setVersion;
            }
        }
        return std::string("(none)");
    };

    bundle::ImportReport report = bundle::importGroupDir(set, library);
    REQUIRE_MESSAGE(report.status == Status::kOk, report.error);
    CHECK(report.groups[0].action == bundle::ImportAction::Added);
    CHECK(sourceVersion("broadsheet-2026") == "2.0.0");
    const std::string bytes = readText(library / "groups" / "broadsheet-2026" / "Button.xt");

    // The same version again: nothing happens.
    report = bundle::importGroupDir(set, library);
    REQUIRE(report.status == Status::kOk);
    CHECK(report.groups[0].action == bundle::ImportAction::Unchanged);
    CHECK(groupCount(library) == 1);

    // A newer version replaces it; the old group moves to .trash/.
    patchFile(setJson, "\"setVersion\": \"2.0.0\"", "\"setVersion\": \"2.1.0\"");
    report = bundle::importGroupDir(set, library);
    REQUIRE_MESSAGE(report.status == Status::kOk, report.error);
    CHECK(report.groups[0].action == bundle::ImportAction::Updated);
    CHECK(sourceVersion("broadsheet-2026") == "2.1.0");
    CHECK_FALSE(report.groups[0].trashed.empty());
    CHECK(fs::exists(library / report.groups[0].trashed / "Button.xt"));
    CHECK(json::parse(readText(library / report.groups[0].trashed / "group.json"))["source"]["setVersion"] ==
          "2.0.0");
    CHECK(groupCount(library) == 1);
    CHECK(readText(library / "groups" / "broadsheet-2026" / "Button.xt") == bytes);

    // An older version is left alone unless asked.
    patchFile(setJson, "\"setVersion\": \"2.1.0\"", "\"setVersion\": \"1.5.0\"");
    report = bundle::importGroupDir(set, library);
    REQUIRE(report.status == Status::kOk);
    CHECK(report.groups[0].action == bundle::ImportAction::OlderSkipped);
    CHECK(sourceVersion("broadsheet-2026") == "2.1.0");
    bundle::ImportOptions replace;
    replace.replaceOlder = true;
    report = bundle::importGroupDir(set, library, replace);
    REQUIRE(report.status == Status::kOk);
    CHECK(report.groups[0].action == bundle::ImportAction::OlderReplaced);
    CHECK(sourceVersion("broadsheet-2026") == "1.5.0");

    // Another author with the same id: the group becomes broadsheet-2026-2; again, it updates that one.
    patchFile(setJson, "\"author\": \"xtframe\"", "\"author\": \"someone\"");
    patchFile(setJson, "\"setVersion\": \"1.5.0\"", "\"setVersion\": \"3.0.0\"");
    report = bundle::importGroupDir(set, library);
    REQUIRE_MESSAGE(report.status == Status::kOk, report.error);
    CHECK(report.groups[0].action == bundle::ImportAction::Added);
    CHECK(report.groups[0].targetId == "broadsheet-2026-2");
    CHECK(groupCount(library) == 2);
    CHECK(sourceVersion("broadsheet-2026-2") == "3.0.0");
    CHECK(sourceVersion("broadsheet-2026") == "1.5.0");
    patchFile(setJson, "\"setVersion\": \"3.0.0\"", "\"setVersion\": \"3.1.0\"");
    report = bundle::importGroupDir(set, library);
    REQUIRE(report.status == Status::kOk);
    CHECK(report.groups[0].action == bundle::ImportAction::Updated);
    CHECK(report.groups[0].targetId == "broadsheet-2026-2");
    CHECK(groupCount(library) == 2);

    // A third author takes the next free suffix.
    patchFile(setJson, "\"author\": \"someone\"", "\"author\": \"third\"");
    report = bundle::importGroupDir(set, library);
    REQUIRE(report.status == Status::kOk);
    CHECK(report.groups[0].targetId == "broadsheet-2026-3");

    // --as names the group; a name another set holds is refused.
    bundle::ImportOptions as;
    as.asGroup = "my-style";
    report = bundle::importGroupDir(set, library, as);
    REQUIRE_MESSAGE(report.status == Status::kOk, report.error);
    CHECK(report.groups[0].targetId == "my-style");
    CHECK(report.groups[0].sourceId == "broadsheet-2026");
    CHECK(sourceVersion("my-style") == "3.1.0");
    as.asGroup = "broadsheet-2026";
    CHECK(bundle::importGroupDir(set, library, as).status == Status::kInvalidArgument);
    as.asGroup = "Bad Name";
    CHECK(bundle::importGroupDir(set, library, as).status == Status::kInvalidArgument);

    // library.json lists them all and every group loads without problems.
    bundle::SymbolLibrary loaded;
    REQUIRE(bundle::loadLibrary(library, loaded) == Status::kOk);
    CHECK(loaded.groups.size() == 4);
    for (const bundle::SymbolGroup& g : loaded.groups) {
        CAPTURE(g.id);
        CHECK(g.problems.empty());
        CHECK(g.symbols.size() == 39);
    }
}

TEST_CASE("symbols cli: a hand-made group of the same id is another set's") {
    const fs::path library = emptyLibrary("handmade_lib");
    std::error_code ec;
    fs::create_directories(library / "groups", ec);
    fs::copy(XT_SYMBOLS_DIR, library / "groups" / "broadsheet-2026", fs::copy_options::recursive, ec);
    REQUIRE(ec.value() == 0);
    writeText(library / "groups" / "broadsheet-2026" / "group.json",
              "{ \"version\": 1, \"id\": \"broadsheet-2026\", \"name\": \"Mine\", \"tokens\": \"tokens.xts\","
              " \"themes\": [\"tokens-night.xts\"], \"thumbnail\": \"thumbnails/group.png\" }");
    bundle::SymbolLibrary loaded;
    REQUIRE(bundle::loadLibrary(library, loaded) == Status::kOk);
    bundle::SymbolGroup group;
    group.id = "broadsheet-2026";
    loaded.groups.push_back(group);
    REQUIRE(bundle::saveLibraryJson(loaded) == Status::kOk);

    const bundle::ImportReport report = bundle::importGroupDir(exportedSet("handmade_set"), library);
    REQUIRE_MESSAGE(report.status == Status::kOk, report.error);
    CHECK(report.groups[0].action == bundle::ImportAction::Added);
    CHECK(report.groups[0].targetId == "broadsheet-2026-2");
    CHECK(groupCount(library) == 2);
    CHECK(json::parse(readText(library / "groups" / "broadsheet-2026" / "group.json"))["name"] ==
          "Mine");  // Untouched.
}

TEST_CASE("symbols cli: import into a read-only library is refused; lint errors do not stop an import") {
    const fs::path set = exportedSet("readonly_set");
    const fs::path library = freshDir("readonly_lib");
    writeText(library / "library.json",
              "{ \"version\": 1, \"id\": \"ro\", \"readOnly\": true, \"groups\": [] }");
    CHECK(bundle::importGroupDir(set, library).status == Status::kUnsupported);

    // A set whose group fails lint (a thumbnail is missing) imports, with the finding in the report.
    const fs::path broken = exportedSet("lint_set");
    fs::remove(broken / "groups" / "broadsheet-2026" / "thumbnails" / "Button.png");
    const fs::path target = emptyLibrary("lint_target");
    const bundle::ImportReport report = bundle::importGroupDir(broken, target);
    REQUIRE_MESSAGE(report.status == Status::kOk, report.error);
    REQUIRE(report.groups.size() == 1);
    CHECK(hasIssue(report.groups[0].lint, "Button", "thumbnail.missing"));
    CHECK(groupCount(target) == 1);
}
