#include <cmath>
#include <cstdint>
#include <vector>

#include "doctest.h"
#include "sw_layer.h"
#include "xt/command_list.h"
#include "xt/node.h"
#include "xt/sw/sw_renderer.h"
#include "xt/types.h"

/// SwRenderer Layer/Mask integration tests, plus a direct LayerStack pool-size test. See
/// docs/tasks/T-12.md "測試" / "test_sw_layer_mask.cpp".

using namespace xt;
using namespace xt::sw;

namespace {

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

PathRef addCirclePath(CommandList& cl, float cx, float cy, float r, int segments = 32) {
    PathRef ref{};
    ref.verbOffset = static_cast<uint32_t>(cl.verbs.size());
    ref.pointOffset = static_cast<uint32_t>(cl.points.size());
    cl.verbs.push_back(PathVerb::Move);
    for (int i = 1; i < segments; ++i) {
        cl.verbs.push_back(PathVerb::Line);
    }
    cl.verbs.push_back(PathVerb::Close);
    for (int i = 0; i < segments; ++i) {
        float angle = static_cast<float>(i) / static_cast<float>(segments) * 6.2831853f;
        cl.points.push_back(cx + r * std::cos(angle));
        cl.points.push_back(cy + r * std::sin(angle));
    }
    ref.verbCount = static_cast<uint32_t>(segments) + 1;
    ref.pointCount = static_cast<uint32_t>(segments) * 2;
    return ref;
}

void addFillPath(CommandList& cl, const PathRef& path, Color color, FillRule rule = FillRule::NonZero) {
    FillCmd f{};
    f.xf = Mat2x3{};
    f.path = path;
    f.paint.kind = PaintKind::Solid;
    f.paint.color = color;
    f.rule = rule;
    f.antialias = 1;
    cl.fills.push_back(f);
    Command cmd{};
    cmd.kind = CmdKind::FillPath;
    cmd.index = static_cast<uint32_t>(cl.fills.size() - 1);
    cl.commands.push_back(cmd);
}

void addFillRect(CommandList& cl, float x, float y, float w, float h, Color color) {
    addFillPath(cl, addRectPath(cl, x, y, w, h), color);
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

void addPushMask(CommandList& cl, MaskMode mode) {
    MaskCmd m{};
    m.mode = mode;
    m.boundsHint = Rect{0, 0, 0, 0};
    cl.masks.push_back(m);
    Command cmd{};
    cmd.kind = CmdKind::PushMask;
    cmd.index = static_cast<uint32_t>(cl.masks.size() - 1);
    cl.commands.push_back(cmd);
}

void addEndMaskContent(CommandList& cl) {
    Command cmd{};
    cmd.kind = CmdKind::EndMaskContent;
    cl.commands.push_back(cmd);
}

void addPopMask(CommandList& cl) {
    Command cmd{};
    cmd.kind = CmdKind::PopMask;
    cl.commands.push_back(cmd);
}

struct Rgba {
    uint8_t r, g, b, a;
};

Rgba pixelAt(const std::vector<uint8_t>& buf, int width, int x, int y) {
    size_t i = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
    return Rgba{buf[i], buf[i + 1], buf[i + 2], buf[i + 3]};
}

bool approxByte(uint8_t a, int expected, int tol = 2) {
    return std::abs(static_cast<int>(a) - expected) <= tol;
}

struct Fixture {
    static constexpr int w = 64, h = 64;
    std::vector<uint8_t> buf;
    SwRenderer renderer;
    SwTarget target;

    Fixture() : buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0), target{buf.data(), w, h, w * 4} {
        renderer.setTarget(target);
    }
};

}  // namespace

TEST_CASE("Layer: opacity 0.5, two overlapping opaque red squares composite once, not twice") {
    Fixture fx;
    fx.renderer.clear(Color{1, 1, 1, 1});

    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(fx.w), static_cast<float>(fx.h)};
    cl.devicePixelRatio = 1;
    addPushLayer(cl, 0.5f, BlendMode::Normal);
    addFillRect(cl, 10, 10, 30, 30, Color{1, 0, 0, 1});
    addFillRect(cl, 20, 20, 30, 30, Color{1, 0, 0, 1});  // overlaps [20,40)x[20,40)
    addPopLayer(cl);
    fx.renderer.render(cl, nullptr, nullptr);

    Rgba overlap = pixelAt(fx.buf, fx.w, 25, 25);
    CHECK(approxByte(overlap.r, 255));
    CHECK(approxByte(overlap.g, 128));
    CHECK(approxByte(overlap.b, 128));
}

