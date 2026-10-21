#include <cmath>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/command_list.h"
#include "xt/command_simplifier.h"
#include "xt/compile/ir_writer.h"
#include "xt/effect_utils.h"
#include "xt/ir.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/painter.h"
#include "xt/property.h"
#include "xt/scene_instance.h"
#include "xt/types.h"
#include "xt/value.h"

/// Painter effect tests (docs/tasks/T-52.md "測試"): builds NodeArena Element/scene subtrees
/// directly (same approach as test_painter_element.cpp/test_painter_3d.cpp -- no Document/
/// LayoutEngine), assigns effects via NodeArena::assignEffects()/IrWriter's SCEN effects pool, and
/// checks dumpCommandList/CommandList::effectCmds output against docs/design/18-effects.md §18.1/
/// §18.5.

using namespace xt;
using namespace xt::compile;

namespace {

NodeId makeElement(NodeArena& arena, NodeId parent) {
    NodeId id = arena.create(NodeKind::Element);
    if (parent != kInvalidNode) {
        arena.appendChild(parent, id);
    }
    return id;
}

/// Force-updates world transforms, paints `root`'s subtree into `cmds` (a large enough viewport
/// so EffectCmd::boundsHint's viewport clamp is a no-op unless the test wants it to clamp) and
/// returns the dump string.
std::string paintDump(NodeArena& arena, NodeId root, CommandList& cmds) {
    cmds.viewport = Rect{-10000, -10000, 20000, 20000};
    updateWorldTransforms(arena, root, /*force=*/true);
    Painter painter;
    painter.paint(arena, root, cmds);
    std::string out;
    dumpCommandList(cmds, out);
    return out;
}

EffectInstance makeBlur(float radius) {
    EffectInstance e{};
    e.kind = EffectKind::Blur;
    e.enabled = 1;
    e.params[0] = radius;
    e.params[1] = radius;
    return e;
}

EffectInstance makeShadow(float offsetX, float offsetY, float blur, float spread, Color color) {
    EffectInstance e{};
    e.kind = EffectKind::DropShadow;
    e.enabled = 1;
    e.params[0] = offsetX;
    e.params[1] = offsetY;
    e.params[2] = blur;
    e.params[3] = spread;
    e.params[4] = 1.0f;  // opacity
    e.colors[0] = color;
    return e;
}

}  // namespace

TEST_CASE("Painter effects 1: Element blur wraps background FillPath, no PushLayer") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& data = arena.element(root);
    data.box.width = 100;
    data.box.height = 50;
    data.style.base[prop::BackgroundColor] = Value::ofColor(Color{1, 0, 0, 1});

    EffectInstance blur = makeBlur(4);
    REQUIRE(arena.assignEffects(root, std::span<const EffectInstance>(&blur, 1), {}) == Status::kOk);

    CommandList cmds;
    std::string out = paintDump(arena, root, cmds);

    CHECK(out.find("PushLayer") == std::string::npos);
    size_t pushPos = out.find("PushEffect count=1");
    size_t fillPos = out.find("FillPath");
    size_t popPos = out.find("PopEffect");
    REQUIRE(pushPos != std::string::npos);
    REQUIRE(fillPos != std::string::npos);
    REQUIRE(popPos != std::string::npos);
    CHECK(pushPos < fillPos);
    CHECK(fillPos < popPos);
}

TEST_CASE("Painter effects 2: Element blur + opacity 0.5 -> Layer wraps Effect, paint alpha stays 1") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& data = arena.element(root);
    data.box.width = 100;
    data.box.height = 50;
    data.style.base[prop::BackgroundColor] = Value::ofColor(Color{1, 0, 0, 1});
    arena.common(root).opacity = 0.5f;

    EffectInstance blur = makeBlur(4);
    REQUIRE(arena.assignEffects(root, std::span<const EffectInstance>(&blur, 1), {}) == Status::kOk);

    CommandList cmds;
    std::string out = paintDump(arena, root, cmds);

    size_t layerPos = out.find("PushLayer opacity=0.500");
    size_t pushPos = out.find("PushEffect");
    size_t popEffectPos = out.find("PopEffect");
    size_t popLayerPos = out.find("PopLayer");
    REQUIRE(layerPos != std::string::npos);
    REQUIRE(pushPos != std::string::npos);
    REQUIRE(popEffectPos != std::string::npos);
    REQUIRE(popLayerPos != std::string::npos);
    CHECK(layerPos < pushPos);
    CHECK(popEffectPos < popLayerPos);

    REQUIRE(cmds.fills.size() == 1);
    CHECK(cmds.fills[0].paint.color.a == doctest::Approx(1.0f));
}

