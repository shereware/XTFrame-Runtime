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
#include "xt/property.h"
#include "xt/timeline.h"
#include "xt/types.h"

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

/// Everything a positive test needs: the writer/sink used for the call, the parse result, and (on
/// success) the resulting IrView -- exercising the "IrWriter::finish() -> IrView::open succeeds"
/// integration point from every successful test case.
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

}  // namespace

TEST_CASE("lottie: empty document has just a root node") {
    ParsedFixture pf = parseBytes(loadFixture("min_empty.json"));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    REQUIRE(scene.nodes().size() == 1);
    CHECK(scene.nodes()[0].kind == static_cast<uint8_t>(NodeKind::Group));
    CHECK(scene.nodes()[0].parent == kInvalidNode);

    const TimelineChunk& tl = pf.view.value().timeline(pf.result.timeline);
    CHECK(tl.duration() == doctest::Approx(2.0f));
    CHECK(tl.frameRate() == doctest::Approx(30.0f));
}

TEST_CASE("lottie: static null layer produces two groups and no tracks") {
    ParsedFixture pf = parseBytes(loadFixture("min_null_static.json"));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    REQUIRE(scene.nodes().size() == 3);

    const IrSceneNode& xform = scene.nodes()[1];
    CHECK(xform.kind == static_cast<uint8_t>(NodeKind::Group));
    CHECK(xform.parent == 0);
    CHECK(xform.translate.x == doctest::Approx(10.0f));
    CHECK(xform.translate.y == doctest::Approx(20.0f));
    CHECK(xform.rotation == doctest::Approx(45.0f));

    const IrSceneNode& content = scene.nodes()[2];
    CHECK(content.kind == static_cast<uint8_t>(NodeKind::Group));
    CHECK(content.parent == 1);

    const TimelineChunk& tl = pf.view.value().timeline(pf.result.timeline);
    CHECK(tl.tracks().empty());
}

TEST_CASE("lottie: animated solid layer produces a rect path and Translate/Opacity tracks") {
    ParsedFixture pf = parseBytes(loadFixture("min_solid_anim.json"));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    REQUIRE(scene.nodes().size() == 4);  // root, transform, content, path

    const IrSceneNode& pathNode = scene.nodes()[3];
    CHECK(pathNode.kind == static_cast<uint8_t>(NodeKind::Path));
    CHECK(pathNode.parent == 2);
    REQUIRE(scene.paths().size() == 1);
    const IrPathData& path = scene.paths()[0];
    CHECK(path.verbCount == 5);  // Move + 3 Line + Close.
    CHECK(path.fillKind == static_cast<uint8_t>(PaintKind::Solid));
    CHECK(path.fillColor.r == doctest::Approx(1.0f));
    CHECK(path.fillColor.g == doctest::Approx(0.0f));
    CHECK(path.fillColor.b == doctest::Approx(0.0f));

    const TimelineChunk& tl = pf.view.value().timeline(pf.result.timeline);
    REQUIRE(tl.tracks().size() == 2);
    const IrTrack* translateTrack = nullptr;
    const IrTrack* opacityTrack = nullptr;
    for (const IrTrack& t : tl.tracks()) {
        if (t.prop == prop::Translate) {
            translateTrack = &t;
        } else if (t.prop == prop::Opacity) {
            opacityTrack = &t;
        }
    }

    REQUIRE(translateTrack != nullptr);
    CHECK(translateTrack->target == 1);  // TransformGroup.
    CHECK(translateTrack->type == static_cast<uint8_t>(ValueType::Vec2));
    REQUIRE(translateTrack->kfCount == 2);
    const IrKeyframe& tkf0 = tl.keyframes()[translateTrack->kfOffset];
    const IrKeyframe& tkf1 = tl.keyframes()[translateTrack->kfOffset + 1];
    CHECK(tkf0.time == doctest::Approx(0.0f));
    CHECK(tkf1.time == doctest::Approx(1.0f));  // sec(30) with fr=30, ip=0.
    CHECK(tkf0.easing.kind == static_cast<uint8_t>(EasingKind::CubicBezier));
    CHECK(tkf0.easing.x1 == doctest::Approx(0.42f));
    CHECK(tkf0.easing.y1 == doctest::Approx(0.0f));
    CHECK(tkf0.easing.x2 == doctest::Approx(0.667f));
    CHECK(tkf0.easing.y2 == doctest::Approx(1.0f));
    CHECK(irValueF32(tkf0.value, 0) == doctest::Approx(0.0f));
    CHECK(irValueF32(tkf1.value, 0) == doctest::Approx(100.0f));

    REQUIRE(opacityTrack != nullptr);
    CHECK(opacityTrack->target == 2);  // ContentGroup.
    REQUIRE(opacityTrack->kfCount == 2);
    const IrKeyframe& okf0 = tl.keyframes()[opacityTrack->kfOffset];
    const IrKeyframe& okf1 = tl.keyframes()[opacityTrack->kfOffset + 1];
    CHECK(okf0.easing.kind == static_cast<uint8_t>(EasingKind::Hold));
    CHECK(irValueF32(okf0.value, 0) == doctest::Approx(1.0f));
    CHECK(irValueF32(okf1.value, 0) == doctest::Approx(0.0f));
}

