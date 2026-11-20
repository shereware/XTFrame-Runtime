#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "flatten_3d_fixtures.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/document.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/painter.h"
#include "xt/property.h"
#include "xt/sw/sw_renderer.h"
#include "xt/text_shaper.h"
#include "xt/types.h"
#include "xt/value.h"

/// T-762 (Docs/design/19-3d-space.md §19.y.2): a flat node with a grouping property (opacity < 1,
/// an effect, a mask, a blend mode, a clip) inside a 3D space is one sort item together with its 3D
/// descendants; they keep their transforms and perspective but are drawn inside the group in
/// document order, and the group composites once. Fixture: Tests/layout/flat_3d_flatten.xt
/// (Tests/common/flatten_3d_fixtures.h).

using namespace xt;
namespace fx = xt::test::flatten;

namespace {

/// The index in `cmds.commands` of the first FillPath of solid colour (r, g, b) (0..255), or -1.
int fillCommand(const CommandList& cmds, int r, int g, int b) {
    for (size_t i = 0; i < cmds.commands.size(); ++i) {
        if (cmds.commands[i].kind != CmdKind::FillPath) {
            continue;
        }
        const Color& c = cmds.fills[cmds.commands[i].index].paint.color;
        if (std::lround(c.r * 255.0f) == r && std::lround(c.g * 255.0f) == g &&
            std::lround(c.b * 255.0f) == b) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int firstCommand(const CommandList& cmds, CmdKind kind, size_t from = 0) {
    for (size_t i = from; i < cmds.commands.size(); ++i) {
        if (cmds.commands[i].kind == kind) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::vector<uint8_t> renderSw(const CommandList& cl, int w, int h) {
    std::vector<uint8_t> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    sw::SwRenderer renderer;
    renderer.setTarget(sw::SwTarget{buf.data(), w, h, w * 4});
    renderer.clear(Color{1, 1, 1, 1});
    renderer.render(cl, nullptr, nullptr);
    return buf;
}

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

CommandList paintArena(NodeArena& arena, NodeId root) {
    updateWorldTransforms(arena, root, /*force=*/true);
    CommandList cl;
    cl.viewport = Rect{0, 0, 200, 160};
    cl.devicePixelRatio = 1;
    Painter painter;
    painter.setBoxShadowCommands(false);
    painter.paint(arena, root, cl);
    return cl;
}

}  // namespace

TEST_CASE("Painter 3D flatten (T-762): an opacity group is one item holding its 3D children") {
    std::string error;
    const CommandList cmds = fx::paintFixture(error);
    REQUIRE_MESSAGE(error.empty(), error);

    // #s1 is painted first: its group's Layer, red and blue inside it in document order, then green.
    const int push = firstCommand(cmds, CmdKind::PushLayer);
    const int pop = firstCommand(cmds, CmdKind::PopLayer);
    const int red = fillCommand(cmds, 255, 0, 0);
    const int blue = fillCommand(cmds, 0, 0, 255);
    const int green = fillCommand(cmds, 0, 255, 0);
    REQUIRE(push >= 0);
    REQUIRE(red >= 0);
    REQUIRE(blue >= 0);
    REQUIRE(green >= 0);
    CHECK(cmds.layers[cmds.commands[static_cast<size_t>(push)].index].opacity == doctest::Approx(0.5f));
    CHECK(push < red);
    CHECK(red < blue);
    CHECK(blue < pop);
    CHECK(pop < green);
    // Composited once: the children paint at full alpha inside the Layer.
    CHECK(cmds.fills[cmds.commands[static_cast<size_t>(red)].index].paint.color.a == doctest::Approx(1.0f));
    CHECK(cmds.fills[cmds.commands[static_cast<size_t>(blue)].index].paint.color.a == doctest::Approx(1.0f));
    // Each child keeps its own plane: the PushTransform3D right before it carries its translateZ.
    for (const int fill : {red, blue}) {
        int plane = -1;
        for (int i = fill; i >= 0 && plane < 0; --i) {
            if (cmds.commands[static_cast<size_t>(i)].kind == CmdKind::PushTransform3D) {
                plane = i;
            }
        }
        REQUIRE(plane > push);
        const Mat4& m = cmds.transforms3[cmds.commands[static_cast<size_t>(plane)].index].world3;
        CHECK(std::fabs(mat4Transform(m, Vec3{0, 0, 0}).z) == doctest::Approx(40.0f));
    }
}

TEST_CASE("Painter 3D flatten (T-762): SW pixels -- the group composites once, at its own depth") {
    std::string error;
    const CommandList cmds = fx::paintFixture(error);
    REQUIRE_MESSAGE(error.empty(), error);
    const std::vector<uint8_t> px = renderSw(cmds, fx::kW, fx::kH);

    SUBCASE("opacity 0.5: one composite, blue (later) over red, green above both") {
        CHECK(fx::near(fx::at(px, 0, fx::kOverlap), 128, 128, 255));  // not 128,64,191 (two halves)
        CHECK(fx::near(fx::at(px, 0, fx::kRedOnly), 255, 128, 128));
        CHECK(fx::near(fx::at(px, 0, fx::kGreenOverRed), 0, 255, 0));
        CHECK(fx::near(fx::at(px, 0, fx::kGreenOverBlue), 0, 255, 0));
    }
    SUBCASE("shadow: cast by the 3D children, under the group") {
        CHECK(fx::near(fx::at(px, 1, fx::kShadowOnly), 0, 0, 0));
        CHECK(fx::near(fx::at(px, 1, fx::kOverlap), 0, 0, 255));
        CHECK(fx::near(fx::at(px, 1, fx::kGreenOverRed), 0, 255, 0));
    }
    SUBCASE("mask (alpha 0x88): applies to the 3D children as well") {
        CHECK(fx::near(fx::at(px, 2, fx::kBlueOnly), 119, 119, 255));
        CHECK(fx::near(fx::at(px, 2, fx::kRedOnly), 255, 119, 119));
        CHECK(fx::near(fx::at(px, 2, fx::kGreenOverRed), 0, 255, 0));
    }
    SUBCASE("multiply: the 3D children blend with the yellow stage") {
        CHECK(fx::near(fx::at(px, 3, fx::kBlueOnly), 0, 0, 0));
        CHECK(fx::near(fx::at(px, 3, fx::kRedOnly), 255, 0, 0));
        CHECK(fx::near(fx::at(px, 3, fx::kGreenOverRed), 0, 255, 0));
    }
}

TEST_CASE("hitTest 3D flatten (T-762): the same order as the painting") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::string error;
    std::unique_ptr<Document> doc = fx::loadFixture(&shaper, &assets, error);
    REQUIRE_MESSAGE(doc != nullptr, error);
    const NodeId red = doc->byId("k11").node;
    const NodeId blue = doc->byId("k12").node;
    const NodeId green = doc->byId("m1").node;
    REQUIRE(red != kInvalidNode);
    REQUIRE(blue != kInvalidNode);
    REQUIRE(green != kInvalidNode);
    // Inside the group the later child is on top; the group as a whole lies below the green layer.
    CHECK(doc->hitTest(fx::kOverlap) == blue);
    CHECK(doc->hitTest(fx::kRedOnly) == red);
    CHECK(doc->hitTest(fx::kGreenOverRed) == green);
    CHECK(doc->hitTest(fx::kGreenOverBlue) == green);
    CHECK(doc->hitTest(fx::kBlueOnly) == blue);
}

TEST_CASE("Painter 3D flatten (T-762): a group with Mask children masks its 3D child") {
    NodeArena arena;
    const NodeId root = makeElement(arena, kInvalidNode, 0, 0, 200, 160, Color{1, 1, 1, 1});
    arena.element(root).perspectivePx = 300;
    arena.element(root).perspectiveOriginPx = Vec2{100, 80};
    const NodeId group = makeElement(arena, root, 0, 0, 200, 160, Color{0, 0, 0, 0});
    const NodeId mask = arena.create(NodeKind::Mask);
    arena.appendChild(group, mask);
    makeElement(arena, mask, 0, 0, 100, 160, Color{1, 1, 1, 1});
    const NodeId child = makeElement(arena, group, 40, 40, 100, 60, Color{0, 0, 1, 1});
    arena.common(child).is3D = 1;
    arena.common(child).translateZ = 10;
    const CommandList cl = paintArena(arena, root);

    const int endMask = firstCommand(cl, CmdKind::EndMaskContent);
    const int popMask = firstCommand(cl, CmdKind::PopMask);
    const int blue = fillCommand(cl, 0, 0, 255);
    REQUIRE(endMask >= 0);
    REQUIRE(blue >= 0);
    CHECK(endMask < blue);
    CHECK(blue < popMask);
}

TEST_CASE("Painter 3D flatten (T-762): backface-visibility inside a group is judged on the child's plane") {
    NodeArena arena;
    const NodeId root = makeElement(arena, kInvalidNode, 0, 0, 200, 160, Color{1, 1, 1, 1});
    arena.element(root).perspectivePx = 300;
    arena.element(root).perspectiveOriginPx = Vec2{100, 80};
    const NodeId group = makeElement(arena, root, 0, 0, 200, 160, Color{0, 0, 0, 0});
    arena.common(group).opacity = 0.5f;
    const NodeId back = makeElement(arena, group, 20, 20, 60, 60, Color{1, 0, 0, 1});
    arena.common(back).is3D = 1;
    arena.common(back).rotationY = 180;
    arena.common(back).anchor = Vec2{30, 30};
    arena.common(back).backfaceHidden = 1;
    const NodeId front = makeElement(arena, group, 100, 20, 60, 60, Color{0, 0, 1, 1});
    arena.common(front).is3D = 1;
    arena.common(front).translateZ = 10;
    const CommandList cl = paintArena(arena, root);

    CHECK(fillCommand(cl, 255, 0, 0) < 0);  // turned away: not drawn
    CHECK(fillCommand(cl, 0, 0, 255) >= 0);
    const int push = firstCommand(cl, CmdKind::PushLayer);
    REQUIRE(push >= 0);
    CHECK(push < fillCommand(cl, 0, 0, 255));
}

TEST_CASE("compile lint (T-762): a grouping element on a 3D space's plane gets a note") {
    compile::CompileOptions options;
    options.strict = true;  // a note never fails a strict compile
    const compile::CompileResult result = compile::compileDocument(fx::fixturePath(), options);
    REQUIRE(result.ok());
    int notes = 0;
    for (const compile::Diagnostic& d : result.diagnostics) {
        if (d.code == "compile.note" && d.message.find("depth sort") != std::string::npos) {
            ++notes;
            CHECK(d.line > 0);
        }
    }
    CHECK(notes == 4);  // #g1..#g4 (opacity, effect, effect, mix-blend-mode)

    // No group in the way: no note.
    const compile::CompileResult plain =
        compile::compileDocument(std::string(XT_TEST_LAYOUT_DIR) + "/flat_3d_interleave.xt", options);
    REQUIRE(plain.ok());
    for (const compile::Diagnostic& d : plain.diagnostics) {
        CHECK(d.message.find("depth sort") == std::string::npos);
    }
}
