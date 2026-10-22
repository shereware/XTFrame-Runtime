#include "gpu_effects.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

#include "effect_math.h"  // Code/Runtime/core/src: shared with xt::sw (T-414)
#include "xt/assert.h"
#include "xt/effect_utils.h"

/// Mirrors Code/Runtime/gl/src/gl_effects.cpp step for step (see that file for the reasoning behind each
/// formula: the SW-effective sigma, merged Gaussian taps, the ColorAdjust matrix, the inset
/// shadow's out-of-range value of 1, ...). Differences: every step is a `Shading::Effect` command
/// instead of a GL draw; the y axis of every texel offset is *down* (gpu_frame.h's row-0-is-top
/// convention), so DropShadow's offset needs no sign flip; there is no final un-premultiply pass,
/// since `Shading::Composite` takes premultiplied input.
///
/// T-151: every pass works in the window of the node's content target (`in.src`): scratch
/// targets share that window, texel sizes are 1 / window size, and the window origin is folded
/// into GradientMask's inverse transform and Shine's band center so the shaders' `uv * size`
/// position math stays as it was. Steps that are the identity (a Dilate of radius 0, a Gaussian
/// of sigma 0) emit no pass at all.
///
/// T-343 (Docs/design/18-effects.md §18.15): a Gaussian whose sigma exceeds 4 device px is run
/// at 1/2, 1/4 or 1/8 resolution (`xt::blurDownsampleShift`): `Downsample` passes into scaled
/// transients (gpu_frame.h `TransientTargetDesc`), the two Gaussian passes at that level with
/// `sigma / 2^shift` and the level's own texel size, then one bilinear `LevelMix` back to the
/// window. BackdropBlur no longer blurs its own copy: it samples the `BackdropPyramid` the
/// builder hands it (box-downsampled, Kawase-blurred levels), mixing the two levels whose sigmas
/// bracket its own (`xt::backdropPyramidMix`), then applies its saturation / brightness. xt::sw
/// runs the same arithmetic (sw_effects.cpp), so the two backends stay within the golden
/// tolerance at every sigma.

namespace xt::gpu::detail {

namespace {

constexpr float kMaxDevicePx = 64.0f;

float clampDevicePx64(float v) {
    if (v > kMaxDevicePx) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::fprintf(stderr,
                         "xt::gpu effects: a radius-like effect parameter exceeded the 64 device px "
                         "cap; clamped (docs/design/18-effects.md \xc2\xa7"
                         "18.3).\n");
        }
        return kMaxDevicePx;
    }
    return v;
}

/// See gl_effects.cpp's swEffectiveSigma: the sigma a true Gaussian needs to match xt::sw's
/// triple-box approximation of the nominal sigma.
float swEffectiveSigma(float sigma) {
    if (sigma <= 1e-6f) {
        return 0.0f;
    }
    const int w = effect_math::boxBlurWidth(sigma);
    if (w < 1) {
        return 0.0f;
    }
    float n = static_cast<float>(w);
    return std::sqrt(n * n - 1.0f) / 2.0f;
}

struct GaussTaps {
    float weights[33] = {};
    float offsets[33] = {};
    int tapCount = 0;
};

GaussTaps computeGaussianTaps(float sigma) {
    GaussTaps t;
    int radius = static_cast<int>(std::ceil(3.0f * sigma));
    radius = std::min(radius, 64);
    if (radius < 1 || sigma <= 1e-6f) {
        t.weights[0] = 1.0f;
        return t;
    }
    std::vector<float> w(static_cast<size_t>(radius) + 1);
    float sum = 0.0f;
    for (int k = 0; k <= radius; ++k) {
        float wk = std::exp(-(static_cast<float>(k) * static_cast<float>(k)) / (2.0f * sigma * sigma));
        w[static_cast<size_t>(k)] = wk;
        sum += (k == 0) ? wk : 2.0f * wk;
    }
    for (float& wk : w) {
        wk /= sum;
    }
    t.weights[0] = w[0];
    int m = 0;
    for (int k = 1; k <= radius && m < 32; k += 2) {
        float w1 = w[static_cast<size_t>(k)];
        float w2 = (k + 1 <= radius) ? w[static_cast<size_t>(k + 1)] : 0.0f;
        float wsum = w1 + w2;
        float offset = (wsum > 1e-8f) ? (static_cast<float>(k) * w1 + static_cast<float>(k + 1) * w2) / wsum
                                      : static_cast<float>(k);
        ++m;
        t.weights[static_cast<size_t>(m)] = wsum;
        t.offsets[static_cast<size_t>(m)] = offset;
    }
    t.tapCount = m;
    return t;
}

