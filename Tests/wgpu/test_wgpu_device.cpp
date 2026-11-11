// xt_wgpu skeleton cases (Docs/tasks/T-437.md item 4, Docs/design/10-backends.md §10.10.3):
// headless device, texture upload/readback round trips, the clear of an empty frame, 4x transients.
// T-438: Direct mode is on; draw commands are executed (Tests/wgpu/test_wgpu_draw.cpp).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"
#include "test_wgpu_common.h"
#include "wgpu_util.h"  // T-670: planInstance, kMaxNonSamplerBindings (Code/Runtime/wgpu/src)
#include "xt/command_list.h"
#include "xt/gpu/gpu_frame.h"
#include "xt/wgpu/wgpu_device.h"
#include "xt/wgpu/wgpu_renderer.h"

using xt::wgpu::WgpuDevice;
using xt::wgpu::WgpuInit;
using xt::wgpu::WgpuRenderer;

namespace {

/// A render target texture the cases draw into and read back.
struct Target {
    WGPUTexture texture = nullptr;
    WGPUTextureView view = nullptr;
    Target(WGPUDevice device, uint32_t w, uint32_t h, WGPUTextureFormat format) {
        WGPUTextureDescriptor d = WGPU_TEXTURE_DESCRIPTOR_INIT;
        // TEXTURE_BINDING: a 4x surface drawn without a clear loads it first (T-438).
        d.usage =
            WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc | WGPUTextureUsage_TextureBinding;
        d.size = WGPUExtent3D{w, h, 1};
        d.format = format;
        texture = wgpuDeviceCreateTexture(device, &d);
        view = wgpuTextureCreateView(texture, nullptr);
    }
    ~Target() {
        wgpuTextureViewRelease(view);
        wgpuTextureDestroy(texture);
        wgpuTextureRelease(texture);
    }
    Target(const Target&) = delete;
    Target& operator=(const Target&) = delete;
};

WgpuInit withSamples(uint32_t samples) {
    WgpuInit in = xt::wgpu::test::sharedInit();
    in.sampleCount = samples;
    in.error.clear();
    return in;
}

/// Every texel of `pixels` (RGBA8) equals `rgba`.
bool allEqual(const std::vector<uint8_t>& pixels, const uint8_t rgba[4]) {
    for (size_t i = 0; i < pixels.size(); i += 4) {
        if (std::memcmp(&pixels[i], rgba, 4) != 0) {
            return false;
        }
    }
    return !pixels.empty();
}

}  // namespace

TEST_CASE("wgpu: headless device and renderer init") {
    WgpuRenderer r;
    REQUIRE(r.init(withSamples(1)));
    CHECK(std::string(r.lastError()).empty());
    const std::string name = r.adapterName();
    CHECK(!name.empty());
    std::printf("xt_wgpu adapter: %s\n", name.c_str());
    REQUIRE(r.device() != nullptr);
    CHECK(r.device()->maxTextureSize() >= 8192u);
    CHECK(r.device()->supportsDirectTarget());  // T-438

    // A WgpuInit without a device is a reported failure, never an abort.
    WgpuRenderer bad;
    WgpuInit none;
    none.error = "no WebGPU adapter (test)";
    CHECK_FALSE(bad.init(none));
    CHECK(std::string(bad.lastError()) == "no WebGPU adapter (test)");
    WgpuInit badSamples = withSamples(2);
    CHECK_FALSE(bad.init(badSamples));
    CHECK(!std::string(bad.lastError()).empty());
}

