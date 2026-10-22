#include "xt/project3d.h"

#include <cmath>

#include "xt/assert.h"

/// project3d.h's implementation. See Docs/design/19-3d-space.md §19.6, §19.7.

namespace xt::project3d {

namespace {

/// One recursion step: projects `p` (already in `xf`'s local coordinates, i.e. what the caller
/// wants projected next) through the mapping levels `stack[0..count)`, innermost (stack[count-1])
/// first, folding outward. `ok` is cleared on any failed homogeneous divide.
Vec2 projectThrough(std::span<const Mapping> stack, Vec2 p, bool& ok) {
    if (stack.empty()) {
        return p;
    }
    const Mapping& top = stack.back();
    std::span<const Mapping> outer = stack.first(stack.size() - 1);
    if (!top.hasProjection) {
        return projectThrough(outer, p, ok);
    }
    Mat4 m = mat4Mul(top.viewProj, top.planeWorld3);
    float w = 0;
    Vec3 r = mat4Project(m, Vec3{p.x, p.y, 0}, &w);
    if (w <= 1e-6f) {
        ok = false;
        return Vec2{0, 0};
    }
    Vec2 rootPoint = apply(top.rootXf, Vec2{r.x, r.y});
    return projectThrough(outer, rootPoint, ok);
}

}  // namespace

Vec2 projectPoint(std::span<const Mapping> stack, const Mat2x3& xf, Vec2 p, bool& ok) {
    ok = true;
    Vec2 q = apply(xf, p);
    return projectThrough(stack, q, ok);
}

bool projectPolyline(std::span<const Mapping> stack, const Mat2x3& xf, std::span<const PathVerb> verbs,
                     std::span<const float> points, std::vector<PathVerb>& outVerbs,
                     std::vector<float>& outPoints) {
    outVerbs.clear();
    outPoints.clear();
    size_t pi = 0;
    for (PathVerb v : verbs) {
        switch (v) {
            case PathVerb::Move:
            case PathVerb::Line: {
                XT_ASSERT(pi + 2 <= points.size());
                Vec2 p{points[pi], points[pi + 1]};
                pi += 2;
                bool ok = false;
                Vec2 s = projectPoint(stack, xf, p, ok);
                if (!ok) {
                    outVerbs.clear();
                    outPoints.clear();
                    return false;
                }
                outVerbs.push_back(v);
                outPoints.push_back(s.x);
                outPoints.push_back(s.y);
                break;
            }
            case PathVerb::Close:
                outVerbs.push_back(v);
                break;
            case PathVerb::Cubic:
                // Callers must flatten (Move/Line/Close only) before projecting; see the header.
                XT_ASSERT(false && "projectPolyline: input must already be flattened");
                outVerbs.clear();
                outPoints.clear();
                return false;
        }
    }
    return true;
}

float projectedScaleMax(std::span<const Mapping> stack, const Mat2x3& xf, Rect bounds) {
    Vec2 corners[4] = {
        Vec2{bounds.x, bounds.y},
        Vec2{bounds.x + bounds.w, bounds.y},
        Vec2{bounds.x + bounds.w, bounds.y + bounds.h},
        Vec2{bounds.x, bounds.y + bounds.h},
    };
    Vec2 before[4];
    Vec2 after[4];
    bool ok[4];
    for (int i = 0; i < 4; ++i) {
        before[i] = apply(xf, corners[i]);
        after[i] = projectPoint(stack, xf, corners[i], ok[i]);
    }
    float sMax = 1.0f;
    for (int i = 0; i < 4; ++i) {
        int j = (i + 1) % 4;
        if (!ok[i] || !ok[j]) {
            continue;
        }
        float beforeLen = std::hypot(before[j].x - before[i].x, before[j].y - before[i].y);
        float afterLen = std::hypot(after[j].x - after[i].x, after[j].y - after[i].y);
        if (beforeLen > 1e-6f) {
            float ratio = afterLen / beforeLen;
            if (ratio > sMax) {
                sMax = ratio;
            }
        }
    }
    return sMax;
}

bool isBackface(std::span<const Mapping> stack, const Mat2x3& xf, Vec2 anchor) {
    bool okXm = false, okXp = false, okYm = false, okYp = false;
    Vec2 xm = projectPoint(stack, xf, Vec2{anchor.x - 1, anchor.y}, okXm);
    Vec2 xp = projectPoint(stack, xf, Vec2{anchor.x + 1, anchor.y}, okXp);
    Vec2 ym = projectPoint(stack, xf, Vec2{anchor.x, anchor.y - 1}, okYm);
    Vec2 yp = projectPoint(stack, xf, Vec2{anchor.x, anchor.y + 1}, okYp);
    if (!okXm || !okXp || !okYm || !okYp) {
        return false;  // Conservative: an unprojectable neighborhood is treated as front-facing.
    }
    Vec2 dx{(xp.x - xm.x) * 0.5f, (xp.y - xm.y) * 0.5f};
    Vec2 dy{(yp.x - ym.x) * 0.5f, (yp.y - ym.y) * 0.5f};
    float det = dx.x * dy.y - dx.y * dy.x;
    return det < 0.0f;
}

Mat2x3 jacobianAt(std::span<const Mapping> stack, const Mat2x3& xf, Vec2 anchor) {
    bool okC = false, okXm = false, okXp = false, okYm = false, okYp = false;
    Vec2 center = projectPoint(stack, xf, anchor, okC);
    Vec2 xm = projectPoint(stack, xf, Vec2{anchor.x - 0.5f, anchor.y}, okXm);
    Vec2 xp = projectPoint(stack, xf, Vec2{anchor.x + 0.5f, anchor.y}, okXp);
    Vec2 ym = projectPoint(stack, xf, Vec2{anchor.x, anchor.y - 0.5f}, okYm);
    Vec2 yp = projectPoint(stack, xf, Vec2{anchor.x, anchor.y + 0.5f}, okYp);
    if (!okC || !okXm || !okXp || !okYm || !okYp) {
        return Mat2x3{};  // identity: an unprojectable neighborhood has no well-defined Jacobian.
    }
    Mat2x3 out{};
    out.a = xp.x - xm.x;
    out.b = xp.y - xm.y;
    out.c = yp.x - ym.x;
    out.d = yp.y - ym.y;
    out.e = center.x;
    out.f = center.y;
    return out;
}

}  // namespace xt::project3d
