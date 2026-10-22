#include "xt/gpu/frame_builder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "damage_internal.h"  // Code/Runtime/core/src: hashCommandList (T-162 effect cache keys)
#include "effect_math.h"      // Code/Runtime/core/src: geometry and effect math shared with xt::sw (T-414)
#include "frame_emit.h"
#include "gpu_effects.h"
#include "gradient_lut.h"
#include "xt/assert.h"
#include "xt/asset_resolver.h"
#include "xt/damage.h"
#include "xt/effect_utils.h"
#include "xt/geom.h"
#include "xt/project3d.h"
#include "xt/text_shaper.h"

#ifndef XT_GPU_NO_TEXT
#include "xt/text/glyph_atlas.h"
#endif

/// The CPU half of Code/Runtime/gl/src/gl_renderer.cpp (Docs/tasks/T-146.md): identical geometry
/// (device-pixel positions via `deviceXf = xf * dpr`, gradient evaluation in raw path-local
/// space through the inverse transform, §19.7 projection stack with homogeneous image/glyph
/// quads) and identical scheduling of layers, masks and effects over offscreen targets. What
/// differs from xt_gl is spelled out in gpu_frame.h's conventions and in the comments below
/// ("Unlike xt_gl").

namespace xt::gpu {

using detail::FrameEmitter;
using detail::Geometry;
using detail::Scissor;

namespace {

/// Largest polygon the convex fast path takes (Docs/tasks/T-155.md point 1); a circle flattened
/// at kFlattenTolerance stays under this up to a radius of several hundred pixels.
constexpr size_t kMaxConvexVertices = 512;

Mat2x3 deviceTransform(const Mat2x3& xf, float dpr) { return mul(scaling(Vec2{dpr, dpr}), xf); }

/// FNV-1a 64 over `count` bytes, continuing from `hash` (T-162 effect cache keys; the same
/// function Code/Runtime/core/src/damage.cpp uses per command).
uint64_t fnv1a(uint64_t hash, const void* data, size_t count) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < count; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}
constexpr uint64_t kFnvBasis = 14695981039346656037ull;

Mat2x3 safeInverse(const Mat2x3& m) {
    Mat2x3 inv;
    if (!invert(m, inv)) {
        inv = Mat2x3{};
    }
    return inv;
}

uint32_t packPremultiplied(Color c) {
    float a = std::clamp(c.a, 0.0f, 1.0f);
    auto ch = [a](float v) {
        float p = std::clamp(v, 0.0f, 1.0f) * a;
        return static_cast<uint32_t>(std::lround(p * 255.0f)) & 0xFFu;
    };
    uint32_t ab = static_cast<uint32_t>(std::lround(a * 255.0f)) & 0xFFu;
    return ch(c.r) | (ch(c.g) << 8) | (ch(c.b) << 16) | (ab << 24);
}

Rect boundsOf(std::span<const Vec2> pts) {
    if (pts.empty()) {
        return Rect{0, 0, 0, 0};
    }
    float minX = pts[0].x, maxX = pts[0].x, minY = pts[0].y, maxY = pts[0].y;
    for (const Vec2& p : pts) {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }
    return Rect{minX, minY, maxX - minX, maxY - minY};
}

struct HomoPoint {
    float x, y, w;
};

/// gl_renderer.cpp's projectHomogeneous: the outermost projection level's divide is left to the
/// GPU (Docs/design/19-3d-space.md §19.7's GL bullet).
HomoPoint projectHomogeneous(std::span<const project3d::Mapping> mappings, Vec2 p, float dpr, bool& ok) {
    ok = true;
    if (mappings.empty()) {
        return HomoPoint{p.x, p.y, 1.0f};
    }
    const float scale = dpr > 0.0f ? dpr : 1.0f;
    p = Vec2{p.x / scale, p.y / scale};
    Vec2 q = p;
    if (mappings.size() > 1) {
        q = project3d::projectPoint(mappings.subspan(1), Mat2x3{}, p, ok);
        if (!ok) {
            return HomoPoint{0, 0, 0};
        }
    }
    const project3d::Mapping& outer = mappings[0];
    Mat4 m = mat4Mul(outer.viewProj, outer.planeWorld3);
    float px = q.x, py = q.y;
    float x = m.m[0] * px + m.m[1] * py + m.m[3];
    float y = m.m[4] * px + m.m[5] * py + m.m[7];
    float w = m.m[12] * px + m.m[13] * py + m.m[15];
    if (w <= 1e-6f) {
        ok = false;
        return HomoPoint{0, 0, 0};
    }
    const Mat2x3& a = outer.rootXf;
    float outX = a.a * x + a.c * y + a.e * w;
    float outY = a.b * x + a.d * y + a.f * w;
    return HomoPoint{outX * scale, outY * scale, w};
}

/// T-201: the paint kinds drawn with Shading::Gradient (a lookup texture and GradientParams).
bool isGradientKind(PaintKind kind) {
    return kind == PaintKind::Linear || kind == PaintKind::Radial || kind == PaintKind::Conic;
}

/// True when `xf` maps axis-aligned rectangles to axis-aligned rectangles (no rotation/shear,
/// or an exact quarter turn).
bool xfKeepsAxisAlignment(const Mat2x3& xf) {
    return (xf.b == 0.0f && xf.c == 0.0f) || (xf.a == 0.0f && xf.d == 0.0f);
}

#ifndef XT_GPU_NO_TEXT
float coverageGamma(const Color& color) {
    const float lum = 0.299f * color.r + 0.587f * color.g + 0.114f * color.b;
    return lum > 0.5f ? 1.0f / 1.2f : 1.2f;
}
#endif

/// Pixel-center rasterization of a float interval: pixel `i` is covered iff `i + 0.5` lies in
/// `[lo, hi)`. Returns the covered [first, last) integer range.
void pixelRange(float lo, float hi, int& first, int& last) {
    first = static_cast<int>(std::ceil(lo - 0.5f));
    last = static_cast<int>(std::ceil(hi - 0.5f));
}

/// T-263c ruling 4: the largest bitmap size (device px) a glyph run is put on the atlas at; a
/// larger run is filled from outlines, so one enlarged label cannot fill the atlas pages. GPU
/// only: the software renderer keeps its own 24 px bitmap limit, and between the two a run is a
/// hinted bitmap here and an outline fill there.
[[maybe_unused]] constexpr float kMaxAtlasGlyphPx = 96.0f;

/// T-263e ruling D: the share of one atlas page a single glyph-effect run may occupy (see
/// Impl::effectRunFitsAtlas); a run over it is drawn from outlines without its effect.
[[maybe_unused]] constexpr double kEffectRunAtlasShare = 0.25;

/// The bundle atlas page `asset` is a cell of, as its name in `ir` says (`atlas:<n>#<asset>`,
/// xt/asset_resolver.h); empty for any other asset.
std::string_view atlasPageOf(AssetId asset, const IrView& ir) {
    const std::span<const IrAsset> assets = ir.assets().assets();
    const size_t index = static_cast<size_t>(asset);
    return index < assets.size() ? atlasAssetPage(ir.string(assets[index].name)) : std::string_view();
}

}  // namespace

struct FrameBuilder::Impl {
    GpuDevice* device = nullptr;
    int targetWidth = 0;
    int targetHeight = 0;
    std::string lastError;
    /// FrameBuilder::setEdgeFringe: 1 px coverage fringe on convex fills (off under MSAA).
    bool edgeFringe = true;

    FrameEmitter emit;
    detail::GradientLutCache luts;

#ifndef XT_GPU_NO_TEXT
    xt::text::GlyphAtlas* glyphAtlas = nullptr;
    /// Device pages of the atlas (R8), created lazily per atlas texture index.
    TextureHandle atlasTextures[4] = {0, 0, 0, 0};
    int atlasTextureSize = 0;
    /// Set when the atlas reset during the current build attempt (see build()).
    bool atlasResetSeen = false;
    bool atlasAllowed = true;
    /// T-263e ruling D: scratch of effectRunFitsAtlas() (the run's distinct atlas entries), and
    /// whether this build already reported an effect run over the bound in `lastError`.
    std::vector<uint64_t> effectRunKeys;
    bool effectRunReported = false;
#endif

    /// One uploaded image. T-199: keyed by (asset, variant) since the `@2x` (T-345: `@3x`) variant
    /// is a different
    /// texture; `shared` = the resolver's image pointer, so every asset of one atlas page reuses
    /// one texture (the first entry owns it, `owned`); `straight` = uploaded straight-alpha
    /// blocks (a compressed DDS), sampled with Shading::TextureStraight.
    struct TextureEntry {
        AssetId asset = kInvalidAsset;
        TextureHandle tex = 0;
        uint8_t variant = 0;  ///< T-345: 0 = 1x, 1 = `@2x`, 2 = `@3x` (the scale it was asked at).
        bool straight = false;
        bool owned = true;
        const void* shared = nullptr;
        /// T-215: `assetEpoch` when uploaded; `shared` pointers only match within one epoch.
        uint32_t epoch = 0;
        /// T-263c ruling 3: the `SourceSlot::id` whose IR numbers `asset`.
        uint32_t source = 0;
        /// T-263c ruling 1: the size of the uploaded image (an atlas page's, for a cell).
        int width = 0;
        int height = 0;
        /// T-263d: what a texture-0 entry stands for. `refused`: the image is larger than the
        /// device's limit and is not tried again. Otherwise an upload failed in host frame
        /// `failedAt` (a `frameCount`), after `retries` earlier retries. T-263f: `announced` =
        /// lastFrameReusable() already said no for the retry that is now due.
        bool refused = false;
        bool announced = false;
        uint8_t retries = 0;
        uint64_t failedAt = 0;
    };
    std::vector<TextureEntry> textureCache;
    /// T-263d: a failed upload is tried again this many host frames later, this many times at
    /// most. (The name says builds: a host that builds every frame counts one per build.)
    static constexpr uint64_t kUploadRetryBuilds = 60;
    static constexpr uint8_t kUploadRetries = 3;
    /// T-263f: the host's frames, which is what the retries are counted in. lastFrameReusable()
    /// -- asked once per frame by a host that reuses frames -- adds one and sets
    /// `frameCounted`; build() adds one only when no lastFrameReusable() call came since the
    /// previous build, and clears the mark. A host that asks and then builds and a host that
    /// only builds so reach the same count in the same frame.
    uint64_t frameCount = 0;
    bool frameCounted = false;
    bool retryDue(const TextureEntry& entry) const {
        return entry.tex == 0 && !entry.refused && entry.retries < kUploadRetries &&
               frameCount - entry.failedAt >= kUploadRetryBuilds;
    }
    /// GpuDevice::maxTextureSize() as the build in progress read it (0 = unknown).
    uint32_t maxTexture = 0;
    bool overTextureLimit(int width, int height) const {
        return maxTexture != 0 &&
               (static_cast<uint32_t>(width) > maxTexture || static_cast<uint32_t>(height) > maxTexture);
    }
    /// T-215: bumped by `invalidateAllAssets()` (a new IR or resolver may reuse an image address).
    uint32_t assetEpoch = 0;
    /// Docs/tasks/T-263b.md E4: where a cached texture's `AssetId` comes from -- the IR and the
    /// resolver of a build that had both. T-263c ruling 2b: the IR is named by
    /// IrView::loadSerial(), which a copy of the view shares and a blob rebuilt at the same
    /// address and opened again does not.
    struct AssetSource {
        const AssetResolver* assets = nullptr;
        uint32_t irSerial = 0;

        bool operator==(const AssetSource&) const = default;
    };
    static AssetSource assetSourceOf(const AssetResolver* assets, const IrView* ir) {
        return AssetSource{assets, ir->loadSerial()};
    }
    /// T-263c ruling 3: textures are cached under (source, AssetId) and the sources of the
    /// `kMaxAssetSources` most recently built IRs are kept, so a builder shared by two documents
    /// (the CryEngine plugin: one IR per plate) does not upload every image again on each
    /// switch. A build with a source that is not kept takes a free slot, or else the least
    /// recently built one, all of whose textures are destroyed first. Ids are never reused.
    /// T-263d: one build may use several sources (a list painted from several Documents names
    /// each in CommandList::sources); every source of the build in progress is kept, so the
    /// number kept is max(kMaxAssetSources, the sources the latest build used).
    static constexpr size_t kMaxAssetSources = 2;
    struct SourceSlot {
        AssetSource source;
        uint32_t id = 0;  ///< 0 = free
        uint64_t lastBuild = 0;
        uint64_t usedIn = 0;  ///< `buildCount` of the last build that selected it
    };
    std::vector<SourceSlot> sourceSlots;
    /// The slot id of the source whose `AssetId`s are being resolved: the build's only one, or
    /// (T-263d) the one of the asset command at hand (0 = none yet).
    uint32_t currentSource = 0;
    uint32_t nextSourceId = 1;
    uint64_t sourceClock = 0;
    /// T-263d: counts build() calls (SourceSlot::usedIn).
    uint64_t buildCount = 0;
    /// T-263d: the slot id of every source of the last build that had a resolver and an IR --
    /// one per CommandList::sources entry in order (0 for an entry without an IR), or the call
    /// IR's alone for a list that names none.
    std::vector<uint32_t> listSources;
    /// What an effect segment's cache key says about `listSources` (the only id when there is
    /// one, else a hash of them all).
    uint32_t listSourcesKey = 0;
    std::vector<uint8_t> premultiplyScratch;

#ifndef XT_GPU_NO_TEXT
    /// T-263c ruling 4: the bitmap size of every atlas-capable glyph run, by run (its font,
    /// glyphs and glyph positions, which a transform animation leaves alone), in the previous
    /// build (sorted by key) and in the current one. A run found with another size only is being
    /// scaled: it is filled from outlines until its size has held for one build. Member scratch.
    struct RunSize {
        uint64_t key = 0;
        float size = 0;
    };
    std::vector<RunSize> prevRunSizes;
    std::vector<RunSize> curRunSizes;
    /// Runs the current build kept off the atlas for that reason: one more build would move them
    /// (back) to the atlas, so `lastFrameReusable` says no meanwhile.
    int scalingRuns = 0;
#endif

    // --- T-162 effect result cache ---------------------------------------------------------
    struct CacheEntry {
        uint64_t hash = 0;
        TextureHandle texture = 0;
        uint16_t width = 0, height = 0;
        bool touched = false;  ///< hit or stored during the current build
    };
    std::vector<CacheEntry> cacheEntries;
    size_t cacheBytes = 0;
    /// Hashes of the previous build's cacheable segments (a segment is stored on its second
    /// consecutive appearance, so an animating one never pays for a copy) and this build's.
    std::vector<uint64_t> prevSegmentHashes;
    std::vector<uint64_t> curSegmentHashes;
    /// Per-command hashes of the current list, computed on the first cacheable segment.
    std::vector<uint64_t> cmdHashes;
    bool cmdHashesValid = false;
    /// The first `acquireCachedTarget` ever returned 0: the device does not cache (the default
    /// implementation), so segments are neither hashed nor counted as pending.
    bool cacheUnsupported = false;
    bool cacheEverAcquired = false;
    /// Cacheable segments this build drew in full although a later build could serve them from
    /// the cache (first appearance, or stored this build): while > 0 the frame is not the
    /// cheapest one for this list, so `lastFrameReusable` says no.
    int cachePending = 0;
    /// `cacheEntries.size()` when the current build() started (buildOnce may restart).
    size_t cacheEntriesAtBuildStart = 0;

    // --- T-162 static frame reuse ----------------------------------------------------------
    bool lastBuildValid = false;
    struct ExternalRef {
        AssetId asset = 0;
        TextureHandle texture = 0;
        bool straightAlpha = false;
        uint32_t source = 0;  ///< T-263d: the SourceSlot::id whose IR numbers `asset`
    };
    /// External (`engine://`) bindings the last frame sampled, re-checked by lastFrameReusable.
    std::vector<ExternalRef> lastExternals;

    // --- T-343 Direct target mode (Docs/design/23-host-integration.md §23.aa) -----------------
    /// The build in progress draws in Direct mode (the list asked for it and the device can).
    bool directMode = false;
    /// `lastError` already said once that the device cannot read target 0.
    bool directWarned = false;
    /// This build drops the band caching (too many bands, too much area, no band targets).
    bool directFallback = false;
    /// `CommandList::sceneStatic` of the build in progress (scene readers may be cached).
    bool sceneStaticBuild = false;
    /// One cached band of the paint order: its persistent target (0 = none), the window it
    /// was drawn at, how many commands it held and the T-162 segment hashes it drew (kept alive
    /// while the band is reused without a walk).
    struct Band {
        TextureHandle texture = 0;
        Scissor window;
        uint32_t cmdCount = 0;
        std::vector<uint64_t> segmentHashes;
    };
    std::vector<Band> prevBands;  ///< the last build's bands (texture ownership)
    std::vector<Band> curBands;   ///< the build in progress
    /// The last build's run structure (0 band / 1 scene run), for the "same place" test.
    std::vector<uint8_t> prevRunKinds;
    std::vector<uint8_t> curRunKinds;
    /// False until a Direct build completes and after anything that invalidates every band
    /// (resize, shutdown, invalidated assets, atlas or fringe change, a Layer-mode build).
    bool prevBandsValid = false;
    /// Bands stored this build: the next build pastes them, so the frame is not the cheapest
    /// one yet (lastFrameReusable says no once).
    int bandPending = 0;
    /// The segment-hash list of the band being drawn (PushEffect records into it); null outside
    /// a band.
    std::vector<uint64_t>* currentBandHashes = nullptr;
    std::vector<SceneRun> runs;
    std::vector<Scissor> bandWindows;  ///< per run (empty for scene runs and empty bands)
    /// A merged backdrop region (§23.aa.3): every top-level backdrop-blur run whose read
    /// rectangle meets another's shares one pyramid, built from the scene at the first of them
    /// and released after the last.
    struct PyramidCluster {
        Scissor window;
        float maxSigma = 0.0f;
        int remaining = 0;
        bool built = false;
        detail::BackdropPyramid pyramid;
    };
    std::vector<PyramidCluster> clusters;
    std::vector<int> runCluster;  ///< per run: its cluster, or -1
    int currentCluster = -1;      ///< the cluster of the scene run being drawn
    const DamageList* damage = nullptr;
    uint32_t statBands = 0;
    uint32_t statBandRedraws = 0;
    uint32_t statBackdropCopyPx = 0;
    uint32_t statPyramidLevels = 0;

    // --- Per-build state -------------------------------------------------------------------
    const CommandList* list = nullptr;
    float currentDpr = 1.0f;
    std::vector<project3d::Mapping> mappings;

    /// One clip level. Rect clips that stay axis-aligned become a scissor (no stencil); every
    /// other clip is a stencil level whose geometry is kept so it can be replayed into a newly
    /// opened offscreen target (unlike xt_gl, which copies the parent's stencil buffer instead).
    struct ClipLevel {
        bool stencil = false;
        Scissor prevScissor;
        Geometry winding;  ///< the shape's fan triangles
        Geometry quad;     ///< its AABB cover quad
        FillRule rule = FillRule::NonZero;
    };
    std::vector<ClipLevel> clipStack;
    int clipDepth = 0;  ///< number of *stencil* levels currently active (0..15)

    enum class FrameKind { Layer, Mask, Effect };
    struct TargetFrame {
        FrameKind kind = FrameKind::Layer;
        TargetHandle parent = 0;
        TargetHandle a = 0;  ///< layer / mask shape / effect content
        TargetHandle b = 0;  ///< mask content
        /// T-343: the backdrop pyramid a leading BackdropBlur samples -- a shared cluster's
        /// (`sharedPyramid`, Direct mode at depth 0) or this frame's own (`ownPyramid`, built
        /// when `hasOwnPyramid`, released at PopEffect). The frame is moved around, so the own
        /// pyramid is never pointed at.
        const detail::BackdropPyramid* sharedPyramid = nullptr;
        bool hasOwnPyramid = false;
        detail::BackdropPyramid ownPyramid;
        float opacity = 1;
        BlendMode blend = BlendMode::Normal;
        MaskMode mode = MaskMode::Alpha;
        // Effect frames:
        std::vector<EffectInstance> screenParams;
        /// T-203: CommandList::time (custom effects' xtTime).
        float time = 0;
        Mat2x3 invScreenXf;
        Rect contentBoundsPx;
        // T-162: the segment's cache key, when it is cacheable at all.
        bool cacheable = false;
        uint64_t cacheHash = 0;
    };
    std::vector<TargetFrame> frameStack;

    // Scratch buffers reused across commands.
    std::vector<Vec2> flatPoints;
    std::vector<uint32_t> flatStarts;
    std::vector<uint8_t> flatClosed;
    std::vector<Vec2> dashPoints;
    std::vector<uint32_t> dashStarts;
    std::vector<uint8_t> dashClosed;
    std::vector<float> scaledDashes;
    std::vector<Vec2> strokeOutPoints;
    std::vector<uint32_t> strokeOutStarts;
    std::vector<PathVerb> glyphOutlineVerbs;
    std::vector<float> glyphOutlinePoints;
    std::vector<Vertex> meshVertexScratch;  // T-245: one DrawImageMesh's projected vertices.
    std::array<std::vector<Vertex>, 4> glyphBatch;

