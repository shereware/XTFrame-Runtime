#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "doctest.h"
#include "gl_device.h"
#include "gl_fbo.h"
#include "gl_loader.h"
#include "golden_common.h"
#include "test_temp.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/compile/ir_writer.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/gl/gl_renderer.h"
#include "xt/ir.h"
#include "xt/node_arena.h"
#include "xt/painter.h"
#include "xt/scene_instance.h"
#include "xt/sw/sw_png.h"
#include "xt/types.h"

/// GlRenderer tests: every TEST_CASE renders into its own 128x128 offscreen FBO (RGBA8 +
/// GL_DEPTH24_STENCIL8, no MSAA, per docs/tasks/T-19.md "測試") and reads it back with
/// glReadPixels for comparison. main() (test_gl_main.cpp) guarantees a current GL context exists
/// before any TEST_CASE runs (otherwise the whole binary exits 0 without running doctest at all).
///
/// Two ways of producing a CommandList are used: most tests go through the real
/// Painter/SceneBuilder pipeline (reusing xt::golden's PathSpec/addPath/addNode helpers, exactly
/// like tests/golden/fixtures/*.cpp) so the exercised CommandList matches what the SW backend
/// would also receive; the clip and image tests build a CommandList by hand because Painter does
/// not emit PushClipRect/PushClipPath yet (not implemented as of this task) and no golden fixture
/// exercises DrawImage.

using namespace xt;

namespace {

// -------------------------------------------------------------------------------------------
// Offscreen render target + readback.
// -------------------------------------------------------------------------------------------

struct GlTestTarget {
    gl::GLuint fbo = 0, color = 0, rbo = 0;
    int width = 0, height = 0;

    void init(int w, int h) {
        width = w;
        height = h;
        using namespace xt::gl;
        glGenTextures(1, &color);
        glBindTexture(GL_TEXTURE_2D, color);
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA8), w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_NEAREST));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_NEAREST));

        glGenRenderbuffers(1, &rbo);
        glBindRenderbuffer(GL_RENDERBUFFER, rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);

        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo);
        REQUIRE(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    }

    void bindAndClear(Color c) {
        using namespace xt::gl;
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, width, height);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(c.r, c.g, c.b, c.a);
        glClearStencil(0);
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }

    std::vector<uint8_t> readback() const {
        using namespace xt::gl;
        std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        return pixels;
    }

    void destroy() {
        using namespace xt::gl;
        if (fbo != 0) glDeleteFramebuffers(1, &fbo);
        if (rbo != 0) glDeleteRenderbuffers(1, &rbo);
        if (color != 0) glDeleteTextures(1, &color);
        fbo = rbo = color = 0;
    }
};

void* sdlLoadProc(const char* name) { return reinterpret_cast<void*>(SDL_GL_GetProcAddress(name)); }

/// Reads one pixel (device space, y-down, matching xt's own coordinate convention -- note
/// glReadPixels itself returns rows bottom-to-top, so this flips y for the caller's convenience).
struct Px {
    uint8_t r, g, b, a;
};
Px pixelAt(const std::vector<uint8_t>& rgba, int width, int height, int x, int y) {
    int flippedY = height - 1 - y;
    size_t idx = (static_cast<size_t>(flippedY) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
    return Px{rgba[idx], rgba[idx + 1], rgba[idx + 2], rgba[idx + 3]};
}

void expectApprox(Px got, uint8_t r, uint8_t g, uint8_t b, uint8_t a, int tol = 16) {
    CHECK(std::abs(static_cast<int>(got.r) - r) <= tol);
    CHECK(std::abs(static_cast<int>(got.g) - g) <= tol);
    CHECK(std::abs(static_cast<int>(got.b) - b) <= tol);
    CHECK(std::abs(static_cast<int>(got.a) - a) <= tol);
}

// -------------------------------------------------------------------------------------------
// Scene -> CommandList (Painter pipeline), matching tests/golden/golden_main.cpp's renderFixture.
// -------------------------------------------------------------------------------------------

CommandList buildFromScene(const std::function<void(compile::SceneBuilder&, compile::IrWriter&)>& build,
                           int w, int h) {
    compile::IrWriter writer;
    compile::SceneBuilder scene;
    scene.width = static_cast<float>(w);
    scene.height = static_cast<float>(h);
    build(scene, writer);
    writer.addScene(scene);
    std::vector<uint8_t> bytes = writer.finish();

    Result<IrView> viewResult = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(viewResult.ok());
    const IrView& view = viewResult.value();

    NodeArena arena;
    NodeId container = arena.create(NodeKind::Group);
    SceneInstance inst;
    REQUIRE(instantiateScene(view, 0, arena, container, inst) == Status::kOk);
    updateWorldTransforms(arena, inst.root, true);

    Painter painter;
    CommandList cmds;
    painter.paint(arena, inst.root, cmds);
    cmds.viewport = Rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
    cmds.devicePixelRatio = 1.0f;
    return cmds;
}

std::vector<uint8_t> renderCommands(const CommandList& cmds, int w, int h, Color clearColor,
                                    AssetResolver* assets = nullptr, const IrView* ir = nullptr) {
    static bool loaded = false;
    if (!loaded) {
        REQUIRE(xt::gl::loadGl(sdlLoadProc));
        loaded = true;
    }
    GlTestTarget target;
    target.init(w, h);
    target.bindAndClear(clearColor);

    gl::GlRenderer renderer;
    REQUIRE(renderer.init(sdlLoadProc));
    renderer.resize(w, h);
    target.bindAndClear(clearColor);  // resize()/init() may have touched GL state; re-clear.
    renderer.render(cmds, nullptr, assets, ir);
    std::vector<uint8_t> pixels = target.readback();
    renderer.shutdown();
    target.destroy();
    return pixels;
}

// -------------------------------------------------------------------------------------------
// Hand-built CommandList helpers, for the clip/image tests Painter can't produce yet.
// -------------------------------------------------------------------------------------------

PathRef addRectPath(CommandList& list, Rect r) {
    uint32_t vOff = static_cast<uint32_t>(list.verbs.size());
    uint32_t pOff = static_cast<uint32_t>(list.points.size());
    list.verbs.push_back(PathVerb::Move);
    list.verbs.push_back(PathVerb::Line);
    list.verbs.push_back(PathVerb::Line);
    list.verbs.push_back(PathVerb::Line);
    list.verbs.push_back(PathVerb::Close);
    float pts[8] = {r.x, r.y, r.x + r.w, r.y, r.x + r.w, r.y + r.h, r.x, r.y + r.h};
    for (float f : pts) list.points.push_back(f);
    return PathRef{vOff, 5, pOff, 8};
}

void addFillRect(CommandList& list, Rect r, Color color, FillRule rule = FillRule::NonZero) {
    PathRef path = addRectPath(list, r);
    FillCmd fill;
    fill.xf = Mat2x3{};
    fill.path = path;
    fill.paint = PaintRef{PaintKind::Solid, color, Vec2{}, Vec2{}, Vec2{}, 0, 0};
    fill.rule = rule;
    fill.antialias = 1;
    list.commands.push_back(Command{CmdKind::FillPath, {}, static_cast<uint32_t>(list.fills.size())});
    list.fills.push_back(fill);
}

void addPushClipRect(CommandList& list, Rect r) {
    ClipRectCmd clip;
    clip.xf = Mat2x3{};
    clip.rect = r;
    list.commands.push_back(Command{CmdKind::PushClipRect, {}, static_cast<uint32_t>(list.clipRects.size())});
    list.clipRects.push_back(clip);
}

void addPopClip(CommandList& list) { list.commands.push_back(Command{CmdKind::PopClip, {}, 0}); }

}  // namespace

// =================================================================================================
// Solid fills.
// =================================================================================================

TEST_CASE("gl: solid rect NonZero") {
    CommandList cmds = buildFromScene(
        [](compile::SceneBuilder& scene, compile::IrWriter&) {
            golden::PathSpec rect;
            rect.verbs = {PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Line, PathVerb::Close};
            rect.points = {24, 24, 104, 24, 104, 104, 24, 104};
            rect.fillKind = PaintKind::Solid;
            rect.fillColor = Color{1, 0, 0, 1};
            uint32_t idx = golden::addPath(scene, rect);
            golden::addNode(scene, NodeKind::Path, golden::kRootParent, idx);
        },
        128, 128);

    std::vector<uint8_t> px = renderCommands(cmds, 128, 128, Color{0, 0, 0, 1});
    expectApprox(pixelAt(px, 128, 128, 64, 64), 255, 0, 0, 255);
    expectApprox(pixelAt(px, 128, 128, 5, 5), 0, 0, 0, 255);
}

TEST_CASE("gl: evenodd rings") {
    CommandList cmds = buildFromScene(
        [](compile::SceneBuilder& scene, compile::IrWriter&) {
            golden::PathSpec rings;
            golden::appendCircle(rings.verbs, rings.points, Vec2{64, 64}, 50.0f);
            golden::appendCircle(rings.verbs, rings.points, Vec2{64, 64}, 25.0f);
            rings.fillRule = FillRule::EvenOdd;
            rings.fillKind = PaintKind::Solid;
            rings.fillColor = Color{0, 1, 0, 1};
            uint32_t idx = golden::addPath(scene, rings);
            golden::addNode(scene, NodeKind::Path, golden::kRootParent, idx);
        },
        128, 128);

    std::vector<uint8_t> px = renderCommands(cmds, 128, 128, Color{0, 0, 0, 1});
    expectApprox(pixelAt(px, 128, 128, 64, 64 - 35), 0, 1 * 255, 0, 255);  // between rings: filled
    expectApprox(pixelAt(px, 128, 128, 64, 64), 0, 0, 0, 255);             // center hole: not filled
}

