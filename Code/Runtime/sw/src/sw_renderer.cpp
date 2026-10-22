#include "xt/sw/sw_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include <vector>

#include "effect_math.h"  // Code/Runtime/core/src: geometry and effect math shared with xt::gpu (T-414)
#include "sw_blend.h"
#include "sw_effects.h"
#include "sw_layer.h"
#include "sw_paint.h"
#include "sw_raster.h"
#include "sw_simd.h"
#include "xt/assert.h"
#include "xt/damage.h"
#include "xt/effect_utils.h"
#include "xt/geom.h"
#include "xt/num_guard.h"
#include "xt/project3d.h"

/// See Docs/design/10-backends.md §10.2, §10.2.2-§10.2.5, §10.4, Docs/tasks/T-11.md
/// "SwRenderer::render 流程", Docs/tasks/T-12.md; Docs/design/19-3d-space.md §19.7 (projection
/// commands, DrawImageQuad) and Docs/tasks/T-45.md.

namespace xt::sw {

// Defined in sw_glyphs.cpp (Docs/design/10-backends.md §10.2.6).
bool buildGlyphContours(TextShaper& shaper, const CommandList& list, const GlyphsCmd& cmd,
                        const Mat2x3& deviceXf, std::vector<Vec2>& outPoints,
                        std::vector<uint32_t>& outStarts);
// Also sw_glyphs.cpp: the small-size bitmap path (Docs/design/23-host-integration.md 23.4,
// Docs/tasks/T-101.md).
bool glyphBitmapPathApplies(const GlyphsCmd& cmd, const Mat2x3& deviceXf);
uint32_t drawGlyphBitmaps(TextShaper& shaper, const CommandList& list, const GlyphsCmd& cmd,
                          const Mat2x3& deviceXf, const Target& dst, const uint8_t* clipMask, int clipStride);
// Also sw_glyphs.cpp: T-178 glyph-level text effects (GlyphEffect, xt/command_list.h).
uint32_t drawGlyphEffect(TextShaper& shaper, const CommandList& list, const GlyphsCmd& cmd,
                         const Mat2x3& deviceXf, const Target& dst, const uint8_t* clipMask, int clipStride);

namespace {

/// Snaps `rect` (device px) outwards to whole pixels and clips it to a `width` x `height` target.
/// Returns false when nothing is left. Shared by SwRenderer::clearRects() and the damage write
/// mask so both cover exactly the same pixels (Docs/tasks/T-99.md).
bool snapRectToTarget(const Rect& rect, int width, int height, int& x0, int& y0, int& x1, int& y1) {
    if (rect.w <= 0 || rect.h <= 0) {
        return false;
    }
    x0 = std::clamp(static_cast<int>(std::floor(rect.x)), 0, width);
    y0 = std::clamp(static_cast<int>(std::floor(rect.y)), 0, height);
    x1 = std::clamp(static_cast<int>(std::ceil(rect.x + rect.w)), 0, width);
    y1 = std::clamp(static_cast<int>(std::ceil(rect.y + rect.h)), 0, height);
    return x1 > x0 && y1 > y0;
}

/// Index of the first byte in `[x, x1)` of `row` that differs from `value`, or `x1`. On the UIs
/// T-128 measures the clip mask is long runs of 255 (inside an `overflow: hidden` box) and long
/// runs of 0 (outside it), so the span painter asks this question once per run instead of testing
/// one byte per pixel; eight bytes at a time is what turns that test from per-pixel into
/// per-cache-line work (Docs/tasks/T-128.md step 4).
int maskRunEnd(const uint8_t* row, int x, int x1, uint8_t value) {
    uint64_t word = 0;
    std::memset(&word, value, sizeof(word));
    int i = x;
    for (; i + 8 <= x1; i += 8) {
        uint64_t w = 0;
        std::memcpy(&w, row + static_cast<size_t>(i), sizeof(w));
        if (w != word) {
            break;
        }
    }
    while (i < x1 && row[static_cast<size_t>(i)] == value) {
        ++i;
    }
    return i;
}

/// Half-open device-px box, the unit T-133's clip bounds are tracked in: `[x0, x1) x [y0, y1)`,
/// always clamped to the target, empty when either extent is non-positive.
struct Box {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;

    bool empty() const { return x1 <= x0 || y1 <= y0; }
};

/// The two boxes' intersection, normalized to `Box{}` when it is empty.
Box intersectBox(const Box& a, const Box& b) {
    Box r{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
    return r.empty() ? Box{} : r;
}

/// One `PushEffect`'s worth of state, stashed between the PushEffect and its matching PopEffect
/// (Docs/tasks/T-53b.md; `PopEffect` carries no `Command::index` of its own, mirroring PopLayer/
/// PopClip, so the converted parameters computed at Push time must be kept somewhere). Push/Pop
/// are always balanced for a well-formed CommandList (Docs/design/09-command-list.md §9.2), so a
/// plain LIFO vector suffices, same pattern as `mappings`/`clipStack` below.
struct EffectFrame {
    /// Every enabled effect on this node, already `xt::effectToScreen`-converted (screen/device
    /// px) in stack order.
    std::vector<EffectInstance> screenParams;
    /// Inverse of the node's dpr-scaled screen xf (identity if singular; Docs/tasks/T-53b.md,
    /// most conservative choice per Docs/design/14-coding-standards.md §14.8).
    Mat2x3 invScreenXf;
    /// The effect buffer's device-px origin (`LayerStack::topLayerOrigin()` right after the
    /// matching `pushEffectLayer`).
    Vec2 bufferOriginPx;
    /// `EffectCmd::contentBounds` scaled by dpr (device px, unshifted -- same frame as
    /// `bufferOriginPx` is added into).
    Rect contentBoundsPx;
    /// T-130 / T-343: when the stack's first effect is an enabled BackdropBlur, the blur pyramid
    /// over whatever lay under this node when `PushEffect` ran (`boundsHint` outset by the blur's
    /// 3 sigma read range and the pyramid margin, clamped to the target; 18 §18.15). Held per frame
    /// rather than in `EffectScratch` so that nesting one glass panel inside another keeps both.
    /// `cluster >= 0`: a top-level panel of a Direct frame shares its merged region's pyramid
    /// (SwRenderer::Impl::clusters) instead and `pyramid` stays empty.
    bool hasBackdrop = false;
    BackdropPyramid pyramid;
    int cluster = -1;
};

}  // namespace

struct SwRenderer::Impl {
    SwTarget target;
    Rasterizer raster;
    LayerStack layers;

    /// The frame record being filled, or null while tracing is off (Docs/tasks/T-82.md step 1).
    /// `timer()` turns a FrameStats member into the `float*` PerfFieldScope wants.
    FrameStats* stats = nullptr;
    float* timer(float FrameStats::* field) const { return stats != nullptr ? &(stats->*field) : nullptr; }
    EffectTimers effectTimers() const {
        EffectTimers t;
        t.blur = timer(&FrameStats::swFxBlurMs);
        t.dilate = timer(&FrameStats::swFxDilateMs);
        t.sdf = timer(&FrameStats::swFxSdfMs);
        t.pix = timer(&FrameStats::swFxPixMs);
        return t;
    }

    // Scratch buffers, reused across commands to avoid per-command allocation churn.
    std::vector<Vec2> flatPoints;
    std::vector<uint32_t> flatStarts;
    std::vector<uint8_t> flatClosed;
    std::vector<Vec2> dashPoints;
    std::vector<uint32_t> dashStarts;
    std::vector<uint8_t> dashClosed;
    std::vector<float> scaledDashes;
    std::vector<Vec2> strokeOutPoints;
    std::vector<uint32_t> strokeOutStarts;
    std::vector<Vec2> glyphPoints;
    std::vector<uint32_t> glyphStarts;
    /// One run of premultiplied gradient source pixels, handed to the T-102 span kernels
    /// (Docs/tasks/T-128.md step 2). Grown to the destination width on first use and kept.
    std::vector<float> spanScratch;
    // Docs/tasks/T-149.md: DrawImage/DrawImageQuad sample and row scratch, kept across draws.
    ImageScratch imageScratch;
    /// T-245: a DrawImageMesh's vertices in device coordinates, kept across draws.
    std::vector<MeshVertex> meshScratch;
    /// This render()'s expanded gradient ramps (Docs/tasks/T-128.md step 2).
    GradientLutCache gradientLuts;

    // PushEffect/PopEffect state (Docs/tasks/T-53b.md): one EffectFrame per active PushEffect,
    // innermost last; fxScratchA/B/Result are the applyEffectStack buffers, reused across
    // PopEffect calls (01.7's allowed exception -- resized, not reallocated, once warm).
    std::vector<EffectFrame> effectFrames;
    Surface fxScratchA;
    Surface fxScratchB;
    Surface fxResult;
    /// The BackdropBlur stage's output, i.e. `applyEffectStack`'s input when the node has one.
    Surface fxBackdropResult;
    /// Every heap buffer the effect passes themselves need, kept warm across frames
    /// (Docs/tasks/T-102b.md step 1).
    EffectScratch fxScratch;

    // Clip state (Docs/design/10-backends.md §10.2.3): one entry per active PushClipRect/
    // PushClipPath, innermost last; an empty stack means "no clip" (every pixel passes, lazily
    // avoiding an allocation of an all-255 buffer). Each layer's `mask` is
    // `target.width * target.height` bytes and -- the T-133 invariant every path below relies on
    // -- is zero everywhere outside `bounds`, so rasterizing, intersecting and clearing only ever
    // touch the rows inside a box instead of the whole viewport.
    struct ClipLayer {
        std::vector<uint8_t> mask;
        Box bounds;
    };
    std::vector<ClipLayer> clipStack;
    /// Clip buffers not currently on the stack. Every one of them is all-zero: popClip clears the
    /// rows its layer wrote before returning it here, so acquireMask never has to.
    std::vector<std::vector<uint8_t>> maskPool;

    // Partial redraw state (Docs/design/23-host-integration.md §23.3, Docs/tasks/T-99.md). Empty
    // `damageMask` means "this render() is a full one" and every path below behaves exactly as it
    // did before T-99. Otherwise `damageMask` is a target-sized 255/0 write mask (255 inside the
    // union of `damageRects`), `damageRects` are the same rectangles snapped to whole pixels (the
    // command-skipping test), and `combinedMask` caches `clipMask` intersected with `damageMask`
    // for draws that land on the root target -- rebuilt only when the clip stack changes, not per
    // draw. 255 (not 1) is what makes a masked-in pixel take the renderer's existing
    // `clip == 255` fast paths and stay byte-identical to a full render.
    bool partialRedraw = false;
    std::vector<uint8_t> damageMask;
    std::vector<Rect> damageRects;
    std::vector<uint8_t> combinedMask;
    bool combinedValid = false;
    uint32_t executedDraws = 0;
    uint32_t skippedDraws = 0;

    // Docs/design/19-3d-space.md §19.7's "plane to screen" mapping stack: one entry per active
    // PushProjection, innermost (current) last. Empty means "no projection active", in which case
    // every projected code path below degenerates to the pre-T-45 behavior exactly (deviceXf only,
    // no project3d call at all) so unprojected rendering stays byte-identical. Capacity retained
    // across render() calls (cleared at the top of each one; Docs/tasks/T-45.md verification note).
    std::vector<project3d::Mapping> mappings;
    float currentDpr = 1.0f;  ///< The CommandList's devicePixelRatio for the render in progress.

