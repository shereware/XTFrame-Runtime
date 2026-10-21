#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "xt/document.h"
#include "xt/flow/flow_graph.h"
#include "xt/flow/flow_runtime.h"

namespace xt {
class Context;
}  // namespace xt

/// Private, shared implementation details for xt_flow, split across flow_graph.cpp (structural
/// validation, used without a Document), flow_bindings.cpp (the shared validator plus Document/
/// `<anim>` connection-point binding, and the document-endpoint layer shared with HostPorts),
/// flow_nodes.cpp (the ten node types' runtime behavior) and
/// flow_runtime.cpp (FlowRuntime::Impl's public-facing load/tick/values) and host_ports.cpp
/// (HostPorts, docs/tasks/T-144.md). See docs/tasks/T-56.md.

namespace xt::flow {

/// One of the fourteen signal node types (docs/design/17-editor.md §17.6's table), parsed from
/// NodeDesc::type. Radio/Lookup/Merge/Format are T-123's additions; System .. Timeline are T-193's
/// (docs/design/27-interaction.md §27.4.1).
enum class NodeType : uint8_t {
    Constant,
    Counter,
    Toggle,
    Timer,
    Clock,
    Random,
    Math,
    Compare,
    ToString,
    Select,
    Radio,
    Lookup,
    Merge,
    Format,
    System,
    Sound,
    Haptic,
    Spawn,
    Emit,
    Timeline
};

/// Parses `name` (NodeDesc::type) into a NodeType. Returns false for an unknown name.
bool parseNodeType(std::string_view name, NodeType& out);

/// T-258a: the shortest timer `interval`, seconds. FlowRuntime::tick() runs an interval in
/// (0, kMinTimerInterval) at it (FlowGraph::parse() keeps the authored value, T-258b); 0, a negative
/// or a non-finite interval keeps meaning "never fires".
inline constexpr double kMinTimerInterval = 0.001;

/// T-258a: `interval` as a timer runs it: raised to kMinTimerInterval when in (0, kMinTimerInterval),
/// otherwise unchanged.
inline double effectiveTimerInterval(double interval) {
    return interval > 0.0 && interval < kMinTimerInterval ? kMinTimerInterval : interval;
}
/// T-258a: the most `tick` pulses one timer fires in one FlowRuntime::tick(); past it the timer's
/// elapsed time is dropped.
inline constexpr int kMaxTimerFiresPerTick = 1000;

/// A default-valued PortValue of type `t`. Used instead of a positional `PortValue{t}` brace-init
/// (which leaves `s` at its in-class default), since that trips Clang's -Wmissing-field-
/// initializers -- see docs/design/14-coding-standards.md §14.1's designated-initializer note;
/// this is the same underlying issue for a positional aggregate init.
inline PortValue typedValue(PortType t) {
    PortValue v;
    v.type = t;
    return v;
}

/// A resolved edge endpoint: either a node's own port, or a document connection point (a plain
/// element, an `<anim>` element, or the document itself, docs/design/17-editor.md §17.6).
struct Endpoint {
    /// T-175: Port = a declared port (`port:` in the `.xt`, the Document's PORT table).
    /// T-193: Lifecycle = `ui#<uiId>.shown` / `ui#<uiId>.hidden` (outputs only; `port` names which).
    enum class Owner : uint8_t { Node, Element, Anim, Doc, Port, Lifecycle } owner = Owner::Node;
    int nodeIndex = -1;                      ///< Owner::Node: index into FlowRuntime::Impl::nodes.
    NodeType nodeType = NodeType::Constant;  ///< Owner::Node: that node's type (for flexible ports).
    std::string elementId;                   ///< Owner::Element/Owner::Anim ("#<elementId>"); empty for Doc.
    std::string port;
    PortType type = PortType::Pulse;
    /// T-175: a declared port resolved without a Document (FlowGraph::parse()): its type is not
    /// known yet, so edges to or from it are type-checked only when a Document is bound.
    bool anyType = false;
    /// T-193: `ui#<uiId>` endpoints: the UI whose Document(s) this endpoint lives in; empty for
    /// node ports and for the single Document of FlowRuntime::load().
    std::string uiId{};
};

/// A validated, resolved edge (FlowGraph::EdgeDesc after endpoint resolution).
struct BoundEdge {
    Endpoint from;
    Endpoint to;
    /// The edge's single relevant string param, if any: `class` for add/remove/toggleClass,
    /// `prop` for setProperty. Empty otherwise.
    std::string param;
    /// T-193: the edge's `instance` param (27.3.1 multi-instance), -1 when absent: a document
    /// source only fires from that instance, a document sink only reaches that instance.
    int64_t instance = -1;
};

// ---- Fixed §17.6 port tables (no Document needed) --------------------------------------------

/// Node port lookup: true if `type`'s node has a port named `port` in the requested direction
/// (isSource: output vs input), and if so, its PortType (see `node`'s own params for `constant`,
/// whose "out" mirrors its "value" param's type).
bool nodePortInfo(NodeType type, const NodeDesc& node, const std::string& port, bool isSource,
                  PortType& outType);
/// `radio`'s `count` param (docs/design/17-editor.md §17.6): how many `set<i>` pulse inputs the
/// node has. Defaults to 2 (the smallest legal count) when the param is missing or out of the
/// 2..64 range -- FlowGraph::parse() rejects both, so only nodeTypeTable()'s param-less probe
/// node and a hand-built graph ever see the default.
uint32_t radioCount(const NodeDesc& node);
/// `lookup`'s `values` array element type (docs/design/17-editor.md §17.6), i.e. its `out` port's
/// type. PortType::Number for an absent/empty array (FlowGraph::parse() rejects both).
PortType lookupValueType(const NodeDesc& node);
/// True if `port` is a §17.6 row-1 ("有 id 的元素") port in the requested direction.
bool elementPortInfo(const std::string& port, bool isSource, PortType& outType);
/// True if `port` is a §17.6 row-2 ("<anim> 元素") port in the requested direction.
bool animPortInfo(const std::string& port, bool isSource, PortType& outType);
/// True if `port` is a §17.6 row-3 ("文件") port (output-only).
bool docPortInfo(const std::string& port, PortType& outType);
/// T-175 (docs/design/27-interaction.md §27.4): the flow PortType of a declared port's value
/// type -- number/entity -> Number, bool -> Bool, event -> Pulse, every other scalar -> String
/// (its text form). False for struct/list ports, which are listed but not connectable yet.
bool declaredPortFlowType(PortValueType type, PortType& out);
/// True for the five pointer-derived row-3 ports (T-77): pointerX/Y, pointerNX/NY, pointerDown.
bool isPointerDocPort(const std::string& port);
/// T-389 (27 §27.4.1): true for a continuous value output -- every `#document.*` port, an
/// element's `hover`, an `<anim>`'s `localTime` and a declared non-event port -- which the flow
/// coalesces per frame; false for the pulse / event outputs (Click .. AnimationEnd, Change,
/// Marker, Finished, event ports), delivered one by one.
bool isValueSignal(Endpoint::Owner owner, const std::string& port, PortType type);
/// A node type's output ports, in docs/tasks/T-56.md table order (used by values()/valueNames()).
std::vector<std::string> outputPortOrder(NodeType type);

/// Shared validator (docs/tasks/T-56.md "規則"): edge endpoint resolution, type matching
/// (`toString.in`/`compare.a`/`compare.b`/`select.a`/`select.b` accept any non-pulse type), value-
/// input fan-in (<=1 edge), edge params (`class`/`prop`), and cycle detection over node-to-node
/// edges (Kahn's algorithm). `doc` may be nullptr (FlowGraph::parse(): "#id" existence and
/// `<anim>`-only ports are not checked then); when non-null (FlowRuntime::load()), every "#id"
/// must resolve and row-2 ports must target an actual `<anim>` element. Collects every problem
/// into `errors`; returns kInvalidArgument if any, else kOk and (if `outEdges` is non-null) the
/// resolved edges in `graph.edges`' order.
///
/// T-193: `uiDocs` non-null selects the global flow: `ui#<uiId>` endpoints resolve against
/// `(*uiDocs)[uiId]` (a missing id is an error), `#id.port` endpoints and `timeline` nodes without
/// a known `ui` param are errors. With `uiDocs` null and `doc` non-null (a UI's own flow), any
/// `ui#` endpoint and any `timeline.ui` param is an error. With both null (FlowGraph::parse()),
/// both spellings are accepted without a Document.
Status validateGraph(const FlowGraph& graph, Document* doc, std::vector<std::string>& errors,
                     std::vector<BoundEdge>* outEdges,
                     const std::map<std::string, Document*>* uiDocs = nullptr);

/// T-193: splits `ui#<uiId>.<rest>`: `uiId`, and `rest` rewritten as a single-UI endpoint --
/// "#<element>.<port>" for three segments, "#document.<port>" for two, or the bare "shown" /
/// "hidden" for the lifecycle outputs (`lifecycle` set). False when `text` is not a `ui#`
/// endpoint or is malformed (then `err` says why; empty for "not a ui# endpoint").
bool splitUiEndpoint(const std::string& text, std::string& uiId, std::string& rest, bool& lifecycle,
                     std::string& err);

// ---- Document endpoint layer (shared by FlowRuntime and HostPorts, docs/tasks/T-144.md) ------

/// Everything one consumer (a FlowRuntime or a HostPorts) keeps about the document connection
/// points it drives or listens to. The free functions below are the single implementation of
/// "send a value into a document input port" and "observe a document output port"; FlowRuntime
/// routes what they report through its edges, HostPorts through the host's listeners.
struct DocEndpointState {
    /// Receives every output a subscribed port produces, after emitDocumentOutput()'s change-only
    /// filter (a value port only reports a value that differs from the last one it reported).
    using OutputFn = std::function<void(Endpoint::Owner owner, const std::string& elementId,
                                        const std::string& port, const PortValue& v)>;

