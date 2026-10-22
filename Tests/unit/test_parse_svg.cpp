#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "doctest.h"
#include "shape_modifiers.h"  // Code/Runtime/core/src (Tests/CMakeLists.txt adds it to the include path).
#include "xt/compile/compile.h"
#include "xt/compile/ir_writer.h"
#include "xt/compile/parse_svg.h"
#include "xt/ir.h"
#include "xt/ir_pod.h"
#include "xt/node.h"
#include "xt/types.h"

/// Tests for `parseSvg` (Docs/design/12-compiler.md §12.2.1, Docs/tasks/T-110.md "測試").

using namespace xt;
using namespace xt::compile;

namespace {

std::string xtPath(const std::string& name) { return std::string(XT_TEST_XT_DIR) + "/" + name; }

/// Everything one `parseSvg()` call produces.
struct ParsedSvg {
    IrWriter writer;
    DiagnosticSink sink;
    SceneBuilder scene;
    Status status = Status::kInternal;

    /// T-225b: one IrPathData per Shape node (by kindIndex): the shape's paint fields plus the
    /// geometry the renderer builds from its parameters, appended to the scene's pools by
    /// materializeShapes().
    std::vector<IrPathData> shapePaths;

    /// The `IrPathData` of the `index`-th Path or (T-225b) Shape node, in node order.
    const IrPathData& path(size_t index) const {
        size_t seen = 0;
        for (const IrSceneNode& n : scene.nodes) {
            if (n.kind == static_cast<uint8_t>(NodeKind::Path) ||
                n.kind == static_cast<uint8_t>(NodeKind::Shape)) {
                if (seen == index) {
                    return n.kind == static_cast<uint8_t>(NodeKind::Path) ? scene.paths[n.kindIndex]
                                                                          : shapePaths[n.kindIndex];
                }
                ++seen;
            }
        }
        REQUIRE(false);
        return scene.paths[0];
    }

    /// Path and (T-225b) Shape nodes.
    size_t pathCount() const {
        size_t count = 0;
        for (const IrSceneNode& n : scene.nodes) {
            count += n.kind == static_cast<uint8_t>(NodeKind::Path) ||
                             n.kind == static_cast<uint8_t>(NodeKind::Shape)
                         ? 1u
                         : 0u;
        }
        return count;
    }

    /// Number of scene nodes of `kind`.
    size_t countKind(NodeKind kind) const {
        size_t count = 0;
        for (const IrSceneNode& n : scene.nodes) {
            count += n.kind == static_cast<uint8_t>(kind) ? 1u : 0u;
        }
        return count;
    }

    void materializeShapes() {
        shapePaths.clear();
        for (const IrShapeData& sd : scene.shapes) {
            std::vector<PathVerb> verbs;
            std::vector<float> points;
            modifiers::buildShape(static_cast<ShapeKind>(sd.kind), sd.params, verbs, points);
            IrPathData d{};
            d.verbOffset = static_cast<uint32_t>(scene.verbPool.size());
            d.verbCount = static_cast<uint32_t>(verbs.size());
            for (PathVerb v : verbs) {
                scene.verbPool.push_back(static_cast<uint8_t>(v));
            }
            d.pointOffset = static_cast<uint32_t>(scene.floatPool.size());
            d.pointCount = static_cast<uint32_t>(points.size());
            scene.floatPool.insert(scene.floatPool.end(), points.begin(), points.end());
            d.fillRule = sd.fillRule;
            d.fillKind = sd.fillKind;
            d.strokeKind = sd.strokeKind;
            d.cap = sd.cap;
            d.join = sd.join;
            d.fillColor = sd.fillColor;
            d.strokeColor = sd.strokeColor;
            d.fillP0 = sd.fillP0;
            d.fillP1 = sd.fillP1;
            d.strokeP0 = sd.strokeP0;
            d.strokeP1 = sd.strokeP1;
            d.fillStopOffset = sd.fillStopOffset;
            d.fillStopCount = sd.fillStopCount;
            d.strokeStopOffset = sd.strokeStopOffset;
            d.strokeStopCount = sd.strokeStopCount;
            d.strokeWidth = sd.strokeWidth;
            d.miterLimit = sd.miterLimit;
            d.trimStart = sd.trimStart;
            d.trimEnd = sd.trimEnd;
            d.trimOffset = sd.trimOffset;
            d.dashOffset = sd.dashOffset;
            d.dashCount = sd.dashCount;
            d.dashPhase = sd.dashPhase;
            shapePaths.push_back(d);
        }
    }
};

ParsedSvg parseText(const char* text) {
    ParsedSvg out;
    out.status = parseSvg(text, "test.svg", out.sink, out.writer, out.scene);
    out.materializeShapes();
    return out;
}

/// Wraps `body` in a 100x100 `<svg>` so the tests only spell out what they care about.
ParsedSvg parseBody(const std::string& body) {
    std::string doc = "<svg viewBox=\"0 0 100 100\">" + body + "</svg>";
    return parseText(doc.c_str());
}

std::vector<uint8_t> verbsOf(const ParsedSvg& p, size_t pathIndex = 0) {
    const IrPathData& d = p.path(pathIndex);
    return std::vector<uint8_t>(p.scene.verbPool.begin() + d.verbOffset,
                                p.scene.verbPool.begin() + d.verbOffset + d.verbCount);
}

constexpr uint8_t kMove = static_cast<uint8_t>(PathVerb::Move);
constexpr uint8_t kLine = static_cast<uint8_t>(PathVerb::Line);
constexpr uint8_t kCubic = static_cast<uint8_t>(PathVerb::Cubic);
constexpr uint8_t kClose = static_cast<uint8_t>(PathVerb::Close);

}  // namespace

TEST_CASE("svg: path d -- every command, mixed case, relative coordinates") {
    ParsedSvg p = parseBody(
        "<path d=\"M10 10 L20 10 H30 V20 C30,30 40,30 40,40 S50,50 50,50 "
        "Q60 50 60 60 T70 70 A 10 10 0 0 1 80,80 z\"/>");
    REQUIRE(p.status == Status::kOk);
    REQUIRE(p.pathCount() == 1);

    std::vector<uint8_t> verbs = verbsOf(p);
    // M L H V C S Q T then >= 1 arc cubic then Z.
    REQUIRE(verbs.size() >= 10);
    CHECK(verbs[0] == kMove);
    CHECK(verbs[1] == kLine);
    CHECK(verbs[2] == kLine);  // H
    CHECK(verbs[3] == kLine);  // V
    CHECK(verbs[4] == kCubic);
    CHECK(verbs[5] == kCubic);  // S
    CHECK(verbs[6] == kCubic);  // Q -> cubic
    CHECK(verbs[7] == kCubic);  // T -> cubic
    CHECK(verbs[8] == kCubic);  // A -> at least one cubic
    CHECK(verbs.back() == kClose);

    // Point count matches the verb operand sizes (Move/Line 2 floats, Cubic 6, Close 0).
    size_t expectedFloats = 0;
    size_t arcCubics = 0;
    for (uint8_t v : verbs) {
        expectedFloats += v == kCubic ? 6 : (v == kClose ? 0 : 2);
        arcCubics += v == kCubic ? 1 : 0;
    }
    CHECK(arcCubics >= 5);
    CHECK(p.path(0).pointCount == expectedFloats);

    // The arc's end point is exactly where `A ... 80,80` asked for.
    const IrPathData& d = p.path(0);
    size_t last = d.pointOffset + d.pointCount;
    CHECK(p.scene.floatPool[last - 2] == doctest::Approx(80.0f).epsilon(1e-3));
    CHECK(p.scene.floatPool[last - 1] == doctest::Approx(80.0f).epsilon(1e-3));
}

TEST_CASE("svg: a relative subpath and a lone arc land on the right points") {
    ParsedSvg p = parseBody("<path d=\"m10,10 l10,0 A 10 10 0 0 1 30 30\"/>");
    REQUIRE(p.status == Status::kOk);
    std::vector<uint8_t> verbs = verbsOf(p);
    REQUIRE(verbs.size() >= 3);
    CHECK(verbs[0] == kMove);
    CHECK(verbs[1] == kLine);
    size_t cubics = 0;
    for (uint8_t v : verbs) {
        cubics += v == kCubic ? 1 : 0;
    }
    CHECK(cubics >= 1);

    const IrPathData& d = p.path(0);
    const std::vector<float>& f = p.scene.floatPool;
    CHECK(f[d.pointOffset + 0] == doctest::Approx(10.0f));
    CHECK(f[d.pointOffset + 1] == doctest::Approx(10.0f));
    CHECK(f[d.pointOffset + 2] == doctest::Approx(20.0f));  // l10,0 is relative to (10,10).
    CHECK(f[d.pointOffset + 3] == doctest::Approx(10.0f));
    size_t last = d.pointOffset + d.pointCount;
    CHECK(f[last - 2] == doctest::Approx(30.0f).epsilon(1e-3));
    CHECK(f[last - 1] == doctest::Approx(30.0f).epsilon(1e-3));
}