/// True when a Gaussian of `sigma` is the identity (computeGaussianTaps would emit the center
/// tap alone), so the pass can be skipped.
bool gaussIsIdentity(float sigma) {
    const float eff = swEffectiveSigma(sigma);
    return eff <= 1e-6f || static_cast<int>(std::ceil(3.0f * eff)) < 1;
}

class Scheduler {
public:
    Scheduler(FrameEmitter& emit, GradientLutCache& luts, const EffectStackInput& in)
        : m_emit(emit), m_luts(luts), m_in(in), m_window(emit.windowOf(in.src)) {
        m_texelW = 1.0f / static_cast<float>(std::max(1, m_window.w));
        m_texelH = 1.0f / static_cast<float>(std::max(1, m_window.h));
    }

    TargetHandle run() {
        TargetHandle cur = m_in.src;
        for (const EffectInstance& p : m_in.screenParams) {
            TargetHandle out = applyOne(p, cur);
            if (out != cur && cur != m_in.src) {
                m_emit.releaseTransient(cur);
            }
            cur = out;
        }
        return cur;
    }

private:
    FrameEmitter& m_emit;
    GradientLutCache& m_luts;
    const EffectStackInput& m_in;
    Scissor m_window;  ///< the content target's window (surface px); every pass here uses it
    float m_texelW = 0;
    float m_texelH = 0;

    TargetHandle scratch() { return m_emit.acquireTransient(TextureFormat::Rgba8, false, m_window); }
    /// T-343: a scratch transient at 1 / 2^shift of the window (the window itself at shift 0).
    TargetHandle scaledScratch(int shift) {
        return m_emit.acquireTransient(TextureFormat::Rgba8, false, m_window, shift);
    }

    /// T-343: `src` (the window's resolution) box-downsampled `shift` times; a new scratch the
    /// caller owns (`src` itself at shift 0).
    TargetHandle downsampleTo(TargetHandle src, int shift) {
        TargetHandle cur = src;
        for (int l = 1; l <= shift; ++l) {
            TargetHandle dst = scaledScratch(l);
            int sw = 0, sh = 0, dw = 0, dh = 0;
            m_emit.sizeOf(cur, sw, sh);
            m_emit.sizeOf(dst, dw, dh);
            DownsampleParams p{static_cast<float>(sw), static_cast<float>(sh), static_cast<float>(dw),
                               static_cast<float>(dh)};
            pass(dst, EffectProgram::Downsample, p, cur);
            if (cur != src) {
                m_emit.releaseTransient(cur);
            }
            cur = dst;
        }
        return cur;
    }

    /// T-343: bilinear upsample of a scaled `level` into a new window-sized scratch.
    TargetHandle upsample(TargetHandle level) {
        TargetHandle dst = scratch();
        LevelMixParams p{1.0f, 1.0f, 0.0f, 0.0f, 0.0f};
        pass(dst, EffectProgram::LevelMix, p, level);
        return dst;
    }

    void release(TargetHandle t) {
        if (t != 0 && t != m_in.src) {
            m_emit.releaseTransient(t);
        }
    }

    /// One window pass writing `dst`.
    template <class P>
    void pass(TargetHandle dst, EffectProgram program, const P& params, TargetHandle src,
              TargetHandle aux = 0, TextureHandle texture = 0) {
        m_emit.beginPass(dst, false);
        m_emit.pushEffect(program, m_emit.pushParams(params), src, aux, texture);
    }

    void passNoParams(TargetHandle dst, EffectProgram program, TargetHandle src, TargetHandle aux) {
        m_emit.beginPass(dst, false);
        m_emit.pushEffect(program, 0, src, aux, 0);
    }

