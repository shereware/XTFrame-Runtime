#pragma once

#include <cstdint>
#include <vector>

#include "xt/event.h"
#include "xt/input.h"
#include "xt/node_arena.h"
#include "xt/types.h"

/// Hit testing (08.2), the listener pool + bubbling dispatch (08.3) and the pointer state machine
/// (08.4), all owned by one Document. Internal to xt_core (not installed under Code/Runtime/core/include): only
/// Document (document.cpp) and its tests (test_hit_test.cpp, test_events.cpp, which the test
/// target's include path adds Code/Runtime/core/src to) use this directly. See Docs/tasks/T-32.md.

namespace xt {

class IrView;
class Document;
struct EditState;
struct CompositionState;

/// hitTestDetailed()'s result: the hit node (kInvalidNode when nothing hit) and, when hit, that
/// node's own local coordinates for `pos` -- Docs/tasks/T-48b.md: `inverse(world) * pos` for a 2D
/// node (`world` already plane-relative inside a 3D root's subtree), or, for a 3D layer node
/// itself, the plane's own intersection point `q` (19.9) untransformed (the layer's plane-relative
/// `world` is identity).
struct HitResult {
    NodeId node = kInvalidNode;
    Vec2 localPos{};
    /// T-174 (08.9): the hit node is `pointer-watch`, i.e. 08.9's `WatchOnly` result (Hover mode
    /// only; a Press-mode test never lands on one).
    bool watchOnly = false;
};

/// What a hit test is for (08.9, T-174). Hover: every hit-testable element (pointer move, hover,
/// leave). Press: `pointer-watch` elements are skipped, so a press passes through them to the
/// next hit below (or to the host when there is none).
enum class HitMode : uint8_t { Hover, Press };

/// Same algorithm as hitTest(), also returning the hit node's own local coordinates (see
/// HitResult). `hitTest()` is a thin wrapper: `hitTestDetailed(...).node`.
HitResult hitTestDetailed(const NodeArena& arena, NodeId root, Vec2 pos, HitMode mode = HitMode::Hover);

/// Non-recursive, explicit-stack depth-first hit test over `root`'s Element subtree. See
/// Docs/design/08-events.md §8.2, hit_test.cpp for the exact rules (display/visibility/
/// pointer-events/overflow-clip/transform). Returns kInvalidNode when nothing hits (e.g. `root`
/// itself is display:none, or its world transform is singular).
NodeId hitTest(const NodeArena& arena, NodeId root, Vec2 pos, HitMode mode = HitMode::Hover);

/// Which built-in behavior an Element carries (Docs/tasks/T-33.md "規則精確化"), derived from its
/// tag and (for `input`) its `type` attribute. Everything but None is focusable by tag.
enum class WidgetKind : uint8_t { None, Button, TextInput, Checkbox, OtherInput };

/// focus.cpp. `ir` is the Document's IR (tag/attribute names live in its string table); `doc`,
/// when not null, resolves them instead (T-270a: Document::string also reads the runtime string
/// pool a createElement() tag or a setAttribute() name/value is interned into).
WidgetKind widgetKind(const NodeArena& arena, const IrView& ir, const Document* doc, NodeId node);

/// Docs/tasks/T-33.md "可 focus": ElementData::focusable (set at load time from the tag /
/// `tabindex`) and not disabled, display:none or visibility:hidden -- the latter three checked on
/// `node` itself *and* every ancestor, so an element inside a hidden subtree is not focusable.
bool isFocusable(const NodeArena& arena, NodeId node);

/// `node` itself if focusable, else its nearest focusable ancestor, else kInvalidNode (08.4's
/// "命中元素或其最近可 focus 祖先").
NodeId nearestFocusable(const NodeArena& arena, NodeId node);

/// The next focusable element after `from` in document (preorder) order, wrapping around;
/// `backward` walks the same order in reverse (Shift+Tab). `from == kInvalidNode` starts at the
/// first (or, backward, the last) focusable element. kInvalidNode when there is none.
NodeId nextFocusable(const NodeArena& arena, NodeId root, NodeId from, bool backward);

/// `element`'s first Text child, or kInvalidNode. (An editable `input` always has exactly one,
/// created at load time.)
NodeId firstTextChild(const NodeArena& arena, NodeId element);

// -- focus_nav.cpp (T-194, 08.9): directional focus navigation. --------------------------------

/// The navigation / Tab scope of a focus at `from`: its nearest ancestor-or-self with
/// NodeFlags2::kFocusTrap (a modal), else `root`.
NodeId focusScope(const NodeArena& arena, NodeId root, NodeId from);
/// The geometric score of moving from `from` to `to` (world-space AABBs) in `dir`; lower is
/// better. `eligible` is false when `to` is not ahead of `from` inside the direction cone. See
/// focus_nav.cpp for the formula.
float navScore(Rect from, Rect to, NavDirection dir, bool& eligible);
/// Where navigation starts with nothing focused: the first focusable `autofocus` element inside
/// `scope`, else the first navigable focusable element of `scope` in document order.
NodeId firstNavTarget(const NodeArena& arena, NodeId scope, const std::vector<NodeId>& autofocus);
/// The element focus navigation in `dir` moves to from `from` (08.9): its `nav:<dir>` target, or
/// under `focus-nav: auto` the best geometric candidate inside focusScope(). kInvalidNode when
/// navigation is blocked. `from == kInvalidNode` returns firstNavTarget(root scope).
NodeId findNavTarget(const NodeArena& arena, NodeId root, NodeId from, NavDirection dir,
                     const std::vector<NodeId>& autofocus);

/// Which custom state a drag toggles (27.7): `Dragging` on the source, `DropHover` on the drop
/// target under the drag.
enum class DragStateKind : uint8_t { Dragging, DropHover };

/// document.cpp; the free-function form of Document::hitTestText (Docs/design/07-text.md §7.7),
/// so the dispatcher can place the caret from a PointerDown without a Document.
uint32_t hitTestTextOffset(const NodeArena& arena, NodeId element, Vec2 localPos);

/// One listener pool entry (08.3 "監聽器儲存"). `next` chains every listener registered on the
/// same element in LIFO (most-recently-added-first) order; `ElementData::listenerHead` is that
/// chain's head (kInvalidListener == none). `dead` marks a removed entry pending the next
/// compaction (EventDispatcher::compact); removeListener never shrinks or reorders m_listeners, so
/// a previously returned id (== the entry's index) stays valid forever, including across
/// compactions (see EventDispatcher::compact's comment).
struct Listener {
    EventType type = EventType::Click;
    EventListener fn;
    NodeId node = kInvalidNode;
    uint32_t next = kInvalidListener;
    uint8_t dead = 0;
};

/// Receives every event the dispatcher delivers, per visited node, after that node's listeners ran
/// (T-174: Document's trigger runtime turns them into trigger events). Also told about a Checked
/// toggle by the checkbox default action.
class TriggerSink {
public:
    /// `event` was delivered at `node` (`event.current == node`).
    virtual void onDomEvent(const Event& event, NodeId node) = 0;
    /// The checkbox default action toggled `node`'s Checked state.
    virtual void onCheckedChanged(NodeId node, bool checked) = 0;
    /// T-194: `player`'s focus moved to `node` (kInvalidNode = cleared); aria-label announce.
    virtual void onFocusMoved(NodeId /*node*/, uint8_t /*player*/) {}
    /// T-194: a drag turned `kind` on or off at `node` (the `Dragging` / `DropHover` states).
    virtual void onDragState(NodeId /*node*/, DragStateKind /*kind*/, bool /*on*/) {}
    /// T-194: a drop was delivered (`event` as dispatched; the host's HostEvent::Drop).
    virtual void onDrop(const Event& /*event*/) {}
    /// T-194: the `<repeat>` entry index `node` belongs to, 0xFFFFFFFF when none.
    virtual uint32_t itemIndexOf(NodeId /*node*/) { return 0xFFFFFFFFu; }
    /// T-201: a `draggable="axis-x"` element reports `drag.value` (0..1) or `nav.value` (-1 / +1).
    virtual void onAxisValue(NodeId /*node*/, bool /*nav*/, float /*value*/, uint8_t /*player*/) {}
    /// T-394 (08.15): true when one of `node`'s `on:outsidepress` triggers holds `consume()` (asked
    /// right after its OutsidePress was delivered; the triggers themselves run later).
    virtual bool consumesOutsidePress(NodeId /*node*/) { return false; }
    /// T-394 (08.15): true when `node` has an `on:pointerdrag` trigger.
    virtual bool hasPointerDrag(NodeId /*node*/) { return false; }
    /// T-394 (08.15): Event::dropIndex of a DragOver / Drop on `target` at `pos` (root
    /// coordinates), whose `targetItem` / `dropFraction` are already computed. The default covers a
    /// `<repeat>` instance target only: `targetItem + (fraction.y >= 0.5)`, 0xFFFFFFFF otherwise.
    virtual uint32_t dropIndexOf(NodeId /*target*/, Vec2 /*pos*/, uint32_t targetItem, Vec2 fraction) {
        return targetItem == 0xFFFFFFFFu ? 0xFFFFFFFFu : targetItem + (fraction.y >= 0.5f ? 1u : 0u);
    }

protected:
    ~TriggerSink() = default;
};

/// Listener pool + bubbling dispatch + pointer state machine for one Document. Operates purely on
/// a caller-supplied NodeArena& (and, for handleInput, a root NodeId); it does not itself own or
/// reference a Document, so it can be unit-tested (test_hit_test.cpp/test_events.cpp) without one.
class EventDispatcher {
public:
    /// Borrows the Document's IR (for tag/attribute lookups), its EditState pool (for the
    /// `input[type=text]` key handling) and its single CompositionState (08.8, T-33b); all three
    /// must outlive the dispatcher. Any of them may be null in a Document-less unit test, which
    /// then simply has no widget behavior (and, for a null `composition`, no IME support).
    void init(const IrView* ir, std::vector<EditState>* editStates, CompositionState* composition);
    /// T-174: where delivered events go after the listeners (null = nowhere).
    void setTriggerSink(TriggerSink* sink) { m_triggerSink = sink; }
    /// T-270a: the owning Document, so widgetKind() resolves host-interned tags and attributes
    /// (null in a Document-less unit test: IR strings only).
    void setDocument(const Document* doc) { m_doc = doc; }
    /// T-260a: the Document's unload is queued (Document::markUnloading): no element listener is
    /// called from now on; the trigger sink still is (its host-facing actions check it themselves).
    void muteListeners() { m_listenersMuted = true; }
    /// T-201: the position of the last pointer move, press or release (viewport px), for
    /// `spawn(..., at=pointer)`.
    Vec2 pointerPosition() const { return m_lastPointer; }
    /// T-552 (27 §27.4.1): true between a pointer move over this document and the pointer leaving
    /// it (PointerLeave): the hover it set still stands at m_pointerPos.
    bool pointerIn() const { return m_pointerIn; }
    /// T-552 (27 §27.4.1, 2026-10-31 ruling "pointer and hover first, then bindings"): Document::update()
    /// laid out or moved something while the pointer stood over the document (markHoverStale());
    /// refreshHover(), called by Context::update() before the queued input, then re-judges hover at the
    /// last pointer position against the new geometry, as a move to the same spot would (PointerLeave /
    /// PointerEnter, the Hover state bit), without a PointerMove event. No-op unless marked stale and the
    /// pointer is over the document.
    void markHoverStale() { m_hoverStale = true; }
    void refreshHover(NodeArena& arena, NodeId root);

