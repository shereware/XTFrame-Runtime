#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "test_temp.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/flow/flow_graph.h"
#include "xt/flow/flow_runtime.h"
#include "xt/input.h"
#include "xt/text_shaper.h"

/// One behavior case per §17.6 node type (docs/tasks/T-56.md "test_flow_nodes"): counter's
/// increment/decrement/reset + min/max clamp, toggle's toggle/setTrue/setFalse + change-only
/// `changed` pulse, timer's multi-tick-per-dt behavior, clock's accumulated seconds, random's
/// trigger (degenerate min==max range for determinism), math's every `op` (plus `clamp`/`map`),
/// compare's every `op` plus its change-only true/false pulses, toString's decimals/shortest/
/// format substitution and select's cond-driven passthrough. Pulse-driven nodes are exercised
/// through a small fixture document's buttons (Document::dispatchInput), the same technique
/// docs/tasks/T-56.md's own test_flow_runtime.cpp uses -- FlowRuntime always binds against a real
/// Document, even for graphs (like most of these) that never reference a "#id" connection point.
///
/// See docs/design/17-editor.md §17.6.

using namespace xt;
using namespace xt::flow;

namespace {

namespace fs = std::filesystem;

std::string writeTempXt(const std::string& name, const std::string& source) {
    fs::path dir = xt::test::tempDir() / "xt_t56_flow_nodes";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path path = dir / (name + ".xt");
    std::FILE* file = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&file, path.string().c_str(), "wb") != 0) {
        file = nullptr;
    }
#else
    file = std::fopen(path.string().c_str(), "wb");
#endif
    REQUIRE(file != nullptr);
    std::fwrite(source.data(), 1, source.size(), file);
    std::fclose(file);
    return path.string();
}

/// A document with six buttons (#b1..#b6) stacked in a column, for driving node pulse inputs via
/// Document::dispatchInput.
std::unique_ptr<Document> buttonsDoc(TextShaper& shaper) {
    const std::string source =
        "<xt>\n<head><style>\nbody{display:flex;flex-direction:column;}\n"
        "button{width:50px;height:20px;}\n</style></head>\n<body>\n"
        "<button id=\"b1\">1</button><button id=\"b2\">2</button><button id=\"b3\">3</button>\n"
        "<button id=\"b4\">4</button><button id=\"b5\">5</button><button id=\"b6\">6</button>\n"
        "</body>\n</xt>\n";
    std::string path = writeTempXt("buttons", source);
    compile::CompileOptions options;
    compile::CompileResult result = compile::compileDocument(path, options);
    for (const compile::Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    std::unique_ptr<Document> doc;
    Status status = Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &shaper, nullptr, doc);
    REQUIRE(status == Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 300});
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

double valueOf(const FlowRuntime& rt, const std::string& name) {
    const std::vector<std::string>& names = rt.valueNames();
    std::span<const PortValue> values = rt.values();
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i] == name) {
            return values[i].n;
        }
    }
    FAIL("no such output: " << name);
    return 0;
}

bool boolValueOf(const FlowRuntime& rt, const std::string& name) {
    const std::vector<std::string>& names = rt.valueNames();
    std::span<const PortValue> values = rt.values();
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i] == name) {
            return values[i].b;
        }
    }
    FAIL("no such output: " << name);
    return false;
}

std::string stringValueOf(const FlowRuntime& rt, const std::string& name) {
    const std::vector<std::string>& names = rt.valueNames();
    std::span<const PortValue> values = rt.values();
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i] == name) {
            return values[i].s;
        }
    }
    FAIL("no such output: " << name);
    return "";
}

Status load(FlowRuntime& rt, Document& doc, const std::string& json) {
    FlowGraph graph;
    std::vector<std::string> parseErrors;
    Status status = FlowGraph::parse(json, graph, parseErrors);
    for (const std::string& e : parseErrors) {
        CAPTURE(e);
    }
    REQUIRE(status == Status::kOk);
    std::vector<std::string> loadErrors;
    Status loadStatus = rt.load(graph, doc, loadErrors);
    for (const std::string& e : loadErrors) {
        CAPTURE(e);
    }
    return loadStatus;
}

}  // namespace

