#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "camera.h"  // Code/Runtime/core/src internal header: cssPerspectiveViewProj (Tests/gpu/CMakeLists.txt adds Code/Runtime/core/src).
#include "doctest.h"
#include "golden_common.h"
#include "test_temp.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/command_simplifier.h"
#include "xt/compile/compile.h"
#include "xt/compile/ir_writer.h"
#include "xt/context.h"
#include "xt/damage.h"
#include "xt/document.h"
#include "xt/effect_utils.h"
#include "xt/gpu/frame_builder.h"
#include "xt/gpu/gpu_device.h"
#include "xt/gpu/gpu_frame.h"
#include "xt/ir.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/painter.h"
#include "xt/perf.h"
#include "xt/scene_instance.h"
#include "xt/sw/sw_png.h"
#include "xt/sw/sw_renderer.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

#ifndef XT_GPU_NO_TEXT
#include "xt/text/ft_text_shaper.h"
#include "xt/text/glyph_atlas.h"
#endif

/// xt_gpu FrameBuilder tests (Docs/tasks/T-146.md "測試"): a recording FakeDevice, the fixture
/// CommandLists Tests/gl also exercises (golden scene fixtures via Painter, UI `.xt` fixtures via
/// the compiler + Context, hand-built lists for clip/layer/effect/text/3D) and the structural
/// golden snapshots in Tests/gpu/golden.

using namespace xt;
using namespace xt::gpu;

namespace {

// -------------------------------------------------------------------------------------------
// FakeDevice: records every call; handles are sequential.
// -------------------------------------------------------------------------------------------

struct TextureRecord {
    std::string name;
    uint16_t w = 0, h = 0;
    TextureFormat format = TextureFormat::Rgba8;
    bool hadInitial = false;
    bool alive = true;
    uint8_t firstByte = 0;  ///< of `initial`, when there was one
};

struct UpdateRecord {
    TextureHandle tex;
    uint16_t x, y, w, h;
    size_t rowPitch;
    uint8_t firstByte;
};

class FakeDevice : public GpuDevice {
public:
    std::map<TextureHandle, TextureRecord> textures;
    std::vector<UpdateRecord> updates;
    int submits = 0;
    TextureHandle next = 1;
    /// T-263b E10: a device limit (GL_MAX_TEXTURE_SIZE); a larger texture is refused with 0.
    uint16_t maxSize = 0xFFFF;
    int createCalls = 0;
    /// T-263d: the next `failCreates` createTexture calls return 0 (an upload that fails for now).
    int failCreates = 0;
    /// T-263d: what maxTextureSize() answers (0 = unknown, the base class's answer) and how often
    /// it was asked.
    uint32_t textureLimit = 0;
    mutable int limitQueries = 0;
    uint32_t maxTextureSize() const override {
        ++limitQueries;
        return textureLimit;
    }

    TextureHandle createTexture(const char* debugName, uint16_t w, uint16_t h, TextureFormat fmt,
                                const void* initial) override {
        ++createCalls;
        if (w > maxSize || h > maxSize) {
            return 0;
        }
        if (failCreates > 0) {
            --failCreates;
            return 0;
        }
        TextureHandle h2 = next++;
        textures[h2] =
            TextureRecord{debugName,
                          w,
                          h,
                          fmt,
                          initial != nullptr,
                          true,
                          initial != nullptr ? static_cast<const uint8_t*>(initial)[0] : uint8_t{0}};
        return h2;
    }
    void updateTexture(TextureHandle tex, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const void* data,
                       size_t rowPitch) override {
        REQUIRE(textures.count(tex) == 1);
        updates.push_back(UpdateRecord{tex, x, y, w, h, rowPitch, static_cast<const uint8_t*>(data)[0]});
    }
    void destroyTexture(TextureHandle tex) override {
        if (tex == 0) return;
        REQUIRE(textures.count(tex) == 1);
        textures[tex].alive = false;
    }
    void submit(const GpuFrame&) override { ++submits; }

    // T-343: whether target 0 may be read back (Direct mode); band targets go through the
    // cached-target path below (the default implementations).
    bool direct = true;
    bool supportsDirectTarget() const override { return direct; }

    // T-162 effect result cache: acquired targets by handle (w, h); `supportsCache == false`
    // behaves like the base class (returns 0).
    bool supportsCache = true;
    std::map<TextureHandle, std::pair<uint16_t, uint16_t>> cached;
    int cacheAcquires = 0;
    int cacheReleases = 0;
    TextureHandle acquireCachedTarget(uint16_t w, uint16_t h, TextureFormat fmt) override {
        if (!supportsCache) return 0;
        CHECK(fmt == TextureFormat::Rgba8);
        TextureHandle h2 = next++;
        cached[h2] = {w, h};
        ++cacheAcquires;
        return h2;
    }
    void releaseCachedTarget(TextureHandle target) override {
        if (target == 0) return;
        REQUIRE(cached.count(target) == 1);
        cached.erase(target);
        ++cacheReleases;
    }

    // T-263d: custom effect programs, one id per distinct GLSL module (`customGlsl[id - 1]`);
    // `customPrograms == false` behaves like the base class (returns 0: the fallback is used).
    bool customPrograms = false;
    std::vector<std::string> customGlsl;
    uint32_t prepareCustomEffect(std::string_view glsl, const char*, const uint8_t*, uint32_t) override {
        if (!customPrograms) return 0;
        for (size_t i = 0; i < customGlsl.size(); ++i) {
            if (customGlsl[i] == glsl) return static_cast<uint32_t>(i + 1);
        }
        customGlsl.emplace_back(glsl);
        return static_cast<uint32_t>(customGlsl.size());
    }

    int aliveCount() const {
        int n = 0;
        for (const auto& [h, t] : textures) {
            (void)h;
            if (t.alive) ++n;
        }
        return n;
    }
};

// -------------------------------------------------------------------------------------------
// Frame validation shared by every case.
// -------------------------------------------------------------------------------------------

void checkFrameWellFormed(const GpuFrame& f) {
    uint32_t coveredCmds = 0;
    for (const Pass& p : f.passes) {
        CHECK(p.firstCmd + p.cmdCount <= f.cmds.size());
        CHECK(p.target <= f.transients.size());
        if (p.copy) {
            CHECK(p.copySource <= f.transients.size());
            CHECK(p.copySource != p.target);
        }
        coveredCmds += p.cmdCount;
    }
    CHECK(coveredCmds == f.cmds.size());
    for (const DrawCmd& c : f.cmds) {
        CHECK(c.firstIndex + c.indexCount <= f.indices.size());
        CHECK(c.baseVertex + c.vertexCount <= f.vertices.size());
        CHECK(c.indexCount % 3 == 0);
        for (uint32_t i = c.firstIndex; i < c.firstIndex + c.indexCount; ++i) {
            CHECK(f.indices[i] < c.vertexCount);
        }
        CHECK(c.src <= f.transients.size());
        CHECK(c.aux <= f.transients.size());
        CHECK(c.clipW >= 0);
        CHECK(c.clipH >= 0);
        if (c.paramIndex != 0 || c.shading == Shading::Gradient || c.shading == Shading::Composite) {
            CHECK(c.paramIndex <= f.params.size());
        }
    }
}

std::vector<const DrawCmd*> cmdsWith(const GpuFrame& f, Shading s) {
    std::vector<const DrawCmd*> out;
    for (const DrawCmd& c : f.cmds) {
        if (c.shading == s) out.push_back(&c);
    }
    return out;
}

std::vector<const DrawCmd*> cmdsWith(const GpuFrame& f, StencilOp op) {
    std::vector<const DrawCmd*> out;
    for (const DrawCmd& c : f.cmds) {
        if (c.stencilOp == op) out.push_back(&c);
    }
    return out;
}

// -------------------------------------------------------------------------------------------
// Hand-built CommandList helpers (same shapes as Tests/gl/test_gl_effects.cpp).
// -------------------------------------------------------------------------------------------

CommandList makeList(int w = 64, int h = 64) {
    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
    cl.devicePixelRatio = 1;
    return cl;
}

PathRef addRectPath(CommandList& cl, float x, float y, float w, float h) {
    PathRef ref{};
    ref.verbOffset = static_cast<uint32_t>(cl.verbs.size());
    ref.pointOffset = static_cast<uint32_t>(cl.points.size());
    cl.verbs.push_back(PathVerb::Move);
    cl.verbs.push_back(PathVerb::Line);
    cl.verbs.push_back(PathVerb::Line);
    cl.verbs.push_back(PathVerb::Line);
    cl.verbs.push_back(PathVerb::Close);
    float pts[8] = {x, y, x + w, y, x + w, y + h, x, y + h};
    cl.points.insert(cl.points.end(), pts, pts + 8);
    ref.verbCount = 5;
    ref.pointCount = 8;
    return ref;
}

void addFillRect(CommandList& cl, float x, float y, float w, float h, Color color, Mat2x3 xf = Mat2x3{},
                 FillRule rule = FillRule::NonZero) {
    PathRef path = addRectPath(cl, x, y, w, h);
    FillCmd f{};
    f.xf = xf;
    f.path = path;
    f.paint.kind = PaintKind::Solid;
    f.paint.color = color;
    f.rule = rule;
    f.antialias = 1;
    cl.fills.push_back(f);
    cl.commands.push_back(Command{CmdKind::FillPath, {}, static_cast<uint32_t>(cl.fills.size() - 1)});
}

/// An L shape (the rect minus its bottom-right quarter): concave, so it takes the stencil path
/// (T-155 draws convex contours directly).
PathRef addLPath(CommandList& cl, float x, float y, float w, float h) {
    PathRef ref{};
    ref.verbOffset = static_cast<uint32_t>(cl.verbs.size());
    ref.pointOffset = static_cast<uint32_t>(cl.points.size());
    cl.verbs.push_back(PathVerb::Move);
    for (int i = 0; i < 5; ++i) cl.verbs.push_back(PathVerb::Line);
    cl.verbs.push_back(PathVerb::Close);
    float pts[12] = {x, y, x + w, y, x + w, y + h / 2, x + w / 2, y + h / 2, x + w / 2, y + h, x, y + h};
    cl.points.insert(cl.points.end(), pts, pts + 12);
    ref.verbCount = 7;
    ref.pointCount = 12;
    return ref;
}

void addFillConcave(CommandList& cl, float x, float y, float w, float h, Color color, Mat2x3 xf = Mat2x3{},
                    FillRule rule = FillRule::NonZero) {
    PathRef path = addLPath(cl, x, y, w, h);
    FillCmd f{};
    f.xf = xf;
    f.path = path;
    f.paint.kind = PaintKind::Solid;
    f.paint.color = color;
    f.rule = rule;
    f.antialias = 1;
    cl.fills.push_back(f);
    cl.commands.push_back(Command{CmdKind::FillPath, {}, static_cast<uint32_t>(cl.fills.size() - 1)});
}

void addFillGradient(CommandList& cl, float x, float y, float w, float h, bool concave = false) {
    PathRef path = concave ? addLPath(cl, x, y, w, h) : addRectPath(cl, x, y, w, h);
    FillCmd f{};
    f.path = path;
    f.paint.kind = PaintKind::Linear;
    f.paint.color = Color{1, 1, 1, 1};
    f.paint.p0 = Vec2{x, y};
    f.paint.p1 = Vec2{x + w, y};
    f.paint.stopOffset = static_cast<uint32_t>(cl.stops.size());
    cl.stops.push_back(GradientStop{0.0f, Color{1, 0, 0, 1}});
    cl.stops.push_back(GradientStop{1.0f, Color{0, 0, 1, 1}});
    f.paint.stopCount = 2;
    f.rule = FillRule::NonZero;
    f.antialias = 1;
    cl.fills.push_back(f);
    cl.commands.push_back(Command{CmdKind::FillPath, {}, static_cast<uint32_t>(cl.fills.size() - 1)});
}

void addPushClipRect(CommandList& cl, Rect rect, Mat2x3 xf = Mat2x3{}) {
    ClipRectCmd c{};
    c.xf = xf;
    c.rect = rect;
    cl.clipRects.push_back(c);
    cl.commands.push_back(Command{CmdKind::PushClipRect, {}, static_cast<uint32_t>(cl.clipRects.size() - 1)});
}

/// A path clip. The default L shape is concave and so a stencil level; a plain rectangle path
/// becomes a scissor level (T-155), like an axis-aligned PushClipRect.
void addPushClipPath(CommandList& cl, float x, float y, float w, float h, bool concave = true) {
    ClipPathCmd c{};
    c.xf = Mat2x3{};
    c.path = concave ? addLPath(cl, x, y, w, h) : addRectPath(cl, x, y, w, h);
    c.rule = FillRule::NonZero;
    cl.clipPaths.push_back(c);
    cl.commands.push_back(Command{CmdKind::PushClipPath, {}, static_cast<uint32_t>(cl.clipPaths.size() - 1)});
}

void addPopClip(CommandList& cl) { cl.commands.push_back(Command{CmdKind::PopClip, {}, 0}); }

void addPushLayer(CommandList& cl, float opacity, BlendMode blend) {
    LayerCmd l{};
    l.opacity = opacity;
    l.blend = blend;
    l.boundsHint = Rect{0, 0, 0, 0};
    cl.layers.push_back(l);
    cl.commands.push_back(Command{CmdKind::PushLayer, {}, static_cast<uint32_t>(cl.layers.size() - 1)});
}

void addPopLayer(CommandList& cl) { cl.commands.push_back(Command{CmdKind::PopLayer, {}, 0}); }

void addPushMask(CommandList& cl, MaskMode mode) {
    MaskCmd m{};
    m.mode = mode;
    cl.masks.push_back(m);
    cl.commands.push_back(Command{CmdKind::PushMask, {}, static_cast<uint32_t>(cl.masks.size() - 1)});
}

void addPushProjection(CommandList& cl, Mat4 viewProj, Mat2x3 rootXf = Mat2x3{},
                       Rect box = Rect{0, 0, 64, 64}) {
    ProjectionCmd c{};
    c.viewProj = viewProj;
    c.rootXf = rootXf;
    c.boxRect = box;
    c.depthTest = 0;
    cl.projections.push_back(c);
    cl.commands.push_back(
        Command{CmdKind::PushProjection, {}, static_cast<uint32_t>(cl.projections.size() - 1)});
}

void addPushTransform3D(CommandList& cl, Mat4 world3) {
    Transform3DCmd c{};
    c.world3 = world3;
    cl.transforms3.push_back(c);
    cl.commands.push_back(
        Command{CmdKind::PushTransform3D, {}, static_cast<uint32_t>(cl.transforms3.size() - 1)});
}

void addPushEffect(CommandList& cl, const std::vector<EffectInstance>& effects, Mat2x3 xf, Rect boundsHint,
                   Rect contentBounds) {
    EffectCmd c{};
    c.effectOffset = static_cast<uint32_t>(cl.effects.size());
    c.effectCount = static_cast<uint32_t>(effects.size());
    for (const EffectInstance& e : effects) {
        cl.effects.push_back(e);
    }
    c.xf = xf;
    c.boundsHint = boundsHint;
    c.contentBounds = contentBounds;
    cl.effectCmds.push_back(c);
    cl.commands.push_back(Command{CmdKind::PushEffect, {}, static_cast<uint32_t>(cl.effectCmds.size() - 1)});
}

void addPopEffect(CommandList& cl) { cl.commands.push_back(Command{CmdKind::PopEffect, {}, 0}); }

EffectInstance blurFx(float rx, float ry) {
    EffectInstance e{};
    e.kind = EffectKind::Blur;
    e.params[0] = rx;
    e.params[1] = ry;
    return e;
}

EffectInstance dropShadow(float ox, float oy, float blur, float spread, float opacity, Color color,
                          bool inset) {
    EffectInstance e{};
    e.kind = EffectKind::DropShadow;
    e.params[0] = ox;
    e.params[1] = oy;
    e.params[2] = blur;
    e.params[3] = spread;
    e.params[4] = opacity;
    e.params[5] = inset ? 1.0f : 0.0f;
    e.colors[0] = color;
    return e;
}

EffectInstance glow(float radius, Color color, float intensity, bool inner) {
    EffectInstance e{};
    e.kind = EffectKind::Glow;
    e.params[0] = radius;
    e.params[1] = intensity;
    e.params[2] = inner ? 1.0f : 0.0f;
    e.colors[0] = color;
    return e;
}

EffectInstance outline(float radius, float softness, float position, Color color) {
    EffectInstance e{};
    e.kind = EffectKind::Outline;
    e.params[0] = radius;
    e.params[1] = softness;
    e.params[2] = position;
    e.colors[0] = color;
    return e;
}

EffectInstance colorAdjust(float brightness, float contrast, float saturation, float hue, float opacity) {
    EffectInstance e{};
    e.kind = EffectKind::ColorAdjust;
    e.params[0] = brightness;
    e.params[1] = contrast;
    e.params[2] = saturation;
    e.params[3] = hue;
    e.params[4] = opacity;
    return e;
}

EffectInstance gradientMask(CommandList& cl, Vec2 p0, Vec2 p1, bool radial, bool invert) {
    EffectInstance e{};
    e.kind = EffectKind::GradientMask;
    e.params[0] = p0.x;
    e.params[1] = p0.y;
    e.params[2] = p1.x;
    e.params[3] = p1.y;
    e.params[4] = radial ? 1.0f : 0.0f;
    e.params[5] = invert ? 1.0f : 0.0f;
    e.stopOffset = static_cast<uint32_t>(cl.stops.size());
    cl.stops.push_back(GradientStop{0.0f, Color{0, 0, 0, 1}});
    cl.stops.push_back(GradientStop{1.0f, Color{0, 0, 0, 0}});
    e.stopCount = 2;
    return e;
}

EffectInstance shine(float position, float width, float softness, float angle, float intensity, Color color) {
    EffectInstance e{};
    e.kind = EffectKind::Shine;
    e.params[0] = position;
    e.params[1] = width;
    e.params[2] = softness;
    e.params[3] = angle;
    e.params[4] = intensity;
    e.colors[0] = color;
    return e;
}

EffectInstance backdropBlur(float radius, float saturation, float brightness) {
    EffectInstance e{};
    e.kind = EffectKind::BackdropBlur;
    e.params[0] = radius;
    e.params[1] = saturation;
    e.params[2] = brightness;
    return e;
}

// -------------------------------------------------------------------------------------------
// Building a frame.
// -------------------------------------------------------------------------------------------

struct Built {
    FakeDevice device;
    FrameBuilder builder;
    GpuFrame frame;
};

void buildInto(Built& b, const CommandList& cl, int w, int h, TextShaper* shaper = nullptr,
               AssetResolver* assets = nullptr, const IrView* ir = nullptr) {
    b.builder.init(b.device);
    b.builder.resize(w, h);
    b.builder.build(cl, shaper, assets, ir, b.frame);
    CHECK(b.device.submits == 0);  // build never submits (Docs/tasks/T-146.md)
    checkFrameWellFormed(b.frame);
}

// -------------------------------------------------------------------------------------------
// Golden snapshots (Docs/tasks/T-146.md test 6): Tests/gpu/golden/<name>.txt, compared on every
// run. XT_GPU_GOLDEN_UPDATE=1 in the environment (re)writes them. T-267a: a missing snapshot is a
// failure, never written silently, and a mismatch's <name>.actual.txt goes to the build tree
// (XT_GPU_ACTUAL_DIR), not into the sources.
// -------------------------------------------------------------------------------------------

bool updateGolden() {
#ifdef _MSC_VER
    char* v = nullptr;
    size_t len = 0;
    if (_dupenv_s(&v, &len, "XT_GPU_GOLDEN_UPDATE") != 0 || v == nullptr) {
        return false;
    }
    bool on = v[0] == '1';
    std::free(v);
    return on;
#else
    const char* v = std::getenv("XT_GPU_GOLDEN_UPDATE");
    return v != nullptr && v[0] == '1';
#endif
}

void checkSnapshot(const char* name, const GpuFrame& frame) {
    std::string actual;
    dumpGpuFrame(frame, actual);
    std::filesystem::path dir(XT_GPU_GOLDEN_DIR);
    std::filesystem::path path = dir / (std::string(name) + ".txt");
    INFO("snapshot: ", path.string());
    if (updateGolden()) {
        std::filesystem::create_directories(dir);
        std::ofstream out(path, std::ios::binary);
        REQUIRE(out.good());
        out << actual;
        MESSAGE("wrote snapshot ", path.string());
        return;
    }
    if (!std::filesystem::exists(path)) {
        FAIL("missing snapshot ", path.string(), " (XT_GPU_GOLDEN_UPDATE=1 writes it)");
    }
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    std::string expected = ss.str();
    // Normalize line endings in case the file was checked out with CRLF.
    expected.erase(std::remove(expected.begin(), expected.end(), '\r'), expected.end());
    if (expected != actual) {
        const std::filesystem::path actualDir(XT_GPU_ACTUAL_DIR);
        std::error_code ec;
        std::filesystem::create_directories(actualDir, ec);
        const std::filesystem::path actualPath = actualDir / (std::string(name) + ".actual.txt");
        std::ofstream out(actualPath, std::ios::binary);
        out << actual;
        MESSAGE("wrote ", actualPath.string());
    }
    CHECK(expected == actual);
}

// -------------------------------------------------------------------------------------------
// Scene fixtures via Painter (Tests/gl/test_gl_renderer.cpp's pipeline).
// -------------------------------------------------------------------------------------------

CommandList paintFixture(const golden::GoldenFixture& fx) {
    compile::IrWriter writer;
    fx.build(writer);
    std::vector<uint8_t> bytes = writer.finish();
    Result<IrView> viewResult = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(viewResult.ok());
    const IrView& view = viewResult.value();

    NodeArena arena;
    NodeId container = arena.create(NodeKind::Group);
    SceneInstance inst;
    REQUIRE(instantiateScene(view, 0, arena, container, inst) == Status::kOk);
    updateWorldTransforms(arena, inst.root, true);

    Painter painter;
    CommandList cmds;
    painter.paint(arena, inst.root, cmds);
    cmds.viewport = Rect{0, 0, static_cast<float>(fx.width), static_cast<float>(fx.height)};
    cmds.devicePixelRatio = fx.dpr;
    return cmds;
}

// -------------------------------------------------------------------------------------------
// UI `.xt` fixtures via the compiler + Context (Tests/golden/golden_ui.cpp's pipeline).
// -------------------------------------------------------------------------------------------

struct UiDoc {
    std::unique_ptr<Context> ctx;
    Document* doc = nullptr;
    CommandList cmds;
    NullTextShaper shaper;
    std::unique_ptr<FileAssetResolver> resolver;
};

bool loadUiFixture(UiDoc& out, const char* file, int w, int h, float time = 0.0f) {
    std::filesystem::path xtPath = std::filesystem::path(XT_GPU_UI_DIR) / file;
    compile::CompileOptions options;
    options.rootDir = xtPath.parent_path().string();
    options.strict = false;
    compile::CompileResult result = compile::compileDocument(xtPath.string(), options);
    if (!result.ok()) {
        return false;
    }
    out.resolver = std::make_unique<FileAssetResolver>(xtPath.parent_path().string(), sw::decodeImageBytes);
    out.ctx = std::make_unique<Context>(ContextConfig{&out.shaper, out.resolver.get()});
    out.ctx->setViewport(Rect{0, 0, static_cast<float>(w), static_cast<float>(h)}, 1.0f);
    Result<DocumentHandle> handle = out.ctx->loadDocument(IrBlob{result.ir.data(), result.ir.size()}, true);
    if (!handle.ok()) {
        return false;
    }
    out.doc = out.ctx->document(handle.value());
    if (out.doc == nullptr) {
        return false;
    }
    out.ctx->update(0.0f);
    if (time != 0.0f) {
        out.ctx->update(time);
    }
    out.ctx->render(out.cmds);
    return true;
}

}  // namespace

// =================================================================================================
// 1. FillPath -> winding + cover + clear, in order, indices inside the vertex range.
// =================================================================================================

TEST_CASE("gpu: FillPath emits winding, cover and clear commands") {
    // Concave (L-shaped) fills: a convex contour takes T-155's direct path instead. Their AABBs
    // overlap, so every deferred ClearRect is flushed before the next winding.
    CommandList cl = makeList();
    addFillConcave(cl, 8, 8, 32, 24, Color{1, 0, 0, 1});
    addFillConcave(cl, 0, 0, 64, 64, Color{0, 1, 0, 0.5f}, Mat2x3{}, FillRule::EvenOdd);
    addFillGradient(cl, 10, 10, 20, 20, true);
    Built b;
    buildInto(b, cl, 64, 64);
    const GpuFrame& f = b.frame;

    REQUIRE(f.cmds.size() == 9);
    REQUIRE(f.passes.size() == 1);
    CHECK(f.passes[0].target == kSurfaceTarget);
    CHECK(f.passes[0].clear == 0);
    CHECK(f.passes[0].cmdCount == 9);
    CHECK(f.transients.empty());

    for (size_t i = 0; i < 3; ++i) {
        const DrawCmd& winding = f.cmds[i * 3];
        const DrawCmd& cover = f.cmds[i * 3 + 1];
        const DrawCmd& clear = f.cmds[i * 3 + 2];
        CHECK(winding.shading == Shading::StencilOnly);
        CHECK(winding.stencilOp == (i == 1 ? StencilOp::WriteInvert : StencilOp::WriteIncrWrap));
        CHECK(winding.stencilRef == 0);
        CHECK(winding.blend == Blend::Replace);
        CHECK(winding.indexCount == 12);  // 6-point contour -> 4 fan triangles
        CHECK(winding.vertexCount == 6);
        CHECK(cover.stencilOp == StencilOp::TestNotEqualZero);
        CHECK(cover.blend == Blend::SrcOver);
        CHECK(cover.indexCount == 6);
        CHECK(cover.vertexCount == 4);
        CHECK(clear.shading == Shading::StencilOnly);
        CHECK(clear.stencilOp == StencilOp::ClearRect);
        CHECK(clear.indexCount == 6);  // one AABB quad
        CHECK(clear.vertexCount == 4);
        // Scissor is the full surface.
        CHECK(cover.clipX == 0);
        CHECK(cover.clipY == 0);
        CHECK(cover.clipW == 64);
        CHECK(cover.clipH == 64);
    }
    // Solid color is premultiplied r,g,b,a.
    CHECK(f.cmds[1].shading == Shading::Solid);
    CHECK(f.vertices[f.cmds[1].baseVertex].rgba == 0xFF0000FFu);
    CHECK(f.cmds[4].shading == Shading::Solid);
    CHECK(f.vertices[f.cmds[4].baseVertex].rgba == 0x80008000u);
    // Gradient: LUT texture created, params written, u,v = path-local coordinates.
    const DrawCmd& grad = f.cmds[7];
    CHECK(grad.shading == Shading::Gradient);
    CHECK(grad.texture != 0);
    REQUIRE(b.device.textures.count(grad.texture) == 1);
    CHECK(b.device.textures[grad.texture].w == kGradientLutWidth);
    CHECK(b.device.textures[grad.texture].h == 1);
    CHECK(b.device.textures[grad.texture].format == TextureFormat::Rgba8);
    REQUIRE(grad.paramIndex + sizeof(GradientParams) / sizeof(float) <= f.params.size());
    GradientParams gp{};
    std::memcpy(&gp, f.params.data() + grad.paramIndex, sizeof(gp));
    CHECK(gp.kind == static_cast<float>(static_cast<int>(PaintKind::Linear)));
    CHECK(gp.p0x == 10.0f);
    CHECK(gp.p1x == 30.0f);
    CHECK(gp.alpha == 1.0f);
    CHECK(f.vertices[grad.baseVertex].u == doctest::Approx(10.0f));
    CHECK(f.vertices[grad.baseVertex].v == doctest::Approx(10.0f));
    // Every vertex of an unprojected frame has w == 1.
    for (const Vertex& v : f.vertices) {
        CHECK(v.w == 1.0f);
    }
    checkSnapshot("fill_basic", f);
}

TEST_CASE("gpu: a dpr of 2 scales positions and the gradient LUT is shared per stop list") {
    CommandList cl = makeList();
    cl.devicePixelRatio = 2.0f;
    addFillGradient(cl, 0, 0, 10, 10);
    addFillGradient(cl, 20, 20, 10, 10);
    Built b;
    buildInto(b, cl, 128, 128);
    auto grads = cmdsWith(b.frame, Shading::Gradient);
    REQUIRE(grads.size() == 2);
    CHECK(grads[0]->texture == grads[1]->texture);
    CHECK(b.device.aliveCount() == 1);
    // Device-pixel positions: the second rect spans 40..60 (the fan is inset 0.5 px by the
    // T-155 edge fringe).
    const Vertex& v = b.frame.vertices[grads[1]->baseVertex];
    CHECK(v.x == doctest::Approx(40.5f));
    // An unused LUT is destroyed on the next frame.
    CommandList empty = makeList();
    b.builder.build(empty, nullptr, nullptr, nullptr, b.frame);
    CHECK(b.device.aliveCount() == 0);
}

// =================================================================================================
// 1b. T-155: convex fast path, edge fringe, state batching, deferred ClearRects.
// =================================================================================================

TEST_CASE("gpu: T-155 a convex fill is one direct command with a 1 px coverage fringe") {
    CommandList cl = makeList();
    addFillRect(cl, 8, 8, 32, 24, Color{1, 0, 0, 1});
    Built b;
    buildInto(b, cl, 64, 64);
    const GpuFrame& f = b.frame;
    REQUIRE(f.cmds.size() == 1);
    const DrawCmd& c = f.cmds[0];
    CHECK(c.shading == Shading::Solid);
    CHECK(c.blend == Blend::SrcOver);
    CHECK(c.stencilOp == StencilOp::TestEqualRef);
    CHECK(c.stencilRef == 0);
    CHECK(c.vertexCount == 8);  // 4 inset fan vertices + 4 outer ring vertices
    CHECK(c.indexCount == 30);  // 2 fan triangles + 4 ring quads
    const Vertex* v = &f.vertices[c.baseVertex];
    for (int i = 0; i < 4; ++i) {
        CHECK(v[i].rgba == 0xFF0000FFu);
        CHECK(v[4 + i].rgba == 0u);
    }
    // Inset by 0.5 px along the corner miters, outer ring 0.5 px outside.
    CHECK(v[0].x == doctest::Approx(8.5f));
    CHECK(v[0].y == doctest::Approx(8.5f));
    CHECK(v[2].x == doctest::Approx(39.5f));
    CHECK(v[2].y == doctest::Approx(31.5f));
    CHECK(v[4].x == doctest::Approx(7.5f));
    CHECK(v[4].y == doctest::Approx(7.5f));
    CHECK(v[6].x == doctest::Approx(40.5f));
    CHECK(v[6].y == doctest::Approx(32.5f));
    for (uint32_t i = c.firstIndex; i < c.firstIndex + c.indexCount; ++i) {
        CHECK(f.indices[i] < 8);
    }
    CHECK(cmdsWith(f, StencilOp::WriteIncrWrap).empty());
    CHECK(cmdsWith(f, StencilOp::ClearRect).empty());
}

