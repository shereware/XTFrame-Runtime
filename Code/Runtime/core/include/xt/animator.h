#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "xt/dirty_cause.h"
#include "xt/timeline.h"
#include "xt/types.h"
#include "xt/value.h"

/// Animator: the runtime driver for Track/Timeline playback, CSS transitions, host overrides and
/// their per-frame layering into node properties. See Docs/design/04-timeline.md §4.5-4.9,
/// Docs/design/12-compiler.md §12.3, Docs/tasks/T-07.md.

namespace xt {

class NodeArena;
class TimelineExprSet;  // T-776: Code/Runtime/core/src/timeline_expr.h (private to core).

/// T-257b (fuzz_ir out-of-memory finding): at most this many per-iteration notes (onIteration
/// calls) per instance per Animator::evaluate(). When one evaluate() crosses more iterations (a
/// tiny or zero duration, a huge dt), the rest collapse into one final note for the last iteration
/// reached. A host can get more only with a sub-millisecond loop, where per-iteration callbacks are
/// meaningless. See Docs/design/03 §3.13.
inline constexpr uint32_t kMaxIterationNotesPerUpdate = 64;

/// See Docs/design/04-timeline.md §4.5.
enum class LoopMode : uint8_t {
    Once = 0,     ///< Play the segment once and stop.
    Loop = 1,     ///< Repeat the segment from the start each iteration.
    PingPong = 2  ///< Alternate direction each iteration.
};
/// Which end(s) of the play range keep applying their value outside it. See
/// Docs/design/04-timeline.md §4.5.
enum class FillMode : uint8_t {
    None = 0,       ///< No fill: the instance contributes nothing before start or after finish.
    Forwards = 1,   ///< Keep the end value applied after the instance finishes.
    Backwards = 2,  ///< Apply the start value before the instance's delay has elapsed.
    Both = 3        ///< Both Forwards and Backwards.
};

/// Parameters for Animator::play(). See Docs/tasks/T-07.md.
struct PlayParams {
    /// Timeline to play, as registered via `Animator::registerTimeline`.
    TimelineId timeline = kInvalidTimeline;
    /// Scene-local index -> actual NodeId table (provided by T-08's SceneInstance). Animator only
    /// borrows this pointer; the caller guarantees it stays alive for as long as the instance plays.
    const NodeId* localToArena = nullptr;
    /// Number of entries in `localToArena`.
    uint32_t localCount = 0;
    NodeId selfNode = kInvalidNode;  ///< Actual target when a track's target == kSelfNode.
    /// Seconds to wait, from `play()`'s clock, before the instance starts.
    float delay = 0;
    /// Playback speed multiplier (negative plays backwards).
    float speed = 1;
    /// Multiplier applied to the timeline's own duration.
    float durationScale = 1;
    /// Segment start time, in the timeline's own seconds.
    float segStart = 0;
    float segEnd = -1;  ///< -1 = timeline.duration.
    /// How the play range repeats.
    LoopMode loop = LoopMode::Once;
    /// 0 means infinite (the IR / API sentinel). A stylesheet's `animation` count of `0` (CSS: does
    /// not play) never reaches here: the compiler removes that item (T-272).
    uint16_t iterations = 1;
    /// Which end(s) of the play range keep applying outside it.
    FillMode fill = FillMode::Forwards;
    uint8_t layer = 0;  ///< 0 or 1.
    /// When `kind != Linear`, overrides the Linear-easing keyframes of the played Track (§12.3).
    Easing overrideEasing;
    /// 1 = a TimelineGroup member (04.10.1, T-172): evaluated after every other instance of the
    /// same layer, so the group wins over the element's own CSS animations on shared properties.
    uint8_t group = 0;
    /// T-747: 1 = a CSS `animation` item; its writes count as DirtyCause::Animation, every other
    /// instance's as DirtyCause::Timeline (57 §57.3).
    uint8_t css = 0;
    /// T-872: 1 = every iteration runs from segEnd to segStart (CSS `animation-direction: reverse`;
    /// with PingPong, `alternate-reverse`). Unlike a negative `speed` it keeps the delay, the
    /// iteration count and the fill of a forward instance; only the direction of each pass flips.
    uint8_t reverse = 0;
};

/// A dynamically created CSS-transition-style tween. See Docs/design/04-timeline.md §4.6.
struct Transition {
    /// The element that owns the transition (its `transition` declaration; TransitionEnd goes
    /// there). For an ordinary element transition this is also the node written.
    NodeId node;
    /// Property being transitioned.
    PropId prop;
    /// Value at the start of the transition.
    Value from;
    /// Value at the end of the transition.
    Value to;
    /// Clock time (seconds) the transition was added at (T-870: double, as the Context clock).
    double startClock;
    /// Seconds to wait before the transition starts.
    float delay;
    /// Transition duration in seconds.
    float duration;
    /// Easing curve applied over the transition.
    Easing easing;
    /// T-218 (31 §31.5.1): the node whose property is written -- an inline `<svg>` scene node for
    /// a scene-target rule's transition, `node` itself for an element's own. kInvalidNode means
    /// `node`.
    NodeId target = kInvalidNode;

