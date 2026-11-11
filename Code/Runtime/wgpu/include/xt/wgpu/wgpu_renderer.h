#pragma once

#include <webgpu/webgpu.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "xt/command_list.h"
#include "xt/damage.h"
#include "xt/ir.h"
#include "xt/perf.h"
#include "xt/wgpu/wgpu_inflight.h"

/// WebGPU backend (Docs/design/10-backends.md §10.10, T-437): `CommandList` -> `gpu::FrameBuilder`
/// -> `GpuFrame` -> `WgpuDevice::submit`, with the interface of `GlRenderer` (§10.3) where the two
/// overlap. A failure to find an adapter or create a device is reported (`init` returns false,
/// `lastError()`), never fatal.
///
/// T-437 built the skeleton (device, textures, pass clears, `readback`); T-438 draws every command
/// (`WgpuDevice::submit`).

namespace xt {
class TextShaper;
class AssetResolver;
}  // namespace xt

#ifndef XT_GPU_NO_TEXT
namespace xt::text {
class GlyphAtlas;
}  // namespace xt::text
#endif

namespace xt::wgpu {

class WgpuDevice;
class WgpuSharedCache;

/// Options of `WgpuInit::headless`.
struct WgpuHeadlessOptions {
    /// Format and sample count (1 or 4) of the targets the renderer will draw into.
    WGPUTextureFormat targetFormat = WGPUTextureFormat_RGBA8Unorm;
    uint32_t sampleCount = 1;
    /// Same as XT_WGPU_FALLBACK=1: ask for the software adapter (WARP, lavapipe) first, then any.
    bool allowFallback = false;
    /// T-636 (50 §50.z `xt_gpu_target_desc::backend`): vulkan|d3d12|metal|gl in place of
    /// XT_WGPU_BACKEND; empty = read XT_WGPU_BACKEND as before. Any other name fails.
    std::string backend;
};

/// What `WgpuRenderer::init` draws with: a device and its queue (the renderer takes its own
/// references), the format of the target views given to `render`, and its sample count (1 or 4).
/// A host fills `device`, `queue`, `targetFormat` and `sampleCount` itself; `headless()` creates
/// everything and also fills `instance` and `adapter`, which `release()` gives back.
struct WgpuInit {
    WGPUDevice device = nullptr;
    WGPUQueue queue = nullptr;
    WGPUTextureFormat targetFormat = WGPUTextureFormat_RGBA8Unorm;
    uint32_t sampleCount = 1;
    WGPUInstance instance = nullptr;
    WGPUAdapter adapter = nullptr;
    /// T-648 (wgpu_shared.h): the cache whose pipeline set and image textures the renderer's device
    /// shares with the other renderers on `device`; null (the default) = none. Not owned: it must
    /// outlive the renderer's `shutdown`. `headless()` leaves it null and `release()` ignores it.
    WgpuSharedCache* shared = nullptr;
    /// Why `headless()` returned no device (empty otherwise).
    std::string error;
    /// T-671 (50 §50.ab addendum 2, items 1 and 5): what `headless()` chose, one line each, for the
    /// caller's log: the adapter (name, backend, integrated/discrete, XT_WGPU_POWER), then the
    /// wgpu-native version and the memory hints in effect (XT_WGPU_MEMORY_HINTS).
    std::string notes;

