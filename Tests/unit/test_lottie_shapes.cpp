#include <cstdint>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/compile/ir_writer.h"
#include "xt/compile/lottie_parser.h"
#include "xt/ir.h"
#include "xt/ir_pod.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/property.h"
#include "xt/scene_instance.h"
#include "xt/timeline.h"
#include "xt/types.h"

/// Tests for T-15 (Lottie shape mapping). See Docs/tasks/T-15.md "測試".

using namespace xt;
using namespace xt::compile;

namespace {

std::vector<uint8_t> loadFixture(const std::string& name) {
    std::string path = std::string(XT_TEST_DATA_DIR) + "/" + name;
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<uint8_t> toBytes(const char* s) {
    return std::vector<uint8_t>(s, s + std::char_traits<char>::length(s));
}

struct ParsedFixture {
    IrWriter writer;
    DiagnosticSink sink;
    LottieResult result;
    std::vector<uint8_t> ir;
    Result<IrView> view = Result<IrView>::error(Status::kInternal);
};

ParsedFixture parseBytes(const std::vector<uint8_t>& bytes, bool strict = false) {
    ParsedFixture pf;
    LottieOptions opts;
    opts.strict = strict;
    opts.sourceName = "test.json";
    pf.result = parseLottie(std::span<const uint8_t>(bytes.data(), bytes.size()), opts, pf.writer, pf.sink);
    pf.ir = pf.writer.finish();
    if (pf.result.ok) {
        pf.view = IrView::open(IrBlob{pf.ir.data(), pf.ir.size()});
    }
    return pf;
}

/// Wraps `shapesJson` (a JSON array literal for a shape layer's `shapes[]`) into a full one-layer
/// document.
std::vector<uint8_t> oneShapeLayerDoc(const std::string& shapesJson) {
    std::string doc = std::string(R"json({
      "v":"5.6.5","fr":30,"ip":0,"op":30,"w":100,"h":100,
      "layers":[
        {"ty":4,"ind":0,"nm":"Layer","ip":0,"op":30,"st":0,"sr":1,
         "ks":{"a":{"a":0,"k":[0,0,0]},"p":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]},
               "r":{"a":0,"k":0},"o":{"a":0,"k":100}},
         "shapes":)json") +
                      shapesJson + "}]}";
    return toBytes(doc.c_str());
}

}  // namespace

TEST_CASE("lottie shapes: rc + fl produces a 5-verb rect path, red fill, NonZero") {
    ParsedFixture pf = parseBytes(loadFixture("shape_rect_fill.json"));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    REQUIRE(scene.nodes().size() == 4);  // root, transform, content, path.
    const IrSceneNode& pathNode = scene.nodes()[3];
    CHECK(pathNode.kind == static_cast<uint8_t>(NodeKind::Path));
    CHECK(pathNode.parent == 2);
    REQUIRE(scene.paths().size() == 1);
    const IrPathData& path = scene.paths()[0];
    CHECK(path.verbCount == 5);
    CHECK(path.fillKind == static_cast<uint8_t>(PaintKind::Solid));
    CHECK(path.fillColor.r == doctest::Approx(1.0f));
    CHECK(path.fillColor.g == doctest::Approx(0.0f));
    CHECK(path.fillColor.b == doctest::Approx(0.0f));
    CHECK(path.fillRule == static_cast<uint8_t>(FillRule::NonZero));
}

TEST_CASE("lottie shapes: rounded rc produces a 10-verb path") {
    static const char* shapes = R"json([
        {"ty":"rc","p":{"a":0,"k":[50,50]},"s":{"a":0,"k":[40,20]},"r":{"a":0,"k":5},"d":1},
        {"ty":"fl","c":{"a":0,"k":[0,1,0,1]},"o":{"a":0,"k":100},"r":1}
    ])json";
    ParsedFixture pf = parseBytes(oneShapeLayerDoc(shapes));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    REQUIRE(scene.paths().size() == 1);
    CHECK(scene.paths()[0].verbCount == 10);
}

