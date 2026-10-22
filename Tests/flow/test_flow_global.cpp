#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
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
#include "xt/flow/bundle_context.h"
#include "xt/flow/flow_graph.h"
#include "xt/flow/flow_runtime.h"
#include "xt/input.h"
#include "xt/text_shaper.h"

/// T-193 (Docs/design/27-interaction.md §27.4/27.4.1): the global flow (`ui#<uiId>...` references,
/// lifecycle events), system events, the action and timeline nodes, multi-instance edges, the
/// host flow interface (BundleContext::enableFlow) and the `.xtflow` tail additions (`pos`,
/// `groups`).

using namespace xt;
using namespace xt::flow;

namespace {

constexpr const char* kHud = R"X(<xt><head><style>
#Title { width: 100px; height: 20px; } #Count { width: 40px; height: 20px; }
@timeline intro {
  duration: 0.5s;
  marker: mid 0.2s;
  #Title { 0s { opacity: 0 } 0.5s { opacity: 1 } }
}
</style></head>
<body id="Root"><div id="Hud" port:hp="number in" port:score="number out">
<span id="Title">t</span><span id="Count">0</span></div></body></xt>)X";

constexpr const char* kMenu = R"X(<xt><head><style>
#Play { width: 60px; height: 30px; } #Name { width: 100px; height: 20px; }
</style></head>
<body id="Root"><button id="Play">p</button><input id="Name" type="text"/></body></xt>)X";

constexpr const char* kPlate = R"X(<xt><head><style>
#Btn { width: 60px; height: 30px; } #Label { width: 60px; height: 20px; }
</style></head>
<body id="Root"><button id="Btn">b</button><span id="Label">x</span></body></xt>)X";

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

void click(Document& doc, const char* id) {
    Rect box = doc.byId(id).boundingBox();
    InputEvent ev;
    ev.kind = InputEvent::PointerDown;
    ev.pos = Vec2{box.x + box.w * 0.5f, box.y + box.h * 0.5f};
    doc.dispatchInput(ev);
    ev.kind = InputEvent::PointerUp;
    doc.dispatchInput(ev);
}

/// A named node output from values()/valueNames(); a default PortValue when absent.
PortValue valueOf(const FlowRuntime& runtime, const std::string& name) {
    const std::vector<std::string>& names = runtime.valueNames();
    std::span<const PortValue> values = runtime.values();
    for (size_t i = 0; i < names.size() && i < values.size(); ++i) {
        if (names[i] == name) {
            return values[i];
        }
    }
    return PortValue{};
}

std::string joined(const std::vector<std::string>& errors) {
    std::string out;
    for (const std::string& e : errors) {
        out += e + "\n";
    }
    return out;
}

/// Two UIs (hud, menu) in one Context, registered on a BundleContext.
struct TwoUis {
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    DocumentHandle hud = 0;
    DocumentHandle menu = 0;
    std::unique_ptr<BundleContext> bundle;

    TwoUis() {
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
};

}  // namespace

TEST_CASE("xtflow: pos and groups round-trip and are optional tail keys") {
    const char* json = R"({"version":1,
      "nodes":[{"id":"c","type":"counter","pos":[120.5,-40]},{"id":"t","type":"toggle"}],
      "edges":[],
      "groups":[{"title":"Score","rect":[10,20,300,200],"color":"accent"},{"rect":"bad"}]})";
    FlowGraph graph;
    std::vector<std::string> errors;
    REQUIRE(FlowGraph::parse(json, graph, errors) == Status::kOk);
    REQUIRE(graph.nodes.size() == 2);
    CHECK(graph.nodes[0].hasPos);
    CHECK(graph.nodes[0].posX == doctest::Approx(120.5));
    CHECK(graph.nodes[0].posY == doctest::Approx(-40));
    CHECK(!graph.nodes[1].hasPos);
    REQUIRE(graph.groups.size() == 1);  // The malformed box is dropped, not an error.
    CHECK(graph.groups[0].title == "Score");
    CHECK(graph.groups[0].w == doctest::Approx(300));
    CHECK(graph.groups[0].color == "accent");

    FlowGraph again;
    REQUIRE(FlowGraph::parse(graph.toJson(), again, errors) == Status::kOk);
    CHECK(again.toJson() == graph.toJson());
    CHECK(again.groups.size() == 1);
    CHECK(again.nodes[0].hasPos);

    // Without pos / groups the serialized form is exactly the pre-T-193 one.
    FlowGraph plain;
    NodeDesc counter;
    counter.id = "k";
    counter.type = "counter";
    plain.nodes.push_back(counter);
    const std::string text = plain.toJson();
    CHECK(text.find("groups") == std::string::npos);
    CHECK(text.find("pos") == std::string::npos);
}

