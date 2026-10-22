#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "color_math.h"
#include "doctest.h"
#include "test_temp.h"
#include "xt/bundle/design_color.h"
#include "xt/bundle/design_doc.h"
#include "xt/bundle/design_system.h"
#include "xt/bundle/ui_project.h"

/// The design document, the colour tools, the audit and the remaining design lint
/// (Docs/design/35-design-system.md §35.3.3, §35.4, §35.7.1, §35.10; Docs/tasks/T-309.md item 7).

using namespace xt;
using namespace xt::bundle;
using namespace xt::bundle::design;
using nlohmann::json;

namespace {

namespace fs = std::filesystem;

std::string sourceDir(const char* relative) { return std::string(XT_TEST_BUNDLE_DIR) + "/../../" + relative; }

std::string freshDir(const std::string& name) {
    const fs::path dir = xt::test::tempDir() / ("xt_designdoc_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir.generic_string();
}

std::string readText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/// A writable copy of the starter UI Project with its standard symbols pulled in.
UiProject starterCopy(const std::string& name) {
    const std::string dir = freshDir(name);
    std::error_code ec;
    fs::copy(sourceDir("Examples/ui-project-starter"), dir, fs::copy_options::recursive, ec);
    REQUIRE(ec.value() == 0);
    std::string error;
    Result<UiProject> loaded = UiProject::load(dir, &error);
    REQUIRE_MESSAGE(loaded.ok(), error);
    REQUIRE(pullStandardSymbols(loaded.value(), sourceDir("Assets/Symbols")) == Status::kOk);
    return loaded.value();
}

struct Loaded {
    DesignSystem ds;
    Graph graph;
};

Loaded loadDesign(const UiProject& project) {
    Loaded out;
    std::string error;
    REQUIRE_MESSAGE(load(project, out.ds, nullptr, &error) == Status::kOk, error);
    REQUIRE_MESSAGE(buildGraph(project, out.ds, out.graph, &error) == Status::kOk, error);
    return out;
}

bool hasIssue(const std::vector<Issue>& issues, const std::string& id, Severity severity) {
    for (const Issue& issue : issues) {
        if (issue.id == id && issue.severity == severity) {
            return true;
        }
    }
    return false;
}

const Issue* findIssue(const std::vector<Issue>& issues, const std::string& id, const std::string& entry) {
    for (const Issue& issue : issues) {
        if (issue.id == id && issue.entry == entry) {
            return &issue;
        }
    }
    return nullptr;
}

size_t lines(const std::string& text) {
    size_t n = 0;
    for (char c : text) {
        n += c == '\n' ? 1 : 0;
    }
    return n;
}

const AuditReport::Literal* literal(const std::vector<AuditReport::Literal>& list, const std::string& value) {
    for (const AuditReport::Literal& l : list) {
        if (l.value == value) {
            return &l;
        }
    }
    return nullptr;
}

/// An RGBA picture: `w` columns split into vertical bands of these colours and widths (fractions).
std::vector<uint8_t> bands(int w, int h, const std::vector<std::pair<uint32_t, float>>& parts) {
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    for (int x = 0; x < w; ++x) {
        float edge = 0.0f;
        uint32_t color = parts.back().first;
        for (const auto& [c, f] : parts) {
            edge += f;
            if ((static_cast<float>(x) + 0.5f) / static_cast<float>(w) < edge) {
                color = c;
                break;
            }
        }
        for (int y = 0; y < h; ++y) {
            uint8_t* p =
                &rgba[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4];
            p[0] = static_cast<uint8_t>(color >> 16);
            p[1] = static_cast<uint8_t>(color >> 8);
            p[2] = static_cast<uint8_t>(color);
            p[3] = 255;
        }
    }
    return rgba;
}

}  // namespace

TEST_CASE("T-309 (a): the starter's DESIGN.md: byte-stable, every section, draft marks, AI truncation") {
    const UiProject project = starterCopy("doc_starter");
    Loaded l = loadDesign(project);
    BuildOptions options;
    options.standardSymbolDir = sourceDir("Assets/Symbols");
    std::string first;
    std::string second;
    std::vector<Issue> issues;
    REQUIRE(buildDocument(project, l.ds, l.graph, options, first, &issues) == Status::kOk);
    REQUIRE(buildDocument(project, l.ds, l.graph, options, second, nullptr) == Status::kOk);
    CHECK(first == second);
    CHECK(first.rfind(std::string(kDesignDocBanner) + "\n", 0) == 0);
    CHECK(first.find('\r') == std::string::npos);
    const char* headings[] = {"## 1 · Identity", "## 2 · Voice",           "## 3 · Color",
                              "## 4 · Type",     "## 5 · Space and shape", "## 6 · Motion",
                              "## 7 · Layout",   "## 8 · Components",      "## 9 · Imagery",
                              "## 10 · Screens", "## 11 · Decisions",      "## 12 · Changelog",
                              "## 13 · Assets"};
    size_t last = 0;
    for (const char* h : headings) {
        const size_t at = first.find(std::string("\n") + h + "\n");
        CHECK_MESSAGE(at != std::string::npos, h);
        CHECK_MESSAGE(at > last, h);  // fixed order
        last = at;
    }
    CHECK(first.find("## 14") == std::string::npos);
    // The texts have no status (= draft); the swatches and variables are approved.
    CHECK(first.find("### Principles (draft)") != std::string::npos);
    CHECK(first.find("### Voice (draft)") != std::string::npos);
    CHECK(first.find("| Teal (`teal`) | `#0f7b8a` | primary |") != std::string::npos);
    CHECK(first.find("`--color-accent` (draft)") == std::string::npos);
    // Variables with the theme column, contrast-free palette, data sections.
    CHECK(first.find("| Variable | Value | dark | From | Description | UIs |") != std::string::npos);
    CHECK(first.find("| `--color-accent` | `#0f7b8a` | `#4cc2ff` | `swatch:teal` |") != std::string::npos);
    CHECK(first.find("| `duration.fast` | [100, 200] |") != std::string::npos);
    CHECK(first.find("`conventions/layouts/hud.xt`") != std::string::npos);
    CHECK(first.find("- Default: 2.5 % of the viewport") != std::string::npos);
    CHECK(first.find("### Standard library") != std::string::npos);
    CHECK(first.find("| `Button` | form |") != std::string::npos);
    CHECK(first.find("History is not enabled for this project.") != std::string::npos);
    CHECK(first.find("| `zh-TW` | Microsoft JhengHei, Noto Sans TC, Noto Sans CJK TC |") !=
          std::string::npos);
    // No heading deeper than ### (the §35.16 subset).
    CHECK(first.find("\n#### ") == std::string::npos);
    for (const Issue& issue : issues) {
        CHECK_MESSAGE(issue.severity != Severity::Error,
                      (issue.id + " " + issue.entry + ": " + issue.message));
    }

    // writeDocument -> no docStale; a source change -> docStale.
    std::string error;
    REQUIRE_MESSAGE(writeDocument(project, l.ds, l.graph, options, nullptr, &error) == Status::kOk, error);
    const std::string onDisk = readText(designDir(project) + "/DESIGN.md");
    CHECK(onDisk == first);
    CHECK_FALSE(hasIssue(lintDocument(project, l.ds, l.graph, options), "design.docStale", Severity::Info));

    // AI version: no images / deprecated, every section cut with "… (N more)".
    BuildOptions ai = options;
    ai.forAi = true;
    ai.maxTokensHint = 650;  // 50 tokens = 200 characters a section
    std::string small;
    REQUIRE(buildDocument(project, l.ds, l.graph, ai, small, nullptr) == Status::kOk);
    CHECK(small.size() < first.size() / 4);
    CHECK(small.find(" more)") != std::string::npos);
    CHECK(small.find("…") != std::string::npos);
    CHECK(small.find("## 13 · Assets") != std::string::npos);
    BuildOptions aiDefault = options;
    aiDefault.forAi = true;
    std::string full;
    REQUIRE(buildDocument(project, l.ds, l.graph, aiDefault, full, nullptr) == Status::kOk);
    CHECK((full.size() + 3) / 4 <= 20000 + 400);

    // design.yaml:sections hides, never reorders; BuildOptions::sections narrows further.
    DesignSystem hidden = l.ds;
    hidden.info.sections = {"color", "identity"};
    std::string some;
    REQUIRE(buildDocument(project, hidden, l.graph, options, some, nullptr) == Status::kOk);
    CHECK(some.find("## 1 · Identity") < some.find("## 3 · Color"));
    CHECK(some.find("## 2 · Voice") == std::string::npos);
    BuildOptions only = options;
    only.sections = {"3"};
    REQUIRE(buildDocument(project, hidden, l.graph, only, some, nullptr) == Status::kOk);
    CHECK(some.find("## 1 · Identity") == std::string::npos);
    CHECK(some.find("## 3 · Color") != std::string::npos);

    char line[200];
    std::snprintf(line, sizeof(line),
                  "T-309 DESIGN.md: starter %zu lines (%zu bytes); for-ai %zu tokens (est.)", lines(first),
                  first.size(), (full.size() + 3) / 4);
    MESSAGE(line);
    std::printf("%s\n", line);
}

TEST_CASE("T-309 (b): entries, drafts, deprecated, featured references, decisions") {
    const UiProject project = starterCopy("doc_entries");
    Loaded l = loadDesign(project);
    // A draft swatch, a deprecated one, a featured reference and three decisions.
    Swatch draft;
    draft.base.kind = EntryKind::Swatch;
    draft.base.id = "ember";
    draft.base.name = "Ember";
    draft.value = "#ff5a1f";
    draft.role = "accent";
    draft.group = "brand";
    l.ds.swatches.push_back(draft);
    l.ds.groups[0].swatches.push_back("ember");
    Swatch old = draft;
    old.base.id = "old-teal";
    old.base.name = "Old teal";
    old.base.status = EntryStatus::Deprecated;
    l.ds.swatches.push_back(old);
    l.ds.groups[0].swatches.push_back("old-teal");
    ReferenceMeta ref;
    ref.base.kind = EntryKind::Reference;
    ref.base.id = "moodboard/key-art";
    ref.base.tags = {"featured"};
    ref.base.note = "Warm light";
    ref.title = "Key art";
    ref.path = "references/moodboard/key-art.png";
    l.ds.references.push_back(ref);
    TextEntry accepted;
    accepted.base.kind = EntryKind::Decision;
    accepted.base.id = "0002";
    accepted.base.extra = json{{"status", "accepted"}, {"date", "2026-10-01"}};
    accepted.title = "Spacing scale";
    accepted.body = "## Context\n\nToo many gaps.\n\n## Decision\n\nA 4 px scale.\n";
    l.ds.decisions.push_back(accepted);
    TextEntry superseded = accepted;
    superseded.base.id = "0001";
    superseded.base.extra = json{{"status", "superseded"}};
    superseded.title = "Free spacing";
    l.ds.decisions.push_back(superseded);
    TextEntry proposed = accepted;
    proposed.base.id = "0003";
    proposed.base.extra = json{{"status", "proposed"}};
    proposed.base.note = "Try an 8 px grid.";
    proposed.title = "Grid";
    l.ds.decisions.push_back(proposed);

    std::string md;
    REQUIRE(buildDocument(project, l.ds, l.graph, BuildOptions(), md, nullptr) == Status::kOk);
    CHECK(md.find("| Ember (`ember`) (draft) | `#ff5a1f` | accent | 0 |") != std::string::npos);
    CHECK(md.find("Old teal (`old-teal`)") == std::string::npos);
    const size_t deprecated = md.find("### Deprecated");
    REQUIRE(deprecated != std::string::npos);
    CHECK(md.find("- Old teal (`swatch:old-teal`)", deprecated) != std::string::npos);
    CHECK(md.find("![Key art](references/moodboard/key-art.png)") != std::string::npos);
    CHECK(md.find("- **0002 · Spacing scale** (accepted, 2026-10-01): A 4 px scale.") != std::string::npos);
    CHECK(md.find("- **0003 · Grid** (draft): Try an 8 px grid.") != std::string::npos);
    const size_t sup = md.find("### Superseded");
    REQUIRE(sup != std::string::npos);
    CHECK(md.find("- **0001 · Free spacing** (superseded)", sup) != std::string::npos);

    BuildOptions ai;
    ai.forAi = true;
    std::string aiMd;
    REQUIRE(buildDocument(project, l.ds, l.graph, ai, aiMd, nullptr) == Status::kOk);
    CHECK(aiMd.find("![") == std::string::npos);
    CHECK(aiMd.find("### Deprecated") == std::string::npos);
    CHECK(aiMd.find("- Key art (draft) (`references/moodboard/key-art.png`): Warm light") !=
          std::string::npos);
}

TEST_CASE("T-309 (c): extractPalette finds three blocks with sensible shares; 1920x1080 timing") {
    const std::vector<uint8_t> rgba = bands(300, 200, {{0xff0000, 0.5f}, {0x00a000, 0.3f}, {0x0000ff, 0.2f}});
    const std::vector<ExtractedColor> colors = extractPalette(rgba.data(), 300, 200, 8);
    REQUIRE(colors.size() == 3);
    CHECK(colors[0].value == "#ff0000");
    CHECK(colors[1].value == "#00a000");
    CHECK(colors[2].value == "#0000ff");
    CHECK(colors[0].share == doctest::Approx(0.5).epsilon(0.03));
    CHECK(colors[1].share == doctest::Approx(0.3).epsilon(0.05));
    CHECK(colors[2].share == doctest::Approx(0.2).epsilon(0.05));
    CHECK(extractPalette(rgba.data(), 300, 200, 8)[0].value == colors[0].value);  // deterministic
    CHECK(extractPalette(rgba.data(), 300, 200, 2).size() == 2);
    CHECK(extractPalette(nullptr, 10, 10).empty());

    // A gradient with noise at 1920 x 1080.
    const int w = 1920;
    const int h = 1080;
    std::vector<uint8_t> big(static_cast<size_t>(w) * h * 4);
    uint32_t seed = 12345;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            seed = seed * 1664525u + 1013904223u;
            uint8_t* p = &big[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4];
            p[0] = static_cast<uint8_t>((x * 255) / w);
            p[1] = static_cast<uint8_t>((y * 255) / h);
            p[2] = static_cast<uint8_t>(seed >> 24);
            p[3] = 255;
        }
    }
    const auto start = std::chrono::steady_clock::now();
    const std::vector<ExtractedColor> palette = extractPalette(big.data(), w, h, 8);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    CHECK(palette.size() == 8);
    float sum = 0.0f;
    for (const ExtractedColor& c : palette) {
        sum += c.share;
    }
    CHECK(sum == doctest::Approx(1.0f).epsilon(0.001));
    char line[96];
    std::snprintf(line, sizeof(line), "T-309 extractPalette 1920x1080: %.1f ms", ms);
    MESSAGE(line);
    std::printf("%s\n", line);
}

