#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

#include "doctest.h"
#include "sw_effects.h"
#include "sw_layer.h"
#include "xt/command_list.h"
#include "xt/node.h"
#include "xt/sw/sw_renderer.h"
#include "xt/types.h"

/// SwRenderer PushEffect/PopEffect integration tests: hand-filled CommandLists rendered to a
/// 64x64 target, quantized pixel assertions (no goldens, same style as test_sw_3d.cpp/
/// test_sw_layer_mask.cpp). See Docs/design/18-effects.md §18.3/§18.5/§18.6, Docs/tasks/T-53b.md
/// "測試".

using namespace xt;
using namespace xt::sw;

namespace {

struct Rgba {
    uint8_t r, g, b, a;
};

struct Fixture {
    static constexpr int w = 64, h = 64;
    std::vector<uint8_t> buf;
    SwRenderer renderer;
    SwTarget target;

    Fixture() : buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0), target{buf.data(), w, h, w * 4} {
        renderer.setTarget(target);
        renderer.clear(Color{0, 0, 0, 0});
    }

    Rgba pixel(int x, int y) const {
        size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
        return Rgba{buf[i], buf[i + 1], buf[i + 2], buf[i + 3]};
    }
    float alphaF(int x, int y) const { return static_cast<float>(pixel(x, y).a) / 255.0f; }
    float rF(int x, int y) const { return static_cast<float>(pixel(x, y).r) / 255.0f; }
    float gF(int x, int y) const { return static_cast<float>(pixel(x, y).g) / 255.0f; }
    float bF(int x, int y) const { return static_cast<float>(pixel(x, y).b) / 255.0f; }
};

CommandList makeList() {
    CommandList cl;
    cl.viewport = Rect{0, 0, 64, 64};
    cl.devicePixelRatio = 1;
    return cl;
}

PathRef addRectPath(CommandList& cl, float x, float y, float w, float h) {
    PathRef ref{};
    ref.verbOffset = static_cast<uint32_t>(cl.verbs.size());
    ref.pointOffset = static_cast<uint32_t>(cl.points.size());
    cl.verbs.push_back(PathVerb::Move);
    cl.verbs.push_back(PathVerb::Line);
    cl.verbs.push_back(PathVerb::Line);
    cl.verbs.push_back(PathVerb::Line);
    cl.verbs.push_back(PathVerb::Close);
    float pts[8] = {x, y, x + w, y, x + w, y + h, x, y + h};
    cl.points.insert(cl.points.end(), pts, pts + 8);
    ref.verbCount = 5;
    ref.pointCount = 8;
    return ref;
}

void addFillRect(CommandList& cl, float x, float y, float w, float h, Color color) {
    PathRef path = addRectPath(cl, x, y, w, h);
    FillCmd f{};
    f.xf = Mat2x3{};
    f.path = path;
    f.paint.kind = PaintKind::Solid;
    f.paint.color = color;
    f.rule = FillRule::NonZero;
    f.antialias = 1;
    cl.fills.push_back(f);
    Command cmd{};
    cmd.kind = CmdKind::FillPath;
    cmd.index = static_cast<uint32_t>(cl.fills.size() - 1);
    cl.commands.push_back(cmd);
}

void addPushClipRect(CommandList& cl, Rect rect) {
    ClipRectCmd c{};
    c.xf = Mat2x3{};
    c.rect = rect;
    cl.clipRects.push_back(c);
    Command cmd{};
    cmd.kind = CmdKind::PushClipRect;
    cmd.index = static_cast<uint32_t>(cl.clipRects.size() - 1);
    cl.commands.push_back(cmd);
}

void addPopClip(CommandList& cl) {
    Command cmd{};
    cmd.kind = CmdKind::PopClip;
    cl.commands.push_back(cmd);
}

/// `boundsHint` defaults to the whole 64x64 fixture viewport: since T-133 the SW backend sizes the
/// layer's offscreen buffer from it, so an empty hint means an empty layer, not a full one.
void addPushLayer(CommandList& cl, float opacity, BlendMode blend, Rect boundsHint = Rect{0, 0, 64, 64}) {
    LayerCmd l{};
    l.opacity = opacity;
    l.blend = blend;
    l.boundsHint = boundsHint;
    cl.layers.push_back(l);
    Command cmd{};
    cmd.kind = CmdKind::PushLayer;
    cmd.index = static_cast<uint32_t>(cl.layers.size() - 1);
    cl.commands.push_back(cmd);
}

void addPopLayer(CommandList& cl) {
    Command cmd{};
    cmd.kind = CmdKind::PopLayer;
    cl.commands.push_back(cmd);
}

void addPushProjection(CommandList& cl, Mat4 viewProj, Mat2x3 rootXf = Mat2x3{},
                       Rect box = Rect{0, 0, 64, 64}) {
    ProjectionCmd c{};
    c.viewProj = viewProj;
    c.rootXf = rootXf;
    c.boxRect = box;
    c.depthTest = 0;
    cl.projections.push_back(c);
    Command cmd{};
    cmd.kind = CmdKind::PushProjection;
    cmd.index = static_cast<uint32_t>(cl.projections.size() - 1);
    cl.commands.push_back(cmd);
}

void addPopProjection(CommandList& cl) {
    Command cmd{};
    cmd.kind = CmdKind::PopProjection;
    cl.commands.push_back(cmd);
}

