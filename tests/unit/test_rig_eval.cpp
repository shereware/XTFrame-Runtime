#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "rig_eval.h"  // core/src (tests/CMakeLists.txt adds it to the include path).
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/painter.h"
#include "xt/perf.h"
#include "xt/property.h"
#include "xt/rig.h"
#include "xt/scene_instance.h"
#include "xt/types.h"
#include "xt/value.h"

/// T-244 (docs/design/32-rigging-and-deformation.md §32.2.3, §32.3, §32.8): bone world transforms,
/// the IK / Transform / Rotation / Spring constraints, LBS skinning, the Painter's skinning stage,
/// the skinned-geometry cache, `world-anchor` on bones and the `<id>.world` / `<id>.angle` ports.

using namespace xt;
using namespace xt::compile;

namespace {

constexpr float kEps = 1e-3f;

/// An arena-built rig: scene root Group -> skeleton Group (+ any other nodes).
struct Rig {
    NodeArena arena;
    NodeId root = kInvalidNode;
    NodeId skel = kInvalidNode;
    rig::RigState rs;

    Rig() {
        root = arena.create(NodeKind::Group);
        skel = arena.create(NodeKind::Group);
        arena.appendChild(root, skel);
    }
    NodeId bone(NodeId parent, float length, Vec2 translate = {}, float rotate = 0) {
        const NodeId b = arena.create(NodeKind::Bone);
        arena.appendChild(parent, b);
        arena.bone(b).length = length;
        arena.common(b).translate = translate;
        arena.common(b).rotation = rotate;
        return b;
    }
    NodeId group(NodeId parent, Vec2 translate, float rotate = 0, Vec2 scale = {1, 1}) {
        const NodeId g = arena.create(NodeKind::Group);
        arena.appendChild(parent, g);
        arena.common(g).translate = translate;
        arena.common(g).rotation = rotate;
        arena.common(g).scale = scale;
        return g;
    }
    NodeId constraint(NodeId bone, ConstraintKind kind, NodeId target, std::initializer_list<float> params,
                      float mix = 1, uint8_t space = 0) {
        const NodeId c = arena.create(NodeKind::Constraint);
        arena.appendChild(bone, c);
        ConstraintNodeData& d = arena.constraint(c);
        d.kind = kind;
        d.target = target;
        d.mix = mix;
        d.space = space;
        int i = 0;
        for (float p : params) {
            d.params[i++] = p;
        }
        return c;
    }
    /// Recomputes the arena worlds (constraint targets), rebuilds the rig over every node, evaluates.
    void build() {
        updateWorldTransforms(arena, root, true);
        std::vector<NodeId> all;
        arena.traversePreorder(root, [&](NodeId n) {
            all.push_back(n);
            return true;
        });
        rig::buildRigState(arena, skel, all, rs);
    }
    void eval(float dt = 0) {
        updateWorldTransforms(arena, root, true);
        rig::evaluate(rs, arena, dt);
    }
    Mat2x3 world(NodeId bone) const { return rs.world[static_cast<size_t>(rig::boneIndex(rs, bone))]; }
    float rotation(NodeId bone) const {
        return rs.pose[static_cast<size_t>(rig::boneIndex(rs, bone))].rotation;
    }
    Vec2 tip(NodeId bone) const { return apply(world(bone), Vec2{arena.bone(bone).length, 0}); }
    Vec2 origin(NodeId bone) const {
        const Mat2x3 w = world(bone);
        return Vec2{w.e, w.f};
    }
};

float dist(Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

/// Whole weight to tendon `t`.
PointWeight whole(uint32_t t) { return PointWeight{t, 255u}; }

/// A skinned path node under `parent`: verbs / points, weights, tendons with identity inverse binds.
NodeId skinnedPath(Rig& r, NodeId parent, std::vector<PathVerb> verbs, std::vector<float> points,
                   std::vector<NodeId> bones, std::vector<PointWeight> weights) {
    const NodeId p = r.arena.create(NodeKind::Path);
    r.arena.appendChild(parent, p);
    PathNodeData& d = r.arena.path(p);
    d.verbs = std::move(verbs);
    d.points = std::move(points);
    d.stroke.kind = PaintKind::Solid;
    d.stroke.color = Color{1, 0, 0, 1};
    d.strokeStyle.width = 2;
    d.skin.skeleton = r.skel;
    for (NodeId b : bones) {
        d.skin.tendons.push_back(Tendon{b, Mat2x3{}});
    }
    d.skin.weights = std::move(weights);
    return p;
}

CompileResult compileXt(const std::string& head, const std::string& body) {
    CompileOptions options;
    options.emitMeta = false;
    const std::string xt =
        "<xt><head><style>" + head + "</style></head><body id=\"Root\">" + body + "</body></xt>";
    std::vector<SourceFile> files{{"main.xt", xt}};
    return compileDocumentFromStrings(files, "main.xt", options);
}

struct Loaded {
    CompileResult result;
    std::unique_ptr<Document> doc;
    NodeId node(std::string_view name) const {
        const NodeArena& arena = doc->arena();
        NodeId found = kInvalidNode;
        arena.traversePreorder(doc->byId("Root").node, [&](NodeId n) {
            if (arena.name(n) != kInvalidString && doc->string(arena.name(n)) == name) {
                found = n;
            }
            return true;
        });
        return found;
    }
};

std::unique_ptr<Loaded> load(const std::string& head, const std::string& body) {
    auto l = std::make_unique<Loaded>();
    l->result = compileXt(head, body);
    for (const Diagnostic& d : l->result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(l->result.ok());
    REQUIRE(Document::load(IrBlob{l->result.ir.data(), l->result.ir.size()}, false, nullptr, nullptr,
                           l->doc) == Status::kOk);
    l->doc->setViewport(Rect{0, 0, 400, 300});
    return l;
}

/// The FK + constraint world of `bone` from a fresh rig over `doc`'s skeleton `skel`.
rig::RigState evalRig(Document& doc, NodeId skel) {
    std::vector<NodeId> none;
    rig::RigState rs;
    rig::buildRigState(doc.arena(), skel, none, rs);
    rig::evaluate(rs, doc.arena(), 0);
    return rs;
}

}  // namespace

// -- Bone world ---------------------------------------------------------------------------------------

TEST_CASE("rig eval: a two-bone chain's FK worlds by hand; a child's translate is (parent length, 0)") {
    Rig r;
    const NodeId a = r.bone(r.skel, 50, Vec2{10, 20}, 90);
    const NodeId b = r.bone(a, 30, Vec2{5, 3}, -90);  // translate ignored: (50, 0).
    r.build();
    REQUIRE(r.rs.bones.size() == 2);
    CHECK(r.rs.bones[0] == a);
    CHECK(r.rs.parent[1] == 0);
    CHECK(rig::evaluate(r.rs, r.arena, 0) == 2u);
    const Mat2x3 wa = r.world(a);
    CHECK(wa.a == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(wa.b == doctest::Approx(1.0f));
    CHECK(wa.c == doctest::Approx(-1.0f));
    CHECK(wa.e == doctest::Approx(10.0f));
    CHECK(wa.f == doctest::Approx(20.0f));
    const Mat2x3 wb = r.world(b);
    CHECK(wb.a == doctest::Approx(1.0f));
    CHECK(std::fabs(wb.b) < 1e-5f);
    CHECK(wb.e == doctest::Approx(10.0f));  // (10, 20) + R90 (50, 0).
    CHECK(wb.f == doctest::Approx(70.0f));
    CHECK(r.tip(b).x == doctest::Approx(40.0f));
    CHECK(r.tip(b).y == doctest::Approx(70.0f));
    // A skeleton group transform is not part of the bone worlds (root bones relative to it).
    r.arena.common(r.skel).translate = Vec2{100, 100};
    r.eval();
    CHECK(r.origin(a).x == doctest::Approx(10.0f));
}

TEST_CASE("rig eval: version changes only when a world changes") {
    Rig r;
    const NodeId a = r.bone(r.skel, 50);
    r.build();
    r.eval();
    const uint32_t v1 = r.rs.version;
    r.eval();
    CHECK(r.rs.version == v1);
    r.arena.common(a).rotation = 10;
    r.eval();
    CHECK(r.rs.version == v1 + 1);
}

// -- IK ------------------------------------------------------------------------------------------------

TEST_CASE("rig eval: 2-bone IK reaches a reachable target, both bend directions") {
    for (int invert = 0; invert < 2; ++invert) {
        CAPTURE(invert);
        Rig r;
        const NodeId a = r.bone(r.skel, 50);
        const NodeId b = r.bone(a, 50);
        const NodeId hand = r.group(r.root, Vec2{70, 0});
        r.constraint(b, ConstraintKind::Ik, hand, {1, static_cast<float>(invert), 2});
        r.build();
        r.eval();
        CHECK(dist(r.tip(b), Vec2{70, 0}) < kEps);
        // invert 0 bends clockwise first (joint below the line on a y-down screen), 1 the other way.
        const Vec2 joint = r.origin(b);
        CHECK(std::fabs(dist(joint, Vec2{0, 0}) - 50.0f) < kEps);
        if (invert == 0) {
            CHECK(joint.y > 10.0f);
        } else {
            CHECK(joint.y < -10.0f);
        }
        CHECK(joint.x == doctest::Approx(35.0f).epsilon(1e-3));
    }
}

TEST_CASE("rig eval: 2-bone IK straightens toward an unreachable target") {
    Rig r;
    const NodeId a = r.bone(r.skel, 50, Vec2{0, 0}, 30);
    const NodeId b = r.bone(a, 40, Vec2{}, -60);
    const NodeId hand = r.group(r.root, Vec2{0, 300});
    r.constraint(b, ConstraintKind::Ik, hand, {1, 0, 2});
    r.build();
    r.eval();
    CHECK(r.origin(b).x == doctest::Approx(0.0f).epsilon(1e-3));
    CHECK(r.origin(b).y == doctest::Approx(50.0f).epsilon(1e-3));
    CHECK(r.tip(b).y == doctest::Approx(90.0f).epsilon(1e-3));
    CHECK(std::fabs(r.tip(b).x) < kEps);
}

TEST_CASE("rig eval: IK strength 0.5 is the shortest-angle midpoint of FK and IK") {
    Rig full;
    const NodeId a1 = full.bone(full.skel, 50, Vec2{}, 10);
    const NodeId b1 = full.bone(a1, 50, Vec2{}, 20);
    const NodeId h1 = full.group(full.root, Vec2{20, 60});
    full.constraint(b1, ConstraintKind::Ik, h1, {1, 0, 2});
    full.build();
    full.eval();

    Rig half;
    const NodeId a2 = half.bone(half.skel, 50, Vec2{}, 10);
    const NodeId b2 = half.bone(a2, 50, Vec2{}, 20);
    const NodeId h2 = half.group(half.root, Vec2{20, 60});
    half.constraint(b2, ConstraintKind::Ik, h2, {0.5f, 0, 2});
    half.build();
    half.eval();
    auto wrap = [](float d) { return std::remainder(d, 360.0f); };
    CHECK(wrap(half.rotation(a2) - (10 + 0.5f * wrap(full.rotation(a1) - 10))) ==
          doctest::Approx(0.0f).epsilon(1e-3));
    CHECK(wrap(half.rotation(b2) - (20 + 0.5f * wrap(full.rotation(b1) - 20))) ==
          doctest::Approx(0.0f).epsilon(1e-3));
    CHECK(dist(half.tip(b2), Vec2{20, 60}) > 1.0f);  // Half way: not at the target.
}

TEST_CASE("rig eval: 1-bone IK aims; a 3-bone chain reaches the target") {
    Rig one;
    const NodeId only = one.bone(one.skel, 40);
    const NodeId t1 = one.group(one.root, Vec2{0, 100});
    one.constraint(only, ConstraintKind::Ik, t1, {1, 0, 1});
    one.build();
    one.eval();
    CHECK(one.rotation(only) == doctest::Approx(90.0f));

    Rig r;
    const NodeId a = r.bone(r.skel, 30);
    const NodeId b = r.bone(a, 30, Vec2{}, 20);
    const NodeId c = r.bone(b, 30, Vec2{}, 20);
    const NodeId hand = r.group(r.root, Vec2{50, 40});
    r.constraint(c, ConstraintKind::Ik, hand, {1, 0, 3});
    r.build();
    r.eval();
    CHECK(dist(r.tip(c), Vec2{50, 40}) < 0.01f);
    // Segment lengths are kept.
    CHECK(dist(r.origin(a), r.origin(b)) == doctest::Approx(30.0f));
    CHECK(dist(r.origin(b), r.origin(c)) == doctest::Approx(30.0f));
}

// -- Transform / Rotation copy -----------------------------------------------------------------------

TEST_CASE("rig eval: Transform copy, local and world, with mix") {
    {
        Rig r;
        const NodeId a = r.bone(r.skel, 10);
        const NodeId g = r.group(r.root, Vec2{40, 20}, 90, Vec2{2, 2});
        r.constraint(a, ConstraintKind::Transform, g, {}, 0.5f, /*local*/ 0);
        r.build();
        r.eval();
        const rig::BonePose& p = r.rs.pose[0];
        CHECK(p.translate.x == doctest::Approx(20.0f));
        CHECK(p.translate.y == doctest::Approx(10.0f));
        CHECK(p.rotation == doctest::Approx(45.0f));
        CHECK(p.scale.x == doctest::Approx(1.5f));
    }
    for (float mix : {1.0f, 0.5f}) {
        CAPTURE(mix);
        Rig r;
        r.arena.common(r.skel).translate = Vec2{5, 0};
        const NodeId a = r.bone(r.skel, 10);
        const NodeId g = r.group(r.root, Vec2{45, 20}, 90);  // (40, 20) in the skeleton's space.
        r.constraint(a, ConstraintKind::Transform, g, {}, mix, /*world*/ 1);
        r.build();
        r.eval();
        CHECK(r.origin(a).x == doctest::Approx(40.0f * mix));
        CHECK(r.origin(a).y == doctest::Approx(20.0f * mix));
        CHECK(std::atan2(r.world(a).b, r.world(a).a) * 57.2957795f == doctest::Approx(90.0f * mix));
    }
}

TEST_CASE("rig eval: Rotation copy copies only the rotation") {
    Rig r;
    const NodeId a = r.bone(r.skel, 10, Vec2{3, 4});
    const NodeId g = r.group(r.root, Vec2{40, 20}, -60);
    r.constraint(a, ConstraintKind::Rotation, g, {}, 0.5f, 0);
    r.build();
    r.eval();
    CHECK(r.rs.pose[0].rotation == doctest::Approx(-30.0f));
    CHECK(r.rs.pose[0].translate.x == doctest::Approx(3.0f));
}

// -- Spring --------------------------------------------------------------------------------------------

TEST_CASE("rig eval: a spring at rest stays at rest; a step displacement overshoots and decays") {
    Rig r;
    const NodeId a = r.bone(r.skel, 20);
    r.constraint(a, ConstraintKind::Spring, kInvalidNode, {1, 60, 8, 0});
    r.build();
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 30; ++i) {
        r.eval(dt);
    }
    CHECK(r.origin(a).x == doctest::Approx(0.0f));
    CHECK(r.rs.springs[0].offset.x == 0.0f);

    r.arena.common(a).translate = Vec2{100, 0};  // The parent chain jumps.
    float minOffset = 0, maxAfter = 0;
    bool wentNegative = false;
    for (int i = 0; i < 180; ++i) {
        r.eval(dt);
        const float off = r.rs.springs[0].offset.x;
        minOffset = std::min(minOffset, off);
        wentNegative = wentNegative || off < -50.0f;
        if (wentNegative) {
            maxAfter = std::max(maxAfter, off);
        }
    }
    CHECK(minOffset < -50.0f);                          // It lagged behind (inertia 1).
    CHECK(maxAfter > 1.0f);                             // ... overshot past the anchor ...
    CHECK(std::fabs(r.rs.springs[0].offset.x) < 0.5f);  // ... and damped out after 3 s.
    CHECK(std::fabs(r.origin(a).x - 100.0f) < 0.5f);

    rig::resetSprings(r.rs);
    CHECK(r.rs.springs[0].primed == false);
}

TEST_CASE("rig eval: spring steps are fixed (10 ms and 50 ms frames agree; at most 4 steps a frame)") {
    auto run = [](float dt, int frames) {
        Rig r;
        const NodeId a = r.bone(r.skel, 20);
        r.constraint(a, ConstraintKind::Spring, kInvalidNode, {1, 60, 8, 30});
        r.build();
        for (int i = 0; i < frames; ++i) {
            r.eval(dt);
        }
        return r.rs.springs[0];
    };
    const rig::SpringState s10 = run(0.01f, 100);
    const rig::SpringState s50 = run(0.05f, 20);
    CHECK(s10.offset.y > 0.1f);  // Gravity pulled it down.
    CHECK(s10.offset.y == doctest::Approx(s50.offset.y).epsilon(1e-4));
    CHECK(s10.angle == doctest::Approx(s50.angle).epsilon(1e-4));
    // 100 ms frames: 6 steps owed, 4 run, the rest dropped -> 40 steps over 10 frames, not 60.
    const rig::SpringState s100 = run(0.1f, 10);
    CHECK(s100.offset.y != doctest::Approx(s10.offset.y).epsilon(1e-4));
    CHECK(s100.accumulator == 0.0f);
}

// -- Skinning ------------------------------------------------------------------------------------------

TEST_CASE("rig eval: LBS of a path -- one bone rotated 90 degrees moves vertices and handles") {
    Rig r;
    const NodeId a = r.bone(r.skel, 10);
    // M(10,0) C(20,0 30,10 40,10): vertex 0 (p), vertex 1 (in = 30,10; p = 40,10), vertex 0's out = 20,0.
    const NodeId p = skinnedPath(r, r.root, {PathVerb::Move, PathVerb::Cubic}, {10, 0, 20, 0, 30, 10, 40, 10},
                                 {a}, {whole(0), whole(0), whole(0), whole(0), whole(0), whole(0)});
    r.build();
    REQUIRE(r.rs.skins.size() == 1);
    r.arena.common(a).rotation = 90;
    r.eval();
    std::vector<float> out;
    REQUIRE(rig::skinPath(r.arena, r.rs, r.rs.skins[0], out));
    const std::vector<float> want{0, 10, 0, 20, -10, 30, -10, 40};  // (x, y) -> (-y, x).
    REQUIRE(out.size() == want.size());
    for (size_t i = 0; i < want.size(); ++i) {
        CHECK(out[i] == doctest::Approx(want[i]).epsilon(1e-4));
    }
    CHECK(r.arena.path(p).points[0] == 10.0f);  // PathNodeData::points untouched.
}

TEST_CASE("rig eval: LBS blends two bones 0.5 / 0.5 and gives a weight deficit to the heaviest bone") {
    Rig r;
    const NodeId a = r.bone(r.skel, 10);
    const NodeId b = r.bone(r.skel, 10);  // A second root bone.
    const PointWeight half{0u | (1u << 8), 128u | (127u << 8)};
    const PointWeight shortSum{0u | (1u << 8), 50u | (100u << 8)};  // 150 / 255.
    skinnedPath(r, r.root, {PathVerb::Move, PathVerb::Line}, {0, 0, 10, 0}, {a, b},
                {half, half, half, shortSum, shortSum, shortSum});
    r.build();
    r.arena.common(b).translate = Vec2{20, 0};
    r.eval();
    std::vector<float> out;
    REQUIRE(rig::skinPath(r.arena, r.rs, r.rs.skins[0], out));
    CHECK(out[0] == doctest::Approx(20.0f * 127.0f / 255.0f));  // The midpoint (8-bit weights).
    CHECK(out[1] == doctest::Approx(0.0f));
    // 50 + 100 = 150: the missing 105 go to b (100) -> a 50 / 255, b 205 / 255.
    CHECK(out[2] == doctest::Approx(10.0f + 20.0f * 205.0f / 255.0f));
}

TEST_CASE("rig eval: the cache -- a static frame skins nothing, a morph or a pose re-skins") {
    Rig r;
    const NodeId a = r.bone(r.skel, 10);
    const NodeId p = skinnedPath(r, r.root, {PathVerb::Move, PathVerb::Line}, {0, 0, 10, 0}, {a},
                                 {whole(0), whole(0), whole(0), whole(0), whole(0), whole(0)});
    r.build();
    r.arena.common(a).translate = Vec2{0, 5};
    r.eval();
    CHECK(rig::updateSkins(r.rs, r.arena) == 2u);
    r.arena.clearAllDirty();
    r.eval();
    CHECK(rig::updateSkins(r.rs, r.arena) == 0u);  // Nothing moved.
    // Morph + skin: `d` changes (setProperty marks kDirtyPaint), the skin follows.
    float pts[4] = {0, 0, 30, 0};
    setProperty(r.arena, p, prop::PathData, Value::ofPath(pts, 4));
    r.eval();
    CHECK(rig::updateSkins(r.rs, r.arena) == 2u);
    const SkinnedGeometry* g = r.arena.skinned(p);
    REQUIRE(g != nullptr);
    CHECK(g->points[2] == doctest::Approx(30.0f));
    CHECK(g->points[3] == doctest::Approx(5.0f));
    r.arena.clearAllDirty();
    r.arena.common(a).translate = Vec2{0, 7};  // A new pose.
    r.eval();
    CHECK(rig::updateSkins(r.rs, r.arena) == 2u);
    CHECK(r.arena.skinned(p)->points[3] == doctest::Approx(7.0f));
}

TEST_CASE("rig eval: the Painter skins before the modifiers (skin + trim), with the parent's world") {
    Rig r;
    r.arena.common(r.root).translate = Vec2{1000, 0};
    const NodeId a = r.bone(r.skel, 10);
    const NodeId grp = r.group(r.root, Vec2{});
    const NodeId p = skinnedPath(r, grp, {PathVerb::Move, PathVerb::Line}, {0, 0, 100, 0}, {a},
                                 {whole(0), whole(0), whole(0), whole(0), whole(0), whole(0)});
    r.arena.common(p).translate = Vec2{500, 500};  // Ignored: a skinned shape's own transform.
    const NodeId trim = r.arena.create(NodeKind::Modifier);
    r.arena.appendChild(grp, trim);
    r.arena.modifier(trim).kind = ModifierKind::Trim;
    r.arena.modifier(trim).params[0] = 0;
    r.arena.modifier(trim).params[1] = 0.5f;
    r.build();
    r.arena.common(a).translate = Vec2{0, 50};
    r.eval();
    rig::updateSkins(r.rs, r.arena);
    updateWorldTransforms(r.arena, r.root, true);
    Painter painter;
    CommandList out;
    painter.paint(r.arena, r.root, out);
    REQUIRE(out.strokes.size() == 1);
    const StrokeCmd& sc = out.strokes[0];
    CHECK(sc.xf.e == doctest::Approx(1000.0f));  // The group's world, not the path's.
    CHECK(sc.xf.f == doctest::Approx(0.0f));
    REQUIRE(sc.path.pointCount == 4);
    const float* q = out.points.data() + sc.path.pointOffset;
    CHECK(q[0] == doctest::Approx(0.0f));
    CHECK(q[1] == doctest::Approx(50.0f));
    CHECK(q[2] == doctest::Approx(50.0f));  // Trimmed to half of the skinned (0,50)-(100,50).
    CHECK(q[3] == doctest::Approx(50.0f));

    // Outside a modified group: the skinned points, drawn with the parent's world.
    r.arena.destroy(trim);
    updateWorldTransforms(r.arena, r.root, true);
    CommandList plain;
    painter.paint(r.arena, r.root, plain);
    REQUIRE(plain.strokes.size() == 1);
    CHECK(plain.strokes[0].xf.e == doctest::Approx(1000.0f));
    const float* q2 = plain.points.data() + plain.strokes[0].path.pointOffset;
    CHECK(q2[2] == doctest::Approx(100.0f));
    CHECK(q2[3] == doctest::Approx(50.0f));
}

TEST_CASE("rig eval: mesh positions are skinned and read back through skinnedMeshPositions") {
    Rig r;
    const NodeId a = r.bone(r.skel, 10);
    const NodeId m = r.arena.create(NodeKind::Mesh);
    r.arena.appendChild(r.root, m);
    MeshNodeData& md = r.arena.mesh(m);
    md.positions = {Vec2{0, 0}, Vec2{10, 0}, Vec2{0, 10}};
    md.uvs = {Vec2{0, 0}, Vec2{1, 0}, Vec2{0, 1}};
    md.indices = {0, 1, 2};
    md.skin.skeleton = r.skel;
    md.skin.tendons.push_back(Tendon{a, Mat2x3{}});
    md.skin.weights = {whole(0), whole(0), whole(0)};
    r.build();
    std::vector<Vec2> pos;
    CHECK_FALSE(rig::skinnedMeshPositions(r.arena, m, pos));
    r.arena.common(a).translate = Vec2{3, 4};
    r.eval();
    CHECK(rig::updateSkins(r.rs, r.arena) == 3u);
    REQUIRE(rig::skinnedMeshPositions(r.arena, m, pos));
    CHECK(pos[1].x == doctest::Approx(13.0f));
    CHECK(pos[1].y == doctest::Approx(4.0f));
    // The Painter's mesh branch (T-245) draws the skinned positions with the parent's world.
    r.arena.common(r.root).translate = Vec2{100, 0};
    r.arena.common(m).translate = Vec2{500, 500};  // Ignored: skinned.
    updateWorldTransforms(r.arena, r.root, true);
    Painter painter;
    CommandList out;
    painter.paint(r.arena, r.root, out);
    REQUIRE(out.imageMeshes.size() == 1);
    REQUIRE(out.meshVertices.size() == 3);
    CHECK(out.meshVertices[1].p.x == doctest::Approx(113.0f));
    CHECK(out.meshVertices[1].p.y == doctest::Approx(4.0f));
}

// -- Document: compile, update, paint, ports, world-anchor ------------------------------------------

namespace {

const char* kSvgStyle = "svg { position: absolute; left: 20px; top: 10px; width: 200px; height: 200px; }";

/// Two bones from (50, 100), a box over them (left half on A, right half on B), B bound to `rb`.
const char* kBoxDoc = R"X(<div port:rb="number"></div>
<svg id="S" viewBox="0 0 200 200" width="200" height="200">
  <path id="Box" d="M50 90 L150 90 L150 110 L50 110 Z" fill="#ff0000" xt-skin="Rig">
    <xt-weights bones="A B">0 0:1
1 1:1
2 1:1
3 0:1</xt-weights></path>
  <xt-skeleton id="Rig">
    <xt-bone id="A" length="50" translate="50 100"><xt-bone id="B" length="50" bind:rotate="rb"/></xt-bone>
  </xt-skeleton>
</svg>)X";

}  // namespace

TEST_CASE("rig eval: an IR-loaded skeleton drives the Painter's commands after Document::update") {
    auto l = load(kSvgStyle, kBoxDoc);
    Document& doc = *l->doc;
    doc.update(0.0f);
    REQUIRE(doc.setPort(doc.lookupPort("rb"), Value::ofFloat(90.0f)) == Status::kOk);
    doc.update(0.1f);
    CommandList out;
    doc.paint(out);
    REQUIRE(out.fills.size() == 1);
    const FillCmd& fc = out.fills[0];
    REQUIRE(fc.path.pointCount == 8);
    const float* q = out.points.data() + fc.path.pointOffset;
    // B at (100, 100) rotated 90: (150, 90) -> (110, 150), (150, 110) -> (90, 150).
    const float want[8] = {50, 90, 110, 150, 90, 150, 50, 110};
    for (int i = 0; i < 8; ++i) {
        CHECK(q[i] == doctest::Approx(want[i]).epsilon(1e-4));
    }
    const NodeId box = l->node("Box");
    REQUIRE(box != kInvalidNode);
    const Mat2x3& parentWorld = doc.arena().common(doc.arena().parent(box)).world;
    CHECK(fc.xf.e == doctest::Approx(parentWorld.e));
    CHECK(fc.xf.f == doctest::Approx(parentWorld.f));
}

TEST_CASE("rig eval: perf counters -- skinnedPoints is 0 on a frame where nothing changed") {
    auto l = load(kSvgStyle, kBoxDoc);
    Document& doc = *l->doc;
    PerfTracker perf;
    perf.setEnabled(true);
    doc.setPerfTracker(&perf);
    perf.beginFrame(0);
    doc.update(0.0f);
    CHECK(perf.current().rigBones == 2u);
    CHECK(perf.current().skinnedPoints == 4u);
    perf.beginFrame(0.1f);
    doc.update(0.1f);
    CHECK(perf.current().rigBones == 2u);
    CHECK(perf.current().skinnedPoints == 0u);
    perf.beginFrame(0.2f);
    REQUIRE(doc.setPort(doc.lookupPort("rb"), Value::ofFloat(30.0f)) == Status::kOk);
    doc.update(0.2f);
    CHECK(perf.current().skinnedPoints == 4u);
    doc.setPerfTracker(nullptr);
}

TEST_CASE("rig eval: <id>.world / <id>.angle report the bone in screen space, notifying only on change") {
    auto l = load(kSvgStyle, kBoxDoc);
    Document& doc = *l->doc;
    const PortId world = doc.lookupPort("B.world");
    const PortId angle = doc.lookupPort("B.angle");
    REQUIRE(world != kInvalidPort);
    REQUIRE(angle != kInvalidPort);
    CHECK(doc.portInfo(world).dir == PortDir::Out);
    CHECK(doc.portInfo(world).type == PortValueType::Vec2);
    int worldEvents = 0, angleEvents = 0;
    doc.addPortListener([&](const PortEvent& e) {
        worldEvents += e.port == world ? 1 : 0;
        angleEvents += e.port == angle ? 1 : 0;
    });
    doc.update(0.0f);
    const NodeId skel = l->node("Rig");
    REQUIRE(skel != kInvalidNode);
    const Mat2x3& group = doc.arena().common(skel).world;
    const Vec2 want = apply(group, Vec2{100, 100});
    CHECK(doc.getPort(world).v2.x == doctest::Approx(want.x));
    CHECK(doc.getPort(world).v2.y == doctest::Approx(want.y));
    CHECK(worldEvents == 1);
    CHECK(angleEvents == 0);  // Still 0 degrees.
    doc.update(0.1f);
    doc.update(0.2f);
    CHECK(worldEvents == 1);  // Unchanged: no notification.
    REQUIRE(doc.setPort(doc.lookupPort("rb"), Value::ofFloat(45.0f)) == Status::kOk);
    doc.update(0.3f);
    CHECK(angleEvents == 1);
    CHECK(worldEvents == 1);  // B's origin did not move.
    CHECK(doc.getPort(angle).f == doctest::Approx(45.0f));
}

TEST_CASE("rig eval: world-anchor on a root bone writes its translate, on a child bone a 2-bone IK") {
    const std::string body = R"X(<div port:p="vec2" port:q="vec2"></div>
<svg id="S" viewBox="0 0 200 200" width="200" height="200">
  <xt-skeleton id="Rig">
    <xt-bone id="A" length="50" translate="50 100" world-anchor="p">
      <xt-bone id="B" length="40" rotate="-30"><xt-bone id="C" length="40" rotate="-40" world-anchor="q"/></xt-bone>
    </xt-bone>
  </xt-skeleton>
</svg>)X";
    auto l = load(kSvgStyle, body);
    Document& doc = *l->doc;
    doc.update(0.0f);
    const NodeId skel = l->node("Rig");
    const NodeId a = l->node("A");
    const NodeId c = l->node("C");
    REQUIRE(c != kInvalidNode);
    const Mat2x3 group = doc.arena().common(skel).world;
    // Root: the screen point mapped into the skeleton group's space.
    const Vec2 screenA = apply(group, Vec2{60, 110});
    REQUIRE(doc.setPort(doc.lookupPort("p"), Value::ofVec2(screenA)) == Status::kOk);
    doc.update(0.1f);
    doc.update(0.2f);
    CHECK(doc.arena().common(a).translate.x == doctest::Approx(60.0f).epsilon(1e-4));
    CHECK(doc.arena().common(a).translate.y == doctest::Approx(110.0f).epsilon(1e-4));
    // Non-root: B and C rotate so that C's tip lands on the point (bending as before).
    const Vec2 screenC = apply(group, Vec2{150, 90});
    REQUIRE(doc.setPort(doc.lookupPort("q"), Value::ofVec2(screenC)) == Status::kOk);
    doc.update(0.3f);
    doc.update(0.4f);
    const rig::RigState rs = evalRig(doc, skel);
    const Mat2x3 wc = rs.world[static_cast<size_t>(rig::boneIndex(rs, c))];
    const Vec2 tip = apply(wc, Vec2{40, 0});
    CHECK(dist(tip, Vec2{150, 90}) < 0.01f);
    CHECK(doc.arena().common(c).rotation < 0.0f);  // Still bending the same way.
}

