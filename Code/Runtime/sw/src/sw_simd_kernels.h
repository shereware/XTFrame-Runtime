#pragma once

#include <cstdint>

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
    return ops;
}

}  // namespace xt::sw::XT_SW_KERNEL_NS
