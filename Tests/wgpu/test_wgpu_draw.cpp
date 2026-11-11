// T-438 part a (Docs/tasks/T-438.md item 8, Docs/design/10-backends.md §10.10.3): geometry and
// compositing through WgpuDevice::submit -- stencil fills, clip depth, transient windows, 4x MSAA --
// compared with xt::sw at 15 §15.3's GPU tolerance. T-658: one 4x resolve per surface frame (or one
// before each pass reading the surface) and WgpuRenderer::build / submit.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "doctest.h"
#include "test_wgpu_draw_util.h"
#include "xt/gpu/frame_builder.h"
#include "xt/gpu/gpu_frame.h"
#include "xt/wgpu/wgpu_device.h"

using namespace xt;
using namespace xt::wgpu::test;

TEST_CASE("wgpu draw: NonZero and EvenOdd fills through the stencil match SW") {
    constexpr int kW = 128, kH = 128;
    CommandList cl = makeList(kW, kH);
    // A concave five-point star (NonZero: the stencil-then-cover path, not the convex fan).
    addFill(cl,
            addPolygon(cl, {Vec2{64, 6}, Vec2{78, 50}, Vec2{122, 50}, Vec2{86, 76}, Vec2{100, 120},
                            Vec2{64, 94}, Vec2{28, 120}, Vec2{42, 76}, Vec2{6, 50}, Vec2{50, 50}}),
            Color{1, 0.4f, 0, 1});
    // Two concentric 48-gons, EvenOdd: a ring whose hole shows the star.
    PathRef rings = emptyPath(cl);
    appendNgon(cl, rings, Vec2{64, 64}, 40, 48);
    appendNgon(cl, rings, Vec2{64, 64}, 22, 48);
    addFill(cl, rings, Color{0, 0.5f, 1, 0.8f}, FillRule::EvenOdd);
    Session s(kW, kH);
    const std::vector<uint8_t> gpu = s.render(cl);
    CHECK_MESSAGE(s.error.empty(), s.error);
    CHECK(s.renderer.stats().drawCalls > 0);
    CHECK(matches(gpu, renderSw(cl, kW, kH), kW, kH, "stencil fills"));
    // The hole of the ring is the star's colour, the ring itself the blend.
    CHECK(pixel(gpu, kW, 64, 64)[0] > 200);
    CHECK(pixel(gpu, kW, 64, 34)[2] > 150);
}

TEST_CASE("wgpu draw: T-263b a 16th nested stencil clip is cut to its bounding box, pops balanced") {
    constexpr int kW = 128, kH = 128;
    CommandList cl = makeList(kW, kH);
    const Mat2x3 xf = mul(translation(Vec2{64, 64}), rotationDeg(10.0f));
    const uint32_t kNested = kMaxClipDepth + 1;
    float half = 0;
    for (uint32_t i = 0; i < kNested; ++i) {
        half = 62.0f - 3.0f * static_cast<float>(i);
        addPushClipRect(cl, Rect{-half, -half, 2 * half, 2 * half}, xf);
    }
    addFillRect(cl, 0, 0, 128, 128, Color{1, 0, 0, 1});
    for (uint32_t i = 0; i < kNested; ++i) {
        addPopClip(cl);
    }
    addFillRect(cl, 0, 0, 8, 8, Color{0, 1, 0, 1});
    Session s(kW, kH);
    const std::vector<uint8_t> px = s.render(cl);
    CHECK_MESSAGE(s.error.find("clip depth limit") != std::string::npos, s.error);
    CHECK(pixel(px, kW, 64, 64)[0] > 240);
    // Just inside a corner of the 15th square: inside every stencil level, outside the 16th's box.
    const float outer = half + 3.0f;
    const Vec2 corner = apply(xf, Vec2{outer - 1.5f, outer - 1.5f});
    const uint8_t* c = pixel(px, kW, static_cast<int>(corner.x), static_cast<int>(corner.y));
    CHECK(c[0] < 16);
    // Every PopClip paired: the last fill is unclipped, nothing else was touched.
    CHECK(pixel(px, kW, 4, 4)[1] > 240);
    CHECK(pixel(px, kW, 120, 4)[0] < 16);
}