TEST_CASE("lottie: split position (p.s == true) produces TranslateX/TranslateY float tracks") {
    static const char* json = R"json({
      "v":"5.6.5","fr":30,"ip":0,"op":60,"w":100,"h":100,
      "layers":[
        {"ty":3,"ind":0,"nm":"N","ip":0,"op":60,"st":0,"sr":1,
         "ks":{"a":{"a":0,"k":[0,0,0]},
               "p":{"s":true,
                    "x":{"a":1,"k":[{"t":0,"s":[0]},{"t":10,"s":[5]}]},
                    "y":{"a":1,"k":[{"t":0,"s":[0]},{"t":10,"s":[8]}]}},
               "s":{"a":0,"k":[100,100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}}}
      ]
    })json";
    ParsedFixture pf = parseBytes(toBytes(json));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const TimelineChunk& tl = pf.view.value().timeline(pf.result.timeline);
    bool hasX = false, hasY = false;
    for (const IrTrack& t : tl.tracks()) {
        CHECK(t.prop != prop::Translate);
        if (t.prop == prop::TranslateX) {
            hasX = true;
            CHECK(t.type == static_cast<uint8_t>(ValueType::Float));
        } else if (t.prop == prop::TranslateY) {
            hasY = true;
            CHECK(t.type == static_cast<uint8_t>(ValueType::Float));
        }
    }
    CHECK(hasX);
    CHECK(hasY);
}

TEST_CASE("lottie: parent chain nests the child's TransformGroup under the parent's") {
    static const char* json = R"json({
      "v":"5.6.5","fr":30,"ip":0,"op":60,"w":100,"h":100,
      "layers":[
        {"ty":3,"ind":0,"nm":"A","ip":0,"op":60,"st":0,"sr":1,
         "ks":{"a":{"a":0,"k":[0,0,0]},"p":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}}},
        {"ty":3,"ind":1,"nm":"B","parent":0,"ip":0,"op":60,"st":0,"sr":1,
         "ks":{"a":{"a":0,"k":[0,0,0]},"p":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}}}
      ]
    })json";
    ParsedFixture pf = parseBytes(toBytes(json));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    // 0 root, 1 A-transform, 2 A-content, 3 B-transform, 4 B-content.
    REQUIRE(scene.nodes().size() == 5);
    CHECK(scene.nodes()[1].parent == 0);
    CHECK(scene.nodes()[2].parent == 1);
    CHECK(scene.nodes()[3].parent == 1);  // B's TransformGroup is a child of A's TransformGroup.
    CHECK(scene.nodes()[4].parent == 3);
    // A's ContentGroup (index 2) and B (index 3) are siblings under node 1; B comes after, so it
    // draws on top of A's own content.
    CHECK(2 < 3);
}

