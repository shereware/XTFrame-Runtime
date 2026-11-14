#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "doctest.h"
#include "fake_text_shaper.h"
#include "style_decode.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/element_ref.h"
#include "xt/event.h"
#include "xt/input.h"
#include "xt/node.h"
#include "xt/world_layer_output.h"

/// T-701 (Docs/design/33-world-layers.md §33.2-§33.6, Docs/tasks/T-701.md): the `world-layer`
/// switch (StyleProp 281), the Painter's world-layer output (one command range per layer, the
/// layer plane's transform to the root box, the rectangle on the step grid, the opacity left to
/// the host) and the ray hit test.

using namespace xt;

namespace {

constexpr uint16_t kWorldLayerProp = style_decode::styleprop::WorldLayer;

compile::CompileResult compileRaw(const std::string& css, const std::string& body) {
    const std::string text = "<xt><head><style>" + css + "</style></head><body>" + body + "</body></xt>";
    const compile::SourceFile file{"world_layers.xt", text};
    return compile::compileDocumentFromStrings({&file, 1}, "world_layers.xt", compile::CompileOptions{});
}

struct Loaded {
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    Document* doc = nullptr;
    std::vector<uint8_t> ir;

    Loaded(const std::string& css, const std::string& body, bool layers = true) {
        compile::CompileResult result = compileRaw(css, body);
        for (const compile::Diagnostic& d : result.diagnostics) {
            CAPTURE(d.message);
            CHECK(d.level != compile::Diagnostic::Level::Error);
        }
        REQUIRE(result.ok());
        ir = std::move(result.ir);
        Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{ir.data(), ir.size()}, false);
        REQUIRE(handle.ok());
        doc = ctx.document(handle.value());
        REQUIRE(doc != nullptr);
        ctx.setViewport(Rect{0, 0, 400, 300}, 1.0f);
        ctx.setWorldLayerOutput(layers);
        ctx.update(0.0f);
    }

    NodeId node(const char* id) {
        ElementRef ref = doc->byId(id);
        REQUIRE(ref.valid());
        return ref.node;
    }
    CommandList render() {
        CommandList list;
        ctx.render(list);
        return list;
    }
};

/// The dump of `list` without its trailing WorldLayer lines.
std::string commandDump(const CommandList& list) {
    std::string text;
    dumpCommandList(list, text);
    std::string out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        end = end == std::string::npos ? text.size() : end + 1;
        if (text.compare(pos, 10, "WorldLayer") != 0) {
            out.append(text, pos, end - pos);
        }
        pos = end;
    }
    return out;
}

/// The dump of the commands of layer `index` alone.
std::string rangeDump(const CommandList& list, size_t index) {
    CommandList part = list;
    const WorldLayerRange& range = list.worldLayers[index];
    part.commands.assign(list.commands.begin() + range.firstCommand,
                         list.commands.begin() + range.firstCommand + range.commandCount);
    part.worldLayers.clear();
    std::string text;
    dumpCommandList(part, text);
    return text;
}

const WorldLayerRange* layerOf(const CommandList& list, NodeId node) {
    for (const WorldLayerRange& range : list.worldLayers) {
        if (range.id == node) {
            return &range;
        }
    }
    return nullptr;
}

void checkMat4(const Mat4& a, const Mat4& b) {
    for (int i = 0; i < 16; ++i) {
        CAPTURE(i);
        CHECK(a.m[i] == doctest::Approx(b.m[i]).epsilon(1e-4).scale(1.0));
    }
}

const char* kBaseCss =
    "#root { position: absolute; left: 0px; top: 0px; width: 400px; height: 300px; "
    "background-color: #102030; }"
    "#flat { position: absolute; left: 20px; top: 30px; width: 100px; height: 60px; "
    "background-color: #ff0000; }"
    "#card { position: absolute; left: 100px; top: 50px; width: 200px; height: 100px; "
    "background-color: #00ff00; }";
const char* kBaseBody = "<div id=\"root\"><div id=\"flat\"></div><div id=\"card\"></div></div>";

}  // namespace

TEST_CASE("T-701: world-layer parses off / on, is not inherited and can be set at run time") {
    Loaded l(std::string(kBaseCss) + "#card { world-layer: on; }", kBaseBody, /*layers=*/false);
    CHECK(l.doc->byId("card").computedStyleValue(kWorldLayerProp).e == 1u);
    CHECK(l.doc->byId("flat").computedStyleValue(kWorldLayerProp).e == 0u);
    CHECK(l.doc->byId("root").computedStyleValue(kWorldLayerProp).e == 0u);
    CHECK(l.doc->arena().common(l.node("card")).worldLayer == 1);

    CHECK(l.doc->byId("card").setStyle("world-layer: off") == Status::kOk);
    CHECK(l.doc->byId("flat").setStyle("world-layer: on") == Status::kOk);
    l.ctx.update(0.016f);
    CHECK(l.doc->byId("card").computedStyleValue(kWorldLayerProp).e == 0u);
    CHECK(l.doc->byId("flat").computedStyleValue(kWorldLayerProp).e == 1u);
    CHECK(l.doc->byId("flat").setStyle("world-layer: maybe") != Status::kOk);
}

