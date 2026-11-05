#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "doctest.h"
#include "test_temp.h"
#include "xt/bundle/design_doc.h"
#include "xt/bundle/design_system.h"
#include "xt/bundle/symbol_lint.h"
#include "xt/bundle/ui_project.h"

/// T-606 (Docs/design/54-source-format-v2.md §54.3, §54.4, §54.7, §54.8; Docs/tasks/T-606.md): the
/// readers of xt_bundle that scanned `.xt` text for `port:`, `<link>` and `<style>` read the parsed
/// DomTree now, so a source format 2 file reads like its format 1 original. The format 2 copies are
/// the output of `xtframe-compile --migrate --anchors -o <dir>` (T-589), committed under
/// Tests/bundle/fixtures/v2/ (only the files the migration writes; each test lays them over a copy
/// of the format 1 original, which stays as it is):
///   - broadsheet-2026: Assets/Symbols/groups/broadsheet-2026 (index.json content hashes refreshed
///     with Tools/Scripts/gen_symbols_index.py --dir);
///   - ui-project-starter: Examples/ui-project-starter;
///   - game-ui: Examples/demos/game-ui.

using namespace xt;
using namespace xt::bundle;
using namespace xt::bundle::design;

namespace {

namespace fs = std::filesystem;

fs::path sourcePath(const char* relative) { return fs::path(XT_TEST_BUNDLE_DIR) / "../.." / relative; }

fs::path fixturePath(const char* name) { return fs::path(XT_TEST_BUNDLE_DIR) / "fixtures" / "v2" / name; }

/// A fresh temp directory `xt_t606_<name>` (removed first).
fs::path freshDirT606(const std::string& name) {
    const fs::path dir = xt::test::tempDir() / ("xt_t606_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

std::string readTextT606(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeTextT606(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

/// A copy of `source` at `target`, with the v2 fixture `fixture` laid over it when not null.
void copyTree(const fs::path& source, const fs::path& target, const char* fixture) {
    std::error_code ec;
    fs::create_directories(target, ec);
    fs::copy(source, target, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    REQUIRE(ec.value() == 0);
    if (fixture != nullptr) {
        fs::copy(fixturePath(fixture), target,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        REQUIRE(ec.value() == 0);
    }
}

/// `<temp>/xt_t606_<name>/broadsheet-2026`: the preset group, format 1 or (`v2`) the migrated copy.
fs::path groupCopy(const std::string& name, bool v2) {
    const fs::path group = freshDirT606(name) / "broadsheet-2026";
    copyTree(sourcePath("Assets/Symbols/groups/broadsheet-2026"), group, v2 ? "broadsheet-2026" : nullptr);
    return group;
}

UiProject loadProject(const fs::path& dir) {
    std::string error;
    Result<UiProject> loaded = UiProject::load(dir.generic_string(), &error);
    REQUIRE_MESSAGE(loaded.ok(), error);
    return loaded.value();
}

/// The starter, format 1 or 2, with the standard symbols of the group of the same format pulled in.
UiProject starterCopy(const std::string& name, bool v2) {
    const fs::path root = freshDirT606(name);
    const fs::path dir = root / "starter";
    copyTree(sourcePath("Examples/ui-project-starter"), dir, v2 ? "ui-project-starter" : nullptr);
    const fs::path group = root / "broadsheet-2026";
    copyTree(sourcePath("Assets/Symbols/groups/broadsheet-2026"), group, v2 ? "broadsheet-2026" : nullptr);
    const UiProject project = loadProject(dir);
    REQUIRE(pullStandardSymbols(project, group.generic_string()) == Status::kOk);
    return project;
}

std::string issuesText(const LintReport& report) {
    std::string text;
    for (const LintIssue& issue : report.issues) {
        text += issue.symbol + " " + issue.code + ": " + issue.message + "\n";
    }
    return text;
}

/// The computed (variable use) edges of a graph, as (from, to) pairs.
std::set<std::pair<std::string, std::string>> usesEdges(const Graph& graph) {
    std::set<std::pair<std::string, std::string>> out;
    for (const Edge& e : graph.edges) {
        if (e.computed) {
            out.emplace(e.from, e.to);
        }
    }
    return out;
}

/// The project nodes of a graph (`ui:`, `layer:`, `symbol:`).
std::set<std::string> projectNodes(const Graph& graph) {
    std::set<std::string> out;
    for (const std::string& node : graph.nodes) {
        if (node.rfind("ui:", 0) == 0 || node.rfind("layer:", 0) == 0 || node.rfind("symbol:", 0) == 0) {
            out.insert(node);
        }
    }
    return out;
}

/// (value, count) of every literal of a list, in report order.
std::vector<std::pair<std::string, int>> counts(const std::vector<AuditReport::Literal>& list) {
    std::vector<std::pair<std::string, int>> out;
    for (const AuditReport::Literal& l : list) {
        out.emplace_back(l.value, l.count);
    }
    return out;
}

/// The token and property statistics of a project: the graph's variable-use edges and project
/// nodes, and the audit's literal colours, lengths and fonts with their counts and the token
/// coverage (scope `all`). The `where` lists are left out: the migration moves rules into
/// `<stem>.layout.xts` fragments, so a file-level `where` names another file.
struct Stats {
    std::set<std::pair<std::string, std::string>> edges;
    std::set<std::string> nodes;
    std::vector<std::pair<std::string, int>> colors, sizes, fonts;
    float coverage = 0.0f;
};

Stats statsOf(const UiProject& project) {
    DesignSystem ds;
    (void)load(project, ds);  // game-ui has no design/: an empty design system.
    Stats stats;
    Graph graph;
    std::string error;
    REQUIRE_MESSAGE(buildGraph(project, ds, graph, &error) == Status::kOk, error);
    stats.edges = usesEdges(graph);
    stats.nodes = projectNodes(graph);
    AuditReport report;
    REQUIRE(audit(project, ds, "all", report) == Status::kOk);
    stats.colors = counts(report.colors);
    stats.sizes = counts(report.sizes);
    stats.fonts = counts(report.fonts);
    stats.coverage = report.tokenCoverage;
    return stats;
}

void checkSameStats(const Stats& v1, const Stats& v2) {
    CHECK(v2.edges == v1.edges);
    CHECK(v2.nodes == v1.nodes);
    CHECK(v2.colors == v1.colors);
    CHECK(v2.sizes == v1.sizes);
    CHECK(v2.fonts == v1.fonts);
    CHECK(v2.coverage == doctest::Approx(v1.coverage));
}

}  // namespace

TEST_CASE("T-606 fixtures: the committed copies are source format 2") {
    CHECK(readTextT606(fixturePath("broadsheet-2026") / "Button.xt").rfind("<xt version=\"2\">", 0) == 0);
    CHECK(readTextT606(fixturePath("broadsheet-2026") / "Button.xt").find("<ports>") != std::string::npos);
    CHECK(readTextT606(fixturePath("ui-project-starter") / "ui" / "hud" / "hud.xt")
              .find("<symbols src=\"../../shared/symbols/HealthBar.xt\"/>") != std::string::npos);
    CHECK(readTextT606(fixturePath("game-ui") / "hud" / "hud.xt").find("<style src=") != std::string::npos);
    CHECK(fs::is_regular_file(fixturePath("game-ui") / "hud" / "hud.layout.xts"));
}

TEST_CASE(
    "T-606 symbol lint: the migrated standard library lints without a finding, ports read from <ports>") {
    const fs::path group = groupCopy("lint", true);
    for (const bool indexOnly : {false, true}) {
        CAPTURE(indexOnly);
        LintOptions options;
        options.indexOnly = indexOnly;
        const LintReport report = lintGroup(group, options);
        CHECK(report.symbols == 39);
        CHECK_MESSAGE(report.issues.empty(), issuesText(report));
    }

    // A port the index lists that `<ports>` declares with another direction; a contract that
    // differs from the index.
    const fs::path broken = groupCopy("lint_broken", true);
    std::string button = readTextT606(broken / "Button.xt");
    const size_t port = button.find("pressed: event out");
    REQUIRE(port != std::string::npos);
    button.replace(port, 18, "pressed: event in");
    const size_t contract = button.find("slots: label\"");
    REQUIRE(contract != std::string::npos);
    button.replace(contract, 13, "slots: caption\"");
    writeTextT606(broken / "Button.xt", button);
    LintOptions indexOnly;
    indexOnly.indexOnly = true;
    const LintReport report = lintGroup(broken, indexOnly);
    bool portIssue = false;
    bool contractIssue = false;
    for (const LintIssue& issue : report.issues) {
        portIssue = portIssue || (issue.symbol == "Button" && issue.code == "index.port" &&
                                  issue.message == "no port:pressed=\"event out\"");
        contractIssue = contractIssue || (issue.symbol == "Button" && issue.code == "index.contract");
    }
    CHECK_MESSAGE(portIssue, issuesText(report));
    CHECK_MESSAGE(contractIssue, issuesText(report));
}

TEST_CASE("T-606 styleRegions: a v2 file's inline sections and style attributes are scanned") {
    const std::string button = readTextT606(fixturePath("broadsheet-2026") / "Button.xt");
    std::string all;
    for (const std::string& region : styleRegions(button, false)) {
        all += region + "\n";
    }
    CHECK(all.find("min-width: 128px;") != std::string::npos);                       // <layout>
    CHECK(all.find("background-color: var(--color-accent);") != std::string::npos);  // <style>
    CHECK(all.find("@timeline hover-in") != std::string::npos);                      // <timelines>
    const std::string doc =
        "<xt version=\"2\"><head></head><body><div id=\"A\" style=\"color: #ff0000;\"/></body>"
        "<style>#A { border-color: red; }</style></xt>";
    std::vector<std::string> found;
    for (const std::string& region : styleRegions(doc, false)) {
        for (const std::string& color : findLiteralColors(region)) {
            found.push_back(color);
        }
    }
    CHECK(found == std::vector<std::string>{"border-color: red", "color: #ff0000"});
}

TEST_CASE("T-606 design graph and audit: the v2 starter has the token and property statistics of v1") {
    const Stats v1 = statsOf(starterCopy("starter_v1", false));
    const Stats v2 = statsOf(starterCopy("starter_v2", true));
    checkSameStats(v1, v2);
    CHECK(!v1.edges.empty());
    CHECK(v1.coverage > 0.5f);
}

TEST_CASE("T-606 DESIGN.md: the v2 starter generates the document of v1") {
    std::string markdown[2];
    for (const bool v2 : {false, true}) {
        const UiProject project = starterCopy(v2 ? "doc_v2" : "doc_v1", v2);
        DesignSystem ds;
        REQUIRE(load(project, ds) == Status::kOk);
        Graph graph;
        REQUIRE(buildGraph(project, ds, graph) == Status::kOk);
        BuildOptions options;
        options.standardSymbolDir =
            (fs::path(project.rootDir).parent_path() / "broadsheet-2026").generic_string();
        REQUIRE(buildDocument(project, ds, graph, options, markdown[v2 ? 1 : 0], nullptr) == Status::kOk);
    }
    CHECK(!markdown[0].empty());
    CHECK(markdown[1] == markdown[0]);
}

TEST_CASE("T-606 design graph and audit: the v2 game-ui has the token and property statistics of v1") {
    const fs::path v1Dir = freshDirT606("game_v1") / "game-ui";
    copyTree(sourcePath("Examples/demos/game-ui"), v1Dir, nullptr);
    const fs::path v2Dir = freshDirT606("game_v2") / "game-ui";
    copyTree(sourcePath("Examples/demos/game-ui"), v2Dir, "game-ui");
    const Stats v1 = statsOf(loadProject(v1Dir));
    const Stats v2 = statsOf(loadProject(v2Dir));
    checkSameStats(v1, v2);
    CHECK(!v1.nodes.empty());  // game-ui writes literal values, no variable: no use edge.
    CHECK(!v1.colors.empty());
    CHECK(!v1.sizes.empty());
}
