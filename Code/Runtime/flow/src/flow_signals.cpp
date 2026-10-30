#include <algorithm>
#include <string>
#include <utility>

#include "flow_internal.h"

/// T-476 (Docs/design/27-interaction.md §27.4.2 items 1-3, 5): the propagation machinery of a
/// compiled flow -- edge compilation (external slots, CSR fan-out, topological order), fan-out
/// with the T-124 feedback guard, the T-389 per-frame signal batch over source bindings, and the
/// values() copy -- all on slot indices, without heap allocation in steady state.

namespace xt::flow {

namespace {

/// Docs/tasks/T-124.md: how often one edge may fire inside a single tick window before it is
/// treated as a feedback loop, and how deep fanOut() may nest. Node-to-node cycles are rejected at
/// load time, so a loop can only run through the document (a node writing a value back into an
/// element whose own output port then feeds the node again) -- which recurses, hence the second,
/// much lower cap: it is what actually keeps the stack from overflowing.
constexpr uint32_t kMaxEdgeFiresPerTick = 1000;
constexpr uint32_t kMaxFanOutDepth = 256;

/// How many deliver-and-settle rounds one flushSignals() / settleNodes() runs. One is the normal
/// case; more only happen when a sink makes the Document report another value (a loop through the
/// document), which then waits for the next tick instead of spinning here.
constexpr int kMaxSignalRounds = 16;

}  // namespace

// ---- RtValue helpers ---------------------------------------------------------------------------

void FlowRuntime::Impl::assignValue(uint32_t home, RtValue& dst, const RtValue& src) {
    if (src.type == PortType::String && src.str != home) {
        strings.buffer(home).assign(strings.text(src.str));
    }
    dst = src;
    dst.str = home;
}

void FlowRuntime::Impl::assignFromPort(uint32_t home, RtValue& dst, const PortValue& src) {
    dst.type = src.type;
    dst.b = src.b;
    dst.n = src.n;
    dst.str = home;
    if (src.type == PortType::String) {
        strings.buffer(home).assign(src.s);
    }
}

PortValue FlowRuntime::Impl::toPortValue(const RtValue& v) const {
    PortValue out;
    out.type = v.type == kNoValue ? PortType::Pulse : v.type;
    out.b = v.b;
    out.n = v.n;
    if (v.type == PortType::String) {
        out.s = strings.text(v.str);
    }
    return out;
}

bool FlowRuntime::Impl::rtEqual(const RtValue& a, const RtValue& b) const {
    if (a.type != b.type) {
        return false;
    }
    switch (a.type) {
        case PortType::Bool:
            return a.b == b.b;
        case PortType::Number:
            return a.n == b.n;  // NOLINT: exact comparison is the documented contract.
        case PortType::String:
            return a.str == b.str || strings.text(a.str) == strings.text(b.str);
        default:
            return true;  // Pulse, or both without a value.
    }
}

// ---- Compilation -------------------------------------------------------------------------------

void FlowRuntime::Impl::compileEdges() {
    rtEdges.clear();
    externals.clear();
    sources.clear();
    pendingList.clear();
    batchTop = 0;
    for (UiDoc& ui : uiDocs) {
        ui.shownExternal = kNoOutput;
        ui.hiddenExternal = kNoOutput;
    }
    if (fanOutHomes.empty()) {
        for (uint32_t depth = 0; depth <= kMaxFanOutDepth; ++depth) {
            fanOutHomes.push_back(strings.addBuffer());
        }
    }

    // One external slot per distinct document endpoint (load time: string keys are fine here).
    std::map<std::string, uint32_t> externalIndex;
    auto externalOf = [&](const Endpoint& ep) {
        const std::string key = ep.uiId + "|" + externalKey(ep.owner, ep.elementId, ep.port);
        auto it = externalIndex.find(key);
        if (it != externalIndex.end()) {
            return it->second;
        }
        const auto slot = static_cast<uint32_t>(externals.size());
        ExternalSlotInfo info;
        info.desc = ep;
        info.valueSignal = isValueSignal(ep.owner, ep.port, ep.type);
        externals.push_back(std::move(info));
        externalIndex.emplace(key, slot);
        return slot;
    };
    auto compileEndpoint = [&](const Endpoint& ep, bool isSource) {
        SlotEndpoint out;
        out.owner = ep.owner;
        out.type = ep.type;
        if (ep.owner == Endpoint::Owner::Node) {
            out.node = ep.nodeIndex;
            out.slot = slotOf(ep.nodeType, isSource ? SlotKind::Output : SlotKind::Input, ep.port);
        } else {
            out.external = externalOf(ep);
        }
        return out;
    };
    rtEdges.reserve(edges.size());
    for (const BoundEdge& e : edges) {
        RtEdge edge;
        edge.from = compileEndpoint(e.from, /*isSource=*/true);
        edge.to = compileEndpoint(e.to, /*isSource=*/false);
        edge.instance = e.instance;
        rtEdges.push_back(edge);
    }
    for (uint32_t x = 0; x < externals.size(); ++x) {
        const Endpoint& ep = externals[x].desc;
        if (ep.owner != Endpoint::Owner::Lifecycle) {
            continue;
        }
        for (UiDoc& ui : uiDocs) {
            if (ui.uiId == ep.uiId) {
                (ep.port == "shown" ? ui.shownExternal : ui.hiddenExternal) = x;
            }
        }
    }

    // CSR fan-out over the keys "node output value slot" and externalKeyBase + external slot.
    externalKeyBase = static_cast<uint32_t>(values.size());
    const size_t keys = values.size() + externals.size();
    fanOutStart.assign(keys + 1, 0u);
    std::vector<uint32_t> edgeKey(rtEdges.size(), UINT32_MAX);
    for (size_t i = 0; i < rtEdges.size(); ++i) {
        const SlotEndpoint& from = rtEdges[i].from;
        if (from.owner == Endpoint::Owner::Node) {
            if (from.node < 0 || static_cast<size_t>(from.node) >= nodes.size() || from.slot == kNoSlot) {
                continue;  // Not a port of the slot tables (validateGraph() never lets one through).
            }
            edgeKey[i] = nodes[static_cast<size_t>(from.node)].outputBase + from.slot;
        } else {
            edgeKey[i] = externalKeyBase + from.external;
        }
        ++fanOutStart[edgeKey[i] + 1];
    }
    for (size_t k = 0; k < keys; ++k) {
        fanOutStart[k + 1] += fanOutStart[k];
    }
    fanOutEdgeList.assign(fanOutStart[keys], 0u);
    std::vector<uint32_t> fill(fanOutStart.begin(), fanOutStart.end() - 1);
    for (size_t i = 0; i < rtEdges.size(); ++i) {  // Edge order within each key.
        if (edgeKey[i] != UINT32_MAX) {
            fanOutEdgeList[fill[edgeKey[i]]++] = static_cast<uint32_t>(i);
        }
    }
    edgeFireCount.assign(edges.size(), 0u);
    edgeLoopReported.assign(edges.size(), false);
    fanOutDepth = 0;
    compileStyleTemplates();

    // T-389: the node evaluation order of a batch (Kahn over the node-to-node edges, which
    // validateGraph() keeps acyclic; anything left over goes last, in index order).
    const size_t count = nodes.size();
    std::vector<uint32_t> indegree(count, 0);
    std::vector<std::vector<int>> next(count);
    for (const BoundEdge& e : edges) {
        if (e.from.owner == Endpoint::Owner::Node && e.to.owner == Endpoint::Owner::Node &&
            e.from.nodeIndex >= 0 && e.to.nodeIndex >= 0 && static_cast<size_t>(e.from.nodeIndex) < count &&
            static_cast<size_t>(e.to.nodeIndex) < count) {
            next[static_cast<size_t>(e.from.nodeIndex)].push_back(e.to.nodeIndex);
            ++indegree[static_cast<size_t>(e.to.nodeIndex)];
        }
    }
    topoOrder.clear();
    topoOrder.reserve(count);
    std::vector<uint8_t> placed(count, 0);
    for (size_t i = 0; i < count; ++i) {
        if (indegree[i] == 0) {
            topoOrder.push_back(static_cast<int>(i));
            placed[i] = 1;
        }
    }
    for (size_t head = 0; head < topoOrder.size(); ++head) {
        for (int to : next[static_cast<size_t>(topoOrder[head])]) {
            if (--indegree[static_cast<size_t>(to)] == 0 && placed[static_cast<size_t>(to)] == 0) {
                topoOrder.push_back(to);
                placed[static_cast<size_t>(to)] = 1;
            }
        }
    }
    for (size_t i = 0; i < count; ++i) {
        if (placed[i] == 0) {
            topoOrder.push_back(static_cast<int>(i));
        }
    }
    nodeDirty.assign(count, 0);
    dirtyNodes = 0;
}

void FlowRuntime::Impl::compileStyleTemplates() {
    // T-477 (27 §27.4.2 item 4): a `format` / `toString` output wired to `setStyle` sends texts
    // of one shape -- the format's literal pieces around its placeholders. When that shape
    // compiles, the sink writes the declarations from the template instead of parsing each text
    // (ElementRef::setStyleTemplate(); a text of another shape still goes through setStyle()).
    styleTemplates.clear();
    std::vector<int32_t> templateOfNode(nodes.size(), -2);  // -2: not tried yet; -1: no template.
    std::vector<std::string_view> literals;
    for (size_t i = 0; i < rtEdges.size(); ++i) {
        const RtEdge& edge = rtEdges[i];
        if (edge.from.owner != Endpoint::Owner::Node || edge.to.owner != Endpoint::Owner::Element ||
            edge.from.node < 0 || static_cast<size_t>(edge.from.node) >= nodes.size() ||
            edge.from.slot != slots::format::kOut || externals[edge.to.external].desc.port != "setStyle") {
            continue;
        }
        const NodeRuntime& n = nodes[static_cast<size_t>(edge.from.node)];
        if (n.type != NodeType::Format && n.type != NodeType::ToString) {
            continue;
        }
        int32_t& index = templateOfNode[static_cast<size_t>(edge.from.node)];
        if (index == -2) {
            // The literals between the placeholders (adjacent literal pieces joined; a format
            // without a placeholder is one literal, a constant text).
            literals.clear();
            size_t from = 0;
            size_t length = 0;
            bool open = false;
            for (const FormatPiece& piece : n.pieces) {
                if (piece.input < 0) {
                    if (!open) {
                        from = piece.offset;
                        length = 0;
                        open = true;
                    }
                    length = piece.offset + piece.length - from;
                    continue;
                }
                literals.push_back(open ? std::string_view(n.formatText).substr(from, length)
                                        : std::string_view());
                open = false;
            }
            literals.push_back(open ? std::string_view(n.formatText).substr(from, length)
                                    : std::string_view());
            StyleTemplate compiled;
            if (compileStyleTemplate(literals, compiled)) {
                index = static_cast<int32_t>(styleTemplates.size());
                styleTemplates.push_back(std::move(compiled));
            } else {
                index = -1;
            }
        }
        rtEdges[i].styleTemplate = index;
    }
}

// ---- Fan-out and the feedback guard (Docs/tasks/T-124.md) --------------------------------------

std::string FlowRuntime::Impl::endpointName(const Endpoint& ep) const {
    if (ep.owner == Endpoint::Owner::Node) {
        const bool known = ep.nodeIndex >= 0 && static_cast<size_t>(ep.nodeIndex) < nodes.size();
        return (known ? nodes[static_cast<size_t>(ep.nodeIndex)].id : std::string("?")) + "." + ep.port;
    }
    if (ep.owner == Endpoint::Owner::Lifecycle) {
        return "ui#" + ep.uiId + "." + ep.port;
    }
    const std::string local = (ep.elementId.empty() ? std::string("document") : ep.elementId) + "." + ep.port;
    return ep.uiId.empty() ? "#" + local : "ui#" + ep.uiId + "." + local;
}

void FlowRuntime::Impl::reportFeedbackLoop(size_t edgeIndex, const char* reason) {
    if (edgeIndex >= edgeLoopReported.size() || edgeLoopReported[edgeIndex]) {
        return;
    }
    edgeLoopReported[edgeIndex] = true;
    const BoundEdge& e = edges[edgeIndex];
    runtimeErrorList.push_back("flow feedback loop: edge '" + endpointName(e.from) + "' -> '" +
                               endpointName(e.to) + "' " + reason);
}

void FlowRuntime::Impl::fanOut(uint32_t key, const RtValue& v) {
    if (!enabled || replacing()) {
        return;  // T-193 (13.8.3 enableFlow(false)): nothing propagates. T-260c: nor once replaced.
    }
    // T-260c: every document, timeline and lifecycle event enters the runtime here; a load asked
    // for from a callback below runs when the outermost one returns.
    DispatchGuard dispatch(*this);
    if (static_cast<size_t>(key) + 1 >= fanOutStart.size()) {
        return;
    }
    const uint32_t begin = fanOutStart[key];
    const uint32_t end = fanOutStart[key + 1];
    if (begin == end) {
        return;
    }
    if (fanOutDepth >= kMaxFanOutDepth) {
        reportFeedbackLoop(fanOutEdgeList[begin], "nested deeper than 256 pushes; propagation stopped");
        return;
    }
    ++fanOutDepth;
    // This level's own copy: a sink can re-enter through a document event and push the source
    // again before the loop below reaches its last edge. (The CSR arrays only change in load() /
    // rebind(), never during a dispatch, so the indices stay valid.)
    RtValue local = v;
    if (local.type == PortType::String) {
        const uint32_t home = fanOutHomes[fanOutDepth];
        if (local.str != home) {
            strings.buffer(home).assign(strings.text(local.str));
            local.str = home;
        }
    }
    for (uint32_t k = begin; k < end; ++k) {
        if (replacing()) {
            break;  // T-260c: a callback asked for another graph; the rest of this one stops.
        }
        const uint32_t i = fanOutEdgeList[k];
        // T-193 (27.3.1): an `instance` edge from a document port only carries that instance's
        // events (the instance of the event propagating right now, see deliverExternal()).
        const RtEdge& edge = rtEdges[i];
        if (edge.instance >= 0 && edge.from.owner != Endpoint::Owner::Node &&
            static_cast<int64_t>(currentInstance) != edge.instance) {
            continue;
        }
        if (++edgeFireCount[i] > kMaxEdgeFiresPerTick) {
            reportFeedbackLoop(i, "fired more than 1000 times in one tick; propagation stopped");
            continue;
        }
        // load()'s initial seeding is not a delivery (FlowRuntime::edgeLastFired()): it only
        // establishes each node's starting output, before any runtime time has passed.
        if (!seeding) {
            edgeLastFiredList[i] = docs.clockAccum;
        }
        perfCount(&FrameStats::flowFanOutEdges);  // T-475.
        applyToSink(i, local);
    }
    --fanOutDepth;
}

// ---- T-389 (Docs/design/27-interaction.md §27.4.1): value signals coalesced per frame ----------

void FlowRuntime::Impl::signalOutput(DocEndpointState& state, uint32_t slot, const PortValue& v) {
    perfCount(&FrameStats::flowSignals);  // T-475.
    if (slot >= state.outputs.size() || state.outputs[slot].tag >= sources.size()) {
        return;  // Not one of this graph's sources (nothing is wired to it).
    }
    const uint32_t binding = state.outputs[slot].tag;
    if (sources[binding].valueSignal && coalescesSignals(state)) {  // T-407: behavior >= 1.
        queueSignal(binding, v);
        return;
    }
    if (perfTracker != nullptr) {  // T-475: timed, off the untimed path's stack frame.
        deliverNowTimed(binding, v);
        return;
    }
    // A pulse / event: the values reported before it are delivered first, so it sees them.
    if (!pendingList.empty()) {
        flushSignals();
    }
    SourceBinding& source = sources[binding];
    RtValue now;
    assignFromPort(source.nowHome, now, v);
    deliverExternal(source.external, source.state, now);
}

void FlowRuntime::Impl::deliverNowTimed(uint32_t binding, const PortValue& v) {
    StageScope stage(*this, PerfStage::Deliver);
    if (!pendingList.empty()) {
        flushSignals();
    }
    SourceBinding& source = sources[binding];
    RtValue now;
    assignFromPort(source.nowHome, now, v);
    deliverExternal(source.external, source.state, now);
}

void FlowRuntime::Impl::queueSignal(uint32_t binding, const PortValue& v) {
    SourceBinding& source = sources[binding];
    if (source.pending != 0) {
        perfCount(&FrameStats::flowSignalsCoalesced);                // T-475.
        assignFromPort(source.pendingHome, source.pendingValue, v);  // Only the frame's latest counts.
        return;
    }
    assignFromPort(source.pendingHome, source.pendingValue, v);
    source.pending = 1;
    pendingList.push_back(binding);  // Reserved for every binding: no allocation.
}

void FlowRuntime::Impl::deliverExternal(uint32_t external, DocEndpointState* state, const RtValue& v) {
    const std::string* savedUi = currentUi;
    const uint32_t savedInstance = currentInstance;
    if (state == &docs) {
        // The load() Document: only the instance id is swapped in, the UI stays empty.
        currentInstance = docs.doc != nullptr ? docs.doc->instanceId() : 0;
    } else {
        currentUi = &externals[external].desc.uiId;
        // T-260b: a lifecycle event can come from a handle whose Document was replaced since the
        // runtime bound it (its old Document is gone): liveDocument(), not `state->doc`.
        const Document* doc = state != nullptr ? liveDocument(*state) : nullptr;
        currentInstance = doc != nullptr ? doc->instanceId() : 0;
    }
    fanOut(externalKeyBase + external, v);
    currentUi = savedUi;
    currentInstance = savedInstance;
}

void FlowRuntime::Impl::evaluateNodeTimed(int nodeIndex) {
    StageScope stage(*this, PerfStage::Eval);
    evaluateNode(nodeIndex);
}

void FlowRuntime::Impl::deliverPendingSignals() {
    if (pendingList.empty()) {
        return;
    }
    DispatchGuard dispatch(*this);                // T-260c: a callback's load waits until the batch is done.
    StageScope stage(*this, PerfStage::Deliver);  // T-475.
    // The batch moves onto batchStack (its values copied out of the pending slots), so a value
    // reported again while it is delivered waits for the next round, and a nested flush stacks
    // its own batch on top.
    const size_t start = batchTop;
    for (uint32_t binding : pendingList) {
        if (batchTop == batchStack.size()) {  // Only past the bound reserved in bindDocumentEndpoints().
            batchStack.emplace_back();
            batchHomes.push_back(strings.addBuffer());
        }
        SourceBinding& source = sources[binding];
        BatchEntry& entry = batchStack[batchTop];
        entry.binding = binding;
        assignValue(batchHomes[batchTop], entry.value, source.pendingValue);
        source.pending = 0;
        ++batchTop;
    }
    pendingList.clear();
    const size_t end = batchTop;
    const bool outer = !batching;
    batching = true;
    for (size_t k = start; k < end; ++k) {
        if (replacing()) {
            break;
        }
        const BatchEntry entry = batchStack[k];  // A copy: a nested flush may grow the stack.
        const SourceBinding& source = sources[entry.binding];
        if (liveDocument(*source.state) == nullptr) {
            continue;  // T-260b: reloaded / unloaded / removed since it reported the value.
        }
        deliverExternal(source.external, source.state, entry.value);
    }
    batchTop = start;
    if (outer) {
        batching = false;
    }
}

void FlowRuntime::Impl::flushSignals() {
    DispatchGuard dispatch(*this);
    const bool outer = !batching;
    batching = true;
    for (int round = 0; round < kMaxSignalRounds && !replacing(); ++round) {
        deliverPendingSignals();
        settleNodes();
        if (pendingList.empty()) {
            break;
        }
    }
    if (outer) {
        batching = false;  // A node left dirty past the round limit is settled by the next tick.
    }
}

void FlowRuntime::Impl::markNodeDirty(int nodeIndex) {
    const size_t i = static_cast<size_t>(nodeIndex);
    if (i < nodeDirty.size() && nodeDirty[i] == 0) {
        nodeDirty[i] = 1;
        ++dirtyNodes;
    }
}

void FlowRuntime::Impl::settleNodes() {
    StageScope stage(*this, PerfStage::Eval);  // T-475.
    for (int round = 0; round < kMaxSignalRounds && dirtyNodes > 0; ++round) {
        // By index into topoOrder: an evaluation only dirties nodes after it (node-to-node edges
        // are acyclic), except through the document, which the next round picks up.
        for (size_t k = 0; k < topoOrder.size() && dirtyNodes > 0; ++k) {
            if (replacing()) {
                return;
            }
            const size_t i = static_cast<size_t>(topoOrder[k]);
            if (i >= nodeDirty.size() || nodeDirty[i] == 0) {
                continue;
            }
            nodeDirty[i] = 0;
            --dirtyNodes;
            evaluateNode(static_cast<int>(i));  // Inside settleNodes()' Eval stage (T-475).
        }
    }
}

// ---- values() (Docs/tasks/T-56.md) -------------------------------------------------------------

void FlowRuntime::Impl::markValuesStale(int nodeIndex) {
    const size_t i = static_cast<size_t>(nodeIndex);
    if (i < nodeStale.size() && nodeStale[i] == 0) {
        nodeStale[i] = 1;
        staleNodes.push_back(nodeIndex);  // Reserved for every node: no allocation.
    }
}

void FlowRuntime::Impl::syncValueList() {
    // T-476 (27 §27.4.2 item 5): nobody has looked yet -- the stale list keeps what to copy.
    if (!valuesObserved) {
        return;
    }
    // Only the nodes pushed since the last call: the cost follows the work, not the graph size.
    for (int nodeIndex : staleNodes) {
        const size_t i = static_cast<size_t>(nodeIndex);
        if (i >= nodes.size()) {
            continue;
        }
        nodeStale[i] = 0;
        const NodeRuntime& n = nodes[i];
        for (uint32_t k = 0; k < n.outputCount; ++k) {
            const RtValue& v = values[n.outputBase + k];
            const size_t entry = static_cast<size_t>(n.valueListBase) + k;
            if (v.type == kNoValue || entry >= valueList.size()) {
                continue;  // Never pushed (a pulse output): keeps its typed default.
            }
            PortValue& out = valueList[entry];
            out.type = v.type;
            out.b = v.b;
            out.n = v.n;
            if (v.type == PortType::String) {
                out.s = strings.text(v.str);
            } else {
                out.s.clear();
            }
        }
    }
    staleNodes.clear();
}

}  // namespace xt::flow
