#pragma once

#include <cstdint>

#include "sw_blend.h"
#include "sw_simd.h"
#include "xt/simd.h"

/// The one and only body of every kernel declared in sw_simd.h, templated on the 4-lane vector
/// type (`xt::simd::f32x4s` for the scalar reference, `xt::simd::f32x4` for SSE2 and -- the same
/// source compiled with /arch:AVX2 in sw_simd_avx2.cpp -- for AVX2). See sw_simd.h.
///
/// Pixel convention: float RGBA is premultiplied, lane 3 is alpha; RGBA8 is non-premultiplied
/// unless the caller says otherwise (Docs/tasks/T-102.md step 2).

#ifndef XT_SW_KERNEL_NS
/// The namespace this header's kernels land in. sw_simd_avx2.cpp defines it to a different name
/// before including, so its /arch:AVX2 instantiations are distinct symbols the linker cannot fold
/// into the SSE2 ones.
#define XT_SW_KERNEL_NS simdk
#endif

namespace xt::sw::XT_SW_KERNEL_NS {

/// `simd::load` and friends take a null tag pointer to pick the overload; these wrap that away.
template <class V>
inline V vload(const float* p) {
    return simd::load(static_cast<const V*>(nullptr), p);
}
/// See `vload`.
template <class V>
inline V vsplat(float s) {
    return simd::splat(static_cast<const V*>(nullptr), s);
}
/// See `vload`.
template <class V>
inline V vloadU8(const uint8_t* p) {
    return simd::loadU8(static_cast<const V*>(nullptr), p);
}

/// Mask selecting lane 3 only (used to keep alpha out of the per-channel premultiply/divide).
template <class V>
inline V alphaLaneMask() {
    const float sel[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    return simd::cmpGt(vload<V>(sel), vsplat<V>(0.5f));
}

/// Non-premultiplied -> premultiplied: RGB *= A, A unchanged.
template <class V>
inline V toPremul(V c) {
    return simd::blend(simd::mul(c, simd::splatW(c)), c, alphaLaneMask<V>());
}

/// Premultiplied -> non-premultiplied: RGB /= A (all channels 0 when A == 0), A unchanged. Same
/// rule the scalar src-over used before T-102 (`outA <= 0` writes transparent black).
template <class V>
inline V fromPremul(V c) {
    const V a = simd::splatW(c);
    const V zero = vsplat<V>(0.0f);
    const V live = simd::cmpGt(a, zero);
    // max() keeps the divisor away from 0 so no lane ever produces inf/NaN; `live` then throws
    // the A == 0 lanes away anyway.
    const V scaled = simd::div(c, simd::max(a, vsplat<V>(1e-20f)));
    const V rgb = simd::blend(zero, scaled, live);
    return simd::blend(rgb, a, alphaLaneMask<V>());
}

/// One premultiplied source-over step: `src + dst * oneMinusA`.
template <class V>
inline V over(V src, V dst, V oneMinusA) {
    return simd::fma(dst, oneMinusA, src);
}

// --- (c) float <-> RGBA8 conversion ------------------------------------------------------------

/// See SwSimdOps::rowToU8.
template <class V>
void rowToU8(const float* src, uint8_t* dst, int n, bool dstPremul) {
    for (int i = 0; i < n; ++i) {
        V c = vload<V>(src + static_cast<size_t>(i) * 4);
        if (!dstPremul) {
            c = fromPremul<V>(c);
        }
        simd::storeU8(dst + static_cast<size_t>(i) * 4, c);
    }
}

/// See SwSimdOps::rowFromU8.
template <class V>
void rowFromU8(const uint8_t* src, float* dst, int n, bool srcPremul) {
    for (int i = 0; i < n; ++i) {
        V c = vloadU8<V>(src + static_cast<size_t>(i) * 4);
        if (!srcPremul) {
            c = toPremul<V>(c);
        }
        simd::store(dst + static_cast<size_t>(i) * 4, c);
    }
}

// --- (a) span blending -------------------------------------------------------------------------

/// See SwSimdOps::spanSolidOverF.
template <class V>
void spanSolidOverF(float* dst, int n, const float src[4], float oneMinusA) {
    const V s = vload<V>(src);
    const V k = vsplat<V>(oneMinusA);
    for (int i = 0; i < n; ++i) {
        float* p = dst + static_cast<size_t>(i) * 4;
        simd::store(p, over<V>(s, vload<V>(p), k));
    }
}

/// See SwSimdOps::spanSrcOverF.
template <class V>
void spanSrcOverF(float* dst, const float* src, int n) {
    const V one = vsplat<V>(1.0f);
    for (int i = 0; i < n; ++i) {
        const V s = vload<V>(src + static_cast<size_t>(i) * 4);
        float* p = dst + static_cast<size_t>(i) * 4;
        simd::store(p, over<V>(s, vload<V>(p), simd::sub(one, simd::splatW(s))));
    }
}

/// See SwSimdOps::spanSolidOverU8.
template <class V>
void spanSolidOverU8(uint8_t* dst, int n, const float src[4], float oneMinusA, bool dstPremul) {
    const V s = vload<V>(src);
    const V k = vsplat<V>(oneMinusA);
    for (int i = 0; i < n; ++i) {
        uint8_t* p = dst + static_cast<size_t>(i) * 4;
        V d = vloadU8<V>(p);
        if (!dstPremul) {
            d = toPremul<V>(d);
        }
        V o = over<V>(s, d, k);
        if (!dstPremul) {
            o = fromPremul<V>(o);
        }
        simd::storeU8(p, o);
    }
}

/// See SwSimdOps::spanSrcOverU8.
template <class V>
void spanSrcOverU8(uint8_t* dst, const float* src, int n, bool dstPremul) {
    const V one = vsplat<V>(1.0f);
    for (int i = 0; i < n; ++i) {
        const V s = vload<V>(src + static_cast<size_t>(i) * 4);
        uint8_t* p = dst + static_cast<size_t>(i) * 4;
        V d = vloadU8<V>(p);
        if (!dstPremul) {
            d = toPremul<V>(d);
        }
        V o = over<V>(s, d, simd::sub(one, simd::splatW(s)));
        if (!dstPremul) {
            o = fromPremul<V>(o);
        }
        simd::storeU8(p, o);
    }
}

// --- (b) Gaussian blur box passes ---------------------------------------------------------------

/// See SwSimdOps::boxPassH. `channels == 4` walks whole pixels with one vector each (the four
/// channels of a pixel are contiguous); any other channel count keeps the scalar per-channel
/// sliding window, which is what the alpha-only blur uses.
template <class V>
void boxPassH(const float* src, float* dst, int w, int h, int channels, int radius) {
    if (w <= 0 || h <= 0) {
        return;
    }
    const size_t ch = static_cast<size_t>(channels);
    const int winLen = 2 * radius + 1;
    const size_t rowStride = static_cast<size_t>(w) * ch;
    if (channels == 4) {
        const V len = vsplat<V>(static_cast<float>(winLen));
        for (int y = 0; y < h; ++y) {
            const float* row = src + static_cast<size_t>(y) * rowStride;
            float* outRow = dst + static_cast<size_t>(y) * rowStride;
            V sum = vsplat<V>(0.0f);
            for (int k = -radius; k <= radius; ++k) {
                int xx = k < 0 ? 0 : (k >= w ? w - 1 : k);
                sum = simd::add(sum, vload<V>(row + static_cast<size_t>(xx) * 4));
            }
            simd::store(outRow, simd::div(sum, len));
            for (int x = 1; x < w; ++x) {
                int addX = x + radius;
                if (addX >= w) {
                    addX = w - 1;
                }
                int subX = x - radius - 1;
                if (subX < 0) {
                    subX = 0;
                }
                sum = simd::add(sum, simd::sub(vload<V>(row + static_cast<size_t>(addX) * 4),
                                               vload<V>(row + static_cast<size_t>(subX) * 4)));
                simd::store(outRow + static_cast<size_t>(x) * 4, simd::div(sum, len));
            }
        }
        return;
    }
    for (int y = 0; y < h; ++y) {
        const float* row = src + static_cast<size_t>(y) * rowStride;
        float* outRow = dst + static_cast<size_t>(y) * rowStride;
        for (int c = 0; c < channels; ++c) {
            const size_t cc = static_cast<size_t>(c);
            float sum = 0.0f;
            for (int k = -radius; k <= radius; ++k) {
                int xx = k < 0 ? 0 : (k >= w ? w - 1 : k);
                sum += row[static_cast<size_t>(xx) * ch + cc];
            }
            outRow[cc] = sum / static_cast<float>(winLen);
            for (int x = 1; x < w; ++x) {
                int addX = x + radius;
                if (addX >= w) {
                    addX = w - 1;
                }
                int subX = x - radius - 1;
                if (subX < 0) {
                    subX = 0;
                }
                sum += row[static_cast<size_t>(addX) * ch + cc] - row[static_cast<size_t>(subX) * ch + cc];
                outRow[static_cast<size_t>(x) * ch + cc] = sum / static_cast<float>(winLen);
            }
        }
    }
}

/// See SwSimdOps::boxPassV. A vertical box filter is independent per float *within* a row, so this
/// slides the window down the image over 16 floats at a time (four vectors, one full cache line)
/// regardless of the channel count -- which also replaces the old column-at-a-time walk that
/// touched 4 bytes per cache line.
template <class V>
void boxPassV(const float* src, float* dst, int w, int h, int channels, int radius) {
    if (w <= 0 || h <= 0) {
        return;
    }
    const size_t rowStride = static_cast<size_t>(w) * static_cast<size_t>(channels);
    const int total = w * channels;
    const int winLen = 2 * radius + 1;
    const V len = vsplat<V>(static_cast<float>(winLen));

    constexpr int kBlock = 4;  // vectors per block == 16 floats == one cache line
    int i = 0;
    for (; i + 4 * kBlock <= total; i += 4 * kBlock) {
        V sum[kBlock];
        for (int b = 0; b < kBlock; ++b) {
            sum[b] = vsplat<V>(0.0f);
        }
        for (int k = -radius; k <= radius; ++k) {
            int yy = k < 0 ? 0 : (k >= h ? h - 1 : k);
            const float* p = src + static_cast<size_t>(yy) * rowStride + static_cast<size_t>(i);
            for (int b = 0; b < kBlock; ++b) {
                sum[b] = simd::add(sum[b], vload<V>(p + b * 4));
            }
        }
        for (int b = 0; b < kBlock; ++b) {
            simd::store(dst + static_cast<size_t>(i) + static_cast<size_t>(b) * 4, simd::div(sum[b], len));
        }
        for (int y = 1; y < h; ++y) {
            int addY = y + radius;
            if (addY >= h) {
                addY = h - 1;
            }
            int subY = y - radius - 1;
            if (subY < 0) {
                subY = 0;
            }
            const float* pa = src + static_cast<size_t>(addY) * rowStride + static_cast<size_t>(i);
            const float* ps = src + static_cast<size_t>(subY) * rowStride + static_cast<size_t>(i);
            float* po = dst + static_cast<size_t>(y) * rowStride + static_cast<size_t>(i);
            for (int b = 0; b < kBlock; ++b) {
                sum[b] = simd::add(sum[b], simd::sub(vload<V>(pa + b * 4), vload<V>(ps + b * 4)));
                simd::store(po + b * 4, simd::div(sum[b], len));
            }
        }
    }
    for (; i < total; ++i) {
        const size_t base = static_cast<size_t>(i);
        float sum = 0.0f;
        for (int k = -radius; k <= radius; ++k) {
            int yy = k < 0 ? 0 : (k >= h ? h - 1 : k);
            sum += src[static_cast<size_t>(yy) * rowStride + base];
        }
        dst[base] = sum / static_cast<float>(winLen);
        for (int y = 1; y < h; ++y) {
            int addY = y + radius;
            if (addY >= h) {
                addY = h - 1;
            }
            int subY = y - radius - 1;
            if (subY < 0) {
                subY = 0;
            }
            sum += src[static_cast<size_t>(addY) * rowStride + base] -
                   src[static_cast<size_t>(subY) * rowStride + base];
            dst[static_cast<size_t>(y) * rowStride + base] = sum / static_cast<float>(winLen);
        }
    }
}

// --- (d) layer compositing at opacity < 1 and in the non-Normal blend modes (T-643) -------------

/// RGBA8 -> premultiplied float exactly as sw_blend.h's `loadPixelU8`: a divide by 255 (`vloadU8`
/// multiplies by 1/255, which differs in the last bit for about half the byte values).
template <class V>
inline V loadU8Exact(const uint8_t* p, bool premul) {
    const float t[4] = {static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2]),
                        static_cast<float>(p[3])};
    const V c = simd::div(vload<V>(t), vsplat<V>(255.0f));
    return premul ? c : toPremul<V>(c);
}

