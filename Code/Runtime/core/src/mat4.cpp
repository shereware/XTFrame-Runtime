#include <cmath>
#include <cstddef>
#include <utility>

#include "xt/types.h"

/// Mat4/Mat3 3D transform and projection operations.
/// 見 Docs/design/19-3d-space.md §19.2, §19.3

namespace xt {

namespace {
constexpr float kDegToRad = 0.017453292519943295f;

/// Solves the linear system a * x = b for an n x n system using Gauss-Jordan elimination with
/// partial pivoting, working in double precision for accuracy. a is row-major, n*n entries, and
/// is destroyed. On success x receives the solution and detOut (if non-null) the determinant.
/// Returns false (x left unchanged) if the matrix is singular (|det| < eps).
bool gaussSolve(double* a, double* b, int n, double* x, double eps, double* detOut = nullptr) {
    double det = 1.0;
    for (int col = 0; col < n; ++col) {
        int pivotRow = col;
        double pivotAbs = std::fabs(a[col * n + col]);
        for (int row = col + 1; row < n; ++row) {
            double v = std::fabs(a[row * n + col]);
            if (v > pivotAbs) {
                pivotAbs = v;
                pivotRow = row;
            }
        }
        if (pivotAbs < eps) {
            return false;
        }
        if (pivotRow != col) {
            for (int c = 0; c < n; ++c) {
                std::swap(a[col * n + c], a[pivotRow * n + c]);
            }
            std::swap(b[col], b[pivotRow]);
            det = -det;
        }
        double pivot = a[col * n + col];
        det *= pivot;
        for (int row = 0; row < n; ++row) {
            if (row == col) {
                continue;
            }
            double factor = a[row * n + col] / pivot;
            if (factor == 0.0) {
                continue;
            }
            for (int c = col; c < n; ++c) {
                a[row * n + c] -= factor * a[col * n + c];
            }
            b[row] -= factor * b[col];
        }
    }
    for (int i = 0; i < n; ++i) {
        x[i] = b[i] / a[i * n + i];
    }
    if (detOut != nullptr) {
        *detOut = det;
    }
    return true;
}
}  // namespace

Mat4 mat4Identity() { return Mat4{}; }

Mat4 mat4Mul(const Mat4& a, const Mat4& b) {
    Mat4 out;
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            float sum = 0;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[row * 4 + k] * b.m[k * 4 + col];
            }
            out.m[row * 4 + col] = sum;
        }
    }
    return out;
}

Mat4 mat4FromMat2x3(const Mat2x3& t) {
    Mat4 out;
    // clang-format off
    out.m[0] = t.a; out.m[1] = t.c; out.m[2] = 0;  out.m[3] = t.e;
    out.m[4] = t.b; out.m[5] = t.d; out.m[6] = 0;  out.m[7] = t.f;
    out.m[8] = 0;   out.m[9] = 0;   out.m[10] = 1; out.m[11] = 0;
    out.m[12] = 0;  out.m[13] = 0;  out.m[14] = 0; out.m[15] = 1;
    // clang-format on
    return out;
}

Mat4 mat4Translate(Vec3 t) {
    Mat4 out;
    out.m[3] = t.x;
    out.m[7] = t.y;
    out.m[11] = t.z;
    return out;
}

Mat4 mat4Scale(Vec3 s) {
    Mat4 out;
    out.m[0] = s.x;
    out.m[5] = s.y;
    out.m[10] = s.z;
    return out;
}

Mat4 mat4RotateX(float deg) {
    float rad = deg * kDegToRad;
    float c = std::cos(rad);
    float s = std::sin(rad);
    Mat4 out;
    // clang-format off
    out.m[0] = 1; out.m[1] = 0; out.m[2] = 0;  out.m[3] = 0;
    out.m[4] = 0; out.m[5] = c; out.m[6] = -s; out.m[7] = 0;
    out.m[8] = 0; out.m[9] = s; out.m[10] = c; out.m[11] = 0;
    out.m[12] = 0; out.m[13] = 0; out.m[14] = 0; out.m[15] = 1;
    // clang-format on
    return out;
}