    // --- T-343 Direct target mode (Docs/design/23-host-integration.md §23.aa, 10 §10.9) -------
    // The host's canvas is its scene: never cleared here, read by the scene runs. The paint order
    // is split into bands (xt::scanSceneRuns) whose pixels are kept in CPU surfaces and pasted
    // back while nothing of them changed (the DamageList says so), the scene runs are drawn every
    // frame, and every top-level backdrop blur of a merged region samples one shared pyramid.
    bool directMode = false;
    bool directFallback = false;
    struct SwBand {
        Surface surface;  ///< the band's pixels (premultiplied), `w x h` at (originX, originY)
        int originX = 0;
        int originY = 0;
        Box box;  ///< the band's bounds in target px (empty: nothing visible)
        uint32_t cmdCount = 0;
        bool valid = false;  ///< `surface` holds the band
    };
    std::vector<SwBand> prevBands;
    std::vector<SwBand> curBands;
    std::vector<uint8_t> prevRunKinds;
    std::vector<uint8_t> curRunKinds;
    bool prevBandsValid = false;
    int bandTargetW = 0, bandTargetH = 0;  ///< the target size the bands were drawn for
    std::vector<SceneRun> runs;
    std::vector<Box> bandBoxes;
    struct Cluster {
        Box box;
        float maxSigma = 0.0f;
        int remaining = 0;
        bool built = false;
        BackdropPyramid pyramid;
    };
    std::vector<Cluster> clusters;
    std::vector<int> runCluster;
    int currentCluster = -1;
    const DamageList* directDamage = nullptr;
    uint32_t statBands = 0, statBandRedraws = 0, statBackdropCopyPx = 0, statPyramidLevels = 0;

    /// One command of `render()`: the partial-redraw skip test, then the command itself. Returns
    /// the index of the last command consumed (the matching pop of a skipped section).
    size_t runCommand(const CommandList& list, size_t ci, TextShaper* shaper, AssetResolver* assets,
                      const IrView* ir, float dpr) {
        const Command& cmd = list.commands[ci];
        // Docs/design/23-host-integration.md §23.3: only the five drawing commands are skippable;
        // every Push*/Pop* runs unconditionally so the clip/layer/projection stacks stay balanced.
        switch (cmd.kind) {
            case CmdKind::FillPath:
            case CmdKind::StrokePath:
            case CmdKind::DrawImage:
            case CmdKind::DrawGlyphs:
            case CmdKind::DrawImageQuad:
            case CmdKind::DrawParticles:
            case CmdKind::DrawImageMesh:
                if (damageSkips(list, cmd, dpr)) {
                    ++skippedDraws;
                    return ci;
                }
                ++executedDraws;
                break;
            default:
                break;
        }
        switch (cmd.kind) {
            case CmdKind::FillPath:
                fillPath(list, list.fills[cmd.index], dpr);
                break;
            case CmdKind::StrokePath:
                strokePath(list, list.strokes[cmd.index], dpr);
                break;
            case CmdKind::PushClipRect: {
                PerfFieldScope clipScope(timer(&FrameStats::swClipMs));
                const ClipRectCmd& c = list.clipRects[cmd.index];
                Mat2x3 deviceXf = mul(deviceScale(dpr), c.xf);
                std::vector<uint8_t> newMask = acquireMask();
                Box shape;
                if (!projecting()) {
                    shape = rasterizeRectMask(deviceXf, c.rect, newMask);
                } else {
                    // §19.7: "把矩形四角當四邊形路徑 -> 走 PushClipPath 分支".
                    Vec2 planeCorners[4] = {
                        apply(deviceXf, Vec2{c.rect.x, c.rect.y}),
                        apply(deviceXf, Vec2{c.rect.x + c.rect.w, c.rect.y}),
                        apply(deviceXf, Vec2{c.rect.x + c.rect.w, c.rect.y + c.rect.h}),
                        apply(deviceXf, Vec2{c.rect.x, c.rect.y + c.rect.h}),
                    };
                    shape = rasterizeProjectedQuadMask(planeCorners, newMask);
                }
                pushClipMask(std::move(newMask), shape);
                break;
            }
            case CmdKind::PushClipPath: {
                PerfFieldScope clipScope(timer(&FrameStats::swClipMs));
                const ClipPathCmd& c = list.clipPaths[cmd.index];
                Mat2x3 deviceXf = mul(deviceScale(dpr), c.xf);
                std::vector<uint8_t> newMask = acquireMask();
                Box shape;
                if (!projecting()) {
                    shape = rasterizePathMask(deviceXf, list, c.path, c.rule, newMask);
                } else {
                    shape = rasterizeProjectedPathMask(deviceXf, list, c.path, c.rule, newMask);
                }
                pushClipMask(std::move(newMask), shape);
                break;
            }
            case CmdKind::PopClip: {
                PerfFieldScope clipScope(timer(&FrameStats::swClipMs));
                popClip();
                break;
            }
            case CmdKind::PushLayer: {
                PerfFieldScope layerScope(timer(&FrameStats::swLayerMs));
                const LayerCmd& c = list.layers[cmd.index];
                // T-133: LayerCmd::boundsHint is the subtree's content AABB in world units,
                // already clamped to the viewport by the Painter; scaling by dpr puts it in the
                // same device-px space as the root target, exactly like PushEffect below.
                const Rect boundsPx = toTargetPx(c.boundsHint, dpr);
                if (sectionSkips(boundsPx, true)) {
                    return skipSection(list, ci, CmdKind::PushLayer, CmdKind::PopLayer);
                }
                layers.pushLayer(rootTarget(), c.opacity, c.blend, boundsPx);
                break;
            }
            case CmdKind::PopLayer: {
                PerfFieldScope layerScope(timer(&FrameStats::swLayerMs));
                layers.popLayer(rootTarget());
                break;
            }
            case CmdKind::PushMask: {
                PerfFieldScope layerScope(timer(&FrameStats::swLayerMs));
                const MaskCmd& c = list.masks[cmd.index];
                layers.pushMask(rootTarget(), c.mode);
                break;
            }
            case CmdKind::EndMaskContent: {
                PerfFieldScope layerScope(timer(&FrameStats::swLayerMs));
                layers.endMaskContent(rootTarget());
                break;
            }
            case CmdKind::PopMask: {
                PerfFieldScope layerScope(timer(&FrameStats::swLayerMs));
                layers.popMask(rootTarget());
                break;
            }
            // T-263d: an asset command's ids are those of its own source's IR -- the list's
            // CommandSource entry, or `ir` when the list names none (sourceIr()).
            case CmdKind::DrawImage: {
                const ImageCmd& c = list.images[cmd.index];
                drawImageCmd(list, c, dpr, assets, sourceIr(list, c.source, ir));
                break;
            }
            case CmdKind::DrawGlyphs: {
                PerfFieldScope textScope(stats != nullptr ? &stats->textMs : nullptr);
                drawGlyphsCmd(list, list.glyphRuns[cmd.index], dpr, shaper);
                break;
            }
            case CmdKind::PushProjection: {
                const ProjectionCmd& c = list.projections[cmd.index];
                project3d::Mapping mapping;
                mapping.hasProjection = true;
                mapping.viewProj = c.viewProj;
                mapping.rootXf = c.rootXf;
                mapping.planeWorld3 = mat4Identity();
                mappings.push_back(mapping);
                break;
            }
            case CmdKind::PopProjection:
                // A well-formed CommandList always balances Push/PopProjection (same rule as
                // PopClip above); XT_ASSERT + no-op in Release per
                // Docs/design/14-coding-standards.md §14.3.
                XT_ASSERT(!mappings.empty());
                if (!mappings.empty()) {
                    mappings.pop_back();
                }
                break;
            case CmdKind::PushTransform3D:
                // §19.7: "新命令在無投影狀態下收到 PushTransform3D -> 視為錯誤".
                XT_ASSERT(!mappings.empty());
                if (!mappings.empty()) {
                    mappings.back().planeWorld3 = list.transforms3[cmd.index].world3;
                }
                break;
            case CmdKind::PopTransform3D:
                XT_ASSERT(!mappings.empty());
                if (!mappings.empty()) {
                    mappings.back().planeWorld3 = mat4Identity();
                }
                break;
            case CmdKind::DrawImageQuad: {
                const ImageQuadCmd& c = list.imageQuads[cmd.index];
                drawImageQuadCmd(c, dpr, assets, sourceIr(list, c.source, ir));
                break;
            }
            case CmdKind::DrawParticles: {
                const ParticlesCmd& c = list.particles[cmd.index];
                drawParticlesCmd(list, c, dpr, assets, sourceIr(list, c.source, ir));
                break;
            }
            case CmdKind::DrawImageMesh: {
                const ImageMeshCmd& c = list.imageMeshes[cmd.index];
                drawImageMeshCmd(list, c, dpr, assets, sourceIr(list, c.source, ir));
                break;
            }
            // Docs/design/18-effects.md §18.1/§18.5, Docs/tasks/T-53b.md: PushEffect opens a
            // bounded offscreen layer (boundsHint x dpr, clamped to the viewport); PopEffect runs
            // the node's effect stack over it and composites the result back (src-over, opacity
            // 1, Normal). The node's own opacity/blend is handled by an outer PushLayer (§18.1),
            // unrelated to this pair.
            case CmdKind::PushEffect: {
                const EffectCmd& c = list.effectCmds[cmd.index];
                Mat2x3 screenXf = mul(deviceScale(dpr), c.xf);
                Mat2x3 invScreenXf;
                if (!invert(screenXf, invScreenXf)) {
                    // A degenerate (non-invertible) node xf: not covered by Docs/tasks/T-53b.md;
                    // most conservative choice per Docs/design/14-coding-standards.md §14.8 (fall
                    // back to identity rather than propagating NaNs into GradientMask/Shine).
                    invScreenXf = Mat2x3{};
                }
                Rect boundsPx = toTargetPx(c.boundsHint, dpr);
                if (sectionSkips(boundsPx, false)) {
                    return skipSection(list, ci, CmdKind::PushEffect, CmdKind::PopEffect);
                }

                EffectFrame ef;
                // Docs/design/18-effects.md §18.6: a leading BackdropBlur reads the target as it
                // stands right now -- before this node's own layer is opened, and before anything
                // drawn inside it. Under partial redraw the area outside the damage rectangles
                // still holds the previous frame's (identical) pixels, which is exactly what a
                // full redraw would have put there by this point in the command list.
                if (c.effectCount > 0) {
                    const EffectInstance& first = list.effects[c.effectOffset];
                    if (first.kind == EffectKind::BackdropBlur && first.enabled != 0) {
                        // T-343 (18 §18.15): a pyramid over the read region -- the merged region's
                        // shared one for a top-level panel of a Direct frame, else this node's own.
                        const EffectInstance screenFirst = effectToScreen(first, screenXf);
                        const float sigma = backdropSigma(screenFirst);
                        if (directMode && layers.empty() && currentCluster >= 0 &&
                            currentCluster < static_cast<int>(clusters.size())) {
                            Cluster& cluster = clusters[static_cast<size_t>(currentCluster)];
                            if (!cluster.built) {
                                buildBackdropPyramid(rootTarget(), cluster.box.x0, cluster.box.y0,
                                                     cluster.box.x1 - cluster.box.x0,
                                                     cluster.box.y1 - cluster.box.y0, cluster.maxSigma,
                                                     cluster.pyramid, effectTimers());
                                cluster.built = true;
                                statBackdropCopyPx += cluster.pyramid.copiedPx;
                                statPyramidLevels += static_cast<uint32_t>(cluster.pyramid.levels);
                            }
                            ef.cluster = currentCluster;
                            ef.hasBackdrop = cluster.pyramid.level[0].w > 0 && cluster.pyramid.level[0].h > 0;
                        } else {
                            const float outset = effectReadExpand(screenFirst) + pyramidMargin(sigma);
                            const Box region = paddedRegion(boundsPx, outset);
                            Target below = layers.currentTarget(rootTarget());
                            buildBackdropPyramid(below, region.x0, region.y0, region.x1 - region.x0,
                                                 region.y1 - region.y0, sigma, ef.pyramid, effectTimers());
                            ef.hasBackdrop = ef.pyramid.level[0].w > 0 && ef.pyramid.level[0].h > 0;
                            if (directMode && layers.empty()) {
                                statBackdropCopyPx += ef.pyramid.copiedPx;
                                statPyramidLevels += static_cast<uint32_t>(ef.pyramid.levels);
                            }
                        }
                    }
                }

                {
                    PerfFieldScope layerScope(timer(&FrameStats::swLayerMs));
                    layers.pushEffectLayer(rootTarget(), boundsPx);
                }

                ef.invScreenXf = invScreenXf;
                ef.bufferOriginPx = layers.topLayerOrigin();
                ef.contentBoundsPx = toTargetPx(c.contentBounds, dpr);
                ef.screenParams.reserve(std::min<size_t>(c.effectCount, list.effects.size()));
                for (uint32_t i = 0; i < c.effectCount; ++i) {
                    const EffectInstance& e = list.effects[c.effectOffset + i];
                    if (e.kind == EffectKind::Custom) {
                        // T-203 (Docs/design/18-effects.md §18.7): SW never runs the shader body;
                        // it applies the asset's `fallback` (nothing for `none` or a missing
                        // ShaderRef). T-263d: the shader id is this PushEffect's source's.
                        for (const ShaderRef& s : list.shaders) {
                            if (s.asset == e.shader && s.source == c.source &&
                                s.fallback.kind != EffectKind::None &&
                                s.fallback.kind != EffectKind::Custom) {
                                EffectInstance fb = effectToScreen(s.fallback, screenXf);
                                fb.enabled = 1;
                                ef.screenParams.push_back(fb);
                                break;
                            }
                        }
                        continue;
                    }
                    ef.screenParams.push_back(effectToScreen(e, screenXf));
                }
                effectFrames.push_back(std::move(ef));
                break;
            }
            case CmdKind::PopEffect: {
                PerfFieldScope effectScope(stats != nullptr ? &stats->effectsMs : nullptr);
                // A well-formed CommandList always balances Push/PopEffect (same rule as
                // PopClip/PopProjection above).
                XT_ASSERT(!effectFrames.empty());
                if (effectFrames.empty()) {
                    break;
                }
                EffectFrame ef = std::move(effectFrames.back());
                effectFrames.pop_back();
                Surface& raw = layers.topLayerSurface();
                // The BackdropBlur stage (always the stack's first, §18.6) runs first and by
                // itself: it needs the snapshot taken at PushEffect time as a second input.
                const Surface* content = &raw;
                std::span<const EffectInstance> rest = ef.screenParams;
                if (ef.hasBackdrop && !rest.empty()) {
                    const BackdropPyramid& pyr =
                        (ef.cluster >= 0 && ef.cluster < static_cast<int>(clusters.size()))
                            ? clusters[static_cast<size_t>(ef.cluster)].pyramid
                            : ef.pyramid;
                    applyBackdropBlur(rest.front(), pyr, ef.bufferOriginPx, raw, fxBackdropResult, fxScratch,
                                      effectTimers());
                    content = &fxBackdropResult;
                    rest = rest.subspan(1);
                }
                applyEffectStack(rest, list.stops, ef.invScreenXf, ef.bufferOriginPx, ef.contentBoundsPx,
                                 *content, fxScratchA, fxScratchB, fxResult, fxScratch, effectTimers());
                {
                    PerfFieldScope layerScope(timer(&FrameStats::swLayerMs));
                    layers.popEffectLayer(rootTarget(), fxResult);
                }
                break;
            }
        }
        return ci;
    }