TEST_CASE("xtflow: ui# references, lifecycle, instance and the new node types parse") {
    const char* json = R"({"version":1,"nodes":[
        {"id":"c","type":"counter"},
        {"id":"sys","type":"system"},
        {"id":"snd","type":"sound","params":{"cue":"ui_click","volume":0.5}},
        {"id":"tl","type":"timeline","params":{"name":"intro","ui":"hud"}}],
      "edges":[
        {"from":"ui#menu.Play.Click","to":"c.increment"},
        {"from":"ui#hud.shown","to":"snd.trigger"},
        {"from":"sys.paused","to":"tl.play"},
        {"from":"ui#menu.Name.Change","to":"ui#hud.Title.setText","params":{"instance":"2"}}]})";
    FlowGraph graph;
    std::vector<std::string> errors;
    CHECK_MESSAGE(FlowGraph::parse(json, graph, errors) == Status::kOk, joined(errors));

    auto parseError = [](const char* text) {
        FlowGraph g;
        std::vector<std::string> errs;
        CHECK(FlowGraph::parse(text, g, errs) == Status::kInvalidArgument);
        return joined(errs);
    };
    CHECK(parseError(R"({"nodes":[{"id":"c","type":"counter"}],
        "edges":[{"from":"c.value","to":"ui#hud.shown"}]})")
              .find("lifecycle output") != std::string::npos);
    CHECK(parseError(R"({"nodes":[{"id":"c","type":"counter"}],
        "edges":[{"from":"ui#hud.Play.Click","to":"c.increment","params":{"instance":"-1"}}]})")
              .find("invalid 'instance'") != std::string::npos);
    CHECK(parseError(R"({"nodes":[{"id":"s","type":"sound"}],"edges":[]})").find("'cue'") !=
          std::string::npos);
    CHECK(parseError(R"({"nodes":[{"id":"e","type":"emit","params":{"port":""}}],"edges":[]})")
              .find("non-empty string") != std::string::npos);
    CHECK(parseError(R"({"nodes":[],"edges":[{"from":"ui#.x","to":"ui#a.b.c"}]})").find("malformed UI") !=
          std::string::npos);

    // The node table lists the new types after the fourteen §17.6 ones.
    std::span<const NodeTypeDesc> table = nodeTypeTable();
    REQUIRE(table.size() == 20);
    CHECK(table[14].type == "system");
    CHECK(table[14].outputs.size() == 9);  // T-330: + reducedMotion.
    CHECK(table[19].type == "timeline");
}

TEST_CASE("global flow: three-segment ui# references drive one UI from another") {
    TwoUis uis;
    const char* json = R"({"version":1,"nodes":[
        {"id":"c","type":"counter"},
        {"id":"s","type":"toString"}],
      "edges":[
        {"from":"ui#menu.Play.Click","to":"c.increment"},
        {"from":"c.value","to":"s.in"},
        {"from":"s.out","to":"ui#hud.Count.setText"},
        {"from":"ui#menu.Name.Change","to":"ui#hud.Title.setText"}]})";
    std::vector<std::string> errors;
    REQUIRE_MESSAGE(uis.bundle->loadGlobalFlow(json, errors) == Status::kOk, joined(errors));
    FlowRuntime& global = uis.bundle->flowRuntime();
    CHECK(global.isGlobal());

    click(uis.menuDoc(), "Play");
    click(uis.menuDoc(), "Play");
    CHECK(uis.hudDoc().byId("Count").text() == "2");
    uis.menuDoc().byId("Name").setValue("Ada");
    CHECK(uis.hudDoc().byId("Title").text() == "Ada");
}

TEST_CASE("global flow: two-segment references reach document-level declared ports") {
    TwoUis uis;
    const char* json = R"({"version":1,"nodes":[{"id":"k","type":"constant","params":{"value":42}}],
      "edges":[{"from":"k.out","to":"ui#hud.hp"}]})";
    std::vector<std::string> errors;
    REQUIRE_MESSAGE(uis.bundle->loadGlobalFlow(json, errors) == Status::kOk, joined(errors));
    PortId hp = uis.hudDoc().lookupPort("hp");
    REQUIRE(hp != kInvalidPort);
    CHECK(uis.hudDoc().portText(hp) == "42");
}

