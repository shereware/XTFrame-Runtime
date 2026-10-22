#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "doctest.h"
#include "test_temp.h"
#include "xt/bundle/project_edit.h"
#include "xt/bundle/ui_project.h"

/// T-371 (Docs/design/37-work-model.md §37.8.4): the UI Project writers the editor's panels and the
/// Editor Service ops share (xt/bundle/project_edit.h).

using namespace xt;
using namespace xt::bundle;
namespace fs = std::filesystem;

namespace {

void writeTextT371(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

std::string readTextT371(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

UiProject freshProject(const char* name) {
    const fs::path root = xt::test::tempDir() / name;
    fs::remove_all(root);
    writeTextT371(root / "P.xtui", R"({"version": 1, "name": "P", "id": "p",
        "uis": [{"id": "hud", "doc": "ui/hud/hud.xt"}, {"id": "menu", "doc": "ui/menu/menu.xt"}]})");
    writeTextT371(root / "ui" / "hud" / "hud.xt", kBlankUiDocument);
    writeTextT371(root / "ui" / "menu" / "menu.xt", kBlankUiDocument);
    Result<UiProject> project = UiProject::load(root.string());
    REQUIRE(project.ok());
    return std::move(project.value());
}

}  // namespace

TEST_CASE("T-371 UI list writers: add, rename (global flow references), remove") {
    UiProject project = freshProject("xt_t371_bundle_uis");
    const fs::path root = project.rootDir;
    CHECK(defaultUiDocPath(project, "pause") == "ui/pause/pause.xt");
    REQUIRE(addProjectUi(project, "pause", "ui/pause/pause.xt", kBlankUiDocument) == Status::kOk);
    CHECK(fs::exists(root / "ui" / "pause" / "pause.xt"));
    CHECK(UiProject::load(project.rootDir).value().ui("pause") != nullptr);
    std::string error;
    CHECK(addProjectUi(project, "pause", "x.xt", "", &error) == Status::kInvalidArgument);
    CHECK(addProjectUi(project, "other", "/abs.xt", "", &error) == Status::kInvalidArgument);
    CHECK(addProjectUi(project, "other", "ui/hud/hud.xt", "", &error) == Status::kInvalidArgument);

    writeTextT371(root / "bundle.xtflow",
                  R"({"nodes": [{"id": "n1", "type": "counter", "params": {"step": 1}}],
        "edges": [{"from": "ui#hud.shown", "to": "n1.increment"}, {"from": "ui#hudx.shown", "to": "n1.reset"}]})");
    std::vector<std::string> rewritten;
    REQUIRE(renameProjectUi(project, "hud", "game-hud", &rewritten) == Status::kOk);
    CHECK(rewritten == std::vector<std::string>{"bundle.xtflow"});
    const std::string flow = readTextT371(root / "bundle.xtflow");
    CHECK(flow.find("ui#game-hud.shown") != std::string::npos);
    CHECK(flow.find("ui#hudx.shown") != std::string::npos);  // Another id that only starts the same.
    CHECK(project.ui("game-hud") != nullptr);
    CHECK(renameProjectUi(project, "nope", "x") == Status::kInvalidArgument);
    CHECK(renameProjectUi(project, "menu", "pause") == Status::kInvalidArgument);  // Taken.

    REQUIRE(removeProjectUi(project, "pause") == Status::kOk);
    REQUIRE(removeProjectUi(project, "menu") == Status::kOk);
    CHECK(removeProjectUi(project, "game-hud", &error) == Status::kInvalidArgument);  // The last one.
    CHECK(UiProject::load(project.rootDir).value().uis.size() == 1);
}

TEST_CASE("T-371 project settings are checked field by field") {
    UiProject project = freshProject("xt_t371_bundle_settings");
    ProjectSettings s = projectSettings(project);
    CHECK(validateProjectSettings(s).empty());
    s.spec.safeAreaDefault = 0.6f;
    CHECK(validateProjectSettings(s).rfind("spec.safeArea.default", 0) == 0);
    s = projectSettings(project);
    s.id = "Bad Id";
    CHECK(validateProjectSettings(s).rfind("id:", 0) == 0);
    s = projectSettings(project);
    s.spec.responsive.scale = "clamp(0.5, 2)";
    CHECK(validateProjectSettings(s).empty());
    s.spec.responsive.scale = "clamp(2, 0.5)";
    CHECK_FALSE(validateProjectSettings(s).empty());
    s = projectSettings(project);
    s.spec.responsive.visibility = "maybe";
    CHECK_FALSE(validateProjectSettings(s).empty());
    s = projectSettings(project);
    s.spec.uiscaleLimits.clamped = true;
    s.spec.uiscaleLimits.clampMin = 2.0f;
    s.spec.uiscaleLimits.clampMax = 1.0f;
    CHECK_FALSE(validateProjectSettings(s).empty());

    const std::string before = readTextT371(fs::path(project.path()));
    s = projectSettings(project);
    s.spec.players = 0;
    std::string error;
    CHECK(setProjectSettings(project, s, &error) == Status::kInvalidArgument);
    CHECK(readTextT371(fs::path(project.path())) == before);
    s.spec.players = 4;
    s.name = "Renamed";
    REQUIRE(setProjectSettings(project, s) == Status::kOk);
    CHECK(UiProject::load(project.rootDir).value().spec.players == 4);
}

TEST_CASE("T-371 convention YAML keeps the leading comments and the key order") {
    nlohmann::json doc = {{"zeta", 1}, {"alpha", {{"b", 2}, {"a", 3}}}, {"new", true}};
    const std::string out = conventionYamlText("# head\n# two\nzeta: 0\nalpha:\n  b: 0\n  a: 0\n", doc);
    CHECK(out.rfind("# head\n# two\n", 0) == 0);
    CHECK(out.find("zeta") < out.find("alpha"));
    CHECK(out.find("b: 2") < out.find("a: 3"));
    CHECK(out.find("new") > out.find("alpha"));

    UiProject project = freshProject("xt_t371_bundle_conventions");
    REQUIRE(putMotionSpecEntry(project, "text", nlohmann::json{{"min_px", 14}}) == Status::kOk);
    CHECK(readTextT371(fs::path(project.rootDir) / kMotionSpecPath).find("min_px: 14") != std::string::npos);
    CHECK(isValidLayoutTemplateName("Two column"));
    CHECK_FALSE(isValidLayoutTemplateName("../x"));
    REQUIRE(saveLayoutTemplate(project, "Grid", "<xt/>", false) == Status::kOk);
    CHECK(saveLayoutTemplate(project, "Grid", "<xt/>", false) == Status::kInvalidArgument);
    CHECK(saveLayoutTemplate(project, "Grid", "<xt></xt>", true) == Status::kOk);
    REQUIRE(writeAgents(project, "# A\n") == Status::kOk);
    CHECK(readTextT371(agentsPath(project)) == "# A\n");
}
