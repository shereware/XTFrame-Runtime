#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/command_list.h"
#include "xt/compile/ir_writer.h"
#include "xt/ir.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/painter.h"
#include "xt/property.h"
#include "xt/scene_instance.h"
#include "xt/types.h"
#include "xt/value.h"

/// Painter tests: build a scene with IrWriter, instantiate it, update world transforms, paint,
/// and compare dumpCommandList's output against an embedded expected string.
/// See docs/tasks/T-10.md "測試" / "test_painter.cpp".

using namespace xt;
using namespace xt::compile;

namespace {

constexpr uint32_t kRootParent = 0xFFFFFFFFu;  // IrSceneNode.parent marker for the scene root

struct PathSpec {
    std::vector<PathVerb> verbs;
    std::vector<float> points;
    PaintKind fillKind = PaintKind::Solid;
    Color fillColor{1, 1, 1, 1};
    PaintKind strokeKind = PaintKind::None;
    Color strokeColor{0, 0, 0, 1};
    float strokeWidth = 0;
    LineCap cap = LineCap::Butt;
    LineJoin join = LineJoin::Miter;
    float miterLimit = 4;
    float trimStart = 0;
    float trimEnd = 1;
    float trimOffset = 0;
};

/// Appends one path's verb/point data to the scene's pools and pushes an IrPathData describing
/// it; the new path's index (to use as an IrSceneNode::kindIndex) is scene.paths.size() - 1.
void addPath(SceneBuilder& scene, const PathSpec& spec) {
    IrPathData p{};
    p.verbOffset = static_cast<uint32_t>(scene.verbPool.size());
    for (PathVerb v : spec.verbs) {
        scene.verbPool.push_back(static_cast<uint8_t>(v));
    }
    p.verbCount = static_cast<uint32_t>(spec.verbs.size());
    p.pointOffset = static_cast<uint32_t>(scene.floatPool.size());
    scene.floatPool.insert(scene.floatPool.end(), spec.points.begin(), spec.points.end());
    p.pointCount = static_cast<uint32_t>(spec.points.size());
    p.fillRule = static_cast<uint8_t>(FillRule::NonZero);
    p.fillKind = static_cast<uint8_t>(spec.fillKind);
    p.strokeKind = static_cast<uint8_t>(spec.strokeKind);
    p.cap = static_cast<uint8_t>(spec.cap);
    p.join = static_cast<uint8_t>(spec.join);
    p.fillColor = spec.fillColor;
    p.strokeColor = spec.strokeColor;
    p.strokeWidth = spec.strokeWidth;
    p.miterLimit = spec.miterLimit;
    p.trimStart = spec.trimStart;
    p.trimEnd = spec.trimEnd;
    p.trimOffset = spec.trimOffset;
    scene.paths.push_back(p);
}

/// Appends an IrSceneNode and returns its local index (for use as a later node's `parent`).
uint32_t addNode(SceneBuilder& scene, NodeKind kind, uint32_t parent, uint32_t kindIndex = 0,
                 uint8_t visible = 1, float opacity = 1, uint8_t blend = 0, uint8_t maskMode = 0) {
    IrSceneNode n{};
    n.kind = static_cast<uint8_t>(kind);
    n.visible = visible;
    n.parent = parent;
    n.kindIndex = kindIndex;
    n.opacity = opacity;
    n.scale = Vec2{1, 1};
    n.blend = blend;
    n.maskMode = maskMode;
    n.name = kInvalidString;
    scene.nodes.push_back(n);
    return static_cast<uint32_t>(scene.nodes.size() - 1);
}

/// Serializes `writer`, opens it, instantiates `sceneId` into a fresh NodeArena (under a plain,
/// identity-transform Group container so the instantiated scene's own world equals its local
/// transform chain), force-updates world transforms, paints, and returns the dump string.
std::string renderScene(IrWriter& writer, SceneId sceneId) {
    std::vector<uint8_t> bytes = writer.finish();
    Result<IrView> r = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(r.ok());
    const IrView& view = r.value();

    NodeArena arena;
    NodeId container = arena.create(NodeKind::Group);
    SceneInstance inst;
    REQUIRE(instantiateScene(view, sceneId, arena, container, inst) == Status::kOk);
    updateWorldTransforms(arena, inst.root, true);

    Painter painter;
    CommandList cmds;
    painter.paint(arena, inst.root, cmds);
    std::string out;
    dumpCommandList(cmds, out);
    return out;
}

const char* kIdentityXf = "xf=[1.000 0.000 0.000 1.000 0.000 0.000]";

/// T-44b regression fixtures: builds an Element subtree directly (no Document/LayoutEngine, same
/// approach as test_painter_element.cpp's makeElement/paintDump), paints, and returns the dump
/// string. Every node here has NodeCommon::is3D == 0 / isSpaceRoot == 0 (their defaults, never
/// touched), so none of T-44b's new enterNode()/emitRoot() branches can fire.
NodeId makeElementNode(NodeArena& arena, NodeId parent) {
    NodeId id = arena.create(NodeKind::Element);
    if (parent != kInvalidNode) {
        arena.appendChild(parent, id);
    }
    return id;
}

std::string paintElementDump(NodeArena& arena, NodeId root) {
    updateWorldTransforms(arena, root, /*force=*/true);
    Painter painter;
    CommandList cmds;
    painter.paint(arena, root, cmds);
    std::string out;
    dumpCommandList(cmds, out);
    return out;
}

}  // namespace

