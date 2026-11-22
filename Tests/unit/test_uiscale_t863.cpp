#include <string>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/layout.h"
#include "xt/node.h"
#include "xt/property.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// T-863: three lengths measured under uiscale != 1 (CE-110's self tests). The `px` of a document is
/// design px, multiplied by the element's factor (Docs/design/25-editor-blueprint.md §25.4.2,
/// §25.4.2.x): grid `px` tracks (06 §6.15) scale like every other px length; a world anchor
/// (13 §13.8.13, 13-api.md `setWorldAnchor(PortId, screenX, screenY)`) puts the margin box's
/// top-left on the screen point, so the factor must not apply to it a second time; `rem` is
/// relative to the `<xt>` root element's font size (05 §5.9.8 T-335), not `<body>`'s.

using namespace xt;

namespace {

std::vector<uint8_t> compileDoc(const std::string& css, const std::string& body,
                                const std::string& bodyAttrs = "") {
    const std::string text =
        "<xt><head><style>" + css + "</style></head><body" + bodyAttrs + ">" + body + "</body></xt>";
    const compile::SourceFile file{"t863.xt", text};
    compile::CompileResult result =
        compile::compileDocumentFromStrings({&file, 1}, "t863.xt", compile::CompileOptions{});
    for (const compile::Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    return result.ir;
}

/// One Context + one Document in a 1000 x 800 viewport at uiscale `scale`.
struct Scaled {
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    DocumentHandle handle = 0;
    Document* doc = nullptr;

    Scaled(const std::vector<uint8_t>& ir, float scale) {
        Result<DocumentHandle> h = ctx.loadDocument(IrBlob{ir.data(), ir.size()}, true);
        REQUIRE(h.ok());
        handle = h.value();
        doc = ctx.document(handle);
        REQUIRE(doc != nullptr);
        ctx.setViewport(Rect{0, 0, 1000, 800}, 1.0f);
        REQUIRE(ctx.setUiScale(scale) == Status::kOk);
        ctx.update(0.0f);
    }

    NodeId node(const char* id) {
        ElementRef ref = doc->byId(id);
        REQUIRE(ref.valid());
        return ref.node;
    }
    const LayoutBox& box(const char* id) { return doc->arena().element(node(id)).box; }
    Vec2 screen(const char* id) { return doc->nodeToScreen(node(id), Vec2{0, 0}); }
    PortId port(const char* name) {
        const PortId p = doc->lookupPort(name);
        REQUIRE(p != kInvalidPort);
        return p;
    }
};

constexpr float kScales[] = {1.0f, 0.888f, 1.5f};

}  // namespace

// -------------------------------------------------------------------------------------------
// 1. grid-template-columns / -rows px tracks are design px (06 §6.15, 25 §25.4.2).
// -------------------------------------------------------------------------------------------

TEST_CASE("T-863: grid px tracks, repeat() and the gaps scale with uiscale") {
    const std::string css =
        "#G{position:absolute;left:0;top:0;display:grid;grid-template-columns:50px repeat(2, 30px);"
        "grid-template-rows:46px 20px;column-gap:4px;row-gap:7px}"
        "#F{position:absolute;left:0;top:300px;width:400px;height:200px;display:grid;"
        "grid-template-columns:100px 1fr;grid-template-rows:46px 1fr;gap:10px}";
    const std::string body =
        R"(<div id="G"><div id="a"></div><div id="b"></div><div id="c"></div><div id="d"></div></div>)"
        R"(<div id="F"><div id="e"></div><div id="f"></div><div id="g"></div><div id="h"></div></div>)";
    const std::vector<uint8_t> ir = compileDoc(css, body);
    for (const float s : kScales) {
        CAPTURE(s);
        Scaled l(ir, s);
        // The shrink-to-fit width (gridContentWidth) and the auto height are the tracks plus gaps.
        CHECK(l.box("G").width == doctest::Approx((50 + 30 + 30 + 2 * 4) * s));
        CHECK(l.box("G").height == doctest::Approx((46 + 7 + 20) * s));
        CHECK(l.box("a").width == doctest::Approx(50 * s));
        CHECK(l.box("a").height == doctest::Approx(46 * s));
        CHECK(l.box("b").width == doctest::Approx(30 * s));
        CHECK(l.box("c").x == doctest::Approx((50 + 4 + 30 + 4) * s));
        CHECK(l.box("d").y == doctest::Approx((46 + 7) * s));
        CHECK(l.box("d").height == doctest::Approx(20 * s));
        // fr takes what the scaled px tracks and gaps leave of the scaled container.
        CHECK(l.box("e").width == doctest::Approx(100 * s));
        CHECK(l.box("e").height == doctest::Approx(46 * s));
        CHECK(l.box("f").width == doctest::Approx((400 - 100 - 10) * s));
        CHECK(l.box("h").height == doctest::Approx((200 - 46 - 10) * s));
    }
}

TEST_CASE("T-863: a ui-scale: none grid keeps its px tracks") {
    const std::string css =
        "#G{position:absolute;left:0;top:0;display:grid;ui-scale:none;"
        "grid-template-columns:50px 30px;grid-template-rows:46px;gap:7px}";
    const std::vector<uint8_t> ir =
        compileDoc(css, R"(<div id="G"><div id="a"></div><div id="b"></div></div>)");
    Scaled l(ir, 1.5f);
    CHECK(l.box("G").width == doctest::Approx(87));
    CHECK(l.box("a").height == doctest::Approx(46));
}

// -------------------------------------------------------------------------------------------
// 2. setWorldAnchor's point is a screen (viewport logical) point (13 §13.8.13).
// -------------------------------------------------------------------------------------------

TEST_CASE("T-863: world-anchor puts the margin box on the screen point at any uiscale") {
    const std::string css =
        "#Box{position:absolute;left:40px;top:30px;width:200px;height:200px;padding:6px}"
        "#Tag{position:absolute;left:10px;top:20px;width:30px;height:10px;margin-left:4px;margin-top:2px}"
        "#Top{position:absolute;left:5px;top:5px;width:8px;height:8px}";
    const std::string body = R"(<div id="Hud" port:pos="vec2" port:pos3="vec3"></div>)"
                             R"(<div id="Box"><div id="Tag" world-anchor="pos"></div></div>)"
                             R"(<div id="Top" world-anchor="pos3"></div>)";
    const std::vector<uint8_t> ir = compileDoc(css, body);
    for (const float s : kScales) {
        CAPTURE(s);
        Scaled l(ir, s);
        REQUIRE(l.ctx.setWorldAnchor(l.handle, l.port("pos"), 100, 200) == Status::kOk);
        REQUIRE(l.doc->setWorldAnchor(l.port("pos3"), 321, 123, 9.0f, false) == Status::kOk);
        l.ctx.update(0.016f);
        // The border box is the margin box moved by the (scaled) margins.
        Vec2 o = l.screen("Tag");
        CHECK(o.x == doctest::Approx(100 + 4 * s));
        CHECK(o.y == doctest::Approx(200 + 2 * s));
        o = l.screen("Top");
        CHECK(o.x == doctest::Approx(321));
        CHECK(o.y == doctest::Approx(123));
        // The rest of the layout is untouched.
        CHECK(l.box("Box").width == doctest::Approx(200 * s));
        // A uiscale change keeps the anchored element on its point.
        REQUIRE(l.ctx.setUiScale(s * 1.25f) == Status::kOk);
        l.ctx.update(0.016f);
        o = l.screen("Tag");
        CHECK(o.x == doctest::Approx(100 + 4 * s * 1.25f));
        CHECK(o.y == doctest::Approx(200 + 2 * s * 1.25f));
    }
}

TEST_CASE("T-863: a world anchor inside a ui-scale: none subtree lands on the point too") {
    const std::string css =
        "#Box{position:absolute;left:40px;top:30px;width:200px;height:200px;ui-scale:none}"
        "#Tag{position:absolute;left:10px;top:20px;width:30px;height:10px}";
    const std::string body = R"(<div id="Hud" port:pos="vec2"></div>)"
                             R"(<div id="Box"><div id="Tag" world-anchor="pos"></div></div>)";
    Scaled l(compileDoc(css, body), 0.888f);
    REQUIRE(l.ctx.setWorldAnchor(l.handle, l.port("pos"), 100, 200) == Status::kOk);
    l.ctx.update(0.016f);
    const Vec2 o = l.screen("Tag");
    CHECK(o.x == doctest::Approx(100));
    CHECK(o.y == doctest::Approx(200));
}

// -------------------------------------------------------------------------------------------
// 3. rem is relative to `<xt>`'s font size (05 §5.9.8 T-335); `<body>` is not the root.
// -------------------------------------------------------------------------------------------

TEST_CASE("T-863: rem follows <xt>'s font size, not <body>'s, at any uiscale") {
    const std::string box = "#R{position:absolute;left:20px;top:20px;width:10rem;height:2rem}";
    const std::vector<uint8_t> onXt = compileDoc("xt{font-size:20px}" + box, R"(<div id="R"></div>)");
    const std::vector<uint8_t> onBody =
        compileDoc("#Root{font-size:20px}" + box, R"(<div id="R"></div>)", R"( id="Root")");
    for (const float s : kScales) {
        CAPTURE(s);
        Scaled x(onXt, s);
        CHECK(x.box("R").width == doctest::Approx(200 * s));
        CHECK(x.box("R").height == doctest::Approx(40 * s));
        Scaled b(onBody, s);
        CHECK(b.box("R").width == doctest::Approx(160 * s));  // 10 x the initial 16px.
        CHECK(b.box("R").height == doctest::Approx(32 * s));
    }
}