TEST_CASE("counter: increment/decrement/reset, clamped to [min, max]") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = buttonsDoc(shaper);
    FlowRuntime rt;
    const std::string json = R"({
        "nodes": [ { "id": "cnt", "type": "counter",
                     "params": { "step": 2, "initial": 0, "min": 0, "max": 5 } } ],
        "edges": [
            { "from": "#b1.Click", "to": "cnt.increment" },
            { "from": "#b2.Click", "to": "cnt.decrement" },
            { "from": "#b3.Click", "to": "cnt.reset" }
        ]
    })";
    REQUIRE(load(rt, *doc, json) == Status::kOk);
    CHECK(valueOf(rt, "cnt.value") == doctest::Approx(0));

    click(*doc, "b1");
    CHECK(valueOf(rt, "cnt.value") == doctest::Approx(2));
    click(*doc, "b1");
    CHECK(valueOf(rt, "cnt.value") == doctest::Approx(4));
    click(*doc, "b1");  // 4 + 2 = 6, clamped to max (5).
    CHECK(valueOf(rt, "cnt.value") == doctest::Approx(5));
    click(*doc, "b2");  // 5 - 2 = 3.
    CHECK(valueOf(rt, "cnt.value") == doctest::Approx(3));
    click(*doc, "b3");  // reset -> initial (0).
    CHECK(valueOf(rt, "cnt.value") == doctest::Approx(0));
}

TEST_CASE("toggle: toggle/setTrue/setFalse; `changed` pulse only fires on an actual change") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = buttonsDoc(shaper);
    FlowRuntime rt;
    const std::string json = R"({
        "nodes": [
            { "id": "tog", "type": "toggle", "params": { "initial": false } },
            { "id": "cnt", "type": "counter", "params": {} }
        ],
        "edges": [
            { "from": "#b1.Click", "to": "tog.toggle" },
            { "from": "#b2.Click", "to": "tog.setTrue" },
            { "from": "#b3.Click", "to": "tog.setFalse" },
            { "from": "tog.changed", "to": "cnt.increment" }
        ]
    })";
    REQUIRE(load(rt, *doc, json) == Status::kOk);
    CHECK_FALSE(boolValueOf(rt, "tog.value"));
    CHECK(valueOf(rt, "cnt.value") == doctest::Approx(0));

    click(*doc, "b2");  // setTrue: false -> true, changes.
    CHECK(boolValueOf(rt, "tog.value"));
    CHECK(valueOf(rt, "cnt.value") == doctest::Approx(1));
    click(*doc, "b2");  // setTrue again: already true, no change.
    CHECK(valueOf(rt, "cnt.value") == doctest::Approx(1));
    click(*doc, "b1");  // toggle: true -> false, changes.
    CHECK_FALSE(boolValueOf(rt, "tog.value"));
    CHECK(valueOf(rt, "cnt.value") == doctest::Approx(2));
    click(*doc, "b3");  // setFalse: already false, no change.
    CHECK(valueOf(rt, "cnt.value") == doctest::Approx(2));
}

TEST_CASE("timer: ticks (and increments `count`) once per interval crossed within one dt") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = buttonsDoc(shaper);
    FlowRuntime rt;
    const std::string json = R"({
        "nodes": [
            { "id": "tmr", "type": "timer", "params": { "interval": 0.1, "autostart": true } },
            { "id": "cnt", "type": "counter", "params": {} }
        ],
        "edges": [ { "from": "tmr.tick", "to": "cnt.increment" } ]
    })";
    REQUIRE(load(rt, *doc, json) == Status::kOk);
    CHECK(valueOf(rt, "tmr.count") == doctest::Approx(0));

    rt.tick(0.25f);  // Crosses 0.1 twice (0.2 <= 0.25), 0.05s remainder.
    CHECK(valueOf(rt, "tmr.count") == doctest::Approx(2));
    CHECK(valueOf(rt, "cnt.value") == doctest::Approx(2));

    rt.tick(0.1f);  // 0.05 + 0.1 = 0.15 >= 0.1: one more tick.
    CHECK(valueOf(rt, "tmr.count") == doctest::Approx(3));
    CHECK(valueOf(rt, "cnt.value") == doctest::Approx(3));
}

TEST_CASE("clock: `seconds` accumulates tick() dt") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = buttonsDoc(shaper);
    FlowRuntime rt;
    const std::string json = R"({
        "nodes": [ { "id": "clk", "type": "clock", "params": {} } ],
        "edges": []
    })";
    REQUIRE(load(rt, *doc, json) == Status::kOk);
    CHECK(valueOf(rt, "clk.seconds") == doctest::Approx(0));
    rt.tick(1.0f);
    rt.tick(0.5f);
    CHECK(valueOf(rt, "clk.seconds") == doctest::Approx(1.5));
}

