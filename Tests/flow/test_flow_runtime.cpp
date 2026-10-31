#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "test_temp.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/flow/flow_graph.h"
#include "xt/flow/flow_runtime.h"
#include "xt/input.h"
#include "xt/property.h"
#include "xt/text_shaper.h"

/// FlowRuntime end-to-end, against Tests/flow/runtime_doc.xt (button#inc, span#count,
/// input#name, span#echo, button#darkToggle, div#bar, `<anim id="anim">`), exercising every kind
/// of connection point (Docs/design/17-editor.md §17.6, Docs/tasks/T-56.md "test_flow_runtime"):
/// `#inc.Click -> counter -> toString -> #count.setText` via two simulated clicks; `#name.Change
/// -> #echo.setText`; `toggle -> #root.toggleClass(dark)`; `#document.clock -> math(map) ->
/// #bar.setProperty(Opacity)`; a one-shot `timer -> <anim>.play`, then `<anim>.Finished` once the
/// clip (2s, Tests/lottie/min_solid_anim.json) completes; and values()/valueNames() correctness.

using namespace xt;
using namespace xt::flow;

namespace {

std::string flowPath(const std::string& name) { return std::string(XT_TEST_FLOW_DIR) + "/" + name; }

std::unique_ptr<Document> loadRuntimeDoc(TextShaper& shaper) {
    compile::CompileOptions options;
    compile::CompileResult result = compile::compileDocument(flowPath("runtime_doc.xt"), options);
    for (const compile::Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    std::unique_ptr<Document> doc;
    Status status = Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &shaper, nullptr, doc);
    REQUIRE(status == Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 400});
    doc->update(0.0f);
    return doc;
}

void click(Document& doc, const char* id) {
    ElementRef ref = doc.byId(id);
    REQUIRE(ref.valid());
    Rect box = ref.boundingBox();
    Vec2 center{box.x + box.w * 0.5f, box.y + box.h * 0.5f};
    InputEvent down;
    down.kind = InputEvent::PointerDown;
    down.pos = center;
    doc.dispatchInput(down);
    InputEvent up = down;
    up.kind = InputEvent::PointerUp;
    doc.dispatchInput(up);
}

void movePointer(Document& doc, Vec2 pos) {
    InputEvent ev;
    ev.kind = InputEvent::PointerMove;
    ev.pos = pos;
    doc.dispatchInput(ev);
}

void pointerButton(Document& doc, Vec2 pos, bool down) {
    InputEvent ev;
    ev.kind = down ? InputEvent::PointerDown : InputEvent::PointerUp;
    ev.pos = pos;
    doc.dispatchInput(ev);
}

Vec2 centerOf(Document& doc, const char* id) {
    ElementRef ref = doc.byId(id);
    REQUIRE(ref.valid());
    Rect box = ref.boundingBox();
    return Vec2{box.x + box.w * 0.5f, box.y + box.h * 0.5f};
}

FlowGraph buildGraph() {
    const std::string json = R"({
        "version": 1,
        "nodes": [
            { "id": "cnt", "type": "counter", "params": { "step": 1, "initial": 0 } },
            { "id": "ts", "type": "toString", "params": { "format": "{}" } },
            { "id": "tog", "type": "toggle", "params": { "initial": false } },
            { "id": "mth", "type": "math",
              "params": { "op": "map", "inLo": 0, "inHi": 2, "outLo": 0, "outHi": 1 } },
            { "id": "tmr", "type": "timer", "params": { "interval": 3.5, "autostart": true } },
            { "id": "finCnt", "type": "counter", "params": {} }
        ],
        "edges": [
            { "from": "#inc.Click", "to": "cnt.increment" },
            { "from": "cnt.value", "to": "ts.in" },
            { "from": "ts.out", "to": "#count.setText" },

            { "from": "#name.Change", "to": "#echo.setText" },

            { "from": "#darkToggle.Click", "to": "tog.toggle" },
            { "from": "tog.changed", "to": "#root.toggleClass", "params": { "class": "dark" } },

            { "from": "#document.clock", "to": "mth.a" },
            { "from": "mth.out", "to": "#bar.setProperty", "params": { "prop": "Opacity" } },

            { "from": "tmr.tick", "to": "#anim.play" },
            { "from": "#anim.Finished", "to": "finCnt.increment" }
        ]
    })";
    FlowGraph graph;
    std::vector<std::string> errors;
    Status status = FlowGraph::parse(json, graph, errors);
    for (const std::string& e : errors) {
        CAPTURE(e);
    }
    REQUIRE(status == Status::kOk);
    return graph;
}

}  // namespace

TEST_CASE("FlowRuntime: end-to-end against runtime_doc.xt") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadRuntimeDoc(shaper);
    FlowGraph graph = buildGraph();

    FlowRuntime rt;
    std::vector<std::string> errors;
    REQUIRE(rt.load(graph, *doc, errors) == Status::kOk);
    for (const std::string& e : errors) {
        CAPTURE(e);
    }

    // valueNames()/values(): node declaration order x each type's own output-port order.
    const std::vector<std::string> expectedNames = {
        "cnt.value", "ts.out", "tog.value", "tog.changed", "mth.out", "tmr.tick", "tmr.count", "finCnt.value",
    };
    CHECK(rt.valueNames() == expectedNames);
    REQUIRE(rt.values().size() == expectedNames.size());

    // #inc.Click -> counter -> toString -> #count.setText, clicked twice -> "2".
    ElementRef count = doc->byId("count");
    REQUIRE(count.valid());
    CHECK(count.text() == "0");
    click(*doc, "inc");
    click(*doc, "inc");
    CHECK(count.text() == "2");
    CHECK(rt.values()[0].n == doctest::Approx(2));  // cnt.value
    CHECK(rt.values()[1].s == "2");                 // ts.out

    // #name.Change -> #echo.setText.
    ElementRef name = doc->byId("name");
    ElementRef echo = doc->byId("echo");
    REQUIRE(name.valid());
    REQUIRE(echo.valid());
    name.setValue("hello");  // Dispatches a (bubbling) Change event, same as a real keystroke.
    CHECK(echo.text() == "hello");

    // toggle -> #root.toggleClass(dark).
    ElementRef root = doc->byId("root");
    REQUIRE(root.valid());
    CHECK_FALSE(root.hasClass("dark"));
    click(*doc, "darkToggle");
    CHECK(root.hasClass("dark"));
    CHECK(rt.values()[2].b);  // tog.value

    // #document.clock -> math(map) -> #bar.setProperty(Opacity). tick(1.0): clock == 1 ->
    // map([0,2] -> [0,1])(1) == 0.5. This also advances tmr's one-shot timer (tick -> its own
    // stop, and tick -> #anim.play) and finCnt via #anim.Finished once the clip completes.
    ElementRef bar = doc->byId("bar");
    REQUIRE(bar.valid());
    float simClock = 0.0f;
    const float dt = 0.1f;
    for (int i = 0; i < 10; ++i) {  // 10 x 0.1s == 1.0s.
        simClock += dt;
        doc->update(simClock);
        rt.tick(dt);
    }
    CHECK(bar.property(prop::Opacity).f == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(rt.values()[4].n == doctest::Approx(0.5).epsilon(0.02));  // mth.out
    CHECK(rt.values()[6].n == doctest::Approx(0));  // tmr.count: interval (3.5s) not reached yet.

    // tmr's interval (3.5s) is crossed once around flow-clock == 3.5s, firing #anim.play; the
    // `<anim>` clip is 2s (Tests/lottie/min_solid_anim.json, fr=30 op=60), so it finishes by
    // roughly clock == 5.5s. Run up to 6.9s total (< 2 x interval, so tmr fires only once) to
    // observe both.
    for (int i = 0; i < 59; ++i) {  // 59 x 0.1s == another 5.9s (total 6.9s).
        simClock += dt;
        doc->update(simClock);
        rt.tick(dt);
    }
    CHECK(rt.values()[6].n == doctest::Approx(1));  // tmr.count: fired exactly once.
    CHECK(rt.values()[7].n == doctest::Approx(1));  // finCnt.value: the Finished pulse arrived once.
}

