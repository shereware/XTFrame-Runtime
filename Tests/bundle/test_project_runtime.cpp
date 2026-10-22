#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "test_temp.h"
#include "xt/bundle/project_runtime.h"
#include "xt/bundle/ui_project.h"
#include "xt/compile/compile.h"
#include "xt/compile/utf8_path.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/text_shaper.h"

/// The whole-project runtime's other UIs load hidden (Docs/design/17-editor.md §17.33.2,
/// Docs/tasks/T-373.md): no `on:show` runs until something shows them. T-374 (27 §27.2.4): a
/// global flow that drives each UI's root `div` through `on:port(active)="if eq(1) show() else
/// hide()"` shows and hides the surfaces themselves.

using namespace xt;

namespace {

namespace fs = std::filesystem;

void writeFile(const fs::path& path, const char* text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << text;
}

/// A fresh temp directory `name` (removed first).
fs::path freshRoot(const char* name) {
    const fs::path root = xt::test::tempDir() / name;
    std::error_code ec;
    fs::remove_all(root, ec);
    return root;
}

/// UIs a (the shown one) and b, whose `<body>` plays its `intro` timeline on `show`.
std::string writeProject(const char* name) {
    const fs::path root = freshRoot(name);
    writeFile(root / "T373.xtui", R"({"version": 1, "id": "t373", "name": "T373",
  "uis": [ { "id": "a", "doc": "a/a.xt" }, { "id": "b", "doc": "b/b.xt" } ],
  "shared": { "styles": [] } })");
    writeFile(root / "a" / "a.xt", R"(<xt><body id="Root"><div id="A"></div></body></xt>)");
    writeFile(root / "b" / "b.xt", R"~(<xt><head><style>
#Title { width: 100px; height: 20px; }
@timeline intro { duration: 1s; #Title { 0s { opacity: 0 } 1s { opacity: 1 } } }
</style></head>
<body id="Root" on:show="play(intro)"><span id="Title">t</span></body></xt>)~");
    return compile::pathToUtf8(root);
}

struct Loaded {
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    bundle::ProjectRuntime runtime;
    std::vector<bundle::ProjectRuntimeProblem> problems;
};

/// loadProjectRuntime() over one Context (as xtframe-view --project and the editor's Demo space).
void load(Loaded& loaded, const std::string& dir, bool othersStartHidden) {
    Result<bundle::UiProject> project = bundle::UiProject::load(dir);
    REQUIRE(project.ok());
    const bundle::ProjectRuntimeSources sources = bundle::projectRuntimeSources(project.value(), "a");
    bundle::ProjectRuntimeOptions options;
    options.compile.rootDir = dir;
    options.othersStartHidden = othersStartHidden;
    const compile::CompileResult a = compile::compileDocument(dir + "/a/a.xt", options.compile);
    REQUIRE(a.ok());
    Result<DocumentHandle> current = loaded.ctx.loadDocument(IrBlob{a.ir.data(), a.ir.size()}, true);
    REQUIRE(current.ok());
    REQUIRE(bundle::loadProjectRuntime(sources, options, loaded.ctx, current.value(), loaded.ctx,
                                       loaded.runtime, loaded.problems) == Status::kOk);
    REQUIRE(loaded.runtime.others.size() == 1);
    CHECK(loaded.ctx.isVisible(current.value()));  // The shown UI is the host's: untouched.
}

}  // namespace

TEST_CASE("T-373 loadProjectRuntime: other UIs load hidden; their on:show waits for requestShow()") {
    const std::string dir = writeProject("xt_t373_others_hidden");
    Loaded loaded;
    load(loaded, dir, /*othersStartHidden=*/true);
    const DocumentHandle b = loaded.runtime.others[0].second;
    Document* docB = loaded.ctx.document(b);
    REQUIRE(docB != nullptr);
    CHECK_FALSE(loaded.ctx.isVisible(b));
    CHECK_FALSE(loaded.ctx.isHiding(b));
    loaded.ctx.update(0.0f);
    loaded.ctx.update(0.05f);
    CHECK_FALSE(loaded.ctx.isVisible(b));
    CHECK_FALSE(docB->isTimelinePlaying("", "intro"));

    REQUIRE(loaded.ctx.requestShow(b) == Status::kOk);
    CHECK(loaded.ctx.isVisible(b));
    CHECK(docB->isTimelinePlaying("", "intro"));
    loaded.ctx.update(0.1f);
    const float t = docB->timelineTime("", "intro");
    CHECK(t > 0.0f);
    CHECK(t < 0.2f);
}

