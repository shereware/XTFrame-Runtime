#include <cmath>

#include "xt/types.h"

/// Mat2x3 affine transform operations.
/// 見 Docs/design/03-scene-and-properties.md §3.3

namespace xt {

namespace {
constexpr float kDegToRad = 0.017453292519943295f;
}

Mat2x3 mul(const Mat2x3& lhs, const Mat2x3& rhs) {
    Mat2x3 out;
    out.a = lhs.a * rhs.a + lhs.c * rhs.b;
    out.c = lhs.a * rhs.c + lhs.c * rhs.d;
    out.e = lhs.a * rhs.e + lhs.c * rhs.f + lhs.e;
    out.b = lhs.b * rhs.a + lhs.d * rhs.b;
    out.d = lhs.b * rhs.c + lhs.d * rhs.d;
    out.f = lhs.b * rhs.e + lhs.d * rhs.f + lhs.f;
    return out;
}

Vec2 apply(const Mat2x3& m, Vec2 p) { return Vec2{m.a * p.x + m.c * p.y + m.e, m.b * p.x + m.d * p.y + m.f}; }

Vec2 applyVector(const Mat2x3& m, Vec2 v) { return Vec2{m.a * v.x + m.c * v.y, m.b * v.x + m.d * v.y}; }

float determinant(const Mat2x3& m) { return m.a * m.d - m.b * m.c; }

bool invert(const Mat2x3& m, Mat2x3& out) {
    float det = determinant(m);
    if (std::fabs(det) < 1e-12f) {
        return false;
    }
    float invDet = 1.0f / det;
    Mat2x3 result;
    result.a = m.d * invDet;
    result.c = -m.c * invDet;
    result.b = -m.b * invDet;
    result.d = m.a * invDet;
    result.e = -(result.a * m.e + result.c * m.f);
    result.f = -(result.b * m.e + result.d * m.f);
    out = result;
    return true;
}

Mat2x3 translation(Vec2 t) { return Mat2x3{1, 0, 0, 1, t.x, t.y}; }

Mat2x3 scaling(Vec2 s) { return Mat2x3{s.x, 0, 0, s.y, 0, 0}; }

Mat2x3 rotationDeg(float deg) {
    float rad = deg * kDegToRad;
    float c = std::cos(rad);
    float s = std::sin(rad);
    return Mat2x3{c, s, -s, c, 0, 0};
}

Mat2x3 skewDeg(float skewX, float skewAxis) {
    float t = std::tan(skewX * kDegToRad);
    Mat2x3 shearX{1, 0, t, 1, 0, 0};
    return mul(::xt::rotationDeg(-skewAxis), mul(shearX, ::xt::rotationDeg(skewAxis)));
}

Mat2x3 composeTRSS(Vec2 translate, float rotDeg, Vec2 skew, Vec2 scale, Vec2 anchor) {
    return mul(
        translation(translate),
        mul(::xt::rotationDeg(rotDeg),
            mul(skewDeg(skew.x, skew.y), mul(scaling(scale), translation(Vec2{-anchor.x, -anchor.y})))));
}

Rect transformBounds(const Mat2x3& m, const Rect& rect) {
    Vec2 p0 = apply(m, Vec2{rect.x, rect.y});
    Vec2 p1 = apply(m, Vec2{rect.x + rect.w, rect.y});
    Vec2 p2 = apply(m, Vec2{rect.x, rect.y + rect.h});
    Vec2 p3 = apply(m, Vec2{rect.x + rect.w, rect.y + rect.h});
    float minX = p0.x, maxX = p0.x, minY = p0.y, maxY = p0.y;
    for (Vec2 p : {p1, p2, p3}) {
        minX = p.x < minX ? p.x : minX;
        maxX = p.x > maxX ? p.x : maxX;
        minY = p.y < minY ? p.y : minY;
        maxY = p.y > maxY ? p.y : maxY;
    }
    return Rect{minX, minY, maxX - minX, maxY - minY};
}

bool approxEqual(const Mat2x3& lhs, const Mat2x3& rhs, float eps) {
    return std::fabs(lhs.a - rhs.a) <= eps && std::fabs(lhs.b - rhs.b) <= eps &&
           std::fabs(lhs.c - rhs.c) <= eps && std::fabs(lhs.d - rhs.d) <= eps &&
           std::fabs(lhs.e - rhs.e) <= eps && std::fabs(lhs.f - rhs.f) <= eps;
}

}  // namespace xt