// Docs/tasks/T-66a.md: edgeLastFired() is -1 for every edge right after load() (seeding does not
// count) and the runtime time in seconds for every edge that has since carried something.
TEST_CASE("FlowRuntime: edgeLastFired() timestamps each edge's last delivery") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadRuntimeDoc(shaper);
    FlowGraph graph = buildGraph();

    FlowRuntime rt;
    std::vector<std::string> errors;
    REQUIRE(rt.load(graph, *doc, errors) == Status::kOk);

    REQUIRE(rt.edgeLastFired().size() == graph.edges.size());
    for (float t : rt.edgeLastFired()) {
        CHECK(t == doctest::Approx(-1.0f));
    }

    // Edge 0 (#inc.Click -> cnt.increment) and the counter's own chain fire on a click, still at
    // runtime time 0 (no tick() yet); the #name/#darkToggle/#document edges have not.
    click(*doc, "inc");
    CHECK(rt.edgeLastFired()[0] == doctest::Approx(0.0f));
    CHECK(rt.edgeLastFired()[1] == doctest::Approx(0.0f));   // cnt.value -> ts.in
    CHECK(rt.edgeLastFired()[2] == doctest::Approx(0.0f));   // ts.out -> #count.setText
    CHECK(rt.edgeLastFired()[3] == doctest::Approx(-1.0f));  // #name.Change -> #echo.setText

    // #document.clock -> mth.a (edge 6) is polled every tick(), so it carries the accumulated
    // runtime time; the untouched #name edge stays at -1.
    for (int i = 0; i < 5; ++i) {
        rt.tick(0.1f);
    }
    CHECK(rt.edgeLastFired()[6] == doctest::Approx(0.5f).epsilon(0.05));
    CHECK(rt.edgeLastFired()[3] == doctest::Approx(-1.0f));

    // A failed load() leaves nothing to report.
    FlowGraph broken;
    NodeDesc bad;
    bad.id = "x";
    bad.type = "not-a-node-type";
    broken.nodes.push_back(bad);
    errors.clear();
    CHECK(rt.load(broken, *doc, errors) != Status::kOk);
    CHECK(rt.edgeLastFired().empty());
}

// Docs/tasks/T-66a.md (T-65b left-over): load() registers Document listeners that capture the
// runtime, so the runtime has to take them back off when it dies -- and when it is load()ed again.
TEST_CASE("FlowRuntime: destruction and re-load() release the document listeners") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadRuntimeDoc(shaper);
    FlowGraph graph = buildGraph();

    ElementRef count = doc->byId("count");
    REQUIRE(count.valid());

    {
        FlowRuntime rt;
        std::vector<std::string> errors;
        REQUIRE(rt.load(graph, *doc, errors) == Status::kOk);
        click(*doc, "inc");
        CHECK(count.text() == "1");
    }
    // The runtime is gone: its listeners must be gone with it (otherwise this click runs straight
    // into the freed Impl), and nothing may change.
    click(*doc, "inc");
    CHECK(count.text() == "1");

    // A second load() on the same runtime must not stack a second set of listeners on top of the
    // first: one click, one increment.
    FlowRuntime reloaded;
    std::vector<std::string> errors;
    REQUIRE(reloaded.load(graph, *doc, errors) == Status::kOk);
    errors.clear();
    REQUIRE(reloaded.load(graph, *doc, errors) == Status::kOk);
    click(*doc, "inc");
    CHECK(count.text() == "1");
    CHECK(reloaded.values()[0].n == doctest::Approx(1));  // cnt.value
}

// Docs/tasks/T-77.md: the five #document pointer ports, from the document-root PointerMove/
// PointerDown/PointerUp listeners.
TEST_CASE("FlowRuntime: #document pointer ports follow the pointer") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadRuntimeDoc(shaper);

    const std::string json = R"({
        "version": 1,
        "nodes": [
            { "id": "nx", "type": "math",
              "params": { "op": "map", "inLo": -1, "inHi": 1, "outLo": 0, "outHi": 1 } },
            { "id": "px", "type": "math", "params": { "op": "add" } },
            { "id": "py", "type": "math", "params": { "op": "add" } },
            { "id": "ny", "type": "math", "params": { "op": "add" } },
            { "id": "dn", "type": "toString", "params": { "format": "{}" } }
        ],
        "edges": [
            { "from": "#document.pointerNX", "to": "nx.a" },
            { "from": "nx.out", "to": "#bar.setProperty", "params": { "prop": "Opacity" } },
            { "from": "#document.pointerX", "to": "px.a" },
            { "from": "#document.pointerY", "to": "py.a" },
            { "from": "#document.pointerNY", "to": "ny.a" },
            { "from": "#document.pointerDown", "to": "dn.in" }
        ]
    })";
    FlowGraph graph;
    std::vector<std::string> parseErrors;
    REQUIRE(FlowGraph::parse(json, graph, parseErrors) == Status::kOk);

    FlowRuntime rt;
    std::vector<std::string> errors;
    REQUIRE(rt.load(graph, *doc, errors) == Status::kOk);
    for (const std::string& e : errors) {
        CAPTURE(e);
    }
    const std::vector<std::string> expectedNames = {"nx.out", "px.out", "py.out", "ny.out", "dn.out"};
    REQUIRE(rt.valueNames() == expectedNames);

    Rect viewport = doc->root().boundingBox();
    REQUIRE(viewport.w > 0);
    REQUIRE(viewport.h > 0);
    const double centerX = viewport.x + viewport.w * 0.5;
    const double centerY = viewport.y + viewport.h * 0.5;

    // Initial values: the viewport center, normalized 0, not pressed.
    rt.tick(0.0f);
    CHECK(rt.values()[1].n == doctest::Approx(centerX));  // px.out == pointerX
    CHECK(rt.values()[2].n == doctest::Approx(centerY));  // py.out == pointerY
    CHECK(rt.values()[3].n == doctest::Approx(0));        // ny.out == pointerNY
    CHECK(rt.values()[0].n == doctest::Approx(0.5));      // nx.out == map(pointerNX == 0)
    CHECK(rt.values()[4].s == "false");                   // dn.out == pointerDown

    // A move over #bar moves pointerX/Y, and the math(map) node's output with them.
    Vec2 target = centerOf(*doc, "bar");
    movePointer(*doc, target);
    rt.tick(0.0f);  // T-389: the frame's pointer values are delivered by the next tick().
    const double expectedNX = (static_cast<double>(target.x) - centerX) / (viewport.w * 0.5);
    const double expectedNY = (static_cast<double>(target.y) - centerY) / (viewport.h * 0.5);
    CHECK(rt.values()[1].n == doctest::Approx(target.x));
    CHECK(rt.values()[2].n == doctest::Approx(target.y));
    CHECK(rt.values()[3].n == doctest::Approx(expectedNY));
    CHECK(rt.values()[0].n == doctest::Approx(0.5 + expectedNX * 0.5));
    // ... and the mapped value reached #bar through setProperty.
    ElementRef bar = doc->byId("bar");
    REQUIRE(bar.valid());
    CHECK(bar.property(prop::Opacity).f == doctest::Approx(0.5 + expectedNX * 0.5));

    // PointerDown/PointerUp drive pointerDown. While a button is held the core sends every move
    // to the active element, so a drag past the viewport edge is observed too: the normalized
    // ports clamp to -1..1 there, the raw ones do not.
    pointerButton(*doc, target, /*down=*/true);
    rt.tick(0.0f);
    CHECK(rt.values()[4].s == "true");

    const float outsideX = viewport.x + viewport.w * 4.0f;
    movePointer(*doc, Vec2{outsideX, target.y});
    rt.tick(0.0f);
    CHECK(rt.values()[1].n == doctest::Approx(outsideX));  // pointerX: unclamped
    CHECK(rt.values()[0].n == doctest::Approx(1.0));       // map(pointerNX == +1)

    movePointer(*doc, target);
    rt.tick(0.0f);
    CHECK(rt.values()[0].n == doctest::Approx(0.5 + expectedNX * 0.5));

    pointerButton(*doc, target, /*down=*/false);
    rt.tick(0.0f);
    CHECK(rt.values()[4].s == "false");

    // A later tick() polls the same state; nothing drifts.
    rt.tick(0.1f);
    CHECK(rt.values()[1].n == doctest::Approx(target.x));
    CHECK(rt.values()[4].s == "false");
}

