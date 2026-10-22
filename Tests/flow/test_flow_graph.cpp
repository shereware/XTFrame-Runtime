#include <fstream>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/flow/flow_graph.h"

/// FlowGraph::parse(): JSON parsing plus every structural check that does not need a Document
/// (Docs/design/17-editor.md §17.6, Docs/tasks/T-56.md "test_flow_graph"): the §17.6 sample
/// graph parses cleanly; type mismatch, value-input fan-in, a node-to-node cycle and an unknown
/// node type/port each produce (at least) one error and Status::kInvalidArgument.

using namespace xt;
using namespace xt::flow;

TEST_CASE("FlowGraph::parse: the Docs/design/17-editor.md §17.6 sample graph") {
    const std::string json = R"({
        "version": 1,
        "nodes": [
            { "id": "n1", "type": "counter", "params": { "step": 1 } },
            { "id": "n2", "type": "toString", "params": { "format": "{}" } }
        ],
        "edges": [
            { "from": "#inc.Click", "to": "n1.increment" },
            { "from": "n1.value", "to": "n2.in" },
            { "from": "n2.out", "to": "#count.setText" },
            { "from": "#darkToggle.Change", "to": "#root.toggleClass", "params": { "class": "dark" } }
        ]
    })";

    FlowGraph graph;
    std::vector<std::string> errors;
    Status status = FlowGraph::parse(json, graph, errors);
    for (const std::string& e : errors) {
        CAPTURE(e);
    }
    CHECK(status == Status::kOk);
    CHECK(errors.empty());
    REQUIRE(graph.nodes.size() == 2);
    CHECK(graph.nodes[0].id == "n1");
    CHECK(graph.nodes[0].type == "counter");
    CHECK(graph.nodes[1].type == "toString");
    REQUIRE(graph.edges.size() == 4);
    CHECK(graph.edges[3].params.at("class") == "dark");
}

TEST_CASE("FlowGraph::parse: malformed JSON is rejected") {
    FlowGraph graph;
    std::vector<std::string> errors;
    Status status = FlowGraph::parse("{ not json", graph, errors);
    CHECK(status == Status::kInvalidArgument);
    CHECK_FALSE(errors.empty());
}

TEST_CASE("FlowGraph::parse: type mismatch (string source -> a strict number sink) is one error") {
    // Note: a pulse sink (e.g. counter.increment) accepts ANY source type, not just pulse --
    // Docs/design/17-editor.md §17.6's own sample graph relies on this (see the T-56 report), so
    // this case instead uses math.a, one of the few strictly-typed (non-flexible) value sinks.
    const std::string json = R"({
        "nodes": [
            { "id": "c", "type": "constant", "params": { "value": "hello" } },
            { "id": "m", "type": "math", "params": { "op": "add" } }
        ],
        "edges": [ { "from": "c.out", "to": "m.a" } ]
    })";
    FlowGraph graph;
    std::vector<std::string> errors;
    Status status = FlowGraph::parse(json, graph, errors);
    CHECK(status == Status::kInvalidArgument);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].find("type mismatch") != std::string::npos);
}