    /// Winding-nibble clears not yet emitted (Docs/tasks/T-155.md point 3): the AABB (surface
    /// pixels) of every stencil fill drawn since the last flush, all in the current pass under
    /// the current scissor. A later stencil fill whose AABB is disjoint from all of them cannot
    /// see their dirty nibbles, so the clears wait and go out as one command.
    std::vector<Rect> pendingClears;

    /// Per-frame parameter dedupe so identical runs share a `paramIndex` and batch: the two
    /// glyph gammas and the last gradient.
    float glyphGamma[2] = {0, 0};
    uint32_t glyphParam[2] = {0, 0};
    int glyphParamCount = 0;
    GradientParams lastGradient{};
    uint32_t lastGradientIndex = 0;
    bool haveLastGradient = false;

    // ---------------------------------------------------------------------------------------
    // Lifetime.
    // ---------------------------------------------------------------------------------------

    void init(GpuDevice& dev) {
        shutdown();
        device = &dev;
        luts.init(device);
        // T-162: learn once whether the device caches, so a non-caching device never sees a
        // segment counted as "pending" (which would cost it one extra build per static period).
        const TextureHandle probe = device->acquireCachedTarget(1, 1, TextureFormat::Rgba8);
        cacheUnsupported = probe == 0;
        cacheEverAcquired = probe != 0;
        device->releaseCachedTarget(probe);
    }

    void shutdown() {
        lastBuildValid = false;
        flushEffectCache();
        releaseAllBands();
        if (device != nullptr) {
            for (const TextureEntry& entry : textureCache) {
                if (entry.owned) {
                    device->destroyTexture(entry.tex);
                }
            }
#ifndef XT_GPU_NO_TEXT
            for (TextureHandle& t : atlasTextures) {
                device->destroyTexture(t);
                t = 0;
            }
            atlasTextureSize = 0;
#endif
            luts.shutdown();
        }
        textureCache.clear();
        sourceSlots.clear();
        listSources.clear();
        listSourcesKey = 0;
        currentSource = 0;
        device = nullptr;
    }

    /// Frees the least recently built slot that the build in progress has not selected, with
    /// every texture uploaded for it (an atlas page is shared within one source only, so owners
    /// and sharers leave together). False when there is none.
    bool evictOldestSource() {
        SourceSlot* pick = nullptr;
        for (SourceSlot& slot : sourceSlots) {
            if (slot.id != 0 && slot.usedIn != buildCount &&
                (pick == nullptr || slot.lastBuild < pick->lastBuild)) {
                pick = &slot;
            }
        }
        if (pick == nullptr) {
            return false;
        }
        for (size_t i = 0; i < textureCache.size();) {
            const TextureEntry& entry = textureCache[i];
            if (entry.source != pick->id) {
                ++i;
                continue;
            }
            if (entry.owned) {
                device->destroyTexture(entry.tex);
            }
            textureCache.erase(textureCache.begin() + static_cast<std::ptrdiff_t>(i));
        }
        *pick = SourceSlot{};
        return true;
    }

    /// T-263c ruling 3: the slot `AssetId`s of `source` are looked up under (see SourceSlot),
    /// marked as used by the build in progress. A source that is not kept takes a free slot;
    /// the least recently built ones go first until it fits among max(kMaxAssetSources, the
    /// sources this build has selected so far and this one).
    uint32_t selectAssetSource(const AssetSource& source) {
        size_t occupied = 0;
        size_t usedNow = 0;
        for (SourceSlot& slot : sourceSlots) {
            if (slot.id == 0) {
                continue;
            }
            if (slot.source == source) {
                slot.lastBuild = ++sourceClock;
                slot.usedIn = buildCount;
                return slot.id;
            }
            ++occupied;
            usedNow += slot.usedIn == buildCount ? 1 : 0;
        }
        const size_t keep = std::max(kMaxAssetSources, usedNow + 1);
        while (occupied >= keep && evictOldestSource()) {
            --occupied;
        }
        SourceSlot* pick = nullptr;
        for (SourceSlot& slot : sourceSlots) {
            if (slot.id == 0) {
                pick = &slot;
                break;
            }
        }
        if (pick == nullptr) {
            pick = &sourceSlots.emplace_back();
        }
        pick->source = source;
        pick->id = nextSourceId++;
        if (nextSourceId == 0) {
            nextSourceId = 1;  // 0 means "free"
        }
        pick->lastBuild = ++sourceClock;
        pick->usedIn = buildCount;
        return pick->id;
    }

    /// T-263d: selects every source `cl` draws from -- its CommandList::sources entries, or the
    /// call's `ir` when it names none -- and remembers their slots in `listSources`. A build
    /// without a resolver or without any IR leaves the cache and `listSources` alone.
    void selectListSources(const CommandList& cl, const AssetResolver* assets, const IrView* ir) {
        if (assets == nullptr || (cl.sources.empty() && ir == nullptr)) {
            return;
        }
        listSources.clear();
        if (cl.sources.empty()) {
            listSources.push_back(selectAssetSource(assetSourceOf(assets, ir)));
        } else {
            // The sources that are kept already are this build's before any new one looks for
            // room, so a new one never evicts a source the same list still draws from.
            for (const CommandSource& s : cl.sources) {
                for (SourceSlot& slot : sourceSlots) {
                    if (s.ir != nullptr && slot.id != 0 && slot.source == AssetSource{assets, s.loadSerial}) {
                        slot.usedIn = buildCount;
                    }
                }
            }
            for (const CommandSource& s : cl.sources) {
                listSources.push_back(s.ir != nullptr ? selectAssetSource(AssetSource{assets, s.loadSerial})
                                                      : 0);
            }
        }
        currentSource = listSources[0];
        listSourcesKey = listSources.size() == 1
                             ? listSources[0]
                             : static_cast<uint32_t>(fnv1a(kFnvBasis, listSources.data(),
                                                           listSources.size() * sizeof(uint32_t)));
        // Sources the build before this one used beyond the two that are always kept go now.
        size_t occupied = 0;
        size_t usedNow = 0;
        for (const SourceSlot& slot : sourceSlots) {
            occupied += slot.id != 0 ? 1 : 0;
            usedNow += slot.id != 0 && slot.usedIn == buildCount ? 1 : 0;
        }
        while (occupied > std::max(kMaxAssetSources, usedNow) && evictOldestSource()) {
            --occupied;
        }
    }

    /// T-263d: the IR the asset command stamped `source` resolves its ids against (sourceIr()),
    /// with `currentSource` set to that IR's slot for the texture cache. nullptr = not drawn.
    const IrView* enterSource(const CommandList& cl, uint16_t source, const AssetResolver* assets,
                              const IrView* callIr) {
        const IrView* ir = sourceIr(cl, source, callIr);
        if (assets != nullptr && ir != nullptr) {
            const size_t index = cl.sources.empty() ? 0 : source;
            currentSource = index < listSources.size() ? listSources[index] : 0;
        }
        return ir;
    }

    /// The slot id `source` is kept under, 0 when it is not kept.
    uint32_t findAssetSource(const AssetSource& source) const {
        for (const SourceSlot& slot : sourceSlots) {
            if (slot.id != 0 && slot.source == source) {
                return slot.id;
            }
        }
        return 0;
    }

    // ---------------------------------------------------------------------------------------
    // §19.7 mapping-stack helpers (mirroring xt::sw / xt_gl).
    // ---------------------------------------------------------------------------------------

    bool projecting() const { return !mappings.empty(); }

    Vec2 projectDevicePoint(Vec2 devicePoint, bool& ok) const {
        const float dpr = currentDpr > 0.0f ? currentDpr : 1.0f;
        const Vec2 logical{devicePoint.x / dpr, devicePoint.y / dpr};
        const Vec2 projected = project3d::projectPoint(mappings, Mat2x3{}, logical, ok);
        return Vec2{projected.x * dpr, projected.y * dpr};
    }

    bool projectPointsInPlace(std::vector<Vec2>& pts) {
        for (Vec2& p : pts) {
            bool ok = false;
            Vec2 s = projectDevicePoint(p, ok);
            if (!ok) {
                return false;
            }
            p = s;
        }
        return true;
    }

    float flattenTolerance(const Mat2x3& deviceXf, std::span<const float> rawPoints) const {
        return effect_math::flattenTolerance(mappings, deviceXf, rawPoints);
    }

    HomoPoint homogeneousVertexPos(Vec2 p, bool& ok) {
        if (!projecting()) {
            ok = true;
            return HomoPoint{p.x, p.y, 1.0f};
        }
        return projectHomogeneous(mappings, p, currentDpr, ok);
    }

    // ---------------------------------------------------------------------------------------
    // Geometry.
    // ---------------------------------------------------------------------------------------

    void flatten(const PathRef& path, const Mat2x3& deviceXf, float tol) {
        std::span<const PathVerb> verbs(list->verbs.data() + path.verbOffset, path.verbCount);
        std::span<const float> points(list->points.data() + path.pointOffset, path.pointCount);
        flatPoints.clear();
        flatStarts.clear();
        flatClosed.clear();
        flattenPath(verbs, points, deviceXf, tol, flatPoints, flatStarts, flatClosed);
    }

    // T-160: a gradient fill/stroke under a 3D projection (19.7). Its contours are flattened in
    // plane space and then projected per point, so a vertex's screen position no longer maps back
    // to the paint's coordinates through `inv` alone: `projScreenToPlane` takes it back into plane
    // space first, and the vertex is written homogeneous (w from `projPlaneToScreen`, normalised by
    // `projWScale` so it stays positive) so the GPU interpolates u,v perspective-correctly, the
    // same way projected images are drawn. Only set between setupProjectedPaint() and the fill.
    bool projPaint = false;
    Mat3 projScreenToPlane;
    Mat3 projPlaneToScreen;
    float projWScale = 1.0f;

    /// Builds the plane <-> screen homographies from the plane-space AABB of `planePts` (the
    /// shape before projection) and its projected corners. Returns false (the paint then stays
    /// unprojected) for a degenerate box or a corner that fails to project.
    bool setupProjectedPaint(std::span<const Vec2> planePts) {
        Vec2 plane[4];
        if (!effect_math::planeBoxCorners(planePts, plane)) {
            return false;
        }
        Vec2 screen[4];
        for (int i = 0; i < 4; ++i) {
            bool ok = false;
            screen[i] = projectDevicePoint(plane[i], ok);
            if (!ok) {
                return false;
            }
        }
        if (!homographyFromQuad(screen, plane, projScreenToPlane) ||
            !homographyFromQuad(plane, screen, projPlaneToScreen)) {
            return false;
        }
        const float x0 = plane[0].x;
        const float y0 = plane[0].y;
        const float wRef = projPlaneToScreen.m[6] * x0 + projPlaneToScreen.m[7] * y0 + projPlaneToScreen.m[8];
        if (std::fabs(wRef) <= 1e-12f) {
            return false;
        }
        projWScale = 1.0f / wRef;
        return true;
    }

    /// A paint vertex at screen position `p`: u,v = the paint-local position (`inv` applied, after
    /// the projection homography when projPaint is set), homogeneous under projPaint.
    Vertex paintVertex(Vec2 p, const Mat2x3& inv, uint32_t rgba) const {
        if (!projPaint) {
            const Vec2 local = apply(inv, p);
            return Vertex{p.x, p.y, 1.0f, local.x, local.y, rgba};
        }
        const Vec2 q = mat3Apply(projScreenToPlane, p);
        const Vec2 local = apply(inv, q);
        float w = (projPlaneToScreen.m[6] * q.x + projPlaneToScreen.m[7] * q.y + projPlaneToScreen.m[8]) *
                  projWScale;
        if (!(w > 1e-6f)) {
            w = 1.0f;  // Defensive: a visible vertex always has w > 0; never emit a clipped one.
        }
        return Vertex{p.x * w, p.y * w, w, local.x, local.y, rgba};
    }

    /// Appends every contour of `pts`/`starts` as triangle fans from each contour's first point.
    /// `u,v` carry the raw path-local position (`inv` applied) for gradient evaluation.
    Geometry emitFans(std::span<const Vec2> pts, std::span<const uint32_t> starts, const Mat2x3& inv,
                      bool wantLocal) {
        Geometry g;
        Vertex* v = emit.appendVertices(static_cast<uint32_t>(pts.size()), g.baseVertex);
        for (size_t i = 0; i < pts.size(); ++i) {
            v[i] = wantLocal ? paintVertex(pts[i], inv, 0u) : Vertex{pts[i].x, pts[i].y, 1.0f, 0, 0, 0u};
        }
        g.vertexCount = static_cast<uint32_t>(pts.size());
        uint32_t triCount = 0;
        for (size_t i = 0; i < starts.size(); ++i) {
            uint32_t start = starts[i];
            uint32_t end = (i + 1 < starts.size()) ? starts[i + 1] : static_cast<uint32_t>(pts.size());
            if (end - start >= 3) {
                triCount += end - start - 2;
            }
        }
        uint32_t* idx = emit.appendIndices(triCount * 3, g.firstIndex);
        uint32_t k = 0;
        for (size_t i = 0; i < starts.size(); ++i) {
            uint32_t start = starts[i];
            uint32_t end = (i + 1 < starts.size()) ? starts[i + 1] : static_cast<uint32_t>(pts.size());
            if (end - start < 3) {
                continue;
            }
            for (uint32_t j = start + 1; j + 1 < end; ++j) {
                idx[k++] = start;
                idx[k++] = j;
                idx[k++] = j + 1;
            }
        }
        g.indexCount = triCount * 3;
        return g;
    }

    Geometry emitQuad(const Vec2 corners[4], const Mat2x3& inv, bool wantLocal, uint32_t rgba) {
        Vertex v[4];
        for (int i = 0; i < 4; ++i) {
            v[i] = wantLocal ? paintVertex(corners[i], inv, rgba)
                             : Vertex{corners[i].x, corners[i].y, 1.0f, 0, 0, rgba};
        }
        return emit.quad(v);
    }

    Geometry emitBoundsQuad(const Rect& bounds, const Mat2x3& inv, bool wantLocal, uint32_t rgba) {
        Vec2 corners[4] = {
            Vec2{bounds.x, bounds.y},
            Vec2{bounds.x + bounds.w, bounds.y},
            Vec2{bounds.x + bounds.w, bounds.y + bounds.h},
            Vec2{bounds.x, bounds.y + bounds.h},
        };
        return emitQuad(corners, inv, wantLocal, rgba);
    }

    // ---------------------------------------------------------------------------------------
    // Stencil-then-cover (Docs/design/10-backends.md §10.3.1).
    // ---------------------------------------------------------------------------------------

    void pushWinding(const Geometry& fans, FillRule rule) {
        DrawCmd c;
        c.shading = Shading::StencilOnly;
        c.blend = Blend::Replace;
        c.stencilOp = rule == FillRule::NonZero ? StencilOp::WriteIncrWrap : StencilOp::WriteInvert;
        c.stencilRef = static_cast<uint8_t>(clipDepth);
        emit.pushCmd(c, fans);
    }

    /// Emits every pending winding-nibble clear as one ClearRect command (one quad per AABB).
    /// Must run before anything that changes the pass or the scissor the fills were drawn under.
    void flushPendingClears() {
        if (pendingClears.empty()) {
            return;
        }
        Geometry g;
        const uint32_t count = static_cast<uint32_t>(pendingClears.size());
        Vertex* v = emit.appendVertices(count * 4, g.baseVertex);
        uint32_t* idx = emit.appendIndices(count * 6, g.firstIndex);
        for (uint32_t i = 0; i < count; ++i) {
            const Rect& r = pendingClears[i];
            v[i * 4 + 0] = Vertex{r.x, r.y, 1.0f, 0, 0, 0u};
            v[i * 4 + 1] = Vertex{r.x + r.w, r.y, 1.0f, 0, 0, 0u};
            v[i * 4 + 2] = Vertex{r.x + r.w, r.y + r.h, 1.0f, 0, 0, 0u};
            v[i * 4 + 3] = Vertex{r.x, r.y + r.h, 1.0f, 0, 0, 0u};
            idx[i * 6 + 0] = i * 4 + 0;
            idx[i * 6 + 1] = i * 4 + 1;
            idx[i * 6 + 2] = i * 4 + 2;
            idx[i * 6 + 3] = i * 4 + 0;
            idx[i * 6 + 4] = i * 4 + 2;
            idx[i * 6 + 5] = i * 4 + 3;
        }
        g.vertexCount = count * 4;
        g.indexCount = count * 6;
        pendingClears.clear();
        DrawCmd c;
        c.shading = Shading::StencilOnly;
        c.blend = Blend::Replace;
        c.stencilOp = StencilOp::ClearRect;
        c.stencilRef = static_cast<uint8_t>(clipDepth);
        emit.pushCmd(c, g);
    }

    static bool rectsOverlap(const Rect& a, const Rect& b) {
        return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
    }

    std::span<const GradientStop> gradientStops(const PaintRef& paint) const {
        if (static_cast<uint64_t>(paint.stopOffset) + paint.stopCount <= list->stops.size()) {
            return std::span<const GradientStop>(list->stops.data() + paint.stopOffset, paint.stopCount);
        }
        return {};
    }

    /// `GradientParams` of `paint` in the frame's pool; identical to the previous gradient's
    /// -> the same index, so consecutive fills of one gradient batch.
    uint32_t gradientParamIndex(const PaintRef& paint) {
        GradientParams gp{};
        gp.kind = static_cast<float>(paint.kind);
        gp.p0x = paint.p0.x;
        gp.p0y = paint.p0.y;
        gp.p1x = paint.p1.x;
        gp.p1y = paint.p1.y;
        float dx = paint.p1.x - paint.p0.x;
        float dy = paint.p1.y - paint.p0.y;
        float rx = std::sqrt(dx * dx + dy * dy);
        float ry = rx;
        if (paint.p2.x != paint.p0.x || paint.p2.y != paint.p0.y) {
            float ex = paint.p2.x - paint.p0.x;
            float ey = paint.p2.y - paint.p0.y;
            ry = std::sqrt(ex * ex + ey * ey);
        }
        gp.rx = rx;
        gp.ry = ry;
        gp.alpha = paint.color.a;
        if (haveLastGradient && std::memcmp(&gp, &lastGradient, sizeof(gp)) == 0) {
            return lastGradientIndex;
        }
        lastGradient = gp;
        lastGradientIndex = emit.pushParams(gp);
        haveLastGradient = true;
        return lastGradientIndex;
    }

    /// Fills `pts`/`starts` (already in surface pixels, projected if need be) with `paint`.
    /// Convex shapes take the direct paths (`fillDirect`); everything else is stencil-then-cover
    /// with its clear deferred (`pendingClears`).
    void fillShapeAndCover(std::span<const Vec2> pts, std::span<const uint32_t> starts,
                           const Mat2x3& deviceXf, FillRule rule, const PaintRef& paint,
                           bool antialias = true) {
        if (paint.kind == PaintKind::None || pts.empty() || emit.scissor().empty()) {
            return;
        }
        if (fillDirect(pts, starts, deviceXf, rule, paint, edgeFringe && antialias)) {
            return;
        }
        const bool gradient = isGradientKind(paint.kind);
        Mat2x3 inv = safeInverse(deviceXf);

        // Pass 1: winding into the stencil low nibble (gated to the current clip depth). The
        // nibble must be clean under the fan: flush the deferred clears when any overlaps.
        Rect bounds = boundsOf(pts);
        for (const Rect& r : pendingClears) {
            if (rectsOverlap(r, bounds)) {
                flushPendingClears();
                break;
            }
        }
        Geometry fans = emitFans(pts, starts, inv, false);
        if (fans.indexCount == 0) {
            return;
        }
        pushWinding(fans, rule);

        // Pass 2: cover the AABB, gated by the winding nibble.
        DrawCmd cover;
        cover.blend = Blend::SrcOver;
        cover.stencilOp = StencilOp::TestNotEqualZero;
        cover.stencilRef = static_cast<uint8_t>(clipDepth);
        uint32_t rgba = 0xFFFFFFFFu;
        if (gradient) {
            cover.shading = Shading::Gradient;
            cover.texture = luts.get(gradientStops(paint));
            cover.sampler = Sampler::LinearClamp;
            cover.paramIndex = gradientParamIndex(paint);
        } else {
            cover.shading = Shading::Solid;
            rgba = packPremultiplied(paint.color);
        }
        Geometry quad = emitBoundsQuad(bounds, inv, gradient, rgba);
        emit.pushCmd(cover, quad);

        // Pass 3: clear the winding nibble across the AABB -- deferred (see pendingClears).
        pendingClears.push_back(bounds);
    }

    // ---------------------------------------------------------------------------------------
    // Direct fills (Docs/tasks/T-155.md point 1). Three shapes skip stencil-then-cover and are
    // drawn as plain triangles in one Solid/Gradient command: a single simple convex contour
    // (fan), several convex contours whose boxes do not touch (fans, batched), and a convex
    // ring -- one convex contour inside another under EvenOdd, or NonZero with opposite
    // windings, i.e. a border (a zipped band). Each edge gets a NanoVG-style 1 px coverage
    // fringe when `edgeFringe` is on (the host's target has no MSAA).
    // ---------------------------------------------------------------------------------------

