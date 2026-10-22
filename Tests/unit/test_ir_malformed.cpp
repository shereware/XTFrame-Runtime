#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/ir_writer.h"
#include "xt/ir.h"

namespace xt {
uint32_t crc32(const uint8_t* data, size_t size);
}  // namespace xt

using namespace xt;
using namespace xt::compile;

namespace {

// ---------------------------------------------------------------------------------------------
// Fixture (duplicated from test_ir_roundtrip.cpp -- see that file for field-by-field comments).
// Both files must build byte-identical IR, since the offsets below are computed from this exact
// layout. Keep the two copies in sync if either changes.
// ---------------------------------------------------------------------------------------------

struct Fixture {
    IrWriter writer;
    std::vector<std::string> internedInOrder;
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

    AssetBuilder embedded;
    embedded.name = logoName;
    embedded.kind = 0;
    embedded.embedded = {0x89, 0x50, 0x4E, 0x47};
    f.embeddedAssetId = w.addAsset(embedded);

    AssetBuilder external;
    external.name = fontName;
    external.kind = 1;
    f.externalAssetId = w.addAsset(external);

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
    scene.floatPool = {0, 0, 10, 0, 20, 10, 30, 0, 4.0f, 2.0f};
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

    // T-42c: 2 3D nodes, 1 camera node + IrCameraData, 2 scene effects (the second referencing the
    // scene's stopPool, GradientMask-style). Kept in sync with test_ir_roundtrip.cpp's fixture.
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
    sceneEffect1.stopCount = 2;
    scene.effects.push_back(sceneEffect1);

    f.sceneId = w.addScene(scene);

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
    pathTrack.prop = 32;
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

    StyleBuilder style;
    style.classIds = {boxClass};

    IrSelPart boxHover{};
    boxHover.combinator = 0;
    boxHover.classCount = 1;
    boxHover.stateMask = 1;
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
    divAncestor.combinator = 2;
    divAncestor.classCount = 0;
    divAncestor.tag = divTag;
    divAncestor.id = kInvalidString;
    divAncestor.classOffset = 1;
    style.selectorParts.push_back(divAncestor);

    IrDecl floatDecl{};
    floatDecl.prop = 0;
    floatDecl.valueType = static_cast<uint8_t>(ValueType::Float);
    irSetValueF32(floatDecl.value, 0, 0.5f);
    style.decls.push_back(floatDecl);

    IrDecl colorDecl{};
    colorDecl.prop = 96;
    colorDecl.valueType = static_cast<uint8_t>(ValueType::Color);
    irSetValueF32(colorDecl.value, 0, 1.0f);
    irSetValueF32(colorDecl.value, 1, 0.0f);
    irSetValueF32(colorDecl.value, 2, 0.0f);
    irSetValueF32(colorDecl.value, 3, 1.0f);
    style.decls.push_back(colorDecl);

    IrDecl listRefDecl{};
    listRefDecl.prop = 180;
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

// ---------------------------------------------------------------------------------------------
// Byte-level helpers
// ---------------------------------------------------------------------------------------------

void writeU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t v) {
    std::memcpy(bytes.data() + offset, &v, 4);
}

uint32_t readU32(const std::vector<uint8_t>& bytes, size_t offset) {
    uint32_t v = 0;
    std::memcpy(&v, bytes.data() + offset, 4);
    return v;
}

void writeF32(std::vector<uint8_t>& bytes, size_t offset, float v) {
    std::memcpy(bytes.data() + offset, &v, 4);
}

void flipBit(std::vector<uint8_t>& bytes, size_t byteOffset, int bit) {
    bytes[byteOffset] = static_cast<uint8_t>(bytes[byteOffset] ^ (1u << bit));
}

/// Returns the byte offset of each chunk header (STRT, ASST, STYL, SCEN, TLIN, DOCT, META for the
/// fixture above), assuming `bytes` is well-formed (as produced by IrWriter::finish()).
std::vector<size_t> chunkOffsets(const std::vector<uint8_t>& bytes) {
    std::vector<size_t> offs;
    size_t pos = 16;
    while (pos < bytes.size()) {
        offs.push_back(pos);
        uint32_t payloadLength = readU32(bytes, pos + 4);
        pos += 12 + payloadLength;
    }
    return offs;
}

Status openStatus(const std::vector<uint8_t>& bytes, bool verifyCrc = false) {
    Result<IrView> r = IrView::open(IrBlob{bytes.data(), bytes.size()}, IrReadOptions{verifyCrc});
    return r.status();
}

}  // namespace

TEST_CASE("malformed: header basics") {
    Fixture f = buildFixture();
    std::vector<uint8_t> good = f.writer.finish();

    CHECK(IrView::open(IrBlob{nullptr, 0}).status() == Status::kInvalidIr);

    std::vector<uint8_t> tiny(good.begin(), good.begin() + 15);
    CHECK(openStatus(tiny) == Status::kInvalidIr);

    std::vector<uint8_t> badMagic = good;
    badMagic[0] = 'X';
    badMagic[1] = 'T';
    badMagic[2] = 'I';
    badMagic[3] = 'X';
    CHECK(openStatus(badMagic) == Status::kInvalidIr);

    // major = 1 (the previous format version) must be rejected: 1.x files are no longer supported.
    std::vector<uint8_t> badMajor = good;
    badMajor[4] = 1;
    badMajor[5] = 0;
    CHECK(openStatus(badMajor) == Status::kInvalidIr);

    std::vector<uint8_t> badMajor3 = good;
    badMajor3[4] = 3;
    badMajor3[5] = 0;
    CHECK(openStatus(badMajor3) == Status::kInvalidIr);

    std::vector<uint8_t> lenPlus = good;
    writeU32(lenPlus, 12, readU32(good, 12) + 1);
    CHECK(openStatus(lenPlus) == Status::kInvalidIr);

    std::vector<uint8_t> lenMinus = good;
    writeU32(lenMinus, 12, readU32(good, 12) - 1);
    CHECK(openStatus(lenMinus) == Status::kInvalidIr);
}

TEST_CASE("malformed: truncated at every chunk boundary") {
    Fixture f = buildFixture();
    std::vector<uint8_t> good = f.writer.finish();
    std::vector<size_t> offs = chunkOffsets(good);
    std::vector<size_t> boundaries = offs;
    boundaries.push_back(good.size());

    for (size_t b : boundaries) {
        if (b == 0) {
            continue;
        }
        std::vector<uint8_t> truncated(good.begin(), good.begin() + static_cast<long>(b - 1));
        CHECK(openStatus(truncated) == Status::kInvalidIr);
    }
}

