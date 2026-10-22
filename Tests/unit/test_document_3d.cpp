#include <cmath>
#include <limits>
#include <vector>

#include "camera.h"
#include "doctest.h"
#include "xt/animator.h"
#include "xt/compile/ir_writer.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/project3d.h"
#include "xt/property.h"
#include "xt/scene_instance.h"
#include "xt/style_engine.h"
#include "xt/timeline.h"
#include "xt/types.h"

/// Document 3D API tests: loading a <camera> element (built directly with IrWriter, since the
/// compiler does not support the `<camera>` tag yet -- T-47), layout/hit-test exclusion,
/// nodeToScreen, setNode3D and setNodeProperty. See Docs/design/19-3d-space.md §19.4,
/// Docs/tasks/T-43b.md "測試（test_document_3d.cpp）".

using namespace xt;
using namespace xt::compile;

namespace {

struct Built {
    std::vector<uint8_t> bytes;
    SceneId sceneId = kInvalidScene;
};

/// DOCT: xt -> body -> [camHost -> [camera#cam, div#after], perspHost -> [anim (2-node scene)]].
/// camHost/perspHost/after have no explicit size (block, width auto -> fills the viewport; height
/// auto -> 0, no content). The <camera> IrCameraData carries explicit zoom/target/near/far (no NaN)
/// so refreshCameraDefaults() leaves them untouched; `position` is left NaN (as if the `position`
/// attribute were omitted, 19.4/T-47a) so the eye position is always recomputed instead.
Built buildIr() {
    Built out;
    IrWriter w;

    StringId xtTag = w.intern("xt");
    StringId bodyTag = w.intern("body");
    StringId divTag = w.intern("div");
    StringId cameraTag = w.intern("camera");
    StringId animTag = w.intern("anim");
    StringId camId = w.intern("cam");
    StringId afterId = w.intern("after");

    SceneBuilder scene;
    // Large enough that the AE default camera's eye (z = width) stays well in front of the
    // translateZ = 250 layer used by the nodeToScreen test below.
    scene.width = 1000;
    scene.height = 1000;
    IrSceneNode root{};
    root.kind = static_cast<uint8_t>(NodeKind::Group);
    root.visible = 1;
    root.parent = 0xFFFFFFFFu;
    root.opacity = 1;
    root.scale = Vec2{1, 1};
    scene.nodes.push_back(root);  // scene-local 0
    IrSceneNode child{};
    child.kind = static_cast<uint8_t>(NodeKind::Group);
    child.visible = 1;
    child.parent = 0;
    child.opacity = 1;
    child.scale = Vec2{1, 1};
    scene.nodes.push_back(child);  // scene-local 1
    out.sceneId = w.addScene(scene);

    DocBuilder doc;

    IrDocElement xtEl{};
    xtEl.parent = 0xFFFFFFFFu;
    xtEl.tag = xtTag;
    xtEl.id = kInvalidString;
    xtEl.scene = kInvalidScene;
    xtEl.firstTextChild = 0xFFFFFFFFu;
    xtEl.cameraIndex = 0xFFFFFFFFu;
    doc.elements.push_back(xtEl);  // 0

    IrDocElement bodyEl{};
    bodyEl.parent = 0;
    bodyEl.tag = bodyTag;
    bodyEl.id = kInvalidString;
    bodyEl.scene = kInvalidScene;
    bodyEl.firstTextChild = 0xFFFFFFFFu;
    bodyEl.cameraIndex = 0xFFFFFFFFu;
    doc.elements.push_back(bodyEl);  // 1

    IrDocElement camHostEl{};
    camHostEl.parent = 1;
    camHostEl.tag = divTag;
    camHostEl.id = kInvalidString;
    camHostEl.scene = kInvalidScene;
    camHostEl.firstTextChild = 0xFFFFFFFFu;
    camHostEl.cameraIndex = 0xFFFFFFFFu;
    doc.elements.push_back(camHostEl);  // 2

    IrCameraData irCam{};
    irCam.kind = 0;
    irCam.useTarget = 1;
    irCam.zoom = 400;
    float nan = std::numeric_limits<float>::quiet_NaN();
    irCam.position = Vec3{nan, nan, nan};  // omitted (T-47a); refreshCameraDefaults() recomputes it.
    irCam.target = Vec3{200, 150, 0};
    irCam.nearZ = 1;
    irCam.farZ = 100000;
    uint32_t camIndex = IrWriter::addDocCamera(doc, irCam);

    IrDocElement cameraEl{};
    cameraEl.parent = 2;
    cameraEl.tag = cameraTag;
    cameraEl.id = camId;
    cameraEl.scene = kInvalidScene;
    cameraEl.firstTextChild = 0xFFFFFFFFu;
    cameraEl.cameraIndex = camIndex;
    doc.elements.push_back(cameraEl);  // 3

    IrDocElement afterEl{};
    afterEl.parent = 2;
    afterEl.tag = divTag;
    afterEl.id = afterId;
    afterEl.scene = kInvalidScene;
    afterEl.firstTextChild = 0xFFFFFFFFu;
    afterEl.cameraIndex = 0xFFFFFFFFu;
    doc.elements.push_back(afterEl);  // 4

    IrDocElement perspHostEl{};
    perspHostEl.parent = 1;
    perspHostEl.tag = divTag;
    perspHostEl.id = kInvalidString;
    perspHostEl.scene = kInvalidScene;
    perspHostEl.firstTextChild = 0xFFFFFFFFu;
    perspHostEl.cameraIndex = 0xFFFFFFFFu;
    doc.elements.push_back(perspHostEl);  // 5

    // No TLIN chunk exists for this scene (this test doesn't need animation playback), so
    // autoplay must be disabled -- otherwise document_load.cpp's step 4 would try to play a
    // nonexistent timeline (see Docs/tasks/T-18.md's SCEN i <-> TLIN i convention).
    StringId autoplayName = w.intern("autoplay");
    StringId falseValue = w.intern("false");
    IrAttr autoplayAttr{};
    autoplayAttr.name = autoplayName;
    autoplayAttr.value = falseValue;
    doc.attrPool.push_back(autoplayAttr);

    IrDocElement animEl{};
    animEl.parent = 5;
    animEl.tag = animTag;
    animEl.id = kInvalidString;
    animEl.attrOffset = 0;
    animEl.attrCount = 1;
    animEl.scene = out.sceneId;
    animEl.firstTextChild = 0xFFFFFFFFu;
    animEl.cameraIndex = 0xFFFFFFFFu;
    doc.elements.push_back(animEl);  // 6

    w.setDoc(doc);
    out.bytes = w.finish();
    return out;
}

DocumentHandle loadOk(Context& ctx, const Built& built) {
    Result<DocumentHandle> r = ctx.loadDocument(IrBlob{built.bytes.data(), built.bytes.size()}, false);
    REQUIRE(r.ok());
    return r.value();
}

}  // namespace

