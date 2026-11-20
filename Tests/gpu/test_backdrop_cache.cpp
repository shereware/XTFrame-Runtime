// T-730 (Docs/design/10-backends.md §10.18 "B 定案" second step, Docs/tasks/T-730.md "測試"): the
// source layer S and the shadow result R' of a stack of BackdropBlur followed only by shadow-type
// effects go into the effect result cache as one pair, on the frame builder alone, against a device that
// hands out handles and reports supportsBackdropMask(). The pixels are checked in
// Tests/wgpu/test_wgpu_backdrop_cache.cpp and Tests/gl/test_gl_backdrop_cache.cpp.
//
// golden/backdrop_cache_legacy.txt was written by the build before T-730: what a device without the
// capability, a device that does not cache, and the stacks the pair does not cover (a plain
// BackdropBlur, a BackdropBlur followed by another kind of effect) get over three builds.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "doctest.h"
#include "xt/command_list.h"
#include "xt/gpu/frame_builder.h"
#include "xt/gpu/gpu_device.h"
#include "xt/gpu/gpu_frame.h"

using namespace xt;
using namespace xt::gpu;

namespace {

constexpr int kW = 320, kH = 200;

/// Hands out texture handles; `cache` hands out cached targets too, `mask` reports the backdrop mask.
/// Executes nothing.
class FakeDevice : public GpuDevice {
public:
    FakeDevice(bool cache, bool mask, bool textures = false)
        : m_cache(cache), m_mask(mask), m_textures(textures) {}
    bool supportsCoverageDraws() const override { return true; }
    bool supportsBoxShadowShading() const override { return true; }
    bool supportsAnalyticClip() const override { return true; }
    bool supportsAlphaChannelReads() const override { return true; }
    bool supportsDirectTarget() const override { return true; }
    bool supportsBackdropMask() const override { return m_mask; }
    bool supportsBackdropMaskTextures() const override { return m_textures; }  // T-764
    TextureHandle createTexture(const char*, uint16_t, uint16_t, TextureFormat, const void*) override {
        return m_next++;
    }
    void updateTexture(TextureHandle, uint16_t, uint16_t, uint16_t, uint16_t, const void*, size_t) override {}
    void destroyTexture(TextureHandle) override {}
    void submit(const GpuFrame&) override {}
    TextureHandle acquireCachedTarget(uint16_t, uint16_t, TextureFormat) override {
        if (!m_cache) {
            return 0;
        }
        ++cachedAlive;
        return m_next++;
    }
    void releaseCachedTarget(TextureHandle t) override { cachedAlive -= t != 0 ? 1 : 0; }

