#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/charconv.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/flow/flow_graph.h"
#include "xt/flow/flow_runtime.h"
#include "xt/input.h"
#include "xt/perf.h"
#include "xt/text_shaper.h"

/// T-477 (Docs/design/27-interaction.md §27.4.2 item 4): the typed document sinks of the flow --
/// `set(port)` through Document::setPort() with a Value, `format -> setStyle` through a compiled
/// StyleTemplate -- give the same Document as the text path (setPortText() / setStyle() of the
/// text the flow produced), and their bind-time caches follow rebind().

using namespace xt;
using namespace xt::flow;

namespace {

constexpr const char* kStyle =
    ".box { position: absolute; left: 10px; top: 10px; width: 40px; height: 30px; background-color: #336699; "
    "}"
    "#P { width: 100px; height: 20px; }";

constexpr const char* kPortDiv =
    "<div id=\"P\" port:n=\"number\" port:b=\"bool\" port:who=\"entity\" port:hit=\"event\" "
    "port:c=\"number call\" port:s=\"string\" bind:text=\"n\" on:port(hit)=\"state(Hit)\" "
    "on:port(c)=\"state(Called)\"></div>";

constexpr const char* kBoxes =
    "<div id=\"E1\" class=\"box\"></div><div id=\"E2\" class=\"box\"></div><div id=\"E3\" "
    "class=\"box\"></div>"
    "<div id=\"E4\" class=\"box\"></div><div id=\"E5\" class=\"box\"></div><div id=\"E6\" "
    "class=\"box\"></div>";

std::unique_ptr<Document> loadDoc(TextShaper& shaper, const std::string& body) {
    const std::string text = std::string("<xt><head><style>") + kStyle + "</style></head><body id=\"Root\">" +
                             body + "</body></xt>";
    const compile::SourceFile file{"typed.xt", text};
    compile::CompileResult result =
        compile::compileDocumentFromStrings({&file, 1}, "typed.xt", compile::CompileOptions{});
    for (const compile::Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &shaper, nullptr, doc) ==
            Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 300});
    doc->update(0.0f);
    return doc;
}

FlowGraph parseGraph(const std::string& json) {
    FlowGraph graph;
    std::vector<std::string> errors;
    const Status status = FlowGraph::parse(json, graph, errors);
    for (const std::string& e : errors) {
        CAPTURE(e);
    }
    REQUIRE(status == Status::kOk);
    return graph;
}

void move(Document& doc, float x, float y) {
    InputEvent ev;
    ev.kind = InputEvent::PointerMove;
    ev.pos = Vec2{x, y};
    doc.dispatchInput(ev);
}

std::string digits17(double v) {
    char buf[64];
    return std::string(buf, formatFloat(buf, sizeof(buf), v, 17));
}

std::string digits6(double v) {
    char buf[64];
    return std::string(buf, formatFloat(buf, sizeof(buf), v, 6));
}

std::string paintDump(Document& doc) {
    CommandList list;
    doc.paint(list);
    std::string out;
    dumpCommandList(list, out);
    return out;
}

bool sameBits(const Value& a, const Value& b) {
    if (a.type != b.type) {
        return false;
    }
    if (a.type == ValueType::Enum) {
        return a.e == b.e;
    }
    return std::memcmp(&a.f, &b.f, sizeof(float)) == 0;
}

/// The flow's current value of `<node>.out` (a `format` / `toString` text).
std::string outputText(const FlowRuntime& flow, const std::string& node) {
    const std::vector<std::string>& names = flow.valueNames();
    const std::span<const PortValue> values = flow.values();
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i] == node + ".out") {
            return values[i].s;
        }
    }
    FAIL("no output " << node);
    return {};
}

constexpr float kMoves[][2] = {{13.37f, 40.0f}, {150.5f, 80.25f},   {250.25f, 10.0f}, {199.9f, 120.5f},
                               {201.0f, 3.0f},  {380.125f, 290.0f}, {5.0f, 150.0f},   {290.333f, 77.7f},
                               {99.5f, 0.5f},   {100.0f, 299.0f}};

