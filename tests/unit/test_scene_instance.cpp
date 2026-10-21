#include <vector>

#include "doctest.h"
#include "xt/compile/ir_writer.h"
#include "xt/ir.h"
#include "xt/node_arena.h"
#include "xt/property.h"
#include "xt/scene_instance.h"
#include "xt/timeline.h"
#include "xt/types.h"

using namespace xt;
using namespace xt::compile;

namespace {

/// Everything needed to check the instantiated scene against the source IR.
struct Fixture {
    IrWriter writer;
    SceneId sceneId = kInvalidScene;
    TimelineId timelineId = kInvalidTimeline;
    StringId imageNameId = kInvalidString;
    IrSceneNode pathNodeSrc{};  // node index 1's source data, for the transform check
};

/// Everything needed to check a scene with a 3D node, a Camera node and scene effects.
struct Fixture3D {
    IrWriter writer;
    SceneId sceneId = kInvalidScene;
    IrSceneNode node3DSrc{};   // local index 1: is3D node with 3D fields set
    IrCameraData cameraSrc{};  // local index 2's camera data
    IrEffect effect0Src{};     // scene effect 0 (no stops), referenced by the 3D node
    IrEffect effect1Src{};     // scene effect 1 (2 stops, GradientMask), referenced by the 3D node
    std::vector<IrGradientStop> effect1StopsSrc;
};

/// Builds one IR scene: root Group -> [3D node (is3D, rotationX/orientation/translateZ, 2
/// effects), Camera]. Matches docs/tasks/T-43a.md's test_scene_instance description.
Fixture3D buildFixture3D() {
    Fixture3D f;
    IrWriter& w = f.writer;

    SceneBuilder scene;
    scene.name = w.intern("scene3d");
    scene.width = 300;
    scene.height = 150;

    IrSceneNode root{};
    root.kind = static_cast<uint8_t>(NodeKind::Group);
    root.visible = 1;
    root.parent = 0xFFFFFFFFu;
    root.opacity = 1;
    root.scale = Vec2{1, 1};
    root.name = w.intern("Root3D");
    scene.nodes.push_back(root);

    // Two scene-level effects: a plain DropShadow (no stops) and a GradientMask (2 stops).
    IrEffect effect0{};
    effect0.kind = static_cast<uint8_t>(EffectKind::DropShadow);
    effect0.enabled = 1;
    effect0.shader = kInvalidAsset;
    effect0.params[0] = 4;  // offsetX
    effect0.params[1] = 6;  // offsetY
    effect0.colors[0] = Color{0, 0, 0, 0.5f};
    f.effect0Src = effect0;
    IrWriter::addSceneEffect(scene, effect0);

    f.effect1StopsSrc = {IrGradientStop{0.0f, Color{1, 1, 1, 1}}, IrGradientStop{1.0f, Color{0, 0, 0, 1}}};
    scene.stopPool = f.effect1StopsSrc;
    IrEffect effect1{};
    effect1.kind = static_cast<uint8_t>(EffectKind::GradientMask);
    effect1.enabled = 1;
    effect1.shader = kInvalidAsset;
    effect1.stopOffset = 0;
    effect1.stopCount = 2;
    f.effect1Src = effect1;
    IrWriter::addSceneEffect(scene, effect1);

    IrSceneNode node3D{};
    node3D.kind = static_cast<uint8_t>(NodeKind::Group);
    node3D.visible = 1;
    node3D.parent = 0;
    node3D.opacity = 1;
    node3D.scale = Vec2{1, 1};
    node3D.name = w.intern("Node3D");
    node3D.translateZ = -40;
    node3D.anchorZ = 5;
    node3D.rotationX = 30;
    node3D.rotationY = 0;
    node3D.orientation = Vec3{1, 2, 3};
    node3D.is3D = 1;
    node3D.backfaceHidden = 1;
    node3D.effectOffset = 0;
    node3D.effectCount = 2;
    f.node3DSrc = node3D;
    scene.nodes.push_back(node3D);

    IrCameraData camera{};
    camera.kind = 1;  // Orthographic
    camera.useTarget = 0;
    camera.zoom = 800;
    camera.target = Vec3{50, 60, 70};
    camera.nearZ = 2;
    camera.farZ = 8000;
    f.cameraSrc = camera;
    uint32_t cameraIndex = IrWriter::addSceneCamera(scene, camera);

    IrSceneNode cameraNode{};
    cameraNode.kind = static_cast<uint8_t>(NodeKind::Camera);
    cameraNode.visible = 1;
    cameraNode.parent = 0;
    cameraNode.kindIndex = cameraIndex;
    cameraNode.opacity = 1;
    cameraNode.scale = Vec2{1, 1};
    cameraNode.translate = Vec2{100, 75};
    cameraNode.translateZ = 500;
    cameraNode.name = w.intern("Camera1");
    scene.nodes.push_back(cameraNode);

    f.sceneId = w.addScene(scene);
    return f;
}

/// Builds one IR: root Group -> [Path (2-stop fill gradient, 2-value dash, trim 0.2/0.8/0),
/// Group -> [Image, Mask(Alpha) -> [Path]]], plus a 1-track timeline animating local node 1's
/// Opacity. Matches docs/tasks/T-08.md's test description.
Fixture buildFixture() {
    Fixture f;
    IrWriter& w = f.writer;

    StringId sceneName = w.intern("scene1");
    StringId timelineName = w.intern("timeline1");
    StringId rootName = w.intern("Root");
    StringId pathName = w.intern("Path1");
    StringId groupName = w.intern("Group1");
    StringId imageName = w.intern("Image1");
    StringId maskName = w.intern("Mask1");
    StringId path2Name = w.intern("Path2");
    StringId assetName = w.intern("logo.png");
    f.imageNameId = imageName;

    AssetBuilder asset;
    asset.name = assetName;
    asset.kind = 0;  // Image
    asset.embedded = {0x89, 0x50, 0x4E, 0x47};
    AssetId assetId = w.addAsset(asset);

    SceneBuilder scene;
    scene.name = sceneName;
    scene.width = 100;
    scene.height = 200;

    IrSceneNode root{};
    root.kind = static_cast<uint8_t>(NodeKind::Group);
    root.visible = 1;
    root.parent = 0xFFFFFFFFu;
    root.opacity = 1;
    root.scale = Vec2{1, 1};
    root.name = rootName;
    scene.nodes.push_back(root);

    IrSceneNode pathNode{};
    pathNode.kind = static_cast<uint8_t>(NodeKind::Path);
    pathNode.visible = 1;
    pathNode.parent = 0;
    pathNode.kindIndex = 0;
    pathNode.opacity = 1;
    pathNode.anchor = Vec2{1, 2};
    pathNode.translate = Vec2{5, 0};
    pathNode.scale = Vec2{1, 1};
    pathNode.rotation = 15;
    pathNode.skew = Vec2{3, 4};
    pathNode.name = pathName;
    scene.nodes.push_back(pathNode);
    f.pathNodeSrc = pathNode;

    IrSceneNode groupNode{};
    groupNode.kind = static_cast<uint8_t>(NodeKind::Group);
    groupNode.visible = 1;
    groupNode.parent = 0;
    groupNode.opacity = 1;
    groupNode.translate = Vec2{10, 0};
    groupNode.scale = Vec2{1, 1};
    groupNode.name = groupName;
    scene.nodes.push_back(groupNode);

    IrSceneNode imageNode{};
    imageNode.kind = static_cast<uint8_t>(NodeKind::Image);
    imageNode.visible = 1;
    imageNode.parent = 2;
    imageNode.kindIndex = 0;
    imageNode.opacity = 1;
    imageNode.translate = Vec2{20, 0};
    imageNode.scale = Vec2{1, 1};
    imageNode.name = imageName;
    scene.nodes.push_back(imageNode);

    IrSceneNode maskNode{};
    maskNode.kind = static_cast<uint8_t>(NodeKind::Mask);
    maskNode.visible = 1;
    maskNode.maskMode = static_cast<uint8_t>(MaskMode::Alpha);
    maskNode.parent = 2;
    maskNode.opacity = 1;
    maskNode.translate = Vec2{30, 0};
    maskNode.scale = Vec2{1, 1};
    maskNode.name = maskName;
    scene.nodes.push_back(maskNode);

    IrSceneNode path2Node{};
    path2Node.kind = static_cast<uint8_t>(NodeKind::Path);
    path2Node.visible = 1;
    path2Node.parent = 4;
    path2Node.kindIndex = 1;
    path2Node.opacity = 1;
    path2Node.translate = Vec2{40, 0};
    path2Node.scale = Vec2{1, 1};
    path2Node.name = path2Name;
    scene.nodes.push_back(path2Node);

    scene.verbPool = {static_cast<uint8_t>(PathVerb::Move), static_cast<uint8_t>(PathVerb::Cubic),
                      static_cast<uint8_t>(PathVerb::Close)};
    scene.floatPool = {0, 0, 10, 0, 20, 10, 30, 0, 4.0f, 2.0f};  // points [0,8), dash [8,10)
    scene.stopPool = {IrGradientStop{0.0f, Color{1, 0, 0, 1}}, IrGradientStop{1.0f, Color{0, 0, 1, 1}}};

    IrPathData path1{};
    path1.verbOffset = 0;
    path1.verbCount = 3;
    path1.pointOffset = 0;
    path1.pointCount = 8;
    path1.fillRule = static_cast<uint8_t>(FillRule::NonZero);
    path1.fillKind = static_cast<uint8_t>(PaintKind::Linear);
    path1.strokeKind = static_cast<uint8_t>(PaintKind::None);
    path1.cap = static_cast<uint8_t>(LineCap::Round);
    path1.join = static_cast<uint8_t>(LineJoin::Bevel);
    path1.fillColor = Color{0, 0, 0, 1};
    path1.strokeColor = Color{1, 0, 0, 1};
    path1.fillP0 = Vec2{0, 0};
    path1.fillP1 = Vec2{10, 10};
    path1.fillStopOffset = 0;
    path1.fillStopCount = 2;
    path1.strokeStopOffset = 0;
    path1.strokeStopCount = 0;
    path1.strokeWidth = 2.0f;
    path1.miterLimit = 4.0f;
    path1.trimStart = 0.2f;
    path1.trimEnd = 0.8f;
    path1.trimOffset = 0.0f;
    path1.dashOffset = 8;
    path1.dashCount = 2;
    path1.dashPhase = 0.5f;
    scene.paths.push_back(path1);

    IrPathData path2{};  // Empty path under the Mask; no stops/dashes.
    path2.fillRule = static_cast<uint8_t>(FillRule::NonZero);
    path2.fillKind = static_cast<uint8_t>(PaintKind::None);
    path2.strokeKind = static_cast<uint8_t>(PaintKind::None);
    path2.cap = static_cast<uint8_t>(LineCap::Butt);
    path2.join = static_cast<uint8_t>(LineJoin::Miter);
    path2.miterLimit = 4.0f;
    path2.trimEnd = 1.0f;
    scene.paths.push_back(path2);

    IrImageData image{};
    image.asset = assetId;
    image.width = 64;
    image.height = 64;
    scene.images.push_back(image);

    f.sceneId = w.addScene(scene);

    TimelineBuilder tl;
    tl.name = timelineName;
    tl.duration = 1.0f;
    tl.frameRate = 30.0f;

    IrTrack opacityTrack{};
    opacityTrack.target = 1;  // local node index 1 (Path1)
    opacityTrack.prop = prop::Opacity;
    opacityTrack.type = static_cast<uint8_t>(ValueType::Float);
    opacityTrack.kfOffset = 0;
    opacityTrack.kfCount = 2;
    tl.tracks.push_back(opacityTrack);

    IrKeyframe kf0{};
    kf0.time = 0.0f;
    kf0.easing = IrEasing{static_cast<uint8_t>(EasingKind::Linear), 0, 0, 0, 0, 0, 0};
    irSetValueF32(kf0.value, 0, 0.0f);
    tl.keyframes.push_back(kf0);

    IrKeyframe kf1{};
    kf1.time = 1.0f;
    kf1.easing = IrEasing{static_cast<uint8_t>(EasingKind::Linear), 0, 0, 0, 0, 0, 0};
    irSetValueF32(kf1.value, 0, 1.0f);
    tl.keyframes.push_back(kf1);

    f.timelineId = w.addTimeline(tl);

    return f;
}

/// Preorder kind sequence of the subtree rooted at `root`.
std::vector<NodeKind> collectKinds(const NodeArena& arena, NodeId root) {
    std::vector<NodeKind> kinds;
    arena.traversePreorder(root, [&](NodeId n) {
        kinds.push_back(arena.kind(n));
        return true;
    });
    return kinds;
}

}  // namespace