TEST_CASE("T-701: outside world-layer output the switch changes nothing") {
    Loaded with(std::string(kBaseCss) + "#card { world-layer: on; transform: translateZ(40px); }", kBaseBody,
                /*layers=*/false);
    Loaded without(std::string(kBaseCss) + "#card { transform: translateZ(40px); }", kBaseBody,
                   /*layers=*/false);
    const CommandList a = with.render();
    const CommandList b = without.render();
    CHECK(a.worldLayers.empty());
    std::string da, db;
    dumpCommandList(a, da);
    dumpCommandList(b, db);
    CHECK(da == db);
    CHECK(with.doc->arena().common(with.node("card")).worldLayerActive == 0);
    CHECK(with.doc->hitTest(Vec2{200, 100}) == with.node("card"));
}

TEST_CASE("T-701: a UI without any switch is one base layer with the ordinary commands") {
    Loaded on(kBaseCss, kBaseBody, /*layers=*/true);
    Loaded off(kBaseCss, kBaseBody, /*layers=*/false);
    const CommandList a = on.render();
    const CommandList b = off.render();
    REQUIRE(a.worldLayers.size() == 1);
    const WorldLayerRange& base = a.worldLayers[0];
    CHECK(base.id == 0u);
    CHECK(base.order == 0u);
    CHECK(base.firstCommand == 0u);
    CHECK(base.commandCount == a.commands.size());
    CHECK(base.rect.w == 400.0f);
    CHECK(base.rect.h == 300.0f);
    CHECK(base.opacity == 1.0f);
    checkMat4(base.world3, mat4Identity());
    CHECK(commandDump(a) == commandDump(b));
    CHECK(validateCommandList(a));
}

TEST_CASE("T-701: a tilted layer's world3 is the 19.2 local3 chain, without any perspective") {
    // #stage has `perspective` (ignored in world-layer output) and a 2D offset; #card is rotated
    // about its centre and pushed toward the viewer.
    Loaded l(std::string(kBaseCss) +
                 "#stage { position: absolute; left: 10px; top: 20px; width: 380px; height: 260px; "
                 "perspective: 500px; }"
                 "#card { world-layer: on; transform: translateZ(40px) rotateY(30deg); "
                 "backface-visibility: hidden; opacity: 0.5; }",
             "<div id=\"root\"><div id=\"stage\"><div id=\"card\"></div></div></div>");
    const CommandList list = l.render();
    REQUIRE(list.worldLayers.size() == 2);
    CHECK(validateCommandList(list));
    const WorldLayerRange* card = layerOf(list, l.node("card"));
    REQUIRE(card != nullptr);
    CHECK(card->order == 1u);
    CHECK(card->backfaceHidden == 1);
    CHECK(card->opacity == doctest::Approx(0.5f));

    // 19 §19.2: local3 = T(translate) * Ry * T(-anchor), the element's box at (100, 50) in #stage,
    // itself at (10, 20); transform-origin is the box centre (100, 50).
    const Mat4 expected = mat4Mul(mat4Translate(Vec3{10, 20, 0}),
                                  mat4Mul(mat4Translate(Vec3{100 + 100, 50 + 50, 40}),
                                          mat4Mul(mat4RotateY(30.0f), mat4Translate(Vec3{-100, -50, 0}))));
    checkMat4(card->world3, expected);
    // The centre stays put in x / y and sits 40 px toward the viewer.
    const Vec3 centre = mat4Transform(card->world3, Vec3{100, 50, 0});
    CHECK(centre.x == doctest::Approx(210.0f));
    CHECK(centre.y == doctest::Approx(120.0f));
    CHECK(centre.z == doctest::Approx(40.0f));

    // The rectangle: the 200 x 100 box (plus 1 px of slack) out to the 32 px grid, in the layer's
    // own coordinates; the content is drawn unrotated and at full opacity (no PushLayer).
    CHECK(card->rect.x == -32.0f);
    CHECK(card->rect.y == -32.0f);
    CHECK(card->rect.w == 256.0f);
    CHECK(card->rect.h == 160.0f);
    const std::string dump = rangeDump(list, 1);
    CHECK(dump.find("PushLayer") == std::string::npos);
    CHECK(dump.find("PushProjection") == 0);
}

