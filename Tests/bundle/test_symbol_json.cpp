#include <filesystem>
#include <string>
#include <vector>

#include "doctest.h"
#include "test_temp.h"
#include "xt/bundle/symbol_json.h"
#include "xt/bundle/symbol_library.h"
#include "xt/bundle/symbol_lint.h"
#include "xt/bundle/symbol_set.h"
#include "xt/compile/utf8_path.h"

/// T-502 (Docs/design/49-symbol-sets.md 49.7): the JSON shapes of xt/bundle/symbol_json.h, which
/// the Editor Service's `symbols.*` operations return, against the preset library in Assets/Symbols.

using namespace xt;
using namespace xt::bundle;
namespace fs = std::filesystem;

namespace {

const fs::path kPresetDir = fs::path(XT_TEST_BUNDLE_DIR) / ".." / ".." / "Assets" / "Symbols";
constexpr const char* kPresetGroup = "broadsheet-2026";

/// The preset library and a fresh user library mounted (nothing of the real user data dir).
LibraryMounts presetMounts(const char* name) {
    const fs::path userData = xt::test::tempDir() / name;
    std::error_code ec;
    fs::remove_all(userData, ec);
    LibraryMounts mounts;
    REQUIRE(mountDefaults(mounts, kPresetDir, userData) == Status::kOk);
    return mounts;
}

}  // namespace

TEST_CASE("T-502 librariesJson: one row per mounted library, then the unmounted entries") {
    LibraryMounts mounts = presetMounts("xt_symjson_libs");
    MountEntry disabled;
    disabled.path = "/nowhere/library";
    disabled.enabled = false;
    mounts.entries.push_back(disabled);
    const SymbolJson rows = librariesJson(mounts);
    REQUIRE(rows.is_array());
    REQUIRE(rows.size() == 3);
    CHECK(rows[0]["scope"] == "preset");
    CHECK(rows[0]["readOnly"] == true);
    CHECK(rows[0]["enabled"] == true);
    CHECK(rows[0]["defaultGroup"] == kPresetGroup);
    CHECK(rows[0]["groups"] == SymbolJson::array({kPresetGroup}));
    CHECK(rows[0]["path"].get<std::string>() == mounts.libraries[0].dir);
    CHECK(rows[1]["scope"] == "user");
    CHECK(rows[1]["id"] == "mine");
    CHECK(rows[1]["readOnly"] == false);
    CHECK(rows[2]["path"] == "/nowhere/library");
    CHECK(rows[2]["enabled"] == false);
    CHECK(rows[2]["id"] == "");
    CHECK(rows[2]["scope"] == "extra");
}

TEST_CASE(
    "T-502 libraryJson / groupJson / symbolSummaryJson: the preset library with its group and symbols") {
    const LibraryMounts mounts = presetMounts("xt_symjson_lib");
    const SymbolJson lib = libraryJson(mounts.libraries[0]);
    CHECK(lib["id"] == mounts.libraries[0].id);
    CHECK(lib["scope"] == "preset");
    CHECK_FALSE(lib.contains("enabled"));
    REQUIRE(lib["groups"].size() == 1);
    const SymbolJson& group = lib["groups"][0];
    CHECK(group["id"] == kPresetGroup);
    CHECK(group["source"]["kind"] == "preset");
    CHECK(group["source"]["set"] == "xtframe/broadsheet-2026");
    CHECK(group["tokens"] == "tokens.xts");
    CHECK(group["lint"]["errors"] == 0);
    CHECK(group["problems"].is_array());
    CHECK(group["problems"].empty());
    CHECK(group["symbols"].size() == mounts.libraries[0].groups[0].symbols.size());
    bool button = false;
    for (const SymbolJson& symbol : group["symbols"]) {
        if (symbol["name"] != "Button") {
            continue;
        }
        button = true;
        CHECK(symbol["file"] == "Button.xt");
        CHECK(symbol["requires"]["states"].is_array());
        CHECK(symbol["ports"].is_array());
        CHECK_FALSE(symbol.contains("tokens"));  // a summary: the full entry is symbolEntryJson()
    }
    CHECK(button);
}