    /// See Listener's docs; target must be alive and NodeKind::Element. Returns the new listener's
    /// id (== its index into the internal pool).
    uint32_t addListener(NodeArena& arena, NodeId target, EventType type, EventListener fn);
    /// Marks `id` dead (no-op for an out-of-range id or one already dead). Every 1024 removals,
    /// rebuilds every touched element's listener chain to skip dead entries (see compact()).
    void removeListener(NodeArena& arena, uint32_t id);

    /// Calls every `event.type`-matching, non-dead listener on `node` only (registration order),
    /// ignoring bubbling. Used for PointerEnter/Leave/Focus/Blur (08.3: "不冒泡").
    void dispatchAt(const NodeArena& arena, Event& event, NodeId node);
    /// Dispatches `event` from `event.target` upward to the root (08.3), setting event.current at
    /// each step. Stops ascending once a node's listeners set event.stopPropagation (that node's
    /// own remaining listeners still all run first).
    void dispatch(const NodeArena& arena, Event& event);

    /// 08.4's pointer state machine: consumes one queued InputEvent, updating hoverNode/activeNode
    /// and ElementState bits, dispatching PointerMove/Enter/Leave/Down/Up/Click/Wheel as needed.
    /// `root` is the Document's root Element (the hit-test subtree). Keyboard, text and
    /// composition InputEvent kinds are handled here too (08.5, 08.8).
    void handleInput(NodeArena& arena, NodeId root, const InputEvent& input);

