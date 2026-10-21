#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/compile/ir_writer.h"
#include "xt/ir.h"

// Declared locally: crc32() lives in xt_core but only in a private header (core/src/crc32.h);
// xt_tests links xt_core, so the symbol resolves at link time. See compile/src/ir_writer.cpp for
// the same technique.
namespace xt {
uint32_t crc32(const uint8_t* data, size_t size);
}  // namespace xt

using namespace xt;
using namespace xt::compile;

namespace {

/// Everything the malformed-IR test needs to locate/mutate specific fields inside the fixture's
/// serialized bytes, alongside the writer itself. All counts/ids mirror docs/tasks/T-03.md's
/// round-trip fixture description ("1 scene, 1 timeline, 3 doc elements, ...").
struct Fixture {
    IrWriter writer;
    std::vector<std::string> internedInOrder;  // mirrors IrWriter's dedup order exactly
    StringId sceneNameId = kInvalidString;
    StringId timelineNameId = kInvalidString;
    StringId rootNodeNameId = kInvalidString;
    StringId helloTextId = kInvalidString;
    SceneId sceneId = kInvalidScene;
    TimelineId timelineId = 0;
    AssetId embeddedAssetId = 0;
    AssetId externalAssetId = 0;
};

StringId internTracked(Fixture& f, std::string_view s) {
    StringId id = f.writer.intern(s);
    if (id == f.internedInOrder.size()) {
        f.internedInOrder.emplace_back(s);
    }
    return id;
}

/// Builds one IR covering every chunk/struct field listed in docs/tasks/T-03.md's round-trip test
/// description. Field offset comments below assume this exact layout (used by
/// tests/unit/test_ir_malformed.cpp to locate bytes to corrupt), so keep counts/order in sync if
/// this function changes.
Fixture buildFixture() {
    Fixture f;
    IrWriter& w = f.writer;

    StringId sceneName = internTracked(f, "scene1");
    StringId timelineName = internTracked(f, "timeline1");
    StringId logoName = internTracked(f, "logo.png");
    StringId fontName = internTracked(f, "font.ttf");
    StringId boxClass = internTracked(f, "box");
    StringId divTag = internTracked(f, "div");
    StringId spanTag = internTracked(f, "span");
    StringId animTag = internTracked(f, "anim");
    StringId rootName = internTracked(f, "Root");
    StringId imgName = internTracked(f, "Img");
    StringId helloText = internTracked(f, "Hello");
    StringId markerName = internTracked(f, "marker1");
    StringId dataXAttr = internTracked(f, "data-x");
    StringId attrValue42 = internTracked(f, "42");
    StringId arialFamily = internTracked(f, "Arial");
    StringId sourceFile = internTracked(f, "src/test.xt");
    StringId compilerVersion = internTracked(f, "xtc-0.1.0");

    f.sceneNameId = sceneName;
    f.timelineNameId = timelineName;
    f.rootNodeNameId = rootName;
    f.helloTextId = helloText;

    // ASST: 1 embedded + 1 external.
    AssetBuilder embedded;
    embedded.name = logoName;
    embedded.kind = 0;  // Image
    embedded.embedded = {0x89, 0x50, 0x4E, 0x47};
    f.embeddedAssetId = w.addAsset(embedded);

    AssetBuilder external;
    external.name = fontName;
    external.kind = 1;  // Font
    f.externalAssetId = w.addAsset(external);

    // SCEN: root Group + Path (Cubic+Close, 2-stop linear gradient fill, solid stroke, 2 dash
    // values) + Image + Mask.
    SceneBuilder scene;
    scene.name = sceneName;
    scene.width = 100;
    scene.height = 200;

    IrSceneNode root{};
    root.kind = static_cast<uint8_t>(NodeKind::Group);
    root.visible = 1;
    root.parent = 0xFFFFFFFFu;
    root.kindIndex = 0;
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
    pathNode.scale = Vec2{1, 1};
    pathNode.name = kInvalidString;
    scene.nodes.push_back(pathNode);

    IrSceneNode imageNode{};
    imageNode.kind = static_cast<uint8_t>(NodeKind::Image);
    imageNode.visible = 1;
    imageNode.parent = 0;
    imageNode.kindIndex = 0;
    imageNode.opacity = 1;
    imageNode.scale = Vec2{1, 1};
    imageNode.name = imgName;
    scene.nodes.push_back(imageNode);

    IrSceneNode maskNode{};
    maskNode.kind = static_cast<uint8_t>(NodeKind::Mask);
    maskNode.visible = 1;
    maskNode.maskMode = static_cast<uint8_t>(MaskMode::Alpha);
    maskNode.parent = 0;
    maskNode.kindIndex = 0;
    maskNode.opacity = 1;
    maskNode.scale = Vec2{1, 1};
    maskNode.name = kInvalidString;
    scene.nodes.push_back(maskNode);

    scene.verbPool = {static_cast<uint8_t>(PathVerb::Move), static_cast<uint8_t>(PathVerb::Cubic),
                      static_cast<uint8_t>(PathVerb::Close)};
    scene.floatPool = {0, 0, 10, 0, 20, 10, 30, 0, 4.0f, 2.0f};  // points [0,8), dash [8,10)
    scene.stopPool = {IrGradientStop{0.0f, Color{1, 0, 0, 1}}, IrGradientStop{1.0f, Color{0, 0, 1, 1}}};

    IrPathData path{};
    path.verbOffset = 0;
    path.verbCount = 3;
    path.pointOffset = 0;
    path.pointCount = 8;
    path.fillRule = static_cast<uint8_t>(FillRule::NonZero);
    path.fillKind = static_cast<uint8_t>(PaintKind::Linear);
    path.strokeKind = static_cast<uint8_t>(PaintKind::Solid);
    path.cap = static_cast<uint8_t>(LineCap::Round);
    path.join = static_cast<uint8_t>(LineJoin::Bevel);
    path.fillColor = Color{0, 0, 0, 1};
    path.strokeColor = Color{1, 0, 0, 1};
    path.fillP0 = Vec2{0, 0};
    path.fillP1 = Vec2{10, 10};
    path.strokeP0 = Vec2{0, 0};
    path.strokeP1 = Vec2{0, 0};
    path.fillStopOffset = 0;
    path.fillStopCount = 2;
    path.strokeStopOffset = 0;
    path.strokeStopCount = 0;
    path.strokeWidth = 2.0f;
    path.miterLimit = 4.0f;
    path.trimStart = 0;
    path.trimEnd = 1;
    path.trimOffset = 0;
    path.dashOffset = 8;
    path.dashCount = 2;
    path.dashPhase = 0.5f;
    scene.paths.push_back(path);

    IrImageData image{};
    image.asset = f.embeddedAssetId;
    image.width = 64;
    image.height = 64;
    scene.images.push_back(image);

    // T-42c: 2 3D nodes (non-zero translateZ/rotationX/rotationY/orientation/is3D), 1 camera node
    // + IrCameraData, 2 scene effects (the second referencing the scene's stopPool, GradientMask-
    // style).
    IrSceneNode group3D{};
    group3D.kind = static_cast<uint8_t>(NodeKind::Group);
    group3D.visible = 1;
    group3D.parent = 0;
    group3D.opacity = 1;
    group3D.scale = Vec2{1, 1};
    group3D.name = kInvalidString;
    group3D.translateZ = 5.0f;
    group3D.anchorZ = 1.0f;
    group3D.rotationX = 15.0f;
    group3D.rotationY = 30.0f;
    group3D.orientation = Vec3{0.1f, 0.2f, 0.3f};
    group3D.is3D = 1;
    group3D.backfaceHidden = 1;
    group3D.effectOffset = 0;
    group3D.effectCount = 2;
    scene.nodes.push_back(group3D);

    IrSceneNode path3D{};
    path3D.kind = static_cast<uint8_t>(NodeKind::Path);
    path3D.visible = 1;
    path3D.parent = 0;
    path3D.kindIndex = 0;
    path3D.opacity = 1;
    path3D.scale = Vec2{1, 1};
    path3D.name = kInvalidString;
    path3D.translateZ = -2.5f;
    path3D.rotationX = 5.0f;
    path3D.orientation = Vec3{0, 0, 1};
    path3D.is3D = 1;
    scene.nodes.push_back(path3D);

    IrCameraData sceneCamera{};
    sceneCamera.kind = 0;
    sceneCamera.useTarget = 1;
    sceneCamera.zoom = 1.0f;
    sceneCamera.target = Vec3{0, 0, -1};
    sceneCamera.nearZ = 0.1f;
    sceneCamera.farZ = 1000.0f;
    scene.cameras.push_back(sceneCamera);

    IrSceneNode cameraNode{};
    cameraNode.kind = static_cast<uint8_t>(NodeKind::Camera);
    cameraNode.visible = 1;
    cameraNode.parent = 0;
    cameraNode.kindIndex = 0;
    cameraNode.opacity = 1;
    cameraNode.scale = Vec2{1, 1};
    cameraNode.name = kInvalidString;
    scene.nodes.push_back(cameraNode);

    IrEffect sceneEffect0{};
    sceneEffect0.kind = 0;
    sceneEffect0.enabled = 1;
    sceneEffect0.shader = 0xFFFFFFFFu;
    scene.effects.push_back(sceneEffect0);

    IrEffect sceneEffect1{};
    sceneEffect1.kind = 6;
    sceneEffect1.enabled = 1;
    sceneEffect1.shader = 0xFFFFFFFFu;
    sceneEffect1.stopOffset = 0;
    sceneEffect1.stopCount = 2;  // reuses scene.stopPool's 2 stops
    scene.effects.push_back(sceneEffect1);

    f.sceneId = w.addScene(scene);

    // TLIN: Float track (3 keyframes, CubicBezier then Hold) + Path track (2 keyframes into
    // timeline floatPool) + 1 marker.
    TimelineBuilder tl;
    tl.name = timelineName;
    tl.duration = 2.0f;
    tl.frameRate = 30.0f;

    IrTrack floatTrack{};
    floatTrack.target = 1;
    floatTrack.prop = 34;  // FillOpacity (T-257b: a Float track needs a Float property)
    floatTrack.type = static_cast<uint8_t>(ValueType::Float);
    floatTrack.kfOffset = 0;
    floatTrack.kfCount = 3;
    tl.tracks.push_back(floatTrack);

    IrTrack pathTrack{};
    pathTrack.target = 1;
    pathTrack.prop = 32;  // PathData
    pathTrack.type = static_cast<uint8_t>(ValueType::Path);
    pathTrack.kfOffset = 3;
    pathTrack.kfCount = 2;
    tl.tracks.push_back(pathTrack);

    IrKeyframe kf0{};
    kf0.time = 0.0f;
    kf0.easing = IrEasing{static_cast<uint8_t>(EasingKind::CubicBezier), 0, 0, 0.25f, 0.1f, 0.25f, 1.0f};
    irSetValueF32(kf0.value, 0, 0.0f);
    tl.keyframes.push_back(kf0);

    IrKeyframe kf1{};
    kf1.time = 1.0f;
    kf1.easing = IrEasing{static_cast<uint8_t>(EasingKind::Hold), 0, 0, 0, 0, 0, 0};
    irSetValueF32(kf1.value, 0, 0.5f);
    tl.keyframes.push_back(kf1);

    IrKeyframe kf2{};
    kf2.time = 2.0f;
    kf2.easing = IrEasing{static_cast<uint8_t>(EasingKind::Linear), 0, 0, 0, 0, 0, 0};
    irSetValueF32(kf2.value, 0, 1.0f);
    tl.keyframes.push_back(kf2);

    IrKeyframe kf3{};
    kf3.time = 0.0f;
    kf3.easing = IrEasing{static_cast<uint8_t>(EasingKind::Linear), 0, 0, 0, 0, 0, 0};
    irSetValueU32(kf3.value, 0, 0);
    irSetValueU32(kf3.value, 1, 4);
    tl.keyframes.push_back(kf3);

    IrKeyframe kf4{};
    kf4.time = 1.5f;
    kf4.easing = IrEasing{static_cast<uint8_t>(EasingKind::Linear), 0, 0, 0, 0, 0, 0};
    irSetValueU32(kf4.value, 0, 4);
    irSetValueU32(kf4.value, 1, 4);
    tl.keyframes.push_back(kf4);

    tl.floatPool = {1, 2, 3, 4, 5, 6, 7, 8};

    IrMarker marker{};
    marker.name = markerName;
    marker.start = 0.5f;
    marker.duration = 0.2f;
    tl.markers.push_back(marker);

    f.timelineId = w.addTimeline(tl);

    // STYL: rule ".box:hover" (class+state) + rule "div > div" (child combinator), 3 decls
    // (Float, Color, ListRef->1 transition), 1 keyframesRef, 1 animation, 1 font family.
    StyleBuilder style;
    style.classIds = {boxClass};

    IrSelPart boxHover{};
    boxHover.combinator = 0;
    boxHover.classCount = 1;
    boxHover.stateMask = 1;  // kStateHover
    boxHover.tag = kInvalidString;
    boxHover.id = kInvalidString;
    boxHover.classOffset = 0;
    style.selectorParts.push_back(boxHover);

    IrSelPart divSubject{};
    divSubject.combinator = 0;
    divSubject.classCount = 0;
    divSubject.tag = divTag;
    divSubject.id = kInvalidString;
    divSubject.classOffset = 1;
    style.selectorParts.push_back(divSubject);

    IrSelPart divAncestor{};
    divAncestor.combinator = 2;  // child '>'
    divAncestor.classCount = 0;
    divAncestor.tag = divTag;
    divAncestor.id = kInvalidString;
    divAncestor.classOffset = 1;
    style.selectorParts.push_back(divAncestor);

    IrDecl floatDecl{};
    floatDecl.prop = 0;  // Opacity
    floatDecl.valueType = static_cast<uint8_t>(ValueType::Float);
    irSetValueF32(floatDecl.value, 0, 0.5f);
    style.decls.push_back(floatDecl);

    IrDecl colorDecl{};
    colorDecl.prop = 96;  // BackgroundColor
    colorDecl.valueType = static_cast<uint8_t>(ValueType::Color);
    irSetValueF32(colorDecl.value, 0, 1.0f);
    irSetValueF32(colorDecl.value, 1, 0.0f);
    irSetValueF32(colorDecl.value, 2, 0.0f);
    irSetValueF32(colorDecl.value, 3, 1.0f);
    style.decls.push_back(colorDecl);

    IrDecl listRefDecl{};
    listRefDecl.prop = 180;  // transition
    listRefDecl.valueType = static_cast<uint8_t>(ValueType::ListRef);
    listRefDecl.important = 1;
    irSetValueU32(listRefDecl.value, 0, 0);
    irSetValueU32(listRefDecl.value, 1, 1);
    style.decls.push_back(listRefDecl);

    IrRule rule0{};
    rule0.selOffset = 0;
    rule0.selCount = 1;
    rule0.declOffset = 0;
    rule0.declCount = 2;
    rule0.specificity = 512;
    style.rules.push_back(rule0);

    IrRule rule1{};
    rule1.selOffset = 1;
    rule1.selCount = 2;
    rule1.declOffset = 2;
    rule1.declCount = 1;
    rule1.specificity = 2;
    style.rules.push_back(rule1);

    IrKeyframesRef kfRef{};
    kfRef.name = timelineName;
    kfRef.timeline = f.timelineId;
    style.keyframesRefs.push_back(kfRef);

    IrTransitionSpec transition{};
    transition.prop = 0xFFFF;
    transition.duration = 0.3f;
    transition.delay = 0.0f;
    transition.easing =
        IrEasing{static_cast<uint8_t>(EasingKind::CubicBezier), 0, 0, 0.25f, 0.1f, 0.25f, 1.0f};
    style.transitions.push_back(transition);

    IrAnimationSpec animation{};
    animation.timeline = f.timelineId;
    animation.duration = 1.0f;
    animation.delay = 0.0f;
    animation.easing = IrEasing{static_cast<uint8_t>(EasingKind::Linear), 0, 0, 0, 0, 0, 0};
    animation.iterations = 1;
    style.animations.push_back(animation);

    style.fontFamilies = {arialFamily};

    IrFontFace fontFace{};
    fontFace.family = arialFamily;
    fontFace.asset = f.externalAssetId;
    fontFace.weight = 700;
    fontFace.italic = 1;
    style.fontFaces.push_back(fontFace);

    // T-42c: 2 STYL effects, the first referencing 1 effectStop.
    style.effectStops.push_back(IrGradientStop{0.5f, Color{0, 1, 0, 1}});

    IrEffect styleEffect0{};
    styleEffect0.kind = 1;
    styleEffect0.enabled = 1;
    styleEffect0.shader = 0xFFFFFFFFu;
    styleEffect0.stopOffset = 0;
    styleEffect0.stopCount = 1;
    style.effects.push_back(styleEffect0);

    IrEffect styleEffect1{};
    styleEffect1.kind = 128;
    styleEffect1.enabled = 0;
    styleEffect1.shader = f.externalAssetId;
    style.effects.push_back(styleEffect1);

    w.setStyle(style);

    // DOCT: root (anim, references the scene) + child with class + inline decl + child with text.
    DocBuilder doc;
    doc.classPool = {boxClass};

    // T-42c: 1 DOCT camera, selected by the root element.
    IrCameraData docCamera{};
    docCamera.kind = 1;  // orthographic
    docCamera.useTarget = 0;
    docCamera.zoom = 1.0f;
    docCamera.nearZ = 0.1f;
    docCamera.farZ = 1000.0f;
    doc.cameras.push_back(docCamera);

    IrDocElement rootEl{};
    rootEl.parent = 0xFFFFFFFFu;
    rootEl.tag = animTag;
    rootEl.id = kInvalidString;
    rootEl.classOffset = 0;
    rootEl.classCount = 0;
    rootEl.attrOffset = 0;
    rootEl.attrCount = 0;
    rootEl.inlineDeclOffset = 0;
    rootEl.inlineDeclCount = 0;
    rootEl.scene = f.sceneId;
    rootEl.firstTextChild = 0xFFFFFFFFu;
    rootEl.cameraIndex = 0;
    doc.elements.push_back(rootEl);

    IrAttr attr{};
    attr.name = dataXAttr;
    attr.value = attrValue42;
    doc.attrPool.push_back(attr);

    IrDecl inlineOpacity{};
    inlineOpacity.prop = 0;
    inlineOpacity.valueType = static_cast<uint8_t>(ValueType::Float);
    irSetValueF32(inlineOpacity.value, 0, 0.8f);
    doc.inlineDecls.push_back(inlineOpacity);

    IrDocElement classEl{};
    classEl.parent = 0;
    classEl.tag = divTag;
    classEl.id = kInvalidString;
    classEl.classOffset = 0;
    classEl.classCount = 1;
    classEl.attrOffset = 0;
    classEl.attrCount = 1;
    classEl.inlineDeclOffset = 0;
    classEl.inlineDeclCount = 1;
    classEl.scene = 0xFFFFFFFFu;
    classEl.firstTextChild = 0xFFFFFFFFu;
    classEl.cameraIndex = 0xFFFFFFFFu;
    doc.elements.push_back(classEl);

    IrDocElement textEl{};
    textEl.parent = 0;
    textEl.tag = spanTag;
    textEl.id = kInvalidString;
    textEl.classOffset = 1;
    textEl.classCount = 0;
    textEl.attrOffset = 1;
    textEl.attrCount = 0;
    textEl.inlineDeclOffset = 1;
    textEl.inlineDeclCount = 0;
    textEl.scene = 0xFFFFFFFFu;
    textEl.firstTextChild = 0;
    textEl.cameraIndex = 0xFFFFFFFFu;
    doc.elements.push_back(textEl);

    IrDocText text{};
    text.parent = 2;
    text.text = helloText;
    doc.texts.push_back(text);

    w.setDoc(doc);

    // T-42c: SRCM, one entry per element then per text (3 elements + 1 text = 4 entries).
    w.setSourceMap({sourceFile}, {IrSourceEntry{0, 1, 1}, IrSourceEntry{0, 2, 3}, IrSourceEntry{0, 3, 5},
                                  IrSourceEntry{0, 3, 10}});

    w.setMeta(sourceFile, compilerVersion, 1700000000ull);

    return f;
}

}  // namespace

