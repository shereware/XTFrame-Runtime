#include <algorithm>
#include <cmath>

#include "xt/geom.h"

/// Stroke-to-fill: converts a flattened polyline stroke into closed NonZero fill contours.
/// See docs/design/10-backends.md §10.1

namespace xt {

namespace {
constexpr float kPi = 3.14159265358979323846f;
}  // namespace

float polygonSignedArea(std::span<const Vec2> pts) {
    if (pts.size() < 3) {
        return 0.0f;
    }
    float area = 0.0f;
    size_t n = pts.size();
    for (size_t i = 0; i < n; ++i) {
        const Vec2& a = pts[i];
        const Vec2& b = pts[(i + 1) % n];
        area += a.x * b.y - b.x * a.y;
    }
    return area * 0.5f;
}

float polylineLength(std::span<const Vec2> pts, bool closed) {
    if (pts.size() < 2) {
        return 0.0f;
    }
    float len = 0.0f;
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        Vec2 d = pts[i + 1] - pts[i];
        len += std::sqrt(d.x * d.x + d.y * d.y);
    }
    if (closed) {
        Vec2 d = pts[0] - pts[pts.size() - 1];
        len += std::sqrt(d.x * d.x + d.y * d.y);
    }
    return len;
}

namespace {

float lengthOf(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }

Vec2 normalize(Vec2 v) {
    float len = lengthOf(v);
    if (len < 1e-12f) {
        return Vec2{0, 0};
    }
    return Vec2{v.x / len, v.y / len};
}

// 90-degree rotation of a direction vector.
Vec2 perp(Vec2 d) { return Vec2{-d.y, d.x}; }

// Appends `pts` as one closed contour, flipping its winding to clockwise (positive
// polygonSignedArea) if needed. Degenerate (near-zero area / < 3 point) contours are still
// appended as-is (used for single-point round-cap dots handled elsewhere; here we simply keep the
// invariant "output contours are clockwise" for anything with a measurable area).
void appendClockwise(const std::vector<Vec2>& pts, std::vector<Vec2>& outPoints,
                     std::vector<uint32_t>& outContourStarts) {
    if (pts.empty()) {
        return;
    }
    outContourStarts.push_back(static_cast<uint32_t>(outPoints.size()));
    float area = polygonSignedArea(pts);
    if (area < 0.0f) {
        for (auto it = pts.rbegin(); it != pts.rend(); ++it) {
            outPoints.push_back(*it);
        }
    } else {
        for (const Vec2& p : pts) {
            outPoints.push_back(p);
        }
    }
}

// Number of segments to approximate an arc of `angle` radians at radius `r` within `tolerance`.
// See docs/tasks/T-09.md: segments = max(2, ceil(angle / acos(1 - tolerance/r))), 2 when r <= tolerance.
uint32_t arcSegments(float angle, float r, float tolerance) {
    if (angle <= 0.0f) {
        return 2u;
    }
    if (r <= tolerance) {
        return 2u;
    }
    float cosStep = 1.0f - tolerance / r;
    if (cosStep < -1.0f) {
        cosStep = -1.0f;
    }
    if (cosStep > 1.0f) {
        cosStep = 1.0f;
    }
    float anglePerSeg = std::acos(cosStep);
    if (anglePerSeg < 1e-6f) {
        return 64u;
    }
    float segF = std::ceil(angle / anglePerSeg);
    if (!(segF >= 2.0f)) {
        segF = 2.0f;
    }
    if (segF > 4096.0f) {
        segF = 4096.0f;
    }
    return static_cast<uint32_t>(segF);
}

// Appends points on the arc centered at `center` with radius `r`, from angle `a0` to `a1`
// (radians, either direction), inclusive of both ends, using `segs` segments.
void appendArc(Vec2 center, float r, float a0, float a1, uint32_t segs, std::vector<Vec2>& out) {
    for (uint32_t i = 0; i <= segs; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(segs);
        float a = a0 + (a1 - a0) * t;
        out.push_back(Vec2{center.x + r * std::cos(a), center.y + r * std::sin(a)});
    }
}

void appendQuad(Vec2 a, Vec2 b, float halfWidth, std::vector<Vec2>& outPoints,
                std::vector<uint32_t>& outContourStarts) {
    Vec2 d = normalize(b - a);
    if (d.x == 0.0f && d.y == 0.0f) {
        return;  // degenerate zero-length segment
    }
    Vec2 n = perp(d) * halfWidth;
    std::vector<Vec2> quad{a + n, b + n, b - n, a - n};
    appendClockwise(quad, outPoints, outContourStarts);
}

// Builds the join shape that fills the gap between segment (prev0 -> v) and segment (v -> next1)
// on the outer/convex side of the turn.
void appendJoin(Vec2 v, Vec2 d0, Vec2 d1, float halfWidth, LineJoin join, float miterLimit, float tolerance,
                std::vector<Vec2>& outPoints, std::vector<uint32_t>& outContourStarts) {
    float cross = d0.x * d1.y - d0.y * d1.x;
    float cosAlpha = d0.x * d1.x + d0.y * d1.y;
    cosAlpha = cosAlpha < -1.0f ? -1.0f : (cosAlpha > 1.0f ? 1.0f : cosAlpha);
    if (std::fabs(cross) < 1e-9f && cosAlpha > 0.0f) {
        return;  // straight continuation: quads already meet flush, nothing to fill
    }
    float side = (cross >= 0.0f) ? -1.0f : 1.0f;
    Vec2 n0dir = perp(d0) * side;
    Vec2 n1dir = perp(d1) * side;
    Vec2 a = v + n0dir * halfWidth;
    Vec2 b = v + n1dir * halfWidth;

    float alpha = std::acos(cosAlpha);                    // turn angle in [0, pi]
    float halfCos = std::sqrt((1.0f + cosAlpha) * 0.5f);  // cos(alpha/2), alpha in [0,pi] => >= 0

    LineJoin effective = join;
    if (join == LineJoin::Miter) {
        if (halfCos < 1e-6f || (1.0f / halfCos) > miterLimit) {
            effective = LineJoin::Bevel;
        }
    }

    std::vector<Vec2> poly;
    switch (effective) {
        case LineJoin::Miter: {
            Vec2 bis = n0dir + n1dir;
            bis = normalize(bis);
            float miterLen = halfWidth / halfCos;
            Vec2 m = v + bis * miterLen;
            poly = {v, a, m, b};
            break;
        }
        case LineJoin::Round: {
            uint32_t segs = arcSegments(alpha, halfWidth, tolerance);
            float a0 = std::atan2(n0dir.y, n0dir.x);
            float a1 = std::atan2(n1dir.y, n1dir.x);
            float delta = a1 - a0;
            while (delta > kPi) {
                delta -= 2.0f * kPi;
            }
            while (delta < -kPi) {
                delta += 2.0f * kPi;
            }
            poly.push_back(v);
            appendArc(v, halfWidth, a0, a0 + delta, segs, poly);
            break;
        }
        case LineJoin::Bevel:
        default:
            poly = {v, a, b};
            break;
    }
    appendClockwise(poly, outPoints, outContourStarts);
}

void appendCap(Vec2 p, Vec2 dOutward, float halfWidth, LineCap cap, float tolerance,
               std::vector<Vec2>& outPoints, std::vector<uint32_t>& outContourStarts) {
    if (cap == LineCap::Butt) {
        return;
    }
    Vec2 n = perp(dOutward) * halfWidth;
    if (cap == LineCap::Square) {
        std::vector<Vec2> poly{p + n, p + n + dOutward * halfWidth, p - n + dOutward * halfWidth, p - n};
        appendClockwise(poly, outPoints, outContourStarts);
        return;
    }
    // Round: half-disc swept from +n to -n through the outward direction. n = perp(dOutward) * hw,
    // so angle(n) = angle(dOutward) + pi/2; sweeping by -pi always passes through angle(dOutward).
    float a0 = std::atan2(n.y, n.x);
    float a1 = a0 - kPi;
    uint32_t segs = arcSegments(kPi, halfWidth, tolerance);
    std::vector<Vec2> poly;
    appendArc(p, halfWidth, a0, a1, segs, poly);
    appendClockwise(poly, outPoints, outContourStarts);
}

void appendPointCap(Vec2 p, float halfWidth, LineCap cap, float tolerance, std::vector<Vec2>& outPoints,
                    std::vector<uint32_t>& outContourStarts) {
    if (cap == LineCap::Butt) {
        return;
    }
    if (cap == LineCap::Square) {
        std::vector<Vec2> poly{p + Vec2{-halfWidth, -halfWidth}, p + Vec2{halfWidth, -halfWidth},
                               p + Vec2{halfWidth, halfWidth}, p + Vec2{-halfWidth, halfWidth}};
        appendClockwise(poly, outPoints, outContourStarts);
        return;
    }
    uint32_t segs = arcSegments(2.0f * kPi, halfWidth, tolerance);
    std::vector<Vec2> poly;
    appendArc(p, halfWidth, 0.0f, 2.0f * kPi, segs, poly);
    poly.pop_back();  // avoid duplicating the seam point
    appendClockwise(poly, outPoints, outContourStarts);
}

}  // namespace