TEST_CASE("lottie shapes: el produces Move + 4 Cubic + Close starting at the top") {
    static const char* shapes = R"json([
        {"ty":"el","p":{"a":0,"k":[50,50]},"s":{"a":0,"k":[40,40]}},
        {"ty":"fl","c":{"a":0,"k":[0,0,1,1]},"o":{"a":0,"k":100}}
    ])json";
    ParsedFixture pf = parseBytes(oneShapeLayerDoc(shapes));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    REQUIRE(scene.paths().size() == 1);
    const IrPathData& path = scene.paths()[0];
    CHECK(path.verbCount == 6);  // Move + 4 Cubic + Close.
    CHECK(scene.floatPool()[path.pointOffset + 0] == doctest::Approx(50.0f));
    CHECK(scene.floatPool()[path.pointOffset + 1] == doctest::Approx(30.0f));  // cy - ry
}

TEST_CASE("lottie shapes: sr 5-point star with no roundness has 10 straight-line vertices") {
    static const char* shapes = R"json([
        {"ty":"sr","sy":1,"pt":{"a":0,"k":5},"p":{"a":0,"k":[50,50]},
         "or":{"a":0,"k":100},"ir":{"a":0,"k":50},"os":{"a":0,"k":0},"is":{"a":0,"k":0},
         "r":{"a":0,"k":0}},
        {"ty":"fl","c":{"a":0,"k":[1,1,0,1]},"o":{"a":0,"k":100}}
    ])json";
    ParsedFixture pf = parseBytes(oneShapeLayerDoc(shapes));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    REQUIRE(scene.paths().size() == 1);
    // Move + 9 Line + Close = 11 verbs, 10 vertices.
    CHECK(scene.paths()[0].verbCount == 11);
    CHECK(scene.paths()[0].pointCount == 20);
}

TEST_CASE("lottie shapes: sr polygon (sy:2) has numPoints vertices") {
    static const char* shapes = R"json([
        {"ty":"sr","sy":2,"pt":{"a":0,"k":5},"p":{"a":0,"k":[50,50]},
         "or":{"a":0,"k":100},"ir":{"a":0,"k":0},"os":{"a":0,"k":0},"is":{"a":0,"k":0},
         "r":{"a":0,"k":0}},
        {"ty":"fl","c":{"a":0,"k":[1,1,0,1]},"o":{"a":0,"k":100}}
    ])json";
    ParsedFixture pf = parseBytes(oneShapeLayerDoc(shapes));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    REQUIRE(scene.paths().size() == 1);
    CHECK(scene.paths()[0].verbCount == 6);  // Move + 4 Line + Close, 5 vertices.
}

TEST_CASE("lottie shapes: static closed sh triangle has correct control points") {
    static const char* shapes = R"json([
        {"ty":"sh","ks":{"a":0,"k":{"c":true,
            "v":[[0,0],[10,0],[5,10]],
            "i":[[-2,0],[-2,0],[-2,0]],
            "o":[[2,0],[2,0],[2,0]]}}},
        {"ty":"fl","c":{"a":0,"k":[1,1,1,1]},"o":{"a":0,"k":100}}
    ])json";
    ParsedFixture pf = parseBytes(oneShapeLayerDoc(shapes));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    REQUIRE(scene.paths().size() == 1);
    const IrPathData& path = scene.paths()[0];
    CHECK(path.verbCount == 5);  // Move + 3 Cubic + Close.
    std::span<const float> pts = scene.floatPool().subspan(path.pointOffset, path.pointCount);
    // Move: v[0] = (0,0).
    CHECK(pts[0] == doctest::Approx(0.0f));
    CHECK(pts[1] == doctest::Approx(0.0f));
    // First Cubic: c1 = v[0]+o[0] = (2,0); c2 = v[1]+i[1] = (10-2,0) = (8,0); end = v[1] = (10,0).
    CHECK(pts[2] == doctest::Approx(2.0f));
    CHECK(pts[3] == doctest::Approx(0.0f));
    CHECK(pts[4] == doctest::Approx(8.0f));
    CHECK(pts[5] == doctest::Approx(0.0f));
    CHECK(pts[6] == doctest::Approx(10.0f));
    CHECK(pts[7] == doctest::Approx(0.0f));
}