TEST_CASE("malformed: chunk payloadLength exceeds remaining bytes") {
    Fixture f = buildFixture();
    std::vector<uint8_t> good = f.writer.finish();
    std::vector<size_t> offs = chunkOffsets(good);

    std::vector<uint8_t> mutated = good;
    writeU32(mutated, offs[0] + 4, 0x7FFFFFFFu);
    CHECK(openStatus(mutated) == Status::kInvalidIr);
}

TEST_CASE("malformed: chunk ordering") {
    Fixture f = buildFixture();
    std::vector<uint8_t> good = f.writer.finish();
    std::vector<size_t> offs = chunkOffsets(good);

    // Delete DOCT (index 5): remove its bytes, chunkCount -= 1, totalLength adjusted.
    {
        std::vector<uint8_t> mutated = good;
        size_t doctStart = offs[5];
        uint32_t doctPayloadLen = readU32(good, doctStart + 4);
        size_t doctChunkSize = 12 + doctPayloadLen;
        mutated.erase(mutated.begin() + static_cast<long>(doctStart),
                      mutated.begin() + static_cast<long>(doctStart + doctChunkSize));
        writeU32(mutated, 8, readU32(good, 8) - 1);
        writeU32(mutated, 12, static_cast<uint32_t>(mutated.size()));
        CHECK(openStatus(mutated) == Status::kInvalidIr);
    }

    // Duplicate STRT (index 0): insert a copy of it right after itself.
    {
        std::vector<uint8_t> mutated = good;
        size_t strtStart = offs[0];
        uint32_t strtPayloadLen = readU32(good, strtStart + 4);
        size_t strtChunkSize = 12 + strtPayloadLen;
        std::vector<uint8_t> copy(good.begin() + static_cast<long>(strtStart),
                                  good.begin() + static_cast<long>(strtStart + strtChunkSize));
        mutated.insert(mutated.begin() + static_cast<long>(strtStart + strtChunkSize), copy.begin(),
                       copy.end());
        writeU32(mutated, 8, readU32(good, 8) + 1);
        writeU32(mutated, 12, static_cast<uint32_t>(mutated.size()));
        CHECK(openStatus(mutated) == Status::kInvalidIr);
    }

    // Swap STRT and ASST (indices 0, 1).
    {
        uint32_t strtPayloadLen = readU32(good, offs[0] + 4);
        uint32_t asstPayloadLen = readU32(good, offs[1] + 4);
        size_t strtChunkSize = 12 + strtPayloadLen;
        size_t asstChunkSize = 12 + asstPayloadLen;
        std::vector<uint8_t> strtBytes(good.begin() + static_cast<long>(offs[0]),
                                       good.begin() + static_cast<long>(offs[0] + strtChunkSize));
        std::vector<uint8_t> asstBytes(good.begin() + static_cast<long>(offs[1]),
                                       good.begin() + static_cast<long>(offs[1] + asstChunkSize));
        std::vector<uint8_t> mutated(good.begin(), good.begin() + static_cast<long>(offs[0]));
        mutated.insert(mutated.end(), asstBytes.begin(), asstBytes.end());
        mutated.insert(mutated.end(), strtBytes.begin(), strtBytes.end());
        mutated.insert(mutated.end(), good.begin() + static_cast<long>(offs[1] + asstChunkSize), good.end());
        REQUIRE(mutated.size() == good.size());
        CHECK(openStatus(mutated) == Status::kInvalidIr);
    }
}

TEST_CASE("malformed: CRC verification") {
    Fixture f = buildFixture();
    std::vector<uint8_t> good = f.writer.finish();
    std::vector<size_t> offs = chunkOffsets(good);

    // META (index 7: STRT,ASST,STYL,SCEN,TLIN,DOCT,SRCM,META) payload: sourceFile(4) +
    // compilerVersion(4) + compileTimeUnix(8). Flip a bit inside compileTimeUnix -- not validated
    // numerically, so this only affects the CRC.
    size_t metaPayloadStart = offs[7] + 12;
    std::vector<uint8_t> mutated = good;
    flipBit(mutated, metaPayloadStart + 8, 0);

    CHECK(openStatus(mutated, /*verifyCrc=*/true) == Status::kInvalidIr);
    CHECK(openStatus(mutated, /*verifyCrc=*/false) == Status::kOk);
}

TEST_CASE("malformed: STRT string table") {
    Fixture f = buildFixture();
    std::vector<uint8_t> good = f.writer.finish();
    std::vector<size_t> offs = chunkOffsets(good);
    size_t strtPayloadStart = offs[0] + 12;

    // offsets[0] pointing past the end of the string data.
    {
        std::vector<uint8_t> mutated = good;
        writeU32(mutated, strtPayloadStart + 4, 0xFFFFFFF0u);
        CHECK(openStatus(mutated) == Status::kInvalidIr);
    }

    // string 0's terminating NUL replaced with 'x'.
    {
        uint32_t count = static_cast<uint32_t>(f.internedInOrder.size());
        size_t dataStart = strtPayloadStart + 4 + count * 4u * 2u;
        std::vector<uint8_t> mutated = good;
        mutated[dataStart + f.internedInOrder[0].size()] = 'x';
        CHECK(openStatus(mutated) == Status::kInvalidIr);
    }
}

/// Returns the byte offset of `span`'s first element within `bytes` (assumes `bytes` is the exact
/// buffer `view` was opened from). Used instead of hardcoded byte-math so these tests stay correct
/// as struct sizes/chunk layouts evolve (they only need offsetof() for individual fields).
template <class T>
size_t spanOffset(const std::vector<uint8_t>& bytes, std::span<const T> span) {
    return static_cast<size_t>(reinterpret_cast<const uint8_t*>(span.data()) -
                               reinterpret_cast<const uint8_t*>(bytes.data()));
}

