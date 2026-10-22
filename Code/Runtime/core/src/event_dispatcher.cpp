#include "event_dispatcher.h"

#include <algorithm>

#include "composition.h"
#include "text_edit.h"
#include "xt/ir.h"
#include "xt/property.h"

/// EventDispatcher: listener pool + bubbling dispatch (08.3), the pointer state machine (08.4),
/// the focus/keyboard model (08.5) and IME composition (08.8). See Docs/tasks/T-32.md,
/// Docs/tasks/T-33.md, Docs/tasks/T-33b.md. hitTest() lives in hit_test.cpp, the focus queries
/// in focus.cpp, the editing primitives in text_edit.cpp, the composition state in
/// composition.cpp.
///
/// Not covered by Docs/tasks/T-33.md (conservative choices made here):
///   - A KeyDown whose focus target died between events falls back to the root, exactly like the
///     no-focus case.
///   - Enter on a focused checkbox does nothing (only Space toggles); Space on a focused
///     `input[type=text]` is ordinary text input, delivered as a TextInput event by the host.
///   - The keyboard-activation Click carries the last known pointer position, since a key press
///     has no position of its own.
///   - Ctrl+A only selects all when the focus is an editable `input`; it is never a document-wide
///     selection (there is no document selection model in this version).
///
/// Not covered by Docs/tasks/T-33b.md (conservative choices made here):
///   - A CompositionUpdate/End with no composition in flight only dispatches its event: it
///     never starts a composition implicitly (the host derives Start itself, Code/Runtime/shell/src/app.cpp).
///   - While a composition is active *every* KeyDown default action is skipped, not just the
///     editing ones -- including Tab's focus move, which would otherwise strand a composition
///     on an element that no longer has the focus.
///   - Moving the focus while composing cancels the composition without touching the value,
///     exactly like a CompositionEnd with nothing committed.
///   - preventDefault on any of the three composition events sets suppressDefault (no
///     underline, no clause highlight) but never skips a state update, per 08.8's
///     preventDefault blocking the default *display* only; the caret is still drawn, since the
///     host has no way to draw one of its own.

