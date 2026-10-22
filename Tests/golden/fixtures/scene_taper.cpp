#include "../golden_common.h"

/// T-233 (31 §31.4.7): variable-width strokes, drawn by the Painter as NonZero fills: a cubic
/// tapered at both ends (round caps degenerate to points), a line with the width profile
/// 0:1 0.5:2 1:1 (twice as wide in the middle), a polyline tapered at the start only (miter joins,
/// square end cap) and a closed circle whose taper is ignored but whose profile applies.

using namespace xt;
using namespace xt::golden;

XT_GOLDEN_FIXTURE(scene_taper, 256, 256, 1.0f, 0.0f, (Color{0, 0, 0, 1}), buildSceneTaper) {
    compile::SceneBuilder scene;
    scene.width = 256;
    scene.height = 256;

    PathSpec curve;
    curve.verbs = {PathVerb::Move, PathVerb::Cubic};
    curve.points = {20, 60, 70, -10, 150, 130, 236, 40};
    curve.fillKind = PaintKind::None;
    curve.strokeKind = PaintKind::Solid;
    curve.strokeColor = Color{1, 1, 1, 1};
    curve.strokeWidth = 16;
    curve.cap = LineCap::Round;
    curve.taperStart = 0.4f;
    curve.taperEnd = 0.4f;
    uint32_t curveIndex = addPath(scene, curve);

    PathSpec line;
    line.verbs = {PathVerb::Move, PathVerb::Line};
    line.points = {20, 120, 236, 120};
    line.fillKind = PaintKind::None;
    line.strokeKind = PaintKind::Solid;
    line.strokeColor = Color{1, 0.4f, 0.2f, 1};
    line.strokeWidth = 8;
    line.cap = LineCap::Butt;
    line.widthProfile = {0, 1, 0.5f, 2, 1, 1};
    uint32_t lineIndex = addPath(scene, line);

    PathSpec zigzag;
    zigzag.verbs = {PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Line};
    zigzag.points = {20, 230, 70, 160, 120, 230, 170, 160};
    zigzag.fillKind = PaintKind::None;
    zigzag.strokeKind = PaintKind::Solid;
    zigzag.strokeColor = Color{0.3f, 0.8f, 1, 1};
    zigzag.strokeWidth = 12;
    zigzag.cap = LineCap::Square;
    zigzag.join = LineJoin::Miter;
    zigzag.taperStart = 0.5f;
    uint32_t zigzagIndex = addPath(scene, zigzag);

    PathSpec ring;
    appendCircle(ring.verbs, ring.points, Vec2{210, 195}, 30.0f);
    ring.fillKind = PaintKind::None;
    ring.strokeKind = PaintKind::Solid;
    ring.strokeColor = Color{0.4f, 1, 0.4f, 1};
    ring.strokeWidth = 4;
    ring.taperStart = 0.5f;  // Ignored: closed contour.
    ring.widthProfile = {0, 1, 0.5f, 3, 1, 1};
    uint32_t ringIndex = addPath(scene, ring);

    uint32_t root = addNode(scene, NodeKind::Group, kRootParent);
    addNode(scene, NodeKind::Path, root, curveIndex);
    addNode(scene, NodeKind::Path, root, lineIndex);
    addNode(scene, NodeKind::Path, root, zigzagIndex);
    addNode(scene, NodeKind::Path, root, ringIndex);
    writer.addScene(scene);
}
