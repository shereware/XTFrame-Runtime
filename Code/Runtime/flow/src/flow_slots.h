#pragma once

#include <cstdint>
#include <span>
#include <string_view>

/// T-476 (Docs/design/27-interaction.md §27.4.2 item 1): every node type's fixed input / output /
/// parameter slot table. A port or parameter name becomes a slot index once, when a graph is
/// loaded (or at an editor API boundary); FlowRuntime's tick only works with the indices.
/// Output order is outputPortOrder()'s (values() / valueNames() order, Docs/tasks/T-56.md).

namespace xt::flow {

/// One of the fourteen signal node types (Docs/design/17-editor.md §17.6's table), parsed from
/// NodeDesc::type. Radio/Lookup/Merge/Format are T-123's additions; System .. Timeline are T-193's
/// (Docs/design/27-interaction.md §27.4.1).
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
    Timeline,
    State,   // T-541 (27 §27.4.4).
    Filter,  // T-543.
    Throttle
};

/// Which of a node's three slot tables a name is looked up in.
enum class SlotKind : uint8_t { Input, Output, Param };

/// "No such slot".
inline constexpr uint16_t kNoSlot = 0xFFFF;

/// One node type's three name tables. `inputs` of `radio` lists only "set": its "set<i>" pulse
/// inputs are slots 1 + i (see slotOf()).
struct NodeSlotTable {
    std::span<const std::string_view> inputs;
    std::span<const std::string_view> outputs;
    std::span<const std::string_view> params;
};

namespace slot_names {
// clang-format off
inline constexpr std::string_view kOut[] = {"out"};
inline constexpr std::string_view kValue[] = {"value"};
inline constexpr std::string_view kTrigger[] = {"trigger"};
inline constexpr std::string_view kIn[] = {"in"};
inline constexpr std::string_view kAB[] = {"a", "b"};
inline constexpr std::string_view kOp[] = {"op"};
inline constexpr std::string_view kFormatParams[] = {"format", "decimals"};
inline constexpr std::string_view kInitial[] = {"initial"};
inline constexpr std::string_view kCue[] = {"cue"};

inline constexpr std::string_view kCounterIn[] = {"increment", "decrement", "reset", "set"};
inline constexpr std::string_view kCounterParams[] = {"step", "initial", "min", "max"};
inline constexpr std::string_view kToggleIn[] = {"toggle", "setTrue", "setFalse"};
inline constexpr std::string_view kToggleOut[] = {"value", "changed"};
inline constexpr std::string_view kTimerIn[] = {"start", "stop"};
inline constexpr std::string_view kTimerOut[] = {"tick", "count"};
inline constexpr std::string_view kTimerParams[] = {"interval", "autostart", "repeat"};
inline constexpr std::string_view kClockOut[] = {"seconds"};
inline constexpr std::string_view kRandomParams[] = {"min", "max", "integer"};
inline constexpr std::string_view kMathParams[] = {"op", "lo", "hi", "inLo", "inHi", "outLo", "outHi"};
inline constexpr std::string_view kCompareOut[] = {"out", "true", "false"};
inline constexpr std::string_view kSelectIn[] = {"cond", "a", "b"};
inline constexpr std::string_view kRadioIn[] = {"set"};
inline constexpr std::string_view kRadioOut[] = {"index", "changed"};
inline constexpr std::string_view kRadioParams[] = {"count", "initial"};
inline constexpr std::string_view kLookupIn[] = {"index"};
inline constexpr std::string_view kFormatIn[] = {"a", "b", "c", "d"};
inline constexpr std::string_view kSystemOut[] = {"loading",  "paused",      "resumed", "disconnected",
                                                  "languageChanged", "platform", "inputDevice", "back",
                                                  "reducedMotion"};  // T-240, T-330.
inline constexpr std::string_view kSoundParams[] = {"cue", "volume"};
inline constexpr std::string_view kSpawnParams[] = {"symbol"};
inline constexpr std::string_view kEmitIn[] = {"trigger", "value"};
inline constexpr std::string_view kEmitParams[] = {"port"};
inline constexpr std::string_view kTimelineIn[] = {"play", "stop", "seek"};
inline constexpr std::string_view kTimelineOut[] = {"end", "marker"};
inline constexpr std::string_view kTimelineParams[] = {"name", "scope", "ui", "loop", "rate"};
// T-541: `state`'s fixed ports. Its event inputs (slot 2 + event index) and enter / exit outputs
// (slot 3 + 2 * state index, +1 for the exit) are per node: stateSlotOf() (flow_internal.h).
inline constexpr std::string_view kStateIn[] = {"set", "guard"};
inline constexpr std::string_view kStateOut[] = {"state", "index", "changed"};
inline constexpr std::string_view kStateParams[] = {"states", "initial", "transitions", "events"};
inline constexpr std::string_view kFilterIn[] = {"in", "trigger", "reset"};  // T-543.
inline constexpr std::string_view kFilterOut[] = {"out", "passed"};
inline constexpr std::string_view kFilterParams[] = {"op", "threshold", "step"};
inline constexpr std::string_view kThrottleIn[] = {"in", "rate", "reset"};
inline constexpr std::string_view kThrottleOut[] = {"out", "passed", "dropped"};
inline constexpr std::string_view kThrottleParams[] = {"unit", "rate", "trailing"};
// clang-format on
}  // namespace slot_names