TEST_CASE("lottie: sibling layer order is reversed (Lottie's topmost layer drawn last)") {
    static const char* json = R"json({
      "v":"5.6.5","fr":30,"ip":0,"op":60,"w":100,"h":100,
      "layers":[
        {"ty":3,"ind":0,"nm":"L0","ip":0,"op":60,"st":0,"sr":1,
         "ks":{"a":{"a":0,"k":[0,0,0]},"p":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}}},
        {"ty":3,"ind":1,"nm":"L1","ip":0,"op":60,"st":0,"sr":1,
         "ks":{"a":{"a":0,"k":[0,0,0]},"p":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}}},
        {"ty":3,"ind":2,"nm":"L2","ip":0,"op":60,"st":0,"sr":1,
         "ks":{"a":{"a":0,"k":[0,0,0]},"p":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}}}
      ]
    })json";
    ParsedFixture pf = parseBytes(toBytes(json));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const IrView& view = pf.view.value();
    const SceneChunk& scene = view.scene(pf.result.scene);
    std::vector<std::string> rootChildNames;
    for (const IrSceneNode& n : scene.nodes()) {
        if (n.parent == 0) {
            rootChildNames.emplace_back(view.string(n.name));
        }
    }
    REQUIRE(rootChildNames.size() == 3);
    CHECK(rootChildNames[0] == "L2");
    CHECK(rootChildNames[1] == "L1");
    CHECK(rootChildNames[2] == "L0");
}

TEST_CASE("lottie: ip/op partial visibility produces a 3-keyframe Visible track") {
    static const char* json = R"json({
      "v":"5.6.5","fr":30,"ip":0,"op":60,"w":100,"h":100,
      "layers":[
        {"ty":3,"ind":0,"nm":"N","ip":15,"op":45,"st":0,"sr":1,
         "ks":{"a":{"a":0,"k":[0,0,0]},"p":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}}}
      ]
    })json";
    ParsedFixture pf = parseBytes(toBytes(json));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const TimelineChunk& tl = pf.view.value().timeline(pf.result.timeline);
    const IrTrack* visibleTrack = nullptr;
    for (const IrTrack& t : tl.tracks()) {
        if (t.prop == prop::Visible) {
            visibleTrack = &t;
        }
    }
    REQUIRE(visibleTrack != nullptr);
    CHECK(visibleTrack->type == static_cast<uint8_t>(ValueType::Enum));
    REQUIRE(visibleTrack->kfCount == 3);
    const IrKeyframe& kf0 = tl.keyframes()[visibleTrack->kfOffset];
    const IrKeyframe& kf1 = tl.keyframes()[visibleTrack->kfOffset + 1];
    const IrKeyframe& kf2 = tl.keyframes()[visibleTrack->kfOffset + 2];
    CHECK(kf0.time == doctest::Approx(0.0f));
    CHECK(irValueU32(kf0.value, 0) == 0);
    CHECK(kf1.time == doctest::Approx(0.5f));  // sec(15)
    CHECK(irValueU32(kf1.value, 0) == 1);
    CHECK(kf2.time == doctest::Approx(1.5f));  // sec(45)
    CHECK(irValueU32(kf2.value, 0) == 0);
    for (const IrKeyframe* kf : {&kf0, &kf1, &kf2}) {
        CHECK(kf->easing.kind == static_cast<uint8_t>(EasingKind::Hold));
    }
}

TEST_CASE("lottie: st/sr transform keyframe frames into sec(st + sr * f)") {
    static const char* json = R"json({
      "v":"5.6.5","fr":30,"ip":0,"op":60,"w":100,"h":100,
      "layers":[
        {"ty":3,"ind":0,"nm":"N","ip":0,"op":60,"st":15,"sr":2,
         "ks":{"a":{"a":0,"k":[0,0,0]},
               "p":{"a":1,"k":[{"t":0,"s":[0,0,0]},{"t":10,"s":[5,5,0]}]},
               "s":{"a":0,"k":[100,100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}}}
      ]
    })json";
    ParsedFixture pf = parseBytes(toBytes(json));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const TimelineChunk& tl = pf.view.value().timeline(pf.result.timeline);
    const IrTrack* translateTrack = nullptr;
    for (const IrTrack& t : tl.tracks()) {
        if (t.prop == prop::Translate) {
            translateTrack = &t;
        }
    }
    REQUIRE(translateTrack != nullptr);
    REQUIRE(translateTrack->kfCount == 2);
    const IrKeyframe& kf0 = tl.keyframes()[translateTrack->kfOffset];
    const IrKeyframe& kf1 = tl.keyframes()[translateTrack->kfOffset + 1];
    CHECK(kf0.time == doctest::Approx((15.0f + 2.0f * 0.0f) / 30.0f));
    CHECK(kf1.time == doctest::Approx((15.0f + 2.0f * 10.0f) / 30.0f));
}

