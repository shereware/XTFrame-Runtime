#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <json.hpp>
#include <string>
#include <vector>

#include "design_internal.h"
#include "doctest.h"
#include "test_temp.h"
#include "xt/bundle/design_export.h"
#include "xt/bundle/design_system.h"
#include "xt/bundle/ui_project.h"

/// The Claude Design design-system exchange: export, import, re-import and untrusted input
/// (Docs/design/35-design-system.md §35.4, §35.7.1; Docs/tasks/T-316.md item 5).

using namespace xt;
using namespace xt::bundle;
using namespace xt::bundle::design;
using nlohmann::json;

namespace {

namespace fs = std::filesystem;

std::string sourceDir(const char* relative) { return std::string(XT_TEST_BUNDLE_DIR) + "/../../" + relative; }

std::string freshDir(const std::string& name) {
    const fs::path dir = xt::test::tempDir() / ("xt_designexport_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir.generic_string();
}

std::string readText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeText(const std::string& path, const std::string& text) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

/// A writable copy of the starter UI Project with its standard symbols pulled in. T-498b/T-498c:
/// the shipped starter declares only its overrides (`--safe-*`); the preset group's tokens.xts is
/// the implied default layer underneath, which the design tools show as the inherited variables
/// (49 §49.12 ruling).
UiProject starterCopy(const std::string& name) {
    const std::string dir = freshDir(name);
    std::error_code ec;
    fs::copy(sourceDir("Examples/ui-project-starter"), dir, fs::copy_options::recursive, ec);
    REQUIRE(ec.value() == 0);
    std::string error;
    Result<UiProject> loaded = UiProject::load(dir, &error);
    REQUIRE_MESSAGE(loaded.ok(), error);
    REQUIRE(pullStandardSymbols(loaded.value(), sourceDir("Assets/Symbols/groups/broadsheet-2026")) ==
            Status::kOk);
    return loaded.value();
}

/// A blank UI Project (no template): an empty `shared/` and the upgraded `design/`.
UiProject blankProject(const std::string& name) {
    const std::string dir = freshDir(name);
    UiProject project;
    std::string error;
    REQUIRE_MESSAGE(createUiProject(dir, "Blank", "blank", "", &project, &error) == Status::kOk, error);
    return project;
}

DesignSystem loadDs(const UiProject& project) {
    DesignSystem ds;
    std::string error;
    REQUIRE_MESSAGE(load(project, ds, nullptr, &error) == Status::kOk, error);
    return ds;
}

/// Exports `project` into `outDir`.
ExportReport exportTo(const UiProject& project, const std::string& outDir) {
    DesignSystem ds = loadDs(project);
    Graph graph;
    std::string error;
    REQUIRE_MESSAGE(buildGraph(project, ds, graph, &error) == Status::kOk, error);
    ExportReport report;
    REQUIRE_MESSAGE(exportClaudeDesign(project, ds, graph, outDir, &report,
                                       sourceDir("Assets/Symbols/groups/broadsheet-2026")) == Status::kOk,
                    report.error);
    return report;
}

/// The starter exported once (into a fresh directory).
std::string starterExport(const std::string& name, ExportReport* report = nullptr) {
    const UiProject starter = starterCopy(name + "_src");
    const std::string out = freshDir(name + "_out");
    const ExportReport r = exportTo(starter, out);
    if (report != nullptr) {
        *report = r;
    }
    return out;
}

const TextEntry* textByRef(const DesignSystem& ds, const std::string& ref) {
    for (const TextEntry& t : ds.texts) {
        if (t.base.source.kind == kClaudeDesignSourceKind && t.base.source.ref == ref) {
            return &t;
        }
    }
    return nullptr;
}

int importedCount(const std::vector<TextEntry>& texts) {
    return static_cast<int>(std::count_if(texts.begin(), texts.end(), [](const TextEntry& t) {
        return t.base.source.kind == kClaudeDesignSourceKind;
    }));
}

}  // namespace

TEST_CASE("T-316 export: the four files, the tokens.json shape, a README in the note subset") {
    ExportReport report;
    const std::string out = starterExport("export", &report);
    for (const char* file : {"README.md", "tokens.json", "palette.json", "components.md"}) {
        INFO(file);
        CHECK(fs::is_regular_file(fs::path(out) / file));
    }
    CHECK(report.files ==
          std::vector<std::string>{"README.md", "tokens.json", "palette.json", "components.md"});

    // tokens.json keeps the shape of Skills/skills/xtui/references/tokens.json.
    // The runtime export (Tools/Scripts/export_runtime.sh, T-400) leaves Skills/ out: no reference there.
    const json tokens = json::parse(readText(out + "/tokens.json"));
    const std::string referencePath = sourceDir("Skills/skills/xtui/references/tokens.json");
    if (fs::is_regular_file(referencePath)) {
        const json reference = json::parse(readText(referencePath));
        for (auto it = reference.begin(); it != reference.end(); ++it) {
            INFO(it.key());
            REQUIRE(tokens.contains(it.key()));
            CHECK(tokens[it.key()].type() == it.value().type());
        }
    }
    CHECK(tokens["name"] == "Starter");
    CHECK(tokens["version"] == 1);
    std::vector<std::string> themes;
    for (auto it = tokens["themes"].begin(); it != tokens["themes"].end(); ++it) {
        themes.push_back(it.key());
    }
    CHECK(themes == std::vector<std::string>{"base", "dark"});
    CHECK(report.themes == themes);
    // T-498c (49 §49.12 ruling): the base theme carries the inherited colour variables too (the
    // slimmed starter declares none of its own), named in the `inherited` object.
    CHECK(tokens["themes"]["base"].size() == 22);
    CHECK(report.colorTokens == 22);
    REQUIRE(tokens["inherited"].is_object());
    CHECK(tokens["inherited"]["--color-accent"] == true);
    CHECK(tokens["inherited"]["--font-family"] == true);
    CHECK(tokens["inherited"]["--space-1"] == true);
    CHECK(tokens["inherited"]["--radius-full"] == true);
    CHECK_FALSE(tokens["inherited"].contains("--safe-x"));
    CHECK(report.inheritedTokens == static_cast<int>(tokens["inherited"].size()));
    CHECK(report.inheritedTokens > 22);
    CHECK(tokens["themes"]["base"]["accent"] == "#1f1c18");
    CHECK(tokens["themes"]["base"]["textOnAccent"] == "#e6e2d6");
    CHECK(tokens["themes"]["base"]["surfaceRaised"] == "#ece8dd");
    CHECK(tokens["themes"]["dark"]["accent"] == "#d9d3c4");
    CHECK(tokens["themes"]["dark"].size() == 22);
    CHECK(contains(tokens["font"]["family"].get<std::string>(), "Noto Serif"));
    CHECK(tokens["font"]["m"]["size"] == 16);
    CHECK(tokens["font"]["xl"]["size"] == 28);
    REQUIRE(tokens["space"].is_array());
    REQUIRE(tokens["space"].size() >= 2);
    for (size_t i = 1; i < tokens["space"].size(); ++i) {
        CHECK(tokens["space"][i - 1].get<double>() < tokens["space"][i].get<double>());
    }
    CHECK(tokens["space"] == json::array({4, 8, 12, 16, 24, 32}));  // --safe-x / --safe-y are space too
    CHECK(tokens["radius"]["s"] == 0);
    CHECK(tokens["radius"]["full"] == 999);
    CHECK(tokens["board"] == json({{"width", 1280}, {"height", 720}}));

    // README.md: the exported sections only, `## <Title>`, no status marks, no UIs column, no
    // images, no HTML, headings 1-3 only (the note subset of 35 §35.16).
    const std::string readme = readText(out + "/README.md");
    CHECK(readme.rfind("# Starter design system\n", 0) == 0);
    CHECK(contains(readme, "Built with XTFrame; colours and sizes below are the project's tokens"));
    CHECK_FALSE(contains(readme, "(draft)"));
    CHECK_FALSE(contains(readme, "<!--"));
    CHECK_FALSE(contains(readme, "!["));
    CHECK_FALSE(contains(readme, "| UIs |"));
    CHECK_FALSE(contains(readme, "Nothing recorded yet."));
    CHECK_FALSE(contains(readme, "\xC2\xB7 "));
    CHECK_FALSE(contains(readme, "\r"));
    for (const char* heading : {"## Identity\n", "## Voice\n", "## Color\n", "## Type\n",
                                "## Space and shape\n", "## Motion\n", "## Components\n"}) {
        INFO(heading);
        CHECK(contains(readme, heading));
    }
    for (const char* heading :
         {"## Layout", "## Imagery", "## Screens", "## Changelog", "## Assets", "## Decisions"}) {
        INFO(heading);
        CHECK_FALSE(contains(readme, heading));
    }
    CHECK(report.sections == std::vector<std::string>{"Identity", "Voice", "Color", "Type", "Space and shape",
                                                      "Motion", "Components"});
    size_t pos = 0;
    while (pos < readme.size()) {
        const size_t nl = readme.find('\n', pos);
        const std::string line = readme.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        CHECK_MESSAGE(line.rfind("####", 0) != 0, line);
        pos = nl == std::string::npos ? readme.size() : nl + 1;
    }
    CHECK(contains(readme, "### Inherited from broadsheet-2026"));
    CHECK(contains(readme, "| `--color-accent` | `#1f1c18` | `#d9d3c4` | `#d9d3c4` | `swatch:ink` |"));
    CHECK(contains(readme, "| `--safe-x` | space | `32px` | Horizontal safe margin at 1280 x 720. |"));

    // palette.json and components.md.
    const json palette = json::parse(readText(out + "/palette.json"));
    REQUIRE(palette["groups"].size() == 3);
    CHECK(palette["groups"][0]["id"] == "paper");
    CHECK(palette["groups"][0]["swatches"][0] ==
          json({{"id", "paper"},
                {"name", "Paper"},
                {"value", "#e6e2d6"},
                {"role", "background"},
                {"note", "Screen background, grey-white newsprint."}}));
    CHECK(report.swatches == 13);
    const std::string components = readText(out + "/components.md");
    CHECK(contains(components, "### Button"));
    CHECK(contains(components, "- Slots: label"));
    CHECK(contains(components, "- Ports: pressed (event, out)"));
    CHECK(report.symbols > 0);

    // Deterministic: a second export gives the same bytes.
    const std::string again = starterExport("export2");
    for (const char* file : {"README.md", "tokens.json", "palette.json", "components.md"}) {
        INFO(file);
        CHECK(readText(again + "/" + file) == readText(out + "/" + file));
    }
}

TEST_CASE("T-316 round trip: export the starter, import into a blank project, import again") {
    ExportReport exported;
    const std::string out = starterExport("roundtrip", &exported);
    const UiProject blank = blankProject("roundtrip_blank");
    const DesignSystem before = loadDs(blank);
    REQUIRE(before.swatches.empty());

    ImportReport r;
    REQUIRE_MESSAGE(importClaudeDesign(blank, out, &r) == Status::kOk, r.error);
    CHECK(r.files == std::vector<std::string>{"README.md", "tokens.json", "palette.json", "components.md"});
    CHECK(r.textsAdded == static_cast<int>(exported.sections.size()) + 1);
    CHECK(r.textsUpdated == 0);
    // T-498c: the 22 base colours are inherited from the preset group (the `inherited` object), so
    // they get neither metadata nor a value here; the dark theme's overrides are the project's own.
    CHECK(r.tokensAdded == 0);
    CHECK(r.swatchesAdded == 13);
    CHECK(r.groupsAdded == 3);
    CHECK(r.valuesWritten == 0);
    CHECK(r.themeValuesWritten == 22);
    CHECK(r.skipped == 0);
    CHECK(r.added() == r.textsAdded + 13 + 3);
    CHECK(std::any_of(r.notes.begin(), r.notes.end(), [](const std::string& n) {
        return contains(n, "22 inherited variable(s) not imported");
    }));

    const DesignSystem after = loadDs(blank);
    CHECK(after.texts.size() == before.texts.size() + exported.sections.size() + 1);
    CHECK(importedCount(after.texts) == static_cast<int>(exported.sections.size()) + 1);
    CHECK(after.tokens.size() == before.tokens.size());
    CHECK(after.swatches.size() == 13);
    CHECK(after.groups.size() == 3);
    for (const TextEntry& t : after.texts) {
        CHECK(t.base.status == EntryStatus::Draft);
    }
    for (const Swatch& s : after.swatches) {
        CHECK(s.base.status == EntryStatus::Draft);
        CHECK(s.base.source.kind == kClaudeDesignSourceKind);
    }
    const TextEntry* color = textByRef(after, "README.md#color");
    REQUIRE(color != nullptr);
    CHECK(color->path == "text/imported/color.md");
    CHECK(color->sections == std::vector<std::string>{"color"});
    CHECK(color->body.rfind("# Color\n\n", 0) == 0);
    const TextEntry* space = textByRef(after, "README.md#space-and-shape");
    REQUIRE(space != nullptr);
    CHECK(space->sections == std::vector<std::string>{"space"});
    const TextEntry* components = textByRef(after, "components.md");
    REQUIRE(components != nullptr);
    CHECK(components->sections == std::vector<std::string>{"components"});
    // Drafts are not written as `status:`; nothing imported carries `approved`.
    const std::string colorFile = readText(blank.rootDir + "/design/text/imported/color.md");
    CHECK(contains(colorFile, "kind: import:claude-design"));
    CHECK(contains(colorFile, "ref: README.md#color"));
    CHECK_FALSE(contains(colorFile, "approved"));
    CHECK_FALSE(contains(readText(blank.rootDir + "/design/palette.yaml"), "approved"));
    CHECK_FALSE(contains(readText(blank.rootDir + "/design/tokens.yaml"), "approved"));

    const std::string sheet = readText(blank.rootDir + "/shared/tokens.xts");
    CHECK_FALSE(contains(sheet, "--color-accent"));
    const std::string dark = readText(blank.rootDir + "/shared/themes/dark.xts");
    CHECK(contains(dark, "--color-accent: #d9d3c4;"));
    CHECK(xt::bundle::design::detail::sharedTokenNames(blank).empty());

    // Again: nothing new, nothing changed, still drafts, the stylesheets untouched.
    ImportReport again;
    REQUIRE_MESSAGE(importClaudeDesign(blank, out, &again) == Status::kOk, again.error);
    CHECK(again.added() == 0);
    CHECK(again.updated() == 0);
    CHECK(again.valuesWritten == 0);
    CHECK(again.themeValuesWritten == 0);
    CHECK(readText(blank.rootDir + "/shared/tokens.xts") == sheet);
    CHECK(readText(blank.rootDir + "/shared/themes/dark.xts") == dark);
    const DesignSystem twice = loadDs(blank);
    CHECK(twice.texts.size() == after.texts.size());
    CHECK(twice.tokens.size() == after.tokens.size());
    CHECK(twice.swatches.size() == after.swatches.size());

    // An approved entry goes back to draft when the same ref is imported again.
    DesignSystem approved = twice;
    for (TextEntry& t : approved.texts) {
        if (t.base.source.ref == "README.md#color") {
            t.base.status = EntryStatus::Approved;
            t.base.approved = Approval{"Person", "2026-10-12"};
            t.body += "\nEdited by hand.\n";
        }
    }
    for (Swatch& s : approved.swatches) {
        if (s.base.id == "ink") {
            s.base.status = EntryStatus::Approved;
        }
    }
    REQUIRE(save(approved) == Status::kOk);
    ImportReport third;
    REQUIRE_MESSAGE(importClaudeDesign(blank, out, &third) == Status::kOk, third.error);
    CHECK(third.added() == 0);
    CHECK(third.textsUpdated == 1);
    CHECK(third.swatchesUpdated == 1);
    const DesignSystem reverted = loadDs(blank);
    const TextEntry* back = textByRef(reverted, "README.md#color");
    REQUIRE(back != nullptr);
    CHECK(back->base.status == EntryStatus::Draft);
    CHECK(back->base.approved.by.empty());
    CHECK_FALSE(contains(back->body, "Edited by hand."));
    for (const Swatch& s : reverted.swatches) {
        CHECK(s.base.status == EntryStatus::Draft);
    }
}

TEST_CASE("T-316 import never overwrites existing values or entries it did not write") {
    const std::string out = starterExport("keep");
    const UiProject starter = starterCopy("keep_target");
    const std::string sheetBefore = readText(starter.rootDir + "/shared/tokens.xts");
    const std::string darkBefore = readText(starter.rootDir + "/shared/themes/dark.xts");
    const std::string paletteBefore = readText(starter.rootDir + "/design/palette.yaml");
    const std::string tokensBefore = readText(starter.rootDir + "/design/tokens.yaml");
    ImportReport r;
    REQUIRE_MESSAGE(importClaudeDesign(starter, out, &r) == Status::kOk, r.error);
    CHECK(r.tokensAdded == 0);
    CHECK(r.swatchesAdded == 0);
    CHECK(r.groupsAdded == 0);
    CHECK(r.valuesWritten == 0);
    CHECK(r.themeValuesWritten == 0);
    CHECK(r.skipped == 13);  // T-498c: the 22 inherited colours are left out before any count.
    CHECK(r.textsAdded > 0);
    CHECK(readText(starter.rootDir + "/shared/tokens.xts") == sheetBefore);
    CHECK(readText(starter.rootDir + "/shared/themes/dark.xts") == darkBefore);
    CHECK(readText(starter.rootDir + "/design/palette.yaml") == paletteBefore);
    CHECK(readText(starter.rootDir + "/design/tokens.yaml") == tokensBefore);
    // The starter's own texts keep their files; README's Voice lands next to them.
    CHECK(fs::is_regular_file(fs::path(starter.rootDir) / "design/text/voice.md"));
    CHECK(fs::is_regular_file(fs::path(starter.rootDir) / "design/text/imported/voice.md"));

    // A theme file that exists only gains what it lacks. `accent` is not marked inherited in this
    // file, but the starter's tokens.yaml has the `inherited:` entry (T-498c): no value is written.
    writeText(out + "/tokens.json",
              R"({"themes": {"base": {"accent": "#000000", "brandNew": "#123456"},
                             "dark": {"accent": "#ffffff", "brandNew": "#654321"}}})");
    ImportReport more;
    REQUIRE_MESSAGE(importClaudeDesign(starter, out, &more) == Status::kOk, more.error);
    CHECK(more.tokensAdded == 1);
    CHECK(more.skipped == 1 + 13);  // `accent` and the 13 swatches read again
    CHECK(more.valuesWritten == 1);
    CHECK(more.themeValuesWritten == 1);
    const std::string sheet = readText(starter.rootDir + "/shared/tokens.xts");
    CHECK_FALSE(contains(sheet, "--color-accent"));
    CHECK(contains(sheet, "--color-brand-new: #123456;"));
    const std::string dark = readText(starter.rootDir + "/shared/themes/dark.xts");
    CHECK(contains(dark, "--color-accent: #d9d3c4;"));
    CHECK(contains(dark, "--color-brand-new: #654321;"));
    const std::vector<std::string> names = xt::bundle::design::detail::sharedTokenNames(starter);
    CHECK(std::find(names.begin(), names.end(), "--color-brand-new") != names.end());
    CHECK(std::find(names.begin(), names.end(), "--safe-y") != names.end());
}