TEST_CASE("Painter effects 3: overflow hidden + shadow -> PushClipRect is inside PushEffect") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& data = arena.element(root);
    data.box.width = 100;
    data.box.height = 50;
    data.style.overflowX = data.style.overflowY = Overflow::Hidden;
    NodeId child = makeElement(arena, root);
    ElementData& childData = arena.element(child);
    childData.box.width = 20;
    childData.box.height = 20;
    childData.style.base[prop::BackgroundColor] = Value::ofColor(Color{0, 1, 0, 1});

    EffectInstance shadow = makeShadow(10, 0, 4, 0, Color{0, 0, 0, 1});
    REQUIRE(arena.assignEffects(root, std::span<const EffectInstance>(&shadow, 1), {}) == Status::kOk);

    CommandList cmds;
    std::string out = paintDump(arena, root, cmds);

    size_t pushEffectPos = out.find("PushEffect");
    size_t pushClipPos = out.find("PushClipRect");
    size_t popEffectPos = out.find("PopEffect");
    REQUIRE(pushEffectPos != std::string::npos);
    REQUIRE(pushClipPos != std::string::npos);
    REQUIRE(popEffectPos != std::string::npos);
    CHECK(pushEffectPos < pushClipPos);
    CHECK(pushClipPos < popEffectPos);
}

TEST_CASE("Painter effects 4: boundsHint = content AABB outset by shadow's expansion") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& data = arena.element(root);
    data.box.width = 100;
    data.box.height = 50;
    data.style.base[prop::BackgroundColor] = Value::ofColor(Color{1, 1, 1, 1});
    arena.common(root).translate = Vec2{20, 20};

    // shadow(10px 0 4px #000): expand = max(|10|,|0|) + spread(0) + 3*sigma(blur/2=2) = 10+0+6=16.
    EffectInstance shadow = makeShadow(10, 0, 4, 0, Color{0, 0, 0, 1});
    REQUIRE(arena.assignEffects(root, std::span<const EffectInstance>(&shadow, 1), {}) == Status::kOk);

    CommandList cmds;
    paintDump(arena, root, cmds);

    REQUIRE(cmds.effectCmds.size() == 1);
    const EffectCmd& ec = cmds.effectCmds[0];
    CHECK(ec.contentBounds.x == doctest::Approx(20.0f));
    CHECK(ec.contentBounds.y == doctest::Approx(20.0f));
    CHECK(ec.contentBounds.w == doctest::Approx(100.0f));
    CHECK(ec.contentBounds.h == doctest::Approx(50.0f));
    CHECK(ec.boundsHint.x == doctest::Approx(4.0f));
    CHECK(ec.boundsHint.y == doctest::Approx(4.0f));
    CHECK(ec.boundsHint.w == doctest::Approx(132.0f));
    CHECK(ec.boundsHint.h == doctest::Approx(82.0f));
}

