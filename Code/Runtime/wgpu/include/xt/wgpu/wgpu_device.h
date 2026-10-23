#pragma once

#include <webgpu/webgpu.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "xt/gpu/gpu_device.h"
#include "xt/gpu/gpu_frame.h"

/// WgpuDevice: the WebGPU implementation of `xt::gpu::GpuDevice` (Docs/design/10-backends.md
/// §10.10, T-437 / T-438) over wgpu-native's `webgpu.h`.
///
/// Textures: every `createTexture` is a 2D `WGPUTexture` (TEXTURE_BINDING | COPY_DST | COPY_SRC)
/// with a default view; the handle indexes an internal table (0 = none). Uploads go through
/// `wgpuQueueWriteTexture`, row 0 = top, premultiplied as the builder gives them (§10.5).
/// Block-compressed formats are created only when the device has `TextureCompressionBC` and the
/// size is a multiple of 4 (otherwise 0, and the builder falls back to Rgba8). A host texture
/// (T-150, `engine://`) enters the same table through `registerExternalTexture`; effect result
/// caches and Direct-mode band targets (`acquireCachedTarget`) are table entries too.
///
/// Targets (§10.10.2 Pass row): target 0 is the view `setSurface` names (`resize` gives its size);
/// with a sample count of 4 the surface is drawn into a 4x attachment of the target format that
/// resolves into that view, and has a Stencil8 attachment of the same sample count. Transient `i`
/// is a pooled `Rgba8Unorm` / `R32Float` texture (RENDER_ATTACHMENT | TEXTURE_BINDING | COPY_SRC |
/// COPY_DST) of its descriptor's size. As in gl_device.cpp (`acquireDrawTarget`), a transient that
/// needs stencil is drawn through a 4x attachment resolving into it when the sample count is 4 and
/// gets a Stencil8 attachment; every other transient (effect scratch, R32Float) is single-sample.
/// The pool keeps entries by exact (size, format, samples, stencil) within the T-163 byte budget.
///
/// `submit` (T-438): the frame's vertex/index/instance buffers are uploaded incrementally into one
/// of three rotating sets; the per-pass constants and every command's parameter block go into one
/// uniform buffer addressed with dynamic offsets; each `Pass` is one render pass whose commands are
/// drawn with the cached pipeline of (shading/program, blend, stencil op, draw kind, target format,
/// samples, stencil attachment) -- the WGSL of src/shaders mirrors gl_shaders.h shader by shader.
/// A `Pass::copy` is a `copyTextureToTexture` when both sides are single-sample textures of one
/// format, otherwise a blit draw (a multisampled destination, the surface as source or
/// destination). Cache stores copy the transient into the persistent target after their pass.
///
/// Surface requirements: the view `setSurface` names must belong to a texture with
/// RENDER_ATTACHMENT usage; it also needs TEXTURE_BINDING when the frame reads target 0 back (Direct
/// mode, `supportsDirectTarget`) or when a 4x surface is drawn without a clear colour -- the first
/// surface pass then blits the host's content into the 4x attachment so it is kept, as GL keeps the
/// bound FBO's content (T-438 ruling on T-437 deviation 4).
/// The public interface never throws.

