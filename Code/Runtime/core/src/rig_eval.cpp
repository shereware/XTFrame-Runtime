#include "rig_eval.h"

#include <algorithm>
#include <cmath>

#include "xt/node.h"
#include "xt/num_guard.h"
#include "xt/property.h"
#include "xt/value.h"

/// See rig_eval.h. Docs/design/32-rigging-and-deformation.md §32.2.3, §32.3, §32.8; T-244.

namespace xt::rig {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kRadToDeg = 180.0f / kPi;
constexpr float kDegToRad = kPi / 180.0f;
/// Spring: fixed 60 Hz step, at most 4 steps per frame (§32.2.3).
constexpr float kSpringStep = 1.0f / 60.0f;
constexpr int kSpringMaxSteps = 4;
/// IK chains longer than 2: rounds of tip-to-root 2-bone solves (T-244 item 1).
constexpr int kIkRounds = 3;

/// `deg` wrapped into (-180, 180].
float wrapDeg(float deg) {
    deg = std::fmod(deg, 360.0f);
    if (deg > 180.0f) {
        deg -= 360.0f;
    } else if (deg <= -180.0f) {
        deg += 360.0f;
    }
    return deg;
}

float angleOf(Vec2 v) { return std::atan2(v.y, v.x) * kRadToDeg; }

float lengthOf(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }

Vec2 sub(Vec2 a, Vec2 b) { return Vec2{a.x - b.x, a.y - b.y}; }

Vec2 origin(const Mat2x3& m) { return Vec2{m.e, m.f}; }

/// T * R * S * T(-anchor) -- composeTRSS() without skew.
Mat2x3 poseMatrix(const BonePose& p) {
    const float rad = p.rotation * kDegToRad;
    const float c = std::cos(rad);
    const float s = std::sin(rad);
    Mat2x3 m;
    m.a = c * p.scale.x;
    m.b = s * p.scale.x;
    m.c = -s * p.scale.y;
    m.d = c * p.scale.y;
    m.e = p.translate.x - (m.a * p.anchor.x + m.c * p.anchor.y);
    m.f = p.translate.y - (m.b * p.anchor.x + m.d * p.anchor.y);
    return m;
}

/// The pose whose poseMatrix() is `m` (skew dropped), keeping `anchor`.
BonePose poseFromMatrix(const Mat2x3& m, Vec2 anchor) {
    BonePose p;
    p.anchor = anchor;
    p.rotation = std::atan2(m.b, m.a) * kRadToDeg;
    const float sx = std::sqrt(m.a * m.a + m.b * m.b);
    p.scale.x = sx;
    p.scale.y = sx > 1e-12f ? determinant(m) / sx : std::sqrt(m.c * m.c + m.d * m.d);
    p.translate = Vec2{m.e + m.a * anchor.x + m.c * anchor.y, m.f + m.b * anchor.x + m.d * anchor.y};
    return p;
}

float boneLength(const NodeArena& arena, NodeId bone) { return arena.bone(bone).length; }

/// The FK pose of bone `bone` from its properties; a child bone's translate is forced to (parent
/// length, 0) (§32.2.1, T-244 item 1a).
BonePose fkPose(const NodeArena& arena, NodeId bone) {
    const NodeCommon& c = arena.common(bone);
    BonePose p;
    p.rotation = c.rotation;
    p.scale = c.scale;
    p.anchor = c.anchor;
    const NodeId parent = arena.parent(bone);
    if (parent != kInvalidNode && arena.kind(parent) == NodeKind::Bone) {
        p.translate = Vec2{boneLength(arena, parent), 0.0f};
    } else {
        p.translate = c.translate;
    }
    return p;
}

Mat2x3 parentWorld(const RigState& rs, size_t i) {
    return rs.parent[i] >= 0 ? rs.world[static_cast<size_t>(rs.parent[i])] : Mat2x3{};
}

/// Recomputes world[begin, end) from the poses (parents before children; the parents of `begin`'s
/// subtree root are outside the range and already current).
void recomputeWorld(RigState& rs, size_t begin, size_t end) {
    for (size_t i = begin; i < end; ++i) {
        const Mat2x3 local = poseMatrix(rs.pose[i]);
        rs.world[i] = rs.parent[i] >= 0 ? mul(rs.world[static_cast<size_t>(rs.parent[i])], local) : local;
    }
}

void recomputeSubtree(RigState& rs, size_t i) { recomputeWorld(rs, i, rs.subtreeEnd[i]); }

/// The tip of bone `i` (its local (length, 0)) in group space.
Vec2 tipOf(const RigState& rs, const NodeArena& arena, size_t i) {
    return apply(rs.world[i], Vec2{boneLength(arena, rs.bones[i]), 0.0f});
}

/// Rotates bone `i`'s world direction by `deltaDeg` (group space) through its local rotation: a
/// mirrored parent space (negative determinant) turns the local rotation the other way.
void rotateBy(RigState& rs, size_t i, float deltaDeg) {
    const float sign = determinant(parentWorld(rs, i)) < 0.0f ? -1.0f : 1.0f;
    rs.pose[i].rotation += sign * wrapDeg(deltaDeg);
}

/// Rotates bone `i` so that the segment from its joint `j` to the effector `e` points at `target`.
void aimSegment(RigState& rs, size_t i, Vec2 j, Vec2 e, Vec2 target) {
    const Vec2 have = sub(e, j);
    const Vec2 want = sub(target, j);
    if (lengthOf(have) < 1e-6f || lengthOf(want) < 1e-6f) {
        return;
    }
    rotateBy(rs, i, angleOf(want) - angleOf(have));
    recomputeSubtree(rs, i);
}

/// The 2-bone solve (law of cosines): rotates `upper` and `lower` (the bone whose origin is the
/// joint) so that the tip of bone `effector` (`lower` itself, or the end of a longer chain held
/// rigid below it) reaches `target`. `invert` picks the bend direction; out of reach the two
/// straighten toward the target.
void solveTwo(RigState& rs, const NodeArena& arena, size_t upper, size_t lower, size_t effector, Vec2 target,
              bool invert) {
    const Vec2 a = origin(rs.world[upper]);
    const Vec2 j = origin(rs.world[lower]);
    const Vec2 e = tipOf(rs, arena, effector);
    const float l1 = lengthOf(sub(j, a));
    const float l2 = lengthOf(sub(e, j));
    const float d = lengthOf(sub(target, a));
    if (l1 < 1e-6f) {
        aimSegment(rs, lower, j, e, target);
        return;
    }
    if (d < 1e-6f) {
        return;  // The target sits on the chain's root: no direction to aim at.
    }
    float theta1 = angleOf(sub(target, a));
    if (l2 >= 1e-6f) {
        const float cosA = std::clamp((l1 * l1 + d * d - l2 * l2) / (2.0f * l1 * d), -1.0f, 1.0f);
        const float alpha = std::acos(cosA) * kRadToDeg;
        theta1 += invert ? -alpha : alpha;
    }
    rotateBy(rs, upper, theta1 - angleOf(sub(j, a)));
    recomputeSubtree(rs, upper);
    aimSegment(rs, lower, origin(rs.world[lower]), tipOf(rs, arena, effector), target);
}

/// `target`'s world matrix in the skeleton group's space: a bone of this rig from `world`, any
/// other node from the arena (its world relative to the skeleton group's world).
bool targetWorld(const RigState& rs, const NodeArena& arena, NodeId target, Mat2x3& out) {
    const int32_t idx = boneIndex(rs, target);
    if (idx >= 0) {
        out = rs.world[static_cast<size_t>(idx)];
        return true;
    }
    if (target == kInvalidNode || !arena.alive(target) || !arena.alive(rs.skeleton)) {
        return false;
    }
    Mat2x3 inv;
    if (!invert(arena.common(rs.skeleton).world, inv)) {
        return false;
    }
    out = mul(inv, arena.common(target).world);
    return true;
}

/// `target`'s local pose: a bone of this rig from `pose`, any other node from its properties.
bool targetPose(const RigState& rs, const NodeArena& arena, NodeId target, BonePose& out) {
    const int32_t idx = boneIndex(rs, target);
    if (idx >= 0) {
        out = rs.pose[static_cast<size_t>(idx)];
        return true;
    }
    if (target == kInvalidNode || !arena.alive(target)) {
        return false;
    }
    const NodeCommon& c = arena.common(target);
    out.translate = c.translate;
    out.rotation = c.rotation;
    out.scale = c.scale;
    out.anchor = c.anchor;
    return true;
}

float lerp(float a, float b, float t) { return a + (b - a) * t; }

Vec2 lerp(Vec2 a, Vec2 b, float t) { return Vec2{lerp(a.x, b.x, t), lerp(a.y, b.y, t)}; }

void applyIk(RigState& rs, const NodeArena& arena, size_t owner, const ConstraintNodeData& c) {
    Mat2x3 tw;
    if (!targetWorld(rs, arena, c.target, tw)) {
        return;
    }
    const Vec2 target = origin(tw);
    const long chainLen = std::max(1L, std::lround(c.params[2]));
    // The chain, root first: the owner bone and up to chainLen - 1 of its ancestor bones.
    size_t chain[64];
    size_t n = 0;
    for (int32_t i = static_cast<int32_t>(owner); i >= 0 && n < static_cast<size_t>(chainLen) && n < 64;
         i = rs.parent[static_cast<size_t>(i)]) {
        chain[n++] = static_cast<size_t>(i);
    }
    std::reverse(chain, chain + n);
    float fk[64];
    for (size_t k = 0; k < n; ++k) {
        fk[k] = rs.pose[chain[k]].rotation;
    }
    const bool invertBend = c.params[1] >= 0.5f;
    const size_t tip = chain[n - 1];
    if (n == 1) {
        aimSegment(rs, tip, origin(rs.world[tip]), tipOf(rs, arena, tip), target);
    } else if (n == 2) {
        solveTwo(rs, arena, chain[0], chain[1], tip, target, invertBend);
    } else {
        for (int round = 0; round < kIkRounds; ++round) {
            for (size_t k = n - 1; k >= 1; --k) {
                solveTwo(rs, arena, chain[k - 1], chain[k], tip, target, invertBend);
            }
        }
    }
    // strength (x mix): the shortest-angle blend of each chain bone's FK and IK rotation.
    const float strength = std::clamp(c.params[0], 0.0f, 1.0f) * std::clamp(c.mix, 0.0f, 1.0f);
    for (size_t k = 0; k < n; ++k) {
        const float ik = rs.pose[chain[k]].rotation;
        rs.pose[chain[k]].rotation = fk[k] + strength * wrapDeg(ik - fk[k]);
    }
    recomputeSubtree(rs, chain[0]);
}

/// Transform copy (`rotationOnly` = false) and Rotation copy (true).
void applyCopy(RigState& rs, const NodeArena& arena, size_t owner, const ConstraintNodeData& c,
               bool rotationOnly) {
    const float mix = std::clamp(c.mix, 0.0f, 1.0f);
    BonePose& pose = rs.pose[owner];
    if (c.space == 0) {  // local
        BonePose tp;
        if (!targetPose(rs, arena, c.target, tp)) {
            return;
        }
        pose.rotation += mix * wrapDeg(tp.rotation - pose.rotation);
        if (!rotationOnly) {
            pose.translate = lerp(pose.translate, tp.translate, mix);
            pose.scale = lerp(pose.scale, tp.scale, mix);
        }
        recomputeSubtree(rs, owner);
        return;
    }
    Mat2x3 tw;
    if (!targetWorld(rs, arena, c.target, tw)) {
        return;
    }
    const Mat2x3& w = rs.world[owner];
    if (rotationOnly) {
        rotateBy(rs, owner,
                 mix * wrapDeg(std::atan2(tw.b, tw.a) * kRadToDeg - std::atan2(w.b, w.a) * kRadToDeg));
        recomputeSubtree(rs, owner);
        return;
    }
    // World: translate / rotate (shortest) / scale interpolated separately, then back to local.
    const BonePose from = poseFromMatrix(w, Vec2{});
    const BonePose to = poseFromMatrix(tw, Vec2{});
    BonePose mixed;
    mixed.translate = lerp(from.translate, to.translate, mix);
    mixed.rotation = from.rotation + mix * wrapDeg(to.rotation - from.rotation);
    mixed.scale = lerp(from.scale, to.scale, mix);
    Mat2x3 invParent;
    if (!invert(parentWorld(rs, owner), invParent)) {
        return;
    }
    pose = poseFromMatrix(mul(invParent, poseMatrix(mixed)), pose.anchor);
    recomputeSubtree(rs, owner);
}

void applySpring(RigState& rs, const NodeArena& arena, size_t owner, const ConstraintNodeData& c,
                 SpringState& s, float dt) {
    const float inertia = c.params[0];
    const float stiffness = c.params[1];
    const float damping = c.params[2];
    const float gravity = c.params[3];
    const Mat2x3 pw = parentWorld(rs, owner);
    // The anchor: the bone's origin as the parent chain places it (before this spring).
    const Vec2 anchor = apply(pw, rs.pose[owner].translate);
    Vec2 dir = applyVector(rs.world[owner], Vec2{1.0f, 0.0f});
    const float dirLen = lengthOf(dir);
    dir = dirLen > 1e-6f ? Vec2{dir.x / dirLen, dir.y / dirLen} : Vec2{1.0f, 0.0f};
    const Vec2 perp{-dir.y, dir.x};  // The direction the tip moves for a positive angle.
    const float radius = std::max(boneLength(arena, rs.bones[owner]), 1.0f);

    if (!s.primed) {
        s.prevAnchor = anchor;
        s.parentVelocity = Vec2{};
        s.primed = true;
    } else if (dt > 0.0f) {
        // The parent chain's motion: its velocity change this frame is the `- inertia *
        // parentAccel` term integrated over the frame (an impulse, so it does not depend on how
        // the frame splits into steps).
        const Vec2 vel{(anchor.x - s.prevAnchor.x) / dt, (anchor.y - s.prevAnchor.y) / dt};
        const Vec2 dv = sub(vel, s.parentVelocity);
        s.velocity.x -= inertia * dv.x;
        s.velocity.y -= inertia * dv.y;
        s.angularVelocity -= inertia * (dv.x * perp.x + dv.y * perp.y) / radius;
        s.parentVelocity = vel;
        s.prevAnchor = anchor;
    }
    if (dt > 0.0f) {
        s.accumulator += dt;
    }
    int steps = toInt32Sat((s.accumulator + 1e-6f) / kSpringStep);  // T-258a: a huge dt saturates.
    if (steps > kSpringMaxSteps) {
        steps = kSpringMaxSteps;
        s.accumulator = 0.0f;  // The rest is dropped.
    } else {
        s.accumulator = std::max(0.0f, s.accumulator - static_cast<float>(steps) * kSpringStep);
    }
    const float gravityPerp = gravity * perp.y / radius;
    const float maxAngle = kPi * 0.5f;
    for (int i = 0; i < steps; ++i) {
        // a = -strength * offset - damping * velocity + gravity * (0, 1); semi-implicit Euler.
        const Vec2 acc{-stiffness * s.offset.x - damping * s.velocity.x,
                       -stiffness * s.offset.y - damping * s.velocity.y + gravity};
        s.velocity.x += acc.x * kSpringStep;
        s.velocity.y += acc.y * kSpringStep;
        s.offset.x += s.velocity.x * kSpringStep;
        s.offset.y += s.velocity.y * kSpringStep;
        const float alpha = -stiffness * s.angle - damping * s.angularVelocity + gravityPerp;
        s.angularVelocity += alpha * kSpringStep;
        s.angle += s.angularVelocity * kSpringStep;
        if (s.angle > maxAngle || s.angle < -maxAngle) {
            s.angle = std::clamp(s.angle, -maxAngle, maxAngle);
            s.angularVelocity = 0.0f;
        }
    }
    const float mix = c.mix;
    if (mix == 0.0f) {
        return;
    }
    Mat2x3 invParent;
    if (invert(pw, invParent)) {
        const Vec2 local = applyVector(invParent, s.offset);
        rs.pose[owner].translate.x += mix * local.x;
        rs.pose[owner].translate.y += mix * local.y;
    }
    rotateBy(rs, owner, mix * s.angle * kRadToDeg);
    recomputeSubtree(rs, owner);
}

/// The blend of the four skin matrices `pw` names, applied to `q`. A weight sum below 1 gives the
/// rest to the heaviest bone.
Vec2 blendPoint(std::span<const Mat2x3> mats, const PointWeight& pw, Vec2 q) {
    float w[4];
    uint32_t idx[4];
    float sum = 0.0f;
    int best = 0;
    for (int k = 0; k < 4; ++k) {
        idx[k] = (pw.idx4 >> (8 * k)) & 0xFFu;
        w[k] = static_cast<float>((pw.w4 >> (8 * k)) & 0xFFu) / 255.0f;
        sum += w[k];
        if (w[k] > w[best]) {
            best = k;
        }
    }
    if (sum < 1.0f) {
        w[best] += 1.0f - sum;
    }
    Mat2x3 m{0, 0, 0, 0, 0, 0};
    for (int k = 0; k < 4; ++k) {
        if (w[k] == 0.0f || idx[k] >= mats.size()) {
            continue;
        }
        const Mat2x3& s = mats[idx[k]];
        m.a += w[k] * s.a;
        m.b += w[k] * s.b;
        m.c += w[k] * s.c;
        m.d += w[k] * s.d;
        m.e += w[k] * s.e;
        m.f += w[k] * s.f;
    }
    return apply(m, q);
}

/// skin matrix[i] = world[tendon.bone] x tendon.inverseBind (a tendon whose bone is not in the
/// rig keeps its rest: identity).
void skinMatrices(const RigState& rs, const SkinData& skin, const SkinTarget& target,
                  std::vector<Mat2x3>& out) {
    out.resize(skin.tendons.size());
    for (size_t t = 0; t < skin.tendons.size(); ++t) {
        const int32_t b = t < target.tendonBone.size() ? target.tendonBone[t] : -1;
        out[t] = b >= 0 ? mul(rs.world[static_cast<size_t>(b)], skin.tendons[t].inverseBind) : Mat2x3{};
    }
}

}  // namespace

