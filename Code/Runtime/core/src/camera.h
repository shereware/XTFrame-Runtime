#pragma once

#include "xt/node_arena.h"
#include "xt/types.h"

/// Camera view-projection setup (Docs/design/19-3d-space.md §19.4). Internal header, shared by
/// world_transform.cpp, document.cpp and document_load.cpp.

namespace xt {

/// The result of one of the functions below: the root's view-projection matrix (19.4 step 5) and
/// whether it is exactly the identity (CSS `perspective: none`).
struct CameraSetup {
    /// The root's view-projection matrix, mapping the root's z=0-embedded 3D points to the root's
    /// own 2D (box) coordinates.
    Mat4 viewProj;
    /// True only for cssPerspectiveViewProj(perspectivePx <= 0, ...): viewProj is the identity,
    /// so 3D rotations degenerate to a flat scale, matching plain CSS behavior.
    bool orthographicIdentity;
    /// The view matrix alone (19.4 step 2), i.e. `viewProj` without the projection/ndcToBox steps.
    /// Identity for cssPerspectiveViewProj (§19.6: "CSS 預設攝影機時 view = I，depth = 錨點 z 經
    /// world3 變換後的 z"). Used by Painter (T-44b) for §19.6's per-layer depth:
    /// `(view * world3 * (anchor.xyz, 1)).z`.
    Mat4 view = mat4Identity();
};

/// 19.4 steps 1-5 for an explicit <camera>/Lottie camera node. `box` is the root's reference box
/// (origin top-left); `cameraNode`'s own NodeCommon::translate/translateZ is the eye position, and
/// its CameraData (NodeArena::camera()) supplies kind/useTarget/zoom/target/near/far.
CameraSetup computeCameraViewProj(const NodeArena& arena, NodeId cameraNode, Rect box);

/// 19.4's CSS default camera for an Element root (built from `perspective`/`perspective-origin`,
/// not a LookAt). `perspectivePx <= 0` returns the identity (orthographicIdentity == true).
CameraSetup cssPerspectiveViewProj(float perspectivePx, Vec2 originPx);

/// 19.4's AE default camera for a scene root with no camera layer: eye = (W/2, H/2, W), target =
/// (W/2, H/2, 0), zoom = W, near = 1, far = 100000.
CameraSetup defaultSceneViewProj(float width, float height);

/// 19.4's root reference box for `root`, computed from `arena` alone (no IrDocument access, unlike
/// Document::spaceRootBox()): an Element root -> its own border-box; a scene-root Group -> the
/// design size cached on its `anim` parent Element's `ElementData::intrinsic` (set by
/// LayoutEngine::intrinsicSize() straight from the SceneChunk's width/height for tag == "anim",
/// Docs/tasks/T-25.md) when that parent has a scene and `hasIntrinsic`; an empty Rect otherwise.
/// Used by Painter::emitRoot() (T-44b), which only has a NodeArena to work with.
Rect spaceRootBoxOf(const NodeArena& arena, NodeId root);

/// 19.4's full per-root setup: findCameraChild() + spaceRootBoxOf(), then
/// computeCameraViewProj()/cssPerspectiveViewProj()/defaultSceneViewProj() depending on what's
/// found (camera child -> Element with perspectivePx -> scene default). Used by Painter::
/// emitRoot() (T-44b); kept separate from Document::viewProjFor()/spaceRootBox(), which use the
/// IrDocument-backed SceneChunk size directly and are unaffected by this task.
CameraSetup rootCameraSetup(const NodeArena& arena, NodeId root);

/// Finds `root`'s 3D space camera (19.4): the first direct child, in document order, that is
/// either a visible NodeKind::Camera node (Lottie/scene camera) or an Element with
/// ElementData::cameraNode set (a `<camera>` tag). Returns kInvalidNode if none. Shared by
/// world_transform.cpp's isSpaceRoot cache and Document::cameraOf().
NodeId findCameraChild(const NodeArena& arena, NodeId root);

/// Walks up from `n`'s parent looking for the nearest ancestor whose NodeCommon::isSpaceRoot
/// cache is already set (1); if none is set before reaching the top, returns the topmost
/// ancestor (the document root). Returns `n` itself if it has no parent. Used to bound
/// NodeArena::markDirtyAncestors() when an Element/scene node's is3D changes: a descendant's
/// is3D can flip a scene Group's computeIsSpaceRoot() result (§19.3's "isSceneRootGroup &&
/// subtreeHas3D"), so every ancestor up to the nearest existing root (or the document root, if
/// no root exists yet) must be re-examined via kDirtyTree. See Docs/tasks/T-43c.md "T-43b 遺留
/// 修正" 2.
NodeId nearestSpaceRootOrDocRoot(const NodeArena& arena, NodeId n);

}  // namespace xt