TEST_CASE("Layer: Multiply blend over a gray backdrop") {
    Fixture fx;
    fx.renderer.clear(Color{0.6f, 0.6f, 0.6f, 1});

    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(fx.w), static_cast<float>(fx.h)};
    cl.devicePixelRatio = 1;
    addPushLayer(cl, 1.0f, BlendMode::Multiply);
    addFillRect(cl, 0, 0, static_cast<float>(fx.w), static_cast<float>(fx.h), Color{0.5f, 0.5f, 0.5f, 1});
    addPopLayer(cl);
    fx.renderer.render(cl, nullptr, nullptr);

    Rgba p = pixelAt(fx.buf, fx.w, 32, 32);
    CHECK(approxByte(p.r, static_cast<int>(std::lround(0.6 * 0.5 * 255))));
}

TEST_CASE("Mask: Alpha mode, circle over square, inside colored, outside transparent") {
    Fixture fx;
    fx.renderer.clear(Color{0, 0, 0, 0});

    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(fx.w), static_cast<float>(fx.h)};
    cl.devicePixelRatio = 1;
    addPushMask(cl, MaskMode::Alpha);
    addFillPath(cl, addCirclePath(cl, 32, 32, 15), Color{1, 1, 1, 1});
    addEndMaskContent(cl);
    addFillRect(cl, 0, 0, static_cast<float>(fx.w), static_cast<float>(fx.h), Color{0, 0, 1, 1});
    addPopMask(cl);
    fx.renderer.render(cl, nullptr, nullptr);

    Rgba inside = pixelAt(fx.buf, fx.w, 32, 32);
    Rgba outside = pixelAt(fx.buf, fx.w, 2, 2);
    CHECK(inside.a > 200);
    CHECK(approxByte(inside.b, 255));
    CHECK(outside.a == 0);
}

TEST_CASE("Mask: AlphaInverted mode is the opposite of Alpha") {
    Fixture fx;
    fx.renderer.clear(Color{0, 0, 0, 0});

    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(fx.w), static_cast<float>(fx.h)};
    cl.devicePixelRatio = 1;
    addPushMask(cl, MaskMode::AlphaInverted);
    addFillPath(cl, addCirclePath(cl, 32, 32, 15), Color{1, 1, 1, 1});
    addEndMaskContent(cl);
    addFillRect(cl, 0, 0, static_cast<float>(fx.w), static_cast<float>(fx.h), Color{0, 0, 1, 1});
    addPopMask(cl);
    fx.renderer.render(cl, nullptr, nullptr);

    Rgba inside = pixelAt(fx.buf, fx.w, 32, 32);
    Rgba outside = pixelAt(fx.buf, fx.w, 2, 2);
    CHECK(inside.a == 0);
    CHECK(outside.a > 200);
}

TEST_CASE("Mask: Luma mode, white mask half opaque, black mask half transparent") {
    Fixture fx;
    fx.renderer.clear(Color{0, 0, 0, 0});

    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(fx.w), static_cast<float>(fx.h)};
    cl.devicePixelRatio = 1;
    addPushMask(cl, MaskMode::Luma);
    addFillRect(cl, 0, 0, 32, 64, Color{1, 1, 1, 1});   // left half: white -> luma 1
    addFillRect(cl, 32, 0, 32, 64, Color{0, 0, 0, 1});  // right half: black -> luma 0
    addEndMaskContent(cl);
    addFillRect(cl, 0, 0, static_cast<float>(fx.w), static_cast<float>(fx.h), Color{1, 0, 0, 1});
    addPopMask(cl);
    fx.renderer.render(cl, nullptr, nullptr);

    Rgba left = pixelAt(fx.buf, fx.w, 8, 32);
    Rgba right = pixelAt(fx.buf, fx.w, 56, 32);
    CHECK(left.a > 200);
    CHECK(right.a == 0);
}

