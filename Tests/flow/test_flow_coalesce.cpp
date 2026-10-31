#include <memory>
#include <span>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/flow/flow_graph.h"
#include "xt/flow/flow_runtime.h"
#include "xt/input.h"
#include "xt/text_shaper.h"

/// T-389 (Docs/design/27-interaction.md §27.4.1, 2026-10-21 ruling): value-type document outputs
/// are coalesced per frame -- whatever the input dispatch reported is delivered once, from the
/// next FlowRuntime::tick(), as one batch evaluated in topological order -- while pulse / event
/// outputs stay synchronous, one delivery per event. T-407 (43 §43.3): only from behavior version
/// 1 (`flow-signal-coalescing`, IR META `behavior`) on; behavior 0 delivers every event at once.

using namespace xt;
using namespace xt::flow;

namespace {

constexpr const char* kDoc = R"X(<xt><head><style>
  #Hot { position: absolute; left: 0px; top: 0px; width: 100px; height: 100px; }
  #Btn { position: absolute; left: 200px; top: 0px; width: 40px; height: 40px; }
</style></head>
<body id="Root"><div id="Hud" port:text="string in" port:level="number out" port:ping="event out"></div>
  <div id="Hot"></div><div id="Btn"></div></body></xt>)X";

/// T-407: `behavior` is the compiled META `behavior` (1 = the coalescing these T-389 cases test).
std::unique_ptr<Document> loadDoc(TextShaper& shaper, uint32_t behavior = 1) {
    const compile::SourceFile file{"coalesce.xt", kDoc};
    compile::CompileOptions options;
    options.behavior = behavior;
    compile::CompileResult result = compile::compileDocumentFromStrings({&file, 1}, "coalesce.xt", options);
    for (const compile::Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &shaper, nullptr, doc) ==
            Status::kOk);
    REQUIRE(doc->behavior() == behavior);
    doc->setViewport(Rect{0, 0, 400, 400});
    doc->update(0.0f);
    return doc;
}

/// Every Changed event of `#Hud.text` (what the graph's sink wrote), in order.
struct TextSink {
    std::vector<std::string> writes;
    uint32_t listener = 0;
    void attach(Document& doc) {
        const PortId text = doc.lookupPort("Hud.text");
        REQUIRE(text != kInvalidPort);
        Document* d = &doc;
        listener = doc.addPortListener([this, text, d](const PortEvent& ev) {
            if (ev.port == text && ev.kind == PortEventKind::Changed) {
                writes.push_back(d->portText(text));
            }
        });
    }
};

void move(Document& doc, float x, float y) {
    InputEvent ev;
    ev.kind = InputEvent::PointerMove;
    ev.pos = Vec2{x, y};
    doc.dispatchInput(ev);
}

void click(Document& doc, float x, float y) {
    InputEvent ev;
    ev.kind = InputEvent::PointerDown;
    ev.pos = Vec2{x, y};
    doc.dispatchInput(ev);
    ev.kind = InputEvent::PointerUp;
    doc.dispatchInput(ev);
}

/// The current value of node output `name` ("<node>.<port>") as values() reports it.
const PortValue* findValue(const FlowRuntime& rt, const std::string& name) {
    const std::vector<std::string>& names = rt.valueNames();
    std::span<const PortValue> values = rt.values();
    for (size_t i = 0; i < names.size() && i < values.size(); ++i) {
        if (names[i] == name) {
            return &values[i];
        }
    }
    return nullptr;
}

double valueOf(const FlowRuntime& rt, const std::string& name) {
    const PortValue* v = findValue(rt, name);
    REQUIRE(v != nullptr);
    return v->n;
}

std::string textOf(const FlowRuntime& rt, const std::string& name) {
    const PortValue* v = findValue(rt, name);
    REQUIRE(v != nullptr);
    return v->s;
}

void loadGraph(FlowRuntime& rt, Document& doc, const std::string& json) {
    FlowGraph graph;
    std::vector<std::string> errors;
    REQUIRE(FlowGraph::parse(json, graph, errors) == Status::kOk);
    REQUIRE(rt.load(graph, doc, errors) == Status::kOk);
    for (const std::string& e : errors) {
        CAPTURE(e);
    }
}

}  // namespace

