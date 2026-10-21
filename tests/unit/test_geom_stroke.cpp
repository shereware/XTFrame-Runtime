#include <algorithm>
#include <cmath>
#include <vector>

#include "doctest.h"
#include "xt/geom.h"

using namespace xt;

namespace {

bool pointInPolygon(std::span<const Vec2> pts, Vec2 p) {
    size_t n = pts.size();
    // Treat exact/near-vertex coincidence as "inside": ray casting is ill-defined exactly on the
    // boundary, and the geometric join construction can legitimately place a vertex exactly at the
    // queried corner point.
    for (const Vec2& v : pts) {
        Vec2 d = v - p;
        if (std::sqrt(d.x * d.x + d.y * d.y) < 1e-3f) {
            return true;
        }
    }
    bool inside = false;
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const Vec2& a = pts[i];
        const Vec2& b = pts[j];
        bool crosses = ((a.y > p.y) != (b.y > p.y));
        if (crosses) {
            float xCross = (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x;
            if (p.x < xCross) {
                inside = !inside;
            }
        }
    }
    return inside;
}

bool pointInAnyContour(const std::vector<Vec2>& pts, const std::vector<uint32_t>& starts, Vec2 p) {
    for (size_t i = 0; i < starts.size(); ++i) {
        uint32_t s = starts[i];
        uint32_t e = (i + 1 < starts.size()) ? starts[i + 1] : static_cast<uint32_t>(pts.size());
        std::span<const Vec2> contour(pts.data() + s, static_cast<size_t>(e - s));
        if (contour.size() >= 3 && pointInPolygon(contour, p)) {
            return true;
        }
    }
    return false;
}

float sumAreas(const std::vector<Vec2>& pts, const std::vector<uint32_t>& starts) {
    float total = 0.0f;
    for (size_t i = 0; i < starts.size(); ++i) {
        uint32_t s = starts[i];
        uint32_t e = (i + 1 < starts.size()) ? starts[i + 1] : static_cast<uint32_t>(pts.size());
        std::span<const Vec2> contour(pts.data() + s, static_cast<size_t>(e - s));
        total += polygonSignedArea(contour);
    }
    return total;
}

void checkAllClockwise(const std::vector<Vec2>& pts, const std::vector<uint32_t>& starts) {
    for (size_t i = 0; i < starts.size(); ++i) {
        uint32_t s = starts[i];
        uint32_t e = (i + 1 < starts.size()) ? starts[i + 1] : static_cast<uint32_t>(pts.size());
        std::span<const Vec2> contour(pts.data() + s, static_cast<size_t>(e - s));
        if (contour.size() >= 3) {
            CHECK(polygonSignedArea(contour) > 0.0f);
        }
    }
}

}  // namespace

TEST_CASE("strokeToFill: horizontal line, Butt cap") {
    std::vector<Vec2> pts{{0, 0}, {10, 0}};
    std::vector<uint32_t> starts{0};
    std::vector<uint8_t> closedFlags{0};
    std::vector<Vec2> outPts;
    std::vector<uint32_t> outStarts;
    strokeToFill(pts, starts, closedFlags, 2.0f, LineCap::Butt, LineJoin::Miter, 4.0f, kFlattenTolerance,
                 outPts, outStarts);
    REQUIRE(outStarts.size() == 1);
    CHECK(outPts.size() == 4);
    CHECK(sumAreas(outPts, outStarts) == doctest::Approx(20.0f).epsilon(1e-3));
    checkAllClockwise(outPts, outStarts);
}

TEST_CASE("strokeToFill: horizontal line, Square cap") {
    std::vector<Vec2> pts{{0, 0}, {10, 0}};
    std::vector<uint32_t> starts{0};
    std::vector<uint8_t> closedFlags{0};
    std::vector<Vec2> outPts;
    std::vector<uint32_t> outStarts;
    strokeToFill(pts, starts, closedFlags, 2.0f, LineCap::Square, LineJoin::Miter, 4.0f, kFlattenTolerance,
                 outPts, outStarts);
    CHECK(sumAreas(outPts, outStarts) == doctest::Approx(24.0f).epsilon(1e-3));
    checkAllClockwise(outPts, outStarts);
}

