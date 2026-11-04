#pragma once

#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xt/types.h"

/// xt_flow's data model: the parsed `.xtflow` document (nodes/edges/params), before any binding
/// to a live Document. See Docs/design/17-editor.md §17.6, Docs/tasks/T-56.md.

namespace xt::flow {

/// T-406 (Docs/design/43-compatibility-and-versions.md §43.2): the `.xtflow` format version toJson()
/// writes and the newest parse() accepts (a missing `version` reads as 1).
inline constexpr int kFlowFormatVersion = 1;

/// Signal port value type (Docs/design/17-editor.md §17.6). Pulse carries no value.
enum class PortType : uint8_t { Pulse, Bool, Number, String };

/// A single port value. `type` selects which of b/n/s is meaningful; Pulse uses none.
struct PortValue {
    PortType type = PortType::Pulse;
    bool b = false;
    double n = 0;
    std::string s;
};

/// T-541 (Docs/design/27-interaction.md §27.4.4): one `state` node transition,
/// `{ "from": "<state>" | "*", "on": "<event>", "to": "<state>" }`.
struct TransitionDesc {
    std::string from;
    std::string on;
    std::string to;
};

/// One signal node instance: its stable id, node type name (one of the fourteen types listed in
/// Docs/design/17-editor.md §17.6) and its literal parameter values.
struct NodeDesc {
    std::string id;
    std::string type;
    std::map<std::string, PortValue> params;
    /// Array-valued parameters (`lookup`'s `values`, T-123): a JSON array param lands here
    /// instead of in `params`, which only ever holds scalars. Every element of one array has to
    /// carry the same PortType (checked by FlowGraph::parse()).
    std::map<std::string, std::vector<PortValue>> arrayParams;
    /// T-541: a `state` node's `transitions` param (an array of objects, which neither `params` nor
    /// `arrayParams` can hold). Its `states` / `events` are string arrays in `arrayParams`.
    std::vector<TransitionDesc> transitions;
    // --- T-193 (Docs/design/27-interaction.md §27.4): editor-only layout, ignored at run time. ---
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
/// "#document.<port>" (a document connection point, Docs/design/17-editor.md §17.6). `params`
/// carries edge-level parameters (e.g. `toggleClass`'s "class", `setProperty`'s "prop").
struct EdgeDesc {
    std::string from;
    std::string to;
    std::map<std::string, std::string> params;
};

/// T-580 (Docs/design/53-flow-language.md §53.101, 22-bundle.md §22.15): one document port a flow
/// touches, as an edge endpoint names it. `uiId` is the `ui#<uiId>` of a global-flow endpoint (empty
/// for a UI's own flow); `owner` is "document" for `#document.<port>` / `ui#<uiId>.<port>`, else the
/// element id; `port` is the declared port name.
struct FlowPortRef {
    std::string uiId;
    std::string owner;
    std::string port;
};

/// T-580: one `emit` node's host event. `type` is the type of the edge into its `value` input
/// (Pulse when nothing is connected). When that type cannot be worked out without a Document (the
/// value comes from a declared port), `typeKnown` is false and `typeSource` names the port.
struct FlowEmitInfo {
    std::string name;
    PortType type = PortType::Pulse;
    bool typeKnown = true;
    FlowPortRef typeSource;
};

/// T-580 (53 §53.101): the host-facing interface of a graph -- `emit` events (one per `port` name,
/// first node wins, in node order), declared ports the graph writes (edge targets) and reads (edge
/// sources), each once in edge order, and the `system` outputs that have an edge, in the
/// §53.4.18 output order. Element ports, `#document` row-3 ports, `<anim>` ports and lifecycle
/// outputs are not part of it.
struct FlowHostInterface {
    std::vector<FlowEmitInfo> emits;
    std::vector<FlowPortRef> writes;
    std::vector<FlowPortRef> reads;
    std::vector<std::string> system;
};

/// The parsed flow graph: nodes/edges as literally declared in a `.xtflow` file, already
/// structurally validated (Docs/tasks/T-56.md's rules that do not require a live Document: node
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
    /// T-193 (Docs/design/27-interaction.md §27.4/27.4.1): endpoints may also be `ui#<uiId>.<element
    /// id>.<port>` / `ui#<uiId>.<port>` (another UI of the bundle, global flow only) and
    /// `ui#<uiId>.shown` / `ui#<uiId>.hidden` (UI lifecycle outputs); an edge may carry `instance`
    /// (a non-negative integer: only that instance's events / only that instance receives); the
    /// node types `system`, `sound`, `haptic`, `spawn`, `emit` and `timeline` exist; nodes may
    /// carry `pos` and the document `groups` (editor-only, never an error when malformed -- a
    /// malformed entry is dropped).
    ///
    /// T-406 (43 §43.2): `version` -- missing = 1; not a positive integer -> kInvalidArgument; newer
    /// than kFlowFormatVersion -> kUnsupported with ".xtflow format version N is newer than this
    /// XTFrame supports (M); a newer XTFrame is needed" (nothing else is checked then).
    static Status parse(std::string_view json, FlowGraph& out, std::vector<std::string>& errors);

    /// Serializes `this` back into a `.xtflow` JSON document: a top-level object with keys in the
    /// fixed order "version" (always kFlowFormatVersion), "nodes", "edges", then (T-193, only when non-empty)
    /// "groups"; each node as {id, type, [params], [pos]} and each edge as {from, to, [params]}
    /// ("params" omitted when empty, "pos" only with NodeDesc::hasPos), each group as
    /// {title, rect: [x, y, w, h], [color]}, 2-space indented. Always
    /// succeeds (an empty FlowGraph serializes to an empty nodes/edges document).
    /// `FlowGraph::parse(g.toJson(), out, errors)` reproduces a `g` equal to the original (same
    /// nodes/edges, in the same order; `errors` empty, kOk).
    std::string toJson() const;

    /// T-580: reflects the graph's host interface (FlowHostInterface) without a Document; read-only,
    /// the graph is not changed. Endpoints are resolved as parse() resolves them, so call it on a
    /// graph parse() accepted; returns kInvalidArgument (and leaves `out` empty) when the graph
    /// does not validate.
    Status hostInterface(FlowHostInterface& out) const;
};

/// One node port in nodeTypeTable(): its name and PortType (Docs/design/17-editor.md §17.6's
/// signal node table).
struct NodeTypePort {
    std::string name;
    PortType type;
};

/// A node type's static shape -- type name, parameter names, and input/output ports -- mirroring
/// one row of Docs/design/17-editor.md §17.6's signal node table (nodeTypeTable()'s per-entry
/// element).
struct NodeTypeDesc {
    std::string type;
    std::vector<std::string> params;
    std::vector<NodeTypePort> inputs;
    std::vector<NodeTypePort> outputs;
};

/// The fourteen §17.6 signal node types' static shape, in the table's own order (constant,
/// counter, toggle, timer, clock, random, math, compare, toString, select, radio, lookup, merge,
/// format), then T-193's six (system, sound, haptic, spawn, emit, timeline), then T-541's `state`
/// (only its fixed ports: its event / enter / exit ports depend on the node's own params). Ports and their
/// PortType are obtained from the same nodePortInfo()/outputPortOrder() functions flow_bindings.cpp uses to
/// bind a live graph (flow_internal.h), not a separate copy of the port rules.
std::span<const NodeTypeDesc> nodeTypeTable();

}  // namespace xt::flow
