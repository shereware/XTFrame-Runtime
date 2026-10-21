#include <cmath>
#include <random>

#include "doctest.h"
#include "xt/types.h"

using namespace xt;

namespace {
void checkVec3Approx(Vec3 actual, Vec3 expected, float eps = 1e-5f) {
    CHECK(actual.x == doctest::Approx(expected.x).epsilon(eps));
    CHECK(actual.y == doctest::Approx(expected.y).epsilon(eps));
    CHECK(actual.z == doctest::Approx(expected.z).epsilon(eps));
}
void checkVec2Approx(Vec2 actual, Vec2 expected, float eps = 1e-4f) {
    CHECK(actual.x == doctest::Approx(expected.x).epsilon(eps));
    CHECK(actual.y == doctest::Approx(expected.y).epsilon(eps));
}
void checkMat4Approx(const Mat4& actual, const Mat4& expected, float eps = 1e-4f) {
    for (int i = 0; i < 16; ++i) {
        CHECK(actual.m[i] == doctest::Approx(expected.m[i]).epsilon(eps));
    }
}
}  // namespace

TEST_CASE("mat4Identity applied leaves point unchanged") {
    checkVec3Approx(mat4Transform(mat4Identity(), Vec3{3, 4, 5}), Vec3{3, 4, 5});
}

TEST_CASE("mat4Mul against hand-computed matrices") {
    // Translate then scale, applied to a point: mat4Mul(scale, translate) applies translate first.
    Mat4 t = mat4Translate(Vec3{1, 2, 3});
    Mat4 s = mat4Scale(Vec3{2, 2, 2});
    Mat4 st = mat4Mul(s, t);
    checkVec3Approx(mat4Transform(st, Vec3{1, 1, 1}), Vec3{4, 6, 8});

    Mat4 ts = mat4Mul(t, s);
    checkVec3Approx(mat4Transform(ts, Vec3{1, 1, 1}), Vec3{3, 4, 5});

    Mat4 rz = mat4RotateZ(90.0f);
    Mat4 combo = mat4Mul(t, rz);
    checkVec3Approx(mat4Transform(combo, Vec3{1, 0, 0}), Vec3{1, 3, 3});
}

TEST_CASE("mat4FromMat2x3 matches Mat2x3::apply") {
    Mat2x3 m2 = composeTRSS(Vec2{5, 3}, 30.0f, Vec2{0, 0}, Vec2{1.5f, 2.0f}, Vec2{0, 0});
    Mat4 m4 = mat4FromMat2x3(m2);
    Vec2 p2{7, -2};
    Vec2 expected2 = apply(m2, p2);
    Vec3 got3 = mat4Transform(m4, Vec3{p2.x, p2.y, 0});
    CHECK(got3.x == doctest::Approx(expected2.x).epsilon(1e-6f));
    CHECK(got3.y == doctest::Approx(expected2.y).epsilon(1e-6f));
    CHECK(got3.z == doctest::Approx(0.0f).epsilon(1e-6f));

    Vec2 t2{5, 3};
    Vec3 gotT = mat4Transform(m4, Vec3{0, 0, 0});
    CHECK(gotT.x == doctest::Approx(t2.x).epsilon(1e-6f));
    CHECK(gotT.y == doctest::Approx(t2.y).epsilon(1e-6f));
}

TEST_CASE("axis rotations") {
    checkVec3Approx(mat4Transform(mat4RotateZ(90.0f), Vec3{1, 0, 0}), Vec3{0, 1, 0}, 1e-5f);
    checkVec3Approx(mat4Transform(mat4RotateX(90.0f), Vec3{0, 1, 0}), Vec3{0, 0, 1}, 1e-5f);
    checkVec3Approx(mat4Transform(mat4RotateY(90.0f), Vec3{0, 0, 1}), Vec3{1, 0, 0}, 1e-5f);
}

TEST_CASE("mat4TransformVector ignores translation") {
    Mat4 t = mat4Translate(Vec3{10, 20, 30});
    checkVec3Approx(mat4TransformVector(t, Vec3{1, 2, 3}), Vec3{1, 2, 3});

    checkVec3Approx(mat4TransformVector(mat4RotateZ(90.0f), Vec3{1, 0, 0}), Vec3{0, 1, 0}, 1e-5f);
}