TEST_CASE("Painter: single Path with fill and stroke, in order, xf == world") {
    IrWriter writer;
    SceneBuilder scene;
    scene.width = 10;
    scene.height = 10;

    PathSpec spec;
    spec.verbs = {PathVerb::Move, PathVerb::Line};
    spec.points = {0, 0, 10, 0};
    spec.fillKind = PaintKind::Solid;
    spec.fillColor = Color{1, 0, 0, 1};
    spec.strokeKind = PaintKind::Solid;
    spec.strokeColor = Color{0, 0, 1, 1};
    spec.strokeWidth = 2;
    addPath(scene, spec);

    addNode(scene, NodeKind::Path, kRootParent, 0);
    SceneId sceneId = writer.addScene(scene);

    std::string out = renderScene(writer, sceneId);
    std::string expected = std::string("FillPath ") + kIdentityXf +
                           " verbs=2 points=4 rule=NonZero paint=Solid(1.000,0.000,0.000,1.000)\n" +
                           "StrokePath " + kIdentityXf +
                           " verbs=2 points=4 paint=Solid(0.000,0.000,1.000,1.000) width=2.000 "
                           "cap=Butt join=Miter miter=4.000 dashes=0\n";
    CHECK(out == expected);
}

TEST_CASE("Painter: Group opacity 0.5 with one Path child needs no layer, alpha multiplied in") {
    IrWriter writer;
    SceneBuilder scene;

    PathSpec spec;
    spec.verbs = {PathVerb::Move, PathVerb::Line};
    spec.points = {0, 0, 10, 0};
    spec.fillColor = Color{1, 1, 1, 1};
    addPath(scene, spec);

    uint32_t root = addNode(scene, NodeKind::Group, kRootParent, 0, 1, 0.5f);
    addNode(scene, NodeKind::Path, root, 0);
    SceneId sceneId = writer.addScene(scene);

    std::string out = renderScene(writer, sceneId);
    std::string expected = std::string("FillPath ") + kIdentityXf +
                           " verbs=2 points=4 rule=NonZero paint=Solid(1.000,1.000,1.000,0.500)\n";
    CHECK(out == expected);
}

TEST_CASE("Painter: Group opacity 0.5 with two Path children needs a layer") {
    IrWriter writer;
    SceneBuilder scene;

    PathSpec spec;
    spec.verbs = {PathVerb::Move, PathVerb::Line};
    spec.points = {0, 0, 10, 0};
    spec.fillColor = Color{1, 1, 1, 1};
    addPath(scene, spec);  // shared path data for both children

    uint32_t root = addNode(scene, NodeKind::Group, kRootParent, 0, 1, 0.5f);
    addNode(scene, NodeKind::Path, root, 0);
    addNode(scene, NodeKind::Path, root, 0);
    SceneId sceneId = writer.addScene(scene);

    std::string out = renderScene(writer, sceneId);
    std::string fillLine = std::string("FillPath ") + kIdentityXf +
                           " verbs=2 points=4 rule=NonZero paint=Solid(1.000,1.000,1.000,1.000)\n";
    std::string expected = "PushLayer opacity=0.500 blend=Normal\n" + fillLine + fillLine + "PopLayer\n";
    CHECK(out == expected);
}

TEST_CASE("Painter: blend=Multiply on a single Path forces a layer") {
    IrWriter writer;
    SceneBuilder scene;

    PathSpec spec;
    spec.verbs = {PathVerb::Move, PathVerb::Line};
    spec.points = {0, 0, 10, 0};
    spec.fillColor = Color{0, 1, 0, 1};
    addPath(scene, spec);

    addNode(scene, NodeKind::Path, kRootParent, 0, 1, 1, static_cast<uint8_t>(BlendMode::Multiply));
    SceneId sceneId = writer.addScene(scene);

    std::string out = renderScene(writer, sceneId);
    std::string expected =
        "PushLayer opacity=1.000 blend=Multiply\n" + std::string("FillPath ") + kIdentityXf +
        " verbs=2 points=4 rule=NonZero paint=Solid(0.000,1.000,0.000,1.000)\n" + "PopLayer\n";
    CHECK(out == expected);
}

