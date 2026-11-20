// T-754 (Docs/design/10-backends.md §10.20, Docs/tasks/T-754.md "測試"): the frame builder's half of
// "outside the viewport is not drawn" -- a known, empty hint skips its section (no pass, no backdrop
// copy, counted in BuildStats::culledOffscreen); only an unknown, empty hint takes the whole-surface
// fallback, whose result is never kept in the effect cache; and a card painted by the Painter while it
// slides out of the viewport stops costing passes and cache bytes once it is out.

#include <cstdint>
#include <vector>

#include "doctest.h"
#include "xt/command_list.h"
#include "xt/gpu/frame_builder.h"
#include "xt/gpu/gpu_device.h"
#include "xt/gpu/gpu_frame.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/painter.h"
#include "xt/property.h"
#include "xt/types.h"
#include "xt/value.h"

using namespace xt;
using namespace xt::gpu;

namespace {

constexpr int kW = 320;
constexpr int kH = 200;

/// Hands out handles and counts the cached targets it holds; executes nothing.
class CullDevice : public GpuDevice {
public:
    bool supportsCoverageDraws() const override { return true; }
    bool supportsBoxShadowShading() const override { return true; }
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
    Built() {
        builder.init(device);
        builder.resize(kW, kH);
    }
    ~Built() { builder.shutdown(); }
    void build(const CommandList& cl) { builder.build(cl, nullptr, nullptr, nullptr, frame); }
    /// Passes drawing into a transient target (everything but the surface).
    size_t offscreenPasses() const {
        size_t n = 0;
        for (const Pass& p : frame.passes) {
            n += p.target != kSurfaceTarget ? 1u : 0u;
        }
        return n;
    }
    CullDevice device;
    FrameBuilder builder;
    GpuFrame frame;
};

CommandList makeList() {
    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(kW), static_cast<float>(kH)};
    cl.devicePixelRatio = 1;
    return cl;
}

void fillRect(CommandList& cl, Rect r, Color color) {
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
    ref.verbCount = 5;
    ref.pointCount = 8;
    FillCmd f{};
    f.path = ref;
    f.paint.kind = PaintKind::Solid;
    f.paint.color = color;
    f.rule = FillRule::NonZero;
    f.antialias = 1;
    cl.fills.push_back(f);
    cl.commands.push_back(Command{CmdKind::FillPath, {}, static_cast<uint32_t>(cl.fills.size() - 1)});
}

EffectInstance shadowFx() {
    EffectInstance e{};
    e.kind = EffectKind::DropShadow;
    e.enabled = 1;
    e.params[0] = 4;
    e.params[1] = 4;
    e.params[2] = 8;
    e.params[4] = 1;
    e.colors[0] = Color{0, 0, 0, 0.8f};
    return e;
}

/// One effect section: PushEffect(`fx`, `hint`, `known`) around a fill of `content`.
void addEffect(CommandList& cl, EffectInstance fx, Rect hint, Rect content, uint8_t known) {
    EffectCmd c{};
    c.effectOffset = static_cast<uint32_t>(cl.effects.size());
    c.effectCount = 1;
    cl.effects.push_back(fx);
    c.boundsHint = hint;
    c.contentBounds = content;
    c.boundsKnown = known;
    cl.effectCmds.push_back(c);
    Command push{CmdKind::PushEffect, {}, static_cast<uint32_t>(cl.effectCmds.size() - 1)};
    if (fx.kind == EffectKind::BackdropBlur) {
        push.flags[0] = kCommandSceneRead;
    }
    cl.commands.push_back(push);
    fillRect(cl, content, Color{0.2f, 0.4f, 0.9f, 1});
    cl.commands.push_back(Command{CmdKind::PopEffect, {}, 0});
}

/// A 60 x 40 card with `shadow(4px 4px 8px)` (reach 4 + 3 x 4 = 16 px) at `x` in a 320 x 200 viewport,
/// painted by the Painter.
CommandList paintedCard(float x) {
    NodeArena arena;
    const NodeId root = arena.create(NodeKind::Element);
    arena.element(root).box.width = kW;
    arena.element(root).box.height = kH;
    const NodeId card = arena.create(NodeKind::Element);
    arena.appendChild(root, card);
    ElementData& data = arena.element(card);
    data.box.width = 60;
    data.box.height = 40;
    data.style.base[prop::BackgroundColor] = Value::ofColor(Color{0.2f, 0.4f, 0.9f, 1});
    arena.common(card).translate = Vec2{x, 80};
    const EffectInstance fx = shadowFx();
    REQUIRE(arena.assignEffects(card, std::span<const EffectInstance>(&fx, 1), {}) == Status::kOk);
    updateWorldTransforms(arena, root, /*force=*/true);
    CommandList cl = makeList();
    Painter painter;
    painter.setBoxShadowCommands(false);  // keep the PushEffect (T-668 would make it a DrawBoxShadow)
    painter.paint(arena, root, cl);
    return cl;
}

size_t effectSections(const CommandList& cl) {
    size_t n = 0;
    for (const Command& c : cl.commands) {
        n += c.kind == CmdKind::PushEffect ? 1u : 0u;
    }
    return n;
}

}  // namespace

