#pragma once

#include <vector>

#include "xt/ir.h"
#include "xt/node_arena.h"
#include "xt/types.h"

/// Instantiates an IR SceneChunk into a NodeArena subtree (with NodeId remapping). Does not touch
/// the Element / document tree (that's T-24+). See Docs/tasks/T-08.md,
/// Docs/design/02-ir-format.md §2.7-2.8, Docs/design/03-scene-and-properties.md §3.2-3.7.

namespace xt {

/// A live instance of a Scene template: the subtree created by instantiateScene() plus the
/// scene-local-index -> actual-NodeId table needed to resolve Track::target.
struct SceneInstance {
    /// The template SceneId this instance was created from.
    SceneId scene = kInvalidScene;
    NodeId root = kInvalidNode;  ///< Instance root (a Group); a child of the parent passed in.
    NodeId base = kInvalidNode;  ///< Actual NodeId that local index 0 maps to (== root).
    /// Local scene index -> actual NodeId. Instantiation may leave this non-contiguous (arena
    /// free-list reuse), so it must be stored rather than computed from `base`.
    std::vector<NodeId> localToArena;
    /// Scene template width (px).
    float width = 0;
    /// Scene template height (px).
    float height = 0;
    /// The scene's name, interned in the IR string table.
    StringId name = kInvalidString;
};

/// Instantiates `scene` from `ir` into `arena`, appending the resulting subtree's root as the last
/// child of `parent` (which must be alive). Nodes are created and appended in the SceneChunk's
/// depth-first preorder (parent before child), matching IR node layout.
///
/// Failure (e.g. an out-of-range kindIndex) does not happen on IR already validated by T-03's
/// IrView::open(); such conditions are logic errors and XT_ASSERT accordingly rather than
/// returning an error. Always returns Status::kOk.
Status instantiateScene(const IrView& ir, SceneId scene, NodeArena& arena, NodeId parent, SceneInstance& out);

/// Destroys the instance's subtree (arena.destroy(inst.root)) and resets `inst` to a default
/// SceneInstance. No-op if inst.root == kInvalidNode.
void destroySceneInstance(NodeArena& arena, SceneInstance& inst);

/// Converts a Track::target (a scene-local index) into the instance's actual NodeId.
/// kSelfNode is returned unchanged. An out-of-range localTarget returns kInvalidNode.
NodeId resolveTrackTarget(const SceneInstance& inst, NodeId localTarget);

}  // namespace xt
