// T-721 (Docs/design/10-backends.md §10.16 W2, Docs/tasks/T-721.md "測試"): the effect result cache
// keyed independently of the segment's position -- a segment that only moves keeps its result -- on
// the frame builder alone, against a device that hands out handles (the pixel checks are in
// Tests/wgpu/test_wgpu_effect_cache.cpp).

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "camera.h"  // Code/Runtime/core/src: cssPerspectiveViewProj
#include "doctest.h"
#include "rounded_rect.h"  // Code/Runtime/core/src: appendRoundedRectPath
#include "xt/command_list.h"
#include "xt/gpu/frame_builder.h"
#include "xt/gpu/gpu_device.h"
#include "xt/gpu/gpu_frame.h"
#include "xt/types.h"

using namespace xt;
using namespace xt::gpu;

namespace {

constexpr int kSurface = 512;

/// Hands out handles and counts the cached targets it holds; executes nothing.
class CacheDevice : public GpuDevice {
public:
    bool supportsCoverageDraws() const override { return true; }
    bool supportsBoxShadowShading() const override { return true; }
    bool supportsAnalyticClip() const override { return true; }
    TextureHandle createTexture(const char*, uint16_t, uint16_t, TextureFormat, const void*) override {
        return m_next++;
    }
    void updateTexture(TextureHandle, uint16_t, uint16_t, uint16_t, uint16_t, const void*, size_t) override {}
    void destroyTexture(TextureHandle) override {}
    void submit(const GpuFrame&) override {}
    TextureHandle acquireCachedTarget(uint16_t, uint16_t, TextureFormat) override {
        ++cachedAlive;
        return m_next++;
    }
    void releaseCachedTarget(TextureHandle) override { --cachedAlive; }

    int cachedAlive = 0;

private:
    TextureHandle m_next = 1;
};

struct Built {
    explicit Built(bool fringe = true) {
        builder.init(device);
        builder.resize(kSurface, kSurface);
        builder.setEdgeFringe(fringe);
    }
    ~Built() { builder.shutdown(); }
    void build(const CommandList& cl) { builder.build(cl, nullptr, nullptr, nullptr, frame); }
    uint32_t hits() const { return builder.lastEffectCacheStats().hits; }
    uint32_t misses() const { return builder.lastEffectCacheStats().misses; }
    uint32_t stores() const { return builder.lastEffectCacheStats().stores; }
    uint32_t phaseHits() const { return builder.lastBuildStats().effectCachePhaseHits; }
    uint32_t settles() const { return builder.lastBuildStats().effectCacheSettles; }
    CacheDevice device;
    FrameBuilder builder;
    GpuFrame frame;
};

CommandList makeList() {
    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(kSurface), static_cast<float>(kSurface)};
    cl.devicePixelRatio = 1;
    return cl;
}

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

/// A fill of the local rectangle `r` under `xf`, as the Painter writes one (local path, world xf).
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

EffectInstance dropShadow(float blur = 6.0f) {
    EffectInstance e{};
    e.kind = EffectKind::DropShadow;
    e.params[0] = 4;
    e.params[1] = 4;
    e.params[2] = blur;
    e.params[3] = 0;
    e.params[4] = 0.8f;
    e.colors[0] = Color{0, 0, 0, 1};
    return e;
}

EffectInstance backdropBlur() {
    EffectInstance e{};
    e.kind = EffectKind::BackdropBlur;
    e.params[0] = 6;
    e.params[1] = 1;
    e.params[2] = 1;
    e.enabled = 1;
    return e;
}

constexpr float kCard = 48.0f;    ///< the card's side
constexpr float kMargin = 24.0f;  ///< the effect's expansion around it

