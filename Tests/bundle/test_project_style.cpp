#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "compile_check.h"
#include "doctest.h"
#include "test_temp.h"
#include "xt/bundle/project_style.h"
#include "xt/bundle/symbol_library.h"
#include "xt/bundle/symbol_lint.h"
#include "xt/bundle/ui_project.h"
#include "xt/compile/compile.h"
#include "xt/compile/utf8_path.h"
#include "xt/document.h"
#include "xt/ir.h"

/// T-743 (Docs/design/56-symbol-identity-style-scope.md 56.2; 47 "共同詞彙清單"): apply a group's
/// style to a project (what it overwrites, adds and leaves uncovered; the record; themes; fonts),
/// duplicate and detach a symbol (it keeps its look across a style change), the implied layer of a
/// project with several groups (47 rule 5), and the token lint rules. Both groups are built here:
/// the repository has one preset library only.

using namespace xt;
using namespace xt::bundle;
namespace fs = std::filesystem;

namespace {

const fs::path kPresetT743 = fs::path(XT_TEST_BUNDLE_DIR) / ".." / ".." / "Assets" / "Symbols";

void writeT743(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

std::string readT743(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

fs::path freshT743(const std::string& name) {
    const fs::path dir = xt::test::tempDir() / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

UiProject projectT743(const std::string& name) {
    const fs::path dir = freshT743(name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    UiProject project;
    std::string error;
    REQUIRE_MESSAGE(
        createUiProject(compile::genericPathToUtf8(dir), "T743", "t743", "", &project, &error) == Status::kOk,
        error);
    REQUIRE_FALSE(project.shared.tokens.empty());
    return project;
}

/// The value group `seed` gives core token number `i`.
std::string coreValueT743(const CoreToken& token, int seed, size_t i) {
    char buffer[32];
    switch (token.type) {
        case CoreTokenType::Color:
            std::snprintf(buffer, sizeof buffer, "#%02x%02x%02x", (seed * 60) & 0xff, static_cast<int>(i) * 4,
                          seed);
            return buffer;
        case CoreTokenType::Length:
            return std::to_string(static_cast<int>(i) + seed) + "px";
        case CoreTokenType::Time:
            return std::to_string(100 + seed * 10 + static_cast<int>(i)) + "ms";
        case CoreTokenType::Family:
            return std::string("\"") + (seed == 1 ? "AlphaDisplay" : "BetaSerif") + "\", serif";
    }
    return "0";
}

/// `:root { every core token of group `seed` }` plus `extra` declarations.
std::string coreSheetT743(int seed, const std::string& extra, const std::string& faces = std::string()) {
    std::string text = faces + ":root {\n";
    size_t i = 0;
    for (const CoreToken& token : coreTokens()) {
        text += std::string("  ") + token.name + ": " + coreValueT743(token, seed, i++) + ";\n";
    }
    return text + extra + "}\n";
}

std::string valueOfT743(int seed, std::string_view name) {
    size_t i = 0;
    for (const CoreToken& token : coreTokens()) {
        if (name == token.name) {
            return coreValueT743(token, seed, i);
        }
        ++i;
    }
    return std::string();
}

const std::string kButtonT743 =
    "<xt><head><symbol name=\"Button\"><div id=\"Bg\" style=\"width: var(--control-m); height: 10px; "
    "background-color: var(--color-bg)\"></div></symbol></head></xt>\n";
const std::string kGrainyT743 =
    "<xt><head><symbol name=\"Grainy\"><div id=\"G\" style=\"width: var(--grain-w); height: 4px\"></div>"
    "</symbol></head></xt>\n";

/// The library `lib`: alpha (seed 1, font AlphaDisplay with its file, an extension --grain-w, a night
/// theme, Button and Grainy) and beta (seed 2, a night theme, Button).
struct LibT743 {
    fs::path dir;
    LibraryMounts mounts;
    explicit LibT743(const std::string& name) {
        dir = freshT743(name);
        writeT743(dir / "library.json", R"({ "version": 1, "id": "lib", "name": "Lib", "readOnly": false,
                                              "groups": ["alpha", "beta"] })");
        const fs::path a = dir / "groups" / "alpha";
        writeT743(a / "group.json", R"({ "version": 1, "id": "alpha", "name": "Alpha", "tokens": "tokens.xts",
            "themes": ["tokens-night.xts"], "source": { "kind": "manual", "set": "me/alpha", "setVersion": "1.0.0" } })");
        writeT743(a / "index.json", R"({ "version": 1, "tokens": "tokens.xts", "symbols": [
            { "name": "Button", "file": "Button.xt", "tokens": [{ "name": "--control-m" }, { "name": "--color-bg" }],
              "fonts": [], "textures": [] },
            { "name": "Grainy", "file": "Grainy.xt", "tokens": [{ "name": "--grain-w" }], "fonts": [], "textures": [] } ] })");
        writeT743(a / "Button.xt", kButtonT743);
        writeT743(a / "Grainy.xt", kGrainyT743);
        writeT743(a / "fonts" / "alpha.ttf", "ALPHA-FONT-BYTES");
        writeT743(a / "tokens.xts", coreSheetT743(1, "  --grain-w: 5px;\n",
                                                  "@font-face { font-family: \"AlphaDisplay\"; src: "
                                                  "\"fonts/alpha.ttf\"; font-weight: 700; }\n"));
        writeT743(a / "tokens-night.xts", ":root { --color-bg: #010101; --color-text: #fefefe; }\n");
        const fs::path b = dir / "groups" / "beta";
        writeT743(b / "group.json", R"({ "version": 1, "id": "beta", "name": "Beta", "tokens": "tokens.xts",
            "themes": ["tokens-night.xts"], "source": { "kind": "manual", "set": "me/beta", "setVersion": "2.1.0" } })");
        writeT743(b / "index.json", R"({ "version": 1, "tokens": "tokens.xts", "symbols": [
            { "name": "Button", "file": "Button.xt", "tokens": [{ "name": "--control-m" }, { "name": "--color-bg" }],
              "fonts": [], "textures": [] } ] })");
        writeT743(b / "Button.xt", kButtonT743);
        writeT743(b / "tokens.xts", coreSheetT743(2, "  --color-beta-glow: var(--color-accent);\n"));
        writeT743(b / "tokens-night.xts", ":root { --color-bg: #020202; }\n");
        REQUIRE(mount(mounts, dir) == Status::kOk);
    }
};

