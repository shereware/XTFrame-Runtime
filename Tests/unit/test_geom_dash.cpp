#include <vector>

#include "doctest.h"
#include "xt/geom.h"

using namespace xt;

namespace {
void checkContour(const std::vector<Vec2>& pts, const std::vector<uint32_t>& starts, size_t i, Vec2 begin,
                  Vec2 endPt) {
    uint32_t s = starts[i];
    uint32_t e = (i + 1 < starts.size()) ? starts[i + 1] : static_cast<uint32_t>(pts.size());
    REQUIRE(e > s);
    CHECK(pts[s].x == doctest::Approx(begin.x));
    CHECK(pts[s].y == doctest::Approx(begin.y));
    CHECK(pts[e - 1].x == doctest::Approx(endPt.x));
    CHECK(pts[e - 1].y == doctest::Approx(endPt.y));
}
}  // namespace

TEST_CASE("applyDash: length-10 line, dashes {2,3}, phase 0") {
    std::vector<Vec2> pts{{0, 0}, {10, 0}};
    std::vector<uint32_t> starts{0};
    std::vector<uint8_t> closedFlags{0};
    std::vector<float> dashes{2, 3};
    std::vector<Vec2> outPts;
    std::vector<uint32_t> outStarts;
    std::vector<uint8_t> outClosed;
    applyDash(pts, starts, closedFlags, dashes, 0.0f, outPts, outStarts, outClosed);
    REQUIRE(outStarts.size() == 3);
    checkContour(outPts, outStarts, 0, Vec2{0, 0}, Vec2{2, 0});
    checkContour(outPts, outStarts, 1, Vec2{5, 0}, Vec2{7, 0});
    checkContour(outPts, outStarts, 2, Vec2{10, 0}, Vec2{10, 0});
    for (uint8_t c : outClosed) {
        CHECK(c == 0);
    }
}

TEST_CASE("applyDash: phase 1") {
    std::vector<Vec2> pts{{0, 0}, {10, 0}};
    std::vector<uint32_t> starts{0};
    std::vector<uint8_t> closedFlags{0};
    std::vector<float> dashes{2, 3};
    std::vector<Vec2> outPts;
    std::vector<uint32_t> outStarts;
    std::vector<uint8_t> outClosed;
    applyDash(pts, starts, closedFlags, dashes, 1.0f, outPts, outStarts, outClosed);
    REQUIRE(outStarts.size() == 3);
    checkContour(outPts, outStarts, 0, Vec2{0, 0}, Vec2{1, 0});
    checkContour(outPts, outStarts, 1, Vec2{4, 0}, Vec2{6, 0});
    checkContour(outPts, outStarts, 2, Vec2{9, 0}, Vec2{10, 0});
}

TEST_CASE("applyDash: phase -1") {
    std::vector<Vec2> pts{{0, 0}, {10, 0}};
    std::vector<uint32_t> starts{0};
    std::vector<uint8_t> closedFlags{0};
    std::vector<float> dashes{2, 3};
    std::vector<Vec2> outPts;
    std::vector<uint32_t> outStarts;
    std::vector<uint8_t> outClosed;
    applyDash(pts, starts, closedFlags, dashes, -1.0f, outPts, outStarts, outClosed);
    REQUIRE(outStarts.size() == 2);
    checkContour(outPts, outStarts, 0, Vec2{1, 0}, Vec2{3, 0});
    checkContour(outPts, outStarts, 1, Vec2{6, 0}, Vec2{8, 0});
}

TEST_CASE("applyDash: odd dash count {2} behaves like {2,2}") {
    std::vector<Vec2> pts{{0, 0}, {10, 0}};
    std::vector<uint32_t> starts{0};
    std::vector<uint8_t> closedFlags{0};
    std::vector<float> dashesOdd{2};
    std::vector<float> dashesEven{2, 2};
    std::vector<Vec2> outPtsOdd, outPtsEven;
    std::vector<uint32_t> outStartsOdd, outStartsEven;
    std::vector<uint8_t> outClosedOdd, outClosedEven;
    applyDash(pts, starts, closedFlags, dashesOdd, 0.0f, outPtsOdd, outStartsOdd, outClosedOdd);
    applyDash(pts, starts, closedFlags, dashesEven, 0.0f, outPtsEven, outStartsEven, outClosedEven);
    REQUIRE(outStartsOdd.size() == outStartsEven.size());
    REQUIRE(outPtsOdd.size() == outPtsEven.size());
    for (size_t i = 0; i < outPtsOdd.size(); ++i) {
        CHECK(outPtsOdd[i].x == doctest::Approx(outPtsEven[i].x));
        CHECK(outPtsOdd[i].y == doctest::Approx(outPtsEven[i].y));
    }
}

TEST_CASE("applyDash: {0,0} passes the contour through unchanged") {
    std::vector<Vec2> pts{{0, 0}, {10, 0}};
    std::vector<uint32_t> starts{0};
    std::vector<uint8_t> closedFlags{0};
    std::vector<float> dashes{0, 0};
    std::vector<Vec2> outPts;
    std::vector<uint32_t> outStarts;
    std::vector<uint8_t> outClosed;
    applyDash(pts, starts, closedFlags, dashes, 0.0f, outPts, outStarts, outClosed);
    REQUIRE(outStarts.size() == 1);
    REQUIRE(outPts.size() == 2);
    CHECK(outPts[0] == Vec2{0, 0});
    CHECK(outPts[1] == Vec2{10, 0});
    CHECK(outClosed[0] == 0);
}

TEST_CASE("applyDash: a dash segment crossing a polyline vertex includes that vertex") {
    // Right-angle polyline: (0,0)-(5,0)-(5,5). One dash covering [3,7] must include (5,0).
    std::vector<Vec2> pts{{0, 0}, {5, 0}, {5, 5}};
    std::vector<uint32_t> starts{0};
    std::vector<uint8_t> closedFlags{0};
    std::vector<float> dashes{4, 100};  // on-length 4, off-length 100 (never repeats within path length)
    std::vector<Vec2> outPts;
    std::vector<uint32_t> outStarts;
    std::vector<uint8_t> outClosed;
    applyDash(pts, starts, closedFlags, dashes, -3.0f, outPts, outStarts, outClosed);
    // With phase -3, the on-run covers s in [3,7], crossing the vertex at (5,0).
    REQUIRE(outStarts.size() >= 1);
    uint32_t s = outStarts[0];
    uint32_t e = (outStarts.size() > 1) ? outStarts[1] : static_cast<uint32_t>(outPts.size());
    bool hasVertex = false;
    for (uint32_t i = s; i < e; ++i) {
        if (outPts[i].x == doctest::Approx(5.0f) && outPts[i].y == doctest::Approx(0.0f)) {
            hasVertex = true;
        }
    }
    CHECK(hasVertex);
}
