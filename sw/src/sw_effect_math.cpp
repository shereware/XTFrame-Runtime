#include "sw_effect_math.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "effect_math.h"  // core/src: shared with xt::gpu (T-414)
#include "sw_blend.h"
#include "sw_simd.h"
#include "xt/effect_utils.h"

/// See docs/design/18-effects.md §18.3/§18.6, docs/tasks/T-53a.md.

namespace xt::sw::fx {

namespace {

constexpr float kPi = 3.14159265358979323846f;

/// Box width per 18.6: `w = floor(sqrt(12*sigma^2/3 + 1))`, forced odd. Returns the box *radius*
/// (half-width); sets `skip` when `w < 1` (the axis should be left unchanged).
int boxRadius(float sigma, bool& skip) {
    const int w = effect_math::boxBlurWidth(sigma);
    if (w < 1) {
        skip = true;
        return 0;
    }
    skip = false;
    return (w - 1) / 2;
}

/// One horizontal box-filter pass, edge-clamped, via a sliding-window sum (O(w*h*channels));
/// `src`/`dst` are `w*h*channels` floats, row-major, `channels` interleaved per pixel. The body
/// lives in sw_simd_kernels.h so the scalar, SSE2 and AVX2 builds share one source
/// (docs/tasks/T-102.md step 4b).
void boxPassHorizontal(const float* src, float* dst, int w, int h, int channels, int radius) {
    swSimd().boxPassH(src, dst, w, h, channels, radius);
}

/// One vertical box-filter pass, same convention as boxPassHorizontal.
void boxPassVertical(const float* src, float* dst, int w, int h, int channels, int radius) {
    swSimd().boxPassV(src, dst, w, h, channels, radius);
}

/// Runs the 3 horizontal + 3 vertical box passes into `cur`/`tmp` (each sized `w*h*channels`),
/// ping-ponging buffers; returns true when the final result ended up in `cur` (as opposed to
/// `tmp`), so the caller knows whether it needs to swap its owning storage back.
bool runThreeBoxPasses(float*& cur, float*& tmp, int w, int h, int channels, int radiusX, bool skipX,
                       int radiusY, bool skipY) {
    bool resultInCur = true;
    if (!skipX) {
        for (int pass = 0; pass < 3; ++pass) {
            boxPassHorizontal(cur, tmp, w, h, channels, radiusX);
            std::swap(cur, tmp);
            resultInCur = !resultInCur;
        }
    }
    if (!skipY) {
        for (int pass = 0; pass < 3; ++pass) {
            boxPassVertical(cur, tmp, w, h, channels, radiusY);
            std::swap(cur, tmp);
            resultInCur = !resultInCur;
        }
    }
    return resultInCur;
}

/// 1D max filter (van Herk / Gil-Werman), window radius `r` (window size `2r+1`), O(n) regardless
/// of `r`. Samples outside `[0, n)` are treated as 0 (18.6). `r <= 0` copies `in` to `out`.
void maxFilter1D(const float* in, float* out, int n, int r) {
    if (r <= 0) {
        std::copy(in, in + n, out);
        return;
    }
    const int windowLen = 2 * r + 1;
    const int paddedLen = n + 2 * r;
    const int blockCount = (paddedLen + windowLen - 1) / windowLen;
    const int extLen = blockCount * windowLen;

    std::vector<float> ext(static_cast<size_t>(extLen), -std::numeric_limits<float>::infinity());
    for (int i = 0; i < paddedLen; ++i) {
        int srcIdx = i - r;
        ext[static_cast<size_t>(i)] = (srcIdx >= 0 && srcIdx < n) ? in[srcIdx] : 0.0f;
    }

    std::vector<float> prefixMax(static_cast<size_t>(extLen));
    std::vector<float> suffixMax(static_cast<size_t>(extLen));
    for (int i = 0; i < extLen; ++i) {
        prefixMax[static_cast<size_t>(i)] =
            (i % windowLen == 0)
                ? ext[static_cast<size_t>(i)]
                : std::max(prefixMax[static_cast<size_t>(i - 1)], ext[static_cast<size_t>(i)]);
    }
    for (int i = extLen - 1; i >= 0; --i) {
        suffixMax[static_cast<size_t>(i)] =
            (i % windowLen == windowLen - 1 || i == extLen - 1)
                ? ext[static_cast<size_t>(i)]
                : std::max(suffixMax[static_cast<size_t>(i + 1)], ext[static_cast<size_t>(i)]);
    }

    for (int i = 0; i < n; ++i) {
        // Window over `ext` starts at padded index i, length windowLen, ends at i + windowLen - 1.
        out[i] =
            std::max(suffixMax[static_cast<size_t>(i)], prefixMax[static_cast<size_t>(i + windowLen - 1)]);
    }
}

/// A per-pixel offset vector to the nearest source pixel, used by the 8SSEDT two-pass scan.
struct DVec {
    int dx = 0;
    int dy = 0;
};

long long sqLen(const DVec& v) {
    return static_cast<long long>(v.dx) * v.dx + static_cast<long long>(v.dy) * v.dy;
}

void updateFrom(std::vector<DVec>& grid, int w, int h, int x, int y, int ox, int oy) {
    int nx = x + ox;
    int ny = y + oy;
    if (nx < 0 || nx >= w || ny < 0 || ny >= h) {
        return;
    }
    DVec candidate = grid[static_cast<size_t>(ny) * static_cast<size_t>(w) + static_cast<size_t>(nx)];
    candidate.dx += ox;
    candidate.dy += oy;
    DVec& current = grid[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
    if (sqLen(candidate) < sqLen(current)) {
        current = candidate;
    }
}

/// Danielsson two-pass 8SSEDT: distance transform of `sourceMask` (nonzero = source, distance 0).
void distanceTransform(const std::vector<uint8_t>& sourceMask, int w, int h, std::vector<float>& outDist) {
    constexpr int kInf = 1 << 14;
    std::vector<DVec> grid(static_cast<size_t>(w) * static_cast<size_t>(h));
    for (size_t i = 0; i < grid.size(); ++i) {
        grid[i] = sourceMask[i] != 0 ? DVec{0, 0} : DVec{kInf, kInf};
    }

    // Forward pass: top-to-bottom, left-to-right; compare with already-visited neighbors.
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            updateFrom(grid, w, h, x, y, -1, 0);
            updateFrom(grid, w, h, x, y, 0, -1);
            updateFrom(grid, w, h, x, y, -1, -1);
            updateFrom(grid, w, h, x, y, 1, -1);
        }
    }
    // Backward pass: bottom-to-top, right-to-left; compare with the remaining neighbors.
    for (int y = h - 1; y >= 0; --y) {
        for (int x = w - 1; x >= 0; --x) {
            updateFrom(grid, w, h, x, y, 1, 0);
            updateFrom(grid, w, h, x, y, 0, 1);
            updateFrom(grid, w, h, x, y, 1, 1);
            updateFrom(grid, w, h, x, y, -1, 1);
        }
    }

