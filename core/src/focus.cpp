#include <algorithm>
#include <cstddef>
#include <string_view>
#include <vector>

#include "event_dispatcher.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/node.h"

/// Focus model helpers (docs/design/08-events.md §8.5, docs/tasks/T-33.md "可 focus" / "Tab"):
/// which elements can take focus, the nearest focusable ancestor of a hit element, and the
/// document-order Tab ring. Pure queries over the arena; the focus itself lives in
/// EventDispatcher (event_dispatcher.cpp).
///
/// Not covered by docs/tasks/T-33.md (conservative choices made here):
///   - `tabindex`'s value is ignored (the sheet's first version: "只當作可 focus"), so the ring is
///     plain document order.
///   - `disabled` is read off the element itself only; display:none / visibility:hidden are
///     checked on the element and every ancestor, since a hidden subtree is not reachable.
///   - An `input` with an unknown `type` (neither text nor checkbox) is focusable but carries no
///     editing or toggling behavior.

namespace xt {

namespace {

/// StringId -> text: through `doc` when there is one (T-270a: a tag or attribute the host set is
/// interned into its runtime string pool), else the IR's own table.
std::string_view stringOf(const IrView& ir, const Document* doc, StringId id) {
    return doc != nullptr ? doc->string(id) : ir.string(id);
}

/// Linear search of `data.attributes` by name; empty view when absent.
std::string_view attribute(const IrView& ir, const Document* doc, const ElementData& data,
                           std::string_view name) {
    for (const auto& entry : data.attributes) {
        if (stringOf(ir, doc, entry.first) == name) {
            return stringOf(ir, doc, entry.second);
        }
    }
    return std::string_view{};
}

bool isElement(const NodeArena& arena, NodeId node) {
    return node != kInvalidNode && arena.alive(node) && arena.kind(node) == NodeKind::Element;
}

/// Pushes `node`'s children onto `stack` in reverse document order, so popping yields preorder.
void pushChildrenReversed(const NodeArena& arena, NodeId node, std::vector<NodeId>& stack) {
    size_t first = stack.size();
    NodeId child = arena.firstChild(node);
    while (child != kInvalidNode) {
        stack.push_back(child);
        child = arena.nextSibling(child);
    }
    std::reverse(stack.begin() + static_cast<ptrdiff_t>(first), stack.end());
}

/// Every focusable element under `root`, in document (preorder) order. Uses a local stack:
/// NodeArena's own traversal helpers are not re-entrant.
void collectFocusable(const NodeArena& arena, NodeId root, std::vector<NodeId>& out) {
    out.clear();
    if (!isElement(arena, root)) {
        return;
    }
    std::vector<NodeId> stack;
    stack.push_back(root);
    while (!stack.empty()) {
        NodeId node = stack.back();
        stack.pop_back();
        if (!isElement(arena, node)) {
            continue;
        }
        if (arena.element(node).style.display == Display::None) {
            continue;  // The whole subtree is unreachable.
        }
        if (isFocusable(arena, node)) {
            out.push_back(node);
        }
        pushChildrenReversed(arena, node, stack);
    }
}

}  // namespace

WidgetKind widgetKind(const NodeArena& arena, const IrView& ir, const Document* doc, NodeId node) {
    if (!isElement(arena, node)) {
        return WidgetKind::None;
    }
    const ElementData& data = arena.element(node);
    if (data.tag == kInvalidString) {
        return WidgetKind::None;
    }
    std::string_view tag = stringOf(ir, doc, data.tag);
    if (tag == "button") {
        return WidgetKind::Button;
    }
    if (tag != "input") {
        return WidgetKind::None;
    }
    std::string_view type = attribute(ir, doc, data, "type");
    if (type == "checkbox") {
        return WidgetKind::Checkbox;
    }
    if (type.empty() || type == "text") {
        return WidgetKind::TextInput;
    }
    return WidgetKind::OtherInput;
}

bool isFocusable(const NodeArena& arena, NodeId node) {
    if (!isElement(arena, node)) {
        return false;
    }
    const ElementData& data = arena.element(node);
    if (data.focusable == 0) {
        return false;
    }
    if ((data.stateBits & static_cast<uint16_t>(kStateDisabled)) != 0) {
        return false;
    }
    NodeId n = node;
    while (isElement(arena, n)) {
        const ElementData& d = arena.element(n);
        if (d.style.display == Display::None || d.style.visibility == Visibility::Hidden) {
            return false;
        }
        if ((d.stateBits & static_cast<uint16_t>(kStateDisabled)) != 0) {
            return false;  // T-390 (27.3.2): a disabled ancestor disables the subtree.
        }
        n = arena.parent(n);
    }
    return true;
}

NodeId nearestFocusable(const NodeArena& arena, NodeId node) {
    NodeId n = node;
    while (n != kInvalidNode) {
        if (isFocusable(arena, n)) {
            return n;
        }
        n = arena.parent(n);
    }
    return kInvalidNode;
}

NodeId nextFocusable(const NodeArena& arena, NodeId root, NodeId from, bool backward) {
    std::vector<NodeId> ring;
    collectFocusable(arena, root, ring);
    if (ring.empty()) {
        return kInvalidNode;
    }
    size_t count = ring.size();
    size_t current = count;  // "not in the ring".
    for (size_t i = 0; i < count; ++i) {
        if (ring[i] == from) {
            current = i;
            break;
        }
    }
    if (current == count) {
        return backward ? ring[count - 1] : ring[0];
    }
    size_t next = backward ? (current + count - 1) % count : (current + 1) % count;
    return ring[next];
}

NodeId firstTextChild(const NodeArena& arena, NodeId element) {
    if (!isElement(arena, element)) {
        return kInvalidNode;
    }
    NodeId child = arena.firstChild(element);
    while (child != kInvalidNode) {
        if (arena.kind(child) == NodeKind::Text) {
            return child;
        }
        child = arena.nextSibling(child);
    }
    return kInvalidNode;
}

}  // namespace xt