// -- T-243b: inverse bind with the shape's own transform; bone ports read by bind: ---------------

TEST_CASE("rig eval: a skinned shape with its own transform draws, at rest, where an unskinned one does") {
    const std::string body = R"X(<svg id="S" viewBox="0 0 200 200" width="200" height="200">
  <path id="Box" d="M0 90 L100 90 L100 110 L0 110 Z" transform="translate(10 0)" fill="#ff0000" xt-skin="Rig">
    <xt-weights bones="A B">0 0:1
1 1:1
2 1:1
3 0:1</xt-weights></path>
  <xt-skeleton id="Rig">
    <xt-bone id="A" length="50" translate="10 100"><xt-bone id="B" length="50"/></xt-bone>
  </xt-skeleton>
</svg>)X";
    auto l = load(kSvgStyle, body);
    Document& doc = *l->doc;
    doc.update(0.0f);
    CommandList out;
    doc.paint(out);
    REQUIRE(out.fills.size() == 1);
    const FillCmd& fc = out.fills[0];
    REQUIRE(fc.path.pointCount == 8);
    const float* q = out.points.data() + fc.path.pointOffset;
    // own x local point: the d points moved 10 to the right.
    const float want[8] = {10, 90, 110, 90, 110, 110, 10, 110};
    for (int i = 0; i < 8; ++i) {
        CAPTURE(i);
        CHECK(q[i] == doctest::Approx(want[i]).epsilon(1e-4));
    }
    const NodeId box = l->node("Box");
    REQUIRE(box != kInvalidNode);
    const Mat2x3& parentWorld = doc.arena().common(doc.arena().parent(box)).world;
    CHECK(fc.xf.e == doctest::Approx(parentWorld.e));
    CHECK(fc.xf.f == doctest::Approx(parentWorld.f));
}