TEST_CASE("lottie shapes: [rc, el, fl, st] inserts st x rc, st x el, fl x rc, fl x el") {
    static const char* shapes = R"json([
        {"ty":"rc","p":{"a":0,"k":[10,10]},"s":{"a":0,"k":[4,4]},"r":{"a":0,"k":0},"d":1},
        {"ty":"el","p":{"a":0,"k":[20,20]},"s":{"a":0,"k":[4,4]}},
        {"ty":"fl","c":{"a":0,"k":[0,1,0,1]},"o":{"a":0,"k":100}},
        {"ty":"st","c":{"a":0,"k":[0,0,1,1]},"o":{"a":0,"k":100},"w":{"a":0,"k":2}}
    ])json";
    ParsedFixture pf = parseBytes(oneShapeLayerDoc(shapes));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    REQUIRE(scene.nodes().size() == 7);  // root, transform, content, + 4 paths.
    REQUIRE(scene.paths().size() == 4);
    // Insertion order: st x rc (5 verbs), st x el (6 verbs), fl x rc (5 verbs), fl x el (6 verbs).
    CHECK(scene.paths()[0].strokeKind == static_cast<uint8_t>(PaintKind::Solid));
    CHECK(scene.paths()[0].fillKind == static_cast<uint8_t>(PaintKind::None));
    CHECK(scene.paths()[0].verbCount == 5);
    CHECK(scene.paths()[1].strokeKind == static_cast<uint8_t>(PaintKind::Solid));
    CHECK(scene.paths()[1].verbCount == 6);
    CHECK(scene.paths()[2].fillKind == static_cast<uint8_t>(PaintKind::Solid));
    CHECK(scene.paths()[2].strokeKind == static_cast<uint8_t>(PaintKind::None));
    CHECK(scene.paths()[2].verbCount == 5);
    CHECK(scene.paths()[3].fillKind == static_cast<uint8_t>(PaintKind::Solid));
    CHECK(scene.paths()[3].verbCount == 6);
}

TEST_CASE("lottie shapes: subgroup with tr becomes a Group node; animated opacity gets a Track") {
    static const char* shapes = R"json([
        {"ty":"gr","nm":"G","it":[
            {"ty":"rc","p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[10,10]},"r":{"a":0,"k":0},"d":1},
            {"ty":"fl","c":{"a":0,"k":[1,0,0,1]},"o":{"a":0,"k":100}},
            {"ty":"tr","a":{"a":0,"k":[0,0]},"p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},
             "r":{"a":0,"k":0},
             "o":{"a":1,"k":[{"t":0,"s":[100],"o":{"x":[0],"y":[0]},"i":{"x":[1],"y":[1]}},
                              {"t":10,"s":[0]}]}}
        ]}
    ])json";
    ParsedFixture pf = parseBytes(oneShapeLayerDoc(shapes));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    REQUIRE(scene.nodes().size() == 5);  // root, transform, content, group, path.
    CHECK(scene.nodes()[3].kind == static_cast<uint8_t>(NodeKind::Group));
    CHECK(scene.nodes()[4].parent == 3);

    const TimelineChunk& tl = pf.view.value().timeline(pf.result.timeline);
    bool foundOpacity = false;
    for (const IrTrack& t : tl.tracks()) {
        if (t.prop == prop::Opacity && t.target == 3) {
            foundOpacity = true;
        }
    }
    CHECK(foundOpacity);
}

TEST_CASE("lottie shapes: static tm produces trimEnd 0.5 on both produced paths") {
    static const char* shapes = R"json([
        {"ty":"rc","p":{"a":0,"k":[10,10]},"s":{"a":0,"k":[4,4]},"r":{"a":0,"k":0},"d":1},
        {"ty":"el","p":{"a":0,"k":[20,20]},"s":{"a":0,"k":[4,4]}},
        {"ty":"fl","c":{"a":0,"k":[0,1,0,1]},"o":{"a":0,"k":100}},
        {"ty":"tm","s":{"a":0,"k":0},"e":{"a":0,"k":50},
         "o":{"a":1,"k":[{"t":0,"s":[0],"o":{"x":[0],"y":[0]},"i":{"x":[1],"y":[1]}},
                          {"t":10,"s":[20]}]}}
    ])json";
    ParsedFixture pf = parseBytes(oneShapeLayerDoc(shapes));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    REQUIRE(scene.paths().size() == 2);
    CHECK(scene.paths()[0].trimEnd == doctest::Approx(0.5f));
    CHECK(scene.paths()[1].trimEnd == doctest::Approx(0.5f));

    const TimelineChunk& tl = pf.view.value().timeline(pf.result.timeline);
    bool foundOffset = false;
    for (const IrTrack& t : tl.tracks()) {
        if (t.prop == prop::TrimOffset) {
            foundOffset = true;
        }
    }
    CHECK(foundOffset);
}

