#include "xt/wgpu/wgpu_renderer.h"

#include <webgpu/webgpu.h>
#include <webgpu/wgpu.h>

#include <chrono>
#include <cstring>
#include <string>

#include "wgpu_util.h"
#include "xt/gpu/frame_builder.h"
#include "xt/gpu/gpu_frame.h"
#include "xt/wgpu/wgpu_device.h"

/// WgpuRenderer (T-437, T-438): `gpu::FrameBuilder` builds the frame, `WgpuDevice` submits it; the shape
/// of `GlRenderer` (gl_renderer.cpp) without the GL state save/restore (§10.10.2: WebGPU has no
/// global state).

namespace xt::wgpu {

namespace {

const char* backendName(WGPUBackendType type) {
    switch (type) {
        case WGPUBackendType_D3D12:
            return "D3D12";
        case WGPUBackendType_D3D11:
            return "D3D11";
        case WGPUBackendType_Metal:
            return "Metal";
        case WGPUBackendType_Vulkan:
            return "Vulkan";
        case WGPUBackendType_OpenGL:
            return "OpenGL";
        case WGPUBackendType_OpenGLES:
            return "OpenGLES";
        default:
            return "unknown backend";
    }
}

struct MapRequest {
    bool done = false;
    WGPUMapAsyncStatus status = WGPUMapAsyncStatus_Error;
    std::string message;
};

}  // namespace

struct WgpuRenderer::Impl {
    WgpuDevice device;
    gpu::FrameBuilder builder;
    gpu::GpuFrame frame;
    bool initialized = false;
    bool frameReused = false;
    int width = 0, height = 0;
    float buildMs = 0, submitMs = 0;
    std::string adapterName;
    std::string lastError;
    std::string lastWarning;

    bool init(const WgpuInit& in) {
        shutdown();
        lastError.clear();
        if (in.device == nullptr || in.queue == nullptr) {
            lastError = in.error.empty() ? std::string("WgpuRenderer::init: no WebGPU device") : in.error;
            return false;
        }
        std::string err;
        if (!device.init(in.device, in.queue, in.targetFormat, in.sampleCount, err)) {
            lastError = err;
            return false;
        }
        // From the adapter: wgpu-native does not implement wgpuDeviceGetAdapterInfo (it panics).
        WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
        if (in.adapter != nullptr && wgpuAdapterGetInfo(in.adapter, &info) == WGPUStatus_Success) {
            adapterName = toString(info.device) + " (" + backendName(info.backendType) + ")";
            wgpuAdapterInfoFreeMembers(info);
        } else {
            adapterName = "unknown adapter";
        }
        builder.init(device);
        builder.setEdgeFringe(in.sampleCount <= 1);  // a multisampled target resolves edges itself
        initialized = true;
        if (width > 0 && height > 0) {
            resize(width, height);
        }
        return true;
    }

    void shutdown() {
        if (!initialized) {
            return;
        }
        builder.shutdown();
        device.shutdown();
        frame.clear();
        adapterName.clear();
        initialized = false;
    }

    void resize(int w, int h) {
        width = w;
        height = h;
        if (!initialized) {
            return;
        }
        device.resize(w, h);
        builder.resize(w, h);
    }