TEST_CASE("T-389: 100 pointer moves in one frame reach a shared format node once, with the last value") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadDoc(shaper);
    FlowRuntime rt;
    // pointerX feeds the format node twice (directly and through `add`), pointerY once: all three
    // inputs change in the same frame, the node is evaluated once.
    loadGraph(rt, *doc, R"({
        "version": 1,
        "nodes": [
            { "id": "r", "type": "constant", "params": { "value": 1000 } },
            { "id": "add", "type": "math", "params": { "op": "add" } },
            { "id": "f", "type": "format", "params": { "format": "{a},{b},{c}" } }
        ],
        "edges": [
            { "from": "#document.pointerX", "to": "add.a" },
            { "from": "r.out", "to": "add.b" },
            { "from": "#document.pointerX", "to": "f.a" },
            { "from": "#document.pointerY", "to": "f.b" },
            { "from": "add.out", "to": "f.c" },
            { "from": "f.out", "to": "#Hud.text" }
        ]
    })");
    rt.tick(0.0f);  // The initial (viewport-center) reading.
    TextSink sink;
    sink.attach(*doc);

    for (int i = 0; i < 100; ++i) {
        move(*doc, 300.0f + static_cast<float>(i), 200.0f + static_cast<float>(i));
    }
    CHECK(sink.writes.empty());  // Nothing propagates during the input dispatch.
    rt.tick(1.0f / 60.0f);
    REQUIRE(sink.writes.size() == 1);
    CHECK(sink.writes[0] == "399,299,1399");

    // The edges still record when they last delivered (the flow panel shows it).
    std::span<const float> fired = rt.edgeLastFired();
    REQUIRE(fired.size() == 6);
    CHECK(fired[0] == doctest::Approx(1.0f / 60.0f));  // pointerX -> add.a
    CHECK(fired[5] == doctest::Approx(1.0f / 60.0f));  // f.out -> #Hud.text
    // values() shows the frame's result.
    CHECK(textOf(rt, "f.out") == "399,299,1399");

    // A frame without movement delivers nothing.
    rt.tick(1.0f / 60.0f);
    CHECK(sink.writes.size() == 1);
}

TEST_CASE("T-389: pulses stay one delivery per event, and see the values reported before them") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadDoc(shaper);
    FlowRuntime rt;
    loadGraph(rt, *doc, R"({
        "version": 1,
        "nodes": [
            { "id": "clicks", "type": "counter" },
            { "id": "pings", "type": "counter" },
            { "id": "ts", "type": "toString" }
        ],
        "edges": [
            { "from": "#Btn.Click", "to": "clicks.increment" },
            { "from": "#Hud.ping", "to": "pings.increment" },
            { "from": "#document.pointerX", "to": "ts.in" },
            { "from": "ts.out", "to": "#Hud.text" }
        ]
    })");
    rt.tick(0.0f);
    TextSink sink;
    sink.attach(*doc);

    for (int i = 0; i < 3; ++i) {
        click(*doc, 220.0f, 20.0f);
    }
    // Three clicks, three increments, at once (no tick in between).
    CHECK(valueOf(rt, "clicks.value") == doctest::Approx(3.0));
    // The pointer moved to the button before the first click: that value was delivered before the
    // click (a pulse flushes the values reported before it), once.
    REQUIRE(sink.writes.size() == 1);
    CHECK(sink.writes[0] == "220");

    const PortId ping = doc->lookupPort("Hud.ping");
    REQUIRE(ping != kInvalidPort);
    for (int i = 0; i < 3; ++i) {
        CHECK(doc->setPort(ping, Value::ofFloat(1.0f)) == Status::kOk);
    }
    CHECK(valueOf(rt, "pings.value") == doctest::Approx(3.0));
}

