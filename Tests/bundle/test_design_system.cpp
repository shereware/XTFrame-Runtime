#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "color_math.h"
#include "doctest.h"
#include "test_temp.h"
#include "xt/bundle/design_system.h"
#include "xt/bundle/ui_project.h"
#include "yaml_lite.h"

/// The design system model (Docs/design/35-design-system.md §35.2-§35.6, §35.10, §35.11;
/// Docs/tasks/T-305.md): design/ save -> load -> save byte stability, front matter, entry
/// references, validation, the starter's reference graph, the semantic diff, the upgrade, the
/// variable metadata sync and CIEDE2000.

using namespace xt;
using namespace xt::bundle;
using namespace xt::bundle::design;
using nlohmann::json;

namespace {

namespace fs = std::filesystem;

std::string sourceDir(const char* relative) { return std::string(XT_TEST_BUNDLE_DIR) + "/../../" + relative; }

/// A fresh temp directory `xt_design_<name>` (removed first).
std::string freshDir(const std::string& name) {
    const fs::path dir = xt::test::tempDir() / ("xt_design_" + name);
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
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    out << text;
}

/// Every file under `dir`: relative path -> bytes.
std::map<std::string, std::string> snapshot(const std::string& dir) {
    std::map<std::string, std::string> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            out[fs::relative(it->path(), dir).generic_string()] = readText(it->path().string());
        }
    }
    return out;
}

/// An in-memory UI Project rooted at `dir` (no UIs unless added).
UiProject projectAt(const std::string& dir) {
    UiProject p;
    p.name = "Design Test";
    p.id = "design-test";
    p.rootDir = dir;
    return p;
}

bool hasIssue(const std::vector<Issue>& issues, const std::string& id, const std::string& entry,
              Severity severity) {
    for (const Issue& issue : issues) {
        if (issue.id == id && issue.entry == entry && issue.severity == severity) {
            return true;
        }
    }
    return false;
}

bool contains(const std::vector<std::string>& list, const std::string& value) {
    for (const std::string& item : list) {
        if (item == value) {
            return true;
        }
    }
    return false;
}

Entry entry(EntryKind kind, const std::string& id) {
    Entry e;
    e.kind = kind;
    e.id = id;
    return e;
}

