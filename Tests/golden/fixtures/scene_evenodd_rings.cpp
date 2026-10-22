#include "../golden_common.h"

/// Two concentric circles (each a 4-segment bezier approximation), EvenOdd fill rule -> a ring.
/// See Docs/tasks/T-13.md fixture table.

using namespace xt;
using namespace xt::golden;

XT_GOLDEN_FIXTURE(scene_evenodd_rings, 256, 256, 1.0f, 0.0f, (Color{0, 0, 0, 1}), buildSceneEvenoddRings) {
    compile::SceneBuilder scene;
    scene.width = 256;
    scene.height = 256;

    PathSpec rings;
    const Vec2 center{128, 128};
    appendCircle(rings.verbs, rings.points, center, 100.0f);
    appendCircle(rings.verbs, rings.points, center, 50.0f);
    rings.fillRule = FillRule::EvenOdd;
    rings.fillKind = PaintKind::Solid;
    rings.fillColor = Color{0, 0.8f, 0.8f, 1};
    uint32_t pathIndex = addPath(scene, rings);

    addNode(scene, NodeKind::Path, kRootParent, pathIndex);
    writer.addScene(scene);
}
