#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "capi_internal.h"

#ifdef XT_CAPI_HAS_WGPU
#include <webgpu/webgpu.h>
#include <webgpu/wgpu.h>

#include <cmath>
#include <span>

#include "xt/gpu/gpu_frame.h"
#include "xt/wgpu/wgpu_device.h"
#include "xt/wgpu/wgpu_inflight.h"
#include "xt/wgpu/wgpu_renderer.h"
#include "xt/wgpu/wgpu_shared.h"
#include "xt/wgpu/wgpu_window.h"
#if defined(XT_CAPI_HAS_TEXT) && !defined(XT_GPU_NO_TEXT)
#define XT_CAPI_GPU_ATLAS 1
#include "xt/text/glyph_atlas.h"
#endif
#endif

/// `xt_gpu_target_*`, `xt_surface_render_gpu`, `xt_surface_detach_gpu` (Docs/design/50-c-abi.md §50.z,
/// Docs/tasks/T-636.md). A target owns a WebGPU device (WgpuInit::headless, 10 §10.10.1) and either a
/// window surface (WgpuWindowSurface) or an offscreen RGBA8 texture. Each attached surface renders its
/// CommandList with its own WgpuRenderer into its own texture of the surface's size; the target then
/// composites that texture into `dest` with a one-command GpuFrame (Shading::Texture, LinearClamp,
/// SrcOver) submitted to its composer's WgpuDevice -- the existing draw path, no shader of its own.
/// T-648 (50 §50.z addendum): the target owns a WgpuSharedCache that its composer and every attached
/// surface's renderer borrow -- one pipeline set (compiled once per target) and one texture per bundle
/// image content. T-656 (50 §50.ab item 1): a texture no surface uses any more (detach, trim, destroy)
/// goes to the cache's idle list, bounded by the idle budget (xt_gpu_target_set_image_budget, default
/// 64 MiB); xt_gpu_target_trim empties it and xt_gpu_target_memory_stats reports it. Each surface keeps
/// its own glyph atlas (starting at 1024 x 1024, growing to 2048 x 2048), transient pool and surface
/// texture.
/// Without wgpu (XT_BUILD_WGPU=OFF) every function only reports "built without wgpu".

using xt::capi::fail;

#ifdef XT_CAPI_HAS_WGPU

/// The object behind `xt_gpu_target*`.
struct xt_gpu_target {
    xt::wgpu::WgpuInit init;             ///< instance / adapter / device / queue (owned)
    xt::wgpu::WgpuWindowSurface window;  ///< valid for a window target
    WGPUTexture offscreen = nullptr;     ///< XT_WINDOW_NONE target
    WGPUTextureFormat format = WGPUTextureFormat_RGBA8Unorm;
    /// T-648: pipelines and image textures shared by the composer and the surfaces' renderers;
    /// declared before them, and shut down by the destructor once they are.
    xt::wgpu::WgpuSharedCache shared;
    /// T-648: the shared-cache counters as last logged (xt_surface_render_gpu logs on a change).
    xt::wgpu::WgpuSharedStats logged;
    /// T-656: the GPU resources of the attached surfaces that have them (xt_gpu_target_memory_stats);
    /// each SurfaceGpu adds itself when built and removes itself when destroyed.
    std::vector<xt::capi::SurfaceGpu*> gpus;
    xt::wgpu::WgpuRenderer composer;  ///< its WgpuDevice composites; also readback
    int width = 0, height = 0;
    uint32_t sampleCount = 4;  ///< T-647: the attached surfaces' renderers (1 or 4)
    bool inFrame = false;
    /// T-658 (sheet item 3): begin_frame's clear, submitted with the frame's first composite (or by
    /// end_frame / readback when nothing was composited before them) -- one submit instead of two.
    bool clearPending = false;
    WGPUColor pendingClear = {0, 0, 0, 0};
    /// T-658: the submits and render passes made on this target so far (composer and surface renderers);
    /// logged when a surface stops or starts drawing (tests read them there).
    uint64_t submits = 0, passes = 0;
    /// T-674 (50 §50.ac item 1): at most two frames in flight -- every submit on the target's device
    /// (composer and surface renderers) notes its index; begin_frame waits for the frame two back.
    xt::wgpu::WgpuFramesInFlight inflight;
    WGPUTexture frameTexture = nullptr;   ///< the frame's texture (window's or `offscreen`), not owned
    WGPUTextureView frameView = nullptr;  ///< the frame's view (owned for `offscreen` only)
    uint32_t attached = 0;                ///< surfaces attached (destroy refuses while > 0)
    xt::gpu::GpuFrame frame;              ///< composition scratch

    ~xt_gpu_target() {
        releaseFrame();
        composer.shutdown();
        shared.shutdown();  // every surface is detached (destroy refuses otherwise)
        window.release();
        releaseOffscreen();
        init.release();
    }
    bool windowed() const { return window.valid(); }
    void releaseFrame() {
        if (windowed()) {
            window.releaseFrame();
        } else if (frameView != nullptr) {
            wgpuTextureViewRelease(frameView);
        }
        frameView = nullptr;
        frameTexture = nullptr;
    }
    void releaseOffscreen() {
        if (offscreen != nullptr) {
            wgpuTextureDestroy(offscreen);
            wgpuTextureRelease(offscreen);
            offscreen = nullptr;
        }
    }
    bool createOffscreen() {
        releaseOffscreen();
        offscreen = xt::wgpu::createRenderTexture(init.device, width, height, format, "xt_capi gpu target");
        return offscreen != nullptr;
    }
};