TEST_CASE("T-502 symbolEntryJson: the full index entry of Button") {
    const LibraryMounts mounts = presetMounts("xt_symjson_entry");
    const SymbolEntry* button = findSymbol(mounts, "Button");
    REQUIRE(button != nullptr);
    const SymbolJson j = symbolEntryJson(*button);
    CHECK(j["name"] == "Button");
    CHECK(j["file"] == "Button.xt");
    CHECK(j["thumbnail"] == "thumbnails/Button.png");
    CHECK(j["content"].get<std::string>().rfind("sha256:", 0) == 0);
    REQUIRE(j["tokens"].is_array());
    CHECK_FALSE(j["tokens"].empty());
    CHECK(j["tokens"][0].contains("name"));
    CHECK(j["tokens"][0].contains("default"));
    CHECK(j["fonts"].is_array());
    CHECK(j["textures"].is_array());
    CHECK(j["ports"].size() == button->ports.size());
    for (const SymbolJson& port : j["ports"]) {
        CHECK(port.contains("name"));
        CHECK(port.contains("type"));
        CHECK(port.contains("dir"));
    }
}

TEST_CASE("T-502 lintReportJson, useReportJson, setInfoJson, importReportJson: the data shapes") {
    LintReport report;
    report.groupId = "g";
    report.dir = "/lib/groups/g";
    report.symbols = 2;
    report.issues.push_back(
        LintIssue{LintLevel::Error, "Button", "contract.state", "no rule for state hover"});
    report.issues.push_back(LintIssue{LintLevel::Warning, "", "group.thumbnail", "no group thumbnail"});
    const SymbolJson lint = lintReportJson(report);
    CHECK(lint["id"] == "g");
    CHECK(lint["symbols"] == 2);
    CHECK(lint["errors"] == 1);
    CHECK(lint["warnings"] == 1);
    REQUIRE(lint["issues"].size() == 2);
    CHECK(lint["issues"][0]["level"] == "error");
    CHECK(lint["issues"][0]["symbol"] == "Button");
    CHECK(lint["issues"][0]["code"] == "contract.state");
    CHECK(lint["issues"][1]["level"] == "warning");

    UseReport use;
    use.status = Status::kIoError;
    use.written = {"shared/symbols/Button.xt", "shared/tokens.xts"};
    use.skipped = {"shared/symbols/Toggle.xt: the project has it"};
    use.renamed = {"fonts/a.ttf -> shared/fonts/a-01234567.ttf"};
    use.tokensAdded = {"--color-accent", "night:--color-accent"};
    use.notFound = {"Nope"};
    const SymbolJson u = useReportJson(use);
    CHECK(u["status"] == "kIoError");
    CHECK(u["written"].size() == 2);
    CHECK(u["skipped"][0] == "shared/symbols/Toggle.xt: the project has it");
    CHECK(u["renamed"].size() == 1);
    CHECK(u["tokensAdded"][1] == "night:--color-accent");
    CHECK(u["notFound"] == SymbolJson::array({"Nope"}));

    ImportReport imported;
    imported.status = Status::kOk;
    imported.set.id = "broadsheet-2026";
    imported.set.author = "xtframe";
    imported.set.setVersion = "2.0.0";
    imported.set.license = "CC0-1.0";
    imported.set.fontsLicense = "OFL-1.1";
    imported.set.groups = {"broadsheet-2026"};
    ImportedGroup g;
    g.sourceId = "broadsheet-2026";
    g.targetId = "broadsheet-2026-2";
    g.action = ImportAction::OlderSkipped;
    g.trashed = "";
    g.lint = report;
    imported.groups.push_back(g);
    const SymbolJson i = importReportJson(imported);
    CHECK(i["status"] == "kOk");
    CHECK(i["set"]["id"] == "broadsheet-2026");
    CHECK(i["set"]["author"] == "xtframe");
    CHECK(i["set"]["version"] == "2.0.0");
    CHECK(i["set"]["license"] == "CC0-1.0");
    CHECK(i["set"]["fontsLicense"] == "OFL-1.1");
    CHECK(i["set"]["groups"] == SymbolJson::array({"broadsheet-2026"}));
    REQUIRE(i["groups"].size() == 1);
    CHECK(i["groups"][0]["action"] == "older-skipped");
    CHECK(i["groups"][0]["targetId"] == "broadsheet-2026-2");
    CHECK(i["groups"][0]["lint"]["errors"] == 1);
    CHECK(i["lintErrors"] == 1);
    CHECK(i["lintWarnings"] == 1);
    CHECK(std::string(importActionName(ImportAction::OlderReplaced)) == "older-replaced");
    CHECK(std::string(libraryScopeName(LibraryScope::Project)) == "project");
}