TEST_CASE("crc32 test vector") {
    const uint8_t data[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(crc32(data, sizeof(data)) == 0xCBF43926u);
}

TEST_CASE("IR round-trip: build, open, verify fields, re-serialize identically") {
    Fixture f = buildFixture();
    std::vector<uint8_t> bytes1 = f.writer.finish();
    REQUIRE(!bytes1.empty());

    Result<IrView> r1 = IrView::open(IrBlob{bytes1.data(), bytes1.size()}, IrReadOptions{true});
    REQUIRE(r1.ok());
    const IrView& view = r1.value();

    // Strings: check every interned string round-trips.
    REQUIRE(view.strings().count() == f.internedInOrder.size());
    for (uint32_t i = 0; i < view.strings().count(); ++i) {
        CHECK(view.string(i) == f.internedInOrder[i]);
    }

    // Assets.
    REQUIRE(view.assets().assets().size() == 2);
    const IrAsset& embeddedAsset = view.assets().assets()[0];
    CHECK(embeddedAsset.embedded == 1);
    CHECK(embeddedAsset.kind == 0);
    std::span<const uint8_t> embeddedData = view.assets().data(embeddedAsset);
    REQUIRE(embeddedData.size() == 4);
    CHECK(embeddedData[0] == 0x89);
    CHECK(embeddedData[3] == 0x47);
    const IrAsset& externalAsset = view.assets().assets()[1];
    CHECK(externalAsset.embedded == 0);
    CHECK(externalAsset.kind == 1);

    // Style.
    const StyleChunk& style = view.style();
    REQUIRE(style.rules().size() == 2);
    CHECK(style.rules()[0].specificity == 512);
    CHECK(style.rules()[1].selCount == 2);
    REQUIRE(style.selectorParts().size() == 3);
    CHECK(style.selectorParts()[0].stateMask == 1);
    CHECK(style.selectorParts()[2].combinator == 2);
    REQUIRE(style.decls().size() == 3);
    CHECK(irValueF32(style.decls()[0].value, 0) == doctest::Approx(0.5f));
    CHECK(irValueF32(style.decls()[1].value, 2) == doctest::Approx(0.0f));
    CHECK(style.decls()[2].important == 1);
    CHECK(irValueU32(style.decls()[2].value, 1) == 1);
    REQUIRE(style.keyframesRefs().size() == 1);
    CHECK(style.keyframesRefs()[0].timeline == f.timelineId);
    REQUIRE(style.transitions().size() == 1);
    CHECK(style.transitions()[0].duration == doctest::Approx(0.3f));
    REQUIRE(style.animations().size() == 1);
    CHECK(style.animations()[0].timeline == f.timelineId);
    REQUIRE(style.fontFamilies().size() == 1);
    REQUIRE(style.fontFaces().size() == 1);
    CHECK(view.string(style.fontFaces()[0].family) == "Arial");
    CHECK(style.fontFaces()[0].asset == f.externalAssetId);
    CHECK(style.fontFaces()[0].weight == 700);
    CHECK(style.fontFaces()[0].italic == 1);
    REQUIRE(style.effects().size() == 2);
    CHECK(style.effects()[0].stopCount == 1);
    CHECK(style.effects()[1].kind == 128);
    REQUIRE(style.effectStops().size() == 1);
    CHECK(style.effectStops()[0].offset == doctest::Approx(0.5f));

    // Scene.
    REQUIRE(view.sceneCount() == 1);
    const SceneChunk& scene = view.scene(f.sceneId);
    CHECK(scene.name() == f.sceneNameId);
    CHECK(scene.width() == doctest::Approx(100.0f));
    CHECK(scene.height() == doctest::Approx(200.0f));
    REQUIRE(scene.nodes().size() == 7);
    CHECK(scene.nodes()[0].parent == 0xFFFFFFFFu);
    CHECK(scene.nodes()[1].kind == static_cast<uint8_t>(NodeKind::Path));
    REQUIRE(scene.paths().size() == 1);
    CHECK(scene.paths()[0].pointCount == 8);
    CHECK(scene.paths()[0].dashCount == 2);
    REQUIRE(scene.images().size() == 1);
    CHECK(scene.images()[0].asset == f.embeddedAssetId);
    REQUIRE(scene.verbPool().size() == 3);
    REQUIRE(scene.floatPool().size() == 10);
    REQUIRE(scene.stopPool().size() == 2);

    // T-42c: 3D nodes, camera, effects.
    CHECK(scene.nodes()[4].is3D == 1);
    CHECK(scene.nodes()[4].translateZ == doctest::Approx(5.0f));
    CHECK(scene.nodes()[4].orientation.z == doctest::Approx(0.3f));
    CHECK(scene.nodes()[6].kind == static_cast<uint8_t>(NodeKind::Camera));
    CHECK(scene.nodes()[6].kindIndex == 0);
    REQUIRE(scene.cameras().size() == 1);
    CHECK(scene.cameras()[0].useTarget == 1);
    CHECK(scene.cameras()[0].target.z == doctest::Approx(-1.0f));
    REQUIRE(scene.effects().size() == 2);
    CHECK(scene.effects()[1].stopCount == 2);

    // Timeline.
    REQUIRE(view.timelineCount() == 1);
    const TimelineChunk& tl = view.timeline(f.timelineId);
    CHECK(tl.name() == f.timelineNameId);
    CHECK(tl.duration() == doctest::Approx(2.0f));
    REQUIRE(tl.tracks().size() == 2);
    CHECK(tl.tracks()[0].kfCount == 3);
    CHECK(tl.tracks()[1].type == static_cast<uint8_t>(ValueType::Path));
    REQUIRE(tl.keyframes().size() == 5);
    CHECK(tl.keyframes()[0].easing.kind == static_cast<uint8_t>(EasingKind::CubicBezier));
    CHECK(tl.keyframes()[1].easing.kind == static_cast<uint8_t>(EasingKind::Hold));
    CHECK(irValueU32(tl.keyframes()[3].value, 1) == 4);
    REQUIRE(tl.markers().size() == 1);
    CHECK(tl.markers()[0].start == doctest::Approx(0.5f));

    // Doc.
    const DocChunk& doc = view.doc();
    REQUIRE(doc.elements().size() == 3);
    CHECK(doc.elements()[0].scene == f.sceneId);
    CHECK(doc.elements()[1].classCount == 1);
    CHECK(doc.elements()[2].firstTextChild == 0);
    REQUIRE(doc.texts().size() == 1);
    CHECK(doc.texts()[0].text == f.helloTextId);
    REQUIRE(doc.attrPool().size() == 1);
    REQUIRE(doc.inlineDecls().size() == 1);
    CHECK(irValueF32(doc.inlineDecls()[0].value, 0) == doctest::Approx(0.8f));
    REQUIRE(doc.cameras().size() == 1);
    CHECK(doc.cameras()[0].kind == 1);
    CHECK(doc.elements()[0].cameraIndex == 0);
    CHECK(doc.elements()[1].cameraIndex == 0xFFFFFFFFu);

    // SRCM.
    CHECK(view.hasSourceMap());
    REQUIRE(view.sourceMap().entries().size() == 4);
    CHECK(view.sourceOf(0).line == 1);
    CHECK(view.sourceOf(3).col == 10);
    CHECK(view.sourceFileName(0) == "src/test.xt");

    // Meta.
    CHECK(view.meta().present());
    REQUIRE(view.meta().meta() != nullptr);
    CHECK(view.meta().meta()->compileTimeUnix == 1700000000ull);

    // IrWriter::fromView(view).finish() must equal the original bytes exactly.
    IrWriter rebuilt = IrWriter::fromView(view);
    std::vector<uint8_t> bytes2 = rebuilt.finish();
    CHECK(bytes1 == bytes2);

    // finish() is deterministic.
    std::vector<uint8_t> bytes3 = f.writer.finish();
    CHECK(bytes1 == bytes3);
}

namespace {

// A minimal file whose STYL chunk is the only thing that matters here (docs/design/02-ir-format.md
// §2.6's `gradientPool[]`, T-115). `withGradient == false` leaves the pool empty, which is what an
// IR minor 0 file -- written before T-115 -- looks like once its trailing count is cut off (see
// the test below).
std::vector<uint8_t> buildStyleOnlyIr(bool withGradient) {
    IrWriter w;
    StyleBuilder style;
    IrDecl decl{};
    decl.prop = 190;  // background-gradient (05.4).
    decl.valueType = static_cast<uint8_t>(ValueType::ListRef);
    irSetValueU32(decl.value, 0, 0);
    irSetValueU32(decl.value, 1, withGradient ? 1 : 0);
    style.decls.push_back(decl);
    if (withGradient) {
        style.effectStops.push_back(IrGradientStop{0.0f, Color{1, 0, 0, 1}});
        style.effectStops.push_back(IrGradientStop{1.0f, Color{0, 0, 1, 1}});
        IrGradient gradient{};
        gradient.kind = 1;  // GradientKind::Linear
        gradient.params[0] = 90;
        gradient.stopOffset = 0;
        gradient.stopCount = 2;
        style.gradients.push_back(gradient);
    }
    w.setStyle(std::move(style));
    return w.finish();
}

}  // namespace

TEST_CASE("IR: STYL gradientPool round-trips (T-115)") {
    std::vector<uint8_t> bytes = buildStyleOnlyIr(/*withGradient=*/true);
    Result<IrView> r = IrView::open(IrBlob{bytes.data(), bytes.size()}, IrReadOptions{true});
    REQUIRE(r.ok());
    const StyleChunk& style = r.value().style();
    REQUIRE(style.gradients().size() == 1);
    CHECK(style.gradients()[0].kind == 1);
    CHECK(style.gradients()[0].params[0] == doctest::Approx(90));
    CHECK(style.gradients()[0].stopCount == 2);
    REQUIRE(style.effectStops().size() == 2);
    CHECK(style.effectStops()[1].color.b == doctest::Approx(1));
    IrHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    CHECK(header.minor == kIrFormatMinor);  // T-115 bumped the IR minor version to 1, T-218 to 2.
}

TEST_CASE("IR: a STYL chunk without gradientPool (IR minor 0) still opens") {
    std::vector<uint8_t> bytes = buildStyleOnlyIr(/*withGradient=*/false);
    // Cut the trailing `uint32 gradientCount == 0` and `uint32 floatCount == 0` (T-218) off the
    // STYL payload, reproducing a file written before T-115 added the first of them. Every STYL array is
    // 4-byte sized, so the payload has no alignment padding and the count really is the last four bytes.
    size_t chunkPos = 0;
    for (size_t i = sizeof(IrHeader); i + 4 <= bytes.size(); i += 4) {
        if (std::memcmp(bytes.data() + i, "STYL", 4) == 0) {
            chunkPos = i;
            break;
        }
    }
    REQUIRE(chunkPos != 0);
    IrChunkHeader chunk{};
    std::memcpy(&chunk, bytes.data() + chunkPos, sizeof(chunk));
    size_t payloadPos = chunkPos + sizeof(IrChunkHeader);
    REQUIRE(chunk.payloadLength >= 8);
    REQUIRE(payloadPos + chunk.payloadLength <= bytes.size());
    bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(payloadPos + chunk.payloadLength - 8),
                bytes.begin() + static_cast<std::ptrdiff_t>(payloadPos + chunk.payloadLength));
    chunk.payloadLength -= 8;
    chunk.crc32 = crc32(bytes.data() + payloadPos, chunk.payloadLength);
    std::memcpy(bytes.data() + chunkPos, &chunk, sizeof(chunk));
    IrHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    header.minor = 0;
    header.totalLength -= 8;
    std::memcpy(bytes.data(), &header, sizeof(header));

    Result<IrView> r = IrView::open(IrBlob{bytes.data(), bytes.size()}, IrReadOptions{true});
    REQUIRE(r.ok());
    CHECK(r.value().style().gradients().empty());
    CHECK(r.value().style().decls().size() == 1);
}