TEST_CASE("rig eval: bind: reads a bone port through a converter into another element (T-243b)") {
    const std::string body = std::string(kBoxDoc) +
                             R"X(<div id="T" style="position: absolute; width: 4px; height: 4px"
     bind:translate-x="B.world | x" bind:translate-y="B.angle"></div>)X";
    auto l = load(kSvgStyle, body);
    Document& doc = *l->doc;
    doc.update(0.0f);
    doc.update(0.1f);
    const PortId world = doc.lookupPort("B.world");
    REQUIRE(world != kInvalidPort);
    const NodeId t = doc.byId("T").node;
    REQUIRE(t != kInvalidNode);
    const Vec2 b = doc.getPort(world).v2;
    CHECK(b.x != doctest::Approx(0.0f));
    CHECK(getProperty(doc.arena(), t, prop::TranslateX).f == doctest::Approx(b.x));
    CHECK(getProperty(doc.arena(), t, prop::TranslateY).f == doctest::Approx(0.0f));
    REQUIRE(doc.setPort(doc.lookupPort("rb"), Value::ofFloat(30.0f)) == Status::kOk);
    doc.update(0.2f);
    doc.update(0.3f);
    CHECK(getProperty(doc.arena(), t, prop::TranslateY).f == doctest::Approx(30.0f));
}

