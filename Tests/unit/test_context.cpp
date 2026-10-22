#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "test_temp.h"
#include "xt/command_list.h"
#include "xt/compile/ir_writer.h"
#include "xt/compile/utf8_path.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/property.h"
#include "xt/scene_instance.h"
#include "xt/types.h"

/// Context/Document tests: full load -> update -> render pipeline. See Docs/tasks/T-18.md
/// "測試（test_context.cpp）".

using namespace xt;
using namespace xt::compile;

namespace {

struct Built {
    std::vector<uint8_t> bytes;
    SceneId sceneId = kInvalidScene;
    TimelineId timelineId = kInvalidTimeline;
};

/// Builds one IR: empty STYL; one scene (root Group -> Path, 100x100 rectangle, design size
/// 200x200); one timeline (Opacity track on scene-local index 1, 0 -> 1 over 1s, marker "mid" at
/// 0.5); DOCT: xt -> body -> [anim (scene 0, loop="true"), div (text "Hi")].
Built buildIr() {
    Built out;
    IrWriter w;

    StringId xtTag = w.intern("xt");
    StringId bodyTag = w.intern("body");
    StringId animTag = w.intern("anim");
    StringId divTag = w.intern("div");
    StringId loopAttrName = w.intern("loop");
    StringId trueValue = w.intern("true");
    StringId hiText = w.intern("Hi");
    StringId markerName = w.intern("mid");

    SceneBuilder scene;
    scene.width = 200;
    scene.height = 200;

    IrSceneNode root{};
    root.kind = static_cast<uint8_t>(NodeKind::Group);
    root.visible = 1;
    root.parent = 0xFFFFFFFFu;
    root.opacity = 1;
    root.scale = Vec2{1, 1};
    scene.nodes.push_back(root);

    scene.verbPool = {static_cast<uint8_t>(PathVerb::Move), static_cast<uint8_t>(PathVerb::Line),
                      static_cast<uint8_t>(PathVerb::Line), static_cast<uint8_t>(PathVerb::Line),
                      static_cast<uint8_t>(PathVerb::Close)};
    scene.floatPool = {0, 0, 100, 0, 100, 100, 0, 100};

    IrPathData path{};
    path.verbOffset = 0;
    path.verbCount = 5;
    path.pointOffset = 0;
    path.pointCount = 8;
    path.fillRule = static_cast<uint8_t>(FillRule::NonZero);
    path.fillKind = static_cast<uint8_t>(PaintKind::Solid);
    path.strokeKind = static_cast<uint8_t>(PaintKind::None);
    path.cap = static_cast<uint8_t>(LineCap::Butt);
    path.join = static_cast<uint8_t>(LineJoin::Miter);
    path.fillColor = Color{1, 1, 1, 1};
    path.miterLimit = 4.0f;
    path.trimEnd = 1.0f;
    scene.paths.push_back(path);

    IrSceneNode pathNode{};
    pathNode.kind = static_cast<uint8_t>(NodeKind::Path);
    pathNode.visible = 1;
    pathNode.parent = 0;
    pathNode.kindIndex = 0;
    pathNode.opacity = 1;
    pathNode.scale = Vec2{1, 1};
    scene.nodes.push_back(pathNode);

    out.sceneId = w.addScene(scene);

    TimelineBuilder tl;
    tl.duration = 1.0f;
    tl.frameRate = 30.0f;

    IrTrack track{};
    track.target = 1;  // Path is scene-local index 1.
    track.prop = prop::Opacity;
    track.type = static_cast<uint8_t>(ValueType::Float);
    track.kfOffset = 0;
    track.kfCount = 2;
    tl.tracks.push_back(track);

    IrKeyframe k0{};
    k0.time = 0.0f;
    k0.easing = IrEasing{static_cast<uint8_t>(EasingKind::Linear), 0, 0, 0, 0, 0, 0};
    irSetValueF32(k0.value, 0, 0.0f);
    tl.keyframes.push_back(k0);

    IrKeyframe k1{};
    k1.time = 1.0f;
    k1.easing = IrEasing{static_cast<uint8_t>(EasingKind::Linear), 0, 0, 0, 0, 0, 0};
    irSetValueF32(k1.value, 0, 1.0f);
    tl.keyframes.push_back(k1);

    IrMarker marker{};
    marker.name = markerName;
    marker.start = 0.5f;
    marker.duration = 0.0f;
    tl.markers.push_back(marker);

    out.timelineId = w.addTimeline(tl);

    DocBuilder doc;

    IrDocElement xtEl{};
    xtEl.parent = 0xFFFFFFFFu;
    xtEl.tag = xtTag;
    xtEl.id = kInvalidString;
    xtEl.scene = kInvalidScene;
    xtEl.firstTextChild = 0xFFFFFFFFu;
    xtEl.cameraIndex = 0xFFFFFFFFu;
    doc.elements.push_back(xtEl);  // index 0

    IrDocElement bodyEl{};
    bodyEl.parent = 0;
    bodyEl.tag = bodyTag;
    bodyEl.id = kInvalidString;
    bodyEl.scene = kInvalidScene;
    bodyEl.firstTextChild = 0xFFFFFFFFu;
    bodyEl.cameraIndex = 0xFFFFFFFFu;
    doc.elements.push_back(bodyEl);  // index 1

    IrAttr loopAttr{};
    loopAttr.name = loopAttrName;
    loopAttr.value = trueValue;
    doc.attrPool.push_back(loopAttr);

    IrDocElement animEl{};
    animEl.parent = 1;
    animEl.tag = animTag;
    animEl.id = kInvalidString;
    animEl.attrOffset = 0;
    animEl.attrCount = 1;
    animEl.scene = out.sceneId;
    animEl.firstTextChild = 0xFFFFFFFFu;
    animEl.cameraIndex = 0xFFFFFFFFu;
    doc.elements.push_back(animEl);  // index 2

    IrDocElement divEl{};
    divEl.parent = 1;
    divEl.tag = divTag;
    divEl.id = kInvalidString;
    divEl.scene = kInvalidScene;
    divEl.firstTextChild = 0;
    divEl.cameraIndex = 0xFFFFFFFFu;
    doc.elements.push_back(divEl);  // index 3

    IrDocText text{};
    text.parent = 3;
    text.text = hiText;
    doc.texts.push_back(text);

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

TEST_CASE("Document: load, root/byId/elementAt, text child") {
    Built built = buildIr();
    Context ctx(ContextConfig{});
    Document* doc = ctx.document(loadOk(ctx, built));
    REQUIRE(doc != nullptr);

    CHECK(doc->root().valid());
    CHECK(!doc->byId("missing").valid());

    ElementRef anim = doc->elementAt(2);
    REQUIRE(anim.valid());
    CHECK(doc->string(doc->arena().element(anim.node).tag) == "anim");

    ElementRef div = doc->elementAt(3);
    REQUIRE(div.valid());
    NodeId textChild = doc->arena().firstChild(div.node);
    REQUIRE(textChild != kInvalidNode);
    CHECK(doc->arena().kind(textChild) == NodeKind::Text);
    CHECK(doc->string(doc->arena().text(textChild).text) == "Hi");
}

TEST_CASE("Document: anim element autoplay creates a playing instance") {
    Built built = buildIr();
    Context ctx(ContextConfig{});
    Document* doc = ctx.document(loadOk(ctx, built));
    ElementRef anim = doc->elementAt(2);

    const SceneInstance* inst = doc->sceneInstance(anim);
    REQUIRE(inst != nullptr);
    CHECK(inst->localToArena.size() == 2);

    uint32_t id = doc->defaultAnimation(anim);
    CHECK(id != 0);
    CHECK(doc->isPlaying(id));
}

// T-25: object-fit is now computed by LayoutEngine (Docs/design/06-layout.md §6.10), from the
// anim element's own content box -- not directly from the viewport (that was T-18's interim
// behavior, removed by T-25). This fixture's `anim` carries no CSS (buildIr() writes an empty
// STYL), so its box is set explicitly here via setProperty and ctx.update() drives layout.
TEST_CASE("Document: layout applies object-fit: contain to the anim scene root") {
    Built built = buildIr();
    Context ctx(ContextConfig{});
    Document* doc = ctx.document(loadOk(ctx, built));
    ElementRef anim = doc->elementAt(2);
    NodeId sceneRoot = doc->arena().element(anim.node).sceneRoot;

    // The very first update() resolves every element's initial (kDirtyStyle-from-load) style,
    // which would otherwise clobber the direct setProperty() writes below (StyleEngine::resolve
    // commits ComputedStyle wholesale, width/height default to auto). Get that out of the way first.
    ctx.update(0.0f);

    setProperty(doc->arena(), anim.node, prop::Width, Value::ofLength(400, LengthUnit::Px));
    setProperty(doc->arena(), anim.node, prop::Height, Value::ofLength(400, LengthUnit::Px));
    ctx.setViewport(Rect{0, 0, 400, 400}, 1.0f);
    ctx.update(0.0f);
    const NodeCommon& c1 = doc->arena().common(sceneRoot);
    CHECK(c1.scale.x == doctest::Approx(2.0f));
    CHECK(c1.scale.y == doctest::Approx(2.0f));
    CHECK(c1.translate.x == doctest::Approx(0.0f));
    CHECK(c1.translate.y == doctest::Approx(0.0f));

    setProperty(doc->arena(), anim.node, prop::Height, Value::ofLength(200, LengthUnit::Px));
    ctx.update(0.0f);
    const NodeCommon& c2 = doc->arena().common(sceneRoot);
    CHECK(c2.scale.x == doctest::Approx(1.0f));
    CHECK(c2.scale.y == doctest::Approx(1.0f));
    CHECK(c2.translate.x == doctest::Approx(100.0f));
    CHECK(c2.translate.y == doctest::Approx(0.0f));
}

TEST_CASE("Document: update evaluates the Animator and render emits the transformed FillPath") {
    Built built = buildIr();
    Context ctx(ContextConfig{});
    Document* doc = ctx.document(loadOk(ctx, built));
    ElementRef anim = doc->elementAt(2);
    NodeId sceneRoot = doc->arena().element(anim.node).sceneRoot;
    NodeId pathNode = doc->sceneInstance(anim)->localToArena[1];

    ctx.setViewport(Rect{0, 0, 400, 200}, 1.0f);
    ctx.update(0.5f);
    CHECK(doc->arena().common(pathNode).opacity == doctest::Approx(0.5f));

    CommandList cmds;
    ctx.render(cmds);
    std::string dump;
    dumpCommandList(cmds, dump);

    Mat2x3 sceneRootWorld = doc->arena().common(sceneRoot).world;
    Mat2x3 pathWorld = doc->arena().common(pathNode).world;
    // Path's own local transform is identity, so its world equals sceneRoot's world x identity.
    CHECK(approxEqual(pathWorld, mul(sceneRootWorld, Mat2x3{})));

    char xfBuf[96];
    std::snprintf(xfBuf, sizeof(xfBuf), "xf=[%.3f %.3f %.3f %.3f %.3f %.3f]", pathWorld.a, pathWorld.b,
                  pathWorld.c, pathWorld.d, pathWorld.e, pathWorld.f);
    std::string expected = std::string("FillPath ") + xfBuf +
                           " verbs=5 points=8 rule=NonZero paint=Solid(1.000,1.000,1.000,0.500)\n";
    CHECK(dump == expected);
}

TEST_CASE("Document: Loop wrap, pause/resume, seek and setSpeed") {
    Built built = buildIr();
    Context ctx(ContextConfig{});
    Document* doc = ctx.document(loadOk(ctx, built));
    ElementRef anim = doc->elementAt(2);
    NodeId pathNode = doc->sceneInstance(anim)->localToArena[1];
    uint32_t id = doc->defaultAnimation(anim);

    ctx.update(0.5f);  // clock 0.5
    CHECK(doc->arena().common(pathNode).opacity == doctest::Approx(0.5f));

    ctx.update(1.0f);  // clock 1.5; Loop wraps localT back to 0.5.
    CHECK(doc->arena().common(pathNode).opacity == doctest::Approx(0.5f));

    doc->pause(id);
    ctx.update(1.0f);  // clock 2.5, but paused -> unchanged.
    CHECK(doc->arena().common(pathNode).opacity == doctest::Approx(0.5f));

    doc->resume(id);
    doc->seek(id, 0.0f);
    ctx.update(0.0f);  // Re-evaluate at the same clock right after seek(0).
    CHECK(doc->arena().common(pathNode).opacity == doctest::Approx(0.0f));

    doc->setSpeed(id, 2.0f);
    ctx.update(0.25f);  // +0.25s wall clock at speed 2 -> localT advances by 0.5 from the seek point.
    CHECK(doc->arena().common(pathNode).opacity == doctest::Approx(0.5f));
}

TEST_CASE("Document: onAnimationFinished fires once after a Once animation completes") {
    Built built = buildIr();
    Context ctx(ContextConfig{});
    Document* doc = ctx.document(loadOk(ctx, built));
    ElementRef anim = doc->elementAt(2);
    const SceneInstance* inst = doc->sceneInstance(anim);
    REQUIRE(inst != nullptr);

    PlayParams params;
    params.timeline = doc->sceneTimeline(inst->scene);
    params.localToArena = inst->localToArena.data();
    params.localCount = static_cast<uint32_t>(inst->localToArena.size());
    params.selfNode = anim.node;
    params.loop = LoopMode::Once;
    // Animator::evaluate() only fires onFinished for instances it removes from the pool, i.e. a
    // finished instance whose fill does not include Forwards (Docs/tasks/T-07.md step 6).
    params.fill = FillMode::None;

    uint32_t id = doc->play(params);
    int finishedCount = 0;
    doc->onAnimationFinished(id, [&]() { ++finishedCount; });

    ctx.update(1.5f);  // Past the 1s duration.
    CHECK(finishedCount == 1);

    ctx.update(0.1f);
    CHECK(finishedCount == 1);
}

TEST_CASE("Context::render is idempotent and CommandList::clear() keeps capacity") {
    Built built = buildIr();
    Context ctx(ContextConfig{});
    loadOk(ctx, built);
    ctx.setViewport(Rect{0, 0, 400, 400}, 1.0f);
    ctx.update(0.2f);

    CommandList first;
    ctx.render(first);
    std::string firstDump;
    dumpCommandList(first, firstDump);

    CommandList second;
    ctx.render(second);
    std::string secondDump;
    dumpCommandList(second, secondDump);
    CHECK(firstDump == secondDump);

    uint32_t commandsCap = static_cast<uint32_t>(first.commands.capacity());
    first.clear();
    CHECK(first.commands.empty());
    CHECK(first.commands.capacity() == commandsCap);

    ctx.render(first);
    std::string thirdDump;
    dumpCommandList(first, thirdDump);
    CHECK(thirdDump == firstDump);
}

TEST_CASE("Context: unloadDocument invalidates the handle; handles are never reused") {
    Built built = buildIr();
    Context ctx(ContextConfig{});
    DocumentHandle h1 = loadOk(ctx, built);

    ctx.unloadDocument(h1);
    CHECK(ctx.document(h1) == nullptr);

    DocumentHandle h2 = loadOk(ctx, built);
    CHECK(h2 != h1);
    CHECK(ctx.document(h2) != nullptr);
}

TEST_CASE("Context::loadDocumentFile") {
    Context ctx(ContextConfig{});

    SUBCASE("nonexistent path returns kIoError") {
        Result<DocumentHandle> r = ctx.loadDocumentFile("this/path/does/not/exist.xtir");
        CHECK(!r.ok());
        CHECK(r.status() == Status::kIoError);
    }

    SUBCASE("a file written by IrWriter::finish() loads successfully") {
        Built built = buildIr();
        std::string path = "t18_loaddocumentfile_test.xtir";
        {
            std::ofstream out(path, std::ios::binary);
            REQUIRE(out.good());
            out.write(reinterpret_cast<const char*>(built.bytes.data()),
                      static_cast<std::streamsize>(built.bytes.size()));
        }
        Result<DocumentHandle> r = ctx.loadDocumentFile(path);
        CHECK(r.ok());
        std::remove(path.c_str());
    }

    SUBCASE("T-261e: a UTF-8 path whose name the ANSI code page cannot spell loads") {
        Built built = buildIr();
        // t261e_アイコン.xtir in the temp directory, made at run time.
        const std::filesystem::path file =
            xt::test::tempDir() / pathFromUtf8("t261e_\xE3\x82\xA2\xE3\x82\xA4\xE3\x82\xB3\xE3\x83\xB3.xtir");
        {
            std::ofstream out(file, std::ios::binary);
            REQUIRE(out.good());
            out.write(reinterpret_cast<const char*>(built.bytes.data()),
                      static_cast<std::streamsize>(built.bytes.size()));
        }
        Result<DocumentHandle> r = ctx.loadDocumentFile(pathToUtf8(file));
        CHECK(r.ok());
        std::error_code ec;
        std::filesystem::remove(file, ec);
    }
}

TEST_CASE("Context::loadDocument rejects truncated IR") {
    Built built = buildIr();
    std::vector<uint8_t> truncated(built.bytes.begin(), built.bytes.end() - 8);
    Context ctx(ContextConfig{});
    Result<DocumentHandle> r = ctx.loadDocument(IrBlob{truncated.data(), truncated.size()}, false);
    CHECK(!r.ok());
    CHECK(r.status() == Status::kInvalidIr);
}