TEST_CASE("T-701: moving or fading a layer changes its world3 / opacity, not its commands") {
    Loaded l(std::string(kBaseCss) + "#card { world-layer: on; }", kBaseBody);
    const CommandList before = l.render();
    REQUIRE(before.worldLayers.size() == 2);
    CHECK(l.doc->byId("card").setStyle(
              "transform: translate(30px, -10px) translateZ(80px) rotateX(20deg); opacity: 0.25") ==
          Status::kOk);
    l.ctx.update(0.016f);
    const CommandList after = l.render();
    REQUIRE(after.worldLayers.size() == 2);
    CHECK(rangeDump(before, 1) == rangeDump(after, 1));
    CHECK(rangeDump(before, 0) == rangeDump(after, 0));
    CHECK(after.worldLayers[1].opacity == doctest::Approx(0.25f));
    const Vec3 a = mat4Transform(before.worldLayers[1].world3, Vec3{100, 50, 0});
    const Vec3 b = mat4Transform(after.worldLayers[1].world3, Vec3{100, 50, 0});
    CHECK(a.z == doctest::Approx(0.0f));
    CHECK(b.x == doctest::Approx(a.x + 30.0f));
    CHECK(b.y == doctest::Approx(a.y - 10.0f));
    CHECK(b.z == doctest::Approx(80.0f));
}

TEST_CASE("T-701: nested layers follow their parent in paint order; ancestors' opacity multiplies") {
    Loaded l(std::string(kBaseCss) +
                 "#flat { world-layer: on; opacity: 0.5; }"
                 "#inner { position: absolute; left: 5px; top: 5px; width: 20px; height: 20px; "
                 "background-color: #ffffff; world-layer: on; opacity: 0.5; transform: translateZ(10px); }"
                 "#card { world-layer: on; }",
             "<div id=\"root\"><div id=\"flat\"><div id=\"inner\"></div></div><div id=\"card\"></div></div>");
    const CommandList list = l.render();
    REQUIRE(list.worldLayers.size() == 4);
    CHECK(validateCommandList(list));
    CHECK(list.worldLayers[0].id == 0u);
    CHECK(list.worldLayers[1].id == l.node("flat"));
    CHECK(list.worldLayers[2].id == l.node("inner"));
    CHECK(list.worldLayers[3].id == l.node("card"));
    for (uint32_t i = 0; i < 4; ++i) {
        CHECK(list.worldLayers[i].order == i);
    }
    CHECK(list.worldLayers[2].opacity == doctest::Approx(0.25f));
    // #inner's plane: #flat's box corner (20, 30) + its own (5, 5), 10 px toward the viewer.
    const Vec3 corner = mat4Transform(list.worldLayers[2].world3, Vec3{0, 0, 0});
    CHECK(corner.x == doctest::Approx(25.0f));
    CHECK(corner.y == doctest::Approx(35.0f));
    CHECK(corner.z == doctest::Approx(10.0f));
    // The ranges are contiguous and cover the list.
    uint32_t next = 0;
    for (const WorldLayerRange& range : list.worldLayers) {
        CHECK(range.firstCommand == next);
        next = range.firstCommand + range.commandCount;
    }
    CHECK(next == list.commands.size());
}

TEST_CASE("T-701: a ray hits the tilted layer at the right local point; 2D input is a ray down -z") {
    Loaded l(std::string(kBaseCss) + "#card { world-layer: on; transform: translateZ(40px) rotateY(30deg); }",
             kBaseBody);
    const CommandList list = l.render();
    const WorldLayerRange* card = layerOf(list, l.node("card"));
    REQUIRE(card != nullptr);

    // A point of the card's own box, in root-box coordinates.
    const Vec2 local{50, 25};
    const Vec3 world = mat4Transform(card->world3, Vec3{local.x, local.y, 0});
    // An oblique ray through it.
    const Vec3 dir{-0.3f, 0.1f, -1.0f};
    const Vec3 origin{world.x - dir.x * 300.0f, world.y - dir.y * 300.0f, world.z - dir.z * 300.0f};
    CHECK(worldLayerHitTest(*l.doc, origin, dir) == l.node("card"));

    Vec2 seen{-1, -1};
    NodeId target = kInvalidNode;
    l.doc->addListener(l.doc->byId("card"), EventType::PointerDown, [&](Event& e) {
        seen = e.localPos;
        target = e.target;
    });
    InputEvent down;
    down.kind = InputEvent::PointerDown;
    l.ctx.pushPointerRay(down, origin, dir);
    l.ctx.update(0.016f);
    CHECK(target == l.node("card"));
    CHECK(seen.x == doctest::Approx(local.x).epsilon(1e-3));
    CHECK(seen.y == doctest::Approx(local.y).epsilon(1e-3));

    // A ray that passes beside the card reaches the base layer behind it.
    CHECK(worldLayerHitTest(*l.doc, Vec3{30, 40, 500}, Vec3{0, 0, -1}) == l.node("flat"));
    // The 2D entry point: straight down -z. (200, 100) is the card's centre, which the rotation
    // leaves in place.
    CHECK(l.doc->hitTest(Vec2{200, 100}) == l.node("card"));
    CHECK(l.doc->hitTest(Vec2{30, 40}) == l.node("flat"));
    // A ray starting behind every plane hits nothing.
    CHECK(worldLayerHitTest(*l.doc, Vec3{200, 100, -50}, Vec3{0, 0, -1}) == kInvalidNode);
}