// T-111: an inset shadow stays inside the shape, so effectExpand() returns 0 and boundsHint is
// exactly the content AABB (docs/design/18-effects.md §18.3's expansion column).
TEST_CASE("Painter effects 4b: inset shadow expands nothing") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& data = arena.element(root);
    data.box.width = 100;
    data.box.height = 50;
    data.style.base[prop::BackgroundColor] = Value::ofColor(Color{1, 1, 1, 1});
    arena.common(root).translate = Vec2{20, 20};

    EffectInstance shadow = makeShadow(10, 0, 4, 2, Color{0, 0, 0, 1});
    shadow.params[5] = 1;  // inset
    REQUIRE(arena.assignEffects(root, std::span<const EffectInstance>(&shadow, 1), {}) == Status::kOk);
    CHECK(effectExpand(shadow) == doctest::Approx(0.0f));

    CommandList cmds;
    paintDump(arena, root, cmds);

    REQUIRE(cmds.effectCmds.size() == 1);
    const EffectCmd& ec = cmds.effectCmds[0];
    CHECK(ec.boundsHint.x == doctest::Approx(20.0f));
    CHECK(ec.boundsHint.y == doctest::Approx(20.0f));
    CHECK(ec.boundsHint.w == doctest::Approx(100.0f));
    CHECK(ec.boundsHint.h == doctest::Approx(50.0f));
}

TEST_CASE("Painter effects 5: transform scale(2) + blur(4px) -> xf linear part is 2x, screen radius 8") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& data = arena.element(root);
    data.box.width = 100;
    data.box.height = 50;
    data.style.base[prop::BackgroundColor] = Value::ofColor(Color{1, 1, 1, 1});
    arena.common(root).scale = Vec2{2, 2};

    EffectInstance blur = makeBlur(4);
    REQUIRE(arena.assignEffects(root, std::span<const EffectInstance>(&blur, 1), {}) == Status::kOk);

    CommandList cmds;
    paintDump(arena, root, cmds);

    REQUIRE(cmds.effectCmds.size() == 1);
    const EffectCmd& ec = cmds.effectCmds[0];
    CHECK(ec.xf.a == doctest::Approx(2.0f));
    CHECK(ec.xf.d == doctest::Approx(2.0f));

    REQUIRE(ec.effectCount == 1);
    EffectInstance screenParams = effectToScreen(cmds.effects[ec.effectOffset], ec.xf);
    CHECK(screenParams.params[0] == doctest::Approx(8.0f));
}

TEST_CASE(
    "Painter effects 6: 3D layer (rotateY 60deg, perspective) -> xf is a Jacobian (det > 0, x-scale < 1)") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& rootData = arena.element(root);
    rootData.box.width = 200;
    rootData.box.height = 200;
    rootData.perspectivePx = 300;

    NodeId child = arena.create(NodeKind::Path);
    arena.appendChild(root, child);
    PathNodeData& path = arena.path(child);
    path.verbs = {PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Close};
    path.points = {0, 0, 10, 0, 10, 10};
    path.fill.kind = PaintKind::Solid;
    path.fill.color = Color{1, 1, 1, 1};

    NodeCommon& childCommon = arena.common(child);
    childCommon.is3D = 1;
    childCommon.rotationY = 60;

    EffectInstance blur = makeBlur(4);
    REQUIRE(arena.assignEffects(child, std::span<const EffectInstance>(&blur, 1), {}) == Status::kOk);

    CommandList cmds;
    paintDump(arena, root, cmds);

    REQUIRE(cmds.effectCmds.size() == 1);
    const EffectCmd& ec = cmds.effectCmds[0];
    float det = ec.xf.a * ec.xf.d - ec.xf.b * ec.xf.c;
    CHECK(det > 0.0f);
    CHECK(std::fabs(ec.xf.a) < 1.0f);
}