TEST_CASE("global flow: binding errors") {
    TwoUis uis;
    auto loadError = [&uis](const char* text) {
        std::vector<std::string> errs;
        CHECK(uis.bundle->loadGlobalFlow(text, errs) == Status::kInvalidArgument);
        return joined(errs);
    };
    CHECK(loadError(R"({"nodes":[{"id":"c","type":"counter"}],
        "edges":[{"from":"#Play.Click","to":"c.increment"}]})")
              .find("ui#<uiId>") != std::string::npos);
    CHECK(loadError(R"({"nodes":[{"id":"c","type":"counter"}],
        "edges":[{"from":"ui#shop.Buy.Click","to":"c.increment"}]})")
              .find("unknown UI 'shop'") != std::string::npos);
    CHECK(loadError(R"({"nodes":[{"id":"c","type":"counter"}],
        "edges":[{"from":"ui#menu.Nope.Click","to":"c.increment"}]})")
              .find("'#Nope'") != std::string::npos);
    CHECK(loadError(R"({"nodes":[{"id":"t","type":"timeline","params":{"name":"intro"}}],"edges":[]})")
              .find("'ui' param") != std::string::npos);

    // A UI's own flow may not use ui# references (nor a timeline `ui`).
    FlowRuntime own;
    FlowGraph graph;
    std::vector<std::string> errs;
    REQUIRE(FlowGraph::parse(R"({"nodes":[{"id":"c","type":"counter"}],
        "edges":[{"from":"ui#menu.Play.Click","to":"c.increment"}]})",
                             graph, errs) == Status::kOk);
    CHECK(own.load(graph, uis.hudDoc(), errs) == Status::kInvalidArgument);
    CHECK(joined(errs).find("only valid in the global flow") != std::string::npos);
}

TEST_CASE("global flow: ui#<id>.shown / hidden follow the Context lifecycle") {
    TwoUis uis;
    const char* json =
        R"({"version":1,"nodes":[{"id":"shown","type":"counter"},{"id":"hidden","type":"counter"}],
      "edges":[{"from":"ui#hud.hidden","to":"hidden.increment"},{"from":"ui#hud.shown","to":"shown.increment"}]})";
    std::vector<std::string> errors;
    REQUIRE_MESSAGE(uis.bundle->loadGlobalFlow(json, errors) == Status::kOk, joined(errors));
    FlowRuntime& global = uis.bundle->flowRuntime();

    REQUIRE(uis.ctx.requestHide(uis.hud) == Status::kOk);
    uis.ctx.update(0.016f);
    CHECK(valueOf(global, "hidden.value").n == doctest::Approx(1));
    REQUIRE(uis.ctx.requestShow(uis.hud) == Status::kOk);
    uis.ctx.update(0.016f);
    CHECK(valueOf(global, "shown.value").n == doctest::Approx(1));
    // The menu's lifecycle is not the hud's.
    REQUIRE(uis.ctx.requestHide(uis.menu) == Status::kOk);
    uis.ctx.update(0.016f);
    CHECK(valueOf(global, "hidden.value").n == doctest::Approx(1));
}

TEST_CASE("system events: pulses and strings reach system nodes; unknown kinds are rejected") {
    TwoUis uis;
    const char* json = R"({"version":1,"nodes":[
        {"id":"sys","type":"system"},{"id":"paused","type":"counter"}],
      "edges":[
        {"from":"sys.paused","to":"paused.increment"},
        {"from":"sys.languageChanged","to":"ui#hud.Title.setText"}]})";
    std::vector<std::string> errors;
    REQUIRE_MESSAGE(uis.bundle->loadGlobalFlow(json, errors) == Status::kOk, joined(errors));
    FlowRuntime& global = uis.bundle->flowRuntime();

    CHECK(uis.bundle->postSystemEvent("paused") == Status::kOk);
    CHECK(uis.bundle->postSystemEvent("paused") == Status::kOk);
    CHECK(valueOf(global, "paused.value").n == doctest::Approx(2));
    CHECK(uis.bundle->postSystemEvent("languageChanged", "fr") == Status::kOk);
    CHECK(uis.hudDoc().byId("Title").text() == "fr");
    CHECK(valueOf(global, "sys.languageChanged").s == "fr");
    uis.hudDoc().byId("Title").setText("x");
    // A string kind fires on every post, not only on change.
    CHECK(uis.bundle->postSystemEvent("languageChanged", "fr") == Status::kOk);
    CHECK(uis.hudDoc().byId("Title").text() == "fr");
    CHECK(uis.bundle->postSystemEvent("reboot") == Status::kInvalidArgument);
    CHECK(systemEventKinds().size() == 9);

    // A UI's own flow takes system events too.
    FlowRuntime own;
    FlowGraph graph;
    REQUIRE(FlowGraph::parse(R"({"nodes":[{"id":"sys","type":"system"}],
        "edges":[{"from":"sys.platform","to":"#Count.setText"}]})",
                             graph, errors) == Status::kOk);
    REQUIRE(own.load(graph, uis.hudDoc(), errors) == Status::kOk);
    CHECK(own.postSystemEvent("platform", "console") == Status::kOk);
    CHECK(uis.hudDoc().byId("Count").text() == "console");
}