TEST_CASE("random: trigger produces a value in [min, max] (degenerate range for determinism)") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = buttonsDoc(shaper);
    FlowRuntime rt;
    const std::string json = R"({
        "nodes": [ { "id": "rnd", "type": "random",
                     "params": { "min": 7, "max": 7, "integer": true } } ],
        "edges": [ { "from": "#b1.Click", "to": "rnd.trigger" } ]
    })";
    REQUIRE(load(rt, *doc, json) == Status::kOk);
    click(*doc, "b1");
    CHECK(valueOf(rt, "rnd.value") == doctest::Approx(7));
}

TEST_CASE("math: every op") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = buttonsDoc(shaper);

    struct Case {
        const char* op;
        double expected;
    };
    // a = -7.4, b = 3 (add/sub/mul/div/min/max); abs/round only use `a`.
    const Case cases[] = {
        {"add", -4.4}, {"sub", -10.4}, {"mul", -22.2}, {"div", -7.4 / 3.0},
        {"min", -7.4}, {"max", 3.0},   {"abs", 7.4},   {"round", -7.0},
    };
    for (const Case& c : cases) {
        CAPTURE(c.op);
        FlowRuntime rt;
        std::string json = std::string(R"({
            "nodes": [
                { "id": "ca", "type": "constant", "params": { "value": -7.4 } },
                { "id": "cb", "type": "constant", "params": { "value": 3 } },
                { "id": "m", "type": "math", "params": { "op": ")") +
                           c.op + R"(" } }
            ],
            "edges": [
                { "from": "ca.out", "to": "m.a" },
                { "from": "cb.out", "to": "m.b" }
            ]
        })";
        REQUIRE(load(rt, *doc, json) == Status::kOk);
        CHECK(valueOf(rt, "m.out") == doctest::Approx(c.expected));
    }
}

TEST_CASE("math: clamp and map") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = buttonsDoc(shaper);

    {
        FlowRuntime rt;
        const std::string json = R"({
            "nodes": [
                { "id": "ca", "type": "constant", "params": { "value": 7 } },
                { "id": "m", "type": "math", "params": { "op": "clamp", "lo": 0, "hi": 5 } }
            ],
            "edges": [ { "from": "ca.out", "to": "m.a" } ]
        })";
        REQUIRE(load(rt, *doc, json) == Status::kOk);
        CHECK(valueOf(rt, "m.out") == doctest::Approx(5));
    }
    {
        FlowRuntime rt;
        const std::string json = R"({
            "nodes": [
                { "id": "ca", "type": "constant", "params": { "value": 5 } },
                { "id": "m", "type": "math",
                  "params": { "op": "map", "inLo": 0, "inHi": 10, "outLo": 0, "outHi": 100 } }
            ],
            "edges": [ { "from": "ca.out", "to": "m.a" } ]
        })";
        REQUIRE(load(rt, *doc, json) == Status::kOk);
        CHECK(valueOf(rt, "m.out") == doctest::Approx(50));
    }
}

TEST_CASE("compare: every op") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = buttonsDoc(shaper);

    struct Case {
        const char* op;
        bool expected;
    };
    // a = 3, b = 5.
    const Case cases[] = {
        {"eq", false}, {"ne", true}, {"lt", true}, {"le", true}, {"gt", false}, {"ge", false},
    };
    for (const Case& c : cases) {
        CAPTURE(c.op);
        FlowRuntime rt;
        std::string json = std::string(R"({
            "nodes": [
                { "id": "ca", "type": "constant", "params": { "value": 3 } },
                { "id": "cb", "type": "constant", "params": { "value": 5 } },
                { "id": "cmp", "type": "compare", "params": { "op": ")") +
                           c.op + R"(" } }
            ],
            "edges": [
                { "from": "ca.out", "to": "cmp.a" },
                { "from": "cb.out", "to": "cmp.b" }
            ]
        })";
        REQUIRE(load(rt, *doc, json) == Status::kOk);
        CHECK(boolValueOf(rt, "cmp.out") == c.expected);
    }
}