    void gaussPass(EffectProgram prog, TargetHandle src, TargetHandle dst, float sigma, bool horizontal,
                   bool constEdge = false, float outside = 0.0f) {
        GaussTaps taps = computeGaussianTaps(swEffectiveSigma(sigma));
        GaussParams p{};
        // T-343: the texel size of the target drawn (a scaled level is smaller than the window).
        int tw = 0, th = 0;
        m_emit.sizeOf(dst, tw, th);
        p.texelW = 1.0f / static_cast<float>(std::max(1, tw));
        p.texelH = 1.0f / static_cast<float>(std::max(1, th));
        p.dirX = horizontal ? 1.0f : 0.0f;
        p.dirY = horizontal ? 0.0f : 1.0f;
        p.tapCount = static_cast<float>(taps.tapCount);
        p.constEdge = constEdge ? 1.0f : 0.0f;
        p.outside = outside;
        for (int i = 0; i < 33; ++i) {
            p.weights[i] = taps.weights[i];
            p.offsets[i] = taps.offsets[i];
        }
        pass(dst, prog, p, src);
    }

    /// Separable Gaussian of `sigma` over `src` into a new scratch target; returns `src` itself
    /// (no pass) when the blur is the identity. With `ownsSrc` the caller hands `src` over: it
    /// is released as soon as the first pass has read it, so the second pass's target reuses its
    /// slot (the frame needs one transient less). The caller releases the result if it is new.
    TargetHandle gauss(EffectProgram prog, TargetHandle src, float sigma, bool ownsSrc,
                       bool constEdge = false, float outside = 0.0f) {
        if (gaussIsIdentity(sigma)) {
            return src;
        }
        const int shift = blurDownsampleShift(sigma);
        if (shift == 0) {
            TargetHandle a = scratch();
            gaussPass(prog, src, a, sigma, true, constEdge, outside);
            if (ownsSrc) {
                release(src);
            }
            TargetHandle b = scratch();
            gaussPass(prog, a, b, sigma, false, constEdge, outside);
            m_emit.releaseTransient(a);
            return b;
        }
        // T-343 (18 §18.15): blur at 1 / 2^shift with sigma / 2^shift, then upsample.
        const float levelSigma = sigma / static_cast<float>(1 << shift);
        TargetHandle level = downsampleTo(src, shift);
        if (ownsSrc) {
            release(src);
        }
        TargetHandle a = scaledScratch(shift);
        gaussPass(prog, level, a, levelSigma, true, constEdge, outside);
        m_emit.releaseTransient(level);
        TargetHandle b = scaledScratch(shift);
        gaussPass(prog, a, b, levelSigma, false, constEdge, outside);
        m_emit.releaseTransient(a);
        TargetHandle up = upsample(b);
        m_emit.releaseTransient(b);
        return up;
    }

    void dilatePass(TargetHandle src, TargetHandle dst, int radius, bool horizontal, bool readAlpha,
                    float outside = 0.0f) {
        DilateParams p{};
        p.texelW = m_texelW;
        p.texelH = m_texelH;
        p.dirX = horizontal ? 1.0f : 0.0f;
        p.dirY = horizontal ? 0.0f : 1.0f;
        p.radius = static_cast<float>(std::clamp(radius, 0, 64));
        p.readAlpha = readAlpha ? 1.0f : 0.0f;
        p.outside = outside;
        pass(dst, EffectProgram::Dilate, p, src);
    }

    void alphaExtract(TargetHandle src, TargetHandle dst, bool invert) {
        AlphaExtractParams p{invert ? 1.0f : 0.0f};
        pass(dst, EffectProgram::AlphaExtract, p, src);
    }

