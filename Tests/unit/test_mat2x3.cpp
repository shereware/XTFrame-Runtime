#include "doctest.h"

#include "xt/types.h"

using namespace xt;

namespace {
void checkVec2Approx(Vec2 actual, Vec2 expected, float eps = 1e-5f) {
    CHECK(actual.x == doctest::Approx(expected.x).epsilon(eps));
    CHECK(actual.y == doctest::Approx(expected.y).epsilon(eps));
}
}  // namespace

TEST_CASE("identity apply is unchanged") {
    Mat2x3 identity;
    checkVec2Approx(apply(identity, Vec2{3, 4}), Vec2{3, 4});
}

TEST_CASE("translation") { checkVec2Approx(apply(translation(Vec2{3, 4}), Vec2{1, 1}), Vec2{4, 5}); }

TEST_CASE("rotationDeg 90 is clockwise in y-down space") {
    checkVec2Approx(apply(rotationDeg(90.0f), Vec2{1, 0}), Vec2{0, 1});
}

TEST_CASE("scaling") { checkVec2Approx(apply(scaling(Vec2{2, 3}), Vec2{1, 1}), Vec2{2, 3}); }

TEST_CASE("mul applies rhs first") {
    Mat2x3 t = translation(Vec2{10, 0});
    Mat2x3 r = rotationDeg(90.0f);
    Vec2 p{1, 0};
    checkVec2Approx(apply(mul(t, r), p), apply(t, apply(r, p)));
    checkVec2Approx(apply(mul(t, r), p), Vec2{10, 1});
}

TEST_CASE("invert of invertible matrix") {
    Mat2x3 m = mul(translation(Vec2{5, -2}), mul(rotationDeg(37.0f), scaling(Vec2{1.5f, 0.7f})));
    Mat2x3 inv;
    REQUIRE(invert(m, inv));
    Mat2x3 identity;
    CHECK(approxEqual(mul(m, inv), identity));
}

TEST_CASE("invert of singular matrix returns false") {
    Mat2x3 m = scaling(Vec2{0, 1});
    Mat2x3 out;
    CHECK_FALSE(invert(m, out));
}

TEST_CASE("skewDeg(45, 0)") { checkVec2Approx(apply(skewDeg(45.0f, 0.0f), Vec2{0, 1}), Vec2{1, 1}); }

TEST_CASE("skewDeg(45, 90)") { checkVec2Approx(apply(skewDeg(45.0f, 90.0f), Vec2{1, 0}), Vec2{1, -1}); }

TEST_CASE("composeTRSS") {
    Mat2x3 m = composeTRSS(Vec2{5, 5}, 0.0f, Vec2{0, 0}, Vec2{2, 2}, Vec2{1, 1});
    checkVec2Approx(apply(m, Vec2{1, 1}), Vec2{5, 5});
    checkVec2Approx(apply(m, Vec2{2, 1}), Vec2{7, 5});
}

TEST_CASE("transformBounds") {
    Rect bounds = transformBounds(rotationDeg(90.0f), Rect{0, 0, 2, 1});
    CHECK(bounds.x == doctest::Approx(-1.0f).epsilon(1e-5f));
    CHECK(bounds.y == doctest::Approx(0.0f).epsilon(1e-5f));
    CHECK(bounds.w == doctest::Approx(1.0f).epsilon(1e-5f));
    CHECK(bounds.h == doctest::Approx(2.0f).epsilon(1e-5f));
}