    outDist.resize(grid.size());
    for (size_t i = 0; i < grid.size(); ++i) {
        outDist[i] = std::sqrt(static_cast<float>(sqLen(grid[i])));
    }
}

/// A 5x5 matrix representing an affine map on the homogeneous (r, g, b, a, 1) vector; row/col 3 is
/// alpha, row/col 4 is the constant 1. Used only to compose the ColorAdjust base transforms (18.3);
/// the public ColorMatrix keeps just the first 4 rows.
struct Mat5 {
    float m[5][5];
};

Mat5 identity5() {
    Mat5 r{};
    for (int i = 0; i < 5; ++i) {
        r.m[i][i] = 1.0f;
    }
    return r;
}

Mat5 multiply(const Mat5& a, const Mat5& b) {
    Mat5 r{};
    for (int i = 0; i < 5; ++i) {
        for (int j = 0; j < 5; ++j) {
            float sum = 0.0f;
            for (int k = 0; k < 5; ++k) {
                sum += a.m[i][k] * b.m[k][j];
            }
            r.m[i][j] = sum;
        }
    }
    return r;
}

/// Brightness: c + b (18.3).
Mat5 brightnessMat(float b) {
    Mat5 r = identity5();
    r.m[0][4] = b;
    r.m[1][4] = b;
    r.m[2][4] = b;
    return r;
}

/// Contrast: (c - 0.5)*(1 + k) + 0.5 = c*(1 + k) - 0.5*k (18.3).
Mat5 contrastMat(float k) {
    Mat5 r = identity5();
    for (int i = 0; i < 3; ++i) {
        r.m[i][i] = 1.0f + k;
        r.m[i][4] = -0.5f * k;
    }
    return r;
}

/// Saturation: lerp(luma, c, 1 + s), luma weights (0.2126, 0.7152, 0.0722) (18.3).
Mat5 saturationMat(float s) {
    constexpr float lr = 0.2126f;
    constexpr float lg = 0.7152f;
    constexpr float lb = 0.0722f;
    Mat5 r = identity5();
    r.m[0][0] = 1.0f + s * (1.0f - lr);
    r.m[0][1] = -s * lg;
    r.m[0][2] = -s * lb;
    r.m[1][0] = -s * lr;
    r.m[1][1] = 1.0f + s * (1.0f - lg);
    r.m[1][2] = -s * lb;
    r.m[2][0] = -s * lr;
    r.m[2][1] = -s * lg;
    r.m[2][2] = 1.0f + s * (1.0f - lb);
    return r;
}

/// Invert (T-334, CSS Filter Effects): c + a*(1 - 2c) = c*(1 - 2a) + a.
Mat5 invertMat(float a) {
    Mat5 r = identity5();
    for (int i = 0; i < 3; ++i) {
        r.m[i][i] = 1.0f - 2.0f * a;
        r.m[i][4] = a;
    }
    return r;
}

/// Sepia (T-334, CSS Filter Effects): the standard sepia matrix lerped with the identity by `a`.
Mat5 sepiaMat(float a) {
    static constexpr float kSepia[3][3] = {
        {0.393f, 0.769f, 0.189f}, {0.349f, 0.686f, 0.168f}, {0.272f, 0.534f, 0.131f}};
    Mat5 r = identity5();
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            const float id = i == j ? 1.0f : 0.0f;
            r.m[i][j] = id + a * (kSepia[i][j] - id);
        }
    }
    return r;
}