/// A design system with every kind of entry and a few unknown keys.
DesignSystem sampleSystem(const std::string& designDir) {
    DesignSystem ds;
    ds.rootDir = designDir;
    ds.info.name = "Sample";
    ds.info.status = "draft";
    ds.info.principles = {"Readable at a glance", "Calm, then loud"};
    ds.info.languages = {"en", "zh-TW"};
    ds.info.extra = json{{"version", "0.1"}};

    Group brand;
    brand.base = entry(EntryKind::Group, "brand");
    brand.name = "Brand";
    brand.base.name = "Brand";
    brand.swatches = {"ember", "ember-deep", "plain"};
    ds.groups.push_back(brand);
    Group neutral;
    neutral.base = entry(EntryKind::Group, "neutral");
    neutral.name = "Neutrals";
    neutral.scale = GroupScale{"slate", 9, "oklch-lightness"};
    ds.groups.push_back(neutral);

    Swatch ember;
    ember.base = entry(EntryKind::Swatch, "ember");
    ember.base.name = "Ember";
    ember.base.status = EntryStatus::Approved;
    ember.base.source = Source{"extracted:moodboard/key-art-01", "", "2026-10-11"};
    ember.base.approved = Approval{"lead", "2026-10-11"};
    ember.base.tags = {"fire", "primary"};
    ember.base.links = {Link{"derived-from", "ref:moodboard/key-art-01", "logo flame"}};
    ember.base.note = "Interaction and progress; not for large areas";
    ember.value = "#ff5a1f";
    ember.role = "primary";
    ember.group = "brand";
    ds.swatches.push_back(ember);
    Swatch deep;
    deep.base = entry(EntryKind::Swatch, "ember-deep");
    deep.value = "#b83a0f";
    deep.role = "pressed";
    deep.group = "brand";
    deep.scale = SwatchScale{"ember", 7, ""};
    ds.swatches.push_back(deep);
    Swatch plain;
    plain.base = entry(EntryKind::Swatch, "plain");
    plain.group = "brand";
    ds.swatches.push_back(plain);
    Swatch loose;
    loose.base = entry(EntryKind::Swatch, "loose");
    loose.value = "#123456";
    ds.swatches.push_back(loose);
    ds.paletteExtra = json{
        {"version", 1},
        {"contrast",
         json::array({json{{"fg", "token:--color-text"}, {"bg", "token:--color-surface"}, {"min", 4.5}}})}};

    TokenMeta accent;
    accent.base = entry(EntryKind::Token, "color-accent");
    accent.base.status = EntryStatus::Approved;
    accent.base.extra = json{{"fromValue", "#ff5a1f"}};
    accent.category = "color";
    accent.description = "Main interactive colour: buttons, focus";
    accent.from = "swatch:ember";
    ds.tokens.push_back(accent);
    TokenMeta space;
    space.base = entry(EntryKind::Token, "space-2");
    space.category = "space";
    space.from = "decision:0002";
    ds.tokens.push_back(space);

    TypeRole body;
    body.base = entry(EntryKind::TypeRole, "body");
    body.family = "Noto Sans";
    body.weights = {"400", "700"};
    ds.typeRoles.push_back(body);
    ds.typeExtra = json{{"scale", json{{"m", "--font-size-m"}, {"l", "--font-size-l"}}}};

    TextEntry voice;
    voice.base = entry(EntryKind::Text, "voice");
    voice.path = "text/voice.md";
    voice.title = "Voice";
    voice.sections = {"voice"};
    voice.base.links = {Link{"applies-to", "symbol:Button", ""}, Link{"see-also", "text:principles", "why"}};
    voice.body = "# Voice\n\n## Numbers\n\nUse digits.\n";
    ds.texts.push_back(voice);
    TextEntry screen;
    screen.base = entry(EntryKind::Text, "screens/hud");
    screen.path = "text/screens/hud.md";
    screen.title = "HUD: overview";
    screen.sections = {"screens", "layout"};
    screen.body = "# HUD\n\nHealth top-left.\n";
    ds.texts.push_back(screen);
    TextEntry decision;
    decision.base = entry(EntryKind::Decision, "0002");
    decision.base.extra = json{{"status", "accepted"}};
    decision.base.links = {Link{"supersedes", "decision:0001", ""}};
    decision.path = "text/decisions/0002-spacing.md";
    decision.title = "Spacing scale";
    decision.body = "## Context\n\nToo many gaps.\n\n## Decision\n\nA 4 px scale.\n";
    ds.decisions.push_back(decision);

    ReferenceMeta ref;
    ref.base = entry(EntryKind::Reference, "moodboard/key-art-01");
    ref.base.source.kind = "manual";
    ref.base.extra = json{{"source", json{{"origin", "https://example.com/key-art"}}}};
    ref.base.tags = {"moodboard"};
    ref.base.links = {Link{"illustrates", "ui:hud", ""}};
    ref.path = "references/moodboard/key-art-01.png";
    ref.guid = "8f3a0000-0000-4000-8000-000000000001";
    ref.title = "Key art 01";
    ref.license = "own";
    ref.contentHash = "sha256:00";
    ReferenceMeta::Annotation a1;
    a1.id = "a1";
    a1.rect = {0.12f, 0.08f, 0.3f, 0.22f};
    a1.note = "flame gradient";
    a1.links = {Link{"evidence-for", "swatch:ember", ""}};
    ref.annotations.push_back(a1);
    ref.extracted.push_back(ReferenceMeta::Extracted{"#ff5a1f", 0.18f});
    ds.references.push_back(ref);
    return ds;
}