// §17.6: `setProperty` is one port per element but targets a different property per `prop` edge
// parameter, so two edges into the same `#id.setProperty` are legal when their `prop` differs
// and still rejected when it is the same (found while wiring the HUD demo's pointer tilt).
TEST_CASE("FlowRuntime: setProperty fan-in is counted per property") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadRuntimeDoc(shaper);
    auto load = [&](const char* secondProp) {
        const std::string json = std::string(R"({
            "version": 1,
            "nodes": [ { "id": "a", "type": "constant", "params": { "value": 0.5 } },
                       { "id": "b", "type": "constant", "params": { "value": 12 } } ],
            "edges": [ { "from": "a.out", "to": "#bar.setProperty", "params": { "prop": "Opacity" } },
                       { "from": "b.out", "to": "#bar.setProperty", "params": { "prop": ")") +
                                 secondProp + R"(" } } ]
        })";
        FlowGraph graph;
        std::vector<std::string> errors;
        if (FlowGraph::parse(json, graph, errors) != Status::kOk) {
            return false;
        }
        FlowRuntime runtime;
        return runtime.load(graph, *doc, errors) == Status::kOk;
    };
    CHECK(load("Rotation"));
    CHECK_FALSE(load("Opacity"));
}

// ---------------------------------------------------------------------------------------------
// T-123: the `playAnimation`/`stopAnimation` element ports and the difficulty-select v2 fixture.
// ---------------------------------------------------------------------------------------------

namespace {

/// Every "#id" an .xtflow's endpoints name (the document's own ports excluded).
std::vector<std::string> referencedElementIds(const FlowGraph& graph) {
    std::vector<std::string> ids;
    for (const EdgeDesc& e : graph.edges) {
        for (const std::string& endpoint : {e.from, e.to}) {
            if (endpoint.empty() || endpoint[0] != '#') {
                continue;
            }
            size_t dot = endpoint.find('.', 1);
            if (dot == std::string::npos) {
                continue;
            }
            std::string id = endpoint.substr(1, dot - 1);
            if (id == "document" || std::find(ids.begin(), ids.end(), id) != ids.end()) {
                continue;
            }
            ids.push_back(std::move(id));
        }
    }
    return ids;
}

/// A stand-in document for a flow whose real one is an imported design: one 20px-tall, clickable
/// div per referenced id, stacked in a column so each has its own hit box.
std::unique_ptr<Document> docWithIds(TextShaper& shaper, const std::vector<std::string>& ids) {
    const std::string stackHeight = std::to_string(40 + 20 * ids.size());
    std::string source = "<xt>\n<head><style>\nbody{display:flex;}\n";
    source += "#stack{display:flex;flex-direction:column;width:200px;height:" + stackHeight + "px;}\n";
    source += ".row{width:120px;height:20px;flex-shrink:0;}\n";
    source += "</style></head>\n<body>\n<div id=\"stack\">\n";
    for (const std::string& id : ids) {
        source += "<div id=\"" + id + "\" class=\"row\"></div>\n";
    }
    source += "</div>\n</body>\n</xt>\n";

    const std::string path = (xt::test::tempDir() / "xt_t123_ids.xt").string();
    {
        std::ofstream out(path, std::ios::binary);
        REQUIRE(out.good());
        out << source;
    }
    compile::CompileOptions options;
    compile::CompileResult result = compile::compileDocument(path, options);
    REQUIRE(result.ok());
    std::unique_ptr<Document> doc;
    Status status = Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &shaper, nullptr, doc);
    REQUIRE(status == Status::kOk);
    doc->setViewport(Rect{0, 0, 400, static_cast<float>(40 + 20 * ids.size())});
    doc->update(0.0f);
    return doc;
}

std::string outputOf(const FlowRuntime& rt, const std::string& name) {
    const std::vector<std::string>& names = rt.valueNames();
    std::span<const PortValue> values = rt.values();
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i] == name) {
            return values[i].s;
        }
    }
    FAIL("no such output: " << name);
    return {};
}

}  // namespace

TEST_CASE("FlowRuntime: playAnimation plays the element's @keyframes, stopAnimation clears it") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadRuntimeDoc(shaper);
    ElementRef spin = doc->byId("spin");
    REQUIRE(spin.valid());
    // The .xt declares `animation: spin 0.8s ease-in` on #spin, so one instance is already live.
    std::vector<uint32_t> declared = doc->animationsOf(spin);
    REQUIRE(declared.size() == 1);

    // `stopAnimation` first: a click clears whatever the element is playing.
    const std::string stopJson = R"({
        "nodes": [],
        "edges": [ { "from": "#inc.Click", "to": "#spin.stopAnimation" } ]
    })";
    FlowGraph stopGraph;
    std::vector<std::string> errors;
    REQUIRE(FlowGraph::parse(stopJson, stopGraph, errors) == Status::kOk);
    {
        FlowRuntime stopRt;
        REQUIRE(stopRt.load(stopGraph, *doc, errors) == Status::kOk);
        click(*doc, "inc");
        doc->update(0.016f);
        CHECK(doc->animationsOf(spin).empty());
    }

    // `playAnimation` then puts it back: the constant seeds at load(), so the port fires at once.
    const std::string playJson = R"({
        "nodes": [ { "id": "name", "type": "constant", "params": { "value": "spin" } } ],
        "edges": [ { "from": "name.out", "to": "#spin.playAnimation" } ]
    })";
    FlowGraph playGraph;
    REQUIRE(FlowGraph::parse(playJson, playGraph, errors) == Status::kOk);
    FlowRuntime rt;
    REQUIRE(rt.load(playGraph, *doc, errors) == Status::kOk);
    doc->update(0.016f);
    std::vector<uint32_t> played = doc->animationsOf(spin);
    REQUIRE(played.size() == 1);
    // Its duration/easing came from #spin's own `animation` declaration, not the 1s fallback.
    CHECK(doc->animationTimelineDuration(played[0]) == doctest::Approx(0.8f));
}

TEST_CASE("FlowRuntime: playAnimation with the same name again restarts the animation every time") {
    // T-270c: the runtime `animation` declaration lands in one of the element's two alternating
    // slots, so declaring the same text again still changes the cascaded offset -- which is what
    // the CSS animation diff restarts on -- while the runtime pool stays bounded.
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadRuntimeDoc(shaper);
    ElementRef spin = doc->byId("spin");
    REQUIRE(spin.valid());
    const std::string playJson = R"({
        "nodes": [ { "id": "name", "type": "constant", "params": { "value": "spin" } } ],
        "edges": [ { "from": "name.out", "to": "#spin.playAnimation" } ]
    })";
    FlowGraph playGraph;
    std::vector<std::string> errors;
    REQUIRE(FlowGraph::parse(playJson, playGraph, errors) == Status::kOk);
    std::vector<uint32_t> last = doc->animationsOf(spin);
    REQUIRE(last.size() == 1);
    float clock = 0;
    for (int i = 0; i < 6; ++i) {
        FlowRuntime rt;  // The constant seeds at load(): the port fires once per load.
        REQUIRE(rt.load(playGraph, *doc, errors) == Status::kOk);
        clock += 0.016f;
        doc->update(clock);
        CAPTURE(i);
        const std::vector<uint32_t> played = doc->animationsOf(spin);
        REQUIRE(played.size() == 1);
        CHECK(played[0] != last[0]);  // A new instance: restarted.
        CHECK(doc->animationTimelineDuration(played[0]) == doctest::Approx(0.8f));
        last = played;
    }
    CHECK(doc->runtimeAnimations().size() <= 2);
}

TEST_CASE("FlowRuntime: playAnimation with an unknown @keyframes name warns and plays nothing") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadRuntimeDoc(shaper);
    const size_t warningsBefore = doc->warnings().size();
    const std::string json = R"({
        "nodes": [ { "id": "name", "type": "constant", "params": { "value": "nosuch" } } ],
        "edges": [ { "from": "name.out", "to": "#spin.playAnimation" } ]
    })";
    FlowGraph graph;
    std::vector<std::string> errors;
    REQUIRE(FlowGraph::parse(json, graph, errors) == Status::kOk);
    FlowRuntime rt;
    REQUIRE(rt.load(graph, *doc, errors) == Status::kOk);
    doc->update(0.016f);
    CHECK(doc->animationsOf(doc->byId("spin")).empty());
    CHECK(doc->warnings().size() > warningsBefore);
}

