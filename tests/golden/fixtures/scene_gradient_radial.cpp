#include "../golden_common.h"

/// A circle with a radial-gradient fill, 3 stops. Non-Solid fills are skipped by the SW renderer
/// until T-12 (sw/src/sw_renderer.cpp fillPath), so this fixture's expected image records the
/// pre-T-12 (blank canvas) result -- re-`--update` once T-12 lands. See docs/tasks/T-13.md
/// fixture table.

using namespace xt;
using namespace xt::golden;

XT_GOLDEN_FIXTURE(scene_gradient_radial, 256, 256, 1.0f, 0.0f, (Color{0, 0, 0, 1}),
                  buildSceneGradientRadial) {
    compile::SceneBuilder scene;
    scene.width = 256;
    scene.height = 256;

    PathSpec circle;
    const Vec2 center{128, 128};
    const float radius = 90.0f;
    appendCircle(circle.verbs, circle.points, center, radius);
    circle.fillKind = PaintKind::Radial;
    circle.fillP0 = center;
    circle.fillP1 = Vec2{center.x + radius, center.y};
    circle.fillStops = {
        IrGradientStop{0.0f, Color{1, 1, 0, 1}},
        IrGradientStop{0.5f, Color{1, 0, 1, 1}},
        IrGradientStop{1.0f, Color{0, 1, 1, 1}},
    };
    uint32_t pathIndex = addPath(scene, circle);

    addNode(scene, NodeKind::Path, kRootParent, pathIndex);
    writer.addScene(scene);
}