TEST_CASE("wgpu draw: a layer is drawn into a windowed transient and composited like SW") {
    constexpr int kW = 256, kH = 192;
    CommandList cl = makeList(kW, kH);
    addFillRect(cl, 0, 0, kW, kH, Color{0.2f, 0.2f, 0.25f, 1});
    // A half-opacity group of two overlapping squares, well inside the surface (T-151 window).
    addPushLayer(cl, 0.5f, BlendMode::Normal, Rect{40, 30, 90, 80});
    addFillRect(cl, 40, 30, 60, 60, Color{1, 0, 0, 1});
    addFillRect(cl, 70, 50, 60, 60, Color{0, 0, 1, 1});
    addPopLayer(cl);
    // A rotated path clip around a second layer (stencil inside the transient, replayed).
    addPushClipRect(cl, Rect{-40, -40, 80, 80}, mul(translation(Vec2{190, 120}), rotationDeg(30.0f)));
    addPushLayer(cl, 0.75f, BlendMode::Normal, Rect{140, 70, 100, 100});
    addFillRect(cl, 140, 70, 100, 100, Color{0, 1, 0, 1});
    addPopLayer(cl);
    addPopClip(cl);
    Session s(kW, kH);
    const std::vector<uint8_t> gpu = s.render(cl);
    CHECK_MESSAGE(s.error.empty(), s.error);
    CHECK(matches(gpu, renderSw(cl, kW, kH), kW, kH, "layer windows"));
    // The first layer's transient is its window (rounded up to 32, at least 64), not the surface.
    WGPUTexture t = s.renderer.device()->transientTexture(1);
    REQUIRE(t != nullptr);
    CHECK(wgpuTextureGetWidth(t) < static_cast<uint32_t>(kW));
    CHECK(wgpuTextureGetWidth(t) % 32u == 0u);
    // The overlap is one composite of the group (opacity applied once): purple at half strength.
    const uint8_t* mid = pixel(gpu, kW, 85, 70);
    CHECK(mid[2] > 100);
    CHECK(mid[0] < 130);
}

TEST_CASE("wgpu draw: 4x MSAA surface and stencil transients match SW") {
    constexpr int kW = 128, kH = 128;
    CommandList cl = makeList(kW, kH);
    PathRef rings = emptyPath(cl);
    appendNgon(cl, rings, Vec2{50, 50}, 40, 40);
    appendNgon(cl, rings, Vec2{50, 50}, 20, 40);
    addFill(cl, rings, Color{1, 1, 0, 1}, FillRule::EvenOdd);
    addPushLayer(cl, 0.6f, BlendMode::Normal, Rect{50, 50, 70, 70});
    addFill(cl, addPolygon(cl, {Vec2{60, 60}, Vec2{120, 70}, Vec2{80, 120}, Vec2{90, 85}}),
            Color{0, 1, 1, 1});
    addPopLayer(cl);
    Session s(kW, kH, 4);
    const std::vector<uint8_t> gpu = s.render(cl);
    CHECK_MESSAGE(s.error.empty(), s.error);
    CHECK(s.renderer.device()->transientSampleCount(1) == 4u);
    CHECK(matches(gpu, renderSw(cl, kW, kH), kW, kH, "4x msaa"));
}

