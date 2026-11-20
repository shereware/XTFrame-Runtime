#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "compile_check.h"
#include "doctest.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/damage.h"
#include "xt/document.h"
#include "xt/geom.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/painter.h"
#include "xt/property.h"
#include "xt/sw/sw_renderer.h"
#include "xt/text_shaper.h"
#include "xt/types.h"
#include "xt/value.h"

/// T-754 (Docs/design/10-backends.md §10.20, Docs/tasks/T-754.md "測試"): effects and layers whose
/// bounds -- their own reach and their descendants' included -- lie wholly outside the viewport are
/// left out by the Painter; a layer's bounds take in its descendants' effect reach; known bounds are
/// flagged (boundsKnown), 3D spaces keep the old unknown bounds. Pixels are checked against xt::sw.

using namespace xt;

namespace {

constexpr float kViewW = 400;
constexpr float kViewH = 300;

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

/// `shadow(ox oy blur)`: reach max(|ox|, |oy|) + 3 x blur / 2.
EffectInstance makeShadow(float ox, float oy, float blur) {
    EffectInstance e{};
    e.kind = EffectKind::DropShadow;
    e.enabled = 1;
    e.params[0] = ox;
    e.params[1] = oy;
    e.params[2] = blur;
    e.params[4] = 1.0f;
    e.colors[0] = Color{0, 0, 0, 0.75f};
    return e;
}

void addShadow(NodeArena& arena, NodeId n, EffectInstance fx) {
    REQUIRE(arena.assignEffects(n, std::span<const EffectInstance>(&fx, 1), {}) == Status::kOk);
}

/// Paints `root` with the given viewport (T-668's box shadow commands off, so the shadows stay
/// PushEffects).
CommandList paintWith(NodeArena& arena, NodeId root, Rect viewport) {
    updateWorldTransforms(arena, root, /*force=*/true);
    CommandList cl;
    cl.viewport = viewport;
    Painter painter;
    painter.setBoxShadowCommands(false);
    painter.paint(arena, root, cl);
    return cl;
}

CommandList paintView(NodeArena& arena, NodeId root) {
    return paintWith(arena, root, Rect{0, 0, kViewW, kViewH});
}

/// "Not culled, drawn as is": the same scene painted with a viewport large enough that nothing is
/// clamped or left out, then drawn into the real viewport.
CommandList paintUnclamped(NodeArena& arena, NodeId root) {
    CommandList cl = paintWith(arena, root, Rect{-10000, -10000, 20000, 20000});
    cl.viewport = Rect{0, 0, kViewW, kViewH};
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

size_t countKind(const CommandList& cl, CmdKind kind) {
    size_t n = 0;
    for (const Command& c : cl.commands) {
        n += c.kind == kind ? 1u : 0u;
    }
    return n;
}

/// A backdrop element filling the viewport and one 40 x 30 card with `shadow(4px 4px 8px)` (reach
/// 4 + 12 = 16 px) at (`x`, `y`).
struct CardScene {
    NodeArena arena;
    NodeId root = kInvalidNode;
    NodeId card = kInvalidNode;
    CardScene(float x, float y) {
        root = makeElement(arena, kInvalidNode, 0, 0, kViewW, kViewH, Color{0.9f, 0.85f, 0.7f, 1});
        card = makeElement(arena, root, x, y, 40, 30, Color{0.2f, 0.4f, 0.9f, 1});
        addShadow(arena, card, makeShadow(4, 4, 8));
    }
};

// -------------------------------------------------------------------------------------------
// A Context harness for the document-level cases (same as Tests/unit/test_damage.cpp's).
// -------------------------------------------------------------------------------------------

struct Harness {
    NullTextShaper shaper;
    FileAssetResolver assets;
    Context ctx;
    CommandList list;
    DamageList damage;
    DocumentHandle handle = 0;

    explicit Harness(const std::string& xtText)
        : assets(XT_TEST_LAYOUT_DIR, nullptr), ctx(ContextConfig{&shaper, &assets}) {
        compile::SourceFile files[1] = {compile::SourceFile{"main.xt", xtText}};
        compile::CompileOptions options;
        compile::CompileResult result = compile::compileDocumentFromStrings(files, "main.xt", options);
        test::requireCompiled(result);
        Result<DocumentHandle> loaded =
            ctx.loadDocument(IrBlob{result.ir.data(), result.ir.size()}, /*takeOwnership=*/true);
        REQUIRE(loaded.ok());
        handle = loaded.value();
        ctx.setBoxShadowCommands(false);  // the shadows stay PushEffects
        ctx.setViewport(Rect{0, 0, kViewW, kViewH}, 1.0f);
    }

    void frame(float dt = 0.0f) {
        ctx.update(dt);
        ctx.render(list, &damage);
    }

    void setStyle(const char* id, const char* declarations) {
        Document* d = ctx.document(handle);
        REQUIRE(d != nullptr);
        ElementRef ref = d->byId(id);
        REQUIRE(ref.valid());
        CHECK(ref.setStyle(declarations) == Status::kOk);
    }
};

}  // namespace

TEST_CASE("offscreen: a shadowed element wholly outside the viewport is left out, in every direction") {
    // Reach 16 px: the card plus its shadow spans [x - 16, x + 40 + 16] x [y - 16, y + 30 + 16].
    const struct {
        const char* name;
        float x, y;
    } cases[] = {
        {"left", -57, 100},
        {"right", 417, 100},
        {"top", 100, -47},
        {"bottom", 100, 317},
        // Exactly touching the edge: the clamped bounds are empty -- still nothing to draw.
        {"left, touching", -56, 100},
        {"right, touching", 416, 100},
    };
    for (const auto& c : cases) {
        CAPTURE(c.name);
        CardScene s(c.x, c.y);
        const CommandList cl = paintView(s.arena, s.root);
        CHECK(countKind(cl, CmdKind::PushEffect) == 0u);
        CHECK(countKind(cl, CmdKind::FillPath) == 1u);  // the backdrop only
        CHECK(cl.culledOffscreen == 1u);
        CHECK(renderSw(cl) == renderSw(paintUnclamped(s.arena, s.root)));
    }
}

TEST_CASE("offscreen: one pixel inside, or only the shadow inside, the effect is drawn with known bounds") {
    SUBCASE("1 px of the reach inside") {
        CardScene s(-55, 100);
        const CommandList cl = paintView(s.arena, s.root);
        REQUIRE(countKind(cl, CmdKind::PushEffect) == 1u);
        const EffectCmd& fx = cl.effectCmds[0];
        CHECK(fx.boundsKnown == 1);
        CHECK(fx.boundsHint.x == doctest::Approx(0.0f));
        CHECK(fx.boundsHint.w == doctest::Approx(1.0f));
        CHECK(cl.culledOffscreen == 0u);
        CHECK(renderSw(cl) == renderSw(paintUnclamped(s.arena, s.root)));
    }
    SUBCASE("the box outside, its shadow reaching in") {
        NodeArena arena;
        const NodeId root =
            makeElement(arena, kInvalidNode, 0, 0, kViewW, kViewH, Color{0.9f, 0.85f, 0.7f, 1});
        const NodeId card = makeElement(arena, root, -50, 100, 40, 30, Color{0.2f, 0.4f, 0.9f, 1});
        addShadow(arena, card, makeShadow(24, 0, 0));  // reach 24: [-74, 14]
        const CommandList cl = paintView(arena, root);
        REQUIRE(countKind(cl, CmdKind::PushEffect) == 1u);
        const EffectCmd& fx = cl.effectCmds[0];
        CHECK(fx.boundsKnown == 1);
        CHECK(fx.boundsHint.x == doctest::Approx(0.0f));
        CHECK(fx.boundsHint.w == doctest::Approx(14.0f));  // the window is the clamped reach
        CHECK(renderSw(cl) == renderSw(paintUnclamped(arena, root)));
    }
}

TEST_CASE("offscreen: an opacity layer takes in its descendants' shadow reach, or is left out") {
    SUBCASE("every box outside, one child's shadow reaching in: drawn, bounds cover the reach") {
        NodeArena arena;
        const NodeId root =
            makeElement(arena, kInvalidNode, 0, 0, kViewW, kViewH, Color{0.9f, 0.85f, 0.7f, 1});
        const NodeId group = makeElement(arena, root, -120, 100, 100, 60, Color{0, 0, 0, 0});
        arena.common(group).opacity = 0.5f;
        makeElement(arena, group, 0, 0, 40, 30, Color{1, 0, 0, 1});
        const NodeId shadowed = makeElement(arena, group, 50, 10, 40, 30, Color{0, 0.6f, 0, 1});
        addShadow(arena, shadowed,
                  makeShadow(40, 0, 0));  // the child spans [-70, -30]; reach 40 -> [-110, 10]
        const CommandList cl = paintView(arena, root);
        REQUIRE(countKind(cl, CmdKind::PushLayer) == 1u);
        const LayerCmd& layer = cl.layers[0];
        CHECK(layer.boundsKnown == 1);
        CHECK(layer.boundsHint.x == doctest::Approx(0.0f));
        CHECK(layer.boundsHint.w == doctest::Approx(10.0f));
        CHECK(cl.culledOffscreen == 0u);
        CHECK(renderSw(cl) == renderSw(paintUnclamped(arena, root)));
    }
    SUBCASE("everything outside: no layer, no content") {
        NodeArena arena;
        const NodeId root =
            makeElement(arena, kInvalidNode, 0, 0, kViewW, kViewH, Color{0.9f, 0.85f, 0.7f, 1});
        const NodeId group = makeElement(arena, root, -220, 100, 100, 60, Color{0, 0, 0, 0});
        arena.common(group).opacity = 0.5f;
        makeElement(arena, group, 0, 0, 40, 30, Color{1, 0, 0, 1});
        const NodeId shadowed = makeElement(arena, group, 50, 10, 40, 30, Color{0, 0.6f, 0, 1});
        addShadow(arena, shadowed, makeShadow(40, 0, 0));  // reach ends at -220 + 50 + 40 + 40 = -90
        const CommandList cl = paintView(arena, root);
        CHECK(countKind(cl, CmdKind::PushLayer) == 0u);
        CHECK(countKind(cl, CmdKind::PushEffect) == 0u);
        CHECK(countKind(cl, CmdKind::FillPath) == 1u);
        CHECK(cl.culledOffscreen == 1u);
        CHECK(renderSw(cl) == renderSw(paintUnclamped(arena, root)));
    }
}

TEST_CASE("offscreen: a glass panel outside the viewport is left out") {
    Harness h(
        "<xt><head><style>"
        "#bg { position: absolute; left: 0px; top: 0px; width: 400px; height: 300px; background-color: "
        "#335577; }"
        "#glass { position: absolute; left: 420px; top: 40px; width: 120px; height: 80px;"
        "         background-color: #ffffff40; effect: backdrop-blur(10px); }"
        "</style></head><body><div id=\"bg\"></div><div id=\"glass\"></div></body></xt>");
    h.frame();
    CHECK(countKind(h.list, CmdKind::PushEffect) == 0u);
    CHECK(h.list.culledOffscreen == 1u);
    h.setStyle("glass", "left: 200px;");
    h.frame(1.0f / 60.0f);
    REQUIRE(countKind(h.list, CmdKind::PushEffect) == 1u);
    CHECK(h.list.effectCmds[0].boundsKnown == 1);
}

TEST_CASE("offscreen: an element moving in damages its bounds on the frame it enters") {
    Harness h(
        "<xt><head><style>"
        "#card { position: absolute; left: -300px; top: 100px; width: 60px; height: 40px;"
        "        background-color: #3366cc; effect: shadow(4px 4px 8px #000000); }"
        "</style></head><body><div id=\"card\"></div></body></xt>");
    h.frame();
    h.frame();
    CHECK(countKind(h.list, CmdKind::PushEffect) == 0u);
    CHECK(h.damage.rects.empty());
    h.setStyle("card", "left: 20px;");
    h.frame(1.0f / 60.0f);
    REQUIRE(countKind(h.list, CmdKind::PushEffect) == 1u);
    const Rect hint = h.list.effectCmds[0].boundsHint;
    CHECK(!h.damage.full);
    Rect bounds{};
    for (const Rect& r : h.damage.rects) {
        bounds = unite(bounds, r);
    }
    CHECK(bounds.x <= hint.x);
    CHECK(bounds.y <= hint.y);
    CHECK(bounds.x + bounds.w >= hint.x + hint.w);
    CHECK(bounds.y + bounds.h >= hint.y + hint.h);
    // And back out: its old bounds are damaged once, then nothing.
    h.setStyle("card", "left: -300px;");
    h.frame(1.0f / 60.0f);
    CHECK(countKind(h.list, CmdKind::PushEffect) == 0u);
    CHECK(!h.damage.rects.empty());
    h.frame(1.0f / 60.0f);
    CHECK(h.damage.rects.empty());
}

TEST_CASE("offscreen: inside a 3D space the bounds stay unknown and nothing is left out") {
    Harness h(
        "<xt><head><style>"
        "#stage { position: absolute; left: 0px; top: 0px; width: 400px; height: 300px; perspective: 600px; }"
        "#panel { position: absolute; left: -300px; top: 100px; width: 80px; height: 60px;"
        "         transform: rotateY(20deg); background-color: #3060c0; effect: shadow(4px 4px 8px #000000); "
        "}"
        "</style></head><body><div id=\"stage\"><div id=\"panel\"></div></div></body></xt>");
    h.frame();
    REQUIRE(countKind(h.list, CmdKind::PushEffect) == 1u);
    CHECK(h.list.effectCmds[0].boundsKnown == 0);
    CHECK(h.list.culledOffscreen == 0u);
}

TEST_CASE(
    "offscreen: on the flat plane of a perspective root the bounds are known and offscreen is left out") {
    // The War Table layout: the whole HUD under one `perspective` root, its panels on the root's own
    // (affine) plane.
    Harness h(
        "<xt><head><style>"
        "#stage { position: absolute; left: 0px; top: 0px; width: 400px; height: 300px; perspective: 600px; }"
        "#out { position: absolute; left: -300px; top: 100px; width: 80px; height: 60px;"
        "       background-color: #3060c0; effect: shadow(4px 4px 8px #000000); }"
        "#in { position: absolute; left: 100px; top: 100px; width: 80px; height: 60px;"
        "      background-color: #c06030; effect: shadow(4px 4px 8px #000000); }"
        "</style></head><body><div id=\"stage\"><div id=\"out\"></div><div "
        "id=\"in\"></div></div></body></xt>");
    h.frame();
    REQUIRE(countKind(h.list, CmdKind::PushProjection) == 1u);
    REQUIRE(countKind(h.list, CmdKind::PushEffect) == 1u);
    CHECK(h.list.effectCmds[0].boundsKnown == 1);
    CHECK(h.list.culledOffscreen == 1u);
}

TEST_CASE("offscreen: an overlay inside a subtree that is outside the viewport is still painted") {
    Harness h(
        "<xt><head><style>"
        "#card { position: absolute; left: -300px; top: 100px; width: 60px; height: 40px;"
        "        background-color: #3366cc; effect: shadow(4px 4px 8px #000000); }"
        "#tip { position: absolute; left: 10px; top: 0px; width: 30px; height: 20px;"
        "       background-color: #ff0000; layer: overlay; }"
        "</style></head><body><div id=\"card\"><div id=\"tip\"></div></div></body></xt>");
    h.frame();
    // Everything lies outside the viewport, but the card holds an overlay root: it is not left out
    // (the overlay is found by walking the card's subtree), and the overlay's own fill is painted.
    CHECK(countKind(h.list, CmdKind::PushEffect) == 1u);
    CHECK(h.list.culledOffscreen == 0u);
    bool red = false;
    for (const FillCmd& f : h.list.fills) {
        red = red || (f.paint.color.r == doctest::Approx(1.0f) && f.paint.color.g == doctest::Approx(0.0f));
    }
    CHECK(red);
}

// -------------------------------------------------------------------------------------------
// T-779 (Docs/tasks/T-779.md item 1): a subtree holding another 3D space root is bounded by that
// space's projected reach.
// -------------------------------------------------------------------------------------------

namespace {

/// A backdrop filling the viewport and a 120 x 80 card at (`x`, `y`) holding a perspective stage
/// (100 x 60 at (20, 10) in the card, `perspective: 100px`, origin at its top-left corner) whose one
/// tile is pushed `z` px towards the eye -- at z = 60 it projects 2.5 x as large (from the stage's
/// corner), well past the card's box. `shadow` gives the card `shadow(4px 4px 8px)` (reach 16);
/// without it the card takes opacity 0.5 (a layer of two drawables).
struct NestedScene {
    NodeArena arena;
    NodeId root = kInvalidNode;
    NodeId card = kInvalidNode;
    NodeId stage = kInvalidNode;
    NestedScene(float x, float y, float z, bool shadow = true) {
        root = makeElement(arena, kInvalidNode, 0, 0, kViewW, kViewH, Color{0.9f, 0.85f, 0.7f, 1});
        card = makeElement(arena, root, x, y, 120, 80, Color{0.2f, 0.4f, 0.9f, 1});
        if (shadow) {
            addShadow(arena, card, makeShadow(4, 4, 8));
        } else {
            arena.common(card).opacity = 0.5f;
        }
        stage = makeElement(arena, card, 20, 10, 100, 60, Color{0, 0, 0, 0});
        arena.element(stage).perspectivePx = 100;
        const NodeId tile = makeElement(arena, stage, 0, 0, 100, 60, Color{0.8f, 0.3f, 0.2f, 1});
        arena.common(tile).is3D = 1;
        arena.common(tile).translateZ = z;
    }
};

Rect outsetBy(Rect r, float e) { return Rect{r.x - e, r.y - e, r.w + 2 * e, r.h + 2 * e}; }

Rect clampToView(Rect r) {
    const float x0 = std::max(r.x, 0.0f), y0 = std::max(r.y, 0.0f);
    const float x1 = std::min(r.x + r.w, kViewW), y1 = std::min(r.y + r.h, kViewH);
    return Rect{x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
}

}  // namespace

TEST_CASE("offscreen (T-779): a shadowed card holding a perspective stage, wholly outside, is left out") {
    NestedScene s(-900, 100, 60);  // the tile projects to x in [-880, -630]
    const CommandList cl = paintView(s.arena, s.root);
    CHECK(countKind(cl, CmdKind::PushEffect) == 0u);
    CHECK(countKind(cl, CmdKind::PushProjection) == 0u);
    CHECK(countKind(cl, CmdKind::FillPath) == 1u);  // the backdrop only
    CHECK(cl.culledOffscreen == 1u);
    CHECK(renderSw(cl) == renderSw(paintUnclamped(s.arena, s.root)));
}

TEST_CASE("offscreen (T-779): an opacity layer holding a perspective stage, wholly outside, is left out") {
    NestedScene s(-900, 100, 60, /*shadow=*/false);
    const CommandList cl = paintView(s.arena, s.root);
    CHECK(countKind(cl, CmdKind::PushLayer) == 0u);
    CHECK(countKind(cl, CmdKind::PushProjection) == 0u);
    CHECK(cl.culledOffscreen == 1u);
    CHECK(renderSw(cl) == renderSw(paintUnclamped(s.arena, s.root)));
}

TEST_CASE("offscreen (T-779): half outside, the card's own known window is kept (T-754's)") {
    // Only where the old hint was the whole-surface fallback does the union become the window; a
    // node whose own hint meets the viewport keeps it (no existing window changes).
    NestedScene s(-60, 100, 60);  // card [-60, 60] x [100, 180]; the tile projects to [-40, 210] x [110, 260]
    const CommandList cl = paintView(s.arena, s.root);
    REQUIRE(countKind(cl, CmdKind::PushEffect) == 1u);
    REQUIRE(countKind(cl, CmdKind::PushProjection) == 1u);
    const EffectCmd& fx = cl.effectCmds[0];
    const Rect projected = cl.projections[0].boundsHint;  // clamped; the clamp does not matter here
    const Rect content{-60, 100, 120, 80};
    REQUIRE(projected.x + projected.w > content.x + content.w);  // the projection leaves the card's box
    REQUIRE(projected.y + projected.h > content.y + content.h);
    const Rect expected = clampToView(outsetBy(content, 16));
    CHECK(fx.boundsKnown == 1);
    CHECK(fx.boundsHint.x == doctest::Approx(expected.x));
    CHECK(fx.boundsHint.y == doctest::Approx(expected.y));
    CHECK(fx.boundsHint.w == doctest::Approx(expected.w));
    CHECK(fx.boundsHint.h == doctest::Approx(expected.h));
    CHECK(fx.contentBounds.x == doctest::Approx(content.x));  // the content AABB itself is unchanged
    CHECK(fx.contentBounds.w == doctest::Approx(content.w));
    CHECK(cl.culledOffscreen == 0u);
    CHECK(renderSw(cl) == renderSw(paintUnclamped(s.arena, s.root)));
}

TEST_CASE("offscreen (T-779): the card outside, the projected stage reaching in -- drawn, and correctly") {
    // Card [-160, -40] (reach to -24); the stage at x = -140 projects its tile to [-140, 110].
    NestedScene s(-160, 100, 60);
    const CommandList cl = paintView(s.arena, s.root);
    REQUIRE(countKind(cl, CmdKind::PushEffect) == 1u);
    REQUIRE(countKind(cl, CmdKind::PushProjection) == 1u);
    const EffectCmd& fx = cl.effectCmds[0];
    const Rect projected = cl.projections[0].boundsHint;
    REQUIRE(projected.w > 0.0f);
    CHECK(fx.boundsKnown == 1);
    CHECK(fx.boundsHint.x == doctest::Approx(0.0f));
    CHECK(fx.boundsHint.x + fx.boundsHint.w >= projected.x + projected.w);
    CHECK(fx.boundsHint.y <= projected.y);
    CHECK(fx.boundsHint.y + fx.boundsHint.h >= projected.y + projected.h);
    CHECK(cl.culledOffscreen == 0u);
    // The oracle is the same list with the whole viewport as the window (the GPU backends' old
    // whole-surface fallback): the union loses nothing. (Painting with an unclamped viewport is no
    // oracle here: there the card's own hint meets the viewport and is kept, cutting the tile.)
    CommandList whole = cl;
    whole.effectCmds[0].boundsHint = Rect{0, 0, kViewW, kViewH};
    const std::vector<uint8_t> pixels = renderSw(cl);
    CHECK(pixels == renderSw(whole));
    NestedScene gone(-900, 100, 60);  // the tile is drawn: the frame is not the bare backdrop
    CHECK(pixels != renderSw(paintView(gone.arena, gone.root)));
}

TEST_CASE("offscreen (T-779): a stage whose projection is not known keeps the subtree unknown") {
    // The tile is pushed past the eye (z 150 > perspective 100): its corners do not project.
    NestedScene s(-900, 100, 150);
    const CommandList cl = paintView(s.arena, s.root);
    REQUIRE(countKind(cl, CmdKind::PushEffect) == 1u);
    CHECK(cl.effectCmds[0].boundsKnown == 0);
    CHECK(cl.culledOffscreen == 0u);
}

// -------------------------------------------------------------------------------------------
// T-779 (Docs/tasks/T-779.md "測試" item 3): the bounds of the T-754 cases -- no space below the node
// -- are those T-754 computed. The texts below were printed by the T-754 painter (main at 8d1e1261).
// T-795 (Docs/tasks/T-795.md "測試" item 4): in kT754InSpace the opacity group whose box is outside
// while a child's shadow reaches in now takes the union as its known window (the flat list's
// hint); every other line is T-754's.
// -------------------------------------------------------------------------------------------

namespace {

const char* const kT754Flat =
    R"(FillPath xf=[1.000 0.000 0.000 1.000 0.000 0.000] verbs=6 points=10 rule=NonZero paint=Solid(0.900,0.850,0.700,1.000)
PushEffect count=1 xf=[1.000 0.000 0.000 1.000 -55.000 20.000] bounds=(0.000,4.000,1.000,62.000) known=1 content=(-55.000,20.000,40.000,30.000)
  effect kind=DropShadow enabled=1 params=[4.000 4.000 8.000 0.000 1.000 0.000 0.000 0.000] color0=(0.000,0.000,0.000,0.750) color1=(0.000,0.000,0.000,1.000) stops=0
FillPath xf=[1.000 0.000 0.000 1.000 -55.000 20.000] verbs=6 points=10 rule=NonZero paint=Solid(0.200,0.400,0.900,1.000)
PopEffect
PushEffect count=1 xf=[1.000 0.000 0.000 1.000 150.000 20.000] bounds=(134.000,4.000,72.000,62.000) known=1 content=(150.000,20.000,40.000,30.000)
  effect kind=DropShadow enabled=1 params=[4.000 4.000 8.000 0.000 1.000 0.000 0.000 0.000] color0=(0.000,0.000,0.000,0.750) color1=(0.000,0.000,0.000,1.000) stops=0
FillPath xf=[1.000 0.000 0.000 1.000 150.000 20.000] verbs=6 points=10 rule=NonZero paint=Solid(0.200,0.400,0.900,1.000)
PopEffect
PushLayer opacity=0.500 blend=Normal known=1
FillPath xf=[1.000 0.000 0.000 1.000 -120.000 100.000] verbs=6 points=10 rule=NonZero paint=Solid(1.000,0.000,0.000,1.000)
PushEffect count=1 xf=[1.000 0.000 0.000 1.000 -70.000 110.000] bounds=(0.000,70.000,10.000,110.000) known=1 content=(-70.000,110.000,40.000,30.000)
  effect kind=DropShadow enabled=1 params=[40.000 0.000 0.000 0.000 1.000 0.000 0.000 0.000] color0=(0.000,0.000,0.000,0.750) color1=(0.000,0.000,0.000,1.000) stops=0
FillPath xf=[1.000 0.000 0.000 1.000 -70.000 110.000] verbs=6 points=10 rule=NonZero paint=Solid(0.000,0.600,0.000,1.000)
PopEffect
PopLayer
layer hint=(0.000,70.000,10.000,110.000) known=1
culled=2
)";
const char* const kT754InSpace =
    R"(FillPath xf=[1.000 0.000 0.000 1.000 0.000 0.000] verbs=6 points=10 rule=NonZero paint=Solid(0.900,0.850,0.700,1.000)
PushProjection viewProj=[1.000 0.000 -0.333 0.000 0.000 1.000 -0.250 0.000 0.000 0.000 1.000 0.000 0.000 0.000 -0.002 1.000] rootXf=[1.000 0.000 0.000 1.000 0.000 0.000] box=(0.000,0.000,400.000,300.000) depth=0
PushTransform3D [1.000 0.000 0.000 0.000 0.000 1.000 0.000 0.000 0.000 0.000 1.000 0.000 0.000 0.000 0.000 1.000]
PopTransform3D
PushTransform3D [1.000 0.000 0.000 0.000 0.000 1.000 0.000 0.000 0.000 0.000 1.000 0.000 0.000 0.000 0.000 1.000]
PushEffect count=1 xf=[1.000 0.000 0.000 1.000 -55.000 20.000] bounds=(0.000,4.000,1.000,62.000) known=1 content=(-55.000,20.000,40.000,30.000)
  effect kind=DropShadow enabled=1 params=[4.000 4.000 8.000 0.000 1.000 0.000 0.000 0.000] color0=(0.000,0.000,0.000,0.750) color1=(0.000,0.000,0.000,1.000) stops=0
FillPath xf=[1.000 0.000 0.000 1.000 -55.000 20.000] verbs=6 points=10 rule=NonZero paint=Solid(0.200,0.400,0.900,1.000)
PopEffect
PopTransform3D
PushTransform3D [1.000 0.000 0.000 0.000 0.000 1.000 0.000 0.000 0.000 0.000 1.000 0.000 0.000 0.000 0.000 1.000]
PushEffect count=1 xf=[1.000 0.000 0.000 1.000 150.000 20.000] bounds=(134.000,4.000,72.000,62.000) known=1 content=(150.000,20.000,40.000,30.000)
  effect kind=DropShadow enabled=1 params=[4.000 4.000 8.000 0.000 1.000 0.000 0.000 0.000] color0=(0.000,0.000,0.000,0.750) color1=(0.000,0.000,0.000,1.000) stops=0
FillPath xf=[1.000 0.000 0.000 1.000 150.000 20.000] verbs=6 points=10 rule=NonZero paint=Solid(0.200,0.400,0.900,1.000)
PopEffect
PopTransform3D
PushTransform3D [1.000 0.000 0.000 0.000 0.000 1.000 0.000 0.000 0.000 0.000 1.000 0.000 0.000 0.000 0.000 1.000]
PushLayer opacity=0.500 blend=Normal known=1
FillPath xf=[1.000 0.000 0.000 1.000 -120.000 100.000] verbs=6 points=10 rule=NonZero paint=Solid(1.000,0.000,0.000,1.000)
PushEffect count=1 xf=[1.000 0.000 0.000 1.000 -70.000 110.000] bounds=(0.000,70.000,10.000,110.000) known=1 content=(-70.000,110.000,40.000,30.000)
  effect kind=DropShadow enabled=1 params=[40.000 0.000 0.000 0.000 1.000 0.000 0.000 0.000] color0=(0.000,0.000,0.000,0.750) color1=(0.000,0.000,0.000,1.000) stops=0
FillPath xf=[1.000 0.000 0.000 1.000 -70.000 110.000] verbs=6 points=10 rule=NonZero paint=Solid(0.000,0.600,0.000,1.000)
PopEffect
PopLayer
PopTransform3D
PushTransform3D [1.000 0.000 0.000 0.000 0.000 1.000 0.000 0.000 0.000 0.000 1.000 0.000 0.000 0.000 0.000 1.000]
PopTransform3D
PushTransform3D [0.866 0.000 0.500 200.000 0.000 1.000 0.000 200.000 -0.500 0.000 0.866 0.000 0.000 0.000 0.000 1.000]
PushEffect count=1 xf=[0.866 -0.042 0.000 1.000 200.000 200.000] bounds=(184.999,181.874,94.953,93.126) known=0 content=(200.000,196.875,64.952,63.125)
  effect kind=DropShadow enabled=1 params=[4.000 4.000 8.000 0.000 1.000 0.000 0.000 0.000] color0=(0.000,0.000,0.000,0.750) color1=(0.000,0.000,0.000,1.000) stops=0
FillPath xf=[1.000 0.000 0.000 1.000 0.000 0.000] verbs=6 points=10 rule=NonZero paint=Solid(0.300,0.300,0.800,1.000)
PopEffect
PopTransform3D
PopProjection
layer hint=(0.000,70.000,10.000,110.000) known=1
culled=2
)";

/// The command dump, each layer's hint and flag (the dump leaves them out) and the culled count.
std::string boundsText(const CommandList& cl) {
    std::string out;
    dumpCommandList(cl, out);
    char line[160];
    for (const LayerCmd& l : cl.layers) {
        std::snprintf(line, sizeof line, "layer hint=(%.3f,%.3f,%.3f,%.3f) known=%u\n", l.boundsHint.x,
                      l.boundsHint.y, l.boundsHint.w, l.boundsHint.h, static_cast<unsigned>(l.boundsKnown));
        out += line;
    }
    std::snprintf(line, sizeof line, "culled=%u\n", static_cast<unsigned>(cl.culledOffscreen));
    out += line;
    return out;
}

/// The T-754 cases in one scene: shadowed cards (out, 1 px in, in), opacity groups (a child's
/// shadow reaching in, wholly out), and under a `perspective` root on its affine plane the same
/// kinds again, plus a group whose box is out while a child's shadow reaches in (T-795: on the
/// affine plane its window is the union, as outside 3D spaces), and a turned (non-affine) panel.
std::string t754Scene(bool inSpace) {
    NodeArena arena;
    const NodeId root = makeElement(arena, kInvalidNode, 0, 0, kViewW, kViewH, Color{0.9f, 0.85f, 0.7f, 1});
    NodeId host = root;
    if (inSpace) {
        host = makeElement(arena, root, 0, 0, kViewW, kViewH, Color{0, 0, 0, 0});
        arena.element(host).perspectivePx = 600;
        arena.element(host).perspectiveOriginPx = Vec2{kViewW / 2, kViewH / 2};
    }
    const float xs[] = {-57, -55, 150};
    for (float x : xs) {
        const NodeId card = makeElement(arena, host, x, 20, 40, 30, Color{0.2f, 0.4f, 0.9f, 1});
        addShadow(arena, card, makeShadow(4, 4, 8));
    }
    const float groupXs[] = {-120, -220};
    for (float x : groupXs) {
        const NodeId group = makeElement(arena, host, x, 100, 100, 60, Color{0, 0, 0, 0});
        arena.common(group).opacity = 0.5f;
        makeElement(arena, group, 0, 0, 40, 30, Color{1, 0, 0, 1});
        const NodeId shadowed = makeElement(arena, group, 50, 10, 40, 30, Color{0, 0.6f, 0, 1});
        addShadow(arena, shadowed, makeShadow(40, 0, 0));
    }
    if (inSpace) {
        const NodeId panel = makeElement(arena, host, 200, 200, 80, 60, Color{0.3f, 0.3f, 0.8f, 1});
        arena.common(panel).is3D = 1;
        arena.common(panel).rotationY = 30;
        addShadow(arena, panel, makeShadow(4, 4, 8));
    }
    return boundsText(paintView(arena, root));
}

}  // namespace