void addPushTransform3D(CommandList& cl, Mat4 world3) {
    Transform3DCmd c{};
    c.world3 = world3;
    cl.transforms3.push_back(c);
    Command cmd{};
    cmd.kind = CmdKind::PushTransform3D;
    cmd.index = static_cast<uint32_t>(cl.transforms3.size() - 1);
    cl.commands.push_back(cmd);
}

void addPopTransform3D(CommandList& cl) {
    Command cmd{};
    cmd.kind = CmdKind::PopTransform3D;
    cl.commands.push_back(cmd);
}

/// Pushes a node's effect stack. `effects` is copied into `cl.effects` (contiguous); `xf` is the
/// node's plane -> screen transform (Docs/design/18-effects.md §18.5's `EffectCmd::xf`, pre-dpr --
/// SwRenderer multiplies by `devicePixelRatio` itself, same as every other Cmd's `xf`).
void addPushEffect(CommandList& cl, const std::vector<EffectInstance>& effects, Mat2x3 xf, Rect boundsHint,
                   Rect contentBounds) {
    EffectCmd c{};
    c.effectOffset = static_cast<uint32_t>(cl.effects.size());
    c.effectCount = static_cast<uint32_t>(effects.size());
    for (const EffectInstance& e : effects) {
        cl.effects.push_back(e);
    }
    c.xf = xf;
    c.boundsHint = boundsHint;
    c.contentBounds = contentBounds;
    cl.effectCmds.push_back(c);
    Command cmd{};
    cmd.kind = CmdKind::PushEffect;
    cmd.index = static_cast<uint32_t>(cl.effectCmds.size() - 1);
    cl.commands.push_back(cmd);
}

void addPopEffect(CommandList& cl) {
    Command cmd{};
    cmd.kind = CmdKind::PopEffect;
    cl.commands.push_back(cmd);
}

/// Appends `stops` to `cl.stops` and returns the (offset, count) pair for a GradientMask
/// EffectInstance's stopOffset/stopCount.
std::pair<uint32_t, uint32_t> addStops(CommandList& cl, std::initializer_list<GradientStop> stops) {
    uint32_t offset = static_cast<uint32_t>(cl.stops.size());
    for (const GradientStop& s : stops) {
        cl.stops.push_back(s);
    }
    return {offset, static_cast<uint32_t>(stops.size())};
}

EffectInstance dropShadow(float ox, float oy, float blur, float spread, float opacity, Color color) {
    EffectInstance e{};
    e.kind = EffectKind::DropShadow;
    e.params[0] = ox;
    e.params[1] = oy;
    e.params[2] = blur;
    e.params[3] = spread;
    e.params[4] = opacity;
    e.colors[0] = color;
    return e;
}

EffectInstance dropShadowInset(float ox, float oy, float blur, float spread, float opacity, Color color) {
    EffectInstance e = dropShadow(ox, oy, blur, spread, opacity, color);
    e.params[5] = 1;  // inset (Docs/design/18-effects.md §18.3)
    return e;
}

EffectInstance glow(float radius, Color color, float intensity, bool inner) {
    EffectInstance e{};
    e.kind = EffectKind::Glow;
    e.params[0] = radius;
    e.params[1] = intensity;
    e.params[2] = inner ? 1.0f : 0.0f;
    e.colors[0] = color;
    return e;
}

EffectInstance blurFx(float radiusX, float radiusY) {
    EffectInstance e{};
    e.kind = EffectKind::Blur;
    e.params[0] = radiusX;
    e.params[1] = radiusY;
    return e;
}

EffectInstance colorAdjust(float brightness, float contrast, float saturation, float hueDeg, float opacity) {
    EffectInstance e{};
    e.kind = EffectKind::ColorAdjust;
    e.params[0] = brightness;
    e.params[1] = contrast;
    e.params[2] = saturation;
    e.params[3] = hueDeg;
    e.params[4] = opacity;
    return e;
}

EffectInstance gradientMask(Vec2 p0, Vec2 p1, bool radial, bool invert, uint32_t stopOffset,
                            uint32_t stopCount) {
    EffectInstance e{};
    e.kind = EffectKind::GradientMask;
    e.params[0] = p0.x;
    e.params[1] = p0.y;
    e.params[2] = p1.x;
    e.params[3] = p1.y;
    e.params[4] = radial ? 1.0f : 0.0f;
    e.params[5] = invert ? 1.0f : 0.0f;
    e.stopOffset = stopOffset;
    e.stopCount = stopCount;
    return e;
}

EffectInstance shine(float position, float width, float softness, float angleDeg, float intensity,
                     Color color) {
    EffectInstance e{};
    e.kind = EffectKind::Shine;
    e.params[0] = position;
    e.params[1] = width;
    e.params[2] = softness;
    e.params[3] = angleDeg;
    e.params[4] = intensity;
    e.colors[0] = color;
    return e;
}

EffectInstance backdropBlur(float radius, float saturation, float brightness) {
    EffectInstance e{};
    e.kind = EffectKind::BackdropBlur;
    e.params[0] = radius;
    e.params[1] = saturation;
    e.params[2] = brightness;
    return e;
}