TEST_CASE("mat4LookAt maps target toward -z") {
    Mat4 view = mat4LookAt(Vec3{0, 0, 5}, Vec3{0, 0, 0}, Vec3{0, -1, 0});
    checkVec3Approx(mat4Transform(view, Vec3{0, 0, 0}), Vec3{0, 0, -5}, 1e-5f);
}

TEST_CASE("mat4Perspective NDC z and x") {
    Mat4 proj = mat4Perspective(90.0f, 1.0f, 1.0f, 100.0f);
    float w = 0;
    Vec3 nearP = mat4Project(proj, Vec3{0, 0, -1}, &w);
    CHECK(nearP.z == doctest::Approx(-1.0f).epsilon(1e-4f));
    Vec3 farP = mat4Project(proj, Vec3{0, 0, -100}, &w);
    CHECK(farP.z == doctest::Approx(1.0f).epsilon(1e-4f));
    Vec3 sideP = mat4Project(proj, Vec3{1, 0, -1}, &w);
    CHECK(sideP.x == doctest::Approx(1.0f).epsilon(1e-4f));
}

TEST_CASE("mat4Ortho corners") {
    Mat4 ortho = mat4Ortho(0.0f, 100.0f, 100.0f, 0.0f, -1.0f, 1.0f);
    checkVec3Approx(mat4Project(ortho, Vec3{50, 50, 0}, nullptr), Vec3{0, 0, 0}, 1e-5f);
    checkVec3Approx(mat4Project(ortho, Vec3{100, 0, 0}, nullptr), Vec3{1, 1, 0}, 1e-5f);
}

TEST_CASE("mat4Invert of random invertible matrix") {
    std::mt19937 rng(777u);
    std::uniform_real_distribution<float> dist(-5.0f, 5.0f);
    Mat4 m = mat4Mul(mat4Translate(Vec3{dist(rng), dist(rng), dist(rng)}),
                     mat4Mul(mat4RotateX(dist(rng) * 10.0f),
                             mat4Mul(mat4RotateY(dist(rng) * 10.0f), mat4Scale(Vec3{1.3f, 0.7f, 2.1f}))));
    Mat4 inv;
    REQUIRE(mat4Invert(m, inv));
    checkMat4Approx(mat4Mul(m, inv), mat4Identity(), 1e-4f);
}

TEST_CASE("mat4Invert of singular matrix returns false") {
    Mat4 m = mat4Scale(Vec3{1, 0, 1});
    Mat4 out;
    CHECK_FALSE(mat4Invert(m, out));
}

TEST_CASE("mat4Project with w below threshold returns origin") {
    // Row3 = (0,0,0,0): w is always 0 regardless of input point.
    Mat4 m = mat4Identity();
    m.m[12] = 0;
    m.m[13] = 0;
    m.m[14] = 0;
    m.m[15] = 0;
    float w = 123.0f;
    Vec3 p = mat4Project(m, Vec3{1, 2, 3}, &w);
    CHECK(p.x == 0.0f);
    CHECK(p.y == 0.0f);
    CHECK(p.z == 0.0f);
    CHECK(w == 0.0f);
}

TEST_CASE("homographyFromQuad affine case: unit square to translated/scaled rect") {
    Vec2 src[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    Vec2 dst[4] = {{10, 20}, {30, 20}, {30, 60}, {10, 60}};
    Mat3 h;
    REQUIRE(homographyFromQuad(src, dst, h));
    for (int i = 0; i < 4; ++i) {
        checkVec2Approx(mat3Apply(h, src[i]), dst[i], 1e-4f);
    }
}

TEST_CASE("homographyFromQuad perspective case: unit square to trapezoid") {
    Vec2 src[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    Vec2 dst[4] = {{0, 0}, {1, 0}, {0.8f, 1}, {0.2f, 1}};
    Mat3 h;
    REQUIRE(homographyFromQuad(src, dst, h));
    for (int i = 0; i < 4; ++i) {
        checkVec2Approx(mat3Apply(h, src[i]), dst[i], 1e-4f);
    }
}

TEST_CASE("homographyFromQuad degenerate (collinear) returns false") {
    Vec2 src[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    Vec2 dst[4] = {{0, 0}, {1, 0}, {2, 0}, {3, 0}};
    Mat3 h;
    CHECK_FALSE(homographyFromQuad(src, dst, h));
}