    int cachedAlive = 0;

private:
    bool m_cache;
    bool m_mask;
    bool m_textures;
    TextureHandle m_next = 1;
};

EffectInstance fx(EffectKind kind, std::initializer_list<float> params, Color color = Color{0, 0, 0, 1}) {
    EffectInstance e{};
    e.kind = kind;
    int i = 0;
    for (float p : params) {
        e.params[i++] = p;
    }
    e.colors[0] = color;
    e.enabled = 1;
    return e;
}

EffectInstance blurFx(float radius = 12.0f) { return fx(EffectKind::BackdropBlur, {radius, 0.3f, 0.1f}); }
EffectInstance outerFx(float blur = 12.0f) {
    return fx(EffectKind::DropShadow, {0, 6, blur, 0, 1, 0}, Color{0, 0, 0, 0.5f});
}
EffectInstance insetFx() { return fx(EffectKind::DropShadow, {0, 2, 8, 0, 1, 1}, Color{0, 0, 0, 0.4f}); }
EffectInstance adjustFx() { return fx(EffectKind::ColorAdjust, {0.1f, 0, -0.3f, 0, 1}); }

PathRef rectPath(CommandList& cl, Rect r) {
    PathRef ref{};
    ref.verbOffset = static_cast<uint32_t>(cl.verbs.size());
    ref.pointOffset = static_cast<uint32_t>(cl.points.size());
    const float pts[8] = {r.x, r.y, r.x + r.w, r.y, r.x + r.w, r.y + r.h, r.x, r.y + r.h};
    for (int i = 0; i < 4; ++i) {
        cl.verbs.push_back(i == 0 ? PathVerb::Move : PathVerb::Line);
        cl.points.push_back(pts[i * 2]);
        cl.points.push_back(pts[i * 2 + 1]);
    }
    cl.verbs.push_back(PathVerb::Close);
    ref.verbCount = static_cast<uint32_t>(cl.verbs.size()) - ref.verbOffset;
    ref.pointCount = static_cast<uint32_t>(cl.points.size()) - ref.pointOffset;
    return ref;
}

void fillRect(CommandList& cl, Rect r, Color color, Mat2x3 xf = Mat2x3{}) {
    FillCmd f{};
    f.xf = xf;
    f.path = rectPath(cl, r);
    f.paint.kind = PaintKind::Solid;
    f.paint.color = color;
    f.rule = FillRule::NonZero;
    f.antialias = 1;
    cl.fills.push_back(f);
    cl.commands.push_back(Command{CmdKind::FillPath, {}, static_cast<uint32_t>(cl.fills.size() - 1)});
}

/// What a scene holds: the panel's effects, where its effect bounds start (the anchor), the stripes'
/// offset (what lies below), the content's colour.
struct Spec {
    std::vector<EffectInstance> effects{blurFx(), outerFx(), insetFx()};
    Vec2 at{40, 30};
    float stripes = 0;
    Color content{1, 1, 1, 0.3f};
    Vec2 size{140, 90};  ///< the panel's side (its effect bounds are 30 px wider each way)
    int surfaceW = kW, surfaceH = kH;
};

constexpr float kMargin = 30.0f;

/// Stripes, then a panel laid out as the Painter does (its world transform on the fills and the effect,
/// its bounds on the screen).
CommandList scene(const Spec& s) {
    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(s.surfaceW), static_cast<float>(s.surfaceH)};
    cl.devicePixelRatio = 1;
    for (int i = 0; i < 12; ++i) {
        fillRect(cl, Rect{s.stripes + static_cast<float>(i) * 32.0f, 0, 32, static_cast<float>(s.surfaceH)},
                 Color{0.08f * static_cast<float>(i), 0.4f, 0.9f - 0.06f * static_cast<float>(i), 1});
    }
    const Mat2x3 world = translation(Vec2{s.at.x + kMargin, s.at.y + kMargin});
    EffectCmd e{};
    e.effectOffset = static_cast<uint32_t>(cl.effects.size());
    for (const EffectInstance& f : s.effects) {
        cl.effects.push_back(f);
    }
    e.effectCount = static_cast<uint32_t>(s.effects.size());
    e.xf = world;
    e.boundsHint = Rect{s.at.x, s.at.y, s.size.x + 2 * kMargin, s.size.y + 2 * kMargin};
    e.contentBounds = Rect{s.at.x + kMargin, s.at.y + kMargin, s.size.x, s.size.y};
    cl.effectCmds.push_back(e);
    cl.commands.push_back(Command{CmdKind::PushEffect, {}, static_cast<uint32_t>(cl.effectCmds.size() - 1)});
    fillRect(cl, Rect{0, 0, s.size.x, s.size.y}, s.content, world);
    fillRect(cl, Rect{10, 10, s.size.x * 0.5f, s.size.y * 0.5f}, Color{0.2f, 0.9f, 0.6f, 0.6f}, world);
    cl.commands.push_back(Command{CmdKind::PopEffect, {}, 0});
    return cl;
}

uint64_t fnv(const void* data, size_t bytes) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < bytes; ++i) {
        h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
}

/// Everything a device receives: the structural dump, every parameter float's bits, vertex and index
/// hashes, the cache stores.
std::string fingerprint(const GpuFrame& frame) {
    std::string out;
    dumpGpuFrame(frame, out);
    char buf[96];
    for (size_t i = 0; i < frame.params.size(); ++i) {
        uint32_t bits = 0;
        std::memcpy(&bits, &frame.params[i], sizeof(bits));
        std::snprintf(buf, sizeof(buf), "%s%08x", i % 8 == 0 ? "\nparams " : " ", bits);
        out += buf;
    }
    std::snprintf(
        buf, sizeof(buf), "\nvertices %zu %016llx\n", frame.vertices.size(),
        static_cast<unsigned long long>(fnv(frame.vertices.data(), frame.vertices.size() * sizeof(Vertex))));
    out += buf;
    std::snprintf(buf, sizeof(buf), "indices %zu %016llx\n", frame.indices.size(),
                  static_cast<unsigned long long>(fnv(frame.indices.data(), frame.indices.size() * 4)));
    out += buf;
    for (const CacheStore& s : frame.cacheStores) {
        std::snprintf(buf, sizeof(buf), "store pass %u src %u texture %u\n", s.pass, s.src, s.texture);
        out += buf;
    }
    return out;
}

struct Built {
    Built(bool cache, bool mask, int w = kW, int h = kH, bool textures = false)
        : device(cache, mask, textures) {
        builder.init(device);
        builder.resize(w, h);
        builder.setEdgeFringe(true);
    }
    ~Built() { builder.shutdown(); }
    Built(const Built&) = delete;
    Built& operator=(const Built&) = delete;
    void build(const CommandList& cl) { builder.build(cl, nullptr, nullptr, nullptr, frame); }
    FakeDevice device;
    FrameBuilder builder;
    GpuFrame frame;
};