    /// Creates an instance, requests an adapter and a device and waits for both (§10.10.1):
    ///   - XT_WGPU_BACKEND=vulkan|d3d12|metal|gl (or `options.backend` when not empty) limits the
    ///     instance to that backend (any other value fails with an error naming it); unset = the
    ///     platform's primary backends (T-670: on Windows a D3D12-only instance first, and the
    ///     primary backends only when it gives no adapter or device);
    ///   - the device requires maxNonSamplerBindings 131072 (T-670), and the base limits when that
    ///     request fails;
    ///   - a software adapter is used only with XT_WGPU_FALLBACK=1 (or `allowFallback`), which
    ///     asks for it first (forceFallbackAdapter) and takes any adapter when there is none;
    ///   - XT_WGPU_POWER=low|high picks the adapter's power preference (unset = high performance);
    ///   - only with a wgpu-native that has memory hints (XT_WGPU_HAS_MEMORY_HINTS, T-671):
    ///     XT_WGPU_MEMORY_HINTS=performance|usage|manual:<startMiB>:<maxMiB>, unset = usage.
    /// On failure `device` is null and `error` says why; whatever was created is released.
    static WgpuInit headless(const WgpuHeadlessOptions& options = {});
    /// Releases the references `headless()` created (device, queue, adapter, instance) and nulls
    /// them. A renderer initialized from this WgpuInit keeps its own references.
    void release();
};

/// Counters of the last `render` (not a stable API).
struct WgpuStats {
    float buildMs = 0;              ///< FrameBuilder::build (0 when the frame was reused)
    float submitMs = 0;             ///< WgpuDevice::submit (CPU encoding and queue submit)
    uint32_t passes = 0;            ///< render passes encoded
    uint32_t resolves = 0;          ///< T-658: 4x resolves into the target view (WgpuDeviceStats)
    uint32_t clears = 0;            ///< passes that cleared their color attachment
    uint32_t skippedCmds = 0;       ///< draw commands that could not be executed
    uint32_t transientCreates = 0;  ///< transient targets created this frame
    uint32_t frameReused = 0;       ///< 1 when the previous frame was submitted again without a build
    uint32_t drawCalls = 0;         ///< draw / drawIndexed calls (T-438)
    uint32_t pipelineBinds = 0;     ///< setPipeline calls
    uint32_t copies = 0;            ///< copy passes done with copyTextureToTexture
    uint32_t blits = 0;             ///< copy passes / surface loads done with a blit draw
    uint32_t cacheStores = 0;       ///< effect-cache / band stores
    uint32_t fxCacheHits = 0;       ///< T-658: effect segments the build drew from the cache
    uint32_t fxCacheMisses = 0;     ///< T-658: cacheable effect segments the build drew in full
    uint32_t fxCacheStores = 0;     ///< T-658: effect results the build stores (0s: frame reused)
    uint64_t uploadBytes = 0;       ///< geometry and uniform bytes written this frame
    float planMs = 0;               ///< submitMs split: targets, pass plan, uniform packing
    float uploadMs = 0;             ///< submitMs split: geometry and uniform writes
    float encodeMs = 0;             ///< submitMs split: pass / command encoding
    float queueMs = 0;              ///< submitMs split: command buffer finish + queue submit
    float effectsMs = 0;            ///< CPU time encoding effect passes (part of submitMs)
    float textMs = 0;               ///< CPU time encoding glyph draws (part of submitMs)

    // T-660b: the coverage atlas (gpu::FrameBuilder::lastCoverageStats); zeros for a reused frame,
    // except coverageAtlasBytes, which is always set.
    uint32_t coverageHits = 0;              ///< fills drawn from a cached coverage bitmap
    uint32_t coverageMisses = 0;            ///< fills whose coverage bitmap was not cached
    uint64_t coverageRasterizedPixels = 0;  ///< coverage pixels the build rasterized
    uint32_t coverageFallbacks = 0;         ///< eligible fills drawn with stencil-then-cover
    uint64_t coverageAtlasBytes = 0;        ///< the builder's coverage atlas pages (R8)
    uint32_t coverageTiles = 0;             ///< T-672: edge tiles of the tiled fills drawn
    uint32_t coverageUniformTiles = 0;      ///< T-672: empty and full tiles of the tiled fills drawn

    // T-663: how the build drew the clips that are stencil levels at 4x (gpu::FrameBuilder::lastClipStats);
    // zeros for a reused frame.
    uint32_t clipSkipped = 0;      ///< no clip needed (content inside the safe interior, or empty)
    uint32_t clipScissor = 0;      ///< drawn as a scissor
    uint32_t clipImageDirect = 0;  ///< one image drawn as a fringed convex polygon
    uint32_t clipMaskLayers = 0;   ///< drawn as a mask layer

    // T-674 (Docs/design/50-c-abi.md §50.ac item 1): the waits of the last `framesInFlight().beginFrame`
    // (0 when the GPU was at most two frames behind) and their milliseconds.
    uint32_t inflightWaits = 0;
    float inflightWaitMs = 0;
};

class WgpuRenderer {
public:
    WgpuRenderer();
    ~WgpuRenderer();
    WgpuRenderer(const WgpuRenderer&) = delete;
    WgpuRenderer& operator=(const WgpuRenderer&) = delete;
    WgpuRenderer(WgpuRenderer&&) noexcept;
    WgpuRenderer& operator=(WgpuRenderer&&) noexcept;

    /// Starts drawing with `init`'s device and queue (references taken here). Returns false when
    /// `init.device` is null (with `init.error` as the reason when there is one) or the arguments
    /// are invalid; `lastError()` explains. Safe to call again after a failure or a shutdown.
    bool init(const WgpuInit& init);
    /// Releases every GPU object and the device references. Safe to call twice.
    void shutdown();
    /// Pixel size of the target views `render` will be given.
    void resize(int width, int height);
    /// Clears the target to `(r, g, b, a)` (premultiplied) at the start of every `render`. Off by
    /// default: the target's content is kept, as `GlRenderer` keeps the bound FBO's.
    void setClearColor(float r, float g, float b, float a);
    void disableClear();