const Swatch* findSwatch(const DesignSystem& ds, const std::string& id) {
    for (const Swatch& s : ds.swatches) {
        if (s.base.id == id) {
            return &s;
        }
    }
    return nullptr;
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

double sinceMs(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace

TEST_CASE("T-305 (a): design/ save -> load -> save is byte-identical and keeps unknown keys") {
    const std::string dir = freshDir("roundtrip");
    const DesignSystem ds = sampleSystem(dir + "/design");
    std::string error;
    REQUIRE_MESSAGE(save(ds, &error) == Status::kOk, error);
    const auto first = snapshot(dir + "/design");
    CHECK(first.count("design.yaml") == 1);
    CHECK(first.count("palette.yaml") == 1);
    CHECK(first.count("tokens.yaml") == 1);
    CHECK(first.count("type.yaml") == 1);
    CHECK(first.count("text/voice.md") == 1);
    CHECK(first.count("text/decisions/0002-spacing.md") == 1);
    CHECK(first.count("references/moodboard/key-art-01.png.meta") == 1);
    CHECK(first.at("tokens.yaml").find("  --color-accent:\n") != std::string::npos);
    CHECK(first.at("palette.yaml").find("      - plain\n") != std::string::npos);  // the abbreviated form

    DesignSystem loaded;
    REQUIRE_MESSAGE(load(projectAt(dir), loaded, nullptr, &error) == Status::kOk, error);
    REQUIRE(save(loaded, &error) == Status::kOk);
    CHECK(snapshot(dir + "/design") == first);

    // What came back.
    CHECK(loaded.info.extra["version"] == "0.1");
    REQUIRE(loaded.groups.size() == 2);
    CHECK(loaded.groups[0].swatches == std::vector<std::string>{"ember", "ember-deep", "plain"});
    CHECK(loaded.groups[1].scale.steps == 9);
    const Swatch* ember = findSwatch(loaded, "ember");
    REQUIRE(ember != nullptr);
    CHECK(ember->base.status == EntryStatus::Approved);
    CHECK(ember->base.approved.by == "lead");
    CHECK(ember->base.source.kind == "extracted:moodboard/key-art-01");
    CHECK(ember->group == "brand");
    REQUIRE(findSwatch(loaded, "ember-deep") != nullptr);
    CHECK(findSwatch(loaded, "ember-deep")->scale.step == 7);
    REQUIRE(findSwatch(loaded, "loose") != nullptr);
    CHECK(findSwatch(loaded, "loose")->group.empty());
    CHECK(loaded.paletteExtra["contrast"][0]["min"] == 4.5);
    REQUIRE(loaded.tokens.size() == 2);
    CHECK(loaded.tokens[0].base.id == "color-accent");
    CHECK(loaded.tokens[0].base.extra["fromValue"] == "#ff5a1f");
    CHECK(loaded.typeRoles[0].weights == std::vector<std::string>{"400", "700"});
    CHECK(loaded.typeExtra["scale"]["m"] == "--font-size-m");
    REQUIRE(loaded.decisions.size() == 1);
    CHECK(loaded.decisions[0].base.id == "0002");
    CHECK(loaded.decisions[0].base.extra["status"] == "accepted");
    REQUIRE(loaded.references.size() == 1);
    const ReferenceMeta& ref = loaded.references[0];
    CHECK(ref.base.id == "moodboard/key-art-01");
    CHECK(ref.path == "references/moodboard/key-art-01.png");
    CHECK(ref.guid == "8f3a0000-0000-4000-8000-000000000001");
    REQUIRE(ref.annotations.size() == 1);
    CHECK(ref.annotations[0].rect[0] == 0.12f);
    CHECK(ref.extracted[0].share == 0.18f);
    CHECK(ref.base.extra["source"]["origin"] == "https://example.com/key-art");

    // A hand-written unknown key survives a save, after the known ones.
    writeText(dir + "/design/type.yaml",
              "owner: art team\nroles:\n  - id: body\n    family: Inter\n    weights: [400]\n");
    REQUIRE(load(projectAt(dir), loaded, nullptr, &error) == Status::kOk);
    REQUIRE(save(loaded, &error) == Status::kOk);
    CHECK(readText(dir + "/design/type.yaml") ==
          "version: 1\nroles:\n  - id: body\n    family: Inter\n    weights: [400]\nowner: art team\n");

    // Missing design/ is kNotFound; a malformed file names itself.
    CHECK(load(projectAt(freshDir("roundtrip_none")), loaded, nullptr, &error) == Status::kNotFound);
    writeText(dir + "/design/palette.yaml", "groups:\n\t- id: x\n");
    CHECK(load(projectAt(dir), loaded, nullptr, &error) == Status::kInvalidArgument);
    CHECK(error.find("palette.yaml") != std::string::npos);
}

TEST_CASE("T-305 (a): emitYamlLite reads back to the same value") {
    nlohmann::ordered_json doc = nlohmann::ordered_json::object();
    doc["name"] = "a: b";
    doc["list"] = {"x", "y, z", "#hex", "100"};
    doc["empty"] = nlohmann::ordered_json::array();
    doc["map"] = {{"--color-accent", {{"from", "swatch:ember"}}}};
    doc["items"] =
        nlohmann::ordered_json::array({nlohmann::ordered_json{{"id", "a"}, {"rect", {0.5, 1}}}, "bare"});
    const std::string text = xt::bundle::detail::emitYamlLite(doc);
    CHECK(text.find("map:\n  --color-accent:\n    from: swatch:ember\n") != std::string::npos);
    CHECK(text.find("items:\n  - id: a\n    rect: [0.5, 1]\n  - bare\n") != std::string::npos);
    json back;
    std::string error;
    REQUIRE_MESSAGE(xt::bundle::detail::parseYamlLite(text, back, &error), error);
    CHECK(back["name"] == "a: b");
    CHECK(back["list"] == json::array({"x", "y, z", "#hex", "100"}));
    CHECK(back["empty"] == json::array());
    CHECK(back["map"]["--color-accent"]["from"] == "swatch:ember");
    CHECK(back["items"][1] == "bare");
}

TEST_CASE("T-305 (b): text front matter (quotes, lists, several sections) and the body byte for byte") {
    const std::string dir = freshDir("frontmatter");
    const std::string body = "# Menu\n\nBody line with --- inside.\n---\nTrailing text.\n";
    writeText(dir + "/design/text/screens/menu.md",
              "---\n"
              "id: \"screens/menu\"\n"
              "title: 'Menu: \"main\" screen'\n"
              "section:\n"
              "  - screens\n"
              "  - layout\n"
              "status: approved\n"
              "tags: [menu, \"a, b\"]\n"
              "applies-to: [symbol:Button, convention:naming]\n"
              "links:\n"
              "  - { rel: see-also, to: \"text:voice#numbers\", note: digits }\n"
              "custom: kept\n"
              "---\n" +
                  body);
    const std::string plainBody = "# Plain\r\n\r\nNo front matter.\r\n";
    writeText(dir + "/design/text/plain.md", plainBody);

    DesignSystem ds;
    std::string error;
    REQUIRE_MESSAGE(load(projectAt(dir), ds, nullptr, &error) == Status::kOk, error);
    REQUIRE(ds.texts.size() == 2);
    const TextEntry& plain = ds.texts[0];
    const TextEntry& menu = ds.texts[1];
    CHECK(plain.base.id == "plain");
    CHECK(plain.body == plainBody);
    CHECK(menu.base.id == "screens/menu");
    CHECK(menu.path == "text/screens/menu.md");
    CHECK(menu.title == "Menu: \"main\" screen");
    CHECK(menu.sections == std::vector<std::string>{"screens", "layout"});
    CHECK(menu.base.status == EntryStatus::Approved);
    CHECK(menu.base.tags == std::vector<std::string>{"menu", "a, b"});
    REQUIRE(menu.base.links.size() == 3);
    CHECK(menu.base.links[0].rel == "see-also");
    CHECK(menu.base.links[0].to == "text:voice#numbers");
    CHECK(menu.base.links[0].note == "digits");
    CHECK(menu.base.links[1].rel == "applies-to");
    CHECK(menu.base.links[1].to == "symbol:Button");
    CHECK(menu.base.links[2].to == "convention:naming");
    CHECK(menu.base.extra["custom"] == "kept");
    CHECK(menu.body == body);

    REQUIRE(save(ds, &error) == Status::kOk);
    const std::string saved = readText(dir + "/design/text/screens/menu.md");
    CHECK(saved.rfind("---\nid: screens/menu\ntitle: ", 0) == 0);
    CHECK(saved.find("section: [screens, layout]\n") != std::string::npos);
    CHECK(saved.find("applies-to: [symbol:Button, convention:naming]\n") != std::string::npos);
    CHECK(saved.find("custom: kept\n---\n") != std::string::npos);
    CHECK(saved.substr(saved.size() - body.size()) == body);
    CHECK(readText(dir + "/design/text/plain.md") == "---\nid: plain\n---\n" + plainBody);

    DesignSystem again;
    REQUIRE(load(projectAt(dir), again, nullptr, &error) == Status::kOk);
    CHECK(again.texts[1].body == body);
    CHECK(again.texts[0].body == plainBody);
    REQUIRE(save(again, &error) == Status::kOk);
    CHECK(readText(dir + "/design/text/screens/menu.md") == saved);
}

TEST_CASE("T-305 (c): entry references, kinds and rels") {
    struct Good {
        const char* ref;
        EntryKind kind;
        const char* id;
    };
    const Good good[] = {
        {"swatch:ember", EntryKind::Swatch, "ember"},
        {"group:brand", EntryKind::Group, "brand"},
        {"ref:moodboard/key-art-01", EntryKind::Reference, "moodboard/key-art-01"},
        {"ref:moodboard/key-art-01@a1", EntryKind::Reference, "moodboard/key-art-01@a1"},
        {"ref:a", EntryKind::Reference, "a"},
        {"text:voice", EntryKind::Text, "voice"},
        {"text:voice#numbers", EntryKind::Text, "voice#numbers"},
        {"text:screens/hud", EntryKind::Text, "screens/hud"},
        {"decision:0007", EntryKind::Decision, "0007"},
        {"token:color-accent", EntryKind::Token, "color-accent"},
        {"token:--color-accent", EntryKind::Token, "color-accent"},
        {"type:display", EntryKind::TypeRole, "display"},
        {"type:scale#m", EntryKind::TypeRole, "scale#m"},
        {"convention:naming", EntryKind::Convention, "naming"},
        {"motion:duration.fast", EntryKind::Motion, "duration.fast"},
        {"symbol:Button", EntryKind::Symbol, "Button"},
        {"ui:hud", EntryKind::Ui, "hud"},
        {"layer:hud#HealthBar", EntryKind::Layer, "hud#HealthBar"},
    };
    for (const Good& g : good) {
        EntryKind kind = EntryKind::Ui;
        std::string id;
        INFO(g.ref);
        REQUIRE(parseEntryRef(g.ref, &kind, &id));
        CHECK(kind == g.kind);
        CHECK(id == g.id);
    }
    const char* bad[] = {
        "",           "ember",       ":ember",   "colour:red", "swatch:",   "swatch:a b",  "swatch:\tx",
        "swatch:a#b", "ref:a@",      "ref:@a",   "ref:a@b@c",  "ref:a@b#c", "text:voice#", "text:#x",
        "text:a#b#c", "layer:hud",   "layer:#x", "token:--",   "ui:hud@x",  "ref:a/",      "ref:/a",
        "ref:a//b",   "decision:1#x"};
    for (const char* b : bad) {
        INFO(b);
        CHECK_FALSE(parseEntryRef(b, nullptr, nullptr));
    }
    CHECK(entryRef(EntryKind::Swatch, "ember") == "swatch:ember");
    CHECK(entryRef(EntryKind::Reference, "a/b") == "ref:a/b");
    CHECK(kindPrefix(EntryKind::TypeRole) == "type");
    CHECK(kindPrefix(EntryKind::Layer) == "layer");
    CHECK(kRels.size() == 7);
    CHECK(relName(Rel::DerivedFrom) == "derived-from");
    CHECK(relName(Rel::SeeAlso) == "see-also");
    Rel rel = Rel::SeeAlso;
    CHECK(parseRel("conflicts-with", &rel));
    CHECK(rel == Rel::ConflictsWith);
    CHECK_FALSE(parseRel("likes"));
}

TEST_CASE("T-305 (d): validate reports every design.* issue once per case") {
    const std::string dir = freshDir("validate");
    writeText(dir + "/design/references/shots/a.png", "AAAA");
    writeText(dir + "/design/references/shots/b.png", "BBBB");
    DesignSystem ds;
    ds.rootDir = dir + "/design";

    ReferenceMeta a;
    a.base = entry(EntryKind::Reference, "shots/a");
    a.path = "references/shots/a.png";
    a.license = "unknown";
    a.contentHash = "sha256:0000";  // not what a.png holds now
    ds.references.push_back(a);
    ReferenceMeta b;
    b.base = entry(EntryKind::Reference, "shots/b");
    b.path = "references/shots/b.png";
    b.license = "own";
    ds.references.push_back(b);

    Swatch s1;
    s1.base = entry(EntryKind::Swatch, "s1");
    s1.value = "#ff0000";
    s1.base.links = {Link{"derived-from", "ref:shots/a", ""}};
    ds.swatches.push_back(s1);
    Swatch s2;
    s2.base = entry(EntryKind::Swatch, "s2");
    s2.value = "#fe0101";
    ds.swatches.push_back(s2);
    Swatch s3;
    s3.base = entry(EntryKind::Swatch, "s3");
    s3.value = "#00ff00";
    s3.base.links = {Link{"likes", "swatch:s1", ""}};
    ds.swatches.push_back(s3);
    Swatch s4;
    s4.base = entry(EntryKind::Swatch, "s4");
    s4.value = "#0000ff";
    s4.base.links = {Link{"see-also", "swatch:nope", ""}, Link{"see-also", "not a ref", ""}};
    ds.swatches.push_back(s4);
    TokenMeta t;
    t.base = entry(EntryKind::Token, "color-a");
    t.base.extra = json{{"fromValue", "#00ff00"}};
    t.from = "swatch:s1";
    ds.tokens.push_back(t);
    TokenMeta described;
    described.base = entry(EntryKind::Token, "color-b");
    described.description = "Described.";
    described.from = "swatch:s3";
    ds.tokens.push_back(described);

    Graph graph;
    std::string error;
    REQUIRE_MESSAGE(buildGraph(projectAt(dir), ds, graph, &error) == Status::kOk, error);
    const std::vector<Issue> issues = validate(ds, graph);
    for (const Issue& issue : issues) {
        MESSAGE((issue.id + " " + issue.entry + ": " + issue.message));
    }
    CHECK(hasIssue(issues, "design.brokenLink", "swatch:s4", Severity::Error));
    CHECK(hasIssue(issues, "design.invalidRel", "swatch:s3", Severity::Error));
    CHECK(hasIssue(issues, "design.tokenNoDescription", "token:color-a", Severity::Info));
    CHECK_FALSE(hasIssue(issues, "design.tokenNoDescription", "token:color-b", Severity::Info));
    CHECK(hasIssue(issues, "design.nearDuplicateSwatch", "swatch:s1", Severity::Info));
    CHECK(hasIssue(issues, "design.stale", "swatch:s1", Severity::Info));      // reference content changed
    CHECK(hasIssue(issues, "design.stale", "token:color-a", Severity::Info));  // from swatch value changed
    CHECK(hasIssue(issues, "design.referenceNoLicense", "ref:shots/a", Severity::Info));
    CHECK_FALSE(hasIssue(issues, "design.referenceNoLicense", "ref:shots/b", Severity::Info));
    CHECK(hasIssue(issues, "design.orphan", "ref:shots/b", Severity::Info));
    CHECK(hasIssue(issues, "design.orphan", "swatch:s2", Severity::Info));
    CHECK_FALSE(hasIssue(issues, "design.orphan", "swatch:s1", Severity::Info));
    CHECK_FALSE(hasIssue(issues, "design.orphan", "ref:shots/a", Severity::Info));
    int broken = 0;
    for (const Issue& issue : issues) {
        broken += issue.id == "design.brokenLink" ? 1 : 0;
    }
    CHECK(broken == 2);
}

TEST_CASE("T-305 (e): the starter's graph: --color-accent is used by UIs and symbols; a swatch reaches UIs") {
    const UiProject project = starterCopy("starter_graph");
    DesignSystem ds;
    std::vector<Issue> issues;
    std::string error;
    REQUIRE_MESSAGE(load(project, ds, &issues, &error) == Status::kOk, error);
    CHECK(ds.info.name == "Starter");
    CHECK(ds.info.principles.size() == 3);
    CHECK(ds.groups.size() == 3);

    auto start = std::chrono::steady_clock::now();
    Graph graph;
    REQUIRE_MESSAGE(buildGraph(project, ds, graph, &error) == Status::kOk, error);
    const double starterMs = sinceMs(start);

    const Usage accent = usage(graph, "token:color-accent");
    CHECK_FALSE(accent.uis.empty());
    CHECK_FALSE(accent.symbols.empty());
    CHECK(contains(accent.symbols, "symbol:Button"));
    CHECK(contains(accent.uis, "ui:main-menu"));
    CHECK(usage(graph, "token:--color-accent").uis == accent.uis);  // the `--` spelling too
    CHECK(contains(usage(graph, "swatch:ink").entries, "token:color-accent"));
    const Usage teal = impact(graph, "swatch:ink");
    CHECK(contains(teal.entries, "token:color-accent"));
    CHECK(contains(teal.uis, "ui:main-menu"));
    CHECK(contains(teal.symbols, "symbol:Button"));
    // A layer use: hud.xts' `#TopLeft { ... var(--safe-x) ... }`.
    CHECK(contains(usage(graph, "token:safe-x").layers, "layer:hud#TopLeft"));
    CHECK(graph.has("convention:hud"));
    CHECK(graph.has("motion:duration.fast"));
    CHECK(graph.has("text:principles#calm-by-default"));
    // The starter's own design data has no broken link and every variable has metadata.
    for (const Issue& issue : validate(ds, graph)) {
        CHECK_MESSAGE(issue.severity != Severity::Error,
                      (issue.id + " " + issue.entry + ": " + issue.message));
    }
    std::vector<Issue> sync;
    syncTokenMeta(ds, project, &sync);
    CHECK(sync.empty());

    // Timing on Examples/demos/game-ui (read in place: buildGraph writes nothing).
    Result<UiProject> game = UiProject::load(sourceDir("Examples/demos/game-ui"), &error);
    REQUIRE_MESSAGE(game.ok(), error);
    DesignSystem none;
    CHECK(load(game.value(), none) == Status::kNotFound);
    start = std::chrono::steady_clock::now();
    Graph gameGraph;
    REQUIRE(buildGraph(game.value(), none, gameGraph, &error) == Status::kOk);
    const double gameMs = sinceMs(start);
    char line[160];
    std::snprintf(
        line, sizeof(line),
        "T-305 buildGraph: starter %.1f ms (%zu nodes, %zu edges); game-ui %.1f ms (%zu nodes, %zu edges)",
        starterMs, graph.nodes.size(), graph.edges.size(), gameMs, gameGraph.nodes.size(),
        gameGraph.edges.size());
    MESSAGE(line);
    std::printf("%s\n", line);
}

TEST_CASE(
    "T-305 (f): diff: swatch value with ΔE, variable description with UIs, paragraphs, reference, links") {
    const UiProject project = starterCopy("starter_diff");
    DesignSystem a;
    std::string error;
    REQUIRE_MESSAGE(load(project, a, nullptr, &error) == Status::kOk, error);
    ReferenceMeta ref;
    ref.base = entry(EntryKind::Reference, "brand/logo");
    ref.path = "references/brand/logo.png";
    ref.contentHash = "sha256:aaaa";
    a.references.push_back(ref);

    DesignSystem b = a;
    for (Swatch& s : b.swatches) {
        if (s.base.id == "ink") {
            s.value = "#3a322b";
            s.base.links.push_back(Link{"see-also", "swatch:ochre", ""});
        }
        if (s.base.id == "seal") {
            s.base.links.clear();
        }
    }
    for (Swatch& s : a.swatches) {
        if (s.base.id == "seal") {
            s.base.links.push_back(Link{"see-also", "swatch:lamp", ""});
        }
    }
    for (TokenMeta& t : b.tokens) {
        if (t.base.id == "color-accent") {
            t.description = "The one accent.";
        }
    }
    for (TextEntry& t : b.texts) {
        if (t.base.id == "principles") {
            const std::string from = "Flat newsprint grey carries the layout";
            const size_t at = t.body.find(from);
            REQUIRE(at != std::string::npos);
            t.body.replace(at, from.size(), "Flat newsprint grey holds the layout");
        }
    }
    b.references[0].contentHash = "sha256:bbbb";

    Graph graph;
    REQUIRE(buildGraph(project, b, graph, &error) == Status::kOk);
    const std::vector<DiffItem> items = diff(a, b, &graph);
    bool value = false, description = false, paragraph = false, content = false, added = false,
         removed = false;
    for (const DiffItem& item : items) {
        MESSAGE((item.entry + " " + item.field + ": " + item.before + " -> " + item.after));
        if (item.entry == "swatch:ink" && item.field == "value") {
            value = item.before == "#1f1c18" && item.after == "#3a322b" && item.deltaE > 1.0f &&
                    item.deltaE < 20.0f;
        } else if (item.entry == "token:color-accent" && item.field == "description") {
            description = item.after == "The one accent." && item.affectedUis >= 1;
        } else if (item.entry == "text:principles" && item.field == "paragraph") {
            paragraph = item.op == DiffItem::Op::Changed &&
                        item.before.find("carries the layout") != std::string::npos &&
                        item.after.find("holds the layout") != std::string::npos;
        } else if (item.entry == "ref:brand/logo" && item.field == "content") {
            content = item.before == "sha256:aaaa" && item.after == "sha256:bbbb";
        } else if (item.entry == "swatch:ink" && item.field == "link") {
            added = item.op == DiffItem::Op::Added && item.after == "see-also swatch:ochre";
        } else if (item.entry == "swatch:seal" && item.field == "link") {
            removed = item.op == DiffItem::Op::Removed && item.before == "see-also swatch:lamp";
        }
    }
    CHECK(value);
    CHECK(description);
    CHECK(paragraph);
    CHECK(content);
    CHECK(added);
    CHECK(removed);
    CHECK(items.size() == 6);
    CHECK(diff(b, b).empty());

    // Whole entries added and removed.
    DesignSystem c = b;
    c.swatches.pop_back();
    TokenMeta extra;
    extra.base = entry(EntryKind::Token, "color-new");
    c.tokens.push_back(extra);
    bool swatchGone = false, tokenNew = false;
    for (const DiffItem& item : diff(b, c)) {
        swatchGone = swatchGone || (item.op == DiffItem::Op::Removed && item.field.empty() &&
                                    item.entry == "swatch:" + b.swatches.back().base.id);
        tokenNew = tokenNew || (item.op == DiffItem::Op::Added && item.entry == "token:color-new");
    }
    CHECK(swatchGone);
    CHECK(tokenNew);
}

TEST_CASE(
    "T-305 (g): upgradeDesign moves references/, adds metas, syncs tokens; a second call does nothing") {
    const std::string dir = freshDir("upgrade");
    writeText(dir + "/references/a.png", "PNGDATA");
    writeText(dir + "/references/.gitkeep", "");
    writeText(dir + "/references/sub/b.txt", "notes");
    writeText(dir + "/references/sub/b.txt.meta", "id: kept-guid\nkind: reference\nlicense: own\n");
    writeText(dir + "/shared/tokens.xts", ":root {\n  --color-x: #ff0000;\n  --space-1: 4px;\n}\n");
    UiProject project = projectAt(dir);

    DesignUpgradeReport report;
    std::string error;
    REQUIRE_MESSAGE(upgradeDesign(project, &report, &error) == Status::kOk, error);
    CHECK(report.created);
    CHECK(report.referencesMoved == 2);
    CHECK(report.tokensMetaAdded == 2);
    CHECK_FALSE(fs::exists(dir + "/references"));
    CHECK(fs::is_regular_file(dir + "/design/design.yaml"));
    CHECK(fs::is_regular_file(dir + "/design/text/principles.md"));
    CHECK(fs::is_regular_file(dir + "/design/text/voice.md"));
    CHECK(fs::is_directory(dir + "/design/references"));
    CHECK_FALSE(fs::exists(dir + "/design/palette.yaml"));
    CHECK_FALSE(fs::exists(dir + "/design/type.yaml"));
    CHECK(readText(dir + "/design/DESIGN.md") ==
          "<!-- generated by xtframe-design; run xtframe-design build -->\n");
    CHECK(readText(dir + "/design/references/a.png") == "PNGDATA");
    CHECK(readText(dir + "/design/references/sub/b.txt.meta") ==
          "id: kept-guid\nkind: reference\nlicense: own\n");  // its own meta moved, not replaced

    DesignSystem ds;
    REQUIRE(load(project, ds, nullptr, &error) == Status::kOk);
    CHECK(ds.info.name == "Design Test");
    REQUIRE(ds.references.size() == 2);
    const ReferenceMeta& a = ds.references[0];
    CHECK(a.base.id == "a");
    CHECK(a.title == "a.png");
    CHECK(a.license == "unknown");
    CHECK(a.guid.size() == 36);
    CHECK(a.contentHash.rfind("sha256:", 0) == 0);
    CHECK(a.contentHash.size() == 7 + 64);
    CHECK(ds.references[1].base.id == "sub/b");
    REQUIRE(ds.tokens.size() == 2);
    CHECK(ds.tokens[0].base.id == "color-x");
    CHECK(ds.tokens[0].category == "color");
    CHECK(ds.tokens[0].base.status == EntryStatus::Draft);
    CHECK(ds.tokens[1].category == "space");
    CHECK(ds.texts.size() == 2);

    const auto before = snapshot(dir);
    DesignUpgradeReport second;
    REQUIRE(upgradeDesign(project, &second, &error) == Status::kOk);
    CHECK_FALSE(second.created);
    CHECK(second.referencesMoved == 0);
    CHECK(second.tokensMetaAdded == 0);
    CHECK(snapshot(dir) == before);

    // A blank createUiProject gets a design/ too (and AGENTS.md's template points at DESIGN.md).
    const std::string fresh = freshDir("upgrade_create") + "/game";
    REQUIRE_MESSAGE(createUiProject(fresh, "Game", "game", "", nullptr, &error) == Status::kOk, error);
    CHECK(fs::is_regular_file(fresh + "/design/design.yaml"));
    CHECK(std::string(agentsTemplate()).find("Read design/DESIGN.md first when it exists") !=
          std::string::npos);
    CHECK(UiProject::load(fresh).value().shared.design == "design");
}

TEST_CASE("T-305 (h): syncTokenMeta adds drafts for variables without metadata and flags orphans") {
    const std::string dir = freshDir("sync");
    writeText(dir + "/shared/tokens.xts",
              ":root {\n  --color-x: #ff0000;\n  --gap-big: 24px;\n  --focus-w: 2px;\n  --shadow-soft: 0 1px "
              "2px #000;\n"
              "  --wobble: 3;\n}\n");
    DesignSystem ds;
    TokenMeta x;
    x.base = entry(EntryKind::Token, "color-x");
    x.description = "Red.";
    ds.tokens.push_back(x);
    TokenMeta gone;
    gone.base = entry(EntryKind::Token, "gone");
    ds.tokens.push_back(gone);

    std::vector<Issue> issues;
    REQUIRE(syncTokenMeta(ds, projectAt(dir), &issues) == Status::kOk);
    REQUIRE(ds.tokens.size() == 6);
    CHECK(ds.tokens[2].base.id == "gap-big");
    CHECK(ds.tokens[2].category == "space");
    CHECK(ds.tokens[3].category == "border");
    CHECK(ds.tokens[4].category == "shadow");
    CHECK(ds.tokens[5].category == "other");
    CHECK(ds.tokens[5].base.status == EntryStatus::Draft);
    CHECK(hasIssue(issues, "design.tokenNoMeta", "token:gap-big", Severity::Info));
    CHECK(hasIssue(issues, "design.tokenNoMeta", "token:wobble", Severity::Info));
    CHECK_FALSE(hasIssue(issues, "design.tokenNoMeta", "token:color-x", Severity::Info));
    CHECK(hasIssue(issues, "design.orphanMeta", "token:gone", Severity::Info));
    CHECK(issues.size() == 5);
    // The orphan is kept; a second sync adds nothing.
    issues.clear();
    syncTokenMeta(ds, projectAt(dir), &issues);
    CHECK(ds.tokens.size() == 6);
    CHECK(issues.size() == 1);
}

TEST_CASE("T-305 (i): CIEDE2000 known values and hex parsing") {
    using xt::bundle::detail::deltaE2000;
    using xt::bundle::detail::Lab;
    double dE = 0.0;
    REQUIRE(xt::bundle::detail::hexDeltaE2000("#000000", "#ffffff", &dE));
    CHECK(std::fabs(dE - 100.0) <= 0.1);
    REQUIRE(xt::bundle::detail::hexDeltaE2000("#FF5A1F", "#ff5a1f", &dE));
    CHECK(dE == doctest::Approx(0.0));
    // Sharma, Wu, Dalal (2005), table 1: pairs 1, 7 and 17.
    CHECK(deltaE2000(Lab{50.0, 2.6772, -79.7751}, Lab{50.0, 0.0, -82.7485}) ==
          doctest::Approx(2.0425).epsilon(0.0005));
    CHECK(deltaE2000(Lab{50.0, 0.0, 0.0}, Lab{50.0, -1.0, 2.0}) == doctest::Approx(2.3669).epsilon(0.0005));
    CHECK(deltaE2000(Lab{50.0, 2.5, 0.0}, Lab{73.0, 25.0, -18.0}) ==
          doctest::Approx(27.1492).epsilon(0.0005));
    float rgba[4];
    REQUIRE(xt::bundle::detail::parseHexColor("#0b0e1340", rgba));
    CHECK(rgba[3] == doctest::Approx(64.0 / 255.0));
    REQUIRE(xt::bundle::detail::parseHexColor("#fff", rgba));
    CHECK(rgba[0] == 1.0f);
    CHECK_FALSE(xt::bundle::detail::parseHexColor("fff", rgba));
    CHECK_FALSE(xt::bundle::detail::parseHexColor("#ggg", rgba));
    CHECK_FALSE(xt::bundle::detail::hexDeltaE2000("red", "#fff", &dE));
}
