#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../../Code/Tools/view/view_flow.h"
#include "doctest.h"
#include "test_temp.h"
#include "xt/compile/compile.h"
#include "xt/compile/utf8_path.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/event.h"
#include "xt/flow/bundle_context.h"
#include "xt/flow/flow_graph.h"
#include "xt/flow/flow_runtime.h"
#include "xt/input.h"
#include "xt/text_shaper.h"

/// T-260b: the flow runtimes against Documents that were replaced (reloadDocument) or unloaded,
/// BundleContext::removeUi, what an unloading Document can still reach through the global flow,
/// and the hosts' "unload, check, load" sequences run from inside callbacks
/// (Docs/reviews/2026-10-02-full-audit-raw/F.md F2; Docs/tasks/T-260b.md).

using namespace xt;
using namespace xt::flow;

namespace {

constexpr const char* kHud = R"X(<xt><head><style>
#Title { width: 100px; height: 20px; } #Count { width: 40px; height: 20px; }
</style></head>
<body id="Root"><div id="Hud" port:hp="number in">
<span id="Title">t</span><span id="Count">0</span></div></body></xt>)X";

constexpr const char* kMenu = R"X(<xt><head><style>
#Play { position: absolute; left: 0px; top: 0px; width: 60px; height: 30px; }
</style></head>
<body id="Root"><button id="Play">p</button></body></xt>)X";

constexpr const char* kPlate = R"X(<xt><head><style>
#Btn { position: absolute; left: 0px; top: 0px; width: 60px; height: 30px; }
</style></head>
<body id="Root" on:show="class(shown)"><button id="Btn">b</button></body></xt>)X";

/// Emits `bye` (a host event) and then sets its `out` port in one trigger chain.
constexpr const char* kSender = R"X(<xt><head><style>
#Go { position: absolute; left: 0px; top: 0px; width: 60px; height: 30px; }
</style></head>
<body id="Root" port:out="number out"><div id="Go" on:click="emit(bye); set(out, 5)"></div></body></xt>)X";

/// Counts clicks on the hud's #Title and the menu's #Play into the hud's #Count.
constexpr const char* kCountFlow = R"({"version":1,"nodes":[
    {"id":"c","type":"counter"},{"id":"s","type":"toString"}],
  "edges":[
    {"from":"ui#hud.Title.Click","to":"c.increment"},
    {"from":"ui#menu.Play.Click","to":"c.increment"},
    {"from":"c.value","to":"s.in"},
    {"from":"s.out","to":"ui#hud.Count.setText"}]})";

/// A UI's own flow: clicks on #Title counted into #Count.
constexpr const char* kOwnFlow = R"({"version":1,"nodes":[
    {"id":"c","type":"counter"},{"id":"s","type":"toString"}],
  "edges":[
    {"from":"#Title.Click","to":"c.increment"},
    {"from":"c.value","to":"s.in"},
    {"from":"s.out","to":"#Count.setText"}]})";

std::vector<uint8_t> compileUi(const char* name, const char* text) {
    const compile::SourceFile file{name, text};
    compile::CompileOptions options;
    compile::CompileResult result = compile::compileDocumentFromStrings({&file, 1}, name, options);
    for (const compile::Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    return std::move(result.ir);
}

DocumentHandle loadUi(Context& ctx, const char* name, const char* text) {
    std::vector<uint8_t> ir = compileUi(name, text);
    Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{ir.data(), ir.size()}, true);
    REQUIRE(handle.ok());
    return handle.value();
}

/// A click straight into `doc` (Document::dispatchInput: a dispatching call of its own).
void click(Document& doc, const char* id) {
    Rect box = doc.byId(id).boundingBox();
    InputEvent ev;
    ev.kind = InputEvent::PointerDown;
    ev.pos = Vec2{box.x + box.w * 0.5f, box.y + box.h * 0.5f};
    ev.button = PointerButton::Left;
    doc.dispatchInput(ev);
    ev.kind = InputEvent::PointerUp;
    doc.dispatchInput(ev);
}