TEST_CASE("T-309 (d): deriveScale, contrastRatio, deltaE2000, normalizeHex") {
    const std::vector<std::string> scale = deriveScale("#0f7b8a", 9);
    REQUIRE(scale.size() == 9);
    double previous = 2.0;
    for (const std::string& hex : scale) {
        float rgba[4];
        REQUIRE(bundle::detail::parseHexColor(hex, rgba));
        const double l = bundle::detail::srgbToOklab(rgba[0], rgba[1], rgba[2]).l;
        CHECK(l < previous);  // monotonic lightness, lightest first
        previous = l;
    }
    CHECK(deriveScale("#0f7b8a", 1) == std::vector<std::string>{"#0f7b8a"});
    CHECK(deriveScale("#0f7b8a", 5, "hsl").empty());
    CHECK(deriveScale("teal", 5).empty());
    CHECK(deriveScale("#0f7b8a", 0).empty());

    CHECK(contrastRatio("#000000", "#ffffff") == doctest::Approx(21.0f).epsilon(0.001));
    CHECK(contrastRatio("#ffffff", "#000") == doctest::Approx(21.0f).epsilon(0.001));
    CHECK(contrastRatio("#777777", "#777777") == doctest::Approx(1.0f));
    CHECK(contrastRatio("#767676", "#ffffff") == doctest::Approx(4.54f).epsilon(0.01));
    CHECK(contrastRatio("nope", "#ffffff") == 0.0f);

    CHECK(deltaE2000("#ff0000", "#ff0000") == doctest::Approx(0.0f));
    double expected = 0.0;
    REQUIRE(bundle::detail::hexDeltaE2000("#0f7b8a", "#12909f", &expected));
    CHECK(deltaE2000("#0f7b8a", "#12909f") == doctest::Approx(static_cast<float>(expected)));
    CHECK(deltaE2000("#0f7b8a", "x") < 0.0f);

    CHECK(normalizeHex("#ABC") == "#aabbcc");
    CHECK(normalizeHex("#0b0e1340") == "#0b0e1340");
    CHECK(normalizeHex("#0B0E13FF") == "#0b0e13");
    CHECK(normalizeHex("0b0e13").empty());
}