TEST_CASE("Mask: nested Mask (Intersect semantics), content visible only where both shapes overlap") {
    Fixture fx;
    fx.renderer.clear(Color{0, 0, 0, 0});

    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(fx.w), static_cast<float>(fx.h)};
    cl.devicePixelRatio = 1;
    addPushMask(cl, MaskMode::Alpha);
    addFillRect(cl, 0, 0, 40, 64, Color{1, 1, 1, 1});  // shape A: x in [0, 40)
    addEndMaskContent(cl);
    addPushMask(cl, MaskMode::Intersect);
    addFillRect(cl, 24, 0, 40, 64, Color{1, 1, 1, 1});  // shape B: x in [24, 64)
    addEndMaskContent(cl);
    addFillRect(cl, 0, 0, static_cast<float>(fx.w), static_cast<float>(fx.h), Color{0, 1, 0, 1});
    addPopMask(cl);  // inner: masked by B
    addPopMask(cl);  // outer: masked by A
    fx.renderer.render(cl, nullptr, nullptr);

    Rgba both = pixelAt(fx.buf, fx.w, 32, 32);   // in [24,40): inside both A and B
    Rgba onlyA = pixelAt(fx.buf, fx.w, 8, 32);   // in [0,24): only A
    Rgba onlyB = pixelAt(fx.buf, fx.w, 56, 32);  // in [40,64): only B
    CHECK(both.a > 200);
    CHECK(onlyA.a == 0);
    CHECK(onlyB.a == 0);
}

TEST_CASE("Layer: clip stays in effect for fills issued inside a Layer") {
    Fixture fx;
    fx.renderer.clear(Color{0, 0, 0, 1});

    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(fx.w), static_cast<float>(fx.h)};
    cl.devicePixelRatio = 1;
    addPushClipRect(cl, Rect{0, 0, 32, 64});
    addPushLayer(cl, 1.0f, BlendMode::Normal);
    addFillRect(cl, 0, 0, static_cast<float>(fx.w), static_cast<float>(fx.h), Color{1, 0, 0, 1});
    addPopLayer(cl);
    addPopClip(cl);
    fx.renderer.render(cl, nullptr, nullptr);

    Rgba left = pixelAt(fx.buf, fx.w, 8, 32);
    Rgba right = pixelAt(fx.buf, fx.w, 48, 32);
    CHECK(approxByte(left.r, 255));
    CHECK(right.r == 0);  // outside the clip rect, the layer's content never reached here
}

TEST_CASE("LayerStack: 20 consecutive Push/Pop cycles do not keep growing the surface pool") {
    LayerStack stack;
    std::vector<uint8_t> rootBuf(static_cast<size_t>(8) * 8 * 4, 0);
    SwTarget swTarget{rootBuf.data(), 8, 8, 32};
    Target root = Target::fromSwTarget(swTarget);

    stack.pushLayer(root, 1.0f, BlendMode::Normal, Rect{0, 0, 8, 8});
    stack.popLayer(root);
    size_t steadyState = stack.poolSize();
    REQUIRE(steadyState > 0);

    for (int i = 0; i < 20; ++i) {
        stack.pushLayer(root, 1.0f, BlendMode::Normal, Rect{0, 0, 8, 8});
        stack.popLayer(root);
        CHECK(stack.poolSize() == steadyState);
    }
}

// T-263a E7: resetting the stack (what SwRenderer::render now does first, and setTarget did
// before) leaves it empty and hands every surface the dropped frames held back to the pool, so a
// list that once left layers open cannot make the pool grow on every later frame.
TEST_CASE("LayerStack: T-263a resetStack empties the stack and frees every pooled surface") {
    LayerStack stack;
    std::vector<uint8_t> rootBuf(static_cast<size_t>(8) * 8 * 4, 0);
    SwTarget swTarget{rootBuf.data(), 8, 8, 32};
    Target root = Target::fromSwTarget(swTarget);

    stack.pushLayer(root, 1.0f, BlendMode::Normal, Rect{0, 0, 8, 8});
    stack.pushMask(root, MaskMode::Alpha);
    stack.endMaskContent(root);
    REQUIRE(!stack.empty());
    const size_t held = stack.poolSize();
    REQUIRE(held == 3);

    for (int frame = 0; frame < 4; ++frame) {
        stack.resetStack();
        CHECK(stack.empty());
        CHECK(stack.currentTarget(root).u8 == root.u8);
        // The same unbalanced frame again reuses the three released surfaces.
        stack.pushLayer(root, 1.0f, BlendMode::Normal, Rect{0, 0, 8, 8});
        stack.pushMask(root, MaskMode::Alpha);
        stack.endMaskContent(root);
        CHECK(stack.poolSize() == held);
    }
}

