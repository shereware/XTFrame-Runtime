#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "event_dispatcher.h"
#include "xt/input_channels.h"
#include "xt/ir_pod.h"
#include "xt/timeline.h"
#include "xt/types.h"

/// Trigger execution (Docs/design/27-interaction.md §27.2, 03 §3.12.3, 08 §8.9, Docs/tasks/T-174.md).
/// Private to core; Document owns one TriggerRuntime behind a unique_ptr. The dispatcher reports
/// every delivered event through the TriggerSink interface; matching triggers are queued and run
/// after the input event is fully handled (so a trigger's `focus()` happens after the default
/// focus change of a press), or at once outside input dispatch. `then` chains that wait for a
/// timeline end are kept as waiters and resumed from Document::flushTimelineEvents().

namespace xt {

class Document;

class TriggerRuntime final : public TriggerSink {
public:
    explicit TriggerRuntime(Document& doc) : m_doc(doc) {}

    /// Binds every TRIG table to its element (NodeCommon::triggerTable) and indexes the timeline
    /// triggers. Called once by Document::loadInternal after the element tree exists.
    void load();

    // TriggerSink.
    void onDomEvent(const Event& event, NodeId node) override;
    void onCheckedChanged(NodeId node, bool checked) override;
    // T-194 (08.9, 27.7).
    void onFocusMoved(NodeId node, uint8_t player) override;
    // T-573 (27.8).
    void onGamepadKey(NodeId target, uint8_t player) override;
    void onDragState(NodeId node, DragStateKind kind, bool on) override;
    void onDrop(const Event& event) override;
    uint32_t itemIndexOf(NodeId node) override;
    void onAxisValue(NodeId node, bool nav, float value, uint8_t player) override;
    // T-394 (08.15).
    bool consumesOutsidePress(NodeId node) override;
    bool hasPointerDrag(NodeId node) override;
    uint32_t dropIndexOf(NodeId target, Vec2 pos, uint32_t targetItem, Vec2 fraction) override;
    // T-871 (60 §60.3.3; trigger_pointer.cpp).
    void onPointerSample(PointerSample kind, Vec2 pos) override;
    bool consumesWheel(NodeId node) override;

    /// T-873 (60 §60.3.2; trigger_channels.cpp): fires the `input(<channel>)`, `.press` and
    /// `.release` triggers of the channels the Document's last commit changed, once each.
    void fireChannels();

    /// T-871: one element's `pointer.turn` / `pointer.grab` state of the press in progress, as a
    /// snapshot keeps it (Document::snapshotState()).
    struct TrackState {
        std::string id;  // The element's (path) id.
        float turn = 0;
        Vec2 grab{};
    };
    /// T-871: the tracked elements' states while a press is in progress (empty otherwise).
    std::vector<TrackState> trackStates() const;
    /// T-871: after a restored pointer state: with a press in progress, the tracked elements start
    /// again from `pos` with the turn and grab point of `states` (an element they do not list: 0
    /// and its grab point at `press`); without one, nothing is tracked.
    void restoreTracks(const std::vector<TrackState>& states, bool pressed, Vec2 pos, Vec2 press);

