#include "trigger_runtime.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

#include "binding_runtime.h"
#include "dispatch_guard.h"
#include "repeat_controller.h"
#include "style_decode.h"
#include "timeline_group.h"
#include "xt/document.h"
#include "xt/ir.h"

/// docs/design/27-interaction.md §27.1-27.2, 08 §8.9, 13 §13.8.2-13.8.3, docs/tasks/T-174.md.
/// TriggerRuntime plus the Document members behind custom states and the surface lifecycle.
///
/// Not covered by the design/task sheet (conservative choices made here):
///   - `hover`/`leave` follow PointerEnter/PointerLeave (fired on every element of the entered /
///     left hover path, not bubbling); `press`/`release`/`click` fire on every element the
///     bubbling PointerDown/PointerUp/Click visits (stopPropagation stops them too); `key(<k>)`
///     fires only on the KeyDown target (the focused element, or the root).
///   - Triggers fired while an input event is dispatched run after it is fully handled, so a
///     trigger's actions see the default actions (e.g. the press's focus change) already applied.
///   - `play()` acts on the timeline of that name in the nearest symbol instance scope around the
///     trigger's element (T-201; T-190 made a symbol's groups per instance), else the root scope;
///     `stop`/`seek`, the `then` waiters and `timeline(<n>).end/.marker()` resolve the same way.
///     Without `once` it holds the last frame (27.2.3's default once + hold); re-firing restarts
///     it, except with an explicit `hold`, which leaves a playing timeline alone. A `then` after
///     `play` waits for that timeline's End; after a looping play it never runs; after a failed play
///     it runs at once. `stop(<tl>)` drops the chains waiting on <tl>.
///   - `show()`/`hide()` act on the trigger's own element; on the surface element (or the root)
///     they are the surface requestShow()/requestHide(). T-374: on a direct child of `<body>`,
///     `show()` also shows a hidden surface, and a `hide()` that leaves no other shown direct
///     child hides the surface instead of the element. An element's `hide` works like the
///     surface's: its `hide` triggers run first and a `then hide` chain delays it. Hiding sets a
///     runtime `visibility: hidden` declaration, showing `visibility: visible`.
///   - The first update() fires the surface's `show` once (the surface appears when it loads).
///   - A run executes at most 10000 queued chains; the rest are dropped with a Document warning
///     (a trigger cycle such as `state(A).enter` -> `state(A, toggle)`).
///   - T-194: `back`, `nav.blocked`, `drag.*`, `drop` and `gesture()` fire on every element their
///     bubbling event visits, like `press`; `drag.start` on the source's path, the others on the
///     drop target's. `player(<n>).<event>` fires alongside the plain event for player n's input.
///     `layer(overlay|normal)` sets a runtime `layer` declaration. The `Dragging` / `DropHover`
///     custom states are only toggled when the document declares them (a `:state()` selector or
///     trigger names them); otherwise the drag has no state to show.
///   - T-177: inside a `<repeat>` instance, `play`/`stop`/`seek` of a timeline that animates the
///     template play a copy of it for that instance only (RepeatController); its `then` waits for
///     that copy's end. `remove` destroys the exiting instance and ends the chain.