    /// The node this transition writes (`target`, or `node` when unset).
    NodeId written() const { return target != kInvalidNode ? target : node; }
};

/// Evaluates Timeline playback (TimelineInstance), Transitions and host overrides into node
/// properties every frame, applying the layering order of Docs/design/04-timeline.md §4.7.
class Animator {
public:
    /// Capacities of `Animator`'s internal scratch buffers, for the steady-state
    /// zero-allocation test in Docs/tasks/T-07.md.
    struct DebugCapacities {
        /// Capacity of the pending property-write buffer.
        size_t pending = 0;
        /// Capacity of the scratch float buffer used while evaluating tracks.
        size_t scratchFloats = 0;
        /// Capacity of the scratch gradient-stop buffer used while evaluating tracks.
        size_t scratchStops = 0;
    };

    // Timeline pool: Animator does not own the Timeline's storage, only a pointer to it; the
    // caller guarantees it stays alive.
    /// Registers `timeline` and returns the id used to `play()` it.
    TimelineId registerTimeline(const Timeline* timeline);
    /// Returns the timeline registered as `id`, or `nullptr` if `id` is invalid.
    const Timeline* timeline(TimelineId id) const;

    /// Returns the new instance id (1-based; 0 is invalid; monotonically increasing, never reused),
    /// or 0 -- and starts nothing -- when `params.timeline` is not registered.
    uint32_t play(const PlayParams& params, double clock);
    /// Immediately removes the instance; fill is NOT applied. No-op for an invalid id.
    void stop(uint32_t id);
    /// Pauses the instance at `clock`. No-op for an invalid or already-finished id.
    void pause(uint32_t id, double clock);
    /// Resumes a paused instance so its local time continues from where it was paused.
    void resume(uint32_t id, double clock);
    /// Sets startClock so the given clock corresponds to localT, and resets the iteration count
    /// (treated as iteration 0).
    void seek(uint32_t id, float localT, double clock);
    /// Keeps the current localT from jumping.
    void setSpeed(uint32_t id, float speed, double clock);
    /// T-616 (04 §4.13, a CSS `animation` item whose identity did not change): replaces the
    /// instance's delay, speed, durationScale, segment, loop, iterations, fill, layer and
    /// overrideEasing with `params`' (its timeline, local-to-arena table, self node and group are
    /// kept; see retarget() for the timeline) and continues from the active seconds already
    /// elapsed (elapsedSeconds()): a new duration keeps the elapsed seconds, not the fraction, and
    /// an instance now past its last iteration finishes as its fill says on the next evaluate().
    /// The paused state is kept. False (nothing changed) for an unknown id or a timeline-less one.
    bool updateParams(uint32_t id, const PlayParams& params, double clock);
    /// T-616 (04 §4.13 item 4, a `@keyframes` whose content changed): the instance plays
    /// `timeline` from now on at the same local time; a segment end at the old timeline's duration
    /// moves to the new one's. False for an unknown id or an unregistered `timeline`.
    bool retarget(uint32_t id, TimelineId timeline);
    /// T-616: the seconds the instance has played since its delay ended, in its own direction (a
    /// negative-speed instance counts from its segment end, as applyAnimationDiff seeks a reversed
    /// CSS animation), not counting paused time; negative while its delay runs. 0 for an unknown id.
    /// T-651: computed in double, where the float differences are exact, so setElapsedSeconds() at
    /// the same clock puts the instance back bit for bit (a state snapshot's frames stay identical).
    double elapsedSeconds(uint32_t id, double clock) const;
    /// T-616: puts the instance at `seconds` of elapsedSeconds() (Document's runtime-state restore,
    /// 04 §4.13). Quiet: an instance this lands past its end counts as already finished (no
    /// onFinished), and no iteration or marker crossed by the jump is reported. No-op for an
    /// unknown id or a zero speed.
    void setElapsedSeconds(uint32_t id, double seconds, double clock);
    /// T-616: true if the instance exists and is paused.
    bool isPaused(uint32_t id) const;
    /// True if the instance exists and is not finished.
    bool isActive(uint32_t id) const;
    /// True if the instance exists and is finished (still in the pool when fill has Forwards).
    bool isFinished(uint32_t id) const;
    /// Returns 0 if the instance does not exist.
    float localTime(uint32_t id, double clock) const;
    /// Docs/tasks/T-70.md: the wall-clock length of one iteration of the instance's played segment,
    /// in seconds, i.e. `(segEnd - segStart) * durationScale`. Returns 0 if the instance does not
    /// exist. Lets a caller map a position given in playback seconds onto the timeline's own
    /// `localT` domain, which `seek()` takes.
    float segmentDuration(uint32_t id) const;
    /// Docs/tasks/T-70.md: the instance's playback speed multiplier (negative = plays the timeline
    /// backwards). Returns 0 if the instance does not exist.
    float speedOf(uint32_t id) const;