TEST_CASE("T-373 loadProjectRuntime: othersStartHidden = false keeps the visible load") {
    const std::string dir = writeProject("xt_t373_others_visible");
    Loaded loaded;
    load(loaded, dir, /*othersStartHidden=*/false);
    const DocumentHandle b = loaded.runtime.others[0].second;
    CHECK(loaded.ctx.isVisible(b));
    loaded.ctx.update(0.0f);
    REQUIRE(loaded.ctx.document(b) != nullptr);
    CHECK(loaded.ctx.document(b)->isTimelinePlaying("", "intro"));
}

TEST_CASE("T-373 Context::setVisible: no hide / show triggers; unknown handle refused") {
    const fs::path root = freshRoot("xt_t373_set_visible");
    writeFile(
        root / "s.xt",
        R"~(<xt><body id="Root" on:show="class(shown)" on:hide="class(hid)"><div id="X"></div></body></xt>)~");
    compile::CompileOptions options;
    options.rootDir = compile::pathToUtf8(root);
    const compile::CompileResult r = compile::compileDocument(compile::pathToUtf8(root / "s.xt"), options);
    REQUIRE(r.ok());
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    Result<DocumentHandle> h = ctx.loadDocument(IrBlob{r.ir.data(), r.ir.size()}, true);
    REQUIRE(h.ok());
    Document* doc = ctx.document(h.value());
    REQUIRE(doc != nullptr);
    REQUIRE(ctx.setVisible(h.value(), false) == Status::kOk);
    CHECK_FALSE(ctx.isVisible(h.value()));
    ctx.update(0.0f);
    CHECK_FALSE(doc->byId("Root").hasClass("hid"));
    CHECK_FALSE(doc->byId("Root").hasClass("shown"));
    REQUIRE(ctx.requestShow(h.value()) == Status::kOk);
    CHECK(doc->byId("Root").hasClass("shown"));
    CHECK(ctx.setVisible(0, false) == Status::kInvalidArgument);
    CHECK(ctx.setVisible(99, true) == Status::kInvalidArgument);
}

namespace {

/// T-374: one UI of the screen-switch project, as the user's project writes it: the root `div`
/// shows / hides itself from its `active` port and plays `intro` on show.
std::string screenUi(const char* id) {
    return std::string(R"~(<xt><head><style>
#Title { width: 100px; height: 20px; }
@timeline intro { duration: 1s; #Title { 0s { opacity: 0 } 1s { opacity: 1 } } }
</style></head>
<body id="Root"><div id=")~") +
           id +
           R"~(" port:active="bool in = true" port:open_options="event out" port:back="event out"
 on:port(active)="if eq(1) show() else hide(); if eq(1) play(intro)" on:show="play(intro)"><span id="Title">t</span></div></body></xt>)~";
}

/// T-374: main-menu (the shown UI), settings and hud; the global flow's `screen` radio (initial 0)
/// sends each UI's `active` through a lookup, `open_options` selects settings, `back` main-menu.
std::string writeScreenProject(const char* name) {
    const fs::path root = freshRoot(name);
    writeFile(root / "T374.xtui", R"({"version": 1, "id": "t374", "name": "T374",
  "uis": [ { "id": "main-menu", "doc": "main-menu/main-menu.xt" },
           { "id": "settings", "doc": "settings/settings.xt" },
           { "id": "hud", "doc": "hud/hud.xt" } ],
  "shared": { "styles": [] } })");
    for (const char* id : {"main-menu", "settings", "hud"}) {
        writeFile(root / id / (std::string(id) + ".xt"), screenUi(id).c_str());
    }
    writeFile(root / "bundle.xtflow", R"({"version": 1,
  "nodes": [
    { "id": "screen", "type": "radio", "params": { "count": 3, "initial": 0 } },
    { "id": "show-main-menu", "type": "lookup", "params": { "values": [true, false, false] } },
    { "id": "show-settings", "type": "lookup", "params": { "values": [false, true, false] } },
    { "id": "show-hud", "type": "lookup", "params": { "values": [false, false, true] } } ],
  "edges": [
    { "from": "ui#main-menu.open_options", "to": "screen.set1" },
    { "from": "ui#settings.back", "to": "screen.set0" },
    { "from": "screen.index", "to": "show-main-menu.index" },
    { "from": "screen.index", "to": "show-settings.index" },
    { "from": "screen.index", "to": "show-hud.index" },
    { "from": "show-main-menu.out", "to": "ui#main-menu.active" },
    { "from": "show-settings.out", "to": "ui#settings.active" },
    { "from": "show-hud.out", "to": "ui#hud.active" } ] })");
    return compile::pathToUtf8(root);
}