// set(port): math(pointerX * 0.1) into number / entity / call ports, compare(> 20) into a bool
// port and (its `true` pulse) an event port, toString into a string port.
constexpr const char* kPortGraph = R"J({
    "version": 1,
    "nodes": [
        { "id": "k", "type": "constant", "params": { "value": 0.1 } },
        { "id": "m", "type": "math", "params": { "op": "mul" } },
        { "id": "k20", "type": "constant", "params": { "value": 20 } },
        { "id": "cmp", "type": "compare", "params": { "op": "gt" } },
        { "id": "ts", "type": "toString", "params": { "format": "{}" } }
    ],
    "edges": [
        { "from": "#document.pointerX", "to": "m.a" },
        { "from": "k.out", "to": "m.b" },
        { "from": "m.out", "to": "#P.n" },
        { "from": "m.out", "to": "#P.who" },
        { "from": "m.out", "to": "#P.c" },
        { "from": "m.out", "to": "cmp.a" },
        { "from": "k20.out", "to": "cmp.b" },
        { "from": "cmp.out", "to": "#P.b" },
        { "from": "cmp.true", "to": "#P.hit" },
        { "from": "m.out", "to": "ts.in" },
        { "from": "ts.out", "to": "#P.s" }
    ]
})J";

struct PortLog {
    std::vector<std::string> events;
    void attach(Document& doc) {
        doc.addPortListener([this, &doc](const PortEvent& ev) {
            events.push_back(std::to_string(ev.port) + ":" + std::to_string(static_cast<int>(ev.kind)) + ":" +
                             doc.portText(ev.port));
        });
    }
};

}  // namespace

TEST_CASE("T-477: set(port) typed gives the same Document as the 17-digit text path") {
    NullTextShaper shaper;
    std::unique_ptr<Document> typed = loadDoc(shaper, kPortDiv);
    std::unique_ptr<Document> text = loadDoc(shaper, kPortDiv);
    FlowRuntime flow;
    std::vector<std::string> errors;
    REQUIRE(flow.load(parseGraph(kPortGraph), *typed, errors) == Status::kOk);
    PerfTracker tracker;
    tracker.setEnabled(true);
    flow.setPerfTracker(&tracker);
    tracker.beginFrame(0.0f);
    flow.tick(0.016f);
    typed->update(0.016f);
    // Start the text Document where the flow left the typed one (load-time seeding).
    const char* ports[] = {"P.n", "P.b", "P.who", "P.c", "P.s"};
    for (const char* name : ports) {
        REQUIRE(text->setPortText(text->lookupPort(name), typed->portText(typed->lookupPort(name))) ==
                Status::kOk);
    }
    text->update(0.016f);
    PortLog typedLog;
    PortLog textLog;
    typedLog.attach(*typed);
    textLog.attach(*text);
    bool above = typed->getPort(typed->lookupPort("P.b")).f != 0.0f;
    float clock = 0.016f;
    uint32_t typedWrites = 0;
    uint32_t textWrites = 0;
    for (const auto& pos : kMoves) {
        tracker.beginFrame(clock);
        move(*typed, pos[0], pos[1]);  // Behavior 0: the flow writes inside the dispatch.
        flow.tick(0.016f);
        typedWrites += tracker.current().flowSinkTyped;
        textWrites += tracker.current().flowSinkText;
        // What the text path sent, in the flow's edge order.
        const double m = static_cast<double>(pos[0]) * 0.1;
        const std::string n = digits17(m);
        Document& d = *text;
        CHECK(d.setPortText(d.lookupPort("P.n"), n) == Status::kOk);
        CHECK(d.setPortText(d.lookupPort("P.who"), n) == Status::kOk);
        CHECK(d.setPortText(d.lookupPort("P.c"), n) == Status::kOk);
        CHECK(d.callPort(d.lookupPort("P.c"), {}).ok());
        const bool now = m > 20.0;
        if (now != above) {
            CHECK(d.setPortText(d.lookupPort("P.b"), now ? "true" : "false") == Status::kOk);
            if (now) {
                CHECK(d.setPortText(d.lookupPort("P.hit"), "") == Status::kOk);
            }
            above = now;
        }
        CHECK(d.setPortText(d.lookupPort("P.s"), digits6(m)) == Status::kOk);
        clock += 0.016f;
        typed->update(clock);
        text->update(clock);
        for (const char* name : ports) {
            CAPTURE(std::string(name));
            CHECK(sameBits(typed->getPort(typed->lookupPort(name)), text->getPort(text->lookupPort(name))));
        }
        CHECK(typed->byId("P").text() == text->byId("P").text());
        CHECK(typed->hasCustomState(typed->byId("P"), "Hit") == text->hasCustomState(text->byId("P"), "Hit"));
        CHECK(typed->hasCustomState(typed->byId("P"), "Called") ==
              text->hasCustomState(text->byId("P"), "Called"));
    }
    CHECK(typedLog.events == textLog.events);
    CHECK(paintDump(*typed) == paintDump(*text));
    CHECK(typedWrites > 0u);  // number / entity / call / bool / event ports.
    CHECK(textWrites > 0u);   // The string port.
    flow.setPerfTracker(nullptr);
}

