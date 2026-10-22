#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "event_dispatcher.h"
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
    void onDragState(NodeId node, DragStateKind kind, bool on) override;
    void onDrop(const Event& event) override;
    uint32_t itemIndexOf(NodeId node) override;
    void onAxisValue(NodeId node, bool nav, float value, uint8_t player) override;
    // T-394 (08.15).
    bool consumesOutsidePress(NodeId node) override;
    bool hasPointerDrag(NodeId node) override;
    uint32_t dropIndexOf(NodeId target, Vec2 pos, uint32_t targetItem, Vec2 fraction) override;

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
    void onTimelineEvent(StringId timeline, TimelineEventKind kind, StringId marker, NodeId scope);
    /// T-177: a timeline played for one `<repeat>` instance (root `scope`) ended: resumes the
    /// chains waiting on it and fires the `timeline(<name>).end` triggers inside that instance.
    void onInstanceTimelineEnd(NodeId scope, StringId timeline);
    /// T-191: the `<video>`/`<sequence>` with id `id` played to its end: resumes the chains
    /// waiting on `play(<id>)` and fires `media(<id>).end`.
    void onMediaEnd(StringId id);
    /// T-177: registers a copied `<repeat>` instance element's timeline triggers (its
    /// NodeCommon::triggerTable is already set).
    void bindNode(NodeId node);

    /// Input dispatch brackets: triggers fired in between run at endDefer().
    void beginDefer() { ++m_defer; }
    void endDefer();

    /// Runs queued trigger chains until the queue is empty (bounded).
    void run();
    /// T-389 (27 §27.4.1): runs the continuous-event triggers (`pointermove`, `scroll`,
    /// `drag.over`) held back since the last call -- each (element, trigger) once, with the last
    /// event's values. Called at the start of Document::update().
    void flushContinuous();

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
    };
    enum class Outcome : uint8_t { Done, Wait, Stop };

    void runJob(const Job& job);
    void runChain(NodeId node, uint32_t trigger, uint16_t index, float value, uint32_t port, uint8_t player);
    Outcome perform(NodeId node, const IrTriggerAction& action, StringId& waitTimeline, NodeId& waitScope,
                    NodeId& waitGroup, float value, uint32_t port, uint8_t player);

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

    Document& m_doc;
    std::vector<Job> m_queue;
    /// T-389: the held-back continuous-event jobs, one per (element, generation, trigger).
    std::vector<Job> m_continuous;
    std::vector<Waiter> m_waiters;
    // (element, trigger index) of every TimelineEnd / TimelineMarker trigger.
    std::vector<std::pair<NodeId, uint32_t>> m_timelineTriggers;
    int m_defer = 0;
    bool m_running = false;
    uint32_t m_currentTrigger = 0xFFFFFFFFu;  // T-374: the TRIG trigger runChain() is running.
    // T-386 (08.11): the pointer position (root coordinates) of the DOM event fire() is called
    // for (queued into each Job), and of the Job runChain() is running (`pointer.fraction()` /
    // `pointer.local()` values of set()).
    Vec2 m_firePointer{};
    Vec2 m_runPointer{};
    // T-390 (08.13): the same for the pointer button (PointerButton + 1, 0 = none): trigger
    // button filters compare m_fireButton, `if button()` reads m_runButton.
    // T-393 (08.14): kFireExternal is OR-ed into both (and Job / Waiter::button) for a drag event
    // of an OS drag (`if external`); kFireButtonMask keeps the button.
    static constexpr uint8_t kFireExternal = 0x80u;
    static constexpr uint8_t kFireButtonMask = 0x7Fu;
    uint8_t m_fireButton = 0;
    uint8_t m_runButton = 0;
};

}  // namespace xt
