#include "../golden_common.h"

/// The same circle path, trimmed 4 different ways, one copy per quadrant: (0, 0.25),
/// (0.25, 0.75), (0.9, 0.2) (wrapping past 1), and the full range with offset 0.5. Trim is
/// applied by the Painter itself (already covered by Tests/unit/test_painter.cpp and
/// test_path_trim.cpp), so this fixture is expected to render correctly even at SW stage 1. See
/// Docs/tasks/T-13.md fixture table.

using namespace xt;
using namespace xt::golden;

namespace {

uint32_t addTrimmedCircle(compile::SceneBuilder& scene, const std::vector<PathVerb>& verbs,
                          const std::vector<float>& points, float trimStart, float trimEnd, float trimOffset,
                          Color color) {
    PathSpec spec;
    spec.verbs = verbs;
    spec.points = points;
    spec.fillKind = PaintKind::Solid;
    spec.fillColor = color;
    spec.trimStart = trimStart;
    spec.trimEnd = trimEnd;
    spec.trimOffset = trimOffset;
    return addPath(scene, spec);
}

}  // namespace

XT_GOLDEN_FIXTURE(scene_trim, 256, 256, 1.0f, 0.0f, (Color{0, 0, 0, 1}), buildSceneTrim) {
    compile::SceneBuilder scene;
    scene.width = 256;
    scene.height = 256;

    std::vector<PathVerb> circleVerbs;
    std::vector<float> circlePoints;
    appendCircle(circleVerbs, circlePoints, Vec2{0, 0}, 50.0f);

    uint32_t quarter =
        addTrimmedCircle(scene, circleVerbs, circlePoints, 0.0f, 0.25f, 0.0f, Color{1, 0, 0, 1});
    uint32_t half = addTrimmedCircle(scene, circleVerbs, circlePoints, 0.25f, 0.75f, 0.0f, Color{0, 1, 0, 1});
    uint32_t wrap =
        addTrimmedCircle(scene, circleVerbs, circlePoints, 0.9f, 0.2f, 0.0f, Color{0, 0.6f, 1, 1});
    uint32_t offset = addTrimmedCircle(scene, circleVerbs, circlePoints, 0.0f, 1.0f, 0.5f, Color{1, 1, 0, 1});

    uint32_t root = addNode(scene, NodeKind::Group, kRootParent);

    NodeOpts topLeft;
    topLeft.translate = Vec2{64, 64};
    addNode(scene, NodeKind::Path, root, quarter, topLeft);

    NodeOpts topRight;
    topRight.translate = Vec2{192, 64};
    addNode(scene, NodeKind::Path, root, half, topRight);

    NodeOpts bottomLeft;
    bottomLeft.translate = Vec2{64, 192};
    addNode(scene, NodeKind::Path, root, wrap, bottomLeft);

    NodeOpts bottomRight;
    bottomRight.translate = Vec2{192, 192};
    addNode(scene, NodeKind::Path, root, offset, bottomRight);

    writer.addScene(scene);
}