namespace xt::capi {

/// The GPU resources of one attached surface: its renderer on the target's device and the texture it
/// renders into, registered with the target's composer as an external texture.
struct SurfaceGpu {
    xt_gpu_target* target = nullptr;
    wgpu::WgpuRenderer renderer;
#ifdef XT_CAPI_GPU_ATLAS
    std::unique_ptr<text::GlyphAtlas> atlas;
#endif
    WGPUTexture texture = nullptr;
    WGPUTextureView view = nullptr;
    gpu::TextureHandle handle = 0;
    int width = 0, height = 0;
    uint32_t executed = 0, skipped = 0;
    /// T-658: the size the renderer was last resized to (its builder and device).
    int renderWidth = 0, renderHeight = 0;
    /// T-658 (50 §50.ab item 3 ii / iii): a frame was built since these resources were made, and whether that
    /// frame had no draw command -- such a surface is neither drawn nor composited, and its texture (and
    /// the renderer's 4x attachments) are not made until a frame has something to draw.
    bool built = false;
    bool empty = false;
    /// T-662 (10 §10.11 item 2, supplement 2): the last build held a coverage shape back for the
    /// frame budget (`WgpuRenderer::wantsAnotherFrame`), so the next render builds again even
    /// without damage, as if the whole surface were dirty.
    bool repaint = false;

    ~SurfaceGpu() {
        if (target != nullptr) {  // T-656
            target->gpus.erase(std::remove(target->gpus.begin(), target->gpus.end(), this),
                               target->gpus.end());
        }
        releaseTexture();
        renderer.shutdown();
    }
    void releaseTexture() {
        if (handle != 0 && target != nullptr && target->composer.device() != nullptr) {
            target->composer.device()->unregisterExternalTexture(handle);
        }
        handle = 0;
        if (view != nullptr) wgpuTextureViewRelease(view);
        if (texture != nullptr) {
            wgpuTextureDestroy(texture);
            wgpuTextureRelease(texture);
        }
        view = nullptr;
        texture = nullptr;
        width = height = 0;
    }
    /// The texture at `w` x `h` (recreated when the size changed). False when it cannot be made.
    bool ensureTexture(int w, int h, bool& fresh) {
        fresh = false;
        if (texture != nullptr && handle != 0 && w == width && h == height) return true;
        releaseTexture();
        texture = wgpu::createRenderTexture(target->init.device, w, h, WGPUTextureFormat_RGBA8Unorm,
                                            "xt_capi surface");
        if (texture == nullptr) return false;
        view = wgpuTextureCreateView(texture, nullptr);
        handle = target->composer.device()->registerExternalTexture(view, static_cast<uint16_t>(w),
                                                                    static_cast<uint16_t>(h));
        width = w;
        height = h;
        fresh = true;
        return handle != 0;
    }
    /// Resizes the renderer to `w` x `h` when it is not that size yet. True when it was resized.
    bool ensureRenderSize(int w, int h) {
        if (w == renderWidth && h == renderHeight) return false;
        renderer.resize(w, h);
        renderWidth = w;
        renderHeight = h;
        return true;
    }
};

void SurfaceGpuDeleter::operator()(SurfaceGpu* gpu) const noexcept { delete gpu; }

void detachGpu(xt_surface& surface) {
    surface.gpu.reset();  // needs the target (alive: destroy refuses while attached)
    if (surface.gpuTarget != nullptr && surface.gpuTarget->attached > 0) {
        --surface.gpuTarget->attached;
    }
    surface.gpuTarget = nullptr;
    surface.renderPath = RenderPath::None;
    // A later xt_surface_render repaints its whole buffer (the damage since its last SW frame is lost).
    surface.target = sw::SwTarget{};
}

void releaseGpuResources(xt_surface& surface) {
    if (surface.gpu == nullptr) return;
    surface.gpu.reset();  // T-641: gpuTarget, `attached` and renderPath stay (still attached)
    // The memory goes back to the target's device, whose allocator keeps it for the next resources.
    log(0, "xt_surface: GPU resources released (" + surface.uiId + ")");
}

bool gpuDrawCounts(const xt_surface& surface, uint32_t& executed, uint32_t& skipped) {
    if (surface.gpu == nullptr) return false;
    executed = surface.gpu->executed;
    skipped = surface.gpu->skipped;
    return true;
}

}  // namespace xt::capi

