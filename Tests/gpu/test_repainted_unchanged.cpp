// T-753 (Docs/design/57-measurement.md §57.3 item 4, `repaintedUnchanged`): effect segments and layers
// the frame builder drew in full although the hash of their commands is one of the previous build's,
// by reason -- on the frame builder alone, against a device that hands out handles.

#include <cstdint>
#include <vector>

#include "doctest.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/compile/ir_writer.h"
#include "xt/gpu/frame_builder.h"
#include "xt/gpu/gpu_device.h"
#include "xt/gpu/gpu_frame.h"
#include "xt/perf.h"
#include "xt/types.h"

using namespace xt;
using namespace xt::gpu;

namespace {

constexpr int kSurface = 256;

/// Hands out handles (cached targets too, unless `cache` is false); executes nothing.
class HandleDevice : public GpuDevice {
public:
    bool cache = true;
    TextureHandle createTexture(const char*, uint16_t, uint16_t, TextureFormat, const void*) override {
        return m_next++;
    }
    void updateTexture(TextureHandle, uint16_t, uint16_t, uint16_t, uint16_t, const void*, size_t) override {}
    void destroyTexture(TextureHandle) override {}
    void submit(const GpuFrame&) override {}
    TextureHandle acquireCachedTarget(uint16_t, uint16_t, TextureFormat) override {
        return cache ? m_next++ : 0;
    }
    void releaseCachedTarget(TextureHandle) override {}

private:
    TextureHandle m_next = 1;
};

struct Rig {
    explicit Rig(int detail, bool cache = true) {
        device.cache = cache;
        builder.init(device);
        builder.resize(kSurface, kSurface);
        perf.setEnabled(true);
        perf.setDetail(detail);
    }
    ~Rig() { builder.shutdown(); }
    /// One frame's build; returns its counts (not cacheable, key changed, budget, first sight).
    std::vector<uint32_t> build(const CommandList& cl, AssetResolver* assets = nullptr,
                                const IrView* ir = nullptr) {
        perf.beginFrame(0.0f);
        builder.build(cl, nullptr, assets, ir, frame, &perf);
        const FrameStats& f = perf.current();
        return {f.repaintedUnchangedNotCacheable, f.repaintedUnchangedKeyChanged, f.repaintedUnchangedBudget,
                f.repaintedUnchangedFirstSight};
    }
    HandleDevice device;
    FrameBuilder builder;
    GpuFrame frame;
    PerfTracker perf;
};

using Counts = std::vector<uint32_t>;

CommandList makeList() {
    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(kSurface), static_cast<float>(kSurface)};
    cl.devicePixelRatio = 1;
    return cl;
}

void fillRect(CommandList& cl, Rect r) {
    FillCmd f{};
    f.path.verbOffset = static_cast<uint32_t>(cl.verbs.size());
    f.path.pointOffset = static_cast<uint32_t>(cl.points.size());
    const float pts[8] = {r.x, r.y, r.x + r.w, r.y, r.x + r.w, r.y + r.h, r.x, r.y + r.h};
    for (int i = 0; i < 4; ++i) {
        cl.verbs.push_back(i == 0 ? PathVerb::Move : PathVerb::Line);
        cl.points.push_back(pts[i * 2]);
        cl.points.push_back(pts[i * 2 + 1]);
    }
    cl.verbs.push_back(PathVerb::Close);
    f.path.verbCount = static_cast<uint32_t>(cl.verbs.size()) - f.path.verbOffset;
    f.path.pointCount = static_cast<uint32_t>(cl.points.size()) - f.path.pointOffset;
    f.paint.kind = PaintKind::Solid;
    f.paint.color = Color{1, 0.2f, 0.1f, 1};
    f.rule = FillRule::NonZero;
    f.antialias = 1;
    cl.fills.push_back(f);
    cl.commands.push_back(Command{CmdKind::FillPath, {}, static_cast<uint32_t>(cl.fills.size() - 1)});
}