/// Hue: rotate the YIQ I/Q plane by `hueDeg` degrees (18.3): RGB -> YIQ -> rotate -> RGB.
Mat5 hueMat(float hueDeg) {
    float theta = hueDeg * kPi / 180.0f;
    float c = std::cos(theta);
    float sn = std::sin(theta);

    Mat5 toYIQ = identity5();
    toYIQ.m[0][0] = 0.299f;
    toYIQ.m[0][1] = 0.587f;
    toYIQ.m[0][2] = 0.114f;
    toYIQ.m[1][0] = 0.596f;
    toYIQ.m[1][1] = -0.274f;
    toYIQ.m[1][2] = -0.322f;
    toYIQ.m[2][0] = 0.211f;
    toYIQ.m[2][1] = -0.523f;
    toYIQ.m[2][2] = 0.312f;

    Mat5 rotate = identity5();
    rotate.m[1][1] = c;
    rotate.m[1][2] = -sn;
    rotate.m[2][1] = sn;
    rotate.m[2][2] = c;

    Mat5 fromYIQ = identity5();
    fromYIQ.m[0][0] = 1.0f;
    fromYIQ.m[0][1] = 0.956f;
    fromYIQ.m[0][2] = 0.621f;
    fromYIQ.m[1][0] = 1.0f;
    fromYIQ.m[1][1] = -0.272f;
    fromYIQ.m[1][2] = -0.647f;
    fromYIQ.m[2][0] = 1.0f;
    fromYIQ.m[2][1] = -1.107f;
    fromYIQ.m[2][2] = 1.705f;

    return multiply(fromYIQ, multiply(rotate, toYIQ));
}

}  // namespace