TEST_CASE("gl: T-165 a rotating squashed border ring keeps its hole") {
    // challenge-menu's #holo-ring (116 px circle, 2 px border, scaleY 0.26) spun through a sweep
    // of rotations: the direct ring path (T-155) must never fill the hole. Before T-165 a
    // rounding tie between the two contours' first vertices fanned the whole disc on ~8 % of
    // the angles (docs/tasks/T-165.md).
    const int w = 256, h = 128;
    const Vec2 centre{128, 64};
    int filled = 0;
    for (int deg = 0; deg < 360; deg += 3) {
        CommandList cmds;
        cmds.viewport = Rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
        cmds.devicePixelRatio = 1.0f;
        PathRef path{};
        path.verbOffset = static_cast<uint32_t>(cmds.verbs.size());
        path.pointOffset = static_cast<uint32_t>(cmds.points.size());
        golden::appendCircle(cmds.verbs, cmds.points, Vec2{0, 0}, 58.0f);
        golden::appendCircle(cmds.verbs, cmds.points, Vec2{0, 0}, 56.0f);
        path.verbCount = static_cast<uint32_t>(cmds.verbs.size()) - path.verbOffset;
        path.pointCount = static_cast<uint32_t>(cmds.points.size()) - path.pointOffset;
        FillCmd fill{};
        fill.xf =
            mul(translation(centre), mul(scaling(Vec2{1.0f, 0.26f}), rotationDeg(static_cast<float>(deg))));
        fill.path = path;
        fill.paint = PaintRef{PaintKind::Solid, Color{0, 1, 0, 1}, Vec2{}, Vec2{}, Vec2{}, 0, 0};
        fill.rule = FillRule::EvenOdd;
        fill.antialias = 1;
        cmds.commands.push_back(Command{CmdKind::FillPath, {}, 0});
        cmds.fills.push_back(fill);

        std::vector<uint8_t> px = renderCommands(cmds, w, h, Color{0, 0, 0, 1});
        // The band's widest point (left/right, 2 px) is drawn; the hole stays the clear colour.
        const Px band = pixelAt(px, w, h, 128 - 57, 64);
        const Px hole = pixelAt(px, w, h, 128, 64);
        const Px holeL = pixelAt(px, w, h, 128 - 30, 64);
        const Px holeR = pixelAt(px, w, h, 128 + 30, 64);
        if (band.g < 40 || hole.g > 8 || holeL.g > 8 || holeR.g > 8) {
            ++filled;
            MESSAGE("ring wrong at ", deg, " deg: band g=", int(band.g), " hole g=", int(hole.g), "/",
                    int(holeL.g), "/", int(holeR.g));
        }
    }
    CHECK(filled == 0);
}

TEST_CASE("gl: semi-transparent overlap") {
    CommandList cmds = buildFromScene(
        [](compile::SceneBuilder& scene, compile::IrWriter&) {
            golden::PathSpec a;
            a.verbs = {PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Line, PathVerb::Close};
            a.points = {20, 20, 90, 20, 90, 90, 20, 90};
            a.fillKind = PaintKind::Solid;
            a.fillColor = Color{1, 0, 0, 0.5f};
            uint32_t idxA = golden::addPath(scene, a);

            golden::PathSpec b;
            b.verbs = {PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Line, PathVerb::Close};
            b.points = {40, 40, 110, 40, 110, 110, 40, 110};
            b.fillKind = PaintKind::Solid;
            b.fillColor = Color{0, 0, 1, 0.5f};
            uint32_t idxB = golden::addPath(scene, b);

            uint32_t root = golden::addNode(scene, NodeKind::Group, golden::kRootParent);
            golden::addNode(scene, NodeKind::Path, root, idxA);
            golden::addNode(scene, NodeKind::Path, root, idxB);
        },
        128, 128);

    std::vector<uint8_t> px = renderCommands(cmds, 128, 128, Color{0, 0, 0, 1});
    // Overlap region: red drawn first (over black), then blue over that with src-over blending.
    Px got = pixelAt(px, 128, 128, 65, 65);
    CHECK(got.b > got.r);  // blue's later half-alpha draw should dominate over the reds's half.
}

TEST_CASE("gl: rotated square") {
    CommandList cmds = buildFromScene(
        [](compile::SceneBuilder& scene, compile::IrWriter&) {
            golden::PathSpec sq;
            sq.verbs = {PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Line, PathVerb::Close};
            sq.points = {-20, -20, 20, -20, 20, 20, -20, 20};
            sq.fillKind = PaintKind::Solid;
            sq.fillColor = Color{1, 1, 0, 1};
            uint32_t idx = golden::addPath(scene, sq);
            golden::NodeOpts opts;
            opts.translate = Vec2{64, 64};
            opts.rotation = 45.0f;
            golden::addNode(scene, NodeKind::Path, golden::kRootParent, idx, opts);
        },
        128, 128);

    std::vector<uint8_t> px = renderCommands(cmds, 128, 128, Color{0, 0, 0, 1});
    expectApprox(pixelAt(px, 128, 128, 64, 64), 255, 255, 0, 255);
    // A corner of the *unrotated* square's bounding box should now be outside the rotated shape.
    expectApprox(pixelAt(px, 128, 128, 64 + 18, 64 + 18), 0, 0, 0, 255);
}

// =================================================================================================
// Clip (hand-built CommandList; Painter does not emit clip commands, see file header comment).
// =================================================================================================

TEST_CASE("gl: clip rect inside/outside") {
    CommandList list;
    list.viewport = Rect{0, 0, 128, 128};
    list.devicePixelRatio = 1.0f;
    addPushClipRect(list, Rect{0, 0, 64, 128});
    addFillRect(list, Rect{0, 0, 128, 128}, Color{0, 1, 0, 1});
    addPopClip(list);

    std::vector<uint8_t> px = renderCommands(list, 128, 128, Color{0, 0, 0, 1});
    expectApprox(pixelAt(px, 128, 128, 10, 64), 0, 255, 0, 255);
    expectApprox(pixelAt(px, 128, 128, 100, 64), 0, 0, 0, 255);
}

TEST_CASE("gl: nested clip") {
    CommandList list;
    list.viewport = Rect{0, 0, 128, 128};
    list.devicePixelRatio = 1.0f;
    addPushClipRect(list, Rect{0, 0, 96, 96});
    addPushClipRect(list, Rect{32, 32, 96, 96});
    addFillRect(list, Rect{0, 0, 128, 128}, Color{1, 0, 1, 1});
    addPopClip(list);
    addPopClip(list);

    std::vector<uint8_t> px = renderCommands(list, 128, 128, Color{0, 0, 0, 1});
    expectApprox(pixelAt(px, 128, 128, 64, 64), 255, 0, 255, 255);  // in both rects
    expectApprox(pixelAt(px, 128, 128, 16, 16), 0, 0, 0, 255);      // only in the first rect
    expectApprox(pixelAt(px, 128, 128, 100, 100), 0, 0, 0, 255);    // only in the second rect
}

TEST_CASE("gl: PopClip restores") {
    CommandList list;
    list.viewport = Rect{0, 0, 128, 128};
    list.devicePixelRatio = 1.0f;
    addPushClipRect(list, Rect{0, 0, 32, 32});
    addFillRect(list, Rect{0, 0, 128, 128}, Color{1, 0, 0, 1});
    addPopClip(list);
    addFillRect(list, Rect{0, 0, 128, 128}, Color{0, 1, 0, 1});

    std::vector<uint8_t> px = renderCommands(list, 128, 128, Color{0, 0, 0, 1});
    // After PopClip, the second fill is unclipped and covers everywhere, including where the
    // first (clipped) red fill did and did not reach.
    expectApprox(pixelAt(px, 128, 128, 10, 10), 0, 255, 0, 255);
    expectApprox(pixelAt(px, 128, 128, 100, 100), 0, 255, 0, 255);
}

// T-263b: GlRenderer draws through gpu::FrameBuilder, so the list validation and the clip depth
// limit (xt::kMaxClipDepth) are the builder's; these pin them down through the GL entry point.
TEST_CASE("gl: T-263b an invalid command list draws nothing and is reported") {
    CommandList list;
    list.viewport = Rect{0, 0, 64, 64};
    list.devicePixelRatio = 1.0f;
    addFillRect(list, Rect{0, 0, 64, 64}, Color{1, 0, 0, 1});
    SUBCASE("a command index past its pool") {
        list.commands.push_back(Command{CmdKind::FillPath, {}, 1000});
    }
    SUBCASE("a PopClip with nothing pushed") { addPopClip(list); }
    SUBCASE("a clip left open") { addPushClipRect(list, Rect{0, 0, 32, 32}); }

    REQUIRE(xt::gl::loadGl(sdlLoadProc));
    GlTestTarget target;
    target.init(64, 64);
    gl::GlRenderer renderer;
    REQUIRE(renderer.init(sdlLoadProc));
    renderer.resize(64, 64);
    target.bindAndClear(Color{0, 0, 0, 1});
    renderer.render(list, nullptr, nullptr, nullptr);
    CHECK(std::string(renderer.lastError()).find("invalid command list") != std::string::npos);
    std::vector<uint8_t> px = target.readback();
    expectApprox(pixelAt(px, 64, 64, 32, 32), 0, 0, 0, 255);  // not even the valid fill
    // The next valid list draws as usual.
    CommandList good;
    good.viewport = Rect{0, 0, 64, 64};
    good.devicePixelRatio = 1.0f;
    addFillRect(good, Rect{0, 0, 64, 64}, Color{0, 1, 0, 1});
    target.bindAndClear(Color{0, 0, 0, 1});
    renderer.render(good, nullptr, nullptr, nullptr);
    CHECK(std::string(renderer.lastError()).empty());
    px = target.readback();
    expectApprox(pixelAt(px, 64, 64, 32, 32), 0, 255, 0, 255);
    renderer.shutdown();
    target.destroy();
}

TEST_CASE("gl: T-263b a 16th nested stencil clip is cut to its bounding box and the pops stay balanced") {
    CommandList list;
    list.viewport = Rect{0, 0, 128, 128};
    list.devicePixelRatio = 1.0f;
    // Rotated squares are stencil clips; the 16th is past xt::kMaxClipDepth.
    const Mat2x3 xf = mul(translation(Vec2{64, 64}), rotationDeg(10.0f));
    const uint32_t kNested = kMaxClipDepth + 1;
    float half = 0;
    for (uint32_t i = 0; i < kNested; ++i) {
        half = 62.0f - 3.0f * static_cast<float>(i);
        addPushClipRect(list, Rect{-half, -half, 2 * half, 2 * half});
        list.clipRects.back().xf = xf;
    }
    addFillRect(list, Rect{0, 0, 128, 128}, Color{1, 0, 0, 1});
    for (uint32_t i = 0; i < kNested; ++i) {
        addPopClip(list);
    }
    addFillRect(list, Rect{0, 0, 8, 8}, Color{0, 1, 0, 1});

    std::vector<uint8_t> px = renderCommands(list, 128, 128, Color{0, 0, 0, 1});
    expectApprox(pixelAt(px, 128, 128, 64, 64), 255, 0, 0, 255);
    // Just inside a corner of the 15th square (half = 20): inside every stencil level, but outside
    // the 16th square's (half = 17) bounding box, to which that clip is reduced.
    const float outer = half + 3.0f;
    const Vec2 corner = apply(xf, Vec2{outer - 1.5f, outer - 1.5f});
    Vec2 lo{1e9f, 1e9f}, hi{-1e9f, -1e9f};
    for (Vec2 c : {Vec2{-half, -half}, Vec2{half, -half}, Vec2{half, half}, Vec2{-half, half}}) {
        const Vec2 p = apply(xf, c);
        lo = Vec2{std::min(lo.x, p.x), std::min(lo.y, p.y)};
        hi = Vec2{std::max(hi.x, p.x), std::max(hi.y, p.y)};
    }
    REQUIRE((corner.x > hi.x + 1.0f || corner.x < lo.x - 1.0f || corner.y > hi.y + 1.0f ||
             corner.y < lo.y - 1.0f));
    expectApprox(pixelAt(px, 128, 128, static_cast<int>(corner.x), static_cast<int>(corner.y)), 0, 0, 0, 255);
    // Every PopClip paired: the fill after them is not clipped (and nothing else was touched).
    expectApprox(pixelAt(px, 128, 128, 4, 4), 0, 255, 0, 255);
    expectApprox(pixelAt(px, 128, 128, 120, 4), 0, 0, 0, 255);
}