namespace {

/// Straight 0..1 clear colour -> premultiplied; NULL = transparent.
WGPUColor clearColor(const float* rgba) {
    if (rgba == nullptr) return WGPUColor{0, 0, 0, 0};
    auto unit = [](float v) { return std::isfinite(v) ? std::clamp(static_cast<double>(v), 0.0, 1.0) : 0.0; };
    const double a = unit(rgba[3]);
    return WGPUColor{unit(rgba[0]) * a, unit(rgba[1]) * a, unit(rgba[2]) * a, a};
}

/// Submits `t->frame` into the frame's view with the composer's device, clearing first when
/// begin_frame's clear is still pending (T-658: the clear rides on the frame's first composite; the
/// same pixels as a clear pass of its own, then a loaded pass). Empty string, or the device's error.
std::string submitComposite(xt_gpu_target& t) {
    xt::wgpu::WgpuDevice* dev = t.composer.device();
    if (dev == nullptr || t.frameView == nullptr) return std::string();
    dev->setSurface(t.frameView);
    dev->setSurfaceClear(t.clearPending, t.pendingClear);
    t.clearPending = false;
    t.frame.targetWidth = t.width;
    t.frame.targetHeight = t.height;
    dev->submit(t.frame);
    dev->setSurfaceClear(false, WGPUColor{0, 0, 0, 0});
    dev->setSurface(nullptr);
    if (dev->lastSubmissionIndex() != 0) t.inflight.noteSubmit(dev->lastSubmissionIndex());  // T-674
    ++t.submits;
    t.passes += dev->lastStats().passes;
    return dev->lastError();
}

/// T-658: begin_frame's clear on its own when no composite carried it (end_frame, readback).
std::string flushPendingClear(xt_gpu_target& t) {
    if (!t.clearPending) return std::string();
    t.frame.clear();  // no pass: the device's own clear pass
    return submitComposite(t);
}

/// One textured quad of `handle` over `dest` (target pixels), scissored to the target.
bool buildCompositeFrame(xt_gpu_target& t, xt::gpu::TextureHandle handle, const xt_rect& dest) {
    xt::gpu::GpuFrame& f = t.frame;
    f.clear();
    const int cx0 = std::clamp(dest.x, 0, t.width), cy0 = std::clamp(dest.y, 0, t.height);
    const int cx1 = static_cast<int>(std::clamp<int64_t>(static_cast<int64_t>(dest.x) + dest.w, 0, t.width));
    const int cy1 = static_cast<int>(std::clamp<int64_t>(static_cast<int64_t>(dest.y) + dest.h, 0, t.height));
    if (cx1 <= cx0 || cy1 <= cy0) return false;  // entirely outside the target
    const float x0 = static_cast<float>(dest.x), y0 = static_cast<float>(dest.y);
    const float x1 = x0 + static_cast<float>(dest.w), y1 = y0 + static_cast<float>(dest.h);
    const uint32_t white = 0xFFFFFFFFu;
    f.vertices = {{x0, y0, 1, 0, 0, white},
                  {x1, y0, 1, 1, 0, white},
                  {x1, y1, 1, 1, 1, white},
                  {x0, y1, 1, 0, 1, white}};
    f.indices = {0, 1, 2, 0, 2, 3};
    xt::gpu::DrawCmd cmd;
    cmd.indexCount = 6;
    cmd.vertexCount = 4;
    cmd.texture = handle;
    cmd.shading = xt::gpu::Shading::Texture;
    cmd.blend = xt::gpu::Blend::SrcOver;
    cmd.sampler = xt::gpu::Sampler::LinearClamp;
    cmd.clipX = static_cast<int16_t>(std::min(cx0, 32767));
    cmd.clipY = static_cast<int16_t>(std::min(cy0, 32767));
    cmd.clipW = static_cast<int16_t>(std::min(cx1 - cx0, 32767));
    cmd.clipH = static_cast<int16_t>(std::min(cy1 - cy0, 32767));
    f.cmds.push_back(cmd);
    xt::gpu::Pass pass;
    pass.target = xt::gpu::kSurfaceTarget;
    pass.cmdCount = 1;
    f.passes.push_back(pass);
    return true;
}

/// The largest surface / target side a texture handle and a scissor can describe.
constexpr int kMaxSide = 16384;

#ifdef XT_CAPI_GPU_ATLAS
/// T-648 (50 §50.z addendum): a surface's glyph atlas pages -- the first one starts at kAtlasFirstPage
/// and grows to kAtlasPage when full.
constexpr int kAtlasPage = 2048;
constexpr int kAtlasFirstPage = 1024;
#endif

/// T-648: logs the target's shared-cache counters when a texture was created, reused or freed, or a
/// pipeline set made or borrowed, since the last line (info level; tests count them).
void logSharedCache(xt_gpu_target& t) {
    const xt::wgpu::WgpuSharedStats s = t.shared.stats();
    const xt::wgpu::WgpuSharedStats& l = t.logged;
    if (s.imageCreates == l.imageCreates && s.imageReuses == l.imageReuses && s.imageFrees == l.imageFrees &&
        s.pipelineSets == l.pipelineSets && s.pipelineBorrows == l.pipelineBorrows) {
        return;
    }
    t.logged = s;
    xt::capi::log(0, "xt_gpu_target: shared image textures " + std::to_string(s.imageTextures) + " (" +
                         std::to_string(s.imageBytes / 1024u) + " KiB; uploaded " +
                         std::to_string(s.imageCreates) + ", reused " + std::to_string(s.imageReuses) +
                         ", freed " + std::to_string(s.imageFrees) + "); pipeline sets " +
                         std::to_string(s.pipelineSets) + ", borrowed " + std::to_string(s.pipelineBorrows));
}

/// T-656: the counters of xt_gpu_target_memory_stats (50 §50.ab item 1), all fields.
xt_gpu_memory_stats collectMemoryStats(const xt_gpu_target& t) {
    xt_gpu_memory_stats m;
    std::memset(&m, 0, sizeof m);
    const xt::wgpu::WgpuSharedStats s = t.shared.stats();
    m.image_textures = s.imageTextures;
    m.image_bytes = s.imageBytes;
    m.image_idle_textures = s.imageIdleTextures;
    m.image_idle_bytes = s.imageIdleBytes;
    m.image_evictions = s.imageEvictions;
    m.attached_surfaces = t.attached;
    for (xt::capi::SurfaceGpu* g : t.gpus) {
        if (g->texture != nullptr) {  // WGPUTextureFormat_RGBA8Unorm, one sample
            m.surface_bytes += static_cast<uint64_t>(g->width) * static_cast<uint64_t>(g->height) * 4u;
        }
        if (xt::wgpu::WgpuDevice* dev = g->renderer.device()) {
            m.surface_bytes += dev->surfaceAttachmentBytes();
            m.transient_bytes += dev->lastStats().poolBytes;
        }
        m.atlas_bytes += g->renderer.stats().coverageAtlasBytes;  // T-660b: coverage atlas pages (R8)
#ifdef XT_CAPI_GPU_ATLAS
        if (g->atlas != nullptr) {
            const auto side = static_cast<uint64_t>(g->atlas->textureSize());
            m.atlas_bytes += static_cast<uint64_t>(g->atlas->textureCount()) * side * side;  // R8 pages
        }
#endif
    }
    // The composer's pool too (read only; WgpuRenderer::device() has no const overload).
    if (xt::wgpu::WgpuDevice* dev = const_cast<xt_gpu_target&>(t).composer.device()) {
        m.transient_bytes += dev->lastStats().poolBytes;
    }
    return m;
}

/// Builds the GPU resources of `s` on `t` (its renderer on the target's device, its glyph atlas):
/// on first attach, and again after a release (T-641). Empty, or why not.
std::string buildSurfaceGpu(xt_surface& s, xt_gpu_target& t) {
    std::unique_ptr<xt::capi::SurfaceGpu, xt::capi::SurfaceGpuDeleter> gpu(new xt::capi::SurfaceGpu);
    gpu->target = &t;
    t.gpus.push_back(gpu.get());  // T-656: removed again by ~SurfaceGpu
    xt::wgpu::WgpuInit in = t.init;
    in.targetFormat = WGPUTextureFormat_RGBA8Unorm;
    // T-647 (50 §50.z addendum): at 4 the renderer draws into its own 4x colour and stencil attachments
    // and resolves into the surface texture (WgpuDevice::ensureSurfaceAttachments), which is what the
    // composer samples and readback sees -- for window and offscreen targets alike.
    in.sampleCount = t.sampleCount;
    in.shared = &t.shared;  // T-648: the target's pipelines and image textures
    in.error.clear();
    if (!gpu->renderer.init(in)) return std::string("WgpuRenderer::init: ") + gpu->renderer.lastError();
#ifdef XT_CAPI_GPU_ATLAS
    if (s.owner->ftShaper != nullptr) {
        // T-648: one 1024 x 1024 page first, grown to the full 2048 x 2048 once it is full.
        gpu->atlas = std::make_unique<xt::text::GlyphAtlas>(*s.owner->ftShaper, kAtlasPage, kAtlasFirstPage);
        gpu->renderer.setGlyphAtlas(gpu->atlas.get());
    }
#endif
    s.gpu = std::move(gpu);
    return std::string();
}

/// Attaches `s` to `t` (the render path; the resources are built by buildSurfaceGpu).
void attachSurface(xt_surface& s, xt_gpu_target& t) {
    s.gpuTarget = &t;
    ++t.attached;
    s.renderPath = xt::capi::RenderPath::Gpu;
}

}  // namespace