namespace {

/// Drives `graph` over `doc` (flow) and a twin Document that receives each `format` node's text
/// through setStyle() (`sinks`: node id -> element id), comparing declarations and paint after
/// every move. Returns the typed / text sink write counts.
void compareStyleSinks(const char* graphJson, const std::vector<std::pair<std::string, std::string>>& sinks,
                       const std::vector<std::pair<float, float>>& moves, uint32_t& typedWrites,
                       uint32_t& textWrites) {
    NullTextShaper shaper;
    std::unique_ptr<Document> typed = loadDoc(shaper, kBoxes);
    std::unique_ptr<Document> text = loadDoc(shaper, kBoxes);
    FlowRuntime flow;
    std::vector<std::string> errors;
    REQUIRE(flow.load(parseGraph(graphJson), *typed, errors) == Status::kOk);
    PerfTracker tracker;
    tracker.setEnabled(true);
    flow.setPerfTracker(&tracker);
    typedWrites = 0;
    textWrites = 0;
    float clock = 0.0f;
    bool first = true;
    for (const auto& [x, y] : moves) {
        tracker.beginFrame(clock);
        if (!first) {
            move(*typed, x, y);
        }
        flow.tick(0.016f);
        typedWrites += tracker.current().flowSinkTyped;
        textWrites += tracker.current().flowSinkText;
        for (const auto& [node, element] : sinks) {
            text->byId(element).setStyle(outputText(flow, node));
        }
        clock += 0.016f;
        typed->update(clock);
        text->update(clock);
        for (const auto& [node, element] : sinks) {
            CAPTURE(outputText(flow, node));
            const ElementRef a = typed->byId(element);
            const ElementRef b = text->byId(element);
            const std::vector<RuntimeStyleDecl>& da = typed->arena().element(a.node).runtimeDecls;
            const std::vector<RuntimeStyleDecl>& db = text->arena().element(b.node).runtimeDecls;
            REQUIRE(da.size() == db.size());
            for (size_t i = 0; i < da.size(); ++i) {
                CHECK(da[i].prop == db[i].prop);
                CHECK(sameBits(da[i].value, db[i].value));
            }
        }
        CHECK(paintDump(*typed) == paintDump(*text));
        first = false;
    }
    flow.setPerfTracker(nullptr);
}

// Three shapes that compile: translate(px, px), a percentage width, opacity.
constexpr const char* kHitGraph = R"J({
    "version": 1,
    "nodes": [
        { "id": "kx", "type": "constant", "params": { "value": 0.1 } },
        { "id": "ky", "type": "constant", "params": { "value": 0.5 } },
        { "id": "ko", "type": "constant", "params": { "value": 0.002 } },
        { "id": "mx", "type": "math", "params": { "op": "mul" } },
        { "id": "my", "type": "math", "params": { "op": "mul" } },
        { "id": "mo", "type": "math", "params": { "op": "mul" } },
        { "id": "f1", "type": "format", "params": { "format": "transform: translate({a}px, {b}px)" } },
        { "id": "f2", "type": "format", "params": { "format": "width: {a}%" } },
        { "id": "f3", "type": "format", "params": { "format": "opacity: {a}" } }
    ],
    "edges": [
        { "from": "#document.pointerX", "to": "mx.a" }, { "from": "kx.out", "to": "mx.b" },
        { "from": "#document.pointerY", "to": "my.a" }, { "from": "ky.out", "to": "my.b" },
        { "from": "#document.pointerX", "to": "mo.a" }, { "from": "ko.out", "to": "mo.b" },
        { "from": "mx.out", "to": "f1.a" }, { "from": "my.out", "to": "f1.b" },
        { "from": "mx.out", "to": "f2.a" },
        { "from": "mo.out", "to": "f3.a" },
        { "from": "f1.out", "to": "#E1.setStyle" },
        { "from": "f2.out", "to": "#E2.setStyle" },
        { "from": "f3.out", "to": "#E3.setStyle" }
    ]
})J";