TEST_CASE("wgpu draw: a 4x surface drawn without a clear keeps the host's content (T-437 deviation 4)") {
    constexpr int kW = 64, kH = 64;
    Session s(kW, kH, 4);
    std::vector<uint8_t> host(static_cast<size_t>(kW) * kH * 4);
    for (size_t i = 0; i < host.size(); i += 4) {
        host[i] = 30;
        host[i + 1] = 160;
        host[i + 2] = 90;
        host[i + 3] = 255;
    }
    s.target.upload(host);
    CommandList cl = makeList(kW, kH);
    addFillRect(cl, 16, 16, 32, 32, Color{1, 0, 0, 1});
    s.renderer.disableClear();
    s.renderer.render(cl, nullptr, nullptr, s.target.view);
    CHECK_MESSAGE(std::string(s.renderer.lastError()).empty(), std::string(s.renderer.lastError()));
    CHECK(s.renderer.stats().blits == 1u);
    const std::vector<uint8_t> px = s.read();
    const uint8_t* corner = pixel(px, kW, 2, 2);
    CHECK(corner[0] == 30);
    CHECK(corner[1] == 160);
    CHECK(corner[2] == 90);
    CHECK(pixel(px, kW, 32, 32)[0] == 255);
}

TEST_CASE("wgpu draw: the geometry buffers upload only what changed (T-159)") {
    constexpr int kW = 64, kH = 64;
    CommandList cl = makeList(kW, kH);
    for (int i = 0; i < 8; ++i) {
        addFillRect(cl, 4.0f + 7.0f * static_cast<float>(i), 4, 5, 50, Color{1, 1, 1, 1});
    }
    Session s(kW, kH);
    s.render(cl);
    const uint64_t first = s.renderer.stats().uploadBytes;
    // Three rotating sets: the fourth identical frame reuses the first set's content.
    s.render(cl);
    s.render(cl);
    s.render(cl);
    CHECK(s.renderer.stats().uploadBytes < first);
}

namespace {

/// T-658: the number of passes of `cl`'s frame that draw into target 0 (a builder of its own on the
/// session's device, edge fringe off as on a 4x target).
uint32_t surfacePasses(Session& s, const CommandList& cl, int w, int h) {
    gpu::FrameBuilder builder;
    builder.init(*s.renderer.device());
    builder.setEdgeFringe(false);
    builder.resize(w, h);
    gpu::GpuFrame frame;
    builder.build(cl, nullptr, nullptr, nullptr, frame);
    builder.shutdown();
    uint32_t n = 0;
    for (const gpu::Pass& p : frame.passes) n += p.target == gpu::kSurfaceTarget ? 1u : 0u;
    return n;
}

}  // namespace

TEST_CASE("wgpu draw: a 4x surface split by root layers and effects resolves once (T-658)") {
    constexpr int kW = 128, kH = 128;
    CommandList cl = makeList(kW, kH);
    addFillRect(cl, 0, 0, 128, 40, Color{0.2f, 0.4f, 1, 1});
    addPushLayer(cl, 0.6f, BlendMode::Normal, Rect{10, 10, 60, 60});
    addFill(cl, addPolygon(cl, {Vec2{12, 12}, Vec2{68, 20}, Vec2{40, 40}, Vec2{30, 66}}), Color{1, 1, 0, 1});
    addPopLayer(cl);
    addFillRect(cl, 0, 70, 128, 20, Color{0, 1, 0, 1});
    addPushEffect(cl, {effect(EffectKind::Blur, {3, 3})}, Rect{60, 60, 60, 60}, Rect{70, 70, 40, 40});
    addFillRect(cl, 70, 70, 40, 40, Color{1, 0, 0, 1});
    addPopEffect(cl);
    addPushLayer(cl, 0.5f, BlendMode::Normal, Rect{20, 90, 60, 30});
    addFillRect(cl, 22, 92, 56, 26, Color{1, 0, 1, 1});
    addPopLayer(cl);
    Session s(kW, kH, 4);
    const std::vector<uint8_t> gpu = s.render(cl);
    CHECK_MESSAGE(s.error.empty(), s.error);
    const uint32_t surface = surfacePasses(s, cl, kW, kH);
    CHECK(surface >= 4u);  // the three root composites each open a surface pass
    const wgpu::WgpuDeviceStats& d = s.renderer.device()->lastStats();
    CHECK(d.surfaceReads == 0u);
    CHECK(d.resolves == 1u);  // was one per surface pass
    CHECK(s.renderer.stats().resolves == 1u);
    CHECK(matches(gpu, renderSw(cl, kW, kH), kW, kH, "4x root layers and effect"));
    // The same frame again (a second submit of a fresh build): the same pixels, still one resolve.
    const std::vector<uint8_t> again = s.render(cl);
    CHECK(again == gpu);
    CHECK(s.renderer.stats().resolves == 1u);
}