TEST_CASE("offscreen (T-779): the T-754 cases keep the T-754 bounds") {
    CHECK(t754Scene(false) == std::string(kT754Flat));
    CHECK(t754Scene(true) == std::string(kT754InSpace));
}

// -------------------------------------------------------------------------------------------
// T-795 (Docs/tasks/T-795.md, 10 §10.20 rule 3 and its T-779 supplement): on an affine plane inside
// a 3D space a layer takes in its descendants' effect reach like outside one; a space root's own
// opacity layer is bounded by the root's projection.
// -------------------------------------------------------------------------------------------

namespace {

/// A backdrop filling the viewport and a `perspective: 600px` stage over it (origin at its centre)
/// holding one opacity-0.5 group (100 x 60 at (`x`, 100)) of a red box and a green box whose
/// `shadow(40px 0 0)` reaches 40 px: at x = -120 the boxes span [-120, -30] and the shadow reaches
/// to 10. `rotateX` turns the group about its own anchor (a 3D layer of its own, a projected plane).
struct SpaceGroupScene {
    NodeArena arena;
    NodeId root = kInvalidNode;
    NodeId stage = kInvalidNode;
    NodeId group = kInvalidNode;
    explicit SpaceGroupScene(float x, float rotateX = 0.0f) {
        root = makeElement(arena, kInvalidNode, 0, 0, kViewW, kViewH, Color{0.9f, 0.85f, 0.7f, 1});
        stage = makeElement(arena, root, 0, 0, kViewW, kViewH, Color{0, 0, 0, 0});
        arena.element(stage).perspectivePx = 600;
        arena.element(stage).perspectiveOriginPx = Vec2{kViewW / 2, kViewH / 2};
        group = makeElement(arena, stage, x, 100, 100, 60, Color{0, 0, 0, 0});
        arena.common(group).opacity = 0.5f;
        if (rotateX != 0.0f) {
            arena.common(group).is3D = 1;
            arena.common(group).rotationX = rotateX;
        }
        makeElement(arena, group, 0, 0, 40, 30, Color{1, 0, 0, 1});
        const NodeId shadowed = makeElement(arena, group, 50, 10, 40, 30, Color{0, 0.6f, 0, 1});
        addShadow(arena, shadowed, makeShadow(40, 0, 0));
    }
};

/// `cl` with every layer's window replaced by the whole viewport (unknown): the GPU backends' old
/// whole-surface fallback, the oracle for a smaller known window.
CommandList wholeViewportLayers(const CommandList& cl) {
    CommandList whole = cl;
    for (LayerCmd& l : whole.layers) {
        l.boundsHint = Rect{0, 0, kViewW, kViewH};
        l.boundsKnown = 0;
    }
    return whole;
}

/// A backdrop and a 200 x 150 `perspective: 300px` stage at (`x`, 60) with opacity 0.5 (its own
/// layer around the projection) holding a 120 x 80 tile at (40, 35) pushed `z` px towards the eye.
struct OpaqueStageScene {
    NodeArena arena;
    NodeId root = kInvalidNode;
    NodeId stage = kInvalidNode;
    OpaqueStageScene(float x, float z) {
        root = makeElement(arena, kInvalidNode, 0, 0, kViewW, kViewH, Color{0.9f, 0.85f, 0.7f, 1});
        stage = makeElement(arena, root, x, 60, 200, 150, Color{0.3f, 0.3f, 0.35f, 1});
        arena.element(stage).perspectivePx = 300;
        arena.element(stage).perspectiveOriginPx = Vec2{100, 75};
        arena.common(stage).opacity = 0.5f;
        const NodeId tile = makeElement(arena, stage, 40, 35, 120, 80, Color{0.8f, 0.3f, 0.2f, 1});
        arena.common(tile).is3D = 1;
        arena.common(tile).translateZ = z;
    }
};

}  // namespace