    /// T-335: replaces the easing point pool (the document's IR STYL `easingPoints[]`) that a
    /// LinearPoints transition, keyframe or override easing reads.
    void setEasingPoints(std::span<const EasingPoint> points);
    /// T-335: the index of `points` in the easing point pool, appended when no equal run is
    /// there yet (a runtime `linear()` from setStyle). False when the pool would pass 0xFFFF
    /// points (the Easing's u16 offset) or `points` has fewer than two.
    bool internEasingPoints(std::span<const EasingPoint> points, uint16_t& offset);
    /// T-335: the easing point pool.
    std::span<const EasingPoint> easingPoints() const { return m_easingPoints; }

    /// Same (node, prop) already tracked -> replaces it (the caller computes `from` for the
    /// §4.6 rule-3 "replace" case).
    void addTransition(const Transition& transition);
    /// Returns true if `node`/`prop` currently has an active transition.
    bool hasTransition(NodeId node, PropId prop) const;
    /// True if `node`/`prop` currently carries a host override (§4.7 layer 3). Read-only
    /// counterpart to setOverride()/clearOverride(), for the devtools agent's `dump.props` source
    /// column (Docs/design/20-agent-interface.md §20.5).
    bool hasOverride(NodeId node, PropId prop) const;
    /// The Value's external data (Path/FloatArray/Stops) must stay alive for as long as the
    /// override is set; the caller guarantees it.
    /// 04 §4.7 (T-545): a layer-4 override that may tween. duration <= 0 writes `value` at once.
    /// With duration > 0 the override animates from the current override value (or, when there is
    /// none, from the property's live value) to `value`; a new call during a tween restarts from
    /// the tweened value. The stored override is always the target; clearOverride() drops the
    /// tween with it. Only Float/Length/Color/Vec2 values tween (others are written at once), and
    /// the tween starts at the clock of the next evaluate().
    void setOverride(NodeId node, PropId prop, const Value& value, float duration = 0.0f,
                     Easing easing = Easing::linear());
    /// Removes an override previously set with `setOverride`. No-op if none is set.
    void clearOverride(NodeId node, PropId prop);
    /// Drops every transition and override targeting `node`, without firing onTransitionEnd.
    /// Called by Document::remove() before a node is destroyed: evaluate() would otherwise apply
    /// them to a freed slot (Docs/tasks/T-35.md "remove").
    void clearNode(NodeId node);
    /// Drops every transition on `node` whose `prop` is in `[propLo, propHi]` inclusive, without
    /// firing onTransitionEnd (same "silent" contract as clearNode() above). Used by
    /// StyleEngine::resolve's `effect` cascade (Docs/design/18-effects.md §18.4, Docs/tasks/
    /// T-51b.md) to cancel every in-flight effect-parameter transition (propLo/propHi =
    /// prop::kEffectPropBase/0xFFFF) when the effect sequence's kind changes and the new sequence
    /// replaces the old one outright instead of transitioning.
    void cancelTransitionsForNodeRange(NodeId node, PropId propLo, PropId propHi);

