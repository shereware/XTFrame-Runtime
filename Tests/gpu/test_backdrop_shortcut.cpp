// T-728 (Docs/design/10-backends.md §10.18 A item 3, Docs/tasks/T-728.md "測試"): a leading
// BackdropBlur whose read region lies in a layer target cleared this frame, with nothing drawn there
// yet, builds no pyramid (no copy, no Downsample / Kawase levels, no LevelMix, no Backdrop pass) and
// passes its source through. Anything drawn into the region first, a surface parent or Direct mode
// keeps the pyramid. No GPU needed: the byte-for-byte pixel checks are in
// Tests/wgpu/test_wgpu_fewer_passes.cpp and Tests/gl/test_gl_channel_fold.cpp.

#include <cstdint>
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

class ShortcutDevice : public GpuDevice {
public:
    bool supportsCoverageDraws() const override { return true; }
    bool supportsBoxShadowShading() const override { return true; }
    bool supportsAnalyticClip() const override { return true; }
    bool supportsAlphaChannelReads() const override { return true; }
    bool supportsDirectTarget() const override { return true; }
    TextureHandle createTexture(const char*, uint16_t, uint16_t, TextureFormat, const void*) override {
        return m_next++;
    }
    void updateTexture(TextureHandle, uint16_t, uint16_t, uint16_t, uint16_t, const void*, size_t) override {}
    void destroyTexture(TextureHandle) override {}
    void submit(const GpuFrame&) override {}

private:
    TextureHandle m_next = 1;
};

void fillRect(CommandList& cl, float x, float y, float w, float h, Color color) {
    PathRef ref{};
    ref.verbOffset = static_cast<uint32_t>(cl.verbs.size());
    ref.pointOffset = static_cast<uint32_t>(cl.points.size());
    const float pts[8] = {x, y, x + w, y, x + w, y + h, x, y + h};
    for (int i = 0; i < 4; ++i) {
        cl.verbs.push_back(i == 0 ? PathVerb::Move : PathVerb::Line);
        cl.points.push_back(pts[i * 2]);
        cl.points.push_back(pts[i * 2 + 1]);
    }
    cl.verbs.push_back(PathVerb::Close);
    ref.verbCount = static_cast<uint32_t>(cl.verbs.size()) - ref.verbOffset;
    ref.pointCount = static_cast<uint32_t>(cl.points.size()) - ref.pointOffset;
    FillCmd f{};
    f.path = ref;
    f.paint.kind = PaintKind::Solid;
    f.paint.color = color;
    f.rule = FillRule::NonZero;
    f.antialias = 1;
    cl.fills.push_back(f);
    cl.commands.push_back(Command{CmdKind::FillPath, {}, static_cast<uint32_t>(cl.fills.size() - 1)});
}

void pushLayer(CommandList& cl, Rect hint) {
    LayerCmd l{};
    l.opacity = 0.95f;
    l.blend = BlendMode::Normal;
    l.boundsHint = hint;
    cl.layers.push_back(l);
    cl.commands.push_back(Command{CmdKind::PushLayer, {}, static_cast<uint32_t>(cl.layers.size() - 1)});
}

/// A glass panel: BackdropBlur (radius 12, saturation and brightness set) then an outer and an inset
/// drop shadow, over a translucent white fill.
void glass(CommandList& cl, Rect box) {
    EffectCmd e{};
    e.effectOffset = static_cast<uint32_t>(cl.effects.size());
    EffectInstance blur{};
    blur.kind = EffectKind::BackdropBlur;
    blur.params[0] = 12;
    blur.params[1] = 0.3f;
    blur.params[2] = 0.1f;
    blur.enabled = 1;
    EffectInstance outer{};
    outer.kind = EffectKind::DropShadow;
    outer.params[0] = 0;
    outer.params[1] = 6;
    outer.params[2] = 12;
    outer.params[4] = 1;
    outer.colors[0] = Color{0, 0, 0, 0.5f};
    outer.enabled = 1;
    EffectInstance inset = outer;
    inset.params[5] = 1;
    for (const EffectInstance& fx : {blur, outer, inset}) {
        cl.effects.push_back(fx);
    }
    e.effectCount = 3;
    e.xf = Mat2x3{};
    e.boundsHint = box;
    e.contentBounds = box;
    cl.effectCmds.push_back(e);
    cl.commands.push_back(Command{CmdKind::PushEffect, {}, static_cast<uint32_t>(cl.effectCmds.size() - 1)});
    fillRect(cl, box.x, box.y, box.w, box.h, Color{1, 1, 1, 0.3f});
    cl.commands.push_back(Command{CmdKind::PopEffect, {}, 0});
}