// -- Compile side (item 0) ---------------------------------------------------------------------------

TEST_CASE("rig eval: <xt-constraint kind=spring> defaults; skin.boneOffset also for y") {
    auto l = load("", R"X(<svg id="S" viewBox="0 0 10 10"><xt-skeleton id="Rig"><xt-bone id="A" length="4">
      <xt-constraint id="Sp" kind="spring"/><xt-bone id="B" length="2" translate="4 3"/></xt-bone></xt-skeleton></svg>)X");
    bool warned = false;
    for (const Diagnostic& d : l->result.diagnostics) {
        warned = warned || d.message.find("skin.boneOffset") != std::string::npos;
    }
    CHECK(warned);  // x matches the parent length; y does not.
    const NodeId sp = l->node("Sp");
    REQUIRE(sp != kInvalidNode);
    const ConstraintNodeData& c = l->doc->arena().constraint(sp);
    CHECK(c.params[0] == doctest::Approx(1.0f));
    CHECK(c.params[1] == doctest::Approx(60.0f));
    CHECK(c.params[2] == doctest::Approx(8.0f));
    CHECK(c.params[3] == doctest::Approx(0.0f));
    CHECK(c.mix == doctest::Approx(1.0f));
}

// -- T-243c: the public RigView ------------------------------------------------------------------------

