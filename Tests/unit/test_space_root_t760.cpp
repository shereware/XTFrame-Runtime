#include <cstdint>
#include <span>
#include <vector>

#include "doctest.h"
#include "xt/command_list.h"
#include "xt/geom.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/painter.h"
#include "xt/property.h"
#include "xt/sw/sw_renderer.h"
#include "xt/types.h"
#include "xt/value.h"

/// T-760 (Docs/tasks/T-760.md): a 3D space root that is not at the origin paints its own box (its
/// background, its overflow clip) once at its own position, as a browser does -- not translated a
/// second time by the projection's rootXf; and the Painter's current 3D layer is restored when a
/// nested space ends (item A). Pixels are checked against xt::sw.

using namespace xt;

namespace {

constexpr float kViewW = 200;
constexpr float kViewH = 160;

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

CommandList paintScene(NodeArena& arena, NodeId root) {
    updateWorldTransforms(arena, root, /*force=*/true);
    CommandList cl;
    cl.viewport = Rect{0, 0, kViewW, kViewH};
    cl.devicePixelRatio = 1;
    Painter painter;
    painter.setBoxShadowCommands(false);
    painter.paint(arena, root, cl);
    return cl;
}

std::vector<uint8_t> renderSw(const CommandList& cl) {
    const int w = static_cast<int>(kViewW), h = static_cast<int>(kViewH);
    std::vector<uint8_t> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    sw::SwRenderer renderer;
    renderer.setTarget(sw::SwTarget{buf.data(), w, h, w * 4});
    renderer.clear(Color{1, 1, 1, 1});
    renderer.render(cl, nullptr, nullptr);
    return buf;
}

struct Px {
    uint8_t r, g, b;
};

Px pixelAt(const std::vector<uint8_t>& buf, int x, int y) {
    const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(kViewW) + static_cast<size_t>(x)) * 4;
    return Px{buf[i], buf[i + 1], buf[i + 2]};
}

bool isRed(Px p) { return p.r > 200 && p.g < 60 && p.b < 60; }
bool isBlue(Px p) { return p.r < 60 && p.g < 60 && p.b > 200; }
bool isBackdrop(Px p) { return p.r > 200 && p.g > 200 && p.b > 200; }

/// A white backdrop and a red 80 x 50 perspective stage at (60, 60) holding a blue 20 x 10 box at
/// (50, 30) of the stage -- in a browser the stage covers [60, 140] x [60, 110] and the box
/// [110, 130] x [90, 100].
struct OffsetStage {
    NodeArena arena;
    NodeId root = kInvalidNode;
    NodeId stage = kInvalidNode;
    NodeId box = kInvalidNode;
    explicit OffsetStage(bool clip = false, Vec2 boxAt = Vec2{50, 30}) {
        root = makeElement(arena, kInvalidNode, 0, 0, kViewW, kViewH, Color{1, 1, 1, 1});
        stage = makeElement(arena, root, 60, 60, 80, 50, Color{1, 0, 0, 1});
        arena.element(stage).perspectivePx = 300;
        arena.element(stage).perspectiveOriginPx = Vec2{40, 25};
        if (clip) {
            arena.element(stage).style.overflowX = Overflow::Hidden;
            arena.element(stage).style.overflowY = Overflow::Hidden;
        }
        box = makeElement(arena, stage, boxAt.x, boxAt.y, 20, 10, Color{0, 0, 1, 1});
    }
};

}  // namespace

TEST_CASE("space root (T-760): a perspective root at (60, 60) paints its background there, once") {
    OffsetStage s;
    const CommandList cl = paintScene(s.arena, s.root);
    REQUIRE(cl.projections.size() == 1u);
    const std::vector<uint8_t> px = renderSw(cl);
    CHECK(isRed(pixelAt(px, 65, 65)));         // inside the stage, left of the box
    CHECK(isRed(pixelAt(px, 135, 105)));       // the stage's bottom-right corner
    CHECK(isBlue(pixelAt(px, 120, 95)));       // the box
    CHECK(isBackdrop(pixelAt(px, 150, 120)));  // right of and below the stage: nothing
    CHECK(isBackdrop(pixelAt(px, 190, 150)));
    // The projection's hint is the stage's own extent (the background is the widest thing in it).
    const Rect hint = cl.projections[0].boundsHint;
    CHECK(hint.x == doctest::Approx(60.0f));
    CHECK(hint.y == doctest::Approx(60.0f));
    CHECK(hint.w == doctest::Approx(80.0f));
    CHECK(hint.h == doctest::Approx(50.0f));
}

TEST_CASE("space root (T-760): the same stage at the origin is unchanged, translated by (60, 60)") {
    // The root's own commands and its children's agree: drawing the stage at (0, 0) and at
    // (60, 60) gives the same picture moved by (60, 60).
    OffsetStage moved;
    OffsetStage origin;
    origin.arena.common(origin.stage).translate = Vec2{0, 0};
    const std::vector<uint8_t> a = renderSw(paintScene(moved.arena, moved.root));
    const std::vector<uint8_t> b = renderSw(paintScene(origin.arena, origin.root));
    for (int y = 0; y < 50; ++y) {
        for (int x = 0; x < 80; ++x) {
            const Px pa = pixelAt(a, x + 60, y + 60);
            const Px pb = pixelAt(b, x, y);
            CAPTURE(x);
            CAPTURE(y);
            REQUIRE((pa.r == pb.r && pa.g == pb.g && pa.b == pb.b));
        }
    }
}

