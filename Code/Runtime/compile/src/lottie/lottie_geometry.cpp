#include "lottie_geometry.h"

#include <algorithm>
#include <cmath>

#include "shape_modifiers.h"  // Code/Runtime/core/src: kMaxGeneratedPoints (T-258b).
#include "xt/num_guard.h"
#include "xt/timeline.h"

namespace xt::compile {

namespace {

constexpr float kPi = 3.14159265358979323846f;

void pushVerb(GeneratedPath& gp, PathVerb v) { gp.verbs.push_back(static_cast<uint8_t>(v)); }

void pushPoint(GeneratedPath& gp, Vec2 p) {
    gp.points.push_back(p.x);
    gp.points.push_back(p.y);
}

/// Reverses the winding of a Move+Line/Cubic*+Close path in place: keeps the first point as the
/// start, reverses the order of the remaining segments, and swaps each Cubic's control points.
/// Operates on the already-generated verb/point stream (used for `direction == 3`).
GeneratedPath reverseWinding(const GeneratedPath& src) {
    // Decompose into a vertex list plus, for each edge, an optional pair of control points.
    struct Edge {
        bool isCubic = false;
        Vec2 c1{}, c2{};
    };
    std::vector<Vec2> verts;
    std::vector<Edge> edges;
    bool closed = false;
    size_t pi = 0;
    for (uint8_t vb : src.verbs) {
        PathVerb verb = static_cast<PathVerb>(vb);
        if (verb == PathVerb::Move) {
            verts.push_back(Vec2{src.points[pi], src.points[pi + 1]});
            pi += 2;
        } else if (verb == PathVerb::Line) {
            verts.push_back(Vec2{src.points[pi], src.points[pi + 1]});
            pi += 2;
            edges.push_back(Edge{});
        } else if (verb == PathVerb::Cubic) {
            Edge e;
            e.isCubic = true;
            e.c1 = Vec2{src.points[pi], src.points[pi + 1]};
            e.c2 = Vec2{src.points[pi + 2], src.points[pi + 3]};
            verts.push_back(Vec2{src.points[pi + 4], src.points[pi + 5]});
            pi += 6;
            edges.push_back(e);
        } else {  // Close
            closed = true;
        }
    }
    GeneratedPath out;
    if (verts.empty()) {
        return out;
    }
    pushVerb(out, PathVerb::Move);
    pushPoint(out, verts[0]);
    // Reversed traversal: from vert[0] back to vert[n-1], vert[n-2], ... using edges in reverse,
    // with each cubic's control points swapped.
    for (size_t k = 0; k < edges.size(); ++k) {
        size_t edgeIdx = edges.size() - 1 - k;
        Vec2 to = verts[edgeIdx];
        if (edges[edgeIdx].isCubic) {
            pushVerb(out, PathVerb::Cubic);
            pushPoint(out, edges[edgeIdx].c2);
            pushPoint(out, edges[edgeIdx].c1);
            pushPoint(out, to);
        } else {
            pushVerb(out, PathVerb::Line);
            pushPoint(out, to);
        }
    }
    if (closed) {
        pushVerb(out, PathVerb::Close);
    }
    return out;
}

}  // namespace

float clampRectRadius(Vec2 size, float radius) {
    float half = std::min(std::abs(size.x), std::abs(size.y)) * 0.5f;
    return std::clamp(radius, 0.0f, half);
}

GeneratedPath buildRectPath(Vec2 center, Vec2 size, float radius, int direction, bool forceRounded) {
    float hw = size.x * 0.5f;
    float hh = size.y * 0.5f;
    float r = clampRectRadius(size, radius);
    GeneratedPath gp;
    if (r <= 0.0f && !forceRounded) {
        Vec2 tr{center.x + hw, center.y - hh};
        Vec2 br{center.x + hw, center.y + hh};
        Vec2 bl{center.x - hw, center.y + hh};
        Vec2 tl{center.x - hw, center.y - hh};
        pushVerb(gp, PathVerb::Move);
        pushPoint(gp, tr);
        pushVerb(gp, PathVerb::Line);
        pushPoint(gp, br);
        pushVerb(gp, PathVerb::Line);
        pushPoint(gp, bl);
        pushVerb(gp, PathVerb::Line);
        pushPoint(gp, tl);
        pushVerb(gp, PathVerb::Close);
    } else {
        float k = r * kBezierKappa;
        Vec2 a{center.x - hw + r, center.y - hh};   // Start: top edge, after TL corner.
        Vec2 b{center.x + hw - r, center.y - hh};   // Before TR corner.
        Vec2 d{center.x + hw, center.y - hh + r};   // After TR corner.
        Vec2 e{center.x + hw, center.y + hh - r};   // Before BR corner.
        Vec2 g{center.x + hw - r, center.y + hh};   // After BR corner.
        Vec2 h{center.x - hw + r, center.y + hh};   // Before BL corner.
        Vec2 j{center.x - hw, center.y + hh - r};   // After BL corner.
        Vec2 kk{center.x - hw, center.y - hh + r};  // Before TL corner.
        pushVerb(gp, PathVerb::Move);
        pushPoint(gp, a);
        pushVerb(gp, PathVerb::Line);
        pushPoint(gp, b);
        pushVerb(gp, PathVerb::Cubic);
        pushPoint(gp, Vec2{b.x + k, b.y});
        pushPoint(gp, Vec2{d.x, d.y - k});
        pushPoint(gp, d);
        pushVerb(gp, PathVerb::Line);
        pushPoint(gp, e);
        pushVerb(gp, PathVerb::Cubic);
        pushPoint(gp, Vec2{e.x, e.y + k});
        pushPoint(gp, Vec2{g.x + k, g.y});
        pushPoint(gp, g);
        pushVerb(gp, PathVerb::Line);
        pushPoint(gp, h);
        pushVerb(gp, PathVerb::Cubic);
        pushPoint(gp, Vec2{h.x - k, h.y});
        pushPoint(gp, Vec2{j.x, j.y + k});
        pushPoint(gp, j);
        pushVerb(gp, PathVerb::Line);
        pushPoint(gp, kk);
        pushVerb(gp, PathVerb::Cubic);
        pushPoint(gp, Vec2{kk.x, kk.y - k});
        pushPoint(gp, Vec2{a.x - k, a.y});
        pushPoint(gp, a);
        pushVerb(gp, PathVerb::Close);
    }
    if (direction == 3) {
        return reverseWinding(gp);
    }
    return gp;
}

GeneratedPath buildEllipsePath(Vec2 center, Vec2 size, int direction) {
    float rx = size.x * 0.5f;
    float ry = size.y * 0.5f;
    float kx = rx * kBezierKappa;
    float ky = ry * kBezierKappa;
    Vec2 top{center.x, center.y - ry};
    Vec2 right{center.x + rx, center.y};
    Vec2 bottom{center.x, center.y + ry};
    Vec2 left{center.x - rx, center.y};
    GeneratedPath gp;
    pushVerb(gp, PathVerb::Move);
    pushPoint(gp, top);
    pushVerb(gp, PathVerb::Cubic);
    pushPoint(gp, Vec2{top.x + kx, top.y});
    pushPoint(gp, Vec2{right.x, right.y - ky});
    pushPoint(gp, right);
    pushVerb(gp, PathVerb::Cubic);
    pushPoint(gp, Vec2{right.x, right.y + ky});
    pushPoint(gp, Vec2{bottom.x + kx, bottom.y});
    pushPoint(gp, bottom);
    pushVerb(gp, PathVerb::Cubic);
    pushPoint(gp, Vec2{bottom.x - kx, bottom.y});
    pushPoint(gp, Vec2{left.x, left.y + ky});
    pushPoint(gp, left);
    pushVerb(gp, PathVerb::Cubic);
    pushPoint(gp, Vec2{left.x, left.y - ky});
    pushPoint(gp, Vec2{top.x - kx, top.y});
    pushPoint(gp, top);
    pushVerb(gp, PathVerb::Close);
    if (direction == 3) {
        return reverseWinding(gp);
    }
    return gp;
}

GeneratedPath buildStarPath(float numPointsF, float innerRadius, float outerRadius, float innerRoundness,
                            float outerRoundness, float rotationDeg, Vec2 center, int starType,
                            int direction) {
    // T-258b (D3): bounded before it sizes anything (the same limit as the run time's star).
    constexpr float kMaxPoints = static_cast<float>(modifiers::kMaxGeneratedPoints);
    const float pointsF = numPointsF > kMaxPoints ? kMaxPoints : xt::finiteOr(numPointsF, 3.0f);
    int numPoints = std::max(3, static_cast<int>(std::lround(std::max(pointsF, 0.0f))));
    bool isStar = starType != 2;
    int vertexCount = isStar ? numPoints * 2 : numPoints;
    float angleStep = kPi / static_cast<float>(numPoints);  // Half of a full point-to-point angle.
    float startAngle = (rotationDeg - 90.0f) * kPi / 180.0f;

    std::vector<Vec2> verts;
    std::vector<float> tangentLens;
    verts.reserve(static_cast<size_t>(vertexCount));
    tangentLens.reserve(static_cast<size_t>(vertexCount));
    for (int i = 0; i < vertexCount; ++i) {
        bool outer = !isStar || (i % 2 == 0);
        float radius = outer ? outerRadius : innerRadius;
        float roundness = outer ? outerRoundness : innerRoundness;
        float angle = startAngle + static_cast<float>(i) * angleStep * (isStar ? 1.0f : 2.0f);
        verts.push_back(Vec2{center.x + radius * std::cos(angle), center.y + radius * std::sin(angle)});
        // Docs/tasks/T-15.md: tangent length = radius * roundness/100 * (2*pi/points) / 4.
        tangentLens.push_back(radius * (roundness / 100.0f) * (2.0f * kPi / static_cast<float>(numPoints)) /
                              4.0f);
    }

    GeneratedPath gp;
    pushVerb(gp, PathVerb::Move);
    pushPoint(gp, verts[0]);
    bool anyRound = false;
    for (float t : tangentLens) {
        if (t != 0.0f) {
            anyRound = true;
            break;
        }
    }
    size_t n = verts.size();
    if (!anyRound) {
        for (size_t i = 1; i < n; ++i) {
            pushVerb(gp, PathVerb::Line);
            pushPoint(gp, verts[i]);
        }
        pushVerb(gp, PathVerb::Close);
    } else {
        for (size_t i = 0; i < n; ++i) {
            size_t next = (i + 1) % n;
            // Tangential unit direction at vertex i (perpendicular to its radius vector).
            Vec2 dir0 = verts[i] - center;
            float len0 = std::sqrt(dir0.x * dir0.x + dir0.y * dir0.y);
            Vec2 tan0 = len0 > 0.0f ? Vec2{-dir0.y / len0, dir0.x / len0} : Vec2{0, 0};
            Vec2 dir1 = verts[next] - center;
            float len1 = std::sqrt(dir1.x * dir1.x + dir1.y * dir1.y);
            Vec2 tan1 = len1 > 0.0f ? Vec2{-dir1.y / len1, dir1.x / len1} : Vec2{0, 0};
            Vec2 c1 = verts[i] + tan0 * tangentLens[i];
            Vec2 c2 = verts[next] - tan1 * tangentLens[next];
            pushVerb(gp, PathVerb::Cubic);
            pushPoint(gp, c1);
            pushPoint(gp, c2);
            pushPoint(gp, verts[next]);
        }
        pushVerb(gp, PathVerb::Close);
    }
    if (direction == 3) {
        return reverseWinding(gp);
    }
    return gp;
}

GeneratedPath buildPathFromLottiePath(const LottiePath& path, int direction) {
    GeneratedPath gp;
    if (path.v.empty()) {
        return gp;
    }
    size_t n = path.v.size();
    pushVerb(gp, PathVerb::Move);
    pushPoint(gp, path.v[0]);
    size_t segCount = path.closed ? n : n - 1;
    for (size_t i = 0; i < segCount; ++i) {
        size_t next = (i + 1) % n;
        Vec2 c1 = path.v[i] + path.out[i];
        Vec2 c2 = path.v[next] + path.in[next];
        pushVerb(gp, PathVerb::Cubic);
        pushPoint(gp, c1);
        pushPoint(gp, c2);
        pushPoint(gp, path.v[next]);
    }
    if (path.closed) {
        pushVerb(gp, PathVerb::Close);
    }
    if (direction == 3) {
        return reverseWinding(gp);
    }
    return gp;
}

LottiePath applyRoundCornersToPath(const LottiePath& src, float radius) {
    LottiePath out;
    out.closed = src.closed;
    if (src.v.size() < 2) {
        return src;
    }
    float radiusClamped = std::max(radius, 0.0f);
    size_t n = src.v.size();
    out.v.reserve(n * 2);
    out.in.reserve(n * 2);
    out.out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        size_t prev = (i + n - 1) % n;
        size_t next = (i + 1) % n;
        Vec2 toPrev = src.v[prev] - src.v[i];
        Vec2 toNext = src.v[next] - src.v[i];
        float lenPrev = std::sqrt(toPrev.x * toPrev.x + toPrev.y * toPrev.y);
        float lenNext = std::sqrt(toNext.x * toNext.x + toNext.y * toNext.y);
        float cutPrev = std::min(radiusClamped, lenPrev * 0.5f);
        float cutNext = std::min(radiusClamped, lenNext * 0.5f);
        Vec2 dirPrev = lenPrev > 0.0f ? Vec2{toPrev.x / lenPrev, toPrev.y / lenPrev} : Vec2{0, 0};
        Vec2 dirNext = lenNext > 0.0f ? Vec2{toNext.x / lenNext, toNext.y / lenNext} : Vec2{0, 0};
        Vec2 pIn = src.v[i] + dirPrev * cutPrev;
        Vec2 pOut = src.v[i] + dirNext * cutNext;
        // Rounding tangent length (kappa-scaled) so the Cubic from pIn to pOut approximates an arc.
        float k1 = cutPrev * kBezierKappa;
        float k2 = cutNext * kBezierKappa;
        out.v.push_back(pIn);
        out.in.push_back(Vec2{0, 0});
        out.out.push_back(dirPrev * -k1);  // Tangent from pIn towards the (cut) corner.
        out.v.push_back(pOut);
        out.in.push_back(dirNext * -k2);  // Tangent into pOut, from the (cut) corner.
        out.out.push_back(Vec2{0, 0});
    }
    return out;
}

