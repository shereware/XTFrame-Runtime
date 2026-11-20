#include <cmath>
#include <span>

#include "camera.h"  // Code/Runtime/core/src: cssPerspectiveViewProj
#include "doctest.h"
#include "xt/project3d.h"
#include "xt/types.h"

/// T-761 (Docs/design/19-3d-space.md §19.y.1): project3d::projectPointHomogeneous -- the whole
/// mapping chain as one homogeneous point, no divide between the levels. Its x / w, y / w must be
/// the point projectPoint() (the Painter's bounds, the damage collection, xt::sw) computes and the
/// point planeHomography()'s H (the affine-plane test) maps to, so the GPU picture, the off-screen
/// bounds and the damage all use the same mapping.

using namespace xt;
using project3d::Mapping;

namespace {

Mapping level(float perspective, Vec2 origin, Mat2x3 rootXf, Mat4 world3) {
    Mapping m;
    m.hasProjection = true;
    m.viewProj = cssPerspectiveViewProj(perspective, origin).viewProj;
    m.rootXf = rootXf;
    m.planeWorld3 = world3;
    return m;
}

/// Checks the three paths agree at a grid of plane points; returns how many points projected.
int checkAgree(std::span<const Mapping> stack, float half) {
    Mat3 h;
    const bool haveH = project3d::planeHomography(stack, h);
    bool okOrigin = false;
    const float wOrigin = project3d::projectPointHomogeneous(stack, Vec2{0, 0}, okOrigin).z;
    REQUIRE(okOrigin);
    int projected = 0;
    for (float y = -half; y <= half; y += half / 4.0f) {
        for (float x = -half; x <= half; x += half / 4.0f) {
            bool okA = false, okB = false;
            const Vec2 a = project3d::projectPoint(stack, Mat2x3{}, Vec2{x, y}, okA);
            const Vec3 b = project3d::projectPointHomogeneous(stack, Vec2{x, y}, okB);
            CHECK(okA == okB);
            if (!okA || !okB) {
                continue;
            }
            ++projected;
            CHECK(b.z > 0.0f);
            CHECK(std::fabs(b.x / b.z - a.x) < 1e-3f);
            CHECK(std::fabs(b.y / b.z - a.y) < 1e-3f);
            if (haveH) {
                const float hx = h.m[0] * x + h.m[1] * y + h.m[2];
                const float hy = h.m[3] * x + h.m[4] * y + h.m[5];
                const float hw = h.m[6] * x + h.m[7] * y + h.m[8];
                CHECK(std::fabs(hx / hw - a.x) < 1e-2f);
                CHECK(std::fabs(hy / hw - a.y) < 1e-2f);
                // Same homogeneous point up to one scale for the whole plane.
                CHECK(std::fabs(hw / h.m[8] - b.z / wOrigin) < 1e-4f);
            }
        }
    }
    return projected;
}

}  // namespace

TEST_CASE("projectPointHomogeneous: an empty stack is the plane point with w = 1 (T-761)") {
    bool ok = false;
    const Vec3 p = project3d::projectPointHomogeneous({}, Vec2{3, -4}, ok);
    CHECK(ok);
    CHECK(p.x == 3.0f);
    CHECK(p.y == -4.0f);
    CHECK(p.z == 1.0f);
}

TEST_CASE("projectPointHomogeneous: flat outer level, perspective in the inner level (T-761)") {
    const Mapping stack[2] = {level(400.0f, Vec2{64, 64}, Mat2x3{}, mat4Identity()),
                              level(150.0f, Vec2{0, 0}, translation(Vec2{64, 64}), mat4RotateX(55.0f))};
    CHECK(checkAgree(stack, 48.0f) == 81);
    // The w that reaches the GPU varies across the plane (the perspective of the inner level); the
    // pre-T-761 vertices carried the outer level's w alone, 1 everywhere.
    bool ok = false;
    const float wTop = project3d::projectPointHomogeneous(stack, Vec2{0, -48}, ok).z;
    const float wBottom = project3d::projectPointHomogeneous(stack, Vec2{0, 48}, ok).z;
    CHECK(std::fabs(wTop - wBottom) > 0.2f);
}

TEST_CASE("projectPointHomogeneous: three levels at large translations (T-761)") {
    const Mapping stack[3] = {level(1200.0f, Vec2{960, 540}, translation(Vec2{0, 0}), mat4RotateX(12.0f)),
                              level(800.0f, Vec2{300, 200}, translation(Vec2{700, 400}), mat4RotateY(-20.0f)),
                              level(500.0f, Vec2{100, 80}, translation(Vec2{150, 90}),
                                    mat4Mul(mat4Translate(Vec3{40, 30, 20}), mat4RotateX(40.0f)))};
    CHECK(checkAgree(stack, 200.0f) == 81);
}

TEST_CASE("projectPointHomogeneous: a level behind the camera fails like projectPoint (T-761)") {
    // The inner layer pushed past the camera: its own w < 0 at every point.
    const Mapping stack[2] = {
        level(400.0f, Vec2{64, 64}, Mat2x3{}, mat4Identity()),
        level(150.0f, Vec2{0, 0}, translation(Vec2{64, 64}), mat4Translate(Vec3{0, 0, 300}))};
    bool ok = true;
    project3d::projectPointHomogeneous(stack, Vec2{0, 0}, ok);
    CHECK_FALSE(ok);
    // Both levels behind the camera: the product of the two w is positive, but each level fails on
    // its own (19.7's rule is per level), so the point is still dropped.
    const Mapping both[2] = {level(150.0f, Vec2{0, 0}, Mat2x3{}, mat4Translate(Vec3{0, 0, 300})),
                             level(150.0f, Vec2{0, 0}, Mat2x3{}, mat4Translate(Vec3{0, 0, 300}))};
    bool okPoint = true;
    project3d::projectPoint(both, Mat2x3{}, Vec2{1, 1}, okPoint);
    CHECK_FALSE(okPoint);
    ok = true;
    project3d::projectPointHomogeneous(both, Vec2{1, 1}, ok);
    CHECK_FALSE(ok);
}
