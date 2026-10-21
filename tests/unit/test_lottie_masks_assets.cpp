#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "doctest.h"
#include "test_temp.h"
#include "xt/compile/compile.h"
#include "xt/compile/ir_writer.h"
#include "xt/compile/lottie_parser.h"
#include "xt/ir.h"
#include "xt/ir_pod.h"
#include "xt/node.h"
#include "xt/property.h"
#include "xt/types.h"

/// Tests for T-16 (masksProperties, track matte, image assets, compileLottie). See
/// docs/tasks/T-16.md "測試".

using namespace xt;
using namespace xt::compile;

namespace {

std::string dataPath(const std::string& name) { return std::string(XT_TEST_DATA_DIR) + "/" + name; }

std::vector<uint8_t> loadFixture(const std::string& name) {
    std::ifstream in(dataPath(name), std::ios::binary);
    REQUIRE(in.good());
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<uint8_t> toBytes(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

struct ParsedFixture {
    IrWriter writer;
    DiagnosticSink sink;
    LottieResult result;
    std::vector<uint8_t> ir;
    Result<IrView> view = Result<IrView>::error(Status::kInternal);
};

ParsedFixture parseBytes(const std::vector<uint8_t>& bytes, bool strict = false, std::string rootDir = "") {
    ParsedFixture pf;
    LottieOptions opts;
    opts.strict = strict;
    opts.sourceName = "test.json";
    opts.rootDir = std::move(rootDir);
    pf.result = parseLottie(std::span<const uint8_t>(bytes.data(), bytes.size()), opts, pf.writer, pf.sink);
    pf.ir = pf.writer.finish();
    if (pf.result.ok) {
        pf.view = IrView::open(IrBlob{pf.ir.data(), pf.ir.size()});
    }
    return pf;
}

ParsedFixture parseFile(const std::string& name, bool strict = false, std::string rootDir = "") {
    return parseBytes(loadFixture(name), strict, std::move(rootDir));
}

/// Wraps a masksProperties JSON array (or "" for none) and a shapes JSON array into a one-layer
/// shape-layer document.
std::string oneMaskLayerDoc(const std::string& masksJson, const std::string& shapesJson = "[]") {
    std::string masksField =
        masksJson.empty() ? std::string() : (std::string(",\"masksProperties\":") + masksJson);
    return std::string(R"json({
      "v":"5.6.5","fr":30,"ip":0,"op":30,"w":100,"h":100,
      "layers":[
        {"ty":4,"ind":0,"nm":"Layer","ip":0,"op":30,"st":0,"sr":1,
         "ks":{"a":{"a":0,"k":[0,0,0]},"p":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]},
               "r":{"a":0,"k":0},"o":{"a":0,"k":100}})json") +
           masksField + R"json(,
         "shapes":)json" +
           shapesJson + "}]}";
}

const char* kSquarePt = R"json({"a":0,"k":{
    "c":true,"v":[[0,0],[10,0],[10,10],[0,10]],
    "i":[[0,0],[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0],[0,0]]
}})json";

}  // namespace

// -------------------------------------------------------------------------------------------
// masksProperties
// -------------------------------------------------------------------------------------------

TEST_CASE("lottie masks: single Add mask precedes shape content") {
    ParsedFixture pf = parseFile("mask_add.json");
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    // root(0), transform(1), content(2), mask(3), mask-path(4), rect-path(5).
    REQUIRE(scene.nodes().size() == 6);
    const IrSceneNode& maskNode = scene.nodes()[3];
    CHECK(maskNode.kind == static_cast<uint8_t>(NodeKind::Mask));
    CHECK(maskNode.parent == 2);
    CHECK(maskNode.maskMode == static_cast<uint8_t>(MaskMode::Add));

    const IrSceneNode& maskPathNode = scene.nodes()[4];
    CHECK(maskPathNode.kind == static_cast<uint8_t>(NodeKind::Path));
    CHECK(maskPathNode.parent == 3);
    REQUIRE(maskPathNode.kindIndex < scene.paths().size());
    const IrPathData& maskPath = scene.paths()[maskPathNode.kindIndex];
    CHECK(maskPath.fillKind == static_cast<uint8_t>(PaintKind::Solid));
    CHECK(maskPath.fillRule == static_cast<uint8_t>(FillRule::NonZero));
    CHECK(maskPath.fillColor.r == doctest::Approx(1.0f));
    CHECK(maskPath.fillColor.g == doctest::Approx(1.0f));
    CHECK(maskPath.fillColor.b == doctest::Approx(1.0f));
    CHECK(maskPath.fillColor.a == doctest::Approx(1.0f));  // o:100.
    CHECK(maskPath.verbCount == 6);                        // Move + 4 Cubic + Close (4-vertex closed path).

    const IrSceneNode& rectNode = scene.nodes()[5];
    CHECK(rectNode.kind == static_cast<uint8_t>(NodeKind::Path));
    CHECK(rectNode.parent == 2);  // Content, after the Mask node.
}