/// True when `p` lies inside (or on) the triangle `a b c`.
bool pointInTriangle(Vec2 p, Vec2 a, Vec2 b, Vec2 c) {
    const float d0 = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
    const float d1 = (c.x - b.x) * (p.y - b.y) - (c.y - b.y) * (p.x - b.x);
    const float d2 = (a.x - c.x) * (p.y - c.y) - (a.y - c.y) * (p.x - c.x);
    const bool neg = d0 < 0 || d1 < 0 || d2 < 0;
    const bool pos = d0 > 0 || d1 > 0 || d2 > 0;
    return !(neg && pos);
}

TEST_CASE("gpu: T-165 a rotating border ring never fans its hole (direct ring merge walk)") {
    // challenge-menu's #holo-ring: a 116 px circle with a 2 px border, squashed to 26 % height and
    // spun by a flow. Both contours of the border start at the same corner, so an inner vertex
    // shares the outer's vertex-0 angle up to rounding; before T-165 about 8 % of the rotations
    // put the inner angle sequence one turn ahead of the outer's and the merge walk fanned every
    // outer edge to one inner vertex -- a solid disc. The sweep covers the rounding cases.
    const Vec2 centre{128, 64};
    const Vec2 holeSamples[] = {centre, Vec2{centre.x - 30, centre.y}, Vec2{centre.x + 30, centre.y},
                                Vec2{centre.x, centre.y - 8}, Vec2{centre.x, centre.y + 8}};
    int fanned = 0;
    for (int deg = 0; deg < 360; ++deg) {
        CommandList cl = makeList(256, 128);
        PathRef path{};
        path.verbOffset = static_cast<uint32_t>(cl.verbs.size());
        path.pointOffset = static_cast<uint32_t>(cl.points.size());
        golden::appendCircle(cl.verbs, cl.points, Vec2{0, 0}, 58.0f);
        golden::appendCircle(cl.verbs, cl.points, Vec2{0, 0}, 56.0f);
        path.verbCount = static_cast<uint32_t>(cl.verbs.size()) - path.verbOffset;
        path.pointCount = static_cast<uint32_t>(cl.points.size()) - path.pointOffset;
        FillCmd fill{};
        fill.xf =
            mul(translation(centre), mul(scaling(Vec2{1.0f, 0.26f}), rotationDeg(static_cast<float>(deg))));
        fill.path = path;
        fill.paint.kind = PaintKind::Solid;
        fill.paint.color = Color{0, 1, 0, 1};
        fill.rule = FillRule::EvenOdd;
        fill.antialias = 1;
        cl.fills.push_back(fill);
        cl.commands.push_back(Command{CmdKind::FillPath, {}, 0});
        Built b;
        buildInto(b, cl, 256, 128);
        const GpuFrame& f = b.frame;
        // The ring takes the direct path: one Solid command, no stencil writes.
        REQUIRE(f.cmds.size() == 1);
        const DrawCmd& c = f.cmds[0];
        CHECK(c.shading == Shading::Solid);
        CHECK(cmdsWith(f, StencilOp::WriteInvert).empty());
        CHECK(cmdsWith(f, StencilOp::WriteIncrWrap).empty());
        // No triangle of the band reaches into the hole.
        const Vertex* v = &f.vertices[c.baseVertex];
        bool holeCovered = false;
        for (uint32_t i = c.firstIndex; i + 2 < c.firstIndex + c.indexCount; i += 3) {
            const Vertex& a = v[f.indices[i]];
            const Vertex& p = v[f.indices[i + 1]];
            const Vertex& q = v[f.indices[i + 2]];
            for (const Vec2& s : holeSamples) {
                holeCovered =
                    holeCovered || pointInTriangle(s, Vec2{a.x, a.y}, Vec2{p.x, p.y}, Vec2{q.x, q.y});
            }
        }
        if (holeCovered) {
            ++fanned;
            MESSAGE("hole covered at ", deg, " deg");
        }
    }
    CHECK(fanned == 0);
}

TEST_CASE("gpu: T-155 EvenOdd, rotation and antialias=0 on a convex contour") {
    CommandList cl = makeList();
    addFillRect(cl, 8, 8, 32, 24, Color{1, 0, 0, 1}, Mat2x3{}, FillRule::EvenOdd);
    addFillRect(cl, -10, -10, 20, 20, Color{0, 1, 0, 1}, mul(translation(Vec2{32, 32}), rotationDeg(30.0f)));
    addFillRect(cl, 0, 0, 8, 8, Color{0, 0, 1, 1});
    cl.fills.back().antialias = 0;
    Built b;
    buildInto(b, cl, 64, 64);
    const GpuFrame& f = b.frame;
    // All three are convex and share one state: one command. EvenOdd needs no WriteInvert.
    REQUIRE(f.cmds.size() == 1);
    CHECK(cmdsWith(f, StencilOp::WriteInvert).empty());
    CHECK(f.cmds[0].vertexCount == 8 + 8 + 4);  // the antialias=0 rect gets no fringe
    const Vertex* v = &f.vertices[f.cmds[0].baseVertex + 16];
    CHECK(v[0].x == 0.0f);
    CHECK(v[0].y == 0.0f);
    CHECK(v[2].x == 8.0f);
    CHECK(v[2].y == 8.0f);
}

TEST_CASE("gpu: T-155 setEdgeFringe(false) draws a plain fan") {
    CommandList cl = makeList();
    addFillRect(cl, 8, 8, 32, 24, Color{1, 0, 0, 1});
    Built b;
    b.builder.init(b.device);
    b.builder.resize(64, 64);
    b.builder.setEdgeFringe(false);
    b.builder.build(cl, nullptr, nullptr, nullptr, b.frame);
    checkFrameWellFormed(b.frame);
    REQUIRE(b.frame.cmds.size() == 1);
    CHECK(b.frame.cmds[0].vertexCount == 4);
    CHECK(b.frame.cmds[0].indexCount == 6);
    CHECK(b.frame.vertices[b.frame.cmds[0].baseVertex].x == 8.0f);
    CHECK(b.frame.vertices[b.frame.cmds[0].baseVertex].y == 8.0f);
}

TEST_CASE("gpu: T-155 a shape thinner than 1 px keeps a zero-width core and scales its alpha") {
    CommandList cl = makeList();
    addFillRect(cl, 10, 10, 20, 0.5f, Color{1, 0, 0, 1});
    Built b;
    buildInto(b, cl, 64, 64);
    const GpuFrame& f = b.frame;
    REQUIRE(f.cmds.size() == 1);
    const Vertex* v = &f.vertices[f.cmds[0].baseVertex];
    // Thickness 0.5 px: inset 0.25 (the core collapses onto the centre line), alpha x 0.5, the
    // ring 0.75 px outside so the ramp is still 1 px wide.
    CHECK(v[0].rgba == 0x80000080u);
    CHECK(v[0].y == doctest::Approx(10.25f));
    CHECK(v[2].y == doctest::Approx(10.25f));
    CHECK(v[4].y == doctest::Approx(9.25f));
    CHECK(v[6].y == doctest::Approx(11.25f));
    CHECK(v[4].rgba == 0u);
}

TEST_CASE("gpu: T-155 a gradient convex fill carries coverage in the vertex alpha") {
    CommandList cl = makeList();
    addFillGradient(cl, 10, 10, 20, 20);
    addFillGradient(cl, 40, 10, 20, 20);
    Built b;
    buildInto(b, cl, 64, 64);
    const GpuFrame& f = b.frame;
    // Same LUT, but the second rect's GradientParams differ (p0 moves), so it is its own command.
    auto grads = cmdsWith(f, Shading::Gradient);
    REQUIRE(grads.size() == 2);
    CHECK(grads[0]->paramIndex != grads[1]->paramIndex);
    CHECK(grads[0]->texture == grads[1]->texture);
    const Vertex* v = &f.vertices[grads[0]->baseVertex];
    CHECK(grads[0]->vertexCount == 8);
    CHECK((v[0].rgba >> 24) == 0xFFu);
    CHECK(v[4].rgba == 0u);
    // u,v are path-local: the inset corner sits at 10.5, the ring corner at 9.5.
    CHECK(v[0].u == doctest::Approx(10.5f));
    CHECK(v[0].v == doctest::Approx(10.5f));
    CHECK(v[4].u == doctest::Approx(9.5f));
}

/// One FillCmd holding two rectangle contours (`inner` reversed when `reverseInner`).
void addFillTwoRects(CommandList& cl, Rect outer, Rect inner, bool reverseInner, FillRule rule, Color color) {
    PathRef ref{};
    ref.verbOffset = static_cast<uint32_t>(cl.verbs.size());
    ref.pointOffset = static_cast<uint32_t>(cl.points.size());
    auto contour = [&](Rect r, bool rev) {
        cl.verbs.push_back(PathVerb::Move);
        cl.verbs.push_back(PathVerb::Line);
        cl.verbs.push_back(PathVerb::Line);
        cl.verbs.push_back(PathVerb::Line);
        cl.verbs.push_back(PathVerb::Close);
        float fwd[8] = {r.x, r.y, r.x + r.w, r.y, r.x + r.w, r.y + r.h, r.x, r.y + r.h};
        float bwd[8] = {r.x, r.y, r.x, r.y + r.h, r.x + r.w, r.y + r.h, r.x + r.w, r.y};
        const float* pts = rev ? bwd : fwd;
        cl.points.insert(cl.points.end(), pts, pts + 8);
    };
    contour(outer, false);
    contour(inner, reverseInner);
    ref.verbCount = 10;
    ref.pointCount = 16;
    FillCmd f{};
    f.path = ref;
    f.paint.kind = PaintKind::Solid;
    f.paint.color = color;
    f.rule = rule;
    f.antialias = 1;
    cl.fills.push_back(f);
    cl.commands.push_back(Command{CmdKind::FillPath, {}, static_cast<uint32_t>(cl.fills.size() - 1)});
}

TEST_CASE("gpu: T-155 a convex ring (border) is one zipped direct command") {
    CommandList cl = makeList();
    addFillTwoRects(cl, Rect{8, 8, 40, 30}, Rect{12, 12, 32, 22}, false, FillRule::EvenOdd,
                    Color{1, 0, 0, 1});
    Built b;
    buildInto(b, cl, 64, 64);
    const GpuFrame& f = b.frame;
    REQUIRE(f.cmds.size() == 1);
    const DrawCmd& c = f.cmds[0];
    CHECK(c.shading == Shading::Solid);
    CHECK(c.stencilOp == StencilOp::TestEqualRef);
    CHECK(c.vertexCount == 16);  // outer 4 + inner 4 cores, then both fringe rings
    CHECK(c.indexCount == 8 * 3 + 8 * 6);
    const Vertex* v = &f.vertices[c.baseVertex];
    // Outer core inset, inner core pushed into the 4 px band, both fringes 0.5 px beyond.
    CHECK(v[0].x == doctest::Approx(8.5f));
    CHECK(v[0].rgba == 0xFF0000FFu);
    bool sawInnerCore = false;
    for (int i = 4; i < 8; ++i) {
        if (v[i].x == doctest::Approx(11.5f) && v[i].y == doctest::Approx(11.5f)) sawInnerCore = true;
        CHECK(v[i].rgba == 0xFF0000FFu);
    }
    CHECK(sawInnerCore);
    for (int i = 8; i < 16; ++i) {
        CHECK(v[i].rgba == 0u);
    }
    CHECK(v[8].x == doctest::Approx(7.5f));
    for (uint32_t i = c.firstIndex; i < c.firstIndex + c.indexCount; ++i) {
        CHECK(f.indices[i] < 16);
    }
    // Every band triangle touches both contours (no triangle lies entirely on one ring).
    for (uint32_t t = 0; t < 8; ++t) {
        const uint32_t* tri = &f.indices[c.firstIndex + t * 3];
        int outerCount = 0;
        for (int k = 0; k < 3; ++k) outerCount += tri[k] < 4 ? 1 : 0;
        CHECK(outerCount >= 1);
        CHECK(outerCount <= 2);
    }
}

TEST_CASE("gpu: T-155 NonZero rings need opposite windings; nested same-winding stays stencil") {
    CommandList cl = makeList();
    addFillTwoRects(cl, Rect{8, 8, 40, 30}, Rect{12, 12, 32, 22}, true, FillRule::NonZero, Color{1, 0, 0, 1});
    addFillTwoRects(cl, Rect{8, 40, 40, 20}, Rect{12, 44, 32, 12}, false, FillRule::NonZero,
                    Color{0, 1, 0, 1});
    Built b;
    buildInto(b, cl, 64, 64);
    const GpuFrame& f = b.frame;
    // ring (1) | winding, cover, clear (3)
    REQUIRE(f.cmds.size() == 4);
    CHECK(f.cmds[0].shading == Shading::Solid);
    CHECK(f.cmds[0].vertexCount == 16);
    CHECK(f.cmds[1].stencilOp == StencilOp::WriteIncrWrap);
    CHECK(f.cmds[3].stencilOp == StencilOp::ClearRect);
}

TEST_CASE("gpu: T-155 disjoint convex contours of one path are fans in one command") {
    CommandList cl = makeList();
    addFillTwoRects(cl, Rect{0, 0, 10, 10}, Rect{20, 0, 10, 10}, false, FillRule::NonZero, Color{1, 0, 0, 1});
    addFillTwoRects(cl, Rect{0, 20, 10, 10}, Rect{10.5f, 20, 10, 10}, false, FillRule::NonZero,
                    Color{0, 1, 0, 1});  // boxes 0.5 px apart: the fringes would overlap
    Built b;
    buildInto(b, cl, 64, 64);
    const GpuFrame& f = b.frame;
    REQUIRE(f.cmds.size() == 4);
    CHECK(f.cmds[0].shading == Shading::Solid);
    CHECK(f.cmds[0].vertexCount == 16);
    CHECK(f.cmds[0].indexCount == 60);
    CHECK(f.cmds[1].stencilOp == StencilOp::WriteIncrWrap);
}

TEST_CASE("gpu: T-155 a rectangular PushClipPath becomes a scissor level") {
    CommandList cl = makeList();
    addPushClipPath(cl, 4.4f, 4.6f, 40, 40, /*concave=*/false);
    addPushLayer(cl, 0.5f, BlendMode::Normal);
    addFillRect(cl, 0, 0, 64, 64, Color{0, 0, 1, 1});
    addPopLayer(cl);
    addPopClip(cl);
    addFillConcave(cl, 0, 0, 64, 64, Color{1, 0, 0, 1});
    Built b;
    buildInto(b, cl, 64, 64);
    const GpuFrame& f = b.frame;
    CHECK(cmdsWith(f, StencilOp::ClipPush).empty());
    CHECK(cmdsWith(f, StencilOp::ClipPop).empty());
    // Layer pass: nothing to replay, just the fill under the pixel-centre scissor (the surface
    // pass before it drew nothing and is dropped).
    REQUIRE(f.passes.size() == 2);
    CHECK(f.passes[0].target == 1);
    CHECK(f.passes[0].cmdCount == 1);
    CHECK(f.cmds[0].clipX == 4);
    CHECK(f.cmds[0].clipY == 5);
    CHECK(f.cmds[0].clipW == 40);
    CHECK(f.cmds[0].clipH == 40);
    CHECK(f.cmds[0].stencilRef == 0);
    CHECK(f.cmds.back().clipW == 64);
}

TEST_CASE("gpu: T-155 consecutive draws of one state batch into one command") {
    CommandList cl = makeList();
    addFillRect(cl, 0, 0, 10, 10, Color{1, 0, 0, 1});
    addFillRect(cl, 20, 0, 10, 10, Color{0, 1, 0, 1});
    addFillRect(cl, 40, 0, 10, 10, Color{0, 0, 1, 0.5f});
    addPushClipRect(cl, Rect{0, 0, 32, 32});
    addFillRect(cl, 0, 20, 10, 10, Color{1, 1, 0, 1});
    addPopClip(cl);
    addFillGradient(cl, 10, 40, 20, 20);
    addFillGradient(cl, 10, 40, 20, 20);
    Built b;
    buildInto(b, cl, 64, 64);
    const GpuFrame& f = b.frame;
    REQUIRE(f.cmds.size() == 3);
    // 1: three rects (different colors are per-vertex) -> one command, indices rebased.
    CHECK(f.cmds[0].shading == Shading::Solid);
    CHECK(f.cmds[0].vertexCount == 24);
    CHECK(f.cmds[0].indexCount == 90);
    uint32_t maxIndex = 0;
    for (uint32_t i = f.cmds[0].firstIndex; i < f.cmds[0].firstIndex + f.cmds[0].indexCount; ++i) {
        maxIndex = std::max(maxIndex, f.indices[i]);
    }
    CHECK(maxIndex == 23);
    CHECK(f.vertices[f.cmds[0].baseVertex + 8].rgba == 0xFF00FF00u);
    // 2: a different scissor starts a new command.
    CHECK(f.cmds[1].clipW == 32);
    CHECK(f.cmds[1].vertexCount == 8);
    // 3: two identical gradients share their GradientParams block and batch.
    CHECK(f.cmds[2].shading == Shading::Gradient);
    CHECK(f.cmds[2].vertexCount == 16);
    CHECK(f.params.size() == sizeof(GradientParams) / sizeof(float));
}

TEST_CASE("gpu: T-155 stencil fills defer their ClearRect until a winding overlaps or state changes") {
    CommandList cl = makeList();
    addFillConcave(cl, 0, 0, 20, 20, Color{1, 0, 0, 1});
    addFillConcave(cl, 30, 0, 20, 20, Color{0, 1, 0, 1});   // disjoint AABB: clear waits
    addFillConcave(cl, 10, 10, 20, 20, Color{0, 0, 1, 1});  // overlaps the first: flush first
    addPushClipRect(cl, Rect{0, 0, 32, 32});                // scissor change: flush
    addFillConcave(cl, 0, 0, 20, 20, Color{1, 1, 0, 1});
    addPopClip(cl);
    Built b;
    buildInto(b, cl, 64, 64);
    const GpuFrame& f = b.frame;
    // W C | W C | Clear(2 rects) | W C | Clear(1) | W C | Clear(1)
    REQUIRE(f.cmds.size() == 11);
    CHECK(f.cmds[0].stencilOp == StencilOp::WriteIncrWrap);
    CHECK(f.cmds[1].stencilOp == StencilOp::TestNotEqualZero);
    CHECK(f.cmds[2].stencilOp == StencilOp::WriteIncrWrap);
    CHECK(f.cmds[3].stencilOp == StencilOp::TestNotEqualZero);
    CHECK(f.cmds[4].stencilOp == StencilOp::ClearRect);
    CHECK(f.cmds[4].vertexCount == 8);
    CHECK(f.cmds[4].indexCount == 12);
    CHECK(f.cmds[4].clipW == 64);
    CHECK(f.cmds[5].stencilOp == StencilOp::WriteIncrWrap);
    CHECK(f.cmds[7].stencilOp == StencilOp::ClearRect);
    CHECK(f.cmds[7].vertexCount == 4);
    CHECK(f.cmds[7].clipW == 64);  // flushed before the clip narrowed the scissor
    CHECK(f.cmds[8].clipW == 32);
    CHECK(f.cmds[10].stencilOp == StencilOp::ClearRect);
    CHECK(f.cmds[10].clipW == 32);
    // Covers keep full alpha (the fringe belongs to the convex path only).
    CHECK(f.vertices[f.cmds[1].baseVertex].rgba == 0xFF0000FFu);
    CHECK(f.cmds[1].vertexCount == 4);
}

// =================================================================================================
// 2. Clip: axis-aligned rect -> scissor only; rotated rect / path -> stencil.
// =================================================================================================

TEST_CASE("gpu: axis-aligned PushClipRect only narrows the scissor") {
    CommandList cl = makeList();
    addPushClipRect(cl, Rect{10, 12, 20, 16});
    addFillRect(cl, 0, 0, 64, 64, Color{0, 0, 1, 1});
    addPushClipRect(cl, Rect{0, 0, 15, 100});  // nested: intersection
    addFillRect(cl, 0, 0, 64, 64, Color{0, 1, 0, 1});
    addPopClip(cl);
    addPopClip(cl);
    addFillRect(cl, 0, 0, 64, 64, Color{1, 0, 0, 1});
    Built b;
    buildInto(b, cl, 64, 64);
    const GpuFrame& f = b.frame;
    // Three convex fills, one direct command each (different scissors keep them apart).
    REQUIRE(f.cmds.size() == 3);
    CHECK(cmdsWith(f, StencilOp::ClipPush).empty());
    CHECK(cmdsWith(f, StencilOp::ClipPop).empty());
    for (const DrawCmd& c : f.cmds) {
        CHECK(c.stencilRef == 0);
        CHECK(c.shading == Shading::Solid);
    }
    CHECK(f.cmds[0].clipX == 10);
    CHECK(f.cmds[0].clipY == 12);
    CHECK(f.cmds[0].clipW == 20);
    CHECK(f.cmds[0].clipH == 16);
    CHECK(f.cmds[1].clipX == 10);
    CHECK(f.cmds[1].clipW == 5);
    CHECK(f.cmds[1].clipH == 16);
    CHECK(f.cmds[2].clipX == 0);
    CHECK(f.cmds[2].clipW == 64);
    checkSnapshot("clip_rect_aligned", f);
}

TEST_CASE("gpu: fractional PushClipRect follows pixel-center coverage") {
    CommandList cl = makeList();
    addPushClipRect(
        cl, Rect{10.4f, 10.6f, 20.0f, 20.0f});  // x: 10.4..30.4 -> pixels 10..29; y: 10.6..30.6 -> 11..30
    addFillRect(cl, 0, 0, 64, 64, Color{0, 0, 1, 1});
    addPopClip(cl);
    Built b;
    buildInto(b, cl, 64, 64);
    CHECK(b.frame.cmds[0].clipX == 10);
    CHECK(b.frame.cmds[0].clipW == 20);
    CHECK(b.frame.cmds[0].clipY == 11);
    CHECK(b.frame.cmds[0].clipH == 20);
}

TEST_CASE("gpu: an empty scissor suppresses draws but keeps the clip stack balanced") {
    CommandList cl = makeList();
    addPushClipRect(cl, Rect{100, 100, 10, 10});  // fully outside the 64x64 surface
    addFillRect(cl, 0, 0, 64, 64, Color{0, 0, 1, 1});
    addPopClip(cl);
    addFillRect(cl, 0, 0, 64, 64, Color{1, 0, 0, 1});
    Built b;
    buildInto(b, cl, 64, 64);
    CHECK(b.frame.cmds.size() == 1);
    CHECK(b.frame.cmds[0].clipW == 64);
}

TEST_CASE("gpu: rotated PushClipRect becomes a stencil clip level") {
    CommandList cl = makeList();
    addPushClipRect(cl, Rect{-10, -10, 20, 20}, mul(translation(Vec2{32, 32}), rotationDeg(30.0f)));
    addFillRect(cl, 0, 0, 64, 64, Color{0, 0, 1, 1});
    addPopClip(cl);
    addFillRect(cl, 0, 0, 64, 64, Color{1, 0, 0, 1});
    Built b;
    buildInto(b, cl, 64, 64);
    const GpuFrame& f = b.frame;
    // winding(4 corners) + ClipPush | convex fill (1, ref 1) | ClipPop | convex fill (1, ref 0)
    REQUIRE(f.cmds.size() == 5);
    CHECK(f.cmds[0].stencilOp == StencilOp::WriteIncrWrap);
    CHECK(f.cmds[0].stencilRef == 0);
    CHECK(f.cmds[0].shading == Shading::StencilOnly);
    CHECK(f.cmds[1].stencilOp == StencilOp::ClipPush);
    CHECK(f.cmds[1].stencilRef == 1);
    CHECK(f.cmds[2].stencilOp == StencilOp::TestEqualRef);
    CHECK(f.cmds[2].stencilRef == 1);
    CHECK(f.cmds[2].shading == Shading::Solid);
    CHECK(f.cmds[3].stencilOp == StencilOp::ClipPop);
    CHECK(f.cmds[3].stencilRef == 1);
    CHECK(f.cmds[4].stencilRef == 0);
    // The stencil level narrows the scissor to the rotated rect's AABB (about 32 +- 13.66).
    CHECK(f.cmds[2].clipX >= 17);
    CHECK(f.cmds[2].clipX <= 19);
    CHECK(f.cmds[2].clipW <= 30);
    CHECK(f.cmds[4].clipW == 64);
    checkSnapshot("clip_rect_rotated", f);
}

TEST_CASE("gpu: PushClipPath nests stencil depth and replays into a layer") {
    CommandList cl = makeList();
    addPushClipPath(cl, 4, 4, 40, 40);
    addPushClipPath(cl, 8, 8, 40, 40);
    addPushLayer(cl, 0.5f, BlendMode::Normal);
    addFillRect(cl, 0, 0, 64, 64, Color{0, 0, 1, 1});
    addPopLayer(cl);
    addPopClip(cl);
    addPopClip(cl);
    Built b;
    buildInto(b, cl, 64, 64);
    const GpuFrame& f = b.frame;
    // Surface: 2 levels (4 cmds). Layer pass: replay 2 levels (4 cmds) + convex fill at ref 2
    // (1 cmd). Surface again: composite, pop(ref 2), pop(ref 1).
    REQUIRE(f.passes.size() == 3);
    REQUIRE(f.cmds.size() == 12);
    CHECK(f.passes[0].cmdCount == 4);
    CHECK(f.passes[1].target == 1);
    CHECK(f.passes[1].clear == 1);
    CHECK(f.passes[1].cmdCount == 5);
    CHECK(f.passes[2].cmdCount == 3);
    CHECK(f.cmds[3].stencilOp == StencilOp::ClipPush);
    CHECK(f.cmds[3].stencilRef == 2);
    CHECK(f.cmds[7].stencilOp == StencilOp::ClipPush);
    CHECK(f.cmds[7].stencilRef == 2);
    CHECK(f.cmds[8].stencilRef == 2);
    CHECK(f.cmds[8].stencilOp == StencilOp::TestEqualRef);
    CHECK(f.cmds[9].shading == Shading::Composite);
    CHECK(f.cmds[10].stencilOp == StencilOp::ClipPop);
    CHECK(f.cmds[10].stencilRef == 2);
    CHECK(f.cmds[11].stencilOp == StencilOp::ClipPop);
    CHECK(f.cmds[11].stencilRef == 1);
    REQUIRE(f.transients.size() == 1);
    CHECK(f.transients[0].needStencil == 1);
    CHECK(f.transients[0].width == 64);
    checkSnapshot("clip_path_layer", f);
}

// =================================================================================================
// 3. Layer / mask / effect scheduling.
// =================================================================================================

TEST_CASE("gpu: PushLayer/PopLayer with Normal blend is one transient and a SrcOver composite") {
    CommandList cl = makeList();
    addFillRect(cl, 0, 0, 64, 64, Color{1, 1, 1, 1});
    addPushLayer(cl, 0.5f, BlendMode::Normal);
    addFillRect(cl, 8, 8, 16, 16, Color{1, 0, 0, 1});
    addPopLayer(cl);
    addFillRect(cl, 0, 0, 4, 4, Color{0, 0, 0, 1});
    Built b;
    buildInto(b, cl, 64, 64);
    const GpuFrame& f = b.frame;
    REQUIRE(f.transients.size() == 1);
    REQUIRE(f.passes.size() == 3);
    CHECK(f.passes[0].target == 0);
    CHECK(f.passes[1].target == 1);
    CHECK(f.passes[1].clear == 1);
    CHECK(f.passes[1].copy == 0);
    CHECK(f.passes[2].target == 0);
    CHECK(f.passes[2].clear == 0);
    const DrawCmd& comp = f.cmds[f.passes[2].firstCmd];
    CHECK(comp.shading == Shading::Composite);
    CHECK(comp.blend == Blend::SrcOver);
    CHECK(comp.src == 1);
    CHECK(comp.aux == 0);
    CHECK(comp.stencilOp == StencilOp::None);
    CompositeParams cp{};
    std::memcpy(&cp, f.params.data() + comp.paramIndex, sizeof(cp));
    CHECK(cp.mode == 0.0f);
    CHECK(cp.opacity == 0.5f);
    // Fullscreen quad: u,v span (0,0)..(1,1).
    CHECK(f.vertices[comp.baseVertex].u == 0.0f);
    CHECK(f.vertices[comp.baseVertex + 2].u == 1.0f);
    CHECK(f.vertices[comp.baseVertex + 2].v == 1.0f);
    CHECK(f.vertices[comp.baseVertex + 2].x == 64.0f);
    checkSnapshot("layer_normal", f);
}

TEST_CASE("gpu: PushLayer with a blend mode copies the backdrop and composites with Replace") {
    CommandList cl = makeList();
    addPushLayer(cl, 1.0f, BlendMode::Multiply);
    addFillRect(cl, 8, 8, 16, 16, Color{1, 0, 0, 1});
    addPopLayer(cl);
    Built b;
    buildInto(b, cl, 64, 64);
    const GpuFrame& f = b.frame;
    REQUIRE(f.transients.size() == 2);
    CHECK(f.transients[1].needStencil == 0);
    REQUIRE(f.passes.size() == 3);
    CHECK(f.passes[0].target == 1);  // the layer (the empty leading surface pass is dropped)
    CHECK(f.passes[1].target == 2);
    CHECK(f.passes[1].copy == 1);
    CHECK(f.passes[1].copySource == 0);
    CHECK(f.passes[1].cmdCount == 0);
    CHECK(f.passes[2].target == 0);
    const DrawCmd& comp = f.cmds[f.passes[2].firstCmd];
    CHECK(comp.shading == Shading::Composite);
    CHECK(comp.blend == Blend::Replace);
    CHECK(comp.src == 1);
    CHECK(comp.aux == 2);
    CompositeParams cp{};
    std::memcpy(&cp, f.params.data() + comp.paramIndex, sizeof(cp));
    CHECK(cp.mode == static_cast<float>(static_cast<int>(BlendMode::Multiply)));
    checkSnapshot("layer_multiply", f);
}

TEST_CASE("gpu: nested layers reuse released transient slots") {
    CommandList cl = makeList();
    for (int i = 0; i < 3; ++i) {
        addPushLayer(cl, 0.5f, BlendMode::Normal);
        addFillRect(cl, 8, 8, 16, 16, Color{1, 0, 0, 1});
        addPopLayer(cl);
    }
    Built b;
    buildInto(b, cl, 64, 64);
    CHECK(b.frame.transients.size() == 1);  // sequential layers share one slot
    CHECK(cmdsWith(b.frame, Shading::Composite).size() == 3);

    CommandList nested = makeList();
    addPushLayer(nested, 0.5f, BlendMode::Normal);
    addPushLayer(nested, 0.5f, BlendMode::Normal);
    addFillRect(nested, 8, 8, 16, 16, Color{1, 0, 0, 1});
    addPopLayer(nested);
    addPopLayer(nested);
    Built b2;
    buildInto(b2, nested, 64, 64);
    CHECK(b2.frame.transients.size() == 2);
    const GpuFrame& f = b2.frame;
    auto comps = cmdsWith(f, Shading::Composite);
    REQUIRE(comps.size() == 2);
    CHECK(comps[0]->src == 2);
    CHECK(comps[1]->src == 1);
    // The inner composite is drawn into the outer layer's target, the outer one into the surface.
    bool innerOnLayer = false, outerOnSurface = false;
    for (const Pass& p : f.passes) {
        for (uint32_t c = p.firstCmd; c < p.firstCmd + p.cmdCount; ++c) {
            if (&f.cmds[c] == comps[0]) innerOnLayer = p.target == 1;
            if (&f.cmds[c] == comps[1]) outerOnSurface = p.target == 0;
        }
    }
    CHECK(innerOnLayer);
    CHECK(outerOnSurface);
    checkSnapshot("layer_nested", f);
}

