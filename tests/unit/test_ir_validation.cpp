#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "doctest.h"
#include "event_dispatcher.h"  // core/src: hitTestDetailed (T-255b).
#include "xt/animator.h"
#include "xt/asset_resolver.h"
#include "xt/baked.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/ir_writer.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/layout.h"
#include "xt/node_arena.h"
#include "xt/property.h"
#include "xt/sw/sw_renderer.h"
#include "xt/timeline.h"

/// T-255 (review 2026-10-02, task 2): load-time validation of malformed IR / `.xtcl` input that
/// used to pass IrView::open (or BakedPlayer::load) and then crash, overrun or misbehave at run
/// time, plus the run-time guards for the same values reaching the engine through its API.

using namespace xt;
using namespace xt::compile;

namespace {

// StyleProp ids (docs/design/05-style-and-css.md §5.4) of the pool-list properties.
constexpr uint16_t kFontFamily = 172;
constexpr uint16_t kTransition = 180;
constexpr uint16_t kEffect = 187;
constexpr uint16_t kTextEffect = 192;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

Status openStatus(const std::vector<uint8_t>& bytes) {
    return IrView::open(IrBlob{bytes.data(), bytes.size()}, IrReadOptions{false}).status();
}

IrDecl listRefDecl(uint16_t prop, uint32_t offset, uint32_t count) {
    IrDecl d{};
    d.prop = prop;
    d.valueType = static_cast<uint8_t>(ValueType::ListRef);
    irSetValueU32(d.value, 0, offset);
    irSetValueU32(d.value, 1, count);
    return d;
}

IrDecl lengthDecl(uint16_t prop, float value, uint8_t unit) {
    IrDecl d{};
    d.prop = prop;
    d.valueType = static_cast<uint8_t>(ValueType::Length);
    irSetValueF32(d.value, 0, value);
    d.value[4] = unit;
    return d;
}

IrEffect blurEffect() {
    IrEffect fx{};
    fx.kind = 0;
    fx.enabled = 1;
    fx.shader = kInvalidAsset;
    return fx;
}

IrDocElement element(uint32_t parent, StringId tag) {
    IrDocElement el{};
    el.parent = parent;
    el.tag = tag;
    el.id = kInvalidString;
    el.scene = 0xFFFFFFFFu;
    el.firstTextChild = 0xFFFFFFFFu;
    el.cameraIndex = 0xFFFFFFFFu;
    return el;
}

/// An IR whose STYL holds `effectCount` plain effects and the single declaration `decl`.
std::vector<uint8_t> styleIr(const IrDecl& decl, uint32_t effectCount = 0,
                             std::vector<float> floatPool = {}) {
    IrWriter w;
    StyleBuilder style;
    for (uint32_t i = 0; i < effectCount; ++i) {
        style.effects.push_back(blurEffect());
    }
    style.floatPool = std::move(floatPool);
    style.decls.push_back(decl);
    w.setStyle(std::move(style));
    return w.finish();
}

/// One scene: a Group root with one Path child, an empty path; `edit` adjusts the builder.
template <class Edit>
std::vector<uint8_t> sceneIr(Edit edit) {
    IrWriter w;
    SceneBuilder scene;
    IrSceneNode root{};
    root.kind = static_cast<uint8_t>(NodeKind::Group);
    root.visible = 1;
    root.parent = 0xFFFFFFFFu;
    root.opacity = 1;
    root.scale = Vec2{1, 1};
    root.name = kInvalidString;
    scene.nodes.push_back(root);
    IrSceneNode pathNode = root;
    pathNode.kind = static_cast<uint8_t>(NodeKind::Path);
    pathNode.parent = 0;
    pathNode.kindIndex = 0;
    scene.nodes.push_back(pathNode);
    IrPathData path{};
    path.miterLimit = 4.0f;
    path.trimEnd = 1.0f;
    scene.paths.push_back(path);
    edit(scene);
    w.addScene(std::move(scene));
    return w.finish();
}

/// One timeline with one track of `type` holding `keyframes` (and `floatPool`).
std::vector<uint8_t> timelineIr(ValueType type, std::vector<IrKeyframe> keyframes,
                                std::vector<float> floatPool = {}) {
    IrWriter w;
    TimelineBuilder tl;
    tl.name = w.intern("tl");
    tl.duration = 1.0f;
    tl.frameRate = 60.0f;
    IrTrack track{};
    track.target = 0;
    // T-257b: a property that takes `type` (IrView::validate checks keyframe values against it).
    track.prop = type == ValueType::Path     ? prop::PathData
                 : type == ValueType::Length ? prop::Width
                                             : prop::Opacity;
    track.type = static_cast<uint8_t>(type);
    track.kfOffset = 0;
    track.kfCount = static_cast<uint32_t>(keyframes.size());
    tl.tracks.push_back(track);
    tl.keyframes = std::move(keyframes);
    tl.floatPool = std::move(floatPool);
    w.addTimeline(std::move(tl));
    return w.finish();
}

IrKeyframe floatKeyframe(float time, float value) {
    IrKeyframe kf{};
    kf.time = time;
    irSetValueF32(kf.value, 0, value);
    return kf;
}

/// A Path keyframe at time `time`: points (offset, count) and own verbs (offset, count).
IrKeyframe pathKeyframe(float time, uint32_t pointOffset, uint32_t pointCount, uint32_t verbOffset,
                        uint32_t verbCount) {
    IrKeyframe kf{};
    kf.time = time;
    irSetValueU32(kf.value, 0, pointOffset);
    irSetValueU32(kf.value, 1, pointCount);
    irSetValueU32(kf.value, 2, verbOffset);
    irSetValueU32(kf.value, 3, verbCount);
    return kf;
}

/// A document of one root element carrying `data-xt-asset="<value>"`, with `assetCount` assets.
std::vector<uint8_t> assetAttrIr(std::string_view value, uint32_t assetCount) {
    IrWriter w;
    for (uint32_t i = 0; i < assetCount; ++i) {
        AssetBuilder asset;
        asset.name = w.intern("img" + std::to_string(i) + ".png");
        asset.kind = 0;
        asset.embedded = {1, 2, 3};
        w.addAsset(asset);
    }
    DocBuilder doc;
    IrDocElement root = element(0xFFFFFFFFu, w.intern("img"));
    root.attrOffset = 0;
    root.attrCount = 1;
    doc.elements.push_back(root);
    doc.attrPool.push_back(IrAttr{w.intern("data-xt-asset"), w.intern(value)});
    w.setDoc(std::move(doc));
    return w.finish();
}

/// A chain of `count` elements, each the only child of the previous one.
std::vector<uint8_t> chainIr(uint32_t count) {
    IrWriter w;
    StringId div = w.intern("div");
    DocBuilder doc;
    for (uint32_t i = 0; i < count; ++i) {
        doc.elements.push_back(element(i == 0 ? 0xFFFFFFFFu : i - 1, div));
    }
    w.setDoc(std::move(doc));
    return w.finish();
}

}  // namespace