TEST_CASE("lottie masks: Add + Subtract masks are inserted in array order") {
    std::string masks = std::string("[") + R"json({"mode":"a","inv":false,"pt":)json" + kSquarePt +
                        R"json(,"o":{"a":0,"k":100}},)json" + R"json({"mode":"s","inv":false,"pt":)json" +
                        kSquarePt + R"json(,"o":{"a":0,"k":100}})json" + "]";
    ParsedFixture pf = parseBytes(toBytes(oneMaskLayerDoc(masks)));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    // root(0), transform(1), content(2), mask0(3), mask0-path(4), mask1(5), mask1-path(6).
    REQUIRE(scene.nodes().size() == 7);
    CHECK(scene.nodes()[3].kind == static_cast<uint8_t>(NodeKind::Mask));
    CHECK(scene.nodes()[3].maskMode == static_cast<uint8_t>(MaskMode::Add));
    CHECK(scene.nodes()[5].kind == static_cast<uint8_t>(NodeKind::Mask));
    CHECK(scene.nodes()[5].maskMode == static_cast<uint8_t>(MaskMode::Subtract));
}

TEST_CASE("lottie masks: inverted single Add mask becomes AlphaInverted") {
    std::string masks =
        std::string("[{\"mode\":\"a\",\"inv\":true,\"pt\":") + kSquarePt + ",\"o\":{\"a\":0,\"k\":100}}]";
    ParsedFixture pf = parseBytes(toBytes(oneMaskLayerDoc(masks)));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    CHECK(scene.nodes()[3].maskMode == static_cast<uint8_t>(MaskMode::AlphaInverted));
}

TEST_CASE("lottie masks: mode 'n' warns and produces no Mask node") {
    std::string masks =
        std::string("[{\"mode\":\"n\",\"inv\":false,\"pt\":") + kSquarePt + ",\"o\":{\"a\":0,\"k\":100}}]";
    ParsedFixture pf = parseBytes(toBytes(oneMaskLayerDoc(masks)));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    // root(0), transform(1), content(2) -- no Mask node, no shape content in this doc's shapes: [].
    REQUIRE(scene.nodes().size() == 3);
    bool warned = false;
    for (const Diagnostic& d : pf.sink.all()) {
        if (d.message.find("not supported") != std::string::npos) {
            warned = true;
        }
    }
    CHECK(warned);
}

TEST_CASE("lottie masks: mode 'f' warns and is skipped") {
    std::string masks =
        std::string("[{\"mode\":\"f\",\"inv\":false,\"pt\":") + kSquarePt + ",\"o\":{\"a\":0,\"k\":100}}]";
    ParsedFixture pf = parseBytes(toBytes(oneMaskLayerDoc(masks)));
    REQUIRE(pf.result.ok);
    CHECK(pf.sink.warningCount() > 0);
}

TEST_CASE("lottie masks: animated pt produces a PathData Track, animated o a FillOpacity Track") {
    std::string masks = std::string(R"json([{"mode":"a","inv":false,"pt":{"a":1,"k":[
        {"t":0,"s":[{"c":true,"v":[[0,0],[10,0],[10,10],[0,10]],"i":[[0,0],[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0],[0,0]]}]},
        {"t":10,"s":[{"c":true,"v":[[0,0],[20,0],[20,20],[0,20]],"i":[[0,0],[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0],[0,0]]}]}
    ]},"o":{"a":1,"k":[{"t":0,"s":[50]},{"t":10,"s":[100]}]}}])json");
    ParsedFixture pf = parseBytes(toBytes(oneMaskLayerDoc(masks)));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const TimelineChunk& tl = pf.view.value().timeline(pf.result.timeline);
    bool hasPathTrack = false;
    bool hasOpacityTrack = false;
    for (const IrTrack& t : tl.tracks()) {
        if (t.prop == prop::PathData) {
            hasPathTrack = true;
        }
        if (t.prop == prop::FillOpacity) {
            hasOpacityTrack = true;
        }
    }
    CHECK(hasPathTrack);
    CHECK(hasOpacityTrack);
}