TEST_CASE("strokeToFill: horizontal line, Round cap") {
    std::vector<Vec2> pts{{0, 0}, {10, 0}};
    std::vector<uint32_t> starts{0};
    std::vector<uint8_t> closedFlags{0};
    std::vector<Vec2> outPts;
    std::vector<uint32_t> outStarts;
    strokeToFill(pts, starts, closedFlags, 2.0f, LineCap::Round, LineJoin::Miter, 4.0f, 0.01f, outPts,
                 outStarts);
    float expected = 20.0f + 3.14159265358979323846f;
    CHECK(sumAreas(outPts, outStarts) == doctest::Approx(expected).epsilon(0.03));
    checkAllClockwise(outPts, outStarts);
}

TEST_CASE("strokeToFill: L shape Miter join covers the outer corner") {
    std::vector<Vec2> pts{{0, 0}, {10, 0}, {10, 10}};
    std::vector<uint32_t> starts{0};
    std::vector<uint8_t> closedFlags{0};
    std::vector<Vec2> outPts;
    std::vector<uint32_t> outStarts;
    strokeToFill(pts, starts, closedFlags, 2.0f, LineCap::Butt, LineJoin::Miter, 4.0f, kFlattenTolerance,
                 outPts, outStarts);
    CHECK(pointInAnyContour(outPts, outStarts, Vec2{11, -1}));
    checkAllClockwise(outPts, outStarts);
}

TEST_CASE("strokeToFill: L shape Bevel join does not cover the outer corner") {
    std::vector<Vec2> pts{{0, 0}, {10, 0}, {10, 10}};
    std::vector<uint32_t> starts{0};
    std::vector<uint8_t> closedFlags{0};
    std::vector<Vec2> outPts;
    std::vector<uint32_t> outStarts;
    strokeToFill(pts, starts, closedFlags, 2.0f, LineCap::Butt, LineJoin::Bevel, 4.0f, kFlattenTolerance,
                 outPts, outStarts);
    CHECK_FALSE(pointInAnyContour(outPts, outStarts, Vec2{11, -1}));
    checkAllClockwise(outPts, outStarts);
}

TEST_CASE("strokeToFill: Miter exceeding limit degrades to Bevel") {
    // d0 = (1,0); d1 makes a 170-degree turn with d0, i.e. a 10-degree interior angle spike.
    float alpha = 170.0f * 0.017453292519943295f;  // turn angle between d0 and d1
    Vec2 d0{1, 0};
    Vec2 d1{std::cos(alpha), std::sin(alpha)};
    Vec2 v{10, 0};
    std::vector<Vec2> pts{{0, 0}, v, v + d1 * 10.0f};
    std::vector<uint32_t> starts{0};
    std::vector<uint8_t> closedFlags{0};
    std::vector<Vec2> outPts;
    std::vector<uint32_t> outStarts;
    strokeToFill(pts, starts, closedFlags, 2.0f, LineCap::Butt, LineJoin::Miter, 4.0f, kFlattenTolerance,
                 outPts, outStarts);
    checkAllClockwise(outPts, outStarts);

    // Reproduce the theoretical (undegraded) miter tip and confirm it is NOT covered.
    float cross = d0.x * d1.y - d0.y * d1.x;
    float side = (cross >= 0.0f) ? -1.0f : 1.0f;
    Vec2 n0{-d0.y * side, d0.x * side};
    Vec2 n1{-d1.y * side, d1.x * side};
    Vec2 bis = n0 + n1;
    float bisLen = std::sqrt(bis.x * bis.x + bis.y * bis.y);
    bis = Vec2{bis.x / bisLen, bis.y / bisLen};
    float cosAlpha = d0.x * d1.x + d0.y * d1.y;
    float halfCos = std::sqrt((1.0f + cosAlpha) * 0.5f);
    float ratio = 1.0f / halfCos;
    CHECK(ratio > 4.0f);  // confirms this case indeed exceeds miterLimit
    Vec2 theoreticalTip = v + bis * (1.0f * ratio);
    CHECK_FALSE(pointInAnyContour(outPts, outStarts, theoreticalTip));
}