namespace {

/// Compile-time-only linear+cubic-bezier interpolation, mirroring
/// Docs/design/04-timeline.md §4.3 but operating on raw Lottie frame numbers instead of seconds/IR
/// easing (shared by float/Vec2 via lerp below).
template <class T>
T lerpGeom(const T& a, const T& b, float t);

template <>
float lerpGeom<float>(const float& a, const float& b, float t) {
    return a + (b - a) * t;
}

template <>
Vec2 lerpGeom<Vec2>(const Vec2& a, const Vec2& b, float t) {
    return Vec2{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

}  // namespace

template <class T>
T sampleAnimated(const AnimatedValue<T>& av, float frame) {
    if (!av.animated || av.keys.empty()) {
        return av.value;
    }
    if (av.keys.size() == 1 || frame <= av.keys.front().frame) {
        return av.keys.front().value;
    }
    if (frame >= av.keys.back().frame) {
        return av.keys.back().value;
    }
    size_t i = 0;
    for (; i + 1 < av.keys.size(); ++i) {
        if (frame < av.keys[i + 1].frame) {
            break;
        }
    }
    const LottieKeyframe<T>& k0 = av.keys[i];
    const LottieKeyframe<T>& k1 = av.keys[i + 1];
    float denom = k1.frame - k0.frame;
    float p = denom != 0.0f ? (frame - k0.frame) / denom : 0.0f;
    float e;
    if (k0.hold) {
        e = 0.0f;
    } else {
        Easing easing = Easing::bezier(k0.ox, k0.oy, k0.ix, k0.iy);
        e = evalEasing(easing, p);
    }
    return lerpGeom<T>(k0.value, k1.value, e);
}

template float sampleAnimated<float>(const AnimatedValue<float>&, float);
template Vec2 sampleAnimated<Vec2>(const AnimatedValue<Vec2>&, float);

}  // namespace xt::compile