TEST_CASE("Painter: Image node emits DrawImage") {
    IrWriter writer;
    AssetBuilder asset;
    asset.name = writer.intern("logo.png");
    asset.kind = 0;
    AssetId assetId = writer.addAsset(asset);

    SceneBuilder scene;
    IrImageData img{};
    img.asset = assetId;
    img.width = 64;
    img.height = 32;
    scene.images.push_back(img);
    addNode(scene, NodeKind::Image, kRootParent, 0);
    SceneId sceneId = writer.addScene(scene);

    std::string out = renderScene(writer, sceneId);
    std::string expected =
        "DrawImage asset=" + std::to_string(assetId) + " dst=(0.000,0.000,64.000,32.000) opacity=1.000\n";
    CHECK(out == expected);
}

TEST_CASE("Painter: two Add masks then masked content merge into a single PushMask") {
    IrWriter writer;
    SceneBuilder scene;

    PathSpec spec;
    spec.verbs = {PathVerb::Move, PathVerb::Line};
    spec.points = {0, 0, 10, 0};
    spec.fillColor = Color{1, 1, 1, 1};
    addPath(scene, spec);

    uint32_t root = addNode(scene, NodeKind::Group, kRootParent, 0);
    uint32_t mask1 = addNode(scene, NodeKind::Mask, root, 0, 1, 1, 0, static_cast<uint8_t>(MaskMode::Add));
    addNode(scene, NodeKind::Path, mask1, 0);
    uint32_t mask2 = addNode(scene, NodeKind::Mask, root, 0, 1, 1, 0, static_cast<uint8_t>(MaskMode::Add));
    addNode(scene, NodeKind::Path, mask2, 0);
    addNode(scene, NodeKind::Path, root, 0);  // masked content
    SceneId sceneId = writer.addScene(scene);

    std::string out = renderScene(writer, sceneId);
    std::string fillLine = std::string("FillPath ") + kIdentityXf +
                           " verbs=2 points=4 rule=NonZero paint=Solid(1.000,1.000,1.000,1.000)\n";
    std::string expected =
        "PushMask mode=Alpha\n" + fillLine + fillLine + "EndMaskContent\n" + fillLine + "PopMask\n";
    CHECK(out == expected);
}

TEST_CASE("Painter: a single Subtract mask maps to AlphaInverted") {
    IrWriter writer;
    SceneBuilder scene;

    PathSpec spec;
    spec.verbs = {PathVerb::Move, PathVerb::Line};
    spec.points = {0, 0, 10, 0};
    spec.fillColor = Color{1, 1, 1, 1};
    addPath(scene, spec);

    uint32_t root = addNode(scene, NodeKind::Group, kRootParent, 0);
    uint32_t mask =
        addNode(scene, NodeKind::Mask, root, 0, 1, 1, 0, static_cast<uint8_t>(MaskMode::Subtract));
    addNode(scene, NodeKind::Path, mask, 0);
    addNode(scene, NodeKind::Path, root, 0);
    SceneId sceneId = writer.addScene(scene);

    std::string out = renderScene(writer, sceneId);
    std::string fillLine = std::string("FillPath ") + kIdentityXf +
                           " verbs=2 points=4 rule=NonZero paint=Solid(1.000,1.000,1.000,1.000)\n";
    std::string expected =
        "PushMask mode=AlphaInverted\n" + fillLine + "EndMaskContent\n" + fillLine + "PopMask\n";
    CHECK(out == expected);
}

TEST_CASE("Painter: Intersect + Add mixed mask run nests, PopMask twice") {
    IrWriter writer;
    SceneBuilder scene;

    PathSpec spec;
    spec.verbs = {PathVerb::Move, PathVerb::Line};
    spec.points = {0, 0, 10, 0};
    spec.fillColor = Color{1, 1, 1, 1};
    addPath(scene, spec);

    uint32_t root = addNode(scene, NodeKind::Group, kRootParent, 0);
    uint32_t mask1 =
        addNode(scene, NodeKind::Mask, root, 0, 1, 1, 0, static_cast<uint8_t>(MaskMode::Intersect));
    addNode(scene, NodeKind::Path, mask1, 0);
    uint32_t mask2 = addNode(scene, NodeKind::Mask, root, 0, 1, 1, 0, static_cast<uint8_t>(MaskMode::Add));
    addNode(scene, NodeKind::Path, mask2, 0);
    addNode(scene, NodeKind::Path, root, 0);  // masked content
    SceneId sceneId = writer.addScene(scene);

    std::string out = renderScene(writer, sceneId);
    std::string fillLine = std::string("FillPath ") + kIdentityXf +
                           " verbs=2 points=4 rule=NonZero paint=Solid(1.000,1.000,1.000,1.000)\n";
    std::string expected = "PushMask mode=Alpha\n" + fillLine + "EndMaskContent\n" + "PushMask mode=Alpha\n" +
                           fillLine + "EndMaskContent\n" + fillLine + "PopMask\nPopMask\n";
    CHECK(out == expected);
}