    /// Separable dilation by `radius` of the scalar field in `src` (`readAlpha`: of `src.a`) into
    /// a new scratch target. Radius 0 is the identity: with `readAlpha` a single AlphaExtract
    /// pass replaces the two Dilates, otherwise no pass at all and `src` is returned. `ownsSrc`
    /// as in `gauss`.
    TargetHandle dilate(TargetHandle src, int radius, bool readAlpha, bool ownsSrc, float outside = 0.0f) {
        if (radius <= 0) {
            if (!readAlpha) {
                return src;
            }
            TargetHandle a = scratch();
            alphaExtract(src, a, false);
            return a;
        }
        TargetHandle a = scratch();
        dilatePass(src, a, radius, true, readAlpha, outside);
        if (ownsSrc) {
            release(src);
        }
        TargetHandle b = scratch();
        dilatePass(a, b, radius, false, false, outside);
        m_emit.releaseTransient(a);
        return b;
    }

    void shadowGlow(TargetHandle src, TargetHandle mask, TargetHandle dst, float offsetX, float offsetY,
                    float scale, Color color, bool effectAlphaMul, bool lineOnTop, float maskOutside) {
        ShadowGlowParams p{};
        // uv' = uv - (offsetX / W, offsetY / H): row 0 is the top (gpu_frame.h), so a +y screen
        // shift is a +v shift -- unlike gl_effects.cpp's flipped v.
        p.offsetU = offsetX * m_texelW;
        p.offsetV = offsetY * m_texelH;
        p.scale = scale;
        p.r = color.r;
        p.g = color.g;
        p.b = color.b;
        p.a = color.a;
        p.effectAlphaMul = effectAlphaMul ? 1.0f : 0.0f;
        p.lineOnTop = lineOnTop ? 1.0f : 0.0f;
        p.maskOutside = maskOutside;
        pass(dst, EffectProgram::ShadowGlow, p, src, mask);
    }

    TargetHandle applyDropShadow(const EffectInstance& p, TargetHandle src) {
        float blur = clampDevicePx64(std::max(0.0f, p.params[2]));
        float spread = clampDevicePx64(std::max(0.0f, p.params[3]));
        int spreadR = static_cast<int>(std::lround(spread));
        float sigma = blur * 0.5f;

        TargetHandle dilated = dilate(src, spreadR, true, false);  // always a new scalar field
        TargetHandle mask = gauss(EffectProgram::GaussScalar, dilated, sigma, true);
        TargetHandle dst = scratch();
        shadowGlow(src, mask, dst, p.params[0], p.params[1], p.params[4], p.colors[0], false, false, 0.0f);
        release(mask);
        return dst;
    }

    TargetHandle applyDropShadowInset(const EffectInstance& p, TargetHandle src) {
        float blur = clampDevicePx64(std::max(0.0f, p.params[2]));
        float spread = clampDevicePx64(std::max(0.0f, p.params[3]));
        int spreadR = static_cast<int>(std::lround(spread));
        float sigma = blur * 0.5f;

        TargetHandle inv = scratch();
        alphaExtract(src, inv, true);
        TargetHandle dilated = dilate(inv, spreadR, false, true, 1.0f);
        TargetHandle mask = gauss(EffectProgram::GaussScalar, dilated, sigma, true, true, 1.0f);
        TargetHandle dst = scratch();
        shadowGlow(src, mask, dst, p.params[0], p.params[1], p.params[4], p.colors[0], true, true, 1.0f);
        release(mask);
        return dst;
    }

    TargetHandle applyGlow(const EffectInstance& p, TargetHandle src) {
        float radius = clampDevicePx64(std::max(0.0f, p.params[0]));
        float intensity = p.params[1];
        bool inner = p.params[2] != 0.0f;
        float sigma = radius * 0.5f;

        TargetHandle field = scratch();
        alphaExtract(src, field, inner);
        TargetHandle mask = gauss(EffectProgram::GaussScalar, field, sigma, true);
        TargetHandle dst = scratch();
        shadowGlow(src, mask, dst, 0.0f, 0.0f, intensity, p.colors[0], inner, inner, 0.0f);
        release(mask);
        return dst;
    }