int32_t boneIndex(const RigState& rs, NodeId bone) {
    for (size_t i = 0; i < rs.bones.size(); ++i) {
        if (rs.bones[i] == bone) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

NodeId skeletonOf(const NodeArena& arena, NodeId bone) {
    NodeId n = bone;
    while (n != kInvalidNode && arena.alive(n) && arena.kind(n) == NodeKind::Bone) {
        n = arena.parent(n);
    }
    return n != bone ? n : kInvalidNode;
}

void buildRigState(const NodeArena& arena, NodeId skeleton, std::span<const NodeId> sceneNodes,
                   RigState& out) {
    out = RigState{};
    out.skeleton = skeleton;
    if (!arena.alive(skeleton)) {
        return;
    }
    std::vector<uint32_t> depth;
    arena.traversePreorder(skeleton, [&](NodeId n) {
        if (n == skeleton) {
            return true;
        }
        const NodeKind kind = arena.kind(n);
        const NodeId p = arena.parent(n);
        const int32_t parentIdx = p != skeleton ? boneIndex(out, p) : -1;
        if (kind == NodeKind::Bone) {
            out.bones.push_back(n);
            out.parent.push_back(parentIdx);
            depth.push_back(parentIdx >= 0 ? depth[static_cast<size_t>(parentIdx)] + 1 : 0);
            return true;
        }
        if (kind == NodeKind::Constraint && parentIdx >= 0) {
            out.constraints.push_back(n);
            out.owner.push_back(parentIdx);
        }
        return false;  // Not part of the skeleton (or a constraint: no children).
    });
    const size_t n = out.bones.size();
    out.subtreeEnd.assign(n, static_cast<uint32_t>(n));
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = i + 1; j < n; ++j) {
            if (depth[j] <= depth[i]) {
                out.subtreeEnd[i] = static_cast<uint32_t>(j);
                break;
            }
        }
    }
    out.world.assign(n, Mat2x3{});
    out.prevWorld.assign(n, Mat2x3{});
    out.pose.assign(n, BonePose{});
    out.springs.assign(out.constraints.size(), SpringState{});
    for (NodeId node : sceneNodes) {
        if (node == kInvalidNode || !arena.alive(node)) {
            continue;
        }
        const SkinData* skin = nullptr;
        if (arena.kind(node) == NodeKind::Path) {
            skin = &arena.path(node).skin;
        } else if (arena.kind(node) == NodeKind::Mesh) {
            skin = &arena.mesh(node).skin;
        }
        if (skin == nullptr || skin->skeleton != skeleton || skin->weights.empty()) {
            continue;
        }
        SkinTarget t;
        t.node = node;
        for (const Tendon& tendon : skin->tendons) {
            t.tendonBone.push_back(boneIndex(out, tendon.bone));
        }
        out.skins.push_back(std::move(t));
    }
}