namespace xt::wgpu {

/// What the last `submit` did (for tests and tools; not a stable API).
struct WgpuDeviceStats {
    uint32_t passes = 0;            ///< render passes encoded
    uint32_t clears = 0;            ///< of which cleared their color attachment
    uint32_t drawCalls = 0;         ///< draw / drawIndexed calls (blits included)
    uint32_t pipelineBinds = 0;     ///< setPipeline calls
    uint32_t bindGroups = 0;        ///< texture bind groups created this submit
    uint32_t copies = 0;            ///< copy passes done with copyTextureToTexture
    uint32_t blits = 0;             ///< copy passes / surface loads done with a blit draw
    uint32_t cacheStores = 0;       ///< cache stores executed
    uint32_t uploadRanges = 0;      ///< writeBuffer calls for geometry and uniforms
    uint64_t uploadBytes = 0;       ///< bytes written by them
    uint32_t skippedCmds = 0;       ///< draw commands not executed (no pipeline, bad parameters)
    uint32_t skippedCopies = 0;     ///< copy passes and cache stores not executed
    uint32_t transientCreates = 0;  ///< pooled transient targets created (not reused)
    uint32_t transientDeletes = 0;  ///< pooled transient targets destroyed (budget / idle)
    uint32_t poolEntries = 0;       ///< pooled transient targets alive after the submit
    uint64_t poolBytes = 0;         ///< their memory
    uint32_t cachedTargets = 0;     ///< persistent cache / band targets alive (free list included)
    float planMs = 0;               ///< targets, pass plan and uniform packing
    float uploadMs = 0;             ///< geometry and uniform writeBuffer calls
    float encodeMs = 0;             ///< render pass / command encoding
    float queueMs = 0;              ///< command buffer finish and queue submit
    float effectsMs = 0;            ///< CPU time encoding Shading::Effect commands
    float textMs = 0;               ///< CPU time encoding Shading::Glyph commands
};

class WgpuDevice : public gpu::GpuDevice {
public:
    WgpuDevice();
    ~WgpuDevice() override;
    WgpuDevice(const WgpuDevice&) = delete;
    WgpuDevice& operator=(const WgpuDevice&) = delete;

    /// Takes a reference to `device` and `queue` (released by shutdown). `targetFormat` is the
    /// format of the views `setSurface` will name; `sampleCount` is 1 or 4. Compiles every shader
    /// and creates the bind group layouts, then builds one minimal probe pipeline (T-456). Returns
    /// false (and `err`) on bad arguments, a shader that does not compile or a failed probe (the
    /// backend's shader compiler is unusable) -- never aborts.
    bool init(WGPUDevice device, WGPUQueue queue, WGPUTextureFormat targetFormat, uint32_t sampleCount,
              std::string& err);
    /// Destroys every texture, pooled target, pipeline and buffer and releases the device
    /// references. Safe to call twice or before init.
    void shutdown();
    bool initialized() const { return m_device != nullptr; }

    /// Size of the surface target in pixels (the views passed to `setSurface`).
    void resize(int width, int height);
    /// The view `submit` draws target 0 into. Not owned; must stay valid through `submit`.
    void setSurface(WGPUTextureView view) { m_surface = view; }
    /// When `enabled`, `submit` clears target 0 to `color` (premultiplied) at the start of the
    /// frame; otherwise the surface's content is kept (see the file comment for a 4x surface).
    void setSurfaceClear(bool enabled, const WGPUColor& color);

    gpu::TextureHandle createTexture(const char* debugName, uint16_t w, uint16_t h, gpu::TextureFormat fmt,
                                     const void* initial) override;
    void updateTexture(gpu::TextureHandle tex, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                       const void* data, size_t rowPitch) override;
    void destroyTexture(gpu::TextureHandle tex) override;
    void submit(const gpu::GpuFrame& frame) override;
    gpu::TextureHandle acquireCachedTarget(uint16_t w, uint16_t h, gpu::TextureFormat fmt) override;
    void releaseCachedTarget(gpu::TextureHandle target) override;
    uint32_t prepareCustomEffect(std::string_view glsl, const char* names, const uint8_t* types,
                                 uint32_t count) override;
    /// `limits.maxTextureDimension2D` as init read it.
    uint32_t maxTextureSize() const override { return m_maxTextureSize; }
    /// T-438: target 0 can be read back (a blit from the surface view, which then needs
    /// TEXTURE_BINDING), so Direct mode works (§10.10.2 Direct row).
    bool supportsDirectTarget() const override { return true; }

    /// T-150: makes a host texture view sampleable as a `TextureHandle` (for
    /// `AssetResolver::setExternalTexture`). Not owned: the host keeps it alive until
    /// `unregisterExternalTexture`. Returns 0 for a null view.
    gpu::TextureHandle registerExternalTexture(WGPUTextureView view, uint16_t width, uint16_t height);
    void unregisterExternalTexture(gpu::TextureHandle tex);

