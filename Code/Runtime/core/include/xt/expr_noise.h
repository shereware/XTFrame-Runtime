#pragma once

#include <cmath>
#include <cstdint>

/// Deterministic functions shared by the timeline expression compiler (Code/Runtime/compile/src/expr,
/// T-766) and the run-time expression evaluator (T-776): the noise behind `wiggle` / `noise`, the
/// random numbers behind `random`, their seed rules and the AE curves of `ease` / `easeIn` /
/// `easeOut`. See Docs/design/58-value-functions-and-drivers.md §58.3 and §58.5 ("the same algorithm
/// and seed rules at compile time and at run time").
///
/// Header-only, single precision, and dependent on nothing but the standard library, so both sides
/// compile the very same code: for the same inputs the results are identical bit for bit (the tree
/// builds without floating-point contraction, 14 §14.10). Tests/unit/test_expr_noise.cpp holds a
/// table of outputs for fixed inputs that both sides check.

namespace xt {

/// AE's `ease(t, ...)` curve as a CSS cubic bezier (x1, y1, x2, y2).
inline constexpr float kExprEase[4] = {0.33f, 0.0f, 0.667f, 1.0f};
/// AE's `easeIn(t, ...)` curve as a CSS cubic bezier.
inline constexpr float kExprEaseIn[4] = {0.333f, 0.0f, 0.833f, 0.833f};
/// AE's `easeOut(t, ...)` curve as a CSS cubic bezier.
inline constexpr float kExprEaseOut[4] = {0.167f, 0.167f, 0.667f, 1.0f};

/// The rate (per second) at which `random()` draws a new number when it is not timeless: the frame
/// index of a time is round(time × this).
inline constexpr float kExprRandomFrameRate = 60.0f;

/// The salt of `random()`'s default seed: exprMixSeed(seed, kExprRandomSalt) until `seedRandom`
/// (the compiler's evaluator uses the same value; T-776's Random op always starts from it).
inline constexpr uint32_t kExprRandomSalt = 0x52414E44u;

/// The most lattice cells one period of a periodic wiggle has (a guard for absurd frequencies).
inline constexpr int32_t kExprMaxWigglePeriod = 1 << 20;
/// The most octaves `wiggle` sums.
inline constexpr int32_t kExprMaxWiggleOctaves = 8;

/// A 32-bit integer hash (an avalanche mix; every input bit affects every output bit).
constexpr uint32_t exprHash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

/// Combines a seed with one more number (a call site, an octave, a frame index...). Order matters:
/// exprMixSeed(exprMixSeed(s, a), b) differs from exprMixSeed(exprMixSeed(s, b), a).
constexpr uint32_t exprMixSeed(uint32_t seed, uint32_t v) {
    return exprHash(seed ^ exprHash(v + 0x9e3779b9U));
}

/// floor(x) as an int32 without undefined behaviour: NaN gives 0, beyond the int32 range it saturates
/// (one below the maximum, so `+ 1` cannot overflow).
inline int32_t exprFloorToInt(float x) {
    const float f = std::floor(x);
    if (f != f) {
        return 0;
    }
    if (f <= -2147483648.0f) {
        return INT32_MIN;
    }
    if (f >= 2147483520.0f) {  // the largest float below 2^31
        return INT32_MAX - 1;
    }
    return static_cast<int32_t>(f);
}

/// `seedRandom(n)`'s number as a seed: floor(n) as an integer (saturating; NaN gives 0).
inline uint32_t exprSeedFromNumber(float n) { return static_cast<uint32_t>(exprFloorToInt(n)); }

/// The value of lattice point `i` in [-1, 1] (the hash of Code/Runtime/core/src/noise.cpp's value
/// noise, one-dimensional).
inline float exprLattice(int32_t i, uint32_t seed) {
    uint32_t h = static_cast<uint32_t>(i) * 374761393u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu) * 2.0f - 1.0f;
}

/// The quintic fade 6t^5 - 15t^4 + 10t^3: 0 and 1 at the ends with zero first and second derivatives,
/// so the noise is smooth across lattice points.
inline float exprFade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

/// One-dimensional smooth value noise in [-1, 1]: the lattice values around `x` blended with
/// exprFade. Continuous with continuous first and second derivatives. `noise(x)` of 58 §58.2.
inline float exprNoise(float x, uint32_t seed) {
    const int32_t i = exprFloorToInt(x);
    const float f = x - std::floor(x);
    const float a = exprLattice(i, seed);
    const float b = exprLattice(i + 1, seed);
    return a + (b - a) * exprFade(f != f ? 0.0f : f);
}

