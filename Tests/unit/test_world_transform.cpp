#include "doctest.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/painter.h"
#include "xt/types.h"

/// updateWorldTransforms tests. See Docs/tasks/T-10.md "測試" / "test_world_transform.cpp".

using namespace xt;

namespace {

/// Builds root(translate 10,0) -> a(rotation 90) -> b(translate 1,0), with opacity 0.5 / 0.5 / 1.
struct Chain {
    NodeArena arena;
    NodeId root = kInvalidNode;
    NodeId a = kInvalidNode;
    NodeId b = kInvalidNode;
};

Chain buildChain() {
    Chain c;
    c.root = c.arena.create(NodeKind::Group);
    c.a = c.arena.create(NodeKind::Group);
    c.b = c.arena.create(NodeKind::Group);
    c.arena.appendChild(c.root, c.a);
    c.arena.appendChild(c.a, c.b);

    NodeCommon& root = c.arena.common(c.root);
    root.translate = Vec2{10, 0};
    root.opacity = 0.5f;

    NodeCommon& a = c.arena.common(c.a);
    a.rotation = 90;
    a.opacity = 0.5f;

    NodeCommon& b = c.arena.common(c.b);
    b.translate = Vec2{1, 0};

    return c;
}

}  // namespace

TEST_CASE("updateWorldTransforms: three-level chain, world and worldOpacity") {
    Chain c = buildChain();
    updateWorldTransforms(c.arena, c.root, true);

    Vec2 p = apply(c.arena.common(c.b).world, Vec2{0, 0});
    CHECK(p.x == doctest::Approx(10.0f));
    CHECK(p.y == doctest::Approx(1.0f));

    CHECK(c.arena.common(c.b).worldOpacity == doctest::Approx(0.25f));
    CHECK(c.arena.common(c.a).worldOpacity == doctest::Approx(0.25f));
    CHECK(c.arena.common(c.root).worldOpacity == doctest::Approx(0.5f));
}

TEST_CASE("updateWorldTransforms: partial dirty subtree recompute, ancestor untouched") {
    Chain c = buildChain();
    updateWorldTransforms(c.arena, c.root, true);

    Mat2x3 rootWorldBefore = c.arena.common(c.root).world;

    c.arena.common(c.a).translate = Vec2{0, 5};
    c.arena.markDirty(c.a, kDirtyTransform);
    updateWorldTransforms(c.arena, c.root, false);

    // root did not carry kDirtyTransform/kDirtyTree, so it must be untouched.
    CHECK(approxEqual(c.arena.common(c.root).world, rootWorldBefore));

    // b is a's descendant, so it must have recomputed against a's new local transform.
    // local_a = R(90) * T(0,5)-ish per composeTRSS's TRSS order: T(translate) * R(rot) * ... so
    // a's own translate happens last relative to R in composeTRSS -- verify against direct compose.
    Mat2x3 expectedWorldA =
        mul(c.arena.common(c.root).world, composeTRSS(Vec2{0, 5}, 90.0f, Vec2{}, Vec2{1, 1}, Vec2{}));
    CHECK(approxEqual(c.arena.common(c.a).world, expectedWorldA));

    Mat2x3 expectedWorldB = mul(expectedWorldA, composeTRSS(Vec2{1, 0}, 0.0f, Vec2{}, Vec2{1, 1}, Vec2{}));
    CHECK(approxEqual(c.arena.common(c.b).world, expectedWorldB));
}

TEST_CASE("updateWorldTransforms: 3D layer under a camera root gets world3 = local3") {
    // root(Group, has a Camera child) -> layer(Group, is3D, translateZ=50).
    NodeArena arena;
    NodeId root = arena.create(NodeKind::Group);
    NodeId cam = arena.create(NodeKind::Camera);
    arena.appendChild(root, cam);
    NodeId layer = arena.create(NodeKind::Group);
    arena.appendChild(root, layer);
    arena.common(layer).is3D = 1;
    arena.common(layer).translateZ = 50;

    updateWorldTransforms(arena, root, true);

    CHECK(arena.common(root).isSpaceRoot == 1);
    CHECK(approxEqual(arena.common(root).world, Mat2x3{}));  // Root itself is an ordinary 2D node.
    CHECK(arena.common(layer).isSpaceRoot == 0);
    // local3 for a pure translateZ = 50, no other transform: world3 must place z=50, x=y=0
    // unmoved.
    Vec3 p = mat4Transform(arena.common(layer).world3, Vec3{0, 0, 0});
    CHECK(p.x == doctest::Approx(0.0f));
    CHECK(p.y == doctest::Approx(0.0f));
    CHECK(p.z == doctest::Approx(50.0f));
    // The layer's own `world` (2D) is identity -- it is its own plane's origin (§19.5).
    CHECK(approxEqual(arena.common(layer).world, Mat2x3{}));
}

