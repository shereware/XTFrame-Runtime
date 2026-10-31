#pragma once

#include <cstdint>

/// Run-time SIMD dispatch for the SW backend's three measured hot spots (Docs/tasks/T-102.md
/// step 3/4, Docs/design/23-host-integration.md §23.5):
///   (a) span blending  -- `spanSolidOver*` (one constant paint colour) and `spanSrcOver*`
///       (per-pixel source, i.e. the gradient/image/layer-composite case);
///   (b) the Gaussian blur's horizontal and vertical box passes -- `boxPassH` / `boxPassV`;
///   (c) float <-> RGBA8 conversion -- `rowToU8` / `rowFromU8` (the u8 span kernels use the same
///       two conversions inline, fused with the blend).
///
/// Every kernel exists three times from one template body (sw_simd_kernels.h): a scalar reference,
/// an SSE2 build and an AVX2 build (a separate translation unit compiled with /arch:AVX2 but without
/// FMA or contraction, so the same source becomes VEX-encoded and writes the same bytes as the SSE2
/// build, Docs/design/23-host-integration.md §23.5). `swSimd()` picks one table once, on first
/// use, from `__cpuid`; `XT_SW_FORCE_SCALAR=1` in the environment forces the scalar table, which is
/// what the SIMD-vs-scalar tests compare against, and `XT_SW_NO_AVX2=1` holds it at SSE2.
///
/// All float pixels are **premultiplied** RGBA (Docs/tasks/T-102.md step 2). RGBA8 buffers are
/// non-premultiplied unless the caller passes `dstPremul`/`srcPremul` (`SwTarget::premultiplied`).

namespace xt::sw {

/// One complete set of kernels; see the file comment.
struct SwSimdOps {
    /// "scalar", "sse2" or "avx2"; reported by the SIMD-vs-scalar tests.
    const char* name;

    /// `dst[i] = src + dst[i] * oneMinusA` over `n` premultiplied float pixels. `src` is already
    /// multiplied by the effective alpha `a`, and `oneMinusA` is `1 - a`.
    void (*spanSolidOverF)(float* dst, int n, const float src[4], float oneMinusA);
    /// `dst[i] = src[i] + dst[i] * (1 - src[i].a)` over `n` premultiplied float pixels.
    void (*spanSrcOverF)(float* dst, const float* src, int n);
    /// `spanSolidOverF` onto an RGBA8 row (converted in and out unless `dstPremul`).
    void (*spanSolidOverU8)(uint8_t* dst, int n, const float src[4], float oneMinusA, bool dstPremul);
    /// `spanSrcOverF` onto an RGBA8 row (converted in and out unless `dstPremul`).
    void (*spanSrcOverU8)(uint8_t* dst, const float* src, int n, bool dstPremul);

    /// One horizontal box-filter pass; see sw_effect_math.cpp's scalar description.
    void (*boxPassH)(const float* src, float* dst, int w, int h, int channels, int radius);
    /// One vertical box-filter pass; see sw_effect_math.cpp's scalar description.
    void (*boxPassV)(const float* src, float* dst, int w, int h, int channels, int radius);

    /// `n` premultiplied float pixels -> RGBA8 (un-premultiplied unless `dstPremul`).
    void (*rowToU8)(const float* src, uint8_t* dst, int n, bool dstPremul);
    /// `n` RGBA8 pixels -> premultiplied float (premultiplied unless `srcPremul`).
    void (*rowFromU8)(const uint8_t* src, float* dst, int n, bool srcPremul);
};

/// The table chosen for this process. Thread-safe (initialized once by a function-local static).
const SwSimdOps& swSimd();

/// The AVX2 table, built in sw_simd_avx2.cpp. Only `swSimd()` should call this (and the test that
/// checks it against the SSE2 table, after `swCpuHasAvx2()`).
const SwSimdOps& swSimdAvx2Ops();

/// True when the CPU reports AVX2 and the OS saves YMM state, i.e. when `swSimdAvx2Ops()` may run.
/// Always false on non-x86 targets.
bool swCpuHasAvx2();

}  // namespace xt::sw