void useT743(const LibraryMounts& mounts, const UiProject& project, std::string_view name,
             std::string_view group) {
    const std::string_view names[] = {name};
    REQUIRE(useSymbols(mounts, project, names, group).status == Status::kOk);
}

const StyleTokenChange* findChangeT743(const std::vector<StyleTokenChange>& list, std::string_view theme,
                                       std::string_view name) {
    for (const StyleTokenChange& c : list) {
        if (c.theme == theme && c.name == name) {
            return &c;
        }
    }
    return nullptr;
}

std::string tokenOfT743(const fs::path& file, const std::string& name) {
    const std::string text = readT743(file);
    const size_t at = text.rfind(name + ":");
    if (at == std::string::npos) {
        return std::string();
    }
    const size_t start = at + name.size() + 1;
    const size_t end = text.find(';', start);
    std::string value = text.substr(start, end - start);
    value.erase(0, value.find_first_not_of(' '));
    return value;
}

/// The width of `#<id>` once the project's `main.xt` text `body` is compiled with the project's
/// token file and symbol files.
float widthT743(const UiProject& project, const std::string& links, const std::string& body,
                const std::string& id) {
    const fs::path root = compile::pathFromUtf8(project.rootDir);
    const std::string doc = "<xt><head>" + links + "</head><body id=\"Root\">" + body + "</body></xt>";
    const compile::SourceFile files[] = {{"main.xt", doc}};
    compile::CompileOptions options;
    options.emitMeta = false;
    options.rootDir = project.rootDir;
    options.xtDir = project.rootDir;
    options.impliedTokenFiles = {
        compile::genericPathToUtf8(root / compile::pathFromUtf8(project.shared.tokens))};
    const compile::CompileResult r = compile::compileDocumentFromStrings(files, "main.xt", options);
    test::requireCompiled(r);
    std::unique_ptr<Document> loaded;
    REQUIRE(Document::load(IrBlob{r.ir.data(), r.ir.size()}, true, nullptr, nullptr, loaded) == Status::kOk);
    loaded->setViewport(Rect{0, 0, 400, 100});
    loaded->update(0.0f);
    return loaded->byId(id).layoutBox().width;
}