TEST_CASE("updateWorldTransforms: nested 3D layers compose world3 = parent.world3 * local3") {
    NodeArena arena;
    NodeId root = arena.create(NodeKind::Group);
    NodeId cam = arena.create(NodeKind::Camera);
    arena.appendChild(root, cam);
    NodeId outer = arena.create(NodeKind::Group);
    arena.appendChild(root, outer);
    arena.common(outer).is3D = 1;
    arena.common(outer).translateZ = 10;
    NodeId inner = arena.create(NodeKind::Group);
    arena.appendChild(outer, inner);
    arena.common(inner).is3D = 1;
    arena.common(inner).translateZ = 20;

    updateWorldTransforms(arena, root, true);

    // outer places the origin at z=10; inner (nested under outer, not a root) composes on top,
    // so its world3 places the local origin at z = 10 + 20 = 30.
    Vec3 p = mat4Transform(arena.common(inner).world3, Vec3{0, 0, 0});
    CHECK(p.x == doctest::Approx(0.0f));
    CHECK(p.y == doctest::Approx(0.0f));
    CHECK(p.z == doctest::Approx(30.0f));
}

TEST_CASE("updateWorldTransforms: a 2D node under a 3D layer gets a plane-relative world") {
    // root(camera) -> layer(is3D) -> child(Group, translate 5,7) -- child is a plain 2D node on
    // the layer's own plane, so its `world` is relative to the layer's plane origin, not the
    // absolute-through-the-3D-transform position.
    NodeArena arena;
    NodeId root = arena.create(NodeKind::Group);
    NodeId cam = arena.create(NodeKind::Camera);
    arena.appendChild(root, cam);
    NodeId layer = arena.create(NodeKind::Group);
    arena.appendChild(root, layer);
    arena.common(layer).is3D = 1;
    arena.common(layer).translateZ = 100;
    arena.common(layer).rotationY = 45;  // Any nonzero 3D transform: must not leak into child.world.
    NodeId child = arena.create(NodeKind::Group);
    arena.appendChild(layer, child);
    arena.common(child).translate = Vec2{5, 7};

    updateWorldTransforms(arena, root, true);

    Vec2 p = apply(arena.common(child).world, Vec2{0, 0});
    CHECK(p.x == doctest::Approx(5.0f));
    CHECK(p.y == doctest::Approx(7.0f));
}

TEST_CASE("updateWorldTransforms: no 3D content anywhere -- legacy behavior is unchanged") {
    Chain c = buildChain();
    updateWorldTransforms(c.arena, c.root, true);
    CHECK(c.arena.common(c.root).isSpaceRoot == 0);
    CHECK(c.arena.common(c.a).isSpaceRoot == 0);
    CHECK(c.arena.common(c.b).isSpaceRoot == 0);
    Vec2 p = apply(c.arena.common(c.b).world, Vec2{0, 0});
    CHECK(p.x == doctest::Approx(10.0f));
    CHECK(p.y == doctest::Approx(1.0f));
}

TEST_CASE("updateWorldTransforms: force=true recomputes every node regardless of dirty bits") {
    Chain c = buildChain();
    updateWorldTransforms(c.arena, c.root, true);
    c.arena.clearAllDirty();

    c.arena.common(c.root).translate = Vec2{20, 0};
    // No markDirty call: force=true must still pick this up.
    updateWorldTransforms(c.arena, c.root, true);

    Vec2 p = apply(c.arena.common(c.root).world, Vec2{0, 0});
    CHECK(p.x == doctest::Approx(20.0f));
    CHECK(p.y == doctest::Approx(0.0f));
}