TEST_CASE("compare: true/false pulses only fire when the result actually changes") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = buttonsDoc(shaper);
    FlowRuntime rt;
    const std::string json = R"({
        "nodes": [
            { "id": "cnt", "type": "counter", "params": { "step": 1, "initial": 0 } },
            { "id": "threshold", "type": "constant", "params": { "value": 2 } },
            { "id": "cmp", "type": "compare", "params": { "op": "ge" } },
            { "id": "hits", "type": "counter", "params": {} },
            { "id": "misses", "type": "counter", "params": {} }
        ],
        "edges": [
            { "from": "#b1.Click", "to": "cnt.increment" },
            { "from": "#b2.Click", "to": "cnt.decrement" },
            { "from": "cnt.value", "to": "cmp.a" },
            { "from": "threshold.out", "to": "cmp.b" },
            { "from": "cmp.true", "to": "hits.increment" },
            { "from": "cmp.false", "to": "misses.increment" }
        ]
    })";
    REQUIRE(load(rt, *doc, json) == Status::kOk);
    // Initial state (cnt.value == 0 < 2, so "out" settles false) fires neither pulse: the settled
    // state right after load() is the baseline, not an observed change.
    CHECK(valueOf(rt, "misses.value") == doctest::Approx(0));
    CHECK(valueOf(rt, "hits.value") == doctest::Approx(0));

    click(*doc, "b1");  // cnt.value == 1, still false: no change, no fire.
    CHECK(valueOf(rt, "misses.value") == doctest::Approx(0));
    click(*doc, "b1");  // cnt.value == 2, now true: changes, fires "true".
    CHECK(valueOf(rt, "hits.value") == doctest::Approx(1));
    click(*doc, "b1");  // cnt.value == 3, still true: no change, no fire.
    CHECK(valueOf(rt, "hits.value") == doctest::Approx(1));

    click(*doc, "b2");  // cnt.value == 2, still true: no change, no fire.
    CHECK(valueOf(rt, "misses.value") == doctest::Approx(0));
    click(*doc, "b2");  // cnt.value == 1, now false: changes, fires "false".
    CHECK(valueOf(rt, "misses.value") == doctest::Approx(1));
    click(*doc, "b2");  // cnt.value == 0, still false: no change, no fire.
    CHECK(valueOf(rt, "misses.value") == doctest::Approx(1));
    CHECK(valueOf(rt, "hits.value") == doctest::Approx(1));
}

TEST_CASE("toString: decimals, shortest representation, format substitution, passthrough") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = buttonsDoc(shaper);
    FlowRuntime rt;
    const std::string json = R"({
        "nodes": [
            { "id": "num", "type": "constant", "params": { "value": 3.14159 } },
            { "id": "flag", "type": "constant", "params": { "value": true } },
            { "id": "text", "type": "constant", "params": { "value": "hi" } },
            { "id": "ts1", "type": "toString", "params": { "decimals": 2 } },
            { "id": "ts2", "type": "toString", "params": {} },
            { "id": "ts3", "type": "toString", "params": { "format": "v={}" } },
            { "id": "ts4", "type": "toString", "params": {} }
        ],
        "edges": [
            { "from": "num.out", "to": "ts1.in" },
            { "from": "num.out", "to": "ts2.in" },
            { "from": "flag.out", "to": "ts3.in" },
            { "from": "text.out", "to": "ts4.in" }
        ]
    })";
    REQUIRE(load(rt, *doc, json) == Status::kOk);
    CHECK(stringValueOf(rt, "ts1.out") == "3.14");
    CHECK(stringValueOf(rt, "ts2.out") == "3.14159");
    CHECK(stringValueOf(rt, "ts3.out") == "v=true");
    CHECK(stringValueOf(rt, "ts4.out") == "hi");
}

TEST_CASE("select: cond picks a or b") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = buttonsDoc(shaper);
    {
        FlowRuntime rt;
        const std::string json = R"({
            "nodes": [
                { "id": "cond", "type": "constant", "params": { "value": true } },
                { "id": "a", "type": "constant", "params": { "value": 10 } },
                { "id": "b", "type": "constant", "params": { "value": 20 } },
                { "id": "sel", "type": "select", "params": {} }
            ],
            "edges": [
                { "from": "cond.out", "to": "sel.cond" },
                { "from": "a.out", "to": "sel.a" },
                { "from": "b.out", "to": "sel.b" }
            ]
        })";
        REQUIRE(load(rt, *doc, json) == Status::kOk);
        CHECK(valueOf(rt, "sel.out") == doctest::Approx(10));
    }
    {
        FlowRuntime rt;
        const std::string json = R"({
            "nodes": [
                { "id": "cond", "type": "constant", "params": { "value": false } },
                { "id": "a", "type": "constant", "params": { "value": 10 } },
                { "id": "b", "type": "constant", "params": { "value": 20 } },
                { "id": "sel", "type": "select", "params": {} }
            ],
            "edges": [
                { "from": "cond.out", "to": "sel.cond" },
                { "from": "a.out", "to": "sel.a" },
                { "from": "b.out", "to": "sel.b" }
            ]
        })";
        REQUIRE(load(rt, *doc, json) == Status::kOk);
        CHECK(valueOf(rt, "sel.out") == doctest::Approx(20));
    }
}