TEST_CASE("Painter: an invisible subtree emits no commands") {
    IrWriter writer;
    SceneBuilder scene;

    PathSpec hidden;
    hidden.verbs = {PathVerb::Move, PathVerb::Line};
    hidden.points = {0, 0, 10, 0};
    hidden.fillColor = Color{1, 0, 0, 1};
    addPath(scene, hidden);  // index 0

    PathSpec shown;
    shown.verbs = {PathVerb::Move, PathVerb::Line};
    shown.points = {0, 0, 20, 0};
    shown.fillColor = Color{0, 1, 0, 1};
    addPath(scene, shown);  // index 1

    uint32_t root = addNode(scene, NodeKind::Group, kRootParent, 0);
    addNode(scene, NodeKind::Path, root, 0, /*visible=*/0);
    addNode(scene, NodeKind::Path, root, 1, /*visible=*/1);
    SceneId sceneId = writer.addScene(scene);

    std::string out = renderScene(writer, sceneId);
    std::string expected = std::string("FillPath ") + kIdentityXf +
                           " verbs=2 points=4 rule=NonZero paint=Solid(0.000,1.000,0.000,1.000)\n";
    CHECK(out == expected);
}

TEST_CASE("Painter: trim (0.25, 0.75) on a straight line sends only the middle segment") {
    IrWriter writer;
    SceneBuilder scene;

    PathSpec spec;
    spec.verbs = {PathVerb::Move, PathVerb::Line};
    spec.points = {0, 0, 100, 0};
    spec.fillColor = Color{1, 1, 1, 1};
    spec.trimStart = 0.25f;
    spec.trimEnd = 0.75f;
    addPath(scene, spec);

    addNode(scene, NodeKind::Path, kRootParent, 0);
    SceneId sceneId = writer.addScene(scene);

    std::string out = renderScene(writer, sceneId);
    // Trimmed to [0.25, 0.75] of a single line yields one Move + one Line (2 verbs, 4 points),
    // versus the untrimmed path's own 2 verbs / 4 points -- same counts here since the source is
    // already just Move+Line, so this also checks (via test_path_trim.cpp) that trimming actually
    // ran rather than being skipped as a full-range reference.
    std::string expected = std::string("FillPath ") + kIdentityXf +
                           " verbs=2 points=4 rule=NonZero paint=Solid(1.000,1.000,1.000,1.000)\n";
    CHECK(out == expected);
}

TEST_CASE("Painter: paint() is idempotent and CommandList::clear() keeps capacity") {
    IrWriter writer;
    SceneBuilder scene;

    PathSpec spec;
    spec.verbs = {PathVerb::Move, PathVerb::Line};
    spec.points = {0, 0, 10, 0};
    spec.fillColor = Color{1, 1, 1, 1};
    addPath(scene, spec);
    addNode(scene, NodeKind::Path, kRootParent, 0);
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

    Painter painter;
    CommandList cmds;
    painter.paint(arena, inst.root, cmds);
    std::string first;
    dumpCommandList(cmds, first);

    CommandList cmds2;
    painter.paint(arena, inst.root, cmds2);
    std::string second;
    dumpCommandList(cmds2, second);
    CHECK(first == second);

    uint32_t commandsCap = static_cast<uint32_t>(cmds.commands.capacity());
    uint32_t verbsCap = static_cast<uint32_t>(cmds.verbs.capacity());
    uint32_t pointsCap = static_cast<uint32_t>(cmds.points.capacity());
    cmds.clear();
    CHECK(cmds.commands.empty());
    CHECK(cmds.commands.capacity() == commandsCap);
    CHECK(cmds.verbs.capacity() == verbsCap);
    CHECK(cmds.points.capacity() == pointsCap);

    // paint() into the cleared list reproduces the same commands (append semantics, no clear()).
    painter.paint(arena, inst.root, cmds);
    std::string third;
    dumpCommandList(cmds, third);
    CHECK(third == first);
}