TEST_CASE("svg: color notations and fill-opacity") {
    ParsedSvg p = parseBody(
        "<rect width=\"10\" height=\"10\" fill=\"#f00\"/>"
        "<rect width=\"10\" height=\"10\" fill=\"#00ff00\"/>"
        "<rect width=\"10\" height=\"10\" fill=\"#0000ff80\"/>"
        "<rect width=\"10\" height=\"10\" fill=\"rgb(255,255,0)\"/>"
        "<rect width=\"10\" height=\"10\" fill=\"rgba(0,0,0,0.25)\"/>"
        "<rect width=\"10\" height=\"10\" fill=\"red\"/>"
        "<rect width=\"10\" height=\"10\" fill=\"black\"/>"
        "<rect width=\"10\" height=\"10\" fill=\"white\"/>"
        "<rect width=\"10\" height=\"10\" fill=\"none\"/>"
        "<rect width=\"10\" height=\"10\" fill=\"transparent\"/>"
        "<rect width=\"10\" height=\"10\" fill=\"red\" fill-opacity=\"0.5\"/>");
    REQUIRE(p.status == Status::kOk);
    REQUIRE(p.pathCount() == 11);

    CHECK(p.path(0).fillColor == Color{1, 0, 0, 1});
    CHECK(p.path(1).fillColor == Color{0, 1, 0, 1});
    CHECK(p.path(2).fillColor.b == doctest::Approx(1.0f));
    CHECK(p.path(2).fillColor.a == doctest::Approx(128.0f / 255.0f));
    CHECK(p.path(3).fillColor == Color{1, 1, 0, 1});
    CHECK(p.path(4).fillColor.a == doctest::Approx(0.25f));
    CHECK(p.path(5).fillColor == Color{1, 0, 0, 1});
    CHECK(p.path(6).fillColor == Color{0, 0, 0, 1});
    CHECK(p.path(7).fillColor == Color{1, 1, 1, 1});
    CHECK(p.path(8).fillKind == static_cast<uint8_t>(PaintKind::None));
    CHECK(p.path(9).fillColor.a == doctest::Approx(0.0f));
    CHECK(p.path(10).fillColor.a == doctest::Approx(0.5f));
    // The default fill is black, the default stroke is none.
    CHECK(p.path(0).strokeKind == static_cast<uint8_t>(PaintKind::None));
}

TEST_CASE("T-322 svg: named colours come from the shared CSS Color Level 4 table") {
    // The names the old 31-entry list knew keep their values; the rest of the 148 now parse too.
    ParsedSvg p = parseBody(
        "<rect width=\"10\" height=\"10\" fill=\"Orange\"/>"
        "<rect width=\"10\" height=\"10\" fill=\"grey\"/>"
        "<rect width=\"10\" height=\"10\" fill=\"turquoise\"/>"
        "<rect width=\"10\" height=\"10\" fill=\"rebeccapurple\"/>"
        "<rect width=\"10\" height=\"10\" fill=\"lightgoldenrodyellow\"/>");
    REQUIRE(p.status == Status::kOk);
    REQUIRE(p.pathCount() == 5);
    CHECK(p.path(0).fillColor == Color{1, 165.0f / 255.0f, 0, 1});
    CHECK(p.path(1).fillColor == Color{128.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f, 1});
    CHECK(p.path(2).fillColor == Color{64.0f / 255.0f, 224.0f / 255.0f, 208.0f / 255.0f, 1});
    CHECK(p.path(3).fillColor == Color{102.0f / 255.0f, 51.0f / 255.0f, 153.0f / 255.0f, 1});
    CHECK(p.path(4).fillColor == Color{250.0f / 255.0f, 250.0f / 255.0f, 210.0f / 255.0f, 1});
}

TEST_CASE("svg: url(#g) linear gradient, both gradientUnits") {
    ParsedSvg p = parseBody(
        "<defs>"
        "<linearGradient id=\"g\" x1=\"0\" y1=\"0\" x2=\"1\" y2=\"0\">"
        "<stop offset=\"0\" stop-color=\"#ff0000\"/>"
        "<stop offset=\"1\" stop-color=\"#0000ff\" stop-opacity=\"0.5\"/>"
        "</linearGradient>"
        "<linearGradient id=\"u\" gradientUnits=\"userSpaceOnUse\" x1=\"5\" y1=\"6\" x2=\"7\" y2=\"8\">"
        "<stop offset=\"0\" stop-color=\"white\"/><stop offset=\"1\" stop-color=\"black\"/>"
        "</linearGradient>"
        "</defs>"
        "<rect x=\"10\" y=\"20\" width=\"80\" height=\"40\" fill=\"url(#g)\"/>"
        "<rect x=\"10\" y=\"20\" width=\"80\" height=\"40\" fill=\"url(#u)\"/>");
    REQUIRE(p.status == Status::kOk);
    // <defs> contents are never drawn: only the two rects produce Path nodes.
    REQUIRE(p.pathCount() == 2);

    const IrPathData& a = p.path(0);
    CHECK(a.fillKind == static_cast<uint8_t>(PaintKind::Linear));
    REQUIRE(a.fillStopCount == 2);
    CHECK(p.scene.stopPool[a.fillStopOffset].offset == doctest::Approx(0.0f));
    CHECK(p.scene.stopPool[a.fillStopOffset].color == Color{1, 0, 0, 1});
    CHECK(p.scene.stopPool[a.fillStopOffset + 1].color.b == doctest::Approx(1.0f));
    CHECK(p.scene.stopPool[a.fillStopOffset + 1].color.a == doctest::Approx(0.5f));
    // objectBoundingBox: the unit square maps onto the rect's own box.
    CHECK(a.fillP0.x == doctest::Approx(10.0f));
    CHECK(a.fillP0.y == doctest::Approx(20.0f));
    CHECK(a.fillP1.x == doctest::Approx(90.0f));
    CHECK(a.fillP1.y == doctest::Approx(20.0f));

    const IrPathData& b = p.path(1);
    CHECK(b.fillKind == static_cast<uint8_t>(PaintKind::Linear));
    CHECK(b.fillP0.x == doctest::Approx(5.0f));
    CHECK(b.fillP0.y == doctest::Approx(6.0f));
    CHECK(b.fillP1.x == doctest::Approx(7.0f));
    CHECK(b.fillP1.y == doctest::Approx(8.0f));
    CHECK(p.scene.stopPool.size() == 4);
}

// T-236: url(#Id) keeps the id's case, and the lookup is case-sensitive (SVG ids are).
TEST_CASE("svg: url(#GradA) resolves a mixed-case gradient id, url(#grada) does not") {
    ParsedSvg p = parseBody(
        "<defs>"
        "<linearGradient id=\"GradA\" x1=\"0\" y1=\"0\" x2=\"1\" y2=\"0\">"
        "<stop offset=\"0\" stop-color=\"#ff0000\"/><stop offset=\"1\" stop-color=\"#0000ff\"/>"
        "</linearGradient>"
        "</defs>"
        "<rect x=\"10\" y=\"20\" width=\"80\" height=\"40\" fill=\"url(#GradA)\"/>"
        "<rect x=\"10\" y=\"20\" width=\"80\" height=\"40\" style=\"fill: URL('#GradA')\"/>"
        "<rect x=\"10\" y=\"20\" width=\"80\" height=\"40\" fill=\"url(#grada)\"/>");
    REQUIRE(p.status == Status::kOk);
    REQUIRE(p.pathCount() == 3);
    CHECK(p.path(0).fillKind == static_cast<uint8_t>(PaintKind::Linear));
    CHECK(p.path(0).fillStopCount == 2);
    CHECK(p.path(1).fillKind == static_cast<uint8_t>(PaintKind::Linear));
    CHECK(p.path(1).fillStopCount == 2);
    CHECK(p.path(2).fillStopCount == 0);  // Not found: painted black, with a warning.
    CHECK(p.path(2).fillColor == Color{0, 0, 0, 1});
}

TEST_CASE("svg: radial gradient maps onto center/edge point") {
    ParsedSvg p = parseBody(
        "<radialGradient id=\"r\" gradientUnits=\"userSpaceOnUse\" cx=\"40\" cy=\"50\" r=\"25\">"
        "<stop offset=\"0\" stop-color=\"white\"/><stop offset=\"1\" stop-color=\"black\"/>"
        "</radialGradient>"
        "<circle cx=\"40\" cy=\"50\" r=\"25\" fill=\"url(#r)\"/>");
    REQUIRE(p.status == Status::kOk);
    const IrPathData& d = p.path(0);
    CHECK(d.fillKind == static_cast<uint8_t>(PaintKind::Radial));
    CHECK(d.fillP0.x == doctest::Approx(40.0f));
    CHECK(d.fillP0.y == doctest::Approx(50.0f));
    CHECK(d.fillP1.x == doctest::Approx(65.0f));
    CHECK(d.fillP1.y == doctest::Approx(50.0f));
}