TEST_CASE("FlowRuntime: setStyle takes several edges, each merged declaration by declaration") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadRuntimeDoc(shaper);
    const std::string json = R"({
        "nodes": [ { "id": "c1", "type": "constant", "params": { "value": "width: 33px" } },
                   { "id": "c2", "type": "constant", "params": { "value": "height: 11px" } } ],
        "edges": [ { "from": "c1.out", "to": "#bar.setStyle" },
                   { "from": "c2.out", "to": "#bar.setStyle" } ]
    })";
    FlowGraph graph;
    std::vector<std::string> errors;
    REQUIRE(FlowGraph::parse(json, graph, errors) == Status::kOk);
    FlowRuntime rt;
    REQUIRE(rt.load(graph, *doc, errors) == Status::kOk);
    doc->update(0.016f);
    Rect box = doc->byId("bar").boundingBox();
    CHECK(box.w == doctest::Approx(33));
    CHECK(box.h == doctest::Approx(11));
}

TEST_CASE("FlowRuntime: the difficulty-select v2 flow loads and its lookups follow the radio") {
    const std::string path = std::string(XT_TEST_FLOW_DIR) +
                             "/../import/fixtures/dc/difficulty-select/difficulty-select.v2.xtflow";
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::ostringstream buffer;
    buffer << in.rdbuf();

    FlowGraph graph;
    std::vector<std::string> errors;
    REQUIRE(FlowGraph::parse(buffer.str(), graph, errors) == Status::kOk);
    CHECK(graph.nodes.size() <= 40);

    NullTextShaper shaper;
    std::unique_ptr<Document> doc = docWithIds(shaper, referencedElementIds(graph));
    FlowRuntime rt;
    Status status = rt.load(graph, *doc, errors);
    for (const std::string& e : errors) {
        CAPTURE(e);
        CHECK(e.empty());
    }
    REQUIRE(status == Status::kOk);

    // The flow starts on SOLDIER (`radio`'s `initial`).
    CHECK(outputOf(rt, "nameText.out") == "SOLDIER");
    CHECK(outputOf(rt, "startLabel.out") == "START WITH DIFFICULTY SOLDIER");

    // Hovering the third level fires its `sel.set2`, and every lookup follows the one index.
    ElementRef veteran = doc->byId("diff-veteran");
    REQUIRE(veteran.valid());
    Rect box = veteran.boundingBox();
    InputEvent move;
    move.kind = InputEvent::PointerMove;
    move.pos = Vec2{box.x + box.w * 0.5f, box.y + box.h * 0.5f};
    doc->dispatchInput(move);

    CHECK(outputOf(rt, "nameText.out") == "VETERAN");
    CHECK(outputOf(rt, "damageText.out") == "65%");
    CHECK(outputOf(rt, "assistText.out") == "35%");
    CHECK(outputOf(rt, "startLabel.out") == "START WITH DIFFICULTY VETERAN");
    doc->update(0.016f);
    CHECK(doc->byId("diff-tab").text() == "VETERAN");
}

// Large-graph propagation and the feedback-loop guard (Docs/tasks/T-124.md).
// ---------------------------------------------------------------------------------------------

/// A graph with a `depth`-deep `select` chain driven by `#document.clock`, plus `fillers` unrelated
/// `toString` nodes hanging off one constant. The fillers exist only to give the graph width: the
/// old fanOut() rescanned every edge on every push, so the chain's cost grew with the whole graph.
FlowGraph deepSelectChain(int depth, int fillers) {
    FlowGraph graph;
    for (int i = 0; i < depth; ++i) {
        NodeDesc node;
        node.id = "s" + std::to_string(i);
        node.type = "select";
        graph.nodes.push_back(std::move(node));
    }
    NodeDesc source;
    source.id = "src";
    source.type = "constant";
    PortValue value;
    value.type = PortType::Number;
    value.n = 7;
    source.params["value"] = value;
    graph.nodes.push_back(std::move(source));
    for (int i = 0; i < fillers; ++i) {
        NodeDesc node;
        node.id = "t" + std::to_string(i);
        node.type = "toString";
        graph.nodes.push_back(std::move(node));
    }

    // `cond` is left unwired, so every select reads false and forwards `b` -- the chain input.
    // `a` must still be wired: since T-123 a select's output type is inferred from `a`/`b`, and
    // load() rejects a select with either input missing.
    EdgeDesc head;
    head.from = "#document.clock";
    head.to = "s0.b";
    graph.edges.push_back(std::move(head));
    for (int i = 1; i < depth; ++i) {
        EdgeDesc link;
        link.from = "s" + std::to_string(i - 1) + ".out";
        link.to = "s" + std::to_string(i) + ".b";
        graph.edges.push_back(std::move(link));
    }
    for (int i = 0; i < depth; ++i) {
        EdgeDesc alt;
        alt.from = "src.out";
        alt.to = "s" + std::to_string(i) + ".a";
        graph.edges.push_back(std::move(alt));
    }
    for (int i = 0; i < fillers; ++i) {
        EdgeDesc fill;
        fill.from = "src.out";
        fill.to = "t" + std::to_string(i) + ".in";
        graph.edges.push_back(std::move(fill));
    }
    return graph;
}

TEST_CASE("FlowRuntime: a 20-deep select chain in a wide graph ticks 1000 times quickly") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadRuntimeDoc(shaper);
    const FlowGraph graph = deepSelectChain(20, 200);
    FlowRuntime runtime;
    std::vector<std::string> errors;
    REQUIRE(runtime.load(graph, *doc, errors) == Status::kOk);

    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i) {
        runtime.tick(1.0f / 60.0f);
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    CAPTURE(ms);
    // T-267a: a wall-clock bound is only asserted in Release, at 10x the original 200 ms; the
    // structural guard against the graph-width cost is the next case.
#ifdef NDEBUG
    CHECK(ms < 2000.0);
#endif

    // The clock really did reach the far end of the chain (values() is in node order, so the last
    // select's "out" is slot 19).
    const std::span<const PortValue> values = runtime.values();
    REQUIRE(values.size() > 19);
    CHECK(values[19].n == doctest::Approx(1000.0f / 60.0f).epsilon(0.01));
    CHECK(runtime.runtimeErrors().empty());
}

namespace {

/// One wall-clock timing (ms) of 1000 ticks of `graph` (a deepSelectChain) on a fresh runtime.
double chainTicksMs(Document& doc, const FlowGraph& graph) {
    FlowRuntime runtime;
    std::vector<std::string> errors;
    REQUIRE(runtime.load(graph, doc, errors) == Status::kOk);
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i) {
        runtime.tick(1.0f / 60.0f);
    }
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace

// T-267a: the structural guard of the case above, asserted in every build. The fillers are not
// on the chain, so ticking it must not get dearer with them: the old fanOut() rescanned every edge
// on every push and cost ~10x here. The two widths are timed in turn, nine rounds, and the fastest
// of each is compared, so a burst of load on the machine hits both alike. T-267d: still timed, not
// counted, because FlowRuntime keeps no count of its evaluations to assert on.
TEST_CASE("FlowRuntime: ten times the graph width does not make the select chain three times dearer") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadRuntimeDoc(shaper);
    const FlowGraph narrowGraph = deepSelectChain(20, 200);
    const FlowGraph wideGraph = deepSelectChain(20, 2000);
    double narrow = 1e300;
    double wide = 1e300;
    for (int round = 0; round < 9; ++round) {
        narrow = std::min(narrow, chainTicksMs(*doc, narrowGraph));
        wide = std::min(wide, chainTicksMs(*doc, wideGraph));
    }
    CAPTURE(narrow);
    CAPTURE(wide);
    CHECK(wide < 3.0 * narrow);
}

