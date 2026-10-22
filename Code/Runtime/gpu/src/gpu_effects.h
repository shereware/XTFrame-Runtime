#pragma once

#include <span>

#include "frame_emit.h"
#include "gradient_lut.h"
#include "xt/effect_utils.h"
#include "xt/node.h"
#include "xt/types.h"
#include "xt/value.h"

/// Effect-pass scheduler (Docs/design/18-effects.md §18.5, Docs/tasks/T-146.md): the CPU half of
/// Code/Runtime/gl/src/gl_effects.cpp. Applies a node's already `xt::effectToScreen`-converted effect stack to
/// a transient target by emitting one window-covering `Shading::Effect` pass per shader step,
/// using scratch transients that share the content target's window (T-151; the window is the
/// node's `EffectCmd::boundsHint`, see frame_builder.cpp). The pass sequence per effect kind is
/// gl_effects.cpp's, one `EffectProgram` per GL program, minus the steps that would be the
/// identity (spread 0, sigma 0).

namespace xt::gpu::detail {

/// T-343 (Docs/design/18-effects.md §18.15, 23 §23.aa.3): the backdrop blur pyramid every
/// `backdrop-blur` of a window (one node in Layer mode, every top-level glass panel of a merged
/// region in Direct mode) samples from. `level[0]` is the copy of the parent target over `window`
/// (full resolution), `level[l]` (1 <= l <= `levels`) the previous level box-downsampled 2 x 2
/// and blurred once with `EffectProgram::Kawase`; `xt::backdropPyramidSigma(l)` is each level's
/// Gaussian-equivalent sigma. Every level is a transient the pyramid owns until
/// `releaseBackdropPyramid`.
struct BackdropPyramid {
    Scissor window;
    int levels = 0;
    TargetHandle level[kBackdropPyramidMaxLevels + 1] = {};
    /// Scene pixels the copy pass reads (`window` area), for FrameStats::backdropCopyPx.
    uint32_t copiedPx = 0;
};

/// Builds a pyramid over `window` (a `targetWindow` result inside `parent`'s window) from
/// `parent`'s current content: a copy pass into level 0, then `xt::backdropPyramidLevels(maxSigma)`
/// downsample + Kawase levels. The current pass is left on the last level written; the caller
/// opens its own pass afterwards. `maxSigma` is the largest sigma (device px) any panel of this
/// pyramid will ask for.
void buildBackdropPyramid(FrameEmitter& emit, TargetHandle parent, const Scissor& window, float maxSigma,
                          BackdropPyramid& out);
/// Releases every level (the pyramid is empty afterwards).
void releaseBackdropPyramid(FrameEmitter& emit, BackdropPyramid& pyramid);

struct EffectStackInput {
    /// In stack order; every entry enabled and screen-converted.
    std::span<const EffectInstance> screenParams;
    /// `CommandList::stops` (GradientMask).
    std::span<const GradientStop> stops;
    /// The node's screen -> local inverse (GradientMask).
    Mat2x3 invScreenXf;
    /// The unexpanded content AABB in surface pixels (Shine).
    Rect contentBoundsPx;
    /// The node's own drawn content (premultiplied color); its window is the window every pass
    /// of the stack uses.
    TargetHandle src = 0;
    /// T-343: the pyramid a leading enabled BackdropBlur samples (built from the parent target
    /// before the node's own layer opened; its window contains `src`'s); null when the stack has
    /// no such entry (a BackdropBlur then passes its input through).
    const BackdropPyramid* pyramid = nullptr;
    /// T-203: `xtTime` of Custom entries. A Custom entry of `screenParams` carries the device's
    /// custom program id in its `shader` field (frame_builder.cpp resolved it; an effect the
    /// device cannot run was already replaced by its fallback there).
    float time = 0;
};

/// Emits the passes for `in.screenParams` (non-empty) and returns the transient holding the
/// final result (premultiplied, `in.src`'s window). Every scratch transient is released before
/// returning; `in.src` and `in.pyramid`'s levels are *not* released (the caller owns them) and the
/// returned target is owned by the caller (release it after compositing). Returns `in.src` when
/// the stack is empty or every effect in it is the identity.
///
/// T-343 (18 §18.15): a Gaussian step whose sigma exceeds 4 device px runs at 1/2, 1/4 or 1/8
/// resolution (`xt::blurDownsampleShift`): `Downsample` passes into scaled transients, the
/// Gaussian at that level with sigma / 2^shift, then one `LevelMix` upsample.
TargetHandle applyEffectStack(FrameEmitter& emit, GradientLutCache& luts, const EffectStackInput& in);

}  // namespace xt::gpu::detail
