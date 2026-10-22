#include <algorithm>
#include <cmath>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "damage_internal.h"  // computeDamage (T-332 damage case).
#include "doctest.h"
#include "fake_text_shaper.h"
#include "rounded_rect.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/damage.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/painter.h"
#include "xt/property.h"
#include "xt/types.h"
#include "xt/value.h"

/// Painter/Element tests: builds NodeArena Element subtrees directly (no Document/LayoutEngine --
/// LayoutBox/ComputedStyle fields are set by hand) and checks dumpCommandList/CommandList field
/// output against Docs/tasks/T-27.md's rules. See its "測試" section.
///
/// Deviation from Docs/tasks/T-27.md's literal wording (documented per the task sheet's own
/// "回報... 回報未涵蓋情況" instruction): the sheet's "rounded_rect 單元: 半徑 0 -> 5 verbs" and its
/// "背景色... verbs 為 Move + 4 Line + Close（無圓角）" (= 6 verbs: 1 Move + 4 Line + 1 Close) bullet
/// describe the same all-zero-radius rectangle shape but give two different verb counts (5 vs 6).
/// appendRoundedRectPath (rounded_rect.h/.cpp) is the single function both emitElementBackground
/// and this rounded_rect-level test call, so it cannot satisfy both counts at once; this file
/// follows the more detailed, explicitly-spelled-out "Move + 4 Line + Close" = 6 verbs rule for
/// both, since that also correctly predicts the "有圓角 -> 含 4 Cubic" case (4 Line + 4 Cubic).

using namespace xt;

namespace {

NodeId makeElement(NodeArena& arena, NodeId parent) {
    NodeId id = arena.create(NodeKind::Element);
    if (parent != kInvalidNode) {
        arena.appendChild(parent, id);
    }
    return id;
}

std::string paintDump(NodeArena& arena, NodeId root, CommandList& cmds) {
    updateWorldTransforms(arena, root, /*force=*/true);
    Painter painter;
    painter.paint(arena, root, cmds);
    std::string out;
    dumpCommandList(cmds, out);
    return out;
}

uint32_t countVerb(const CommandList& cmds, const PathRef& ref, PathVerb v) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < ref.verbCount; ++i) {
        if (cmds.verbs[ref.verbOffset + i] == v) {
            ++n;
        }
    }
    return n;
}

}  // namespace

TEST_CASE("PaintElement: background color, no radius -> Move + 4 Line + Close, no Cubic") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& data = arena.element(root);
    data.box.width = 100;
    data.box.height = 50;
    data.style.base[prop::BackgroundColor] = Value::ofColor(Color{1, 0, 0, 1});

    CommandList cmds;
    paintDump(arena, root, cmds);

    REQUIRE(cmds.fills.size() == 1);
    const FillCmd& fc = cmds.fills[0];
    CHECK(fc.path.verbCount == 6);  // Move + 4 Line + Close
    CHECK(fc.rule == FillRule::NonZero);
    CHECK(countVerb(cmds, fc.path, PathVerb::Cubic) == 0);
    CHECK(countVerb(cmds, fc.path, PathVerb::Line) == 4);
    CHECK(fc.paint.color.r == doctest::Approx(1.0f));
}

TEST_CASE("PaintElement: background color with uniform border-radius -> 4 Cubic verbs") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& data = arena.element(root);
    data.box.width = 100;
    data.box.height = 50;
    data.style.base[prop::BackgroundColor] = Value::ofColor(Color{0, 1, 0, 1});
    data.style.base[prop::BorderRadiusTL] = Value::ofLength(10, LengthUnit::Px);
    data.style.base[prop::BorderRadiusTR] = Value::ofLength(10, LengthUnit::Px);
    data.style.base[prop::BorderRadiusBR] = Value::ofLength(10, LengthUnit::Px);
    data.style.base[prop::BorderRadiusBL] = Value::ofLength(10, LengthUnit::Px);

    CommandList cmds;
    paintDump(arena, root, cmds);

    REQUIRE(cmds.fills.size() == 1);
    const FillCmd& fc = cmds.fills[0];
    CHECK(countVerb(cmds, fc.path, PathVerb::Cubic) == 4);
    CHECK(countVerb(cmds, fc.path, PathVerb::Line) == 4);
    CHECK(fc.path.verbCount == 10);  // Move + 4x(Line + Cubic) + Close
}

TEST_CASE("PaintElement: border -> NonZero FillPath, points = outer + reversed inner ring") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& data = arena.element(root);
    data.box.width = 100;
    data.box.height = 60;
    data.box.border = Edges{4, 4, 4, 4};
    data.style.base[prop::BorderColor] = Value::ofColor(Color{0.2f, 0.2f, 0.2f, 1});

    CommandList cmds;
    paintDump(arena, root, cmds);

    REQUIRE(cmds.fills.size() == 1);
    const FillCmd& fc = cmds.fills[0];
    // T-201: the inner ring runs counter-clockwise, so a zero-width side cancels exactly.
    CHECK(fc.rule == FillRule::NonZero);
    // Radius 0 both rings: each ring is Move + 4 Line + Close (6 verbs, 5 vertices -> 10 floats).
    CHECK(fc.path.verbCount == 12);
    CHECK(fc.path.pointCount == 20);
}

TEST_CASE("rounded_rect: clampRoundedRectRadii scales all four radii down proportionally") {
    float tl = 200, tr = 200, br = 200, bl = 200;
    clampRoundedRectRadii(100, 50, tl, tr, br, bl);
    CHECK(tl == doctest::Approx(25.0f));
    CHECK(tr == doctest::Approx(25.0f));
    CHECK(br == doctest::Approx(25.0f));
    CHECK(bl == doctest::Approx(25.0f));
}

TEST_CASE("rounded_rect: appendRoundedRectPath radius 0 -> Move + 4 Line + Close") {
    std::vector<PathVerb> verbs;
    std::vector<float> points;
    appendRoundedRectPath(Rect{0, 0, 100, 50}, 0, 0, 0, 0, verbs, points);
    CHECK(verbs.size() == 6);
    CHECK(verbs.front() == PathVerb::Move);
    CHECK(verbs.back() == PathVerb::Close);
}

TEST_CASE("PaintElement: overflow hidden wraps children in PushClipRect/PopClip (padding box)") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& rootData = arena.element(root);
    rootData.box.width = 80;
    rootData.box.height = 60;
    rootData.style.overflowX = rootData.style.overflowY = Overflow::Hidden;

    NodeId child = makeElement(arena, root);
    ElementData& childData = arena.element(child);
    childData.box.width = 10;
    childData.box.height = 10;
    childData.style.base[prop::BackgroundColor] = Value::ofColor(Color{0, 0, 1, 1});

    CommandList cmds;
    std::string out = paintDump(arena, root, cmds);

    std::string expected =
        "PushClipRect rect=(0.000,0.000,80.000,60.000)\n"
        "FillPath xf=[1.000 0.000 0.000 1.000 0.000 0.000] verbs=6 points=10 rule=NonZero "
        "paint=Solid(0.000,0.000,1.000,1.000)\n"
        "PopClip\n";
    CHECK(out == expected);
}

TEST_CASE("PaintElement: z-index reorders Element siblings, stable on ties") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    arena.element(root).box = LayoutBox{};

    NodeId c1 = makeElement(arena, root);  // document order 0, z-index 2
    arena.element(c1).box.width = 10;
    arena.element(c1).box.height = 10;
    arena.element(c1).style.zIndex = 2;
    arena.element(c1).style.base[prop::BackgroundColor] = Value::ofColor(Color{1, 0, 0, 1});

    NodeId c2 = makeElement(arena, root);  // document order 1, z-index 0
    arena.element(c2).box.width = 10;
    arena.element(c2).box.height = 10;
    arena.element(c2).style.zIndex = 0;
    arena.element(c2).style.base[prop::BackgroundColor] = Value::ofColor(Color{0, 1, 0, 1});

    NodeId c3 = makeElement(arena, root);  // document order 2, z-index 0 (tie with c2)
    arena.element(c3).box.width = 10;
    arena.element(c3).box.height = 10;
    arena.element(c3).style.zIndex = 0;
    arena.element(c3).style.base[prop::BackgroundColor] = Value::ofColor(Color{0, 0, 1, 1});

    CommandList cmds;
    paintDump(arena, root, cmds);

    // Expected paint order: (z=0, c2), (z=0, c3) [tie -> document order], (z=2, c1).
    REQUIRE(cmds.fills.size() == 3);
    CHECK(cmds.fills[0].paint.color.g == doctest::Approx(1.0f));  // c2
    CHECK(cmds.fills[1].paint.color.b == doctest::Approx(1.0f));  // c3
    CHECK(cmds.fills[2].paint.color.r == doctest::Approx(1.0f));  // c1
}

TEST_CASE("PaintElement: visibility hidden suppresses own visuals but not children") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& rootData = arena.element(root);
    rootData.box.width = 40;
    rootData.box.height = 40;
    rootData.style.visibility = Visibility::Hidden;
    rootData.style.base[prop::BackgroundColor] = Value::ofColor(Color{1, 0, 0, 1});

    NodeId child = makeElement(arena, root);
    ElementData& childData = arena.element(child);
    childData.box.width = 10;
    childData.box.height = 10;
    childData.style.base[prop::BackgroundColor] = Value::ofColor(Color{0, 1, 0, 1});

    CommandList cmds;
    paintDump(arena, root, cmds);

    REQUIRE(cmds.fills.size() == 1);  // Only the child's background.
    CHECK(cmds.fills[0].paint.color.g == doctest::Approx(1.0f));
}

TEST_CASE("PaintElement: opacity 0.5 with 2 drawable children needs a Layer") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    arena.common(root).opacity = 0.5f;

    NodeId c1 = makeElement(arena, root);
    arena.element(c1).box.width = 10;
    arena.element(c1).box.height = 10;
    arena.element(c1).style.base[prop::BackgroundColor] = Value::ofColor(Color{1, 1, 1, 1});
    NodeId c2 = makeElement(arena, root);
    arena.element(c2).box.width = 10;
    arena.element(c2).box.height = 10;
    arena.element(c2).style.base[prop::BackgroundColor] = Value::ofColor(Color{1, 1, 1, 1});

    CommandList cmds;
    std::string out = paintDump(arena, root, cmds);

    CHECK(out.substr(0, 30) == "PushLayer opacity=0.500 blend=");
    CHECK(out.substr(out.size() - 9) == "PopLayer\n");
    REQUIRE(cmds.fills.size() == 2);
    // Post-Layer alpha: full color alpha, not multiplied by the 0.5 (that happens at composite).
    CHECK(cmds.fills[0].paint.color.a == doctest::Approx(1.0f));
}