TEST_CASE("T-309 (e): audit of Examples/demos/game-ui finds literal colours with the nearest swatch") {
    std::string error;
    Result<UiProject> game = UiProject::load(sourceDir("Examples/demos/game-ui"), &error);
    REQUIRE_MESSAGE(game.ok(), error);
    DesignSystem ds;
    for (const auto& [id, value] : std::vector<std::pair<std::string, std::string>>{
             {"danger", "#f85149"}, {"accent", "#1f6feb"}, {"text", "#e6edf3"}, {"night", "#0b0f17"}}) {
        Swatch s;
        s.base.kind = EntryKind::Swatch;
        s.base.id = id;
        s.value = value;
        ds.swatches.push_back(s);
    }
    AuditReport report;
    REQUIRE(audit(game.value(), ds, "uis", report) == Status::kOk);
    const AuditReport::Literal* danger = literal(report.colors, "#f85149");
    REQUIRE(danger != nullptr);
    CHECK(danger->count >= 5);
    CHECK(danger->nearestSwatch == "danger");
    CHECK(danger->deltaE == doctest::Approx(0.0f));
    const AuditReport::Literal* blue = literal(report.colors, "#58a6ff");
    REQUIRE(blue != nullptr);
    CHECK(blue->nearestSwatch == "accent");
    CHECK(blue->deltaE > 2.0f);
    bool layerWhere = false;
    for (const AuditReport::Literal& l : report.colors) {
        for (const std::string& w : l.where) {
            layerWhere = layerWhere || w.rfind("hud#", 0) == 0;
        }
    }
    CHECK(layerWhere);
    CHECK(literal(report.colors, "#161b22e6") != nullptr);  // alpha kept
    CHECK(literal(report.sizes, "16px") != nullptr);
    REQUIRE_FALSE(report.fonts.empty());
    CHECK(report.fonts[0].value.find("Segoe UI") != std::string::npos);
    CHECK(report.tokenCoverage >= 0.0f);
    CHECK(report.tokenCoverage <= 1.0f);
    for (size_t i = 1; i < report.colors.size(); ++i) {
        CHECK(report.colors[i - 1].count >= report.colors[i].count);
    }
    CHECK(audit(game.value(), ds, "nope", report) == Status::kInvalidArgument);

    // The starter draws through variables: high coverage, symbol scope included.
    const UiProject starter = starterCopy("doc_audit");
    Loaded l = loadDesign(starter);
    AuditReport all;
    REQUIRE(audit(starter, l.ds, "all", all) == Status::kOk);
    AuditReport uis;
    REQUIRE(audit(starter, l.ds, "uis", uis) == Status::kOk);
    CHECK(all.tokenCoverage > 0.5f);
    CHECK(all.colors.size() >= uis.colors.size());
}