TEST_CASE("wgpu: texture create / update / readback round trip") {
    WgpuRenderer r;
    REQUIRE(r.init(withSamples(1)));
    WgpuDevice& dev = *r.device();

    // Rgba8, 33 x 17: a row (132 bytes) that is not a multiple of 256.
    const uint16_t w = 33, h = 17;
    std::vector<uint8_t> image(size_t{w} * h * 4);
    for (size_t i = 0; i < image.size(); ++i) {
        image[i] = static_cast<uint8_t>((i * 7u + 3u) & 0xFFu);
    }
    const xt::gpu::TextureHandle tex =
        dev.createTexture("rgba", w, h, xt::gpu::TextureFormat::Rgba8, image.data());
    REQUIRE(tex != 0);
    REQUIRE(dev.texture(tex) != nullptr);
    std::vector<uint8_t> back(image.size());
    REQUIRE_MESSAGE(r.readback(dev.texture(tex), back), r.lastError());
    CHECK(back == image);  // row 0 = top, byte for byte

    // A sub-rectangle with a row pitch wider than its rows.
    const uint16_t ux = 5, uy = 3, uw = 7, uh = 4;
    const size_t pitch = size_t{uw} * 4 + 12;
    std::vector<uint8_t> patch(pitch * uh, 0xEE);
    for (uint16_t y = 0; y < uh; ++y) {
        for (size_t x = 0; x < size_t{uw} * 4; ++x) {
            patch[y * pitch + x] = static_cast<uint8_t>(200u + y * 11u + x);
            image[((uy + y) * size_t{w} + ux) * 4 + x] = patch[y * pitch + x];
        }
    }
    dev.updateTexture(tex, ux, uy, uw, uh, patch.data(), pitch);
    CHECK(dev.lastError().empty());
    REQUIRE(r.readback(dev.texture(tex), back));
    CHECK(back == image);

    // R8 (the glyph atlas format), 13 x 9.
    std::vector<uint8_t> coverage(13 * 9);
    for (size_t i = 0; i < coverage.size(); ++i) {
        coverage[i] = static_cast<uint8_t>(255u - i);
    }
    const xt::gpu::TextureHandle r8 =
        dev.createTexture("r8", 13, 9, xt::gpu::TextureFormat::R8, coverage.data());
    REQUIRE(r8 != 0);
    std::vector<uint8_t> r8back(coverage.size());
    REQUIRE(r.readback(dev.texture(r8), r8back));
    CHECK(r8back == coverage);

    // A wrong output size is refused, not written past.
    std::vector<uint8_t> small(10);
    CHECK_FALSE(r.readback(dev.texture(r8), small));
    CHECK(!std::string(r.lastError()).empty());

    // Destroy: the handle is free again and reused.
    dev.destroyTexture(r8);
    CHECK(dev.texture(r8) == nullptr);
    dev.destroyTexture(r8);  // twice: ignored
    const xt::gpu::TextureHandle again =
        dev.createTexture("again", 4, 4, xt::gpu::TextureFormat::Rgba8, nullptr);
    CHECK(again == r8);
    // Over the device limit or empty: 0, never an abort.
    CHECK(dev.createTexture("empty", 0, 4, xt::gpu::TextureFormat::Rgba8, nullptr) == 0);
    // Compressed sizes that are not a multiple of 4: 0 (the builder falls back to Rgba8).
    CHECK(dev.createTexture("bc", 6, 6, xt::gpu::TextureFormat::Bc7, nullptr) == 0);
}

TEST_CASE("wgpu: render of an empty command list clears the target") {
    WgpuRenderer r;
    REQUIRE(r.init(withSamples(1)));
    Target target(xt::wgpu::test::sharedInit().device, 64, 48, WGPUTextureFormat_RGBA8Unorm);
    r.resize(64, 48);
    r.setClearColor(51.0f / 255.0f, 102.0f / 255.0f, 204.0f / 255.0f, 1.0f);
    xt::CommandList list;
    r.render(list, nullptr, nullptr, target.view);
    CHECK_MESSAGE(std::string(r.lastError()).empty(), r.lastError());
    CHECK(r.stats().clears == 1);
    std::vector<uint8_t> pixels(64 * 48 * 4);
    REQUIRE_MESSAGE(r.readback(target.texture, pixels), r.lastError());
    const uint8_t expected[4] = {51, 102, 204, 255};
    CHECK(allEqual(pixels, expected));

    // Another color, then no clear at all: the content stays.
    r.setClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    r.render(list, nullptr, nullptr, target.view);
    REQUIRE(r.readback(target.texture, pixels));
    const uint8_t transparent[4] = {0, 0, 0, 0};
    CHECK(allEqual(pixels, transparent));
    r.setClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    r.render(list, nullptr, nullptr, target.view);
    r.disableClear();
    r.render(list, nullptr, nullptr, target.view);
    CHECK(r.stats().passes == 0);
    REQUIRE(r.readback(target.texture, pixels));
    const uint8_t white[4] = {255, 255, 255, 255};
    CHECK(allEqual(pixels, white));
}