uint32_t evaluate(RigState& rs, NodeArena& arena, float dt) {
    const size_t n = rs.bones.size();
    for (size_t i = 0; i < n; ++i) {
        if (!arena.alive(rs.bones[i]) || arena.kind(rs.bones[i]) != NodeKind::Bone) {
            return 0;  // The skeleton was destroyed under us.
        }
        rs.pose[i] = fkPose(arena, rs.bones[i]);
    }
    recomputeWorld(rs, 0, n);
    if (dt < 0.0f) {
        dt = 0.0f;
    }
    for (size_t k = 0; k < rs.constraints.size(); ++k) {
        const NodeId node = rs.constraints[k];
        if (!arena.alive(node) || arena.kind(node) != NodeKind::Constraint) {
            continue;
        }
        const ConstraintNodeData& c = arena.constraint(node);
        if (c.enabled == 0) {
            continue;
        }
        const size_t owner = static_cast<size_t>(rs.owner[k]);
        switch (c.kind) {
            case ConstraintKind::Ik:
                applyIk(rs, arena, owner, c);
                break;
            case ConstraintKind::Transform:
                applyCopy(rs, arena, owner, c, /*rotationOnly=*/false);
                break;
            case ConstraintKind::Rotation:
                applyCopy(rs, arena, owner, c, /*rotationOnly=*/true);
                break;
            case ConstraintKind::Spring:
                applySpring(rs, arena, owner, c, rs.springs[k], dt);
                break;
        }
    }
    bool changed = !rs.evaluated;
    for (size_t i = 0; i < n && !changed; ++i) {
        const Mat2x3& a = rs.world[i];
        const Mat2x3& b = rs.prevWorld[i];
        changed = a.a != b.a || a.b != b.b || a.c != b.c || a.d != b.d || a.e != b.e || a.f != b.f;
    }
    if (changed) {
        ++rs.version;
    }
    rs.prevWorld = rs.world;
    rs.evaluated = true;
    return static_cast<uint32_t>(n);
}