// T-236: Lottie `tm.o` is in degrees; the per-path TrimOffset is in turns (lottie-web: o / 360).
TEST_CASE("lottie shapes: single-path tm offset 90 degrees becomes trimOffset 0.25 turns") {
    static const char* shapes = R"json([
        {"ty":"rc","p":{"a":0,"k":[10,10]},"s":{"a":0,"k":[4,4]},"r":{"a":0,"k":0},"d":1},
        {"ty":"st","c":{"a":0,"k":[0,0,0,1]},"o":{"a":0,"k":100},"w":{"a":0,"k":1}},
        {"ty":"tm","s":{"a":0,"k":0},"e":{"a":0,"k":50},"o":{"a":0,"k":90}}
    ])json";
    ParsedFixture pf = parseBytes(oneShapeLayerDoc(shapes));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    REQUIRE(scene.paths().size() == 1);
    CHECK(scene.paths()[0].trimOffset == doctest::Approx(0.25f));
}

// T-236: `ml` written as a plain number (the usual exporter output) was read as 0.
TEST_CASE("lottie shapes: st miter limit as a plain number, a property object, or missing") {
    static const char* shapes = R"json([
        {"ty":"rc","p":{"a":0,"k":[10,10]},"s":{"a":0,"k":[4,4]},"r":{"a":0,"k":0},"d":1},
        {"ty":"st","c":{"a":0,"k":[0,0,0,1]},"o":{"a":0,"k":100},"w":{"a":0,"k":1},"lj":1,"ml":7}
    ])json";
    ParsedFixture pf = parseBytes(oneShapeLayerDoc(shapes));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    REQUIRE(scene.paths().size() == 1);
    CHECK(scene.paths()[0].miterLimit == doctest::Approx(7.0f));

    static const char* objectShapes = R"json([
        {"ty":"rc","p":{"a":0,"k":[10,10]},"s":{"a":0,"k":[4,4]},"r":{"a":0,"k":0},"d":1},
        {"ty":"st","c":{"a":0,"k":[0,0,0,1]},"o":{"a":0,"k":100},"w":{"a":0,"k":1},"lj":1,
         "ml":{"a":0,"k":3}}
    ])json";
    ParsedFixture po = parseBytes(oneShapeLayerDoc(objectShapes));
    REQUIRE(po.result.ok);
    REQUIRE(po.view.ok());
    REQUIRE(po.view.value().scene(po.result.scene).paths().size() == 1);
    CHECK(po.view.value().scene(po.result.scene).paths()[0].miterLimit == doctest::Approx(3.0f));

    static const char* missingShapes = R"json([
        {"ty":"rc","p":{"a":0,"k":[10,10]},"s":{"a":0,"k":[4,4]},"r":{"a":0,"k":0},"d":1},
        {"ty":"st","c":{"a":0,"k":[0,0,0,1]},"o":{"a":0,"k":100},"w":{"a":0,"k":1},"lj":1}
    ])json";
    ParsedFixture pm = parseBytes(oneShapeLayerDoc(missingShapes));
    REQUIRE(pm.result.ok);
    REQUIRE(pm.view.ok());
    REQUIRE(pm.view.value().scene(pm.result.scene).paths().size() == 1);
    CHECK(pm.view.value().scene(pm.result.scene).paths()[0].miterLimit == doctest::Approx(4.0f));
}

