#include "../golden_common.h"

/// T-225b (docs/design/31-vector-authoring.md §31.6.1): runtime shape modifiers, one group per
/// quadrant. Top left: a spoke repeated 12 times around the group origin, then a serial trim over
/// the expanded copies (0..0.75 keeps 9 spokes). Top right: a square Shape node rounded by
/// `round`. Bottom left: a stroked circle Shape zigzagged. Bottom right: a square path puckered
/// (amount -60: pinched, corners pushed out) next to a bloated copy (amount 50).

using namespace xt;
using namespace xt::golden;

namespace {

uint32_t addModifier(compile::SceneBuilder& scene, uint32_t parent, ModifierKind kind,
                     std::initializer_list<float> params, uint8_t mode = 0, float opacityA = 1,
                     float opacityB = 1) {
    IrModifierData m{};
    m.kind = static_cast<uint8_t>(kind);
    m.mode = mode;
    m.enabled = 1;
    int i = 0;
    for (float p : params) {
        m.params[i++] = p;
    }
    m.opacityA = opacityA;
    m.opacityB = opacityB;
    const uint32_t index = static_cast<uint32_t>(scene.modifiers.size());
    scene.modifiers.push_back(m);
    return addNode(scene, NodeKind::Modifier, parent, index);
}

uint32_t addShape(compile::SceneBuilder& scene, uint32_t parent, ShapeKind kind,
                  std::initializer_list<float> params, PaintKind fillKind, Color fill, PaintKind strokeKind,
                  Color stroke, float strokeWidth) {
    IrShapeData s{};
    s.kind = static_cast<uint8_t>(kind);
    int i = 0;
    for (float p : params) {
        s.params[i++] = p;
    }
    s.fillRule = static_cast<uint8_t>(FillRule::NonZero);
    s.fillKind = static_cast<uint8_t>(fillKind);
    s.fillColor = fill;
    s.strokeKind = static_cast<uint8_t>(strokeKind);
    s.strokeColor = stroke;
    s.cap = static_cast<uint8_t>(LineCap::Round);
    s.join = static_cast<uint8_t>(LineJoin::Round);
    s.strokeWidth = strokeWidth;
    s.miterLimit = 4;
    s.trimStart = 0;
    s.trimEnd = 1;
    s.trimOffset = 0;
    const uint32_t index = static_cast<uint32_t>(scene.shapes.size());
    scene.shapes.push_back(s);
    return addNode(scene, NodeKind::Shape, parent, index);
}

uint32_t addGroupAt(compile::SceneBuilder& scene, uint32_t parent, Vec2 at) {
    NodeOpts opts;
    opts.translate = at;
    return addNode(scene, NodeKind::Group, parent, 0, opts);
}

}  // namespace

XT_GOLDEN_FIXTURE(scene_modifiers, 256, 256, 1.0f, 0.0f, (Color{0, 0, 0, 1}), buildSceneModifiers) {
    compile::SceneBuilder scene;
    scene.width = 256;
    scene.height = 256;

    // Geometry first (addPath only fills the pools); nodes after the root.
    PathSpec spoke;
    spoke.verbs = {PathVerb::Move, PathVerb::Line};
    spoke.points = {0, -22, 0, -46};
    spoke.fillKind = PaintKind::None;
    spoke.strokeKind = PaintKind::Solid;
    spoke.strokeColor = Color{0.2f, 0.8f, 1, 1};
    spoke.strokeWidth = 7;
    spoke.cap = LineCap::Round;
    const uint32_t spokePath = addPath(scene, spoke);

    PathSpec square;
    square.verbs = {PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Line, PathVerb::Close};
    square.points = {-22, -22, 22, -22, 22, 22, -22, 22};
    square.fillColor = Color{1, 0.6f, 0.1f, 1};
    const uint32_t pinchPath = addPath(scene, square);
    square.fillColor = Color{0.9f, 0.3f, 0.6f, 1};
    const uint32_t bloatPath = addPath(scene, square);

    const uint32_t root = addNode(scene, NodeKind::Group, kRootParent);

    // Top left: repeat (expanded, a modifier follows) then serial trim across the copies.
    const uint32_t tl = addGroupAt(scene, root, Vec2{64, 64});
    addNode(scene, NodeKind::Path, tl, spokePath);
    addModifier(scene, tl, ModifierKind::Repeat, {12, 0, 0, 0, 0, 0, 30, 1}, 0, 1.0f, 0.35f);
    addModifier(scene, tl, ModifierKind::Trim, {0, 0.75f, 0}, /*serial*/ 1);

    // Top right: a square Shape node, rounded.
    const uint32_t tr = addGroupAt(scene, root, Vec2{192, 64});
    addShape(scene, tr, ShapeKind::Rect, {-40, -40, 80, 80, 0, 0}, PaintKind::Solid,
             Color{0.3f, 0.85f, 0.4f, 1}, PaintKind::None, Color{}, 0);
    addModifier(scene, tr, ModifierKind::Round, {18});

    // Bottom left: a stroked circle Shape, zigzagged (corner).
    const uint32_t bl = addGroupAt(scene, root, Vec2{64, 192});
    addShape(scene, bl, ShapeKind::Ellipse, {0, 0, 40, 40}, PaintKind::None, Color{}, PaintKind::Solid,
             Color{1, 0.85f, 0.2f, 1}, 3);
    addModifier(scene, bl, ModifierKind::ZigZag, {6, 5});

    // Bottom right: the same square pinched (left) and bloated (right).
    const uint32_t brA = addGroupAt(scene, root, Vec2{168, 192});
    NodeOpts half;
    half.scale = Vec2{0.8f, 0.8f};
    addNode(scene, NodeKind::Path, brA, pinchPath, half);
    addModifier(scene, brA, ModifierKind::Pucker, {-60});
    const uint32_t brB = addGroupAt(scene, root, Vec2{224, 192});
    addNode(scene, NodeKind::Path, brB, bloatPath, half);
    addModifier(scene, brB, ModifierKind::Pucker, {50});

    writer.addScene(scene);
}
