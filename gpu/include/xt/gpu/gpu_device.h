#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "xt/gpu/gpu_frame.h"

/// The host-implemented device behind `FrameBuilder` (docs/design/10-backends.md §10.5). A device
/// owns textures and executes frames; it never sees a CommandList. xt_gl (from T-147) and the
/// CryEngine plugin (CE_XTUI/docs/05-stage2-design.md §5.2) are the two implementations.
///
/// Call order per frame: `FrameBuilder::build` calls `createTexture`/`updateTexture`/
/// `destroyTexture` as it goes (uploads happen *before* the frame that uses them is submitted);
/// the host then calls `submit` whenever it wants the frame drawn. A frame references only
/// textures that exist at `submit` time.

namespace xt::gpu {

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

    // -- Effect result cache (T-162, docs/tasks/T-162.md). Appended with default
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

    // -- Custom effects (T-203, docs/design/18-effects.md §18.7). Appended with a default
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

    // -- Device limits (docs/tasks/T-263d.md). Appended with a default implementation: a device
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

    // -- Direct target mode (T-343, docs/design/23-host-integration.md §23.aa.4). Appended with
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
};

}  // namespace xt::gpu