// T-233 (IR minor 4): the stroke taper / width profile fields of IrPathData and IrShapeData.

namespace {

std::vector<uint8_t> buildTaperIr() {
    IrWriter w;
    SceneBuilder scene;
    scene.width = 100;
    scene.height = 100;
    IrSceneNode root{};
    root.kind = static_cast<uint8_t>(NodeKind::Group);
    root.visible = 1;
    root.parent = 0xFFFFFFFFu;
    root.scale = Vec2{1, 1};
    root.name = kInvalidString;
    scene.nodes.push_back(root);

    IrPathData p{};
    p.verbOffset = 0;
    p.verbCount = 2;
    scene.verbPool = {static_cast<uint8_t>(PathVerb::Move), static_cast<uint8_t>(PathVerb::Line)};
    p.pointOffset = 0;
    p.pointCount = 4;
    scene.floatPool = {0, 0, 100, 0};
    p.strokeKind = static_cast<uint8_t>(PaintKind::Solid);
    p.strokeColor = Color{0, 0, 0, 1};
    p.strokeWidth = 8;
    p.miterLimit = 4;
    p.trimEnd = 1;
    p.taperStart = 0.25f;
    p.taperEnd = 0.5f;
    p.profileOffset = static_cast<uint32_t>(scene.floatPool.size());
    p.profileCount = 2;
    scene.floatPool.insert(scene.floatPool.end(), {0.0f, 1.0f, 1.0f, 3.0f});
    scene.paths.push_back(p);

    IrSceneNode pn = root;
    pn.kind = static_cast<uint8_t>(NodeKind::Path);
    pn.parent = 0;
    pn.kindIndex = 0;
    scene.nodes.push_back(pn);
    w.addScene(std::move(scene));
    return w.finish();
}

}  // namespace