    Document* doc = nullptr;
    /// T-260b: where `doc` came from when a Context owns it (BundleContext's runtimes). Every use
    /// of `doc` goes through liveDocument(), which looks `handle` up again at that moment: a
    /// reloaded (another Document) or unloaded (none) one counts as gone. Null for a runtime the
    /// host loaded against a Document directly (FlowRuntime::load()'s contract: it outlives us).
    Context* context = nullptr;
    DocumentHandle handle = 0;
    /// T-260c: `doc`'s Document::loadSerial(), set together with `doc`. liveDocument() compares it
    /// with Context::documentSerial(handle), never the addresses: a Document installed later at
    /// the same handle may live at the old one's address.
    uint32_t serial = 0;
    OutputFn output;  ///< Set once by the owner; must stay callable while listeners are bound.

    // Keyed by element id.
    std::map<std::string, ElementRef> elementRefs;
    std::map<std::string, uint32_t> animInstance;  // <anim> element id -> current Animator instance id.
    std::map<std::string, PortValue> lastOutputs;  // externalKey() -> last reported value.
    std::vector<std::string> docOutputPorts;       // Distinct "#document.<port>" outputs subscribed, polled.
    std::vector<std::string> animLocalTimeElements;  // Distinct <anim> ids with "localTime" subscribed.
    std::set<std::string> subscribed;  // externalKey()s subscribeDocumentOutput() already handled.
    bool pointerSubscribed = false;    // The document-root pointer listeners are registered.
    float clockAccum = 0;              // "#document.clock": the sum of every tick() `dt`.

