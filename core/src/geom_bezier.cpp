#include <algorithm>
#include <cmath>

#include "xt/geom.h"

/// Public bezier helpers (xt/geom.h's `xt::geom`, T-218): de Casteljau split/sub-curve, the
/// 16-segment arc-length table (moved unchanged from path_trim.cpp), exact path bounds and a
/// fill-rule point test. See docs/design/31-vector-authoring.md §31.4.1.

namespace xt::geom {

namespace {

Vec2 lerp(Vec2 a, Vec2 b, float t) { return a + (b - a) * t; }

Vec2 cubicAt(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, float t) {
    float mt = 1.0f - t;
    float a = mt * mt * mt;
    float b = 3.0f * mt * mt * t;
    float c = 3.0f * mt * t * t;
    float d = t * t * t;
    return Vec2{a * p0.x + b * p1.x + c * p2.x + d * p3.x, a * p0.y + b * p1.y + c * p2.y + d * p3.y};
}

/// Roots in (0, 1) of the derivative of one cubic coordinate (q0..q3); appends them to `out`.
void cubicExtrema(float q0, float q1, float q2, float q3, float out[2], int& count) {
    // B'(t)/3 = a t^2 + b t + c with a = -q0 + 3q1 - 3q2 + q3, b = 2(q0 - 2q1 + q2), c = q1 - q0.
    const float a = -q0 + 3.0f * q1 - 3.0f * q2 + q3;
    const float b = 2.0f * (q0 - 2.0f * q1 + q2);
    const float c = q1 - q0;
    count = 0;
    auto push = [&](float t) {
        if (t > 0.0f && t < 1.0f) {
            out[count++] = t;
        }
    };
    if (std::fabs(a) < 1e-12f) {
        if (std::fabs(b) > 1e-12f) {
            push(-c / b);
        }
        return;
    }
    const float disc = b * b - 4.0f * a * c;
    if (disc < 0.0f) {
        return;
    }
    const float s = std::sqrt(disc);
    push((-b + s) / (2.0f * a));
    push((-b - s) / (2.0f * a));
}

}  // namespace

void splitCubic(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, float t, Vec2 left[4], Vec2 right[4]) {
    Vec2 a = lerp(p0, p1, t);
    Vec2 b = lerp(p1, p2, t);
    Vec2 c = lerp(p2, p3, t);
    Vec2 d = lerp(a, b, t);
    Vec2 e = lerp(b, c, t);
    Vec2 f = lerp(d, e, t);
    left[0] = p0;
    left[1] = a;
    left[2] = d;
    left[3] = f;
    right[0] = f;
    right[1] = e;
    right[2] = c;
    right[3] = p3;
}

void subCubic(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, float tA, float tB, Vec2 out[4]) {
    Vec2 leftA[4];
    Vec2 rightA[4];
    splitCubic(p0, p1, p2, p3, tB, leftA, rightA);
    // leftA is the curve for [0, tB]; now take its sub-range [tA/tB, 1].
    float u = tB > 1e-9f ? (tA / tB) : 0.0f;
    if (u < 0.0f) {
        u = 0.0f;
    }
    if (u > 1.0f) {
        u = 1.0f;
    }
    Vec2 leftB[4];
    Vec2 rightB[4];
    splitCubic(leftA[0], leftA[1], leftA[2], leftA[3], u, leftB, rightB);
    out[0] = rightB[0];
    out[1] = rightB[1];
    out[2] = rightB[2];
    out[3] = rightB[3];
}

CubicTable buildCubicTable(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3) {
    CubicTable table;
    table.cum[0] = 0.0f;
    table.pts[0] = p0;
    for (int i = 1; i <= CubicTable::kSteps; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(CubicTable::kSteps);
        float mt = 1.0f - t;
        float a = mt * mt * mt;
        float b = 3.0f * mt * mt * t;
        float c = 3.0f * mt * t * t;
        float d = t * t * t;
        Vec2 p{a * p0.x + b * p1.x + c * p2.x + d * p3.x, a * p0.y + b * p1.y + c * p2.y + d * p3.y};
        table.pts[i] = p;
        Vec2 diff = p - table.pts[i - 1];
        table.cum[i] = table.cum[i - 1] + std::sqrt(diff.x * diff.x + diff.y * diff.y);
    }
    table.length = table.cum[CubicTable::kSteps];
    return table;
}

float lengthFractionToT(const CubicTable& table, float lengthFraction) {
    if (lengthFraction <= 0.0f) {
        return 0.0f;
    }
    if (lengthFraction >= 1.0f) {
        return 1.0f;
    }
    float target = lengthFraction * table.length;
    int i = 0;
    while (i < CubicTable::kSteps && table.cum[i + 1] < target) {
        ++i;
    }
    float segLen = table.cum[i + 1] - table.cum[i];
    float localFrac = segLen > 1e-12f ? (target - table.cum[i]) / segLen : 0.0f;
    return (static_cast<float>(i) + localFrac) / static_cast<float>(CubicTable::kSteps);
}

Rect pathBounds(std::span<const PathVerb> verbs, std::span<const float> points) {
    bool any = false;
    float minX = 0, minY = 0, maxX = 0, maxY = 0;
    auto add = [&](Vec2 p) {
        if (!any) {
            minX = maxX = p.x;
            minY = maxY = p.y;
            any = true;
            return;
        }
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    };
    size_t pi = 0;
    Vec2 cur{0, 0};
    auto pt = [&](size_t i) { return Vec2{points[i], points[i + 1]}; };
    for (PathVerb v : verbs) {
        switch (v) {
            case PathVerb::Move:
            case PathVerb::Line:
                if (pi + 2 > points.size()) {
                    return any ? Rect{minX, minY, maxX - minX, maxY - minY} : Rect{};
                }
                cur = pt(pi);
                add(cur);
                pi += 2;
                break;
            case PathVerb::Cubic: {
                if (pi + 6 > points.size()) {
                    return any ? Rect{minX, minY, maxX - minX, maxY - minY} : Rect{};
                }
                const Vec2 p1 = pt(pi), p2 = pt(pi + 2), p3 = pt(pi + 4);
                add(cur);
                add(p3);
                float ts[2];
                int n = 0;
                cubicExtrema(cur.x, p1.x, p2.x, p3.x, ts, n);
                for (int i = 0; i < n; ++i) {
                    add(cubicAt(cur, p1, p2, p3, ts[i]));
                }
                cubicExtrema(cur.y, p1.y, p2.y, p3.y, ts, n);
                for (int i = 0; i < n; ++i) {
                    add(cubicAt(cur, p1, p2, p3, ts[i]));
                }
                cur = p3;
                pi += 6;
                break;
            }
            case PathVerb::Close:
                break;
        }
    }
    return any ? Rect{minX, minY, maxX - minX, maxY - minY} : Rect{};
}

bool pathContains(std::span<const PathVerb> verbs, std::span<const float> points, Vec2 p, FillRule rule) {
    std::vector<Vec2> pts;
    std::vector<uint32_t> starts;
    std::vector<uint8_t> closed;
    flattenPath(verbs, points, Mat2x3{}, kFlattenTolerance, pts, starts, closed);
    int winding = 0;
    for (size_t c = 0; c < starts.size(); ++c) {
        const size_t begin = starts[c];
        const size_t end = c + 1 < starts.size() ? starts[c + 1] : pts.size();
        if (end - begin < 2) {
            continue;
        }
        for (size_t i = begin; i < end; ++i) {
            const Vec2 a = pts[i];
            const Vec2 b = pts[i + 1 < end ? i + 1 : begin];  // Every contour closes for filling.
            if (a.y <= p.y) {
                if (b.y > p.y && (b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y) > 0.0f) {
                    ++winding;
                }
            } else if (b.y <= p.y && (b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y) < 0.0f) {
                --winding;
            }
        }
    }
    return rule == FillRule::EvenOdd ? (winding & 1) != 0 : winding != 0;
}

}  // namespace xt::geom