// =================================================================================================
// Layer / Mask.
// =================================================================================================

TEST_CASE("gl: layer opacity overlap unchanged") {
    CommandList cmds = buildFromScene(
        [](compile::SceneBuilder& scene, compile::IrWriter&) {
            golden::PathSpec a;
            a.verbs = {PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Line, PathVerb::Close};
            a.points = {20, 20, 80, 20, 80, 80, 20, 80};
            a.fillKind = PaintKind::Solid;
            a.fillColor = Color{1, 0, 0, 1};
            uint32_t idxA = golden::addPath(scene, a);

            golden::PathSpec b;
            b.verbs = {PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Line, PathVerb::Close};
            b.points = {50, 50, 110, 50, 110, 110, 50, 110};
            b.fillKind = PaintKind::Solid;
            b.fillColor = Color{1, 0, 0, 1};
            uint32_t idxB = golden::addPath(scene, b);

            golden::NodeOpts groupOpts;
            groupOpts.opacity = 0.5f;
            uint32_t group = golden::addNode(scene, NodeKind::Group, golden::kRootParent, 0, groupOpts);
            golden::addNode(scene, NodeKind::Path, group, idxA);
            golden::addNode(scene, NodeKind::Path, group, idxB);
        },
        128, 128);

    std::vector<uint8_t> px = renderCommands(cmds, 128, 128, Color{0, 0, 0, 1});
    // docs/tasks/T-19.md: overlapping shapes inside one layer must not double-darken/lighten
    // relative to the layer's own opacity -- the overlap region should look the same as either
    // square alone once the whole layer's opacity is applied (not the two blended twice).
    Px overlap = pixelAt(px, 128, 128, 65, 65);
    Px single = pixelAt(px, 128, 128, 25, 25);
    expectApprox(overlap, single.r, single.g, single.b, single.a, 20);
}

TEST_CASE("gl: multiply layer over grey") {
    CommandList cmds = buildFromScene(
        [](compile::SceneBuilder& scene, compile::IrWriter&) {
            golden::PathSpec bg;
            bg.verbs = {PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Line, PathVerb::Close};
            bg.points = {0, 0, 128, 0, 128, 128, 0, 128};
            bg.fillKind = PaintKind::Solid;
            bg.fillColor = Color{0.6f, 0.6f, 0.6f, 1};
            uint32_t idxBg = golden::addPath(scene, bg);

            golden::PathSpec sq;
            sq.verbs = {PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Line, PathVerb::Close};
            sq.points = {30, 30, 100, 30, 100, 100, 30, 100};
            sq.fillKind = PaintKind::Solid;
            sq.fillColor = Color{0.5f, 0.5f, 0.5f, 1};
            uint32_t idxSq = golden::addPath(scene, sq);

            uint32_t root = golden::addNode(scene, NodeKind::Group, golden::kRootParent);
            golden::addNode(scene, NodeKind::Path, root, idxBg);
            golden::NodeOpts groupOpts;
            groupOpts.blend = static_cast<uint8_t>(BlendMode::Multiply);
            uint32_t group = golden::addNode(scene, NodeKind::Group, root, 0, groupOpts);
            golden::addNode(scene, NodeKind::Path, group, idxSq);
        },
        128, 128);

    std::vector<uint8_t> px = renderCommands(cmds, 128, 128, Color{0, 0, 0, 1});
    // 0.6 * 0.5 = 0.3 -> ~76/255.
    expectApprox(pixelAt(px, 128, 128, 64, 64), 76, 76, 76, 255, 24);
}

TEST_CASE("gl: mask alpha circle masks square") {
    CommandList cmds = buildFromScene(
        [](compile::SceneBuilder& scene, compile::IrWriter&) {
            golden::PathSpec maskCircle;
            golden::appendCircle(maskCircle.verbs, maskCircle.points, Vec2{64, 64}, 40.0f);
            maskCircle.fillKind = PaintKind::Solid;
            maskCircle.fillColor = Color{1, 1, 1, 1};
            uint32_t maskIdx = golden::addPath(scene, maskCircle);

            golden::PathSpec square;
            square.verbs = {PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Line, PathVerb::Close};
            square.points = {24, 24, 104, 24, 104, 104, 24, 104};
            square.fillKind = PaintKind::Solid;
            square.fillColor = Color{1, 0.5f, 0, 1};
            uint32_t squareIdx = golden::addPath(scene, square);

            uint32_t root = golden::addNode(scene, NodeKind::Group, golden::kRootParent);
            golden::NodeOpts maskOpts;
            maskOpts.maskMode = static_cast<uint8_t>(MaskMode::Alpha);
            uint32_t mask = golden::addNode(scene, NodeKind::Mask, root, 0, maskOpts);
            golden::addNode(scene, NodeKind::Path, mask, maskIdx);
            golden::addNode(scene, NodeKind::Path, root, squareIdx);
        },
        128, 128);

    std::vector<uint8_t> px = renderCommands(cmds, 128, 128, Color{0, 0, 0, 1});
    expectApprox(pixelAt(px, 128, 128, 64, 64), 255, 128, 0, 255);  // inside circle: visible
    expectApprox(pixelAt(px, 128, 128, 30, 30), 0, 0, 0, 255);      // in square, outside circle: masked out
}

// =================================================================================================
// Stroke.
// =================================================================================================

TEST_CASE("gl: stroke horizontal line width 4") {
    CommandList cmds = buildFromScene(
        [](compile::SceneBuilder& scene, compile::IrWriter&) {
            golden::PathSpec line;
            line.verbs = {PathVerb::Move, PathVerb::Line};
            line.points = {10, 64, 118, 64};
            line.fillKind = PaintKind::None;
            line.strokeKind = PaintKind::Solid;
            line.strokeColor = Color{1, 1, 1, 1};
            line.strokeWidth = 4;
            line.cap = LineCap::Butt;
            uint32_t idx = golden::addPath(scene, line);
            golden::addNode(scene, NodeKind::Path, golden::kRootParent, idx);
        },
        128, 128);

    std::vector<uint8_t> px = renderCommands(cmds, 128, 128, Color{0, 0, 0, 1});
    expectApprox(pixelAt(px, 128, 128, 64, 64), 255, 255, 255, 255);
    expectApprox(pixelAt(px, 128, 128, 64, 64 + 10), 0, 0, 0, 255);
}

TEST_CASE("gl: dash") {
    CommandList cmds = buildFromScene(
        [](compile::SceneBuilder& scene, compile::IrWriter&) {
            golden::PathSpec line;
            line.verbs = {PathVerb::Move, PathVerb::Line};
            line.points = {10, 64, 118, 64};
            line.fillKind = PaintKind::None;
            line.strokeKind = PaintKind::Solid;
            line.strokeColor = Color{1, 1, 1, 1};
            line.strokeWidth = 6;
            line.cap = LineCap::Butt;
            line.dashes = {12, 12};
            uint32_t idx = golden::addPath(scene, line);
            golden::addNode(scene, NodeKind::Path, golden::kRootParent, idx);
        },
        128, 128);

    std::vector<uint8_t> px = renderCommands(cmds, 128, 128, Color{0, 0, 0, 1});
    expectApprox(pixelAt(px, 128, 128, 12, 64), 255, 255, 255, 255);  // first "on" segment
    expectApprox(pixelAt(px, 128, 128, 24, 64), 0, 0, 0, 255);        // first "off" gap
}

// =================================================================================================
// Gradients.
// =================================================================================================

TEST_CASE("gl: linear gradient midpoint") {
    CommandList cmds = buildFromScene(
        [](compile::SceneBuilder& scene, compile::IrWriter&) {
            golden::PathSpec sq;
            sq.verbs = {PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Line, PathVerb::Close};
            sq.points = {0, 0, 128, 0, 128, 128, 0, 128};
            sq.fillKind = PaintKind::Linear;
            sq.fillP0 = Vec2{0, 64};
            sq.fillP1 = Vec2{128, 64};
            sq.fillStops = {
                IrGradientStop{0.0f, Color{1, 0, 0, 1}},
                IrGradientStop{1.0f, Color{0, 0, 1, 1}},
            };
            uint32_t idx = golden::addPath(scene, sq);
            golden::addNode(scene, NodeKind::Path, golden::kRootParent, idx);
        },
        128, 128);

    std::vector<uint8_t> px = renderCommands(cmds, 128, 128, Color{0, 0, 0, 1});
    expectApprox(pixelAt(px, 128, 128, 64, 64), 128, 0, 128, 255, 24);
    expectApprox(pixelAt(px, 128, 128, 4, 64), 255, 0, 0, 255, 24);
    expectApprox(pixelAt(px, 128, 128, 124, 64), 0, 0, 255, 255, 24);
}

TEST_CASE("gl: radial gradient") {
    CommandList cmds = buildFromScene(
        [](compile::SceneBuilder& scene, compile::IrWriter&) {
            golden::PathSpec sq;
            sq.verbs = {PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Line, PathVerb::Close};
            sq.points = {0, 0, 128, 0, 128, 128, 0, 128};
            sq.fillKind = PaintKind::Radial;
            sq.fillP0 = Vec2{64, 64};
            sq.fillP1 = Vec2{64 + 64, 64};
            sq.fillStops = {
                IrGradientStop{0.0f, Color{1, 1, 1, 1}},
                IrGradientStop{1.0f, Color{0, 0, 0, 1}},
            };
            uint32_t idx = golden::addPath(scene, sq);
            golden::addNode(scene, NodeKind::Path, golden::kRootParent, idx);
        },
        128, 128);

    std::vector<uint8_t> px = renderCommands(cmds, 128, 128, Color{0, 0, 0, 1});
    expectApprox(pixelAt(px, 128, 128, 64, 64), 255, 255, 255, 255, 24);
    expectApprox(pixelAt(px, 128, 128, 1, 64), 0, 0, 0, 255, 24);
}