int lastWriter(const GpuFrame& f, TargetHandle target, int before) {
    for (int q = before - 1; q >= 0; --q) {
        if (f.passes[static_cast<size_t>(q)].target == target) {
            return q;
        }
    }
    return -1;
}

/// What one frame does with the panel.
struct Shape {
    size_t passes = 0;
    uint32_t shadowPasses = 0;  ///< ShadowGlow, Gauss*, Dilate, AlphaExtract: the shadow chains
    uint32_t masked = 0;        ///< BackdropMasked commands
    uint32_t levelMix = 0;
    uint32_t copies = 0;
    /// Every BackdropMasked's top (src) and mask were last written by one Texture quad sampling a
    /// texture the frame does not create (a cached result) -- the pair served from the cache.
    bool pastedInputs = false;
    std::set<TextureHandle> pastedTextures;
};

Shape shapeOf(const GpuFrame& f) {
    Shape s;
    s.passes = f.passes.size();
    bool pasted = true;
    bool anyMasked = false;
    for (size_t p = 0; p < f.passes.size(); ++p) {
        const Pass& pass = f.passes[p];
        s.copies += pass.copy != 0 ? 1u : 0u;
        for (uint32_t i = pass.firstCmd; i < pass.firstCmd + pass.cmdCount; ++i) {
            const DrawCmd& c = f.cmds[i];
            if (c.shading != Shading::Effect) {
                continue;
            }
            switch (c.program) {
                case EffectProgram::ShadowGlow:
                case EffectProgram::GaussScalar:
                case EffectProgram::GaussColor:
                case EffectProgram::Dilate:
                case EffectProgram::AlphaExtract:
                    ++s.shadowPasses;
                    break;
                case EffectProgram::LevelMix:
                    ++s.levelMix;
                    break;
                default:
                    break;
            }
            if (c.program != EffectProgram::BackdropMasked) {
                continue;
            }
            ++s.masked;
            anyMasked = true;
            for (TargetHandle input : {c.src, backdropMaskTarget(f, c)}) {
                const int w = lastWriter(f, input, static_cast<int>(p));
                if (w < 0 || f.passes[static_cast<size_t>(w)].cmdCount != 1) {
                    pasted = false;
                    continue;
                }
                const DrawCmd& q = f.cmds[f.passes[static_cast<size_t>(w)].firstCmd];
                pasted =
                    pasted && q.shading == Shading::Texture && f.passes[static_cast<size_t>(w)].clear != 0;
                s.pastedTextures.insert(q.texture);
            }
        }
    }
    s.pastedInputs = anyMasked && pasted && s.pastedTextures.size() == 2;
    return s;
}

bool updateSnapshots() {
#ifdef _MSC_VER
    char* v = nullptr;
    size_t len = 0;
    if (_dupenv_s(&v, &len, "XT_GPU_GOLDEN_UPDATE") != 0 || v == nullptr) {
        return false;
    }
    const bool on = v[0] != '\0' && v[0] != '0';
    std::free(v);
    return on;
#else
    const char* v = std::getenv("XT_GPU_GOLDEN_UPDATE");
    return v != nullptr && v[0] != '\0' && v[0] != '0';
#endif
}

void checkSnapshot(const char* name, const std::string& actual) {
    const std::filesystem::path path = std::filesystem::path(XT_GPU_GOLDEN_DIR) / name;
    INFO("snapshot: ", path.string());
    if (updateSnapshots()) {
        std::ofstream out(path, std::ios::binary);
        REQUIRE(out.good());
        out << actual;
        MESSAGE("wrote snapshot ", path.string());
        return;
    }
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    std::string expected = ss.str();
    expected.erase(std::remove(expected.begin(), expected.end(), '\r'), expected.end());
    if (expected != actual) {
        const std::filesystem::path dir(XT_GPU_ACTUAL_DIR);
        std::filesystem::create_directories(dir);
        std::ofstream out(dir / (std::string(name) + ".actual.txt"), std::ios::binary);
        out << actual;
    }
    CHECK(expected == actual);
}

/// Four builds of a panel that stands still over stripes that move (frames 0..3), each fingerprinted.
std::string fourBuilds(bool cache, bool mask, std::vector<EffectInstance> effects) {
    Built b(cache, mask);
    std::string out;
    for (int i = 0; i < 4; ++i) {
        Spec s;
        s.effects = effects;
        s.size = Vec2{180, 120};  // the snapshot's panel (it reaches the surface's bottom edge)
        s.stripes = static_cast<float>(i % 2) * 3.0f;
        b.build(scene(s));
        out += "-- build " + std::to_string(i) + "\n" + fingerprint(b.frame);
    }
    return out;
}