    /// A cleaned contour: consecutive/closing duplicates dropped, vertex order normalised to a
    /// positive shoelace area, plus its outward edge normals and per-vertex miters.
    struct Contour {
        std::vector<Vec2> pts;
        std::vector<Vec2> normals;  ///< normals[i]: outward unit normal of edge pts[i] -> pts[i+1]
        std::vector<Vec2> miters;   ///< miters[i]: outward offset direction at pts[i] (NanoVG's dm)
        float area = 0.0f;
        Rect bounds;
    };
    Contour contourA, contourB;
    /// Deduped copies of every contour of a multi-contour path (`contourStarts` indexes it).
    std::vector<Vec2> contourPool;
    std::vector<uint32_t> contourStarts;
    std::vector<Rect> contourBounds;
    /// Merge-walk scratch for rings.
    std::vector<float> ringAngles;
    static constexpr size_t kMaxDirectContours = 16;

    /// Copies `src` into `out.pts` (duplicates dropped, positive area) and returns true when it is
    /// a simple convex polygon: every turn has the same sign (collinear turns allowed) and x and y
    /// each change direction at most twice around the loop, which rules out one-signed
    /// self-overlapping "spirals". `out.area`/`out.bounds` are set; normals/miters are not.
    bool cleanConvexContour(std::span<const Vec2> src, Contour& out) {
        std::vector<Vec2>& pts = out.pts;
        pts.clear();
        constexpr float kDupSq = 1e-6f;
        for (const Vec2& p : src) {
            if (!pts.empty()) {
                const Vec2 q = pts.back();
                if ((p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y) <= kDupSq) {
                    continue;
                }
            }
            pts.push_back(p);
        }
        while (pts.size() > 1) {
            const Vec2 p = pts.front(), q = pts.back();
            if ((p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y) > kDupSq) {
                break;
            }
            pts.pop_back();
        }
        const size_t n = pts.size();
        if (n < 3 || n > kMaxConvexVertices) {
            return false;
        }
        float area = 0.0f;
        bool pos = false, neg = false;
        int dxChanges = 0, dyChanges = 0;
        int lastDx = 0, lastDy = 0;
        // Start the direction tracking from the last non-zero step so the cyclic count is exact.
        for (size_t i = n; i-- > 0 && (lastDx == 0 || lastDy == 0);) {
            const Vec2 e{pts[(i + 1) % n].x - pts[i].x, pts[(i + 1) % n].y - pts[i].y};
            if (lastDx == 0 && e.x != 0.0f) lastDx = e.x > 0.0f ? 1 : -1;
            if (lastDy == 0 && e.y != 0.0f) lastDy = e.y > 0.0f ? 1 : -1;
        }
        for (size_t i = 0; i < n; ++i) {
            const Vec2 a = pts[(i + n - 1) % n], b = pts[i], c = pts[(i + 1) % n];
            const Vec2 e0{b.x - a.x, b.y - a.y}, e1{c.x - b.x, c.y - b.y};
            area += b.x * c.y - c.x * b.y;
            const float cross = e0.x * e1.y - e0.y * e1.x;
            const float tol = 1e-5f * std::sqrt((e0.x * e0.x + e0.y * e0.y) * (e1.x * e1.x + e1.y * e1.y));
            if (cross > tol) pos = true;
            if (cross < -tol) neg = true;
            if (e1.x != 0.0f) {
                const int s = e1.x > 0.0f ? 1 : -1;
                if (s != lastDx) ++dxChanges;
                lastDx = s;
            }
            if (e1.y != 0.0f) {
                const int s = e1.y > 0.0f ? 1 : -1;
                if (s != lastDy) ++dyChanges;
                lastDy = s;
            }
        }
        area *= 0.5f;
        if ((pos && neg) || (!pos && !neg) || dxChanges > 2 || dyChanges > 2 || std::fabs(area) <= 1e-4f) {
            return false;
        }
        if (area < 0.0f) {
            std::reverse(pts.begin(), pts.end());
            area = -area;
        }
        out.area = area;
        out.bounds = boundsOf(pts);
        return true;
    }

    /// `contour`'s outward edge normals and NanoVG miters (expandFill's dm: the averaged
    /// normals scaled by 1 / |dm|^2, capped at 600 for near-180-degree spikes).
    static void computeMiters(Contour& contour) {
        const std::vector<Vec2>& pts = contour.pts;
        const size_t n = pts.size();
        contour.normals.resize(n);
        for (size_t i = 0; i < n; ++i) {
            const Vec2 a = pts[i], b = pts[(i + 1) % n];
            Vec2 e{b.x - a.x, b.y - a.y};
            const float len = std::sqrt(e.x * e.x + e.y * e.y);
            if (len > 0.0f) {
                e.x /= len;
                e.y /= len;
            }
            contour.normals[i] = Vec2{e.y, -e.x};  // positive area: (e.y, -e.x) points outward
        }
        contour.miters.resize(n);
        for (size_t i = 0; i < n; ++i) {
            const Vec2 n0 = contour.normals[(i + n - 1) % n], n1 = contour.normals[i];
            Vec2 dm{(n0.x + n1.x) * 0.5f, (n0.y + n1.y) * 0.5f};
            const float dmr2 = dm.x * dm.x + dm.y * dm.y;
            if (dmr2 > 1e-6f) {
                const float scale = std::min(1.0f / dmr2, 600.0f);
                dm.x *= scale;
                dm.y *= scale;
            }
            contour.miters[i] = dm;
        }
    }

    /// Minimum width of a convex contour (rotating calipers: for every edge, the farthest vertex
    /// behind it; the antipodal index only advances). Needs `normals`.
    static float convexMinWidth(const Contour& c) {
        const std::vector<Vec2>& pts = c.pts;
        const size_t n = pts.size();
        auto depth = [&](size_t i, size_t j) {
            return (pts[i].x - pts[j].x) * c.normals[i].x + (pts[i].y - pts[j].y) * c.normals[i].y;
        };
        size_t j = 0;
        for (size_t k = 1; k < n; ++k) {
            if (depth(0, k) > depth(0, j)) j = k;
        }
        float minWidth = depth(0, j);
        for (size_t i = 1; i < n; ++i) {
            size_t guard = 0;
            while (guard++ < n && depth(i, (j + 1) % n) > depth(i, j)) {
                j = (j + 1) % n;
            }
            minWidth = std::min(minWidth, depth(i, j));
        }
        return minWidth;
    }

    /// Fringe geometry for a thickness `t` (px): the core is inset by `woff` and its alpha
    /// scaled by `alphaScale`; the alpha-0 ring sits `1 - woff` outside, so the ramp is always
    /// 1 px wide and the integrated coverage equals the true area. Shapes thinner than 1 px keep
    /// a zero-width core with alpha = t (NanoVG's thin-stroke rule).
    static void fringeFor(float t, float& woff, float& alphaScale) {
        woff = std::min(0.5f, t * 0.5f);
        alphaScale = std::min(1.0f, t);
    }

    /// The Solid/Gradient command state for `paint` under the current clip; `innerRgba` is the
    /// core vertex color (alpha scaled by `alphaScale`).
    DrawCmd directCmd(const PaintRef& paint, float alphaScale, uint32_t& innerRgba) {
        DrawCmd c;
        c.blend = Blend::SrcOver;
        c.stencilOp = StencilOp::TestEqualRef;
        c.stencilRef = static_cast<uint8_t>(clipDepth);
        if (isGradientKind(paint.kind)) {
            c.shading = Shading::Gradient;
            c.texture = luts.get(gradientStops(paint));
            c.sampler = Sampler::LinearClamp;
            c.paramIndex = gradientParamIndex(paint);
            innerRgba = packPremultiplied(Color{1, 1, 1, alphaScale});
        } else {
            c.shading = Shading::Solid;
            Color color = paint.color;
            color.a *= alphaScale;
            innerRgba = packPremultiplied(color);
        }
        return c;
    }

    /// Writes `contour`'s vertices offset along their miters by `offset` (positive = outward)
    /// into `v`, with `rgba`; `u,v` carry the path-local position when `inv` is given.
    void writeOffsetVertices(const Contour& contour, float offset, uint32_t rgba, const Mat2x3* inv,
                             Vertex* v) const {
        const size_t n = contour.pts.size();
        for (size_t i = 0; i < n; ++i) {
            const Vec2 p{contour.pts[i].x + contour.miters[i].x * offset,
                         contour.pts[i].y + contour.miters[i].y * offset};
            v[i] = inv != nullptr ? paintVertex(p, *inv, rgba) : Vertex{p.x, p.y, 1.0f, 0, 0, rgba};
        }
    }