/// exprNoise with the lattice repeating every `period` cells (period >= 1): the value at u and at
/// u + period are the same.
inline float exprPeriodicNoise(float u, int32_t period, uint32_t seed) {
    if (period < 1) {
        period = 1;
    }
    const int32_t i = exprFloorToInt(u);
    const float f = u - std::floor(u);
    int32_t i0 = i % period;
    if (i0 < 0) {
        i0 += period;
    }
    const int32_t i1 = i0 + 1 == period ? 0 : i0 + 1;
    const float a = exprLattice(i0, seed);
    const float b = exprLattice(i1, seed);
    return a + (b - a) * exprFade(f != f ? 0.0f : f);
}

/// The number of lattice cells over one timeline length for a wiggle of `freq` per second:
/// max(1, round(|freq| × duration)) (58 §58.3: the value at time 0 and at `duration` are the same).
/// 0 when the wiggle is not periodic: `duration` <= 0 or a non-finite input (the run-time path passes
/// duration 0 when the frequency comes from a run-time input).
inline int32_t exprWigglePeriod(float freq, float duration) {
    if (!(duration > 0.0f) || !std::isfinite(duration) || !std::isfinite(freq)) {
        return 0;
    }
    const float cells = std::floor(std::fabs(freq) * duration + 0.5f);
    if (!(cells >= 1.0f)) {
        return 1;
    }
    if (cells >= static_cast<float>(kExprMaxWigglePeriod)) {
        return kExprMaxWigglePeriod;
    }
    return static_cast<int32_t>(cells);
}

/// The noise of `wiggle(freq, amp, octaves, ampMult)` at `time`, before it is scaled by `amp` and
/// added to `value`: a sum of `octaves` (1..kExprMaxWiggleOctaves) layers, layer k at twice the
/// frequency of layer k - 1 and `ampMult` times its amplitude, each with its own seed. With a
/// periodic lattice (exprWigglePeriod > 0) every layer repeats over [0, duration]; otherwise the
/// lattice coordinate is |freq| × time. The first layer stays in [-1, 1].
inline float exprWiggleNoise(float time, float freq, float duration, uint32_t seed, int32_t octaves = 1,
                             float ampMult = 0.5f) {
    if (octaves < 1) {
        octaves = 1;
    }
    if (octaves > kExprMaxWiggleOctaves) {
        octaves = kExprMaxWiggleOctaves;
    }
    const int32_t period = exprWigglePeriod(freq, duration);
    float u = period > 0 ? time / duration * static_cast<float>(period) : std::fabs(freq) * time;
    float sum = 0.0f;
    float amp = 1.0f;
    int32_t layerPeriod = period;
    for (int32_t k = 0; k < octaves; ++k) {
        const uint32_t layerSeed = exprMixSeed(seed, static_cast<uint32_t>(k));
        const float n = period > 0 ? exprPeriodicNoise(u, layerPeriod, layerSeed) : exprNoise(u, layerSeed);
        sum += amp * n;
        amp *= ampMult;
        u *= 2.0f;
        if (layerPeriod > 0 && layerPeriod <= kExprMaxWigglePeriod / 2) {
            layerPeriod *= 2;
        }
    }
    return sum;
}

/// The frame index `random()` uses at `time`: round(time × kExprRandomFrameRate), saturating.
inline uint32_t exprRandomFrame(float time) {
    return static_cast<uint32_t>(exprFloorToInt(time * kExprRandomFrameRate + 0.5f));
}

/// The seed of one `random()` draw: the expression's random seed, then (unless `timeless`, i.e.
/// after `seedRandom(n, true)`) the frame index, then the draw's call number.
constexpr uint32_t exprRandomSeed(uint32_t seed, uint32_t frame, uint32_t call, bool timeless) {
    return timeless ? exprMixSeed(exprMixSeed(seed, 0xA511E9B3U), call)
                    : exprMixSeed(exprMixSeed(seed, frame), call);
}

/// A uniform number in [0, 1) from a seed (24 bits of its hash).
inline float exprRandom01(uint32_t seed) {
    return static_cast<float>(exprHash(seed) >> 8) * (1.0f / 16777216.0f);
}

}  // namespace xt