    /// §4.7 evaluation order. Finished instances are removed here and trigger onFinished.
    /// T-260a: onTransitionEnd / onIteration / onMarker are called after step 6 (before
    /// onFinished), in the order they occurred, never while the transitions and instances are
    /// being walked, so they may play, stop or clear freely. A notification whose node was
    /// destroyed by an earlier callback is dropped; one raised inside a callback (a nested
    /// evaluate()) is delivered by the next evaluate().
    void evaluate(double clock, NodeArena& arena);
    /// T-702 (19 §19.3): sets is3D = 1 again on every Element the last evaluate() wrote a PropId
    /// 16-22 to. A style resolve run after evaluate() in the same frame (Document::update's extra
    /// pass, e.g. for a `forwards` animation that just finished and stays in the pool) re-derives
    /// is3D from the cascade alone and would draw that frame flat. A transition's final write
    /// is left to that resolve (the transition is over).
    void reassertElement3D(NodeArena& arena);

    /// Called once when an instance finishes playing (all iterations complete).
    std::function<void(uint32_t id)> onFinished;
    /// Fired when iter increases, once per iteration crossed -- but at most
    /// kMaxIterationNotesPerUpdate + 1 times per instance per evaluate(): after the first
    /// kMaxIterationNotesPerUpdate iterations of one evaluate(), only the last iteration reached in
    /// that evaluate() is reported (the ones in between get no call of their own).
    std::function<void(uint32_t id, uint32_t iteration)> onIteration;
    /// Called when playback crosses a timeline marker.
    std::function<void(uint32_t id, StringId marker)> onMarker;
    /// Called when a transition finishes (naturally or via replacement).
    std::function<void(NodeId, PropId)> onTransitionEnd;

    /// Returns the number of instances that are still active (not yet finished/stopped).
    uint32_t activeInstanceCount() const;
    /// Returns the number of currently active transitions.
    uint32_t transitionCount() const;
    /// T-717 (10 §10.16 W4): the instances whose tracks the last evaluate() evaluated. A
    /// `@timeline` member held at its end stop is evaluated on the frame it gets there and then only
    /// when one of its target nodes was written or restyled since (see evaluate()). T-783: so is a
    /// paused instance wherever it stands (a group paused mid-way or held after a seek).
    uint32_t evaluatedInstanceCount() const { return m_evaluatedInstances; }
    /// T-749 (Docs/design/23-host-integration.md §23.ag item 6): false when a later evaluate(), at
    /// any clock, writes nothing, notifies nothing and leaves the "in motion" nodes as they are: the
    /// last evaluate() left every instance and override alone (each was settled -- a paused instance,
    /// a hold at an end stop, a finished `forwards` animation, an applied override without a tween --
    /// and none of their targets was touched), evaluated no transition, removed and finished nothing,
    /// queued no notification and kept the same nodes in motion; and since then nothing was played,
    /// stopped, moved, changed or added (every mutating call), no target of a settled instance or
    /// override was written or restyled, no expression program's input moved and no notification is
    /// waiting. Anything else (an instance in its delay or playing, a transition, a tweening override)
    /// is true.
    bool needsEvaluate(const NodeArena& arena) const;

    // T-776 (Docs/design/58-value-functions-and-drivers.md §58.5): timeline expression programs on
    // `@timeline` members. A program replaces one component of a track's interpolated value; it runs
    // when the member's group time moved, when an input it reads changed (a port, a watched layout
    // box, the port history, every update for another element's property) or when it is attached
    // or reseeded, and otherwise its last output is reused. A member held at its end stop with a
    // program that must run again runs it and writes only when an output changed.
    /// The Document's programs (Code/Runtime/core/src/timeline_expr.h); null = none.
    void setExpressions(const TimelineExprSet* set) { m_exprSet = set; }
    /// Runs program `expr` of the set on instance `id` from now on. False for an unknown id or
    /// no set.
    bool attachExpression(uint32_t id, uint32_t expr);
    /// The group time of member instance `id` (seconds); a change makes its programs run again.
    void setGroupTime(uint32_t id, float groupT);
    /// The per-play random seed of instance `id` (drawn at play); its programs run again.
    void setPlaySeed(uint32_t id, uint32_t seed);
    /// The programs the last evaluate() ran (FrameStats::exprEvaluated).
    uint32_t exprEvaluatedCount() const { return m_exprEvaluated; }