bool hasIssueT743(const LintReport& report, const std::string& code, const std::string& text) {
    return std::any_of(report.issues.begin(), report.issues.end(), [&](const LintIssue& issue) {
        return issue.code == code && issue.message.find(text) != std::string::npos;
    });
}

}  // namespace

TEST_CASE("T-743 core vocabulary: 52 tokens, types, value checks") {
    CHECK(coreTokens().size() == 52);
    REQUIRE(findCoreToken("--color-bg") != nullptr);
    CHECK(findCoreToken("--color-bg")->type == CoreTokenType::Color);
    CHECK(findCoreToken("--tex-grain") == nullptr);
    CHECK(std::string(coreTokenTypeName(CoreTokenType::Family)) == "family");
    CHECK(coreTokenValueFits(CoreTokenType::Color, "#1f1c18b3"));
    CHECK(coreTokenValueFits(CoreTokenType::Color, "var(--x)"));
    CHECK_FALSE(coreTokenValueFits(CoreTokenType::Color, "12px"));
    CHECK(coreTokenValueFits(CoreTokenType::Length, "0"));
    CHECK(coreTokenValueFits(CoreTokenType::Length, "0.5px"));
    CHECK_FALSE(coreTokenValueFits(CoreTokenType::Length, "150ms"));
    CHECK(coreTokenValueFits(CoreTokenType::Time, "150ms"));
    CHECK_FALSE(coreTokenValueFits(CoreTokenType::Time, "4px"));
    CHECK(coreTokenValueFits(CoreTokenType::Family, "\"Noto Serif\", serif"));
    CHECK_FALSE(coreTokenValueFits(CoreTokenType::Family, "12px"));
}

