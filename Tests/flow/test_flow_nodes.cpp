#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
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
#include "xt/text_shaper.h"

/// One behavior case per §17.6 node type (Docs/tasks/T-56.md "test_flow_nodes"): counter's
/// increment/decrement/reset + min/max clamp, toggle's toggle/setTrue/setFalse + change-only
/// `changed` pulse, timer's multi-tick-per-dt behavior, clock's accumulated seconds, random's
/// trigger (degenerate min==max range for determinism), math's every `op` (plus `clamp`/`map`),
/// compare's every `op` plus its change-only true/false pulses, toString's decimals/shortest/
/// format substitution and select's cond-driven passthrough. Pulse-driven nodes are exercised
/// through a small fixture document's buttons (Document::dispatchInput), the same technique
/// Docs/tasks/T-56.md's own test_flow_runtime.cpp uses -- FlowRuntime always binds against a real
/// Document, even for graphs (like most of these) that never reference a "#id" connection point.
///
/// See Docs/design/17-editor.md §17.6.

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
std::unique_ptr<Document> buttonsDoc(TextShaper& shaper, uint32_t behavior = 0) {
    const std::string source =
        "<xt>\n<head><style>\nbody{display:flex;flex-direction:column;}\n"
        "button{width:50px;height:20px;}\n</style></head>\n<body>\n"
        "<button id=\"b1\">1</button><button id=\"b2\">2</button><button id=\"b3\">3</button>\n"
        "<button id=\"b4\">4</button><button id=\"b5\">5</button><button id=\"b6\">6</button>\n"
        "</body>\n</xt>\n";
    std::string path = writeTempXt("buttons", source);
    compile::CompileOptions options;
    options.behavior = behavior;  // T-541: 1 = signals coalesce per frame (T-389).
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

// -- T-258a: numbers from the graph are bounded (Docs/reviews/2026-10-02-full-audit-raw/F.md F1, F16).

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

// ---- T-541 (Docs/design/27-interaction.md §27.4.4): the `state` node ---------------------------------

namespace {

std::string readFixture(const std::string& name) {
    std::ifstream in(std::string(XT_TEST_FLOW_DIR) + "/fixtures/" + name, std::ios::binary);
    REQUIRE(in.good());
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

}  // namespace

TEST_CASE("state: the game-flow machine walks its transitions and fires enter / exit / changed") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = buttonsDoc(shaper);
    FlowRuntime rt;
    REQUIRE(load(rt, *doc, readFixture("state_machine.xtflow")) == Status::kOk);

    // Loading pushes `initial` and fires no enter.
    CHECK(stringValueOf(rt, "game.state") == "Menu");
    CHECK(valueOf(rt, "game.index") == doctest::Approx(0));
    CHECK(valueOf(rt, "changed.value") == doctest::Approx(0));
    CHECK(valueOf(rt, "enterMenu.value") == doctest::Approx(0));

    click(*doc, "b2");  // pause in Menu: no transition, no output.
    CHECK(stringValueOf(rt, "game.state") == "Menu");
    CHECK(valueOf(rt, "changed.value") == doctest::Approx(0));

    click(*doc, "b1");  // start
    CHECK(stringValueOf(rt, "game.state") == "Playing");
    CHECK(valueOf(rt, "game.index") == doctest::Approx(1));
    click(*doc, "b2");  // pause
    CHECK(stringValueOf(rt, "game.state") == "Paused");
    CHECK(valueOf(rt, "game.index") == doctest::Approx(2));
    click(*doc, "b2");  // pause again in Paused: nothing.
    CHECK(stringValueOf(rt, "game.state") == "Paused");
    click(*doc, "b3");  // resume
    CHECK(stringValueOf(rt, "game.state") == "Playing");
    click(*doc, "b4");  // quit, the "*" fallback
    CHECK(stringValueOf(rt, "game.state") == "Menu");

    CHECK(valueOf(rt, "changed.value") == doctest::Approx(4));
    CHECK(valueOf(rt, "enterPaused.value") == doctest::Approx(1));
    CHECK(valueOf(rt, "exitPaused.value") == doctest::Approx(1));
    CHECK(valueOf(rt, "exitMenu.value") == doctest::Approx(1));
    CHECK(valueOf(rt, "enterMenu.value") == doctest::Approx(1));
}

TEST_CASE("state: a self-transition fires nothing, an explicit (from, on) beats the \"*\" fallback") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = buttonsDoc(shaper);
    FlowRuntime rt;
    const std::string json = R"({
        "nodes": [
            { "id": "s", "type": "state", "params": {
                "states": ["A", "B", "C"],
                "transitions": [
                    { "from": "A", "on": "stay", "to": "A" },
                    { "from": "*", "on": "go", "to": "C" },
                    { "from": "A", "on": "go", "to": "B" }
                ] } },
            { "id": "changed", "type": "counter" },
            { "id": "enterA", "type": "counter" }
        ],
        "edges": [
            { "from": "#b1.Click", "to": "s.stay" },
            { "from": "#b2.Click", "to": "s.go" },
            { "from": "s.changed", "to": "changed.increment" },
            { "from": "s.enterA", "to": "enterA.increment" }
        ]
    })";
    REQUIRE(load(rt, *doc, json) == Status::kOk);
    CHECK(stringValueOf(rt, "s.state") == "A");  // `initial` defaults to states[0].
    click(*doc, "b1");
    CHECK(valueOf(rt, "changed.value") == doctest::Approx(0));
    CHECK(valueOf(rt, "enterA.value") == doctest::Approx(0));
    click(*doc, "b2");  // (A, go) -> B wins over (*, go) -> C.
    CHECK(stringValueOf(rt, "s.state") == "B");
    click(*doc, "b2");  // (B, go) falls back to (*, go) -> C.
    CHECK(stringValueOf(rt, "s.state") == "C");
    CHECK(valueOf(rt, "changed.value") == doctest::Approx(2));
}