// T-116: an axis-aligned elliptical radial gradient (PaintRef::p2 != p0) with a wide horizontal
// axis and a narrow vertical one. A hand-built CommandList is used (rather than
// golden::PathSpec/buildFromScene, whose fillP0/fillP1-only IrPathData path keeps the pre-T-116
// circle semantics per painter.cpp's makePaintRef -- see the T-116 report).
TEST_CASE("gl: elliptical radial gradient (PaintRef.p2)") {
    CommandList list;
    list.viewport = Rect{0, 0, 128, 128};
    list.devicePixelRatio = 1.0f;
    PathRef path = addRectPath(list, Rect{0, 0, 128, 128});
    list.stops.push_back(GradientStop{0.0f, Color{1, 1, 1, 1}});
    list.stops.push_back(GradientStop{1.0f, Color{0, 0, 0, 1}});
    FillCmd fill;
    fill.xf = Mat2x3{};
    fill.path = path;
    fill.paint.kind = PaintKind::Radial;
    fill.paint.color = Color{1, 1, 1, 1};
    fill.paint.p0 = Vec2{64, 64};
    fill.paint.p1 = Vec2{64 + 48, 64};  // rx = 48
    fill.paint.p2 = Vec2{64, 64 + 24};  // ry = 24
    fill.paint.stopOffset = 0;
    fill.paint.stopCount = 2;
    fill.rule = FillRule::NonZero;
    fill.antialias = 1;
    list.fills.push_back(fill);
    list.commands.push_back(Command{CmdKind::FillPath, {}, 0});

    std::vector<uint8_t> px = renderCommands(list, 128, 128, Color{0, 0, 0, 1});
    // 30px right of center: within rx (48) -- partway through the gradient, not fully white/black.
    Px right = pixelAt(px, 128, 128, 64 + 30, 64);
    CHECK(right.r < 255);
    CHECK(right.r > 0);
    // 30px below center: beyond ry (24) -- clamped to the gradient's last stop (black), unlike a
    // circle of radius 48 (the pre-T-116 behavior), which would still be partway through.
    expectApprox(pixelAt(px, 128, 128, 64, 64 + 30), 0, 0, 0, 255, 24);
}

// =================================================================================================
// Image.
// =================================================================================================

namespace {

/// A trivial AssetResolver that always returns a fixed 4x4 checkerboard image (no IrView lookups
/// needed by this test), matching xt::ImageDecodeFn's contract loosely.
class CheckerboardResolver : public AssetResolver {
public:
    bool readBytes(std::string_view, std::vector<uint8_t>&) override { return false; }
    bool decodeImage(AssetId, const IrView&, ImageData& out) override {
        out.width = 4;
        out.height = 4;
        out.rgba.resize(4 * 4 * 4);
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                uint8_t v = ((x + y) % 2 == 0) ? 255 : 0;
                size_t i = (static_cast<size_t>(y) * 4 + static_cast<size_t>(x)) * 4;
                out.rgba[i + 0] = v;
                out.rgba[i + 1] = v;
                out.rgba[i + 2] = v;
                out.rgba[i + 3] = 255;
            }
        }
        return true;
    }
    Vec2 imageSize(AssetId, const IrView&) override { return Vec2{4, 4}; }
};

}  // namespace

TEST_CASE("gl: image checkerboard nearest") {
    CommandList list;
    list.viewport = Rect{0, 0, 8, 8};
    list.devicePixelRatio = 1.0f;
    ImageCmd img;
    img.xf = Mat2x3{};
    img.asset = 0;
    img.dst = Rect{0, 0, 8, 8};
    img.srcUv = Rect{0, 0, 1, 1};
    img.opacity = 1.0f;
    img.smooth = 0;
    list.images.push_back(img);
    list.commands.push_back(Command{CmdKind::DrawImage, {}, 0});

    // decodeImage needs *some* IrView; open an empty-but-valid one via a minimal IrWriter blob.
    compile::IrWriter writer;
    compile::SceneBuilder scene;
    scene.width = 8;
    scene.height = 8;
    writer.addScene(scene);
    std::vector<uint8_t> bytes = writer.finish();
    Result<IrView> viewResult = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(viewResult.ok());

    CheckerboardResolver resolver;
    std::vector<uint8_t> px = renderCommands(list, 8, 8, Color{0, 0, 0, 1}, &resolver, &viewResult.value());
    // Each source texel maps to a 2x2 device block with NEAREST sampling.
    expectApprox(pixelAt(px, 8, 8, 0, 0), 255, 255, 255, 255);
    expectApprox(pixelAt(px, 8, 8, 3, 0), 0, 0, 0, 255);
}

namespace {

/// T-215: one solid 2x2 image whose color the test switches between renders.
class SwitchableResolver : public AssetResolver {
public:
    uint8_t red = 255;
    uint8_t green = 0;
    bool readBytes(std::string_view, std::vector<uint8_t>&) override { return false; }
    bool decodeImage(AssetId, const IrView&, ImageData& out) override {
        out.width = 2;
        out.height = 2;
        out.rgba.clear();
        for (int i = 0; i < 4; ++i) {
            out.rgba.insert(out.rgba.end(), {red, green, 0, 255});
        }
        return true;
    }
    Vec2 imageSize(AssetId, const IrView&) override { return Vec2{2, 2}; }
};

}  // namespace

TEST_CASE("gl: T-215 invalidateImages uploads every cached image again") {
    CommandList list;
    list.viewport = Rect{0, 0, 8, 8};
    list.devicePixelRatio = 1.0f;
    ImageCmd img;
    img.xf = Mat2x3{};
    img.asset = 0;
    img.dst = Rect{0, 0, 8, 8};
    img.srcUv = Rect{0, 0, 1, 1};
    img.opacity = 1.0f;
    img.smooth = 0;
    list.images.push_back(img);
    list.commands.push_back(Command{CmdKind::DrawImage, {}, 0});
    compile::IrWriter writer;
    compile::SceneBuilder scene;
    scene.width = 8;
    scene.height = 8;
    writer.addScene(scene);
    std::vector<uint8_t> bytes = writer.finish();
    Result<IrView> viewResult = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(viewResult.ok());

    REQUIRE(xt::gl::loadGl(sdlLoadProc));
    GlTestTarget target;
    target.init(8, 8);
    gl::GlRenderer renderer;
    REQUIRE(renderer.init(sdlLoadProc));
    renderer.resize(8, 8);
    SwitchableResolver resolver;
    auto renderOnce = [&]() {
        target.bindAndClear(Color{0, 0, 0, 1});
        renderer.render(list, nullptr, &resolver, &viewResult.value());
        return target.readback();
    };
    expectApprox(pixelAt(renderOnce(), 8, 8, 4, 4), 255, 0, 0, 255);
    // Asset 0 now names another image (a new IR): the texture cached by id is stale...
    resolver.red = 0;
    resolver.green = 255;
    expectApprox(pixelAt(renderOnce(), 8, 8, 4, 4), 255, 0, 0, 255);
    // ...until the host drops the cache.
    renderer.invalidateImages();
    expectApprox(pixelAt(renderOnce(), 8, 8, 4, 4), 0, 255, 0, 255);
    renderer.shutdown();
    target.destroy();
}

TEST_CASE("gl: T-263b a render with another IR or resolver uploads the images again by itself") {
    CommandList list;
    list.viewport = Rect{0, 0, 8, 8};
    list.devicePixelRatio = 1.0f;
    ImageCmd img;
    img.xf = Mat2x3{};
    img.asset = 0;
    img.dst = Rect{0, 0, 8, 8};
    img.srcUv = Rect{0, 0, 1, 1};
    img.opacity = 1.0f;
    img.smooth = 0;
    list.images.push_back(img);
    list.commands.push_back(Command{CmdKind::DrawImage, {}, 0});
    auto makeIr = []() {
        compile::IrWriter writer;
        compile::SceneBuilder scene;
        scene.width = 8;
        scene.height = 8;
        writer.addScene(scene);
        return writer.finish();
    };
    // Two documents whose asset 0 is another image (the shell reloaded or switched documents).
    std::vector<uint8_t> bytesA = makeIr();
    std::vector<uint8_t> bytesB = makeIr();
    Result<IrView> viewA = IrView::open(IrBlob{bytesA.data(), bytesA.size()});
    Result<IrView> viewB = IrView::open(IrBlob{bytesB.data(), bytesB.size()});
    REQUIRE(viewA.ok());
    REQUIRE(viewB.ok());

    REQUIRE(xt::gl::loadGl(sdlLoadProc));
    GlTestTarget target;
    target.init(8, 8);
    gl::GlRenderer renderer;
    REQUIRE(renderer.init(sdlLoadProc));
    renderer.resize(8, 8);
    SwitchableResolver resolverA;
    SwitchableResolver resolverB;
    resolverB.red = 0;
    resolverB.green = 255;
    auto renderOnce = [&](AssetResolver* assets, const IrView* ir) {
        target.bindAndClear(Color{0, 0, 0, 1});
        renderer.render(list, nullptr, assets, ir);
        return target.readback();
    };
    expectApprox(pixelAt(renderOnce(&resolverA, &viewA.value()), 8, 8, 4, 4), 255, 0, 0, 255);
    // Another IR (same resolver object, whose asset 0 changed): no invalidateImages() call.
    resolverA.red = 0;
    resolverA.green = 255;
    expectApprox(pixelAt(renderOnce(&resolverA, &viewB.value()), 8, 8, 4, 4), 0, 255, 0, 255);
    // Another resolver for the same IR.
    resolverB.red = 255;
    resolverB.green = 255;
    expectApprox(pixelAt(renderOnce(&resolverB, &viewB.value()), 8, 8, 4, 4), 255, 255, 0, 255);
    renderer.shutdown();
    target.destroy();
}

namespace {

/// T-263b E10: asset 0 is `width` x 1 opaque white pixels; counts the decodes.
class WideImageResolver : public AssetResolver {
public:
    int width = 1;
    int decodes = 0;
    bool readBytes(std::string_view, std::vector<uint8_t>&) override { return false; }
    bool decodeImage(AssetId, const IrView&, ImageData& out) override {
        ++decodes;
        out.width = width;
        out.height = 1;
        out.rgba.assign(static_cast<size_t>(width) * 4, 255);
        return true;
    }
    Vec2 imageSize(AssetId, const IrView&) override { return Vec2{static_cast<float>(width), 1}; }
};

}  // namespace

