#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "event_dispatcher.h"
#include "xt/ir.h"
#include "xt/node.h"

/// Directional focus navigation (Docs/design/08-events.md §8.9, 27 §27.7, Docs/tasks/T-194.md
/// goal 1): `focus-nav: auto | explicit | none`, `nav:<dir>="#id"`, the geometric scoring of
/// `auto` and the `kFocusTrap` scope. Pure queries over the arena, like focus.cpp.
///
/// Geometric scoring (not specified beyond "direction cone + distance + alignment" in 08.9;
/// chosen here). Rectangles are the world-space AABBs of the border boxes; along the requested
/// direction `major` is the centre-to-centre offset, `gap` the distance from the current
/// element's far edge to the candidate's near edge (0 when they overlap), `orthoGap` the gap
/// between the two rectangles on the perpendicular axis (0 when their spans overlap) and
/// `orthoCenter` the perpendicular centre-to-centre offset:
///   - eligible: major > 0.5 px and (orthoGap == 0 or orthoCenter <= 2 * major), i.e. the
///     candidate lies ahead, inside a cone of about 63 degrees half-angle, or directly in line;
///   - score = gap + 4 * orthoGap + 0.5 * orthoCenter; the lowest score wins, ties keep document
///     order.
/// Not covered by the design (conservative choices made here):
///   - `focus-nav: none` removes an element (and, inherited, its subtree) from the candidates;
///     navigating *from* such an element (focused by pointer or API) still scores geometrically.
///   - An explicit `nav:<dir>` target that is not focusable falls back to geometric scoring under
///     `auto` and blocks under `explicit`. `explicit` without a `nav:<dir>` blocks.
///   - With nothing focused, navigation focuses the first `autofocus` element, else the first
///     focusable element in document order (inside the scope).
///   - Elements inside a 3D space root use their 2D world transform (no projection).