TEST_CASE("instantiateScene: topology, counts, parent") {
    Fixture f = buildFixture();
    std::vector<uint8_t> bytes = f.writer.finish();
    Result<IrView> r = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(r.ok());
    const IrView& view = r.value();

    NodeArena arena;
    NodeId parent = arena.create(NodeKind::Group);
    uint32_t aliveBefore = arena.aliveCount();

    SceneInstance inst;
    Status status = instantiateScene(view, f.sceneId, arena, parent, inst);
    CHECK(status == Status::kOk);

    CHECK(arena.aliveCount() == aliveBefore + 6);
    REQUIRE(inst.localToArena.size() == 6);
    CHECK(arena.parent(inst.root) == parent);
    CHECK(inst.base == inst.root);

    std::vector<NodeKind> kinds = collectKinds(arena, inst.root);
    std::vector<NodeKind> expected = {NodeKind::Group, NodeKind::Path, NodeKind::Group,
                                      NodeKind::Image, NodeKind::Mask, NodeKind::Path};
    CHECK(kinds == expected);
}

TEST_CASE("instantiateScene: Path data copied (stops, dashes, trim)") {
    Fixture f = buildFixture();
    std::vector<uint8_t> bytes = f.writer.finish();
    Result<IrView> r = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(r.ok());
    const IrView& view = r.value();
    const SceneChunk& chunk = view.scene(f.sceneId);
    const IrPathData& srcPath = chunk.paths()[0];

    NodeArena arena;
    NodeId parent = arena.create(NodeKind::Group);
    SceneInstance inst;
    REQUIRE(instantiateScene(view, f.sceneId, arena, parent, inst) == Status::kOk);

    NodeId pathId = inst.localToArena[1];
    const PathNodeData& path = arena.path(pathId);

    REQUIRE(path.verbs.size() == srcPath.verbCount);
    for (uint32_t i = 0; i < srcPath.verbCount; ++i) {
        CHECK(path.verbs[i] == static_cast<PathVerb>(chunk.verbPool()[srcPath.verbOffset + i]));
    }
    REQUIRE(path.points.size() == srcPath.pointCount);
    for (uint32_t i = 0; i < srcPath.pointCount; ++i) {
        CHECK(path.points[i] == doctest::Approx(chunk.floatPool()[srcPath.pointOffset + i]));
    }

    REQUIRE(path.stops.size() == 2);
    CHECK(path.fill.stopOffset == 0);
    CHECK(path.fill.stopCount == 2);
    CHECK(path.stops[0].offset == doctest::Approx(0.0f));
    CHECK(path.stops[1].offset == doctest::Approx(1.0f));

    REQUIRE(path.dashes.size() == 2);
    CHECK(path.dashes[0] == doctest::Approx(4.0f));
    CHECK(path.dashes[1] == doctest::Approx(2.0f));

    CHECK(path.trimStart == doctest::Approx(0.2f));
    CHECK(path.trimEnd == doctest::Approx(0.8f));
    CHECK(path.trimOffset == doctest::Approx(0.0f));
}

