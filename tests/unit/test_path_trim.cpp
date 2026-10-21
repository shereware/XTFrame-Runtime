#include <cmath>
#include <vector>

#include "doctest.h"
#include "xt/geom.h"

using namespace xt;

TEST_CASE("trimPath: simple sub-range of a single line") {
    std::vector<PathVerb> verbs{PathVerb::Move, PathVerb::Line};
    std::vector<float> points{0, 0, 10, 0};
    std::vector<PathVerb> outVerbs;
    std::vector<float> outPoints;
    trimPath(verbs, points, 0.2f, 0.5f, 0.0f, outVerbs, outPoints);
    REQUIRE(outVerbs.size() == 2);
    CHECK(outVerbs[0] == PathVerb::Move);
    CHECK(outVerbs[1] == PathVerb::Line);
    REQUIRE(outPoints.size() == 4);
    CHECK(outPoints[0] == doctest::Approx(2.0f));
    CHECK(outPoints[1] == doctest::Approx(0.0f));
    CHECK(outPoints[2] == doctest::Approx(5.0f));
    CHECK(outPoints[3] == doctest::Approx(0.0f));
}

TEST_CASE("trimPath: offset wraps past 1, producing two fragments") {
    std::vector<PathVerb> verbs{PathVerb::Move, PathVerb::Line};
    std::vector<float> points{0, 0, 10, 0};
    std::vector<PathVerb> outVerbs;
    std::vector<float> outPoints;
    trimPath(verbs, points, 0.0f, 0.5f, 0.7f, outVerbs, outPoints);
    // Interval [0.7, 1.2] -> [7,10] then [0,2].
    REQUIRE(outVerbs.size() == 4);
    CHECK(outVerbs[0] == PathVerb::Move);
    CHECK(outVerbs[1] == PathVerb::Line);
    CHECK(outVerbs[2] == PathVerb::Move);
    CHECK(outVerbs[3] == PathVerb::Line);
    REQUIRE(outPoints.size() == 8);
    CHECK(outPoints[0] == doctest::Approx(7.0f));
    CHECK(outPoints[1] == doctest::Approx(0.0f));
    CHECK(outPoints[2] == doctest::Approx(10.0f));
    CHECK(outPoints[3] == doctest::Approx(0.0f));
    CHECK(outPoints[4] == doctest::Approx(0.0f));
    CHECK(outPoints[5] == doctest::Approx(0.0f));
    CHECK(outPoints[6] == doctest::Approx(2.0f));
    CHECK(outPoints[7] == doctest::Approx(0.0f));
}

TEST_CASE("trimPath: start == end yields empty output") {
    std::vector<PathVerb> verbs{PathVerb::Move, PathVerb::Line};
    std::vector<float> points{0, 0, 10, 0};
    std::vector<PathVerb> outVerbs;
    std::vector<float> outPoints;
    trimPath(verbs, points, 0.3f, 0.3f, 0.0f, outVerbs, outPoints);
    CHECK(outVerbs.empty());
    CHECK(outPoints.empty());
}

TEST_CASE("trimPath: full range [0,1] copies input unchanged") {
    std::vector<PathVerb> verbs{PathVerb::Move, PathVerb::Line};
    std::vector<float> points{0, 0, 10, 0};
    std::vector<PathVerb> outVerbs;
    std::vector<float> outPoints;
    trimPath(verbs, points, 0.0f, 1.0f, 0.0f, outVerbs, outPoints);
    CHECK(outVerbs == verbs);
    REQUIRE(outPoints.size() == points.size());
    for (size_t i = 0; i < points.size(); ++i) {
        CHECK(outPoints[i] == doctest::Approx(points[i]));
    }
}

TEST_CASE("trimPath: two 10-length line segments treated as one continuous path") {
    std::vector<PathVerb> verbs{PathVerb::Move, PathVerb::Line, PathVerb::Line};
    std::vector<float> points{0, 0, 10, 0, 10, 10};
    std::vector<PathVerb> outVerbs;
    std::vector<float> outPoints;
    trimPath(verbs, points, 0.25f, 0.75f, 0.0f, outVerbs, outPoints);
    // Total length 20; range [5,15] -> Move(5,0) Line(10,0) Line(10,5).
    REQUIRE(outVerbs.size() == 3);
    CHECK(outVerbs[0] == PathVerb::Move);
    CHECK(outVerbs[1] == PathVerb::Line);
    CHECK(outVerbs[2] == PathVerb::Line);
    REQUIRE(outPoints.size() == 6);
    CHECK(outPoints[0] == doctest::Approx(5.0f));
    CHECK(outPoints[1] == doctest::Approx(0.0f));
    CHECK(outPoints[2] == doctest::Approx(10.0f));
    CHECK(outPoints[3] == doctest::Approx(0.0f));
    CHECK(outPoints[4] == doctest::Approx(10.0f));
    CHECK(outPoints[5] == doctest::Approx(5.0f));
}

TEST_CASE("trimPath: cubic quarter circle trimmed to its first half ends near the 45-degree point") {
    constexpr float kKappa = 0.5522847498f;
    std::vector<PathVerb> verbs{PathVerb::Move, PathVerb::Cubic};
    std::vector<float> points{1, 0, 1, kKappa, kKappa, 1, 0, 1};
    std::vector<PathVerb> outVerbs;
    std::vector<float> outPoints;
    trimPath(verbs, points, 0.0f, 0.5f, 0.0f, outVerbs, outPoints);
    REQUIRE(outVerbs.size() == 2);
    REQUIRE(outPoints.size() == 8);
    float endX = outPoints[6];
    float endY = outPoints[7];
    float midX = std::cos(3.14159265358979323846f / 4.0f);
    float midY = std::sin(3.14159265358979323846f / 4.0f);
    float dist = std::sqrt((endX - midX) * (endX - midX) + (endY - midY) * (endY - midY));
    CHECK(dist <= 0.02f);  // r = 1
}

TEST_CASE("trimPath: Close is treated as a line segment") {
    // Closed unit square: (0,0)->(10,0)->(10,10)->(0,10)->Close (back to (0,0)).
    std::vector<PathVerb> verbs{PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Line,
                                PathVerb::Close};
    std::vector<float> points{0, 0, 10, 0, 10, 10, 0, 10};
    std::vector<PathVerb> outVerbs;
    std::vector<float> outPoints;
    trimPath(verbs, points, 0.875f, 1.0f, 0.0f, outVerbs, outPoints);
    // Perimeter 40; range [35,40] is the second half of the last edge (0,10)->(0,0): (0,5)->(0,0).
    REQUIRE(outVerbs.size() == 2);
    CHECK(outVerbs[0] == PathVerb::Move);
    CHECK(outVerbs[1] == PathVerb::Line);
    REQUIRE(outPoints.size() == 4);
    CHECK(outPoints[0] == doctest::Approx(0.0f));
    CHECK(outPoints[1] == doctest::Approx(5.0f));
    CHECK(outPoints[2] == doctest::Approx(0.0f));
    CHECK(outPoints[3] == doctest::Approx(0.0f));
}

TEST_CASE("trimPath: reversed interval is swapped") {
    const xt::PathVerb verbs[] = {xt::PathVerb::Move, xt::PathVerb::Line};
    const float pts[] = {0.f, 0.f, 10.f, 0.f};
    std::vector<xt::PathVerb> ov;
    std::vector<float> op;
    xt::trimPath(verbs, pts, 0.5f, 0.2f, 0.0f, ov, op);
    REQUIRE(op.size() == 4);
    CHECK(op[0] == doctest::Approx(2.f));
    CHECK(op[2] == doctest::Approx(5.f));
}