TEST_CASE("PaintElement: img object-fit contain centers and letterboxes the shorter axis") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& data = arena.element(root);
    data.box.width = 200;
    data.box.height = 100;
    data.imageAsset = 42;
    data.hasIntrinsic = 1;
    data.intrinsic = Vec2{100, 100};  // square asset, wide content box.
    data.style.objectFit = ObjectFit::Contain;

    CommandList cmds;
    paintDump(arena, root, cmds);

    REQUIRE(cmds.images.size() == 1);
    const ImageCmd& ic = cmds.images[0];
    CHECK(ic.asset == 42);
    CHECK(ic.dst.x == doctest::Approx(50.0f));
    CHECK(ic.dst.y == doctest::Approx(0.0f));
    CHECK(ic.dst.w == doctest::Approx(100.0f));
    CHECK(ic.dst.h == doctest::Approx(100.0f));
    CHECK(ic.srcUv.x == doctest::Approx(0.0f));
    CHECK(ic.srcUv.w == doctest::Approx(1.0f));
}

TEST_CASE("PaintElement: img object-fit cover fills the box and crops srcUv") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& data = arena.element(root);
    data.box.width = 200;
    data.box.height = 100;
    data.imageAsset = 7;
    data.hasIntrinsic = 1;
    data.intrinsic = Vec2{100, 100};  // square asset, wide content box.
    data.style.objectFit = ObjectFit::Cover;

    CommandList cmds;
    paintDump(arena, root, cmds);

    REQUIRE(cmds.images.size() == 1);
    const ImageCmd& ic = cmds.images[0];
    CHECK(ic.dst.x == doctest::Approx(0.0f));
    CHECK(ic.dst.w == doctest::Approx(200.0f));
    CHECK(ic.dst.h == doctest::Approx(100.0f));
    CHECK(ic.srcUv.x == doctest::Approx(0.0f));
    CHECK(ic.srcUv.y == doctest::Approx(0.25f));
    CHECK(ic.srcUv.w == doctest::Approx(1.0f));
    CHECK(ic.srcUv.h == doctest::Approx(0.5f));
}

// --- Docs/design/05-style-and-css.md §5.5c (T-115): background-gradient. ---

TEST_CASE("PaintElement: linear background-gradient emits a second Fill with a Linear paint") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& data = arena.element(root);
    data.box.width = 100;
    data.box.height = 50;
    data.style.base[prop::BackgroundColor] = Value::ofColor(Color{1, 1, 1, 1});
    ElementGradient& g = data.backgroundGradient;
    g.kind = GradientKind::Linear;
    g.angle = 90;
    g.stops.push_back(GradientStop{0.0f, Color{1, 0, 0, 1}});
    g.stops.push_back(GradientStop{1.0f, Color{0, 0, 1, 1}});

    CommandList cmds;
    paintDump(arena, root, cmds);

    REQUIRE(cmds.fills.size() == 2);
    const FillCmd& fc = cmds.fills[1];
    CHECK(fc.paint.kind == PaintKind::Linear);
    // 90deg: the gradient line runs left to right through the center, |W sin a| + |H cos a| long.
    CHECK(fc.paint.p0.x == doctest::Approx(0).epsilon(1e-3));
    CHECK(fc.paint.p0.y == doctest::Approx(25).epsilon(1e-3));
    CHECK(fc.paint.p1.x == doctest::Approx(100).epsilon(1e-3));
    CHECK(fc.paint.p1.y == doctest::Approx(25).epsilon(1e-3));
    CHECK(fc.paint.color.a == doctest::Approx(1));
    REQUIRE(fc.paint.stopCount == 2);
    CHECK(cmds.stops[fc.paint.stopOffset].color.r == doctest::Approx(1));
    CHECK(cmds.stops[fc.paint.stopOffset + 1].color.b == doctest::Approx(1));
}

TEST_CASE("PaintElement: 0deg gradient runs bottom to top, 45deg uses |W sin| + |H cos|") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& data = arena.element(root);
    data.box.width = 100;
    data.box.height = 50;
    ElementGradient& g = data.backgroundGradient;
    g.kind = GradientKind::Linear;
    g.angle = 0;
    g.stops.push_back(GradientStop{0.0f, Color{1, 0, 0, 1}});
    g.stops.push_back(GradientStop{1.0f, Color{0, 0, 1, 1}});

    CommandList cmds;
    paintDump(arena, root, cmds);
    REQUIRE(cmds.fills.size() == 1);
    CHECK(cmds.fills[0].paint.p0.x == doctest::Approx(50).epsilon(1e-3));
    CHECK(cmds.fills[0].paint.p0.y == doctest::Approx(50).epsilon(1e-3));
    CHECK(cmds.fills[0].paint.p1.x == doctest::Approx(50).epsilon(1e-3));
    CHECK(cmds.fills[0].paint.p1.y == doctest::Approx(0).epsilon(1e-3));

    arena.element(root).backgroundGradient.angle = 45;
    CommandList cmds45;
    paintDump(arena, root, cmds45);
    REQUIRE(cmds45.fills.size() == 1);
    const PaintRef& paint = cmds45.fills[0].paint;
    float k = 0.70710678f;  // sin 45 == cos 45
    float length = 100.0f * k + 50.0f * k;
    CHECK(paint.p0.x == doctest::Approx(50 - k * length * 0.5f).epsilon(1e-3));
    CHECK(paint.p0.y == doctest::Approx(25 + k * length * 0.5f).epsilon(1e-3));
    CHECK(paint.p1.x == doctest::Approx(50 + k * length * 0.5f).epsilon(1e-3));
    CHECK(paint.p1.y == doctest::Approx(25 - k * length * 0.5f).epsilon(1e-3));
}

TEST_CASE("PaintElement: radial background-gradient center and radius") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& data = arena.element(root);
    data.box.width = 100;
    data.box.height = 50;
    ElementGradient& g = data.backgroundGradient;
    g.kind = GradientKind::Radial;
    g.rx = Length{50, LengthUnit::Percent};
    g.ry = Length{50, LengthUnit::Percent};
    g.cx = Length{30, LengthUnit::Percent};
    g.cy = Length{40, LengthUnit::Percent};
    g.stops.push_back(GradientStop{0.0f, Color{1, 1, 1, 1}});
    g.stops.push_back(GradientStop{1.0f, Color{0, 0, 0, 1}});

    CommandList cmds;
    paintDump(arena, root, cmds);
    REQUIRE(cmds.fills.size() == 1);
    const PaintRef& paint = cmds.fills[0].paint;
    CHECK(paint.kind == PaintKind::Radial);
    CHECK(paint.p0.x == doctest::Approx(30).epsilon(1e-3));
    CHECK(paint.p0.y == doctest::Approx(20).epsilon(1e-3));
    CHECK(paint.p1.x == doctest::Approx(80).epsilon(1e-3));  // p0 + (rx, 0)
    CHECK(paint.p1.y == doctest::Approx(20).epsilon(1e-3));
    CHECK(paint.p2.x == doctest::Approx(30).epsilon(1e-3));  // p0 + (0, ry)
    CHECK(paint.p2.y == doctest::Approx(45).epsilon(1e-3));
}

// T-116: an axis-aligned elliptical radial gradient (rx != ry) sets PaintRef::p2 to the
// center-plus-vertical-radius point, distinct from p0.
TEST_CASE("PaintElement: elliptical radial background-gradient sets p1/p2 for each axis") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& data = arena.element(root);
    data.box.width = 200;
    data.box.height = 100;
    ElementGradient& g = data.backgroundGradient;
    g.kind = GradientKind::Radial;
    g.rx = Length{50, LengthUnit::Percent};
    g.ry = Length{25, LengthUnit::Percent};
    g.cx = Length{50, LengthUnit::Percent};
    g.cy = Length{50, LengthUnit::Percent};
    g.stops.push_back(GradientStop{0.0f, Color{1, 1, 1, 1}});
    g.stops.push_back(GradientStop{1.0f, Color{0, 0, 0, 1}});

    CommandList cmds;
    paintDump(arena, root, cmds);
    REQUIRE(cmds.fills.size() == 1);
    const PaintRef& paint = cmds.fills[0].paint;
    CHECK(paint.kind == PaintKind::Radial);
    CHECK(paint.p0.x == doctest::Approx(100).epsilon(1e-3));
    CHECK(paint.p0.y == doctest::Approx(50).epsilon(1e-3));
    CHECK(paint.p1.x == doctest::Approx(200).epsilon(1e-3));  // p0 + (rx, 0), rx = 50% of 200
    CHECK(paint.p1.y == doctest::Approx(50).epsilon(1e-3));
    CHECK(paint.p2.x == doctest::Approx(100).epsilon(1e-3));  // p0 + (0, ry), ry = 25% of 100
    CHECK(paint.p2.y == doctest::Approx(75).epsilon(1e-3));
}

TEST_CASE("PaintElement: gradient uses the same rounded-rect path and no gradient emits nothing") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& data = arena.element(root);
    data.box.width = 100;
    data.box.height = 50;
    data.style.base[prop::BorderRadiusTL] = Value::ofLength(10, LengthUnit::Px);
    data.style.base[prop::BorderRadiusTR] = Value::ofLength(10, LengthUnit::Px);
    data.style.base[prop::BorderRadiusBR] = Value::ofLength(10, LengthUnit::Px);
    data.style.base[prop::BorderRadiusBL] = Value::ofLength(10, LengthUnit::Px);

    CommandList empty;
    paintDump(arena, root, empty);
    CHECK(empty.fills.empty());  // No background color and no gradient: nothing to paint.

    ElementGradient& g = data.backgroundGradient;
    g.kind = GradientKind::Linear;
    g.angle = 90;
    g.stops.push_back(GradientStop{0.0f, Color{1, 0, 0, 1}});
    g.stops.push_back(GradientStop{1.0f, Color{0, 0, 1, 1}});

    CommandList cmds;
    paintDump(arena, root, cmds);
    REQUIRE(cmds.fills.size() == 1);
    CHECK(cmds.fills[0].path.verbCount == 10);  // Move + 4x(Line + Cubic) + Close
    CHECK(countVerb(cmds, cmds.fills[0].path, PathVerb::Cubic) == 4);
}