/// T-764: a panel over eleven builds -- at rest, under a backdrop that moves, a whole number of pixels
/// away, by fractions of a pixel and settling, then cut by the surface edge -- each fingerprinted. On a
/// device with `textures` the hits sample the cached pair directly; without, they paste it.
std::string pairBuilds(bool textures) {
    Built b(true, true, kW, kH, textures);
    std::string out;
    const Vec2 at[] = {{40, 30},     {40, 30},       {40, 30},       {40, 30},       {41, 30}, {43, 31},
                       {43.25f, 31}, {43.5f, 31.5f}, {43.5f, 31.5f}, {43.5f, 31.5f}, {-50, 30}};
    const float stripes[] = {0, 0, 0, 3, 3, 3, 3, 3, 3, 6, 6};
    for (size_t i = 0; i < sizeof(at) / sizeof(at[0]); ++i) {
        Spec s;
        s.at = at[i];
        s.stripes = stripes[i];
        b.build(scene(s));
        out += "-- build " + std::to_string(i) + "\n" + fingerprint(b.frame);
    }
    return out;
}

}  // namespace

TEST_CASE(
    "gpu backdrop cache: a device without the texture capability gets the frames of before, the pair "
    "pasted (T-764)") {
    // golden/backdrop_cache_paste.txt was written by the build before T-764.
    checkSnapshot("backdrop_cache_paste.txt", pairBuilds(false));
}

TEST_CASE(
    "gpu backdrop cache: without the capability, without a cache, and for the stacks it does not cover, "
    "the frames of before (T-730)") {
    std::string all;
    struct Case {
        const char* name;
        bool cache, mask;
        std::vector<EffectInstance> effects;
    };
    const Case cases[] = {
        {"cache, no mask: blur outer inset", true, false, {blurFx(), outerFx(), insetFx()}},
        {"mask, no cache: blur outer inset", false, true, {blurFx(), outerFx(), insetFx()}},
        {"cache and mask: plain blur", true, true, {blurFx()}},
        {"cache and mask: blur adjust", true, true, {blurFx(), adjustFx()}},
        {"cache and mask: blur outer adjust", true, true, {blurFx(), outerFx(), adjustFx()}},
        {"cache and mask: outer then blur", true, true, {outerFx(), blurFx()}},
    };
    for (const Case& c : cases) {
        all += "== ";
        all += c.name;
        all += "\n";
        all += fourBuilds(c.cache, c.mask, c.effects);
    }
    checkSnapshot("backdrop_cache_legacy.txt", all);
}

TEST_CASE("gpu backdrop cache: a panel at rest is served from the pair from the third build (T-730)") {
    Built b(true, true);
    Spec s;
    b.build(scene(s));
    const Shape first = shapeOf(b.frame);
    CHECK(first.masked == 1u);
    CHECK(first.shadowPasses > 0u);
    CHECK(b.frame.cacheStores.empty());
    CHECK(b.builder.lastBuildStats().backdropPairMisses == 1u);
    CHECK(b.builder.lastEffectCacheStats().misses == 0u);  // pairs are counted apart
    b.build(scene(s));
    // The second build stores both S and R'.
    CHECK(b.frame.cacheStores.size() == 2u);
    CHECK(b.builder.lastBuildStats().backdropPairStores == 1u);
    CHECK(b.builder.lastBuildStats().effectCacheEntries == 2u);
    CHECK(b.device.cachedAlive == 2);
    CHECK(shapeOf(b.frame).passes == first.passes);
    for (int i = 0; i < 2; ++i) {
        // Only what lies below moves: the backdrop is read again, the pair is pasted.
        s.stripes = static_cast<float>(i + 1) * 3.0f;
        b.build(scene(s));
        const Shape hit = shapeOf(b.frame);
        CHECK(hit.masked == 1u);
        CHECK(hit.shadowPasses == 0u);
        CHECK(hit.levelMix == 1u);
        CHECK(hit.copies == first.copies);
        CHECK(hit.pastedInputs);
        CHECK(b.frame.cacheStores.empty());
        CHECK(hit.passes < first.passes);
        CHECK(b.builder.lastBuildStats().backdropPairHits == 1u);
        CHECK(b.builder.lastBuildStats().backdropPairMisses == 0u);
        CHECK(b.builder.lastEffectCacheStats().hits == 0u);
        MESSAGE("passes: in full ", first.passes, ", from the pair ", hit.passes);
    }
}