TEST_CASE("gpu: PushMask emits a MaskApply pass and a Normal composite") {
    CommandList cl = makeList();
    addPushMask(cl, MaskMode::Luma);
    addFillRect(cl, 0, 0, 32, 64, Color{1, 1, 1, 1});  // mask shape
    cl.commands.push_back(Command{CmdKind::EndMaskContent, {}, 0});
    addFillRect(cl, 0, 0, 64, 64, Color{1, 0, 0, 1});  // content
    cl.commands.push_back(Command{CmdKind::PopMask, {}, 0});
    Built b;
    buildInto(b, cl, 64, 64);
    const GpuFrame& f = b.frame;
    REQUIRE(f.transients.size() == 3);
    CHECK(f.transients[0].needStencil == 1);
    CHECK(f.transients[1].needStencil == 1);
    CHECK(f.transients[2].needStencil == 0);
    auto effects = cmdsWith(f, Shading::Effect);
    REQUIRE(effects.size() == 1);
    CHECK(effects[0]->program == EffectProgram::MaskApply);
    CHECK(effects[0]->src == 2);  // content
    CHECK(effects[0]->aux == 1);  // mask shape
    MaskApplyParams mp{};
    std::memcpy(&mp, f.params.data() + effects[0]->paramIndex, sizeof(mp));
    CHECK(mp.mode == 2.0f);
    auto comps = cmdsWith(f, Shading::Composite);
    REQUIRE(comps.size() == 1);
    CHECK(comps[0]->src == 3);
    CHECK(comps[0]->blend == Blend::SrcOver);
    checkSnapshot("mask_luma", f);
}

namespace {

std::vector<EffectProgram> effectSequence(const GpuFrame& f) {
    std::vector<EffectProgram> out;
    for (const DrawCmd& c : f.cmds) {
        if (c.shading == Shading::Effect) out.push_back(c.program);
    }
    return out;
}

/// Builds `effects` around one filled rect and returns the frame.
void buildEffect(Built& b, const std::vector<EffectInstance>& effects, CommandList& cl,
                 bool backdropContent = false) {
    if (backdropContent) {
        addFillRect(cl, 0, 0, 64, 64, Color{0, 0, 1, 1});
    }
    Rect content{16, 16, 32, 32};
    Rect bounds{0, 0, 64, 64};
    addPushEffect(cl, effects, Mat2x3{}, bounds, content);
    addFillRect(cl, 16, 16, 32, 32, Color{1, 0, 0, 1});
    addPopEffect(cl);
    buildInto(b, cl, 64, 64);
}

}  // namespace

TEST_CASE("gpu: Blur effect is two GaussColor passes then a Normal composite (gl_effects.cpp order)") {
    Built b;
    CommandList cl = makeList();
    buildEffect(b, {blurFx(4, 4)}, cl);
    const GpuFrame& f = b.frame;
    using P = EffectProgram;
    CHECK(effectSequence(f) == std::vector<P>{P::GaussColor, P::GaussColor});
    auto fx = cmdsWith(f, Shading::Effect);
    CHECK(fx[0]->src == 1);  // the effect layer
    CHECK(fx[0]->aux == 0);
    CHECK(fx[1]->src != fx[0]->src);
    GaussParams gp{};
    std::memcpy(&gp, f.params.data() + fx[0]->paramIndex, sizeof(gp));
    CHECK(gp.dirX == 1.0f);
    CHECK(gp.dirY == 0.0f);
    CHECK(gp.texelW == doctest::Approx(1.0f / 64.0f));
    CHECK(gp.tapCount >= 1.0f);
    CHECK(gp.weights[0] > 0.0f);
    std::memcpy(&gp, f.params.data() + fx[1]->paramIndex, sizeof(gp));
    CHECK(gp.dirY == 1.0f);
    auto comps = cmdsWith(f, Shading::Composite);
    REQUIRE(comps.size() == 1);
    CHECK(comps[0]->blend == Blend::SrcOver);
    // The composite reads the last pass's target.
    const Pass* lastFxPass = nullptr;
    for (const Pass& p : f.passes) {
        for (uint32_t c = p.firstCmd; c < p.firstCmd + p.cmdCount; ++c) {
            if (&f.cmds[c] == fx[1]) lastFxPass = &p;
        }
    }
    REQUIRE(lastFxPass != nullptr);
    CHECK(comps[0]->src == lastFxPass->target);
    CHECK(f.transients.size() == 3);  // effect layer + 2 scratch
    checkSnapshot("effect_blur", f);
}

TEST_CASE("gpu: DropShadow / inset / Glow / Outline / ColorAdjust / GradientMask / Shine pass sequences") {
    using P = EffectProgram;
    {
        Built b;
        CommandList cl = makeList();
        buildEffect(b, {dropShadow(6, 6, 4, 2, 1, Color{0, 0, 0, 1}, false)}, cl);
        CHECK(effectSequence(b.frame) ==
              std::vector<P>{P::Dilate, P::Dilate, P::GaussScalar, P::GaussScalar, P::ShadowGlow});
        auto fx = cmdsWith(b.frame, Shading::Effect);
        DilateParams dp{};
        std::memcpy(&dp, b.frame.params.data() + fx[0]->paramIndex, sizeof(dp));
        CHECK(dp.radius == 2.0f);
        CHECK(dp.readAlpha == 1.0f);
        ShadowGlowParams sp{};
        std::memcpy(&sp, b.frame.params.data() + fx[4]->paramIndex, sizeof(sp));
        CHECK(sp.offsetU == doctest::Approx(6.0f / 64.0f));
        CHECK(sp.offsetV == doctest::Approx(6.0f / 64.0f));  // y down: no sign flip
        CHECK(sp.lineOnTop == 0.0f);
        CHECK(fx[4]->src == 1);
        CHECK(fx[4]->aux != 0);
        checkSnapshot("effect_drop_shadow", b.frame);
    }
    {
        Built b;
        CommandList cl = makeList();
        buildEffect(b, {dropShadow(4, 4, 6, 2, 1, Color{0, 0, 0, 1}, true)}, cl);
        CHECK(effectSequence(b.frame) == std::vector<P>{P::AlphaExtract, P::Dilate, P::Dilate, P::GaussScalar,
                                                        P::GaussScalar, P::ShadowGlow});
        auto fx = cmdsWith(b.frame, Shading::Effect);
        GaussParams gp{};
        std::memcpy(&gp, b.frame.params.data() + fx[3]->paramIndex, sizeof(gp));
        CHECK(gp.constEdge == 1.0f);
        CHECK(gp.outside == 1.0f);
        ShadowGlowParams sp{};
        std::memcpy(&sp, b.frame.params.data() + fx[5]->paramIndex, sizeof(sp));
        CHECK(sp.lineOnTop == 1.0f);
        CHECK(sp.effectAlphaMul == 1.0f);
        CHECK(sp.maskOutside == 1.0f);
        checkSnapshot("effect_drop_shadow_inset", b.frame);
    }
    {
        Built b;
        CommandList cl = makeList();
        buildEffect(b, {glow(6, Color{1, 1, 0, 1}, 1, false)}, cl);
        CHECK(effectSequence(b.frame) ==
              std::vector<P>{P::AlphaExtract, P::GaussScalar, P::GaussScalar, P::ShadowGlow});
        checkSnapshot("effect_glow", b.frame);
    }
    {
        Built b;
        CommandList cl = makeList();
        buildEffect(b, {outline(3, 1, 0, Color{0, 0, 0, 1})}, cl);
        CHECK(effectSequence(b.frame) == std::vector<P>{P::Sdf, P::Outline});
        bool haveR32f = false;
        for (const TransientTargetDesc& t : b.frame.transients) {
            if (t.format == TextureFormat::R32F) haveR32f = true;
        }
        CHECK(haveR32f);
        auto fx = cmdsWith(b.frame, Shading::Effect);
        // The Outline pass reads the SDF target the Sdf pass wrote.
        const Pass* sdfPass = nullptr;
        for (const Pass& p : b.frame.passes) {
            for (uint32_t c = p.firstCmd; c < p.firstCmd + p.cmdCount; ++c) {
                if (&b.frame.cmds[c] == fx[0]) sdfPass = &p;
            }
        }
        REQUIRE(sdfPass != nullptr);
        CHECK(fx[1]->aux == sdfPass->target);
        CHECK(b.frame.transients[sdfPass->target - 1].format == TextureFormat::R32F);
        checkSnapshot("effect_outline", b.frame);
    }
    {
        Built b;
        CommandList cl = makeList();
        buildEffect(b, {colorAdjust(0.1f, 0.2f, -0.5f, 90.0f, 0.5f)}, cl);
        CHECK(effectSequence(b.frame) == std::vector<P>{P::ColorMatrix});
        auto fx = cmdsWith(b.frame, Shading::Effect);
        ColorMatrixParams cp{};
        std::memcpy(&cp, b.frame.params.data() + fx[0]->paramIndex, sizeof(cp));
        CHECK(cp.opacity == 0.5f);
        checkSnapshot("effect_color_adjust", b.frame);
    }
    {
        Built b;
        CommandList cl = makeList();
        std::vector<EffectInstance> fxs{gradientMask(cl, Vec2{16, 16}, Vec2{48, 16}, false, true)};
        buildEffect(b, fxs, cl);
        CHECK(effectSequence(b.frame) == std::vector<P>{P::GradientMask});
        auto fx = cmdsWith(b.frame, Shading::Effect);
        CHECK(fx[0]->texture != 0);  // the stop LUT
        GradientMaskParams gp{};
        std::memcpy(&gp, b.frame.params.data() + fx[0]->paramIndex, sizeof(gp));
        CHECK(gp.invert == 1.0f);
        CHECK(gp.radial == 0.0f);
        CHECK(gp.targetW == 64.0f);
        checkSnapshot("effect_gradient_mask", b.frame);
    }
    {
        Built b;
        CommandList cl = makeList();
        buildEffect(b, {shine(0.5f, 10, 0.5f, 0, 1, Color{1, 1, 1, 1})}, cl);
        CHECK(effectSequence(b.frame) == std::vector<P>{P::Shine});
        auto fx = cmdsWith(b.frame, Shading::Effect);
        ShineParams sp{};
        std::memcpy(&sp, b.frame.params.data() + fx[0]->paramIndex, sizeof(sp));
        CHECK(sp.center == doctest::Approx(32.0f));  // content 16..48 along +x, position 0.5
        CHECK(sp.edge1 == 5.0f);
        checkSnapshot("effect_shine", b.frame);
    }
}

TEST_CASE("gpu: BackdropBlur copies the parent before the layer opens") {
    using P = EffectProgram;
    Built b;
    CommandList cl = makeList();
    buildEffect(b, {backdropBlur(8, 0.5f, 0.0f)}, cl, true);
    const GpuFrame& f = b.frame;
    // T-343 (18 §18.15): the copy becomes a two-level pyramid (sigma 4 lies between levels 1
    // and 2), the panel mixes the two, adjusts, composes.
    CHECK(effectSequence(f) == std::vector<P>{P::Downsample, P::Kawase, P::Downsample, P::Kawase, P::LevelMix,
                                              P::ColorMatrix, P::Backdrop});
    // Pass order: surface fill, backdrop copy from surface, the pyramid, layer (clear), fx
    // passes, composite.
    REQUIRE(f.passes.size() >= 8);
    CHECK(f.passes[0].target == 0);
    CHECK(f.passes[1].copy == 1);
    CHECK(f.passes[1].copySource == 0);
    CHECK(f.passes[6].clear == 1);
    auto fx = cmdsWith(f, Shading::Effect);
    CHECK(fx[0]->src == f.passes[1].target);  // the first downsample reads the backdrop copy
    CHECK(fx[4]->src == fx[1]->src + 1);      // LevelMix mixes level 1 ...
    CHECK(fx[4]->aux != 0);                   // ... with level 2
    CHECK(fx[6]->src == f.passes[6].target);  // Backdrop composes the node's own content...
    CHECK(fx[6]->aux != 0);                   // ...over the adjusted blur
    // The pyramid levels are scaled transients: half and quarter the window.
    bool half = false, quarter = false;
    for (const TransientTargetDesc& t : f.transients) {
        half = half || (t.width == 32 && t.height == 32);
        quarter = quarter || (t.width == 16 && t.height == 16);
    }
    CHECK(half);
    CHECK(quarter);
    checkSnapshot("effect_backdrop_blur", f);
}

TEST_CASE("gpu: stacked effects chain scratch targets and an empty stack composites directly") {
    using P = EffectProgram;
    Built b;
    CommandList cl = makeList();
    // Radius 4 (radius 2 is an identity blur under the SW-effective sigma and would be culled).
    buildEffect(b, {blurFx(4, 4), dropShadow(2, 2, 0, 0, 1, Color{0, 0, 0, 1}, false)}, cl);
    // T-151: the hard shadow's identity Dilate/Gauss steps are culled (AlphaExtract + ShadowGlow).
    CHECK(effectSequence(b.frame) ==
          std::vector<P>{P::GaussColor, P::GaussColor, P::AlphaExtract, P::ShadowGlow});
    auto fx = cmdsWith(b.frame, Shading::Effect);
    // The shadow's src is the blur's output, not the original layer.
    const Pass* blurOut = nullptr;
    for (const Pass& p : b.frame.passes) {
        for (uint32_t c = p.firstCmd; c < p.firstCmd + p.cmdCount; ++c) {
            if (&b.frame.cmds[c] == fx[1]) blurOut = &p;
        }
    }
    REQUIRE(blurOut != nullptr);
    CHECK(fx[3]->src == blurOut->target);
    checkSnapshot("effect_stack", b.frame);

    Built b2;
    CommandList cl2 = makeList();
    buildEffect(b2, {}, cl2);
    CHECK(cmdsWith(b2.frame, Shading::Effect).empty());
    auto comps = cmdsWith(b2.frame, Shading::Composite);
    REQUIRE(comps.size() == 1);
    CHECK(comps[0]->src == 1);
}

// =================================================================================================
// 3b. T-151: windowed transient targets (Docs/tasks/T-151.md points 1-4, 6).
// =================================================================================================

namespace {

/// The pass that holds `cmd`.
const Pass* passOf(const GpuFrame& f, const DrawCmd* cmd) {
    for (const Pass& p : f.passes) {
        for (uint32_t c = p.firstCmd; c < p.firstCmd + p.cmdCount; ++c) {
            if (&f.cmds[c] == cmd) return &p;
        }
    }
    return nullptr;
}

void checkQuad(const GpuFrame& f, const DrawCmd& c, float x0, float y0, float x1, float y1) {
    REQUIRE(c.vertexCount == 4);
    const Vertex* v = f.vertices.data() + c.baseVertex;
    CHECK(v[0].x == x0);
    CHECK(v[0].y == y0);
    CHECK(v[0].u == 0.0f);
    CHECK(v[0].v == 0.0f);
    CHECK(v[2].x == x1);
    CHECK(v[2].y == y1);
    CHECK(v[2].u == 1.0f);
    CHECK(v[2].v == 1.0f);
}

}  // namespace

TEST_CASE("gpu: T-151 a small drop-shadowed element on a large canvas gets window-sized targets") {
    Built b;
    CommandList cl = makeList(1920, 1080);
    const Rect content{500, 300, 100, 50};
    const Rect bounds{480, 280, 140, 90};  // content outset by the shadow's expansion
    addPushEffect(cl, {dropShadow(4, 4, 8, 2, 1, Color{0, 0, 0, 1}, false)}, Mat2x3{}, bounds, content);
    addFillRect(cl, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    addPopEffect(cl);
    buildInto(b, cl, 1920, 1080);
    const GpuFrame& f = b.frame;

    // Every transient is the hint's window padded to 32 px multiples (140x90 -> 160x96), never
    // the 1920x1080 surface.
    REQUIRE(!f.transients.empty());
    for (const TransientTargetDesc& t : f.transients) {
        CHECK(t.width == 160);
        CHECK(t.height == 96);
    }
    // Passes into transients carry that window; the surface pass stays whole.
    for (const Pass& p : f.passes) {
        if (p.target == kSurfaceTarget) {
            CHECK(p.viewX == 0);
            CHECK(p.viewY == 0);
            CHECK(p.viewW == 1920);
            CHECK(p.viewH == 1080);
        } else {
            CHECK(p.viewX == 480);
            CHECK(p.viewY == 280);
            CHECK(p.viewW == 160);
            CHECK(p.viewH == 96);
        }
    }
    // The layer's own fill keeps surface coordinates (no translation into the window).
    auto solids = cmdsWith(f, Shading::Solid);
    REQUIRE(!solids.empty());
    CHECK(f.vertices[solids[0]->baseVertex].x >= 500.0f);
    // Effect quads cover the window (surface pixels, u,v 0..1), scissored to it; texel sizes
    // and the shadow offset are relative to the window.
    auto fx = cmdsWith(f, Shading::Effect);
    REQUIRE(fx.size() == 5);
    checkQuad(f, *fx[0], 480, 280, 640, 376);
    CHECK(fx[0]->clipX == 480);
    CHECK(fx[0]->clipY == 280);
    CHECK(fx[0]->clipW == 160);
    CHECK(fx[0]->clipH == 96);
    DilateParams dp{};
    std::memcpy(&dp, f.params.data() + fx[0]->paramIndex, sizeof(dp));
    CHECK(dp.texelW == doctest::Approx(1.0f / 160.0f));
    CHECK(dp.texelH == doctest::Approx(1.0f / 96.0f));
    ShadowGlowParams sp{};
    std::memcpy(&sp, f.params.data() + fx[4]->paramIndex, sizeof(sp));
    CHECK(sp.offsetU == doctest::Approx(4.0f / 160.0f));
    CHECK(sp.offsetV == doctest::Approx(4.0f / 96.0f));
    // The composite draws the result's window quad onto the surface under the full scissor.
    auto comps = cmdsWith(f, Shading::Composite);
    REQUIRE(comps.size() == 1);
    checkQuad(f, *comps[0], 480, 280, 640, 376);
    CHECK(comps[0]->clipX == 0);
    CHECK(comps[0]->clipW == 1920);
    CHECK(passOf(f, comps[0])->target == kSurfaceTarget);
    checkSnapshot("effect_windowed_shadow", f);
}

TEST_CASE("gpu: T-151 windows follow the scissor, the hint's edge and the surface size") {
    // A hint partly outside the current clip: the window is the intersection, padded.
    {
        Built b;
        CommandList cl = makeList(1920, 1080);
        addPushClipRect(cl, Rect{0, 0, 550, 1080});
        addPushEffect(cl, {blurFx(4, 4)}, Mat2x3{}, Rect{500, 300, 200, 100}, Rect{510, 310, 180, 80});
        addFillRect(cl, 510, 310, 180, 80, Color{1, 0, 0, 1});
        addPopEffect(cl);
        addPopClip(cl);
        buildInto(b, cl, 1920, 1080);
        REQUIRE(!b.frame.transients.empty());
        CHECK(b.frame.transients[0].width == 64);  // 50 -> min 64
        CHECK(b.frame.transients[0].height == 128);
        const Pass& p = b.frame.passes[0];
        CHECK(p.target == 1);
        CHECK(p.viewX == 500);
        CHECK(p.viewY == 300);
        CHECK(p.viewW == 64);
        CHECK(p.viewH == 128);
        // Effect quads are scissored to the window, not to the clip.
        auto fx = cmdsWith(b.frame, Shading::Effect);
        REQUIRE(!fx.empty());
        CHECK(fx[0]->clipW == 64);
        // The composite keeps the clip's scissor.
        auto comps = cmdsWith(b.frame, Shading::Composite);
        REQUIRE(comps.size() == 1);
        CHECK(comps[0]->clipW == 550);
    }
    // Padding that would leave the surface slides the window back inside it.
    {
        Built b;
        CommandList cl = makeList(1920, 1080);
        addPushEffect(cl, {blurFx(4, 4)}, Mat2x3{}, Rect{1900, 1060, 20, 20}, Rect{1900, 1060, 20, 20});
        addFillRect(cl, 1900, 1060, 20, 20, Color{1, 0, 0, 1});
        addPopEffect(cl);
        buildInto(b, cl, 1920, 1080);
        const Pass& p = b.frame.passes[0];
        CHECK(p.viewX == 1920 - 64);
        CHECK(p.viewY == 1080 - 64);
        CHECK(p.viewW == 64);
        CHECK(p.viewH == 64);
    }
    // A surface smaller than the minimum window: the window is the surface (as before T-151).
    {
        Built b;
        CommandList cl = makeList(40, 30);
        addPushLayer(cl, 0.5f, BlendMode::Normal);
        addFillRect(cl, 0, 0, 10, 10, Color{1, 0, 0, 1});
        addPopLayer(cl);
        buildInto(b, cl, 40, 30);
        REQUIRE(b.frame.transients.size() == 1);
        CHECK(b.frame.transients[0].width == 40);
        CHECK(b.frame.transients[0].height == 30);
    }
    // An empty scissor still yields a (minimal) window so the push/pop stays balanced.
    {
        Built b;
        CommandList cl = makeList(256, 256);
        addPushClipRect(cl, Rect{300, 300, 10, 10});
        addPushLayer(cl, 0.5f, BlendMode::Normal);
        addFillRect(cl, 0, 0, 10, 10, Color{1, 0, 0, 1});
        addPopLayer(cl);
        addPopClip(cl);
        buildInto(b, cl, 256, 256);
        REQUIRE(b.frame.transients.size() == 1);
        CHECK(b.frame.transients[0].width == 64);
        CHECK(b.frame.transients[0].height == 64);
    }
}

TEST_CASE("gpu: T-151 layer, blend backdrop, mask and nested windows") {
    // A Multiply layer with a hint: the layer, its backdrop copy and the composite quad share
    // the hint's window; the copy pass carries it.
    {
        Built b;
        CommandList cl = makeList(1920, 1080);
        addFillRect(cl, 0, 0, 1920, 1080, Color{1, 1, 1, 1});
        LayerCmd l{};
        l.opacity = 1.0f;
        l.blend = BlendMode::Multiply;
        l.boundsHint = Rect{100, 100, 50, 50};
        cl.layers.push_back(l);
        cl.commands.push_back(Command{CmdKind::PushLayer, {}, static_cast<uint32_t>(cl.layers.size() - 1)});
        addFillRect(cl, 100, 100, 50, 50, Color{1, 0, 0, 1});
        addPopLayer(cl);
        buildInto(b, cl, 1920, 1080);
        const GpuFrame& f = b.frame;
        REQUIRE(f.transients.size() == 2);
        for (const TransientTargetDesc& t : f.transients) {
            CHECK(t.width == 64);
            CHECK(t.height == 64);
        }
        const Pass* copyPass = nullptr;
        for (const Pass& p : f.passes) {
            if (p.copy) copyPass = &p;
        }
        REQUIRE(copyPass != nullptr);
        CHECK(copyPass->copySource == kSurfaceTarget);
        CHECK(copyPass->viewX == 100);
        CHECK(copyPass->viewY == 100);
        CHECK(copyPass->viewW == 64);
        CHECK(copyPass->viewH == 64);
        auto comps = cmdsWith(f, Shading::Composite);
        REQUIRE(comps.size() == 1);
        CHECK(comps[0]->aux == 2);
        checkQuad(f, *comps[0], 100, 100, 164, 164);
    }
    // A mask (its hint is the lazy full viewport) inside a clip: both mask targets and the
    // MaskApply target take the clip's window.
    {
        Built b;
        CommandList cl = makeList(1920, 1080);
        addPushClipRect(cl, Rect{200, 200, 300, 300});
        addPushMask(cl, MaskMode::Luma);
        addFillRect(cl, 200, 200, 150, 300, Color{1, 1, 1, 1});
        cl.commands.push_back(Command{CmdKind::EndMaskContent, {}, 0});
        addFillRect(cl, 200, 200, 300, 300, Color{1, 0, 0, 1});
        cl.commands.push_back(Command{CmdKind::PopMask, {}, 0});
        addPopClip(cl);
        buildInto(b, cl, 1920, 1080);
        const GpuFrame& f = b.frame;
        REQUIRE(f.transients.size() == 3);
        for (const TransientTargetDesc& t : f.transients) {
            CHECK(t.width == 320);
            CHECK(t.height == 320);
        }
        auto fx = cmdsWith(f, Shading::Effect);
        REQUIRE(fx.size() == 1);
        CHECK(fx[0]->program == EffectProgram::MaskApply);
        checkQuad(f, *fx[0], 200, 200, 520, 520);
        auto comps = cmdsWith(f, Shading::Composite);
        REQUIRE(comps.size() == 1);
        checkQuad(f, *comps[0], 200, 200, 520, 520);
        CHECK(comps[0]->clipX == 200);
        CHECK(comps[0]->clipW == 300);
    }
    // A child window never leaves its parent's window (an effect hint wider than the layer's).
    {
        Built b;
        CommandList cl = makeList(1920, 1080);
        LayerCmd l{};
        l.opacity = 0.5f;
        l.blend = BlendMode::Normal;
        l.boundsHint = Rect{100, 100, 100, 100};
        cl.layers.push_back(l);
        cl.commands.push_back(Command{CmdKind::PushLayer, {}, static_cast<uint32_t>(cl.layers.size() - 1)});
        addPushEffect(cl, {blurFx(4, 4)}, Mat2x3{}, Rect{50, 50, 300, 300}, Rect{100, 100, 100, 100});
        addFillRect(cl, 100, 100, 100, 100, Color{1, 0, 0, 1});
        addPopEffect(cl);
        addPopLayer(cl);
        buildInto(b, cl, 1920, 1080);
        const GpuFrame& f = b.frame;
        for (const Pass& p : f.passes) {
            if (p.target != kSurfaceTarget) {
                CHECK(p.viewX == 100);
                CHECK(p.viewY == 100);
                CHECK(p.viewW == 128);
                CHECK(p.viewH == 128);
            }
        }
    }
}

TEST_CASE("gpu: T-151 GradientMask and Shine fold the window origin into their parameters") {
    Built b;
    CommandList cl = makeList(1920, 1080);
    const Rect content{500, 300, 100, 50};
    std::vector<EffectInstance> fx = {gradientMask(cl, Vec2{500, 325}, Vec2{600, 325}, false, false),
                                      shine(0.5f, 10, 0.5f, 0, 1, Color{1, 1, 1, 1})};
    addPushEffect(cl, fx, Mat2x3{}, content, content);
    addFillRect(cl, content.x, content.y, content.w, content.h, Color{1, 1, 1, 1});
    addPopEffect(cl);
    buildInto(b, cl, 1920, 1080);
    const GpuFrame& f = b.frame;
    auto effects = cmdsWith(f, Shading::Effect);
    REQUIRE(effects.size() == 2);
    // Window: (500,300) 100x50 -> 128x64.
    GradientMaskParams gp{};
    std::memcpy(&gp, f.params.data() + effects[0]->paramIndex, sizeof(gp));
    CHECK(gp.targetW == 128.0f);
    CHECK(gp.targetH == 64.0f);
    // invXf = identity * translation(origin): window px -> surface px.
    CHECK(gp.invXf[0] == 1.0f);
    CHECK(gp.invXf[3] == 1.0f);
    CHECK(gp.invXf[4] == 500.0f);
    CHECK(gp.invXf[5] == 300.0f);
    CHECK(gp.p0x == 500.0f);  // p0/p1 stay in the node's (surface) space
    CHECK(gp.p1x == 600.0f);
    ShineParams sp{};
    std::memcpy(&sp, f.params.data() + effects[1]->paramIndex, sizeof(sp));
    CHECK(sp.targetW == 128.0f);
    CHECK(sp.targetH == 64.0f);
    // Band center 550 in surface px along +x, minus the window origin's projection (500).
    CHECK(sp.center == doctest::Approx(50.0f));
}

TEST_CASE("gpu: T-151 identity Dilate/Gauss steps emit no pass") {
    using P = EffectProgram;
    {
        // spread 0: a single AlphaExtract replaces the two Dilates.
        Built b;
        CommandList cl = makeList();
        buildEffect(b, {dropShadow(6, 6, 4, 0, 1, Color{0, 0, 0, 1}, false)}, cl);
        CHECK(effectSequence(b.frame) ==
              std::vector<P>{P::AlphaExtract, P::GaussScalar, P::GaussScalar, P::ShadowGlow});
        auto fx = cmdsWith(b.frame, Shading::Effect);
        CHECK(fx[3]->src == 1);
        CHECK(fx[3]->aux == passOf(b.frame, fx[2])->target);
        checkSnapshot("effect_drop_shadow_spread0", b.frame);
    }
    {
        // blur 0: no Gaussian.
        Built b;
        CommandList cl = makeList();
        buildEffect(b, {dropShadow(6, 6, 0, 2, 1, Color{0, 0, 0, 1}, false)}, cl);
        CHECK(effectSequence(b.frame) == std::vector<P>{P::Dilate, P::Dilate, P::ShadowGlow});
        auto fx = cmdsWith(b.frame, Shading::Effect);
        CHECK(fx[2]->aux == passOf(b.frame, fx[1])->target);
    }
    {
        // spread 0 and blur 0: the hard shadow is AlphaExtract + ShadowGlow.
        Built b;
        CommandList cl = makeList();
        buildEffect(b, {dropShadow(6, 6, 0, 0, 1, Color{0, 0, 0, 1}, false)}, cl);
        CHECK(effectSequence(b.frame) == std::vector<P>{P::AlphaExtract, P::ShadowGlow});
        auto fx = cmdsWith(b.frame, Shading::Effect);
        CHECK(fx[1]->aux == passOf(b.frame, fx[0])->target);
    }
    {
        // inset, spread 0, blur 0: the inverted alpha field feeds ShadowGlow directly.
        Built b;
        CommandList cl = makeList();
        buildEffect(b, {dropShadow(4, 4, 0, 0, 1, Color{0, 0, 0, 1}, true)}, cl);
        CHECK(effectSequence(b.frame) == std::vector<P>{P::AlphaExtract, P::ShadowGlow});
    }
    {
        // Glow radius 0.
        Built b;
        CommandList cl = makeList();
        buildEffect(b, {glow(0, Color{1, 1, 0, 1}, 1, false)}, cl);
        CHECK(effectSequence(b.frame) == std::vector<P>{P::AlphaExtract, P::ShadowGlow});
    }
    {
        // Blur (4, 0): one GaussColor; Blur (0, 0): nothing, the content composites directly.
        Built b;
        CommandList cl = makeList();
        buildEffect(b, {blurFx(4, 0)}, cl);
        CHECK(effectSequence(b.frame) == std::vector<P>{P::GaussColor});
        Built b2;
        CommandList cl2 = makeList();
        buildEffect(b2, {blurFx(0, 0)}, cl2);
        CHECK(cmdsWith(b2.frame, Shading::Effect).empty());
        auto comps = cmdsWith(b2.frame, Shading::Composite);
        REQUIRE(comps.size() == 1);
        CHECK(comps[0]->src == 1);
    }
    {
        // BackdropBlur radius 0: a pyramid of level 0 alone; the Backdrop pass reads the copy
        // resampled 1:1 by LevelMix (T-343), no Kawase, no downsample.
        Built b;
        CommandList cl = makeList();
        buildEffect(b, {backdropBlur(0, 0, 0)}, cl, true);
        CHECK(effectSequence(b.frame) == std::vector<P>{P::LevelMix, P::Backdrop});
        auto fx = cmdsWith(b.frame, Shading::Effect);
        const Pass* copyPass = nullptr;
        for (const Pass& p : b.frame.passes) {
            if (p.copy) copyPass = &p;
        }
        REQUIRE(copyPass != nullptr);
        CHECK(fx[0]->src == copyPass->target);
        CHECK(fx[0]->aux == 0);
    }
}

// =================================================================================================
// 4. Glyphs: atlas pages are R8, uploads go through updateTexture, Glyph cmds sample the page.
// =================================================================================================

#ifndef XT_GPU_NO_TEXT

namespace {

std::string fontPath(const char* name) { return std::string(XT_TEST_FONTS_DIR) + "/" + name; }

const std::vector<std::string_view>& notoFamily() {
    static const std::vector<std::string_view> families = {"Noto Sans"};
    return families;
}

CommandList buildGlyphsList(text::FtTextShaper& shaper, const std::string& textStr, float size, Vec2 origin,
                            const Mat2x3& xf, Color color, int vpW, int vpH) {
    ShapeParams params;
    params.font = FontRequest{std::span<const std::string_view>(notoFamily()), 400, false};
    params.size = size;
    ShapeResult shaped;
    shaper.shape(textStr, params, shaped);
    REQUIRE(!shaped.glyphs.empty());
    REQUIRE(!shaped.runs.empty());

    CommandList list = makeList(vpW, vpH);
    GlyphsCmd cmd{};
    cmd.xf = xf;
    cmd.font = shaped.runs[0].font;
    cmd.size = size;
    cmd.color = color;
    cmd.glyphOffset = static_cast<uint32_t>(list.glyphs.size());
    float penX = origin.x;
    for (const ShapedGlyph& g : shaped.glyphs) {
        list.glyphs.push_back(g.glyphId);
        list.glyphPos.push_back(Vec2{penX + g.xOffset, origin.y + g.yOffset});
        penX += g.xAdvance;
    }
    cmd.glyphCount = static_cast<uint32_t>(list.glyphs.size()) - cmd.glyphOffset;
    list.glyphRuns.push_back(cmd);
    list.commands.push_back(Command{CmdKind::DrawGlyphs, {}, 0});
    return list;
}

}  // namespace

TEST_CASE("gpu: DrawGlyphs uploads R8 atlas pages and emits Glyph commands") {
    text::FtTextShaper shaper;
    REQUIRE(shaper.addFontFile(fontPath("NotoSans-Regular.subset.ttf")));
    text::GlyphAtlas atlas(shaper, 256);
    CommandList cl =
        buildGlyphsList(shaper, "Hello", 16.0f, Vec2{4, 24}, Mat2x3{}, Color{0, 0, 0, 1}, 96, 32);

    Built b;
    b.builder.init(b.device);
    b.builder.resize(96, 32);
    b.builder.setGlyphAtlas(&atlas);
    b.builder.build(cl, &shaper, nullptr, nullptr, b.frame);
    checkFrameWellFormed(b.frame);
    const GpuFrame& f = b.frame;

    // One R8 page created (with no initial data) and at least one region upload into it.
    TextureHandle page = 0;
    for (const auto& [h, t] : b.device.textures) {
        if (t.format == TextureFormat::R8) {
            page = h;
            CHECK(t.w == 256);
            CHECK(t.h == 256);
            CHECK(!t.hadInitial);
        }
    }
    REQUIRE(page != 0);
    REQUIRE(!b.device.updates.empty());
    for (const UpdateRecord& u : b.device.updates) {
        CHECK(u.tex == page);
        CHECK(u.rowPitch == 256);
        CHECK(u.w > 0);
        CHECK(u.h > 0);
    }
    auto glyphs = cmdsWith(f, Shading::Glyph);
    REQUIRE(glyphs.size() == 1);
    CHECK(glyphs[0]->texture == page);
    CHECK(glyphs[0]->stencilOp == StencilOp::TestEqualRef);
    CHECK(glyphs[0]->blend == Blend::SrcOver);
    CHECK(glyphs[0]->vertexCount == 5 * 4);
    CHECK(glyphs[0]->indexCount == 5 * 6);
    GlyphParams gp{};
    std::memcpy(static_cast<void*>(&gp), f.params.data() + glyphs[0]->paramIndex, sizeof(gp));
    CHECK(gp.gamma == doctest::Approx(1.2f));              // dark text
    CHECK(cmdsWith(f, StencilOp::WriteIncrWrap).empty());  // no outline fallback
    // UVs lie inside the page.
    for (uint32_t i = 0; i < glyphs[0]->vertexCount; ++i) {
        const Vertex& v = f.vertices[glyphs[0]->baseVertex + i];
        CHECK(v.u >= 0.0f);
        CHECK(v.u <= 1.0f);
        CHECK(v.v >= 0.0f);
        CHECK(v.v <= 1.0f);
        CHECK(v.rgba == 0xFF000000u);
    }
    checkSnapshot("text_atlas", f);

    // A second identical frame re-uploads nothing.
    size_t uploads = b.device.updates.size();
    b.builder.build(cl, &shaper, nullptr, nullptr, b.frame);
    CHECK(b.device.updates.size() == uploads);
}

TEST_CASE("gpu: rotated DrawGlyphs and a missing atlas fall back to outline fills") {
    text::FtTextShaper shaper;
    REQUIRE(shaper.addFontFile(fontPath("NotoSans-Regular.subset.ttf")));
    text::GlyphAtlas atlas(shaper, 256);
    Mat2x3 rot = mul(translation(Vec2{48, 16}), rotationDeg(20.0f));
    CommandList cl = buildGlyphsList(shaper, "Hi", 16.0f, Vec2{0, 0}, rot, Color{0, 0, 0, 1}, 96, 32);

    Built b;
    b.builder.init(b.device);
    b.builder.resize(96, 32);
    b.builder.setGlyphAtlas(&atlas);
    b.builder.build(cl, &shaper, nullptr, nullptr, b.frame);
    checkFrameWellFormed(b.frame);
    CHECK(cmdsWith(b.frame, Shading::Glyph).empty());
    // Outline fallback: "H" is concave (stencil); "i" is two disjoint convex pieces (direct).
    CHECK(cmdsWith(b.frame, StencilOp::WriteIncrWrap).size() == 1);
    CHECK(cmdsWith(b.frame, Shading::Solid).size() >= 1);
    CHECK(b.device.updates.empty());

    Built b2;
    CommandList plain =
        buildGlyphsList(shaper, "Hi", 16.0f, Vec2{4, 24}, Mat2x3{}, Color{0, 0, 0, 1}, 96, 32);
    buildInto(b2, plain, 96, 32, &shaper);
    CHECK(cmdsWith(b2.frame, Shading::Glyph).empty());
    CHECK(cmdsWith(b2.frame, StencilOp::WriteIncrWrap).size() == 1);
}

TEST_CASE("gpu: an atlas reset mid-frame rebuilds the frame against the repacked atlas") {
    text::FtTextShaper shaper;
    REQUIRE(shaper.addFontFile(fontPath("NotoSans-Regular.subset.ttf")));
    // A tiny atlas: 4 pages of 24x24 hold only a few 20 px glyphs, so a long run resets it.
    text::GlyphAtlas atlas(shaper, 24);
    CommandList cl = buildGlyphsList(shaper, "abcdefghijklmnopqrstuvwxyz", 20.0f, Vec2{0, 20}, Mat2x3{},
                                     Color{0, 0, 0, 1}, 400, 32);
    Built b;
    b.builder.init(b.device);
    b.builder.resize(400, 32);
    b.builder.setGlyphAtlas(&atlas);
    b.builder.build(cl, &shaper, nullptr, nullptr, b.frame);
    checkFrameWellFormed(b.frame);
    // Every emitted quad must reference a glyph that is in the atlas *now*: re-look every glyph
    // up and make sure no reset happens (the frame's glyphs all fit after the rebuild, or the
    // builder fell back to outlines for the whole frame).
    bool sawReset = false;
    for (uint32_t i = 0; i < cl.glyphRuns[0].glyphCount; ++i) {
        text::AtlasEntry e{};
        (void)atlas.lookup(cl.glyphRuns[0].font, cl.glyphs[i], 20.0f, 0.0f, e);
        if (atlas.wasReset()) sawReset = true;
    }
    auto glyphCmds = cmdsWith(b.frame, Shading::Glyph);
    if (!glyphCmds.empty()) {
        CHECK(!sawReset);
    }
    CHECK((!glyphCmds.empty() || !cmdsWith(b.frame, StencilOp::WriteIncrWrap).empty()));
}

// T-270b: ElementRef::setText overwrites the Text node's runtime text slot in place (same StringId,
// new content). Two equal-length values in a row must still reach the GPU frame as new glyphs.
TEST_CASE("gpu: T-270b setText twice with equal-length values uploads and draws the new glyphs") {
    text::FtTextShaper shaper;
    REQUIRE(shaper.addFontFile(fontPath("NotoSans-Regular.subset.ttf")));
    text::GlyphAtlas atlas(shaper, 256);
    Context ctx(ContextConfig{&shaper, nullptr});
    const std::string xt =
        "<xt><head><style>#A { position: absolute; left: 4px; top: 4px; width: 120px; height: 24px;"
        " font-family: \"Noto Sans\"; font-size: 16px; color: #000000; }</style></head>"
        "<body><div id=\"A\">x</div></body></xt>";
    compile::SourceFile files[1] = {compile::SourceFile{"main.xt", xt}};
    compile::CompileResult compiled =
        compile::compileDocumentFromStrings(files, "main.xt", compile::CompileOptions{});
    REQUIRE(compiled.ok());
    Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{compiled.ir.data(), compiled.ir.size()}, false);
    REQUIRE(handle.ok());
    ctx.setViewport(Rect{0, 0, 128, 32}, 1.0f);
    Document* doc = ctx.document(handle.value());
    REQUIRE(doc != nullptr);
    ElementRef a = doc->byId("A");
    REQUIRE(a.valid());

    auto glyphIds = [](const CommandList& list) {
        std::vector<uint32_t> out;
        for (const Command& cmd : list.commands) {
            if (cmd.kind == CmdKind::DrawGlyphs) {
                const GlyphsCmd& run = list.glyphRuns[cmd.index];
                out.insert(out.end(), list.glyphs.begin() + run.glyphOffset,
                           list.glyphs.begin() + run.glyphOffset + run.glyphCount);
            }
        }
        return out;
    };
    auto glyphUvs = [](const GpuFrame& f) {
        std::vector<float> out;
        for (const DrawCmd* cmd : cmdsWith(f, Shading::Glyph)) {
            for (uint32_t i = 0; i < cmd->vertexCount; ++i) {
                out.push_back(f.vertices[cmd->baseVertex + i].u);
                out.push_back(f.vertices[cmd->baseVertex + i].v);
            }
        }
        return out;
    };

    CommandList list;
    DamageList damage;
    Built b;
    b.builder.init(b.device);
    b.builder.resize(128, 32);
    b.builder.setGlyphAtlas(&atlas);

    a.setText("Hello");
    ctx.update(0.016f);
    ctx.render(list, &damage);
    const std::vector<uint32_t> firstGlyphs = glyphIds(list);
    REQUIRE(firstGlyphs.size() == 5);
    b.builder.build(list, &shaper, nullptr, nullptr, b.frame);
    checkFrameWellFormed(b.frame);
    const std::vector<float> firstUvs = glyphUvs(b.frame);
    REQUIRE(!firstUvs.empty());
    const size_t firstUploads = b.device.updates.size();

    a.setText("World");  // Same length; the node's slot is overwritten in place.
    ctx.update(0.016f);
    ctx.render(list, &damage);
    const std::vector<uint32_t> secondGlyphs = glyphIds(list);
    REQUIRE(secondGlyphs.size() == 5);
    CHECK(secondGlyphs != firstGlyphs);
    CHECK(!damage.full);
    CHECK(!damage.rects.empty());
    b.builder.build(list, &shaper, nullptr, nullptr, b.frame);
    checkFrameWellFormed(b.frame);
    const std::vector<float> secondUvs = glyphUvs(b.frame);
    CHECK(!secondUvs.empty());
    CHECK(secondUvs != firstUvs);
    CHECK(b.device.updates.size() > firstUploads);  // 'W', 'r', 'd' were not in the atlas yet.
}