// T-230 (31 §31.9): rd is a runtime Round modifier node after the geometry, no longer baked.
TEST_CASE("lottie shapes: rd becomes a Round modifier node after the rc / sh path") {
    static const char* shapesRect = R"json([
        {"ty":"rc","p":{"a":0,"k":[50,50]},"s":{"a":0,"k":[40,20]},"r":{"a":0,"k":0},"d":1},
        {"ty":"fl","c":{"a":0,"k":[1,0,0,1]},"o":{"a":0,"k":100}},
        {"ty":"rd","r":{"a":0,"k":5}}
    ])json";
    {
        ParsedFixture pf = parseBytes(oneShapeLayerDoc(shapesRect));
        REQUIRE(pf.result.ok);
        REQUIRE(pf.view.ok());
        const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
        REQUIRE(scene.paths().size() == 1);
        CHECK(scene.paths()[0].verbCount == 5);  // The plain rect: Move + 3 Line + Close.
        REQUIRE(scene.modifiers().size() == 1);
        CHECK(scene.modifiers()[0].kind == static_cast<uint8_t>(ModifierKind::Round));
        CHECK(scene.modifiers()[0].params[0] == doctest::Approx(5.0f));
        // The modifier follows the path in the same group.
        const auto& nodes = scene.nodes();
        CHECK(nodes[nodes.size() - 1].kind == static_cast<uint8_t>(NodeKind::Modifier));
        CHECK(nodes[nodes.size() - 2].kind == static_cast<uint8_t>(NodeKind::Path));
        CHECK(nodes[nodes.size() - 1].parent == nodes[nodes.size() - 2].parent);
    }
    static const char* shapesTriangle = R"json([
        {"ty":"sh","ks":{"a":0,"k":{"c":true,"v":[[0,0],[10,0],[5,10]],
            "i":[[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0]]}}},
        {"ty":"fl","c":{"a":0,"k":[1,1,1,1]},"o":{"a":0,"k":100}},
        {"ty":"rd","r":{"a":0,"k":2}}
    ])json";
    {
        ParsedFixture pf = parseBytes(oneShapeLayerDoc(shapesTriangle));
        REQUIRE(pf.result.ok);
        REQUIRE(pf.view.ok());
        const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
        REQUIRE(scene.paths().size() == 1);
        // The triangle itself (Move + 3 segments + Close); the Painter rounds it at run time.
        CHECK(scene.paths()[0].verbCount == 5);
        REQUIRE(scene.modifiers().size() == 1);
        CHECK(scene.modifiers()[0].params[0] == doctest::Approx(2.0f));
    }
}

// T-230: a repeater directly after its shapes is a runtime Repeat modifier node.
TEST_CASE("lottie shapes: rp becomes a Repeat modifier node with its transform and opacities") {
    static const char* shapes = R"json([
        {"ty":"rc","p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[4,4]},"r":{"a":0,"k":0},"d":1},
        {"ty":"fl","c":{"a":0,"k":[1,0,0,1]},"o":{"a":0,"k":100}},
        {"ty":"rp","c":{"a":0,"k":3},"o":{"a":0,"k":0},
         "tr":{"a":{"a":0,"k":[0,0]},"p":{"a":0,"k":[10,0]},"s":{"a":0,"k":[100,100]},
               "r":{"a":0,"k":10},"so":{"a":0,"k":100},"eo":{"a":0,"k":0}}}
    ])json";
    ParsedFixture pf = parseBytes(oneShapeLayerDoc(shapes));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    REQUIRE(scene.modifiers().size() == 1);
    const IrModifierData& m = scene.modifiers()[0];
    CHECK(m.kind == static_cast<uint8_t>(ModifierKind::Repeat));
    CHECK(m.mode == 1);  // Lottie m = 1 (above): copy 0 at the bottom, as the old expansion.
    CHECK(m.params[0] == doctest::Approx(3.0f));   // copies
    CHECK(m.params[4] == doctest::Approx(10.0f));  // translate x
    CHECK(m.params[6] == doctest::Approx(10.0f));  // rotation
    CHECK(m.params[7] == doctest::Approx(1.0f));   // scale
    CHECK(m.opacityA == doctest::Approx(1.0f));
    CHECK(m.opacityB == doctest::Approx(0.0f));
    REQUIRE(scene.paths().size() == 1);
}

// T-230: a repeater after a nested group is still expanded at compile time (Modifier nodes do not
// reach nested groups).
TEST_CASE("lottie shapes: rp after a nested group is expanded into copies") {
    static const char* shapes = R"json([
        {"ty":"gr","it":[
            {"ty":"rc","p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[4,4]},"r":{"a":0,"k":0},"d":1},
            {"ty":"fl","c":{"a":0,"k":[1,0,0,1]},"o":{"a":0,"k":100}}]},
        {"ty":"rp","c":{"a":0,"k":3},"o":{"a":0,"k":0},
         "tr":{"a":{"a":0,"k":[0,0]},"p":{"a":0,"k":[10,0]},"s":{"a":0,"k":[100,100]},
               "r":{"a":0,"k":10},"so":{"a":0,"k":100},"eo":{"a":0,"k":0}}}
    ])json";
    ParsedFixture pf = parseBytes(oneShapeLayerDoc(shapes));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    CHECK(scene.modifiers().empty());
    CHECK(scene.paths().size() == 3);
}