TEST_CASE("T-389: a declared value port and an element's hover coalesce like the document ports") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadDoc(shaper);
    FlowRuntime rt;
    loadGraph(rt, *doc, R"({
        "version": 1,
        "nodes": [
            { "id": "f", "type": "format", "params": { "format": "{a}/{b}" } }
        ],
        "edges": [
            { "from": "#Hud.level", "to": "f.a" },
            { "from": "#Hot.hover", "to": "f.b" },
            { "from": "f.out", "to": "#Hud.text" }
        ]
    })");
    rt.tick(0.0f);
    TextSink sink;
    sink.attach(*doc);

    const PortId level = doc->lookupPort("Hud.level");
    REQUIRE(level != kInvalidPort);
    for (int i = 1; i <= 100; ++i) {
        CHECK(doc->setPort(level, Value::ofFloat(static_cast<float>(i))) == Status::kOk);
    }
    // In, out, in again over #Hot in the same frame.
    move(*doc, 50.0f, 50.0f);
    move(*doc, 300.0f, 300.0f);
    move(*doc, 50.0f, 50.0f);
    CHECK(sink.writes.empty());
    rt.tick(1.0f / 60.0f);
    REQUIRE(sink.writes.size() == 1);
    CHECK(sink.writes[0] == "100/true");
}

// -- T-407 (43 §43.3): behavior 0 keeps the pre-T-389 per-event delivery --------------------------

namespace {

/// pointerX -> toString -> #Hud.text: one downstream evaluation per delivered pointerX.
constexpr const char* kPointerTextGraph = R"({
    "version": 1,
    "nodes": [ { "id": "ts", "type": "toString" } ],
    "edges": [
        { "from": "#document.pointerX", "to": "ts.in" },
        { "from": "ts.out", "to": "#Hud.text" }
    ]
})";

}  // namespace

TEST_CASE("T-407: 100 pointer moves -- behavior 0 evaluates downstream 100 times, behavior 1 once") {
    for (const uint32_t behavior : {0u, 1u}) {
        CAPTURE(behavior);
        NullTextShaper shaper;
        std::unique_ptr<Document> doc = loadDoc(shaper, behavior);
        FlowRuntime rt;
        loadGraph(rt, *doc, kPointerTextGraph);
        rt.tick(0.0f);
        TextSink sink;
        sink.attach(*doc);
        for (int i = 0; i < 100; ++i) {
            move(*doc, 300.0f + static_cast<float>(i), 200.0f);
        }
        if (behavior == 0) {
            // Pre-T-389: each move reaches the sink within its own input dispatch.
            REQUIRE(sink.writes.size() == 100);
            CHECK(sink.writes.front() == "300");
            CHECK(textOf(rt, "ts.out") == "399");  // values() without a tick.
        } else {
            CHECK(sink.writes.empty());
        }
        rt.tick(1.0f / 60.0f);
        CHECK(sink.writes.size() == (behavior == 0 ? 100u : 1u));
        CHECK(sink.writes.back() == "399");
        rt.tick(1.0f / 60.0f);  // No movement, nothing more either way.
        CHECK(sink.writes.size() == (behavior == 0 ? 100u : 1u));
    }
}

// The pre-T-389 test_flow_runtime expectation ("#document pointer ports follow the pointer"): with
// behavior 0 a move and a press are visible in values() at once, no tick in between.
TEST_CASE("T-407: behavior 0 -- the pointer ports and the format node follow each event at once") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadDoc(shaper, 0);
    FlowRuntime rt;
    loadGraph(rt, *doc, R"({
        "version": 1,
        "nodes": [
            { "id": "r", "type": "constant", "params": { "value": 1000 } },
            { "id": "add", "type": "math", "params": { "op": "add" } },
            { "id": "down", "type": "toString" },
            { "id": "f", "type": "format", "params": { "format": "{a},{b},{c}" } }
        ],
        "edges": [
            { "from": "#document.pointerX", "to": "add.a" },
            { "from": "r.out", "to": "add.b" },
            { "from": "#document.pointerX", "to": "f.a" },
            { "from": "#document.pointerY", "to": "f.b" },
            { "from": "add.out", "to": "f.c" },
            { "from": "#document.pointerDown", "to": "down.in" },
            { "from": "f.out", "to": "#Hud.text" }
        ]
    })");
    rt.tick(0.0f);
    move(*doc, 310.0f, 210.0f);
    CHECK(textOf(rt, "f.out") == "310,210,1310");
    CHECK(valueOf(rt, "add.out") == doctest::Approx(1310.0));
    InputEvent ev;
    ev.kind = InputEvent::PointerDown;
    ev.pos = Vec2{310.0f, 210.0f};
    doc->dispatchInput(ev);
    CHECK(textOf(rt, "down.out") == "true");
    ev.kind = InputEvent::PointerUp;
    doc->dispatchInput(ev);
    CHECK(textOf(rt, "down.out") == "false");
}