// T-262b (F12): `back` is a pulse port (nodePortInfo), so it fires a pulse like `paused` does --
// not an empty string event.
TEST_CASE("T-262b system events: back fires a pulse into a pulse input") {
    TwoUis uis;
    const char* json = R"({"version":1,"nodes":[
        {"id":"sys","type":"system"},{"id":"backs","type":"counter"}],
      "edges":[{"from":"sys.back","to":"backs.increment"}]})";
    std::vector<std::string> errors;
    REQUIRE_MESSAGE(uis.bundle->loadGlobalFlow(json, errors) == Status::kOk, joined(errors));
    FlowRuntime& global = uis.bundle->flowRuntime();
    CHECK(uis.bundle->postSystemEvent("back") == Status::kOk);
    CHECK(uis.bundle->postSystemEvent("back") == Status::kOk);
    CHECK(valueOf(global, "backs.value").n == doctest::Approx(2));
    // The values list shows the pulse port as a pulse, as it does `paused` -- not as a string.
    CHECK(valueOf(global, "sys.back").type == PortType::Pulse);
    CHECK(valueOf(global, "sys.back").type == valueOf(global, "sys.paused").type);
}

// T-262b (PR #270 review): a published bundle's per-UI flow that does not parse -- the `{}` of
// Tests/bundle/fixtures/published/ui/hud.xtflow -- is reported by loadUiFlow() as a non-OK status
// with its problem, never accepted silently; the UI's Document is left as it was.
TEST_CASE("T-262b loadUiFlow: an unparsable per-UI flow ({}) returns a non-OK status") {
    std::ifstream in(std::string(XT_TEST_FLOW_DIR) + "/../bundle/fixtures/published/ui/hud.xtflow",
                     std::ios::binary);
    REQUIRE(in.good());
    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    TwoUis uis;
    std::vector<std::string> errors;
    CHECK(uis.bundle->loadUiFlow("hud", json, errors) != Status::kOk);
    CHECK_FALSE(errors.empty());
    CHECK(uis.ctx.document(uis.hud) != nullptr);
}

TEST_CASE("action nodes: sound, haptic, spawn and emit send host events with the UI and instance") {
    TwoUis uis;
    uis.ctx.setInstanceId(uis.menu, 7);
    std::vector<FlowHostEvent> events;
    uis.bundle->setHostEventListener([&events](const FlowHostEvent& e) { events.push_back(e); });
    const char* json = R"({"version":1,"nodes":[
        {"id":"snd","type":"sound","params":{"cue":"ui_click","volume":0.25}},
        {"id":"hap","type":"haptic","params":{"cue":"rumble"}},
        {"id":"spw","type":"spawn","params":{"symbol":"Coin"}},
        {"id":"k","type":"constant","params":{"value":"gold"}},
        {"id":"emt","type":"emit","params":{"port":"loot"}},
        {"id":"sys","type":"system"}],
      "edges":[
        {"from":"ui#menu.Play.Click","to":"snd.trigger"},
        {"from":"ui#menu.Play.Click","to":"hap.trigger"},
        {"from":"sys.loading","to":"spw.trigger"},
        {"from":"k.out","to":"emt.value"},
        {"from":"sys.loading","to":"emt.trigger"}]})";
    std::vector<std::string> errors;
    REQUIRE_MESSAGE(uis.bundle->loadGlobalFlow(json, errors) == Status::kOk, joined(errors));

    click(uis.menuDoc(), "Play");
    REQUIRE(events.size() == 2);
    CHECK(events[0].kind == FlowHostEventKind::Sound);
    CHECK(events[0].name == "ui_click");
    CHECK(events[0].volume == doctest::Approx(0.25f));
    CHECK(events[0].uiId == "menu");
    CHECK(events[0].instanceId == 7);
    CHECK(events[0].nodeId == "snd");
    CHECK(events[1].kind == FlowHostEventKind::Haptic);
    CHECK(events[1].name == "rumble");

    events.clear();
    REQUIRE(uis.bundle->postSystemEvent("loading") == Status::kOk);
    REQUIRE(events.size() == 2);
    CHECK(events[0].kind == FlowHostEventKind::Spawn);
    CHECK(events[0].name == "Coin");
    CHECK(events[0].uiId.empty());
    CHECK(events[0].instanceId == 0);
    CHECK(events[1].kind == FlowHostEventKind::Emit);
    CHECK(events[1].name == "loot");
    CHECK(events[1].value.type == PortType::String);
    CHECK(events[1].value.s == "gold");
}