TEST_CASE("wgpu: 4x MSAA surface and transients") {
    WgpuRenderer r;
    REQUIRE(r.init(withSamples(4)));
    WgpuDevice& dev = *r.device();
    CHECK(dev.sampleCount() == 4u);
    Target target(xt::wgpu::test::sharedInit().device, 64, 64, WGPUTextureFormat_RGBA8Unorm);
    r.resize(64, 64);

    // The surface: a 4x attachment that resolves into the target view.
    r.setClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    xt::CommandList list;
    r.render(list, nullptr, nullptr, target.view);
    CHECK_MESSAGE(std::string(r.lastError()).empty(), r.lastError());
    std::vector<uint8_t> pixels(64 * 64 * 4);
    REQUIRE(r.readback(target.texture, pixels));
    const uint8_t green[4] = {0, 255, 0, 255};
    CHECK(allEqual(pixels, green));

    // Transients: Rgba8 with stencil is 4x (resolved into a sampleable texture), R32F single-sample.
    xt::gpu::GpuFrame frame;
    frame.targetWidth = 64;
    frame.targetHeight = 64;
    xt::gpu::TransientTargetDesc color;
    color.width = 64;
    color.height = 64;
    color.format = xt::gpu::TextureFormat::Rgba8;
    color.needStencil = 1;
    xt::gpu::TransientTargetDesc sdf;
    sdf.width = 96;
    sdf.height = 64;
    sdf.format = xt::gpu::TextureFormat::R32F;
    frame.transients = {color, sdf};
    xt::gpu::Pass p1;
    p1.target = 1;
    p1.clear = 1;
    xt::gpu::Pass p2;
    p2.target = 2;
    p2.clear = 1;
    xt::gpu::Pass p3;  // back on the surface
    p3.target = 0;
    frame.passes = {p1, p2, p3};
    dev.setSurface(target.view);
    dev.submit(frame);
    CHECK_MESSAGE(dev.lastError().empty(), dev.lastError());
    CHECK(dev.lastStats().transientCreates == 2u);
    CHECK(dev.lastStats().passes == 3u);
    CHECK(dev.transientSampleCount(1) == 4u);
    CHECK(dev.transientSampleCount(2) == 1u);
    REQUIRE(dev.transientTexture(1) != nullptr);
    REQUIRE(dev.transientTexture(2) != nullptr);
    CHECK(wgpuTextureGetSampleCount(dev.transientTexture(1)) == 1u);
    REQUIRE(r.readback(dev.transientTexture(1), pixels));
    const uint8_t transparent[4] = {0, 0, 0, 0};
    CHECK(allEqual(pixels, transparent));
    std::vector<uint8_t> floats(96 * 64 * 4);
    REQUIRE(r.readback(dev.transientTexture(2), floats));

    // The next frame reuses both pool entries.
    dev.submit(frame);
    CHECK(dev.lastStats().transientCreates == 0u);
    CHECK(dev.lastStats().poolEntries == 2u);
    dev.setSurface(nullptr);
}

TEST_CASE("wgpu: empty draw commands draw nothing and report nothing (T-438)") {
    WgpuRenderer r;
    REQUIRE(r.init(withSamples(1)));
    WgpuDevice& dev = *r.device();
    Target target(xt::wgpu::test::sharedInit().device, 32, 32, WGPUTextureFormat_RGBA8Unorm);
    r.resize(32, 32);
    xt::gpu::GpuFrame frame;
    frame.targetWidth = 32;
    frame.targetHeight = 32;
    frame.cmds.resize(3);
    xt::gpu::Pass p;
    p.target = 0;
    p.firstCmd = 0;
    p.cmdCount = 3;
    frame.passes = {p};
    dev.setSurface(target.view);
    dev.submit(frame);
    dev.setSurface(nullptr);
    CHECK_MESSAGE(dev.lastError().empty(), dev.lastError());
    CHECK(dev.lastStats().skippedCmds == 0u);
    CHECK(dev.lastStats().drawCalls == 0u);
    CHECK(dev.lastStats().passes == 1u);
}