TEST_CASE("FlowGraph::parse: a value input with two incoming edges is one error") {
    const std::string json = R"({
        "nodes": [
            { "id": "c1", "type": "constant", "params": { "value": 1 } },
            { "id": "c2", "type": "constant", "params": { "value": 2 } },
            { "id": "m", "type": "math", "params": { "op": "add" } }
        ],
        "edges": [
            { "from": "c1.out", "to": "m.a" },
            { "from": "c2.out", "to": "m.a" }
        ]
    })";
    FlowGraph graph;
    std::vector<std::string> errors;
    Status status = FlowGraph::parse(json, graph, errors);
    CHECK(status == Status::kInvalidArgument);
    bool found = false;
    for (const std::string& e : errors) {
        if (e.find("more than one incoming edge") != std::string::npos) {
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("FlowGraph::parse: a node-to-node cycle is one error") {
    const std::string json = R"({
        "nodes": [
            { "id": "m1", "type": "math", "params": { "op": "add" } },
            { "id": "m2", "type": "math", "params": { "op": "add" } }
        ],
        "edges": [
            { "from": "m1.out", "to": "m2.a" },
            { "from": "m2.out", "to": "m1.a" }
        ]
    })";
    FlowGraph graph;
    std::vector<std::string> errors;
    Status status = FlowGraph::parse(json, graph, errors);
    CHECK(status == Status::kInvalidArgument);
    bool found = false;
    for (const std::string& e : errors) {
        if (e.find("cycle") != std::string::npos) {
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("FlowGraph::parse: unknown node type is an error") {
    const std::string json = R"({
        "nodes": [ { "id": "x", "type": "bogus", "params": {} } ],
        "edges": []
    })";
    FlowGraph graph;
    std::vector<std::string> errors;
    Status status = FlowGraph::parse(json, graph, errors);
    CHECK(status == Status::kInvalidArgument);
    bool found = false;
    for (const std::string& e : errors) {
        if (e.find("unknown node type") != std::string::npos) {
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("FlowGraph::toJson: round-trips Examples/desktop_app/ui/app.xtflow") {
    std::ifstream in(std::string(XT_EXAMPLES_DIR) + "/app.xtflow");
    REQUIRE(in.is_open());
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string original = ss.str();

    FlowGraph graph;
    std::vector<std::string> errors;
    REQUIRE(FlowGraph::parse(original, graph, errors) == Status::kOk);

    std::string json1 = graph.toJson();
    // Fixed top-level key order.
    CHECK(json1.find("\"version\"") < json1.find("\"nodes\""));
    CHECK(json1.find("\"nodes\"") < json1.find("\"edges\""));
    // 2-space indent.
    CHECK(json1.find("\n  \"version\"") != std::string::npos);

    FlowGraph reparsed;
    std::vector<std::string> errors2;
    REQUIRE(FlowGraph::parse(json1, reparsed, errors2) == Status::kOk);
    std::string json2 = reparsed.toJson();
    CHECK(json1 == json2);

    REQUIRE(reparsed.nodes.size() == graph.nodes.size());
    for (size_t i = 0; i < graph.nodes.size(); ++i) {
        CHECK(reparsed.nodes[i].id == graph.nodes[i].id);
        CHECK(reparsed.nodes[i].type == graph.nodes[i].type);
    }
    REQUIRE(reparsed.edges.size() == graph.edges.size());
    for (size_t i = 0; i < graph.edges.size(); ++i) {
        CHECK(reparsed.edges[i].from == graph.edges[i].from);
        CHECK(reparsed.edges[i].to == graph.edges[i].to);
    }
}

TEST_CASE("nodeTypeTable: the fourteen §17.6 node types, ports matching the shared port rules") {
    std::span<const NodeTypeDesc> table = nodeTypeTable();
    // T-193 appended six types (system, sound, haptic, spawn, emit, timeline; test_flow_global.cpp).
    REQUIRE(table.size() == 20);

    std::set<std::string> names;
    for (const NodeTypeDesc& d : table.first(14)) {
        names.insert(d.type);
    }
    CHECK(names == std::set<std::string>{"constant", "counter", "toggle", "timer", "clock", "random", "math",
                                         "compare", "toString", "select", "radio", "lookup", "merge",
                                         "format"});

    auto find = [&](const std::string& type) -> const NodeTypeDesc& {
        for (const NodeTypeDesc& d : table) {
            if (d.type == type) {
                return d;
            }
        }
        FAIL("missing node type " << type);
        static NodeTypeDesc dummy;
        return dummy;
    };

    const NodeTypeDesc& counter = find("counter");
    REQUIRE(counter.inputs.size() == 4);  // T-123 adds `set`.
    CHECK(counter.inputs[0].name == "increment");
    CHECK(counter.inputs[0].type == PortType::Pulse);
    CHECK(counter.inputs[3].name == "set");
    CHECK(counter.inputs[3].type == PortType::Number);
    REQUIRE(counter.outputs.size() == 1);
    CHECK(counter.outputs[0].name == "value");
    CHECK(counter.outputs[0].type == PortType::Number);

    const NodeTypeDesc& compare = find("compare");
    REQUIRE(compare.outputs.size() == 3);
    CHECK(compare.outputs[0].name == "out");
    CHECK(compare.outputs[0].type == PortType::Bool);
    CHECK(compare.outputs[1].name == "true");
    CHECK(compare.outputs[1].type == PortType::Pulse);

    // T-123's four new types.
    const NodeTypeDesc& radio = find("radio");
    REQUIRE(radio.inputs.size() == 3);
    CHECK(radio.inputs[0].name == "set0");
    CHECK(radio.inputs[2].name == "set");
    CHECK(radio.inputs[2].type == PortType::Number);
    REQUIRE(radio.outputs.size() == 2);
    CHECK(radio.outputs[0].name == "index");
    CHECK(radio.outputs[1].type == PortType::Pulse);
    const NodeTypeDesc& merge = find("merge");
    CHECK(merge.inputs[0].type == PortType::Pulse);
    CHECK(merge.outputs[0].type == PortType::Pulse);
    const NodeTypeDesc& format = find("format");
    REQUIRE(format.inputs.size() == 4);
    CHECK(format.outputs[0].type == PortType::String);
    CHECK(find("lookup").inputs[0].name == "index");

    const NodeTypeDesc& toggle = find("toggle");
    REQUIRE(toggle.outputs.size() == 2);
    CHECK(toggle.outputs[0].name == "value");
    CHECK(toggle.outputs[1].name == "changed");
}

TEST_CASE("FlowGraph::parse: unknown port on a known node type is an error") {
    const std::string json = R"({
        "nodes": [ { "id": "c", "type": "counter", "params": {} } ],
        "edges": [ { "from": "c.bogus", "to": "c.increment" } ]
    })";
    FlowGraph graph;
    std::vector<std::string> errors;
    Status status = FlowGraph::parse(json, graph, errors);
    CHECK(status == Status::kInvalidArgument);
    bool found = false;
    for (const std::string& e : errors) {
        if (e.find("no output port") != std::string::npos) {
            found = true;
        }
    }
    CHECK(found);
}

// ---------------------------------------------------------------------------------------------
// T-123: radio/lookup/merge/format params, `select`'s inferred type, `setStyle` fan-in and the
// pulse self-edge exception. See Docs/design/17-editor.md §17.6.
// ---------------------------------------------------------------------------------------------

namespace {

/// Parses `json`, expecting failure, and returns true when some error mentions `needle`.
bool rejectedWith(const std::string& json, const std::string& needle) {
    FlowGraph graph;
    std::vector<std::string> errors;
    if (FlowGraph::parse(json, graph, errors) != Status::kInvalidArgument) {
        return false;
    }
    for (const std::string& e : errors) {
        if (e.find(needle) != std::string::npos) {
            return true;
        }
    }
    for (const std::string& e : errors) {
        CAPTURE(e);
    }
    return false;
}

Status parseOk(const std::string& json, FlowGraph& graph) {
    std::vector<std::string> errors;
    Status status = FlowGraph::parse(json, graph, errors);
    for (const std::string& e : errors) {
        CAPTURE(e);
        CHECK(e.empty());
    }
    return status;
}

}  // namespace

TEST_CASE("FlowGraph::parse: radio's count and its set<i> ports") {
    FlowGraph graph;
    CHECK(parseOk(R"({
        "nodes": [ { "id": "r", "type": "radio", "params": { "count": 3, "initial": 1 } },
                   { "id": "k", "type": "clock" } ],
        "edges": [ { "from": "#a.Click", "to": "r.set0" }, { "from": "#b.Click", "to": "r.set2" },
                   { "from": "k.seconds", "to": "r.set" } ]
    })",
                  graph) == Status::kOk);

    // set3 is past `count`; only set0..set2 exist.
    CHECK(rejectedWith(R"({
        "nodes": [ { "id": "r", "type": "radio", "params": { "count": 3 } } ],
        "edges": [ { "from": "#a.Click", "to": "r.set3" } ]
    })",
                       "no input port"));
    CHECK(rejectedWith(R"({ "nodes": [ { "id": "r", "type": "radio" } ], "edges": [] })",
                       "missing required param 'count'"));
    CHECK(rejectedWith(R"({
        "nodes": [ { "id": "r", "type": "radio", "params": { "count": 1 } } ], "edges": []
    })",
                       "invalid 'count'"));
    CHECK(rejectedWith(R"({
        "nodes": [ { "id": "r", "type": "radio", "params": { "count": 100 } } ], "edges": []
    })",
                       "invalid 'count'"));
}