TEST_CASE("timeline node: play / seek / stop a named timeline, end and marker outputs") {
    TwoUis uis;
    const char* json = R"({"version":1,"nodes":[
        {"id":"sys","type":"system"},
        {"id":"tl","type":"timeline","params":{"name":"intro","ui":"hud"}},
        {"id":"ends","type":"counter"},
        {"id":"missing","type":"timeline","params":{"name":"outro","ui":"hud"}}],
      "edges":[
        {"from":"sys.loading","to":"tl.play"},
        {"from":"sys.paused","to":"tl.stop"},
        {"from":"sys.resumed","to":"missing.play"},
        {"from":"tl.end","to":"ends.increment"},
        {"from":"tl.marker","to":"ui#hud.Count.setText"}]})";
    std::vector<std::string> errors;
    REQUIRE_MESSAGE(uis.bundle->loadGlobalFlow(json, errors) == Status::kOk, joined(errors));
    FlowRuntime& global = uis.bundle->flowRuntime();

    REQUIRE(uis.bundle->postSystemEvent("loading") == Status::kOk);
    CHECK(uis.hudDoc().isTimelinePlaying("", "intro"));
    for (int i = 0; i < 20; ++i) {
        uis.ctx.update(0.05f);
    }
    CHECK(uis.hudDoc().byId("Count").text() == "mid");
    CHECK(valueOf(global, "tl.marker").s == "mid");
    CHECK(valueOf(global, "ends.value").n == doctest::Approx(1));

    REQUIRE(uis.bundle->postSystemEvent("loading") == Status::kOk);
    REQUIRE(uis.bundle->postSystemEvent("paused") == Status::kOk);
    CHECK(!uis.hudDoc().isTimelinePlaying("", "intro"));

    // A timeline the UI does not have is one runtime error, not a load error.
    REQUIRE(uis.bundle->postSystemEvent("resumed") == Status::kOk);
    REQUIRE(uis.bundle->postSystemEvent("resumed") == Status::kOk);
    REQUIRE(global.runtimeErrors().size() == 1);
    CHECK(global.runtimeErrors()[0].find("'outro'") != std::string::npos);

    // In a UI's own flow the node acts on that Document (seek is a value input).
    FlowRuntime own;
    FlowGraph graph;
    REQUIRE(FlowGraph::parse(R"({"nodes":[{"id":"tl","type":"timeline","params":{"name":"intro"}},
        {"id":"sys","type":"system"}],"edges":[{"from":"sys.loading","to":"tl.play"}]})",
                             graph, errors) == Status::kOk);
    REQUIRE_MESSAGE(own.load(graph, uis.hudDoc(), errors) == Status::kOk, joined(errors));
    REQUIRE(own.postSystemEvent("loading") == Status::kOk);
    CHECK(uis.hudDoc().isTimelinePlaying("", "intro"));
}

TEST_CASE("multi-instance: instance-filtered sources and sinks") {
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    DocumentHandle plate1 = loadUi(ctx, "plate.xt", kPlate);
    DocumentHandle plate2 = loadUi(ctx, "plate.xt", kPlate);
    ctx.setViewport(Rect{0, 0, 300, 200}, 1.0f);
    ctx.update(0.0f);
    REQUIRE(ctx.setInstanceId(plate1, 1) == Status::kOk);
    REQUIRE(ctx.setInstanceId(plate2, 2) == Status::kOk);
    BundleContext bundle(ctx);
    REQUIRE(bundle.addUi("plate", plate1) == Status::kOk);
    REQUIRE(bundle.addUi("plate", plate2) == Status::kOk);
    CHECK(bundle.addUi("plate", plate2) == Status::kInvalidArgument);
    CHECK(bundle.handles("plate").size() == 2);
    CHECK(bundle.uiIds().size() == 1);

    const char* json = R"({"version":1,"nodes":[
        {"id":"any","type":"counter"},{"id":"two","type":"counter"},
        {"id":"k","type":"constant","params":{"value":"one"}},
        {"id":"all","type":"constant","params":{"value":"all"}}],
      "edges":[
        {"from":"ui#plate.Btn.Click","to":"any.increment"},
        {"from":"ui#plate.Btn.Click","to":"two.increment","params":{"instance":"2"}},
        {"from":"k.out","to":"ui#plate.Label.setText","params":{"instance":"1"}}]})";
    std::vector<std::string> errors;
    REQUIRE_MESSAGE(bundle.loadGlobalFlow(json, errors) == Status::kOk, joined(errors));
    FlowRuntime& global = bundle.flowRuntime();

    CHECK(ctx.document(plate1)->byId("Label").text() == "one");
    CHECK(ctx.document(plate2)->byId("Label").text() == "x");

    click(*ctx.document(plate1), "Btn");
    CHECK(valueOf(global, "any.value").n == doctest::Approx(1));
    CHECK(valueOf(global, "two.value").n == doctest::Approx(0));
    click(*ctx.document(plate2), "Btn");
    CHECK(valueOf(global, "any.value").n == doctest::Approx(2));
    CHECK(valueOf(global, "two.value").n == doctest::Approx(1));
}

