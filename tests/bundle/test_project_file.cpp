#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "doctest.h"
#include "test_temp.h"
#include "xt/bundle/project_file.h"

/// ProjectFile: `bundle.xtproj` v2 reading, the implied single-UI bundle, and stable saving.
/// See docs/design/22-bundle.md §22.2, docs/tasks/T-90.md "test_project_file".

using namespace xt;
using namespace xt::bundle;

namespace {

std::string fixture(const std::string& name) { return std::string(XT_TEST_BUNDLE_DIR) + "/fixtures/" + name; }

/// A fresh, empty directory under the system temp dir; removed first so reruns start clean.
std::string tempDir(const std::string& name) {
    std::filesystem::path dir = xt::test::tempDir() / ("xt_bundle_" + name);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir.generic_string();
}

/// Writes `text` as `<dir>/bundle.xtproj`.
void writeProject(const std::string& dir, const std::string& text) {
    std::ofstream out(dir + "/bundle.xtproj", std::ios::binary);
    out << text;
}

std::string readAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("project_file: loads the sample bundle.xtproj") {
    std::string error = "unchanged";
    Result<ProjectFile> result = ProjectFile::load(fixture("sample"), &error);
    REQUIRE(result.ok());
    CHECK(error.empty());

    const ProjectFile& project = result.value();
    CHECK(project.version == 2);
    CHECK(project.id == "mygame");
    CHECK(project.name == "MyGame UI");
    CHECK(project.target == "engine");
    CHECK(project.previewDpr == doctest::Approx(1.0f));
    CHECK(project.rootDir == fixture("sample"));

    REQUIRE(project.uis.size() == 2);
    CHECK(project.uis[0].id == "hud");
    CHECK(project.uis[0].doc == "hud/hud.xt");
    CHECK_FALSE(project.uis[0].flow.empty());
    CHECK(project.uis[0].size.x == doctest::Approx(1280.0f));
    CHECK(project.uis[0].size.y == doctest::Approx(720.0f));
    REQUIRE(project.uis[0].tags.size() == 1);
    CHECK(project.uis[0].tags[0] == "ingame");
    CHECK(project.uis[1].id == "pause");
    CHECK(project.uis[1].flow.empty());

    CHECK(project.shared.assetRoot == "shared");
    REQUIRE(project.shared.styles.size() == 1);
    CHECK(project.shared.styles[0] == "shared/base.xts");
    REQUIRE(project.shared.fontDirs.size() == 1);
    CHECK(project.shared.fontDirs[0] == "shared/fonts");
    REQUIRE(project.shared.templateDirs.size() == 1);
    CHECK(project.shared.templateDirs[0] == "shared/fx");

    CHECK_FALSE(project.publish.embedImages);
    CHECK_FALSE(project.publish.embedFonts);
    CHECK(project.publish.thumbnail.x == doctest::Approx(320.0f));

    REQUIRE(project.ui("pause") != nullptr);
    CHECK(project.ui("pause")->doc == "menus/pause.xt");
    CHECK(project.ui("nope") == nullptr);
}

TEST_CASE("project_file: rejects malformed projects") {
    const std::string dir = tempDir("bad");

    SUBCASE("missing directory") {
        std::string error;
        Result<ProjectFile> result = ProjectFile::load(dir + "/nope", &error);
        CHECK(result.status() == Status::kNotFound);
        CHECK_FALSE(error.empty());
    }
    SUBCASE("missing id") {
        writeProject(dir, R"({"version": 2, "uis": [{"id": "hud", "doc": "hud.xt"}]})");
        std::string error;
        Result<ProjectFile> result = ProjectFile::load(dir, &error);
        CHECK(result.status() == Status::kInvalidArgument);
        CHECK_FALSE(error.empty());
    }
    SUBCASE("invalid id") {
        writeProject(dir, R"({"version": 2, "id": "My Game", "uis": [{"id": "hud", "doc": "hud.xt"}]})");
        std::string error;
        Result<ProjectFile> result = ProjectFile::load(dir, &error);
        CHECK(result.status() == Status::kInvalidArgument);
        CHECK_FALSE(error.empty());
    }
    SUBCASE("duplicate UI id") {
        writeProject(dir, R"({"version": 2, "id": "game",
                              "uis": [{"id": "hud", "doc": "a.xt"}, {"id": "hud", "doc": "b.xt"}]})");
        std::string error;
        Result<ProjectFile> result = ProjectFile::load(dir, &error);
        CHECK(result.status() == Status::kInvalidArgument);
        CHECK_FALSE(error.empty());
    }
    SUBCASE("missing uis") {
        writeProject(dir, R"({"version": 2, "id": "game"})");
        std::string error;
        Result<ProjectFile> result = ProjectFile::load(dir, &error);
        CHECK(result.status() == Status::kInvalidArgument);
        CHECK_FALSE(error.empty());
    }
    SUBCASE("version 1") {
        writeProject(dir, R"({"version": 1, "id": "game", "uis": []})");
        std::string error;
        Result<ProjectFile> result = ProjectFile::load(dir, &error);
        CHECK(result.status() == Status::kUnsupported);
        CHECK_FALSE(error.empty());
    }
    SUBCASE("not JSON") {
        writeProject(dir, "not json at all");
        std::string error;
        Result<ProjectFile> result = ProjectFile::load(dir, &error);
        CHECK(result.status() == Status::kIoError);
        CHECK_FALSE(error.empty());
    }
}