TEST_CASE("T-407: behavior 0 -- a declared value port reaches the flow within its own set") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadDoc(shaper, 0);
    FlowRuntime rt;
    loadGraph(rt, *doc, R"({
        "version": 1,
        "nodes": [ { "id": "ts", "type": "toString" } ],
        "edges": [
            { "from": "#Hud.level", "to": "ts.in" },
            { "from": "ts.out", "to": "#Hud.text" }
        ]
    })");
    rt.tick(0.0f);
    TextSink sink;
    sink.attach(*doc);
    const PortId level = doc->lookupPort("Hud.level");
    REQUIRE(level != kInvalidPort);
    for (int i = 1; i <= 5; ++i) {
        REQUIRE(doc->setPort(level, Value::ofFloat(static_cast<float>(i))) == Status::kOk);
    }
    REQUIRE(sink.writes.size() == 5);
    CHECK(sink.writes.back() == "5");
}

TEST_CASE("T-543: 100 pointer moves in one frame reach a throttle once (T-389 coalescing comes first)") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadDoc(shaper);
    FlowRuntime rt;
    // `rate` is absent: the throttle does not limit, so `seen` counts what reached its `in`.
    loadGraph(rt, *doc, R"({
        "version": 1,
        "nodes": [
            { "id": "t", "type": "throttle", "params": { "unit": "frames" } },
            { "id": "seen", "type": "counter" },
            { "id": "d", "type": "filter", "params": { "op": "deadband", "threshold": 0 } },
            { "id": "dseen", "type": "counter" }
        ],
        "edges": [
            { "from": "#document.pointerX", "to": "t.in" },
            { "from": "t.passed", "to": "seen.increment" },
            { "from": "#document.pointerY", "to": "d.in" },
            { "from": "d.passed", "to": "dseen.increment" }
        ]
    })");
    rt.tick(0.0f);  // The initial (viewport-center) reading.
    const double before = valueOf(rt, "seen.value");
    const double dBefore = valueOf(rt, "dseen.value");
    for (int i = 0; i < 100; ++i) {
        move(*doc, 300.0f + static_cast<float>(i), 200.0f + static_cast<float>(i));
    }
    CHECK(valueOf(rt, "seen.value") == doctest::Approx(before));  // Nothing during the dispatch.
    rt.tick(1.0f / 60.0f);
    CHECK(valueOf(rt, "seen.value") == doctest::Approx(before + 1.0));
    CHECK(valueOf(rt, "dseen.value") == doctest::Approx(dBefore + 1.0));
}

// -- T-552 (27 §27.4.1, 2026-10-31 ruling): pointer and hover first, then bindings -------------------

namespace {

/// #Hot moves with the port `shift` (`bind:left`); its hover / leave triggers set `hot`, which #Probe's
/// width is bound to. The flow reads `#Hot.hover` into a toString node.
constexpr const char* kHoverBindDoc = R"X(<xt><head><style>
  #Hot { position: absolute; left: 0px; top: 0px; width: 100px; height: 100px; }
  #Probe { position: absolute; left: 0px; top: 300px; width: 10px; height: 10px; }
</style></head>
<body id="Root" port:shift="number in" port:hot="number in">
  <div id="Hot" bind:left="shift | range(0, 1, 0px, 200px)" on:hover="set(hot, 1)" on:leave="set(hot, 0)"></div>
  <div id="Probe" bind:width="hot | range(0, 1, 10px, 50px)"></div>
</body></xt>)X";

}  // namespace

