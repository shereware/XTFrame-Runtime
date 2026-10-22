#pragma once

#include <span>

#include "sw_layer.h"
#include "xt/effect_utils.h"
#include "xt/node.h"
#include "xt/types.h"
#include "xt/value.h"

/// SW built-in effect passes (DropShadow, Glow, Blur, ColorAdjust, GradientMask, Shine, Outline)
/// and the effect stack driver used by PushEffect/PopEffect. See Docs/design/18-effects.md §18.3
/// (per-effect formulas), §18.5 ("局部量換算", backend semantics), §18.6 (SW algorithm details),
/// Docs/tasks/T-53b.md.
///
/// All effects operate on non-premultiplied float RGBA (`Surface`, sw_layer.h). `screenParams` is
/// always an `EffectInstance` already converted to screen units by `xt::effectToScreen` (the
/// caller, sw_renderer.cpp's PopEffect, does this once per stacked effect using the node's
/// dpr-scaled screen xf). GradientMask and Shine additionally need to recover the node-local
/// plane the effect buffer's device pixels sit on:
///
/// - `invScreenXf` is the inverse of that same dpr-scaled screen xf (identity if the xf was
///   singular -- a degenerate case not covered by Docs/tasks/T-53b.md; the most conservative
///   choice per Docs/design/14-coding-standards.md §14.8).
/// - `bufferOriginPx` is the effect buffer's top-left corner in device px (`EffectCmd.boundsHint`
///   scaled by dpr and clamped to the viewport), added to a buffer-local pixel index to recover
///   its full device-px (screen) position before `invScreenXf`/`contentBoundsPx` comparisons.
/// - `contentBoundsPx` is `EffectCmd.contentBounds` scaled by dpr (device/screen px, unshifted --
///   i.e. in the same device-px frame `bufferOriginPx` is added into), used by Shine's band
///   center per §18.3.
namespace xt::sw {

/// Optional fine-grained timers for the effect sub-steps (Docs/tasks/T-82.md step 1). Each member
/// is either null (no timing) or points at a `FrameStats` float that the elapsed milliseconds are
/// added to; the SW renderer fills them from its PerfTracker, tests leave them all null.
struct EffectTimers {
    /// Gaussian blur passes.
    float* blur = nullptr;
    /// Dilate (max filter) passes.
    float* dilate = nullptr;
    /// Signed distance transform.
    float* sdf = nullptr;
    /// Everything else: the per-pixel composition loops.
    float* pix = nullptr;
};

/// Every heap buffer the effect passes need, owned by the caller so a warm renderer allocates
/// nothing per frame (Docs/tasks/T-102b.md; before it, each `applyEffect` call built a handful of
/// fresh `std::vector<float>`s and `applyEffectStack` two fresh `Surface`s, ~2 MB of allocation
/// plus zero-fill per HUD frame). `SwRenderer::Impl` holds one; the overloads without a scratch
/// parameter use a `thread_local` instance so the existing tests and any other caller keep
/// working. The members are only ever `resize`d, never shrunk, so capacities stop growing once
/// the largest effect buffer of a scene has been seen; their contents are undefined between
/// calls, and every pass fills the region it reads.
struct EffectScratch {
    /// `src`'s alpha channel (or, for the inset shadow, its padded inverse).
    std::vector<float> alpha;
    /// `fx::dilateAlpha` output.
    std::vector<float> dilated;
    /// `fx::gaussianBlurAlpha` / `fx::signedDistance` output.
    std::vector<float> blurred;
    /// Ping-pong scratch for `fx::dilateAlpha`.
    std::vector<float> fieldA;
    /// Ping-pong scratch for `fx::gaussianBlurAlpha`.
    std::vector<float> fieldB;
    /// One premultiplied RGBA row (`w * 4` floats) staged for `SwSimdOps::spanSrcOverF` when the
    /// composited layer has to go *on top* of the content (inner glow, inner outline, inset
    /// shadow) and `dst` therefore already holds the content.
    std::vector<float> row;
    /// `applyEffectStack`'s ping-pong stage buffers (used only when more than one effect stacks).
    Surface stageA;
    Surface stageB;
    /// `applyBackdropBlur`'s blurred copy of the backdrop, and its blur ping-pong buffer.
    Surface backdrop;
    Surface backdropScratch;
};

/// Copies the rectangle `x0, y0, w, h` (device px, in `src`'s own caller-space coordinates) out of
/// `src` into `out` (resized to `w x h`, premultiplied float like every Surface). Pixels the
/// rectangle covers but `src` does not are transparent (`Target::get`'s convention). Used by
/// sw_renderer.cpp at `PushEffect` time to snapshot what lies under a BackdropBlur node, before
/// its own offscreen layer is opened (Docs/design/18-effects.md §18.6).
void captureBackdrop(const Target& src, int x0, int y0, int w, int h, Surface& out);

/// T-343 (Docs/design/18-effects.md §18.15, 23 §23.aa.3): the backdrop blur pyramid, the CPU
/// twin of xt::gpu's `BackdropPyramid`. `level[0]` is the captured region (`x0`, `y0`, size
/// `level[0].w x level[0].h`, target px), `level[l]` the previous level box-downsampled 2 x 2
/// (`fx::downsample2x2`) and blurred once with the Kawase pass (center weight 4, the four 2 x 2
/// blocks 1.5 texels away diagonally weight 1 each, / 8); `xt::backdropPyramidSigma(l)` is each
/// level's Gaussian-equivalent sigma.
struct BackdropPyramid {
    int x0 = 0;
    int y0 = 0;
    int levels = 0;
    Surface level[kBackdropPyramidMaxLevels + 1];
    /// Pixels captured (level 0's area), for FrameStats::backdropCopyPx.
    uint32_t copiedPx = 0;
};

/// Captures `src`'s region (`x0`, `y0`, `w x h`, already clamped to the target) into level 0 and
/// builds `xt::backdropPyramidLevels(maxSigma)` levels above it.
void buildBackdropPyramid(const Target& src, int x0, int y0, int w, int h, float maxSigma,
                          BackdropPyramid& out, const EffectTimers& timers = EffectTimers{});

/// BackdropBlur over a pyramid (replaces the per-node Gaussian of the captured region): the two
/// levels whose sigmas bracket the node's (`xt::backdropPyramidMix`) are sampled bilinearly at
/// every content pixel (its device position mapped into the pyramid's region, the same
/// normalised mapping gpu_frame.h's `LevelMix` uses), mixed, given the node's saturation /
/// brightness, and composed under `src` as before.
void applyBackdropBlur(const EffectInstance& screenParams, const BackdropPyramid& pyramid,
                       Vec2 bufferOriginPx, const Surface& src, Surface& dst, EffectScratch& scratch,
                       const EffectTimers& timers = EffectTimers{});

/// The BackdropBlur stage (Docs/design/18-effects.md §18.3's kind 8, §18.6): blurs `backdrop`
/// (sigma = radius/2, radius capped at 64 device px), applies `saturation`/`brightness` with
/// ColorAdjust's own formulas, and composites the result *under* `src` masked by `src`'s own
/// alpha -- `dst = srcOver(backdropBlurred * alpha(src), src)`, so opaque content is untouched
/// and translucent content shows the blurred backdrop through it. `backdrop` is the snapshot
/// `captureBackdrop` took, positioned at `backdropOriginPx` (device px), and it must cover
/// `src`'s own rectangle (at `bufferOriginPx`) outset by the blur's 3 sigma read range for the
/// blur's edge clamping to match the GL backend's; pixels outside it read as transparent. `dst`
/// is resized to match `src`. Unlike every other effect this one is not reachable through
/// `applyEffect`: it needs a second input buffer, and (18.6) it is only ever the stack's first
/// stage, run by sw_renderer.cpp's PopEffect before `applyEffectStack` gets the rest.
void applyBackdropBlur(const EffectInstance& screenParams, const Surface& backdrop, Vec2 backdropOriginPx,
                       Vec2 bufferOriginPx, const Surface& src, Surface& dst, EffectScratch& scratch,
                       const EffectTimers& timers = EffectTimers{});

/// Applies one effect (`screenParams.kind`; `enabled == 0` is a no-op copy of `src` to `dst`) to
/// `src`, writing the result to `dst` (resized to match `src`). `stops` is the *owning pool*
/// (`CommandList::stops`); GradientMask reads `stops.subspan(screenParams.stopOffset,
/// screenParams.stopCount)`, every other kind ignores it. `scratchA`/`scratchB` are working
/// buffers private to this single call (resized as needed; their contents are undefined on
/// return). `src` and `dst` must not alias `scratchA`/`scratchB`, but `dst` may alias nothing in
/// particular -- callers chaining effects use `applyEffectStack` instead of calling this directly
/// stage-by-stage. See Docs/design/18-effects.md §18.3, §18.6.
void applyEffect(const EffectInstance& screenParams, std::span<const GradientStop> stops,
                 const Mat2x3& invScreenXf, Vec2 bufferOriginPx, const Rect& contentBoundsPx,
                 const Surface& src, Surface& scratchA, Surface& scratchB, Surface& dst,
                 EffectScratch& scratch, const EffectTimers& timers = EffectTimers{});

/// `applyEffect` on a `thread_local` `EffectScratch`; see `EffectScratch`.
void applyEffect(const EffectInstance& screenParams, std::span<const GradientStop> stops,
                 const Mat2x3& invScreenXf, Vec2 bufferOriginPx, const Rect& contentBoundsPx,
                 const Surface& src, Surface& scratchA, Surface& scratchB, Surface& dst,
                 const EffectTimers& timers = EffectTimers{});

/// Applies every entry of `screenParams` in order (stage k's input is stage k-1's output; stage 0
/// reads `src`) via repeated `applyEffect` calls, writing the final result to `dst`. An empty
/// `screenParams` copies `src` to `dst` unchanged. `scratchA`/`scratchB` are forwarded to every
/// `applyEffect` call as its private per-stage scratch; when more than one effect is stacked, two
/// additional ping-pong buffers (`scratch.stageA`/`stageB`) hold each stage's output until the
/// last stage writes `dst` (Docs/design/01-architecture.md §1.7's allowed exception -- not pooled,
/// since `LayerStack`'s pool is not reachable from this pure function).
void applyEffectStack(std::span<const EffectInstance> screenParams, std::span<const GradientStop> stops,
                      const Mat2x3& invScreenXf, Vec2 bufferOriginPx, const Rect& contentBoundsPx,
                      const Surface& src, Surface& scratchA, Surface& scratchB, Surface& dst,
                      EffectScratch& scratch, const EffectTimers& timers = EffectTimers{});

/// `applyEffectStack` on a `thread_local` `EffectScratch`; see `EffectScratch`.
void applyEffectStack(std::span<const EffectInstance> screenParams, std::span<const GradientStop> stops,
                      const Mat2x3& invScreenXf, Vec2 bufferOriginPx, const Rect& contentBoundsPx,
                      const Surface& src, Surface& scratchA, Surface& scratchB, Surface& dst,
                      const EffectTimers& timers = EffectTimers{});

}  // namespace xt::sw