TEST_CASE("gl: T-263b an image wider than GL_MAX_TEXTURE_SIZE is not drawn and raises no GL error") {
    REQUIRE(xt::gl::loadGl(sdlLoadProc));
    gl::GLint maxSize = 0;
    gl::glGetIntegerv(gl::GL_MAX_TEXTURE_SIZE, &maxSize);
    REQUIRE(maxSize > 0);
    WideImageResolver resolver;
    // The frame builder takes images up to 65535 px; a driver whose limit is that high or higher
    // simply draws this one.
    resolver.width = std::min(maxSize + 1, 65535);
    const bool tooLarge = resolver.width > maxSize;

    CommandList list;
    list.viewport = Rect{0, 0, 8, 8};
    list.devicePixelRatio = 1.0f;
    ImageCmd img;
    img.xf = Mat2x3{};
    img.asset = 0;
    img.dst = Rect{0, 0, 8, 8};
    img.srcUv = Rect{0, 0, 1, 1};
    img.opacity = 1.0f;
    img.smooth = 1;
    list.images.push_back(img);
    list.commands.push_back(Command{CmdKind::DrawImage, {}, 0});
    compile::IrWriter writer;
    compile::SceneBuilder scene;
    scene.width = 8;
    scene.height = 8;
    writer.addScene(scene);
    std::vector<uint8_t> bytes = writer.finish();
    Result<IrView> viewResult = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(viewResult.ok());

    GlTestTarget target;
    target.init(8, 8);
    gl::GlRenderer renderer;
    REQUIRE(renderer.init(sdlLoadProc));
    renderer.resize(8, 8);
    while (gl::glGetError() != gl::GL_NO_ERROR) {
    }
    for (int frame = 0; frame < 3; ++frame) {
        CAPTURE(frame);
        target.bindAndClear(Color{0, 1, 0, 1});
        renderer.render(list, nullptr, &resolver, &viewResult.value());
        CHECK(gl::glGetError() == gl::GL_NO_ERROR);
        std::vector<uint8_t> px = target.readback();
        if (tooLarge) {
            // Not a black box: the image is left out and the failure reported -- T-263d: once,
            // by the build that finds it over GlDevice::maxTextureSize(), which never hands it
            // to createTexture.
            expectApprox(pixelAt(px, 8, 8, 4, 4), 0, 255, 0, 255);
            const std::string expected = "image " + std::to_string(resolver.width) +
                                         "x1 exceeds the device texture limit " + std::to_string(maxSize);
            CHECK(std::string(renderer.lastError()) == (frame == 0 ? expected : std::string()));
        } else {
            expectApprox(pixelAt(px, 8, 8, 4, 4), 255, 255, 255, 255);
        }
    }
    // The refused image is not decoded, premultiplied and uploaded again every frame.
    CHECK(resolver.decodes == 1);
    renderer.shutdown();
    target.destroy();
}

namespace {

/// T-263b E9: asset 0 is a 4x4 image whose texel (x, y) is (60x, 60y, 0, 255).
class RampResolver : public AssetResolver {
public:
    bool readBytes(std::string_view, std::vector<uint8_t>&) override { return false; }
    bool decodeImage(AssetId, const IrView&, ImageData& out) override {
        out.width = 4;
        out.height = 4;
        out.rgba.clear();
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                out.rgba.insert(out.rgba.end(),
                                {static_cast<uint8_t>(60 * x), static_cast<uint8_t>(60 * y), 0, 255});
            }
        }
        return true;
    }
    Vec2 imageSize(AssetId, const IrView&) override { return Vec2{4, 4}; }
};

}  // namespace

TEST_CASE("gl: T-263b uploads do not depend on the host's unpack state and leave it as it was") {
    using namespace xt::gl;
    REQUIRE(xt::gl::loadGl(sdlLoadProc));
    CommandList list;
    list.viewport = Rect{0, 0, 8, 8};
    list.devicePixelRatio = 1.0f;
    ImageCmd img;
    img.xf = Mat2x3{};
    img.asset = 0;
    img.dst = Rect{0, 0, 4, 4};
    img.srcUv = Rect{0, 0, 1, 1};
    img.opacity = 1.0f;
    img.smooth = 0;
    list.images.push_back(img);
    list.commands.push_back(Command{CmdKind::DrawImage, {}, 0});
    compile::IrWriter writer;
    compile::SceneBuilder scene;
    scene.width = 8;
    scene.height = 8;
    writer.addScene(scene);
    std::vector<uint8_t> bytes = writer.finish();
    Result<IrView> viewResult = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(viewResult.ok());

    GlTestTarget target;
    target.init(8, 8);
    gl::GlRenderer renderer;
    REQUIRE(renderer.init(sdlLoadProc));
    renderer.resize(8, 8);
    RampResolver resolver;
    target.bindAndClear(Color{0, 0, 0, 1});
    while (glGetError() != GL_NO_ERROR) {
    }
    GLuint pbo = 0;

    SUBCASE("row length and skips") {
        // A streaming host's leftovers. (Chosen so that a loader honoring them still reads
        // inside the 4x4 image: rows 2 texels apart, starting 1 row and 1 texel in.)
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 2);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 1);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 1);
    }
    SUBCASE("a bound pixel unpack buffer") {
        glGenBuffers(1, &pbo);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
        glBufferData(GL_PIXEL_UNPACK_BUFFER, 64, nullptr, GL_STREAM_DRAW);
    }
    GLint rowLength = 0, skipRows = 0, skipPixels = 0;
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skipRows);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skipPixels);

    renderer.render(list, nullptr, &resolver, &viewResult.value());
    CHECK(std::string(renderer.lastError()).empty());
    CHECK(glGetError() == GL_NO_ERROR);

    // The host's state is what it was...
    GLint v = -1;
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &v);
    CHECK(v == rowLength);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &v);
    CHECK(v == skipRows);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &v);
    CHECK(v == skipPixels);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &v);
    CHECK(v == static_cast<GLint>(pbo));
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    if (pbo != 0) {
        glDeleteBuffers(1, &pbo);
    }

    // ...and the image arrived texel for texel.
    std::vector<uint8_t> px = target.readback();
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            CAPTURE(x);
            CAPTURE(y);
            expectApprox(pixelAt(px, 8, 8, x, y), static_cast<uint8_t>(60 * x), static_cast<uint8_t>(60 * y),
                         0, 255, 2);
        }
    }
    renderer.shutdown();
    target.destroy();
}

// =================================================================================================
// saveState / restoreState.
// =================================================================================================

TEST_CASE("gl: saveState/restoreState leaves external GL state untouched") {
    static bool loaded = false;
    if (!loaded) {
        REQUIRE(xt::gl::loadGl(sdlLoadProc));
        loaded = true;
    }
    GlTestTarget target;
    target.init(64, 64);
    target.bindAndClear(Color{0, 0, 0, 1});

    using namespace xt::gl;
    // Establish deliberately non-default state before render().
    glDisable(GL_BLEND);
    glBindVertexArray(0);
    glUseProgram(0);
    GLboolean blendBefore = glIsEnabled(GL_BLEND);
    GLint programBefore = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &programBefore);
    GLint vaoBefore = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vaoBefore);

    CommandList list;
    list.viewport = Rect{0, 0, 64, 64};
    list.devicePixelRatio = 1.0f;
    addFillRect(list, Rect{0, 0, 64, 64}, Color{1, 0, 0, 1});

    gl::GlRenderer renderer;
    REQUIRE(renderer.init(sdlLoadProc));
    renderer.resize(64, 64);
    renderer.render(list, nullptr, nullptr, nullptr);

    GLboolean blendAfter = glIsEnabled(GL_BLEND);
    GLint programAfter = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &programAfter);
    GLint vaoAfter = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vaoAfter);

    CHECK(blendBefore == blendAfter);
    CHECK(programBefore == programAfter);
    CHECK(vaoBefore == vaoAfter);

    renderer.shutdown();
    target.destroy();
}

namespace {

/// A 2x2 host texture (the kind an `engine://` asset names) with LINEAR filtering.
gl::GLuint makeHostTexture(uint8_t r, uint8_t g, uint8_t b) {
    using namespace xt::gl;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    std::vector<uint8_t> px;
    for (int i = 0; i < 4; ++i) {
        px.insert(px.end(), {r, g, b, 255});
    }
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA8), 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 px.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_LINEAR));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_LINEAR));
    return tex;
}

/// T-263b E5: a list that makes the device touch texture units 0..2 (an effect, a blended layer),
/// the two-sided stencil ops (a rotated clip) and a host texture's filter (`engine://`, asset 0,
/// drawn with nearest sampling).
CommandList stateTouchingList() {
    CommandList list;
    list.viewport = Rect{0, 0, 64, 64};
    list.devicePixelRatio = 1.0f;
    addFillRect(list, Rect{0, 0, 64, 64}, Color{0.5f, 0.5f, 0.5f, 1});
    addPushClipRect(list, Rect{-20, -20, 40, 40});
    list.clipRects.back().xf = mul(translation(Vec2{32, 32}), rotationDeg(20.0f));
    addFillRect(list, Rect{0, 0, 64, 64}, Color{1, 0, 0, 1});
    addPopClip(list);

    LayerCmd layer{};
    layer.opacity = 1.0f;
    layer.blend = BlendMode::Multiply;
    layer.boundsHint = Rect{0, 0, 0, 0};
    list.layers.push_back(layer);
    list.commands.push_back(Command{CmdKind::PushLayer, {}, 0});
    addFillRect(list, Rect{8, 8, 16, 16}, Color{0, 0, 1, 1});
    list.commands.push_back(Command{CmdKind::PopLayer, {}, 0});

    EffectInstance blur{};
    blur.kind = EffectKind::Blur;
    blur.params[0] = 3;
    blur.params[1] = 3;
    EffectCmd fx{};
    fx.effectOffset = 0;
    fx.effectCount = 1;
    list.effects.push_back(blur);
    fx.xf = Mat2x3{};
    fx.boundsHint = Rect{24, 24, 40, 40};
    fx.contentBounds = Rect{32, 32, 16, 16};
    list.effectCmds.push_back(fx);
    list.commands.push_back(Command{CmdKind::PushEffect, {}, 0});
    addFillRect(list, Rect{32, 32, 16, 16}, Color{0, 1, 0, 1});
    list.commands.push_back(Command{CmdKind::PopEffect, {}, 0});

    ImageCmd img;
    img.xf = Mat2x3{};
    img.asset = 0;
    img.dst = Rect{0, 48, 16, 16};
    img.srcUv = Rect{0, 0, 1, 1};
    img.opacity = 1.0f;
    img.smooth = 0;
    list.images.push_back(img);
    list.commands.push_back(Command{CmdKind::DrawImage, {}, 0});
    return list;
}

/// An IR whose asset 0 is the external texture `engine://scene`.
std::vector<uint8_t> irWithEngineAsset() {
    compile::IrWriter writer;
    compile::AssetBuilder asset;
    asset.name = writer.intern("engine://scene");
    asset.kind = 0;
    writer.addAsset(asset);
    compile::SceneBuilder scene;
    scene.width = 64;
    scene.height = 64;
    writer.addScene(scene);
    return writer.finish();
}

}  // namespace

