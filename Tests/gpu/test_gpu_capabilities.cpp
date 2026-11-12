// T-691 (Docs/design/10-backends.md §10.14, Docs/tasks/T-691.md item 5): the frame builder against a
// device that answers the two capability queries either way -- no GPU needed (the pixel checks are in
// Tests/wgpu/test_wgpu_capabilities.cpp).

#include <cstdint>
#include <cstring>
#include <string>

#include "doctest.h"
#include "xt/command_list.h"
#include "xt/context.h"
#include "xt/gpu/frame_builder.h"
#include "xt/gpu/gpu_device.h"

using namespace xt;
using namespace xt::gpu;

namespace {

/// Hands out texture handles; executes nothing.
class CapsDevice : public GpuDevice {
public:
    CapsDevice(bool coverage, bool boxShadow) : m_coverage(coverage), m_boxShadow(boxShadow) {}
    uint32_t coverageTextures = 0;
    bool supportsCoverageDraws() const override { return m_coverage; }
    bool supportsBoxShadowShading() const override { return m_boxShadow; }
    TextureHandle createTexture(const char* debugName, uint16_t, uint16_t, TextureFormat,
                                const void*) override {
        if (debugName != nullptr && std::strcmp(debugName, "xt coverage atlas") == 0) {
            ++coverageTextures;
        }
        return m_next++;
    }
    void updateTexture(TextureHandle, uint16_t, uint16_t, uint16_t, uint16_t, const void*, size_t) override {}
    void destroyTexture(TextureHandle) override {}
    void submit(const GpuFrame&) override {}

private:
    bool m_coverage;
    bool m_boxShadow;
    TextureHandle m_next = 1;
};

PathRef polygon(CommandList& cl, std::initializer_list<Vec2> pts) {
    PathRef ref{};
    ref.verbOffset = static_cast<uint32_t>(cl.verbs.size());
    ref.pointOffset = static_cast<uint32_t>(cl.points.size());
    bool first = true;
    for (Vec2 p : pts) {
        cl.verbs.push_back(first ? PathVerb::Move : PathVerb::Line);
        cl.points.push_back(p.x);
        cl.points.push_back(p.y);
        first = false;
    }
    cl.verbs.push_back(PathVerb::Close);
    ref.verbCount = static_cast<uint32_t>(cl.verbs.size()) - ref.verbOffset;
    ref.pointCount = static_cast<uint32_t>(cl.points.size()) - ref.pointOffset;
    return ref;
}

/// A concave fill, a concave clip path around a rect and a blurred plus a sharp DrawBoxShadow.
CommandList capabilityList() {
    CommandList cl;
    cl.viewport = Rect{0, 0, 200, 160};
    cl.devicePixelRatio = 1;
    FillCmd f{};
    f.path =
        polygon(cl, {Vec2{20, 20}, Vec2{80, 25}, Vec2{50, 45}, Vec2{85, 75}, Vec2{25, 70}, Vec2{40, 45}});
    f.paint.kind = PaintKind::Solid;
    f.paint.color = Color{0.8f, 0.2f, 0.1f, 1};
    f.rule = FillRule::NonZero;
    f.antialias = 1;
    cl.fills.push_back(f);
    cl.commands.push_back(Command{CmdKind::FillPath, {}, 0});
    ClipPathCmd c{};
    c.path = polygon(cl, {Vec2{125, 95}, Vec2{190, 100}, Vec2{160, 120}, Vec2{185, 150}, Vec2{120, 145}});
    c.rule = FillRule::NonZero;
    cl.clipPaths.push_back(c);
    cl.commands.push_back(Command{CmdKind::PushClipPath, {}, 0});
    FillCmd r = f;
    r.path = polygon(cl, {Vec2{110, 90}, Vec2{195, 90}, Vec2{195, 155}, Vec2{110, 155}});
    cl.fills.push_back(r);
    cl.commands.push_back(Command{CmdKind::FillPath, {}, 1});
    cl.commands.push_back(Command{CmdKind::PopClip, {}, 0});
    for (float blur : {12.0f, 0.0f}) {
        BoxShadowCmd bs{};
        bs.rect = Rect{blur > 0 ? 20.0f : 100.0f, 100, 40, 30};
        for (float& radius : bs.radii) {
            radius = 6;
        }
        bs.offset = Vec2{2, 3};
        bs.blur = blur;
        bs.color = Color{0, 0, 0, 0.5f};
        cl.commands.push_back(
            Command{CmdKind::DrawBoxShadow, {}, static_cast<uint32_t>(cl.boxShadows.size())});
        cl.boxShadows.push_back(bs);
    }
    return cl;
}

struct Counts {
    uint32_t coverage = 0, boxShadow = 0;
};

Counts countDraws(const GpuFrame& frame) {
    Counts n;
    for (const DrawCmd& c : frame.cmds) {
        n.coverage += c.coverage != 0 ? 1 : 0;
        n.boxShadow += c.shading == Shading::BoxShadow ? 1 : 0;
    }
    return n;
}

}  // namespace