void resetSprings(RigState& rs) {
    for (SpringState& s : rs.springs) {
        s = SpringState{};
    }
}

bool skinPath(const NodeArena& arena, const RigState& rs, const SkinTarget& target, std::vector<float>& out) {
    if (!arena.alive(target.node) || arena.kind(target.node) != NodeKind::Path) {
        return false;
    }
    const PathNodeData& p = arena.path(target.node);
    const SkinData& skin = p.skin;
    size_t vertices = 0;
    for (PathVerb v : p.verbs) {
        vertices += v != PathVerb::Close ? 1u : 0u;
    }
    if (skin.weights.size() != vertices * 3) {
        return false;
    }
    std::vector<Mat2x3> mats;
    skinMatrices(rs, skin, target, mats);
    out.resize(p.points.size());
    size_t pi = 0;          // Float index into points.
    size_t vertex = 0;      // The vertex the current verb ends at.
    int64_t last = -1;      // The vertex at the current point (the next cubic's "out").
    int64_t subStart = -1;  // The current subpath's first vertex.
    auto skinOne = [&](size_t weight) {
        if (pi + 1 >= p.points.size()) {
            return;
        }
        const Vec2 q = blendPoint(mats, skin.weights[weight], Vec2{p.points[pi], p.points[pi + 1]});
        out[pi] = q.x;
        out[pi + 1] = q.y;
        pi += 2;
    };
    for (PathVerb v : p.verbs) {
        switch (v) {
            case PathVerb::Move:
                skinOne(vertex * 3);
                subStart = static_cast<int64_t>(vertex);
                last = subStart;
                ++vertex;
                break;
            case PathVerb::Line:
                skinOne(vertex * 3);
                last = static_cast<int64_t>(vertex);
                ++vertex;
                break;
            case PathVerb::Cubic:
                skinOne(last >= 0 ? static_cast<size_t>(last) * 3 + 2
                                  : vertex * 3);  // The previous vertex's out.
                skinOne(vertex * 3 + 1);          // This vertex's in.
                skinOne(vertex * 3);              // This vertex.
                last = static_cast<int64_t>(vertex);
                ++vertex;
                break;
            case PathVerb::Close:
                last = subStart;
                break;
        }
    }
    return true;
}

