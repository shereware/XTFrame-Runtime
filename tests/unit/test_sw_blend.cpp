#include <algorithm>
#include <cmath>

#include "doctest.h"
#include "sw_blend.h"
#include "xt/node.h"
#include "xt/types.h"

/// blendPixel tests. See docs/tasks/T-12.md "測試" / "test_sw_blend.cpp".
///
/// Hue/Saturation/Color/Luminosity reference values are computed here with a second, independent
/// implementation of the W3C Compositing and Blending Level 1 §9.1.4 formulas (Lum/ClipColor/
/// SetLum/Sat/SetSat), deliberately not sharing code with sw_blend.cpp, so the test actually
/// verifies the production formula rather than just re-checking itself.

using namespace xt;
using namespace xt::sw;

namespace {

// ---------------------------------------------------------------------------------------------
// Independent reference implementation of W3C Compositing Level 1 §9.1.4.
// ---------------------------------------------------------------------------------------------

struct Rgb {
    float r, g, b;
};

float refLum(const Rgb& c) { return 0.3f * c.r + 0.59f * c.g + 0.11f * c.b; }

Rgb refClipColor(Rgb c) {
    float l = refLum(c);
    float n = std::min(c.r, std::min(c.g, c.b));
    float x = std::max(c.r, std::max(c.g, c.b));
    if (n < 0.0f) {
        c.r = l + (c.r - l) * l / (l - n);
        c.g = l + (c.g - l) * l / (l - n);
        c.b = l + (c.b - l) * l / (l - n);
    }
    if (x > 1.0f) {
        c.r = l + (c.r - l) * (1.0f - l) / (x - l);
        c.g = l + (c.g - l) * (1.0f - l) / (x - l);
        c.b = l + (c.b - l) * (1.0f - l) / (x - l);
    }
    return c;
}

Rgb refSetLum(Rgb c, float l) {
    float d = l - refLum(c);
    c.r += d;
    c.g += d;
    c.b += d;
    return refClipColor(c);
}

float refSat(const Rgb& c) { return std::max(c.r, std::max(c.g, c.b)) - std::min(c.r, std::min(c.g, c.b)); }

Rgb refSetSat(Rgb c, float s) {
    // Straightforward re-derivation from the spec pseudocode: find which channel is max/mid/min by
    // explicit comparisons instead of sw_blend.cpp's sort-based approach.
    float* lo;
    float* mid;
    float* hi;
    if (c.r <= c.g && c.r <= c.b) {
        lo = &c.r;
        if (c.g <= c.b) {
            mid = &c.g;
            hi = &c.b;
        } else {
            mid = &c.b;
            hi = &c.g;
        }
    } else if (c.g <= c.r && c.g <= c.b) {
        lo = &c.g;
        if (c.r <= c.b) {
            mid = &c.r;
            hi = &c.b;
        } else {
            mid = &c.b;
            hi = &c.r;
        }
    } else {
        lo = &c.b;
        if (c.r <= c.g) {
            mid = &c.r;
            hi = &c.g;
        } else {
            mid = &c.g;
            hi = &c.r;
        }
    }
    if (*hi > *lo) {
        *mid = (*mid - *lo) * s / (*hi - *lo);
        *hi = s;
    } else {
        *mid = 0.0f;
        *hi = 0.0f;
    }
    *lo = 0.0f;
    return c;
}

Rgb refHue(const Rgb& cb, const Rgb& cs) { return refSetLum(refSetSat(cs, refSat(cb)), refLum(cb)); }
Rgb refSaturation(const Rgb& cb, const Rgb& cs) { return refSetLum(refSetSat(cb, refSat(cs)), refLum(cb)); }
Rgb refColor(const Rgb& cb, const Rgb& cs) { return refSetLum(cs, refLum(cb)); }
Rgb refLuminosity(const Rgb& cb, const Rgb& cs) { return refSetLum(cb, refLum(cs)); }

/// Independent re-derivation of the general compositing formula in docs/tasks/T-12.md
/// "混合（sw_blend）", parameterized by a caller-supplied B(Cb,Cs) function for the non-separable
/// modes under test.
Color refComposite(const Rgb& b, Color src, Color dst) {
    float as = src.a;
    float ab = dst.a;
    float coR = (1.0f - ab) * src.r + ab * b.r;
    float coG = (1.0f - ab) * src.g + ab * b.g;
    float coB = (1.0f - ab) * src.b + ab * b.b;
    float ao = as + ab * (1.0f - as);
    if (ao <= 0.0f) {
        return Color{0, 0, 0, 0};
    }
    return Color{
        (as * coR + ab * dst.r * (1.0f - as)) / ao,
        (as * coG + ab * dst.g * (1.0f - as)) / ao,
        (as * coB + ab * dst.b * (1.0f - as)) / ao,
        ao,
    };
}

}  // namespace

