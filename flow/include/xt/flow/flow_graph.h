#pragma once

#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xt/types.h"

/// xt_flow's data model: the parsed `.xtflow` document (nodes/edges/params), before any binding
/// to a live Document. See docs/design/17-editor.md §17.6, docs/tasks/T-56.md.

namespace xt::flow {

/// Signal port value type (docs/design/17-editor.md §17.6). Pulse carries no value.
enum class PortType : uint8_t { Pulse, Bool, Number, String };

/// A single port value. `type` selects which of b/n/s is meaningful; Pulse uses none.
struct PortValue {
    PortType type = PortType::Pulse;
    bool b = false;
    double n = 0;
    std::string s;
};

/// One signal node instance: its stable id, node type name (one of the fourteen types listed in
/// docs/design/17-editor.md §17.6) and its literal parameter values.
struct NodeDesc {
    std::string id;
    std::string type;
    std::map<std::string, PortValue> params;
    /// Array-valued parameters (`lookup`'s `values`, T-123): a JSON array param lands here
    /// instead of in `params`, which only ever holds scalars. Every element of one array has to
    /// carry the same PortType (checked by FlowGraph::parse()).
    std::map<std::string, std::vector<PortValue>> arrayParams;
    // --- T-193 (docs/design/27-interaction.md §27.4): editor-only layout, ignored at run time. ---
    /// True when the `.xtflow` node carries `"pos": [x, y]` (the node canvas position).
    bool hasPos = false;
    /// Canvas position (scene units); meaningful only with `hasPos`.
    double posX = 0;
    double posY = 0;
};

/// T-193: one annotation box of the node canvas (`.xtflow` `groups[]`, tail addition). Pure editor
/// data: the runtime ignores it, and which nodes a box holds is decided by geometry.
struct GroupDesc {
    /// Caption shown on the box.
    std::string title;
    /// Box rectangle in canvas scene units.
    double x = 0;
    double y = 0;
    double w = 0;
    double h = 0;
    /// Editor theme token the box is tinted with ("" = the default).
    std::string color;
};

/// One edge: `from`/`to` are "<nodeId>.<port>" (a node's own port) or "#<elementId>.<port>" /
/// "#document.<port>" (a document connection point, docs/design/17-editor.md §17.6). `params`
/// carries edge-level parameters (e.g. `toggleClass`'s "class", `setProperty`'s "prop").
struct EdgeDesc {
    std::string from;
    std::string to;
    std::map<std::string, std::string> params;
};

/// The parsed flow graph: nodes/edges as literally declared in a `.xtflow` file, already
/// structurally validated (docs/tasks/T-56.md's rules that do not require a live Document: node
/// type/port names, param shapes, edge type matching, value-input fan-in, cycles). Binding to a
/// specific Document (id existence, `<anim>`-only ports) happens in FlowRuntime::load().
struct FlowGraph {
    std::vector<NodeDesc> nodes;
    std::vector<EdgeDesc> edges;
    /// T-193: the canvas annotation boxes (editor-only; see GroupDesc).
    std::vector<GroupDesc> groups;

    /// Parses `json` (a `.xtflow` document) into `out`. Validates: JSON well-formedness, the
    /// top-level "nodes"/"edges" array shape, each node having a known `type` and (for
    /// `constant`/`timer`/`math`/`compare`/`radio`/`lookup`) its required params, duplicate node ids, each
    /// edge's endpoint syntax and port names (for a "#id.port" endpoint, only the port name against the fixed
    /// §17.6 tables -- `doc` is not available here, so element existence / `<anim>`-only ports are NOT
    /// checked; see FlowRuntime::load()), edge type matching (`pulse` only to `pulse`; value types must match
    /// except any value -> `toString.in`/`format.a`..`format.d`; `select`'s own type is inferred from the
    /// edges reaching its `a`/`b`, T-123), value-input fan-in (at most one edge per value-typed input, except
    /// an element's `setStyle`, T-123), and cycles (Kahn's algorithm over node-to-node edges, ignoring an
    /// edge that is a node's pulse-to-pulse self-edge, T-123). Collects every problem found (not just the
    /// first) into `errors` and returns kInvalidArgument, leaving `out` untouched, when any check fails; kOk
    /// otherwise.
    ///
    /// T-193 (docs/design/27-interaction.md §27.4/27.4.1): endpoints may also be `ui#<uiId>.<element
    /// id>.<port>` / `ui#<uiId>.<port>` (another UI of the bundle, global flow only) and
    /// `ui#<uiId>.shown` / `ui#<uiId>.hidden` (UI lifecycle outputs); an edge may carry `instance`
    /// (a non-negative integer: only that instance's events / only that instance receives); the
    /// node types `system`, `sound`, `haptic`, `spawn`, `emit` and `timeline` exist; nodes may
    /// carry `pos` and the document `groups` (editor-only, never an error when malformed -- a
    /// malformed entry is dropped).
    static Status parse(std::string_view json, FlowGraph& out, std::vector<std::string>& errors);

    /// Serializes `this` back into a `.xtflow` JSON document: a top-level object with keys in the
    /// fixed order "version" (always 1), "nodes", "edges", then (T-193, only when non-empty)
    /// "groups"; each node as {id, type, [params], [pos]} and each edge as {from, to, [params]}
    /// ("params" omitted when empty, "pos" only with NodeDesc::hasPos), each group as
    /// {title, rect: [x, y, w, h], [color]}, 2-space indented. Always
    /// succeeds (an empty FlowGraph serializes to an empty nodes/edges document).
    /// `FlowGraph::parse(g.toJson(), out, errors)` reproduces a `g` equal to the original (same
    /// nodes/edges, in the same order; `errors` empty, kOk).
    std::string toJson() const;
};

/// One node port in nodeTypeTable(): its name and PortType (docs/design/17-editor.md §17.6's
/// signal node table).
struct NodeTypePort {
    std::string name;
    PortType type;
};

/// A node type's static shape -- type name, parameter names, and input/output ports -- mirroring
/// one row of docs/design/17-editor.md §17.6's signal node table (nodeTypeTable()'s per-entry
/// element).
struct NodeTypeDesc {
    std::string type;
    std::vector<std::string> params;
    std::vector<NodeTypePort> inputs;
    std::vector<NodeTypePort> outputs;
};

/// The fourteen §17.6 signal node types' static shape, in the table's own order (constant,
/// counter, toggle, timer, clock, random, math, compare, toString, select, radio, lookup, merge,
/// format), then T-193's six (system, sound, haptic, spawn, emit, timeline). Ports and their PortType are
/// obtained from the same nodePortInfo()/outputPortOrder() functions flow_bindings.cpp uses to
/// bind a live graph (flow_internal.h), not a separate copy of the port rules.
std::span<const NodeTypeDesc> nodeTypeTable();

}  // namespace xt::flow