// T-270d (item 2): a font family the host registers only after the document loaded, named by
// setStyle and by a style binding. "世界" is in the SC subset only; Noto Sans has no CJK glyphs and
// addFontFile() puts nothing on the global fallback list, so the run's font tells the two apart.
// Reproduced, not fixed (it was skipped in T-270d): ComputedStyle's fontFamilyOffset/Count can only
// point into the IR's font-family pool (05 ComputedStyle, read by the cascade, layout_text's shaping
// families, external media and the Lottie export), so a family no compiled list names needs a
// runtime representation the design has not decided yet.
// T-270e: the value still does nothing (both elements stay on Noto Sans), but the limit is no longer
// silent: one warning per element and property, written when setStyle / the binding writes it.
TEST_CASE(
    "gpu: T-270e setStyle / a style binding naming an undeclared font family keeps the compiled font and "
    "warns once") {
    text::FtTextShaper shaper;
    REQUIRE(shaper.addFontFile(fontPath("NotoSans-Regular.subset.ttf")));
    Context ctx(ContextConfig{&shaper, nullptr});
    const std::string xt =
        "<xt><head><style>div { position: absolute; left: 4px; width: 120px; height: 24px;"
        " font-family: \"Noto Sans\"; font-size: 16px; color: #000000; } #A { top: 4px; } #B { top: 40px; }"
        "</style></head><body><div id=\"A\">\xE4\xB8\x96\xE7\x95\x8C</div>"
        "<div id=\"B\" port:fam=\"string\" "
        "bind:font-family=\"fam\">\xE4\xB8\x96\xE7\x95\x8C</div></body></xt>";
    compile::SourceFile files[1] = {compile::SourceFile{"main.xt", xt}};
    compile::CompileResult compiled =
        compile::compileDocumentFromStrings(files, "main.xt", compile::CompileOptions{});
    REQUIRE(compiled.ok());
    Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{compiled.ir.data(), compiled.ir.size()}, false);
    REQUIRE(handle.ok());
    ctx.setViewport(Rect{0, 0, 128, 80}, 1.0f);
    Document* doc = ctx.document(handle.value());
    REQUIRE(doc != nullptr);
    ctx.update(0.016f);

    REQUIRE(shaper.addFontFile(fontPath("NotoSansSC-Regular.subset.ttf")));
    const std::vector<std::string_view> scFamily = {"Noto Sans SC"};
    const std::vector<std::string_view> latinFamily = {"Noto Sans"};
    const FontId sc = shaper.match(FontRequest{std::span<const std::string_view>(scFamily), 400, false});
    const FontId latin =
        shaper.match(FontRequest{std::span<const std::string_view>(latinFamily), 400, false});
    REQUIRE(sc != latin);
    const std::string warning =
        "font-family \"Noto Sans SC\" is not declared by the compiled document; setStyle uses only the "
        "first family of a list and can only select families the document names (declare it in a "
        "stylesheet, or use @font-fallback)";
    auto familyWarnings = [&]() {
        size_t n = 0;
        for (const std::string& w : doc->warnings()) {
            n += w.rfind("font-family \"", 0) == 0 ? size_t{1} : size_t{0};
        }
        return n;
    };

    ElementRef a = doc->byId("A");
    REQUIRE(a.valid());
    REQUIRE(a.setStyle("font-family: \"Noto Sans SC\"") == Status::kOk);
    CHECK(std::count(doc->warnings().begin(), doc->warnings().end(), warning) == 1);
    const PortId fam = doc->lookupPort("fam");
    REQUIRE(fam != kInvalidPort);
    REQUIRE(doc->setPortText(fam, "Noto Sans SC") == Status::kOk);
    ctx.update(0.016f);
    CHECK(std::count(doc->warnings().begin(), doc->warnings().end(), warning) == 2);
    CommandList list;
    ctx.render(list);
    for (const char* id : {"A", "B"}) {
        CAPTURE(id);
        CHECK(doc->arena().element(doc->byId(id).node).style.resolvedFont == latin);
    }
    size_t runs = 0;
    for (const Command& cmd : list.commands) {
        if (cmd.kind == CmdKind::DrawGlyphs) {
            ++runs;
            CHECK(list.glyphRuns[cmd.index].font == latin);
        }
    }
    CHECK(runs >= 2);

    // The same element and property again, with the same or another undeclared family: no more.
    REQUIRE(a.setStyle("font-family: \"Noto Sans SC\"") == Status::kOk);
    REQUIRE(a.setStyle("font-family: \"Other Face\"") == Status::kOk);
    REQUIRE(doc->setPortText(fam, "Other Face") == Status::kOk);
    ctx.update(0.016f);
    REQUIRE(doc->setPortText(fam, "Noto Sans SC") == Status::kOk);
    ctx.update(0.016f);
    CHECK(familyWarnings() == 2);
}

namespace {

/// T-263b E3: the frame of `cl` built with a fresh 256 px atlas.
void buildWithAtlas(Built& b, text::FtTextShaper& shaper, text::GlyphAtlas& atlas, const CommandList& cl) {
    b.builder.init(b.device);
    b.builder.resize(static_cast<int>(cl.viewport.w * cl.devicePixelRatio),
                     static_cast<int>(cl.viewport.h * cl.devicePixelRatio));
    b.builder.setGlyphAtlas(&atlas);
    b.builder.build(cl, &shaper, nullptr, nullptr, b.frame);
    checkFrameWellFormed(b.frame);
}

/// `cl` with its one run's scale `s` folded into the font size and the glyph positions: the same
/// picture, drawn by an unscaled run.
CommandList unscaledEquivalent(const CommandList& cl, float s) {
    CommandList out = cl;
    GlyphsCmd& run = out.glyphRuns[0];
    run.xf = Mat2x3{};
    run.size *= s;
    run.effectRadius *= s;
    for (Vec2& p : out.glyphPos) {
        p = Vec2{p.x * s, p.y * s};
    }
    return out;
}

void checkSameGlyphQuads(const GpuFrame& scaled, const GpuFrame& plain) {
    auto a = cmdsWith(scaled, Shading::Glyph);
    auto b = cmdsWith(plain, Shading::Glyph);
    REQUIRE(!b.empty());
    REQUIRE(a.size() == b.size());
    for (size_t c = 0; c < a.size(); ++c) {
        REQUIRE(a[c]->vertexCount == b[c]->vertexCount);
        for (uint32_t i = 0; i < a[c]->vertexCount; ++i) {
            const Vertex& va = scaled.vertices[a[c]->baseVertex + i];
            const Vertex& vb = plain.vertices[b[c]->baseVertex + i];
            CHECK(va.x == vb.x);
            CHECK(va.y == vb.y);
            CHECK(va.u == vb.u);
            CHECK(va.v == vb.v);
        }
    }
}

}  // namespace

TEST_CASE("gpu: T-263b atlas glyphs are rasterized at the run's scaled size") {
    text::FtTextShaper shaper;
    REQUIRE(shaper.addFontFile(fontPath("NotoSans-Regular.subset.ttf")));

    SUBCASE("transform: scale(2) draws what an unscaled run of twice the size draws") {
        CommandList scaled = buildGlyphsList(shaper, "Hello", 12.0f, Vec2{2, 12}, scaling(Vec2{2, 2}),
                                             Color{0, 0, 0, 1}, 128, 48);
        CommandList plain = unscaledEquivalent(scaled, 2.0f);
        text::GlyphAtlas atlasA(shaper, 256);
        text::GlyphAtlas atlasB(shaper, 256);
        Built a;
        Built b;
        buildWithAtlas(a, shaper, atlasA, scaled);
        buildWithAtlas(b, shaper, atlasB, plain);
        CHECK(cmdsWith(a.frame, StencilOp::WriteIncrWrap).empty());  // still the atlas path
        checkSameGlyphQuads(a.frame, b.frame);
    }
    SUBCASE("the scale multiplies with the device pixel ratio") {
        CommandList scaled = buildGlyphsList(shaper, "Hello", 12.0f, Vec2{2, 12}, scaling(Vec2{1.5f, 1.5f}),
                                             Color{0, 0, 0, 1}, 128, 48);
        scaled.devicePixelRatio = 2.0f;
        CommandList plain = unscaledEquivalent(scaled, 1.5f);
        text::GlyphAtlas atlasA(shaper, 256);
        text::GlyphAtlas atlasB(shaper, 256);
        Built a;
        Built b;
        buildWithAtlas(a, shaper, atlasA, scaled);
        buildWithAtlas(b, shaper, atlasB, plain);
        checkSameGlyphQuads(a.frame, b.frame);
    }
    SUBCASE("a glyph effect follows the scale too") {
        CommandList scaled = buildGlyphsList(shaper, "Hi", 12.0f, Vec2{4, 14}, scaling(Vec2{2, 2}),
                                             Color{0, 0, 0, 1}, 128, 48);
        scaled.glyphRuns[0].effect = GlyphEffect::Dilate;
        scaled.glyphRuns[0].effectRadius = 1.0f;
        CommandList plain = unscaledEquivalent(scaled, 2.0f);
        text::GlyphAtlas atlasA(shaper, 256);
        text::GlyphAtlas atlasB(shaper, 256);
        Built a;
        Built b;
        buildWithAtlas(a, shaper, atlasA, scaled);
        buildWithAtlas(b, shaper, atlasB, plain);
        checkSameGlyphQuads(a.frame, b.frame);
    }
    SUBCASE("a mirrored or half-turned run (a = d <= 0) takes the outline fallback, like SW") {
        CommandList flipped =
            buildGlyphsList(shaper, "Hi", 16.0f, Vec2{0, 0},
                            mul(translation(Vec2{48, 8}), scaling(Vec2{-1, -1})), Color{0, 0, 0, 1}, 96, 32);
        text::GlyphAtlas atlas(shaper, 256);
        Built b;
        buildWithAtlas(b, shaper, atlas, flipped);
        CHECK(cmdsWith(b.frame, Shading::Glyph).empty());
        CHECK(cmdsWith(b.frame, StencilOp::WriteIncrWrap).size() == 1);  // the concave "H"
        CHECK(b.device.updates.empty());
        // ...and its glyph effect is not drawn at all (SW draws none for such a run).
        flipped.glyphRuns[0].effect = GlyphEffect::Blur;
        flipped.glyphRuns[0].effectRadius = 2.0f;
        Built e;
        text::GlyphAtlas atlasE(shaper, 256);
        buildWithAtlas(e, shaper, atlasE, flipped);
        CHECK(e.frame.cmds.empty());
    }
}

// T-263c ruling 4: a run whose size changed since the previous build is being scaled; it is filled
// from outlines until its size has held for one build, so an animation does not rasterize every
// 1/4 px size into the atlas.
TEST_CASE("gpu: T-263c a run being scaled stays off the atlas until its size holds for a frame") {
    text::FtTextShaper shaper;
    REQUIRE(shaper.addFontFile(fontPath("NotoSans-Regular.subset.ttf")));
    text::GlyphAtlas atlas(shaper, 512);
    Built b;
    b.builder.init(b.device);
    b.builder.resize(256, 96);
    b.builder.setGlyphAtlas(&atlas);
    auto buildAt = [&](float scale) {
        // The same run (font, glyphs, positions); only its transform's scale moves.
        CommandList cl = buildGlyphsList(shaper, "Hello", 12.0f, Vec2{2, 12}, scaling(Vec2{scale, scale}),
                                         Color{0, 0, 0, 1}, 256, 96);
        b.builder.build(cl, &shaper, nullptr, nullptr, b.frame);
        checkFrameWellFormed(b.frame);
    };
    auto outlined = [&]() {
        return cmdsWith(b.frame, Shading::Glyph).empty() && !cmdsWith(b.frame, Shading::Solid).empty();
    };

    buildAt(1.0f);  // first seen: the atlas, as a static label always was
    CHECK(cmdsWith(b.frame, Shading::Glyph).size() == 1);
    const size_t firstUploads = b.device.updates.size();
    CHECK(firstUploads > 0);
    float scale = 1.0f;
    for (int frame = 1; frame < 30; ++frame) {
        CAPTURE(frame);
        scale = 1.0f + 0.05f * static_cast<float>(frame);
        buildAt(scale);
        CHECK(outlined());
        CHECK(b.device.updates.size() == firstUploads);  // nothing rasterized into the atlas
    }
    // The animation stopped: the next frame has the size of the one before and is back on the atlas.
    buildAt(scale);
    CHECK(cmdsWith(b.frame, Shading::Glyph).size() == 1);
    CHECK(cmdsWith(b.frame, StencilOp::WriteIncrWrap).empty());
    const size_t settledUploads = b.device.updates.size();
    CHECK(settledUploads > firstUploads);
    buildAt(scale);
    CHECK(cmdsWith(b.frame, Shading::Glyph).size() == 1);
    CHECK(b.device.updates.size() == settledUploads);
}

TEST_CASE("gpu: T-263c a run rasterized larger than kMaxAtlasGlyphPx is filled from outlines") {
    text::FtTextShaper shaper;
    REQUIRE(shaper.addFontFile(fontPath("NotoSans-Regular.subset.ttf")));
    SUBCASE("96 px: still the atlas") {
        text::GlyphAtlas atlas(shaper, 1024);
        CommandList cl = buildGlyphsList(shaper, "Hi", 12.0f, Vec2{1, 12}, scaling(Vec2{8, 8}),
                                         Color{0, 0, 0, 1}, 256, 128);
        Built b;
        buildWithAtlas(b, shaper, atlas, cl);
        CHECK(cmdsWith(b.frame, Shading::Glyph).size() == 1);
        CHECK(!b.device.updates.empty());
    }
    SUBCASE("108 px, static: outlines, nothing in the atlas") {
        text::GlyphAtlas atlas(shaper, 1024);
        CommandList cl = buildGlyphsList(shaper, "Hi", 12.0f, Vec2{1, 12}, scaling(Vec2{9, 9}),
                                         Color{0, 0, 0, 1}, 256, 128);
        Built b;
        buildWithAtlas(b, shaper, atlas, cl);
        CHECK(cmdsWith(b.frame, Shading::Glyph).empty());
        CHECK(!cmdsWith(b.frame, Shading::Solid).empty());
        CHECK(b.device.updates.empty());
        b.builder.build(cl, &shaper, nullptr, nullptr, b.frame);  // held for a frame: still outlines
        CHECK(cmdsWith(b.frame, Shading::Glyph).empty());
        CHECK(b.device.updates.empty());
    }
}

// T-263e ruling D: a glyph effect has no form but the atlas coverage, so T-263c exempted effect
// runs from the 96 px limit; the missing bound is what one such run may occupy -- a quarter of an
// atlas page. A run over it is drawn from outlines without its effect, and nothing of it is
// rasterized into the atlas (so it evicts no other glyph).
TEST_CASE("gpu: T-263e a glyph-effect run over a quarter of an atlas page is drawn without its effect") {
    text::FtTextShaper shaper;
    REQUIRE(shaper.addFontFile(fontPath("NotoSans-Regular.subset.ttf")));
    // "Hi" at 12 px scaled by 9: two glyphs of 108 device px, an em square of 11664 px each.
    CommandList effectRun =
        buildGlyphsList(shaper, "Hi", 12.0f, Vec2{1, 12}, scaling(Vec2{9, 9}), Color{0, 0, 0, 1}, 256, 128);
    effectRun.glyphRuns[0].effect = GlyphEffect::Dilate;
    effectRun.glyphRuns[0].effectRadius = 0.25f;
    auto glyphUvs = [](const GpuFrame& f) {
        std::vector<float> out;
        for (const DrawCmd* cmd : cmdsWith(f, Shading::Glyph)) {
            for (uint32_t i = 0; i < cmd->vertexCount; ++i) {
                out.push_back(f.vertices[cmd->baseVertex + i].u);
                out.push_back(f.vertices[cmd->baseVertex + i].v);
            }
        }
        return out;
    };

    SUBCASE("under the bound: on the atlas with its effect, above 96 px too") {
        text::GlyphAtlas atlas(shaper, 1024);  // a quarter page is 262144 px
        Built b;
        buildWithAtlas(b, shaper, atlas, effectRun);
        CHECK(cmdsWith(b.frame, Shading::Glyph).size() == 2);  // one quad per glyph, never batched
        CHECK(!b.device.updates.empty());
        CHECK(std::string(b.builder.lastError()).empty());
    }
    SUBCASE("over the bound: outlines without the effect, nothing rasterized, nothing evicted") {
        text::GlyphAtlas atlas(shaper, 256);  // a quarter page is 16384 px < 2 * 11664
        const CommandList label =
            buildGlyphsList(shaper, "Hello", 12.0f, Vec2{2, 12}, Mat2x3{}, Color{0, 0, 0, 1}, 256, 128);
        Built b;
        buildWithAtlas(b, shaper, atlas, label);  // a small label already lives in the atlas
        REQUIRE(cmdsWith(b.frame, Shading::Glyph).size() == 1);
        const size_t labelUploads = b.device.updates.size();
        REQUIRE(labelUploads > 0);
        const std::vector<float> labelUvs = glyphUvs(b.frame);

        for (int frame = 0; frame < 2; ++frame) {
            CAPTURE(frame);
            b.builder.build(effectRun, &shaper, nullptr, nullptr, b.frame);
            checkFrameWellFormed(b.frame);
            CHECK(cmdsWith(b.frame, Shading::Glyph).empty());
            CHECK(!cmdsWith(b.frame, Shading::Solid).empty());  // the glyph outlines, filled
            CHECK(b.device.updates.size() == labelUploads);     // nothing went into the atlas
            CHECK(std::string(b.builder.lastError()) ==
                  "glyph effect run too large for the atlas; drawn without its effect");
        }
        // The label is where it was: not evicted, nothing uploaded again, nothing to report.
        b.builder.build(label, &shaper, nullptr, nullptr, b.frame);
        CHECK(cmdsWith(b.frame, Shading::Glyph).size() == 1);
        CHECK(glyphUvs(b.frame) == labelUvs);
        CHECK(b.device.updates.size() == labelUploads);
        CHECK(std::string(b.builder.lastError()).empty());
    }
    SUBCASE("a run of one repeated glyph counts it once") {
        // 40 times the same glyph at the same subpixel phase is one atlas entry: 24 px, 576 px.
        CommandList repeated =
            buildGlyphsList(shaper, "H", 24.0f, Vec2{2, 24}, Mat2x3{}, Color{0, 0, 0, 1}, 256, 128);
        REQUIRE(repeated.glyphs.size() == 1);
        for (int i = 1; i < 40; ++i) {
            repeated.glyphs.push_back(repeated.glyphs[0]);
            repeated.glyphPos.push_back(
                Vec2{repeated.glyphPos[0].x + 4.0f * static_cast<float>(i), repeated.glyphPos[0].y});
        }
        repeated.glyphRuns[0].glyphCount = 40;
        repeated.glyphRuns[0].effect = GlyphEffect::Blur;
        repeated.glyphRuns[0].effectRadius = 1.0f;
        text::GlyphAtlas atlas(shaper, 128);  // a quarter page is 4096 px < 40 * 576
        Built b;
        buildWithAtlas(b, shaper, atlas, repeated);
        CHECK(cmdsWith(b.frame, Shading::Glyph).size() == 40);
        CHECK(std::string(b.builder.lastError()).empty());
    }
}