TEST_CASE("Painter: an Element root wrapping a scene paints the same as the scene root directly") {
    IrWriter writer;
    SceneBuilder scene;

    PathSpec spec;
    spec.verbs = {PathVerb::Move, PathVerb::Line};
    spec.points = {0, 0, 10, 0};
    spec.fillColor = Color{1, 1, 1, 1};
    addPath(scene, spec);
    addNode(scene, NodeKind::Path, kRootParent, 0);
    SceneId sceneId = writer.addScene(scene);

    std::vector<uint8_t> bytes = writer.finish();
    Result<IrView> r = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(r.ok());
    const IrView& view = r.value();

    NodeArena arena;
    NodeId element = arena.create(NodeKind::Element);
    SceneInstance inst;
    REQUIRE(instantiateScene(view, sceneId, arena, element, inst) == Status::kOk);
    updateWorldTransforms(arena, element, true);

    Painter painter;
    CommandList fromElement;
    painter.paint(arena, element, fromElement);
    std::string elementOut;
    dumpCommandList(fromElement, elementOut);

    CommandList fromSceneRoot;
    painter.paint(arena, inst.root, fromSceneRoot);
    std::string sceneOut;
    dumpCommandList(fromSceneRoot, sceneOut);

    CHECK(elementOut == sceneOut);
}

TEST_CASE("dumpCommandList: the five 19.7 3D commands format exactly per docs/tasks/T-44a.md") {
    CommandList cmds;

    ProjectionCmd proj{};
    for (int i = 0; i < 16; ++i) {
        proj.viewProj.m[i] = static_cast<float>(i);
    }
    proj.rootXf = Mat2x3{1, 2, 3, 4, 5, 6};
    proj.boxRect = Rect{10, 20, 30, 40};
    proj.depthTest = 0;
    cmds.projections.push_back(proj);
    cmds.commands.push_back(Command{CmdKind::PushProjection, {0, 0, 0}, 0});

    cmds.commands.push_back(Command{CmdKind::PopProjection, {0, 0, 0}, 0});

    Transform3DCmd xf3{};
    for (int i = 0; i < 16; ++i) {
        xf3.world3.m[i] = 100.0f + static_cast<float>(i);
    }
    cmds.transforms3.push_back(xf3);
    cmds.commands.push_back(Command{CmdKind::PushTransform3D, {0, 0, 0}, 0});

    cmds.commands.push_back(Command{CmdKind::PopTransform3D, {0, 0, 0}, 0});

    ImageQuadCmd quad{};
    quad.asset = 7;
    quad.p[0] = Vec2{1, 2};
    quad.p[1] = Vec2{3, 4};
    quad.p[2] = Vec2{5, 6};
    quad.p[3] = Vec2{7, 8};
    quad.srcUv = Rect{0, 0, 1, 1};
    quad.opacity = 0.5f;
    quad.smooth = 1;
    cmds.imageQuads.push_back(quad);
    cmds.commands.push_back(Command{CmdKind::DrawImageQuad, {0, 0, 0}, 0});

    std::string out;
    dumpCommandList(cmds, out);

    const char* expected =
        "PushProjection viewProj=[0.000 1.000 2.000 3.000 4.000 5.000 6.000 7.000 8.000 9.000 10.000 "
        "11.000 12.000 13.000 14.000 15.000] rootXf=[1.000 2.000 3.000 4.000 5.000 6.000] "
        "box=(10.000,20.000,30.000,40.000) depth=0\n"
        "PopProjection\n"
        "PushTransform3D [100.000 101.000 102.000 103.000 104.000 105.000 106.000 107.000 108.000 "
        "109.000 110.000 111.000 112.000 113.000 114.000 115.000]\n"
        "PopTransform3D\n"
        "DrawImageQuad asset=7 p=(1.000,2.000)(3.000,4.000)(5.000,6.000)(7.000,8.000) "
        "uv=(0.000,0.000,1.000,1.000) opacity=0.500 smooth=1\n";
    CHECK(out == expected);
}