TEST_CASE("IR T-233: taper and width profile round-trip byte for byte (minor 4)") {
    std::vector<uint8_t> bytes = buildTaperIr();
    IrHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    CHECK(header.minor == kIrFormatMinor);
    Result<IrView> r = IrView::open(IrBlob{bytes.data(), bytes.size()}, IrReadOptions{true});
    REQUIRE(r.ok());
    REQUIRE(r.value().sceneCount() == 1);
    const SceneChunk& sc = r.value().scene(0);
    REQUIRE(sc.paths().size() == 1);
    CHECK(sc.paths()[0].taperStart == doctest::Approx(0.25f));
    CHECK(sc.paths()[0].taperEnd == doctest::Approx(0.5f));
    CHECK(sc.paths()[0].profileOffset == 4);
    CHECK(sc.paths()[0].profileCount == 2);
    std::vector<uint8_t> again = IrWriter::fromView(r.value()).finish();
    CHECK(again == bytes);
}

TEST_CASE("IR T-233: a minor 3 file's 136-byte path records open with taper / profile 0") {
    std::vector<uint8_t> bytes = buildTaperIr();
    // Rewrite the SCEN chunk as IR minor 3 wrote it: every IrPathData record 16 bytes shorter.
    size_t chunkPos = 0;
    for (size_t i = sizeof(IrHeader); i + 4 <= bytes.size(); i += 4) {
        if (std::memcmp(bytes.data() + i, "SCEN", 4) == 0) {
            chunkPos = i;
            break;
        }
    }
    REQUIRE(chunkPos != 0);
    IrChunkHeader chunk{};
    std::memcpy(&chunk, bytes.data() + chunkPos, sizeof(chunk));
    const size_t payloadPos = chunkPos + sizeof(IrChunkHeader);
    // name, width, height, then nodes[] (count + records), then paths[] (count + records).
    uint32_t nodeCount = 0;
    std::memcpy(&nodeCount, bytes.data() + payloadPos + 12, 4);
    const size_t pathsPos = payloadPos + 16 + static_cast<size_t>(nodeCount) * sizeof(IrSceneNode);
    uint32_t pathCount = 0;
    std::memcpy(&pathCount, bytes.data() + pathsPos, 4);
    REQUIRE(pathCount == 1);
    const size_t cut = sizeof(IrPathData) - kIrPathDataSizeV3;
    const size_t recordEnd = pathsPos + 4 + sizeof(IrPathData);
    bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(recordEnd - cut),
                bytes.begin() + static_cast<std::ptrdiff_t>(recordEnd));
    chunk.payloadLength -= static_cast<uint32_t>(cut);
    chunk.crc32 = crc32(bytes.data() + payloadPos, chunk.payloadLength);
    std::memcpy(bytes.data() + chunkPos, &chunk, sizeof(chunk));
    IrHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    header.minor = 3;
    header.totalLength -= static_cast<uint32_t>(cut);
    std::memcpy(bytes.data(), &header, sizeof(header));

    Result<IrView> r = IrView::open(IrBlob{bytes.data(), bytes.size()}, IrReadOptions{true});
    REQUIRE(r.ok());
    const SceneChunk& sc = r.value().scene(0);
    REQUIRE(sc.paths().size() == 1);
    CHECK(sc.paths()[0].strokeWidth == doctest::Approx(8.0f));
    CHECK(sc.paths()[0].miterLimit == doctest::Approx(4.0f));
    CHECK(sc.paths()[0].taperStart == 0.0f);
    CHECK(sc.paths()[0].taperEnd == 0.0f);
    CHECK(sc.paths()[0].profileCount == 0);
    // The widened records outlive copies of the view.
    IrView copy = r.value();
    CHECK(copy.scene(0).paths()[0].strokeWidth == doctest::Approx(8.0f));
}