TEST_CASE(
    "offscreen (T-795): on the affine plane of a perspective root a group's window takes in the shadow") {
    SpaceGroupScene s(-120);
    const CommandList cl = paintView(s.arena, s.root);
    REQUIRE(countKind(cl, CmdKind::PushProjection) == 1u);
    REQUIRE(countKind(cl, CmdKind::PushLayer) == 1u);
    const LayerCmd& layer = cl.layers[0];
    // The union of the boxes [-120, -30] x [100, 140] and the shadow's reach [-110, 10] x [70, 180].
    CHECK(layer.boundsKnown == 1);
    CHECK(layer.boundsHint.x == doctest::Approx(0.0f));
    CHECK(layer.boundsHint.y == doctest::Approx(70.0f));
    CHECK(layer.boundsHint.w == doctest::Approx(10.0f));
    CHECK(layer.boundsHint.h == doctest::Approx(110.0f));
    CHECK(cl.culledOffscreen == 0u);
    // The oracle: the same list with the whole viewport as the window. (The shadow of a box outside
    // the viewport is not drawn by xt::sw at all -- an effect window clamped to the viewport holds
    // none of its source, flat or not -- so the frame is the backdrop either way.)
    CHECK(renderSw(cl) == renderSw(wholeViewportLayers(cl)));
}