// T-325 (05 §5.9.8): the `outline-*` ring. outlineStyle 1 = solid; width / offset are Lengths.
namespace {

void setOutline(ElementData& data, float width, float offset, Color color, uint8_t style = 1) {
    data.style.outlineStyle = style;
    data.style.base[prop::OutlineWidth] = Value::ofLength(width, LengthUnit::Px);
    data.style.base[prop::OutlineOffset] = Value::ofLength(offset, LengthUnit::Px);
    data.style.base[prop::OutlineColor] = Value::ofColor(color);
}

void setRadius(ElementData& data, float r) {
    data.style.base[prop::BorderRadiusTL] = Value::ofLength(r, LengthUnit::Px);
    data.style.base[prop::BorderRadiusTR] = Value::ofLength(r, LengthUnit::Px);
    data.style.base[prop::BorderRadiusBR] = Value::ofLength(r, LengthUnit::Px);
    data.style.base[prop::BorderRadiusBL] = Value::ofLength(r, LengthUnit::Px);
}

/// The x/y range of `count` floats (count / 2 points) of `ref`, from float index `first`.
Rect pointsBounds(const CommandList& cmds, const PathRef& ref, uint32_t first, uint32_t count) {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (uint32_t i = first; i < first + count; i += 2) {
        const float x = cmds.points[ref.pointOffset + i];
        const float y = cmds.points[ref.pointOffset + i + 1];
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    }
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

}  // namespace

TEST_CASE("PaintElement: T-325 outline-style none or width 0 draws no ring") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& data = arena.element(root);
    data.box.width = 100;
    data.box.height = 60;
    setOutline(data, 3, 2, Color{0, 0.4f, 1, 1}, /*style=*/0);
    CommandList none;
    paintDump(arena, root, none);
    CHECK(none.fills.empty());

    setOutline(data, 0, 2, Color{0, 0.4f, 1, 1});
    CommandList zero;
    paintDump(arena, root, zero);
    CHECK(zero.fills.empty());
}

TEST_CASE("PaintElement: T-325 outline is a NonZero ring outside the border box, after the border") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& data = arena.element(root);
    data.box.width = 100;
    data.box.height = 60;
    data.box.border = Edges{4, 4, 4, 4};
    data.style.base[prop::BorderColor] = Value::ofColor(Color{0.2f, 0.2f, 0.2f, 1});
    data.style.overflowX = data.style.overflowY = Overflow::Hidden;
    NodeId child = makeElement(arena, root);
    arena.element(child).box.width = 10;
    arena.element(child).box.height = 10;
    arena.element(child).style.base[prop::BackgroundColor] = Value::ofColor(Color{1, 0, 0, 1});

    CommandList without;
    paintDump(arena, root, without);
    REQUIRE(without.fills.size() == 2);  // border, child background

    // Re-fetched: creating the child may have moved the element storage `data` pointed into.
    setOutline(arena.element(root), 3, 2, Color{0, 0.4f, 1, 1});
    CommandList cmds;
    paintDump(arena, root, cmds);
    REQUIRE(cmds.fills.size() == 3);  // border, outline, child background
    // Command order: border, outline, PushClipRect (overflow), child, PopClip.
    REQUIRE(cmds.commands.size() >= 3);
    CHECK(cmds.commands[0].kind == CmdKind::FillPath);
    CHECK(cmds.commands[0].index == 0u);
    CHECK(cmds.commands[1].kind == CmdKind::FillPath);
    CHECK(cmds.commands[1].index == 1u);
    CHECK(cmds.commands[2].kind == CmdKind::PushClipRect);

    const FillCmd& ring = cmds.fills[1];
    CHECK(ring.rule == FillRule::NonZero);
    CHECK(ring.paint.color.b == doctest::Approx(1.0f));
    CHECK(ring.path.verbCount == 12);  // Two square rings: Move + 4 Line + Close each.
    CHECK(countVerb(cmds, ring.path, PathVerb::Cubic) == 0);
    // Outer edge: border box grown by offset + width = 5; inner edge: grown by offset = 2.
    const Rect outer = pointsBounds(cmds, ring.path, 0, 10);
    CHECK(outer.x == doctest::Approx(-5));
    CHECK(outer.y == doctest::Approx(-5));
    CHECK(outer.w == doctest::Approx(110));
    CHECK(outer.h == doctest::Approx(70));
    const Rect inner = pointsBounds(cmds, ring.path, 10, 10);
    CHECK(inner.x == doctest::Approx(-2));
    CHECK(inner.y == doctest::Approx(-2));
    CHECK(inner.w == doctest::Approx(104));
    CHECK(inner.h == doctest::Approx(64));

    // Damage (23.2) reads the same command bounds: the ring's whole extent.
    const Rect bounds = commandBounds(cmds, cmds.commands[1]);
    CHECK(bounds.x == doctest::Approx(-5));
    CHECK(bounds.w == doctest::Approx(110));
}

TEST_CASE("PaintElement: T-325 negative outline-offset draws inside the border box") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& data = arena.element(root);
    data.box.width = 100;
    data.box.height = 60;
    setOutline(data, 3, -4, Color{1, 1, 1, 1});
    CommandList cmds;
    paintDump(arena, root, cmds);
    REQUIRE(cmds.fills.size() == 1);
    const FillCmd& ring = cmds.fills[0];
    const Rect outer = pointsBounds(cmds, ring.path, 0, 10);
    CHECK(outer.x == doctest::Approx(1));
    CHECK(outer.w == doctest::Approx(98));
    const Rect inner = pointsBounds(cmds, ring.path, 10, 10);
    CHECK(inner.x == doctest::Approx(4));
    CHECK(inner.w == doctest::Approx(92));

    // An offset past the box's half height collapses the inner edge (100x60 grown by -31 is 38x0)
    // while the outer one (grown by -28: 44x4) remains: the ring becomes a filled box.
    setOutline(data, 3, -31, Color{1, 1, 1, 1});
    CommandList collapsed;
    paintDump(arena, root, collapsed);
    REQUIRE(collapsed.fills.size() == 1);
    CHECK(collapsed.fills[0].path.verbCount == 6);  // Outer edge only.
}

TEST_CASE("PaintElement: T-325 outline corners = border-radius + offset, clamped at 0") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& data = arena.element(root);
    data.box.width = 100;
    data.box.height = 60;
    setRadius(data, 10);
    setOutline(data, 3, 2, Color{1, 1, 1, 1});
    CommandList cmds;
    paintDump(arena, root, cmds);
    REQUIRE(cmds.fills.size() == 1);
    const FillCmd& ring = cmds.fills[0];
    CHECK(countVerb(cmds, ring.path, PathVerb::Cubic) == 8);
    // Outer Move = (x + rOuter, y) = (-5 + 15, -5); the inner ring's Move = (-2 + 12, -2).
    CHECK(cmds.points[ring.path.pointOffset + 0] == doctest::Approx(10));
    CHECK(cmds.points[ring.path.pointOffset + 1] == doctest::Approx(-5));
    // Outer ring: Move + 4 Line + 4 Cubic -> 1 + 4 + 12 = 17 points = 34 floats.
    CHECK(cmds.points[ring.path.pointOffset + 34] == doctest::Approx(10));
    CHECK(cmds.points[ring.path.pointOffset + 35] == doctest::Approx(-2));

    // radius 3 with offset -4: the inner corner is square (clamped) and so is the outer one.
    setRadius(data, 3);
    setOutline(data, 2, -4, Color{1, 1, 1, 1});
    CommandList square;
    paintDump(arena, root, square);
    REQUIRE(square.fills.size() == 1);
    CHECK(countVerb(square, square.fills[0].path, PathVerb::Cubic) == 0);
}

TEST_CASE("PaintElement: T-325 outline extent joins the element's effect bounds") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& data = arena.element(root);
    data.box.width = 100;
    data.box.height = 50;
    data.style.base[prop::BackgroundColor] = Value::ofColor(Color{1, 1, 1, 1});
    setOutline(data, 3, 2, Color{0, 0, 1, 1});
    EffectInstance blur{};
    blur.kind = EffectKind::Blur;
    blur.enabled = 1;
    REQUIRE(arena.assignEffects(root, std::span<const EffectInstance>(&blur, 1), {}) == Status::kOk);
    CommandList cmds;
    cmds.viewport = Rect{-10000, -10000, 20000, 20000};
    paintDump(arena, root, cmds);
    REQUIRE(cmds.effectCmds.size() == 1);
    const EffectCmd& ec = cmds.effectCmds[0];
    CHECK(ec.contentBounds.x == doctest::Approx(-5));
    CHECK(ec.contentBounds.y == doctest::Approx(-5));
    CHECK(ec.contentBounds.w == doctest::Approx(110));
    CHECK(ec.contentBounds.h == doctest::Approx(60));
}

// T-324 (05 §5.9.8): `text-decoration`, end to end (compiler -> StyleEngine -> layout -> Painter)
// with FakeTextShaper's metrics (ascent 0.8em, descent 0.2em, no underline metrics, so the 0.1em
// underline offset and the max(1px, 1/14em) `auto` thickness apply unless a test says otherwise).
namespace {

std::string decorationDoc(const std::string& css, const std::string& body) {
    return "<xt>\n<head>\n<style>\n" + css + "\n</style>\n</head>\n<body>\n" + body + "\n</body>\n</xt>\n";
}

std::unique_ptr<Document> loadDecorationDoc(const std::string& source, TextShaper& shaper) {
    compile::SourceFile file{"deco.xt", source};
    compile::CompileOptions options;
    compile::CompileResult result = compile::compileDocumentFromStrings(
        std::span<const compile::SourceFile>(&file, 1), "deco.xt", options);
    for (const compile::Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &shaper, nullptr, doc) ==
            Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 300});
    doc->update(0.0f);
    return doc;
}

const TextNodeData& decorationText(Document& doc, const char* id) {
    ElementRef element = doc.byId(id);
    REQUIRE(element.valid());
    NodeId found = kInvalidNode;
    doc.arena().forEachChild(element.node, [&](NodeId child) {
        if (found == kInvalidNode && doc.arena().kind(child) == NodeKind::Text) {
            found = child;
        }
    });
    REQUIRE(found != kInvalidNode);
    return doc.arena().text(found);
}