// ---------------------------------------------------------------------------------------------
// T-123's node set: radio, lookup, merge, format, counter's `set` input and timer's `repeat`.
// ---------------------------------------------------------------------------------------------

TEST_CASE("radio: set<i>, set, clamping and the changed pulse") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = buttonsDoc(shaper);
    FlowRuntime rt;
    const std::string json = R"({
        "nodes": [ { "id": "r", "type": "radio", "params": { "count": 4, "initial": 1 } },
                   { "id": "hits", "type": "counter", "params": {} } ],
        "edges": [
            { "from": "#b1.Click", "to": "r.set0" },
            { "from": "#b2.Click", "to": "r.set3" },
            { "from": "r.changed", "to": "hits.increment" }
        ]
    })";
    REQUIRE(load(rt, *doc, json) == Status::kOk);
    // `initial` is the starting index, and seeding it does not count as a change.
    CHECK(valueOf(rt, "r.index") == doctest::Approx(1));
    CHECK(valueOf(rt, "hits.value") == doctest::Approx(0));

    click(*doc, "b2");
    CHECK(valueOf(rt, "r.index") == doctest::Approx(3));
    CHECK(valueOf(rt, "hits.value") == doctest::Approx(1));
    click(*doc, "b2");  // Same index again: no `changed`.
    CHECK(valueOf(rt, "hits.value") == doctest::Approx(1));
    click(*doc, "b1");
    CHECK(valueOf(rt, "r.index") == doctest::Approx(0));
    CHECK(valueOf(rt, "hits.value") == doctest::Approx(2));
}

TEST_CASE("radio: the number `set` input is rounded and clamped into 0..count-1") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = buttonsDoc(shaper);
    FlowRuntime rt;
    const std::string json = R"({
        "nodes": [ { "id": "r", "type": "radio", "params": { "count": 3 } },
                   { "id": "n", "type": "counter", "params": { "step": 4, "initial": 0 } } ],
        "edges": [ { "from": "#b1.Click", "to": "n.increment" },
                   { "from": "#b2.Click", "to": "n.decrement" },
                   { "from": "n.value", "to": "r.set" } ]
    })";
    REQUIRE(load(rt, *doc, json) == Status::kOk);
    CHECK(valueOf(rt, "r.index") == doctest::Approx(0));
    click(*doc, "b1");  // 4 -> clamped to 2.
    CHECK(valueOf(rt, "r.index") == doctest::Approx(2));
    click(*doc, "b2");  // back to 0.
    click(*doc, "b2");  // -4 -> clamped to 0.
    CHECK(valueOf(rt, "r.index") == doctest::Approx(0));
}

TEST_CASE("lookup: index picks a value, out of range clamps") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = buttonsDoc(shaper);
    FlowRuntime rt;
    const std::string json = R"({
        "nodes": [ { "id": "r", "type": "radio", "params": { "count": 3 } },
                   { "id": "l", "type": "lookup", "params": { "values": ["low", "mid", "high"] } },
                   { "id": "n", "type": "lookup", "params": { "values": [10, 20, 30] } } ],
        "edges": [ { "from": "#b1.Click", "to": "r.set2" }, { "from": "#b2.Click", "to": "r.set1" },
                   { "from": "r.index", "to": "l.index" }, { "from": "r.index", "to": "n.index" } ]
    })";
    REQUIRE(load(rt, *doc, json) == Status::kOk);
    CHECK(stringValueOf(rt, "l.out") == "low");
    CHECK(valueOf(rt, "n.out") == doctest::Approx(10));
    click(*doc, "b1");
    CHECK(stringValueOf(rt, "l.out") == "high");
    CHECK(valueOf(rt, "n.out") == doctest::Approx(30));
    click(*doc, "b2");
    CHECK(stringValueOf(rt, "l.out") == "mid");
}

