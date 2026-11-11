#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include "doctest.h"
#include "xt/command_list.h"
#include "xt/coverage_bitmap.h"
#include "xt/geom.h"
#include "xt/node.h"
#include "xt/sw/sw_renderer.h"
#include "xt/types.h"

/// rasterizeCoverage vs the software renderer: the R8 coverage must equal the alpha SW writes when
/// filling the same path in white on a transparent target. See Docs/tasks/T-660a.md item 4.

using namespace xt;

namespace {

/// A path built in CommandList pool layout (§3.4).
struct TestPath {
    std::vector<PathVerb> verbs;
    std::vector<float> points;

    void move(float x, float y) {
        verbs.push_back(PathVerb::Move);
        points.push_back(x);
        points.push_back(y);
    }
    void line(float x, float y) {
        verbs.push_back(PathVerb::Line);
        points.push_back(x);
        points.push_back(y);
    }
    void cubic(float x1, float y1, float x2, float y2, float x3, float y3) {
        verbs.push_back(PathVerb::Cubic);
        float p[6] = {x1, y1, x2, y2, x3, y3};
        points.insert(points.end(), p, p + 6);
    }
    void close() { verbs.push_back(PathVerb::Close); }

    /// A circle of four cubics, clockwise in y-down space.
    void circle(float cx, float cy, float r) {
        const float k = 0.5522847f * r;
        move(cx + r, cy);
        cubic(cx + r, cy + k, cx + k, cy + r, cx, cy + r);
        cubic(cx - k, cy + r, cx - r, cy + k, cx - r, cy);
        cubic(cx - r, cy - k, cx - k, cy - r, cx, cy - r);
        cubic(cx + k, cy - r, cx + r, cy - k, cx + r, cy);
        close();
    }
};

constexpr int kSize = 200;

/// Renders `path` with SW in opaque white on a transparent kSize x kSize target; returns alpha.
std::vector<uint8_t> swAlpha(const TestPath& path, const Mat2x3& xf, FillRule rule) {
    std::vector<uint8_t> buf(static_cast<size_t>(kSize) * kSize * 4, 0);
    sw::SwRenderer renderer;
    renderer.setTarget(sw::SwTarget{buf.data(), kSize, kSize, kSize * 4});
    renderer.clear(Color{0, 0, 0, 0});

    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(kSize), static_cast<float>(kSize)};
    cl.devicePixelRatio = 1;
    cl.verbs = path.verbs;
    cl.points = path.points;
    FillCmd f{};
    f.xf = xf;
    f.path =
        PathRef{0, static_cast<uint32_t>(path.verbs.size()), 0, static_cast<uint32_t>(path.points.size())};
    f.paint.kind = PaintKind::Solid;
    f.paint.color = Color{1, 1, 1, 1};
    f.rule = rule;
    f.antialias = 1;
    cl.fills.push_back(f);
    Command cmd{};
    cmd.kind = CmdKind::FillPath;
    cmd.index = 0;
    cl.commands.push_back(cmd);
    renderer.render(cl, nullptr, nullptr);

    std::vector<uint8_t> alpha(static_cast<size_t>(kSize) * kSize);
    for (size_t i = 0; i < alpha.size(); ++i) {
        alpha[i] = buf[i * 4 + 3];
    }
    return alpha;
}

/// Rasterizes `path` and compares it with SW's alpha over the whole target, byte for byte.
void checkMatchesSw(const TestPath& path, const Mat2x3& xf, FillRule rule) {
    CoverageBitmap bm;
    REQUIRE(rasterizeCoverage(path.verbs, path.points, xf, rule, bm));
    REQUIRE(bm.width > 2);
    REQUIRE(bm.height > 2);
    REQUIRE(bm.originX >= 0);
    REQUIRE(bm.originY >= 0);
    REQUIRE(bm.originX + bm.width <= kSize);
    REQUIRE(bm.originY + bm.height <= kSize);
    REQUIRE(bm.pixels.size() == static_cast<size_t>(bm.width) * static_cast<size_t>(bm.height));

    // 1 px transparent border on every side.
    for (int x = 0; x < bm.width; ++x) {
        CHECK(bm.pixels[static_cast<size_t>(x)] == 0);
        CHECK(bm.pixels[static_cast<size_t>(bm.height - 1) * static_cast<size_t>(bm.width) +
                        static_cast<size_t>(x)] == 0);
    }
    for (int y = 0; y < bm.height; ++y) {
        CHECK(bm.pixels[static_cast<size_t>(y) * static_cast<size_t>(bm.width)] == 0);
        CHECK(bm.pixels[static_cast<size_t>(y) * static_cast<size_t>(bm.width) +
                        static_cast<size_t>(bm.width - 1)] == 0);
    }

    const std::vector<uint8_t> alpha = swAlpha(path, xf, rule);
    int mismatches = 0;
    int covered = 0;
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const int bx = x - bm.originX;
            const int by = y - bm.originY;
            uint8_t cov = 0;
            if (bx >= 0 && by >= 0 && bx < bm.width && by < bm.height) {
                cov = bm.pixels[static_cast<size_t>(by) * static_cast<size_t>(bm.width) +
                                static_cast<size_t>(bx)];
            }
            const uint8_t a = alpha[static_cast<size_t>(y) * kSize + static_cast<size_t>(x)];
            if (cov != a) {
                ++mismatches;
            }
            if (cov != 0) {
                ++covered;
            }
        }
    }
    CHECK(mismatches == 0);
    CHECK(covered > 0);
}

}  // namespace