/// A 48 px card with a drop shadow whose effect bounds start at `at` (so `at` is the anchor), as the
/// Painter lays one out: the node's world transform on the fill and on the effect, its bounds on
/// the screen.
void addCard(CommandList& cl, Vec2 at, EffectInstance fx = dropShadow(),
             Color color = Color{1, 0.2f, 0.1f, 1}) {
    const Mat2x3 world = translation(Vec2{at.x + kMargin, at.y + kMargin});
    EffectCmd c{};
    c.effectOffset = static_cast<uint32_t>(cl.effects.size());
    c.effectCount = 1;
    cl.effects.push_back(fx);
    c.xf = world;
    c.boundsHint = Rect{at.x, at.y, kCard + 2 * kMargin, kCard + 2 * kMargin};
    c.contentBounds = Rect{at.x + kMargin, at.y + kMargin, kCard, kCard};
    cl.effectCmds.push_back(c);
    cl.commands.push_back(Command{CmdKind::PushEffect, {}, static_cast<uint32_t>(cl.effectCmds.size() - 1)});
    fillRect(cl, Rect{0, 0, kCard, kCard}, color, world);
    cl.commands.push_back(Command{CmdKind::PopEffect, {}, 0});
}

CommandList cardList(Vec2 at, EffectInstance fx = dropShadow(), Color color = Color{1, 0.2f, 0.1f, 1}) {
    CommandList cl = makeList();
    addCard(cl, at, fx, color);
    return cl;
}

/// A rounded clip as the Painter writes one: the box in the node's own space, its world transform.
void pushRoundedClip(CommandList& cl, Rect box, float radius, Mat2x3 xf) {
    ClipPathCmd c{};
    c.xf = xf;
    c.path.verbOffset = static_cast<uint32_t>(cl.verbs.size());
    c.path.pointOffset = static_cast<uint32_t>(cl.points.size());
    appendRoundedRectPath(box, radius, radius, radius, radius, cl.verbs, cl.points);
    c.path.verbCount = static_cast<uint32_t>(cl.verbs.size()) - c.path.verbOffset;
    c.path.pointCount = static_cast<uint32_t>(cl.points.size()) - c.path.pointOffset;
    c.rule = FillRule::NonZero;
    cl.clipPaths.push_back(c);
    cl.commands.push_back(Command{CmdKind::PushClipPath, {}, static_cast<uint32_t>(cl.clipPaths.size() - 1)});
}

std::vector<const DrawCmd*> cmdsWith(const GpuFrame& f, Shading s) {
    std::vector<const DrawCmd*> out;
    for (const DrawCmd& c : f.cmds) {
        if (c.shading == s) {
            out.push_back(&c);
        }
    }
    return out;
}

/// The quads sampling a cached effect result (the only Texture commands of these lists).
std::vector<const DrawCmd*> pastes(const GpuFrame& f) { return cmdsWith(f, Shading::Texture); }

bool effectDrawn(const GpuFrame& f) { return !cmdsWith(f, Shading::Effect).empty(); }

/// The top-left corner and size of a paste quad (the `index`-th of a command that batched several).
Rect quadRect(const GpuFrame& f, const DrawCmd& c, uint32_t index = 0) {
    const Vertex& a = f.vertices[c.baseVertex + index * 4];
    const Vertex& b = f.vertices[c.baseVertex + index * 4 + 2];
    return Rect{a.x, a.y, b.x - a.x, b.y - a.y};
}

/// Everything of a frame but the texture handles (two devices number them differently).
bool sameFrame(const GpuFrame& a, const GpuFrame& b) {
    if (a.cmds.size() != b.cmds.size() || a.passes.size() != b.passes.size() ||
        a.vertices.size() != b.vertices.size() || a.indices.size() != b.indices.size() ||
        a.params.size() != b.params.size() || a.transients.size() != b.transients.size() ||
        a.cacheStores.size() != b.cacheStores.size()) {
        return false;
    }
    for (size_t i = 0; i < a.cmds.size(); ++i) {
        DrawCmd x = a.cmds[i], y = b.cmds[i];
        x.texture = y.texture = 0;
        if (std::memcmp(&x, &y, sizeof(DrawCmd)) != 0) {
            return false;
        }
    }
    return std::memcmp(a.passes.data(), b.passes.data(), a.passes.size() * sizeof(Pass)) == 0 &&
           std::memcmp(a.vertices.data(), b.vertices.data(), a.vertices.size() * sizeof(Vertex)) == 0 &&
           std::memcmp(a.indices.data(), b.indices.data(), a.indices.size() * sizeof(uint32_t)) == 0 &&
           std::memcmp(a.params.data(), b.params.data(), a.params.size() * sizeof(float)) == 0;
}

