#include "rounded_rect.h"

#include <algorithm>
#include <cmath>

namespace xt {

namespace {

void pushVerb(std::vector<PathVerb>& verbs, PathVerb v) { verbs.push_back(v); }

void pushPoint(std::vector<float>& points, Vec2 p) {
    points.push_back(p.x);
    points.push_back(p.y);
}

}  // namespace

void clampRoundedRectRadii(float w, float h, float& tl, float& tr, float& br, float& bl) {
    float half = std::min(w, h) * 0.5f;
    if (half < 0.0f) {
        half = 0.0f;
    }
    tl = std::clamp(tl, 0.0f, half);
    tr = std::clamp(tr, 0.0f, half);
    br = std::clamp(br, 0.0f, half);
    bl = std::clamp(bl, 0.0f, half);

    float scale = 1.0f;
    // Top edge: tl + tr <= w. Right edge: tr + br <= h. Bottom edge: bl + br <= w. Left edge:
    // tl + bl <= h.
    auto edgeScale = [](float sum, float length) {
        return (sum > length && sum > 0.0f) ? (length / sum) : 1.0f;
    };
    scale = std::min(scale, edgeScale(tl + tr, w));
    scale = std::min(scale, edgeScale(tr + br, h));
    scale = std::min(scale, edgeScale(bl + br, w));
    scale = std::min(scale, edgeScale(tl + bl, h));

    if (scale < 1.0f) {
        tl *= scale;
        tr *= scale;
        br *= scale;
        bl *= scale;
    }
}

void appendRoundedRectPath(Rect box, float rTL, float rTR, float rBR, float rBL, std::vector<PathVerb>& verbs,
                           std::vector<float>& points) {
    float x = box.x;
    float y = box.y;
    float w = box.w;
    float h = box.h;
    float k = kRoundedRectKappa;

    Vec2 start{x + rTL, y};             // Top edge start, just after the TL corner.
    Vec2 beforeTR{x + w - rTR, y};      // Top edge end, just before the TR corner.
    Vec2 afterTR{x + w, y + rTR};       // Right edge start, just after the TR corner.
    Vec2 beforeBR{x + w, y + h - rBR};  // Right edge end, just before the BR corner.
    Vec2 afterBR{x + w - rBR, y + h};   // Bottom edge start, just after the BR corner.
    Vec2 beforeBL{x + rBL, y + h};      // Bottom edge end, just before the BL corner.
    Vec2 afterBL{x, y + h - rBL};       // Left edge start, just after the BL corner.
    Vec2 beforeTL{x, y + rTL};          // Left edge end, just before the TL corner.

    pushVerb(verbs, PathVerb::Move);
    pushPoint(points, start);

    // TR corner.
    pushVerb(verbs, PathVerb::Line);
    pushPoint(points, beforeTR);
    if (rTR > 0.0f) {
        pushVerb(verbs, PathVerb::Cubic);
        pushPoint(points, Vec2{beforeTR.x + rTR * k, beforeTR.y});
        pushPoint(points, Vec2{afterTR.x, afterTR.y - rTR * k});
        pushPoint(points, afterTR);
    }

    // BR corner.
    pushVerb(verbs, PathVerb::Line);
    pushPoint(points, beforeBR);
    if (rBR > 0.0f) {
        pushVerb(verbs, PathVerb::Cubic);
        pushPoint(points, Vec2{beforeBR.x, beforeBR.y + rBR * k});
        pushPoint(points, Vec2{afterBR.x + rBR * k, afterBR.y});
        pushPoint(points, afterBR);
    }

    // BL corner.
    pushVerb(verbs, PathVerb::Line);
    pushPoint(points, beforeBL);
    if (rBL > 0.0f) {
        pushVerb(verbs, PathVerb::Cubic);
        pushPoint(points, Vec2{beforeBL.x - rBL * k, beforeBL.y});
        pushPoint(points, Vec2{afterBL.x, afterBL.y + rBL * k});
        pushPoint(points, afterBL);
    }

    // TL corner (closes back onto `start`).
    pushVerb(verbs, PathVerb::Line);
    pushPoint(points, beforeTL);
    if (rTL > 0.0f) {
        pushVerb(verbs, PathVerb::Cubic);
        pushPoint(points, Vec2{beforeTL.x, beforeTL.y - rTL * k});
        pushPoint(points, Vec2{start.x - rTL * k, start.y});
        pushPoint(points, start);
    }

    pushVerb(verbs, PathVerb::Close);
}

/// T-201 (moved from painter_element.cpp by T-680, which also needs it in xt::gpu):
/// appendRoundedRectPath()'s outline traced the other way round (counter-clockwise: TL
/// corner, left edge, BL, bottom, BR, right edge, TR, top edge), so a ring of an outer and this
/// inner outline fills with the nonzero rule and a side of zero width -- where the two outlines
/// coincide -- cancels exactly instead of leaving an antialiasing seam (even-odd adds the two
/// coincident edges' coverage).
void appendRoundedRectPathReversed(Rect box, float rTL, float rTR, float rBR, float rBL,
                                   std::vector<PathVerb>& verbs, std::vector<float>& points) {
    const float x = box.x;
    const float y = box.y;
    const float w = box.w;
    const float h = box.h;
    const float k = kRoundedRectKappa;
    auto point = [&points](float px, float py) {
        points.push_back(px);
        points.push_back(py);
    };
    auto line = [&](float px, float py) {
        verbs.push_back(PathVerb::Line);
        point(px, py);
    };
    auto cubic = [&](float x1, float y1, float x2, float y2, float x3, float y3) {
        verbs.push_back(PathVerb::Cubic);
        point(x1, y1);
        point(x2, y2);
        point(x3, y3);
    };
    verbs.push_back(PathVerb::Move);
    point(x + rTL, y);
    if (rTL > 0.0f) {
        cubic(x + rTL - rTL * k, y, x, y + rTL - rTL * k, x, y + rTL);
    }
    line(x, y + h - rBL);
    if (rBL > 0.0f) {
        cubic(x, y + h - rBL + rBL * k, x + rBL - rBL * k, y + h, x + rBL, y + h);
    }
    line(x + w - rBR, y + h);
    if (rBR > 0.0f) {
        cubic(x + w - rBR + rBR * k, y + h, x + w, y + h - rBR + rBR * k, x + w, y + h - rBR);
    }
    line(x + w, y + rTR);
    if (rTR > 0.0f) {
        cubic(x + w, y + rTR - rTR * k, x + w - rTR + rTR * k, y, x + w - rTR, y);
    }
    line(x + rTL, y);
    verbs.push_back(PathVerb::Close);
}

bool matchRoundedRectPath(std::span<const PathVerb> verbs, std::span<const float> points, Rect& rect,
                          float radii[4]) {
    // Move + 4 x (Line [+ Cubic]) + Close: 6..10 verbs, 5..17 points.
    constexpr size_t kMaxPoints = 17;
    if (verbs.size() < 6 || verbs.size() > 10 || points.size() % 2 != 0 || points.size() / 2 > kMaxPoints) {
        return false;
    }
    if (verbs.front() != PathVerb::Move || verbs.back() != PathVerb::Close) {
        return false;
    }
    const size_t pointCount = points.size() / 2;
    auto pt = [&points](size_t i) { return Vec2{points[2 * i], points[2 * i + 1]}; };
    // Per corner in the generator's order (TR, BR, BL, TL): the point its Line ends at, whether a
    // Cubic follows, and the Cubic's end point.
    Vec2 before[4];
    Vec2 after[4];
    bool rounded[4] = {false, false, false, false};
    size_t v = 1;
    size_t p = 1;
    for (int corner = 0; corner < 4; ++corner) {
        if (v >= verbs.size() || verbs[v] != PathVerb::Line || p >= pointCount) {
            return false;
        }
        before[corner] = pt(p);
        after[corner] = before[corner];
        ++v;
        ++p;
        if (v < verbs.size() && verbs[v] == PathVerb::Cubic) {
            if (p + 3 > pointCount) {
                return false;
            }
            rounded[corner] = true;
            after[corner] = pt(p + 2);
            ++v;
            p += 3;
        }
    }
    if (v + 1 != verbs.size() || p != pointCount) {
        return false;
    }
    for (const float value : points) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    // The box and the radii as the generator placed these points.
    const Vec2 start = pt(0);
    const float top = start.y;
    const float right = after[0].x;
    const float bottom = after[1].y;
    const float left = after[2].x;
    const float w = right - left;
    const float h = bottom - top;
    if (!(w > 0.0f) || !(h > 0.0f)) {
        return false;
    }
    const float rTR = rounded[0] ? right - before[0].x : 0.0f;
    const float rBR = rounded[1] ? bottom - before[1].y : 0.0f;
    const float rBL = rounded[2] ? before[2].x - left : 0.0f;
    const float rTL = rounded[3] ? start.x - left : 0.0f;
    const float tol = 1e-4f * std::max({1.0f, w, h, std::fabs(left), std::fabs(top)});
    const float cornerRadius[4] = {rTR, rBR, rBL, rTL};
    for (int corner = 0; corner < 4; ++corner) {
        if (rounded[corner] && !(cornerRadius[corner] > 0.0f)) {
            return false;
        }
    }
    if (rTL + rTR > w + tol || rBL + rBR > w + tol || rTR + rBR > h + tol || rTL + rBL > h + tol) {
        return false;
    }
    // The generator's output for that box, point by point (appendRoundedRectPath's order).
    const float k = kRoundedRectKappa;
    Vec2 expected[kMaxPoints];
    size_t n = 0;
    expected[n++] = Vec2{left + rTL, top};
    expected[n++] = Vec2{right - rTR, top};
    if (rounded[0]) {
        expected[n++] = Vec2{right - rTR + rTR * k, top};
        expected[n++] = Vec2{right, top + rTR - rTR * k};
        expected[n++] = Vec2{right, top + rTR};
    }
    expected[n++] = Vec2{right, bottom - rBR};
    if (rounded[1]) {
        expected[n++] = Vec2{right, bottom - rBR + rBR * k};
        expected[n++] = Vec2{right - rBR + rBR * k, bottom};
        expected[n++] = Vec2{right - rBR, bottom};
    }
    expected[n++] = Vec2{left + rBL, bottom};
    if (rounded[2]) {
        expected[n++] = Vec2{left + rBL - rBL * k, bottom};
        expected[n++] = Vec2{left, bottom - rBL + rBL * k};
        expected[n++] = Vec2{left, bottom - rBL};
    }
    expected[n++] = Vec2{left, top + rTL};
    if (rounded[3]) {
        expected[n++] = Vec2{left, top + rTL - rTL * k};
        expected[n++] = Vec2{left + rTL - rTL * k, top};
        expected[n++] = Vec2{left + rTL, top};
    }
    if (n != pointCount) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        const Vec2 q = pt(i);
        if (std::fabs(q.x - expected[i].x) > tol || std::fabs(q.y - expected[i].y) > tol) {
            return false;
        }
    }
    rect = Rect{left, top, w, h};
    radii[0] = rTL;
    radii[1] = rTR;
    radii[2] = rBR;
    radii[3] = rBL;
    return true;
}

}  // namespace xt