/// A click queued on the Context at `doc`'s element `id` (delivered by the next update()).
void queueClick(Context& ctx, Document& doc, const char* id) {
    Rect box = doc.byId(id).boundingBox();
    InputEvent ev;
    ev.kind = InputEvent::PointerDown;
    ev.pos = Vec2{box.x + box.w * 0.5f, box.y + box.h * 0.5f};
    ev.button = PointerButton::Left;
    ctx.pushInput(ev);
    ev.kind = InputEvent::PointerUp;
    ctx.pushInput(ev);
}

std::string joined(const std::vector<std::string>& errors) {
    std::string out;
    for (const std::string& e : errors) {
        out += e + "\n";
    }
    return out;
}

/// Runs the test's callbacks from inside the Context's dispatch.
struct HostListener : ContextListener {
    std::function<void(const HostEvent&)> host;
    void onHostEvent(const HostEvent& e) override {
        if (host) {
            host(e);
        }
    }
};

/// A hud and a menu (loaded in that order, so the menu is the top surface), registered on a
/// BundleContext.
struct Uis {
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    DocumentHandle hud = 0;
    DocumentHandle menu = 0;
    std::unique_ptr<BundleContext> bundle;

    Uis() {
        hud = loadUi(ctx, "hud.xt", kHud);
        menu = loadUi(ctx, "menu.xt", kMenu);
        ctx.setViewport(Rect{0, 0, 400, 300}, 1.0f);
        ctx.update(0.0f);
        bundle = std::make_unique<BundleContext>(ctx);
        REQUIRE(bundle->addUi("hud", hud) == Status::kOk);
        REQUIRE(bundle->addUi("menu", menu) == Status::kOk);
    }
    Document& hudDoc() { return *ctx.document(hud); }
    Document& menuDoc() { return *ctx.document(menu); }
    void loadGlobal(const char* json) {
        std::vector<std::string> errors;
        REQUIRE_MESSAGE(bundle->loadGlobalFlow(json, errors) == Status::kOk, joined(errors));
    }
};

}  // namespace

// -- F2: the global flow against a replaced or unloaded Document -----------------------------------

TEST_CASE("F2: reloadDocument of a registered UI, then loadGlobalFlow again") {
    Uis uis;
    uis.loadGlobal(kCountFlow);
    click(uis.hudDoc(), "Title");
    CHECK(uis.hudDoc().byId("Count").text() == "1");
    std::vector<uint8_t> ir = compileUi("hud.xt", kHud);
    REQUIRE(uis.ctx.reloadDocument(uis.hud, IrBlob{ir.data(), ir.size()}, true).ok());
    // Until the global flow is loaded again it does not reach the replaced Document at all.
    click(uis.menuDoc(), "Play");
    CHECK(uis.hudDoc().byId("Count").text() == "0");
    uis.bundle->tick(0.016f);
    uis.ctx.update(0.016f);      // Lays the reloaded hud out.
    uis.loadGlobal(kCountFlow);  // Releases the listeners it had on the Document that went away.
    click(uis.hudDoc(), "Title");
    CHECK(uis.hudDoc().byId("Count").text() == "1");
    click(uis.menuDoc(), "Play");
    CHECK(uis.hudDoc().byId("Count").text() == "2");
}

TEST_CASE("F2: unloadDocument of a registered UI, then destroy the BundleContext") {
    Uis uis;
    SUBCASE("the global flow listens on it") { uis.loadGlobal(kCountFlow); }
    SUBCASE("its own flow listens on it") {
        std::vector<std::string> errors;
        REQUIRE_MESSAGE(uis.bundle->loadUiFlow("hud", kOwnFlow, errors) == Status::kOk, joined(errors));
        click(uis.hudDoc(), "Title");
        CHECK(uis.hudDoc().byId("Count").text() == "1");
    }
    uis.ctx.unloadDocument(uis.hud);
    uis.bundle->tick(0.016f);
    uis.bundle.reset();  // The runtimes unbind; the hud's Document is gone already.
    CHECK(uis.ctx.document(uis.menu) != nullptr);
    uis.ctx.update(0.016f);
}