// T-456: a pipeline that cannot be created (here: SrcOver blending into an R32Float transient,
// which WebGPU does not allow without float32-blendable) is named once in lastError and its draws
// are skipped; the submit returns, the rest of the frame is drawn, nothing reaches the
// uncaptured-error sink.
TEST_CASE("wgpu: a failed pipeline skips its draws, is reported once, the rest renders (T-456)") {
    WgpuRenderer r;
    REQUIRE(r.init(withSamples(1)));
    WgpuDevice& dev = *r.device();
    Target target(xt::wgpu::test::sharedInit().device, 32, 32, WGPUTextureFormat_RGBA8Unorm);
    r.resize(32, 32);

    xt::gpu::GpuFrame frame;
    frame.targetWidth = 32;
    frame.targetHeight = 32;
    const uint32_t red = 0xFF0000FFu;  // RGBA bytes 255, 0, 0, 255
    frame.vertices = {
        {0, 0, 1, 0, 0, red}, {32, 0, 1, 1, 0, red}, {32, 32, 1, 1, 1, red}, {0, 32, 1, 0, 1, red}};
    frame.indices = {0, 1, 2, 0, 2, 3};
    xt::gpu::DrawCmd cmd;
    cmd.indexCount = 6;
    cmd.vertexCount = 4;
    cmd.shading = xt::gpu::Shading::Solid;
    cmd.blend = xt::gpu::Blend::SrcOver;
    cmd.clipW = 32;
    cmd.clipH = 32;
    frame.cmds = {cmd, cmd};
    xt::gpu::TransientTargetDesc scratch;
    scratch.width = 32;
    scratch.height = 32;
    scratch.format = xt::gpu::TextureFormat::R32F;
    frame.transients = {scratch};
    xt::gpu::Pass bad;  // the invalid pipeline
    bad.target = 1;
    bad.clear = 1;
    bad.firstCmd = 0;
    bad.cmdCount = 1;
    xt::gpu::Pass good;  // the surface, drawn as usual
    good.target = 0;
    good.firstCmd = 1;
    good.cmdCount = 1;
    frame.passes = {bad, good};
    dev.setSurface(target.view);
    dev.setSurfaceClear(true, WGPUColor{0, 0, 0, 0});
    dev.submit(frame);
    const std::string first = dev.lastError();
    CHECK_MESSAGE(first.find("render pipeline") != std::string::npos, first);
    CHECK_MESSAGE(first.find("unavailable") != std::string::npos, first);
    CHECK(dev.lastStats().skippedCmds == 1u);
    CHECK(dev.lastStats().drawCalls >= 1u);
    std::vector<uint8_t> pixels(32 * 32 * 4);
    REQUIRE(r.readback(target.texture, pixels));
    const uint8_t opaqueRed[4] = {255, 0, 0, 255};
    CHECK(allEqual(pixels, opaqueRed));

    // The failure is cached: the next submit skips the same draw without naming the pipeline again.
    dev.submit(frame);
    const std::string second = dev.lastError();
    CHECK_MESSAGE(second.find("render pipeline") == std::string::npos, second);
    CHECK(dev.lastStats().skippedCmds == 1u);
    dev.setSurface(nullptr);

    // Nothing leaked into the uncaptured-error sink (render reports it first).
    xt::CommandList list;
    r.setClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    r.render(list, nullptr, nullptr, target.view);
    CHECK_MESSAGE(std::string(r.lastError()).empty(), r.lastError());
}

