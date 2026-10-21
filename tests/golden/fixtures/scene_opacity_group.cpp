#include "../golden_common.h"

/// Group opacity 0.5 wrapping two overlapping squares. Two Path children force the Painter to
/// emit PushLayer/PopLayer (docs/design/09-command-list.md); the SW renderer ignores layers until
/// T-12, so this fixture's expected image records the pre-T-12 (group opacity not applied)
/// result, per docs/tasks/T-13.md fixture table -- re-`--update` once T-12 lands.

using namespace xt;
using namespace xt::golden;

XT_GOLDEN_FIXTURE(scene_opacity_group, 256, 256, 1.0f, 0.0f, (Color{0, 0, 0, 1}), buildSceneOpacityGroup) {
    compile::SceneBuilder scene;
    scene.width = 256;
    scene.height = 256;

    PathSpec squareA;
    squareA.verbs = {PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Line, PathVerb::Close};
    squareA.points = {68, 68, 148, 68, 148, 148, 68, 148};
    squareA.fillKind = PaintKind::Solid;
    squareA.fillColor = Color{1, 0, 0, 1};
    uint32_t pathA = addPath(scene, squareA);

    PathSpec squareB;
    squareB.verbs = {PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Line, PathVerb::Close};
    squareB.points = {108, 108, 188, 108, 188, 188, 108, 188};
    squareB.fillKind = PaintKind::Solid;
    squareB.fillColor = Color{0, 0, 1, 1};
    uint32_t pathB = addPath(scene, squareB);

    NodeOpts groupOpts;
    groupOpts.opacity = 0.5f;
    uint32_t group = addNode(scene, NodeKind::Group, kRootParent, 0, groupOpts);
    addNode(scene, NodeKind::Path, group, pathA);
    addNode(scene, NodeKind::Path, group, pathB);
    writer.addScene(scene);
}
