#include <cstdint>
#include <iterator>
#include <vector>

#include "damage_internal.h"  // Code/Runtime/core/src: computeDamage
#include "doctest.h"
#include "xt/command_list.h"
#include "xt/damage.h"
#include "xt/geom.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/painter.h"
#include "xt/property.h"
#include "xt/sw/sw_renderer.h"
#include "xt/types.h"
#include "xt/value.h"

/// T-760 (Docs/tasks/T-760.md items B and C, 10 §10.16 W6): the "whole frame" threshold compares
/// the union area of the damage rectangles, not the sum; inside a projection, a moving 3D layer and
/// the commands drawn on a plane damage their projected extent on screen. Checked against the pixels
/// xt::sw draws: every pixel that changes lies in a damage rectangle.

using namespace xt;

namespace {

constexpr float kViewW = 400;
constexpr float kViewH = 300;

CommandList emptyList() {
    CommandList list;
    list.viewport = Rect{0, 0, kViewW, kViewH};
    list.devicePixelRatio = 1;
    return list;
}

void addBox(CommandList& list, float x, float y, float w, float h, Color color) {
    PathRef ref{};
    ref.verbOffset = static_cast<uint32_t>(list.verbs.size());
    ref.pointOffset = static_cast<uint32_t>(list.points.size());
    const PathVerb verbs[] = {PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Line,
                              PathVerb::Close};
    const float pts[] = {x, y, x + w, y, x + w, y + h, x, y + h};
    list.verbs.insert(list.verbs.end(), std::begin(verbs), std::end(verbs));
    list.points.insert(list.points.end(), std::begin(pts), std::end(pts));
    ref.verbCount = 5;
    ref.pointCount = 8;
    FillCmd f{};
    f.xf = Mat2x3{};
    f.path = ref;
    f.paint.kind = PaintKind::Solid;
    f.paint.color = color;
    f.rule = FillRule::NonZero;
    list.fills.push_back(f);
    list.commands.push_back(Command{CmdKind::FillPath, {}, static_cast<uint32_t>(list.fills.size() - 1)});
}

float sumArea(const DamageList& d) {
    float a = 0;
    for (const Rect& r : d.rects) {
        a += r.w * r.h;
    }
    return a;
}

}  // namespace

TEST_CASE("damage (T-760 B): two heavily overlapping rectangles whose union is under the threshold") {
    // Each 250 x 250 = 62,500 px (sum 125,000 > 0.6 x 120,000); offset by 10 px, their union is
    // 67,400 px < 72,000.
    CommandList prev = emptyList();
    addBox(prev, 0, 0, kViewW, kViewH, Color{1, 1, 1, 1});
    CommandList cur = prev;
    addBox(cur, 20, 20, 250, 250, Color{1, 0, 0, 1});
    addBox(cur, 30, 30, 250, 250, Color{0, 0, 1, 1});
    DamageList d;
    computeDamage(prev, cur, Rect{0, 0, kViewW, kViewH}, d);
    CHECK_FALSE(d.full);
    REQUIRE(d.rects.size() == 2u);
    CHECK(sumArea(d) > kFullDamageAreaRatio * kViewW * kViewH);  // the case the sum misjudged
}

TEST_CASE("damage (T-760 B): disjoint rectangles judge as before") {
    SUBCASE("under the threshold") {
        CommandList prev = emptyList();
        addBox(prev, 0, 0, kViewW, kViewH, Color{1, 1, 1, 1});
        CommandList cur = prev;
        addBox(cur, 0, 0, 190, 140, Color{1, 0, 0, 1});
        addBox(cur, 200, 150, 190, 140, Color{0, 0, 1, 1});
        DamageList d;
        computeDamage(prev, cur, Rect{0, 0, kViewW, kViewH}, d);
        CHECK_FALSE(d.full);
        CHECK(d.rects.size() == 2u);
    }
    SUBCASE("over the threshold") {
        CommandList prev = emptyList();
        addBox(prev, 0, 0, kViewW, kViewH, Color{1, 1, 1, 1});
        CommandList cur = prev;
        addBox(cur, 0, 0, 390, 140, Color{1, 0, 0, 1});
        addBox(cur, 0, 150, 390, 140, Color{0, 0, 1, 1});
        DamageList d;
        computeDamage(prev, cur, Rect{0, 0, kViewW, kViewH}, d);
        CHECK(d.full);
        CHECK(d.rects.empty());
    }
}