TEST_CASE("LayerStack: a bounded layer composites at its origin and nowhere else (T-133)") {
    LayerStack stack;
    constexpr int rw = 160;
    constexpr int rh = 120;
    std::vector<uint8_t> rootBuf(static_cast<size_t>(rw) * rh * 4, 0);
    SwTarget swTarget{rootBuf.data(), rw, rh, rw * 4};
    Target root = Target::fromSwTarget(swTarget);

    stack.pushLayer(root, 0.5f, BlendMode::Normal, Rect{100, 50, 40, 40});
    Target layer = stack.currentTarget(root);
    CHECK(layer.originX == 100);
    CHECK(layer.originY == 50);
    CHECK(layer.w == 40);
    CHECK(layer.h == 40);
    // Draws keep addressing full-viewport device coordinates; the Target remaps them.
    for (int y = 50; y < 90; ++y) {
        for (int x = 100; x < 140; ++x) {
            layer.set(x, y, Color{1, 0, 0, 1});
        }
    }
    stack.popLayer(root);
    CHECK(stack.empty());

    Rgba inside = pixelAt(rootBuf, rw, 120, 70);
    CHECK(approxByte(inside.r, 255));
    CHECK(approxByte(inside.a, 128));
    for (const Rgba& outside : {pixelAt(rootBuf, rw, 99, 70), pixelAt(rootBuf, rw, 140, 70),
                                pixelAt(rootBuf, rw, 120, 49), pixelAt(rootBuf, rw, 120, 90)}) {
        CHECK(outside.r == 0);
        CHECK(outside.a == 0);
    }
}

TEST_CASE("LayerStack: an empty boundsHint still balances push/pop and writes nothing (T-133)") {
    LayerStack stack;
    constexpr int rw = 16;
    constexpr int rh = 16;
    std::vector<uint8_t> rootBuf(static_cast<size_t>(rw) * rh * 4, 0);
    SwTarget swTarget{rootBuf.data(), rw, rh, rw * 4};
    Target root = Target::fromSwTarget(swTarget);

    stack.pushLayer(root, 1.0f, BlendMode::Normal, Rect{0, 0, 0, 0});
    CHECK(!stack.empty());
    Target layer = stack.currentTarget(root);
    CHECK(layer.w == 1);
    CHECK(layer.h == 1);
    stack.popLayer(root);
    CHECK(stack.empty());
    for (uint8_t byte : rootBuf) {
        CHECK(byte == 0);
    }
}

TEST_CASE("Clip: a popped clip's pooled mask is zeroed, so the next clip sees only its own box (T-133)") {
    Fixture fx;

    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(fx.w), static_cast<float>(fx.h)};
    cl.devicePixelRatio = 1;
    addPushClipRect(cl, Rect{0, 0, 16, 16});
    addFillRect(cl, 0, 0, static_cast<float>(fx.w), static_cast<float>(fx.h), Color{1, 0, 0, 1});
    addPopClip(cl);
    // Reuses the first clip's buffer from the mask pool; any coverage left over from it would show
    // up as blue inside the *first* rectangle.
    addPushClipRect(cl, Rect{32, 32, 16, 16});
    addFillRect(cl, 0, 0, static_cast<float>(fx.w), static_cast<float>(fx.h), Color{0, 0, 1, 1});
    addPopClip(cl);
    fx.renderer.render(cl, nullptr, nullptr);

    Rgba first = pixelAt(fx.buf, fx.w, 8, 8);
    CHECK(approxByte(first.r, 255));
    CHECK(first.b == 0);
    Rgba second = pixelAt(fx.buf, fx.w, 40, 40);
    CHECK(approxByte(second.b, 255));
    CHECK(second.r == 0);
    for (const Rgba& outside : {pixelAt(fx.buf, fx.w, 8, 40), pixelAt(fx.buf, fx.w, 40, 8)}) {
        CHECK(outside.a == 0);
    }
}

TEST_CASE("Clip: a nested clip's box is the intersection, and outside it the mask is zero (T-133)") {
    Fixture fx;

    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(fx.w), static_cast<float>(fx.h)};
    cl.devicePixelRatio = 1;
    addPushClipRect(cl, Rect{0, 0, 32, 64});
    addPushClipRect(cl, Rect{16, 0, 32, 64});
    addFillRect(cl, 0, 0, static_cast<float>(fx.w), static_cast<float>(fx.h), Color{0, 1, 0, 1});
    addPopClip(cl);
    addPopClip(cl);
    fx.renderer.render(cl, nullptr, nullptr);

    CHECK(approxByte(pixelAt(fx.buf, fx.w, 24, 32).g, 255));  // inside both
    CHECK(pixelAt(fx.buf, fx.w, 8, 32).a == 0);               // only the outer one
    CHECK(pixelAt(fx.buf, fx.w, 40, 32).a == 0);              // only the inner one
}
