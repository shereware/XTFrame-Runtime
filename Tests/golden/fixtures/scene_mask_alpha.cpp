#include "../golden_common.h"

/// A circular alpha mask over a square. PushMask/EndMaskContent/PopMask are ignored by the SW
/// renderer until T-12 (Code/Runtime/sw/src/sw_renderer.cpp render), so the mask's own circle and the masked
/// square are both drawn unconditionally in this fixture's expected image (pre-T-12 result) --
/// re-`--update` once T-12 lands. See Docs/tasks/T-13.md fixture table.

using namespace xt;
using namespace xt::golden;

XT_GOLDEN_FIXTURE(scene_mask_alpha, 256, 256, 1.0f, 0.0f, (Color{0, 0, 0, 1}), buildSceneMaskAlpha) {
    compile::SceneBuilder scene;
    scene.width = 256;
    scene.height = 256;

    PathSpec maskCircle;
    appendCircle(maskCircle.verbs, maskCircle.points, Vec2{128, 128}, 80.0f);
    maskCircle.fillKind = PaintKind::Solid;
    maskCircle.fillColor = Color{1, 1, 1, 1};
    uint32_t maskPathIndex = addPath(scene, maskCircle);

    PathSpec square;
    square.verbs = {PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Line, PathVerb::Close};
    square.points = {48, 48, 208, 48, 208, 208, 48, 208};
    square.fillKind = PaintKind::Solid;
    square.fillColor = Color{1, 0.5f, 0, 1};
    uint32_t squarePathIndex = addPath(scene, square);

    uint32_t root = addNode(scene, NodeKind::Group, kRootParent);

    NodeOpts maskOpts;
    maskOpts.maskMode = static_cast<uint8_t>(MaskMode::Add);
    uint32_t mask = addNode(scene, NodeKind::Mask, root, 0, maskOpts);
    addNode(scene, NodeKind::Path, mask, maskPathIndex);

    addNode(scene, NodeKind::Path, root, squarePathIndex);
    writer.addScene(scene);
}