    // Pointer connection-point state (T-77), fed by the document-root PointerMove/Down/Up
    // listeners and read back by documentPortValue().
    Vec2 pointerPos{};         // Last observed position, viewport logical px; only once pointerSeen.
    bool pointerSeen = false;  // False until the first pointer event (position reads as the viewport center).
    bool pointerDown = false;  // True while a pointer button is held (any button).

    std::vector<uint32_t> listenerIds;      // Document::addListener ids, see unbindDocumentEndpoints().
    std::vector<uint32_t> portListenerIds;  // T-175: Document::addPortListener ids, likewise.
    /// Liveness token for the Animator "finished" callbacks, which Document offers no way to
    /// unregister: each callback holds a weak_ptr to it and does nothing once it has expired.
    std::shared_ptr<bool> aliveToken = std::make_shared<bool>(true);
};

/// T-260b: `state.doc` while it is still the Document the Context holds at `state.handle` (always
/// `state.doc` when `state.context` is null), else nullptr. Resolved at the moment of use, never
/// cached across a dispatch (a reload's install is deferred to the end of one, T-260a). T-260c:
/// "still the Document" is Context::documentSerial(state.handle) == state.serial.
Document* liveDocument(const DocEndpointState& state);
/// T-407 (43 §43.3): true when `state`'s Document is still live and its behavior version is at
/// least kBehaviorFlowSignalCoalescing -- only then are its value signals coalesced per frame;
/// below it (or with no live Document) every output is delivered at once, as before T-389.
bool coalescesSignals(const DocEndpointState& state);

/// "E:<id>:<port>" / "A:<id>:<port>" / "D::<port>": one output port's identity.
std::string externalKey(Endpoint::Owner owner, const std::string& elementId, const std::string& port);

/// Resolves the connection point "#<id>.<port>" (`id` == "document" for the row-3 ports) against
/// the §17.6 tables in the requested direction. `doc` may be nullptr (then "#id" existence and
/// `<anim>`-only ports are not checked). On failure, `err` says why.
bool resolveDocumentEndpoint(const std::string& id, const std::string& port, bool isSource, Document* doc,
                             Endpoint& out, std::string& err);

/// The edge parameter a document input port takes ("class" for add/remove/toggleClass, "prop"
/// for setProperty), or nullptr.
const char* documentInputParamName(const std::string& port);

/// Checks the edge parameter of document input port `port`: `param` is its value (nullptr when
/// absent). `subject` starts the error message ("edge to '#a.addClass'"). True when valid or when
/// the port takes no parameter.
bool checkDocumentInputParam(const std::string& port, const std::string* param, const std::string& subject,
                             std::string& err);

/// True when `ep` names an element / `<anim>` connection point `document` can no longer offer,
/// appending one warning per distinct "#id.port" ("#id.port: element not found" / "#id.port: not
/// an <anim> element"); `warned` tracks the ones already reported. Document ports never are.
bool documentEndpointLost(Document& document, const Endpoint& ep, std::vector<std::string>& warnings,
                          std::set<std::string>& warned);

/// Caches `ep`'s ElementRef (element / `<anim>` endpoints only) and, for an `<anim>` element, its
/// default Animator instance plus that instance's "Finished" callback.
void registerDocumentEndpoint(DocEndpointState& state, const Endpoint& ep);

/// Sends `v` into the element / `<anim>` input port `sink` (registerDocumentEndpoint() first).
/// `param` is the edge parameter (see documentInputParamName()). Type checks are the caller's.
void applyDocumentInput(DocEndpointState& state, const Endpoint& sink, const PortValue& v,
                        const std::string& param);

/// Starts observing the output port `source` (registerDocumentEndpoint() first for an element /
/// `<anim>` one): Document listeners for the event-driven ports (reported as the Document
/// dispatches), the polled lists for "#document.*" and `<anim>` "localTime" (reported by
/// pollDocumentOutputs()). Idempotent per port.
void subscribeDocumentOutput(DocEndpointState& state, const Endpoint& source);

/// Reports every subscribed polled port's current value (change-only).
void pollDocumentOutputs(DocEndpointState& state);

/// The current value of one "#document.<port>" output port.
PortValue documentPortValue(const DocEndpointState& state, const std::string& port);

/// Change-only filter plus dispatch to `state.output`: a pulse always goes through, a value only
/// when it differs from the last one reported for the same port.
void emitDocumentOutput(DocEndpointState& state, Endpoint::Owner owner, const std::string& elementId,
                        const std::string& port, const PortValue& v, bool isPulse);

/// Drops every Document listener subscribeDocumentOutput() registered -- removing them from
/// `state.doc` only when `removeListeners` (false when that Document is already destroyed) -- and
/// silences pending "finished" callbacks. Clears the subscription bookkeeping and the polled
/// lists; keeps the tree state (see resetDocumentTreeState()) and the clock.
void unbindDocumentEndpoints(DocEndpointState& state, bool removeListeners);

/// T-260b (BundleContext::removeUi): removes `state`'s listeners from its Document while that is
/// still live (liveDocument()), drops the ids, silences the "finished" callbacks and detaches it
/// (`doc` null, so every use skips it). The subscription lists and cached refs stay as they are:
/// a loop over them (pollDocumentOutputs()) or a callback of this state may be running further
/// up the stack.
void detachDocumentEndpoints(DocEndpointState& state);

/// Forgets everything derived from the bound tree: cached refs, `<anim>` instances, last
/// reported values and the pointer state. Keeps the clock.
void resetDocumentTreeState(DocEndpointState& state);

// ---- Runtime node state ------------------------------------------------------------------------

/// One node's live state: its declared params plus mutable behavior state and cached port values.
struct NodeRuntime {
    NodeType type = NodeType::Constant;
    std::string id;
    std::map<std::string, PortValue> params;       // From NodeDesc::params.
    std::vector<PortValue> values;                 // `lookup`: from NodeDesc::arrayParams["values"].
    std::string uiId;                              // T-193 `timeline`: its `ui` param (global flow).
    uint32_t radioCount = 2;                       // `radio`: its `count` param.
    double radioIndex = 0;                         // `radio`: its current index.
    std::map<std::string, PortValue> inputValues;  // Current cached value-typed input values.
    std::map<std::string, PortValue>
        outputValues;  // Current output values (see FlowRuntime::Impl::pushRawOutput).
    double counterValue = 0;
    bool toggleValue = false;
    bool timerRunning = false;
    float timerElapsed = 0;
    double timerCount = 0;
    bool compareLastValid = false;
    bool compareLastResult = false;
    std::mt19937 rng{std::random_device{}()};
};

/// Reads a numeric node param, defaulting to `def` when absent or not a Number/Bool.
double paramNum(const NodeRuntime& n, const std::string& name, double def);
/// Reads a boolean node param, defaulting to `def` when absent or not a Bool.
bool paramBool(const NodeRuntime& n, const std::string& name, bool def);
/// T-193: the param `type` requires as a string (`sound.cue`, `haptic.cue`, `spawn.symbol`,
/// `emit.port`, `timeline.name`); nullptr when the type has none.
const char* requiredStringParam(NodeType type);

/// Reads a string node param, defaulting to `def` when absent.
std::string paramStr(const NodeRuntime& n, const std::string& name, const std::string& def);

/// Exact comparison (docs/tasks/T-56.md "值型埠：只在值改變時推送（浮點比較 exact）").
bool valuesEqual(const PortValue& a, const PortValue& b);
/// Two-value comparison per `compare`'s `op` (docs/tasks/T-56.md's table); Bool operands compare
/// as 0/1 for the ordering ops. Unrecognized `op` returns false.
bool compareValues(const PortValue& a, const PortValue& b, const std::string& op);
/// `toString`'s value formatting: Bool -> "true"/"false", String passed through, Number ->
/// `%.*f` when `params` has "decimals", else the shortest `%g` representation.
std::string formatPortValue(const PortValue& v, const std::map<std::string, PortValue>& params);
/// Replaces the first "{}" in `format` with `value`; returns `format` unchanged if it has none.
std::string substitutePlaceholder(const std::string& format, const std::string& value);
/// `format`'s named placeholders (T-123): replaces every "{a}".."{d}" in `format` with
/// `values[0]`..`values[3]`. A placeholder whose input has never arrived gets the empty string
/// (the caller passes one for it); any other brace text is left alone.
std::string substituteNamedPlaceholders(const std::string& format, const std::string (&values)[4]);

}  // namespace xt::flow

