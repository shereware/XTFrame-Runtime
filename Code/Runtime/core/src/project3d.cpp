#include "xt/project3d.h"

#include <algorithm>
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

namespace {

/// Row-major 3x3 product a * b.
Mat3 mat3Mul(const Mat3& a, const Mat3& b) {
    Mat3 r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r.m[i * 3 + j] = a.m[i * 3 + 0] * b.m[0 * 3 + j] + a.m[i * 3 + 1] * b.m[1 * 3 + j] +
                             a.m[i * 3 + 2] * b.m[2 * 3 + j];
        }
    }
    return r;
}

/// The adjugate of `h` (its inverse up to a scale factor, which a homography does not care about);
/// false when `h` is singular.
bool mat3Adjugate(const Mat3& h, Mat3& out) {
    const float* m = h.m;
    out.m[0] = m[4] * m[8] - m[5] * m[7];
    out.m[1] = m[2] * m[7] - m[1] * m[8];
    out.m[2] = m[1] * m[5] - m[2] * m[4];
    out.m[3] = m[5] * m[6] - m[3] * m[8];
    out.m[4] = m[0] * m[8] - m[2] * m[6];
    out.m[5] = m[2] * m[3] - m[0] * m[5];
    out.m[6] = m[3] * m[7] - m[4] * m[6];
    out.m[7] = m[1] * m[6] - m[0] * m[7];
    out.m[8] = m[0] * m[4] - m[1] * m[3];
    const float det = m[0] * out.m[0] + m[1] * out.m[3] + m[2] * out.m[6];
    return std::isfinite(det) && std::fabs(det) > 1e-12f;
}

bool finiteMat3(const Mat3& h) {
    for (float v : h.m) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool planeHomography(std::span<const Mapping> stack, Mat3& out) {
    out = Mat3{};
    for (const Mapping& level : stack) {
        if (!level.hasProjection) {
            continue;
        }
        const Mat4 m = mat4Mul(level.viewProj, level.planeWorld3);
        // Rows x, y, w and columns x, y, 1 of the 4x4 (the plane's z = 0 drops column z).
        Mat3 plane;
        plane.m[0] = m.m[0];
        plane.m[1] = m.m[1];
        plane.m[2] = m.m[3];
        plane.m[3] = m.m[4];
        plane.m[4] = m.m[5];
        plane.m[5] = m.m[7];
        plane.m[6] = m.m[12];
        plane.m[7] = m.m[13];
        plane.m[8] = m.m[15];
        if (!(plane.m[8] > 1e-6f)) {
            return false;
        }
        const Mat2x3& r = level.rootXf;
        Mat3 root;
        root.m[0] = r.a;
        root.m[1] = r.c;
        root.m[2] = r.e;
        root.m[3] = r.b;
        root.m[4] = r.d;
        root.m[5] = r.f;
        // Outermost first: screen = H0 * H1 * ... * Hn * p.
        out = mat3Mul(out, mat3Mul(root, plane));
    }
    return finiteMat3(out);
}

bool affinePlane(const Mat3& h, Rect viewport, float tolerance, Mat2x3& out) {
    const float gx = h.m[6];
    const float gy = h.m[7];
    const float w0 = h.m[8];
    if (!finiteMat3(h) || !(w0 > 1e-6f)) {
        return false;
    }
    if (gx != 0.0f || gy != 0.0f) {
        // The affine map puts plane point p at H(p) * W(p) / w0, so its error at screen point s is
        // |s| * |W(p) / w0 - 1| with p the plane point under s. W is linear on the plane and the
        // plane region under the viewport is the convex quad of the corners' preimages, so the
        // largest |W / w0 - 1| is at a corner; |s| is at most the largest corner distance.
        Mat3 inv;
        if (!mat3Adjugate(h, inv)) {
            return false;
        }
        const Vec2 corners[4] = {Vec2{viewport.x, viewport.y}, Vec2{viewport.x + viewport.w, viewport.y},
                                 Vec2{viewport.x + viewport.w, viewport.y + viewport.h},
                                 Vec2{viewport.x, viewport.y + viewport.h}};
        float maxRel = 0.0f;
        float maxDist = 0.0f;
        for (const Vec2& s : corners) {
            const float px = inv.m[0] * s.x + inv.m[1] * s.y + inv.m[2];
            const float py = inv.m[3] * s.x + inv.m[4] * s.y + inv.m[5];
            const float pw = inv.m[6] * s.x + inv.m[7] * s.y + inv.m[8];
            if (!(std::fabs(pw) > 1e-12f)) {
                return false;
            }
            const float w = gx * (px / pw) + gy * (py / pw) + w0;
            if (!(w > 1e-6f)) {
                return false;  // this corner sees the plane behind the camera (or its horizon)
            }
            maxRel = std::max(maxRel, std::fabs(w / w0 - 1.0f));
            maxDist = std::max(maxDist, std::hypot(s.x, s.y));
        }
        if (!(maxDist * maxRel < tolerance)) {
            return false;
        }
    }
    out.a = h.m[0] / w0;
    out.c = h.m[1] / w0;
    out.e = h.m[2] / w0;
    out.b = h.m[3] / w0;
    out.d = h.m[4] / w0;
    out.f = h.m[5] / w0;
    return std::isfinite(out.a) && std::isfinite(out.b) && std::isfinite(out.c) && std::isfinite(out.d) &&
           std::isfinite(out.e) && std::isfinite(out.f);
}

}  // namespace xt::project3d
