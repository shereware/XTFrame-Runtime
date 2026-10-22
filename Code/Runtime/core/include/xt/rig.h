#pragma once

#include <cstdint>
#include <vector>

#include "xt/types.h"

/// T-243c (Docs/design/32-rigging-and-deformation.md §32.2.3, §32.5): a read-only copy of one
/// skeleton's evaluated pose, for an editor's bone overlay and IK gizmo (`Document::rigAt()`).
/// Every matrix and point is in the skeleton group's space (the `<xt-skeleton>` Group's local
/// space; multiply by that Group's world to reach the document). It is the result of the most
/// recent `Document::update()`: FK, the constraints in document order and the spring offsets.

namespace xt {

/// One bone of a RigView.
struct RigViewBone {
    NodeId node = kInvalidNode;  ///< The Bone scene node.
    int32_t parent = -1;         ///< Index of its parent bone in RigView::bones, -1 for a root bone.
    Mat2x3 world{};              ///< Bone space -> skeleton group space, after the constraints.
    float length = 0;            ///< The bone's length (PropId BoneLength).
};

/// One IK constraint of a RigView.
struct RigViewIk {
    NodeId constraint = kInvalidNode;  ///< The Constraint scene node.
    int32_t bone = -1;                 ///< Index of the bone it is under in RigView::bones.
    NodeId target = kInvalidNode;      ///< The target node (kInvalidNode when it has none).
    Vec2 position{};                   ///< The target's origin in skeleton group space.
    bool invert = false;               ///< The `invert` parameter (params[1]) is on.
};

/// One skeleton's pose (`Document::rigAt()`).
struct RigView {
    NodeId skeleton = kInvalidNode;    ///< The `<xt-skeleton>` Group.
    std::vector<RigViewBone> bones;    ///< Document order: every parent before its children.
    std::vector<RigViewIk> ik;         ///< The IK constraints, document order.
    std::vector<int32_t> springBones;  ///< Bones with a Spring constraint (indices into `bones`).
};

}  // namespace xt