TEST_CASE("blendPixel: Multiply(0.5, 0.5) = 0.25, opaque") {
    Color src{0.5f, 0.5f, 0.5f, 1};
    Color dst{0.5f, 0.5f, 0.5f, 1};
    Color out = blendPixel(BlendMode::Multiply, src, dst);
    CHECK(out.r == doctest::Approx(0.25f));
    CHECK(out.a == doctest::Approx(1.0f));
}

TEST_CASE("blendPixel: Screen(0.5, 0.5) = 0.75, opaque") {
    Color src{0.5f, 0.5f, 0.5f, 1};
    Color dst{0.5f, 0.5f, 0.5f, 1};
    Color out = blendPixel(BlendMode::Screen, src, dst);
    CHECK(out.r == doctest::Approx(0.75f));
}

TEST_CASE("blendPixel: Difference(0.8, 0.3) = 0.5, opaque") {
    Color src{0.3f, 0.3f, 0.3f, 1};
    Color dst{0.8f, 0.8f, 0.8f, 1};
    Color out = blendPixel(BlendMode::Difference, src, dst);
    CHECK(out.r == doctest::Approx(0.5f));
}

TEST_CASE("blendPixel: Add(0.6, 0.6) = 1, opaque, clamped") {
    Color src{0.6f, 0.6f, 0.6f, 1};
    Color dst{0.6f, 0.6f, 0.6f, 1};
    Color out = blendPixel(BlendMode::Add, src, dst);
    CHECK(out.r == doctest::Approx(1.0f));
}

TEST_CASE("blendPixel: alpha compositing, alphaS=0.5 alphaB=0.5 -> alphaO=0.75") {
    Color src{1, 0, 0, 0.5f};
    Color dst{0, 1, 0, 0.5f};
    Color out = blendPixel(BlendMode::Normal, src, dst);
    CHECK(out.a == doctest::Approx(0.75f));
}

TEST_CASE("blendPixel: non-separable modes match an independent W3C reference implementation") {
    // Example from the task sheet: src = (0.5, 0.25, 0.75), dst = (0.25, 0.5, 0.25), opaque.
    Color src{0.5f, 0.25f, 0.75f, 1};
    Color dst{0.25f, 0.5f, 0.25f, 1};
    Rgb cs{src.r, src.g, src.b};
    Rgb cb{dst.r, dst.g, dst.b};

    struct Case {
        BlendMode mode;
        Rgb expected;
    };
    Case cases[] = {
        {BlendMode::Hue, refHue(cb, cs)},
        {BlendMode::Saturation, refSaturation(cb, cs)},
        {BlendMode::Color, refColor(cb, cs)},
        {BlendMode::Luminosity, refLuminosity(cb, cs)},
    };
    for (const Case& c : cases) {
        Color expected = refComposite(c.expected, src, dst);
        Color actual = blendPixel(c.mode, src, dst);
        CHECK(actual.r == doctest::Approx(expected.r).epsilon(1e-4));
        CHECK(actual.g == doctest::Approx(expected.g).epsilon(1e-4));
        CHECK(actual.b == doctest::Approx(expected.b).epsilon(1e-4));
        CHECK(actual.a == doctest::Approx(expected.a).epsilon(1e-4));
    }
}

TEST_CASE("blendPixel: transparent source leaves destination unchanged") {
    Color src{1, 1, 1, 0};
    Color dst{0.2f, 0.4f, 0.6f, 0.8f};
    Color out = blendPixel(BlendMode::Multiply, src, dst);
    CHECK(out.r == doctest::Approx(dst.r));
    CHECK(out.g == doctest::Approx(dst.g));
    CHECK(out.b == doctest::Approx(dst.b));
    CHECK(out.a == doctest::Approx(dst.a));
}
