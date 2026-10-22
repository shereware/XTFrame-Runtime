#include "../golden_common.h"

/// Group(translate, rotation 30deg) -> Group(scale 1.5) -> rectangle Path. Exercises transform
/// composition order (world = outer.local * inner.local * path-local). See Docs/tasks/T-13.md
/// fixture table.

using namespace xt;
using namespace xt::golden;

XT_GOLDEN_FIXTURE(scene_transform_chain, 256, 256, 1.0f, 0.0f, (Color{0, 0, 0, 1}),
                  buildSceneTransformChain) {
    compile::SceneBuilder scene;
    scene.width = 256;
    scene.height = 256;

    PathSpec rect;
    rect.verbs = {PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Line, PathVerb::Close};
    rect.points = {-30, -30, 30, -30, 30, 30, -30, 30};
    rect.fillKind = PaintKind::Solid;
    rect.fillColor = Color{0, 1, 0, 1};
    uint32_t pathIndex = addPath(scene, rect);

    NodeOpts outerOpts;
    outerOpts.translate = Vec2{128, 128};
    outerOpts.rotation = 30;
    uint32_t outerGroup = addNode(scene, NodeKind::Group, kRootParent, 0, outerOpts);

    NodeOpts innerOpts;
    innerOpts.scale = Vec2{1.5f, 1.5f};
    uint32_t innerGroup = addNode(scene, NodeKind::Group, outerGroup, 0, innerOpts);

    addNode(scene, NodeKind::Path, innerGroup, pathIndex);
    writer.addScene(scene);
}