TEST_CASE("svg: chained transform matches the hand-computed matrix") {
    ParsedSvg p = parseBody(
        "<g transform=\"translate(10,20) rotate(90) scale(2)\"><rect width=\"4\" "
        "height=\"4\"/></g>");
    REQUIRE(p.status == Status::kOk);
    // nodes[0] is the scene root; nodes[1] is the <g>.
    REQUIRE(p.scene.nodes.size() == 3);
    const IrSceneNode& g = p.scene.nodes[1];
    CHECK(g.kind == static_cast<uint8_t>(NodeKind::Group));

    Mat2x3 expected = mul(translation(Vec2{10, 20}), mul(rotationDeg(90), scaling(Vec2{2, 2})));
    Mat2x3 actual = composeTRSS(g.translate, g.rotation, g.skew, g.scale, g.anchor);
    CHECK(approxEqual(actual, expected, 1e-4f));
}

TEST_CASE("svg: skew and matrix transforms round-trip through the node fields") {
    ParsedSvg p = parseBody("<g transform=\"matrix(2 0 1 3 5 7)\"><rect width=\"4\" height=\"4\"/></g>");
    REQUIRE(p.status == Status::kOk);
    const IrSceneNode& g = p.scene.nodes[1];
    Mat2x3 expected{2, 0, 1, 3, 5, 7};
    CHECK(approxEqual(composeTRSS(g.translate, g.rotation, g.skew, g.scale, g.anchor), expected, 1e-4f));
}

TEST_CASE("svg: <use> expands its target, <defs> contents are not drawn on their own") {
    ParsedSvg p = parseBody(
        "<defs><rect id=\"r\" width=\"10\" height=\"10\" fill=\"red\"/></defs>"
        "<use href=\"#r\" x=\"30\" y=\"40\"/>");
    REQUIRE(p.status == Status::kOk);
    REQUIRE(p.pathCount() == 1);
    // Root Group + the <use> Group + the expanded rect.
    REQUIRE(p.scene.nodes.size() == 3);
    const IrSceneNode& useGroup = p.scene.nodes[1];
    CHECK(useGroup.kind == static_cast<uint8_t>(NodeKind::Group));
    CHECK(useGroup.translate.x == doctest::Approx(30.0f));
    CHECK(useGroup.translate.y == doctest::Approx(40.0f));
    CHECK(p.path(0).fillColor == Color{1, 0, 0, 1});
    CHECK(p.sink.warningCount() == 0);
}

TEST_CASE("svg: unsupported elements are skipped with a warning, the rest still imports") {
    ParsedSvg p = parseBody(
        "<text x=\"0\" y=\"0\">hi</text>"
        "<filter id=\"f\"><feGaussianBlur stdDeviation=\"2\"/></filter>"
        "<rect width=\"10\" height=\"10\" fill=\"blue\"/>");
    REQUIRE(p.status == Status::kOk);
    CHECK(p.pathCount() == 1);
    CHECK(p.path(0).fillColor == Color{0, 0, 1, 1});
    CHECK(p.sink.warningCount() == 2);
    CHECK_FALSE(p.sink.hasErrors());
}

TEST_CASE("svg: style attribute wins over the presentation attribute, and inherits") {
    ParsedSvg p = parseBody(
        "<g style=\"fill:#00ff00;stroke:red;stroke-width:3;stroke-linecap:round;"
        "stroke-linejoin:bevel;stroke-dasharray:4 2\" fill-rule=\"evenodd\">"
        "<rect width=\"10\" height=\"10\" fill=\"blue\" style=\"fill:#ffffff\"/>"
        "<rect width=\"10\" height=\"10\"/>"
        "</g>");
    REQUIRE(p.status == Status::kOk);
    CHECK(p.path(0).fillColor == Color{1, 1, 1, 1});
    CHECK(p.path(1).fillColor == Color{0, 1, 0, 1});
    CHECK(p.path(1).strokeKind == static_cast<uint8_t>(PaintKind::Solid));
    CHECK(p.path(1).strokeColor == Color{1, 0, 0, 1});
    CHECK(p.path(1).strokeWidth == doctest::Approx(3.0f));
    CHECK(p.path(1).cap == static_cast<uint8_t>(LineCap::Round));
    CHECK(p.path(1).join == static_cast<uint8_t>(LineJoin::Bevel));
    CHECK(p.path(1).fillRule == static_cast<uint8_t>(FillRule::EvenOdd));
    REQUIRE(p.path(1).dashCount == 2);
    CHECK(p.scene.floatPool[p.path(1).dashOffset] == doctest::Approx(4.0f));
}

TEST_CASE("svg: viewBox sets the design size and the origin; missing one warns") {
    ParsedSvg a = parseText("<svg viewBox=\"10 20 300 200\" width=\"600\" height=\"400\"></svg>");
    REQUIRE(a.status == Status::kOk);
    CHECK(a.scene.width == doctest::Approx(300.0f));
    CHECK(a.scene.height == doctest::Approx(200.0f));
    CHECK(a.scene.nodes[0].translate.x == doctest::Approx(-10.0f));
    CHECK(a.scene.nodes[0].translate.y == doctest::Approx(-20.0f));

    ParsedSvg b = parseText("<svg width=\"120\" height=\"60\"></svg>");
    REQUIRE(b.status == Status::kOk);
    CHECK(b.scene.width == doctest::Approx(120.0f));
    CHECK(b.scene.height == doctest::Approx(60.0f));
    CHECK(b.sink.warningCount() == 0);

    ParsedSvg c = parseText("<svg></svg>");
    REQUIRE(c.status == Status::kOk);
    CHECK(c.scene.width == doctest::Approx(300.0f));
    CHECK(c.scene.height == doctest::Approx(150.0f));
    CHECK(c.sink.warningCount() == 1);
}

TEST_CASE("svg: XML comments, CDATA, the XML declaration and entities are handled") {
    ParsedSvg p = parseText(
        "<?xml version=\"1.0\"?>\n"
        "<!-- a comment -->\n"
        "<svg viewBox=\"0 0 10 10\">\n"
        "  <title>a &amp; b &lt;c&gt; &#65;</title>\n"
        "  <rect width=\"4\" height=\"4\" fill=\"red\"/>\n"
        "</svg>");
    REQUIRE(p.status == Status::kOk);
    CHECK(p.pathCount() == 1);
}

TEST_CASE("svg: a malformed document is an error, not a crash") {
    ParsedSvg p = parseText("<svg><rect width=\"4\"></svg>");
    CHECK(p.status == Status::kInvalidArgument);
    CHECK(p.sink.hasErrors());

    ParsedSvg q = parseText("<html><body/></html>");
    CHECK(q.status == Status::kInvalidArgument);
    CHECK(q.sink.hasErrors());
}

TEST_CASE("compileDocument: two <img> on the same .svg share one scene") {
    CompileOptions options;
    CompileResult result = compileDocument(xtPath("doc_svg.xt"), options);
    REQUIRE(result.ok());
    Result<IrView> view = IrView::open(IrBlob{result.ir.data(), result.ir.size()});
    REQUIRE(view.ok());
    // One scene for the .svg, shared by both <img>; no other scene in the document.
    CHECK(view.value().sceneCount() == 1);

    uint32_t withScene = 0;
    for (const IrDocElement& el : view.value().doc().elements()) {
        withScene += el.scene != kInvalidScene ? 1u : 0u;
    }
    CHECK(withScene == 2);
    // The scene's design size is the .svg's viewBox size.
    CHECK(view.value().scene(0).width() == doctest::Approx(64.0f));
    CHECK(view.value().scene(0).height() == doctest::Approx(64.0f));
}

TEST_CASE("compileDocument: <text> in an imported .svg warns, and --strict turns it into a failure") {
    CompileOptions options;
    CompileResult result = compileDocument(xtPath("doc_svg_text.xt"), options);
    CHECK(result.ok());
    size_t warnings = 0;
    for (const Diagnostic& d : result.diagnostics) {
        warnings += d.level == Diagnostic::Level::Warning ? 1u : 0u;
    }
    CHECK(warnings >= 1);

    CompileOptions strict;
    strict.strict = true;
    CompileResult strictResult = compileDocument(xtPath("doc_svg_text.xt"), strict);
    CHECK_FALSE(strictResult.ok());
}

TEST_CASE("svg: stroke-dasharray as a presentation attribute reaches IrPathData") {
    ParsedSvg p = parseBody(
        "<path d=\"M10 50 H 90\" stroke=\"#884499\" stroke-width=\"10\" stroke-dasharray=\"14 8\" "
        "stroke-linecap=\"square\" fill=\"none\"/>");
    REQUIRE(p.status == Status::kOk);
    const IrPathData& d = p.path(0);
    REQUIRE(d.dashCount == 2);
    CHECK(p.scene.floatPool[d.dashOffset] == doctest::Approx(14.0f));
    CHECK(p.scene.floatPool[d.dashOffset + 1] == doctest::Approx(8.0f));
    CHECK(d.cap == static_cast<uint8_t>(LineCap::Square));
    CHECK(d.fillKind == static_cast<uint8_t>(PaintKind::None));
}

