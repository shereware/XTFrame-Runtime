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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <set>
#include <sstream>
#include <string>
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
    FakeDevice(bool cache, bool mask) : m_cache(cache), m_mask(mask) {}
    bool supportsCoverageDraws() const override { return true; }
    bool supportsBoxShadowShading() const override { return true; }
    bool supportsAnalyticClip() const override { return true; }
    bool supportsAlphaChannelReads() const override { return true; }
    bool supportsDirectTarget() const override { return true; }
    bool supportsBackdropMask() const override { return m_mask; }
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
    Built(bool cache, bool mask, int w = kW, int h = kH) : device(cache, mask) {
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

}  // namespace

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