// -------------------------------------------------------------------------------------------
// Track matte
// -------------------------------------------------------------------------------------------

TEST_CASE("lottie track matte: [B(td:1), A(tt:1), C] -> C, Mask(Alpha){B}, A at the scene root") {
    ParsedFixture pf = parseFile("mask_matte.json");
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    std::span<const IrSceneNode> nodes = scene.nodes();

    std::vector<uint32_t> rootChildren;
    for (uint32_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].parent == 0) {
            rootChildren.push_back(i);
        }
    }
    REQUIRE(rootChildren.size() == 3);  // C-transform, Mask, A-transform.
    CHECK(nodes[rootChildren[0]].kind == static_cast<uint8_t>(NodeKind::Group));
    CHECK(nodes[rootChildren[1]].kind == static_cast<uint8_t>(NodeKind::Mask));
    CHECK(nodes[rootChildren[1]].maskMode == static_cast<uint8_t>(MaskMode::Alpha));
    CHECK(nodes[rootChildren[2]].kind == static_cast<uint8_t>(NodeKind::Group));

    // B's subtree (TransformGroup + ContentGroup) is parented under the Mask node, not the root.
    uint32_t maskIdx = rootChildren[1];
    int bChildCount = 0;
    for (const IrSceneNode& n : nodes) {
        if (n.parent == maskIdx) {
            ++bChildCount;
        }
    }
    CHECK(bChildCount == 1);  // B's TransformGroup (its ContentGroup is B-transform's child, not the Mask's).
}

TEST_CASE("lottie track matte: tt:2/3/4 map to AlphaInverted/Luma/LumaInverted") {
    auto checkMode = [](int tt, MaskMode expected) {
        std::string doc = std::string(R"json({
          "v":"5.6.5","fr":30,"ip":0,"op":30,"w":100,"h":100,
          "layers":[
            {"ty":4,"ind":1,"nm":"B","ip":0,"op":30,"st":0,"sr":1,"td":1,
             "ks":{"a":{"a":0,"k":[0,0,0]},"p":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}},
             "shapes":[]},
            {"ty":4,"ind":2,"nm":"A","ip":0,"op":30,"st":0,"sr":1,"tt":)json") +
                          std::to_string(tt) + R"json(,
             "ks":{"a":{"a":0,"k":[0,0,0]},"p":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}},
             "shapes":[]}
          ]})json";
        ParsedFixture pf = parseBytes(toBytes(doc));
        REQUIRE(pf.result.ok);
        REQUIRE(pf.view.ok());
        const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
        bool found = false;
        for (const IrSceneNode& n : scene.nodes()) {
            if (n.kind == static_cast<uint8_t>(NodeKind::Mask)) {
                CHECK(n.maskMode == static_cast<uint8_t>(expected));
                found = true;
            }
        }
        CHECK(found);
    };
    checkMode(2, MaskMode::AlphaInverted);
    checkMode(3, MaskMode::Luma);
    checkMode(4, MaskMode::LumaInverted);
}

TEST_CASE("lottie track matte: tp selects a non-adjacent layer") {
    std::string doc = R"json({
      "v":"5.6.5","fr":30,"ip":0,"op":30,"w":100,"h":100,
      "layers":[
        {"ty":4,"ind":1,"nm":"B","ip":0,"op":30,"st":0,"sr":1,"td":1,
         "ks":{"a":{"a":0,"k":[0,0,0]},"p":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}},
         "shapes":[]},
        {"ty":4,"ind":2,"nm":"Middle","ip":0,"op":30,"st":0,"sr":1,
         "ks":{"a":{"a":0,"k":[0,0,0]},"p":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}},
         "shapes":[]},
        {"ty":4,"ind":3,"nm":"A","ip":0,"op":30,"st":0,"sr":1,"tt":1,"tp":1,
         "ks":{"a":{"a":0,"k":[0,0,0]},"p":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}},
         "shapes":[]}
      ]})json";
    ParsedFixture pf = parseBytes(toBytes(doc));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    int maskCount = 0;
    for (const IrSceneNode& n : scene.nodes()) {
        if (n.kind == static_cast<uint8_t>(NodeKind::Mask)) {
            ++maskCount;
        }
    }
    CHECK(maskCount == 1);
    // Middle must still be an ordinary top-level sibling (not consumed).
    int rootGroups = 0;
    for (const IrSceneNode& n : scene.nodes()) {
        if (n.parent == 0 && n.kind == static_cast<uint8_t>(NodeKind::Group)) {
            ++rootGroups;
        }
    }
    CHECK(rootGroups == 2);  // Middle-transform, A-transform (B is pulled into the Mask subtree).
}

