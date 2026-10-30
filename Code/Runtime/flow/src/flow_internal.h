#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "flow_slots.h"
#include "xt/document.h"
#include "xt/flow/flow_graph.h"
#include "xt/flow/flow_runtime.h"
#include "xt/perf.h"
#include "xt/style_template.h"

namespace xt {
class Context;
}  // namespace xt

/// Private, shared implementation details for xt_flow, split across flow_graph.cpp (structural
/// validation, used without a Document), flow_bindings.cpp (the shared validator plus Document/
/// `<anim>` connection-point binding, and the document-endpoint layer shared with HostPorts),
/// flow_nodes.cpp (the node types' runtime behavior), flow_signals.cpp (fan-out, the T-389 signal
/// batch, values()) and flow_runtime.cpp (FlowRuntime::Impl's public-facing load/tick/values) and
/// host_ports.cpp (HostPorts, Docs/tasks/T-144.md). See Docs/tasks/T-56.md.
///
/// T-476 (Docs/design/27-interaction.md §27.4.2 items 1-3, 5): a loaded graph is compiled into
/// integers -- node ports and params are slots of one RtValue array (flow_slots.h), document
/// endpoints are external slots, fan-out is a CSR table, strings live in the runtime's StringPool
/// -- so the tick works without string keys and, in steady state, without heap allocation.

namespace xt::flow {

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
/// initializers -- see Docs/design/14-coding-standards.md §14.1's designated-initializer note;
/// this is the same underlying issue for a positional aggregate init.
inline PortValue typedValue(PortType t) {
    PortValue v;
    v.type = t;
    return v;
}

/// A resolved edge endpoint: either a node's own port, or a document connection point (a plain
/// element, an `<anim>` element, or the document itself, Docs/design/17-editor.md §17.6).
/// T-476: this is the descriptor validation produces and diagnostics / document bindings read;
/// the running graph addresses endpoints by SlotEndpoint.
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
/// `radio`'s `count` param (Docs/design/17-editor.md §17.6): how many `set<i>` pulse inputs the
/// node has. Defaults to 2 (the smallest legal count) when the param is missing or out of the
/// 2..64 range -- FlowGraph::parse() rejects both, so only nodeTypeTable()'s param-less probe
/// node and a hand-built graph ever see the default.
uint32_t radioCount(const NodeDesc& node);
/// `lookup`'s `values` array element type (Docs/design/17-editor.md §17.6), i.e. its `out` port's
/// type. PortType::Number for an absent/empty array (FlowGraph::parse() rejects both).
PortType lookupValueType(const NodeDesc& node);
/// True if `port` is a §17.6 row-1 ("有 id 的元素") port in the requested direction.
bool elementPortInfo(const std::string& port, bool isSource, PortType& outType);
/// True if `port` is a §17.6 row-2 ("<anim> 元素") port in the requested direction.
bool animPortInfo(const std::string& port, bool isSource, PortType& outType);
/// True if `port` is a §17.6 row-3 ("文件") port (output-only).
bool docPortInfo(const std::string& port, PortType& outType);
/// T-175 (Docs/design/27-interaction.md §27.4): the flow PortType of a declared port's value
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
/// A node type's output ports, in Docs/tasks/T-56.md table order (used by values()/valueNames()).
/// T-476: nodeSlotTable(type).outputs as strings.
std::vector<std::string> outputPortOrder(NodeType type);

/// Shared validator (Docs/tasks/T-56.md "規則"): edge endpoint resolution, type matching
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

// ---- Document endpoint layer (shared by FlowRuntime and HostPorts, Docs/tasks/T-144.md) ------

/// T-476: a `#document.*` output port, parsed once when it is subscribed (documentPortValue()).
enum class DocPort : uint8_t {
    None,
    Clock,
    ViewportWidth,
    ViewportHeight,
    PointerX,
    PointerY,
    PointerNX,
    PointerNY,
    PointerDown
};
/// The DocPort of the row-3 port `port`; DocPort::None for any other name.
DocPort docPortOf(const std::string& port);

/// T-476 (27 §27.4.2 item 2): "no output slot" / "no tag".
inline constexpr uint32_t kNoOutput = 0xFFFFFFFFu;

/// T-476: one document output port a DocEndpointState observes (its output slot is the index in
/// DocEndpointState::outputs). The Document listeners capture the slot, never a name.
struct DocOutput {
    Endpoint::Owner owner = Endpoint::Owner::Doc;
    std::string elementId;
    std::string port;
    PortType type = PortType::Pulse;
    DocPort docPort = DocPort::None;  ///< Owner::Doc only.
    /// The change-only filter (emitDocumentOutput()): the last value reported, when `hasLast`.
    PortValue last;
    bool hasLast = false;
    /// The consumer's own tag (FlowRuntime: its source binding), kNoOutput until it sets one.
    uint32_t tag = kNoOutput;
    /// T-477 (27 §27.4.2 item 4): an `<anim>` "localTime" output: its element's entry of
    /// DocEndpointState::animInstance (the current Animator instance), found on the first poll
    /// after the element was registered; null again whenever resetDocumentTreeState() clears it.
    const uint32_t* animInstance = nullptr;
};

/// Everything one consumer (a FlowRuntime or a HostPorts) keeps about the document connection
/// points it drives or listens to. The free functions below are the single implementation of
/// "send a value into a document input port" and "observe a document output port"; FlowRuntime
/// routes what they report through its edges, HostPorts through the host's listeners.
struct DocEndpointState {
    /// Receives every output a subscribed port produces, after emitDocumentOutput()'s change-only
    /// filter (a value port only reports a value that differs from the last one it reported).
    /// T-476: `slot` is the port's index in `outputs`.
    using OutputFn = std::function<void(uint32_t slot, const PortValue& v)>;

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
    /// T-476: every subscribed output port (slot = index; the change-only filter's last value
    /// lives in it), and externalKey() -> slot for subscribeDocumentOutput()'s own lookups.
    std::vector<DocOutput> outputs;
    std::map<std::string, uint32_t> outputIndex;
    std::vector<uint32_t> docOutputSlots;      // The "#document.<port>" output slots, polled.
    std::vector<uint32_t> animLocalTimeSlots;  // The <anim> "localTime" output slots, polled.
    bool pointerSubscribed = false;            // The document-root pointer listeners are registered.
    float clockAccum = 0;                      // "#document.clock": the sum of every tick() `dt`.

