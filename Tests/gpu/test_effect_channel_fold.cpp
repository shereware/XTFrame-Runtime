// T-728 (Docs/design/10-backends.md §10.18 A item 1, Docs/tasks/T-728.md "測試"): the AlphaExtract
// that starts a shadow / glow chain folds into the Downsample / GaussScalar pass after it on a device
// whose supportsAlphaChannelReads() is true -- one pass less, the first blur pass reading the effect's
// source through GaussParams / DownsampleParams `channel`. Against a device answering false the frame
// is the one of before (the AlphaExtract pass, the blocks without the field; the byte-for-byte frame
// snapshots of test_gpu_frame.cpp use such a device). No GPU needed: the pixel checks are in
// Tests/wgpu/test_wgpu_channel_fold.cpp and Tests/gl/test_gl_channel_fold.cpp. T-735: only the
// `.a` chains (outer shadow, glow) fold; inset shadows and inner glows (`1 - .a`) keep the pass.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "doctest.h"
#include "xt/command_list.h"
#include "xt/gpu/frame_builder.h"
#include "xt/gpu/gpu_device.h"
#include "xt/gpu/gpu_frame.h"

using namespace xt;
using namespace xt::gpu;

namespace {

/// Hands out texture handles; executes nothing. Draws what xt_gl / xt_wgpu draw, except that the
/// channel reads are reported as asked.
class ChannelDevice : public GpuDevice {
public:
    explicit ChannelDevice(bool channels) : m_channels(channels) {}
    bool supportsCoverageDraws() const override { return true; }
    bool supportsBoxShadowShading() const override { return true; }
    bool supportsAnalyticClip() const override { return true; }
    bool supportsAlphaChannelReads() const override { return m_channels; }
    TextureHandle createTexture(const char*, uint16_t, uint16_t, TextureFormat, const void*) override {
        return m_next++;
    }
    void updateTexture(TextureHandle, uint16_t, uint16_t, uint16_t, uint16_t, const void*, size_t) override {}
    void destroyTexture(TextureHandle) override {}
    void submit(const GpuFrame&) override {}

private:
    bool m_channels;
    TextureHandle m_next = 1;
};

PathRef rectPath(CommandList& cl, float x, float y, float w, float h) {
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
    return ref;
}

/// A 128 x 128 surface; one effect group (hint = the surface, content 32..96) holding a red square.
CommandList effectList(std::initializer_list<EffectInstance> effects) {
    CommandList cl;
    cl.viewport = Rect{0, 0, 128, 128};
    cl.devicePixelRatio = 1;
    EffectCmd e{};
    e.effectOffset = 0;
    e.effectCount = static_cast<uint32_t>(effects.size());
    for (const EffectInstance& fx : effects) {
        cl.effects.push_back(fx);
    }
    e.xf = Mat2x3{};
    e.boundsHint = Rect{0, 0, 128, 128};
    e.contentBounds = Rect{32, 32, 64, 64};
    cl.effectCmds.push_back(e);
    cl.commands.push_back(Command{CmdKind::PushEffect, {}, 0});
    FillCmd f{};
    f.path = rectPath(cl, 32, 32, 64, 64);
    f.paint.kind = PaintKind::Solid;
    f.paint.color = Color{1, 0, 0, 1};
    f.rule = FillRule::NonZero;
    f.antialias = 1;
    cl.fills.push_back(f);
    cl.commands.push_back(Command{CmdKind::FillPath, {}, 0});
    cl.commands.push_back(Command{CmdKind::PopEffect, {}, 0});
    return cl;
}

EffectInstance dropShadow(float blur, float spread, bool inset) {
    EffectInstance e{};
    e.kind = EffectKind::DropShadow;
    e.params[0] = 3;
    e.params[1] = 4;
    e.params[2] = blur;
    e.params[3] = spread;
    e.params[4] = 1;
    e.params[5] = inset ? 1.0f : 0.0f;
    e.colors[0] = Color{0, 0, 0, 0.8f};
    e.enabled = 1;
    return e;
}

EffectInstance glow(float radius, bool inner) {
    EffectInstance e{};
    e.kind = EffectKind::Glow;
    e.params[0] = radius;
    e.params[1] = 1;
    e.params[2] = inner ? 1.0f : 0.0f;
    e.colors[0] = Color{1, 0.9f, 0.2f, 1};
    e.enabled = 1;
    return e;
}

struct Built {
    GpuFrame frame;
    std::vector<const DrawCmd*> effects;  ///< the Shading::Effect commands in order
};

void build(const CommandList& cl, bool channels, Built& out) {
    ChannelDevice device(channels);
    FrameBuilder builder;
    builder.init(device);
    builder.resize(128, 128);
    builder.build(cl, nullptr, nullptr, nullptr, out.frame);
    for (const DrawCmd& c : out.frame.cmds) {
        if (c.shading == Shading::Effect) {
            out.effects.push_back(&c);
        }
    }
    builder.shutdown();
}

std::vector<EffectProgram> programs(const Built& b) {
    std::vector<EffectProgram> out;
    for (const DrawCmd* c : b.effects) {
        out.push_back(c->program);
    }
    return out;
}

GaussParams gaussAt(const Built& b, size_t i) {
    GaussParams p{};
    std::memcpy(static_cast<void*>(&p), b.frame.params.data() + b.effects[i]->paramIndex, sizeof(p));
    return p;
}

DownsampleParams downsampleAt(const Built& b, size_t i) {
    DownsampleParams p{};
    std::memcpy(static_cast<void*>(&p), b.frame.params.data() + b.effects[i]->paramIndex, sizeof(p));
    return p;
}

/// The effect group's own layer: what the first effect pass of the unfolded frame reads.
TargetHandle sourceOf(const Built& unfolded) { return unfolded.effects.front()->src; }

/// T-735: a chain that reads `1 - alpha` is not folded -- the frame keeps the AlphaExtract pass,
/// the same programs and passes as on a device without channel reads, and no pass reads a channel.
void checkNotFolded(const Built& before, const Built& after) {
    using P = EffectProgram;
    REQUIRE(!after.effects.empty());
    CHECK(after.effects.front()->program == P::AlphaExtract);
    CHECK(programs(after) == programs(before));
    CHECK(after.frame.passes.size() == before.frame.passes.size());
    for (size_t i = 0; i < after.effects.size(); ++i) {
        if (after.effects[i]->program == P::GaussScalar) {
            CHECK(gaussAt(after, i).channel == 0.0f);
        } else if (after.effects[i]->program == P::Downsample) {
            CHECK(downsampleAt(after, i).channel == 0.0f);
        }
    }
}

}  // namespace