TEST_CASE("T-743 apply style: dry run lists, writes, record, custom values, themes, fonts") {
    LibT743 lib("xt_t743_apply_lib");
    const UiProject project = projectT743("xt_t743_apply");
    const fs::path root = compile::pathFromUtf8(project.rootDir);
    const fs::path tokens = root / compile::pathFromUtf8(project.shared.tokens);
    writeT743(tokens, "/* Design tokens. */\n:root {\n  --color-bg: #111111;\n  --space-1: 1px;\n}\n");
    useT743(lib.mounts, project, "Button", "alpha");  // Appends alpha's --control-m.
    const std::string before = readT743(tokens);

    // Dry run: nothing written; --color-bg and --space-1 are overwritten (custom: no record, no implied
    // layer), --control-m already holds alpha's value, the other 49 are added.
    StyleReport dry = applyGroupStyle(lib.mounts, project, "lib", "alpha", {}, true);
    REQUIRE_MESSAGE(dry.status == Status::kOk, dry.error);
    CHECK(readT743(tokens) == before);
    CHECK(dry.overwritten.size() == 2);
    const StyleTokenChange* bg = findChangeT743(dry.overwritten, "", "--color-bg");
    REQUIRE(bg != nullptr);
    CHECK(bg->before == "#111111");
    CHECK(bg->after == valueOfT743(1, "--color-bg"));
    CHECK(bg->custom);
    CHECK(findChangeT743(dry.added, "", "--control-m") == nullptr);
    CHECK(std::count_if(dry.added.begin(), dry.added.end(),
                        [](const StyleTokenChange& c) { return c.theme.empty(); }) == 49);
    CHECK(findChangeT743(dry.added, "night", "--color-text") != nullptr);  // The theme file is new.
    CHECK(dry.missingCore.empty());
    REQUIRE(dry.copied.size() == 1);
    CHECK(dry.copied[0] == "fonts/alpha.ttf -> shared/fonts/alpha.ttf");

    StyleReport applied = applyGroupStyle(lib.mounts, project, "lib", "alpha");
    REQUIRE_MESSAGE(applied.status == Status::kOk, applied.error);
    CHECK(applied.changes.size() == dry.changes.size());
    CHECK(tokenOfT743(tokens, "--color-bg") == valueOfT743(1, "--color-bg"));
    CHECK(tokenOfT743(tokens, "--motion-fast") == valueOfT743(1, "--motion-fast"));
    const std::string text = readT743(tokens);
    CHECK(text.find("/* style Alpha (lib/alpha) */") != std::string::npos);
    CHECK(text.find(
              "@font-face { font-family: \"AlphaDisplay\"; src: \"fonts/alpha.ttf\"; font-weight: 700; }") !=
          std::string::npos);
    CHECK(text.find("--grain-w") == std::string::npos);  // Extension tokens are not written.
    CHECK(readT743(root / "shared" / "fonts" / "alpha.ttf") == "ALPHA-FONT-BYTES");
    const fs::path night = root / "shared" / "themes" / "night.xts";
    CHECK(tokenOfT743(night, "--color-bg") == "#010101");
    CHECK(tokenOfT743(night, "--color-text") == "#fefefe");

    ProjectStyle style;
    REQUIRE(readProjectStyle(project, style));
    CHECK(style.library == "lib");
    CHECK(style.group == "alpha");
    CHECK(style.name == "Alpha");
    CHECK(style.setVersion == "1.0.0");
    CHECK(style.tokens.size() == 52);
    CHECK(style.themes["night"].size() == 2);
    StyleStatus status = projectStyleStatus(project);
    CHECK(status.recorded);
    CHECK(status.changed.empty());
    // The copy record of Button is kept beside the style record.
    CopySource source;
    CHECK(readCopySource(project, "Button", source));

    // The user changes one value: "custom (from Alpha)", and the next style marks it.
    writeT743(tokens,
              readT743(tokens).replace(readT743(tokens).find(valueOfT743(1, "--color-bg")), 7, "#222222"));
    status = projectStyleStatus(project);
    CHECK(status.changed == std::vector<std::string>{"--color-bg"});
    StyleReport beta = applyGroupStyle(lib.mounts, project, "lib", "beta", {}, true);
    REQUIRE(beta.status == Status::kOk);
    CHECK(beta.overwritten.size() == 52 + 1);  // Every core value, plus night's --color-bg.
    for (const StyleTokenChange& c : beta.overwritten) {
        CAPTURE(c.name);
        CHECK(c.custom == (c.theme.empty() && c.name == "--color-bg"));
    }
    CHECK(findChangeT743(beta.overwritten, "night", "--color-bg")->after == "#020202");
    CHECK(beta.added.empty());
    CHECK(beta.copied.empty());  // BetaSerif has no @font-face.

    // Applied: alpha's file is replaced by beta's values in place; the record names beta.
    REQUIRE(applyGroupStyle(lib.mounts, project, "lib", "beta").status == Status::kOk);
    CHECK(tokenOfT743(tokens, "--color-bg") == valueOfT743(2, "--color-bg"));
    CHECK(tokenOfT743(tokens, "--control-m") == valueOfT743(2, "--control-m"));
    CHECK(tokenOfT743(night, "--color-text") == "#fefefe");  // Beta's night does not set it: kept.
    REQUIRE(readProjectStyle(project, style));
    CHECK(style.group == "beta");
    CHECK(projectStyleStatus(project).changed.empty());

    // A theme as the base: alpha + its night, and no theme file written.
    StyleReport themed = applyGroupStyle(lib.mounts, project, "lib", "alpha", "night", true);
    REQUIRE(themed.status == Status::kOk);
    CHECK(findChangeT743(themed.overwritten, "", "--color-bg")->after == "#010101");
    CHECK(std::none_of(themed.overwritten.begin(), themed.overwritten.end(),
                       [](const StyleTokenChange& c) { return !c.theme.empty(); }));
    CHECK(applyGroupStyle(lib.mounts, project, "lib", "alpha", "dusk", true).status == Status::kNotFound);
    CHECK(applyGroupStyle(lib.mounts, project, "lib", "gamma", {}, true).status == Status::kNotFound);
}