    // Pointer connection-point state (T-77), fed by the document-root PointerMove/Down/Up
    // listeners and read back by documentPortValue().
    Vec2 pointerPos{};         // Last observed position, viewport logical px; only once pointerSeen.
    bool pointerSeen = false;  // False until the first pointer event (position reads as the viewport center).
    bool pointerDown = false;  // True while a pointer button is held (any button).
    /// T-476: the values pollDocumentOutputs() and the pointer listeners report, reused (a
    /// PortValue per report would construct a std::string each time).
    PortValue pollValue;
    PortValue pointerValue;

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

/// T-476: a value on its way into a document input port, its string viewed (not copied).
struct InputValue {
    PortType type = PortType::Pulse;
    bool b = false;
    double n = 0;
    std::string_view s;
};

/// T-477 (27 §27.4.2 item 4): which document input a sink endpoint is (its port name, parsed once).
enum class SinkOp : uint8_t {
    None,
    PortSet,   ///< A declared `in` / `out` port (Owner::Port).
    PortCall,  ///< A declared `call` port.
    SetText,
    SetStyle,
    SetValue,
    PlayAnimation,
    StopAnimation,
    Focus,
    AddClass,
    RemoveClass,
    ToggleClass,
    SetProperty,
    AnimPlay,
    AnimPause,
    AnimResume,
    AnimSeek,
    AnimSetSpeed
};

/// T-477: one document sink resolved against one endpoint state, once (bind time; rebind() and a
/// state change resolve it again), so a write no longer looks the port, the element or the
/// property up by name. Valid while `doc` / `serial` are the state's (sinkCacheValid()).
struct SinkCache {
    const Document* doc = nullptr;  ///< The Document it was resolved against; null = not yet.
    uint32_t serial = 0;            ///< Its loadSerial() then.
    SinkOp op = SinkOp::None;
    /// Owner::Port: the declared port, its direction and value type. `portStable` false: the name
    /// may resolve to another port later (a `<repeat>` instance copy, T-380 / T-391), so every
    /// write looks it up again, as before T-477.
    PortId port = kInvalidPort;
    PortDir dir = PortDir::In;
    PortValueType type = PortValueType::Number;
    bool portStable = true;
    /// Owner::Element / Owner::Anim: the element (an ElementRef's node and generation, taken from
    /// DocEndpointState::elementRefs).
    NodeId element = kInvalidNode;
    uint32_t elementGen = 0;
    /// SinkOp::SetProperty: the edge's `prop` param.
    PropId prop = prop::kInvalidProp;
};

/// Which way one sink write went (FrameStats::flowSinkTyped / flowSinkText).
enum class SinkPath : uint8_t { Other, Typed, Text };

/// True when `cache` was resolved against `state`'s current Document.
bool sinkCacheValid(const SinkCache& cache, const DocEndpointState& state);
/// Resolves `sink` (registerDocumentEndpoint() first) with edge parameter `param` against `state`.
/// `reused` false: for one write only (the port-table stability check is skipped).
void resolveSink(const DocEndpointState& state, const Endpoint& sink, const std::string& param,
                 SinkCache& out, bool reused = true);
/// Sends `v` into the sink `cache` describes (resolveSink() against `state`). `styleTemplate`
/// (SinkOp::SetStyle only, may be null) is the compiled shape of the texts this sink receives
/// (ElementRef::setStyleTemplate()). Returns which way the value went.
SinkPath applyResolvedInput(DocEndpointState& state, const Endpoint& sink, const SinkCache& cache,
                            const InputValue& v, const std::string& param,
                            const StyleTemplate* styleTemplate);

/// Sends `v` into the element / `<anim>` input port `sink` (registerDocumentEndpoint() first).
/// `param` is the edge parameter (see documentInputParamName()). Type checks are the caller's.
void applyDocumentInput(DocEndpointState& state, const Endpoint& sink, const InputValue& v,
                        const std::string& param);
/// The same with a PortValue (HostPorts).
void applyDocumentInput(DocEndpointState& state, const Endpoint& sink, const PortValue& v,
                        const std::string& param);

/// Starts observing the output port `source` (registerDocumentEndpoint() first for an element /
/// `<anim>` one): Document listeners for the event-driven ports (reported as the Document
/// dispatches), the polled lists for "#document.*" and `<anim>` "localTime" (reported by
/// pollDocumentOutputs()). Idempotent per port. T-476: returns the port's output slot (the same
/// one again for a port already subscribed), kNoOutput for a node or lifecycle endpoint.
uint32_t subscribeDocumentOutput(DocEndpointState& state, const Endpoint& source);

/// Reports every subscribed polled port's current value (change-only).
void pollDocumentOutputs(DocEndpointState& state);

/// The current value of one "#document.<port>" output port, written into `out` (T-476: a
/// reused PortValue, so a poll constructs none).
void documentPortValue(const DocEndpointState& state, DocPort port, PortValue& out);

/// Change-only filter plus dispatch to `state.output`: a pulse always goes through, a value only
/// when it differs from the last one reported for the same port (output slot `slot`).
void emitDocumentOutput(DocEndpointState& state, uint32_t slot, const PortValue& v, bool isPulse);

/// Drops every Document listener subscribeDocumentOutput() registered -- removing them from
/// `state.doc` only when `removeListeners` (false when that Document is already destroyed) -- and
/// silences pending "finished" callbacks. Clears the subscription bookkeeping (the output slots
/// with their last values) and the polled lists; keeps the tree state (see
/// resetDocumentTreeState()) and the clock.
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

// ---- Runtime values (T-476, 27 §27.4.2 item 3) --------------------------------------------------

/// The type of an RtValue that holds nothing yet: an input no edge has delivered to, an output
/// never pushed, an absent param. Never a PortValue type; never reaches a PortValue.
inline constexpr PortType kNoValue = static_cast<PortType>(0xFF);

/// One runtime value. `str` is a StringPool id, meaningful for PortType::String only.
struct RtValue {
    PortType type = kNoValue;
    bool b = false;
    double n = 0;
    uint32_t str = 0;
};

/// The runtime's strings: interned constants (params, `lookup` tables; deduplicated, never
/// written) and mutable buffers, each owned by one storage place (a value slot, a pending signal,
/// a fan-out level) that copies a string into it -- so the same text never has two owners that
/// can drift apart, and a buffer keeps its capacity: copying into it allocates only while it
/// grows. Append-only while a graph is loaded (a std::deque: ids and references stay valid);
/// clear() empties it. Id 0 is the empty string.
class StringPool {
public:
    StringPool() { clear(); }
    /// The id of a constant `text` (load time).
    uint32_t intern(std::string_view text);
    /// A new mutable buffer (load / bind time).
    uint32_t addBuffer();
    const std::string& text(uint32_t id) const { return m_strings[id]; }
    std::string& buffer(uint32_t id) { return m_strings[id]; }
    size_t size() const { return m_strings.size(); }
    void clear();

private:
    std::deque<std::string> m_strings;
    std::map<std::string, uint32_t, std::less<>> m_interned;
};

/// One piece of a `format` / `toString` format string, cut at load time: a literal range of the
/// node's `formatText`, or (input >= 0) the formatted value of that input slot.
struct FormatPiece {
    uint32_t offset = 0;
    uint32_t length = 0;
    int32_t input = -1;
};

/// `math`'s `op`, parsed at load (an unknown or absent op computes 0).
enum class MathOp : uint8_t { Unknown, Add, Sub, Mul, Div, Min, Max, Clamp, Map, Abs, Round };
/// `compare`'s `op`, parsed at load (an unknown or absent op is false).
enum class CompareOp : uint8_t { Unknown, Eq, Ne, Lt, Le, Gt, Ge };
MathOp parseMathOp(std::string_view op);
CompareOp parseCompareOp(std::string_view op);

// ---- Runtime node state ------------------------------------------------------------------------

/// One node's live state. Its ports and params are slots of FlowRuntime::Impl::values:
/// `inputBase + slot`, `outputBase + slot`, `paramBase + slot` (flow_slots.h tables).
struct NodeRuntime {
    NodeType type = NodeType::Constant;
    std::string id;
    uint32_t inputBase = 0;
    uint32_t outputBase = 0;
    uint32_t paramBase = 0;
    uint16_t inputCount = 0;
    uint16_t outputCount = 0;
    uint16_t paramCount = 0;
    uint8_t op = 0;               // `math`: MathOp; `compare`: CompareOp.
    uint32_t valueListBase = 0;   // This node's first entry in values() / valueNames().
    std::vector<RtValue> lookup;  // `lookup`: NodeDesc::arrayParams["values"] (strings interned).
    std::string uiId;             // T-193 `timeline`: its `ui` param (global flow).
    // `format` / `toString`: the format string, cut into pieces, and the evaluation buffer.
    std::string formatText;
    std::vector<FormatPiece> pieces;
    std::string scratch;
    bool hasDecimals = false;
    int decimals = 0;
    uint32_t radioCount = 2;  // `radio`: its `count` param.
    double radioIndex = 0;    // `radio`: its current index.
    double counterValue = 0;
    bool toggleValue = false;
    bool timerRunning = false;
    float timerElapsed = 0;
    double timerCount = 0;
    bool compareLastValid = false;
    bool compareLastResult = false;
    std::mt19937 rng{std::random_device{}()};
};

/// T-476: an edge endpoint of the running graph -- a node slot or an external slot.
struct SlotEndpoint {
    Endpoint::Owner owner = Endpoint::Owner::Node;
    int32_t node = -1;        ///< Owner::Node: index into nodes.
    uint16_t slot = kNoSlot;  ///< Owner::Node: input / output slot of that node.
    uint32_t external = 0;    ///< Otherwise: index into externals.
    PortType type = PortType::Pulse;
};

/// T-476: one edge of the running graph (same index as FlowRuntime::Impl::edges).
struct RtEdge {
    SlotEndpoint from;
    SlotEndpoint to;
    int64_t instance = -1;
    /// T-477: a document sink, resolved (resolveSink()) for the endpoint state it last wrote to.
    SinkCache sink;
    /// T-477: a `format` / `toString` -> `setStyle` edge whose format compiled
    /// (compileStyleTemplate()): its index in FlowRuntime::Impl::styleTemplates, else -1.
    int32_t styleTemplate = -1;
};

/// T-476: one document endpoint (element / `<anim>` / document / declared port / lifecycle) the
/// edges reference, allocated when the graph is compiled. `desc` keeps the names for the
/// document bindings and the diagnostics.
struct ExternalSlotInfo {
    Endpoint desc;
    bool valueSignal = false;  ///< isValueSignal() of it as a source.
};

/// Exact comparison (Docs/tasks/T-56.md "值型埠：只在值改變時推送（浮點比較 exact）").
bool valuesEqual(const PortValue& a, const PortValue& b);
/// T-193: the param `type` requires as a string (`sound.cue`, `haptic.cue`, `spawn.symbol`,
/// `emit.port`, `timeline.name`); nullptr when the type has none.
const char* requiredStringParam(NodeType type);

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
        /// T-476: the external slots of `ui#<uiId>.shown` / `.hidden`, kNoOutput when unused.
        uint32_t shownExternal = kNoOutput;
        uint32_t hiddenExternal = kNoOutput;
    };
    std::vector<UiDoc> uiDocs;
    /// True after a successful loadGlobal().
    bool global = false;
    /// FlowRuntime::setEnabled().
    bool enabled = true;
    FlowHostEventListener hostListener;
    /// The UI / instance of the external event currently propagating (FlowHostEvent::uiId /
    /// instanceId, and the source side of an edge's `instance` filter). T-476: `currentUi` points
    /// at a UI id the graph owns (null = none), so switching it copies no string.
    const std::string* currentUi = nullptr;
    uint32_t currentInstance = 0;
    /// Timeline names already reported as missing (one runtime error each).
    std::set<std::string> missingTimelines;

