#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/flow/flow_graph.h"
#include "xt/flow/flow_ports.h"
#include "xt/flow/flow_runtime.h"
#include "xt/flow/host_ports.h"
#include "xt/input.h"
#include "xt/text_shaper.h"

/// Declared ports (`port:`) in xt_flow: connection-point reflection off the PORT table, flow
/// edges to / from them, and the HostPorts upgrade (docs/design/27-interaction.md §27.3/27.4,
/// 13-api.md §13.8.1, docs/tasks/T-175.md).

using namespace xt;
using namespace xt::flow;

namespace {

constexpr const char* kDoc = R"X(<xt><head><style>#Btn { width: 40px; height: 40px; }</style></head>
<body id="Root"><div id="Hud" port:hp="number in" port:label="string" port:alive="bool out"
    port:score="number out" port:hit="event out" port:tint="color" port:ask="number call"
    port:stats="struct{a: number}" port:items="list<struct{n: number}>"
    bind:text="format('{0} {1}', hp, label)" on:port(ask)="emit(score)"></div>
  <div id="Btn" on:click="emit(hit); emit(score)"></div></body></xt>)X";

std::unique_ptr<Document> loadDoc(TextShaper& shaper) {
    const compile::SourceFile file{"ports.xt", kDoc};
    compile::CompileOptions options;
    compile::CompileResult result = compile::compileDocumentFromStrings({&file, 1}, "ports.xt", options);
    for (const compile::Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &shaper, nullptr, doc) ==
            Status::kOk);
    doc->setViewport(Rect{0, 0, 200, 200});
    doc->update(0.0f);
    return doc;
}

const PortDesc* find(const std::vector<PortDesc>& points, const std::string& owner, const std::string& port) {
    for (const PortDesc& p : points) {
        if (p.owner == owner && p.port == port) {
            return &p;
        }
    }
    return nullptr;
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

}  // namespace

TEST_CASE("connectionPoints: declared ports are reflected off the PORT table") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadDoc(shaper);
    std::vector<PortDesc> points = connectionPoints(*doc);

    const PortDesc* hp = find(points, "#Hud", "hp");
    REQUIRE(hp != nullptr);
    CHECK(hp->declared);
    CHECK(hp->type == PortType::Number);
    CHECK(hp->isInput);
    CHECK(hp->typeName == "number");
    CHECK(hp->connectable);
    const PortDesc* alive = find(points, "#Hud", "alive");
    REQUIRE(alive != nullptr);
    CHECK(alive->type == PortType::Bool);
    CHECK(!alive->isInput);
    CHECK(find(points, "#Hud", "hit")->type == PortType::Pulse);
    CHECK(find(points, "#Hud", "label")->type == PortType::String);
    CHECK(find(points, "#Hud", "tint")->type == PortType::String);  // Its text form.
    CHECK(find(points, "#Hud", "ask")->isInput);                    // call = an input.
    const PortDesc* stats = find(points, "#Hud", "stats");
    REQUIRE(stats != nullptr);
    CHECK(!stats->connectable);  // Struct / list: listed only.
    CHECK(stats->typeName == "struct");
    CHECK(!find(points, "#Hud", "items")->connectable);
    // The §17.6 rows are still there, undeclared.
    const PortDesc* click = find(points, "#Hud", "Click");
    REQUIRE(click != nullptr);
    CHECK(!click->declared);
}