/// `type`'s slot tables.
constexpr NodeSlotTable nodeSlotTable(NodeType type) {
    using namespace slot_names;
    using S = std::span<const std::string_view>;
    switch (type) {
        case NodeType::Constant:
            return {S{}, S{kOut}, S{kValue}};
        case NodeType::Counter:
            return {S{kCounterIn}, S{kValue}, S{kCounterParams}};
        case NodeType::Toggle:
            return {S{kToggleIn}, S{kToggleOut}, S{kInitial}};
        case NodeType::Timer:
            return {S{kTimerIn}, S{kTimerOut}, S{kTimerParams}};
        case NodeType::Clock:
            return {S{}, S{kClockOut}, S{}};
        case NodeType::Random:
            return {S{kTrigger}, S{kValue}, S{kRandomParams}};
        case NodeType::Math:
            return {S{kAB}, S{kOut}, S{kMathParams}};
        case NodeType::Compare:
            return {S{kAB}, S{kCompareOut}, S{kOp}};
        case NodeType::ToString:
            return {S{kIn}, S{kOut}, S{kFormatParams}};
        case NodeType::Select:
            return {S{kSelectIn}, S{kOut}, S{}};
        case NodeType::Radio:
            return {S{kRadioIn}, S{kRadioOut}, S{kRadioParams}};
        case NodeType::Lookup:
            return {S{kLookupIn}, S{kOut}, S{}};
        case NodeType::Merge:
            return {S{kIn}, S{kOut}, S{}};
        case NodeType::Format:
            return {S{kFormatIn}, S{kOut}, S{kFormatParams}};
        case NodeType::System:
            return {S{}, S{kSystemOut}, S{}};
        case NodeType::Sound:
            return {S{kTrigger}, S{}, S{kSoundParams}};
        case NodeType::Haptic:
            return {S{kTrigger}, S{}, S{kCue}};
        case NodeType::Spawn:
            return {S{kTrigger}, S{}, S{kSpawnParams}};
        case NodeType::Emit:
            return {S{kEmitIn}, S{}, S{kEmitParams}};
        case NodeType::Timeline:
            return {S{kTimelineIn}, S{kTimelineOut}, S{kTimelineParams}};
        case NodeType::State:
            return {S{kStateIn}, S{kStateOut}, S{kStateParams}};
        case NodeType::Filter:
            return {S{kFilterIn}, S{kFilterOut}, S{kFilterParams}};
        case NodeType::Throttle:
            return {S{kThrottleIn}, S{kThrottleOut}, S{kThrottleParams}};
    }
    return {};
}

/// The slot of `name` in `type`'s `kind` table, or kNoSlot. `radio`'s "set<i>" input (no leading
/// zero, i < 64) is slot 1 + i. `state`'s event / enter / exit ports are not found here (they
/// depend on the node's params; see stateSlotOf()). Only for loading and the editor API boundary:
/// never on the tick.
constexpr uint16_t slotOf(NodeType type, SlotKind kind, std::string_view name) {
    const NodeSlotTable table = nodeSlotTable(type);
    const std::span<const std::string_view> names =
        kind == SlotKind::Input ? table.inputs : (kind == SlotKind::Output ? table.outputs : table.params);
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i] == name) {
            return static_cast<uint16_t>(i);
        }
    }
    if (type == NodeType::Radio && kind == SlotKind::Input && name.size() >= 4 && name.size() <= 5 &&
        name.substr(0, 3) == "set" && !(name[3] == '0' && name.size() != 4)) {
        uint32_t index = 0;
        for (size_t i = 3; i < name.size(); ++i) {
            if (name[i] < '0' || name[i] > '9') {
                return kNoSlot;
            }
            index = index * 10 + static_cast<uint32_t>(name[i] - '0');
        }
        return index < 64 ? static_cast<uint16_t>(1 + index) : kNoSlot;
    }
    return kNoSlot;
}