/// A loop the load-time cycle check cannot see: it runs through the document. `#name.setValue`
/// raises a Change event, which increments the counter, which re-formats the string, which writes
/// `#name` again. Without the guard this recurses until the stack runs out.
TEST_CASE("FlowRuntime: a feedback loop through the document is stopped and reported") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadRuntimeDoc(shaper);
    const std::string json = R"({
        "version": 1,
        "nodes": [ { "id": "c", "type": "counter" },
                   { "id": "ts", "type": "toString" } ],
        "edges": [ { "from": "#name.Change", "to": "c.increment" },
                   { "from": "c.value", "to": "ts.in" },
                   { "from": "ts.out", "to": "#name.setValue" } ]
    })";
    FlowGraph graph;
    std::vector<std::string> errors;
    REQUIRE(FlowGraph::parse(json, graph, errors) == Status::kOk);
    FlowRuntime runtime;
    // load()'s own seeding already closes the loop (`counter` seeds 0, which formats to "0",
    // which writes `#name`, which raises Change), so the guard reports there and nothing runs away.
    REQUIRE(runtime.load(graph, *doc, errors) == Status::kOk);
    REQUIRE_FALSE(runtime.runtimeErrors().empty());
    CHECK(runtime.runtimeErrors().front().rfind("flow feedback loop:", 0) == 0);
    // Reported once per edge, not once per push.
    CHECK(runtime.runtimeErrors().size() <= graph.edges.size());

    // Driving it from the outside re-enters the same loop; it is still stopped, and the list stays
    // bounded by the edge count however often the loop runs (one report per edge, not per push).
    ElementRef name = doc->byId("name");
    REQUIRE(name.valid());
    name.setValue("start");
    name.setValue("again");
    CHECK(runtime.runtimeErrors().size() <= graph.edges.size());
}

// ---- T-540 (27 §27.4.3): flow.animatesWithContinuousSignal --------------------------------------

namespace {

/// Loads `json` against runtime_doc.xt and returns its continuityWarnings().
std::vector<std::string> continuityOf(const char* json) {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadRuntimeDoc(shaper);
    FlowGraph graph;
    std::vector<std::string> errors;
    REQUIRE(FlowGraph::parse(json, graph, errors) == Status::kOk);
    FlowRuntime rt;
    REQUIRE(rt.load(graph, *doc, errors) == Status::kOk);
    std::vector<std::string> out;
    rt.continuityWarnings(out);
    return out;
}

}  // namespace

TEST_CASE("FlowRuntime::continuityWarnings: continuous sources driving visual inputs are reported") {
    const std::string tail =
        " drives a visual property from a continuous signal; author it as a timeline or a bind: instead";
    {
        std::vector<std::string> w = continuityOf(R"({
            "nodes": [ { "id": "m", "type": "math",
                         "params": { "op": "map", "inLo": -1, "inHi": 1, "outLo": -8, "outHi": 8 } } ],
            "edges": [ { "from": "#document.pointerNX", "to": "m.a" },
                       { "from": "m.out", "to": "#bar.setProperty", "params": { "prop": "RotationY" } } ]
        })");
        REQUIRE(w.size() == 1);
        CHECK(w[0] == "flow.animatesWithContinuousSignal: edge m.out -> #bar.setProperty" + tail);
    }
    {
        std::vector<std::string> w = continuityOf(R"({
            "nodes": [ { "id": "c", "type": "clock" },
                       { "id": "f", "type": "format", "params": { "format": "{a}" } } ],
            "edges": [ { "from": "c.seconds", "to": "f.a" }, { "from": "f.out", "to": "#bar.setStyle" } ]
        })");
        REQUIRE(w.size() == 1);
        CHECK(w[0] == "flow.animatesWithContinuousSignal: edge f.out -> #bar.setStyle" + tail);
    }
    {
        std::vector<std::string> w = continuityOf(R"({
            "nodes": [ { "id": "s", "type": "toString", "params": { "format": "{}" } } ],
            "edges": [ { "from": "#anim.localTime", "to": "s.in" },
                       { "from": "s.out", "to": "#spin.playAnimation" } ]
        })");
        REQUIRE(w.size() == 1);
        CHECK(w[0] == "flow.animatesWithContinuousSignal: edge s.out -> #spin.playAnimation" + tail);
    }
}

TEST_CASE("FlowRuntime::continuityWarnings: cut, pulse, discrete and non-visual paths report nothing") {
    // compare cuts the mark; toggleClass is not a visual-property input.
    CHECK(continuityOf(R"({
        "nodes": [ { "id": "g", "type": "compare", "params": { "op": "gt" } },
                   { "id": "z", "type": "constant", "params": { "value": 0 } } ],
        "edges": [ { "from": "#document.pointerNX", "to": "g.a" }, { "from": "z.out", "to": "g.b" },
                   { "from": "g.true", "to": "#bar.toggleClass", "params": { "class": "x" } } ]
    })")
              .empty());
    // setText is exempt.
    CHECK(continuityOf(R"({
        "nodes": [ { "id": "c", "type": "clock" },
                   { "id": "s", "type": "toString", "params": { "format": "{}" } } ],
        "edges": [ { "from": "c.seconds", "to": "s.in" }, { "from": "s.out", "to": "#count.setText" } ]
    })")
              .empty());
    // lookup cuts the mark.
    CHECK(continuityOf(R"({
        "nodes": [ { "id": "r", "type": "radio", "params": { "count": 2 } },
                   { "id": "l", "type": "lookup", "params": { "values": [0.5, 1] } } ],
        "edges": [ { "from": "#inc.Click", "to": "r.set1" }, { "from": "r.index", "to": "l.index" },
                   { "from": "l.out", "to": "#bar.setProperty", "params": { "prop": "Opacity" } } ]
    })")
              .empty());
    // select.cond (fed by a compare of a continuous value) is not a value path.
    CHECK(continuityOf(R"({
        "nodes": [ { "id": "g", "type": "compare", "params": { "op": "gt" } },
                   { "id": "z", "type": "constant", "params": { "value": 0 } },
                   { "id": "a", "type": "constant", "params": { "value": "opacity: 1" } },
                   { "id": "b", "type": "constant", "params": { "value": "opacity: 0.5" } },
                   { "id": "sel", "type": "select" } ],
        "edges": [ { "from": "#document.pointerNX", "to": "g.a" }, { "from": "z.out", "to": "g.b" },
                   { "from": "g.out", "to": "sel.cond" }, { "from": "a.out", "to": "sel.a" },
                   { "from": "b.out", "to": "sel.b" }, { "from": "sel.out", "to": "#bar.setStyle" } ]
    })")
              .empty());
    // A pulse edge does not propagate (a pulse cannot reach playAnimation directly, so it goes
    // through a counter, whose value is discrete).
    CHECK(continuityOf(R"({
        "nodes": [ { "id": "t", "type": "timer", "params": { "interval": 1, "autostart": true } },
                   { "id": "n", "type": "counter", "params": {} },
                   { "id": "s", "type": "toString", "params": { "format": "{}" } } ],
        "edges": [ { "from": "t.tick", "to": "n.increment" }, { "from": "n.value", "to": "s.in" },
                   { "from": "s.out", "to": "#spin.playAnimation" } ]
    })")
              .empty());
    // timer.count is discrete, like counter.value.
    CHECK(continuityOf(R"({
        "nodes": [ { "id": "t", "type": "timer", "params": { "interval": 1, "autostart": true } },
                   { "id": "m", "type": "math", "params": { "op": "map", "inLo": 0, "inHi": 10,
                                                            "outLo": 0, "outHi": 1 } },
                   { "id": "s", "type": "toString", "params": { "format": "opacity: {}" } } ],
        "edges": [ { "from": "t.count", "to": "m.a" }, { "from": "m.out", "to": "s.in" },
                   { "from": "s.out", "to": "#bar.setStyle" } ]
    })")
              .empty());
}

// ---- T-541 (Docs/design/27-interaction.md §27.4.4): `#id.setState` --------------------------------

namespace {

/// Four buttons and a `#Box` with the state group `Tone: Normal|Warn|Danger` plus the standalone
/// state `Lit`.
std::unique_ptr<Document> stateBoxDoc(TextShaper& shaper) {
    const char* source =
        "<xt><head><style>body{display:flex;flex-direction:column;} button{width:50px;height:20px;}"
        "</style></head><body>"
        "<div id=\"Box\" state-groups=\"Tone: Normal|Warn|Danger; Lit: Lit|Dim\"></div>"
        "<button id=\"b1\">1</button><button id=\"b2\">2</button><button id=\"b3\">3</button>"
        "<button id=\"b4\">4</button></body></xt>";
    std::vector<compile::SourceFile> files{{"main.xt", source}};
    compile::CompileResult compiled =
        compile::compileDocumentFromStrings(files, "main.xt", compile::CompileOptions{});
    for (const compile::Diagnostic& d : compiled.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(compiled.ok());
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{compiled.ir.data(), compiled.ir.size()}, true, &shaper, nullptr, doc) ==
            Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 400});
    doc->update(0.0f);
    return doc;
}

}  // namespace

