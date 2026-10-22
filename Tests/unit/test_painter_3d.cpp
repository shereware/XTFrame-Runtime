#include <cmath>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/command_list.h"
#include "xt/compile/ir_writer.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/painter.h"
#include "xt/scene_instance.h"
#include "xt/types.h"

/// Painter 3D tests (Docs/tasks/T-44b.md "測試"): builds a scene with IrWriter under an `anim`
/// Element (via DocBuilder, like test_document_3d.cpp -- so ElementData::intrinsic, which
/// Painter::emitRoot() reads for the scene root's reference box, is populated by a real layout
/// pass), loads it through Document, paints, and checks the resulting CommandList's command-kind
/// sequence, fill order and Transform3DCmd payloads against Docs/design/19-3d-space.md §19.5-19.6.
///
/// Every scene here uses the same camera setup (eye = (100, 100, 200), target = (100, 100, 0),
/// no rotation), either as an explicit `<camera>`-equivalent IrSceneNode (test 1) or the AE
/// default camera a scene root with no camera layer gets automatically (tests 2-5; scene width =
/// height = 200, so its eye/target are numerically identical to test 1's explicit camera). With
/// eye/target sharing x/y, the camera frame reduces to a pure translation (its s/d/b basis vectors
/// come out exactly axis-aligned -- see camera.cpp's cameraFrameView), so every node's own-anchor
/// depth (§19.6) is just its translateZ minus a constant (eye.z): translateZ order alone decides
/// draw order, letting the fixtures below use plain integer Z values without hand-deriving matrix
/// values.

using namespace xt;
using namespace xt::compile;