TEST_CASE("Document 3D: <camera> element loads a Camera child, cameraOf, sceneNode") {
    Built built = buildIr();
    Context ctx(ContextConfig{});
    Document* doc = ctx.document(loadOk(ctx, built));
    ctx.setViewport(Rect{0, 0, 400, 300}, 1.0f);
    ctx.update(0.0f);

    ElementRef cam = doc->byId("cam");
    REQUIRE(cam.valid());
    NodeId camNode = cam.sceneNode("");
    REQUIRE(camNode != kInvalidNode);
    CHECK(doc->arena().kind(camNode) == NodeKind::Camera);

    ElementRef camHost = doc->elementAt(2);
    REQUIRE(camHost.valid());
    CHECK(doc->cameraOf(camHost.node) == camNode);

    // Refreshed (NaN-omitted) eye position: (W/2, H/2, W) of camHost's border-box.
    const LayoutBox& hostBox = doc->arena().element(camHost.node).box;
    const NodeCommon& camCommon = doc->arena().common(camNode);
    CHECK(camCommon.translate.x == doctest::Approx(hostBox.width * 0.5f));
    CHECK(camCommon.translate.y == doctest::Approx(hostBox.height * 0.5f));
    CHECK(camCommon.translateZ == doctest::Approx(hostBox.width));
    // zoom/target were given explicitly in the IR (not NaN) -> left untouched.
    CHECK(doc->arena().camera(camNode).zoom == doctest::Approx(400.0f));
}