/// Premultiplied float -> RGBA8 exactly as sw_blend.h's `storePixelU8`: `unpremultiply` (a plain
/// divide, all four channels 0 when A <= 0 -- unlike `fromPremul`, no lower bound on A) unless
/// `premul`, then `floatToByte`'s rounding (`simd::storeU8`).
template <class V>
inline void storeU8Exact(uint8_t* p, V c, bool premul) {
    if (!premul) {
        const V a = simd::splatW(c);
        const V zero = vsplat<V>(0.0f);
        const V plain = simd::blend(simd::div(c, a), a, alphaLaneMask<V>());
        c = simd::blend(plain, zero, simd::cmpLe(a, zero));
    }
    simd::storeU8(p, c);
}

/// Normal at `opacity`: `blendSrcOverPremul(src * opacity, dst)` per pixel, skipping a pixel whose
/// scaled alpha is `<= 0`.
template <class V>
void layerOverF(float* dst, const float* src, int n, float opacity) {
    const V o = vsplat<V>(opacity);
    const V one = vsplat<V>(1.0f);
    for (int i = 0; i < n; ++i) {
        const float* sp = src + static_cast<size_t>(i) * 4;
        if (sp[3] * opacity <= 0.0f) {
            continue;
        }
        const V s = simd::mul(vload<V>(sp), o);
        float* p = dst + static_cast<size_t>(i) * 4;
        simd::store(p, over<V>(s, vload<V>(p), simd::sub(one, simd::splatW(s))));
    }
}