    /// The endpoint states `uiId` addresses: the load() Document for "" (when bound), else every
    /// registered instance of that UI.
    std::vector<DocEndpointState*> statesFor(const std::string& uiId);
    /// ContextListener::onShown/onHidden of a registered handle.
    void lifecycleEvent(const Context* context, DocumentHandle handle, bool shown);
    Status postSystemEvent(std::string_view kind, std::string_view value);
    /// Sends one FlowHostEvent from node `nodeIndex`.
    void sendHostEvent(FlowHostEventKind kind, int nodeIndex, const std::string& name, const PortValue& value,
                       float volume);
    /// `timeline` node inputs (input slot `slot`).
    void timelineInput(int nodeIndex, uint16_t slot, const RtValue& v);
    /// Registers the `timeline` nodes' TimelineEnd / TimelineMarker listeners.
    void bindTimelineNodes();
    /// Stores `text` in String output `slot` and fans it out: an output that fires on every event
    /// (system strings, `timeline.marker`).
    void fireEventText(int nodeIndex, uint16_t slot, std::string_view text);
    /// Shared part of load()/loadGlobal(): builds the node array and value slots and validates
    /// (`uiMap` as validateGraph()). Expects the endpoint states to be reset already.
    Status loadGraph(const FlowGraph& graph, std::vector<std::string>& errors,
                     const std::map<std::string, Document*>* uiMap);
    /// T-476: builds node `nd`'s slots (values, params, format pieces, ops) as node `index`.
    void compileNode(const NodeDesc& nd, NodeRuntime& rt);
    /// Unbinds every endpoint state (removing listeners only when `removeListeners`).
    void unbindAll(bool removeListeners);
    /// T-260b (BundleContext::removeUi): unbinds every instance of `uiId` and detaches it (no
    /// Document, no UI id, no handle). The endpoint states stay allocated until the next load: a
    /// callback of theirs may be running further up the stack.
    void removeUi(std::string_view uiId);
    // ---- T-475 (Docs/design/21-perf-tracing.md §21.6): stage times and counters --------------
    /// FlowRuntime::setPerfTracker(); null = nothing recorded.
    PerfTracker* perfTracker = nullptr;
    /// The frame being recorded, or null when there is no tracker or it is disabled.
    FrameStats* perfStats() {
        return perfTracker != nullptr && perfTracker->enabled() ? &perfTracker->current() : nullptr;
    }
    /// Adds one to `field` of the frame being recorded (no-op without one).
    void perfCount(uint32_t FrameStats::* field) {
        if (FrameStats* stats = perfStats()) {
            ++(stats->*field);
        }
    }
    /// The stage whose clock runs now (exclusive times: entering a stage stops the outer one).
    enum class PerfStage : uint8_t { None, Deliver, Eval, Sink, Poll };
    PerfStage perfStage = PerfStage::None;
    std::chrono::steady_clock::time_point perfStageStart{};
    /// Charges the time since the last switch to the current stage, then makes `next` current.
    void perfSwitch(PerfStage next);
    // The timed twins of the steps a feedback loop through the document recurses through
    // (Docs/tasks/T-124.md): the untimed path keeps its stack frames as they were, so only a run
    // with a tracker pays for the StageScope.
    /// An output delivered at once (not queued), inside a Deliver stage: flushSignals() first,
    /// then deliverExternal().
    void deliverNowTimed(uint32_t binding, const PortValue& v);
    /// T-478: allocations inside the text-path `setStyle` writes of applyDocumentInputTimed()
    /// (the core's CSS parse; outermost ones only, `sinkDepth` tells), left out of
    /// FrameStats::flowAllocs. Every other sink write is counted.
    uint64_t sinkAllocs = 0;
    uint32_t sinkDepth = 0;
    /// applyResolvedInput() inside a Sink stage, counted (T-477: also typed / text).
    void applyDocumentInputTimed(DocEndpointState& state, const Endpoint& sink, const SinkCache& cache,
                                 const InputValue& v, const std::string& param, const StyleTemplate* tmpl);
    /// evaluateNode() inside an Eval stage.
    void evaluateNodeTimed(int nodeIndex);
    /// Runs `stage` for its lifetime and returns to the outer stage after. One branch and no
    /// clock read while nothing is recorded or `stage` is already the current one.
    struct StageScope {
        Impl& impl;
        PerfStage outer;
        bool active;
        StageScope(Impl& owner, PerfStage stage)
            : impl(owner),
              outer(owner.perfStage),
              active(stage != owner.perfStage && owner.perfStats() != nullptr) {
            if (active) {
                impl.perfSwitch(stage);
            }
        }
        ~StageScope() {
            if (active) {
                impl.perfSwitch(outer);
            }
        }
        StageScope(const StageScope&) = delete;
        StageScope& operator=(const StageScope&) = delete;
    };

