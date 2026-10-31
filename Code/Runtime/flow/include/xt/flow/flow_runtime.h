#pragma once

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xt/document.h"
#include "xt/flow/flow_graph.h"
#include "xt/types.h"

namespace xt {
class PerfTracker;
}  // namespace xt

/// FlowRuntime: binds a parsed FlowGraph's nodes to each other and to a Document's connection
/// points (Docs/design/17-editor.md §17.6), then ticks the whole thing per frame with
/// change-only value propagation. See Docs/tasks/T-56.md.

namespace xt::flow {

class BundleContext;

/// T-193 (Docs/design/27-interaction.md §27.4.1, 13-api.md §13.8.3): what a flow action node sends
/// to the host.
enum class FlowHostEventKind : uint8_t {
    Sound = 0,   ///< `sound` node: `name` is the cue, `volume` its volume.
    Haptic = 1,  ///< `haptic` node: `name` is the cue.
    Spawn = 2,   ///< `spawn` node: `name` is the symbol to spawn (the host decides where).
    Emit = 3,    ///< `emit` node: `name` is the port, `value` the last value its `value` input got.
};

/// T-193: one out event of a flow action node.
struct FlowHostEvent {
    FlowHostEventKind kind = FlowHostEventKind::Sound;
    /// The cue / symbol / port name (the node's `cue` / `symbol` / `port` param).
    std::string name;
    /// `sound` only: the node's `volume` param (default 1).
    float volume = 1;
    /// `emit` only: the value (Pulse when the `value` input never received one).
    PortValue value;
    /// The UI whose event started the propagation that reached the node (`ui#<id>` endpoints,
    /// lifecycle events, timeline events of a `ui` timeline node); empty for the Document of
    /// load() and for timers / system events.
    std::string uiId;
    /// That UI Document's instance id (Document::instanceId(), 27.3.1); 0 when not from a UI.
    uint32_t instanceId = 0;
    /// The sending node's id.
    std::string nodeId;
};

/// T-193: receives every FlowHostEvent of a FlowRuntime.
using FlowHostEventListener = std::function<void(const FlowHostEvent&)>;

/// T-193: the system event kinds postSystemEvent() accepts, in the `system` node's output order:
/// loading, paused, resumed, disconnected (pulses), languageChanged, platform, inputDevice
/// (strings), back (pulse, T-240), reducedMotion (string, T-330).
std::span<const std::string_view> systemEventKinds();

/// See the file-level comment above for scope.
class FlowRuntime {
public:
    FlowRuntime();
    ~FlowRuntime();
    FlowRuntime(const FlowRuntime&) = delete;
    FlowRuntime& operator=(const FlowRuntime&) = delete;

    /// Validates `graph` against `doc` (edge type-checking, value-input fan-in, cycle detection --
    /// the same rules as FlowGraph::parse(), redone here since a graph may be hand-built without
    /// going through parse() -- plus what only a live Document can answer: every "#id" exists,
    /// `<anim>`-only ports are only used on an actual `<anim>` element), then binds every node and
    /// connection point (Document::addListener for every document/`<anim>` output port used;
    /// direct calls -- setText, addClass, play, ... -- for every input port used). On ANY error,
    /// `errors` collects every problem found, this call returns kInvalidArgument and nothing is
    /// bound or runs (tick() is then a no-op). `doc` must outlive this FlowRuntime.
    /// T-260c: a failed load leaves the runtime empty, as unload() does (no Document, no listener
    /// on any Document, the previous graph gone too). Called from a callback this runtime is
    /// running (a FlowHostEvent listener, a Document or Context listener reached from its
    /// propagation), it takes effect once that dispatch returns: the current graph propagates
    /// nothing more meanwhile, kOk is returned and `errors` gets nothing (the load's own errors,
    /// if any, are dropped and leave the runtime empty); of several such calls only the last runs.
    Status load(const FlowGraph& graph, Document& doc, std::vector<std::string>& errors);

    /// 27 §27.4.3: one line per edge that drives a visual element input (setProperty, setStyle,
    /// playAnimation) from a continuous source. Computed on the bound graph (needs the endpoint
    /// kinds), appended to `out`; never fails the load. Format:
    /// "flow.animatesWithContinuousSignal: edge <from> -> <to> drives a visual property from a
    ///  continuous signal; author it as a timeline or a bind: instead"
    /// Recomputed by load() / rebind(); nothing is appended when load() has not last succeeded.
    void continuityWarnings(std::vector<std::string>& out) const;
    /// T-546: everything worth printing after a successful load() / rebind(), appended to `out`:
    /// continuityWarnings() first, then one `#id.setState: the document declares no state "<name>"`
    /// per distinct undeclared state an `#id.setState` edge names (single-Document mode; a global
    /// runtime has none). Nothing is appended when load() has not last succeeded.
    void loadWarnings(std::vector<std::string>& out) const;

    /// T-260c: takes every listener off the bound Document(s) (one the Context reloaded or unloaded
    /// meanwhile is not touched) and drops the graph: afterwards the runtime is as constructed,
    /// apart from setEnabled() / setHostEventListener(). Call it before unloading the Document a
    /// runtime is bound to when no new load() follows. Inside this runtime's own dispatch it is
    /// deferred like load().
    void unload();