TEST_CASE("malformed: cross-reference checks") {
    Fixture f = buildFixture();
    std::vector<uint8_t> good = f.writer.finish();

    Result<IrView> opened = IrView::open(IrBlob{good.data(), good.size()}, IrReadOptions{false});
    REQUIRE(opened.ok());
    const IrView& view = opened.value();
    const StyleChunk& style = view.style();
    REQUIRE(view.sceneCount() > 0);
    const SceneChunk& scene = view.scene(0);
    REQUIRE(view.timelineCount() > 0);
    const TimelineChunk& tl = view.timeline(0);
    const DocChunk& doc = view.doc();

    // Rule[0].declOffset out of range.
    {
        std::vector<uint8_t> mutated = good;
        size_t off = spanOffset(good, style.rules());
        writeU32(mutated, off + offsetof(IrRule, declOffset), 0xFFFFu);
        CHECK(openStatus(mutated) == Status::kInvalidIr);
    }

    // SelPart[0].tag references a nonexistent string.
    {
        std::vector<uint8_t> mutated = good;
        size_t off = spanOffset(good, style.selectorParts());
        writeU32(mutated, off + offsetof(IrSelPart, tag), 0x00FFFFF0u);
        CHECK(openStatus(mutated) == Status::kInvalidIr);
    }

    // Decl[2] (ListRef) count out of range.
    {
        std::vector<uint8_t> mutated = good;
        size_t off = spanOffset(good, style.decls());
        writeU32(mutated, off + 2 * sizeof(IrDecl) + offsetof(IrDecl, value) + 4, 0xFFFFu);
        CHECK(openStatus(mutated) == Status::kInvalidIr);
    }

    // KeyframesRef[0].timeline out of range.
    {
        std::vector<uint8_t> mutated = good;
        size_t off = spanOffset(good, style.keyframesRefs());
        writeU32(mutated, off + offsetof(IrKeyframesRef, timeline), 0xFFFFFFF0u);
        CHECK(openStatus(mutated) == Status::kInvalidIr);
    }

    // STYL IrEffect[0]: enabled out of range ({0,1} only).
    {
        std::vector<uint8_t> mutated = good;
        size_t off = spanOffset(good, style.effects());
        mutated[off + offsetof(IrEffect, enabled)] = 2;
        CHECK(openStatus(mutated) == Status::kInvalidIr);
    }

    // STYL IrEffect[1]: kind == 9 (not in {0..7, 128}).
    {
        std::vector<uint8_t> mutated = good;
        size_t off = spanOffset(good, style.effects());
        mutated[off + sizeof(IrEffect) + offsetof(IrEffect, kind)] = 9;
        CHECK(openStatus(mutated) == Status::kInvalidIr);
    }

    // SceneNode[1].parent points forward (to an index after itself).
    {
        std::vector<uint8_t> mutated = good;
        size_t off = spanOffset(good, scene.nodes());
        writeU32(mutated, off + sizeof(IrSceneNode) + offsetof(IrSceneNode, parent), 2u);
        CHECK(openStatus(mutated) == Status::kInvalidIr);
    }

    // PathData[0].pointCount inconsistent with its verbs.
    {
        std::vector<uint8_t> mutated = good;
        size_t off = spanOffset(good, scene.paths());
        writeU32(mutated, off + offsetof(IrPathData, pointCount), 9u);
        CHECK(openStatus(mutated) == Status::kInvalidIr);
    }

    // SceneNode with kind == Camera (index 6): kindIndex out of range (only 1 camera exists).
    {
        std::vector<uint8_t> mutated = good;
        size_t off = spanOffset(good, scene.nodes());
        REQUIRE(scene.nodes()[6].kind == static_cast<uint8_t>(NodeKind::Camera));
        writeU32(mutated, off + 6 * sizeof(IrSceneNode) + offsetof(IrSceneNode, kindIndex), 5u);
        CHECK(openStatus(mutated) == Status::kInvalidIr);
    }

    // SceneNode[4].effectOffset/effectCount out of range (only 2 scene effects exist).
    {
        std::vector<uint8_t> mutated = good;
        size_t off = spanOffset(good, scene.nodes());
        writeU32(mutated, off + 4 * sizeof(IrSceneNode) + offsetof(IrSceneNode, effectCount), 5u);
        CHECK(openStatus(mutated) == Status::kInvalidIr);
    }

    // SCEN IrCameraData[0].kind out of range (must be <= 1).
    {
        std::vector<uint8_t> mutated = good;
        size_t off = spanOffset(good, scene.cameras());
        mutated[off + offsetof(IrCameraData, kind)] = 2;
        CHECK(openStatus(mutated) == Status::kInvalidIr);
    }

    // Track[0].kfCount == 0.
    {
        std::vector<uint8_t> mutated = good;
        size_t off = spanOffset(good, tl.tracks());
        writeU32(mutated, off + offsetof(IrTrack, kfCount), 0u);
        CHECK(openStatus(mutated) == Status::kInvalidIr);
    }

    // Keyframe[1].time not strictly increasing relative to Keyframe[0].time.
    {
        std::vector<uint8_t> mutated = good;
        size_t off = spanOffset(good, tl.keyframes());
        writeF32(mutated, off + sizeof(IrKeyframe) + offsetof(IrKeyframe, time), -1.0f);
        CHECK(openStatus(mutated) == Status::kInvalidIr);
    }

    // DocElement[0].scene out of range.
    {
        std::vector<uint8_t> mutated = good;
        size_t off = spanOffset(good, doc.elements());
        writeU32(mutated, off + offsetof(IrDocElement, scene), 0xFFFFFFF0u);
        CHECK(openStatus(mutated) == Status::kInvalidIr);
    }

    // DocElement[0].cameraIndex out of range (only 1 DOCT camera exists).
    {
        std::vector<uint8_t> mutated = good;
        size_t off = spanOffset(good, doc.elements());
        writeU32(mutated, off + offsetof(IrDocElement, cameraIndex), 5u);
        CHECK(openStatus(mutated) == Status::kInvalidIr);
    }

    // DOCT IrCameraData[0].kind out of range (must be <= 1).
    {
        std::vector<uint8_t> mutated = good;
        size_t off = spanOffset(good, doc.cameras());
        mutated[off + offsetof(IrCameraData, kind)] = 9;
        CHECK(openStatus(mutated) == Status::kInvalidIr);
    }

    // DocText[0].parent out of range.
    {
        std::vector<uint8_t> mutated = good;
        size_t off = spanOffset(good, doc.texts());
        writeU32(mutated, off + offsetof(IrDocText, parent), 0xFFFFFFF0u);
        CHECK(openStatus(mutated) == Status::kInvalidIr);
    }

    // SRCM: entryCount not equal to elementCount + textCount. Only the count field is decremented
    // (the trailing entry bytes are left in the payload, unconsumed but still in-bounds), so this
    // exercises the entryCount-vs-elementCount+textCount check specifically, not a bounds error.
    {
        REQUIRE(view.hasSourceMap());
        std::vector<size_t> offs = chunkOffsets(good);
        size_t srcmPayloadStart = offs[6] + 12;  // STRT,ASST,STYL,SCEN,TLIN,DOCT,SRCM
        uint32_t fileCount = readU32(good, srcmPayloadStart);
        size_t entryCountOffset = srcmPayloadStart + 4 + static_cast<size_t>(fileCount) * 4;
        std::vector<uint8_t> mutated = good;
        writeU32(mutated, entryCountOffset, readU32(good, entryCountOffset) - 1);
        CHECK(openStatus(mutated) == Status::kInvalidIr);
    }
}