TEST_CASE("rig eval: Document::rigAt is the last update's pose, constraints and spring offsets included") {
    auto l = load(kSvgStyle, R"X(<svg id="S" viewBox="0 0 200 200" width="200" height="200">
  <circle id="Hand" cx="0" cy="0" r="5" transform="translate(130 60)"/>
  <xt-skeleton id="Rig">
    <xt-bone id="A" length="50" translate="50 100">
      <xt-bone id="B" length="50"><xt-constraint id="Ik" kind="ik" target="#Hand" invert="1"/></xt-bone>
      <xt-bone id="C" length="20"><xt-constraint id="Sp" kind="spring" gravity="400"/></xt-bone>
    </xt-bone>
  </xt-skeleton>
</svg>)X");
    Document& doc = *l->doc;
    REQUIRE(doc.rigCount() == 1);
    CHECK(doc.rigAt(1).bones.empty());
    CHECK(doc.rigAt(1).skeleton == kInvalidNode);
    for (int frame = 0; frame <= 30; ++frame) {
        doc.update(static_cast<float>(frame) / 60.0f);
    }
    const RigView v = doc.rigAt(0);
    const NodeId skel = l->node("Rig");
    CHECK(v.skeleton == skel);
    REQUIRE(v.bones.size() == 3);
    CHECK(v.bones[0].node == l->node("A"));
    CHECK(v.bones[1].node == l->node("B"));
    CHECK(v.bones[2].node == l->node("C"));
    CHECK(v.bones[0].parent == -1);
    CHECK(v.bones[1].parent == 0);
    CHECK(v.bones[2].parent == 0);
    CHECK(v.bones[0].length == doctest::Approx(50.0f));
    CHECK(v.bones[2].length == doctest::Approx(20.0f));

    // IK: B's tip reaches the target; the gizmo has the target's origin in skeleton group space.
    REQUIRE(v.ik.size() == 1);
    CHECK(v.ik[0].constraint == l->node("Ik"));
    CHECK(v.ik[0].bone == 1);
    CHECK(v.ik[0].target == l->node("Hand"));
    CHECK(v.ik[0].invert);
    CHECK(v.ik[0].position.x == doctest::Approx(130.0f).epsilon(1e-4));
    CHECK(v.ik[0].position.y == doctest::Approx(60.0f).epsilon(1e-4));
    const Vec2 tip = apply(v.bones[1].world, Vec2{50, 0});
    CHECK(dist(tip, Vec2{130, 60}) < 0.05f);

    // Spring: C hangs below its rest pose (gravity), unlike a scratch dt-0 evaluation.
    REQUIRE(v.springBones.size() == 1);
    CHECK(v.springBones[0] == 2);
    const rig::RigState scratch = evalRig(doc, skel);
    CHECK(approxEqual(v.bones[0].world, scratch.world[0], 1e-4f));
    CHECK(approxEqual(v.bones[1].world, scratch.world[1], 1e-3f));
    const Vec2 sprung{v.bones[2].world.e, v.bones[2].world.f};
    const Vec2 rest{scratch.world[2].e, scratch.world[2].f};
    CHECK(sprung.y > rest.y + 0.5f);
}