/// FlowRuntime::Impl: the whole bound, running graph. See the file-level comment above.
struct xt::flow::FlowRuntime::Impl {
    Impl();

    /// The bound Document and every connection point the graph uses (doc, cached refs, polled
    /// ports, pointer state, Document listeners, the runtime clock). Shared code with HostPorts.
    DocEndpointState docs;
    bool loaded = false;

    // ---- T-193: global flow (loadGlobal), actions, system events -------------------------------
    /// One registered UI instance of the global flow: its UI id and endpoint state (stable address:
    /// the Document listeners capture it).
    struct UiDoc {
        std::string uiId;
        DocumentHandle handle = 0;
        std::unique_ptr<DocEndpointState> state;
    };
    std::vector<UiDoc> uiDocs;
    /// True after a successful loadGlobal().
    bool global = false;
    /// FlowRuntime::setEnabled().
    bool enabled = true;
    FlowHostEventListener hostListener;
    /// The UI / instance of the external event currently propagating (FlowHostEvent::uiId /
    /// instanceId, and the source side of an edge's `instance` filter).
    std::string currentUi;
    uint32_t currentInstance = 0;
    /// Timeline names already reported as missing (one runtime error each).
    std::set<std::string> missingTimelines;

    /// The endpoint states `uiId` addresses: the load() Document for "" (when bound), else every
    /// registered instance of that UI.
    std::vector<DocEndpointState*> statesFor(const std::string& uiId);
    /// Fans out an external output (document port, lifecycle) of `state` under the UI / instance
    /// context it came from.
    void externalOutput(const std::string& uiId, DocEndpointState* state, Endpoint::Owner owner,
                        const std::string& elementId, const std::string& port, const PortValue& v);
    /// ContextListener::onShown/onHidden of a registered handle.
    void lifecycleEvent(const Context* context, DocumentHandle handle, bool shown);
    Status postSystemEvent(std::string_view kind, std::string_view value);
    /// Sends one FlowHostEvent from node `nodeIndex`.
    void sendHostEvent(FlowHostEventKind kind, int nodeIndex, const std::string& name, const PortValue& value,
                       float volume);
    /// `timeline` node inputs.
    void timelineInput(int nodeIndex, const std::string& port, const PortValue& v);
    /// Registers the `timeline` nodes' TimelineEnd / TimelineMarker listeners.
    void bindTimelineNodes();
    /// Stores and fans out an output that fires on every event (system strings, `timeline.marker`).
    void fireEventOutput(int nodeIndex, const std::string& port, const PortValue& v);
    /// Shared part of load()/loadGlobal(): builds the node array and value slots and validates
    /// (`uiMap` as validateGraph()). Expects the endpoint states to be reset already.
    Status loadGraph(const FlowGraph& graph, std::vector<std::string>& errors,
                     const std::map<std::string, Document*>* uiMap);
    /// Unbinds every endpoint state (removing listeners only when `removeListeners`).
    void unbindAll(bool removeListeners);
    /// T-260b (BundleContext::removeUi): unbinds every instance of `uiId` and detaches it (no
    /// Document, no UI id, no handle). The endpoint states stay allocated until the next load: a
    /// callback of theirs may be running further up the stack.
    void removeUi(std::string_view uiId);
    // True only inside seedInitialOutputs(): see its call in load() and evaluateNode()'s Compare
    // case for why `compare`'s true/false pulses are suppressed while this is set.
    bool seeding = false;