    /// Two triangles per edge between vertex rings `[a, a + n)` and `[b, b + n)`.
    static uint32_t* writeRingQuads(uint32_t* idx, uint32_t a, uint32_t b, uint32_t n) {
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t j = (i + 1) % n;
            *idx++ = a + i;
            *idx++ = a + j;
            *idx++ = b + j;
            *idx++ = a + i;
            *idx++ = b + j;
            *idx++ = b + i;
        }
        return idx;
    }

    /// Draws convex `contour` (surface pixels, cleaned) as one fan, with the fringe ring when
    /// `fringe`.
    void fillConvex(Contour& contour, const Mat2x3& deviceXf, const PaintRef& paint, bool fringe) {
        const size_t n = contour.pts.size();
        const bool gradient = isGradientKind(paint.kind);
        const Mat2x3 inv = safeInverse(deviceXf);
        const Mat2x3* invp = gradient ? &inv : nullptr;
        float woff = 0.0f, alphaScale = 1.0f;
        if (fringe) {
            computeMiters(contour);
            const float t = convexMinWidth(contour);
            if (t <= 1e-4f) {
                return;
            }
            fringeFor(t, woff, alphaScale);
        }
        uint32_t innerRgba = 0;
        DrawCmd c = directCmd(paint, alphaScale, innerRgba);

        Geometry g;
        const uint32_t nn = static_cast<uint32_t>(n);
        g.vertexCount = fringe ? 2 * nn : nn;
        Vertex* v = emit.appendVertices(g.vertexCount, g.baseVertex);
        if (fringe) {
            writeOffsetVertices(contour, -woff, innerRgba, invp, v);
            writeOffsetVertices(contour, 1.0f - woff, 0u, invp, v + n);
        } else {
            for (size_t i = 0; i < n; ++i) {
                const Vec2 p = contour.pts[i];
                v[i] = gradient ? paintVertex(p, inv, innerRgba) : Vertex{p.x, p.y, 1.0f, 0, 0, innerRgba};
            }
        }
        g.indexCount = (nn - 2) * 3 + (fringe ? nn * 6 : 0);
        uint32_t* idx = emit.appendIndices(g.indexCount, g.firstIndex);
        for (uint32_t j = 1; j + 1 < nn; ++j) {
            *idx++ = 0;
            *idx++ = j;
            *idx++ = j + 1;
        }
        if (fringe) {
            writeRingQuads(idx, 0, nn, nn);
        }
        emit.pushCmd(c, g);
    }

    /// True when every vertex of `inner` lies inside (or on) convex `outer` (needs
    /// `outer.normals`).
    static bool contourInside(const Contour& inner, const Contour& outer) {
        const size_t n = outer.pts.size();
        for (const Vec2& p : inner.pts) {
            for (size_t i = 0; i < n; ++i) {
                const float d =
                    (p.x - outer.pts[i].x) * outer.normals[i].x + (p.y - outer.pts[i].y) * outer.normals[i].y;
                if (d > 1e-4f) {
                    return false;
                }
            }
        }
        return true;
    }

    /// Band thickness estimate of a ring: the smallest distance from an inner vertex to an outer
    /// edge line (a lower bound of the true distance, so thin borders never come out too dark).
    static float ringMinWidth(const Contour& inner, const Contour& outer) {
        float t = 1e9f;
        const size_t n = outer.pts.size();
        for (const Vec2& p : inner.pts) {
            for (size_t i = 0; i < n; ++i) {
                const float d =
                    (outer.pts[i].x - p.x) * outer.normals[i].x + (outer.pts[i].y - p.y) * outer.normals[i].y;
                t = std::min(t, d);
            }
        }
        return t;
    }

    /// Draws the band between convex `outer` and convex `inner` (both cleaned, inner inside
    /// outer) as one command: a merge walk around the inner's centroid zips the two contours into
    /// n + m triangles; the fringe insets the outer edge and pushes the inner edge into the band,
    /// with alpha-0 rings beyond both. Returns false (nothing drawn) when a contour is not
    /// star-shaped around that centre, which only a degenerate input produces.
    bool fillRing(Contour& outer, Contour& inner, const Mat2x3& deviceXf, const PaintRef& paint,
                  bool fringe) {
        const size_t n = outer.pts.size(), m = inner.pts.size();
        Vec2 c{0, 0};
        for (const Vec2& p : inner.pts) {
            c.x += p.x;
            c.y += p.y;
        }
        c.x /= static_cast<float>(m);
        c.y /= static_cast<float>(m);
        // Unwrapped angles: outer from its vertex 0, inner rotated to start just after it.
        constexpr float kTwoPi = 6.28318530718f;
        ringAngles.resize(n + 2 * m + 2);
        float* ao = ringAngles.data();
        float* ai = ao + n + 1;
        for (size_t i = 0; i < n; ++i) {
            float a = std::atan2(outer.pts[i].y - c.y, outer.pts[i].x - c.x);
            if (i > 0) {
                while (a < ao[i - 1]) a += kTwoPi;
            }
            ao[i] = a;
        }
        ao[n] = ao[0] + kTwoPi;
        if (ao[n - 1] >= ao[n]) {
            return false;
        }
        // Inner angles normalised into [ao[0], ao[0] + 2pi) once, in vertex order (`an`), and the
        // unwrap below reads those values back. T-165: a border's inner and outer contours start
        // at the same corner, so one inner vertex sits at the outer's vertex-0 angle up to
        // rounding; when it landed a hair below ao[0], the start search wrapped it down
        // (a -= 2pi) while the unwrap only wrapped up, leaving the whole inner sequence one turn
        // ahead -- every outer edge then merged against that single inner vertex and the fan
        // filled the hole. One normalisation shared by both steps keeps the sequences on the same
        // turn (an inner vertex a rounding error below ao[0] is a harmless sliver).
        float* an = ai + m + 1;
        size_t j0 = 0;
        for (size_t j = 0; j < m; ++j) {
            float a = std::atan2(inner.pts[j].y - c.y, inner.pts[j].x - c.x);
            while (a < ao[0]) a += kTwoPi;
            while (a >= ao[0] + kTwoPi) a -= kTwoPi;
            an[j] = a;
            if (a < an[j0]) {
                j0 = j;
            }
        }
        for (size_t k = 0; k < m; ++k) {
            float a = an[(j0 + k) % m];
            if (k > 0) {
                while (a < ai[k - 1]) a += kTwoPi;
            }
            ai[k] = a;
        }
        ai[m] = ai[0] + kTwoPi;
        if (ai[m - 1] >= ai[m]) {
            return false;
        }

        const bool gradient = isGradientKind(paint.kind);
        const Mat2x3 inv = safeInverse(deviceXf);
        const Mat2x3* invp = gradient ? &inv : nullptr;
        float woff = 0.0f, alphaScale = 1.0f;
        if (fringe) {
            computeMiters(inner);
            const float t = ringMinWidth(inner, outer);
            if (t <= 1e-4f) {
                return true;  // a zero-width band draws nothing
            }
            fringeFor(t, woff, alphaScale);
        }
        uint32_t innerRgba = 0;
        DrawCmd cmd = directCmd(paint, alphaScale, innerRgba);

        // Layout: outer core [0,n), inner core [n,n+m), then outer fringe [n+m, 2n+m) and inner
        // fringe [2n+m, 2n+2m).
        const uint32_t nn = static_cast<uint32_t>(n), mm = static_cast<uint32_t>(m);
        Geometry g;
        g.vertexCount = fringe ? 2 * (nn + mm) : nn + mm;
        Vertex* v = emit.appendVertices(g.vertexCount, g.baseVertex);
        if (fringe) {
            writeOffsetVertices(outer, -woff, innerRgba, invp, v);
            writeOffsetVertices(inner, woff, innerRgba, invp, v + n);
            writeOffsetVertices(outer, 1.0f - woff, 0u, invp, v + n + m);
            writeOffsetVertices(inner, -(1.0f - woff), 0u, invp, v + 2 * n + m);
        } else {
            for (size_t i = 0; i < n + m; ++i) {
                const Vec2 p = i < n ? outer.pts[i] : inner.pts[i - n];
                v[i] = gradient ? paintVertex(p, inv, innerRgba) : Vertex{p.x, p.y, 1.0f, 0, 0, innerRgba};
            }
        }
        g.indexCount = (nn + mm) * 3 + (fringe ? (nn + mm) * 6 : 0);
        uint32_t* idx = emit.appendIndices(g.indexCount, g.firstIndex);
        size_t i = 0, j = 0;
        while (i < n || j < m) {
            const uint32_t oi = static_cast<uint32_t>(i % n);
            const uint32_t ij = nn + static_cast<uint32_t>((j0 + j) % m);
            if (i < n && (j >= m || ao[i + 1] <= ai[j + 1])) {
                *idx++ = oi;
                *idx++ = static_cast<uint32_t>((i + 1) % n);
                *idx++ = ij;
                ++i;
            } else {
                *idx++ = ij;
                *idx++ = nn + static_cast<uint32_t>((j0 + j + 1) % m);
                *idx++ = oi;
                ++j;
            }
        }
        if (fringe) {
            idx = writeRingQuads(idx, 0, nn + mm, nn);
            writeRingQuads(idx, nn, 2 * nn + mm, mm);
        }
        emit.pushCmd(cmd, g);
        return true;
    }

    /// Tries the direct paths for `pts`/`starts` (see the section comment); false means the
    /// caller must use stencil-then-cover.
    bool fillDirect(std::span<const Vec2> pts, std::span<const uint32_t> starts, const Mat2x3& deviceXf,
                    FillRule rule, const PaintRef& paint, bool fringe) {
        // Gather the non-degenerate contours.
        contourPool.clear();
        contourStarts.clear();
        contourBounds.clear();
        size_t count = 0;
        for (size_t k = 0; k < starts.size(); ++k) {
            const uint32_t start = starts[k];
            const uint32_t end = (k + 1 < starts.size()) ? starts[k + 1] : static_cast<uint32_t>(pts.size());
            if (end - start < 3) {
                continue;
            }
            if (++count > kMaxDirectContours) {
                return false;
            }
            if (!cleanConvexContour(pts.subspan(start, end - start), contourA)) {
                return false;
            }
            contourStarts.push_back(static_cast<uint32_t>(contourPool.size()));
            contourPool.insert(contourPool.end(), contourA.pts.begin(), contourA.pts.end());
            contourBounds.push_back(contourA.bounds);
        }
        if (count == 0) {
            return true;  // nothing to draw
        }
        auto loadContour = [&](size_t k, Contour& out) {
            const uint32_t start = contourStarts[k];
            const uint32_t end = (k + 1 < contourStarts.size()) ? contourStarts[k + 1]
                                                                : static_cast<uint32_t>(contourPool.size());
            out.pts.assign(contourPool.begin() + start, contourPool.begin() + end);
            out.bounds = contourBounds[k];
        };
        if (count == 1) {
            loadContour(0, contourA);
            fillConvex(contourA, deviceXf, paint, fringe);
            return true;
        }
        // Ring: two contours, one inside the other, and the rule makes the inner a hole.
        // (Both are stored with positive area, so NonZero's "opposite winding" is read off the
        // original orientation: a hole under NonZero needs the inner reversed, which the
        // painter's borders and strokeToFill's rings always are.)
        if (count == 2) {
            const bool outerFirst =
                contourBounds[0].w * contourBounds[0].h >= contourBounds[1].w * contourBounds[1].h;
            loadContour(outerFirst ? 0 : 1, contourA);
            loadContour(outerFirst ? 1 : 0, contourB);
            const Rect& ob = contourA.bounds;
            const Rect& ib = contourB.bounds;
            const bool boxInside =
                ib.x >= ob.x && ib.y >= ob.y && ib.x + ib.w <= ob.x + ob.w && ib.y + ib.h <= ob.y + ob.h;
            if (boxInside) {
                computeMiters(contourA);
                if (contourInside(contourB, contourA) &&
                    (rule == FillRule::EvenOdd || ringIsHoleUnderNonZero(pts, starts))) {
                    return fillRing(contourA, contourB, deviceXf, paint, fringe);
                }
                return false;
            }
        }
        // Disjoint convex pieces (a dashed stroke, a multi-rect icon): fans in one batch. The
        // boxes must not touch, fringe included, or SrcOver would blend the overlap twice.
        const float pad = fringe ? 1.0f : 0.0f;
        for (size_t a = 0; a < count; ++a) {
            for (size_t b = a + 1; b < count; ++b) {
                Rect ra = contourBounds[a];
                ra.x -= pad;
                ra.y -= pad;
                ra.w += 2 * pad;
                ra.h += 2 * pad;
                if (rectsOverlap(ra, contourBounds[b])) {
                    return false;
                }
            }
        }
        for (size_t k = 0; k < count; ++k) {
            loadContour(k, contourA);
            fillConvex(contourA, deviceXf, paint, fringe);
        }
        return true;
    }

    /// NonZero ring test: the two non-degenerate contours of `pts` wind in opposite directions
    /// (so the inner one cancels the outer's winding and is a hole).
    static bool ringIsHoleUnderNonZero(std::span<const Vec2> pts, std::span<const uint32_t> starts) {
        float signs[2] = {0, 0};
        int found = 0;
        for (size_t k = 0; k < starts.size() && found < 2; ++k) {
            const uint32_t start = starts[k];
            const uint32_t end = (k + 1 < starts.size()) ? starts[k + 1] : static_cast<uint32_t>(pts.size());
            if (end - start < 3) {
                continue;
            }
            float area = 0.0f;
            for (uint32_t i = start; i < end; ++i) {
                const Vec2 a = pts[i], b = pts[i + 1 < end ? i + 1 : start];
                area += a.x * b.y - b.x * a.y;
            }
            signs[found++] = area;
        }
        return found == 2 && signs[0] * signs[1] < 0.0f;
    }

    /// The scissor fast path's helper: cleans the single contour of `pts`/`starts` into
    /// `contourA` when it is convex.
    bool singleConvexContour(std::span<const Vec2> pts, std::span<const uint32_t> starts) {
        int contours = 0;
        uint32_t theStart = 0, theEnd = 0;
        for (size_t k = 0; k < starts.size(); ++k) {
            const uint32_t start = starts[k];
            const uint32_t end = (k + 1 < starts.size()) ? starts[k + 1] : static_cast<uint32_t>(pts.size());
            if (end - start >= 3) {
                ++contours;
                theStart = start;
                theEnd = end;
            }
        }
        return contours == 1 && cleanConvexContour(pts.subspan(theStart, theEnd - theStart), contourA);
    }

    // ---------------------------------------------------------------------------------------
    // FillPath / StrokePath.
    // ---------------------------------------------------------------------------------------

    void doFill(const FillCmd& cmd, float dpr) {
        Mat2x3 deviceXf = deviceTransform(cmd.xf, dpr);
        std::span<const float> rawPoints(list->points.data() + cmd.path.pointOffset, cmd.path.pointCount);
        float tol = projecting() ? flattenTolerance(deviceXf, rawPoints) : kFlattenTolerance;
        flatten(cmd.path, deviceXf, tol);
        const bool gradient = isGradientKind(cmd.paint.kind);
        const bool projGradient = projecting() && gradient && setupProjectedPaint(flatPoints);
        if (projecting() && !projectPointsInPlace(flatPoints)) {
            return;  // §19.7: the whole path is dropped.
        }
        projPaint = projGradient;
        fillShapeAndCover(flatPoints, flatStarts, deviceXf, cmd.rule, cmd.paint, cmd.antialias != 0);
        projPaint = false;
    }

    void doStroke(const StrokeCmd& cmd, float dpr) {
        if (cmd.paint.kind == PaintKind::None) {
            return;
        }
        Mat2x3 deviceXf = deviceTransform(cmd.xf, dpr);
        std::span<const float> rawPoints(list->points.data() + cmd.path.pointOffset, cmd.path.pointCount);
        float tol = projecting() ? flattenTolerance(deviceXf, rawPoints) : kFlattenTolerance;
        flatten(cmd.path, deviceXf, tol);

        float scale = std::sqrt(std::fabs(determinant(cmd.xf))) * dpr;

        const std::vector<Vec2>* srcPoints = &flatPoints;
        const std::vector<uint32_t>* srcStarts = &flatStarts;
        const std::vector<uint8_t>* srcClosed = &flatClosed;

        if (cmd.dashCount > 0) {
            scaledDashes.clear();
            for (uint32_t i = 0; i < cmd.dashCount; ++i) {
                scaledDashes.push_back(list->dashes[cmd.dashOffset + i] * scale);
            }
            float scaledPhase = cmd.dashPhase * scale;
            dashPoints.clear();
            dashStarts.clear();
            dashClosed.clear();
            applyDash(flatPoints, flatStarts, flatClosed, scaledDashes, scaledPhase, dashPoints, dashStarts,
                      dashClosed);
            srcPoints = &dashPoints;
            srcStarts = &dashStarts;
            srcClosed = &dashClosed;
        }

        strokeOutPoints.clear();
        strokeOutStarts.clear();
        strokeToFill(*srcPoints, *srcStarts, *srcClosed, cmd.width * scale, cmd.cap, cmd.join, cmd.miterLimit,
                     tol, strokeOutPoints, strokeOutStarts);
        const bool gradient = isGradientKind(cmd.paint.kind);
        const bool projGradient = projecting() && gradient && setupProjectedPaint(strokeOutPoints);
        if (projecting() && !projectPointsInPlace(strokeOutPoints)) {
            return;
        }
        projPaint = projGradient;
        fillShapeAndCover(strokeOutPoints, strokeOutStarts, deviceXf, FillRule::NonZero, cmd.paint);
        projPaint = false;
    }

    // ---------------------------------------------------------------------------------------
    // Images.
    // ---------------------------------------------------------------------------------------

    TextureHandle getOrCreateTexture(AssetId asset, AssetResolver* assets, const IrView* ir, bool& straight,
                                     float variantScale) {
        // T-345 (25 §25.4.2 item 4): the drawing element's `ui-scale` factor x dpr picks the
        // variant (`@3x` at >= 2.5, `@2x` at >= 1.5), as AssetResolver::imageView(dpr) does.
        const float scale = currentDpr * variantScale;
        const uint8_t variant = scale >= 2.5f ? 2 : scale >= 1.5f ? 1 : 0;
        straight = false;
        // T-263c ruling 3: an id means something under its own IR and resolver only.
        uint8_t retries = 0;
        for (size_t i = 0; i < textureCache.size(); ++i) {
            const TextureEntry& entry = textureCache[i];
            if (entry.source != currentSource || entry.asset != asset || entry.variant != variant) {
                continue;
            }
            if (entry.tex != 0 || entry.refused) {
                straight = entry.straight;
                return entry.tex;  // 0 for an image over the device's limit: reported once, below
            }
            // Docs/tasks/T-263d.md part two: an upload that failed (E10: remembered below) is
            // tried again kUploadRetryBuilds host frames later (T-263f: frames, see
            // `frameCount`), kUploadRetries times at most; in between, and after the last try,
            // the image is left out and the failure reported.
            if (!retryDue(entry)) {
                lastError = "createTexture failed for an image";
                return 0;
            }
            retries = static_cast<uint8_t>(entry.retries + 1);
            textureCache.erase(textureCache.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
        auto remember = [&](const void* shared, TextureHandle tex, bool isStraight, int width, int height) {
            TextureEntry entry;
            entry.asset = asset;
            entry.tex = tex;
            entry.variant = variant;
            entry.straight = isStraight;
            entry.shared = shared;
            entry.owned = true;
            entry.epoch = assetEpoch;
            entry.source = currentSource;
            entry.width = width;
            entry.height = height;
            for (const TextureEntry& other : textureCache) {
                if (other.tex == tex && other.source == currentSource) {
                    entry.owned = false;
                    break;
                }
            }
            textureCache.push_back(entry);
            straight = isStraight;
            return tex;
        };
        // Docs/tasks/T-263c.md ruling 1: an image address alone does not make two assets one
        // texture -- AssetResolver's default imageView() hands every asset the same scratch
        // image. A texture is shared only by the cells of one atlas page: both assets are atlas
        // assets of the same page by their names in the IR (`atlas:<n>#<asset>`), and the image
        // is the one that was uploaded (same address, same size, same variant, same source).
        const std::string_view page = atlasPageOf(asset, *ir);
        auto sharedTexture = [&](const void* shared, int width, int height,
                                 bool& isStraight) -> TextureHandle {
            if (shared == nullptr || page.empty()) {
                return 0;
            }
            for (const TextureEntry& entry : textureCache) {
                if (entry.shared == shared && entry.tex != 0 && entry.epoch == assetEpoch &&
                    entry.source == currentSource && entry.variant == variant && entry.width == width &&
                    entry.height == height && atlasPageOf(entry.asset, *ir) == page) {
                    isStraight = entry.straight;
                    return entry.tex;
                }
            }
            return 0;
        };
        // T-199 (23 §23.8): a block-compressed asset is uploaded as it is (straight alpha); a
        // device that cannot take the format returns 0 and the decoded pixels are used instead.
        if (const CompressedImage* compressed = assets->compressedImage(asset, *ir, scale)) {
            bool isStraight = true;
            if (TextureHandle shared =
                    sharedTexture(compressed, compressed->width, compressed->height, isStraight)) {
                return remember(compressed, shared, isStraight, compressed->width, compressed->height);
            }
            TextureFormat fmt = TextureFormat::Bc7;
            if (compressed->format == DdsFormat::Bc1) {
                fmt = TextureFormat::Bc1;
            } else if (compressed->format == DdsFormat::Bc3) {
                fmt = TextureFormat::Bc3;
            }
            if (compressed->width > 0 && compressed->height > 0 && compressed->width <= 65535 &&
                compressed->height <= 65535 && !overTextureLimit(compressed->width, compressed->height) &&
                compressed->blocks.size() >=
                    ddsLevelSize(compressed->format, compressed->width, compressed->height)) {
                TextureHandle tex = device->createTexture(
                    "xt image (bc)", static_cast<uint16_t>(compressed->width),
                    static_cast<uint16_t>(compressed->height), fmt, compressed->blocks.data());
                if (tex != 0) {
                    return remember(compressed, tex, true, compressed->width, compressed->height);
                }
            }
        }
        // T-148: borrow the resolver's decoded image instead of copying it (T-199: the `@2x`
        // variant at dpr >= 1.5; an atlas page is one image shared by its assets).
        const ImageData* view = assets->imageView(asset, *ir, scale);
        if (view == nullptr) {
            lastError = "image decode failed";
            return 0;
        }
        {
            bool isStraight = false;
            if (TextureHandle shared = sharedTexture(view, view->width, view->height, isStraight)) {
                return remember(view, shared, isStraight, view->width, view->height);
            }
        }
        const ImageData& image = *view;
        if (image.width <= 0 || image.height <= 0 || image.width > 65535 || image.height > 65535 ||
            image.rgba.size() < static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 4) {
            lastError = "image has an unsupported size";
            return 0;
        }
        // Docs/tasks/T-263d.md part two: larger than the device says it can create. Not handed
        // to createTexture at all; remembered with texture 0 until the asset is invalidated, and
        // reported this once.
        if (overTextureLimit(image.width, image.height)) {
            lastError = "image " + std::to_string(image.width) + "x" + std::to_string(image.height) +
                        " exceeds the device texture limit " + std::to_string(maxTexture);
            TextureEntry refused;
            refused.asset = asset;
            refused.variant = variant;
            refused.owned = false;
            refused.epoch = assetEpoch;
            refused.source = currentSource;
            refused.refused = true;
            textureCache.push_back(refused);
            return 0;
        }
        // Premultiply on the CPU (gpu_frame.h: every texture is premultiplied).
        const size_t count = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
        premultiplyScratch.resize(count * 4);
        for (size_t i = 0; i < count; ++i) {
            const uint8_t* s = image.rgba.data() + i * 4;
            uint8_t* d = premultiplyScratch.data() + i * 4;
            const uint32_t a = s[3];
            d[0] = static_cast<uint8_t>((s[0] * a + 127u) / 255u);
            d[1] = static_cast<uint8_t>((s[1] * a + 127u) / 255u);
            d[2] = static_cast<uint8_t>((s[2] * a + 127u) / 255u);
            d[3] = s[3];
        }
        TextureHandle tex = device->createTexture("xt image", static_cast<uint16_t>(image.width),
                                                  static_cast<uint16_t>(image.height), TextureFormat::Rgba8,
                                                  premultiplyScratch.data());
        if (tex == 0) {
            // Docs/tasks/T-263b.md E10: the device could not take this image (the upload failed,
            // or it is larger than a limit the device does not report). Remembered with texture
            // 0 -- the image is not drawn -- so the following frames do not premultiply and
            // upload it again; T-263d: it is tried again later (see the lookup above), and an
            // invalidateAsset()/invalidateAllAssets() forgets the failure altogether.
            lastError = "createTexture failed for an image";
            TextureEntry failed;
            failed.asset = asset;
            failed.variant = variant;
            failed.owned = false;
            failed.epoch = assetEpoch;
            failed.source = currentSource;
            failed.retries = retries;
            failed.failedAt = frameCount;
            textureCache.push_back(failed);
            return 0;
        }
        return remember(view, tex, false, image.width, image.height);
    }

    /// T-150: resolves the texture a DrawImage/DrawImageQuad samples. An `engine://` asset takes
    /// the host's binding (`AssetResolver::setExternalTexture`); unbound, or bound with texture 0,
    /// it draws nothing (0 out, no error: "not bound yet" is a normal host state). Any other
    /// asset goes through the decode-and-upload cache.
    TextureHandle resolveImageTexture(AssetId asset, AssetResolver* assets, const IrView* ir,
                                      Shading& shading, float variantScale = 1.0f) {
        shading = Shading::Texture;
        if (isExternalAsset(asset, *ir)) {
            const AssetResolver::ExternalTexture* ext = assets->externalTexture(asset, *ir);
            rememberExternal(asset, ext);
            if (ext == nullptr || ext->texture == 0) {
                return 0;
            }
            if (ext->straightAlpha) {
                shading = Shading::TextureStraight;
            }
            return ext->texture;
        }
        bool straight = false;
        TextureHandle tex = getOrCreateTexture(asset, assets, ir, straight, variantScale);
        if (straight) {
            shading = Shading::TextureStraight;  // T-199: compressed blocks keep straight alpha.
        }
        return tex;
    }

    /// T-162: records what the frame saw of an external binding (a missing one as texture 0), so
    /// lastFrameReusable notices a rebind.
    void rememberExternal(AssetId asset, const AssetResolver::ExternalTexture* ext) {
        for (const ExternalRef& r : lastExternals) {
            if (r.asset == asset && r.source == currentSource) {
                return;
            }
        }
        ExternalRef r;
        r.asset = asset;
        r.source = currentSource;
        r.texture = ext != nullptr ? ext->texture : 0;
        r.straightAlpha = ext != nullptr && ext->straightAlpha;
        lastExternals.push_back(r);
    }

    void drawHomogeneousQuad(const Vertex v[4], TextureHandle tex, bool smooth,
                             Shading shading = Shading::Texture) {
        if (emit.scissor().empty()) {
            return;
        }
        DrawCmd c;
        c.shading = shading;
        c.texture = tex;
        c.sampler = smooth ? Sampler::LinearClamp : Sampler::PointClamp;
        c.blend = Blend::SrcOver;
        c.stencilOp = StencilOp::TestEqualRef;
        c.stencilRef = static_cast<uint8_t>(clipDepth);
        emit.pushCmd(c, emit.quad(v));
    }

    void doImage(const ImageCmd& cmd, float dpr, AssetResolver* assets, const IrView* ir) {
        if (assets == nullptr || ir == nullptr) {
            return;
        }
        Shading shading = Shading::Texture;
        TextureHandle tex = resolveImageTexture(cmd.asset, assets, ir, shading, cmd.variantScale);
        if (tex == 0) {
            return;
        }
        Mat2x3 deviceXf = deviceTransform(cmd.xf, dpr);
        Vec2 dstCorners[4] = {
            apply(deviceXf, Vec2{cmd.dst.x, cmd.dst.y}),
            apply(deviceXf, Vec2{cmd.dst.x + cmd.dst.w, cmd.dst.y}),
            apply(deviceXf, Vec2{cmd.dst.x + cmd.dst.w, cmd.dst.y + cmd.dst.h}),
            apply(deviceXf, Vec2{cmd.dst.x, cmd.dst.y + cmd.dst.h}),
        };
        Vec2 uv[4] = {
            Vec2{cmd.srcUv.x, cmd.srcUv.y},
            Vec2{cmd.srcUv.x + cmd.srcUv.w, cmd.srcUv.y},
            Vec2{cmd.srcUv.x + cmd.srcUv.w, cmd.srcUv.y + cmd.srcUv.h},
            Vec2{cmd.srcUv.x, cmd.srcUv.y + cmd.srcUv.h},
        };
        const uint32_t rgba = packPremultiplied(Color{1, 1, 1, cmd.opacity});
        Vertex v[4];
        for (int i = 0; i < 4; ++i) {
            bool ok = false;
            HomoPoint hp = homogeneousVertexPos(dstCorners[i], ok);
            if (!ok) {
                return;  // §19.7: dropped.
            }
            v[i] = Vertex{hp.x, hp.y, hp.w, uv[i].x, uv[i].y, rgba};
        }
        drawHomogeneousQuad(v, tex, cmd.smooth != 0, shading);
    }

    /// T-202 (Docs/tasks/T-202.md item 5): one instanced `DrawKind::Particles` command; the
    /// instances are copied into `GpuFrame::particleInstances` (colors packed, already
    /// premultiplied). Nothing is drawn under a 3D projection or when the texture is not ready.
    void doParticles(const CommandList& cl, const ParticlesCmd& cmd, float dpr, AssetResolver* assets,
                     const IrView* ir) {
        if (projecting() || cmd.count == 0 || emit.scissor().empty() ||
            static_cast<size_t>(cmd.instanceOffset) + cmd.count > cl.particleInstances.size()) {
            return;
        }
        Shading shading = Shading::ParticleSolid;
        TextureHandle tex = 0;
        ParticleParams pp{};
        if (cmd.texture != kInvalidAsset) {
            if (assets == nullptr || ir == nullptr) {
                return;
            }
            Shading imageShading = Shading::Texture;
            tex = resolveImageTexture(cmd.texture, assets, ir, imageShading);
            if (tex == 0) {
                return;
            }
            shading = Shading::ParticleTexture;
            pp.straight = imageShading == Shading::TextureStraight ? 1.0f : 0.0f;
        }
        const Mat2x3 x = deviceTransform(cmd.xf, dpr);
        pp.xf[0] = x.a;
        pp.xf[1] = x.b;
        pp.xf[2] = x.c;
        pp.xf[3] = x.d;
        pp.xf[4] = x.e;
        pp.xf[5] = x.f;
        GpuFrame& frame = emit.frame();
        DrawCmd c;
        c.kind = DrawKind::Particles;
        c.shading = shading;
        c.texture = tex;
        c.sampler = Sampler::LinearClamp;
        c.blend = cmd.blend == ParticleBlend::Additive ? Blend::Additive : Blend::SrcOver;
        c.stencilOp = StencilOp::TestEqualRef;
        c.stencilRef = static_cast<uint8_t>(clipDepth);
        c.paramIndex = emit.pushParams(pp);
        c.firstInstance = static_cast<uint32_t>(frame.particleInstances.size());
        auto channel = [](float v) {
            return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)) & 0xFFu;
        };
        for (uint32_t i = 0; i < cmd.count; ++i) {
            const xt::ParticleInstance& src = cl.particleInstances[cmd.instanceOffset + i];
            gpu::ParticleInstance p{};
            p.x = src.x;
            p.y = src.y;
            p.w = src.w;
            p.h = src.h;
            p.rot = src.rot;
            p.rgba = channel(src.rgba.r) | (channel(src.rgba.g) << 8) | (channel(src.rgba.b) << 16) |
                     (channel(src.rgba.a) << 24);
            p.u0 = src.uv0.x;
            p.v0 = src.uv0.y;
            p.u1 = src.uv1.x;
            p.v1 = src.uv1.y;
            frame.particleInstances.push_back(p);
        }
        c.instanceCount = cmd.count;
        emit.pushCmd(c, Geometry{});
    }

    void doImageQuad(const ImageQuadCmd& cmd, float dpr, AssetResolver* assets, const IrView* ir) {
        if (assets == nullptr || ir == nullptr) {
            return;
        }
        Shading shading = Shading::Texture;
        TextureHandle tex = resolveImageTexture(cmd.asset, assets, ir, shading, cmd.variantScale);
        if (tex == 0) {
            return;
        }
        Mat2x3 dprXf = scaling(Vec2{dpr, dpr});
        Vec2 uv[4] = {
            Vec2{cmd.srcUv.x, cmd.srcUv.y},
            Vec2{cmd.srcUv.x + cmd.srcUv.w, cmd.srcUv.y},
            Vec2{cmd.srcUv.x + cmd.srcUv.w, cmd.srcUv.y + cmd.srcUv.h},
            Vec2{cmd.srcUv.x, cmd.srcUv.y + cmd.srcUv.h},
        };
        const uint32_t rgba = packPremultiplied(Color{1, 1, 1, cmd.opacity});
        Vertex v[4];
        for (int i = 0; i < 4; ++i) {
            Vec2 plane = apply(dprXf, cmd.p[i]);
            bool ok = false;
            HomoPoint hp = homogeneousVertexPos(plane, ok);
            if (!ok) {
                return;
            }
            v[i] = Vertex{hp.x, hp.y, hp.w, uv[i].x, uv[i].y, rgba};
        }
        drawHomogeneousQuad(v, tex, cmd.smooth != 0, shading);
    }

    /// T-245 (Docs/design/32-rigging-and-deformation.md §32.2.4): the mesh's vertices go through
    /// the current mapping like DrawImageQuad's corners and straight into a `DrawKind::Triangles`
    /// command (rgba = the opacity as premultiplied white, indices rebased onto the frame's
    /// vertices); the emitter folds it into an adjacent image draw with the same state.
    void doImageMesh(const CommandList& cl, const ImageMeshCmd& cmd, float dpr, AssetResolver* assets,
                     const IrView* ir) {
        if (assets == nullptr || ir == nullptr || cmd.indexCount < 3 || cmd.vertexCount == 0 ||
            emit.scissor().empty() ||
            static_cast<size_t>(cmd.vertexOffset) + cmd.vertexCount > cl.meshVertices.size() ||
            static_cast<size_t>(cmd.indexOffset) + cmd.indexCount > cl.meshIndices.size()) {
            return;
        }
        const uint32_t triIndices = cmd.indexCount / 3 * 3;
        for (uint32_t i = 0; i < triIndices; ++i) {
            if (cl.meshIndices[cmd.indexOffset + i] >= cmd.vertexCount) {
                return;  // Out-of-range index (never produced by the Painter): dropped.
            }
        }
        Shading shading = Shading::Texture;
        TextureHandle tex = resolveImageTexture(cmd.asset, assets, ir, shading);
        if (tex == 0) {
            return;
        }
        const Mat2x3 dprXf = scaling(Vec2{dpr, dpr});
        const uint32_t rgba = packPremultiplied(Color{1, 1, 1, cmd.opacity});
        meshVertexScratch.resize(cmd.vertexCount);
        for (uint32_t i = 0; i < cmd.vertexCount; ++i) {
            const MeshVertex& src = cl.meshVertices[cmd.vertexOffset + i];
            bool ok = false;
            HomoPoint hp = homogeneousVertexPos(apply(dprXf, src.p), ok);
            if (!ok) {
                return;  // §19.7: dropped.
            }
            meshVertexScratch[i] = Vertex{hp.x, hp.y, hp.w, src.uv.x, src.uv.y, rgba};
        }
        Geometry g;
        Vertex* outVerts = emit.appendVertices(cmd.vertexCount, g.baseVertex);
        std::copy(meshVertexScratch.begin(), meshVertexScratch.end(), outVerts);
        g.vertexCount = cmd.vertexCount;
        uint32_t* idx = emit.appendIndices(triIndices, g.firstIndex);
        for (uint32_t i = 0; i < triIndices; ++i) {
            idx[i] = cl.meshIndices[cmd.indexOffset + i];
        }
        g.indexCount = triIndices;
        DrawCmd c;
        c.shading = shading;
        c.texture = tex;
        c.sampler = cmd.smooth != 0 ? Sampler::LinearClamp : Sampler::PointClamp;
        c.blend = Blend::SrcOver;
        c.stencilOp = StencilOp::TestEqualRef;
        c.stencilRef = static_cast<uint8_t>(clipDepth);
        emit.pushCmd(c, g);
    }

    // ---------------------------------------------------------------------------------------
    // DrawGlyphs (Docs/design/10-backends.md §10.3.4).
    // ---------------------------------------------------------------------------------------

    void doOutlineGlyph(FontId font, uint32_t glyphId, Vec2 glyphPos, float size, const Mat2x3& xf, float dpr,
                        Color color, TextShaper* shaper) {
        if (shaper == nullptr) {
            return;
        }
        glyphOutlineVerbs.clear();
        glyphOutlinePoints.clear();
        if (!shaper->glyphOutline(font, glyphId, glyphOutlineVerbs, glyphOutlinePoints)) {
            return;
        }
        Mat2x3 local = mul(translation(glyphPos), scaling(Vec2{size, size}));
        Mat2x3 deviceXf = mul(deviceTransform(xf, dpr), local);
        flatPoints.clear();
        flatStarts.clear();
        flatClosed.clear();
        flattenPath(glyphOutlineVerbs, glyphOutlinePoints, deviceXf, kFlattenTolerance, flatPoints,
                    flatStarts, flatClosed);
        if (projecting() && !projectPointsInPlace(flatPoints)) {
            return;
        }
        PaintRef paint{};
        paint.kind = PaintKind::Solid;
        paint.color = color;
        fillShapeAndCover(flatPoints, flatStarts, deviceXf, FillRule::NonZero, paint);
    }