TEST_CASE("project_file: implied single-UI bundle") {
    const ProjectFile project = ProjectFile::implied("x/y/menu.xt");
    CHECK(project.version == 2);
    CHECK(project.id == "menu");
    CHECK(project.name == "menu");
    CHECK(project.rootDir == "x/y");
    CHECK(project.shared.assetRoot.empty());
    REQUIRE(project.uis.size() == 1);
    CHECK(project.uis[0].id == "menu");
    CHECK(project.uis[0].doc == "menu.xt");

    // Backslashes, uppercase and illegal characters: the stem becomes a valid stable id.
    const ProjectFile windowsPath = ProjectFile::implied(R"(C:\games\My Game.xt)");
    CHECK(windowsPath.id == "my-game");
    CHECK(windowsPath.rootDir == "C:/games");
    REQUIRE(windowsPath.uis.size() == 1);
    CHECK(windowsPath.uis[0].doc == "My Game.xt");

    // No directory at all: rootDir is empty and save() refuses.
    const ProjectFile bare = ProjectFile::implied("solo.xt");
    CHECK(bare.rootDir.empty());
    CHECK(bare.save(nullptr) == Status::kInvalidArgument);
}

TEST_CASE("project_file: save round-trips and is byte-stable") {
    Result<ProjectFile> loaded = ProjectFile::load(fixture("sample"), nullptr);
    REQUIRE(loaded.ok());

    const std::string dir = tempDir("save");
    ProjectFile project = loaded.value();
    project.rootDir = dir;
    std::string error = "unchanged";
    REQUIRE(project.save(&error) == Status::kOk);
    CHECK(error.empty());

    const std::string first = readAll(dir + "/bundle.xtproj");
    CHECK_FALSE(first.empty());
    CHECK(first.find('\r') == std::string::npos);
    CHECK(first.back() == '\n');
    REQUIRE(project.save(nullptr) == Status::kOk);
    CHECK(readAll(dir + "/bundle.xtproj") == first);

    Result<ProjectFile> reloaded = ProjectFile::load(dir, nullptr);
    REQUIRE(reloaded.ok());
    const ProjectFile& b = reloaded.value();
    CHECK(b.id == project.id);
    CHECK(b.name == project.name);
    CHECK(b.target == project.target);
    CHECK(b.previewDpr == doctest::Approx(project.previewDpr));
    CHECK(b.shared.assetRoot == project.shared.assetRoot);
    CHECK(b.shared.fontDirs == project.shared.fontDirs);
    CHECK(b.shared.styles == project.shared.styles);
    CHECK(b.shared.templateDirs == project.shared.templateDirs);
    CHECK(b.publish.embedImages == project.publish.embedImages);
    CHECK(b.publish.embedFonts == project.publish.embedFonts);
    CHECK(b.publish.thumbnail.x == doctest::Approx(project.publish.thumbnail.x));
    CHECK(b.publish.thumbnail.y == doctest::Approx(project.publish.thumbnail.y));
    REQUIRE(b.uis.size() == project.uis.size());
    for (size_t i = 0; i < b.uis.size(); ++i) {
        CHECK(b.uis[i].id == project.uis[i].id);
        CHECK(b.uis[i].doc == project.uis[i].doc);
        CHECK(b.uis[i].flow == project.uis[i].flow);
        CHECK(b.uis[i].tags == project.uis[i].tags);
        CHECK(b.uis[i].size.x == doctest::Approx(project.uis[i].size.x));
        CHECK(b.uis[i].size.y == doctest::Approx(project.uis[i].size.y));
    }
}

TEST_CASE("project_file: fit defaults to native, round-trips, and native is not written (T-156)") {
    Result<ProjectFile> loaded = ProjectFile::load(fixture("sample"), nullptr);
    REQUIRE(loaded.ok());
    ProjectFile project = loaded.value();
    REQUIRE(project.uis.size() == 2);
    CHECK(project.uis[0].fit == "native");
    CHECK(project.uis[1].fit == "native");
    CHECK(ProjectFile::implied("board.xt").uis[0].fit == "native");

    const std::string dir = tempDir("fit");
    project.rootDir = dir;
    REQUIRE(project.save(nullptr) == Status::kOk);
    CHECK(readAll(dir + "/bundle.xtproj").find("\"fit\"") == std::string::npos);

    project.uis[0].fit = "contain";
    REQUIRE(project.save(nullptr) == Status::kOk);
    const std::string text = readAll(dir + "/bundle.xtproj");
    CHECK(text.find("\"fit\": \"contain\"") != std::string::npos);
    CHECK(text.find("\"fit\"") == text.rfind("\"fit\""));

    Result<ProjectFile> reloaded = ProjectFile::load(dir, nullptr);
    REQUIRE(reloaded.ok());
    CHECK(reloaded.value().uis[0].fit == "contain");
    CHECK(reloaded.value().uis[1].fit == "native");
    REQUIRE(reloaded.value().save(nullptr) == Status::kOk);
    CHECK(readAll(dir + "/bundle.xtproj") == text);

    writeProject(
        dir,
        "{\"version\": 2, \"id\": \"b\", \"uis\": [{\"id\": \"a\", \"doc\": \"a.xt\", \"fit\": \"cover\"},"
        " {\"id\": \"c\", \"doc\": \"c.xt\", \"fit\": \"stretch\"}, {\"id\": \"d\", \"doc\": \"d.xt\", "
        "\"fit\": \"center\"}]}");
    Result<ProjectFile> modes = ProjectFile::load(dir, nullptr);
    REQUIRE(modes.ok());
    CHECK(modes.value().uis[0].fit == "cover");
    CHECK(modes.value().uis[1].fit == "stretch");
    CHECK(modes.value().uis[2].fit == "center");
}