    // True only inside seedInitialOutputs(): see its call in load() and evaluateNode()'s Compare
    // case for why `compare`'s true/false pulses are suppressed while this is set.
    bool seeding = false;

    std::vector<NodeRuntime> nodes;
    std::map<std::string, int> nodeIndexById;
    /// The validated edges (descriptors: rebind() and the diagnostics read them) and, same index,
    /// the compiled ones the tick runs (T-476).
    std::vector<BoundEdge> edges;
    std::vector<RtEdge> rtEdges;

    // ---- T-476: compiled graph (27 §27.4.2 items 1-3) ------------------------------------------
    /// Every node's input, output and param slots.
    std::vector<RtValue> values;
    /// The runtime's strings (see StringPool).
    StringPool strings;
    /// values[i]'s string buffer is strings.buffer(slotHomeBase + i).
    uint32_t slotHomeBase = 0;
    uint32_t slotHome(uint32_t slot) const { return slotHomeBase + slot; }
    /// The document endpoints the edges use.
    std::vector<ExternalSlotInfo> externals;
    /// CSR fan-out: the edges leaving fan-out key `k` are fanOutEdgeList[fanOutStart[k] ..
    /// fanOutStart[k + 1]), in edge order. Keys: a node output's value slot, then
    /// externalKeyBase + external slot.
    std::vector<uint32_t> fanOutStart;
    std::vector<uint32_t> fanOutEdgeList;
    uint32_t externalKeyBase = 0;
    /// One fan-out level's private copy of the value it delivers (a string one in its own buffer,
    /// so a nested push cannot change it under the outer loop), by fanOutDepth.
    std::vector<uint32_t> fanOutHomes;
    /// Builds rtEdges, externals, the CSR fan-out and the topological order from `edges`.
    void compileEdges();
    /// T-477: the compiled `format` / `toString` -> `setStyle` shapes (RtEdge::styleTemplate).
    std::vector<StyleTemplate> styleTemplates;
    /// T-477: gives every `format` / `toString` -> `setStyle` edge its StyleTemplate when the
    /// node's format compiles (compileEdges()).
    void compileStyleTemplates();
    /// T-477: resolves the document sinks of the edges against the endpoint states (bind time).
    void resolveSinks();
    /// Copies `src` into `dst`, whose string buffer is `home` (no allocation while it fits).
    void assignValue(uint32_t home, RtValue& dst, const RtValue& src);
    /// Converts `src` into `dst` (string into buffer `home`).
    void assignFromPort(uint32_t home, RtValue& dst, const PortValue& src);
    /// `v` as a PortValue (allocates for a string): values(), host events.
    PortValue toPortValue(const RtValue& v) const;
    /// Exact equality, strings by content.
    bool rtEqual(const RtValue& a, const RtValue& b) const;
    /// Reads a numeric / boolean param slot (absent or another type: `def`).
    double paramNum(const NodeRuntime& n, uint16_t slot, double def) const;
    bool paramBool(const NodeRuntime& n, uint16_t slot, bool def) const;
    /// A string param slot's text ("" when absent or not a string).
    const std::string& paramText(const NodeRuntime& n, uint16_t slot) const;
    /// Appends `v` formatted as `toString` / `format` do (formatPortValue()) to `out`.
    void appendFormatted(std::string& out, const RtValue& v, const NodeRuntime& n) const;

