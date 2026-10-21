#include <cmath>
#include <string>
#include <vector>

#include "doctest.h"
#include "shape_modifiers.h"  // core/src (tests/CMakeLists.txt adds it to the include path).
#include "xt/animator.h"
#include "xt/command_list.h"
#include "xt/compile/ir_writer.h"
#include "xt/geom.h"
#include "xt/ir.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/painter.h"
#include "xt/property.h"
#include "xt/scene_instance.h"
#include "xt/timeline.h"
#include "xts_props.h"  // compile/src/xts (on the test include path).

/// T-225a (docs/design/31-vector-authoring.md §31.6.1): the modifier algorithms, the Painter's
/// collect-apply-draw, Modifier/Shape properties and the SCEN modifiers[]/shapes[] tail. Expected
/// values are worked out by hand from §31.6.1's formulas.

using namespace xt;

namespace {

constexpr float kTol = 1e-3f;

struct TPath {
    std::vector<PathVerb> verbs;
    std::vector<float> points;
    TPath& move(float x, float y) {
        verbs.push_back(PathVerb::Move);
        points.insert(points.end(), {x, y});
        return *this;
    }
    TPath& line(float x, float y) {
        verbs.push_back(PathVerb::Line);
        points.insert(points.end(), {x, y});
        return *this;
    }
    TPath& cubic(float x1, float y1, float x2, float y2, float x, float y) {
        verbs.push_back(PathVerb::Cubic);
        points.insert(points.end(), {x1, y1, x2, y2, x, y});
        return *this;
    }
    TPath& close() {
        verbs.push_back(PathVerb::Close);
        return *this;
    }
    modifiers::PathSpan span() const { return modifiers::PathSpan{verbs, points}; }
};

TPath square(float s) { return TPath{}.move(0, 0).line(s, 0).line(s, s).line(0, s).close(); }

/// On-curve points (Move/Line/Cubic end points) of a path, in order.
std::vector<Vec2> onCurve(std::span<const PathVerb> verbs, std::span<const float> points) {
    std::vector<Vec2> out;
    size_t pc = 0;
    for (PathVerb v : verbs) {
        if (v == PathVerb::Move || v == PathVerb::Line) {
            out.push_back(Vec2{points[pc], points[pc + 1]});
            pc += 2;
        } else if (v == PathVerb::Cubic) {
            out.push_back(Vec2{points[pc + 4], points[pc + 5]});
            pc += 6;
        }
    }
    return out;
}

bool approxPt(Vec2 a, Vec2 b, float tol = kTol) {
    return std::fabs(a.x - b.x) <= tol && std::fabs(a.y - b.y) <= tol;
}

std::vector<PathVerb> verbsOf(std::initializer_list<PathVerb> v) { return std::vector<PathVerb>(v); }

constexpr PathVerb M = PathVerb::Move;
constexpr PathVerb L = PathVerb::Line;
constexpr PathVerb C = PathVerb::Cubic;
constexpr PathVerb Z = PathVerb::Close;

// --- Painter fixtures: an arena built by hand, painted with a fresh Painter. ---

NodeId addPath(NodeArena& arena, NodeId parent, const TPath& p, bool fill = true, float strokeWidth = 0) {
    NodeId id = arena.create(NodeKind::Path);
    arena.appendChild(parent, id);
    PathNodeData& d = arena.path(id);
    d.verbs = p.verbs;
    d.points = p.points;
    if (fill) {
        d.fill.kind = PaintKind::Solid;
        d.fill.color = Color{1, 0, 0, 1};
    }
    if (strokeWidth > 0) {
        d.stroke.kind = PaintKind::Solid;
        d.stroke.color = Color{0, 0, 1, 1};
        d.strokeStyle.width = strokeWidth;
    }
    return id;
}

NodeId addModifier(NodeArena& arena, NodeId parent, ModifierKind kind, std::initializer_list<float> params,
                   uint8_t mode = 0) {
    NodeId id = arena.create(NodeKind::Modifier);
    arena.appendChild(parent, id);
    ModifierNodeData& m = arena.modifier(id);
    m.kind = kind;
    m.mode = mode;
    int i = 0;
    for (float p : params) {
        m.params[i++] = p;
    }
    return id;
}

CommandList paintTree(NodeArena& arena, NodeId root, float time = 0) {
    updateWorldTransforms(arena, root, true);
    Painter painter;
    painter.setTime(time);
    CommandList cmds;
    painter.paint(arena, root, cmds);
    return cmds;
}

std::vector<Vec2> pathPoints(const CommandList& cmds, const PathRef& ref) {
    return onCurve(std::span<const PathVerb>(cmds.verbs.data() + ref.verbOffset, ref.verbCount),
                   std::span<const float>(cmds.points.data() + ref.pointOffset, ref.pointCount));
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// trim
// ------------------------------------------------------------------------------------------------

TEST_CASE("modifiers::trimPaths: parallel trims each path over its own length") {
    TPath a = TPath{}.move(0, 0).line(10, 0);    // length 10
    TPath b = TPath{}.move(0, 10).line(30, 10);  // length 30
    modifiers::PathSpan spans[] = {a.span(), b.span()};
    std::vector<PathVerb> v;
    std::vector<float> p;
    std::vector<modifiers::PathRange> r;
    modifiers::trimPaths(spans, 0, 0.5f, 0, modifiers::kTrimParallel, v, p, r);
    REQUIRE(r.size() == 2);
    auto pa = onCurve(std::span(v).subspan(r[0].verbOffset, r[0].verbCount),
                      std::span(p).subspan(r[0].pointOffset, r[0].pointCount));
    auto pb = onCurve(std::span(v).subspan(r[1].verbOffset, r[1].verbCount),
                      std::span(p).subspan(r[1].pointOffset, r[1].pointCount));
    REQUIRE(pa.size() == 2);
    CHECK(approxPt(pa[1], Vec2{5, 0}));
    REQUIRE(pb.size() == 2);
    CHECK(approxPt(pb[1], Vec2{15, 10}));
}

TEST_CASE("modifiers::trimPaths: serial maps the interval over the paths' joined length in order") {
    TPath a = TPath{}.move(0, 0).line(10, 0);
    TPath b = TPath{}.move(0, 10).line(30, 10);
    modifiers::PathSpan spans[] = {a.span(), b.span()};
    std::vector<PathVerb> v;
    std::vector<float> p;
    std::vector<modifiers::PathRange> r;
    modifiers::trimPaths(spans, 0, 0.5f, 0, modifiers::kTrimSerial, v, p, r);  // [0, 20] of 40
    REQUIRE(r.size() == 2);
    auto pa = onCurve(std::span(v).subspan(r[0].verbOffset, r[0].verbCount),
                      std::span(p).subspan(r[0].pointOffset, r[0].pointCount));
    auto pb = onCurve(std::span(v).subspan(r[1].verbOffset, r[1].verbCount),
                      std::span(p).subspan(r[1].pointOffset, r[1].pointCount));
    REQUIRE(pa.size() == 2);
    CHECK(approxPt(pa[1], Vec2{10, 0}));  // All of a.
    REQUIRE(pb.size() == 2);
    CHECK(approxPt(pb[0], Vec2{0, 10}));
    CHECK(approxPt(pb[1], Vec2{10, 10}));  // The first 10 of b.

    // Offset is in degrees: 90 = a quarter turn.
    v.clear();
    p.clear();
    r.clear();
    modifiers::trimPaths(spans, 0, 0.25f, 90, modifiers::kTrimParallel, v, p, r);
    auto qa = onCurve(std::span(v).subspan(r[0].verbOffset, r[0].verbCount),
                      std::span(p).subspan(r[0].pointOffset, r[0].pointCount));
    REQUIRE(qa.size() == 2);
    CHECK(approxPt(qa[0], Vec2{2.5f, 0}));
    CHECK(approxPt(qa[1], Vec2{5, 0}));
}

TEST_CASE("modifiers::trimPaths: an interval past 1 continues a closed contour without a break") {
    TPath sq = square(10);  // length 40
    modifiers::PathSpan spans[] = {sq.span()};
    std::vector<PathVerb> v;
    std::vector<float> p;
    std::vector<modifiers::PathRange> r;
    // s = 0.6 + 0.25, e = 0.9 + 0.25 -> [0.85, 1] + [0, 0.15] -> [34, 40] + [0, 6].
    modifiers::trimPaths(spans, 0.6f, 0.9f, 90, modifiers::kTrimParallel, v, p, r);
    CHECK(v == verbsOf({M, L, L}));
    auto pts = onCurve(v, p);
    REQUIRE(pts.size() == 3);
    CHECK(approxPt(pts[0], Vec2{0, 6}));
    CHECK(approxPt(pts[1], Vec2{0, 0}));
    CHECK(approxPt(pts[2], Vec2{6, 0}));

    // The same on an open polyline (length 30) breaks into two fragments.
    TPath open = TPath{}.move(0, 0).line(10, 0).line(10, 10).line(0, 10);
    modifiers::PathSpan openSpans[] = {open.span()};
    v.clear();
    p.clear();
    r.clear();
    modifiers::trimPaths(openSpans, 0.6f, 0.9f, 90, modifiers::kTrimParallel, v, p, r);
    int moves = 0;
    for (PathVerb verb : v) {
        moves += verb == M ? 1 : 0;
    }
    CHECK(moves == 2);
}

// ------------------------------------------------------------------------------------------------
// repeat
// ------------------------------------------------------------------------------------------------

TEST_CASE("modifiers::repeatCopies: transforms and opacity so + (eo - so) * i / n") {
    ModifierNodeData m;
    m.kind = ModifierKind::Repeat;
    m.params[0] = 3;   // copies
    m.params[1] = 0;   // offset
    m.params[4] = 10;  // translate x
    m.params[6] = 90;  // rotation
    m.params[7] = 1;   // scale
    m.opacityA = 1.0f;
    m.opacityB = 0.4f;
    std::vector<modifiers::RepeatCopy> copies;
    modifiers::repeatCopies(m, copies);
    REQUIRE(copies.size() == 3);
    CHECK(copies[0].alpha == doctest::Approx(1.0f));
    CHECK(copies[1].alpha == doctest::Approx(0.8f));
    CHECK(copies[2].alpha == doctest::Approx(0.6f));
    CHECK(approxPt(apply(copies[0].m, Vec2{1, 0}), Vec2{1, 0}));
    CHECK(approxPt(apply(copies[1].m, Vec2{1, 0}), Vec2{10, 1}));  // R(90) then +10
    CHECK(approxPt(apply(copies[2].m, Vec2{1, 0}), Vec2{19, 0}));  // R(180) then +20
    // copies rounds and clamps; a copy at opacity <= 0 is left out.
    m.params[0] = 2.6f;
    m.opacityA = 0.0f;
    m.opacityB = 1.0f;
    copies.clear();
    modifiers::repeatCopies(m, copies);
    REQUIRE(copies.size() == 2);  // round(2.6) = 3, copy 0 has opacity 0.
    CHECK(copies[0].index == 1);
}

TEST_CASE("Painter: a trailing repeat duplicates commands over one PathRef (xf, alpha, order)") {
    NodeArena arena;
    NodeId group = arena.create(NodeKind::Group);
    addPath(arena, group, square(4));
    NodeId rep = addModifier(arena, group, ModifierKind::Repeat, {3, 0, 0, 0, 10, 0, 0, 1});
    arena.modifier(rep).opacityA = 1.0f;
    arena.modifier(rep).opacityB = 0.4f;

    CommandList below = paintTree(arena, group);
    REQUIRE(below.fills.size() == 3);
    for (const FillCmd& f : below.fills) {
        CHECK(f.path.verbOffset == below.fills[0].path.verbOffset);  // Shared geometry.
        CHECK(f.path.verbCount == 5);
    }
    CHECK(below.verbs.size() == 5);
    // order = below: copy 0 on top, i.e. drawn last.
    CHECK(below.fills[0].xf.e == doctest::Approx(20));
    CHECK(below.fills[0].paint.color.a == doctest::Approx(0.6f));
    CHECK(below.fills[2].xf.e == doctest::Approx(0));
    CHECK(below.fills[2].paint.color.a == doctest::Approx(1.0f));

    arena.modifier(rep).mode = 1;  // order = above
    CommandList above = paintTree(arena, group);
    REQUIRE(above.fills.size() == 3);
    CHECK(above.fills[0].xf.e == doctest::Approx(0));
    CHECK(above.fills[1].xf.e == doctest::Approx(10));
    CHECK(above.fills[1].paint.color.a == doctest::Approx(0.8f));
}

TEST_CASE("Painter: repeat then serial trim expands the copies and trims them in copy order") {
    NodeArena arena;
    NodeId group = arena.create(NodeKind::Group);
    addPath(arena, group, TPath{}.move(0, 0).line(10, 0), false, 1);
    addModifier(arena, group, ModifierKind::Repeat, {2, 0, 0, 0, 0, 10, 0, 1});
    addModifier(arena, group, ModifierKind::Trim, {0, 0.75f, 0}, modifiers::kTrimSerial);

    CommandList cmds = paintTree(arena, group);
    REQUIRE(cmds.strokes.size() == 2);
    // Drawn copy 1 first (order below); total 20, [0, 15]: copy 0 whole, copy 1 its first 5.
    auto first = pathPoints(cmds, cmds.strokes[0].path);
    auto second = pathPoints(cmds, cmds.strokes[1].path);
    REQUIRE(first.size() == 2);
    CHECK(approxPt(first[0], Vec2{0, 10}));
    CHECK(approxPt(first[1], Vec2{5, 10}));
    REQUIRE(second.size() == 2);
    CHECK(approxPt(second[1], Vec2{10, 0}));
    CHECK(cmds.strokes[0].xf.f == doctest::Approx(0));  // Geometry carries the copy transform.
}

// ------------------------------------------------------------------------------------------------
// round
// ------------------------------------------------------------------------------------------------

TEST_CASE("modifiers::roundCorners: each corner becomes two vertices joined by a 0.5523 cubic") {
    TPath sq = square(20);
    std::vector<PathVerb> v;
    std::vector<float> p;
    modifiers::roundCorners(sq.verbs, sq.points, 5, v, p);
    CHECK(v == verbsOf({M, C, L, C, L, C, L, C, Z}));
    CHECK(modifiers::vertexCount(v) == 8);
    auto pts = onCurve(v, p);
    CHECK(approxPt(pts[0], Vec2{0, 5}));
    CHECK(approxPt(pts[1], Vec2{5, 0}));
    CHECK(approxPt(pts[2], Vec2{15, 0}));
    // First corner cubic: handles run along the cut tangents, 0.5523 * 5 long.
    CHECK(p[2] == doctest::Approx(0));
    CHECK(p[3] == doctest::Approx(5 - 0.5523f * 5));
    CHECK(p[4] == doctest::Approx(5 - 0.5523f * 5));
    CHECK(p[5] == doctest::Approx(0));

    // The cut is min(r, L / 2): a radius larger than half an edge stops at the midpoint.
    v.clear();
    p.clear();
    modifiers::roundCorners(sq.verbs, sq.points, 50, v, p);
    auto big = onCurve(v, p);
    CHECK(approxPt(big[0], Vec2{0, 10}));
    CHECK(approxPt(big[1], Vec2{10, 0}));
}

TEST_CASE("modifiers::roundCorners: along the arc of a curve; handle vertices and open ends stay") {
    // Corner (10, 0) between a line and a cubic whose first handle sits on it: the cut on the
    // curve side is measured along the curve.
    TPath p1 = TPath{}.move(0, 0).line(10, 0).cubic(10, 0, 20, 10, 20, 20);
    std::vector<PathVerb> v;
    std::vector<float> p;
    modifiers::roundCorners(p1.verbs, p1.points, 2, v, p);
    auto pts = onCurve(v, p);
    REQUIRE(pts.size() == 4);  // start, A, B, end (open ends are not rounded)
    CHECK(approxPt(pts[0], Vec2{0, 0}));
    CHECK(approxPt(pts[1], Vec2{8, 0}));
    const float chord = std::sqrt((pts[2].x - 10) * (pts[2].x - 10) + pts[2].y * pts[2].y);
    CHECK(chord <= 2.0f + kTol);
    CHECK(chord >= 1.9f);
    CHECK(approxPt(pts.back(), Vec2{20, 20}));

    // A rounded rectangle's vertices all carry a handle: nothing to round.
    std::vector<PathVerb> rv;
    std::vector<float> rp;
    const float rect[12] = {0, 0, 40, 20, 5, 5};
    modifiers::buildShape(ShapeKind::Rect, rect, rv, rp);
    v.clear();
    p.clear();
    modifiers::roundCorners(rv, rp, 3, v, p);
    CHECK(modifiers::vertexCount(v) == modifiers::vertexCount(rv));
    CHECK(onCurve(v, p).front().x == doctest::Approx(5));
}

// ------------------------------------------------------------------------------------------------
// pucker, zigzag, twist, wiggle
// ------------------------------------------------------------------------------------------------

TEST_CASE("modifiers::pucker: vertices toward the sub-path centre, handles away from it") {
    TPath sq = square(10);
    std::vector<PathVerb> v;
    std::vector<float> p;
    modifiers::pucker(sq.verbs, sq.points, 50, v, p);  // a = 0.5, C = (5, 5)
    CHECK(v == verbsOf({M, C, C, C, C, Z}));
    auto pts = onCurve(v, p);
    CHECK(approxPt(pts[0], Vec2{2.5f, 2.5f}));
    CHECK(approxPt(pts[1], Vec2{7.5f, 2.5f}));
    // First cubic: out-handle of v0 = lerp((0,0), C, -0.5) = (-2.5, -2.5); in of v1 = (12.5, -2.5).
    CHECK(p[2] == doctest::Approx(-2.5f));
    CHECK(p[3] == doctest::Approx(-2.5f));
    CHECK(p[4] == doctest::Approx(12.5f));
    CHECK(p[5] == doctest::Approx(-2.5f));

    // Two sub-paths: each uses its own centre.
    TPath two = square(10);
    two.move(100, 0).line(110, 0).line(110, 10).line(100, 10).close();
    v.clear();
    p.clear();
    modifiers::pucker(two.verbs, two.points, 100, v, p);  // a = 1: every vertex at its centre.
    auto all = onCurve(v, p);
    CHECK(approxPt(all[0], Vec2{5, 5}));
    CHECK(approxPt(all.back(), Vec2{105, 5}));
}

TEST_CASE("modifiers::zigzag: points at arc fractions, alternating normals, no wrap on open ends") {
    TPath line = TPath{}.move(0, 0).line(30, 0);
    std::vector<PathVerb> v;
    std::vector<float> p;
    modifiers::zigzag(line.verbs, line.points, 2, 2, false, v, p);
    CHECK(v == verbsOf({M, L, L, L}));
    auto pts = onCurve(v, p);
    CHECK(approxPt(pts[0], Vec2{0, 2}));
    CHECK(approxPt(pts[1], Vec2{10, -2}));
    CHECK(approxPt(pts[2], Vec2{20, 2}));
    CHECK(approxPt(pts[3], Vec2{30, -2}));

    // Closed with an odd point count: the last point stays, so there is no seam.
    TPath tri = TPath{}.move(0, 0).line(10, 0).line(0, 10).close();
    v.clear();
    p.clear();
    modifiers::zigzag(tri.verbs, tri.points, 1, 0, false, v, p);
    auto tp = onCurve(v, p);
    REQUIRE(tp.size() == 3);
    CHECK(approxPt(tp[2], Vec2{0, 10}));
    CHECK(!approxPt(tp[0], Vec2{0, 0}));

    // Smooth: tangent handles of chord / (2 (ridges + 1)) = 30 / 6 = 5.
    v.clear();
    p.clear();
    modifiers::zigzag(line.verbs, line.points, 2, 2, true, v, p);
    CHECK(v == verbsOf({M, C, C, C}));
    CHECK(p[2] == doctest::Approx(5));  // out-handle of the first point: (0 + 5, 2)
    CHECK(p[3] == doctest::Approx(2));
}

TEST_CASE("modifiers::twist: ceil(|a| / 15) pieces per segment, rotation a * (1 - r / R)") {
    TPath sq = square(10);
    std::vector<PathVerb> v;
    std::vector<float> p;
    modifiers::twist(sq.verbs, sq.points, 30, Vec2{5, 5}, v, p);
    CHECK(v.size() == 10);  // M + 8 cubics + Z: every edge split in 2.
    auto pts = onCurve(v, p);
    CHECK(approxPt(pts[0], Vec2{0, 0}));  // Farthest points (the corners) do not turn.
    const float theta = 30.0f * (1.0f - 5.0f / std::sqrt(50.0f)) * 3.14159265f / 180.0f;
    CHECK(approxPt(pts[1], Vec2{5 + 5 * std::sin(theta), 5 - 5 * std::cos(theta)}));
    v.clear();
    p.clear();
    modifiers::twist(sq.verbs, sq.points, 0, Vec2{5, 5}, v, p);
    CHECK(v == sq.verbs);
}

TEST_CASE("modifiers::wiggle: a pure function of its inputs; seed and time change it") {
    TPath sq = square(10);
    modifiers::WiggleParams wp;
    wp.size = 3;
    wp.detail = 1;
    wp.seed = 7;
    wp.speed = 1;
    auto run = [&](const modifiers::WiggleParams& w, float t) {
        std::vector<PathVerb> v;
        std::vector<float> p;
        uint32_t ordinal = 0;
        modifiers::wiggle(sq.verbs, sq.points, w, t, ordinal, v, p);
        return p;
    };
    std::vector<float> a = run(wp, 0.5f);
    std::vector<float> b = run(wp, 0.5f);
    CHECK(a == b);
    CHECK(a.size() == 16);  // 4 segments x 2 pieces, corner mode: M + 7 L + Z.
    modifiers::WiggleParams other = wp;
    other.seed = 8;
    CHECK(run(other, 0.5f) != a);
    CHECK(run(wp, 0.9f) != a);
    for (float f : a) {
        CHECK(std::isfinite(f));
    }
}

// ------------------------------------------------------------------------------------------------
// Shape nodes and properties
// ------------------------------------------------------------------------------------------------

TEST_CASE("Shape node: painted from its parameters; setProperty(ShapeW / ShapeR) changes the path") {
    NodeArena arena;
    NodeId group = arena.create(NodeKind::Group);
    NodeId rect = arena.create(NodeKind::Shape);
    arena.appendChild(group, rect);
    ShapeNodeData& s = arena.shape(rect);
    s.kind = ShapeKind::Rect;
    s.params[2] = 10;
    s.params[3] = 5;
    s.fill.kind = PaintKind::Solid;
    s.fill.color = Color{0, 1, 0, 1};

    CommandList c1 = paintTree(arena, group);
    REQUIRE(c1.fills.size() == 1);
    auto p1 = pathPoints(c1, c1.fills[0].path);
    REQUIRE(p1.size() == 4);
    CHECK(approxPt(p1[1], Vec2{10, 0}));

    setProperty(arena, rect, prop::ShapeW, Value::ofFloat(30));
    CHECK(getProperty(arena, rect, prop::ShapeW).f == doctest::Approx(30));
    CommandList c2 = paintTree(arena, group);
    auto p2 = pathPoints(c2, c2.fills[0].path);
    CHECK(approxPt(p2[1], Vec2{30, 0}));

    NodeId star = arena.create(NodeKind::Shape);
    arena.appendChild(group, star);
    ShapeNodeData& st = arena.shape(star);
    st.kind = ShapeKind::Star;
    st.params[2] = 5;
    st.params[3] = 10;
    st.params[4] = 4;
    st.fill.kind = PaintKind::Solid;
    setProperty(arena, star, prop::ShapeR, Value::ofFloat(20));
    CommandList c3 = paintTree(arena, group);
    REQUIRE(c3.fills.size() == 2);
    auto sp = pathPoints(c3, c3.fills[1].path);
    REQUIRE(sp.size() == 10);
    CHECK(approxPt(sp[0], Vec2{0, -20}));  // First outer point up, at the new radius.
    // A prop the kind does not use reads 0 and ignores writes.
    setProperty(arena, rect, prop::ShapeR, Value::ofFloat(3));
    CHECK(getProperty(arena, rect, prop::ShapeR).f == doctest::Approx(0));
}

TEST_CASE("Modifier / Shape properties: applicability, types, params mapping") {
    CHECK(propertyApplies(NodeKind::Modifier, prop::ModTrimEnd));
    CHECK(propertyApplies(NodeKind::Modifier, prop::Opacity));
    CHECK(propertyApplies(NodeKind::Modifier, prop::Visible));
    CHECK_FALSE(propertyApplies(NodeKind::Modifier, prop::TranslateX));
    CHECK_FALSE(propertyApplies(NodeKind::Modifier, prop::ShapeX));
    CHECK_FALSE(propertyApplies(NodeKind::Modifier, effectParam(0, 1)));
    CHECK_FALSE(propertyApplies(NodeKind::Path, prop::ModTrimEnd));
    CHECK(propertyApplies(NodeKind::Shape, prop::ShapeRound2));
    CHECK(propertyApplies(NodeKind::Shape, prop::FillColor));
    CHECK(propertyApplies(NodeKind::Shape, prop::TranslateX));
    CHECK_FALSE(propertyApplies(NodeKind::Shape, prop::PathData));
    CHECK_FALSE(propertyApplies(NodeKind::Element, prop::ShapeX));
    CHECK(propertyType(prop::ModAnchor) == ValueType::Vec2);
    CHECK(propertyType(prop::ModCenter) == ValueType::Vec2);
    CHECK(propertyType(prop::ModCopies) == ValueType::Float);
    CHECK(propertyType(prop::ShapePoints) == ValueType::Float);
    CHECK(propertyDirtyBits(prop::ModTrimEnd) == kDirtyPaint);
    CHECK(std::string(propertyName(prop::ModSpatialPhase)) == "ModSpatialPhase");
    CHECK(propertyByName("ShapeRound2") == prop::ShapeRound2);

    NodeArena arena;
    NodeId m = arena.create(NodeKind::Modifier);
    arena.modifier(m).kind = ModifierKind::Repeat;
    setProperty(arena, m, prop::ModAnchor, Value::ofVec2(Vec2{3, 4}));
    setProperty(arena, m, prop::ModCopies, Value::ofFloat(6));
    setProperty(arena, m, prop::ModEndOpacity, Value::ofFloat(0.25f));
    setProperty(arena, m, prop::ModRadius, Value::ofFloat(9));  // Not a repeat parameter.
    CHECK(arena.modifier(m).params[0] == doctest::Approx(6));
    CHECK(arena.modifier(m).params[2] == doctest::Approx(3));
    CHECK(arena.modifier(m).params[3] == doctest::Approx(4));
    CHECK(arena.modifier(m).opacityB == doctest::Approx(0.25f));
    CHECK(getProperty(arena, m, prop::ModAnchor).v2.y == doctest::Approx(4));
    CHECK(getProperty(arena, m, prop::ModRadius).f == doctest::Approx(0));
    setProperty(arena, m, prop::ModEnabled, Value::ofFloat(0));
    CHECK(arena.modifier(m).enabled == 0);
}

TEST_CASE(".xts names: modifier / shape parameters are vector props; width -> ShapeW on a Shape") {
    using namespace xt::compile;
    const PropInfo* trimEnd = findProp("trim-end");
    REQUIRE(trimEnd != nullptr);
    CHECK(trimEnd->prop == prop::ModTrimEnd);
    CHECK(isVectorPropInfo(*trimEnd));
    const PropInfo* points = findProp("points");
    REQUIRE(points != nullptr);
    CHECK(points->prop == prop::ShapePoints);
    // T-225b: no StyleProp shares the number any more; findPropById() does not see svg-only names.
    CHECK(findPropById(prop::ModTrimEnd) == nullptr);
    CHECK(std::string(findSvgNodePropById(prop::ModTrimEnd)->cssName) == "trim-end");
    CHECK(appliesToSvgNode(prop::ModTrimEnd, NodeKind::Modifier));
    CHECK_FALSE(appliesToSvgNode(prop::ModTrimEnd, NodeKind::Path));
    CHECK_FALSE(appliesToSvgNode(prop::TranslateX, NodeKind::Modifier));
    CHECK(appliesToSvgNode(prop::ShapeR, NodeKind::Shape));
    CHECK(appliesToSvgNode(prop::FillColor, NodeKind::Shape));
    CHECK_FALSE(appliesToSvgNode(prop::PathData, NodeKind::Shape));
    CHECK(svgNodeProp(prop::Width, NodeKind::Shape) == prop::ShapeW);
    CHECK(svgNodeProp(prop::Width, NodeKind::Path) == prop::Width);
    CHECK(appliesToAnySvgNode(prop::ModCopies));
}

TEST_CASE("Painter: wiggle follows setTime; disabled modifiers and later nodes") {
    NodeArena arena;
    NodeId group = arena.create(NodeKind::Group);
    addPath(arena, group, square(10));
    NodeId w = addModifier(arena, group, ModifierKind::Wiggle, {3, 1, 5, 1, 0, 0, 0});
    CommandList t0 = paintTree(arena, group, 0.0f);
    CommandList t0b = paintTree(arena, group, 0.0f);
    CommandList t1 = paintTree(arena, group, 0.7f);
    CHECK(t0.points == t0b.points);
    CHECK(t0.points != t1.points);

    arena.modifier(w).enabled = 0;
    CommandList off = paintTree(arena, group, 0.7f);
    REQUIRE(off.fills.size() == 1);
    CHECK(off.verbs.size() == 5);  // The square as is.

    // A path after the modifier is not affected by it.
    arena.modifier(w).enabled = 1;
    addPath(arena, group, TPath{}.move(50, 50).line(60, 50).line(60, 60).close());
    CommandList later = paintTree(arena, group, 0.7f);
    REQUIRE(later.fills.size() == 2);
    auto tri = pathPoints(later, later.fills[1].path);
    CHECK(approxPt(tri[0], Vec2{50, 50}));
}

// ------------------------------------------------------------------------------------------------
// IR
// ------------------------------------------------------------------------------------------------

TEST_CASE("IR: SCEN modifiers[]/shapes[] round-trip byte for byte and instantiate") {
    compile::IrWriter writer;
    compile::SceneBuilder scene;
    scene.width = 10;
    scene.height = 10;
    auto node = [&](NodeKind kind, uint32_t parent, uint32_t kindIndex) {
        IrSceneNode n{};
        n.kind = static_cast<uint8_t>(kind);
        n.visible = 1;
        n.parent = parent;
        n.kindIndex = kindIndex;
        n.opacity = 1;
        n.scale = Vec2{1, 1};
        n.name = kInvalidString;
        scene.nodes.push_back(n);
    };
    node(NodeKind::Group, 0xFFFFFFFFu, 0);
    IrShapeData sd{};
    sd.kind = static_cast<uint8_t>(ShapeKind::Ellipse);
    sd.params[0] = 5;
    sd.params[1] = 5;
    sd.params[2] = 4;
    sd.params[3] = 3;
    sd.fillKind = static_cast<uint8_t>(PaintKind::Solid);
    sd.fillColor = Color{1, 0, 0, 1};
    sd.miterLimit = 4;
    sd.trimEnd = 1;
    scene.shapes.push_back(sd);
    node(NodeKind::Shape, 0, 0);
    IrModifierData md{};
    md.kind = static_cast<uint8_t>(ModifierKind::Pucker);
    md.enabled = 1;
    md.params[0] = 25;
    md.opacityA = md.opacityB = 1;
    scene.modifiers.push_back(md);
    node(NodeKind::Modifier, 0, 0);
    SceneId id = writer.addScene(std::move(scene));
    std::vector<uint8_t> bytes = writer.finish();

    Result<IrView> r = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(r.ok());
    const IrView& view = r.value();
    REQUIRE(view.scene(id).modifiers().size() == 1);
    REQUIRE(view.scene(id).shapes().size() == 1);
    CHECK(compile::IrWriter::fromView(view).finish() == bytes);

    NodeArena arena;
    NodeId container = arena.create(NodeKind::Group);
    SceneInstance inst;
    REQUIRE(instantiateScene(view, id, arena, container, inst) == Status::kOk);
    const NodeId shapeNode = inst.localToArena[1];
    const NodeId modNode = inst.localToArena[2];
    REQUIRE(arena.kind(shapeNode) == NodeKind::Shape);
    REQUIRE(arena.kind(modNode) == NodeKind::Modifier);
    CHECK(arena.shape(shapeNode).kind == ShapeKind::Ellipse);
    CHECK(arena.shape(shapeNode).params[2] == doctest::Approx(4));
    CHECK(arena.shape(shapeNode).fill.color.r == doctest::Approx(1));
    CHECK(arena.modifier(modNode).kind == ModifierKind::Pucker);
    CHECK(arena.modifier(modNode).params[0] == doctest::Approx(25));

    CommandList cmds = paintTree(arena, inst.root);
    REQUIRE(cmds.fills.size() == 1);
    CHECK(pathPoints(cmds, cmds.fills[0].path).front().x < 9.0f);  // Puckered toward the centre.
}

TEST_CASE("IR: a scene without modifiers or shapes keeps its pre-T-225 SCEN payload") {
    compile::IrWriter writer;
    compile::SceneBuilder scene;
    IrSceneNode n{};
    n.kind = static_cast<uint8_t>(NodeKind::Group);
    n.visible = 1;
    n.parent = 0xFFFFFFFFu;
    n.opacity = 1;
    n.scale = Vec2{1, 1};
    n.name = kInvalidString;
    scene.nodes.push_back(n);
    SceneId id = writer.addScene(std::move(scene));
    std::vector<uint8_t> bytes = writer.finish();
    Result<IrView> r = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(r.ok());
    CHECK(r.value().scene(id).modifiers().empty());
    CHECK(r.value().scene(id).shapes().empty());
    CHECK(compile::IrWriter::fromView(r.value()).finish() == bytes);
}

TEST_CASE("Animator: a timeline track drives a Modifier parameter (trim-end) and the painted trim") {
    NodeArena arena;
    NodeId group = arena.create(NodeKind::Group);
    addPath(arena, group, TPath{}.move(0, 0).line(10, 0), false, 1);
    NodeId trim = addModifier(arena, group, ModifierKind::Trim, {0, 1, 0});

    Timeline tl;
    tl.duration = 1.0f;
    Keyframe k0;
    k0.time = 0.0f;
    k0.value = Value::ofFloat(1.0f);
    k0.easing = Easing::linear();
    Keyframe k1;
    k1.time = 1.0f;
    k1.value = Value::ofFloat(0.0f);
    k1.easing = Easing::linear();
    tl.keyframes = {k0, k1};
    Track track;
    track.target = 1;
    track.prop = prop::ModTrimEnd;
    track.type = ValueType::Float;
    track.kfOffset = 0;
    track.kfCount = 2;
    tl.tracks.push_back(track);

    Animator anim;
    TimelineId tid = anim.registerTimeline(&tl);
    NodeId local[2] = {group, trim};
    PlayParams params;
    params.timeline = tid;
    params.localToArena = local;
    params.localCount = 2;
    params.selfNode = kInvalidNode;
    anim.play(params, 0.0f);
    anim.evaluate(0.7f, arena);
    CHECK(arena.modifier(trim).params[1] == doctest::Approx(0.3f));

    CommandList cmds = paintTree(arena, group);
    REQUIRE(cmds.strokes.size() == 1);
    auto pts = pathPoints(cmds, cmds.strokes[0].path);
    REQUIRE(pts.size() == 2);
    CHECK(approxPt(pts[1], Vec2{3, 0}));
}

TEST_CASE("Painter: modifierVertices counts the modified output (T-225b, 21.1)") {
    NodeArena arena;
    NodeId group = arena.create(NodeKind::Group);
    addPath(arena, group, square(4));
    NodeId rep = addModifier(arena, group, ModifierKind::Repeat, {3, 0, 0, 0, 10, 0, 0, 1});
    updateWorldTransforms(arena, group, true);
    Painter painter;
    CommandList cmds;
    painter.paint(arena, group, cmds);
    CHECK(painter.modifierVertices() == 12);  // 4 on-curve points x 3 command copies.

    // A round after the repeat expands the geometry: 3 copies x 4 corners x 2 points.
    addModifier(arena, group, ModifierKind::Round, {1});
    cmds.clear();
    painter.paint(arena, group, cmds);
    CHECK(painter.modifierVertices() == 24);

    // No modified group: back to 0 (reset by every paint()).
    arena.modifier(rep).enabled = 0;
    NodeArena plain;
    NodeId g2 = plain.create(NodeKind::Group);
    addPath(plain, g2, square(4));
    updateWorldTransforms(plain, g2, true);
    cmds.clear();
    painter.paint(plain, g2, cmds);
    CHECK(painter.modifierVertices() == 0);
}