TEST_CASE(
    "T-552: a binding sees the hover of its own frame; geometry a binding moves is hover-judged next frame") {
    NullTextShaper shaper;
    const compile::SourceFile file{"hover_bind.xt", kHoverBindDoc};
    compile::CompileOptions options;
    options.behavior = 1;
    compile::CompileResult result = compile::compileDocumentFromStrings({&file, 1}, "hover_bind.xt", options);
    for (const compile::Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    Context ctx{ContextConfig{&shaper, nullptr}};
    ctx.setViewport(Rect{0, 0, 400, 400}, 1.0f);
    Result<DocumentHandle> loaded = ctx.loadDocument(IrBlob{result.ir.data(), result.ir.size()}, true);
    REQUIRE(loaded.ok());
    Document* doc = ctx.document(loaded.value());
    REQUIRE(doc != nullptr);
    FlowRuntime rt;
    loadGraph(rt, *doc, R"({
        "version": 1,
        "nodes": [ { "id": "ts", "type": "toString" } ],
        "edges": [ { "from": "#Hot.hover", "to": "ts.in" } ]
    })");
    const PortId shift = doc->lookupPort("shift");
    const PortId hot = doc->lookupPort("hot");
    REQUIRE(shift != kInvalidPort);
    REQUIRE(hot != kInvalidPort);
    const float dt = 1.0f / 60.0f;
    auto frame = [&]() {
        ctx.update(dt);
        rt.tick(dt);
    };
    auto probeWidth = [&]() { return doc->byId("Probe").layoutBox().width; };
    frame();

    // The pointer comes to rest at (250, 50), right of #Hot.
    InputEvent move;
    move.kind = InputEvent::PointerMove;
    move.pos = Vec2{250.0f, 50.0f};
    ctx.pushInput(move);
    frame();
    CHECK(doc->getPort(hot).f == 0.0f);
    CHECK(probeWidth() == doctest::Approx(10.0f));

    // The host moves #Hot under the still pointer. This frame's binding moves it; the hover of this
    // frame was judged against the geometry before it, so nothing is hovered yet.
    REQUIRE(doc->setPort(shift, Value::ofFloat(1.0f)) == Status::kOk);
    frame();
    CHECK(doc->byId("Hot").boundingBox().x == doctest::Approx(200.0f));
    CHECK(doc->getPort(hot).f == 0.0f);
    CHECK(probeWidth() == doctest::Approx(10.0f));

    // Next frame, no input: hover is judged first (PointerEnter, `set(hot, 1)`), then the bindings --
    // #Probe's width follows in this same frame.
    ctx.update(dt);
    CHECK(doc->getPort(hot).f == 1.0f);
    CHECK(probeWidth() == doctest::Approx(50.0f));
    rt.tick(dt);
    CHECK(textOf(rt, "ts.out") == "true");  // The flow gets that hover at this frame's tick.

    // And away again: the binding moves #Hot back this frame, the leave and #Probe follow the next.
    REQUIRE(doc->setPort(shift, Value::ofFloat(0.0f)) == Status::kOk);
    frame();
    CHECK(doc->getPort(hot).f == 1.0f);
    ctx.update(dt);
    CHECK(doc->getPort(hot).f == 0.0f);
    CHECK(probeWidth() == doctest::Approx(10.0f));
    rt.tick(dt);
    CHECK(textOf(rt, "ts.out") == "false");

    // A pointer that left the window is not re-judged: #Hot comes back under (250, 50), nothing hovers.
    InputEvent leave;
    leave.kind = InputEvent::PointerLeave;
    ctx.pushInput(leave);
    frame();
    REQUIRE(doc->setPort(shift, Value::ofFloat(1.0f)) == Status::kOk);
    frame();
    frame();
    CHECK(doc->getPort(hot).f == 0.0f);
}