TEST_CASE("T-316 import: README sections, untrusted names and values, malformed files") {
    const UiProject blank = blankProject("untrusted");
    const std::string dir = freshDir("untrusted_in");
    ImportReport r;
    CHECK(importClaudeDesign(blank, dir, &r) == Status::kNotFound);  // no README.md
    CHECK(importClaudeDesign(blank, dir + "/nope", &r) == Status::kNotFound);

    writeText(
        dir + "/README.md",
        "# Some game\n\nIntro text.\n\n## 2. Tone\n\nShort.\n\n### Numbers\n\nDigits.\n\n## Lore\n\nOld "
        "kingdoms.\n\n```\n## not a heading\n```\n\n## Typography\n\nRajdhani.\n\n## Lore\n\nMore.\n\n## "
        "Empty\n\n");
    writeText(dir + "/tokens.json", R"({"themes": {"base": {"good": "#112233", "bad": "red; } body { x: 1",
              "../evil": "#000000", "comment": "/* x */"}, "../up": {"a": "#000000"}, "dark": "nope"}})");
    REQUIRE_MESSAGE(importClaudeDesign(blank, dir, &r) == Status::kOk, r.error);
    CHECK(r.textsAdded == 4);
    CHECK(r.tokensAdded == 1);
    CHECK(r.valuesWritten == 1);
    CHECK(r.themeValuesWritten == 0);
    CHECK(r.notes.size() >= 5);
    const DesignSystem ds = loadDs(blank);
    const TextEntry* tone = textByRef(ds, "README.md#tone");
    REQUIRE(tone != nullptr);
    CHECK(tone->sections == std::vector<std::string>{"voice"});
    CHECK(contains(tone->body, "### Numbers"));
    const TextEntry* lore = textByRef(ds, "README.md#lore");
    REQUIRE(lore != nullptr);
    CHECK(lore->sections == std::vector<std::string>{"identity"});
    CHECK(lore->path == "text/imported/lore.md");
    CHECK(contains(lore->body, "## not a heading"));
    const TextEntry* lore2 = textByRef(ds, "README.md#lore-1");
    REQUIRE(lore2 != nullptr);
    CHECK(lore2->path == "text/imported/lore-1.md");
    const TextEntry* type = textByRef(ds, "README.md#typography");
    REQUIRE(type != nullptr);
    CHECK(type->sections == std::vector<std::string>{"type"});
    CHECK(textByRef(ds, "README.md#empty") == nullptr);
    const std::string sheet = readText(blank.rootDir + "/shared/tokens.xts");
    CHECK(sheet == ":root {\n  --color-good: #112233;\n}\n");
    CHECK_FALSE(fs::exists(fs::path(blank.rootDir) / "shared/up.xts"));

    // Malformed JSON: refused before anything is written.
    const std::string bad = freshDir("untrusted_bad");
    writeText(bad + "/README.md", "## Color\n\nTeal.\n");
    writeText(bad + "/palette.json", "{\"groups\": [");
    ImportReport refused;
    CHECK(importClaudeDesign(blank, bad, &refused) == Status::kInvalidArgument);
    CHECK(contains(refused.error, "palette.json"));
    CHECK(textByRef(loadDs(blank), "README.md#color") == nullptr);

    // A palette with a bad id or value skips that swatch only.
    writeText(bad + "/palette.json",
              R"({"groups": [{"id": "fx", "name": "Effects", "swatches": [{"id": "glow", "value": "#FFAA00"},
              {"id": "../x", "value": "#000000"}, {"id": "dim", "value": "dark"}]}], "swatches": [{"id": "ink",
              "value": "#111"}]})");
    ImportReport palette;
    REQUIRE_MESSAGE(importClaudeDesign(blank, bad, &palette) == Status::kOk, palette.error);
    CHECK(palette.groupsAdded == 1);
    CHECK(palette.swatchesAdded == 2);
    const DesignSystem withPalette = loadDs(blank);
    REQUIRE(withPalette.swatches.size() == 2);
    for (const Swatch& s : withPalette.swatches) {
        if (s.base.id == "glow") {
            CHECK(s.value == "#ffaa00");
            CHECK(s.group == "fx");
        } else {
            CHECK(s.base.id == "ink");
            CHECK(s.value == "#111111");
            CHECK(s.group.empty());
        }
    }

    // A stylesheet whose :root block has no final semicolon keeps parsing after the insertion.
    writeText(blank.rootDir + "/shared/tokens.xts", "/* :root in a comment { */\n:root {\n  --a: 1px\n}\n");
    writeText(bad + "/tokens.json", R"({"themes": {"base": {"teal": "#0f7b8a"}}})");
    ImportReport insert;
    REQUIRE_MESSAGE(importClaudeDesign(blank, bad, &insert) == Status::kOk, insert.error);
    CHECK(readText(blank.rootDir + "/shared/tokens.xts") ==
          "/* :root in a comment { */\n:root {\n  --a: 1px;\n  --color-teal: #0f7b8a;\n}\n");
    const std::vector<std::string> names = xt::bundle::design::detail::sharedTokenNames(blank);
    CHECK(names == std::vector<std::string>{"--a", "--color-teal"});

    // ImportOptions::writeBaseValues = false lists the values instead of writing them.
    writeText(bad + "/tokens.json", R"({"themes": {"base": {"sky": "#87ceeb"}}})");
    ImportOptions options;
    options.writeBaseValues = false;
    ImportReport listed;
    REQUIRE_MESSAGE(importClaudeDesign(blank, bad, &listed, options) == Status::kOk, listed.error);
    REQUIRE(listed.baseValues.size() == 1);
    CHECK(listed.baseValues[0] == std::pair<std::string, std::string>{"--color-sky", "#87ceeb"});
    CHECK_FALSE(contains(readText(blank.rootDir + "/shared/tokens.xts"), "--color-sky"));
}

TEST_CASE("T-316 import needs a design directory; export refuses an empty output path") {
    const std::string dir = freshDir("nodesign");
    writeText(dir + "/x.xtui", R"({"version": 1, "name": "X", "id": "x", "uis": []})");
    std::string error;
    Result<UiProject> loaded = UiProject::load(dir, &error);
    REQUIRE_MESSAGE(loaded.ok(), error);
    const std::string in = freshDir("nodesign_in");
    writeText(in + "/README.md", "## Color\n\nTeal.\n");
    ImportReport r;
    CHECK(importClaudeDesign(loaded.value(), in, &r) == Status::kNotFound);
    CHECK(contains(r.error, "design directory"));

    const UiProject starter = starterCopy("emptyout");
    DesignSystem ds = loadDs(starter);
    Graph graph;
    REQUIRE(buildGraph(starter, ds, graph, &error) == Status::kOk);
    ExportReport report;
    CHECK(exportClaudeDesign(starter, ds, graph, "", &report) == Status::kInvalidArgument);
}
