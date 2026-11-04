#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "flow_internal.h"
#include "xt/context.h"
#include "xt/flow/bundle_context.h"
#include "xt/property.h"

/// T-582 (Docs/design/53-flow-language.md §53.5.5 "late binding"): the global flow binds a UI
/// registered after it was loaded (BundleContext::addUi()), and a UI removed and registered again,
/// without a reload and without touching the node state. loadGlobal() leaves the `ui#` endpoints
/// of a UI not registered yet dormant (validateGraph()'s `dormantUis`); bindUi() makes the checks
/// that need the UI's Document, gives the instance its endpoint state and subscribes it.

namespace xt::flow {

namespace {

/// True for a document endpoint (not a node port, not a lifecycle output) of UI `uiId`.
bool onUi(const Endpoint& ep, const std::string& uiId) {
    return ep.uiId == uiId && ep.owner != Endpoint::Owner::Node && ep.owner != Endpoint::Owner::Lifecycle;
}

/// The connection point id resolveDocumentEndpoint() takes for `ep` ("document" for row 3).
std::string endpointId(const Endpoint& ep) {
    return ep.owner == Endpoint::Owner::Doc ? std::string("document") : ep.elementId;
}

}  // namespace

void FlowRuntime::Impl::checkUiEndpoints(const std::string& uiId, Document& doc,
                                         std::vector<std::string>& errors,
                                         std::vector<std::pair<Endpoint, Endpoint>>& fresh) {
    fresh.clear();
    fresh.reserve(edges.size());
    for (size_t i = 0; i < edges.size(); ++i) {
        const BoundEdge& e = edges[i];
        const std::string& fromText = edgeTexts[i].first;
        const std::string& toText = edgeTexts[i].second;
        Endpoint from = e.from;
        Endpoint to = e.to;
        bool ok = true;
        bool touched = false;
        for (int side = 0; side < 2; ++side) {
            Endpoint& ep = side == 0 ? from : to;
            if (!onUi(ep, uiId)) {
                continue;
            }
            Endpoint resolved;
            std::string err;
            if (!resolveDocumentEndpoint(endpointId(ep), ep.port, /*isSource=*/side == 0, &doc, resolved,
                                         err)) {
                errors.push_back(err);
                ok = false;
                continue;
            }
            resolved.uiId = uiId;
            // An endpoint typed already (by the load or an earlier instance) must keep its type.
            if (!ep.anyType && resolved.type != ep.type) {
                errors.push_back("type mismatch on edge '" + fromText + "' -> '" + toText + "'");
                ok = false;
                continue;
            }
            ep = std::move(resolved);
            touched = true;
        }
        if (ok && touched) {
            std::string err;
            if (!checkEdgeTypes(from, to, fromText, toText, err)) {
                errors.push_back(err);
                ok = false;
            }
        }
        if (ok && touched && onUi(to, uiId) && to.port == "setProperty" && isEffectPropText(e.param)) {
            std::string effectErr;  // T-547: as validateGraph(), against this instance's Document.
            PropId resolved = prop::kInvalidProp;
            if (!resolveEffectProp(doc, doc.byId(to.elementId), e.param, effectErr, resolved)) {
                errors.push_back("#" + to.elementId + ".setProperty: " + effectErr);
            }
        }
        fresh.emplace_back(std::move(from), std::move(to));
    }
    // The fan-in of a declared value input that had no type at load (validateGraph() leaves it to
    // the Document, T-560): one incoming edge per instance filter, as validateGraph() counts.
    std::map<std::string, int> fanIn;
    for (size_t i = 0; i < fresh.size(); ++i) {
        const Endpoint& to = fresh[i].second;
        if (!onUi(to, uiId) || to.owner != Endpoint::Owner::Port || !edges[i].to.anyType ||
            to.type == PortType::Pulse) {
            continue;
        }
        std::string identity = to.elementId + ":" + to.port;
        if (edges[i].instance >= 0) {
            identity += "|i" + std::to_string(edges[i].instance);
        }
        if (++fanIn[identity] == 2) {
            errors.push_back("value input '" + edgeTexts[i].second + "' has more than one incoming edge");
        }
    }
}

void FlowRuntime::Impl::bindUi(const std::string& uiId, Context& context, DocumentHandle handle) {
    if (dispatchDepth > 0) {
        // Inside this runtime's own dispatch (a host callback): the edge and endpoint tables are
        // being walked further up the stack, so the binding waits until the dispatch returns.
        deferredBinds.push_back(DeferredBind{uiId, &context, handle, context.documentSerial(handle)});
        return;
    }
    if (!loaded || !global || replacing()) {
        return;  // No global flow (yet): loadGlobal() binds every registered instance.
    }
    Document* doc = context.document(handle);
    if (doc == nullptr) {
        return;
    }
    for (const UiDoc& ui : uiDocs) {
        if (ui.uiId == uiId && ui.handle == handle && ui.state->context == &context &&
            liveDocument(*ui.state) == doc) {
            return;  // Bound already (loadGlobal() ran after the addUi() asked for this).
        }
    }
    std::vector<std::string> errors;
    std::vector<std::pair<Endpoint, Endpoint>> fresh;
    checkUiEndpoints(uiId, *doc, errors, fresh);
    if (!errors.empty()) {
        // 53 §53.5.5: the binding fails, the instance stays dormant, the errors are reported.
        runtimeErrorList.insert(runtimeErrorList.end(), errors.begin(), errors.end());
        return;
    }
    // The endpoints that had no type at load (a dormant UI's declared ports) take this Document's.
    bool retyped = false;
    for (size_t i = 0; i < edges.size(); ++i) {
        for (int side = 0; side < 2; ++side) {
            Endpoint& stored = side == 0 ? edges[i].from : edges[i].to;
            if (!onUi(stored, uiId) || !stored.anyType) {
                continue;
            }
            stored = side == 0 ? fresh[i].first : fresh[i].second;
            SlotEndpoint& slot = side == 0 ? rtEdges[i].from : rtEdges[i].to;
            slot.type = stored.type;
            ExternalSlotInfo& external = externals[slot.external];
            external.desc = stored;
            external.valueSignal = isValueSignal(stored.owner, stored.port, stored.type);
            retyped = true;
        }
    }
    if (retyped) {
        computeContinuityWarnings();
    }

    UiDoc ui;
    ui.uiId = uiId;
    ui.handle = handle;
    ui.state = std::make_unique<DocEndpointState>();
    DocEndpointState* state = ui.state.get();
    state->doc = doc;
    state->context = &context;  // T-260b: liveDocument() re-resolves `handle`.
    state->handle = handle;
    state->serial = doc->loadSerial();  // T-260c.
    state->output = [this, state](uint32_t slot, const PortValue& v) { signalOutput(*state, slot, v); };
    for (uint32_t x = 0; x < externals.size(); ++x) {
        const Endpoint& ep = externals[x].desc;
        if (ep.owner == Endpoint::Owner::Lifecycle && ep.uiId == uiId) {
            (ep.port == "shown" ? ui.shownExternal : ui.hiddenExternal) = x;
        }
    }
    uiDocs.push_back(std::move(ui));
    bindUiState(uiId, *state);
    resendCurrentValues(uiId, *state);  // T-583.
}

void FlowRuntime::Impl::bindUiState(const std::string& uiId, DocEndpointState& state) {
    // bindDocumentEndpoints() for one endpoint state, in the same order.
    for (const BoundEdge& e : edges) {
        for (const Endpoint* ep : {&e.from, &e.to}) {
            if (ep->uiId == uiId) {
                registerDocumentEndpoint(state, *ep);
            }
        }
    }
    for (size_t i = 0; i < edges.size(); ++i) {
        const Endpoint& from = edges[i].from;
        if (from.uiId == uiId &&
            (from.owner == Endpoint::Owner::Element || from.owner == Endpoint::Owner::Anim ||
             from.owner == Endpoint::Owner::Port)) {
            bindSource(state, subscribeDocumentOutput(state, from), rtEdges[i].from.external);
        }
    }
    for (size_t i = 0; i < edges.size(); ++i) {
        const Endpoint& from = edges[i].from;
        if (from.uiId == uiId && from.owner == Endpoint::Owner::Doc) {
            bindSource(state, subscribeDocumentOutput(state, from), rtEdges[i].from.external);
        }
    }
    // As bindDocumentEndpoints(): queueing and delivering a frame's signals never allocates.
    pendingList.reserve(sources.size());
    while (batchStack.size() < sources.size()) {
        batchStack.emplace_back();
        batchHomes.push_back(strings.addBuffer());
    }
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].type == NodeType::Timeline && nodes[i].uiId == uiId) {
            bindTimelineNode(i, state);
        }
    }
}

