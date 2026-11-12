#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "xt/gpu/gpu_frame.h"

/// The host-implemented device behind `FrameBuilder` (Docs/design/10-backends.md §10.5). A device
/// owns textures and executes frames; it never sees a CommandList. xt_gl (from T-147) and the
/// CryEngine plugin (CE_XTUI/docs/05-stage2-design.md §5.2) are the two implementations.
///
/// Call order per frame: `FrameBuilder::build` calls `createTexture`/`updateTexture`/
/// `destroyTexture` as it goes (uploads happen *before* the frame that uses them is submitted);
/// the host then calls `submit` whenever it wants the frame drawn. A frame references only
/// textures that exist at `submit` time.

namespace xt::gpu {

/// T-648: what an image texture holds, as `FrameBuilder` hashes it (the decoded straight RGBA8 pixels,
/// or a compressed image's blocks) -- two 64-bit lanes of one content hash. Together with the size
/// and format it names the texture in `GpuDevice::findSharedImage`.
struct ImageKey {
    uint64_t lo = 0;
    uint64_t hi = 0;

    bool operator==(const ImageKey&) const = default;
};

class GpuDevice {
public:
    virtual ~GpuDevice() = default;

    /// Creates a `w x h` texture of `fmt`. `initial` (may be null) is tightly packed, row 0 =
    /// top. `debugName` is a static string for tools. Returns 0 on failure; a non-zero handle
    /// stays valid until `destroyTexture`.
    virtual TextureHandle createTexture(const char* debugName, uint16_t w, uint16_t h, TextureFormat fmt,
                                        const void* initial) = 0;

    /// Replaces the `w x h` region at `(x, y)` of `tex` with `data` (`rowPitch` bytes between
    /// rows; `data` points at the region's own first texel, not the whole image).
    virtual void updateTexture(TextureHandle tex, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                               const void* data, size_t rowPitch) = 0;

    /// Releases a texture created by `createTexture`. Ignores 0.
    virtual void destroyTexture(TextureHandle tex) = 0;

    /// Draws `frame` into the currently bound output surface: allocates (or reuses, by
    /// `TransientTargetDesc`) one target per `frame.transients` entry, then runs
    /// `frame.passes` in order, executing every `frame.cacheStores` entry right after its pass
    /// (T-162). The frame stays valid only for the duration of the call.
    virtual void submit(const GpuFrame& frame) = 0;

    // -- Effect result cache (T-162, Docs/tasks/T-162.md). Appended with default
    //    implementations: a device that does not cache changes nothing. --

    /// Creates a persistent `w x h` color target of `fmt` (always `Rgba8` today) whose content
    /// survives across `submit` calls: a `CacheStore` fills it from a transient, and any later
    /// frame samples it like an uploaded texture (`Shading::Texture`, `DrawCmd::texture`, row 0
    /// = top). The returned handle lives in the same namespace as `createTexture`'s and stays
    /// valid until `releaseCachedTarget`. Returns 0 when the device does not cache (the default)
    /// or cannot allocate; the builder then draws the effect chain as usual and never emits a
    /// store for it. The builder keeps the total under its own budget
    /// (`FrameBuilder::kEffectCacheBudgetBytes`) and releases every target on resize/shutdown.
    /// `FrameBuilder::init` acquires and releases one 1 x 1 target to learn whether the device
    /// caches at all.
    virtual TextureHandle acquireCachedTarget(uint16_t w, uint16_t h, TextureFormat fmt) {
        (void)w;
        (void)h;
        (void)fmt;
        return 0;
    }

    /// Releases a target from `acquireCachedTarget`. Ignores 0. Never called from within
    /// `submit`.
    virtual void releaseCachedTarget(TextureHandle target) { (void)target; }

    // -- Custom effects (T-203, Docs/design/18-effects.md §18.7). Appended with a default
    //    implementation: a device without them applies every custom effect's fallback. --

    /// Makes the `.xtfx` GLSL module `glsl` (xt/compile/xtfx.h: parameter uniforms, `XtInput`,
    /// `xtSample`, `xtMain`; no `#version`/`main`) runnable as `EffectProgram::Custom`, once per
    /// distinct module (the device caches by content). `names` holds the `count` parameter
    /// uniform names NUL-separated in declaration order, `types` their ShaderParamDesc types.
    /// Returns a non-zero program id, or 0 when the device cannot run it (the default; a failed
    /// compile, which the device reports once): the builder then schedules the effect's
    /// `fallback` instead. Called during `FrameBuilder::build`, never from within `submit`.
    virtual uint32_t prepareCustomEffect(std::string_view glsl, const char* names, const uint8_t* types,
                                         uint32_t count) {
        (void)glsl;
        (void)names;
        (void)types;
        (void)count;
        return 0;
    }

    // -- Device limits (Docs/tasks/T-263d.md). Appended with a default implementation: a device
    //    that does not say keeps behaving as before. --

    /// The largest texture width or height the device can create, in texels; 0 = unknown (the
    /// default). `FrameBuilder::build` asks once per build. An image wider or taller than a
    /// non-zero limit is never handed to `createTexture`: it is left out of the frame, reported
    /// once in `FrameBuilder::lastError` and not tried again until it is invalidated. A
    /// `createTexture` that returns 0 for an image within the limit (or when the limit is
    /// unknown) is taken as a failure that may pass: the builder tries again 60 frames later, at
    /// most three times (T-263f: host frames, counted by `FrameBuilder::build` and
    /// `FrameBuilder::lastFrameReusable`, see the latter).
    virtual uint32_t maxTextureSize() const { return 0; }

    // -- Direct target mode (T-343, Docs/design/23-host-integration.md §23.aa.4). Appended with
    //    default implementations: a device that says nothing keeps drawing in Layer mode. --

