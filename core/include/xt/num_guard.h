#pragma once

#include <cmath>
#include <cstdint>

/// Guards for numbers that come from files, bindings or host calls (docs/tasks/T-258a.md): a
/// NaN, an infinity or an out-of-range value never reaches a loop bound, an array size or a
/// float-to-integer conversion unchecked. Header-only and dependency-free, so sw/, flow/ and
/// bundle/ use it too.

namespace xt {

/// v if finite, otherwise fallback.
inline float finiteOr(float v, float fallback) { return std::isfinite(v) ? v : fallback; }
/// v if finite, otherwise fallback.
inline double finiteOr(double v, double fallback) { return std::isfinite(v) ? v : fallback; }

/// Clamps to [lo, hi]; a non-finite v yields fallback.
inline float clampFinite(float v, float lo, float hi, float fallback) {
    if (!std::isfinite(v)) {
        return fallback;
    }
    return v < lo ? lo : (v > hi ? hi : v);
}

/// Float to integer without undefined behaviour: NaN -> 0, out of range saturates; in range it
/// truncates toward zero like static_cast.
constexpr int32_t toInt32Sat(double v) {
    if (v != v) {
        return 0;
    }
    if (v <= -2147483648.0) {
        return INT32_MIN;
    }
    if (v >= 2147483647.0) {
        return INT32_MAX;
    }
    return static_cast<int32_t>(v);
}

/// Float to integer without undefined behaviour: NaN -> 0, out of range saturates.
constexpr uint32_t toUint32Sat(double v) {
    if (!(v > -1.0)) {
        return 0;  // NaN, or truncates below 0.
    }
    if (v >= 4294967295.0) {
        return UINT32_MAX;
    }
    return static_cast<uint32_t>(v);
}

/// Float to integer without undefined behaviour: NaN -> 0, out of range saturates.
constexpr uint16_t toUint16Sat(double v) {
    if (!(v > -1.0)) {
        return 0;  // NaN, or truncates below 0.
    }
    if (v >= 65535.0) {
        return UINT16_MAX;
    }
    return static_cast<uint16_t>(v);
}

/// Float to integer without undefined behaviour: NaN -> 0, out of range saturates.
constexpr int64_t toInt64Sat(double v) {
    if (v != v) {
        return 0;
    }
    if (v <= -9223372036854775808.0) {
        return INT64_MIN;
    }
    if (v >= 9223372036854775808.0) {
        return INT64_MAX;
    }
    return static_cast<int64_t>(v);
}

}  // namespace xt