/// The frame a builder with an empty cache reaches for `cl` at rest (its third build).
void restFrame(const CommandList& cl, Built& fresh) {
    fresh.build(cl);
    fresh.build(cl);
    fresh.build(cl);
}

}  // namespace

TEST_CASE(
    "gpu effect cache: T-721 a segment moving by whole pixels is served from its result from the third "
    "frame") {
    Built b;
    // Frame 1: first appearance. Frame 2: the key again, one pixel on: drawn in its usual window
    // (a whole-pixel move needs no border) and stored.
    b.build(cardList(Vec2{100, 60}));
    CHECK(b.misses() == 1);
    CHECK(b.frame.cacheStores.empty());
    b.build(cardList(Vec2{101, 60}));
    CHECK(b.misses() == 1);
    REQUIRE(b.frame.cacheStores.size() == 1);
    REQUIRE(b.frame.cacheStores[0].src >= 1);
    CHECK(b.frame.transients[b.frame.cacheStores[0].src - 1].width == 96);
    const TextureHandle cached = b.frame.cacheStores[0].texture;

    // Frames 3 and 4: one quad of the result at the new place, texel on texel.
    for (float x : {102.0f, 103.0f}) {
        b.build(cardList(Vec2{x, 61}));
        CHECK(b.hits() == 1);
        CHECK(b.misses() == 0);
        CHECK(b.phaseHits() == 0);
        CHECK(b.settles() == 0);
        CHECK_FALSE(effectDrawn(b.frame));
        CHECK(b.frame.transients.empty());
        REQUIRE(pastes(b.frame).size() == 1);
        const DrawCmd& q = *pastes(b.frame)[0];
        CHECK(q.texture == cached);
        CHECK(q.sampler == Sampler::PointClamp);  // T-735: same phase, the nearest texel
        CHECK(q.clipParamIndex == 0);
        const Rect r = quadRect(b.frame, q);
        CHECK(r.x == x);
        CHECK(r.y == 61.0f);
        CHECK(r.w == 96.0f);
        CHECK(r.h == 96.0f);
        CHECK(b.frame.vertices[q.baseVertex].u == 0.0f);
        CHECK(b.frame.vertices[q.baseVertex + 2].u == 1.0f);
        // A build of the same list would find the segment at rest: not the final frame yet.
        CHECK_FALSE(b.builder.lastFrameReusable(nullptr, nullptr));
    }

    // It stops: the result is exactly its own (a whole number of pixels away, same exact relative
    // hash), so nothing is drawn again and the frame is the one an empty cache reaches.
    b.build(cardList(Vec2{103, 61}));
    CHECK(b.hits() == 1);
    CHECK(b.settles() == 0);
    CHECK(b.builder.lastFrameReusable(nullptr, nullptr));
    Built fresh;
    restFrame(cardList(Vec2{103, 61}), fresh);
    CHECK(sameFrame(b.frame, fresh.frame));
    b.build(cardList(Vec2{103, 61}));
    CHECK(b.hits() == 1);
    CHECK(sameFrame(b.frame, fresh.frame));
    CHECK(b.device.cachedAlive == 1);
}