TEST_CASE("space root (T-760): the root's overflow clip sits on its own box") {
    // The box at (0, 0) of the stage: inside the stage's clip, so drawn at (60, 60).
    OffsetStage s(/*clip=*/true, Vec2{0, 0});
    const std::vector<uint8_t> px = renderSw(paintScene(s.arena, s.root));
    CHECK(isBlue(pixelAt(px, 65, 65)));
    CHECK(isRed(pixelAt(px, 100, 80)));
    CHECK(isBackdrop(pixelAt(px, 150, 120)));
}

// -------------------------------------------------------------------------------------------
// Item A: Painter::m_currentLayer after a nested space root ends.
// -------------------------------------------------------------------------------------------

namespace {

EffectInstance makeShadow() {
    EffectInstance e{};
    e.kind = EffectKind::DropShadow;
    e.enabled = 1;
    e.params[0] = 4;
    e.params[1] = 4;
    e.params[2] = 6;
    e.params[4] = 1.0f;
    e.colors[0] = Color{0, 0, 0, 0.75f};
    return e;
}

/// An outer perspective root (the whole view) > a 3D layer L (rotateY(`turn`)) > [a nested
/// perspective root N holding one 3D child (when `nested`), a sibling S].
struct NestedLayerScene {
    NodeArena arena;
    NodeId root = kInvalidNode;
    NodeId sibling = kInvalidNode;
    NestedLayerScene(bool nested, float turn, bool siblingShadow, bool siblingBackfaceHidden) {
        root = makeElement(arena, kInvalidNode, 0, 0, kViewW, kViewH, Color{1, 1, 1, 1});
        const NodeId outer = makeElement(arena, root, 0, 0, kViewW, kViewH, Color{0, 0, 0, 0});
        arena.element(outer).perspectivePx = 500;
        arena.element(outer).perspectiveOriginPx = Vec2{kViewW / 2, kViewH / 2};
        const NodeId layer = makeElement(arena, outer, 30, 20, 140, 120, Color{0.8f, 0.8f, 0.6f, 1});
        arena.common(layer).is3D = 1;
        arena.common(layer).rotationY = turn;
        arena.common(layer).anchor = Vec2{70, 60};
        if (nested) {
            const NodeId inner = makeElement(arena, layer, 10, 10, 50, 40, Color{0.2f, 0.6f, 0.2f, 1});
            arena.element(inner).perspectivePx = 200;
            arena.element(inner).perspectiveOriginPx = Vec2{25, 20};
            const NodeId tile = makeElement(arena, inner, 5, 5, 30, 20, Color{0.9f, 0.4f, 0.1f, 1});
            arena.common(tile).is3D = 1;
            arena.common(tile).translateZ = 30;
            arena.common(tile).rotationX = 25;
        }
        sibling = makeElement(arena, layer, 80, 60, 40, 30, Color{0.1f, 0.2f, 0.9f, 1});
        if (siblingShadow) {
            const EffectInstance fx = makeShadow();
            REQUIRE(arena.assignEffects(sibling, std::span<const EffectInstance>(&fx, 1), {}) == Status::kOk);
        }
        if (siblingBackfaceHidden) {
            arena.element(sibling).style.backfaceHidden = 1;
            arena.common(sibling).backfaceHidden = 1;
        }
    }
};

}  // namespace

TEST_CASE("space root (T-760 A): a sibling after a nested space keeps its layer's plane") {
    NestedLayerScene with(/*nested=*/true, 40.0f, /*siblingShadow=*/true, false);
    NestedLayerScene without(/*nested=*/false, 40.0f, /*siblingShadow=*/true, false);
    const CommandList a = paintScene(with.arena, with.root);
    const CommandList b = paintScene(without.arena, without.root);
    REQUIRE(a.effectCmds.size() == 1u);
    REQUIRE(b.effectCmds.size() == 1u);
    const EffectCmd& fa = a.effectCmds[0];
    const EffectCmd& fb = b.effectCmds[0];
    CHECK(fa.xf.a == doctest::Approx(fb.xf.a));
    CHECK(fa.xf.b == doctest::Approx(fb.xf.b));
    CHECK(fa.xf.c == doctest::Approx(fb.xf.c));
    CHECK(fa.xf.d == doctest::Approx(fb.xf.d));
    CHECK(fa.xf.e == doctest::Approx(fb.xf.e));
    CHECK(fa.xf.f == doctest::Approx(fb.xf.f));
    CHECK(fa.contentBounds.x == doctest::Approx(fb.contentBounds.x));
    CHECK(fa.contentBounds.y == doctest::Approx(fb.contentBounds.y));
    CHECK(fa.contentBounds.w == doctest::Approx(fb.contentBounds.w));
    CHECK(fa.contentBounds.h == doctest::Approx(fb.contentBounds.h));
    CHECK(fa.boundsHint.x == doctest::Approx(fb.boundsHint.x));
    CHECK(fa.boundsHint.w == doctest::Approx(fb.boundsHint.w));
}

TEST_CASE("space root (T-760 A): a backface-hidden sibling after a nested space is still culled") {
    // L turned by rotateY(180deg): S (backface-visibility: hidden, flat on L's plane) faces away.
    NestedLayerScene with(/*nested=*/true, 180.0f, false, /*siblingBackfaceHidden=*/true);
    NestedLayerScene without(/*nested=*/false, 180.0f, false, /*siblingBackfaceHidden=*/true);
    NestedLayerScene shown(/*nested=*/false, 180.0f, false, /*siblingBackfaceHidden=*/false);
    const CommandList a = paintScene(with.arena, with.root);
    const CommandList b = paintScene(without.arena, without.root);
    const CommandList c = paintScene(shown.arena, shown.root);
    REQUIRE(c.fills.size() == b.fills.size() + 1);  // the oracle: without N, S is culled
    // With N, S is culled too: the fills are those of the scene without N plus N's own two.
    CHECK(a.fills.size() == b.fills.size() + 2);
}