    std::vector<float> edgeLastFiredList;  // FlowRuntime::edgeLastFired(), in `edges` order.

    // ---- T-389: value signals coalesced per frame (Docs/design/27-interaction.md §27.4.1) -------
    //
    // A value-type document output (isValueSignal()) is not fanned out while the Document
    // dispatches input: its latest value waits in its source binding's pending slot, and tick()
    // delivers every pending one once, as one batch. Pulse / event outputs still go through at
    // once (after the pending values, so they see them).
    /// T-476: one subscribed document output of one endpoint state -- what a Document listener
    /// reports to (DocOutput::tag) -- with its pending-signal slot.
    struct SourceBinding {
        DocEndpointState* state = nullptr;  ///< `docs` or a UiDoc's state.
        uint32_t external = 0;
        bool valueSignal = false;
        uint8_t pending = 0;  ///< `pendingValue` waits in pendingList.
        RtValue pendingValue;
        uint32_t pendingHome = 0;  ///< pendingValue's string buffer.
        uint32_t nowHome = 0;      ///< The string buffer of a value delivered at once.
    };
    std::vector<SourceBinding> sources;
    /// The source bindings with a pending value, in the order they were first queued.
    std::vector<uint32_t> pendingList;
    /// The batches being delivered (deliverPendingSignals(); a nested flush stacks on top): each
    /// entry a binding and a copy of its value, the string in batchHomes[entry index].
    struct BatchEntry {
        uint32_t binding = 0;
        RtValue value;
    };
    std::vector<BatchEntry> batchStack;
    std::vector<uint32_t> batchHomes;
    size_t batchTop = 0;
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
    /// T-476 (27 §27.4.2 item 5): values() / valueNames() has been called since the load; until
    /// then syncValueList() leaves valueList alone (the stale list keeps what it has to copy).
    bool valuesObserved = false;
    /// Marks `nodeIndex`'s outputs for syncValueList().
    void markValuesStale(int nodeIndex);
    /// T-407 (43 §43.3): true when any bound Document (`docs` or a global flow's UI) coalesces
    /// (coalescesSignals()); tick() then runs as one batch. False: every push is evaluated at
    /// once, the pre-T-389 tick.
    bool anyCoalescing() const;

