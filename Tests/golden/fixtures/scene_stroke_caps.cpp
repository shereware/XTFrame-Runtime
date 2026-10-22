#include "../golden_common.h"

/// Three straight lines, stroked (no fill) with Butt/Round/Square caps, width 12. StrokePath is
/// ignored by the SW renderer until T-12 (Docs/tasks/T-11.md), so this fixture's expected image
/// records the pre-T-12 (blank canvas) result, per Docs/tasks/T-13.md fixture table -- re-`--update`
/// once T-12 lands.

using namespace xt;
using namespace xt::golden;

namespace {

uint32_t addStrokeLine(compile::SceneBuilder& scene, float y, LineCap cap) {
    PathSpec line;
    line.verbs = {PathVerb::Move, PathVerb::Line};
    line.points = {40, y, 216, y};
    line.fillKind = PaintKind::None;
    line.strokeKind = PaintKind::Solid;
    line.strokeColor = Color{1, 1, 1, 1};
    line.strokeWidth = 12;
    line.cap = cap;
    return addPath(scene, line);
}

}  // namespace

XT_GOLDEN_FIXTURE(scene_stroke_caps, 256, 256, 1.0f, 0.0f, (Color{0, 0, 0, 1}), buildSceneStrokeCaps) {
    compile::SceneBuilder scene;
    scene.width = 256;
    scene.height = 256;

    uint32_t buttPath = addStrokeLine(scene, 64, LineCap::Butt);
    uint32_t roundPath = addStrokeLine(scene, 128, LineCap::Round);
    uint32_t squarePath = addStrokeLine(scene, 192, LineCap::Square);

    uint32_t root = addNode(scene, NodeKind::Group, kRootParent);
    addNode(scene, NodeKind::Path, root, buttPath);
    addNode(scene, NodeKind::Path, root, roundPath);
    addNode(scene, NodeKind::Path, root, squarePath);
    writer.addScene(scene);
}