TEST_CASE("T-309 (f): lintDocument: literalColorNotInPalette, contrastLow, draftShipped, docStale") {
    // design.literalColorNotInPalette on game-ui against a two-swatch palette.
    {
        std::string error;
        Result<UiProject> game = UiProject::load(sourceDir("Examples/demos/game-ui"), &error);
        REQUIRE_MESSAGE(game.ok(), error);
        DesignSystem ds;
        Swatch s;
        s.base.kind = EntryKind::Swatch;
        s.base.id = "danger";
        s.value = "#f85149";
        ds.swatches.push_back(s);
        Graph graph;
        REQUIRE(buildGraph(game.value(), ds, graph, &error) == Status::kOk);
        const std::vector<Issue> issues = lintDocument(game.value(), ds, graph);
        bool found = false;
        for (const Issue& issue : issues) {
            if (issue.id == "design.literalColorNotInPalette") {
                CHECK(issue.severity == Severity::Info);
                CHECK(issue.message.find("#f85149") != 0);  // the palette colour itself is not reported
                found = found || issue.message.rfind("#58a6ff", 0) == 0;
            }
        }
        CHECK(found);
    }

    const UiProject project = starterCopy("doc_lint");
    Loaded l = loadDesign(project);
    // Missing DESIGN.md -> docStale (the starter ships one; remove it).
    std::error_code removed;
    fs::remove(designDir(project) + "/DESIGN.md", removed);
    CHECK(hasIssue(lintDocument(project, l.ds, l.graph), "design.docStale", Severity::Info));
    REQUIRE(writeDocument(project, l.ds, l.graph, BuildOptions(), nullptr) == Status::kOk);
    std::vector<Issue> clean = lintDocument(project, l.ds, l.graph);
    CHECK_FALSE(hasIssue(clean, "design.docStale", Severity::Info));
    CHECK_FALSE(hasIssue(clean, "design.draftShipped", Severity::Warning));
    CHECK_FALSE(hasIssue(clean, "design.contrastLow", Severity::Warning));
    CHECK_FALSE(hasIssue(clean, "design.literalColorNotInPalette", Severity::Info));

    // design.contrastLow: slate-250 on white, and a passing pair.
    l.ds.paletteExtra["contrast"] =
        json::array({json{{"fg", "swatch:slate-250"}, {"bg", "swatch:white"}, {"min", 4.5}},
                     json{{"fg", "token:--color-text"}, {"bg", "token:--color-surface"}, {"min", 4.5}}});
    // design.draftShipped: one variable back to draft.
    for (TokenMeta& t : l.ds.tokens) {
        if (t.base.id == "space-2") {
            t.base.status = EntryStatus::Draft;
        }
    }
    const std::vector<Issue> issues = lintDocument(project, l.ds, l.graph);
    const Issue* contrast = findIssue(issues, "design.contrastLow", "swatch:slate-250");
    REQUIRE(contrast != nullptr);
    CHECK(contrast->severity == Severity::Warning);
    CHECK(findIssue(issues, "design.contrastLow", "token:color-text") == nullptr);
    const Issue* draft = findIssue(issues, "design.draftShipped", "token:space-2");
    REQUIRE(draft != nullptr);
    CHECK(draft->severity == Severity::Warning);
    // The sources changed after the write -> docStale.
    CHECK(hasIssue(issues, "design.docStale", Severity::Info));
    // The new contrast table is in the document.
    std::string md;
    REQUIRE(buildDocument(project, l.ds, l.graph, BuildOptions(), md, nullptr) == Status::kOk);
    CHECK(md.find("| Foreground | Background | Ratio | Minimum | Result |") != std::string::npos);
    CHECK(md.find("| `token:--color-text` | `token:--color-surface` |") != std::string::npos);
}