    /// The entry point of every output a bound Document reports (already change-filtered), from
    /// `state`'s output slot `slot`: a value signal is queued, anything else is delivered at once.
    void signalOutput(DocEndpointState& state, uint32_t slot, const PortValue& v);
    /// Keeps the latest value of one value signal for the next tick().
    void queueSignal(uint32_t binding, const PortValue& v);
    /// Fans one document output out along its edges (the pre-T-389 immediate path), under the UI
    /// / instance context it came from.
    void deliverExternal(uint32_t external, DocEndpointState* state, const RtValue& v);
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

    // ---- Feedback guard (Docs/tasks/T-124.md) --------------------------------------------------
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

    // values()/valueNames() (node outputs only, Docs/tasks/T-56.md).
    std::vector<PortValue> valueList;
    std::vector<std::string> valueNamesList;
    /// The `clock` / `timer` nodes, in node order (tick() advances them).
    std::vector<int> timedNodes;

    Status load(const FlowGraph& graph, Document& document, std::vector<std::string>& errors);
    /// T-193: FlowRuntime::loadGlobal().
    Status loadGlobal(const FlowGraph& graph, BundleContext& bundle, std::vector<std::string>& errors);
    Status rebind(Document& document, std::vector<std::string>& warnings);
    /// T-260c: FlowRuntime::unload() (deferred like load() inside a dispatch).
    void unload();
    void tick(float dt);