    // --- T-343 Direct target mode --------------------------------------------------------------

    static float backdropSigma(const EffectInstance& screenParams) {
        return effect_math::backdropSigma(screenParams);
    }
    static float pyramidMargin(float sigma) { return effect_math::pyramidMargin(sigma); }

    /// A read region `boundsPx` outset by `outset`, clamped to the target and padded exactly as
    /// the GPU builder pads a transient window (frame_emit.h: a multiple of 32, at least 64,
    /// never past the target, slid left / up to fit), so both backends downsample the same grid.
    Box paddedRegion(const Rect& boundsPx, float outset) const {
        const Target root = rootTarget();
        Box r;
        r.x0 = std::clamp(static_cast<int>(std::floor(boundsPx.x - outset)), 0, root.w);
        r.y0 = std::clamp(static_cast<int>(std::floor(boundsPx.y - outset)), 0, root.h);
        r.x1 = std::clamp(static_cast<int>(std::ceil(boundsPx.x + boundsPx.w + outset)), 0, root.w);
        r.y1 = std::clamp(static_cast<int>(std::ceil(boundsPx.y + boundsPx.h + outset)), 0, root.h);
        if (r.empty()) {
            return Box{};
        }
        auto pad = [](int v) { return (std::max(v, 64) + 31) / 32 * 32; };
        const int w = std::min(pad(r.x1 - r.x0), std::max(1, root.w));
        const int h = std::min(pad(r.y1 - r.y0), std::max(1, root.h));
        const int x = std::clamp(r.x0, 0, std::max(0, root.w - w));
        const int y = std::clamp(r.y0, 0, std::max(0, root.h - h));
        return Box{x, y, x + w, y + h};
    }

    /// The bounds of run `run`'s commands in target px (xt::commandBounds, the rectangles the
    /// damage was computed from), snapped outwards and clamped; empty when nothing draws.
    Box runBox(const CommandList& list, const SceneRun& run, float dpr) const {
        bool any = false;
        Rect u{};
        for (uint32_t i = run.first; i < run.end && i < list.commands.size(); ++i) {
            const Rect r = commandBounds(list, list.commands[i]);
            if (!(r.w > 0.0f) || !(r.h > 0.0f)) {
                continue;
            }
            if (!any) {
                u = r;
                any = true;
            } else {
                const float x0 = std::min(u.x, r.x), y0 = std::min(u.y, r.y);
                const float x1 = std::max(u.x + u.w, r.x + r.w), y1 = std::max(u.y + u.h, r.y + r.h);
                u = Rect{x0, y0, x1 - x0, y1 - y0};
            }
        }
        if (!any) {
            return Box{};
        }
        const Rect px = toTargetPx(u, dpr);
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        if (!snapRectToTarget(px, target.width, target.height, x0, y0, x1, y1)) {
            return Box{};
        }
        return Box{x0, y0, x1, y1};
    }

    /// True when this frame's damage (device px) touches `box` (target px); null or full damage
    /// touches everything.
    bool damageTouches(const Box& box) const {
        if (directDamage == nullptr || directDamage->full) {
            return true;
        }
        for (const Rect& dr : directDamage->rects) {
            const Rect r = deviceToTarget(dr);
            if (r.x < static_cast<float>(box.x1) && r.x + r.w > static_cast<float>(box.x0) &&
                r.y < static_cast<float>(box.y1) && r.y + r.h > static_cast<float>(box.y0)) {
                return true;
            }
        }
        return false;
    }

    /// A reused band's commands, for their effect on the stacks only: the depth-0 clip and
    /// projection commands (a clip opened in this band applies to later runs); its draws count
    /// as skipped.
    void dryWalk(const CommandList& list, const SceneRun& run, TextShaper* shaper, AssetResolver* assets,
                 const IrView* ir, float dpr) {
        int depth = 0;
        for (uint32_t ci = run.first; ci < run.end && ci < list.commands.size(); ++ci) {
            const Command& cmd = list.commands[ci];
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
                case CmdKind::PushClipPath:
                case CmdKind::PopClip:
                case CmdKind::PushProjection:
                case CmdKind::PopProjection:
                case CmdKind::PushTransform3D:
                case CmdKind::PopTransform3D:
                    if (depth == 0) {
                        runCommand(list, ci, shaper, assets, ir, dpr);
                    }
                    break;
                case CmdKind::FillPath:
                case CmdKind::StrokePath:
                case CmdKind::DrawImage:
                case CmdKind::DrawGlyphs:
                case CmdKind::DrawImageQuad:
                case CmdKind::DrawParticles:
                case CmdKind::DrawImageMesh:
                    ++skippedDraws;
                    break;
                default:
                    break;
            }
        }
    }

    /// One band: pasted from its kept surface when nothing of it changed, else drawn into a
    /// bounded layer, kept (unless this frame fell back) and composited.
    void drawBandRun(const CommandList& list, size_t runIndex, size_t bandIndex, TextShaper* shaper,
                     AssetResolver* assets, const IrView* ir, float dpr) {
        const SceneRun& run = runs[runIndex];
        SwBand& band = curBands[bandIndex];
        band.box = bandBoxes[runIndex];
        band.cmdCount = run.end - run.first;
        band.valid = false;
        if (band.box.empty()) {
            dryWalk(list, run, shaper, assets, ir, dpr);
            return;
        }
        const bool structureSame =
            prevBandsValid && prevRunKinds == curRunKinds && bandIndex < prevBands.size();
        SwBand* prev = structureSame ? &prevBands[bandIndex] : nullptr;
        if (!directFallback && prev != nullptr && prev->valid && prev->box.x0 == band.box.x0 &&
            prev->box.y0 == band.box.y0 && prev->box.x1 == band.box.x1 && prev->box.y1 == band.box.y1 &&
            prev->cmdCount == band.cmdCount && !damageTouches(band.box)) {
            PerfFieldScope layerScope(timer(&FrameStats::swLayerMs));
            layers.compositeSurface(rootTarget(), prev->surface, prev->originX, prev->originY);
            std::swap(band.surface, prev->surface);  // taken over (prev is dropped at the end)
            band.originX = prev->originX;
            band.originY = prev->originY;
            band.valid = true;
            prev->valid = false;
            dryWalk(list, run, shaper, assets, ir, dpr);
            return;
        }
        ++statBandRedraws;
        const Rect boundsPx{static_cast<float>(band.box.x0), static_cast<float>(band.box.y0),
                            static_cast<float>(band.box.x1 - band.box.x0),
                            static_cast<float>(band.box.y1 - band.box.y0)};
        {
            PerfFieldScope layerScope(timer(&FrameStats::swLayerMs));
            layers.pushLayer(rootTarget(), 1.0f, BlendMode::Normal, boundsPx);
        }
        for (size_t ci = run.first; ci < run.end && ci < list.commands.size(); ++ci) {
            ci = runCommand(list, ci, shaper, assets, ir, dpr);
        }
        PerfFieldScope layerScope(timer(&FrameStats::swLayerMs));
        if (!directFallback) {
            const Surface& drawn = layers.topLayerSurface();
            const Vec2 origin = layers.topLayerOrigin();
            band.surface = drawn;
            band.originX = static_cast<int>(origin.x);
            band.originY = static_cast<int>(origin.y);
            band.valid = true;
        }
        layers.popLayer(rootTarget());
    }