// T-332 (05 §5.9.8): the optional `CLIP` chunk (clip-path shape pool).
namespace {

/// A minimal file with one `clip-path: circle(40px at 50% 50%)` rule declaration and its `CLIP`
/// entry.
std::vector<uint8_t> buildClipIr(bool declareNone = false) {
    IrWriter w;
    ElementClipShape circle;
    circle.kind = ClipShapeKind::Circle;
    circle.params = {Length{40, LengthUnit::Px}, Length{50, LengthUnit::Percent},
                     Length{50, LengthUnit::Percent}};
    const uint32_t index = w.addClipShape(circle);
    StyleBuilder style;
    IrDecl decl{};
    decl.prop = 207;  // clip-path (05 §5.9.8).
    decl.valueType = static_cast<uint8_t>(ValueType::Enum);
    irSetValueU32(decl.value, 0, declareNone ? 0 : index + 1);
    style.decls.push_back(decl);
    w.setStyle(std::move(style));
    return w.finish();
}

/// Rewrites the `CLIP` chunk's type to `type` in place (the CRC covers only the payload, so the
/// file stays well-formed) and returns the chunk's offset, or 0 when absent.
size_t renameClipChunk(std::vector<uint8_t>& bytes, const char type[4]) {
    for (size_t i = sizeof(IrHeader); i + 4 <= bytes.size(); i += 4) {
        if (std::memcmp(bytes.data() + i, "CLIP", 4) == 0) {
            std::memcpy(bytes.data() + i, type, 4);
            return i;
        }
    }
    return 0;
}

}  // namespace