TEST_CASE("gpu backdrop cache: another content or shadow misses, another backdrop blur still hits (T-730)") {
    Built b(true, true);
    Spec s;
    b.build(scene(s));
    b.build(scene(s));
    b.build(scene(s));
    CHECK(shapeOf(b.frame).pastedInputs);
    // The content changes: drawn in full (and stored again two builds later).
    Spec content = s;
    content.content = Color{1, 0.9f, 0.9f, 0.35f};
    b.build(scene(content));
    CHECK_FALSE(shapeOf(b.frame).pastedInputs);
    CHECK(shapeOf(b.frame).shadowPasses > 0u);
    // A shadow parameter changes: drawn in full.
    Spec shadow = s;
    shadow.effects = {blurFx(), outerFx(16.0f), insetFx()};
    b.build(scene(shadow));
    CHECK_FALSE(shapeOf(b.frame).pastedInputs);
    b.build(scene(shadow));
    b.build(scene(shadow));
    CHECK(shapeOf(b.frame).pastedInputs);
    // The backdrop blur's radius is not part of the pair's key.
    Spec radius = shadow;
    radius.effects = {blurFx(20.0f), outerFx(16.0f), insetFx()};
    b.build(scene(radius));
    CHECK(shapeOf(b.frame).pastedInputs);
}

TEST_CASE(
    "gpu backdrop cache: a panel moving by whole pixels hits, by fractions hits a phase and settles "
    "(T-730)") {
    Built b(true, true);
    Spec s;
    for (int i = 0; i < 5; ++i) {
        CAPTURE(i);
        s.at = Vec2{40.0f + static_cast<float>(i), 30.0f + static_cast<float>(i / 2)};
        b.build(scene(s));
        if (i >= 2) {
            CHECK(shapeOf(b.frame).pastedInputs);
            CHECK(shapeOf(b.frame).shadowPasses == 0u);
        }
    }
    Built f(true, true);
    const float xs[] = {40.0f, 40.25f, 40.5f, 40.75f, 41.125f};
    int frame = 0;
    for (float x : xs) {
        CAPTURE(frame);
        s.at = Vec2{x, 30.0f};
        f.build(scene(s));
        if (frame >= 2) {
            CHECK(shapeOf(f.frame).pastedInputs);
        }
        ++frame;
    }
    // It stops: one more build in full (the settle), then the pair again.
    f.build(scene(s));
    CHECK_FALSE(shapeOf(f.frame).pastedInputs);
    CHECK(f.frame.cacheStores.size() == 2u);
    f.build(scene(s));
    CHECK(shapeOf(f.frame).pastedInputs);
}

TEST_CASE("gpu backdrop cache: a panel cut by the surface edge hits with its absolute key (T-730)") {
    Built b(true, true);
    Spec s;
    s.at = Vec2{-50, 30};  // the left column of a layout, flush with the surface edge
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        s.stripes = static_cast<float>(i) * 2.0f;
        b.build(scene(s));
        if (i >= 2) {
            CHECK(shapeOf(b.frame).pastedInputs);
        }
    }
}

TEST_CASE("gpu backdrop cache: a pair over the budget is not stored and is drawn as usual (T-730)") {
    // Two 2960 x 2960 results (35 MB each) exceed the 64 MiB budget; one alone would fit.
    constexpr int kBig = 3000;
    Built b(true, true, kBig, kBig);
    Spec s;
    s.surfaceW = s.surfaceH = kBig;
    s.at = Vec2{0, 0};
    s.size = Vec2{2900, 2900};
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        b.build(scene(s));
        CHECK(b.frame.cacheStores.empty());
        CHECK(shapeOf(b.frame).masked == 1u);
        CHECK(shapeOf(b.frame).shadowPasses > 0u);
        CHECK(b.builder.lastBuildStats().backdropPairStores == 0u);
        CHECK(b.builder.lastBuildStats().backdropPairMisses == 1u);
    }
    CHECK(b.device.cachedAlive == 0);
}