#ifndef XT_GPU_NO_TEXT
    TextureHandle ensureAtlasTexture(uint16_t index) {
        TextureHandle& tex = atlasTextures[index];
        if (tex != 0) {
            return tex;
        }
        const int size = glyphAtlas->textureSize();
        tex = device->createTexture("xt glyph atlas", static_cast<uint16_t>(std::clamp(size, 0, 65535)),
                                    static_cast<uint16_t>(std::clamp(size, 0, 65535)), TextureFormat::R8,
                                    nullptr);
        atlasTextureSize = size;
        return tex;
    }

    void uploadPendingAtlas() {
        glyphAtlas->flushUploads(
            [this](uint16_t texIndex, int x, int y, int w, int h, const uint8_t* pixels, int stride) {
                if (texIndex >= 4 || w <= 0 || h <= 0) {
                    return;
                }
                TextureHandle tex = ensureAtlasTexture(texIndex);
                if (tex == 0) {
                    return;
                }
                const uint8_t* region =
                    pixels + static_cast<size_t>(y) * static_cast<size_t>(stride) + static_cast<size_t>(x);
                device->updateTexture(tex, static_cast<uint16_t>(x), static_cast<uint16_t>(y),
                                      static_cast<uint16_t>(w), static_cast<uint16_t>(h), region,
                                      static_cast<size_t>(stride));
            });
    }

    uint32_t glyphParamIndex(float gamma) {
        for (int i = 0; i < glyphParamCount; ++i) {
            if (glyphGamma[i] == gamma) {
                return glyphParam[i];
            }
        }
        GlyphParams params{};
        params.gamma = gamma;
        const uint32_t index = emit.pushParams(params);
        if (glyphParamCount < 2) {
            glyphGamma[glyphParamCount] = gamma;
            glyphParam[glyphParamCount] = index;
            ++glyphParamCount;
        }
        return index;
    }

    void flushGlyphBatch(float gamma) {
        uint32_t paramIndex = 0;
        bool haveParams = false;
        for (uint16_t t = 0; t < 4; ++t) {
            std::vector<Vertex>& verts = glyphBatch[t];
            if (verts.empty()) {
                continue;
            }
            if (!haveParams) {
                paramIndex = glyphParamIndex(gamma);
                haveParams = true;
            }
            Geometry g;
            Vertex* out = emit.appendVertices(static_cast<uint32_t>(verts.size()), g.baseVertex);
            std::copy(verts.begin(), verts.end(), out);
            g.vertexCount = static_cast<uint32_t>(verts.size());
            const uint32_t quads = g.vertexCount / 4;
            uint32_t* idx = emit.appendIndices(quads * 6, g.firstIndex);
            for (uint32_t q = 0; q < quads; ++q) {
                idx[q * 6 + 0] = q * 4 + 0;
                idx[q * 6 + 1] = q * 4 + 1;
                idx[q * 6 + 2] = q * 4 + 2;
                idx[q * 6 + 3] = q * 4 + 0;
                idx[q * 6 + 4] = q * 4 + 2;
                idx[q * 6 + 5] = q * 4 + 3;
            }
            g.indexCount = quads * 6;
            DrawCmd c;
            c.shading = Shading::Glyph;
            c.texture = ensureAtlasTexture(t);
            c.sampler = Sampler::LinearClamp;
            c.blend = Blend::SrcOver;
            c.stencilOp = StencilOp::TestEqualRef;
            c.stencilRef = static_cast<uint8_t>(clipDepth);
            c.paramIndex = paramIndex;
            emit.pushCmd(c, g);
            verts.clear();
        }
    }
#endif

#ifndef XT_GPU_NO_TEXT
    /// T-178: a glyph-level text effect (GlyphsCmd::effect), one quad per glyph -- the glyph's
    /// atlas quad outset by the kernel radius -- each with its own GlyphParams (its atlas
    /// rectangle), so these quads never batch. Glyphs missing from the atlas are skipped (an
    /// outline has no coverage to dilate or blur).
    void doEffectGlyphs(const GlyphsCmd& cmd, const Mat2x3& deviceXf, float dpr) {
        if (emit.scissor().empty() || cmd.color.a <= 0.0f) {
            return;
        }
        const float sizeDevice = glyphRunBitmapSize(cmd, deviceXf);
        const float texSize = static_cast<float>(glyphAtlas->textureSize());
        const float radius = std::min(cmd.effectRadius * std::fabs(cmd.xf.a) * dpr, 8.0f);
        const bool dilate = cmd.effect == GlyphEffect::Dilate;
        int k = 0;
        if (dilate) {
            k = radius > 0.0f ? static_cast<int>(std::ceil(radius)) : 0;
        } else if (radius > 0.0f) {
            k = static_cast<int>(std::ceil(3.0f * radius * 0.5f));
        }
        const float pad = static_cast<float>(k);
        const uint32_t rgba = packPremultiplied(cmd.color);
        for (uint32_t i = 0; i < cmd.glyphCount; ++i) {
            const uint32_t glyphId = list->glyphs[cmd.glyphOffset + i];
            const Vec2 glyphPos = list->glyphPos[cmd.glyphOffset + i];
            const Vec2 posWorld = apply(cmd.xf, glyphPos);
            float wholeX = std::floor(posWorld.x * dpr);
            int phase = static_cast<int>(std::lround((posWorld.x * dpr - wholeX) * 4.0f));
            if (phase >= 4) {
                phase = 0;
                wholeX += 1.0f;
            }
            xt::text::AtlasEntry entry{};
            const bool found =
                glyphAtlas->lookup(cmd.font, glyphId, sizeDevice, static_cast<float>(phase) * 0.25f, entry);
            if (glyphAtlas->wasReset()) {
                atlasResetSeen = true;
            }
            if (!found || entry.w == 0 || entry.h == 0 || entry.texture >= 4) {
                continue;
            }
            uploadPendingAtlas();
            const float x = wholeX + static_cast<float>(entry.bearingX) - pad;
            const float y = std::round(posWorld.y * dpr) - static_cast<float>(entry.bearingY) - pad;
            const float w = static_cast<float>(entry.w) + 2.0f * pad;
            const float h = static_cast<float>(entry.h) + 2.0f * pad;
            const float u0 = (static_cast<float>(entry.x) - pad) / texSize;
            const float v0 = (static_cast<float>(entry.y) - pad) / texSize;
            const float u1 = (static_cast<float>(entry.x + entry.w) + pad) / texSize;
            const float v1 = (static_cast<float>(entry.y + entry.h) + pad) / texSize;
            const Vec2 corners[4] = {Vec2{x, y}, Vec2{x + w, y}, Vec2{x + w, y + h}, Vec2{x, y + h}};
            const Vec2 uvs[4] = {Vec2{u0, v0}, Vec2{u1, v0}, Vec2{u1, v1}, Vec2{u0, v1}};
            Vertex quad[4];
            bool allOk = true;
            for (int c = 0; c < 4; ++c) {
                bool ok = false;
                HomoPoint hp = homogeneousVertexPos(corners[c], ok);
                if (!ok) {
                    allOk = false;
                    break;
                }
                quad[c] = Vertex{hp.x, hp.y, hp.w, uvs[c].x, uvs[c].y, rgba};
            }
            if (!allOk) {
                continue;
            }
            GlyphParams params{};
            params.gamma = 1.0f;
            params.effect = static_cast<float>(static_cast<uint8_t>(cmd.effect));
            params.radius = radius;
            params.rectX0 = static_cast<float>(entry.x);
            params.rectY0 = static_cast<float>(entry.y);
            params.rectX1 = static_cast<float>(entry.x + entry.w);
            params.rectY1 = static_cast<float>(entry.y + entry.h);
            params.texSize = texSize;
            Geometry g;
            Vertex* out = emit.appendVertices(4, g.baseVertex);
            std::copy(quad, quad + 4, out);
            g.vertexCount = 4;
            uint32_t* idx = emit.appendIndices(6, g.firstIndex);
            idx[0] = 0;
            idx[1] = 1;
            idx[2] = 2;
            idx[3] = 0;
            idx[4] = 2;
            idx[5] = 3;
            g.indexCount = 6;
            DrawCmd c;
            c.shading = Shading::Glyph;
            c.texture = ensureAtlasTexture(entry.texture);
            c.sampler = Sampler::LinearClamp;  // Unused: the effect shader uses texelFetch.
            c.blend = Blend::SrcOver;
            c.stencilOp = StencilOp::TestEqualRef;
            c.stencilRef = static_cast<uint8_t>(clipDepth);
            c.paramIndex = emit.pushParams(params);
            emit.pushCmd(c, g);
        }
    }
#endif

#ifndef XT_GPU_NO_TEXT
    /// Docs/tasks/T-263e.md ruling D: a glyph effect has no form but the atlas coverage (T-263c
    /// exempted effect runs from kMaxAtlasGlyphPx and from the scale rule for that reason), so
    /// what one effect run may occupy in the atlas is bounded instead: a quarter of one page.
    /// Measured before anything is rasterized -- a lookup would already put the glyph in the
    /// atlas and could evict the others -- as one em square of the run's bitmap size
    /// (ceil(size) x ceil(size) device px) per distinct atlas entry of the run, i.e. per distinct
    /// (glyph id, subpixel phase). False: the run is over the bound.
    bool effectRunFitsAtlas(const GlyphsCmd& cmd, const Mat2x3& deviceXf, float dpr) {
        const double cell = std::ceil(static_cast<double>(glyphRunBitmapSize(cmd, deviceXf)));
        const double page = static_cast<double>(glyphAtlas->textureSize());
        const double bound = page * page * kEffectRunAtlasShare;
        const double cellArea = cell * cell;
        if (static_cast<double>(cmd.glyphCount) * cellArea <= bound) {
            return true;  // Fits even if every glyph were an entry of its own.
        }
        effectRunKeys.clear();
        for (uint32_t i = 0; i < cmd.glyphCount; ++i) {
            // The entry doEffectGlyphs() would look up: the glyph at its quarter-pixel phase.
            const Vec2 posWorld = apply(cmd.xf, list->glyphPos[cmd.glyphOffset + i]);
            const float wholeX = std::floor(posWorld.x * dpr);
            const int phase = static_cast<int>(std::lround((posWorld.x * dpr - wholeX) * 4.0f)) & 3;
            effectRunKeys.push_back((static_cast<uint64_t>(list->glyphs[cmd.glyphOffset + i]) << 2) |
                                    static_cast<uint64_t>(phase));
        }
        std::sort(effectRunKeys.begin(), effectRunKeys.end());
        const size_t distinct = static_cast<size_t>(std::unique(effectRunKeys.begin(), effectRunKeys.end()) -
                                                    effectRunKeys.begin());
        return static_cast<double>(distinct) * cellArea <= bound;
    }
#endif

#ifndef XT_GPU_NO_TEXT
    /// T-263c ruling 4: records the bitmap size of the run `cmd` for the next build and says
    /// whether the previous build drew the same run (see RunSize) at another size only. A run the
    /// previous build did not draw has nothing to compare with and is not "being scaled".
    bool runIsBeingScaled(const GlyphsCmd& cmd, float size) {
        uint64_t key = fnv1a(kFnvBasis, &cmd.font, sizeof(cmd.font));
        key = fnv1a(key, list->glyphs.data() + cmd.glyphOffset, cmd.glyphCount * sizeof(uint32_t));
        key = fnv1a(key, list->glyphPos.data() + cmd.glyphOffset, cmd.glyphCount * sizeof(Vec2));
        curRunSizes.push_back(RunSize{key, size});
        auto it = std::lower_bound(prevRunSizes.begin(), prevRunSizes.end(), key,
                                   [](const RunSize& r, uint64_t k) { return r.key < k; });
        if (it == prevRunSizes.end() || it->key != key) {
            return false;
        }
        for (; it != prevRunSizes.end() && it->key == key; ++it) {
            if (it->size == size) {
                return false;
            }
        }
        return true;
    }