TEST_CASE("T-701: backface-hidden layers are not hit from behind; coplanar layers: last painted wins") {
    Loaded l(std::string(kBaseCss) +
                 "#flat { world-layer: on; left: 100px; top: 50px; width: 200px; height: 100px; }"
                 "#card { world-layer: on; backface-visibility: hidden; }",
             kBaseBody);
    l.render();
    // Same plane (z = 0), same box: #card is painted last.
    CHECK(worldLayerHitTest(*l.doc, Vec3{200, 100, 300}, Vec3{0, 0, -1}) == l.node("card"));
    // From behind, #card (backface hidden) lets the ray through to #flat.
    CHECK(worldLayerHitTest(*l.doc, Vec3{200, 100, -300}, Vec3{0, 0, 1}) == l.node("flat"));
    // A nearer plane wins over paint order. (Written with an explicit translate(): a run-time
    // setStyle of `transform: translateZ(..)` alone is not applied -- found here, not part of T-701.)
    CHECK(l.doc->byId("flat").setStyle("transform: translate(0px, 0px) translateZ(20px)") == Status::kOk);
    l.ctx.update(0.016f);
    const CommandList moved = l.render();
    REQUIRE(layerOf(moved, l.node("flat")) != nullptr);
    CHECK(mat4Transform(layerOf(moved, l.node("flat"))->world3, Vec3{0, 0, 0}).z == doctest::Approx(20.0f));
    CHECK(worldLayerHitTest(*l.doc, Vec3{200, 100, 300}, Vec3{0, 0, -1}) == l.node("flat"));
}

TEST_CASE("T-701: 3D descendants of a layer are flattened inside it; a nested space keeps its camera") {
    Loaded l(std::string(kBaseCss) +
                 "#card { world-layer: on; }"
                 "#tilt { position: absolute; left: 10px; top: 10px; width: 50px; height: 50px; "
                 "background-color: #0000ff; transform: rotateY(60deg); }",
             "<div id=\"root\"><div id=\"card\"><div id=\"tilt\"></div></div></div>");
    const CommandList list = l.render();
    REQUIRE(list.worldLayers.size() == 2);
    CHECK(validateCommandList(list));
    // The layer became a camera-less space root: its range holds the tilted child as a 3D layer.
    CHECK(l.doc->arena().common(l.node("card")).isSpaceRoot == 1);
    const std::string dump = rangeDump(list, 1);
    CHECK(dump.find("PushTransform3D") != std::string::npos);
    // #tilt's world3 is relative to the layer's plane, not to the root.
    const Vec3 centre = mat4Transform(l.doc->arena().common(l.node("tilt")).world3, Vec3{25, 25, 0});
    CHECK(centre.x == doctest::Approx(35.0f));
    CHECK(centre.y == doctest::Approx(35.0f));
    // Hit through the layer's plane: #tilt's centre is where the flat layout put it.
    CHECK(worldLayerHitTest(*l.doc, Vec3{135, 85, 300}, Vec3{0, 0, -1}) == l.node("tilt"));
}

TEST_CASE("T-701: more than 64 layers: the extra switches are ignored and reported once") {
    std::string body = "<div id=\"root\">";
    for (int i = 0; i < 70; ++i) {
        body += "<div class=\"cell\"></div>";
    }
    body += "</div>";
    Loaded l(std::string(kBaseCss) +
                 ".cell { width: 10px; height: 4px; background-color: #ffffff; world-layer: on; }",
             body);
    const CommandList list = l.render();
    CHECK(list.worldLayers.size() == kMaxWorldLayers);
    CHECK(validateCommandList(list));
    size_t warnings = 0;
    for (const std::string& w : l.doc->warnings()) {
        warnings += w == kWarnWorldLayerLimit ? 1u : 0u;
    }
    CHECK(warnings == 1);
    l.ctx.update(0.016f);
    l.render();
    warnings = 0;
    for (const std::string& w : l.doc->warnings()) {
        warnings += w == kWarnWorldLayerLimit ? 1u : 0u;
    }
    CHECK(warnings == 1);
}

TEST_CASE("T-701: turning world-layer output off restores the ordinary list and hit test") {
    Loaded l(std::string(kBaseCss) + "#card { world-layer: on; transform: translateZ(40px); }", kBaseBody);
    Loaded reference(std::string(kBaseCss) + "#card { transform: translateZ(40px); }", kBaseBody,
                     /*layers=*/false);
    CHECK(l.render().worldLayers.size() == 2);
    l.ctx.setWorldLayerOutput(false);
    l.ctx.update(0.016f);
    reference.ctx.update(0.016f);
    const CommandList a = l.render();
    const CommandList b = reference.render();
    CHECK(a.worldLayers.empty());
    std::string da, db;
    dumpCommandList(a, da);
    dumpCommandList(b, db);
    CHECK(da == db);
    CHECK(l.doc->arena().common(l.node("card")).worldLayerActive == 0);
}