TEST_CASE("T-255 A1: an effect list longer than kMaxEffectSequence is rejected (STYL and SCEN)") {
    CHECK(openStatus(styleIr(listRefDecl(kEffect, 0, 16), 20)) == Status::kOk);
    CHECK(openStatus(styleIr(listRefDecl(kEffect, 0, 20), 20)) == Status::kInvalidIr);
    CHECK(openStatus(styleIr(listRefDecl(kTextEffect, 2, 17), 20)) == Status::kInvalidIr);

    auto withNodeEffects = [](uint32_t count) {
        return sceneIr([count](SceneBuilder& scene) {
            for (uint32_t i = 0; i < 20; ++i) {
                scene.effects.push_back(blurEffect());
            }
            scene.nodes[1].effectOffset = 0;
            scene.nodes[1].effectCount = count;
        });
    };
    CHECK(openStatus(withNodeEffects(16)) == Status::kOk);
    CHECK(openStatus(withNodeEffects(17)) == Status::kInvalidIr);
}

TEST_CASE("T-255 A2: a pool-list property must be a ListRef (or a var() reference)") {
    IrDecl transition{};
    transition.prop = kTransition;
    transition.valueType = static_cast<uint8_t>(ValueType::Float);
    irSetValueU32(transition.value, 0, 0xFFFFFFFFu);
    irSetValueU32(transition.value, 1, 2);
    CHECK(openStatus(styleIr(transition)) == Status::kInvalidIr);

    IrDecl family = transition;
    family.prop = kFontFamily;
    irSetValueU32(family.value, 0, 0);
    irSetValueU32(family.value, 1, 0xFFFFFFFFu);
    CHECK(openStatus(styleIr(family)) == Status::kInvalidIr);

    IrDecl effect = family;
    effect.prop = kEffect;
    effect.valueType = static_cast<uint8_t>(ValueType::Color);
    CHECK(openStatus(styleIr(effect, 20)) == Status::kInvalidIr);

    // A var() reference is substituted before the value is read, so it stays valid.
    {
        IrWriter w;
        IrDecl var{};
        var.prop = kTransition;
        var.valueType = kDeclVarRef;
        irSetValueU32(var.value, 0, w.intern("--t"));
        irSetValueU32(var.value, 1, kNoVarFallback);
        StyleBuilder style;
        style.decls.push_back(var);
        w.setStyle(std::move(style));
        CHECK(openStatus(w.finish()) == Status::kOk);
    }
    // A var() fallback is read as a plain declaration.
    {
        IrWriter w;
        w.addVarFallback(transition);
        CHECK(openStatus(w.finish()) == Status::kInvalidIr);
    }
}

TEST_CASE("T-255 A3: a Path keyframe's own verbs must account for exactly its points") {
    // Pool: 14 point floats, then the verbs Move, Cubic, Cubic (2 + 6 + 6 = 14 floats).
    std::vector<float> pool(14, 1.0f);
    pool.insert(pool.end(), {0.0f, 2.0f, 2.0f});
    CHECK(openStatus(timelineIr(ValueType::Path, {pathKeyframe(0, 0, 14, 14, 3)}, pool)) == Status::kOk);
    CHECK(openStatus(timelineIr(ValueType::Path, {pathKeyframe(0, 0, 2, 14, 3)}, pool)) ==
          Status::kInvalidIr);
    std::vector<float> badCode = pool;
    badCode[15] = 1.5f;
    CHECK(openStatus(timelineIr(ValueType::Path, {pathKeyframe(0, 0, 14, 14, 3)}, badCode)) ==
          Status::kInvalidIr);
    // No own verbs (count 0): the node's verbs apply, so the point count is not checked here.
    CHECK(openStatus(timelineIr(ValueType::Path, {pathKeyframe(0, 0, 2, 0, 0)}, pool)) == Status::kOk);
}

TEST_CASE("T-255 A3: a STYL `d` declaration's own verbs must account for exactly its points") {
    std::vector<float> pool(14, 1.0f);
    pool.insert(pool.end(), {0.0f, 2.0f, 2.0f});
    auto d = [](uint32_t points, uint32_t verbCount) {
        IrDecl decl = listRefDecl(prop::PathData, 0, points);
        irSetValueU32(decl.value, 2, 14);
        irSetValueU32(decl.value, 3, verbCount);
        return decl;
    };
    CHECK(openStatus(styleIr(d(14, 3), 0, pool)) == Status::kOk);
    CHECK(openStatus(styleIr(d(2, 3), 0, pool)) == Status::kInvalidIr);
    std::vector<float> badCode = pool;
    badCode[16] = 1e10f;
    CHECK(openStatus(styleIr(d(14, 3), 0, badCode)) == Status::kInvalidIr);
}

TEST_CASE("T-255 A3: setProperty ignores a Path value whose own verbs and points disagree") {
    NodeArena arena;
    NodeId node = arena.create(NodeKind::Path);
    PathNodeData& path = arena.path(node);
    path.verbs = {PathVerb::Move, PathVerb::Line};
    path.points = {0, 0, 10, 10};

    const float points[2] = {5, 5};
    const float verbs[3] = {0.0f, 2.0f, 2.0f};  // Move, Cubic, Cubic: 14 floats wanted.
    setProperty(arena, node, prop::PathData, Value::ofPathWithVerbs(points, 2, verbs, 3));
    CHECK(arena.path(node).verbs.size() == 2);
    CHECK(arena.path(node).points.size() == 4);

    const float line[4] = {1, 2, 3, 4};
    const float lineVerbs[2] = {0.0f, 1.0f};
    const float closeVerbs[3] = {0.0f, 1.0f, 3.0f};
    setProperty(arena, node, prop::PathData, Value::ofPathWithVerbs(line, 4, closeVerbs, 3));
    CHECK(arena.path(node).verbs.size() == 3);
    CHECK(arena.path(node).points[0] == 1.0f);
    setProperty(arena, node, prop::PathData, Value::ofPathWithVerbs(line, 4, lineVerbs, 2));
    CHECK(arena.path(node).verbs.size() == 2);
}

