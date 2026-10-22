#pragma once

#include "xt/command_list.h"  // ShaderRef (T-203).
#include "xt/node.h"
#include "xt/types.h"

/// Backend-shared effect utilities: converting an EffectInstance's node-local-px parameters to
/// screen units, and computing an effect's bounds expansion. See Docs/design/18-effects.md §18.3
/// (expansion column), §18.5 ("局部量換算"). Used by Painter (boundsHint), and (in a later task)
/// the SW/GL effect passes themselves.

namespace xt {

/// Converts `e`'s node-local-px parameters (Docs/design/18-effects.md §18.3's tables) to screen
/// units using `xf` (the node's plane -> screen transform, `EffectCmd::xf`): length-like scalars
/// (radii, blur, spread, width, softness) are scaled by `s = sqrt(|det(xf.linear)|)`; the
/// DropShadow offset vector and Shine's angle are transformed by `xf`'s linear part (preserving
/// rotation/shear); GradientMask's p0/p1 are transformed by the full `xf` (including
/// translation). Every other field (colors, stop range, flags, 0..1 fractions) is copied
/// unchanged. `kind`/`enabled`/`shader`/`stopOffset`/`stopCount` are untouched.
EffectInstance effectToScreen(const EffectInstance& e, const Mat2x3& xf);

/// The screen-px amount `effectToScreen`'s result outsets its content AABB by, per
/// Docs/design/18-effects.md §18.3's "擴張量" column. Returns 0 for effects with no expansion
/// (ColorAdjust, GradientMask, Shine, BackdropBlur) and for a disabled/None effect.
float effectExpand(const EffectInstance& screenParams);

/// T-203 (Docs/design/18-effects.md §18.7): the screen-px expansion of a custom effect whose
/// ShaderRef is `ref`: the larger of its `expand:` (local px scaled like every length, by
/// `s = sqrt(|det(xf.linear)|)`) and its `fallback`'s own effectExpand() (SW, and GL after a
/// failed compile, draw the fallback instead). 0 for a null `ref`.
float customEffectExpand(const ShaderRef* ref, const Mat2x3& xf);

/// The screen-px amount an effect reads *beyond* the content AABB without drawing anything there,
/// per Docs/design/18-effects.md §18.3's BackdropBlur row ("0（讀取 backdrop 時向外多取 3σ）").
/// BackdropBlur returns `3 * sigma` (sigma = radius/2, radius capped at 64 device px like every
/// other radius-like parameter); every other kind reads only what it draws and returns 0. Used by
/// `computeDamage` (Docs/design/23-host-integration.md §23.2) and by the SW/GL backends to size
/// the backdrop copy.
float effectReadExpand(const EffectInstance& screenParams);

// --- T-343 (Docs/design/18-effects.md §18.15): downsampled blur and the backdrop pyramid. The
//     numbers both backends share, so xt::sw and xt_gpu/xt_gl agree texel for texel. ---

/// The power-of-two shift a plain Gaussian of `sigma` device px (a blur, a glow's or shadow's
/// mask) runs at: 0 for sigma <= 4, 1 (half size) above 4, 2 (quarter) above 8, 3 (eighth) above
/// 16. The blur is applied at that level with `sigma / 2^shift`, then upsampled bilinearly.
int blurDownsampleShift(float sigma);

/// Most levels of a backdrop pyramid above the full-resolution copy (level 0).
inline constexpr int kBackdropPyramidMaxLevels = 6;

/// The Gaussian-equivalent sigma, in full-resolution px, of pyramid level `level` (0 = the copy
/// itself, sigma 0): each level is the previous one box-downsampled 2 x 2 and blurred once with
/// the Kawase pass of `KawaseParams` (offset 1.5 texels), so the variance grows by
/// `(1/16 + 5/4) * 4^level` px^2 per level: about 2.3, 5.1, 10.5, 21, 42, 85 for levels 1..6.
float backdropPyramidSigma(int level);

/// Levels a pyramid needs so that the largest `backdrop-blur` sigma of the frame (`maxSigma`,
/// device px) can be served: the smallest level whose sigma reaches `maxSigma`, at most
/// kBackdropPyramidMaxLevels; 0 when `maxSigma` is 0 (the copy is used as it is).
int backdropPyramidLevels(float maxSigma);

/// Which two levels a panel of `sigma` samples and how they mix (18 §18.15: "the nearest level,
/// blended linearly with its neighbour"): `lower` <= `upper` <= `levels`, `t` in [0, 1] is the
/// weight of `upper`. A sigma beyond the top level takes that level alone.
void backdropPyramidMix(float sigma, int levels, int& lower, int& upper, float& t);

}  // namespace xt