TEST_CASE("state: guard false swallows the events (but not set); a missing edge means true") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = buttonsDoc(shaper);
    FlowRuntime rt;
    REQUIRE(load(rt, *doc, readFixture("state_machine.xtflow")) == Status::kOk);
    click(*doc, "b5");  // gate -> false
    click(*doc, "b1");  // start: swallowed
    CHECK(stringValueOf(rt, "game.state") == "Menu");
    CHECK(valueOf(rt, "changed.value") == doctest::Approx(0));
    click(*doc, "b5");  // gate -> true
    click(*doc, "b1");
    CHECK(stringValueOf(rt, "game.state") == "Playing");

    // No `guard` edge at all: events pass (the "valid minimal graph" of the first test already ran).
    const std::string json = R"({
        "nodes": [ { "id": "s", "type": "state", "params": {
            "states": ["A", "B"], "transitions": [ { "from": "A", "on": "go", "to": "B" } ] } } ],
        "edges": [ { "from": "#b1.Click", "to": "s.go" } ]
    })";
    std::unique_ptr<Document> doc2 = buttonsDoc(shaper);
    FlowRuntime other;  // After its Document, so it is destroyed first.
    REQUIRE(load(other, *doc2, json) == Status::kOk);
    click(*doc2, "b1");
    CHECK(stringValueOf(other, "s.state") == "B");
}

TEST_CASE("state: set jumps without the transition table; an unknown name is ignored and reported once") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = buttonsDoc(shaper);
    FlowRuntime rt;
    REQUIRE(load(rt, *doc, readFixture("state_machine.xtflow")) == Status::kOk);
    CHECK(rt.runtimeErrors().empty());

    click(*doc, "b6");  // pick = 1 -> lookup "Paused" -> set, from Menu.
    CHECK(stringValueOf(rt, "game.state") == "Paused");
    CHECK(valueOf(rt, "exitMenu.value") == doctest::Approx(1));
    CHECK(valueOf(rt, "enterPaused.value") == doctest::Approx(1));
    CHECK(valueOf(rt, "changed.value") == doctest::Approx(1));

    click(*doc, "b6");  // pick = 2 -> "Nope": no output, one runtime error.
    CHECK(stringValueOf(rt, "game.state") == "Paused");
    CHECK(valueOf(rt, "changed.value") == doctest::Approx(1));
    REQUIRE(rt.runtimeErrors().size() == 1);
    CHECK(rt.runtimeErrors()[0].find("'Nope'") != std::string::npos);
    CHECK(rt.runtimeErrors()[0].find("'game'") != std::string::npos);
    click(*doc, "b6");  // pick stays clamped at 2: no new value, so nothing more.
    CHECK(rt.runtimeErrors().size() == 1);
}

