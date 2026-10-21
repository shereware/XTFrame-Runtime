#include "noise.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "xt/num_guard.h"

/// See noise.h.

namespace xt {

namespace {

/// Hash of one lattice point, mapped to [-1, 1].
float latticeValue(int32_t ix, int32_t iy, uint32_t seed) {
    uint32_t h =
        static_cast<uint32_t>(ix) * 374761393u + static_cast<uint32_t>(iy) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu) * 2.0f - 1.0f;
}

}  // namespace

float valueNoise2(float x, float y, uint32_t seed) {
    const float fx = std::floor(x);
    const float fy = std::floor(y);
    // T-258a: beyond the int32 range the lattice coordinate saturates (one below the maximum, so
    // `+ 1` below cannot overflow); NaN gives 0.
    const int32_t ix = std::min(toInt32Sat(fx), INT32_MAX - 1);
    const int32_t iy = std::min(toInt32Sat(fy), INT32_MAX - 1);
    const float tx = x - fx;
    const float ty = y - fy;
    const float v00 = latticeValue(ix, iy, seed);
    const float v10 = latticeValue(ix + 1, iy, seed);
    const float v01 = latticeValue(ix, iy + 1, seed);
    const float v11 = latticeValue(ix + 1, iy + 1, seed);
    const float a = v00 + (v10 - v00) * tx;
    const float b = v01 + (v11 - v01) * tx;
    return a + (b - a) * ty;
}

}  // namespace xt