    std::vector<NodeRuntime> nodes;
    std::map<std::string, int> nodeIndexById;
    std::vector<BoundEdge> edges;

    std::vector<float> edgeLastFiredList;  // FlowRuntime::edgeLastFired(), in `edges` order.

    // ---- T-389: value signals coalesced per frame (docs/design/27-interaction.md §27.4.1) -------
    //
    // A value-type document output (isValueSignal()) is not fanned out while the Document
    // dispatches input: signalOutput() keeps its latest value here, keyed by (endpoint state,
    // externalKey()), and tick() delivers every pending one once, as one batch. Pulse / event
    // outputs still go through at once (after the pending values, so they see them).
    struct PendingSignal {
        DocEndpointState* state = nullptr;  ///< `docs` or a UiDoc's state.
        std::string uiId;                   ///< externalOutput()'s UI; "" for the load() Document.
        Endpoint::Owner owner = Endpoint::Owner::Doc;
        std::string elementId;
        std::string port;
        PortValue value;
    };
    std::vector<PendingSignal> pendingSignals;
    std::map<std::pair<const DocEndpointState*, std::string>, size_t> pendingSignalIndex;
    /// True while a batch is being delivered (tick(), flushSignals()): a value input of a
    /// recomputed node (math, compare, toString, select, lookup, format) only marks the node dirty;
    /// settleNodes() then evaluates every dirty node once, in topological order.
    bool batching = false;
    std::vector<int> topoOrder;      ///< Node indices, sources first (node-to-node edges).
    std::vector<uint8_t> nodeDirty;  ///< Per node: a value input changed during the batch.
    size_t dirtyNodes = 0;
    /// Nodes whose outputs were pushed since valueList was last refreshed (syncValueList()).
    std::vector<int> staleNodes;
    std::vector<uint8_t> nodeStale;
    /// Marks `nodeIndex`'s outputs for syncValueList().
    void markValuesStale(int nodeIndex);
    /// T-407 (43 §43.3): true when any bound Document (`docs` or a global flow's UI) coalesces
    /// (coalescesSignals()); tick() then runs as one batch. False: every push is evaluated at
    /// once, the pre-T-389 tick.
    bool anyCoalescing() const;