TEST_CASE("malformed: unknown chunk type is skipped for forward compatibility") {
    Fixture f = buildFixture();
    std::vector<uint8_t> good = f.writer.finish();
    std::vector<size_t> offs = chunkOffsets(good);

    // Insert an unknown "ZZZZ" chunk right after DOCT (index 5, so right before SRCM).
    std::vector<uint8_t> zzzzPayload = {0, 0, 0, 0};
    IrChunkHeader zzzzHeader{};
    uint32_t zzzzType = fourcc('Z', 'Z', 'Z', 'Z');
    std::memcpy(zzzzHeader.type, &zzzzType, 4);
    zzzzHeader.payloadLength = static_cast<uint32_t>(zzzzPayload.size());
    zzzzHeader.crc32 = xt::crc32(zzzzPayload.data(), zzzzPayload.size());

    std::vector<uint8_t> mutated(good.begin(), good.begin() + static_cast<long>(offs[6]));
    const uint8_t* headerBytes = reinterpret_cast<const uint8_t*>(&zzzzHeader);
    mutated.insert(mutated.end(), headerBytes, headerBytes + sizeof(zzzzHeader));
    mutated.insert(mutated.end(), zzzzPayload.begin(), zzzzPayload.end());
    mutated.insert(mutated.end(), good.begin() + static_cast<long>(offs[6]), good.end());

    // minor = 5
    mutated[6] = 5;
    mutated[7] = 0;
    writeU32(mutated, 8, readU32(good, 8) + 1);
    writeU32(mutated, 12, static_cast<uint32_t>(mutated.size()));

    CHECK(openStatus(mutated) == Status::kOk);
}

TEST_CASE("malformed: scene node and path enum fields out of range") {
    Fixture f = buildFixture();
    std::vector<uint8_t> good = f.writer.finish();
    Result<IrView> opened = IrView::open(IrBlob{good.data(), good.size()}, IrReadOptions{false});
    REQUIRE(opened.ok());
    REQUIRE(opened.value().sceneCount() > 0);
    const SceneChunk& scene = opened.value().scene(0);
    REQUIRE(!scene.nodes().empty());
    const auto* base = reinterpret_cast<const uint8_t*>(good.data());
    size_t nodeOff = static_cast<size_t>(reinterpret_cast<const uint8_t*>(scene.nodes().data()) - base);

    SUBCASE("node kind 9 (fuzz finding, Tests/fuzz/crashes/ir-1.bin)") {
        std::vector<uint8_t> mutated = good;
        mutated[nodeOff + offsetof(IrSceneNode, kind)] = 9;
        CHECK(openStatus(mutated) == Status::kInvalidIr);
    }
    SUBCASE("node kind Element is not a scene node") {
        std::vector<uint8_t> mutated = good;
        mutated[nodeOff + offsetof(IrSceneNode, kind)] = static_cast<uint8_t>(NodeKind::Element);
        CHECK(openStatus(mutated) == Status::kInvalidIr);
    }
    SUBCASE("blend and maskMode out of range") {
        std::vector<uint8_t> mutated = good;
        mutated[nodeOff + offsetof(IrSceneNode, blend)] = 200;
        CHECK(openStatus(mutated) == Status::kInvalidIr);
        mutated = good;
        mutated[nodeOff + offsetof(IrSceneNode, maskMode)] = 7;
        CHECK(openStatus(mutated) == Status::kInvalidIr);
    }
    if (!scene.paths().empty()) {
        size_t pathOff = static_cast<size_t>(reinterpret_cast<const uint8_t*>(scene.paths().data()) - base);
        SUBCASE("path paint kinds, fill rule, cap and join out of range") {
            std::vector<uint8_t> mutated = good;
            mutated[pathOff + offsetof(IrPathData, fillKind)] = 4;
            CHECK(openStatus(mutated) == Status::kInvalidIr);
            mutated = good;
            mutated[pathOff + offsetof(IrPathData, fillRule)] = 2;
            CHECK(openStatus(mutated) == Status::kInvalidIr);
            mutated = good;
            mutated[pathOff + offsetof(IrPathData, join)] = 3;
            CHECK(openStatus(mutated) == Status::kInvalidIr);
        }
    }
}

// =============================================================================================
// T-255b (review 2026-10-02 L8): malformed-IR cases for the chunks added after T-03 (SYMB, TGRP,
// VARS, STAT, TRIG, STRS, PORT, BIND, RPT , CALC, MDIA, SHDR) and for every SCEN rig pool (bones,
// constraints, meshes, skins, tendons, weights, mesh pins, mesh triangles): a truncated chunk, a
// count past the payload and an index outside its pool, each rejected with kInvalidIr. The
// fixture is compiled from one small .xt that uses all of them.
// =============================================================================================