    /// T-654 (23 XF-097 version 4): the active transitions, in no particular order. Read-only;
    /// addTransition() restores one.
    std::span<const Transition> transitions() const { return m_transitions; }

    /// Test-only introspection of scratch/pending vector capacities, used to verify evaluate()
    /// performs no allocations in steady state. See Docs/tasks/T-07.md.
    DebugCapacities debugCapacities() const;

    /// Read-only snapshot of one live instance, for the devtools agent's `dump.animator`
    /// (Docs/design/20-agent-interface.md §20.5). `localToArena`/`localCount` are the borrowed
    /// scene-local -> NodeId table the instance was played with (see PlayParams).
    struct InstanceInfo {
        uint32_t id = 0;
        TimelineId timeline = kInvalidTimeline;
        NodeId selfNode = kInvalidNode;
        const NodeId* localToArena = nullptr;
        uint32_t localCount = 0;
        float localT = 0;
        float delay = 0;
        float speed = 1;
        float durationScale = 1;
        float segStart = 0;
        float segEnd = 0;
        LoopMode loop = LoopMode::Once;
        uint16_t iterations = 1;
        FillMode fill = FillMode::Forwards;
        uint8_t layer = 0;
        bool paused = false;
        bool finished = false;
    };
    /// Every instance currently in the pool (active, paused or finished-with-fill), in play order.
    /// `localT` is each instance's local time at `clock`. See InstanceInfo.
    std::vector<InstanceInfo> debugInstances(double clock) const;

private:
    /// T-776: one program on one track component of an instance, with its last run.
    struct ExprSlot {
        uint32_t expr = 0;  // Index in the TimelineExprSet.
        uint16_t track = 0;
        uint8_t component = 0;
        uint8_t valid = 0;  // `out` holds the output for input `in`.
        float in = 0;       // The track component the last run replaced.
        float out = 0;      // Its output.
    };

    // 04.5 TimelineInstance plus bookkeeping needed by the exact evaluate() procedure of
    // Docs/tasks/T-07.md ("evaluate 精確流程").
    struct Instance {
        uint32_t id = 0;
        TimelineId timeline = kInvalidTimeline;
        const NodeId* localToArena = nullptr;
        uint32_t localCount = 0;
        NodeId selfNode = kInvalidNode;
        double startClock = 0;
        float delay = 0;
        float speed = 1;
        float durationScale = 1;
        float segStart = 0, segEnd = 0;  // Resolved: segEnd is never the -1 sentinel here.
        LoopMode loop = LoopMode::Once;
        uint16_t iterations = 1;  // 0 = infinite; already forced to 1 when loop == Once.
        FillMode fill = FillMode::Forwards;
        uint8_t layer = 0;
        uint8_t group = 0;
        uint8_t css = 0;      // T-747: PlayParams::css.
        uint8_t reverse = 0;  // T-872: PlayParams::reverse.
        uint8_t paused = 0;
        double pausedAt = 0;  // T-870: double, as the clocks (an infinite animation's grows).
        uint8_t finished = 0;
        // T-717: 1 once a group member held at its end stop has applied its values there (cleared
        // when it leaves the stop and by every call that moves or changes it); `heldSkipped` is 1
        // when the last evaluate() left such a member alone (reassertElement3D() covers it).
        uint8_t held = 0;
        uint8_t heldSkipped = 0;
        // T-783 (Docs/tasks/T-783.md): 1 once a paused instance (any group member, wherever it
        // stands; a paused `<anim sync>` clip or CSS animation) or a T-717 / T-748 hold has applied
        // its values at its current state; cleared by every call that moves or changes it -- but not
        // by a seek() onto the position it already stands at (that seek still clears the T-717
        // `held` above, which decides "in motion", so pixel snapping is as before).
        uint8_t settled = 0;
        Easing overrideEasing;

        // Marker/iteration-event bookkeeping.
        uint8_t everEvaluated = 0;
        uint8_t finishedNotified = 0;  // onFinished already fired for the current finish
        float prevLocalT = 0;
        uint32_t prevIter = 0;   // Raw floor(elapsed/segLen) from the previous evaluate() call.
        uint32_t iterCount = 0;  // Highest iteration index reported via onIteration so far.

