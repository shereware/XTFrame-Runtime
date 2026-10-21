#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "xt/node_arena.h"
#include "xt/rig.h"
#include "xt/types.h"

/// Rig evaluation (docs/design/32-rigging-and-deformation.md §32.3, §32.2.3, §32.8; T-244): bone
/// world transforms in the skeleton group's space, the IK / Transform / Rotation / Spring
/// constraints in document order, and linear blend skinning (LBS) of Path and Mesh nodes. Private
/// to core: Document owns one RigState per `<xt-skeleton>` group and runs evaluate() from update();
/// the Painter only reads the skinned geometry the Document caches in NodeArena's side table.

namespace xt::rig {

/// One Spring constraint's simulation state (§32.2.3). `parentVelocity` / `prevAnchor` /
/// `primed` track the motion of the parent chain (the bone's origin before the spring applies).
struct SpringState {
    Vec2 offset{};    ///< Translate offset, skeleton group space.
    Vec2 velocity{};  ///< d offset / dt.
    float angle = 0;  ///< Rotation offset, radians (clamped to +-90 degrees).
    float angularVelocity = 0;
    float accumulator = 0;  ///< Seconds not yet simulated (< one 1/60 s step).
    Vec2 parentVelocity{};  ///< The anchor's velocity measured last frame.
    Vec2 prevAnchor{};      ///< The anchor's position last frame.
    bool primed = false;    ///< prevAnchor / parentVelocity hold a measurement.
};

/// A bone's local pose: the transform components the rig reads (§32.2.1) and the constraints write.
struct BonePose {
    Vec2 translate{};
    float rotation = 0;  ///< Degrees.
    Vec2 scale{1, 1};
    Vec2 anchor{};
};

/// A skinned Path or Mesh of a skeleton, with each tendon's index into RigState::bones.
struct SkinTarget {
    NodeId node = kInvalidNode;
    std::vector<int32_t> tendonBone;  ///< -1 = the tendon's bone is not in this skeleton.
};

/// Per-skeleton evaluation state, built by buildRigState() at load.
struct RigState {
    NodeId skeleton = kInvalidNode;    ///< The `<xt-skeleton>` Group.
    std::vector<NodeId> bones;         ///< Document order: every parent before its children.
    std::vector<Mat2x3> world;         ///< Per bone, skeleton group space (root bones relative to it).
    std::vector<Mat2x3> prevWorld;     ///< `world` at the end of the previous evaluate().
    std::vector<SpringState> springs;  ///< One per entry of `constraints` (used by Spring ones).
    uint32_t version = 0;              ///< Bumped by evaluate() whenever any world changed.

    std::vector<int32_t> parent;       ///< Per bone: its parent bone's index, -1 for a root bone.
    std::vector<uint32_t> subtreeEnd;  ///< Per bone: one past the last index of its subtree.
    std::vector<BonePose> pose;        ///< Per bone: this frame's local pose (FK + constraints).
    std::vector<NodeId> constraints;   ///< Constraint children of the bones, document order.
    std::vector<int32_t> owner;        ///< Per constraint: the index of the bone it is under.
    std::vector<SkinTarget> skins;     ///< The Paths / Meshes skinned to this skeleton.
    bool evaluated = false;            ///< evaluate() has run at least once.
};

/// Builds `out` for skeleton group `skeleton`: its bones (preorder), their constraints, and every
/// node of `sceneNodes` that is a Path or Mesh skinned to it (non-empty weights).
void buildRigState(const NodeArena& arena, NodeId skeleton, std::span<const NodeId> sceneNodes,
                   RigState& out);

/// §32.3 steps 2-3: (a) every bone's local pose from its properties -- a child bone's translate
/// is always (parent length, 0) -- and its world, parents first; (b) the constraints in document
/// order, each modifying its bone's local pose, then recomputing that bone's subtree (an IK chain:
/// from the chain's root); (c) `version++` when any world differs from the previous evaluation.
/// `dt` (seconds, >= 0) advances the springs. Returns the number of bones evaluated.
uint32_t evaluate(RigState& rs, NodeArena& arena, float dt);

/// Zeroes every spring's state (the editor's scrub reset, §32.2.3).
void resetSprings(RigState& rs);

/// LBS of Path `path` (§32.2.2): every point of PathNodeData::points (p / in / out of each vertex,
/// weights 3 per vertex) through sum(w_i * world[bone_i] * inverseBind_i), into `out` (skeleton
/// group space; the node's own transform is not applied). False (out untouched) when the weights
/// do not match the path's vertices.
bool skinPath(const NodeArena& arena, const RigState& rs, const SkinTarget& target, std::vector<float>& out);

/// LBS of Mesh `mesh`'s positions (one weight per vertex) into `out`. False as skinPath().
bool skinMesh(const NodeArena& arena, const RigState& rs, const SkinTarget& target, std::vector<Vec2>& out);

/// Re-skins every skinned shape of `rs` whose cache in NodeArena's side table is stale (another
/// rig version, the node marked kDirtyPaint -- a `d` morph -- , a size change, or invalidated).
/// Returns the number of points skinned (0 on a static frame).
uint32_t updateSkins(const RigState& rs, NodeArena& arena);

/// T-245 hook: the skinned positions of Mesh `mesh` from the side table, copied into `out`. False
/// (out untouched) when the mesh is not skinned or has not been evaluated yet.
bool skinnedMeshPositions(const NodeArena& arena, NodeId mesh, std::vector<Vec2>& out);

/// T-243c: the public read-only copy of `rs` (Document::rigAt()): bones with their world and
/// length, the IK constraints with their target's origin in skeleton group space, the bones under
/// a Spring constraint.
RigView makeView(const RigState& rs, const NodeArena& arena);

/// The index of `bone` in `rs.bones`, or -1.
int32_t boneIndex(const RigState& rs, NodeId bone);

/// §32.8 `world-anchor` on a bone: `point` in the skeleton group's space. A root bone (its parent
/// is not a bone) gets it as its translate; any other bone and its parent bone are rotated by a
/// 2-bone IK (strength 1, bending the way the bone currently bends) so that the bone's tip reaches
/// it. Writes the bones' TranslateX/Y or Rotation properties (only when they change). The FK pose
/// comes from the properties (no constraints).
void applyBoneAnchor(NodeArena& arena, NodeId bone, Vec2 point);

/// The skeleton group of `bone` (its nearest ancestor that is not a bone), or kInvalidNode.
NodeId skeletonOf(const NodeArena& arena, NodeId bone);

}  // namespace xt::rig