// -------------------------------------------------------------------------------------------
// Item C: 3D layers inside a projection.
// -------------------------------------------------------------------------------------------

namespace {

NodeId makeElement(NodeArena& arena, NodeId parent, float x, float y, float w, float h, Color background) {
    const NodeId id = arena.create(NodeKind::Element);
    if (parent != kInvalidNode) {
        arena.appendChild(parent, id);
    }
    ElementData& data = arena.element(id);
    data.box.width = w;
    data.box.height = h;
    data.style.base[prop::BackgroundColor] = Value::ofColor(background);
    arena.common(id).translate = Vec2{x, y};
    return id;
}

/// A white backdrop and a perspective stage at `stageAt` holding two 3D layers: a large one
/// (rotateY(15deg)) and, in front of it and always inside its projected extent, a small one.
struct TwoLayers {
    NodeArena arena;
    NodeId root = kInvalidNode;
    NodeId small = kInvalidNode;
    explicit TwoLayers(Vec2 stageAt = Vec2{0, 0}) {
        root = makeElement(arena, kInvalidNode, 0, 0, kViewW, kViewH, Color{1, 1, 1, 1});
        const NodeId stage = makeElement(arena, root, stageAt.x, stageAt.y, 300, 220, Color{0, 0, 0, 0});
        arena.element(stage).perspectivePx = 600;
        arena.element(stage).perspectiveOriginPx = Vec2{150, 110};
        const NodeId big = makeElement(arena, stage, 20, 20, 260, 180, Color{0.7f, 0.75f, 0.8f, 1});
        arena.common(big).is3D = 1;
        arena.common(big).rotationY = 15;
        small = makeElement(arena, stage, 60, 60, 40, 30, Color{0.9f, 0.2f, 0.1f, 1});
        arena.common(small).is3D = 1;
        arena.common(small).translateZ = 40;
        arena.common(small).rotationX = 20;
    }
    CommandList paint() {
        updateWorldTransforms(arena, root, /*force=*/true);
        CommandList cl = emptyList();
        Painter painter;
        painter.paint(arena, root, cl);
        return cl;
    }
};

std::vector<uint8_t> renderFull(const CommandList& list) {
    const int w = static_cast<int>(kViewW);
    const int h = static_cast<int>(kViewH);
    std::vector<uint8_t> px(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    sw::SwRenderer renderer;
    renderer.setTarget(sw::SwTarget{px.data(), w, h, w * 4});
    renderer.clear(Color{1, 1, 1, 1});
    renderer.render(list, nullptr, nullptr);
    return px;
}

struct PixelDamage {
    int changed = 0;  ///< pixels that differ between the two full renders
    int outside = 0;  ///< of those, the ones no damage rectangle contains
    DamageList damage;
};

PixelDamage pixelDamage(const CommandList& prev, const CommandList& cur) {
    PixelDamage result;
    computeDamage(prev, cur, Rect{0, 0, kViewW, kViewH}, result.damage);
    const std::vector<uint8_t> a = renderFull(prev);
    const std::vector<uint8_t> b = renderFull(cur);
    const int w = static_cast<int>(kViewW);
    const int h = static_cast<int>(kViewH);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
            if (a[i] == b[i] && a[i + 1] == b[i + 1] && a[i + 2] == b[i + 2] && a[i + 3] == b[i + 3]) {
                continue;
            }
            ++result.changed;
            bool inside = result.damage.full;
            for (const Rect& r : result.damage.rects) {
                inside = inside || (static_cast<float>(x) >= r.x && static_cast<float>(x + 1) <= r.x + r.w &&
                                    static_cast<float>(y) >= r.y && static_cast<float>(y + 1) <= r.y + r.h);
            }
            result.outside += inside ? 0 : 1;
        }
    }
    return result;
}

}  // namespace