/// A painted decoration: its rectangle in text-node space and its color.
struct DecoRect {
    Rect rect;
    Color color;
};

std::vector<DecoRect> decorationRects(const CommandList& list) {
    std::vector<DecoRect> out;
    for (const Command& c : list.commands) {
        if (c.kind == CmdKind::FillPath) {
            const FillCmd& f = list.fills[c.index];
            out.push_back(DecoRect{pointsBounds(list, f.path, 0, f.path.pointCount), f.paint.color});
        }
    }
    return out;
}

/// FakeTextShaper with a font underline: 0.2em below the baseline, 0.1em thick.
class UnderlineShaper : public FakeTextShaper {
public:
    FontMetrics metrics(FontId font) override {
        FontMetrics m = FakeTextShaper::metrics(font);
        m.underlineOffset = 0.2f;
        m.underlineThickness = 0.1f;
        return m;
    }
};

}  // namespace

TEST_CASE("PaintText: T-324 underline, overline and line-through, one each") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc = loadDecorationDoc(
        decorationDoc("p { font-size: 20px; color: #ffffff; }"
                      " #u { text-decoration: underline; }"
                      " #o { text-decoration: overline #ff0000 2px; }"
                      " #l { text-decoration-line: line-through; text-decoration-color: #00ff00; }",
                      "<p id=\"u\">ab</p><p id=\"o\">ab</p><p id=\"l\">ab</p>"),
        shaper);
    // Snapped to whole pixels: the 20/14 px `auto` thickness is 1px.
    const float auto20 = 20.0f / 14.0f;
    struct Case {
        const char* id;
        float top;  // relative to the baseline
        float thickness;
        Color color;
    };
    const Case cases[] = {
        {"u", 2.0f - auto20 * 0.5f, 1.0f, Color{1, 1, 1, 1}},   // 0.1em below, centered; color = `color`
        {"o", -16.0f, 2.0f, Color{1, 0, 0, 1}},                 // top edge at the ascent
        {"l", -6.0f - auto20 * 0.5f, 1.0f, Color{0, 1, 0, 1}},  // 0.3em above, centered
    };
    for (const Case& c : cases) {
        CAPTURE(c.id);
        const TextNodeData& text = decorationText(*doc, c.id);
        REQUIRE(text.runs.size() == 1);
        REQUIRE(text.lines.size() == 1);
        // Paint just this element: its text node's commands are the glyphs then the decoration.
        CommandList list;
        doc->paint(list);
        std::vector<DecoRect> rects = decorationRects(list);
        REQUIRE(rects.size() == 3);
        const size_t index = c.id[0] == 'u' ? 0 : c.id[0] == 'o' ? 1 : 2;
        const DecoRect& d = rects[index];
        CHECK(d.rect.x == doctest::Approx(text.runs[0].bounds.x));
        CHECK(d.rect.w == doctest::Approx(text.runs[0].bounds.w));
        CHECK(d.rect.y == doctest::Approx(std::round(text.lines[0].baseline + c.top)));
        CHECK(d.rect.h == doctest::Approx(c.thickness));
        CHECK(d.color.r == doctest::Approx(c.color.r));
        CHECK(d.color.g == doctest::Approx(c.color.g));
        CHECK(d.color.b == doctest::Approx(c.color.b));
    }
}

TEST_CASE("PaintText: T-324 order is text-effect, glyphs, decoration; no decoration by default") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc = loadDecorationDoc(
        decorationDoc("#a { font-size: 10px; text-effect: glow 2px #000; text-decoration: underline; }",
                      "<p id=\"a\">ab</p><p id=\"b\">cd</p>"),
        shaper);
    CommandList list;
    doc->paint(list);
    std::vector<CmdKind> kinds;
    for (const Command& c : list.commands) {
        if (c.kind == CmdKind::DrawGlyphs || c.kind == CmdKind::FillPath) {
            kinds.push_back(c.kind);
        }
    }
    // #a: glow, fill, underline; #b: fill only.
    REQUIRE(kinds.size() == 4);
    CHECK(kinds[0] == CmdKind::DrawGlyphs);
    CHECK(list.glyphRuns[0].effect == GlyphEffect::Blur);
    CHECK(kinds[1] == CmdKind::DrawGlyphs);
    CHECK(kinds[2] == CmdKind::FillPath);
    CHECK(kinds[3] == CmdKind::DrawGlyphs);
}

TEST_CASE("PaintText: T-324 every line of a wrapped text is decorated") {
    FakeTextShaper shaper;
    // "aaaa bbbb cccc" at 10px wraps into two lines at 60px (see test_rich_text.cpp).
    std::unique_ptr<Document> doc =
        loadDecorationDoc(decorationDoc("#a { width: 60px; font-size: 10px; text-decoration: underline; }",
                                        "<p id=\"a\">aaaa bbbb cccc</p>"),
                          shaper);
    const TextNodeData& text = decorationText(*doc, "a");
    REQUIRE(text.lines.size() == 2);
    CommandList list;
    doc->paint(list);
    std::vector<DecoRect> rects = decorationRects(list);
    REQUIRE(rects.size() == text.runs.size());
    REQUIRE(text.runs.size() == 2);
    const float t = 1.0f;  // max(1px, 10/14 px)
    for (size_t i = 0; i < 2; ++i) {
        CAPTURE(i);
        CHECK(rects[i].rect.y == doctest::Approx(std::round(text.lines[i].baseline + 1.0f - t * 0.5f)));
        CHECK(rects[i].rect.h == doctest::Approx(t));
        CHECK(rects[i].rect.w == doctest::Approx(text.runs[i].bounds.w));
    }
}

TEST_CASE("PaintText: T-324 run overrides add lines; <u> and <s> map to them") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc = loadDecorationDoc(
        decorationDoc("#a { font-size: 10px; color: #0000ff; }",
                      "<p id=\"a\">ab<u>cd</u><s style=\"text-decoration-color: #ff0000\">ef</s>"
                      "<span style=\"color: #00ff00; text-decoration-line: overline\">gh</span></p>"),
        shaper);
    const TextNodeData& text = decorationText(*doc, "a");
    REQUIRE(text.runs.size() == 4);
    CommandList list;
    doc->paint(list);
    std::vector<DecoRect> rects = decorationRects(list);
    REQUIRE(rects.size() == 3);
    const float baseline = text.lines[0].baseline;
    // <u>: underline in the element's decoration color (= its `color`).
    CHECK(rects[0].rect.x == doctest::Approx(text.runs[1].bounds.x));
    CHECK(rects[0].rect.y == doctest::Approx(std::round(baseline + 1.0f - 0.5f)));
    CHECK(rects[0].color.b == doctest::Approx(1.0f));
    // <s>: line-through in its own text-decoration-color.
    CHECK(rects[1].rect.x == doctest::Approx(text.runs[2].bounds.x));
    CHECK(rects[1].rect.y == doctest::Approx(std::round(baseline - 3.0f - 0.5f)));
    CHECK(rects[1].color.r == doctest::Approx(1.0f));
    // A run's own `color` colors its own line.
    CHECK(rects[2].rect.x == doctest::Approx(text.runs[3].bounds.x));
    CHECK(rects[2].rect.y == doctest::Approx(std::round(baseline - 8.0f)));
    CHECK(rects[2].color.g == doctest::Approx(1.0f));
}

TEST_CASE("PaintText: T-324 a RTL run is decorated across its own box") {
    // Codepoints 2..4 ("cde") are RTL.
    FakeTextShaper shaper({{2, 5}});
    std::unique_ptr<Document> doc = loadDecorationDoc(
        decorationDoc("#a { font-size: 10px; text-decoration: line-through; }", "<p id=\"a\">abcdefg</p>"),
        shaper);
    const TextNodeData& text = decorationText(*doc, "a");
    REQUIRE(text.runs.size() == 3);
    CHECK(text.runs[1].bidiLevel % 2 == 1);
    CommandList list;
    doc->paint(list);
    std::vector<DecoRect> rects = decorationRects(list);
    REQUIRE(rects.size() == 3);
    for (size_t i = 0; i < 3; ++i) {
        CAPTURE(i);
        CHECK(rects[i].rect.x == doctest::Approx(text.runs[i].bounds.x));
        CHECK(rects[i].rect.w == doctest::Approx(text.runs[i].bounds.w));
    }
}

TEST_CASE("PaintText: T-324 a decoration reaches descendant text in the ancestor's color") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc =
        loadDecorationDoc(decorationDoc("#box { font-size: 10px; text-decoration: underline #ff0000 2px; }"
                                        " #a { color: #00ff00; }",
                                        "<div id=\"box\"><p id=\"a\">ab</p></div>"),
                          shaper);
    ElementRef a = doc->byId("a");
    REQUIRE(a.valid());
    const ComputedStyle& style = doc->arena().element(a.node).style;
    CHECK(style.textDecorationLine == 0);
    CHECK(style.textDecorations == kTextDecorationUnderline);
    CommandList list;
    doc->paint(list);
    std::vector<DecoRect> rects = decorationRects(list);
    REQUIRE(rects.size() == 1);
    CHECK(rects[0].color.r == doctest::Approx(1.0f));
    CHECK(rects[0].color.g == doctest::Approx(0.0f));
    CHECK(rects[0].rect.h == doctest::Approx(2.0f));
}

TEST_CASE("PaintText: T-324 the font's underline metrics; an extreme thickness stays in the run box") {
    UnderlineShaper shaper;
    std::unique_ptr<Document> doc =
        loadDecorationDoc(decorationDoc("p { font-size: 20px; } #a { text-decoration: underline; }"
                                        " #b { text-decoration: overline 100px; }",
                                        "<p id=\"a\">ab</p><p id=\"b\">ab</p>"),
                          shaper);
    const TextNodeData& ta = decorationText(*doc, "a");
    const TextNodeData& tb = decorationText(*doc, "b");
    CommandList list;
    doc->paint(list);
    std::vector<DecoRect> rects = decorationRects(list);
    REQUIRE(rects.size() == 2);
    // 0.2em below the baseline, 0.1em (2px) thick, centered.
    CHECK(rects[0].rect.y == doctest::Approx(std::round(ta.lines[0].baseline + 4.0f - 1.0f)));
    CHECK(rects[0].rect.h == doctest::Approx(2.0f));
    // Clamped to the font height (1em = 20px) and kept inside the run box (no paint-bounds growth).
    const Rect& box = tb.runs[0].bounds;
    CHECK(rects[1].rect.h == doctest::Approx(20.0f));
    CHECK(rects[1].rect.y >= box.y - 1e-4f);
    CHECK(rects[1].rect.y + rects[1].rect.h <= box.y + box.h + 1e-4f);
}