TEST_CASE("lottie: two layers referencing the same precomp produce independent nested nodes") {
    static const char* json = R"json({
      "v":"5.6.5","fr":30,"ip":0,"op":60,"w":100,"h":100,
      "assets":[
        {"id":"comp1","layers":[
          {"ty":3,"ind":0,"nm":"Inner","ip":0,"op":60,"st":5,"sr":1,
           "ks":{"a":{"a":0,"k":[0,0,0]},
                 "p":{"a":1,"k":[{"t":0,"s":[0,0,0]},{"t":10,"s":[50,50,0]}]},
                 "s":{"a":0,"k":[100,100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}}}
        ]}
      ],
      "layers":[
        {"ty":0,"ind":0,"nm":"P1","refId":"comp1","w":100,"h":100,"ip":0,"op":60,"st":10,"sr":1,
         "ks":{"a":{"a":0,"k":[0,0,0]},"p":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}}},
        {"ty":0,"ind":1,"nm":"P2","refId":"comp1","w":100,"h":100,"ip":0,"op":60,"st":0,"sr":1,
         "ks":{"a":{"a":0,"k":[0,0,0]},"p":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}}}
      ]
    })json";
    ParsedFixture pf = parseBytes(toBytes(json));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    // root + (P-transform, P-content, Inner-transform, Inner-content) x 2 instances.
    CHECK(scene.nodes().size() == 9);

    const TimelineChunk& tl = pf.view.value().timeline(pf.result.timeline);
    std::vector<std::pair<float, float>> pairs;
    for (const IrTrack& t : tl.tracks()) {
        if (t.prop == prop::Translate && t.kfCount == 2) {
            const IrKeyframe& kf0 = tl.keyframes()[t.kfOffset];
            const IrKeyframe& kf1 = tl.keyframes()[t.kfOffset + 1];
            pairs.emplace_back(kf0.time, kf1.time);
        }
    }
    REQUIRE(pairs.size() == 2);
    // P1 (st=10): inner ownMap = {1, 10 + 5} -> sec(15)=0.5, sec(25)=0.8333.
    // P2 (st=0): inner ownMap = {1, 0 + 5} -> sec(5)=0.1667, sec(15)=0.5.
    bool foundP1 = false, foundP2 = false;
    for (const auto& p : pairs) {
        if (p.first == doctest::Approx(0.5f) && p.second == doctest::Approx(25.0f / 30.0f)) {
            foundP1 = true;
        }
        if (p.first == doctest::Approx(5.0f / 30.0f) && p.second == doctest::Approx(0.5f)) {
            foundP2 = true;
        }
    }
    CHECK(foundP1);
    CHECK(foundP2);
}

TEST_CASE("lottie: two markers become Timeline markers") {
    static const char* json = R"json({
      "v":"5.6.5","fr":30,"ip":0,"op":60,"w":100,"h":100,
      "markers":[{"cm":"start","tm":0,"dr":0},{"cm":"mid","tm":30,"dr":15}],
      "layers":[]
    })json";
    ParsedFixture pf = parseBytes(toBytes(json));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const IrView& view = pf.view.value();
    const TimelineChunk& tl = view.timeline(pf.result.timeline);
    REQUIRE(tl.markers().size() == 2);
    CHECK(view.string(tl.markers()[0].name) == "start");
    CHECK(tl.markers()[0].start == doctest::Approx(0.0f));
    CHECK(tl.markers()[0].duration == doctest::Approx(0.0f));
    CHECK(view.string(tl.markers()[1].name) == "mid");
    CHECK(tl.markers()[1].start == doctest::Approx(1.0f));
    CHECK(tl.markers()[1].duration == doctest::Approx(0.5f));
}