TEST_CASE("rasterizeCoverage: concave five-point star matches SW alpha byte for byte") {
    TestPath p;
    const float cx = 100.37f, cy = 98.61f;
    for (int i = 0; i < 10; ++i) {
        const float r = (i % 2 == 0) ? 80.0f : 32.0f;
        const float ang = -1.5707963f + static_cast<float>(i) * 0.62831853f;
        const float x = cx + r * std::cos(ang);
        const float y = cy + r * std::sin(ang);
        if (i == 0) {
            p.move(x, y);
        } else {
            p.line(x, y);
        }
    }
    p.close();
    checkMatchesSw(p, Mat2x3{}, FillRule::NonZero);
}

TEST_CASE("rasterizeCoverage: EvenOdd donut matches SW alpha byte for byte") {
    TestPath p;
    p.circle(97.3f, 101.8f, 70.25f);
    p.circle(97.3f, 101.8f, 33.6f);  // same direction: only EvenOdd leaves the hole
    checkMatchesSw(p, Mat2x3{}, FillRule::EvenOdd);
}

TEST_CASE("rasterizeCoverage: self-intersecting pentagram (NonZero) matches SW alpha byte for byte") {
    TestPath p;
    const float cx = 99.5f, cy = 102.25f, r = 85.0f;
    for (int i = 0; i < 5; ++i) {
        const float ang = -1.5707963f + static_cast<float>((i * 2) % 5) * 1.2566371f;
        const float x = cx + r * std::cos(ang);
        const float y = cy + r * std::sin(ang);
        if (i == 0) {
            p.move(x, y);
        } else {
            p.line(x, y);
        }
    }
    p.close();
    checkMatchesSw(p, Mat2x3{}, FillRule::NonZero);
}

TEST_CASE("rasterizeCoverage: thin sliver triangle matches SW alpha byte for byte") {
    TestPath p;
    p.move(10.3f, 20.1f);
    p.line(187.7f, 34.6f);
    p.line(10.3f, 21.05f);
    p.close();
    checkMatchesSw(p, Mat2x3{}, FillRule::NonZero);
}

TEST_CASE("rasterizeCoverage: concave shape rotated 30 degrees matches SW alpha byte for byte") {
    TestPath p;  // an arrow with a curved notch, centered on the origin
    p.move(-50, -15);
    p.line(10, -15);
    p.line(10, -40);
    p.line(55, 0);
    p.line(10, 40);
    p.line(10, 15);
    p.line(-50, 15);
    p.cubic(-30, 8, -30, -8, -50, -15);
    p.close();
    const Mat2x3 xf = mul(translation(Vec2{100.4f, 99.7f}), rotationDeg(30.0f));
    checkMatchesSw(p, xf, FillRule::NonZero);
}

TEST_CASE("rasterizeCoverage: refuses zero-area and oversized shapes without touching pixels") {
    CoverageBitmap bm;
    bm.pixels.assign(7, 42);
    TestPath flat;
    flat.move(10, 10);
    flat.line(50, 10);
    flat.close();
    CHECK_FALSE(rasterizeCoverage(flat.verbs, flat.points, Mat2x3{}, FillRule::NonZero, bm));
    CHECK(bm.pixels.size() == 7);

    TestPath big;  // 4095 wide + borders = 4097 > kMaxCoverageBitmapSide (T-672)
    big.move(0, 0);
    big.line(4095, 0);
    big.line(4095, 10);
    big.close();
    CHECK_FALSE(rasterizeCoverage(big.verbs, big.points, Mat2x3{}, FillRule::NonZero, bm));
    CHECK(bm.pixels.size() == 7);

    TestPath tall;  // the same limit on the height
    tall.move(0, 0);
    tall.line(10, 0);
    tall.line(10, 4095);
    tall.close();
    CHECK_FALSE(rasterizeCoverage(tall.verbs, tall.points, Mat2x3{}, FillRule::NonZero, bm));
    CHECK(bm.pixels.size() == 7);

    TestPath ok;  // 4094 wide + borders = exactly 4096
    ok.move(0, 0);
    ok.line(4094, 0);
    ok.line(4094, 10);
    ok.close();
    CHECK(rasterizeCoverage(ok.verbs, ok.points, Mat2x3{}, FillRule::NonZero, bm));
    CHECK(bm.width == 4096);
    CHECK(bm.width == kMaxCoverageBitmapSide);
    CHECK(bm.originX == -1);

    TestPath none;
    CHECK_FALSE(rasterizeCoverage(none.verbs, none.points, Mat2x3{}, FillRule::NonZero, bm));
}

