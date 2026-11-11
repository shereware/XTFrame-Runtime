#pragma once

#include <memory>

#include "xt/command_list.h"
#include "xt/damage.h"
#include "xt/gpu/gpu_device.h"
#include "xt/gpu/gpu_frame.h"
#include "xt/ir.h"
#include "xt/perf.h"

/// FrameBuilder: turns a CommandList into a GpuFrame (Docs/design/10-backends.md §10.5,
/// Docs/tasks/T-146.md). It is the CPU half of the former xt_gl renderer with every GL call
/// removed: path flattening, fan triangulation, stroke-to-fill, dashing, the stencil-then-cover
/// scheme, scissor/stencil clipping, layer/mask/effect scheduling over transient targets, the
/// §19.7 projection stack, image texture caching and glyph-atlas batching.
///
/// It never calls `GpuDevice::submit`: the host decides when the frame is drawn.
///
/// T-162 (Docs/tasks/T-162.md) adds two levels of reuse:
///
/// 1. **Static frame reuse.** When `Context::render(list, &damage)` reports no visual change
///    (`!damage.full && damage.rects.empty()`), the host asks `lastFrameReusable()` and, when it
///    says yes, skips `build` and submits the previous `GpuFrame` again. The builder answers no
///    whenever anything the frame depends on moved since that build (surface size, glyph atlas,
///    external texture bindings, an invalidated asset, the fringe setting) or when one more
///    build would produce a cheaper frame (an effect result not yet cached, see 2).
/// 2. **Effect result cache.** Every `PushEffect..PopEffect` segment whose stack does not read
///    the backdrop and draws no external (`engine://`) texture is hashed (its commands' bytes and
///    pools, dpr, surface size, the clip and projection state at PushEffect, the parent window
///    and the edge-fringe setting). A segment whose hash was seen in the previous build and is
///    not cached yet has its final effect output copied into a device target acquired with
///    `GpuDevice::acquireCachedTarget` (`GpuFrame::cacheStores`); a segment whose hash is cached
///    is skipped entirely and replaced by one `Shading::Texture` quad of the cached target over
///    its window (same scissor, opacity 1, Normal: exactly what the composite would do).
///    Animated segments hash differently every frame and are never stored, so a full-animation
///    frame carries no extra copy. Targets untouched for a whole build are released at the end
///    of that build; the cache also stops storing at `kEffectCacheBudgetBytes` /
///    `kEffectCacheMaxEntries` and is emptied by `resize`, `invalidateAsset` and `shutdown`.
///
/// T-343 (Docs/design/23-host-integration.md §23.aa, Docs/tasks/T-343.md) adds the host's
/// **Direct target mode** (`CommandList::hostTargetMode == HostTargetMode::Direct`, from
/// `Context::setHostTargetMode`): target 0 is the host's scene and is never cleared; the paint
/// order is split by `xt::scanSceneRuns` into *bands* (the commands between scene-reading
/// subtrees) and *scene runs* (a `mix-blend-mode` layer or a `backdrop-blur` effect node, marked
/// `kCommandSceneRead` by the Painter). Every band is drawn into its own stencil-capable transient
/// (its window = the union of its commands' bounds), copied into a persistent band target
/// (`GpuDevice::acquireBandTarget`) and pasted onto the scene with one SrcOver quad; on later
/// builds a band whose damage (`build`'s `damage`) does not touch its window, whose window and
/// command count are unchanged and whose place in the run structure is the same is pasted from
/// that target without walking its commands. Scene runs are drawn as in Layer mode (a transient
/// with the clips replayed, composited onto the scene), so the scene target itself only ever
/// receives quads and copies and needs no stencil. More than `kMaxBands` bands, or band windows
/// whose areas sum to more than `kBandAreaLimit` times the surface, drop the caching (every band
/// is still drawn through its transient, every frame; `FrameStats::directFallback`). The
/// region-limited backdrop copies and the shared blur pyramid of §23.aa.3 / 18 §18.15 live in
/// gpu_effects.cpp. `lastFrameReusable` in Direct mode means every band target is reusable (a
/// build that redrew a band says no once, so the host builds the pure-quad frame next). A Direct
/// list on a device whose `supportsDirectTarget()` is false is drawn in Layer mode and `lastError`
/// says so on the first such build.

namespace xt {
class TextShaper;
class AssetResolver;
}  // namespace xt

#ifndef XT_GPU_NO_TEXT
namespace xt::text {
class GlyphAtlas;
}  // namespace xt::text
#endif