TEST_CASE("F2: a UI's own flow loaded again after its Document was reloaded") {
    Uis uis;
    std::vector<std::string> errors;
    REQUIRE_MESSAGE(uis.bundle->loadUiFlow("hud", kOwnFlow, errors) == Status::kOk, joined(errors));
    std::vector<uint8_t> ir = compileUi("hud.xt", kHud);
    REQUIRE(uis.ctx.reloadDocument(uis.hud, IrBlob{ir.data(), ir.size()}, true).ok());
    uis.bundle->tick(0.016f);  // Polls nothing from the replaced Document.
    REQUIRE_MESSAGE(uis.bundle->loadUiFlow("hud", kOwnFlow, errors) == Status::kOk, joined(errors));
    uis.ctx.update(0.016f);  // Lays the reloaded Document out.
    click(uis.hudDoc(), "Title");
    FlowRuntime* own = uis.bundle->uiFlowRuntime(uis.hud);
    REQUIRE(own != nullptr);
    CHECK(own->values()[0].n == doctest::Approx(1));  // c.value
    CHECK(uis.hudDoc().byId("Count").text() == "1");
}

TEST_CASE("F2: after removeUi a UI's events no longer reach the global flow") {
    Uis uis;
    uis.loadGlobal(kCountFlow);
    click(uis.menuDoc(), "Play");
    CHECK(uis.hudDoc().byId("Count").text() == "1");
    SUBCASE("removed while still loaded") {
        REQUIRE(uis.bundle->removeUi("menu") == Status::kOk);
        click(uis.menuDoc(), "Play");
        CHECK(uis.hudDoc().byId("Count").text() == "1");
        click(uis.hudDoc(), "Title");  // The hud is still bound.
        CHECK(uis.hudDoc().byId("Count").text() == "2");
        uis.ctx.unloadDocument(uis.menu);
    }
    SUBCASE("removed after unloadDocument") {
        uis.ctx.unloadDocument(uis.menu);
        REQUIRE(uis.bundle->removeUi("menu") == Status::kOk);
    }
    SUBCASE("removed after a reload") {
        std::vector<uint8_t> ir = compileUi("menu.xt", kMenu);
        REQUIRE(uis.ctx.reloadDocument(uis.menu, IrBlob{ir.data(), ir.size()}, true).ok());
        REQUIRE(uis.bundle->removeUi("menu") == Status::kOk);
        uis.ctx.update(0.016f);  // Lays the reloaded menu out.
        click(uis.menuDoc(), "Play");
        CHECK(uis.hudDoc().byId("Count").text() == "1");
    }
    CHECK(uis.bundle->removeUi("menu") == Status::kInvalidArgument);
    CHECK(uis.bundle->uiIds() == std::vector<std::string>{"hud"});
    CHECK(uis.bundle->uiOf(uis.menu).empty());
    uis.ctx.update(0.016f);
    uis.bundle->tick(0.016f);
    uis.loadGlobal(R"({"version":1,"nodes":[{"id":"c","type":"counter"}],
      "edges":[{"from":"ui#hud.Title.Click","to":"c.increment"}]})");
    uis.bundle.reset();
}

TEST_CASE("removeUi: from a UI's own flow event, together with unloading it") {
    Uis uis;
    std::vector<std::string> errors;
    REQUIRE_MESSAGE(uis.bundle->loadUiFlow("hud", R"({"version":1,"nodes":[
        {"id":"e","type":"emit","params":{"port":"close"}},{"id":"c","type":"counter"}],
      "edges":[{"from":"#Title.Click","to":"e.trigger"},{"from":"#Title.Click","to":"c.increment"}]})",
                                           errors) == Status::kOk,
                    joined(errors));
    int closes = 0;
    uis.bundle->setHostEventListener([&](const FlowHostEvent& e) {
        if (e.kind == FlowHostEventKind::Emit && e.name == "close") {
            ++closes;
            uis.ctx.unloadDocument(uis.hud);  // Deferred: the hud is dispatching.
            CHECK(uis.bundle->removeUi("hud") == Status::kOk);
        }
    });
    click(uis.hudDoc(), "Title");  // The flow's own listener runs on: its runtime is retired, not gone.
    CHECK(closes == 1);
    CHECK(uis.ctx.document(uis.hud) == nullptr);
    CHECK(uis.bundle->uiFlowRuntime(uis.hud) == nullptr);
    uis.ctx.update(0.016f);
    uis.bundle->tick(0.016f);  // Destroys the retired runtime.
    uis.bundle.reset();
}