namespace xt {

namespace {

constexpr size_t kMaxJobsPerRun = 10000;

void markStyleDirtySubtree(NodeArena& arena, NodeId node) {
    arena.markDirty(node, static_cast<uint8_t>(kDirtyStyle));
    arena.traversePreorder(node, [&arena](NodeId n) {
        arena.markDirty(n, static_cast<uint8_t>(kDirtyStyle));
        return true;
    });
}

bool eraseNode(std::vector<NodeId>& list, NodeId node) {
    auto it = std::find(list.begin(), list.end(), node);
    if (it == list.end()) {
        return false;
    }
    list.erase(it);
    return true;
}

}  // namespace

// -- TriggerRuntime -----------------------------------------------------------------------------

void TriggerRuntime::load() {
    const TriggerChunk& tc = m_doc.m_ir.triggers();
    for (size_t i = 0; i < tc.tables().size(); ++i) {
        const IrTriggerTable& table = tc.tables()[i];
        if (table.element >= m_doc.m_docElementNodes.size()) {
            continue;
        }
        NodeId node = m_doc.m_docElementNodes[table.element];
        if (node == kInvalidNode || !m_doc.m_arena.alive(node)) {
            continue;
        }
        m_doc.m_arena.common(node).triggerTable = static_cast<uint32_t>(i + 1);
        // T-194 (08.9): an element with `on:focus` / `on:press` can take the focus (gamepad and
        // arrow-key navigation reach it); set before the template check so copies inherit it.
        for (uint32_t k = 0; k < table.triggerCount; ++k) {
            const TriggerEvent ev = static_cast<TriggerEvent>(tc.triggers()[table.triggerOffset + k].event);
            if (ev == TriggerEvent::Focus || ev == TriggerEvent::Press) {
                m_doc.m_arena.element(node).focusable = 1;
            }
        }
        if (m_doc.m_repeats->isTemplateElement(table.element)) {
            continue;  // T-177: a template element never fires; its copies call bindNode().
        }
        for (uint32_t k = 0; k < table.triggerCount; ++k) {
            uint32_t index = table.triggerOffset + k;
            TriggerEvent ev = static_cast<TriggerEvent>(tc.triggers()[index].event);
            if (ev == TriggerEvent::TimelineEnd || ev == TriggerEvent::TimelineMarker ||
                ev == TriggerEvent::MediaEnd) {  // T-191: media ends are routed the same way.
                m_timelineTriggers.emplace_back(node, index);
            } else if (ev == TriggerEvent::OutsidePress) {
                // T-387 (08.12): the dispatcher only checks registered elements on a press.
                m_doc.m_events->watchOutsidePress(m_doc.m_arena, node);
            }
        }
    }
}

void TriggerRuntime::onDomEvent(const Event& event, NodeId node) {
    TriggerEvent ev = TriggerEvent::None;
    uint32_t arg0 = kInvalidString;
    uint32_t arg1 = kInvalidString;
    switch (event.type) {
        // T-194 (27.2.2, 27.7).
        case EventType::Back:
            ev = TriggerEvent::Back;
            break;
        case EventType::NavBlocked:
            ev = TriggerEvent::NavBlocked;
            arg0 = static_cast<uint32_t>(event.direction);
            break;
        case EventType::DragStart:
            ev = TriggerEvent::DragStart;
            break;
        case EventType::DragOver:
            ev = TriggerEvent::DragOver;
            break;
        case EventType::DragLeave:
            ev = TriggerEvent::DragLeave;
            break;
        case EventType::Drop:
            ev = TriggerEvent::Drop;
            break;
        case EventType::Gesture:
            ev = TriggerEvent::Gesture;
            arg0 = static_cast<uint32_t>(event.gesture);
            arg1 = static_cast<uint32_t>(event.direction);
            break;
        case EventType::PointerEnter:
            ev = TriggerEvent::Hover;
            break;
        case EventType::PointerLeave:
            ev = TriggerEvent::Leave;
            break;
        case EventType::PointerDown:
            ev = TriggerEvent::Press;
            break;
        case EventType::PointerUp:
            ev = TriggerEvent::Release;
            break;
        case EventType::Click:
            ev = TriggerEvent::Click;
            break;
        case EventType::DoubleClick:
            ev = TriggerEvent::DoubleClick;  // T-379 (08.10).
            break;
        case EventType::PointerMove:  // T-386 (08.11).
            ev = TriggerEvent::PointerMove;
            break;
        case EventType::PointerCancel:  // T-386 (08.11).
            ev = TriggerEvent::PointerCancel;
            break;
        case EventType::DragCancel:  // T-388 (08.12).
            ev = TriggerEvent::DragCancel;
            break;
        case EventType::Scroll:  // T-387 (08.12).
            ev = TriggerEvent::Scroll;
            break;
        case EventType::OutsidePress:  // T-387 (08.12): delivered only to registered elements.
            ev = TriggerEvent::OutsidePress;
            break;
        case EventType::PointerDrag:  // T-394 (08.15): sent to the pointerdrag element only.
            ev = TriggerEvent::PointerDrag;
            break;
        case EventType::Focus:
            ev = TriggerEvent::Focus;
            break;
        case EventType::Blur:
            ev = TriggerEvent::Blur;
            break;
        case EventType::FocusIn:  // T-397 (08.17): bubbles, so it fires on every ancestor too.
            ev = TriggerEvent::FocusIn;
            break;
        case EventType::FocusOut:  // T-397 (08.17).
            ev = TriggerEvent::FocusOut;
            break;
        case EventType::Change:
            if (node != event.target) {
                return;  // T-392 (27.3.5): `change` / `input` on the element whose value changed.
            }
            ev = TriggerEvent::Change;
            break;
        case EventType::KeyDown:
            if (node != event.target) {
                return;  // 27.2.2: key(<k>) only on the focused element.
            }
            ev = TriggerEvent::Key;
            arg0 = static_cast<uint32_t>(event.key);
            break;
        default:
            return;
    }
    // T-386: the Jobs queued by this fire() remember where the pointer was (pointer.fraction()).
    struct PointerScope {
        Vec2& slot;
        Vec2 saved;
        ~PointerScope() { slot = saved; }
    } pointerScope{m_firePointer, m_firePointer};
    m_firePointer = event.pos;
    // T-390 (08.13): the button trigger filters and `if button()` compare; a move reports the
    // button held (none without a press in progress).
    struct ButtonScope {
        uint8_t& slot;
        uint8_t saved;
        ~ButtonScope() { slot = saved; }
    } buttonScope{m_fireButton, m_fireButton};
    m_fireButton = static_cast<uint8_t>(static_cast<uint8_t>(event.button) + 1u);
    if (ev == TriggerEvent::PointerMove && m_doc.m_events->activeNode() == kInvalidNode) {
        m_fireButton = 0;
    }
    // T-393 (08.14): `if external` -- a drag event with no source comes from an OS drag.
    if ((ev == TriggerEvent::DragOver || ev == TriggerEvent::DragLeave || ev == TriggerEvent::Drop) &&
        event.dragSource == kInvalidNode) {
        m_fireButton = static_cast<uint8_t>(m_fireButton | kFireExternal);
    }
    fire(node, ev, arg0, arg1, 0.0f, 0xFFFFFFFFu, event.player);
    if (ev == TriggerEvent::Change) {
        // T-392 (27.3.5): core's Change fires once per edit, so it is also the `input` trigger.
        fire(node, TriggerEvent::Input, arg0, arg1, 0.0f, 0xFFFFFFFFu, event.player);
    }
    switch (ev) {
        case TriggerEvent::Press:
        case TriggerEvent::Release:
        case TriggerEvent::Click:
        case TriggerEvent::Focus:
        case TriggerEvent::Blur:
        case TriggerEvent::Hover:
        case TriggerEvent::Leave:
        case TriggerEvent::Back:
        case TriggerEvent::NavBlocked:
            // T-194: `player(<n>).<event>` for the player whose input this is.
            fire(node, TriggerEvent::Player, event.player, static_cast<uint32_t>(ev), 0.0f, 0xFFFFFFFFu,
                 event.player);
            break;
        default:
            break;
    }
}

void TriggerRuntime::onFocusMoved(NodeId node, uint8_t player) {
    // T-194 (27.7): the new focus's `aria-label` is read aloud by the host.
    Document& doc = m_doc;
    // T-260a: an unloading Document (m_unloading) sends no host event, here and below.
    if (node == kInvalidNode || !doc.m_arena.alive(node) || doc.m_arena.kind(node) != NodeKind::Element ||
        !doc.m_hostEventListener || doc.m_unloading) {
        return;
    }
    std::string_view label;
    for (const auto& attr : doc.m_arena.element(node).attributes) {
        if (doc.string(attr.first) == "aria-label") {
            label = doc.string(attr.second);
            break;
        }
    }
    if (label.empty()) {
        return;
    }
    HostEvent event;
    event.kind = HostEventKind::Announce;
    event.name = "announce";
    event.text = label;
    event.element = node;
    const StringId id = doc.m_arena.element(node).id;
    event.elementId = id != kInvalidString ? doc.string(id) : std::string_view{};
    event.instanceId = doc.m_instanceId;
    event.itemIndex = doc.m_repeats->itemIndexOf(node, nullptr);
    event.player = player;
    HostEventListener listener = doc.m_hostEventListener;
    listener(event);
}

void TriggerRuntime::onDragState(NodeId node, DragStateKind kind, bool on) {
    std::span<const uint32_t> names = m_doc.m_ir.customStates().names();
    const std::string_view name = kind == DragStateKind::Dragging ? "Dragging" : "DropHover";
    for (uint32_t bit = 0; bit < names.size(); ++bit) {
        if (m_doc.m_ir.string(names[bit]) == name) {
            m_doc.applyCustomState(node, bit, static_cast<uint8_t>(on ? TriggerMode::On : TriggerMode::Off));
            return;
        }
    }
}

void TriggerRuntime::onDrop(const Event& ev) {
    Document& doc = m_doc;
    if (!doc.m_hostEventListener || doc.m_unloading) {
        return;
    }
    HostEvent event;
    event.kind = HostEventKind::Drop;
    event.name = "drop";
    event.element = ev.dragSource;
    if (ev.dragSource != kInvalidNode && doc.m_arena.alive(ev.dragSource)) {
        const StringId id = doc.m_arena.element(ev.dragSource).id;
        event.elementId = id != kInvalidString ? doc.string(id) : std::string_view{};
    }
    event.target = ev.dropTarget;
    if (ev.dropTarget != kInvalidNode && doc.m_arena.alive(ev.dropTarget)) {
        const StringId id = doc.m_arena.element(ev.dropTarget).id;
        event.targetId = id != kInvalidString ? doc.string(id) : std::string_view{};
    }
    event.itemIndex = ev.sourceItem;
    event.targetItemIndex = ev.targetItem;
    event.dropFraction = ev.dropFraction;  // T-388.
    event.paths = ev.externalPaths;        // T-393 (08.14): an OS drag's files.
    event.instanceId = doc.m_instanceId;
    event.player = ev.player;
    HostEventListener listener = doc.m_hostEventListener;
    listener(event);
}

uint32_t TriggerRuntime::itemIndexOf(NodeId node) {
    if (node == kInvalidNode || !m_doc.m_arena.alive(node)) {
        return 0xFFFFFFFFu;
    }
    return m_doc.m_repeats->itemIndexOf(node, nullptr);
}

void TriggerRuntime::onAxisValue(NodeId node, bool nav, float value, uint8_t player) {
    // T-201 (27.7): `drag.value` / `nav.value` carry the value `set(..., event.value)` reads.
    fire(node, nav ? TriggerEvent::NavValue : TriggerEvent::DragValue, kInvalidString, kInvalidString, value,
         0xFFFFFFFFu, player);
}

void TriggerRuntime::onCheckedChanged(NodeId node, bool checked) {
    fire(node, checked ? TriggerEvent::Check : TriggerEvent::Uncheck);
}

void TriggerRuntime::fire(NodeId node, TriggerEvent event, uint32_t arg0, uint32_t arg1, float value,
                          uint32_t port, uint8_t player) {
    // T-394 (08.15): `pointerdrag` is coalesced like `pointermove`.
    const bool continuous = event == TriggerEvent::PointerMove || event == TriggerEvent::Scroll ||
                            event == TriggerEvent::DragOver || event == TriggerEvent::PointerDrag;
    const NodeArena& arena = m_doc.m_arena;
    if (node == kInvalidNode || !arena.alive(node) || arena.kind(node) != NodeKind::Element) {
        return;
    }
    uint32_t table = arena.common(node).triggerTable;
    const TriggerChunk& tc = m_doc.m_ir.triggers();
    if (table == 0 || table > tc.tables().size()) {
        return;
    }
    const IrTriggerTable& t = tc.tables()[table - 1];
    bool queued = false;
    for (uint32_t k = 0; k < t.triggerCount; ++k) {
        uint32_t index = t.triggerOffset + k;
        const IrTrigger& trig = tc.triggers()[index];
        if (static_cast<TriggerEvent>(trig.event) != event) {
            continue;
        }
        if (trig.button != 0 && trig.button != (m_fireButton & kFireButtonMask)) {
            continue;  // T-390 (08.13): `press(<button>)` etc.
        }
        switch (event) {
            case TriggerEvent::Key:
            case TriggerEvent::StateEnter:
            case TriggerEvent::StateExit:
            case TriggerEvent::TimelineEnd:
                if (trig.arg0 != arg0) {
                    continue;
                }
                break;
            case TriggerEvent::TimelineMarker:
                if (trig.arg0 != arg0 || trig.arg1 != arg1) {
                    continue;
                }
                break;
            case TriggerEvent::Port:
                if (trig.arg1 != arg1) {  // T-175: PORT index + 1.
                    continue;
                }
                break;
            case TriggerEvent::NavBlocked:  // T-194: kInvalidString = any direction.
                if (trig.arg0 != kInvalidString && trig.arg0 != arg0) {
                    continue;
                }
                break;
            case TriggerEvent::Gesture:  // T-194: the kind, and the swipe direction when given.
                if (trig.arg0 != arg0 || (trig.arg1 != kInvalidString && trig.arg1 != arg1)) {
                    continue;
                }
                break;
            case TriggerEvent::Player:  // T-194: player index and inner event.
                if (trig.arg0 != arg0 || trig.arg1 != arg1) {
                    continue;
                }
                break;
            default:
                break;
        }
        const Job job{node,   arena.generation(node), index,       0, value, port,
                      player, m_firePointer,          m_fireButton};
        if (continuous) {
            // T-389 (27 §27.4.1): a continuous event runs once per frame per trigger, with the
            // frame's last event (flushContinuous()).
            bool replaced = false;
            for (Job& held : m_continuous) {
                if (held.node == job.node && held.generation == job.generation &&
                    held.trigger == job.trigger) {
                    held = job;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                m_continuous.push_back(job);
            }
            continue;
        }
        m_queue.push_back(job);
        queued = true;
    }
    if (queued) {
        run();
    }
}

void TriggerRuntime::flushContinuous() {
    if (m_continuous.empty()) {
        return;
    }
    std::vector<Job> held;
    held.swap(m_continuous);
    m_queue.insert(m_queue.end(), held.begin(), held.end());
    run();
}

void TriggerRuntime::onTimelineEvent(StringId timeline, TimelineEventKind kind, StringId marker,
                                     NodeId scope) {
    if (kind == TimelineEventKind::End) {
        for (size_t i = 0; i < m_waiters.size();) {
            const Waiter& w = m_waiters[i];
            if (w.timeline != timeline || w.scope != kInvalidNode || w.group != scope) {
                ++i;
                continue;
            }
            m_queue.push_back(
                Job{w.node, w.generation, w.trigger, w.next, w.value, w.port, w.player, w.pointer, w.button});
            m_waiters.erase(m_waiters.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    for (const auto& [node, index] : m_timelineTriggers) {
        const IrTrigger& trig = m_doc.m_ir.triggers().triggers()[index];
        const bool match =
            trig.arg0 == timeline &&
            ((kind == TimelineEventKind::End &&
              static_cast<TriggerEvent>(trig.event) == TriggerEvent::TimelineEnd) ||
             (kind == TimelineEventKind::Marker &&
              static_cast<TriggerEvent>(trig.event) == TriggerEvent::TimelineMarker && trig.arg1 == marker));
        // T-201: a trigger reacts to the group its own `play()` would reach.
        if (match && m_doc.m_arena.alive(node) && timelineScope(node, timeline) == scope) {
            m_queue.push_back(Job{node, m_doc.m_arena.generation(node), index, 0, 0.0f, 0xFFFFFFFFu});
        }
    }
    run();
}

NodeId TriggerRuntime::timelineScope(NodeId node, StringId name) const {
    const Document& doc = m_doc;
    const std::vector<TimelineGroupRuntime>& groups = doc.m_timelineGroups->groups;
    for (NodeId n = node; n != kInvalidNode && doc.m_arena.alive(n); n = doc.m_arena.parent(n)) {
        if (n == doc.m_root) {
            break;
        }
        for (const TimelineGroupRuntime& g : groups) {
            if (g.name == name && g.scopeElement != 0 && g.scopeElement < doc.m_docElementNodes.size() &&
                doc.m_docElementNodes[g.scopeElement] == n) {
                return n;
            }
        }
    }
    return doc.m_root;
}

std::string_view TriggerRuntime::scopePath(NodeId scope) const {
    if (scope == kInvalidNode || scope == m_doc.m_root || !m_doc.m_arena.alive(scope) ||
        m_doc.m_arena.kind(scope) != NodeKind::Element) {
        return {};
    }
    const StringId id = m_doc.m_arena.element(scope).id;
    return id != kInvalidString ? m_doc.string(id) : std::string_view{};
}

void TriggerRuntime::onInstanceTimelineEnd(NodeId scope, StringId timeline) {
    for (size_t i = 0; i < m_waiters.size();) {
        const Waiter& w = m_waiters[i];
        if (w.timeline != timeline || w.scope != scope) {
            ++i;
            continue;
        }
        m_queue.push_back(
            Job{w.node, w.generation, w.trigger, w.next, w.value, w.port, w.player, w.pointer, w.button});
        m_waiters.erase(m_waiters.begin() + static_cast<std::ptrdiff_t>(i));
    }
    const NodeArena& arena = m_doc.m_arena;
    for (const auto& [node, index] : m_timelineTriggers) {
        const IrTrigger& trig = m_doc.m_ir.triggers().triggers()[index];
        if (trig.arg0 != timeline || static_cast<TriggerEvent>(trig.event) != TriggerEvent::TimelineEnd ||
            !arena.alive(node) || !arena.alive(scope) ||
            (node != scope && !arena.isAncestorOf(scope, node))) {
            continue;
        }
        m_queue.push_back(Job{node, arena.generation(node), index, 0, 0.0f, 0xFFFFFFFFu});
    }
    run();
}

void TriggerRuntime::onMediaEnd(StringId id) {
    for (size_t i = 0; i < m_waiters.size();) {
        const Waiter& w = m_waiters[i];
        if (w.timeline != id || w.scope != kInvalidNode) {
            ++i;
            continue;
        }
        m_queue.push_back(
            Job{w.node, w.generation, w.trigger, w.next, w.value, w.port, w.player, w.pointer, w.button});
        m_waiters.erase(m_waiters.begin() + static_cast<std::ptrdiff_t>(i));
    }
    for (const auto& [node, index] : m_timelineTriggers) {
        const IrTrigger& trig = m_doc.m_ir.triggers().triggers()[index];
        if (trig.arg0 == id && static_cast<TriggerEvent>(trig.event) == TriggerEvent::MediaEnd &&
            m_doc.m_arena.alive(node)) {
            m_queue.push_back(Job{node, m_doc.m_arena.generation(node), index, 0, 0.0f, 0xFFFFFFFFu});
        }
    }
    run();
}

void TriggerRuntime::bindNode(NodeId node) {
    const NodeArena& arena = m_doc.m_arena;
    if (node == kInvalidNode || !arena.alive(node) || arena.kind(node) != NodeKind::Element) {
        return;
    }
    const uint32_t table = arena.common(node).triggerTable;
    const TriggerChunk& tc = m_doc.m_ir.triggers();
    if (table == 0 || table > tc.tables().size()) {
        return;
    }
    const IrTriggerTable& t = tc.tables()[table - 1];
    for (uint32_t k = 0; k < t.triggerCount; ++k) {
        const uint32_t index = t.triggerOffset + k;
        const TriggerEvent ev = static_cast<TriggerEvent>(tc.triggers()[index].event);
        if ((ev == TriggerEvent::TimelineEnd || ev == TriggerEvent::TimelineMarker ||
             ev == TriggerEvent::MediaEnd) &&
            std::find(m_timelineTriggers.begin(), m_timelineTriggers.end(), std::make_pair(node, index)) ==
                m_timelineTriggers.end()) {
            m_timelineTriggers.emplace_back(node, index);
        }
        if (ev == TriggerEvent::OutsidePress) {
            m_doc.m_events->watchOutsidePress(m_doc.m_arena, node);  // T-387 (08.12).
        }
    }
}

void TriggerRuntime::endDefer() {
    if (m_defer > 0) {
        --m_defer;
    }
    run();
}

void TriggerRuntime::run() {
    if (m_running || m_defer > 0) {
        return;
    }
    m_running = true;
    size_t executed = 0;
    for (size_t i = 0; i < m_queue.size(); ++i) {
        if (executed++ >= kMaxJobsPerRun) {
            m_doc.addWarning("trigger chain limit reached; " + std::to_string(m_queue.size() - i) +
                             " queued trigger(s) dropped");
            break;
        }
        Job job = m_queue[i];  // Copy: running it may append to m_queue.
        runJob(job);
    }
    m_queue.clear();
    m_running = false;
    m_doc.finishPendingHides();
}

void TriggerRuntime::runJob(const Job& job) {
    const NodeArena& arena = m_doc.m_arena;
    if (!arena.alive(job.node) || arena.generation(job.node) != job.generation) {
        return;
    }
    const TriggerChunk& tc = m_doc.m_ir.triggers();
    if (job.trigger >= tc.triggers().size()) {
        return;
    }
    const IrTrigger& trig = tc.triggers()[job.trigger];
    // T-386: the pointer position set()'s `pointer.*` values read while this job runs.
    struct PointerScope {
        Vec2& slot;
        Vec2 saved;
        ~PointerScope() { slot = saved; }
    } pointerScope{m_runPointer, m_runPointer};
    m_runPointer = job.pointer;
    struct ButtonScope {
        uint8_t& slot;
        uint8_t saved;
        ~ButtonScope() { slot = saved; }
    } buttonScope{m_runButton, m_runButton};
    m_runButton = job.button;  // T-390: `if button()`.
    if (job.start != 0) {
        runChain(job.node, job.trigger, job.start, job.value, job.port, job.player);
        return;
    }
    for (uint16_t head = 1; head <= trig.headCount; ++head) {
        runChain(job.node, job.trigger, head, job.value, job.port, job.player);
    }
}

void TriggerRuntime::runChain(NodeId node, uint32_t trigger, uint16_t index, float value, uint32_t port,
                              uint8_t player) {
    const TriggerChunk& tc = m_doc.m_ir.triggers();
    const IrTrigger& trig = tc.triggers()[trigger];
    // T-374: the source a surface hide records (Document::lastHideSource()).
    struct CurrentTrigger {
        uint32_t& slot;
        uint32_t saved;
        ~CurrentTrigger() { slot = saved; }
    } current{m_currentTrigger, m_currentTrigger};
    m_currentTrigger = trigger;
    uint32_t steps = 0;
    while (index != 0 && index <= trig.actionCount && steps++ <= trig.actionCount) {
        if (!m_doc.m_arena.alive(node)) {
            return;
        }
        const IrTriggerAction& action = tc.actions()[trig.actionOffset + index - 1];
        if (static_cast<TriggerAction>(action.kind) == TriggerAction::If) {
            index = condition(node, action, value, port) ? action.next : action.alt;
            continue;
        }
        StringId waitFor = kInvalidString;
        NodeId waitScope = kInvalidNode;
        NodeId waitGroup = kInvalidNode;
        Outcome outcome = perform(node, action, waitFor, waitScope, waitGroup, value, port, player);
        if (outcome == Outcome::Stop) {
            return;
        }
        if (outcome == Outcome::Wait) {
            if (action.next != 0) {
                uint32_t generation = m_doc.m_arena.generation(node);
                m_waiters.erase(std::remove_if(m_waiters.begin(), m_waiters.end(),
                                               [&](const Waiter& w) {
                                                   return w.node == node && w.trigger == trigger &&
                                                          w.next == action.next;
                                               }),
                                m_waiters.end());
                m_waiters.push_back(Waiter{node, generation, trigger, action.next, waitFor, value, port,
                                           waitScope, player, waitGroup, m_runPointer, m_runButton});
            }
            return;
        }
        index = action.next;
    }
}

std::string TriggerRuntime::currentSource() const {
    const TriggerChunk& tc = m_doc.m_ir.triggers();
    if (m_currentTrigger >= tc.triggers().size()) {
        return "host";
    }
    const IrTrigger& trig = tc.triggers()[m_currentTrigger];
    const auto name = [this](uint32_t id) {
        return id != kInvalidString ? std::string(m_doc.m_ir.string(id)) : std::string();
    };
    switch (static_cast<TriggerEvent>(trig.event)) {
        case TriggerEvent::Port:
            return "port " + name(trig.arg0);
        case TriggerEvent::TimelineEnd:
            return "trigger on:timeline(" + name(trig.arg0) + ").end";
        case TriggerEvent::TimelineMarker:
            return "trigger on:timeline(" + name(trig.arg0) + ").marker(" + name(trig.arg1) + ")";
        case TriggerEvent::MediaEnd:
            return "trigger on:media(" + name(trig.arg0) + ").end";
        case TriggerEvent::Hover:
            return "trigger on:hover";
        case TriggerEvent::Leave:
            return "trigger on:leave";
        case TriggerEvent::Press:
            return "trigger on:press";
        case TriggerEvent::Release:
            return "trigger on:release";
        case TriggerEvent::Click:
            return "trigger on:click";
        case TriggerEvent::Focus:
            return "trigger on:focus";
        case TriggerEvent::Blur:
            return "trigger on:blur";
        case TriggerEvent::Check:
            return "trigger on:check";
        case TriggerEvent::Uncheck:
            return "trigger on:uncheck";
        case TriggerEvent::Show:
            return "trigger on:show";
        case TriggerEvent::Hide:
            return "trigger on:hide";
        case TriggerEvent::StateEnter:
            return "trigger on:state.enter";
        case TriggerEvent::StateExit:
            return "trigger on:state.exit";
        case TriggerEvent::DragStart:
            return "trigger on:drag.start";
        case TriggerEvent::DragOver:
            return "trigger on:drag.over";
        case TriggerEvent::Drop:
            return "trigger on:drop";
        case TriggerEvent::Key:
            return "trigger on:key";
        case TriggerEvent::Gesture:
            return "trigger on:gesture";
        case TriggerEvent::Player:
            return "trigger on:player";
        case TriggerEvent::ItemEnter:
            return "trigger on:item-enter";
        case TriggerEvent::ItemExit:
            return "trigger on:item-exit";
        case TriggerEvent::DragLeave:
            return "trigger on:drag.leave";
        case TriggerEvent::Back:
            return "trigger on:back";
        case TriggerEvent::NavBlocked:
            return "trigger on:nav.blocked";
        case TriggerEvent::DragValue:
            return "trigger on:drag.value";
        case TriggerEvent::NavValue:
            return "trigger on:nav.value";
        case TriggerEvent::DoubleClick:
            return "trigger on:dblclick";
        case TriggerEvent::PointerCancel:
            return "trigger on:pointercancel";
        case TriggerEvent::PointerMove:
            return "trigger on:pointermove";
        case TriggerEvent::DragCancel:
            return "trigger on:drag.cancel";
        case TriggerEvent::Scroll:
            return "trigger on:scroll";
        case TriggerEvent::OutsidePress:
            return "trigger on:outsidepress";
        case TriggerEvent::Change:
            return "trigger on:change";
        case TriggerEvent::Input:
            return "trigger on:input";
        case TriggerEvent::PointerDrag:
            return "trigger on:pointerdrag";
        case TriggerEvent::FocusIn:
            return "trigger on:focusin";
        case TriggerEvent::FocusOut:
            return "trigger on:focusout";
        case TriggerEvent::None:
            break;
    }
    return "trigger";
}

bool TriggerRuntime::condition(NodeId node, const IrTriggerAction& action, float value, uint32_t port) const {
    if ((action.flags & kIfActive) != 0) {  // T-386 (08.11): `if active`.
        return (m_doc.m_arena.element(node).stateBits & kStateActive) != 0;
    }
    // T-175: the converter chain is evaluated by the binding runtime (ports, `N%`, words).
    std::span<const IrConverterStep> steps =
        m_doc.m_ir.triggers().conditions().subspan(action.arg0, action.arg1);
    if ((action.flags & kIfTerms) != 0) {
        return termsHold(node, steps, value, port);
    }
    return m_doc.m_bindings->evalCondition(steps, value, port);
}

bool TriggerRuntime::termsHold(NodeId node, std::span<const IrConverterStep> steps, float value,
                               uint32_t port) const {
    const NodeArena& arena = m_doc.m_arena;
    for (size_t i = 0; i < steps.size();) {
        const IrConverterStep& head = steps[i];
        const size_t count = head.argCount;
        if (count > steps.size() - i - 1 || !(head.args[0] >= 0.0f) ||
            head.args[0] > static_cast<float>(kIfTermLast)) {
            return false;  // Malformed (the validator checks only the ranges).
        }
        std::span<const IrConverterStep> chain = steps.subspan(i + 1, count);
        i += 1 + count;
        bool holds = false;
        switch (static_cast<IfTerm>(static_cast<uint8_t>(head.args[0]))) {
            case IfTerm::Compare:
                holds = m_doc.m_bindings->evalCondition(chain, value, port);
                break;
            case IfTerm::Active:
                holds = (arena.element(node).stateBits & kStateActive) != 0;
                break;
            case IfTerm::Button:
                holds = (m_runButton & kFireButtonMask) != 0 &&
                        static_cast<float>((m_runButton & kFireButtonMask) - 1) == head.args[2];
                break;
            case IfTerm::External:  // T-393 (08.14).
                holds = (m_runButton & kFireExternal) != 0;
                break;
            case IfTerm::Dragging:  // T-394 (08.15): this element is in a pointerdrag.
                holds = m_doc.m_events->pointerDragNode() == node;
                break;
            case IfTerm::State: {
                const float bit = head.args[2];
                if (!(bit >= 0.0f) || bit >= 32.0f) {
                    break;
                }
                const uint32_t mask = 1u << static_cast<uint32_t>(bit);
                holds = head.args[3] != 0.0f ? (arena.element(node).stateBits & mask) != 0
                                             : (arena.common(node).customStates & mask) != 0;
                break;
            }
            case IfTerm::Port: {
                uint32_t index = 0;
                std::memcpy(&index, &head.args[2], sizeof(index));
                // A template port inside a `<repeat>` instance means the instance's copy (T-380).
                holds = m_doc.m_bindings->evalCondition(chain, 0.0f,
                                                        m_doc.m_repeats->instancePortOf(node, index));
                break;
            }
        }
        if (holds == (head.args[1] != 0.0f)) {
            return false;
        }
    }
    return true;
}

TriggerRuntime::Outcome TriggerRuntime::perform(NodeId node, const IrTriggerAction& action, StringId& waitFor,
                                                NodeId& waitScope, NodeId& waitGroup, float value,
                                                uint32_t port, uint8_t player) {
    Document& doc = m_doc;
    const IrView& ir = doc.m_ir;
    switch (static_cast<TriggerAction>(action.kind)) {
        case TriggerAction::Play: {
            std::string_view name = ir.string(action.arg0);
            const bool loop = (action.flags & kPlayLoop) != 0;
            if ((action.flags & kActionMedia) != 0) {
                const NodeId target = doc.byId(name).node;
                if (doc.particlesOf(target) != kInvalidNode) {
                    doc.playParticles(target);  // T-202: emission on; an emitter never ends.
                    return Outcome::Done;
                }
                // T-191 (28.2): `play(<video|sequence id>[, loop])`; `then` waits for its end.
                if (doc.playMedia(target, loop) != Status::kOk) {
                    return Outcome::Done;
                }
                waitFor = action.arg0;
                return loop ? Outcome::Stop : Outcome::Wait;
            }
            {
                // T-177: a timeline over a `<repeat>` template plays for this instance only.
                PlayOptions options;
                options.reverse = (action.flags & kPlayReverse) != 0;
                options.loop = loop;
                options.once = (action.flags & kPlayOnce) != 0;
                options.hold = !options.once;
                options.fromMarker =
                    action.arg1 != kInvalidString ? ir.string(action.arg1) : std::string_view{};
                NodeId scope = kInvalidNode;
                const RepeatPlay played =
                    doc.m_repeats->play(node, action.arg0, options, (action.flags & kPlayHold) != 0, scope);
                if (played == RepeatPlay::Failed) {
                    return Outcome::Done;
                }
                if (played != RepeatPlay::NotInstance) {
                    waitFor = action.arg0;
                    waitScope = scope;
                    return loop ? Outcome::Stop : Outcome::Wait;
                }
            }
            // T-201: the nearest symbol instance scope with a timeline of that name, else the root.
            const NodeId group = timelineScope(node, action.arg0);
            const std::string_view path = scopePath(group);
            if ((action.flags & kPlayHold) != 0 && doc.isTimelinePlaying(path, name)) {
                waitFor = action.arg0;  // Explicit `hold`: keep the running playback.
                waitGroup = group;
                return loop ? Outcome::Stop : Outcome::Wait;
            }
            PlayOptions options;
            options.reverse = (action.flags & kPlayReverse) != 0;
            options.loop = loop;
            options.once = (action.flags & kPlayOnce) != 0;
            options.hold = !options.once;
            options.fromMarker = action.arg1 != kInvalidString ? ir.string(action.arg1) : std::string_view{};
            if (doc.playTimeline(path, name, options) != Status::kOk) {
                return Outcome::Done;
            }
            waitFor = action.arg0;
            waitGroup = group;
            return loop ? Outcome::Stop : Outcome::Wait;
        }
        case TriggerAction::Stop: {
            if ((action.flags & kActionMedia) != 0) {
                const NodeId target = doc.byId(ir.string(action.arg0)).node;
                if (doc.particlesOf(target) != kInvalidNode) {
                    doc.stopParticles(target);  // T-202: emission off.
                    return Outcome::Done;
                }
                // T-191: a stopped video/sequence never ends; its waiting chains are dropped.
                doc.stopMedia(target);
                m_waiters.erase(std::remove_if(m_waiters.begin(), m_waiters.end(),
                                               [&](const Waiter& w) {
                                                   return w.timeline == action.arg0 &&
                                                          w.scope == kInvalidNode;
                                               }),
                                m_waiters.end());
                return Outcome::Done;
            }
            NodeId scope = kInvalidNode;
            if (doc.m_repeats->stop(node, action.arg0, scope)) {
                // T-177: the instance's copy; its waiting chains would never resume.
                m_waiters.erase(std::remove_if(m_waiters.begin(), m_waiters.end(),
                                               [&](const Waiter& w) {
                                                   return w.timeline == action.arg0 && w.scope == scope;
                                               }),
                                m_waiters.end());
                return Outcome::Done;
            }
            const NodeId group = timelineScope(node, action.arg0);  // T-201.
            doc.stopTimeline(scopePath(group), ir.string(action.arg0));
            // A stopped group never fires End: its waiting chains would never resume.
            m_waiters.erase(std::remove_if(m_waiters.begin(), m_waiters.end(),
                                           [&](const Waiter& w) {
                                               return w.timeline == action.arg0 && w.scope == kInvalidNode &&
                                                      w.group == group;
                                           }),
                            m_waiters.end());
            return Outcome::Done;
        }
        case TriggerAction::Seek: {
            std::string_view name = ir.string(action.arg0);
            float t = action.arg2;
            if ((action.flags & kActionMedia) != 0) {
                doc.seekMedia(doc.byId(name).node, t);  // T-191.
                return Outcome::Done;
            }
            const std::string_view path = scopePath(timelineScope(node, action.arg0));  // T-201.
            if (action.arg1 != kInvalidString) {
                int group = doc.findTimelineGroup(path, name);
                if (group >= 0) {
                    for (const IrTimelineGroupMarker& m :
                         doc.m_timelineGroups->groups[static_cast<size_t>(group)].markers) {
                        if (m.name == action.arg1) {
                            t = m.time;
                            break;
                        }
                    }
                }
            }
            if (!doc.m_repeats->seek(node, action.arg0, t)) {  // T-177: an instance's copy first.
                doc.seekTimeline(path, name, t);
            }
            return Outcome::Done;
        }
        case TriggerAction::Remove:
            // T-177: `item-exit ... then remove`.
            doc.m_repeats->removeAction(node);
            return Outcome::Stop;
        case TriggerAction::Consume:
            // T-394 (08.15): decided when the OutsidePress was dispatched (consumesOutsidePress()).
            return Outcome::Done;
        case TriggerAction::Reset:
            // T-399 (08 §8.18): `reset(value)`, the element's `bind:value` applied again.
            doc.m_bindings->reapplyValue(node);
            return Outcome::Done;
        case TriggerAction::State: {
            // T-201: `target=#<id>` (compiled to the path id inside the trigger's symbol scope).
            NodeId target = node;
            if ((action.flags & kStateTarget) != 0) {
                target = doc.byId(ir.string(action.arg1)).node;
                if (target == kInvalidNode) {
                    return Outcome::Done;
                }
            }
            const uint8_t mode = static_cast<uint8_t>(action.flags & ~(kStateTarget | kStateBuiltin));
            if ((action.flags & kStateBuiltin) != 0) {
                // T-390 (27.3.2): `state(Disabled, ...)` (arg0 = 0); T-392 (27.3.5): another
                // built-in state (arg0 = its ElementState bit).
                doc.applyBuiltinState(
                    target, action.arg0 == 0 ? uint16_t{kStateDisabled} : static_cast<uint16_t>(action.arg0),
                    mode);
                return Outcome::Done;
            }
            doc.applyCustomState(target, action.arg0, mode);
            return Outcome::Done;
        }
        case TriggerAction::Class: {
            ElementRef ref{&doc, node, doc.m_arena.generation(node)};
            std::string_view cls = ir.string(action.arg0);
            switch (static_cast<TriggerMode>(action.flags)) {
                case TriggerMode::On:
                    ref.addClass(cls);
                    break;
                case TriggerMode::Off:
                    ref.removeClass(cls);
                    break;
                case TriggerMode::Toggle:
                    ref.toggleClass(cls);
                    break;
            }
            return Outcome::Done;
        }
        case TriggerAction::Show:
            doc.showElement(node);
            return Outcome::Done;
        case TriggerAction::Hide:
            doc.hideElement(node);
            return Outcome::Done;
        case TriggerAction::Emit:
            // T-175: the port's value, port listeners and `call` handlers (binding_runtime.cpp).
            doc.m_bindings->emit(node, action);
            return Outcome::Done;
        case TriggerAction::Spawn: {
            // T-201 (27.2.3): the new instance's top-left goes to `at` (viewport px).
            Vec2 at{};
            const SpawnAt where = static_cast<SpawnAt>(action.flags);
            if (where == SpawnAt::Pointer) {
                at = doc.m_events->pointerPosition();
            } else {
                const NodeId anchor =
                    where == SpawnAt::Element ? doc.byId(ir.string(action.arg0)).node : node;
                if (anchor == kInvalidNode || !doc.m_arena.alive(anchor)) {
                    return Outcome::Done;
                }
                const Rect box = ElementRef{&doc, anchor, doc.m_arena.generation(anchor)}.boundingBox();
                at = Vec2{box.x + box.w * 0.5f, box.y + box.h * 0.5f};
            }
            doc.m_repeats->spawn(node, action.extra, at, action.arg2, action.arg1);
            return Outcome::Done;
        }
        case TriggerAction::Set:
            // T-201: an `in` port is set (its bindings and `port()` triggers react); an `out`
            // port takes the value and emits it.
            if (action.flags >= static_cast<uint8_t>(SetSource::PointerFractionX) &&
                action.flags <= static_cast<uint8_t>(SetSource::PointerLocalY)) {
                // T-386 (08.11): `pointer.fraction(#id)` / `pointer.local(#id)` -- the firing
                // event's position relative to that element, then the EventValue path (converters).
                uint32_t bits = 0;
                std::memcpy(&bits, &action.arg2, sizeof(bits));
                const ElementRef target = doc.byId(ir.string(bits));
                const SetSource src = static_cast<SetSource>(action.flags);
                const bool fraction =
                    src == SetSource::PointerFractionX || src == SetSource::PointerFractionY;
                const bool yAxis = src == SetSource::PointerFractionY || src == SetSource::PointerLocalY;
                Vec2 p{};
                if (target.valid()) {
                    p = fraction ? doc.fractionIn(target.node, m_runPointer)
                                 : doc.localPosIn(target.node, m_runPointer);
                }
                IrTriggerAction asValue = action;
                asValue.flags = static_cast<uint8_t>(SetSource::EventValue);
                doc.m_bindings->setAction(node, asValue, yAxis ? p.y : p.x, 0xFFFFFFFFu);
                return Outcome::Done;
            }
            doc.m_bindings->setAction(node, action, value, port);
            return Outcome::Done;
        case TriggerAction::Sound:
        case TriggerAction::Haptic: {
            if (!doc.m_hostEventListener || doc.m_unloading) {  // T-260a.
                return Outcome::Done;
            }
            HostEvent event;
            event.kind = action.kind == static_cast<uint8_t>(TriggerAction::Sound) ? HostEventKind::Sound
                                                                                   : HostEventKind::Haptic;
            event.name = ir.string(action.arg0);
            event.volume = action.kind == static_cast<uint8_t>(TriggerAction::Sound) ? action.arg2 : 1.0f;
            // T-194: `haptic(<cue>, <strength>)`; a pre-T-194 IR carries 0 there (= default 1).
            if (action.kind == static_cast<uint8_t>(TriggerAction::Haptic)) {
                event.strength = action.arg2 > 0.0f ? action.arg2 : 1.0f;
            }
            event.player = player;
            event.element = node;
            StringId id = doc.m_arena.element(node).id;
            event.elementId = id != kInvalidString ? doc.string(id) : std::string_view{};
            event.instanceId = doc.m_instanceId;
            event.itemIndex = doc.m_repeats->itemIndexOf(node, nullptr);  // T-177.
            HostEventListener listener = doc.m_hostEventListener;         // The callback may replace it.

            listener(event);
            return Outcome::Done;
        }
        case TriggerAction::Focus: {
            // T-392 (27.3.5): `focus()` without an argument (kInvalidString) focuses the element.
            ElementRef target = action.arg0 == kInvalidString
                                    ? ElementRef{&doc, node, doc.m_arena.generation(node)}
                                    : doc.byId(ir.string(action.arg0));
            if (target.valid()) {
                doc.setFocus(target, player);  // T-194: for the player whose input fired it.
            }
            return Outcome::Done;
        }
        case TriggerAction::Layer:
            // T-194 (27.2.3): `layer(overlay)` / `layer(normal)` as a runtime declaration.
            ElementRef{&doc, node, doc.m_arena.generation(node)}.setStyle(
                action.flags != 0 ? "layer: overlay" : "layer: normal");
            return Outcome::Done;
        default:
            return Outcome::Done;
    }
}

bool TriggerRuntime::waiting(NodeId node, TriggerEvent event) const {
    const TriggerChunk& tc = m_doc.m_ir.triggers();
    for (const Waiter& w : m_waiters) {
        if (w.node == node && w.trigger < tc.triggers().size() &&
            static_cast<TriggerEvent>(tc.triggers()[w.trigger].event) == event) {
            return true;
        }
    }
    return false;
}

bool TriggerRuntime::consumesOutsidePress(NodeId node) {
    // T-394 (08.15): any `consume()` in an `on:outsidepress` trigger of `node` takes the press
    // (the compiler only allows it there). Its `if` conditions are not evaluated: the actions run
    // after the input, the press is decided now.
    const NodeArena& arena = m_doc.m_arena;
    if (node == kInvalidNode || !arena.alive(node) || arena.kind(node) != NodeKind::Element) {
        return false;
    }
    const uint32_t table = arena.common(node).triggerTable;
    const TriggerChunk& tc = m_doc.m_ir.triggers();
    if (table == 0 || table > tc.tables().size()) {
        return false;
    }
    const IrTriggerTable& t = tc.tables()[table - 1];
    for (uint32_t k = 0; k < t.triggerCount; ++k) {
        const IrTrigger& trig = tc.triggers()[t.triggerOffset + k];
        if (static_cast<TriggerEvent>(trig.event) != TriggerEvent::OutsidePress) {
            continue;
        }
        for (uint32_t a = 0; a < trig.actionCount; ++a) {
            if (tc.actions()[trig.actionOffset + a].kind == static_cast<uint8_t>(TriggerAction::Consume)) {
                return true;
            }
        }
    }
    return false;
}

bool TriggerRuntime::hasPointerDrag(NodeId node) { return hasTrigger(node, TriggerEvent::PointerDrag); }

uint32_t TriggerRuntime::dropIndexOf(NodeId target, Vec2 pos, uint32_t targetItem, Vec2 fraction) {
    // T-394 (08.15): a `<repeat>` instance (or an element inside one): its entry + lower half.
    if (targetItem != 0xFFFFFFFFu) {
        return targetItem + (fraction.y >= 0.5f ? 1u : 0u);
    }
    // The `<repeat>` element itself: the entry under the pointer, by that entry's own geometry.
    const NodeArena& arena = m_doc.m_arena;
    if (target == kInvalidNode || !arena.alive(target) || arena.kind(target) != NodeKind::Element ||
        arena.element(target).id == kInvalidString) {
        return 0xFFFFFFFFu;
    }
    const std::string_view id = m_doc.string(arena.element(target).id);
    const uint32_t item = m_doc.m_repeats->indexAt(id, pos);
    RepeatController::ItemSpan span;
    if (item == RepeatController::kNoItem || !m_doc.m_repeats->itemSpan(id, item, span) ||
        span.element != target) {
        return 0xFFFFFFFFu;
    }
    float y = 0.0f;
    if (span.instance != kInvalidNode) {
        y = m_doc.fractionIn(span.instance, pos).y;
    } else if (span.height > 0.0f) {
        y = (m_doc.localPosIn(span.element, pos).y - span.top) / span.height;
    }
    return item + (y >= 0.5f ? 1u : 0u);
}

bool TriggerRuntime::hasTrigger(NodeId node, TriggerEvent event) const {
    const NodeArena& arena = m_doc.m_arena;
    if (node == kInvalidNode || !arena.alive(node)) {
        return false;
    }
    uint32_t table = arena.common(node).triggerTable;
    const TriggerChunk& tc = m_doc.m_ir.triggers();
    if (table == 0 || table > tc.tables().size()) {
        return false;
    }
    const IrTriggerTable& t = tc.tables()[table - 1];
    for (uint32_t k = 0; k < t.triggerCount; ++k) {
        if (static_cast<TriggerEvent>(tc.triggers()[t.triggerOffset + k].event) == event) {
            return true;
        }
    }
    return false;
}

void TriggerRuntime::cancelWaiting(NodeId node, TriggerEvent event) {
    const TriggerChunk& tc = m_doc.m_ir.triggers();
    m_waiters.erase(std::remove_if(m_waiters.begin(), m_waiters.end(),
                                   [&](const Waiter& w) {
                                       return w.node == node && w.trigger < tc.triggers().size() &&
                                              static_cast<TriggerEvent>(tc.triggers()[w.trigger].event) ==
                                                  event;
                                   }),
                    m_waiters.end());
}

void TriggerRuntime::forgetNode(NodeId node) {
    m_waiters.erase(std::remove_if(m_waiters.begin(), m_waiters.end(),
                                   [node](const Waiter& w) { return w.node == node; }),
                    m_waiters.end());
    // Queued jobs are generation-checked when they run; the timeline index drops the node now.
    m_timelineTriggers.erase(
        std::remove_if(m_timelineTriggers.begin(), m_timelineTriggers.end(),
                       [node](const std::pair<NodeId, uint32_t>& t) { return t.first == node; }),
        m_timelineTriggers.end());
}

// -- Document: custom states ---------------------------------------------------------------------

Status Document::setCustomState(ElementRef element, std::string_view name, bool on) {
    DispatchGuard dispatch(m_context);  // T-260a: enter / exit triggers.
    if (!element.valid() || element.doc != this || m_arena.kind(element.node) != NodeKind::Element) {
        return Status::kInvalidArgument;
    }
    if (const uint16_t builtin = builtinStateAlias(name); builtin != 0) {
        // T-390 / T-392 (27.3.2, 27.3.5): Disabled, Focused, Hover, Active, Checked are built-in.
        applyBuiltinState(element.node, builtin,
                          static_cast<uint8_t>(on ? TriggerMode::On : TriggerMode::Off));
        return Status::kOk;
    }
    std::span<const uint32_t> names = m_ir.customStates().names();
    for (uint32_t bit = 0; bit < names.size(); ++bit) {
        if (m_ir.string(names[bit]) == name) {
            applyCustomState(element.node, bit,
                             static_cast<uint8_t>(on ? TriggerMode::On : TriggerMode::Off));
            return Status::kOk;
        }
    }
    return Status::kNotFound;
}

void Document::applyBuiltinState(NodeId node, uint16_t bit, uint8_t mode) {
    if (!m_arena.alive(node) || m_arena.kind(node) != NodeKind::Element) {
        return;
    }
    ElementData& data = m_arena.element(node);
    const bool before = (data.stateBits & bit) != 0;
    const bool on = static_cast<TriggerMode>(mode) == TriggerMode::Toggle
                        ? !before
                        : static_cast<TriggerMode>(mode) == TriggerMode::On;
    if (bit == kStateDisabled || bit == kStateChecked) {
        // ElementRef::setState: Checked also runs the check / uncheck triggers.
        ElementRef{this, node, m_arena.generation(node)}.setState(static_cast<ElementState>(bit), on);
        return;
    }
    // T-392: Focused / Hover / Active set the bit only (the selector state, like `bind:state.X`);
    // the focus target and the pointer state do not move.
    if (before == on) {
        return;
    }
    data.stateBits =
        on ? static_cast<uint16_t>(data.stateBits | bit) : static_cast<uint16_t>(data.stateBits & ~bit);
    m_arena.markDirty(node, static_cast<uint8_t>(kDirtyStyle));
    m_arena.traversePreorder(node, [this](NodeId n) {
        m_arena.markDirty(n, static_cast<uint8_t>(kDirtyStyle));
        return true;
    });
}

Status Document::fireTrigger(ElementRef element, TriggerEvent event) {
    switch (event) {
        case TriggerEvent::Click:
        case TriggerEvent::Press:
        case TriggerEvent::Release:
        case TriggerEvent::Hover:
        case TriggerEvent::Leave:
            break;
        default:
            return Status::kInvalidArgument;
    }
    if (!element.valid() || element.doc != this || m_arena.kind(element.node) != NodeKind::Element) {
        return Status::kInvalidArgument;
    }
    DispatchGuard dispatch(m_context);  // T-260a: the fired actions.
    m_triggers->fire(element.node, event);
    return Status::kOk;
}

bool Document::hasCustomState(ElementRef element, std::string_view name) const {
    if (!element.valid() || element.doc != this) {
        return false;
    }
    if (const uint16_t builtin = builtinStateAlias(name); builtin != 0) {
        // T-390 / T-392 (27.3.2, 27.3.5): the built-in state bit.
        return m_arena.kind(element.node) == NodeKind::Element &&
               (m_arena.element(element.node).stateBits & builtin) != 0;
    }
    std::span<const uint32_t> names = m_ir.customStates().names();
    for (uint32_t bit = 0; bit < names.size(); ++bit) {
        if (m_ir.string(names[bit]) == name) {
            return (m_arena.common(element.node).customStates & (1u << bit)) != 0;
        }
    }
    return false;
}

void Document::applyCustomState(NodeId node, uint32_t bit, uint8_t mode) {
    if (bit >= kMaxCustomStates || !m_arena.alive(node) || m_arena.kind(node) != NodeKind::Element) {
        return;
    }
    const uint32_t mask = 1u << bit;
    const uint32_t before = m_arena.common(node).customStates;
    bool on = true;
    switch (static_cast<TriggerMode>(mode)) {
        case TriggerMode::On:
            on = true;
            break;
        case TriggerMode::Off:
            on = false;
            break;
        case TriggerMode::Toggle:
            on = (before & mask) == 0;
            break;
    }
    uint32_t after = before & ~mask;
    if (on) {
        // 27.1: setting one member of a state group clears the others.
        for (const IrStateGroup& group : m_ir.customStates().groups()) {
            if ((group.mask & mask) != 0) {
                after &= ~group.mask;
            }
        }
        after |= mask;
    }
    if (after == before) {
        return;
    }
    m_arena.common(node).customStates = after;
    markStyleDirtySubtree(m_arena, node);
    const uint32_t exits = before & ~after;
    const uint32_t enters = after & ~before;
    for (uint32_t b = 0; b < kMaxCustomStates; ++b) {
        if ((exits & (1u << b)) != 0) {
            m_triggers->fire(node, TriggerEvent::StateExit, b);
        }
    }
    for (uint32_t b = 0; b < kMaxCustomStates; ++b) {
        if ((enters & (1u << b)) != 0) {
            m_triggers->fire(node, TriggerEvent::StateEnter, b);
        }
    }
}

void Document::checkedChanged(NodeId node, bool checked) { m_triggers->onCheckedChanged(node, checked); }

// -- Document: surface and element visibility ----------------------------------------------------

NodeId Document::surfaceElement() const {
    if (m_root == kInvalidNode || !m_arena.alive(m_root)) {
        return kInvalidNode;
    }
    for (NodeId c = m_arena.firstChild(m_root); c != kInvalidNode; c = m_arena.nextSibling(c)) {
        if (m_arena.kind(c) == NodeKind::Element && string(m_arena.element(c).tag) == "body") {
            return c;
        }
    }
    return m_root;
}

Status Document::requestShow() {
    if (m_root == kInvalidNode) {
        return Status::kInvalidArgument;
    }
    if (deferSurfaceRequest(kSurfaceRequestShow)) {
        return Status::kOk;
    }
    return applyShow();
}

Status Document::requestHide() {
    if (m_root == kInvalidNode) {
        return Status::kInvalidArgument;
    }
    if (deferSurfaceRequest(kSurfaceRequestHide)) {
        return Status::kOk;
    }
    return applyHide();
}

bool Document::deferSurfaceRequest(uint8_t request) {
    // T-397 (08 §8.17): a host call made while an event is being dispatched (a listener, a port /
    // visibility callback, anything inside Context::update()) is applied once the outermost
    // dispatching call returns (Context::runPendingDocOps); the last request wins.
    if (m_context == nullptr || m_context->m_dispatchDepth == 0) {
        return false;
    }
    m_pendingSurfaceRequest = request;
    m_context->m_surfaceRequestsPending = true;
    return true;
}

void Document::applyPendingSurfaceRequest() {
    const uint8_t request = m_pendingSurfaceRequest;
    m_pendingSurfaceRequest = kSurfaceRequestNone;
    if (request == kSurfaceRequestShow) {
        applyShow();
    } else if (request == kSurfaceRequestHide) {
        applyHide();
    }
}

Status Document::applyShow() {
    DispatchGuard dispatch(m_context);  // T-260a: OnShown and the `show` triggers.
    if (m_root == kInvalidNode) {
        return Status::kInvalidArgument;
    }
    NodeId surface = surfaceElement();
    if (m_surfaceVisible && !m_surfaceHiding) {
        return Status::kOk;
    }
    const bool wasHidden = !m_surfaceVisible;
    m_surfaceHiding = false;
    m_surfaceVisible = true;
    m_shownOnce = true;
    // A pending `then hide` must not win (T-349: nor one of a direct child of `<body>`).
    m_triggers->cancelWaiting(surface, TriggerEvent::Hide);
    if (surface != m_root) {
        for (NodeId c = m_arena.firstChild(surface); c != kInvalidNode; c = m_arena.nextSibling(c)) {
            if (m_arena.kind(c) == NodeKind::Element) {
                m_triggers->cancelWaiting(c, TriggerEvent::Hide);
            }
        }
    }
    if (wasHidden && m_visibilityListener && !m_unloading) {  // T-260a: no OnShown when unloading.
        std::function<void(bool)> listener = m_visibilityListener;
        listener(true);
    }
    fireSurfaceEvent(true);  // T-349: `<body>` and its direct children.
    applyAutofocus();        // T-194 (27.7).
    return Status::kOk;
}

void Document::setInitialVisibility(bool visible) {
    m_surfaceHiding = false;
    m_surfaceVisible = visible;
    if (!visible) {
        m_lastHideSource = "host";  // T-374.
    }
}

Status Document::applyHide() {
    DispatchGuard dispatch(m_context);  // T-260a: the `hide` triggers and OnHidden.
    if (m_root == kInvalidNode) {
        return Status::kInvalidArgument;
    }
    if (!m_surfaceVisible || m_surfaceHiding) {
        return Status::kOk;
    }
    m_surfaceHiding = true;
    m_lastHideSource = m_triggers->currentSource();  // T-374 (17 §17.33.3).
    // T-349: `<body>` and its direct children; the deferred run settles the hide
    // (finishPendingHides) even when nothing was queued.
    fireSurfaceEvent(false);
    return Status::kOk;
}

void Document::fireSurfaceEvent(bool show) {
    const TriggerEvent event = show ? TriggerEvent::Show : TriggerEvent::Hide;
    NodeId surface = surfaceElement();
    // One run for all members: a member without a waiting chain must not settle the surface's
    // hide before the later members have started theirs.
    m_triggers->beginDefer();
    m_triggers->fire(surface, event);
    if (surface != kInvalidNode && surface != m_root) {
        for (NodeId c = m_arena.firstChild(surface); c != kInvalidNode; c = m_arena.nextSibling(c)) {
            if (m_arena.kind(c) == NodeKind::Element) {
                m_triggers->fire(c, event);
            }
        }
    }
    m_triggers->endDefer();
}

bool Document::isSurfaceMember(NodeId node) const {
    if (node == kInvalidNode || !m_arena.alive(node)) {
        return false;
    }
    NodeId surface = surfaceElement();
    if (node == surface || node == m_root) {
        return true;
    }
    return surface != m_root && m_arena.kind(node) == NodeKind::Element && m_arena.parent(node) == surface;
}

bool Document::surfaceHideWaiting() const {
    NodeId surface = surfaceElement();
    if (m_triggers->waiting(surface, TriggerEvent::Hide)) {
        return true;
    }
    if (surface == kInvalidNode || surface == m_root) {
        return false;
    }
    for (NodeId c = m_arena.firstChild(surface); c != kInvalidNode; c = m_arena.nextSibling(c)) {
        if (m_arena.kind(c) == NodeKind::Element && m_triggers->waiting(c, TriggerEvent::Hide)) {
            return true;
        }
    }
    return false;
}

void Document::finishSurfaceHide() {
    if (!m_surfaceHiding) {
        return;
    }
    m_surfaceHiding = false;
    m_surfaceVisible = false;
    if (m_visibilityListener && !m_unloading) {  // T-260a: no OnHidden when unloading.
        std::function<void(bool)> listener = m_visibilityListener;
        listener(false);
    }
}

void Document::finishPendingHides() {
    // T-349: the surface hides once no member (`<body>` or a direct child) still waits.
    if (m_surfaceHiding && !surfaceHideWaiting()) {
        finishSurfaceHide();
    }
    for (size_t i = 0; i < m_hidingElements.size();) {
        NodeId node = m_hidingElements[i];
        if (!m_arena.alive(node)) {
            m_hidingElements.erase(m_hidingElements.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (m_triggers->waiting(node, TriggerEvent::Hide)) {
            ++i;
            continue;
        }
        m_hidingElements.erase(m_hidingElements.begin() + static_cast<std::ptrdiff_t>(i));
        ElementRef{this, node, m_arena.generation(node)}.setStyle("visibility: hidden");
        m_hiddenElements.push_back(node);
    }
}

bool Document::surfaceChildShown(NodeId node) const {
    if (std::find(m_hiddenElements.begin(), m_hiddenElements.end(), node) != m_hiddenElements.end() ||
        std::find(m_hidingElements.begin(), m_hidingElements.end(), node) != m_hidingElements.end()) {
        return false;
    }
    const ElementData& data = m_arena.element(node);
    if (data.style.display == Display::None) {
        return false;
    }
    // A runtime `visibility` (show() / hide(), setStyle) wins over the resolved style, which is
    // only brought up to date by the next update().
    for (auto it = data.runtimeDecls.rbegin(); it != data.runtimeDecls.rend(); ++it) {
        if (it->prop == style_decode::styleprop::Visibility && it->varName == kInvalidString &&
            it->value.type == ValueType::Enum) {
            return it->value.e != static_cast<uint32_t>(Visibility::Hidden);
        }
    }
    return data.style.visibility != Visibility::Hidden;
}

void Document::showElement(NodeId node) {
    if (node == surfaceElement() || node == m_root) {
        applyShow();  // T-397: a trigger action applies at once (never deferred).
        return;
    }
    if (!m_arena.alive(node) || m_arena.kind(node) != NodeKind::Element) {
        return;
    }
    bool wasHidden = eraseNode(m_hiddenElements, node);
    wasHidden = eraseNode(m_hidingElements, node) || wasHidden;
    wasHidden = wasHidden || m_arena.element(node).style.visibility == Visibility::Hidden;
    m_triggers->cancelWaiting(node, TriggerEvent::Hide);
    ElementRef{this, node, m_arena.generation(node)}.setStyle("visibility: visible");
    if (isSurfaceMember(node) && (!m_surfaceVisible || m_surfaceHiding)) {
        // T-374 (27.2.4): `show()` of a direct child of `<body>` shows a hidden (or hiding)
        // surface; requestShow() fires the members' `show`, this element's included.
        applyShow();
        return;
    }
    if (wasHidden) {
        m_triggers->fire(node, TriggerEvent::Show);
    }
}

void Document::hideElement(NodeId node) {
    if (m_surfaceHiding && isSurfaceMember(node)) {
        // T-349 (27.2.4): `hide()` / `... then hide` of `<body>` or a direct child while the
        // surface hides belongs to the surface: the run ends in finishPendingHides(), which
        // hides the surface once no member's `hide` chain still waits. No element-level
        // `visibility: hidden`.
        return;
    }
    if (node == surfaceElement() || node == m_root) {
        applyHide();  // T-397: a trigger action applies at once (never deferred).
        return;
    }
    if (!m_arena.alive(node) || m_arena.kind(node) != NodeKind::Element) {
        return;
    }
    if (isSurfaceMember(node)) {
        // T-374 (27.2.4): `hide()` of a direct child of `<body>` that leaves no other shown direct
        // child hides the surface (the members' `hide` triggers and `then hide` chains, then
        // OnHidden) and leaves the element's own visibility alone, so a later surface `show`
        // brings it back. With another shown child left, only this element hides (below).
        bool otherShown = false;
        const NodeId surface = surfaceElement();
        for (NodeId c = m_arena.firstChild(surface); c != kInvalidNode; c = m_arena.nextSibling(c)) {
            if (c != node && m_arena.kind(c) == NodeKind::Element && surfaceChildShown(c)) {
                otherShown = true;
                break;
            }
        }
        if (!otherShown) {
            applyHide();
            return;
        }
    }
    if (eraseNode(m_hidingElements, node)) {
        ElementRef{this, node, m_arena.generation(node)}.setStyle("visibility: hidden");
        m_hiddenElements.push_back(node);
        return;
    }
    if (std::find(m_hiddenElements.begin(), m_hiddenElements.end(), node) != m_hiddenElements.end()) {
        return;
    }
    m_hidingElements.push_back(node);
    m_triggers->fire(node, TriggerEvent::Hide);  // Settled by finishPendingHides() after the run.
}

void Document::setHostEventListener(HostEventListener listener) { m_hostEventListener = std::move(listener); }

void Document::setVisibilityListener(std::function<void(bool visible)> listener) {
    m_visibilityListener = std::move(listener);
}

}  // namespace xt
