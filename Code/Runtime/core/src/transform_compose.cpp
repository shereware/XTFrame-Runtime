#include "transform_compose.h"

#include <cmath>

/// 05.5's compose-then-decompose path for arbitrary-order `transform` sequences (T-120).
/// See Docs/design/05-style-and-css.md §5.5 for the algorithm this file implements verbatim.

namespace xt {

namespace {

constexpr double kRadToDeg = 57.29577951308232;

struct Vec3d {
    double x = 0;
    double y = 0;
    double z = 0;
};

Vec3d sub(Vec3d a, Vec3d b) { return Vec3d{a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3d scaled(Vec3d v, double s) { return Vec3d{v.x * s, v.y * s, v.z * s}; }
double dot(Vec3d a, Vec3d b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double length(Vec3d v) { return std::sqrt(dot(v, v)); }

/// Normalizes an angle in degrees into (-180, 180].
float normalizeDeg(double deg) {
    if (!std::isfinite(deg)) {
        return 0;
    }
    while (deg > 180.0) {
        deg -= 360.0;
    }
    while (deg <= -180.0) {
        deg += 360.0;
    }
    // -0 would round-trip fine but reads badly in diagnostics and golden IR dumps.
    return deg == 0.0 ? 0.0f : static_cast<float>(deg);
}

}  // namespace

Mat4 composeTransformSequence(const TransformFn* fns, size_t count) {
    // CSS order: M = F1 * F2 * ... * Fn, so the last function written is applied first to a point.
    Mat4 m = mat4Identity();
    for (size_t i = 0; i < count; ++i) {
        const TransformFn& f = fns[i];
        Mat4 step;
        switch (f.kind) {
            case TransformFn::Kind::Translate:
                step = mat4Translate(Vec3{f.a, f.b, f.c});
                break;
            case TransformFn::Kind::RotateX:
                step = mat4RotateX(f.a);
                break;
            case TransformFn::Kind::RotateY:
                step = mat4RotateY(f.a);
                break;
            case TransformFn::Kind::RotateZ:
                step = mat4RotateZ(f.a);
                break;
            case TransformFn::Kind::Skew:
                // 03.3's skew is Lottie's (angle, axis) pair, which is what the fixed-order fast
                // path stores for `skew(a, b)`; composing through the same skewDeg() keeps the
                // two paths consistent.
                step = mat4FromMat2x3(skewDeg(f.a, f.b));
                break;
            case TransformFn::Kind::Scale:
                step = mat4Scale(Vec3{f.a, f.b, 1.0f});
                break;
        }
        m = mat4Mul(m, step);
    }
    return m;
}

Mat4 composeTransformComponents(const TransformComponents& c) {
    // 03.3 order, matching Code/Runtime/core/src/world_transform.cpp's composeLocal3() with no anchor.
    Mat4 m = mat4Translate(Vec3{c.translateX, c.translateY, c.translateZ});
    m = mat4Mul(m, mat4RotateZ(c.rotation));
    m = mat4Mul(m, mat4RotateY(c.rotationY));
    m = mat4Mul(m, mat4RotateX(c.rotationX));
    m = mat4Mul(m, mat4FromMat2x3(skewDeg(c.skewX, c.skewY)));
    m = mat4Mul(m, mat4Scale(Vec3{c.scaleX, c.scaleY, 1.0f}));
    return m;
}

TransformDecomposeStatus decomposeTransform(const Mat4& m, TransformComponents& out) {
    TransformComponents r;
    r.translateX = m.m[3];
    r.translateY = m.m[7];
    r.translateZ = m.m[11];

    // Columns of the upper-left 3x3.
    Vec3d a1{m.m[0], m.m[4], m.m[8]};
    Vec3d a2{m.m[1], m.m[5], m.m[9]};
    Vec3d a3{m.m[2], m.m[6], m.m[10]};

    double scaleX = length(a1);
    Vec3d r1 = scaleX > 1e-9 ? scaled(a1, 1.0 / scaleX) : Vec3d{1, 0, 0};

    double skewXRaw = dot(r1, a2);
    Vec3d a2p = sub(a2, scaled(r1, skewXRaw));
    double scaleY = length(a2p);
    Vec3d r2 = scaleY > 1e-9 ? scaled(a2p, 1.0 / scaleY) : Vec3d{0, 1, 0};

    // The third axis must be a unit vector orthogonal to the other two; anything else needs a 3D
    // shear or a z scale, which 03.3's component set cannot represent (05.5).
    double d1 = dot(r1, a3);
    double d2 = dot(r2, a3);
    Vec3d a3p = sub(sub(a3, scaled(r1, d1)), scaled(r2, d2));
    if (std::fabs(length(a3p) - 1.0) > 1e-3 || std::fabs(d1) > 1e-3 || std::fabs(d2) > 1e-3) {
        return TransformDecomposeStatus::Shear3D;
    }

    // R = [r1 r2 a3p] as columns. A negative determinant means the sequence mirrors; 05.5 puts
    // the flip into scaleY so the remaining rotation stays proper.
    double det = r1.x * (r2.y * a3p.z - a3p.y * r2.z) - r2.x * (r1.y * a3p.z - a3p.y * r1.z) +
                 a3p.x * (r1.y * r2.z - r2.y * r1.z);
    if (det < 0) {
        scaleY = -scaleY;
        r2 = scaled(r2, -1.0);
    }

    r.scaleX = static_cast<float>(scaleX);
    r.scaleY = static_cast<float>(scaleY);
    r.skewX = std::fabs(scaleY) > 1e-9 ? normalizeDeg(std::atan(skewXRaw / scaleY) * kRadToDeg) : 0.0f;
    r.skewY = 0;

    // Euler angles of R = Rz * Ry * Rx (world_transform.cpp's composeLocal3 order).
    double m20 = r1.z, m21 = r2.z, m22 = a3p.z;
    double m10 = r1.y, m00 = r1.x, m01 = r2.x, m11 = r2.y;
    double sinY = -m20;
    sinY = sinY < -1.0 ? -1.0 : (sinY > 1.0 ? 1.0 : sinY);
    double rotY = std::asin(sinY);
    double rotX = 0;
    double rotZ = 0;
    if (std::fabs(std::cos(rotY)) > 1e-6) {
        rotX = std::atan2(m21, m22);
        rotZ = std::atan2(m10, m00);
    } else {
        // Gimbal lock (rotateY = +/-90 degrees): X and Z rotate about the same axis, so 05.5 puts
        // the whole remaining rotation into Z and leaves rotateX at 0.
        rotX = 0;
        rotZ = std::atan2(-m01, m11);
    }
    r.rotationX = normalizeDeg(rotX * kRadToDeg);
    r.rotationY = normalizeDeg(rotY * kRadToDeg);
    r.rotation = normalizeDeg(rotZ * kRadToDeg);

    out = r;
    return TransformDecomposeStatus::Ok;
}

TransformDecomposeStatus decomposeTransformSequence(const TransformFn* fns, size_t count,
                                                    TransformComponents& out) {
    return decomposeTransform(composeTransformSequence(fns, count), out);
}

bool decomposeMatrix2D(float a, float b, float c, float d, float e, float f, TransformFn out[4]) {
    const double det = static_cast<double>(a) * d - static_cast<double>(b) * c;
    if (!std::isfinite(det) || !std::isfinite(e) || !std::isfinite(f) || std::fabs(det) < 1e-6) {
        return false;
    }
    // Columns (a, b) and (c, d): M = R(r) * [sx k'; 0 sy] with sx = |(a, b)|, and the upper
    // triangle is skewX(k) * scale(sx, sy) = [sx tan(k)*sy; 0 sy].
    const double sx = std::sqrt(static_cast<double>(a) * a + static_cast<double>(b) * b);
    const double r = std::atan2(static_cast<double>(b), static_cast<double>(a));
    const double cr = std::cos(r);
    const double sr = std::sin(r);
    const double upper = cr * c + sr * d;  // R(-r) applied to (c, d): x.
    const double sy = -sr * c + cr * d;    // ...and y (= det / sx, never 0 here).
    const double skew = std::atan(upper / sy);
    out[0] = TransformFn{TransformFn::Kind::Translate, e, f, 0};
    out[1] = TransformFn{TransformFn::Kind::RotateZ, static_cast<float>(r * kRadToDeg), 0, 0};
    out[2] = TransformFn{TransformFn::Kind::Skew, static_cast<float>(skew * kRadToDeg), 0, 0};
    out[3] = TransformFn{TransformFn::Kind::Scale, static_cast<float>(sx), static_cast<float>(sy), 0};
    return true;
}

bool transformSkewOutOfRange(const TransformComponents& c) {
    return std::fabs(c.skewX) > 89.0f || std::fabs(c.skewY) > 89.0f;
}

}  // namespace xt