TEST_CASE("host flow interface: enableFlow stops propagation and ticking; per-UI flows follow") {
    TwoUis uis;
    const char* json = R"({"version":1,"nodes":[{"id":"c","type":"counter"},
        {"id":"t","type":"timer","params":{"interval":0.1,"autostart":true}},{"id":"ticks","type":"counter"}],
      "edges":[{"from":"ui#menu.Play.Click","to":"c.increment"},{"from":"t.tick","to":"ticks.increment"}]})";
    std::vector<std::string> errors;
    REQUIRE_MESSAGE(uis.bundle->loadGlobalFlow(json, errors) == Status::kOk, joined(errors));
    const char* hudFlow = R"({"nodes":[{"id":"sys","type":"system"}],
        "edges":[{"from":"sys.platform","to":"#Count.setText"}]})";
    REQUIRE_MESSAGE(uis.bundle->loadUiFlow("hud", hudFlow, errors) == Status::kOk, joined(errors));
    REQUIRE(uis.bundle->uiFlowRuntime(uis.hud) != nullptr);
    CHECK(uis.bundle->uiFlowRuntime(uis.menu) == nullptr);
    CHECK(uis.bundle->loadUiFlow("shop", hudFlow, errors) == Status::kInvalidArgument);
    FlowRuntime& global = uis.bundle->flowRuntime();

    uis.bundle->enableFlow(false);
    CHECK(!uis.bundle->flowEnabled());
    CHECK(!global.enabled());
    click(uis.menuDoc(), "Play");
    uis.bundle->tick(0.25f);
    uis.bundle->postSystemEvent("platform", "console");
    CHECK(valueOf(global, "c.value").n == doctest::Approx(0));
    CHECK(valueOf(global, "ticks.value").n == doctest::Approx(0));
    CHECK(uis.hudDoc().byId("Count").text() == "0");

    uis.bundle->enableFlow(true);
    click(uis.menuDoc(), "Play");
    uis.bundle->tick(0.25f);
    uis.bundle->postSystemEvent("platform", "console");
    CHECK(valueOf(global, "c.value").n == doctest::Approx(1));
    CHECK(valueOf(global, "ticks.value").n == doctest::Approx(2));
    CHECK(uis.hudDoc().byId("Count").text() == "console");

    // A global runtime is not rebound; it is loaded again.
    std::vector<std::string> warnings;
    CHECK(global.rebind(uis.hudDoc(), warnings) == Status::kUnsupported);
}

TEST_CASE("T-200: system events reach the Documents' system.* ports and the flows both ways") {
    TwoUis uis;
    const DocumentHandle sys =
        loadUi(uis.ctx, "sys.xt",
               R"X(<xt><body id="Root"><span id="Plat" bind:text="system.platform"></span>
                                          <div id="P" on:port(system.paused)="class(paused)"></div></body></xt>)X");
    REQUIRE(uis.bundle->addUi("sys", sys) == Status::kOk);
    const char* json = R"({"version":1,"nodes":[
        {"id":"sys","type":"system"},{"id":"paused","type":"counter"}],
      "edges":[{"from":"sys.paused","to":"paused.increment"}]})";
    std::vector<std::string> errors;
    REQUIRE_MESSAGE(uis.bundle->loadGlobalFlow(json, errors) == Status::kOk, joined(errors));
    FlowRuntime& global = uis.bundle->flowRuntime();

    // BundleContext -> Context -> the Documents' ports, then the flows.
    REQUIRE(uis.bundle->postSystemEvent("platform", "console") == Status::kOk);
    uis.ctx.update(0.0f);  // T-389: the bindings of a port are evaluated by the next update().
    CHECK(uis.ctx.document(sys)->byId("Plat").text() == "console");
    REQUIRE(uis.bundle->postSystemEvent("paused") == Status::kOk);
    CHECK(uis.ctx.document(sys)->byId("P").hasClass("paused"));
    CHECK(valueOf(global, "paused.value").n == doctest::Approx(1));
    // A host posting on the Context directly reaches the flows through the BundleContext.
    REQUIRE(uis.ctx.postSystemEvent("paused") == Status::kOk);
    CHECK(valueOf(global, "paused.value").n == doctest::Approx(2));
    CHECK(uis.ctx.postSystemEvent("reboot") == Status::kInvalidArgument);
    CHECK(valueOf(global, "paused.value").n == doctest::Approx(2));
}