namespace xt::gpu {

class FrameBuilder {
public:
    FrameBuilder();
    ~FrameBuilder();
    FrameBuilder(const FrameBuilder&) = delete;
    FrameBuilder& operator=(const FrameBuilder&) = delete;

    /// Binds the device textures are created on. Must precede `build`. Calling it again (or with
    /// a different device) first releases every texture created on the previous device.
    void init(GpuDevice& device);

    /// Releases every device texture this builder created (images, gradient tables, glyph atlas
    /// pages). Safe to call repeatedly; `init` must be called again before the next `build`.
    void shutdown();

    /// Sets the output surface size in pixels. Must be called before `build` and whenever the
    /// surface changes.
    void resize(int targetWidth, int targetHeight);

#ifndef XT_GPU_NO_TEXT
    /// Injects the glyph atlas DrawGlyphs samples from (Docs/design/10-backends.md §10.3.4). Not
    /// owned; the caller keeps it alive. Null (the default) makes every DrawGlyphs command use
    /// the outline-fill fallback.
    void setGlyphAtlas(xt::text::GlyphAtlas* atlas);
#endif

    /// Builds `out` from `list`. `shaper` is only the DrawGlyphs outline fallback (no atlas, a
    /// rotated/mirrored/non-uniformly-scaled run, `text-rendering: geometricPrecision`, or a glyph
    /// the atlas cannot hold; atlas glyphs are rasterized at the run's scaled size, T-263b; T-263c: also a
    /// run rasterized larger than 96 device px, and a run whose size differs from the one the previous
    /// build drew it at -- a scale animation -- until that size has held for one build); a null `shaper`
    /// draws nothing for such glyphs. T-263e: a run with a glyph effect (`GlyphsCmd::effect`) has no
    /// outline form and is exempt from both rules; instead one such run may occupy at most a quarter of
    /// one atlas page -- an em square of its bitmap size per distinct glyph and subpixel phase. A run over
    /// that is filled from its outlines without the effect, puts nothing into the atlas, and `lastError`
    /// says "glyph effect run too large for the atlas; drawn without its effect" (once per build).
    /// `assets`/`ir` decode images on first use (null: images are skipped); an
    /// `engine://<name>` image instead samples the host texture bound with
    /// `AssetResolver::setExternalTexture` (unbound: drawn as nothing; Docs/tasks/T-150.md). `perf`, when
    /// enabled, adds this call's duration to the current frame's `FrameStats::gpuBuildMs`. `out` is cleared
    /// first (capacity retained).
    ///
    /// T-263b: image textures are cached by `AssetId`, which only means something within one IR
    /// and one resolver. T-263c: they are cached under the pair (`ir->loadSerial()`, `assets`)
    /// of the build, and the two most recently built pairs keep their textures (two documents
    /// built in turn upload nothing again); a build with a third pair first destroys every
    /// texture of the least recently built one. A view opened again -- also into the same
    /// `IrView` object over the same blob address -- has a new serial and so is another pair; a
    /// resolver replaced by another object at the same address is not noticed
    /// (`invalidateAllAssets()`). Two assets share one texture only as cells of one atlas page
    /// (`atlas:<n>#<asset>` names in `ir`), never merely because the resolver returned the same
    /// image address for both (the base `AssetResolver::imageView()` does).
    ///
    /// T-263d: a list that names its sources (`list.sources`, filled by `Context::render` with one
    /// entry per Document) resolves each asset command and each custom effect shader against its
    /// own source's IR; `ir` is then not used for assets and may be null. The textures are cached
    /// under (`assets`, the source's `loadSerial`, `AssetId`, hi-dpi variant), every source of the
    /// build being one pair above: all the sources one build uses are kept (the number kept is
    /// max(2, that build's sources)). `assets` is the one resolver of every source.
    ///
    /// T-343: `damage` is this frame's dirty list from `Context::render(list, &damage)`, in device
    /// pixels; Direct mode reuses a band target only when no rectangle meets the band's window
    /// (null or `full` redraws every band). Layer mode ignores it.
    void build(const CommandList& list, TextShaper* shaper, AssetResolver* assets, const IrView* ir,
               GpuFrame& out, PerfTracker* perf = nullptr, const DamageList* damage = nullptr);