        // T-776: the expression programs on this member's tracks (sorted by track; allocated when
        // attached, never in evaluate()), its group time, the per-play seed, the inputs the programs
        // read (kExprInput* bits) and the input generations they last ran with.
        std::vector<ExprSlot> exprs;
        float groupT = 0;
        uint32_t playSeed = 0;
        uint16_t exprMask = 0;
        uint8_t exprDirty = 0;  // The group time, the seed or the program list changed.
        uint64_t exprPortGen = 0, exprLayoutGen = 0, exprHistoryGen = 0;
    };

    struct Override {
        NodeId node;
        PropId prop;
        Value value;  // The target.
        // T-545 tween state; all in the entry, so a tween allocates nothing.
        Value from;   // Valid when fromKnown.
        Value shown;  // The value the last evaluate() applied (valid when shownValid).
        double startClock = 0;
        float duration = 0;
        Easing easing{};
        uint8_t tweenState = 0;  // 0 = none, 1 = starts at the next evaluate(), 2 = running.
        bool fromKnown = false;
        bool shownValid = false;
        // T-748 (23 §23.ag): evaluate() applied `value` itself (no tween) and setOverride() has not
        // changed it since; while its target is not written or restyled the override writes nothing
        // (`skipped`: this evaluate() left it alone).
        bool applied = false;
        bool skipped = false;
    };

    struct Pending {
        NodeId node;
        PropId prop;
        Value value;
        /// T-702: the last write of a transition that ended this frame (reassertElement3D() leaves
        /// its node to the style resolve, as 19.3 says).
        bool ended = false;
        /// T-725: a PropId 16-22 write whose source (track, transition or override) can write a 3D
        /// component other than zero; only such a write makes its Element a 3D layer.
        bool may3D = true;
        /// T-747: who wrote it (Transition, Animation, Timeline or the override's HostApi).
        DirtyCause cause = DirtyCause::Animation;
    };

    // Result of evaluating an instance's local-time formula (Docs/design/04-timeline.md §4.5).
    struct LocalState {
        float localT = 0;
        uint32_t iter = 0;
        bool finished = false;
        // True only for "not yet started, fill has no Backwards": the instance contributes
        // nothing this frame (no track writes, no marker/iteration events, no bookkeeping update).
        bool skip = false;
        // T-869: in the delay of a Backwards fill (a Neutral start key writes nothing then).
        bool beforeStart = false;
    };

    // T-869 (12 §12.3): the base value a Neutral key of track `track` of instance `instance`
    // stands in for, when it is read back from the node (a property the Animator writes over its
    // only copy): `base` is the value seen when nothing but the cascade, a host or a layer below
    // had written it since -- i.e. whenever the node's value is not the one `applied` after the
    // last evaluate(). Entries of stopped instances are dropped by evaluate().
    struct NeutralBase {
        uint32_t instance = 0;
        uint32_t track = 0;
        NodeId node = kInvalidNode;
        PropId prop = 0;
        Value base{};
        Value applied{};
        uint8_t hasApplied = 0;
        uint8_t used = 0;  // Read this evaluate(): `applied` is taken after step 5.
    };