#endif  // XT_GPU_NO_TEXT

// =================================================================================================
// Images.
// =================================================================================================

namespace {

class TestAssetResolver : public AssetResolver {
public:
    AssetId add(ImageData data) {
        AssetId id = static_cast<AssetId>(m_images.size());
        m_images.push_back(std::move(data));
        return id;
    }
    void replace(AssetId id, ImageData data) { m_images[id] = std::move(data); }
    bool readBytes(std::string_view, std::vector<uint8_t>&) override { return false; }
    bool decodeImage(AssetId asset, const IrView&, ImageData& out) override {
        if (asset >= m_images.size()) return false;
        out = m_images[asset];
        return true;
    }
    Vec2 imageSize(AssetId asset, const IrView& view) override {
        ImageData data;
        if (!decodeImage(asset, view, data)) return Vec2{0, 0};
        return Vec2{static_cast<float>(data.width), static_cast<float>(data.height)};
    }

private:
    std::vector<ImageData> m_images;
};

ImageData makeChecker4x4() {
    ImageData img;
    img.width = 4;
    img.height = 4;
    img.rgba.resize(4 * 4 * 4);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            uint8_t v = ((x + y) % 2 == 0) ? 255 : 0;
            size_t i = (static_cast<size_t>(y) * 4 + static_cast<size_t>(x)) * 4;
            img.rgba[i + 0] = v;
            img.rgba[i + 1] = v;
            img.rgba[i + 2] = v;
            img.rgba[i + 3] = 128;
        }
    }
    return img;
}

void addDrawImage(CommandList& cl, AssetId asset, Rect dst, float opacity, bool smooth) {
    ImageCmd c{};
    c.xf = Mat2x3{};
    c.asset = asset;
    c.dst = dst;
    c.srcUv = Rect{0, 0, 1, 1};
    c.opacity = opacity;
    c.smooth = smooth ? 1 : 0;
    cl.images.push_back(c);
    cl.commands.push_back(Command{CmdKind::DrawImage, {}, static_cast<uint32_t>(cl.images.size() - 1)});
}

std::vector<uint8_t> emptyIr() {
    compile::IrWriter writer;
    compile::SceneBuilder scene;
    scene.width = 1;
    scene.height = 1;
    writer.addScene(scene);
    return writer.finish();
}

}  // namespace

TEST_CASE("gpu: DrawImage creates one premultiplied texture per asset and invalidateAsset drops it") {
    std::vector<uint8_t> bytes = emptyIr();
    Result<IrView> viewResult = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(viewResult.ok());
    TestAssetResolver assets;
    AssetId id = assets.add(makeChecker4x4());

    CommandList cl = makeList();
    addDrawImage(cl, id, Rect{0, 0, 32, 32}, 1.0f, true);
    addDrawImage(cl, id, Rect{32, 0, 32, 32}, 0.5f, false);
    Built b;
    buildInto(b, cl, 64, 64, nullptr, &assets, &viewResult.value());
    const GpuFrame& f = b.frame;
    auto imgs = cmdsWith(f, Shading::Texture);
    REQUIRE(imgs.size() == 2);
    CHECK(imgs[0]->texture == imgs[1]->texture);
    CHECK(imgs[0]->sampler == Sampler::LinearClamp);
    CHECK(imgs[1]->sampler == Sampler::PointClamp);
    CHECK(imgs[0]->stencilOp == StencilOp::TestEqualRef);
    CHECK(b.device.aliveCount() == 1);
    CHECK(b.device.textures[imgs[0]->texture].w == 4);
    CHECK(b.device.textures[imgs[0]->texture].hadInitial);
    CHECK(f.vertices[imgs[0]->baseVertex].rgba == 0xFFFFFFFFu);
    CHECK(f.vertices[imgs[1]->baseVertex].rgba == 0x80808080u);
    CHECK(f.vertices[imgs[1]->baseVertex + 2].x == 64.0f);
    CHECK(f.vertices[imgs[1]->baseVertex + 2].u == 1.0f);
    checkSnapshot("image_quad", f);

    b.builder.invalidateAsset(id);
    CHECK(b.device.aliveCount() == 0);
    b.builder.build(cl, nullptr, &assets, &viewResult.value(), b.frame);
    CHECK(b.device.aliveCount() == 1);
    b.builder.shutdown();
    CHECK(b.device.aliveCount() == 0);
}

// =================================================================================================
// 5. Projection: homogeneous image quads have w != 1; paths are CPU-projected.
// =================================================================================================

TEST_CASE("gpu: a projected DrawImage carries w != 1 and a projected fill stays w == 1") {
    std::vector<uint8_t> bytes = emptyIr();
    Result<IrView> viewResult = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(viewResult.ok());
    TestAssetResolver assets;
    AssetId id = assets.add(makeChecker4x4());

    CommandList cl = makeList();
    Mat4 viewProj = cssPerspectiveViewProj(150.0f, Vec2{32, 32}).viewProj;
    addPushProjection(cl, viewProj);
    addPushTransform3D(cl, mat4RotateY(60.0f));
    addDrawImage(cl, id, Rect{8, 8, 48, 48}, 1.0f, true);
    addFillRect(cl, 16, 16, 32, 32, Color{1, 0, 0, 1});
    cl.commands.push_back(Command{CmdKind::PopTransform3D, {}, 0});
    cl.commands.push_back(Command{CmdKind::PopProjection, {}, 0});
    addFillRect(cl, 0, 0, 8, 8, Color{0, 1, 0, 1});
    Built b;
    buildInto(b, cl, 64, 64, nullptr, &assets, &viewResult.value());
    const GpuFrame& f = b.frame;
    auto imgs = cmdsWith(f, Shading::Texture);
    REQUIRE(imgs.size() == 1);
    bool sawNonUnitW = false;
    for (uint32_t i = 0; i < 4; ++i) {
        const Vertex& v = f.vertices[imgs[0]->baseVertex + i];
        if (v.w != 1.0f) sawNonUnitW = true;
        CHECK(v.w > 0.0f);
    }
    CHECK(sawNonUnitW);
    // Both fills are convex (direct) and share one state, so they batch into one command whose
    // first 8 vertices (fan + fringe) belong to the projected rect.
    auto fills = cmdsWith(f, Shading::Solid);
    REQUIRE(fills.size() == 1);
    CHECK(fills[0]->vertexCount == 16);
    for (uint32_t i = 0; i < fills[0]->vertexCount; ++i) {
        CHECK(f.vertices[fills[0]->baseVertex + i].w == 1.0f);
    }
    // The projected rect is no longer axis-aligned: its top edge's two corners differ in y.
    const Vertex* pv = &f.vertices[fills[0]->baseVertex];
    CHECK(pv[0].y != doctest::Approx(pv[1].y));
    checkSnapshot("projection_image_fill", f);
}

// T-160 (bench BUG-5): a gradient on a 3D layer carries plane-local u,v. The layer is moved 30 px
// right by its world3 and the fill spans plane x 0..20: its vertices sit at screen x ~30..50 but
// their u must stay within 0..20 (before the fix u was the screen x, clamping the whole rect to
// the last stop).
TEST_CASE("gpu: a projected gradient fill keeps plane-local u,v") {
    CommandList cl = makeList();
    Mat4 viewProj = cssPerspectiveViewProj(200.0f, Vec2{32, 32}).viewProj;
    addPushProjection(cl, viewProj);
    addPushTransform3D(cl, mat4Translate(Vec3{30, 0, 0}));
    addFillGradient(cl, 0, 20, 20, 20);
    cl.commands.push_back(Command{CmdKind::PopTransform3D, {}, 0});
    cl.commands.push_back(Command{CmdKind::PopProjection, {}, 0});
    Built b;
    buildInto(b, cl, 64, 64);
    auto grads = cmdsWith(b.frame, Shading::Gradient);
    REQUIRE(grads.size() == 1);
    REQUIRE(grads[0]->vertexCount > 0);
    for (uint32_t i = 0; i < grads[0]->vertexCount; ++i) {
        const Vertex& v = b.frame.vertices[grads[0]->baseVertex + i];
        const float sx = v.x / v.w;
        CHECK(sx > 28.0f);
        CHECK(v.u == doctest::Approx(sx - 30.0f).epsilon(0.01));
        CHECK(v.u > -2.0f);
        CHECK(v.u < 22.0f);
    }
}

// T-160: under a real perspective the gradient vertices are homogeneous (w != 1), so the GPU
// interpolates u,v perspective-correctly, and every vertex's u,v maps back into the plane rect.
TEST_CASE("gpu: a perspective-projected gradient fill has homogeneous vertices") {
    CommandList cl = makeList();
    Mat4 viewProj = cssPerspectiveViewProj(150.0f, Vec2{32, 32}).viewProj;
    addPushProjection(cl, viewProj);
    addPushTransform3D(cl, mat4RotateY(40.0f));
    addFillGradient(cl, 16, 16, 32, 32);
    cl.commands.push_back(Command{CmdKind::PopTransform3D, {}, 0});
    cl.commands.push_back(Command{CmdKind::PopProjection, {}, 0});
    Built b;
    buildInto(b, cl, 64, 64);
    auto grads = cmdsWith(b.frame, Shading::Gradient);
    REQUIRE(grads.size() == 1);
    bool sawNonUnitW = false;
    for (uint32_t i = 0; i < grads[0]->vertexCount; ++i) {
        const Vertex& v = b.frame.vertices[grads[0]->baseVertex + i];
        CHECK(v.w > 0.0f);
        if (std::fabs(v.w - 1.0f) > 1e-3f) sawNonUnitW = true;
        CHECK(v.u > 14.0f);
        CHECK(v.u < 50.0f);
    }
    CHECK(sawNonUnitW);
}

TEST_CASE("gpu: a projected PushClipRect becomes a stencil clip") {
    CommandList cl = makeList();
    Mat4 viewProj = cssPerspectiveViewProj(200.0f, Vec2{32, 32}).viewProj;
    addPushProjection(cl, viewProj);
    addPushTransform3D(cl, mat4RotateY(45.0f));
    addPushClipRect(cl, Rect{8, 8, 48, 48});
    addFillRect(cl, 0, 0, 64, 64, Color{1, 0, 0, 1});
    addPopClip(cl);
    cl.commands.push_back(Command{CmdKind::PopTransform3D, {}, 0});
    cl.commands.push_back(Command{CmdKind::PopProjection, {}, 0});
    Built b;
    buildInto(b, cl, 64, 64);
    CHECK(cmdsWith(b.frame, StencilOp::ClipPush).size() == 1);
    CHECK(cmdsWith(b.frame, StencilOp::ClipPop).size() == 1);
}

TEST_CASE("gpu: T-155 a projected but untilted PushClipRect stays a scissor level") {
    CommandList cl = makeList();
    Mat4 viewProj = cssPerspectiveViewProj(200.0f, Vec2{32, 32}).viewProj;
    addPushProjection(cl, viewProj);
    addPushClipRect(cl, Rect{8, 8, 48, 48});
    addFillRect(cl, 0, 0, 64, 64, Color{1, 0, 0, 1});
    addPopClip(cl);
    cl.commands.push_back(Command{CmdKind::PopProjection, {}, 0});
    Built b;
    buildInto(b, cl, 64, 64);
    CHECK(cmdsWith(b.frame, StencilOp::ClipPush).empty());
    CHECK(cmdsWith(b.frame, StencilOp::ClipPop).empty());
    REQUIRE(b.frame.cmds.size() == 1);
    CHECK(b.frame.cmds[0].clipX == 8);
    CHECK(b.frame.cmds[0].clipW == 48);
}

// =================================================================================================
// PerfTracker and reuse.
// =================================================================================================

TEST_CASE("gpu: build reports gpuBuildMs and reuses the frame's storage") {
    CommandList cl = makeList();
    addFillRect(cl, 8, 8, 32, 24, Color{1, 0, 0, 1});
    PerfTracker perf;
    perf.setEnabled(true);
    perf.beginFrame(0.0f);
    Built b;
    b.builder.init(b.device);
    b.builder.resize(64, 64);
    b.builder.build(cl, nullptr, nullptr, nullptr, b.frame, &perf);
    CHECK(perf.current().gpuBuildMs >= 0.0f);
    perf.endFrame();
    const Vertex* before = b.frame.vertices.data();
    size_t cap = b.frame.vertices.capacity();
    b.builder.build(cl, nullptr, nullptr, nullptr, b.frame, &perf);
    CHECK(b.frame.vertices.data() == before);
    CHECK(b.frame.vertices.capacity() == cap);
    CHECK(b.frame.cmds.size() == 1);
}

// =================================================================================================
// 6. Structural snapshots of the golden scene fixtures and UI documents.
// =================================================================================================

TEST_CASE("gpu: golden scene fixtures build well-formed frames (snapshots)") {
    int count = 0;
    for (const golden::GoldenFixture& fx : golden::fixtures()) {
        INFO("fixture: ", std::string(fx.name));
        CommandList cmds = paintFixture(fx);
        Built b;
        buildInto(b, cmds, fx.width, fx.height);
        CHECK(!b.frame.cmds.empty());
        checkSnapshot(fx.name, b.frame);
        ++count;
    }
    CHECK(count >= 10);
}

TEST_CASE("gpu: UI documents build well-formed frames (snapshots)") {
    struct Doc {
        const char* file;
        const char* snapshot;
        int w, h;
        float time;
    };
    // doc_basic-style documents from Tests/golden/ui: plain boxes, an overflow clip, a group
    // opacity layer, an effect, a 3D flip.
    const Doc docs[] = {
        {"box_bg.xt", "ui_box_bg", 256, 256, 0.0f},
        {"overflow_hidden.xt", "ui_overflow_hidden", 256, 256, 0.0f},
        {"opacity_group.xt", "ui_opacity_group", 256, 256, 0.0f},
        {"ui_fx_blur.xt", "ui_fx_blur", 256, 256, 0.0f},
        {"ui_fx_stack.xt", "ui_fx_stack", 256, 256, 0.0f},
        {"ui_3d_card_flip.xt", "ui_3d_card_flip", 256, 256, 0.0f},
    };
    for (const Doc& d : docs) {
        INFO("document: ", std::string(d.file));
        UiDoc ui;
        REQUIRE(loadUiFixture(ui, d.file, d.w, d.h, d.time));
        Built b;
        buildInto(b, ui.cmds, d.w, d.h, &ui.shaper, ui.resolver.get(), &ui.doc->ir());
        CHECK(!b.frame.cmds.empty());
        checkSnapshot(d.snapshot, b.frame);
    }
}

// =================================================================================================
// 8. External textures (Docs/tasks/T-150.md): `<img src="engine://scene">` samples the host's
// texture bound on the resolver, sized by the binding; unbound it draws nothing.
// =================================================================================================

namespace {

/// Compiles an in-memory `.xt`, loads it into a Context over `assets` and renders one frame.
bool loadUiString(UiDoc& out, AssetResolver& assets, std::string_view xt, int w, int h) {
    compile::SourceFile file{"doc.xt", xt};
    compile::CompileOptions options;
    options.strict = true;
    compile::CompileResult result = compile::compileDocumentFromStrings(
        std::span<const compile::SourceFile>(&file, 1), "doc.xt", options);
    if (!result.ok()) {
        return false;
    }
    out.ctx = std::make_unique<Context>(ContextConfig{&out.shaper, &assets});
    out.ctx->setViewport(Rect{0, 0, static_cast<float>(w), static_cast<float>(h)}, 1.0f);
    Result<DocumentHandle> handle = out.ctx->loadDocument(IrBlob{result.ir.data(), result.ir.size()}, true);
    if (!handle.ok()) {
        return false;
    }
    out.doc = out.ctx->document(handle.value());
    if (out.doc == nullptr) {
        return false;
    }
    out.ctx->update(0.0f);
    out.ctx->render(out.cmds);
    return true;
}

/// Width/height of a quad command's vertex bounding box.
Vec2 quadSize(const GpuFrame& f, const DrawCmd& c) {
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    for (uint32_t i = 0; i < c.vertexCount; ++i) {
        const Vertex& v = f.vertices[c.baseVertex + i];
        minX = std::min(minX, v.x);
        maxX = std::max(maxX, v.x);
        minY = std::min(minY, v.y);
        maxY = std::max(maxY, v.y);
    }
    return Vec2{maxX - minX, maxY - minY};
}

}  // namespace

TEST_CASE("gpu: an engine:// img samples the host texture bound on the resolver, sized by the binding") {
    constexpr std::string_view kDoc = R"(<xt>
  <body style="padding: 0">
    <img src="engine://scene"/>
    <img src="engine://scene" style="width: 64px; height: 32px"/>
  </body>
</xt>)";

    TestAssetResolver assets;
    assets.setExternalTexture("scene", 42, 256, 128);
    UiDoc ui;
    REQUIRE(loadUiString(ui, assets, kDoc, 512, 512));

    Built b;
    buildInto(b, ui.cmds, 512, 512, &ui.shaper, &assets, &ui.doc->ir());
    // The two quads share one texture and state, so T-155 batches them into one command.
    auto imgs = cmdsWith(b.frame, Shading::Texture);
    REQUIRE(imgs.size() == 1);
    CHECK(imgs[0]->texture == 42);
    REQUIRE(imgs[0]->vertexCount == 8);
    DrawCmd first = *imgs[0], second = *imgs[0];
    first.vertexCount = 4;
    second.baseVertex += 4;
    second.vertexCount = 4;
    // No width/height in the document: the binding's size is the intrinsic size.
    CHECK(quadSize(b.frame, first).x == 256.0f);
    CHECK(quadSize(b.frame, first).y == 128.0f);
    // Explicit size wins, object-fit fill (the default) stretches the texture over it.
    CHECK(quadSize(b.frame, second).x == 64.0f);
    CHECK(quadSize(b.frame, second).y == 32.0f);
    CHECK(b.frame.vertices[imgs[0]->baseVertex + 2].u == 1.0f);
    // Nothing was decoded or uploaded: the host owns the texture.
    CHECK(b.device.aliveCount() == 0);
    CHECK(std::string(b.builder.lastError()).empty());
    CHECK(cmdsWith(b.frame, Shading::TextureStraight).empty());

    // A straight-alpha binding switches the shading; the layout is unchanged.
    assets.setExternalTexture("scene", 43, 256, 128, /*straightAlpha=*/true);
    b.builder.build(ui.cmds, &ui.shaper, &assets, &ui.doc->ir(), b.frame);
    CHECK(cmdsWith(b.frame, Shading::Texture).empty());
    auto straight = cmdsWith(b.frame, Shading::TextureStraight);
    REQUIRE(straight.size() == 1);
    CHECK(straight[0]->vertexCount == 8);
    CHECK(straight[0]->texture == 43);
    CHECK(straight[0]->sampler == Sampler::LinearClamp);
    CHECK(straight[0]->stencilOp == StencilOp::TestEqualRef);
    checkSnapshot("image_external_straight", b.frame);

    // Unbound (or bound as size only): no textured command, no error, nothing created.
    assets.clearExternalTexture("scene");
    CHECK(assets.externalTexture("scene") == nullptr);
    b.builder.build(ui.cmds, &ui.shaper, &assets, &ui.doc->ir(), b.frame);
    CHECK(cmdsWith(b.frame, Shading::Texture).empty());
    CHECK(cmdsWith(b.frame, Shading::TextureStraight).empty());
    CHECK(std::string(b.builder.lastError()).empty());
    CHECK(b.device.aliveCount() == 0);
    b.builder.shutdown();
}

TEST_CASE("gpu: an engine:// img measures 0 x 0 while unbound and never touches the resolver's files") {
    constexpr std::string_view kDoc = R"(<xt>
  <body style="padding: 0">
    <img src="engine://scene"/>
  </body>
</xt>)";
    // A FileAssetResolver would look for `engine://scene` under its root; it must not (and would
    // not find it anyway): the image simply has no intrinsic size.
    FileAssetResolver files(std::string(XT_GPU_UI_DIR), sw::decodeImageBytes);
    UiDoc ui;
    REQUIRE(loadUiString(ui, files, kDoc, 512, 512));
    Built b;
    buildInto(b, ui.cmds, 512, 512, &ui.shaper, &files, &ui.doc->ir());
    CHECK(cmdsWith(b.frame, Shading::Texture).empty());
    CHECK(b.device.aliveCount() == 0);
    CHECK(files.imageSize(0, ui.doc->ir()).x == 0.0f);
    CHECK(files.imageView(0, ui.doc->ir()) == nullptr);

    // Binding after the fact takes effect at the next layout pass (a relayout is the host's job;
    // here the viewport change forces one).
    files.setExternalTexture("scene", 7, 100, 50);
    ui.ctx->setViewport(Rect{0, 0, 400, 400}, 1.0f);
    ui.ctx->update(0.0f);
    ui.cmds = CommandList{};
    ui.ctx->render(ui.cmds);
    b.builder.build(ui.cmds, &ui.shaper, &files, &ui.doc->ir(), b.frame);
    auto imgs = cmdsWith(b.frame, Shading::Texture);
    REQUIRE(imgs.size() == 1);
    CHECK(imgs[0]->texture == 7);
    CHECK(quadSize(b.frame, *imgs[0]).x == 100.0f);
    CHECK(quadSize(b.frame, *imgs[0]).y == 50.0f);
    b.builder.shutdown();
}

// -------------------------------------------------------------------------------------------
// 7. T-162: effect result cache and static frame reuse.
// -------------------------------------------------------------------------------------------

namespace {

/// A build on an already initialised builder (buildInto re-inits, which empties the cache).
void rebuild(Built& b, const CommandList& cl) {
    b.builder.build(cl, nullptr, nullptr, nullptr, b.frame);
    checkFrameWellFormed(b.frame);
}

CommandList blurredRectList(float radius = 4.0f, Rect hint = Rect{0, 0, 64, 64}) {
    CommandList cl = makeList();
    addPushEffect(cl, {blurFx(radius, radius)}, Mat2x3{}, hint, Rect{16, 16, 32, 32});
    addFillRect(cl, 16, 16, 32, 32, Color{1, 0, 0, 1});
    addPopEffect(cl);
    return cl;
}

}  // namespace

TEST_CASE("gpu: T-162 an effect segment is stored on its second identical build and served on the third") {
    Built b;
    CommandList cl = blurredRectList();
    b.builder.init(b.device);
    b.builder.resize(64, 64);

    // Build 1: first appearance, drawn in full, nothing stored (an animating segment never pays
    // for a copy). Not reusable: a later build will be cheaper. (init() probed the device with
    // one 1 x 1 target, released at once.)
    CHECK(b.device.cacheAcquires == 1);
    CHECK(b.device.cached.empty());
    rebuild(b, cl);
    CHECK(cmdsWith(b.frame, Shading::Effect).size() == 2);
    CHECK(b.frame.cacheStores.empty());
    CHECK(b.device.cacheAcquires == 1);
    CHECK_FALSE(b.builder.lastFrameReusable(nullptr, nullptr));

    // Build 2: seen before -> drawn in full and copied into a cached target after the pass that
    // wrote the final result.
    rebuild(b, cl);
    CHECK(cmdsWith(b.frame, Shading::Effect).size() == 2);
    REQUIRE(b.frame.cacheStores.size() == 1);
    const CacheStore store = b.frame.cacheStores[0];
    REQUIRE(store.pass < b.frame.passes.size());
    CHECK(b.frame.passes[store.pass].target == store.src);
    REQUIRE(store.src >= 1);
    CHECK(b.frame.transients[store.src - 1].width == 64);
    CHECK(b.frame.transients[store.src - 1].height == 64);
    CHECK(b.frame.transients[store.src - 1].needStencil == 0);
    REQUIRE(b.device.cached.count(store.texture) == 1);
    CHECK(b.device.cached[store.texture].first == 64);
    CHECK(b.device.cached[store.texture].second == 64);
    // The composite still reads the transient, after the store.
    auto comps = cmdsWith(b.frame, Shading::Composite);
    REQUIRE(comps.size() == 1);
    CHECK(comps[0]->src == store.src);
    CHECK_FALSE(b.builder.lastFrameReusable(nullptr, nullptr));
    checkSnapshot("effect_cache_store", b.frame);

    // Build 3: served from the cache: one textured quad over the window, no transient, no
    // effect pass. Now the frame is the cheapest one for this list: reusable.
    rebuild(b, cl);
    CHECK(cmdsWith(b.frame, Shading::Effect).empty());
    CHECK(cmdsWith(b.frame, Shading::Composite).empty());
    CHECK(b.frame.transients.empty());
    CHECK(b.frame.cacheStores.empty());
    REQUIRE(b.frame.passes.size() == 1);
    CHECK(b.frame.passes[0].target == kSurfaceTarget);
    auto quads = cmdsWith(b.frame, Shading::Texture);
    REQUIRE(quads.size() == 1);
    CHECK(quads[0]->texture == store.texture);
    CHECK(quads[0]->blend == Blend::SrcOver);
    CHECK(quads[0]->stencilOp == StencilOp::None);
    CHECK(quads[0]->vertexCount == 4);
    CHECK(b.frame.vertices[quads[0]->baseVertex].x == 0.0f);
    CHECK(b.frame.vertices[quads[0]->baseVertex].u == 0.0f);
    CHECK(b.device.cacheReleases == 1);  // the init probe only
    CHECK(b.builder.lastFrameReusable(nullptr, nullptr));
    checkSnapshot("effect_cache_hit", b.frame);

    // Build 4 with a different radius: a miss, and the stale entry goes at the end of the build.
    CommandList other = blurredRectList(8.0f);
    rebuild(b, other);
    CHECK(cmdsWith(b.frame, Shading::Effect).size() == 2);
    CHECK(b.frame.cacheStores.empty());
    CHECK(b.device.cacheReleases == 2);  // the probe and the stale entry
    CHECK(b.device.cached.empty());
    CHECK_FALSE(b.builder.lastFrameReusable(nullptr, nullptr));
    b.builder.shutdown();
}

TEST_CASE("gpu: T-162 the cache key covers dpr, surface size, scissor and clip state") {
    Built b;
    CommandList cl = blurredRectList();
    b.builder.init(b.device);
    b.builder.resize(64, 64);
    rebuild(b, cl);
    rebuild(b, cl);
    rebuild(b, cl);
    REQUIRE(cmdsWith(b.frame, Shading::Effect).empty());
    const int acquires = b.device.cacheAcquires;

    SUBCASE("a different dpr misses") {
        CommandList dpr = cl;
        dpr.devicePixelRatio = 2.0f;
        rebuild(b, dpr);
        CHECK_FALSE(cmdsWith(b.frame, Shading::Effect).empty());
        CHECK(b.frame.cacheStores.empty());
    }
    SUBCASE("a scissor around the segment misses") {
        CommandList clipped = makeList();
        addPushClipRect(clipped, Rect{8, 8, 40, 40});
        addPushEffect(clipped, {blurFx(4, 4)}, Mat2x3{}, Rect{0, 0, 64, 64}, Rect{16, 16, 32, 32});
        addFillRect(clipped, 16, 16, 32, 32, Color{1, 0, 0, 1});
        addPopEffect(clipped);
        addPopClip(clipped);
        rebuild(b, clipped);
        CHECK_FALSE(cmdsWith(b.frame, Shading::Effect).empty());
        CHECK(b.frame.cacheStores.empty());
        // ...and, repeated, it gets its own entry.
        rebuild(b, clipped);
        CHECK(b.frame.cacheStores.size() == 1);
        rebuild(b, clipped);
        CHECK(cmdsWith(b.frame, Shading::Effect).empty());
        CHECK(b.device.cacheAcquires == acquires + 1);
    }
    SUBCASE("a resize empties the cache and invalidates the frame") {
        b.builder.resize(128, 128);
        CHECK(b.device.cached.empty());
        CHECK_FALSE(b.builder.lastFrameReusable(nullptr, nullptr));
        rebuild(b, cl);
        CHECK_FALSE(cmdsWith(b.frame, Shading::Effect).empty());
        CHECK(b.frame.cacheStores.empty());
    }
    b.builder.shutdown();
}

TEST_CASE("gpu: T-162 BackdropBlur-first and identity stacks are never cached and do not block reuse") {
    SUBCASE("BackdropBlur reads the parent") {
        Built b;
        CommandList cl = makeList();
        addFillRect(cl, 0, 0, 64, 64, Color{0, 0, 1, 1});
        addPushEffect(cl, {backdropBlur(8, 0.5f, 0.0f)}, Mat2x3{}, Rect{0, 0, 64, 64}, Rect{16, 16, 32, 32});
        addFillRect(cl, 16, 16, 32, 32, Color{1, 0, 0, 1});
        addPopEffect(cl);
        b.builder.init(b.device);
        b.builder.resize(64, 64);
        for (int i = 0; i < 3; ++i) {
            rebuild(b, cl);
            CHECK_FALSE(cmdsWith(b.frame, Shading::Effect).empty());
            CHECK(b.frame.cacheStores.empty());
            CHECK(b.builder.lastFrameReusable(nullptr, nullptr));
        }
        CHECK(b.device.cacheAcquires == 1);  // the init probe only
        b.builder.shutdown();
    }
    SUBCASE("an identity stack has nothing to keep") {
        Built b;
        CommandList cl = blurredRectList(0.0f);  // sigma 0: no pass at all
        b.builder.init(b.device);
        b.builder.resize(64, 64);
        for (int i = 0; i < 3; ++i) {
            rebuild(b, cl);
            CHECK(cmdsWith(b.frame, Shading::Effect).empty());
            CHECK(cmdsWith(b.frame, Shading::Composite).size() == 1);
            CHECK(b.frame.cacheStores.empty());
            CHECK(b.builder.lastFrameReusable(nullptr, nullptr));
        }
        CHECK(b.device.cacheAcquires == 1);  // the init probe only
        b.builder.shutdown();
    }
}

TEST_CASE("gpu: T-162 a device without cached targets draws plain frames that are reusable at once") {
    Built b;
    b.device.supportsCache = false;
    CommandList cl = blurredRectList();
    b.builder.init(b.device);
    b.builder.resize(64, 64);
    for (int i = 0; i < 3; ++i) {
        rebuild(b, cl);
        CHECK(cmdsWith(b.frame, Shading::Effect).size() == 2);
        CHECK(b.frame.cacheStores.empty());
        CHECK(b.builder.lastFrameReusable(nullptr, nullptr));
    }
    b.builder.shutdown();
}