TEST_CASE("FlowRuntime: setState toggles a custom state, a state group stays exclusive") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = stateBoxDoc(shaper);
    FlowGraph graph;
    std::vector<std::string> errors;
    const std::string json = R"({"nodes":[],"edges":[
        {"from":"#b1.Click","to":"#Box.setState","params":{"state":"Warn","mode":"toggle"}},
        {"from":"#b2.Click","to":"#Box.setState","params":{"state":"Danger"}},
        {"from":"#b3.Click","to":"#Box.setState","params":{"state":"Danger","mode":"off"}},
        {"from":"#b4.Click","to":"#Box.setState","params":{"state":"Nope"}}]})";
    REQUIRE(FlowGraph::parse(json, graph, errors) == Status::kOk);
    FlowRuntime flow;
    REQUIRE(flow.load(graph, *doc, errors) == Status::kOk);
    ElementRef box = doc->byId("Box");
    REQUIRE(box.valid());

    CHECK_FALSE(doc->hasCustomState(box, "Warn"));
    click(*doc, "b1");  // toggle -> on
    CHECK(doc->hasCustomState(box, "Warn"));
    click(*doc, "b1");  // toggle -> off: twice is back to the start.
    CHECK_FALSE(doc->hasCustomState(box, "Warn"));
    click(*doc, "b1");
    CHECK(doc->hasCustomState(box, "Warn"));

    click(*doc, "b2");  // Danger on clears Warn: the group's rule is Document::setCustomState's.
    CHECK(doc->hasCustomState(box, "Danger"));
    CHECK_FALSE(doc->hasCustomState(box, "Warn"));
    click(*doc, "b2");  // on again: stays on.
    CHECK(doc->hasCustomState(box, "Danger"));
    click(*doc, "b3");  // mode off
    CHECK_FALSE(doc->hasCustomState(box, "Danger"));
    click(*doc, "b4");  // an undeclared name changes nothing and breaks nothing.
    CHECK_FALSE(doc->hasCustomState(box, "Danger"));
    CHECK(flow.runtimeErrors().empty());
}

TEST_CASE("FlowRuntime: setState reaches the built-in aliases (Disabled)") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = stateBoxDoc(shaper);
    FlowGraph graph;
    std::vector<std::string> errors;
    const std::string json = R"({"nodes":[],"edges":[
        {"from":"#b1.Click","to":"#Box.setState","params":{"state":"Disabled","mode":"toggle"}},
        {"from":"#b2.Click","to":"#Box.setState","params":{"state":"Disabled","mode":"off"}}]})";
    REQUIRE(FlowGraph::parse(json, graph, errors) == Status::kOk);
    FlowRuntime flow;
    REQUIRE(flow.load(graph, *doc, errors) == Status::kOk);
    ElementRef box = doc->byId("Box");
    CHECK_FALSE(doc->hasCustomState(box, "Disabled"));
    click(*doc, "b1");
    CHECK(doc->hasCustomState(box, "Disabled"));
    click(*doc, "b2");
    CHECK_FALSE(doc->hasCustomState(box, "Disabled"));
}

TEST_CASE("FlowRuntime: a state node drives #Box.setState through enter<Name>") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = stateBoxDoc(shaper);
    FlowGraph graph;
    std::vector<std::string> errors;
    const std::string json = R"({"nodes":[
        {"id":"tone","type":"state","params":{"states":["Normal","Warn","Danger"],
          "transitions":[{"from":"*","on":"warn","to":"Warn"},{"from":"Warn","on":"worse","to":"Danger"},
                         {"from":"*","on":"calm","to":"Normal"}]}}],
      "edges":[
        {"from":"#b1.Click","to":"tone.warn"},{"from":"#b2.Click","to":"tone.worse"},
        {"from":"#b3.Click","to":"tone.calm"},
        {"from":"tone.enterWarn","to":"#Box.setState","params":{"state":"Warn"}},
        {"from":"tone.enterDanger","to":"#Box.setState","params":{"state":"Danger"}},
        {"from":"tone.enterNormal","to":"#Box.setState","params":{"state":"Normal"}}]})";
    REQUIRE(FlowGraph::parse(json, graph, errors) == Status::kOk);
    FlowRuntime flow;
    REQUIRE(flow.load(graph, *doc, errors) == Status::kOk);
    ElementRef box = doc->byId("Box");
    click(*doc, "b1");
    CHECK(doc->hasCustomState(box, "Warn"));
    click(*doc, "b2");
    CHECK(doc->hasCustomState(box, "Danger"));
    CHECK_FALSE(doc->hasCustomState(box, "Warn"));
    click(*doc, "b3");
    CHECK(doc->hasCustomState(box, "Normal"));
    CHECK_FALSE(doc->hasCustomState(box, "Danger"));
}

// ---- T-543 (Docs/design/27-interaction.md §27.4.5): throttle -----------------------------------

namespace {

/// Declared out ports a host sets (`Hud.v` number, `Hud.rate` number), two buttons for pulse
/// inputs (#b1 in, #b2 reset).
constexpr const char* kThrottleDoc = R"X(<xt><head><style>
  #b1 { position: absolute; left: 0px; top: 0px; width: 40px; height: 20px; }
  #b2 { position: absolute; left: 0px; top: 30px; width: 40px; height: 20px; }
</style></head>
<body id="Root"><div id="Hud" port:v="number out" port:rate="number out"></div>
  <div id="b1"></div><div id="b2"></div></body></xt>)X";

std::unique_ptr<Document> throttleDoc(TextShaper& shaper) {
    const compile::SourceFile file{"throttle.xt", kThrottleDoc};
    compile::CompileOptions options;
    options.behavior = 1;
    compile::CompileResult result = compile::compileDocumentFromStrings({&file, 1}, "throttle.xt", options);
    REQUIRE(result.ok());
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &shaper, nullptr, doc) ==
            Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 300});
    doc->update(0.0f);
    return doc;
}

void clickButton(Document& doc, float y) {
    InputEvent ev;
    ev.kind = InputEvent::PointerDown;
    ev.pos = Vec2{20.0f, y};
    doc.dispatchInput(ev);
    ev.kind = InputEvent::PointerUp;
    doc.dispatchInput(ev);
}

/// Every `emit` of a runtime in order (what it carried and the step it was sent in).
struct Sent {
    std::vector<double> values;
    std::vector<int> steps;
    int step = 0;
    void attach(FlowRuntime& rt) {
        rt.setHostEventListener([this](const FlowHostEvent& ev) {
            if (ev.kind == FlowHostEventKind::Emit) {
                values.push_back(ev.value.n);
                steps.push_back(step);
            }
        });
    }
};

double numberOf(const FlowRuntime& rt, const std::string& name) {
    const std::vector<std::string>& names = rt.valueNames();
    std::span<const PortValue> values = rt.values();
    for (size_t i = 0; i < names.size() && i < values.size(); ++i) {
        if (names[i] == name) {
            return values[i].n;
        }
    }
    FAIL("no such output: " << name);
    return 0;
}

constexpr const char* kValueThrottle = R"({
    "nodes": [
        { "id": "t", "type": "throttle", "params": { "unit": "hz", "rate": 10, "trailing": TRAILING } },
        { "id": "e", "type": "emit", "params": { "port": "t" } }
    ],
    "edges": [
        { "from": "#Hud.v", "to": "t.in" },
        { "from": "t.out", "to": "e.value" }, { "from": "t.passed", "to": "e.trigger" },
        { "from": "#b2.Click", "to": "t.reset" }
    ]
})";

std::string valueThrottleJson(const char* unit, const char* rate, bool trailing) {
    std::string json = kValueThrottle;
    auto replace = [&json](const std::string& from, const std::string& to) {
        const size_t at = json.find(from);
        REQUIRE(at != std::string::npos);
        json.replace(at, from.size(), to);
    };
    replace("\"hz\"", std::string("\"") + unit + "\"");
    replace("10", rate);
    replace("TRAILING", trailing ? "true" : "false");
    return json;
}

void loadJson(FlowRuntime& rt, Document& doc, const std::string& json) {
    FlowGraph graph;
    std::vector<std::string> errors;
    REQUIRE(FlowGraph::parse(json, graph, errors) == Status::kOk);
    REQUIRE(rt.load(graph, doc, errors) == Status::kOk);
}

}  // namespace