    /// Edge anti-aliasing for convex fills (Docs/tasks/T-155.md): on (the default), a single
    /// convex contour is drawn directly with a 1 px coverage fringe (a ring of alpha-0 vertices
    /// around an inset fan) so its edges are smooth on a target without multisampling. A host
    /// whose target is multisampled turns it off, since MSAA already resolves the edge and the
    /// two together thin the shape by about a quarter pixel. T-660b: with it on, concave and
    /// multi-contour fills (FillPath) are drawn from the coverage atlas (`lastCoverageStats`);
    /// with it off they go through stencil-then-cover and rely on MSAA.
    void setEdgeFringe(bool enabled);

    /// Drops the cached texture of `asset` (destroying it on the device); the next DrawImage of
    /// that asset decodes and uploads it again. Hosts call this when an asset is replaced. Also
    /// empties the effect result cache (T-162), since a cached result may have drawn the asset.
    /// T-263f: every IR numbers its assets from 0, so the id alone is ambiguous in a builder
    /// that holds the textures of several IRs. `source` null (the default) drops that id under
    /// every source, as before; otherwise only the texture cached under `source->loadSerial()`
    /// -- the IR of the Document (or view) whose asset was replaced -- and the other sources'
    /// textures of the same id are neither destroyed nor uploaded again. A `source` this builder
    /// holds nothing of drops no texture. Either way the last frame is not reusable afterwards.
    void invalidateAsset(AssetId asset, const IrView* source = nullptr);

    /// T-215: drops every cached image texture of every IR (destroying the ones this builder created),
    /// empties the effect result cache and forgets the last frame. For a host whose `AssetId`s were
    /// renumbered -- they are assigned per IR from 0, so a recompiled or newly loaded document can
    /// give an old id to another image -- or whose resolver was replaced. A resolver image pointer
    /// only ever matches a texture uploaded since the last call (atlas-page sharing, T-199).
    /// T-263e: the textures of an IR the Context dropped (a reload, an unload) are destroyed by
    /// `build` itself once a later source takes that IR's place (see `build`), so nothing grows
    /// without this call. The Context does not know this builder; a host that wants them gone at
    /// once forwards AssetResolver::releaseSource (which the Context calls for every IR it drops)
    /// to this function from its resolver.
    void invalidateAllAssets();

    /// T-675 (Docs/design/50-c-abi.md §50.ac item 2): lets go of what a hidden surface does not keep:
    /// empties the effect result cache and releases the band targets (`releaseCachedTarget`: back to
    /// the device, which keeps them for reuse), and gives back every image texture (a shared image:
    /// `releaseSharedImage(tex, true)`, as `shutdown` does; others destroyed). T-683 (§50.ad item 3):
    /// the coverage atlas too (its pages, CPU copies and keys; `lastCoverageStats().atlasBytes` is 0
    /// until a build makes it again). The gradient tables and the glyph atlas pages stay, so a later
    /// build does not make them again. The last frame is forgotten (the next `build` is a full one).
    void trim();

    /// T-162 static frame reuse: true when the `GpuFrame` the last `build` wrote may be submitted
    /// again, unchanged, for a frame whose CommandList is identical to that build's (the host
    /// knows this from `DamageList`: `!full && rects.empty()`). False when no frame was built
    /// since `init`/`resize`/`shutdown`/`invalidateAsset`/`setGlyphAtlas`/`setEdgeFringe`, when
    /// `assets`/`ir` are not the ones that build got (T-263b), when an `engine://` texture the frame samples
    /// was rebound (`assets`/`ir` are re-queried, so pass the same ones `build` got; null with external
    /// images in the frame means no), when the glyph atlas has changes pending upload, or when a further
    /// `build` would yield a cheaper frame (an effect segment that repeated but is not cached yet; T-263c:
    /// a glyph run drawn from outlines because its size had just changed). The glyph
    /// atlas must not be modified by another consumer between the build and the reuse.
    /// T-263d: after a build of a list that names several sources, `ir` must be one of them (any
    /// Document's of that Context); an `engine://` texture sampled by another source than `ir`'s
    /// cannot be re-queried through it and means no.
    /// T-263f: call it once per host frame. An image upload that failed is tried again 60 host
    /// frames later (three times at most), and only a build can try: each call counts one frame
    /// (a `build` counts one itself only when no call came since the previous build, so a host
    /// that asks and then builds and a host that only builds retry in the same frames), and when
    /// a retry of an image of the last build's sources has come due it answers false -- once per
    /// due retry -- so that the host builds and the attempt is made on a screen where nothing
    /// else changes. A host that does not build then is not asked again for that retry; the
    /// attempt is made by its next build.
    /// T-662: also false after a build that held a coverage shape back for the frame budget (see
    /// wantsAnotherFrame).
    bool lastFrameReusable(AssetResolver* assets, const IrView* ir) const;

