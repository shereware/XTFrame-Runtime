#pragma once

#include <cstdint>

/// Value noise (Docs/design/31-vector-authoring.md §31.6.1 wiggle, T-225).

namespace xt {

/// 2D value noise in [-1, 1]: a hash of each integer lattice point (ix, iy) and seed,
/// bilinearly interpolated between the four lattice points around (x, y). Continuous in x and y.
float valueNoise2(float x, float y, uint32_t seed);

}  // namespace xt