TEST_CASE("lottie track matte: tt with no resolvable matte source warns and draws A normally") {
    std::string doc = R"json({
      "v":"5.6.5","fr":30,"ip":0,"op":30,"w":100,"h":100,
      "layers":[
        {"ty":4,"ind":0,"nm":"A","ip":0,"op":30,"st":0,"sr":1,"tt":1,
         "ks":{"a":{"a":0,"k":[0,0,0]},"p":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}},
         "shapes":[]}
      ]})json";
    ParsedFixture pf = parseBytes(toBytes(doc));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    CHECK(pf.sink.warningCount() > 0);
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    for (const IrSceneNode& n : scene.nodes()) {
        CHECK(n.kind != static_cast<uint8_t>(NodeKind::Mask));
    }
}

// -------------------------------------------------------------------------------------------
// Image assets
// -------------------------------------------------------------------------------------------

TEST_CASE("lottie image assets: external asset -> one ASST entry, correct Image node size") {
    ParsedFixture pf = parseFile("image_external.json", false, std::string(XT_TEST_DATA_DIR));
    REQUIRE(pf.result.ok);
    CHECK(pf.sink.warningCount() == 0);
    REQUIRE(pf.view.ok());
    std::span<const IrAsset> assets = pf.view.value().assets().assets();
    REQUIRE(assets.size() == 1);
    CHECK(pf.view.value().string(assets[0].name) == "img/pix.png");
    CHECK(assets[0].embedded == 0);

    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    REQUIRE(scene.images().size() == 1);
    CHECK(scene.images()[0].width == doctest::Approx(2.0f));
    CHECK(scene.images()[0].height == doctest::Approx(2.0f));
}

TEST_CASE("lottie image assets: missing rootDir warns \"not found\"") {
    ParsedFixture pf = parseFile("image_external.json", false, std::string());
    REQUIRE(pf.result.ok);
    bool warned = false;
    for (const Diagnostic& d : pf.sink.all()) {
        if (d.message.find("not found") != std::string::npos) {
            warned = true;
        }
    }
    // No rootDir configured means no existence check at all -- not covered explicitly by the
    // spec's wording ("rootDir 非空...file not found"); conservative choice: rootDir empty skips
    // the check rather than warning. See docs/tasks/T-16.md "圖片資產".
    CHECK_FALSE(warned);

    ParsedFixture pf2 =
        parseFile("image_external.json", false, std::string(XT_TEST_DATA_DIR) + "/does_not_exist");
    warned = false;
    for (const Diagnostic& d : pf2.sink.all()) {
        if (d.message.find("not found") != std::string::npos) {
            warned = true;
        }
    }
    CHECK(warned);
}

TEST_CASE("lottie image assets: embedded data URI decodes to the original PNG bytes") {
    ParsedFixture pf = parseFile("image_embedded.json");
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    std::span<const IrAsset> assets = pf.view.value().assets().assets();
    REQUIRE(assets.size() == 1);
    CHECK(assets[0].embedded == 1);
    std::span<const uint8_t> data = pf.view.value().assets().data(assets[0]);
    std::vector<uint8_t> original = loadFixture("img/pix.png");
    REQUIRE(data.size() == original.size());
    CHECK(std::equal(data.begin(), data.end(), original.begin()));
}

TEST_CASE("lottie image assets: the same asset id referenced by two Image layers -> one ASST entry") {
    std::string doc = std::string(R"json({
      "v":"5.6.5","fr":30,"ip":0,"op":30,"w":100,"h":100,
      "assets":[{"id":"image_0","w":2,"h":2,"u":"img/","p":"pix.png","e":0}],
      "layers":[
        {"ty":2,"ind":0,"nm":"Img1","ip":0,"op":30,"st":0,"sr":1,"refId":"image_0",
         "ks":{"a":{"a":0,"k":[0,0,0]},"p":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}}},
        {"ty":2,"ind":1,"nm":"Img2","ip":0,"op":30,"st":0,"sr":1,"refId":"image_0",
         "ks":{"a":{"a":0,"k":[0,0,0]},"p":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}}}
      ]})json");
    ParsedFixture pf = parseBytes(toBytes(doc));
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    CHECK(pf.view.value().assets().assets().size() == 1);
    const SceneChunk& scene = pf.view.value().scene(pf.result.scene);
    REQUIRE(scene.images().size() == 2);
    CHECK(scene.images()[0].asset == scene.images()[1].asset);
}