// T-456: init builds one minimal pipeline first; a shader that does not compile (here through the
// test hook: invalid WGSL, then WGSL whose pipeline fails validation) makes init return false with
// an explanation, never abort.
TEST_CASE("wgpu: init fails cleanly when the probe pipeline cannot be built (T-456)") {
    WgpuDevice::setProbeShaderForTesting("this is not WGSL");
    WgpuRenderer bad;
    CHECK_FALSE(bad.init(withSamples(1)));
    std::string err = bad.lastError();
    CHECK_MESSAGE(err.find("minimal render pipeline") != std::string::npos, err);

    // A valid module whose fragment output (i32) does not match the RGBA8 unorm target.
    WgpuDevice::setProbeShaderForTesting(R"(
@vertex fn vs_probe(@builtin(vertex_index) i: u32) -> @builtin(position) vec4<f32> {
    return vec4<f32>(0.0, 0.0, 0.0, 1.0);
}
@fragment fn fs_probe() -> @location(0) vec4<i32> {
    return vec4<i32>(1, 1, 1, 1);
}
)");
    CHECK_FALSE(bad.init(withSamples(4)));
    err = bad.lastError();
    CHECK_MESSAGE(err.find("minimal render pipeline") != std::string::npos, err);

    WgpuDevice::setProbeShaderForTesting(nullptr);
    WgpuRenderer good;
    REQUIRE_MESSAGE(good.init(withSamples(1)), good.lastError());
    Target target(xt::wgpu::test::sharedInit().device, 16, 16, WGPUTextureFormat_RGBA8Unorm);
    good.resize(16, 16);
    good.setClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    xt::CommandList list;
    good.render(list, nullptr, nullptr, target.view);
    CHECK_MESSAGE(std::string(good.lastError()).empty(), good.lastError());  // no leaked errors
}

// T-457: the uniform buffer grows in 64 KB steps; the group 0 / group 1 bind groups over it must
// follow every growth. 200 submits whose parameter blocks fill a step exactly, then overrun it by
// one block (buffer 64, 128, 192, 256 KB, then the same sizes again): every dynamic offset stays
// inside the buffer its bind group names (a stale group is a validation error at submit, which
// aborts the process), nothing is skipped, and the frame still draws. Run under D3D12 and, with
// XT_WGPU_BACKEND=vulkan, Vulkan.
TEST_CASE("wgpu: parameter blocks that fill or overrun a uniform buffer step bind in range (T-457)") {
    WgpuRenderer r;
    REQUIRE(r.init(withSamples(1)));
    WgpuDevice& dev = *r.device();
    Target target(xt::wgpu::test::sharedInit().device, 16, 16, WGPUTextureFormat_RGBA8Unorm);
    r.resize(16, 16);
    const uint32_t red = 0xFF0000FFu;
    constexpr uint32_t kGlyphFloats = sizeof(xt::gpu::GlyphParams) / sizeof(float);
    // Uniform bytes of a frame with one pass and `n` glyph parameter blocks: the 512-byte zero
    // block, the pass block (256), one 256-byte block each, the 512-byte tail.
    const auto blocksFilling = [](uint32_t bytes) { return (bytes - 1280u) / 256u; };
    for (int f = 0; f < 200; ++f) {
        const uint32_t step = 64u * 1024u * static_cast<uint32_t>(1 + (f / 2) % 4);
        const uint32_t glyphs = blocksFilling(step) + static_cast<uint32_t>(f % 2);  // fill, then +1
        xt::gpu::GpuFrame frame;
        frame.targetWidth = 16;
        frame.targetHeight = 16;
        frame.vertices = {
            {0, 0, 1, 0, 0, red}, {16, 0, 1, 1, 0, red}, {16, 16, 1, 1, 1, red}, {0, 16, 1, 0, 1, red}};
        frame.indices = {0, 1, 2, 0, 2, 3};
        frame.params.assign(static_cast<size_t>(glyphs) * kGlyphFloats, 0.0f);
        xt::gpu::DrawCmd glyph;
        glyph.indexCount = 6;
        glyph.vertexCount = 4;
        glyph.shading = xt::gpu::Shading::Glyph;  // the dummy (transparent) atlas: draws nothing
        glyph.blend = xt::gpu::Blend::SrcOver;
        glyph.clipW = 16;
        glyph.clipH = 16;
        frame.cmds.reserve(glyphs + 1u);
        for (uint32_t i = 0; i < glyphs; ++i) {
            frame.params[static_cast<size_t>(i) * kGlyphFloats] = 1.0f;  // gamma
            glyph.paramIndex = i * kGlyphFloats;
            frame.cmds.push_back(glyph);
        }
        xt::gpu::DrawCmd solid = glyph;
        solid.shading = xt::gpu::Shading::Solid;
        solid.paramIndex = 0;
        frame.cmds.push_back(solid);
        xt::gpu::Pass p;
        p.target = 0;
        p.firstCmd = 0;
        p.cmdCount = static_cast<uint32_t>(frame.cmds.size());
        frame.passes = {p};
        dev.setSurface(target.view);
        dev.setSurfaceClear(true, WGPUColor{0, 0, 0, 0});
        dev.submit(frame);
        dev.setSurface(nullptr);
        REQUIRE_MESSAGE(dev.lastError().empty(), "frame " << f << ": " << dev.lastError());
        REQUIRE(dev.lastStats().skippedCmds == 0u);
        REQUIRE(dev.lastStats().drawCalls == glyphs + 1u);
    }
    std::vector<uint8_t> pixels(16 * 16 * 4);
    REQUIRE(r.readback(target.texture, pixels));
    const uint8_t opaqueRed[4] = {255, 0, 0, 255};
    CHECK(allEqual(pixels, opaqueRed));
    // Nothing reached the uncaptured-error sink.
    xt::CommandList list;
    r.render(list, nullptr, nullptr, target.view);
    CHECK_MESSAGE(std::string(r.lastError()).empty(), r.lastError());
}

