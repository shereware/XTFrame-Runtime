#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

#include "doctest.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// T-727 (Docs/design/19-3d-space.md §19.x ruling 1): inside a sorted 3D segment the flat content
/// takes part in the depth sort -- an item at depth 0 per run of flat content between 3D layers,
/// ties in document order -- instead of always being drawn below every layer. Fixtures compiled
/// from Tests/layout/flat_3d_*.xt, a 400x300 viewport, one update(0).

using namespace xt;
using namespace xt::compile;

namespace {

std::string layoutPath(const std::string& name) { return std::string(XT_TEST_LAYOUT_DIR) + "/" + name; }

std::unique_ptr<Document> loadDoc(const std::string& name, TextShaper* shaper, AssetResolver* assets,
                                  float time = 0.0f) {
    CompileOptions options;
    CompileResult result = compileDocument(layoutPath(name), options);
    for (const Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());

    std::unique_ptr<Document> doc;
    Status status = Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, shaper, assets, doc);
    REQUIRE(status == Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 300});
    doc->update(time);
    return doc;
}

/// The index in `cmds.fills` of the solid fill of colour (r, g, b) (0..255), or -1.
int fillIndex(const CommandList& cmds, int r, int g, int b) {
    for (size_t i = 0; i < cmds.fills.size(); ++i) {
        const Color& c = cmds.fills[i].paint.color;
        if (std::lround(c.r * 255.0f) == r && std::lround(c.g * 255.0f) == g &&
            std::lround(c.b * 255.0f) == b) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

}  // namespace

TEST_CASE("Painter 3D flat (T-727): flat content sorts with the layers of its segment") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("flat_3d_interleave.xt", &shaper, &assets);
    CommandList cmds;
    doc->paint(cmds);

    SUBCASE("depth tie (b2 at depth 0): document order A, b1, b2, b3") {
        const int a = fillIndex(cmds, 255, 0, 0);
        const int b1 = fillIndex(cmds, 0, 255, 0);
        const int b2 = fillIndex(cmds, 0, 0, 255);
        const int b3 = fillIndex(cmds, 255, 255, 0);
        REQUIRE(a >= 0);
        REQUIRE(b1 >= 0);
        REQUIRE(b2 >= 0);
        REQUIRE(b3 >= 0);
        CHECK(a < b1);
        CHECK(b1 < b2);
        CHECK(b2 < b3);
    }
    SUBCASE("b2 slightly in front (depth 0.01): A, b1, b3, b2") {
        const int a = fillIndex(cmds, 128, 0, 0);
        const int b1 = fillIndex(cmds, 0, 128, 0);
        const int b2 = fillIndex(cmds, 0, 0, 128);
        const int b3 = fillIndex(cmds, 128, 128, 0);
        REQUIRE(a >= 0);
        REQUIRE(b1 >= 0);
        REQUIRE(b2 >= 0);
        REQUIRE(b3 >= 0);
        CHECK(a < b1);
        CHECK(b1 < b3);
        CHECK(b3 < b2);
    }
    SUBCASE("b2 far behind (depth -50): b2, A, b1, b3") {
        const int a = fillIndex(cmds, 64, 0, 0);
        const int b1 = fillIndex(cmds, 0, 64, 0);
        const int b2 = fillIndex(cmds, 0, 0, 64);
        const int b3 = fillIndex(cmds, 64, 64, 0);
        REQUIRE(a >= 0);
        REQUIRE(b1 >= 0);
        REQUIRE(b2 >= 0);
        REQUIRE(b3 >= 0);
        CHECK(b2 < a);
        CHECK(a < b1);
        CHECK(b1 < b3);
    }
}

TEST_CASE("Painter 3D flat (T-727): each flat piece is wrapped in the root plane's PushTransform3D") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("flat_3d_interleave.xt", &shaper, &assets);
    CommandList cmds;
    doc->paint(cmds);

    // Every FillPath of b1 / b3 lies between a PushTransform3D whose world3 is the identity (the
    // root plane) and its PopTransform3D.
    int planeOpen = -1;  // command index of the open PushTransform3D, -1 when none
    bool identityPlane = false;
    int checked = 0;
    for (size_t i = 0; i < cmds.commands.size(); ++i) {
        const Command& cmd = cmds.commands[i];
        if (cmd.kind == CmdKind::PushTransform3D) {
            planeOpen = static_cast<int>(i);
            const Mat4& m = cmds.transforms3[cmd.index].world3;
            const Mat4 id = mat4Identity();
            identityPlane = true;
            for (int k = 0; k < 16; ++k) {
                identityPlane = identityPlane && std::fabs(m.m[k] - id.m[k]) < 1e-6f;
            }
        } else if (cmd.kind == CmdKind::PopTransform3D) {
            planeOpen = -1;
        } else if (cmd.kind == CmdKind::FillPath) {
            const Color& c = cmds.fills[cmd.index].paint.color;
            const bool flatChild = std::lround(c.g * 255.0f) == 255 && std::lround(c.b * 255.0f) == 0;
            if (flatChild) {  // b1-1 (00ff00) and b1-3 (ffff00)
                CHECK(planeOpen >= 0);
                CHECK(identityPlane);
                ++checked;
            }
        }
    }
    CHECK(checked == 2);
}