EffectInstance outline(float radius, float softness, float position, Color color) {
    EffectInstance e{};
    e.kind = EffectKind::Outline;
    e.params[0] = radius;
    e.params[1] = softness;
    e.params[2] = position;
    e.colors[0] = color;
    return e;
}

/// `content` outset by `margin` on every side, clamped to the 64x64 viewport.
Rect outsetClamped(Rect content, float margin) {
    float x0 = std::max(0.0f, content.x - margin);
    float y0 = std::max(0.0f, content.y - margin);
    float x1 = std::min(64.0f, content.x + content.w + margin);
    float y1 = std::min(64.0f, content.y + content.h + margin);
    return Rect{x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
}

}  // namespace

// 1. Blur: a 20x20 red square blurred 4px stays red at the center, has partial alpha 4px outside
// its edge, and (approximately) conserves total alpha.
TEST_CASE("SwEffects: Blur keeps the center opaque, spreads alpha at the edge, conserves mass") {
    Fixture fx;
    CommandList cl = makeList();
    Rect content{22, 22, 20, 20};
    Rect bounds = outsetClamped(content, 16);

    addPushEffect(cl, {blurFx(4, 4)}, Mat2x3{}, bounds, content);
    addFillRect(cl, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    addPopEffect(cl);
    fx.renderer.render(cl, nullptr, nullptr);

    CHECK(fx.rF(32, 32) > 0.95f);
    // The box-approximated Gaussian (Docs/design/18-effects.md §18.6) has *bounded* support
    // (three box passes of half-width r=1 at sigma=2 reach exactly 3px, unlike a true Gaussian's
    // unbounded tail) and tapers off quickly within that reach, so 2px beyond the edge is already
    // a small (measured ~0.035) but clearly nonzero value -- not the ~0.05-0.6 a true Gaussian
    // would give at the same distance.
    float edgeAlpha = fx.alphaF(44, 32);  // 2px outside the right edge (x=42).
    CHECK(edgeAlpha > 0.02f);
    CHECK(edgeAlpha < 0.6f);

    // Mass conservation vs. an unblurred reference render of the same square, within 3%.
    Fixture ref;
    CommandList clRef = makeList();
    addFillRect(clRef, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    ref.renderer.render(clRef, nullptr, nullptr);

    double sumBlurred = 0, sumRef = 0;
    for (int y = 0; y < Fixture::h; ++y) {
        for (int x = 0; x < Fixture::w; ++x) {
            sumBlurred += fx.alphaF(x, y);
            sumRef += ref.alphaF(x, y);
        }
    }
    CHECK(sumBlurred == doctest::Approx(sumRef).epsilon(0.03));
}

// 2. DropShadow: shadow(6px 6px 0 #000) puts black 3px beyond the bottom-right corner and leaves
// the top-left corner's outside untouched; adding blur turns the edge into a gradient.
TEST_CASE("SwEffects: DropShadow offsets a hard-edged shadow behind the content") {
    Fixture fx;
    CommandList cl = makeList();
    Rect content{20, 20, 20, 20};  // x,y in [20,40).
    Rect bounds = outsetClamped(content, 14);

    addPushEffect(cl, {dropShadow(6, 6, 0, 0, 1, Color{0, 0, 0, 1})}, Mat2x3{}, bounds, content);
    addFillRect(cl, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    addPopEffect(cl);
    fx.renderer.render(cl, nullptr, nullptr);

    // 3px beyond the bottom-right corner (40,40): pure shadow, no content there.
    Rgba shadowPx = fx.pixel(43, 43);
    CHECK(shadowPx.a > 200);
    CHECK(shadowPx.r < 20);
    // 3px beyond the top-left corner (20,20): neither content nor shadow reach here.
    CHECK(fx.pixel(17, 17).a == 0);
}

// Regression (found while building the HUD demo through the Marionette dev endpoint): an element
// with a blurred effect that lies completely outside the target used to crash the box blur, whose
// edge clamping indexes `h - 1` / `w - 1` of an empty effect buffer.
TEST_CASE("SwEffects: an effect layer entirely outside the target renders nothing and does not crash") {
    Fixture fx;
    CommandList cl = makeList();
    Rect content{2000, 2000, 40, 40};
    Rect bounds{1980, 1980, 80, 80};  // Unclamped on purpose: nowhere near the target.

    addPushEffect(cl, {dropShadow(0, 8, 12, 2, 1, Color{0, 0, 0, 1})}, Mat2x3{}, bounds, content);
    addFillRect(cl, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    addPopEffect(cl);
    fx.renderer.render(cl, nullptr, nullptr);

    CHECK(fx.pixel(10, 10).a == 0);
}

TEST_CASE("SwEffects: DropShadow with blur turns the shadow edge into a gradient") {
    Fixture fx;
    CommandList cl = makeList();
    Rect content{20, 20, 20, 20};
    Rect bounds = outsetClamped(content, 20);

    addPushEffect(cl, {dropShadow(6, 6, 4, 0, 1, Color{0, 0, 0, 1})}, Mat2x3{}, bounds, content);
    addFillRect(cl, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    addPopEffect(cl);
    fx.renderer.render(cl, nullptr, nullptr);

    // Well inside the shadow-only region (shifted square [26,46)x[26,46), >3px from every edge --
    // the box-approximated blur's bounded reach, Docs/design/18-effects.md §18.6): fully opaque
    // black.
    float deepA = fx.alphaF(36, 36);
    // 2px beyond the shifted square's right edge only (not a corner -- a corner's alpha is the
    // product of two small per-axis falloffs, which can round to 0 once quantized to 8-bit):
    // a partial, gradient alpha (not 0, not deepA).
    float farA = fx.alphaF(48, 36);
    CHECK(deepA > 0.9f);
    CHECK(farA > 0.0f);
    CHECK(farA < deepA);
}

// 2c. DropShadow inset (T-111): the shadow lives inside the shape only -- every pixel outside it
// is byte-identical to the same content rendered without the effect -- and with a (+6,+6) offset
// the top-left inner edge is darker than the center.
TEST_CASE("SwEffects: DropShadow inset darkens the inner edge and leaves the outside untouched") {
    Rect content{20, 20, 20, 20};

    Fixture plain;
    {
        CommandList cl = makeList();
        addFillRect(cl, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
        plain.renderer.render(cl, nullptr, nullptr);
    }

    Fixture fx;
    CommandList cl = makeList();
    // inset expands nothing (18.3), so boundsHint is exactly the content AABB.
    addPushEffect(cl, {dropShadowInset(6, 6, 6, 0, 1, Color{0, 0, 0, 1})}, Mat2x3{}, content, content);
    addFillRect(cl, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    addPopEffect(cl);
    fx.renderer.render(cl, nullptr, nullptr);

    for (int y = 0; y < Fixture::h; ++y) {
        for (int x = 0; x < Fixture::w; ++x) {
            bool insideShape = x >= 20 && x < 40 && y >= 20 && y < 40;
            if (insideShape) {
                continue;
            }
            Rgba a = fx.pixel(x, y);
            Rgba b = plain.pixel(x, y);
            bool same = a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
            REQUIRE(same);
        }
    }

    // The shape stays fully opaque; the shadow only darkens it.
    CHECK(fx.pixel(30, 30).a == 255);
    CHECK(fx.pixel(22, 22).a == 255);
    // Top-left inner edge (opposite the +6,+6 offset) is darker than the center.
    CHECK(fx.rF(22, 22) < fx.rF(30, 30));
    // The bottom-right inner edge is the lit side: not darker than the center.
    CHECK(fx.rF(37, 37) >= fx.rF(30, 30));
}

// 3. Glow: outer glow(6px #ff0) is yellow-ish with a/>0 2px outside the square; inner glow makes
// the inner edge brighter (higher r/g) than the square's center.
TEST_CASE("SwEffects: Glow outer halos the content, inner brightens its inner edge") {
    Fixture fx;
    CommandList cl = makeList();
    Rect content{22, 22, 20, 20};
    Rect bounds = outsetClamped(content, 16);

    addPushEffect(cl, {glow(6, Color{1, 1, 0, 1}, 1, false)}, Mat2x3{}, bounds, content);
    addFillRect(cl, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    addPopEffect(cl);
    fx.renderer.render(cl, nullptr, nullptr);

    // 2px outside the right edge (x=42): yellow-ish (g much higher than a plain red shadow would
    // give) and some alpha.
    CHECK(fx.alphaF(44, 32) > 0.0f);
    CHECK(fx.gF(44, 32) > 0.05f);

    Fixture fxInner;
    CommandList clInner = makeList();
    // Inner Glow has 0 expansion (Docs/design/18-effects.md §18.3), but the buffer still needs a
    // few px of margin beyond the shape for this test: without it the buffer is *exactly* the
    // filled rectangle, so every pixel reads alpha=1 with no "outside" reference at all, and the
    // blurred (1 - alpha) field used for the inner highlight is uniformly 0 everywhere.
    Rect innerBounds = outsetClamped(content, 10);
    addPushEffect(clInner, {glow(6, Color{1, 1, 1, 1}, 2, true)}, Mat2x3{}, innerBounds, content);
    addFillRect(clInner, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    addPopEffect(clInner);
    fxInner.renderer.render(clInner, nullptr, nullptr);

    float innerEdgeR = fxInner.rF(23, 32);  // 1px inside the left edge.
    float centerR = fxInner.rF(32, 32);
    float innerEdgeG = fxInner.gF(23, 32);
    float centerG = fxInner.gF(32, 32);
    CHECK(innerEdgeR >= centerR);
    CHECK(innerEdgeG > centerG);
}

// 4. ColorAdjust: saturate(-1) desaturates a red square to gray; hue-rotate(180deg) turns red into
// cyan; opacity(0.5) halves alpha.
TEST_CASE("SwEffects: ColorAdjust saturate/hue-rotate/opacity") {
    Rect content{22, 22, 20, 20};

    Fixture fxSat;
    CommandList clSat = makeList();
    addPushEffect(clSat, {colorAdjust(0, 0, -1, 0, 1)}, Mat2x3{}, content, content);
    addFillRect(clSat, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    addPopEffect(clSat);
    fxSat.renderer.render(clSat, nullptr, nullptr);
    CHECK(fxSat.rF(32, 32) == doctest::Approx(fxSat.gF(32, 32)).epsilon(0.05));
    CHECK(fxSat.gF(32, 32) == doctest::Approx(fxSat.bF(32, 32)).epsilon(0.05));

    Fixture fxHue;
    CommandList clHue = makeList();
    addPushEffect(clHue, {colorAdjust(0, 0, 0, 180, 1)}, Mat2x3{}, content, content);
    addFillRect(clHue, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    addPopEffect(clHue);
    fxHue.renderer.render(clHue, nullptr, nullptr);
    CHECK(fxHue.rF(32, 32) < 0.2f);
    CHECK(fxHue.gF(32, 32) > 0.4f);
    CHECK(fxHue.bF(32, 32) > 0.4f);

    Fixture fxOp;
    CommandList clOp = makeList();
    addPushEffect(clOp, {colorAdjust(0, 0, 0, 0, 0.5f)}, Mat2x3{}, content, content);
    addFillRect(clOp, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    addPopEffect(clOp);
    fxOp.renderer.render(clOp, nullptr, nullptr);
    CHECK(fxOp.alphaF(32, 32) == doctest::Approx(0.5f).epsilon(0.05));
}

// 5. GradientMask: linear left-to-right alpha 1->0 across the content; invert flips it; radial is
// 1 at the center and 0 at the edge.
TEST_CASE("SwEffects: GradientMask linear, invert, radial") {
    Rect content{12, 12, 40, 40};  // x,y in [12,52).

    Fixture fxLin;
    CommandList clLin = makeList();
    auto stopsLin = addStops(clLin, {GradientStop{0, Color{0, 0, 0, 1}}, GradientStop{1, Color{0, 0, 0, 0}}});
    addPushEffect(clLin,
                  {gradientMask(Vec2{12, 32}, Vec2{52, 32}, false, false, stopsLin.first, stopsLin.second)},
                  Mat2x3{}, content, content);
    addFillRect(clLin, content.x, content.y, content.w, content.h, Color{1, 1, 1, 1});
    addPopEffect(clLin);
    fxLin.renderer.render(clLin, nullptr, nullptr);
    CHECK(fxLin.alphaF(13, 32) > 0.85f);
    CHECK(fxLin.alphaF(50, 32) < 0.15f);
    CHECK(fxLin.alphaF(32, 32) == doctest::Approx(0.5f).epsilon(0.1));

    Fixture fxInv;
    CommandList clInv = makeList();
    auto stopsInv = addStops(clInv, {GradientStop{0, Color{0, 0, 0, 1}}, GradientStop{1, Color{0, 0, 0, 0}}});
    addPushEffect(clInv,
                  {gradientMask(Vec2{12, 32}, Vec2{52, 32}, false, true, stopsInv.first, stopsInv.second)},
                  Mat2x3{}, content, content);
    addFillRect(clInv, content.x, content.y, content.w, content.h, Color{1, 1, 1, 1});
    addPopEffect(clInv);
    fxInv.renderer.render(clInv, nullptr, nullptr);
    CHECK(fxInv.alphaF(13, 32) < 0.15f);
    CHECK(fxInv.alphaF(50, 32) > 0.85f);

    Fixture fxRad;
    CommandList clRad = makeList();
    auto stopsRad = addStops(clRad, {GradientStop{0, Color{0, 0, 0, 1}}, GradientStop{1, Color{0, 0, 0, 0}}});
    addPushEffect(clRad,
                  {gradientMask(Vec2{32, 32}, Vec2{52, 32}, true, false, stopsRad.first, stopsRad.second)},
                  Mat2x3{}, content, content);
    addFillRect(clRad, content.x, content.y, content.w, content.h, Color{1, 1, 1, 1});
    addPopEffect(clRad);
    fxRad.renderer.render(clRad, nullptr, nullptr);
    CHECK(fxRad.alphaF(32, 32) > 0.85f);
    CHECK(fxRad.alphaF(13, 32) < 0.15f);
}

// 6. Shine: position 0.5, width 10px, angle 0 puts a bright band in the middle, dimmer 10px either
// side.
TEST_CASE("SwEffects: Shine puts a bright band at the content's center") {
    Fixture fx;
    CommandList cl = makeList();
    Rect content{12, 12, 40, 40};  // center (32, 32).

    addPushEffect(cl, {shine(0.5f, 10, 0.5f, 0, 1, Color{1, 1, 1, 1})}, Mat2x3{}, content, content);
    addFillRect(cl, content.x, content.y, content.w, content.h, Color{0.2f, 0.2f, 0.2f, 1});
    addPopEffect(cl);
    fx.renderer.render(cl, nullptr, nullptr);

    float centerR = fx.rF(32, 32);
    float leftR = fx.rF(22, 32);
    float rightR = fx.rF(42, 32);
    CHECK(centerR > leftR);
    CHECK(centerR > rightR);
}

// 7. Outline: outer places blue 1.5px outside the square; inner places blue 1.5px inside the edge
// (center stays red); center places blue on both sides of the edge.
TEST_CASE("SwEffects: Outline outer/inner/center") {
    Rect content{22, 22, 20, 20};  // x,y in [22,42).
    Color blue{0, 0, 1, 1};

    Fixture fxOuter;
    CommandList clOuter = makeList();
    Rect boundsOuter = outsetClamped(content, 6);
    addPushEffect(clOuter, {outline(3, 0, 0, blue)}, Mat2x3{}, boundsOuter, content);
    addFillRect(clOuter, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    addPopEffect(clOuter);
    fxOuter.renderer.render(clOuter, nullptr, nullptr);
    Rgba outerPx = fxOuter.pixel(43, 32);  // ~1.5px outside the right edge (x=42).
    CHECK(outerPx.b > 200);
    CHECK(outerPx.r < 60);

    Fixture fxInner;
    CommandList clInner = makeList();
    // Inner Outline has 0 expansion (Docs/design/18-effects.md §18.3); same margin note as the
    // Glow inner subtest above -- without it the buffer is exactly the filled rectangle and the
    // alpha field has no "outside" reference for signedDistance to measure against.
    Rect innerBounds = outsetClamped(content, 6);
    addPushEffect(clInner, {outline(3, 0, 2, blue)}, Mat2x3{}, innerBounds, content);
    addFillRect(clInner, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    addPopEffect(clInner);
    fxInner.renderer.render(clInner, nullptr, nullptr);
    Rgba innerPx = fxInner.pixel(40, 32);  // ~1.5px inside the right edge.
    CHECK(innerPx.b > 200);
    CHECK(fxInner.rF(32, 32) > 0.9f);  // center still red.

    // Center's `dst = srcOver(line, src)` puts the line *below* the content (same as outer), so
    // over a pixel-aligned hard edge the line is fully hidden by the opaque interior -- "blue on
    // both sides" only shows up at a fractional (antialiased, partially-covered) edge pixel, where
    // `src`'s own alpha is < 1 and lets the line blend through. `contentFrac`'s right edge sits at
    // x=42.5, so pixel 42 has ~0.5 coverage.
    Rect contentFrac{22, 22, 20.5f, 20};
    Fixture fxCenter;
    CommandList clCenter = makeList();
    Rect boundsCenter = outsetClamped(contentFrac, 6);
    addPushEffect(clCenter, {outline(3, 0, 1, blue)}, Mat2x3{}, boundsCenter, contentFrac);
    addFillRect(clCenter, contentFrac.x, contentFrac.y, contentFrac.w, contentFrac.h, Color{1, 0, 0, 1});
    addPopEffect(clCenter);
    fxCenter.renderer.render(clCenter, nullptr, nullptr);
    // Center's threshold is radius/2 (1.5px here), a narrower band than outer/inner's full radius,
    // so "close outside" has to stay within that band too.
    CHECK(fxCenter.pixel(43, 32).b > 150);  // just outside (src fully transparent): mostly blue.
    CHECK(fxCenter.pixel(42, 32).b > 50);   // the fractional edge pixel: blue blends through.
    CHECK(fxCenter.pixel(42, 32).r > 50);   // ...blended with the (partially covering) red, too.
}

// 8. Stacking order matters: blur then shadow != shadow then blur.
TEST_CASE("SwEffects: effect stacking order changes the result") {
    Rect content{20, 20, 20, 20};
    Rect bounds = outsetClamped(content, 20);
    Color shadowColor{0, 0, 0, 1};

    // blur(2px) alone (sigma=1) rounds down to a box width of 1 (Docs/design/18-effects.md §18.6's
    // `w = floor(sqrt(12*sigma^2/3+1))`, forced odd), i.e. a no-op -- too weak to tell stacking
    // order apart. A wider blur(8px) (sigma=4) has real reach: blur-then-shadow generates the
    // shadow's alpha from an already-softened square (then shifts it, an exact translation of a
    // blurred field), while shadow-then-blur composites a *crisp* shadow next to the crisp square
    // first and blurs that whole composite together, mixing the square's own color into the blur
    // near their shared boundary -- a real, order-dependent difference.
    Fixture fxA;
    CommandList clA = makeList();
    addPushEffect(clA, {blurFx(8, 8), dropShadow(6, 6, 2, 0, 1, shadowColor)}, Mat2x3{}, bounds, content);
    addFillRect(clA, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    addPopEffect(clA);
    fxA.renderer.render(clA, nullptr, nullptr);

    Fixture fxB;
    CommandList clB = makeList();
    addPushEffect(clB, {dropShadow(6, 6, 2, 0, 1, shadowColor), blurFx(8, 8)}, Mat2x3{}, bounds, content);
    addFillRect(clB, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    addPopEffect(clB);
    fxB.renderer.render(clB, nullptr, nullptr);

    int differing = 0;
    for (int y = 0; y < Fixture::h; ++y) {
        for (int x = 0; x < Fixture::w; ++x) {
            Rgba a = fxA.pixel(x, y);
            Rgba b = fxB.pixel(x, y);
            if (std::abs(a.r - b.r) > 2 || std::abs(a.g - b.g) > 2 || std::abs(a.b - b.b) > 2 ||
                std::abs(a.a - b.a) > 2) {
                ++differing;
            }
        }
    }
    CHECK(static_cast<float>(differing) / static_cast<float>(Fixture::w * Fixture::h) >= 0.01f);
}

// 9. An outer PushLayer(0.5) halves the whole effect's output alpha; overflow clipping inside the
// effect (around the fill only) does not clip the shadow the effect stack produces afterward.
TEST_CASE("SwEffects: outer PushLayer halves alpha; overflow clip does not clip the shadow") {
    Fixture fx;
    CommandList cl = makeList();
    // Fill a 12x12 square at (20,20)-(32,32), clipped to itself (a no-op clip -- exercises the
    // "clip active inside PushEffect" path) then a DropShadow offset by (15,15), landing well
    // outside the clip rect at [35,47)x[35,47).
    Rect fillRect{20, 20, 12, 12};
    Rect bounds{10, 10, 50, 50};

    addPushLayer(cl, 0.5f, BlendMode::Normal);
    addPushEffect(cl, {dropShadow(15, 15, 0, 0, 1, Color{0, 0, 0, 1})}, Mat2x3{}, bounds, fillRect);
    addPushClipRect(cl, fillRect);
    addFillRect(cl, fillRect.x, fillRect.y, fillRect.w, fillRect.h, Color{1, 0, 0, 1});
    addPopClip(cl);
    addPopEffect(cl);
    addPopLayer(cl);
    fx.renderer.render(cl, nullptr, nullptr);

    // Content pixel (inside the clip rect): alpha ~0.5 (1.0 content alpha * 0.5 layer opacity).
    CHECK(fx.alphaF(25, 25) == doctest::Approx(0.5f).epsilon(0.1));
    // Shadow pixel, well outside the overflow clip rect [20,32)x[20,32): still present, scaled by
    // the outer layer's opacity, proving the overflow clip did not suppress the shadow.
    CHECK(fx.alphaF(40, 40) == doctest::Approx(0.5f).epsilon(0.1));
    CHECK(fx.rF(40, 40) < 0.1f);
}

// 10. Effect + 3D: PushEffect nested inside a PushProjection/PushTransform3D pair does not crash,
// and a foreshortened screen xf (as effectToScreen would produce for a rotateY(60deg) layer)
// shrinks the shadow's on-screen displacement below its node-local-px magnitude.
TEST_CASE("SwEffects: effect nested inside a 3D projection does not crash; xf foreshortens the shadow") {
    Fixture fx;
    CommandList cl = makeList();
    Rect content{22, 22, 20, 20};
    Rect bounds = outsetClamped(content, 20);
    // `xf` approximates the anchor Jacobian of a rotateY(60deg) layer (Docs/design/19-3d-space.md
    // §19.6): the local x axis foreshortens by cos(60deg) = 0.5, y is unaffected. Not a full
    // project3d::jacobianAt reproduction (out of scope for a hand-built CommandList per
    // Docs/design/14-coding-standards.md §14.8) -- chosen to exercise effectToScreen's linear-part
    // offset conversion (Docs/design/18-effects.md §18.5) under a representative foreshortening,
    // decoupled from the projection stack's own geometry (identity viewProj/world3 below) so the
    // filled square's screen position stays predictable for the pixel assertions.
    Mat2x3 xf{0.5f, 0, 0, 1, 0, 0};

    addPushProjection(cl, mat4Identity());
    addPushTransform3D(cl, mat4Identity());
    addPushEffect(cl, {dropShadow(10, 0, 0, 0, 1, Color{0, 0, 0, 1})}, xf, bounds, content);
    addFillRect(cl, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    addPopEffect(cl);
    addPopTransform3D(cl);
    addPopProjection(cl);
    fx.renderer.render(cl, nullptr, nullptr);  // Must not crash/assert.

    // The shadow's offsetX (10 local px) foreshortens to 5 device px on screen (blur/spread 0, so
    // an exact shift): its footprint is [27,47)x[22,42), well under a 10px-offset shadow's
    // (which would reach x=52).
    CHECK(fx.alphaF(44, 32) > 0.0f);  // inside the 5px-shifted shadow's footprint.
    CHECK(fx.pixel(53, 32).a == 0);   // well beyond even a full 10px-offset shadow.
}

// 11. Scratch reuse (Docs/tasks/T-102b.md "測試"): a warm `EffectScratch` stops growing. 100
// identical `applyEffectStack` frames over a three-effect stack; every buffer's capacity after
// frame 2 must equal its capacity after frame 100 (nothing is reallocated on a steady-state
// frame, which is the point of hoisting the buffers out of the passes).
// T-130 (Docs/design/18-effects.md §18.3's kind 8, §18.6). Both cases sit on a *uniform* opaque
// backdrop, so the blurred backdrop equals the backdrop itself everywhere (an edge-clamped blur of
// a constant field is that constant) and the composite can be worked out by hand:
//   dst = srcOver(backdropBlurred * alpha(src), src) = src + backdrop * alpha(src) * (1 - alpha(src))
// followed by PopEffect's own src-over of that onto the target.
TEST_CASE("SwEffects: BackdropBlur leaves fully transparent content untouched") {
    const Color backdrop{0.2f, 0.6f, 0.4f, 1};
    Rect content{16, 16, 32, 32};

    // With the effect: the panel draws nothing at all inside its own layer.
    Fixture fx;
    CommandList cl = makeList();
    addFillRect(cl, 0, 0, 64, 64, backdrop);
    addPushEffect(cl, {backdropBlur(8, 0, 0)}, Mat2x3{}, content, content);
    addPopEffect(cl);
    fx.renderer.render(cl, nullptr, nullptr);

    // Without it: the same backdrop, nothing else.
    Fixture ref;
    CommandList clRef = makeList();
    addFillRect(clRef, 0, 0, 64, 64, backdrop);
    ref.renderer.render(clRef, nullptr, nullptr);

    // alpha(src) == 0 masks the blurred backdrop away entirely, so the panel contributes nothing.
    for (int y = 0; y < Fixture::h; ++y) {
        for (int x = 0; x < Fixture::w; ++x) {
            REQUIRE(fx.pixel(x, y).r == ref.pixel(x, y).r);
            REQUIRE(fx.pixel(x, y).g == ref.pixel(x, y).g);
            REQUIRE(fx.pixel(x, y).b == ref.pixel(x, y).b);
            REQUIRE(fx.pixel(x, y).a == ref.pixel(x, y).a);
        }
    }
}

TEST_CASE("SwEffects: BackdropBlur mixes the blurred backdrop into a half-transparent panel") {
    const Color backdrop{0.2f, 0.6f, 0.4f, 1};
    Rect content{16, 16, 32, 32};
    const float kTol = 2.0f / 255.0f;

    Fixture fx;
    CommandList cl = makeList();
    addFillRect(cl, 0, 0, 64, 64, backdrop);
    addPushEffect(cl, {backdropBlur(8, 0, 0)}, Mat2x3{}, content, content);
    addFillRect(cl, content.x, content.y, content.w, content.h, Color{1, 1, 1, 0.5f});
    addPopEffect(cl);
    fx.renderer.render(cl, nullptr, nullptr);

    // src (premultiplied) = (0.5, 0.5, 0.5, 0.5); bottom = backdrop * 0.5;
    // effect layer = src + bottom * 0.5 -> a = 0.75, rgb = (0.5, 0.5, 0.5) + backdrop * 0.25;
    // PopEffect src-overs that onto the opaque backdrop -> a = 1,
    // rgb = 0.5 + backdrop * 0.25 + backdrop * 0.25.
    auto expect = [&](float channel) { return 0.5f + channel * 0.5f; };
    CHECK(std::fabs(fx.alphaF(32, 32) - (1.0f)) <= kTol);
    CHECK(std::fabs(fx.rF(32, 32) - (expect(backdrop.r))) <= kTol);
    CHECK(std::fabs(fx.gF(32, 32) - (expect(backdrop.g))) <= kTol);
    CHECK(std::fabs(fx.bF(32, 32) - (expect(backdrop.b))) <= kTol);

    // Outside the panel nothing changed.
    CHECK(std::fabs(fx.rF(4, 4) - (backdrop.r)) <= kTol);
    CHECK(std::fabs(fx.gF(4, 4) - (backdrop.g)) <= kTol);

    // An *opaque* panel is untouched by the effect (alpha(src) == 1 -> the masked backdrop is
    // fully covered by the content itself).
    Fixture opaque;
    CommandList clOpaque = makeList();
    addFillRect(clOpaque, 0, 0, 64, 64, backdrop);
    addPushEffect(clOpaque, {backdropBlur(8, 0, 0)}, Mat2x3{}, content, content);
    addFillRect(clOpaque, content.x, content.y, content.w, content.h, Color{1, 1, 1, 1});
    addPopEffect(clOpaque);
    opaque.renderer.render(clOpaque, nullptr, nullptr);
    CHECK(std::fabs(opaque.rF(32, 32) - (1.0f)) <= kTol);
    CHECK(std::fabs(opaque.gF(32, 32) - (1.0f)) <= kTol);
    CHECK(std::fabs(opaque.bF(32, 32) - (1.0f)) <= kTol);
}

TEST_CASE("SwEffects: EffectScratch capacities stop growing after the first frames") {
    const int w = 48, h = 40;
    Surface src;
    src.w = w;
    src.h = h;
    src.rgba.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0.0f);
    for (int y = 10; y < 30; ++y) {
        for (int x = 10; x < 38; ++x) {
            src.set(x, y, Color{1, 0, 0, 1});  // premultiplied opaque red.
        }
    }

    // One of each kind that owns scratch buffers: shadow (alpha/dilate/blur), inner glow
    // (alpha/invert/blur) and inner outline (alpha/SDF/row), plus two stage buffers because the
    // stack has more than one entry.
    std::vector<EffectInstance> stack = {
        dropShadow(3, 3, 6, 1, 1, Color{0, 0, 0, 1}),
        glow(5, Color{1, 1, 0, 1}, 1, /*inner=*/true),
        outline(2, 1, /*position=*/2, Color{1, 1, 1, 1}),
    };

    EffectScratch scratch;
    Surface scratchA, scratchB, dst;
    std::vector<GradientStop> stops;
    size_t warm[8] = {};
    for (int frame = 1; frame <= 100; ++frame) {
        applyEffectStack(stack, stops, Mat2x3{1, 0, 0, 1, 0, 0}, Vec2{0, 0}, Rect{10, 10, 28, 20}, src,
                         scratchA, scratchB, dst, scratch);
        size_t caps[8] = {
            scratch.alpha.capacity(),       scratch.dilated.capacity(),     scratch.blurred.capacity(),
            scratch.fieldA.capacity(),      scratch.fieldB.capacity(),      scratch.row.capacity(),
            scratch.stageA.rgba.capacity(), scratch.stageB.rgba.capacity(),
        };
        if (frame == 2) {
            for (int i = 0; i < 8; ++i) {
                warm[i] = caps[i];
                CHECK(caps[i] > 0);
            }
        } else if (frame == 100) {
            for (int i = 0; i < 8; ++i) {
                CHECK(caps[i] == warm[i]);
            }
        }
    }
}