// T-670 (Docs/design/50-c-abi.md §50.ab addendum 1 i): the instance masks headless() tries. The
// D3D12-only -> Primary retry has no injection point in a real device request, so the choice of
// masks is tested here as a pure function (wgpu_util.h planInstance).
TEST_CASE("wgpu: instance backend plan (T-670)") {
    xt::wgpu::InstancePlan plan;
    REQUIRE(xt::wgpu::planInstance("", true, plan));
    CHECK(plan.first == WGPUInstanceBackend_DX12);
    CHECK(plan.retryPrimary);
    REQUIRE(xt::wgpu::planInstance("", false, plan));
    CHECK(plan.first == WGPUInstanceBackend_Primary);
    CHECK_FALSE(plan.retryPrimary);
    const struct {
        const char* name;
        WGPUInstanceBackend mask;
    } named[] = {{"vulkan", WGPUInstanceBackend_Vulkan},
                 {"d3d12", WGPUInstanceBackend_DX12},
                 {"metal", WGPUInstanceBackend_Metal},
                 {"gl", WGPUInstanceBackend_GL}};
    for (const auto& n : named) {
        for (bool windows : {true, false}) {
            REQUIRE(xt::wgpu::planInstance(n.name, windows, plan));
            CHECK(plan.first == n.mask);
            CHECK_FALSE(plan.retryPrimary);  // an explicit backend never falls back
        }
    }
    CHECK_FALSE(xt::wgpu::planInstance("dx11", true, plan));
}

TEST_CASE("wgpu: headless device on the default backend, smaller descriptor heap (T-670)") {
    const WgpuInit& in = xt::wgpu::test::sharedInit();
    REQUIRE(in.adapter != nullptr);
    WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
    REQUIRE(wgpuAdapterGetInfo(in.adapter, &info) == WGPUStatus_Success);
    const WGPUBackendType backend = info.backendType;
    const WGPUAdapterType type = info.adapterType;
    wgpuAdapterInfoFreeMembers(info);
#if defined(_WIN32)
    size_t envLen = 0;
    char env[64] = {0};
    if (getenv_s(&envLen, env, sizeof(env), "XT_WGPU_BACKEND") != 0 || envLen <= 1) {
        if (type != WGPUAdapterType_CPU) {
            CHECK(backend == WGPUBackendType_D3D12);  // the DX12-only instance, no Primary retry
        }
    }
#else
    (void)backend;
    (void)type;
#endif
    WGPUNativeLimits native = WGPU_NATIVE_LIMITS_INIT;
    WGPULimits limits = WGPU_LIMITS_INIT;
    limits.nextInChain = &native.chain;
    wgpuDeviceGetLimits(in.device, &limits);
    CHECK(native.maxNonSamplerBindings == xt::wgpu::kMaxNonSamplerBindings);
    CHECK(limits.maxTextureDimension2D >= 8192u);  // the other limits are the base ones, as before
}

