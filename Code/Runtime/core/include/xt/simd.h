#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define XT_SIMD_SSE2 1
#include <emmintrin.h>
#else
#define XT_SIMD_SSE2 0
#endif

#if !XT_SIMD_SSE2 && defined(__wasm_simd128__)
#define XT_SIMD_WASM 1
#include <wasm_simd128.h>
#else
#define XT_SIMD_WASM 0
#endif

/// Minimal, dependency-free, header-only 4-lane float vector used by the SW backend's hot pixel
/// loops (Docs/design/23-host-integration.md §23.5, Docs/tasks/T-102.md step 3). Two types with
/// the same interface (15 functions, plus T-643's comparisons, mask logic, `sqrt`, `abs` and
/// `transpose4`, which the layer blend-mode kernels need):
///   - `f32x4`  -- one of three backends, picked at compile time: SSE2 (`__m128`) where the target
///                 has it; wasm SIMD128 (`v128_t`, <wasm_simd128.h>) under `-msimd128` without
///                 SSE2 (Docs/design/52-web-wasm.md §52.2, Docs/tasks/T-531.md), bit-identical to
///                 the SSE2 backend; a plain scalar struct otherwise;
///   - `f32x4s` -- always scalar, the reference the SIMD kernels are checked against and what
///                 `XT_SW_FORCE_SCALAR=1` selects at run time.
/// Every kernel in sw_simd_kernels.h is a template over one of the two, so there is exactly one
/// copy of each algorithm. Lane order matches the RGBA memory order: lane 0 is R, lane 3 is A.
///
/// No exceptions, no RTTI, no allocation, nothing outside <cmath>/<cstdint>/<cstring> and the
/// intrinsic header (Docs/design/01-architecture.md §1.7: xt_core stays dependency-free).

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
#elif XT_SIMD_WASM
/// wasm SIMD128-backed 4-lane vector.
struct f32x4 {
    v128_t v;
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
/// Lane-wise `a < b` (`op` 0), `a <= b` (1) or `a == b` (2) as a bit mask.
inline f32x4s cmpOp(f32x4s a, f32x4s b, int op) {
    f32x4s r{};
    for (int i = 0; i < 4; ++i) {
        const bool t = op == 0 ? a.v[i] < b.v[i] : (op == 1 ? a.v[i] <= b.v[i] : a.v[i] == b.v[i]);
        uint32_t bits = t ? 0xFFFFFFFFu : 0u;
        std::memcpy(&r.v[i], &bits, sizeof(bits));
    }
    return r;
}
/// Lane-wise `a < b` as a bit mask (T-643).
inline f32x4s cmpLt(f32x4s a, f32x4s b) { return cmpOp(a, b, 0); }
/// Lane-wise `a <= b` as a bit mask (T-643).
inline f32x4s cmpLe(f32x4s a, f32x4s b) { return cmpOp(a, b, 1); }
/// Lane-wise `a == b` as a bit mask (T-643).
inline f32x4s cmpEq(f32x4s a, f32x4s b) { return cmpOp(a, b, 2); }
/// Bitwise `a & b`, `a | b` (`op` 1) or `a & ~b` (`op` 2) of two lanes' bits.
inline f32x4s maskOp(f32x4s a, f32x4s b, int op) {
    f32x4s r{};
    for (int i = 0; i < 4; ++i) {
        uint32_t x = 0;
        uint32_t y = 0;
        std::memcpy(&x, &a.v[i], sizeof(x));
        std::memcpy(&y, &b.v[i], sizeof(y));
        x = op == 0 ? (x & y) : (op == 1 ? (x | y) : (x & ~y));
        std::memcpy(&r.v[i], &x, sizeof(x));
    }
    return r;
}
/// Bitwise `a & b` of two masks (T-643).
inline f32x4s maskAnd(f32x4s a, f32x4s b) { return maskOp(a, b, 0); }
/// Bitwise `a | b` of two masks (T-643).
inline f32x4s maskOr(f32x4s a, f32x4s b) { return maskOp(a, b, 1); }
/// Bitwise `a & ~b` of two masks (T-643).
inline f32x4s maskAndNot(f32x4s a, f32x4s b) { return maskOp(a, b, 2); }
/// Lane-wise correctly rounded square root, `std::sqrt` (T-643).
inline f32x4s sqrt(f32x4s a) {
    return f32x4s{{std::sqrt(a.v[0]), std::sqrt(a.v[1]), std::sqrt(a.v[2]), std::sqrt(a.v[3])}};
}
/// Lane-wise absolute value: the sign bit cleared, like `std::fabs` (T-643).
inline f32x4s abs(f32x4s a) {
    f32x4s r{};
    for (int i = 0; i < 4; ++i) {
        uint32_t x = 0;
        std::memcpy(&x, &a.v[i], sizeof(x));
        x &= 0x7FFFFFFFu;
        std::memcpy(&r.v[i], &x, sizeof(x));
    }
    return r;
}
/// Transposes the 4 x 4 matrix whose rows are `a`, `b`, `c`, `d` in place: four RGBA pixels become
/// the R, G, B and A vectors of those pixels, and back (T-643).
inline void transpose4(f32x4s& a, f32x4s& b, f32x4s& c, f32x4s& d) {
    const f32x4s r[4] = {a, b, c, d};
    for (int i = 0; i < 4; ++i) {
        a.v[i] = r[i].v[0];
        b.v[i] = r[i].v[1];
        c.v[i] = r[i].v[2];
        d.v[i] = r[i].v[3];
    }
}
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
/// See the `f32x4s` overload. Two roundings on every backend: the AVX2 unit is built without FMA
/// so all tables stay byte-identical (Docs/design/23-host-integration.md §23.5, T-529).
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
inline f32x4 cmpLt(f32x4 a, f32x4 b) { return f32x4{_mm_cmplt_ps(a.v, b.v)}; }
/// See the `f32x4s` overload.
inline f32x4 cmpLe(f32x4 a, f32x4 b) { return f32x4{_mm_cmple_ps(a.v, b.v)}; }
/// See the `f32x4s` overload.
inline f32x4 cmpEq(f32x4 a, f32x4 b) { return f32x4{_mm_cmpeq_ps(a.v, b.v)}; }
/// See the `f32x4s` overload.
inline f32x4 maskAnd(f32x4 a, f32x4 b) { return f32x4{_mm_and_ps(a.v, b.v)}; }
/// See the `f32x4s` overload.
inline f32x4 maskOr(f32x4 a, f32x4 b) { return f32x4{_mm_or_ps(a.v, b.v)}; }
/// See the `f32x4s` overload. `_mm_andnot_ps(x, y)` is `~x & y`.
inline f32x4 maskAndNot(f32x4 a, f32x4 b) { return f32x4{_mm_andnot_ps(b.v, a.v)}; }
/// See the `f32x4s` overload. `sqrtps` is correctly rounded, like `std::sqrt`.
inline f32x4 sqrt(f32x4 a) { return f32x4{_mm_sqrt_ps(a.v)}; }
/// See the `f32x4s` overload.
inline f32x4 abs(f32x4 a) { return f32x4{_mm_andnot_ps(_mm_set1_ps(-0.0f), a.v)}; }
/// See the `f32x4s` overload.
inline void transpose4(f32x4& a, f32x4& b, f32x4& c, f32x4& d) { _MM_TRANSPOSE4_PS(a.v, b.v, c.v, d.v); }
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

#elif XT_SIMD_WASM

// Every function here produces the same bits as its SSE2 counterpart above (Docs/tasks/T-531.md
// step 1). wasm has no FMA without relaxed-simd, so nothing is fused.

/// See the `f32x4s` overload. Unaligned loads are fine.
inline f32x4 load(const f32x4*, const float* p) { return f32x4{wasm_v128_load(p)}; }
/// See the `f32x4s` overload.
inline f32x4 splat(const f32x4*, float s) { return f32x4{wasm_f32x4_splat(s)}; }
/// See the `f32x4s` overload.
inline void store(float* p, f32x4 a) { wasm_v128_store(p, a.v); }
/// See the `f32x4s` overload.
inline f32x4 add(f32x4 a, f32x4 b) { return f32x4{wasm_f32x4_add(a.v, b.v)}; }
/// See the `f32x4s` overload.
inline f32x4 sub(f32x4 a, f32x4 b) { return f32x4{wasm_f32x4_sub(a.v, b.v)}; }
/// See the `f32x4s` overload.
inline f32x4 mul(f32x4 a, f32x4 b) { return f32x4{wasm_f32x4_mul(a.v, b.v)}; }
/// See the `f32x4s` overload.
inline f32x4 div(f32x4 a, f32x4 b) { return f32x4{wasm_f32x4_div(a.v, b.v)}; }
/// See the `f32x4s` overload. A multiply then an add, as in the SSE2 backend.
inline f32x4 fma(f32x4 a, f32x4 b, f32x4 c) { return add(mul(a, b), c); }
/// See the `f32x4s` overload. `pmin(x, y)` is `y < x ? y : x`, so `pmin(b, a)` is x86
/// `_mm_min_ps(a, b)` = `a < b ? a : b`, NaN and signed zeros included (both return `b`).
inline f32x4 min(f32x4 a, f32x4 b) { return f32x4{wasm_f32x4_pmin(b.v, a.v)}; }
/// See the `f32x4s` overload. `pmax(b, a)` is x86 `_mm_max_ps(a, b)` = `a > b ? a : b`.
inline f32x4 max(f32x4 a, f32x4 b) { return f32x4{wasm_f32x4_pmax(b.v, a.v)}; }
/// See the `f32x4s` overload.
inline f32x4 cmpGt(f32x4 a, f32x4 b) { return f32x4{wasm_f32x4_gt(a.v, b.v)}; }
/// See the `f32x4s` overload. `bitselect(x, y, m)` takes `x` where `m` is 1.
inline f32x4 blend(f32x4 a, f32x4 b, f32x4 mask) { return f32x4{wasm_v128_bitselect(b.v, a.v, mask.v)}; }
/// See the `f32x4s` overload.
inline f32x4 splatW(f32x4 a) { return f32x4{wasm_i32x4_shuffle(a.v, a.v, 3, 3, 3, 3)}; }
/// See the `f32x4s` overload.
inline f32x4 cmpLt(f32x4 a, f32x4 b) { return f32x4{wasm_f32x4_lt(a.v, b.v)}; }
/// See the `f32x4s` overload.
inline f32x4 cmpLe(f32x4 a, f32x4 b) { return f32x4{wasm_f32x4_le(a.v, b.v)}; }
/// See the `f32x4s` overload.
inline f32x4 cmpEq(f32x4 a, f32x4 b) { return f32x4{wasm_f32x4_eq(a.v, b.v)}; }
/// See the `f32x4s` overload.
inline f32x4 maskAnd(f32x4 a, f32x4 b) { return f32x4{wasm_v128_and(a.v, b.v)}; }
/// See the `f32x4s` overload.
inline f32x4 maskOr(f32x4 a, f32x4 b) { return f32x4{wasm_v128_or(a.v, b.v)}; }
/// See the `f32x4s` overload. `wasm_v128_andnot(x, y)` is `x & ~y`.
inline f32x4 maskAndNot(f32x4 a, f32x4 b) { return f32x4{wasm_v128_andnot(a.v, b.v)}; }
/// See the `f32x4s` overload.
inline f32x4 sqrt(f32x4 a) { return f32x4{wasm_f32x4_sqrt(a.v)}; }
/// See the `f32x4s` overload.
inline f32x4 abs(f32x4 a) { return f32x4{wasm_f32x4_abs(a.v)}; }
/// See the `f32x4s` overload.
inline void transpose4(f32x4& a, f32x4& b, f32x4& c, f32x4& d) {
    const v128_t t0 = wasm_i32x4_shuffle(a.v, b.v, 0, 4, 1, 5);
    const v128_t t1 = wasm_i32x4_shuffle(c.v, d.v, 0, 4, 1, 5);
    const v128_t t2 = wasm_i32x4_shuffle(a.v, b.v, 2, 6, 3, 7);
    const v128_t t3 = wasm_i32x4_shuffle(c.v, d.v, 2, 6, 3, 7);
    a.v = wasm_i32x4_shuffle(t0, t1, 0, 1, 4, 5);
    b.v = wasm_i32x4_shuffle(t0, t1, 2, 3, 6, 7);
    c.v = wasm_i32x4_shuffle(t2, t3, 0, 1, 4, 5);
    d.v = wasm_i32x4_shuffle(t2, t3, 2, 3, 6, 7);
}
/// See the `f32x4s` overload. Same zero-extend, convert, multiply chain as the SSE2 backend.
inline f32x4 loadU8(const f32x4*, const uint8_t* p) {
    v128_t bytes = wasm_v128_load32_zero(p);
    v128_t words = wasm_u16x8_extend_low_u8x16(bytes);
    v128_t dwords = wasm_u32x4_extend_low_u16x8(words);
    return f32x4{wasm_f32x4_mul(wasm_f32x4_convert_i32x4(dwords), wasm_f32x4_splat(1.0f / 255.0f))};
}
/// See the `f32x4s` overload. The clamp keeps the SSE2 operand order (`_mm_max_ps(a, 0)` =
/// `pmax(0, a)`, so NaN becomes 0; `_mm_min_ps(c, 1)` = `pmin(1, c)`); the scaled value is then in
/// [0.5, 255.5], where the saturating truncation equals x86's `cvttps`.
inline void storeU8(uint8_t* p, f32x4 a) {
    v128_t c = wasm_f32x4_pmin(wasm_f32x4_splat(1.0f), wasm_f32x4_pmax(wasm_f32x4_splat(0.0f), a.v));
    v128_t scaled = wasm_f32x4_add(wasm_f32x4_mul(c, wasm_f32x4_splat(255.0f)), wasm_f32x4_splat(0.5f));
    v128_t ints = wasm_i32x4_trunc_sat_f32x4(scaled);
    v128_t words = wasm_i16x8_narrow_i32x4(ints, ints);
    v128_t bytes = wasm_u8x16_narrow_i16x8(words, words);
    wasm_v128_store32_lane(p, bytes, 0);
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
/// The same lanes as an `f32x4s`, and back: the T-643 ops below reuse the reference ones.
inline f32x4s asRef(f32x4 a) { return f32x4s{{a.v[0], a.v[1], a.v[2], a.v[3]}}; }
/// See `asRef`.
inline f32x4 fromRef(f32x4s a) { return f32x4{{a.v[0], a.v[1], a.v[2], a.v[3]}}; }
/// See the `f32x4s` overload.
inline f32x4 cmpLt(f32x4 a, f32x4 b) { return fromRef(cmpLt(asRef(a), asRef(b))); }
/// See the `f32x4s` overload.
inline f32x4 cmpLe(f32x4 a, f32x4 b) { return fromRef(cmpLe(asRef(a), asRef(b))); }
/// See the `f32x4s` overload.
inline f32x4 cmpEq(f32x4 a, f32x4 b) { return fromRef(cmpEq(asRef(a), asRef(b))); }
/// See the `f32x4s` overload.
inline f32x4 maskAnd(f32x4 a, f32x4 b) { return fromRef(maskAnd(asRef(a), asRef(b))); }
/// See the `f32x4s` overload.
inline f32x4 maskOr(f32x4 a, f32x4 b) { return fromRef(maskOr(asRef(a), asRef(b))); }
/// See the `f32x4s` overload.
inline f32x4 maskAndNot(f32x4 a, f32x4 b) { return fromRef(maskAndNot(asRef(a), asRef(b))); }
/// See the `f32x4s` overload.
inline f32x4 sqrt(f32x4 a) { return fromRef(sqrt(asRef(a))); }
/// See the `f32x4s` overload.
inline f32x4 abs(f32x4 a) { return fromRef(abs(asRef(a))); }
/// See the `f32x4s` overload.
inline void transpose4(f32x4& a, f32x4& b, f32x4& c, f32x4& d) {
    f32x4s r[4] = {asRef(a), asRef(b), asRef(c), asRef(d)};
    transpose4(r[0], r[1], r[2], r[3]);
    a = fromRef(r[0]);
    b = fromRef(r[1]);
    c = fromRef(r[2]);
    d = fromRef(r[3]);
}

#endif  // XT_SIMD_SSE2

}  // namespace xt::simd