/// How many input slots a node of `type` has (`radio`: "set" plus one per "set<i>"; `state`: only
/// its fixed ones, its event inputs come on top).
constexpr uint32_t inputSlotCount(NodeType type, uint32_t radioCount) {
    return type == NodeType::Radio ? 1 + radioCount
                                   : static_cast<uint32_t>(nodeSlotTable(type).inputs.size());
}

/// The slot indices the runtime uses, per node type (each matching the table above).
namespace slots {
namespace counter {
inline constexpr uint16_t kIncrement = 0, kDecrement = 1, kReset = 2, kSet = 3;
inline constexpr uint16_t kValue = 0;
inline constexpr uint16_t kStep = 0, kInitial = 1, kMin = 2, kMax = 3;
}  // namespace counter
namespace toggle {
inline constexpr uint16_t kToggle = 0, kSetTrue = 1, kSetFalse = 2;
inline constexpr uint16_t kValue = 0, kChanged = 1;
inline constexpr uint16_t kInitial = 0;
}  // namespace toggle
namespace timer {
inline constexpr uint16_t kStart = 0, kStop = 1;
inline constexpr uint16_t kTick = 0, kCount = 1;
inline constexpr uint16_t kInterval = 0, kAutostart = 1, kRepeat = 2;
}  // namespace timer
namespace random {
inline constexpr uint16_t kTrigger = 0;
inline constexpr uint16_t kValue = 0;
inline constexpr uint16_t kMin = 0, kMax = 1, kInteger = 2;
}  // namespace random
namespace math {
inline constexpr uint16_t kA = 0, kB = 1;
inline constexpr uint16_t kOut = 0;
inline constexpr uint16_t kOp = 0, kLo = 1, kHi = 2, kInLo = 3, kInHi = 4, kOutLo = 5, kOutHi = 6;
}  // namespace math
namespace compare {
inline constexpr uint16_t kA = 0, kB = 1;
inline constexpr uint16_t kOut = 0, kTrue = 1, kFalse = 2;
inline constexpr uint16_t kOp = 0;
}  // namespace compare
namespace format {  // `format` and `toString` (whose single input "in" is slot 0).
inline constexpr uint16_t kOut = 0;
inline constexpr uint16_t kFormat = 0, kDecimals = 1;
}  // namespace format
namespace select {
inline constexpr uint16_t kCond = 0, kA = 1, kB = 2;
inline constexpr uint16_t kOut = 0;
}  // namespace select
namespace radio {
inline constexpr uint16_t kSet = 0;  // "set<i>" = 1 + i.
inline constexpr uint16_t kIndex = 0, kChanged = 1;
inline constexpr uint16_t kCount = 0, kInitial = 1;
}  // namespace radio
namespace lookup {
inline constexpr uint16_t kIndex = 0;
inline constexpr uint16_t kOut = 0;
}  // namespace lookup
namespace merge {
inline constexpr uint16_t kIn = 0;
inline constexpr uint16_t kOut = 0;
}  // namespace merge
namespace action {  // sound / haptic / spawn / emit: "trigger" is input 0; the name param is 0.
inline constexpr uint16_t kTrigger = 0, kEmitValue = 1;
inline constexpr uint16_t kName = 0, kVolume = 1;
}  // namespace action
namespace timeline {
inline constexpr uint16_t kPlay = 0, kStop = 1, kSeek = 2;
inline constexpr uint16_t kEnd = 0, kMarker = 1;
inline constexpr uint16_t kName = 0, kScope = 1, kUi = 2, kLoop = 3, kRate = 4;
}  // namespace timeline
namespace state {
inline constexpr uint16_t kSet = 0, kGuard = 1, kFirstEvent = 2;
inline constexpr uint16_t kState = 0, kIndex = 1, kChanged = 2, kFirstStatePort = 3;
inline constexpr uint16_t kStates = 0, kInitial = 1, kTransitions = 2, kEvents = 3;
/// The output slot of `enter<state s>` / `exit<state s>`.
constexpr uint16_t enterSlot(uint32_t s) { return static_cast<uint16_t>(kFirstStatePort + 2 * s); }
constexpr uint16_t exitSlot(uint32_t s) { return static_cast<uint16_t>(kFirstStatePort + 2 * s + 1); }
}  // namespace state
namespace filter {  // T-543 (27 §27.4.5).
inline constexpr uint16_t kIn = 0, kTrigger = 1, kReset = 2;
inline constexpr uint16_t kOut = 0, kPassed = 1;
inline constexpr uint16_t kOp = 0, kThreshold = 1, kStep = 2;
}  // namespace filter
namespace throttle {
inline constexpr uint16_t kIn = 0, kRate = 1, kReset = 2;
inline constexpr uint16_t kOut = 0, kPassed = 1, kDropped = 2;
inline constexpr uint16_t kUnit = 0, kRateParam = 1, kTrailing = 2;
}  // namespace throttle
}  // namespace slots

