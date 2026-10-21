#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <utility>

#include "flow_internal.h"
#include "xt/charconv.h"
#include "xt/num_guard.h"

/// The fourteen signal node types' runtime behavior (docs/design/17-editor.md §17.6's table,
/// T-123 adding radio/lookup/merge/format to docs/tasks/T-56.md's ten), plus the generic
/// value/pulse propagation machinery (FlowRuntime::Impl::fanOut/applyToSink et al.) they and the
/// document connection-point bindings (flow_bindings.cpp) both use.

namespace xt::flow {

double paramNum(const NodeRuntime& n, const std::string& name, double def) {
    auto it = n.params.find(name);
    if (it == n.params.end()) {
        return def;
    }
    if (it->second.type == PortType::Number) {
        return it->second.n;
    }
    if (it->second.type == PortType::Bool) {
        return it->second.b ? 1.0 : 0.0;
    }
    return def;
}

bool paramBool(const NodeRuntime& n, const std::string& name, bool def) {
    auto it = n.params.find(name);
    if (it == n.params.end()) {
        return def;
    }
    if (it->second.type == PortType::Bool) {
        return it->second.b;
    }
    if (it->second.type == PortType::Number) {
        return it->second.n != 0.0;
    }
    return def;
}

std::string paramStr(const NodeRuntime& n, const std::string& name, const std::string& def) {
    auto it = n.params.find(name);
    if (it == n.params.end() || it->second.type != PortType::String) {
        return def;
    }
    return it->second.s;
}

bool valuesEqual(const PortValue& a, const PortValue& b) {
    if (a.type != b.type) {
        return false;
    }
    switch (a.type) {
        case PortType::Pulse:
            return true;
        case PortType::Bool:
            return a.b == b.b;
        case PortType::Number:
            return a.n == b.n;  // NOLINT: exact comparison is the documented contract.
        case PortType::String:
            return a.s == b.s;
    }
    return false;
}

bool compareValues(const PortValue& a, const PortValue& b, const std::string& op) {
    if (a.type == PortType::String && b.type == PortType::String) {
        if (op == "eq") return a.s == b.s;
        if (op == "ne") return a.s != b.s;
        if (op == "lt") return a.s < b.s;
        if (op == "le") return a.s <= b.s;
        if (op == "gt") return a.s > b.s;
        if (op == "ge") return a.s >= b.s;
        return false;
    }
    double av = a.type == PortType::Bool ? (a.b ? 1.0 : 0.0) : a.n;
    double bv = b.type == PortType::Bool ? (b.b ? 1.0 : 0.0) : b.n;
    if (op == "eq") return av == bv;
    if (op == "ne") return av != bv;
    if (op == "lt") return av < bv;
    if (op == "le") return av <= bv;
    if (op == "gt") return av > bv;
    if (op == "ge") return av >= bv;
    return false;
}

std::string formatPortValue(const PortValue& v, const std::map<std::string, PortValue>& params) {
    switch (v.type) {
        case PortType::Bool:
            return v.b ? "true" : "false";
        case PortType::String:
            return v.s;
        case PortType::Pulse:
            return "";
        case PortType::Number: {
            char buf[64];
            auto it = params.find("decimals");
            if (it != params.end()) {
                int decimals = static_cast<int>(it->second.n);
                if (decimals < 0) {
                    decimals = 0;
                }
                return std::string(buf, xt::formatFixed(buf, sizeof(buf), v.n, decimals));
            }
            return std::string(buf, xt::formatFloat(buf, sizeof(buf), v.n, 6));
        }
    }
    return "";
}

std::string substitutePlaceholder(const std::string& format, const std::string& value) {
    size_t pos = format.find("{}");
    if (pos == std::string::npos) {
        return format;
    }
    std::string out = format;
    out.replace(pos, 2, value);
    return out;
}

std::string substituteNamedPlaceholders(const std::string& format, const std::string (&values)[4]) {
    std::string out;
    out.reserve(format.size());
    for (size_t i = 0; i < format.size(); ++i) {
        if (format[i] == '{' && i + 2 < format.size() && format[i + 2] == '}' && format[i + 1] >= 'a' &&
            format[i + 1] <= 'd') {
            out += values[static_cast<size_t>(format[i + 1] - 'a')];
            i += 2;
            continue;
        }
        out += format[i];
    }
    return out;
}

void FlowRuntime::Impl::seedInitialOutputs() {
    for (size_t i = 0; i < nodes.size(); ++i) {
        NodeRuntime& n = nodes[i];
        int idx = static_cast<int>(i);
        switch (n.type) {
            case NodeType::Constant: {
                static const PortValue kZero{};
                auto it = n.params.find("value");
                pushRawOutput(idx, "out", it != n.params.end() ? it->second : kZero);
                break;
            }
            case NodeType::Counter: {
                double initial = paramNum(n, "initial", 0.0);
                setCounter(idx, initial);
                break;
            }
            case NodeType::Toggle: {
                n.toggleValue = paramBool(n, "initial", false);
                pushBoolOutput(idx, "value", n.toggleValue);
                break;
            }
            case NodeType::Timer: {
                n.timerRunning = paramBool(n, "autostart", false);
                n.timerElapsed = 0;
                n.timerCount = 0;
                pushNumberOutput(idx, "count", 0);
                break;
            }
            case NodeType::Clock:
                pushNumberOutput(idx, "seconds", 0);
                break;
            case NodeType::Random:
                pushNumberOutput(idx, "value", paramNum(n, "min", 0.0));
                break;
            case NodeType::Radio: {
                n.radioCount = static_cast<uint32_t>(paramNum(n, "count", 2.0));
                if (n.radioCount < 2) {
                    n.radioCount = 2;
                }
                double initial = paramNum(n, "initial", 0.0);
                n.radioIndex = -1;  // Forces the first setRadio() to push, without a `changed`.
                setRadio(idx, initial);
                break;
            }
            case NodeType::Merge:
                break;  // Pulse-only: nothing to seed.
            case NodeType::System:
            case NodeType::Sound:
            case NodeType::Haptic:
            case NodeType::Spawn:
            case NodeType::Emit:
            case NodeType::Timeline:
                break;  // T-193: event-driven only; their outputs start at the typed default.
            case NodeType::Math:
            case NodeType::Compare:
            case NodeType::ToString:
            case NodeType::Select:
            case NodeType::Lookup:
            case NodeType::Format:
                evaluateNode(idx);
                break;
        }
    }
}

void FlowRuntime::Impl::setCounter(int nodeIndex, double value) {
    NodeRuntime& n = nodes[static_cast<size_t>(nodeIndex)];
    auto minIt = n.params.find("min");
    auto maxIt = n.params.find("max");
    if (minIt != n.params.end()) {
        value = std::max(value, minIt->second.n);
    }
    if (maxIt != n.params.end()) {
        value = std::min(value, maxIt->second.n);
    }
    n.counterValue = value;
    pushNumberOutput(nodeIndex, "value", value);
}

void FlowRuntime::Impl::setRadio(int nodeIndex, double index) {
    NodeRuntime& n = nodes[static_cast<size_t>(nodeIndex)];
    double clamped = std::round(index);
    clamped = std::max(0.0, std::min(clamped, static_cast<double>(n.radioCount) - 1.0));
    bool changed = clamped != n.radioIndex;  // NOLINT: exact, like every other value comparison.
    bool firstPush = n.radioIndex < 0;
    n.radioIndex = clamped;
    pushNumberOutput(nodeIndex, "index", clamped);
    if (changed && !firstPush) {
        firePulseOutput(nodeIndex, "changed");
    }
}

void FlowRuntime::Impl::handlePulseInput(int nodeIndex, const std::string& port) {
    NodeRuntime& n = nodes[static_cast<size_t>(nodeIndex)];
    switch (n.type) {
        case NodeType::Counter: {
            double step = paramNum(n, "step", 1.0);
            if (port == "increment") {
                setCounter(nodeIndex, n.counterValue + step);
            } else if (port == "decrement") {
                setCounter(nodeIndex, n.counterValue - step);
            } else if (port == "reset") {
                setCounter(nodeIndex, paramNum(n, "initial", 0.0));
            }
            break;
        }
        case NodeType::Toggle: {
            bool old = n.toggleValue;
            if (port == "toggle") {
                n.toggleValue = !n.toggleValue;
            } else if (port == "setTrue") {
                n.toggleValue = true;
            } else if (port == "setFalse") {
                n.toggleValue = false;
            }
            if (n.toggleValue != old) {
                pushBoolOutput(nodeIndex, "value", n.toggleValue);
                firePulseOutput(nodeIndex, "changed");
            }
            break;
        }
        case NodeType::Radio: {
            // "set<i>": everything after "set" is the index (nodePortInfo() already checked that
            // it is a plain number below `count`).
            if (port.size() > 3) {
                double index = 0;  // T-268: xt::fromChars, not the locale's atof.
                xt::fromChars(port.data() + 3, port.data() + port.size(), index);
                setRadio(nodeIndex, index);
            }
            break;
        }
        case NodeType::Merge:
            if (port == "in") {
                firePulseOutput(nodeIndex, "out");
            }
            break;
        case NodeType::Timer: {
            if (port == "start") {
                n.timerRunning = true;
                n.timerElapsed = 0;
            } else if (port == "stop") {
                n.timerRunning = false;
            }
            break;
        }
        case NodeType::Sound:
        case NodeType::Haptic:
        case NodeType::Spawn:
        case NodeType::Emit:
        case NodeType::Timeline:
            // T-193: in a helper of its own, so this function's stack frame (on the path of a
            // feedback loop through the document, see fanOut()) does not grow.
            handleActionPulse(nodeIndex, port);
            break;
        case NodeType::Random: {
            if (port == "trigger") {
                // T-258a: non-finite bounds are 0 and a reversed range is swapped, so the standard
                // distributions' `min <= max` precondition holds; integer bounds saturate.
                double mn = finiteOr(paramNum(n, "min", 0.0), 0.0);
                double mx = finiteOr(paramNum(n, "max", 1.0), 0.0);
                if (mn > mx) {
                    std::swap(mn, mx);
                }
                double value;
                if (paramBool(n, "integer", false)) {
                    std::uniform_int_distribution<long long> dist(toInt64Sat(mn), toInt64Sat(mx));
                    value = static_cast<double>(dist(n.rng));
                } else {
                    std::uniform_real_distribution<double> dist(mn, mx);
                    value = dist(n.rng);
                }
                pushNumberOutput(nodeIndex, "value", value);
            }
            break;
        }
        default:
            break;  // Constant/Clock/Math/Compare/ToString/Select/Lookup/Format take no pulse input.
    }
}

void FlowRuntime::Impl::handleActionPulse(int nodeIndex, const std::string& port) {
    NodeRuntime& n = nodes[static_cast<size_t>(nodeIndex)];
    switch (n.type) {
        case NodeType::Sound:
            if (port == "trigger") {
                sendHostEvent(FlowHostEventKind::Sound, nodeIndex, paramStr(n, "cue", std::string()),
                              typedValue(PortType::Pulse), static_cast<float>(paramNum(n, "volume", 1.0)));
            }
            break;
        case NodeType::Haptic:
            if (port == "trigger") {
                sendHostEvent(FlowHostEventKind::Haptic, nodeIndex, paramStr(n, "cue", std::string()),
                              typedValue(PortType::Pulse), 1.0f);
            }
            break;
        case NodeType::Spawn:
            if (port == "trigger") {
                sendHostEvent(FlowHostEventKind::Spawn, nodeIndex, paramStr(n, "symbol", std::string()),
                              typedValue(PortType::Pulse), 1.0f);
            }
            break;
        case NodeType::Emit:
            if (port == "trigger") {
                auto it = n.inputValues.find("value");
                sendHostEvent(FlowHostEventKind::Emit, nodeIndex, paramStr(n, "port", std::string()),
                              it != n.inputValues.end() ? it->second : typedValue(PortType::Pulse), 1.0f);
            }
            break;
        case NodeType::Timeline:
            timelineInput(nodeIndex, port, typedValue(PortType::Pulse));
            break;
        default:
            break;
    }
}

void FlowRuntime::Impl::evaluateNode(int nodeIndex) {
    NodeRuntime& n = nodes[static_cast<size_t>(nodeIndex)];
    switch (n.type) {
        case NodeType::Math: {
            static const PortValue kZero = typedValue(PortType::Number);
            auto ai = n.inputValues.find("a");
            auto bi = n.inputValues.find("b");
            double a = ai != n.inputValues.end() ? ai->second.n : kZero.n;
            double b = bi != n.inputValues.end() ? bi->second.n : kZero.n;
            const std::string op = paramStr(n, "op", std::string());
            double out = 0;
            if (op == "add") {
                out = a + b;
            } else if (op == "sub") {
                out = a - b;
            } else if (op == "mul") {
                out = a * b;
            } else if (op == "div") {
                out = (b != 0.0) ? a / b : 0.0;
            } else if (op == "min") {
                out = std::min(a, b);
            } else if (op == "max") {
                out = std::max(a, b);
            } else if (op == "clamp") {
                double lo = paramNum(n, "lo", 0.0);
                double hi = paramNum(n, "hi", 1.0);
                out = std::min(std::max(a, lo), hi);
            } else if (op == "map") {
                double inLo = paramNum(n, "inLo", 0.0);
                double inHi = paramNum(n, "inHi", 1.0);
                double outLo = paramNum(n, "outLo", 0.0);
                double outHi = paramNum(n, "outHi", 1.0);
                double t = (inHi != inLo) ? (a - inLo) / (inHi - inLo) : 0.0;
                out = outLo + t * (outHi - outLo);
            } else if (op == "abs") {
                out = std::fabs(a);
            } else if (op == "round") {
                out = std::round(a);
            }
            pushNumberOutput(nodeIndex, "out", out);
            break;
        }
        case NodeType::Compare: {
            static const PortValue kZero = typedValue(PortType::Number);
            auto ai = n.inputValues.find("a");
            auto bi = n.inputValues.find("b");
            const PortValue& a = ai != n.inputValues.end() ? ai->second : kZero;
            const PortValue& b = bi != n.inputValues.end() ? bi->second : kZero;
            bool result = compareValues(a, b, paramStr(n, "op", std::string()));
            pushBoolOutput(nodeIndex, "out", result);
            bool changed = !n.compareLastValid || result != n.compareLastResult;
            n.compareLastValid = true;
            n.compareLastResult = result;
            // Not during seedInitialOutputs(): a multi-input node like this one is evaluated once
            // per incoming edge as each source seeds its own initial value (see load()), so its
            // early evaluations see a transient mix of real and not-yet-seeded (default) inputs.
            // Firing "true"/"false" off those transients would be spurious; only the settled
            // state after every node has seeded is externally observable, established here
            // without a pulse and compared against from the first real (post-load) evaluation on.
            if (changed && !seeding) {
                firePulseOutput(nodeIndex, result ? "true" : "false");
            }
            break;
        }
        case NodeType::ToString: {
            static const PortValue kEmpty = typedValue(PortType::String);
            auto it = n.inputValues.find("in");
            const PortValue& in = it != n.inputValues.end() ? it->second : kEmpty;
            std::string formatted = formatPortValue(in, n.params);
            std::string out = substitutePlaceholder(paramStr(n, "format", std::string("{}")), formatted);
            pushStringOutput(nodeIndex, "out", out);
            break;
        }
        case NodeType::Lookup: {
            const std::vector<PortValue>& values = n.values;
            if (values.empty()) {
                break;  // FlowGraph::parse() rejects this; a hand-built graph just has no output.
            }
            auto it = n.inputValues.find("index");
            double index = it != n.inputValues.end() ? std::round(it->second.n) : 0.0;
            index = std::max(0.0, std::min(index, static_cast<double>(values.size()) - 1.0));
            pushRawOutput(nodeIndex, "out", values[static_cast<size_t>(index)]);
            break;
        }
        case NodeType::Format: {
            std::string parts[4];
            for (size_t i = 0; i < 4; ++i) {
                auto it = n.inputValues.find(std::string(1, static_cast<char>('a' + i)));
                // An unconnected (or not-yet-delivered) placeholder formats as the empty string.
                parts[i] = it != n.inputValues.end() ? formatPortValue(it->second, n.params) : std::string();
            }
            pushStringOutput(nodeIndex, "out",
                             substituteNamedPlaceholders(paramStr(n, "format", std::string("{a}")), parts));
            break;
        }
        case NodeType::Select: {
            auto condIt = n.inputValues.find("cond");
            bool cond = condIt != n.inputValues.end() && condIt->second.b;
            static const PortValue kDefault{};
            auto ai = n.inputValues.find("a");
            auto bi = n.inputValues.find("b");
            const PortValue& chosen = cond ? (ai != n.inputValues.end() ? ai->second : kDefault)
                                           : (bi != n.inputValues.end() ? bi->second : kDefault);
            pushRawOutput(nodeIndex, "out", chosen);
            break;
        }
        default:
            break;  // Constant/Counter/Toggle/Timer/Clock/Random are not recomputed from inputs.
    }
}

void FlowRuntime::Impl::deliverToNode(int nodeIndex, const std::string& port, const PortValue& v,
                                      bool isPulse) {
    if (isPulse) {
        // T-389: a pulse inside a batch sees the values delivered before it, settled.
        if (batching && dirtyNodes > 0) {
            settleNodes();
        }
        handlePulseInput(nodeIndex, port);
        return;
    }
    NodeRuntime& n = nodes[static_cast<size_t>(nodeIndex)];
    n.inputValues[port] = v;
    switch (n.type) {
        case NodeType::Math:
        case NodeType::Compare:
        case NodeType::ToString:
        case NodeType::Select:
        case NodeType::Lookup:
        case NodeType::Format:
            // T-389 (27 §27.4.1): inside a batch the node is evaluated once, after all its inputs.
            if (batching) {
                markNodeDirty(nodeIndex);
            } else {
                evaluateNode(nodeIndex);
            }
            break;
        case NodeType::Counter:
            if (port == "set") {  // T-123: clamped into min/max like every other counter write.
                setCounter(nodeIndex, v.n);
            }
            break;
        case NodeType::Radio:
            if (port == "set") {
                setRadio(nodeIndex, v.n);
            }
            break;
        case NodeType::Timeline:
            // T-193: `seek`. Not while load() seeds: a constant wired to `seek` must not move a
            // timeline just because the graph was loaded.
            if (port == "seek" && !seeding) {
                timelineInput(nodeIndex, port, v);
            }
            break;
        default:
            break;  // No other node type acts on a value input (`emit.value` is only stored).
    }
}

namespace {

/// docs/tasks/T-124.md: how often one edge may fire inside a single tick window before it is
/// treated as a feedback loop, and how deep fanOut() may nest. Node-to-node cycles are rejected at
/// load time, so a loop can only run through the document (a node writing a value back into an
/// element whose own output port then feeds the node again) -- which recurses, hence the second,
/// much lower cap: it is what actually keeps the stack from overflowing.
constexpr uint32_t kMaxEdgeFiresPerTick = 1000;
constexpr uint32_t kMaxFanOutDepth = 256;

}  // namespace

namespace {

/// externalFanOut's key: externalKey(), prefixed with "<uiId>|" for a `ui#` endpoint (T-193). Kept
/// to one string for the load() Document: fanOut() recurses through here in a document feedback
/// loop, so its stack frame stays as small as before.
std::string fanOutKey(const Endpoint& ep) {
    if (ep.uiId.empty()) {
        return externalKey(ep.owner, ep.elementId, ep.port);
    }
    std::string key = ep.uiId;
    key += '|';
    key += externalKey(ep.owner, ep.elementId, ep.port);
    return key;
}

}  // namespace

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

void FlowRuntime::Impl::buildFanOutIndex() {
    nodeFanOut.clear();
    externalFanOut.clear();
    for (size_t i = 0; i < edges.size(); ++i) {
        const Endpoint& from = edges[i].from;
        if (from.owner == Endpoint::Owner::Node) {
            nodeFanOut[{from.nodeIndex, from.port}].push_back(static_cast<uint32_t>(i));
        } else {
            externalFanOut[fanOutKey(from)].push_back(static_cast<uint32_t>(i));
        }
    }
    edgeFireCount.assign(edges.size(), 0u);
    edgeLoopReported.assign(edges.size(), false);
    fanOutDepth = 0;

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

const std::vector<uint32_t>* FlowRuntime::Impl::fanOutEdges(const Endpoint& source) const {
    if (source.owner == Endpoint::Owner::Node) {
        auto it = nodeFanOut.find({source.nodeIndex, source.port});
        return it == nodeFanOut.end() ? nullptr : &it->second;
    }
    auto it = externalFanOut.find(fanOutKey(source));
    return it == externalFanOut.end() ? nullptr : &it->second;
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

void FlowRuntime::Impl::fanOut(const Endpoint& source, const PortValue& v) {
    if (!enabled || replacing()) {
        return;  // T-193 (13.8.3 enableFlow(false)): nothing propagates. T-260c: nor once replaced.
    }
    // T-260c: every document, timeline and lifecycle event enters the runtime here; a load asked
    // for from a callback below runs when the outermost one returns.
    DispatchGuard dispatch(*this);
    const std::vector<uint32_t>* list = fanOutEdges(source);
    if (list == nullptr) {
        return;
    }
    if (fanOutDepth >= kMaxFanOutDepth) {
        reportFeedbackLoop(list->front(), "nested deeper than 256 pushes; propagation stopped");
        return;
    }
    ++fanOutDepth;
    // A copy: applyToSink() can re-enter through a document event and push further, and `list`
    // points into a map this Impl owns -- but never into one that changes during a tick, so the
    // indices stay valid. (buildFanOutIndex() only runs from load()/rebind().)
    for (uint32_t i : *list) {
        if (replacing()) {
            break;  // T-260c: a callback asked for another graph; the rest of this one stops.
        }
        // T-193 (27.3.1): an `instance` edge from a document port only carries that instance's
        // events (the instance of the event propagating right now, see externalOutput()).
        const BoundEdge& edge = edges[i];
        if (edge.instance >= 0 && edge.from.owner != Endpoint::Owner::Node &&
            static_cast<int64_t>(currentInstance) != edge.instance) {
            continue;
        }
        if (i < edgeFireCount.size() && ++edgeFireCount[i] > kMaxEdgeFiresPerTick) {
            reportFeedbackLoop(i, "fired more than 1000 times in one tick; propagation stopped");
            continue;
        }
        // load()'s initial seeding is not a delivery (FlowRuntime::edgeLastFired()): it only
        // establishes each node's starting output, before any runtime time has passed.
        if (!seeding && i < edgeLastFiredList.size()) {
            edgeLastFiredList[i] = docs.clockAccum;
        }
        applyToSink(edges[i], v);
    }
    --fanOutDepth;
}

void FlowRuntime::Impl::pushRawOutput(int nodeIndex, const std::string& port, const PortValue& v) {
    NodeRuntime& n = nodes[static_cast<size_t>(nodeIndex)];
    auto it = n.outputValues.find(port);
    bool changed = (it == n.outputValues.end()) || !valuesEqual(it->second, v);
    n.outputValues[port] = v;
    markValuesStale(nodeIndex);  // T-389: valueList follows in syncValueList().
    if (changed) {
        fanOut(Endpoint{Endpoint::Owner::Node, nodeIndex, n.type, "", port, v.type}, v);
    }
}

void FlowRuntime::Impl::pushNumberOutput(int nodeIndex, const std::string& port, double v) {
    PortValue pv = typedValue(PortType::Number);
    pv.n = v;
    pushRawOutput(nodeIndex, port, pv);
}

void FlowRuntime::Impl::pushBoolOutput(int nodeIndex, const std::string& port, bool v) {
    PortValue pv = typedValue(PortType::Bool);
    pv.b = v;
    pushRawOutput(nodeIndex, port, pv);
}

void FlowRuntime::Impl::pushStringOutput(int nodeIndex, const std::string& port, const std::string& v) {
    PortValue pv = typedValue(PortType::String);
    pv.s = v;
    pushRawOutput(nodeIndex, port, pv);
}

void FlowRuntime::Impl::firePulseOutput(int nodeIndex, const std::string& port) {
    NodeRuntime& n = nodes[static_cast<size_t>(nodeIndex)];
    PortValue pv = typedValue(PortType::Pulse);
    fanOut(Endpoint{Endpoint::Owner::Node, nodeIndex, n.type, "", port, PortType::Pulse}, pv);
}

// ---- T-193 (docs/design/27-interaction.md §27.4.1): actions, timelines, system events ----------

void FlowRuntime::Impl::fireEventOutput(int nodeIndex, const std::string& port, const PortValue& v) {
    NodeRuntime& n = nodes[static_cast<size_t>(nodeIndex)];
    n.outputValues[port] = v;
    markValuesStale(nodeIndex);  // T-389.
    fanOut(Endpoint{Endpoint::Owner::Node, nodeIndex, n.type, "", port, v.type}, v);
}

// ---- T-389 (docs/design/27-interaction.md §27.4.1): value signals coalesced per frame ----------

namespace {

/// How many deliver-and-settle rounds one flushSignals() / settleNodes() runs. One is the normal
/// case; more only happen when a sink makes the Document report another value (a loop through the
/// document), which then waits for the next tick instead of spinning here.
constexpr int kMaxSignalRounds = 16;

}  // namespace

void FlowRuntime::Impl::queueSignal(const std::string& uiId, DocEndpointState* state, Endpoint::Owner owner,
                                    const std::string& elementId, const std::string& port,
                                    const PortValue& v) {
    std::pair<const DocEndpointState*, std::string> key{state, externalKey(owner, elementId, port)};
    auto it = pendingSignalIndex.find(key);
    if (it != pendingSignalIndex.end()) {
        pendingSignals[it->second].value = v;  // Only the latest value of the frame counts.
        return;
    }
    pendingSignalIndex.emplace(std::move(key), pendingSignals.size());
    pendingSignals.push_back(PendingSignal{state, uiId, owner, elementId, port, v});
}

void FlowRuntime::Impl::signalOutput(const std::string& uiId, DocEndpointState* state, Endpoint::Owner owner,
                                     const std::string& elementId, const std::string& port,
                                     const PortValue& v) {
    if (isValueSignal(owner, port, v.type) && coalescesSignals(*state)) {  // T-407: behavior >= 1.
        queueSignal(uiId, state, owner, elementId, port, v);
        return;
    }
    // A pulse / event: the values reported before it are delivered first, so it sees them.
    if (!pendingSignals.empty()) {
        flushSignals();
    }
    deliverExternal(uiId, state, owner, elementId, port, v);
}

void FlowRuntime::Impl::deliverExternal(const std::string& uiId, DocEndpointState* state,
                                        Endpoint::Owner owner, const std::string& elementId,
                                        const std::string& port, const PortValue& v) {
    if (uiId.empty() && state == &docs) {
        // The load() Document's outputs keep the pre-T-193 stack footprint (see Impl()).
        const uint32_t savedInstance = currentInstance;
        currentInstance = docs.doc != nullptr ? docs.doc->instanceId() : 0;
        fanOut(Endpoint{owner, -1, NodeType::Constant, elementId, port, v.type}, v);
        currentInstance = savedInstance;
        return;
    }
    externalOutput(uiId, state, owner, elementId, port, v);
}

void FlowRuntime::Impl::deliverPendingSignals() {
    if (pendingSignals.empty()) {
        return;
    }
    DispatchGuard dispatch(*this);  // T-260c: a callback's load waits until the batch is done.
    std::vector<PendingSignal> batch;
    batch.swap(pendingSignals);
    pendingSignalIndex.clear();
    const bool outer = !batching;
    batching = true;
    for (const PendingSignal& signal : batch) {
        if (replacing()) {
            break;
        }
        if (liveDocument(*signal.state) == nullptr) {
            continue;  // T-260b: reloaded / unloaded / removed since it reported the value.
        }
        deliverExternal(signal.uiId, signal.state, signal.owner, signal.elementId, signal.port, signal.value);
    }
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
        if (pendingSignals.empty()) {
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
            evaluateNode(static_cast<int>(i));
        }
    }
}

void FlowRuntime::Impl::markValuesStale(int nodeIndex) {
    const size_t i = static_cast<size_t>(nodeIndex);
    if (i >= nodeStale.size()) {
        nodeStale.resize(nodes.size(), 0);
    }
    if (i < nodeStale.size() && nodeStale[i] == 0) {
        nodeStale[i] = 1;
        staleNodes.push_back(nodeIndex);
    }
}

void FlowRuntime::Impl::syncValueList() {
    // Only the nodes pushed since the last call: the cost follows the work, not the graph size.
    for (int nodeIndex : staleNodes) {
        const size_t i = static_cast<size_t>(nodeIndex);
        if (i >= nodes.size()) {
            continue;
        }
        nodeStale[i] = 0;
        for (const auto& [port, value] : nodes[i].outputValues) {
            auto slot = outputSlot.find({nodeIndex, port});
            if (slot != outputSlot.end() && slot->second < valueList.size()) {
                valueList[slot->second] = value;
            }
        }
    }
    staleNodes.clear();
}

void FlowRuntime::Impl::sendHostEvent(FlowHostEventKind kind, int nodeIndex, const std::string& name,
                                      const PortValue& value, float volume) {
    if (!hostListener || !enabled || replacing()) {
        return;
    }
    FlowHostEvent event;
    event.kind = kind;
    event.name = name;
    event.volume = volume;
    event.value = value;
    event.uiId = currentUi;
    event.instanceId = currentInstance;
    event.nodeId = nodes[static_cast<size_t>(nodeIndex)].id;
    // A copy: the listener may replace itself (setHostEventListener) while being called.
    FlowHostEventListener listener = hostListener;
    listener(event);
}

void FlowRuntime::Impl::timelineInput(int nodeIndex, const std::string& port, const PortValue& v) {
    const NodeRuntime& n = nodes[static_cast<size_t>(nodeIndex)];
    const std::string name = paramStr(n, "name", std::string());
    const std::string scope = paramStr(n, "scope", std::string());
    const std::string nodeId = n.id;
    for (DocEndpointState* state : statesFor(n.uiId)) {
        Status status = Status::kOk;
        if (port == "play") {
            PlayOptions options;
            options.loop = paramBool(n, "loop", false);
            options.rate = static_cast<float>(paramNum(n, "rate", 1.0));
            status = state->doc->playTimeline(scope, name, options);
        } else if (port == "stop") {
            status = state->doc->stopTimeline(scope, name);
        } else if (port == "seek") {
            status = state->doc->seekTimeline(scope, name, static_cast<float>(v.n));
        }
        if (status == Status::kNotFound && missingTimelines.insert(nodeId).second) {
            runtimeErrorList.push_back("node '" + nodeId + "': no @timeline '" + name + "'" +
                                       (scope.empty() ? std::string() : " in scope '" + scope + "'"));
        }
    }
}

void FlowRuntime::Impl::externalOutput(const std::string& uiId, DocEndpointState* state,
                                       Endpoint::Owner owner, const std::string& elementId,
                                       const std::string& port, const PortValue& v) {
    const std::string savedUi = currentUi;
    const uint32_t savedInstance = currentInstance;
    currentUi = uiId;
    // T-260b: a lifecycle event can come from a handle whose Document was replaced since the
    // runtime bound it (its old Document is gone): liveDocument(), not `state->doc`.
    const Document* doc = state != nullptr ? liveDocument(*state) : nullptr;
    currentInstance = doc != nullptr ? doc->instanceId() : 0;
    Endpoint source{owner, -1, NodeType::Constant, elementId, port, v.type};
    source.uiId = uiId;
    fanOut(source, v);
    currentUi = savedUi;
    currentInstance = savedInstance;
}

void FlowRuntime::Impl::lifecycleEvent(const Context* context, DocumentHandle handle, bool shown) {
    if (!loaded) {
        return;
    }
    for (UiDoc& ui : uiDocs) {
        if (ui.handle == handle && ui.state->context == context) {  // T-356: handles are per Context.
            externalOutput(ui.uiId, ui.state.get(), Endpoint::Owner::Lifecycle, "",
                           shown ? "shown" : "hidden", typedValue(PortType::Pulse));
        }
    }
}

Status FlowRuntime::Impl::postSystemEvent(std::string_view kind, std::string_view value) {
    std::span<const std::string_view> kinds = systemEventKinds();
    size_t index = kinds.size();
    for (size_t i = 0; i < kinds.size(); ++i) {
        if (kinds[i] == kind) {
            index = i;
            break;
        }
    }
    if (index == kinds.size()) {
        return Status::kInvalidArgument;
    }
    if (!loaded || !enabled) {
        return Status::kOk;
    }
    DispatchGuard dispatch(*this);  // T-260c.
    const std::string savedUi = currentUi;
    const uint32_t savedInstance = currentInstance;
    currentUi.clear();
    currentInstance = 0;
    const std::string port(kind);
    // T-262b (F12): the port's declared type (nodePortInfo, the table the bindings use) decides
    // pulse or string, not the kind's position in systemEventKinds(). System ports take no params.
    static const NodeDesc kNoParams;
    PortType portType = PortType::String;
    nodePortInfo(NodeType::System, kNoParams, port, /*isSource=*/true, portType);
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].type != NodeType::System) {
            continue;
        }
        if (portType == PortType::Pulse) {
            firePulseOutput(static_cast<int>(i), port);
        } else {
            PortValue v = typedValue(PortType::String);
            v.s = std::string(value);
            fireEventOutput(static_cast<int>(i), port, v);
        }
    }
    currentUi = savedUi;
    currentInstance = savedInstance;
    return Status::kOk;
}

}  // namespace xt::flow