TEST_CASE(
    "T-543 throttle hz 10, trailing: leading A, B and C held, C sent at the window's end, then silence") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = throttleDoc(shaper);
    FlowRuntime rt;
    Sent sent;
    sent.attach(rt);
    loadJson(rt, *doc, valueThrottleJson("hz", "10", true));
    const PortId v = doc->lookupPort("Hud.v");
    REQUIRE(v != kInvalidPort);

    // The sheet's A, B, C are 1, 2, 3.
    std::string timeline;
    double t = 0;
    auto step = [&](float inputOrNaN, float dt) {
        if (!std::isnan(inputOrNaN)) {
            REQUIRE(doc->setPort(v, Value::ofFloat(inputOrNaN)) == Status::kOk);
        }
        rt.tick(dt);
        t += static_cast<double>(dt);
        char line[160];
        std::snprintf(line, sizeof(line), "t=%.2f in=%s sent=%zu out=%g dropped=%g\n", t,
                      std::isnan(inputOrNaN) ? "-" : std::to_string(static_cast<int>(inputOrNaN)).c_str(),
                      sent.values.size(), sent.values.empty() ? 0.0 : sent.values.back(),
                      numberOf(rt, "t.dropped"));
        timeline += line;
    };
    const float none = std::nanf("");
    step(1.0f, 0.0f);  // t=0: A goes out at once.
    CHECK(sent.values == std::vector<double>{1.0});
    step(2.0f, 0.02f);  // t=0.02: B is held.
    CHECK(sent.values.size() == 1);
    CHECK(numberOf(rt, "t.dropped") == 0.0);
    step(3.0f, 0.03f);  // t=0.05: C replaces B.
    CHECK(sent.values.size() == 1);
    CHECK(numberOf(rt, "t.dropped") == 1.0);
    step(none, 0.05f);  // t=0.1: the window ends, C goes out.
    CHECK(sent.values == std::vector<double>{1.0, 3.0});
    CHECK(numberOf(rt, "t.out") == 3.0);
    CHECK(numberOf(rt, "t.dropped") == 1.0);  // The value sent at the end is not a dropped one.
    for (int i = 0; i < 6; ++i) {             // No input: no output (not a metronome).
        step(none, 0.05f);
    }
    CHECK(sent.values == std::vector<double>{1.0, 3.0});
    MESSAGE("hz 10 timeline (A=1, B=2, C=3):\n" << timeline);
}

TEST_CASE(
    "T-543 throttle hz 10, trailing false: only A, and the first input after the window passes at once") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = throttleDoc(shaper);
    FlowRuntime rt;
    Sent sent;
    sent.attach(rt);
    loadJson(rt, *doc, valueThrottleJson("hz", "10", false));
    const PortId v = doc->lookupPort("Hud.v");
    auto step = [&](float input, float dt) {
        REQUIRE(doc->setPort(v, Value::ofFloat(input)) == Status::kOk);
        rt.tick(dt);
    };
    step(1.0f, 0.0f);
    step(2.0f, 0.02f);
    step(3.0f, 0.03f);
    rt.tick(0.05f);  // t=0.1: nothing is held, nothing is sent.
    CHECK(sent.values == std::vector<double>{1.0});
    CHECK(numberOf(rt, "t.dropped") == 2.0);
    step(4.0f, 0.01f);  // t=0.11: a new input, the window is over.
    CHECK(sent.values == std::vector<double>{1.0, 4.0});
}

TEST_CASE("T-543 throttle frames 3 on a pulse: sent in ticks 1, 4 and 7; trailing false drops the rest") {
    for (bool trailing : {true, false}) {
        CAPTURE(trailing);
        NullTextShaper shaper;
        std::unique_ptr<Document> doc = throttleDoc(shaper);
        FlowRuntime rt;
        Sent sent;
        sent.attach(rt);
        loadJson(rt, *doc,
                 R"({
            "nodes": [
                { "id": "t", "type": "throttle", "params": { "unit": "frames", "rate": 3, "trailing": )" +
                     std::string(trailing ? "true" : "false") + R"( } },
                { "id": "e", "type": "emit", "params": { "port": "t" } }
            ],
            "edges": [ { "from": "#b1.Click", "to": "t.in" }, { "from": "t.out", "to": "e.trigger" } ]
        })");
        for (int tick = 1; tick <= 7; ++tick) {
            sent.step = tick;
            rt.tick(1.0f / 60.0f);
            clickButton(*doc, 10.0f);
        }
        CHECK(sent.steps == std::vector<int>{1, 4, 7});
        if (trailing) {
            // The click of tick 7 is held and goes out when the next window ends (tick 10).
            for (int tick = 8; tick <= 12; ++tick) {
                sent.step = tick;
                rt.tick(1.0f / 60.0f);
            }
            CHECK(sent.steps == std::vector<int>{1, 4, 7, 10});
        } else {
            for (int tick = 8; tick <= 12; ++tick) {
                sent.step = tick;
                rt.tick(1.0f / 60.0f);
            }
            CHECK(sent.steps == std::vector<int>{1, 4, 7});
            CHECK(numberOf(rt, "t.dropped") == 4.0);
        }
    }
}

TEST_CASE("T-543 throttle seconds: the window is measured in seconds of tick time") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = throttleDoc(shaper);
    FlowRuntime rt;
    Sent sent;
    sent.attach(rt);
    loadJson(rt, *doc, valueThrottleJson("seconds", "0.5", true));
    const PortId v = doc->lookupPort("Hud.v");
    for (int i = 1; i <= 12; ++i) {  // An input every 0.1 s for 1.2 s.
        REQUIRE(doc->setPort(v, Value::ofFloat(static_cast<float>(i))) == Status::kOk);
        rt.tick(0.1f);
    }
    // 1 at t=0.1 opens a window to t=0.6; 2..5 are held and 6, arriving as it ends, replaces them;
    // the next window ends at t=1.1, where 11 replaces 7..10.
    CHECK(sent.values == std::vector<double>{1.0, 6.0, 11.0});
}

TEST_CASE("T-543 throttle: the rate edge changes the window while it runs") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = throttleDoc(shaper);
    FlowRuntime rt;
    Sent sent;
    sent.attach(rt);
    loadJson(rt, *doc, R"({
        "nodes": [
            { "id": "t", "type": "throttle", "params": { "unit": "hz", "rate": 1 } },
            { "id": "e", "type": "emit", "params": { "port": "t" } }
        ],
        "edges": [
            { "from": "#Hud.v", "to": "t.in" }, { "from": "#Hud.rate", "to": "t.rate" },
            { "from": "t.out", "to": "e.value" }, { "from": "t.passed", "to": "e.trigger" }
        ]
    })");
    const PortId v = doc->lookupPort("Hud.v");
    const PortId rate = doc->lookupPort("Hud.rate");
    REQUIRE(doc->setPort(v, Value::ofFloat(1.0f)) == Status::kOk);
    rt.tick(0.0f);  // t=0: sent; the 1 Hz window runs to t=1.
    REQUIRE(doc->setPort(v, Value::ofFloat(2.0f)) == Status::kOk);
    rt.tick(0.2f);  // t=0.2: held.
    CHECK(sent.values == std::vector<double>{1.0});
    // 1 Hz -> 100 Hz with a held value: the window is re-measured from the last send (0 + 0.01),
    // it is over, and the held value goes out now.
    REQUIRE(doc->setPort(rate, Value::ofFloat(100.0f)) == Status::kOk);
    rt.tick(0.0f);
    CHECK(sent.values == std::vector<double>{1.0, 2.0});
    // Back to 1 Hz: the window (from the send at t=0.2) is open until t=1.2; a value is held.
    REQUIRE(doc->setPort(rate, Value::ofFloat(1.0f)) == Status::kOk);
    rt.tick(0.0f);
    REQUIRE(doc->setPort(v, Value::ofFloat(3.0f)) == Status::kOk);
    rt.tick(0.1f);
    CHECK(sent.values == std::vector<double>{1.0, 2.0});
    // 0: no limit. The held value is released, and every later input passes at once.
    REQUIRE(doc->setPort(rate, Value::ofFloat(0.0f)) == Status::kOk);
    rt.tick(0.0f);
    CHECK(sent.values == std::vector<double>{1.0, 2.0, 3.0});
    for (float input : {4.0f, 5.0f, 6.0f}) {
        REQUIRE(doc->setPort(v, Value::ofFloat(input)) == Status::kOk);
        rt.tick(0.001f);
    }
    CHECK(sent.values == std::vector<double>{1.0, 2.0, 3.0, 4.0, 5.0, 6.0});
}