TEST_CASE("IR: CLIP chunk round-trips and the IR version is unchanged (T-332)") {
    std::vector<uint8_t> bytes = buildClipIr();
    Result<IrView> r = IrView::open(IrBlob{bytes.data(), bytes.size()}, IrReadOptions{true});
    REQUIRE(r.ok());
    const ClipShapeChunk& clips = r.value().clipShapes();
    REQUIRE(clips.present());
    REQUIRE(clips.shapes().size() == 1);
    CHECK(clips.shapes()[0].kind == static_cast<uint8_t>(ClipShapeKind::Circle));
    REQUIRE(clips.shapes()[0].paramCount == kClipCircleParamCount);
    CHECK(clips.params()[0].value == doctest::Approx(40));
    CHECK(clips.params()[0].unit == static_cast<uint8_t>(LengthUnit::Px));
    CHECK(clips.params()[1].unit == static_cast<uint8_t>(LengthUnit::Percent));
    IrHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    CHECK(header.minor == kIrFormatMinor);  // No version bump: the chunk is optional.

    // IrWriter::fromView carries the chunk through byte for byte.
    std::vector<uint8_t> again = IrWriter::fromView(r.value()).finish();
    CHECK(again == bytes);
}

TEST_CASE("IR: a reader that does not know the CLIP chunk skips it and keeps the declaration (T-332)") {
    // The reader ignores unknown chunk types (02 §2.1): renaming `CLIP` to a type no reader knows
    // reproduces what a runtime from before T-332 does with this file -- it opens and no shape
    // pool is seen. (That runtime also keeps a `1 + index` declaration: StyleProp 207 is a plain
    // Enum it never checks against a pool and applyEnumDecl ignores; this reader validates the
    // index, so the declaration here is `none`.)
    std::vector<uint8_t> bytes = buildClipIr(/*declareNone=*/true);
    REQUIRE(renameClipChunk(bytes, "ZZQQ") != 0);
    Result<IrView> r = IrView::open(IrBlob{bytes.data(), bytes.size()}, IrReadOptions{true});
    REQUIRE(r.ok());
    CHECK_FALSE(r.value().clipShapes().present());
    REQUIRE(r.value().style().decls().size() == 1);
    CHECK(r.value().style().decls()[0].prop == 207);
}