    /// Queues every trigger of `node` for `event` whose arguments match, then runs the queue unless
    /// deferred or already running. `value` is the compared value of an `if` (0 for T-174 events).
    /// T-175: a Port event matches on arg1 (PORT index + 1); `port` is that PORT index, whose
    /// current value the `if` conditions then compare.
    /// T-194: `player` is the local player whose input fired the event (actions such as `focus()`
    /// and `haptic()` act for that player).
    void fire(NodeId node, TriggerEvent event, uint32_t arg0 = kInvalidString, uint32_t arg1 = kInvalidString,
              float value = 0.0f, uint32_t port = 0xFFFFFFFFu, uint8_t player = 0);
    /// A `@timeline` group ended or crossed a marker: resumes the chains waiting for its end and
    /// fires the `timeline(<name>).end` / `.marker(<m>)` triggers. T-201: `scope` is the group's
    /// scope element (the root, or a symbol instance root); only chains and triggers that resolve
    /// `timeline` to that scope react.
    /// T-774: `selfApplied` = applySelfControl() already performed this occurrence's self-control
    /// actions; the trigger runs skip them (taking the outcome recorded then).
    void onTimelineEvent(StringId timeline, TimelineEventKind kind, StringId marker, NodeId scope,
                         bool selfApplied = false);
    /// T-774 (58 §58.6 item 2; trigger_self_control.cpp): a self-control action is a `pause` /
    /// `stop` / `seek` / `play(from=<marker>)` (not `hold`, not media) of a `timeline(<x>).marker`
    /// or `.end` trigger acting on that same timeline `<x>`, run when the trigger fires (on a
    /// statement head's chain before any `play` or `remove`; T-783: also behind an `if` reached
    /// through self-control actions only, decided at the marker's time). The timeline step performs
    /// them at the marker's (end's) time, before the frame is evaluated.
    bool hasSelfControl() const { return !m_selfActions.empty(); }
    /// True when some trigger reacting to this event of the group (`scope`) has a self-control action.
    /// T-783: one behind an `if` (reached through self-control actions only) counts when the
    /// condition, evaluated now, leads to it.
    bool selfControls(StringId timeline, TimelineEventKind kind, StringId marker, NodeId scope);
    /// Performs those actions now, in trigger order, and records each one's outcome for the run of
    /// that trigger (onTimelineEvent() with `selfApplied`).
    void applySelfControl(StringId timeline, TimelineEventKind kind, StringId marker, NodeId scope);
    /// Forgets the recorded outcomes no trigger run took (Document::flushTimelineEvents()); T-783:
    /// those of `<repeat>` instance copies go at RepeatController::flushEvents() (dropInstance...).
    void dropSelfApplied() { dropSelfAppliedOf(false); }
    void dropInstanceSelfApplied() { dropSelfAppliedOf(true); }
    /// T-177: a timeline played for one `<repeat>` instance (root `scope`) ended: resumes the
    /// chains waiting on it and fires the `timeline(<name>).end` triggers inside that instance.
    /// T-783: `selfApplied` as onTimelineEvent().
    void onInstanceTimelineEnd(NodeId scope, StringId timeline, bool selfApplied = false);
    /// T-783: selfControls() / applySelfControl() for the end of the copy of `timeline` played for
    /// the `<repeat>` instance `scope` (the `.end` triggers inside that instance).
    bool instanceSelfControls(NodeId scope, StringId timeline);
    void applyInstanceSelfControl(NodeId scope, StringId timeline);
    /// T-191: the `<video>`/`<sequence>` with id `id` played to its end: resumes the chains
    /// waiting on `play(<id>)` and fires `media(<id>).end`.
    void onMediaEnd(StringId id);
    /// T-177: registers a copied `<repeat>` instance element's timeline triggers (its
    /// NodeCommon::triggerTable is already set).
    void bindNode(NodeId node);
    /// T-738 (56 §56.4): the custom-state bits to clear on `node` when the state bit(s) `mask` are
    /// set: the group holding them in the first symbol scope (STGS) found from `node` outwards,
    /// else every STAT group holding them (0 when none does).
    uint32_t exclusiveMask(NodeId node, uint32_t mask);

    /// Input dispatch brackets: triggers fired in between run at endDefer().
    void beginDefer() { ++m_defer; }
    void endDefer();

    /// Runs queued trigger chains until the queue is empty (bounded).
    void run();
    /// T-389 (27 §27.4.1): runs the continuous-event triggers (`pointermove`, `scroll`,
    /// `drag.over`) held back since the last call -- each (element, trigger) once, with the last
    /// event's values. Called at the start of Document::update().
    void flushContinuous();
    /// T-749 (23 §23.ag item 6): chains queued (run()) or continuous-event triggers held back
    /// (flushContinuous()) that the next update() runs. Chains waiting on a timeline are not counted:
    /// the timeline they wait on keeps the Document updating while it plays.
    bool hasQueued() const { return !m_queue.empty() || !m_continuous.empty(); }

    /// True when `node` has a chain started by one of its `event` triggers still waiting on a
    /// timeline.
    bool waiting(NodeId node, TriggerEvent event) const;
    /// True when `node` has at least one `event` trigger.
    bool hasTrigger(NodeId node, TriggerEvent event) const;
    /// Drops the waiting chains of `node`'s `event` triggers (a show cancels a pending hide).
    void cancelWaiting(NodeId node, TriggerEvent event);
    /// Drops queued jobs and waiters of a node that is being destroyed.
    void forgetNode(NodeId node);
    /// T-374 (17 §17.33.3): what is running right now, for Document::lastHideSource():
    /// `port <name>` for a `port(<name>)` trigger, `trigger on:<event>` for any other trigger, and
    /// `host` outside a trigger chain.
    std::string currentSource() const;