TEST_CASE("instantiateScene: node name") {
    Fixture f = buildFixture();
    std::vector<uint8_t> bytes = f.writer.finish();
    Result<IrView> r = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(r.ok());
    const IrView& view = r.value();

    NodeArena arena;
    NodeId parent = arena.create(NodeKind::Group);
    SceneInstance inst;
    REQUIRE(instantiateScene(view, f.sceneId, arena, parent, inst) == Status::kOk);

    CHECK(arena.name(inst.localToArena[3]) == f.imageNameId);
}

TEST_CASE("instantiateScene: local transform matches composeTRSS") {
    Fixture f = buildFixture();
    std::vector<uint8_t> bytes = f.writer.finish();
    Result<IrView> r = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(r.ok());
    const IrView& view = r.value();

    NodeArena arena;
    NodeId parent = arena.create(NodeKind::Group);
    SceneInstance inst;
    REQUIRE(instantiateScene(view, f.sceneId, arena, parent, inst) == Status::kOk);

    NodeId pathId = inst.localToArena[1];
    const NodeCommon& common = arena.common(pathId);
    const IrSceneNode& src = f.pathNodeSrc;
    Mat2x3 expected = composeTRSS(src.translate, src.rotation, src.skew, src.scale, src.anchor);
    CHECK(approxEqual(common.local, expected));
    CHECK(approxEqual(common.world, expected));
}

