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

/// The signal node types' runtime behavior (Docs/design/17-editor.md §17.6's table, T-123 adding
/// radio/lookup/merge/format to Docs/tasks/T-56.md's ten, T-193 the action / system / timeline
/// nodes). T-476 (27 §27.4.2 items 1, 3): a node is compiled at load time into slots of the
/// runtime's RtValue array (compileNode()); evaluation reads and writes slots, never a name.
/// The propagation machinery (fan-out, the signal batch, values()) is in flow_signals.cpp.

namespace xt::flow {

// ---- StringPool, ops, legacy value helpers -----------------------------------------------------

void StringPool::clear() {
    m_strings.clear();
    m_interned.clear();
    m_strings.emplace_back();  // Id 0: "".
    m_interned.emplace(std::string(), 0u);
}

uint32_t StringPool::intern(std::string_view text) {
    auto it = m_interned.find(text);
    if (it != m_interned.end()) {
        return it->second;
    }
    const auto id = static_cast<uint32_t>(m_strings.size());
    m_strings.emplace_back(text);
    m_interned.emplace(std::string(text), id);
    return id;
}

uint32_t StringPool::addBuffer() {
    const auto id = static_cast<uint32_t>(m_strings.size());
    m_strings.emplace_back();
    return id;
}

MathOp parseMathOp(std::string_view op) {
    static constexpr std::pair<std::string_view, MathOp> kOps[] = {
        {"add", MathOp::Add}, {"sub", MathOp::Sub},    {"mul", MathOp::Mul},     {"div", MathOp::Div},
        {"min", MathOp::Min}, {"max", MathOp::Max},    {"clamp", MathOp::Clamp}, {"map", MathOp::Map},
        {"abs", MathOp::Abs}, {"round", MathOp::Round}};
    for (const auto& [name, value] : kOps) {
        if (name == op) {
            return value;
        }
    }
    return MathOp::Unknown;
}

CompareOp parseCompareOp(std::string_view op) {
    static constexpr std::pair<std::string_view, CompareOp> kOps[] = {
        {"eq", CompareOp::Eq}, {"ne", CompareOp::Ne}, {"lt", CompareOp::Lt},
        {"le", CompareOp::Le}, {"gt", CompareOp::Gt}, {"ge", CompareOp::Ge}};
    for (const auto& [name, value] : kOps) {
        if (name == op) {
            return value;
        }
    }
    return CompareOp::Unknown;
}

FilterOp parseFilterOp(std::string_view op) {
    if (op == "deadband") {
        return FilterOp::Deadband;
    }
    if (op == "quantize") {
        return FilterOp::Quantize;
    }
    if (op == "sample") {
        return FilterOp::Sample;
    }
    return FilterOp::Unknown;
}

ThrottleUnit parseThrottleUnit(std::string_view unit) {
    if (unit == "frames") {
        return ThrottleUnit::Frames;
    }
    if (unit == "seconds") {
        return ThrottleUnit::Seconds;
    }
    if (unit == "hz") {
        return ThrottleUnit::Hz;
    }
    return ThrottleUnit::Unknown;
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

namespace {

/// `compare`'s `op` on two operands: strings by content when `strings`, else the numbers.
bool compareWith(CompareOp op, const std::string& sa, const std::string& sb, bool strings, double av,
                 double bv) {
    if (strings) {
        switch (op) {
            case CompareOp::Eq:
                return sa == sb;
            case CompareOp::Ne:
                return sa != sb;
            case CompareOp::Lt:
                return sa < sb;
            case CompareOp::Le:
                return sa <= sb;
            case CompareOp::Gt:
                return sa > sb;
            case CompareOp::Ge:
                return sa >= sb;
            case CompareOp::Unknown:
                return false;
        }
        return false;
    }
    switch (op) {
        case CompareOp::Eq:
            return av == bv;  // NOLINT: exact, as every value comparison.
        case CompareOp::Ne:
            return av != bv;  // NOLINT
        case CompareOp::Lt:
            return av < bv;
        case CompareOp::Le:
            return av <= bv;
        case CompareOp::Gt:
            return av > bv;
        case CompareOp::Ge:
            return av >= bv;
        case CompareOp::Unknown:
            return false;
    }
    return false;
}

/// A `compare` operand as a number: Bool as 0/1, anything else its `n`.
double numericOperand(PortType type, bool b, double n) {
    return type == PortType::Bool ? (b ? 1.0 : 0.0) : n;
}

/// Cuts `toString`'s format (`named` false: the first "{}" is input 0) or `format`'s (`named`:
/// every "{a}".."{d}" is input 0..3) into pieces; any other text is literal.
void cutFormat(const std::string& text, bool named, std::vector<FormatPiece>& out) {
    out.clear();
    auto literal = [&out](size_t from, size_t to) {
        if (to > from) {
            out.push_back(FormatPiece{static_cast<uint32_t>(from), static_cast<uint32_t>(to - from), -1});
        }
    };
    if (!named) {
        const size_t pos = text.find("{}");
        if (pos == std::string::npos) {
            literal(0, text.size());  // No placeholder: the value is not shown.
            return;
        }
        literal(0, pos);
        out.push_back(FormatPiece{0, 0, 0});
        literal(pos + 2, text.size());
        return;
    }
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '{' && i + 2 < text.size() && text[i + 2] == '}' && text[i + 1] >= 'a' &&
            text[i + 1] <= 'd') {
            literal(start, i);
            out.push_back(FormatPiece{0, 0, text[i + 1] - 'a'});
            i += 2;
            start = i + 1;
        }
    }
    literal(start, text.size());
}

constexpr RtValue kPulseValue{PortType::Pulse, false, 0.0, 0u};

}  // namespace

// ---- Slot access ---------------------------------------------------------------------------------