TEST_CASE(
    "gpu effect cache: T-721 a segment moving by fractions is pasted bilinearly, then settles once at rest") {
    Built b;
    b.build(cardList(Vec2{100.0f, 60}));
    CHECK(b.misses() == 1);
    // Frame 2: moving by a fraction -> drawn in a window grown by the 1 px border, and stored.
    b.build(cardList(Vec2{100.25f, 60}));
    CHECK(b.misses() == 1);
    REQUIRE(b.frame.cacheStores.size() == 1);
    const TargetHandle src = b.frame.cacheStores[0].src;
    REQUIRE(src >= 1);
    const Pass& storePass = b.frame.passes[b.frame.cacheStores[0].pass];
    CHECK(storePass.viewX == 99);  // floor(100.25) - 1
    CHECK(storePass.viewY == 59);
    CHECK(storePass.viewW == 128);  // 97 px of bounds + 2 -> the next 32
    const TextureHandle cached = b.frame.cacheStores[0].texture;

    // Frames 3 and 4: the same result half a pixel and three quarters of a pixel on.
    const float xs[2] = {100.5f, 100.75f};
    for (float x : xs) {
        b.build(cardList(Vec2{x, 60}));
        CHECK(b.hits() == 1);
        CHECK(b.misses() == 0);
        CHECK(b.phaseHits() == 1);
        CHECK(b.settles() == 0);
        CHECK_FALSE(effectDrawn(b.frame));
        REQUIRE(pastes(b.frame).size() == 1);
        const DrawCmd& q = *pastes(b.frame)[0];
        CHECK(q.texture == cached);
        const Rect r = quadRect(b.frame, q);
        CHECK(r.x == 99.0f + (x - 100.25f));
        CHECK(r.y == 59.0f);
        CHECK(r.w == 128.0f);
        CHECK_FALSE(b.builder.lastFrameReusable(nullptr, nullptr));
    }

    // It stops at 100.75: drawn once more in place, in its usual window, and the entry replaced.
    b.build(cardList(Vec2{100.75f, 60}));
    CHECK(b.settles() == 1);
    CHECK(b.misses() == 1);
    CHECK(b.hits() == 0);
    CHECK(b.phaseHits() == 0);
    CHECK(effectDrawn(b.frame));
    REQUIRE(b.frame.cacheStores.size() == 1);
    CHECK(b.frame.passes[b.frame.cacheStores[0].pass].viewX == 100);
    CHECK(b.frame.cacheStores[0].texture != cached);
    CHECK(b.device.cachedAlive == 1);  // the moving result went at the end of the build
    CHECK_FALSE(b.builder.lastFrameReusable(nullptr, nullptr));

    // From then on: the frame an empty cache reaches for the list.
    Built fresh;
    restFrame(cardList(Vec2{100.75f, 60}), fresh);
    for (int i = 0; i < 2; ++i) {
        b.build(cardList(Vec2{100.75f, 60}));
        CHECK(b.hits() == 1);
        CHECK(b.phaseHits() == 0);
        CHECK(b.settles() == 0);
        CHECK(sameFrame(b.frame, fresh.frame));
        CHECK(b.builder.lastFrameReusable(nullptr, nullptr));
    }
}

TEST_CASE(
    "gpu effect cache: T-721 a result kept at rest is drawn once more with its border when the segment "
    "starts to drift") {
    Built b;
    restFrame(cardList(Vec2{100, 60}), b);
    CHECK(b.hits() == 1);
    b.build(cardList(Vec2{100.5f, 60}));  // the entry has no border: not sampled between texels
    CHECK(b.misses() == 1);
    CHECK(b.phaseHits() == 0);
    CHECK(b.settles() == 0);
    REQUIRE(b.frame.cacheStores.size() == 1);
    CHECK(b.frame.passes[b.frame.cacheStores[0].pass].viewX == 99);
    b.build(cardList(Vec2{101.0f, 60}));
    CHECK(b.hits() == 1);
    CHECK(b.phaseHits() == 1);
    CHECK(b.device.cachedAlive == 1);
}