/// `layerOverF` onto an RGBA8 row.
template <class V>
void layerOverU8(uint8_t* dst, const float* src, int n, float opacity, bool dstPremul) {
    const V o = vsplat<V>(opacity);
    const V one = vsplat<V>(1.0f);
    for (int i = 0; i < n; ++i) {
        const float* sp = src + static_cast<size_t>(i) * 4;
        if (sp[3] * opacity <= 0.0f) {
            continue;
        }
        const V s = simd::mul(vload<V>(sp), o);
        uint8_t* p = dst + static_cast<size_t>(i) * 4;
        storeU8Exact<V>(p, over<V>(s, loadU8Exact<V>(p, dstPremul), simd::sub(one, simd::splatW(s))),
                        dstPremul);
    }
}

/// Three channel vectors of four pixels (one lane per pixel).
template <class V>
struct Rgb4 {
    V r;
    V g;
    V b;
};

/// `std::min(a, b)` lane for lane: `b < a ? b : a` (ties and signed zeros resolved the same way).
template <class V>
inline V stdMin(V a, V b) {
    return simd::blend(a, b, simd::cmpLt(b, a));
}

/// `std::max(a, b)` lane for lane: `a < b ? b : a`.
template <class V>
inline V stdMax(V a, V b) {
    return simd::blend(a, b, simd::cmpLt(a, b));
}