double FlowRuntime::Impl::paramNum(const NodeRuntime& n, uint16_t slot, double def) const {
    const RtValue& p = values[n.paramBase + slot];
    if (p.type == PortType::Number) {
        return p.n;
    }
    if (p.type == PortType::Bool) {
        return p.b ? 1.0 : 0.0;
    }
    return def;
}

bool FlowRuntime::Impl::paramBool(const NodeRuntime& n, uint16_t slot, bool def) const {
    const RtValue& p = values[n.paramBase + slot];
    if (p.type == PortType::Bool) {
        return p.b;
    }
    if (p.type == PortType::Number) {
        return p.n != 0.0;
    }
    return def;
}

const std::string& FlowRuntime::Impl::paramText(const NodeRuntime& n, uint16_t slot) const {
    const RtValue& p = values[n.paramBase + slot];
    return strings.text(p.type == PortType::String ? p.str : 0u);
}

void FlowRuntime::Impl::appendFormatted(std::string& out, const RtValue& v, const NodeRuntime& n) const {
    switch (v.type) {
        case PortType::Bool:
            out += v.b ? "true" : "false";
            return;
        case PortType::String:
            out += strings.text(v.str);
            return;
        case PortType::Number: {
            char buf[64];
            const size_t len = n.hasDecimals ? xt::formatFixed(buf, sizeof(buf), v.n, n.decimals)
                                             : xt::formatFloat(buf, sizeof(buf), v.n, 6);
            out.append(buf, len);
            return;
        }
        default:
            return;  // Pulse / no value yet: the empty string.
    }
}

// ---- Load-time compilation -------------------------------------------------------------------------

uint16_t stateSlotOf(const NodeRuntime& node, SlotKind kind, std::string_view name) {
    if (kind == SlotKind::Param) {
        return slotOf(NodeType::State, kind, name);
    }
    if (const uint16_t fixed = slotOf(NodeType::State, kind, name); fixed != kNoSlot) {
        return fixed;
    }
    if (kind == SlotKind::Input) {
        for (size_t e = 0; e < node.stateEvents.size(); ++e) {
            if (node.stateEvents[e] == name) {
                return static_cast<uint16_t>(slots::state::kFirstEvent + e);
            }
        }
        return kNoSlot;
    }
    const bool enter = name.size() > 5 && name.substr(0, 5) == "enter";
    const bool exit = name.size() > 4 && name.substr(0, 4) == "exit";
    if (!enter && !exit) {
        return kNoSlot;
    }
    const std::string_view stateName = name.substr(enter ? 5 : 4);
    for (size_t s = 0; s < node.stateNames.size(); ++s) {
        if (node.stateNames[s] == stateName) {
            return enter ? slots::state::enterSlot(static_cast<uint32_t>(s))
                         : slots::state::exitSlot(static_cast<uint32_t>(s));
        }
    }
    return kNoSlot;
}

void FlowRuntime::Impl::compileStateNode(const NodeDesc& nd, NodeRuntime& rt) {
    // load() compiles before it validates: a node that fails checkStateNode() stays empty here (its
    // errors are reported by validateGraph(), which then fails the load).
    std::vector<std::string> problems;
    checkStateNode(nd, problems);
    if (!problems.empty()) {
        return;
    }
    for (const PortValue& v : nd.arrayParams.at("states")) {
        rt.stateNames.push_back(v.s);
        rt.stateNameIds.push_back(strings.intern(v.s));
    }
    rt.stateEvents = stateEventNames(nd);
    rt.stateCount = static_cast<uint16_t>(rt.stateNames.size());
    rt.eventCount = static_cast<uint16_t>(rt.stateEvents.size());
    auto indexOf = [](const std::vector<std::string>& list, const std::string& name) {
        return static_cast<int16_t>(std::find(list.begin(), list.end(), name) - list.begin());
    };
    rt.stateTrans.assign(static_cast<size_t>(rt.stateCount) * rt.eventCount, int16_t{-1});
    rt.stateWild.assign(rt.eventCount, int16_t{-1});
    for (const TransitionDesc& t : nd.transitions) {
        const int16_t event = indexOf(rt.stateEvents, t.on);
        const int16_t to = indexOf(rt.stateNames, t.to);
        if (t.from == "*") {
            rt.stateWild[static_cast<size_t>(event)] = to;
        } else {
            rt.stateTrans[static_cast<size_t>(indexOf(rt.stateNames, t.from)) * rt.eventCount +
                          static_cast<size_t>(event)] = to;
        }
    }
    if (auto initial = nd.params.find("initial"); initial != nd.params.end()) {
        rt.stateCurrent = static_cast<uint16_t>(indexOf(rt.stateNames, initial->second.s));
    }
}

