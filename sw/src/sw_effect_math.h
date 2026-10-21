#pragma once

#include <vector>

#include "sw_layer.h"
#include "xt/types.h"

/// Pure SW effect math: separable box-approximated Gaussian blur, alpha-channel max filter
/// (dilation, for DropShadow's `spread`), an 8SSEDT signed distance field (for Outline), and the
/// ColorAdjust 4x5 color matrix. See docs/design/18-effects.md §18.3 (ColorAdjust) and §18.6 (SW
/// algorithm details), docs/tasks/T-53a.md.
///
/// All functions operate on premultiplied float buffers, consistent with sw_layer.h's Surface
/// (premultiplied since T-102).
/// None allocate on a steady-state per-frame path beyond resizing the caller-owned output/scratch
/// buffers to the working size (docs/design/01-architecture.md §1.7's allowed exception). These are
/// pure functions: no dependency on Surface beyond its layout, no touching of sw_renderer.cpp or
/// core/.

namespace xt::sw::fx {

/// T-343 (docs/design/18-effects.md §18.15): exact 2 x 2 box downsample of a `w x h` field of
/// `channels` interleaved floats into `ceil(w / 2) x ceil(h / 2)` (`outW`/`outH`), each output the
/// mean of its four sources clamped to the last column / row -- gpu_frame.h's `Downsample`.
void downsample2x2(const float* src, int w, int h, int channels, std::vector<float>& out, int& outW,
                   int& outH);

/// T-343: bilinear, edge-clamped upsample of a `sw x sh` field into `dw x dh` (gpu_frame.h's
/// `LevelMix` with scale 1 / offset 0): output `(x, y)` samples `p = ((x + 0.5) / dw * sw - 0.5,
/// (y + 0.5) / dh * sh - 0.5)`. `out` is resized.
void upsampleBilinear(const float* src, int sw, int sh, int channels, std::vector<float>& out, int dw,
                      int dh);

/// Separable three-pass box-approximated Gaussian blur (18.6). `sigmaX`/`sigmaY` are in pixels; the
/// box width per pass is `w = floor(sqrt(12*sigma^2/3 + 1))`, forced odd (even -> w -= 1); `w < 1`
/// skips that axis (copy through unchanged). Boundary samples are clamped to the edge (repeat).
/// `dst` and `scratch` are resized to match `src` and used as ping-pong buffers; `dst` holds the
/// result on return. `premultiply` is vestigial since T-102: a Surface is already premultiplied,
/// which is the space a blur must run in for a translucent edge not to darken, so both values now
/// mean the same three box passes over all four channels. T-343 (18 §18.15): a sigma above 4 px
/// (the larger of the two) runs at 1/2, 1/4 or 1/8 resolution (`xt::blurDownsampleShift`):
/// `downsample2x2` that many times, the box passes with `sigma / 2^shift`, `upsampleBilinear`
/// back -- the same arithmetic as the GPU path, so the backends agree.
void gaussianBlur(const Surface& src, Surface& dst, Surface& scratch, float sigmaX, float sigmaY,
                  bool premultiply);

/// Blurs only `src`'s alpha channel with an isotropic sigma (3 horizontal + 3 vertical box passes),
/// writing the result to `outAlpha` (resized to `src.w * src.h`, row-major). `scratch` is a
/// ping-pong buffer, resized as needed.
void gaussianBlurAlpha(const Surface& src, std::vector<float>& outAlpha, std::vector<float>& scratch,
                       float sigma);

/// `gaussianBlurAlpha` on a bare scalar field (`w * h`, row-major) instead of a Surface's alpha
/// channel. The effect passes build such a field anyway (the dilated or inverted alpha), so this
/// spares them packing it back into a 4-channel Surface only for the blur to unpack it again
/// (docs/tasks/T-102b.md). `outAlpha` may not alias `alpha`.
void gaussianBlurAlphaField(const std::vector<float>& alpha, int w, int h, std::vector<float>& outAlpha,
                            std::vector<float>& scratch, float sigma);

/// Separable max filter (van Herk / Gil-Werman, O(w*h) regardless of radius) over window radius `r`
/// (window size `2r+1`); used for DropShadow's `spread`. Samples outside `[0,w) x [0,h)` are treated
/// as 0. `out` and `scratch` are resized to `w*h`. `r <= 0` copies `in` to `out` unchanged.
void dilateAlpha(const std::vector<float>& in, std::vector<float>& out, std::vector<float>& scratch, int w,
                 int h, int r);

/// 8SSEDT signed distance field (Danielsson two-pass, forward + backward, 4-neighborhood + diagonal
/// each): `alpha >= 0.5` is foreground. `outSdf` (resized to `w*h`) holds the signed distance in
/// pixels, negative inside the foreground, positive outside (18.6): `sdf = dOut - dIn`, where `dOut`
/// is the distance transform of the foreground mask (0 on foreground pixels) and `dIn` is the
/// distance transform of the background mask (0 on background pixels).
void signedDistance(const std::vector<float>& alpha, std::vector<float>& outSdf, int w, int h);

/// A 4x5 affine color matrix: row `i` (0=R, 1=G, 2=B, 3=A) dotted with `(r, g, b, a, 1)` gives
/// output channel `i`. `applyColorMatrix` only reads rows 0..2 (RGB); row 3 is left as an identity
/// row on alpha by `colorAdjustMatrix` but is otherwise unused (alpha instead goes through the
/// `opacity` parameter, per 18.3).
struct ColorMatrix {
    float m[20];
};

/// Composes the ColorAdjust matrix per 18.3: `M = Sepia(sepia) . Invert(invert) . Hue(hueDeg) .
/// Sat(saturation) . Contrast(contrast) . Brightness(brightness)` (matrix product; Brightness is
/// applied first to the color vector; Invert/Sepia are T-334's slots 5/6, CSS Filter Effects
/// formulas). Ranges per 18.3's param table (e.g. brightness/contrast/saturation in [-1, 1]) are the
/// caller's responsibility; this function does not clamp its inputs.
ColorMatrix colorAdjustMatrix(float brightness, float contrast, float saturation, float hueDeg,
                              float invert = 0.0f, float sepia = 0.0f);

/// Applies `m` to every pixel of `s`: RGB' = clamp(M.row[0..2] . (r, g, b, a, 1), 0, 1);
/// A' = a * opacity.
void applyColorMatrix(Surface& s, const ColorMatrix& m, float opacity);

}  // namespace xt::sw::fx