TEST_CASE("FlowRuntime: edges into and out of declared ports") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadDoc(shaper);
    const std::string json = R"({
        "version": 1,
        "nodes": [
            { "id": "k", "type": "constant", "params": { "value": 42 } },
            { "id": "cnt", "type": "counter", "params": {} },
            { "id": "hits", "type": "counter", "params": {} }
        ],
        "edges": [
            { "from": "k.out", "to": "#Hud.hp" },
            { "from": "#Hud.score", "to": "cnt.set" },
            { "from": "#Hud.hit", "to": "hits.increment" }
        ]
    })";
    FlowGraph graph;
    std::vector<std::string> errors;
    REQUIRE(FlowGraph::parse(json, graph, errors) == Status::kOk);  // Untyped until a Document binds.
    FlowRuntime rt;
    REQUIRE(rt.load(graph, *doc, errors) == Status::kOk);
    CHECK(doc->portText(doc->lookupPort("hp")) == "42");
    doc->update(0.0f);  // T-389: the bindings of a port are evaluated by the next update().
    CHECK(std::string(doc->byId("Hud").text()) == "42 ");

    doc->setPort(doc->lookupPort("score"), Value::ofFloat(7));
    click(*doc, "Btn");
    const std::vector<std::string>& names = rt.valueNames();
    auto value = [&](const std::string& name) {
        for (size_t i = 0; i < names.size(); ++i) {
            if (names[i] == name) {
                return rt.values()[i].n;
            }
        }
        return -1.0;
    };
    CHECK(value("cnt.value") == 7.0);
    CHECK(value("hits.value") == 1.0);

    // Direction and type are checked once the Document is known.
    FlowGraph wrong;
    REQUIRE(FlowGraph::parse(R"({"version":1,"nodes":[{"id":"k","type":"constant","params":{"value":1}}],
        "edges":[{"from":"k.out","to":"#Hud.alive"}]})",
                             wrong, errors) == Status::kOk);
    FlowRuntime rt2;
    errors.clear();
    CHECK(rt2.load(wrong, *doc, errors) != Status::kOk);
    bool mentioned = false;
    for (const std::string& e : errors) {
        mentioned = mentioned || e.find("output (out)") != std::string::npos;
    }
    CHECK(mentioned);
}

TEST_CASE("HostPorts: lookup / set / get / call / emit listeners / converters / portTable / instanceId") {
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    const compile::SourceFile file{"ports.xt", kDoc};
    compile::CompileOptions options;
    compile::CompileResult result = compile::compileDocumentFromStrings({&file, 1}, "ports.xt", options);
    REQUIRE(result.ok());
    Result<DocumentHandle> h = ctx.loadDocument(IrBlob{result.ir.data(), result.ir.size()}, false);
    REQUIRE(h.ok());
    ctx.setViewport(Rect{0, 0, 200, 200}, 1.0f);
    ctx.update(0.0f);
    Document& doc = *ctx.document(h.value());

    HostPorts ports;
    CHECK(ports.lookup("hp") == kInvalidPort);  // Not bound.
    CHECK(ports.set(1, Value::ofFloat(1)) == Status::kInvalidArgument);
    REQUIRE(ports.bind(doc) == Status::kOk);

    PortId hp = ports.lookup("hp");
    REQUIRE(hp != kInvalidPort);
    CHECK(ports.lookup("#Hud.hp") == hp);
    CHECK(ports.set(hp, Value::ofFloat(72)) == Status::kOk);
    CHECK(ports.get(hp).f == 72.0f);
    CHECK(ports.setText(ports.lookup("label"), "hp") == Status::kOk);
    doc.update(0.0f);  // T-389.
    CHECK(std::string(doc.byId("Hud").text()) == "72 hp");

    std::vector<PortRecord> table = ports.portTable();
    REQUIRE(table.size() == 9);
    CHECK(table[0].name == "hp");
    CHECK(table[7].type == PortValueType::Struct);
    CHECK(table[7].fields.size() == 1);

    CHECK(ports.setInstanceId(3) == Status::kOk);
    std::vector<std::pair<PortId, float>> emitted;
    std::vector<uint32_t> instances;
    uint32_t id = ports.addEmitListener([&](PortId port, const Value& v, uint32_t instance) {
        emitted.emplace_back(port, v.f);
        instances.push_back(instance);
    });
    CHECK(id != 0);
    ports.set(ports.lookup("score"), Value::ofFloat(5));
    click(doc, "Btn");
    REQUIRE(emitted.size() == 2);
    CHECK(emitted[0].first == ports.lookup("hit"));
    CHECK(emitted[1].first == ports.lookup("score"));
    CHECK(emitted[1].second == 5.0f);
    CHECK(instances[1] == 3u);

    // Host -> UI call: port(ask) emits score; that value comes back.
    Value arg = Value::ofFloat(1);
    Result<Value> r = ports.call(ports.lookup("ask"), std::span<const Value>(&arg, 1));
    REQUIRE(r.ok());
    CHECK(r.value().f == 5.0f);

    // Lists and converters forward to the Document.
    CHECK(ports.listSet(ports.lookup("items"), {}) == Status::kOk);
    CHECK(ports.listRemove(ports.lookup("items"), 0) == Status::kInvalidArgument);
    Result<uint32_t> op = ports.registerConverter(
        "neg", [](const Value& v, std::span<const Value>) { return Value::ofFloat(-v.f); });
    REQUIRE(op.ok());
    CHECK(op.value() == 0x80u);

    ports.removeEmitListener(id);
    click(doc, "Btn");
    CHECK(emitted.size() == 3);  // Only the call's emit above, nothing new after removal.
}