// T-230: pb / zz / tw become modifier nodes; op / mm are ignored (with a warning) by the <anim>
// builder; tm m = 2 is a serial Trim modifier.
TEST_CASE("lottie shapes: pb, zz, tw, serial tm as modifier nodes; op and mm ignored") {
    static const char* shapes = R"json([
        {"ty":"rc","p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[40,40]},"r":{"a":0,"k":0},"d":1},
        {"ty":"fl","c":{"a":0,"k":[1,0,0,1]},"o":{"a":0,"k":100}},
        {"ty":"pb","a":{"a":0,"k":25}},
        {"ty":"zz","s":{"a":0,"k":3},"r":{"a":0,"k":4},"pt":{"a":0,"k":2}},
        {"ty":"tw","a":{"a":0,"k":45},"c":{"a":0,"k":[1,2]}},
        {"ty":"op","a":{"a":0,"k":3},"lj":1},
        {"ty":"mm","mm":1},
        {"ty":"tm","s":{"a":0,"k":10},"e":{"a":0,"k":90},"o":{"a":0,"k":30},"m":2}
    ])json";
    ParsedFixture pf = parseBytes(oneShapeLayerDoc(shapes));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    REQUIRE(scene.modifiers().size() == 4);
    CHECK(scene.modifiers()[0].kind == static_cast<uint8_t>(ModifierKind::Pucker));
    CHECK(scene.modifiers()[0].params[0] == doctest::Approx(25.0f));
    CHECK(scene.modifiers()[1].kind == static_cast<uint8_t>(ModifierKind::ZigZag));
    CHECK(scene.modifiers()[1].mode == 1);  // pt 2 = smooth.
    CHECK(scene.modifiers()[1].params[1] == doctest::Approx(4.0f));
    CHECK(scene.modifiers()[2].kind == static_cast<uint8_t>(ModifierKind::Twist));
    CHECK(scene.modifiers()[2].params[2] == doctest::Approx(2.0f));
    CHECK(scene.modifiers()[3].kind == static_cast<uint8_t>(ModifierKind::Trim));
    CHECK(scene.modifiers()[3].mode == 1);  // serial
    CHECK(scene.modifiers()[3].params[0] == doctest::Approx(0.1f));
    CHECK(scene.modifiers()[3].params[2] == doctest::Approx(30.0f));  // degrees
    bool warnedBaked = false;
    for (const auto& d : pf.sink.all()) {
        warnedBaked = warnedBaked || d.message.find("offset paths / merge paths") != std::string::npos;
    }
    CHECK(warnedBaked);
}

TEST_CASE("lottie shapes: gf linear gradient merges 3 color stops with 2 alpha stops") {
    static const char* shapes = R"json([
        {"ty":"rc","p":{"a":0,"k":[50,50]},"s":{"a":0,"k":[40,20]},"r":{"a":0,"k":0},"d":1},
        {"ty":"gf","t":1,"s":{"a":0,"k":[0,0]},"e":{"a":0,"k":[100,0]},
         "g":{"p":3,"k":{"a":0,"k":[
            0,1,0,0,
            0.5,0,1,0,
            1,0,0,1,
            0,0,
            1,1
         ]}}}
    ])json";
    ParsedFixture pf = parseBytes(oneShapeLayerDoc(shapes));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    REQUIRE(scene.paths().size() == 1);
    const IrPathData& path = scene.paths()[0];
    CHECK(path.fillKind == static_cast<uint8_t>(PaintKind::Linear));
    REQUIRE(path.fillStopCount == 3);
    std::span<const IrGradientStop> stops = scene.stopPool().subspan(path.fillStopOffset, path.fillStopCount);
    CHECK(stops[0].offset == doctest::Approx(0.0f));
    CHECK(stops[0].color.r == doctest::Approx(1.0f));
    CHECK(stops[0].color.a == doctest::Approx(0.0f));
    CHECK(stops[1].offset == doctest::Approx(0.5f));
    CHECK(stops[1].color.g == doctest::Approx(1.0f));
    CHECK(stops[1].color.a == doctest::Approx(0.5f));
    CHECK(stops[2].offset == doctest::Approx(1.0f));
    CHECK(stops[2].color.b == doctest::Approx(1.0f));
    CHECK(stops[2].color.a == doctest::Approx(1.0f));
}