// ---------------------------------------------------------------------------------------------
// T-119: gradients are collected document-wide before any shape is built (12.2.1), so `url(#id)`
// resolves regardless of where the definition sits, and a stop-less gradient inherits its stops
// through `href`.
// ---------------------------------------------------------------------------------------------

TEST_CASE("svg: T-119 <defs> after the shape that references it") {
    ParsedSvg p = parseBody(
        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" fill=\"url(#late)\"/>"
        "<defs>"
        "<linearGradient id=\"late\" x1=\"0\" y1=\"0\" x2=\"1\" y2=\"0\">"
        "<stop offset=\"0\" stop-color=\"#ff0000\"/><stop offset=\"1\" stop-color=\"#0000ff\"/>"
        "</linearGradient>"
        "</defs>");
    REQUIRE(p.status == Status::kOk);
    CHECK(p.sink.all().empty());
    REQUIRE(p.pathCount() == 1);
    const IrPathData& d = p.path(0);
    CHECK(d.fillKind == static_cast<uint8_t>(PaintKind::Linear));
    REQUIRE(d.fillStopCount == 2);
    CHECK(p.scene.stopPool[d.fillStopOffset].color == Color{1, 0, 0, 1});
}

TEST_CASE("svg: T-119 <defs> nested inside a <g>") {
    ParsedSvg p = parseBody(
        "<g transform=\"translate(10,0)\">"
        "<defs>"
        "<radialGradient id=\"deep\" cx=\"0.5\" cy=\"0.5\" r=\"0.5\">"
        "<stop offset=\"0\" stop-color=\"#00ff00\"/><stop offset=\"1\" stop-color=\"#000000\"/>"
        "</radialGradient>"
        "</defs>"
        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" fill=\"url(#deep)\"/>"
        "</g>");
    REQUIRE(p.status == Status::kOk);
    CHECK(p.sink.all().empty());
    REQUIRE(p.pathCount() == 1);
    const IrPathData& d = p.path(0);
    CHECK(d.fillKind == static_cast<uint8_t>(PaintKind::Radial));
    REQUIRE(d.fillStopCount == 2);
    CHECK(p.scene.stopPool[d.fillStopOffset].color == Color{0, 1, 0, 1});
}

TEST_CASE("svg: T-119 href inherits the referenced gradient's stops") {
    ParsedSvg p = parseBody(
        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" fill=\"url(#child)\"/>"
        "<defs>"
        "<linearGradient id=\"child\" xlink:href=\"#base\" x1=\"0\" y1=\"0\" x2=\"0\" y2=\"1\"/>"
        "<linearGradient id=\"base\">"
        "<stop offset=\"0\" stop-color=\"#ff0000\"/><stop offset=\"1\" stop-color=\"#0000ff\"/>"
        "</linearGradient>"
        "</defs>");
    REQUIRE(p.status == Status::kOk);
    CHECK(p.sink.all().empty());
    REQUIRE(p.pathCount() == 1);
    const IrPathData& d = p.path(0);
    CHECK(d.fillKind == static_cast<uint8_t>(PaintKind::Linear));
    REQUIRE(d.fillStopCount == 2);
    CHECK(p.scene.stopPool[d.fillStopOffset].color == Color{1, 0, 0, 1});
    CHECK(p.scene.stopPool[d.fillStopOffset + 1].color == Color{0, 0, 1, 1});
    // The referencing gradient keeps its own geometry (x2/y2 = 0,1 -> vertical).
    CHECK(d.fillP1.x == doctest::Approx(0.0f));
    CHECK(d.fillP1.y == doctest::Approx(100.0f));
}

TEST_CASE("svg: T-223 <xt-star> five-point star expands to the golden path, id recorded") {
    ParsedSvg p =
        parseBody("<xt-star id=\"Ping\" cx=\"50\" cy=\"50\" points=\"5\" r=\"40\" r2=\"20\" fill=\"#fff\"/>");
    REQUIRE(p.status == Status::kOk);
    CHECK(p.sink.warningCount() == 0);
    REQUIRE(p.pathCount() == 1);
    std::vector<uint8_t> verbs = verbsOf(p);
    REQUIRE(verbs.size() == 11);
    CHECK(verbs[0] == kMove);
    for (size_t i = 1; i < 10; ++i) {
        CHECK(verbs[i] == kLine);
    }
    CHECK(verbs[10] == kClose);
    const float golden[10][2] = {
        {50.000f, 10.000f}, {61.756f, 33.820f}, {88.042f, 37.639f}, {69.021f, 56.180f}, {73.511f, 82.361f},
        {50.000f, 70.000f}, {26.489f, 82.361f}, {30.979f, 56.180f}, {11.958f, 37.639f}, {38.244f, 33.820f}};
    const IrPathData& d = p.path(0);
    REQUIRE(d.pointCount == 20);
    for (size_t i = 0; i < 10; ++i) {
        CHECK(p.scene.floatPool[d.pointOffset + i * 2] == doctest::Approx(golden[i][0]).epsilon(1e-4));
        CHECK(p.scene.floatPool[d.pointOffset + i * 2 + 1] == doctest::Approx(golden[i][1]).epsilon(1e-4));
    }
    CHECK(d.fillColor == Color{1, 1, 1, 1});
    bool named = false;
    for (const IrSceneNode& n : p.scene.nodes) {
        named =
            named || (n.kind == static_cast<uint8_t>(NodeKind::Shape) && n.name == p.writer.intern("Ping"));
    }
    CHECK(named);
}

TEST_CASE("svg: T-223 <xt-star> without r2 is a regular polygon; rotation turns it") {
    ParsedSvg p = parseBody("<xt-star cx=\"50\" cy=\"50\" points=\"6\" r=\"30\" rotation=\"30\"/>");
    REQUIRE(p.status == Status::kOk);
    REQUIRE(p.pathCount() == 1);
    std::vector<uint8_t> verbs = verbsOf(p);
    REQUIRE(verbs.size() == 7);  // M + 5 L + Z
    const IrPathData& d = p.path(0);
    // rotation 30: the first corner sits at -60 deg.
    CHECK(p.scene.floatPool[d.pointOffset] == doctest::Approx(65.0f).epsilon(1e-4));
    CHECK(p.scene.floatPool[d.pointOffset + 1] == doctest::Approx(50.0f - 25.980762f).epsilon(1e-4));
    for (size_t i = 0; i < 6; ++i) {
        float x = p.scene.floatPool[d.pointOffset + i * 2] - 50.0f;
        float y = p.scene.floatPool[d.pointOffset + i * 2 + 1] - 50.0f;
        CHECK(std::sqrt(x * x + y * y) == doctest::Approx(30.0f).epsilon(1e-4));
    }
}

TEST_CASE("svg: T-224 <xt-star> round 0.5 cuts each corner into two anchors with handles toward it") {
    ParsedSvg p = parseBody("<xt-star cx=\"50\" cy=\"50\" points=\"4\" r=\"30\" round=\"0.5\"/>");
    REQUIRE(p.status == Status::kOk);
    std::vector<uint8_t> verbs = verbsOf(p);
    REQUIRE(verbs.size() == 9);  // M C (L C) x3 Z: the vertex count doubles
    CHECK(verbs[1] == kCubic);
    for (size_t i = 2; i < 8; i += 2) {
        CHECK(verbs[i] != kCubic);
        CHECK(verbs[i + 1] == kCubic);
    }
    const IrPathData& d = p.path(0);
    // Corner (50,20) between (20,50) and (80,50): edges 30*sqrt(2), cut d = 0.5 * edge / 2.
    const float s = 30.0f * std::sqrt(2.0f);
    const float cut = 0.5f * s / 2.0f;
    const float k = cut / std::sqrt(2.0f);             // cut along a 45-degree edge
    const float hk = 0.5523f * cut / std::sqrt(2.0f);  // handle along the same edge
    const float* f = &p.scene.floatPool[d.pointOffset];
    CHECK(f[0] == doctest::Approx(50.0f - k).epsilon(1e-4));
    CHECK(f[1] == doctest::Approx(20.0f + k).epsilon(1e-4));
    CHECK(f[2] == doctest::Approx(50.0f - k + hk).epsilon(1e-4));
    CHECK(f[3] == doctest::Approx(20.0f + k - hk).epsilon(1e-4));
    CHECK(f[4] == doctest::Approx(50.0f + k - hk).epsilon(1e-4));
    CHECK(f[5] == doctest::Approx(20.0f + k - hk).epsilon(1e-4));
    CHECK(f[6] == doctest::Approx(50.0f + k).epsilon(1e-4));
    CHECK(f[7] == doctest::Approx(20.0f + k).epsilon(1e-4));
    // round 0 stays a sharp polygon.
    ParsedSvg sharp = parseBody("<xt-star cx=\"50\" cy=\"50\" points=\"4\" r=\"30\" round=\"0\"/>");
    CHECK(verbsOf(sharp).size() == 5);
}