Mat4 mat4RotateY(float deg) {
    float rad = deg * kDegToRad;
    float c = std::cos(rad);
    float s = std::sin(rad);
    Mat4 out;
    // clang-format off
    out.m[0] = c;  out.m[1] = 0; out.m[2] = s; out.m[3] = 0;
    out.m[4] = 0;  out.m[5] = 1; out.m[6] = 0; out.m[7] = 0;
    out.m[8] = -s; out.m[9] = 0; out.m[10] = c; out.m[11] = 0;
    out.m[12] = 0; out.m[13] = 0; out.m[14] = 0; out.m[15] = 1;
    // clang-format on
    return out;
}

Mat4 mat4RotateZ(float deg) {
    float rad = deg * kDegToRad;
    float c = std::cos(rad);
    float s = std::sin(rad);
    Mat4 out;
    // clang-format off
    out.m[0] = c; out.m[1] = -s; out.m[2] = 0; out.m[3] = 0;
    out.m[4] = s; out.m[5] = c;  out.m[6] = 0; out.m[7] = 0;
    out.m[8] = 0; out.m[9] = 0;  out.m[10] = 1; out.m[11] = 0;
    out.m[12] = 0; out.m[13] = 0; out.m[14] = 0; out.m[15] = 1;
    // clang-format on
    return out;
}