    // ---- binding (flow_bindings.cpp) ----
    /// Registers every edge endpoint and subscribes every document output port the edges read
    /// (element / `<anim>` ones first, then the "#document.*" ones, in edge order), giving each
    /// subscribed output of each endpoint state a source binding.
    void bindDocumentEndpoints();
    /// Gives `state`'s output slot `slot` a source binding for external slot `external`.
    void bindSource(DocEndpointState& state, uint32_t slot, uint32_t external);
    /// Delivers `v` along edge `edgeIndex` into its sink.
    void applyToSink(uint32_t edgeIndex, const RtValue& v);

    // ---- evaluation (flow_nodes.cpp) ----
    void seedInitialOutputs();
    void deliverToNode(int nodeIndex, uint16_t slot, const RtValue& v, bool isPulse);
    void handlePulseInput(int nodeIndex, uint16_t slot);
    /// T-193: the pulse inputs of the action and `timeline` nodes.
    void handleActionPulse(int nodeIndex, uint16_t slot);
    void evaluateNode(int nodeIndex);
    void setCounter(int nodeIndex, double value);
    /// `radio` (T-123): clamps `index` into 0..count-1, stores it and pushes `index`, firing
    /// `changed` when it actually moved.
    void setRadio(int nodeIndex, double index);
    /// Delivers `v` along every edge leaving fan-out key `key` (see fanOutStart).
    void fanOut(uint32_t key, const RtValue& v);
    /// Appends one "flow feedback loop: ..." entry for `edgeIndex`, at most once per edge.
    void reportFeedbackLoop(size_t edgeIndex, const char* reason);
    /// A "<nodeId>.<port>" / "#<elementId>.<port>" / "#document.<port>" name, for diagnostics.
    std::string endpointName(const Endpoint& ep) const;
    /// Output slot `slot` of node `nodeIndex`: stores `v` and fans it out when it changed.
    void pushRawOutput(int nodeIndex, uint16_t slot, const RtValue& v);
    void pushNumberOutput(int nodeIndex, uint16_t slot, double v);
    void pushBoolOutput(int nodeIndex, uint16_t slot, bool v);
    /// The node's `scratch` text as its String output `slot` (swapped in, no copy).
    void pushScratchOutput(int nodeIndex, uint16_t slot);
    void firePulseOutput(int nodeIndex, uint16_t slot);
};