TEST_CASE(
    "instantiateScene: two instances are independent; destroy + reinstantiate leaves a "
    "non-contiguous but valid table") {
    Fixture f = buildFixture();
    std::vector<uint8_t> bytes = f.writer.finish();
    Result<IrView> r = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(r.ok());
    const IrView& view = r.value();

    NodeArena arena;
    NodeId parentA = arena.create(NodeKind::Group);
    NodeId parentB = arena.create(NodeKind::Group);

    SceneInstance instA;
    REQUIRE(instantiateScene(view, f.sceneId, arena, parentA, instA) == Status::kOk);
    SceneInstance instB;
    REQUIRE(instantiateScene(view, f.sceneId, arena, parentB, instB) == Status::kOk);

    NodeId pathA = resolveTrackTarget(instA, 1);
    NodeId pathB = resolveTrackTarget(instB, 1);
    REQUIRE(pathA != pathB);

    arena.common(pathA).opacity = 0.25f;
    CHECK(arena.common(pathA).opacity == doctest::Approx(0.25f));
    CHECK(arena.common(pathB).opacity == doctest::Approx(1.0f));

    uint32_t aliveBeforeDestroy = arena.aliveCount();
    NodeId rootA = instA.root;
    destroySceneInstance(arena, instA);
    CHECK(arena.aliveCount() == aliveBeforeDestroy - 6);
    CHECK(instA.root == kInvalidNode);
    CHECK(!arena.alive(rootA));

    // A third instantiation should reuse freed slots (non-contiguous localToArena), but every
    // local index must still resolve to an alive node of the expected kind.
    NodeId parentC = arena.create(NodeKind::Group);
    SceneInstance instC;
    REQUIRE(instantiateScene(view, f.sceneId, arena, parentC, instC) == Status::kOk);

    std::vector<NodeKind> expectedKinds = {NodeKind::Group, NodeKind::Path, NodeKind::Group,
                                           NodeKind::Image, NodeKind::Mask, NodeKind::Path};
    for (uint32_t i = 0; i < expectedKinds.size(); ++i) {
        NodeId actual = resolveTrackTarget(instC, i);
        CHECK(actual != kInvalidNode);
        REQUIRE(arena.alive(actual));
        CHECK(arena.kind(actual) == expectedKinds[i]);
    }
    // instB is untouched by the destroy/reinstantiate of instA/instC.
    CHECK(arena.alive(instB.root));
    CHECK(arena.common(resolveTrackTarget(instB, 1)).opacity == doctest::Approx(1.0f));
}