TEST_CASE("PaintText: T-324 setStyle text-decoration at run time") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc =
        loadDecorationDoc(decorationDoc("#a { font-size: 10px; }", "<p id=\"a\">ab</p>"), shaper);
    ElementRef a = doc->byId("a");
    REQUIRE(a.valid());
    CHECK(a.setStyle("text-decoration: underline overline #ff0000") == Status::kOk);
    CHECK(a.setStyle("text-decoration-line: underline underline") == Status::kInvalidArgument);
    doc->update(0.1f);
    CommandList list;
    doc->paint(list);
    std::vector<DecoRect> rects = decorationRects(list);
    REQUIRE(rects.size() == 2);
    CHECK(rects[0].color.r == doctest::Approx(1.0f));
}
// T-326 (18 §18.1 / §18.12): an Element's `mix-blend-mode` (StyleEngine copies it into
// NodeCommon::blend) takes the same PushLayer(opacity, blend) branch a scene node does. Normal
// costs nothing; any other mode wraps the element in one Layer, and its opacity then goes into
// the Layer instead of each paint's alpha.
TEST_CASE("PaintElement: T-326 mix-blend-mode normal draws no Layer") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& data = arena.element(root);
    data.box.width = 100;
    data.box.height = 50;
    data.style.base[prop::BackgroundColor] = Value::ofColor(Color{1, 0, 0, 1});
    arena.common(root).blend = static_cast<uint8_t>(BlendMode::Normal);
    arena.common(root).opacity = 0.5f;

    CommandList cmds;
    std::string out = paintDump(arena, root, cmds);
    CHECK(out.find("PushLayer") == std::string::npos);
    REQUIRE(cmds.fills.size() == 1);
    CHECK(cmds.fills[0].paint.color.a == doctest::Approx(0.5f));  // One drawable: opacity folded.
}

TEST_CASE("PaintElement: T-326 mix-blend-mode multiply wraps the element in a Layer, alpha unfolded") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& data = arena.element(root);
    data.box.width = 100;
    data.box.height = 50;
    data.style.base[prop::BackgroundColor] = Value::ofColor(Color{1, 0, 0, 1});
    arena.common(root).blend = static_cast<uint8_t>(BlendMode::Multiply);
    arena.common(root).opacity = 0.5f;

    CommandList cmds;
    std::string out = paintDump(arena, root, cmds);
    CHECK(out.substr(0, 38) == "PushLayer opacity=0.500 blend=Multiply");
    CHECK(out.substr(out.size() - 9) == "PopLayer\n");
    REQUIRE(cmds.layers.size() == 1);
    CHECK(cmds.layers[0].blend == BlendMode::Multiply);
    CHECK(cmds.layers[0].opacity == doctest::Approx(0.5f));
    REQUIRE(cmds.fills.size() == 1);
    CHECK(cmds.fills[0].paint.color.a == doctest::Approx(1.0f));  // Composited by the Layer.

    // Every other non-Normal mode takes the Layer too, opacity 1 included.
    arena.common(root).opacity = 1.0f;
    for (uint8_t m = 1; m <= static_cast<uint8_t>(BlendMode::Add); ++m) {
        arena.common(root).blend = m;
        CommandList each;
        paintDump(arena, root, each);
        CAPTURE(int(m));
        REQUIRE(each.layers.size() == 1);
        CHECK(each.layers[0].blend == static_cast<BlendMode>(m));
    }
}

// T-327 (05 §5.9.8): background-size / background-position / background-repeat, the rounded
// padding-box clip and image-rendering.
namespace {

/// A 200 x 100 element (no border, no padding) with a 40 x 20 background image (asset 7).
NodeId backgroundElement(NodeArena& arena) {
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& data = arena.element(root);
    data.box.width = 200;
    data.box.height = 100;
    data.style.backgroundImage = 7;
    data.backgroundImageSize = Vec2{40, 20};
    return root;
}

void setBackground(ElementData& data, uint8_t repeat, Length sizeX, Length sizeY, Length posX, Length posY) {
    data.style.backgroundRepeat = repeat;
    data.style.base[prop::BackgroundSizeX] = Value::ofLength(sizeX.value, sizeX.unit);
    data.style.base[prop::BackgroundSizeY] = Value::ofLength(sizeY.value, sizeY.unit);
    data.style.base[prop::BackgroundPositionX] = Value::ofLength(posX.value, posX.unit);
    data.style.base[prop::BackgroundPositionY] = Value::ofLength(posY.value, posY.unit);
}

constexpr Length kAutoLen{0, LengthUnit::Auto};
constexpr Length kZeroPct{0, LengthUnit::Percent};

size_t countKind(const CommandList& cmds, CmdKind kind) {
    return static_cast<size_t>(std::count_if(cmds.commands.begin(), cmds.commands.end(),
                                             [kind](const Command& c) { return c.kind == kind; }));
}

}  // namespace

TEST_CASE("PaintElement: T-327 default stretch keeps the single padding-box quad") {
    NodeArena arena;
    NodeId root = backgroundElement(arena);
    ElementData& data = arena.element(root);
    data.box.border = Edges{2, 2, 2, 2};
    // Size / position are ignored while background-repeat is `stretch`.
    setBackground(data, kBackgroundRepeatStretch, Length{10, LengthUnit::Px}, kAutoLen,
                  Length{30, LengthUnit::Px}, kZeroPct);
    CommandList cmds;
    paintDump(arena, root, cmds);
    REQUIRE(cmds.images.size() == 1);
    CHECK(cmds.images[0].dst.x == doctest::Approx(2));
    CHECK(cmds.images[0].dst.y == doctest::Approx(2));
    CHECK(cmds.images[0].dst.w == doctest::Approx(196));
    CHECK(cmds.images[0].dst.h == doctest::Approx(96));
    CHECK(cmds.images[0].srcUv.w == doctest::Approx(1));
    CHECK(cmds.images[0].smooth == 1);
    CHECK(countKind(cmds, CmdKind::PushClipPath) == 0);
}

TEST_CASE("PaintElement: T-327 cover and contain") {
    NodeArena arena;
    NodeId root = backgroundElement(arena);
    ElementData& data = arena.element(root);
    data.box.height = 200;  // 200 x 200 box, 40 x 20 image.

    const Length cover{kBackgroundSizeCover, LengthUnit::Auto};
    setBackground(data, kBackgroundRepeatNone, cover, cover, Length{50, LengthUnit::Percent},
                  Length{50, LengthUnit::Percent});
    CommandList c1;
    paintDump(arena, root, c1);
    // Cover: scaled to 400 x 200, centred, so the middle half of the image is visible.
    REQUIRE(c1.images.size() == 1);
    CHECK(c1.images[0].dst.x == doctest::Approx(0));
    CHECK(c1.images[0].dst.w == doctest::Approx(200));
    CHECK(c1.images[0].dst.h == doctest::Approx(200));
    CHECK(c1.images[0].srcUv.x == doctest::Approx(0.25));
    CHECK(c1.images[0].srcUv.w == doctest::Approx(0.5));
    CHECK(c1.images[0].srcUv.h == doctest::Approx(1));

    const Length contain{kBackgroundSizeContain, LengthUnit::Auto};
    setBackground(arena.element(root), kBackgroundRepeatNone, contain, contain,
                  Length{50, LengthUnit::Percent}, Length{50, LengthUnit::Percent});
    CommandList c2;
    paintDump(arena, root, c2);
    // Contain: 200 x 100, centred vertically, the whole image.
    REQUIRE(c2.images.size() == 1);
    CHECK(c2.images[0].dst.x == doctest::Approx(0));
    CHECK(c2.images[0].dst.y == doctest::Approx(50));
    CHECK(c2.images[0].dst.w == doctest::Approx(200));
    CHECK(c2.images[0].dst.h == doctest::Approx(100));
    CHECK(c2.images[0].srcUv.w == doctest::Approx(1));
    CHECK(c2.images[0].srcUv.h == doctest::Approx(1));
}

TEST_CASE("PaintElement: T-327 px / auto sizes and percentage positions") {
    NodeArena arena;
    NodeId root = backgroundElement(arena);
    // `80px auto`: 80 x 40 (aspect kept); 100% 100% puts it in the bottom-right corner.
    setBackground(arena.element(root), kBackgroundRepeatNone, Length{80, LengthUnit::Px}, kAutoLen,
                  Length{100, LengthUnit::Percent}, Length{100, LengthUnit::Percent});
    CommandList c1;
    paintDump(arena, root, c1);
    REQUIRE(c1.images.size() == 1);
    CHECK(c1.images[0].dst.x == doctest::Approx(120));
    CHECK(c1.images[0].dst.y == doctest::Approx(60));
    CHECK(c1.images[0].dst.w == doctest::Approx(80));
    CHECK(c1.images[0].dst.h == doctest::Approx(40));

    // `auto auto` = the image's own 40 x 20; 25% of (200 - 40) = 40px, 10px down.
    setBackground(arena.element(root), kBackgroundRepeatNone, kAutoLen, kAutoLen,
                  Length{25, LengthUnit::Percent}, Length{10, LengthUnit::Px});
    CommandList c2;
    paintDump(arena, root, c2);
    REQUIRE(c2.images.size() == 1);
    CHECK(c2.images[0].dst.x == doctest::Approx(40));
    CHECK(c2.images[0].dst.y == doctest::Approx(10));
    CHECK(c2.images[0].dst.w == doctest::Approx(40));
    CHECK(c2.images[0].dst.h == doctest::Approx(20));

    // Partly outside: cropped through srcUv (no clip command), 30px of the 40px tile visible.
    setBackground(arena.element(root), kBackgroundRepeatNone, kAutoLen, kAutoLen, Length{-10, LengthUnit::Px},
                  kZeroPct);
    CommandList c3;
    paintDump(arena, root, c3);
    REQUIRE(c3.images.size() == 1);
    CHECK(c3.images[0].dst.x == doctest::Approx(0));
    CHECK(c3.images[0].dst.w == doctest::Approx(30));
    CHECK(c3.images[0].srcUv.x == doctest::Approx(0.25));
    CHECK(c3.images[0].srcUv.w == doctest::Approx(0.75));
    CHECK(countKind(c3, CmdKind::PushClipPath) == 0);
    CHECK(countKind(c3, CmdKind::PushClipRect) == 0);

    // Entirely outside: nothing.
    setBackground(arena.element(root), kBackgroundRepeatNone, kAutoLen, kAutoLen, Length{300, LengthUnit::Px},
                  kZeroPct);
    CommandList c4;
    paintDump(arena, root, c4);
    CHECK(c4.images.empty());
}

