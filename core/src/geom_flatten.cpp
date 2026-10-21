#include <cmath>

#include "xt/geom.h"

/// Path flattening: cubic beziers -> line segments.
/// See docs/design/10-backends.md §10.1

namespace xt {

namespace {

float clampf(float v, float lo, float hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

Vec2 evalCubic(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, float t) {
    float mt = 1.0f - t;
    float a = mt * mt * mt;
    float b = 3.0f * mt * mt * t;
    float c = 3.0f * mt * t * t;
    float d = t * t * t;
    return Vec2{a * p0.x + b * p1.x + c * p2.x + d * p3.x, a * p0.y + b * p1.y + c * p2.y + d * p3.y};
}

}  // namespace

uint32_t cubicSubdivisions(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, float tolerance) {
    if (tolerance <= 0.0f) {
        return 64u;
    }
    // Wang's formula: n = ceil(sqrt(0.75 * L / tolerance)) where L is the largest second
    // difference of the control polygon (|p0 - 2 p1 + p2|, |p1 - 2 p2 + p3|).
    const Vec2 d1{p0.x - 2.0f * p1.x + p2.x, p0.y - 2.0f * p1.y + p2.y};
    const Vec2 d2{p1.x - 2.0f * p2.x + p3.x, p1.y - 2.0f * p2.y + p3.y};
    float ll = d1.x * d1.x + d1.y * d1.y;
    const float ll2 = d2.x * d2.x + d2.y * d2.y;
    if (ll2 > ll) {
        ll = ll2;
    }
    if (!(ll > 0.0f)) {
        return 1u;
    }
    float value = std::sqrt(0.75f * std::sqrt(ll) / tolerance);
    float nF = std::ceil(value);
    if (!(nF >= 1.0f)) {  // handles NaN too
        nF = 1.0f;
    }
    nF = clampf(nF, 1.0f, 64.0f);
    return static_cast<uint32_t>(nF);
}

void flattenPath(std::span<const PathVerb> verbs, std::span<const float> points, const Mat2x3& xf,
                 float tolerance, std::vector<Vec2>& outPoints, std::vector<uint32_t>& outContourStarts,
                 std::vector<uint8_t>& outClosed) {
    size_t pointCursor = 0;
    bool contourStarted = false;
    Vec2 cur{0, 0};

    auto startImplicitContour = [&]() {
        Vec2 p0 = apply(xf, Vec2{0, 0});
        outContourStarts.push_back(static_cast<uint32_t>(outPoints.size()));
        outPoints.push_back(p0);
        contourStarted = true;
        cur = p0;
    };

    for (PathVerb verb : verbs) {
        switch (verb) {
            case PathVerb::Move: {
                XT_ASSERT(pointCursor + 2 <= points.size());
                float x = points[pointCursor];
                float y = points[pointCursor + 1];
                pointCursor += 2;
                Vec2 p = apply(xf, Vec2{x, y});
                if (contourStarted) {
                    outClosed.push_back(0);  // previous contour ends unclosed
                }
                outContourStarts.push_back(static_cast<uint32_t>(outPoints.size()));
                outPoints.push_back(p);
                contourStarted = true;
                cur = p;
                break;
            }
            case PathVerb::Line: {
                XT_ASSERT(contourStarted);
                if (!contourStarted) {
                    startImplicitContour();
                }
                XT_ASSERT(pointCursor + 2 <= points.size());
                float x = points[pointCursor];
                float y = points[pointCursor + 1];
                pointCursor += 2;
                Vec2 p = apply(xf, Vec2{x, y});
                outPoints.push_back(p);
                cur = p;
                break;
            }
            case PathVerb::Cubic: {
                XT_ASSERT(contourStarted);
                if (!contourStarted) {
                    startImplicitContour();
                }
                XT_ASSERT(pointCursor + 6 <= points.size());
                Vec2 p1 = apply(xf, Vec2{points[pointCursor], points[pointCursor + 1]});
                Vec2 p2 = apply(xf, Vec2{points[pointCursor + 2], points[pointCursor + 3]});
                Vec2 p3 = apply(xf, Vec2{points[pointCursor + 4], points[pointCursor + 5]});
                pointCursor += 6;
                uint32_t n = cubicSubdivisions(cur, p1, p2, p3, tolerance);
                for (uint32_t i = 1; i <= n; ++i) {
                    float t = static_cast<float>(i) / static_cast<float>(n);
                    outPoints.push_back(evalCubic(cur, p1, p2, p3, t));
                }
                cur = p3;
                break;
            }
            case PathVerb::Close: {
                if (contourStarted) {
                    outClosed.push_back(1);
                    contourStarted = false;
                }
                // Close without a preceding Move: not covered by the spec, treated as a no-op.
                break;
            }
        }
    }
    if (contourStarted) {
        outClosed.push_back(0);
    }
}

}  // namespace xt