TEST_CASE("T-701: the world-layers demo: six layers, one tilted, and a ray reaches its button") {
    // Examples/demos/world-layers (Docs/tasks/T-701.md item 6): the base layer plus #Grid, #Rings,
    // #Panel (tilted), #Tags and #Fore, each at its own depth.
    const std::string path = std::string(XT_EXAMPLES_DIR) + "/demos/world-layers/stage/stage.xt";
    compile::CompileResult result = compile::compileDocument(path, compile::CompileOptions{});
    for (const compile::Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{result.ir.data(), result.ir.size()}, false);
    REQUIRE(handle.ok());
    Document* doc = ctx.document(handle.value());
    ctx.setViewport(Rect{0, 0, 1280, 720}, 1.0f);
    ctx.setWorldLayerOutput(true);
    ctx.update(0.0f);
    CommandList list;
    ctx.render(list);
    CHECK(validateCommandList(list));
    REQUIRE(list.worldLayers.size() == 6);
    const char* ids[] = {"Grid", "Rings", "Panel", "Tags", "Fore"};
    const float depths[] = {30, 90, 160, 230, 300};
    for (size_t i = 0; i < 5; ++i) {
        CAPTURE(ids[i]);
        const ElementRef element = doc->byId(ids[i]);
        REQUIRE(element.valid());
        const WorldLayerRange& layer = list.worldLayers[i + 1];
        CHECK(layer.id == element.node);
        CHECK(layer.order == i + 1);
        // Each layer's own centre sits at its translateZ (the panel turns about its centre).
        const LayoutBox& box = doc->arena().element(element.node).box;
        CHECK(mat4Transform(layer.world3, Vec3{box.width * 0.5f, box.height * 0.5f, 0}).z ==
              doctest::Approx(depths[i]));
    }
    // #Panel: rotateY(-22deg) about its centre -- its plane's x axis leaves the root plane.
    const WorldLayerRange& panel = list.worldLayers[3];
    CHECK(std::fabs(panel.world3.m[8]) ==
          doctest::Approx(std::sin(22.0f * 3.14159265f / 180.0f)).epsilon(1e-3));

    // A ray at #Engage's centre, from an oblique camera, hits it on the tilted plane.
    const ElementRef engage = doc->byId("Engage");
    REQUIRE(engage.valid());
    const Vec3 target = mat4Transform(panel.world3, Vec3{32 + 188, 292 + 28, 0});
    const Vec3 dir{0.35f, -0.1f, -1.0f};
    const Vec3 origin{target.x - dir.x * 900.0f, target.y - dir.y * 900.0f, target.z - dir.z * 900.0f};
    CHECK(worldLayerHitTest(*doc, origin, dir) == engage.node);

    // A later frame: only the turning ring's layer has different commands.
    ctx.update(0.5f);
    CommandList next;
    ctx.render(next);
    REQUIRE(next.worldLayers.size() == 6);
    for (size_t i = 0; i < 6; ++i) {
        CAPTURE(i);
        CHECK((rangeDump(list, i) == rangeDump(next, i)) == (i != 2));
    }
    // #Tags floated out along z (its plane moved), #Fore pulsed (its opacity changed): no command
    // changed.
    const ElementRef tags = doc->byId("Tags");
    const LayoutBox& tagsBox = doc->arena().element(tags.node).box;
    const Vec3 tagsCentre{tagsBox.width * 0.5f, tagsBox.height * 0.5f, 0};
    CHECK(mat4Transform(next.worldLayers[4].world3, tagsCentre).z > 230.5f);
    CHECK(next.worldLayers[5].opacity < 1.0f);

    // T-702: mid-loop (3 s of 6) #Tags is half way through its turn about Y (real @keyframes 3D
    // transforms): its plane's x axis points back along -x. Still only the ring's layer has new
    // commands.
    ctx.update(2.5f);
    CommandList flipped;
    ctx.render(flipped);
    REQUIRE(flipped.worldLayers.size() == 6);
    CHECK(rangeDump(list, 4) == rangeDump(flipped, 4));
    CHECK(flipped.worldLayers[4].world3.m[0] < -0.9f);
    CHECK(mat4Transform(flipped.worldLayers[4].world3, tagsCentre).z == doctest::Approx(280.0f));
}

// -- T-703 (Docs/design/33-world-layers.md §33.13 items 1-2, Docs/tasks/T-703.md). --

namespace {

/// The world-layers demo (Examples/demos/world-layers), loaded at 1280 x 720 in world-layer output.
struct Demo {
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    Document* doc = nullptr;
    std::vector<uint8_t> ir;

    Demo() {
        const std::string path = std::string(XT_EXAMPLES_DIR) + "/demos/world-layers/stage/stage.xt";
        compile::CompileResult result = compile::compileDocument(path, compile::CompileOptions{});
        REQUIRE(result.ok());
        ir = std::move(result.ir);
        Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{ir.data(), ir.size()}, false);
        REQUIRE(handle.ok());
        doc = ctx.document(handle.value());
        REQUIRE(doc != nullptr);
        ctx.setViewport(Rect{0, 0, 1280, 720}, 1.0f);
        ctx.setWorldLayerOutput(true);
        ctx.update(0.0f);
    }
    NodeId node(const char* id) {
        ElementRef ref = doc->byId(id);
        REQUIRE(ref.valid());
        return ref.node;
    }
    CommandList render() {
        CommandList list;
        ctx.render(list);
        return list;
    }
};