TEST_CASE("PaintElement: T-327 repeat splits into quads") {
    NodeArena arena;
    NodeId root = backgroundElement(arena);
    // 40 x 20 tiles over 200 x 100 from 0,0: 5 x 5.
    setBackground(arena.element(root), kBackgroundRepeatBoth, kAutoLen, kAutoLen, kZeroPct, kZeroPct);
    CommandList c1;
    paintDump(arena, root, c1);
    CHECK(c1.images.size() == 25);

    // Offset by 10px: 6 columns (the first and last cropped), 5 rows.
    setBackground(arena.element(root), kBackgroundRepeatBoth, kAutoLen, kAutoLen, Length{10, LengthUnit::Px},
                  kZeroPct);
    CommandList c2;
    paintDump(arena, root, c2);
    REQUIRE(c2.images.size() == 30);
    CHECK(c2.images[0].dst.x == doctest::Approx(0));
    CHECK(c2.images[0].dst.w == doctest::Approx(10));
    CHECK(c2.images[0].srcUv.x == doctest::Approx(0.75));
    CHECK(c2.images[1].dst.x == doctest::Approx(10));
    CHECK(c2.images[1].dst.w == doctest::Approx(40));

    setBackground(arena.element(root), kBackgroundRepeatX, kAutoLen, kAutoLen, kZeroPct, kZeroPct);
    CommandList c3;
    paintDump(arena, root, c3);
    CHECK(c3.images.size() == 5);

    setBackground(arena.element(root), kBackgroundRepeatY, kAutoLen, kAutoLen, kZeroPct, kZeroPct);
    CommandList c4;
    paintDump(arena, root, c4);
    CHECK(c4.images.size() == 5);
    for (const ImageCmd& ic : c4.images) {
        CHECK(ic.dst.x == doctest::Approx(0));
        CHECK(ic.asset == 7u);
    }
}

TEST_CASE("PaintElement: T-327 more than 4096 tiles falls back to stretch and flags it") {
    NodeArena arena;
    NodeId root = backgroundElement(arena);
    // 64 x 64 = 4096 tiles of 3.125 x 1.5625 px: drawn as tiles.
    setBackground(arena.element(root), kBackgroundRepeatBoth, Length{3.125f, LengthUnit::Px},
                  Length{1.5625f, LengthUnit::Px}, kZeroPct, kZeroPct);
    updateWorldTransforms(arena, root, /*force=*/true);
    Painter painter;
    CommandList c1;
    painter.paint(arena, root, c1);
    CHECK(c1.images.size() == 4096);
    CHECK_FALSE(painter.backgroundTilesCapped());

    // 1 x 1 px tiles: 20000 > 4096, one stretched quad instead.
    setBackground(arena.element(root), kBackgroundRepeatBoth, Length{1, LengthUnit::Px},
                  Length{1, LengthUnit::Px}, kZeroPct, kZeroPct);
    CommandList c2;
    painter.paint(arena, root, c2);
    REQUIRE(c2.images.size() == 1);
    CHECK(c2.images[0].dst.w == doctest::Approx(200));
    CHECK(c2.images[0].dst.h == doctest::Approx(100));
    CHECK(painter.backgroundTilesCapped());

    // Reset by the next paint.
    setBackground(arena.element(root), kBackgroundRepeatNone, kAutoLen, kAutoLen, kZeroPct, kZeroPct);
    CommandList c3;
    painter.paint(arena, root, c3);
    CHECK_FALSE(painter.backgroundTilesCapped());
}

TEST_CASE("PaintElement: T-327 a rounded box clips the image set with one PushClipPath") {
    NodeArena arena;
    NodeId root = backgroundElement(arena);
    setRadius(arena.element(root), 12);
    setBackground(arena.element(root), kBackgroundRepeatBoth, kAutoLen, kAutoLen, kZeroPct, kZeroPct);
    CommandList cmds;
    paintDump(arena, root, cmds);
    REQUIRE(countKind(cmds, CmdKind::PushClipPath) == 1);
    REQUIRE(countKind(cmds, CmdKind::PopClip) == 1);
    // PushClipPath, 25 DrawImage, PopClip -- in that order.
    REQUIRE(cmds.commands.size() == 27);
    CHECK(cmds.commands.front().kind == CmdKind::PushClipPath);
    CHECK(cmds.commands.back().kind == CmdKind::PopClip);
    const ClipPathCmd& clip = cmds.clipPaths[cmds.commands.front().index];
    CHECK(countVerb(cmds, clip.path, PathVerb::Cubic) == 4);
    CHECK(clip.rule == FillRule::NonZero);

    // The default stretch is clipped too; square corners never are.
    setBackground(arena.element(root), kBackgroundRepeatStretch, kAutoLen, kAutoLen, kZeroPct, kZeroPct);
    CommandList stretched;
    paintDump(arena, root, stretched);
    CHECK(countKind(stretched, CmdKind::PushClipPath) == 1);
    setRadius(arena.element(root), 0);
    CommandList square;
    paintDump(arena, root, square);
    CHECK(countKind(square, CmdKind::PushClipPath) == 0);
    CHECK(countKind(square, CmdKind::PopClip) == 0);
}

TEST_CASE("PaintElement: T-327 image-rendering pixelated -> smooth 0 for background and img") {
    NodeArena arena;
    NodeId root = backgroundElement(arena);
    arena.element(root).style.imageRendering = 1;
    CommandList bg;
    paintDump(arena, root, bg);
    REQUIRE(bg.images.size() == 1);
    CHECK(bg.images[0].smooth == 0);
    setBackground(arena.element(root), kBackgroundRepeatBoth, kAutoLen, kAutoLen, kZeroPct, kZeroPct);
    CommandList tiles;
    paintDump(arena, root, tiles);
    for (const ImageCmd& ic : tiles.images) {
        CHECK(ic.smooth == 0);
    }

    NodeArena imgArena;
    NodeId img = makeElement(imgArena, kInvalidNode);
    ElementData& data = imgArena.element(img);
    data.box.width = 64;
    data.box.height = 64;
    data.imageAsset = 3;
    data.style.base[prop::ImageOpacity] = Value::ofFloat(1);
    CommandList smooth;
    paintDump(imgArena, img, smooth);
    REQUIRE(smooth.images.size() == 1);
    CHECK(smooth.images[0].smooth == 1);
    imgArena.element(img).style.imageRendering = 1;
    CommandList pixelated;
    paintDump(imgArena, img, pixelated);
    REQUIRE(pixelated.images.size() == 1);
    CHECK(pixelated.images[0].smooth == 0);
}

TEST_CASE("PaintElement: T-327 the tile cap warns once per document") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc = loadDecorationDoc(
        decorationDoc("#a { width: 400px; height: 300px; background-image: url(tile.png); "
                      "background-repeat: repeat; background-size: 1px 1px; image-rendering: pixelated; }",
                      "<div id=\"a\"></div>"),
        shaper);
    ElementRef a = doc->byId("a");
    REQUIRE(a.valid());
    const ComputedStyle& style = doc->arena().element(a.node).style;
    CHECK(style.backgroundRepeat == kBackgroundRepeatBoth);
    CHECK(style.imageRendering == 1);
    CHECK(style.base[prop::BackgroundSizeX].len.value == doctest::Approx(1));
    CommandList first;
    doc->paint(first);
    CommandList second;
    doc->paint(second);
    const auto warned =
        std::count(doc->warnings().begin(), doc->warnings().end(), std::string(kWarnBackgroundTiles));
    // No resolver: the image has no known size, so it is sized as the padding box, then 1 x 1 px
    // tiles -> 120000 > 4096; warned once over the two paints.
    REQUIRE(style.backgroundImage != kInvalidAsset);
    CHECK(warned == 1);
    CHECK(first.images.size() == 1);

    // setStyle (mini_css) reaches the same properties.
    REQUIRE(a.setStyle("background-size: cover; background-position: right; background-repeat: no-repeat") ==
            Status::kOk);
    doc->update(0.0f);
    const ComputedStyle& after = doc->arena().element(a.node).style;
    CHECK(after.backgroundRepeat == kBackgroundRepeatNone);
    CHECK(after.base[prop::BackgroundSizeX].len.unit == LengthUnit::Auto);
    CHECK(after.base[prop::BackgroundSizeX].len.value == doctest::Approx(kBackgroundSizeCover));
    CHECK(after.base[prop::BackgroundPositionX].len.value == doctest::Approx(100));
    CHECK(after.base[prop::BackgroundPositionY].len.value == doctest::Approx(50));
}

// T-332 (05 §5.9.8): clip-path.
namespace {

ElementClipShape circleShape(float r) {
    ElementClipShape s;
    s.kind = ClipShapeKind::Circle;
    s.params = {Length{r, LengthUnit::Px}, Length{50, LengthUnit::Percent}, Length{50, LengthUnit::Percent}};
    return s;
}

}  // namespace