// -- Perf (reported, not asserted) --------------------------------------------------------------------

TEST_CASE("rig eval: perf -- 1k cubic vertices, 32 bones, 4 IK") {
    Rig r;
    std::vector<NodeId> bones;
    for (int chain = 0; chain < 4; ++chain) {
        NodeId parent = r.skel;
        for (int i = 0; i < 8; ++i) {
            const NodeId b = r.bone(parent, 10, Vec2{static_cast<float>(chain * 50), 0}, 5);
            bones.push_back(b);
            parent = b;
        }
        const NodeId target = r.group(r.root, Vec2{static_cast<float>(chain * 50 + 30), 40});
        r.constraint(parent, ConstraintKind::Ik, target, {1, 0, 2});
    }
    // 1000 cubic vertices: Move + 999 Cubic -> 1 + 999 * 3 = 2998 points.
    std::vector<PathVerb> verbs{PathVerb::Move};
    std::vector<float> points{0, 0};
    for (int i = 1; i < 1000; ++i) {
        verbs.push_back(PathVerb::Cubic);
        const float x = static_cast<float>(i);
        points.insert(points.end(), {x - 0.6f, 1, x - 0.3f, 1, x, 0});
    }
    std::vector<PointWeight> weights;
    for (int i = 0; i < 1000 * 3; ++i) {
        const uint32_t b0 = static_cast<uint32_t>(i % 32);
        const uint32_t b1 = static_cast<uint32_t>((i + 7) % 32);
        weights.push_back(PointWeight{b0 | (b1 << 8), 170u | (85u << 8)});
    }
    skinnedPath(r, r.root, verbs, points, bones, weights);
    r.build();
    updateWorldTransforms(r.arena, r.root, true);
    const int frames = 200;
    const auto start = std::chrono::steady_clock::now();
    uint32_t skinned = 0;
    for (int i = 0; i < frames; ++i) {
        r.arena.common(bones[0]).rotation = static_cast<float>(i);  // Every frame moves.
        rig::evaluate(r.rs, r.arena, 1.0f / 60.0f);
        skinned += rig::updateSkins(r.rs, r.arena);
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / frames;
    CHECK(skinned == static_cast<uint32_t>(frames) * 2998u);
    std::printf("[T-244 perf] 1k cubic vertices (2998 points), 32 bones, 4 IK: %.4f ms/frame (rig + skin)\n",
                ms);
}