TEST_CASE("gpu: T-162 lastFrameReusable is withdrawn by every setting that changes the frame") {
    Built b;
    CommandList cl = blurredRectList();
    b.builder.init(b.device);
    b.builder.resize(64, 64);
    CHECK_FALSE(b.builder.lastFrameReusable(nullptr, nullptr));  // nothing built yet
    rebuild(b, cl);
    rebuild(b, cl);
    rebuild(b, cl);
    REQUIRE(b.builder.lastFrameReusable(nullptr, nullptr));

    b.builder.setEdgeFringe(false);
    CHECK_FALSE(b.builder.lastFrameReusable(nullptr, nullptr));
    rebuild(b, cl);  // the fringe setting is part of the key: a miss, entry released
    CHECK_FALSE(cmdsWith(b.frame, Shading::Effect).empty());
    CHECK_FALSE(b.builder.lastFrameReusable(nullptr, nullptr));
    rebuild(b, cl);
    rebuild(b, cl);
    REQUIRE(b.builder.lastFrameReusable(nullptr, nullptr));

    b.builder.invalidateAsset(0);
    CHECK_FALSE(b.builder.lastFrameReusable(nullptr, nullptr));
    CHECK(b.device.cached.empty());

    rebuild(b, cl);
    rebuild(b, cl);
    rebuild(b, cl);
    REQUIRE(b.builder.lastFrameReusable(nullptr, nullptr));
    b.builder.shutdown();
    CHECK_FALSE(b.builder.lastFrameReusable(nullptr, nullptr));
    CHECK(b.device.cached.empty());
}

TEST_CASE("gpu: T-162 nested segments: an outer hit skips the inner one, whose entry then expires") {
    Built b;
    CommandList cl = makeList();
    addPushEffect(cl, {blurFx(4, 4)}, Mat2x3{}, Rect{0, 0, 64, 64}, Rect{8, 8, 48, 48});
    addPushEffect(cl, {dropShadow(2, 2, 0, 0, 1, Color{0, 0, 0, 1}, false)}, Mat2x3{}, Rect{0, 0, 64, 64},
                  Rect{16, 16, 32, 32});
    addFillRect(cl, 16, 16, 32, 32, Color{1, 0, 0, 1});
    addPopEffect(cl);
    addPopEffect(cl);
    b.builder.init(b.device);
    b.builder.resize(64, 64);
    rebuild(b, cl);
    rebuild(b, cl);
    CHECK(b.frame.cacheStores.size() == 2);  // inner and outer both stored
    rebuild(b, cl);
    CHECK(cmdsWith(b.frame, Shading::Effect).empty());
    CHECK(cmdsWith(b.frame, Shading::Texture).size() == 1);  // the outer result only
    CHECK(b.device.cacheReleases == 2);                      // the probe, then the unused inner entry
    CHECK(b.device.cached.size() == 1);
    CHECK(b.builder.lastFrameReusable(nullptr, nullptr));
    b.builder.shutdown();
}

TEST_CASE("gpu: T-162 at most kEffectCacheMaxEntries results are kept") {
    Built b;
    const int n = static_cast<int>(FrameBuilder::kEffectCacheMaxEntries) + 20;
    CommandList cl = makeList(2048, 1024);
    for (int i = 0; i < n; ++i) {
        const float x = static_cast<float>((i % 32) * 64);
        const float y = static_cast<float>((i / 32) * 64);
        addPushEffect(cl, {blurFx(4, 4)}, Mat2x3{}, Rect{x, y, 64, 64}, Rect{x + 16, y + 16, 32, 32});
        addFillRect(cl, x + 16, y + 16, 32, 32, Color{1, 0, 0, 1});
        addPopEffect(cl);
    }
    b.builder.init(b.device);
    b.builder.resize(2048, 1024);
    rebuild(b, cl);
    rebuild(b, cl);
    CHECK(b.frame.cacheStores.size() == FrameBuilder::kEffectCacheMaxEntries);
    rebuild(b, cl);
    CHECK(cmdsWith(b.frame, Shading::Texture).size() == FrameBuilder::kEffectCacheMaxEntries);
    CHECK(cmdsWith(b.frame, Shading::Effect).size() == 2u * 20u);
    CHECK(b.device.cached.size() == FrameBuilder::kEffectCacheMaxEntries);
    // The 20 that do not fit are drawn every frame and never make the frame "pending".
    CHECK(b.builder.lastFrameReusable(nullptr, nullptr));
    b.builder.shutdown();
}

TEST_CASE("gpu: T-162 engine:// textures: a rebind withdraws reuse and their segments are never cached") {
    constexpr std::string_view kDoc = R"xt(<xt>
  <body style="padding: 0">
    <div style="effect: blur(6px)"><img src="engine://scene"/></div>
  </body>
</xt>)xt";
    TestAssetResolver assets;
    assets.setExternalTexture("scene", 42, 256, 128);
    UiDoc ui;
    REQUIRE(loadUiString(ui, assets, kDoc, 512, 512));

    Built b;
    b.builder.init(b.device);
    b.builder.resize(512, 512);
    for (int i = 0; i < 3; ++i) {
        b.builder.build(ui.cmds, &ui.shaper, &assets, &ui.doc->ir(), b.frame);
        checkFrameWellFormed(b.frame);
        CHECK_FALSE(cmdsWith(b.frame, Shading::Effect).empty());  // the host texture may change
        CHECK(b.frame.cacheStores.empty());
    }
    CHECK(b.builder.lastFrameReusable(&assets, &ui.doc->ir()));
    CHECK_FALSE(b.builder.lastFrameReusable(nullptr, nullptr));  // cannot re-check the binding
    assets.setExternalTexture("scene", 43, 256, 128);
    CHECK_FALSE(b.builder.lastFrameReusable(&assets, &ui.doc->ir()));
    b.builder.build(ui.cmds, &ui.shaper, &assets, &ui.doc->ir(), b.frame);
    CHECK(b.builder.lastFrameReusable(&assets, &ui.doc->ir()));
    assets.clearExternalTexture("scene");
    CHECK_FALSE(b.builder.lastFrameReusable(&assets, &ui.doc->ir()));
    b.builder.shutdown();
}

TEST_CASE("gpu: T-202 DrawParticles is one instanced command with its instance buffer") {
    CommandList cl = makeList();
    ParticlesCmd pc{};
    pc.xf = Mat2x3{1, 0, 0, 1, 10, 20};
    pc.texture = kInvalidAsset;
    pc.blend = ParticleBlend::Additive;
    pc.instanceOffset = 0;
    pc.count = 3;
    for (int i = 0; i < 3; ++i) {
        xt::ParticleInstance p{};
        p.x = static_cast<float>(4 * i);
        p.y = 2;
        p.w = 4;
        p.h = 6;
        p.rot = 0.5f * static_cast<float>(i);
        p.rgba = Color{0.5f, 0.25f, 0, 0.5f};
        p.uv0 = Vec2{0, 0};
        p.uv1 = Vec2{1, 1};
        cl.particleInstances.push_back(p);
    }
    cl.particles.push_back(pc);
    cl.commands.push_back(Command{CmdKind::DrawParticles, {}, 0});
    cl.particles.push_back(ParticlesCmd{Mat2x3{}, kInvalidAsset, ParticleBlend::Normal, 0, 0, 1, 2});
    cl.commands.push_back(Command{CmdKind::DrawParticles, {}, 1});
    cl.devicePixelRatio = 2;
    Built b;
    buildInto(b, cl, 128, 128);
    const GpuFrame& f = b.frame;
    REQUIRE(f.cmds.size() == 2);
    const DrawCmd& a = f.cmds[0];
    CHECK(a.kind == DrawKind::Particles);
    CHECK(a.shading == Shading::ParticleSolid);
    CHECK(a.blend == Blend::Additive);
    CHECK(a.indexCount == 0);
    CHECK(a.vertexCount == 0);
    CHECK(a.firstInstance == 0);
    CHECK(a.instanceCount == 3);
    const DrawCmd& n = f.cmds[1];
    CHECK(n.blend == Blend::SrcOver);
    CHECK(n.firstInstance == 3);  // not batched: its own instance range
    CHECK(n.instanceCount == 2);
    REQUIRE(f.particleInstances.size() == 5);
    const gpu::ParticleInstance& p1 = f.particleInstances[1];
    CHECK(p1.x == doctest::Approx(4));
    CHECK(p1.h == doctest::Approx(6));
    CHECK(p1.rot == doctest::Approx(0.5));
    CHECK((p1.rgba & 0xFFu) == 128u);          // premultiplied red as given
    CHECK(((p1.rgba >> 8) & 0xFFu) == 64u);    // green
    CHECK(((p1.rgba >> 24) & 0xFFu) == 128u);  // alpha
    ParticleParams pp{};
    REQUIRE(a.paramIndex + sizeof(pp) / sizeof(float) <= f.params.size());
    std::memcpy(static_cast<void*>(&pp), f.params.data() + a.paramIndex, sizeof(pp));
    CHECK(pp.xf[0] == doctest::Approx(2));  // dpr 2
    CHECK(pp.xf[4] == doctest::Approx(20));
    CHECK(pp.xf[5] == doctest::Approx(40));
    CHECK(pp.straight == 0);
    std::string dump;
    dumpGpuFrame(f, dump);
    CHECK(dump.find("ParticleSolid Additive") != std::string::npos);
    CHECK(dump.find("particles=0+3") != std::string::npos);
    CHECK(dump.find("particles=3+2") != std::string::npos);
    // A textured emitter whose texture cannot be resolved draws nothing.
    CommandList t = makeList();
    t.particleInstances.push_back(cl.particleInstances[0]);
    t.particles.push_back(ParticlesCmd{Mat2x3{}, 7, ParticleBlend::Normal, 0, 0, 0, 1});
    t.commands.push_back(Command{CmdKind::DrawParticles, {}, 0});
    Built bt;
    buildInto(bt, t, 64, 64);
    CHECK(bt.frame.cmds.empty());
    CHECK(bt.frame.particleInstances.empty());
}

TEST_CASE("gpu: T-215 invalidateAllAssets destroys every image texture and re-uploads on the next build") {
    std::vector<uint8_t> bytes = emptyIr();
    Result<IrView> viewResult = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(viewResult.ok());
    TestAssetResolver assets;
    const AssetId a = assets.add(makeChecker4x4());
    CommandList cl = makeList();
    addDrawImage(cl, a, Rect{0, 0, 32, 32}, 1.0f, true);
    Built b;
    buildInto(b, cl, 64, 64, nullptr, &assets, &viewResult.value());
    CHECK(b.device.aliveCount() == 1);
    b.builder.invalidateAllAssets();
    CHECK(b.device.aliveCount() == 0);
    CHECK_FALSE(b.builder.lastFrameReusable(&assets, &viewResult.value()));
    b.builder.build(cl, nullptr, &assets, &viewResult.value(), b.frame);
    CHECK(b.device.aliveCount() == 1);
    b.builder.shutdown();
    CHECK(b.device.aliveCount() == 0);
}

// =================================================================================================
// T-245: DrawImageMesh -> DrawKind::Triangles (Docs/design/32-rigging-and-deformation.md §32.2.4).
// =================================================================================================

namespace {

void addTestImageMesh(CommandList& cl, AssetId asset, float opacity, bool smooth) {
    ImageMeshCmd mc{};
    mc.asset = asset;
    mc.vertexOffset = static_cast<uint32_t>(cl.meshVertices.size());
    mc.vertexCount = 5;
    mc.indexOffset = static_cast<uint32_t>(cl.meshIndices.size());
    mc.indexCount = 12;
    mc.opacity = opacity;
    mc.smooth = smooth ? 1 : 0;
    cl.meshVertices.push_back(MeshVertex{{4, 4}, {0, 0}});
    cl.meshVertices.push_back(MeshVertex{{28, 6}, {1, 0}});
    cl.meshVertices.push_back(MeshVertex{{30, 30}, {1, 1}});
    cl.meshVertices.push_back(MeshVertex{{3, 27}, {0, 1}});
    cl.meshVertices.push_back(MeshVertex{{18, 15}, {0.5f, 0.5f}});
    const uint16_t idx[12] = {0, 1, 4, 1, 2, 4, 2, 3, 4, 3, 0, 4};
    cl.meshIndices.insert(cl.meshIndices.end(), idx, idx + 12);
    cl.imageMeshes.push_back(mc);
    cl.commands.push_back(
        Command{CmdKind::DrawImageMesh, {}, static_cast<uint32_t>(cl.imageMeshes.size() - 1)});
}

}  // namespace

TEST_CASE("gpu: T-245 DrawImageMesh is one textured triangle list; it batches with a DrawImage") {
    std::vector<uint8_t> bytes = emptyIr();
    Result<IrView> viewResult = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(viewResult.ok());
    TestAssetResolver assets;
    AssetId id = assets.add(makeChecker4x4());

    // Alone, at dpr 2, half opacity, nearest.
    CommandList cl = makeList();
    cl.devicePixelRatio = 2;
    addTestImageMesh(cl, id, 0.5f, false);
    Built b;
    buildInto(b, cl, 128, 128, nullptr, &assets, &viewResult.value());
    const GpuFrame& f = b.frame;
    REQUIRE(f.cmds.size() == 1);
    const DrawCmd& c = f.cmds[0];
    CHECK(c.kind == DrawKind::Triangles);
    CHECK(c.shading == Shading::Texture);
    CHECK(c.sampler == Sampler::PointClamp);
    CHECK(c.blend == Blend::SrcOver);
    CHECK(c.stencilOp == StencilOp::TestEqualRef);
    CHECK(c.vertexCount == 5);
    CHECK(c.indexCount == 12);
    CHECK(b.device.aliveCount() == 1);
    CHECK(b.device.textures[c.texture].w == 4);
    const Vertex& v2 = f.vertices[c.baseVertex + 2];
    CHECK(v2.x == doctest::Approx(60));  // 30 x dpr 2
    CHECK(v2.y == doctest::Approx(60));
    CHECK(v2.w == doctest::Approx(1));
    CHECK(v2.u == doctest::Approx(1));
    CHECK(v2.v == doctest::Approx(1));
    CHECK(v2.rgba == 0x80808080u);
    CHECK(f.indices[c.firstIndex + 2] == 4);
    CHECK(f.indices[c.firstIndex + 11] == 4);

    // After a DrawImage of the same texture and state: one command, the mesh's indices rebased.
    CommandList both = makeList();
    addDrawImage(both, id, Rect{0, 0, 32, 32}, 1.0f, true);
    addTestImageMesh(both, id, 1.0f, true);
    Built b2;
    buildInto(b2, both, 64, 64, nullptr, &assets, &viewResult.value());
    REQUIRE(b2.frame.cmds.size() == 1);
    const DrawCmd& m = b2.frame.cmds[0];
    CHECK(m.vertexCount == 4 + 5);
    CHECK(m.indexCount == 6 + 12);
    CHECK(b2.frame.indices[m.firstIndex + 6 + 2] == 4 + 4);

    // Out-of-range indices or an unresolvable asset draw nothing.
    CommandList bad = makeList();
    addTestImageMesh(bad, id, 1.0f, true);
    bad.meshIndices[3] = 9;
    addTestImageMesh(bad, 77, 1.0f, true);
    Built b3;
    buildInto(b3, bad, 64, 64, nullptr, &assets, &viewResult.value());
    CHECK(b3.frame.cmds.empty());
}

// =================================================================================================
// T-263a: the shared clip depth limit (E8) and command list validation (E1).
// =================================================================================================

TEST_CASE("gpu: T-263a a 16th nested clip is reduced to its bounding box and the pops stay balanced") {
    // Rotated rects become stencil levels; the 16th push is past xt::kMaxClipDepth: no stencil
    // level, but a scissor level of the shape's bounds (T-263b).
    CommandList cl = makeList();
    const uint32_t kNested = kMaxClipDepth + 1;
    for (uint32_t i = 0; i < kNested; ++i) {
        const float half = 30.0f - static_cast<float>(i);
        addPushClipRect(cl, Rect{-half, -half, 2 * half, 2 * half},
                        mul(translation(Vec2{32, 32}), rotationDeg(10.0f)));
    }
    addFillRect(cl, 0, 0, 64, 64, Color{0, 0, 1, 1});
    for (uint32_t i = 0; i < kNested; ++i) {
        addPopClip(cl);
    }
    addFillRect(cl, 0, 0, 64, 64, Color{1, 0, 0, 1});
    Built b;
    buildInto(b, cl, 64, 64);
    CHECK(cmdsWith(b.frame, StencilOp::ClipPush).size() == kMaxClipDepth);
    CHECK(cmdsWith(b.frame, StencilOp::ClipPop).size() == kMaxClipDepth);
    CHECK(std::string(b.builder.lastError()).find("clip depth limit") != std::string::npos);
    // The fill inside all 16 is cut to the 16th rect's bounds: half = 15 turned by 10 degrees is
    // 32 +- 17.376, pixels 15..48 (the 15th level's own bounds are the wider 13..49).
    auto fills = cmdsWith(b.frame, Shading::Solid);
    REQUIRE(fills.size() == 2);
    CHECK(fills[0]->stencilRef == kMaxClipDepth);
    CHECK(fills[0]->clipX == 15);
    CHECK(fills[0]->clipY == 15);
    CHECK(fills[0]->clipW == 34);
    CHECK(fills[0]->clipH == 34);
    // The last fill is drawn after every level is popped (stencil reference 0, whole surface).
    REQUIRE(!b.frame.cmds.empty());
    CHECK(b.frame.cmds.back().stencilRef == 0);
    CHECK(fills[1]->clipX == 0);
    CHECK(fills[1]->clipW == 64);
}

TEST_CASE("gpu: T-263a axis-aligned clips are scissor levels and nest past kMaxClipDepth") {
    CommandList cl = makeList();
    const uint32_t kNested = kMaxClipDepth + 5;
    for (uint32_t i = 0; i < kNested; ++i) {
        const float inset = static_cast<float>(i);
        addPushClipRect(cl, Rect{inset, inset, 64 - 2 * inset, 64 - 2 * inset});
    }
    addFillRect(cl, 0, 0, 64, 64, Color{0, 0, 1, 1});
    for (uint32_t i = 0; i < kNested; ++i) {
        addPopClip(cl);
    }
    Built b;
    buildInto(b, cl, 64, 64);
    CHECK(cmdsWith(b.frame, StencilOp::ClipPush).empty());
    CHECK(std::string(b.builder.lastError()).find("clip depth limit") == std::string::npos);
    // The fill is cut by the innermost rectangle: every one of the 20 clips applied.
    REQUIRE(!b.frame.cmds.empty());
    const float innermost = static_cast<float>(kNested - 1);
    CHECK(b.frame.cmds.back().clipX == static_cast<int>(innermost));
    CHECK(b.frame.cmds.back().clipW == 64 - 2 * static_cast<int>(innermost));
}

TEST_CASE("gpu: T-263a an invalid command list builds an empty frame instead of reading out of bounds") {
    SUBCASE("a command index past its pool") {
        CommandList cl = makeList();
        addFillRect(cl, 0, 0, 64, 64, Color{0, 0, 1, 1});
        cl.commands.push_back(Command{CmdKind::FillPath, {}, 1000});
        Built b;
        buildInto(b, cl, 64, 64);
        CHECK(b.frame.cmds.empty());
        CHECK(!std::string(b.builder.lastError()).empty());
    }
    SUBCASE("effectCount = 0xFFFFFFFF") {
        CommandList cl = makeList();
        addPushEffect(cl, {blurFx(4, 4)}, Mat2x3{}, Rect{0, 0, 64, 64}, Rect{8, 8, 48, 48});
        addFillRect(cl, 8, 8, 48, 48, Color{0, 0, 1, 1});
        addPopEffect(cl);
        cl.effectCmds.back().effectCount = 0xFFFFFFFFu;
        Built b;
        buildInto(b, cl, 64, 64);
        CHECK(b.frame.cmds.empty());
    }
    SUBCASE("a PopLayer with nothing open") {
        CommandList cl = makeList();
        addPopLayer(cl);
        addFillRect(cl, 0, 0, 64, 64, Color{0, 0, 1, 1});
        Built b;
        buildInto(b, cl, 64, 64);
        CHECK(b.frame.cmds.empty());
    }
}

// =================================================================================================
// T-263b E4: the image texture cache follows the IR and the resolver of the build.
// =================================================================================================

namespace {

ImageData makeSolid(int w, int h, uint8_t v) {
    ImageData img;
    img.width = w;
    img.height = h;
    img.rgba.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, v);
    return img;
}

}  // namespace

TEST_CASE("gpu: T-263b a build with another IR or resolver drops the image textures cached by id") {
    // Two documents: AssetId 0 names a 4x4 image in the first and a 2x2 image in the second.
    std::vector<uint8_t> bytesA = emptyIr();
    std::vector<uint8_t> bytesB = emptyIr();
    Result<IrView> viewA = IrView::open(IrBlob{bytesA.data(), bytesA.size()});
    Result<IrView> viewB = IrView::open(IrBlob{bytesB.data(), bytesB.size()});
    REQUIRE(viewA.ok());
    REQUIRE(viewB.ok());
    TestAssetResolver assetsA;
    TestAssetResolver assetsB;
    REQUIRE(assetsA.add(makeSolid(4, 4, 255)) == 0);
    REQUIRE(assetsB.add(makeSolid(2, 2, 255)) == 0);
    CommandList cl = makeList();
    addDrawImage(cl, 0, Rect{0, 0, 32, 32}, 1.0f, true);

    SUBCASE("another IR and another resolver") {
        Built b;
        buildInto(b, cl, 64, 64, nullptr, &assetsA, &viewA.value());
        auto imgs = cmdsWith(b.frame, Shading::Texture);
        REQUIRE(imgs.size() == 1);
        CHECK(b.device.textures[imgs[0]->texture].w == 4);
        // The host switches documents and calls nothing else.
        CHECK_FALSE(b.builder.lastFrameReusable(&assetsB, &viewB.value()));
        b.builder.build(cl, nullptr, &assetsB, &viewB.value(), b.frame);
        imgs = cmdsWith(b.frame, Shading::Texture);
        REQUIRE(imgs.size() == 1);
        CHECK(b.device.textures[imgs[0]->texture].w == 2);
        CHECK(b.device.textures[imgs[0]->texture].alive);
        // T-263c ruling 3: the first document's texture is kept (the two most recent IRs are),
        // under its own IR; it is not what asset 0 draws here.
        CHECK(b.device.aliveCount() == 2);
    }
    SUBCASE("the same IR with a replaced resolver") {
        Built b;
        buildInto(b, cl, 64, 64, nullptr, &assetsA, &viewA.value());
        CHECK_FALSE(b.builder.lastFrameReusable(&assetsB, &viewA.value()));
        b.builder.build(cl, nullptr, &assetsB, &viewA.value(), b.frame);
        auto imgs = cmdsWith(b.frame, Shading::Texture);
        REQUIRE(imgs.size() == 1);
        CHECK(b.device.textures[imgs[0]->texture].w == 2);
        CHECK(b.device.aliveCount() == 2);  // T-263c: as above
    }
    SUBCASE("another IR with the same resolver uploads again") {
        Built b;
        buildInto(b, cl, 64, 64, nullptr, &assetsA, &viewA.value());
        const size_t created = b.device.textures.size();
        CHECK_FALSE(b.builder.lastFrameReusable(&assetsA, &viewB.value()));
        b.builder.build(cl, nullptr, &assetsA, &viewB.value(), b.frame);
        CHECK(b.device.textures.size() == created + 1);
        CHECK(b.device.aliveCount() == 2);  // T-263c: as above
    }
    SUBCASE("the same IR and resolver keep the texture, also across a build without them") {
        Built b;
        buildInto(b, cl, 64, 64, nullptr, &assetsA, &viewA.value());
        const size_t created = b.device.textures.size();
        b.builder.build(cl, nullptr, &assetsA, &viewA.value(), b.frame);
        CommandList plain = makeList();
        addFillRect(plain, 0, 0, 64, 64, Color{0, 0, 1, 1});
        b.builder.build(plain, nullptr, nullptr, nullptr, b.frame);
        b.builder.build(cl, nullptr, &assetsA, &viewA.value(), b.frame);
        CHECK(b.device.textures.size() == created);
        CHECK(b.device.aliveCount() == 1);
        CHECK(b.builder.lastFrameReusable(&assetsA, &viewA.value()));
    }
}

// =================================================================================================
// T-263b E10: an image the device cannot hold is not drawn, and not uploaded again every frame.
// =================================================================================================

namespace {

/// A resolver whose imageView() pointer is the asset's own, as FileAssetResolver's is (the base
/// class hands out one scratch address for every asset).
class PerAssetViewResolver : public TestAssetResolver {
public:
    using TestAssetResolver::imageView;
    const ImageData* imageView(AssetId asset, const IrView& view) override {
        if (asset >= m_views.size()) {
            m_views.resize(asset + 1);  // a deque: earlier addresses stay
        }
        return decodeImage(asset, view, m_views[asset]) ? &m_views[asset] : nullptr;
    }

private:
    std::deque<ImageData> m_views;
};

}  // namespace

TEST_CASE("gpu: T-263b an image the device refuses is skipped and remembered as not uploadable") {
    std::vector<uint8_t> bytes = emptyIr();
    Result<IrView> viewResult = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(viewResult.ok());
    PerAssetViewResolver assets;
    const AssetId wide = assets.add(makeSolid(8, 2, 255));  // past the device's 4 px limit
    const AssetId small = assets.add(makeSolid(4, 4, 255));
    CommandList cl = makeList();
    addDrawImage(cl, wide, Rect{0, 0, 32, 8}, 1.0f, true);
    addDrawImage(cl, small, Rect{0, 16, 16, 16}, 1.0f, true);

    Built b;
    b.device.maxSize = 4;
    buildInto(b, cl, 64, 64, nullptr, &assets, &viewResult.value());
    // Only the image that fits is drawn; the failure is reported.
    auto imgs = cmdsWith(b.frame, Shading::Texture);
    REQUIRE(imgs.size() == 1);
    CHECK(b.device.textures[imgs[0]->texture].w == 4);
    CHECK(b.device.aliveCount() == 1);
    CHECK(!std::string(b.builder.lastError()).empty());
    const int calls = b.device.createCalls;

    // The following frames neither premultiply nor try to upload the refused image again...
    b.builder.build(cl, nullptr, &assets, &viewResult.value(), b.frame);
    b.builder.build(cl, nullptr, &assets, &viewResult.value(), b.frame);
    CHECK(b.device.createCalls == calls);
    CHECK(cmdsWith(b.frame, Shading::Texture).size() == 1);
    CHECK(!std::string(b.builder.lastError()).empty());  // ...but keep reporting it.

    // ...until the host says the asset changed.
    b.builder.invalidateAsset(wide);
    b.device.maxSize = 0xFFFF;
    b.builder.build(cl, nullptr, &assets, &viewResult.value(), b.frame);
    CHECK(cmdsWith(b.frame, Shading::Texture).size() == 2);
    CHECK(b.device.aliveCount() == 2);
    b.builder.shutdown();
    CHECK(b.device.aliveCount() == 0);
}

// =================================================================================================
// T-263c rulings 1, 2b, 3: what an image texture is cached under.
// =================================================================================================

namespace {

/// The texture each Shading::Texture command of `f` samples, in order.
std::vector<const TextureRecord*> imageTextures(const Built& b) {
    std::vector<const TextureRecord*> out;
    for (const DrawCmd* c : cmdsWith(b.frame, Shading::Texture)) {
        auto it = b.device.textures.find(c->texture);
        out.push_back(it != b.device.textures.end() ? &it->second : nullptr);
    }
    return out;
}

}  // namespace

// Ruling 1: the base class's imageView() decodes into one scratch image, so two assets come back at
// the same address; only assets of one atlas page (by their names in the IR) share a texture.
TEST_CASE("gpu: T-263c two images of a resolver without its own imageView get a texture each") {
    std::vector<uint8_t> bytes = emptyIr();
    Result<IrView> viewResult = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(viewResult.ok());
    TestAssetResolver assets;  // AssetResolver::imageView: the shared scratch
    ImageData red200 = makeSolid(4, 4, 255);
    ImageData red90 = makeSolid(4, 4, 255);  // same size, another first texel
    red200.rgba[0] = 200;
    red90.rgba[0] = 90;
    const AssetId first = assets.add(red200);
    const AssetId second = assets.add(red90);
    const AssetId third = assets.add(makeSolid(2, 2, 255));
    CommandList cl = makeList();
    addDrawImage(cl, first, Rect{0, 0, 16, 16}, 1.0f, true);
    addDrawImage(cl, second, Rect{16, 0, 16, 16}, 1.0f, true);
    addDrawImage(cl, third, Rect{32, 0, 16, 16}, 1.0f, true);

    Built b;
    buildInto(b, cl, 64, 64, nullptr, &assets, &viewResult.value());
    const std::vector<const TextureRecord*> texs = imageTextures(b);
    REQUIRE(texs.size() == 3);  // three textures: no batching into one command
    REQUIRE(texs[0] != nullptr);
    REQUIRE(texs[1] != nullptr);
    REQUIRE(texs[2] != nullptr);
    CHECK(texs[0] != texs[1]);
    CHECK(texs[0]->firstByte == 200);
    CHECK(texs[1]->firstByte == 90);
    CHECK(texs[2]->w == 2);
    CHECK(b.device.aliveCount() == 3);
    // Cached per asset afterwards.
    const int calls = b.device.createCalls;
    b.builder.build(cl, nullptr, &assets, &viewResult.value(), b.frame);
    CHECK(b.device.createCalls == calls);
    b.builder.shutdown();
    CHECK(b.device.aliveCount() == 0);
}

namespace {

/// An IR whose assets are the cells `names` of bundle atlas pages (`atlas:<n>#<asset>`, kind 3).
std::vector<uint8_t> irWithAtlasAssets(std::initializer_list<const char*> names) {
    compile::IrWriter writer;
    for (const char* name : names) {
        compile::AssetBuilder asset;
        asset.name = writer.intern(name);
        asset.kind = 3;
        writer.addAsset(asset);
    }
    compile::SceneBuilder scene;
    scene.width = 1;
    scene.height = 1;
    writer.addScene(scene);
    return writer.finish();
}

/// Every asset decodes to the one image `page` (an atlas page shared by its cells).
class OnePageResolver : public AssetResolver {
public:
    ImageData page;
    bool readBytes(std::string_view, std::vector<uint8_t>&) override { return false; }
    bool decodeImage(AssetId, const IrView&, ImageData& out) override {
        out = page;
        return true;
    }
    Vec2 imageSize(AssetId, const IrView&) override { return Vec2{1, 1}; }
    using AssetResolver::imageView;
    const ImageData* imageView(AssetId, const IrView&) override { return &page; }
};

}  // namespace

TEST_CASE("gpu: T-263c the cells of one atlas page still share its texture; another page's do not") {
    // Assets 0 and 1 are cells of page 0; asset 2 is on page 1, though this resolver hands out the
    // same image address for it.
    std::vector<uint8_t> bytes = irWithAtlasAssets({"atlas:0#a.png", "atlas:0#b.png", "atlas:1#c.png"});
    Result<IrView> viewResult = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(viewResult.ok());
    OnePageResolver assets;
    assets.page = makeSolid(8, 8, 255);
    CommandList cl = makeList();
    addDrawImage(cl, 0, Rect{0, 0, 16, 16}, 1.0f, true);
    addDrawImage(cl, 1, Rect{16, 0, 16, 16}, 1.0f, true);
    addDrawImage(cl, 2, Rect{32, 0, 16, 16}, 1.0f, true);
    Built b;
    buildInto(b, cl, 64, 64, nullptr, &assets, &viewResult.value());
    CHECK(b.device.createCalls == 2);
    CHECK(b.device.aliveCount() == 2);
    b.builder.shutdown();
    CHECK(b.device.aliveCount() == 0);
}