/// A 40 px square with a drop shadow (or a backdrop blur), its effect bounds 24 px around it.
void addEffect(CommandList& cl, bool backdrop = false) {
    EffectInstance e{};
    if (backdrop) {
        e.kind = EffectKind::BackdropBlur;
        e.params[0] = 6;
        e.enabled = 1;
    } else {
        e.kind = EffectKind::DropShadow;
        e.params[0] = 4;
        e.params[1] = 4;
        e.params[2] = 6;
        e.params[4] = 0.8f;
        e.colors[0] = Color{0, 0, 0, 1};
    }
    EffectCmd c{};
    c.effectOffset = static_cast<uint32_t>(cl.effects.size());
    c.effectCount = 1;
    cl.effects.push_back(e);
    c.boundsHint = Rect{40, 40, 88, 88};
    c.boundsKnown = 1;
    c.contentBounds = Rect{64, 64, 40, 40};
    cl.effectCmds.push_back(c);
    cl.commands.push_back(Command{CmdKind::PushEffect, {}, static_cast<uint32_t>(cl.effectCmds.size() - 1)});
    fillRect(cl, Rect{64, 64, 40, 40});
    cl.commands.push_back(Command{CmdKind::PopEffect, {}, 0});
}

void addLayer(CommandList& cl) {
    cl.layers.push_back(LayerCmd{0.5f, BlendMode::Normal, Rect{0, 0, 120, 120}, 1});
    cl.commands.push_back(Command{CmdKind::PushLayer, {}, static_cast<uint32_t>(cl.layers.size() - 1)});
    fillRect(cl, Rect{10, 10, 100, 100});
    cl.commands.push_back(Command{CmdKind::PopLayer, {}, 0});
}

/// T-746: resolves nothing but the external bindings of the base class.
class ExternalResolver : public AssetResolver {
public:
    bool readBytes(std::string_view, std::vector<uint8_t>&) override { return false; }
    bool decodeImage(AssetId, const IrView&, ImageData&) override { return false; }
    Vec2 imageSize(AssetId, const IrView&) override { return Vec2{0, 0}; }
};

/// T-746: an IR whose asset 0 is `engine://scene`.
std::vector<uint8_t> irWithEngineAsset() {
    compile::IrWriter writer;
    compile::AssetBuilder asset;
    asset.name = writer.intern("engine://scene");
    asset.kind = 0;
    writer.addAsset(asset);
    compile::SceneBuilder scene;
    scene.width = 64;
    scene.height = 64;
    writer.addScene(scene);
    return writer.finish();
}

/// T-746: addEffect() with the host texture asset 0 drawn inside it instead of the fill.
void addExternalEffect(CommandList& cl) {
    addEffect(cl);
    cl.commands.pop_back();  // PopEffect
    ImageCmd img{};
    img.xf = Mat2x3{};
    img.asset = 0;
    img.dst = Rect{64, 64, 40, 40};
    img.srcUv = Rect{0, 0, 1, 1};
    img.opacity = 1;
    img.smooth = 1;
    cl.images.push_back(img);
    cl.commands.push_back(Command{CmdKind::DrawImage, {}, static_cast<uint32_t>(cl.images.size() - 1)});
    cl.commands.push_back(Command{CmdKind::PopEffect, {}, 0});
}

void pushClip(CommandList& cl, Rect r) {
    ClipRectCmd c{};
    c.rect = r;
    cl.clipRects.push_back(c);
    cl.commands.push_back(Command{CmdKind::PushClipRect, {}, static_cast<uint32_t>(cl.clipRects.size() - 1)});
}

}  // namespace

TEST_CASE("repaintedUnchanged: an effect segment's first repeat is stored, then served from the cache") {
    Rig rig(1);
    CommandList cl = makeList();
    addEffect(cl);
    CHECK(rig.build(cl) == Counts{0, 0, 0, 0});  // first sight: nothing to compare with
    CHECK(rig.build(cl) == Counts{0, 0, 0, 1});  // drawn again, stored now
    CHECK(rig.builder.lastEffectCacheStats().stores == 1u);
    CHECK(rig.build(cl) == Counts{0, 0, 0, 0});  // pasted from the cache: not drawn
    CHECK(rig.builder.lastEffectCacheStats().hits == 1u);
}

TEST_CASE("repaintedUnchanged: layers and backdrop readers are not cacheable") {
    Rig rig(1);
    CommandList cl = makeList();
    addLayer(cl);
    addEffect(cl, /*backdrop=*/true);
    rig.build(cl);
    CHECK(rig.build(cl) == Counts{2, 0, 0, 0});
    CHECK(rig.build(cl) == Counts{2, 0, 0, 0});
}