TEST_CASE("caller audit: a UI switch (unload, check, removeUi, load, addUi) from a host callback") {
    Uis uis;
    // The menu's Play asks for the switch (a host event from its click trigger).
    std::vector<uint8_t> menuIr = compileUi("menu.xt", R"X(<xt><head><style>
#Play { position: absolute; left: 0px; top: 0px; width: 60px; height: 30px; }
</style></head><body id="Root"><button id="Play" on:click="emit(switch)">p</button></body></xt>)X");
    REQUIRE(uis.ctx.reloadDocument(uis.menu, IrBlob{menuIr.data(), menuIr.size()}, true).ok());
    uis.ctx.update(0.016f);  // Lays the reloaded menu out.
    uis.loadGlobal(kCountFlow);
    HostListener listener;
    DocumentHandle plate = 0;
    bool goneAtOnce = false;
    std::vector<uint8_t> ir = compileUi("plate.xt", kPlate);
    listener.host = [&](const HostEvent& e) {
        if (e.name != "switch") {
            return;
        }
        uis.ctx.unloadDocument(uis.menu);
        goneAtOnce = uis.ctx.document(uis.menu) == nullptr;
        CHECK(uis.bundle->removeUi("menu") == Status::kOk);
        Result<DocumentHandle> h = uis.ctx.loadDocument(IrBlob{ir.data(), ir.size()}, false);
        REQUIRE(h.ok());
        plate = h.value();
        CHECK(uis.bundle->addUi("plate", plate) == Status::kOk);
    };
    uis.ctx.addListener(&listener);
    queueClick(uis.ctx, uis.menuDoc(), "Play");
    uis.ctx.update(0.016f);
    CHECK(goneAtOnce);
    CHECK(uis.ctx.document(uis.menu) == nullptr);
    REQUIRE(plate != 0);
    REQUIRE(uis.ctx.document(plate) != nullptr);
    CHECK(uis.hudDoc().byId("Count").text() == "1");  // The click counted before the switch.
    uis.bundle->tick(0.016f);
    uis.ctx.update(0.016f);
    CHECK(uis.ctx.document(plate)->byId("Root").hasClass("shown"));
    uis.loadGlobal(R"({"version":1,"nodes":[{"id":"c","type":"counter"},{"id":"s","type":"toString"}],
      "edges":[{"from":"ui#plate.Btn.Click","to":"c.increment"},{"from":"c.value","to":"s.in"},
               {"from":"s.out","to":"ui#hud.Count.setText"}]})");
    click(*uis.ctx.document(plate), "Btn");
    CHECK(uis.hudDoc().byId("Count").text() == "1");
    uis.ctx.removeListener(&listener);
}

// -- An unloading Document reaches no other Document ----------------------------------------------

TEST_CASE("unloading: a Document that unloads itself mid-chain does not set another UI's port") {
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    const DocumentHandle hud = loadUi(ctx, "hud.xt", kHud);
    const DocumentHandle sender = loadUi(ctx, "sender.xt", kSender);  // The top surface.
    ctx.setViewport(Rect{0, 0, 400, 300}, 1.0f);
    ctx.update(0.0f);
    BundleContext bundle(ctx);
    REQUIRE(bundle.addUi("hud", hud) == Status::kOk);
    REQUIRE(bundle.addUi("sender", sender) == Status::kOk);
    std::vector<std::string> errors;
    REQUIRE_MESSAGE(bundle.loadGlobalFlow(R"({"version":1,"nodes":[],
        "edges":[{"from":"ui#sender.out","to":"ui#hud.hp"}]})",
                                          errors) == Status::kOk,
                    joined(errors));
    const PortId hp = ctx.document(hud)->lookupPort("hp");
    REQUIRE(hp != kInvalidPort);
    HostListener listener;
    bool unload = true;
    listener.host = [&](const HostEvent& e) {
        if (e.name == "bye" && unload) {
            ctx.unloadDocument(e.document);
        }
    };
    ctx.addListener(&listener);

    SUBCASE("unloaded by the emit: the later set(out) reaches nobody") {
        queueClick(ctx, *ctx.document(sender), "Go");
        ctx.update(0.016f);
        bundle.tick(0.016f);  // T-389: value ports reach the flow on its next tick().
        CHECK(ctx.document(sender) == nullptr);
        CHECK(ctx.document(hud)->portText(hp) == "0");
    }
    SUBCASE("control: without the unload the edge carries the value") {
        unload = false;
        queueClick(ctx, *ctx.document(sender), "Go");
        ctx.update(0.016f);
        bundle.tick(0.016f);  // T-389.
        CHECK(ctx.document(sender) != nullptr);
        CHECK(ctx.document(hud)->portText(hp) == "5");
    }
    ctx.removeListener(&listener);
}

