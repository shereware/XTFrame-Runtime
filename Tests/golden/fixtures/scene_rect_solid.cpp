#include "../golden_common.h"

/// A single rectangle Path, solid red fill, NonZero rule. See Docs/tasks/T-13.md fixture table.

using namespace xt;
using namespace xt::golden;

XT_GOLDEN_FIXTURE(scene_rect_solid, 256, 256, 1.0f, 0.0f, (Color{0, 0, 0, 1}), buildSceneRectSolid) {
    compile::SceneBuilder scene;
    scene.width = 256;
    scene.height = 256;

    PathSpec rect;
    rect.verbs = {PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Line, PathVerb::Close};
    rect.points = {48, 48, 208, 48, 208, 208, 48, 208};
    rect.fillRule = FillRule::NonZero;
    rect.fillKind = PaintKind::Solid;
    rect.fillColor = Color{1, 0, 0, 1};
    uint32_t pathIndex = addPath(scene, rect);

    addNode(scene, NodeKind::Path, kRootParent, pathIndex);
    writer.addScene(scene);
}