    NodeId hoverNode() const { return m_hoverNode; }
    NodeId activeNode() const { return m_activeNode; }
    /// Player 0's focus (mouse / keyboard).
    NodeId focusNode() const { return m_focus[0]; }
    /// T-194 (08.9): `player`'s focus (kInvalidNode for a player index >= kMaxPlayers).
    NodeId focusNode(uint8_t player) const { return player < kMaxPlayers ? m_focus[player] : kInvalidNode; }

    /// Docs/tasks/T-33.md "setFocus(n)": Blur (non-bubbling) + kStateFocus cleared on the old
    /// focus, kStateFocus + Focus on the new one, both marked kDirtyStyle. `node == kInvalidNode`
    /// (or a node that is not a live Element) just clears the focus. No-op when `node` already has
    /// the focus. T-194: per player; kStateFocus stays while any player focuses the element
    /// (ElementData::focusPlayers), Focus/Blur carry Event::player.
    void setFocus(NodeArena& arena, NodeId node, uint8_t player = 0);
    /// Called once per Document::update(): drops a focus whose element died or stopped being
    /// focusable (removed, display:none, disabled). A still-live element gets its Blur.
    /// T-399 (08 §8.18): runs after the style / layout pass. Returns true when a focus changed.
    bool validateFocus(NodeArena& arena);
    /// T-399 (08 §8.18): ElementRef::focus() on an element that is `display: none` now but may be
    /// shown by this frame's pending changes records it here; applyPendingFocus() (in update(),
    /// after style / layout) gives it the focus when it is focusable by then. Any setFocus() for
    /// the player drops it.
    void setPendingFocus(NodeId node, uint32_t generation, uint8_t player = 0);
    /// Applies (or drops) the pending focus of every player. Returns true when a focus changed;
    /// `dropped` is set when a pending element was still not focusable.
    bool applyPendingFocus(NodeArena& arena, bool& dropped);

