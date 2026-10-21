#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "xt/timeline.h"
#include "xt/types.h"
#include "xt/value.h"

/// Animator: the runtime driver for Track/Timeline playback, CSS transitions, host overrides and
/// their per-frame layering into node properties. See docs/design/04-timeline.md §4.5-4.9,
/// docs/design/12-compiler.md §12.3, docs/tasks/T-07.md.

namespace xt {

class NodeArena;

/// T-257b (fuzz_ir out-of-memory finding): at most this many per-iteration notes (onIteration
/// calls) per instance per Animator::evaluate(). When one evaluate() crosses more iterations (a
/// tiny or zero duration, a huge dt), the rest collapse into one final note for the last iteration
/// reached. A host can get more only with a sub-millisecond loop, where per-iteration callbacks are
/// meaningless. See docs/design/03 §3.13.
inline constexpr uint32_t kMaxIterationNotesPerUpdate = 64;

/// See docs/design/04-timeline.md §4.5.
enum class LoopMode : uint8_t {
    Once = 0,     ///< Play the segment once and stop.
    Loop = 1,     ///< Repeat the segment from the start each iteration.
    PingPong = 2  ///< Alternate direction each iteration.
};
/// Which end(s) of the play range keep applying their value outside it. See
/// docs/design/04-timeline.md §4.5.
enum class FillMode : uint8_t {
    None = 0,       ///< No fill: the instance contributes nothing before start or after finish.
    Forwards = 1,   ///< Keep the end value applied after the instance finishes.
    Backwards = 2,  ///< Apply the start value before the instance's delay has elapsed.
    Both = 3        ///< Both Forwards and Backwards.
};

/// Parameters for Animator::play(). See docs/tasks/T-07.md.
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
};

/// A dynamically created CSS-transition-style tween. See docs/design/04-timeline.md §4.6.
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
    /// Clock time (seconds) the transition was added at.
    float startClock;
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
/// properties every frame, applying the layering order of docs/design/04-timeline.md §4.7.
class Animator {
public:
    /// Capacities of `Animator`'s internal scratch buffers, for the steady-state
    /// zero-allocation test in docs/tasks/T-07.md.
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
    uint32_t play(const PlayParams& params, float clock);
    /// Immediately removes the instance; fill is NOT applied. No-op for an invalid id.
    void stop(uint32_t id);
    /// Pauses the instance at `clock`. No-op for an invalid or already-finished id.
    void pause(uint32_t id, float clock);
    /// Resumes a paused instance so its local time continues from where it was paused.
    void resume(uint32_t id, float clock);
    /// Sets startClock so the given clock corresponds to localT, and resets the iteration count
    /// (treated as iteration 0).
    void seek(uint32_t id, float localT, float clock);
    /// Keeps the current localT from jumping.
    void setSpeed(uint32_t id, float speed, float clock);
    /// True if the instance exists and is not finished.
    bool isActive(uint32_t id) const;
    /// True if the instance exists and is finished (still in the pool when fill has Forwards).
    bool isFinished(uint32_t id) const;
    /// Returns 0 if the instance does not exist.
    float localTime(uint32_t id, float clock) const;
    /// docs/tasks/T-70.md: the wall-clock length of one iteration of the instance's played segment,
    /// in seconds, i.e. `(segEnd - segStart) * durationScale`. Returns 0 if the instance does not
    /// exist. Lets a caller map a position given in playback seconds onto the timeline's own
    /// `localT` domain, which `seek()` takes.
    float segmentDuration(uint32_t id) const;
    /// docs/tasks/T-70.md: the instance's playback speed multiplier (negative = plays the timeline
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
    /// column (docs/design/20-agent-interface.md §20.5).
    bool hasOverride(NodeId node, PropId prop) const;
    /// The Value's external data (Path/FloatArray/Stops) must stay alive for as long as the
    /// override is set; the caller guarantees it.
    void setOverride(NodeId node, PropId prop, const Value& value);
    /// Removes an override previously set with `setOverride`. No-op if none is set.
    void clearOverride(NodeId node, PropId prop);
    /// Drops every transition and override targeting `node`, without firing onTransitionEnd.
    /// Called by Document::remove() before a node is destroyed: evaluate() would otherwise apply
    /// them to a freed slot (docs/tasks/T-35.md "remove").
    void clearNode(NodeId node);
    /// Drops every transition on `node` whose `prop` is in `[propLo, propHi]` inclusive, without
    /// firing onTransitionEnd (same "silent" contract as clearNode() above). Used by
    /// StyleEngine::resolve's `effect` cascade (docs/design/18-effects.md §18.4, docs/tasks/
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
    void evaluate(float clock, NodeArena& arena);

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

    /// Test-only introspection of scratch/pending vector capacities, used to verify evaluate()
    /// performs no allocations in steady state. See docs/tasks/T-07.md.
    DebugCapacities debugCapacities() const;