extern "C" {

XT_CAPI xt_gpu_target* xt_gpu_target_create(const xt_gpu_target_desc* desc) {
    XT_CAPI_GUARD_OR("xt_gpu_target_create", xt_gpu_target*, nullptr, {
        auto failNull = [](const std::string& message) -> xt_gpu_target* {
            xt::capi::setLastError("xt_gpu_target_create: " + message);
            return nullptr;
        };
        if (desc == nullptr) return failNull("desc is NULL");
        // T-647: sample_count was appended; a desc ending before it (an older host) still creates.
        constexpr size_t kMinDescSize = offsetof(xt_gpu_target_desc, sample_count);
        if (desc->struct_size < kMinDescSize) {
            return failNull("desc->struct_size is smaller than sizeof(xt_gpu_target_desc)");
        }
        const int samples = desc->struct_size >= sizeof(xt_gpu_target_desc) ? desc->sample_count : 0;
        if (samples != 0 && samples != 1 && samples != 4) {
            return failNull("sample_count " + std::to_string(samples) + " (allowed: 0 = 4, 1, 4)");
        }
        if (desc->width <= 0 || desc->height <= 0 || desc->width > kMaxSide || desc->height > kMaxSide) {
            return failNull("width / height must be in 1.." + std::to_string(kMaxSide));
        }
        xt::wgpu::WgpuNativeWindow native;
        const xt_native_window& w = desc->window;
        switch (w.kind) {
            case XT_WINDOW_NONE:
                break;
            case XT_WINDOW_WIN32:
                native.kind = xt::wgpu::WgpuNativeWindow::Kind::Win32;
                native.window = w.u.win32.hwnd;
                break;
            case XT_WINDOW_COCOA:
                native.kind = xt::wgpu::WgpuNativeWindow::Kind::CocoaWindow;
                native.window = w.u.cocoa.ns_window;
                break;
            case XT_WINDOW_X11:
                native.kind = xt::wgpu::WgpuNativeWindow::Kind::X11;
                native.display = w.u.x11.display;
                native.x11Window = w.u.x11.window;
                break;
            case XT_WINDOW_WAYLAND:
                native.kind = xt::wgpu::WgpuNativeWindow::Kind::Wayland;
                native.display = w.u.wayland.display;
                native.window = w.u.wayland.surface;
                break;
            default:
                return failNull("unknown window kind " + std::to_string(w.kind));
        }
        auto t = std::make_unique<xt_gpu_target>();
        t->width = desc->width;
        t->height = desc->height;
        t->sampleCount = samples == 1 ? 1u : 4u;
        xt::wgpu::WgpuHeadlessOptions options;
        options.backend = desc->backend != nullptr ? desc->backend : "";
        t->init = xt::wgpu::WgpuInit::headless(options);
        if (t->init.device == nullptr) return failNull(t->init.error);
        // T-671 (50 §50.ab addendum 2, items 1 and 5): the adapter and the memory strategy in effect.
        for (size_t at = 0; at < t->init.notes.size();) {
            const size_t nl = std::min(t->init.notes.find('\n', at), t->init.notes.size());
            xt::capi::log(0, "xt_gpu_target: " + t->init.notes.substr(at, nl - at));
            at = nl + 1;
        }
        if (native.kind != xt::wgpu::WgpuNativeWindow::Kind::None) {
            std::string error;
            if (!t->window.create(t->init, native, desc->vsync != 0, error)) return failNull(error);
            t->format = t->window.format();
            t->window.configure(t->init.device, t->width, t->height);
        } else {
            t->format = WGPUTextureFormat_RGBA8Unorm;
            if (!t->createOffscreen()) return failNull("the offscreen texture cannot be created");
        }
        xt::wgpu::WgpuInit in = t->init;
        in.targetFormat = t->format;
        in.sampleCount = 1;      // one axis-aligned textured quad per surface: nothing to antialias
        in.shared = &t->shared;  // T-648: the composer makes the target's pipeline set
        in.error.clear();
        if (!t->composer.init(in))
            return failNull(std::string("WgpuRenderer::init: ") + t->composer.lastError());
        t->composer.resize(t->width, t->height);
        return t.release();
    });
}

XT_CAPI xt_status xt_gpu_target_destroy(xt_gpu_target* t) {
    XT_CAPI_GUARD("xt_gpu_target_destroy", {
        if (t == nullptr) return XT_STATUS_OK;
        if (t->attached > 0) {
            return fail(XT_STATUS_BUSY, "xt_gpu_target_destroy: " + std::to_string(t->attached) +
                                            " surface(s) still attached; destroy them or call "
                                            "xt_surface_detach_gpu first");
        }
        t->inflight.waitIdle(t->init.device);  // T-674: the GPU is done with everything before it is freed
        const uint32_t images = t->shared.stats().imageTextures;
        // T-674: the frames-in-flight counters and the target's submit / pass totals, for tests and tools.
        char waitMs[32];
        std::snprintf(waitMs, sizeof waitMs, "%.3f", t->inflight.waitMs());
        const std::string inflight =
            "; frames in flight: " + std::to_string(t->inflight.waits()) + " wait(s), " + waitMs +
            " ms, peak " + std::to_string(t->inflight.peakInFlight()) + "; " + std::to_string(t->submits) +
            " submits, " + std::to_string(t->passes) + " passes";
        delete t;
        xt::capi::log(0, "xt_gpu_target: destroyed; " + std::to_string(images) +
                             " shared image texture(s) freed" + inflight);
        return XT_STATUS_OK;
    });
}

XT_CAPI xt_status xt_gpu_target_resize(xt_gpu_target* t, int width, int height) {
    XT_CAPI_GUARD("xt_gpu_target_resize", {
        if (t == nullptr) return fail(XT_STATUS_INVALID_ARGUMENT, "xt_gpu_target_resize: target is NULL");
        if (width <= 0 || height <= 0 || width > kMaxSide || height > kMaxSide) {
            return fail(XT_STATUS_INVALID_ARGUMENT,
                        "xt_gpu_target_resize: width / height must be in 1.." + std::to_string(kMaxSide));
        }
        if (t->inFrame) {
            return fail(XT_STATUS_INVALID_ARGUMENT, "xt_gpu_target_resize: called inside a frame");
        }
        if (width == t->width && height == t->height) return XT_STATUS_OK;
        t->width = width;
        t->height = height;
        if (t->windowed()) {
            t->window.configure(t->init.device, width, height);
        } else if (!t->createOffscreen()) {
            return fail(XT_STATUS_INTERNAL, "xt_gpu_target_resize: the offscreen texture cannot be created");
        }
        t->composer.resize(width, height);
        return XT_STATUS_OK;
    });
}

XT_CAPI xt_status xt_gpu_target_begin_frame(xt_gpu_target* t, const float clear_rgba[4]) {
    XT_CAPI_GUARD("xt_gpu_target_begin_frame", {
        if (t == nullptr)
            return fail(XT_STATUS_INVALID_ARGUMENT, "xt_gpu_target_begin_frame: target is NULL");
        if (t->inFrame) {
            return fail(
                XT_STATUS_INVALID_ARGUMENT,
                "xt_gpu_target_begin_frame: the previous frame was not ended (xt_gpu_target_end_frame)");
        }
        // T-674 (50 §50.ac item 1): waits only while the GPU is more than two frames behind.
        t->inflight.beginFrame(t->init.device, t->init.queue);
        const std::string inflightWarning = t->inflight.takeWarning();
        if (!inflightWarning.empty()) xt::capi::log(1, "xt_gpu_target_begin_frame: " + inflightWarning);
        t->inFrame = true;
        if (t->windowed()) {
            if (t->window.acquire()) {  // else: skipped (minimized, timeout)
                t->frameTexture = t->window.frameTexture();
                t->frameView = t->window.frameView();
            }
        } else {
            t->frameTexture = t->offscreen;
            t->frameView = wgpuTextureCreateView(t->offscreen, nullptr);
        }
        // T-658: submitted with the first composite of the frame, or by end_frame / readback.
        t->pendingClear = clearColor(clear_rgba);
        t->clearPending = t->frameView != nullptr;
        return XT_STATUS_OK;
    });
}

XT_CAPI xt_status xt_gpu_target_end_frame(xt_gpu_target* t) {
    XT_CAPI_GUARD("xt_gpu_target_end_frame", {
        if (t == nullptr) return fail(XT_STATUS_INVALID_ARGUMENT, "xt_gpu_target_end_frame: target is NULL");
        if (!t->inFrame) {
            return fail(XT_STATUS_INVALID_ARGUMENT, "xt_gpu_target_end_frame: no frame was begun");
        }
        const std::string error = flushPendingClear(*t);  // T-658: nothing was composited
        if (t->windowed()) {
            t->window.present();  // releases the frame's texture
            t->frameView = nullptr;
            t->frameTexture = nullptr;
        } else {
            t->releaseFrame();  // the work is already submitted
        }
        t->inFrame = false;
        t->inflight.endFrame(t->init.queue);         // T-674: the frame's last submission index
        wgpuDevicePoll(t->init.device, 0, nullptr);  // let finished work be reclaimed, never waits
        if (!error.empty()) return fail(XT_STATUS_INTERNAL, "xt_gpu_target_end_frame: " + error);
        return XT_STATUS_OK;
    });
}

XT_CAPI xt_status xt_gpu_target_readback(xt_gpu_target* t, void* rgba, int stride) {
    XT_CAPI_GUARD("xt_gpu_target_readback", {
        if (t == nullptr) return fail(XT_STATUS_INVALID_ARGUMENT, "xt_gpu_target_readback: target is NULL");
        const size_t rowBytes = static_cast<size_t>(t->width) * 4u;
        if (rgba == nullptr || stride < 0 || static_cast<size_t>(stride) < rowBytes) {
            return fail(XT_STATUS_INVALID_ARGUMENT,
                        "xt_gpu_target_readback: rgba is NULL or stride < width * 4");
        }
        WGPUTexture texture = t->windowed() ? t->frameTexture : t->offscreen;
        if (texture == nullptr) {
            return fail(
                XT_STATUS_UNSUPPORTED,
                "xt_gpu_target_readback: a window target is read only between begin_frame and end_frame");
        }
        if (t->windowed() && (t->window.usage() & WGPUTextureUsage_CopySrc) == 0) {
            return fail(XT_STATUS_UNSUPPORTED,
                        "xt_gpu_target_readback: this window's swapchain cannot be copied");
        }
        // T-658: inside a frame whose clear no composite has carried yet, the clear comes first.
        const std::string clearError = flushPendingClear(*t);
        if (!clearError.empty()) {
            return fail(XT_STATUS_INTERNAL, "xt_gpu_target_readback: " + clearError);
        }
        std::vector<uint8_t> tight(rowBytes * static_cast<size_t>(t->height));
        if (!t->composer.readback(texture, std::span<uint8_t>(tight))) {
            return fail(XT_STATUS_INTERNAL,
                        std::string("xt_gpu_target_readback: ") + t->composer.lastError());
        }
        auto* out = static_cast<uint8_t*>(rgba);
        for (int y = 0; y < t->height; ++y) {
            std::memcpy(out + static_cast<size_t>(y) * static_cast<size_t>(stride),
                        tight.data() + rowBytes * static_cast<size_t>(y), rowBytes);
        }
        return XT_STATUS_OK;
    });
}

XT_CAPI xt_status xt_surface_render_gpu(xt_surface* s, xt_gpu_target* t, const xt_rect* dest) {
    XT_CAPI_GUARD("xt_surface_render_gpu", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_render_gpu");
        if (t == nullptr) return fail(XT_STATUS_INVALID_ARGUMENT, "xt_surface_render_gpu: target is NULL");
        if (s->renderPath == xt::capi::RenderPath::Software) {
            return fail(XT_STATUS_UNSUPPORTED,
                        "xt_surface_render_gpu: the surface renders with xt_surface_render; call "
                        "xt_surface_detach_gpu first");
        }
        if (s->gpuTarget != nullptr && s->gpuTarget != t) {
            return fail(XT_STATUS_INVALID_ARGUMENT,
                        "xt_surface_render_gpu: the surface is attached to another target; call "
                        "xt_surface_detach_gpu first");
        }
        if (!t->inFrame) {
            return fail(XT_STATUS_INVALID_ARGUMENT,
                        "xt_surface_render_gpu: called outside xt_gpu_target_begin_frame / end_frame");
        }
        const xt_rect rect = dest != nullptr ? *dest : xt_rect{0, 0, t->width, t->height};
        if (rect.w <= 0 || rect.h <= 0) {
            return fail(XT_STATUS_INVALID_ARGUMENT, "xt_surface_render_gpu: dest has no area");
        }
        if (s->width > kMaxSide || s->height > kMaxSide) {
            return fail(XT_STATUS_INVALID_ARGUMENT, "xt_surface_render_gpu: the surface is larger than " +
                                                        std::to_string(kMaxSide) + " px");
        }
        if (s->gpu == nullptr && s->gpuTarget != nullptr && !s->ctx->isVisible(s->handle)) {
            // T-641 (50 §50.aa item 4): released while hidden; a hidden surface paints nothing, so
            // its resources are rebuilt only once it has something to draw again.
            xt::capi::DispatchScope scope(*s);
            s->ctx->render(s->list, &s->damage);
            if (s->list.commands.empty()) return XT_STATUS_OK;
        }
        if (s->gpu == nullptr) {
            const std::string error = buildSurfaceGpu(*s, *t);
            if (!error.empty()) return fail(XT_STATUS_INTERNAL, "xt_surface_render_gpu: " + error);
            if (s->gpuTarget == nullptr) {
                attachSurface(*s, *t);
            } else {
                xt::capi::log(0, "xt_surface_render_gpu: GPU resources rebuilt (" + s->uiId + ")");
            }
        }
        xt::capi::SurfaceGpu& g = *s->gpu;
        const bool resized = g.ensureRenderSize(s->width, s->height);
        {
            xt::capi::DispatchScope scope(*s);
            s->ctx->render(s->list, &s->damage);
            // The texture keeps the last frame: an unchanged frame is not drawn again. Any change redraws
            // the whole texture (cleared), so the partial-damage path of the builder is not needed.
            // T-662: a frame whose budget held a coverage shape back is drawn again until none is.
            const bool unchanged =
                g.built && !resized && !g.repaint && !s->damage.full && s->damage.rects.empty();
            if (!unchanged) {
                xt::PerfTracker* perf = &s->ctx->perf();
                const uint32_t draws = g.renderer.build(s->list, s->owner->shaper.get(),
                                                        &s->owner->bundle->assets(), nullptr, perf, nullptr);
                const bool wasEmpty = g.built && g.empty;
                g.built = true;
                g.repaint = g.renderer.wantsAnotherFrame();
                g.empty = draws == 0;
                if (g.empty) {
                    // T-658 (50 §50.ab item 3 ii / iii): nothing to draw -- no texture, no 4x attachments,
                    // no submit and no composite until a frame has something.
                    g.executed = g.skipped = 0;
                    if (!wasEmpty) {
                        xt::capi::log(0, "xt_surface_render_gpu: " + s->uiId +
                                             " has nothing to draw; not composited (" +
                                             (g.texture != nullptr ? "texture kept" : "no texture") +
                                             "; target " + std::to_string(t->submits) + " submits, " +
                                             std::to_string(t->passes) + " passes)");
                    }
                } else {
                    if (wasEmpty) {
                        xt::capi::log(0, "xt_surface_render_gpu: " + s->uiId + " draws again (target " +
                                             std::to_string(t->submits) + " submits, " +
                                             std::to_string(t->passes) + " passes)");
                    }
                    bool fresh = false;
                    if (!g.ensureTexture(s->width, s->height, fresh)) {
                        g.built = false;  // drawn by the next call
                        return fail(XT_STATUS_INTERNAL,
                                    "xt_surface_render_gpu: the surface texture cannot be created");
                    }
                    g.renderer.setClearColor(0, 0, 0, 0);
                    g.renderer.submit(g.view, perf);
                    const xt::wgpu::WgpuStats stats = g.renderer.stats();
                    g.executed = stats.drawCalls;
                    g.skipped = stats.skippedCmds;
                    if (xt::wgpu::WgpuDevice* d = g.renderer.device();
                        d != nullptr && d->lastSubmissionIndex() != 0) {
                        t->inflight.noteSubmit(d->lastSubmissionIndex());  // T-674
                    }
                    ++t->submits;
                    t->passes += stats.passes;
                }
                logSharedCache(*t);
                if (g.renderer.lastError()[0] != '\0') {
                    xt::capi::log(2, std::string("xt_surface_render_gpu: ") + g.renderer.lastError());
                }
            }
        }
        if (!g.empty && t->frameView != nullptr && buildCompositeFrame(*t, g.handle, rect)) {
            const std::string error = submitComposite(*t);
            if (!error.empty()) return fail(XT_STATUS_INTERNAL, "xt_surface_render_gpu: " + error);
        }
        return XT_STATUS_OK;
    });
}