TEST_CASE("resolveTrackTarget: kSelfNode passthrough and out-of-range") {
    Fixture f = buildFixture();
    std::vector<uint8_t> bytes = f.writer.finish();
    Result<IrView> r = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(r.ok());
    const IrView& view = r.value();

    NodeArena arena;
    NodeId parent = arena.create(NodeKind::Group);
    SceneInstance inst;
    REQUIRE(instantiateScene(view, f.sceneId, arena, parent, inst) == Status::kOk);

    CHECK(resolveTrackTarget(inst, kSelfNode) == kSelfNode);
    CHECK(resolveTrackTarget(inst, static_cast<NodeId>(inst.localToArena.size())) == kInvalidNode);
    CHECK(resolveTrackTarget(inst, 999999u) == kInvalidNode);
}

TEST_CASE("destroySceneInstance: aliveCount restored, inst reset") {
    Fixture f = buildFixture();
    std::vector<uint8_t> bytes = f.writer.finish();
    Result<IrView> r = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(r.ok());
    const IrView& view = r.value();

    NodeArena arena;
    NodeId parent = arena.create(NodeKind::Group);
    uint32_t aliveBefore = arena.aliveCount();

    SceneInstance inst;
    REQUIRE(instantiateScene(view, f.sceneId, arena, parent, inst) == Status::kOk);
    CHECK(arena.aliveCount() == aliveBefore + 6);

    destroySceneInstance(arena, inst);
    CHECK(arena.aliveCount() == aliveBefore);
    CHECK(inst.root == kInvalidNode);
    CHECK(inst.localToArena.empty());
}