RigView makeView(const RigState& rs, const NodeArena& arena) {
    RigView out;
    out.skeleton = rs.skeleton;
    out.bones.reserve(rs.bones.size());
    for (size_t i = 0; i < rs.bones.size(); ++i) {
        RigViewBone b;
        b.node = rs.bones[i];
        b.parent = rs.parent[i];
        b.world = rs.world[i];
        if (arena.alive(b.node) && arena.kind(b.node) == NodeKind::Bone) {
            b.length = arena.bone(b.node).length;
        }
        out.bones.push_back(b);
    }
    for (size_t k = 0; k < rs.constraints.size(); ++k) {
        const NodeId c = rs.constraints[k];
        if (!arena.alive(c) || arena.kind(c) != NodeKind::Constraint) {
            continue;
        }
        const ConstraintNodeData& data = arena.constraint(c);
        if (data.kind == ConstraintKind::Spring) {
            out.springBones.push_back(rs.owner[k]);
        } else if (data.kind == ConstraintKind::Ik) {
            RigViewIk ik;
            ik.constraint = c;
            ik.bone = rs.owner[k];
            ik.target = data.target;
            ik.invert = data.params[1] >= 0.5f;
            Mat2x3 tw;
            if (targetWorld(rs, arena, data.target, tw)) {
                ik.position = Vec2{tw.e, tw.f};
            }
            out.ik.push_back(ik);
        }
    }
    return out;
}