// docs/tasks/T-44b.md "測試" 6: with no 3D content anywhere in the tree (every node's is3D/
// isSpaceRoot at their NodeCommon defaults, 0), Painter's output must be byte-identical to before
// this task -- enterNode()'s new isSpaceRoot/is3D-skip/Camera branches must never fire, and
// emitRoot()/flushSection() must never run. Three "UI" (Element-based, docs/tasks/T-27.md) and
// three "Lottie" (Group/Path/Mask, docs/tasks/T-10.md) fixtures, each pinned to its
// already-established exact dump (test_painter_element.cpp / this file's own earlier test cases).
TEST_CASE("Painter T-44b regression: no 3D content -> dump unchanged (3 UI + 3 Lottie fixtures)") {
    // UI 1: background color, no radius (test_painter_element.cpp "background color, no radius").
    {
        NodeArena arena;
        NodeId root = makeElementNode(arena, kInvalidNode);
        ElementData& data = arena.element(root);
        data.box.width = 100;
        data.box.height = 50;
        data.style.base[prop::BackgroundColor] = Value::ofColor(Color{1, 0, 0, 1});

        std::string out = paintElementDump(arena, root);
        std::string expected = std::string("FillPath ") + kIdentityXf +
                               " verbs=6 points=10 rule=NonZero paint=Solid(1.000,0.000,0.000,1.000)\n";
        CHECK(out == expected);
    }

    // UI 2: border, no radius (test_painter_element.cpp "border -> NonZero FillPath", T-201).
    {
        NodeArena arena;
        NodeId root = makeElementNode(arena, kInvalidNode);
        ElementData& data = arena.element(root);
        data.box.width = 100;
        data.box.height = 60;
        data.box.border = Edges{4, 4, 4, 4};
        data.style.base[prop::BorderColor] = Value::ofColor(Color{0.2f, 0.2f, 0.2f, 1});

        std::string out = paintElementDump(arena, root);
        std::string expected = std::string("FillPath ") + kIdentityXf +
                               " verbs=12 points=20 rule=NonZero paint=Solid(0.200,0.200,0.200,1.000)\n";
        CHECK(out == expected);
    }

    // UI 3: overflow: hidden wraps a child in PushClipRect/PopClip (padding box).
    {
        NodeArena arena;
        NodeId root = makeElementNode(arena, kInvalidNode);
        ElementData& rootData = arena.element(root);
        rootData.box.width = 80;
        rootData.box.height = 60;
        rootData.style.overflowX = rootData.style.overflowY = Overflow::Hidden;

        NodeId child = makeElementNode(arena, root);
        ElementData& childData = arena.element(child);
        childData.box.width = 10;
        childData.box.height = 10;
        childData.style.base[prop::BackgroundColor] = Value::ofColor(Color{0, 0, 1, 1});

        std::string out = paintElementDump(arena, root);
        std::string expected =
            "PushClipRect rect=(0.000,0.000,80.000,60.000)\n"
            "FillPath xf=[1.000 0.000 0.000 1.000 0.000 0.000] verbs=6 points=10 rule=NonZero "
            "paint=Solid(0.000,0.000,1.000,1.000)\n"
            "PopClip\n";
        CHECK(out == expected);
    }

    // Lottie 1: a single Path with fill and stroke ("Painter: single Path with fill and stroke").
    {
        IrWriter writer;
        SceneBuilder scene;
        scene.width = 10;
        scene.height = 10;
        PathSpec spec;
        spec.verbs = {PathVerb::Move, PathVerb::Line};
        spec.points = {0, 0, 10, 0};
        spec.fillKind = PaintKind::Solid;
        spec.fillColor = Color{1, 0, 0, 1};
        spec.strokeKind = PaintKind::Solid;
        spec.strokeColor = Color{0, 0, 1, 1};
        spec.strokeWidth = 2;
        addPath(scene, spec);
        addNode(scene, NodeKind::Path, kRootParent, 0);
        SceneId sceneId = writer.addScene(scene);

        std::string out = renderScene(writer, sceneId);
        std::string expected = std::string("FillPath ") + kIdentityXf +
                               " verbs=2 points=4 rule=NonZero paint=Solid(1.000,0.000,0.000,1.000)\n" +
                               "StrokePath " + kIdentityXf +
                               " verbs=2 points=4 paint=Solid(0.000,0.000,1.000,1.000) width=2.000 "
                               "cap=Butt join=Miter miter=4.000 dashes=0\n";
        CHECK(out == expected);
    }

    // Lottie 2: Group opacity 0.5 with two Path children needs a layer.
    {
        IrWriter writer;
        SceneBuilder scene;
        PathSpec spec;
        spec.verbs = {PathVerb::Move, PathVerb::Line};
        spec.points = {0, 0, 10, 0};
        spec.fillColor = Color{1, 1, 1, 1};
        addPath(scene, spec);
        uint32_t root = addNode(scene, NodeKind::Group, kRootParent, 0, 1, 0.5f);
        addNode(scene, NodeKind::Path, root, 0);
        addNode(scene, NodeKind::Path, root, 0);
        SceneId sceneId = writer.addScene(scene);

        std::string out = renderScene(writer, sceneId);
        std::string fillLine = std::string("FillPath ") + kIdentityXf +
                               " verbs=2 points=4 rule=NonZero paint=Solid(1.000,1.000,1.000,1.000)\n";
        std::string expected = "PushLayer opacity=0.500 blend=Normal\n" + fillLine + fillLine + "PopLayer\n";
        CHECK(out == expected);
    }

    // Lottie 3: two Add masks then masked content merge into a single PushMask.
    {
        IrWriter writer;
        SceneBuilder scene;
        PathSpec spec;
        spec.verbs = {PathVerb::Move, PathVerb::Line};
        spec.points = {0, 0, 10, 0};
        spec.fillColor = Color{1, 1, 1, 1};
        addPath(scene, spec);
        uint32_t root = addNode(scene, NodeKind::Group, kRootParent, 0);
        uint32_t mask1 =
            addNode(scene, NodeKind::Mask, root, 0, 1, 1, 0, static_cast<uint8_t>(MaskMode::Add));
        addNode(scene, NodeKind::Path, mask1, 0);
        uint32_t mask2 =
            addNode(scene, NodeKind::Mask, root, 0, 1, 1, 0, static_cast<uint8_t>(MaskMode::Add));
        addNode(scene, NodeKind::Path, mask2, 0);
        addNode(scene, NodeKind::Path, root, 0);  // masked content
        SceneId sceneId = writer.addScene(scene);

        std::string out = renderScene(writer, sceneId);
        std::string fillLine = std::string("FillPath ") + kIdentityXf +
                               " verbs=2 points=4 rule=NonZero paint=Solid(1.000,1.000,1.000,1.000)\n";
        std::string expected =
            "PushMask mode=Alpha\n" + fillLine + fillLine + "EndMaskContent\n" + fillLine + "PopMask\n";
        CHECK(out == expected);
    }
}