TEST_CASE("merge: any incoming pulse fires one outgoing pulse") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = buttonsDoc(shaper);
    FlowRuntime rt;
    const std::string json = R"({
        "nodes": [ { "id": "m", "type": "merge" },
                   { "id": "c", "type": "counter", "params": {} } ],
        "edges": [ { "from": "#b1.Click", "to": "m.in" }, { "from": "#b2.Click", "to": "m.in" },
                   { "from": "#b3.Click", "to": "m.in" }, { "from": "m.out", "to": "c.increment" } ]
    })";
    REQUIRE(load(rt, *doc, json) == Status::kOk);
    CHECK(valueOf(rt, "c.value") == doctest::Approx(0));
    click(*doc, "b1");
    click(*doc, "b3");
    click(*doc, "b2");
    CHECK(valueOf(rt, "c.value") == doctest::Approx(3));
}

TEST_CASE("format: named placeholders, decimals and unconnected inputs") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = buttonsDoc(shaper);
    FlowRuntime rt;
    const std::string json = R"({
        "nodes": [ { "id": "c", "type": "counter", "params": { "step": 1, "initial": 2 } },
                   { "id": "k", "type": "constant", "params": { "value": "HP" } },
                   { "id": "f", "type": "format",
                     "params": { "format": "{a}: {b}/{c}{d}", "decimals": 2 } } ],
        "edges": [ { "from": "k.out", "to": "f.a" }, { "from": "c.value", "to": "f.b" },
                   { "from": "#b1.Click", "to": "c.increment" } ]
    })";
    REQUIRE(load(rt, *doc, json) == Status::kOk);
    // `c` and `d` have no edge at all, so they format as the empty string.
    CHECK(stringValueOf(rt, "f.out") == "HP: 2.00/");
    click(*doc, "b1");
    CHECK(stringValueOf(rt, "f.out") == "HP: 3.00/");

    // The default format is "{a}", and text that is not a placeholder is left alone.
    FlowRuntime plain;
    const std::string other = R"({
        "nodes": [ { "id": "k", "type": "constant", "params": { "value": "x" } },
                   { "id": "f", "type": "format" },
                   { "id": "g", "type": "format", "params": { "format": "{z}{a}" } } ],
        "edges": [ { "from": "k.out", "to": "f.a" }, { "from": "k.out", "to": "g.a" } ]
    })";
    REQUIRE(load(plain, *doc, other) == Status::kOk);
    CHECK(stringValueOf(plain, "f.out") == "x");
    CHECK(stringValueOf(plain, "g.out") == "{z}x");
}

TEST_CASE("counter: the `set` input writes the value, clamped like every other write") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = buttonsDoc(shaper);
    FlowRuntime rt;
    const std::string json = R"({
        "nodes": [ { "id": "src", "type": "counter", "params": { "step": 7, "initial": 0 } },
                   { "id": "dst", "type": "counter", "params": { "min": 0, "max": 5 } } ],
        "edges": [ { "from": "#b1.Click", "to": "src.increment" },
                   { "from": "src.value", "to": "dst.set" } ]
    })";
    REQUIRE(load(rt, *doc, json) == Status::kOk);
    CHECK(valueOf(rt, "dst.value") == doctest::Approx(0));
    click(*doc, "b1");
    CHECK(valueOf(rt, "src.value") == doctest::Approx(7));
    CHECK(valueOf(rt, "dst.value") == doctest::Approx(5));  // Clamped to `max`.
}

TEST_CASE("timer: repeat=false ticks once, and `start` runs it again") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = buttonsDoc(shaper);
    FlowRuntime rt;
    const std::string json = R"({
        "nodes": [ { "id": "t", "type": "timer",
                     "params": { "interval": 1, "autostart": true, "repeat": false } },
                   { "id": "c", "type": "counter", "params": {} } ],
        "edges": [ { "from": "t.tick", "to": "c.increment" },
                   { "from": "#b1.Click", "to": "t.start" } ]
    })";
    REQUIRE(load(rt, *doc, json) == Status::kOk);
    rt.tick(3.0f);  // A repeating timer would have ticked three times.
    CHECK(valueOf(rt, "c.value") == doctest::Approx(1));
    rt.tick(3.0f);
    CHECK(valueOf(rt, "c.value") == doctest::Approx(1));
    click(*doc, "b1");  // Restart.
    rt.tick(1.0f);
    CHECK(valueOf(rt, "c.value") == doctest::Approx(2));
    rt.tick(5.0f);
    CHECK(valueOf(rt, "c.value") == doctest::Approx(2));
}