TEST_CASE("state: exit<s> reaches its downstream before state is rewritten, enter<to> after") {
    // `state` feeds both emit nodes; exitMenu triggers `before`, enterPlaying triggers `after`. The
    // emit nodes report the `state` value they hold when triggered: the old state for the exit
    // pulse, the new one for the enter pulse (the pulse sees the values delivered before it,
    // 27 §27.4.1 -- the batch rule and the node's own output order agree). Both with signals
    // coalescing per frame (behavior 1) and without, driven by a click and by a timer inside tick().
    const std::string json = R"({
        "nodes": [
            { "id": "s", "type": "state", "params": {
                "states": ["Menu", "Playing"],
                "transitions": [ { "from": "Menu", "on": "start", "to": "Playing" } ] } },
            { "id": "t", "type": "timer", "params": { "interval": 0.1, "autostart": false, "repeat": false } },
            { "id": "text", "type": "toString" },
            { "id": "before", "type": "emit", "params": { "port": "before" } },
            { "id": "after", "type": "emit", "params": { "port": "after" } }
        ],
        "edges": [
            { "from": "#b1.Click", "to": "s.start" },
            { "from": "#b2.Click", "to": "t.start" },
            { "from": "t.tick", "to": "s.start" },
            { "from": "s.state", "to": "before.value" },
            { "from": "s.state", "to": "text.in" },
            { "from": "text.out", "to": "after.value" },
            { "from": "s.exitMenu", "to": "before.trigger" },
            { "from": "s.enterPlaying", "to": "after.trigger" }
        ]
    })";
    for (uint32_t behavior : {0u, 1u}) {
        for (bool byTimer : {false, true}) {
            CAPTURE(behavior);
            CAPTURE(byTimer);
            NullTextShaper shaper;
            std::unique_ptr<Document> doc = buttonsDoc(shaper, behavior);
            CHECK(doc->behavior() == behavior);
            FlowRuntime rt;
            std::vector<std::pair<std::string, std::string>> seen;  // (emit port, value text)
            rt.setHostEventListener([&seen](const FlowHostEvent& e) {
                if (e.kind == FlowHostEventKind::Emit) {
                    seen.emplace_back(e.name, e.value.s);
                }
            });
            REQUIRE(load(rt, *doc, json) == Status::kOk);
            if (byTimer) {
                click(*doc, "b2");
                rt.tick(0.2f);
            } else {
                click(*doc, "b1");
                rt.tick(0.0f);
            }
            REQUIRE(seen.size() == 2);
            CHECK(seen[0] == std::make_pair(std::string("before"), std::string("Menu")));
            CHECK(seen[1] == std::make_pair(std::string("after"), std::string("Playing")));
        }
    }
}

TEST_CASE("state: events feeding back through enter / exit are the general cycle rule") {
    // A pulse edge from a node to itself is not a cycle (timer.tick -> timer.stop): same here.
    const std::string self = R"({
        "nodes": [ { "id": "s", "type": "state", "params": {
            "states": ["A", "B"], "transitions": [ { "from": "A", "on": "go", "to": "B" },
                                                    { "from": "B", "on": "back", "to": "A" } ] } } ],
        "edges": [ { "from": "s.enterB", "to": "s.back" } ]
    })";
    FlowGraph graph;
    std::vector<std::string> errors;
    CHECK(FlowGraph::parse(self, graph, errors) == Status::kOk);
    // Through a second node it is a loop.
    const std::string loop = R"({
        "nodes": [ { "id": "s", "type": "state", "params": {
            "states": ["A", "B"], "transitions": [ { "from": "A", "on": "go", "to": "B" } ] } },
                   { "id": "m", "type": "merge" } ],
        "edges": [ { "from": "s.enterB", "to": "m.in" }, { "from": "m.out", "to": "s.go" } ]
    })";
    errors.clear();
    CHECK(FlowGraph::parse(loop, graph, errors) == Status::kInvalidArgument);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0] == "cycle detected in the node graph");
}