TEST_CASE("lottie shapes: st dash [o:5, d:10, g:5] produces dashPhase 5 and dashes {10,5}") {
    static const char* shapes = R"json([
        {"ty":"rc","p":{"a":0,"k":[50,50]},"s":{"a":0,"k":[40,20]},"r":{"a":0,"k":0},"d":1},
        {"ty":"st","c":{"a":0,"k":[0,0,0,1]},"o":{"a":0,"k":100},"w":{"a":0,"k":2},
         "d":[{"n":"o","v":{"a":0,"k":5}},{"n":"d","v":{"a":0,"k":10}},{"n":"g","v":{"a":0,"k":5}}]}
    ])json";
    ParsedFixture pf = parseBytes(oneShapeLayerDoc(shapes));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    REQUIRE(scene.paths().size() == 1);
    const IrPathData& path = scene.paths()[0];
    CHECK(path.dashPhase == doctest::Approx(5.0f));
    REQUIRE(path.dashCount == 2);
    std::span<const float> dashes = scene.floatPool().subspan(path.dashOffset, path.dashCount);
    CHECK(dashes[0] == doctest::Approx(10.0f));
    CHECK(dashes[1] == doctest::Approx(5.0f));
}

TEST_CASE("lottie shapes: animated rc size produces a 2-keyframe PathData Track with matching verbs") {
    static const char* shapes = R"json([
        {"ty":"rc","p":{"a":0,"k":[50,50]},
         "s":{"a":1,"k":[{"t":0,"s":[40,20]},{"t":10,"s":[60,20]}]},
         "r":{"a":0,"k":0},"d":1},
        {"ty":"fl","c":{"a":0,"k":[1,0,0,1]},"o":{"a":0,"k":100}}
    ])json";
    ParsedFixture pf = parseBytes(oneShapeLayerDoc(shapes));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    REQUIRE(scene.paths().size() == 1);
    CHECK(scene.paths()[0].verbCount == 5);

    const TimelineChunk& tl = pf.view.value().timeline(pf.result.timeline);
    const IrTrack* pathTrack = nullptr;
    for (const IrTrack& t : tl.tracks()) {
        if (t.prop == prop::PathData) {
            pathTrack = &t;
        }
    }
    REQUIRE(pathTrack != nullptr);
    REQUIRE(pathTrack->kfCount == 2);
    const IrKeyframe& kf0 = tl.keyframes()[pathTrack->kfOffset];
    const IrKeyframe& kf1 = tl.keyframes()[pathTrack->kfOffset + 1];
    uint32_t count0 = irValueU32(kf0.value, 1);
    uint32_t count1 = irValueU32(kf1.value, 1);
    CHECK(count0 == count1);
    CHECK(count0 == 8);  // 4 points x 2 floats (unrounded rect).
    uint32_t off1 = irValueU32(kf1.value, 0);
    // Frame 10: size (60,20) -> half-width 30; top-right corner = center + (30,-10) = (80,40).
    CHECK(tl.floatPool()[off1 + 0] == doctest::Approx(80.0f));
    CHECK(tl.floatPool()[off1 + 1] == doctest::Approx(40.0f));
}

TEST_CASE("lottie shapes: animated sh with a differing vertex count warns and stays static") {
    static const char* shapes = R"json([
        {"ty":"sh","ks":{"a":1,"k":[
            {"t":0,"s":[{"c":true,"v":[[0,0],[10,0],[5,10]],"i":[[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0]]}]},
            {"t":10,"s":[{"c":true,"v":[[0,0],[10,0],[10,10],[0,10]],"i":[[0,0],[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0],[0,0]]}]}
        ]}},
        {"ty":"fl","c":{"a":0,"k":[1,1,1,1]},"o":{"a":0,"k":100}}
    ])json";
    ParsedFixture pf = parseBytes(oneShapeLayerDoc(shapes));
    REQUIRE(pf.result.ok);
    CHECK(pf.sink.warningCount() >= 1);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    REQUIRE(scene.paths().size() == 1);
    CHECK(scene.paths()[0].verbCount == 5);  // First keyframe (triangle): Move + 3 Cubic + Close.
    const TimelineChunk& tl = pf.view.value().timeline(pf.result.timeline);
    for (const IrTrack& t : tl.tracks()) {
        CHECK(t.prop != prop::PathData);
    }
}

