#include <algorithm>
#include <cfloat>
#include <cmath>
#include <set>
#include <utility>

#include "flow_internal.h"
#include "xt/flow/bundle_context.h"

/// FlowRuntime's public surface (ctor/dtor/load/tick/values/valueNames) and its per-frame tick()
/// loop: clock/timer node advancement plus the polled `<anim>` `localTime` and `#document.*`
/// connection points (Docs/tasks/T-56.md "執行規則"). load() builds the node array, delegates
/// structural validation + endpoint resolution to validateGraph() (flow_bindings.cpp), then binds
/// every connection point and seeds every node's initial output. rebind() (T-93) redoes that
/// last part alone against a reloaded Document, keeping the node state.

namespace xt::flow {

FlowRuntime::Impl::Impl() {
    // Every document / element / `<anim>` output the shared endpoint layer reports (already
    // change-filtered) fans out along the edges leaving that port.
    // The load() Document's outputs keep the pre-T-193 stack footprint (a feedback loop through
    // the document recurses through here up to kMaxFanOutDepth times): only the instance id is
    // swapped in, the UI id stays empty (externalOutput() is the global flow's path).
    // T-389: a value signal is held back until the next tick() (queueSignal()); an event first
    // delivers the held values, then goes out as before -- inline, so a loop through the document
    // recurses through no more frames than it did. T-407: only from behavior 1 on.
    // T-476: the listeners report an output slot of `docs`; signalOutput() maps it to its source
    // binding and external slot.
    docs.output = [this](uint32_t slot, const PortValue& v) { signalOutput(docs, slot, v); };
}

void FlowRuntime::Impl::unbindAll(bool removeListeners) {
    unbindDocumentEndpoints(docs, removeListeners);
    for (UiDoc& ui : uiDocs) {
        unbindDocumentEndpoints(*ui.state, removeListeners);
    }
}

void FlowRuntime::Impl::removeUi(std::string_view uiId) {
    for (UiDoc& ui : uiDocs) {
        if (ui.uiId != uiId) {
            continue;
        }
        detachDocumentEndpoints(*ui.state);
        ui.uiId.clear();  // statesFor() / applyToSink() never match it again.
        ui.handle = 0;    // Nor does lifecycleEvent() (handles start at 1).
    }
}

Status FlowRuntime::Impl::load(const FlowGraph& graph, Document& document, std::vector<std::string>& errors) {
    if (dispatchDepth > 0) {
        // T-260c: asked from inside this runtime's own dispatch (a host callback): run once it
        // returns, the last request winning.
        pending = PendingLoad{};
        pending.kind = PendingLoad::Kind::Load;
        pending.graph = graph;
        pending.doc = &document;
        pending.serial = document.loadSerial();
        return Status::kOk;
    }
    DispatchGuard dispatch(*this);
    // Release whatever a previous load() put on the previous Document first: its listeners hold a
    // `this` that is about to describe a completely different graph.
    unbindAll(/*removeListeners=*/true);
    uiDocs.clear();
    resetDocumentTreeState(docs);
    docs.doc = &document;
    docs.serial = document.loadSerial();  // T-260c.
    const Status status = loadGraph(graph, errors, nullptr);
    if (status != Status::kOk) {
        clearAll();  // T-260c: a failed load keeps nothing of the Document.
    }
    return status;
}

Status FlowRuntime::Impl::loadGlobal(const FlowGraph& graph, BundleContext& bundle,
                                     std::vector<std::string>& errors) {
    if (dispatchDepth > 0) {  // T-260c: as load().
        pending = PendingLoad{};
        pending.kind = PendingLoad::Kind::LoadGlobal;
        pending.graph = graph;
        pending.bundle = &bundle;
        return Status::kOk;
    }
    DispatchGuard dispatch(*this);
    unbindAll(/*removeListeners=*/true);
    uiDocs.clear();
    resetDocumentTreeState(docs);
    docs.doc = nullptr;  // The global flow has no Document of its own.
    std::map<std::string, Document*> uiMap;
    for (const std::string& uiId : bundle.uiIds()) {
        for (const BundleContext::Instance& instance : bundle.instances(uiId)) {  // T-356: any Context.
            const DocumentHandle handle = instance.handle;
            Document* document = instance.context->document(handle);
            if (document == nullptr) {
                continue;
            }
            UiDoc ui;
            ui.uiId = uiId;
            ui.handle = handle;
            ui.state = std::make_unique<DocEndpointState>();
            ui.state->doc = document;
            ui.state->context = instance.context;  // T-260b: liveDocument() re-resolves `handle`.
            ui.state->handle = handle;
            ui.state->serial = document->loadSerial();  // T-260c.
            DocEndpointState* state = ui.state.get();
            state->output = [this, state](uint32_t slot, const PortValue& v) {
                signalOutput(*state, slot, v);  // T-389, T-476.
            };
            uiDocs.push_back(std::move(ui));
            uiMap.emplace(uiId, document);  // The first instance answers the validation.
        }
    }
    Status status = loadGraph(graph, errors, &uiMap);
    global = status == Status::kOk;
    if (status != Status::kOk) {
        clearAll();  // T-260c: as load().
    }
    return status;
}

void FlowRuntime::Impl::unload() {
    if (dispatchDepth > 0) {  // T-260c: as load().
        pending = PendingLoad{};
        pending.kind = PendingLoad::Kind::Unload;
        return;
    }
    clearAll();
}

void FlowRuntime::Impl::clearAll() {
    unbindAll(/*removeListeners=*/true);
    uiDocs.clear();
    resetDocumentTreeState(docs);
    docs.doc = nullptr;
    docs.context = nullptr;
    docs.handle = 0;
    docs.serial = 0;
    resetGraph();
}

void FlowRuntime::Impl::runPending() {
    runningPending = true;
    while (pending.kind != PendingLoad::Kind::None) {
        PendingLoad op = std::move(pending);
        pending = PendingLoad{};
        std::vector<std::string> errors;  // Nobody is left to report them to.
        switch (op.kind) {
            case PendingLoad::Kind::Load:
                // A Context-owned Document (BundleContext's UI flows) that was reloaded or unloaded
                // since the request is gone: nothing to load against.
                if (docs.context != nullptr && docs.context->documentSerial(docs.handle) != op.serial) {
                    clearAll();
                } else {
                    load(op.graph, *op.doc, errors);
                }
                break;
            case PendingLoad::Kind::LoadGlobal:
                loadGlobal(op.graph, *op.bundle, errors);
                break;
            case PendingLoad::Kind::Unload:
                clearAll();
                break;
            case PendingLoad::Kind::None:
                break;
        }
    }
    runningPending = false;
}

void FlowRuntime::Impl::resetGraph() {
    loaded = false;
    global = false;
    missingTimelines.clear();
    currentUi = nullptr;
    currentInstance = 0;
    nodes.clear();
    nodeIndexById.clear();
    edges.clear();
    rtEdges.clear();
    // T-476: the compiled graph.
    values.clear();
    strings.clear();
    slotHomeBase = 0;
    externals.clear();
    styleTemplates.clear();  // T-477.
    fanOutStart.clear();
    fanOutEdgeList.clear();
    externalKeyBase = 0;
    fanOutHomes.clear();
    sources.clear();
    batchStack.clear();
    batchHomes.clear();
    batchTop = 0;
    timedNodes.clear();
    valuesObserved = false;
    valueList.clear();
    valueNamesList.clear();
    edgeLastFiredList.clear();
    edgeFireCount.clear();
    edgeLoopReported.clear();
    runtimeErrorList.clear();
    fanOutDepth = 0;
    docs.clockAccum = 0;
    // T-389: nothing pending survives the graph (the states it points at go with it).
    pendingList.clear();
    batching = false;
    topoOrder.clear();
    nodeDirty.clear();
    dirtyNodes = 0;
    staleNodes.clear();
    nodeStale.clear();
}

Status FlowRuntime::Impl::loadGraph(const FlowGraph& graph, std::vector<std::string>& errors,
                                    const std::map<std::string, Document*>* uiMap) {
    resetGraph();
    for (const NodeDesc& nd : graph.nodes) {
        if (nodeIndexById.count(nd.id) != 0) {
            errors.push_back("duplicate node id '" + nd.id + "'");
            continue;
        }
        NodeType type;
        if (!parseNodeType(nd.type, type)) {
            errors.push_back("unknown node type '" + nd.type + "' for node '" + nd.id + "'");
            continue;
        }
        NodeRuntime rt;
        rt.type = type;
        rt.id = nd.id;
        compileNode(nd, rt);  // T-476: its slots, params, ops and format pieces.
        nodeIndexById[nd.id] = static_cast<int>(nodes.size());
        nodes.push_back(std::move(rt));
    }
    if (!errors.empty()) {
        return Status::kInvalidArgument;
    }

    std::vector<BoundEdge> boundEdges;
    Status status = validateGraph(graph, docs.doc, errors, &boundEdges, uiMap);
    if (status != Status::kOk) {
        return status;
    }
    // T-476: one string buffer per value slot (a slot's strings are copied into its own).
    slotHomeBase = static_cast<uint32_t>(strings.size());
    for (size_t i = 0; i < values.size(); ++i) {
        strings.addBuffer();
    }
    edges = std::move(boundEdges);
    edgeLastFiredList.assign(edges.size(), -1.0f);
    compileEdges();

    // Node i is graph.nodes[i] here (a duplicate id or an unknown type failed above).
    for (size_t i = 0; i < nodes.size(); ++i) {
        NodeDesc paramsOnly;
        paramsOnly.params = graph.nodes[i].params;
        nodes[i].valueListBase = static_cast<uint32_t>(valueList.size());
        for (const std::string& port : outputPortOrder(nodes[i].type)) {
            valueNamesList.push_back(nodes[i].id + "." + port);
            PortType t = PortType::Number;
            nodePortInfo(nodes[i].type, paramsOnly, port, /*isSource=*/true, t);
            valueList.push_back(typedValue(t));
        }
        if (nodes[i].type == NodeType::Clock || nodes[i].type == NodeType::Timer) {
            timedNodes.push_back(static_cast<int>(i));
        }
    }
    staleNodes.reserve(nodes.size());
    nodeStale.assign(nodes.size(), 0);

    bindDocumentEndpoints();
    loaded = true;
    seeding = true;
    seedInitialOutputs();
    seeding = false;
    return Status::kOk;
}

Status FlowRuntime::Impl::rebind(Document& document, std::vector<std::string>& warnings) {
    if (!loaded) {
        return Status::kInvalidArgument;
    }
    if (global) {
        return Status::kUnsupported;  // T-193: a global runtime is loaded again instead.
    }
    // Deliberately NOT unbindListeners(): the previous Document is normally the one that was just
    // reloaded, i.e. already destroyed, so removing listeners from it would be a use-after-free.
    // The ids are dropped and the liveness token replaced, so nothing left behind can call back in
    // through an animation "finished" callback (see rebind()'s contract in flow_runtime.h).
    unbindDocumentEndpoints(docs, /*removeListeners=*/false);
    docs.doc = &document;
    docs.serial = document.loadSerial();  // T-260c.
    // T-389: values the previous Document reported and the flow has not delivered yet (the
    // source bindings themselves are rebuilt by compileEdges() below).
    pendingList.clear();

    // Everything derived from the old tree; node state (counters, toggles, timers, valueList) is
    // kept on purpose.
    resetDocumentTreeState(docs);

    std::set<std::string> warned;
    std::vector<BoundEdge> keptEdges;
    std::vector<float> keptLastFired;
    keptEdges.reserve(edges.size());
    keptLastFired.reserve(edges.size());
    for (size_t i = 0; i < edges.size(); ++i) {
        const BoundEdge& e = edges[i];
        // Both endpoints are checked (no short-circuit), so an edge losing both ends reports both.
        const bool fromLost = documentEndpointLost(document, e.from, warnings, warned);
        const bool toLost = documentEndpointLost(document, e.to, warnings, warned);
        if (fromLost || toLost) {
            continue;
        }
        keptEdges.push_back(e);
        keptLastFired.push_back(edgeLastFiredList[i]);
    }
    edges = std::move(keptEdges);
    edgeLastFiredList = std::move(keptLastFired);
    compileEdges();
    bindDocumentEndpoints();
    return Status::kOk;
}

bool FlowRuntime::Impl::anyCoalescing() const {
    if (coalescesSignals(docs)) {
        return true;
    }
    for (const UiDoc& ui : uiDocs) {
        if (coalescesSignals(*ui.state)) {
            return true;
        }
    }
    return false;
}

void FlowRuntime::Impl::tick(float dt) {
    if (!loaded || !enabled) {
        return;
    }
    DispatchGuard dispatch(*this);  // T-260c.
    // T-475: the allocations of this tick (FrameStats::flowAllocs; 0 unless the executable counts).
    const uint64_t allocsBefore = perfStats() != nullptr ? xt::perf::AllocCounter::total() : 0;
    const uint64_t sinkAllocsBefore = sinkAllocs;  // T-478: text setStyle parses, left out.
    // One tick is one feedback-guard window (Docs/tasks/T-124.md): the counters only have to
    // catch a loop that keeps pushing without the clock ever moving on.
    std::fill(edgeFireCount.begin(), edgeFireCount.end(), 0u);
    docs.clockAccum += dt;
    for (UiDoc& ui : uiDocs) {
        ui.state->clockAccum += dt;  // T-193: each UI's `document.clock`.
    }
    // T-389 (27 §27.4.1): the whole tick is one batch. The value signals the input dispatch
    // reported since the last tick go in first (their latest values only), then the clocks,
    // timers and polled ports; every node they reach is evaluated once at the end.
    // T-407 (43 §43.3): a behavior-0 flow (no bound Document at behavior >= 1) keeps the
    // pre-T-389 tick: no batch, every push evaluated at once.
    const bool outerBatch = !batching;
    const bool coalesce = anyCoalescing();
    if (coalesce) {
        batching = true;
    }
    deliverPendingSignals();

    for (size_t t = 0; t < timedNodes.size(); ++t) {  // T-476: the clock / timer nodes only.
        const int idx = timedNodes[t];
        NodeRuntime& n = nodes[static_cast<size_t>(idx)];
        if (n.type == NodeType::Clock) {
            pushNumberOutput(idx, 0, static_cast<double>(docs.clockAccum));
        } else if (n.type == NodeType::Timer && n.timerRunning) {
            // T-258a: (0, kMinTimerInterval) runs at kMinTimerInterval; 0, negative and non-finite
            // never fire.
            const double interval = effectiveTimerInterval(paramNum(n, slots::timer::kInterval, 0.0));
            if (interval > 0.0 && std::isfinite(interval)) {
                n.timerElapsed += dt;
                // Re-checks timerRunning every iteration so a "stop" pulse delivered synchronously
                // from a "tick" listener (e.g. a one-shot timer wired tick -> its own stop) takes
                // effect immediately, even mid-burst.
                const bool repeat = paramBool(n, slots::timer::kRepeat, true);
                // T-258a: at most kMaxTimerFiresPerTick ticks per call; past that the elapsed time
                // is dropped, so a tick that falls far behind cannot spin here.
                const auto step = static_cast<float>(std::min(interval, static_cast<double>(FLT_MAX)));
                int fired = 0;
                while (n.timerRunning && n.timerElapsed >= step) {
                    if (fired++ == kMaxTimerFiresPerTick) {
                        n.timerElapsed = 0;
                        break;
                    }
                    n.timerElapsed -= step;
                    n.timerCount += 1.0;
                    if (!repeat) {
                        // `repeat: false` (T-123): one tick, then stop. Stopping before the pulse
                        // is what lets a `start` delivered from a `tick` listener restart it.
                        n.timerRunning = false;
                        n.timerElapsed = 0;
                    }
                    pushNumberOutput(idx, slots::timer::kCount, n.timerCount);
                    firePulseOutput(idx, slots::timer::kTick);
                }
            }
        }
    }

    // The polled `#document.*` and `<anim>` `localTime` connection points (flow_bindings.cpp).
    // T-389: by index, re-reading the size: a callback may load another graph meanwhile.
    {
        StageScope stage(*this, PerfStage::Poll);  // T-475.
        pollDocumentOutputs(docs);
        for (size_t i = 0; i < uiDocs.size() && !replacing(); ++i) {
            pollDocumentOutputs(*uiDocs[i].state);
        }
    }
    if (coalesce) {
        flushSignals();  // T-389: what the polls queued, and the dirty nodes.
    }
    if (outerBatch) {
        batching = false;
    }
    syncValueList();  // T-389: values() once per tick, not per push.
    if (FrameStats* stats = perfStats()) {
        stats->flowAllocs += static_cast<uint32_t>(xt::perf::AllocCounter::total() - allocsBefore -
                                                   (sinkAllocs - sinkAllocsBefore));
    }
}

void FlowRuntime::Impl::perfSwitch(PerfStage next) {
    const auto now = std::chrono::steady_clock::now();
    if (perfStage != PerfStage::None) {
        if (FrameStats* stats = perfStats()) {
            const float ms = std::chrono::duration<float, std::milli>(now - perfStageStart).count();
            switch (perfStage) {
                case PerfStage::Deliver:
                    stats->flowDeliverMs += ms;
                    break;
                case PerfStage::Eval:
                    stats->flowEvalMs += ms;
                    break;
                case PerfStage::Sink:
                    stats->flowSinkMs += ms;
                    break;
                case PerfStage::Poll:
                    stats->flowPollMs += ms;
                    break;
                case PerfStage::None:
                    break;
            }
        }
    }
    perfStage = next;
    perfStageStart = now;
}

FlowRuntime::FlowRuntime() : m_impl(std::make_unique<Impl>()) {}

FlowRuntime::~FlowRuntime() {
    // The Document outlives this runtime (load()'s contract), and every listener load() registered
    // captures `this` -- so they have to come off before Impl goes away.
    m_impl->unbindAll(/*removeListeners=*/true);
}

Status FlowRuntime::load(const FlowGraph& graph, Document& doc, std::vector<std::string>& errors) {
    return m_impl->load(graph, doc, errors);
}

Status FlowRuntime::rebind(Document& doc, std::vector<std::string>& warnings) {
    return m_impl->rebind(doc, warnings);
}

void FlowRuntime::unload() { m_impl->unload(); }

void FlowRuntime::tick(float dt) { m_impl->tick(dt); }

std::span<const PortValue> FlowRuntime::values() const {
    if (!m_impl->loaded) {
        return {};
    }
    m_impl->valuesObserved = true;  // T-476: from now on syncValueList() runs every tick.
    m_impl->syncValueList();        // T-389: pushes outside tick() (pulse events) show at once too.
    return std::span<const PortValue>(m_impl->valueList);
}

const std::vector<std::string>& FlowRuntime::valueNames() const {
    static const std::vector<std::string> kEmpty;
    if (!m_impl->loaded) {
        return kEmpty;
    }
    m_impl->valuesObserved = true;  // T-476: an observer of the value table.
    return m_impl->valueNamesList;
}

const std::vector<std::string>& FlowRuntime::runtimeErrors() const {
    static const std::vector<std::string> kEmpty;
    return m_impl->loaded ? m_impl->runtimeErrorList : kEmpty;
}

std::span<const float> FlowRuntime::edgeLastFired() const {
    return m_impl->loaded ? std::span<const float>(m_impl->edgeLastFiredList) : std::span<const float>{};
}

// ---- T-193 ----------------------------------------------------------------------------------------

std::span<const std::string_view> systemEventKinds() {
    static constexpr std::string_view kKinds[] = {
        "loading",  "paused",      "resumed", "disconnected", "languageChanged",
        "platform", "inputDevice", "back",    "reducedMotion"};  // T-240, T-330.
    return kKinds;
}

Status FlowRuntime::loadGlobal(std::string_view json, BundleContext& bundle,
                               std::vector<std::string>& errors) {
    FlowGraph graph;
    Status parsed = FlowGraph::parse(json, graph, errors);
    if (parsed != Status::kOk) {
        return parsed;
    }
    return loadGlobal(graph, bundle, errors);
}

Status FlowRuntime::loadGlobal(const FlowGraph& graph, BundleContext& bundle,
                               std::vector<std::string>& errors) {
    return m_impl->loadGlobal(graph, bundle, errors);
}

bool FlowRuntime::isGlobal() const { return m_impl->loaded && m_impl->global; }

Status FlowRuntime::postSystemEvent(std::string_view kind, std::string_view value) {
    return m_impl->postSystemEvent(kind, value);
}

void FlowRuntime::setHostEventListener(FlowHostEventListener listener) {
    m_impl->hostListener = std::move(listener);
}

void FlowRuntime::setEnabled(bool enabled) { m_impl->enabled = enabled; }

bool FlowRuntime::enabled() const { return m_impl->enabled; }

void FlowRuntime::setPerfTracker(PerfTracker* tracker) { m_impl->perfTracker = tracker; }

}  // namespace xt::flow