// -- Caller audit ------------------------------------------------------------------------------------

TEST_CASE("caller audit: a global flow `spawn` loads and registers a UI from inside the dispatch") {
    Uis uis;
    uis.loadGlobal(R"({"version":1,"nodes":[{"id":"spw","type":"spawn","params":{"symbol":"Plate"}}],
      "edges":[{"from":"ui#menu.Play.Click","to":"spw.trigger"}]})");
    DocumentHandle plate = 0;
    std::vector<uint8_t> ir = compileUi("plate.xt", kPlate);
    uis.bundle->setHostEventListener([&](const FlowHostEvent& e) {
        if (e.kind != FlowHostEventKind::Spawn || plate != 0) {
            return;
        }
        Result<DocumentHandle> h = uis.ctx.loadDocument(IrBlob{ir.data(), ir.size()}, false);
        REQUIRE(h.ok());
        plate = h.value();
        CHECK(uis.bundle->addUi("plate", plate) == Status::kOk);
    });
    queueClick(uis.ctx, uis.menuDoc(), "Play");
    uis.ctx.update(0.016f);
    REQUIRE(plate != 0);
    REQUIRE(uis.ctx.document(plate) != nullptr);
    CHECK_FALSE(uis.ctx.document(plate)->byId("Root").hasClass("shown"));  // Updated from the next frame.
    uis.bundle->tick(0.016f);
    uis.ctx.update(0.016f);
    CHECK(uis.ctx.document(plate)->byId("Root").hasClass("shown"));
    // The global flow binds the new UI once it is loaded again (after the dispatch returned).
    uis.loadGlobal(R"({"version":1,"nodes":[{"id":"c","type":"counter"},{"id":"s","type":"toString"}],
      "edges":[{"from":"ui#plate.Btn.Click","to":"c.increment"},{"from":"c.value","to":"s.in"},
               {"from":"s.out","to":"ui#hud.Count.setText"}]})");
    click(*uis.ctx.document(plate), "Btn");
    CHECK(uis.hudDoc().byId("Count").text() == "1");
}

TEST_CASE("caller audit: a standalone flow re-loaded against a replacement before the old one goes") {
    // Code/Tools/view's R (reload): load the replacement, load the flow against it while the
    // previous Document is still loaded (FlowRuntime::load releases the previous listeners), then
    // unload the previous one.
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    ctx.setViewport(Rect{0, 0, 400, 300}, 1.0f);
    DocumentHandle handle = loadUi(ctx, "hud.xt", kHud);
    ctx.update(0.0f);
    FlowGraph graph;
    std::vector<std::string> errors;
    REQUIRE(FlowGraph::parse(kOwnFlow, graph, errors) == Status::kOk);
    FlowRuntime runtime;
    REQUIRE_MESSAGE(runtime.load(graph, *ctx.document(handle), errors) == Status::kOk, joined(errors));
    click(*ctx.document(handle), "Title");
    CHECK(ctx.document(handle)->byId("Count").text() == "1");
    const DocumentHandle previous = handle;
    handle = loadUi(ctx, "hud.xt", kHud);
    REQUIRE_MESSAGE(runtime.load(graph, *ctx.document(handle), errors) == Status::kOk, joined(errors));
    ctx.unloadDocument(previous);
    ctx.update(0.016f);
    runtime.tick(0.016f);
    click(*ctx.document(handle), "Title");
    CHECK(ctx.document(handle)->byId("Count").text() == "1");
}