TEST_CASE("T-309 (g): game-ui after upgradeDesign: DESIGN.md line count") {
    const std::string dir = freshDir("doc_game");
    std::error_code ec;
    fs::copy(sourceDir("Examples/demos/game-ui"), dir, fs::copy_options::recursive, ec);
    REQUIRE(ec.value() == 0);
    std::string error;
    Result<UiProject> game = UiProject::load(dir, &error);
    REQUIRE_MESSAGE(game.ok(), error);
    REQUIRE_MESSAGE(upgradeDesign(game.value(), nullptr, &error) == Status::kOk, error);
    Loaded l = loadDesign(game.value());
    BuildOptions options;
    options.standardSymbolDir = sourceDir("Assets/Symbols");
    REQUIRE(writeDocument(game.value(), l.ds, l.graph, options, nullptr, &error) == Status::kOk);
    const std::string md = readText(designDir(game.value()) + "/DESIGN.md");
    CHECK(md.find("## 13 · Assets") != std::string::npos);
    BuildOptions ai = options;
    ai.forAi = true;
    std::string aiMd;
    REQUIRE(buildDocument(game.value(), l.ds, l.graph, ai, aiMd, nullptr) == Status::kOk);
    char line[160];
    std::snprintf(line, sizeof(line),
                  "T-309 DESIGN.md: game-ui %zu lines (%zu bytes); for-ai %zu tokens (est.)", lines(md),
                  md.size(), (aiMd.size() + 3) / 4);
    MESSAGE(line);
    std::printf("%s\n", line);
}

TEST_CASE("T-309 (h): the starter ships its DESIGN.md, byte-identical to a fresh build") {
    // As `xtframe-design build` and xtframe-pack see it: standard symbols pulled, the standard
    // library next to the tool (a copy of Assets/Symbols).
    const UiProject project = starterCopy("doc_shipped");
    Loaded l = loadDesign(project);
    BuildOptions options;
    options.standardSymbolDir = sourceDir("Assets/Symbols");
    std::string built;
    REQUIRE(buildDocument(project, l.ds, l.graph, options, built, nullptr) == Status::kOk);
    const std::string shipped = readText(sourceDir("Examples/ui-project-starter/design/DESIGN.md"));
    REQUIRE_FALSE(shipped.empty());
    CHECK(built == shipped);
    CHECK_FALSE(hasIssue(lintDocument(project, l.ds, l.graph, options), "design.docStale", Severity::Info));
}