namespace {

/// T-764: the BackdropMasked commands of a frame and their parsed inputs.
std::vector<std::pair<DrawCmd, BackdropInputs>> maskedOf(const GpuFrame& f) {
    std::vector<std::pair<DrawCmd, BackdropInputs>> out;
    for (const DrawCmd& c : f.cmds) {
        BackdropInputs in;
        if (backdropInputs(f, c, in)) {
            out.emplace_back(c, in);
        }
    }
    return out;
}

size_t textureQuads(const GpuFrame& f) {
    size_t n = 0;
    for (const DrawCmd& c : f.cmds) {
        n += c.shading == Shading::Texture ? 1u : 0u;
    }
    return n;
}

/// The same builds on a device with and one without the texture capability.
struct TwoBuilds {
    Built paste{true, true, kW, kH, false};
    Built direct{true, true, kW, kH, true};
    void build(const CommandList& cl) {
        paste.build(cl);
        direct.build(cl);
    }
};

/// Two glass panels side by side (each its own pair).
CommandList twoPanels(float stripes) {
    Spec a;
    a.stripes = stripes;
    a.size = Vec2{80, 60};
    CommandList cl = scene(a);
    Spec b = a;
    b.at = Vec2{180, 40};
    const CommandList second = scene(b);
    // The second list's panel (its PushEffect .. PopEffect, after its 12 stripes) appended to the first.
    const uint32_t verbBase = static_cast<uint32_t>(cl.verbs.size());
    const uint32_t pointBase = static_cast<uint32_t>(cl.points.size());
    const uint32_t fillBase = static_cast<uint32_t>(cl.fills.size());
    const uint32_t effectBase = static_cast<uint32_t>(cl.effects.size());
    const uint32_t effectCmdBase = static_cast<uint32_t>(cl.effectCmds.size());
    cl.verbs.insert(cl.verbs.end(), second.verbs.begin(), second.verbs.end());
    cl.points.insert(cl.points.end(), second.points.begin(), second.points.end());
    for (FillCmd fc : second.fills) {
        fc.path.verbOffset += verbBase;
        fc.path.pointOffset += pointBase;
        cl.fills.push_back(fc);
    }
    cl.effects.insert(cl.effects.end(), second.effects.begin(), second.effects.end());
    for (EffectCmd e : second.effectCmds) {
        e.effectOffset += effectBase;
        cl.effectCmds.push_back(e);
    }
    for (size_t i = 12; i < second.commands.size(); ++i) {
        Command cmd = second.commands[i];
        if (cmd.kind == CmdKind::FillPath) {
            cmd.index += fillBase;
        } else if (cmd.kind == CmdKind::PushEffect) {
            cmd.index += effectCmdBase;
        }
        cl.commands.push_back(cmd);
    }
    return cl;
}

}  // namespace

TEST_CASE(
    "gpu backdrop cache: a device with the texture capability gets the frames of before on a miss (T-764)") {
    TwoBuilds b;
    Spec s;
    for (int i = 0; i < 2; ++i) {  // drawn in full, then stored
        b.build(scene(s));
        CHECK(fingerprint(b.direct.frame) == fingerprint(b.paste.frame));
    }
    Spec content = s;
    content.content = Color{1, 0.9f, 0.9f, 0.35f};
    b.build(scene(content));  // another content: a miss
    CHECK(b.direct.builder.lastBuildStats().backdropPairMisses == 1u);
    CHECK(fingerprint(b.direct.frame) == fingerprint(b.paste.frame));
    // A plain BackdropBlur and one followed by another kind of effect: not pairs.
    for (const std::vector<EffectInstance>& effects :
         {std::vector<EffectInstance>{blurFx()},
          std::vector<EffectInstance>{blurFx(), outerFx(), adjustFx()}}) {
        TwoBuilds o;
        Spec e;
        e.effects = effects;
        for (int i = 0; i < 4; ++i) {
            o.build(scene(e));
            CHECK(fingerprint(o.direct.frame) == fingerprint(o.paste.frame));
        }
    }
}