// Ruling 2b: the IR is identified by IrView::loadSerial(), not by where the view or its blob is.
TEST_CASE("gpu: T-263c an IrView opened again over the same memory drops the textures cached by id") {
    std::vector<uint8_t> bytes = emptyIr();
    Result<IrView> opened = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(opened.ok());
    IrView view = opened.value();
    PerAssetViewResolver assets4;
    REQUIRE(assets4.add(makeSolid(4, 4, 255)) == 0);
    CommandList cl = makeList();
    addDrawImage(cl, 0, Rect{0, 0, 32, 32}, 1.0f, true);

    Built b;
    buildInto(b, cl, 64, 64, nullptr, &assets4, &view);
    REQUIRE(imageTextures(b).size() == 1);
    CHECK(imageTextures(b)[0]->w == 4);
    const int calls = b.device.createCalls;

    // A copy of the view is the same IR: nothing is uploaded again, the frame stays reusable.
    const IrView copy = view;
    CHECK(copy.loadSerial() == view.loadSerial());
    CHECK(b.builder.lastFrameReusable(&assets4, &copy));
    b.builder.build(cl, nullptr, &assets4, &copy, b.frame);
    CHECK(b.device.createCalls == calls);

    // The host recompiles into the same buffer and opens it into the same IrView object: the
    // view's address, the blob's address and its size are all what they were. (The resolver
    // object is the same too; what it decodes for asset 0 changed with the IR.)
    const std::vector<uint8_t> rebuilt = emptyIr();
    REQUIRE(rebuilt.size() == bytes.size());
    std::copy(rebuilt.begin(), rebuilt.end(), bytes.begin());
    Result<IrView> reopened = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(reopened.ok());
    view = reopened.value();
    CHECK(view.loadSerial() != copy.loadSerial());
    assets4.replace(0, makeSolid(2, 2, 255));
    CHECK_FALSE(b.builder.lastFrameReusable(&assets4, &view));
    b.builder.build(cl, nullptr, &assets4, &view, b.frame);
    REQUIRE(imageTextures(b).size() == 1);
    CHECK(imageTextures(b)[0]->w == 2);
    b.builder.shutdown();
}

// Ruling 3: textures are cached per (IR, AssetId) and the two most recent IRs are kept, so a
// builder shared by two documents does not upload everything again on every switch.
TEST_CASE("gpu: T-263c two IRs built in turn upload each image once; a third evicts the oldest") {
    std::vector<uint8_t> bytesA = emptyIr();
    std::vector<uint8_t> bytesB = emptyIr();
    std::vector<uint8_t> bytesC = emptyIr();
    Result<IrView> viewA = IrView::open(IrBlob{bytesA.data(), bytesA.size()});
    Result<IrView> viewB = IrView::open(IrBlob{bytesB.data(), bytesB.size()});
    Result<IrView> viewC = IrView::open(IrBlob{bytesC.data(), bytesC.size()});
    REQUIRE(viewA.ok());
    REQUIRE(viewB.ok());
    REQUIRE(viewC.ok());
    // One resolver for all three (a bundle's): asset 0 is 4 px wide in A, 2 px in B, 6 px in C,
    // and A has a second image.
    struct PerIrResolver : PerAssetViewResolver {
        const IrView* a = nullptr;
        const IrView* b = nullptr;
        using PerAssetViewResolver::imageView;
        const ImageData* imageView(AssetId asset, const IrView& view) override {
            // Ids 0..1 are A's, 2 is B's, 3 is C's in the underlying table.
            const AssetId mapped = &view == a ? asset : &view == b ? 2 : 3;
            return PerAssetViewResolver::imageView(mapped, view);
        }
    } assets;
    assets.a = &viewA.value();
    assets.b = &viewB.value();
    assets.add(makeSolid(4, 4, 255));
    assets.add(makeSolid(8, 8, 255));
    assets.add(makeSolid(2, 2, 255));
    assets.add(makeSolid(6, 6, 255));
    CommandList listA = makeList();
    addDrawImage(listA, 0, Rect{0, 0, 16, 16}, 1.0f, true);
    addDrawImage(listA, 1, Rect{16, 0, 16, 16}, 1.0f, true);
    CommandList listOne = makeList();
    addDrawImage(listOne, 0, Rect{0, 0, 16, 16}, 1.0f, true);

    Built b;
    b.builder.init(b.device);
    b.builder.resize(64, 64);
    auto widths = [&]() {
        std::vector<int> out;
        for (const TextureRecord* t : imageTextures(b)) {
            out.push_back(t != nullptr && t->alive ? t->w : -1);
        }
        return out;
    };
    for (int round = 0; round < 10; ++round) {
        CAPTURE(round);
        b.builder.build(listA, nullptr, &assets, &viewA.value(), b.frame);
        CHECK(widths() == std::vector<int>{4, 8});
        // Another IR than the last build's: that frame is not reusable, whatever is cached.
        CHECK_FALSE(b.builder.lastFrameReusable(&assets, &viewB.value()));
        b.builder.build(listOne, nullptr, &assets, &viewB.value(), b.frame);
        CHECK(widths() == std::vector<int>{2});
    }
    CHECK(b.device.createCalls == 3);  // the three images, once each
    CHECK(b.device.aliveCount() == 3);

    // A third IR (an embedding host: one IR per plate, one builder): A, the least recently
    // built, goes with both of its textures.
    b.builder.build(listOne, nullptr, &assets, &viewC.value(), b.frame);
    CHECK(widths() == std::vector<int>{6});
    CHECK(b.device.createCalls == 4);
    CHECK(b.device.aliveCount() == 2);  // B's and C's
    b.builder.build(listOne, nullptr, &assets, &viewB.value(), b.frame);
    CHECK(widths() == std::vector<int>{2});
    CHECK(b.device.createCalls == 4);  // B was kept
    // A again: uploaded again, and C (now the oldest) goes.
    b.builder.build(listA, nullptr, &assets, &viewA.value(), b.frame);
    CHECK(widths() == std::vector<int>{4, 8});
    CHECK(b.device.createCalls == 6);
    CHECK(b.device.aliveCount() == 3);  // B's one and A's two

    // invalidateAllAssets() still drops everything, of every IR.
    b.builder.invalidateAllAssets();
    CHECK(b.device.aliveCount() == 0);
    b.builder.build(listOne, nullptr, &assets, &viewB.value(), b.frame);
    CHECK(widths() == std::vector<int>{2});
    CHECK(b.device.aliveCount() == 1);
    b.builder.shutdown();
    CHECK(b.device.aliveCount() == 0);
}

// =================================================================================================
// T-263b: the simplifier's output is structurally valid for the whole UI corpus, so it is handed
// to the backends marked `structureChecked` (they skip validateCommandList()). Here because this
// suite already compiles and paints the Tests/golden/ui documents.
// =================================================================================================

TEST_CASE("simplifier: T-263b every Tests/golden/ui document simplifies to a valid, checked list") {
    std::vector<std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator(XT_GPU_UI_DIR)) {
        if (entry.path().extension() == ".xt") {
            files.push_back(entry.path().filename().string());
        }
    }
    std::sort(files.begin(), files.end());
    REQUIRE(files.size() >= 50);

    // Every on/off combination of the nine rules.
    constexpr uint32_t kRules = 9;
    auto optionsOf = [](uint32_t mask) {
        SimplifyOptions o;
        o.flatten3D = (mask & 1u) != 0;
        o.strokes = (mask & 2u) != 0;
        o.gradients = (mask & 4u) != 0;
        o.layers = (mask & 8u) != 0;
        o.masks = (mask & 16u) != 0;
        o.clipPaths = (mask & 32u) != 0;
        o.glyphsToPaths = (mask & 64u) != 0;
        o.effects = (mask & 128u) != 0;
        o.particles = (mask & 256u) != 0;
        return o;
    };
    CommandListSimplifier simplifier;
    CommandList work;
    std::string notLoaded;
    std::string invalid;
    std::string unchecked;
    size_t loaded = 0;
    for (const std::string& file : files) {
        UiDoc ui;
        if (!loadUiFixture(ui, file.c_str(), 256, 256)) {
            notLoaded += file + " ";
            continue;
        }
        ++loaded;
        REQUIRE(validateCommandList(ui.cmds));
        for (uint32_t mask = 0; mask < (1u << kRules); ++mask) {
            work = ui.cmds;
            simplifier.simplify(work, optionsOf(mask), &ui.shaper);
            if (!validateCommandList(work)) {
                invalid += file + "#" + std::to_string(mask) + " ";
                break;  // one report per document
            }
            if (!work.structureChecked) {
                unchecked += file + "#" + std::to_string(mask) + " ";
                break;
            }
        }
    }
    CAPTURE(notLoaded);
    CAPTURE(invalid);
    CAPTURE(unchecked);
    CHECK(loaded == files.size());
    CHECK(invalid.empty());
    CHECK(unchecked.empty());
}

// =================================================================================================
// T-263c A: several Documents of one Context are painted into one CommandList, and every IR
// numbers its assets from 0 while a backend is handed a single IrView. Test and finding only.
// =================================================================================================

namespace {

/// One Context with two surfaces whose asset 0 is another image: the first document embeds an
/// 8x8 red PNG and draws it at x 0..16, the second names a 4x4 blue PNG file and draws it at
/// x 32..48 (64 x 32 viewport). `aloneA` / `aloneB` hold each document loaded by itself.
struct TwoSurfaces {
    static constexpr int kW = 64;
    static constexpr int kH = 32;
    std::filesystem::path dir;
    std::unique_ptr<FileAssetResolver> assets;
    NullTextShaper shaper;
    std::vector<uint8_t> irA, irB;
    std::unique_ptr<Context> both, aloneA, aloneB;
    Document* bothDocA = nullptr;
    Document* bothDocB = nullptr;
    CommandList listBoth, listA, listB;

    static std::vector<uint8_t> compileDoc(const std::filesystem::path& root, const char* image, int left,
                                           bool embed) {
        const std::string xt = std::string("<xt><body style=\"padding: 0\"><img src=\"") + image +
                               "\" style=\"position: absolute; left: " + std::to_string(left) +
                               "px; top: 0; width: 16px; height: 16px\"/></body></xt>";
        compile::SourceFile file{"doc.xt", xt};
        compile::CompileOptions options;
        options.rootDir = root.string();
        options.embedImages = embed;
        return compile::compileDocumentFromStrings(std::span<const compile::SourceFile>(&file, 1), "doc.xt",
                                                   options)
            .ir;
    }

    std::unique_ptr<Context> makeContext() {
        auto ctx = std::make_unique<Context>(ContextConfig{&shaper, assets.get()});
        ctx->setViewport(Rect{0, 0, static_cast<float>(kW), static_cast<float>(kH)}, 1.0f);
        return ctx;
    }

    bool init(const char* tag) {
        dir = xt::test::tempDir() / tag;
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        std::vector<uint8_t> red(8 * 8 * 4), blue(4 * 4 * 4);
        for (size_t i = 0; i < red.size(); i += 4) {
            red[i] = 255, red[i + 1] = 0, red[i + 2] = 0, red[i + 3] = 255;
        }
        for (size_t i = 0; i < blue.size(); i += 4) {
            blue[i] = 0, blue[i + 1] = 0, blue[i + 2] = 255, blue[i + 3] = 255;
        }
        if (!sw::writePng((dir / "a.png").string(), red.data(), 8, 8, 8 * 4) ||
            !sw::writePng((dir / "b.png").string(), blue.data(), 4, 4, 4 * 4)) {
            return false;
        }
        irA = compileDoc(dir, "a.png", 0, /*embed=*/true);
        irB = compileDoc(dir, "b.png", 32, /*embed=*/false);
        if (irA.empty() || irB.empty()) {
            return false;
        }
        assets = std::make_unique<FileAssetResolver>(dir.string(), sw::decodeImageBytes);
        both = makeContext();
        aloneA = makeContext();
        aloneB = makeContext();
        Result<DocumentHandle> a = both->loadDocument(IrBlob{irA.data(), irA.size()}, false);
        Result<DocumentHandle> b = both->loadDocument(IrBlob{irB.data(), irB.size()}, false);
        if (!a.ok() || !b.ok() || !aloneA->loadDocument(IrBlob{irA.data(), irA.size()}, false).ok() ||
            !aloneB->loadDocument(IrBlob{irB.data(), irB.size()}, false).ok()) {
            return false;
        }
        bothDocA = both->document(a.value());
        bothDocB = both->document(b.value());
        both->update(0.0f);
        aloneA->update(0.0f);
        aloneB->update(0.0f);
        both->render(listBoth);
        aloneA->render(listA);
        aloneB->render(listB);
        return true;
    }

    ~TwoSurfaces() {
        both.reset();
        aloneA.reset();
        aloneB.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

std::vector<uint8_t> swRender(const CommandList& list, AssetResolver* assets, const IrView* ir) {
    std::vector<uint8_t> px(static_cast<size_t>(TwoSurfaces::kW) * TwoSurfaces::kH * 4, 0);
    sw::SwRenderer renderer;
    renderer.setTarget(sw::SwTarget{px.data(), TwoSurfaces::kW, TwoSurfaces::kH, TwoSurfaces::kW * 4});
    renderer.clear(Color{0, 0, 0, 0});
    renderer.render(list, nullptr, assets, ir);
    return px;
}

/// Pixels of the columns [x0, x1) that differ between two kW x kH buffers.
int columnsDiffer(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int x0, int x1) {
    int n = 0;
    for (int y = 0; y < TwoSurfaces::kH; ++y) {
        for (int x = x0; x < x1; ++x) {
            const size_t i = (static_cast<size_t>(y) * TwoSurfaces::kW + static_cast<size_t>(x)) * 4;
            n += std::memcmp(&a[i], &b[i], 4) != 0 ? 1 : 0;
        }
    }
    return n;
}

/// Width of the texture the image quad whose left edge is at `left` samples (0 = no such quad).
int textureWidthOfQuadAt(const Built& b, float left) {
    for (const DrawCmd& c : b.frame.cmds) {
        if (c.shading != Shading::Texture) continue;
        for (uint32_t q = 0; q + 4 <= c.vertexCount; q += 4) {
            float minX = 1e9f;
            for (uint32_t i = 0; i < 4; ++i) {
                minX = std::min(minX, b.frame.vertices[c.baseVertex + q + i].x);
            }
            if (minX == left) {
                auto it = b.device.textures.find(c.texture);
                return it != b.device.textures.end() ? it->second.w : 0;
            }
        }
    }
    return 0;
}

}  // namespace

// T-263d: Context::render names each Document's IR in CommandList::sources and every asset command
// carries its Document's index, so each surface draws its own asset 0 whichever IR the host passes.
TEST_CASE("sw: T-263c two documents in one frame each draw their own asset 0") {
    TwoSurfaces t;
    REQUIRE(t.init("xt_t263c_two_surfaces_sw"));
    const std::vector<uint8_t> refA = swRender(t.listA, t.assets.get(), &t.bothDocA->ir());
    const std::vector<uint8_t> refB = swRender(t.listB, t.assets.get(), &t.bothDocB->ir());
    // The references are what they should be: red at the left, blue at x 32..48.
    REQUIRE(refA[(8 * TwoSurfaces::kW + 8) * 4] == 255);
    REQUIRE(refB[(8 * TwoSurfaces::kW + 40) * 4 + 2] == 255);
    for (const Document* doc : {t.bothDocA, t.bothDocB}) {
        CAPTURE(doc == t.bothDocA);  // which document's IR the host passed
        const std::vector<uint8_t> px = swRender(t.listBoth, t.assets.get(), &doc->ir());
        CHECK(columnsDiffer(px, refA, 0, 32) == 0);
        CHECK(columnsDiffer(px, refB, 32, 64) == 0);
    }
}

TEST_CASE("gpu: T-263c two documents in one frame each sample their own asset 0") {
    TwoSurfaces t;
    REQUIRE(t.init("xt_t263c_two_surfaces_gpu"));
    {
        Built a, b;
        buildInto(a, t.listA, TwoSurfaces::kW, TwoSurfaces::kH, nullptr, t.assets.get(), &t.bothDocA->ir());
        buildInto(b, t.listB, TwoSurfaces::kW, TwoSurfaces::kH, nullptr, t.assets.get(), &t.bothDocB->ir());
        REQUIRE(textureWidthOfQuadAt(a, 0.0f) == 8);
        REQUIRE(textureWidthOfQuadAt(b, 32.0f) == 4);
    }
    for (const Document* doc : {t.bothDocA, t.bothDocB}) {
        CAPTURE(doc == t.bothDocA);
        Built b;
        buildInto(b, t.listBoth, TwoSurfaces::kW, TwoSurfaces::kH, nullptr, t.assets.get(), &doc->ir());
        CHECK(textureWidthOfQuadAt(b, 0.0f) == 8);
        CHECK(textureWidthOfQuadAt(b, 32.0f) == 4);
    }
}

// =================================================================================================
// T-263d: the command list's source table (CommandList::sources) on the SW and GPU paths.
// =================================================================================================

namespace {

/// One Context with two surfaces whose asset 0 is another `.xtfx` of the same name: the first
/// document's (a white square at x 8..24) falls back to a 4 px blur and multiplies by 0.125 in its body,
/// the second's (x 40..56) has no fallback and multiplies by 0.625. `aloneA` / `aloneB` hold each
/// document loaded by itself. 64 x 32 viewport, like TwoSurfaces.
struct TwoShaderSurfaces {
    std::filesystem::path dir;
    std::unique_ptr<FileAssetResolver> assets;
    NullTextShaper shaper;
    std::vector<uint8_t> irA, irB;
    std::unique_ptr<Context> both, aloneA, aloneB;
    Document* bothDocA = nullptr;
    Document* bothDocB = nullptr;
    CommandList listBoth, listA, listB;

    static std::vector<uint8_t> compileDoc(const std::filesystem::path& root, int left, const char* xtfx) {
        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        {
            std::ofstream f(root / "fx.xtfx", std::ios::binary);
            f << xtfx;
        }
        const std::string xt = "<xt><head><style>#a { position: absolute; left: " + std::to_string(left) +
                               "px; top: 8px; width: 16px; height: 16px; background-color: #ffffff;"
                               " effect: custom(\"fx.xtfx\"); }</style></head>"
                               "<body style=\"padding: 0\"><div id=\"a\"></div></body></xt>";
        compile::SourceFile file{"doc.xt", xt};
        compile::CompileOptions options;
        options.rootDir = root.string();
        return compile::compileDocumentFromStrings(std::span<const compile::SourceFile>(&file, 1), "doc.xt",
                                                   options)
            .ir;
    }

    std::unique_ptr<Context> makeContext() {
        auto ctx = std::make_unique<Context>(ContextConfig{&shaper, assets.get()});
        ctx->setViewport(Rect{0, 0, static_cast<float>(TwoSurfaces::kW), static_cast<float>(TwoSurfaces::kH)},
                         1.0f);
        return ctx;
    }

    bool init(const char* tag) {
        dir = xt::test::tempDir() / tag;
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        irA = compileDoc(dir / "a", 8,
                         "fallback: blur(4px)\n---\nfloat4 xtMain(XtInput i)\n{\n"
                         "    return xtSample(i.uv) * 0.125;\n}\n");
        irB = compileDoc(dir / "b", 40,
                         "fallback: none\n---\nfloat4 xtMain(XtInput i)\n{\n"
                         "    return xtSample(i.uv) * 0.625;\n}\n");
        if (irA.empty() || irB.empty()) {
            return false;
        }
        assets = std::make_unique<FileAssetResolver>(dir.string(), sw::decodeImageBytes);
        both = makeContext();
        aloneA = makeContext();
        aloneB = makeContext();
        Result<DocumentHandle> a = both->loadDocument(IrBlob{irA.data(), irA.size()}, false);
        Result<DocumentHandle> b = both->loadDocument(IrBlob{irB.data(), irB.size()}, false);
        if (!a.ok() || !b.ok() || !aloneA->loadDocument(IrBlob{irA.data(), irA.size()}, false).ok() ||
            !aloneB->loadDocument(IrBlob{irB.data(), irB.size()}, false).ok()) {
            return false;
        }
        bothDocA = both->document(a.value());
        bothDocB = both->document(b.value());
        both->update(0.0f);
        aloneA->update(0.0f);
        aloneB->update(0.0f);
        both->render(listBoth);
        aloneA->render(listA);
        aloneB->render(listB);
        return true;
    }