    TargetHandle applyBlur(const EffectInstance& p, TargetHandle src) {
        float radiusX = clampDevicePx64(std::max(0.0f, p.params[0]));
        float radiusY = clampDevicePx64(std::max(0.0f, p.params[1]));
        const float sigmaX = radiusX * 0.5f;
        const float sigmaY = radiusY * 0.5f;
        const bool doX = !gaussIsIdentity(sigmaX);
        const bool doY = !gaussIsIdentity(sigmaY);
        if (!doX && !doY) {
            return src;
        }
        // T-343 (18 §18.15): both axes share one level, chosen by the larger sigma.
        const int shift = blurDownsampleShift(std::max(doX ? sigmaX : 0.0f, doY ? sigmaY : 0.0f));
        const float scale = 1.0f / static_cast<float>(1 << shift);
        TargetHandle cur = shift > 0 ? downsampleTo(src, shift) : src;
        if (doX) {
            TargetHandle a = scaledScratch(shift);
            gaussPass(EffectProgram::GaussColor, cur, a, sigmaX * scale, true);
            if (cur != src) {
                m_emit.releaseTransient(cur);
            }
            cur = a;
        }
        if (doY) {
            TargetHandle b = scaledScratch(shift);
            gaussPass(EffectProgram::GaussColor, cur, b, sigmaY * scale, false);
            if (cur != src) {
                m_emit.releaseTransient(cur);
            }
            cur = b;
        }
        if (shift > 0) {
            TargetHandle up = upsample(cur);
            m_emit.releaseTransient(cur);
            cur = up;
        }
        return cur;
    }

    TargetHandle applyColorAdjust(const EffectInstance& p, TargetHandle src) {
        // Same 4x5 matrix composition as gl_effects.cpp / xt::sw::fx::colorAdjustMatrix.
        float brightness = p.params[0];
        float contrast = p.params[1];
        float saturation = p.params[2];
        float hueDeg = p.params[3];
        float opacity = p.params[4];

        using Mat3x5 = std::array<std::array<float, 5>, 3>;
        auto identity = []() {
            Mat3x5 m{};
            m[0][0] = m[1][1] = m[2][2] = 1.0f;
            return m;
        };
        auto multiply = [](const Mat3x5& a, const Mat3x5& b) {
            Mat3x5 r{};
            for (size_t i = 0; i < 3; ++i) {
                for (size_t j = 0; j < 5; ++j) {
                    float sum = 0.0f;
                    for (size_t k = 0; k < 3; ++k) {
                        sum += a[i][k] * b[k][j];
                    }
                    if (j == 4) {
                        sum += a[i][4];
                    }
                    r[i][j] = sum;
                }
            }
            return r;
        };

        Mat3x5 bri = identity();
        bri[0][4] = bri[1][4] = bri[2][4] = brightness;
        Mat3x5 con = identity();
        for (size_t i = 0; i < 3; ++i) {
            con[i][i] = 1.0f + contrast;
            con[i][4] = -0.5f * contrast;
        }
        constexpr float lr = 0.2126f, lg = 0.7152f, lb = 0.0722f;
        Mat3x5 sat = identity();
        sat[0] = {1.0f + saturation * (1.0f - lr), -saturation * lg, -saturation * lb, 0.0f, 0.0f};
        sat[1] = {-saturation * lr, 1.0f + saturation * (1.0f - lg), -saturation * lb, 0.0f, 0.0f};
        sat[2] = {-saturation * lr, -saturation * lg, 1.0f + saturation * (1.0f - lb), 0.0f, 0.0f};

        float theta = hueDeg * effect_math::kDegToRad;
        float cs = std::cos(theta);
        float sn = std::sin(theta);
        Mat3x5 toYiq = identity();
        toYiq[0] = {0.299f, 0.587f, 0.114f, 0.0f, 0.0f};
        toYiq[1] = {0.596f, -0.274f, -0.322f, 0.0f, 0.0f};
        toYiq[2] = {0.211f, -0.523f, 0.312f, 0.0f, 0.0f};
        Mat3x5 rot = identity();
        rot[1] = {0.0f, cs, -sn, 0.0f, 0.0f};
        rot[2] = {0.0f, sn, cs, 0.0f, 0.0f};
        Mat3x5 fromYiq = identity();
        fromYiq[0] = {1.0f, 0.956f, 0.621f, 0.0f, 0.0f};
        fromYiq[1] = {1.0f, -0.272f, -0.647f, 0.0f, 0.0f};
        fromYiq[2] = {1.0f, -1.107f, 1.705f, 0.0f, 0.0f};
        Mat3x5 hue = multiply(fromYiq, multiply(rot, toYiq));
        // T-334: invert c*(1 - 2a) + a, then sepia = lerp(identity, standard sepia matrix, a).
        float invert = p.params[5];
        float sepia = p.params[6];
        Mat3x5 inv = identity();
        for (size_t i = 0; i < 3; ++i) {
            inv[i][i] = 1.0f - 2.0f * invert;
            inv[i][4] = invert;
        }
        static constexpr float kSepia[3][3] = {
            {0.393f, 0.769f, 0.189f}, {0.349f, 0.686f, 0.168f}, {0.272f, 0.534f, 0.131f}};
        Mat3x5 sep = identity();
        for (size_t i = 0; i < 3; ++i) {
            for (size_t j = 0; j < 3; ++j) {
                const float id = i == j ? 1.0f : 0.0f;
                sep[i][j] = id + sepia * (kSepia[i][j] - id);
            }
        }
        Mat3x5 m = multiply(sep, multiply(inv, multiply(hue, multiply(sat, multiply(con, bri)))));

        ColorMatrixParams cp{};
        for (size_t row = 0; row < 3; ++row) {
            for (size_t col = 0; col < 3; ++col) {
                cp.m[row * 3 + col] = m[row][col];
            }
            cp.offset[row] = m[row][4];
        }
        cp.opacity = opacity;
        TargetHandle dst = scratch();
        pass(dst, EffectProgram::ColorMatrix, cp, src);
        return dst;
    }