namespace xt {

namespace {

bool isElementNode(const NodeArena& arena, NodeId node) {
    return node != kInvalidNode && arena.alive(node) && arena.kind(node) == NodeKind::Element;
}

/// World-space AABB of `node`'s border box.
Rect worldRect(const NodeArena& arena, NodeId node) {
    const ElementData& data = arena.element(node);
    const Mat2x3& world = arena.common(node).world;
    const Vec2 corners[4] = {apply(world, Vec2{0, 0}), apply(world, Vec2{data.box.width, 0}),
                             apply(world, Vec2{0, data.box.height}),
                             apply(world, Vec2{data.box.width, data.box.height})};
    float x0 = corners[0].x;
    float y0 = corners[0].y;
    float x1 = x0;
    float y1 = y0;
    for (const Vec2& c : corners) {
        x0 = std::min(x0, c.x);
        y0 = std::min(y0, c.y);
        x1 = std::max(x1, c.x);
        y1 = std::max(y1, c.y);
    }
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

/// True when `node` may be a navigation target: focusable and not under `focus-nav: none`.
bool navCandidate(const NodeArena& arena, NodeId node) {
    return isFocusable(arena, node) && arena.element(node).style.focusNav != 2;
}

/// Preorder list of the navigation candidates under `scope` (display:none subtrees skipped).
void collectCandidates(const NodeArena& arena, NodeId scope, std::vector<NodeId>& out) {
    out.clear();
    if (!isElementNode(arena, scope)) {
        return;
    }
    std::vector<NodeId> stack{scope};
    while (!stack.empty()) {
        NodeId node = stack.back();
        stack.pop_back();
        if (!isElementNode(arena, node) || arena.element(node).style.display == Display::None) {
            continue;
        }
        if (navCandidate(arena, node)) {
            out.push_back(node);
        }
        const size_t first = stack.size();
        for (NodeId c = arena.firstChild(node); c != kInvalidNode; c = arena.nextSibling(c)) {
            stack.push_back(c);
        }
        std::reverse(stack.begin() + static_cast<std::ptrdiff_t>(first), stack.end());
    }
}

uint32_t navSlot(const NodeCommon& c, NavDirection dir) {
    switch (dir) {
        case NavDirection::Up:
            return c.navUp;
        case NavDirection::Down:
            return c.navDown;
        case NavDirection::Left:
            return c.navLeft;
        case NavDirection::Right:
            return c.navRight;
    }
    return 0;
}

/// The element under `scope` whose id is `id` (preorder, first match), or kInvalidNode.
NodeId elementWithId(const NodeArena& arena, NodeId scope, StringId id) {
    std::vector<NodeId> stack{scope};
    while (!stack.empty()) {
        NodeId node = stack.back();
        stack.pop_back();
        if (!isElementNode(arena, node)) {
            continue;
        }
        if (arena.element(node).id == id) {
            return node;
        }
        for (NodeId c = arena.firstChild(node); c != kInvalidNode; c = arena.nextSibling(c)) {
            stack.push_back(c);
        }
    }
    return kInvalidNode;
}

}  // namespace

NodeId focusScope(const NodeArena& arena, NodeId root, NodeId from) {
    for (NodeId n = from; isElementNode(arena, n); n = arena.parent(n)) {
        if ((arena.common(n).flags2 & kFocusTrap) != 0) {
            return n;
        }
    }
    return root;
}

float navScore(Rect from, Rect to, NavDirection dir, bool& eligible) {
    const float fromCx = from.x + from.w * 0.5f;
    const float fromCy = from.y + from.h * 0.5f;
    const float toCx = to.x + to.w * 0.5f;
    const float toCy = to.y + to.h * 0.5f;
    float major = 0;
    float gap = 0;
    float orthoGap = 0;
    float orthoCenter = 0;
    const bool vertical = dir == NavDirection::Up || dir == NavDirection::Down;
    switch (dir) {
        case NavDirection::Right:
            major = toCx - fromCx;
            gap = to.x - (from.x + from.w);
            break;
        case NavDirection::Left:
            major = fromCx - toCx;
            gap = from.x - (to.x + to.w);
            break;
        case NavDirection::Down:
            major = toCy - fromCy;
            gap = to.y - (from.y + from.h);
            break;
        case NavDirection::Up:
            major = fromCy - toCy;
            gap = from.y - (to.y + to.h);
            break;
    }
    gap = std::max(0.0f, gap);
    if (vertical) {
        orthoGap = std::max(0.0f, std::max(from.x, to.x) - std::min(from.x + from.w, to.x + to.w));
        orthoCenter = std::fabs(toCx - fromCx);
    } else {
        orthoGap = std::max(0.0f, std::max(from.y, to.y) - std::min(from.y + from.h, to.y + to.h));
        orthoCenter = std::fabs(toCy - fromCy);
    }
    eligible = major > 0.5f && (orthoGap == 0.0f || orthoCenter <= 2.0f * major);
    return gap + 4.0f * orthoGap + 0.5f * orthoCenter;
}

NodeId firstNavTarget(const NodeArena& arena, NodeId scope, const std::vector<NodeId>& autofocus) {
    for (NodeId n : autofocus) {
        if (navCandidate(arena, n) && (n == scope || arena.isAncestorOf(scope, n))) {
            return n;
        }
    }
    std::vector<NodeId> candidates;
    collectCandidates(arena, scope, candidates);
    return candidates.empty() ? kInvalidNode : candidates.front();
}

NodeId findNavTarget(const NodeArena& arena, NodeId root, NodeId from, NavDirection dir,
                     const std::vector<NodeId>& autofocus) {
    if (!isElementNode(arena, root)) {
        return kInvalidNode;
    }
    const NodeId scope = focusScope(arena, root, from);
    if (!isElementNode(arena, from)) {
        return firstNavTarget(arena, scope, autofocus);
    }
    const uint8_t mode = arena.element(from).style.focusNav;
    const uint32_t slot = navSlot(arena.common(from), dir);
    if (slot != 0) {
        NodeId target = elementWithId(arena, root, static_cast<StringId>(slot - 1));
        if (target != kInvalidNode && target != from && isFocusable(arena, target)) {
            return target;
        }
    }
    if (mode == 1) {
        return kInvalidNode;  // `explicit`: only nav:<dir> moves the focus.
    }
    std::vector<NodeId> candidates;
    collectCandidates(arena, scope, candidates);
    const Rect fromRect = worldRect(arena, from);
    NodeId best = kInvalidNode;
    float bestScore = 0;
    for (NodeId n : candidates) {
        if (n == from || arena.isAncestorOf(n, from) || arena.isAncestorOf(from, n)) {
            continue;  // Nested focusables overlap their container; never a direction.
        }
        bool eligible = false;
        const float score = navScore(fromRect, worldRect(arena, n), dir, eligible);
        if (eligible && (best == kInvalidNode || score < bestScore)) {
            best = n;
            bestScore = score;
        }
    }
    return best;
}

// -- EventDispatcher: navigation, gamepad, gestures (T-194) ------------------------------------

namespace {

void markDirtySubtree(NodeArena& arena, NodeId node) {
    arena.markDirty(node, static_cast<uint8_t>(kDirtyStyle));
    arena.traversePreorder(node, [&arena](NodeId n) {
        arena.markDirty(n, static_cast<uint8_t>(kDirtyStyle));
        return true;
    });
}

/// Same as event_dispatcher.cpp's helper (a local copy: that one is file-private).
void setStateBit(NodeArena& arena, NodeId node, uint16_t bit, bool on) {
    if (!isElementNode(arena, node)) {
        return;
    }
    ElementData& data = arena.element(node);
    if (((data.stateBits & bit) != 0) == on) {
        return;
    }
    data.stateBits =
        on ? static_cast<uint16_t>(data.stateBits | bit) : static_cast<uint16_t>(data.stateBits & ~bit);
    markDirtySubtree(arena, node);
}

/// T-390 (27.3.2): `node` or an element ancestor is disabled (the state is subtree-wide).
bool disabledElement(const NodeArena& arena, NodeId node) {
    for (NodeId n = node; isElementNode(arena, n); n = arena.parent(n)) {
        if ((arena.element(n).stateBits & static_cast<uint16_t>(kStateDisabled)) != 0) {
            return true;
        }
    }
    return false;
}

/// `node`'s border-box centre in viewport coordinates.
Vec2 centerOf(const NodeArena& arena, NodeId node) {
    const ElementData& data = arena.element(node);
    return apply(arena.common(node).world, Vec2{data.box.width * 0.5f, data.box.height * 0.5f});
}

bool padDirection(GamepadButton button, NavDirection& dir) {
    switch (button) {
        case GamepadButton::DpadUp:
            dir = NavDirection::Up;
            return true;
        case GamepadButton::DpadDown:
            dir = NavDirection::Down;
            return true;
        case GamepadButton::DpadLeft:
            dir = NavDirection::Left;
            return true;
        case GamepadButton::DpadRight:
            dir = NavDirection::Right;
            return true;
        default:
            return false;
    }
}

constexpr float kStickPress = 0.5f;    // A stick direction starts past this deflection...
constexpr float kStickRelease = 0.3f;  // ...and ends below this one (hysteresis).
constexpr float kLongPress = 0.5f;     // Seconds of A held on a draggable before a gamepad drag.

}  // namespace

bool EventDispatcher::navigate(NodeArena& arena, NodeId root, NavDirection dir, uint8_t player) {
    if (player >= kMaxPlayers || !isElementNode(arena, root)) {
        return false;
    }
    NodeId from = m_focus[player];
    if (from != kInvalidNode && !isElementNode(arena, from)) {
        from = kInvalidNode;
    }
    // T-201 (27.7): left / right on a focused `draggable="axis-x"` element adjust it instead.
    if (from != kInvalidNode && (arena.common(from).flags2 & kDragAxisX) != 0 &&
        (dir == NavDirection::Left || dir == NavDirection::Right) && m_triggerSink != nullptr) {
        m_triggerSink->onAxisValue(from, true, dir == NavDirection::Left ? -1.0f : 1.0f, player);
        return true;
    }
    const NodeId target = findNavTarget(arena, root, from, dir, m_autofocus);
    if (target != kInvalidNode) {
        setFocus(arena, target, player);
        return true;
    }
    Event ev;
    ev.type = EventType::NavBlocked;
    ev.target = from != kInvalidNode ? from : root;
    ev.direction = dir;
    ev.player = player;
    dispatch(arena, ev);
    return false;
}

void EventDispatcher::tick(NodeArena& arena, NodeId root, float time) {
    m_time = time;
    autoScroll(arena, root, time);  // T-388 (08.12): a pointer drag at a scroll container's edge.
    const uint8_t saved = m_curPlayer;
    for (uint8_t p = 0; p < kMaxPlayers; ++p) {
        PadState& pad = m_pads[p];
        m_curPlayer = p;
        if (pad.holdDir >= 0 && time >= pad.nextRepeat) {
            pad.nextRepeat = std::max(pad.nextRepeat + m_repeatInterval, time);
            navigate(arena, root, static_cast<NavDirection>(pad.holdDir), p);
        }
        if (pad.aNode != kInvalidNode && !pad.aDragged && !m_drag.active && time - pad.aTime >= kLongPress) {
            NodeId source = kInvalidNode;
            for (NodeId n = pad.aNode; isElementNode(arena, n); n = arena.parent(n)) {
                if ((arena.common(n).flags2 & kDraggable) != 0) {
                    source = n;
                    break;
                }
            }
            if (source != kInvalidNode && !disabledElement(arena, source)) {
                pad.aDragged = true;
                clearActiveChain(arena, pad.aChain);
                startDrag(arena, source, p, /*gamepad=*/true, centerOf(arena, source));
            }
        }
    }
    m_curPlayer = saved;
}

void EventDispatcher::clearActiveChain(NodeArena& arena, std::vector<NodeId>& chain) {
    for (NodeId n : chain) {
        setStateBit(arena, n, static_cast<uint16_t>(kStateActive), false);
    }
    chain.clear();
}

void EventDispatcher::handleGamepadButton(NodeArena& arena, NodeId root, const InputEvent& input, bool down) {
    const uint8_t p = m_curPlayer;
    PadState& pad = m_pads[p];
    NavDirection dir = NavDirection::Up;
    if (padDirection(input.gamepadButton, dir)) {
        if (down) {
            pad.holdDir = static_cast<int8_t>(dir);
            pad.holdFromStick = false;
            pad.nextRepeat = m_time + m_repeatDelay;
            navigate(arena, root, dir, p);
        } else if (pad.holdDir == static_cast<int8_t>(dir) && !pad.holdFromStick) {
            pad.holdDir = -1;
        }
        return;
    }
    switch (input.gamepadButton) {
        case GamepadButton::A:
            if (down) {
                padPress(arena, root, p);
            } else {
                padRelease(arena, p);
            }
            return;
        case GamepadButton::B:
            if (!down) {
                return;
            }
            if (m_drag.active && m_drag.gamepad && m_drag.player == p) {
                cancelDrag(arena);  // B cancels a gamepad drag instead of going back.
                return;
            }
            dispatchBack(arena, root, p);
            return;
        case GamepadButton::LeftShoulder:
        case GamepadButton::RightShoulder: {
            if (!down) {
                return;
            }
            const NodeId from = m_focus[p];
            const NodeId next = nextFocusable(arena, focusScope(arena, root, from), from,
                                              input.gamepadButton == GamepadButton::LeftShoulder);
            setFocus(arena, next, p);
            return;
        }
        default:
            return;
    }
}

void EventDispatcher::handleGamepadAxis(NodeArena& arena, NodeId root, const InputEvent& input) {
    const uint8_t p = m_curPlayer;
    PadState& pad = m_pads[p];
    if (input.gamepadAxis == GamepadAxis::LeftX) {
        pad.stickX = input.axisValue;
    } else if (input.gamepadAxis == GamepadAxis::LeftY) {
        pad.stickY = input.axisValue;
    } else {
        return;
    }
    int8_t dir = -1;
    if (pad.stickDir >= 0) {
        // Keep the current direction while its component stays above the release threshold.
        const auto d = static_cast<NavDirection>(pad.stickDir);
        const float along = d == NavDirection::Right  ? pad.stickX
                            : d == NavDirection::Left ? -pad.stickX
                            : d == NavDirection::Down ? pad.stickY
                                                      : -pad.stickY;
        if (along >= kStickRelease) {
            dir = pad.stickDir;
        }
    }
    if (dir < 0 && std::max(std::fabs(pad.stickX), std::fabs(pad.stickY)) >= kStickPress) {
        const bool horizontal = std::fabs(pad.stickX) >= std::fabs(pad.stickY);
        const NavDirection d = horizontal ? (pad.stickX > 0 ? NavDirection::Right : NavDirection::Left)
                                          : (pad.stickY > 0 ? NavDirection::Down : NavDirection::Up);
        dir = static_cast<int8_t>(d);
    }
    if (dir == pad.stickDir) {
        return;
    }
    pad.stickDir = dir;
    if (dir >= 0) {
        pad.holdDir = dir;
        pad.holdFromStick = true;
        pad.nextRepeat = m_time + m_repeatDelay;
        navigate(arena, root, static_cast<NavDirection>(dir), p);
    } else if (pad.holdFromStick) {
        pad.holdDir = -1;
    }
}

void EventDispatcher::padPress(NodeArena& arena, NodeId root, uint8_t player) {
    PadState& pad = m_pads[player];
    if (m_drag.active && m_drag.gamepad && m_drag.player == player) {
        finishDrag(arena, m_drag.over != kInvalidNode);  // A drops a gamepad drag.
        pad.aSuppressed = true;
        return;
    }
    NodeId node = m_focus[player];
    if (!isElementNode(arena, node)) {
        // Nothing focused yet: A focuses the first target instead of pressing.
        setFocus(arena, firstNavTarget(arena, root, m_autofocus), player);
        pad.aSuppressed = true;
        return;
    }
    pad.aNode = node;
    pad.aTime = m_time;
    pad.aDragged = false;
    pad.aSuppressed = false;
    pad.aChain.clear();
    if (!disabledElement(arena, node)) {
        for (NodeId n = node; n != kInvalidNode; n = arena.parent(n)) {
            pad.aChain.push_back(n);
            setStateBit(arena, n, static_cast<uint16_t>(kStateActive), true);
        }
    }
    const Vec2 center = centerOf(arena, node);
    Event ev;
    ev.type = EventType::PointerDown;
    ev.target = node;
    ev.pos = center;
    ev.localPos = Vec2{arena.element(node).box.width * 0.5f, arena.element(node).box.height * 0.5f};
    ev.player = player;
    dispatch(arena, ev);
}

void EventDispatcher::padRelease(NodeArena& arena, uint8_t player) {
    PadState& pad = m_pads[player];
    if (pad.aSuppressed) {
        pad.aSuppressed = false;
        return;
    }
    const NodeId node = pad.aNode;
    pad.aNode = kInvalidNode;
    clearActiveChain(arena, pad.aChain);
    if (!isElementNode(arena, node) || pad.aDragged) {
        pad.aDragged = false;
        return;  // The long press became a drag.
    }
    const Vec2 center = centerOf(arena, node);
    const Vec2 local{arena.element(node).box.width * 0.5f, arena.element(node).box.height * 0.5f};
    Event ev;
    ev.type = EventType::PointerUp;
    ev.target = node;
    ev.pos = center;
    ev.localPos = local;
    ev.player = player;
    dispatch(arena, ev);
    if (isElementNode(arena, node) && !disabledElement(arena, node)) {
        activate(arena, node, center, local);  // Click (+ checkbox toggle), like Enter.
    }
}

void EventDispatcher::dispatchBack(NodeArena& arena, NodeId root, uint8_t player) {
    const NodeId focus = m_focus[player];
    Event ev;
    ev.type = EventType::Back;
    ev.target = isElementNode(arena, focus) ? focus : root;
    ev.player = player;
    dispatch(arena, ev);
}

void EventDispatcher::handleGesture(NodeArena& arena, NodeId root, const InputEvent& input) {
    const HitResult hit = hitTestDetailed(arena, root, input.pos, HitMode::Press);
    Event ev;
    ev.type = EventType::Gesture;
    ev.target = hit.node != kInvalidNode ? hit.node : root;
    ev.pos = input.pos;
    ev.localPos = hit.localPos;
    ev.gesture = input.gesture;
    ev.direction = input.direction;
    ev.gestureScale = input.gestureScale;
    dispatch(arena, ev);
}

}  // namespace xt
