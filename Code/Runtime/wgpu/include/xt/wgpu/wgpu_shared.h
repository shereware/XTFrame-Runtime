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
///   - **Idle budget** (T-656, 50 §50.ab item 1): a texture left with no use goes to the idle list,
///     in the order its last use ended; while the idle textures together exceed the budget (default
///     kDefaultImageIdleBudget, 0 = destroyed as soon as unused) the oldest is destroyed. A texture
///     found again while idle leaves the list (no upload). `trimIdleImages` destroys every idle one.
///
/// The transient pool, glyph atlas pages, effect caches and band targets stay per device. Not
/// thread-safe: the devices that borrow a cache are used from one thread, as the cache itself. Every
/// device that borrows it must be shut down before `shutdown` / destruction.

namespace xt::wgpu {

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

private:
    friend class WgpuDevice;
    struct Impl;  ///< wgpu_internal.h
    std::unique_ptr<Impl> m_impl;
};

}  // namespace xt::wgpu