    /// Builds `list` and draws it into `target` (a view of a texture of the init's target format
    /// and of the `resize` size, single-sample). `shaper` is the DrawGlyphs outline fallback, as
    /// in `GlRenderer::render`; `ir`, `perf` and `damage` likewise (an unchanged frame is submitted
    /// again without a build). The work is submitted to the queue; nothing waits for the GPU.
    /// `perf` (when enabled) gets `renderMs` (the whole call), `effectsMs` and `textMs` (the CPU
    /// time encoding effect passes and glyph draws, T-438 item 14) and, from the builder, `gpuBuildMs`.
    void render(const CommandList& list, TextShaper* shaper, AssetResolver* assets, WGPUTextureView target,
                const IrView* ir = nullptr, PerfTracker* perf = nullptr, const DamageList* damage = nullptr);
    /// T-658 (50 §50.ab item 3 ii / iii): `render` in two halves, for a host that skips a frame with
    /// nothing to draw before it has a target. `build` makes the frame (or keeps the last one, as
    /// `render` does for an unchanged `damage`) and returns its draw command count; it touches no
    /// target and makes no attachment. `submit` draws the frame of the last `build` into `target`
    /// (nothing without a build since init / resize; `lastError` says so). Both add their time to
    /// `perf`'s `renderMs`, as `render` does with its whole duration.
    uint32_t build(const CommandList& list, TextShaper* shaper, AssetResolver* assets,
                   const IrView* ir = nullptr, PerfTracker* perf = nullptr,
                   const DamageList* damage = nullptr);
    void submit(WGPUTextureView target, PerfTracker* perf = nullptr);

#ifndef XT_GPU_NO_TEXT
    /// Injects the glyph atlas DrawGlyphs samples from, as `GlRenderer::setGlyphAtlas`. Not owned;
    /// null (the default) draws every glyph from its outline.
    void setGlyphAtlas(xt::text::GlyphAtlas* atlas);
#endif

    /// Drops every cached image texture (`gpu::FrameBuilder::invalidateAllAssets()`), as
    /// `GlRenderer::invalidateImages`.
    void invalidateImages();
    /// T-675 (Docs/design/50-c-abi.md §50.ac item 2): what a hidden surface gives back while its
    /// renderer stays (`gpu::FrameBuilder::trim`: the effect cache and band targets to the device's
    /// pool, the image textures' uses ended); the device, its buffers, bind groups, gradient tables and
    /// atlas pages stay. The next `build` is a full one.
    void trim();
    WgpuStats stats() const;
    /// T-662 (Docs/design/10-backends.md §10.11 item 2, supplement 2): `gpu::FrameBuilder::
    /// wantsAnotherFrame()` of the frame last built -- a coverage shape the frame budget held back
    /// is drawn hard edged; a host that skips unchanged frames renders once more while this is true.
    bool wantsAnotherFrame() const;
    /// The adapter's device name and backend, e.g. "NVIDIA GeForce RTX 4070 Ti (D3D12)", read from
    /// `WgpuInit::adapter` ("unknown adapter" when the host gave none); empty before init.
    const char* adapterName() const;

    /// Copies the whole of `texture` (single-sample, COPY_SRC, a format `bytesPerTexel` knows) into
    /// `out`, tightly packed, row 0 = top: `copyTextureToBuffer` with 256-byte row alignment,
    /// `mapAsync`, and a blocking `wgpuDevicePoll`. BGRA8 is returned as RGBA8. `out.size()` must
    /// be width x height x bytes per texel. Returns false (and `lastError()`) otherwise or on a
    /// map failure.
    bool readback(WGPUTexture texture, std::span<uint8_t> out);

    /// Empty string when the last init/render/readback had no error.
    const char* lastError() const;
    /// T-203: the one-time warning of the last render (a custom effect shader that failed to
    /// compile; its fallback is drawn instead), as `GlRenderer::lastWarning`.
    const char* lastWarning() const;
    /// The device (textures, transient introspection) for tests and tools; null before init.
    WgpuDevice* device();
    /// T-674 (50 §50.ac item 1): the frame limit of a host's own frame loop; every `submit` / `render`
    /// notes its submission index here. Its frames begin and end only when the host calls
    /// `beginFrame` / `endFrame` (Code/Backends/xt_renderer_wgpu.cpp); `shutdown` resets it.
    WgpuFramesInFlight& framesInFlight();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace xt::wgpu