TEST_CASE(
    "gpu backdrop cache: a hit samples the cached pair directly, two passes and two transients fewer "
    "(T-764)") {
    TwoBuilds b;
    Spec s;
    b.build(scene(s));
    b.build(scene(s));
    for (int i = 0; i < 3; ++i) {
        CAPTURE(i);
        s.stripes = static_cast<float>(i) * 3.0f;
        b.build(scene(s));
        CHECK(b.direct.builder.lastBuildStats().backdropPairHits == 1u);
        CHECK(b.paste.builder.lastBuildStats().backdropPairHits == 1u);
        const Shape paste = shapeOf(b.paste.frame);
        const Shape direct = shapeOf(b.direct.frame);
        CHECK(paste.pastedInputs);
        CHECK(direct.passes + 2u == paste.passes);
        CHECK(b.direct.frame.transients.size() + 2u == b.paste.frame.transients.size());
        CHECK(direct.masked == 1u);
        CHECK(direct.levelMix == 1u);
        CHECK(direct.shadowPasses == 0u);
        CHECK(direct.copies == paste.copies);
        CHECK(textureQuads(b.direct.frame) == 0u);
        CHECK(textureQuads(b.paste.frame) == 2u);
        const auto masked = maskedOf(b.direct.frame);
        REQUIRE(masked.size() == 1u);
        const DrawCmd& c = masked[0].first;
        const BackdropInputs& in = masked[0].second;
        CHECK(in.textured);
        CHECK(c.src == 0u);
        CHECK(c.aux != 0u);
        CHECK(c.texture == 0u);  // DrawCmd::texture keeps its own use (the LUT), unused here
        CHECK(in.maskTarget == 0u);
        // The two cached textures, R' and S, are the ones the paste device pasted.
        CHECK(std::set<TextureHandle>{in.topTexture, in.maskTexture} == paste.pastedTextures);
        CHECK(in.topTexture != in.maskTexture);
        // At rest: the same window, the identity mapping, the nearest texel.
        for (const float* uv : {in.params.topUv, in.params.maskUv}) {
            CHECK(uv[0] == 1.0f);
            CHECK(uv[1] == 1.0f);
            CHECK(uv[2] == 0.0f);
            CHECK(uv[3] == 0.0f);
        }
        CHECK(in.params.topSampler == static_cast<float>(Sampler::PointClamp));
        CHECK(in.params.maskSampler == static_cast<float>(Sampler::PointClamp));
        std::string dump;
        dumpGpuFrame(b.direct.frame, dump);
        CHECK(dump.find("top=tex:point uv=(1,1,0,0) mask=tex:point uv=(1,1,0,0)") != std::string::npos);
    }
}

TEST_CASE("gpu backdrop cache: two panels served directly, four passes fewer (T-764)") {
    TwoBuilds b;
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        b.build(twoPanels(static_cast<float>(i) * 2.0f));
        if (i < 2) {
            CHECK(fingerprint(b.direct.frame) == fingerprint(b.paste.frame));
            continue;
        }
        CHECK(b.direct.builder.lastBuildStats().backdropPairHits == 2u);
        CHECK(shapeOf(b.direct.frame).passes + 4u == shapeOf(b.paste.frame).passes);
        // Four transients fewer are borrowed; the second panel's pastes reuse the first panel's released
        // slots, so the frame declares two fewer.
        CHECK(b.direct.frame.transients.size() + 2u == b.paste.frame.transients.size());
        CHECK(maskedOf(b.direct.frame).size() == 2u);
        CHECK(textureQuads(b.direct.frame) == 0u);
        CHECK(textureQuads(b.paste.frame) == 4u);
    }
}

namespace {

/// The quad the paste device drew for a cached texture (its first Texture command): x0, y0, x1, y1.
struct Quad {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
};
Quad pastedQuad(const GpuFrame& f) {
    Quad q;
    for (const DrawCmd& c : f.cmds) {
        if (c.shading != Shading::Texture) continue;
        for (uint32_t i = 0; i < c.indexCount; ++i) {
            const Vertex& v = f.vertices[c.baseVertex + f.indices[c.firstIndex + i]];
            q.x0 = std::min(q.x0, v.x);
            q.y0 = std::min(q.y0, v.y);
            q.x1 = std::max(q.x1, v.x);
            q.y1 = std::max(q.y1, v.y);
        }
        break;
    }
    return q;
}

/// Where the texture inputs of the direct frame's BackdropMasked lie in surface pixels (the window of
/// its pass mapped back through the uv scale and offset), and their sampler.
Quad sampledQuad(const GpuFrame& f, float& sampler) {
    Quad q;
    for (const Pass& pass : f.passes) {
        for (uint32_t k = pass.firstCmd; k < pass.firstCmd + pass.cmdCount; ++k) {
            BackdropInputs in;
            if (!backdropInputs(f, f.cmds[k], in) || !in.textured) continue;
            const BackdropParams& bp = in.params;
            for (int i = 0; i < 4; ++i) {
                CHECK(bp.maskUv[i] == bp.topUv[i]);
            }
            CHECK(bp.maskSampler == bp.topSampler);
            sampler = bp.topSampler;
            const float w = static_cast<float>(pass.viewW) / bp.topUv[0];
            const float h = static_cast<float>(pass.viewH) / bp.topUv[1];
            q.x0 = static_cast<float>(pass.viewX) - bp.topUv[2] * w;
            q.y0 = static_cast<float>(pass.viewY) - bp.topUv[3] * h;
            q.x1 = q.x0 + w;
            q.y1 = q.y0 + h;
        }
    }
    return q;
}

void checkSameQuad(const GpuFrame& direct, const GpuFrame& paste, Sampler expected) {
    float sampler = -1;
    const Quad s = sampledQuad(direct, sampler);
    const Quad p = pastedQuad(paste);
    MESSAGE("pasted quad ", p.x0, ",", p.y0, " - ", p.x1, ",", p.y1, "; sampled ", s.x0, ",", s.y0, " - ",
            s.x1, ",", s.y1);
    CHECK(sampler == static_cast<float>(expected));
    CHECK(s.x0 == doctest::Approx(p.x0).epsilon(1e-5));
    CHECK(s.y0 == doctest::Approx(p.y0).epsilon(1e-5));
    CHECK(s.x1 == doctest::Approx(p.x1).epsilon(1e-5));
    CHECK(s.y1 == doctest::Approx(p.y1).epsilon(1e-5));
}

}  // namespace