TEST_CASE("offscreen (T-795): on the affine plane of a perspective root a group wholly outside is left out") {
    SpaceGroupScene s(-220);  // the shadow reaches to -90
    const CommandList cl = paintView(s.arena, s.root);
    REQUIRE(countKind(cl, CmdKind::PushProjection) == 1u);
    CHECK(countKind(cl, CmdKind::PushLayer) == 0u);
    CHECK(countKind(cl, CmdKind::PushEffect) == 0u);
    CHECK(cl.culledOffscreen == 1u);
    CHECK(renderSw(cl) == renderSw(paintUnclamped(s.arena, s.root)));
}

TEST_CASE("offscreen (T-795): on a projected plane (rotateX under perspective) the group stays unknown") {
    SpaceGroupScene s(-120, 40.0f);
    const CommandList cl = paintView(s.arena, s.root);
    REQUIRE(countKind(cl, CmdKind::PushProjection) == 1u);
    REQUIRE(countKind(cl, CmdKind::PushLayer) == 1u);
    CHECK(cl.layers[0].boundsKnown == 0);
    CHECK(cl.culledOffscreen == 0u);
}

TEST_CASE("offscreen (T-795): a space root's opacity layer is bounded by its projection") {
    SUBCASE("inside the viewport: the projection's extent") {
        OpaqueStageScene s(60, 60);
        const CommandList cl = paintView(s.arena, s.root);
        REQUIRE(countKind(cl, CmdKind::PushLayer) == 1u);
        REQUIRE(countKind(cl, CmdKind::PushProjection) == 1u);
        const LayerCmd& layer = cl.layers[0];
        const Rect projected = cl.projections[0].boundsHint;
        CHECK(layer.boundsKnown == 1);
        CHECK(layer.boundsHint.x == doctest::Approx(projected.x));
        CHECK(layer.boundsHint.y == doctest::Approx(projected.y));
        CHECK(layer.boundsHint.w == doctest::Approx(projected.w));
        CHECK(layer.boundsHint.h == doctest::Approx(projected.h));
        CHECK(layer.boundsHint.w * layer.boundsHint.h < kViewW * kViewH);  // smaller than the viewport
        CHECK(renderSw(cl) == renderSw(wholeViewportLayers(cl)));
    }
    SUBCASE("half outside: clamped to the viewport") {
        OpaqueStageScene s(-120, 60);
        const CommandList cl = paintView(s.arena, s.root);
        REQUIRE(countKind(cl, CmdKind::PushLayer) == 1u);
        const LayerCmd& layer = cl.layers[0];
        CHECK(layer.boundsKnown == 1);
        CHECK(layer.boundsHint.x == doctest::Approx(0.0f));
        CHECK(layer.boundsHint.w < kViewW);
        CHECK(renderSw(cl) == renderSw(wholeViewportLayers(cl)));
    }
    SUBCASE("a corner behind the eye: the whole viewport, unknown") {
        OpaqueStageScene s(60, 400);  // the tile at z 400, past the eye (perspective 300)
        const CommandList cl = paintView(s.arena, s.root);
        REQUIRE(countKind(cl, CmdKind::PushLayer) == 1u);
        const LayerCmd& layer = cl.layers[0];
        CHECK(layer.boundsKnown == 0);
        CHECK(layer.boundsHint.x == doctest::Approx(0.0f));
        CHECK(layer.boundsHint.y == doctest::Approx(0.0f));
        CHECK(layer.boundsHint.w == doctest::Approx(kViewW));
        CHECK(layer.boundsHint.h == doctest::Approx(kViewH));
    }
}