TEST_CASE("Document 3D: <camera> element does not take layout space or hit test") {
    Built built = buildIr();
    Context ctx(ContextConfig{});
    Document* doc = ctx.document(loadOk(ctx, built));
    ctx.setViewport(Rect{0, 0, 400, 300}, 1.0f);
    ctx.update(0.0f);

    ElementRef cam = doc->byId("cam");
    REQUIRE(cam.valid());
    CHECK(cam.layoutBox().width == 0.0f);
    CHECK(cam.layoutBox().height == 0.0f);

    // Its following sibling ("after") is positioned as though <camera> did not exist (both are
    // block, auto height => both collapse to 0-height at y=0; the meaningful assertion is that
    // "after" is not pushed down by <camera>'s own box).
    ElementRef after = doc->byId("after");
    REQUIRE(after.valid());
    CHECK(after.layoutBox().y == doctest::Approx(0.0f));

    // hitTest: a point inside camHost's box must not resolve to the (zero-box) camera element or
    // the Camera node -- with everything zero-sized here the only reachable target is camHost
    // itself (or nothing); either way it is not `cam`.
    NodeId hit = doc->hitTest(Vec2{5, 5});
    CHECK(hit != cam.node);
}

TEST_CASE("Document 3D: setNode3D rejects Elements, accepts scene nodes") {
    Built built = buildIr();
    Context ctx(ContextConfig{});
    Document* doc = ctx.document(loadOk(ctx, built));
    ctx.setViewport(Rect{0, 0, 400, 300}, 1.0f);
    ctx.update(0.0f);

    ElementRef body = doc->elementAt(1);
    REQUIRE(body.valid());
    CHECK(doc->setNode3D(body.node, true) == Status::kInvalidArgument);

    ElementRef anim = doc->elementAt(6);
    REQUIRE(anim.valid());
    NodeId sceneRoot = doc->arena().element(anim.node).sceneRoot;
    REQUIRE(sceneRoot != kInvalidNode);
    CHECK(doc->setNode3D(sceneRoot, true) == Status::kOk);
    CHECK(doc->arena().common(sceneRoot).is3D == 1);
}

// Docs/tasks/T-43c.md "T-43b 遺留修正" 2: a descendant's is3D change must invalidate the
// isSpaceRoot cache of every ancestor up to the nearest existing root (or the document root), not
// just the descendant itself -- otherwise a scene root Group's "isSceneRootGroup && subtreeHas3D"
// auto-detection (§19.3) would never pick up a deep 3D descendant without a manual markDirty.
TEST_CASE("Document 3D: deep child setNode3D(true) invalidates the scene root's isSpaceRoot cache") {
    Built built = buildIr();
    Context ctx(ContextConfig{});
    Document* doc = ctx.document(loadOk(ctx, built));
    ctx.setViewport(Rect{0, 0, 400, 300}, 1.0f);
    ctx.update(0.0f);

    ElementRef anim = doc->elementAt(6);
    REQUIRE(anim.valid());
    const SceneInstance* inst = doc->sceneInstance(anim);
    REQUIRE(inst != nullptr);
    REQUIRE(inst->localToArena.size() == 2);
    NodeId sceneRoot = inst->localToArena[0];
    NodeId child = inst->localToArena[1];
    CHECK(doc->arena().common(sceneRoot).isSpaceRoot == 0);  // No 3D content yet.

    REQUIRE(doc->setNode3D(child, true) == Status::kOk);
    // No manual markDirty here (contrast the nodeToScreen test below): setNode3D's own kDirtyTree
    // ancestor propagation must be enough.
    ctx.update(0.0f);

    CHECK(doc->arena().common(sceneRoot).isSpaceRoot == 1);
    CHECK(doc->arena().common(child).is3D == 1);
}

