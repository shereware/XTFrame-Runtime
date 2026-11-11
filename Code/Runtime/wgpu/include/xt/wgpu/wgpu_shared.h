#pragma once

#include <webgpu/webgpu.h>

#include <cstdint>
#include <memory>

/// WgpuSharedCache (T-648, Docs/design/50-c-abi.md §50.z addendum): what the `WgpuDevice`s drawing on
/// one `WGPUDevice` share -- the C ABI's GPU target owns one and every renderer of its attached
/// surfaces (and its composer) borrows it through `WgpuInit::shared`:
///
///   - **Pipelines**: the shader modules, bind group layouts, samplers and render pipelines
///     (wgpu_internal.h `Pipelines`) are created once, by the first device that borrows the cache;
///     a later device on the same `WGPUDevice` reuses them, so only the first surface pays the
///     shader compiles. A device on another `WGPUDevice` keeps its own (and shares no image).
///   - **Image textures**: `FrameBuilder` asks its device for the texture of an image's content
///     (`gpu::GpuDevice::findSharedImage`); the cache keeps one immutable texture per (content key,
///     size, format), which each device registers in its own handle table. A use ends with
///     `releaseSharedImage`: a builder shutting down (detach, trim, destroy of its surface) leaves
///     the texture in the cache even when nothing uses it, an eviction or invalidation destroys it
///     once no device uses it; `shutdown` destroys the rest.
///   - **Idle budget** (T-656, 50 §50.ab item 1): a texture left with no use goes to the idle list,
///     in the order its last use ended; while the idle textures together exceed the budget (default
///     kDefaultImageIdleBudget, 0 = destroyed as soon as unused) the oldest is destroyed. A texture
///     found again while idle leaves the list (no upload). `trimIdleImages` destroys every idle one.
///
///   - **Target-level pools** (T-675, 50 §50.ac item 2): the devices also borrow the cache's
///     transient pool (T-678, 50 §50.ac addendum item 1: its byte budget is the most bytes one
///     submit of the last target frame borrowed plus 16 MiB, set and applied by `endFrame`; least
///     recently used first, never an entry used in that frame; T-685: nor one whose last borrower is a
///     named user that has not let go of it -- WgpuDevice::setPoolUser / releasePoolUse), its released
///     effect-cache / band targets (16 MB, oldest first), its surface attachment sets (a 4x colour attachment
///     and its stencil, or the stencil alone, one set per size, sample count and format, borrowed for the
///     length of one submit) and its 1x1 dummy. The host returns surface textures it no longer shows to a
///     pool of (size, format)
///     (`releaseSurfaceTexture`) and takes them back (`acquireSurfaceTexture`). The pools age by the
///     host's frames (`endFrame`): a transient or a released cached target unused for 1800 frames
///     goes (at most one of each per frame), an idle surface texture or an unborrowed attachment set
///     after 300; `trimPools` destroys every pooled texture no device borrows.
///
/// Glyph atlas pages, coverage atlas pages, gradient tables and the effect caches' entries stay per
/// device. Not thread-safe: the devices that borrow a cache are used from one thread, as the cache
/// itself. Every device that borrows it must be shut down before `shutdown` / destruction.