bool isWithin(const NodeArena& arena, NodeId n, NodeId ancestor) {
    for (NodeId a = n; a != kInvalidNode; a = arena.parent(a)) {
        if (a == ancestor) {
            return true;
        }
    }
    return false;
}

/// Where the ray `origin` + t `dir` meets the plane of `world3`, in that plane's coordinates.
bool planePoint(const Mat4& world3, Vec3 origin, Vec3 dir, Vec2& q) {
    Mat4 inv{};
    if (!mat4Invert(world3, inv)) {
        return false;
    }
    const Vec3 q0 = mat4Transform(inv, origin);
    const Vec3 q1 = mat4Transform(inv, Vec3{origin.x + dir.x, origin.y + dir.y, origin.z + dir.z});
    if (q0.z == q1.z) {
        return false;
    }
    const float t = q0.z / (q0.z - q1.z);
    q = Vec2{q0.x + (q1.x - q0.x) * t, q0.y + (q1.y - q0.y) * t};
    return true;
}

}  // namespace

TEST_CASE("T-703: a layer's content is hit only where the layer is, never at its untransformed place") {
    // CE-80's report (Docs/tasks/PENDING.md 2026-11-13): #Panel's children were also hit inside the
    // root box's (0..440, 0..380), the panel's own box with no layer transform applied.
    Demo d;
    const CommandList list = d.render();
    const NodeId panel = d.node("Panel");
    const WorldLayerRange* panelLayer = layerOf(list, panel);
    REQUIRE(panelLayer != nullptr);
    const NodeArena& arena = d.doc->arena();
    const LayoutBox& box = arena.element(panel).box;

    // Straight down -z at #Engage's place in the panel's own box: the panel is far from there.
    const NodeId engage = d.node("Engage");
    const NodeId atPhantom = worldLayerHitTest(*d.doc, Vec3{32 + 188, 292 + 28, 1000}, Vec3{0, 0, -1});
    CHECK(atPhantom != engage);
    CHECK_FALSE(isWithin(arena, atPhantom, panel));
    CHECK(d.doc->hitTest(Vec2{32 + 188, 292 + 28}) != engage);

    // The CE probe's grid (32 x 18 rays over the root box), straight and oblique: whatever hit
    // lies inside #Panel must be where the ray crosses the panel's plane inside its box.
    const Vec3 dirs[] = {Vec3{0, 0, -1}, Vec3{0.35f, -0.1f, -1.0f}, Vec3{-0.3f, 0.2f, -1.0f}};
    int panelHits = 0;
    for (const Vec3& dir : dirs) {
        for (int gy = 0; gy < 18; ++gy) {
            for (int gx = 0; gx < 32; ++gx) {
                const float x = (static_cast<float>(gx) + 0.5f) * 1280.0f / 32.0f;
                const float y = (static_cast<float>(gy) + 0.5f) * 720.0f / 18.0f;
                // Through (x, y, 0), from 1000 px in front of the root plane.
                const Vec3 origin{x - dir.x * 1000.0f, y - dir.y * 1000.0f, -dir.z * 1000.0f};
                const NodeId hit = worldLayerHitTest(*d.doc, origin, dir);
                if (hit == kInvalidNode || !isWithin(arena, hit, panel)) {
                    continue;
                }
                ++panelHits;
                Vec2 q{};
                REQUIRE(planePoint(panelLayer->world3, origin, dir, q));
                CAPTURE(x);
                CAPTURE(y);
                CAPTURE(q.x);
                CAPTURE(q.y);
                CHECK(q.x >= 0.0f);
                CHECK(q.y >= 0.0f);
                CHECK(q.x <= box.width);
                CHECK(q.y <= box.height);
            }
        }
    }
    CHECK(panelHits > 0);  // the panel itself is still hit where it is

    // The ray at #Engage's real place still reaches it.
    const Vec3 target = mat4Transform(panelLayer->world3, Vec3{32 + 188, 292 + 28, 0});
    CHECK(worldLayerHitTest(*d.doc, Vec3{target.x, target.y, target.z + 900.0f}, Vec3{0, 0, -1}) == engage);
}