TEST_CASE("T-255 A4: a data-xt-asset id outside the ASST table is rejected") {
    CHECK(openStatus(assetAttrIr("0", 1)) == Status::kOk);
    CHECK(openStatus(assetAttrIr("none", 0)) == Status::kOk);
    CHECK(openStatus(assetAttrIr("4294967295", 0)) == Status::kOk);  // kInvalidAsset = no asset.
    CHECK(openStatus(assetAttrIr("1", 1)) == Status::kInvalidIr);
    CHECK(openStatus(assetAttrIr("4000000000", 0)) == Status::kInvalidIr);
}

TEST_CASE("T-255 A4 resolver: an asset id outside the ASST table never sizes the decode cache") {
    std::vector<uint8_t> bytes = assetAttrIr("0", 1);
    Result<IrView> view = IrView::open(IrBlob{bytes.data(), bytes.size()}, IrReadOptions{false});
    REQUIRE(view.ok());
    FileAssetResolver resolver(std::string("."), nullptr);
    CHECK(resolver.imageView(4000000000u, view.value()) == nullptr);
    CHECK(resolver.imageSize(4000000000u, view.value()).x == 0.0f);
}

TEST_CASE("T-255 A5: a .xtcl frameCount the file cannot hold is rejected before it allocates") {
    BakedHeader header{};
    std::memcpy(header.magic, "XTCL", 4);
    header.major = 1;
    header.minor = 4;
    header.fps = 30.0f;
    header.dpr = 1.0f;
    std::vector<uint8_t> bytes(sizeof(header));

    header.frameCount = 0;
    std::memcpy(bytes.data(), &header, sizeof(header));
    BakedPlayer empty;
    CHECK(empty.load(IrBlob{bytes.data(), bytes.size()}, false) == Status::kOk);

    header.frameCount = 1000000;
    std::memcpy(bytes.data(), &header, sizeof(header));
    BakedPlayer player;
    CHECK(player.load(IrBlob{bytes.data(), bytes.size()}, false) == Status::kInvalidIr);

    // One empty frame record (frameIndex 0, payloadLength 0) holds frameCount 1 but not 2.
    bytes.resize(sizeof(header) + 8, 0);
    header.frameCount = 2;
    std::memcpy(bytes.data(), &header, sizeof(header));
    CHECK(player.load(IrBlob{bytes.data(), bytes.size()}, false) == Status::kInvalidIr);
    header.frameCount = 1;
    std::memcpy(bytes.data(), &header, sizeof(header));
    CHECK(player.load(IrBlob{bytes.data(), bytes.size()}, false) == Status::kOk);
}

TEST_CASE("T-255 A8: a plain Length value's unit byte must be a LengthUnit up to UiScale") {
    const uint8_t uiScale = static_cast<uint8_t>(LengthUnit::UiScale);
    const uint8_t calc = static_cast<uint8_t>(LengthUnit::Calc);
    CHECK(openStatus(styleIr(lengthDecl(prop::Width, 10, uiScale))) == Status::kOk);
    CHECK(openStatus(styleIr(lengthDecl(prop::Width, 1e30f, calc))) == Status::kInvalidIr);
    CHECK(openStatus(styleIr(lengthDecl(prop::Width, 10, 200))) == Status::kInvalidIr);

    {
        IrWriter w;
        IrVar token{};
        token.name = w.intern("--w");
        token.valueType = static_cast<uint8_t>(ValueType::Length);
        irSetValueF32(token.value, 0, 4.0f);
        token.value[4] = calc;
        w.addVarToken(token);
        CHECK(openStatus(w.finish()) == Status::kInvalidIr);
    }

    IrKeyframe kf = floatKeyframe(0, 5);
    kf.value[4] = calc;
    CHECK(openStatus(timelineIr(ValueType::Length, {kf})) == Status::kInvalidIr);
    kf.value[4] = 0;
    CHECK(openStatus(timelineIr(ValueType::Length, {kf})) == Status::kOk);

    // The run-time conversion of a Calc length to its program index.
    CHECK(calcProgramIndex(3.0f) == 3u);
    CHECK(calcProgramIndex(kNaN) == 0xFFFFFFFFu);
    CHECK(calcProgramIndex(-1.0f) == 0xFFFFFFFFu);
    CHECK(calcProgramIndex(1e30f) == 0xFFFFFFFFu);
}

TEST_CASE("T-255 A9: scene paint counts above the uint16 run-time fields are rejected") {
    auto withCounts = [](uint32_t stops, uint32_t dashes) {
        return sceneIr([stops, dashes](SceneBuilder& scene) {
            scene.stopPool.resize(stops);
            scene.floatPool.resize(dashes);
            scene.paths[0].fillStopCount = stops;
            scene.paths[0].dashCount = dashes;
        });
    };
    CHECK(openStatus(withCounts(65535, 65535)) == Status::kOk);
    CHECK(openStatus(withCounts(65536, 0)) == Status::kInvalidIr);
    CHECK(openStatus(withCounts(0, 65536)) == Status::kInvalidIr);
}

TEST_CASE("T-255 A11: text runs must be grouped by ascending text, ascending start") {
    auto withRuns = [](std::vector<std::array<uint32_t, 3>> runs) {
        IrWriter w;
        DocBuilder doc;
        doc.elements.push_back(element(0xFFFFFFFFu, w.intern("p")));
        doc.texts.push_back(IrDocText{0, w.intern("Hello")});
        doc.texts.push_back(IrDocText{0, w.intern("World")});
        for (const auto& r : runs) {
            IrTextRun run{};
            run.text = r[0];
            run.start = r[1];
            run.end = r[2];
            run.inlineObject = 0xFFFFFFFFu;
            run.link = 0xFFFFFFFFu;
            doc.textRuns.push_back(run);
        }
        w.setDoc(std::move(doc));
        return w.finish();
    };
    CHECK(openStatus(withRuns({{0, 0, 2}, {0, 2, 5}, {1, 0, 5}})) == Status::kOk);
    CHECK(openStatus(withRuns({{1, 0, 5}, {0, 0, 2}})) == Status::kInvalidIr);
    CHECK(openStatus(withRuns({{0, 2, 5}, {0, 0, 2}})) == Status::kInvalidIr);
}