CommandList baseList() {
    CommandList cl;
    cl.viewport = Rect{0, 0, kW, kH};
    cl.devicePixelRatio = 1;
    fillRect(cl, 0, 0, kW, kH, Color{0.2f, 0.3f, 0.45f, 1});
    return cl;
}

struct Counts {
    size_t passes = 0;
    uint32_t copies = 0, backdrops = 0, kawases = 0;  ///< Kawase: pyramid levels only
};

Counts build(const CommandList& cl) {
    ShortcutDevice device;
    FrameBuilder builder;
    builder.init(device);
    builder.resize(kW, kH);
    GpuFrame frame;
    builder.build(cl, nullptr, nullptr, nullptr, frame);
    Counts n;
    n.passes = frame.passes.size();
    for (const Pass& p : frame.passes) {
        n.copies += p.copy != 0 ? 1u : 0u;
    }
    for (const DrawCmd& c : frame.cmds) {
        if (c.shading == Shading::Effect) {
            n.backdrops += c.program == EffectProgram::Backdrop ? 1u : 0u;
            n.kawases += c.program == EffectProgram::Kawase ? 1u : 0u;
        }
    }
    builder.shutdown();
    return n;
}

const Rect kGlass{100, 60, 120, 80};

}  // namespace

TEST_CASE("gpu backdrop shortcut: a glass panel on an empty layer builds no pyramid (T-728 item 3)") {
    CommandList cl = baseList();
    pushLayer(cl, Rect{0, 0, kW, kH});
    glass(cl, kGlass);
    fillRect(cl, 10, 10, 40, 40, Color{1, 0, 0, 1});  // drawn after the panel: irrelevant
    cl.commands.push_back(Command{CmdKind::PopLayer, {}, 0});
    const Counts n = build(cl);
    CHECK(n.copies == 0u);
    CHECK(n.backdrops == 0u);
    CHECK(n.kawases == 0u);
}

TEST_CASE("gpu backdrop shortcut: a drawing in the region read keeps the pyramid (T-728 item 3)") {
    for (const float alpha : {1.0f, 0.0f}) {
        CAPTURE(alpha);
        CommandList cl = baseList();
        pushLayer(cl, Rect{0, 0, kW, kH});
        // Under the panel's blur margin (not under the panel itself); even a fully transparent fill
        // counts -- the check looks at commands, not at what they leave.
        fillRect(cl, kGlass.x - 20, kGlass.y + 10, 16, 16, Color{0, 1, 0, alpha});
        glass(cl, kGlass);
        cl.commands.push_back(Command{CmdKind::PopLayer, {}, 0});
        const Counts n = build(cl);
        CHECK(n.copies == 1u);
        CHECK(n.backdrops == 1u);
        CHECK(n.kawases >= 1u);
    }
}

TEST_CASE("gpu backdrop shortcut: a drawing outside the region read still takes it (T-728 item 3)") {
    CommandList with = baseList();
    pushLayer(with, Rect{0, 0, kW, kH});
    fillRect(with, 4, 4, 20, 20, Color{0, 1, 0, 1});  // far from the panel and its blur margin
    glass(with, kGlass);
    with.commands.push_back(Command{CmdKind::PopLayer, {}, 0});
    const Counts n = build(with);
    CHECK(n.copies == 0u);
    CHECK(n.backdrops == 0u);
}

TEST_CASE("gpu backdrop shortcut: a surface parent and Direct mode keep the pyramid (T-728 item 3)") {
    SUBCASE("the panel straight on the surface") {
        CommandList cl;
        cl.viewport = Rect{0, 0, kW, kH};
        cl.devicePixelRatio = 1;
        glass(cl, kGlass);  // nothing drawn on the surface either: it is not a cleared transient
        const Counts n = build(cl);
        CHECK(n.copies == 1u);
        CHECK(n.backdrops == 1u);
    }
    SUBCASE("Direct mode, the panel in an empty layer") {
        CommandList cl = baseList();
        cl.hostTargetMode = HostTargetMode::Direct;
        pushLayer(cl, Rect{0, 0, kW, kH});
        glass(cl, kGlass);
        cl.commands.push_back(Command{CmdKind::PopLayer, {}, 0});
        const Counts n = build(cl);
        CHECK(n.copies >= 1u);
        CHECK(n.backdrops == 1u);
    }
    SUBCASE("a region read that its padding pushes out of the layer's window") {
        CommandList cl = baseList();
        // The layer's window is 128 x 96 at (100, 60); the panel sits in its bottom-right corner, so
        // the region read (cut at the window) is padded to 64 px past the window's edges.
        pushLayer(cl, Rect{100, 60, 120, 80});
        glass(cl, Rect{196, 124, 30, 30});
        cl.commands.push_back(Command{CmdKind::PopLayer, {}, 0});
        const Counts n = build(cl);
        CHECK(n.copies == 1u);
        CHECK(n.backdrops == 1u);
    }
}
