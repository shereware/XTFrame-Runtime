#include "layer_collect.h"

#include <algorithm>

/// See docs/design/19-3d-space.md §19.6, docs/tasks/T-44a.md "layer_collect.h".

namespace xt::layer_collect {

void collectSections(const NodeArena& /*arena*/, std::span<const NodeId> orderedChildren,
                     std::span<const uint8_t> has3D, std::vector<Section>& out) {
    out.clear();
    uint32_t i = 0;
    uint32_t n = static_cast<uint32_t>(orderedChildren.size());
    while (i < n) {
        NodeId child = orderedChildren[i];
        bool h = child < has3D.size() && has3D[child] != 0;
        if (h) {
            uint32_t start = i;
            while (i < n && has3D[orderedChildren[i]] != 0) {
                ++i;
            }
            out.push_back(Section{start, i - start, true});
        } else {
            out.push_back(Section{i, 1, false});
            ++i;
        }
    }
}

void collectLayers(const NodeArena& arena, NodeId child, const Mat4& view, std::vector<LayerEntry>& out,
                   uint32_t& docOrderCounter) {
    // Manual explicit-stack preorder (rather than NodeArena::traversePreorder) so each frame can
    // carry the accumulated 2D-ancestor opacity product down to its children -- docs/tasks/
    // T-48.md "T-44b 遺留修正" 2's `inheritedAlpha`. Document order (needed for docOrder, the
    // sort's tiebreaker) is preserved the same way NodeArena::pushChildrenReversed does: push
    // children in reverse so they pop left-to-right.
    struct Frame {
        NodeId node;
        float alpha;  // Product of 2D ancestors' opacity from `child` down to (not including) node.
    };
    std::vector<Frame> stack;
    stack.push_back(Frame{child, 1.0f});
    std::vector<NodeId> kidsScratch;
    while (!stack.empty()) {
        Frame top = stack.back();
        stack.pop_back();
        NodeId n = top.node;
        if (arena.kind(n) == NodeKind::Camera) {
            continue;  // Not drawn/hit-tested (19.4); no layers of interest below it either.
        }
        const NodeCommon& c = arena.common(n);
        float childAlpha = top.alpha;
        if (c.is3D != 0) {
            Vec3 anchorPt{c.anchor.x, c.anchor.y, c.anchorZ};
            Mat4 vw = mat4Mul(view, c.world3);
            Vec3 depthPoint = mat4Transform(vw, anchorPt);
            out.push_back(LayerEntry{n, depthPoint.z, docOrderCounter, top.alpha});
            ++docOrderCounter;
            // The layer's own opacity is not a "2D ancestor" opacity (it is applied separately
            // when the layer itself is entered) -- childAlpha stays top.alpha.
        } else {
            childAlpha = top.alpha * c.opacity;
        }
        if (c.isSpaceRoot != 0) {
            continue;  // Nested root's subtree is a separate projection (19.6).
        }
        kidsScratch.clear();
        for (NodeId ch = arena.firstChild(n); ch != kInvalidNode; ch = arena.nextSibling(ch)) {
            kidsScratch.push_back(ch);
        }
        for (auto it = kidsScratch.rbegin(); it != kidsScratch.rend(); ++it) {
            stack.push_back(Frame{*it, childAlpha});
        }
    }
}

void sortLayers(std::vector<LayerEntry>& entries) {
    std::sort(entries.begin(), entries.end(), [](const LayerEntry& a, const LayerEntry& b) {
        if (a.depth != b.depth) {
            return a.depth < b.depth;
        }
        return a.docOrder < b.docOrder;
    });
}

void computeHas3D(const NodeArena& arena, NodeId root, std::vector<uint8_t>& has3D) {
    has3D.assign(arena.capacity(), 0);
    arena.traversePostorder(root, [&](NodeId n) {
        const NodeCommon& c = arena.common(n);
        uint8_t v = c.is3D != 0 ? 1 : 0;
        for (NodeId ch = arena.firstChild(n); ch != kInvalidNode; ch = arena.nextSibling(ch)) {
            if (arena.common(ch).isSpaceRoot != 0) {
                continue;  // Opaque: a nested root's subtree does not count toward its parent's has3D.
            }
            if (has3D[ch] != 0) {
                v = 1;
            }
        }
        has3D[n] = v;
    });
}

}  // namespace xt::layer_collect