    // -- T-194 (08.9, 27.7) --------------------------------------------------------------------
    /// The clock (seconds) gamepad repeat and long press are measured against; Document sets it
    /// before every handleInput()/tick().
    void setTime(float time) { m_time = time; }
    /// Per-frame: held d-pad / stick repeats and the A long press that starts a gamepad drag.
    void tick(NodeArena& arena, NodeId root, float time);
    /// Moves `player`'s focus in `dir` (findNavTarget); dispatches NavBlocked (bubbling, from the
    /// focus or `root`) when there is no target. Returns true when the focus moved.
    bool navigate(NodeArena& arena, NodeId root, NavDirection dir, uint8_t player);
    /// The `autofocus` elements in document order (firstNavTarget()'s preference).
    void setAutofocus(std::vector<NodeId> nodes) { m_autofocus = std::move(nodes); }
    const std::vector<NodeId>& autofocus() const { return m_autofocus; }
    /// Held-direction repeat: first repeat after `delay` seconds, then every `interval`.
    void setNavRepeat(float delay, float interval) {
        m_repeatDelay = delay;
        m_repeatInterval = interval;
    }
    /// T-379 (08.10): false skips the Enter / Space activation of the focused element (button,
    /// checkbox, `tabindex` element); Tab and arrow-key navigation are unaffected. Document sets
    /// it before every handleInput() (`key-activation` AND Context::keyboardActivation()).
    void setKeyboardActivation(bool enabled) { m_keyActivation = enabled; }
    bool keyboardActivation() const { return m_keyActivation; }
    /// True while a drag is in progress (pointer or gamepad).
    bool dragActive() const { return m_drag.active; }
    /// The dragged element (kInvalidNode when no drag).
    NodeId dragSource() const { return m_drag.active ? m_drag.source : kInvalidNode; }
    /// The drop target under the drag (kInvalidNode when none).
    NodeId dragOver() const { return m_drag.active ? m_drag.over : kInvalidNode; }
    /// The player driving the drag.
    uint8_t dragPlayer() const { return m_drag.player; }
    /// T-394 (08.15): the element in a pointerdrag (`on:pointerdrag` / a PointerDrag listener, its
    /// press moved past the threshold); kInvalidNode when none (`if dragging`).
    NodeId pointerDragNode() const { return m_ptrDrag.dragging ? m_ptrDrag.node : kInvalidNode; }
    /// The drag ghost's offset from the source's laid-out position (27.7 `drag-ghost`).
    Vec2 dragGhostOffset(const NodeArena& arena) const;
    /// Cancels the drag in progress (T-388, 08.12): states cleared, DragLeave on the target, then
    /// DragCancel and DragEnd (dropped = false) on the source; no Drop. No-op without one.
    void cancelDrag(NodeArena& arena);
    /// The nearest `drop-target` ancestor-or-self of `hit` that is not the drag source nor inside
    /// it; kInvalidNode when none.
    NodeId dropTargetOf(const NodeArena& arena, NodeId hit) const;
    /// 08.8: true when the focused element is a non-disabled `input[type=text]`.
    bool wantsTextInput(const NodeArena& arena) const;
    /// T-35's Document::remove(): marks every listener registered on `node` dead and resets its
    /// chain head, so the pool holds nothing pointing at a node that is about to be destroyed.
    void removeListenersFor(NodeArena& arena, NodeId node);
    /// T-35's Document::remove(): drops the focus (Blur), the hover path (PointerLeave on every
    /// hovered node inside the subtree) and the active chain when they point into the subtree
    /// rooted at `root`, which is about to be destroyed. Called while the subtree is still intact,
    /// so the Blur/Leave listeners still see a live tree.
    void clearForSubtreeRemoval(NodeArena& arena, NodeId root);
    /// T-387 (08.12, 13 §13.9): the one place a scroll offset changes (wheel, the ElementRef
    /// scroll API). Writes ScrollX / ScrollY = (`x`, `y`), each clamped to [0, maxScroll] of an
    /// axis that is `overflow: scroll` / `auto` (0 on any other axis). When the offset actually
    /// changed and `notify` is set, dispatches one bubbling Scroll (target = `node`,
    /// Event::scrollPos = the new offset). Returns true when the offset changed. No-op (false)
    /// for a node that is not a live Element.
    bool setScroll(NodeArena& arena, NodeId node, float x, float y, bool notify);
    /// T-387: `node`'s largest scroll offset per axis: maxScrollX / maxScrollY of its layout box on
    /// an `overflow: scroll` / `auto` axis, 0 on any other axis.
    static Vec2 maxScrollOf(const NodeArena& arena, NodeId node);
    /// T-387 (08.12): registers `node` for OutsidePress (an OutsidePress listener or an
    /// `on:outsidepress` trigger). A press checks only the registered elements; duplicates and
    /// destroyed nodes are dropped. addListener() calls it for an OutsidePress listener.
    void watchOutsidePress(const NodeArena& arena, NodeId node);
    /// Marks `element` (and its Text child) dirty after an edit and, when the value actually
    /// changed, invalidates its shaping cache and dispatches a bubbling Change. Public because
    /// ElementRef::setValue() (document.cpp) needs the same after-edit bookkeeping.
    void afterEdit(NodeArena& arena, NodeId element, bool valueChanged);
    /// T-392 (27 §27.3.5): `bind:value` -- replaces editable `element`'s value (caret to the end)
    /// and invalidates its display string, without a Change event. No-op when it is not an
    /// editable `input` or the value is already `text`.
    void setEditValue(NodeArena& arena, NodeId element, std::string_view text);

private:
    void callListeners(const NodeArena& arena, NodeId node, Event& event);
    /// Rebuilds every element's listener chain referenced by m_listeners, in place, dropping dead
    /// entries (Docs/tasks/T-32.md "壓實（重建鏈表）"). Never resizes m_listeners itself, so ids
    /// (== indices) issued before a compaction stay valid after it: this is what lets
    /// removeListener() be called (with a pre-compaction id) safely from inside a listener
    /// callback that runs after a compaction has already happened. Listeners whose node was
    /// destroyed since being registered are dropped permanently (their arena slot's
    /// ElementData::listenerHead was already reset by NodeArena::destroy(); see the "uncovered
    /// cases" note in the T-32 report for the resulting unreclaimed m_listeners slots).
    void compact(NodeArena& arena);

