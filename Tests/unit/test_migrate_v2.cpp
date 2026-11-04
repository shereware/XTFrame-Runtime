#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "doctest.h"
#include "test_temp.h"
#include "xt/compile/canonical_writer.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/migrate.h"
#include "xt/compile/utf8_path.h"
#include "xt_v2_test_util.h"

/// T-589 (Docs/design/54-source-format-v2.md §54.10-§54.11): the source format 1 -> 2 migrator
/// (xt/compile/migrate.h) over the frozen fixtures of Tests/xt/v1 and small documents written to
/// the test temp directory: every step, the self-check (layer two must be equal, layer one is only
/// reported), the computed-style dump catching a real difference, the editor's id rule, the
/// canonical output and writing without backup files.

using namespace xt::compile;

namespace {

namespace fs = std::filesystem;

const MigrateFileReport* findReport(const MigrateReport& report, const std::string& fileName) {
    for (const MigrateFileReport& f : report.files) {
        if (pathFromUtf8(f.path).filename() == pathFromUtf8(fileName)) {
            return &f;
        }
    }
    return nullptr;
}

const MigratedFile* findFile(const std::vector<MigratedFile>& files, const std::string& fileName) {
    for (const MigratedFile& f : files) {
        if (pathFromUtf8(f.path).filename() == pathFromUtf8(fileName)) {
            return &f;
        }
    }
    return nullptr;
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

void writeFile(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

/// A fresh directory under the test temp root.
fs::path freshDir(const std::string& name) {
    const fs::path dir = xt::test::tempDir() / ("migrate_v2_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void requireMigrated(const MigrateReport& report) {
    for (const MigrateFileReport& f : report.files) {
        CAPTURE(f.path);
        CAPTURE(f.checkDetail);
        for (const std::string& e : f.errors) {
            CAPTURE(e);
            CHECK(false);
        }
    }
    REQUIRE(report.ok());
}

std::string fixtureDir() { return std::string(XT_TEST_XT_DIR) + "/v1"; }

}  // namespace

TEST_CASE("T-589 migrate: the v1 fixtures, every step, layer two equal") {
    MigrateOptions options;
    std::vector<MigratedFile> files;
    MigrateReport report;
    migrateToV2(fixtureDir(), options, files, report);
    requireMigrated(report);

    const MigrateFileReport* basic = findReport(report, "migrate_basic.xt");
    REQUIRE(basic != nullptr);
    CHECK(basic->kind == "ui");
    CHECK(basic->layerTwo == MigrateCheck::Equal);
    CHECK(basic->layerOne == MigrateCheck::Differ);  // Split rules: layer one is only a report.
    CHECK(basic->portsMoved == 4);                   // hp, tab, go on <body>; level on the symbol root.
    CHECK(basic->triggersMoved == 3);                // on-click, state-enter, #Pages/T1 on-press.
    CHECK(basic->rulesSplit == 3);                   // #Btn, #Badge, symbol(Chip) #Dot.
    CHECK(basic->inlineMoved == 3);
    REQUIRE(basic->idsAssigned.size() == 2);
    CHECK(contains(basic->idsAssigned[0], "-> Group_1"));
    CHECK(contains(basic->idsAssigned[1], "-> Status_ready"));

    const MigratedFile* doc = findFile(files, "migrate_basic.xt");
    REQUIRE(doc != nullptr);
    const std::string& text = doc->text;
    // Step 1: links, the inline <style>, the <head> symbol.
    CHECK(contains(text, "<xt version=\"2\">"));
    CHECK(contains(text, "<symbols src=\"migrate_lib.xt\"/>"));
    CHECK(contains(text, "<layout src=\"migrate_basic.layout.xts\"/>"));
    CHECK(contains(text, "<style src=\"migrate_basic.xts\"/>"));
    CHECK(contains(text, "<timelines src=\"migrate_basic.timelines.xts\"/>"));
    CHECK(contains(text, "\n  <symbol name=\"Chip\">\n"));
    CHECK_FALSE(contains(text, "<link"));
    // Step 2: ports, in declaration order; none left on elements.
    CHECK(contains(
        text, "  <ports>\n    hp: number in = 0.8\n    tab: number in = 0\n    go: event out\n  </ports>"));
    CHECK(contains(text, "    <ports>\n      level: number in = 1\n    </ports>"));
    CHECK_FALSE(contains(text, "port:"));
    // Step 3: the symbol timeline.
    CHECK(contains(text, "  <timelines>\n    symbol(Chip) {\n      @timeline blink {"));
    // Step 4: rule triggers on the elements, before their own on: attributes.
    CHECK(contains(text, "<use symbol=\"Tabs\" id=\"Pages\" on:press@T1=\"set(tab, 0)\"/>"));
    CHECK(contains(text, "on:state(Low).enter=\"class(alarm)\""));
    CHECK(contains(text, "<button id=\"Btn\" on:click=\"emit(go)\" on:hover=\"play(flash)\">"));
    CHECK_FALSE(contains(text, "on-click"));
    // Step 5: one rule, two sections, same selector.
    CHECK(contains(text, "    #Btn {\n      left: 24px;"));
    CHECK(contains(text, "    #Btn {\n      color: #ffffff;\n    }"));
    // Step 6: inline geometry -> the last <layout>, ids from the editor's rule.
    CHECK(contains(text, "<div id=\"Group_1\" class=\"card\" style=\"opacity: 0.8\">"));
    CHECK(contains(text,
                   "    #Group_1 {\n      position: absolute;\n      left: 16px;\n      top: 120px;\n    }"));
    CHECK(contains(text, "<p id=\"Status_ready\" style=\"color: #c0c0c0\">"));

    const MigratedFile* style = findFile(files, "migrate_basic.xts");
    const MigratedFile* layout = findFile(files, "migrate_basic.layout.xts");
    const MigratedFile* timelines = findFile(files, "migrate_basic.timelines.xts");
    REQUIRE(style != nullptr);
    REQUIRE(layout != nullptr);
    REQUIRE(timelines != nullptr);
    CHECK_FALSE(style->created);
    CHECK(layout->created);
    CHECK(contains(style->text, ".card:hover {\n  background-color: var(--accent);\n}"));
    CHECK(contains(layout->text, ".card:hover {\n  width: 220px;\n}"));
    CHECK(contains(layout->text, "@media (max-width: 500px) {\n  .card {\n    padding: 4px;"));
    CHECK(contains(style->text, "@media (max-width: 500px) {\n  .card {\n    color: #ffffff;"));
    CHECK(contains(timelines->text, "@timeline flash from \"migrate_basic.xtanim.json#flash\";"));
    CHECK_FALSE(contains(style->text, "on-press"));
    CHECK_FALSE(contains(style->text, "state-enter"));

    const MigrateFileReport* lib = findReport(report, "migrate_lib.xt");
    REQUIRE(lib != nullptr);
    CHECK(lib->kind == "library");
    CHECK(lib->portsMoved == 2);
    const MigratedFile* libFile = findFile(files, "migrate_lib.xt");
    REQUIRE(libFile != nullptr);
    CHECK(contains(libFile->text,
                   "<ports>\n      label: string in = \"Tab\"\n      pressed: event out\n    </ports>"));
    CHECK(
        contains(libFile->text, "    <layout>\n      #Label {\n        left: 4px;\n      }\n    </layout>"));

    // Every migrated .xt is already in canonical form.
    for (const MigratedFile& f : files) {
        if (pathFromUtf8(f.path).extension() != ".xt") {
            continue;
        }
        CAPTURE(f.path);
        DiagnosticSink sink;
        CanonicalXt canonical;
        REQUIRE(readCanonicalXt(f.text, f.path, sink, canonical));
        CHECK(writeCanonicalXt(canonical) == f.text);
    }
}

TEST_CASE("T-589 migrate: --anchors rewrites the three formulas and leaves other forms") {
    MigrateOptions options;
    options.anchors = true;
    std::vector<MigratedFile> files;
    MigrateReport report;
    migrateToV2(fixtureDir() + "/migrate_anchors.xt", options, files, report);
    requireMigrated(report);
    const MigrateFileReport* r = findReport(report, "migrate_anchors.xt");
    REQUIRE(r != nullptr);
    CHECK(r->anchors == 4);
    CHECK(r->layerTwo == MigrateCheck::Equal);
    CHECK(r->layerOneWithoutSourceMap == MigrateCheck::Equal);  // The shorthand expands to the same IR.
    const MigratedFile* f = findFile(files, "migrate_anchors.xt");
    REQUIRE(f != nullptr);
    CHECK(contains(
        f->text,
        "#TopLeft {\n      anchor: left top;\n      offset: 24px 0;\n      size: 100px 40px;\n    }"));
    CHECK(contains(f->text, "anchor: right bottom;\n      offset: 16px 8px;"));
    CHECK(contains(
        f->text,
        "anchor: stretch top;\n      offset: 10px 50px;\n      offset-end: 20px 50px;\n      height: 12px;"));
    CHECK(contains(f->text, "anchor: center center;\n      offset: 5px 0;\n      size: 200px 60px;"));
    CHECK(contains(f->text, "#Other {\n      position: absolute;\n      left: calc(1safe-left - 4px);"));

    // Without --anchors the formulas stay as they are.
    options.anchors = false;
    migrateToV2(fixtureDir() + "/migrate_anchors.xt", options, files, report);
    requireMigrated(report);
    const MigratedFile* plain = findFile(files, "migrate_anchors.xt");
    REQUIRE(plain != nullptr);
    CHECK_FALSE(contains(plain->text, "anchor:"));
}

TEST_CASE("T-589 migrate: a #<use>/<path> trigger keeps the TRIG chunk byte for byte") {
    const fs::path dir = freshDir("trig");
    writeFile(dir / "lib.xt",
              "<xt><head><style>symbol(Tab) { #Box { width: 10px; } }</style>"
              "<symbol name=\"Tab\"><div id=\"Box\"></div></symbol>"
              "<symbol name=\"Tabs\"><div id=\"Row\"><use symbol=\"Tab\" id=\"T1\"/><use symbol=\"Tab\" "
              "id=\"T2\"/></div>"
              "</symbol></head></xt>\n");
    writeFile(dir / "doc.xts",
              "#Pages/T1 { on-press: set(tab, 0); }\n#Pages/T2 { on-press: set(tab, 1); }\n");
    writeFile(dir / "doc.xt",
              "<xt><head><link rel=\"symbols\" href=\"lib.xt\"/><link href=\"doc.xts\"/></head>"
              "<body port:tab=\"number in = 0\"><use symbol=\"Tabs\" id=\"Pages\"/></body></xt>\n");
    MigrateOptions options;
    std::vector<MigratedFile> files;
    MigrateReport report;
    migrateToV2(pathToUtf8(dir), options, files, report);
    requireMigrated(report);

    std::vector<SourceFile> before;
    std::vector<SourceFile> after;
    std::vector<std::string> originals;
    for (const char* name : {"doc.xt", "doc.xts", "lib.xt"}) {
        std::ifstream in(dir / name, std::ios::binary);
        originals.emplace_back(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::string docPath = genericPathToUtf8(dir / "doc.xt");
    const std::string sheetPath = genericPathToUtf8(dir / "doc.xts");
    const std::string libPath = genericPathToUtf8(dir / "lib.xt");
    before = {SourceFile{docPath, originals[0]}, SourceFile{sheetPath, originals[1]},
              SourceFile{libPath, originals[2]}};
    const MigratedFile* doc = findFile(files, "doc.xt");
    REQUIRE(doc != nullptr);
    CHECK(contains(doc->text, "on:press@T1=\"set(tab, 0)\" on:press@T2=\"set(tab, 1)\""));
    after.push_back(SourceFile{doc->path, doc->text});
    for (const MigratedFile& f : files) {
        if (f.path != doc->path) {
            after.push_back(SourceFile{f.path, f.text});
        }
    }
    CompileOptions compile;
    compile.emitMeta = false;
    compile.xtDir = genericPathToUtf8(dir);
    compile.rootDir = compile.xtDir;
    const CompileResult v1 = compileDocumentFromStrings(before, docPath, compile);
    const CompileResult v2 = compileDocumentFromStrings(after, docPath, compile);
    xt::test::v2::requireCompiled(v1);
    xt::test::v2::requireCompiled(v2);
    const std::vector<uint8_t> trig = xt::test::v2::chunkPayload(v1.ir, "TRIG");
    CHECK_FALSE(trig.empty());
    CHECK(trig == xt::test::v2::chunkPayload(v2.ir, "TRIG"));
}

TEST_CASE("T-589 migrate: the self-check catches inline geometry that loses to a :hover rule") {
    // Version 1: the inline `left` wins over `#A:hover`. Moved into `#A`, the :hover rule wins:
    // layer two differs and the migration is not ok (nothing would be written in place).
    const fs::path dir = freshDir("differ");
    writeFile(dir / "doc.xt",
              "<xt><head><style>#A:hover { left: 9px; }</style></head>"
              "<body><div id=\"A\" style=\"position: absolute; left: 5px\"></div></body></xt>\n");
    MigrateOptions options;
    std::vector<MigratedFile> files;
    MigrateReport report;
    CHECK_FALSE(migrateToV2(pathToUtf8(dir / "doc.xt"), options, files, report));
    const MigrateFileReport* r = findReport(report, "doc.xt");
    REQUIRE(r != nullptr);
    CHECK(r->layerTwo == MigrateCheck::Differ);
    CHECK(contains(r->checkDetail, "left"));
    CHECK(contains(report.summary(), "layer two differs"));
    CHECK(contains(report.json(), "\"layerTwo\": \"differ\""));
}

TEST_CASE("T-589 migrate: ids for id-less elements follow Document::autoName()") {
    const fs::path dir = freshDir("ids");
    writeFile(dir / "doc.xt",
              "<xt><head><symbol name=\"Gem\"><div id=\"G\"></div></symbol></head><body>"
              "<div id=\"Group_3\"></div>"
              "<div style=\"width: 10px\"></div>"
              "<span style=\"left: 1px\">Hello world!</span>"
              "<span style=\"left: 2px\">Hello world</span>"
              "<img src=\"a.png\" style=\"width: 4px\"/>"
              "<use symbol=\"Gem\" style=\"left: 3px\"/>"
              "<div style=\"display: flex; gap: 2px\"><div id=\"In\"></div></div>"
              "</body></xt>\n");
    MigrateOptions options;
    options.selfCheck = false;
    std::vector<MigratedFile> files;
    MigrateReport report;
    migrateToV2(pathToUtf8(dir / "doc.xt"), options, files, report);
    const MigrateFileReport* r = findReport(report, "doc.xt");
    REQUIRE(r != nullptr);
    std::vector<std::string> ids;
    for (const std::string& line : r->idsAssigned) {
        ids.push_back(line.substr(line.rfind("-> ") + 3));
    }
    const std::vector<std::string> expected{"Group_4", "Hello_world", "Hello_world_2",
                                            "Image_1", "Gem_1",       "Stack_1"};
    CHECK(ids == expected);
}

TEST_CASE("T-589 migrate: skipped files and writing without backup files") {
    const fs::path dir = freshDir("write");
    writeFile(dir / "src" / "a.xt",
              "<xt><head><link href=\"a.xts\"/></head><body><div id=\"A\"></div></body></xt>\n");
    writeFile(dir / "src" / "a.xts", "#A { width: 10px; color: #ff0000; }\n");
    writeFile(dir / "src" / "v2.xt", "<xt version=\"2\">\n  <body></body>\n</xt>\n");
    writeFile(dir / "src" / "broken.xt", "<xt><body><div></body></xt>\n");
    MigrateOptions options;
    std::vector<MigratedFile> files;
    MigrateReport report;
    migrateToV2(pathToUtf8(dir / "src"), options, files, report);
    requireMigrated(report);
    const MigrateFileReport* v2 = findReport(report, "v2.xt");
    const MigrateFileReport* broken = findReport(report, "broken.xt");
    REQUIRE(v2 != nullptr);
    REQUIRE(broken != nullptr);
    CHECK(v2->status == "skipped");
    CHECK(v2->reason == "already version 2");
    CHECK(broken->status == "skipped");
    CHECK(contains(broken->reason, "does not parse"));

    std::string error;
    const fs::path out = dir / "out";
    REQUIRE(writeMigratedFiles(files, report, pathToUtf8(out), error));
    std::vector<std::string> written;
    for (const auto& entry : fs::recursive_directory_iterator(out)) {
        if (entry.is_regular_file()) {
            written.push_back(genericPathToUtf8(entry.path().lexically_relative(out)));
        }
    }
    std::sort(written.begin(), written.end());
    const std::vector<std::string> expected{"a.layout.xts", "a.xt", "a.xts"};
    CHECK(written == expected);

    // In place: the same three files, nothing else appears next to them.
    REQUIRE(writeMigratedFiles(files, report, std::string(), error));
    std::vector<std::string> inPlace;
    for (const auto& entry : fs::directory_iterator(dir / "src")) {
        inPlace.push_back(pathToUtf8(entry.path().filename()));
    }
    std::sort(inPlace.begin(), inPlace.end());
    const std::vector<std::string> expectedInPlace{"a.layout.xts", "a.xt", "a.xts", "broken.xt", "v2.xt"};
    CHECK(inPlace == expectedInPlace);
}

TEST_CASE("T-589 dumpComputedStyle: combinations, media and determinism") {
    const std::string doc =
        "<xt><head><style>#A { width: 10px; } #A:hover { width: 20px; } "
        "@media (max-width: 300px) { #A { width: 5px; } } #A:state(Open) { opacity: 0.5; }</style></head>"
        "<body><div id=\"A\" style=\"color: #ff0000\"></div></body></xt>";
    const CompileResult r = xt::test::v2::compileFiles({SourceFile{"dump.xt", doc}});
    xt::test::v2::requireCompiled(r);
    std::string a;
    std::string b;
    std::string error;
    REQUIRE(dumpComputedStyle(r.ir, a, error));
    REQUIRE(dumpComputedStyle(r.ir, b, error));
    CHECK(a == b);
    CHECK(contains(a, "\"combinations\": [\"\", \":hover\", \":state(Open)\"]"));
    CHECK(contains(a, "\"\": {\"base\": {\"color\": \"rgba(1,0,0,1)\", \"width\": \"10px\"}"));
    CHECK(contains(a, "\":hover\": {\"base\": {\"color\": \"rgba(1,0,0,1)\", \"width\": \"20px\"}"));
    CHECK(contains(a, "\"@media (1:300)\": {\"width\": \"5px\"}"));
    CHECK(contains(a, "\"opacity\": \"0.5\""));

    const std::vector<uint8_t> junk{1, 2, 3};
    CHECK_FALSE(dumpComputedStyle(junk, a, error));
}

TEST_CASE("T-589 migrate: UI root ports move and their flow endpoints become #document") {
    // T-604 ruling (53): `#<root id>.<port>` in the sibling .xtflow -> `#document.<port>`.
    const fs::path dir = freshDir("rootports");
    writeFile(
        dir / "doc.xt",
        "<xt><body port:a=\"number in\"><div id=\"Root\" port:x=\"number in = 2\" port:go=\"event out\">"
        "<div id=\"In\" bind:opacity=\"x\"></div></div></body></xt>\n");
    const std::string flowBefore =
        "{\"edges\": [{\"from\": \"#Root.go\", \"to\": \"n.in\"}, {\"from\": \"n.out\", \"to\": "
        "\"#Root.x\"}, "
        "{\"from\": \"#Root.xy\", \"to\": \"m.in\"}]}\n";
    const std::string flowAfter =
        "{\"edges\": [{\"from\": \"#document.go\", \"to\": \"n.in\"}, {\"from\": \"n.out\", \"to\": "
        "\"#document.x\"}, {\"from\": \"#Root.xy\", \"to\": \"m.in\"}]}\n";
    writeFile(dir / "doc.xtflow", flowBefore);
    MigrateOptions options;
    std::vector<MigratedFile> files;
    MigrateReport report;
    migrateToV2(pathToUtf8(dir / "doc.xt"), options, files, report);
    requireMigrated(report);
    const MigratedFile* doc = findFile(files, "doc.xt");
    const MigratedFile* flow = findFile(files, "doc.xtflow");
    REQUIRE(doc != nullptr);
    REQUIRE(flow != nullptr);
    CHECK(contains(doc->text,
                   "<ports>\n    a: number in\n    x: number in = 2\n    go: event out\n  </ports>"));
    CHECK(contains(doc->text, "<div id=\"Root\">"));
    CHECK(flow->text == flowAfter);
    const MigrateFileReport* r = findReport(report, "doc.xtflow");
    REQUIRE(r != nullptr);
    CHECK(r->kind == "flow");
}

TEST_CASE("T-589 migrate: a port on an element other than <body>, the UI root or a symbol root is an error") {
    // 54 §54.4 has no place for it; moved to <ports> its name #Deep.x would change.
    const fs::path dir = freshDir("ports");
    writeFile(dir / "doc.xt",
              "<xt><body port:a=\"number in\"><div id=\"Root\"><div id=\"Deep\" port:x=\"number in\"></div>"
              "</div></body></xt>\n");
    MigrateOptions options;
    std::vector<MigratedFile> files;
    MigrateReport report;
    CHECK_FALSE(migrateToV2(pathToUtf8(dir / "doc.xt"), options, files, report));
    const MigrateFileReport* r = findReport(report, "doc.xt");
    REQUIRE(r != nullptr);
    CHECK(r->status == "error");
    REQUIRE(r->errors.size() == 1);
    CHECK(contains(r->errors[0], "#Deep.x"));
    CHECK(findFile(files, "doc.xt") == nullptr);
}