void FlowRuntime::Impl::resendCurrentValues(const std::string& uiId, DocEndpointState& state) {
    if (!enabled || replacing()) {
        return;  // As fanOut(): nothing propagates while the flow is off or on its way out.
    }
    // A sink write can raise document events that enter the runtime: a load they ask for waits.
    DispatchGuard dispatch(*this);
    for (size_t i = 0; i < edges.size(); ++i) {
        if (replacing()) {
            break;
        }
        const BoundEdge& e = edges[i];
        const RtEdge& edge = rtEdges[i];
        // Only the edges bound just now: a value-type sink on this UI, fed from outside it.
        if (!onUi(e.to, uiId) || edge.to.type == PortType::Pulse || edge.from.type == PortType::Pulse ||
            e.from.uiId == uiId || edge.from.owner == Endpoint::Owner::Lifecycle) {
            continue;
        }
        const RtValue* current = nullptr;
        if (edge.from.owner == Endpoint::Owner::Node) {
            if (edge.from.slot == kNoSlot) {
                continue;
            }
            current = &values[nodes[static_cast<size_t>(edge.from.node)].outputBase + edge.from.slot];
        } else {
            const ExternalSlotInfo& info = externals[edge.from.external];
            // An `instance` edge only carries the events of that source instance (as fanOut()).
            if (edge.instance >= 0 && static_cast<int64_t>(info.lastInstance) != edge.instance) {
                continue;
            }
            current = &info.last;
        }
        if (current->type == kNoValue || current->type == PortType::Pulse) {
            continue;  // No value yet: nothing to resend.
        }
        edgeLastFiredList[i] = docs.clockAccum;
        applyToSink(static_cast<uint32_t>(i), *current, &state);
    }
}

void FlowRuntime::Impl::runDeferredBinds() {
    std::vector<DeferredBind> binds;
    binds.swap(deferredBinds);
    for (const DeferredBind& bind : binds) {
        if (globalBundle == nullptr) {
            return;
        }
        // Still registered (no removeUi() since), with the same Document (no reload since).
        bool registered = false;
        for (const BundleContext::Instance& instance : globalBundle->instances(bind.uiId)) {
            registered = registered || (instance.context == bind.context && instance.handle == bind.handle);
        }
        if (registered && bind.context->documentSerial(bind.handle) == bind.serial) {
            bindUi(bind.uiId, *bind.context, bind.handle);
        }
    }
}

}  // namespace xt::flow