TEST_CASE("wgpu draw: a 4x surface resolves before each pass that reads it (T-658)") {
    constexpr int kW = 128, kH = 128;
    CommandList cl = makeList(kW, kH);
    for (int i = 0; i < 8; ++i) {
        addFillRect(cl, static_cast<float>(i) * 16.0f, 0, 16, kH,
                    Color{static_cast<float>(i) / 7.0f, 0.5f, 1.0f - static_cast<float>(i) / 7.0f, 1});
    }
    // Two mix-blend layers on the surface (each reads a backdrop copy of it) and a backdrop blur
    // (its pyramid starts with a copy of it), with surface draws between them; a Normal layer opens
    // one more surface pass that no read follows directly (it does not resolve).
    addPushLayer(cl, 0.9f, BlendMode::Multiply, Rect{8, 8, 48, 48});
    addFillRect(cl, 10, 10, 44, 44, Color{0.9f, 0.3f, 0.6f, 0.8f});
    addPopLayer(cl);
    addFillRect(cl, 60, 8, 20, 20, Color{1, 1, 1, 1});
    addPushLayer(cl, 0.7f, BlendMode::Normal, Rect{84, 8, 30, 30});
    addFillRect(cl, 86, 10, 26, 26, Color{0, 0, 1, 1});
    addPopLayer(cl);
    addPushLayer(cl, 0.9f, BlendMode::Screen, Rect{64, 64, 48, 48});
    addFillRect(cl, 66, 66, 44, 44, Color{0.2f, 0.8f, 0.3f, 0.8f});
    addPopLayer(cl);
    addFillRect(cl, 8, 100, 20, 20, Color{0, 0, 0, 1});
    const Rect glass{20, 64, 40, 40};
    addPushEffect(cl, {effect(EffectKind::BackdropBlur, {6})}, glass, glass);
    addFillRect(cl, glass.x, glass.y, glass.w, glass.h, Color{1, 1, 1, 0.25f});
    addPopEffect(cl);
    Session s(kW, kH, 4);
    const std::vector<uint8_t> gpu = s.render(cl);
    CHECK_MESSAGE(s.error.empty(), s.error);
    const wgpu::WgpuDeviceStats& d = s.renderer.device()->lastStats();
    CHECK(d.surfaceReads == 3u);
    CHECK(d.resolves == d.surfaceReads + 1u);
    CHECK(surfacePasses(s, cl, kW, kH) == d.resolves + 1u);
    CHECK(matches(gpu, renderSw(cl, kW, kH), kW, kH, "4x surface reads"));
}

TEST_CASE("wgpu draw: build and submit apart draw what render draws (T-658)") {
    constexpr int kW = 64, kH = 64;
    CommandList cl = makeList(kW, kH);
    addFillRect(cl, 8, 8, 40, 40, Color{1, 0.5f, 0, 1});
    Session s(kW, kH, 4);
    const std::vector<uint8_t> whole = s.render(cl);
    CHECK(s.renderer.build(cl, nullptr, nullptr) == 1u);
    s.renderer.submit(s.target.view);
    CHECK(std::string(s.renderer.lastError()).empty());
    CHECK(s.read() == whole);
    // An empty list builds no draw command; a submit after a resize without a build draws nothing.
    CHECK(s.renderer.build(makeList(kW, kH), nullptr, nullptr) == 0u);
    s.renderer.resize(kW, kH);
    s.renderer.submit(s.target.view);
    CHECK(std::string(s.renderer.lastError()).find("no frame was built") != std::string::npos);
}