TEST_CASE("PaintElement: T-332 clip-path wraps the background and the children in PushClipPath/PopClip") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& data = arena.element(root);
    data.box.width = 200;
    data.box.height = 100;
    data.style.base[prop::BackgroundColor] = Value::ofColor(Color{1, 0, 0, 1});
    data.clipShape = circleShape(40);
    NodeId child = makeElement(arena, root);
    arena.element(child).box.width = 50;
    arena.element(child).box.height = 50;
    arena.element(child).style.base[prop::BackgroundColor] = Value::ofColor(Color{0, 1, 0, 1});
    CommandList cmds;
    paintDump(arena, root, cmds);
    // PushClipPath, FillPath (root bg), FillPath (child bg), PopClip.
    REQUIRE(cmds.commands.size() == 4);
    CHECK(cmds.commands[0].kind == CmdKind::PushClipPath);
    CHECK(cmds.commands[1].kind == CmdKind::FillPath);
    CHECK(cmds.commands[2].kind == CmdKind::FillPath);
    CHECK(cmds.commands[3].kind == CmdKind::PopClip);
    const ClipPathCmd& clip = cmds.clipPaths[cmds.commands[0].index];
    CHECK(clip.rule == FillRule::NonZero);
    // A circle: Move + 4 Cubic + Close, starting at the top of the circle (100, 10) and passing
    // through (140, 50), (100, 90), (60, 50).
    CHECK(countVerb(cmds, clip.path, PathVerb::Cubic) == 4);
    REQUIRE(clip.path.pointCount == 2 + 4 * 6);
    const float* pts = cmds.points.data() + clip.path.pointOffset;
    CHECK(pts[0] == doctest::Approx(100));
    CHECK(pts[1] == doctest::Approx(10));
    CHECK(pts[6] == doctest::Approx(140));  // End of the first arc.
    CHECK(pts[7] == doctest::Approx(50));
    CHECK(pts[12] == doctest::Approx(100));
    CHECK(pts[13] == doctest::Approx(90));
    CHECK(pts[18] == doctest::Approx(60));
    CHECK(pts[19] == doctest::Approx(50));
}

TEST_CASE("PaintElement: T-332 clip-path nests outside the overflow clip; effects stay outside both") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& data = arena.element(root);
    data.box.width = 200;
    data.box.height = 100;
    data.style.overflowX = data.style.overflowY = Overflow::Hidden;
    data.style.base[prop::BackgroundColor] = Value::ofColor(Color{1, 0, 0, 1});
    ElementClipShape poly;
    poly.kind = ClipShapeKind::Polygon;
    poly.params = {Length{0, LengthUnit::Px},        Length{0, LengthUnit::Px},
                   Length{100, LengthUnit::Percent}, Length{0, LengthUnit::Px},
                   Length{50, LengthUnit::Percent},  Length{100, LengthUnit::Percent}};
    data.clipShape = poly;
    NodeId child = makeElement(arena, root);
    arena.element(child).box.width = 50;
    arena.element(child).box.height = 50;
    arena.element(child).style.base[prop::BackgroundColor] = Value::ofColor(Color{0, 1, 0, 1});
    CommandList cmds;
    paintDump(arena, root, cmds);
    // PushClipPath (shape), FillPath (root bg), PushClipRect (overflow), FillPath (child), PopClip
    // (overflow), PopClip (shape).
    REQUIRE(cmds.commands.size() == 6);
    CHECK(cmds.commands[0].kind == CmdKind::PushClipPath);
    CHECK(cmds.commands[1].kind == CmdKind::FillPath);
    CHECK(cmds.commands[2].kind == CmdKind::PushClipRect);
    CHECK(cmds.commands[3].kind == CmdKind::FillPath);
    CHECK(cmds.commands[4].kind == CmdKind::PopClip);
    CHECK(cmds.commands[5].kind == CmdKind::PopClip);
    const ClipPathCmd& clip = cmds.clipPaths[cmds.commands[0].index];
    CHECK(clip.rule == FillRule::EvenOdd);
    REQUIRE(clip.path.pointCount == 6);
    const float* pts = cmds.points.data() + clip.path.pointOffset;
    CHECK(pts[2] == doctest::Approx(200));
    CHECK(pts[4] == doctest::Approx(100));
    CHECK(pts[5] == doctest::Approx(100));

    // With an effect, the PushEffect comes first and the clip sits inside it (18 §18.1).
    EffectInstance shadow{};
    shadow.kind = EffectKind::DropShadow;
    shadow.enabled = 1;
    shadow.params[2] = 4;  // blur radius
    arena.assignEffects(root, std::span<const EffectInstance>(&shadow, 1), {});
    CommandList fx;
    paintDump(arena, root, fx);
    REQUIRE(fx.commands.size() >= 3);
    CHECK(fx.commands[0].kind == CmdKind::PushEffect);
    CHECK(fx.commands[1].kind == CmdKind::PushClipPath);
    CHECK(fx.commands.back().kind == CmdKind::PopEffect);

    // An empty shape clips everything away with an empty rect.
    ElementClipShape empty;
    empty.kind = ClipShapeKind::Circle;
    empty.params = {Length{0, LengthUnit::Px}, Length{50, LengthUnit::Percent},
                    Length{50, LengthUnit::Percent}};
    arena.assignEffects(root, {}, {});
    arena.element(root).clipShape = empty;  // `data` may dangle after the child's creation.
    CommandList none;
    paintDump(arena, root, none);
    REQUIRE(none.commands.size() >= 2);
    CHECK(none.commands[0].kind == CmdKind::PushClipRect);
    CHECK(none.clipRects[none.commands[0].index].rect.w == 0);
}

TEST_CASE("PaintElement: T-332 damage of a change inside a clip-path is cut to the shape's box") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& data = arena.element(root);
    data.box.width = 200;
    data.box.height = 100;
    data.clipShape = circleShape(20);  // (80, 30)-(120, 70)
    NodeId child = makeElement(arena, root);
    arena.element(child).box.width = 200;
    arena.element(child).box.height = 100;
    arena.element(child).style.base[prop::BackgroundColor] = Value::ofColor(Color{0, 1, 0, 1});
    CommandList before;
    paintDump(arena, root, before);
    arena.element(child).style.base[prop::BackgroundColor] = Value::ofColor(Color{0, 0, 1, 1});
    CommandList after;
    paintDump(arena, root, after);
    DamageList damage;
    computeDamage(before, after, Rect{0, 0, 200, 100}, damage);
    const Rect box = damage.bounds();
    CHECK(box.x >= 79);
    CHECK(box.y >= 29);
    CHECK(box.x + box.w <= 121);
    CHECK(box.y + box.h <= 71);
    CHECK(box.w > 0);
}

// T-333 (05 §5.9.8): per-side border colors and `dashed` / `dotted` border styles.
namespace {

/// fnv1a(fillBytes()) of border_radius.xt painted by the pre-T-333 painter (main at e6c2b72a).
constexpr uint64_t kPreT333BorderRadiusHash = 3348850703092603236ull;

/// Every byte of `list` a backend reads for its FillPaths: the debug dump plus the raw verbs and
/// points and each fill's transform, paint color, rule and antialias flag.
std::string fillBytes(const CommandList& list) {
    std::string out;
    dumpCommandList(list, out);
    out.append(reinterpret_cast<const char*>(list.verbs.data()), list.verbs.size() * sizeof(PathVerb));
    out.append(reinterpret_cast<const char*>(list.points.data()), list.points.size() * sizeof(float));
    for (const FillCmd& f : list.fills) {
        const float xf[6] = {f.xf.a, f.xf.b, f.xf.c, f.xf.d, f.xf.e, f.xf.f};
        out.append(reinterpret_cast<const char*>(xf), sizeof(xf));
        const float c[4] = {f.paint.color.r, f.paint.color.g, f.paint.color.b, f.paint.color.a};
        out.append(reinterpret_cast<const char*>(c), sizeof(c));
        out.push_back(static_cast<char>(f.paint.kind));
        out.push_back(static_cast<char>(f.rule));
        out.push_back(static_cast<char>(f.antialias));
    }
    return out;
}

uint64_t fnv1a(const std::string& bytes) {
    uint64_t h = 1469598103934665603ull;
    for (char ch : bytes) {
        h ^= static_cast<uint8_t>(ch);
        h *= 1099511628211ull;
    }
    return h;
}

/// Tests/golden/ui/border_radius.xt (a solid one-color border with four different radii) with
/// `extra` appended to its rule.
std::string borderRadiusFixture(const std::string& extra) {
    return "<xt>\n  <head>\n    <style>\n      #a {\n        width: 150px;\n        height: 100px;\n"
           "        margin: 40px;\n        background-color: #66ccff;\n        border: 4px #333333;\n"
           "        border-top-left-radius: 30px;\n        border-top-right-radius: 5px;\n"
           "        border-bottom-right-radius: 30px;\n        border-bottom-left-radius: 5px;\n" +
           extra + "      }\n    </style>\n  </head>\n  <body>\n    <div id=\"a\"></div>\n  </body>\n</xt>\n";
}

std::string paintFixtureBytes(const std::string& extra) {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc = loadDecorationDoc(borderRadiusFixture(extra), shaper);
    CommandList list;
    doc->paint(list);
    return fillBytes(list);
}

/// A 100 x 60 element with `widths` border and every side solid in `color`.
NodeId borderBox(NodeArena& arena, Edges widths, float w = 100, float h = 60) {
    NodeId root = makeElement(arena, kInvalidNode);
    ElementData& data = arena.element(root);
    data.box.width = w;
    data.box.height = h;
    data.box.border = widths;
    data.style.base[prop::BorderColor] = Value::ofColor(Color{0.2f, 0.2f, 0.2f, 1});
    return root;
}

void setSideColors(ElementData& data, const Color colors[4]) {
    for (int i = 0; i < 4; ++i) {
        data.style.base[prop::BorderTopColor + i] = Value::ofColor(colors[i]);
    }
    data.style.borderColorSides = 0xF;
}

uint32_t contourCount(const CommandList& cmds, const PathRef& ref) {
    return countVerb(cmds, ref, PathVerb::Move);
}

}  // namespace

TEST_CASE("PaintElement: T-333 a one-color solid border is byte-identical to the pre-T-333 ring") {
    const std::string plain = paintFixtureBytes("");
    // The value recorded with the pre-T-333 painter (main at e6c2b72a) for border_radius.xt.
    CHECK(fnv1a(plain) == kPreT333BorderRadiusHash);
    // The same color written per side, as a 4-value `border-color`, or with an explicit `solid`
    // draws the very same bytes.
    CHECK(paintFixtureBytes("border-style: solid;\n") == plain);
    CHECK(paintFixtureBytes("border-color: #333333 #333333;\n") == plain);
    CHECK(paintFixtureBytes("border-top-color: #333333; border-left-style: solid;\n") == plain);
}