TEST_CASE("T-703: #Tags floating and turning a full loop keeps its rectangle and commands (no redraw)") {
    Demo d;
    const NodeId tags = d.node("Tags");
    const CommandList first = d.render();
    const WorldLayerRange* start = layerOf(first, tags);
    REQUIRE(start != nullptr);
    const Rect rect0 = start->rect;
    const size_t index = static_cast<size_t>(start - first.worldLayers.data());
    const std::string dump0 = rangeDump(first, index);
    int changes = 0;
    float minM0 = 1.0f;
    for (int frame = 1; frame <= 360; ++frame) {  // 6 s at 60 frames a second: one loop
        d.ctx.update(1.0f / 60.0f);
        const CommandList list = d.render();
        const WorldLayerRange* layer = layerOf(list, tags);
        REQUIRE(layer != nullptr);
        minM0 = std::min(minM0, layer->world3.m[0]);
        const bool same = layer->rect.x == rect0.x && layer->rect.y == rect0.y && layer->rect.w == rect0.w &&
                          layer->rect.h == rect0.h &&
                          rangeDump(list, static_cast<size_t>(layer - list.worldLayers.data())) == dump0;
        changes += same ? 0 : 1;
    }
    CHECK(minM0 < -0.9f);  // it did turn (edge-on and past it)
    CHECK(changes == 0);
}

TEST_CASE("T-703: an oversized layer is cut to three root boxes about its centre, whatever its orientation") {
    const std::string css =
        std::string(kBaseCss) +
        "#card { world-layer: on; left: -2000px; top: 20px; width: 5000px; height: 40px; }";
    Loaded flat(css, kBaseBody);
    Loaded turned(css + "#card { transform: translateZ(60px) rotateY(80deg) rotateX(30deg); }", kBaseBody);
    const CommandList a = flat.render();
    const CommandList b = turned.render();
    const WorldLayerRange* la = layerOf(a, flat.node("card"));
    const WorldLayerRange* lb = layerOf(b, turned.node("card"));
    REQUIRE(la != nullptr);
    REQUIRE(lb != nullptr);
    // 5000 (+2 px of slack: -1 .. 5001) cut to 3 x 400 = 1200 about the centre 2500: 1900 .. 3100,
    // out to the 32 px grid: 1888 .. 3104. The height (40 + 2) is far below 3 x 300.
    CHECK(la->rect.x == 1888.0f);
    CHECK(la->rect.w == 3104.0f - 1888.0f);
    CHECK(la->rect.y == -32.0f);
    CHECK(la->rect.h == 96.0f);
    CHECK(lb->rect.x == la->rect.x);
    CHECK(lb->rect.y == la->rect.y);
    CHECK(lb->rect.w == la->rect.w);
    CHECK(lb->rect.h == la->rect.h);
    for (Loaded* l : {&flat, &turned}) {
        size_t warnings = 0;
        for (const std::string& w : l->doc->warnings()) {
            warnings += w == kWarnWorldLayerClamped ? 1u : 0u;
        }
        CHECK(warnings == 1);
    }
    // A layer within the limit is not cut, even turned edge-on (the old rule shrank it).
    Loaded edgeOn(std::string(kBaseCss) + "#card { world-layer: on; transform: rotateY(89deg); }", kBaseBody);
    const CommandList c = edgeOn.render();
    const WorldLayerRange* lc = layerOf(c, edgeOn.node("card"));
    REQUIRE(lc != nullptr);
    CHECK(lc->rect.x == -32.0f);
    CHECK(lc->rect.w == 256.0f);
    for (const std::string& w : edgeOn.doc->warnings()) {
        CHECK(w != kWarnWorldLayerClamped);
    }
}

// -- T-706 (Docs/design/33-world-layers.md §33.14 item 1, Docs/tasks/T-706.md). --

namespace {

bool sameRect(const Rect& a, const Rect& b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }

}  // namespace

TEST_CASE("T-706: #Rings turning for 600 frames no longer switches its rectangle back and forth") {
    // xtframe-view --layers saw #Rings's target switch between 480 and 512 four times in 600 frames
    // (frames 78, 193, 348, 463): the turning dashed ring's bounds sat on a 32 px step boundary.
    // 33 §33.14 item 1 keeps the previous rectangle while the content lies inside it, so the one
    // change left is the first growth past 480 (frame 78: the content leaves the 480 rectangle,
    // which the rule does not cover); it never shrinks back.
    Demo d;
    const NodeId rings = d.node("Rings");
    const CommandList first = d.render();
    const WorldLayerRange* start = layerOf(first, rings);
    REQUIRE(start != nullptr);
    Rect previous = start->rect;
    int changes = 0;
    int shrinks = 0;
    for (int frame = 1; frame < 600; ++frame) {
        d.ctx.update(1.0f / 60.0f);
        const CommandList list = d.render();
        const WorldLayerRange* layer = layerOf(list, rings);
        REQUIRE(layer != nullptr);
        const Rect& r = layer->rect;
        if (!sameRect(r, previous)) {
            ++changes;
            const bool grew = r.x <= previous.x && r.y <= previous.y &&
                              r.x + r.w >= previous.x + previous.w && r.y + r.h >= previous.y + previous.h;
            shrinks += grew ? 0 : 1;
            MESSAGE("frame " << frame << ": #Rings " << previous.w << " x " << previous.h << " -> " << r.w
                             << " x " << r.h);
        }
        previous = r;
    }
    MESSAGE("#Rings rectangle changed " << changes << " time(s) in 600 frames");
    CHECK(shrinks == 0);
    CHECK(changes == 1);
    CHECK(previous.w == 512.0f);
    CHECK(previous.h == 512.0f);
}