TEST_CASE("svg: T-223 <xt-star> without points or r warns and draws nothing") {
    ParsedSvg p = parseBody(
        "<xt-star cx=\"50\" cy=\"50\" r=\"30\"/><xt-star points=\"5\"/><xt-star points=\"2\" r=\"5\"/>");
    REQUIRE(p.status == Status::kOk);
    CHECK(p.pathCount() == 0);
    CHECK(p.sink.warningCount() == 3);
    CHECK_FALSE(p.sink.hasErrors());
}

TEST_CASE("svg: T-223 data-xt-* attributes are ignored") {
    ParsedSvg p = parseBody(
        "<path id=\"P\" d=\"M0 0 L10 0 L10 10 Z\" data-xt-vertex-types=\"c s y\" data-xt-anything=\"1\" "
        "fill=\"red\"/>");
    REQUIRE(p.status == Status::kOk);
    CHECK(p.sink.warningCount() == 0);
    REQUIRE(p.pathCount() == 1);
    CHECK(verbsOf(p).size() == 4);
    CHECK(p.path(0).fillColor == Color{1, 0, 0, 1});
}

// --- T-225b (Docs/design/31-vector-authoring.md §31.6.1, §31.5.3) ---

namespace {

/// Parses `body` (inside a 100x100 `<svg>`) and also collects the recorded ids.
struct ParsedSvgIds {
    ParsedSvg p;
    std::vector<std::pair<std::string, uint32_t>> ids;
};

ParsedSvgIds parseBodyIds(const std::string& body) {
    ParsedSvgIds out;
    std::string doc = "<svg viewBox=\"0 0 100 100\">" + body + "</svg>";
    out.p.status = parseSvg(doc, "test.svg", out.p.sink, out.p.writer, out.p.scene, &out.ids);
    out.p.materializeShapes();
    return out;
}

/// Index of the first scene node of `kind` at or after `from`; SIZE_MAX if none.
size_t findKind(const ParsedSvg& p, NodeKind kind, size_t from = 0) {
    for (size_t i = from; i < p.scene.nodes.size(); ++i) {
        if (p.scene.nodes[i].kind == static_cast<uint8_t>(kind)) {
            return i;
        }
    }
    return SIZE_MAX;
}

const IrModifierData& modifierAt(const ParsedSvg& p, size_t ordinal) {
    size_t seen = 0;
    for (const IrSceneNode& n : p.scene.nodes) {
        if (n.kind == static_cast<uint8_t>(NodeKind::Modifier)) {
            if (seen == ordinal) {
                return p.scene.modifiers[n.kindIndex];
            }
            ++seen;
        }
    }
    REQUIRE(false);
    return p.scene.modifiers[0];
}

bool warned(const ParsedSvg& p, std::string_view needle) {
    for (const Diagnostic& d : p.sink.all()) {
        if (d.level == Diagnostic::Level::Warning && d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("svg: T-225b rect / circle / ellipse / xt-star are Shape nodes; line, polygon, path stay Path") {
    ParsedSvgIds r = parseBodyIds(
        "<rect id=\"R\" x=\"1\" y=\"2\" width=\"30\" height=\"20\" rx=\"4\" fill=\"#ff0000\"/>"
        "<rect x=\"0\" y=\"0\" width=\"10\" height=\"10\" ry=\"3\"/>"
        "<circle cx=\"50\" cy=\"40\" r=\"9\"/>"
        "<ellipse cx=\"5\" cy=\"6\" rx=\"7\" ry=\"8\"/>"
        "<xt-star id=\"S\" cx=\"50\" cy=\"50\" points=\"6\" r=\"20\" r2=\"10\" rotation=\"15\" round=\"2\"/>"
        "<line x1=\"0\" y1=\"0\" x2=\"5\" y2=\"5\" stroke=\"#000\"/>"
        "<polygon points=\"0,0 5,0 5,5\"/><path d=\"M0 0 L1 1\"/>");
    const ParsedSvg& p = r.p;
    REQUIRE(p.status == Status::kOk);
    CHECK(p.sink.warningCount() == 0);
    CHECK(p.countKind(NodeKind::Shape) == 5);
    CHECK(p.countKind(NodeKind::Path) == 3);
    REQUIRE(p.scene.shapes.size() == 5);

    const IrShapeData& rect = p.scene.shapes[0];
    CHECK(rect.kind == static_cast<uint8_t>(ShapeKind::Rect));
    const float rectParams[6] = {1, 2, 30, 20, 4, 4};  // ry missing -> rx.
    for (int i = 0; i < 6; ++i) {
        CHECK(rect.params[i] == doctest::Approx(rectParams[i]));
    }
    CHECK(rect.fillColor == Color{1, 0, 0, 1});
    CHECK(rect.trimEnd == doctest::Approx(1.0f));
    CHECK(p.scene.shapes[1].params[4] == doctest::Approx(3.0f));  // rx missing -> ry.
    CHECK(p.scene.shapes[1].params[5] == doctest::Approx(3.0f));

    const IrShapeData& circle = p.scene.shapes[2];
    CHECK(circle.kind == static_cast<uint8_t>(ShapeKind::Ellipse));
    CHECK(circle.params[0] == doctest::Approx(50.0f));
    CHECK(circle.params[2] == doctest::Approx(9.0f));
    CHECK(circle.params[3] == doctest::Approx(9.0f));
    CHECK(p.scene.shapes[3].params[3] == doctest::Approx(8.0f));

    const IrShapeData& star = p.scene.shapes[4];
    CHECK(star.kind == static_cast<uint8_t>(ShapeKind::Star));
    const float starParams[8] = {50, 50, 6, 20, 10, 15, 1, 0};  // round clamped to 1.
    for (int i = 0; i < 8; ++i) {
        CHECK(star.params[i] == doctest::Approx(starParams[i]));
    }

    // svgTag per element; ids recorded for the Shape nodes.
    const uint8_t tags[] = {static_cast<uint8_t>(IrSvgTag::Rect), static_cast<uint8_t>(IrSvgTag::Rect),
                            static_cast<uint8_t>(IrSvgTag::Circle), static_cast<uint8_t>(IrSvgTag::Ellipse),
                            static_cast<uint8_t>(IrSvgTag::XtStar)};
    size_t shapeIndex = 0;
    for (const IrSceneNode& n : p.scene.nodes) {
        if (n.kind == static_cast<uint8_t>(NodeKind::Shape)) {
            CHECK(n.svgTag == tags[shapeIndex++]);
        }
    }
    REQUIRE(r.ids.size() == 2);
    CHECK(r.ids[0].first == "R");
    CHECK(p.scene.nodes[r.ids[0].second].kind == static_cast<uint8_t>(NodeKind::Shape));
    CHECK(r.ids[1].first == "S");
    CHECK(p.scene.nodes[r.ids[1].second].kind == static_cast<uint8_t>(NodeKind::Shape));
}

TEST_CASE("svg: T-225b path-trim on a Shape; a degenerate rect is still a Shape node") {
    ParsedSvg p = parseBody(
        "<circle cx=\"0\" cy=\"0\" r=\"5\" path-trim=\"0.25 0.5 0.1\"/>"
        "<rect x=\"0\" y=\"0\" width=\"0\" height=\"10\"/>");
    REQUIRE(p.status == Status::kOk);
    REQUIRE(p.scene.shapes.size() == 2);
    CHECK(p.scene.shapes[0].trimStart == doctest::Approx(0.25f));
    CHECK(p.scene.shapes[0].trimEnd == doctest::Approx(0.5f));
    CHECK(p.scene.shapes[0].trimOffset == doctest::Approx(0.1f));
    CHECK(p.scene.shapes[1].params[2] == doctest::Approx(0.0f));
    CHECK(p.sink.warningCount() == 0);
}

TEST_CASE("svg: T-225b every <xt-*> modifier element becomes a Modifier node with its parameters") {
    ParsedSvgIds r = parseBodyIds(
        "<g><path d=\"M0 0 L10 0 L10 10 Z\"/>"
        "<xt-trim id=\"T\" trim-start=\"0.1\" trim-end=\"80%\" trim-offset=\"90\" mode=\"serial\"/>"
        "<xt-repeat id=\"Rp\" copies=\"5\" repeat-offset=\"1\" repeat-anchor=\"2 3\" repeat-translate=\"4 "
        "5\" "
        "repeat-rotation=\"30\" repeat-scale=\"0.9\" opacity-start=\"1\" opacity-end=\"20%\" "
        "order=\"above\"/>"
        "<xt-round radius=\"6\"/>"
        "<xt-pucker amount=\"-40\"/>"
        "<xt-zigzag size=\"3\" ridges=\"7\" points=\"smooth\"/>"
        "<xt-twist angle=\"45\" center=\"10 20\"/>"
        "<xt-wiggle size=\"2\" detail=\"4\" seed=\"9\" speed=\"1.5\" correlation=\"25%\" "
        "temporal-phase=\"10\" "
        "spatial-phase=\"20\" points=\"smooth\" enabled=\"0\"/>"
        "</g>");
    const ParsedSvg& p = r.p;
    REQUIRE(p.status == Status::kOk);
    CHECK(p.sink.warningCount() == 0);
    REQUIRE(p.countKind(NodeKind::Modifier) == 7);

    const IrModifierData& trim = modifierAt(p, 0);
    CHECK(trim.kind == static_cast<uint8_t>(ModifierKind::Trim));
    CHECK(trim.mode == 1);
    CHECK(trim.enabled == 1);
    CHECK(trim.params[0] == doctest::Approx(0.1f));
    CHECK(trim.params[1] == doctest::Approx(0.8f));
    CHECK(trim.params[2] == doctest::Approx(90.0f));

    const IrModifierData& rep = modifierAt(p, 1);
    CHECK(rep.kind == static_cast<uint8_t>(ModifierKind::Repeat));
    const float repParams[8] = {5, 1, 2, 3, 4, 5, 30, 0.9f};
    for (int i = 0; i < 8; ++i) {
        CHECK(rep.params[i] == doctest::Approx(repParams[i]));
    }
    CHECK(rep.opacityA == doctest::Approx(1.0f));
    CHECK(rep.opacityB == doctest::Approx(0.2f));
    CHECK(rep.mode == 1);  // order="above".

    CHECK(modifierAt(p, 2).kind == static_cast<uint8_t>(ModifierKind::Round));
    CHECK(modifierAt(p, 2).params[0] == doctest::Approx(6.0f));
    CHECK(modifierAt(p, 3).params[0] == doctest::Approx(-40.0f));
    const IrModifierData& zz = modifierAt(p, 4);
    CHECK(zz.params[0] == doctest::Approx(3.0f));
    CHECK(zz.params[1] == doctest::Approx(7.0f));
    CHECK(zz.mode == 1);
    const IrModifierData& tw = modifierAt(p, 5);
    CHECK(tw.params[0] == doctest::Approx(45.0f));
    CHECK(tw.params[1] == doctest::Approx(10.0f));
    CHECK(tw.params[2] == doctest::Approx(20.0f));
    const IrModifierData& wg = modifierAt(p, 6);
    const float wgParams[7] = {2, 4, 9, 1.5f, 0.25f, 10, 20};
    for (int i = 0; i < 7; ++i) {
        CHECK(wg.params[i] == doctest::Approx(wgParams[i]));
    }
    CHECK(wg.mode == 1);
    CHECK(wg.enabled == 0);

    // Modifier nodes: svgTag Modifier, identity transform, children of the <g>; ids recorded.
    const size_t first = findKind(p, NodeKind::Modifier);
    REQUIRE(first != SIZE_MAX);
    const IrSceneNode& mn = p.scene.nodes[first];
    CHECK(mn.svgTag == static_cast<uint8_t>(IrSvgTag::Modifier));
    CHECK(p.scene.nodes[mn.parent].kind == static_cast<uint8_t>(NodeKind::Group));
    CHECK(mn.opacity == doctest::Approx(1.0f));
    REQUIRE(r.ids.size() == 2);
    CHECK(r.ids[0].first == "T");
    CHECK(r.ids[0].second == first);
    CHECK(r.ids[1].first == "Rp");
}

TEST_CASE("svg: T-225b modifier attribute aliases and defaults (31.6.1 element spellings)") {
    ParsedSvg p = parseBody(
        "<g><path d=\"M0 0 L1 0\"/><path d=\"M0 1 L1 1\"/>"  // Two paths: stays a modifier (T-236).
        "<xt-trim start=\"0.2\" end=\"0.6\" offset=\"45\"/>"
        "<xt-repeat copies=\"3\" offset=\"0.5\" anchor=\"1 2\" translate=\"7\" rotate=\"10\" scale=\"50%\"/>"
        "<xt-wiggle size=\"1\"/>"
        "</g>");
    REQUIRE(p.status == Status::kOk);
    CHECK(p.sink.warningCount() == 0);
    const IrModifierData& trim = modifierAt(p, 0);
    CHECK(trim.params[0] == doctest::Approx(0.2f));
    CHECK(trim.params[1] == doctest::Approx(0.6f));
    CHECK(trim.params[2] == doctest::Approx(45.0f));
    CHECK(trim.mode == 0);
    const IrModifierData& rep = modifierAt(p, 1);
    CHECK(rep.params[1] == doctest::Approx(0.5f));
    CHECK(rep.params[2] == doctest::Approx(1.0f));
    CHECK(rep.params[3] == doctest::Approx(2.0f));
    CHECK(rep.params[4] == doctest::Approx(7.0f));  // One number = both components.
    CHECK(rep.params[5] == doctest::Approx(7.0f));
    CHECK(rep.params[6] == doctest::Approx(10.0f));
    CHECK(rep.params[7] == doctest::Approx(0.5f));
    CHECK(rep.opacityA == doctest::Approx(1.0f));
    CHECK(rep.opacityB == doctest::Approx(1.0f));
    CHECK(rep.mode == 0);  // order defaults to below.
    const IrModifierData& wg = modifierAt(p, 2);
    CHECK(wg.params[1] == doctest::Approx(3.0f));  // detail
    CHECK(wg.params[3] == doctest::Approx(2.0f));  // speed
    CHECK(wg.params[4] == doctest::Approx(0.5f));  // correlation
}

TEST_CASE("svg: T-225b offset / merge: baked ones are silent, unbaked ones warn; both disabled") {
    ParsedSvg baked = parseBody(
        "<g><path d=\"M0 0 L1 0\"/><xt-offset amount=\"4\" join=\"round\" miter=\"2\" baked=\"1\"/>"
        "<xt-merge mode=\"subtract\" baked=\"1\"/></g>");
    REQUIRE(baked.status == Status::kOk);
    CHECK(baked.sink.warningCount() == 0);
    const IrModifierData& off = modifierAt(baked, 0);
    CHECK(off.kind == static_cast<uint8_t>(ModifierKind::Offset));
    CHECK(off.enabled == 0);
    CHECK(off.params[0] == doctest::Approx(4.0f));
    CHECK(off.params[1] == doctest::Approx(2.0f));
    CHECK(off.mode == 1);
    const IrModifierData& merge = modifierAt(baked, 1);
    CHECK(merge.kind == static_cast<uint8_t>(ModifierKind::Merge));
    CHECK(merge.mode == 2);
    CHECK(merge.enabled == 0);

    ParsedSvg unbaked = parseBody("<g><path d=\"M0 0 L1 0\"/><xt-offset amount=\"4\"/><xt-merge/></g>");
    REQUIRE(unbaked.status == Status::kOk);
    CHECK(unbaked.sink.warningCount() == 2);
    CHECK(warned(unbaked, "vector.unbakedModifier"));
    CHECK(modifierAt(unbaked, 0).enabled == 0);
    CHECK(modifierAt(unbaked, 1).enabled == 0);
}

TEST_CASE("svg: T-225b a modifier missing a required parameter warns, bad keywords warn") {
    ParsedSvg p = parseBody(
        "<g><path d=\"M0 0 L1 0\"/><xt-round/><xt-repeat/><xt-zigzag size=\"2\"/><xt-pucker/><xt-twist/>"
        "<xt-wiggle/><xt-trim mode=\"sideways\"/><xt-repeat copies=\"2\" scale=\"1 2\"/></g>");
    REQUIRE(p.status == Status::kOk);
    CHECK(p.countKind(NodeKind::Modifier) == 8);  // Still emitted, with defaults.
    CHECK(warned(p, "<xt-round> needs radius"));
    CHECK(warned(p, "<xt-repeat> needs copies"));
    CHECK(warned(p, "<xt-zigzag> needs ridges"));
    CHECK(warned(p, "<xt-pucker> needs amount"));
    CHECK(warned(p, "<xt-twist> needs angle"));
    CHECK(warned(p, "<xt-wiggle> needs size"));
    CHECK(warned(p, "unknown mode"));
    CHECK(warned(p, "non-uniform scale"));
    CHECK(p.sink.warningCount() == 8);
    CHECK(modifierAt(p, 1).params[0] == doctest::Approx(1.0f));  // copies default.
    CHECK(modifierAt(p, 1).params[7] == doctest::Approx(1.0f));  // scale default.
}

TEST_CASE("svg: T-225b clip-path wraps the element in a Group whose first child is an Intersect Mask") {
    ParsedSvgIds r = parseBodyIds(
        "<defs><clipPath id=\"c\" clip-rule=\"evenodd\"><circle id=\"inner\" cx=\"0\" cy=\"0\" r=\"10\" "
        "fill=\"#ff0000\" stroke=\"#00ff00\" opacity=\"0.3\"/></clipPath></defs>"
        "<rect id=\"R\" x=\"-20\" y=\"-20\" width=\"40\" height=\"40\" fill=\"#0000ff\" "
        "transform=\"translate(50 50)\" "
        "clip-path=\"url(#c)\"/>");
    const ParsedSvg& p = r.p;
    REQUIRE(p.status == Status::kOk);
    CHECK(p.sink.warningCount() == 0);
    // root, wrapper Group, Mask, clip circle (Shape), the rect (Shape).
    REQUIRE(p.scene.nodes.size() == 5);
    const IrSceneNode& wrapper = p.scene.nodes[1];
    const IrSceneNode& mask = p.scene.nodes[2];
    const IrSceneNode& clipShape = p.scene.nodes[3];
    const IrSceneNode& rect = p.scene.nodes[4];
    CHECK(wrapper.kind == static_cast<uint8_t>(NodeKind::Group));
    CHECK(wrapper.parent == 0);
    CHECK(wrapper.translate.x == doctest::Approx(0.0f));
    CHECK(mask.kind == static_cast<uint8_t>(NodeKind::Mask));
    CHECK(mask.maskMode == static_cast<uint8_t>(MaskMode::Intersect));
    CHECK(mask.parent == 1);
    CHECK(mask.translate.x == doctest::Approx(50.0f));  // The referencing element's user space.
    CHECK(mask.translate.y == doctest::Approx(50.0f));
    CHECK(clipShape.parent == 2);
    CHECK(clipShape.opacity == doctest::Approx(1.0f));
    const IrShapeData& cs = p.scene.shapes[clipShape.kindIndex];
    CHECK(cs.fillKind == static_cast<uint8_t>(PaintKind::Solid));
    CHECK(cs.fillColor == Color{0, 0, 0, 1});
    CHECK(cs.strokeKind == static_cast<uint8_t>(PaintKind::None));
    CHECK(cs.fillRule == static_cast<uint8_t>(FillRule::EvenOdd));
    CHECK(rect.kind == static_cast<uint8_t>(NodeKind::Shape));
    CHECK(rect.parent == 1);
    CHECK(rect.translate.x == doctest::Approx(50.0f));
    // T-230: the clip content's first copy is addressable too (an animated Lottie mask path).
    REQUIRE(r.ids.size() == 2);
    CHECK(r.ids[0].first == "inner");
    CHECK(r.ids[0].second == 3);
    CHECK(r.ids[1].first == "R");
    CHECK(r.ids[1].second == 4);
}

TEST_CASE("svg: T-225b mask is Luma (Alpha with mask-type=alpha); clip + mask = two layers") {
    ParsedSvg p = parseBody(
        "<clipPath id=\"c\" transform=\"translate(5 0)\"><rect x=\"0\" y=\"0\" width=\"10\" "
        "height=\"10\"/></clipPath>"
        "<mask id=\"m\"><rect x=\"0\" y=\"0\" width=\"10\" height=\"10\" fill=\"#ffffff\" "
        "opacity=\"0.5\"/></mask>"
        "<mask id=\"a\" style=\"mask-type: alpha\"><circle cx=\"0\" cy=\"0\" r=\"3\"/></mask>"
        "<path d=\"M0 0 L20 0 L20 20 Z\" clip-path=\"url(#c)\" mask=\"url(#m)\"/>"
        "<g mask=\"url(#a)\"><path d=\"M0 0 L1 1\"/></g>");
    REQUIRE(p.status == Status::kOk);
    CHECK(p.sink.warningCount() == 0);
    // <clipPath>/<mask> outside <defs> draw nothing themselves:
    // root, G(clip), Mask(Intersect), rect, G(mask), Mask(Luma), rect, path,
    // G(mask a), Mask(Alpha), circle, g, path.
    REQUIRE(p.scene.nodes.size() == 13);
    const auto& n = p.scene.nodes;
    CHECK(n[1].kind == static_cast<uint8_t>(NodeKind::Group));
    CHECK(n[2].kind == static_cast<uint8_t>(NodeKind::Mask));
    CHECK(n[2].maskMode == static_cast<uint8_t>(MaskMode::Intersect));
    CHECK(n[2].translate.x == doctest::Approx(5.0f));  // The clipPath's own transform.
    CHECK(n[4].kind == static_cast<uint8_t>(NodeKind::Group));
    CHECK(n[4].parent == 1);
    CHECK(n[5].kind == static_cast<uint8_t>(NodeKind::Mask));
    CHECK(n[5].maskMode == static_cast<uint8_t>(MaskMode::Luma));
    CHECK(n[5].parent == 4);
    CHECK(n[6].opacity == doctest::Approx(0.5f));  // Mask content keeps its own paint/opacity.
    CHECK(n[7].kind == static_cast<uint8_t>(NodeKind::Path));
    CHECK(n[7].parent == 4);
    CHECK(n[9].kind == static_cast<uint8_t>(NodeKind::Mask));
    CHECK(n[9].maskMode == static_cast<uint8_t>(MaskMode::Alpha));
    CHECK(n[11].kind == static_cast<uint8_t>(NodeKind::Group));
    CHECK(n[11].parent == 8);
}

TEST_CASE("svg: T-225b two references to one clipPath get their own copies; bad references warn") {
    ParsedSvg p = parseBody(
        "<defs><clipPath id=\"c\"><circle cx=\"0\" cy=\"0\" r=\"4\"/></clipPath>"
        "<clipPath id=\"obb\" clipPathUnits=\"objectBoundingBox\"><rect width=\"1\" height=\"1\"/></clipPath>"
        "<linearGradient id=\"g\"><stop offset=\"0\" stop-color=\"#fff\"/></linearGradient></defs>"
        "<path d=\"M0 0 L1 1\" clip-path=\"url(#c)\"/>"
        "<path d=\"M0 0 L2 2\" clip-path=\"url(#c)\"/>"
        "<path d=\"M0 0 L3 3\" clip-path=\"url(#missing)\"/>"
        "<path d=\"M0 0 L4 4\" clip-path=\"url(#g)\"/>"
        "<path d=\"M0 0 L5 5\" mask=\"#c\"/>"
        "<path d=\"M0 0 L6 6\" clip-path=\"url(#obb)\"/>");
    REQUIRE(p.status == Status::kOk);
    CHECK(p.countKind(NodeKind::Mask) == 3);
    CHECK(p.countKind(NodeKind::Path) == 6);
    CHECK(p.countKind(NodeKind::Shape) == 3);  // Two circle copies, one obb rect.
    CHECK(warned(p, "\"#missing\" is not a <clipPath>"));
    CHECK(warned(p, "\"#g\" is not a <clipPath>"));
    CHECK(warned(p, "not a local url(#id)"));
    CHECK(warned(p, "clipPathUnits=\"objectBoundingBox\" is not supported"));
    CHECK(p.sink.warningCount() == 4);
}

TEST_CASE("svg: T-225b clip-path inside clip content is ignored with a warning") {
    ParsedSvg p = parseBody(
        "<clipPath id=\"a\"><rect width=\"5\" height=\"5\" clip-path=\"url(#a)\"/></clipPath>"
        "<path d=\"M0 0 L1 1\" clip-path=\"url(#a)\"/>");
    REQUIRE(p.status == Status::kOk);
    CHECK(p.countKind(NodeKind::Mask) == 1);
    CHECK(warned(p, "inside <clipPath>/<mask> content is not supported"));
    CHECK(p.sink.warningCount() == 1);
}

TEST_CASE("compileDocument: T-225b a modifier missing a parameter warns; --strict turns it into a failure") {
    const std::string xt = R"(<xt><body>
      <svg id="Icon" viewBox="0 0 32 32" width="32" height="32">
        <g><path d="M0 0 L10 0" stroke="#000000"/><xt-round/></g>
      </svg>
    </body></xt>)";
    std::vector<SourceFile> files{{"main.xt", xt}};
    CompileOptions options;
    options.emitMeta = false;
    CompileResult loose = compileDocumentFromStrings(files, "main.xt", options);
    CHECK(loose.ok());
    bool warnedRadius = false;
    for (const Diagnostic& d : loose.diagnostics) {
        warnedRadius = warnedRadius || d.message.find("<xt-round> needs radius") != std::string::npos;
    }
    CHECK(warnedRadius);

    CompileOptions strict = options;
    strict.strict = true;
    CHECK_FALSE(compileDocumentFromStrings(files, "main.xt", strict).ok());
}

// T-236 (31 §31.6.1): an enabled parallel <xt-trim> over exactly one path, with no modifier before
// it, becomes that path's own trim (offset degrees -> turns); its id names the path.
TEST_CASE("svg: T-236 a single-path parallel <xt-trim> is written into the path's trim") {
    ParsedSvgIds r = parseBodyIds(
        "<g><path id=\"P\" d=\"M0 0 L10 0\"/>"
        "<xt-trim id=\"Trim1\" trim-start=\"0.1\" trim-end=\"0.6\" trim-offset=\"90\"/>"
        "<xt-repeat id=\"Rp\" copies=\"2\"/></g>"
        "<g><rect id=\"S\" x=\"0\" y=\"0\" width=\"4\" height=\"4\"/><xt-trim trim-end=\"0.5\"/></g>");
    ParsedSvg& p = r.p;
    REQUIRE(p.status == Status::kOk);
    CHECK(p.countKind(NodeKind::Modifier) == 1);  // Only the repeat.
    CHECK(modifierAt(p, 0).kind == static_cast<uint8_t>(ModifierKind::Repeat));
    const IrPathData& path = p.path(0);
    CHECK(path.trimStart == doctest::Approx(0.1f));
    CHECK(path.trimEnd == doctest::Approx(0.6f));
    CHECK(path.trimOffset == doctest::Approx(0.25f));
    CHECK(p.path(1).trimEnd == doctest::Approx(0.5f));  // The Shape too.
    const size_t pathNode = findKind(p, NodeKind::Path);
    bool trimNamesPath = false;
    for (const auto& [id, index] : r.ids) {
        trimNamesPath = trimNamesPath || (id == "Trim1" && index == pathNode);
    }
    CHECK(trimNamesPath);
}

TEST_CASE("svg: T-236 <xt-trim> stays a modifier: two paths, serial, an earlier modifier, disabled") {
    ParsedSvg two =
        parseBody("<g><path d=\"M0 0 L1 0\"/><path d=\"M0 1 L1 1\"/><xt-trim trim-end=\"0.5\"/></g>");
    REQUIRE(two.status == Status::kOk);
    CHECK(two.countKind(NodeKind::Modifier) == 1);
    CHECK(two.path(0).trimEnd == doctest::Approx(1.0f));

    ParsedSvg serial = parseBody("<g><path d=\"M0 0 L1 0\"/><xt-trim trim-end=\"0.5\" mode=\"serial\"/></g>");
    REQUIRE(serial.status == Status::kOk);
    CHECK(serial.countKind(NodeKind::Modifier) == 1);

    ParsedSvg after =
        parseBody("<g><path d=\"M0 0 L1 0\"/><xt-round radius=\"2\"/><xt-trim trim-end=\"0.5\"/></g>");
    REQUIRE(after.status == Status::kOk);
    CHECK(after.countKind(NodeKind::Modifier) == 2);

    ParsedSvg own =
        parseBody("<g><path d=\"M0 0 L1 0\" path-trim=\"0 0.8\"/><xt-trim trim-end=\"0.5\"/></g>");
    REQUIRE(own.status == Status::kOk);
    CHECK(own.countKind(NodeKind::Modifier) == 1);  // The path already trims itself.

    ParsedSvg disabled = parseBody("<g><path d=\"M0 0 L1 0\"/><xt-trim trim-end=\"0.5\" enabled=\"0\"/></g>");
    REQUIRE(disabled.status == Status::kOk);
    CHECK(disabled.path(0).trimEnd == doctest::Approx(1.0f));
}

TEST_CASE("svg: T-233 xt-taper and xt-width-profile on a path") {
    ParsedSvg p = parseBody(
        "<path d=\"M0 0 L100 0\" stroke=\"#000\" stroke-width=\"4\" xt-taper=\"0.2 0.3\" "
        "xt-width-profile=\"1:1, 0:1 0.5:2\"/>");
    REQUIRE(p.status == Status::kOk);
    REQUIRE(p.scene.paths.size() == 1);
    const IrPathData& d = p.scene.paths[0];
    CHECK(d.taperStart == doctest::Approx(0.2f));
    CHECK(d.taperEnd == doctest::Approx(0.3f));
    REQUIRE(d.profileCount == 3);
    REQUIRE(d.profileOffset + 6 <= p.scene.floatPool.size());
    // Sorted by t: (0, 1), (0.5, 2), (1, 1).
    const float* f = p.scene.floatPool.data() + d.profileOffset;
    CHECK(f[0] == doctest::Approx(0.0f));
    CHECK(f[1] == doctest::Approx(1.0f));
    CHECK(f[2] == doctest::Approx(0.5f));
    CHECK(f[3] == doctest::Approx(2.0f));
    CHECK(f[4] == doctest::Approx(1.0f));
    CHECK(f[5] == doctest::Approx(1.0f));
    CHECK(p.sink.warningCount() == 0);
}

TEST_CASE("svg: T-233 xt-taper on a closed path or shape warns; malformed values warn and are ignored") {
    ParsedSvg closed = parseBody(
        "<path d=\"M0 0 L10 0 L10 10 Z\" stroke=\"#000\" xt-taper=\"0.5 0.5\"/>"
        "<circle cx=\"0\" cy=\"0\" r=\"5\" stroke=\"#000\" xt-taper=\"0.1 0\"/>");
    REQUIRE(closed.status == Status::kOk);
    CHECK(closed.sink.warningCount() == 2);
    CHECK(closed.scene.paths[0].taperStart == doctest::Approx(0.5f));  // Kept; the Painter ignores it.
    CHECK(closed.scene.shapes[0].taperStart == doctest::Approx(0.1f));

    ParsedSvg bad =
        parseBody("<path d=\"M0 0 L10 0\" stroke=\"#000\" xt-taper=\"0.5\" xt-width-profile=\"0:1 0.5\"/>");
    REQUIRE(bad.status == Status::kOk);
    CHECK(bad.sink.warningCount() == 2);
    CHECK(bad.scene.paths[0].taperStart == doctest::Approx(0.0f));
    CHECK(bad.scene.paths[0].profileCount == 0);
}

// --- T-258b (Docs/reviews/2026-10-02-full-audit-raw/D.md D3, D10): numbers from the file are bounded ---

namespace {

bool erred(const ParsedSvg& p, std::string_view needle) {
    for (const Diagnostic& d : p.sink.all()) {
        if (d.level == Diagnostic::Level::Error && d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("svg: T-258b D3 <xt-star points> above 1024 is clamped to 1024 with a warning") {
    for (const char* points : {"5000", "1e9", "1.2e9", "1e30"}) {
        CAPTURE(points);
        ParsedSvg p = parseBody(std::string("<xt-star cx=\"50\" cy=\"50\" points=\"") + points +
                                "\" r=\"10\" r2=\"5\"/>");
        REQUIRE(p.status == Status::kOk);
        REQUIRE(p.scene.shapes.size() == 1);
        CHECK(p.scene.shapes[0].params[2] == doctest::Approx(1024.0f));
        CHECK(warned(p, "points clamped to 1024"));
        CHECK(p.path(0).pointCount <= 2 * 2048);  // At most 2048 vertices.
    }
}

TEST_CASE("svg: T-258b D3 a non-finite <xt-star points> is an error and draws nothing") {
    ParsedSvg p = parseBody("<xt-star cx=\"50\" cy=\"50\" points=\"nan\" r=\"10\" r2=\"5\"/>");
    CHECK(p.scene.shapes.empty());
    CHECK(erred(p, "points"));
}

TEST_CASE("svg: T-258b D10 opacity=\"nan\" is not a number: the node keeps opacity 1") {
    ParsedSvg p = parseBody("<g opacity=\"nan\"><rect width=\"10\" height=\"10\" fill-opacity=\"inf\"/></g>");
    REQUIRE(p.status == Status::kOk);
    for (const IrSceneNode& n : p.scene.nodes) {
        CHECK(std::isfinite(n.opacity));
        CHECK(n.opacity == doctest::Approx(1.0f));
    }
    REQUIRE(p.scene.shapes.size() == 1);
    CHECK(p.scene.shapes[0].fillColor.a == doctest::Approx(1.0f));
}

TEST_CASE("svg: T-258b D10 viewBox=\"0 0 inf inf\" is malformed: the default size, finite") {
    ParsedSvg p = parseText("<svg viewBox=\"0 0 inf inf\"><rect width=\"10\" height=\"10\"/></svg>");
    REQUIRE(p.status == Status::kOk);
    CHECK(std::isfinite(p.scene.width));
    CHECK(std::isfinite(p.scene.height));
    CHECK(p.scene.width == doctest::Approx(300.0f));
    CHECK(warned(p, "malformed viewBox"));
}

TEST_CASE("svg: T-258b D10 <xt-mesh width=\"inf\"> is not a positive width: no mesh") {
    ParsedSvg p = parseBody("<xt-mesh href=\"face.png\" width=\"inf\" height=\"10\"/>");
    CHECK(p.scene.meshes.empty());
    CHECK(erred(p, "positive width and height"));
}