TEST_CASE("PaintElement: T-333 undeclared sides follow border-color, a later border-color resets them") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc = loadDecorationDoc(
        decorationDoc("#a { width: 50px; height: 30px; border: 2px #ff0000; border-top-color: #00ff00; }"
                      " #b { width: 50px; height: 30px; border-top-color: #00ff00; border: 2px #ff0000; }",
                      "<div id=\"a\"></div><div id=\"b\"></div>"),
        shaper);
    const ComputedStyle& a = doc->arena().element(doc->byId("a").node).style;
    CHECK(a.borderColorSides == 1u);
    CHECK(a.base[prop::BorderTopColor].c.g == doctest::Approx(1));
    CHECK(a.base[prop::BorderLeftColor].c.r == doctest::Approx(1));  // Copied from border-color.
    const ComputedStyle& b = doc->arena().element(doc->byId("b").node).style;
    CHECK(b.borderColorSides == 0u);  // `border` came later: every side is red again.
    CommandList list;
    doc->paint(list);
    // #a: four sides (top green, three red); #b: the single red ring.
    REQUIRE(list.fills.size() == 5);
    CHECK(list.fills[0].paint.color.g == doctest::Approx(1));
    CHECK(list.fills[1].paint.color.r == doctest::Approx(1));
    CHECK(list.fills[4].path.verbCount == 12);  // The T-201 ring: two square outlines.
}

TEST_CASE("PaintElement: T-333 four colors -> four trapezoid FillPaths split at 45 degrees") {
    NodeArena arena;
    NodeId root = borderBox(arena, Edges{4, 4, 4, 4});
    const Color colors[4] = {Color{1, 0, 0, 1}, Color{0, 1, 0, 1}, Color{0, 0, 1, 1}, Color{1, 1, 0, 1}};
    setSideColors(arena.element(root), colors);

    CommandList cmds;
    paintDump(arena, root, cmds);
    REQUIRE(cmds.fills.size() == 4);
    REQUIRE(cmds.commands.size() == 4);
    const Rect expected[4] = {Rect{0, 0, 100, 4}, Rect{96, 0, 4, 60}, Rect{0, 56, 100, 4}, Rect{0, 0, 4, 60}};
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        const FillCmd& f = cmds.fills[static_cast<size_t>(i)];
        CHECK(cmds.commands[static_cast<size_t>(i)].kind == CmdKind::FillPath);
        CHECK(f.rule == FillRule::NonZero);
        CHECK(f.paint.color.r == doctest::Approx(colors[i].r));
        CHECK(f.paint.color.g == doctest::Approx(colors[i].g));
        CHECK(f.paint.color.b == doctest::Approx(colors[i].b));
        // Square corners: Move + 3 Line + Close, a trapezoid.
        CHECK(f.path.verbCount == 5);
        CHECK(countVerb(cmds, f.path, PathVerb::Cubic) == 0);
        const Rect r = pointsBounds(cmds, f.path, 0, f.path.pointCount);
        CHECK(r.x == doctest::Approx(expected[i].x));
        CHECK(r.y == doctest::Approx(expected[i].y));
        CHECK(r.w == doctest::Approx(expected[i].w));
        CHECK(r.h == doctest::Approx(expected[i].h));
    }
    // The top trapezoid's inner edge runs from the inner corner (4, 4) to (96, 4): the corner
    // squares are split along their diagonals.
    const FillCmd& top = cmds.fills[0];
    const float* p = &cmds.points[top.path.pointOffset];
    CHECK(p[0] == doctest::Approx(0));  // Outer TL corner.
    CHECK(p[1] == doctest::Approx(0));
    CHECK(p[2] == doctest::Approx(100));  // Outer TR corner.
    CHECK(p[3] == doctest::Approx(0));
    CHECK(p[4] == doctest::Approx(96));  // Inner TR corner.
    CHECK(p[5] == doctest::Approx(4));
    CHECK(p[6] == doctest::Approx(4));  // Inner TL corner.
    CHECK(p[7] == doctest::Approx(4));
}

TEST_CASE("PaintElement: T-333 rounded four-color border: each side has its two half arcs") {
    NodeArena arena;
    NodeId root = borderBox(arena, Edges{4, 4, 4, 4});
    ElementData& data = arena.element(root);
    for (PropId p = prop::BorderRadiusTL; p <= prop::BorderRadiusBL; ++p) {
        data.style.base[p] = Value::ofLength(12, LengthUnit::Px);
    }
    const Color colors[4] = {Color{1, 0, 0, 1}, Color{0, 1, 0, 1}, Color{0, 0, 1, 1}, Color{1, 1, 0, 1}};
    setSideColors(data, colors);
    CommandList cmds;
    paintDump(arena, root, cmds);
    REQUIRE(cmds.fills.size() == 4);
    for (const FillCmd& f : cmds.fills) {
        // Outer and inner: half an arc at each end of the side (8 radius > 0 inside too).
        CHECK(countVerb(cmds, f.path, PathVerb::Cubic) == 4);
    }
    // The top side starts at the outer arc's 45-degree point.
    const float* p = &cmds.points[cmds.fills[0].path.pointOffset];
    const float k = 12.0f - 12.0f * 0.70710678f;
    CHECK(p[0] == doctest::Approx(k));
    CHECK(p[1] == doctest::Approx(k));
}

TEST_CASE("PaintElement: T-333 a one-color border with a `none` side is drawn per side") {
    NodeArena arena;
    NodeId root = borderBox(arena, Edges{4, 4, 4, 4});
    arena.element(root).style.borderStyle[1] = kBorderStyleNone;
    CommandList cmds;
    paintDump(arena, root, cmds);
    CHECK(cmds.fills.size() == 3);  // Top, bottom, left.
}

TEST_CASE("PaintElement: T-333 dashed side: 3x width dashes, 2x width gaps") {
    NodeArena arena;
    NodeId root = borderBox(arena, Edges{2, 0, 0, 0}, 100, 20);
    arena.element(root).style.borderStyle[0] = kBorderStyleDashed;
    CommandList cmds;
    paintDump(arena, root, cmds);
    REQUIRE(cmds.fills.size() == 1);
    const FillCmd& f = cmds.fills[0];
    CHECK(f.rule == FillRule::NonZero);
    // 100 px of centerline, period 10 (6 on, 4 off): dashes at 0, 10, ..., 90.
    CHECK(contourCount(cmds, f.path) == 10);
    const Rect r = pointsBounds(cmds, f.path, 0, f.path.pointCount);
    CHECK(r.x == doctest::Approx(0));
    CHECK(r.y == doctest::Approx(0));
    CHECK(r.w == doctest::Approx(96));
    CHECK(r.h == doctest::Approx(2));
    // The second dash covers x 10..16.
    uint32_t moves = 0;
    float minX = 1e9f, maxX = -1e9f;
    uint32_t pt = f.path.pointOffset;
    for (uint32_t i = 0; i < f.path.verbCount; ++i) {
        const PathVerb v = cmds.verbs[f.path.verbOffset + i];
        if (v == PathVerb::Close) {
            continue;
        }
        if (v == PathVerb::Move) {
            ++moves;
        }
        if (moves == 2) {
            minX = std::min(minX, cmds.points[pt]);
            maxX = std::max(maxX, cmds.points[pt]);
        }
        pt += 2;
    }
    CHECK(minX == doctest::Approx(10));
    CHECK(maxX == doctest::Approx(16));
}

TEST_CASE("PaintElement: T-333 dashed square corner: the end dashes fill their half of the corner") {
    NodeArena arena;
    NodeId root = borderBox(arena, Edges{4, 4, 4, 4}, 100, 60);
    for (uint8_t& s : arena.element(root).style.borderStyle) {
        s = kBorderStyleDashed;
    }
    CommandList cmds;
    paintDump(arena, root, cmds);
    REQUIRE(cmds.fills.size() == 4);
    // The top side's first dash reaches the outer corner (0, 0) and is cut by the diagonal.
    const Rect r = pointsBounds(cmds, cmds.fills[0].path, 0, cmds.fills[0].path.pointCount);
    CHECK(r.x == doctest::Approx(0));
    CHECK(r.y == doctest::Approx(0));
    CHECK(r.h == doctest::Approx(4));
    // Nothing leaves the border box.
    for (const FillCmd& f : cmds.fills) {
        const Rect b = pointsBounds(cmds, f.path, 0, f.path.pointCount);
        CHECK(b.x >= -1e-3f);
        CHECK(b.y >= -1e-3f);
        CHECK(b.x + b.w <= 100.001f);
        CHECK(b.y + b.h <= 60.001f);
    }
}

TEST_CASE("PaintElement: T-333 rounded dotted border: round dots that follow the corner arcs") {
    NodeArena arena;
    NodeId root = borderBox(arena, Edges{4, 4, 4, 4}, 100, 100);
    ElementData& data = arena.element(root);
    for (PropId p = prop::BorderRadiusTL; p <= prop::BorderRadiusBL; ++p) {
        data.style.base[p] = Value::ofLength(20, LengthUnit::Px);
    }
    for (uint8_t& s : data.style.borderStyle) {
        s = kBorderStyleDotted;
    }
    CommandList cmds;
    paintDump(arena, root, cmds);
    REQUIRE(cmds.fills.size() == 4);
    const FillCmd& top = cmds.fills[0];
    // Centerline radius 18: 60 px straight + two 45-degree arcs of 18 * pi / 4 = 88.27 px; one dot
    // every 8 px (4 px dot, 4 px gap) from 0 to 88.
    CHECK(contourCount(cmds, top.path) == 12);
    const Rect r = pointsBounds(cmds, top.path, 0, top.path.pointCount);
    // The end dots sit on the arcs' 45-degree points (y = 20 - 18 / sqrt 2 = 7.27), radius 2.
    CHECK(r.y >= -1e-3f);
    CHECK(r.y + r.h > 8.0f);
    CHECK(r.y + r.h < 9.6f);
    // Every point stays inside the ring's band around the centerline (2 px either side).
    for (uint32_t i = 0; i < top.path.pointCount; i += 2) {
        const float x = cmds.points[top.path.pointOffset + i];
        const float y = cmds.points[top.path.pointOffset + i + 1];
        float d = 0.0f;  // Distance from the centerline.
        if (x < 20.0f) {
            d = std::fabs(std::hypot(x - 20.0f, y - 20.0f) - 18.0f);
        } else if (x > 80.0f) {
            d = std::fabs(std::hypot(x - 80.0f, y - 20.0f) - 18.0f);
        } else {
            d = std::fabs(y - 2.0f);
        }
        CHECK(d <= 2.3f);
    }
}
