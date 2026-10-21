#include <cmath>
#include <cstring>
#include <system_error>

#include "doctest.h"
#include "xt/charconv.h"

/// T-240: xt::fromChars, the floating-point std::from_chars every target (Android NDK included)
/// can call. These cases hold for both the std::from_chars forward and the NDK fallback.

namespace {

std::from_chars_result parse(const char* text, float& value) {
    return xt::fromChars(text, text + std::strlen(text), value);
}

}  // namespace

TEST_CASE("xt::fromChars: decimal, exponent and partial spans") {
    float v = -1.0f;
    const char* text = "12.5px";
    std::from_chars_result r = parse(text, v);
    CHECK(r.ec == std::errc{});
    CHECK(r.ptr == text + 4);
    CHECK(v == doctest::Approx(12.5f));

    CHECK(parse("-0.25", v).ec == std::errc{});
    CHECK(v == doctest::Approx(-0.25f));
    CHECK(parse(".5", v).ec == std::errc{});
    CHECK(v == doctest::Approx(0.5f));
    CHECK(parse("3e2", v).ec == std::errc{});
    CHECK(v == doctest::Approx(300.0f));
    text = "2emph";  // Not an exponent: stops after "2".
    r = parse(text, v);
    CHECK(r.ec == std::errc{});
    CHECK(r.ptr == text + 1);
    CHECK(v == doctest::Approx(2.0f));

    double d = 0.0;
    const char* big = "1.5e300";
    CHECK(xt::fromChars(big, big + std::strlen(big), d).ec == std::errc{});
    CHECK(d == doctest::Approx(1.5e300));
}

TEST_CASE("xt::fromChars: rejects what std::from_chars rejects and keeps the value") {
    float v = 7.0f;
    const char* text = "+1";
    std::from_chars_result r = parse(text, v);
    CHECK(r.ec == std::errc::invalid_argument);
    CHECK(r.ptr == text);
    CHECK(parse(" 1", v).ec == std::errc::invalid_argument);
    CHECK(parse("", v).ec == std::errc::invalid_argument);
    CHECK(parse(".", v).ec == std::errc::invalid_argument);
    CHECK(v == 7.0f);
    // The value after an out-of-range result differs between libraries (MSVC writes inf).
    CHECK(parse("1e999", v).ec == std::errc::result_out_of_range);
    CHECK(parse("inf", v).ec == std::errc{});
    CHECK(std::isinf(v));
}