TEST_CASE("T-743 apply style: uncovered extension tokens, themes it does not provide, missing core tokens") {
    LibT743 lib("xt_t743_cover_lib");
    const UiProject project = projectT743("xt_t743_cover");
    const fs::path root = compile::pathFromUtf8(project.rootDir);
    useT743(lib.mounts, project, "Grainy",
            "alpha");  // --grain-w in an "added with Grainy from alpha" section.
    writeT743(root / "shared" / "themes" / "sepia.xts", ":root { --color-text: #000000; }\n");
    const StyleReport report = applyGroupStyle(lib.mounts, project, "lib", "beta", {}, true);
    REQUIRE(report.status == Status::kOk);
    REQUIRE(report.uncovered.size() == 1);
    CHECK(report.uncovered[0].name == "--grain-w");
    CHECK(report.uncovered[0].origin == "alpha");
    CHECK(report.uncovered[0].usedBy == std::vector<std::string>{"Grainy"});
    CHECK(report.themesNotProvided == std::vector<std::string>{"sepia"});

    // A group missing a core token: listed, and the project's value stays.
    const fs::path beta = lib.dir / "groups" / "beta" / "tokens.xts";
    std::string sheet = readT743(beta);
    const size_t at = sheet.find("  --motion-normal:");
    sheet.erase(at, sheet.find('\n', at) + 1 - at);
    writeT743(beta, sheet);
    LibraryMounts remounted;
    REQUIRE(mount(remounted, lib.dir) == Status::kOk);
    const StyleReport missing = applyGroupStyle(remounted, project, "lib", "beta", {}, true);
    CHECK(missing.missingCore == std::vector<std::string>{"--motion-normal"});
}