TEST_CASE("gpu channel fold: an outer drop shadow reads alpha in its first GaussScalar pass (T-728)") {
    using P = EffectProgram;
    const CommandList cl = effectList({dropShadow(6, 0, false)});
    Built before, after;
    build(cl, false, before);
    build(cl, true, after);
    CHECK(programs(before) == std::vector<P>{P::AlphaExtract, P::GaussScalar, P::GaussScalar, P::ShadowGlow});
    CHECK(programs(after) == std::vector<P>{P::GaussScalar, P::GaussScalar, P::ShadowGlow});
    CHECK(after.frame.passes.size() + 1 == before.frame.passes.size());
    REQUIRE(after.effects.size() == 3);
    const GaussParams first = gaussAt(after, 0);
    CHECK(first.channel == 1.0f);
    CHECK(first.constEdge == 0.0f);
    CHECK(first.dirX == 1.0f);
    CHECK(after.effects[0]->src == sourceOf(before));  // the color source itself
    CHECK(gaussAt(after, 1).channel == 0.0f);
    CHECK(after.effects[2]->src == sourceOf(before));  // ShadowGlow still reads the source
    // The same taps as the unfolded chain's horizontal pass.
    GaussParams unfolded{};
    std::memcpy(static_cast<void*>(&unfolded), before.frame.params.data() + before.effects[1]->paramIndex,
                offsetof(GaussParams, channel));
    CHECK(std::memcmp(&unfolded, &first, offsetof(GaussParams, channel)) == 0);
}

TEST_CASE("gpu channel fold: an inset drop shadow keeps its AlphaExtract pass (T-735)") {
    using P = EffectProgram;
    const CommandList cl = effectList({dropShadow(6, 0, true)});
    Built before, after;
    build(cl, false, before);
    build(cl, true, after);
    CHECK(programs(before) == std::vector<P>{P::AlphaExtract, P::GaussScalar, P::GaussScalar, P::ShadowGlow});
    checkNotFolded(before, after);
    REQUIRE(after.effects.size() == 4);
    CHECK(gaussAt(after, 1).constEdge == 1.0f);
    CHECK(gaussAt(after, 1).outside == 1.0f);
}