TEST_CASE("damage (T-760 C): a 3D layer moving inside another layer's extent damages both positions") {
    for (const Vec2 stageAt : {Vec2{0, 0}, Vec2{70, 50}}) {
        CAPTURE(stageAt.x);
        TwoLayers s(stageAt);
        const CommandList prev = s.paint();
        s.arena.common(s.small).translate = Vec2{150, 110};
        const CommandList cur = s.paint();
        REQUIRE(prev.projections.size() == 1u);
        // The projection's own extent does not change (the small layer stays inside the big one).
        REQUIRE(prev.projections[0].boundsHint.x == cur.projections[0].boundsHint.x);
        REQUIRE(prev.projections[0].boundsHint.w == cur.projections[0].boundsHint.w);
        const PixelDamage p = pixelDamage(prev, cur);
        CHECK(p.changed > 0);
        CHECK_FALSE(p.damage.full);
        CHECK(p.outside == 0);
        CHECK(sumArea(p.damage) < 0.5f * prev.projections[0].boundsHint.w * prev.projections[0].boundsHint.h);
    }
}

TEST_CASE("damage (T-760 C): a color change inside a 3D layer damages its projected position") {
    for (const Vec2 stageAt : {Vec2{0, 0}, Vec2{70, 50}}) {
        CAPTURE(stageAt.x);
        TwoLayers s(stageAt);
        s.arena.common(s.small).translate = Vec2{150, 110};
        const CommandList prev = s.paint();
        s.arena.element(s.small).style.base[prop::BackgroundColor] =
            Value::ofColor(Color{0.1f, 0.8f, 0.2f, 1});
        const CommandList cur = s.paint();
        const PixelDamage p = pixelDamage(prev, cur);
        CHECK(p.changed > 0);
        CHECK_FALSE(p.damage.full);
        CHECK(p.outside == 0);
        // Not the projection's whole extent: the layer's own projected box.
        CHECK(sumArea(p.damage) <
              0.25f * prev.projections[0].boundsHint.w * prev.projections[0].boundsHint.h);
    }
}

TEST_CASE("damage (T-760 C): a change on the root plane of an offset stage damages where it is drawn") {
    // A flat (non-3D) box on the stage's own plane: its commands are in the stage's coordinates.
    NodeArena arena;
    const NodeId root = makeElement(arena, kInvalidNode, 0, 0, kViewW, kViewH, Color{1, 1, 1, 1});
    const NodeId stage = makeElement(arena, root, 120, 90, 200, 150, Color{0, 0, 0, 0});
    arena.element(stage).perspectivePx = 500;
    arena.element(stage).perspectiveOriginPx = Vec2{100, 75};
    const NodeId flat = makeElement(arena, stage, 10, 10, 40, 30, Color{0.2f, 0.3f, 0.9f, 1});
    const NodeId layer = makeElement(arena, stage, 80, 40, 60, 50, Color{0.8f, 0.8f, 0.2f, 1});
    arena.common(layer).is3D = 1;
    arena.common(layer).rotationY = 30;
    auto paint = [&] {
        updateWorldTransforms(arena, root, /*force=*/true);
        CommandList cl = emptyList();
        Painter painter;
        painter.paint(arena, root, cl);
        return cl;
    };
    const CommandList prev = paint();
    arena.element(flat).style.base[prop::BackgroundColor] = Value::ofColor(Color{0.9f, 0.1f, 0.1f, 1});
    const CommandList cur = paint();
    const PixelDamage p = pixelDamage(prev, cur);
    CHECK(p.changed > 0);
    CHECK(p.outside == 0);
}