namespace {

/// One 16x16 pass drawing a quad with `shading` (sampling `tex` when not 0) into `target`.
void submitQuad(WgpuDevice& dev, WGPUTextureView target, xt::gpu::Shading shading,
                xt::gpu::TextureHandle tex) {
    const uint32_t white = 0xFFFFFFFFu;
    xt::gpu::GpuFrame frame;
    frame.targetWidth = 16;
    frame.targetHeight = 16;
    frame.vertices = {
        {0, 0, 1, 0, 0, white}, {16, 0, 1, 1, 0, white}, {16, 16, 1, 1, 1, white}, {0, 16, 1, 0, 1, white}};
    frame.indices = {0, 1, 2, 0, 2, 3};
    frame.params.assign(64, 0.0f);
    xt::gpu::DrawCmd cmd;
    cmd.indexCount = 6;
    cmd.vertexCount = 4;
    cmd.shading = shading;
    cmd.blend = xt::gpu::Blend::SrcOver;
    cmd.texture = tex;
    cmd.clipW = 16;
    cmd.clipH = 16;
    frame.cmds = {cmd};
    xt::gpu::Pass p;
    p.target = 0;
    p.firstCmd = 0;
    p.cmdCount = 1;
    frame.passes = {p};
    dev.setSurface(target);
    dev.setSurfaceClear(true, WGPUColor{0, 0, 0, 0});
    dev.submit(frame);
    dev.setSurface(nullptr);
}

}  // namespace

TEST_CASE("wgpu: live non-sampler bindings are counted per device (T-670)") {
    WgpuRenderer r;
    REQUIRE(r.init(withSamples(1)));
    WgpuDevice& dev = *r.device();
    Target target(xt::wgpu::test::sharedInit().device, 16, 16, WGPUTextureFormat_RGBA8Unorm);
    r.resize(16, 16);

    // Group 0 / group 1 (one uniform buffer each) and the solid draw's texture group (3 views).
    submitQuad(dev, target.view, xt::gpu::Shading::Solid, 0);
    REQUIRE_MESSAGE(dev.lastError().empty(), dev.lastError());
    const uint32_t base = dev.lastStats().liveBindings;
    CHECK(base == 2u + 3u);
    CHECK(dev.lastStats().peakBindings >= base);

    // A texture draw adds one cached group of 3 views.
    std::vector<uint8_t> texels(4 * 4 * 4, 0xFF);
    const xt::gpu::TextureHandle tex =
        dev.createTexture("t670", 4, 4, xt::gpu::TextureFormat::Rgba8, texels.data());
    REQUIRE(tex != 0);
    submitQuad(dev, target.view, xt::gpu::Shading::Texture, tex);
    REQUIRE_MESSAGE(dev.lastError().empty(), dev.lastError());
    REQUIRE(dev.lastStats().drawCalls == 1u);
    const uint32_t withTexture = dev.lastStats().liveBindings;
    CHECK(withTexture == base + 3u);
    CHECK(dev.lastStats().peakBindings >= withTexture);

    // Destroying the texture flushes the cached groups; the next solid frame is back to the base.
    dev.destroyTexture(tex);
    submitQuad(dev, target.view, xt::gpu::Shading::Solid, 0);
    REQUIRE_MESSAGE(dev.lastError().empty(), dev.lastError());
    CHECK(dev.lastStats().liveBindings == base);
    CHECK(dev.lastStats().peakBindings >= withTexture);
    CHECK(dev.lastStats().peakBindings >= dev.lastStats().liveBindings);
}