TEST_CASE("gpu channel fold: a glow reads alpha; an inner glow keeps its AlphaExtract pass (T-728, T-735)") {
    using P = EffectProgram;
    {
        const CommandList cl = effectList({glow(8, false)});
        Built before, after;
        build(cl, false, before);
        build(cl, true, after);
        CHECK(programs(before) ==
              std::vector<P>{P::AlphaExtract, P::GaussScalar, P::GaussScalar, P::ShadowGlow});
        CHECK(programs(after) == std::vector<P>{P::GaussScalar, P::GaussScalar, P::ShadowGlow});
        CHECK(after.frame.passes.size() + 1 == before.frame.passes.size());
        REQUIRE(after.effects.size() == 3);
        CHECK(gaussAt(after, 0).channel == 1.0f);
        CHECK(gaussAt(after, 0).constEdge == 0.0f);
        CHECK(after.effects[0]->src == sourceOf(before));
        CHECK(gaussAt(after, 1).channel == 0.0f);
    }
    {
        const CommandList cl = effectList({glow(8, true)});
        Built before, after;
        build(cl, false, before);
        build(cl, true, after);
        CHECK(programs(before) ==
              std::vector<P>{P::AlphaExtract, P::GaussScalar, P::GaussScalar, P::ShadowGlow});
        checkNotFolded(before, after);
    }
}

TEST_CASE("gpu channel fold: a downsampled blur reads the channel in its first Downsample only (T-728)") {
    using P = EffectProgram;
    // blur 40 = sigma 20: an eighth of the resolution, three Downsample passes.
    {
        // T-735: the inset chain (1 - alpha) is not folded.
        const CommandList cl = effectList({dropShadow(40, 0, true)});
        Built before, after;
        build(cl, false, before);
        build(cl, true, after);
        checkNotFolded(before, after);
    }
    {
        const CommandList cl = effectList({dropShadow(40, 0, false)});
        Built before, after;
        build(cl, false, before);
        build(cl, true, after);
        REQUIRE(programs(before).front() == P::AlphaExtract);
        std::vector<P> expect = programs(before);
        expect.erase(expect.begin());
        CHECK(programs(after) == expect);
        CHECK(after.frame.passes.size() + 1 == before.frame.passes.size());
        REQUIRE(after.effects.size() >= 4);
        REQUIRE(after.effects[0]->program == P::Downsample);
        REQUIRE(after.effects[1]->program == P::Downsample);
        CHECK(downsampleAt(after, 0).channel == 1.0f);
        CHECK(after.effects[0]->src == sourceOf(before));
        CHECK(downsampleAt(after, 1).channel == 0.0f);
        for (size_t i = 0; i < after.effects.size(); ++i) {
            if (after.effects[i]->program == P::GaussScalar) {
                CHECK(gaussAt(after, i).channel == 0.0f);
            }
        }
    }
}

TEST_CASE("gpu channel fold: no fold with a spread, an identity blur or a device without channels (T-728)") {
    using P = EffectProgram;
    SUBCASE("spread: the outer chain starts with a Dilate reading alpha itself") {
        Built before, after;
        const CommandList cl = effectList({dropShadow(6, 2, false)});
        build(cl, false, before);
        build(cl, true, after);
        CHECK(programs(after) ==
              std::vector<P>{P::Dilate, P::Dilate, P::GaussScalar, P::GaussScalar, P::ShadowGlow});
        CHECK(programs(after) == programs(before));
        DilateParams dp{};
        std::memcpy(&dp, after.frame.params.data() + after.effects[0]->paramIndex, sizeof(dp));
        CHECK(dp.readAlpha == 1.0f);
        CHECK(after.effects[0]->src == sourceOf(before));
        CHECK(gaussAt(after, 2).channel == 0.0f);
    }
    SUBCASE("spread: the inset chain keeps its AlphaExtract (the Dilate reads the inverted field)") {
        Built before, after;
        const CommandList cl = effectList({dropShadow(6, 2, true)});
        build(cl, false, before);
        build(cl, true, after);
        CHECK(programs(after) == std::vector<P>{P::AlphaExtract, P::Dilate, P::Dilate, P::GaussScalar,
                                                P::GaussScalar, P::ShadowGlow});
        CHECK(after.frame.passes.size() == before.frame.passes.size());
    }
    SUBCASE("blur 0: the AlphaExtract result is the mask itself") {
        Built before, after;
        const CommandList cl = effectList({dropShadow(0, 0, false)});
        build(cl, false, before);
        build(cl, true, after);
        CHECK(programs(after) == std::vector<P>{P::AlphaExtract, P::ShadowGlow});
        CHECK(programs(after) == programs(before));
    }
    SUBCASE("a device without channel reads: the blocks carry no channel field") {
        Built before;
        const CommandList cl = effectList({dropShadow(6, 0, false)});
        build(cl, false, before);
        REQUIRE(before.effects.size() == 4);
        // Consecutive parameter blocks: the GaussScalar block is as long as before T-728.
        const uint32_t gauss = before.effects[1]->paramIndex;
        const uint32_t next = before.effects[2]->paramIndex;
        CHECK(next - gauss == offsetof(GaussParams, channel) / sizeof(float));
    }
}