// Three that do not: rgb() channels (converted, no template), `rad` (not a runtime unit: an
// invalid text either way) and `line-clamp` (an integer, not a float field).
constexpr const char* kMissGraph = R"J({
    "version": 1,
    "nodes": [
        { "id": "f4", "type": "format", "params": { "format": "background-color: rgb({a}, 0, 0)" } },
        { "id": "f5", "type": "format", "params": { "format": "line-clamp: {a}" } },
        { "id": "f6", "type": "format", "params": { "format": "transform: rotate({a}rad)" } }
    ],
    "edges": [
        { "from": "#document.pointerX", "to": "f4.a" },
        { "from": "#document.pointerX", "to": "f5.a" },
        { "from": "#document.pointerX", "to": "f6.a" },
        { "from": "f4.out", "to": "#E4.setStyle" },
        { "from": "f5.out", "to": "#E5.setStyle" },
        { "from": "f6.out", "to": "#E6.setStyle" }
    ]
})J";

}  // namespace

TEST_CASE("T-477: format -> setStyle hits write typed and paint as the text path") {
    uint32_t typedWrites = 0;
    uint32_t textWrites = 0;
    std::vector<std::pair<float, float>> moves;
    for (const auto& pos : kMoves) {
        moves.emplace_back(pos[0], pos[1]);
    }
    compareStyleSinks(kHitGraph, {{"f1", "E1"}, {"f2", "E2"}, {"f3", "E3"}}, moves, typedWrites, textWrites);
    CHECK(typedWrites > 0u);
    CHECK(textWrites == 0u);
}

TEST_CASE("T-477: format -> setStyle misses go through setStyle and paint the same") {
    uint32_t typedWrites = 0;
    uint32_t textWrites = 0;
    const std::vector<std::pair<float, float>> moves = {{10.0f, 10.0f}, {20.5f, 30.0f}, {55.25f, 70.0f},
                                                        {0.5f, 5.0f},   {99.0f, 99.0f}, {42.0f, 1.0f}};
    compareStyleSinks(kMissGraph, {{"f4", "E4"}, {"f5", "E5"}, {"f6", "E6"}}, moves, typedWrites, textWrites);
    CHECK(typedWrites == 0u);
    CHECK(textWrites > 0u);
}