TEST_CASE("instantiateScene: 3D node fields copied to NodeCommon") {
    Fixture3D f = buildFixture3D();
    std::vector<uint8_t> bytes = f.writer.finish();
    Result<IrView> r = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(r.ok());
    const IrView& view = r.value();

    NodeArena arena;
    NodeId parent = arena.create(NodeKind::Group);
    SceneInstance inst;
    REQUIRE(instantiateScene(view, f.sceneId, arena, parent, inst) == Status::kOk);

    NodeId node3D = inst.localToArena[1];
    const NodeCommon& c = arena.common(node3D);
    CHECK(c.translateZ == doctest::Approx(f.node3DSrc.translateZ));
    CHECK(c.anchorZ == doctest::Approx(f.node3DSrc.anchorZ));
    CHECK(c.rotationX == doctest::Approx(f.node3DSrc.rotationX));
    CHECK(c.rotationY == doctest::Approx(f.node3DSrc.rotationY));
    CHECK(c.orientation.x == doctest::Approx(f.node3DSrc.orientation.x));
    CHECK(c.orientation.y == doctest::Approx(f.node3DSrc.orientation.y));
    CHECK(c.orientation.z == doctest::Approx(f.node3DSrc.orientation.z));
    CHECK(c.is3D == f.node3DSrc.is3D);
    CHECK(c.backfaceHidden == f.node3DSrc.backfaceHidden);
}

TEST_CASE("instantiateScene: Camera node kind, CameraData and translate") {
    Fixture3D f = buildFixture3D();
    std::vector<uint8_t> bytes = f.writer.finish();
    Result<IrView> r = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(r.ok());
    const IrView& view = r.value();

    NodeArena arena;
    NodeId parent = arena.create(NodeKind::Group);
    SceneInstance inst;
    REQUIRE(instantiateScene(view, f.sceneId, arena, parent, inst) == Status::kOk);

    NodeId camNode = inst.localToArena[2];
    CHECK(arena.kind(camNode) == NodeKind::Camera);

    const CameraData& cam = arena.camera(camNode);
    CHECK(cam.kind == f.cameraSrc.kind);
    CHECK(cam.useTarget == f.cameraSrc.useTarget);
    CHECK(cam.zoom == doctest::Approx(f.cameraSrc.zoom));
    CHECK(cam.target.x == doctest::Approx(f.cameraSrc.target.x));
    CHECK(cam.target.y == doctest::Approx(f.cameraSrc.target.y));
    CHECK(cam.target.z == doctest::Approx(f.cameraSrc.target.z));
    CHECK(cam.nearZ == doctest::Approx(f.cameraSrc.nearZ));
    CHECK(cam.farZ == doctest::Approx(f.cameraSrc.farZ));

    const NodeCommon& c = arena.common(camNode);
    CHECK(c.translate.x == doctest::Approx(100));
    CHECK(c.translate.y == doctest::Approx(75));
    CHECK(c.translateZ == doctest::Approx(500));
}

