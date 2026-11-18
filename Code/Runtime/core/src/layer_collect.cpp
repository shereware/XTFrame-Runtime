#include "layer_collect.h"

#include <algorithm>

/// See Docs/design/19-3d-space.md §19.6, Docs/tasks/T-44a.md "layer_collect.h".

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
    // carry the accumulated 2D-ancestor opacity product down to its children -- Docs/tasks/
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
        if (c.worldLayerActive != 0) {
            continue;  // T-701 (33 §33.2): a world layer is painted and hit as a layer of its own.
        }
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

namespace {

/// T-727: would painting `n`'s flat content in more than one piece change what its group
/// composites? True for every group the Painter can open around a node's content: a Layer (opacity
/// < 1 or a blend mode), an effect chain (enabled effects) and a mask run (Mask children); also a
/// Group with Modifier children, which draws its shape children together on entry. Clips are not
/// listed: a clip pushed again around each piece clips the same pixels.
bool formsGroup(const NodeArena& arena, NodeId n) {
    const NodeCommon& c = arena.common(n);
    if (c.opacity < 1.0f || c.blend != static_cast<uint8_t>(BlendMode::Normal)) {
        return true;
    }
    const std::vector<EffectInstance>& pool = arena.effects();
    for (uint32_t i = 0; i < c.effectCount; ++i) {
        if (pool[c.effectOffset + i].enabled != 0) {
            return true;
        }
    }
    for (NodeId ch = arena.firstChild(n); ch != kInvalidNode; ch = arena.nextSibling(ch)) {
        const NodeKind kind = arena.kind(ch);
        if (kind == NodeKind::Mask || kind == NodeKind::Modifier) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool collectSegmentItems(const NodeArena& arena, NodeId child, const Mat4& view, std::vector<LayerEntry>& out,
                         uint32_t& docOrderCounter, std::vector<PieceRange>& ranges) {
    // The same explicit-stack preorder as collectLayers() (so the layers, their docOrder and their
    // inheritedAlpha come out exactly as there), with an exit marker after every flat node that has
    // children, where the node's PieceRange::last is known.
    struct Frame {
        NodeId node;
        float alpha;  // Product of 2D ancestors' opacity from `child` down to (not including) node.
        bool flat;    // Outside every 3D layer: flat content of the segment child.
        bool exit;    // The exit marker of a flat node.
    };
    const size_t firstItem = out.size();
    const NodeCommon& cc = arena.common(child);
    // §19.6's depth rule for a point of the root plane: the child's anchor, untransformed (z = 0).
    const float flatDepth =
        mat4Transform(mat4Mul(view, mat4FromMat2x3(cc.world)), Vec3{cc.anchor.x, cc.anchor.y, 0.0f}).z;

    std::vector<Frame> stack;
    stack.push_back(Frame{child, 1.0f, true, false});
    std::vector<NodeId> kidsScratch;
    uint32_t cut = 0;             // 3D layers met so far: the piece of the next flat node
    uint32_t lastFlat = 0;        // piece of the flat node met last
    uint32_t created = kNoPiece;  // last piece given an item (pieces only grow)
    uint32_t pieceCount = 0;
    bool splittable = true;
    auto flatNode = [&](NodeId n) {
        ranges[n] = PieceRange{cut, cut};
        lastFlat = cut;
        if (created != cut) {
            out.push_back(LayerEntry{child, flatDepth, docOrderCounter, 1.0f, cut});
            ++docOrderCounter;
            created = cut;
            ++pieceCount;
        }
    };
    while (!stack.empty()) {
        Frame top = stack.back();
        stack.pop_back();
        NodeId n = top.node;
        if (top.exit) {
            ranges[n].last = lastFlat;
            if (lastFlat > ranges[n].first && formsGroup(arena, n)) {
                splittable = false;
            }
            continue;
        }
        if (top.flat) {
            ranges[n] = PieceRange{};  // set below when the node is flat content
        }
        if (arena.kind(n) == NodeKind::Camera) {
            continue;  // Not drawn/hit-tested (19.4); no layers of interest below it either.
        }
        const NodeCommon& c = arena.common(n);
        if (c.worldLayerActive != 0) {
            // T-701: painted and hit as a layer of its own; met (and queued) where it stands.
            if (top.flat) {
                flatNode(n);
            }
            continue;
        }
        float childAlpha = top.alpha;
        bool childFlat = top.flat;
        if (c.is3D != 0) {
            Vec3 anchorPt{c.anchor.x, c.anchor.y, c.anchorZ};
            Vec3 depthPoint = mat4Transform(mat4Mul(view, c.world3), anchorPt);
            out.push_back(LayerEntry{n, depthPoint.z, docOrderCounter, top.alpha});
            ++docOrderCounter;
            ++cut;
            childFlat = false;
        } else {
            childAlpha = top.alpha * c.opacity;
            if (top.flat) {
                flatNode(n);
                if (c.isSpaceRoot == 0 && arena.firstChild(n) != kInvalidNode) {
                    stack.push_back(Frame{n, 0.0f, true, true});
                }
            }
        }
        if (c.isSpaceRoot != 0) {
            continue;  // Nested root's subtree is a separate projection (19.6).
        }
        kidsScratch.clear();
        for (NodeId ch = arena.firstChild(n); ch != kInvalidNode; ch = arena.nextSibling(ch)) {
            kidsScratch.push_back(ch);
        }
        for (auto it = kidsScratch.rbegin(); it != kidsScratch.rend(); ++it) {
            stack.push_back(Frame{*it, childAlpha, childFlat, false});
        }
    }
    if (splittable || pieceCount == 0) {
        return true;
    }
    out.erase(std::remove_if(out.begin() + static_cast<std::ptrdiff_t>(firstItem), out.end(),
                             [](const LayerEntry& e) { return e.piece != kLayerItem; }),
              out.end());
    return false;
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
        uint8_t v = (c.is3D != 0 && c.worldLayerActive == 0) ? 1 : 0;
        for (NodeId ch = arena.firstChild(n); ch != kInvalidNode; ch = arena.nextSibling(ch)) {
            if (arena.common(ch).isSpaceRoot != 0 || arena.common(ch).worldLayerActive != 0) {
                // Opaque: a nested root's subtree does not count toward its parent's has3D, and
                // (T-701) neither does a world layer's.
                continue;
            }
            if (has3D[ch] != 0) {
                v = 1;
            }
        }
        has3D[n] = v;
    });
}

}  // namespace xt::layer_collect