// -- T-260c: load serials, deferred flow loads, load failure, BundleContext walks ---------------------

TEST_CASE("T-260c: a UI's own flow after its Document was reloaded twice, the flow not reloaded") {
    Uis uis;
    std::vector<std::string> errors;
    REQUIRE_MESSAGE(uis.bundle->loadUiFlow("hud", kOwnFlow, errors) == Status::kOk, joined(errors));
    const Document* bound = &uis.hudDoc();
    std::vector<uint8_t> ir = compileUi("hud.xt", kHud);
    // Reloaded until a new Document lands where the bound one was (the allocator normally hands
    // the freed block back from the second reload on): only its load serial tells it apart then.
    bool reused = false;
    for (int i = 0; i < 16 && !reused; ++i) {
        REQUIRE(uis.ctx.reloadDocument(uis.hud, IrBlob{ir.data(), ir.size()}, false).ok());
        reused = i > 0 && uis.ctx.document(uis.hud) == bound;
    }
    if (!reused) {
        MESSAGE("no reload reused the bound Document's address; the serial check still runs");
    }
    uis.ctx.update(0.016f);
    // The host's own listener on the new Document, likely at the id the flow's listener had on
    // the old one.
    int hostClicks = 0;
    uis.hudDoc().addListener(uis.hudDoc().byId("Title"), EventType::Click, [&](Event&) { ++hostClicks; });
    uis.bundle->tick(0.016f);
    click(uis.hudDoc(), "Title");
    CHECK(hostClicks == 1);
    CHECK(uis.hudDoc().byId("Count").text() == "0");  // The flow does not reach the new Document.
    FlowRuntime* own = uis.bundle->uiFlowRuntime(uis.hud);
    REQUIRE(own != nullptr);
    CHECK(own->values()[0].n == doctest::Approx(0));  // c.value
    uis.bundle.reset();  // Unbinds: must not take the host's listener off the new Document.
    click(uis.hudDoc(), "Title");
    CHECK(hostClicks == 2);
}

TEST_CASE("T-260c: BundleContext::loadUiFlow while a host callback it causes changes the UI list") {
    Uis uis;
    // Setting the gauge's `hp` port (the flow's initial output) emits a host event. The gauge is
    // registered between the hud and the menu: [hud, gauge, menu].
    const DocumentHandle gauge = loadUi(uis.ctx, "gauge.xt", R"X(<xt><body id="Root">
<div id="Hud" port:hp="number in" on:port(hp)="emit(bye)"></div></body></xt>)X");
    const DocumentHandle extra = loadUi(uis.ctx, "plate.xt", kPlate);
    REQUIRE(uis.bundle->removeUi("menu") == Status::kOk);
    REQUIRE(uis.bundle->addUi("gauge", gauge) == Status::kOk);
    REQUIRE(uis.bundle->addUi("menu", uis.menu) == Status::kOk);
    std::vector<std::string> expected;
    std::function<void()> change;
    SUBCASE("removes another UI") {
        change = [&] { CHECK(uis.bundle->removeUi("menu") == Status::kOk); };
        expected = {"hud", "gauge"};
    }
    SUBCASE("registers another UI (the list grows)") {
        change = [&] { CHECK(uis.bundle->addUi("plate", extra) == Status::kOk); };
        expected = {"hud", "gauge", "menu", "plate"};
    }
    int byes = 0;
    HostListener listener;
    listener.host = [&](const HostEvent& e) {
        if (e.document == gauge && byes++ == 0) {
            change();
        }
    };
    uis.ctx.addListener(&listener);
    std::vector<std::string> errors;
    CHECK_MESSAGE(uis.bundle->loadUiFlow("gauge", R"({"version":1,"nodes":[
        {"id":"k","type":"constant","params":{"value":5}}],"edges":[{"from":"k.out","to":"#Hud.hp"}]})",
                                         errors) == Status::kOk,
                  joined(errors));
    CHECK(byes == 1);
    CHECK(uis.bundle->uiIds() == expected);
    CHECK(uis.bundle->uiFlowRuntime(gauge) != nullptr);
    uis.bundle->setHostEventListener([](const FlowHostEvent&) {});
    uis.bundle->enableFlow(false);
    uis.bundle->tick(0.016f);
    uis.ctx.removeListener(&listener);
}