TEST_CASE("T-477: rebind() resolves the sink caches again (element moved, port retyped, element gone)") {
    NullTextShaper shaper;
    constexpr const char* graphJson = R"J({
        "version": 1,
        "nodes": [
            { "id": "k", "type": "constant", "params": { "value": 0.1 } },
            { "id": "m", "type": "math", "params": { "op": "mul" } },
            { "id": "f", "type": "format", "params": { "format": "width: {a}px" } }
        ],
        "edges": [
            { "from": "#document.pointerX", "to": "m.a" }, { "from": "k.out", "to": "m.b" },
            { "from": "m.out", "to": "#P.n" },
            { "from": "m.out", "to": "f.a" },
            { "from": "f.out", "to": "#E2.setStyle" }
        ]
    })J";
    // The Documents outlive the runtime (FlowRuntime::rebind()'s contract).
    std::unique_ptr<Document> first = loadDoc(shaper, std::string(kPortDiv) + kBoxes);
    // E2 at another place in the tree (another node), and `n` now a string port.
    std::unique_ptr<Document> second =
        loadDoc(shaper, "<div id=\"Extra\" class=\"box\"></div><div id=\"P\" port:n=\"string\"></div>" +
                            std::string(kBoxes));
    // E2 gone.
    std::unique_ptr<Document> third =
        loadDoc(shaper, std::string(kPortDiv) + "<div id=\"E1\" class=\"box\"></div>");
    PerfTracker tracker;
    tracker.setEnabled(true);
    FlowRuntime flow;
    std::vector<std::string> errors;
    REQUIRE(flow.load(parseGraph(graphJson), *first, errors) == Status::kOk);
    flow.setPerfTracker(&tracker);
    tracker.beginFrame(0.0f);
    move(*first, 120.0f, 10.0f);
    flow.tick(0.016f);
    first->update(0.016f);
    CHECK(first->byId("E2").layoutBox().width == doctest::Approx(12.0f));
    CHECK(tracker.current().flowSinkTyped == 2u);  // `n` (number) and E2's width (template).
    CHECK(tracker.current().flowSinkText == 0u);

    std::vector<std::string> warnings;
    REQUIRE(flow.rebind(*second, warnings) == Status::kOk);
    CHECK(warnings.empty());
    tracker.beginFrame(0.0f);
    move(*second, 250.0f, 10.0f);
    flow.tick(0.016f);
    second->update(0.032f);
    CHECK(second->byId("E2").layoutBox().width == doctest::Approx(25.0f));
    CHECK(second->portText(second->lookupPort("P.n")) == digits17(static_cast<double>(250.0f) * 0.1));
    CHECK(tracker.current().flowSinkText == 1u);   // `n`, as text now.
    CHECK(tracker.current().flowSinkTyped == 1u);  // E2's width.

    warnings.clear();
    REQUIRE(flow.rebind(*third, warnings) == Status::kOk);
    CHECK_FALSE(warnings.empty());  // "#E2.setStyle: element not found"; that edge is dropped.
    tracker.beginFrame(0.0f);
    move(*third, 140.0f, 10.0f);
    flow.tick(0.016f);
    third->update(0.048f);
    CHECK(third->getPort(third->lookupPort("P.n")).f == doctest::Approx(14.0f));
    CHECK(tracker.current().flowSinkTyped == 1u);  // `n` is a number port again.
    CHECK(tracker.current().flowSinkText == 0u);
    flow.setPerfTracker(nullptr);
}

// -- T-547: `setProperty` edges address effect parameters (27 §27.4.1, 18 §18.16) ----------------

namespace {

constexpr const char* kMaskDiv =
    "<div id=\"M\" style=\"effect: gradient-mask(radial, 0px 0px, 0px 0px, #000f 0%, #0000 100%)\"></div>";

std::string setPropertyGraph(const std::string& target, const std::string& prop) {
    return R"J({"version":1,"nodes":[],"edges":[{"from":"#document.pointerX","to":")J" + target +
           R"J(.setProperty","params":{"prop":")J" + prop + R"J("}}]})J";
}

float effectParams(Document& doc, const char* id, uint32_t slot) {
    const NodeCommon& c = doc.arena().common(doc.byId(id).node);
    return doc.arena().effects()[c.effectOffset].params[slot];
}

std::vector<std::string> loadErrors(Document& doc, const std::string& json) {
    FlowGraph graph;
    std::vector<std::string> errors;
    REQUIRE(FlowGraph::parse(json, graph, errors) == Status::kOk);
    FlowRuntime flow;
    errors.clear();
    CHECK(flow.load(graph, doc, errors) == Status::kInvalidArgument);
    return errors;
}

}  // namespace

TEST_CASE("T-547: setProperty prop effect.<name>.<param> writes the parameter and survives a restyle") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadDoc(shaper, std::string(kMaskDiv) + "<div id=\"E1\"></div>");
    FlowRuntime flow;
    std::vector<std::string> errors;
    REQUIRE(flow.load(parseGraph(setPropertyGraph("#M", "effect.gradient-mask.p0x")), *doc, errors) ==
            Status::kOk);
    move(*doc, 120.0f, 10.0f);
    flow.tick(0.016f);
    doc->update(0.016f);
    CHECK(effectParams(*doc, "M", 0) == doctest::Approx(120.0f));
    // A restyle keeps it (layer 4, T-545).
    REQUIRE(doc->byId("M").addClass("box"));
    doc->update(0.032f);
    CHECK(effectParams(*doc, "M", 0) == doctest::Approx(120.0f));
    move(*doc, 40.0f, 10.0f);
    flow.tick(0.016f);
    doc->update(0.048f);
    CHECK(effectParams(*doc, "M", 0) == doctest::Approx(40.0f));
}