    void setPointerMove(NodeArena& arena, NodeId root, const InputEvent& input);
    /// Hit-tests `pos` and moves the hover there (leave / enter chains); setPointerMove() and
    /// refreshHover() (T-552).
    void updateHover(NodeArena& arena, NodeId root, Vec2 pos);
    void setPointerLeaveWindow(NodeArena& arena);
    void handlePointerDown(NodeArena& arena, NodeId root, const InputEvent& input);
    void handlePointerUp(NodeArena& arena, NodeId root, const InputEvent& input);
    void handleWheel(NodeArena& arena, NodeId root, const InputEvent& input);
    /// T-387 (08.12): after a PointerDown on `hit`: OutsidePress (not bubbling) to every
    /// registered element that is visible inside `root`'s tree and whose subtree does not contain
    /// `hit` (kInvalidNode: the press hit nothing, i.e. it is outside every element).
    /// T-394 (08.15): dispatched before the PointerDown; returns true when a listener set
    /// Event::consume or an `on:outsidepress` trigger holds `consume()` (the press is taken).
    bool dispatchOutsidePress(NodeArena& arena, NodeId root, NodeId hit, const InputEvent& input);
    /// T-386 (08.11): InputEvent::PointerCancel.
    void handlePointerCancel(NodeArena& arena);
    void handleKeyDown(NodeArena& arena, NodeId root, const InputEvent& input);
    void handleKeyUp(NodeArena& arena, NodeId root, const InputEvent& input);
    void handleTextInput(NodeArena& arena, NodeId root, const InputEvent& input);
    /// 08.8's three composition events. Each dispatches a bubbling event first (target = the
    /// focused element, or `root` when nothing has the focus) and only then applies its default
    /// behavior; preventDefault sets CompositionState::suppressDefault (which suppresses the
    /// Painter's underline/clause highlight only) rather than skipping the state update, since
    /// §8.8's preventDefault "阻止預設顯示（不影響 value 語意）".
    void handleCompositionStart(NodeArena& arena, NodeId root, const InputEvent& input);
    void handleCompositionUpdate(NodeArena& arena, NodeId root, const InputEvent& input);
    void handleCompositionEnd(NodeArena& arena, NodeId root, const InputEvent& input);
    /// Rebuilds CompositionState::display from the composing element's value and marks that
    /// element's Text node for a reshape + relayout. No Change event (the value did not change).
    void refreshComposition(NodeArena& arena);
    /// Drops an in-flight composition without touching the value (used when the focus leaves the
    /// composing element). The caret returns to CompositionState::anchorByte.
    void cancelComposition(NodeArena& arena);
    /// Dispatches Click on `node` and then applies 08.4's checkbox default action (toggle
    /// kStateChecked + Change), shared by PointerUp and the keyboard Enter/Space activation.
    /// `localPos` is the Click event's `Event::localPos` (T-48b): the triggering hit's local
    /// coordinates, or (keyboard activation) the last known pointer hit's.
    void activate(NodeArena& arena, NodeId node, Vec2 pos, Vec2 localPos, uint8_t clicks = 1);
    /// The focused element's EditState, or nullptr when it is not an editable `input`.
    EditState* focusedEdit(NodeArena& arena);
    /// Applies one KeyDown to the focused `input[type=text]` (08.5's default actions). Returns
    /// true if the key was consumed.
    bool editKeyDown(NodeArena& arena, NodeId element, const InputEvent& input);
    // T-194 (08.9): gamepad, gesture and drag handling (focus_nav.cpp / drag_drop.cpp).
    void handleGamepadButton(NodeArena& arena, NodeId root, const InputEvent& input, bool down);
    void handleGamepadAxis(NodeArena& arena, NodeId root, const InputEvent& input);
    void handleGesture(NodeArena& arena, NodeId root, const InputEvent& input);
    void padPress(NodeArena& arena, NodeId root, uint8_t player);
    void padRelease(NodeArena& arena, uint8_t player);
    void dispatchBack(NodeArena& arena, NodeId root, uint8_t player);
    void startDrag(NodeArena& arena, NodeId source, uint8_t player, bool gamepad, Vec2 pos);
    void updateDragOver(NodeArena& arena, NodeId target);
    /// Ends the drag: Drop on the target under it when `drop`, else DragLeave; `cancel` adds
    /// DragCancel on the source; DragEnd on the source last (T-388).
    void finishDrag(NodeArena& arena, bool drop, bool cancel = false);
    /// T-393 (08.14): ExternalDragEnter / Move / Leave / ExternalDrop -- an OS drag driven through
    /// the drag state machine with no source (no ghost, no threshold, no DragCancel / DragEnd).
    void handleExternalDrag(NodeArena& arena, NodeId root, const InputEvent& input);
    /// T-388: `node`'s attribute `name` ("" when absent or without a Document).
    std::string_view attributeOf(const NodeArena& arena, NodeId node, std::string_view name) const;
    /// T-388 (08.12): the `drag-threshold` of a draggable (logical px, default 4).
    float dragThreshold(const NodeArena& arena, NodeId source) const;
    /// T-388: Document::fractionIn(target, pos), (0, 0) without a Document.
    Vec2 dropFractionOf(const NodeArena& arena, NodeId target, Vec2 pos) const;
    /// T-388 (08.12): one frame of the edge auto-scroll of a pointer drag (called from tick()).
    /// T-394 (08.15): when the previous frame scrolled, first re-hit-tests the pointer against the
    /// laid-out geometry and dispatches DragOver again (`root` = the hit-test subtree).
    void autoScroll(NodeArena& arena, NodeId root, float time);
    /// T-394 (08.15): the drop target of a pointer drag over `hit`: dropTargetOf(hit), except that
    /// over the source's own subtree a source that is itself a `drop-target` inside a `<repeat>`
    /// instance is the target (its row geometry decides the insertion index).
    NodeId pointerDropTarget(const NodeArena& arena, NodeId hit) const;
    /// T-394 (08.15): Event::dropIndex for `target` (TriggerSink::dropIndexOf).
    uint32_t dropIndexFor(NodeId target, Vec2 pos, uint32_t targetItem, Vec2 fraction);
    /// T-394 (08.15): true when `node` has a live listener for `type`.
    bool hasListener(const NodeArena& arena, NodeId node, EventType type) const;
    /// T-394 (08.15): ends the current pointer press without a Click (Escape before the drag
    /// threshold): the pending drag dropped, other buttons swallowed until every one is released.
    void cancelPress();
    /// Clears kStateActive on `chain` (the A-press chain of a pad).
    void clearActiveChain(NodeArena& arena, std::vector<NodeId>& chain);
    /// T-379 (08.10), T-383: called before a pointer Click on `clicked` is dispatched; returns the
    /// press's click number: the host's click count when it gives one, else 2 when the previous
    /// pointer Click hit the same element with the same button and its press was <= 400 ms and
    /// <= 4 px before this one, else 1. Records this click for pairing with the next one.
    uint8_t classifyClick(NodeArena& arena, NodeId clicked, uint32_t generation, const InputEvent& input);
    /// T-379 (08.10): called after that Click; dispatches DoubleClick (bubbling, same target,
    /// `Event::clicks` = `clicks`) when `clicks` >= 2.
    void detectDoubleClick(NodeArena& arena, NodeId clicked, uint32_t generation, const InputEvent& input,
                           Vec2 localPos, uint8_t clicks);