TEST_CASE("gpu offscreen cull: a known, empty effect or layer section is skipped (T-754)") {
    for (const bool known : {false, true}) {
        CAPTURE(known);
        CommandList cl = makeList();
        fillRect(cl, Rect{0, 0, kW, kH}, Color{0.9f, 0.9f, 0.9f, 1});
        addEffect(cl, shadowFx(), Rect{0, 40, 0, 72}, Rect{-90, 56, 60, 40}, known ? 1 : 0);
        LayerCmd layer{0.5f, BlendMode::Normal, Rect{40, 0, 80, 0}, static_cast<uint8_t>(known ? 1 : 0)};
        cl.layers.push_back(layer);
        cl.commands.push_back(Command{CmdKind::PushLayer, {}, 0});
        fillRect(cl, Rect{40, -60, 80, 30}, Color{0, 1, 0, 1});
        cl.commands.push_back(Command{CmdKind::PopLayer, {}, 0});
        Built b;
        b.build(cl);
        if (known) {
            CHECK(b.frame.passes.size() == 1u);  // the surface only
            CHECK(b.offscreenPasses() == 0u);
            CHECK(b.builder.lastBuildStats().culledOffscreen == 2u);
        } else {
            // The old meaning: the whole surface as the window of both sections.
            CHECK(b.offscreenPasses() >= 2u);
            CHECK(b.builder.lastBuildStats().culledOffscreen == 0u);
        }
    }
}

TEST_CASE("gpu offscreen cull: a glass panel outside the viewport copies no backdrop (T-754)") {
    EffectInstance glass{};
    glass.kind = EffectKind::BackdropBlur;
    glass.enabled = 1;
    glass.params[0] = 8;
    glass.params[1] = 1;
    glass.params[2] = 1;
    CommandList cl = makeList();
    fillRect(cl, Rect{0, 0, kW, kH}, Color{0.9f, 0.9f, 0.9f, 1});
    addEffect(cl, glass, Rect{kW, 20, 0, 60}, Rect{kW + 10.0f, 20, 60, 60}, 1);
    Built b;
    b.build(cl);
    CHECK(b.frame.passes.size() == 1u);
    for (const Pass& p : b.frame.passes) {
        CHECK(p.copy == 0);
    }
    CHECK(b.frame.transients.empty());
}

TEST_CASE("gpu offscreen cull: a result drawn in the whole-surface fallback window is not cached (T-754)") {
    // An unknown, empty hint (a hand-made list, or inside a 3D space): drawn in the whole surface,
    // repeated frame after frame -- never stored.
    {
        CommandList cl = makeList();
        addEffect(cl, shadowFx(), Rect{0, 0, 0, 0}, Rect{20, 20, 60, 40}, 0);
        Built b;
        for (int f = 0; f < 4; ++f) {
            b.build(cl);
            CHECK(b.builder.lastEffectCacheStats().stores == 0u);
        }
        CHECK(b.device.cachedAlive == 0);
        CHECK(b.builder.lastBuildStats().effectCacheBytes == 0u);
    }
    // The same segment with its real window is stored on its second appearance, as before.
    {
        CommandList cl = makeList();
        addEffect(cl, shadowFx(), Rect{4, 4, 92, 72}, Rect{20, 20, 60, 40}, 1);
        Built b;
        for (int f = 0; f < 4; ++f) {
            b.build(cl);
        }
        CHECK(b.device.cachedAlive == 1);
        CHECK(b.builder.lastBuildStats().effectCacheBytes > 0u);
    }
}

TEST_CASE("gpu offscreen cull: a card sliding out of the viewport stops costing passes and cache (T-754)") {
    Built b;
    uint64_t inViewBytes = 0;
    uint64_t lastBytes = 0;
    bool wasOut = false;
    // From fully inside to fully outside on the left (the shadow's reach is 16 px), then back.
    const float xs[] = {100, 100, 100, 40, 0, -40, -60, -75, -77, -80, -120, -120, -120, -76, -60, 100};
    for (const float x : xs) {
        CAPTURE(x);
        const CommandList cl = paintedCard(x);
        b.build(cl);
        const bool out = x + 60 + 16 <= 0;  // the shadow's reach ends at or left of the viewport
        CHECK(effectSections(cl) == (out ? 0u : 1u));
        CHECK(cl.culledOffscreen == (out ? 1u : 0u));
        const uint64_t bytes = b.builder.lastBuildStats().effectCacheBytes;
        if (x == 100 && !wasOut) {
            inViewBytes = bytes;
        }
        if (out) {
            CHECK(b.offscreenPasses() == 0u);
            CHECK(bytes <= lastBytes);  // never grows while the card is out
            // No window ever reaches the whole surface (the old fallback).
        } else {
            for (const Pass& p : b.frame.passes) {
                if (p.target != kSurfaceTarget) {
                    CHECK(static_cast<int>(p.viewW) * static_cast<int>(p.viewH) < kW * kH / 2);
                }
            }
        }
        wasOut = wasOut || out;
        lastBytes = bytes;
    }
    CHECK(inViewBytes > 0u);
    CHECK(lastBytes <= inViewBytes);
}
