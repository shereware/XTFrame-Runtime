#include "xt/world_layer_output.h"

#include "event_dispatcher.h"
#include "xt/element_ref.h"
#include "xt/node_arena.h"

/// xt/world_layer_output.h (Docs/design/33-world-layers.md, T-701).

namespace xt {

void setWorldLayerOutput(Document& document, bool on) {
    NodeArena& arena = document.arena();
    NodeArena::WorldLayerState& state = arena.worldLayerState();
    if (state.output == on) {
        return;
    }
    state.output = on;
    state.overLimit = false;
    state.previousRects.clear();  // T-706: switching off and on again starts every layer afresh.
    const NodeId root = document.root().node;
    if (!on && root != kInvalidNode && arena.alive(root)) {
        // The marks belong to the mode: without them every branch is as before.
        arena.traversePreorder(root, [&arena](NodeId n) {
            arena.common(n).worldLayerActive = 0;
            return true;
        });
    }
    // Planes appear or go: the next transform pass recomputes every world / world3 and every
    // space-root flag.
    state.pendingTransforms = true;
}

bool worldLayerOutput(const Document& document) { return document.arena().worldLayerState().output; }

NodeId worldLayerHitTest(const Document& document, Vec3 origin, Vec3 direction) {
    const NodeArena& arena = document.arena();
    const NodeId root = document.root().node;
    if (root == kInvalidNode || !arena.alive(root) || !arena.worldLayerState().output) {
        return kInvalidNode;
    }
    return hitTestRay(arena, root, origin, direction).node;
}

void setWorldLayerPointerRay(Document& document, Vec3 origin, Vec3 direction, Vec2 pos) {
    NodeArena::WorldLayerState& state = document.arena().worldLayerState();
    state.rayValid = true;
    state.rayOrigin = origin;
    state.rayDir = direction;
    state.rayPos = pos;
}

}  // namespace xt