    /// Keeps the latest value of one value signal for the next tick().
    void queueSignal(const std::string& uiId, DocEndpointState* state, Endpoint::Owner owner,
                     const std::string& elementId, const std::string& port, const PortValue& v);
    /// The entry point of every output a global flow's UI reports (already change-filtered): a
    /// value signal is queued, anything else is delivered at once (the load() Document's lambda
    /// does the same inline).
    void signalOutput(const std::string& uiId, DocEndpointState* state, Endpoint::Owner owner,
                      const std::string& elementId, const std::string& port, const PortValue& v);
    /// Fans one document output out along its edges (the pre-T-389 immediate path).
    void deliverExternal(const std::string& uiId, DocEndpointState* state, Endpoint::Owner owner,
                         const std::string& elementId, const std::string& port, const PortValue& v);
    /// Delivers every pending value signal, then evaluates the dirty nodes (rounds bounded: a
    /// signal raised again by a sink in the same round waits for the next one).
    void flushSignals();
    /// Delivers the pending value signals once, without evaluating (tick()'s first step).
    void deliverPendingSignals();
    /// Evaluates every dirty node once, in topological order (rounds bounded).
    void settleNodes();
    /// Marks `nodeIndex` for settleNodes().
    void markNodeDirty(int nodeIndex);
    /// Copies every node's current outputs into valueList (values() reads it).
    void syncValueList();