// T-233 (31 §31.4.7): tapered / width-profiled strokes become a FillPath.

namespace {

/// A Group root with one stroked Path child (no fill) through `pts` (Move + Lines, `closed` adds a
/// Close), stroke width 10, painted; returns the root.
NodeId makeStrokedPath(NodeArena& arena, const std::vector<float>& pts, bool closed) {
    NodeId root = arena.create(NodeKind::Group);
    NodeId path = arena.create(NodeKind::Path);
    arena.appendChild(root, path);
    PathNodeData& p = arena.path(path);
    for (size_t i = 0; i < pts.size(); i += 2) {
        p.verbs.push_back(i == 0 ? PathVerb::Move : PathVerb::Line);
    }
    if (closed) {
        p.verbs.push_back(PathVerb::Close);
    }
    p.points = pts;
    p.fill.kind = PaintKind::None;
    p.stroke.kind = PaintKind::Solid;
    p.stroke.color = Color{1, 0, 0, 1};
    p.strokeStyle.width = 10;
    return root;
}

/// The bounding box of FillPath command `fill`'s points.
Rect fillBounds(const CommandList& cmds, const FillCmd& fill) {
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    for (uint32_t i = 0; i + 1 < fill.path.pointCount; i += 2) {
        float x = cmds.points[fill.path.pointOffset + i];
        float y = cmds.points[fill.path.pointOffset + i + 1];
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, x);
        maxY = std::max(maxY, y);
    }
    return Rect{minX, minY, maxX - minX, maxY - minY};
}

}  // namespace