TEST_CASE("gpu effect cache: T-721 a window cut by the scissor or the surface keeps the absolute key") {
    // Moving one pixel a frame: with the absolute key nothing is ever stored or served.
    auto neverServed = [](auto&& listAt) {
        Built b;
        for (int i = 0; i < 5; ++i) {
            b.build(listAt(static_cast<float>(i)));
            CHECK(b.hits() == 0);
            CHECK(b.misses() == 1);
            CHECK(b.frame.cacheStores.empty());
            CHECK(b.phaseHits() == 0);
            CHECK(b.settles() == 0);
        }
        // At rest it is stored on the second identical build and served on the third, as before.
        b.build(listAt(4.0f));
        CHECK(b.frame.cacheStores.size() == 1);
        b.build(listAt(4.0f));
        CHECK(b.hits() == 1);
        CHECK(b.builder.lastFrameReusable(nullptr, nullptr));
    };
    SUBCASE("a scissor through the effect's bounds") {
        neverServed([](float i) {
            CommandList cl = makeList();
            ClipRectCmd clip{};
            clip.rect = Rect{0, 0, 180, 400};
            cl.clipRects.push_back(clip);
            cl.commands.push_back(Command{CmdKind::PushClipRect, {}, 0});
            addCard(cl, Vec2{100 + i, 60});
            cl.commands.push_back(Command{CmdKind::PopClip, {}, 0});
            return cl;
        });
    }
    SUBCASE("a scissor closer than the window's padding") {
        // The 96 px bounds fit, the window padded for the border (128) does not.
        neverServed([](float i) {
            CommandList cl = makeList();
            ClipRectCmd clip{};
            clip.rect = Rect{0, 0, 210, 400};
            cl.clipRects.push_back(clip);
            cl.commands.push_back(Command{CmdKind::PushClipRect, {}, 0});
            addCard(cl, Vec2{100 + i, 60});
            cl.commands.push_back(Command{CmdKind::PopClip, {}, 0});
            return cl;
        });
    }
    SUBCASE("bounds on the surface's edge") {
        neverServed([](float i) { return cardList(Vec2{0, 60 + i}); });
    }
    SUBCASE("a window that would leave the surface") {
        neverServed([](float i) { return cardList(Vec2{kSurface - 100.0f, 60 + i}); });
    }
    SUBCASE("inside a layer whose window cuts it") {
        neverServed([](float i) {
            CommandList cl = makeList();
            LayerCmd layer{};
            layer.opacity = 0.5f;
            layer.blend = BlendMode::Normal;
            layer.boundsHint = Rect{64, 32, 128, 256};
            cl.layers.push_back(layer);
            cl.commands.push_back(Command{CmdKind::PushLayer, {}, 0});
            addCard(cl, Vec2{100 + i, 60});
            cl.commands.push_back(Command{CmdKind::PopLayer, {}, 0});
            return cl;
        });
    }
}

TEST_CASE("gpu effect cache: T-721 a segment in a perspective projection keeps the absolute key") {
    auto listAt = [](float i) {
        CommandList cl = makeList();
        ProjectionCmd p{};
        p.viewProj = cssPerspectiveViewProj(400.0f, Vec2{256, 256}).viewProj;
        p.boxRect = Rect{0, 0, static_cast<float>(kSurface), static_cast<float>(kSurface)};
        cl.projections.push_back(p);
        cl.commands.push_back(Command{CmdKind::PushProjection, {}, 0});
        Transform3DCmd t{};
        t.world3 = mat4RotateY(35.0f);
        cl.transforms3.push_back(t);
        cl.commands.push_back(Command{CmdKind::PushTransform3D, {}, 0});
        addCard(cl, Vec2{200 + i, 200});
        cl.commands.push_back(Command{CmdKind::PopTransform3D, {}, 0});
        cl.commands.push_back(Command{CmdKind::PopProjection, {}, 0});
        return cl;
    };
    Built b;
    for (int i = 0; i < 5; ++i) {
        b.build(listAt(static_cast<float>(i)));
        CHECK(b.hits() == 0);
        CHECK(b.frame.cacheStores.empty());
        CHECK(b.phaseHits() == 0);
    }
    b.build(listAt(4.0f));
    CHECK(b.frame.cacheStores.size() == 1);
    b.build(listAt(4.0f));
    CHECK(b.hits() == 1);
}