    // ---- Fan-out index and feedback guard (docs/tasks/T-124.md) --------------------------------
    //
    // fanOut() used to scan every edge on every push, which is O(edges) per push and O(nodes x
    // edges) for one load()'s seeding pass: the 196-node / 352-edge field repro spent most of a
    // FlowRuntime::load() there. These two maps hold the edges leaving each source port instead.
    /// Edges leaving a node's output port, keyed by (node index, port name).
    std::map<std::pair<int, std::string>, std::vector<uint32_t>> nodeFanOut;
    /// Edges leaving a document / element / `<anim>` output port, keyed by externalKey().
    std::map<std::string, std::vector<uint32_t>> externalFanOut;
    /// How often each edge has fired inside the current tick window; reset by tick(). An edge past
    /// kMaxEdgeFiresPerTick stops delivering and is reported once as a feedback loop.
    std::vector<uint32_t> edgeFireCount;
    /// Edges already reported as a feedback loop (one report per edge per load()).
    std::vector<bool> edgeLoopReported;
    /// Current fanOut() nesting depth, capped at kMaxFanOutDepth.
    uint32_t fanOutDepth = 0;

    // ---- T-260c: dispatch depth, deferred loads ------------------------------------------------
    /// A load(), loadGlobal() or unload() asked for while this runtime is dispatching, run once
    /// the outermost dispatching call returns (runPending()); only the last one asked for is kept.
    struct PendingLoad {
        enum class Kind : uint8_t { None, Load, LoadGlobal, Unload };
        Kind kind = Kind::None;
        FlowGraph graph;
        Document* doc = nullptr;  ///< Load: the Document.
        uint32_t serial = 0;      ///< Load: its loadSerial(), checked again when `docs.context` is set.
        BundleContext* bundle = nullptr;  ///< LoadGlobal.
    };
    PendingLoad pending;
    /// How many of this runtime's dispatching calls are on the stack: tick(), postSystemEvent(),
    /// load() / loadGlobal() (their initial outputs) and fanOut(), which every document, timeline
    /// and lifecycle event goes through. Counted by DispatchGuard.
    uint32_t dispatchDepth = 0;
    /// True inside runPending(), whose loads are dispatching calls of their own.
    bool runningPending = false;
    struct DispatchGuard {
        Impl& impl;
        explicit DispatchGuard(Impl& owner) : impl(owner) { ++impl.dispatchDepth; }
        ~DispatchGuard() {
            if (--impl.dispatchDepth == 0 && impl.pending.kind != PendingLoad::Kind::None &&
                !impl.runningPending) {
                impl.runPending();
            }
        }
        DispatchGuard(const DispatchGuard&) = delete;
        DispatchGuard& operator=(const DispatchGuard&) = delete;
    };
    /// True while a deferred load / unload waits: the current graph is on its way out, so nothing
    /// more propagates through it (fanOut() and sendHostEvent() stop), as an unloading Document
    /// goes silent (T-260a).
    bool replacing() const { return pending.kind != PendingLoad::Kind::None; }
    /// Runs the deferred load / unload (and any one asked for while it runs).
    void runPending();
    /// FlowRuntime::unload(), also what a failed load() / loadGlobal() leaves: every listener off
    /// the Documents, no Document, no UI, no graph.
    void clearAll();
    /// The graph half of clearAll() (nodes, edges, values, guard state); loadGraph() starts with it.
    void resetGraph();
    /// FlowRuntime::runtimeErrors().
    std::vector<std::string> runtimeErrorList;