TEST_CASE(
    "gl: T-263b render restores units 0-2, two-sided stencil, the alpha blend equation and a host "
    "texture's filter") {
    using namespace xt::gl;
    REQUIRE(xt::gl::loadGl(sdlLoadProc));
    GlTestTarget target;
    target.init(64, 64);
    gl::GlRenderer renderer;
    REQUIRE(renderer.init(sdlLoadProc));
    renderer.resize(64, 64);
    std::vector<uint8_t> irBytes = irWithEngineAsset();
    Result<IrView> viewResult = IrView::open(IrBlob{irBytes.data(), irBytes.size()});
    REQUIRE(viewResult.ok());
    const GLuint scene = makeHostTexture(255, 255, 0);
    SwitchableResolver resolver;
    resolver.setExternalTexture("scene", scene, 2, 2);
    const CommandList list = stateTouchingList();

    // The host's state: its own textures on units 0..2 with unit 1 active, different stencil
    // state per face, different blend equations for color and alpha.
    const GLuint host[3] = {makeHostTexture(1, 2, 3), makeHostTexture(4, 5, 6), makeHostTexture(7, 8, 9)};
    target.bindAndClear(Color{0, 0, 0, 1});
    for (int unit = 0; unit < 3; ++unit) {
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
        glBindTexture(GL_TEXTURE_2D, host[unit]);
    }
    glActiveTexture(GL_TEXTURE1);
    glStencilFuncSeparate(GL_FRONT, GL_LEQUAL, 3, 0x3Fu);
    glStencilFuncSeparate(GL_BACK, GL_GEQUAL, 5, 0x7Fu);
    glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_INCR, GL_REPLACE);
    glStencilOpSeparate(GL_BACK, GL_INVERT, GL_DECR, GL_INCR_WRAP);
    glStencilMaskSeparate(GL_FRONT, 0x0Fu);
    glStencilMaskSeparate(GL_BACK, 0xF0u);
    glBlendEquationSeparate(GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT);

    renderer.render(list, nullptr, &resolver, &viewResult.value());
    CHECK(std::string(renderer.lastError()).empty());

    auto get = [](GLenum name) {
        GLint v = -1;
        glGetIntegerv(name, &v);
        return v;
    };
    CHECK(get(GL_ACTIVE_TEXTURE) == static_cast<GLint>(GL_TEXTURE1));
    for (int unit = 0; unit < 3; ++unit) {
        CAPTURE(unit);
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
        CHECK(get(GL_TEXTURE_BINDING_2D) == static_cast<GLint>(host[unit]));
    }
    CHECK(get(GL_STENCIL_FUNC) == static_cast<GLint>(GL_LEQUAL));
    CHECK(get(GL_STENCIL_REF) == 3);
    CHECK(get(GL_STENCIL_VALUE_MASK) == 0x3F);
    CHECK(get(GL_STENCIL_FAIL) == static_cast<GLint>(GL_KEEP));
    CHECK(get(GL_STENCIL_PASS_DEPTH_FAIL) == static_cast<GLint>(GL_INCR));
    CHECK(get(GL_STENCIL_PASS_DEPTH_PASS) == static_cast<GLint>(GL_REPLACE));
    CHECK(get(GL_STENCIL_WRITEMASK) == 0x0F);
    CHECK(get(GL_STENCIL_BACK_FUNC) == static_cast<GLint>(GL_GEQUAL));
    CHECK(get(GL_STENCIL_BACK_REF) == 5);
    CHECK(get(GL_STENCIL_BACK_VALUE_MASK) == 0x7F);
    CHECK(get(GL_STENCIL_BACK_FAIL) == static_cast<GLint>(GL_INVERT));
    CHECK(get(GL_STENCIL_BACK_PASS_DEPTH_FAIL) == static_cast<GLint>(GL_DECR));
    CHECK(get(GL_STENCIL_BACK_PASS_DEPTH_PASS) == static_cast<GLint>(GL_INCR_WRAP));
    CHECK(get(GL_STENCIL_BACK_WRITEMASK) == 0xF0);
    CHECK(get(GL_BLEND_EQUATION_RGB) == static_cast<GLint>(GL_FUNC_SUBTRACT));
    CHECK(get(GL_BLEND_EQUATION_ALPHA) == static_cast<GLint>(GL_FUNC_REVERSE_SUBTRACT));

    // The engine:// texture was sampled with nearest filtering; it has its LINEAR filter back.
    std::vector<uint8_t> px = target.readback();
    expectApprox(pixelAt(px, 64, 64, 8, 56), 255, 255, 0, 255);
    glBindTexture(GL_TEXTURE_2D, scene);
    GLint minFilter = 0, magFilter = 0;
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minFilter);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &magFilter);
    CHECK(minFilter == static_cast<GLint>(GL_LINEAR));
    CHECK(magFilter == static_cast<GLint>(GL_LINEAR));

    // Back to defaults for the tests that follow.
    glStencilFunc(GL_ALWAYS, 0, 0xFFu);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glStencilMask(0xFFu);
    glBlendEquation(GL_FUNC_ADD);
    for (int unit = 0; unit < 3; ++unit) {
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glActiveTexture(GL_TEXTURE0);
    renderer.shutdown();
    glDeleteTextures(3, host);
    glDeleteTextures(1, &scene);
    target.destroy();
}

// T-161: the pool deletes entries that stay unused for longer than the idle window, keeps the
// ones in use, and refills an emptied slot on the next matching acquire.
TEST_CASE("gl: FboPool evicts entries idle for longer than the window") {
    static bool loaded = false;
    if (!loaded) {
        REQUIRE(xt::gl::loadGl(sdlLoadProc));
        loaded = true;
    }
    using namespace xt::gl;
    FboPool pool;
    const int a = pool.acquire(64, 64, 0);
    REQUIRE(a >= 0);
    const int held = pool.acquire(32, 32, 0);
    REQUIRE(held >= 0);
    pool.release(a);
    // Acquired in frame 0; frames 1..6 leave it idle for at most 5 frames.
    for (int f = 0; f < 6; ++f) pool.endFrame(5);
    CHECK(pool.get(a).fbo != 0);
    pool.endFrame(5);  // idle for 6 frames -> deleted
    CHECK(pool.get(a).fbo == 0);
    CHECK(pool.get(held).fbo != 0);  // still in use: never evicted
    bool created = false;
    const int b = pool.acquire(64, 64, 0, GL_RGBA8, true, &created);
    CHECK(created);
    CHECK(b == a);  // the emptied slot is reused before the pool grows
    pool.release(held);
    pool.shutdown();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// =================================================================================================
// 19.7 3D commands: not implemented by this backend yet (T-46) -- render() must skip them (with a
// one-time warning) instead of crashing/asserting. See docs/tasks/T-44a.md.
// =================================================================================================

TEST_CASE("gl: a CommandList carrying the five 19.7 3D commands does not crash render()") {
    CommandList list;
    list.viewport = Rect{0, 0, 128, 128};
    list.devicePixelRatio = 1.0f;

    ProjectionCmd proj{};
    proj.viewProj = Mat4{};
    proj.rootXf = Mat2x3{};
    proj.boxRect = Rect{0, 0, 128, 128};
    proj.depthTest = 0;
    list.commands.push_back(
        Command{CmdKind::PushProjection, {}, static_cast<uint32_t>(list.projections.size())});
    list.projections.push_back(proj);

    Transform3DCmd xf3{};
    xf3.world3 = Mat4{};
    list.commands.push_back(
        Command{CmdKind::PushTransform3D, {}, static_cast<uint32_t>(list.transforms3.size())});
    list.transforms3.push_back(xf3);

    // A normal 2D fill in between, to confirm the surrounding commands still render normally.
    addFillRect(list, Rect{0, 0, 128, 128}, Color{0, 1, 0, 1});

    ImageQuadCmd quad{};
    quad.asset = 0;
    quad.p[0] = Vec2{0, 0};
    quad.p[1] = Vec2{1, 0};
    quad.p[2] = Vec2{1, 1};
    quad.p[3] = Vec2{0, 1};
    quad.srcUv = Rect{0, 0, 1, 1};
    quad.opacity = 1;
    quad.smooth = 0;
    list.commands.push_back(
        Command{CmdKind::DrawImageQuad, {}, static_cast<uint32_t>(list.imageQuads.size())});
    list.imageQuads.push_back(quad);

    list.commands.push_back(Command{CmdKind::PopTransform3D, {}, 0});
    list.commands.push_back(Command{CmdKind::PopProjection, {}, 0});

    std::vector<uint8_t> px = renderCommands(list, 128, 128, Color{0, 0, 0, 1});
    // The 3D commands are skipped, so the fill in between still covers the whole target.
    expectApprox(pixelAt(px, 128, 128, 64, 64), 0, 255, 0, 255);
}

// =================================================================================================
// SW vs GL comparison against tests/golden/expected/*.png.
// =================================================================================================

TEST_CASE("gl: matches SW golden fixtures") {
    static bool loaded = false;
    if (!loaded) {
        REQUIRE(xt::gl::loadGl(sdlLoadProc));
        loaded = true;
    }

    for (const golden::GoldenFixture& fx : golden::fixtures()) {
        INFO("fixture: ", std::string(fx.name));
        compile::IrWriter writer;
        fx.build(writer);
        std::vector<uint8_t> bytes = writer.finish();
        Result<IrView> viewResult = IrView::open(IrBlob{bytes.data(), bytes.size()});
        REQUIRE(viewResult.ok());
        const IrView& view = viewResult.value();

        NodeArena arena;
        NodeId container = arena.create(NodeKind::Group);
        SceneInstance inst;
        REQUIRE(instantiateScene(view, 0, arena, container, inst) == Status::kOk);
        updateWorldTransforms(arena, inst.root, true);

        Painter painter;
        CommandList cmds;
        painter.paint(arena, inst.root, cmds);
        cmds.viewport = Rect{0, 0, static_cast<float>(fx.width), static_cast<float>(fx.height)};
        cmds.devicePixelRatio = fx.dpr;

        std::vector<uint8_t> actual = renderCommands(cmds, fx.width, fx.height, fx.clearColor);
        // renderCommands flips rows for pixelAt()'s convenience; compareRgba needs a top-down
        // tightly-packed buffer matching xt::sw's / the expected PNG's row order, so un-flip here.
        std::vector<uint8_t> topDown(actual.size());
        for (int y = 0; y < fx.height; ++y) {
            size_t srcRow = static_cast<size_t>(fx.height - 1 - y) * static_cast<size_t>(fx.width) * 4;
            size_t dstRow = static_cast<size_t>(y) * static_cast<size_t>(fx.width) * 4;
            std::copy(actual.begin() + static_cast<long>(srcRow),
                      actual.begin() + static_cast<long>(srcRow) + fx.width * 4,
                      topDown.begin() + static_cast<long>(dstRow));
        }

        std::vector<uint8_t> expected;
        int ew = 0, eh = 0;
        std::string path = std::string(XT_GOLDEN_EXPECTED_DIR) + "/" + fx.name + ".png";
        REQUIRE(sw::readPng(path, expected, ew, eh));
        REQUIRE(ew == fx.width);
        REQUIRE(eh == fx.height);

        golden::GoldenResult result =
            golden::compareRgba(topDown.data(), expected.data(), fx.width, fx.height, 16, 0.03f);
        CHECK(result.pass);
    }
}

// =================================================================================================
// T-199: border-image nine-slice (docs/design/26-layers-symbols-library.md §26.3.4).
// =================================================================================================

namespace {

/// The golden fixture's 24 x 24 nine-slice image (tests/golden/ui/nine.png) for every asset.
class NineSliceResolver : public AssetResolver {
public:
    NineSliceResolver() {
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
        if (sw::readPng(std::string(XT_GOLDEN_UI_DIR) + "/nine.png", rgba, w, h)) {
            m_image.width = w;
            m_image.height = h;
            m_image.rgba = std::move(rgba);
        }
    }
    bool ok() const { return m_image.width == 24 && m_image.height == 24; }
    bool readBytes(std::string_view, std::vector<uint8_t>&) override { return false; }
    bool decodeImage(AssetId, const IrView&, ImageData& out) override {
        out = m_image;
        return ok();
    }
    Vec2 imageSize(AssetId, const IrView&) override { return Vec2{24, 24}; }

private:
    ImageData m_image;
};

}  // namespace

TEST_CASE("gl: border-image nine-slice keeps the corners and stretches the edges") {
    NineSliceResolver resolver;
    REQUIRE(resolver.ok());
    // A 64 x 40 element with 8 px slices: 8 x 8 corners, stretched edges, filled middle.
    NodeArena arena;
    NodeId root = arena.create(NodeKind::Element);
    ElementData& data = arena.element(root);
    data.box.width = 64;
    data.box.height = 40;
    data.style.borderImage = 0;
    data.style.borderImageRepeat = 0;
    data.style.borderImageFill = 1;
    data.borderImageSize = Vec2{24, 24};
    for (float& s : data.borderImageSlicePx) {
        s = 8;
    }
    updateWorldTransforms(arena, root, true);
    Painter painter;
    CommandList list;
    painter.paint(arena, root, list);
    list.viewport = Rect{0, 0, 64, 40};
    list.devicePixelRatio = 1.0f;

    compile::IrWriter writer;
    compile::SceneBuilder scene;
    scene.width = 8;
    scene.height = 8;
    writer.addScene(scene);
    compile::AssetBuilder asset;
    asset.name = writer.intern("nine.png");
    writer.addAsset(asset);
    std::vector<uint8_t> bytes = writer.finish();
    Result<IrView> view = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(view.ok());
    std::vector<uint8_t> px = renderCommands(list, 64, 40, Color{1, 1, 1, 1}, &resolver, &view.value());
    expectApprox(pixelAt(px, 64, 40, 2, 2), 255, 0, 0, 255);      // top-left corner: red
    expectApprox(pixelAt(px, 64, 40, 61, 2), 0, 255, 0, 255);     // top-right: green
    expectApprox(pixelAt(px, 64, 40, 2, 37), 0, 0, 255, 255);     // bottom-left: blue
    expectApprox(pixelAt(px, 64, 40, 61, 37), 255, 255, 0, 255);  // bottom-right: yellow
    expectApprox(pixelAt(px, 64, 40, 20, 3), 0, 255, 255, 255);   // top edge: cyan (stretched)
    expectApprox(pixelAt(px, 64, 40, 3, 12), 255, 128, 0, 255);   // left edge: orange (off the stripe)
    expectApprox(pixelAt(px, 64, 40, 32, 20), 64, 64, 64, 255);   // middle: fill
}

// =================================================================================================
// T-263c A: several Documents of one Context in one CommandList (see tests/gpu's twin cases).
// =================================================================================================

namespace {

std::vector<uint8_t> compileImageDoc(const std::filesystem::path& root, const char* image, int left,
                                     bool embed) {
    const std::string xt = std::string("<xt><body style=\"padding: 0\"><img src=\"") + image +
                           "\" style=\"position: absolute; left: " + std::to_string(left) +
                           "px; top: 0; width: 16px; height: 16px\"/></body></xt>";
    compile::SourceFile file{"doc.xt", xt};
    compile::CompileOptions options;
    options.rootDir = root.string();
    options.embedImages = embed;
    return compile::compileDocumentFromStrings(std::span<const compile::SourceFile>(&file, 1), "doc.xt",
                                               options)
        .ir;
}

/// Pixels of the columns [x0, x1) that differ between two w x h buffers.
int columnsDiffer(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int w, int h, int x0,
                  int x1) {
    int n = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = x0; x < x1; ++x) {
            const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
            n += std::memcmp(&a[i], &b[i], 4) != 0 ? 1 : 0;
        }
    }
    return n;
}

}  // namespace