XT_CAPI xt_status xt_surface_detach_gpu(xt_surface* s) {
    XT_CAPI_GUARD("xt_surface_detach_gpu", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_detach_gpu");
        xt::capi::detachGpu(*s);
        return XT_STATUS_OK;
    });
}

// -- T-656 (50 §50.ab item 1): the target's image budget, trim and memory counters. --

XT_CAPI xt_status xt_gpu_target_set_image_budget(xt_gpu_target* t, uint64_t idle_bytes) {
    XT_CAPI_GUARD("xt_gpu_target_set_image_budget", {
        if (t == nullptr)
            return fail(XT_STATUS_INVALID_ARGUMENT, "xt_gpu_target_set_image_budget: target is NULL");
        t->shared.setImageIdleBudget(idle_bytes);
        logSharedCache(*t);
        return XT_STATUS_OK;
    });
}

XT_CAPI xt_status xt_gpu_target_trim(xt_gpu_target* t) {
    XT_CAPI_GUARD("xt_gpu_target_trim", {
        if (t == nullptr) return fail(XT_STATUS_INVALID_ARGUMENT, "xt_gpu_target_trim: target is NULL");
        if (t->inFrame) return fail(XT_STATUS_INVALID_ARGUMENT, "xt_gpu_target_trim: called inside a frame");
        t->inflight.waitIdle(t->init.device);  // T-674: the GPU is done with everything before it is freed
        const uint32_t freed = t->shared.trimIdleImages();
        wgpuDevicePoll(t->init.device, 0, nullptr);  // let the driver reclaim it, never waits
        logSharedCache(*t);
        xt::capi::log(0, "xt_gpu_target: trimmed; " + std::to_string(freed) + " idle image texture(s) freed");
        return XT_STATUS_OK;
    });
}