void FlowRuntime::Impl::compileNode(const NodeDesc& nd, NodeRuntime& rt) {
    const NodeSlotTable table = nodeSlotTable(rt.type);
    if (rt.type == NodeType::State) {
        compileStateNode(nd, rt);
    }
    // `radio`'s "set<i>" inputs: room for the largest legal count, so a slot never depends on it.
    // `state` (T-541): one pulse input per event, an enter / exit output pair per state.
    const uint32_t inputs = inputSlotCount(rt.type, 64) + rt.eventCount;
    const uint32_t outputs = static_cast<uint32_t>(table.outputs.size()) + 2u * rt.stateCount;
    rt.inputBase = static_cast<uint32_t>(values.size());
    rt.inputCount = static_cast<uint16_t>(inputs);
    values.resize(values.size() + inputs);
    rt.outputBase = static_cast<uint32_t>(values.size());
    rt.outputCount = static_cast<uint16_t>(outputs);
    values.resize(values.size() + outputs);
    rt.paramBase = static_cast<uint32_t>(values.size());
    rt.paramCount = static_cast<uint16_t>(table.params.size());
    for (std::string_view name : table.params) {
        RtValue p;
        auto it = nd.params.find(std::string(name));
        if (it != nd.params.end()) {
            p.type = it->second.type;
            p.b = it->second.b;
            p.n = it->second.n;
            p.str = it->second.type == PortType::String ? strings.intern(it->second.s) : 0u;
        }
        values.push_back(p);
    }
    if (auto lookup = nd.arrayParams.find("values"); lookup != nd.arrayParams.end()) {
        for (const PortValue& v : lookup->second) {  // `lookup`'s table (T-123).
            RtValue entry;
            entry.type = v.type;
            entry.b = v.b;
            entry.n = v.n;
            entry.str = v.type == PortType::String ? strings.intern(v.s) : 0u;
            rt.lookup.push_back(entry);
        }
    }
    if (auto ui = nd.params.find("ui"); ui != nd.params.end() && ui->second.type == PortType::String) {
        rt.uiId = ui->second.s;  // T-193: a global `timeline` node's UI.
    }
    switch (rt.type) {
        case NodeType::Math:
            rt.op = static_cast<uint8_t>(parseMathOp(paramText(rt, slots::math::kOp)));
            break;
        case NodeType::Compare:
            rt.op = static_cast<uint8_t>(parseCompareOp(paramText(rt, slots::compare::kOp)));
            break;
        case NodeType::ToString:
        case NodeType::Format: {
            const bool named = rt.type == NodeType::Format;
            const RtValue& format = values[rt.paramBase + slots::format::kFormat];
            rt.formatText = format.type == PortType::String ? strings.text(format.str)
                                                            : std::string(named ? "{a}" : "{}");
            cutFormat(rt.formatText, named, rt.pieces);
            const RtValue& decimals = values[rt.paramBase + slots::format::kDecimals];
            if (decimals.type != kNoValue) {
                rt.hasDecimals = true;
                rt.decimals = std::max(0, static_cast<int>(decimals.n));
            }
            break;
        }
        case NodeType::Radio:
            rt.radioCount = std::max(2u, static_cast<uint32_t>(paramNum(rt, slots::radio::kCount, 2.0)));
            break;
        case NodeType::Filter:
            rt.op = static_cast<uint8_t>(parseFilterOp(paramText(rt, slots::filter::kOp)));
            rt.filterLastHome = strings.addBuffer();  // T-543: the last sent value's string.
            break;
        case NodeType::Throttle:
            rt.op = static_cast<uint8_t>(parseThrottleUnit(paramText(rt, slots::throttle::kUnit)));
            rt.throttleTrailing = paramBool(rt, slots::throttle::kTrailing, true);
            rt.throttlePendingHome = strings.addBuffer();
            break;
        default:
            break;
    }
}

// ---- Node behavior -----------------------------------------------------------------------------------