TEST_CASE("T-543 throttle: a rate that is NaN, negative or infinite does not limit; frames 0.4 is 1") {
    struct Case {
        const char* label;
        double rate;
        bool limits;
    };
    const Case cases[] = {{"NaN", std::numeric_limits<double>::quiet_NaN(), false},
                          {"-1", -1.0, false},
                          {"+inf", std::numeric_limits<double>::infinity(), false},
                          {"0", 0.0, false},
                          {"frames 0.4", 0.4, true}};
    for (const Case& c : cases) {
        CAPTURE(c.label);
        NullTextShaper shaper;
        std::unique_ptr<Document> doc = throttleDoc(shaper);
        FlowRuntime rt;
        Sent sent;
        sent.attach(rt);
        FlowGraph graph;
        std::vector<std::string> errors;
        REQUIRE(FlowGraph::parse(R"({
            "nodes": [
                { "id": "t", "type": "throttle", "params": { "unit": "frames", "rate": 1 } },
                { "id": "e", "type": "emit", "params": { "port": "t" } }
            ],
            "edges": [ { "from": "#b1.Click", "to": "t.in" }, { "from": "t.out", "to": "e.trigger" } ]
        })",
                                 graph, errors) == Status::kOk);
        graph.nodes[0].params["rate"].n = c.rate;
        if (std::string(c.label) != "frames 0.4") {
            graph.nodes[0].params["unit"].s = "hz";  // Window = 1 / rate for a finite positive one.
        }
        REQUIRE(rt.load(graph, *doc, errors) == Status::kOk);
        rt.tick(0.016f);
        clickButton(*doc, 10.0f);
        clickButton(*doc, 10.0f);
        clickButton(*doc, 10.0f);
        if (c.limits) {
            CHECK(sent.values.size() == 1);  // Window of one tick: two held, sent next tick.
            CHECK(numberOf(rt, "t.dropped") == 1.0);
            rt.tick(0.016f);
            CHECK(sent.values.size() == 2);
        } else {
            CHECK(sent.values.size() == 3);  // Every input passes.
            CHECK(numberOf(rt, "t.dropped") == 0.0);
        }
    }
}

TEST_CASE("T-543 throttle reset: clears the window and the held value, keeps `dropped`") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = throttleDoc(shaper);
    FlowRuntime rt;
    Sent sent;
    sent.attach(rt);
    loadJson(rt, *doc, valueThrottleJson("hz", "10", true));
    const PortId v = doc->lookupPort("Hud.v");
    REQUIRE(doc->setPort(v, Value::ofFloat(1.0f)) == Status::kOk);
    rt.tick(0.0f);
    REQUIRE(doc->setPort(v, Value::ofFloat(2.0f)) == Status::kOk);
    rt.tick(0.01f);
    REQUIRE(doc->setPort(v, Value::ofFloat(3.0f)) == Status::kOk);
    rt.tick(0.01f);
    CHECK(numberOf(rt, "t.dropped") == 1.0);
    clickButton(*doc, 40.0f);  // reset: 3 is forgotten, the window is open again.
    rt.tick(0.2f);
    CHECK(sent.values == std::vector<double>{1.0});
    CHECK(numberOf(rt, "t.dropped") == 1.0);
    REQUIRE(doc->setPort(v, Value::ofFloat(4.0f)) == Status::kOk);
    rt.tick(0.001f);  // Inside what would have been the old window: sent at once.
    CHECK(sent.values == std::vector<double>{1.0, 4.0});
}

TEST_CASE("T-543 throttle: a held value goes out once at the window's end, then nothing waits") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = throttleDoc(shaper);
    FlowRuntime rt;
    Sent sent;
    sent.attach(rt);
    // Two throttles on one source; the second never gets a second input, so it never waits.
    loadJson(rt, *doc, R"({
        "nodes": [
            { "id": "a", "type": "throttle", "params": { "unit": "hz", "rate": 10 } },
            { "id": "b", "type": "throttle", "params": { "unit": "hz", "rate": 10 } },
            { "id": "ea", "type": "emit", "params": { "port": "a" } },
            { "id": "eb", "type": "emit", "params": { "port": "b" } }
        ],
        "edges": [
            { "from": "#b1.Click", "to": "a.in" }, { "from": "#b2.Click", "to": "b.in" },
            { "from": "a.out", "to": "ea.trigger" }, { "from": "b.out", "to": "eb.trigger" }
        ]
    })");
    rt.tick(0.0f);
    clickButton(*doc, 10.0f);
    clickButton(*doc, 10.0f);  // `a` holds one.
    clickButton(*doc, 40.0f);  // `b` passes one.
    CHECK(sent.values.size() == 2);
    rt.tick(0.05f);
    CHECK(sent.values.size() == 2);
    rt.tick(0.05f);  // `a`'s window ends: its held click goes out.
    CHECK(sent.values.size() == 3);
    for (int i = 0; i < 10; ++i) {
        rt.tick(0.1f);
    }
    CHECK(sent.values.size() == 3);
}

TEST_CASE("T-543 continuityWarnings: filter and throttle keep the mark like math; a port write is silent") {
    {
        // Positive: pointerNX -> deadband -> setProperty.
        std::vector<std::string> w = continuityOf(R"({
            "nodes": [ { "id": "d", "type": "filter", "params": { "op": "deadband", "threshold": 0.02 } } ],
            "edges": [ { "from": "#document.pointerNX", "to": "d.in" },
                       { "from": "d.out", "to": "#bar.setProperty", "params": { "prop": "RotationY" } } ]
        })");
        REQUIRE(w.size() == 1);
        CHECK(w[0].find("edge d.out -> #bar.setProperty") != std::string::npos);
        // `passed` carries no mark (and is a pulse).
    }
    {
        // The mark runs through a throttle too; its `dropped` never carries it.
        std::vector<std::string> w = continuityOf(R"({
            "nodes": [ { "id": "d", "type": "filter", "params": { "op": "deadband", "threshold": 0.02 } },
                       { "id": "t", "type": "throttle", "params": { "unit": "hz", "rate": 10 } } ],
            "edges": [ { "from": "#document.pointerNX", "to": "d.in" }, { "from": "d.out", "to": "t.in" },
                       { "from": "t.out", "to": "#bar.setProperty", "params": { "prop": "RotationY" } },
                       { "from": "t.dropped", "to": "#bar.setProperty", "params": { "prop": "Opacity" } } ]
        })");
        REQUIRE(w.size() == 1);
        CHECK(w[0].find("edge t.out -> #bar.setProperty") != std::string::npos);
    }
    {
        // Negative: pointerNX -> deadband -> throttle -> a declared port is not a visual input.
        NullTextShaper shaper;
        const compile::SourceFile file{
            "cont.xt",
            "<xt><head><style></style></head><body id=\"Root\"><div id=\"Hud\" port:nx=\"number in\"></div>"
            "</body></xt>"};
        compile::CompileOptions options;
        compile::CompileResult result = compile::compileDocumentFromStrings({&file, 1}, "cont.xt", options);
        REQUIRE(result.ok());
        std::unique_ptr<Document> doc;
        REQUIRE(Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &shaper, nullptr, doc) ==
                Status::kOk);
        doc->setViewport(Rect{0, 0, 400, 400});
        doc->update(0.0f);
        FlowGraph graph;
        std::vector<std::string> errors;
        REQUIRE(FlowGraph::parse(R"({
            "nodes": [ { "id": "d", "type": "filter", "params": { "op": "deadband", "threshold": 0.02 } },
                       { "id": "t", "type": "throttle", "params": { "unit": "hz", "rate": 10 } } ],
            "edges": [ { "from": "#document.pointerNX", "to": "d.in" }, { "from": "d.out", "to": "t.in" },
                       { "from": "t.out", "to": "#Hud.nx" } ]
        })",
                                 graph, errors) == Status::kOk);
        FlowRuntime rt;
        REQUIRE(rt.load(graph, *doc, errors) == Status::kOk);
        std::vector<std::string> out;
        rt.continuityWarnings(out);
        CHECK(out.empty());
    }
}