    /// The texture behind a handle of `createTexture` / `acquireCachedTarget` (null for 0, a
    /// destroyed one or an external view).
    WGPUTexture texture(gpu::TextureHandle tex) const;
    /// The sampleable (single-sample) texture of transient `target` (1-based) as the last submit
    /// left it; null when out of range. For tests and tools.
    WGPUTexture transientTexture(gpu::TargetHandle target) const;
    /// The sample count transient `target` was drawn with in the last submit (0 when out of range).
    uint32_t transientSampleCount(gpu::TargetHandle target) const;

    WGPUDevice device() const { return m_device; }
    WGPUQueue queue() const { return m_queue; }
    uint32_t sampleCount() const { return m_sampleCount; }
    /// T-456 test hook: the WGSL (entry points `vs_probe` / `fs_probe`) `init`'s probe compiles
    /// instead of its own; null restores it. Process-wide; tests only.
    static void setProbeShaderForTesting(const char* wgsl);

    /// Empty when the last submit had nothing to report. T-456: a pipeline (or its shader module)
    /// that fails to be created is named here once, by the submit that first needed it; its draws
    /// are skipped in that and every later submit while the rest of the frame is drawn.
    const std::string& lastError() const { return m_lastError; }
    const WgpuDeviceStats& lastStats() const { return m_stats; }
    /// T-203: the one-time warnings raised since the last call (a custom effect shader that failed
    /// to compile), then cleared.
    std::string takeWarning() {
        std::string w;
        w.swap(m_warning);
        return w;
    }

private:
    struct Texture {
        WGPUTexture texture = nullptr;  ///< null for an external view
        WGPUTextureView view = nullptr;
        gpu::TextureFormat format = gpu::TextureFormat::Rgba8;
        uint16_t width = 0, height = 0;
        bool external = false;  ///< view not owned (registerExternalTexture)
        bool cached = false;    ///< an acquireCachedTarget target
    };
    /// One pooled offscreen target: `color` is drawn into (4x when `samples` is 4) and resolves
    /// into `resolve`; with one sample `resolve` is null and `color` is the sampleable texture.
    struct Pooled {
        uint16_t width = 0, height = 0;
        gpu::TextureFormat format = gpu::TextureFormat::Rgba8;
        uint32_t samples = 1;
        bool stencil = false;
        WGPUTexture color = nullptr;
        WGPUTextureView colorView = nullptr;
        WGPUTexture resolve = nullptr;
        WGPUTextureView resolveView = nullptr;
        WGPUTexture depthStencil = nullptr;
        WGPUTextureView depthStencilView = nullptr;
        bool inUse = false;
        uint32_t lastUsed = 0;
        uint64_t bytes = 0;
    };
    /// A pass window in surface pixels (gpu_frame.h `Pass::view*`).
    struct Window {
        int x = 0, y = 0, w = 0, h = 0;
    };
    /// What `submit` worked out for one pass before encoding (T-438).
    struct PassPlan {
        Window win;
        int originX = 0, originY = 0;
        int targetW = 0, targetH = 0;
        float scaleX = 1.0f, scaleY = 1.0f;
        uint32_t constOffset = 0;  ///< group 0 dynamic offset
        uint8_t copyMode = 0;      ///< 0 none, 1 copyTextureToTexture, 2 blit
        int dstX = 0, dstY = 0, copyW = 0, copyH = 0, srcX = 0, srcY = 0;
        uint32_t blitOffset = 0;
    };
    struct Gpu;  ///< pipelines, buffers, bind groups (wgpu_internal.h)
    /// A command whose parameter block is missing (it is not drawn).
    static constexpr uint32_t kNoParams = 0xFFFFFFFFu;