#endif

    void doGlyphs(const GlyphsCmd& cmd, float dpr, TextShaper* shaper) {
        if (cmd.glyphCount == 0) {
            return;
        }
        bool useAtlas = false;
#ifndef XT_GPU_NO_TEXT
        // Docs/tasks/T-263b.md E3: the same test and the same bitmap size as the software
        // renderer's bitmap path (xt/command_list.h).
        const Mat2x3 deviceXf = mul(scaling(Vec2{dpr, dpr}), cmd.xf);
        useAtlas = glyphAtlas != nullptr && atlasAllowed && glyphRunBitmapApplies(cmd, deviceXf);
        // Docs/tasks/T-263c.md ruling 4, for plain runs (a glyph effect works on the atlas
        // coverage and has no outline form): a run larger than kMaxAtlasGlyphPx, or one whose
        // size differs from the previous build's (a scale animation, which would rasterize
        // every 1/4 px size it passes through), is filled from outlines instead.
        if (useAtlas && cmd.effect == GlyphEffect::None) {
            const float size = glyphRunBitmapSize(cmd, deviceXf);
            if (size > kMaxAtlasGlyphPx) {
                useAtlas = false;
            } else if (runIsBeingScaled(cmd, size)) {
                useAtlas = false;
                ++scalingRuns;
            }
        }
#endif
        if (cmd.effect != GlyphEffect::None) {
            // T-178: effects need the atlas coverage (SW draws the same subset of runs).
#ifndef XT_GPU_NO_TEXT
            if (useAtlas && !projecting()) {
                if (effectRunFitsAtlas(cmd, deviceXf, dpr)) {
                    doEffectGlyphs(cmd, deviceXf, dpr);
                } else {
                    // Docs/tasks/T-263e.md ruling D: over the bound. Drawn this frame from the
                    // outlines, without the effect; no glyph of it is looked up, so nothing is
                    // rasterized into the atlas and no other glyph is evicted. Reported once per
                    // build.
                    if (!effectRunReported) {
                        effectRunReported = true;
                        lastError = "glyph effect run too large for the atlas; drawn without its effect";
                    }
                    for (uint32_t i = 0; i < cmd.glyphCount; ++i) {
                        doOutlineGlyph(cmd.font, list->glyphs[cmd.glyphOffset + i],
                                       list->glyphPos[cmd.glyphOffset + i], cmd.size, cmd.xf, dpr, cmd.color,
                                       shaper);
                    }
                }
            }
#endif
            return;
        }
        if (!useAtlas) {
            for (uint32_t i = 0; i < cmd.glyphCount; ++i) {
                uint32_t glyphId = list->glyphs[cmd.glyphOffset + i];
                Vec2 glyphPos = list->glyphPos[cmd.glyphOffset + i];
                doOutlineGlyph(cmd.font, glyphId, glyphPos, cmd.size, cmd.xf, dpr, cmd.color, shaper);
            }
            return;
        }
#ifndef XT_GPU_NO_TEXT
        if (emit.scissor().empty()) {
            return;
        }
        const float sizeDevice = glyphRunBitmapSize(cmd, deviceXf);
        float texSize = static_cast<float>(glyphAtlas->textureSize());
        const float gamma = coverageGamma(cmd.color);
        const uint32_t rgba = packPremultiplied(cmd.color);
        for (auto& b : glyphBatch) {
            b.clear();
        }

        for (uint32_t i = 0; i < cmd.glyphCount; ++i) {
            uint32_t glyphId = list->glyphs[cmd.glyphOffset + i];
            Vec2 glyphPos = list->glyphPos[cmd.glyphOffset + i];

            const Vec2 posWorld = apply(cmd.xf, glyphPos);
            float wholeX = std::floor(posWorld.x * dpr);
            int phase = static_cast<int>(std::lround((posWorld.x * dpr - wholeX) * 4.0f));
            if (phase >= 4) {
                phase = 0;
                wholeX += 1.0f;
            }

            xt::text::AtlasEntry entry{};
            bool found =
                glyphAtlas->lookup(cmd.font, glyphId, sizeDevice, static_cast<float>(phase) * 0.25f, entry);
            // Unlike xt_gl (which draws the batch before the repacked atlas is uploaded), a
            // deferred frame cannot draw now: build() restarts the whole frame instead.
            if (glyphAtlas->wasReset()) {
                atlasResetSeen = true;
            }
            if (!found) {
                doOutlineGlyph(cmd.font, glyphId, glyphPos, cmd.size, cmd.xf, dpr, cmd.color, shaper);
                continue;
            }
            if (entry.w == 0 || entry.h == 0) {
                continue;
            }
            uploadPendingAtlas();

            float x = wholeX + static_cast<float>(entry.bearingX);
            float y = std::round(posWorld.y * dpr) - static_cast<float>(entry.bearingY);
            float w = static_cast<float>(entry.w);
            float h = static_cast<float>(entry.h);
            float u0 = static_cast<float>(entry.x) / texSize;
            float v0 = static_cast<float>(entry.y) / texSize;
            float u1 = static_cast<float>(entry.x + entry.w) / texSize;
            float v1 = static_cast<float>(entry.y + entry.h) / texSize;

            Vec2 corners[4] = {Vec2{x, y}, Vec2{x + w, y}, Vec2{x + w, y + h}, Vec2{x, y + h}};
            Vec2 uvs[4] = {Vec2{u0, v0}, Vec2{u1, v0}, Vec2{u1, v1}, Vec2{u0, v1}};
            Vertex quad[4];
            bool allOk = true;
            for (int c = 0; c < 4; ++c) {
                bool ok = false;
                HomoPoint hp = homogeneousVertexPos(corners[c], ok);
                if (!ok) {
                    allOk = false;
                    break;
                }
                quad[c] = Vertex{hp.x, hp.y, hp.w, uvs[c].x, uvs[c].y, rgba};
            }
            if (!allOk || entry.texture >= 4) {
                continue;
            }
            std::vector<Vertex>& verts = glyphBatch[entry.texture];
            verts.push_back(quad[0]);
            verts.push_back(quad[1]);
            verts.push_back(quad[2]);
            verts.push_back(quad[3]);
        }
        flushGlyphBatch(gamma);
#endif
    }

    // ---------------------------------------------------------------------------------------
    // Clip (Docs/design/10-backends.md §10.3.3; rect clips as scissor per Docs/tasks/T-146.md).
    // ---------------------------------------------------------------------------------------

    void pushScissorLevel(const Scissor& next) {
        ClipLevel level;
        level.stencil = false;
        level.prevScissor = emit.scissor();
        clipStack.push_back(level);
        emit.setScissor(next);
    }

    /// A clip that passes nothing (a projected clip whose corners failed to project): an empty
    /// scissor level, so the matching PopClip still balances.
    void pushDegenerateClip() { pushScissorLevel(Scissor{0, 0, 0, 0}); }

    void emitStencilClipLevel(const ClipLevel& level, int depthBefore) {
        DrawCmd winding;
        winding.shading = Shading::StencilOnly;
        winding.blend = Blend::Replace;
        winding.stencilOp =
            level.rule == FillRule::NonZero ? StencilOp::WriteIncrWrap : StencilOp::WriteInvert;
        winding.stencilRef = static_cast<uint8_t>(depthBefore);
        emit.pushCmd(winding, level.winding);

        DrawCmd push;
        push.shading = Shading::StencilOnly;
        push.blend = Blend::Replace;
        push.stencilOp = StencilOp::ClipPush;
        push.stencilRef = static_cast<uint8_t>(depthBefore + 1);
        emit.pushCmd(push, level.quad);
    }

    void pushClipStencil(std::span<const Vec2> pts, std::span<const uint32_t> starts, FillRule rule) {
        if (pts.empty()) {
            pushScissorLevel(emit.scissor());  // nothing to clip to: a no-op level for the PopClip
            return;
        }
        if (clipDepth >= static_cast<int>(kMaxClipDepth)) {
            // Docs/tasks/T-263a.md E8, T-263b: a stencil clip nested deeper than
            // xt::kMaxClipDepth gets no stencil level, without asserting. It is reduced to its
            // bounding box as a scissor level, so the content can only overflow the shape inside
            // that box, and the matching PopClip still pairs with it.
            lastError = "clip depth limit (15) exceeded; clip reduced to its bounding box";
            pushScissorRect(boundsOf(pts));
            return;
        }
        ClipLevel level;
        level.stencil = true;
        level.prevScissor = emit.scissor();
        level.rule = rule;
        level.winding = emitFans(pts, starts, Mat2x3{}, false);
        level.quad = emitBoundsQuad(boundsOf(pts), Mat2x3{}, false, 0u);
        // The stencil level also narrows the scissor to the shape's AABB: everything outside it
        // fails the depth test anyway, so this only saves fill.
        Rect b = boundsOf(pts);
        int x0, x1, y0, y1;
        pixelRange(b.x, b.x + b.w, x0, x1);
        pixelRange(b.y, b.y + b.h, y0, y1);
        Scissor aabb{x0, y0, x1 - x0, y1 - y0};
        if (!emit.scissor().empty() && targetHasStencil()) {
            emitStencilClipLevel(level, clipDepth);
        }
        clipStack.push_back(level);
        ++clipDepth;
        emit.setScissor(detail::intersect(emit.scissor(), aabb));
    }

    void doPushClipRect(const ClipRectCmd& cmd, float dpr) {
        flushPendingClears();
        Mat2x3 deviceXf = deviceTransform(cmd.xf, dpr);
        Vec2 corners[4] = {
            apply(deviceXf, Vec2{cmd.rect.x, cmd.rect.y}),
            apply(deviceXf, Vec2{cmd.rect.x + cmd.rect.w, cmd.rect.y}),
            apply(deviceXf, Vec2{cmd.rect.x + cmd.rect.w, cmd.rect.y + cmd.rect.h}),
            apply(deviceXf, Vec2{cmd.rect.x, cmd.rect.y + cmd.rect.h}),
        };
        if (projecting()) {
            Vec2 q[4];
            for (int i = 0; i < 4; ++i) {
                bool ok = false;
                q[i] = projectDevicePoint(corners[i], ok);
                if (!ok) {
                    pushDegenerateClip();
                    return;
                }
            }
            // A projected but untilted plane (perspective on an ancestor, no 3D rotation here)
            // still yields an axis-aligned rectangle: keep it a scissor level.
            Rect rect;
            if (axisAlignedQuad(std::span<const Vec2>(q, 4), rect)) {
                pushScissorRect(rect);
                return;
            }
            uint32_t starts[1] = {0};
            pushClipStencil(std::span<const Vec2>(q, 4), std::span<const uint32_t>(starts, 1),
                            FillRule::NonZero);
            return;
        }
        if (xfKeepsAxisAlignment(deviceXf)) {
            pushScissorRect(boundsOf(std::span<const Vec2>(corners, 4)));
            return;
        }
        uint32_t starts[1] = {0};
        pushClipStencil(std::span<const Vec2>(corners, 4), std::span<const uint32_t>(starts, 1),
                        FillRule::NonZero);
    }

    /// True when the four corners `q` form an axis-aligned rectangle (each edge horizontal or
    /// vertical within kAxisTolerancePx, the float noise of a projected but untilted plane);
    /// `out` receives its bounds.
    static bool axisAlignedQuad(std::span<const Vec2> q, Rect& out) {
        constexpr float kAxisTolerancePx = 1e-3f;
        if (q.size() != 4) {
            return false;
        }
        for (size_t i = 0; i < 4; ++i) {
            const Vec2 a = q[i], b = q[(i + 1) % 4];
            if (std::fabs(a.x - b.x) > kAxisTolerancePx && std::fabs(a.y - b.y) > kAxisTolerancePx) {
                return false;
            }
        }
        out = boundsOf(q);
        return true;
    }

    /// T-155: a clip that is one axis-aligned rectangle in surface pixels clips exactly like an
    /// axis-aligned PushClipRect, so it becomes a scissor level (pixel-centre rule) instead of a
    /// stencil level: no winding/ClipPush/ClipPop and nothing to replay into layers.
    void pushScissorRect(const Rect& rect) {
        int x0, x1, y0, y1;
        pixelRange(rect.x, rect.x + rect.w, x0, x1);
        pixelRange(rect.y, rect.y + rect.h, y0, y1);
        pushScissorLevel(detail::intersect(emit.scissor(), Scissor{x0, y0, x1 - x0, y1 - y0}));
    }

    void doPushClipPath(const ClipPathCmd& cmd, float dpr) {
        flushPendingClears();
        Mat2x3 deviceXf = deviceTransform(cmd.xf, dpr);
        std::span<const float> rawPoints(list->points.data() + cmd.path.pointOffset, cmd.path.pointCount);
        float tol = projecting() ? flattenTolerance(deviceXf, rawPoints) : kFlattenTolerance;
        flatten(cmd.path, deviceXf, tol);
        if (projecting() && !projectPointsInPlace(flatPoints)) {
            pushDegenerateClip();
            return;
        }
        // An unrounded overflow box or the simplifier's flattened clip rect: a scissor level.
        Rect rect;
        if (singleConvexContour(flatPoints, flatStarts) && axisAlignedQuad(contourA.pts, rect)) {
            pushScissorRect(rect);
            return;
        }
        pushClipStencil(flatPoints, flatStarts, cmd.rule);
    }

    void doPopClip() {
        flushPendingClears();
        XT_ASSERT(!clipStack.empty());
        if (clipStack.empty()) {
            return;
        }
        ClipLevel level = clipStack.back();
        clipStack.pop_back();
        if (level.stencil) {
            if (!level.prevScissor.empty() && targetHasStencil()) {
                DrawCmd pop;
                pop.shading = Shading::StencilOnly;
                pop.blend = Blend::Replace;
                pop.stencilOp = StencilOp::ClipPop;
                pop.stencilRef = static_cast<uint8_t>(clipDepth);
                // The pop covers the level's own AABB under the scissor it was pushed with.
                emit.setScissor(level.prevScissor);
                emit.pushCmd(pop, level.quad);
            }
            --clipDepth;
        }
        emit.setScissor(level.prevScissor);
    }

    /// T-343 (23 §23.aa.3): in Direct mode the scene target has no stencil (a host LDR target);
    /// clip levels are only emitted into transients, which replay them when they open. Every
    /// draw of a Direct frame lands in a transient (band targets, scene-run layers), so nothing
    /// on the scene target ever needs the stencil.
    bool targetHasStencil() const { return !directMode || emit.currentTarget() != kSurfaceTarget; }

    /// Re-establishes every active stencil clip level in a freshly cleared target (whose
    /// stencil is all zero). Replaces xt_gl's stencil blit.
    void replayClips() {
        int depth = 0;
        for (const ClipLevel& level : clipStack) {
            if (!level.stencil) {
                continue;
            }
            Scissor saved = emit.scissor();
            emit.setScissor(level.prevScissor);
            emitStencilClipLevel(level, depth);
            emit.setScissor(saved);
            ++depth;
        }
    }

    /// Opens a cleared, stencil-capable transient for `window` as the new current target.
    TargetHandle openLayerTarget(const Scissor& window) {
        TargetHandle t = emit.acquireTransient(TextureFormat::Rgba8, true, window);
        emit.beginPass(t, true);
        replayClips();
        return t;
    }

    /// The window a layer/mask/effect target gets from its command's `boundsHint` (world units,
    /// T-133; scaled by dpr like xt::sw does), intersected with the current scissor (nothing
    /// outside it can be drawn or composited) and normalised by the emitter (Docs/tasks/
    /// T-151.md point 2). An empty hint is the lazy "whole viewport" default: full surface.
    /// `outsetPx` grows the hint first (a leading BackdropBlur's read range).
    Scissor hintWindow(const Rect& hint, float dpr, float outsetPx = 0.0f) {
        if (hint.w <= 0.0f || hint.h <= 0.0f) {
            return emit.targetWindow(emit.scissor());
        }
        const float x0 = hint.x * dpr - outsetPx;
        const float y0 = hint.y * dpr - outsetPx;
        const float x1 = (hint.x + hint.w) * dpr + outsetPx;
        const float y1 = (hint.y + hint.h) * dpr + outsetPx;
        const float lo = -65536.0f, hi = 65536.0f;
        const int ix0 = static_cast<int>(std::floor(std::clamp(x0, lo, hi)));
        const int iy0 = static_cast<int>(std::floor(std::clamp(y0, lo, hi)));
        const int ix1 = static_cast<int>(std::ceil(std::clamp(x1, lo, hi)));
        const int iy1 = static_cast<int>(std::ceil(std::clamp(y1, lo, hi)));
        const Scissor px{ix0, iy0, ix1 - ix0, iy1 - iy0};
        return emit.targetWindow(detail::intersect(px, emit.scissor()));
    }

    // ---------------------------------------------------------------------------------------
    // Layer / Mask / Effect.
    // ---------------------------------------------------------------------------------------

    /// Composites `src` onto `parent` (which becomes the current target) with `mode`/`opacity`:
    /// a quad over `src`'s window (u,v 0..1 = the whole of `src`), under the current scissor.
    void compositeOnto(TargetHandle parent, TargetHandle src, BlendMode mode, float opacity) {
        const Scissor window = emit.windowOf(src);
        if (mode == BlendMode::Normal) {
            emit.beginPass(parent, false);
            DrawCmd c;
            c.shading = Shading::Composite;
            c.src = src;
            c.blend = Blend::SrcOver;
            c.stencilOp = StencilOp::None;
            c.paramIndex = emit.pushParams(CompositeParams{0.0f, opacity});
            emit.pushCmd(c, emit.windowQuad(window));
            return;
        }
        // The backdrop copy shares `src`'s window so the shader reads both at the same u,v.
        TargetHandle backdrop = emit.acquireTransient(TextureFormat::Rgba8, false, window);
        emit.beginPass(backdrop, false, true, parent);
        if (directMode && parent == kSurfaceTarget) {
            statBackdropCopyPx += static_cast<uint32_t>(window.w) * static_cast<uint32_t>(window.h);
        }
        emit.beginPass(parent, false);
        DrawCmd c;
        c.shading = Shading::Composite;
        c.src = src;
        c.aux = backdrop;
        c.blend = Blend::Replace;
        c.stencilOp = StencilOp::None;
        c.paramIndex = emit.pushParams(CompositeParams{static_cast<float>(static_cast<int>(mode)), opacity});
        emit.pushCmd(c, emit.windowQuad(window));
        emit.releaseTransient(backdrop);
    }

    void doPushLayer(const LayerCmd& cmd) {
        flushPendingClears();
        TargetFrame f;
        f.kind = FrameKind::Layer;
        f.parent = emit.currentTarget();
        f.opacity = cmd.opacity;
        f.blend = cmd.blend;
        f.a = openLayerTarget(hintWindow(cmd.boundsHint, currentDpr));
        frameStack.push_back(std::move(f));
    }

    void doPopLayer() {
        flushPendingClears();
        XT_ASSERT(!frameStack.empty() && frameStack.back().kind == FrameKind::Layer);
        if (frameStack.empty() || frameStack.back().kind != FrameKind::Layer) {
            return;
        }
        TargetFrame f = std::move(frameStack.back());
        frameStack.pop_back();
        compositeOnto(f.parent, f.a, f.blend, f.opacity);
        emit.releaseTransient(f.a);
    }

    void doPushMask(const MaskCmd& cmd) {
        flushPendingClears();
        TargetFrame f;
        f.kind = FrameKind::Mask;
        f.parent = emit.currentTarget();
        f.mode = cmd.mode;
        const Scissor window = hintWindow(cmd.boundsHint, currentDpr);
        f.a = openLayerTarget(window);  // mask shape first (§9.2: PushMask -> mask content)
        f.b = emit.acquireTransient(TextureFormat::Rgba8, true, window);
        frameStack.push_back(std::move(f));
    }

    void doEndMaskContent() {
        flushPendingClears();
        XT_ASSERT(!frameStack.empty() && frameStack.back().kind == FrameKind::Mask);
        if (frameStack.empty() || frameStack.back().kind != FrameKind::Mask) {
            return;
        }
        TargetFrame& f = frameStack.back();
        emit.beginPass(f.b, true);
        replayClips();
    }

    void doPopMask() {
        flushPendingClears();
        XT_ASSERT(!frameStack.empty() && frameStack.back().kind == FrameKind::Mask);
        if (frameStack.empty() || frameStack.back().kind != FrameKind::Mask) {
            return;
        }
        TargetFrame f = std::move(frameStack.back());
        frameStack.pop_back();

        // MaskMode -> MaskApply mode: Alpha/AlphaInverted/Luma/LumaInverted; Add/Subtract/
        // Intersect (4-6) are expanded by the Painter (Docs/design/09-command-list.md §9.4) and
        // fall back to Alpha here, as in xt_gl.
        int modeIdx = static_cast<int>(f.mode);
        float mode = (modeIdx >= 0 && modeIdx <= 3) ? static_cast<float>(modeIdx) : 0.0f;
        TargetHandle masked = emit.acquireTransient(TextureFormat::Rgba8, false, emit.windowOf(f.b));
        emit.beginPass(masked, false);
        emit.pushEffect(EffectProgram::MaskApply, emit.pushParams(MaskApplyParams{mode}), f.b, f.a);
        compositeOnto(f.parent, masked, BlendMode::Normal, 1.0f);
        emit.releaseTransient(masked);
        emit.releaseTransient(f.a);
        emit.releaseTransient(f.b);
    }

    // ---------------------------------------------------------------------------------------
    // T-162 effect result cache.
    // ---------------------------------------------------------------------------------------

    struct Segment {
        size_t end = 0;  ///< index of the matching PopEffect (commands.size() when unbalanced)
        bool cacheable = false;
        uint64_t hash = 0;
    };

    /// Classifies the `PushEffect` at `start`: finds its PopEffect, decides whether its result
    /// is a pure function of the segment (no backdrop read, no external texture, a caching
    /// device) and, if so, computes the cache key.
    Segment classifySegment(const CommandList& cl, size_t start, const EffectCmd& c, const IrView* ir) {
        Segment seg;
        bool external = false;
        int depth = 0;
        seg.end = cl.commands.size();
        // T-263d: each asset command's id is its own source's.
        auto isExternal = [&](AssetId asset, uint16_t source) {
            const IrView* src = sourceIr(cl, source, ir);
            return src != nullptr && isExternalAsset(asset, *src);
        };
        for (size_t i = start; i < cl.commands.size(); ++i) {
            const Command& cmd = cl.commands[i];
            if (cmd.kind == CmdKind::PushEffect) {
                ++depth;
            } else if (cmd.kind == CmdKind::PopEffect) {
                if (--depth == 0) {
                    seg.end = i;
                    break;
                }
            } else if (cmd.kind == CmdKind::DrawImage) {
                const ImageCmd& image = cl.images[cmd.index];
                external = external || isExternal(image.asset, image.source);
            } else if (cmd.kind == CmdKind::DrawImageQuad) {
                const ImageQuadCmd& quad = cl.imageQuads[cmd.index];
                external = external || isExternal(quad.asset, quad.source);
            } else if (cmd.kind == CmdKind::DrawParticles) {
                const ParticlesCmd& pc = cl.particles[cmd.index];  // T-202
                external = external || (pc.texture != kInvalidAsset && isExternal(pc.texture, pc.source));
            } else if (cmd.kind == CmdKind::DrawImageMesh) {
                const ImageMeshCmd& mesh = cl.imageMeshes[cmd.index];  // T-245
                external = external || isExternal(mesh.asset, mesh.source);
            }
        }
        if (cacheUnsupported || external || seg.end >= cl.commands.size() || c.effectCount == 0 ||
            c.effectOffset >= cl.effects.size()) {
            return seg;
        }
        const EffectInstance& first = cl.effects[c.effectOffset];
        if (first.kind == EffectKind::BackdropBlur && first.enabled != 0) {
            // Reads what lies below the node: not a function of the segment -- except (T-343,
            // 23 §23.aa.4) a top-level glass panel over a scene the host declared static: the
            // scene is then a constant and the panel's output may be kept until a build without
            // the promise releases it (its hash is not computed then, so the entry goes idle).
            if (!(directMode && sceneStaticBuild && frameStack.empty())) {
                return seg;
            }
        }
        seg.cacheable = true;
        seg.hash = segmentHash(cl, start, seg.end);
        return seg;
    }

    /// The cache key of commands `[start, end]`: everything the segment's pixels depend on
    /// besides the commands themselves (dpr, surface size, fringe, the parent window, the
    /// scissor and stencil clip levels, the projection stack), then core's per-command hashes.
    uint64_t segmentHash(const CommandList& cl, size_t start, size_t end) {
        if (!cmdHashesValid) {
            hashCommandList(cl, cmdHashes);
            cmdHashesValid = true;
        }
        struct Header {
            float dpr;
            int32_t width, height, fringe;
            Scissor parent, scissor;
            int32_t clipDepth;
            uint32_t mappingCount;
            /// T-263c: the commands' AssetIds mean other images under another IR (T-263d: the
            /// IRs of the list's sources; a command's hash says which one it uses).
            uint32_t assetSource;
        };
        const Scissor parent = emit.currentWindow();
        const Scissor scissor = emit.scissor();
        const Header hdr{currentDpr,    targetWidth, targetHeight, edgeFringe ? 1 : 0,
                         parent,        scissor,     clipDepth,    static_cast<uint32_t>(mappings.size()),
                         listSourcesKey};
        uint64_t h = fnv1a(kFnvBasis, &hdr, sizeof(hdr));
        for (const project3d::Mapping& m : mappings) {
            const uint8_t has = m.hasProjection ? 1 : 0;
            h = fnv1a(h, &has, 1);
            h = fnv1a(h, &m.viewProj, sizeof(m.viewProj));
            h = fnv1a(h, &m.rootXf, sizeof(m.rootXf));
            h = fnv1a(h, &m.planeWorld3, sizeof(m.planeWorld3));
        }
        for (const ClipLevel& level : clipStack) {
            if (!level.stencil) {
                continue;  // an axis-aligned rect clip is the scissor, hashed above
            }
            const uint8_t rule = static_cast<uint8_t>(level.rule);
            h = fnv1a(h, &rule, 1);
            const GpuFrame& f = emit.frame();
            h = fnv1a(h, f.vertices.data() + level.winding.baseVertex,
                      size_t(level.winding.vertexCount) * sizeof(Vertex));
            h = fnv1a(h, f.vertices.data() + level.quad.baseVertex,
                      size_t(level.quad.vertexCount) * sizeof(Vertex));
        }
        for (size_t i = start; i <= end && i < cmdHashes.size(); ++i) {
            h = fnv1a(h, &cmdHashes[i], sizeof(uint64_t));
        }
        return h;
    }

    CacheEntry* findCacheEntry(uint64_t hash) {
        for (CacheEntry& e : cacheEntries) {
            if (e.hash == hash) {
                return &e;
            }
        }
        return nullptr;
    }

    bool seenLastBuild(uint64_t hash) const {
        for (uint64_t h : prevSegmentHashes) {
            if (h == hash) {
                return true;
            }
        }
        return false;
    }

    bool cacheHasRoom(size_t bytes) const {
        return !cacheUnsupported && cacheEntries.size() < FrameBuilder::kEffectCacheMaxEntries &&
               cacheBytes + bytes <= FrameBuilder::kEffectCacheBudgetBytes;
    }

    /// Draws the cached result of a segment: one textured quad over its window, under the
    /// current scissor, where the segment's composite would have gone (opacity 1, Normal).
    /// Returns false when the window does not match the entry (the segment is then drawn).
    bool emitCachedEffect(const EffectCmd& c, CacheEntry& entry) {
        const Scissor window = hintWindow(c.boundsHint, currentDpr);
        if (window.w != entry.width || window.h != entry.height) {
            XT_ASSERT(false && "effect cache entry does not match its window");
            return false;
        }
        flushPendingClears();
        entry.touched = true;
        DrawCmd d;
        d.shading = Shading::Texture;
        d.texture = entry.texture;
        d.sampler = Sampler::LinearClamp;
        d.blend = Blend::SrcOver;
        d.stencilOp = StencilOp::None;
        emit.pushCmd(d, emit.windowQuad(window));
        return true;
    }

    /// Asks the device for a cached target, accounting for the budget. 0 when refused.
    TextureHandle acquireCacheTarget(uint16_t w, uint16_t h) {
        const size_t bytes = size_t(w) * size_t(h) * 4u;
        if (!cacheHasRoom(bytes)) {
            return 0;
        }
        const TextureHandle t = device->acquireCachedTarget(w, h, TextureFormat::Rgba8);
        if (t == 0) {
            if (!cacheEverAcquired) {
                cacheUnsupported = true;
            }
            return 0;
        }
        cacheEverAcquired = true;
        cacheBytes += bytes;
        return t;
    }

    void releaseCacheEntry(size_t index) {
        CacheEntry& e = cacheEntries[index];
        if (device != nullptr) {
            device->releaseCachedTarget(e.texture);
        }
        cacheBytes -= size_t(e.width) * size_t(e.height) * 4u;
        cacheEntries.erase(cacheEntries.begin() + static_cast<std::ptrdiff_t>(index));
    }

    void flushEffectCache() {
        while (!cacheEntries.empty()) {
            releaseCacheEntry(cacheEntries.size() - 1);
        }
        cacheBytes = 0;
        prevSegmentHashes.clear();
        curSegmentHashes.clear();
        cachePending = 0;
    }

    /// Start of a buildOnce: entries this build() already added belong to a frame that is being
    /// discarded (an atlas restart), so they go; the rest start untouched.
    void beginCacheBuild() {
        while (cacheEntries.size() > cacheEntriesAtBuildStart) {
            releaseCacheEntry(cacheEntries.size() - 1);
        }
        for (CacheEntry& e : cacheEntries) {
            e.touched = false;
        }
        curSegmentHashes.clear();
        cachePending = 0;
    }

    /// End of build(): entries no segment used this build are released (one frame idle).
    void endCacheBuild() {
        for (size_t i = cacheEntries.size(); i-- > 0;) {
            if (!cacheEntries[i].touched) {
                releaseCacheEntry(i);
            }
        }
        prevSegmentHashes.swap(curSegmentHashes);
        curSegmentHashes.clear();
        cmdHashesValid = false;
    }

    /// T-203 (Docs/design/18-effects.md §18.7): `e` (Custom) as the scheduler runs it: kind
    /// Custom with `shader` = the device's program id when the device runs the asset's GLSL,
    /// else the asset's screen-converted `fallback`. False when neither applies (no ShaderRef in
    /// the list, or a `none` fallback): the effect is skipped. T-263d: `source` is the
    /// PushEffect's -- the shader id is that source's, and so is the ShaderRef.
    bool resolveCustomEffect(const EffectInstance& e, uint16_t source, const Mat2x3& screenXf,
                             EffectInstance& out) {
        const ShaderRef* ref = nullptr;
        for (const ShaderRef& s : list->shaders) {
            if (s.asset == e.shader && s.source == source) {
                ref = &s;
                break;
            }
        }
        if (ref == nullptr) {
            return false;
        }
        const size_t textSize = list->shaderText.size();
        uint32_t program = 0;
        if (device != nullptr && size_t(ref->glslOffset) + ref->glslLength <= textSize &&
            size_t(ref->namesOffset) + ref->namesLength <= textSize &&
            size_t(ref->paramOffset) + ref->paramCount <= list->shaderParams.size() &&
            ref->paramCount <= 16) {
            uint8_t types[16] = {};
            for (uint32_t i = 0; i < ref->paramCount; ++i) {
                types[i] = list->shaderParams[ref->paramOffset + i].type;
            }
            const std::string_view glsl(list->shaderText.data() + ref->glslOffset, ref->glslLength);
            const std::string names(list->shaderText.data() + ref->namesOffset, ref->namesLength);
            program = device->prepareCustomEffect(glsl, names.c_str(), types, ref->paramCount);
        }
        if (program != 0) {
            out = e;
            out.shader = program;
            return true;
        }
        if (ref->fallback.kind == EffectKind::None || ref->fallback.kind == EffectKind::Custom) {
            return false;
        }
        out = effectToScreen(ref->fallback, screenXf);
        out.enabled = 1;
        return true;
    }

    void doPushEffect(const EffectCmd& c, float dpr, const Segment& seg) {
        flushPendingClears();
        Mat2x3 screenXf = mul(scaling(Vec2{dpr, dpr}), c.xf);
        Mat2x3 invScreenXf;
        if (!invert(screenXf, invScreenXf)) {
            invScreenXf = Mat2x3{};
        }
        TargetFrame f;
        f.kind = FrameKind::Effect;
        f.parent = emit.currentTarget();
        // A leading BackdropBlur reads what lies below the node (Docs/design/18-effects.md
        // §18.6): the parent is copied before the node's own layer is opened. T-343 (18 §18.15,
        // 23 §23.aa.3): the copy is the base of a blur pyramid over the hint outset by the
        // blur's read range (3 sigma, like xt::sw's capture) plus the pyramid's own margin; a
        // top-level panel of a Direct frame shares the pyramid of its merged region instead.
        const EffectInstance* backdropFirst = nullptr;
        if (c.effectCount > 0 && c.effectOffset < list->effects.size()) {
            const EffectInstance& first = list->effects[c.effectOffset];
            if (first.kind == EffectKind::BackdropBlur && first.enabled != 0) {
                backdropFirst = &first;
            }
        }
        const Scissor window = hintWindow(c.boundsHint, dpr);
        if (backdropFirst != nullptr) {
            const EffectInstance screenFirst = effectToScreen(*backdropFirst, screenXf);
            const float sigma = backdropSigma(screenFirst);
            if (directMode && frameStack.empty() && currentCluster >= 0 &&
                currentCluster < static_cast<int>(clusters.size())) {
                PyramidCluster& cluster = clusters[static_cast<size_t>(currentCluster)];
                if (!cluster.built) {
                    detail::buildBackdropPyramid(emit, kSurfaceTarget, cluster.window, cluster.maxSigma,
                                                 cluster.pyramid);
                    cluster.built = true;
                    statBackdropCopyPx += cluster.pyramid.copiedPx;
                    statPyramidLevels += static_cast<uint32_t>(cluster.pyramid.levels);
                }
                f.sharedPyramid = &cluster.pyramid;
            } else {
                const Scissor region =
                    hintWindow(c.boundsHint, dpr, effectReadExpand(screenFirst) + pyramidMargin(sigma));
                detail::buildBackdropPyramid(emit, f.parent, region, sigma, f.ownPyramid);
                if (directMode && f.parent == kSurfaceTarget) {
                    statBackdropCopyPx += f.ownPyramid.copiedPx;
                    statPyramidLevels += static_cast<uint32_t>(f.ownPyramid.levels);
                }
                f.hasOwnPyramid = true;
            }
        }
        f.a = openLayerTarget(window);
        f.invScreenXf = invScreenXf;
        f.contentBoundsPx = Rect{c.contentBounds.x * dpr, c.contentBounds.y * dpr, c.contentBounds.w * dpr,
                                 c.contentBounds.h * dpr};
        f.screenParams.reserve(std::min<size_t>(c.effectCount, list->effects.size()));
        bool hasCustom = false;
        for (uint32_t i = 0;
             i < c.effectCount && static_cast<uint64_t>(c.effectOffset) + i < list->effects.size(); ++i) {
            const EffectInstance& e = list->effects[c.effectOffset + i];
            if (e.kind == EffectKind::Custom) {
                // T-203 (18.7): run the shader when the device can, else its fallback.
                hasCustom = true;
                EffectInstance resolved;
                if (resolveCustomEffect(e, c.source, screenXf, resolved)) {
                    f.screenParams.push_back(resolved);
                }
                continue;
            }
            f.screenParams.push_back(effectToScreen(e, screenXf));
        }
        f.time = list->time;
        // T-203: a custom shader may read xtTime, so its result is never cached.
        f.cacheable = seg.cacheable && !hasCustom;
        f.cacheHash = seg.hash;
        frameStack.push_back(std::move(f));
    }

    void doPopEffect() {
        flushPendingClears();
        XT_ASSERT(!frameStack.empty() && frameStack.back().kind == FrameKind::Effect);
        if (frameStack.empty() || frameStack.back().kind != FrameKind::Effect) {
            return;
        }
        TargetFrame f = std::move(frameStack.back());
        frameStack.pop_back();

        detail::EffectStackInput in;
        in.screenParams = f.screenParams;
        in.stops = list->stops;
        in.invScreenXf = f.invScreenXf;
        in.contentBoundsPx = f.contentBoundsPx;
        in.src = f.a;
        in.pyramid =
            f.sharedPyramid != nullptr ? f.sharedPyramid : (f.hasOwnPyramid ? &f.ownPyramid : nullptr);
        in.time = f.time;
        TargetHandle result = detail::applyEffectStack(emit, luts, in);
        detail::releaseBackdropPyramid(emit, f.ownPyramid);
        // T-162: keep the result for later frames once the segment has repeated. An identity
        // stack (`result == f.a`) has nothing worth keeping.
        if (f.cacheable && result != f.a && !cacheUnsupported) {
            const Scissor window = emit.windowOf(result);
            const size_t bytes = size_t(window.w) * size_t(window.h) * 4u;
            if (seenLastBuild(f.cacheHash)) {
                const TextureHandle tex =
                    acquireCacheTarget(static_cast<uint16_t>(window.w), static_cast<uint16_t>(window.h));
                if (tex != 0) {
                    GpuFrame& frame = emit.frame();
                    CacheStore store;
                    store.pass = static_cast<uint32_t>(frame.passes.size()) - 1;
                    store.src = result;
                    store.texture = tex;
                    frame.cacheStores.push_back(store);
                    CacheEntry e;
                    e.hash = f.cacheHash;
                    e.texture = tex;
                    e.width = static_cast<uint16_t>(window.w);
                    e.height = static_cast<uint16_t>(window.h);
                    e.touched = true;
                    cacheEntries.push_back(e);
                    ++cachePending;  // the next build serves it from the cache
                }
            } else if (cacheHasRoom(bytes)) {
                ++cachePending;  // first appearance: stored on the next build
            }
        }
        // opacity 1, Normal per Docs/design/18-effects.md §18.5.
        compositeOnto(f.parent, result, BlendMode::Normal, 1.0f);
        if (result != f.a) {
            emit.releaseTransient(result);
        }
        emit.releaseTransient(f.a);
    }

    // ---------------------------------------------------------------------------------------
    // build().
    // ---------------------------------------------------------------------------------------

    void buildOnce(const CommandList& cl, TextShaper* shaper, AssetResolver* assets, const IrView* ir,
                   GpuFrame& out) {
        list = &cl;
        emit.begin(out, targetWidth, targetHeight);
        clipStack.clear();
        clipDepth = 0;
        frameStack.clear();
        mappings.clear();
        pendingClears.clear();
        glyphParamCount = 0;
        haveLastGradient = false;
        currentDpr = cl.devicePixelRatio;
        const float dpr = currentDpr;
        lastExternals.clear();
        beginCacheBuild();
        statBandRedraws = statBackdropCopyPx = statPyramidLevels = 0;  // T-343: per attempt
        bandPending = 0;
#ifndef XT_GPU_NO_TEXT
        curRunSizes.clear();
        scalingRuns = 0;
#endif

        if (directMode) {
            buildDirect(cl, shaper, assets, ir, dpr);
        } else {
            for (size_t ci = 0; ci < cl.commands.size(); ++ci) {
                ci = processCommand(cl, ci, shaper, assets, ir, dpr);
            }
        }
        finishBuildOnce();
        list = nullptr;
    }

    /// Executes command `ci` of `cl`; returns the index of the last command consumed (the
    /// matching PopEffect when a cached effect segment was replaced by its quad).
    size_t processCommand(const CommandList& cl, size_t ci, TextShaper* shaper, AssetResolver* assets,
                          const IrView* ir, float dpr) {
        {
            const Command& cmd = cl.commands[ci];
            switch (cmd.kind) {
                case CmdKind::FillPath:
                    doFill(cl.fills[cmd.index], dpr);
                    break;
                case CmdKind::StrokePath:
                    doStroke(cl.strokes[cmd.index], dpr);
                    break;
                case CmdKind::DrawImage: {
                    // T-263d: the ids of an asset command are its own source's (enterSource()).
                    const ImageCmd& c = cl.images[cmd.index];
                    doImage(c, dpr, assets, enterSource(cl, c.source, assets, ir));
                    break;
                }
                case CmdKind::DrawGlyphs:
                    doGlyphs(cl.glyphRuns[cmd.index], dpr, shaper);
                    break;
                case CmdKind::PushClipRect:
                    doPushClipRect(cl.clipRects[cmd.index], dpr);
                    break;
                case CmdKind::PushClipPath:
                    doPushClipPath(cl.clipPaths[cmd.index], dpr);
                    break;
                case CmdKind::PopClip:
                    doPopClip();
                    break;
                case CmdKind::PushLayer:
                    doPushLayer(cl.layers[cmd.index]);
                    break;
                case CmdKind::PopLayer:
                    doPopLayer();
                    break;
                case CmdKind::PushMask:
                    doPushMask(cl.masks[cmd.index]);
                    break;
                case CmdKind::EndMaskContent:
                    doEndMaskContent();
                    break;
                case CmdKind::PopMask:
                    doPopMask();
                    break;
                case CmdKind::PushProjection: {
                    const ProjectionCmd& c = cl.projections[cmd.index];
                    project3d::Mapping mapping;
                    mapping.hasProjection = true;
                    mapping.viewProj = c.viewProj;
                    mapping.rootXf = c.rootXf;
                    mapping.planeWorld3 = mat4Identity();
                    mappings.push_back(mapping);
                    break;
                }
                case CmdKind::PopProjection:
                    XT_ASSERT(!mappings.empty());
                    if (!mappings.empty()) {
                        mappings.pop_back();
                    }
                    break;
                case CmdKind::PushTransform3D:
                    XT_ASSERT(!mappings.empty());
                    if (!mappings.empty()) {
                        mappings.back().planeWorld3 = cl.transforms3[cmd.index].world3;
                    }
                    break;
                case CmdKind::PopTransform3D:
                    XT_ASSERT(!mappings.empty());
                    if (!mappings.empty()) {
                        mappings.back().planeWorld3 = mat4Identity();
                    }
                    break;
                case CmdKind::DrawImageQuad: {
                    const ImageQuadCmd& c = cl.imageQuads[cmd.index];
                    doImageQuad(c, dpr, assets, enterSource(cl, c.source, assets, ir));
                } break;
                case CmdKind::PushEffect: {
                    const EffectCmd& c = cl.effectCmds[cmd.index];
                    const Segment seg = classifySegment(cl, ci, c, ir);
                    if (seg.cacheable) {
                        curSegmentHashes.push_back(seg.hash);
                        if (currentBandHashes != nullptr) {
                            currentBandHashes->push_back(seg.hash);  // T-343: kept while the band is
                        }
                        CacheEntry* entry = findCacheEntry(seg.hash);
                        if (entry != nullptr && emitCachedEffect(c, *entry)) {
                            return seg.end;  // the whole segment came from the cache
                        }
                    }
                    doPushEffect(c, dpr, seg);
                    break;
                }
                case CmdKind::PopEffect:
                    doPopEffect();
                    break;
                case CmdKind::DrawParticles: {
                    const ParticlesCmd& c = cl.particles[cmd.index];
                    doParticles(cl, c, dpr, assets, enterSource(cl, c.source, assets, ir));
                } break;
                case CmdKind::DrawImageMesh: {
                    const ImageMeshCmd& c = cl.imageMeshes[cmd.index];
                    doImageMesh(cl, c, dpr, assets, enterSource(cl, c.source, assets, ir));
                } break;
            }
        }
        return ci;
    }

    /// The end of a buildOnce: unbalanced stacks recovered, pending clears flushed, the frame closed.
    void finishBuildOnce() {
        // A well-formed list balances every push/pop; recover from an unbalanced one so the
        // frame stays drawable.
        while (!frameStack.empty()) {
            XT_ASSERT(false && "unbalanced layer/mask/effect stack");
            switch (frameStack.back().kind) {
                case FrameKind::Layer:
                    doPopLayer();
                    break;
                case FrameKind::Mask:
                    doPopMask();
                    break;
                case FrameKind::Effect:
                    doPopEffect();
                    break;
            }
        }
        while (!clipStack.empty()) {
            XT_ASSERT(false && "unbalanced clip stack");
            doPopClip();
        }
        flushPendingClears();
        emit.end();
    }

    // ---------------------------------------------------------------------------------------
    // T-343 Direct target mode (Docs/design/23-host-integration.md §23.aa).
    // ---------------------------------------------------------------------------------------

    static float backdropSigma(const EffectInstance& screenParams) {
        return effect_math::backdropSigma(screenParams);
    }
    static float pyramidMargin(float sigma) { return effect_math::pyramidMargin(sigma); }

    void releaseAllBands() {
        for (Band& b : prevBands) {
            if (b.texture != 0 && device != nullptr) {
                device->releaseBandTarget(b.texture);
            }
            b.texture = 0;
        }
        prevBands.clear();
        prevRunKinds.clear();
        resetCurBands();
        prevBandsValid = false;
        bandPending = 0;
    }

    /// Drops the bands of a buildOnce that is being started over: targets it acquired itself
    /// (not taken over from `prevBands`) go back to the device.
    void resetCurBands() {
        for (Band& b : curBands) {
            if (b.texture == 0) {
                continue;
            }
            bool fromPrev = false;
            for (const Band& p : prevBands) {
                fromPrev = fromPrev || p.texture == b.texture;
            }
            if (!fromPrev && device != nullptr) {
                device->releaseBandTarget(b.texture);
            }
        }
        curBands.clear();
        curRunKinds.clear();
        for (PyramidCluster& c : clusters) {
            c.pyramid = detail::BackdropPyramid{};
        }
        clusters.clear();
        runCluster.clear();
        currentCluster = -1;
        currentBandHashes = nullptr;
    }

    /// The device-pixel bounds of run `run`'s commands (xt::commandBounds, the rectangles the
    /// damage was computed from), as a window on the surface; empty when nothing draws.
    Scissor runWindow(const CommandList& cl, const SceneRun& run, float dpr) const {
        bool any = false;
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        for (uint32_t i = run.first; i < run.end && i < cl.commands.size(); ++i) {
            const Rect r = commandBounds(cl, cl.commands[i]);
            if (!(r.w > 0.0f) || !(r.h > 0.0f)) {
                continue;
            }
            if (!any) {
                x0 = r.x;
                y0 = r.y;
                x1 = r.x + r.w;
                y1 = r.y + r.h;
                any = true;
            } else {
                x0 = std::min(x0, r.x);
                y0 = std::min(y0, r.y);
                x1 = std::max(x1, r.x + r.w);
                y1 = std::max(y1, r.y + r.h);
            }
        }
        if (!any) {
            return Scissor{};
        }
        const float lo = -65536.0f, hi = 65536.0f;
        const int ix0 = static_cast<int>(std::floor(std::clamp(x0 * dpr, lo, hi)));
        const int iy0 = static_cast<int>(std::floor(std::clamp(y0 * dpr, lo, hi)));
        const int ix1 = static_cast<int>(std::ceil(std::clamp(x1 * dpr, lo, hi)));
        const int iy1 = static_cast<int>(std::ceil(std::clamp(y1 * dpr, lo, hi)));
        const Scissor px = detail::intersect(Scissor{ix0, iy0, ix1 - ix0, iy1 - iy0}, emit.fullScissor());
        if (px.empty()) {
            return Scissor{};
        }
        return emit.targetWindow(px);
    }

    /// True when this frame's damage touches `window` (null or full damage touches everything).
    bool damageTouches(const Scissor& window) const {
        if (damage == nullptr || damage->full) {
            return true;
        }
        for (const Rect& r : damage->rects) {
            if (r.x < static_cast<float>(window.x + window.w) && r.x + r.w > static_cast<float>(window.x) &&
                r.y < static_cast<float>(window.y + window.h) && r.y + r.h > static_cast<float>(window.y)) {
                return true;
            }
        }
        return false;
    }

    /// Walks a reused band's commands for their side effects on the stacks only: the clip and
    /// projection commands at depth 0 (a clip opened in this band applies to later runs); the
    /// scene target gets no stencil ops in Direct mode (targetHasStencil), so nothing is drawn.
    void dryWalk(const CommandList& cl, const SceneRun& run, float dpr) {
        int depth = 0;
        for (uint32_t ci = run.first; ci < run.end && ci < cl.commands.size(); ++ci) {
            const Command& cmd = cl.commands[ci];
            switch (cmd.kind) {
                case CmdKind::PushLayer:
                case CmdKind::PushMask:
                case CmdKind::PushEffect:
                    ++depth;
                    break;
                case CmdKind::PopLayer:
                case CmdKind::PopMask:
                case CmdKind::PopEffect:
                    --depth;
                    break;
                case CmdKind::PushClipRect:
                    if (depth == 0) doPushClipRect(cl.clipRects[cmd.index], dpr);
                    break;
                case CmdKind::PushClipPath:
                    if (depth == 0) doPushClipPath(cl.clipPaths[cmd.index], dpr);
                    break;
                case CmdKind::PopClip:
                    if (depth == 0) doPopClip();
                    break;
                case CmdKind::PushProjection:
                case CmdKind::PopProjection:
                case CmdKind::PushTransform3D:
                case CmdKind::PopTransform3D:
                    if (depth == 0) {
                        processCommand(cl, ci, nullptr, nullptr, nullptr, dpr);
                    }
                    break;
                default:
                    break;
            }
        }
    }

    /// Pastes `src` (a band transient or band target) onto the scene over `window` with one
    /// SrcOver quad, under the window itself as scissor (the band's own commands were already
    /// clipped when they were drawn, whatever clips are open now).
    void pasteBand(const Scissor& window, TargetHandle transient, TextureHandle texture) {
        emit.beginPass(kSurfaceTarget, false);
        const Scissor saved = emit.scissor();
        emit.setScissor(window);
        DrawCmd c;
        c.blend = Blend::SrcOver;
        c.stencilOp = StencilOp::None;
        c.sampler = Sampler::LinearClamp;
        if (transient != 0) {
            c.shading = Shading::Composite;
            c.src = transient;
            c.paramIndex = emit.pushParams(CompositeParams{0.0f, 1.0f});
        } else {
            c.shading = Shading::Texture;
            c.texture = texture;
        }
        emit.pushCmd(c, emit.windowQuad(window));
        emit.setScissor(saved);
    }

    /// One band run: pasted from its cached target when nothing of it changed, otherwise drawn
    /// into a stencil-capable transient, stored into a band target (unless this build fell back)
    /// and pasted from the transient.
    void drawBandRun(const CommandList& cl, size_t runIndex, size_t bandIndex, TextShaper* shaper,
                     AssetResolver* assets, const IrView* ir, float dpr) {
        const SceneRun& run = runs[runIndex];
        Band& band = curBands[bandIndex];
        band.window = bandWindows[runIndex];
        band.cmdCount = run.end - run.first;
        if (band.window.empty()) {
            dryWalk(cl, run, dpr);  // nothing visible: just its clips
            return;
        }
        const bool structureSame =
            prevBandsValid && prevRunKinds == curRunKinds && bandIndex < prevBands.size();
        Band* prev = structureSame ? &prevBands[bandIndex] : nullptr;
        if (!directFallback && prev != nullptr && prev->texture != 0 && prev->window == band.window &&
            prev->cmdCount == band.cmdCount && !damageTouches(band.window)) {
            // Reused: the target as it is, and its effect segments stay alive in the cache.
            band.texture = prev->texture;
            band.segmentHashes = prev->segmentHashes;
            for (uint64_t h : band.segmentHashes) {
                curSegmentHashes.push_back(h);
                if (CacheEntry* e = findCacheEntry(h)) {
                    e->touched = true;
                }
            }
            flushPendingClears();
            pasteBand(band.window, 0, band.texture);
            dryWalk(cl, run, dpr);
            return;
        }
        ++statBandRedraws;
        const TargetHandle t = emit.acquireTransient(TextureFormat::Rgba8, true, band.window);
        emit.beginPass(t, true);
        replayClips();
        band.segmentHashes.clear();
        currentBandHashes = &band.segmentHashes;
        for (size_t ci = run.first; ci < run.end && ci < cl.commands.size(); ++ci) {
            ci = processCommand(cl, ci, shaper, assets, ir, dpr);
        }
        currentBandHashes = nullptr;
        flushPendingClears();
        XT_ASSERT(frameStack.empty() && emit.currentTarget() == t);
        if (!directFallback) {
            TextureHandle tex = 0;
            if (prev != nullptr && prev->texture != 0 && prev->window.w == band.window.w &&
                prev->window.h == band.window.h) {
                tex = prev->texture;  // same size: overwritten by the store
            } else {
                tex = device->acquireBandTarget(static_cast<uint16_t>(band.window.w),
                                                static_cast<uint16_t>(band.window.h));
            }
            if (tex != 0) {
                GpuFrame& frame = emit.frame();
                CacheStore store;
                store.pass = static_cast<uint32_t>(frame.passes.size()) - 1;
                store.src = t;
                store.texture = tex;
                frame.cacheStores.push_back(store);
                band.texture = tex;
                ++bandPending;
            } else {
                directFallback = true;  // no band targets on this device: everything every frame
            }
        }
        pasteBand(band.window, t, 0);
        emit.releaseTransient(t);
    }

    /// The Direct-mode body of buildOnce: splits the list into runs, decides the fallback and the
    /// pyramid clusters, then draws run by run.
    void buildDirect(const CommandList& cl, TextShaper* shaper, AssetResolver* assets, const IrView* ir,
                     float dpr) {
        resetCurBands();
        scanSceneRuns(cl, runs);
        bandWindows.assign(runs.size(), Scissor{});
        runCluster.assign(runs.size(), -1);
        size_t bandCount = 0;
        float bandArea = 0.0f;
        for (size_t i = 0; i < runs.size(); ++i) {
            curRunKinds.push_back(runs[i].sceneRead ? 1 : 0);
            if (runs[i].sceneRead) {
                continue;
            }
            ++bandCount;
            bandWindows[i] = runWindow(cl, runs[i], dpr);
            bandArea += static_cast<float>(bandWindows[i].w) * static_cast<float>(bandWindows[i].h);
        }
        const float surfaceArea =
            static_cast<float>(std::max(1, targetWidth)) * static_cast<float>(std::max(1, targetHeight));
        directFallback =
            bandCount > FrameBuilder::kMaxBands || bandArea > FrameBuilder::kBandAreaLimit * surfaceArea;
        statBands = static_cast<uint32_t>(bandCount);
        curBands.resize(bandCount);

        // §23.aa.3: the read regions of the top-level backdrop-blur runs, merged where they meet.
        for (size_t i = 0; i < runs.size(); ++i) {
            const EffectCmd* lead = effect_math::leadingBackdropBlur(cl, runs[i]);
            if (lead == nullptr) {
                continue;
            }
            const EffectCmd& c = *lead;
            const EffectInstance& first = cl.effects[c.effectOffset];
            const EffectInstance screenFirst = effectToScreen(first, mul(scaling(Vec2{dpr, dpr}), c.xf));
            const float sigma = backdropSigma(screenFirst);
            const float outset = effectReadExpand(screenFirst) + pyramidMargin(sigma);
            const Rect h = c.boundsHint;
            if (!(h.w > 0.0f) || !(h.h > 0.0f)) {
                continue;
            }
            const float lo = -65536.0f, hi = 65536.0f;
            Scissor region{static_cast<int>(std::floor(std::clamp(h.x * dpr - outset, lo, hi))),
                           static_cast<int>(std::floor(std::clamp(h.y * dpr - outset, lo, hi))), 0, 0};
            region.w = static_cast<int>(std::ceil(std::clamp((h.x + h.w) * dpr + outset, lo, hi))) - region.x;
            region.h = static_cast<int>(std::ceil(std::clamp((h.y + h.h) * dpr + outset, lo, hi))) - region.y;
            region = detail::intersect(region, emit.fullScissor());
            if (region.empty()) {
                continue;
            }
            PyramidCluster c2;
            c2.window = region;
            c2.maxSigma = sigma;
            c2.remaining = 1;
            clusters.push_back(c2);
            runCluster[i] = static_cast<int>(clusters.size()) - 1;
        }
        // Merge clusters whose windows meet (repeat until nothing merges).
        auto meets = [](const Scissor& a, const Scissor& b) {
            return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
        };
        auto unite = [](const Scissor& a, const Scissor& b) {
            const int x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
            const int x1 = std::max(a.x + a.w, b.x + b.w), y1 = std::max(a.y + a.h, b.y + b.h);
            return Scissor{x0, y0, x1 - x0, y1 - y0};
        };
        for (bool merged = true; merged;) {
            merged = false;
            for (size_t a = 0; a < clusters.size() && !merged; ++a) {
                for (size_t b = a + 1; b < clusters.size(); ++b) {
                    if (!meets(clusters[a].window, clusters[b].window)) {
                        continue;
                    }
                    clusters[a].window = unite(clusters[a].window, clusters[b].window);
                    clusters[a].maxSigma = std::max(clusters[a].maxSigma, clusters[b].maxSigma);
                    clusters[a].remaining += clusters[b].remaining;
                    clusters.erase(clusters.begin() + static_cast<std::ptrdiff_t>(b));
                    for (int& rc : runCluster) {
                        if (rc == static_cast<int>(b)) {
                            rc = static_cast<int>(a);
                        } else if (rc > static_cast<int>(b)) {
                            --rc;
                        }
                    }
                    merged = true;
                    break;
                }
            }
        }
        for (PyramidCluster& c : clusters) {
            c.window = emit.targetWindow(c.window);  // padded like every transient window
        }

        size_t bandIndex = 0;
        for (size_t i = 0; i < runs.size(); ++i) {
            const SceneRun& run = runs[i];
            if (!run.sceneRead) {
                drawBandRun(cl, i, bandIndex++, shaper, assets, ir, dpr);
                continue;
            }
            currentCluster = runCluster[i];
            for (size_t ci = run.first; ci < run.end && ci < cl.commands.size(); ++ci) {
                ci = processCommand(cl, ci, shaper, assets, ir, dpr);
            }
            if (currentCluster >= 0 && currentCluster < static_cast<int>(clusters.size())) {
                PyramidCluster& c = clusters[static_cast<size_t>(currentCluster)];
                if (--c.remaining <= 0 && c.built) {
                    detail::releaseBackdropPyramid(emit, c.pyramid);
                    c.built = false;
                }
            }
            currentCluster = -1;
        }
        for (PyramidCluster& c : clusters) {
            if (c.built) {
                detail::releaseBackdropPyramid(emit, c.pyramid);
                c.built = false;
            }
        }
    }

    /// After a Direct build(): `curBands` become the bands the next build may reuse; targets of
    /// the previous bands that no current band took over go back to the device.
    void endDirectBuild() {
        for (Band& p : prevBands) {
            if (p.texture == 0) {
                continue;
            }
            bool kept = false;
            for (const Band& c : curBands) {
                kept = kept || c.texture == p.texture;
            }
            if (!kept && device != nullptr) {
                device->releaseBandTarget(p.texture);
            }
            p.texture = 0;
        }
        prevBands.swap(curBands);
        curBands.clear();
        prevRunKinds.swap(curRunKinds);
        curRunKinds.clear();
        prevBandsValid = true;
    }

    void build(const CommandList& cl, TextShaper* shaper, AssetResolver* assets, const IrView* ir,
               GpuFrame& out, PerfTracker* perf, const DamageList* damageList) {
        FrameStats* stats = (perf != nullptr && perf->enabled()) ? &perf->current() : nullptr;
        PerfFieldScope scope(stats != nullptr ? &stats->gpuBuildMs : nullptr);
        // T-263f: one host frame, unless lastFrameReusable() already counted it.
        if (!frameCounted) {
            ++frameCount;
        }
        frameCounted = false;
        lastError.clear();
        lastBuildValid = false;
        XT_ASSERT(device != nullptr);
        if (device == nullptr) {
            out.clear();
            lastError = "FrameBuilder::init was not called";
            return;
        }
        // Docs/tasks/T-263a.md E1: a list a host filled by hand is checked before any pool is
        // indexed (one the library built or already checked says so); an invalid one builds an
        // empty frame.
        if (!cl.structureChecked && !validateCommandList(cl)) {
            out.clear();
            lastError = "invalid command list; frame not drawn";
            return;
        }
        // Docs/tasks/T-263b.md E4: `AssetId`s are per IR (assigned from 0) and decoded by the
        // resolver, so a texture is only found under the IR and resolver it was uploaded for
        // (T-263c ruling 3: the two most recent pairs keep theirs). T-263d: the IRs are the
        // list's sources when it names them, else the call's `ir`.
        ++buildCount;
        selectListSources(cl, assets, ir);
        maxTexture = device->maxTextureSize();  // T-263d: once per build, not per texture
        cacheEntriesAtBuildStart = cacheEntries.size();
        // T-343: the host's target mode. A device that cannot read target 0 draws in Layer mode.
        damage = damageList;
        sceneStaticBuild = cl.sceneStatic;
        directMode = cl.hostTargetMode == HostTargetMode::Direct;
        if (directMode && !device->supportsDirectTarget()) {
            directMode = false;
            if (!directWarned) {
                directWarned = true;
                lastError = "direct target mode is not supported by the device; drawing in Layer mode";
            }
        }
        if (!directMode) {
            releaseAllBands();
        }
        directFallback = false;
        statBands = statBandRedraws = statBackdropCopyPx = statPyramidLevels = 0;
        bandPending = 0;
#ifndef XT_GPU_NO_TEXT
        atlasAllowed = true;
        atlasResetSeen = false;
        effectRunReported = false;
        buildOnce(cl, shaper, assets, ir, out);
        if (atlasResetSeen) {
            // The atlas was repacked mid-frame, so quads emitted before the reset point at stale
            // texels. Every glyph of this frame is now cached (or provably does not fit), so a
            // second attempt hits the cache; if it resets again the frame's glyphs exceed the
            // whole atlas and the outline fallback is used for the entire frame.
            atlasResetSeen = false;
            buildOnce(cl, shaper, assets, ir, out);
            if (atlasResetSeen) {
                atlasAllowed = false;
                buildOnce(cl, shaper, assets, ir, out);
            }
        }
        // T-263c ruling 4: this build's run sizes are what the next build compares with.
        std::sort(curRunSizes.begin(), curRunSizes.end(), [](const RunSize& a, const RunSize& b) {
            return a.key != b.key ? a.key < b.key : a.size < b.size;
        });
        prevRunSizes.swap(curRunSizes);
#else
        buildOnce(cl, shaper, assets, ir, out);
#endif
        luts.endFrame();
        endCacheBuild();
        if (directMode) {
            endDirectBuild();
        }
        if (stats != nullptr) {
            stats->bands = statBands;
            stats->bandRedraws = statBandRedraws;
            stats->backdropCopyPx = statBackdropCopyPx;
            stats->pyramidLevels = statPyramidLevels;
            stats->directFallback = (directMode && directFallback) ? 1u : 0u;
        }
        damage = nullptr;
        lastBuildValid = true;
    }

    bool lastFrameReusable(AssetResolver* assets, const IrView* ir) {
        // T-263f: a reusing host asks this once per frame, so it is what counts the frames of a
        // static screen (see `frameCount`).
        ++frameCount;
        frameCounted = true;
        if (!lastBuildValid || cachePending > 0 || bandPending > 0) {
            return false;
        }
        // E4: the frame was built from another IR or resolver (see build()). T-263d: a frame
        // built from a list of several sources is this IR's when it is one of them.
        uint32_t asked = 0;
        if (assets != nullptr && ir != nullptr) {
            asked = findAssetSource(assetSourceOf(assets, ir));
            bool anyBuilt = false;
            bool among = false;
            for (uint32_t id : listSources) {
                anyBuilt = anyBuilt || id != 0;
                among = among || (id != 0 && id == asked);
            }
            if (anyBuilt && !among) {
                return false;
            }
        }
#ifndef XT_GPU_NO_TEXT
        if (glyphAtlas != nullptr && glyphAtlas->hasPendingUploads()) {
            return false;
        }
        // T-263c ruling 4: a run was drawn from outlines because its size had just changed; the
        // next build puts it on the atlas.
        if (scalingRuns > 0) {
            return false;
        }
#endif
        for (const ExternalRef& r : lastExternals) {
            if (assets == nullptr || ir == nullptr) {
                return false;
            }
            // T-263d: a binding of another source of the list cannot be asked through `ir`.
            if (r.source != asked) {
                return false;
            }
            const AssetResolver::ExternalTexture* ext = assets->externalTexture(r.asset, *ir);
            const TextureHandle tex = ext != nullptr ? ext->texture : 0;
            const bool straight = ext != nullptr && ext->straightAlpha;
            if (tex != r.texture || straight != r.straightAlpha) {
                return false;
            }
        }
        // T-263f: an upload that failed is due for its retry and only a build makes the attempt,
        // so the frame is not reusable -- once per due retry: the build that follows tries (and
        // schedules the next retry if it fails again), and a host that does not build is not
        // asked a second time. Only images of the sources the last build drew: a build of that
        // list is what the answer asks for.
        bool retry = false;
        for (TextureEntry& entry : textureCache) {
            if (entry.announced || !retryDue(entry) ||
                std::find(listSources.begin(), listSources.end(), entry.source) == listSources.end()) {
                continue;
            }
            entry.announced = true;
            retry = true;
        }
        return !retry;
    }

    void invalidateAllAssets() {
        lastBuildValid = false;
        flushEffectCache();
        prevBandsValid = false;  // T-343: a band may have drawn the asset
        if (device != nullptr) {
            for (const TextureEntry& entry : textureCache) {
                if (entry.owned) {
                    device->destroyTexture(entry.tex);
                }
            }
        }
        textureCache.clear();
        ++assetEpoch;
    }

    void invalidateAsset(AssetId asset, const IrView* source) {
        lastBuildValid = false;
        flushEffectCache();
        prevBandsValid = false;  // T-343: a band may have drawn the asset
        // T-263f: every IR numbers its assets from 0, so the id alone means that id under every
        // source; with `source`, only the entries cached under that IR's load serial (whatever
        // the resolver) are meant.
        auto meant = [&](const TextureEntry& entry) {
            if (entry.asset != asset) {
                return false;
            }
            if (source == nullptr) {
                return true;
            }
            for (const SourceSlot& slot : sourceSlots) {
                if (slot.id != 0 && slot.id == entry.source) {
                    return slot.source.irSerial == source->loadSerial();
                }
            }
            return false;
        };
        // T-199: an atlas page's texture is shared by its assets, so dropping one asset drops
        // every entry that samples the same texture (they all re-upload on their next use).
        // (A texture is only ever shared within one source.)
        std::vector<TextureHandle> doomed;
        for (const TextureEntry& entry : textureCache) {
            if (meant(entry) && entry.tex != 0) {
                doomed.push_back(entry.tex);
            }
        }
        for (size_t i = 0; i < textureCache.size();) {
            const TextureEntry& entry = textureCache[i];
            // E10: the asset's own "could not be uploaded" entry (texture 0) goes too.
            if (meant(entry) || std::find(doomed.begin(), doomed.end(), entry.tex) != doomed.end()) {
                if (device != nullptr && entry.owned) {
                    device->destroyTexture(entry.tex);
                }
                textureCache.erase(textureCache.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    }
};

FrameBuilder::FrameBuilder() : m_impl(std::make_unique<Impl>()) {}
FrameBuilder::~FrameBuilder() {
    if (m_impl) {
        m_impl->shutdown();
    }
}

void FrameBuilder::init(GpuDevice& device) { m_impl->init(device); }
void FrameBuilder::shutdown() { m_impl->shutdown(); }
void FrameBuilder::resize(int targetWidth, int targetHeight) {
    if (targetWidth != m_impl->targetWidth || targetHeight != m_impl->targetHeight) {
        m_impl->lastBuildValid = false;
        m_impl->flushEffectCache();  // every window changes size or position with the surface
        m_impl->releaseAllBands();   // T-343: band windows move with the surface too
    }
    m_impl->targetWidth = targetWidth;
    m_impl->targetHeight = targetHeight;
}

#ifndef XT_GPU_NO_TEXT
void FrameBuilder::setGlyphAtlas(xt::text::GlyphAtlas* atlas) {
    if (atlas != m_impl->glyphAtlas) {
        m_impl->lastBuildValid = false;
        m_impl->prevBandsValid = false;  // T-343: a band may hold glyphs of the old atlas
    }
    m_impl->glyphAtlas = atlas;
}
#endif

void FrameBuilder::build(const CommandList& list, TextShaper* shaper, AssetResolver* assets, const IrView* ir,
                         GpuFrame& out, PerfTracker* perf, const DamageList* damage) {
    m_impl->build(list, shaper, assets, ir, out, perf, damage);
}

void FrameBuilder::invalidateAsset(AssetId asset, const IrView* source) {
    m_impl->invalidateAsset(asset, source);
}

void FrameBuilder::invalidateAllAssets() { m_impl->invalidateAllAssets(); }

void FrameBuilder::setEdgeFringe(bool enabled) {
    if (enabled != m_impl->edgeFringe) {
        m_impl->lastBuildValid = false;
        m_impl->prevBandsValid = false;  // T-343
    }
    m_impl->edgeFringe = enabled;
}

bool FrameBuilder::lastFrameReusable(AssetResolver* assets, const IrView* ir) const {
    return m_impl->lastFrameReusable(assets, ir);
}

const char* FrameBuilder::lastError() const { return m_impl->lastError.c_str(); }

}  // namespace xt::gpu