TEST_CASE("lottie: a missing \"fr\" field is an error and leaves the writer unmodified") {
    IrWriter writer;
    std::vector<uint8_t> emptyBytes = writer.finish();

    static const char* json = R"json({"v":"5.6.5","ip":0,"op":60,"w":100,"h":100,"layers":[]})json";
    std::vector<uint8_t> bytes = toBytes(json);
    LottieOptions opts;
    opts.sourceName = "bad.json";
    DiagnosticSink sink;
    LottieResult result =
        parseLottie(std::span<const uint8_t>(bytes.data(), bytes.size()), opts, writer, sink);

    CHECK_FALSE(result.ok);
    CHECK(sink.hasErrors());
    CHECK(writer.finish() == emptyBytes);
}

TEST_CASE("lottie: a parent cycle is an error") {
    static const char* json = R"json({
      "v":"5.6.5","fr":30,"ip":0,"op":60,"w":100,"h":100,
      "layers":[
        {"ty":3,"ind":0,"nm":"A","parent":1,"ip":0,"op":60,"st":0,"sr":1,
         "ks":{"a":{"a":0,"k":[0,0,0]},"p":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}}},
        {"ty":3,"ind":1,"nm":"B","parent":0,"ip":0,"op":60,"st":0,"sr":1,
         "ks":{"a":{"a":0,"k":[0,0,0]},"p":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}}}
      ]
    })json";
    ParsedFixture pf = parseBytes(toBytes(json));
    CHECK_FALSE(pf.result.ok);
    CHECK(pf.sink.hasErrors());
}

TEST_CASE("lottie: text layers warn and are skipped; --strict turns the warning into a failure") {
    static const char* json = R"json({
      "v":"5.6.5","fr":30,"ip":0,"op":60,"w":100,"h":100,
      "layers":[{"ty":5,"ind":0,"nm":"Title","ip":0,"op":60,"st":0,"sr":1}]
    })json";
    {
        ParsedFixture pf = parseBytes(toBytes(json), /*strict=*/false);
        CHECK(pf.result.ok);
        CHECK(pf.sink.warningCount() == 1);
        REQUIRE(pf.view.ok());
        CHECK(pf.view.value().scene(pf.result.scene).nodes().size() == 1);  // Only the root: layer skipped.
    }
    {
        ParsedFixture pf = parseBytes(toBytes(json), /*strict=*/true);
        CHECK_FALSE(pf.result.ok);
    }
}

TEST_CASE("lottie: a precomp referencing an unknown asset is an error") {
    static const char* json = R"json({
      "v":"5.6.5","fr":30,"ip":0,"op":60,"w":100,"h":100,
      "layers":[
        {"ty":0,"ind":0,"nm":"P","refId":"missing","w":100,"h":100,"ip":0,"op":60,"st":0,"sr":1,
         "ks":{"a":{"a":0,"k":[0,0,0]},"p":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}}}
      ]
    })json";
    ParsedFixture pf = parseBytes(toBytes(json));
    CHECK_FALSE(pf.result.ok);
    CHECK(pf.sink.hasErrors());
}

TEST_CASE("lottie: malformed JSON is an error") {
    static const char* json = "{ this is not valid json ";
    ParsedFixture pf = parseBytes(toBytes(json));
    CHECK_FALSE(pf.result.ok);
    CHECK(pf.sink.hasErrors());
}

// -------------------------------------------------------------------------------------------
// T-47b: ddd:1 3D layers, camera layers (ty:13). See Docs/design/19-3d-space.md, Docs/tasks/T-47b.md.
// -------------------------------------------------------------------------------------------