    /// T-601 (53 §53.102 item 7): Document::addSurfaceListener() / removeSurfaceListener().
    uint32_t addSurfaceListener(std::function<void(bool shown)> listener);
    void removeSurfaceListener(uint32_t id);
    /// Calls every surface listener (a copy of the list: one may remove itself or another).
    void notifySurface(bool shown);

private:
    struct Job {
        NodeId node = kInvalidNode;
        uint32_t generation = 0;
        uint32_t trigger = 0;  // Index into TRIG triggers().
        uint16_t start = 0;    // 0 = every statement head, else the 1-based action to resume at.
        float value = 0.0f;
        uint32_t port = 0xFFFFFFFFu;  // T-175: the PORT index of a Port event.
        uint8_t player = 0;           // T-194: the player whose input fired the trigger.
        Vec2 pointer{};               // T-386: the firing pointer event's `pos` (root coordinates).
        uint8_t button = 0;           // T-390: the firing pointer event's PointerButton + 1, 0 = none.
        bool selfApplied = false;     // T-774: its self-control actions already ran.
        // T-871 (60 §60.3.3): the pointer movement (summed over the events a frame coalesces), the
        // press point, and a wheel event's (summed) delta.
        Vec2 delta{};
        Vec2 press{};
        Vec2 wheel{};
    };
    struct Waiter {
        NodeId node = kInvalidNode;
        uint32_t generation = 0;
        uint32_t trigger = 0;
        uint16_t next = 0;  // 1-based action run once `timeline` ends.
        StringId timeline = kInvalidString;
        float value = 0.0f;
        uint32_t port = 0xFFFFFFFFu;
        NodeId scope = kInvalidNode;  // T-177: the `<repeat>` instance the timeline plays for.
        uint8_t player = 0;           // T-194.
        NodeId group = kInvalidNode;  // T-201: the timeline group's scope element (root or instance).
        Vec2 pointer{};               // T-386: Job::pointer, carried across the wait.
        uint8_t button = 0;           // T-390: Job::button, carried across the wait.
        Vec2 delta{};                 // T-871: Job::delta / press / wheel, carried across the wait.
        Vec2 press{};
        Vec2 wheel{};
    };
    /// T-871: the Job that resumes `w` at its `next` action.
    static Job resumeJob(const Waiter& w);
    enum class Outcome : uint8_t { Done, Wait, Stop };

    void runJob(const Job& job);
    void runChain(NodeId node, uint32_t trigger, uint16_t index, float value, uint32_t port, uint8_t player,
                  bool selfApplied = false);
    /// T-774: whether `node`'s trigger `index` reacts to this timeline event (onTimelineEvent()).
    bool matchesTimelineEvent(NodeId node, uint32_t index, StringId timeline, TimelineEventKind kind,
                              StringId marker, NodeId scope) const;
    /// T-774: fills m_selfActions from the TRIG chunk (load()).
    void indexSelfActions();
    /// T-783: follows trigger `index` of `node` as runChain() would, deciding each `if` it may
    /// (see indexSelfActions()) now; `apply`: performs the self-control actions reached and records
    /// their outcomes and the `if` results for the trigger run. True when one was reached.
    bool selfChain(NodeId node, uint32_t index, bool apply, bool instance = false);
    /// T-783: whether trigger `index` of `node` is a `timeline(<timeline>).end` trigger inside the
    /// instance `scope` with a self-control action (onInstanceTimelineEnd()'s match).
    bool instanceEndTrigger(NodeId node, uint32_t index, NodeId scope, StringId timeline) const;
    void dropSelfAppliedOf(bool instance);
    /// T-774: takes (removes) the outcome applySelfControl() recorded for this action, if any.
    bool takeSelfApplied(NodeId node, uint32_t trigger, uint16_t action, Outcome& outcome, StringId& waitFor,
                         NodeId& waitScope, NodeId& waitGroup);
    Outcome perform(NodeId node, const IrTriggerAction& action, StringId& waitTimeline, NodeId& waitScope,
                    NodeId& waitGroup, float value, uint32_t port, uint8_t player);

    /// T-871 (trigger_pointer.cpp): the value of a set() / add() source read when the action runs
    /// from the firing event (`pointer.*`, `event.dx` / `event.dy`); false for any other source.
    bool runtimeSourceValue(uint8_t source, float arg2, float& out) const;
    /// T-871: `node`'s border-box centre in root coordinates.
    Vec2 centreOf(NodeId node) const;
    /// T-871: the turn / grab state of the element whose (path) id is `id`, or null.
    struct Tracker {
        StringId id = kInvalidString;
        NodeId node = kInvalidNode;
        uint32_t generation = 0;
        float lastAngle = 0;  // Degrees, -180..180.
        float turn = 0;
        Vec2 grab{};
    };
    const Tracker* trackerOf(StringId id) const;
    /// T-873: one element's input-channel trigger (each (event, channel, player) once per element).
    struct ChannelTrigger {
        NodeId node = kInvalidNode;
        uint32_t generation = 0;
        TriggerEvent event = TriggerEvent::None;
        StringId name = kInvalidString;  // IrTrigger::arg0.
        uint8_t player = 0;              // IrTrigger::arg1.
        ChannelId channel = kInvalidChannel;
    };
    std::vector<ChannelTrigger> m_channelTriggers;
    /// T-873: lists `node`'s input-channel triggers (load(), bindNode()).
    void indexChannelTriggers(NodeId node);
    Tracker startTracker(StringId id, NodeId node, Vec2 pos, Vec2 press) const;