TEST_CASE("gpu capabilities: a capable device gets coverage and box-shadow draws, no stats (T-691)") {
    CapsDevice device(true, true);
    FrameBuilder builder;
    builder.init(device);
    builder.resize(200, 160);
    GpuFrame frame;
    builder.build(capabilityList(), nullptr, nullptr, nullptr, frame);
    const Counts n = countDraws(frame);
    CHECK(n.coverage > 0u);
    CHECK(n.boxShadow == 1u);
    CHECK(device.coverageTextures > 0u);
    CHECK(builder.lastBuildStats().coverageFallbacks == 0u);
    CHECK(builder.lastBuildStats().boxShadowSkipped == 0u);
    CHECK(std::string(builder.lastError()).empty());
}

TEST_CASE("gpu capabilities: a device without them gets neither, counted; box shadow reported once (T-691)") {
    CapsDevice device(false, false);  // the defaults of GpuDevice
    FrameBuilder builder;
    builder.init(device);
    builder.resize(200, 160);
    GpuFrame frame;
    const CommandList cl = capabilityList();
    builder.build(cl, nullptr, nullptr, nullptr, frame);
    Counts n = countDraws(frame);
    CHECK(n.coverage == 0u);
    CHECK(n.boxShadow == 0u);
    CHECK(device.coverageTextures == 0u);
    CHECK(builder.lastBuildStats().coverageFallbacks == 2u);  // the concave fill and the clip path
    CHECK(builder.lastBuildStats().boxShadowSkipped == 1u);   // the sharp one is a path fill
    CHECK(std::string(builder.lastError()).find("Context::setBoxShadowCommands(false)") != std::string::npos);
    builder.build(cl, nullptr, nullptr, nullptr, frame);
    n = countDraws(frame);
    CHECK(n.coverage == 0u);
    CHECK(n.boxShadow == 0u);
    CHECK(builder.lastBuildStats().boxShadowSkipped == 1u);
    CHECK(std::string(builder.lastError()).empty());  // reported once
    builder.shutdown();
}

TEST_CASE("gpu capabilities: applyDeviceCapabilities only ever turns box-shadow commands off (T-691)") {
    Context ctx(ContextConfig{});
    CapsDevice capable(true, true), plain(false, false);
    applyDeviceCapabilities(capable, ctx);
    CHECK(ctx.boxShadowCommands());
    applyDeviceCapabilities(plain, ctx);
    CHECK(!ctx.boxShadowCommands());
    applyDeviceCapabilities(capable, ctx);  // a host's (or an earlier device's) false stays
    CHECK(!ctx.boxShadowCommands());
    const GpuDevice& base = plain;
    CHECK(!base.GpuDevice::supportsCoverageDraws());  // the defaults
    CHECK(!base.GpuDevice::supportsBoxShadowShading());
}