TEST_CASE("lottie image assets: a u + p that leaves rootDir is skipped with a warning (T-261a)") {
    // `<rootDir>/../img/pix.png` exists, so before T-261a it resolved and was registered.
    std::string doc = std::string(R"json({
      "v":"5.6.5","fr":30,"ip":0,"op":30,"w":100,"h":100,
      "assets":[{"id":"image_0","w":2,"h":2,"u":"../img/","p":"pix.png","e":0}],
      "layers":[
        {"ty":2,"ind":0,"nm":"Img1","ip":0,"op":30,"st":0,"sr":1,"refId":"image_0",
         "ks":{"a":{"a":0,"k":[0,0,0]},"p":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}}}
      ]})json");
    ParsedFixture pf = parseBytes(toBytes(doc), false, std::string(XT_TEST_DATA_DIR) + "/img");
    REQUIRE(pf.result.ok);
    REQUIRE(pf.view.ok());
    // The entry keeps only the asset id: nothing names, or was read from, the outside file.
    std::span<const IrAsset> assets = pf.view.value().assets().assets();
    REQUIRE(assets.size() == 1);
    CHECK(pf.view.value().string(assets[0].name) == "image_0");
    CHECK(assets[0].embedded == 0);
    bool warned = false;
    for (const Diagnostic& d : pf.sink.all()) {
        warned = warned || d.message.find("outside") != std::string::npos;
    }
    CHECK(warned);
}

// -------------------------------------------------------------------------------------------
// compileLottie
// -------------------------------------------------------------------------------------------

TEST_CASE("compileLottie: builds a 3-element DOCT (xt > body > anim) with autoplay/loop and META") {
    CompileOptions opts;
    CompileResult result = compileLottie(dataPath("shape_rect_fill.json"), opts);
    REQUIRE(result.ok());
    Result<IrView> view = IrView::open(IrBlob{result.ir.data(), result.ir.size()});
    REQUIRE(view.ok());
    REQUIRE(view.value().doc().elements().size() == 3);
    const IrDocElement& anim = view.value().doc().elements()[2];
    CHECK(view.value().string(anim.tag) == "anim");
    CHECK(anim.scene == 0);
    CHECK(view.value().meta().present());
}

TEST_CASE("compileLottie: strict + a file with warnings fails with the warning in diagnostics") {
    std::string masks =
        std::string("[{\"mode\":\"n\",\"inv\":false,\"pt\":") + kSquarePt + ",\"o\":{\"a\":0,\"k\":100}}]";
    std::string path = (xt::test::tempDir() / "xt_t16_mask_add_n_strict_tmp.json").string();
    {
        std::ofstream out(path, std::ios::binary);
        std::string doc = oneMaskLayerDoc(masks);
        out.write(doc.data(), static_cast<std::streamsize>(doc.size()));
    }
    CompileOptions opts;
    opts.strict = true;
    CompileResult result = compileLottie(path, opts);
    CHECK_FALSE(result.ok());
    bool hasWarning = false;
    for (const Diagnostic& d : result.diagnostics) {
        if (d.level == Diagnostic::Level::Warning) {
            hasWarning = true;
        }
    }
    CHECK(hasWarning);
}

TEST_CASE("compileLottie: nonexistent path is an error") {
    CompileOptions opts;
    CompileResult result = compileLottie(dataPath("does_not_exist.json"), opts);
    CHECK_FALSE(result.ok());
    REQUIRE(result.diagnostics.size() >= 1);
    CHECK(result.diagnostics.front().level == Diagnostic::Level::Error);
}

TEST_CASE("compileLottie: embedImages=true embeds an external image asset") {
    CompileOptions opts;
    opts.rootDir = std::string(XT_TEST_DATA_DIR);
    opts.embedImages = true;
    CompileResult result = compileLottie(dataPath("image_external.json"), opts);
    REQUIRE(result.ok());
    Result<IrView> view = IrView::open(IrBlob{result.ir.data(), result.ir.size()});
    REQUIRE(view.ok());
    std::span<const IrAsset> assets = view.value().assets().assets();
    REQUIRE(assets.size() == 1);
    CHECK(assets[0].embedded == 1);
}