bool skinMesh(const NodeArena& arena, const RigState& rs, const SkinTarget& target, std::vector<Vec2>& out) {
    if (!arena.alive(target.node) || arena.kind(target.node) != NodeKind::Mesh) {
        return false;
    }
    const MeshNodeData& m = arena.mesh(target.node);
    if (m.skin.weights.size() != m.positions.size()) {
        return false;
    }
    std::vector<Mat2x3> mats;
    skinMatrices(rs, m.skin, target, mats);
    out.resize(m.positions.size());
    for (size_t i = 0; i < m.positions.size(); ++i) {
        out[i] = blendPoint(mats, m.skin.weights[i], m.positions[i]);
    }
    return true;
}

uint32_t updateSkins(const RigState& rs, NodeArena& arena) {
    uint32_t points = 0;
    for (const SkinTarget& t : rs.skins) {
        if (!arena.alive(t.node)) {
            continue;
        }
        const NodeKind kind = arena.kind(t.node);
        if (kind != NodeKind::Path && kind != NodeKind::Mesh) {
            continue;
        }
        SkinnedGeometry& g = arena.skinnedSlot(t.node);
        const bool dirty = (arena.dirty(t.node) & static_cast<uint8_t>(kDirtyPaint)) != 0;
        const size_t size =
            kind == NodeKind::Path ? arena.path(t.node).points.size() : arena.mesh(t.node).positions.size();
        const size_t have = kind == NodeKind::Path ? g.points.size() : g.positions.size();
        if (g.valid != 0 && g.skinnedVersion == rs.version && !dirty && have == size) {
            continue;  // A static frame: the cache stands.
        }
        const bool ok =
            kind == NodeKind::Path ? skinPath(arena, rs, t, g.points) : skinMesh(arena, rs, t, g.positions);
        g.valid = ok ? 1 : 0;
        g.skinnedVersion = ok ? rs.version : 0xFFFFFFFFu;
        if (ok) {
            points +=
                static_cast<uint32_t>(kind == NodeKind::Path ? g.points.size() / 2 : g.positions.size());
        }
    }
    return points;
}