    /// T-662 (Docs/design/10-backends.md §10.11 item 2, supplement 2): true when the last `build`
    /// drew a coverage shape with stencil-then-cover (hard edged) only because the frame's
    /// rasterization budget was used up; a further build draws it from the atlas. A host that
    /// skips frames with nothing changed builds once more while this is true (as if everything
    /// were dirty), until it is false. Shapes over the size limit do not count (another build
    /// would not change them). False before any build and after `shutdown`.
    bool wantsAnotherFrame() const;

    /// Effect result cache limits (T-162): total texels * 4 bytes of live cached targets, and
    /// their count. A segment that would exceed either is drawn every frame instead.
    static constexpr size_t kEffectCacheBudgetBytes = 64u * 1024u * 1024u;
    static constexpr size_t kEffectCacheMaxEntries = 256;

    /// T-343 (23 §23.aa.2): Direct mode caches at most this many bands, whose windows may cover
    /// at most `kBandAreaLimit` times the surface in total; beyond either the whole UI is drawn
    /// every frame (`FrameStats::directFallback`).
    static constexpr size_t kMaxBands = 4;
    static constexpr float kBandAreaLimit = 2.0f;

    /// Empty string if the last `build` had nothing to report (e.g. an image that failed to
    /// decode, a transient-target limit hit).
    const char* lastError() const;

    /// T-658 (Docs/design/50-c-abi.md §50.ab item 3 iv): what the effect result cache (T-162) did in
    /// the last `build` (zero after a build that drew nothing). Only cacheable segments count.
    struct EffectCacheStats {
        uint32_t hits = 0;    ///< segments drawn from their cached result (one quad)
        uint32_t misses = 0;  ///< cacheable segments drawn in full
        uint32_t stores = 0;  ///< results the frame copies into the cache (`GpuFrame::cacheStores`)
    };
    EffectCacheStats lastEffectCacheStats() const;

    /// T-660b (Docs/design/10-backends.md §10.11 item 2): what the coverage atlas did in the last
    /// `build` (hits, misses, fallbacks: of the frame it produced -- a build that had to start over
    /// because an atlas grew counts its last attempt; rasterizedPixels: the whole call). With
    /// `setEdgeFringe(true)`, a FillPath the direct paths do not take is drawn as one quad sampling
    /// a cached coverage bitmap; it falls back to stencil-then-cover (hard edged) when its bitmap
    /// would exceed 4096 px a side, when the frame's rasterization budget (2 Mpx of covered
    /// pixels, counted after each shape, T-662) is already used up, when the atlas cannot hold it,
    /// or when its transform is not finite. T-672: a bitmap over 512 px a side is stored and drawn
    /// as 256 px tiles; uniform tiles take no atlas space. Not counted at all: fills under a 3D
    /// projection or with `antialias == 0` (always stencil-then-cover).
    struct CoverageStats {
        uint32_t hits = 0;              ///< fills drawn from a bitmap already in the atlas
        uint32_t misses = 0;            ///< fills whose bitmap was not in the atlas
        uint64_t rasterizedPixels = 0;  ///< covered pixels of the bitmaps this build call rasterized (T-662)
        uint32_t fallbacks = 0;         ///< eligible fills drawn with stencil-then-cover instead
        uint64_t atlasBytes = 0;        ///< the atlas pages' bytes (R8; the device holds a copy)
        uint32_t tiles = 0;             ///< T-672: edge tiles (held in the atlas) of the tiled fills drawn
        uint32_t uniformTiles = 0;      ///< T-672: empty and full tiles of the tiled fills drawn
        /// T-682: glyph outlines this build asked of the shaper (none on a static frame at 1x).
        uint32_t glyphOutlines = 0;
    };
    CoverageStats lastCoverageStats() const;

    /// T-663 (Docs/design/10-backends.md §10.11 ruling 5 final): how the last `build` drew the
    /// clips that take a stencil level at 4x (rounded / rotated rects, clip paths), with
    /// `setEdgeFringe(true)` and no 3D projection; all zero otherwise.
    struct ClipStats {
        uint32_t skipped = 0;  ///< the content stays inside the shape's safe interior (or is empty): no clip
        uint32_t scissor = 0;  ///< axis-aligned on the pixel grid, corners untouched: a scissor level
        uint32_t imageDirect = 0;  ///< one image drawn as a fringed convex polygon instead of clipped
        uint32_t maskLayers = 0;   ///< the rest: the content composited through the shape's coverage
    };
    ClipStats lastClipStats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace xt::gpu