void downsample2x2(const float* src, int w, int h, int channels, std::vector<float>& out, int& outW,
                   int& outH) {
    outW = (w + 1) / 2;
    outH = (h + 1) / 2;
    const size_t ch = static_cast<size_t>(std::max(1, channels));
    out.resize(static_cast<size_t>(outW) * static_cast<size_t>(outH) * ch);
    if (w <= 0 || h <= 0) {
        return;
    }
    for (int y = 0; y < outH; ++y) {
        const int y0 = std::min(2 * y, h - 1);
        const int y1 = std::min(2 * y + 1, h - 1);
        const float* r0 = src + static_cast<size_t>(y0) * static_cast<size_t>(w) * ch;
        const float* r1 = src + static_cast<size_t>(y1) * static_cast<size_t>(w) * ch;
        float* o = out.data() + static_cast<size_t>(y) * static_cast<size_t>(outW) * ch;
        for (int x = 0; x < outW; ++x) {
            const size_t x0 = static_cast<size_t>(std::min(2 * x, w - 1)) * ch;
            const size_t x1 = static_cast<size_t>(std::min(2 * x + 1, w - 1)) * ch;
            for (size_t c = 0; c < ch; ++c) {
                o[static_cast<size_t>(x) * ch + c] =
                    (r0[x0 + c] + r0[x1 + c] + r1[x0 + c] + r1[x1 + c]) * 0.25f;
            }
        }
    }
}

void upsampleBilinear(const float* src, int sw, int sh, int channels, std::vector<float>& out, int dw,
                      int dh) {
    const size_t ch = static_cast<size_t>(std::max(1, channels));
    out.resize(static_cast<size_t>(std::max(0, dw)) * static_cast<size_t>(std::max(0, dh)) * ch);
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) {
        return;
    }
    for (int y = 0; y < dh; ++y) {
        const float py =
            (static_cast<float>(y) + 0.5f) / static_cast<float>(dh) * static_cast<float>(sh) - 0.5f;
        const int iy = static_cast<int>(std::floor(py));
        const float fy = py - static_cast<float>(iy);
        const int y0 = std::clamp(iy, 0, sh - 1);
        const int y1 = std::clamp(iy + 1, 0, sh - 1);
        const float* r0 = src + static_cast<size_t>(y0) * static_cast<size_t>(sw) * ch;
        const float* r1 = src + static_cast<size_t>(y1) * static_cast<size_t>(sw) * ch;
        float* o = out.data() + static_cast<size_t>(y) * static_cast<size_t>(dw) * ch;
        for (int x = 0; x < dw; ++x) {
            const float px =
                (static_cast<float>(x) + 0.5f) / static_cast<float>(dw) * static_cast<float>(sw) - 0.5f;
            const int ix = static_cast<int>(std::floor(px));
            const float fx = px - static_cast<float>(ix);
            const size_t x0 = static_cast<size_t>(std::clamp(ix, 0, sw - 1)) * ch;
            const size_t x1 = static_cast<size_t>(std::clamp(ix + 1, 0, sw - 1)) * ch;
            const float w00 = (1.0f - fx) * (1.0f - fy), w10 = fx * (1.0f - fy);
            const float w01 = (1.0f - fx) * fy, w11 = fx * fy;
            for (size_t c = 0; c < ch; ++c) {
                o[static_cast<size_t>(x) * ch + c] =
                    r0[x0 + c] * w00 + r0[x1 + c] * w10 + r1[x0 + c] * w01 + r1[x1 + c] * w11;
            }
        }
    }
}

namespace {

/// T-343: the three-pass box blur of `data` (`w x h x channels`, in place through `scratch`) at
/// 1 / 2^shift resolution: downsample, blur with the halved sigmas, upsample back into `data`.
void blurDownsampled(std::vector<float>& data, std::vector<float>& scratch, int w, int h, int channels,
                     float sigmaX, float sigmaY, bool skipX, bool skipY, int shift) {
    std::vector<float> level;
    std::vector<float> next;
    int lw = w, lh = h;
    downsample2x2(data.data(), w, h, channels, level, lw, lh);
    for (int l = 2; l <= shift; ++l) {
        int nw = 0, nh = 0;
        downsample2x2(level.data(), lw, lh, channels, next, nw, nh);
        level.swap(next);
        lw = nw;
        lh = nh;
    }
    const float scale = 1.0f / static_cast<float>(1 << shift);
    bool sx = skipX, sy = skipY;
    const int rx = skipX ? 0 : boxRadius(sigmaX * scale, sx);
    const int ry = skipY ? 0 : boxRadius(sigmaY * scale, sy);
    next.assign(level.size(), 0.0f);
    float* cur = level.data();
    float* tmp = next.data();
    const bool inLevel = runThreeBoxPasses(cur, tmp, lw, lh, channels, rx, sx, ry, sy);
    const std::vector<float>& blurred = inLevel ? level : next;
    upsampleBilinear(blurred.data(), lw, lh, channels, data, w, h);
    scratch.assign(data.size(), 0.0f);
}

}  // namespace