XT_CAPI xt_status xt_gpu_target_memory_stats(const xt_gpu_target* t, xt_gpu_memory_stats* out) {
    XT_CAPI_GUARD("xt_gpu_target_memory_stats", {
        if (t == nullptr || out == nullptr) {
            return fail(XT_STATUS_INVALID_ARGUMENT, "xt_gpu_target_memory_stats: target or out is NULL");
        }
        const xt_gpu_memory_stats m = collectMemoryStats(*t);
        // Only the fields `struct_size` covers (struct_size itself is the caller's).
        const size_t size = std::min<size_t>(out->struct_size, sizeof m);
        if (size > sizeof m.struct_size) {
            std::memcpy(reinterpret_cast<uint8_t*>(out) + sizeof m.struct_size,
                        reinterpret_cast<const uint8_t*>(&m) + sizeof m.struct_size,
                        size - sizeof m.struct_size);
        }
        return XT_STATUS_OK;
    });
}

}  // extern "C"

#else  // !XT_CAPI_HAS_WGPU

/// Never created: xt_gpu_target_create fails in a build without wgpu.
struct xt_gpu_target {};

namespace xt::capi {

struct SurfaceGpu {};

void SurfaceGpuDeleter::operator()(SurfaceGpu* gpu) const noexcept { delete gpu; }

void detachGpu(xt_surface& surface) {
    surface.gpu.reset();
    surface.gpuTarget = nullptr;
    surface.renderPath = RenderPath::None;
}

void releaseGpuResources(xt_surface& surface) { surface.gpu.reset(); }

bool gpuDrawCounts(const xt_surface& /*surface*/, uint32_t& /*executed*/, uint32_t& /*skipped*/) {
    return false;
}

}  // namespace xt::capi