float strokeOutset(float width, LineCap cap, LineJoin join, float miterLimit) {
    float k = 1.0f;
    if (join == LineJoin::Miter) {
        k = std::max(k, miterLimit);
    }
    if (cap == LineCap::Square) {
        k = std::max(k, std::sqrt(2.0f));
    }
    return 0.5f * width * k;
}

void strokeToFill(std::span<const Vec2> points, std::span<const uint32_t> contourStarts,
                  std::span<const uint8_t> closed, float width, LineCap cap, LineJoin join, float miterLimit,
                  float tolerance, std::vector<Vec2>& outPoints, std::vector<uint32_t>& outContourStarts) {
    if (width <= 0.0f) {
        return;
    }
    float halfWidth = width * 0.5f;
    size_t contourCount = contourStarts.size();
    for (size_t ci = 0; ci < contourCount; ++ci) {
        uint32_t start = contourStarts[ci];
        uint32_t end = (ci + 1 < contourCount) ? contourStarts[ci + 1] : static_cast<uint32_t>(points.size());
        if (end <= start) {
            continue;
        }
        size_t n = static_cast<size_t>(end - start);
        bool isClosed = closed[ci] != 0;
        std::span<const Vec2> contourPts = points.subspan(start, n);

        if (n == 1) {
            appendPointCap(contourPts[0], halfWidth, cap, tolerance, outPoints, outContourStarts);
            continue;
        }

        size_t segCount = isClosed ? n : (n - 1);
        // Segment quads.
        for (size_t i = 0; i < segCount; ++i) {
            Vec2 a = contourPts[i];
            Vec2 b = contourPts[(i + 1) % n];
            appendQuad(a, b, halfWidth, outPoints, outContourStarts);
        }
        // Interior joins.
        size_t joinCount = isClosed ? n : (n - 2);
        for (size_t i = 0; i < joinCount; ++i) {
            size_t vi = isClosed ? i : (i + 1);
            Vec2 prev = contourPts[(vi + n - 1) % n];
            Vec2 v = contourPts[vi];
            Vec2 next = contourPts[(vi + 1) % n];
            Vec2 d0 = normalize(v - prev);
            Vec2 d1 = normalize(next - v);
            if ((d0.x == 0.0f && d0.y == 0.0f) || (d1.x == 0.0f && d1.y == 0.0f)) {
                continue;
            }
            appendJoin(v, d0, d1, halfWidth, join, miterLimit, tolerance, outPoints, outContourStarts);
        }
        if (!isClosed) {
            Vec2 d0 = normalize(contourPts[1] - contourPts[0]);
            if (!(d0.x == 0.0f && d0.y == 0.0f)) {
                appendCap(contourPts[0], -d0, halfWidth, cap, tolerance, outPoints, outContourStarts);
            }
            Vec2 dEnd = normalize(contourPts[n - 1] - contourPts[n - 2]);
            if (!(dEnd.x == 0.0f && dEnd.y == 0.0f)) {
                appendCap(contourPts[n - 1], dEnd, halfWidth, cap, tolerance, outPoints, outContourStarts);
            }
        }
    }
}

