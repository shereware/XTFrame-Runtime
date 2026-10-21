#include <vector>

#include "doctest.h"
#include "xt/geom.h"

/// T-218: the public bezier helpers of xt/geom.h (`xt::geom`): split, exact bounds and the
/// fill-rule point test. trimPath's own behaviour stays covered by test_path_trim.cpp.

using namespace xt;

TEST_CASE("geom::splitCubic halves meet at the curve point") {
    Vec2 left[4];
    Vec2 right[4];
    geom::splitCubic(Vec2{0, 0}, Vec2{0, 10}, Vec2{10, 10}, Vec2{10, 0}, 0.5f, left, right);
    CHECK(left[3].x == doctest::Approx(5.0f));
    CHECK(left[3].y == doctest::Approx(7.5f));
    CHECK(right[0].x == doctest::Approx(left[3].x));
    CHECK(right[3].x == doctest::Approx(10.0f));
    geom::CubicTable table = geom::buildCubicTable(Vec2{0, 0}, Vec2{1, 0}, Vec2{2, 0}, Vec2{3, 0});
    CHECK(table.length == doctest::Approx(3.0f));
    CHECK(geom::lengthFractionToT(table, 0.5f) == doctest::Approx(0.5f));
}

TEST_CASE("geom::pathBounds uses the cubic's extrema, not its control points") {
    // M0 0 C0 10 10 10 10 0: the top of the arch is y = 7.5 at t = 0.5, control points reach 10.
    const std::vector<PathVerb> verbs{PathVerb::Move, PathVerb::Cubic};
    const std::vector<float> points{0, 0, 0, 10, 10, 10, 10, 0};
    Rect r = geom::pathBounds(verbs, points);
    CHECK(r.x == doctest::Approx(0.0f));
    CHECK(r.y == doctest::Approx(0.0f));
    CHECK(r.w == doctest::Approx(10.0f));
    CHECK(r.h == doctest::Approx(7.5f));

    // An S-curve whose x extrema lie outside the end points: M0 0 C-10 0 20 10 10 10.
    const std::vector<float> s{0, 0, -10, 0, 20, 10, 10, 10};
    Rect rs = geom::pathBounds(verbs, s);
    CHECK(rs.x < 0.0f);
    CHECK(rs.x == doctest::Approx(-2.07f).epsilon(0.02));
    CHECK(rs.x + rs.w > 10.0f);
    CHECK(rs.x + rs.w == doctest::Approx(12.07f).epsilon(0.02));
    CHECK(rs.h == doctest::Approx(10.0f));

    CHECK(geom::pathBounds({}, {}) == Rect{});
}

TEST_CASE("geom::pathContains honours non-zero and even-odd") {
    // Two nested squares wound the same way: the inner one is a hole only under even-odd.
    const std::vector<PathVerb> verbs{PathVerb::Move,  PathVerb::Line, PathVerb::Line, PathVerb::Line,
                                      PathVerb::Close, PathVerb::Move, PathVerb::Line, PathVerb::Line,
                                      PathVerb::Line,  PathVerb::Close};
    const std::vector<float> points{0, 0, 10, 0, 10, 10, 0, 10, 3, 3, 7, 3, 7, 7, 3, 7};
    CHECK(geom::pathContains(verbs, points, Vec2{5, 5}, FillRule::NonZero));
    CHECK_FALSE(geom::pathContains(verbs, points, Vec2{5, 5}, FillRule::EvenOdd));
    CHECK(geom::pathContains(verbs, points, Vec2{1, 1}, FillRule::EvenOdd));
    CHECK_FALSE(geom::pathContains(verbs, points, Vec2{11, 5}, FillRule::NonZero));

    // A curved edge: the arch M0 0 C0 10 10 10 10 0 Z contains (5, 7) but not (5, 8).
    const std::vector<PathVerb> arch{PathVerb::Move, PathVerb::Cubic, PathVerb::Close};
    const std::vector<float> archPts{0, 0, 0, 10, 10, 10, 10, 0};
    CHECK(geom::pathContains(arch, archPts, Vec2{5, 7}, FillRule::NonZero));
    CHECK_FALSE(geom::pathContains(arch, archPts, Vec2{5, 8}, FillRule::NonZero));
}