struct VisibilityLog : ContextListener {
    std::vector<std::string> log;
    void onShown(DocumentHandle d) override { log.push_back("shown#" + std::to_string(d)); }
    void onHidden(DocumentHandle d) override { log.push_back("hidden#" + std::to_string(d)); }
    size_t count(const std::string& entry) const {
        return static_cast<size_t>(std::count(log.begin(), log.end(), entry));
    }
};

}  // namespace

TEST_CASE("T-374 global flow: a radio drives the UIs' root div show() / hide() to the surfaces") {
    const std::string dir = writeScreenProject("xt_t374_screen_flow");
    Result<bundle::UiProject> project = bundle::UiProject::load(dir);
    REQUIRE(project.ok());
    const bundle::ProjectRuntimeSources sources = bundle::projectRuntimeSources(project.value(), "main-menu");
    bundle::ProjectRuntimeOptions options;
    options.compile.rootDir = dir;
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    VisibilityLog vis;
    ctx.addListener(&vis);
    const compile::CompileResult menu =
        compile::compileDocument(dir + "/main-menu/main-menu.xt", options.compile);
    REQUIRE(menu.ok());
    Result<DocumentHandle> current = ctx.loadDocument(IrBlob{menu.ir.data(), menu.ir.size()}, true);
    REQUIRE(current.ok());
    bundle::ProjectRuntime runtime;
    std::vector<bundle::ProjectRuntimeProblem> problems;
    REQUIRE(bundle::loadProjectRuntime(sources, options, ctx, current.value(), ctx, runtime, problems) ==
            Status::kOk);
    REQUIRE(runtime.bundle != nullptr);
    REQUIRE(runtime.others.size() == 2);
    const DocumentHandle mainMenu = current.value();
    const DocumentHandle settings = runtime.others[0].second;
    const DocumentHandle hud = runtime.others[1].second;
    REQUIRE(runtime.others[0].first == "settings");

    for (int i = 0; i < 3; ++i) {
        runtime.bundle->tick(1.0f / 60.0f);
        ctx.update(static_cast<float>(i) / 60.0f);
    }
    CHECK(ctx.isVisible(mainMenu));
    CHECK_FALSE(ctx.isVisible(settings));  // active = false from the radio's initial 0.
    CHECK_FALSE(ctx.isVisible(hud));
    CHECK(vis.log.empty());
    Document* settingsDoc = ctx.document(settings);
    REQUIRE(settingsDoc != nullptr);
    CHECK_FALSE(settingsDoc->isTimelinePlaying("", "intro"));

    // main-menu's `open_options` -> screen.set1: settings shows, main-menu hides.
    Document* menuDoc = ctx.document(mainMenu);
    REQUIRE(menuDoc != nullptr);
    REQUIRE(menuDoc->setPort(menuDoc->lookupPort("open_options"), Value::ofFloat(1)) == Status::kOk);
    for (int i = 0; i < 3; ++i) {
        runtime.bundle->tick(1.0f / 60.0f);
        ctx.update(0.1f + static_cast<float>(i) / 60.0f);
    }
    CHECK(ctx.isVisible(settings));
    CHECK_FALSE(ctx.isVisible(mainMenu));
    CHECK_FALSE(ctx.isVisible(hud));
    CHECK(vis.count("shown#" + std::to_string(settings)) == 1);
    CHECK(vis.count("hidden#" + std::to_string(mainMenu)) == 1);
    CHECK(vis.log.size() == 2);
    CHECK(settingsDoc->isTimelinePlaying("", "intro"));
    CHECK(menuDoc->lastHideSource() == "port active");
    ctx.removeListener(&vis);
    runtime.reset();
}