    /// Re-resolves every binding against `doc` (a reloaded Document with the same or a changed
    /// tree, xt::Context::reloadDocument()): listeners are re-added, ports whose owner id no longer
    /// exists (or is no longer an `<anim>` element) are reported in `warnings` -- appended, one per
    /// distinct "#id.port", as "#id.port: element not found" / "#id.port: not an <anim> element" --
    /// and their edges are dropped; an `#id.setState` edge whose "state" param is neither a state `doc`
    /// declares nor a built-in alias (T-546) adds `#id.setState: the document declares no state
    /// "<name>"` (edge kept, runtime behavior unchanged; load() has no warnings output, so it does not
    /// check); node state (counter values, toggles, timers) is kept.
    /// Returns kInvalidArgument when load() has not last succeeded, kOk otherwise (missing ports
    /// are warnings, not errors).
    ///
    /// The Document load() bound to is never touched here: the usual caller reloaded it, so it is
    /// already destroyed. A host that instead keeps the old Document alive must stop dispatching
    /// input to it -- the listeners this runtime left on it are not removed.
    Status rebind(Document& doc, std::vector<std::string>& warnings);

    /// Advances every `clock`/`timer` node and every bound `<anim>` `localTime` output by `dt`
    /// seconds, then propagates the resulting value/pulse changes (Docs/tasks/T-56.md "執行規
    /// 則"). No-op if load() has not last succeeded.
    void tick(float dt);

    /// Current node output-port values, in node declaration order x each node type's own output-
    /// port order (Docs/tasks/T-56.md's table order), for panel display. Empty if load() has not
    /// last succeeded.
    std::span<const PortValue> values() const;
    /// "<nodeId>.<port>" names matching values(), same order. Empty if load() has not last
    /// succeeded.
    const std::vector<std::string>& valueNames() const;

    /// Feedback loops observed since the last load() (Docs/tasks/T-124.md), one entry per
    /// offending edge: "flow feedback loop: edge '<from>' -> '<to>' <reason>". A loop can only run
    /// through the document -- node-to-node cycles are rejected by load() -- so it is not a
    /// structural error the graph could be rejected for; the edge simply stops delivering for the
    /// rest of that tick and is reported here. Empty if load() has not last succeeded.
    const std::vector<std::string>& runtimeErrors() const;

    /// Per-edge "last carried a value" timestamps, in the loaded FlowGraph::edges order: the
    /// runtime time in seconds (the sum of every tick() `dt` since load()) at which that edge last
    /// delivered something to its sink, or -1 for an edge that never has. load()'s initial output
    /// seeding does not count as a delivery, so every entry is -1 right after a successful load().
    /// Empty if load() has not last succeeded.
    std::span<const float> edgeLastFired() const;

    // ---- T-193 (Docs/design/27-interaction.md §27.4/27.4.1, 13-api.md §13.8.3) ----------------

    /// Loads the bundle's global flow (`bundle.xtflow`): every document endpoint must be a
    /// `ui#<uiId>...` reference to a UI registered on `bundle` (each of its instances is bound;
    /// `#id.port` is an error here), `ui#<uiId>.shown/hidden` fire from `bundle`'s Context
    /// lifecycle callbacks, and a `timeline` node needs a `ui` param naming a registered UI.
    /// Validation and error reporting as load(). `bundle` (and every Document it lists) must
    /// outlive this runtime or the next load. The json overload parses first (FlowGraph::parse()).
    /// rebind() is not supported on a global runtime (kUnsupported): load it again instead.
    /// T-260c: failure, and a call from inside this runtime's own dispatch, as load().
    Status loadGlobal(std::string_view json, BundleContext& bundle, std::vector<std::string>& errors);
    Status loadGlobal(const FlowGraph& graph, BundleContext& bundle, std::vector<std::string>& errors);
    /// True after a successful loadGlobal() (until the next load()/loadGlobal()).
    bool isGlobal() const;

    /// Feeds a host system event into every `system` node: `kind` is one of systemEventKinds();
    /// `value` is the payload of the string kinds (ignored by the pulse kinds). A string output
    /// fires on every post, not only on change. kInvalidArgument for an unknown kind; a no-op
    /// (kOk) when nothing is loaded or the runtime is disabled.
    Status postSystemEvent(std::string_view kind, std::string_view value = {});

    /// The host's receiver for `sound` / `haptic` / `spawn` / `emit` node events (one listener;
    /// an empty function removes it). Kept across load().
    void setHostEventListener(FlowHostEventListener listener);

    /// 13.8.3 `enableFlow(bool)`: a disabled runtime keeps its bindings and node state but tick()
    /// does nothing and no output propagates (document events arriving meanwhile are dropped).
    /// Enabled by default; kept across load().
    void setEnabled(bool enabled);
    bool enabled() const;

    /// T-475 (Docs/design/21-perf-tracing.md §21.6): the tracker the flow stages
    /// (FrameStats::flowDeliverMs / flowEvalMs / flowSinkMs / flowPollMs) and counters
    /// (flowSignals .. flowAllocs) are written into, while it is enabled; nullptr (the default)
    /// records nothing. Usually the host Context's perf(); BundleContext sets its Context's on
    /// every runtime it owns. `tracker` must outlive this runtime or be replaced first. Kept
    /// across load().
    void setPerfTracker(PerfTracker* tracker);

private:
    friend class BundleContext;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace xt::flow
