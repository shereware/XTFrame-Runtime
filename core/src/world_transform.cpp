#include <vector>

#include "camera.h"
#include "xt/node_arena.h"
#include "xt/types.h"

/// World transform update: a non-recursive preorder pass over a subtree that recomputes
/// NodeCommon::local/world/world3/worldOpacity and, inside a 3D space root's subtree, the plane-
/// relative semantics of docs/design/19-3d-space.md §19.5. See docs/design/01-architecture.md
/// §1.5 step 4, docs/tasks/T-10.md "世界變換", docs/tasks/T-43b.md.

namespace xt {

namespace {

/// True when `n` is a Group node that is the instantiated root of some Element's `anim` scene
/// (ElementData::sceneRoot == n).
bool isSceneRootGroup(const NodeArena& arena, NodeId n) {
    if (arena.kind(n) != NodeKind::Group) {
        return false;
    }
    NodeId p = arena.parent(n);
    if (p == kInvalidNode || arena.kind(p) != NodeKind::Element) {
        return false;
    }
    return arena.element(p).sceneRoot == n;
}

/// Non-recursive scan (own local stack, not NodeArena's shared traversal buffers, since this runs
/// from inside updateWorldTransforms' own traversePreorder callback) for any is3D node in root's
/// subtree (root included).
bool subtreeHas3D(const NodeArena& arena, NodeId root) {
    std::vector<NodeId> stack;
    stack.push_back(root);
    while (!stack.empty()) {
        NodeId n = stack.back();
        stack.pop_back();
        if (arena.common(n).is3D != 0) {
            return true;
        }
        for (NodeId c = arena.firstChild(n); c != kInvalidNode; c = arena.nextSibling(c)) {
            stack.push_back(c);
        }
    }
    return false;
}

/// docs/design/19-3d-space.md §19.5's "根" test.
bool computeIsSpaceRoot(const NodeArena& arena, NodeId n) {
    if (findCameraChild(arena, n) != kInvalidNode) {
        return true;
    }
    if (arena.kind(n) == NodeKind::Element && arena.element(n).perspectivePx > 0.0f) {
        return true;
    }
    if (isSceneRootGroup(arena, n) && subtreeHas3D(arena, n)) {
        return true;
    }
    return false;
}

/// §19.2's local3, the 3D counterpart of Mat2x3's composeTRSS.
Mat4 composeLocal3(const NodeCommon& c) {
    Mat4 t = mat4Translate(Vec3{c.translate.x, c.translate.y, c.translateZ});
    Mat4 rz = mat4RotateZ(c.rotation + c.orientPathDeg);  // T-228 auto-orient.
    Mat4 ry = mat4RotateY(c.rotationY);
    Mat4 rx = mat4RotateX(c.rotationX);
    Mat4 orz = mat4RotateZ(c.orientation.z);
    Mat4 ory = mat4RotateY(c.orientation.y);
    Mat4 orx = mat4RotateX(c.orientation.x);
    Mat4 sk = mat4FromMat2x3(skewDeg(c.skew.x, c.skew.y));
    Mat4 s = mat4Scale(Vec3{c.scale.x, c.scale.y, 1});
    Mat4 negAnchor = mat4Translate(Vec3{-c.anchor.x, -c.anchor.y, -c.anchorZ});
    Mat4 m = mat4Mul(
        t,
        mat4Mul(
            rz,
            mat4Mul(
                ry,
                mat4Mul(rx, mat4Mul(orz, mat4Mul(ory, mat4Mul(orx, mat4Mul(sk, mat4Mul(s, negAnchor)))))))));
    return m;
}

}  // namespace

/// Recomputes local/world/world3/worldOpacity for `root`'s subtree (root included), and the
/// isSpaceRoot cache for nodes that carry kDirtyTree (or every node, when `force`).
///
/// A node is recomputed when `force` is true, when it carries kDirtyTransform or kDirtyTree, or
/// when its parent was just recomputed in this same call (kDirtyTransform propagates to every
/// descendant when set -- see NodeArena::markDirty -- so once the first dirty node on a branch is
/// found the rest of that branch is known to need recomputing too). Nodes that are not recomputed
/// keep their cached local/world/world3/worldOpacity untouched; traversal still descends into
/// their children, since a deeper descendant may carry the dirty bits on its own.
///
/// worldOpacity has its own, wider recompute condition (T-158): an opacity change only marks
/// kDirtyPaint (01-architecture.md §1.4), never kDirtyTransform, so a node is also refreshed when
/// it carries kDirtyPaint or its parent's worldOpacity was just refreshed. Without this, an element
/// whose animated opacity starts at 0 kept worldOpacity == 0 (and was culled by the Painter and hit
/// test) unless something else happened to mark its subtree transform-dirty.
///
/// Outside any 3D space root's subtree, behavior is byte-identical to the pre-19-3d-space version
/// (docs/design/19-3d-space.md §19.5's "不在任何根子樹內...既有行為完全不變").
void updateWorldTransforms(NodeArena& arena, NodeId root, bool force) {
    // T-38 (docs/design/01-architecture.md §1.7): function-local `static` scratch, reused/resized
    // (not reallocated) across calls. Single-threaded, never called recursively (see T-38's note).
    static std::vector<uint8_t> recompute;
    static std::vector<uint8_t> recomputeOpacity;  // T-158: see the function comment.
    // Per-node context handed to a node's children (written when the node itself is processed,
    // read via the parent's slot when a child is processed -- preorder guarantees the parent's
    // slot is already written).
    static std::vector<Mat2x3> outParent2D;  // A 2D child's world = outParent2D[parent] * local.
    static std::vector<Mat4> outParentP;     // A 3D-layer child's world3 = outParentP[parent] * local3.
    static std::vector<Mat4> outPlaneW3;     // The plane context a plain-2D child hands its own children.
    static std::vector<uint8_t> outInSpace;  // 1 when the node is a space root or inside one's subtree.
    uint32_t cap = arena.capacity();
    recompute.assign(cap, 0);
    recomputeOpacity.assign(cap, 0);
    outParent2D.assign(cap, Mat2x3{});
    outParentP.assign(cap, mat4Identity());
    outPlaneW3.assign(cap, mat4Identity());
    outInSpace.assign(cap, 0);

    arena.traversePreorder(root, [&](NodeId n) {
        NodeId p = arena.parent(n);
        bool parentRecompute = p != kInvalidNode && recompute[p] != 0;
        bool selfDirty = (arena.dirty(n) & static_cast<uint8_t>(kDirtyTransform | kDirtyTree)) != 0;
        bool doRecompute = force || selfDirty || parentRecompute;
        recompute[n] = doRecompute ? 1 : 0;
        bool parentOpacity = p != kInvalidNode && recomputeOpacity[p] != 0;
        bool opacityDirty = (arena.dirty(n) & static_cast<uint8_t>(kDirtyPaint)) != 0;
        bool doOpacity = doRecompute || opacityDirty || parentOpacity;
        recomputeOpacity[n] = doOpacity ? 1 : 0;

        if (arena.kind(n) == NodeKind::Camera) {
            // §19.4: a Camera node is not drawn/hit-tested and its world/world3 are not part of
            // any plane -- skip, but still hand a (never-consulted) context down in case it has
            // children (load-time warns and ignores those, but traversal must stay well-defined).
            Mat4 planeIn = (p == kInvalidNode) ? mat4Identity() : outPlaneW3[p];
            outParent2D[n] = Mat2x3{};
            outParentP[n] = planeIn;
            outPlaneW3[n] = planeIn;
            outInSpace[n] = (p == kInvalidNode) ? 0 : outInSpace[p];
            return true;
        }

        NodeCommon& c = arena.common(n);

        bool recomputeRootFlag = force || (arena.dirty(n) & static_cast<uint8_t>(kDirtyTree)) != 0;
        if (recomputeRootFlag) {
            c.isSpaceRoot = computeIsSpaceRoot(arena, n) ? 1 : 0;
        }
        bool asRoot = c.isSpaceRoot != 0;
        // §19.4: only a node inside a space root's subtree is a 3D layer. A 3D-transformed node with
        // no root above it (an element using rotateX/rotateY/translateZ where no ancestor sets
        // `perspective` or owns a <camera>) is drawn orthographically, as in CSS with
        // `perspective: none`: its local3 is flattened onto the z = 0 plane and it stays in the
        // ordinary 2D chain, so it keeps its layout position.
        bool parentInSpace = p != kInvalidNode && outInSpace[p] != 0;
        outInSpace[n] = (asRoot || parentInSpace) ? 1 : 0;
        bool as3DLayer = !asRoot && c.is3D != 0 && parentInSpace;
        bool asFlattened3D = !asRoot && c.is3D != 0 && !parentInSpace;

        if (doRecompute) {
            c.local = composeTRSS(c.translate, c.rotation + c.orientPathDeg, c.skew, c.scale, c.anchor);
            if (as3DLayer) {
                Mat4 local3 = composeLocal3(c);
                Mat4 parentP = (p == kInvalidNode) ? mat4Identity() : outParentP[p];
                c.world3 = mat4Mul(parentP, local3);
                c.world = Mat2x3{};  // Identity: "圖層自己的平面原點" (§19.5).
            } else {
                Mat2x3 parent2D = (p == kInvalidNode) ? Mat2x3{} : outParent2D[p];
                if (asFlattened3D) {
                    // Row-major, column vectors: x' = m00 x + m01 y + m03, y' = m10 x + m11 y + m13.
                    const Mat4 local3 = composeLocal3(c);
                    c.local =
                        Mat2x3{local3.m[0], local3.m[4], local3.m[1], local3.m[5], local3.m[3], local3.m[7]};
                }
                c.world = mul(parent2D, c.local);
            }
        }
        if (doOpacity) {
            if (p == kInvalidNode) {
                c.worldOpacity = c.opacity;
            } else {
                const NodeCommon& pc = arena.common(p);
                c.worldOpacity = pc.worldOpacity * c.opacity;
            }
        }

        // Context handed to n's children, computed from n's current (possibly just-recomputed,
        // possibly cached-from-before) state -- children need this whether or not n itself was
        // recomputed this call.
        Mat4 planeIn = (p == kInvalidNode) ? mat4Identity() : outPlaneW3[p];
        if (asRoot) {
            outParent2D[n] = Mat2x3{};
            outParentP[n] = mat4Identity();
            outPlaneW3[n] = mat4Identity();
        } else if (as3DLayer) {
            outParent2D[n] = Mat2x3{};
            outParentP[n] = c.world3;
            outPlaneW3[n] = c.world3;
        } else {
            outParent2D[n] = c.world;
            outParentP[n] = mat4Mul(planeIn, mat4FromMat2x3(c.world));
            outPlaneW3[n] = planeIn;
        }

        return true;  // always descend: dirty bits may exist deeper even if this node has none
    });
}

}  // namespace xt