TEST_CASE("lottie: ddd:1 layer sets is3D and negates z/rotationX/rotationY (not rotation Z)") {
    ParsedFixture pf = parseBytes(loadFixture("ddd_layer.json"));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    REQUIRE(scene.nodes().size() == 3);  // root, transform, content (Null layer has no own content).
    const IrSceneNode& t = scene.nodes()[1];
    CHECK(t.is3D == 1);
    CHECK(t.translateZ == doctest::Approx(-30.0f));    // p.z = 30, negated.
    CHECK(t.rotationX == doctest::Approx(-10.0f));     // rx = 10, negated.
    CHECK(t.rotationY == doctest::Approx(-20.0f));     // ry = 20, negated.
    CHECK(t.orientation.x == doctest::Approx(-1.0f));  // or.x negated.
    CHECK(t.orientation.y == doctest::Approx(-2.0f));  // or.y negated.
    CHECK(t.orientation.z == doctest::Approx(3.0f));   // or.z unchanged.
}

TEST_CASE("lottie: camera layer (ty:13) produces a Camera scene node with position/zoom/target") {
    ParsedFixture pf = parseBytes(loadFixture("camera_layer.json"));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    REQUIRE(scene.nodes().size() == 2);  // root, camera (no TransformGroup/ContentGroup pair).
    const IrSceneNode& cam = scene.nodes()[1];
    CHECK(cam.kind == static_cast<uint8_t>(NodeKind::Camera));
    CHECK(cam.parent == 0);
    CHECK(cam.translate.x == doctest::Approx(200.0f));
    CHECK(cam.translate.y == doctest::Approx(150.0f));
    CHECK(cam.translateZ == doctest::Approx(800.0f));  // p.z = -800, negated.

    REQUIRE(scene.cameras().size() == 1);
    const IrCameraData& data = scene.cameras()[cam.kindIndex];
    CHECK(data.zoom == doctest::Approx(800.0f));
    CHECK(data.useTarget == 1);
    CHECK(data.position.x == doctest::Approx(200.0f));
    CHECK(data.position.y == doctest::Approx(150.0f));
    CHECK(data.position.z == doctest::Approx(800.0f));  // p.z = -800, negated (T-47b).
    CHECK(data.target.x == doctest::Approx(200.0f));
    CHECK(data.target.y == doctest::Approx(150.0f));
    CHECK(data.target.z == doctest::Approx(0.0f));  // a.z = 0, negated.
}

TEST_CASE("lottie: animated camera zoom/position produce CameraZoom and Translate3 tracks") {
    ParsedFixture pf = parseBytes(loadFixture("camera_anim.json"));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const TimelineChunk& tl = pf.view.value().timeline(pf.result.timeline);
    const IrTrack* zoomTrack = nullptr;
    const IrTrack* posTrack = nullptr;
    for (const IrTrack& t : tl.tracks()) {
        if (t.prop == prop::CameraZoom) {
            zoomTrack = &t;
        } else if (t.prop == prop::Translate3) {
            posTrack = &t;
        }
    }

    REQUIRE(zoomTrack != nullptr);
    CHECK(zoomTrack->type == static_cast<uint8_t>(ValueType::Float));
    REQUIRE(zoomTrack->kfCount == 2);
    CHECK(irValueF32(tl.keyframes()[zoomTrack->kfOffset].value, 0) == doctest::Approx(800.0f));
    CHECK(irValueF32(tl.keyframes()[zoomTrack->kfOffset + 1].value, 0) == doctest::Approx(1000.0f));

    REQUIRE(posTrack != nullptr);
    CHECK(posTrack->type == static_cast<uint8_t>(ValueType::Vec3));
    REQUIRE(posTrack->kfCount == 2);
    const IrKeyframe& kf0 = tl.keyframes()[posTrack->kfOffset];
    CHECK(irValueF32(kf0.value, 0) == doctest::Approx(200.0f));
    CHECK(irValueF32(kf0.value, 1) == doctest::Approx(150.0f));
    CHECK(irValueF32(kf0.value, 2) == doctest::Approx(800.0f));  // -(-800).
    CHECK(kf0.hasSpatial == 1);
    CHECK(kf0.tanOut.x == doctest::Approx(10.0f));
    CHECK(kf0.tanOut.z == doctest::Approx(-20.0f));  // to.z = 20, negated.
    CHECK(kf0.tanIn.z == doctest::Approx(20.0f));    // ti.z = -20, negated.
}