TEST_CASE("gpu effect cache: T-721 a segment holding a 3D space or reading the backdrop is not relocated") {
    SUBCASE("a 3D space root inside the effect") {
        auto listAt = [](float i) {
            CommandList cl = makeList();
            const Vec2 at{100 + i, 60};
            const Mat2x3 world = translation(Vec2{at.x + kMargin, at.y + kMargin});
            EffectCmd c{};
            c.effectCount = 1;
            cl.effects.push_back(dropShadow());
            c.xf = world;
            c.boundsHint = Rect{at.x, at.y, kCard + 2 * kMargin, kCard + 2 * kMargin};
            c.contentBounds = Rect{at.x + kMargin, at.y + kMargin, kCard, kCard};
            cl.effectCmds.push_back(c);
            cl.commands.push_back(Command{CmdKind::PushEffect, {}, 0});
            ProjectionCmd p{};
            p.viewProj = cssPerspectiveViewProj(400.0f, Vec2{at.x + 48, at.y + 48}).viewProj;
            p.boxRect = Rect{0, 0, kCard, kCard};
            p.rootXf = world;
            cl.projections.push_back(p);
            cl.commands.push_back(Command{CmdKind::PushProjection, {}, 0});
            fillRect(cl, Rect{0, 0, kCard, kCard}, Color{1, 0, 0, 1});
            cl.commands.push_back(Command{CmdKind::PopProjection, {}, 0});
            cl.commands.push_back(Command{CmdKind::PopEffect, {}, 0});
            return cl;
        };
        Built b;
        for (int i = 0; i < 5; ++i) {
            b.build(listAt(static_cast<float>(i)));
            CHECK(b.hits() == 0);
            CHECK(b.frame.cacheStores.empty());
        }
    }
    SUBCASE("a backdrop blur") {
        Built b;
        for (int i = 0; i < 5; ++i) {
            b.build(cardList(Vec2{100.0f + static_cast<float>(i), 60}, backdropBlur()));
            CHECK(b.hits() == 0);
            CHECK(b.misses() == 0);  // not cacheable at all, as before
            CHECK(b.frame.cacheStores.empty());
            CHECK(b.phaseHits() == 0);
        }
    }
}

TEST_CASE(
    "gpu effect cache: T-721 an analytic clip moving with the segment hits, one it moves inside does not") {
    // The clip's content covers its corners (a fill over the whole box), so it is an analytic level.
    auto listAt = [](float card, float clip) {
        CommandList cl = makeList();
        pushRoundedClip(cl, Rect{0, 0, 300, 300}, 24, translation(Vec2{40 + clip, 20}));
        fillRect(cl, Rect{0, 0, static_cast<float>(kSurface), static_cast<float>(kSurface)},
                 Color{0, 0, 1, 1});
        addCard(cl, Vec2{100 + card, 60});
        cl.commands.push_back(Command{CmdKind::PopClip, {}, 0});
        return cl;
    };
    SUBCASE("the clip moves with the card") {
        Built b;
        b.build(listAt(0, 0));
        REQUIRE(b.builder.lastBuildStats().analyticClips == 1);
        CHECK(b.misses() == 1);
        b.build(listAt(1, 1));
        CHECK(b.frame.cacheStores.size() == 1);
        for (float i : {2.0f, 3.0f}) {
            b.build(listAt(i, i));
            CHECK(b.hits() == 1);
            CHECK(b.misses() == 0);
            CHECK_FALSE(effectDrawn(b.frame));
        }
    }
    SUBCASE("the card moves inside a clip that stays") {
        Built b;
        for (int i = 0; i < 5; ++i) {
            b.build(listAt(static_cast<float>(i), 0));
            REQUIRE(b.builder.lastBuildStats().analyticClips == 1);
            CHECK(b.hits() == 0);
            CHECK(b.misses() == 1);
            CHECK(b.frame.cacheStores.empty());
        }
    }
}