TEST_CASE("timer: tick -> its own stop is allowed (a pulse self-edge is not a cycle)") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = buttonsDoc(shaper);
    FlowRuntime rt;
    const std::string json = R"({
        "nodes": [ { "id": "t", "type": "timer", "params": { "interval": 1, "autostart": true } },
                   { "id": "c", "type": "counter", "params": {} } ],
        "edges": [ { "from": "t.tick", "to": "c.increment" }, { "from": "t.tick", "to": "t.stop" } ]
    })";
    REQUIRE(load(rt, *doc, json) == Status::kOk);
    rt.tick(3.0f);
    CHECK(valueOf(rt, "c.value") == doctest::Approx(1));
}

TEST_CASE("select: its output carries the inferred type straight into a string sink") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = buttonsDoc(shaper);
    FlowRuntime rt;
    const std::string json = R"({
        "nodes": [ { "id": "t", "type": "toggle", "params": { "initial": false } },
                   { "id": "on", "type": "constant", "params": { "value": "ON" } },
                   { "id": "off", "type": "constant", "params": { "value": "OFF" } },
                   { "id": "s", "type": "select" } ],
        "edges": [ { "from": "#b1.Click", "to": "t.toggle" }, { "from": "t.value", "to": "s.cond" },
                   { "from": "on.out", "to": "s.a" }, { "from": "off.out", "to": "s.b" },
                   { "from": "s.out", "to": "#b2.setText" } ]
    })";
    REQUIRE(load(rt, *doc, json) == Status::kOk);
    CHECK(stringValueOf(rt, "s.out") == "OFF");
    click(*doc, "b1");
    CHECK(stringValueOf(rt, "s.out") == "ON");
    CHECK(doc->byId("b2").text() == "ON");
}

// -- T-258a: numbers from the graph are bounded (docs/reviews/2026-10-02-full-audit-raw/F.md F1, F16).

namespace {

/// Parses `json`, lets `edit` change the graph (to reach the runtime with values parse() would not
/// produce), then loads it.
template <class Edit>
Status loadEdited(FlowRuntime& rt, Document& doc, const std::string& json, Edit&& edit) {
    FlowGraph graph;
    std::vector<std::string> errors;
    REQUIRE(FlowGraph::parse(json, graph, errors) == Status::kOk);
    edit(graph);
    return rt.load(graph, doc, errors);
}

PortValue& paramOf(FlowGraph& graph, const std::string& node, const std::string& name) {
    for (NodeDesc& n : graph.nodes) {
        if (n.id == node) {
            return n.params[name];
        }
    }
    FAIL("no such node: " << node);
    return graph.nodes.front().params[name];
}

}  // namespace

TEST_CASE("T-258a F1: parse keeps the authored timer interval; toJson round-trips it byte for byte") {
    // T-258b (PR #269 review): the editor parses and saves with the same FlowGraph, so parse must
    // not rewrite a value; only tick() runs an interval in (0, 1 ms) at 1 ms.
    struct Case {
        const char* text;
        double expected;
    };
    for (const Case& c : {Case{"1e-10", 1e-10}, Case{"0.0005", 0.0005}, Case{"0.25", 0.25}, Case{"0", 0.0},
                          Case{"-5", -5.0}}) {
        CAPTURE(c.text);
        const std::string json =
            std::string(R"({ "nodes": [ { "id": "tmr", "type": "timer", "params": { "interval": )") + c.text +
            R"( } } ], "edges": [] })";
        FlowGraph graph;
        std::vector<std::string> errors;
        REQUIRE(FlowGraph::parse(json, graph, errors) == Status::kOk);
        CHECK(paramOf(graph, "tmr", "interval").n == c.expected);
        const std::string saved = graph.toJson();
        FlowGraph again;
        REQUIRE(FlowGraph::parse(saved, again, errors) == Status::kOk);
        CHECK(again.toJson() == saved);
        CHECK(paramOf(again, "tmr", "interval").n == c.expected);
    }
}

TEST_CASE("T-258a F1: an authored interval of 0.5 ms still fires every 1 ms") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = buttonsDoc(shaper);
    FlowRuntime rt;
    const std::string json = R"({
        "nodes": [ { "id": "tmr", "type": "timer", "params": { "interval": 0.0005, "autostart": true } } ],
        "edges": []
    })";
    REQUIRE(load(rt, *doc, json) == Status::kOk);
    rt.tick(0.0105f);  // About 10 intervals of 1 ms (21 of the authored 0.5 ms).
    CHECK(valueOf(rt, "tmr.count") >= 9);
    CHECK(valueOf(rt, "tmr.count") <= 11);
}