void FlowRuntime::Impl::seedInitialOutputs() {
    for (size_t i = 0; i < nodes.size(); ++i) {
        NodeRuntime& n = nodes[i];
        int idx = static_cast<int>(i);
        switch (n.type) {
            case NodeType::Constant: {
                RtValue v = values[n.paramBase];  // "value"; absent: a default (Pulse) value.
                if (v.type == kNoValue) {
                    v = kPulseValue;
                }
                pushRawOutput(idx, 0, v);
                break;
            }
            case NodeType::Counter:
                setCounter(idx, paramNum(n, slots::counter::kInitial, 0.0));
                break;
            case NodeType::Toggle:
                n.toggleValue = paramBool(n, slots::toggle::kInitial, false);
                pushBoolOutput(idx, slots::toggle::kValue, n.toggleValue);
                break;
            case NodeType::Timer:
                n.timerRunning = paramBool(n, slots::timer::kAutostart, false);
                n.timerElapsed = 0;
                n.timerCount = 0;
                pushNumberOutput(idx, slots::timer::kCount, 0);
                break;
            case NodeType::Clock:
                pushNumberOutput(idx, 0, 0);
                break;
            case NodeType::Random:
                pushNumberOutput(idx, slots::random::kValue, paramNum(n, slots::random::kMin, 0.0));
                break;
            case NodeType::Radio:
                n.radioIndex = -1;  // Forces the first setRadio() to push, without a `changed`.
                setRadio(idx, paramNum(n, slots::radio::kInitial, 0.0));
                break;
            case NodeType::State:
                // T-541: the initial state is pushed like a `constant`; `enter<initial>` is not fired.
                if (n.stateCount > 0) {
                    statePushValues(idx);
                }
                break;
            case NodeType::Merge:
                break;  // Pulse-only: nothing to seed.
            case NodeType::System:
            case NodeType::Sound:
            case NodeType::Haptic:
            case NodeType::Spawn:
            case NodeType::Emit:
            case NodeType::Timeline:
                break;  // T-193: event-driven only; their outputs start at the typed default.
            case NodeType::Filter:
                break;  // T-543: event-driven only.
            case NodeType::Throttle:
                pushNumberOutput(idx, slots::throttle::kDropped, 0);
                break;
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
    const RtValue& min = values[n.paramBase + slots::counter::kMin];
    const RtValue& max = values[n.paramBase + slots::counter::kMax];
    if (min.type != kNoValue) {
        value = std::max(value, min.n);
    }
    if (max.type != kNoValue) {
        value = std::min(value, max.n);
    }
    n.counterValue = value;
    pushNumberOutput(nodeIndex, slots::counter::kValue, value);
}

void FlowRuntime::Impl::setRadio(int nodeIndex, double index) {
    NodeRuntime& n = nodes[static_cast<size_t>(nodeIndex)];
    double clamped = std::round(index);
    clamped = std::max(0.0, std::min(clamped, static_cast<double>(n.radioCount) - 1.0));
    bool changed = clamped != n.radioIndex;  // NOLINT: exact, like every other value comparison.
    bool firstPush = n.radioIndex < 0;
    n.radioIndex = clamped;
    pushNumberOutput(nodeIndex, slots::radio::kIndex, clamped);
    if (changed && !firstPush) {
        firePulseOutput(nodeIndex, slots::radio::kChanged);
    }
}

void FlowRuntime::Impl::statePushValues(int nodeIndex) {
    NodeRuntime& n = nodes[static_cast<size_t>(nodeIndex)];
    // The state changed (or is being established), so the string is written and fanned out without
    // comparing it with the old one: no string comparison and, past the buffer's first growth, no
    // allocation (like fireEventText()).
    const uint32_t i = n.outputBase + slots::state::kState;
    strings.buffer(slotHome(i)).assign(strings.text(n.stateNameIds[n.stateCurrent]));
    values[i] = RtValue{PortType::String, false, 0.0, slotHome(i)};
    markValuesStale(nodeIndex);
    fanOut(i, values[i]);
    pushNumberOutput(nodeIndex, slots::state::kIndex, static_cast<double>(n.stateCurrent));
}

void FlowRuntime::Impl::stateTransition(int nodeIndex, uint32_t to) {
    NodeRuntime& n = nodes[static_cast<size_t>(nodeIndex)];
    const uint32_t from = n.stateCurrent;
    if (to == from || to >= n.stateCount) {
        return;  // A self-transition fires nothing (27 §27.4.4).
    }
    firePulseOutput(nodeIndex, slots::state::exitSlot(from));  // Downstream still sees the old state.
    n.stateCurrent = static_cast<uint16_t>(to);
    statePushValues(nodeIndex);
    firePulseOutput(nodeIndex, slots::state::enterSlot(to));  // ... and here the new one.
    firePulseOutput(nodeIndex, slots::state::kChanged);
}

void FlowRuntime::Impl::stateEvent(int nodeIndex, uint32_t event) {
    const NodeRuntime& n = nodes[static_cast<size_t>(nodeIndex)];
    const RtValue& guard = values[n.inputBase + slots::state::kGuard];
    if (guard.type == PortType::Bool && !guard.b) {
        return;  // `guard` false swallows every event (no guard on a missing edge).
    }
    if (event >= n.eventCount) {
        return;
    }
    int16_t to = n.stateTrans[static_cast<size_t>(n.stateCurrent) * n.eventCount + event];
    if (to < 0) {
        to = n.stateWild[event];  // The explicit (from, on) wins; "*" is the fallback.
    }
    if (to >= 0) {
        stateTransition(nodeIndex, static_cast<uint32_t>(to));
    }
}

void FlowRuntime::Impl::stateSet(int nodeIndex, const RtValue& v) {
    NodeRuntime& n = nodes[static_cast<size_t>(nodeIndex)];
    if (v.type != PortType::String) {
        return;
    }
    const std::string& name = strings.text(v.str);
    for (uint32_t s = 0; s < n.stateCount; ++s) {
        if (n.stateNames[s] == name) {
            stateTransition(nodeIndex, s);
            return;
        }
    }
    if (!n.stateSetReported) {  // Once per node: the error path may allocate.
        n.stateSetReported = true;
        runtimeErrorList.push_back("node '" + n.id + "': set: unknown state '" + name + "'");
    }
}

void FlowRuntime::Impl::handlePulseInput(int nodeIndex, uint16_t slot) {
    NodeRuntime& n = nodes[static_cast<size_t>(nodeIndex)];
    switch (n.type) {
        case NodeType::Counter: {
            double step = paramNum(n, slots::counter::kStep, 1.0);
            if (slot == slots::counter::kIncrement) {
                setCounter(nodeIndex, n.counterValue + step);
            } else if (slot == slots::counter::kDecrement) {
                setCounter(nodeIndex, n.counterValue - step);
            } else if (slot == slots::counter::kReset) {
                setCounter(nodeIndex, paramNum(n, slots::counter::kInitial, 0.0));
            }
            break;
        }
        case NodeType::Toggle: {
            bool old = n.toggleValue;
            if (slot == slots::toggle::kToggle) {
                n.toggleValue = !n.toggleValue;
            } else if (slot == slots::toggle::kSetTrue) {
                n.toggleValue = true;
            } else if (slot == slots::toggle::kSetFalse) {
                n.toggleValue = false;
            }
            if (n.toggleValue != old) {
                pushBoolOutput(nodeIndex, slots::toggle::kValue, n.toggleValue);
                firePulseOutput(nodeIndex, slots::toggle::kChanged);
            }
            break;
        }
        case NodeType::Radio:
            // "set<i>" is slot 1 + i (validation already checked i < `count`).
            if (slot > slots::radio::kSet) {
                setRadio(nodeIndex, static_cast<double>(slot - 1));
            }
            break;
        case NodeType::Merge:
            if (slot == slots::merge::kIn) {
                firePulseOutput(nodeIndex, slots::merge::kOut);
            }
            break;
        case NodeType::State:
            if (slot >= slots::state::kFirstEvent) {
                stateEvent(nodeIndex, static_cast<uint32_t>(slot - slots::state::kFirstEvent));
            }
            break;
        case NodeType::Timer: {
            if (slot == slots::timer::kStart) {
                n.timerRunning = true;
                n.timerElapsed = 0;
            } else if (slot == slots::timer::kStop) {
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
            handleActionPulse(nodeIndex, slot);
            break;
        case NodeType::Filter:
            if (slot == slots::filter::kReset) {
                n.filterHasLast = false;
            } else if (slot == slots::filter::kTrigger && n.op == static_cast<uint8_t>(FilterOp::Sample)) {
                // `sample`: the current `in`; nothing before the first one.
                const RtValue& current = values[n.inputBase + slots::filter::kIn];
                if (current.type != kNoValue && current.type != PortType::Pulse) {
                    emitPassed(nodeIndex, slots::filter::kOut, current, slots::filter::kPassed);
                }
            }
            break;
        case NodeType::Throttle:
            if (slot == slots::throttle::kReset) {
                // Clears the window and the held value; `dropped` keeps counting (debugging total).
                n.throttleHasPending = false;
                n.throttleEmitted = false;
                n.throttleNextAllowed = 0;
            } else if (slot == slots::throttle::kIn) {  // A pulse-typed `in`.
                throttleInput(nodeIndex, kPulseValue);
            }
            break;
        case NodeType::Random: {
            if (slot == slots::random::kTrigger) {
                // T-258a: non-finite bounds are 0 and a reversed range is swapped, so the standard
                // distributions' `min <= max` precondition holds; integer bounds saturate.
                double mn = finiteOr(paramNum(n, slots::random::kMin, 0.0), 0.0);
                double mx = finiteOr(paramNum(n, slots::random::kMax, 1.0), 0.0);
                if (mn > mx) {
                    std::swap(mn, mx);
                }
                double value;
                if (paramBool(n, slots::random::kInteger, false)) {
                    std::uniform_int_distribution<long long> dist(toInt64Sat(mn), toInt64Sat(mx));
                    value = static_cast<double>(dist(n.rng));
                } else {
                    std::uniform_real_distribution<double> dist(mn, mx);
                    value = dist(n.rng);
                }
                pushNumberOutput(nodeIndex, slots::random::kValue, value);
            }
            break;
        }
        default:
            break;  // Constant/Clock/Math/Compare/ToString/Select/Lookup/Format take no pulse input.
    }
}

void FlowRuntime::Impl::handleActionPulse(int nodeIndex, uint16_t slot) {
    const NodeRuntime& n = nodes[static_cast<size_t>(nodeIndex)];
    if (n.type == NodeType::Timeline) {
        timelineInput(nodeIndex, slot, kPulseValue);
        return;
    }
    if (slot != slots::action::kTrigger) {
        return;
    }
    switch (n.type) {
        case NodeType::Sound:
            sendHostEvent(FlowHostEventKind::Sound, nodeIndex, paramText(n, slots::action::kName),
                          typedValue(PortType::Pulse),
                          static_cast<float>(paramNum(n, slots::action::kVolume, 1.0)));
            break;
        case NodeType::Haptic:
            sendHostEvent(FlowHostEventKind::Haptic, nodeIndex, paramText(n, slots::action::kName),
                          typedValue(PortType::Pulse), 1.0f);
            break;
        case NodeType::Spawn:
            sendHostEvent(FlowHostEventKind::Spawn, nodeIndex, paramText(n, slots::action::kName),
                          typedValue(PortType::Pulse), 1.0f);
            break;
        case NodeType::Emit: {
            const RtValue& value = values[n.inputBase + slots::action::kEmitValue];
            sendHostEvent(FlowHostEventKind::Emit, nodeIndex, paramText(n, slots::action::kName),
                          value.type != kNoValue ? toPortValue(value) : typedValue(PortType::Pulse), 1.0f);
            break;
        }
        default:
            break;
    }
}

void FlowRuntime::Impl::evaluateNode(int nodeIndex) {
    perfCount(&FrameStats::flowNodeEvals);  // T-475.
    NodeRuntime& n = nodes[static_cast<size_t>(nodeIndex)];
    const RtValue* in = values.data() + n.inputBase;  // An input no edge reached yet: kNoValue, 0.
    switch (n.type) {
        case NodeType::Math: {
            const double a = in[slots::math::kA].n;
            const double b = in[slots::math::kB].n;
            double out = 0;
            switch (static_cast<MathOp>(n.op)) {
                case MathOp::Add:
                    out = a + b;
                    break;
                case MathOp::Sub:
                    out = a - b;
                    break;
                case MathOp::Mul:
                    out = a * b;
                    break;
                case MathOp::Div:
                    out = (b != 0.0) ? a / b : 0.0;
                    break;
                case MathOp::Min:
                    out = std::min(a, b);
                    break;
                case MathOp::Max:
                    out = std::max(a, b);
                    break;
                case MathOp::Clamp: {
                    const double lo = paramNum(n, slots::math::kLo, 0.0);
                    const double hi = paramNum(n, slots::math::kHi, 1.0);
                    out = std::min(std::max(a, lo), hi);
                    break;
                }
                case MathOp::Map: {
                    const double inLo = paramNum(n, slots::math::kInLo, 0.0);
                    const double inHi = paramNum(n, slots::math::kInHi, 1.0);
                    const double outLo = paramNum(n, slots::math::kOutLo, 0.0);
                    const double outHi = paramNum(n, slots::math::kOutHi, 1.0);
                    const double t = (inHi != inLo) ? (a - inLo) / (inHi - inLo) : 0.0;
                    out = outLo + t * (outHi - outLo);
                    break;
                }
                case MathOp::Abs:
                    out = std::fabs(a);
                    break;
                case MathOp::Round:
                    out = std::round(a);
                    break;
                case MathOp::Unknown:
                    break;
            }
            pushNumberOutput(nodeIndex, slots::math::kOut, out);
            break;
        }
        case NodeType::Compare: {
            const RtValue& a = in[slots::compare::kA];  // Absent: compares as the number 0.
            const RtValue& b = in[slots::compare::kB];
            const bool texts = a.type == PortType::String && b.type == PortType::String;
            const bool result =
                compareWith(static_cast<CompareOp>(n.op), strings.text(texts ? a.str : 0u),
                            strings.text(texts ? b.str : 0u), texts, numericOperand(a.type, a.b, a.n),
                            numericOperand(b.type, b.b, b.n));
            pushBoolOutput(nodeIndex, slots::compare::kOut, result);
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
                firePulseOutput(nodeIndex, result ? slots::compare::kTrue : slots::compare::kFalse);
            }
            break;
        }
        case NodeType::ToString:
        case NodeType::Format:
            // The pieces cut at load time; an unconnected (or not-yet-delivered) placeholder
            // formats as the empty string.
            n.scratch.clear();
            for (const FormatPiece& piece : n.pieces) {
                if (piece.input < 0) {
                    n.scratch.append(n.formatText, piece.offset, piece.length);
                } else {
                    appendFormatted(n.scratch, in[piece.input], n);
                }
            }
            pushScratchOutput(nodeIndex, slots::format::kOut);
            break;
        case NodeType::Lookup: {
            if (n.lookup.empty()) {
                break;  // FlowGraph::parse() rejects this; a hand-built graph just has no output.
            }
            double index = std::round(in[slots::lookup::kIndex].n);
            index = std::max(0.0, std::min(index, static_cast<double>(n.lookup.size()) - 1.0));
            pushRawOutput(nodeIndex, slots::lookup::kOut, n.lookup[static_cast<size_t>(index)]);
            break;
        }
        case NodeType::Select: {
            const bool cond = in[slots::select::kCond].b;
            const RtValue& chosen = cond ? in[slots::select::kA] : in[slots::select::kB];
            // An input that never arrived selects a default (Pulse) value.
            pushRawOutput(nodeIndex, slots::select::kOut, chosen.type == kNoValue ? kPulseValue : chosen);
            break;
        }
        default:
            break;  // Constant/Counter/Toggle/Timer/Clock/Random are not recomputed from inputs.
    }
}

void FlowRuntime::Impl::deliverToNode(int nodeIndex, uint16_t slot, const RtValue& v, bool isPulse) {
    if (isPulse) {
        // T-389: a pulse inside a batch sees the values delivered before it, settled.
        if (batching && dirtyNodes > 0) {
            settleNodes();
        }
        handlePulseInput(nodeIndex, slot);
        return;
    }
    NodeRuntime& n = nodes[static_cast<size_t>(nodeIndex)];
    const uint32_t input = n.inputBase + slot;
    assignValue(slotHome(input), values[input], v);
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
            } else if (perfTracker != nullptr) {
                evaluateNodeTimed(nodeIndex);  // T-475.
            } else {
                evaluateNode(nodeIndex);
            }
            break;
        case NodeType::Counter:
            if (slot == slots::counter::kSet) {  // T-123: clamped into min/max like every other write.
                setCounter(nodeIndex, v.n);
            }
            break;
        case NodeType::Radio:
            if (slot == slots::radio::kSet) {
                setRadio(nodeIndex, v.n);
            }
            break;
        case NodeType::State:
            // T-541: `set` jumps to the named state. Not while load() seeds: a constant wired to it
            // must not move the node away from its `initial` just because the graph was loaded.
            if (slot == slots::state::kSet && !seeding) {
                stateSet(nodeIndex, v);
            }
            break;
        case NodeType::Timeline:
            // T-193: `seek`. Not while load() seeds: a constant wired to `seek` must not move a
            // timeline just because the graph was loaded.
            if (slot == slots::timeline::kSeek && !seeding) {
                timelineInput(nodeIndex, slot, v);
            }
            break;
        case NodeType::Filter:  // T-543: event-driven, never batched.
            if (slot == slots::filter::kIn) {
                filterInput(nodeIndex);
            }
            break;
        case NodeType::Throttle:
            if (slot == slots::throttle::kIn) {
                throttleInput(nodeIndex, values[input]);
            } else if (slot == slots::throttle::kRate) {
                throttleRateChanged(nodeIndex);
            }
            break;
        default:
            break;  // No other node type acts on a value input (`emit.value` is only stored).
    }
}

void FlowRuntime::Impl::pushRawOutput(int nodeIndex, uint16_t slot, const RtValue& v) {
    const uint32_t i = nodes[static_cast<size_t>(nodeIndex)].outputBase + slot;
    RtValue& current = values[i];
    const bool changed = current.type == kNoValue || !rtEqual(current, v);
    assignValue(slotHome(i), current, v);
    markValuesStale(nodeIndex);  // T-389: valueList follows in syncValueList().
    if (changed) {
        fanOut(i, current);  // fanOut() delivers a copy of its own.
    }
}

void FlowRuntime::Impl::pushNumberOutput(int nodeIndex, uint16_t slot, double v) {
    pushRawOutput(nodeIndex, slot, RtValue{PortType::Number, false, v, 0u});
}

void FlowRuntime::Impl::pushBoolOutput(int nodeIndex, uint16_t slot, bool v) {
    pushRawOutput(nodeIndex, slot, RtValue{PortType::Bool, v, 0.0, 0u});
}

void FlowRuntime::Impl::pushScratchOutput(int nodeIndex, uint16_t slot) {
    NodeRuntime& n = nodes[static_cast<size_t>(nodeIndex)];
    const uint32_t i = n.outputBase + slot;
    RtValue& current = values[i];
    std::string& home = strings.buffer(slotHome(i));
    const bool changed = current.type != PortType::String || home != n.scratch;
    if (changed) {
        home.swap(n.scratch);  // Both keep their capacity: no allocation once they are large enough.
        current = RtValue{PortType::String, false, 0.0, slotHome(i)};
    }
    markValuesStale(nodeIndex);
    if (changed) {
        fanOut(i, current);
    }
}

void FlowRuntime::Impl::firePulseOutput(int nodeIndex, uint16_t slot) {
    fanOut(nodes[static_cast<size_t>(nodeIndex)].outputBase + slot, kPulseValue);
}

// ---- T-543 (Docs/design/27-interaction.md §27.4.5): filter and throttle -----------------------------

void FlowRuntime::Impl::emitPassed(int nodeIndex, uint16_t slot, const RtValue& v, uint16_t passedSlot) {
    if (v.type == PortType::Pulse || v.type == kNoValue) {
        firePulseOutput(nodeIndex, slot);
    } else {
        // Every value that passes is sent, even one equal to the output's current value: this
        // node decides what is worth sending, not the change-only filter of pushRawOutput().
        const uint32_t i = nodes[static_cast<size_t>(nodeIndex)].outputBase + slot;
        assignValue(slotHome(i), values[i], v);
        markValuesStale(nodeIndex);
        fanOut(i, values[i]);
    }
    firePulseOutput(nodeIndex, passedSlot);
}

void FlowRuntime::Impl::filterInput(int nodeIndex) {
    NodeRuntime& n = nodes[static_cast<size_t>(nodeIndex)];
    const RtValue& in = values[n.inputBase + slots::filter::kIn];
    RtValue send = in;
    switch (static_cast<FilterOp>(n.op)) {
        case FilterOp::Deadband: {
            if (in.type == kNoValue || in.type == PortType::Pulse) {
                return;
            }
            bool pass = !n.filterHasLast || n.filterLast.type != in.type;
            if (!pass) {
                if (in.type == PortType::Number) {
                    const double threshold =
                        std::max(0.0, finiteOr(paramNum(n, slots::filter::kThreshold, 0.0), 0.0));
                    pass = !(std::fabs(in.n - n.filterLast.n) < threshold);
                } else {
                    pass = !rtEqual(in, n.filterLast);  // bool / string: sent when the value changed.
                }
            }
            if (!pass) {
                return;
            }
            break;
        }
        case FilterOp::Quantize: {
            if (in.type != PortType::Number) {
                return;  // Load rejects a non-number `in`; a value that gets here anyway is dropped.
            }
            const double step = paramNum(n, slots::filter::kStep, 0.0);
            if (!(step > 0.0) || !std::isfinite(step) || !std::isfinite(in.n)) {
                return;
            }
            const double q = std::round(in.n / step) * step + 0.0;  // "+ 0.0": -0 becomes 0.
            if (!std::isfinite(q) ||
                (n.filterHasLast && n.filterLast.type == PortType::Number && q == n.filterLast.n)) {
                return;
            }
            send = RtValue{PortType::Number, false, q, 0u};
            break;
        }
        case FilterOp::Sample:  // `in` only stores; `trigger` sends.
        case FilterOp::Unknown:
            return;
    }
    assignValue(n.filterLastHome, n.filterLast, send);
    n.filterHasLast = true;
    emitPassed(nodeIndex, slots::filter::kOut, send, slots::filter::kPassed);
}

double FlowRuntime::Impl::throttleNow(const NodeRuntime& n) const {
    return static_cast<ThrottleUnit>(n.op) == ThrottleUnit::Frames ? static_cast<double>(tickCount)
                                                                   : tickTime;
}

double FlowRuntime::Impl::throttleWindow(const NodeRuntime& n) const {
    const auto unit = static_cast<ThrottleUnit>(n.op);
    if (unit == ThrottleUnit::Unknown) {
        return 0.0;
    }
    // The `rate` edge's last value wins over the param; either way a non-finite or non-positive
    // rate does not limit (27 §27.4.5, 14 §14.9).
    const RtValue& wired = values[n.inputBase + slots::throttle::kRate];
    const double raw =
        wired.type == PortType::Number ? wired.n : paramNum(n, slots::throttle::kRateParam, 0.0);
    const double rate = finiteOr(raw, 0.0);
    if (!(rate > 0.0)) {
        return 0.0;
    }
    switch (unit) {
        case ThrottleUnit::Frames:
            return static_cast<double>(std::max<int32_t>(1, toInt32Sat(rate)));
        case ThrottleUnit::Seconds:
            return std::min(rate, kMaxThrottleWindow);
        case ThrottleUnit::Hz:
            return std::min(1.0 / rate, kMaxThrottleWindow);
        case ThrottleUnit::Unknown:
            break;
    }
    return 0.0;
}

void FlowRuntime::Impl::throttleInput(int nodeIndex, const RtValue& v) {
    NodeRuntime& n = nodes[static_cast<size_t>(nodeIndex)];
    const RtValue input = v.type == kNoValue ? kPulseValue : v;  // A copy: `v` may be an input slot.
    const double window = throttleWindow(n);
    if (window <= 0.0) {
        emitPassed(nodeIndex, slots::throttle::kOut, input, slots::throttle::kPassed);  // No limit.
        return;
    }
    const double now = throttleNow(n);
    if (now + kThrottleTimeSlack >= n.throttleNextAllowed) {
        if (n.throttleHasPending) {  // Superseded by this newer input.
            n.throttleHasPending = false;
            n.throttleDropped += 1.0;
            pushNumberOutput(nodeIndex, slots::throttle::kDropped, n.throttleDropped);
        }
        n.throttleEmitted = true;
        n.throttleLastEmit = now;
        n.throttleNextAllowed = now + window;
        emitPassed(nodeIndex, slots::throttle::kOut, input, slots::throttle::kPassed);
        return;
    }
    // Inside the window: only the last value is kept, and only for the trailing send.
    if (!n.throttleTrailing || n.throttleHasPending) {
        n.throttleDropped += 1.0;  // Dropped for good, or overwritten.
        pushNumberOutput(nodeIndex, slots::throttle::kDropped, n.throttleDropped);
    }
    if (!n.throttleTrailing) {
        return;
    }
    assignValue(n.throttlePendingHome, n.throttlePending, input);
    n.throttleHasPending = true;
    if (!n.throttleQueued) {
        n.throttleQueued = true;
        throttleWaiting.push_back(nodeIndex);  // Reserved for every throttle node at load.
    }
}

void FlowRuntime::Impl::throttleSendPending(int nodeIndex) {
    NodeRuntime& n = nodes[static_cast<size_t>(nodeIndex)];
    const double window = throttleWindow(n);
    const double now = throttleNow(n);
    n.throttleHasPending = false;
    if (window > 0.0) {
        n.throttleEmitted = true;
        n.throttleLastEmit = now;
        n.throttleNextAllowed = now + window;
    }
    const RtValue held = n.throttlePending;  // Its string stays in throttlePendingHome.
    emitPassed(nodeIndex, slots::throttle::kOut, held, slots::throttle::kPassed);
}

void FlowRuntime::Impl::throttleRateChanged(int nodeIndex) {
    NodeRuntime& n = nodes[static_cast<size_t>(nodeIndex)];
    const double window = throttleWindow(n);
    if (window > 0.0 && n.throttleEmitted) {
        n.throttleNextAllowed = n.throttleLastEmit + window;  // The window re-measured, not restarted.
    }
    if (n.throttleHasPending &&
        (window <= 0.0 || throttleNow(n) + kThrottleTimeSlack >= n.throttleNextAllowed)) {
        throttleSendPending(nodeIndex);
    }
}

void FlowRuntime::Impl::flushThrottles() {
    // Only the waiting nodes are visited. A send may queue another node (appended past `count`,
    // seen next tick); the loop runs by index since the list can grow under it.
    const size_t count = throttleWaiting.size();
    for (size_t k = 0; k < count && k < throttleWaiting.size(); ++k) {
        const int idx = throttleWaiting[k];
        NodeRuntime& n = nodes[static_cast<size_t>(idx)];
        if (!n.throttleHasPending) {
            continue;  // Sent already, superseded or reset.
        }
        const double window = throttleWindow(n);
        if (window <= 0.0 || throttleNow(n) + kThrottleTimeSlack >= n.throttleNextAllowed) {
            throttleSendPending(idx);
        }
    }
    size_t kept = 0;
    for (size_t k = 0; k < throttleWaiting.size(); ++k) {
        NodeRuntime& n = nodes[static_cast<size_t>(throttleWaiting[k])];
        if (n.throttleHasPending) {
            throttleWaiting[kept++] = throttleWaiting[k];
        } else {
            n.throttleQueued = false;
        }
    }
    throttleWaiting.resize(kept);
}

// ---- T-193 (Docs/design/27-interaction.md §27.4.1): actions, timelines, system events ----------

void FlowRuntime::Impl::fireEventText(int nodeIndex, uint16_t slot, std::string_view text) {
    const uint32_t i = nodes[static_cast<size_t>(nodeIndex)].outputBase + slot;
    strings.buffer(slotHome(i)).assign(text);
    values[i] = RtValue{PortType::String, false, 0.0, slotHome(i)};
    markValuesStale(nodeIndex);  // T-389.
    fanOut(i, values[i]);
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
    event.uiId = currentUi != nullptr ? *currentUi : std::string();
    event.instanceId = currentInstance;
    event.nodeId = nodes[static_cast<size_t>(nodeIndex)].id;
    // A copy: the listener may replace itself (setHostEventListener) while being called.
    FlowHostEventListener listener = hostListener;
    listener(event);
}

void FlowRuntime::Impl::timelineInput(int nodeIndex, uint16_t slot, const RtValue& v) {
    const NodeRuntime& n = nodes[static_cast<size_t>(nodeIndex)];
    const std::string name = paramText(n, slots::timeline::kName);
    const std::string scope = paramText(n, slots::timeline::kScope);
    const std::string nodeId = n.id;
    const bool loop = paramBool(n, slots::timeline::kLoop, false);
    const auto rate = static_cast<float>(paramNum(n, slots::timeline::kRate, 1.0));
    for (DocEndpointState* state : statesFor(n.uiId)) {
        Status status = Status::kOk;
        if (slot == slots::timeline::kPlay) {
            PlayOptions options;
            options.loop = loop;
            options.rate = rate;
            status = state->doc->playTimeline(scope, name, options);
        } else if (slot == slots::timeline::kStop) {
            status = state->doc->stopTimeline(scope, name);
        } else if (slot == slots::timeline::kSeek) {
            status = state->doc->seekTimeline(scope, name, static_cast<float>(v.n));
        }
        if (status == Status::kNotFound && missingTimelines.insert(nodeId).second) {
            runtimeErrorList.push_back("node '" + nodeId + "': no @timeline '" + name + "'" +
                                       (scope.empty() ? std::string() : " in scope '" + scope + "'"));
        }
    }
}

void FlowRuntime::Impl::lifecycleEvent(const Context* context, DocumentHandle handle, bool shown) {
    if (!loaded) {
        return;
    }
    for (size_t i = 0; i < uiDocs.size(); ++i) {
        UiDoc& ui = uiDocs[i];
        if (ui.handle == handle && ui.state->context == context) {  // T-356: handles are per Context.
            const uint32_t external = shown ? ui.shownExternal : ui.hiddenExternal;
            if (external != kNoOutput) {
                deliverExternal(external, ui.state.get(), kPulseValue);
            }
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
    const std::string* savedUi = currentUi;
    const uint32_t savedInstance = currentInstance;
    currentUi = nullptr;
    currentInstance = 0;
    const std::string port(kind);
    const uint16_t slot = slotOf(NodeType::System, SlotKind::Output, kind);
    // T-262b (F12): the port's declared type (nodePortInfo, the table the bindings use) decides
    // pulse or string, not the kind's position in systemEventKinds(). System ports take no params.
    static const NodeDesc kNoParams;
    PortType portType = PortType::String;
    nodePortInfo(NodeType::System, kNoParams, port, /*isSource=*/true, portType);
    for (size_t i = 0; i < nodes.size() && slot != kNoSlot; ++i) {
        if (nodes[i].type != NodeType::System) {
            continue;
        }
        if (portType == PortType::Pulse) {
            firePulseOutput(static_cast<int>(i), slot);
        } else {
            fireEventText(static_cast<int>(i), slot, value);
        }
    }
    currentUi = savedUi;
    currentInstance = savedInstance;
    return Status::kOk;
}

}  // namespace xt::flow