TEST_CASE("T-260c: loadGlobalFlow from a flow callback takes effect once the dispatch returns") {
    Uis uis;
    // A click on Play emits `swap`, then `late`: the second must not run once the flow is swapped.
    uis.loadGlobal(R"({"version":1,"nodes":[
        {"id":"swap","type":"emit","params":{"port":"swap"}},{"id":"late","type":"emit","params":{"port":"late"}}],
      "edges":[{"from":"ui#menu.Play.Click","to":"swap.trigger"},{"from":"ui#menu.Play.Click","to":"late.trigger"}]})");
    int swaps = 0;
    int lates = 0;
    int fromNew = 0;
    uis.bundle->setHostEventListener([&](const FlowHostEvent& e) {
        if (e.name == "swap") {
            ++swaps;
            std::vector<std::string> errors;
            CHECK(uis.bundle->loadGlobalFlow(R"({"version":1,"nodes":[
                {"id":"b","type":"emit","params":{"port":"fromNew"}}],
              "edges":[{"from":"ui#hud.Title.Click","to":"b.trigger"}]})",
                                             errors) == Status::kOk);
            CHECK(errors.empty());
        } else if (e.name == "late") {
            ++lates;
        } else if (e.name == "fromNew") {
            ++fromNew;
        }
    });
    click(uis.menuDoc(), "Play");
    CHECK(swaps == 1);
    CHECK(lates == 0);
    CHECK(uis.bundle->flowRuntime().isGlobal());
    CHECK(uis.bundle->flowRuntime().valueNames().empty());  // The new graph: `b` has no outputs.
    click(uis.hudDoc(), "Title");
    CHECK(fromNew == 1);
    click(uis.menuDoc(), "Play");  // The old graph is gone.
    CHECK(swaps == 1);
}

TEST_CASE("T-260c: FlowRuntime::load failure leaves the runtime empty") {
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    ctx.setViewport(Rect{0, 0, 400, 300}, 1.0f);
    const DocumentHandle first = loadUi(ctx, "hud.xt", kHud);
    const DocumentHandle second = loadUi(ctx, "hud.xt", kHud);
    ctx.update(0.0f);
    FlowGraph good;
    FlowGraph bad;
    std::vector<std::string> errors;
    REQUIRE(FlowGraph::parse(kOwnFlow, good, errors) == Status::kOk);
    REQUIRE(FlowGraph::parse(R"({"version":1,"nodes":[{"id":"c","type":"counter"}],
      "edges":[{"from":"#Nope.Click","to":"c.increment"}]})",
                             bad, errors) == Status::kOk);
    {
        FlowRuntime runtime;
        REQUIRE_MESSAGE(runtime.load(good, *ctx.document(first), errors) == Status::kOk, joined(errors));
        click(*ctx.document(first), "Title");
        CHECK(ctx.document(first)->byId("Count").text() == "1");
        CHECK(runtime.load(bad, *ctx.document(second), errors) == Status::kInvalidArgument);
        CHECK(runtime.values().empty());
        click(*ctx.document(first), "Title");  // Off the first Document too.
        CHECK(ctx.document(first)->byId("Count").text() == "1");
        ctx.unloadDocument(first);
        ctx.unloadDocument(second);
        runtime.tick(0.016f);
        const DocumentHandle third = loadUi(ctx, "hud.xt", kHud);
        ctx.update(0.016f);
        errors.clear();
        REQUIRE_MESSAGE(runtime.load(good, *ctx.document(third), errors) == Status::kOk, joined(errors));
        click(*ctx.document(third), "Title");
        CHECK(ctx.document(third)->byId("Count").text() == "1");
        const DocumentHandle fourth = loadUi(ctx, "hud.xt", kHud);
        CHECK(runtime.load(bad, *ctx.document(fourth), errors) == Status::kInvalidArgument);
        ctx.unloadDocument(third);
        ctx.unloadDocument(fourth);
    }  // Destroyed after a failed load: nothing of the Documents it once had is touched.
    ctx.update(0.016f);
}