TEST_CASE("IR: CLIP validation rejects a bad kind, count, unit and a dangling declaration (T-332)") {
    {
        IrWriter w;
        ElementClipShape bad;
        bad.kind = static_cast<ClipShapeKind>(9);
        bad.params = {Length{1, LengthUnit::Px}};
        w.addClipShape(bad);
        std::vector<uint8_t> bytes = w.finish();
        CHECK_FALSE(IrView::open(IrBlob{bytes.data(), bytes.size()}, IrReadOptions{true}).ok());
    }
    {
        IrWriter w;
        ElementClipShape bad;
        bad.kind = ClipShapeKind::Circle;
        bad.params = {Length{1, LengthUnit::Px}, Length{1, LengthUnit::Px}};  // Needs 3.
        w.addClipShape(bad);
        std::vector<uint8_t> bytes = w.finish();
        CHECK_FALSE(IrView::open(IrBlob{bytes.data(), bytes.size()}, IrReadOptions{true}).ok());
    }
    {
        IrWriter w;
        ElementClipShape bad;
        bad.kind = ClipShapeKind::Polygon;
        bad.params = {Length{1, LengthUnit::Em}, Length{1, LengthUnit::Px}, Length{1, LengthUnit::Px},
                      Length{1, LengthUnit::Px}, Length{1, LengthUnit::Px}, Length{1, LengthUnit::Px}};
        w.addClipShape(bad);
        std::vector<uint8_t> bytes = w.finish();
        CHECK_FALSE(IrView::open(IrBlob{bytes.data(), bytes.size()}, IrReadOptions{true}).ok());
    }
    {
        // A `closest-side` keyword on a polygon vertex is not a length.
        IrWriter w;
        ElementClipShape bad;
        bad.kind = ClipShapeKind::Polygon;
        bad.params = {Length{0, LengthUnit::Auto}, Length{1, LengthUnit::Px}, Length{1, LengthUnit::Px},
                      Length{1, LengthUnit::Px},   Length{1, LengthUnit::Px}, Length{1, LengthUnit::Px}};
        w.addClipShape(bad);
        std::vector<uint8_t> bytes = w.finish();
        CHECK_FALSE(IrView::open(IrBlob{bytes.data(), bytes.size()}, IrReadOptions{true}).ok());
    }
    {
        // A declaration pointing past the pool.
        IrWriter w;
        StyleBuilder style;
        IrDecl decl{};
        decl.prop = 207;
        decl.valueType = static_cast<uint8_t>(ValueType::Enum);
        irSetValueU32(decl.value, 0, 1);
        style.decls.push_back(decl);
        w.setStyle(std::move(style));
        std::vector<uint8_t> bytes = w.finish();
        CHECK_FALSE(IrView::open(IrBlob{bytes.data(), bytes.size()}, IrReadOptions{true}).ok());
    }
    {
        // ... and `none` (0) without any pool is fine.
        IrWriter w;
        StyleBuilder style;
        IrDecl decl{};
        decl.prop = 207;
        decl.valueType = static_cast<uint8_t>(ValueType::Enum);
        irSetValueU32(decl.value, 0, 0);
        style.decls.push_back(decl);
        w.setStyle(std::move(style));
        std::vector<uint8_t> bytes = w.finish();
        CHECK(IrView::open(IrBlob{bytes.data(), bytes.size()}, IrReadOptions{true}).ok());
    }
}

