// T-718 (Docs/design/10-backends.md §10.16 W1, Docs/tasks/T-718.md item 1): matchRoundedRectPath
// recognizes exactly what appendRoundedRectPath writes and nothing else.

#include <cmath>
#include <span>
#include <vector>

#include "doctest.h"
#include "rounded_rect.h"  // Code/Runtime/core/src
#include "xt/node.h"
#include "xt/types.h"

using namespace xt;

namespace {

struct Path {
    std::vector<PathVerb> verbs;
    std::vector<float> points;
};

Path generated(Rect box, float tl, float tr, float br, float bl) {
    Path p;
    appendRoundedRectPath(box, tl, tr, br, bl, p.verbs, p.points);
    return p;
}

bool match(const Path& p, Rect& rect, float radii[4]) {
    return matchRoundedRectPath(p.verbs, p.points, rect, radii);
}

void checkRecognized(Rect box, float tl, float tr, float br, float bl) {
    CAPTURE(box.x);
    CAPTURE(box.w);
    CAPTURE(tl);
    CAPTURE(tr);
    CAPTURE(br);
    CAPTURE(bl);
    const Path p = generated(box, tl, tr, br, bl);
    Rect rect{};
    float radii[4] = {-1, -1, -1, -1};
    REQUIRE(match(p, rect, radii));
    const float tol = 1e-3f * std::max(1.0f, std::max(box.w, box.h));
    CHECK(std::fabs(rect.x - box.x) <= tol);
    CHECK(std::fabs(rect.y - box.y) <= tol);
    CHECK(std::fabs(rect.w - box.w) <= tol);
    CHECK(std::fabs(rect.h - box.h) <= tol);
    CHECK(std::fabs(radii[0] - tl) <= tol);
    CHECK(std::fabs(radii[1] - tr) <= tol);
    CHECK(std::fabs(radii[2] - br) <= tol);
    CHECK(std::fabs(radii[3] - bl) <= tol);
}

}  // namespace

TEST_CASE("matchRoundedRectPath: every output of appendRoundedRectPath is recognized (T-718)") {
    checkRecognized(Rect{0, 0, 100, 60}, 8, 8, 8, 8);
    checkRecognized(Rect{12.5f, -7.25f, 320, 180}, 4, 16, 24, 2);
    checkRecognized(Rect{3, 4, 50, 40}, 0, 10, 0, 10);  // some corners square
    checkRecognized(Rect{3, 4, 50, 40}, 10, 0, 0, 0);
    checkRecognized(Rect{3, 4, 50, 40}, 0, 0, 0, 7);
    checkRecognized(Rect{3, 4, 50, 40}, 0, 0, 0, 0);  // a plain rectangle, as the generator writes it
    checkRecognized(Rect{1000.5f, 700.25f, 64, 64}, 32, 32, 32, 32);  // a circle
    checkRecognized(Rect{0.125f, 0.5f, 0.75f, 0.5f}, 0.1f, 0.2f, 0.05f, 0.25f);
    // Radii clamped to half the shorter side (a pill) and scaled down by the overlap rule.
    float tl = 500, tr = 500, br = 500, bl = 500;
    clampRoundedRectRadii(120, 40, tl, tr, br, bl);
    checkRecognized(Rect{10, 10, 120, 40}, tl, tr, br, bl);
    tl = 90;
    tr = 60;
    br = 10;
    bl = 30;
    clampRoundedRectRadii(100, 80, tl, tr, br, bl);
    checkRecognized(Rect{-40, 20, 100, 80}, tl, tr, br, bl);
}

