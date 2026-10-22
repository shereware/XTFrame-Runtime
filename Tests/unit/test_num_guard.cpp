#include <cstdint>
#include <limits>

#include "doctest.h"
#include "xt/num_guard.h"

/// xt/num_guard.h (Docs/tasks/T-258a.md "共用輔助"): finiteOr, clampFinite and the saturating
/// float-to-integer conversions -- NaN, both infinities, out of range and the exact boundaries.

using namespace xt;

namespace {

constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr float kNanF = std::numeric_limits<float>::quiet_NaN();
constexpr float kInfF = std::numeric_limits<float>::infinity();

}  // namespace

TEST_CASE("num_guard: finiteOr keeps finite values and replaces NaN / Inf") {
    CHECK(finiteOr(1.5f, 7.0f) == 1.5f);
    CHECK(finiteOr(-0.0f, 7.0f) == 0.0f);
    CHECK(finiteOr(std::numeric_limits<float>::max(), 7.0f) == std::numeric_limits<float>::max());
    CHECK(finiteOr(kNanF, 7.0f) == 7.0f);
    CHECK(finiteOr(kInfF, 7.0f) == 7.0f);
    CHECK(finiteOr(-kInfF, 7.0f) == 7.0f);
    CHECK(finiteOr(2.5, 9.0) == 2.5);
    CHECK(finiteOr(kNan, 9.0) == 9.0);
    CHECK(finiteOr(kInf, 9.0) == 9.0);
    CHECK(finiteOr(-kInf, 9.0) == 9.0);
}

TEST_CASE("num_guard: clampFinite clamps finite values, non-finite gives the fallback") {
    CHECK(clampFinite(5.0f, 0.0f, 10.0f, -1.0f) == 5.0f);
    CHECK(clampFinite(0.0f, 0.0f, 10.0f, -1.0f) == 0.0f);
    CHECK(clampFinite(10.0f, 0.0f, 10.0f, -1.0f) == 10.0f);
    CHECK(clampFinite(-3.0f, 0.0f, 10.0f, -1.0f) == 0.0f);
    CHECK(clampFinite(1e30f, 0.0f, 10.0f, -1.0f) == 10.0f);
    CHECK(clampFinite(kNanF, 0.0f, 10.0f, -1.0f) == -1.0f);
    CHECK(clampFinite(kInfF, 0.0f, 10.0f, -1.0f) == -1.0f);
    CHECK(clampFinite(-kInfF, 0.0f, 10.0f, -1.0f) == -1.0f);
}

TEST_CASE("num_guard: toInt32Sat") {
    CHECK(toInt32Sat(kNan) == 0);
    CHECK(toInt32Sat(kInf) == INT32_MAX);
    CHECK(toInt32Sat(-kInf) == INT32_MIN);
    CHECK(toInt32Sat(1e20) == INT32_MAX);
    CHECK(toInt32Sat(-1e20) == INT32_MIN);
    CHECK(toInt32Sat(2147483647.0) == INT32_MAX);
    CHECK(toInt32Sat(2147483647.9) == INT32_MAX);
    CHECK(toInt32Sat(2147483648.0) == INT32_MAX);
    CHECK(toInt32Sat(-2147483648.0) == INT32_MIN);
    CHECK(toInt32Sat(-2147483649.0) == INT32_MIN);
    CHECK(toInt32Sat(2147483646.0) == 2147483646);
    CHECK(toInt32Sat(-2147483647.0) == -2147483647);
    CHECK(toInt32Sat(3.9) == 3);  // Truncates toward zero, like static_cast.
    CHECK(toInt32Sat(-3.9) == -3);
    CHECK(toInt32Sat(0.0) == 0);
    static_assert(toInt32Sat(12.5) == 12);
}

TEST_CASE("num_guard: toUint32Sat") {
    CHECK(toUint32Sat(kNan) == 0u);
    CHECK(toUint32Sat(kInf) == UINT32_MAX);
    CHECK(toUint32Sat(-kInf) == 0u);
    CHECK(toUint32Sat(1e20) == UINT32_MAX);
    CHECK(toUint32Sat(-1.0) == 0u);
    CHECK(toUint32Sat(-5.0) == 0u);
    CHECK(toUint32Sat(-0.5) == 0u);
    CHECK(toUint32Sat(4294967295.0) == UINT32_MAX);
    CHECK(toUint32Sat(4294967296.0) == UINT32_MAX);
    CHECK(toUint32Sat(4294967294.0) == 4294967294u);
    CHECK(toUint32Sat(7.9) == 7u);
    static_assert(toUint32Sat(3.0) == 3u);
}

TEST_CASE("num_guard: toUint16Sat") {
    CHECK(toUint16Sat(kNan) == 0);
    CHECK(toUint16Sat(kInf) == UINT16_MAX);
    CHECK(toUint16Sat(-kInf) == 0);
    CHECK(toUint16Sat(1e6) == UINT16_MAX);
    CHECK(toUint16Sat(70000.0) == UINT16_MAX);
    CHECK(toUint16Sat(65535.0) == UINT16_MAX);
    CHECK(toUint16Sat(65534.0) == 65534);
    CHECK(toUint16Sat(-1.0) == 0);
    CHECK(toUint16Sat(2.7) == 2);
    static_assert(toUint16Sat(9.0) == 9);
}

TEST_CASE("num_guard: toInt64Sat") {
    CHECK(toInt64Sat(kNan) == 0);
    CHECK(toInt64Sat(kInf) == INT64_MAX);
    CHECK(toInt64Sat(-kInf) == INT64_MIN);
    CHECK(toInt64Sat(1e300) == INT64_MAX);
    CHECK(toInt64Sat(-1e300) == INT64_MIN);
    CHECK(toInt64Sat(9223372036854775808.0) == INT64_MAX);
    CHECK(toInt64Sat(-9223372036854775808.0) == INT64_MIN);
    CHECK(toInt64Sat(9007199254740992.0) == 9007199254740992LL);
    CHECK(toInt64Sat(-2.5) == -2);
    static_assert(toInt64Sat(-7.0) == -7);
}