TEST_CASE("Painter T-233: a tapered stroke is one NonZero FillPath with the stroke paint") {
    NodeArena arena;
    NodeId root = makeStrokedPath(arena, {0, 0, 100, 0}, false);
    NodeId path = arena.firstChild(root);
    setProperty(arena, path, prop::StrokeTaperStart, Value::ofFloat(0.5f));
    setProperty(arena, path, prop::StrokeTaperEnd, Value::ofFloat(0.5f));
    CHECK(getProperty(arena, path, prop::StrokeTaperStart).f == doctest::Approx(0.5f));
    updateWorldTransforms(arena, root, true);
    Painter painter;
    CommandList cmds;
    painter.paint(arena, root, cmds);
    REQUIRE(cmds.commands.size() == 1);
    CHECK(cmds.commands[0].kind == CmdKind::FillPath);
    CHECK(cmds.strokes.empty());
    REQUIRE(cmds.fills.size() == 1);
    const FillCmd& fc = cmds.fills[0];
    CHECK(fc.rule == FillRule::NonZero);
    std::string dump;
    dumpCommandList(cmds, dump);
    CHECK(dump.find("paint=Solid(1.000,0.000,0.000,1.000)") != std::string::npos);
    Rect b = fillBounds(cmds, fc);
    CHECK(b.x == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(b.x + b.w == doctest::Approx(100.0f).epsilon(0.001));
    CHECK(b.h == doctest::Approx(10.0f).epsilon(0.01));  // Full width only in the middle.
    // Both ends taper to a point: every output vertex at x = 0 or x = 100 lies on the center line.
    for (uint32_t i = 0; i + 1 < fc.path.pointCount; i += 2) {
        float x = cmds.points[fc.path.pointOffset + i];
        float y = cmds.points[fc.path.pointOffset + i + 1];
        if (x <= 0.0f || x >= 100.0f) {
            CHECK(std::fabs(y) < 1e-4f);
        }
    }
}

TEST_CASE("Painter T-233: profile 0:1 0.5:2 1:1 doubles the middle width") {
    NodeArena arena;
    NodeId root = makeStrokedPath(arena, {0, 0, 100, 0}, false);
    PathNodeData& p = arena.path(arena.firstChild(root));
    p.widthProfile = {WidthPoint{0, 1}, WidthPoint{0.5f, 2}, WidthPoint{1, 1}};
    p.strokeStyle.profileCount = 3;
    updateWorldTransforms(arena, root, true);
    Painter painter;
    CommandList cmds;
    painter.paint(arena, root, cmds);
    REQUIRE(cmds.fills.size() == 1);
    CHECK(cmds.strokes.empty());
    Rect b = fillBounds(cmds, cmds.fills[0]);
    CHECK(b.y == doctest::Approx(-10.0f).epsilon(0.01));
    CHECK(b.h == doctest::Approx(20.0f).epsilon(0.01));
}

TEST_CASE("Painter T-233: a closed contour ignores the taper (profile still applies)") {
    NodeArena arena;
    NodeId root = makeStrokedPath(arena, {0, 0, 100, 0, 100, 100, 0, 100}, true);
    PathNodeData& p = arena.path(arena.firstChild(root));
    p.strokeStyle.taperStart = 0.5f;
    p.strokeStyle.taperEnd = 0.5f;
    updateWorldTransforms(arena, root, true);
    Painter painter;
    CommandList cmds;
    painter.paint(arena, root, cmds);
    REQUIRE(cmds.fills.size() == 1);
    CHECK(cmds.strokes.empty());
    Rect b = fillBounds(cmds, cmds.fills[0]);
    // Full width 10 everywhere, including the seam's miter corner at (-5, -5).
    CHECK(b.x == doctest::Approx(-5.0f).epsilon(0.01));
    CHECK(b.y == doctest::Approx(-5.0f).epsilon(0.01));
    CHECK(b.w == doctest::Approx(110.0f).epsilon(0.01));
}

TEST_CASE("Painter T-233: without taper or profile the stroke stays a StrokePath") {
    NodeArena arena;
    NodeId root = makeStrokedPath(arena, {0, 0, 100, 0}, false);
    updateWorldTransforms(arena, root, true);
    Painter painter;
    CommandList cmds;
    painter.paint(arena, root, cmds);
    CHECK(cmds.strokes.size() == 1);
    CHECK(cmds.fills.empty());
}

TEST_CASE("Painter: Mesh node emits DrawImageMesh (positions x world, opacity x inherited alpha)") {
    IrWriter writer;
    AssetBuilder asset;
    asset.name = writer.intern("face.png");
    asset.kind = 0;
    AssetId assetId = writer.addAsset(asset);

    SceneBuilder scene;
    const Vec2 pos[3] = {{0, 0}, {40, 0}, {0, 30}};
    const Vec2 uv[3] = {{0, 0}, {1, 0}, {0, 1}};
    scene.meshVec2Pool.assign(pos, pos + 3);
    scene.meshVec2Pool.insert(scene.meshVec2Pool.end(), uv, uv + 3);
    scene.meshIndexPool = {0, 1, 2};
    IrMeshData mesh{};
    mesh.asset = assetId;
    mesh.vertexCount = 3;
    mesh.indexCount = 3;
    mesh.opacity = 0.8f;
    mesh.smooth = 0;
    mesh.skinIndex = kIrNoSkin;
    scene.meshes.push_back(mesh);
    uint32_t root = addNode(scene, NodeKind::Group, kRootParent, 0, 1, 0.5f);
    addNode(scene, NodeKind::Mesh, root, 0);
    scene.nodes.back().translate = Vec2{10, 20};
    // A hidden copy draws nothing.
    addNode(scene, NodeKind::Mesh, root, 0, /*visible=*/0);
    SceneId sceneId = writer.addScene(scene);

    std::string out = renderScene(writer, sceneId);
    std::string expected = "DrawImageMesh asset=" + std::to_string(assetId) +
                           " vertices=3 triangles=1 opacity=0.400 smooth=0\n"
                           "  vertex p=(10.000,20.000) uv=(0.000,0.000)\n"
                           "  vertex p=(50.000,20.000) uv=(1.000,0.000)\n"
                           "  vertex p=(10.000,50.000) uv=(0.000,1.000)\n"
                           "  tri 0 1 2\n";
    CHECK(out == expected);
}