    void renderDirect(const CommandList& list, TextShaper* shaper, AssetResolver* assets, const IrView* ir,
                      float dpr) {
        if (bandTargetW != target.width || bandTargetH != target.height) {
            prevBands.clear();
            prevRunKinds.clear();
            prevBandsValid = false;
            bandTargetW = target.width;
            bandTargetH = target.height;
        }
        curBands.clear();
        curRunKinds.clear();
        clusters.clear();
        runCluster.clear();
        currentCluster = -1;
        scanSceneRuns(list, runs);
        bandBoxes.assign(runs.size(), Box{});
        runCluster.assign(runs.size(), -1);
        size_t bandCount = 0;
        double bandArea = 0.0;
        for (size_t i = 0; i < runs.size(); ++i) {
            curRunKinds.push_back(runs[i].sceneRead ? 1 : 0);
            if (runs[i].sceneRead) {
                continue;
            }
            ++bandCount;
            bandBoxes[i] = runBox(list, runs[i], dpr);
            bandArea += static_cast<double>(bandBoxes[i].x1 - bandBoxes[i].x0) *
                        static_cast<double>(bandBoxes[i].y1 - bandBoxes[i].y0);
        }
        const double surfaceArea =
            static_cast<double>(std::max(1, target.width)) * static_cast<double>(std::max(1, target.height));
        directFallback = bandCount > 4 || bandArea > 2.0 * surfaceArea;
        statBands = static_cast<uint32_t>(bandCount);
        curBands.resize(bandCount);

        // §23.aa.3: the read regions of the top-level backdrop-blur runs, merged where they meet.
        for (size_t i = 0; i < runs.size(); ++i) {
            const EffectCmd* lead = effect_math::leadingBackdropBlur(list, runs[i]);
            if (lead == nullptr) {
                continue;
            }
            const EffectCmd& c = *lead;
            const EffectInstance& first = list.effects[c.effectOffset];
            const EffectInstance screenFirst = effectToScreen(first, mul(deviceScale(dpr), c.xf));
            const float sigma = backdropSigma(screenFirst);
            const Box region = paddedRegion(toTargetPx(c.boundsHint, dpr),
                                            effectReadExpand(screenFirst) + pyramidMargin(sigma));
            if (region.empty()) {
                continue;
            }
            Cluster cl;
            cl.box = region;
            cl.maxSigma = sigma;
            cl.remaining = 1;
            clusters.push_back(cl);
            runCluster[i] = static_cast<int>(clusters.size()) - 1;
        }
        auto meets = [](const Box& a, const Box& b) {
            return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
        };
        for (bool merged = true; merged;) {
            merged = false;
            for (size_t a = 0; a < clusters.size() && !merged; ++a) {
                for (size_t b = a + 1; b < clusters.size(); ++b) {
                    if (!meets(clusters[a].box, clusters[b].box)) {
                        continue;
                    }
                    clusters[a].box = Box{std::min(clusters[a].box.x0, clusters[b].box.x0),
                                          std::min(clusters[a].box.y0, clusters[b].box.y0),
                                          std::max(clusters[a].box.x1, clusters[b].box.x1),
                                          std::max(clusters[a].box.y1, clusters[b].box.y1)};
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
        for (Cluster& cl : clusters) {
            // The merged box, padded like a transient window (as the GPU builder does).
            const Rect r{static_cast<float>(cl.box.x0), static_cast<float>(cl.box.y0),
                         static_cast<float>(cl.box.x1 - cl.box.x0),
                         static_cast<float>(cl.box.y1 - cl.box.y0)};
            cl.box = paddedRegion(r, 0.0f);
        }

        size_t bandIndex = 0;
        for (size_t i = 0; i < runs.size(); ++i) {
            const SceneRun& run = runs[i];
            if (!run.sceneRead) {
                drawBandRun(list, i, bandIndex++, shaper, assets, ir, dpr);
                continue;
            }
            currentCluster = runCluster[i];
            for (size_t ci = run.first; ci < run.end && ci < list.commands.size(); ++ci) {
                ci = runCommand(list, ci, shaper, assets, ir, dpr);
            }
            if (currentCluster >= 0 && currentCluster < static_cast<int>(clusters.size())) {
                Cluster& cl = clusters[static_cast<size_t>(currentCluster)];
                if (--cl.remaining <= 0) {
                    cl.pyramid = BackdropPyramid{};
                    cl.built = false;
                }
            }
            currentCluster = -1;
        }
        prevBands.swap(curBands);
        curBands.clear();
        prevRunKinds.swap(curRunKinds);
        curRunKinds.clear();
        prevBandsValid = true;
        clusters.clear();
    }

    Target rootTarget() const { return Target::fromSwTarget(target); }

    /// T-239 (item 5): canvas logical px -> target px: the device pixel ratio, then the target's
    /// origin subtracted (a no-op at origin 0/0, so whole-canvas output is unchanged).
    Mat2x3 deviceScale(float dpr) const {
        Mat2x3 m = scaling(Vec2{dpr, dpr});
        m.e -= static_cast<float>(target.originX);
        m.f -= static_cast<float>(target.originY);
        return m;
    }
    /// A canvas logical-px rectangle in target px (see deviceScale()).
    Rect toTargetPx(const Rect& r, float dpr) const {
        return Rect{r.x * dpr - static_cast<float>(target.originX),
                    r.y * dpr - static_cast<float>(target.originY), r.w * dpr, r.h * dpr};
    }
    /// A canvas device-px rectangle in target px.
    Rect deviceToTarget(const Rect& r) const {
        return Rect{r.x - static_cast<float>(target.originX), r.y - static_cast<float>(target.originY), r.w,
                    r.h};
    }

    /// Installs (or clears) this render()'s partial-redraw state. A null or `full` DamageList
    /// leaves every member empty, which is exactly the pre-T-99 behavior.
    void beginDamage(const DamageList* damage) {
        executedDraws = 0;
        skippedDraws = 0;
        partialRedraw = false;
        damageMask.clear();
        damageRects.clear();
        combinedValid = false;
        layers.setRootWriteMask(nullptr, 0);
        if (damage == nullptr || damage->full || target.width <= 0 || target.height <= 0) {
            return;
        }
        // An empty rectangle list means "nothing changed on screen": every draw is then skipped
        // and the all-zero mask below blocks anything that is not skippable, so the target keeps
        // the previous frame untouched.
        partialRedraw = true;
        damageMask.assign(static_cast<size_t>(target.width) * static_cast<size_t>(target.height), 0);
        for (const Rect& rect : damage->rects) {
            int x0 = 0;
            int y0 = 0;
            int x1 = 0;
            int y1 = 0;
            if (!snapRectToTarget(deviceToTarget(rect), target.width, target.height, x0, y0, x1, y1)) {
                continue;
            }
            damageRects.push_back(Rect{static_cast<float>(x0), static_cast<float>(y0),
                                       static_cast<float>(x1 - x0), static_cast<float>(y1 - y0)});
            for (int y = y0; y < y1; ++y) {
                uint8_t* row = damageMask.data() + static_cast<size_t>(y) * static_cast<size_t>(target.width);
                for (int x = x0; x < x1; ++x) {
                    row[x] = 255;
                }
            }
        }
        layers.setRootWriteMask(damageMask.data(), target.width);
    }

    /// Drops the partial-redraw state so the next render() (and any stray LayerStack use) is a
    /// full one again; the buffers keep their capacity.
    void endDamage() {
        partialRedraw = false;
        damageMask.clear();
        damageRects.clear();
        combinedValid = false;
        layers.setRootWriteMask(nullptr, 0);
    }

    bool projecting() const { return !mappings.empty(); }

    /// Projects every point of `pts` in place (already in "plane space", i.e. deviceXf has already
    /// been applied) through the current mapping stack. Returns false (leaving `pts` partially
    /// projected -- callers must then discard the whole shape) as soon as one point's homogeneous
    /// divide fails, per §19.7 "任何點 ok == false → 整條路徑丟棄".
    /// The projection stack (viewProj, rootXf, planeWorld3) is expressed in logical px, while every
    /// point this renderer projects has already been scaled by the device pixel ratio. Projecting
    /// the scaled point directly put every 3D layer at the wrong place whenever dpr != 1 (a root at
    /// x = 48 landed at device x = 48 instead of 96, and the perspective distance was off by the
    /// same factor), so: back to logical px, project, then scale to device px again.
    Vec2 projectDevicePoint(Vec2 devicePoint, bool& ok) const {
        const float dpr = currentDpr > 0.0f ? currentDpr : 1.0f;
        // T-239: plane space is target px; the projection works on the whole canvas.
        const float ox = static_cast<float>(target.originX);
        const float oy = static_cast<float>(target.originY);
        const Vec2 logical{(devicePoint.x + ox) / dpr, (devicePoint.y + oy) / dpr};
        const Vec2 projected = project3d::projectPoint(mappings, Mat2x3{}, logical, ok);
        return Vec2{projected.x * dpr - ox, projected.y * dpr - oy};
    }

    /// T-160: the homography taking a projected device pixel back into plane space, for a
    /// gradient paint drawn under a projection (19.7 flattens in plane space, then projects each
    /// point, so the paint's own coordinates are plane coordinates). Built from the plane-space
    /// AABB of `planePts` (the shape before projection) and its four projected corners -- a plane
    /// maps to the screen through one homography, so any four non-collinear points pin it down.
    /// Returns false (leave the paint unprojected) when the box is degenerate or a corner fails to
    /// project.
    bool screenToPlaneHomography(std::span<const Vec2> planePts, Mat3& out) const {
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
        return homographyFromQuad(screen, plane, out);
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

    /// §19.7's FillPath/StrokePath/PushClipPath flattening tolerance: kFlattenTolerance when no
    /// projection is active (unchanged), else divided by projectedScaleMax so the *projected*
    /// (device-space) chord error stays ~kFlattenTolerance. `deviceXf` is the command's own
    /// dpr-scaled xf (the flattening transform); `rawPoints` is the unflattened path's point pool
    /// (fed to boundsOfPoints for the tolerance estimate only).
    float flattenTolerance(const Mat2x3& deviceXf, std::span<const float> rawPoints) const {
        return effect_math::flattenTolerance(mappings, deviceXf, rawPoints);
    }

    /// The clip byte array that applies to the draw about to be issued: the plain clip mask,
    /// except that a draw landing on the root target while a partial redraw is in progress also
    /// gets the damage write mask folded in. Null means "no clip at all".
    /// The innermost clip layer's mask, or null when nothing is clipped.
    const uint8_t* clipData() const { return clipStack.empty() ? nullptr : clipStack.back().mask.data(); }

    /// The device-px box outside which the current clip is known to be zero; the whole target when
    /// nothing is clipped.
    Box clipBounds() const {
        return clipStack.empty() ? Box{0, 0, target.width, target.height} : clipStack.back().bounds;
    }

    /// Checks out an all-zero target-sized clip buffer (see `maskPool`).
    std::vector<uint8_t> acquireMask() {
        const size_t n = static_cast<size_t>(target.width) * static_cast<size_t>(target.height);
        if (!maskPool.empty()) {
            std::vector<uint8_t> m = std::move(maskPool.back());
            maskPool.pop_back();
            if (m.size() != n) {
                m.assign(n, 0);  // The target was resized while this buffer sat in the pool.
            }
            return m;
        }
        return std::vector<uint8_t>(n, 0);
    }

    const uint8_t* activeClipData() {
        const uint8_t* plain = clipData();
        // Offscreen layer/mask/effect buffers are computed in full; only their composite back onto
        // the root is masked (LayerStack::setRootWriteMask).
        if (!partialRedraw || !layers.empty()) {
            return plain;
        }
        if (plain == nullptr) {
            return damageMask.data();
        }
        if (!combinedValid) {
            // Outside the clip's box the clip mask is zero, so the intersection is zero there too:
            // only the box's rows need the per-byte min (T-133).
            combinedMask.assign(static_cast<size_t>(target.width) * static_cast<size_t>(target.height), 0);
            const Box b = clipBounds();
            for (int y = b.y0; y < b.y1; ++y) {
                const size_t off = static_cast<size_t>(y) * static_cast<size_t>(target.width);
                for (int x = b.x0; x < b.x1; ++x) {
                    const size_t i = off + static_cast<size_t>(x);
                    combinedMask[i] = std::min(plain[i], damageMask[i]);
                }
            }
            combinedValid = true;
        }
        return combinedMask.data();
    }

    /// True when `cmd` draws entirely outside every damage rectangle and can be skipped. Only
    /// draws that land on the root target are ever skipped: content inside an offscreen buffer
    /// must stay complete (an effect reads its neighborhood), and under an active projection the
    /// world-space commandBounds() no longer bounds the command on screen, so both cases answer
    /// "do not skip" (the conservative option, Docs/design/14-coding-standards.md §14.8).
    /// T-239 (item 7): true when a root-level PushLayer / PushEffect section can be skipped as a
    /// whole under partial redraw: the offscreen buffer it would composite back (`boundsPx`
    /// snapped and clamped exactly as LayerStack does, including pushLayer's 1x1 stand-in for an
    /// empty hint when `layerFallback`) misses every damage rectangle, so nothing it draws can
    /// reach a pixel the root write mask lets through. Nested sections (inside another offscreen
    /// buffer, which an effect may read in full) and projected ones are never skipped, the same
    /// rule damageSkips() follows.
    bool sectionSkips(const Rect& boundsPx, bool layerFallback) const {
        if (!partialRedraw || !layers.empty() || projecting()) {
            return false;
        }
        const int w = target.width;
        const int h = target.height;
        int x0 = std::clamp(static_cast<int>(std::floor(boundsPx.x)), 0, w);
        int y0 = std::clamp(static_cast<int>(std::floor(boundsPx.y)), 0, h);
        int x1 = std::clamp(static_cast<int>(std::ceil(boundsPx.x + boundsPx.w)), 0, w);
        int y1 = std::clamp(static_cast<int>(std::ceil(boundsPx.y + boundsPx.h)), 0, h);
        if (x1 <= x0 || y1 <= y0) {
            if (!layerFallback) {
                return true;  // An empty effect buffer composites nothing.
            }
            x0 = std::min(x0, std::max(0, w - 1));
            y0 = std::min(y0, std::max(0, h - 1));
            x1 = x0 + 1;
            y1 = y0 + 1;
        }
        for (const Rect& r : damageRects) {
            if (static_cast<float>(x0) < r.x + r.w && r.x < static_cast<float>(x1) &&
                static_cast<float>(y0) < r.y + r.h && r.y < static_cast<float>(y1)) {
                return false;
            }
        }
        return true;
    }

    /// T-239 (item 7): the index of the command that closes the section `list.commands[open]`
    /// opens (`pushKind` ... matching `popKind`), counting every draw command inside it as skipped.
    /// A list that never closes it (malformed) ends the scan at the last command.
    size_t skipSection(const CommandList& list, size_t open, CmdKind pushKind, CmdKind popKind) {
        int depth = 0;
        size_t i = open;
        for (; i < list.commands.size(); ++i) {
            const CmdKind kind = list.commands[i].kind;
            if (kind == pushKind) {
                ++depth;
            } else if (kind == popKind) {
                if (--depth == 0) {
                    return i;
                }
            } else if (kind == CmdKind::FillPath || kind == CmdKind::StrokePath ||
                       kind == CmdKind::DrawImage || kind == CmdKind::DrawGlyphs ||
                       kind == CmdKind::DrawImageQuad || kind == CmdKind::DrawParticles ||
                       kind == CmdKind::DrawImageMesh) {
                ++skippedDraws;
            }
        }
        XT_ASSERT(false);
        return list.commands.size() - 1;
    }

    bool damageSkips(const CommandList& list, const Command& cmd, float dpr) {
        if (!partialRedraw || !layers.empty() || projecting()) {
            return false;
        }
        Rect box = commandBounds(list, cmd);
        if (box.w <= 0 || box.h <= 0) {
            return false;
        }
        const float ox = static_cast<float>(target.originX);
        const float oy = static_cast<float>(target.originY);
        const float x0 = std::floor(box.x * dpr) - ox;
        const float y0 = std::floor(box.y * dpr) - oy;
        const float x1 = std::ceil((box.x + box.w) * dpr) - ox;
        const float y1 = std::ceil((box.y + box.h) * dpr) - oy;
        for (const Rect& r : damageRects) {
            if (x0 < r.x + r.w && r.x < x1 && y0 < r.y + r.h && r.y < y1) {
                return false;
            }
        }
        return true;
    }

    uint8_t clipAt(int x, int y) const {
        const uint8_t* m = clipData();
        if (m == nullptr) {
            return 255;
        }
        return m[static_cast<size_t>(y) * static_cast<size_t>(target.width) + static_cast<size_t>(x)];
    }

    /// Adds every input contour's edges to `raster`, unconditionally closing each one (a fill or a
    /// clip mask always treats its shape as closed; Docs/tasks/T-11.md "含閉合段：若 closed==0 也
    /// 視為閉合"). `raster.reset` must already have been called by the caller.
    /// Device-px box (rounded outwards, clamped to the target) covering every point of an
    /// already-flattened, already-projected contour set. Empty when there are no finite points
    /// (T-258a: a non-finite point is left out, as the rasterizer drops its edges; the int
    /// conversion saturates).
    Box devicePointsBox(const std::vector<Vec2>& points) const {
        bool any = false;
        float minX = 0;
        float maxX = 0;
        float minY = 0;
        float maxY = 0;
        for (const Vec2& p : points) {
            if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
                continue;
            }
            if (!any) {
                minX = maxX = p.x;
                minY = maxY = p.y;
                any = true;
            }
            minX = std::min(minX, p.x);
            maxX = std::max(maxX, p.x);
            minY = std::min(minY, p.y);
            maxY = std::max(maxY, p.y);
        }
        if (!any) {
            return Box{};
        }
        Box b{toInt32Sat(std::floor(minX)), toInt32Sat(std::floor(minY)),
              toInt32Sat(static_cast<double>(std::ceil(maxX)) + 1.0),
              toInt32Sat(static_cast<double>(std::ceil(maxY)) + 1.0)};
        return intersectBox(b, Box{0, 0, target.width, target.height});
    }

    void addClosedContours(const std::vector<Vec2>& points, const std::vector<uint32_t>& starts) {
        for (size_t i = 0; i < starts.size(); ++i) {
            uint32_t start = starts[i];
            uint32_t end = (i + 1 < starts.size()) ? starts[i + 1] : static_cast<uint32_t>(points.size());
            if (end - start < 2) {
                continue;  // a single-point contour has no edges to fill.
            }
            for (uint32_t j = start; j + 1 < end; ++j) {
                raster.addLine(points[j], points[j + 1]);
            }
            raster.addLine(points[end - 1], points[start]);
        }
    }

    /// Sweeps the rasterizer's accumulated shape into `outMask` (which must be all-zero on entry,
    /// as every buffer from `acquireMask` is) and returns the box it wrote, clamped to the target.
    /// Zero-coverage spans are skipped: writing 0 over a 0 is what they did before (T-133).
    Box sweepIntoMask(FillRule rule, std::vector<uint8_t>& outMask) {
        const int w = target.width;
        Box b{w, target.height, 0, 0};
        raster.sweep(rule, [&](int y, int x0, int x1, uint8_t coverage) {
            if (coverage == 0) {
                return;
            }
            uint8_t* rowStart = outMask.data() + static_cast<size_t>(y) * static_cast<size_t>(w);
            for (int x = x0; x < x1; ++x) {
                rowStart[x] = coverage;
            }
            b.x0 = std::min(b.x0, x0);
            b.x1 = std::max(b.x1, x1);
            b.y0 = std::min(b.y0, y);
            b.y1 = std::max(b.y1, y + 1);
        });
        return b.empty() ? Box{} : b;
    }

    Box rasterizeRectMask(const Mat2x3& deviceXf, const Rect& rect, std::vector<uint8_t>& outMask) {
        Vec2 corners[4] = {
            apply(deviceXf, Vec2{rect.x, rect.y}),
            apply(deviceXf, Vec2{rect.x + rect.w, rect.y}),
            apply(deviceXf, Vec2{rect.x + rect.w, rect.y + rect.h}),
            apply(deviceXf, Vec2{rect.x, rect.y + rect.h}),
        };
        raster.reset(target.width, target.height);
        for (int i = 0; i < 4; ++i) {
            raster.addLine(corners[i], corners[(i + 1) % 4]);
        }
        return sweepIntoMask(FillRule::NonZero, outMask);
    }

    Box rasterizePathMask(const Mat2x3& deviceXf, const CommandList& list, const PathRef& path, FillRule rule,
                          std::vector<uint8_t>& outMask) {
        std::span<const PathVerb> verbs(list.verbs.data() + path.verbOffset, path.verbCount);
        std::span<const float> points(list.points.data() + path.pointOffset, path.pointCount);
        flatPoints.clear();
        flatStarts.clear();
        flatClosed.clear();
        flattenPath(verbs, points, deviceXf, kFlattenTolerance, flatPoints, flatStarts, flatClosed);

        raster.reset(target.width, target.height);
        addClosedContours(flatPoints, flatStarts);
        return sweepIntoMask(rule, outMask);
    }

    /// §19.7 PushClipRect under projection: "把矩形四角當四邊形路徑 → 走 PushClipPath 分支".
    /// `planeCorners` are the rect's 4 corners already through deviceXf (plane space, pre-
    /// projection), clockwise. A degenerate projection (any corner's ok == false) drops the whole
    /// shape -- conservative choice per Docs/design/14-coding-standards.md §14.8: the clip then
    /// passes nothing rather than everything.
    Box rasterizeProjectedQuadMask(const Vec2 planeCorners[4], std::vector<uint8_t>& outMask) {
        Vec2 q[4];
        for (int i = 0; i < 4; ++i) {
            bool ok = false;
            q[i] = projectDevicePoint(planeCorners[i], ok);
            if (!ok) {
                return Box{};  // Mask stays all-zero.
            }
        }
        raster.reset(target.width, target.height);
        for (int i = 0; i < 4; ++i) {
            raster.addLine(q[i], q[(i + 1) % 4]);
        }
        return sweepIntoMask(FillRule::NonZero, outMask);
    }

    /// §19.7 PushClipPath under projection: same flattening/tolerance rule as FillPath, then
    /// projected before rasterizing. See rasterizeProjectedQuadMask's degenerate-shape note.
    Box rasterizeProjectedPathMask(const Mat2x3& deviceXf, const CommandList& list, const PathRef& path,
                                   FillRule rule, std::vector<uint8_t>& outMask) {
        std::span<const PathVerb> verbs(list.verbs.data() + path.verbOffset, path.verbCount);
        std::span<const float> points(list.points.data() + path.pointOffset, path.pointCount);
        float tol = flattenTolerance(deviceXf, points);
        flatPoints.clear();
        flatStarts.clear();
        flatClosed.clear();
        flattenPath(verbs, points, deviceXf, tol, flatPoints, flatStarts, flatClosed);
        if (!projectPointsInPlace(flatPoints)) {
            return Box{};  // Mask stays all-zero.
        }
        raster.reset(target.width, target.height);
        addClosedContours(flatPoints, flatStarts);
        return sweepIntoMask(rule, outMask);
    }

    /// Pushes a clip layer whose freshly rasterized shape occupies `shape` in `newMask` (from
    /// `acquireMask`, so zero outside `shape`). The layer's box is `shape` intersected with the
    /// enclosing layer's, and the per-byte min with the enclosing mask runs only inside that
    /// intersection; what `shape` covers outside it is cleared, restoring the "zero outside the
    /// box" invariant (T-133).
    void pushClipMask(std::vector<uint8_t>&& newMask, Box shape) {
        combinedValid = false;
        const Box bounds = intersectBox(shape, clipBounds());
        if (!clipStack.empty()) {
            const int w = target.width;
            const uint8_t* prev = clipStack.back().mask.data();
            for (int y = shape.y0; y < shape.y1; ++y) {
                const size_t off = static_cast<size_t>(y) * static_cast<size_t>(w);
                uint8_t* row = newMask.data() + off;
                if (y < bounds.y0 || y >= bounds.y1) {
                    std::memset(row + shape.x0, 0, static_cast<size_t>(shape.x1 - shape.x0));
                    continue;
                }
                const uint8_t* prevRow = prev + off;
                for (int x = shape.x0; x < bounds.x0; ++x) {
                    row[x] = 0;
                }
                for (int x = bounds.x1; x < shape.x1; ++x) {
                    row[x] = 0;
                }
                for (int x = bounds.x0; x < bounds.x1; ++x) {
                    row[x] = std::min(prevRow[x], row[x]);
                }
            }
        }
        clipStack.push_back(ClipLayer{std::move(newMask), bounds});
    }

    void popClip() {
        // A well-formed CommandList always balances PushClipRect/PushClipPath with PopClip
        // (Docs/design/09-command-list.md §9.2); an empty stack here means malformed input from
        // an internal producer, which XT_ASSERT covers per Docs/design/14-coding-standards.md
        // §14.3. Not covered explicitly by Docs/tasks/T-11.md; chosen as the most conservative
        // option (no-op in Release rather than reading out of bounds).
        XT_ASSERT(!clipStack.empty());
        if (clipStack.empty()) {
            return;
        }
        ClipLayer top = std::move(clipStack.back());
        clipStack.pop_back();
        // Clearing only the rows this layer wrote (instead of the whole buffer) is what keeps the
        // pool's all-zero invariant cheap; see `maskPool`.
        const int w = target.width;
        for (int y = top.bounds.y0; y < top.bounds.y1; ++y) {
            std::memset(top.mask.data() + static_cast<size_t>(y) * static_cast<size_t>(w) + top.bounds.x0, 0,
                        static_cast<size_t>(top.bounds.x1 - top.bounds.x0));
        }
        maskPool.push_back(std::move(top.mask));
        combinedValid = false;
    }

    /// Empties the clip stack (its masks go back to the pool), the layer stack (releasing every
    /// pooled surface), the projection mappings and the effect frames: the state a render() must
    /// start from (Docs/tasks/T-263a.md E7). Shared by setTarget() and render().
    void resetStacks() {
        while (!clipStack.empty()) {
            popClip();
        }
        layers.resetStack();
        mappings.clear();
        effectFrames.clear();
    }

    /// Fills `points`/`starts` (already device-space, closed per addClosedContours) into `dst`
    /// using `eval` for per-pixel color, `rule` for winding, honoring the current clip mask.
    void paintContours(const std::vector<Vec2>& points, const std::vector<uint32_t>& starts, FillRule rule,
                       const PaintEval& eval, Target dst) {
        // T-133 step 4: the spans that can survive lie inside the path's own device-px box, the
        // destination's box and the current clip's box; when those three do not meet, the draw
        // writes nothing and the rasterizer never has to sweep it at all.
        const Box region = intersectBox(
            intersectBox(devicePointsBox(points),
                         Box{dst.originX, dst.originY, dst.originX + dst.w, dst.originY + dst.h}),
            clipBounds());
        if (region.empty()) {
            // Entirely clipped away: the same accounting a damage-skipped draw gets (the dispatch
            // loop has already counted this command as executed).
            if (executedDraws > 0) {
                --executedDraws;
            }
            ++skippedDraws;
            return;
        }
        {
            PerfFieldScope rasterScope(timer(&FrameStats::swRasterMs));
            raster.reset(target.width, target.height);
            addClosedContours(points, starts);
            raster.setSweepRows(region.y0, region.y1);
        }
        const bool solid = eval.isSolid();
        const uint8_t* clipData = activeClipData();
        PerfFieldScope spanScope(timer(&FrameStats::swSpanMs));
        // Docs/tasks/T-128.md step 1's breakdown. One scope per paint kind, plus a second,
        // deliberately *overlapping* one for the clipped case (see FrameStats::swSpanClippedMs).
        // Opened once per draw, never per span: a clock read per span would cost more than a span.
        PerfFieldScope kindScope(timer(solid ? &FrameStats::swSpanSolidMs : &FrameStats::swSpanGradientMs));
        PerfFieldScope spanClipScope(clipData != nullptr ? timer(&FrameStats::swSpanClippedMs) : nullptr);

        // Docs/tasks/T-82.md: this loop was the single hottest part of a frame, so everything that
        // does not change per pixel is hoisted out of it. The arithmetic is deliberately identical
        // to the previous Target::blendSrcOver/PaintEval::at path, operator for operator, so the
        // golden images stay bit-identical:
        //  - spans are clipped to the destination's own rectangle first (a bounded effect buffer
        //    used to evaluate and then discard every pixel outside itself, via Target::set);
        //  - a Solid paint's color is read once instead of per pixel;
        //  - a fully covered, unclipped pixel of an opaque solid paint reduces src-over to a store
        //    (a == 1 makes outA == 1 and the destination weight 0, so the result is the paint);
        //  - the destination row and the clip row are looked up once per span.
        //
        // Docs/tasks/T-128.md steps 2 and 4 extend the same idea to gradients: a run the clip mask
        // lets through completely is evaluated into `spanScratch` (one ramp lookup per pixel, `t`
        // stepped incrementally) and then blended by the same T-102 span kernels the solid case
        // uses, so a gradient span costs a vector blend instead of a scalar one plus a per-pixel
        // PaintEval::at.
        const Color solidColor = solid ? eval.solid() : Color{};
        const bool opaqueSolid = solid && solidColor.a == 1.0f;
        const uint8_t solidR8 = floatToByte(solidColor.r);
        const uint8_t solidG8 = floatToByte(solidColor.g);
        const uint8_t solidB8 = floatToByte(solidColor.b);
        const SwSimdOps& simd = swSimd();
        const bool dstPremul = dst.u8Premultiplied;
        const size_t clipStride = static_cast<size_t>(target.width);
        float* scratch = nullptr;
        if (!solid) {
            // At most one run per span, and a run is never longer than the destination is wide.
            spanScratch.resize(static_cast<size_t>(dst.w) * 4);
            scratch = spanScratch.data();
        }
        // `region` already folds in the destination's own rectangle and the clip's box, so the
        // per-span clamp below needs no separate destination test (T-133).
        const int dstX0 = region.x0;
        const int dstX1 = region.x1;

        raster.sweep(rule, [&](int y, int x0, int x1, uint8_t coverage) {
            if (coverage == 0) {
                return;
            }
            if (x0 < dstX0) {
                x0 = dstX0;
            }
            if (x1 > dstX1) {
                x1 = dstX1;
            }
            if (x0 >= x1) {
                return;
            }
            float* rowF = dst.rowF(y);
            uint8_t* rowU8 = rowF != nullptr ? nullptr : dst.rowU8(y);
            if (rowF == nullptr && rowU8 == nullptr) {
                return;
            }
            if (stats != nullptr) {
                ++stats->spansPerFrame;
                stats->spanPixelsPerFrame += static_cast<uint32_t>(x1 - x0);
            }
            const uint8_t* clipRow =
                clipData != nullptr ? clipData + static_cast<size_t>(y) * clipStride : nullptr;
            const float cov = static_cast<float>(coverage) / 255.0f;
            const bool fullCoverage = coverage == 255;

            // Docs/tasks/T-102.md step 4a: a solid paint at full clip is one constant
            // premultiplied source colour over a contiguous run, which is exactly what
            // `spanSolidOver*` does. The run is cut at every pixel the clip mask only partly lets
            // through, so a *partially* redrawn frame (whose damage mask arrives as a 255/0 clip)
            // takes the same kernel over the same pixels as a full redraw and stays byte-identical
            // to it (Docs/tasks/T-99.md test 1).
            const bool solidRuns = solid && !(opaqueSolid && fullCoverage);
            const float runA = cov * solidColor.a;
            const float runSrc[4] = {solidColor.r * runA, solidColor.g * runA, solidColor.b * runA, runA};

            for (int x = x0; x < x1;) {
                // Not a conditional expression: that would promote both arms to int and need a
                // narrowing cast back, which the merge gate's -Wconversion rejects.
                uint8_t clip = 255;
                if (clipRow != nullptr) {
                    clip = clipRow[static_cast<size_t>(x)];
                }
                if (clip == 0) {
                    x = maskRunEnd(clipRow, x, x1, 0);
                    continue;
                }
                const size_t lx = static_cast<size_t>(x - dst.originX);
                if (clip == 255) {
                    // Docs/tasks/T-128.md step 4: the longest run the clip mask lets through
                    // completely is painted exactly as if there were no clip -- no per-pixel
                    // multiply, and the whole run goes to one kernel call.
                    int runEnd = x1;
                    if (clipRow != nullptr) {
                        runEnd = maskRunEnd(clipRow, x + 1, x1, 255);
                    }
                    const int n = runEnd - x;
                    if (solidRuns) {
                        if (runA > 0.0f) {
                            if (rowF != nullptr) {
                                simd.spanSolidOverF(rowF + lx * 4, n, runSrc, 1.0f - runA);
                            } else {
                                simd.spanSolidOverU8(rowU8 + lx * 4, n, runSrc, 1.0f - runA, dstPremul);
                            }
                        }
                    } else if (solid) {
                        // opaqueSolid && fullCoverage: src-over is a store.
                        if (rowF != nullptr) {
                            for (int i = 0; i < n; ++i) {
                                float* p = rowF + (lx + static_cast<size_t>(i)) * 4;
                                p[0] = solidColor.r;
                                p[1] = solidColor.g;
                                p[2] = solidColor.b;
                                p[3] = 1.0f;
                            }
                        } else {
                            for (int i = 0; i < n; ++i) {
                                uint8_t* p = rowU8 + (lx + static_cast<size_t>(i)) * 4;
                                p[0] = solidR8;
                                p[1] = solidG8;
                                p[2] = solidB8;
                                p[3] = 255;
                            }
                        }
                    } else {
                        const PaintEval::SpanInfo info = eval.spanPremul(x, y, n, cov, scratch);
                        if (info.first < info.last) {
                            const int m = info.last - info.first;
                            const size_t off = lx + static_cast<size_t>(info.first);
                            const float* src = scratch + static_cast<size_t>(info.first) * 4;
                            if (info.allOpaque) {
                                // Every source pixel has alpha 1, so `src + dst * 0` is the source:
                                // the destination is never read (Docs/tasks/T-128.md step 2).
                                if (rowF != nullptr) {
                                    std::memcpy(rowF + off * 4, src,
                                                static_cast<size_t>(m) * 4 * sizeof(float));
                                } else {
                                    simd.rowToU8(src, rowU8 + off * 4, m, dstPremul);
                                }
                            } else if (rowF != nullptr) {
                                simd.spanSrcOverF(rowF + off * 4, src, m);
                            } else {
                                simd.spanSrcOverU8(rowU8 + off * 4, src, m, dstPremul);
                            }
                        }
                    }
                    x = runEnd;
                    continue;
                }
                const Color paint =
                    solid ? solidColor : eval.at(static_cast<float>(x), static_cast<float>(y));
                const float a = cov * paint.a * (static_cast<float>(clip) / 255.0f);
                if (a <= 0.0f) {
                    ++x;
                    continue;
                }
                // Premultiplied src-over (Docs/tasks/T-102.md step 2): no divide, no outA branch.
                const Color src{paint.r * a, paint.g * a, paint.b * a, a};
                const float k = 1.0f - a;
                if (rowF != nullptr) {
                    float* p = rowF + lx * 4;
                    p[0] = src.r + p[0] * k;
                    p[1] = src.g + p[1] * k;
                    p[2] = src.b + p[2] * k;
                    p[3] = src.a + p[3] * k;
                } else {
                    uint8_t* p = rowU8 + lx * 4;
                    Color d{static_cast<float>(p[0]) / 255.0f, static_cast<float>(p[1]) / 255.0f,
                            static_cast<float>(p[2]) / 255.0f, static_cast<float>(p[3]) / 255.0f};
                    if (!dstPremul) {
                        d = premultiply(d);
                    }
                    Color o = blendSrcOverPremul(src, d);
                    if (!dstPremul) {
                        o = unpremultiply(o);
                    }
                    p[0] = floatToByte(o.r);
                    p[1] = floatToByte(o.g);
                    p[2] = floatToByte(o.b);
                    p[3] = floatToByte(o.a);
                }
                ++x;
            }
        });
    }

    void fillPath(const CommandList& list, const FillCmd& cmd, float dpr) {
        Mat2x3 deviceXf = mul(deviceScale(dpr), cmd.xf);
        Mat3 screenToPlane;
        bool haveScreenToPlane = false;

        std::span<const PathVerb> verbs(list.verbs.data() + cmd.path.verbOffset, cmd.path.verbCount);
        std::span<const float> points(list.points.data() + cmd.path.pointOffset, cmd.path.pointCount);

        {
            PerfFieldScope pathScope(timer(&FrameStats::swPathMs));
            float tol = flattenTolerance(deviceXf, points);
            flatPoints.clear();
            flatStarts.clear();
            flatClosed.clear();
            flattenPath(verbs, points, deviceXf, tol, flatPoints, flatStarts, flatClosed);
            if (projecting() && cmd.paint.kind != PaintKind::Solid) {
                haveScreenToPlane = screenToPlaneHomography(flatPoints, screenToPlane);
            }
            if (projecting() && !projectPointsInPlace(flatPoints)) {
                return;  // §19.7: the whole path is dropped.
            }
        }

        std::span<const GradientStop> stops(list.stops.data() + cmd.paint.stopOffset, cmd.paint.stopCount);
        PaintEval eval = PaintEval::make(cmd.paint, stops, deviceXf, gradientLuts,
                                         haveScreenToPlane ? &screenToPlane : nullptr);
        Target dst = layers.currentTarget(rootTarget());
        paintContours(flatPoints, flatStarts, cmd.rule, eval, dst);
    }

    void strokePath(const CommandList& list, const StrokeCmd& cmd, float dpr) {
        Mat2x3 deviceXf = mul(deviceScale(dpr), cmd.xf);
        Mat3 screenToPlane;
        bool haveScreenToPlane = false;

        std::span<const PathVerb> verbs(list.verbs.data() + cmd.path.verbOffset, cmd.path.verbCount);
        std::span<const float> points(list.points.data() + cmd.path.pointOffset, cmd.path.pointCount);

        {
            PerfFieldScope pathScope(timer(&FrameStats::swPathMs));
            float tol = flattenTolerance(deviceXf, points);
            flatPoints.clear();
            flatStarts.clear();
            flatClosed.clear();
            flattenPath(verbs, points, deviceXf, tol, flatPoints, flatStarts, flatClosed);

            // Uniform-scale approximation of `cmd.xf` (Docs/tasks/T-12.md "描邊寬度以 xf 的均勻縮放近似").
            // §19.7 "寬度不變" under projection: no extra scale factor here -- the stroke is converted
            // to a fill in plane space (this same `scale`, same as unprojected) and only *that* result
            // gets projected below.
            float scale = std::sqrt(std::fabs(determinant(cmd.xf))) * dpr;

            const std::vector<Vec2>* srcPoints = &flatPoints;
            const std::vector<uint32_t>* srcStarts = &flatStarts;
            const std::vector<uint8_t>* srcClosed = &flatClosed;

            if (cmd.dashCount > 0) {
                scaledDashes.clear();
                for (uint32_t i = 0; i < cmd.dashCount; ++i) {
                    scaledDashes.push_back(list.dashes[cmd.dashOffset + i] * scale);
                }
                float scaledPhase = cmd.dashPhase * scale;
                dashPoints.clear();
                dashStarts.clear();
                dashClosed.clear();
                applyDash(flatPoints, flatStarts, flatClosed, scaledDashes, scaledPhase, dashPoints,
                          dashStarts, dashClosed);
                srcPoints = &dashPoints;
                srcStarts = &dashStarts;
                srcClosed = &dashClosed;
            }

            strokeOutPoints.clear();
            strokeOutStarts.clear();
            strokeToFill(*srcPoints, *srcStarts, *srcClosed, cmd.width * scale, cmd.cap, cmd.join,
                         cmd.miterLimit, tol, strokeOutPoints, strokeOutStarts);
            if (projecting() && cmd.paint.kind != PaintKind::Solid) {
                haveScreenToPlane = screenToPlaneHomography(strokeOutPoints, screenToPlane);
            }
            if (projecting() && !projectPointsInPlace(strokeOutPoints)) {
                return;  // §19.7: the whole path is dropped.
            }
        }

        std::span<const GradientStop> stops(list.stops.data() + cmd.paint.stopOffset, cmd.paint.stopCount);
        PaintEval eval = PaintEval::make(cmd.paint, stops, deviceXf, gradientLuts,
                                         haveScreenToPlane ? &screenToPlane : nullptr);
        Target dst = layers.currentTarget(rootTarget());
        paintContours(strokeOutPoints, strokeOutStarts, FillRule::NonZero, eval, dst);
    }

    void drawGlyphsCmd(const CommandList& list, const GlyphsCmd& cmd, float dpr, TextShaper* shaper) {
        if (shaper == nullptr) {
            return;  // No way to get outlines: skip (same rule DrawImage uses for a null resolver).
        }
        Mat2x3 deviceXf = mul(deviceScale(dpr), cmd.xf);
        if (cmd.effect != GlyphEffect::None) {
            // T-178: effects work on the glyph coverage bitmaps, like the GL atlas path; a run
            // those cannot follow (projected, rotated/skewed, geometricPrecision) draws no effect.
            if (projecting() || !glyphRunBitmapApplies(cmd, deviceXf)) {
                return;
            }
            Target effectDst = layers.currentTarget(rootTarget());
            drawGlyphEffect(*shaper, list, cmd, deviceXf, effectDst, activeClipData(), target.width);
            return;
        }
        // T-101: small `text-rendering: auto` text takes the hinted-bitmap path instead. Not
        // available under a 3D projection -- an axis-aligned bitmap blit cannot follow the
        // per-point projection 19.7 requires -- so projected runs keep using outlines.
        if (!projecting() && glyphBitmapPathApplies(cmd, deviceXf)) {
            Target bitmapDst = layers.currentTarget(rootTarget());
            const uint32_t drawn =
                drawGlyphBitmaps(*shaper, list, cmd, deviceXf, bitmapDst, activeClipData(), target.width);
            if (stats != nullptr) {
                stats->swGlyphBitmaps += drawn;
            }
            return;
        }
        {
            PerfFieldScope pathScope(timer(&FrameStats::swPathMs));
            if (!buildGlyphContours(*shaper, list, cmd, deviceXf, glyphPoints, glyphStarts)) {
                return;
            }
            if (stats != nullptr) {
                stats->swGlyphOutlines += cmd.glyphCount;
            }
        }
        // §19.7 "外框在平面空間縮放/平移到 glyphPos 後投影": buildGlyphContours already flattens
        // each glyph outline into plane space (mul(deviceXf, local), i.e. the same space FillPath
        // flattens into); project that result the same way FillPath does. The per-glyph flatten
        // tolerance itself is not divided by projectedScaleMax (sw_glyphs.cpp's
        // buildGlyphContours has no tolerance parameter and T-45's file list does not include it;
        // not covered by the task sheet, kept conservative/unchanged -- see report).
        if (projecting() && !projectPointsInPlace(glyphPoints)) {
            return;  // §19.7: the whole glyph run is dropped.
        }
        PaintRef paint{};
        paint.kind = PaintKind::Solid;
        paint.color = cmd.color;
        paint.p0 = Vec2{0, 0};
        paint.p1 = Vec2{0, 0};
        paint.stopOffset = 0;
        paint.stopCount = 0;
        PaintEval eval = PaintEval::make(paint, std::span<const GradientStop>{}, deviceXf, gradientLuts);
        Target dst = layers.currentTarget(rootTarget());
        paintContours(glyphPoints, glyphStarts, FillRule::NonZero, eval, dst);
    }

    void drawImageCmd(const CommandList& list, const ImageCmd& cmd, float dpr, AssetResolver* assets,
                      const IrView* ir) {
        if (assets == nullptr || ir == nullptr) {
            // No way to resolve the asset: skip (Docs/tasks/T-12.md "assets == nullptr 或解碼失敗
            // → 跳過該命令"; a missing IrView is treated the same way, see sw_renderer.h).
            return;
        }
        (void)list;
        const ImageData* view = nullptr;
        if (isExternalAsset(cmd.asset, *ir)) {
            // T-150: a host (engine://) texture has no CPU pixels and draws as transparent --
            // unless the host also bound CPU pixels for it (T-191, setExternalPixels).
            const AssetResolver::ExternalTexture* ext = assets->externalTexture(cmd.asset, *ir);
            view = ext != nullptr ? ext->pixels : nullptr;
        } else {
            // T-199: the `@2x` variant at dpr >= 1.5; T-345: `@3x` at >= 2.5, times the element's
            // `ui-scale` factor.
            view = assets->imageView(cmd.asset, *ir, dpr * cmd.variantScale);
        }
        if (view == nullptr) {
            return;
        }
        const ImageData& image = *view;
        Mat2x3 deviceXf = mul(deviceScale(dpr), cmd.xf);
        Target dst = layers.currentTarget(rootTarget());
        const uint8_t* clipPtr = activeClipData();
        // Docs/tasks/T-128.md step 1: image sampling is its own per-pixel loop, timed separately
        // from swSpanMs (see FrameStats::swSpanImageMs). Asset decode above is deliberately
        // outside the scope -- it is cached by the resolver, not per-pixel work.
        PerfFieldScope imageScope(timer(&FrameStats::swSpanImageMs));
        if (!projecting()) {
            drawImage(dst, raster, deviceXf, cmd, image, clipPtr, target.width, imageScratch);
            return;
        }
        // §19.7 DrawImage under projection: "dst 四角經 projectPoint 得裝置四邊形 q[4] ->
        // drawImageQuad".
        Vec2 planeCorners[4] = {
            apply(deviceXf, Vec2{cmd.dst.x, cmd.dst.y}),
            apply(deviceXf, Vec2{cmd.dst.x + cmd.dst.w, cmd.dst.y}),
            apply(deviceXf, Vec2{cmd.dst.x + cmd.dst.w, cmd.dst.y + cmd.dst.h}),
            apply(deviceXf, Vec2{cmd.dst.x, cmd.dst.y + cmd.dst.h}),
        };
        Vec2 q[4];
        for (int i = 0; i < 4; ++i) {
            bool ok = false;
            q[i] = projectDevicePoint(planeCorners[i], ok);
            if (!ok) {
                return;  // §19.7: dropped.
            }
        }
        drawImageQuad(dst, raster, q, cmd.srcUv, cmd.opacity, cmd.smooth, image, clipPtr, target.width,
                      imageScratch);
    }

    /// T-202 (Docs/tasks/T-202.md item 4): one rotated quad per particle; no texture (or no way to
    /// resolve it) draws the filled squares only when the command has no texture at all. Not
    /// drawn under a 3D projection (the simplifier's flatten3D drops it there as well).
    void drawParticlesCmd(const CommandList& list, const ParticlesCmd& cmd, float dpr, AssetResolver* assets,
                          const IrView* ir) {
        if (projecting() || cmd.count == 0 ||
            static_cast<size_t>(cmd.instanceOffset) + cmd.count > list.particleInstances.size()) {
            return;
        }
        const ImageData* image = nullptr;
        if (cmd.texture != kInvalidAsset) {
            if (assets == nullptr || ir == nullptr) {
                return;
            }
            if (isExternalAsset(cmd.texture, *ir)) {
                const AssetResolver::ExternalTexture* ext = assets->externalTexture(cmd.texture, *ir);
                image = ext != nullptr ? ext->pixels : nullptr;
            } else {
                image = assets->imageView(cmd.texture, *ir, dpr);
            }
            if (image == nullptr) {
                return;
            }
        }
        Mat2x3 deviceXf = mul(deviceScale(dpr), cmd.xf);
        Target dst = layers.currentTarget(rootTarget());
        const uint8_t* clipPtr = activeClipData();
        PerfFieldScope imageScope(timer(&FrameStats::swSpanImageMs));
        drawParticles(dst, raster, deviceXf, list.particleInstances.data() + cmd.instanceOffset, cmd.count,
                      cmd.blend == ParticleBlend::Additive, image, clipPtr, target.width, imageScratch);
    }

    void drawImageQuadCmd(const ImageQuadCmd& cmd, float dpr, AssetResolver* assets, const IrView* ir) {
        if (assets == nullptr || ir == nullptr) {
            return;  // Same "no way to resolve the asset" rule as DrawImage.
        }
        const ImageData* view = nullptr;
        if (isExternalAsset(cmd.asset, *ir)) {
            // T-150/T-191: same as DrawImage.
            const AssetResolver::ExternalTexture* ext = assets->externalTexture(cmd.asset, *ir);
            view = ext != nullptr ? ext->pixels : nullptr;
        } else {
            // T-199: the `@2x` variant at dpr >= 1.5; T-345: `@3x` at >= 2.5, times the element's
            // `ui-scale` factor.
            view = assets->imageView(cmd.asset, *ir, dpr * cmd.variantScale);
        }
        if (view == nullptr) {
            return;
        }
        const ImageData& image = *view;
        // §19.7 DrawImageQuad: "p[4] 經目前映射（無投影時即 deviceXf）"; ImageQuadCmd carries no xf
        // of its own, so the "current mapping" is just the dpr scale composed with the projection
        // stack (empty stack -> projectPoint degenerates to plain apply(dprXf, p), i.e. deviceXf).
        Mat2x3 dprXf = deviceScale(dpr);
        Vec2 q[4];
        for (int i = 0; i < 4; ++i) {
            bool ok = false;
            q[i] = projectDevicePoint(apply(dprXf, cmd.p[i]), ok);
            if (!ok) {
                return;  // §19.7: dropped.
            }
        }
        Target dst = layers.currentTarget(rootTarget());
        const uint8_t* clipPtr = activeClipData();
        PerfFieldScope imageScope(timer(&FrameStats::swSpanImageMs));
        drawImageQuad(dst, raster, q, cmd.srcUv, cmd.opacity, cmd.smooth, image, clipPtr, target.width,
                      imageScratch);
    }

    /// T-245 (Docs/design/32-rigging-and-deformation.md §32.2.4): the vertices go through the
    /// current mapping like DrawImageQuad's corners (dpr scale; under a projection each vertex is
    /// projected and the triangles stay affine), then drawImageMesh textures each triangle.
    void drawImageMeshCmd(const CommandList& list, const ImageMeshCmd& cmd, float dpr, AssetResolver* assets,
                          const IrView* ir) {
        if (assets == nullptr || ir == nullptr || cmd.indexCount < 3 ||
            static_cast<size_t>(cmd.vertexOffset) + cmd.vertexCount > list.meshVertices.size() ||
            static_cast<size_t>(cmd.indexOffset) + cmd.indexCount > list.meshIndices.size()) {
            return;
        }
        const ImageData* view = nullptr;
        if (isExternalAsset(cmd.asset, *ir)) {
            const AssetResolver::ExternalTexture* ext = assets->externalTexture(cmd.asset, *ir);
            view = ext != nullptr ? ext->pixels : nullptr;
        } else {
            view = assets->imageView(cmd.asset, *ir, dpr);
        }
        if (view == nullptr) {
            return;
        }
        const Mat2x3 dprXf = deviceScale(dpr);
        const bool project = projecting();
        meshScratch.resize(cmd.vertexCount);
        for (uint32_t i = 0; i < cmd.vertexCount; ++i) {
            const MeshVertex& src = list.meshVertices[cmd.vertexOffset + i];
            Vec2 p = apply(dprXf, src.p);
            if (project) {
                bool ok = false;
                p = projectDevicePoint(p, ok);
                if (!ok) {
                    return;  // §19.7: dropped.
                }
            }
            meshScratch[i] = MeshVertex{p, src.uv};
        }
        Target dst = layers.currentTarget(rootTarget());
        const uint8_t* clipPtr = activeClipData();
        PerfFieldScope imageScope(timer(&FrameStats::swSpanImageMs));
        drawImageMesh(dst, meshScratch.data(), cmd.vertexCount, list.meshIndices.data() + cmd.indexOffset,
                      cmd.indexCount, cmd.opacity, cmd.smooth, *view, clipPtr, target.width, imageScratch);
    }
};

SwRenderer::SwRenderer() : m_impl(std::make_unique<Impl>()) {}
SwRenderer::~SwRenderer() = default;

void SwRenderer::setTarget(const SwTarget& target) {
    // Before the new target is installed: popping a clip layer clears its rows at the old width.
    m_impl->resetStacks();
    m_impl->target = target;
    m_impl->maskPool.clear();
    m_impl->partialRedraw = false;
    m_impl->damageMask.clear();
    m_impl->damageRects.clear();
    m_impl->combinedValid = false;
}

void SwRenderer::clear(Color color) {
    SwTarget& target = m_impl->target;
    if (target.rgba == nullptr || target.width <= 0 || target.height <= 0) {
        return;
    }
    const Color stored = m_impl->target.premultiplied ? premultiply(color) : color;
    uint8_t r = floatToByte(stored.r);
    uint8_t g = floatToByte(stored.g);
    uint8_t b = floatToByte(stored.b);
    uint8_t a = floatToByte(stored.a);
    for (int y = 0; y < target.height; ++y) {
        uint8_t* row = target.rgba + static_cast<size_t>(y) * static_cast<size_t>(target.strideBytes);
        for (int x = 0; x < target.width; ++x) {
            uint8_t* px = row + static_cast<size_t>(x) * 4;
            px[0] = r;
            px[1] = g;
            px[2] = b;
            px[3] = a;
        }
    }
}

void SwRenderer::clearRects(Color color, std::span<const Rect> rects) {
    SwTarget& target = m_impl->target;
    if (target.rgba == nullptr || target.width <= 0 || target.height <= 0) {
        return;
    }
    const Color stored = m_impl->target.premultiplied ? premultiply(color) : color;
    uint8_t r = floatToByte(stored.r);
    uint8_t g = floatToByte(stored.g);
    uint8_t b = floatToByte(stored.b);
    uint8_t a = floatToByte(stored.a);
    for (const Rect& rect : rects) {
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        if (!snapRectToTarget(m_impl->deviceToTarget(rect), target.width, target.height, x0, y0, x1, y1)) {
            continue;
        }
        for (int y = y0; y < y1; ++y) {
            uint8_t* row = target.rgba + static_cast<size_t>(y) * static_cast<size_t>(target.strideBytes);
            for (int x = x0; x < x1; ++x) {
                uint8_t* px = row + static_cast<size_t>(x) * 4;
                px[0] = r;
                px[1] = g;
                px[2] = b;
                px[3] = a;
            }
        }
    }
}

uint32_t SwRenderer::lastExecutedDrawCommands() const { return m_impl->executedDraws; }

uint32_t SwRenderer::lastSkippedDrawCommands() const { return m_impl->skippedDraws; }

void SwRenderer::render(const CommandList& list, TextShaper* shaper, AssetResolver* assets, const IrView* ir,
                        PerfTracker* perf, const DamageList* damage) {
    if (m_impl->target.rgba == nullptr) {
        return;
    }
    // Docs/tasks/T-263a.md E1: a list a host filled by hand is checked before any pool is indexed
    // (one the library built or already checked says so); an invalid one draws nothing this frame.
    if (!list.structureChecked && !validateCommandList(list)) {
        m_impl->executedDraws = 0;
        m_impl->skippedDraws = 0;
        return;
    }
    // E7: nothing a previous render() left open may leak into this one.
    m_impl->resetStacks();
    // T-343: in Direct mode the canvas is the host's scene (changing every frame), so there is no
    // partial redraw; the damage decides which bands are pasted from their kept pixels instead.
    m_impl->directMode = list.hostTargetMode == HostTargetMode::Direct;
    m_impl->directFallback = false;
    m_impl->directDamage = m_impl->directMode ? damage : nullptr;
    m_impl->statBands = m_impl->statBandRedraws = m_impl->statBackdropCopyPx = m_impl->statPyramidLevels = 0;
    m_impl->beginDamage(m_impl->directMode ? nullptr : damage);
    // Gradient ramps are keyed by stop *contents* but only within one render(): a CommandList is
    // rebuilt every frame, so nothing may survive into the next one (Docs/tasks/T-128.md step 2).
    m_impl->gradientLuts.reset();
    // Docs/design/21-perf-tracing.md §21.2: one branch when tracing is off. The FrameStats& stays
    // valid for the whole call (PerfTracker only advances its ring buffer in beginFrame()).
    FrameStats* stats = (perf != nullptr && perf->enabled()) ? &perf->current() : nullptr;
    m_impl->stats = stats;
    PerfFieldScope renderScope(stats != nullptr ? &stats->renderMs : nullptr);
    const float dpr = list.devicePixelRatio;
    m_impl->currentDpr = dpr;
    if (m_impl->directMode) {
        m_impl->renderDirect(list, shaper, assets, ir, dpr);
    } else {
        m_impl->prevBands.clear();
        m_impl->prevRunKinds.clear();
        m_impl->prevBandsValid = false;
        for (size_t ci = 0; ci < list.commands.size(); ++ci) {
            ci = m_impl->runCommand(list, ci, shaper, assets, ir, dpr);
        }
    }
    if (stats != nullptr) {
        stats->bands = m_impl->statBands;
        stats->bandRedraws = m_impl->statBandRedraws;
        stats->backdropCopyPx = m_impl->statBackdropCopyPx;
        stats->pyramidLevels = m_impl->statPyramidLevels;
        stats->directFallback = (m_impl->directMode && m_impl->directFallback) ? 1u : 0u;
    }
    m_impl->directDamage = nullptr;
    m_impl->endDamage();
}

}  // namespace xt::sw