// ---- T-543 (Docs/design/27-interaction.md §27.4.5): filter -------------------------------------

namespace {

/// Declared out ports a host sets (`Hud.v` number, `Hud.flag` bool, `Hud.name` string) and three
/// buttons for the pulse inputs. A declared port only reports a change, so a value is repeated by
/// a `sample` filter whose `trigger` is a button.
constexpr const char* kFilterDoc = R"X(<xt><head><style>
  #b1 { position: absolute; left: 0px; top: 0px; width: 40px; height: 20px; }
  #b2 { position: absolute; left: 0px; top: 30px; width: 40px; height: 20px; }
</style></head>
<body id="Root"><div id="Hud" port:v="number out" port:flag="bool out" port:name="string out"></div>
  <div id="b1"></div><div id="b2"></div></body></xt>)X";

std::unique_ptr<Document> filterDoc(TextShaper& shaper) {
    const compile::SourceFile file{"filter.xt", kFilterDoc};
    compile::CompileOptions options;
    options.behavior = 1;
    compile::CompileResult result = compile::compileDocumentFromStrings({&file, 1}, "filter.xt", options);
    REQUIRE(result.ok());
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &shaper, nullptr, doc) ==
            Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 300});
    doc->update(0.0f);
    return doc;
}

void clickAt(Document& doc, float x, float y) {
    InputEvent ev;
    ev.kind = InputEvent::PointerDown;
    ev.pos = Vec2{x, y};
    doc.dispatchInput(ev);
    ev.kind = InputEvent::PointerUp;
    doc.dispatchInput(ev);
}

/// Every `emit` event of a runtime, in order: which node sent it and the value it carried.
struct EmitLog {
    struct Entry {
        std::string port;
        PortValue value;
    };
    std::vector<Entry> entries;
    void attach(FlowRuntime& rt) {
        rt.setHostEventListener([this](const FlowHostEvent& ev) {
            if (ev.kind == FlowHostEventKind::Emit) {
                entries.push_back(Entry{ev.name, ev.value});
            }
        });
    }
    /// The numbers sent by the `emit` named `port`.
    std::vector<double> numbers(const std::string& port) const {
        std::vector<double> out;
        for (const Entry& e : entries) {
            if (e.port == port) {
                out.push_back(e.value.n);
            }
        }
        return out;
    }
    size_t count(const std::string& port) const {
        size_t n = 0;
        for (const Entry& e : entries) {
            n += e.port == port ? 1u : 0u;
        }
        return n;
    }
};

bool sameNumbers(const std::vector<double>& got, const std::vector<double>& want) {
    if (got.size() != want.size()) {
        return false;
    }
    for (size_t i = 0; i < got.size(); ++i) {
        if (std::abs(got[i] - want[i]) > 1e-6) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE(
    "T-543 filter deadband 0.1: 0, 0.05, 0.2, 0.25, 0.31 send 0, 0.2, 0.31; reset sends the next input") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = filterDoc(shaper);
    FlowRuntime rt;
    EmitLog log;
    log.attach(rt);
    // `f` is fed by the port directly; `s` -> `g` repeats the port's current value on every
    // click of #b1 (a declared port does not report an unchanged value), reset by #b2.
    const std::string json = R"({
        "nodes": [
            { "id": "f", "type": "filter", "params": { "op": "deadband", "threshold": 0.1 } },
            { "id": "s", "type": "filter", "params": { "op": "sample" } },
            { "id": "g", "type": "filter", "params": { "op": "deadband", "threshold": 0.1 } },
            { "id": "ef", "type": "emit", "params": { "port": "f" } },
            { "id": "eg", "type": "emit", "params": { "port": "g" } }
        ],
        "edges": [
            { "from": "#Hud.v", "to": "f.in" },
            { "from": "f.out", "to": "ef.value" }, { "from": "f.passed", "to": "ef.trigger" },
            { "from": "#Hud.v", "to": "s.in" }, { "from": "#b1.Click", "to": "s.trigger" },
            { "from": "s.out", "to": "g.in" }, { "from": "#b2.Click", "to": "g.reset" },
            { "from": "g.out", "to": "eg.value" }, { "from": "g.passed", "to": "eg.trigger" }
        ]
    })";
    REQUIRE(load(rt, *doc, json) == Status::kOk);
    const PortId v = doc->lookupPort("Hud.v");
    REQUIRE(v != kInvalidPort);
    // The port starts at 0 and reports changes only, so 0.7 first gives both filters a last value;
    // then the inputs 0, 0.05, 0.2, 0.25, 0.31.
    for (float input : {0.7f, 0.0f, 0.05f, 0.2f, 0.25f, 0.31f}) {
        REQUIRE(doc->setPort(v, Value::ofFloat(input)) == Status::kOk);
        rt.tick(1.0f / 60.0f);
        clickAt(*doc, 20.0f, 10.0f);  // s -> g sees the same input.
    }
    CHECK(sameNumbers(log.numbers("f"), {0.7, 0.0, 0.2, 0.31}));
    CHECK(sameNumbers(log.numbers("g"), {0.7, 0.0, 0.2, 0.31}));

    // The same value again: not sent, until `reset` makes the next input count.
    clickAt(*doc, 20.0f, 10.0f);
    CHECK(log.count("g") == 4);
    clickAt(*doc, 20.0f, 40.0f);  // reset
    CHECK(log.count("g") == 4);
    clickAt(*doc, 20.0f, 10.0f);
    REQUIRE(log.count("g") == 5);
    CHECK(sameNumbers({log.numbers("g").back()}, {0.31}));
    CHECK(valueOf(rt, "g.out") == doctest::Approx(0.31));
}