namespace xt {

namespace {

/// Same rule as ElementRef::setState/document.cpp's markStyleDirtySubtree (05.8): a state-bit
/// change re-resolves `node`, every ancestor (automatic, via NodeArena::markDirty) and every
/// descendant (explicit, since a `:hover .child` descendant selector may now match differently).
/// Duplicated here (rather than shared) because document.cpp's copy is a local, unexported
/// anonymous-namespace helper -- see the T-32 report's "uncovered cases" for this tradeoff.
void markStyleDirtySubtree(NodeArena& arena, NodeId node) {
    arena.markDirty(node, static_cast<uint8_t>(kDirtyStyle));
    arena.traversePreorder(node, [&arena](NodeId n) {
        arena.markDirty(n, static_cast<uint8_t>(kDirtyStyle));
        return true;
    });
}

/// Sets or clears one ElementState bit directly (unlike ElementRef::setState, every bit is allowed
/// here -- hover/active are pointer-driven, see Docs/tasks/T-32.md "kStateHover 的設定/清除走
/// ElementRef::setState 的內部版本"). No-op if `node` isn't alive or isn't an Element, or the bit
/// is already at the requested value.
void setElementStateBit(NodeArena& arena, NodeId node, uint16_t bit, bool on) {
    if (!arena.alive(node) || arena.kind(node) != NodeKind::Element) {
        return;
    }
    ElementData& data = arena.element(node);
    bool currentlyOn = (data.stateBits & bit) != 0;
    if (currentlyOn == on) {
        return;
    }
    data.stateBits =
        on ? static_cast<uint16_t>(data.stateBits | bit) : static_cast<uint16_t>(data.stateBits & ~bit);
    markStyleDirtySubtree(arena, node);
}

/// T-390 (27.3.2, XF-068): kStateDisabled is subtree-wide for pointer and keyboard dispatch --
/// `node` or any element ancestor carrying it (like `fieldset[disabled]`).
bool isDisabled(const NodeArena& arena, NodeId node) {
    for (NodeId n = node; n != kInvalidNode && arena.alive(n); n = arena.parent(n)) {
        if (arena.kind(n) == NodeKind::Element &&
            (arena.element(n).stateBits & static_cast<uint16_t>(kStateDisabled)) != 0) {
            return true;
        }
    }
    return false;
}

/// `node`, its parent, its parent's parent, ... up to and including the root (parent ==
/// kInvalidNode). Empty when node == kInvalidNode.
void collectAncestorChain(const NodeArena& arena, NodeId node, std::vector<NodeId>& out) {
    out.clear();
    while (node != kInvalidNode) {
        out.push_back(node);
        node = arena.parent(node);
    }
}

/// The EditState behind `element`'s Text child, or nullptr when `element` is not an editable
/// `input` (or the pool is absent, i.e. a Document-less test).
EditState* editStateOf(const NodeArena& arena, std::vector<EditState>* pool, NodeId element) {
    if (pool == nullptr) {
        return nullptr;
    }
    NodeId textNode = firstTextChild(arena, element);
    if (textNode == kInvalidNode) {
        return nullptr;
    }
    uint32_t index = arena.text(textNode).editIndex;
    if (index >= pool->size()) {
        return nullptr;
    }
    return &(*pool)[index];
}

/// The shaping cache of `element`'s Text child, or an empty ShapeResult when it has none (the
/// text-edit primitives then fall back to UTF-8 boundaries and logical movement).
const ShapeResult& shapeOf(const NodeArena& arena, NodeId element) {
    static const ShapeResult kEmpty;
    NodeId textNode = firstTextChild(arena, element);
    if (textNode == kInvalidNode) {
        return kEmpty;
    }
    return arena.text(textNode).shaped;
}

/// Moves `state`'s caret to the grapheme boundary nearest `pos` (viewport coordinates) inside
/// `element`'s text, per Docs/design/07-text.md §7.7. No-op when the text node's world transform
/// is singular.
void placeCaretAt(const NodeArena& arena, NodeId element, EditState& state, Vec2 pos, bool extend) {
    NodeId textNode = firstTextChild(arena, element);
    if (textNode == kInvalidNode) {
        return;
    }
    Mat2x3 inv{};
    if (!invert(arena.common(textNode).world, inv)) {
        return;
    }
    uint32_t offset = hitTestTextOffset(arena, element, apply(inv, pos));
    setCaret(state, arena.text(textNode).shaped, offset, extend);
}

}  // namespace

void EventDispatcher::init(const IrView* ir, std::vector<EditState>* editStates,
                           CompositionState* composition) {
    m_ir = ir;
    m_editStates = editStates;
    m_composition = composition;
}

uint32_t EventDispatcher::addListener(NodeArena& arena, NodeId target, EventType type, EventListener fn) {
    ElementData& data = arena.element(target);
    Listener l;
    l.type = type;
    l.fn = std::move(fn);
    l.node = target;
    l.next = data.listenerHead;
    l.dead = 0;
    uint32_t id = static_cast<uint32_t>(m_listeners.size());
    m_listeners.push_back(std::move(l));
    data.listenerHead = id;
    if (type == EventType::OutsidePress) {
        watchOutsidePress(arena, target);  // T-387 (08.12).
    }
    return id;
}

void EventDispatcher::watchOutsidePress(const NodeArena& arena, NodeId node) {
    if (!arena.alive(node) || arena.kind(node) != NodeKind::Element) {
        return;
    }
    const uint32_t generation = arena.generation(node);
    // Drop the entries whose node died (or whose slot was reused) while looking for a duplicate.
    bool present = false;
    m_outsidePress.erase(std::remove_if(m_outsidePress.begin(), m_outsidePress.end(),
                                        [&](const PathEntry& e) {
                                            if (!stillThere(arena, e)) {
                                                return true;
                                            }
                                            present =
                                                present || (e.node == node && e.generation == generation);
                                            return false;
                                        }),
                         m_outsidePress.end());
    if (!present) {
        m_outsidePress.push_back(PathEntry{.node = node, .generation = generation});
    }
}

void EventDispatcher::removeListener(NodeArena& arena, uint32_t id) {
    if (id >= m_listeners.size() || m_listeners[id].dead) {
        return;
    }
    m_listeners[id].dead = 1;
    ++m_deadCount;
    if (m_deadCount >= 1024) {
        compact(arena);
        m_deadCount = 0;
    }
}

void EventDispatcher::removeListenersFor(NodeArena& arena, NodeId node) {
    if (!arena.alive(node) || arena.kind(node) != NodeKind::Element) {
        return;
    }
    // T-260a: m_callScratch is a stack; a callListeners() further out may still be reading its part.
    const size_t start = m_callScratch.size();
    for (uint32_t id = arena.element(node).listenerHead; id != kInvalidListener;) {
        if (id >= m_listeners.size()) {
            break;
        }
        m_callScratch.push_back(id);
        id = m_listeners[id].next;
    }
    for (size_t i = start; i < m_callScratch.size(); ++i) {
        removeListener(arena, m_callScratch[i]);
    }
    m_callScratch.resize(start);
    if (arena.alive(node) && arena.kind(node) == NodeKind::Element) {
        arena.element(node).listenerHead = kInvalidListener;
    }
}

void EventDispatcher::clearForSubtreeRemoval(NodeArena& arena, NodeId root) {
    auto inSubtree = [&arena, root](NodeId n) {
        return n != kInvalidNode && arena.alive(n) && (n == root || arena.isAncestorOf(root, n));
    };
    for (uint8_t p = 0; p < kMaxPlayers; ++p) {
        if (inSubtree(m_focus[p])) {
            setFocus(arena, kInvalidNode, p);  // Dispatches Blur and clears kStateFocus.
        }
        PadState& pad = m_pads[p];
        if (inSubtree(pad.aNode)) {
            pad.aNode = kInvalidNode;
        }
        pad.aChain.erase(std::remove_if(pad.aChain.begin(), pad.aChain.end(), inSubtree), pad.aChain.end());
    }
    // T-194: a drag whose source or target is going away ends without a drop.
    if (m_drag.active && (inSubtree(m_drag.source) || inSubtree(m_drag.over))) {
        if (inSubtree(m_drag.over)) {
            m_drag.over = kInvalidNode;
        }
        if (inSubtree(m_drag.source)) {
            m_drag.source = kInvalidNode;
        }
        cancelDrag(arena);
    }
    if (m_drag.pending && inSubtree(m_drag.source)) {
        m_drag = DragState{};
    }
    if (inSubtree(m_dragEdit)) {
        m_dragEdit = kInvalidNode;
    }
    if (inSubtree(m_hoverNode)) {
        // T-260a: the chain is recorded first (m_pathStack); a node a Leave listener destroyed is
        // skipped.
        const size_t start = m_pathStack.size();
        pushPath(arena, m_hoverNode);
        const size_t end = m_pathStack.size();
        for (size_t i = start; i < end; ++i) {
            const PathEntry entry = m_pathStack[i];
            if (!stillThere(arena, entry)) {
                continue;
            }
            const NodeId n = entry.node;
            if (!inSubtree(n)) {
                break;  // The hover path continues above the removed subtree; those stay hovered.
            }
            setElementStateBit(arena, n, static_cast<uint16_t>(kStateHover), false);
            Event ev;
            ev.type = EventType::PointerLeave;
            ev.target = n;
            ev.pos = m_pointerPos;
            ev.localPos = m_pointerLocalPos;
            dispatchAt(arena, ev, n);
        }
        m_pathStack.resize(start);
        m_hoverNode = kInvalidNode;
    }
    for (NodeId n : m_activeChain) {
        if (inSubtree(n)) {
            setElementStateBit(arena, n, static_cast<uint16_t>(kStateActive), false);
        }
    }
    if (inSubtree(m_activeNode)) {
        m_activeNode = kInvalidNode;
        m_activeChain.clear();
    }
}

void EventDispatcher::compact(NodeArena& arena) {
    std::vector<uint8_t> visited(m_listeners.size(), 0);
    std::vector<uint32_t> chain;
    for (uint32_t i = 0; i < m_listeners.size(); ++i) {
        if (visited[i]) {
            continue;
        }
        NodeId node = m_listeners[i].node;
        if (!arena.alive(node) || arena.kind(node) != NodeKind::Element) {
            // Orphaned (the element was destroyed after registering listeners): its listenerHead
            // was already reset by NodeArena::destroy(), so there is nothing to relink -- just
            // mark every entry sharing this node visited so later iterations skip them.
            for (uint32_t j = i; j < m_listeners.size(); ++j) {
                if (m_listeners[j].node == node) {
                    visited[j] = 1;
                }
            }
            continue;
        }
        chain.clear();
        uint32_t cur = arena.element(node).listenerHead;
        while (cur != kInvalidListener) {
            visited[cur] = 1;
            if (!m_listeners[cur].dead) {
                chain.push_back(cur);
            }
            cur = m_listeners[cur].next;
        }
        if (chain.empty()) {
            arena.element(node).listenerHead = kInvalidListener;
            continue;
        }
        for (size_t k = 0; k + 1 < chain.size(); ++k) {
            m_listeners[chain[k]].next = chain[k + 1];
        }
        m_listeners[chain.back()].next = kInvalidListener;
        arena.element(node).listenerHead = chain.front();
    }
}

void EventDispatcher::callListeners(const NodeArena& arena, NodeId node, Event& event) {
    if (!arena.alive(node) || arena.kind(node) != NodeKind::Element) {
        return;
    }
    // T-260a: m_callScratch is a stack (see its declaration): this call owns [start, end).
    const size_t start = m_callScratch.size();
    uint32_t cur = arena.element(node).listenerHead;
    while (cur != kInvalidListener) {
        const Listener& l = m_listeners[cur];
        if (!l.dead && l.type == event.type) {
            m_callScratch.push_back(cur);
        }
        cur = l.next;
    }
    const size_t end = m_callScratch.size();
    // m_callScratch is newest-first (the listenerHead chain's order); call oldest-first
    // (registration order, per 08.3).
    for (size_t k = end; k > start; --k) {
        uint32_t idx = m_callScratch[k - 1];
        if (m_listeners[idx].dead) {
            continue;  // Removed by an earlier listener in this same loop.
        }
        if (m_listenersMuted) {
            break;  // T-260a: an earlier listener unloaded this Document (muteListeners()).
        }
        // Copy the std::function out before invoking it: a callback may call addListener(), which
        // can reallocate m_listeners, so no reference into it may be held across the call (08.3
        // "用索引迭代，不用迭代器").
        EventListener fn = m_listeners[idx].fn;
        if (fn) {
            fn(event);
        }
    }
    m_callScratch.resize(start);
    if (m_triggerSink != nullptr && arena.alive(node) && arena.common(node).triggerTable != 0) {
        m_triggerSink->onDomEvent(event, node);  // T-174: after the node's own listeners.
    }
}

void EventDispatcher::dispatchAt(const NodeArena& arena, Event& event, NodeId node) {
    if (event.player == 0) {
        event.player = m_curPlayer;  // T-194: the player of the input being handled.
    }
    if (event.modifiers == 0) {
        event.modifiers = m_curModifiers;  // T-387 (08.12): the pointer input's modifier keys.
    }
    event.current = node;
    callListeners(arena, node, event);
}

void EventDispatcher::dispatch(const NodeArena& arena, Event& event) {
    if (event.player == 0) {
        event.player = m_curPlayer;  // T-194: the player of the input being handled.
    }
    if (event.modifiers == 0) {
        event.modifiers = m_curModifiers;  // T-387 (08.12): the pointer input's modifier keys.
    }
    // T-260a: the propagation path (target to root) is fixed before any listener runs, as in the
    // DOM. A node a listener destroyed meanwhile is skipped (its listeners do not run) and the
    // event goes on to the next node of the recorded path. Read by index: a nested dispatch
    // appends to m_pathStack (and may reallocate it) but shrinks it back before returning.
    const size_t start = m_pathStack.size();
    pushPath(arena, event.target);
    const size_t end = m_pathStack.size();
    for (size_t i = start; i < end; ++i) {
        const PathEntry entry = m_pathStack[i];
        if (!stillThere(arena, entry)) {
            continue;
        }
        event.current = entry.node;
        callListeners(arena, entry.node, event);
        if (event.stopPropagation) {
            break;
        }
    }
    m_pathStack.resize(start);
}

void EventDispatcher::pushPath(const NodeArena& arena, NodeId node) {
    for (NodeId n = node; n != kInvalidNode && arena.alive(n); n = arena.parent(n)) {
        m_pathStack.push_back(PathEntry{.node = n, .generation = arena.generation(n)});
    }
}

bool EventDispatcher::stillThere(const NodeArena& arena, const PathEntry& entry) {
    return arena.alive(entry.node) && arena.generation(entry.node) == entry.generation;
}

void EventDispatcher::setPointerMove(NodeArena& arena, NodeId root, const InputEvent& input) {
    m_pointerPos = input.pos;
    if (m_axisDrag != kInvalidNode) {
        axisDragValue(arena, input.pos);  // T-201.
    }
    HitResult hit = hitTestDetailed(arena, root, input.pos);
    NodeId h = hit.node;
    m_pointerLocalPos = hit.localPos;
    if (h != m_hoverNode) {
        // T-260a: both chains are recorded before any listener runs: the old one in [oldStart,
        // newStart), the new one in [newStart, end) of m_pathStack. A node a listener destroyed
        // meanwhile is skipped.
        const size_t oldStart = m_pathStack.size();
        pushPath(arena, m_hoverNode);
        const size_t newStart = m_pathStack.size();
        pushPath(arena, h);
        const size_t end = m_pathStack.size();

        NodeId lca = kInvalidNode;
        for (size_t i = oldStart; i < newStart && lca == kInvalidNode; ++i) {
            for (size_t j = newStart; j < end; ++j) {
                if (m_pathStack[j].node == m_pathStack[i].node) {
                    lca = m_pathStack[i].node;
                    break;
                }
            }
        }

        for (size_t i = oldStart; i < newStart; ++i) {
            const PathEntry entry = m_pathStack[i];
            if (entry.node == lca) {
                break;
            }
            if (!stillThere(arena, entry)) {
                continue;
            }
            setElementStateBit(arena, entry.node, static_cast<uint16_t>(kStateHover), false);
            Event ev;
            ev.type = EventType::PointerLeave;
            ev.target = entry.node;
            ev.pos = input.pos;
            ev.localPos = m_pointerLocalPos;
            dispatchAt(arena, ev, entry.node);
        }
        for (size_t i = newStart; i < end; ++i) {
            const PathEntry entry = m_pathStack[i];
            if (entry.node == lca) {
                break;
            }
            if (!stillThere(arena, entry)) {
                continue;
            }
            setElementStateBit(arena, entry.node, static_cast<uint16_t>(kStateHover), true);
            Event ev;
            ev.type = EventType::PointerEnter;
            ev.target = entry.node;
            ev.pos = input.pos;
            ev.localPos = m_pointerLocalPos;
            dispatchAt(arena, ev, entry.node);
        }
        m_pathStack.resize(oldStart);
        m_hoverNode = h;
    }

    // Docs/tasks/T-33.md "拖曳（active 時 PointerMove）擴選".
    if (m_dragEdit != kInvalidNode && m_activeNode == m_dragEdit) {
        EditState* state = editStateOf(arena, m_editStates, m_dragEdit);
        if (state != nullptr) {
            placeCaretAt(arena, m_dragEdit, *state, input.pos, /*extend=*/true);
            arena.markDirty(m_dragEdit, static_cast<uint8_t>(kDirtyPaint));
        }
    }

    NodeId moveTarget = m_activeNode != kInvalidNode ? m_activeNode : h;
    if (!isDisabled(arena, moveTarget)) {  // T-390: a disabled subtree gets no PointerMove.
        Event ev;
        ev.type = EventType::PointerMove;
        ev.target = moveTarget;
        ev.pos = input.pos;
        ev.localPos = m_pointerLocalPos;
        if (m_activeNode != kInvalidNode) {
            ev.button = m_pressButton;  // T-390: the button held.
        }
        dispatch(arena, ev);
    }

    // T-194 (27.7): a press on a draggable becomes a drag past its `drag-threshold` (T-388,
    // default 4 px); the drag then tracks the drop target under the pointer.
    if (m_drag.pending && m_activeNode != kInvalidNode) {
        const Vec2 d = input.pos - m_drag.start;
        const float threshold = dragThreshold(arena, m_drag.source);
        if (d.x * d.x + d.y * d.y > threshold * threshold) {
            startDrag(arena, m_drag.source, m_drag.player, /*gamepad=*/false, m_drag.start);
        }
    }
    if (m_drag.active && !m_drag.gamepad && !m_drag.external) {
        m_drag.pos = input.pos;
        updateDragOver(arena, pointerDropTarget(arena, h));  // T-394: the source row counts.
    }

    // T-394 (08.15): `on:pointerdrag` -- past the element's `drag-threshold` (default 4 px) the
    // press is a pointerdrag; every move then sends PointerDrag (not bubbling) to the element.
    if (m_ptrDrag.node != kInvalidNode && m_activeNode != kInvalidNode) {
        const PathEntry entry{.node = m_ptrDrag.node, .generation = m_ptrDrag.generation};
        if (!stillThere(arena, entry)) {
            m_ptrDrag = PointerDragState{};
            return;
        }
        if (!m_ptrDrag.dragging) {
            const Vec2 d = input.pos - m_ptrDrag.start;
            const float threshold = dragThreshold(arena, m_ptrDrag.node);
            m_ptrDrag.dragging = d.x * d.x + d.y * d.y >= threshold * threshold;
        }
        if (m_ptrDrag.dragging) {
            Event ev;
            ev.type = EventType::PointerDrag;
            ev.target = m_ptrDrag.node;
            ev.pos = input.pos;
            ev.button = m_pressButton;
            Mat2x3 inv{};
            if (invert(arena.common(m_ptrDrag.node).world, inv)) {
                ev.localPos = apply(inv, input.pos);
            }
            dispatchAt(arena, ev, m_ptrDrag.node);
        }
    }
}

void EventDispatcher::setPointerLeaveWindow(NodeArena& arena) {
    if (m_hoverNode == kInvalidNode) {
        return;
    }
    // T-260a: recorded first, destroyed nodes skipped (see setPointerMove).
    const size_t start = m_pathStack.size();
    pushPath(arena, m_hoverNode);
    const size_t end = m_pathStack.size();
    for (size_t i = start; i < end; ++i) {
        const PathEntry entry = m_pathStack[i];
        if (!stillThere(arena, entry)) {
            continue;
        }
        setElementStateBit(arena, entry.node, static_cast<uint16_t>(kStateHover), false);
        Event ev;
        ev.type = EventType::PointerLeave;
        ev.target = entry.node;
        ev.localPos = m_pointerLocalPos;
        dispatchAt(arena, ev, entry.node);
    }
    m_pathStack.resize(start);
    m_hoverNode = kInvalidNode;
}

void EventDispatcher::handlePointerDown(NodeArena& arena, NodeId root, const InputEvent& input) {
    // T-388 (08.12): a press while a pointer drag holds the first button is a second button: it
    // cancels the drag and is consumed (no PointerDown, the first press stays the active one).
    if (m_drag.active && !m_drag.gamepad && !m_drag.external) {
        cancelDrag(arena);
        return;
    }
    m_pressDragged = false;
    m_consumedPress = false;
    m_ptrDrag = PointerDragState{};
    m_pressTime = m_time;  // T-379: double-click detection compares presses.
    m_pressPos = input.pos;
    m_pressClicks = input.clicks;
    m_pressButton = input.button;  // T-390.
    HitResult hit = hitTestDetailed(arena, root, input.pos, HitMode::Press);
    NodeId h = hit.node;

    // T-387 (08.12): light-dismiss. T-394 (08.15): before the PointerDown, so an OutsidePress
    // `consume` takes the press: no PointerDown, no active element, no default actions, and its
    // PointerUp / Click are suppressed. An element made visible by this press's own PointerDown
    // listeners therefore never receives this OutsidePress.
    {
        const PathEntry hitEntry{.node = h, .generation = arena.alive(h) ? arena.generation(h) : 0u};
        if (dispatchOutsidePress(arena, root, h, input)) {
            m_consumedPress = true;
            return;
        }
        if (h != kInvalidNode && !stillThere(arena, hitEntry)) {
            h = kInvalidNode;  // An OutsidePress listener destroyed it.
            hit.localPos = Vec2{};
        }
    }
    m_activeNode = h;
    if (!isDisabled(arena, h)) {
        collectAncestorChain(arena, h, m_activeChain);
        for (NodeId n : m_activeChain) {
            setElementStateBit(arena, n, static_cast<uint16_t>(kStateActive), true);
        }
    } else {
        m_activeChain.clear();
    }
    Event ev;
    ev.type = EventType::PointerDown;
    ev.target = h;
    ev.pos = input.pos;
    ev.localPos = hit.localPos;
    ev.button = input.button;
    ev.modifiers = input.modifiers;  // T-387 (08.12).
    const PathEntry pressed{.node = h, .generation = arena.alive(h) ? arena.generation(h) : 0u};
    if (!isDisabled(arena, h)) {  // T-390: a disabled subtree gets no PointerDown.
        dispatch(arena, ev);
    }

    // 08.4: the hit element or its nearest focusable ancestor takes the focus; nothing focusable
    // above it clears the focus entirely. preventDefault on PointerDown suppresses both.
    if (ev.preventDefault) {
        return;
    }
    if (h != kInvalidNode && !stillThere(arena, pressed)) {
        h = kInvalidNode;  // T-260a: a listener destroyed it; its ancestor walks below see nothing.
    }
    // T-201 (27.7): a press on (or inside) a `draggable="axis-x"` element drags along it.
    for (NodeId n = h; n != kInvalidNode; n = arena.parent(n)) {
        if (arena.kind(n) == NodeKind::Element && (arena.common(n).flags2 & kDragAxisX) != 0) {
            if (!isDisabled(arena, n)) {
                m_axisDrag = n;
                axisDragValue(arena, input.pos);
            }
            break;
        }
    }
    // T-194 (27.7): remember a press on a draggable; setPointerMove turns it into a drag.
    // T-394 (08.15): only the primary button starts one, unless `draggable="any-button"`.
    for (NodeId n = h; n != kInvalidNode; n = arena.parent(n)) {
        if (arena.kind(n) == NodeKind::Element && (arena.common(n).flags2 & kDraggable) != 0) {
            const bool buttonOk =
                input.button == PointerButton::Left || attributeOf(arena, n, "draggable") == "any-button";
            if (buttonOk && !isDisabled(arena, n) && !m_drag.active) {
                m_drag = DragState{};
                m_drag.pending = true;
                m_drag.source = n;
                m_drag.start = input.pos;
                m_drag.pos = input.pos;
                m_drag.player = m_curPlayer;
            }
            break;
        }
    }
    // T-394 (08.15): a primary press on (or inside) an element with `on:pointerdrag` or a
    // PointerDrag listener may become a pointerdrag; a press that may become a drag never does.
    if (input.button == PointerButton::Left && !m_drag.pending && !m_drag.active) {
        for (NodeId n = h; n != kInvalidNode; n = arena.parent(n)) {
            if (arena.kind(n) != NodeKind::Element) {
                continue;
            }
            const bool wants = hasListener(arena, n, EventType::PointerDrag) ||
                               (m_triggerSink != nullptr && arena.common(n).triggerTable != 0 &&
                                m_triggerSink->hasPointerDrag(n));
            if (wants) {
                if (!isDisabled(arena, n)) {
                    m_ptrDrag.node = n;
                    m_ptrDrag.generation = arena.generation(n);
                    m_ptrDrag.start = input.pos;
                }
                break;
            }
        }
    }
    NodeId target = nearestFocusable(arena, h);
    setFocus(arena, target, m_curPlayer);
    if (target == kInvalidNode || m_ir == nullptr ||
        widgetKind(arena, *m_ir, m_doc, target) != WidgetKind::TextInput) {
        return;
    }
    EditState* state = editStateOf(arena, m_editStates, target);
    if (state == nullptr) {
        return;
    }
    placeCaretAt(arena, target, *state, input.pos, (input.modifiers & kShift) != 0);
    arena.markDirty(target, static_cast<uint8_t>(kDirtyPaint));
    m_dragEdit = target;
}

void EventDispatcher::handlePointerUp(NodeArena& arena, NodeId root, const InputEvent& input) {
    m_axisDrag = kInvalidNode;  // T-201.
    NodeId active = m_activeNode;
    HitResult upHitResult = hitTestDetailed(arena, root, input.pos, HitMode::Press);
    Event upEv;
    upEv.type = EventType::PointerUp;
    upEv.target = active;
    upEv.pos = input.pos;
    upEv.localPos = upHitResult.localPos;
    upEv.button = input.button;
    upEv.modifiers = input.modifiers;  // T-387 (08.12).
    const PathEntry activeEntry{.node = active,
                                .generation = arena.alive(active) ? arena.generation(active) : 0u};
    const PathEntry upEntry{
        .node = upHitResult.node,
        .generation = arena.alive(upHitResult.node) ? arena.generation(upHitResult.node) : 0u};
    if (!isDisabled(arena, active)) {  // T-390: a disabled subtree gets no PointerUp.
        dispatch(arena, upEv);
    }

    // T-194 (27.7): releasing a pointer drag drops it (or cancels it off a target); no Click.
    // T-388: also a press whose drag was cancelled before the release: no Click either.
    const bool dragging = m_drag.active && !m_drag.gamepad && !m_drag.external;
    const bool dragged = dragging || m_pressDragged;
    m_pressDragged = false;
    if (dragging) {
        finishDrag(arena, m_drag.over != kInvalidNode);
    }
    if (m_drag.pending) {
        m_drag = DragState{};
    }
    if (active != kInvalidNode && dragged) {
        for (NodeId n : m_activeChain) {
            setElementStateBit(arena, n, static_cast<uint16_t>(kStateActive), false);
        }
    } else if (active != kInvalidNode) {
        NodeId upHit = upHitResult.node;
        // No node under the release point (outside the document's root, e.g. the letterbox around
        // a fixed-size viewport): isAncestorOf() asserts on kInvalidNode -- 2026-09-22's "press a HUD
        // button, release outside" abort (Docs/tasks/T-87.md).
        // T-260a: a PointerUp listener may have destroyed either one; then there is no Click.
        bool sameOrDescendant = upHit != kInvalidNode && stillThere(arena, activeEntry) &&
                                stillThere(arena, upEntry) &&
                                (upHit == active || arena.isAncestorOf(active, upHit));
        if (sameOrDescendant && !isDisabled(arena, active)) {
            const uint32_t generation = arena.generation(active);
            const uint8_t clicks = classifyClick(arena, active, generation, input);  // T-383.
            m_clickButton = input.button;  // T-390: the Click carries the released button.
            activate(arena, active, input.pos, upHitResult.localPos, clicks);
            m_clickButton = PointerButton::Left;
            detectDoubleClick(arena, active, generation, input, upHitResult.localPos, clicks);  // T-379.
        }
        for (NodeId n : m_activeChain) {
            setElementStateBit(arena, n, static_cast<uint16_t>(kStateActive), false);
        }
    }
    m_activeNode = kInvalidNode;
    m_activeChain.clear();
    m_dragEdit = kInvalidNode;
    m_ptrDrag = PointerDragState{};  // T-394: the release ends a pointerdrag.
}

void EventDispatcher::handlePointerCancel(NodeArena& arena) {
    // T-386 (08.11): the press is taken away -- PointerCancel to the active element (bubbling),
    // the active state cleared, no PointerUp / Click. Hover is untouched. Nothing pressed: no-op.
    const NodeId active = m_activeNode;
    if (active == kInvalidNode) {
        return;
    }
    m_axisDrag = kInvalidNode;
    m_pressDragged = false;
    m_ptrDrag = PointerDragState{};  // T-394: PointerCancel ends a pointerdrag.
    if (m_drag.active && !m_drag.gamepad && !m_drag.external) {
        cancelDrag(arena);  // T-388: DragCancel + DragEnd.
    }
    m_swallowButtons = false;  // T-394: the press is over; nothing more to swallow.
    m_heldButtons = 0;
    if (m_drag.pending) {
        m_drag = DragState{};
    }
    // Cleared before the dispatch, so a listener sees no active state and a re-entrant press
    // starts a fresh chain.
    const std::vector<NodeId> chain = std::move(m_activeChain);
    m_activeChain.clear();
    m_activeNode = kInvalidNode;
    m_dragEdit = kInvalidNode;
    for (NodeId n : chain) {
        if (arena.alive(n)) {
            setElementStateBit(arena, n, static_cast<uint16_t>(kStateActive), false);
        }
    }
    if (!arena.alive(active)) {
        return;
    }
    Event ev;
    ev.type = EventType::PointerCancel;
    ev.target = active;
    ev.pos = m_lastPointer;
    ev.localPos = m_pointerLocalPos;
    dispatch(arena, ev);
}

void EventDispatcher::handleWheel(NodeArena& arena, NodeId root, const InputEvent& input) {
    HitResult hit = hitTestDetailed(arena, root, input.pos, HitMode::Press);
    NodeId h = hit.node;
    Event ev;
    ev.type = EventType::Wheel;
    ev.target = h;
    ev.pos = input.pos;
    ev.localPos = hit.localPos;
    ev.wheelDelta = input.wheelDelta;
    ev.modifiers = input.modifiers;  // T-387 (08.12).
    const PathEntry hitEntry{.node = h, .generation = arena.alive(h) ? arena.generation(h) : 0u};
    dispatch(arena, ev);
    if (ev.preventDefault) {
        return;
    }
    // T-260a: a target a listener destroyed scrolls nothing (its ancestors cannot be reached).
    NodeId n = stillThere(arena, hitEntry) ? h : kInvalidNode;
    while (n != kInvalidNode) {
        if (arena.kind(n) == NodeKind::Element) {
            ElementData& data = arena.element(n);
            // T-336 (05 §5.9.8, 08 §8.4): per axis -- only a live scroll axis (`scroll`, or `auto`
            // whose content overflows) takes the wheel, each clamped on its own; an element with
            // neither (e.g. `auto` that fits) lets the wheel pass to its ancestors.
            const float maxX = maxScrollX(data.box);
            const float maxY = maxScrollY(data.box);
            const bool liveX = scrollsOnAxis(usedOverflowX(data.style), maxX);
            const bool liveY = scrollsOnAxis(usedOverflowY(data.style), maxY);
            if (liveX || liveY) {
                // T-387 (08.12): through setScroll(), which clamps and dispatches Scroll.
                const float curX = data.style.base[prop::ScrollX].f;
                const float curY = data.style.base[prop::ScrollY].f;
                const float newX = liveX ? curX + input.wheelDelta.x * 40.0f : curX;
                const float newY = liveY ? curY + input.wheelDelta.y * 40.0f : curY;
                setScroll(arena, n, newX, newY, /*notify=*/true);
                break;
            }
        }
        n = arena.parent(n);
    }
}

void EventDispatcher::setFocus(NodeArena& arena, NodeId node, uint8_t player) {
    if (player >= kMaxPlayers) {
        return;
    }
    m_pendingFocus[player] = kInvalidNode;  // T-399: an explicit focus change wins.
    NodeId target = node;
    if (target != kInvalidNode && (!arena.alive(target) || arena.kind(target) != NodeKind::Element)) {
        target = kInvalidNode;
    }
    if (target == m_focus[player]) {
        return;
    }
    if (player == 0 && m_composition != nullptr && m_composition->active &&
        m_composition->element != target) {
        cancelComposition(arena);
    }
    NodeId previous = m_focus[player];
    m_focus[player] = target;
    const uint8_t bit = static_cast<uint8_t>(1u << player);
    if (previous != kInvalidNode && arena.alive(previous) && arena.kind(previous) == NodeKind::Element) {
        ElementData& data = arena.element(previous);
        data.focusPlayers = static_cast<uint8_t>(data.focusPlayers & ~bit);
        // T-194: `:focus` stays while another player still focuses the element.
        setElementStateBit(arena, previous, static_cast<uint16_t>(kStateFocus), data.focusPlayers != 0);
        markStyleDirtySubtree(arena, previous);  // `:focus-player(n)`.
        Event ev;
        ev.type = EventType::Blur;
        ev.target = previous;
        ev.player = player;
        const uint32_t generation = arena.generation(previous);
        dispatchAt(arena, ev, previous);
        // T-397 (08.17): the bubbling twin, unless a Blur listener removed the element.
        if (arena.alive(previous) && arena.generation(previous) == generation) {
            Event out;
            out.type = EventType::FocusOut;
            out.target = previous;
            out.player = player;
            dispatch(arena, out);
        }
    }
    if (target != kInvalidNode && arena.alive(target)) {
        ElementData& data = arena.element(target);
        data.focusPlayers = static_cast<uint8_t>(data.focusPlayers | bit);
        setElementStateBit(arena, target, static_cast<uint16_t>(kStateFocus), true);
        markStyleDirtySubtree(arena, target);
        Event ev;
        ev.type = EventType::Focus;
        ev.target = target;
        ev.player = player;
        const uint32_t generation = arena.generation(target);
        dispatchAt(arena, ev, target);
        // T-397 (08.17): the bubbling twin, unless a Focus listener removed the element.
        if (arena.alive(target) && arena.generation(target) == generation) {
            Event in;
            in.type = EventType::FocusIn;
            in.target = target;
            in.player = player;
            dispatch(arena, in);
        }
    }
    if (m_triggerSink != nullptr) {
        m_triggerSink->onFocusMoved(m_focus[player], player);  // T-194: aria-label announce.
    }
    // T-194 (27.7): a gamepad drag follows its player's focus.
    if (m_drag.active && m_drag.gamepad && m_drag.player == player) {
        updateDragOver(arena, dropTargetOf(arena, m_focus[player]));
    }
}

bool EventDispatcher::validateFocus(NodeArena& arena) {
    bool changed = false;
    for (uint8_t p = 0; p < kMaxPlayers; ++p) {
        NodeId& focus = m_focus[p];
        if (focus == kInvalidNode) {
            continue;
        }
        if (!arena.alive(focus) || arena.kind(focus) != NodeKind::Element) {
            // The element is gone: its listeners (and so any Blur handler) went with it, so there
            // is nothing left to dispatch to -- just drop the focus.
            focus = kInvalidNode;
            changed = true;
            continue;
        }
        if (!isFocusable(arena, focus)) {
            setFocus(arena, kInvalidNode, p);  // Still alive (display:none / disabled): it gets its Blur.
            changed = true;
        }
    }
    return changed;
}

void EventDispatcher::setPendingFocus(NodeId node, uint32_t generation, uint8_t player) {
    if (player < kMaxPlayers) {
        m_pendingFocus[player] = node;
        m_pendingFocusGen[player] = generation;
    }
}

bool EventDispatcher::applyPendingFocus(NodeArena& arena, bool& dropped) {
    bool changed = false;
    for (uint8_t p = 0; p < kMaxPlayers; ++p) {
        const NodeId node = m_pendingFocus[p];
        if (node == kInvalidNode) {
            continue;
        }
        m_pendingFocus[p] = kInvalidNode;
        if (arena.alive(node) && arena.generation(node) == m_pendingFocusGen[p] && isFocusable(arena, node)) {
            if (m_focus[p] != node) {
                setFocus(arena, node, p);
                changed = true;
            }
        } else {
            dropped = true;
        }
    }
    return changed;
}

bool EventDispatcher::wantsTextInput(const NodeArena& arena) const {
    const NodeId focus = m_focus[0];
    if (focus == kInvalidNode || m_ir == nullptr) {
        return false;
    }
    return widgetKind(arena, *m_ir, m_doc, focus) == WidgetKind::TextInput && !isDisabled(arena, focus);
}

EditState* EventDispatcher::focusedEdit(NodeArena& arena) {
    // Text editing belongs to player 0 (the keyboard); 08.5/08.8 have one caret.
    const NodeId focus = m_focus[0];
    if (m_curPlayer != 0 || focus == kInvalidNode || m_ir == nullptr || isDisabled(arena, focus)) {
        return nullptr;
    }
    if (widgetKind(arena, *m_ir, m_doc, focus) != WidgetKind::TextInput) {
        return nullptr;
    }
    return editStateOf(arena, m_editStates, focus);
}

void EventDispatcher::activate(NodeArena& arena, NodeId node, Vec2 pos, Vec2 localPos, uint8_t clicks) {
    Event clickEv;
    clickEv.type = EventType::Click;
    clickEv.target = node;
    clickEv.pos = pos;
    clickEv.localPos = localPos;
    clickEv.clicks = clicks;  // T-383 (08.10).
    clickEv.button = m_clickButton;
    dispatch(arena, clickEv);

    // 08.4: a non-disabled checkbox toggles kStateChecked and fires Change after its Click.
    // T-201: a click on an element laid out inside the checkbox (a knob, a mark) toggles it too.
    if (m_ir == nullptr) {
        return;
    }
    while (node != kInvalidNode && arena.kind(node) == NodeKind::Element &&
           widgetKind(arena, *m_ir, m_doc, node) != WidgetKind::Checkbox) {
        node = arena.parent(node);
    }
    if (node == kInvalidNode || arena.kind(node) != NodeKind::Element || isDisabled(arena, node)) {
        return;
    }
    bool checked = (arena.element(node).stateBits & static_cast<uint16_t>(kStateChecked)) != 0;
    setElementStateBit(arena, node, static_cast<uint16_t>(kStateChecked), !checked);
    if (m_triggerSink != nullptr) {
        m_triggerSink->onCheckedChanged(node, !checked);  // T-174: check / uncheck triggers.
    }
    Event changeEv;
    changeEv.type = EventType::Change;
    changeEv.target = node;
    dispatch(arena, changeEv);
}

uint8_t EventDispatcher::classifyClick(NodeArena& arena, NodeId clicked, uint32_t generation,
                                       const InputEvent& input) {
    constexpr float kDoubleClickTime = 0.4f;  // 08.10: seconds between the two presses.
    constexpr float kDoubleClickSlop = 4.0f;  // 08.10: logical px between the two presses.
    const uint8_t hostClicks = input.clicks != 0 ? input.clicks : m_pressClicks;
    bool isDouble = false;
    if (hostClicks != 0) {
        isDouble = hostClicks >= 2;  // The host counts; trust it.
    } else if (m_lastClick.valid) {
        const float dx = m_pressPos.x - m_lastClick.pos.x;
        const float dy = m_pressPos.y - m_lastClick.pos.y;
        isDouble = m_lastClick.node.node == clicked && stillThere(arena, m_lastClick.node) &&
                   m_lastClick.node.generation == generation && m_lastClick.button == input.button &&
                   m_pressTime - m_lastClick.time <= kDoubleClickTime &&
                   dx * dx + dy * dy <= kDoubleClickSlop * kDoubleClickSlop;
    }
    if (isDouble) {
        m_lastClick.valid = false;  // A third click starts a new pair.
    } else {
        m_lastClick.valid = true;
        m_lastClick.node = PathEntry{.node = clicked, .generation = generation};
        m_lastClick.button = input.button;
        m_lastClick.time = m_pressTime;
        m_lastClick.pos = m_pressPos;
    }
    if (hostClicks != 0) {
        return hostClicks;
    }
    return isDouble ? uint8_t{2} : uint8_t{1};
}

void EventDispatcher::detectDoubleClick(NodeArena& arena, NodeId clicked, uint32_t generation,
                                        const InputEvent& input, Vec2 localPos, uint8_t clicks) {
    // A Click listener may have destroyed the target: then there is no DoubleClick.
    if (clicks < 2 || !arena.alive(clicked) || arena.generation(clicked) != generation) {
        return;
    }
    Event ev;
    ev.type = EventType::DoubleClick;
    ev.target = clicked;
    ev.pos = input.pos;
    ev.localPos = localPos;
    ev.button = input.button;
    ev.clicks = clicks;  // T-383 (08.10).
    dispatch(arena, ev);
}

namespace {

/// Drops `element`'s Text node shaping cache and marks it (and the element) kDirtyLayout. The
/// cache key covers only IR-derived fields, so every change to the *displayed* string -- the
/// runtime value or the composition -- has to invalidate it explicitly.
void invalidateDisplayString(NodeArena& arena, NodeId element) {
    NodeId textNode = firstTextChild(arena, element);
    if (textNode != kInvalidNode) {
        arena.text(textNode).shapedValid = 0;
        arena.markDirty(textNode, static_cast<uint8_t>(kDirtyLayout));
    }
    arena.markDirty(element, static_cast<uint8_t>(kDirtyLayout));
}

}  // namespace

void EventDispatcher::afterEdit(NodeArena& arena, NodeId element, bool valueChanged) {
    if (!valueChanged) {
        arena.markDirty(element, static_cast<uint8_t>(kDirtyPaint));
        return;
    }
    invalidateDisplayString(arena, element);
    Event ev;
    ev.type = EventType::Change;
    ev.target = element;
    dispatch(arena, ev);
}

void EventDispatcher::setEditValue(NodeArena& arena, NodeId element, std::string_view text) {
    EditState* state = editStateOf(arena, m_editStates, element);
    if (state == nullptr || state->value == text) {
        return;
    }
    state->value.assign(text);
    state->cursor = static_cast<uint32_t>(state->value.size());
    state->anchor = state->cursor;
    invalidateDisplayString(arena, element);
}

bool EventDispatcher::editKeyDown(NodeArena& arena, NodeId element, const InputEvent& input) {
    EditState* state = focusedEdit(arena);
    if (state == nullptr) {
        return false;
    }
    const ShapeResult& shaped = shapeOf(arena, element);
    bool shift = (input.modifiers & kShift) != 0;
    bool ctrl = (input.modifiers & kCtrl) != 0;
    bool changed = false;
    switch (input.key) {
        case KeyCode::Backspace:
            changed = deleteBackward(*state, shaped);
            break;
        case KeyCode::Delete:
            changed = deleteForward(*state, shaped);
            break;
        case KeyCode::Left:
            moveLeft(*state, shaped, shift);
            break;
        case KeyCode::Right:
            moveRight(*state, shaped, shift);
            break;
        case KeyCode::Home:
            moveHome(*state, shift);
            break;
        case KeyCode::End:
            moveEnd(*state, shift);
            break;
        case KeyCode::KeyA:
            if (!ctrl) {
                return false;
            }
            selectAll(*state);
            break;
        default:
            return false;  // Enter (single-line), Escape, ...: no default action.
    }
    afterEdit(arena, element, changed);
    return true;
}

void EventDispatcher::handleKeyDown(NodeArena& arena, NodeId root, const InputEvent& input) {
    // T-388 (08.12): Escape cancels the drag in progress (pointer or gamepad) and is consumed.
    if (input.key == KeyCode::Escape && m_drag.active) {
        cancelDrag(arena);
        return;
    }
    // T-394 (08.15): Escape before the drag threshold cancels the press (no Click on release) and
    // is consumed too.
    if (input.key == KeyCode::Escape && m_drag.pending && m_activeNode != kInvalidNode) {
        cancelPress();
        return;
    }
    bool hasFocus = m_focus[m_curPlayer] != kInvalidNode && arena.alive(m_focus[m_curPlayer]);
    Event ev;
    ev.type = EventType::KeyDown;
    ev.target = hasFocus ? m_focus[m_curPlayer] : root;
    ev.key = input.key;
    ev.modifiers = input.modifiers;
    dispatch(arena, ev);
    if (ev.preventDefault) {
        return;
    }

    // 08.8: a KeyDown received mid-composition performs no default action (the IME owns it);
    // it still bubbled above.
    if (m_composition != nullptr && m_composition->active) {
        return;
    }
    if (input.key == KeyCode::Tab) {
        const NodeId from = m_focus[m_curPlayer];
        NodeId next =
            nextFocusable(arena, focusScope(arena, root, from), from, (input.modifiers & kShift) != 0);
        setFocus(arena, next, m_curPlayer);
        return;
    }
    // T-194 (08.9): arrow keys navigate unless the focus is a text input (its caret keys).
    if (input.key == KeyCode::Up || input.key == KeyCode::Down || input.key == KeyCode::Left ||
        input.key == KeyCode::Right) {
        if (!hasFocus || m_ir == nullptr ||
            widgetKind(arena, *m_ir, m_doc, m_focus[m_curPlayer]) != WidgetKind::TextInput) {
            const NavDirection dir = input.key == KeyCode::Up     ? NavDirection::Up
                                     : input.key == KeyCode::Down ? NavDirection::Down
                                     : input.key == KeyCode::Left ? NavDirection::Left
                                                                  : NavDirection::Right;
            navigate(arena, root, dir, m_curPlayer);
            return;
        }
    }
    if (!hasFocus || m_ir == nullptr || isDisabled(arena, m_focus[m_curPlayer])) {
        return;
    }
    WidgetKind kind = widgetKind(arena, *m_ir, m_doc, m_focus[m_curPlayer]);
    // T-379 (08.10): `key-activation: none` (or Context::setKeyboardActivation(false)) skips the
    // Enter / Space activation below; navigation above and text editing stay.
    const bool activates = input.repeat == 0 && m_keyActivation;
    if (kind == WidgetKind::Button && activates &&
        (input.key == KeyCode::Enter || input.key == KeyCode::Space)) {
        activate(arena, m_focus[m_curPlayer], m_pointerPos, m_pointerLocalPos);
        return;
    }
    if (kind == WidgetKind::Checkbox && activates && input.key == KeyCode::Space) {
        activate(arena, m_focus[m_curPlayer], m_pointerPos, m_pointerLocalPos);
        return;
    }
    // T-194: an element focusable through `tabindex` / triggers / drag attributes is activated
    // by Enter like a button.
    if (kind == WidgetKind::None && activates && input.key == KeyCode::Enter) {
        activate(arena, m_focus[m_curPlayer], m_pointerPos, m_pointerLocalPos);
        return;
    }
    if (kind == WidgetKind::TextInput) {
        editKeyDown(arena, m_focus[m_curPlayer], input);
    }
}

void EventDispatcher::handleKeyUp(NodeArena& arena, NodeId root, const InputEvent& input) {
    bool hasFocus = m_focus[m_curPlayer] != kInvalidNode && arena.alive(m_focus[m_curPlayer]);
    Event ev;
    ev.type = EventType::KeyUp;
    ev.target = hasFocus ? m_focus[m_curPlayer] : root;
    ev.key = input.key;
    ev.modifiers = input.modifiers;
    dispatch(arena, ev);
}

void EventDispatcher::handleTextInput(NodeArena& arena, NodeId root, const InputEvent& input) {
    bool hasFocus = m_focus[m_curPlayer] != kInvalidNode && arena.alive(m_focus[m_curPlayer]);
    std::string_view text(input.utf8, input.utf8Len);
    Event ev;
    ev.type = EventType::TextInput;
    ev.target = hasFocus ? m_focus[m_curPlayer] : root;
    ev.modifiers = input.modifiers;
    ev.text = text;
    dispatch(arena, ev);
    if (ev.preventDefault || text.empty()) {
        return;
    }
    EditState* state = focusedEdit(arena);
    if (state == nullptr) {
        return;
    }
    bool changed = insertUtf8(*state, text);
    afterEdit(arena, m_focus[m_curPlayer], changed);
}

void EventDispatcher::refreshComposition(NodeArena& arena) {
    if (m_composition == nullptr || !m_composition->active) {
        return;
    }
    const EditState* state = editStateOf(arena, m_editStates, m_composition->element);
    composeDisplay(*m_composition, state != nullptr ? std::string_view(state->value) : std::string_view{});
    invalidateDisplayString(arena, m_composition->element);
}

void EventDispatcher::cancelComposition(NodeArena& arena) {
    if (m_composition == nullptr || !m_composition->active) {
        return;
    }
    NodeId element = m_composition->element;
    uint32_t anchor = m_composition->anchorByte;
    m_composition->active = false;
    m_composition->suppressDefault = false;
    m_composition->element = kInvalidNode;
    m_composition->text.clear();
    m_composition->display.clear();
    m_composition->cursor = 0;
    m_composition->selLen = 0;
    // 08.8: the display string returns to the value and the caret to the composition's anchor.
    EditState* state = editStateOf(arena, m_editStates, element);
    if (state != nullptr) {
        uint32_t size = static_cast<uint32_t>(state->value.size());
        uint32_t offset = anchor < size ? anchor : size;
        state->cursor = offset;
        state->anchor = offset;
    }
    if (arena.alive(element) && arena.kind(element) == NodeKind::Element) {
        invalidateDisplayString(arena, element);
    }
}

void EventDispatcher::handleCompositionStart(NodeArena& arena, NodeId root, const InputEvent& input) {
    bool hasFocus = m_focus[m_curPlayer] != kInvalidNode && arena.alive(m_focus[m_curPlayer]);
    Event ev;
    ev.type = EventType::CompositionStart;
    ev.target = hasFocus ? m_focus[m_curPlayer] : root;
    ev.modifiers = input.modifiers;
    dispatch(arena, ev);

    if (m_composition == nullptr) {
        return;
    }
    EditState* state = focusedEdit(arena);
    if (state == nullptr) {
        return;  // No focused editable element: the event bubbled, but there is no default action.
    }
    // 08.8 / T-33b: an existing selection is deleted first (and that does fire Change).
    if (hasSelection(*state)) {
        bool changed = insertUtf8(*state, std::string_view{});
        afterEdit(arena, m_focus[m_curPlayer], changed);
    }
    m_composition->active = true;
    m_composition->element = m_focus[m_curPlayer];
    m_composition->anchorByte = state->cursor;
    m_composition->cursor = 0;
    m_composition->selLen = 0;
    m_composition->text.clear();
    m_composition->suppressDefault = ev.preventDefault;
    refreshComposition(arena);
}

void EventDispatcher::handleCompositionUpdate(NodeArena& arena, NodeId root, const InputEvent& input) {
    bool hasFocus = m_focus[m_curPlayer] != kInvalidNode && arena.alive(m_focus[m_curPlayer]);
    Event ev;
    ev.type = EventType::CompositionUpdate;
    ev.target = hasFocus ? m_focus[m_curPlayer] : root;
    ev.modifiers = input.modifiers;
    ev.text = input.composition;
    dispatch(arena, ev);

    if (m_composition == nullptr || !m_composition->active) {
        return;
    }
    m_composition->text.assign(input.composition);
    uint32_t size = static_cast<uint32_t>(m_composition->text.size());
    m_composition->cursor = input.compositionCursor < size ? input.compositionCursor : size;
    uint32_t remaining = size - m_composition->cursor;
    m_composition->selLen = input.compositionSelLen < remaining ? input.compositionSelLen : remaining;
    m_composition->suppressDefault = ev.preventDefault;
    // 08.8: the value itself is untouched and no Change is dispatched; only the display string
    // (and so the layout) changes.
    refreshComposition(arena);
}

void EventDispatcher::handleCompositionEnd(NodeArena& arena, NodeId root, const InputEvent& input) {
    bool hasFocus = m_focus[m_curPlayer] != kInvalidNode && arena.alive(m_focus[m_curPlayer]);
    Event ev;
    ev.type = EventType::CompositionEnd;
    ev.target = hasFocus ? m_focus[m_curPlayer] : root;
    ev.modifiers = input.modifiers;
    dispatch(arena, ev);
    cancelComposition(arena);
}

void EventDispatcher::handleInput(NodeArena& arena, NodeId root, const InputEvent& input) {
    // T-194: every event dispatched while this input is handled carries its player.
    struct PlayerScope {
        uint8_t& slot;
        uint8_t saved;
        ~PlayerScope() { slot = saved; }
    } playerScope{m_curPlayer, m_curPlayer};
    m_curPlayer = input.player < kMaxPlayers ? input.player : static_cast<uint8_t>(kMaxPlayers - 1);
    // T-387 (08.12): every event a pointer / wheel / gesture input causes carries its modifiers
    // (key events fill their own).
    PlayerScope modifierScope{m_curModifiers, m_curModifiers};
    const bool pointerInput =
        input.kind == InputEvent::PointerMove || input.kind == InputEvent::PointerDown ||
        input.kind == InputEvent::PointerUp || input.kind == InputEvent::Wheel ||
        input.kind == InputEvent::PointerLeave || input.kind == InputEvent::PointerCancel ||
        input.kind == InputEvent::Gesture || input.kind == InputEvent::ExternalDragEnter ||
        input.kind == InputEvent::ExternalDragMove || input.kind == InputEvent::ExternalDragLeave ||
        input.kind == InputEvent::ExternalDrop;
    m_curModifiers = pointerInput ? input.modifiers : static_cast<uint8_t>(0);
    if (input.kind == InputEvent::PointerMove || input.kind == InputEvent::PointerDown ||
        input.kind == InputEvent::PointerUp) {
        m_lastPointer = input.pos;  // T-201.
    }
    switch (input.kind) {
        case InputEvent::PointerMove:
            setPointerMove(arena, root, input);
            break;
        case InputEvent::PointerLeave:
            setPointerLeaveWindow(arena);
            break;
        case InputEvent::PointerDown: {
            // T-394 (08.15): after a cancelled press, every other button is swallowed until all
            // buttons are up (a press that already ended stops the swallowing).
            const uint8_t bit = static_cast<uint8_t>(1u << (static_cast<uint8_t>(input.button) & 7u));
            if (m_swallowButtons && m_activeNode == kInvalidNode) {
                m_swallowButtons = false;
            }
            if (!m_swallowButtons) {
                handlePointerDown(arena, root, input);
            }
            m_heldButtons = static_cast<uint8_t>(m_heldButtons | bit);
            break;
        }
        case InputEvent::PointerUp: {
            const uint8_t bit = static_cast<uint8_t>(1u << (static_cast<uint8_t>(input.button) & 7u));
            const bool swallowed = (m_swallowButtons && input.button != m_pressButton) ||
                                   (m_consumedPress && input.button == m_pressButton);
            if (m_consumedPress && input.button == m_pressButton) {
                m_consumedPress = false;  // T-394: the release of a press an OutsidePress took.
            }
            if (!swallowed) {
                handlePointerUp(arena, root, input);
            }
            m_heldButtons = static_cast<uint8_t>(m_heldButtons & ~bit);
            if (m_heldButtons == 0) {
                m_swallowButtons = false;
            }
            break;
        }
        case InputEvent::Wheel:
            handleWheel(arena, root, input);
            break;
        case InputEvent::KeyDown:
            handleKeyDown(arena, root, input);
            break;
        case InputEvent::KeyUp:
            handleKeyUp(arena, root, input);
            break;
        case InputEvent::TextInput:
            handleTextInput(arena, root, input);
            break;
        case InputEvent::CompositionStart:
            handleCompositionStart(arena, root, input);
            break;
        case InputEvent::CompositionUpdate:
            handleCompositionUpdate(arena, root, input);
            break;
        case InputEvent::CompositionEnd:
            handleCompositionEnd(arena, root, input);
            break;
        case InputEvent::GamepadButtonDown:
        case InputEvent::GamepadButtonUp:
            handleGamepadButton(arena, root, input, input.kind == InputEvent::GamepadButtonDown);
            break;
        case InputEvent::GamepadAxisMotion:
            handleGamepadAxis(arena, root, input);
            break;
        case InputEvent::Gesture:
            handleGesture(arena, root, input);
            break;
        case InputEvent::PointerCancel:
            handlePointerCancel(arena);
            break;
        case InputEvent::ExternalDragEnter:
        case InputEvent::ExternalDragMove:
        case InputEvent::ExternalDragLeave:
        case InputEvent::ExternalDrop:
            handleExternalDrag(arena, root, input);  // T-393 (08.14).
            break;
    }
}

// -- T-387 (08.12, 13 §13.9): scroll offsets and OutsidePress -------------------------------------

Vec2 EventDispatcher::maxScrollOf(const NodeArena& arena, NodeId node) {
    if (!arena.alive(node) || arena.kind(node) != NodeKind::Element) {
        return Vec2{};
    }
    const ElementData& data = arena.element(node);
    auto scrollable = [](Overflow o) { return o == Overflow::Scroll || o == Overflow::Auto; };
    return Vec2{scrollable(usedOverflowX(data.style)) ? maxScrollX(data.box) : 0.0f,
                scrollable(usedOverflowY(data.style)) ? maxScrollY(data.box) : 0.0f};
}

bool EventDispatcher::setScroll(NodeArena& arena, NodeId node, float x, float y, bool notify) {
    if (!arena.alive(node) || arena.kind(node) != NodeKind::Element) {
        return false;
    }
    const Vec2 maxPos = maxScrollOf(arena, node);
    // Written so a NaN request clamps to 0, like any value below the range.
    const float newX = x > 0.0f ? std::min(x, maxPos.x) : 0.0f;
    const float newY = y > 0.0f ? std::min(y, maxPos.y) : 0.0f;
    const ElementData& data = arena.element(node);
    const float curX = data.style.base[prop::ScrollX].f;
    const float curY = data.style.base[prop::ScrollY].f;
    if (newX == curX && newY == curY) {
        return false;
    }
    if (newX != curX) {
        setProperty(arena, node, prop::ScrollX, Value::ofFloat(newX));
    }
    if (newY != curY) {
        setProperty(arena, node, prop::ScrollY, Value::ofFloat(newY));
    }
    if (notify) {
        Event ev;
        ev.type = EventType::Scroll;
        ev.target = node;
        ev.pos = m_lastPointer;
        ev.scrollPos = Vec2{newX, newY};
        dispatch(arena, ev);
    }
    return true;
}

bool EventDispatcher::dispatchOutsidePress(NodeArena& arena, NodeId root, NodeId hit,
                                           const InputEvent& input) {
    if (m_outsidePress.empty()) {
        return false;
    }
    bool consumed = false;
    // The registry is copied first: a listener may register or destroy elements meanwhile (a
    // destroyed one is skipped, a new one waits for the next press).
    const size_t start = m_pathStack.size();
    for (const PathEntry& e : m_outsidePress) {
        m_pathStack.push_back(e);
    }
    const size_t end = m_pathStack.size();
    for (size_t i = start; i < end; ++i) {
        const PathEntry entry = m_pathStack[i];
        if (!stillThere(arena, entry) || arena.kind(entry.node) != NodeKind::Element) {
            continue;
        }
        const NodeId n = entry.node;
        // Inside the document's tree (not a detached template or pooled instance) and visible:
        // neither it nor an ancestor is display:none or visibility:hidden.
        bool visible = true;
        NodeId top = n;
        for (NodeId a = n; a != kInvalidNode; a = arena.parent(a)) {
            if (arena.kind(a) == NodeKind::Element) {
                const ElementData& d = arena.element(a);
                if (d.style.display == Display::None || d.style.visibility == Visibility::Hidden) {
                    visible = false;
                    break;
                }
            }
            top = a;
        }
        if (!visible || top != root) {
            continue;
        }
        if (hit != kInvalidNode && arena.alive(hit) && (hit == n || arena.isAncestorOf(n, hit))) {
            continue;  // Pressed inside its subtree.
        }
        Event ev;
        ev.type = EventType::OutsidePress;
        ev.target = n;
        ev.pos = input.pos;
        ev.button = input.button;
        ev.modifiers = input.modifiers;
        Mat2x3 inv{};
        if (invert(arena.common(n).world, inv)) {
            ev.localPos = apply(inv, input.pos);
        }
        dispatchAt(arena, ev, n);
        // T-394 (08.15): a listener's `consume`, or a `consume()` in the element's trigger (the
        // trigger runs after the input, so its action is looked up now).
        if (ev.consume || (m_triggerSink != nullptr && arena.alive(n) && arena.common(n).triggerTable != 0 &&
                           m_triggerSink->consumesOutsidePress(n))) {
            consumed = true;
        }
    }
    m_pathStack.resize(start);
    return consumed;
}

// -- T-394 (08.15): press cancel, listener lookup --------------------------------------------------

void EventDispatcher::cancelPress() {
    m_drag = DragState{};
    m_ptrDrag = PointerDragState{};
    m_axisDrag = kInvalidNode;
    m_pressDragged = true;  // Its release dispatches no Click.
    m_swallowButtons = true;
}

bool EventDispatcher::hasListener(const NodeArena& arena, NodeId node, EventType type) const {
    if (!arena.alive(node) || arena.kind(node) != NodeKind::Element) {
        return false;
    }
    for (uint32_t cur = arena.element(node).listenerHead; cur != kInvalidListener;
         cur = m_listeners[cur].next) {
        if (!m_listeners[cur].dead && m_listeners[cur].type == type) {
            return true;
        }
    }
    return false;
}

}  // namespace xt