TEST_CASE("T-743 duplicate and detach: the copy keeps its look when the style changes") {
    LibT743 lib("xt_t743_detach_lib");
    const UiProject project = projectT743("xt_t743_detach");
    const fs::path root = compile::pathFromUtf8(project.rootDir);
    REQUIRE(applyGroupStyle(lib.mounts, project, "lib", "alpha").status == Status::kOk);
    useT743(lib.mounts, project, "Button", "alpha");

    DetachReport bad = duplicateDetached(project, "Button", "Button");
    CHECK(bad.status == Status::kInvalidArgument);
    CHECK(duplicateDetached(project, "Nope", "Other").status == Status::kNotFound);

    const DetachReport report = duplicateDetached(project, "Button", "FancyButton");
    REQUIRE_MESSAGE(report.status == Status::kOk, report.error);
    CHECK(report.file == "shared/symbols/FancyButton.xt");
    CHECK(report.unresolved.empty());
    REQUIRE(report.tokens.size() == 2);
    CHECK(report.tokens[0].from == "--control-m");
    CHECK(report.tokens[0].to == "--fancybutton-control-m");
    CHECK(report.tokens[0].value == valueOfT743(1, "--control-m"));
    CHECK_FALSE(fs::exists(root / "shared" / "symbols" / "FancyButton.xt"));  // Planned only.
    REQUIRE(writeFileChanges(project, report.changes) == Status::kOk);
    const std::string copy = readT743(root / "shared" / "symbols" / "FancyButton.xt");
    CHECK(copy.find("<symbol name=\"FancyButton\">") != std::string::npos);
    CHECK(copy.find("var(--fancybutton-control-m)") != std::string::npos);
    CHECK(copy.find("var(--control-m)") == std::string::npos);
    const std::string tokens = readT743(root / compile::pathFromUtf8(project.shared.tokens));
    CHECK(tokens.find("/* detached: FancyButton (from Button) */") != std::string::npos);
    // The night theme gives --color-bg another value: the detached token gets it too.
    CHECK(tokenOfT743(root / "shared" / "themes" / "night.xts", "--fancybutton-color-bg") == "#010101");
    CopySource source;
    CHECK_FALSE(readCopySource(project, "FancyButton", source));  // The project's own: never updated.
    const std::string index = readT743(root / "shared" / "symbols" / "index.json");
    CHECK(index.find("\"derivedFrom\"") != std::string::npos);

    const std::string links =
        "<link rel=\"symbols\" href=\"shared/symbols/Button.xt\"/>"
        "<link rel=\"symbols\" href=\"shared/symbols/FancyButton.xt\"/>";
    const std::string body = R"(<use symbol="Button" id="A"/><use symbol="FancyButton" id="B"/>)";
    const float alphaWidth = static_cast<float>(std::stoi(valueOfT743(1, "--control-m")));
    CHECK(widthT743(project, links, body, "A/Bg") == doctest::Approx(alphaWidth));
    CHECK(widthT743(project, links, body, "B/Bg") == doctest::Approx(alphaWidth));
    REQUIRE(applyGroupStyle(lib.mounts, project, "lib", "beta").status == Status::kOk);
    CHECK(widthT743(project, links, body, "A/Bg") ==
          doctest::Approx(static_cast<float>(std::stoi(valueOfT743(2, "--control-m")))));
    CHECK(widthT743(project, links, body, "B/Bg") == doctest::Approx(alphaWidth));  // Detached.
}

TEST_CASE("T-743 implied layer of a project with several groups (47 rule 5)") {
    LibT743 lib("xt_t743_implied_lib");
    const UiProject created = projectT743("xt_t743_implied");
    const fs::path root = compile::pathFromUtf8(created.rootDir);
    writeT743(root / "shared" / "symbols" / "Button.xt", kButtonT743);  // The project's own Button.
    useT743(lib.mounts, created, "Button", "beta");                     // -> beta/Button
    useT743(lib.mounts, created, "Grainy", "alpha");                    // -> Grainy (flat)
    std::string error;
    Result<UiProject> loaded = UiProject::load(created.rootDir, &error);
    REQUIRE_MESSAGE(loaded.ok(), error);
    UiProject project = loaded.value();
    REQUIRE_FALSE(project.uis.empty());
    const fs::path doc = root / compile::pathFromUtf8(project.uis[0].doc);
    const std::string relSymbols =
        compile::genericPathToUtf8((root / "shared" / "symbols").lexically_relative(doc.parent_path()));
    auto link = [&](const std::string& file) {
        return "<link rel=\"symbols\" href=\"" + relSymbols + "/" + file + "\"/>";
    };
    const std::vector<std::string> dirs{compile::genericPathToUtf8(lib.dir)};

    // One group linked: no style, no standard group -> nothing implied (as before T-743).
    writeT743(
        doc, "<xt><head>" + link("Grainy.xt") + "</head><body><use symbol=\"Grainy\" id=\"G\"/></body></xt>");
    SymbolGroup group;
    std::string dir;
    CHECK_FALSE(projectImpliedGroup(project.rootDir, project.uis, std::string(), project.shared.symbols, dirs,
                                    group, &dir));

    // Two groups: the first linked one.
    writeT743(doc, "<xt><head>" + link("Grainy.xt") + link("beta/Button.xt") +
                       "</head><body><use symbol=\"Grainy\" id=\"G\"/><use symbol=\"beta/Button\" "
                       "id=\"B\"/></body></xt>");
    REQUIRE(projectImpliedGroup(project.rootDir, project.uis, std::string(), project.shared.symbols, dirs,
                                group, &dir));
    CHECK(group.id == "alpha");
    CHECK(dir == compile::genericPathToUtf8(lib.dir / "groups" / "alpha"));
    CHECK(projectImpliedTokenFile(project, std::string(), dirs) ==
          compile::genericPathToUtf8(lib.dir / "groups" / "alpha" / "tokens.xts"));
    // Not mounted: the T-498b rule (nothing here).
    CHECK_FALSE(
        projectImpliedGroup(project.rootDir, project.uis, std::string(), project.shared.symbols, {}, group));

    // The applied style wins.
    REQUIRE(applyGroupStyle(lib.mounts, project, "lib", "beta").status == Status::kOk);
    REQUIRE(projectImpliedGroup(project.rootDir, project.uis, std::string(), project.shared.symbols, dirs,
                                group, &dir));
    CHECK(group.id == "beta");
    // ... even with one group linked.
    writeT743(
        doc, "<xt><head>" + link("Grainy.xt") + "</head><body><use symbol=\"Grainy\" id=\"G\"/></body></xt>");
    REQUIRE(projectImpliedGroup(project.rootDir, project.uis, std::string(), project.shared.symbols, dirs,
                                group, &dir));
    CHECK(group.id == "beta");
}