/// sw_blend.cpp's `screenF`: `cb + cs - cb * cs`.
template <class V>
inline V screenV(V cb, V cs) {
    return simd::sub(simd::add(cb, cs), simd::mul(cb, cs));
}

/// sw_blend.cpp's `hardLightF`.
template <class V>
inline V hardLightV(V cb, V cs) {
    const V cs2 = simd::mul(vsplat<V>(2.0f), cs);
    const V lo = simd::mul(cb, cs2);
    const V hi = screenV<V>(cb, simd::sub(cs2, vsplat<V>(1.0f)));
    return simd::blend(hi, lo, simd::cmpLe(cs, vsplat<V>(0.5f)));
}

/// sw_blend.cpp's `softLightF` (and `softLightDF`; the unused branch's square root of a negative
/// value is thrown away by the select).
template <class V>
inline V softLightV(V cb, V cs) {
    const V one = vsplat<V>(1.0f);
    const V cs2 = simd::mul(vsplat<V>(2.0f), cs);
    const V lo = simd::sub(cb, simd::mul(simd::mul(simd::sub(one, cs2), cb), simd::sub(one, cb)));
    const V poly =
        simd::mul(simd::add(simd::mul(simd::sub(simd::mul(vsplat<V>(16.0f), cb), vsplat<V>(12.0f)), cb),
                            vsplat<V>(4.0f)),
                  cb);
    const V d = simd::blend(simd::sqrt(cb), poly, simd::cmpLe(cb, vsplat<V>(0.25f)));
    const V hi = simd::add(cb, simd::mul(simd::sub(cs2, one), simd::sub(d, cb)));
    return simd::blend(hi, lo, simd::cmpLe(cs, vsplat<V>(0.5f)));
}