    TargetHandle applyGradientMask(const EffectInstance& p, TargetHandle src) {
        std::span<const GradientStop> stops;
        if (static_cast<uint64_t>(p.stopOffset) + p.stopCount <= m_in.stops.size()) {
            stops = m_in.stops.subspan(p.stopOffset, p.stopCount);
        }
        Vec2 p0 = apply(m_in.invScreenXf, Vec2{p.params[0], p.params[1]});
        Vec2 p1 = apply(m_in.invScreenXf, Vec2{p.params[2], p.params[3]});
        // The shader's `screenPx` is `uv * targetSize`, i.e. relative to the window origin; fold
        // the origin into the inverse so `invXf * (origin + windowPx)` comes out (T-151).
        const Mat2x3 invXf =
            mul(m_in.invScreenXf,
                translation(Vec2{static_cast<float>(m_window.x), static_cast<float>(m_window.y)}));
        GradientMaskParams gp{};
        gp.invXf[0] = invXf.a;
        gp.invXf[1] = invXf.b;
        gp.invXf[2] = invXf.c;
        gp.invXf[3] = invXf.d;
        gp.invXf[4] = invXf.e;
        gp.invXf[5] = invXf.f;
        gp.p0x = p0.x;
        gp.p0y = p0.y;
        gp.p1x = p1.x;
        gp.p1y = p1.y;
        gp.radial = p.params[4] != 0.0f ? 1.0f : 0.0f;
        gp.invert = p.params[5] != 0.0f ? 1.0f : 0.0f;
        gp.targetW = static_cast<float>(m_window.w);
        gp.targetH = static_cast<float>(m_window.h);
        TextureHandle lut = m_luts.get(stops);
        TargetHandle dst = scratch();
        pass(dst, EffectProgram::GradientMask, gp, src, 0, lut);
        return dst;
    }

    TargetHandle applyShine(const EffectInstance& p, TargetHandle src) {
        float intensity = p.params[4];
        Color color = p.colors[0];

        const effect_math::ShineBand band = effect_math::shineBand(p, m_in.contentBoundsPx);
        const Vec2 d = band.dir;
        ShineParams sp{};
        sp.dirX = d.x;
        sp.dirY = d.y;
        // The shader projects window-relative positions; move the band center by the window
        // origin's projection so it stays where the surface-space content box puts it (T-151).
        sp.center =
            band.center - (static_cast<float>(m_window.x) * d.x + static_cast<float>(m_window.y) * d.y);
        sp.edge0 = band.edge0;
        sp.edge1 = band.edge1;
        sp.r = color.r;
        sp.g = color.g;
        sp.b = color.b;
        sp.intensity = intensity;
        sp.targetW = static_cast<float>(m_window.w);
        sp.targetH = static_cast<float>(m_window.h);
        TargetHandle dst = scratch();
        pass(dst, EffectProgram::Shine, sp, src);
        return dst;
    }

