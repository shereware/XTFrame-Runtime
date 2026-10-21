#include <filesystem>
#include <fstream>
#include <iterator>
#include <json.hpp>
#include <string>

#include "doctest.h"
#include "test_temp.h"
#include "xt/bundle/asset_meta.h"
#include "xt/bundle/design_system.h"
#include "xt/bundle/migrations.h"
#include "xt/bundle/project_edit.h"
#include "xt/bundle/ui_project.h"

/// T-406 (docs/design/43-compatibility-and-versions.md §43.2): the source-format `version` fields,
/// read old / write new, the migration registry (with a fake v0 -> v1 step), "a newer XTFrame is
/// needed" refusals and unknown keys kept across a load / save. The `.xtflow`, `<xt version>` and
/// `.xteditor.json` cases live in tests/flow, tests/unit and tests/editor.

using namespace xt;
using namespace xt::bundle;
using nlohmann::json;

namespace {

namespace fs = std::filesystem;

/// A fresh temp directory `xt_t406_<name>` (removed first).
fs::path freshDirT406(const std::string& name) {
    const fs::path dir = xt::test::tempDir() / ("xt_t406_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

void writeTextT406(const fs::path& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    out << text;
}

std::string readTextT406(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

constexpr const char* kUisT406 = R"("uis": [{"id": "hud", "doc": "ui/hud/hud.xt"}])";

/// The fake v0 -> v1 step of the framework test: renames `old` to `new` and marks the document.
bool fakeV0ToV1(json& doc, std::string& error) {
    if (!doc.is_object()) {
        error = "not an object";
        return false;
    }
    if (doc.contains("old")) {
        doc["new"] = doc["old"];
        doc.erase("old");
    }
    doc["migrated"] = true;
    return true;
}

bool failingStep(json&, std::string& error) {
    error = "cannot";
    return false;
}

}  // namespace

TEST_CASE("T-406 migration registry: empty sequences, a fake v0 -> v1 step, newer refused") {
    for (SourceFormat format : {SourceFormat::UiProject, SourceFormat::Flow, SourceFormat::EditorState,
                                SourceFormat::Meta, SourceFormat::DesignYaml}) {
        CHECK(migrationTable(format).latest == 1);
        CHECK(migrationTable(format).count == 0);
    }
    CHECK(kFlowFormatVersion == flow::kFlowFormatVersion);

    static constexpr MigrationStep kSteps[] = {fakeV0ToV1};
    const MigrationTable fake{1, kSteps, 1};
    json doc = {{"old", 5}};
    Result<int> reached = migrateWith(fake, doc, 0);
    REQUIRE(reached.ok());
    CHECK(reached.value() == 1);
    CHECK(doc == json{{"new", 5}, {"migrated", true}});
    // Already the latest: nothing runs.
    json latest = {{"old", 5}};
    REQUIRE(migrateWith(fake, latest, 1).ok());
    CHECK(latest == json{{"old", 5}});
    // Newer: refused, never guessed at.
    std::string error;
    CHECK(migrateWith(fake, latest, 2, &error).status() == Status::kUnsupported);
    CHECK(error.find("newer XTFrame") != std::string::npos);
    // No step from an unknown old version; a failing step is kInvalidArgument.
    CHECK(migrateWith(migrationTable(SourceFormat::Meta), latest, 0, &error).status() ==
          Status::kUnsupported);
    static constexpr MigrationStep kFailing[] = {failingStep};
    CHECK(migrateWith(MigrationTable{1, kFailing, 1}, latest, 0, &error).status() ==
          Status::kInvalidArgument);
    CHECK(error.find("cannot") != std::string::npos);

    // Every registered sequence: 1 is a no-op, 2 is refused.
    json any = json::object();
    for (auto migrate : {migrateUiProject, migrateFlow, migrateEditorState, migrateMeta, migrateDesignYaml}) {
        REQUIRE(migrate(any, 1, nullptr).ok());
        CHECK(migrate(any, 2, nullptr).status() == Status::kUnsupported);
    }
    CHECK(any == json::object());

    // readFormatVersion: missing = 1; anything but a positive integer is an error.
    int version = 0;
    CHECK(readFormatVersion(json::object(), version));
    CHECK(version == 1);
    CHECK(readFormatVersion(json{{"version", 3}}, version));
    CHECK(version == 3);
    CHECK_FALSE(readFormatVersion(json{{"version", "1"}}, version));
    CHECK_FALSE(readFormatVersion(json{{"version", 0}}, version));
    CHECK_FALSE(readFormatVersion(json{{"version", 1.5}}, version));
    CHECK(readFormatVersion(json{{"v", 2}}, version, nullptr, "v"));
    CHECK(version == 2);
    int from = 0;
    json versioned = {{"version", 1}};
    CHECK(migrateToLatest(SourceFormat::UiProject, versioned, &from) == Status::kOk);
    CHECK(from == 1);
    json future = {{"version", 9}};
    CHECK(migrateToLatest(SourceFormat::DesignYaml, future, &from, &error) == Status::kUnsupported);
    json malformed = {{"version", "x"}};
    CHECK(migrateToLatest(SourceFormat::Meta, malformed, &from, &error) == Status::kInvalidArgument);
}

TEST_CASE("T-406 .xtui: missing version = 1, newer refused, unknown keys kept at every level") {
    const fs::path dir = freshDirT406("xtui");
    writeTextT406(dir / "P.xtui", std::string(R"({"name": "P", "id": "p", )") + kUisT406 + "}");
    std::string error;
    Result<UiProject> loaded = UiProject::load(dir.string(), &error);
    REQUIRE_MESSAGE(loaded.ok(), error);
    CHECK(loaded.value().version == 1);
    CHECK(loaded.value().migratedFrom == 0);
    REQUIRE(loaded.value().save(&error) == Status::kOk);
    CHECK(json::parse(readTextT406(dir / "P.xtui"))["version"] == 1);

    writeTextT406(dir / "P.xtui", std::string(R"({"version": 2, "name": "P", "id": "p", )") + kUisT406 + "}");
    CHECK(UiProject::load(dir.string(), &error).status() == Status::kUnsupported);
    CHECK(error.find("newer XTFrame") != std::string::npos);
    writeTextT406(dir / "P.xtui",
                  std::string(R"({"version": "one", "name": "P", "id": "p", )") + kUisT406 + "}");
    CHECK(UiProject::load(dir.string(), &error).status() == Status::kInvalidArgument);

    writeTextT406(dir / "P.xtui", std::string(R"({"version": 1, "name": "P", "id": "p", )") + kUisT406 +
                                      R"(, "future": {"a": 1}, "spec": {"futureSpec": 2},
        "shared": {"styles": [], "futureShared": "s"}, "preview": {"dpr": 2, "futurePreview": [1, 2]},
        "publish": {"outDir": "out", "futurePublish": true}, "assets": {"futureAssets": null}})");
    loaded = UiProject::load(dir.string(), &error);
    REQUIRE_MESSAGE(loaded.ok(), error);
    REQUIRE(loaded.value().save(&error) == Status::kOk);
    const json saved = json::parse(readTextT406(dir / "P.xtui"));
    CHECK(saved["version"] == 1);
    CHECK(saved["future"] == json{{"a", 1}});
    CHECK(saved["spec"]["futureSpec"] == 2);
    CHECK(saved["shared"]["futureShared"] == "s");
    CHECK(saved["preview"]["futurePreview"] == json::array({1, 2}));
    CHECK(saved["preview"]["dpr"] == 2);
    CHECK(saved["publish"]["futurePublish"] == true);
    CHECK(saved["publish"]["outDir"] == "out");
    CHECK(saved["assets"].contains("futureAssets"));
}

TEST_CASE("T-406 .meta: missing version = 1, newer refused, version written first, unknown keys kept") {
    assets::AssetMeta meta;
    std::string error;
    REQUIRE(assets::parseMeta("id: a\ntype: image\n", meta, &error));
    CHECK(meta.id == "a");
    CHECK_FALSE(assets::parseMeta("version: 2\nid: a\n", meta, &error));
    CHECK(error.find("newer XTFrame") != std::string::npos);
    CHECK_FALSE(assets::parseMeta("version: x\nid: a\n", meta, &error));
    REQUIRE(assets::parseMeta("version: 1\nid: a\nfutureKey: [1, 2]\n", meta, &error));
    CHECK_FALSE(meta.extra.contains("version"));
    const std::string text = assets::metaText(meta);
    CHECK(text.rfind("version: 1\nid: a\n", 0) == 0);
    CHECK(text.find("futureKey: [1, 2]\n") != std::string::npos);
}

TEST_CASE("T-406 design/*.yaml and reference .meta: version read, refused when newer, written") {
    const fs::path dir = freshDirT406("design");
    writeTextT406(dir / "P.xtui", std::string(R"({"version": 1, "name": "P", "id": "p", )") + kUisT406 + "}");
    std::string error;
    Result<UiProject> project = UiProject::load(dir.string(), &error);
    REQUIRE_MESSAGE(project.ok(), error);
    const fs::path design = dir / "design";
    // No version anywhere: version 1. design.yaml's own `version` is the design system's, untouched.
    writeTextT406(design / "design.yaml", "name: D\nversion: \"0.3\"\n");
    writeTextT406(design / "type.yaml", "roles:\n  - id: body\n    family: Inter\nfutureType: 1\n");
    writeTextT406(design / "tokens.yaml", "tokens:\n  --a:\n    description: x\n");
    writeTextT406(design / "references/moods/a.png.meta", "title: A\n");
    design::DesignSystem ds;
    REQUIRE_MESSAGE(design::load(project.value(), ds, nullptr, &error) == Status::kOk, error);
    CHECK(ds.info.extra["version"] == "0.3");
    CHECK_FALSE(ds.typeExtra.contains("version"));
    CHECK_FALSE(ds.tokensExtra.contains("version"));
    REQUIRE(design::save(ds, &error) == Status::kOk);
    CHECK(readTextT406(design / "type.yaml").rfind("version: 1\nroles:\n", 0) == 0);
    CHECK(readTextT406(design / "type.yaml").find("futureType: 1\n") != std::string::npos);
    CHECK(readTextT406(design / "tokens.yaml").rfind("version: 1\n", 0) == 0);
    CHECK(readTextT406(design / "references/moods/a.png.meta").rfind("version: 1\n", 0) == 0);
    CHECK(readTextT406(design / "design.yaml").find("version: \"0.3\"") != std::string::npos);
    // A second save of the same data writes the same bytes.
    const std::string typeText = readTextT406(design / "type.yaml");
    REQUIRE(design::load(project.value(), ds, nullptr, &error) == Status::kOk);
    REQUIRE(design::save(ds, &error) == Status::kOk);
    CHECK(readTextT406(design / "type.yaml") == typeText);

    // Newer: refused with the file's name.
    writeTextT406(design / "palette.yaml", "version: 2\nswatches: []\n");
    CHECK(design::load(project.value(), ds, nullptr, &error) != Status::kOk);
    CHECK(error.find("palette.yaml") != std::string::npos);
    CHECK(error.find("newer XTFrame") != std::string::npos);
    fs::remove(design / "palette.yaml");
    writeTextT406(design / "references/moods/a.png.meta", "version: 3\ntitle: A\n");
    CHECK(design::load(project.value(), ds, nullptr, &error) != Status::kOk);
    CHECK(error.find("newer XTFrame") != std::string::npos);
}

TEST_CASE("T-406 manifest/assets.yaml: missing version = 1, newer refused") {
    AssetsManifest manifest;
    std::string error;
    CHECK(AssetsManifest::parse("assets: []\n", manifest, &error));
    CHECK(AssetsManifest::parse("version: 1\nassets: []\n", manifest, &error));
    CHECK_FALSE(AssetsManifest::parse("version: 2\nassets: []\n", manifest, &error));
    CHECK(error.find("newer XTFrame") != std::string::npos);
    CHECK(AssetsManifest{}.toYaml().find("\nversion: 1\n") != std::string::npos);
}

TEST_CASE("T-406 contracts/ and conventions/: version stamped first, newer never written over") {
    CHECK(std::string(defaultMotionSpecYaml()).find("\nversion: 1\n") != std::string::npos);
    const std::string out = conventionYamlText("# head\nzeta: 0\n", json{{"zeta", 1}});
    CHECK(out == "# head\nversion: 1\nzeta: 1\n");
    CHECK(conventionYamlText("", json{{"version", 1}, {"a", 1}}) == "version: 1\na: 1\n");
    CHECK(conventionYamlText("", json{{"version", 2}, {"a", 1}}).empty());

    const fs::path dir = freshDirT406("conventions");
    writeTextT406(dir / "P.xtui", std::string(R"({"version": 1, "name": "P", "id": "p", )") + kUisT406 + "}");
    std::string error;
    Result<UiProject> project = UiProject::load(dir.string(), &error);
    REQUIRE_MESSAGE(project.ok(), error);
    REQUIRE(putMotionSpecEntry(project.value(), "text", json{{"min_px", 14}}, &error) == Status::kOk);
    CHECK(readTextT406(dir / kMotionSpecPath).rfind("version: 1\n", 0) == 0);
    writeTextT406(dir / kMotionSpecPath, "version: 5\ntext: {min_px: 12}\n");
    CHECK(putMotionSpecEntry(project.value(), "text", json{{"min_px", 14}}, &error) == Status::kUnsupported);
    CHECK(error.find("newer XTFrame") != std::string::npos);
    CHECK(readTextT406(dir / kMotionSpecPath) == "version: 5\ntext: {min_px: 12}\n");
}