/// sw_blend.cpp's separable B(Cb, Cs) for every separable mode (Normal returns `cs`).
template <class V>
inline V separableV(BlendMode mode, V cb, V cs) {
    const V zero = vsplat<V>(0.0f);
    const V one = vsplat<V>(1.0f);
    switch (mode) {
        case BlendMode::Multiply:
            return simd::mul(cb, cs);
        case BlendMode::Screen:
            return screenV<V>(cb, cs);
        case BlendMode::Overlay:
            return hardLightV<V>(cs, cb);
        case BlendMode::Darken:
            return stdMin<V>(cb, cs);
        case BlendMode::Lighten:
            return stdMax<V>(cb, cs);
        case BlendMode::ColorDodge: {
            const V q = simd::div(cb, simd::sub(one, cs));
            V r = stdMin<V>(one, q);
            r = simd::blend(r, one, simd::cmpLe(one, cs));
            return simd::blend(r, zero, simd::cmpLe(cb, zero));
        }
        case BlendMode::ColorBurn: {
            const V q = simd::div(simd::sub(one, cb), cs);
            V r = simd::sub(one, stdMin<V>(one, q));
            r = simd::blend(r, zero, simd::cmpLe(cs, zero));
            return simd::blend(r, one, simd::cmpLe(one, cb));
        }
        case BlendMode::HardLight:
            return hardLightV<V>(cb, cs);
        case BlendMode::SoftLight:
            return softLightV<V>(cb, cs);
        case BlendMode::Difference:
            return simd::abs(simd::sub(cb, cs));
        case BlendMode::Exclusion:
            return simd::sub(simd::add(cb, cs), simd::mul(simd::mul(vsplat<V>(2.0f), cb), cs));
        case BlendMode::Add:
            return stdMin<V>(one, simd::add(cb, cs));
        default:
            return cs;
    }
}

/// sw_blend.cpp's `lumOf`: `0.3 r + 0.59 g + 0.11 b`, left to right.
template <class V>
inline V lumV(const Rgb4<V>& c) {
    return simd::add(simd::add(simd::mul(vsplat<V>(0.3f), c.r), simd::mul(vsplat<V>(0.59f), c.g)),
                     simd::mul(vsplat<V>(0.11f), c.b));
}

/// `std::min({r, g, b})` (`min_element`: the first smallest).
template <class V>
inline V min3V(const Rgb4<V>& c) {
    V m = c.r;
    m = simd::blend(m, c.g, simd::cmpLt(c.g, m));
    return simd::blend(m, c.b, simd::cmpLt(c.b, m));
}

/// `std::max({r, g, b})` (`max_element`: the first largest).
template <class V>
inline V max3V(const Rgb4<V>& c) {
    V m = c.r;
    m = simd::blend(m, c.g, simd::cmpLt(m, c.g));
    return simd::blend(m, c.b, simd::cmpLt(m, c.b));
}

/// sw_blend.cpp's `clipColor`: both corrections in order, `l`, `n` and `x` taken from the input.
template <class V>
inline Rgb4<V> clipColorV(Rgb4<V> c) {
    const V zero = vsplat<V>(0.0f);
    const V one = vsplat<V>(1.0f);
    const V l = lumV<V>(c);
    const V n = min3V<V>(c);
    const V x = max3V<V>(c);
    const V low = simd::maskAndNot(simd::cmpLt(n, zero), simd::cmpEq(l, n));
    const V dn = simd::sub(l, n);
    c.r = simd::blend(c.r, simd::add(l, simd::div(simd::mul(simd::sub(c.r, l), l), dn)), low);
    c.g = simd::blend(c.g, simd::add(l, simd::div(simd::mul(simd::sub(c.g, l), l), dn)), low);
    c.b = simd::blend(c.b, simd::add(l, simd::div(simd::mul(simd::sub(c.b, l), l), dn)), low);
    const V high = simd::maskAndNot(simd::cmpLt(one, x), simd::cmpEq(x, l));
    const V ol = simd::sub(one, l);
    const V dx = simd::sub(x, l);
    c.r = simd::blend(c.r, simd::add(l, simd::div(simd::mul(simd::sub(c.r, l), ol), dx)), high);
    c.g = simd::blend(c.g, simd::add(l, simd::div(simd::mul(simd::sub(c.g, l), ol), dx)), high);
    c.b = simd::blend(c.b, simd::add(l, simd::div(simd::mul(simd::sub(c.b, l), ol), dx)), high);
    return c;
}

/// sw_blend.cpp's `setLum`.
template <class V>
inline Rgb4<V> setLumV(Rgb4<V> c, V l) {
    const V d = simd::sub(l, lumV<V>(c));
    c.r = simd::add(c.r, d);
    c.g = simd::add(c.g, d);
    c.b = simd::add(c.b, d);
    return clipColorV<V>(c);
}