TEST_CASE("T-543 filter deadband: bool and string send only when the value changed") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = filterDoc(shaper);
    FlowRuntime rt;
    EmitLog log;
    log.attach(rt);
    const std::string json = R"({
        "nodes": [
            { "id": "sb", "type": "filter", "params": { "op": "sample" } },
            { "id": "fb", "type": "filter", "params": { "op": "deadband", "threshold": 5 } },
            { "id": "ss", "type": "filter", "params": { "op": "sample" } },
            { "id": "fs", "type": "filter", "params": { "op": "deadband", "threshold": 0 } },
            { "id": "eb", "type": "emit", "params": { "port": "b" } },
            { "id": "es", "type": "emit", "params": { "port": "s" } }
        ],
        "edges": [
            { "from": "#Hud.flag", "to": "sb.in" }, { "from": "#b1.Click", "to": "sb.trigger" },
            { "from": "sb.out", "to": "fb.in" },
            { "from": "fb.out", "to": "eb.value" }, { "from": "fb.passed", "to": "eb.trigger" },
            { "from": "#Hud.name", "to": "ss.in" }, { "from": "#b1.Click", "to": "ss.trigger" },
            { "from": "ss.out", "to": "fs.in" },
            { "from": "fs.out", "to": "es.value" }, { "from": "fs.passed", "to": "es.trigger" }
        ]
    })";
    REQUIRE(load(rt, *doc, json) == Status::kOk);
    const PortId flag = doc->lookupPort("Hud.flag");
    const PortId name = doc->lookupPort("Hud.name");
    REQUIRE(flag != kInvalidPort);
    REQUIRE(name != kInvalidPort);
    // true, true, false: the second `true` is a repeat of the sent value.
    for (float b : {1.0f, 1.0f, 0.0f}) {
        REQUIRE(doc->setPort(flag, Value::ofFloat(b)) == Status::kOk);
        rt.tick(0.0f);
        clickAt(*doc, 20.0f, 10.0f);
    }
    REQUIRE(log.count("b") == 2);
    CHECK(log.entries[0].port == "b");
    CHECK(log.entries[0].value.type == PortType::Bool);
    CHECK(log.entries[0].value.b);
    CHECK_FALSE(log.entries.back().value.b);
    // Strings: the clicks above found no name yet (nothing to sample), then two names.
    CHECK(log.count("s") == 0);
    for (const char* text : {"ada", "ada", "bob"}) {
        REQUIRE(doc->setPortText(name, text) == Status::kOk);
        rt.tick(0.0f);
        clickAt(*doc, 20.0f, 10.0f);
    }
    REQUIRE(log.count("s") == 2);
    std::vector<std::string> sent;
    for (const EmitLog::Entry& e : log.entries) {
        if (e.port == "s") {
            REQUIRE(e.value.type == PortType::String);
            sent.push_back(e.value.s);
        }
    }
    CHECK(sent == std::vector<std::string>{"ada", "bob"});
}