TEST_CASE("FlowGraph::parse: lookup's values array types its out port") {
    FlowGraph graph;
    CHECK(parseOk(R"({
        "nodes": [ { "id": "l", "type": "lookup", "params": { "values": ["a", "b"] } },
                   { "id": "r", "type": "radio", "params": { "count": 2 } } ],
        "edges": [ { "from": "r.index", "to": "l.index" }, { "from": "l.out", "to": "#t.setText" } ]
    })",
                  graph) == Status::kOk);
    REQUIRE(graph.nodes[0].arrayParams.count("values") == 1);
    CHECK(graph.nodes[0].arrayParams.at("values").size() == 2);
    CHECK(graph.nodes[0].arrayParams.at("values")[1].s == "b");
    // Round-trips through toJson() with the array intact.
    FlowGraph again;
    CHECK(parseOk(graph.toJson(), again) == Status::kOk);
    CHECK(again.nodes[0].arrayParams.at("values")[0].s == "a");

    // A number table cannot drive setText (a string sink).
    CHECK(rejectedWith(R"({
        "nodes": [ { "id": "l", "type": "lookup", "params": { "values": [1, 2] } } ],
        "edges": [ { "from": "l.out", "to": "#t.setText" } ]
    })",
                       "type mismatch"));
    CHECK(rejectedWith(R"({ "nodes": [ { "id": "l", "type": "lookup" } ], "edges": [] })",
                       "missing required param 'values'"));
    CHECK(rejectedWith(R"({
        "nodes": [ { "id": "l", "type": "lookup", "params": { "values": [1, "x"] } } ], "edges": []
    })",
                       "same-typed"));
    CHECK(rejectedWith(R"({
        "nodes": [ { "id": "l", "type": "lookup", "params": { "values": [] } } ], "edges": []
    })",
                       "same-typed"));
}