    ~TwoShaderSurfaces() {
        both.reset();
        aloneA.reset();
        aloneB.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

/// The GLSL module of every EffectProgram::Custom pass of `b.frame`, in order.
std::vector<std::string> customPassModules(const Built& b) {
    std::vector<std::string> out;
    for (const DrawCmd& c : b.frame.cmds) {
        if (c.shading != Shading::Effect || c.program != EffectProgram::Custom) continue;
        CustomEffectParams cp{};
        std::memcpy(&cp, &b.frame.params[c.paramIndex], sizeof(cp));
        const size_t id = static_cast<size_t>(cp.program);
        out.push_back(id >= 1 && id <= b.device.customGlsl.size() ? b.device.customGlsl[id - 1]
                                                                  : std::string());
    }
    return out;
}

bool hasText(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

}  // namespace

// Spec point 2: both documents' shader is asset 0. The Painter copies one ShaderRef per (source,
// id) and a Custom effect finds its own through its PushEffect's source: the first square is
// blurred (its fallback), the second is not.
TEST_CASE("sw: T-263d two documents in one frame each apply their own custom effect shader") {
    TwoShaderSurfaces t;
    REQUIRE(t.init("xt_t263d_two_shaders_sw"));
    const std::vector<uint8_t> refA = swRender(t.listA, t.assets.get(), &t.bothDocA->ir());
    const std::vector<uint8_t> refB = swRender(t.listB, t.assets.get(), &t.bothDocB->ir());
    // The references are what they should be: the first square bleeds past its edge (x 8), the
    // second one's edge (x 40) is hard.
    REQUIRE(refA[(16 * TwoSurfaces::kW + 7) * 4 + 3] != 0);
    REQUIRE(refB[(16 * TwoSurfaces::kW + 39) * 4 + 3] == 0);
    REQUIRE(refB[(16 * TwoSurfaces::kW + 48) * 4 + 3] == 255);
    CHECK(t.listBoth.shaders.size() == 2);
    for (const Document* doc : {t.bothDocA, t.bothDocB}) {
        CAPTURE(doc == t.bothDocA);  // which document's IR the host passed
        const std::vector<uint8_t> px = swRender(t.listBoth, t.assets.get(), &doc->ir());
        CHECK(columnsDiffer(px, refA, 0, 32) == 0);
        CHECK(columnsDiffer(px, refB, 32, 64) == 0);
    }
    // The call IR is not needed at all once the list names its sources.
    const std::vector<uint8_t> px = swRender(t.listBoth, t.assets.get(), nullptr);
    CHECK(columnsDiffer(px, refA, 0, 32) == 0);
    CHECK(columnsDiffer(px, refB, 32, 64) == 0);
}

TEST_CASE("gpu: T-263d two documents in one frame each run their own custom effect shader") {
    TwoShaderSurfaces t;
    REQUIRE(t.init("xt_t263d_two_shaders_gpu"));
    for (const Document* doc : {t.bothDocA, t.bothDocB}) {
        CAPTURE(doc == t.bothDocA);
        Built b;
        b.device.customPrograms = true;
        buildInto(b, t.listBoth, TwoSurfaces::kW, TwoSurfaces::kH, nullptr, t.assets.get(), &doc->ir());
        const std::vector<std::string> modules = customPassModules(b);
        REQUIRE(modules.size() == 2);
        CHECK(hasText(modules[0], "0.125"));
        CHECK(hasText(modules[1], "0.625"));
    }
    // A device without custom programs schedules each document's own fallback: a blur for the
    // first square, nothing for the second.
    Built both, a, b;
    buildInto(both, t.listBoth, TwoSurfaces::kW, TwoSurfaces::kH, nullptr, t.assets.get(), &t.bothDocB->ir());
    buildInto(a, t.listA, TwoSurfaces::kW, TwoSurfaces::kH, nullptr, t.assets.get(), &t.bothDocA->ir());
    buildInto(b, t.listB, TwoSurfaces::kW, TwoSurfaces::kH, nullptr, t.assets.get(), &t.bothDocB->ir());
    std::vector<EffectProgram> expected = effectSequence(a.frame);
    const std::vector<EffectProgram> second = effectSequence(b.frame);
    expected.insert(expected.end(), second.begin(), second.end());
    CHECK(effectSequence(both.frame) == expected);
    CHECK(effectSequence(a.frame).size() > effectSequence(b.frame).size());
}

// The texture cache keeps every source one build uses (Docs/tasks/T-263d.md): three documents of
// one Context, one image each, are uploaded once, not again on every frame.
TEST_CASE("gpu: T-263d three documents in one Context upload their image once over ten builds") {
    const std::filesystem::path dir = xt::test::tempDir() / "xt_t263d_three_documents";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const int sizes[3] = {8, 4, 2};
    std::vector<uint8_t> irs[3];
    for (int i = 0; i < 3; ++i) {
        std::vector<uint8_t> px(static_cast<size_t>(sizes[i]) * static_cast<size_t>(sizes[i]) * 4, 255);
        const std::string name = std::string(1, static_cast<char>('a' + i)) + ".png";
        REQUIRE(sw::writePng((dir / name).string(), px.data(), sizes[i], sizes[i], sizes[i] * 4));
        irs[i] = TwoSurfaces::compileDoc(dir, name.c_str(), 16 * i, /*embed=*/false);
        REQUIRE(!irs[i].empty());
    }
    {
        FileAssetResolver assets(dir.string(), sw::decodeImageBytes);
        NullTextShaper shaper;
        Context ctx(ContextConfig{&shaper, &assets});
        ctx.setViewport(Rect{0, 0, 64, 32}, 1.0f);
        const Document* first = nullptr;
        for (int i = 0; i < 3; ++i) {
            Result<DocumentHandle> h = ctx.loadDocument(IrBlob{irs[i].data(), irs[i].size()}, false);
            REQUIRE(h.ok());
            if (i == 0) first = ctx.document(h.value());
        }
        Built b;
        b.builder.init(b.device);
        b.builder.resize(64, 32);
        CommandList list;
        for (int frame = 0; frame < 10; ++frame) {
            CAPTURE(frame);
            ctx.update(frame == 0 ? 0.0f : 1.0f / 60.0f);
            ctx.render(list);
            REQUIRE(list.sources.size() == 3);
            b.builder.build(list, nullptr, &assets, &first->ir(), b.frame);
            CHECK(textureWidthOfQuadAt(b, 0.0f) == 8);
            CHECK(textureWidthOfQuadAt(b, 16.0f) == 4);
            CHECK(textureWidthOfQuadAt(b, 32.0f) == 2);
        }
        CHECK(b.device.createCalls == 3);
        CHECK(b.device.aliveCount() == 3);
        // A build of one other document afterwards keeps the two most recent sources only.
        std::vector<uint8_t> bytes = emptyIr();
        Result<IrView> other = IrView::open(IrBlob{bytes.data(), bytes.size()});
        REQUIRE(other.ok());
        CommandList plain = makeList();
        addFillRect(plain, 0, 0, 8, 8, Color{0, 0, 1, 1});
        b.builder.build(plain, nullptr, &assets, &other.value(), b.frame);
        CHECK(b.device.aliveCount() == 1);
        b.builder.shutdown();
        CHECK(b.device.aliveCount() == 0);
    }
    std::filesystem::remove_all(dir, ec);
}

// Spec points 3 and 9: a list a host fills by hand names no source; its ids are the call IR's.
TEST_CASE("sw/gpu: T-263d a hand-built list with empty sources resolves its assets against the call IR") {
    std::vector<uint8_t> bytes = emptyIr();
    Result<IrView> view = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(view.ok());
    PerAssetViewResolver assets;
    REQUIRE(assets.add(makeSolid(4, 4, 255)) == 0);
    CommandList cl = makeList(TwoSurfaces::kW, TwoSurfaces::kH);
    addDrawImage(cl, 0, Rect{0, 0, 16, 16}, 1.0f, true);
    REQUIRE(cl.sources.empty());
    CHECK(sourceIr(cl, 0, &view.value()) == &view.value());
    CHECK(sourceIr(cl, 0, nullptr) == nullptr);

    const std::vector<uint8_t> px = swRender(cl, &assets, &view.value());
    CHECK(px[(8 * TwoSurfaces::kW + 8) * 4 + 3] == 255);
    const std::vector<uint8_t> none = swRender(cl, &assets, nullptr);
    CHECK(none[(8 * TwoSurfaces::kW + 8) * 4 + 3] == 0);  // no IR, no image: as before

    Built b;
    buildInto(b, cl, TwoSurfaces::kW, TwoSurfaces::kH, nullptr, &assets, &view.value());
    CHECK(textureWidthOfQuadAt(b, 0.0f) == 4);
    CHECK(b.builder.lastFrameReusable(&assets, &view.value()));

    // The same list with a source table of one entry draws the same without a call IR...
    cl.sources.push_back(CommandSource{&view.value(), view.value().loadSerial()});
    CHECK(sourceIr(cl, 0, nullptr) == &view.value());
    CHECK(swRender(cl, &assets, nullptr) == px);
    b.builder.build(cl, nullptr, &assets, nullptr, b.frame);
    CHECK(textureWidthOfQuadAt(b, 0.0f) == 4);
    CHECK(b.device.createCalls == 1);  // the same (resolver, loadSerial, id): no second upload
    // ...and one whose command names a source the table does not have is rejected whole.
    cl.images[0].source = 1;
    CHECK_FALSE(validateCommandList(cl));
    CHECK(sourceIr(cl, 1, &view.value()) == nullptr);
    const std::vector<uint8_t> rejected = swRender(cl, &assets, &view.value());
    CHECK(rejected[(8 * TwoSurfaces::kW + 8) * 4 + 3] == 0);
    b.builder.build(cl, nullptr, &assets, &view.value(), b.frame);
    CHECK(b.frame.cmds.empty());
    CHECK(std::string(b.builder.lastError()) == "invalid command list; frame not drawn");
    b.builder.shutdown();
}

// =================================================================================================
// T-263d part two: GpuDevice::maxTextureSize and bounded retries of an upload that failed.
// =================================================================================================

TEST_CASE("gpu: T-263d an image over the device's texture limit is never uploaded and reported once") {
    std::vector<uint8_t> bytes = emptyIr();
    Result<IrView> view = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(view.ok());
    PerAssetViewResolver assets;
    const AssetId big = assets.add(makeSolid(128, 128, 255));  // past the device's 64 px limit
    const AssetId small = assets.add(makeSolid(4, 4, 255));
    CommandList cl = makeList();
    addDrawImage(cl, big, Rect{0, 0, 32, 32}, 1.0f, true);
    addDrawImage(cl, small, Rect{32, 0, 16, 16}, 1.0f, true);

    Built b;
    b.device.textureLimit = 64;
    buildInto(b, cl, 64, 64, nullptr, &assets, &view.value());
    // No createTexture call for the large image at all; the limit was asked once for the build.
    CHECK(b.device.createCalls == 1);
    CHECK(b.device.limitQueries == 1);
    REQUIRE(imageTextures(b).size() == 1);
    CHECK(imageTextures(b)[0]->w == 4);
    CHECK(std::string(b.builder.lastError()) == "image 128x128 exceeds the device texture limit 64");

    // Refused for good: no upload, and no error again, however many builds follow.
    for (int i = 0; i < 200; ++i) {
        b.builder.build(cl, nullptr, &assets, &view.value(), b.frame);
        REQUIRE(std::string(b.builder.lastError()).empty());
    }
    CHECK(b.device.createCalls == 1);
    CHECK(b.device.limitQueries == 201);
    CHECK(imageTextures(b).size() == 1);

    // ...until the host invalidates the asset (here the limit is gone too).
    b.device.textureLimit = 0;
    b.builder.invalidateAsset(big);
    b.builder.build(cl, nullptr, &assets, &view.value(), b.frame);
    CHECK(b.device.createCalls == 2);
    CHECK(imageTextures(b).size() == 2);
    b.builder.shutdown();
    CHECK(b.device.aliveCount() == 0);
}

TEST_CASE("gpu: T-263d a failed upload is tried again 60 builds later, three times at most") {
    std::vector<uint8_t> bytes = emptyIr();
    Result<IrView> view = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(view.ok());
    PerAssetViewResolver assets;
    REQUIRE(assets.add(makeSolid(4, 4, 255)) == 0);
    CommandList cl = makeList();
    addDrawImage(cl, 0, Rect{0, 0, 16, 16}, 1.0f, true);

    Built b;
    b.builder.init(b.device);
    b.builder.resize(64, 64);
    // The builds (1-based) in which createTexture was called, over `count` builds.
    auto attempts = [&](int first, int count) {
        std::vector<int> out;
        for (int build = first; build < first + count; ++build) {
            const int before = b.device.createCalls;
            b.builder.build(cl, nullptr, &assets, &view.value(), b.frame);
            if (b.device.createCalls != before) {
                out.push_back(build);
            }
        }
        return out;
    };

    SUBCASE("it fails twice: the texture appears on the third attempt, with no tries in between") {
        b.device.failCreates = 2;
        CHECK(attempts(1, 120) == std::vector<int>{1, 61});
        CHECK(imageTextures(b).empty());
        CHECK(!std::string(b.builder.lastError()).empty());  // still reported while it is missing
        CHECK(attempts(121, 1) == std::vector<int>{121});
        REQUIRE(imageTextures(b).size() == 1);
        CHECK(imageTextures(b)[0]->w == 4);
        CHECK(std::string(b.builder.lastError()).empty());
        CHECK(attempts(122, 200).empty());
        CHECK(b.device.createCalls == 3);
    }
    SUBCASE("it never succeeds: the first try and three more, then remembered until invalidation") {
        b.device.failCreates = 1000000;
        CHECK(attempts(1, 500) == std::vector<int>{1, 61, 121, 181});
        CHECK(imageTextures(b).empty());
        b.device.failCreates = 0;
        CHECK(attempts(501, 100).empty());  // the device recovered; nothing asks it
        b.builder.invalidateAllAssets();    // a device reset, as the host reports it
        CHECK(attempts(601, 1) == std::vector<int>{601});
        CHECK(imageTextures(b).size() == 1);
    }
    b.builder.shutdown();
}

// =================================================================================================
// T-263f: retries reach a static screen (lastFrameReusable counts the host's frames and asks for
// one build when a retry is due), and invalidateAsset names the IR whose id it means.
// =================================================================================================

TEST_CASE("gpu: T-263f a failed upload is retried on a static screen, at the same frames as when building") {
    std::vector<uint8_t> bytes = emptyIr();
    Result<IrView> view = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(view.ok());
    PerAssetViewResolver assets;
    REQUIRE(assets.add(makeSolid(4, 4, 255)) == 0);
    CommandList cl = makeList();
    addDrawImage(cl, 0, Rect{0, 0, 16, 16}, 1.0f, true);

    Built b;
    b.builder.init(b.device);
    b.builder.resize(64, 64);
    // A reusing host: every frame it asks first and builds only when told the last frame cannot
    // be submitted again. The list never changes. `refused` = the frames (1-based) in which
    // lastFrameReusable said no, `tried` = the frames in which createTexture was called.
    std::vector<int> refused, tried;
    auto reusingHost = [&](int first, int count) {
        for (int frame = first; frame < first + count; ++frame) {
            if (b.builder.lastFrameReusable(&assets, &view.value())) {
                continue;
            }
            refused.push_back(frame);
            const int before = b.device.createCalls;
            b.builder.build(cl, nullptr, &assets, &view.value(), b.frame);
            if (b.device.createCalls != before) {
                tried.push_back(frame);
            }
        }
    };

    SUBCASE("it fails once: the texture appears at frame 61 with no document change") {
        b.device.failCreates = 1;
        reusingHost(1, 60);
        CHECK(refused == std::vector<int>{1});
        CHECK(imageTextures(b).empty());
        reusingHost(61, 1);
        CHECK(refused == std::vector<int>{1, 61});
        CHECK(tried == std::vector<int>{1, 61});
        REQUIRE(imageTextures(b).size() == 1);
        CHECK(imageTextures(b)[0]->w == 4);
        reusingHost(62, 400);
        CHECK(refused == std::vector<int>{1, 61});  // nothing pending: reusable from then on
        CHECK(b.device.createCalls == 2);
    }
    SUBCASE("it never succeeds: no is answered once per attempt, and never after the last one") {
        b.device.failCreates = 1000000;
        reusingHost(1, 500);
        CHECK(refused == std::vector<int>{1, 61, 121, 181});
        CHECK(tried == std::vector<int>{1, 61, 121, 181});
        CHECK(imageTextures(b).empty());
    }
    SUBCASE("a host that asks and builds every frame counts each frame once") {
        b.device.failCreates = 1000000;
        for (int frame = 1; frame <= 500; ++frame) {
            (void)b.builder.lastFrameReusable(&assets, &view.value());
            const int before = b.device.createCalls;
            b.builder.build(cl, nullptr, &assets, &view.value(), b.frame);
            if (b.device.createCalls != before) {
                tried.push_back(frame);
            }
        }
        CHECK(tried == std::vector<int>{1, 61, 121, 181});
    }
    SUBCASE("a due retry is announced once even when the host does not build") {
        b.device.failCreates = 1000000;
        reusingHost(1, 60);
        CHECK(refused == std::vector<int>{1});
        CHECK_FALSE(b.builder.lastFrameReusable(&assets, &view.value()));  // frame 61: due
        for (int frame = 62; frame <= 200; ++frame) {
            CAPTURE(frame);
            REQUIRE(b.builder.lastFrameReusable(&assets, &view.value()));  // not asked for again
        }
        // The build the host finally makes carries out the attempt.
        const int before = b.device.createCalls;
        b.builder.build(cl, nullptr, &assets, &view.value(), b.frame);
        CHECK(b.device.createCalls == before + 1);
    }
    b.builder.shutdown();
}

TEST_CASE("gpu: T-263f invalidateAsset(id, source) drops the id under that IR only") {
    SUBCASE("two IRs built in turn") {
        std::vector<uint8_t> bytesA = emptyIr();
        std::vector<uint8_t> bytesB = emptyIr();
        Result<IrView> viewA = IrView::open(IrBlob{bytesA.data(), bytesA.size()});
        Result<IrView> viewB = IrView::open(IrBlob{bytesB.data(), bytesB.size()});
        REQUIRE(viewA.ok());
        REQUIRE(viewB.ok());
        // One resolver for both: asset 0 is 4 px wide in A and 2 px wide in B.
        struct PerIrResolver : PerAssetViewResolver {
            const IrView* a = nullptr;
            using PerAssetViewResolver::imageView;
            const ImageData* imageView(AssetId, const IrView& view) override {
                return PerAssetViewResolver::imageView(&view == a ? 0 : 1, view);
            }
        } assets;
        assets.a = &viewA.value();
        assets.add(makeSolid(4, 4, 255));
        assets.add(makeSolid(2, 2, 255));
        CommandList cl = makeList();
        addDrawImage(cl, 0, Rect{0, 0, 16, 16}, 1.0f, true);

        Built b;
        b.builder.init(b.device);
        b.builder.resize(64, 64);
        b.builder.build(cl, nullptr, &assets, &viewA.value(), b.frame);
        b.builder.build(cl, nullptr, &assets, &viewB.value(), b.frame);
        REQUIRE(b.device.createCalls == 2);
        REQUIRE(b.device.aliveCount() == 2);

        b.builder.invalidateAsset(0, &viewA.value());
        CHECK(b.device.aliveCount() == 1);  // B's asset 0 is still there
        CHECK_FALSE(b.builder.lastFrameReusable(&assets, &viewB.value()));
        b.builder.build(cl, nullptr, &assets, &viewB.value(), b.frame);
        CHECK(b.device.createCalls == 2);  // B uploads nothing again
        REQUIRE(imageTextures(b).size() == 1);
        CHECK(imageTextures(b)[0]->w == 2);
        b.builder.build(cl, nullptr, &assets, &viewA.value(), b.frame);
        CHECK(b.device.createCalls == 3);  // A's was dropped
        CHECK(b.device.aliveCount() == 2);

        // An IR this builder holds nothing of: nothing is dropped.
        std::vector<uint8_t> bytesC = emptyIr();
        Result<IrView> viewC = IrView::open(IrBlob{bytesC.data(), bytesC.size()});
        REQUIRE(viewC.ok());
        b.builder.invalidateAsset(0, &viewC.value());
        CHECK(b.device.aliveCount() == 2);

        // The id alone still means every source.
        b.builder.invalidateAsset(0);
        CHECK(b.device.aliveCount() == 0);
        b.builder.build(cl, nullptr, &assets, &viewA.value(), b.frame);
        b.builder.build(cl, nullptr, &assets, &viewB.value(), b.frame);
        CHECK(b.device.createCalls == 5);
        b.builder.shutdown();
        CHECK(b.device.aliveCount() == 0);
    }
    SUBCASE("two documents of one Context, in one frame") {
        TwoSurfaces t;
        REQUIRE(t.init("xt_t263f_invalidate_by_source"));
        Built b;
        buildInto(b, t.listBoth, TwoSurfaces::kW, TwoSurfaces::kH, nullptr, t.assets.get(),
                  &t.bothDocA->ir());
        REQUIRE(b.device.createCalls == 2);
        REQUIRE(textureWidthOfQuadAt(b, 0.0f) == 8);
        REQUIRE(textureWidthOfQuadAt(b, 32.0f) == 4);
        TextureHandle keptB = 0;
        for (const auto& [handle, record] : b.device.textures) {
            if (record.alive && record.w == 4) {
                keptB = handle;
            }
        }
        REQUIRE(keptB != 0);

        // Both documents call their image asset 0; only document A's is invalidated.
        b.builder.invalidateAsset(0, &t.bothDocA->ir());
        CHECK(b.device.aliveCount() == 1);
        CHECK(b.device.textures[keptB].alive);
        b.builder.build(t.listBoth, nullptr, t.assets.get(), &t.bothDocA->ir(), b.frame);
        CHECK(b.device.createCalls == 3);  // A's again; B's upload count is unchanged
        CHECK(textureWidthOfQuadAt(b, 0.0f) == 8);
        CHECK(textureWidthOfQuadAt(b, 32.0f) == 4);
        CHECK(b.device.textures[keptB].alive);
        b.builder.shutdown();
    }
}

// -------------------------------------------------------------------------------------------
// T-343 Direct target mode (Docs/design/23-host-integration.md §23.aa, Docs/tasks/T-343.md).
// -------------------------------------------------------------------------------------------

namespace {

/// A PushLayer that the Painter would mark as a scene reader (a blend mode).
void addScenePushLayer(CommandList& cl, BlendMode blend, Rect hint) {
    LayerCmd l{};
    l.opacity = 1.0f;
    l.blend = blend;
    l.boundsHint = hint;
    cl.layers.push_back(l);
    Command c{CmdKind::PushLayer, {}, static_cast<uint32_t>(cl.layers.size() - 1)};
    c.flags[0] = kCommandSceneRead;
    cl.commands.push_back(c);
}

/// A glass panel: PushEffect(backdrop-blur radius) marked as the Painter marks it, a translucent
/// fill, PopEffect.
void addGlassPanel(CommandList& cl, Rect box, float radius) {
    EffectCmd c{};
    c.effectOffset = static_cast<uint32_t>(cl.effects.size());
    c.effectCount = 1;
    cl.effects.push_back(backdropBlur(radius, 0.0f, 0.0f));
    c.boundsHint = box;
    c.contentBounds = box;
    cl.effectCmds.push_back(c);
    Command cmd{CmdKind::PushEffect, {}, static_cast<uint32_t>(cl.effectCmds.size() - 1)};
    cmd.flags[0] = kCommandSceneRead;
    cl.commands.push_back(cmd);
    addFillRect(cl, box.x, box.y, box.w, box.h, Color{1, 1, 1, 0.3f});
    addPopEffect(cl);
}

/// A HUD-shaped Direct list on a `w x h` surface: band 0 (two rects), a glass panel, band 1
/// (two rects), a multiply vignette (scene run), band 2 (one rect).
CommandList directHudList(int w, int h) {
    CommandList cl = makeList(w, h);
    cl.hostTargetMode = HostTargetMode::Direct;
    addFillRect(cl, 10, 10, 40, 20, Color{1, 0, 0, 1});
    addFillRect(cl, 10, 40, 40, 20, Color{0, 1, 0, 1});
    addGlassPanel(cl, Rect{100, 20, 60, 40}, 16.0f);
    addFillRect(cl, 100, 100, 50, 20, Color{0, 0, 1, 1});
    addFillRect(cl, 160, 100, 30, 20, Color{1, 1, 0, 1});
    addScenePushLayer(cl, BlendMode::Multiply, Rect{0, 0, static_cast<float>(w), static_cast<float>(h)});
    addFillRect(cl, 0, 0, static_cast<float>(w), static_cast<float>(h), Color{0.5f, 0.2f, 0.2f, 0.5f});
    addPopLayer(cl);
    addFillRect(cl, 200, 150, 40, 30, Color{0, 1, 1, 1});
    return cl;
}

size_t countProgram(const GpuFrame& f, EffectProgram p) {
    size_t n = 0;
    for (const DrawCmd& c : f.cmds) {
        n += (c.shading == Shading::Effect && c.program == p) ? 1 : 0;
    }
    return n;
}

size_t countCopiesFromSurface(const GpuFrame& f) {
    size_t n = 0;
    for (const Pass& p : f.passes) {
        n += (p.copy != 0 && p.copySource == kSurfaceTarget) ? 1 : 0;
    }
    return n;
}

struct DirectBuilt {
    FakeDevice device;
    FrameBuilder builder;
    GpuFrame frame;
    PerfTracker perf;
    DirectBuilt() { perf.setEnabled(true); }
    const FrameStats& build(const CommandList& cl, const DamageList* damage) {
        perf.beginFrame(0.0f);
        builder.build(cl, nullptr, nullptr, nullptr, frame, &perf, damage);
        checkFrameWellFormed(frame);
        return perf.current();
    }
};

DamageList noDamage() {
    DamageList d;
    d.full = false;
    return d;
}

}  // namespace

TEST_CASE("gpu: T-343 scanSceneRuns splits the paint order at depth-0 scene readers") {
    CommandList cl = directHudList(256, 192);
    std::vector<SceneRun> runs;
    scanSceneRuns(cl, runs);
    REQUIRE(runs.size() == 5);
    CHECK_FALSE(runs[0].sceneRead);
    CHECK(runs[0].first == 0);
    CHECK(runs[0].end == 2);
    CHECK(runs[1].sceneRead);
    CHECK(runs[1].first == 2);
    CHECK(runs[1].end == 5);  // PushEffect, fill, PopEffect
    CHECK_FALSE(runs[2].sceneRead);
    CHECK(runs[3].sceneRead);
    CHECK(runs[3].end == runs[3].first + 3);
    CHECK_FALSE(runs[4].sceneRead);
    CHECK(runs[4].end == cl.commands.size());

    // A marked push nested inside an unmarked layer does not split: depth 0 only.
    CommandList nested = makeList();
    addPushLayer(nested, 0.5f, BlendMode::Normal);
    addScenePushLayer(nested, BlendMode::Screen, Rect{0, 0, 64, 64});
    addFillRect(nested, 0, 0, 10, 10, Color{1, 0, 0, 1});
    addPopLayer(nested);
    addPopLayer(nested);
    scanSceneRuns(nested, runs);
    REQUIRE(runs.size() == 1);
    CHECK_FALSE(runs[0].sceneRead);
    // An unmarked blend layer is not a scene run either (the mark decides, not the payload).
    CommandList unmarked = makeList();
    addPushLayer(unmarked, 1.0f, BlendMode::Multiply);
    addFillRect(unmarked, 0, 0, 10, 10, Color{1, 0, 0, 1});
    addPopLayer(unmarked);
    scanSceneRuns(unmarked, runs);
    REQUIRE(runs.size() == 1);
    CHECK_FALSE(runs[0].sceneRead);
}

TEST_CASE("gpu: T-343 Direct mode draws bands into stored transients and scene runs onto the scene") {
    DirectBuilt b;
    b.builder.init(b.device);
    b.builder.resize(256, 192);
    CommandList cl = directHudList(256, 192);
    const FrameStats& st = b.build(cl, nullptr);
    const GpuFrame& f = b.frame;
    CHECK(st.bands == 3);
    CHECK(st.bandRedraws == 3);
    CHECK(st.directFallback == 0);
    CHECK(st.pyramidLevels == backdropPyramidLevels(8.0f));
    CHECK(st.backdropCopyPx > 0);
    // Three band stores, one per band; nothing is ever cleared on target 0.
    CHECK(f.cacheStores.size() == 3);
    for (const Pass& p : f.passes) {
        if (p.target == kSurfaceTarget) {
            CHECK(p.clear == 0);
        }
    }
    // Target 0 receives only quads and composites: no stencil op, no Solid fill.
    for (const Pass& p : f.passes) {
        if (p.target != kSurfaceTarget) continue;
        for (uint32_t c = p.firstCmd; c < p.firstCmd + p.cmdCount; ++c) {
            const DrawCmd& cmd = f.cmds[c];
            CHECK(cmd.stencilOp == StencilOp::None);
            CHECK((cmd.shading == Shading::Composite || cmd.shading == Shading::Texture));
        }
    }
    // The glass panel's pyramid: one copy from the scene, the downsample / Kawase chain, and the
    // panel's LevelMix; the vignette copies its own backdrop.
    CHECK(countCopiesFromSurface(f) == 2);
    CHECK(countProgram(f, EffectProgram::Downsample) == static_cast<size_t>(st.pyramidLevels));
    CHECK(countProgram(f, EffectProgram::Kawase) == static_cast<size_t>(st.pyramidLevels));
    CHECK(countProgram(f, EffectProgram::LevelMix) == 1);
    CHECK(countProgram(f, EffectProgram::GaussColor) == 0);
    // Every band transient has a stencil; the band targets came from the device.
    CHECK(b.device.cacheAcquires == 1 + 3);
    CHECK_FALSE(b.builder.lastFrameReusable(nullptr, nullptr));  // bands stored: paste next
    checkSnapshot("direct_bands", f);

    // Second build, nothing changed: bands pasted (Texture quads), only the scene runs drawn.
    const DamageList none = noDamage();
    const FrameStats& st2 = b.build(cl, &none);
    const GpuFrame& f2 = b.frame;
    CHECK(st2.bands == 3);
    CHECK(st2.bandRedraws == 0);
    CHECK(f2.cacheStores.empty());
    CHECK(cmdsWith(f2, Shading::Texture).size() == 3);
    CHECK(cmdsWith(f2, StencilOp::WriteIncrWrap).empty());
    size_t solidOnTransients = 0;
    for (const Pass& p : f2.passes) {
        for (uint32_t c = p.firstCmd; c < p.firstCmd + p.cmdCount; ++c) {
            solidOnTransients += f2.cmds[c].shading == Shading::Solid ? size_t{1} : size_t{0};
        }
    }
    CHECK(solidOnTransients == 2);  // the panel's fill and the vignette's fill only
    CHECK(b.device.cacheAcquires == 1 + 3);
    CHECK(b.builder.lastFrameReusable(nullptr, nullptr));
    checkSnapshot("direct_bands_reused", f2);

    // Damage inside band 1's window only: band 1 is redrawn, the others pasted.
    DamageList one = noDamage();
    one.rects.push_back(Rect{105, 102, 10, 10});
    const FrameStats& st3 = b.build(cl, &one);
    CHECK(st3.bandRedraws == 1);
    CHECK(b.frame.cacheStores.size() == 1);
    CHECK(cmdsWith(b.frame, Shading::Texture).size() == 2);
    CHECK(b.device.cacheAcquires == 1 + 3);  // the same-size target is overwritten

    // Full damage: every band again.
    DamageList full;
    const FrameStats& st4 = b.build(cl, &full);
    CHECK(st4.bandRedraws == 3);
    b.builder.shutdown();
    CHECK(b.device.cached.empty());
}

TEST_CASE("gpu: T-343 a band whose commands changed is redrawn even when the damage misses its old window") {
    DirectBuilt b;
    b.builder.init(b.device);
    b.builder.resize(256, 192);
    CommandList cl = directHudList(256, 192);
    b.build(cl, nullptr);
    b.build(cl, nullptr);
    // A new rect appears in band 2, far from its old window: its count changes.
    CommandList grown = directHudList(256, 192);
    addFillRect(grown, 20, 170, 20, 10, Color{1, 0, 1, 1});
    DamageList d = noDamage();
    d.rects.push_back(Rect{20, 170, 20, 10});
    const FrameStats& st = b.build(grown, &d);
    CHECK(st.bandRedraws == 1);
    b.builder.shutdown();
}

TEST_CASE("gpu: T-343 more than four bands, or too much band area, falls back to drawing everything") {
    SUBCASE("five bands") {
        DirectBuilt b;
        b.builder.init(b.device);
        b.builder.resize(256, 192);
        CommandList cl = makeList(256, 192);
        cl.hostTargetMode = HostTargetMode::Direct;
        for (int i = 0; i < 5; ++i) {
            addFillRect(cl, 10.0f + 40.0f * static_cast<float>(i), 10, 20, 20, Color{1, 0, 0, 1});
            addScenePushLayer(cl, BlendMode::Screen, Rect{10.0f + 40.0f * static_cast<float>(i), 40, 20, 20});
            addFillRect(cl, 10.0f + 40.0f * static_cast<float>(i), 40, 20, 20, Color{0, 1, 0, 0.5f});
            addPopLayer(cl);
        }
        addFillRect(cl, 10, 100, 20, 20, Color{0, 0, 1, 1});  // the sixth band
        const FrameStats& st = b.build(cl, nullptr);
        CHECK(st.bands == 6);
        CHECK(st.directFallback == 1);
        CHECK(b.frame.cacheStores.empty());
        CHECK(b.device.cacheAcquires == 1);                            // the probe only
        CHECK(cmdsWith(b.frame, Shading::Composite).size() == 6 + 5);  // bands pasted + scene layers
        CHECK(b.builder.lastFrameReusable(nullptr, nullptr));
        b.builder.shutdown();
    }
    SUBCASE("band area over twice the surface") {
        DirectBuilt b;
        b.builder.init(b.device);
        b.builder.resize(128, 128);
        CommandList cl = makeList(128, 128);
        cl.hostTargetMode = HostTargetMode::Direct;
        for (int i = 0; i < 3; ++i) {
            addFillRect(cl, 0, 0, 128, 128, Color{1, 0, 0, 1});  // a full-surface band
            addScenePushLayer(cl, BlendMode::Screen, Rect{0, 0, 10, 10});
            addFillRect(cl, 0, 0, 10, 10, Color{0, 1, 0, 0.5f});
            addPopLayer(cl);
        }
        const FrameStats& st = b.build(cl, nullptr);
        CHECK(st.bands == 3);
        CHECK(st.directFallback == 1);
        CHECK(b.frame.cacheStores.empty());
        b.builder.shutdown();
    }
}

TEST_CASE("gpu: T-343 overlapping glass panels share one pyramid; a disjoint one gets its own") {
    DirectBuilt b;
    b.builder.init(b.device);
    b.builder.resize(512, 256);
    CommandList cl = makeList(512, 256);
    cl.hostTargetMode = HostTargetMode::Direct;
    addGlassPanel(cl, Rect{20, 20, 80, 60}, 8.0f);  // sigma 4
    addFillRect(cl, 200, 200, 10, 10, Color{1, 0, 0, 1});
    addGlassPanel(cl, Rect{60, 40, 80, 60}, 24.0f);    // sigma 12, overlaps the first
    addGlassPanel(cl, Rect{400, 150, 60, 60}, 16.0f);  // sigma 8, far away
    const FrameStats& st = b.build(cl, nullptr);
    const GpuFrame& f = b.frame;
    // Two merged regions: two copies from the scene, two pyramids (3 + 2 levels), three panels.
    CHECK(countCopiesFromSurface(f) == 2);
    CHECK(st.pyramidLevels ==
          static_cast<uint32_t>(backdropPyramidLevels(12.0f) + backdropPyramidLevels(8.0f)));
    CHECK(countProgram(f, EffectProgram::LevelMix) == 3);
    CHECK(countProgram(f, EffectProgram::Downsample) == static_cast<size_t>(st.pyramidLevels));
    // The merged region's copy covers both panels' read rectangles.
    const Pass* copyPass = nullptr;
    for (const Pass& p : f.passes) {
        if (p.copy != 0 && p.copySource == kSurfaceTarget) {
            copyPass = &p;
            break;
        }
    }
    REQUIRE(copyPass != nullptr);
    CHECK(copyPass->viewX <= 20 - 12);
    CHECK(copyPass->viewX + copyPass->viewW >= 140 + 36);
    CHECK(st.backdropCopyPx == static_cast<uint32_t>(copyPass->viewW * copyPass->viewH) + [&] {
        uint32_t px = 0;
        int seen = 0;
        for (const Pass& p : f.passes) {
            if (p.copy != 0 && p.copySource == kSurfaceTarget && seen++ == 1) {
                px = static_cast<uint32_t>(p.viewW * p.viewH);
            }
        }
        return px;
    }());
    checkSnapshot("direct_pyramid_shared", f);
    b.builder.shutdown();
}

TEST_CASE("gpu: T-343 setSceneStatic caches a top-level glass panel's result") {
    DirectBuilt b;
    b.builder.init(b.device);
    b.builder.resize(256, 192);
    CommandList cl = directHudList(256, 192);
    cl.sceneStatic = true;
    b.build(cl, nullptr);  // first appearance
    const DamageList none = noDamage();
    b.build(cl, &none);  // stored (bands pasted, panel stored)
    CHECK(countProgram(b.frame, EffectProgram::Backdrop) == 1);
    CHECK(b.frame.cacheStores.size() == 1);  // the panel's result
    CHECK_FALSE(b.builder.lastFrameReusable(nullptr, nullptr));
    const FrameStats& st = b.build(cl, &none);  // served from the cache: pure quads
    CHECK(countProgram(b.frame, EffectProgram::Backdrop) == 0);
    CHECK(countProgram(b.frame, EffectProgram::LevelMix) == 0);
    CHECK(countCopiesFromSurface(b.frame) == 1);  // the vignette's backdrop only
    CHECK(st.pyramidLevels == 0);
    CHECK(b.frame.cacheStores.empty());
    CHECK(cmdsWith(b.frame, Shading::Texture).size() == 4);  // 3 bands + the panel
    CHECK(b.builder.lastFrameReusable(nullptr, nullptr));
    checkSnapshot("direct_scene_static", b.frame);
    // The promise withdrawn: the panel is drawn again and its entry released.
    cl.sceneStatic = false;
    b.build(cl, &none);
    CHECK(countProgram(b.frame, EffectProgram::Backdrop) == 1);
    b.build(cl, &none);
    CHECK(b.device.cacheReleases >= 1);
    b.builder.shutdown();
}

TEST_CASE("gpu: T-343 a device that cannot read target 0 draws a Direct list in Layer mode, once reported") {
    DirectBuilt b;
    b.device.direct = false;
    b.builder.init(b.device);
    b.builder.resize(256, 192);
    CommandList cl = directHudList(256, 192);
    const FrameStats& st = b.build(cl, nullptr);
    CHECK(std::string(b.builder.lastError()) ==
          "direct target mode is not supported by the device; drawing in Layer mode");
    CHECK(st.bands == 0);
    CHECK(b.frame.cacheStores.empty());
    CHECK_FALSE(cmdsWith(b.frame, StencilOp::TestEqualRef).empty());  // fills on target 0 as usual
    b.build(cl, nullptr);
    CHECK(b.builder.lastError()[0] == '\0');
    b.builder.shutdown();
}

TEST_CASE("gpu: T-343 a blur above sigma 4 runs downsampled (Downsample, Gauss at the level, LevelMix)") {
    using P = EffectProgram;
    SUBCASE("sigma 6: half resolution") {
        Built b;
        CommandList cl = makeList(128, 128);
        addPushEffect(cl, {blurFx(12, 12)}, Mat2x3{}, Rect{0, 0, 128, 128}, Rect{32, 32, 64, 64});
        addFillRect(cl, 32, 32, 64, 64, Color{1, 0, 0, 1});
        addPopEffect(cl);
        buildInto(b, cl, 128, 128);
        CHECK(effectSequence(b.frame) ==
              std::vector<P>{P::Downsample, P::GaussColor, P::GaussColor, P::LevelMix});
        // The level transient is half the window.
        bool half = false;
        for (const TransientTargetDesc& t : b.frame.transients) {
            half = half || (t.width == 64 && t.height == 64);
        }
        CHECK(half);
        auto fx = cmdsWith(b.frame, Shading::Effect);
        GaussParams gp{};
        std::memcpy(&gp, b.frame.params.data() + fx[1]->paramIndex, sizeof(gp));
        CHECK(gp.texelW == doctest::Approx(1.0f / 64.0f));
        checkSnapshot("effect_blur_downsampled", b.frame);
    }
    SUBCASE("sigma 20: eighth resolution, a glow's mask too") {
        Built b;
        CommandList cl = makeList(256, 256);
        EffectInstance g{};
        g.kind = EffectKind::Glow;
        g.params[0] = 40.0f;  // radius 40: sigma 20
        g.params[1] = 1.0f;
        g.colors[0] = Color{1, 1, 0, 1};
        addPushEffect(cl, {g}, Mat2x3{}, Rect{0, 0, 256, 256}, Rect{64, 64, 128, 128});
        addFillRect(cl, 64, 64, 128, 128, Color{1, 0, 0, 1});
        addPopEffect(cl);
        buildInto(b, cl, 256, 256);
        CHECK(countProgram(b.frame, P::Downsample) == 3);
        CHECK(countProgram(b.frame, P::LevelMix) == 1);
        CHECK(countProgram(b.frame, P::GaussScalar) == 2);
    }
    SUBCASE("sigma 4 stays at full resolution") {
        Built b;
        CommandList cl = blurredRectList(8.0f);
        buildInto(b, cl, 64, 64);
        CHECK(countProgram(b.frame, P::Downsample) == 0);
        CHECK(effectSequence(b.frame) == std::vector<P>{P::GaussColor, P::GaussColor});
    }
}
