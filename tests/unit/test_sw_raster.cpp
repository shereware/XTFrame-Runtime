#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "doctest.h"
#include "rounded_rect.h"
#include "sw_raster.h"
#include "xt/geom.h"
#include "xt/node.h"
#include "xt/types.h"

/// Rasterizer unit tests. See docs/tasks/T-11.md "測試" / "test_sw_raster.cpp".

using namespace xt;
using namespace xt::sw;

namespace {

/// Sweeps `raster` into a dense width*height coverage buffer.
std::vector<uint8_t> sweepToBuffer(const Rasterizer& raster, int width, int height, FillRule rule) {
    std::vector<uint8_t> buf(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
    raster.sweep(rule, [&](int y, int x0, int x1, uint8_t coverage) {
        for (int x = x0; x < x1; ++x) {
            buf[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] = coverage;
        }
    });
    return buf;
}

int sumCoverage(const std::vector<uint8_t>& buf) {
    int sum = 0;
    for (uint8_t v : buf) {
        sum += v;
    }
    return sum;
}

/// Adds the 4 edges of an axis-aligned rectangle, closed, in the given corner order.
void addRect(Rasterizer& raster, float x0, float y0, float x1, float y1, bool reversed = false) {
    Vec2 corners[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    if (!reversed) {
        raster.addLine(corners[0], corners[1]);
        raster.addLine(corners[1], corners[2]);
        raster.addLine(corners[2], corners[3]);
        raster.addLine(corners[3], corners[0]);
    } else {
        raster.addLine(corners[0], corners[3]);
        raster.addLine(corners[3], corners[2]);
        raster.addLine(corners[2], corners[1]);
        raster.addLine(corners[1], corners[0]);
    }
}

}  // namespace

TEST_CASE("Rasterizer: 10x10 axis-aligned rect (2,2)-(8,8), NonZero") {
    Rasterizer raster;
    raster.reset(10, 10);
    addRect(raster, 2, 2, 8, 8);
    std::vector<uint8_t> buf = sweepToBuffer(raster, 10, 10, FillRule::NonZero);
    for (int y = 0; y < 10; ++y) {
        for (int x = 0; x < 10; ++x) {
            bool inside = x >= 2 && x < 8 && y >= 2 && y < 8;
            uint8_t expected = inside ? 255 : 0;
            CHECK(buf[static_cast<size_t>(y) * 10 + static_cast<size_t>(x)] == expected);
        }
    }
    CHECK(sumCoverage(buf) == 36 * 255);
}

TEST_CASE("Rasterizer: same rect, reversed (CCW) winding gives the same NonZero result") {
    Rasterizer raster;
    raster.reset(10, 10);
    addRect(raster, 2, 2, 8, 8, /*reversed=*/true);
    std::vector<uint8_t> buf = sweepToBuffer(raster, 10, 10, FillRule::NonZero);
    for (int y = 0; y < 10; ++y) {
        for (int x = 0; x < 10; ++x) {
            bool inside = x >= 2 && x < 8 && y >= 2 && y < 8;
            uint8_t expected = inside ? 255 : 0;
            CHECK(buf[static_cast<size_t>(y) * 10 + static_cast<size_t>(x)] == expected);
        }
    }
    CHECK(sumCoverage(buf) == 36 * 255);
}

TEST_CASE("Rasterizer: half-pixel offset rect (2.5,2)-(8.5,8)") {
    Rasterizer raster;
    raster.reset(10, 10);
    addRect(raster, 2.5f, 2, 8.5f, 8);
    std::vector<uint8_t> buf = sweepToBuffer(raster, 10, 10, FillRule::NonZero);
    for (int y = 2; y < 8; ++y) {
        int base = y * 10;
        CHECK(std::abs(static_cast<int>(buf[static_cast<size_t>(base + 2)]) - 128) <= 1);
        CHECK(std::abs(static_cast<int>(buf[static_cast<size_t>(base + 8)]) - 128) <= 1);
        for (int x = 3; x < 8; ++x) {
            CHECK(buf[static_cast<size_t>(base + x)] == 255);
        }
        CHECK(buf[static_cast<size_t>(base + 1)] == 0);
        CHECK(buf[static_cast<size_t>(base + 9)] == 0);
    }
}

TEST_CASE("Rasterizer: two overlapping same-direction rects, NonZero vs EvenOdd") {
    Rasterizer raster;
    raster.reset(10, 10);
    addRect(raster, 1, 1, 6, 6);
    addRect(raster, 3, 3, 8, 8);
    std::vector<uint8_t> nz = sweepToBuffer(raster, 10, 10, FillRule::NonZero);
    CHECK(nz[3 * 10 + 4] == 255);  // overlap region (3..6, 3..6)

    Rasterizer raster2;
    raster2.reset(10, 10);
    addRect(raster2, 1, 1, 6, 6);
    addRect(raster2, 3, 3, 8, 8);
    std::vector<uint8_t> eo = sweepToBuffer(raster2, 10, 10, FillRule::EvenOdd);
    CHECK(eo[3 * 10 + 4] == 0);    // overlap cancels out under EvenOdd
    CHECK(eo[1 * 10 + 2] == 255);  // non-overlapping part of the first rect still filled
}

TEST_CASE("Rasterizer: triangle (0,0),(10,0),(0,10)") {
    Rasterizer raster;
    raster.reset(10, 10);
    raster.addLine(Vec2{0, 0}, Vec2{10, 0});
    raster.addLine(Vec2{10, 0}, Vec2{0, 10});
    raster.addLine(Vec2{0, 10}, Vec2{0, 0});
    std::vector<uint8_t> buf = sweepToBuffer(raster, 10, 10, FillRule::NonZero);
    for (int y = 0; y < 9; ++y) {
        int x = 9 - y;  // pixel straddling the hypotenuse x + y == 10
        if (x < 0 || x >= 10) {
            continue;
        }
        int v = buf[static_cast<size_t>(y) * 10 + static_cast<size_t>(x)];
        CHECK(std::abs(v - 128) <= 2);
    }
    int sum = sumCoverage(buf);
    CHECK(sum >= static_cast<int>(50 * 255 * 0.98));
    CHECK(sum <= static_cast<int>(50 * 255 * 1.02));
}

TEST_CASE("Rasterizer: segments outside bounds do not crash and clip correctly") {
    Rasterizer raster;
    raster.reset(10, 10);
    addRect(raster, -5, -5, 5, 5);
    std::vector<uint8_t> buf = sweepToBuffer(raster, 10, 10, FillRule::NonZero);
    for (int y = 0; y < 10; ++y) {
        for (int x = 0; x < 10; ++x) {
            bool inside = x < 5 && y < 5;
            uint8_t expected = inside ? 255 : 0;
            CHECK(buf[static_cast<size_t>(y) * 10 + static_cast<size_t>(x)] == expected);
        }
    }
}

TEST_CASE(
    "Rasterizer: rounded rect (flattenPath + appendRoundedRectPath) has zero cover past its right edge") {
    // T-11b repro: a closed contour's cover must sum to 0 on every row, so no row should show any
    // coverage to the right of the shape's flattened AABB.
    // Matches tests/golden/ui/border_radius.xt's box (docs/tasks/T-11b.md's repro fixture).
    Rect box{40, 40, 150, 100};
    float tl = 30;
    float tr = 5;
    float br = 30;
    float bl = 5;
    clampRoundedRectRadii(box.w, box.h, tl, tr, br, bl);

    std::vector<PathVerb> verbs;
    std::vector<float> points;
    appendRoundedRectPath(box, tl, tr, br, bl, verbs, points);

    std::vector<Vec2> outPoints;
    std::vector<uint32_t> starts;
    std::vector<uint8_t> closedFlags;
    Mat2x3 identity{};
    flattenPath(verbs, points, identity, kFlattenTolerance, outPoints, starts, closedFlags);
    REQUIRE(outPoints.size() >= 3);

    float maxX = 0.0f;
    for (const Vec2& p : outPoints) {
        maxX = std::max(maxX, p.x);
    }
    int rightEdge = static_cast<int>(std::ceil(maxX)) + 1;  // one pixel past the shape's AABB.

    constexpr int kSize = 256;
    Rasterizer raster;
    raster.reset(kSize, kSize);
    for (size_t j = 0; j + 1 < outPoints.size(); ++j) {
        raster.addLine(outPoints[j], outPoints[j + 1]);
    }
    raster.addLine(outPoints.back(), outPoints.front());

    std::vector<uint8_t> buf = sweepToBuffer(raster, kSize, kSize, FillRule::NonZero);
    for (int y = 0; y < kSize; ++y) {
        for (int x = rightEdge; x < kSize; ++x) {
            CHECK(buf[static_cast<size_t>(y) * kSize + static_cast<size_t>(x)] == 0);
        }
    }
}

TEST_CASE("Rasterizer: random 200-segment closed polygon has zero net cover on every row") {
    // T-11b repro: cover accumulated by addLine must telescope exactly for a closed contour,
    // regardless of how many small segments make it up (fixed seed for reproducibility).
    constexpr int kSize = 256;
    Rasterizer raster;
    raster.reset(kSize, kSize);

    std::mt19937 rng(12345u);
    std::uniform_real_distribution<float> coord(-50.0f, 306.0f);  // some points fall outside bounds.
    std::vector<Vec2> pts;
    pts.reserve(200);
    for (int i = 0; i < 200; ++i) {
        pts.push_back(Vec2{coord(rng), coord(rng)});
    }
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        raster.addLine(pts[i], pts[i + 1]);
    }
    raster.addLine(pts.back(), pts.front());

    for (int y = 0; y < kSize; ++y) {
        CHECK(raster.debugRowCoverSum(y) == 0);
    }
}

TEST_CASE("Rasterizer: reset retains row storage capacity") {
    Rasterizer raster;
    raster.reset(10, 10);
    addRect(raster, 2, 2, 8, 8);
    size_t capBefore = raster.rowCapacity(2);
    CHECK(capBefore > 0);
    raster.reset(10, 10);
    CHECK(raster.rowCapacity(2) == capBefore);
}
