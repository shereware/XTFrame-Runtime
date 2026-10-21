#include <memory>
#include <span>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/flow/flow_graph.h"
#include "xt/flow/flow_runtime.h"
#include "xt/input.h"
#include "xt/text_shaper.h"

/// T-389 (docs/design/27-interaction.md §27.4.1, 2026-10-21 ruling): value-type document outputs
/// are coalesced per frame -- whatever the input dispatch reported is delivered once, from the
/// next FlowRuntime::tick(), as one batch evaluated in topological order -- while pulse / event
/// outputs stay synchronous, one delivery per event.

using namespace xt;
using namespace xt::flow;

namespace {

constexpr const char* kDoc = R"X(<xt><head><style>
  #Hot { position: absolute; left: 0px; top: 0px; width: 100px; height: 100px; }
  #Btn { position: absolute; left: 200px; top: 0px; width: 40px; height: 40px; }
</style></head>
<body id="Root"><div id="Hud" port:text="string in" port:level="number out" port:ping="event out"></div>
  <div id="Hot"></div><div id="Btn"></div></body></xt>)X";

std::unique_ptr<Document> loadDoc(TextShaper& shaper) {
    const compile::SourceFile file{"coalesce.xt", kDoc};
    compile::CompileOptions options;
    compile::CompileResult result = compile::compileDocumentFromStrings({&file, 1}, "coalesce.xt", options);
    for (const compile::Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &shaper, nullptr, doc) ==
            Status::kOk);
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