TEST_CASE(
    "rasterizeCoverageContours: a stroke's strokeToFill outline matches SW's stroke alpha byte for byte") {
    // T-661 (§10.11 item 3): SW strokes by flattening the path in device space, (dashing,)
    // strokeToFill at width * sqrt|det xf| and a NonZero fill of the outline; the same outline
    // rasterized here must give SW's alpha. A polyline with a sharp miter, a round join and a
    // 0.25 px polyline, under a rotation at a fractional offset.
    struct Case {
        float width;
        LineJoin join;
        LineCap cap;
    };
    const Case cases[] = {
        {2.0f, LineJoin::Miter, LineCap::Butt},
        {3.0f, LineJoin::Round, LineCap::Round},
        {0.25f, LineJoin::Miter, LineCap::Butt},
    };
    const Mat2x3 xf = mul(translation(Vec2{60.3f, 40.6f}), rotationDeg(13.0f));
    for (const Case& c : cases) {
        INFO("width ", c.width);
        TestPath p;
        p.move(0, 100);
        p.line(30, 10);
        p.line(60, 100);
        p.line(90, 40);

        std::vector<uint8_t> buf(static_cast<size_t>(kSize) * kSize * 4, 0);
        sw::SwRenderer renderer;
        renderer.setTarget(sw::SwTarget{buf.data(), kSize, kSize, kSize * 4});
        renderer.clear(Color{0, 0, 0, 0});
        CommandList cl;
        cl.viewport = Rect{0, 0, static_cast<float>(kSize), static_cast<float>(kSize)};
        cl.devicePixelRatio = 1;
        cl.verbs = p.verbs;
        cl.points = p.points;
        StrokeCmd s{};
        s.xf = xf;
        s.path = PathRef{0, static_cast<uint32_t>(p.verbs.size()), 0, static_cast<uint32_t>(p.points.size())};
        s.paint.kind = PaintKind::Solid;
        s.paint.color = Color{1, 1, 1, 1};
        s.width = c.width;
        s.cap = c.cap;
        s.join = c.join;
        s.miterLimit = 10.0f;
        cl.strokes.push_back(s);
        Command cmd{};
        cmd.kind = CmdKind::StrokePath;
        cmd.index = 0;
        cl.commands.push_back(cmd);
        renderer.render(cl, nullptr, nullptr);

        std::vector<Vec2> flat;
        std::vector<uint32_t> starts;
        std::vector<uint8_t> closed;
        flattenPath(p.verbs, p.points, xf, kFlattenTolerance, flat, starts, closed);
        std::vector<Vec2> outline;
        std::vector<uint32_t> outlineStarts;
        const float scale = std::sqrt(std::fabs(determinant(xf)));
        strokeToFill(flat, starts, closed, c.width * scale, c.cap, c.join, s.miterLimit, kFlattenTolerance,
                     outline, outlineStarts);
        CoverageBitmap bm;
        REQUIRE(rasterizeCoverageContours(outline, outlineStarts, FillRule::NonZero, bm));
        REQUIRE(bm.originX >= 0);
        REQUIRE(bm.originY >= 0);
        REQUIRE(bm.originX + bm.width <= kSize);
        REQUIRE(bm.originY + bm.height <= kSize);
        int mismatches = 0;
        int covered = 0;
        for (int y = 0; y < kSize; ++y) {
            for (int x = 0; x < kSize; ++x) {
                const int bx = x - bm.originX;
                const int by = y - bm.originY;
                uint8_t cov = 0;
                if (bx >= 0 && by >= 0 && bx < bm.width && by < bm.height) {
                    cov = bm.pixels[static_cast<size_t>(by) * static_cast<size_t>(bm.width) +
                                    static_cast<size_t>(bx)];
                }
                mismatches +=
                    cov != buf[(static_cast<size_t>(y) * kSize + static_cast<size_t>(x)) * 4 + 3] ? 1 : 0;
                covered += cov != 0 ? 1 : 0;
            }
        }
        CHECK(mismatches == 0);
        CHECK(covered > 0);
    }

    // The same refusals as rasterizeCoverage.
    CoverageBitmap bm;
    const std::vector<Vec2> flatLine = {Vec2{10, 10}, Vec2{50, 10}};
    const std::vector<uint32_t> one = {0};
    CHECK_FALSE(rasterizeCoverageContours(flatLine, one, FillRule::NonZero, bm));
}