namespace {

const char* const kL8Source = R"XT(<xt><head><style>
  :root { --gap: 4px; }
  #Box { width: calc(10px + 2px); margin: var(--gap); opacity: var(--fade, 0.5); }
  @media (aspect-ratio >= 21/9) { #Box { height: 3px; } }
  #Fx { width: 10px; height: 10px; effect: custom("fx_ripple.xtfx", amplitude 6); }
  #Bar:state(Low) { background-color: #cc3333; }
  @timeline intro { duration: 1s; #Box { 0s { opacity: 0 } 1s { opacity: 1 } } }
  @timeline item-in { duration: 0.4s; #Row { 0s { opacity: 0 } 0.4s { opacity: 1 } } }
</style>
<symbol name="Btn"><div id="Face"></div></symbol>
</head><body>
<div id="P" port:hp="number in" port:items="list<struct{w: number}>"></div>
<div id="Box" on:hover="play(intro)"></div>
<div id="Fx"></div>
<div id="L">@hud_hp</div>
<div id="Bar" bind:width="hp | range(0, 100, 0px, 200px)" bind:state.Low="hp | lt(40)"></div>
<div id="List"><repeat port="items" id="Item" item-enter="play(item-in)">
  <div id="Row" bind:width="item(w)"></div></repeat></div>
<use symbol="Btn" id="B1"/>
<svg id="S" viewBox="0 0 100 100">
  <path d="M0 0 L10 0" xt-skin="Rig"><xt-weights bones="B">1 0:1</xt-weights></path>
  <xt-skeleton id="Rig"><xt-bone id="B" length="20" bind:rotate="hp">
    <xt-constraint kind="spring" damping="0.5"/></xt-bone></xt-skeleton>
  <xt-mesh href="m.png" width="10" height="10" xt-skin="Rig"><xt-weights bones="B"></xt-weights></xt-mesh>
</svg></body></xt>)XT";

const std::vector<uint8_t>& l8Fixture() {
    static const std::vector<uint8_t> ir = [] {
        CompileOptions options;
        options.emitMeta = false;
        options.rootDir = std::string(XT_TEST_XT_DIR) + "/../golden/ui";  // fx_ripple.xtfx
        std::vector<SourceFile> files{{"main.xt", kL8Source}};
        CompileResult result = compileDocumentFromStrings(files, "main.xt", options);
        for (const Diagnostic& d : result.diagnostics) {
            if (d.level == Diagnostic::Level::Error) {
                MESSAGE(d.message);
            }
        }
        return result.ok() ? result.ir : std::vector<uint8_t>{};
    }();
    return ir;
}

struct ChunkLoc {
    size_t payload = 0;
    uint32_t length = 0;
};

/// The `nth` chunk of `type` (a fourcc string) in a well-formed blob.
ChunkLoc findChunk(const std::vector<uint8_t>& bytes, const char* type, uint32_t nth = 0) {
    for (size_t off : chunkOffsets(bytes)) {
        if (std::memcmp(bytes.data() + off, type, 4) == 0 && nth-- == 0) {
            return ChunkLoc{off + 12, readU32(bytes, off + 4)};
        }
    }
    FAIL("chunk ", type, " is not in the fixture");
    return {};
}

/// `bytes` with the chunk at `c` cut to `length` payload bytes (the chunk and file lengths
/// adjusted, so only the chunk's own contents are short).
std::vector<uint8_t> withChunkLength(const std::vector<uint8_t>& bytes, const ChunkLoc& c, uint32_t length) {
    std::vector<uint8_t> out = bytes;
    out.erase(out.begin() + static_cast<std::ptrdiff_t>(c.payload + length),
              out.begin() + static_cast<std::ptrdiff_t>(c.payload + c.length));
    writeU32(out, c.payload - 8, length);
    writeU32(out, 12, static_cast<uint32_t>(out.size()));
    return out;
}

/// Truncation: the chunk cut to nothing, to its first count, and short by its last 4 bytes.
int checkTruncated(const std::vector<uint8_t>& good, const char* type, uint32_t nth = 0) {
    CAPTURE(type);
    ChunkLoc c = findChunk(good, type, nth);
    REQUIRE(c.length >= 8);
    CHECK(openStatus(withChunkLength(good, c, 0)) == Status::kInvalidIr);
    CHECK(openStatus(withChunkLength(good, c, 4)) == Status::kInvalidIr);
    CHECK(openStatus(withChunkLength(good, c, c.length - 4)) == Status::kInvalidIr);
    return 3;
}

/// Count too large: the count before `span` set one record past the payload's end, and to
/// 0xFFFFFFFF.
template <class T>
int checkCountPastPayload(const std::vector<uint8_t>& good, const ChunkLoc& c, std::span<const T> span,
                          const char* what) {
    CAPTURE(what);
    REQUIRE(!span.empty());
    const size_t at = spanOffset(good, span) - 4;
    REQUIRE(readU32(good, at) == span.size());
    const uint64_t past = (c.payload + c.length - (at + 4)) / sizeof(T) + 1;
    std::vector<uint8_t> mutated = good;
    writeU32(mutated, at, static_cast<uint32_t>(past));
    CHECK(openStatus(mutated) == Status::kInvalidIr);
    writeU32(mutated, at, 0xFFFFFFFFu);
    CHECK(openStatus(mutated) == Status::kInvalidIr);
    return 2;
}

/// The byte offset of field `field` of `span[index]` within `bytes`.
template <class T>
size_t fieldAt(const std::vector<uint8_t>& bytes, std::span<const T> span, size_t index, size_t field) {
    REQUIRE(index < span.size());
    return spanOffset(bytes, span) + index * sizeof(T) + field;
}

/// Index out of range: the u32 (or u16) at `offset` set to `value`.
int checkIndex(const std::vector<uint8_t>& good, size_t offset, uint32_t value, const char* what) {
    CAPTURE(what);
    std::vector<uint8_t> mutated = good;
    writeU32(mutated, offset, value);
    CHECK(openStatus(mutated) == Status::kInvalidIr);
    return 1;
}

int checkIndex16(const std::vector<uint8_t>& good, size_t offset, uint16_t value, const char* what) {
    CAPTURE(what);
    std::vector<uint8_t> mutated = good;
    std::memcpy(mutated.data() + offset, &value, 2);
    CHECK(openStatus(mutated) == Status::kInvalidIr);
    return 1;
}

uint32_t u32(size_t v) { return static_cast<uint32_t>(v); }

/// The first STYL declaration with valueType `type`.
size_t styleDeclIndex(const IrView& view, uint8_t type) {
    std::span<const IrDecl> decls = view.style().decls();
    for (size_t i = 0; i < decls.size(); ++i) {
        if (decls[i].valueType == type) {
            return i;
        }
    }
    FAIL("no declaration of that type in the fixture");
    return 0;
}

struct L8 {
    const std::vector<uint8_t>& good = l8Fixture();
    Result<IrView> opened = IrView::open(IrBlob{good.data(), good.size()}, IrReadOptions{false});
    const IrView& view() const { return opened.value(); }
    uint32_t strings() const { return view().strings().count(); }
    uint32_t elements() const { return u32(view().doc().elements().size()); }
};

}  // namespace

TEST_CASE("malformed: T-255b the L8 fixture compiles and opens") {
    L8 f;
    REQUIRE(!f.good.empty());
    REQUIRE(f.opened.ok());
}

TEST_CASE("malformed: T-255b SYMB") {
    L8 f;
    REQUIRE(f.opened.ok());
    const SymbolChunk& symb = f.view().symbols();
    REQUIRE(symb.instances().size() == 1);
    const IrSymbolInstance& inst = symb.instances()[0];
    ChunkLoc c = findChunk(f.good, "SYMB");
    int n = checkTruncated(f.good, "SYMB");
    // The instance count (records of at least 12 bytes) and the instance's path count.
    n += checkIndex(f.good, c.payload, (c.length - 4) / 12 + 1, "instance count");
    n += checkIndex(f.good, c.payload, 0xFFFFFFFFu, "instance count");
    n += checkCountPastPayload(f.good, c, inst.paths, "paths");
    n += checkIndex(f.good, c.payload + 4, f.strings(), "symbol name");
    n += checkIndex(f.good, c.payload + 8, f.elements(), "instanceElement");
    n +=
        checkIndex(f.good, fieldAt(f.good, inst.paths, 0, offsetof(IrSymbolPath, path)), f.strings(), "path");
    n += checkIndex(f.good, fieldAt(f.good, inst.paths, 0, offsetof(IrSymbolPath, element)), f.elements(),
                    "path element");
    MESSAGE("SYMB cases: ", n);
}

TEST_CASE("malformed: T-255b TGRP") {
    L8 f;
    REQUIRE(f.opened.ok());
    const TimelineGroupChunk& g = f.view().timelineGroups();
    ChunkLoc c = findChunk(f.good, "TGRP");
    int n = checkTruncated(f.good, "TGRP");
    n += checkCountPastPayload(f.good, c, g.groups(), "groups");
    n += checkCountPastPayload(f.good, c, g.members(), "members");
    n += checkIndex(f.good, fieldAt(f.good, g.groups(), 0, offsetof(IrTimelineGroup, name)), f.strings(),
                    "name");
    n += checkIndex(f.good, fieldAt(f.good, g.groups(), 0, offsetof(IrTimelineGroup, scopeElement)),
                    f.elements(), "scopeElement");
    n += checkIndex(f.good, fieldAt(f.good, g.groups(), 0, offsetof(IrTimelineGroup, memberOffset)),
                    u32(g.members().size()), "memberOffset");
    n += checkIndex(f.good, fieldAt(f.good, g.groups(), 0, offsetof(IrTimelineGroup, markerOffset)),
                    u32(g.markers().size()) + 1, "markerOffset");
    n += checkIndex(f.good, fieldAt(f.good, g.groups(), 0, offsetof(IrTimelineGroup, soundOffset)),
                    u32(g.sounds().size()) + 1, "soundOffset");
    n += checkIndex(f.good, fieldAt(f.good, g.members(), 0, offsetof(IrTimelineGroupMember, element)),
                    f.elements(), "member element");
    n += checkIndex(f.good, fieldAt(f.good, g.members(), 0, offsetof(IrTimelineGroupMember, timeline)),
                    f.view().timelineCount(), "member timeline");
    MESSAGE("TGRP cases: ", n);
}

TEST_CASE("malformed: T-255b VARS") {
    L8 f;
    REQUIRE(f.opened.ok());
    const VarsChunk& vars = f.view().vars();
    ChunkLoc c = findChunk(f.good, "VARS");
    int n = checkTruncated(f.good, "VARS");
    n += checkCountPastPayload(f.good, c, vars.tokens(), "tokens");
    n += checkCountPastPayload(f.good, c, vars.fallbacks(), "fallbacks");
    n += checkIndex(f.good, fieldAt(f.good, vars.tokens(), 0, offsetof(IrVar, name)), f.strings(),
                    "token name");
    // A STYL var() reference names a token string and a VARS fallback.
    std::span<const IrDecl> decls = f.view().style().decls();
    size_t withFallback = decls.size();
    for (size_t i = 0; i < decls.size(); ++i) {
        if (decls[i].valueType == kDeclVarRef && irValueU32(decls[i].value, 1) != kNoVarFallback) {
            withFallback = i;
        }
    }
    REQUIRE(withFallback < decls.size());
    const size_t value = fieldAt(f.good, decls, withFallback, offsetof(IrDecl, value));
    n += checkIndex(f.good, value, f.strings(), "var() name");
    n += checkIndex(f.good, value + 4, u32(vars.fallbacks().size()), "var() fallback");
    MESSAGE("VARS cases: ", n);
}

TEST_CASE("malformed: T-255b STAT") {
    L8 f;
    REQUIRE(f.opened.ok());
    const CustomStateChunk& st = f.view().customStates();
    ChunkLoc c = findChunk(f.good, "STAT");
    int n = checkTruncated(f.good, "STAT");
    n += checkCountPastPayload(f.good, c, st.names(), "names");
    n += checkIndex(f.good, spanOffset(f.good, st.names()), f.strings(), "name");
    // A State binding's targetId is a STAT name index.
    std::span<const IrBinding> binds = f.view().bindings().bindings();
    size_t state = binds.size();
    for (size_t i = 0; i < binds.size(); ++i) {
        if (binds[i].target == static_cast<uint8_t>(BindTarget::State)) {
            state = i;
        }
    }
    REQUIRE(state < binds.size());
    n += checkIndex16(f.good, fieldAt(f.good, binds, state, offsetof(IrBinding, targetId)),
                      static_cast<uint16_t>(st.names().size()), "State binding targetId");
    MESSAGE("STAT cases: ", n);
}

TEST_CASE("malformed: T-255b TRIG") {
    L8 f;
    REQUIRE(f.opened.ok());
    const TriggerChunk& trig = f.view().triggers();
    ChunkLoc c = findChunk(f.good, "TRIG");
    int n = checkTruncated(f.good, "TRIG");
    n += checkCountPastPayload(f.good, c, trig.tables(), "tables");
    n += checkCountPastPayload(f.good, c, trig.triggers(), "triggers");
    n += checkCountPastPayload(f.good, c, trig.actions(), "actions");
    n += checkIndex(f.good, fieldAt(f.good, trig.tables(), 0, offsetof(IrTriggerTable, element)),
                    f.elements(), "table element");
    n += checkIndex(f.good, fieldAt(f.good, trig.tables(), 0, offsetof(IrTriggerTable, triggerOffset)),
                    u32(trig.triggers().size()), "triggerOffset");
    n += checkIndex(f.good, fieldAt(f.good, trig.triggers(), 0, offsetof(IrTrigger, actionOffset)),
                    u32(trig.actions().size()), "actionOffset");
    REQUIRE(trig.actions()[0].kind == static_cast<uint8_t>(TriggerAction::Play));
    n += checkIndex(f.good, fieldAt(f.good, trig.actions(), 0, offsetof(IrTriggerAction, arg0)), f.strings(),
                    "Play timeline name");
    n += checkIndex16(f.good, fieldAt(f.good, trig.actions(), 0, offsetof(IrTriggerAction, next)),
                      static_cast<uint16_t>(trig.triggers()[0].actionCount + 1), "next");
    n += checkIndex16(f.good, fieldAt(f.good, trig.actions(), 0, offsetof(IrTriggerAction, alt)),
                      static_cast<uint16_t>(trig.triggers()[0].actionCount + 1), "alt");
    MESSAGE("TRIG cases: ", n);
}

TEST_CASE("malformed: T-255b STRS") {
    L8 f;
    REQUIRE(f.opened.ok());
    const LocalizationChunk& l10n = f.view().localization();
    ChunkLoc c = findChunk(f.good, "STRS");
    int n = checkTruncated(f.good, "STRS");
    n += checkCountPastPayload(f.good, c, l10n.keys(), "keys");
    n += checkCountPastPayload(f.good, c, l10n.textKeys(), "textKeys");
    n += checkIndex(f.good, spanOffset(f.good, l10n.keys()), f.strings(), "key");
    n += checkIndex(f.good, spanOffset(f.good, l10n.textKeys()), u32(l10n.keys().size()) + 1, "textKey");
    MESSAGE("STRS cases: ", n);
}

TEST_CASE("malformed: T-255b PORT") {
    L8 f;
    REQUIRE(f.opened.ok());
    const PortChunk& pc = f.view().ports();
    ChunkLoc c = findChunk(f.good, "PORT");
    int n = checkTruncated(f.good, "PORT");
    n += checkCountPastPayload(f.good, c, pc.ports(), "ports");
    n += checkCountPastPayload(f.good, c, pc.fields(), "fields");
    n += checkIndex(f.good, fieldAt(f.good, pc.ports(), 0, offsetof(IrPort, name)), f.strings(), "name");
    n += checkIndex(f.good, fieldAt(f.good, pc.ports(), 0, offsetof(IrPort, element)), f.elements(),
                    "element");
    n += checkIndex(f.good, fieldAt(f.good, pc.ports(), 0, offsetof(IrPort, scope)), f.elements(), "scope");
    size_t withFields = pc.ports().size();
    for (size_t i = 0; i < pc.ports().size(); ++i) {
        if (pc.ports()[i].fieldCount != 0) {
            withFields = i;
        }
    }
    REQUIRE(withFields < pc.ports().size());
    n += checkIndex(f.good, fieldAt(f.good, pc.ports(), withFields, offsetof(IrPort, fieldOffset)),
                    u32(pc.fields().size()), "fieldOffset");
    n += checkIndex(f.good, fieldAt(f.good, pc.fields(), 0, offsetof(IrPortField, name)), f.strings(),
                    "field name");
    MESSAGE("PORT cases: ", n);
}

TEST_CASE("malformed: T-255b BIND") {
    L8 f;
    REQUIRE(f.opened.ok());
    const BindingChunk& bc = f.view().bindings();
    std::span<const IrBinding> binds = bc.bindings();
    ChunkLoc c = findChunk(f.good, "BIND");
    int n = checkTruncated(f.good, "BIND");
    n += checkCountPastPayload(f.good, c, binds, "bindings");
    n += checkCountPastPayload(f.good, c, bc.sources(), "sources");
    n += checkCountPastPayload(f.good, c, bc.steps(), "steps");
    n += checkIndex(f.good, fieldAt(f.good, binds, 0, offsetof(IrBinding, element)), f.elements(), "element");
    n += checkIndex(f.good, fieldAt(f.good, binds, 0, offsetof(IrBinding, sourceOffset)),
                    u32(bc.sources().size()), "sourceOffset");
    n += checkIndex(f.good, fieldAt(f.good, binds, 0, offsetof(IrBinding, targetName)), f.strings(),
                    "targetName");
    n += checkIndex(f.good, spanOffset(f.good, bc.sources()), u32(f.view().ports().ports().size()),
                    "source port");
    size_t withSteps = binds.size();
    size_t sceneBinding = binds.size();
    for (size_t i = 0; i < binds.size(); ++i) {
        if (binds[i].stepCount != 0) {
            withSteps = i;
        }
        if (binds[i].sceneNode != kBindElementSelf) {
            sceneBinding = i;
        }
    }
    REQUIRE(withSteps < binds.size());
    REQUIRE(sceneBinding < binds.size());
    n += checkIndex(f.good, fieldAt(f.good, binds, withSteps, offsetof(IrBinding, stepOffset)),
                    u32(bc.steps().size()), "stepOffset");
    n += checkIndex(f.good, fieldAt(f.good, binds, sceneBinding, offsetof(IrBinding, sceneNode)), 0xFFFFFF00u,
                    "sceneNode");
    MESSAGE("BIND cases: ", n);
}

TEST_CASE("malformed: T-255b RPT") {
    L8 f;
    REQUIRE(f.opened.ok());
    std::span<const IrRepeat> rpt = f.view().repeats().repeats();
    ChunkLoc c = findChunk(f.good, "RPT ");
    int n = checkTruncated(f.good, "RPT ");
    n += checkCountPastPayload(f.good, c, rpt, "repeats");
    n += checkIndex(f.good, fieldAt(f.good, rpt, 0, offsetof(IrRepeat, element)), f.elements(), "element");
    n += checkIndex(f.good, fieldAt(f.good, rpt, 0, offsetof(IrRepeat, templateRoot)), f.elements(),
                    "templateRoot");
    n += checkIndex(f.good, fieldAt(f.good, rpt, 0, offsetof(IrRepeat, listPort)),
                    u32(f.view().ports().ports().size()), "listPort");
    n += checkIndex(f.good, fieldAt(f.good, rpt, 0, offsetof(IrRepeat, enterTriggerActions)),
                    u32(f.view().triggers().triggers().size()) + 1, "enterTriggerActions");
    n += checkIndex(f.good, fieldAt(f.good, rpt, 0, offsetof(IrRepeat, exitTriggerActions)),
                    u32(f.view().triggers().triggers().size()) + 1, "exitTriggerActions");
    n += checkIndex(f.good, fieldAt(f.good, rpt, 0, offsetof(IrRepeat, viewportElement)), f.elements(),
                    "viewportElement");
    MESSAGE("RPT cases: ", n);
}

TEST_CASE("malformed: T-255b CALC") {
    L8 f;
    REQUIRE(f.opened.ok());
    const CalcChunk& calc = f.view().calcs();
    ChunkLoc c = findChunk(f.good, "CALC");
    int n = checkTruncated(f.good, "CALC");
    n += checkCountPastPayload(f.good, c, calc.calcs(), "calcs");
    n += checkCountPastPayload(f.good, c, calc.ops(), "ops");
    n += checkIndex(f.good, fieldAt(f.good, calc.calcs(), 0, offsetof(IrCalc, opOffset)),
                    u32(calc.ops().size()), "opOffset");
    n += checkIndex(f.good, fieldAt(f.good, calc.calcs(), 0, offsetof(IrCalc, opCount)),
                    u32(calc.ops().size()) + 1, "opCount");
    const size_t decl = styleDeclIndex(f.view(), kDeclCalc);
    n += checkIndex(f.good, fieldAt(f.good, f.view().style().decls(), decl, offsetof(IrDecl, value)),
                    u32(calc.calcs().size()), "calc() program");
    MESSAGE("CALC cases: ", n);
}

TEST_CASE("malformed: T-255b MDIA") {
    L8 f;
    REQUIRE(f.opened.ok());
    const MediaChunk& m = f.view().media();
    ChunkLoc c = findChunk(f.good, "MDIA");
    int n = checkTruncated(f.good, "MDIA");
    n += checkCountPastPayload(f.good, c, m.media(), "media");
    n += checkCountPastPayload(f.good, c, m.queries(), "queries");
    n += checkCountPastPayload(f.good, c, m.conds(), "conds");
    n += checkCountPastPayload(f.good, c, m.rules(), "rules");
    n += checkIndex(f.good, fieldAt(f.good, m.media(), 0, offsetof(IrMedia, queryOffset)),
                    u32(m.queries().size()), "queryOffset");
    n += checkIndex(f.good, fieldAt(f.good, m.queries(), 0, offsetof(IrMediaQuery, condOffset)),
                    u32(m.conds().size()), "condOffset");
    n += checkIndex(f.good, fieldAt(f.good, m.rules(), 0, offsetof(IrMediaRule, rule)),
                    u32(f.view().style().rules().size()), "rule");
    n += checkIndex(f.good, fieldAt(f.good, m.rules(), 0, offsetof(IrMediaRule, media)),
                    u32(m.media().size()), "media");
    MESSAGE("MDIA cases: ", n);
}

TEST_CASE("malformed: T-255b SHDR") {
    L8 f;
    REQUIRE(f.opened.ok());
    const ShaderChunk& sh = f.view().shaders();
    const std::span<const uint8_t> text(reinterpret_cast<const uint8_t*>(sh.text().data()), sh.text().size());
    ChunkLoc c = findChunk(f.good, "SHDR");
    int n = checkTruncated(f.good, "SHDR");
    n += checkCountPastPayload(f.good, c, sh.shaders(), "shaders");
    n += checkCountPastPayload(f.good, c, sh.params(), "params");
    n += checkCountPastPayload(f.good, c, text, "text");
    const uint32_t textSize = u32(text.size());
    n += checkIndex(f.good, fieldAt(f.good, sh.shaders(), 0, offsetof(IrShaderRef, asset)),
                    u32(f.view().assets().assets().size()), "asset");
    n += checkIndex(f.good, fieldAt(f.good, sh.shaders(), 0, offsetof(IrShaderRef, sourceOffset)), textSize,
                    "sourceOffset");
    n += checkIndex(f.good, fieldAt(f.good, sh.shaders(), 0, offsetof(IrShaderRef, glslOffset)), textSize,
                    "glslOffset");
    n += checkIndex(f.good, fieldAt(f.good, sh.shaders(), 0, offsetof(IrShaderRef, paramOffset)),
                    u32(sh.params().size()), "paramOffset");
    n += checkIndex(f.good, fieldAt(f.good, sh.params(), 0, offsetof(IrShaderParam, name)), f.strings(),
                    "param name");
    MESSAGE("SHDR cases: ", n);
}

TEST_CASE("malformed: T-255b SCEN rig pools") {
    L8 f;
    REQUIRE(f.opened.ok());
    const IrView& view = f.view();
    uint32_t nth = view.sceneCount();
    for (uint32_t i = 0; i < view.sceneCount(); ++i) {
        if (!view.scene(i).bones().empty()) {
            nth = i;
        }
    }
    REQUIRE(nth < view.sceneCount());
    const SceneChunk& s = view.scene(nth);
    const uint32_t nodeCount = u32(s.nodes().size());
    ChunkLoc c = findChunk(f.good, "SCEN", nth);
    int n = checkTruncated(f.good, "SCEN", nth);
    n += checkCountPastPayload(f.good, c, s.bones(), "bones");
    n += checkCountPastPayload(f.good, c, s.constraints(), "constraints");
    n += checkCountPastPayload(f.good, c, s.meshes(), "meshes");
    n += checkCountPastPayload(f.good, c, s.skins(), "skins");
    n += checkCountPastPayload(f.good, c, s.tendonPool(), "tendons");
    n += checkCountPastPayload(f.good, c, s.weightPool(), "weights");
    n += checkCountPastPayload(f.good, c, s.meshVec2Pool(), "mesh pins");
    n += checkCountPastPayload(f.good, c, s.meshIndexPool(), "mesh tris");
    // Node kindIndex into each rig record array.
    for (size_t i = 0; i < s.nodes().size(); ++i) {
        const NodeKind kind = static_cast<NodeKind>(s.nodes()[i].kind);
        const size_t at = fieldAt(f.good, s.nodes(), i, offsetof(IrSceneNode, kindIndex));
        if (kind == NodeKind::Bone) {
            n += checkIndex(f.good, at, u32(s.bones().size()), "Bone kindIndex");
        } else if (kind == NodeKind::Constraint) {
            n += checkIndex(f.good, at, u32(s.constraints().size()), "Constraint kindIndex");
        } else if (kind == NodeKind::Mesh) {
            n += checkIndex(f.good, at, u32(s.meshes().size()), "Mesh kindIndex");
        }
    }
    n += checkIndex(f.good, fieldAt(f.good, s.constraints(), 0, offsetof(IrConstraintData, target)),
                    nodeCount, "constraint target");
    n += checkIndex(f.good, fieldAt(f.good, s.skins(), 0, offsetof(IrSkinData, skeleton)), nodeCount,
                    "skeleton");
    n += checkIndex(f.good, fieldAt(f.good, s.skins(), 0, offsetof(IrSkinData, tendonOffset)),
                    u32(s.tendonPool().size()), "tendonOffset");
    n += checkIndex(f.good, fieldAt(f.good, s.skins(), 0, offsetof(IrSkinData, weightOffset)),
                    u32(s.weightPool().size()), "weightOffset");
    n += checkIndex(f.good, fieldAt(f.good, s.tendonPool(), 0, offsetof(IrTendon, bone)), nodeCount,
                    "tendon bone");
    n += checkIndex(f.good, fieldAt(f.good, s.weightPool(), 0, offsetof(IrPointWeight, idx4)), 0xFFFFFFFFu,
                    "weight tendon index");
    const IrMeshData& mesh = s.meshes()[0];
    n += checkIndex(f.good, fieldAt(f.good, s.meshes(), 0, offsetof(IrMeshData, asset)),
                    u32(view.assets().assets().size()), "mesh asset");
    n += checkIndex(f.good, fieldAt(f.good, s.meshes(), 0, offsetof(IrMeshData, posOffset)),
                    u32(s.meshVec2Pool().size()), "mesh pins offset");
    n += checkIndex(f.good, fieldAt(f.good, s.meshes(), 0, offsetof(IrMeshData, indexOffset)),
                    u32(s.meshIndexPool().size()), "mesh tris offset");
    n += checkIndex(f.good, fieldAt(f.good, s.meshes(), 0, offsetof(IrMeshData, skinIndex)),
                    u32(s.skins().size()), "mesh skin");
    n += checkIndex16(f.good, fieldAt(f.good, s.meshIndexPool(), mesh.indexOffset, 0),
                      static_cast<uint16_t>(mesh.vertexCount), "triangle vertex");
    REQUIRE(!s.paths().empty());
    n += checkIndex(f.good, fieldAt(f.good, s.paths(), 0, offsetof(IrPathData, skinIndex)),
                    u32(s.skins().size()), "path skin");
    MESSAGE("SCEN rig cases: ", n);
}