    /// Read-only snapshot of one live instance, for the devtools agent's `dump.animator`
    /// (docs/design/20-agent-interface.md §20.5). `localToArena`/`localCount` are the borrowed
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
    std::vector<InstanceInfo> debugInstances(float clock) const;

private:
    // 04.5 TimelineInstance plus bookkeeping needed by the exact evaluate() procedure of
    // docs/tasks/T-07.md ("evaluate 精確流程").
    struct Instance {
        uint32_t id = 0;
        TimelineId timeline = kInvalidTimeline;
        const NodeId* localToArena = nullptr;
        uint32_t localCount = 0;
        NodeId selfNode = kInvalidNode;
        float startClock = 0;
        float delay = 0;
        float speed = 1;
        float durationScale = 1;
        float segStart = 0, segEnd = 0;  // Resolved: segEnd is never the -1 sentinel here.
        LoopMode loop = LoopMode::Once;
        uint16_t iterations = 1;  // 0 = infinite; already forced to 1 when loop == Once.
        FillMode fill = FillMode::Forwards;
        uint8_t layer = 0;
        uint8_t group = 0;
        uint8_t paused = 0;
        float pausedAt = 0;
        uint8_t finished = 0;
        Easing overrideEasing;

        // Marker/iteration-event bookkeeping.
        uint8_t everEvaluated = 0;
        uint8_t finishedNotified = 0;  // onFinished already fired for the current finish
        float prevLocalT = 0;
        uint32_t prevIter = 0;   // Raw floor(elapsed/segLen) from the previous evaluate() call.
        uint32_t iterCount = 0;  // Highest iteration index reported via onIteration so far.
    };

    struct Override {
        NodeId node;
        PropId prop;
        Value value;
    };

    struct Pending {
        NodeId node;
        PropId prop;
        Value value;
    };

    // Result of evaluating an instance's local-time formula (docs/design/04-timeline.md §4.5).
    struct LocalState {
        float localT = 0;
        uint32_t iter = 0;
        bool finished = false;
        // True only for "not yet started, fill has no Backwards": the instance contributes
        // nothing this frame (no track writes, no marker/iteration events, no bookkeeping update).
        bool skip = false;
    };

    LocalState computeLocalState(const Instance& inst, float clock) const;
    Instance* findInstance(uint32_t id);
    const Instance* findInstance(uint32_t id) const;
    void growScratchForTimeline(const Timeline& tl);
    void growScratchFor(const Value& value);
    void growPendingCapacity(size_t additionalSlots);
    void growOrderCapacity();
    Value evalTrackAdvancing(const Timeline& tl, const Track& track, float t, const Easing* overrideLinear,
                             size_t& floatCursor, size_t& stopCursor);
    void checkMarkers(Instance& inst, const LocalState& st, const NodeArena& arena);
    /// T-260a: calls the queued notifications (m_notes) after step 6.
    void dispatchNotes(const NodeArena& arena);
    void gatherOrient(const Timeline& tl, const Track& track, float t, NodeId node);

    std::vector<const Timeline*> m_timelines;
    std::vector<EasingPoint> m_easingPoints;  // T-335: IR STYL pool, then interned runtime points.
    std::vector<Instance> m_instances;
    std::vector<Transition> m_transitions;
    std::vector<Override> m_overrides;  // Sorted by (node, prop); binary search.
    std::vector<Pending> m_pending;
    // T-228 auto-orient: per-node direction of motion gathered from this evaluate()'s Translate /
    // TranslateX / TranslateY tracks; resolved into NodeCommon::orientPathDeg after step 3.
    struct OrientDir {
        NodeId node;
        Vec2 dir;
        uint8_t has;  // Bit 0: x known, bit 1: y known.
    };
    std::vector<OrientDir> m_orient;
    // T-345: the nodes evaluate() flagged kInMotion last time (cleared at the next evaluate()).
    std::vector<NodeId> m_motion;
    std::vector<NodeId> m_motionPrev;  // T-345: evaluate()'s scratch (the previous m_motion).
    void noteMotion(NodeArena& arena, NodeId node, PropId prop);
    std::vector<float> m_scratchFloats;
    std::vector<GradientStop> m_scratchStops;
    std::vector<uint32_t> m_order;  // Instance sort indices for evaluate(); avoids moving instances.
    // T-38 (docs/design/01-architecture.md §1.7): evaluate()'s step-6 scratch, used to be locals.
    // Under MSVC's Debug STL (_ITERATOR_DEBUG_LEVEL == 2, the Debug default), constructing *any*
    // std::vector -- even one nothing is ever pushed into -- allocates a 16-byte _Container_proxy
    // for debug iterator tracking, so a local vector "allocates" on every evaluate() call purely
    // from being constructed. Members are constructed once (with the Animator) instead.
    std::vector<uint32_t> m_toRemove;
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

    uint32_t m_nextId = 1;
    size_t m_pendingCapacityNeeded = 0;
    size_t m_scratchFloatsNeeded = 0;
    size_t m_scratchStopsNeeded = 0;
};

}  // namespace xt