// T-263d: the list names each Document's IR (CommandList::sources) and every asset command carries
// its Document's index, so each surface draws its own asset 0 whichever IR the host passes.
TEST_CASE("gl: T-263c two documents in one frame each draw their own asset 0") {
    constexpr int kW = 64;
    constexpr int kH = 32;
    const std::filesystem::path dir = xt::test::tempDir() / "xt_t263c_two_surfaces_gl";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    // The first document embeds an 8x8 red PNG (drawn at x 0..16), the second names a 4x4 blue PNG
    // file (drawn at x 32..48): asset 0 in both.
    std::vector<uint8_t> red(8 * 8 * 4), blue(4 * 4 * 4);
    for (size_t i = 0; i < red.size(); i += 4) {
        red[i] = 255, red[i + 1] = 0, red[i + 2] = 0, red[i + 3] = 255;
    }
    for (size_t i = 0; i < blue.size(); i += 4) {
        blue[i] = 0, blue[i + 1] = 0, blue[i + 2] = 255, blue[i + 3] = 255;
    }
    REQUIRE(sw::writePng((dir / "a.png").string(), red.data(), 8, 8, 8 * 4));
    REQUIRE(sw::writePng((dir / "b.png").string(), blue.data(), 4, 4, 4 * 4));
    const std::vector<uint8_t> irA = compileImageDoc(dir, "a.png", 0, /*embed=*/true);
    const std::vector<uint8_t> irB = compileImageDoc(dir, "b.png", 32, /*embed=*/false);
    REQUIRE(!irA.empty());
    REQUIRE(!irB.empty());
    {
        FileAssetResolver assets(dir.string(), sw::decodeImageBytes);
        auto makeContext = [&]() {
            auto ctx = std::make_unique<Context>(ContextConfig{nullptr, &assets});
            ctx->setViewport(Rect{0, 0, static_cast<float>(kW), static_cast<float>(kH)}, 1.0f);
            return ctx;
        };
        std::unique_ptr<Context> both = makeContext();
        std::unique_ptr<Context> aloneA = makeContext();
        std::unique_ptr<Context> aloneB = makeContext();
        Result<DocumentHandle> a = both->loadDocument(IrBlob{irA.data(), irA.size()}, false);
        Result<DocumentHandle> b = both->loadDocument(IrBlob{irB.data(), irB.size()}, false);
        REQUIRE(a.ok());
        REQUIRE(b.ok());
        REQUIRE(aloneA->loadDocument(IrBlob{irA.data(), irA.size()}, false).ok());
        REQUIRE(aloneB->loadDocument(IrBlob{irB.data(), irB.size()}, false).ok());
        const Document* docA = both->document(a.value());
        const Document* docB = both->document(b.value());
        CommandList listBoth, listA, listB;
        both->update(0.0f);
        aloneA->update(0.0f);
        aloneB->update(0.0f);
        both->render(listBoth);
        aloneA->render(listA);
        aloneB->render(listB);

        const Color clear{0, 0, 0, 1};
        const std::vector<uint8_t> refA = renderCommands(listA, kW, kH, clear, &assets, &docA->ir());
        const std::vector<uint8_t> refB = renderCommands(listB, kW, kH, clear, &assets, &docB->ir());
        expectApprox(pixelAt(refA, kW, kH, 8, 8), 255, 0, 0, 255);
        expectApprox(pixelAt(refB, kW, kH, 40, 8), 0, 0, 255, 255);
        for (const Document* doc : {docA, docB}) {
            CAPTURE(doc == docA);  // which document's IR the host passed
            const std::vector<uint8_t> px = renderCommands(listBoth, kW, kH, clear, &assets, &doc->ir());
            CHECK(columnsDiffer(px, refA, kW, kH, 0, 32) == 0);
            CHECK(columnsDiffer(px, refB, kW, kH, 32, 64) == 0);
        }
    }
    std::filesystem::remove_all(dir, ec);
}

// T-263d spec point 2: both documents' custom effect shader is asset 0 (an `.xtfx` of the same
// name, another body); each PushEffect runs the ShaderRef of its own source.
TEST_CASE("gl: T-263d two documents in one frame each run their own custom effect shader") {
    constexpr int kW = 64;
    constexpr int kH = 32;
    const std::filesystem::path dir = xt::test::tempDir() / "xt_t263d_two_shaders_gl";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    // A white square at x `left`..`left` + 16 whose effect multiplies it by `factor`.
    auto compileDoc = [&](const char* sub, int left, const char* factor) {
        std::filesystem::create_directories(dir / sub, ec);
        {
            std::ofstream f(dir / sub / "fx.xtfx", std::ios::binary);
            f << "fallback: none\n---\nfloat4 xtMain(XtInput i)\n{\n    return xtSample(i.uv) * " << factor
              << ";\n}\n";
        }
        const std::string xt = "<xt><head><style>#a { position: absolute; left: " + std::to_string(left) +
                               "px; top: 8px; width: 16px; height: 16px; background-color: #ffffff;"
                               " effect: custom(\"fx.xtfx\"); }</style></head>"
                               "<body style=\"padding: 0\"><div id=\"a\"></div></body></xt>";
        compile::SourceFile file{"doc.xt", xt};
        compile::CompileOptions options;
        options.rootDir = (dir / sub).string();
        return compile::compileDocumentFromStrings(std::span<const compile::SourceFile>(&file, 1), "doc.xt",
                                                   options)
            .ir;
    };
    const std::vector<uint8_t> irA = compileDoc("a", 8, "0.25");
    const std::vector<uint8_t> irB = compileDoc("b", 40, "0.75");
    REQUIRE(!irA.empty());
    REQUIRE(!irB.empty());
    {
        FileAssetResolver assets(dir.string(), sw::decodeImageBytes);
        auto makeContext = [&]() {
            auto ctx = std::make_unique<Context>(ContextConfig{nullptr, &assets});
            ctx->setViewport(Rect{0, 0, static_cast<float>(kW), static_cast<float>(kH)}, 1.0f);
            return ctx;
        };
        std::unique_ptr<Context> both = makeContext();
        std::unique_ptr<Context> aloneA = makeContext();
        std::unique_ptr<Context> aloneB = makeContext();
        Result<DocumentHandle> a = both->loadDocument(IrBlob{irA.data(), irA.size()}, false);
        Result<DocumentHandle> b = both->loadDocument(IrBlob{irB.data(), irB.size()}, false);
        REQUIRE(a.ok());
        REQUIRE(b.ok());
        REQUIRE(aloneA->loadDocument(IrBlob{irA.data(), irA.size()}, false).ok());
        REQUIRE(aloneB->loadDocument(IrBlob{irB.data(), irB.size()}, false).ok());
        const Document* docA = both->document(a.value());
        const Document* docB = both->document(b.value());
        CommandList listBoth, listA, listB;
        both->update(0.0f);
        aloneA->update(0.0f);
        aloneB->update(0.0f);
        both->render(listBoth);
        aloneA->render(listA);
        aloneB->render(listB);

        const Color clear{0, 0, 0, 1};
        const std::vector<uint8_t> refA = renderCommands(listA, kW, kH, clear, &assets, &docA->ir());
        const std::vector<uint8_t> refB = renderCommands(listB, kW, kH, clear, &assets, &docB->ir());
        // The references differ: each square is dimmed by its own shader's factor.
        const int grayA = pixelAt(refA, kW, kH, 16, 16).r;
        const int grayB = pixelAt(refB, kW, kH, 48, 16).r;
        REQUIRE(grayA > 0);
        REQUIRE(grayB > grayA + 32);
        for (const Document* doc : {docA, docB}) {
            CAPTURE(doc == docA);  // which document's IR the host passed
            const std::vector<uint8_t> px = renderCommands(listBoth, kW, kH, clear, &assets, &doc->ir());
            CHECK(columnsDiffer(px, refA, kW, kH, 0, 32) == 0);
            CHECK(columnsDiffer(px, refB, kW, kH, 32, 64) == 0);
        }
    }
    std::filesystem::remove_all(dir, ec);
}