// T-330: `reducedMotion` is a string output of the `system` node, posted through the Context (which
// also sets DeviceInfo::reducedMotion).
TEST_CASE("T-330 system events: reducedMotion reaches the flow as a string") {
    TwoUis uis;
    const char* json = R"({"version":1,"nodes":[{"id":"sys","type":"system"}],
      "edges":[{"from":"sys.reducedMotion","to":"ui#hud.Title.setText"}]})";
    std::vector<std::string> errors;
    REQUIRE_MESSAGE(uis.bundle->loadGlobalFlow(json, errors) == Status::kOk, joined(errors));
    FlowRuntime& global = uis.bundle->flowRuntime();
    CHECK(uis.bundle->postSystemEvent("reducedMotion", "reduce") == Status::kOk);
    CHECK(valueOf(global, "sys.reducedMotion").s == "reduce");
    CHECK(valueOf(global, "sys.reducedMotion").type == PortType::String);
    CHECK(uis.hudDoc().byId("Title").text() == "reduce");
    CHECK(uis.bundle->postSystemEvent("reducedMotion", "maybe") == Status::kInvalidArgument);
}

// T-356: a BundleContext whose UIs live in two Contexts (the editor's Demo space: the shown UI in
// the preview's Context, the other UIs in one it never renders). Same handle numbers in both.
TEST_CASE("T-356 BundleContext: UIs in another Context take part in the global flow") {
    NullTextShaper shaper;
    Context shown{ContextConfig{&shaper, nullptr}};
    Context others{ContextConfig{&shaper, nullptr}};
    const DocumentHandle hud = loadUi(shown, "hud.xt", kHud);
    const DocumentHandle menu = loadUi(others, "menu.xt", kMenu);
    CHECK(hud == menu);  // Handles are per Context.
    for (Context* ctx : {&shown, &others}) {
        ctx->setViewport(Rect{0, 0, 400, 300}, 1.0f);
        ctx->update(0.0f);
    }
    BundleContext bundle(shown);
    REQUIRE(bundle.addUi("hud", hud) == Status::kOk);
    REQUIRE(bundle.addUi("menu", others, menu) == Status::kOk);
    CHECK(bundle.addUi("menu", others, menu) == Status::kInvalidArgument);
    CHECK(bundle.uiIds() == std::vector<std::string>{"hud", "menu"});
    CHECK(bundle.handles("menu").empty());  // context()'s instances only.
    REQUIRE(bundle.instances("menu").size() == 1);
    CHECK(bundle.instances("menu")[0].context == &others);

    const char* json = R"({"version":1,"nodes":[{"id":"c","type":"counter"},{"id":"s","type":"toString"},
        {"id":"hidden","type":"counter"}],
      "edges":[{"from":"ui#menu.Play.Click","to":"c.increment"},{"from":"c.value","to":"s.in"},
        {"from":"s.out","to":"ui#hud.Count.setText"},{"from":"ui#menu.hidden","to":"hidden.increment"}]})";
    std::vector<std::string> errors;
    REQUIRE_MESSAGE(bundle.loadGlobalFlow(json, errors) == Status::kOk, joined(errors));
    click(*others.document(menu), "Play");
    CHECK(shown.document(hud)->byId("Count").text() == "1");

    // The menu's lifecycle comes from its own Context; the hud's (same handle) is not the menu's.
    REQUIRE(shown.requestHide(hud) == Status::kOk);
    shown.update(0.016f);
    CHECK(valueOf(bundle.flowRuntime(), "hidden.value").n == doctest::Approx(0));
    REQUIRE(others.requestHide(menu) == Status::kOk);
    others.update(0.016f);
    CHECK(valueOf(bundle.flowRuntime(), "hidden.value").n == doctest::Approx(1));
}

namespace {

/// T-356: writes `text` to `path`, creating its directory.
void writeFile(const std::filesystem::path& path, const char* text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << text;
}

/// T-356: a UI Project on disk: UIs a (out port), b (in port), c (own sibling flow) and a global
/// flow in shared/ that sends a's `score` to b's `hp`.
std::string writeRuntimeProject(const char* name) {
    const std::filesystem::path root = xt::test::tempDir() / name;
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    writeFile(root / "T356.xtui", R"({"version": 1, "id": "t356", "name": "T356",
  "uis": [ { "id": "a", "doc": "a/a.xt" }, { "id": "b", "doc": "b/b.xt" }, { "id": "c", "doc": "c/c.xt" } ],
  "shared": { "styles": [] } })");
    writeFile(root / "a" / "a.xt",
              R"(<xt><body id="Root"><div id="A" port:score="number out"></div></body></xt>)");
    writeFile(root / "b" / "b.xt",
              R"(<xt><body id="Root"><div id="B" port:hp="number in"></div></body></xt>)");
    writeFile(root / "c" / "c.xt", R"(<xt><body id="Root"><span id="Label">x</span></body></xt>)");
    writeFile(root / "c" / "c.xtflow",
              R"({"version":1,"nodes":[{"id":"k","type":"constant","params":{"value":"hi"}}],
      "edges":[{"from":"k.out","to":"#Label.setText"}]})");
    writeFile(root / "shared" / "bundle.xtflow",
              R"({"version":1,"nodes":[],"edges":[{"from":"ui#a.score","to":"ui#b.hp"}]})");
    return compile::pathToUtf8(root);
}

}  // namespace