namespace {

xt_status withoutWgpu(const char* function) {
    return fail(XT_STATUS_UNSUPPORTED, std::string(function) + ": built without wgpu");
}

}  // namespace

extern "C" {

XT_CAPI xt_gpu_target* xt_gpu_target_create(const xt_gpu_target_desc* /*desc*/) {
    XT_CAPI_GUARD_OR("xt_gpu_target_create", xt_gpu_target*, nullptr, {
        xt::capi::setLastError("xt_gpu_target_create: built without wgpu");
        return nullptr;
    });
}

XT_CAPI xt_status xt_gpu_target_destroy(xt_gpu_target* /*t*/) {
    XT_CAPI_GUARD("xt_gpu_target_destroy", { return withoutWgpu("xt_gpu_target_destroy"); });
}

XT_CAPI xt_status xt_gpu_target_resize(xt_gpu_target* /*t*/, int /*width*/, int /*height*/) {
    XT_CAPI_GUARD("xt_gpu_target_resize", { return withoutWgpu("xt_gpu_target_resize"); });
}

XT_CAPI xt_status xt_gpu_target_begin_frame(xt_gpu_target* /*t*/, const float /*clear_rgba*/[4]) {
    XT_CAPI_GUARD("xt_gpu_target_begin_frame", { return withoutWgpu("xt_gpu_target_begin_frame"); });
}

XT_CAPI xt_status xt_gpu_target_end_frame(xt_gpu_target* /*t*/) {
    XT_CAPI_GUARD("xt_gpu_target_end_frame", { return withoutWgpu("xt_gpu_target_end_frame"); });
}

XT_CAPI xt_status xt_gpu_target_readback(xt_gpu_target* /*t*/, void* /*rgba*/, int /*stride*/) {
    XT_CAPI_GUARD("xt_gpu_target_readback", { return withoutWgpu("xt_gpu_target_readback"); });
}

XT_CAPI xt_status xt_surface_render_gpu(xt_surface* /*s*/, xt_gpu_target* /*t*/, const xt_rect* /*dest*/) {
    XT_CAPI_GUARD("xt_surface_render_gpu", { return withoutWgpu("xt_surface_render_gpu"); });
}

XT_CAPI xt_status xt_surface_detach_gpu(xt_surface* /*s*/) {
    XT_CAPI_GUARD("xt_surface_detach_gpu", { return withoutWgpu("xt_surface_detach_gpu"); });
}

XT_CAPI xt_status xt_gpu_target_set_image_budget(xt_gpu_target* /*t*/, uint64_t /*idle_bytes*/) {
    XT_CAPI_GUARD("xt_gpu_target_set_image_budget",
                  { return withoutWgpu("xt_gpu_target_set_image_budget"); });
}

XT_CAPI xt_status xt_gpu_target_trim(xt_gpu_target* /*t*/) {
    XT_CAPI_GUARD("xt_gpu_target_trim", { return withoutWgpu("xt_gpu_target_trim"); });
}

XT_CAPI xt_status xt_gpu_target_memory_stats(const xt_gpu_target* /*t*/, xt_gpu_memory_stats* /*out*/) {
    XT_CAPI_GUARD("xt_gpu_target_memory_stats", { return withoutWgpu("xt_gpu_target_memory_stats"); });
}

}  // extern "C"

#endif  // XT_CAPI_HAS_WGPU