TEST_CASE("T-743 lint: token.core and token.role") {
    LibT743 lib("xt_t743_lint_lib");
    LintOptions options;
    options.presetGroupDir = compile::genericPathToUtf8(kPresetT743 / "groups" / "broadsheet-2026");
    const fs::path alpha = lib.dir / "groups" / "alpha";
    const fs::path beta = lib.dir / "groups" / "beta";

    // Complete groups: no token.core. beta's extension colour is written with a core token: no token.role.
    LintReport report = lintGroup(beta, options);
    CHECK_FALSE(hasIssueT743(report, "token.core", ""));
    CHECK_FALSE(hasIssueT743(report, "token.role", ""));
    // index-only lint (what saveAsGroup records) does not run them.
    LintOptions indexOnly = options;
    indexOnly.indexOnly = true;

    // A missing core token and one of the wrong type.
    std::string sheet = readT743(alpha / "tokens.xts");
    sheet.erase(sheet.find("  --radius-s:"),
                sheet.find('\n', sheet.find("  --radius-s:")) + 1 - sheet.find("  --radius-s:"));
    const size_t motion = sheet.find("--motion-fast:");
    sheet.replace(motion, sheet.find(';', motion) - motion, "--motion-fast: 12px");
    // An extension colour with a literal value and no theme value; textures named like the preset's.
    sheet.replace(sheet.rfind('}'), 1,
                  "  --color-alpha-glow: #ff00ff;\n  --tex-grain: url(\"textures/film.png\");\n"
                  "  --tex-hatch: url(\"textures/hatch-ink.png\");\n}");
    writeT743(alpha / "tokens.xts", sheet);
    report = lintGroup(alpha, options);
    CHECK(hasIssueT743(report, "token.core", "missing core token --radius-s"));
    CHECK(hasIssueT743(report, "token.core", "--motion-fast: '12px' is not a time"));
    CHECK(hasIssueT743(report, "token.role", "--color-alpha-glow"));
    CHECK(hasIssueT743(report, "token.role", "--tex-grain names 'film.png'"));
    CHECK_FALSE(hasIssueT743(report, "token.role", "--tex-hatch"));
    CHECK_FALSE(hasIssueT743(lintGroup(alpha, indexOnly), "token.core", ""));
    // Without the preset group the rule 3 comparison is skipped.
    CHECK_FALSE(hasIssueT743(lintGroup(alpha, LintOptions{}), "token.role", "--tex-grain"));

    // The extension colour set in every theme file follows the themes: no token.role.
    writeT743(alpha / "tokens-night.xts", ":root { --color-bg: #010101; --color-alpha-glow: #00ff00; }\n");
    CHECK_FALSE(hasIssueT743(lintGroup(alpha, options), "token.role", "--color-alpha-glow"));
}