    // values()/valueNames() (node outputs only, docs/tasks/T-56.md).
    std::map<std::pair<int, std::string>, size_t> outputSlot;
    std::vector<PortValue> valueList;
    std::vector<std::string> valueNamesList;

    Status load(const FlowGraph& graph, Document& document, std::vector<std::string>& errors);
    /// T-193: FlowRuntime::loadGlobal().
    Status loadGlobal(const FlowGraph& graph, BundleContext& bundle, std::vector<std::string>& errors);
    Status rebind(Document& document, std::vector<std::string>& warnings);
    /// T-260c: FlowRuntime::unload() (deferred like load() inside a dispatch).
    void unload();
    void tick(float dt);

    // ---- binding (flow_bindings.cpp) ----
    /// Registers every edge endpoint and subscribes every document output port the edges read
    /// (element / `<anim>` ones first, then the "#document.*" ones, in edge order).
    void bindDocumentEndpoints();
    void applyToSink(const BoundEdge& edge, const PortValue& v);

    // ---- evaluation (flow_nodes.cpp) ----
    void seedInitialOutputs();
    void deliverToNode(int nodeIndex, const std::string& port, const PortValue& v, bool isPulse);
    void handlePulseInput(int nodeIndex, const std::string& port);
    /// T-193: the pulse inputs of the action and `timeline` nodes.
    void handleActionPulse(int nodeIndex, const std::string& port);
    void evaluateNode(int nodeIndex);
    void setCounter(int nodeIndex, double value);
    /// `radio` (T-123): clamps `index` into 0..count-1, stores it and pushes `index`, firing
    /// `changed` when it actually moved.
    void setRadio(int nodeIndex, double index);
    void fanOut(const Endpoint& source, const PortValue& v);
    /// Rebuilds nodeFanOut/externalFanOut (and the per-edge guard state) from the current `edges`.
    /// load() and rebind() both need it after they settle `edges`.
    void buildFanOutIndex();
    /// The edges leaving `source`, or nullptr when nothing is wired to it.
    const std::vector<uint32_t>* fanOutEdges(const Endpoint& source) const;
    /// Appends one "flow feedback loop: ..." entry for `edgeIndex`, at most once per edge.
    void reportFeedbackLoop(size_t edgeIndex, const char* reason);
    /// A "<nodeId>.<port>" / "#<elementId>.<port>" / "#document.<port>" name, for diagnostics.
    std::string endpointName(const Endpoint& ep) const;
    void pushRawOutput(int nodeIndex, const std::string& port, const PortValue& v);
    void pushNumberOutput(int nodeIndex, const std::string& port, double v);
    void pushBoolOutput(int nodeIndex, const std::string& port, bool v);
    void pushStringOutput(int nodeIndex, const std::string& port, const std::string& v);
    void firePulseOutput(int nodeIndex, const std::string& port);
};