    TargetHandle applyOutline(const EffectInstance& p, TargetHandle src) {
        float radius = clampDevicePx64(std::max(0.0f, p.params[0]));
        float softness = clampDevicePx64(std::max(0.0f, p.params[1]));
        float position = p.params[2];
        Color color = p.colors[0];

        TargetHandle sdf = m_emit.acquireTransient(TextureFormat::R32F, false, m_window);
        SdfParams sp{};
        sp.texelW = m_texelW;
        sp.texelH = m_texelH;
        sp.searchRadius = std::min(64.0f, radius + softness + 1.0f);
        pass(sdf, EffectProgram::Sdf, sp, src);

        OutlineParams op{};
        op.radius = radius;
        op.softness = softness;
        op.position = (position < 0.5f) ? 0.0f : (position < 1.5f ? 1.0f : 2.0f);
        op.r = color.r;
        op.g = color.g;
        op.b = color.b;
        op.a = color.a;
        TargetHandle dst = scratch();
        pass(dst, EffectProgram::Outline, op, src, sdf);
        m_emit.releaseTransient(sdf);
        return dst;
    }

    TargetHandle applyCustom(const EffectInstance& p, TargetHandle src) {
        if (p.shader == 0 || p.shader == kInvalidAsset) {
            return src;
        }
        CustomEffectParams cp{};
        cp.program = static_cast<float>(p.shader);
        cp.time = m_in.time;
        cp.layerW = static_cast<float>(std::max(1, m_window.w));
        cp.layerH = static_cast<float>(std::max(1, m_window.h));
        for (int i = 0; i < 8; ++i) {
            cp.params[i] = p.params[i];
        }
        for (int c = 0; c < 2; ++c) {
            cp.colors[c * 4 + 0] = p.colors[c].r;
            cp.colors[c * 4 + 1] = p.colors[c].g;
            cp.colors[c * 4 + 2] = p.colors[c].b;
            cp.colors[c * 4 + 3] = p.colors[c].a;
        }
        TargetHandle dst = scratch();
        pass(dst, EffectProgram::Custom, cp, src);
        return dst;
    }

    TargetHandle applyBackdropBlur(const EffectInstance& p, TargetHandle src) {
        const BackdropPyramid* pyr = m_in.pyramid;
        if (pyr == nullptr || pyr->level[0] == 0 || pyr->window.empty()) {
            return src;
        }
        float radius = clampDevicePx64(std::max(0.0f, p.params[0]));
        float sigma = radius * 0.5f;
        // T-343 (18 §18.15): the two pyramid levels bracketing this panel's sigma, mixed
        // linearly, sampled over the content window (its uv mapped into the pyramid's window).
        int lower = 0, upper = 0;
        float t = 0.0f;
        backdropPyramidMix(sigma, pyr->levels, lower, upper, t);
        LevelMixParams mp{};
        const float pw = static_cast<float>(std::max(1, pyr->window.w));
        const float ph = static_cast<float>(std::max(1, pyr->window.h));
        mp.scaleU = static_cast<float>(m_window.w) / pw;
        mp.scaleV = static_cast<float>(m_window.h) / ph;
        mp.offU = static_cast<float>(m_window.x - pyr->window.x) / pw;
        mp.offV = static_cast<float>(m_window.y - pyr->window.y) / ph;
        TargetHandle second = (upper != lower && t > 0.0f) ? pyr->level[upper] : 0;
        mp.t = second != 0 ? t : 0.0f;
        TargetHandle blurred = scratch();
        pass(blurred, EffectProgram::LevelMix, mp, pyr->level[lower], second);
        TargetHandle adjusted = blurred;
        if (p.params[1] != 0.0f || p.params[2] != 0.0f) {
            EffectInstance adjust{};
            adjust.kind = EffectKind::ColorAdjust;
            adjust.params[0] = p.params[2];  // brightness
            adjust.params[2] = p.params[1];  // saturation
            adjust.params[4] = 1.0f;         // opacity
            adjusted = applyColorAdjust(adjust, blurred);
            release(blurred);
        }
        TargetHandle dst = scratch();
        passNoParams(dst, EffectProgram::Backdrop, src, adjusted);
        release(adjusted);
        return dst;
    }

