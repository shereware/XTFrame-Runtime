#include <algorithm>
#include <cstdlib>
#include <string>

#include "event_dispatcher.h"
#include "xt/document.h"
#include "xt/node.h"
#include "xt/property.h"

/// Drag and drop (docs/design/27-interaction.md §27.7, 08 §8.9, docs/tasks/T-194.md goal 3):
/// `draggable` / `drop-target` elements, the drag.start / drag.over / drag.leave / drop events,
/// the `Dragging` / `DropHover` states (through TriggerSink::onDragState) and the ghost offset the
/// Painter draws the source snapshot at.
///
/// Not covered by the design (conservative choices made here):
///   - One drag per Document at a time; a pointer press becomes a drag once it moved more than
///     the source's `drag-threshold` (default 4 px, T-388) while pressed, and a drag never
///     produces a Click (also when it was cancelled and the button is released later).
///   - The drop target under a pointer drag is the nearest `drop-target` ancestor-or-self of the
///     hover hit, excluding the source itself and its subtree (a container holding the source is
///     allowed). A gamepad drag targets its player's focus the same way.
///   - drag.over fires when the drag enters a target and (T-388) on every pointer move over it;
///     a drag released or
///     cancelled over a target sends it drag.leave (cancel) or drop (release) -- never both.
///   - T-388 (08.12): a cancel (Escape, a second pointer button, PointerCancel, gamepad B,
///     cancelDrag()) clears the states, sends drag.leave to the target under the drag, then
///     DragCancel and DragEnd (dropped = false) to the source. Every drag ends with DragEnd.
///     DragOver / Drop carry Event::dropFraction (Document::fractionIn of the drop target).
///   - T-388: edge auto-scroll (autoScroll(), every tick of a pointer drag): the nearest live
///     scroll container (`scroll`, or `auto` that overflows) of the drop target, else of the
///     element under the pointer, ancestor-or-self; `drag-autoscroll="false"` on it turns it off.
///   - The ghost: pointer drags offset it by the pointer's travel since the press; gamepad drags
///     centre it on the focused element plus (8, 8) px. T-394: `drag-ghost="none"` on the source
///     draws none (Document hands the Painter no ghost).
///   - T-394 (08.15): only the primary button starts a pointer drag (`draggable="any-button"`:
///     any); Escape before the threshold cancels the press (no Click); after a cancel the other
///     buttons are swallowed until every button is up. DragOver / Drop carry Event::dropIndex;
///     over its own row a source that is a drop target inside a `<repeat>` instance is the target.
///     While the edge auto-scroll moves, DragOver is sent again every frame (new geometry).
///   - T-201: a `draggable="axis-x"` element (a slider track) is no drag source: a press on it (or
///     inside it) and every pointer move until the release fire its `drag.value` with the
///     pointer's position across its border box (0..1, clamped; the element's own transform
///     applied). Left / right navigation while it has the focus fires `nav.value` (-1 / +1)
///     instead of moving the focus (focus_nav.cpp).
///   - T-393 (08.14): an external (OS) drag -- InputEvent::ExternalDragEnter / Move / Leave /
///     ExternalDrop -- is a drag with no source (m_drag.external): no threshold, no ghost, no
///     DragStart / DragCancel / DragEnd. DragOver on every move over a drop target, DragLeave
///     when it moves off or leaves the viewport, Drop (with Event::externalPaths) on release. A
///     drop with no move before it (the SDL fallback) sends Drop alone (no DragOver, no
///     DropHover). While an internal drag (or a press on a draggable) is in progress, external
///     drag input is ignored.

