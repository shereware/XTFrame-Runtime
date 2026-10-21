#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "xt/node.h"
#include "xt/types.h"

/// Internal pixel blending. `blendSrcOverPremul` is the plain Normal-blend src-over the hot
/// FillPath/StrokePath/compositing loops use (stage 1, docs/design/10-backends.md §10.2.1 step 5);
/// `blendPixel`/`blendPixelPremul` implement the full W3C Compositing and Blending Level 1 set
/// (17 modes) used by Layer/Mask compositing. Float pixels are premultiplied since T-102.
/// See docs/tasks/T-11.md "SwRenderer::render 流程", docs/tasks/T-12.md "混合（sw_blend）".

namespace xt::sw {

/// Converts a float color component in [0, 1] to a rounded, clamped byte. Clamp, scale, round
/// half up: on a value already clamped to [0, 1] that is what the pre-T-102 `lround` did, and it
/// is exactly what `xt::simd::storeU8` computes, so the scalar and SIMD paths agree bit for bit
/// (docs/tasks/T-102.md test 2).
inline uint8_t floatToByte(float v) {
    float clamped = std::clamp(v, 0.0f, 1.0f);
    return static_cast<uint8_t>(static_cast<int>(clamped * 255.0f + 0.5f));
}

/// Non-premultiplied -> premultiplied RGBA (docs/tasks/T-102.md step 2).
inline Color premultiply(const Color& c) { return Color{c.r * c.a, c.g * c.a, c.b * c.a, c.a}; }

/// Premultiplied -> non-premultiplied RGBA; alpha 0 is fully transparent black, matching what
/// src-over wrote when `outA <= 0` before T-102. Divides (rather than multiplying by a reciprocal)
/// so the result matches `xt::simd::f32x4`'s `div`.
inline Color unpremultiply(const Color& c) {
    if (c.a <= 0.0f) {
        return Color{0, 0, 0, 0};
    }
    return Color{c.r / c.a, c.g / c.a, c.b / c.a, c.a};
}

/// Premultiplied source-over: `dst = src + dst * (1 - srcA)` (docs/design/23-host-integration.md
/// §23.5). No division and no branch, which is the point of the premultiplied pipeline.
inline Color blendSrcOverPremul(const Color& src, const Color& dst) {
    const float k = 1.0f - src.a;
    return Color{src.r + dst.r * k, src.g + dst.g * k, src.b + dst.b * k, src.a + dst.a * k};
}

/// Composites non-premultiplied `src` (top) over non-premultiplied `dst` (backdrop) using `mode`,
/// per W3C Compositing and Blending Level 1 §9.1.3 (separable modes), §9.1.4 (non-separable modes:
/// Hue/Saturation/Color/Luminosity), and BlendMode::Add (= Cs + Cb clamped to 1, the color part of
/// "plus-lighter"). Returns the composited non-premultiplied color (see docs/tasks/T-12.md
/// "混合（sw_blend）" for the general compositing formula).
Color blendPixel(BlendMode mode, Color src, Color dst);

/// `blendPixel` for the premultiplied pipeline: both `src` and `dst` are premultiplied and so is
/// the result. Normal is evaluated directly as `src + dst * (1 - srcA)`; the other 16 modes are
/// defined by W3C on *non*-premultiplied channels (B(Cb, Cs) is not alpha-homogeneous -- Multiply,
/// ColorDodge, the four non-separable modes), so they divide alpha out, run the same
/// `applyBlendFunc`/composite arithmetic as before and multiply it back in. Bit-identical results
/// for every mode; only Normal, the mode the layer/effect compositing loops actually use, gets
/// faster (docs/tasks/T-102.md step 2, docs/design/09-command-list.md's blend mode list).
Color blendPixelPremul(BlendMode mode, const Color& src, const Color& dst);

}  // namespace xt::sw