namespace {

/// T-233: minimum width samples per contour (uniform in arc length), so that a width function with
/// kinks between two input vertices (a taper end, a profile point) is still followed.
constexpr float kWidthSamplesPerContour = 64.0f;
/// T-233: maximum recursive halvings of a sample step whose midpoint width is not linear enough.
constexpr int kWidthRefineDepth = 6;

/// T-233: one resampled point of a variable-width contour; `h` is the half width there.
struct WidthSample {
    Vec2 p;
    float u = 0;
    float h = 0;
};

float halfWidthAt(const WidthFn& width, float u) {
    float w = width(u);
    return w > 0.0f ? w * 0.5f : 0.0f;
}

/// Appends the samples strictly after `a` up to and including `b`, halving the step while the
/// half width at its midpoint is off the linear interpolation by more than `tolerance`.
void refineWidth(const WidthSample& a, const WidthSample& b, const WidthFn& width, float tolerance, int depth,
                 std::vector<WidthSample>& out) {
    if (depth > 0) {
        float um = (a.u + b.u) * 0.5f;
        float hm = halfWidthAt(width, um);
        if (std::fabs(hm - (a.h + b.h) * 0.5f) > tolerance) {
            WidthSample m{Vec2{(a.p.x + b.p.x) * 0.5f, (a.p.y + b.p.y) * 0.5f}, um, hm};
            refineWidth(a, m, width, tolerance, depth - 1, out);
            refineWidth(m, b, width, tolerance, depth - 1, out);
            return;
        }
    }
    out.push_back(b);
}

/// The quad (or, with one zero half width, triangle) between a and b, offset by +-ha at a and +-hb
/// at b along the a->b normal.
void appendTrapezoid(const WidthSample& a, const WidthSample& b, std::vector<Vec2>& outPoints,
                     std::vector<uint32_t>& outContourStarts) {
    if (a.h <= 0.0f && b.h <= 0.0f) {
        return;
    }
    Vec2 d = normalize(b.p - a.p);
    if (d.x == 0.0f && d.y == 0.0f) {
        return;
    }
    Vec2 n = perp(d);
    std::vector<Vec2> poly;
    if (a.h <= 0.0f) {
        poly = {a.p, b.p + n * b.h, b.p - n * b.h};
    } else if (b.h <= 0.0f) {
        poly = {a.p + n * a.h, b.p, a.p - n * a.h};
    } else {
        poly = {a.p + n * a.h, b.p + n * b.h, b.p - n * b.h, a.p - n * a.h};
    }
    appendClockwise(poly, outPoints, outContourStarts);
}

}  // namespace