TEST_CASE("strokeToFill: closed square covers inner and outer edge, not the center") {
    std::vector<Vec2> pts{{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    std::vector<uint32_t> starts{0};
    std::vector<uint8_t> closedFlags{1};
    std::vector<Vec2> outPts;
    std::vector<uint32_t> outStarts;
    strokeToFill(pts, starts, closedFlags, 2.0f, LineCap::Butt, LineJoin::Miter, 4.0f, kFlattenTolerance,
                 outPts, outStarts);
    CHECK(pointInAnyContour(outPts, outStarts, Vec2{0, -1}));       // outside the square
    CHECK(pointInAnyContour(outPts, outStarts, Vec2{0, 1}));        // inside the stroke band
    CHECK_FALSE(pointInAnyContour(outPts, outStarts, Vec2{5, 5}));  // center, not covered
    checkAllClockwise(outPts, outStarts);
}

TEST_CASE("strokeToFill: single-point contour") {
    std::vector<Vec2> pts{{0, 0}};
    std::vector<uint32_t> starts{0};
    std::vector<uint8_t> closedFlags{0};

    {
        std::vector<Vec2> outPts;
        std::vector<uint32_t> outStarts;
        strokeToFill(pts, starts, closedFlags, 2.0f, LineCap::Round, LineJoin::Miter, 4.0f, 0.01f, outPts,
                     outStarts);
        float expected = 3.14159265358979323846f;  // pi * r^2, r=1
        CHECK(sumAreas(outPts, outStarts) == doctest::Approx(expected).epsilon(0.03));
        checkAllClockwise(outPts, outStarts);
    }
    {
        std::vector<Vec2> outPts;
        std::vector<uint32_t> outStarts;
        strokeToFill(pts, starts, closedFlags, 2.0f, LineCap::Butt, LineJoin::Miter, 4.0f, kFlattenTolerance,
                     outPts, outStarts);
        CHECK(outPts.empty());
        CHECK(outStarts.empty());
    }
}

// T-233 (31 §31.4.7): variable-width strokeToFill.

TEST_CASE("strokeToFill width fn: a line tapered at both ends is 0 wide at both ends") {
    std::vector<Vec2> pts{{0, 0}, {100, 0}};
    std::vector<uint32_t> starts{0};
    std::vector<uint8_t> closedFlags{0};
    auto width = [](float u) { return 10.0f * std::min(u / 0.5f, (1.0f - u) / 0.5f); };
    std::vector<Vec2> outPts;
    std::vector<uint32_t> outStarts;
    strokeToFill(pts, starts, closedFlags, WidthFn(width), LineCap::Round, LineJoin::Miter, 4.0f,
                 kFlattenTolerance, outPts, outStarts);
    REQUIRE_FALSE(outStarts.empty());
    checkAllClockwise(outPts, outStarts);
    // Full width (10) in the middle.
    CHECK(pointInAnyContour(outPts, outStarts, Vec2{50, 4.5f}));
    CHECK(pointInAnyContour(outPts, outStarts, Vec2{50, -4.5f}));
    CHECK_FALSE(pointInAnyContour(outPts, outStarts, Vec2{50, 5.5f}));
    // Width 0 at both ends: the Round caps degenerate to points.
    CHECK_FALSE(pointInAnyContour(outPts, outStarts, Vec2{1, 0.3f}));
    CHECK_FALSE(pointInAnyContour(outPts, outStarts, Vec2{99, 0.3f}));
    CHECK_FALSE(pointInAnyContour(outPts, outStarts, Vec2{-0.5f, 0}));
    CHECK_FALSE(pointInAnyContour(outPts, outStarts, Vec2{100.5f, 0}));
    float maxAbsYNearEnds = 0.0f;
    for (const Vec2& p : outPts) {
        if (p.x <= 0.0f || p.x >= 100.0f) {
            maxAbsYNearEnds = std::max(maxAbsYNearEnds, std::fabs(p.y));
        }
    }
    CHECK(maxAbsYNearEnds == doctest::Approx(0.0f));
    // Area = length * mean width = 100 * 5.
    CHECK(sumAreas(outPts, outStarts) == doctest::Approx(500.0f).epsilon(0.01));
}

TEST_CASE("strokeToFill width fn: profile 0:1 0.5:2 1:1 is twice as wide in the middle") {
    std::vector<Vec2> pts{{0, 0}, {100, 0}};
    std::vector<uint32_t> starts{0};
    std::vector<uint8_t> closedFlags{0};
    auto width = [](float u) { return 10.0f * (u < 0.5f ? 1.0f + u / 0.5f : 1.0f + (1.0f - u) / 0.5f); };
    std::vector<Vec2> outPts;
    std::vector<uint32_t> outStarts;
    strokeToFill(pts, starts, closedFlags, WidthFn(width), LineCap::Butt, LineJoin::Miter, 4.0f,
                 kFlattenTolerance, outPts, outStarts);
    checkAllClockwise(outPts, outStarts);
    CHECK(pointInAnyContour(outPts, outStarts, Vec2{50, 9.5f}));
    CHECK_FALSE(pointInAnyContour(outPts, outStarts, Vec2{50, 10.5f}));
    CHECK(pointInAnyContour(outPts, outStarts, Vec2{0.5f, 4.9f}));
    CHECK_FALSE(pointInAnyContour(outPts, outStarts, Vec2{0.5f, 5.3f}));
    CHECK(sumAreas(outPts, outStarts) == doctest::Approx(1500.0f).epsilon(0.01));
}

TEST_CASE("strokeToFill width fn: a constant width matches the constant overload's coverage") {
    std::vector<Vec2> pts{{0, 0}, {100, 0}, {100, 100}, {0, 100}};
    std::vector<uint32_t> starts{0};
    std::vector<uint8_t> closedFlags{1};
    auto width = [](float) { return 4.0f; };
    std::vector<Vec2> outPts;
    std::vector<uint32_t> outStarts;
    strokeToFill(pts, starts, closedFlags, WidthFn(width), LineCap::Butt, LineJoin::Miter, 4.0f,
                 kFlattenTolerance, outPts, outStarts);
    checkAllClockwise(outPts, outStarts);
    CHECK(pointInAnyContour(outPts, outStarts, Vec2{50, 1.5f}));
    CHECK(pointInAnyContour(outPts, outStarts, Vec2{-1.5f, -1.5f}));  // Miter corner at the seam.
    CHECK(pointInAnyContour(outPts, outStarts, Vec2{101.5f, 101.5f}));
    CHECK_FALSE(pointInAnyContour(outPts, outStarts, Vec2{50, 50}));
    CHECK_FALSE(pointInAnyContour(outPts, outStarts, Vec2{50, 2.5f}));
}

TEST_CASE("strokeToFill width fn: a cap at width 0 is a point, the other end keeps its cap") {
    std::vector<Vec2> pts{{0, 0}, {100, 0}};
    std::vector<uint32_t> starts{0};
    std::vector<uint8_t> closedFlags{0};
    auto width = [](float u) { return 10.0f * std::min(u / 0.2f, 1.0f); };
    std::vector<Vec2> outPts;
    std::vector<uint32_t> outStarts;
    strokeToFill(pts, starts, closedFlags, WidthFn(width), LineCap::Square, LineJoin::Miter, 4.0f,
                 kFlattenTolerance, outPts, outStarts);
    CHECK_FALSE(pointInAnyContour(outPts, outStarts, Vec2{-1, 0}));
    CHECK(pointInAnyContour(outPts, outStarts, Vec2{104, 4}));  // Square cap, half width 5.
    CHECK(pointInAnyContour(outPts, outStarts, Vec2{10, 2}));   // Width 5 at u = 0.1.
    CHECK_FALSE(pointInAnyContour(outPts, outStarts, Vec2{10, 3}));
}