TEST_CASE("T-258a F1: a timer interval of 0, below 0 or non-finite never fires") {
    NullTextShaper shaper;
    const std::string json = R"({
        "nodes": [ { "id": "tmr", "type": "timer", "params": { "interval": 1, "autostart": true } } ],
        "edges": []
    })";
    for (double interval :
         {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        CAPTURE(interval);
        std::unique_ptr<Document> doc = buttonsDoc(shaper);
        FlowRuntime rt;
        REQUIRE(loadEdited(rt, *doc, json,
                           [&](FlowGraph& g) { paramOf(g, "tmr", "interval").n = interval; }) == Status::kOk);
        rt.tick(0.5f);
        rt.tick(2.0f);
        CHECK(valueOf(rt, "tmr.count") == 0.0);
    }
}

TEST_CASE("T-258a F1: one tick fires a timer at most 1000 times, then drops the rest") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = buttonsDoc(shaper);
    FlowRuntime rt;
    const std::string json = R"({
        "nodes": [ { "id": "tmr", "type": "timer", "params": { "interval": 0.001, "autostart": true } } ],
        "edges": []
    })";
    REQUIRE(load(rt, *doc, json) == Status::kOk);
    rt.tick(2.0f);  // 2000 intervals.
    CHECK(valueOf(rt, "tmr.count") == doctest::Approx(1000));
    rt.tick(0.0105f);  // The dropped remainder does not come back: about 10 more.
    CHECK(valueOf(rt, "tmr.count") >= 1009);
    CHECK(valueOf(rt, "tmr.count") <= 1011);

    // An interval parse() never produces (a graph built in code): 1e-10 s used to spin forever once
    // `timerElapsed -= interval` stopped changing the float. tick() runs it at 1 ms, capped at 1000
    // fires per tick.
    std::unique_ptr<Document> doc2 = buttonsDoc(shaper);
    FlowRuntime tiny;
    REQUIRE(loadEdited(tiny, *doc2, json, [](FlowGraph& g) { paramOf(g, "tmr", "interval").n = 1e-10; }) ==
            Status::kOk);
    tiny.tick(1.0f / 60.0f);
    CHECK(valueOf(tiny, "tmr.count") <= 1000);
    tiny.tick(5.0f);
    CHECK(valueOf(tiny, "tmr.count") <= 2000);
}

TEST_CASE("T-258a F16: random takes non-finite bounds as 0 and saturates integer bounds") {
    NullTextShaper shaper;
    {
        std::unique_ptr<Document> doc = buttonsDoc(shaper);
        FlowRuntime rt;
        const std::string json = R"({
            "nodes": [ { "id": "rnd", "type": "random", "params": { "min": 1e300, "max": 1e300, "integer": true } } ],
            "edges": [ { "from": "#b1.Click", "to": "rnd.trigger" } ]
        })";
        REQUIRE(load(rt, *doc, json) == Status::kOk);
        click(*doc, "b1");
        CHECK(valueOf(rt, "rnd.value") == doctest::Approx(9223372036854775807.0));
    }
    {
        std::unique_ptr<Document> doc = buttonsDoc(shaper);
        FlowRuntime rt;
        const std::string json = R"({
            "nodes": [ { "id": "rnd", "type": "random", "params": { "min": 1, "max": 1, "integer": true } } ],
            "edges": [ { "from": "#b2.Click", "to": "rnd.trigger" } ]
        })";
        REQUIRE(loadEdited(rt, *doc, json, [](FlowGraph& g) {
                    paramOf(g, "rnd", "min").n = std::numeric_limits<double>::quiet_NaN();
                    paramOf(g, "rnd", "max").n = std::numeric_limits<double>::quiet_NaN();
                }) == Status::kOk);
        click(*doc, "b2");
        CHECK(valueOf(rt, "rnd.value") == 0.0);
    }
    // min > max: swapped (the standard distributions' precondition used to be broken).
    for (const char* integer : {"true", "false"}) {
        CAPTURE(integer);
        std::unique_ptr<Document> doc = buttonsDoc(shaper);
        FlowRuntime rt;
        const std::string json = std::string(R"({
            "nodes": [ { "id": "rnd", "type": "random", "params": { "min": 10, "max": 1, "integer": )") +
                                 integer + R"( } } ],
            "edges": [ { "from": "#b3.Click", "to": "rnd.trigger" } ]
        })";
        REQUIRE(load(rt, *doc, json) == Status::kOk);
        for (int i = 0; i < 20; ++i) {
            click(*doc, "b3");
            CHECK(valueOf(rt, "rnd.value") >= 1.0);
            CHECK(valueOf(rt, "rnd.value") <= 10.0);
        }
    }
}