TEST_CASE("instantiateScene: scene effects copied into NodeArena::effects/effectStops") {
    Fixture3D f = buildFixture3D();
    std::vector<uint8_t> bytes = f.writer.finish();
    Result<IrView> r = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(r.ok());
    const IrView& view = r.value();

    NodeArena arena;
    NodeId parent = arena.create(NodeKind::Group);
    SceneInstance inst;
    REQUIRE(instantiateScene(view, f.sceneId, arena, parent, inst) == Status::kOk);

    NodeId node3D = inst.localToArena[1];
    const NodeCommon& c = arena.common(node3D);
    REQUIRE(c.effectCount == 2);
    REQUIRE(arena.effects().size() >= c.effectOffset + c.effectCount);

    const EffectInstance& fx0 = arena.effects()[c.effectOffset];
    CHECK(fx0.kind == EffectKind::DropShadow);
    CHECK(fx0.enabled == f.effect0Src.enabled);
    CHECK(fx0.params[0] == doctest::Approx(f.effect0Src.params[0]));
    CHECK(fx0.params[1] == doctest::Approx(f.effect0Src.params[1]));
    CHECK(fx0.colors[0].a == doctest::Approx(0.5f));
    CHECK(fx0.stopCount == 0);

    const EffectInstance& fx1 = arena.effects()[c.effectOffset + 1];
    CHECK(fx1.kind == EffectKind::GradientMask);
    REQUIRE(fx1.stopCount == 2);
    REQUIRE(arena.effectStops().size() >= fx1.stopOffset + fx1.stopCount);
    CHECK(arena.effectStops()[fx1.stopOffset].offset == doctest::Approx(0.0f));
    CHECK(arena.effectStops()[fx1.stopOffset + 1].offset == doctest::Approx(1.0f));
    CHECK(arena.effectStops()[fx1.stopOffset].color == f.effect1StopsSrc[0].color);
}

TEST_CASE("Animator integration: timelineFromIr + evalTrack + resolveTrackTarget + setProperty") {
    Fixture f = buildFixture();
    std::vector<uint8_t> bytes = f.writer.finish();
    Result<IrView> r = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(r.ok());
    const IrView& view = r.value();

    NodeArena arena;
    NodeId parent = arena.create(NodeKind::Group);
    SceneInstance inst;
    REQUIRE(instantiateScene(view, f.sceneId, arena, parent, inst) == Status::kOk);

    Timeline timeline;
    REQUIRE(timelineFromIr(view.timeline(f.timelineId), timeline) == Status::kOk);
    REQUIRE(timeline.tracks.size() == 1);
    const Track& track = timeline.tracks[0];

    float scratchFloats[4] = {};
    GradientStop scratchStops[4] = {};
    Value v = evalTrack(timeline, track, 0.5f, scratchFloats, scratchStops);
    REQUIRE(v.type == ValueType::Float);
    CHECK(v.f == doctest::Approx(0.5f));

    NodeId target = resolveTrackTarget(inst, track.target);
    REQUIRE(target != kInvalidNode);
    REQUIRE(target != kSelfNode);
    setProperty(arena, target, track.prop, v);

    CHECK(arena.common(target).opacity == doctest::Approx(0.5f));
}