    LocalState computeLocalState(const Instance& inst, double clock) const;
    // T-616: elapsedSeconds() / setElapsedSeconds() of one instance, and the event bookkeeping
    // after such a jump (`quietFinish`: an instance now past its end counts as notified).
    double activeElapsed(const Instance& inst, double clock) const;
    void setActiveElapsed(Instance& inst, double seconds, double clock);
    void resyncAfterJump(Instance& inst, double clock, bool quietFinish);
    /// T-870 (59 §59.7): the instance's elapsed seconds at `clock` (its paused position while
    /// paused), in double as the clocks: a float holds an infinite animation's 36000 s to 4 ms.
    static double elapsedAt(const Instance& inst, double clock) {
        return inst.paused != 0 ? inst.pausedAt : (clock - inst.startClock - inst.delay) * inst.speed;
    }
    Instance* findInstance(uint32_t id);
    const Instance* findInstance(uint32_t id) const;
    void growScratchForTimeline(const Timeline& tl);
    void growScratchFor(const Value& value);
    void growPendingCapacity(size_t additionalSlots);
    void growOrderCapacity();
    /// `base`: T-869, evalTrackWithBase()'s stand-in for a Neutral key (null: evalTrack()).
    Value evalTrackAdvancing(const Timeline& tl, const Track& track, float t, const Easing* overrideLinear,
                             size_t& floatCursor, size_t& stopCursor, const Value* base = nullptr);
    /// T-869 (12 §12.3, 04 §4.7; animator_neutral.cpp): for a track with a Neutral end, the value
    /// its Neutral key stands for -- what the property of `node` is without this instance: the
    /// write a lower layer queued this evaluate() (a transition, an earlier instance), else an
    /// Element's cascade value for a general property, else the node's value tracked by a
    /// NeutralBase entry; scaled px, as the node holds it. Value::neutral() for a track without a
    /// Neutral end, or when there is none (a pool-backed value, a missing effect).
    Value neutralBase(const Instance& inst, const Timeline& tl, const Track& track, NodeId node,
                      NodeArena& arena);
    /// T-869: after step 5, the value each NeutralBase entry read this evaluate() now holds.
    void neutralBasesApplied(const NodeArena& arena);
    /// T-869: drops the NeutralBase entries of `instance`, or (0) of every instance not in the pool.
    void dropNeutralBases(uint32_t instance);
    /// T-869: a CSS animation that ends without a `forwards` fill leaves its Elements' general
    /// properties (opacity, the transform components) at their cascade values again -- the restyle
    /// that follows keeps those values apart from what the Animator wrote, so it would not.
    void restoreCascadeValues(const Instance& inst, NodeArena& arena);
    void checkMarkers(Instance& inst, const LocalState& st, const NodeArena& arena);
    /// T-260a: calls the queued notifications (m_notes) after step 6.
    void dispatchNotes(const NodeArena& arena);
    void gatherOrient(const Timeline& tl, const Track& track, float t, NodeId node, DirtyCause cause);
    /// The arena node `track` of `inst` writes, or kInvalidNode.
    static NodeId trackNode(const Instance& inst, const Track& track);
    /// T-717: whether a held member must apply its values again this frame -- one of its target
    /// nodes is gone, was written or restyled since the Animator last applied, or already has a
    /// write queued earlier in this evaluate() that the member's would have replaced.
    bool heldTargetsTouched(const Instance& inst, const Timeline& tl, const NodeArena& arena) const;
    /// T-783: noteMotion() for what a held instance left alone this frame would have marked "in
    /// motion" had it written (its motion tracks whose value at `localT` is not neutral).
    void noteHeldMotion(const Instance& inst, const Timeline& tl, float localT, NodeArena& arena);
    /// T-748: the same test for one target and property (a settled override).
    bool targetTouched(NodeId node, PropId prop, const NodeArena& arena) const;
    /// T-725: m_trackForces3D's entry for `track` (one of `tl`'s, registered as `id`).
    bool trackForces3D(TimelineId id, const Timeline& tl, const Track& track) const;
    /// T-776: whether `inst`'s programs must run this evaluate() (time, seed or an input moved).
    bool exprsStale(const Instance& inst) const;
    /// T-776: notes that `inst`'s programs ran with the current inputs.
    void exprsRan(Instance& inst) const;
    /// T-776: replaces the components of `v` (track `trackIndex` of `inst`) that have a program,
    /// running each program when `stale` or its input component changed. Returns whether an
    /// output differs from the one it replaced last time. `cursor` walks inst.exprs.
    bool applyExprs(Instance& inst, size_t trackIndex, Value& v, bool stale, size_t& cursor);
    /// T-776: a held member's programs run without writing (local time `localT`); whether an
    /// output changed.
    bool probeHeldExprs(Instance& inst, const Timeline& tl, float localT);