TEST_CASE("FlowGraph::parse: select's type comes from its a/b edges") {
    FlowGraph graph;
    // A chain of selects: the second one's type is only knowable once the first one's is.
    CHECK(parseOk(R"({
        "nodes": [ { "id": "s1", "type": "select" }, { "id": "s2", "type": "select" },
                   { "id": "t", "type": "toggle", "params": { "initial": false } },
                   { "id": "c1", "type": "constant", "params": { "value": "on" } },
                   { "id": "c2", "type": "constant", "params": { "value": "off" } } ],
        "edges": [ { "from": "t.value", "to": "s2.cond" }, { "from": "s1.out", "to": "s2.a" },
                   { "from": "c2.out", "to": "s2.b" }, { "from": "t.value", "to": "s1.cond" },
                   { "from": "c1.out", "to": "s1.a" }, { "from": "c2.out", "to": "s1.b" },
                   { "from": "s2.out", "to": "#t.setText" } ]
    })",
                  graph) == Status::kOk);

    // Only `a` wired.
    CHECK(rejectedWith(R"({
        "nodes": [ { "id": "s", "type": "select" },
                   { "id": "c", "type": "constant", "params": { "value": "x" } } ],
        "edges": [ { "from": "c.out", "to": "s.a" } ]
    })",
                       "select needs a and b"));
    // Nothing wired at all.
    CHECK(rejectedWith(R"({ "nodes": [ { "id": "s", "type": "select" } ], "edges": [] })",
                       "select needs a and b"));
    // a and b disagree.
    CHECK(rejectedWith(R"({
        "nodes": [ { "id": "s", "type": "select" },
                   { "id": "c1", "type": "constant", "params": { "value": "x" } },
                   { "id": "c2", "type": "constant", "params": { "value": 3 } } ],
        "edges": [ { "from": "c1.out", "to": "s.a" }, { "from": "c2.out", "to": "s.b" } ]
    })",
                       "type mismatch"));
    // A typed select.out only fits a sink of that same type.
    CHECK(rejectedWith(R"({
        "nodes": [ { "id": "s", "type": "select" },
                   { "id": "c1", "type": "constant", "params": { "value": 1 } },
                   { "id": "c2", "type": "constant", "params": { "value": 2 } } ],
        "edges": [ { "from": "c1.out", "to": "s.a" }, { "from": "c2.out", "to": "s.b" },
                   { "from": "s.out", "to": "#t.setText" } ]
    })",
                       "type mismatch"));
}