    /// `node` is the trigger's element (T-386: `if active` reads its state).
    bool condition(NodeId node, const IrTriggerAction& action, float value, uint32_t port) const;
    /// T-390 (08.13): one kIfTerms term list (`[not] <term> and ...`) of an If action.
    bool termsHold(NodeId node, std::span<const IrConverterStep> steps, float value, uint32_t port) const;
    /// T-201: the scope element of the timeline `name` as seen from `node`: the nearest
    /// ancestor-or-self that is the scope of a group of that name (a symbol instance root), else
    /// the document root.
    NodeId timelineScope(NodeId node, StringId name) const;
    /// The scopePath of Document::playTimeline() etc. for a scope element ("" for the root).
    std::string_view scopePath(NodeId scope) const;

    /// T-738: the STGS scope of `node` when it is a listed instance root (or a `<repeat>` copy of
    /// one), else kNoScope.
    uint32_t scopeOf(NodeId node);
    static constexpr uint32_t kNoScope = 0xFFFFFFFFu;
    /// T-738: the STGS roots that are document elements (not template elements), by node.
    struct ScopeRoot {
        NodeId node = kInvalidNode;
        uint32_t generation = 0;
        uint32_t scope = 0;
    };

    Document& m_doc;
    std::vector<ScopeRoot> m_scopeRoots;  // T-738: ascending node.
    bool m_scopeRootInTemplate = false;   // T-738: some STGS root is a template element.
    std::vector<Job> m_queue;
    /// T-389: the held-back continuous-event jobs, one per (element, generation, trigger).
    std::vector<Job> m_continuous;
    std::vector<Waiter> m_waiters;
    // (element, trigger index) of every TimelineEnd / TimelineMarker trigger.
    std::vector<std::pair<NodeId, uint32_t>> m_timelineTriggers;
    /// T-774: (trigger index, 1-based action index) of every self-control action, ascending.
    std::vector<std::pair<uint32_t, uint16_t>> m_selfActions;
    /// T-783: the triggers (ascending) with a self-control action behind an `if`.
    std::vector<uint32_t> m_guardedSelf;
    /// T-774: what applySelfControl() performed and the trigger runs have not taken yet.
    struct SelfApplied {
        NodeId node = kInvalidNode;
        uint32_t generation = 0;
        uint32_t trigger = 0;
        uint16_t action = 0;
        Outcome outcome = Outcome::Done;
        StringId waitFor = kInvalidString;
        NodeId waitScope = kInvalidNode;
        NodeId waitGroup = kInvalidNode;
        bool instance = false;  // T-783: recorded for a `<repeat>` instance copy's end.
    };
    std::vector<SelfApplied> m_selfApplied;
    int m_defer = 0;
    bool m_running = false;
    uint32_t m_currentTrigger = 0xFFFFFFFFu;  // T-374: the TRIG trigger runChain() is running.
    // T-386 (08.11): the pointer position (root coordinates) of the DOM event fire() is called
    // for (queued into each Job), and of the Job runChain() is running (`pointer.fraction()` /
    // `pointer.local()` values of set()).
    Vec2 m_firePointer{};
    Vec2 m_runPointer{};
    // T-871 (60 §60.3.3): the same for the pointer movement, the press point and a wheel's delta.
    Vec2 m_fireDelta{};
    Vec2 m_firePress{};
    Vec2 m_fireWheel{};
    Vec2 m_runDelta{};
    Vec2 m_runPress{};
    Vec2 m_runWheel{};
    // T-871: the element ids `pointer.turn(#id)` / `pointer.grab(#id)` name (load()), and their
    // state for the press in progress (m_tracking: a press is held).
    std::vector<StringId> m_trackIds;
    std::vector<Tracker> m_trackers;
    bool m_tracking = false;
    // T-390 (08.13): the same for the pointer button (PointerButton + 1, 0 = none): trigger
    // button filters compare m_fireButton, `if button()` reads m_runButton.
    // T-393 (08.14): kFireExternal is OR-ed into both (and Job / Waiter::button) for a drag event
    // of an OS drag (`if external`); kFireButtonMask keeps the button.
    static constexpr uint8_t kFireExternal = 0x80u;
    static constexpr uint8_t kFireButtonMask = 0x7Fu;
    uint8_t m_fireButton = 0;
    uint8_t m_runButton = 0;
    // T-601: the surface listeners (id, listener), in registration order; ids are never reused.
    std::vector<std::pair<uint32_t, std::function<void(bool)>>> m_surfaceListeners;
    uint32_t m_nextSurfaceListener = 1;
};

}  // namespace xt
