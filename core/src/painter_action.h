#pragma once

#include "xt/command_list.h"
#include "xt/node_arena.h"
#include "xt/painter.h"
#include "xt/types.h"

/// Painter::Action's full definition (Painter::Action is only forward-declared, as an incomplete
/// nested type, by the public xt/painter.h -- see that header's comment on Painter's out-of-line
/// ctor/dtor). Shared between painter.cpp (the explicit-stack traversal loop in Painter::paint(),
/// which switches on every Kind) and painter_3d.cpp (Painter::emitRoot()/flushSection(), which
/// build Action lists for the four T-44b kinds below). Not included by any public header.

namespace xt {

/// One pending step of the explicit-stack traversal (replaces C++ recursion; see 14.1's "no
/// recursion in traversal code" rule). EnterNode carries the node to process and the alpha
/// inherited from ancestors that did not get their own Layer; the other kinds just emit one
/// parameter-less CommandList command when popped (PopClip: T-27's overflow clip, matching
/// PushClipRect which paintElement() emits immediately since it needs no deferred state).
///
/// T-44b (docs/design/19-3d-space.md §19.5-19.6) adds five kinds, all built by emitRoot()/
/// flushSection() (painter_3d.cpp): EnterNode2D and EnterNodeLayer both carry a node + alpha like
/// EnterNode, but first set Painter::m_currentLayer (EnterNode2D: kInvalidNode, so no is3D
/// descendant matches "the current layer" and every one is skipped; EnterNodeLayer: `node` itself,
/// so exactly that layer's own is3D is let through) before calling enterNode() -- see enterNode()'s
/// new isSpaceRoot/is3D-skip/Camera branches. PushTransform3D/PopTransform3D wrap one "on this
/// plane" quantum (a plain section, a sorted segment's non-layer content, or one sorted layer);
/// PushTransform3D carries the plane's `xf3` (world3, or identity for the root's own plane).
/// PopProjection also pops Painter::m_mappingStack (pushed by emitRoot() when its matching
/// PushProjection is emitted).
///
/// T-52 (docs/design/18-effects.md §18.1) adds PopEffect: pushed by enterNode() right after an
/// immediately-emitted PushEffect command, below whatever content actions the node's own
/// dispatch (paintElement/pushPlan/emitPath/emitImage) pushes on top of it -- so it pops (i.e.
/// executes) after all of that content, and before a wrapping PopLayer (if any), matching 18.1's
/// "PushEffect ... 內容 ... PopEffect / 若有 Layer：PopLayer" order.
struct Painter::Action {
    enum class Kind : uint8_t {
        EnterNode,
        PushMask,
        EndMaskContent,
        PopMask,
        PopLayer,
        PopClip,
        EnterNode2D,
        EnterNodeLayer,
        PushTransform3D,
        PopTransform3D,
        PopProjection,
        PopEffect,
    };
    Kind kind = Kind::EnterNode;
    NodeId node = kInvalidNode;
    float alpha = 1.0f;
    MaskMode mode = MaskMode::Alpha;
    Mat4 xf3 = mat4Identity();  ///< PushTransform3D's payload (T-44b).
};

}  // namespace xt