// T-356: xtframe-view --project (one Context) and the editor's Demo space (the shown UI apart)
// load the same project through bundle::loadProjectRuntime() and get the same UIs and global flow.
TEST_CASE("T-356 loadProjectRuntime: one Context (view) and two Contexts (editor) load the same project") {
    const std::string dir = writeRuntimeProject("xt_t356_project_runtime");
    Result<bundle::UiProject> project = bundle::UiProject::load(dir);
    REQUIRE(project.ok());
    const bundle::ProjectRuntimeSources sources = bundle::projectRuntimeSources(project.value(), "a");
    CHECK(sources.currentUi == "a");
    REQUIRE(sources.others.size() == 2);
    CHECK(sources.others[0].id == "b");
    CHECK(sources.others[0].flow.empty());
    CHECK(sources.others[1].id == "c");
    CHECK(std::filesystem::path(compile::pathFromUtf8(sources.others[1].flow)).filename() == "c.xtflow");
    CHECK(std::filesystem::path(compile::pathFromUtf8(sources.globalFlow)).parent_path().filename() ==
          "shared");

    bundle::ProjectRuntimeOptions options;
    options.compile.rootDir = dir;
    NullTextShaper shaper;
    struct Loaded {
        std::vector<std::string> uiIds;
        bool global = false;
        std::string label;
        std::string hp;
    };
    auto run = [&](bool split) {
        Context shown{ContextConfig{&shaper, nullptr}};
        Context others{ContextConfig{&shaper, nullptr}};
        Context& othersCtx = split ? others : shown;
        // The view loads the other UIs first (the shown one stays the foreground Document).
        bundle::ProjectRuntime runtime;
        std::vector<bundle::ProjectRuntimeProblem> problems;
        REQUIRE(bundle::loadProjectUis(sources, options, othersCtx, runtime, problems) == Status::kOk);
        const compile::CompileResult a = compile::compileDocument(dir + "/a/a.xt", options.compile);
        REQUIRE(a.ok());
        Result<DocumentHandle> current = shown.loadDocument(IrBlob{a.ir.data(), a.ir.size()}, true);
        REQUIRE(current.ok());
        REQUIRE(bundle::startProjectFlows(sources, options, shown, current.value(), runtime, problems) ==
                Status::kOk);
        for (const bundle::ProjectRuntimeProblem& p : problems) {
            CAPTURE(p.message);
        }
        CHECK(problems.empty());
        REQUIRE(runtime.bundle != nullptr);
        Loaded out;
        out.uiIds = runtime.bundle->uiIds();
        out.global = runtime.bundle->flowRuntime().isGlobal();
        REQUIRE(runtime.others.size() == 2);
        Document* b = othersCtx.document(runtime.others[0].second);
        Document* c = othersCtx.document(runtime.others[1].second);
        REQUIRE(b != nullptr);
        REQUIRE(c != nullptr);
        out.label = std::string(c->byId("Label").text());
        Document* docA = shown.document(current.value());
        docA->setPort(docA->lookupPort("score"), Value::ofFloat(7));
        runtime.bundle->tick(0.0f);  // T-389: a value port reaches the flow on its next tick().
        out.hp = b->portText(b->lookupPort("hp"));
        const DocumentHandle bHandle = runtime.others[0].second;
        runtime.reset();  // Unloads the other UIs; the shown one is the host's.
        CHECK(othersCtx.document(bHandle) == nullptr);
        CHECK(shown.document(current.value()) == docA);
        return out;
    };
    const Loaded view = run(false);
    const Loaded editor = run(true);
    CHECK(view.uiIds == std::vector<std::string>{"a", "b", "c"});
    CHECK(editor.uiIds == view.uiIds);
    CHECK(view.global);
    CHECK(editor.global);
    CHECK(view.label == "hi");
    CHECK(editor.label == "hi");
    CHECK(view.hp == "7");
    CHECK(editor.hp == "7");
}
