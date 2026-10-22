#include "camera.h"
#include "doctest.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/types.h"

/// camera.h tests: computeCameraViewProj / cssPerspectiveViewProj / defaultSceneViewProj. See
/// Docs/design/19-3d-space.md §19.4, Docs/tasks/T-43b.md "測試".

using namespace xt;

namespace {
constexpr float kEps = 0.05f;
}

TEST_CASE("cssPerspectiveViewProj: CSS perspective() mapping") {
    CameraSetup setup = cssPerspectiveViewProj(500.0f, Vec2{100, 50});
    CHECK(!setup.orthographicIdentity);

    float w = 0;
    Vec3 p0 = mat4Project(setup.viewProj, Vec3{100, 50, 0}, &w);
    CHECK(w > 0);
    CHECK(p0.x == doctest::Approx(100.0f).epsilon(kEps));
    CHECK(p0.y == doctest::Approx(50.0f).epsilon(kEps));

    // x' = ox + (x - ox) / (1 - z/d) = 100 + 100 / (1 - 250/500) = 300.
    Vec3 p1 = mat4Project(setup.viewProj, Vec3{200, 50, 250}, &w);
    CHECK(w > 0);
    CHECK(p1.x == doctest::Approx(300.0f).epsilon(kEps));
    CHECK(p1.y == doctest::Approx(50.0f).epsilon(kEps));
}

TEST_CASE("cssPerspectiveViewProj: perspective:none is the identity") {
    CameraSetup setup = cssPerspectiveViewProj(0.0f, Vec2{100, 50});
    CHECK(setup.orthographicIdentity);
    float w = 0;
    Vec3 p = mat4Project(setup.viewProj, Vec3{10, 20, 5}, &w);
    CHECK(w > 0);
    CHECK(p.x == doctest::Approx(10.0f));
    CHECK(p.y == doctest::Approx(20.0f));
}

TEST_CASE("defaultSceneViewProj: AE default camera, center point unaffected") {
    CameraSetup setup = defaultSceneViewProj(400.0f, 300.0f);
    CHECK(!setup.orthographicIdentity);
    float w = 0;
    Vec3 p = mat4Project(setup.viewProj, Vec3{200, 150, 0}, &w);
    CHECK(w > 0);
    CHECK(p.x == doctest::Approx(200.0f).epsilon(kEps));
    CHECK(p.y == doctest::Approx(150.0f).epsilon(kEps));
}

namespace {
/// Builds a perspective camera node looking at (200,150,0) from (200,150,400), zoom 400.
NodeId buildTestCamera(NodeArena& arena) {
    NodeId camNode = arena.create(NodeKind::Camera);
    NodeCommon& c = arena.common(camNode);
    c.translate = Vec2{200, 150};
    c.translateZ = 400;
    CameraData& cam = arena.camera(camNode);
    cam.kind = 0;
    cam.useTarget = 1;
    cam.target = Vec3{200, 150, 0};
    cam.zoom = 400;
    cam.nearZ = 1;
    cam.farZ = 100000;
    return camNode;
}
}  // namespace

TEST_CASE("computeCameraViewProj: perspective, on-axis points project unmoved") {
    NodeArena arena;
    NodeId camNode = buildTestCamera(arena);
    CameraSetup setup = computeCameraViewProj(arena, camNode, Rect{0, 0, 400, 300});
    CHECK(!setup.orthographicIdentity);

    float w = 0;
    Vec3 p0 = mat4Project(setup.viewProj, Vec3{200, 150, 0}, &w);
    CHECK(w > 0);
    CHECK(p0.x == doctest::Approx(200.0f).epsilon(kEps));
    CHECK(p0.y == doctest::Approx(150.0f).epsilon(kEps));

    // On-axis: z alone must not move x/y.
    Vec3 p2 = mat4Project(setup.viewProj, Vec3{200, 150, 200}, &w);
    CHECK(w > 0);
    CHECK(p2.x == doctest::Approx(200.0f).epsilon(kEps));
    CHECK(p2.y == doctest::Approx(150.0f).epsilon(kEps));
}

TEST_CASE("computeCameraViewProj: orthographic, z does not affect x/y") {
    NodeArena arena;
    NodeId camNode = buildTestCamera(arena);
    arena.camera(camNode).kind = 1;  // Orthographic.
    CameraSetup setup = computeCameraViewProj(arena, camNode, Rect{0, 0, 400, 300});

    float w = 0;
    Vec3 near = mat4Project(setup.viewProj, Vec3{300, 150, 0}, &w);
    CHECK(w > 0);
    Vec3 far = mat4Project(setup.viewProj, Vec3{300, 150, 200}, &w);
    CHECK(w > 0);
    CHECK(near.x == doctest::Approx(far.x).epsilon(kEps));
    CHECK(near.y == doctest::Approx(far.y).epsilon(kEps));
}

// T-43b legacy fix: 19.4's y-down camera frame (b/s/d), not gluLookAt (which mirrors an axis in
// a y-down world). See Docs/tasks/T-43c.md "T-43b 遺留修正" 1.
TEST_CASE("computeCameraViewProj: y-down camera frame, off-axis points do not mirror") {
    NodeArena arena;
    NodeId camNode = buildTestCamera(arena);  // eye (200,150,400), target (200,150,0), zoom 400.
    CameraSetup setup = computeCameraViewProj(arena, camNode, Rect{0, 0, 400, 300});

    float w = 0;
    // On-axis point still maps to (200, 150).
    Vec3 center = mat4Project(setup.viewProj, Vec3{200, 150, 0}, &w);
    CHECK(w > 0);
    CHECK(center.x == doctest::Approx(200.0f).epsilon(kEps));
    CHECK(center.y == doctest::Approx(150.0f).epsilon(kEps));

    // (400, 150, 0): x = 400, not mirrored to 0.
    Vec3 right = mat4Project(setup.viewProj, Vec3{400, 150, 0}, &w);
    CHECK(w > 0);
    CHECK(right.x == doctest::Approx(400.0f).epsilon(kEps));

    // (300, 150, 200): closer to the camera (larger z), so x > 300 (perspective magnifies it).
    Vec3 nearer = mat4Project(setup.viewProj, Vec3{300, 150, 200}, &w);
    CHECK(w > 0);
    CHECK(nearer.x > 300.0f);

    // y direction: (200, 300, 0) -> y = 300 (view space is already y-down, no flip).
    Vec3 down = mat4Project(setup.viewProj, Vec3{200, 300, 0}, &w);
    CHECK(w > 0);
    CHECK(down.y == doctest::Approx(300.0f).epsilon(kEps));
}