TEST_CASE("T-547: effect.0.p0y is the same address by stack index") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadDoc(shaper, kMaskDiv);
    FlowRuntime flow;
    std::vector<std::string> errors;
    REQUIRE(flow.load(parseGraph(setPropertyGraph("#M", "effect.0.p0y")), *doc, errors) == Status::kOk);
    move(*doc, 75.0f, 10.0f);
    flow.tick(0.016f);
    doc->update(0.016f);
    CHECK(effectParams(*doc, "M", 1) == doctest::Approx(75.0f));
    CHECK(effectParams(*doc, "M", 0) == doctest::Approx(0.0f));
}

TEST_CASE("T-547: setProperty effect address errors, verbatim") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadDoc(shaper, std::string(kMaskDiv) + "<div id=\"E1\"></div>");
    auto only = [&](const char* target, const char* prop) {
        std::vector<std::string> errors = loadErrors(*doc, setPropertyGraph(target, prop));
        REQUIRE(errors.size() == 1u);
        return errors[0];
    };
    CHECK(only("#E1", "effect.gradient-mask.p0x") == "#E1.setProperty: the element has no effect");
    CHECK(only("#M", "effect.foo.p0x") == "#M.setProperty: unknown effect \"foo\"");
    CHECK(only("#M", "effect.blur.radiusX") == "#M.setProperty: the element has no such effect");
    CHECK(only("#M", "effect.3.p0x") == "#M.setProperty: effect index out of range");
    CHECK(only("#M", "effect.gradient-mask.nope") == "#M.setProperty: unknown effect parameter");

    // Without a Document only the shape is checked: exactly two non-empty segments.
    FlowGraph graph;
    std::vector<std::string> errors;
    CHECK(FlowGraph::parse(setPropertyGraph("#M", "effect.gradient-mask"), graph, errors) != Status::kOk);
    REQUIRE(errors.size() == 1u);
    CHECK(errors[0].find("malformed effect address 'effect.gradient-mask'") != std::string::npos);
    errors.clear();
    CHECK(FlowGraph::parse(setPropertyGraph("#M", "effect..p0x"), graph, errors) != Status::kOk);
    errors.clear();
    CHECK(FlowGraph::parse(setPropertyGraph("#M", "effect.gradient-mask.p0x"), graph, errors) == Status::kOk);
}

TEST_CASE("T-547: a color effect parameter is refused (setProperty carries a number)") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc =
        loadDoc(shaper, "<div id=\"S\" style=\"effect: shadow(0 4px 8px #00000080)\"></div>");
    std::vector<std::string> errors = loadErrors(*doc, setPropertyGraph("#S", "effect.shadow.color"));
    REQUIRE(errors.size() == 1u);
    CHECK(errors[0] == "#S.setProperty: the effect parameter is not a number");
}

TEST_CASE("T-547: rebind() resolves the effect address again and warns when the effect is gone") {
    NullTextShaper shaper;
    std::unique_ptr<Document> first = loadDoc(shaper, kMaskDiv);
    std::unique_ptr<Document> second = loadDoc(shaper, "<div id=\"M\"></div>");
    std::unique_ptr<Document> third = loadDoc(shaper, std::string("<div id=\"Pad\"></div>") + kMaskDiv);
    FlowRuntime flow;
    std::vector<std::string> errors;
    REQUIRE(flow.load(parseGraph(setPropertyGraph("#M", "effect.gradient-mask.p0x")), *first, errors) ==
            Status::kOk);
    std::vector<std::string> warnings;
    REQUIRE(flow.rebind(*second, warnings) == Status::kOk);
    REQUIRE(warnings.size() == 1u);
    CHECK(warnings[0] == "#M.setProperty: the element has no effect");
    move(*second, 90.0f, 10.0f);  // Writes nothing; must not crash.
    flow.tick(0.016f);
    second->update(0.016f);

    warnings.clear();
    REQUIRE(flow.rebind(*third, warnings) == Status::kOk);
    CHECK(warnings.empty());
    move(*third, 64.0f, 10.0f);
    flow.tick(0.016f);
    third->update(0.016f);
    CHECK(effectParams(*third, "M", 0) == doctest::Approx(64.0f));
}