TEST_CASE("Painter effects 7: scene Group with SCEN effects -> same order as test 1") {
    IrWriter writer;
    SceneBuilder scene;
    scene.width = 100;
    scene.height = 100;

    IrEffect irEffect{};
    irEffect.kind = static_cast<uint8_t>(EffectKind::Blur);
    irEffect.enabled = 1;
    irEffect.shader = kInvalidAsset;
    irEffect.params[0] = 4;
    irEffect.params[1] = 4;
    uint32_t effectIndex = IrWriter::addSceneEffect(scene, irEffect);
    CHECK(effectIndex == 0);

    IrPathData p{};
    p.verbOffset = 0;
    scene.verbPool.push_back(static_cast<uint8_t>(PathVerb::Move));
    scene.verbPool.push_back(static_cast<uint8_t>(PathVerb::Line));
    scene.verbPool.push_back(static_cast<uint8_t>(PathVerb::Line));
    scene.verbPool.push_back(static_cast<uint8_t>(PathVerb::Close));
    p.verbCount = 4;
    p.pointOffset = 0;
    float pts[] = {0, 0, 10, 0, 10, 10};
    scene.floatPool.insert(scene.floatPool.end(), std::begin(pts), std::end(pts));
    p.pointCount = 6;
    p.fillRule = static_cast<uint8_t>(FillRule::NonZero);
    p.fillKind = static_cast<uint8_t>(PaintKind::Solid);
    p.fillColor = Color{1, 0, 0, 1};
    p.strokeKind = static_cast<uint8_t>(PaintKind::None);
    p.trimStart = 0.0f;
    p.trimEnd = 1.0f;
    scene.paths.push_back(p);

    IrSceneNode root{};
    root.kind = static_cast<uint8_t>(NodeKind::Group);
    root.visible = 1;
    root.parent = 0xFFFFFFFFu;
    root.opacity = 1;
    root.scale = Vec2{1, 1};
    root.name = kInvalidString;
    root.effectOffset = effectIndex;
    root.effectCount = 1;
    scene.nodes.push_back(root);

    IrSceneNode pathNode{};
    pathNode.kind = static_cast<uint8_t>(NodeKind::Path);
    pathNode.visible = 1;
    pathNode.parent = 0;
    pathNode.kindIndex = 0;
    pathNode.opacity = 1;
    pathNode.scale = Vec2{1, 1};
    pathNode.name = kInvalidString;
    scene.nodes.push_back(pathNode);

    SceneId sceneId = writer.addScene(scene);
    std::vector<uint8_t> bytes = writer.finish();
    Result<IrView> r = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(r.ok());
    const IrView& view = r.value();

    NodeArena arena;
    NodeId container = arena.create(NodeKind::Group);
    SceneInstance inst;
    REQUIRE(instantiateScene(view, sceneId, arena, container, inst) == Status::kOk);
    updateWorldTransforms(arena, inst.root, true);

    CommandList cmds;
    cmds.viewport = Rect{-10000, -10000, 20000, 20000};
    Painter painter;
    painter.paint(arena, inst.root, cmds);
    std::string out;
    dumpCommandList(cmds, out);

    CHECK(out.find("PushLayer") == std::string::npos);
    size_t pushPos = out.find("PushEffect count=1");
    size_t fillPos = out.find("FillPath");
    size_t popPos = out.find("PopEffect");
    REQUIRE(pushPos != std::string::npos);
    REQUIRE(fillPos != std::string::npos);
    REQUIRE(popPos != std::string::npos);
    CHECK(pushPos < fillPos);
    CHECK(fillPos < popPos);
}

TEST_CASE("Painter effects 8: simplifier drops PushEffect/PopEffect, droppedEffects == 1") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& data = arena.element(root);
    data.box.width = 100;
    data.box.height = 50;
    data.style.base[prop::BackgroundColor] = Value::ofColor(Color{1, 0, 0, 1});

    EffectInstance blur = makeBlur(4);
    REQUIRE(arena.assignEffects(root, std::span<const EffectInstance>(&blur, 1), {}) == Status::kOk);

    CommandList cmds;
    paintDump(arena, root, cmds);
    REQUIRE(cmds.effectCmds.size() == 1);

    CommandList simplified = cmds;
    CommandListSimplifier simplifier;
    SimplifyOptions options;
    options.flatten3D = false;
    SimplifyStats stats = simplifier.simplify(simplified, options, nullptr);
    CHECK(stats.droppedEffects == 1);

    std::string out;
    dumpCommandList(simplified, out);
    CHECK(out.find("PushEffect") == std::string::npos);
    CHECK(out.find("PopEffect") == std::string::npos);
    CHECK(out.find("FillPath") != std::string::npos);
}

