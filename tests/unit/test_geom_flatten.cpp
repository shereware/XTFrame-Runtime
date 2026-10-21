#include <cmath>
#include <vector>

#include "doctest.h"
#include "xt/geom.h"

using namespace xt;

namespace {
constexpr Mat2x3 kIdentity{};
}

TEST_CASE("flattenPath: single Move+Line") {
    std::vector<PathVerb> verbs{PathVerb::Move, PathVerb::Line};
    std::vector<float> points{0, 0, 10, 0};
    std::vector<Vec2> outPoints;
    std::vector<uint32_t> starts;
    std::vector<uint8_t> closedFlags;
    flattenPath(verbs, points, kIdentity, kFlattenTolerance, outPoints, starts, closedFlags);
    CHECK(outPoints.size() == 2);
    CHECK(starts.size() == 1);
    CHECK(starts[0] == 0);
    CHECK(closedFlags.size() == 1);
    CHECK(closedFlags[0] == 0);
}

TEST_CASE("flattenPath: Move+Line+Close is closed") {
    std::vector<PathVerb> verbs{PathVerb::Move, PathVerb::Line, PathVerb::Close};
    std::vector<float> points{0, 0, 10, 0};
    std::vector<Vec2> outPoints;
    std::vector<uint32_t> starts;
    std::vector<uint8_t> closedFlags;
    flattenPath(verbs, points, kIdentity, kFlattenTolerance, outPoints, starts, closedFlags);
    CHECK(outPoints.size() == 2);
    CHECK(starts.size() == 1);
    CHECK(closedFlags.size() == 1);
    CHECK(closedFlags[0] == 1);
}

TEST_CASE("flattenPath: cubic quarter circle stays close to the arc") {
    // Quarter circle of radius 10 centered at origin, from (10,0) to (0,10), standard kappa constant.
    // (Radius 10, rather than 1, so that at tolerance 0.25 the chord-error formula requires more than
    // a single subdivision.)
    constexpr float kRadius = 10.0f;
    constexpr float kKappa = 0.5522847498f * kRadius;
    std::vector<PathVerb> verbs{PathVerb::Move, PathVerb::Cubic};
    std::vector<float> points{kRadius, 0, kRadius, kKappa, kKappa, kRadius, 0, kRadius};

    {
        std::vector<Vec2> outPoints;
        std::vector<uint32_t> starts;
        std::vector<uint8_t> closedFlags;
        flattenPath(verbs, points, kIdentity, 0.25f, outPoints, starts, closedFlags);
        CHECK(outPoints.size() >= 3);
        for (const Vec2& p : outPoints) {
            float dist = std::sqrt(p.x * p.x + p.y * p.y);
            CHECK(std::fabs(dist - kRadius) <= 0.25f + 1e-3f);
        }

        std::vector<Vec2> finePoints;
        std::vector<uint32_t> fineStarts;
        std::vector<uint8_t> fineClosed;
        flattenPath(verbs, points, kIdentity, 0.01f, finePoints, fineStarts, fineClosed);
        CHECK(finePoints.size() > outPoints.size());
        for (const Vec2& p : finePoints) {
            float dist = std::sqrt(p.x * p.x + p.y * p.y);
            CHECK(std::fabs(dist - kRadius) <= 0.01f + 1e-3f);
        }
    }
}

TEST_CASE("cubicSubdivisions: straight control points give 1") {
    Vec2 p0{0, 0};
    Vec2 p1{1.0f / 3.0f, 0};
    Vec2 p2{2.0f / 3.0f, 0};
    Vec2 p3{1, 0};
    CHECK(cubicSubdivisions(p0, p1, p2, p3, 0.25f) == 1);
}

TEST_CASE("cubicSubdivisions: quarter circle of radius 50 needs about 9 segments at 0.25 px") {
    const float k = 0.5522847498f * 50.0f;
    Vec2 p0{50, 0};
    Vec2 p1{50, k};
    Vec2 p2{k, 50};
    Vec2 p3{0, 50};
    const uint32_t n = cubicSubdivisions(p0, p1, p2, p3, 0.25f);
    CHECK(n >= 8);
    CHECK(n <= 12);
}

TEST_CASE("cubicSubdivisions: clamps to 64") {
    Vec2 p0{0, 0};
    Vec2 p1{0, 1000.0f};
    Vec2 p2{1000.0f, -1000.0f};
    Vec2 p3{1, 0};
    CHECK(cubicSubdivisions(p0, p1, p2, p3, 1e-4f) == 64);
}

TEST_CASE("flattenPath: two subpaths produce two contours") {
    std::vector<PathVerb> verbs{PathVerb::Move, PathVerb::Line, PathVerb::Move, PathVerb::Line};
    std::vector<float> points{0, 0, 1, 0, 5, 5, 6, 5};
    std::vector<Vec2> outPoints;
    std::vector<uint32_t> starts;
    std::vector<uint8_t> closedFlags;
    flattenPath(verbs, points, kIdentity, kFlattenTolerance, outPoints, starts, closedFlags);
    CHECK(starts.size() == 2);
    CHECK(closedFlags.size() == 2);
    CHECK(outPoints.size() == 4);
}

TEST_CASE("flattenPath: Move followed by Move gives a single-point contour") {
    std::vector<PathVerb> verbs{PathVerb::Move, PathVerb::Move, PathVerb::Line};
    std::vector<float> points{0, 0, 5, 5, 6, 5};
    std::vector<Vec2> outPoints;
    std::vector<uint32_t> starts;
    std::vector<uint8_t> closedFlags;
    flattenPath(verbs, points, kIdentity, kFlattenTolerance, outPoints, starts, closedFlags);
    REQUIRE(starts.size() == 2);
    // First contour: only the point (0,0).
    uint32_t firstLen = starts[1] - starts[0];
    CHECK(firstLen == 1);
    CHECK(outPoints[starts[0]] == Vec2{0, 0});
    CHECK(closedFlags[0] == 0);
}

TEST_CASE("flattenPath: Move then immediate Close gives a single-point closed contour") {
    std::vector<PathVerb> verbs{PathVerb::Move, PathVerb::Close};
    std::vector<float> points{3, 4};
    std::vector<Vec2> outPoints;
    std::vector<uint32_t> starts;
    std::vector<uint8_t> closedFlags;
    flattenPath(verbs, points, kIdentity, kFlattenTolerance, outPoints, starts, closedFlags);
    REQUIRE(starts.size() == 1);
    CHECK(outPoints.size() == 1);
    CHECK(outPoints[0] == Vec2{3, 4});
    CHECK(closedFlags[0] == 1);
}

TEST_CASE("flattenPath: translation xf is applied to output points") {
    std::vector<PathVerb> verbs{PathVerb::Move, PathVerb::Line};
    std::vector<float> points{0, 0, 10, 0};
    Mat2x3 xf = translation(Vec2{2, 3});
    std::vector<Vec2> outPoints;
    std::vector<uint32_t> starts;
    std::vector<uint8_t> closedFlags;
    flattenPath(verbs, points, xf, kFlattenTolerance, outPoints, starts, closedFlags);
    REQUIRE(outPoints.size() == 2);
    CHECK(outPoints[0] == Vec2{2, 3});
    CHECK(outPoints[1] == Vec2{12, 3});
}