TEST_CASE("Painter 3D flat (T-727): group boundaries -- a clip is split, a Layer group is not") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("flat_3d_groups.xt", &shaper, &assets);
    CommandList cmds;
    doc->paint(cmds);

    SUBCASE("overflow clip: split, the clip pushed again around each piece") {
        const int a = fillIndex(cmds, 255, 0, 0);
        const int c1 = fillIndex(cmds, 0, 255, 0);
        const int c2 = fillIndex(cmds, 0, 0, 255);
        const int c3 = fillIndex(cmds, 255, 255, 0);
        REQUIRE(std::min({a, c1, c2, c3}) >= 0);
        CHECK(a < c1);
        CHECK(c1 < c2);
        CHECK(c2 < c3);
        int clips = 0;
        int pops = 0;
        for (const Command& cmd : cmds.commands) {
            clips += cmd.kind == CmdKind::PushClipRect ? 1 : 0;
            pops += cmd.kind == CmdKind::PopClip ? 1 : 0;
        }
        CHECK(clips == 2);  // #gbox1's padding box, once per piece (c1, then c3)
        CHECK(pops == clips);
    }
    SUBCASE("opacity group spanning two pieces: the pre-T-727 order (whole, before the items)") {
        const int a = fillIndex(cmds, 128, 0, 0);
        const int d1 = fillIndex(cmds, 0, 128, 0);
        const int d2 = fillIndex(cmds, 0, 0, 128);
        const int d3 = fillIndex(cmds, 128, 128, 0);
        REQUIRE(std::min({a, d1, d2, d3}) >= 0);
        CHECK(d1 < d3);
        CHECK(d3 < a);
        CHECK(a < d2);
    }
    SUBCASE("opacity group whose flat content is one piece: sorted as one item") {
        const int a = fillIndex(cmds, 64, 0, 0);
        const int e1 = fillIndex(cmds, 0, 64, 0);
        const int e2 = fillIndex(cmds, 0, 0, 64);
        const int e3 = fillIndex(cmds, 64, 64, 0);
        REQUIRE(std::min({a, e1, e2, e3}) >= 0);
        CHECK(a < e1);
        CHECK(e1 < e3);
        CHECK(e3 < e2);
    }
    // One Layer per opacity group, each drawn once.
    int layers = 0;
    for (const Command& cmd : cmds.commands) {
        layers += cmd.kind == CmdKind::PushLayer ? 1 : 0;
    }
    CHECK(layers == 2);
}

TEST_CASE("Painter 3D flat (T-727): War Table shape -- an animated figure no longer buries the others") {
    // Tests/golden/ui/ui_3d_flat_interleave.xt: board (rotateX, 3D layer) then a flat #figures with a
    // prop and four figures, #fig-2 a 3D layer through its running translateZ(0.01px) animation.
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("../golden/ui/ui_3d_flat_interleave.xt", &shaper, &assets);
    REQUIRE(doc->arena().common(doc->byId("fig-2").node).is3D != 0);
    CommandList cmds;
    doc->paint(cmds);

    const int board = fillIndex(cmds, 0x6b, 0x4a, 0x2b);
    REQUIRE(board >= 0);
    const int prop = fillIndex(cmds, 0xa0, 0xa0, 0xa0);
    const int fig1 = fillIndex(cmds, 0xe0, 0xc0, 0x60);
    const int fig2 = fillIndex(cmds, 0x60, 0xa0, 0xe0);
    const int fig3 = fillIndex(cmds, 0xe0, 0x60, 0x60);
    const int fig4 = fillIndex(cmds, 0x60, 0xe0, 0x60);
    CHECK(prop > board);
    CHECK(fig1 > board);
    CHECK(fig2 > board);
    CHECK(fig3 > board);
    CHECK(fig4 > board);
}

TEST_CASE("hitTest 3D flat (T-727): a flat child drawn above a 3D layer is hit before it") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("flat_3d_interleave.xt", &shaper, &assets);

    ElementRef a1 = doc->byId("a1");
    ElementRef b1 = doc->byId("b1-1");
    ElementRef b3 = doc->byId("b1-3");
    ElementRef b3far = doc->byId("b3-3");
    REQUIRE(a1.valid());
    REQUIRE(b1.valid());
    REQUIRE(b3.valid());
    REQUIRE(b3far.valid());

    // b1 (5..35, 40..80) and b3 (85..115, 40..80) lie on the root plane, inside A's projection.
    CHECK(doc->hitTest(Vec2{20, 60}) == b1.node);
    CHECK(doc->hitTest(Vec2{100, 60}) == b3.node);
    // #s3 (at x = 260): b3 lies above A there as well.
    CHECK(doc->hitTest(Vec2{360, 60}) == b3far.node);
}