    /// Applies one effect to `src` and returns the target holding the result: a new scratch
    /// target the caller owns, or `src` itself when the effect is the identity (no pass).
    TargetHandle applyOne(const EffectInstance& p, TargetHandle src) {
        if (p.enabled == 0 || p.kind == EffectKind::None) {
            return src;
        }
        switch (p.kind) {
            case EffectKind::DropShadow:
                return p.params[5] != 0.0f ? applyDropShadowInset(p, src) : applyDropShadow(p, src);
            case EffectKind::Glow:
                return applyGlow(p, src);
            case EffectKind::Blur:
                return applyBlur(p, src);
            case EffectKind::ColorAdjust:
                return applyColorAdjust(p, src);
            case EffectKind::GradientMask:
                return applyGradientMask(p, src);
            case EffectKind::Shine:
                return applyShine(p, src);
            case EffectKind::Outline:
                return applyOutline(p, src);
            case EffectKind::BackdropBlur:
                return applyBackdropBlur(p, src);
            case EffectKind::Custom:
                return applyCustom(p, src);  // T-203.
            case EffectKind::None:
            default:
                XT_ASSERT(false && "unsupported: unknown effect kind");
                return src;
        }
    }
};

}  // namespace

TargetHandle applyEffectStack(FrameEmitter& emit, GradientLutCache& luts, const EffectStackInput& in) {
    if (in.screenParams.empty()) {
        return in.src;
    }
    Scheduler s(emit, luts, in);
    return s.run();
}

void buildBackdropPyramid(FrameEmitter& emit, TargetHandle parent, const Scissor& window, float maxSigma,
                          BackdropPyramid& out) {
    releaseBackdropPyramid(emit, out);
    out.window = window;
    if (window.empty()) {
        return;
    }
    // Level 0: the region of the parent, copied (a resolve on a multisampled parent).
    out.level[0] = emit.acquireTransient(TextureFormat::Rgba8, false, window);
    emit.beginPass(out.level[0], false, true, parent);
    out.copiedPx =
        static_cast<uint32_t>(std::min<int64_t>(int64_t(window.w) * int64_t(window.h), 0xFFFFFFFFll));
    out.levels = backdropPyramidLevels(std::min(maxSigma, kMaxDevicePx * 0.5f));
    for (int l = 1; l <= out.levels; ++l) {
        // Box downsample into a scaled scratch, then one Kawase pass into the level itself.
        TargetHandle box = emit.acquireTransient(TextureFormat::Rgba8, false, window, l);
        int sw = 0, sh = 0, dw = 0, dh = 0;
        emit.sizeOf(out.level[l - 1], sw, sh);
        emit.sizeOf(box, dw, dh);
        DownsampleParams dp{static_cast<float>(sw), static_cast<float>(sh), static_cast<float>(dw),
                            static_cast<float>(dh)};
        emit.beginPass(box, false);
        emit.pushEffect(EffectProgram::Downsample, emit.pushParams(dp), out.level[l - 1]);
        out.level[l] = emit.acquireTransient(TextureFormat::Rgba8, false, window, l);
        KawaseParams kp{1.0f / static_cast<float>(std::max(1, dw)),
                        1.0f / static_cast<float>(std::max(1, dh)), 1.5f};
        emit.beginPass(out.level[l], false);
        emit.pushEffect(EffectProgram::Kawase, emit.pushParams(kp), box);
        emit.releaseTransient(box);
    }
}

void releaseBackdropPyramid(FrameEmitter& emit, BackdropPyramid& pyramid) {
    for (TargetHandle& t : pyramid.level) {
        if (t != 0) {
            emit.releaseTransient(t);
        }
        t = 0;
    }
    pyramid.levels = 0;
    pyramid.copiedPx = 0;
    pyramid.window = Scissor{};
}

}  // namespace xt::gpu::detail