TEST_CASE("T-706: a layer's rectangle grows with its content and shrinks only past two steps") {
    Loaded l(std::string(kBaseCss) + "#card { world-layer: on; }", kBaseBody);
    const NodeId card = l.node("card");
    ElementRef ref = l.doc->byId("card");
    const auto rectAt = [&](float width) {
        CHECK(ref.setStyle("width: " + std::to_string(static_cast<int>(width)) + "px") == Status::kOk);
        l.ctx.update(0.016f);
        const CommandList list = l.render();
        const WorldLayerRange* layer = layerOf(list, card);
        REQUIRE(layer != nullptr);
        return layer->rect;
    };
    // 200 px wide: content -1 .. 201 (1 px of slack), out to the grid -32 .. 224.
    Rect rect = rectAt(200);
    CHECK(rect.x == -32.0f);
    CHECK(rect.w == 256.0f);
    // Content that keeps growing: the rectangle grows with it and always holds it.
    float lastW = rect.w;
    for (int width = 210; width <= 400; width += 10) {
        CAPTURE(width);
        rect = rectAt(static_cast<float>(width));
        CHECK(rect.w >= lastW);
        CHECK(rect.x <= -1.0f);
        CHECK(rect.x + rect.w >= static_cast<float>(width) + 1.0f);
        lastW = rect.w;
    }
    CHECK(rect.w == 448.0f);  // 400 px: -32 .. 416

    // Shrinking by more than two steps (64 px): 300 px needs -32 .. 320, 448 - 352 = 96 > 64.
    rect = rectAt(300);
    CHECK(rect.x == -32.0f);
    CHECK(rect.w == 352.0f);
    // Less than two steps: 260 px needs 320 (32 less), the rectangle stays.
    rect = rectAt(260);
    CHECK(rect.x == -32.0f);
    CHECK(rect.w == 352.0f);
    // Exactly two steps: 230 px needs 288 (64 less), the rectangle still stays.
    rect = rectAt(230);
    CHECK(rect.x == -32.0f);
    CHECK(rect.w == 352.0f);
    // Past two steps again: 200 px needs 256 (96 less).
    rect = rectAt(200);
    CHECK(rect.x == -32.0f);
    CHECK(rect.w == 256.0f);
    // The height never changed.
    CHECK(rect.y == -32.0f);
    CHECK(rect.h == 160.0f);
}

TEST_CASE("T-706: the previous rectangle is forgotten when the layer goes or the output is switched") {
    Loaded l(std::string(kBaseCss) + "#card { world-layer: on; width: 300px; }", kBaseBody);
    const NodeId card = l.node("card");
    ElementRef ref = l.doc->byId("card");
    const auto layerRect = [&](Rect& rect) {
        const CommandList list = l.render();
        const WorldLayerRange* layer = layerOf(list, card);
        if (layer == nullptr) {
            return false;
        }
        rect = layer->rect;
        return true;
    };
    Rect rect{};
    REQUIRE(layerRect(rect));
    CHECK(rect.w == 352.0f);  // -32 .. 320
    CHECK(ref.setStyle("width: 260px") == Status::kOk);
    l.ctx.update(0.016f);
    REQUIRE(layerRect(rect));
    CHECK(rect.w == 352.0f);  // kept (fresh: 320)

    // Output off and on again, without a frame between: a fresh rectangle.
    l.ctx.setWorldLayerOutput(false);
    l.ctx.setWorldLayerOutput(true);
    l.ctx.update(0.016f);
    REQUIRE(layerRect(rect));
    CHECK(rect.w == 320.0f);

    // Back to 352 (it grew), then off for a painted frame and on again: fresh.
    CHECK(ref.setStyle("width: 300px") == Status::kOk);
    l.ctx.update(0.016f);
    REQUIRE(layerRect(rect));
    CHECK(rect.w == 352.0f);
    CHECK(ref.setStyle("width: 260px") == Status::kOk);
    l.ctx.setWorldLayerOutput(false);
    l.ctx.update(0.016f);
    CHECK(l.render().worldLayers.empty());
    l.ctx.setWorldLayerOutput(true);
    l.ctx.update(0.016f);
    REQUIRE(layerRect(rect));
    CHECK(rect.w == 320.0f);

    // The layer goes for a frame (its switch off) and comes back: fresh.
    CHECK(ref.setStyle("width: 300px") == Status::kOk);
    l.ctx.update(0.016f);
    REQUIRE(layerRect(rect));
    CHECK(rect.w == 352.0f);
    CHECK(ref.setStyle("width: 260px; world-layer: off") == Status::kOk);
    l.ctx.update(0.016f);
    CHECK_FALSE(layerRect(rect));
    CHECK(ref.setStyle("width: 260px; world-layer: on") == Status::kOk);
    l.ctx.update(0.016f);
    REQUIRE(layerRect(rect));
    CHECK(rect.w == 320.0f);
}