TEST_CASE("T-255 A12: a Custom scene effect must name an ASST entry") {
    auto withShader = [](uint32_t shader) {
        return sceneIr([shader](SceneBuilder& scene) {
            IrEffect fx = blurEffect();
            fx.kind = static_cast<uint8_t>(EffectKind::Custom);
            fx.shader = shader;
            scene.effects.push_back(fx);
        });
    };
    CHECK(openStatus(withShader(0xFFFFFFF0u)) == Status::kInvalidIr);
}

TEST_CASE("T-255 C4: non-finite keyframe times and Seek times are rejected at load") {
    CHECK(openStatus(timelineIr(ValueType::Float, {floatKeyframe(0, 1)})) == Status::kOk);
    CHECK(openStatus(timelineIr(ValueType::Float, {floatKeyframe(kNaN, 1)})) == Status::kInvalidIr);
    CHECK(openStatus(timelineIr(ValueType::Float, {floatKeyframe(0, 1), floatKeyframe(kInf, 2)})) ==
          Status::kInvalidIr);

    auto withSeek = [](float t) {
        IrWriter w;
        DocBuilder doc;
        doc.elements.push_back(element(0xFFFFFFFFu, w.intern("div")));
        StringId name = w.intern("intro");
        w.setDoc(std::move(doc));
        TriggerBuilder trig;
        trig.tables.push_back(IrTriggerTable{0, 0, 1});
        IrTrigger t0{};
        t0.event = static_cast<uint8_t>(TriggerEvent::Click);
        t0.arg1 = kInvalidString;
        t0.actionOffset = 0;
        t0.actionCount = 1;
        t0.headCount = 1;
        trig.triggers.push_back(t0);
        IrTriggerAction seek{};
        seek.kind = static_cast<uint8_t>(TriggerAction::Seek);
        seek.arg0 = name;
        seek.arg1 = kInvalidString;
        seek.arg2 = t;
        trig.actions.push_back(seek);
        w.setTriggers(std::move(trig));
        return w.finish();
    };
    CHECK(openStatus(withSeek(1.5f)) == Status::kOk);
    CHECK(openStatus(withSeek(kNaN)) == Status::kInvalidIr);
    CHECK(openStatus(withSeek(-kInf)) == Status::kInvalidIr);
}

TEST_CASE("T-255 C4 / T-255b: evalTrack reads NaN and -Inf as 0 and +Inf as the end") {
    Timeline tl;
    tl.duration = 2;
    for (float t : {0.0f, 1.0f, 2.0f}) {
        Keyframe kf;
        kf.time = t;
        kf.value = Value::ofFloat(t * 10.0f);
        kf.easing = Easing::linear();
        tl.keyframes.push_back(kf);
    }
    Track track;
    track.type = ValueType::Float;
    track.kfOffset = 0;
    track.kfCount = 3;
    tl.tracks.push_back(track);
    CHECK(evalTrack(tl, tl.tracks[0], kNaN, nullptr, nullptr).f == 0.0f);
    CHECK(evalTrack(tl, tl.tracks[0], -kInf, nullptr, nullptr).f == 0.0f);
    CHECK(evalTrack(tl, tl.tracks[0], kInf, nullptr, nullptr).f == 20.0f);  // The last keyframe.
}

TEST_CASE("T-255 C5: an element tree kMaxElementDepth deep is rejected") {
    CHECK(openStatus(chainIr(kMaxElementDepth)) ==
          Status::kOk);  // Deepest element at depth kMaxElementDepth - 1.
    CHECK(openStatus(chainIr(kMaxElementDepth + 1)) == Status::kInvalidIr);
}

TEST_CASE("T-255 C5: the compiler reports a symbol-expanded tree deeper than the runtime allows") {
    auto nest = [](int levels, const std::string& inner) {
        std::string s;
        for (int i = 0; i < levels; ++i) {
            s += "<div>";
        }
        s += inner;
        for (int i = 0; i < levels; ++i) {
            s += "</div>";
        }
        return s;
    };
    const std::string text =
        "<xt><head>"
        "<symbol name=\"S1\">" +
        nest(200, "<span>x</span>") +
        "</symbol>"
        "<symbol name=\"S2\">" +
        nest(200, "<use symbol=\"S1\" id=\"Inner\"/>") +
        "</symbol>"
        "<symbol name=\"S3\">" +
        nest(200, "<use symbol=\"S2\" id=\"Mid\"/>") +
        "</symbol>"
        "</head><body><use symbol=\"S3\" id=\"Outer\"/></body></xt>";
    std::array<SourceFile, 1> files{SourceFile{"doc.xt", text}};
    CompileResult result = compileDocumentFromStrings(files, "doc.xt", CompileOptions{});
    bool found = false;
    for (const Diagnostic& d : result.diagnostics) {
        found = found || (d.level == Diagnostic::Level::Error &&
                          d.message.find("nesting too deep") != std::string::npos);
    }
    CHECK(found);
    CHECK(!result.ok());
}

// =============================================================================================
// T-255b (review 2026-10-02 task 2, second half): what T-255 left, and the PR #266 review items.
// =============================================================================================

namespace {

std::vector<uint8_t> gradientIr(uint8_t unit) {
    IrWriter w;
    StyleBuilder style;
    IrGradient g{};
    g.kind = static_cast<uint8_t>(GradientKind::Radial);
    g.units[2] = unit;
    style.gradients.push_back(g);
    w.setStyle(std::move(style));
    return w.finish();
}

std::vector<uint8_t> compileXt(const std::string& text) {
    std::array<SourceFile, 1> files{SourceFile{"doc.xt", text}};
    CompileOptions options;
    options.emitMeta = false;
    CompileResult result = compileDocumentFromStrings(files, "doc.xt", options);
    for (const Diagnostic& d : result.diagnostics) {
        if (d.level == Diagnostic::Level::Error) {
            MESSAGE(d.message);
        }
    }
    return result.ok() ? result.ir : std::vector<uint8_t>{};
}

uint32_t depthOf(const NodeArena& arena, NodeId node) {
    uint32_t depth = 0;
    for (NodeId p = arena.parent(node); p != kInvalidNode; p = arena.parent(p)) {
        ++depth;
    }
    return depth;
}

/// The deepest-allowed chain (deepest element at kMaxElementDepth - 1, each a 4x4 painted box),
/// updated, rendered in software and hit tested. Records instead of asserting, so it can run on a
/// thread of its own.
struct DeepRun {
    const std::vector<uint8_t>* ir = nullptr;
    bool loaded = false;
    uint32_t hitDepth = 0;
    size_t commands = 0;
};

void runDeep(DeepRun& run) {
    ContextConfig config;
    Context ctx(config);
    Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{run.ir->data(), run.ir->size()}, false);
    if (!handle.ok()) {
        return;
    }
    run.loaded = true;
    ctx.setViewport(Rect{0, 0, 32, 32}, 1.0f);
    ctx.update(0.016f);
    CommandList list;
    ctx.render(list);
    run.commands = list.commands.size();
    std::vector<uint8_t> pixels(32u * 32u * 4u, 0);
    sw::SwRenderer renderer;
    renderer.setTarget(sw::SwTarget{pixels.data(), 32, 32, 32 * 4});
    renderer.render(list, nullptr, nullptr);
    Document* doc = ctx.document(handle.value());
    const NodeId hit = doc->hitTest(Vec2{1, 1});
    run.hitDepth = hit != kInvalidNode ? depthOf(doc->arena(), hit) : 0;
}