namespace xt {

namespace {

bool liveElement(const NodeArena& arena, NodeId node) {
    return node != kInvalidNode && arena.alive(node) && arena.kind(node) == NodeKind::Element;
}

Vec2 centerOf(const NodeArena& arena, NodeId node) {
    const ElementData& data = arena.element(node);
    return apply(arena.common(node).world, Vec2{data.box.width * 0.5f, data.box.height * 0.5f});
}

// T-388 (08.12): the edge band (px) and the top speed (px/s) of the drag auto-scroll.
constexpr float kAutoScrollBand = 24.0f;
constexpr float kAutoScrollSpeed = 600.0f;

/// Scroll velocity factor along one axis: -1..0 near the start edge, 0..1 near the end edge.
float edgeFactor(float local, float size) {
    float v = 0.0f;
    if (local <= kAutoScrollBand) {
        v -= (kAutoScrollBand - local) / kAutoScrollBand;
    }
    if (size - local <= kAutoScrollBand) {
        v += (kAutoScrollBand - (size - local)) / kAutoScrollBand;
    }
    return v;
}

}  // namespace

std::string_view EventDispatcher::attributeOf(const NodeArena& arena, NodeId node,
                                              std::string_view name) const {
    if (m_doc == nullptr || !liveElement(arena, node)) {
        return std::string_view{};
    }
    for (const auto& entry : arena.element(node).attributes) {
        if (m_doc->string(entry.first) == name) {
            return m_doc->string(entry.second);
        }
    }
    return std::string_view{};
}

float EventDispatcher::dragThreshold(const NodeArena& arena, NodeId source) const {
    // T-388 (08.12): `drag-threshold="<px>"` on the draggable element, default 4.
    const std::string_view text = attributeOf(arena, source, "drag-threshold");
    if (text.empty()) {
        return 4.0f;
    }
    const std::string copy(text);
    char* end = nullptr;
    const float v = std::strtof(copy.c_str(), &end);
    return end != copy.c_str() && v >= 0.0f ? v : 4.0f;
}

Vec2 EventDispatcher::dropFractionOf(const NodeArena& arena, NodeId target, Vec2 pos) const {
    return m_doc != nullptr && liveElement(arena, target) ? m_doc->fractionIn(target, pos) : Vec2{};
}

void EventDispatcher::autoScroll(NodeArena& arena, NodeId root, float time) {
    const float dt = time - m_autoScrollClock;
    m_autoScrollClock = time;
    if (!m_drag.active || m_drag.gamepad) {
        m_autoScrolled = false;
        return;
    }
    // T-394 (08.15): the previous step scrolled, and the frame since laid the content out again:
    // the pointer is over different geometry, so DragOver is sent again (fresh target, fraction,
    // index) -- once per frame while the auto-scroll runs.
    if (m_autoScrolled && root != kInvalidNode) {
        m_autoScrolled = false;
        const NodeId hit = hitTestDetailed(arena, root, m_drag.pos).node;
        updateDragOver(arena, m_drag.external ? dropTargetOf(arena, hit) : pointerDropTarget(arena, hit));
        if (!m_drag.active) {
            return;  // A DragOver listener ended the drag.
        }
    }
    if (!(dt > 0.0f)) {
        return;
    }
    const NodeId from = liveElement(arena, m_drag.over) ? m_drag.over : m_hoverNode;
    for (NodeId n = from; n != kInvalidNode && arena.alive(n); n = arena.parent(n)) {
        if (arena.kind(n) != NodeKind::Element) {
            continue;
        }
        const ElementData& data = arena.element(n);
        const float maxX = maxScrollX(data.box);
        const float maxY = maxScrollY(data.box);
        const bool liveX = scrollsOnAxis(usedOverflowX(data.style), maxX);
        const bool liveY = scrollsOnAxis(usedOverflowY(data.style), maxY);
        if (!liveX && !liveY) {
            continue;
        }
        // The nearest scroll container decides: switched off, or the pointer outside it, means
        // no auto-scroll (its ancestors are not tried).
        if (attributeOf(arena, n, "drag-autoscroll") == "false") {
            return;
        }
        Mat2x3 inverse;
        if (!invert(arena.common(n).world, inverse)) {
            return;
        }
        const Vec2 local = apply(inverse, m_drag.pos);
        const float w = data.box.width;
        const float h = data.box.height;
        if (local.x < 0.0f || local.y < 0.0f || local.x > w || local.y > h) {
            return;
        }
        const float step = kAutoScrollSpeed * dt;
        // T-387 (08.12): through setScroll(), which clamps and dispatches Scroll on a change.
        const float curX = data.style.base[prop::ScrollX].f;
        const float curY = data.style.base[prop::ScrollY].f;
        const float nextX = liveX ? curX + edgeFactor(local.x, w) * step : curX;
        const float nextY = liveY ? curY + edgeFactor(local.y, h) * step : curY;
        m_autoScrolled = setScroll(arena, n, nextX, nextY, /*notify=*/true);
        return;
    }
}

void EventDispatcher::axisDragValue(NodeArena& arena, Vec2 pos) {
    // T-201 (27.7): the pointer's position across the element's border box, 0..1.
    const NodeId node = m_axisDrag;
    if (!liveElement(arena, node) || m_triggerSink == nullptr) {
        m_axisDrag = kInvalidNode;
        return;
    }
    Mat2x3 inverse;
    if (!invert(arena.common(node).world, inverse)) {
        return;
    }
    const float width = arena.element(node).box.width;
    const float x = apply(inverse, pos).x;
    const float value = width > 0.0f ? (x <= 0.0f ? 0.0f : (x >= width ? 1.0f : x / width)) : 0.0f;
    m_triggerSink->onAxisValue(node, false, value, m_curPlayer);
}

NodeId EventDispatcher::pointerDropTarget(const NodeArena& arena, NodeId hit) const {
    // T-394 (08.15): over its own row, a source that is a drop target inside a `<repeat>` instance
    // is the target, so the row's own geometry gives the insertion index (targetItem = itself).
    const NodeId source = m_drag.source;
    if (liveElement(arena, source) && liveElement(arena, hit) &&
        (arena.common(source).flags2 & kDropTarget) != 0 &&
        (hit == source || arena.isAncestorOf(source, hit)) && m_triggerSink != nullptr &&
        m_triggerSink->itemIndexOf(source) != 0xFFFFFFFFu) {
        return source;
    }
    return dropTargetOf(arena, hit);
}

uint32_t EventDispatcher::dropIndexFor(NodeId target, Vec2 pos, uint32_t targetItem, Vec2 fraction) {
    if (m_triggerSink == nullptr) {
        return targetItem == 0xFFFFFFFFu ? 0xFFFFFFFFu : targetItem + (fraction.y >= 0.5f ? 1u : 0u);
    }
    return m_triggerSink->dropIndexOf(target, pos, targetItem, fraction);
}

NodeId EventDispatcher::dropTargetOf(const NodeArena& arena, NodeId hit) const {
    const NodeId source = m_drag.source;
    for (NodeId n = hit; liveElement(arena, n); n = arena.parent(n)) {
        if (liveElement(arena, source) && (n == source || arena.isAncestorOf(source, n))) {
            continue;  // Inside the dragged subtree: keep climbing to its containers.
        }
        if ((arena.common(n).flags2 & kDropTarget) != 0) {
            return n;
        }
    }
    return kInvalidNode;
}

Vec2 EventDispatcher::dragGhostOffset(const NodeArena& arena) const {
    if (!m_drag.active || !liveElement(arena, m_drag.source)) {
        return Vec2{};
    }
    if (!m_drag.gamepad) {
        return m_drag.pos - m_drag.start;
    }
    const NodeId focus = m_focus[m_drag.player < kMaxPlayers ? m_drag.player : 0];
    if (!liveElement(arena, focus) || focus == m_drag.source) {
        return Vec2{8, 8};
    }
    return centerOf(arena, focus) - centerOf(arena, m_drag.source) + Vec2{8, 8};
}

void EventDispatcher::startDrag(NodeArena& arena, NodeId source, uint8_t player, bool gamepad, Vec2 pos) {
    if (!liveElement(arena, source)) {
        m_drag = DragState{};
        return;
    }
    m_drag = DragState{};
    m_drag.active = true;
    m_drag.gamepad = gamepad;
    m_drag.player = player;
    m_drag.source = source;
    m_drag.start = pos;
    m_drag.pos = pos;
    m_autoScrollClock = m_time;  // T-388: the first auto-scroll step measures from here.
    if (!gamepad) {
        m_pressDragged = true;  // T-388: this press never clicks, even if the drag is cancelled.
    }
    if (m_triggerSink != nullptr) {
        m_triggerSink->onDragState(source, DragStateKind::Dragging, true);
    }
    Event ev;
    ev.type = EventType::DragStart;
    ev.target = source;
    ev.pos = pos;
    ev.player = player;
    ev.dragSource = source;
    ev.sourceItem = m_triggerSink != nullptr ? m_triggerSink->itemIndexOf(source) : 0xFFFFFFFFu;
    dispatch(arena, ev);
    if (gamepad && m_drag.active) {
        updateDragOver(arena, dropTargetOf(arena, m_focus[player]));
    }
}

void EventDispatcher::updateDragOver(NodeArena& arena, NodeId target) {
    // T-388 (08.12): a pointer drag sends DragOver again on every move over the same target (web
    // `dragover`), so dropFraction / targetItem follow the pointer; DropHover is set on entry only.
    const bool same = target == m_drag.over;
    if (!m_drag.active || (same && (m_drag.gamepad || !liveElement(arena, target)))) {
        return;
    }
    const NodeId old = m_drag.over;
    m_drag.over = target;
    const uint32_t sourceItem =
        m_triggerSink != nullptr ? m_triggerSink->itemIndexOf(m_drag.source) : 0xFFFFFFFFu;
    if (!same && liveElement(arena, old)) {
        if (m_triggerSink != nullptr) {
            m_triggerSink->onDragState(old, DragStateKind::DropHover, false);
        }
        Event ev;
        ev.type = EventType::DragLeave;
        ev.target = old;
        ev.pos = m_drag.pos;
        ev.player = m_drag.player;
        ev.dragSource = m_drag.source;
        ev.dropTarget = old;
        ev.sourceItem = sourceItem;
        ev.targetItem = m_triggerSink != nullptr ? m_triggerSink->itemIndexOf(old) : 0xFFFFFFFFu;
        dispatch(arena, ev);
    }
    if (liveElement(arena, target) && m_drag.active && m_drag.over == target) {
        if (!same && m_triggerSink != nullptr) {
            m_triggerSink->onDragState(target, DragStateKind::DropHover, true);
        }
        Event ev;
        ev.type = EventType::DragOver;
        ev.target = target;
        ev.pos = m_drag.pos;
        ev.dropFraction = dropFractionOf(arena, target, m_drag.pos);  // T-388.
        ev.player = m_drag.player;
        ev.dragSource = m_drag.source;
        ev.dropTarget = target;
        ev.sourceItem = sourceItem;
        ev.targetItem = m_triggerSink != nullptr ? m_triggerSink->itemIndexOf(target) : 0xFFFFFFFFu;
        ev.dropIndex = dropIndexFor(target, m_drag.pos, ev.targetItem, ev.dropFraction);  // T-394.
        dispatch(arena, ev);
    }
}

void EventDispatcher::finishDrag(NodeArena& arena, bool drop, bool cancel) {
    if (!m_drag.active) {
        return;
    }
    const DragState drag = m_drag;
    m_drag = DragState{};  // Listeners below see no drag in progress.
    if (cancel && !drag.gamepad && !drag.external && m_activeNode != kInvalidNode) {
        m_swallowButtons = true;  // T-394 (08.15): other buttons are swallowed until all are up.
    }
    const uint32_t sourceItem = m_triggerSink != nullptr && liveElement(arena, drag.source)
                                    ? m_triggerSink->itemIndexOf(drag.source)
                                    : 0xFFFFFFFFu;
    bool dropped = false;
    if (liveElement(arena, drag.over)) {
        if (m_triggerSink != nullptr) {
            m_triggerSink->onDragState(drag.over, DragStateKind::DropHover, false);
        }
        Event ev;
        ev.type = drop ? EventType::Drop : EventType::DragLeave;
        ev.target = drag.over;
        ev.pos = drag.pos;
        ev.player = drag.player;
        ev.dragSource = drag.source;
        ev.dropTarget = drag.over;
        ev.sourceItem = sourceItem;
        ev.targetItem = m_triggerSink != nullptr ? m_triggerSink->itemIndexOf(drag.over) : 0xFFFFFFFFu;
        ev.leftWindow = !drop;  // T-399 (08 §8.18): the drag ended (or left the window) here.
        if (drop) {
            ev.dropFraction = dropFractionOf(arena, drag.over, drag.pos);                      // T-388.
            ev.dropIndex = dropIndexFor(drag.over, drag.pos, ev.targetItem, ev.dropFraction);  // T-394.
            if (drag.external) {
                ev.externalPaths = m_externalPaths;  // T-393 (08.14).
            }
        }
        dispatch(arena, ev);
        if (drop && m_triggerSink != nullptr) {
            m_triggerSink->onDrop(ev);
        }
        dropped = drop;
    }
    if (!liveElement(arena, drag.source)) {
        return;
    }
    if (m_triggerSink != nullptr) {
        m_triggerSink->onDragState(drag.source, DragStateKind::Dragging, false);
    }
    // T-388 (08.12): DragCancel (a cancel only), then DragEnd, both to the source, bubbling. A
    // listener may destroy the source in between: the generation check skips the rest.
    const PathEntry source{.node = drag.source, .generation = arena.generation(drag.source)};
    if (cancel) {
        Event ev;
        ev.type = EventType::DragCancel;
        ev.target = drag.source;
        ev.pos = drag.pos;
        ev.player = drag.player;
        ev.dragSource = drag.source;
        ev.sourceItem = sourceItem;
        dispatch(arena, ev);
        if (!stillThere(arena, source)) {
            return;
        }
    }
    Event end;
    end.type = EventType::DragEnd;
    end.target = drag.source;
    end.pos = drag.pos;
    end.player = drag.player;
    end.dragSource = drag.source;
    end.sourceItem = sourceItem;
    end.dropped = dropped;
    dispatch(arena, end);
}

void EventDispatcher::cancelDrag(NodeArena& arena) { finishDrag(arena, /*drop=*/false, /*cancel=*/true); }

void EventDispatcher::handleExternalDrag(NodeArena& arena, NodeId root, const InputEvent& input) {
    if ((m_drag.active && !m_drag.external) || m_drag.pending) {
        return;  // The document's one drag is an internal one (or a press may become one).
    }
    if (input.kind == InputEvent::ExternalDragLeave) {
        // DragLeave on the target, the DropHover state cleared; no source, so no DragCancel /
        // DragEnd (08.14).
        finishDrag(arena, /*drop=*/false);
        return;
    }
    const NodeId target = dropTargetOf(arena, hitTestDetailed(arena, root, input.pos).node);
    if (!m_drag.active) {
        m_drag = DragState{};
        m_drag.active = true;
        m_drag.external = true;
        m_drag.player = m_curPlayer;
        m_drag.start = input.pos;
        m_autoScrollClock = m_time;
        if (input.kind == InputEvent::ExternalDrop) {
            m_drag.over = target;  // A drop with no hover before it: Drop alone.
        }
    }
    m_drag.pos = input.pos;
    if (input.kind != InputEvent::ExternalDrop) {
        updateDragOver(arena, target);  // DragOver on every move, DragLeave when it moves off.
        return;
    }
    if (target != m_drag.over) {
        updateDragOver(arena, target);  // Released somewhere else than the last move reported.
    }
    m_externalPaths.assign(input.paths.begin(), input.paths.end());
    finishDrag(arena, /*drop=*/true);  // Drop when over a target, else DragLeave / nothing.
    m_externalPaths.clear();
}

}  // namespace xt