namespace {

struct Built {
    std::vector<uint8_t> bytes;
};

/// Appends one drawable path (a small solid-filled triangle) to `scene`'s pools/paths and returns
/// its `paths[]` index (the IrSceneNode::kindIndex a Path node referencing it needs).
uint32_t addPath(SceneBuilder& scene, Color fillColor) {
    IrPathData p{};
    p.verbOffset = static_cast<uint32_t>(scene.verbPool.size());
    scene.verbPool.push_back(static_cast<uint8_t>(PathVerb::Move));
    scene.verbPool.push_back(static_cast<uint8_t>(PathVerb::Line));
    scene.verbPool.push_back(static_cast<uint8_t>(PathVerb::Line));
    scene.verbPool.push_back(static_cast<uint8_t>(PathVerb::Close));
    p.verbCount = 4;
    p.pointOffset = static_cast<uint32_t>(scene.floatPool.size());
    float pts[] = {0, 0, 10, 0, 10, 10};
    scene.floatPool.insert(scene.floatPool.end(), std::begin(pts), std::end(pts));
    p.pointCount = 6;
    p.fillRule = static_cast<uint8_t>(FillRule::NonZero);
    p.fillKind = static_cast<uint8_t>(PaintKind::Solid);
    p.fillColor = fillColor;
    p.strokeKind = static_cast<uint8_t>(PaintKind::None);
    p.trimStart = 0.0f;
    p.trimEnd = 1.0f;  // IrPathData{} zero-inits trimEnd, which Painter's emitPath treats as an
                       // empty (start==end) trim interval -- must be set explicitly to draw the
                       // whole path.
    scene.paths.push_back(p);
    return static_cast<uint32_t>(scene.paths.size() - 1);
}

/// Appends a Group/Path IrSceneNode with §19.3's 3D fields set, returning its scene-local index.
uint32_t addNode3D(SceneBuilder& scene, NodeKind kind, uint32_t parent, uint32_t kindIndex, uint8_t is3D,
                   float translateZ, float rotationY = 0.0f, uint8_t backfaceHidden = 0) {
    IrSceneNode n{};
    n.kind = static_cast<uint8_t>(kind);
    n.visible = 1;
    n.parent = parent;
    n.kindIndex = kindIndex;
    n.opacity = 1;
    n.scale = Vec2{1, 1};
    n.name = kInvalidString;
    n.is3D = is3D;
    n.translateZ = translateZ;
    n.rotationY = rotationY;
    n.backfaceHidden = backfaceHidden;
    scene.nodes.push_back(n);
    return static_cast<uint32_t>(scene.nodes.size() - 1);
}

/// Appends the scene root Group (scene-local index 0, no parent) that every fixture below starts
/// from.
void addRootGroup(SceneBuilder& scene) {
    IrSceneNode root{};
    root.kind = static_cast<uint8_t>(NodeKind::Group);
    root.visible = 1;
    root.parent = 0xFFFFFFFFu;
    root.opacity = 1;
    root.scale = Vec2{1, 1};
    root.name = kInvalidString;
    scene.nodes.push_back(root);
}

/// Appends an explicit camera child of the scene root (scene-local index 1): eye = (100, 100,
/// 200), target = (100, 100, 0), matching the AE default camera tests 2-5 get automatically for a
/// 200x200 scene (see the file comment).
void addCameraChild(SceneBuilder& scene) {
    IrCameraData cam{};
    cam.kind = 0;  // Perspective.
    cam.useTarget = 1;
    cam.zoom = 200;
    cam.target = Vec3{100, 100, 0};
    cam.nearZ = 1;
    cam.farZ = 100000;
    uint32_t camIdx = IrWriter::addSceneCamera(scene, cam);

    IrSceneNode n{};
    n.kind = static_cast<uint8_t>(NodeKind::Camera);
    n.visible = 1;
    n.parent = 0;
    n.kindIndex = camIdx;
    n.opacity = 1;
    n.translate = Vec2{100, 100};
    n.translateZ = 200;
    n.scale = Vec2{1, 1};
    n.name = kInvalidString;
    scene.nodes.push_back(n);
}

/// Wraps `scene` (width/height forced to 200x200) as `<xt><body><anim>` (autoplay disabled, no
/// TLIN chunk needed), matching test_document_3d.cpp's buildIr(). Returns the finished IR bytes.
Built buildAnimDoc(IrWriter& w, SceneBuilder scene) {
    scene.width = 200;
    scene.height = 200;
    SceneId sceneId = w.addScene(scene);

    StringId xtTag = w.intern("xt");
    StringId bodyTag = w.intern("body");
    StringId animTag = w.intern("anim");

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

    // No TLIN chunk exists for this scene -- disable autoplay (matches test_document_3d.cpp).
    StringId autoplayName = w.intern("autoplay");
    StringId falseValue = w.intern("false");
    IrAttr autoplayAttr{};
    autoplayAttr.name = autoplayName;
    autoplayAttr.value = falseValue;
    doc.attrPool.push_back(autoplayAttr);

    IrDocElement animEl{};
    animEl.parent = 1;
    animEl.tag = animTag;
    animEl.attrOffset = 0;
    animEl.attrCount = 1;
    animEl.scene = sceneId;
    animEl.firstTextChild = 0xFFFFFFFFu;
    animEl.cameraIndex = 0xFFFFFFFFu;
    doc.elements.push_back(animEl);  // 2

    w.setDoc(doc);
    Built out;
    out.bytes = w.finish();
    return out;
}

DocumentHandle loadOk(Context& ctx, const Built& built) {
    Result<DocumentHandle> r = ctx.loadDocument(IrBlob{built.bytes.data(), built.bytes.size()}, false);
    REQUIRE(r.ok());
    return r.value();
}

/// Loads `built` and paints it, returning both the CommandList and its dump string.
CommandList loadAndPaint(const Built& built, std::string& dumpOut) {
    Context ctx(ContextConfig{});
    Document* doc = ctx.document(loadOk(ctx, built));
    ctx.setViewport(Rect{0, 0, 400, 300}, 1.0f);
    ctx.update(0.0f);

    CommandList cmds;
    doc->paint(cmds);
    dumpCommandList(cmds, dumpOut);
    return cmds;
}

bool mat4Approx(const Mat4& a, const Mat4& b) {
    for (int i = 0; i < 16; ++i) {
        if (std::fabs(a.m[i] - b.m[i]) > 1e-3f) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("Painter 3D 1: scene root with Camera + two 3D Paths, depth-sorted, doc order reversed") {
    IrWriter w;
    SceneBuilder scene;
    addRootGroup(scene);
    addCameraChild(scene);  // local 1 -- excluded from sectioning/painting.

    uint32_t pNear = addPath(scene, Color{0, 1, 0, 1});     // green
    addNode3D(scene, NodeKind::Path, 0, pNear, 1, 50.0f);   // local 2: doc order first, z=+50
    uint32_t pFar = addPath(scene, Color{1, 0, 0, 1});      // red
    addNode3D(scene, NodeKind::Path, 0, pFar, 1, -100.0f);  // local 3: doc order second, z=-100

    Built built = buildAnimDoc(w, scene);
    std::string dump;
    CommandList cmds = loadAndPaint(built, dump);

    // §19.4: the root's chosen camera child never reaches sectioning/painting -- "no root-plane 2D
    // content" (no empty PushTransform3D{I} pair for it).
    REQUIRE(cmds.commands.size() == 8);
    CHECK(cmds.commands[0].kind == CmdKind::PushProjection);
    CHECK(cmds.commands[1].kind == CmdKind::PushTransform3D);
    CHECK(cmds.commands[2].kind == CmdKind::FillPath);
    CHECK(cmds.commands[3].kind == CmdKind::PopTransform3D);
    CHECK(cmds.commands[4].kind == CmdKind::PushTransform3D);
    CHECK(cmds.commands[5].kind == CmdKind::FillPath);
    CHECK(cmds.commands[6].kind == CmdKind::PopTransform3D);
    CHECK(cmds.commands[7].kind == CmdKind::PopProjection);

    // Farthest (z=-100, red) drawn first; nearest (z=+50, green) second (§19.6: depth ascending).
    REQUIRE(cmds.fills.size() == 2);
    CHECK(cmds.fills[0].paint.color.r == doctest::Approx(1.0f));
    CHECK(cmds.fills[1].paint.color.g == doctest::Approx(1.0f));

    // Case 1's full dump (captured for Docs/tasks/T-44b.md's report) is reproduced verbatim there;
    // here it's just confirmed non-empty (the exact command sequence/fills are already checked
    // above via cmds.commands and cmds.fills directly).
    CHECK_FALSE(dump.empty());
}

TEST_CASE("Painter 3D 2: 3D/2D/3D children form three sections; the 2D one wraps in PushTransform3D{I}") {
    IrWriter w;
    SceneBuilder scene;
    addRootGroup(scene);

    uint32_t p0 = addPath(scene, Color{1, 0, 0, 1});   // red
    addNode3D(scene, NodeKind::Path, 0, p0, 1, 0.0f);  // local 1: 3D

    uint32_t p1 = addPath(scene, Color{0, 1, 0, 1});   // green
    addNode3D(scene, NodeKind::Path, 0, p1, 0, 0.0f);  // local 2: plain 2D

    uint32_t p2 = addPath(scene, Color{0, 0, 1, 1});   // blue
    addNode3D(scene, NodeKind::Path, 0, p2, 1, 0.0f);  // local 3: 3D

    Built built = buildAnimDoc(w, scene);
    std::string dump;
    CommandList cmds = loadAndPaint(built, dump);

    REQUIRE(cmds.commands.size() == 11);
    CHECK(cmds.commands[0].kind == CmdKind::PushProjection);
    CHECK(cmds.commands[1].kind == CmdKind::PushTransform3D);  // section 1 (3D: p0)
    CHECK(cmds.commands[2].kind == CmdKind::FillPath);
    CHECK(cmds.commands[3].kind == CmdKind::PopTransform3D);
    CHECK(cmds.commands[4].kind == CmdKind::PushTransform3D);  // section 2 (plain 2D: p1)
    CHECK(cmds.commands[5].kind == CmdKind::FillPath);
    CHECK(cmds.commands[6].kind == CmdKind::PopTransform3D);
    CHECK(cmds.commands[7].kind == CmdKind::PushTransform3D);  // section 3 (3D: p2)
    CHECK(cmds.commands[8].kind == CmdKind::FillPath);
    CHECK(cmds.commands[9].kind == CmdKind::PopTransform3D);
    CHECK(cmds.commands[10].kind == CmdKind::PopProjection);

    REQUIRE(cmds.fills.size() == 3);
    CHECK(cmds.fills[0].paint.color.r == doctest::Approx(1.0f));  // p0
    CHECK(cmds.fills[1].paint.color.g == doctest::Approx(1.0f));  // p1
    CHECK(cmds.fills[2].paint.color.b == doctest::Approx(1.0f));  // p2

    // The middle (plain 2D) quantum's own plane transform must be identity.
    const Mat4& midXf = cmds.transforms3[cmds.commands[4].index].world3;
    CHECK(mat4Approx(midXf, mat4Identity()));
}

TEST_CASE("Painter 3D 3: a 3D layer's 3D descendant is a separate layer, world3 = parent.world3 * local3") {
    IrWriter w;
    SceneBuilder scene;
    addRootGroup(scene);
    addNode3D(scene, NodeKind::Group, 0, 0, 1, 30.0f);  // local 1: 3D layer, z=30
    uint32_t p = addPath(scene, Color{1, 1, 0, 1});     // yellow
    addNode3D(scene, NodeKind::Path, 1, p, 1, 20.0f);   // local 2: 3D grandchild, z=20 (local)

    Built built = buildAnimDoc(w, scene);
    std::string dump;
    CommandList cmds = loadAndPaint(built, dump);

    // Both layers appear in the same (single) sorted list, each with its own PushTransform3D pair:
    // PushProjection, PushT3D(local1, empty content), PopT3D, PushT3D(local2), FillPath, PopT3D,
    // PopProjection.
    REQUIRE(cmds.commands.size() == 7);
    CHECK(cmds.commands[1].kind == CmdKind::PushTransform3D);
    CHECK(cmds.commands[2].kind == CmdKind::PopTransform3D);
    CHECK(cmds.commands[3].kind == CmdKind::PushTransform3D);
    CHECK(cmds.commands[4].kind == CmdKind::FillPath);
    CHECK(cmds.commands[5].kind == CmdKind::PopTransform3D);
    CHECK(cmds.commands[6].kind == CmdKind::PopProjection);

    Context ctx(ContextConfig{});
    Document* doc = ctx.document(loadOk(ctx, built));
    ctx.setViewport(Rect{0, 0, 400, 300}, 1.0f);
    ctx.update(0.0f);
    ElementRef anim = doc->elementAt(2);
    REQUIRE(anim.valid());
    const SceneInstance* inst = doc->sceneInstance(anim);
    REQUIRE(inst != nullptr);
    REQUIRE(inst->localToArena.size() == 3);
    NodeId groupId = inst->localToArena[1];
    NodeId pathId = inst->localToArena[2];

    const Mat4& emittedGroup = cmds.transforms3[cmds.commands[1].index].world3;
    const Mat4& emittedPath = cmds.transforms3[cmds.commands[3].index].world3;
    CHECK(mat4Approx(emittedGroup, doc->arena().common(groupId).world3));
    CHECK(mat4Approx(emittedPath, doc->arena().common(pathId).world3));

    // §19.5: world3(grandchild) = world3(parent 3D layer) * local3(grandchild); local3 here is a
    // pure Z translate (see the file comment), so this checks the pass-through, not the formula's
    // own implementation (already world_transform.cpp's job).
    Mat4 expectedPath = mat4Mul(doc->arena().common(groupId).world3, mat4Translate(Vec3{0, 0, 20}));
    CHECK(mat4Approx(emittedPath, expectedPath));
}

TEST_CASE("Painter 3D 4: backfaceHidden skips a rotationY=180 layer, draws a rotationY=0 one") {
    // rotationY = 180 -> back-facing -> not drawn (not even an empty PushTransform3D pair).
    {
        IrWriter w;
        SceneBuilder scene;
        addRootGroup(scene);
        uint32_t p = addPath(scene, Color{1, 0, 0, 1});
        addNode3D(scene, NodeKind::Path, 0, p, 1, 0.0f, 180.0f, /*backfaceHidden=*/1);

        Built built = buildAnimDoc(w, scene);
        std::string dump;
        CommandList cmds = loadAndPaint(built, dump);

        REQUIRE(cmds.commands.size() == 2);
        CHECK(cmds.commands[0].kind == CmdKind::PushProjection);
        CHECK(cmds.commands[1].kind == CmdKind::PopProjection);
        CHECK(cmds.fills.empty());
    }
    // rotationY = 0 -> front-facing -> drawn normally.
    {
        IrWriter w;
        SceneBuilder scene;
        addRootGroup(scene);
        uint32_t p = addPath(scene, Color{1, 0, 0, 1});
        addNode3D(scene, NodeKind::Path, 0, p, 1, 0.0f, 0.0f, /*backfaceHidden=*/1);

        Built built = buildAnimDoc(w, scene);
        std::string dump;
        CommandList cmds = loadAndPaint(built, dump);

        REQUIRE(cmds.commands.size() == 5);
        CHECK(cmds.commands[0].kind == CmdKind::PushProjection);
        CHECK(cmds.commands[1].kind == CmdKind::PushTransform3D);
        CHECK(cmds.commands[2].kind == CmdKind::FillPath);
        CHECK(cmds.commands[3].kind == CmdKind::PopTransform3D);
        CHECK(cmds.commands[4].kind == CmdKind::PopProjection);
        REQUIRE(cmds.fills.size() == 1);
    }
}

TEST_CASE("Painter 3D 5: a 2D child splits sorting into segments; depth sort never crosses a segment") {
    // Same 3D/2D/3D shape as case 2, but z is chosen so a *global* sort would reorder the two
    // segments' layers relative to each other (z=-100 is farther than z=+50) -- the segment
    // boundary must still keep them in document order (section 1's layer before section 3's).
    IrWriter w;
    SceneBuilder scene;
    addRootGroup(scene);

    uint32_t p0 = addPath(scene, Color{1, 0, 0, 1});    // red
    addNode3D(scene, NodeKind::Path, 0, p0, 1, 50.0f);  // local 1: 3D, z=+50 (nearer)

    uint32_t p1 = addPath(scene, Color{0, 1, 0, 1});   // green
    addNode3D(scene, NodeKind::Path, 0, p1, 0, 0.0f);  // local 2: plain 2D

    uint32_t p2 = addPath(scene, Color{0, 0, 1, 1});      // blue
    addNode3D(scene, NodeKind::Path, 0, p2, 1, -100.0f);  // local 3: 3D, z=-100 (farther)

    Built built = buildAnimDoc(w, scene);
    std::string dump;
    CommandList cmds = loadAndPaint(built, dump);

    REQUIRE(cmds.fills.size() == 3);
    CHECK(cmds.fills[0].paint.color.r == doctest::Approx(1.0f));  // p0 (section 1, z=+50)
    CHECK(cmds.fills[1].paint.color.g == doctest::Approx(1.0f));  // p1 (section 2, plain 2D)
    CHECK(cmds.fills[2].paint.color.b == doctest::Approx(1.0f));  // p2 (section 3, z=-100)
}

// Docs/tasks/T-48.md "T-44b 遺留修正" 2: a 2D Group (opacity 0.5) sitting between the sorted
// segment's direct child and a 3D layer must multiply its opacity into that layer's paint alpha
// (layer_collect::LayerEntry::inheritedAlpha), same as an ordinary 2D ancestor's opacity would.
TEST_CASE(
    "Painter 3D 6 (T-48 T-44b fix 2): 2D group opacity 0.5 multiplies into its 3D layer child's alpha") {
    IrWriter w;
    SceneBuilder scene;
    addRootGroup(scene);

    IrSceneNode group{};
    group.kind = static_cast<uint8_t>(NodeKind::Group);
    group.visible = 1;
    group.parent = 0;
    group.opacity = 0.5f;
    group.scale = Vec2{1, 1};
    group.name = kInvalidString;
    scene.nodes.push_back(group);  // local 1: plain 2D group, opacity 0.5.

    uint32_t p = addPath(scene, Color{1, 0, 0, 1});
    addNode3D(scene, NodeKind::Path, 1, p, 1, 0.0f);  // local 2: 3D layer, child of the group.

    Built built = buildAnimDoc(w, scene);
    std::string dump;
    CommandList cmds = loadAndPaint(built, dump);

    REQUIRE(cmds.fills.size() == 1);
    CHECK(cmds.fills[0].paint.color.a == doctest::Approx(0.5f));
}

// Docs/tasks/T-48.md "T-44b 遺留修正" 1: the 3D root's own opacity < 1 must wrap
// PushProjection..PopProjection in PushLayer/PopLayer (like enterNode()'s needsLayer branch)
// instead of folding into every descendant's paint alpha.
TEST_CASE("Painter 3D 7 (T-48 T-44b fix 1): root opacity < 1 wraps the projection in PushLayer/PopLayer") {
    IrWriter w;
    SceneBuilder scene;
    addRootGroup(scene);
    scene.nodes[0].opacity = 0.4f;  // The scene root itself (also the 3D space root here).

    uint32_t p = addPath(scene, Color{1, 0, 0, 1});
    addNode3D(scene, NodeKind::Path, 0, p, 1, 0.0f);  // local 1: 3D layer, opacity 1 (default).

    Built built = buildAnimDoc(w, scene);
    std::string dump;
    CommandList cmds = loadAndPaint(built, dump);

    REQUIRE(cmds.commands.size() == 7);
    CHECK(cmds.commands[0].kind == CmdKind::PushLayer);
    CHECK(cmds.commands[1].kind == CmdKind::PushProjection);
    CHECK(cmds.commands[2].kind == CmdKind::PushTransform3D);
    CHECK(cmds.commands[3].kind == CmdKind::FillPath);
    CHECK(cmds.commands[4].kind == CmdKind::PopTransform3D);
    CHECK(cmds.commands[5].kind == CmdKind::PopProjection);
    CHECK(cmds.commands[6].kind == CmdKind::PopLayer);
    REQUIRE(cmds.layers.size() == 1);
    CHECK(cmds.layers[0].opacity == doctest::Approx(0.4f));

    // The layer's own paint alpha is 1.0 (not multiplied by the root's opacity -- that 0.4 is
    // applied once via PushLayer/PopLayer's compositing instead).
    REQUIRE(cmds.fills.size() == 1);
    CHECK(cmds.fills[0].paint.color.a == doctest::Approx(1.0f));
}