#if defined(_WIN32)
DWORD WINAPI deepThread(LPVOID arg) {
    runDeep(*static_cast<DeepRun*>(arg));
    return 0;
}
/// Runs runDeep() on a thread whose stack is `bytes` (the stack an engine job thread or an
/// Android main thread may have, smaller than a desktop main thread's).
bool runDeepOnStack(DeepRun& run, size_t bytes) {
    HANDLE thread =
        CreateThread(nullptr, bytes, deepThread, &run, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (thread == nullptr) {
        return false;
    }
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return true;
}
#else
void* deepThread(void* arg) {
    runDeep(*static_cast<DeepRun*>(arg));
    return nullptr;
}
bool runDeepOnStack(DeepRun& run, size_t bytes) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, bytes);
    pthread_t thread;
    const bool started = pthread_create(&thread, &attr, deepThread, &run) == 0;
    pthread_attr_destroy(&attr);
    if (started) {
        pthread_join(thread, nullptr);
    }
    return started;
}
#endif

/// chainIr(kMaxElementDepth) (the deepest tree the validator allows; the .xt parser itself stops
/// at 256 levels) with `div { width: 4px; height: 4px; background-color: red }`, so every level
/// lays out, paints and can be hit.
std::vector<uint8_t> deepStyledChainIr() {
    IrWriter w;
    StringId div = w.intern("div");
    StyleBuilder style;
    IrSelPart part{};
    part.tag = div;
    part.id = kInvalidString;
    style.selectorParts.push_back(part);
    style.decls.push_back(lengthDecl(prop::Width, 4, static_cast<uint8_t>(LengthUnit::Px)));
    style.decls.push_back(lengthDecl(prop::Height, 4, static_cast<uint8_t>(LengthUnit::Px)));
    IrDecl background{};
    background.prop = prop::BackgroundColor;
    background.valueType = static_cast<uint8_t>(ValueType::Color);
    irSetValueF32(background.value, 0, 1.0f);
    irSetValueF32(background.value, 3, 1.0f);
    style.decls.push_back(background);
    style.rules.push_back(IrRule{0, 1, 0, 3, 1});
    w.setStyle(std::move(style));
    DocBuilder doc;
    for (uint32_t i = 0; i < kMaxElementDepth; ++i) {
        doc.elements.push_back(element(i == 0 ? 0xFFFFFFFFu : i - 1, div));
    }
    w.setDoc(std::move(doc));
    return w.finish();
}

}  // namespace

TEST_CASE("T-255b: a gradient's params[] units are LengthUnits up to UiScale") {
    CHECK(openStatus(gradientIr(static_cast<uint8_t>(LengthUnit::UiScale))) == Status::kOk);
    CHECK(openStatus(gradientIr(static_cast<uint8_t>(LengthUnit::UiScale) + 1)) == Status::kInvalidIr);
    CHECK(openStatus(gradientIr(0xFF)) == Status::kInvalidIr);
}

TEST_CASE("T-255b: a font-size declaration is a Length, a calc() or a var() reference") {
    CHECK(openStatus(styleIr(lengthDecl(prop::FontSize, 12, static_cast<uint8_t>(LengthUnit::Px)))) ==
          Status::kOk);
    IrDecl asFloat{};
    asFloat.prop = prop::FontSize;
    asFloat.valueType = static_cast<uint8_t>(ValueType::Float);
    irSetValueF32(asFloat.value, 0, 12.0f);
    asFloat.value[4] = 0xEE;  // What the cascade would read as the unit.
    CHECK(openStatus(styleIr(asFloat)) == Status::kInvalidIr);
    IrDecl asColor = asFloat;
    asColor.valueType = static_cast<uint8_t>(ValueType::Color);
    CHECK(openStatus(styleIr(asColor)) == Status::kInvalidIr);
    // Any other property keeps its Float declaration.
    IrDecl opacity = asFloat;
    opacity.prop = prop::Opacity;
    CHECK(openStatus(styleIr(opacity)) == Status::kOk);
}

TEST_CASE("T-255b: seekTimeline reads +Inf as the group's end, NaN and -Inf as 0") {
    std::vector<uint8_t> ir = compileXt(R"(<xt><head><style>
      #Box { position: absolute; width: 10px; height: 10px; }
      @timeline main { duration: 1s; #Box { 0s { opacity: 0.25 } 1s { opacity: 0.75 } } }
    </style></head><body><div id="Box"></div></body></xt>)");
    REQUIRE(!ir.empty());
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{ir.data(), ir.size()}, false, nullptr, nullptr, doc) == Status::kOk);
    doc->setViewport(Rect{0, 0, 100, 100});
    doc->update(0.0f);
    const NodeId box = doc->byId("Box").node;
    auto opacityAfterSeek = [&](float t) {
        REQUIRE(doc->seekTimeline("", "main", t) == Status::kOk);
        doc->update(0.0f);
        return getProperty(doc->arena(), box, prop::Opacity).f;
    };
    CHECK(opacityAfterSeek(kInf) == doctest::Approx(0.75f));
    CHECK(opacityAfterSeek(kNaN) == doctest::Approx(0.25f));
    CHECK(opacityAfterSeek(kInf) == doctest::Approx(0.75f));
    CHECK(opacityAfterSeek(-kInf) == doctest::Approx(0.25f));
}