TEST_CASE("FlowGraph::parse: setStyle takes several edges, other value inputs do not") {
    FlowGraph graph;
    CHECK(parseOk(R"({
        "nodes": [ { "id": "c1", "type": "constant", "params": { "value": "color: red" } },
                   { "id": "c2", "type": "constant", "params": { "value": "width: 10px" } } ],
        "edges": [ { "from": "c1.out", "to": "#box.setStyle" },
                   { "from": "c2.out", "to": "#box.setStyle" } ]
    })",
                  graph) == Status::kOk);
    CHECK(rejectedWith(R"({
        "nodes": [ { "id": "c1", "type": "constant", "params": { "value": "a" } },
                   { "id": "c2", "type": "constant", "params": { "value": "b" } } ],
        "edges": [ { "from": "c1.out", "to": "#box.setText" },
                   { "from": "c2.out", "to": "#box.setText" } ]
    })",
                       "more than one incoming edge"));
}

TEST_CASE("FlowGraph::parse: a pulse self-edge is not a cycle") {
    FlowGraph graph;
    CHECK(parseOk(R"({
        "nodes": [ { "id": "t", "type": "timer", "params": { "interval": 1 } } ],
        "edges": [ { "from": "t.tick", "to": "t.stop" } ]
    })",
                  graph) == Status::kOk);
    // A value-carrying self-edge still is one.
    CHECK(rejectedWith(R"({
        "nodes": [ { "id": "c", "type": "counter", "params": {} } ],
        "edges": [ { "from": "c.value", "to": "c.set" } ]
    })",
                       "cycle detected"));
    // And so is a two-node loop of pulses.
    CHECK(rejectedWith(R"({
        "nodes": [ { "id": "t1", "type": "timer", "params": { "interval": 1 } },
                   { "id": "t2", "type": "timer", "params": { "interval": 1 } } ],
        "edges": [ { "from": "t1.tick", "to": "t2.start" }, { "from": "t2.tick", "to": "t1.start" } ]
    })",
                       "cycle detected"));
}

TEST_CASE("FlowGraph::parse: merge, format and the new element ports") {
    FlowGraph graph;
    CHECK(parseOk(R"({
        "nodes": [ { "id": "m", "type": "merge" },
                   { "id": "f", "type": "format", "params": { "format": "{a}/{b}", "decimals": 1 } },
                   { "id": "k", "type": "clock" },
                   { "id": "n", "type": "constant", "params": { "value": "spin" } } ],
        "edges": [ { "from": "#a.Click", "to": "m.in" }, { "from": "#b.Click", "to": "m.in" },
                   { "from": "m.out", "to": "#c.stopAnimation" },
                   { "from": "k.seconds", "to": "f.a" }, { "from": "#d.Change", "to": "f.b" },
                   { "from": "f.out", "to": "#e.setText" },
                   { "from": "n.out", "to": "#c.playAnimation" } ]
    })",
                  graph) == Status::kOk);
    // `playAnimation` is a string port: a number source does not fit.
    CHECK(rejectedWith(R"({
        "nodes": [ { "id": "k", "type": "clock" } ],
        "edges": [ { "from": "k.seconds", "to": "#c.playAnimation" } ]
    })",
                       "type mismatch"));
}