TEST_CASE("gpu effect cache: T-721 two segments alike at different places share one result") {
    SUBCASE("a whole number of pixels apart") {
        CommandList cl = makeList();
        addCard(cl, Vec2{40, 60});
        addCard(cl, Vec2{240, 200});
        Built b;
        b.build(cl);
        CHECK(b.misses() == 2);
        // The first is stored; the second is already served from it (the store precedes its pass).
        b.build(cl);
        CHECK(b.misses() == 1);
        CHECK(b.hits() == 1);
        CHECK(b.frame.cacheStores.size() == 1);
        for (int i = 0; i < 2; ++i) {
            b.build(cl);
            CHECK(b.hits() == 2);
            CHECK(b.misses() == 0);
            CHECK(b.phaseHits() == 0);
            CHECK(b.settles() == 0);
            CHECK(b.device.cachedAlive == 1);
            // Two quads of one texture, one after the other: batched into one command.
            const auto quads = pastes(b.frame);
            REQUIRE(quads.size() == 1);
            REQUIRE(quads[0]->vertexCount == 8);
            CHECK(quadRect(b.frame, *quads[0], 0).x == 40.0f);
            CHECK(quadRect(b.frame, *quads[0], 1).x == 240.0f);
            CHECK(quadRect(b.frame, *quads[0], 1).y == 200.0f);
            CHECK(b.builder.lastFrameReusable(nullptr, nullptr));
        }
    }
    SUBCASE("at different sub-pixel phases: the first one drawn keeps its phase") {
        CommandList cl = makeList();
        addCard(cl, Vec2{40, 60});
        addCard(cl, Vec2{240.5f, 200});
        Built b;
        b.build(cl);
        b.build(cl);
        CHECK(b.frame.cacheStores.size() == 1);
        for (int i = 0; i < 3; ++i) {
            b.build(cl);
            CHECK(b.hits() == 2);
            CHECK(b.phaseHits() == 1);
            CHECK(b.settles() == 0);
            CHECK(b.device.cachedAlive == 1);
            // T-735: the one at its own phase is drawn with the nearest texel, the other bilinearly --
            // two samplers, so two commands of one texture.
            const auto quads = pastes(b.frame);
            REQUIRE(quads.size() == 2);
            REQUIRE(quads[0]->vertexCount == 4);
            REQUIRE(quads[1]->vertexCount == 4);
            CHECK(quads[0]->texture == quads[1]->texture);
            CHECK(quads[0]->sampler == Sampler::PointClamp);
            CHECK(quads[1]->sampler == Sampler::LinearClamp);
            CHECK(quadRect(b.frame, *quads[0], 0).x == 40.0f);
            CHECK(quadRect(b.frame, *quads[1], 0).x == 240.5f);
            CHECK(b.builder.lastFrameReusable(nullptr, nullptr));
        }
    }
}

TEST_CASE("gpu effect cache: T-721 another effect parameter or content misses, wherever the segment is") {
    Built b;
    b.build(cardList(Vec2{100, 60}));
    b.build(cardList(Vec2{101, 60}));
    b.build(cardList(Vec2{102, 60}));
    REQUIRE(b.hits() == 1);
    SUBCASE("the shadow's blur") {
        b.build(cardList(Vec2{103, 60}, dropShadow(7.0f)));
        CHECK(b.hits() == 0);
        CHECK(b.misses() == 1);
        CHECK(b.frame.cacheStores.empty());
    }
    SUBCASE("the content's colour") {
        b.build(cardList(Vec2{103, 60}, dropShadow(), Color{0, 1, 0, 1}));
        CHECK(b.hits() == 0);
        CHECK(b.misses() == 1);
    }
    SUBCASE("the content's size") {
        CommandList cl = cardList(Vec2{103, 60});
        cl.points[2] = kCard - 1;  // the fill's top-right corner
        b.build(cl);
        CHECK(b.hits() == 0);
        CHECK(b.misses() == 1);
    }
    SUBCASE("the content's place inside the effect") {
        CommandList cl = cardList(Vec2{103, 60});
        cl.fills[0].xf.e += 2;
        b.build(cl);
        CHECK(b.hits() == 0);
        CHECK(b.misses() == 1);
    }
    SUBCASE("the pixel ratio") {
        CommandList cl = cardList(Vec2{103, 60});
        cl.devicePixelRatio = 1.5f;
        b.build(cl);
        CHECK(b.hits() == 0);
        CHECK(b.misses() == 1);
    }
}