TEST_CASE("T-255b: Context::update reads a NaN or infinite dt as 0 and never runs the clock backwards") {
    ContextConfig config;
    Context ctx(config);
    ctx.update(0.5f);
    ctx.update(kNaN);
    CHECK(ctx.clock() == 0.5f);
    ctx.update(kInf);
    CHECK(ctx.clock() == 0.5f);
    ctx.update(-1.0f);
    CHECK(ctx.clock() == 0.5f);
    ctx.update(0.25f);
    CHECK(ctx.clock() == 0.75f);
}

TEST_CASE("T-255b: Document inserts never make a tree kMaxElementDepth deep") {
    std::vector<uint8_t> ir = chainIr(kMaxElementDepth);  // Deepest element at depth kMaxElementDepth - 1.
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{ir.data(), ir.size()}, false, nullptr, nullptr, doc) == Status::kOk);
    const NodeArena& arena = doc->arena();
    std::vector<ElementRef> chain;
    for (ElementRef e = doc->root(); e.valid(); e = e.firstChild()) {
        chain.push_back(e);
    }
    REQUIRE(chain.size() >= 3);
    REQUIRE(depthOf(arena, chain.back().node) == kMaxElementDepth - 1);
    const ElementRef secondDeepest = chain[chain.size() - 2];
    const ElementRef thirdDeepest = chain[chain.size() - 3];

    ElementRef leaf = doc->createElement("div");
    doc->appendChild(chain.back(), leaf);  // Would be at depth kMaxElementDepth.
    CHECK(arena.parent(leaf.node) == kInvalidNode);
    doc->appendChild(secondDeepest, leaf);  // Depth kMaxElementDepth - 1.
    CHECK(arena.parent(leaf.node) == secondDeepest.node);

    // A detached two-level subtree: its bottom would be at kMaxElementDepth under secondDeepest, one less
    // under thirdDeepest.
    ElementRef top = doc->createElement("div");
    ElementRef bottom = doc->createElement("div");
    doc->appendChild(top, bottom);
    REQUIRE(arena.parent(bottom.node) == top.node);
    doc->insertBefore(secondDeepest, top, leaf);
    CHECK(arena.parent(top.node) == kInvalidNode);
    doc->insertBefore(thirdDeepest, top, ElementRef{});
    CHECK(arena.parent(top.node) == thirdDeepest.node);
}

TEST_CASE("T-255b: hit testing descends no deeper than kMaxElementDepth") {
    NodeArena arena;
    std::vector<NodeId> chain;
    for (uint32_t i = 0; i < kMaxElementDepth + 8; ++i) {
        const NodeId n = arena.create(NodeKind::Element);
        arena.element(n).box.width = 10;
        arena.element(n).box.height = 10;
        if (!chain.empty()) {
            arena.insertBefore(chain.back(), n, kInvalidNode);
        }
        chain.push_back(n);
    }
    CHECK(hitTestDetailed(arena, chain[0], Vec2{1, 1}).node == chain[kMaxElementDepth - 1]);
}

TEST_CASE("T-255b: the deepest tree the validator allows updates, renders and hit tests") {
    const std::vector<uint8_t> ir = deepStyledChainIr();
    REQUIRE(!ir.empty());
    REQUIRE(openStatus(ir) == Status::kOk);
    {
        // The test thread's default stack (Debug build).
        DeepRun run;
        run.ir = &ir;
        runDeep(run);
        CHECK(run.loaded);
        CHECK(run.commands > kMaxElementDepth / 2);
        CHECK(run.hitDepth >= kMaxElementDepth - 2);
    }
    {
        // 512 KiB: smaller stacks (engine job threads, an Android main thread) must hold it too.
        DeepRun run;
        run.ir = &ir;
        REQUIRE(runDeepOnStack(run, 512u * 1024u));
        CHECK(run.loaded);
        CHECK(run.hitDepth >= kMaxElementDepth - 2);
    }
}

// =============================================================================================
// T-257b (fuzz_ir findings of the final merge gate): IR that passed IrView::open and then hit an
// XT_ASSERT in Document::update / load.
// =============================================================================================