    const IrView* m_ir = nullptr;
    const Document* m_doc = nullptr;  // T-270a: setDocument().
    TriggerSink* m_triggerSink = nullptr;
    std::vector<EditState>* m_editStates = nullptr;
    CompositionState* m_composition = nullptr;

    std::vector<Listener> m_listeners;
    uint32_t m_deadCount = 0;

    NodeId m_hoverNode = kInvalidNode;
    NodeId m_activeNode = kInvalidNode;
    // T-390 (08.13): the button of the press in progress (PointerMove's Event::button while
    // m_activeNode is set) and of the release activate() turns into a Click (Left otherwise).
    PointerButton m_pressButton = PointerButton::Left;
    PointerButton m_clickButton = PointerButton::Left;
    // T-194: one focus per local player; [0] is the mouse / keyboard focus of 08.5.
    NodeId m_focus[kMaxPlayers] = {kInvalidNode, kInvalidNode, kInvalidNode, kInvalidNode,
                                   kInvalidNode, kInvalidNode, kInvalidNode, kInvalidNode};
    // T-399: ElementRef::focus() targets waiting for this frame's style / layout (with their
    // generations, so a recycled slot is never focused).
    NodeId m_pendingFocus[kMaxPlayers] = {kInvalidNode, kInvalidNode, kInvalidNode, kInvalidNode,
                                          kInvalidNode, kInvalidNode, kInvalidNode, kInvalidNode};
    uint32_t m_pendingFocusGen[kMaxPlayers] = {};
    // T-194: the player of the input event being handled; fills Event::player when a dispatch
    // site leaves it 0.
    uint8_t m_curPlayer = 0;
    // T-387 (08.12): InputEvent::modifiers of the pointer / wheel / gesture input being handled;
    // fills Event::modifiers when a dispatch site leaves it 0 (0 outside such an input).
    uint8_t m_curModifiers = 0;
    float m_time = 0;
    float m_repeatDelay = 0.4f;
    float m_repeatInterval = 0.15f;
    std::vector<NodeId> m_autofocus;
    // T-194: per player gamepad state: the held navigation direction (-1 = none) and its next
    // repeat time, the left stick's direction, and the element A is pressing.
    struct PadState {
        int8_t holdDir = -1;
        bool holdFromStick = false;
        int8_t stickDir = -1;
        float stickX = 0;
        float stickY = 0;
        float nextRepeat = 0;
        NodeId aNode = kInvalidNode;
        float aTime = 0;
        bool aDragged = false;     // The long press started a drag: the release does nothing.
        bool aSuppressed = false;  // This press dropped a gamepad drag: ignore its release.
        std::vector<NodeId> aChain;
    };
    PadState m_pads[kMaxPlayers];
    // T-194 (27.7): the one drag of this document. `pending` = pointer down on a draggable, not
    // yet moved past the threshold.
    struct DragState {
        bool pending = false;
        bool active = false;
        bool gamepad = false;
        bool external = false;  // T-393 (08.14): an OS drag from outside (source = kInvalidNode).
        uint8_t player = 0;
        NodeId source = kInvalidNode;
        NodeId over = kInvalidNode;
        Vec2 start{};
        Vec2 pos{};
    };
    DragState m_drag;
    // T-393 (08.14): the paths of the ExternalDrop being handled (Event::externalPaths of its
    // Drop); empty otherwise.
    std::vector<std::string> m_externalPaths;
    // T-388: the current pointer press became a drag (cleared by the next press, its release or
    // PointerCancel): its release dispatches no Click even when the drag was cancelled meanwhile.
    bool m_pressDragged = false;
    // T-388: the clock of the previous autoScroll() step.
    float m_autoScrollClock = 0;
    // T-394 (08.15): the previous autoScroll() step moved a container (the next one re-dispatches
    // DragOver against the new geometry).
    bool m_autoScrolled = false;
    // T-394 (08.15): the pointer buttons held (bit = PointerButton), and the press was cancelled
    // (drag cancel, Escape before the threshold): every other button's PointerDown / PointerUp is
    // swallowed until all buttons are up.
    uint8_t m_heldButtons = 0;
    bool m_swallowButtons = false;
    // T-394 (08.15): the press was taken by an OutsidePress `consume`: no active element; its
    // PointerUp and Click are not dispatched.
    bool m_consumedPress = false;
    // T-394 (08.15): the pointerdrag candidate of the current primary press (the nearest
    // ancestor-or-self with `on:pointerdrag` or a PointerDrag listener) and whether it passed its
    // threshold.
    struct PointerDragState {
        NodeId node = kInvalidNode;
        uint32_t generation = 0;
        Vec2 start{};
        bool dragging = false;
    };
    PointerDragState m_ptrDrag;
    // Set while a PointerDown landed on an editable `input`, so PointerMove extends its selection
    // (Docs/tasks/T-33.md "拖曳（active 時 PointerMove）擴選"). Cleared on PointerUp.
    NodeId m_dragEdit = kInvalidNode;
    Vec2 m_pointerPos{};
    bool m_pointerIn = false;   // T-552: see pointerIn().
    bool m_hoverStale = false;  // T-552: see markHoverStale().
    Vec2 m_lastPointer{};       // T-201: see pointerPosition().
    // T-201: the `draggable="axis-x"` element a pointer press is dragging along (drag_drop.cpp).
    NodeId m_axisDrag = kInvalidNode;
    void axisDragValue(NodeArena& arena, Vec2 pos);
    // T-48b: `m_hoverNode`'s own local coordinates as of the most recent hit (set alongside
    // m_pointerPos in setPointerMove); reused for a later PointerLeave's Event::localPos when
    // there is no fresh hit to compute one from (window-leave, subtree removal) and for a
    // keyboard-triggered Click's Event::localPos (08.4/08.5: both carry "the last known pointer
    // position").
    Vec2 m_pointerLocalPos{};
    // The (node, all ancestors) chain kStateActive was set on at the most recent PointerDown, so
    // PointerUp can clear exactly those bits without a tree search. Empty when m_activeNode ==
    // kInvalidNode or the down-time target was disabled (kStateActive is never set for it, 08.4
    // "disabled 元素：...不設 active").
    std::vector<NodeId> m_activeChain;