/// sw_blend.cpp's `satOf`.
template <class V>
inline V satV(const Rgb4<V>& c) {
    return simd::sub(max3V<V>(c), min3V<V>(c));
}

/// sw_blend.cpp's `setSat`. Its `std::sort` of the three channel indices is an insertion sort for
/// three elements (MSVC STL and libstdc++), which is stable: of equal channels the lower index
/// sorts first, so it is the minimum when they tie low and the maximum is the higher index when
/// they tie high. The masks below pick the same min / mid / max channel per lane.
template <class V>
inline Rgb4<V> setSatV(const Rgb4<V>& c, V s) {
    const V zero = vsplat<V>(0.0f);
    const V all = simd::cmpEq(zero, zero);
    const V gLtR = simd::cmpLt(c.g, c.r);
    const V bLtR = simd::cmpLt(c.b, c.r);
    const V bLtG = simd::cmpLt(c.b, c.g);
    // r is the minimum when neither other channel is below it, the maximum when both are; g sorts
    // after r unless g < r; b sorts after r unless b < r and after g unless b < g.
    const V rMin = simd::maskAndNot(simd::maskAndNot(all, gLtR), bLtR);
    const V rMax = simd::maskAnd(gLtR, bLtR);
    const V gMin = simd::maskAndNot(gLtR, bLtG);
    const V gMax = simd::maskAndNot(bLtG, gLtR);
    const V bMin = simd::maskAnd(bLtR, bLtG);
    const V bMax = simd::maskAndNot(simd::maskAndNot(all, bLtR), bLtG);
    const V rMid = simd::maskAndNot(simd::maskAndNot(all, rMin), rMax);
    const V gMid = simd::maskAndNot(simd::maskAndNot(all, gMin), gMax);
    const V vMin = simd::blend(simd::blend(c.b, c.g, gMin), c.r, rMin);
    const V vMax = simd::blend(simd::blend(c.b, c.g, gMax), c.r, rMax);
    const V vMid = simd::blend(simd::blend(c.b, c.g, gMid), c.r, rMid);
    const V spread = simd::cmpGt(vMax, vMin);
    const V mid =
        simd::blend(zero, simd::div(simd::mul(simd::sub(vMid, vMin), s), simd::sub(vMax, vMin)), spread);
    const V top = simd::blend(zero, s, spread);
    Rgb4<V> out;
    out.r = simd::blend(simd::blend(mid, top, rMax), zero, rMin);
    out.g = simd::blend(simd::blend(mid, top, gMax), zero, gMin);
    out.b = simd::blend(simd::blend(mid, top, bMax), zero, bMin);
    return out;
}

/// sw_blend.cpp's `applyBlendFunc` for four pixels (alpha ignored).
template <class V>
inline Rgb4<V> blendFuncV(BlendMode mode, const Rgb4<V>& cb, const Rgb4<V>& cs) {
    switch (mode) {
        case BlendMode::Hue:
            return setLumV<V>(setSatV<V>(cs, satV<V>(cb)), lumV<V>(cb));
        case BlendMode::Saturation:
            return setLumV<V>(setSatV<V>(cb, satV<V>(cs)), lumV<V>(cb));
        case BlendMode::Color:
            return setLumV<V>(cs, lumV<V>(cb));
        case BlendMode::Luminosity:
            return setLumV<V>(cb, lumV<V>(cs));
        default:
            return Rgb4<V>{separableV<V>(mode, cb.r, cs.r), separableV<V>(mode, cb.g, cs.g),
                           separableV<V>(mode, cb.b, cs.b)};
    }
}