void gaussianBlur(const Surface& src, Surface& dst, Surface& scratch, float sigmaX, float sigmaY,
                  bool premultiply) {
    const int w = src.w;
    const int h = src.h;
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);

    dst.w = w;
    dst.h = h;
    dst.rgba = src.rgba;
    scratch.w = w;
    scratch.h = h;
    scratch.rgba.assign(n * 4, 0.0f);

    // `premultiply` is a no-op since T-102: a Surface is already premultiplied, which is exactly
    // the space a blur has to run in for a translucent edge not to darken (docs/tasks/T-102.md
    // step 2). The parameter stays so `premultiply = false` can still ask for the four channels to
    // be blurred independently... which, for premultiplied data, is the same three box passes.
    (void)premultiply;

    bool skipX = false;
    bool skipY = false;
    int rx = boxRadius(sigmaX, skipX);
    int ry = boxRadius(sigmaY, skipY);
    if (w <= 0 || h <= 0 || (skipX && skipY)) {
        return;
    }
    // T-343 (18 §18.15): large sigmas run at a lower resolution, like the GPU path.
    const int shift = blurDownsampleShift(std::max(skipX ? 0.0f : sigmaX, skipY ? 0.0f : sigmaY));
    if (shift > 0) {
        blurDownsampled(dst.rgba, scratch.rgba, w, h, 4, sigmaX, sigmaY, skipX, skipY, shift);
        return;
    }

    float* cur = dst.rgba.data();
    float* tmp = scratch.rgba.data();
    bool resultInDst = runThreeBoxPasses(cur, tmp, w, h, 4, rx, skipX, ry, skipY);
    if (!resultInDst) {
        std::swap(dst.rgba, scratch.rgba);
    }
}

namespace {

/// Blurs `alpha` (already holding the `w * h` field to blur) in place with an isotropic sigma:
/// three horizontal plus three vertical box passes against `scratch` as the ping-pong buffer.
/// Shared by `gaussianBlurAlpha` and `gaussianBlurAlphaField`.
void blurAlphaInPlace(std::vector<float>& alpha, int w, int h, std::vector<float>& scratch, float sigma) {
    scratch.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0.0f);

    bool skip = false;
    int r = boxRadius(sigma, skip);
    if (skip || w <= 0 || h <= 0) {
        return;
    }
    // T-343 (18 §18.15): a glow's or shadow's mask blurs at a lower resolution too.
    const int shift = blurDownsampleShift(sigma);
    if (shift > 0) {
        blurDownsampled(alpha, scratch, w, h, 1, sigma, sigma, false, false, shift);
        return;
    }

    float* cur = alpha.data();
    float* tmp = scratch.data();
    bool resultInOut = runThreeBoxPasses(cur, tmp, w, h, 1, r, false, r, false);
    if (!resultInOut) {
        std::swap(alpha, scratch);
    }
}

}  // namespace

void gaussianBlurAlpha(const Surface& src, std::vector<float>& outAlpha, std::vector<float>& scratch,
                       float sigma) {
    const int w = src.w;
    const int h = src.h;
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);

    outAlpha.resize(n);
    for (size_t i = 0; i < n; ++i) {
        outAlpha[i] = src.rgba[i * 4 + 3];
    }
    blurAlphaInPlace(outAlpha, w, h, scratch, sigma);
}

void gaussianBlurAlphaField(const std::vector<float>& alpha, int w, int h, std::vector<float>& outAlpha,
                            std::vector<float>& scratch, float sigma) {
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    outAlpha.resize(n);
    for (size_t i = 0; i < n; ++i) {
        outAlpha[i] = alpha[i];
    }
    blurAlphaInPlace(outAlpha, w, h, scratch, sigma);
}

