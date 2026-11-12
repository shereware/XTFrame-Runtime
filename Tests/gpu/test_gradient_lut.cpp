// T-695 (Docs/design/50-c-abi.md §50.ac note 3 item 1, Docs/tasks/T-695.md item 3): gradient lookup
// tables survive 1800 unused builder frames, at most 256 are kept, trim-on-hide keeps them and a full
// release (FrameBuilder::shutdown, a device change) destroys them -- against a recording device.

#include <cstdint>
#include <cstring>
#include <map>

#include "doctest.h"
#include "xt/command_list.h"
#include "xt/gpu/frame_builder.h"
#include "xt/gpu/gpu_device.h"
#include "xt/gpu/gpu_frame.h"

using namespace xt;
using namespace xt::gpu;

namespace {

/// Records the gradient tables it creates and destroys; executes nothing.
class LutDevice : public GpuDevice {
public:
    bool supportsCoverageDraws() const override { return true; }
    bool supportsBoxShadowShading() const override { return true; }
    int lutCreates = 0;
    std::map<TextureHandle, bool> luts;  ///< handle -> alive

    TextureHandle createTexture(const char* debugName, uint16_t w, uint16_t h, TextureFormat,
                                const void*) override {
        const TextureHandle tex = m_next++;
        if (debugName != nullptr && std::strcmp(debugName, "xt gradient lut") == 0) {
            CHECK(w == kGradientLutWidth);
            CHECK(h == 1);
            ++lutCreates;
            luts[tex] = true;
        }
        return tex;
    }
    void updateTexture(TextureHandle, uint16_t, uint16_t, uint16_t, uint16_t, const void*, size_t) override {}
    void destroyTexture(TextureHandle tex) override {
        auto it = luts.find(tex);
        if (it != luts.end()) {
            CHECK(it->second);  // destroyed once
            it->second = false;
        }
    }
    void submit(const GpuFrame&) override {}

    int aliveLuts() const {
        int n = 0;
        for (const auto& [tex, alive] : luts) {
            (void)tex;
            n += alive ? 1 : 0;
        }
        return n;
    }

private:
    TextureHandle m_next = 1;
};

/// A list with one linear-gradient rect whose ramp is identified by `ramp` (distinct ramps have
/// distinct stops); `ramp < 0` gives an empty list.
CommandList gradientList(int ramp) {
    CommandList cl;
    cl.viewport = Rect{0, 0, 64, 64};
    cl.devicePixelRatio = 1;
    if (ramp < 0) return cl;
    PathRef ref{};
    ref.verbOffset = 0;
    ref.pointOffset = 0;
    cl.verbs = {PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Line, PathVerb::Close};
    cl.points = {4, 4, 60, 4, 60, 60, 4, 60};
    ref.verbCount = 5;
    ref.pointCount = 8;
    FillCmd f{};
    f.path = ref;
    f.paint.kind = PaintKind::Linear;
    f.paint.color = Color{1, 1, 1, 1};
    f.paint.p0 = Vec2{4, 4};
    f.paint.p1 = Vec2{60, 4};
    f.paint.stopOffset = 0;
    cl.stops.push_back(GradientStop{0.0f, Color{static_cast<float>(ramp) / 1024.0f, 0, 0, 1}});
    cl.stops.push_back(GradientStop{1.0f, Color{0, 0, 1, 1}});
    f.paint.stopCount = 2;
    f.rule = FillRule::NonZero;
    f.antialias = 1;
    cl.fills.push_back(f);
    cl.commands.push_back(Command{CmdKind::FillPath, {}, 0});
    return cl;
}

struct Lut {
    LutDevice device;
    FrameBuilder builder;
    GpuFrame frame;
    Lut() {
        builder.init(device);
        builder.resize(64, 64);
    }
    void build(const CommandList& cl) { builder.build(cl, nullptr, nullptr, nullptr, frame); }
};

}  // namespace

TEST_CASE("gpu: T-695 a gradient drawn now and then creates its lookup table once") {
    const CommandList drawn = gradientList(1);
    const CommandList empty = gradientList(-1);
    for (const int every : {2, 100}) {
        CAPTURE(every);
        Lut l;
        for (int frame = 0; frame < 1000; ++frame) {
            l.build(frame % every == 0 ? drawn : empty);
            REQUIRE(l.builder.lastError()[0] == 0);
        }
        CHECK(l.device.lutCreates == 1);
        CHECK(l.device.aliveLuts() == 1);
    }
}

TEST_CASE("gpu: T-695 a lookup table is destroyed after 1801 unused frames") {
    Lut l;
    l.build(gradientList(1));
    REQUIRE(l.device.aliveLuts() == 1);
    const CommandList empty = gradientList(-1);
    for (int i = 0; i < 1800; ++i) l.build(empty);
    CHECK(l.device.aliveLuts() == 1);  // 1800 unused frames: kept
    l.build(empty);
    CHECK(l.device.aliveLuts() == 0);  // the 1801st
    CHECK(l.device.lutCreates == 1);
}

TEST_CASE("gpu: T-695 at most 256 lookup tables are kept, the least recently used go first") {
    Lut l;
    for (int ramp = 0; ramp < 300; ++ramp) l.build(gradientList(ramp));
    CHECK(l.device.lutCreates == 300);
    CHECK(l.device.aliveLuts() <= 256);
    CHECK(l.device.aliveLuts() == 256);
    // The 256 most recent ramps (44..299) are the ones kept: drawing them again creates nothing.
    for (int ramp = 44; ramp < 300; ++ramp) l.build(gradientList(ramp));
    CHECK(l.device.lutCreates == 300);
    l.build(gradientList(0));  // evicted: created again
    CHECK(l.device.lutCreates == 301);
    CHECK(l.device.aliveLuts() == 256);
}

TEST_CASE("gpu: T-695 trim-on-hide keeps the lookup tables, a full release destroys them") {
    Lut l;
    const CommandList drawn = gradientList(1);
    l.build(drawn);
    REQUIRE(l.device.aliveLuts() == 1);
    l.builder.trim();  // trim-on-hide (WgpuRenderer::trim)
    CHECK(l.device.aliveLuts() == 1);
    l.build(drawn);  // shown again
    CHECK(l.device.lutCreates == 1);
    l.builder.shutdown();  // the renderer reset of xt_surface_trim
    CHECK(l.device.aliveLuts() == 0);

    // A device change (init with another device) destroys the old device's tables.
    l.builder.init(l.device);
    l.builder.resize(64, 64);
    l.build(drawn);
    REQUIRE(l.device.aliveLuts() == 1);
    LutDevice other;
    l.builder.init(other);
    CHECK(l.device.aliveLuts() == 0);
    l.builder.shutdown();
}