    // Scratch reused by dispatch()/dispatchAt()'s callListeners(): the matching listener ids on
    // one node, captured before any of their callbacks run (Docs/tasks/T-32.md "回呼中允許
    // addListener/removeListener...新增的監聽器本次不觸發"). T-260a: used as a stack -- each user
    // records its start offset, appends its ids, reads them by index and shrinks back to that
    // offset -- so a dispatch nested inside a callback never overwrites the outer one's ids.
    std::vector<uint32_t> m_callScratch;
    // T-260a: a node of a recorded propagation path (dispatch()) or hover chain (setPointerMove,
    // setPointerLeaveWindow, clearForSubtreeRemoval) with its generation when recorded. The path
    // is fixed before any listener runs (as in the DOM); a node destroyed meanwhile (dead, or its
    // slot reused) is skipped. m_pathStack is a stack like m_callScratch.
    struct PathEntry {
        NodeId node = kInvalidNode;
        uint32_t generation = 0;
    };
    std::vector<PathEntry> m_pathStack;
    bool m_listenersMuted = false;  // T-260a: muteListeners().
    // T-387 (08.12): the elements watchOutsidePress() registered (with their generation, so a
    // destroyed node or a reused slot is recognized and dropped).
    std::vector<PathEntry> m_outsidePress;
    // T-379 (08.10): setKeyboardActivation().
    bool m_keyActivation = true;
    // T-379 (08.10): the current press (time, position, host click count) and the last pointer
    // Click detectDoubleClick() pairs the next one with (`valid` false after a double click).
    float m_pressTime = 0;
    Vec2 m_pressPos{};
    uint8_t m_pressClicks = 0;
    struct LastClick {
        bool valid = false;
        PathEntry node;
        PointerButton button = PointerButton::Left;
        float time = 0;
        Vec2 pos{};
    };
    LastClick m_lastClick;
    /// Appends `node`, its parent, ... up to the root (stopping at a node that is not alive).
    void pushPath(const NodeArena& arena, NodeId node);
    /// True while `entry.node` is alive and still the node recorded there.
    static bool stillThere(const NodeArena& arena, const PathEntry& entry);
};

}  // namespace xt
