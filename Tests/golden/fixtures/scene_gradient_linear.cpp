#include "../golden_common.h"

/// A square with a diagonal linear-gradient fill, 3 stops. Non-Solid fills are skipped by the SW
/// renderer until T-12 (Code/Runtime/sw/src/sw_renderer.cpp fillPath), so this fixture's expected image
/// records the pre-T-12 (blank canvas) result -- re-`--update` once T-12 lands. See
/// Docs/tasks/T-13.md fixture table.

using namespace xt;
using namespace xt::golden;

XT_GOLDEN_FIXTURE(scene_gradient_linear, 256, 256, 1.0f, 0.0f, (Color{0, 0, 0, 1}),
                  buildSceneGradientLinear) {
    compile::SceneBuilder scene;
    scene.width = 256;
    scene.height = 256;

    PathSpec square;
    square.verbs = {PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Line, PathVerb::Close};
    square.points = {48, 48, 208, 48, 208, 208, 48, 208};
    square.fillKind = PaintKind::Linear;
    square.fillP0 = Vec2{48, 48};
    square.fillP1 = Vec2{208, 208};
    square.fillStops = {
        IrGradientStop{0.0f, Color{1, 0, 0, 1}},
        IrGradientStop{0.5f, Color{0, 1, 0, 1}},
        IrGradientStop{1.0f, Color{0, 0, 1, 1}},
    };
    uint32_t pathIndex = addPath(scene, square);

    addNode(scene, NodeKind::Path, kRootParent, pathIndex);
    writer.addScene(scene);
}