TEST_CASE(
    "gpu backdrop cache: a moving panel samples the pair where the paste would have put it -- the nearest "
    "texel at whole pixels, bilinearly at fractions (T-764)") {
    TwoBuilds w;
    Spec s;
    for (int i = 0; i < 5; ++i) {
        CAPTURE(i);
        s.at = Vec2{40.0f + static_cast<float>(i), 30.0f + static_cast<float>(i / 2)};
        w.build(scene(s));
        if (i >= 2) {
            REQUIRE(w.direct.builder.lastBuildStats().backdropPairHits == 1u);
            checkSameQuad(w.direct.frame, w.paste.frame, Sampler::PointClamp);
        }
    }
    TwoBuilds f;
    const float xs[] = {40.0f, 40.25f, 40.5f, 40.75f, 41.125f};
    for (int i = 0; i < 5; ++i) {
        CAPTURE(i);
        s.at = Vec2{xs[i], 30.0f + 0.375f * static_cast<float>(i)};
        f.build(scene(s));
        if (i >= 2) {
            REQUIRE(f.direct.builder.lastBuildStats().backdropPairHits == 1u);
            REQUIRE(f.direct.builder.lastBuildStats().effectCachePhaseHits == 1u);
            checkSameQuad(f.direct.frame, f.paste.frame, Sampler::LinearClamp);
        }
    }
    // It stops: the settle is drawn in full on both, then the pair is served again at the same phase.
    f.build(scene(s));
    CHECK(fingerprint(f.direct.frame) == fingerprint(f.paste.frame));
    f.build(scene(s));
    REQUIRE(f.direct.builder.lastBuildStats().backdropPairHits == 1u);
    checkSameQuad(f.direct.frame, f.paste.frame, Sampler::PointClamp);
}

TEST_CASE(
    "gpu backdrop cache: BackdropParams' trailing block is parsed only when it is well formed (T-764)") {
    GpuFrame f;
    f.transients.resize(3);
    DrawCmd c;
    c.shading = Shading::Effect;
    c.program = EffectProgram::BackdropMasked;
    BackdropParams bp{};
    bp.mask = kBackdropMaskTextured;
    bp.topIsTexture = 1;
    bp.maskIsTexture = 0;
    bp.topTexture = 7;
    bp.maskTexture = 2;  // a target
    bp.topUv[0] = bp.topUv[1] = 1;
    bp.topSampler = static_cast<float>(Sampler::LinearClamp);
    auto parse = [&](const BackdropParams& p, BackdropInputs& in) {
        f.params.assign(sizeof(p) / sizeof(float), 0.0f);
        std::memcpy(f.params.data(), &p, sizeof(p));
        return backdropInputs(f, c, in);
    };
    BackdropInputs in;
    REQUIRE(parse(bp, in));
    CHECK(in.textured);
    CHECK(in.topTexture == 7u);
    CHECK(in.maskTexture == 0u);
    CHECK(in.maskTarget == 2u);
    BackdropParams bad = bp;
    bad.topIsTexture = 0.5f;
    REQUIRE(parse(bad, in));
    CHECK_FALSE(in.textured);
    CHECK(in.topTexture == 0u);
    CHECK(in.maskTarget == 0u);
    bad = bp;
    bad.topTexture = 7.5f;
    parse(bad, in);
    CHECK_FALSE(in.textured);
    bad = bp;
    bad.topSampler = 3;
    parse(bad, in);
    CHECK_FALSE(in.textured);
    bad = bp;
    bad.topUv[2] = std::numeric_limits<float>::infinity();
    parse(bad, in);
    CHECK_FALSE(in.textured);
    // Too short a block for the trailing fields.
    f.params.assign(3, 0.0f);
    f.params[0] = kBackdropMaskTextured;
    REQUIRE(backdropInputs(f, c, in));
    CHECK_FALSE(in.textured);
    // The one-float block of before.
    f.params.assign(1, 2.0f);
    REQUIRE(backdropInputs(f, c, in));
    CHECK_FALSE(in.textured);
    CHECK(in.maskTarget == 2u);
    CHECK(backdropMaskTarget(f, c) == 2u);
}