namespace {
Vec3 vecSub(Vec3 a, Vec3 b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 vecCross(Vec3 a, Vec3 b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float vecDot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 vecNormalize(Vec3 v) {
    float len = std::sqrt(vecDot(v, v));
    if (len < 1e-12f) {
        return v;
    }
    return Vec3{v.x / len, v.y / len, v.z / len};
}
}  // namespace

Mat4 mat4LookAt(Vec3 eye, Vec3 target, Vec3 up) {
    Vec3 f = vecNormalize(vecSub(target, eye));
    Vec3 s = vecNormalize(vecCross(f, up));
    Vec3 u = vecCross(s, f);
    Mat4 out;
    // clang-format off
    out.m[0] = s.x; out.m[1] = s.y; out.m[2] = s.z;  out.m[3] = -vecDot(s, eye);
    out.m[4] = u.x; out.m[5] = u.y; out.m[6] = u.z;  out.m[7] = -vecDot(u, eye);
    out.m[8] = -f.x; out.m[9] = -f.y; out.m[10] = -f.z; out.m[11] = vecDot(f, eye);
    out.m[12] = 0; out.m[13] = 0; out.m[14] = 0; out.m[15] = 1;
    // clang-format on
    return out;
}

Mat4 mat4Perspective(float fovYDeg, float aspect, float nearZ, float farZ) {
    float f = 1.0f / std::tan(fovYDeg * kDegToRad * 0.5f);
    Mat4 out;
    out.m[0] = f / aspect;
    out.m[1] = 0;
    out.m[2] = 0;
    out.m[3] = 0;
    out.m[4] = 0;
    out.m[5] = f;
    out.m[6] = 0;
    out.m[7] = 0;
    out.m[8] = 0;
    out.m[9] = 0;
    out.m[10] = (farZ + nearZ) / (nearZ - farZ);
    out.m[11] = (2.0f * farZ * nearZ) / (nearZ - farZ);
    out.m[12] = 0;
    out.m[13] = 0;
    out.m[14] = -1;
    out.m[15] = 0;
    return out;
}

Mat4 mat4Ortho(float l, float r, float b, float t, float nearZ, float farZ) {
    Mat4 out;
    // clang-format off
    out.m[0] = 2.0f / (r - l); out.m[1] = 0; out.m[2] = 0; out.m[3] = -(r + l) / (r - l);
    out.m[4] = 0; out.m[5] = 2.0f / (t - b); out.m[6] = 0; out.m[7] = -(t + b) / (t - b);
    out.m[8] = 0; out.m[9] = 0; out.m[10] = -2.0f / (farZ - nearZ); out.m[11] = -(farZ + nearZ) / (farZ - nearZ);
    out.m[12] = 0; out.m[13] = 0; out.m[14] = 0; out.m[15] = 1;
    // clang-format on
    return out;
}

bool mat4Invert(const Mat4& m, Mat4& out) {
    double a[16];
    for (int i = 0; i < 16; ++i) {
        a[i] = static_cast<double>(m.m[i]);
    }
    Mat4 result;
    for (int col = 0; col < 4; ++col) {
        double work[16];
        for (int i = 0; i < 16; ++i) {
            work[i] = a[i];
        }
        double b[4] = {0, 0, 0, 0};
        b[col] = 1.0;
        double x[4];
        double det = 0.0;
        if (!gaussSolve(work, b, 4, x, 1e-12, &det)) {
            return false;
        }
        if (col == 0 && std::fabs(det) < 1e-12) {
            return false;
        }
        for (int row = 0; row < 4; ++row) {
            result.m[row * 4 + col] = static_cast<float>(x[row]);
        }
    }
    out = result;
    return true;
}

Vec3 mat4Transform(const Mat4& m, Vec3 p) {
    return Vec3{m.m[0] * p.x + m.m[1] * p.y + m.m[2] * p.z + m.m[3],
                m.m[4] * p.x + m.m[5] * p.y + m.m[6] * p.z + m.m[7],
                m.m[8] * p.x + m.m[9] * p.y + m.m[10] * p.z + m.m[11]};
}

Vec3 mat4TransformVector(const Mat4& m, Vec3 v) {
    return Vec3{m.m[0] * v.x + m.m[1] * v.y + m.m[2] * v.z, m.m[4] * v.x + m.m[5] * v.y + m.m[6] * v.z,
                m.m[8] * v.x + m.m[9] * v.y + m.m[10] * v.z};
}

Vec3 mat4Project(const Mat4& m, Vec3 p, float* wOut) {
    float x = m.m[0] * p.x + m.m[1] * p.y + m.m[2] * p.z + m.m[3];
    float y = m.m[4] * p.x + m.m[5] * p.y + m.m[6] * p.z + m.m[7];
    float z = m.m[8] * p.x + m.m[9] * p.y + m.m[10] * p.z + m.m[11];
    float w = m.m[12] * p.x + m.m[13] * p.y + m.m[14] * p.z + m.m[15];
    if (w <= 1e-6f) {
        if (wOut != nullptr) {
            *wOut = 0;
        }
        return Vec3{0, 0, 0};
    }
    if (wOut != nullptr) {
        *wOut = w;
    }
    return Vec3{x / w, y / w, z / w};
}

bool homographyFromQuad(const Vec2 src[4], const Vec2 dst[4], Mat3& out) {
    // 見 Docs/design/19-3d-space.md §19.3: DLT for a 4-point-to-4-point 2D homography, solving
    // for h0..h7 with h8 fixed at 1.
    double a[64];
    double b[8];
    for (int i = 0; i < 8; ++i) {
        b[i] = 0;
        for (int j = 0; j < 8; ++j) {
            a[i * 8 + j] = 0;
        }
    }
    for (int i = 0; i < 4; ++i) {
        double x = static_cast<double>(src[i].x);
        double y = static_cast<double>(src[i].y);
        double xp = static_cast<double>(dst[i].x);
        double yp = static_cast<double>(dst[i].y);
        int r0 = i * 2;
        a[r0 * 8 + 0] = x;
        a[r0 * 8 + 1] = y;
        a[r0 * 8 + 2] = 1;
        a[r0 * 8 + 6] = -x * xp;
        a[r0 * 8 + 7] = -y * xp;
        b[r0] = xp;
        int r1 = i * 2 + 1;
        a[r1 * 8 + 3] = x;
        a[r1 * 8 + 4] = y;
        a[r1 * 8 + 5] = 1;
        a[r1 * 8 + 6] = -x * yp;
        a[r1 * 8 + 7] = -y * yp;
        b[r1] = yp;
    }
    double h[8];
    if (!gaussSolve(a, b, 8, h, 1e-9)) {
        return false;
    }
    Mat3 result;
    for (int i = 0; i < 8; ++i) {
        result.m[i] = static_cast<float>(h[i]);
    }
    result.m[8] = 1;
    out = result;
    return true;
}

Vec2 mat3Apply(const Mat3& h, Vec2 p) {
    float w = h.m[6] * p.x + h.m[7] * p.y + h.m[8];
    if (w <= 1e-6f) {
        return p;
    }
    float x = (h.m[0] * p.x + h.m[1] * p.y + h.m[2]) / w;
    float y = (h.m[3] * p.x + h.m[4] * p.y + h.m[5]) / w;
    return Vec2{x, y};
}

}  // namespace xt