/// `blendPixelPremul(mode, s, d)` for four pixels held one vector per channel (`s[3]` / `d[3]` are
/// the alphas; every lane of `s` has alpha > 0, the caller drops the others). The steps and their
/// order are sw_blend.cpp's: un-premultiply both, B(Cb, Cs), `Co = (1 - ab) Cs + ab B`, `ao = as +
/// ab (1 - as)`, `(as Co + ab Cb (1 - as)) / ao`, premultiply by `ao` (all 0 when `ao <= 0`).
template <class V>
inline void blendQuad(BlendMode mode, const V s[4], const V d[4], V out[4]) {
    const V zero = vsplat<V>(0.0f);
    const V one = vsplat<V>(1.0f);
    const V as = s[3];
    const Rgb4<V> cs{simd::div(s[0], as), simd::div(s[1], as), simd::div(s[2], as)};
    const V dEmpty = simd::cmpLe(d[3], zero);
    const V ab = simd::blend(d[3], zero, dEmpty);
    const Rgb4<V> cb{simd::blend(simd::div(d[0], d[3]), zero, dEmpty),
                     simd::blend(simd::div(d[1], d[3]), zero, dEmpty),
                     simd::blend(simd::div(d[2], d[3]), zero, dEmpty)};
    const Rgb4<V> b = blendFuncV<V>(mode, cb, cs);
    const V keep = simd::sub(one, ab);
    const V oneMinusAs = simd::sub(one, as);
    const V ao = simd::add(as, simd::mul(ab, oneMinusAs));
    const V empty = simd::cmpLe(ao, zero);
    const V csv[3] = {cs.r, cs.g, cs.b};
    const V cbv[3] = {cb.r, cb.g, cb.b};
    const V bv[3] = {b.r, b.g, b.b};
    for (int c = 0; c < 3; ++c) {
        const V co = simd::add(simd::mul(keep, csv[c]), simd::mul(ab, bv[c]));
        const V plain =
            simd::div(simd::add(simd::mul(as, co), simd::mul(simd::mul(ab, cbv[c]), oneMinusAs)), ao);
        out[c] = simd::blend(simd::mul(plain, ao), zero, empty);
    }
    out[3] = simd::blend(ao, zero, empty);
}

/// The scalar reference the two `spanLayerBlend*` kernels must match byte for byte (LayerStack::
/// popLayer's per-pixel loop before T-643), over pixels `[i, n)`: each run's last `n % 4` pixels and
/// any mode value outside BlendMode go through it.
inline void layerBlendScalarF(float* dst, const float* src, int i, int n, float opacity, BlendMode mode) {
    for (; i < n; ++i) {
        const float* sp = src + static_cast<size_t>(i) * 4;
        const Color sc{sp[0] * opacity, sp[1] * opacity, sp[2] * opacity, sp[3] * opacity};
        if (sc.a <= 0.0f) {
            continue;
        }
        float* p = dst + static_cast<size_t>(i) * 4;
        const Color r = blendPixelPremul(mode, sc, Color{p[0], p[1], p[2], p[3]});
        p[0] = r.r;
        p[1] = r.g;
        p[2] = r.b;
        p[3] = r.a;
    }
}

/// `layerBlendScalarF` onto an RGBA8 row.
inline void layerBlendScalarU8(uint8_t* dst, const float* src, int i, int n, float opacity, BlendMode mode,
                               bool dstPremul) {
    for (; i < n; ++i) {
        const float* sp = src + static_cast<size_t>(i) * 4;
        const Color sc{sp[0] * opacity, sp[1] * opacity, sp[2] * opacity, sp[3] * opacity};
        if (sc.a <= 0.0f) {
            continue;
        }
        uint8_t* p = dst + static_cast<size_t>(i) * 4;
        storePixelU8(p, blendPixelPremul(mode, sc, loadPixelU8(p, dstPremul)), dstPremul);
    }
}

/// Loads four layer pixels scaled by `o` into `s` (one vector per channel) and reports which of
/// them are live (scaled alpha > 0, popLayer's rule); false when none is.
template <class V>
inline bool loadLayerQuad(const float* sp, float opacity, V o, V s[4], bool live[4]) {
    bool any = false;
    for (int k = 0; k < 4; ++k) {
        live[k] = !(sp[k * 4 + 3] * opacity <= 0.0f);
        any = any || live[k];
    }
    if (!any) {
        return false;
    }
    for (int k = 0; k < 4; ++k) {
        s[k] = simd::mul(vload<V>(sp + k * 4), o);
    }
    simd::transpose4(s[0], s[1], s[2], s[3]);
    return true;
}

/// The non-Normal modes of `spanLayerBlendF`: four pixels at a time, the last `n % 4` through the
/// scalar reference.
template <class V, BlendMode M>
void layerBlendQuadsF(float* dst, const float* src, int n, float opacity) {
    const V o = vsplat<V>(opacity);
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        V s[4];
        bool live[4];
        if (!loadLayerQuad<V>(src + static_cast<size_t>(i) * 4, opacity, o, s, live)) {
            continue;
        }
        float* dp = dst + static_cast<size_t>(i) * 4;
        V d[4] = {vload<V>(dp), vload<V>(dp + 4), vload<V>(dp + 8), vload<V>(dp + 12)};
        simd::transpose4(d[0], d[1], d[2], d[3]);
        V out[4];
        blendQuad<V>(M, s, d, out);
        simd::transpose4(out[0], out[1], out[2], out[3]);
        for (int k = 0; k < 4; ++k) {
            if (live[k]) {
                simd::store(dp + k * 4, out[k]);
            }
        }
    }
    layerBlendScalarF(dst, src, i, n, opacity, M);
}