TEST_CASE("FlowGraph::parse: the T-123 difficulty-select fixture") {
    const std::string path = std::string(XT_TEST_FLOW_DIR) +
                             "/../import/fixtures/dc/difficulty-select/difficulty-select.v2.xtflow";
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::ostringstream buffer;
    buffer << in.rdbuf();

    FlowGraph graph;
    std::vector<std::string> errors;
    Status status = FlowGraph::parse(buffer.str(), graph, errors);
    for (const std::string& e : errors) {
        CAPTURE(e);
        CHECK(e.empty());
    }
    CHECK(status == Status::kOk);
    // T-123's budget: the v1 graph needed 196 nodes for the same screen.
    CHECK(graph.nodes.size() <= 40);
    CHECK(graph.nodes.size() > 30);
}

namespace {

/// T-262c: `levels` nested arrays, `[[[...]]]`.
std::string nestedArrays(int levels) {
    return std::string(static_cast<size_t>(levels), '[') + std::string(static_cast<size_t>(levels), ']');
}

}  // namespace

TEST_CASE("FlowGraph::parse: T-262c a 1100-level .xtflow is rejected and the process lives") {
    const std::string json =
        R"({"version": 1, "nodes": [{"id": "n1", "type": "counter", "params": {"step": )" +
        nestedArrays(1100) + R"(}}], "edges": []})";
    FlowGraph graph;
    std::vector<std::string> errors;
    CHECK(FlowGraph::parse(json, graph, errors) == Status::kInvalidArgument);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0] == ".xtflow: nesting too deep");
    CHECK(graph.nodes.empty());
}

TEST_CASE("FlowGraph::parse: T-262c 32 levels parse; 33 levels is rejected") {
    FlowGraph graph;
    std::vector<std::string> errors;
    // The root object is level 1, so 31 arrays under a key make 32 levels.
    CHECK(FlowGraph::parse(R"({"nodes": [], "edges": [], "extra": )" + nestedArrays(31) + "}", graph,
                           errors) == Status::kOk);
    CHECK(errors.empty());
    CHECK(FlowGraph::parse(R"({"nodes": [], "edges": [], "extra": )" + nestedArrays(32) + "}", graph,
                           errors) == Status::kInvalidArgument);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0] == ".xtflow: nesting too deep");
}

TEST_CASE("T-406 .xtflow version: missing = 1, newer refused, malformed rejected, written as the latest") {
    FlowGraph graph;
    std::vector<std::string> errors;
    CHECK(FlowGraph::parse(R"({"nodes": [], "edges": []})", graph, errors) == Status::kOk);
    CHECK(FlowGraph::parse(R"({"version": 1, "nodes": [], "edges": []})", graph, errors) == Status::kOk);
    errors.clear();
    CHECK(FlowGraph::parse(R"({"version": 2, "nodes": [], "edges": []})", graph, errors) ==
          Status::kUnsupported);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].find("a newer XTFrame is needed") != std::string::npos);
    errors.clear();
    CHECK(FlowGraph::parse(R"({"version": "1", "nodes": [], "edges": []})", graph, errors) ==
          Status::kInvalidArgument);
    errors.clear();
    CHECK(FlowGraph::parse(R"({"version": 0, "nodes": [], "edges": []})", graph, errors) ==
          Status::kInvalidArgument);
    CHECK(FlowGraph{}.toJson().find("\"version\": " + std::to_string(kFlowFormatVersion)) !=
          std::string::npos);
}