// =================================================================================================
// T-263c ruling 7: a host's sampler objects on units 0..2 would override every texture parameter
// xt_gl sets; they are unbound for the render and bound again afterwards.
// =================================================================================================

namespace {

/// Asset 0 is a 2x1 image: one black texel, one white.
class BlackWhiteResolver : public AssetResolver {
public:
    bool readBytes(std::string_view, std::vector<uint8_t>&) override { return false; }
    bool decodeImage(AssetId, const IrView&, ImageData& out) override {
        out.width = 2;
        out.height = 1;
        out.rgba = {0, 0, 0, 255, 255, 255, 255, 255};
        return true;
    }
    Vec2 imageSize(AssetId, const IrView&) override { return Vec2{2, 1}; }
};

// Sampler-object entry points and the binding query (GL 3.3 core), resolved by the test itself.
constexpr gl::GLenum kGlSamplerBinding = 0x8919;
using GenSamplersFn = void (*)(gl::GLsizei, gl::GLuint*);
using DeleteSamplersFn = void (*)(gl::GLsizei, const gl::GLuint*);
using SamplerParameteriFn = void (*)(gl::GLuint, gl::GLenum, gl::GLint);
using BindSamplerFn = void (*)(gl::GLuint, gl::GLuint);

}  // namespace

TEST_CASE("gl: T-263c a host sampler object on units 0-2 does not change xt_gl's sampling and stays bound") {
    using namespace xt::gl;
    REQUIRE(xt::gl::loadGl(sdlLoadProc));
    const auto genSamplers = reinterpret_cast<GenSamplersFn>(sdlLoadProc("glGenSamplers"));
    const auto deleteSamplers = reinterpret_cast<DeleteSamplersFn>(sdlLoadProc("glDeleteSamplers"));
    const auto samplerParameteri = reinterpret_cast<SamplerParameteriFn>(sdlLoadProc("glSamplerParameteri"));
    const auto bindSampler = reinterpret_cast<BindSamplerFn>(sdlLoadProc("glBindSampler"));
    REQUIRE(genSamplers != nullptr);
    REQUIRE(deleteSamplers != nullptr);
    REQUIRE(samplerParameteri != nullptr);
    REQUIRE(bindSampler != nullptr);

    // The image stretched over 8 px with linear filtering: texel centers at x = 2 and x = 6, a
    // ramp between them.
    CommandList list;
    list.viewport = Rect{0, 0, 8, 8};
    list.devicePixelRatio = 1.0f;
    ImageCmd img;
    img.xf = Mat2x3{};
    img.asset = 0;
    img.dst = Rect{0, 0, 8, 8};
    img.srcUv = Rect{0, 0, 1, 1};
    img.opacity = 1.0f;
    img.smooth = 1;
    list.images.push_back(img);
    list.commands.push_back(Command{CmdKind::DrawImage, {}, 0});
    compile::IrWriter writer;
    compile::SceneBuilder scene;
    scene.width = 8;
    scene.height = 8;
    writer.addScene(scene);
    std::vector<uint8_t> bytes = writer.finish();
    Result<IrView> viewResult = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(viewResult.ok());

    GlTestTarget target;
    target.init(8, 8);
    gl::GlRenderer renderer;
    REQUIRE(renderer.init(sdlLoadProc));
    renderer.resize(8, 8);
    BlackWhiteResolver resolver;

    // The host: a NEAREST sampler on each of units 0..2, unit 2 active.
    GLuint samplers[3] = {0, 0, 0};
    genSamplers(3, samplers);
    for (int unit = 0; unit < 3; ++unit) {
        samplerParameteri(samplers[unit], GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_NEAREST));
        samplerParameteri(samplers[unit], GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_NEAREST));
        bindSampler(static_cast<GLuint>(unit), samplers[unit]);
    }
    target.bindAndClear(Color{0, 0, 0, 1});
    glActiveTexture(GL_TEXTURE2);

    renderer.render(list, nullptr, &resolver, &viewResult.value());
    CHECK(std::string(renderer.lastError()).empty());

    // The sampler bindings and the active unit are the host's again...
    GLint active = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    CHECK(active == static_cast<GLint>(GL_TEXTURE2));
    for (int unit = 0; unit < 3; ++unit) {
        CAPTURE(unit);
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
        GLint bound = -1;
        glGetIntegerv(kGlSamplerBinding, &bound);
        CHECK(bound == static_cast<GLint>(samplers[unit]));
        bindSampler(static_cast<GLuint>(unit), 0);
    }
    glActiveTexture(GL_TEXTURE0);
    deleteSamplers(3, samplers);

    // ...and the image was filtered linearly (nearest would give 0 and 255 here).
    std::vector<uint8_t> px = target.readback();
    expectApprox(pixelAt(px, 8, 8, 3, 4), 96, 96, 96, 255, 12);
    expectApprox(pixelAt(px, 8, 8, 4, 4), 159, 159, 159, 255, 12);
    renderer.shutdown();
    target.destroy();
}

// =================================================================================================
// T-263c ruling 8: updateTexture with a row pitch wider than the region is one glTexSubImage2D
// under GL_UNPACK_ROW_LENGTH.
// =================================================================================================

// T-263d: the device reports GL_MAX_TEXTURE_SIZE, so the frame builder leaves a larger image out
// without calling createTexture.
TEST_CASE("gl: T-263d GlDevice::maxTextureSize is GL_MAX_TEXTURE_SIZE") {
    using namespace xt::gl;
    REQUIRE(xt::gl::loadGl(sdlLoadProc));
    GlDevice device;
    CHECK(device.maxTextureSize() == 0);  // unknown before init()
    std::string err;
    REQUIRE(device.init(err));
    GLint limit = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
    REQUIRE(limit > 0);
    CHECK(device.maxTextureSize() == static_cast<uint32_t>(limit));
    const gpu::GpuDevice& base = device;
    CHECK(base.maxTextureSize() == static_cast<uint32_t>(limit));
}

TEST_CASE("gl: T-263c updateTexture takes rows wider than the updated region") {
    using namespace xt::gl;
    REQUIRE(xt::gl::loadGl(sdlLoadProc));
    GlDevice device;
    std::string err;
    REQUIRE(device.init(err));
    while (glGetError() != GL_NO_ERROR) {
    }
    // Reads back a 4x4 texture through a framebuffer (row 0 first).
    auto readTexture = [](GLuint tex) {
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        REQUIRE(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
        std::vector<uint8_t> px(4 * 4 * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        return px;
    };

    SUBCASE("Rgba8, 5 texels per source row, a 2x3 region") {
        const std::vector<uint8_t> zero(4 * 4 * 4, 0);
        const gpu::TextureHandle tex =
            device.createTexture("t", 4, 4, gpu::TextureFormat::Rgba8, zero.data());
        REQUIRE(tex != 0);
        // Source texel (sx, sy) = (10 * sx + 1, 10 * sy + 1, 7, 255); the region starts at its (1, 0).
        std::vector<uint8_t> src(5 * 3 * 4);
        for (int sy = 0; sy < 3; ++sy) {
            for (int sx = 0; sx < 5; ++sx) {
                uint8_t* p = &src[(static_cast<size_t>(sy) * 5 + static_cast<size_t>(sx)) * 4];
                p[0] = static_cast<uint8_t>(10 * sx + 1);
                p[1] = static_cast<uint8_t>(10 * sy + 1);
                p[2] = 7;
                p[3] = 255;
            }
        }
        device.updateTexture(tex, 1, 1, 2, 3, src.data() + 4, 5 * 4);
        CHECK(glGetError() == GL_NO_ERROR);
        GLint rowLength = -1;
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
        CHECK(rowLength == 0);  // left as the device's uploads expect it
        const std::vector<uint8_t> px = readTexture(tex);
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                CAPTURE(x);
                CAPTURE(y);
                const uint8_t* p = &px[(static_cast<size_t>(y) * 4 + static_cast<size_t>(x)) * 4];
                const bool inside = x >= 1 && x < 3 && y >= 1 && y < 4;
                CHECK(p[0] == (inside ? 10 * x + 1 : 0));        // source x = x - 1 + 1
                CHECK(p[1] == (inside ? 10 * (y - 1) + 1 : 0));  // source y = y - 1
                CHECK(p[2] == (inside ? 7 : 0));
            }
        }
        device.destroyTexture(tex);
    }
    SUBCASE("R8, an odd row pitch") {
        const std::vector<uint8_t> zero(4 * 4, 0);
        const gpu::TextureHandle tex = device.createTexture("t", 4, 4, gpu::TextureFormat::R8, zero.data());
        REQUIRE(tex != 0);
        const uint8_t src[3 * 2] = {11, 12, 13, 21, 22, 23};
        device.updateTexture(tex, 2, 0, 2, 2, src, 3);
        CHECK(glGetError() == GL_NO_ERROR);
        const std::vector<uint8_t> px = readTexture(tex);
        CHECK(px[(0 * 4 + 2) * 4] == 11);
        CHECK(px[(0 * 4 + 3) * 4] == 12);
        CHECK(px[(1 * 4 + 2) * 4] == 21);
        CHECK(px[(1 * 4 + 3) * 4] == 22);
        CHECK(px[(1 * 4 + 1) * 4] == 0);
        device.destroyTexture(tex);
    }
    device.shutdown();
}
