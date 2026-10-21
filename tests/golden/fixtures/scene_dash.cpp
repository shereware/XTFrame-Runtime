#include "../golden_common.h"

/// A dashed circle outline (stroke only, no fill). StrokePath is ignored by the SW renderer until
/// T-12 (docs/tasks/T-11.md), so this fixture's expected image records the pre-T-12 (blank
/// canvas) result -- re-`--update` once T-12 lands. See docs/tasks/T-13.md fixture table.

using namespace xt;
using namespace xt::golden;

XT_GOLDEN_FIXTURE(scene_dash, 256, 256, 1.0f, 0.0f, (Color{0, 0, 0, 1}), buildSceneDash) {
    compile::SceneBuilder scene;
    scene.width = 256;
    scene.height = 256;

    PathSpec circle;
    appendCircle(circle.verbs, circle.points, Vec2{128, 128}, 90.0f);
    circle.fillKind = PaintKind::None;
    circle.strokeKind = PaintKind::Solid;
    circle.strokeColor = Color{1, 1, 1, 1};
    circle.strokeWidth = 8;
    circle.cap = LineCap::Butt;
    circle.dashes = {20, 10};
    uint32_t pathIndex = addPath(scene, circle);

    addNode(scene, NodeKind::Path, kRootParent, pathIndex);
    writer.addScene(scene);
}