namespace xt::wgpu {

/// T-683 (Docs/design/50-c-abi.md §50.ad item 2): the shader compiles and render pipeline creates of a
/// device's pipeline set (its own, or the WgpuSharedCache's it borrows), counted apart while
/// `WgpuDevice::prewarmPipelines` runs ("prewarm", summed over every prewarm) and after it (since the
/// last prewarm ended; since the set was made when none ran). A shader compile is the first pipeline
/// of a program and vertex stage (wgpu keeps the compiled shader on its device; a later blend /
/// stencil / sample count of it only makes the pipeline) and every T-456 probe. Not a stable API.
struct WgpuPipelineCounters {
    uint32_t prewarmShaderCompiles = 0;
    uint32_t prewarmPipelineCreates = 0;
    uint32_t shaderCompiles = 0;
    uint32_t pipelineCreates = 0;
};

/// T-683 (Docs/design/50-c-abi.md §50.ad item 2): what one `WgpuDevice::prewarmPipelines` did.
struct WgpuPipelinePrewarm {
    uint32_t shaders = 0;    ///< shader compiles it made (0 when every program was compiled before)
    uint32_t pipelines = 0;  ///< render pipelines it created
    double ms = 0;           ///< its wall time
    bool ok = true;          ///< false: a pipeline failed (lastError names it after the next submit)
};

/// Counters of a `WgpuSharedCache` (for tests and tools; not a stable API).
struct WgpuSharedStats {
    uint32_t imageTextures = 0;      ///< shared image textures alive (used or idle)
    uint32_t imagesInUse = 0;        ///< of which at least one device uses
    uint64_t imageBytes = 0;         ///< their memory (texels x bytes, or the blocks)
    uint32_t imageCreates = 0;       ///< textures created and uploaded since construction
    uint32_t imageReuses = 0;        ///< uses served from an existing texture (no upload)
    uint32_t imageFrees = 0;         ///< textures destroyed before `shutdown` (evictions included)
    uint32_t pipelineSets = 0;       ///< times the pipeline set (shader modules, layouts) was created
    uint32_t pipelineBorrows = 0;    ///< device inits that reused an existing pipeline set
    uint32_t devices = 0;            ///< devices borrowing the cache now
    uint32_t imageIdleTextures = 0;  ///< T-656: of `imageTextures`, those no device uses (idle list)
    uint64_t imageIdleBytes = 0;     ///< T-656: their memory
    uint64_t imageEvictions = 0;     ///< T-656: idle textures destroyed (budget or trimIdleImages)
    /// T-675: every texture created on the cache's device since construction by the devices that
    /// borrow it (images, gradient tables, atlas pages, transients, cached targets) and by the pools
    /// (attachment sets, surface textures, the dummy).
    uint32_t textureCreates = 0;
    uint32_t poolTextures = 0;  ///< T-675: textures in the pools now (idle or borrowed)
    /// T-675: their memory (texels x bytes x samples; T-678: a Stencil8 at the backend's bytes per
    /// texel, `stencil8TexelBytes`). Content bytes: no allocator alignment or block slack.
    uint64_t poolBytes = 0;
    uint32_t attachmentSets = 0;       ///< T-675: surface attachment sets alive
    uint64_t attachmentBytes = 0;      ///< T-675: their memory
    uint32_t surfaceTexturesIdle = 0;  ///< T-675: idle surface textures in the pool
    uint32_t poolFrees = 0;            ///< T-675: pooled textures destroyed (age, budget, trimPools)
    /// T-685 (50 §50.ac addendum 2): of `poolBytes`, the transients the budget may not evict (their last
    /// borrower is a named user that still holds them: WgpuDevice::setPoolUser); transients the budget
    /// evicted and transients the 1800-frame ageing destroyed since construction (entries, not textures).
    uint64_t poolProtectedBytes = 0;
    uint32_t poolBudgetEvictions = 0;
    uint32_t poolAgeEvictions = 0;
    /// T-683 (50 §50.ad item 2): the pipeline set's counters (wgpu_device.h WgpuPipelineCounters): shader
    /// compiles and pipeline creates while WgpuDevice::prewarmPipelines ran, and since it ended.
    uint32_t prewarmShaderCompiles = 0;
    uint32_t prewarmPipelineCreates = 0;
    uint32_t shaderCompiles = 0;
    uint32_t pipelineCreates = 0;
};

class WgpuSharedCache {
public:
    WgpuSharedCache();
    ~WgpuSharedCache();
    WgpuSharedCache(const WgpuSharedCache&) = delete;
    WgpuSharedCache& operator=(const WgpuSharedCache&) = delete;

    /// Destroys every shared image texture and the pipeline set and releases the `WGPUDevice`
    /// reference. The devices that borrowed the cache must be shut down first. Safe to call twice;
    /// the cache may be borrowed again afterwards.
    void shutdown();
    WgpuSharedStats stats() const;

    /// T-656: the idle budget a new cache starts with (64 MiB).
    static constexpr uint64_t kDefaultImageIdleBudget = uint64_t(64) << 20;
    /// T-656: sets the idle budget in bytes (0 = an unused texture is destroyed at once) and applies
    /// it now (the oldest idle textures are destroyed until the rest fit).
    void setImageIdleBudget(uint64_t bytes);
    uint64_t imageIdleBudget() const;
    /// T-656: destroys every idle texture; returns how many.
    uint32_t trimIdleImages();

    /// T-675: a single-sample `width` x `height` texture of `format` (RENDER_ATTACHMENT |
    /// TEXTURE_BINDING | COPY_SRC) and its view: an idle one of the pool, else a new one; null before a
    /// device borrowed the cache, or when it cannot be made. The caller owns both until it gives them
    /// back with `releaseSurfaceTexture` (or destroys them itself).
    WGPUTexture acquireSurfaceTexture(int width, int height, WGPUTextureFormat format, WGPUTextureView& view);
    /// T-675: puts a texture of `acquireSurfaceTexture` (and its view) back into the pool, idle from
    /// this frame; nothing may use it until it is acquired again.
    void releaseSurfaceTexture(WGPUTexture texture, WGPUTextureView view);
    /// T-675: one host frame ended; ages the pools (see the file comment). T-678: first sets the
    /// transient budget from the frame that ended and applies it.
    void endFrame();
    /// T-675: destroys every pooled texture no device borrows now (transients, released cached
    /// targets, attachment sets, idle surface textures); returns how many textures.
    uint32_t trimPools();

private:
    friend class WgpuDevice;
    struct Impl;  ///< wgpu_internal.h
    std::unique_ptr<Impl> m_impl;
};

}  // namespace xt::wgpu