    void render(const CommandList& list, TextShaper* shaper, AssetResolver* assets, WGPUTextureView target,
                const IrView* ir, PerfTracker* perf, const DamageList* damage) {
        FrameStats* stats = (perf != nullptr && perf->enabled()) ? &perf->current() : nullptr;
        PerfFieldScope renderScope(stats != nullptr ? &stats->renderMs : nullptr);
        lastError.clear();
        buildMs = submitMs = 0;
        if (!initialized) {
            lastError = "WgpuRenderer::init was not called";
            return;
        }
        if (target == nullptr) {
            lastError = "WgpuRenderer::render: no target view";
            return;
        }
        device.setSurface(target);
        frameReused = damage != nullptr && !damage->full && damage->rects.empty() &&
                      builder.lastFrameReusable(assets, ir);
        if (!frameReused) {
            const auto b0 = std::chrono::steady_clock::now();
            builder.build(list, shaper, assets, ir, frame, perf, damage);
            buildMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - b0).count();
        }
        const auto s0 = std::chrono::steady_clock::now();
        device.submit(frame);
        submitMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - s0).count();
        device.setSurface(nullptr);
        lastWarning = device.takeWarning();
        if (stats != nullptr) {
            // T-438 item 14: CPU submission time per kind (21 section 21.2).
            stats->effectsMs += device.lastStats().effectsMs;
            stats->textMs += device.lastStats().textMs;
        }
        const std::string deviceErrors = takeDeviceErrors();
        if (!deviceErrors.empty()) {
            lastError = deviceErrors;
        } else if (builder.lastError()[0] != '\0') {
            lastError = builder.lastError();
        } else if (!device.lastError().empty()) {
            lastError = device.lastError();
        }
    }

    bool readback(WGPUTexture texture, std::span<uint8_t> out) {
        lastError.clear();
        if (!initialized || texture == nullptr) {
            lastError = "readback: no device or no texture";
            return false;
        }
        const WGPUTextureFormat format = wgpuTextureGetFormat(texture);
        const uint32_t bpp = bytesPerTexel(format);
        const uint32_t w = wgpuTextureGetWidth(texture), h = wgpuTextureGetHeight(texture);
        if (bpp == 0 || wgpuTextureGetSampleCount(texture) != 1 ||
            (wgpuTextureGetUsage(texture) & WGPUTextureUsage_CopySrc) == 0) {
            lastError = "readback: the texture is not a single-sample COPY_SRC texture of a readable format";
            return false;
        }
        const size_t rowBytes = static_cast<size_t>(w) * bpp;
        if (out.size() != rowBytes * h) {
            lastError = "readback: output size " + std::to_string(out.size()) + ", texture needs " +
                        std::to_string(rowBytes * h);
            return false;
        }
        const uint32_t paddedRow = static_cast<uint32_t>((rowBytes + 255u) & ~static_cast<size_t>(255u));
        const uint64_t bufferSize = static_cast<uint64_t>(paddedRow) * h;
        WGPUDevice dev = device.device();
        WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
        bd.label = WGPUStringView{"xt_wgpu readback", WGPU_STRLEN};
        bd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
        bd.size = bufferSize;
        WGPUBuffer buffer = wgpuDeviceCreateBuffer(dev, &bd);
        if (buffer == nullptr) {
            lastError = "readback: buffer creation failed";
            return false;
        }
        WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(dev, nullptr);
        WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        src.texture = texture;
        WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
        dst.buffer = buffer;
        dst.layout.bytesPerRow = paddedRow;
        dst.layout.rowsPerImage = h;
        const WGPUExtent3D size{w, h, 1};
        wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &size);
        WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, nullptr);
        wgpuQueueSubmit(device.queue(), 1, &cb);
        wgpuCommandBufferRelease(cb);
        wgpuCommandEncoderRelease(enc);

        MapRequest req;
        WGPUBufferMapCallbackInfo mcb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
        mcb.mode = WGPUCallbackMode_AllowSpontaneous;
        mcb.callback = [](WGPUMapAsyncStatus status, WGPUStringView message, void* u1, void*) {
            auto* r = static_cast<MapRequest*>(u1);
            r->status = status;
            r->message = toString(message);
            r->done = true;
        };
        mcb.userdata1 = &req;
        wgpuBufferMapAsync(buffer, WGPUMapMode_Read, 0, static_cast<size_t>(bufferSize), mcb);
        for (int i = 0; i < 1000 && !req.done; ++i) {
            wgpuDevicePoll(dev, 1, nullptr);
        }
        bool ok = req.done && req.status == WGPUMapAsyncStatus_Success;
        if (ok) {
            const auto* mapped = static_cast<const uint8_t*>(
                wgpuBufferGetConstMappedRange(buffer, 0, static_cast<size_t>(bufferSize)));
            if (mapped == nullptr) {
                ok = false;
                req.message = "no mapped range";
            } else {
                const bool bgra =
                    format == WGPUTextureFormat_BGRA8Unorm || format == WGPUTextureFormat_BGRA8UnormSrgb;
                for (uint32_t y = 0; y < h; ++y) {
                    uint8_t* row = out.data() + rowBytes * y;
                    std::memcpy(row, mapped + static_cast<size_t>(paddedRow) * y, rowBytes);
                    if (bgra) {
                        for (size_t x = 0; x < rowBytes; x += 4) {
                            const uint8_t b = row[x];
                            row[x] = row[x + 2];
                            row[x + 2] = b;
                        }
                    }
                }
            }
            wgpuBufferUnmap(buffer);
        }
        wgpuBufferDestroy(buffer);
        wgpuBufferRelease(buffer);
        const std::string deviceErrors = takeDeviceErrors();
        if (!ok) {
            lastError = "readback: map failed (" + (req.done ? req.message : std::string("no answer")) + ")";
        } else if (!deviceErrors.empty()) {
            lastError = deviceErrors;
            ok = false;
        }
        return ok;
    }
};