bool skinnedMeshPositions(const NodeArena& arena, NodeId mesh, std::vector<Vec2>& out) {
    if (!arena.alive(mesh) || arena.kind(mesh) != NodeKind::Mesh) {
        return false;
    }
    const SkinnedGeometry* g = arena.skinned(mesh);
    if (g == nullptr || g->valid == 0 || g->positions.size() != arena.mesh(mesh).positions.size()) {
        return false;
    }
    out = g->positions;
    return true;
}

void applyBoneAnchor(NodeArena& arena, NodeId bone, Vec2 point) {
    if (!arena.alive(bone) || arena.kind(bone) != NodeKind::Bone) {
        return;
    }
    const NodeId parent = arena.parent(bone);
    if (parent == kInvalidNode || arena.kind(parent) != NodeKind::Bone) {
        // A root bone: the point is its translate.
        const NodeCommon& c = arena.common(bone);
        if (c.translate.x != point.x) {
            setProperty(arena, bone, prop::TranslateX, Value::ofFloat(point.x));
        }
        if (c.translate.y != point.y) {
            setProperty(arena, bone, prop::TranslateY, Value::ofFloat(point.y));
        }
        return;
    }
    // The FK chain from the root bone down to `bone` (properties only, no constraints).
    RigState chain;
    for (NodeId n = bone; n != kInvalidNode && arena.kind(n) == NodeKind::Bone; n = arena.parent(n)) {
        chain.bones.push_back(n);
    }
    std::reverse(chain.bones.begin(), chain.bones.end());
    const size_t n = chain.bones.size();
    chain.parent.resize(n);
    chain.subtreeEnd.assign(n, static_cast<uint32_t>(n));
    chain.pose.resize(n);
    chain.world.resize(n);
    for (size_t i = 0; i < n; ++i) {
        chain.parent[i] = static_cast<int32_t>(i) - 1;
        chain.pose[i] = fkPose(arena, chain.bones[i]);
    }
    recomputeWorld(chain, 0, n);
    const size_t upper = n - 2;
    const size_t lower = n - 1;
    // Bend the way the bone currently bends (the sign of the joint's turn).
    const Vec2 u = sub(origin(chain.world[lower]), origin(chain.world[upper]));
    const Vec2 v = sub(tipOf(chain, arena, lower), origin(chain.world[lower]));
    const bool invertBend = u.x * v.y - u.y * v.x > 0.0f;
    solveTwo(chain, arena, upper, lower, lower, point, invertBend);
    for (size_t i : {upper, lower}) {
        const float r = chain.pose[i].rotation;
        if (arena.common(chain.bones[i]).rotation != r) {
            setProperty(arena, chain.bones[i], prop::Rotation, Value::ofFloat(r));
        }
    }
}

}  // namespace xt::rig