namespace {

/// A document of one `anim` root element showing scene 0; `withTimeline` adds TLIN 0 (the scene's
/// timeline, Document::sceneTimeline()); a non-empty `autoplay` becomes its `autoplay` attribute.
std::vector<uint8_t> animIr(bool withTimeline, std::string_view autoplay = {}) {
    IrWriter w;
    SceneBuilder scene;
    scene.name = w.intern("s");
    IrSceneNode root{};
    root.kind = static_cast<uint8_t>(NodeKind::Group);
    root.visible = 1;
    root.parent = 0xFFFFFFFFu;
    root.opacity = 1;
    root.scale = Vec2{1, 1};
    root.name = kInvalidString;
    scene.nodes.push_back(root);
    w.addScene(std::move(scene));
    if (withTimeline) {
        TimelineBuilder tl;
        tl.name = w.intern("s");
        tl.duration = 1.0f;
        tl.frameRate = 60.0f;
        w.addTimeline(std::move(tl));
    }
    DocBuilder doc;
    IrDocElement el = element(0xFFFFFFFFu, w.intern("anim"));
    el.scene = 0;
    if (!autoplay.empty()) {
        el.attrOffset = 0;
        el.attrCount = 1;
        doc.attrPool.push_back(IrAttr{w.intern("autoplay"), w.intern(autoplay)});
    }
    doc.elements.push_back(el);
    w.setDoc(std::move(doc));
    return w.finish();
}

/// A document of one root `div` whose inline style is the single declaration `decl`.
std::vector<uint8_t> inlineDeclIr(const IrDecl& decl) {
    IrWriter w;
    DocBuilder doc;
    IrDocElement el = element(0xFFFFFFFFu, w.intern("div"));
    el.inlineDeclOffset = 0;
    el.inlineDeclCount = 1;
    doc.elements.push_back(el);
    doc.inlineDecls.push_back(decl);
    w.setDoc(std::move(doc));
    return w.finish();
}

IrDecl floatDecl(uint16_t prop, float value, uint8_t unitByte) {
    IrDecl d{};
    d.prop = prop;
    d.valueType = static_cast<uint8_t>(ValueType::Float);
    irSetValueF32(d.value, 0, value);
    d.value[4] = unitByte;
    return d;
}

std::vector<uint8_t> readRegression(const char* name) {
    // tests/fuzz/regressions/ir/ (what run_fuzz.sh replays first), next to XT_TEST_XT_DIR (tests/xt).
    const std::string path = std::string(XT_TEST_XT_DIR) + "/../fuzz/regressions/ir/" + name;
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/// tests/fuzz/fuzz_ir.cpp's sequence: open, then (only when that succeeds) load, one frame.
/// Returns the IrView::open status; `loaded` says whether the load step ran and succeeded. With
/// `iterationEvents`, a listener on the root element counts the AnimationIteration events of the
/// frame per animation instance id.
Status runFuzzIrSequence(const std::vector<uint8_t>& bytes, bool& loaded,
                         std::map<uint32_t, uint32_t>* iterationEvents = nullptr) {
    loaded = false;
    IrBlob blob{bytes.data(), bytes.size()};
    Result<IrView> view = IrView::open(blob);
    if (!view.ok()) {
        return view.status();
    }
    ContextConfig config;
    Context ctx(config);
    Result<DocumentHandle> handle = ctx.loadDocument(blob, /*takeOwnership=*/false);
    if (!handle.ok()) {
        return Status::kOk;
    }
    loaded = true;
    if (iterationEvents != nullptr) {
        Document* doc = ctx.document(handle.value());
        CHECK(doc != nullptr);
        ElementRef root = doc != nullptr ? doc->root() : ElementRef{};
        if (root.valid()) {
            doc->addListener(root, EventType::AnimationIteration,
                             [iterationEvents](Event& ev) { ++(*iterationEvents)[ev.animationId]; });
        }
    }
    ctx.setViewport(Rect{0, 0, 800, 600}, 1.0f);
    ctx.update(1.0f / 60.0f);
    CommandList cmds;
    ctx.render(cmds);
    return Status::kOk;
}

}  // namespace

TEST_CASE("T-257b: a declaration the cascade reads as a Length carries a unit it can resolve") {
    const uint8_t em = static_cast<uint8_t>(LengthUnit::Em);
    // The fuzz_ir finding: a Float-typed transform-origin whose unit byte says `em`.
    CHECK(openStatus(inlineDeclIr(floatDecl(prop::AnchorX, 2, em))) == Status::kInvalidIr);
    CHECK(openStatus(inlineDeclIr(floatDecl(prop::AnchorY, 2, 98))) == Status::kInvalidIr);
    CHECK(openStatus(styleIr(floatDecl(prop::AnchorX, 2, em))) == Status::kInvalidIr);
    CHECK(openStatus(styleIr(floatDecl(prop::TranslateX, 2, em))) == Status::kInvalidIr);
    CHECK(openStatus(styleIr(floatDecl(prop::PerspectiveOriginX, 2, em))) == Status::kInvalidIr);
    CHECK(openStatus(styleIr(floatDecl(prop::Width, 2, em))) == Status::kInvalidIr);
    CHECK(openStatus(styleIr(floatDecl(prop::LineHeight, 2, em))) == Status::kInvalidIr);
    // A plain Float with a zero unit byte (what the compiler writes, e.g. a scene-target AnchorX)
    // and a Length-typed `em` (resolved by the cascade) are fine.
    CHECK(openStatus(styleIr(floatDecl(prop::AnchorX, 2, 0))) == Status::kOk);
    CHECK(openStatus(inlineDeclIr(lengthDecl(prop::AnchorX, 2, em))) == Status::kOk);
    // A property the cascade reads as a Float keeps whatever its unused bytes hold.
    CHECK(openStatus(styleIr(floatDecl(prop::Opacity, 1, em))) == Status::kOk);
}

TEST_CASE("T-257b: a transform-origin in em resolves against the element's font size") {
    std::vector<uint8_t> ir = compileXt(R"(<xt><head><style>
      #Box { position: absolute; width: 100px; height: 50px; font-size: 10px;
             transform-origin: 2em 1em; }
    </style></head><body><div id="Box"></div></body></xt>)");
    REQUIRE(!ir.empty());
    REQUIRE(openStatus(ir) == Status::kOk);
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{ir.data(), ir.size()}, false, nullptr, nullptr, doc) == Status::kOk);
    doc->setViewport(Rect{0, 0, 200, 200});
    doc->update(0.0f);
    const NodeId box = doc->byId("Box").node;
    CHECK(doc->arena().common(box).anchor.x == doctest::Approx(20.0f));
    CHECK(doc->arena().common(box).anchor.y == doctest::Approx(10.0f));
}

TEST_CASE("T-257b: resolveLength and resolveHeightLength read an unresolved em as the fallback") {
    CHECK(resolveLength(Length{2, LengthUnit::Em}, 100, 7) == 7.0f);
    float out = 3;
    CHECK(!resolveHeightLength(Length{2, LengthUnit::Em}, 100, out));
    CHECK(out == 3.0f);
}

TEST_CASE("T-257b: an autoplaying anim element's scene timeline must exist") {
    CHECK(openStatus(animIr(false)) == Status::kInvalidIr);
    CHECK(openStatus(animIr(false, "true")) == Status::kInvalidIr);
    CHECK(openStatus(animIr(true)) == Status::kOk);
    // Not played at load: the sync / timeline-group paths check the timeline at run time.
    CHECK(openStatus(animIr(false, "false")) == Status::kOk);
}

TEST_CASE("T-257b: Animator::play on a missing timeline returns 0 and starts nothing") {
    Animator animator;
    PlayParams params;
    params.timeline = 5;
    CHECK(animator.play(params, 0.0f) == 0u);
    params.timeline = kInvalidTimeline;
    CHECK(animator.play(params, 0.0f) == 0u);
}

namespace {

/// One timeline whose single track animates `prop` on `target` with two keyframes of `type`
/// (`unit` for a Length track).
std::vector<uint8_t> trackIr(uint32_t target, PropId prop, ValueType type, uint8_t unit = 0) {
    IrWriter w;
    TimelineBuilder tl;
    tl.name = w.intern("tl");
    tl.duration = 1.0f;
    tl.frameRate = 60.0f;
    IrTrack track{};
    track.target = target;
    track.prop = prop;
    track.type = static_cast<uint8_t>(type);
    track.kfOffset = 0;
    track.kfCount = 2;
    tl.tracks.push_back(track);
    for (float t : {0.0f, 1.0f}) {
        IrKeyframe kf = floatKeyframe(t, 4.0f);
        kf.value[4] = unit;
        tl.keyframes.push_back(kf);
    }
    w.addTimeline(std::move(tl));
    return w.finish();
}

}  // namespace