TEST_CASE("Document 3D: setNodeProperty validates type/applicability") {
    Built built = buildIr();
    Context ctx(ContextConfig{});
    Document* doc = ctx.document(loadOk(ctx, built));
    ctx.setViewport(Rect{0, 0, 400, 300}, 1.0f);
    ctx.update(0.0f);

    ElementRef anim = doc->elementAt(6);
    NodeId sceneRoot = doc->arena().element(anim.node).sceneRoot;

    // Wrong value type for a Float prop.
    CHECK(doc->setNodeProperty(sceneRoot, prop::TranslateZ, Value::ofVec2(Vec2{1, 2})) ==
          Status::kInvalidArgument);
    // Prop that doesn't apply to this node kind (Camera-only prop on a Group).
    CHECK(doc->setNodeProperty(sceneRoot, prop::CameraZoom, Value::ofFloat(100)) == Status::kInvalidArgument);
    // Valid write.
    CHECK(doc->setNodeProperty(sceneRoot, prop::TranslateZ, Value::ofFloat(42)) == Status::kOk);
    CHECK(doc->arena().common(sceneRoot).translateZ == doctest::Approx(42.0f));
}

// §19.3, Docs/tasks/T-43c.md: an Element's is3D is derived at runtime, not stored in the IR.
// ElementRef::setProperty(sticky=true) is Animator::setOverride, applied every evaluate() (after
// StyleEngine::resolve, per Document::update()'s order) -- while the override on a PropId 16-22
// prop is active, it forces is3D = 1 even though the cascade itself never set RotationY.
TEST_CASE("Document 3D: ElementRef::setProperty(RotationY, sticky) sets is3D; clearOverride restores it") {
    Built built = buildIr();
    Context ctx(ContextConfig{});
    Document* doc = ctx.document(loadOk(ctx, built));
    ctx.setViewport(Rect{0, 0, 400, 300}, 1.0f);
    ctx.update(0.0f);

    ElementRef after = doc->byId("after");
    REQUIRE(after.valid());
    CHECK(doc->arena().common(after.node).is3D == 0);

    after.setProperty(prop::RotationY, Value::ofFloat(45.0f), /*sticky=*/true);
    ctx.update(0.0f);
    CHECK(doc->arena().common(after.node).is3D == 1);

    after.clearOverride(prop::RotationY);
    ctx.update(0.0f);
    CHECK(doc->arena().common(after.node).is3D == 0);
}