TEST_CASE("repaintedUnchanged: no cache, a full cache, and a cache key changed by the clip around") {
    {
        Rig rig(1, /*cache=*/false);  // the device never caches (FrameBuilder::init's probe sees it)
        CommandList cl = makeList();
        addEffect(cl);
        rig.build(cl);
        CHECK(rig.build(cl) == Counts{1, 0, 0, 0});
    }
    {
        Rig rig(1);
        rig.device.cache = false;  // caches, but refuses this target: the store does not happen
        CommandList cl = makeList();
        addEffect(cl);
        rig.build(cl);
        CHECK(rig.build(cl) == Counts{0, 0, 1, 0});
    }
    Rig rig(1);
    CommandList a = makeList();
    pushClip(a, Rect{0, 0, 90, 90});  // cuts the segment's window: its key holds the scissor
    addEffect(a);
    a.commands.push_back(Command{CmdKind::PopClip, {}, 0});
    CommandList b = makeList();
    pushClip(b, Rect{0, 0, 100, 100});
    addEffect(b);
    b.commands.push_back(Command{CmdKind::PopClip, {}, 0});
    rig.build(a);
    CHECK(rig.build(b) == Counts{0, 1, 0, 0});  // same commands, another key: not seen last build
}

TEST_CASE("repaintedUnchanged: nothing is counted at detail level 0 or with a changed segment") {
    {
        Rig rig(0);
        CommandList cl = makeList();
        addLayer(cl);
        rig.build(cl);
        CHECK(rig.build(cl) == Counts{0, 0, 0, 0});
    }
    Rig rig(1);
    CommandList a = makeList();
    addLayer(a);
    CommandList b = makeList();
    b.layers.push_back(LayerCmd{0.5f, BlendMode::Normal, Rect{0, 0, 120, 120}, 1});
    b.commands.push_back(Command{CmdKind::PushLayer, {}, 0});
    fillRect(b, Rect{12, 10, 100, 100});  // moved by 2 px: other commands
    b.commands.push_back(Command{CmdKind::PopLayer, {}, 0});
    rig.build(a);
    CHECK(rig.build(b) == Counts{0, 0, 0, 0});
}

TEST_CASE(
    "repaintedUnchanged: T-746 an effect over a host texture is cacheable once the texture has a version") {
    const std::vector<uint8_t> irBytes = irWithEngineAsset();
    Result<IrView> view = IrView::open(IrBlob{irBytes.data(), irBytes.size()});
    REQUIRE(view.ok());
    CommandList cl = makeList();
    addExternalEffect(cl);
    ExternalResolver assets;
    assets.setExternalTexture("scene", 42, 40, 40);
    {
        // Without a version (as before T-746): never cached, counted as not cacheable every frame.
        Rig rig(1);
        rig.build(cl, &assets, &view.value());
        CHECK(rig.build(cl, &assets, &view.value()) == Counts{1, 0, 0, 0});
        CHECK(rig.build(cl, &assets, &view.value()) == Counts{1, 0, 0, 0});
        CHECK(rig.builder.lastEffectCacheStats().hits == 0u);
        CHECK(rig.builder.lastFrameReusable(&assets, &view.value()));
    }
    assets.setExternalTextureVersion("scene", 1);
    Rig rig(1);
    CHECK(rig.build(cl, &assets, &view.value()) == Counts{0, 0, 0, 0});
    CHECK(rig.build(cl, &assets, &view.value()) == Counts{0, 0, 0, 1});  // stored now
    CHECK(rig.builder.lastEffectCacheStats().stores == 1u);
    CHECK(rig.build(cl, &assets, &view.value()) == Counts{0, 0, 0, 0});  // from the cache
    CHECK(rig.builder.lastEffectCacheStats().hits == 1u);
    CHECK(rig.builder.lastFrameReusable(&assets, &view.value()));
    // A new version: the frame (which pastes the old result) is not reusable, the segment misses.
    assets.setExternalTextureVersion("scene", 2);
    CHECK_FALSE(rig.builder.lastFrameReusable(&assets, &view.value()));
    rig.build(cl, &assets, &view.value());
    CHECK(rig.builder.lastEffectCacheStats().hits == 0u);
    CHECK(rig.builder.lastEffectCacheStats().misses == 1u);
    // A rebind under the same version is a change too.
    rig.build(cl, &assets, &view.value());
    rig.build(cl, &assets, &view.value());
    CHECK(rig.builder.lastEffectCacheStats().hits == 1u);
    assets.setExternalTexture("scene", 43, 40, 40);
    CHECK_FALSE(rig.builder.lastFrameReusable(&assets, &view.value()));
    rig.build(cl, &assets, &view.value());
    CHECK(rig.builder.lastEffectCacheStats().hits == 0u);
    CHECK(rig.builder.lastEffectCacheStats().misses == 1u);
}