/// `layerBlendQuadsF` onto an RGBA8 row.
template <class V, BlendMode M>
void layerBlendQuadsU8(uint8_t* dst, const float* src, int n, float opacity, bool dstPremul) {
    const V o = vsplat<V>(opacity);
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        V s[4];
        bool live[4];
        if (!loadLayerQuad<V>(src + static_cast<size_t>(i) * 4, opacity, o, s, live)) {
            continue;
        }
        uint8_t* dp = dst + static_cast<size_t>(i) * 4;
        V d[4] = {loadU8Exact<V>(dp, dstPremul), loadU8Exact<V>(dp + 4, dstPremul),
                  loadU8Exact<V>(dp + 8, dstPremul), loadU8Exact<V>(dp + 12, dstPremul)};
        simd::transpose4(d[0], d[1], d[2], d[3]);
        V out[4];
        blendQuad<V>(M, s, d, out);
        simd::transpose4(out[0], out[1], out[2], out[3]);
        for (int k = 0; k < 4; ++k) {
            if (live[k]) {
                storeU8Exact<V>(dp + k * 4, out[k], dstPremul);
            }
        }
    }
    layerBlendScalarU8(dst, src, i, n, opacity, M, dstPremul);
}

/// The 16 non-Normal modes, each its own instantiation so the mode switch folds away.
#define XT_SW_LAYER_BLEND_MODES(X) \
    X(Multiply)                    \
    X(Screen)                      \
    X(Overlay)                     \
    X(Darken)                      \
    X(Lighten)                     \
    X(ColorDodge)                  \
    X(ColorBurn)                   \
    X(HardLight)                   \
    X(SoftLight)                   \
    X(Difference)                  \
    X(Exclusion)                   \
    X(Hue)                         \
    X(Saturation)                  \
    X(Color)                       \
    X(Luminosity)                  \
    X(Add)

/// See SwSimdOps::spanLayerBlendF.
template <class V>
void spanLayerBlendF(float* dst, const float* src, int n, float opacity, BlendMode mode) {
    switch (mode) {
#define XT_SW_CASE(M)                                            \
    case BlendMode::M:                                           \
        layerBlendQuadsF<V, BlendMode::M>(dst, src, n, opacity); \
        return;
        XT_SW_LAYER_BLEND_MODES(XT_SW_CASE)
#undef XT_SW_CASE
        case BlendMode::Normal:
            layerOverF<V>(dst, src, n, opacity);
            return;
    }
    layerBlendScalarF(dst, src, 0, n, opacity, mode);
}

/// See SwSimdOps::spanLayerBlendU8.
template <class V>
void spanLayerBlendU8(uint8_t* dst, const float* src, int n, float opacity, BlendMode mode, bool dstPremul) {
    switch (mode) {
#define XT_SW_CASE(M)                                                        \
    case BlendMode::M:                                                       \
        layerBlendQuadsU8<V, BlendMode::M>(dst, src, n, opacity, dstPremul); \
        return;
        XT_SW_LAYER_BLEND_MODES(XT_SW_CASE)
#undef XT_SW_CASE
        case BlendMode::Normal:
            layerOverU8<V>(dst, src, n, opacity, dstPremul);
            return;
    }
    layerBlendScalarU8(dst, src, 0, n, opacity, mode, dstPremul);
}

#undef XT_SW_LAYER_BLEND_MODES

/// Fills a table with the kernels instantiated for `V`.
template <class V>
SwSimdOps makeOps(const char* name) {
    SwSimdOps ops{};
    ops.name = name;
    ops.spanSolidOverF = &spanSolidOverF<V>;
    ops.spanSrcOverF = &spanSrcOverF<V>;
    ops.spanSolidOverU8 = &spanSolidOverU8<V>;
    ops.spanSrcOverU8 = &spanSrcOverU8<V>;
    ops.boxPassH = &boxPassH<V>;
    ops.boxPassV = &boxPassV<V>;
    ops.rowToU8 = &rowToU8<V>;
    ops.rowFromU8 = &rowFromU8<V>;
    ops.spanLayerBlendF = &spanLayerBlendF<V>;
    ops.spanLayerBlendU8 = &spanLayerBlendU8<V>;
    return ops;
}

}  // namespace xt::sw::XT_SW_KERNEL_NS