    WGPUDevice m_device = nullptr;
    WGPUQueue m_queue = nullptr;
    WGPUTextureFormat m_targetFormat = WGPUTextureFormat_RGBA8Unorm;
    uint32_t m_sampleCount = 1;
    uint32_t m_maxTextureSize = 0;
    bool m_hasBc = false;
    int m_width = 0, m_height = 0;
    WGPUTextureView m_surface = nullptr;
    bool m_clearSurface = false;
    WGPUColor m_clearColor = {0, 0, 0, 0};
    std::unique_ptr<Gpu> m_gpu;

    std::vector<Texture> m_textures;  ///< handle h = m_textures[h - 1]
    std::vector<uint32_t> m_freeTextures;
    std::vector<Pooled> m_pool;
    uint64_t m_poolBudget = 0;
    std::vector<int> m_frameTargets;  ///< pool index of each transient of the last submit (-1: none)
    uint32_t m_frameCounter = 0;
    /// Released cached targets kept for reuse by exact size (T-163), with their release frame.
    struct FreeCached {
        Texture texture;
        uint32_t releasedFrame = 0;
    };
    std::vector<FreeCached> m_cachedFree;
    uint64_t m_cachedFreeBytes = 0;

    /// The surface's 4x color attachment and its stencil (sized by resize, created lazily).
    WGPUTexture m_surfaceMsaa = nullptr;
    WGPUTextureView m_surfaceMsaaView = nullptr;
    WGPUTexture m_surfaceStencil = nullptr;
    WGPUTextureView m_surfaceStencilView = nullptr;
    int m_surfaceAttachW = 0, m_surfaceAttachH = 0;

    // -- Per-submit state --
    const gpu::GpuFrame* m_frame = nullptr;
    std::vector<Window> m_windows;  ///< each transient's window (its last pass's)
    std::vector<PassPlan> m_plans;
    std::vector<uint32_t> m_cmdParamOffsets;  ///< group 1 dynamic offset per command
    std::vector<float> m_uniformData;

    std::string m_lastError;
    std::string m_warning;
    WgpuDeviceStats m_stats;

    const Texture* find(gpu::TextureHandle tex) const;
    gpu::TextureHandle addTexture(const Texture& t);
    bool ensureSurfaceAttachments();
    void releaseSurfaceAttachments();
    int acquirePooled(const gpu::TransientTargetDesc& desc);
    static void releasePooled(Pooled& p);
    void trimPool();
    void trimCachedFree();
    static void releaseTextureEntry(Texture& t);

    Window windowOf(gpu::TargetHandle target) const;
    /// The sampleable view of target `t` (a transient's resolve or color view; the surface view).
    WGPUTextureView sampleView(gpu::TargetHandle t) const;
    /// Plans every pass (windows, copies) and fills the uniform buffer.
    void planFrame(const gpu::GpuFrame& frame);
    /// Encodes pass `index` (its copy, its commands and its cache stores).
    void encodePass(WGPUCommandEncoder enc, size_t index, bool& surfaceStarted);
    /// Draws one command into the open pass (wgpu_draw.cpp).
    void drawCmd(WGPURenderPassEncoder pass, const gpu::DrawCmd& cmd, uint32_t cmdIndex, const PassPlan& plan,
                 gpu::TargetHandle target);
    /// One blit draw of `src` into the open pass over the target rectangle (x, y, w, h).
    void blit(WGPURenderPassEncoder pass, WGPUTextureView src, uint32_t uniformOffset, int x, int y, int w,
              int h, WGPUTextureFormat format, uint32_t samples, bool hasStencil, uint32_t targetW,
              uint32_t targetH);
    void executeCacheStore(WGPUCommandEncoder enc, const gpu::CacheStore& store);
};

/// Bytes per texel of an uncompressed WebGPU color format `readback` handles (RGBA8/BGRA8 and their
/// sRGB forms, R8Unorm, R32Float); 0 for any other.
uint32_t bytesPerTexel(WGPUTextureFormat format);

}  // namespace xt::wgpu