TEST_CASE("T-257b: a keyframe value is one its property takes") {
    const uint8_t em = static_cast<uint8_t>(LengthUnit::Em);
    const uint8_t pct = static_cast<uint8_t>(LengthUnit::Percent);
    // The fuzz_ir finding: an element translate keyframe in `em` (setProperty takes px or %).
    CHECK(openStatus(trackIr(kSelfNode, prop::TranslateX, ValueType::Length, em)) == Status::kInvalidIr);
    CHECK(openStatus(trackIr(kSelfNode, prop::TranslateY, ValueType::Color)) == Status::kInvalidIr);
    CHECK(openStatus(trackIr(kSelfNode, prop::Opacity, ValueType::Color)) == Status::kInvalidIr);
    CHECK(openStatus(trackIr(0, prop::TranslateX, ValueType::Length, pct)) == Status::kInvalidIr);
    CHECK(openStatus(trackIr(kSelfNode, 15, ValueType::Float)) == Status::kInvalidIr);  // Undefined.
    // What the property writers take.
    CHECK(openStatus(trackIr(kSelfNode, prop::TranslateX, ValueType::Length, pct)) == Status::kOk);
    CHECK(openStatus(trackIr(kSelfNode, prop::TranslateX, ValueType::Vec2)) == Status::kOk);
    CHECK(openStatus(trackIr(kSelfNode, prop::TranslateX, ValueType::Float)) == Status::kOk);
    CHECK(openStatus(trackIr(0, prop::Opacity, ValueType::Float)) == Status::kOk);
    CHECK(openStatus(trackIr(kSelfNode, prop::Width, ValueType::Length, em)) == Status::kOk);
}

namespace {

/// SCEN 0 (a Group root, node 0, with one Path child, node 1) and its TLIN 0 holding one Float
/// track of `prop` on `target`.
std::vector<uint8_t> sceneTrackIr(uint32_t target, PropId prop) {
    IrWriter w;
    SceneBuilder scene;
    IrSceneNode root{};
    root.kind = static_cast<uint8_t>(NodeKind::Group);
    root.visible = 1;
    root.parent = 0xFFFFFFFFu;
    root.opacity = 1;
    root.scale = Vec2{1, 1};
    root.name = kInvalidString;
    scene.nodes.push_back(root);
    IrSceneNode pathNode = root;
    pathNode.kind = static_cast<uint8_t>(NodeKind::Path);
    pathNode.parent = 0;
    pathNode.kindIndex = 0;
    scene.nodes.push_back(pathNode);
    IrPathData path{};
    path.miterLimit = 4.0f;
    path.trimEnd = 1.0f;
    scene.paths.push_back(path);
    w.addScene(std::move(scene));
    TimelineBuilder tl;
    tl.name = w.intern("tl");
    tl.duration = 1.0f;
    tl.frameRate = 60.0f;
    IrTrack track{};
    track.target = target;
    track.prop = prop;
    track.type = static_cast<uint8_t>(ValueType::Float);
    track.kfOffset = 0;
    track.kfCount = 1;
    tl.tracks.push_back(track);
    tl.keyframes.push_back(floatKeyframe(0, 0.5f));
    w.addTimeline(std::move(tl));
    return w.finish();
}

}  // namespace

TEST_CASE("T-257b: a track's property applies to its target node's kind") {
    // A Path property on the Group root, and on the playing Element.
    CHECK(openStatus(sceneTrackIr(0, prop::FillOpacity)) == Status::kInvalidIr);
    CHECK(openStatus(sceneTrackIr(kSelfNode, prop::FillOpacity)) == Status::kInvalidIr);
    // An Element property on a scene node.
    CHECK(openStatus(sceneTrackIr(1, prop::ParticleMax)) == Status::kInvalidIr);
    // What applies.
    CHECK(openStatus(sceneTrackIr(1, prop::FillOpacity)) == Status::kOk);
    CHECK(openStatus(sceneTrackIr(0, prop::Opacity)) == Status::kOk);
    CHECK(openStatus(sceneTrackIr(kSelfNode, prop::Opacity)) == Status::kOk);
}

TEST_CASE("T-257b: setProperty ignores a value its property does not take") {
    NodeArena arena;
    const NodeId el = arena.create(NodeKind::Element);
    setProperty(arena, el, prop::TranslateX, Value::ofLength(3, LengthUnit::Px));
    setProperty(arena, el, prop::TranslateX, Value::ofLength(2, LengthUnit::Em));
    setProperty(arena, el, prop::TranslateX, Value::ofColor(Color{1, 0, 0, 1}));
    CHECK(arena.element(el).cssTranslate.x == 3.0f);
    setProperty(arena, el, prop::Opacity, Value::ofFloat(0.5f));
    setProperty(arena, el, prop::Opacity, Value::ofColor(Color{1, 0, 0, 1}));
    CHECK(arena.common(el).opacity == 0.5f);
}

TEST_CASE("T-257b: the fuzz_ir regression inputs are rejected before load") {
    for (const char* name :
         {"transform_origin_em.bin", "autoplay_missing_timeline.bin", "translate_keyframe_value_type.bin"}) {
        CAPTURE(name);
        const std::vector<uint8_t> bytes = readRegression(name);
        REQUIRE(!bytes.empty());
        bool loaded = true;
        CHECK(runFuzzIrSequence(bytes, loaded) == Status::kInvalidIr);
        CHECK(!loaded);
    }
}

// The fuzz_ir out-of-memory inputs are valid IR: they load, and one frame used to queue one
// iteration note per iteration crossed (hundreds of millions). The frame now completes and
// delivers at most kMaxIterationNotesPerUpdate + 1 iteration notes per instance.
TEST_CASE("T-257b: the fuzz_ir iteration-note OOM inputs complete one frame with bounded notes") {
    for (const char* name : {"iteration_notes_oom_1.bin", "iteration_notes_oom_2.bin"}) {
        CAPTURE(name);
        const std::vector<uint8_t> bytes = readRegression(name);
        REQUIRE(!bytes.empty());
        bool loaded = false;
        std::map<uint32_t, uint32_t> iterationEvents;
        CHECK(runFuzzIrSequence(bytes, loaded, &iterationEvents) == Status::kOk);
        CHECK(loaded);
        uint32_t total = 0;
        for (const auto& [id, count] : iterationEvents) {
            CAPTURE(id);
            CHECK(count <= kMaxIterationNotesPerUpdate + 1);
            total += count;
        }
        MESSAGE(std::string(name) << ": " << iterationEvents.size() << " instance(s), " << total
                                  << " iteration event(s)");
    }
}