TEST_CASE(
    "gpu effect cache: T-721 nested effects are keyed by their own anchors; an outer hit skips the inner") {
    auto listAt = [](float i, float inner) {
        CommandList cl = makeList();
        const Vec2 at{100 + i, 100};
        EffectCmd c{};
        c.effectCount = 1;
        cl.effects.push_back(dropShadow(4.0f));
        c.xf = translation(at);
        c.boundsHint = Rect{at.x, at.y, 240, 160};
        c.contentBounds = Rect{at.x + 24, at.y + 24, 192, 112};
        cl.effectCmds.push_back(c);
        cl.commands.push_back(Command{CmdKind::PushEffect, {}, 0});
        addCard(cl, Vec2{at.x + 30 + inner, at.y + 30});
        cl.commands.push_back(Command{CmdKind::PopEffect, {}, 0});
        return cl;
    };
    Built b;
    b.build(listAt(0, 0));
    CHECK(b.misses() == 2);
    b.build(listAt(1, 0));
    CHECK(b.frame.cacheStores.size() == 2);
    b.build(listAt(2, 0));  // the outer result: the inner segment is not walked
    CHECK(b.hits() == 1);
    CHECK(b.misses() == 0);
    CHECK(pastes(b.frame).size() == 1);
    // The inner card moves inside the outer effect: the outer misses, the inner one is unknown to
    // this build's predecessor (not walked), so it is drawn too.
    b.build(listAt(3, 1));
    CHECK(b.hits() == 0);
    CHECK(b.misses() == 2);
}

TEST_CASE(
    "gpu effect cache: T-721 a stencil clip level moving with the segment hits, one it moves inside does "
    "not") {
    // A multisampled target (no fringe): the rotated clip rectangle is a stencil level, whose
    // vertices are part of the key relative to the segment's anchor. The level narrows the scissor
    // to its box, which holds the segment's window with room to spare.
    auto listAt = [](float card, float clip) {
        CommandList cl = makeList();
        ClipRectCmd c{};
        c.xf = mul(translation(Vec2{60 + clip, 20}), rotationDeg(4.0f));
        c.rect = Rect{0, 0, 380, 380};
        cl.clipRects.push_back(c);
        cl.commands.push_back(Command{CmdKind::PushClipRect, {}, 0});
        addCard(cl, Vec2{180 + card, 160});
        cl.commands.push_back(Command{CmdKind::PopClip, {}, 0});
        return cl;
    };
    auto stencilLevels = [](const GpuFrame& f) {
        int n = 0;
        for (const DrawCmd& c : f.cmds) {
            n += c.stencilOp == StencilOp::ClipPush ? 1 : 0;
        }
        return n;
    };
    SUBCASE("the clip moves with the card") {
        Built b(false);
        b.build(listAt(0, 0));
        REQUIRE(stencilLevels(b.frame) >= 1);
        CHECK(b.misses() == 1);
        b.build(listAt(1, 1));
        CHECK(b.frame.cacheStores.size() == 1);
        for (float i : {2.0f, 3.0f}) {
            b.build(listAt(i, i));
            CHECK(b.hits() == 1);
            CHECK(b.misses() == 0);
            CHECK_FALSE(effectDrawn(b.frame));
        }
    }
    SUBCASE("the card moves inside a clip that stays") {
        Built b(false);
        for (int i = 0; i < 5; ++i) {
            b.build(listAt(static_cast<float>(i), 0));
            CHECK(b.hits() == 0);
            CHECK(b.misses() == 1);
            CHECK(b.frame.cacheStores.empty());
        }
    }
}
