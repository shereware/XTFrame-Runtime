#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "mini_css.h"         // core/src (on the test include path).
#include "noise.h"            // core/src
#include "shape_modifiers.h"  // core/src
#include "sw_raster.h"        // sw/src
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/document.h"
#include "xt/geom.h"
#include "xt/ir.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/painter.h"
#include "xt/particles.h"
#include "xt/sw/sw_png.h"
#include "xt/sw/sw_renderer.h"
#include "xt/types.h"

/// T-258a (docs/reviews/2026-10-02-full-audit-raw B2-B5, B16, C9, E6, E11): numbers that come
/// from files, bindings or host calls are bounded before they reach a loop, an array size or a
/// float-to-integer conversion. Every case that used to hang is checked with an upper bound on the
/// work done (emitted particles, points, commands, cells), never by waiting for a hang.

using namespace xt;

namespace {

constexpr float kNanF = std::numeric_limits<float>::quiet_NaN();
constexpr float kInfF = std::numeric_limits<float>::infinity();

// --- documents ---

std::string page(std::string_view body, std::string_view style = "") {
    return "<xt><head><style>" + std::string(style) + "</style></head><body id=\"body\">" +
           std::string(body) + "</body></xt>";
}

struct Loaded {
    compile::CompileResult result;
    std::unique_ptr<Document> doc;
};

Loaded loadXt(const std::string& xt) {
    Loaded out;
    compile::CompileOptions options;
    options.emitMeta = false;
    options.rootDir = XT_TEST_XT_DIR;
    std::vector<compile::SourceFile> files{{"main.xt", xt}};
    out.result = compile::compileDocumentFromStrings(files, "main.xt", options);
    for (const compile::Diagnostic& d : out.result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(out.result.ok());
    REQUIRE(Document::load(IrBlob{out.result.ir.data(), out.result.ir.size()}, false, nullptr, nullptr,
                           out.doc) == Status::kOk);
    out.doc->setViewport(Rect{0, 0, 200, 200});
    return out;
}

// --- paths ---

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
    TPath& close() {
        verbs.push_back(PathVerb::Close);
        return *this;
    }
};

TPath square(float s) { return TPath{}.move(0, 0).line(s, 0).line(s, s).line(0, s).close(); }

// --- border-image ---

NodeId borderImageElement(NodeArena& arena, float w, float h, float border, uint8_t repeat, uint8_t fill) {
    NodeId id = arena.create(NodeKind::Element);
    ElementData& data = arena.element(id);
    data.box.width = w;
    data.box.height = h;
    data.box.border.top = border;
    data.box.border.right = border;
    data.box.border.bottom = border;
    data.box.border.left = border;
    data.style.borderImage = 0;
    data.style.borderImageRepeat = repeat;
    data.style.borderImageFill = fill;
    data.borderImageSize = Vec2{24, 24};
    for (float& s : data.borderImageSlicePx) {
        s = 8;
    }
    data.borderImageSubUv = Rect{0, 0, 1, 1};
    return id;
}

size_t paintedImages(NodeArena& arena, NodeId root) {
    updateWorldTransforms(arena, root, true);
    Painter painter;
    CommandList cmds;
    painter.paint(arena, root, cmds);
    size_t n = 0;
    for (const Command& c : cmds.commands) {
        n += c.kind == CmdKind::DrawImage ? 1 : 0;
    }
    return n;
}

// --- dash ---

/// applyDash over one open 2-point contour of `length` px; returns the output contour count.
size_t dashContours(float length, std::vector<float> dashes, std::vector<Vec2>& outPoints) {
    const std::vector<Vec2> pts{Vec2{0, 0}, Vec2{length, 0}};
    const std::vector<uint32_t> starts{0};
    const std::vector<uint8_t> closed{0};
    std::vector<uint32_t> outStarts;
    std::vector<uint8_t> outClosed;
    outPoints.clear();
    applyDash(pts, starts, closed, dashes, 0.0f, outPoints, outStarts, outClosed);
    return outStarts.size();
}

// --- images ---

/// A PNG made of a signature, an IHDR chunk declaring `w` x `h` 8-bit grayscale, and IEND (no
/// pixel data; stb_image does not check CRCs).
std::vector<uint8_t> pngHeaderOnly(uint32_t w, uint32_t h) {
    std::vector<uint8_t> b{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
    for (uint32_t v : {w, h}) {
        b.push_back(static_cast<uint8_t>(v >> 24));
        b.push_back(static_cast<uint8_t>(v >> 16));
        b.push_back(static_cast<uint8_t>(v >> 8));
        b.push_back(static_cast<uint8_t>(v));
    }
    b.insert(b.end(), {8, 0, 0, 0, 0, 0, 0, 0, 0});  // depth, gray, deflate, filter, no interlace, CRC
    b.insert(b.end(), {0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82});
    return b;
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// B2 particles
// ------------------------------------------------------------------------------------------------

TEST_CASE("T-258a B2: a huge particle rate emits at most maxParticles per step and drops the carry") {
    const Rect box{0, 0, 10, 10};
    ParticleNodeData d;
    d.lifetime[0] = d.lifetime[1] = 100;
    d.maxParticles = 4;
    d.emitting = 1;
    d.rate = 1e6f;
    stepParticles(d, box, kParticleStep);
    CHECK(d.particles.size() == 4);
    CHECK(d.emitCarry < 1.0f);
    {
        // A full emitter keeps the fraction the one-by-one loop used to leave, so an emitter that
        // sits at its cap emits at the same moments as before.
        ParticleNodeData f;
        f.lifetime[0] = f.lifetime[1] = 100;
        f.maxParticles = 2;
        f.emitting = 1;
        f.rate = 5.75f / kParticleStep;  // Owes more than five particles in one step; room for two.
        stepParticles(f, box, kParticleStep);
        float expected = f.rate * kParticleStep;
        REQUIRE(expected >= 3.0f);
        while (expected >= 1.0f) {
            expected -= 1.0f;
        }
        CHECK(f.particles.size() == 2);
        CHECK(f.emitCarry == expected);
    }
    // A carry that `-= 1` no longer changes (1e30, Inf) used to spin forever.
    for (float rate : {1e30f, kInfF}) {
        CAPTURE(rate);
        ParticleNodeData e;
        e.lifetime[0] = e.lifetime[1] = 100;
        e.maxParticles = 16;
        e.emitting = 1;
        e.rate = rate;
        stepParticles(e, box, 0.5f);
        CHECK(e.particles.size() == 16);
        CHECK(e.emitCarry == 0.0f);
    }
}

TEST_CASE("T-258a B2: particle-rate is clamped to 100000; a huge burst is clamped to max") {
    // The .xts lexer has no exponent syntax: 1e6 and 1e30 spelled out.
    Loaded l =
        loadXt(page(R"~(<particles id="P" rate="1000000" burst="1000000000000000000000000000000" max="8"
        lifetime="5" autoplay></particles>)~",
                    "#P { width: 10px; height: 10px; }"));
    Document& d = *l.doc;
    d.update(0);
    const ParticleNodeData* p = d.particleData(d.byId("P").node);
    REQUIRE(p != nullptr);
    CHECK(p->rate == 100000.0f);
    CHECK(p->particles.size() == 8);  // The burst, capped at max.
    d.update(1.0f);
    CHECK(d.particleData(d.byId("P").node)->particles.size() == 8);

    Loaded huge =
        loadXt(page(R"~(<particles id="P" rate="1000000000000000000000000000000" max="8" lifetime="5"
        autoplay></particles>)~",
                    "#P { width: 10px; height: 10px; }"));
    huge.doc->update(0);
    huge.doc->update(0.5f);  // Returns: used to hang in stepOnce.
    const ParticleNodeData* q = huge.doc->particleData(huge.doc->byId("P").node);
    REQUIRE(q != nullptr);
    CHECK(q->rate == 100000.0f);
    CHECK(q->particles.size() == 8);
}

// ------------------------------------------------------------------------------------------------
// B3 modifiers and shapes
// ------------------------------------------------------------------------------------------------

TEST_CASE("T-258a B3: star / polygon points are clamped to 1024; non-finite draws nothing") {
    for (float points : {5000.0f, 1e9f, 2e9f}) {
        CAPTURE(points);
        const float star[8] = {0, 0, points, 10, 5, 0, 0, 0};
        std::vector<PathVerb> v;
        std::vector<float> p;
        modifiers::buildShape(ShapeKind::Star, star, v, p);
        CHECK(modifiers::vertexCount(v) == 2 * modifiers::kMaxGeneratedPoints);
        const float polygon[8] = {0, 0, points, 10, 0, 0, 0, 0};
        v.clear();
        p.clear();
        modifiers::buildShape(ShapeKind::Star, polygon, v, p);
        CHECK(modifiers::vertexCount(v) == modifiers::kMaxGeneratedPoints);
    }
    for (float points : {kNanF, kInfF, -kInfF, -1e30f}) {
        const float star[8] = {0, 0, points, 10, 5, 0, 0, 0};
        std::vector<PathVerb> v;
        std::vector<float> p;
        modifiers::buildShape(ShapeKind::Star, star, v, p);
        CHECK(v.empty());
        Rect bounds{};
        CHECK_FALSE(modifiers::shapeBounds(ShapeKind::Star, star, bounds));
    }
}

TEST_CASE("T-258a B3: zigzag ridges are clamped to 1024 per segment; non-finite leaves the path") {
    const TPath line = TPath{}.move(0, 0).line(30, 0);
    std::vector<PathVerb> v;
    std::vector<float> p;
    for (float ridges : {5000.0f, 1e9f}) {
        CAPTURE(ridges);
        v.clear();
        p.clear();
        modifiers::zigzag(line.verbs, line.points, 2, ridges, false, v, p);
        CHECK(modifiers::vertexCount(v) == modifiers::kMaxGeneratedPoints + 2);
    }
    for (float ridges : {kNanF, kInfF}) {
        v.clear();
        p.clear();
        modifiers::zigzag(line.verbs, line.points, 2, ridges, false, v, p);
        CHECK(v == line.verbs);
        CHECK(p == line.points);
    }
}

TEST_CASE("T-258a B3: wiggle detail is clamped to 1024 per segment; non-finite leaves the path") {
    const TPath sq = square(10);
    modifiers::WiggleParams wp;
    wp.size = 3;
    wp.seed = 7;
    wp.speed = 1;
    std::vector<PathVerb> v;
    std::vector<float> p;
    uint32_t ordinal = 0;
    for (float detail : {5000.0f, 1e9f}) {
        CAPTURE(detail);
        v.clear();
        p.clear();
        wp.detail = detail;
        modifiers::wiggle(sq.verbs, sq.points, wp, 0.5f, ordinal, v, p);
        CHECK(modifiers::vertexCount(v) == 4 * (modifiers::kMaxGeneratedPoints + 1));
    }
    for (float detail : {kNanF, kInfF}) {
        v.clear();
        p.clear();
        ordinal = 0;
        wp.detail = detail;
        modifiers::wiggle(sq.verbs, sq.points, wp, 0.5f, ordinal, v, p);
        CHECK(v == sq.verbs);
        CHECK(p == sq.points);
    }
}

TEST_CASE("T-258a B3: twist angle is clamped to 36000 degrees, pieces to 1024; non-finite leaves the path") {
    const TPath sq = square(10);
    std::vector<PathVerb> v;
    std::vector<float> p;
    for (float angle : {1e6f, 1e9f, -1e10f}) {
        CAPTURE(angle);
        v.clear();
        p.clear();
        modifiers::twist(sq.verbs, sq.points, angle, Vec2{5, 5}, v, p);
        CHECK(modifiers::vertexCount(v) == 4 * modifiers::kMaxGeneratedPoints + 1);  // M + the cubics.
    }
    for (float angle : {kNanF, kInfF, -kInfF}) {
        v.clear();
        p.clear();
        modifiers::twist(sq.verbs, sq.points, angle, Vec2{5, 5}, v, p);
        CHECK(v == sq.verbs);
        CHECK(p == sq.points);
    }
}

// ------------------------------------------------------------------------------------------------
// B4 border-image
// ------------------------------------------------------------------------------------------------

TEST_CASE(
    "T-258a B4: border-image tiles under 1 px, over 1024 on an axis or 65536 in a region, draw stretched") {
    // A 0.01 px border: the 8 px slice tiles at 0.01 px. Every region falls back to one stretched
    // tile, so the nine-slice is 8 commands (no fill).
    NodeArena arena;
    NodeId root = borderImageElement(arena, 100, 60, 0.01f, 1, 0);
    CHECK(paintedImages(arena, root) == 8);
    NodeArena arena2;
    NodeId root2 = borderImageElement(arena2, 100, 60, 0.01f, 2, 0);  // round
    CHECK(paintedImages(arena2, root2) == 8);
    // A 1 px border on a 3000 px element: 1 px tiles, but about 3000 of them per edge.
    NodeArena arena3;
    NodeId root3 = borderImageElement(arena3, 3000, 40, 1.0f, 1, 0);
    const size_t wide = paintedImages(arena3, root3);
    CHECK(wide <= 8 + 2 * 40);  // Top / bottom stretched; left / right edges 38 tiles each.
    // A 0.0001 px border with fill: used to emit about 1e6 tiles per edge and 1e12 in the middle
    // (or loop forever once `x += tile` stopped advancing).
    for (uint8_t mode : {uint8_t{1}, uint8_t{2}}) {
        NodeArena a;
        NodeId r = borderImageElement(a, 300, 300, 0.0001f, mode, 1);
        CHECK(paintedImages(a, r) == 9);
    }
    // 1 px tiles on a 300 px element with fill: each edge is 298 tiles (tiled), but the middle
    // would be 298 x 298 = 88804 > 65536 and is stretched: 4 corners + 4 x 298 + 1.
    NodeArena arena4;
    NodeId root4 = borderImageElement(arena4, 300, 300, 1.0f, 2, 1);
    CHECK(paintedImages(arena4, root4) == 4 + 4 * 298 + 1);
}

TEST_CASE("T-258a B4: a 40 x 40 tile fill middle is still tiled") {
    // 8 px border, 8 px slices: scale 1, 8 px middle tiles; `round` over 320 px is exactly 40 per
    // axis. 4 corners + 4 edges x 40 + the 1600-tile middle.
    NodeArena arena;
    NodeId root = borderImageElement(arena, 336, 336, 8.0f, 2, 1);
    CHECK(paintedImages(arena, root) == 4 + 4 * 40 + 40 * 40);
}

// ------------------------------------------------------------------------------------------------
// B5 dash
// ------------------------------------------------------------------------------------------------

TEST_CASE("T-258a B5: a dash pattern with more than 100000 periods on a contour strokes it solid") {
    std::vector<Vec2> out;
    CHECK(dashContours(1000, {0.001f}, out) == 1);  // 500000 periods: solid.
    REQUIRE(out.size() == 2);
    CHECK(out[1].x == 1000.0f);
    CHECK(dashContours(1000, {1e-6f}, out) == 1);     // Used to run ~2e9 iterations.
    CHECK(dashContours(1000, {1e-40f}, out) == 1);    // Denormal: totalLen / total was Inf.
    CHECK(dashContours(1000, {kInfF, 1}, out) == 1);  // Non-finite total: solid.
    CHECK(dashContours(1000, {10, 10}, out) >= 50);   // A normal pattern still dashes.
}

// ------------------------------------------------------------------------------------------------
// B16 / C9 float -> int
// ------------------------------------------------------------------------------------------------

TEST_CASE("T-258a C9: an entity port rejects negative and non-finite numbers; text saturates") {
    Loaded l = loadXt(page(R"~(<div id="Hud" port:who="entity" port:mode="enum(Normal|Warn)"></div>)~"));
    Document& d = *l.doc;
    d.update(0);
    const PortId who = d.lookupPort("who");
    REQUIRE(who != kInvalidPort);
    CHECK(d.setPort(who, Value::ofFloat(-1)) == Status::kInvalidArgument);
    CHECK(d.setPort(who, Value::ofFloat(kNanF)) == Status::kInvalidArgument);
    CHECK(d.setPort(who, Value::ofFloat(kInfF)) == Status::kInvalidArgument);
    CHECK(d.setPort(who, Value::ofFloat(5.7f)) == Status::kOk);
    CHECK(d.getPort(who).e == 5u);
    CHECK(d.setPortText(who, "inf") == Status::kInvalidArgument);
    CHECK(d.setPortText(who, "1e20") == Status::kOk);
    CHECK(d.getPort(who).e == UINT32_MAX);
    const PortId mode = d.lookupPort("mode");
    REQUIRE(mode != kInvalidPort);
    CHECK(d.setPortText(mode, "1e20") == Status::kInvalidArgument);  // Saturates, then out of range.
}

TEST_CASE("T-258a C9: runtime CSS steps() and iteration counts saturate instead of wrapping") {
    std::vector<mini_css::Decl> decls;
    REQUIRE(mini_css::parseDeclarations("animation: a 1s steps(1000000) 70000", decls) == Status::kOk);
    REQUIRE(decls.size() == 1);
    REQUIRE(decls[0].animations.size() == 1);
    CHECK(decls[0].animations[0].spec.easing.stepCount == 65535);
    CHECK(decls[0].animations[0].spec.iterations == 65535);
    decls.clear();
    REQUIRE(mini_css::parseDeclarations("animation: a 1s steps(0.5) 2", decls) == Status::kOk);
    REQUIRE(decls.size() == 1);
    CHECK(decls[0].animations[0].spec.easing.stepCount == 1);  // steps(n) is at least 1.
    CHECK(decls[0].animations[0].spec.iterations == 2);
}

TEST_CASE("T-258a C9: a sequence's frames attribute saturates") {
    Loaded l = loadXt(
        page(R"~(<sequence id="A" src="media/atlas.png" frame-size="8x4" frames="1e20"></sequence>)~"));
    const ExternalNodeData* x = l.doc->externalData(l.doc->byId("A").node);
    REQUIRE(x != nullptr);
    CHECK(x->atlasFrames == UINT32_MAX);
}

TEST_CASE("T-258a B16: repeat copies, noise and the wiggle seed take non-finite values safely") {
    ModifierNodeData repeat{};
    repeat.kind = ModifierKind::Repeat;
    repeat.params[7] = 1;
    repeat.opacityA = repeat.opacityB = 1;
    std::vector<modifiers::RepeatCopy> copies;
    repeat.params[0] = kNanF;
    modifiers::repeatCopies(repeat, copies);
    CHECK(copies.empty());
    repeat.params[0] = 1e9f;
    modifiers::repeatCopies(repeat, copies);
    CHECK(copies.size() == modifiers::kMaxRepeatCopies);
    // Beyond +-2^31 the lattice coordinate saturates: finite, and the same value further out.
    CHECK(std::isfinite(valueNoise2(3e9f, 0.25f, 7)));
    CHECK(valueNoise2(3e9f, 0.25f, 7) == valueNoise2(5e9f, 0.25f, 7));
}

// ------------------------------------------------------------------------------------------------
// E6 SW rasterizer
// ------------------------------------------------------------------------------------------------

TEST_CASE("T-258a E6: the rasterizer splits columns only inside [-1, width]") {
    sw::Rasterizer r;
    r.reset(16, 4);
    r.addLine(Vec2{-100000, 0.25f}, Vec2{100000, 0.75f});
    r.addLine(Vec2{100000, 0.75f}, Vec2{-100000, 0.25f});
    CHECK(r.rowCapacity(0) <= 64);
    CHECK(r.debugRowCoverSum(0) == 0);
    // Non-finite endpoints are dropped (a NaN used to loop about 2^64 times); 1e30 stays bounded.
    sw::Rasterizer n;
    n.reset(16, 4);
    n.addLine(Vec2{kNanF, 0.0f}, Vec2{5.0f, 3.0f});
    n.addLine(Vec2{1.0f, kInfF}, Vec2{5.0f, 3.0f});
    CHECK(n.minRow() > n.maxRow());
    n.addLine(Vec2{-1e30f, 0.5f}, Vec2{1e30f, 3.5f});
    for (int row = 0; row < 4; ++row) {
        CHECK(n.rowCapacity(row) <= 64);
    }
}

TEST_CASE("T-258a E6: SwRenderer renders paths with NaN and 1e30 coordinates") {
    const int w = 32, h = 32;
    std::vector<uint8_t> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    sw::SwRenderer renderer;
    renderer.setTarget(sw::SwTarget{buf.data(), w, h, w * 4});
    renderer.clear(Color{0, 0, 0, 1});
    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
    cl.devicePixelRatio = 1;
    const float shapes[3][6] = {
        {kNanF, 2, 30, 2, 16, 30}, {-1e30f, 2, 1e30f, 4, 16, 30}, {4, 4, 28, 4, 16, 28}};
    for (const auto& s : shapes) {
        PathRef ref{};
        ref.verbOffset = static_cast<uint32_t>(cl.verbs.size());
        ref.pointOffset = static_cast<uint32_t>(cl.points.size());
        cl.verbs.insert(cl.verbs.end(), {PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Close});
        cl.points.insert(cl.points.end(), s, s + 6);
        ref.verbCount = 4;
        ref.pointCount = 6;
        FillCmd f{};
        f.xf = Mat2x3{};
        f.path = ref;
        f.paint.kind = PaintKind::Solid;
        f.paint.color = Color{1, 0, 0, 1};
        f.rule = FillRule::NonZero;
        f.antialias = 1;
        cl.fills.push_back(f);
        Command cmd{};
        cmd.kind = CmdKind::FillPath;
        cmd.index = static_cast<uint32_t>(cl.fills.size() - 1);
        cl.commands.push_back(cmd);
    }
    renderer.render(cl, nullptr, nullptr);  // Returns.
    // The finite triangle still fills its center.
    CHECK(buf[(static_cast<size_t>(10) * w + 16) * 4] == 255);
}

namespace {

/// Fills the closed polygon `pts` (x, y pairs) red, anti-aliased, into a fresh w x h black target.
std::vector<uint8_t> fillPolygon(int w, int h, const std::vector<float>& pts) {
    std::vector<uint8_t> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    sw::SwRenderer renderer;
    renderer.setTarget(sw::SwTarget{buf.data(), w, h, w * 4});
    renderer.clear(Color{0, 0, 0, 1});
    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
    cl.devicePixelRatio = 1;
    PathRef ref{};
    cl.verbs.push_back(PathVerb::Move);
    for (size_t i = 2; i < pts.size(); i += 2) {
        cl.verbs.push_back(PathVerb::Line);
    }
    cl.verbs.push_back(PathVerb::Close);
    cl.points = pts;
    ref.verbCount = static_cast<uint32_t>(cl.verbs.size());
    ref.pointCount = static_cast<uint32_t>(pts.size());
    FillCmd f{};
    f.xf = Mat2x3{};
    f.path = ref;
    f.paint.kind = PaintKind::Solid;
    f.paint.color = Color{1, 0, 0, 1};
    f.rule = FillRule::NonZero;
    f.antialias = 1;
    cl.fills.push_back(f);
    Command cmd{};
    cmd.kind = CmdKind::FillPath;
    cmd.index = 0;
    cl.commands.push_back(cmd);
    renderer.render(cl, nullptr, nullptr);
    return buf;
}

}  // namespace

// T-258b (PR #269 review): the column clamp of E6 must not change what a far-reaching edge covers.
TEST_CASE("T-258a E6: a shape reaching far outside the target fills the same bytes as it clipped") {
    const int w = 32, h = 32;
    const float far0 = -5000.0f, far1 = static_cast<float>(w) + 5000.0f;
    const float near0 = -10.0f, near1 = static_cast<float>(w) + 10.0f;
    // Axis-aligned rectangle with fractional top and bottom edges.
    auto rect = [&](float x0, float x1) {
        return std::vector<float>{x0, 4.25f, x1, 4.25f, x1, 27.5f, x0, 27.5f};
    };
    CHECK(fillPolygon(w, h, rect(far0, far1)) == fillPolygon(w, h, rect(near0, near1)));
    // Parallelogram: top and bottom on y = 8 + x / 1024 and y = 24 + x / 1024, exact in float at
    // every x used here, so both versions lie on the same lines.
    auto slanted = [&](float x0, float x1) {
        return std::vector<float>{x0, 8.0f + x0 / 1024.0f,  x1, 8.0f + x1 / 1024.0f,
                                  x1, 24.0f + x1 / 1024.0f, x0, 24.0f + x0 / 1024.0f};
    };
    CHECK(fillPolygon(w, h, slanted(far0, far1)) == fillPolygon(w, h, slanted(near0, near1)));
}

// ------------------------------------------------------------------------------------------------
// E11 image decode
// ------------------------------------------------------------------------------------------------

TEST_CASE("T-258a E11: images wider or taller than 16384, or over 64M pixels, fail to decode") {
    ImageData image;
    CHECK_FALSE(
        sw::decodeImageBytes(pngHeaderOnly(23000, 23000).data(), pngHeaderOnly(23000, 23000).size(), image));
    CHECK_FALSE(
        sw::decodeImageBytes(pngHeaderOnly(8200, 8200).data(), pngHeaderOnly(8200, 8200).size(), image));
    for (int w : {16384, 16385}) {
        CAPTURE(w);
        std::vector<uint8_t> rgba(static_cast<size_t>(w) * 4, 128);
        std::vector<uint8_t> png;
        REQUIRE(sw::encodePng(rgba.data(), w, 1, w * 4, png));
        CHECK(sw::decodeImageBytes(png.data(), png.size(), image) == (w <= 16384));
    }
}
