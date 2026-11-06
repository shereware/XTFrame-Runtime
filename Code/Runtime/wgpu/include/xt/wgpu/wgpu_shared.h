#pragma once

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
///
/// The transient pool, glyph atlas pages, effect caches and band targets stay per device. Not
/// thread-safe: the devices that borrow a cache are used from one thread, as the cache itself. Every
/// device that borrows it must be shut down before `shutdown` / destruction.

namespace xt::wgpu {

/// Counters of a `WgpuSharedCache` (for tests and tools; not a stable API).
struct WgpuSharedStats {
    uint32_t imageTextures = 0;    ///< shared image textures alive (used or kept)
    uint32_t imagesInUse = 0;      ///< of which at least one device uses
    uint64_t imageBytes = 0;       ///< their memory (texels x bytes, or the blocks)
    uint32_t imageCreates = 0;     ///< textures created and uploaded since construction
    uint32_t imageReuses = 0;      ///< uses served from an existing texture (no upload)
    uint32_t imageFrees = 0;       ///< textures destroyed before `shutdown`
    uint32_t pipelineSets = 0;     ///< times the pipeline set (shader modules, layouts) was created
    uint32_t pipelineBorrows = 0;  ///< device inits that reused an existing pipeline set
    uint32_t devices = 0;          ///< devices borrowing the cache now
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

private:
    friend class WgpuDevice;
    struct Impl;  ///< wgpu_internal.h
    std::unique_ptr<Impl> m_impl;
};

}  // namespace xt::wgpu
