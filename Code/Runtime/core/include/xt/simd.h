#pragma once

#include <cstdint>
#include <cstring>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define XT_SIMD_SSE2 1
#include <emmintrin.h>
#else
#define XT_SIMD_SSE2 0
#endif

/// Minimal, dependency-free, header-only 4-lane float vector used by the SW backend's hot pixel
/// loops (Docs/design/23-host-integration.md §23.5, Docs/tasks/T-102.md step 3). Two types with
/// the same 15-function interface:
///   - `f32x4`  -- SSE2 (`__m128`) where the target has it, a plain scalar struct otherwise;
///   - `f32x4s` -- always scalar, the reference the SIMD kernels are checked against and what
///                 `XT_SW_FORCE_SCALAR=1` selects at run time.
/// Every kernel in sw_simd_kernels.h is a template over one of the two, so there is exactly one
/// copy of each algorithm. Lane order matches the RGBA memory order: lane 0 is R, lane 3 is A.
///
/// No exceptions, no RTTI, no allocation, nothing outside <cstdint>/<cstring> and the intrinsic
/// header (Docs/design/01-architecture.md §1.7: xt_core stays dependency-free).

namespace xt::simd {

/// Always-scalar 4-lane vector; see the file comment.
struct f32x4s {
    float v[4];
};

#if XT_SIMD_SSE2
/// SSE2-backed 4-lane vector.
struct f32x4 {
    __m128 v;
};
#else
/// Scalar stand-in for `f32x4` on targets without SSE2; same interface, no vector registers.
struct f32x4 {
    float v[4];
};
#endif

// --- f32x4s (scalar reference) ---------------------------------------------------------------

/// Loads 4 consecutive floats.
inline f32x4s load(const f32x4s*, const float* p) { return f32x4s{{p[0], p[1], p[2], p[3]}}; }
/// Broadcasts one float to all 4 lanes.
inline f32x4s splat(const f32x4s*, float s) { return f32x4s{{s, s, s, s}}; }
/// Stores 4 consecutive floats.
inline void store(float* p, f32x4s a) {
    p[0] = a.v[0];
    p[1] = a.v[1];
    p[2] = a.v[2];
    p[3] = a.v[3];
}
/// Lane-wise `a + b`.
inline f32x4s add(f32x4s a, f32x4s b) {
    return f32x4s{{a.v[0] + b.v[0], a.v[1] + b.v[1], a.v[2] + b.v[2], a.v[3] + b.v[3]}};
}
/// Lane-wise `a - b`.
inline f32x4s sub(f32x4s a, f32x4s b) {
    return f32x4s{{a.v[0] - b.v[0], a.v[1] - b.v[1], a.v[2] - b.v[2], a.v[3] - b.v[3]}};
}
/// Lane-wise `a * b`.
inline f32x4s mul(f32x4s a, f32x4s b) {
    return f32x4s{{a.v[0] * b.v[0], a.v[1] * b.v[1], a.v[2] * b.v[2], a.v[3] * b.v[3]}};
}
/// Lane-wise `a / b`.
inline f32x4s div(f32x4s a, f32x4s b) {
    return f32x4s{{a.v[0] / b.v[0], a.v[1] / b.v[1], a.v[2] / b.v[2], a.v[3] / b.v[3]}};
}
/// Lane-wise `a * b + c` (a multiply followed by an add; no fused rounding is promised).
inline f32x4s fma(f32x4s a, f32x4s b, f32x4s c) { return add(mul(a, b), c); }
/// Lane-wise minimum.
inline f32x4s min(f32x4s a, f32x4s b) {
    return f32x4s{{a.v[0] < b.v[0] ? a.v[0] : b.v[0], a.v[1] < b.v[1] ? a.v[1] : b.v[1],
                   a.v[2] < b.v[2] ? a.v[2] : b.v[2], a.v[3] < b.v[3] ? a.v[3] : b.v[3]}};
}
/// Lane-wise maximum.
inline f32x4s max(f32x4s a, f32x4s b) {
    return f32x4s{{a.v[0] > b.v[0] ? a.v[0] : b.v[0], a.v[1] > b.v[1] ? a.v[1] : b.v[1],
                   a.v[2] > b.v[2] ? a.v[2] : b.v[2], a.v[3] > b.v[3] ? a.v[3] : b.v[3]}};
}
/// Lane-wise `a > b` as an all-ones / all-zeros bit mask, for `blend`.
inline f32x4s cmpGt(f32x4s a, f32x4s b) {
    f32x4s r{};
    const uint32_t ones = 0xFFFFFFFFu;
    const uint32_t zero = 0u;
    for (int i = 0; i < 4; ++i) {
        uint32_t bits = a.v[i] > b.v[i] ? ones : zero;
        std::memcpy(&r.v[i], &bits, sizeof(bits));
    }
    return r;
}
/// Per lane: `mask` all-ones selects `b`, all-zeros selects `a`.
inline f32x4s blend(f32x4s a, f32x4s b, f32x4s mask) {
    f32x4s r{};
    for (int i = 0; i < 4; ++i) {
        uint32_t ma = 0;
        uint32_t ba = 0;
        uint32_t bb = 0;
        std::memcpy(&ma, &mask.v[i], sizeof(ma));
        std::memcpy(&ba, &a.v[i], sizeof(ba));
        std::memcpy(&bb, &b.v[i], sizeof(bb));
        uint32_t out = (ba & ~ma) | (bb & ma);
        std::memcpy(&r.v[i], &out, sizeof(out));
    }
    return r;
}
/// Broadcasts lane 3 (alpha) to all 4 lanes.
inline f32x4s splatW(f32x4s a) { return f32x4s{{a.v[3], a.v[3], a.v[3], a.v[3]}}; }
/// Loads 4 bytes and scales them to [0, 1].
inline f32x4s loadU8(const f32x4s*, const uint8_t* p) {
    const float k = 1.0f / 255.0f;
    return f32x4s{{static_cast<float>(p[0]) * k, static_cast<float>(p[1]) * k, static_cast<float>(p[2]) * k,
                   static_cast<float>(p[3]) * k}};
}
/// Clamps to [0, 1], scales by 255 and rounds half up, storing 4 bytes. Matches
/// sw_blend.h's `floatToByte` exactly, so the SIMD and scalar paths agree bit for bit.
inline void storeU8(uint8_t* p, f32x4s a) {
    for (int i = 0; i < 4; ++i) {
        float c = a.v[i] < 0.0f ? 0.0f : (a.v[i] > 1.0f ? 1.0f : a.v[i]);
        p[i] = static_cast<uint8_t>(static_cast<int>(c * 255.0f + 0.5f));
    }
}

// --- f32x4 (SSE2 where available) ------------------------------------------------------------

#if XT_SIMD_SSE2

/// See the `f32x4s` overload.
inline f32x4 load(const f32x4*, const float* p) { return f32x4{_mm_loadu_ps(p)}; }
/// See the `f32x4s` overload.
inline f32x4 splat(const f32x4*, float s) { return f32x4{_mm_set1_ps(s)}; }
/// See the `f32x4s` overload.
inline void store(float* p, f32x4 a) { _mm_storeu_ps(p, a.v); }
/// See the `f32x4s` overload.
inline f32x4 add(f32x4 a, f32x4 b) { return f32x4{_mm_add_ps(a.v, b.v)}; }
/// See the `f32x4s` overload.
inline f32x4 sub(f32x4 a, f32x4 b) { return f32x4{_mm_sub_ps(a.v, b.v)}; }
/// See the `f32x4s` overload.
inline f32x4 mul(f32x4 a, f32x4 b) { return f32x4{_mm_mul_ps(a.v, b.v)}; }
/// See the `f32x4s` overload.
inline f32x4 div(f32x4 a, f32x4 b) { return f32x4{_mm_div_ps(a.v, b.v)}; }
/// See the `f32x4s` overload. Compiled with /arch:AVX2 this contracts into a single vfmadd.
inline f32x4 fma(f32x4 a, f32x4 b, f32x4 c) { return f32x4{_mm_add_ps(_mm_mul_ps(a.v, b.v), c.v)}; }
/// See the `f32x4s` overload.
inline f32x4 min(f32x4 a, f32x4 b) { return f32x4{_mm_min_ps(a.v, b.v)}; }
/// See the `f32x4s` overload.
inline f32x4 max(f32x4 a, f32x4 b) { return f32x4{_mm_max_ps(a.v, b.v)}; }
/// See the `f32x4s` overload.
inline f32x4 cmpGt(f32x4 a, f32x4 b) { return f32x4{_mm_cmpgt_ps(a.v, b.v)}; }
/// See the `f32x4s` overload.
inline f32x4 blend(f32x4 a, f32x4 b, f32x4 mask) {
    return f32x4{_mm_or_ps(_mm_andnot_ps(mask.v, a.v), _mm_and_ps(mask.v, b.v))};
}
/// See the `f32x4s` overload.
inline f32x4 splatW(f32x4 a) { return f32x4{_mm_shuffle_ps(a.v, a.v, _MM_SHUFFLE(3, 3, 3, 3))}; }
/// See the `f32x4s` overload.
inline f32x4 loadU8(const f32x4*, const uint8_t* p) {
    uint32_t packed = 0;
    std::memcpy(&packed, p, sizeof(packed));
    __m128i bytes = _mm_cvtsi32_si128(static_cast<int>(packed));
    __m128i zero = _mm_setzero_si128();
    __m128i words = _mm_unpacklo_epi8(bytes, zero);
    __m128i dwords = _mm_unpacklo_epi16(words, zero);
    return f32x4{_mm_mul_ps(_mm_cvtepi32_ps(dwords), _mm_set1_ps(1.0f / 255.0f))};
}
/// See the `f32x4s` overload. `+0.5` then truncate is exactly `floatToByte`'s round-half-up on a
/// value already clamped to [0, 1], so no separate rounding-mode dance is needed.
inline void storeU8(uint8_t* p, f32x4 a) {
    __m128 c = _mm_min_ps(_mm_max_ps(a.v, _mm_setzero_ps()), _mm_set1_ps(1.0f));
    __m128 scaled = _mm_add_ps(_mm_mul_ps(c, _mm_set1_ps(255.0f)), _mm_set1_ps(0.5f));
    __m128i ints = _mm_cvttps_epi32(scaled);
    __m128i words = _mm_packs_epi32(ints, ints);
    __m128i bytes = _mm_packus_epi16(words, words);
    uint32_t packed = static_cast<uint32_t>(_mm_cvtsi128_si32(bytes));
    std::memcpy(p, &packed, sizeof(packed));
}

#else

/// See the `f32x4s` overload (this build has no SSE2; `f32x4` is scalar too).
inline f32x4 load(const f32x4*, const float* p) { return f32x4{{p[0], p[1], p[2], p[3]}}; }
/// See the `f32x4s` overload.
inline f32x4 splat(const f32x4*, float s) { return f32x4{{s, s, s, s}}; }
/// See the `f32x4s` overload.
inline void store(float* p, f32x4 a) {
    p[0] = a.v[0];
    p[1] = a.v[1];
    p[2] = a.v[2];
    p[3] = a.v[3];
}
/// See the `f32x4s` overload.
inline f32x4 add(f32x4 a, f32x4 b) {
    return f32x4{{a.v[0] + b.v[0], a.v[1] + b.v[1], a.v[2] + b.v[2], a.v[3] + b.v[3]}};
}
/// See the `f32x4s` overload.
inline f32x4 sub(f32x4 a, f32x4 b) {
    return f32x4{{a.v[0] - b.v[0], a.v[1] - b.v[1], a.v[2] - b.v[2], a.v[3] - b.v[3]}};
}
/// See the `f32x4s` overload.
inline f32x4 mul(f32x4 a, f32x4 b) {
    return f32x4{{a.v[0] * b.v[0], a.v[1] * b.v[1], a.v[2] * b.v[2], a.v[3] * b.v[3]}};
}
/// See the `f32x4s` overload.
inline f32x4 div(f32x4 a, f32x4 b) {
    return f32x4{{a.v[0] / b.v[0], a.v[1] / b.v[1], a.v[2] / b.v[2], a.v[3] / b.v[3]}};
}
/// See the `f32x4s` overload.
inline f32x4 fma(f32x4 a, f32x4 b, f32x4 c) { return add(mul(a, b), c); }
/// See the `f32x4s` overload.
inline f32x4 min(f32x4 a, f32x4 b) {
    return f32x4{{a.v[0] < b.v[0] ? a.v[0] : b.v[0], a.v[1] < b.v[1] ? a.v[1] : b.v[1],
                  a.v[2] < b.v[2] ? a.v[2] : b.v[2], a.v[3] < b.v[3] ? a.v[3] : b.v[3]}};
}
/// See the `f32x4s` overload.
inline f32x4 max(f32x4 a, f32x4 b) {
    return f32x4{{a.v[0] > b.v[0] ? a.v[0] : b.v[0], a.v[1] > b.v[1] ? a.v[1] : b.v[1],
                  a.v[2] > b.v[2] ? a.v[2] : b.v[2], a.v[3] > b.v[3] ? a.v[3] : b.v[3]}};
}
/// See the `f32x4s` overload.
inline f32x4 cmpGt(f32x4 a, f32x4 b) {
    f32x4 r{};
    for (int i = 0; i < 4; ++i) {
        uint32_t bits = a.v[i] > b.v[i] ? 0xFFFFFFFFu : 0u;
        std::memcpy(&r.v[i], &bits, sizeof(bits));
    }
    return r;
}
/// See the `f32x4s` overload.
inline f32x4 blend(f32x4 a, f32x4 b, f32x4 mask) {
    f32x4 r{};
    for (int i = 0; i < 4; ++i) {
        uint32_t ma = 0;
        uint32_t ba = 0;
        uint32_t bb = 0;
        std::memcpy(&ma, &mask.v[i], sizeof(ma));
        std::memcpy(&ba, &a.v[i], sizeof(ba));
        std::memcpy(&bb, &b.v[i], sizeof(bb));
        uint32_t out = (ba & ~ma) | (bb & ma);
        std::memcpy(&r.v[i], &out, sizeof(out));
    }
    return r;
}
/// See the `f32x4s` overload.
inline f32x4 splatW(f32x4 a) { return f32x4{{a.v[3], a.v[3], a.v[3], a.v[3]}}; }
/// See the `f32x4s` overload.
inline f32x4 loadU8(const f32x4*, const uint8_t* p) {
    const float k = 1.0f / 255.0f;
    return f32x4{{static_cast<float>(p[0]) * k, static_cast<float>(p[1]) * k, static_cast<float>(p[2]) * k,
                  static_cast<float>(p[3]) * k}};
}
/// See the `f32x4s` overload.
inline void storeU8(uint8_t* p, f32x4 a) {
    for (int i = 0; i < 4; ++i) {
        float c = a.v[i] < 0.0f ? 0.0f : (a.v[i] > 1.0f ? 1.0f : a.v[i]);
        p[i] = static_cast<uint8_t>(static_cast<int>(c * 255.0f + 0.5f));
    }
}

#endif  // XT_SIMD_SSE2

}  // namespace xt::simd