// The slot constants above must name the table entries they stand for.
static_assert(slotOf(NodeType::Counter, SlotKind::Input, "set") == slots::counter::kSet);
static_assert(slotOf(NodeType::Counter, SlotKind::Param, "max") == slots::counter::kMax);
static_assert(slotOf(NodeType::Toggle, SlotKind::Output, "changed") == slots::toggle::kChanged);
static_assert(slotOf(NodeType::Timer, SlotKind::Param, "repeat") == slots::timer::kRepeat);
static_assert(slotOf(NodeType::Timer, SlotKind::Output, "count") == slots::timer::kCount);
static_assert(slotOf(NodeType::Random, SlotKind::Param, "integer") == slots::random::kInteger);
static_assert(slotOf(NodeType::Math, SlotKind::Param, "outHi") == slots::math::kOutHi);
static_assert(slotOf(NodeType::Compare, SlotKind::Output, "false") == slots::compare::kFalse);
static_assert(slotOf(NodeType::Format, SlotKind::Param, "decimals") == slots::format::kDecimals);
static_assert(slotOf(NodeType::ToString, SlotKind::Param, "decimals") == slots::format::kDecimals);
static_assert(slotOf(NodeType::Select, SlotKind::Input, "b") == slots::select::kB);
static_assert(slotOf(NodeType::Radio, SlotKind::Input, "set63") == 64);
static_assert(slotOf(NodeType::Radio, SlotKind::Input, "set01") == kNoSlot);
static_assert(slotOf(NodeType::Radio, SlotKind::Param, "initial") == slots::radio::kInitial);
static_assert(slotOf(NodeType::Emit, SlotKind::Input, "value") == slots::action::kEmitValue);
static_assert(slotOf(NodeType::Sound, SlotKind::Param, "volume") == slots::action::kVolume);
static_assert(slotOf(NodeType::Timeline, SlotKind::Param, "rate") == slots::timeline::kRate);
static_assert(slotOf(NodeType::Timeline, SlotKind::Input, "seek") == slots::timeline::kSeek);
static_assert(slotOf(NodeType::State, SlotKind::Input, "set") == slots::state::kSet);
static_assert(slotOf(NodeType::State, SlotKind::Input, "guard") == slots::state::kGuard);
static_assert(nodeSlotTable(NodeType::State).inputs.size() == slots::state::kFirstEvent);
static_assert(slotOf(NodeType::State, SlotKind::Output, "state") == slots::state::kState);
static_assert(slotOf(NodeType::State, SlotKind::Output, "index") == slots::state::kIndex);
static_assert(slotOf(NodeType::State, SlotKind::Output, "changed") == slots::state::kChanged);
static_assert(nodeSlotTable(NodeType::State).outputs.size() == slots::state::kFirstStatePort);
static_assert(slotOf(NodeType::State, SlotKind::Param, "states") == slots::state::kStates);
static_assert(slotOf(NodeType::State, SlotKind::Param, "initial") == slots::state::kInitial);
static_assert(slotOf(NodeType::State, SlotKind::Param, "transitions") == slots::state::kTransitions);
static_assert(slotOf(NodeType::State, SlotKind::Param, "events") == slots::state::kEvents);
static_assert(slotOf(NodeType::Filter, SlotKind::Input, "reset") == slots::filter::kReset);
static_assert(slotOf(NodeType::Filter, SlotKind::Output, "passed") == slots::filter::kPassed);
static_assert(slotOf(NodeType::Filter, SlotKind::Param, "step") == slots::filter::kStep);
static_assert(slotOf(NodeType::Throttle, SlotKind::Input, "rate") == slots::throttle::kRate);
static_assert(slotOf(NodeType::Throttle, SlotKind::Output, "dropped") == slots::throttle::kDropped);
static_assert(slotOf(NodeType::Throttle, SlotKind::Param, "trailing") == slots::throttle::kTrailing);

}  // namespace xt::flow