// §19.3, Docs/tasks/T-43c.md: once a PropId 16-22 Track/transition/override finishes, is3D is
// restored by the next style resolve (not cleared directly). document_load.cpp wires
// Animator::onFinished to Document::handleAnimationFinished, which marks kDirtyStyle on the
// instance's element; driving Animator::evaluate()/a standalone StyleEngine directly (rather than
// Document::update(), which clears every dirty bit at the end of each frame -- see test_style_
// engine.cpp's Engine helper) lets this test observe that kDirtyStyle surviving into a later,
// separate resolve() call.
TEST_CASE("Document 3D: a finished PropId 16-22 animation restores is3D on the next resolve") {
    Built built = buildIr();
    Context ctx(ContextConfig{});
    Document* doc = ctx.document(loadOk(ctx, built));
    ctx.setViewport(Rect{0, 0, 400, 300}, 1.0f);
    ctx.update(0.0f);

    ElementRef after = doc->byId("after");
    REQUIRE(after.valid());
    CHECK(doc->arena().common(after.node).is3D == 0);

    Timeline tl;
    tl.duration = 1.0f;
    tl.keyframes.push_back(Keyframe{0.0f, Value::ofFloat(0.0f), Easing::linear(), {}, {}, 0});
    tl.keyframes.push_back(Keyframe{1.0f, Value::ofFloat(45.0f), Easing::linear(), {}, {}, 0});
    tl.tracks.push_back(Track{kSelfNode, prop::RotationY, ValueType::Float, 0, 2, 0});
    TimelineId tid = doc->animator().registerTimeline(&tl);
    PlayParams pp;
    pp.timeline = tid;
    pp.selfNode = after.node;
    pp.fill = FillMode::None;  // Finishing removes the instance -- no lingering Forwards write.
    doc->play(pp);

    StyleEngine engine;
    engine.init(doc->ir(), doc->animator());

    doc->animator().evaluate(0.5f, doc->arena());  // Mid-playback: the Track is active.
    CHECK(doc->arena().common(after.node).is3D == 1);

    doc->animator().evaluate(1.5f, doc->arena());  // Past duration: finishes, fires onFinished
                                                   // synchronously -> handleAnimationFinished
                                                   // -> markDirty(kDirtyStyle).
    CHECK(engine.resolve(doc->arena(), 1.5f) > 0);
    CHECK(doc->arena().common(after.node).is3D == 0);
}

TEST_CASE("Document 3D: nodeToScreen projects a 3D layer consistently with its own root chain") {
    Built built = buildIr();
    Context ctx(ContextConfig{});
    Document* doc = ctx.document(loadOk(ctx, built));
    ctx.setViewport(Rect{0, 0, 400, 300}, 1.0f);
    ctx.update(0.0f);

    ElementRef anim = doc->elementAt(6);
    REQUIRE(anim.valid());
    const SceneInstance* inst = doc->sceneInstance(anim);
    REQUIRE(inst != nullptr);
    REQUIRE(inst->localToArena.size() == 2);
    NodeId sceneRoot = inst->localToArena[0];
    NodeId layer = inst->localToArena[1];

    REQUIRE(doc->setNode3D(layer, true) == Status::kOk);
    REQUIRE(doc->setNodeProperty(layer, prop::TranslateZ, Value::ofFloat(250)) == Status::kOk);

    ElementRef perspHost = doc->elementAt(5);
    REQUIRE(perspHost.valid());
    doc->arena().element(perspHost.node).perspectivePx = 500.0f;
    doc->arena().element(perspHost.node).perspectiveOriginPx = Vec2{200, 150};
    doc->arena().markDirty(perspHost.node, static_cast<uint8_t>(kDirtyTree | kDirtyTransform));
    // isSpaceRoot is a cache recomputed only when kDirtyTree is set on that exact node (§19.5), so
    // sceneRoot's own cache (stale from the first update(), before `layer` had is3D) also needs an
    // explicit kDirtyTree to pick up its new 3D descendant and become a nested root.
    doc->arena().markDirty(sceneRoot, static_cast<uint8_t>(kDirtyTree | kDirtyTransform));
    ctx.update(0.0f);

    CHECK(doc->arena().common(perspHost.node).isSpaceRoot == 1);
    // §19.4's "scene root Group with a 3D descendant" auto-root rule applies to the whole
    // subtree, not just direct children, so `sceneRoot` (an ancestor of `layer`) becomes a
    // *nested* root here too (§19.5's nested-root case) -- both are exercised by this test.
    CHECK(doc->arena().common(sceneRoot).isSpaceRoot == 1);
    CHECK(doc->arena().common(layer).isSpaceRoot == 0);
    CHECK(doc->arena().common(layer).is3D == 1);

    Vec2 actual = doc->nodeToScreen(layer, Vec2{0, 0});

    // Independently assembled expected value, following §19.7's mapping-stack algorithm by hand
    // for the same two nested roots nodeToScreen must discover: sceneRoot's own AE default camera
    // (its box is the scene's 1000x1000 design size, set in buildIr() above -- not the anim
    // element's actual laid-out box) nested inside perspHost's CSS-perspective root.
    CameraSetup innerSetup = defaultSceneViewProj(1000.0f, 1000.0f);
    CameraSetup outerSetup = cssPerspectiveViewProj(500.0f, Vec2{200, 150});
    project3d::Mapping inner;
    inner.hasProjection = true;
    inner.viewProj = innerSetup.viewProj;
    inner.rootXf = doc->arena().common(sceneRoot).world;
    inner.planeWorld3 = doc->arena().common(layer).world3;  // layer is3D, not a root: its own world3.
    project3d::Mapping outer;
    outer.hasProjection = true;
    outer.viewProj = outerSetup.viewProj;
    outer.rootXf = doc->arena().common(perspHost.node).world;
    outer.planeWorld3 = mat4Identity();                   // sceneRoot is itself a root: identity plane.
    std::vector<project3d::Mapping> stack{outer, inner};  // outermost first.
    bool ok = false;
    Vec2 expected = project3d::projectPoint(stack, doc->arena().common(layer).world, Vec2{0, 0}, ok);
    REQUIRE(ok);
    CHECK(actual.x == doctest::Approx(expected.x));
    CHECK(actual.y == doctest::Approx(expected.y));
}