void dilateAlpha(const std::vector<float>& in, std::vector<float>& out, std::vector<float>& scratch, int w,
                 int h, int r) {
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    out.resize(n);
    if (r <= 0) {
        out = in;
        return;
    }
    scratch.assign(n, 0.0f);

    const size_t uw = static_cast<size_t>(w);
    for (int y = 0; y < h; ++y) {
        size_t rowOff = static_cast<size_t>(y) * uw;
        maxFilter1D(in.data() + rowOff, scratch.data() + rowOff, w, r);
    }

    std::vector<float> colIn(static_cast<size_t>(h));
    std::vector<float> colOut(static_cast<size_t>(h));
    for (int x = 0; x < w; ++x) {
        size_t ux = static_cast<size_t>(x);
        for (int y = 0; y < h; ++y) {
            colIn[static_cast<size_t>(y)] = scratch[static_cast<size_t>(y) * uw + ux];
        }
        maxFilter1D(colIn.data(), colOut.data(), h, r);
        for (int y = 0; y < h; ++y) {
            out[static_cast<size_t>(y) * uw + ux] = colOut[static_cast<size_t>(y)];
        }
    }
}

void signedDistance(const std::vector<float>& alpha, std::vector<float>& outSdf, int w, int h) {
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    std::vector<uint8_t> fg(n);
    std::vector<uint8_t> bg(n);
    for (size_t i = 0; i < n; ++i) {
        fg[i] = alpha[i] >= 0.5f ? 1 : 0;
        bg[i] = fg[i] != 0 ? 0 : 1;
    }

    std::vector<float> dOut;  // distance to nearest foreground pixel; 0 on foreground.
    std::vector<float> dIn;   // distance to nearest background pixel; 0 on background.
    distanceTransform(fg, w, h, dOut);
    distanceTransform(bg, w, h, dIn);

    outSdf.resize(n);
    for (size_t i = 0; i < n; ++i) {
        outSdf[i] = dOut[i] - dIn[i];
    }
}

ColorMatrix colorAdjustMatrix(float brightness, float contrast, float saturation, float hueDeg, float invert,
                              float sepia) {
    Mat5 m = multiply(hueMat(hueDeg), multiply(saturationMat(saturation),
                                               multiply(contrastMat(contrast), brightnessMat(brightness))));
    // T-334: invert then sepia after hue, in the fixed slot order (declaration order is not kept).
    m = multiply(sepiaMat(sepia), multiply(invertMat(invert), m));
    ColorMatrix cm{};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 5; ++j) {
            cm.m[i * 5 + j] = m.m[i][j];
        }
    }
    return cm;
}

void applyColorMatrix(Surface& s, const ColorMatrix& m, float opacity) {
    const size_t n = s.rgba.size() / 4;
    for (size_t i = 0; i < n; ++i) {
        float* p = s.rgba.data() + i * 4;
        // The matrix is defined on the colour the viewer sees, so a premultiplied Surface has to
        // divide alpha out around it and multiply it back in (docs/tasks/T-102.md step 2).
        const Color plain = unpremultiply(Color{p[0], p[1], p[2], p[3]});
        const float r = plain.r;
        const float g = plain.g;
        const float b = plain.b;
        const float a = plain.a;
        float outR = m.m[0] * r + m.m[1] * g + m.m[2] * b + m.m[3] * a + m.m[4];
        float outG = m.m[5] * r + m.m[6] * g + m.m[7] * b + m.m[8] * a + m.m[9];
        float outB = m.m[10] * r + m.m[11] * g + m.m[12] * b + m.m[13] * a + m.m[14];
        const Color out = premultiply(Color{std::clamp(outR, 0.0f, 1.0f), std::clamp(outG, 0.0f, 1.0f),
                                            std::clamp(outB, 0.0f, 1.0f), a * opacity});
        p[0] = out.r;
        p[1] = out.g;
        p[2] = out.b;
        p[3] = out.a;
    }
}

}  // namespace xt::sw::fx