// T-338 (06 §6.15): the optional `GRID` chunk (grid track and area pools).
namespace {

/// A minimal file whose rule declares `grid-template-columns: 10px 1fr auto`,
/// `grid-template-areas: "a a" "b ."` and `grid-area: a`, with their `GRID` entries.
std::vector<uint8_t> buildGridIr(bool declareNone = false) {
    IrWriter w;
    const GridTrack tracks[] = {{GridTrackKind::Px, 10}, {GridTrackKind::Fr, 1}, {GridTrackKind::Auto, 0}};
    const uint32_t list = w.addGridTrackList(tracks);
    const StringId a = w.intern("a");
    const StringId b = w.intern("b");
    const GridArea areas[] = {{a, 0, 1, 0, 2}, {b, 1, 2, 0, 1}};
    const uint32_t areaList = w.addGridAreaList(areas, 2, 2);
    StyleBuilder style;
    IrDecl columns{};
    columns.prop = 264;  // grid-template-columns (05 §5.9.8).
    columns.valueType = static_cast<uint8_t>(ValueType::Enum);
    irSetValueU32(columns.value, 0, declareNone ? 0 : list + 1);
    style.decls.push_back(columns);
    IrDecl templ{};
    templ.prop = 266;  // grid-template-areas.
    templ.valueType = static_cast<uint8_t>(ValueType::Enum);
    irSetValueU32(templ.value, 0, declareNone ? 0 : areaList + 1);
    style.decls.push_back(templ);
    IrDecl area{};
    area.prop = 272;  // grid-area.
    area.valueType = static_cast<uint8_t>(ValueType::StringRef);
    irSetValueU32(area.value, 0, a);
    style.decls.push_back(area);
    w.setStyle(std::move(style));
    return w.finish();
}

size_t renameGridChunk(std::vector<uint8_t>& bytes, const char type[4]) {
    for (size_t i = sizeof(IrHeader); i + 4 <= bytes.size(); i += 4) {
        if (std::memcmp(bytes.data() + i, "GRID", 4) == 0) {
            std::memcpy(bytes.data() + i, type, 4);
            return i;
        }
    }
    return 0;
}

bool opensWith(IrWriter& w) {
    std::vector<uint8_t> bytes = w.finish();
    return IrView::open(IrBlob{bytes.data(), bytes.size()}, IrReadOptions{true}).ok();
}

}  // namespace

TEST_CASE("IR: GRID chunk round-trips and the IR version is unchanged (T-338)") {
    std::vector<uint8_t> bytes = buildGridIr();
    Result<IrView> r = IrView::open(IrBlob{bytes.data(), bytes.size()}, IrReadOptions{true});
    REQUIRE(r.ok());
    const GridChunk& grid = r.value().grid();
    REQUIRE(grid.present());
    REQUIRE(grid.trackLists().size() == 1);
    CHECK(grid.trackLists()[0].count == 3);
    REQUIRE(grid.tracks().size() == 3);
    CHECK(grid.tracks()[0].kind == static_cast<uint8_t>(GridTrackKind::Px));
    CHECK(grid.tracks()[0].value == doctest::Approx(10));
    CHECK(grid.tracks()[1].kind == static_cast<uint8_t>(GridTrackKind::Fr));
    CHECK(grid.tracks()[2].kind == static_cast<uint8_t>(GridTrackKind::Auto));
    REQUIRE(grid.areaLists().size() == 1);
    CHECK(grid.areaLists()[0].rows == 2);
    CHECK(grid.areaLists()[0].cols == 2);
    REQUIRE(grid.areas().size() == 2);
    CHECK(r.value().string(grid.areas()[0].name) == "a");
    CHECK(grid.areas()[0].colEnd == 2);
    CHECK(r.value().string(grid.areas()[1].name) == "b");
    IrHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    CHECK(header.minor == kIrFormatMinor);  // No version bump: the chunk is optional.

    std::vector<uint8_t> again = IrWriter::fromView(r.value()).finish();
    CHECK(again == bytes);
}

TEST_CASE("IR: a reader that does not know the GRID chunk skips it and keeps the declarations (T-338)") {
    std::vector<uint8_t> bytes = buildGridIr(/*declareNone=*/true);
    REQUIRE(renameGridChunk(bytes, "ZZQQ") != 0);
    Result<IrView> r = IrView::open(IrBlob{bytes.data(), bytes.size()}, IrReadOptions{true});
    REQUIRE(r.ok());
    CHECK_FALSE(r.value().grid().present());
    REQUIRE(r.value().style().decls().size() == 3);
    CHECK(r.value().style().decls()[0].prop == 264);
}

TEST_CASE("IR: GRID validation rejects bad tracks, areas, lines and dangling declarations (T-338)") {
    {
        IrWriter w;  // Unknown track kind.
        const GridTrack bad[] = {{static_cast<GridTrackKind>(7), 1}};
        w.addGridTrackList(bad);
        CHECK_FALSE(opensWith(w));
    }
    {
        IrWriter w;  // Negative size.
        const GridTrack bad[] = {{GridTrackKind::Px, -5}};
        w.addGridTrackList(bad);
        CHECK_FALSE(opensWith(w));
    }
    {
        IrWriter w;  // An area outside its matrix.
        const GridArea bad[] = {{w.intern("a"), 0, 3, 0, 1}};
        w.addGridAreaList(bad, 2, 2);
        CHECK_FALSE(opensWith(w));
    }
    {
        IrWriter w;  // An empty area.
        const GridArea bad[] = {{w.intern("a"), 1, 1, 0, 1}};
        w.addGridAreaList(bad, 2, 2);
        CHECK_FALSE(opensWith(w));
    }
    const auto declOnly = [](uint16_t prop, ValueType type, uint32_t value) {
        IrWriter w;
        StyleBuilder style;
        IrDecl decl{};
        decl.prop = prop;
        decl.valueType = static_cast<uint8_t>(type);
        irSetValueU32(decl.value, 0, value);
        style.decls.push_back(decl);
        w.setStyle(std::move(style));
        return opensWith(w);
    };
    CHECK_FALSE(declOnly(264, ValueType::Enum, 1));  // Past the (absent) track lists.
    CHECK_FALSE(declOnly(266, ValueType::Enum, 1));  // Past the area lists.
    CHECK(declOnly(265, ValueType::Enum, 0));        // `none` needs no pool.
    CHECK(declOnly(268, ValueType::Enum, kGridLineSpan | 2));
    CHECK_FALSE(declOnly(268, ValueType::Enum, kGridLineSpan));  // `span 0`.
    CHECK_FALSE(declOnly(271, ValueType::Enum, 5000));           // Past the line cap.
    CHECK_FALSE(declOnly(267, ValueType::Enum, 9));              // justify-items.
    CHECK_FALSE(declOnly(272, ValueType::Enum, 0));              // grid-area is a string.
}
