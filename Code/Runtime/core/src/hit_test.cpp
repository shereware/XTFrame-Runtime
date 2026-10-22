#include <algorithm>
#include <vector>

#include "camera.h"
#include "clip_shape.h"
#include "event_dispatcher.h"
#include "layer_collect.h"
#include "xt/node_arena.h"
#include "xt/painter.h"
#include "xt/project3d.h"
#include "xt/types.h"

/// hitTest(): depth-first, same-layer-reverse hit test over the Element tree. See
/// Docs/design/08-events.md §8.2, Docs/tasks/T-32.md "命中測試", and (3D roots)
/// Docs/design/19-3d-space.md §19.9, Docs/tasks/T-48.md "命中規則".

namespace xt {

namespace {

/// Everything needed to test `node` and its children, or nullopt (via the `ok` out-param) if
/// `node`'s whole subtree must be skipped (display:none, a `<camera>` element, a singular world
/// transform, or -- when node itself clips overflow -- `pos` landing outside node's own padding
/// box). Does not itself test node's own border box (the caller does that once no child produced
/// a hit). `pos` is in `node`'s *own* coordinate space -- for a plain 2D node this is the same
/// constant used for the whole enclosing plane's descendants (08.2's existing "one world-space
/// pos, reused down the whole subtree" rule; see world_transform.cpp: a 2D node's `world` is the
/// full chain from the nearest enclosing 3D plane, or from the document root outside any 3D
/// root), and for a node being entered as a 3D space root or a 3D layer's own plane origin it is
/// that plane's local (box/layer) coordinate -- s or q (19.9).
struct HitFrame {
    NodeId node = kInvalidNode;
    Vec2 localPos{};
    float width = 0;
    float height = 0;
    bool selfEligible = false;  // !visibility:hidden && !pointer-events:none
    std::vector<NodeId> children;
    /// T-386 (05 §5.9.11): the rectangle the node itself is hit in -- its border box, grown by
    /// `hit-slop` unless a `clip-path` shapes the hit area.
    Rect hitRect{};
};

/// T-386 (05 §5.9.11): `data`'s border box grown by its `hit-slop` (ComputedStyle::hitSlop: px,
/// or -fraction of the border-box width / height). The plain border box with a `clip-path`.
Rect hitRectOf(const ElementData& data) {
    const float w = data.box.width;
    const float h = data.box.height;
    Rect r{0, 0, w, h};
    if (data.clipShape.kind != ClipShapeKind::None) {
        return r;  // A shaped hit area is not grown (05 §5.9.11).
    }
    const float* s = data.style.hitSlop;
    if (s[0] == 0.0f && s[1] == 0.0f && s[2] == 0.0f && s[3] == 0.0f) {
        return r;
    }
    const auto side = [](float v, float size) { return v >= 0.0f ? v : -v * size; };
    const float top = side(s[0], h);
    const float right = side(s[1], w);
    const float bottom = side(s[2], h);
    const float left = side(s[3], w);
    return Rect{-left, -top, w + left + right, h + top + bottom};
}

bool tryEnter(const NodeArena& arena, NodeId node, Vec2 pos, HitMode mode, HitFrame& out) {
    const ElementData& data = arena.element(node);
    // Docs/design/19-3d-space.md §19.4/§19.9: a <camera> element does not hit test (its 3D ray/
    // plane intersection is handled separately, from within hit3DRoot() below).
    if (data.style.display == Display::None || data.cameraNode != kInvalidNode) {
        return false;
    }
    const NodeCommon& c = arena.common(node);
    Mat2x3 inv{};
    if (!invert(c.world, inv)) {
        return false;  // Singular transform: no point in this node's local space is well-defined.
    }
    Vec2 localPos = apply(inv, pos);
    const Rect hitRect = hitRectOf(data);
    bool selfOnly = false;
    if (clipsOverflow(data)) {  // T-336: the same test as the Painter's clip.
        Rect padding = data.box.paddingBox();
        if (!contains(padding, localPos)) {
            // Outside the clip: the whole subtree is unreachable. T-386: the node itself still is
            // inside its `hit-slop` rectangle.
            const bool grown = hitRect.x != 0.0f || hitRect.y != 0.0f || hitRect.w != data.box.width ||
                               hitRect.h != data.box.height;
            if (!grown || !contains(hitRect, localPos)) {
                return false;
            }
            selfOnly = true;
        }
    }
    // 08 §8.2 (2026-10-14 T-332 revision): an element with a `clip-path` is entered only when the
    // point lies inside the shape (border-box space, after the element's own transform); outside
    // it neither the element nor its subtree can be hit.
    if (data.clipShape.kind != ClipShapeKind::None &&
        !clip_shape::contains(data.clipShape, Rect{0, 0, data.box.width, data.box.height}, localPos)) {
        return false;
    }
    out.node = node;
    out.localPos = localPos;
    out.width = data.box.width;
    out.height = data.box.height;
    out.hitRect = hitRect;
    out.selfEligible = data.style.visibility != Visibility::Hidden &&
                       data.style.pointerEvents != PointerEvents::None &&
                       // T-174 (08.9): a `pointer-watch` element takes part in hover/move only; a press
                       // passes through it to whatever is below.
                       !(mode == HitMode::Press && (c.flags2 & kPointerWatch) != 0);
    if (selfOnly) {
        out.children.clear();
    } else {
        sortedElementChildren(arena, node, out.children);
    }
    return true;
}

/// T-255b: `depth` is `node`'s depth below the hit test's root; like layout, the descent stops
/// at kMaxElementDepth (a backstop: IrView::validate and Document's insert API keep trees shallower).
HitResult hitNode(const NodeArena& arena, NodeId node, Vec2 pos, HitMode mode, bool in3D = false,
                  uint32_t depth = 0);

/// Docs/design/19-3d-space.md §19.9 / Docs/tasks/T-48.md's `hitNode` pseudocode's 3D branch: `s`
/// is `frame`'s already-computed local position (the root's own box/plane coordinate). Returns
/// the hit found among the root's sorted candidates (near-to-far reversed within each sorted
/// segment, topmost section first), or an invalid HitResult if none of them hit.
HitResult hit3DRoot(const NodeArena& arena, NodeId root, const HitFrame& frame, HitMode mode,
                    uint32_t depth) {
    Vec2 s = frame.localPos;

    // Same three-way camera setup as camera.h's rootCameraSetup()/Painter::emitRoot(), but with
    // the extra branch information (explicit camera kind / CSS perspective vs. none) T-48's ray
    // construction needs (see 19.9's three ray formulas).
    enum class RayKind { Perspective, CssDefault, OrthoOrIdentity };
    NodeId camNode = findCameraChild(arena, root);
    Rect box = spaceRootBoxOf(arena, root);
    CameraSetup setup{};
    RayKind rayKind = RayKind::OrthoOrIdentity;
    float cssD = 0.0f;
    Vec2 cssOrigin{};
    if (camNode != kInvalidNode) {
        setup = computeCameraViewProj(arena, camNode, box);
        rayKind = arena.camera(camNode).kind == 0 ? RayKind::Perspective : RayKind::OrthoOrIdentity;
    } else if (arena.kind(root) == NodeKind::Element && arena.element(root).perspectivePx > 0.0f) {
        const ElementData& data = arena.element(root);
        setup = cssPerspectiveViewProj(data.perspectivePx, data.perspectiveOriginPx);
        rayKind = RayKind::CssDefault;
        cssD = data.perspectivePx;
        cssOrigin = data.perspectiveOriginPx;
    } else if (arena.kind(root) == NodeKind::Element) {
        setup = cssPerspectiveViewProj(0.0f, Vec2{});  // perspective: none -> identity.
        rayKind = RayKind::OrthoOrIdentity;
    } else {
        setup = defaultSceneViewProj(box.w, box.h);  // AE default camera is always Perspective.
        rayKind = RayKind::Perspective;
    }

    Vec3 p0{};
    Vec3 p1{};
    bool rayOk = true;
    switch (rayKind) {
        case RayKind::Perspective: {
            Mat4 inv{};
            if (!mat4Invert(setup.viewProj, inv)) {
                rayOk = false;
                break;
            }
            float w0 = 0;
            float w1 = 0;
            p0 = mat4Project(inv, Vec3{s.x, s.y, -1.0f}, &w0);
            p1 = mat4Project(inv, Vec3{s.x, s.y, 1.0f}, &w1);
            if (w0 <= 1e-6f || w1 <= 1e-6f) {
                rayOk = false;
            }
            break;
        }
        case RayKind::CssDefault:
            p0 = Vec3{cssOrigin.x, cssOrigin.y, cssD};
            p1 = Vec3{s.x, s.y, 0.0f};
            break;
        case RayKind::OrthoOrIdentity:
            p0 = Vec3{s.x, s.y, 1e6f};
            p1 = Vec3{s.x, s.y, 0.0f};
            break;
    }
    if (!rayOk) {
        return HitResult{};
    }

    // §19.6's sectioning, identical inputs to Painter::emitRoot() (T-44b), via the same shared
    // layer_collect.h used by Painter. `camNode` (looked up above) is excluded, like emitRoot()'s
    // own m_childScratch construction.
    std::vector<NodeId> orderedChildren;
    sortedElementChildren(arena, root, orderedChildren);
    if (camNode != kInvalidNode) {
        orderedChildren.erase(std::remove(orderedChildren.begin(), orderedChildren.end(), camNode),
                              orderedChildren.end());
    }
    std::vector<uint8_t> has3D;
    layer_collect::computeHas3D(arena, root, has3D);
    std::vector<layer_collect::Section> sections;
    layer_collect::collectSections(arena, orderedChildren, has3D, sections);

    std::vector<layer_collect::LayerEntry> layers;
    for (size_t i = sections.size(); i-- > 0;) {
        const layer_collect::Section& sec = sections[i];
        if (!sec.sorted) {
            // §19.9: "2D 段 -> 既有 2D 命中（posInPlane = s）".
            NodeId child = orderedChildren[sec.firstChild];
            HitResult hit = hitNode(arena, child, s, mode, /*in3D=*/true, depth + 1);
            if (hit.node != kInvalidNode) {
                return hit;
            }
            continue;
        }
        layers.clear();
        uint32_t docOrder = 0;
        for (uint32_t k = 0; k < sec.childCount; ++k) {
            NodeId child = orderedChildren[sec.firstChild + k];
            layer_collect::collectLayers(arena, child, setup.view, layers, docOrder);
        }
        layer_collect::sortLayers(layers);          // Ascending depth: farthest first.
        for (size_t j = layers.size(); j-- > 0;) {  // Nearest first (§19.9: "近者優先").
            const layer_collect::LayerEntry& layer = layers[j];
            const NodeCommon& lc = arena.common(layer.node);
            if (lc.backfaceHidden != 0) {
                project3d::Mapping mapping;
                mapping.hasProjection = true;
                mapping.viewProj = setup.viewProj;
                mapping.rootXf = arena.common(root).world;
                mapping.planeWorld3 = lc.world3;
                std::vector<project3d::Mapping> stack{mapping};
                Vec2 anchor{lc.anchor.x, lc.anchor.y};
                if (project3d::isBackface(stack, Mat2x3{}, anchor)) {
                    continue;
                }
            }
            Mat4 inv3{};
            if (!mat4Invert(lc.world3, inv3)) {
                continue;
            }
            Vec3 q0 = mat4Transform(inv3, p0);
            Vec3 q1 = mat4Transform(inv3, p1);
            if (q0.z == q1.z) {
                continue;  // Parallel to the layer's plane.
            }
            float t = q0.z / (q0.z - q1.z);
            Vec2 q{q0.x + (q1.x - q0.x) * t, q0.y + (q1.y - q0.y) * t};
            HitResult hit = hitNode(arena, layer.node, q, mode, /*in3D=*/true, depth + 1);
            if (hit.node != kInvalidNode) {
                return hit;
            }
        }
        // Docs/tasks/T-48b.md: the sorted segment's own root-plane 2D content (T-44b Painter's
        // PushTransform3D{I} content) is drawn *below* every one of its 3D layers, so it is hit
        // tested last, in the same same-layer-reverse order as an ordinary 2D segment, at `s`.
        // A child that is itself a 3D layer (non-root) was already covered by the layers loop
        // above -- entering it here with `s` (the section's root-plane coordinate, not that
        // layer's own plane coordinate `q`) would be meaningless, so it is skipped.
        for (size_t k = sec.childCount; k-- > 0;) {
            NodeId child = orderedChildren[sec.firstChild + k];
            const NodeCommon& cc = arena.common(child);
            if (cc.is3D != 0 && cc.isSpaceRoot == 0) {
                continue;
            }
            HitResult hit2d = hitNode(arena, child, s, mode, /*in3D=*/true, depth + 1);
            if (hit2d.node != kInvalidNode) {
                return hit2d;
            }
        }
    }
    return HitResult{};
}

/// One node's worth of the depth-first descent, recursive so a nested 3D space root's subtree can
/// switch back to hit3DRoot() at any depth. `pos` is in `node`'s own coordinate space (see
/// HitFrame's comment above: constant across an entire plane's descendants, changing only at a
/// plane boundary -- entering a 3D root's own plane, or a 3D layer's plane).
HitResult hitNode(const NodeArena& arena, NodeId node, Vec2 pos, HitMode mode, bool in3D, uint32_t depth) {
    if (depth >= kMaxElementDepth || !arena.alive(node) || arena.kind(node) != NodeKind::Element) {
        return HitResult{};
    }
    HitFrame frame;
    if (!tryEnter(arena, node, pos, mode, frame)) {
        return HitResult{};
    }
    if (arena.common(node).isSpaceRoot != 0) {
        HitResult hit = hit3DRoot(arena, node, frame, mode, depth);
        if (hit.node != kInvalidNode) {
            return hit;
        }
        if (frame.selfEligible && contains(frame.hitRect, frame.localPos)) {  // T-386: hit-slop.
            return HitResult{node, frame.localPos, (arena.common(node).flags2 & kPointerWatch) != 0};
        }
        return HitResult{};
    }
    // Same-layer-reverse (topmost/last-painted child first, 08.2), same `pos` reused for every
    // child (see HitFrame's comment): a plain 2D node's world chains from the enclosing plane, not
    // from `node` itself.
    for (size_t i = frame.children.size(); i-- > 0;) {
        NodeId child = frame.children[i];
        if (arena.kind(child) != NodeKind::Element) {
            continue;  // Scene nodes and Text are never individually hit (08.2).
        }
        const NodeCommon& cc = arena.common(child);
        if (cc.is3D != 0 && cc.isSpaceRoot == 0) {
            // A 3D layer (not itself a root): belongs to some sorted segment's own candidate
            // list, tested independently by hit3DRoot() with its own ray-plane intersection --
            // not reachable through ordinary 2D descent (§19.9: "遇到 is3D 子孫跳過").
            continue;
        }
        if (!in3D && (cc.flags2 & kLayerOverlay) != 0) {
            continue;  // T-194: a `layer: overlay` subtree was already tested, before the tree.
        }
        HitResult hit = hitNode(arena, child, pos, mode, in3D, depth + 1);
        if (hit.node != kInvalidNode) {
            return hit;
        }
    }
    if (frame.selfEligible) {
        if (contains(frame.hitRect, frame.localPos)) {  // T-386: the border box grown by hit-slop.
            return HitResult{node, frame.localPos, (arena.common(node).flags2 & kPointerWatch) != 0};
        }
    }
    return HitResult{};
}

/// T-194 (08.9): every `layer: overlay` element under `root` in paint order (z-index sorted
/// preorder), outside 3D space roots and display:none / invisible subtrees.
void collectOverlays(const NodeArena& arena, NodeId root, std::vector<NodeId>& out) {
    std::vector<NodeId> stack{root};
    std::vector<NodeId> children;
    while (!stack.empty()) {
        const NodeId node = stack.back();
        stack.pop_back();
        if (!arena.alive(node) || arena.kind(node) != NodeKind::Element) {
            continue;
        }
        const NodeCommon& c = arena.common(node);
        if (arena.element(node).style.display == Display::None || c.visible == 0) {
            continue;
        }
        if (node != root && (c.flags2 & kLayerOverlay) != 0) {
            out.push_back(node);
        }
        if (c.isSpaceRoot != 0) {
            continue;
        }
        sortedElementChildren(arena, node, children);
        for (size_t i = children.size(); i-- > 0;) {
            stack.push_back(children[i]);
        }
    }
}

}  // namespace

HitResult hitTestDetailed(const NodeArena& arena, NodeId root, Vec2 pos, HitMode mode) {
    // T-194 (08.9): overlays hit first -- the last painted first -- without their ancestors'
    // overflow clip (their world transform already places them).
    std::vector<NodeId> overlays;
    collectOverlays(arena, root, overlays);
    for (size_t i = overlays.size(); i-- > 0;) {
        HitResult hit = hitNode(arena, overlays[i], pos, mode);
        if (hit.node != kInvalidNode) {
            return hit;
        }
    }
    return hitNode(arena, root, pos, mode);
}

NodeId hitTest(const NodeArena& arena, NodeId root, Vec2 pos, HitMode mode) {
    return hitTestDetailed(arena, root, pos, mode).node;
}

}  // namespace xt