TEST_CASE("T-391: HostPorts looks up a nested list by its path and edits it through the list API") {
    constexpr const char* kNested = R"X(<xt><body id="Root">
<div id="Hud" port:groups="list<struct{name: string, rows: list<string>}>"></div>
<repeat port="groups" id="Rows"><div id="Row">
  <repeat port="item(rows)" id="Sub"><span id="Cell" bind:text="item()"></span></repeat>
</div></repeat></body></xt>)X";
    const compile::SourceFile file{"nested.xt", kNested};
    compile::CompileOptions options;
    compile::CompileResult result = compile::compileDocumentFromStrings({&file, 1}, "nested.xt", options);
    for (const compile::Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    NullTextShaper shaper;
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &shaper, nullptr, doc) ==
            Status::kOk);
    doc->setViewport(Rect{0, 0, 200, 200});
    doc->update(0.0f);
    HostPorts ports;
    REQUIRE(ports.bind(*doc) == Status::kOk);
    REQUIRE(ports.setText(ports.lookup("groups"),
                          R"([{"name":"A","rows":["x"]},{"name":"B","rows":["y","z"]}])") == Status::kOk);
    doc->update(0.016f);
    const PortId rows = ports.lookup("Rows/1.rows");
    REQUIRE(rows != kInvalidPort);
    const Value w = Value::ofStringId(doc->intern("w"));
    CHECK(ports.listInsert(rows, 2, w) == Status::kOk);
    doc->update(0.032f);
    CHECK(std::string(doc->byId("Rows/1/Sub/2/Cell").text()) == "w");
    CHECK(ports.get(ports.lookup("groups")).items[1].items[1].itemCount == 3);
    CHECK(ports.portTable().size() == 1);  // The nested list is no declared port.
}

TEST_CASE(
    "HostPorts: addInputListener reports document writes of an in-port, not the host's own set (T-394)") {
    NullTextShaper shaper;
    const char* xt = R"X(<xt><head><style>#Knob { width: 40px; height: 40px; }</style></head>
<body id="Root"><div id="Knob" port:value="number in" port:on="bool in" port:out="number out"
    on:click="set(value, 0.5); set(on, true)"></div></body></xt>)X";
    const compile::SourceFile file{"in.xt", xt};
    compile::CompileOptions options;
    compile::CompileResult result = compile::compileDocumentFromStrings({&file, 1}, "in.xt", options);
    REQUIRE(result.ok());
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &shaper, nullptr, doc) ==
            Status::kOk);
    doc->setViewport(Rect{0, 0, 200, 200});
    doc->update(0.0f);

    HostPorts ports;
    CHECK(ports.addInputListener(1, [](const PortValue&) {}) == 0u);  // Not bound.
    REQUIRE(ports.bind(*doc) == Status::kOk);
    const PortId value = ports.lookup("value");
    const PortId on = ports.lookup("on");
    REQUIRE(value != kInvalidPort);
    CHECK(ports.addInputListener(ports.lookup("out"), [](const PortValue&) {}) == 0u);  // Not an in-port.
    std::vector<PortValue> seen;
    const uint32_t id = ports.addInputListener(value, [&](const PortValue& v) { seen.push_back(v); });
    CHECK(id != 0u);
    bool onSeen = false;
    ports.addInputListener(on, [&](const PortValue& v) { onSeen = v.type == PortType::Bool && v.b; });

    CHECK(ports.set(value, Value::ofFloat(0.25f)) == Status::kOk);  // The host: no callback.
    CHECK(ports.setText(value, "0.3") == Status::kOk);
    CHECK(seen.empty());
    click(*doc, "Knob");  // The document's own set(value, 0.5).
    CHECK(ports.get(value).f == doctest::Approx(0.5f));
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].type == PortType::Number);
    CHECK(seen[0].n == doctest::Approx(0.5));
    CHECK(onSeen);
    click(*doc, "Knob");  // Same value: no change, no callback.
    CHECK(seen.size() == 1);
    ports.removeInputListener(id);
    CHECK(ports.set(value, Value::ofFloat(0.0f)) == Status::kOk);
    click(*doc, "Knob");
    CHECK(seen.size() == 1);
}