TEST_CASE("lottie shapes: an \"mm\" shape warns exactly once") {
    static const char* shapes = R"json([{"ty":"mm"},{"ty":"mm"}])json";
    ParsedFixture pf = parseBytes(oneShapeLayerDoc(shapes));
    REQUIRE(pf.result.ok);
    CHECK(pf.sink.warningCount() == 1);
}

// T-47b: a ddd:1 shape layer still builds its shape content normally underneath a 3D
// TransformGroup (is3D is purely a transform-node property; content emission is unaffected). See
// Docs/tasks/T-47b.md.
TEST_CASE("lottie shapes: ddd:1 shape layer builds shape content under a 3D TransformGroup") {
    static const char* json = R"json({
      "v":"5.6.5","fr":30,"ip":0,"op":30,"w":100,"h":100,
      "layers":[
        {"ty":4,"ind":0,"nm":"Layer","ddd":1,"ip":0,"op":30,"st":0,"sr":1,
         "ks":{"a":{"a":0,"k":[0,0,0]},"p":{"a":0,"k":[0,0,10]},"s":{"a":0,"k":[100,100,100]},
               "r":{"a":0,"k":0},"o":{"a":0,"k":100}},
         "shapes":[
            {"ty":"rc","p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[10,10]},"r":{"a":0,"k":0}},
            {"ty":"fl","c":{"a":0,"k":[1,1,1,1]},"o":{"a":0,"k":100}}
         ]}
      ]
    })json";
    ParsedFixture pf = parseBytes(toBytes(json));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    REQUIRE(scene.nodes().size() == 4);  // root, transform, content, path.
    CHECK(scene.nodes()[1].is3D == 1);
    CHECK(scene.nodes()[1].translateZ == doctest::Approx(-10.0f));
    CHECK(scene.paths()[0].verbCount == 5);  // rc: Move + 3 Line + Close.
}

TEST_CASE("lottie shapes: integration -- finish() -> IrView::open -> instantiateScene succeeds") {
    ParsedFixture pf = parseBytes(loadFixture("shape_rect_fill.json"));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);

    NodeArena arena;
    NodeId parent = arena.create(NodeKind::Group);
    SceneInstance inst;
    REQUIRE(instantiateScene(pf.view.value(), pf.result.scene, arena, parent, inst) == Status::kOk);
    CHECK(inst.localToArena.size() == scene.nodes().size());
}

// --- T-258b (Docs/reviews/2026-10-02-full-audit-raw/D.md D3): star points (D6 budgets:
// Tests/import/test_lottie_bounds.cpp) ---

namespace {

bool hasDiagnostic(const DiagnosticSink& sink, Diagnostic::Level level, const std::string& needle) {
    for (const Diagnostic& d : sink.all()) {
        if (d.level == level && d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("lottie shapes: T-258b D3 sr pt above 1024 is clamped to 1024 with a warning") {
    static const char* shapes = R"json([
        {"ty":"sr","sy":1,"pt":{"a":0,"k":5000},"p":{"a":0,"k":[50,50]},
         "or":{"a":0,"k":100},"ir":{"a":0,"k":50},"os":{"a":0,"k":0},"is":{"a":0,"k":0},
         "r":{"a":0,"k":0}},
        {"ty":"fl","c":{"a":0,"k":[1,1,0,1]},"o":{"a":0,"k":100}}
    ])json";
    ParsedFixture pf = parseBytes(oneShapeLayerDoc(shapes));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    REQUIRE(scene.paths().size() == 1);
    CHECK(scene.paths()[0].pointCount == 2 * 2048);  // 1024 points x 2 vertices x (x, y).
    CHECK(hasDiagnostic(pf.sink, Diagnostic::Level::Warning, "clamped to 1024"));
}