TEST_CASE("matchRoundedRectPath: other shapes are refused (T-718)") {
    Rect rect{};
    float radii[4] = {};
    SUBCASE("a polygon") {
        Path p;
        p.verbs = {PathVerb::Move, PathVerb::Line, PathVerb::Line,
                   PathVerb::Line, PathVerb::Line, PathVerb::Close};
        p.points = {10, 0, 90, 10, 100, 60, 0, 60, 0, 10};  // five points, not a rectangle
        CHECK(!match(p, rect, radii));
        Path tri;
        tri.verbs = {PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Close};
        tri.points = {0, 0, 10, 0, 5, 8};
        CHECK(!match(tri, rect, radii));
    }
    SUBCASE("elliptical corners (inset(... round a / b))") {
        // The generator's layout with rx = 20, ry = 10 at every corner.
        const float x = 0, y = 0, w = 100, h = 60, rx = 20, ry = 10, k = kRoundedRectKappa;
        Path p;
        p.verbs = {PathVerb::Move, PathVerb::Line,  PathVerb::Cubic, PathVerb::Line,  PathVerb::Cubic,
                   PathVerb::Line, PathVerb::Cubic, PathVerb::Line,  PathVerb::Cubic, PathVerb::Close};
        p.points = {x + rx,
                    y,
                    x + w - rx,
                    y,
                    x + w - rx + rx * k,
                    y,
                    x + w,
                    y + ry - ry * k,
                    x + w,
                    y + ry,
                    x + w,
                    y + h - ry,
                    x + w,
                    y + h - ry + ry * k,
                    x + w - rx + rx * k,
                    y + h,
                    x + w - rx,
                    y + h,
                    x + rx,
                    y + h,
                    x + rx - rx * k,
                    y + h,
                    x,
                    y + h - ry + ry * k,
                    x,
                    y + h - ry,
                    x,
                    y + ry,
                    x,
                    y + ry - ry * k,
                    x + rx - rx * k,
                    y,
                    x + rx,
                    y};
        CHECK(!match(p, rect, radii));
    }
    SUBCASE("an ellipse") {
        // Four cubics, no lines: not the generator's verb pattern.
        Path p;
        p.verbs = {PathVerb::Move,  PathVerb::Cubic, PathVerb::Cubic,
                   PathVerb::Cubic, PathVerb::Cubic, PathVerb::Close};
        p.points.assign(26, 1.0f);
        CHECK(!match(p, rect, radii));
    }
    SUBCASE("one control point moved") {
        const Path good = generated(Rect{0, 0, 100, 60}, 12, 12, 12, 12);
        REQUIRE(match(good, rect, radii));
        for (size_t i = 0; i < good.points.size(); ++i) {
            Path bad = good;
            bad.points[i] += 0.5f;
            CAPTURE(i);
            CHECK(!match(bad, rect, radii));
        }
    }
    SUBCASE("the reversed outline, a truncated path, two subpaths") {
        Path rev;
        appendRoundedRectPathReversed(Rect{0, 0, 100, 60}, 12, 12, 12, 12, rev.verbs, rev.points);
        CHECK(!match(rev, rect, radii));
        Path cut = generated(Rect{0, 0, 100, 60}, 12, 12, 12, 12);
        cut.verbs.pop_back();
        CHECK(!match(cut, rect, radii));
        Path two = generated(Rect{0, 0, 100, 60}, 12, 12, 12, 12);
        appendRoundedRectPath(Rect{0, 0, 100, 60}, 12, 12, 12, 12, two.verbs, two.points);
        CHECK(!match(two, rect, radii));
        CHECK(!matchRoundedRectPath({}, {}, rect, radii));
    }
    SUBCASE("an empty, inverted or non-finite box; overlapping radii") {
        CHECK(!match(generated(Rect{0, 0, 0, 60}, 0, 0, 0, 0), rect, radii));
        CHECK(!match(generated(Rect{0, 0, -100, 60}, 0, 0, 0, 0), rect, radii));
        Path nan = generated(Rect{0, 0, 100, 60}, 12, 12, 12, 12);
        nan.points[3] = std::nanf("");
        CHECK(!match(nan, rect, radii));
        // Unclamped radii whose sum exceeds the side: not a shape the SDF describes.
        CHECK(!match(generated(Rect{0, 0, 100, 60}, 70, 70, 10, 10), rect, radii));
    }
}