WgpuRenderer::WgpuRenderer() : m_impl(std::make_unique<Impl>()) {}
WgpuRenderer::~WgpuRenderer() {
    if (m_impl) {
        m_impl->shutdown();
    }
}
WgpuRenderer::WgpuRenderer(WgpuRenderer&&) noexcept = default;
WgpuRenderer& WgpuRenderer::operator=(WgpuRenderer&&) noexcept = default;

bool WgpuRenderer::init(const WgpuInit& init) { return m_impl->init(init); }
void WgpuRenderer::shutdown() { m_impl->shutdown(); }
void WgpuRenderer::resize(int width, int height) { m_impl->resize(width, height); }

void WgpuRenderer::setClearColor(float r, float g, float b, float a) {
    m_impl->device.setSurfaceClear(true, WGPUColor{r, g, b, a});
}

void WgpuRenderer::disableClear() { m_impl->device.setSurfaceClear(false, WGPUColor{0, 0, 0, 0}); }

void WgpuRenderer::render(const CommandList& list, TextShaper* shaper, AssetResolver* assets,
                          WGPUTextureView target, const IrView* ir, PerfTracker* perf,
                          const DamageList* damage) {
    m_impl->render(list, shaper, assets, target, ir, perf, damage);
}

void WgpuRenderer::invalidateImages() { m_impl->builder.invalidateAllAssets(); }

WgpuStats WgpuRenderer::stats() const {
    const WgpuDeviceStats& d = m_impl->device.lastStats();
    WgpuStats s;
    s.buildMs = m_impl->buildMs;
    s.submitMs = m_impl->submitMs;
    s.passes = d.passes;
    s.clears = d.clears;
    s.skippedCmds = d.skippedCmds;
    s.transientCreates = d.transientCreates;
    s.frameReused = m_impl->frameReused ? 1u : 0u;
    s.drawCalls = d.drawCalls;
    s.pipelineBinds = d.pipelineBinds;
    s.copies = d.copies;
    s.blits = d.blits;
    s.cacheStores = d.cacheStores;
    s.uploadBytes = d.uploadBytes;
    s.planMs = d.planMs;
    s.uploadMs = d.uploadMs;
    s.encodeMs = d.encodeMs;
    s.queueMs = d.queueMs;
    s.effectsMs = d.effectsMs;
    s.textMs = d.textMs;
    return s;
}

const char* WgpuRenderer::adapterName() const { return m_impl->adapterName.c_str(); }

bool WgpuRenderer::readback(WGPUTexture texture, std::span<uint8_t> out) {
    return m_impl->readback(texture, out);
}

const char* WgpuRenderer::lastError() const { return m_impl->lastError.c_str(); }

const char* WgpuRenderer::lastWarning() const { return m_impl->lastWarning.c_str(); }

#ifndef XT_GPU_NO_TEXT
void WgpuRenderer::setGlyphAtlas(xt::text::GlyphAtlas* atlas) { m_impl->builder.setGlyphAtlas(atlas); }
#endif

WgpuDevice* WgpuRenderer::device() { return m_impl->initialized ? &m_impl->device : nullptr; }

}  // namespace xt::wgpu