TEST_CASE("Document 3D: nodeToScreen3 with z = 0 matches nodeToScreen; z-offset differs under rotateY") {
    Built built = buildIr();
    Context ctx(ContextConfig{});
    Document* doc = ctx.document(loadOk(ctx, built));
    ctx.setViewport(Rect{0, 0, 400, 300}, 1.0f);
    ctx.update(0.0f);

    ElementRef anim = doc->elementAt(6);
    REQUIRE(anim.valid());
    const SceneInstance* inst = doc->sceneInstance(anim);
    REQUIRE(inst != nullptr);
    NodeId layer = inst->localToArena[1];

    // A plain 2D node outside any 3D space root (the `body` element -- `camHost`, not `body`, is
    // the root that owns the `<camera>`), T-64's "2D 節點 z 忽略" case: nodeToScreen3(node,
    // (x, y, anything)) must equal nodeToScreen(node, (x, y)).
    ElementRef body = doc->elementAt(1);
    REQUIRE(body.valid());
    Vec2 expected2D = doc->nodeToScreen(body.node, Vec2{3, 5});
    Vec2 actual2D = doc->nodeToScreen3(body.node, Vec3{3, 5, 999});
    CHECK(actual2D.x == doctest::Approx(expected2D.x));
    CHECK(actual2D.y == doctest::Approx(expected2D.y));

    REQUIRE(doc->setNode3D(layer, true) == Status::kOk);
    ctx.update(0.0f);
    REQUIRE(doc->arena().common(layer).is3D == 1);

    // z = 0 on a 3D layer: nodeToScreen3(node, (x, y, 0)) == nodeToScreen(node, (x, y)).
    Vec2 base = doc->nodeToScreen(layer, Vec2{0, 0});
    Vec2 baseZ0 = doc->nodeToScreen3(layer, Vec3{0, 0, 0});
    CHECK(baseZ0.x == doctest::Approx(base.x));
    CHECK(baseZ0.y == doctest::Approx(base.y));

    // rotateY(90deg): the anchor's projection and the anchor + local-z-offset(40) projection must
    // differ (a faithful 3D projection, not the T-59 in-plane approximation).
    REQUIRE(doc->setNodeProperty(layer, prop::RotationY, Value::ofFloat(90)) == Status::kOk);
    ctx.update(0.0f);
    Vec2 anchorScreen = doc->nodeToScreen3(layer, Vec3{0, 0, 0});
    Vec2 zOffsetScreen = doc->nodeToScreen3(layer, Vec3{0, 0, 40});
    bool differs = std::abs(zOffsetScreen.x - anchorScreen.x) > 0.5f ||
                   std::abs(zOffsetScreen.y - anchorScreen.y) > 0.5f;
    CHECK(differs);
}