TEST_CASE("T-260c: FlowRuntime::unload, at once and from a callback of its own") {
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    ctx.setViewport(Rect{0, 0, 400, 300}, 1.0f);
    const DocumentHandle handle = loadUi(ctx, "hud.xt", kHud);
    ctx.update(0.0f);
    FlowGraph graph;
    std::vector<std::string> errors;
    FlowRuntime runtime;
    SUBCASE("at once") {
        REQUIRE(FlowGraph::parse(kOwnFlow, graph, errors) == Status::kOk);
        REQUIRE_MESSAGE(runtime.load(graph, *ctx.document(handle), errors) == Status::kOk, joined(errors));
        runtime.unload();
    }
    SUBCASE("from its own host event; the rest of that propagation stops") {
        REQUIRE(FlowGraph::parse(R"({"version":1,"nodes":[
            {"id":"e","type":"emit","params":{"port":"stop"}},{"id":"c","type":"counter"},
            {"id":"s","type":"toString"}],
          "edges":[{"from":"#Title.Click","to":"e.trigger"},{"from":"#Title.Click","to":"c.increment"},
            {"from":"c.value","to":"s.in"},{"from":"s.out","to":"#Count.setText"}]})",
                                 graph, errors) == Status::kOk);
        REQUIRE_MESSAGE(runtime.load(graph, *ctx.document(handle), errors) == Status::kOk, joined(errors));
        int stops = 0;
        runtime.setHostEventListener([&](const FlowHostEvent&) {
            ++stops;
            runtime.unload();
            CHECK_FALSE(runtime.values().empty());  // Still the old graph until the dispatch returns.
        });
        click(*ctx.document(handle), "Title");
        CHECK(stops == 1);
    }
    CHECK(runtime.values().empty());
    CHECK(runtime.valueNames().empty());
    click(*ctx.document(handle), "Title");
    CHECK(ctx.document(handle)->byId("Count").text() == "0");
    ctx.unloadDocument(handle);
    runtime.tick(0.016f);
    const DocumentHandle next = loadUi(ctx, "hud.xt", kHud);
    ctx.update(0.016f);
    FlowGraph own;
    REQUIRE(FlowGraph::parse(kOwnFlow, own, errors) == Status::kOk);
    REQUIRE_MESSAGE(runtime.load(own, *ctx.document(next), errors) == Status::kOk, joined(errors));
    click(*ctx.document(next), "Title");
    CHECK(ctx.document(next)->byId("Count").text() == "1");
}

TEST_CASE("T-260c: xtframe-view R with a flow file that no longer parses") {
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    ctx.setViewport(Rect{0, 0, 400, 300}, 1.0f);
    DocumentHandle handle = loadUi(ctx, "hud.xt", kHud);
    ctx.update(0.0f);
    const std::filesystem::path flowPath = xt::test::tempDir() / "t260c_view.xtflow";
    {
        std::ofstream out(flowPath, std::ios::binary);
        out << kOwnFlow;
    }
    const std::string flowUtf8 = compile::pathToUtf8(flowPath);
    FlowRuntime runtime;
    REQUIRE(tools::view::loadFlow(ctx, handle, flowUtf8, runtime));
    click(*ctx.document(handle), "Title");
    CHECK(ctx.document(handle)->byId("Count").text() == "1");
    {
        std::ofstream out(flowPath, std::ios::binary | std::ios::trunc);
        out << "{ not json";
    }
    const DocumentHandle previous = handle;
    handle = loadUi(ctx, "hud.xt", kHud);
    CHECK_FALSE(tools::view::reloadFlow(ctx, previous, handle, flowUtf8, /*hasFlow=*/true, runtime));
    CHECK(ctx.document(previous) == nullptr);
    REQUIRE(ctx.document(handle) != nullptr);
    CHECK(runtime.values().empty());
    ctx.update(0.016f);
    runtime.tick(0.016f);
    click(*ctx.document(handle), "Title");
    CHECK(ctx.document(handle)->byId("Count").text() == "0");
}  // `runtime` goes first: nothing of the unloaded Document is touched.