    std::vector<const Timeline*> m_timelines;
    /// T-725 (19 §19.x ruling 2): per registered timeline, per track, 1 when the track writes a 3D
    /// component (PropId 16-22) with a keyframe other than that component's identity -- only such a
    /// track makes its Element a 3D layer. Decided once in registerTimeline().
    std::vector<std::vector<uint8_t>> m_trackForces3D;
    std::vector<EasingPoint> m_easingPoints;  // T-335: IR STYL pool, then interned runtime points.
    std::vector<Instance> m_instances;
    std::vector<Transition> m_transitions;
    std::vector<Override> m_overrides;  // Sorted by (node, prop); binary search.
    std::vector<Pending> m_pending;
    std::vector<NeutralBase> m_neutralBases;  // T-869
    // T-228 auto-orient: per-node direction of motion gathered from this evaluate()'s Translate /
    // TranslateX / TranslateY tracks; resolved into NodeCommon::orientPathDeg after step 3.
    struct OrientDir {
        NodeId node;
        Vec2 dir;
        uint8_t has;       // Bit 0: x known, bit 1: y known.
        DirtyCause cause;  // T-747: the instance's (Animation or Timeline).
    };
    std::vector<OrientDir> m_orient;
    // T-345: the nodes evaluate() flagged kInMotion last time (cleared at the next evaluate()).
    std::vector<NodeId> m_motion;
    std::vector<NodeId> m_motionPrev;  // T-345: evaluate()'s scratch (the previous m_motion).
    void noteMotion(NodeArena& arena, NodeId node, PropId prop);
    std::vector<float> m_scratchFloats;
    std::vector<GradientStop> m_scratchStops;
    std::vector<uint32_t> m_order;  // Instance sort indices for evaluate(); avoids moving instances.
    // T-38 (Docs/design/01-architecture.md §1.7): evaluate()'s step-6 scratch, used to be locals.
    // Under MSVC's Debug STL (_ITERATOR_DEBUG_LEVEL == 2, the Debug default), constructing *any*
    // std::vector -- even one nothing is ever pushed into -- allocates a 16-byte _Container_proxy
    // for debug iterator tracking, so a local vector "allocates" on every evaluate() call purely
    // from being constructed. Members are constructed once (with the Animator) instead.
    std::vector<uint32_t> m_toRemove;
    // T-717: the nodes a held member evaluated this update targets (their kAnimValueStale bit is
    // cleared after step 5 with the written nodes'), and the count of evaluatedInstanceCount().
    std::vector<NodeId> m_heldTargets;
    uint32_t m_evaluatedInstances = 0;
    const TimelineExprSet* m_exprSet = nullptr;  // T-776: setExpressions().
    uint32_t m_exprEvaluated = 0;                // T-776: exprEvaluatedCount().
    std::vector<uint32_t> m_toNotify;
    // T-260a: the transition-end / iteration / marker notifications of evaluate(), in the order
    // they occurred, called by dispatchNotes() from m_notesOut (swapped with m_notes, so one a
    // callback raises waits for the next evaluate()). Members, so their capacity is kept.
    enum class NoteKind : uint8_t { TransitionEnd, Iteration, Marker };
    struct Note {
        NoteKind kind;
        NodeId node;          // TransitionEnd: the owning element; Iteration / Marker: selfNode.
        uint32_t generation;  // `node`'s generation when queued (a reused slot is another node).
        PropId prop;          // TransitionEnd.
        uint32_t id;          // Iteration / Marker: the instance.
        uint32_t iteration;   // Iteration.
        StringId marker;      // Marker.
        // Iteration: iterations before this one that got no note of their own (0 unless the
        // per-update bound kMaxIterationNotesPerUpdate was hit).
        uint32_t skipped;
    };
    std::vector<Note> m_notes;
    std::vector<Note> m_notesOut;
    bool m_dispatchingNotes = false;
    // T-260a: transitions an earlier callback of the batch cleared (clearNode() /
    // cancelTransitionsForNodeRange() while dispatching): a later TransitionEnd note for that node
    // and property is dropped, like an Iteration / Marker note whose instance a callback stopped.
    struct ClearedTransitions {
        NodeId node;
        PropId propLo;
        PropId propHi;
    };
    std::vector<ClearedTransitions> m_clearedWhileDispatching;
    /// T-260a: whether `note` still describes something that exists (see dispatchNotes()).
    bool noteStillValid(const Note& note, const NodeArena& arena) const;

    // T-749 (needsEvaluate()): a mutating call since the last evaluate() began (set again by one a
    // callback of that evaluate() makes), and whether the last evaluate() left everything alone.
    bool m_changedSinceEvaluate = true;
    bool m_lastEvaluateIdle = false;

    uint32_t m_nextId = 1;
    size_t m_pendingCapacityNeeded = 0;
    size_t m_scratchFloatsNeeded = 0;
    size_t m_scratchStopsNeeded = 0;
};

}  // namespace xt