TEST_CASE("T-543 filter quantize 0.5: 0.1, 0.2, 0.3, 0.76 send 0, 0.5, 1.0") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = filterDoc(shaper);
    FlowRuntime rt;
    EmitLog log;
    log.attach(rt);
    const std::string json = R"({
        "nodes": [
            { "id": "q", "type": "filter", "params": { "op": "quantize", "step": 0.5 } },
            { "id": "e", "type": "emit", "params": { "port": "q" } }
        ],
        "edges": [
            { "from": "#document.pointerX", "to": "q.in" },
            { "from": "q.out", "to": "e.value" }, { "from": "q.passed", "to": "e.trigger" }
        ]
    })";
    REQUIRE(load(rt, *doc, json) == Status::kOk);
    // The initial reading (the viewport center, 200) quantizes to 200: the first value sent.
    rt.tick(0.0f);
    log.entries.clear();
    // 0.1 -> 0, 0.2 -> 0 (same as the last sent), 0.3 -> 0.5, 0.76 -> 1.0 (the pointer moves in
    // float pixels; 0.5 steps are exact in binary).
    for (float x : {0.1f, 0.2f, 0.3f, 0.76f}) {
        InputEvent ev;
        ev.kind = InputEvent::PointerMove;
        ev.pos = Vec2{x, 10.0f};
        doc->dispatchInput(ev);
        rt.tick(1.0f / 60.0f);
    }
    CHECK(sameNumbers(log.numbers("q"), {0.0, 0.5, 1.0}));
    CHECK(valueOf(rt, "q.out") == doctest::Approx(1.0));
}

TEST_CASE("T-543 filter sample: `in` stores, `trigger` sends the current value every time") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = filterDoc(shaper);
    FlowRuntime rt;
    EmitLog log;
    log.attach(rt);
    const std::string json = R"({
        "nodes": [
            { "id": "s", "type": "filter", "params": { "op": "sample" } },
            { "id": "e", "type": "emit", "params": { "port": "s" } }
        ],
        "edges": [
            { "from": "#Hud.v", "to": "s.in" }, { "from": "#b1.Click", "to": "s.trigger" },
            { "from": "s.out", "to": "e.value" }, { "from": "s.passed", "to": "e.trigger" }
        ]
    })";
    REQUIRE(load(rt, *doc, json) == Status::kOk);
    clickAt(*doc, 20.0f, 10.0f);  // No value has reached `in` yet: nothing to send.
    CHECK(log.entries.empty());
    const PortId v = doc->lookupPort("Hud.v");
    for (float input : {1.0f, 2.0f, 3.0f}) {
        REQUIRE(doc->setPort(v, Value::ofFloat(input)) == Status::kOk);
        rt.tick(1.0f / 60.0f);
    }
    CHECK(log.entries.empty());  // `in` alone sends nothing.
    clickAt(*doc, 20.0f, 10.0f);
    clickAt(*doc, 20.0f, 10.0f);  // The same value again is still sent.
    CHECK(sameNumbers(log.numbers("s"), {3.0, 3.0}));
}

TEST_CASE("T-543 filter: FlowRuntime::load() repeats the edge checks on a hand-edited graph") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = filterDoc(shaper);
    FlowRuntime rt;
    FlowGraph graph;
    std::vector<std::string> errors;
    REQUIRE(FlowGraph::parse(R"({
        "nodes": [ { "id": "f", "type": "filter", "params": { "op": "deadband", "threshold": 1 } },
                   { "id": "b", "type": "toggle" } ],
        "edges": [ { "from": "b.value", "to": "f.in" } ]
    })",
                             graph, errors) == Status::kOk);
    graph.nodes[0].params["op"].s = "quantize";
    graph.nodes[0].params["step"].type = PortType::Number;
    graph.nodes[0].params["step"].n = 1;
    errors.clear();
    CHECK(rt.load(graph, *doc, errors) == Status::kInvalidArgument);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0] == "filter f: quantize takes a number");
}