    /// True when target 0 (the surface the host bound) may be read back by a `Pass::copy`
    /// (`copySource == kSurfaceTarget`): the host's scene is then usable as the backdrop of blend
    /// modes and backdrop blurs. `HostTargetMode::Direct` needs it; a builder asked for Direct on
    /// a device that returns false (the default) draws in Layer mode and reports it once in
    /// `FrameBuilder::lastError`.
    virtual bool supportsDirectTarget() const { return false; }

    /// A persistent `w x h` Rgba8 color target for one band of the paint order (§23.aa.2): filled
    /// by a `CacheStore` from the band's transient on the frames the band is redrawn, and sampled
    /// like an uploaded texture (`Shading::Texture`) on every frame in between. Same contract as
    /// `acquireCachedTarget` (which the default forwards to): 0 = not available, and the builder
    /// then draws the whole UI directly every frame (§23.aa.2's fallback). Never called from
    /// within `submit`.
    virtual TextureHandle acquireBandTarget(uint16_t w, uint16_t h) {
        return acquireCachedTarget(w, h, TextureFormat::Rgba8);
    }

    /// Releases a target from `acquireBandTarget`. Ignores 0. Never called from within `submit`.
    virtual void releaseBandTarget(TextureHandle target) { releaseCachedTarget(target); }

    // -- Shared image textures (T-648, Docs/design/50-c-abi.md §50.z addendum). Appended with
    //    default implementations: a device that does not share keeps one texture per builder. --

    /// True when image textures are shared with the other devices of one owner (the C ABI's GPU
    /// target): the builder then asks `findSharedImage` before it prepares an upload, and creates
    /// and releases its images with the two functions below instead of `createTexture` /
    /// `destroyTexture`. False (the default): those functions are never called.
    virtual bool sharesImageTextures() const { return false; }

    /// A new handle on this device to the shared texture of `key` (`w` x `h`, `fmt`), when one
    /// exists; 0 otherwise. Each non-zero handle is one use, ended by `releaseSharedImage`.
    virtual TextureHandle findSharedImage(const ImageKey& key, uint16_t w, uint16_t h, TextureFormat fmt) {
        (void)key;
        (void)w;
        (void)h;
        (void)fmt;
        return 0;
    }

    /// Like `createTexture`, and the texture becomes the shared one of `key` (one use). An image
    /// texture is never updated after it is created.
    virtual TextureHandle createSharedImage(const ImageKey& key, const char* debugName, uint16_t w,
                                            uint16_t h, TextureFormat fmt, const void* initial) {
        (void)key;
        return createTexture(debugName, w, h, fmt, initial);
    }

    /// Ends one use of a texture from `findSharedImage` / `createSharedImage` (the handle is gone).
    /// `keep`: the builder is shutting down (its surface was detached, trimmed or destroyed), and
    /// the texture stays with the owner even when nothing uses it (T-656: on the owner's idle list,
    /// within its budget); otherwise (an eviction or an invalidation) it is destroyed once no device
    /// uses it. Ignores 0.
    virtual void releaseSharedImage(TextureHandle tex, bool keep) {
        (void)keep;
        destroyTexture(tex);
    }

    /// T-665 (Docs/design/10-backends.md §10.11 item 7, Docs/tasks/T-665.md 規格 3): gives image
    /// texture `tex` (from `createTexture` / `createSharedImage` / `findSharedImage`) its full mip
    /// chain the first time the builder draws it minified, and returns the number of levels it has
    /// afterwards (1 = it stays single-level: the device has no mip support, the texture is not an
    /// image, or a block-compressed image comes without `storedLevels`). The handle stays the same:
    /// the device makes a texture with every level, copies level 0, makes each further level the 2x2
    /// box mean of the one above (xt/image_mips.h; Rgba8) or uploads `storedLevelCount` - 1 stored
    /// levels from `storedLevels` (levels 1.. back to back, block-compressed formats), and puts it in
    /// place of the old one (a shared image for every device of its owner) before any later draw of
    /// it is executed. A texture that has its chain already returns its level count at once.
    virtual uint32_t requestMipChain(TextureHandle tex, const void* storedLevels = nullptr,
                                     uint32_t storedLevelCount = 0) {
        (void)tex;
        (void)storedLevels;
        (void)storedLevelCount;
        return 1;
    }

    // -- Draw capabilities (T-691, Docs/design/10-backends.md §10.14). Appended with default
    //    implementations: a device that says nothing only receives draws it could already draw. --

    /// True when the device multiplies the shading result by `DrawCmd::coverage`, sampled as an R8
    /// coverage texture (T-660b's coverage quads). False (the default): `FrameBuilder::build` emits
    /// no draw with `coverage` and creates no coverage atlas texture; concave fills, strokes,
    /// outline glyphs, clips and borders take their earlier paths (convex fills with the edge
    /// fringe, stencil-then-cover, the glyph atlas), so their edges are not antialiased on a
    /// single-sampled target. Asked once per build.
    virtual bool supportsCoverageDraws() const { return false; }

    /// True when the device implements `Shading::BoxShadow` (with `DrawCmd::boxShadowMulti`).
    /// False (the default): whoever connects a `Context` to this device calls
    /// `Context::setBoxShadowCommands(false)`, so box shadows stay ordinary drop-shadow effects; a
    /// blurred `DrawBoxShadow` that still reaches `FrameBuilder::build` is not drawn, is counted
    /// (`FrameBuilder::BuildStats::boxShadowSkipped`) and reported once in `lastError`. Asked once
    /// per build.
    virtual bool supportsBoxShadowShading() const { return false; }
};

}  // namespace xt::gpu