void strokeToFill(std::span<const Vec2> points, std::span<const uint32_t> contourStarts,
                  std::span<const uint8_t> closed, WidthFn width, LineCap cap, LineJoin join,
                  float miterLimit, float tolerance, std::vector<Vec2>& outPoints,
                  std::vector<uint32_t>& outContourStarts) {
    std::vector<float> uAt;
    std::vector<float> segStartH;
    std::vector<float> segEndH;
    std::vector<WidthSample> samples;
    size_t contourCount = contourStarts.size();
    for (size_t ci = 0; ci < contourCount; ++ci) {
        uint32_t start = contourStarts[ci];
        uint32_t end = (ci + 1 < contourCount) ? contourStarts[ci + 1] : static_cast<uint32_t>(points.size());
        if (end <= start) {
            continue;
        }
        size_t n = static_cast<size_t>(end - start);
        bool isClosed = closed[ci] != 0;
        std::span<const Vec2> contourPts = points.subspan(start, n);
        if (n == 1) {
            float h = halfWidthAt(width, 0.0f);
            if (h > 0.0f) {
                appendPointCap(contourPts[0], h, cap, tolerance, outPoints, outContourStarts);
            }
            continue;
        }

        size_t segCount = isClosed ? n : (n - 1);
        // Arc-length fraction of each segment start; the last segment ends at exactly 1.
        uAt.assign(segCount + 1, 0.0f);
        float total = 0.0f;
        for (size_t i = 0; i < segCount; ++i) {
            uAt[i] = total;
            total += lengthOf(contourPts[(i + 1) % n] - contourPts[i]);
        }
        if (!(total > 1e-12f)) {
            float h = halfWidthAt(width, 0.0f);  // every point coincides: draw it as a point
            if (h > 0.0f) {
                appendPointCap(contourPts[0], h, cap, tolerance, outPoints, outContourStarts);
            }
            continue;
        }
        for (size_t i = 0; i < segCount; ++i) {
            uAt[i] /= total;
        }
        uAt[segCount] = 1.0f;

        // Segment trapezoids, resampled along each segment.
        segStartH.assign(segCount, 0.0f);
        segEndH.assign(segCount, 0.0f);
        for (size_t i = 0; i < segCount; ++i) {
            Vec2 a = contourPts[i];
            Vec2 b = contourPts[(i + 1) % n];
            float ua = uAt[i];
            float ub = uAt[i + 1];
            WidthSample first{a, ua, halfWidthAt(width, ua)};
            segStartH[i] = first.h;
            float steps = std::ceil((ub - ua) * kWidthSamplesPerContour);
            uint32_t k = steps >= 1.0f ? static_cast<uint32_t>(steps) : 1u;
            samples.clear();
            samples.push_back(first);
            for (uint32_t j = 1; j <= k; ++j) {
                float f = static_cast<float>(j) / static_cast<float>(k);
                float u = j == k ? ub : ua + (ub - ua) * f;
                Vec2 p = j == k ? b : Vec2{a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f};
                WidthSample next{p, u, halfWidthAt(width, u)};
                WidthSample prev = samples.back();
                refineWidth(prev, next, width, tolerance, kWidthRefineDepth, samples);
            }
            segEndH[i] = samples.back().h;
            for (size_t s = 0; s + 1 < samples.size(); ++s) {
                appendTrapezoid(samples[s], samples[s + 1], outPoints, outContourStarts);
            }
        }
        // Joins at the input vertices, sized by the larger adjacent segment width.
        size_t joinCount = isClosed ? n : (n - 2);
        for (size_t i = 0; i < joinCount; ++i) {
            size_t vi = isClosed ? i : (i + 1);
            size_t prevSeg = (vi + n - 1) % n;
            Vec2 prev = contourPts[(vi + n - 1) % n];
            Vec2 v = contourPts[vi];
            Vec2 next = contourPts[(vi + 1) % n];
            Vec2 d0 = normalize(v - prev);
            Vec2 d1 = normalize(next - v);
            if ((d0.x == 0.0f && d0.y == 0.0f) || (d1.x == 0.0f && d1.y == 0.0f)) {
                continue;
            }
            float h = std::max(segEndH[prevSeg], segStartH[vi]);
            if (h > 0.0f) {
                appendJoin(v, d0, d1, h, join, miterLimit, tolerance, outPoints, outContourStarts);
            }
        }
        if (!isClosed) {
            Vec2 d0 = normalize(contourPts[1] - contourPts[0]);
            float h0 = halfWidthAt(width, 0.0f);
            if (!(d0.x == 0.0f && d0.y == 0.0f) && h0 > 0.0f) {
                appendCap(contourPts[0], -d0, h0, cap, tolerance, outPoints, outContourStarts);
            }
            Vec2 dEnd = normalize(contourPts[n - 1] - contourPts[n - 2]);
            float h1 = halfWidthAt(width, 1.0f);
            if (!(dEnd.x == 0.0f && dEnd.y == 0.0f) && h1 > 0.0f) {
                appendCap(contourPts[n - 1], dEnd, h1, cap, tolerance, outPoints, outContourStarts);
            }
        }
    }
}

}  // namespace xt