// -------------------------------------------------------------------------------------------
// T-55: Lottie ef[] -> Blur/DropShadow effects (Docs/design/11-lottie-mapping.md,
// Docs/design/18-effects.md §18.9). See Docs/tasks/T-55.md "測試".
// -------------------------------------------------------------------------------------------

TEST_CASE("lottie: ef[] Gaussian Blur (ty:29) maps to a Blur effect, unsupported ty warns once") {
    ParsedFixture pf = parseBytes(loadFixture("fx_blur.json"));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    CHECK(pf.sink.warningCount() == 1);  // The fixture's second ef[] entry, ty:20 (Tint).

    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    REQUIRE(scene.nodes().size() == 3);
    const IrSceneNode& content = scene.nodes()[2];
    REQUIRE(content.effectCount == 1);

    const IrEffect& eff = scene.effects()[content.effectOffset];
    CHECK(eff.kind == static_cast<uint8_t>(EffectKind::Blur));
    CHECK(eff.enabled == 1);
    CHECK(eff.params[0] == doctest::Approx(10.0f));  // b/2 = 20/2.
    CHECK(eff.params[0] == eff.params[1]);           // Dimensions 1 (both).
}

TEST_CASE("lottie: ef[] Drop Shadow (ty:25) maps offset/blur/opacity/color per 18.9's formula") {
    ParsedFixture pf = parseBytes(loadFixture("fx_shadow.json"));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());

    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    REQUIRE(scene.nodes().size() == 3);
    const IrSceneNode& content = scene.nodes()[2];
    REQUIRE(content.effectCount == 1);

    const IrEffect& eff = scene.effects()[content.effectOffset];
    CHECK(eff.kind == static_cast<uint8_t>(EffectKind::DropShadow));
    // direction 135 deg, distance 10 -> offset ~= (7.07, 7.07).
    CHECK(eff.params[0] == doctest::Approx(7.0710678f));
    CHECK(eff.params[1] == doctest::Approx(7.0710678f));
    CHECK(eff.params[2] == doctest::Approx(5.0f));  // softness -> blur.
    CHECK(eff.params[3] == doctest::Approx(0.0f));  // spread always 0.
    CHECK(eff.params[4] == doctest::Approx(1.0f));  // opacity 255/255.
    CHECK(eff.colors[0].r == doctest::Approx(0.0f));
    CHECK(eff.colors[0].a == doctest::Approx(1.0f));
}

TEST_CASE("lottie: ef[] Drop Shadow distance animation produces an effectParam(0,18) Vec2 track") {
    ParsedFixture pf = parseBytes(loadFixture("fx_shadow_anim.json"));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());

    const TimelineChunk& tl = pf.view.value().timeline(pf.result.timeline);
    const IrTrack* offsetTrack = nullptr;
    for (const IrTrack& t : tl.tracks()) {
        if (t.prop == effectParam(0, prop::kEffectParamOffset)) {
            offsetTrack = &t;
        }
    }
    REQUIRE(offsetTrack != nullptr);
    CHECK(offsetTrack->type == static_cast<uint8_t>(ValueType::Vec2));
    REQUIRE(offsetTrack->kfCount == 2);

    // Direction is static at 180 deg (straight down); distance animates 0 -> 10 over frame 0 -> 30
    // (0s -> 1s at fr=30). offsetX = dist*sin(180) ~= 0; offsetY = -dist*cos(180) = dist.
    const IrKeyframe& kf0 = tl.keyframes()[offsetTrack->kfOffset];
    const IrKeyframe& kf1 = tl.keyframes()[offsetTrack->kfOffset + 1];
    CHECK(kf0.time == doctest::Approx(0.0f));
    CHECK(irValueF32(kf0.value, 0) == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(irValueF32(kf0.value, 1) == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(kf1.time == doctest::Approx(1.0f));
    CHECK(irValueF32(kf1.value, 0) == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(irValueF32(kf1.value, 1) == doctest::Approx(10.0f));
}