TEST_CASE("Painter effects 9: document with no effects -> byte-identical output (regression)") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& data = arena.element(root);
    data.box.width = 100;
    data.box.height = 50;
    data.style.base[prop::BackgroundColor] = Value::ofColor(Color{1, 0, 0, 1});
    data.box.border = Edges{4, 4, 4, 4};
    data.style.base[prop::BorderColor] = Value::ofColor(Color{0.2f, 0.2f, 0.2f, 1});

    CommandList cmds;
    std::string out = paintDump(arena, root, cmds);

    CHECK(out.find("PushEffect") == std::string::npos);
    CHECK(out.find("PopEffect") == std::string::npos);
    CHECK(cmds.effectCmds.empty());
    CHECK(cmds.effects.empty());
}

// T-160 (bench BUG-6, `#boot-scan`): an Element whose only visual is a `background-gradient` is a
// drawable item too. It used to count as 0 drawables, so its `shadow()` was never pushed and the
// glow of the bench's sweep line disappeared.
TEST_CASE("Painter effects 10: a gradient-only Element still gets its shadow") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& data = arena.element(root);
    data.box.width = 1280;
    data.box.height = 3;
    ElementGradient& g = data.backgroundGradient;
    g.kind = GradientKind::Linear;
    g.angle = 90;
    g.stops.push_back(GradientStop{0.0f, Color{0.6f, 1, 0.75f, 0}});
    g.stops.push_back(GradientStop{0.5f, Color{0.8f, 1, 0.86f, 0.93f}});
    g.stops.push_back(GradientStop{1.0f, Color{0.6f, 1, 0.75f, 0}});

    EffectInstance shadow = makeShadow(0, 0, 24, 4, Color{0.44f, 1, 0.62f, 0.53f});
    REQUIRE(arena.assignEffects(root, std::span<const EffectInstance>(&shadow, 1), {}) == Status::kOk);

    CommandList cmds;
    std::string out = paintDump(arena, root, cmds);

    size_t pushPos = out.find("PushEffect count=1");
    size_t fillPos = out.find("FillPath");
    size_t popPos = out.find("PopEffect");
    REQUIRE(pushPos != std::string::npos);
    REQUIRE(fillPos != std::string::npos);
    REQUIRE(popPos != std::string::npos);
    CHECK(pushPos < fillPos);
    CHECK(fillPos < popPos);
    REQUIRE(cmds.effectCmds.size() == 1);
    // The glow reaches past the 3 px line: boundsHint is outset by the shadow's expansion.
    CHECK(cmds.effectCmds[0].boundsHint.h > 3.0f + 2.0f * 24.0f);
}

// T-160: with opacity < 1 the Layer wraps the Effect (18.1). The Layer's boundsHint used to be the
// content AABB alone, so the SW/GL bounded layer clipped the shadow's glow to the element's box;
// it must cover the effect's boundsHint.
TEST_CASE("Painter effects 11: Layer around an Effect covers the effect's outset") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& data = arena.element(root);
    data.box.width = 200;
    data.box.height = 3;
    data.style.base[prop::BackgroundColor] = Value::ofColor(Color{1, 1, 1, 1});
    arena.common(root).opacity = 0.5f;

    EffectInstance shadow = makeShadow(0, 0, 24, 4, Color{0, 1, 0, 0.5f});
    REQUIRE(arena.assignEffects(root, std::span<const EffectInstance>(&shadow, 1), {}) == Status::kOk);

    CommandList cmds;
    paintDump(arena, root, cmds);

    REQUIRE(cmds.layers.size() == 1);
    REQUIRE(cmds.effectCmds.size() == 1);
    const Rect layer = cmds.layers[0].boundsHint;
    const Rect effect = cmds.effectCmds[0].boundsHint;
    CHECK(effect.h > 3.0f);
    CHECK(layer.x <= effect.x);
    CHECK(layer.y <= effect.y);
    CHECK(layer.x + layer.w >= effect.x + effect.w);
    CHECK(layer.y + layer.h >= effect.y + effect.h);
}
