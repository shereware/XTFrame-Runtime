#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "camera.h"  // core/src internal header: cssPerspectiveViewProj (tests/gl/CMakeLists.txt adds core/src).
#include "doctest.h"
#include "gl_loader.h"
#include "golden_common.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/gl/gl_renderer.h"
#include "xt/ir.h"
#include "xt/sw/sw_renderer.h"
#include "xt/types.h"

/// GlRenderer 3D projection tests (docs/tasks/T-46.md): renders the *same* CommandList used by
/// tests/unit/test_sw_3d.cpp's seven cases (PushProjection/PopProjection, PushTransform3D/
/// PopTransform3D, DrawImageQuad, and Fill/Clip/DrawImage under projection) through both
/// xt::sw::SwRenderer and xt::gl::GlRenderer at 64x64, and compares the two RGBA8 buffers with
/// docs/design/15-testing.md §15.3's GL/SW cross-check tolerance (channel <= 16/255, <= 3%
/// differing pixels), per docs/tasks/T-46.md "測試". main() (test_gl_main.cpp) guarantees a
/// current GL context exists before any TEST_CASE runs (otherwise the whole binary exits 0
/// without running doctest at all). The pre-T-46 (unprojected) comparisons in
/// tests/gl/test_gl_renderer.cpp are untouched by this file and remain the "no projection
/// regression" check called for by the same test rule.

using namespace xt;

namespace {

// -------------------------------------------------------------------------------------------
// Offscreen render target + readback (mirrors test_gl_renderer.cpp; duplicated rather than
// shared since each xt_gl_tests translation unit keeps its own anonymous namespace).
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

/// Renders `cmds` (viewport already `w`x`h`) with xt::gl::GlRenderer into a fresh offscreen FBO and
/// returns a tightly-packed, top-down RGBA8 buffer (glReadPixels itself returns bottom-to-top rows;
/// flipped here so it directly compares against xt::sw::SwRenderer's row order).
std::vector<uint8_t> renderGlTopDown(const CommandList& cmds, int w, int h, AssetResolver* assets = nullptr,
                                     const IrView* ir = nullptr) {
    static bool loaded = false;
    if (!loaded) {
        REQUIRE(xt::gl::loadGl(sdlLoadProc));
        loaded = true;
    }
    GlTestTarget target;
    target.init(w, h);
    target.bindAndClear(Color{0, 0, 0, 0});

    gl::GlRenderer renderer;
    REQUIRE(renderer.init(sdlLoadProc));
    renderer.resize(w, h);
    target.bindAndClear(Color{0, 0, 0, 0});  // resize()/init() may have touched GL state; re-clear.
    renderer.render(cmds, nullptr, assets, ir);
    std::vector<uint8_t> pixels = target.readback();
    renderer.shutdown();
    target.destroy();

    std::vector<uint8_t> topDown(pixels.size());
    for (int y = 0; y < h; ++y) {
        size_t srcRow = static_cast<size_t>(h - 1 - y) * static_cast<size_t>(w) * 4;
        size_t dstRow = static_cast<size_t>(y) * static_cast<size_t>(w) * 4;
        std::copy(pixels.begin() + static_cast<long>(srcRow),
                  pixels.begin() + static_cast<long>(srcRow) + w * 4,
                  topDown.begin() + static_cast<long>(dstRow));
    }
    return topDown;
}

/// Renders `cmds` (viewport already `w`x`h`) with xt::sw::SwRenderer into a tightly-packed,
/// top-down RGBA8 buffer, matching renderGlTopDown()'s row order.
std::vector<uint8_t> renderSw(const CommandList& cmds, int w, int h, AssetResolver* assets = nullptr,
                              const IrView* ir = nullptr) {
    std::vector<uint8_t> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    sw::SwRenderer renderer;
    sw::SwTarget target{buf.data(), w, h, w * 4};
    renderer.setTarget(target);
    renderer.clear(Color{0, 0, 0, 0});
    renderer.render(cmds, nullptr, assets, ir);
    return buf;
}

int countFilled(const std::vector<uint8_t>& buf, int w, int h) {
    int n = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (buf[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4 + 3] >
                128) {
                ++n;
            }
        }
    }
    return n;
}

/// A trivial in-test AssetResolver, identical in spirit to tests/unit/test_sw_3d.cpp's: looks a
/// decoded image up by AssetId directly, ignoring the IrView.
class TestAssetResolver : public AssetResolver {
public:
    AssetId add(ImageData data) {
        AssetId id = static_cast<AssetId>(m_images.size());
        m_images.push_back(std::move(data));
        return id;
    }

    bool readBytes(std::string_view, std::vector<uint8_t>&) override { return false; }

    bool decodeImage(AssetId asset, const IrView&, ImageData& out) override {
        if (asset >= m_images.size()) {
            return false;
        }
        out = m_images[asset];
        return true;
    }

    Vec2 imageSize(AssetId asset, const IrView& view) override {
        ImageData data;
        if (!decodeImage(asset, view, data)) {
            return Vec2{0, 0};
        }
        return Vec2{static_cast<float>(data.width), static_cast<float>(data.height)};
    }

private:
    std::vector<ImageData> m_images;
};

/// A 4x4 checkerboard, opaque everywhere (alpha 255): texel (x, y) is white when (x + y) is even,
/// black otherwise. Same fixture as tests/unit/test_sw_3d.cpp's makeCheckerboard4x4.
ImageData makeCheckerboard4x4() {
    ImageData img;
    img.width = 4;
    img.height = 4;
    img.rgba.resize(4 * 4 * 4);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            uint8_t v = ((x + y) % 2 == 0) ? 255 : 0;
            size_t i = (static_cast<size_t>(y) * 4 + static_cast<size_t>(x)) * 4;
            img.rgba[i + 0] = v;
            img.rgba[i + 1] = v;
            img.rgba[i + 2] = v;
            img.rgba[i + 3] = 255;
        }
    }
    return img;
}

// -------------------------------------------------------------------------------------------
// CommandList builder helpers, mirroring tests/unit/test_sw_3d.cpp's exactly (duplicated: xt_sw's
// test helpers are file-local to that translation unit).
// -------------------------------------------------------------------------------------------

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

void addDrawImage(CommandList& cl, AssetId asset, Rect dst) {
    ImageCmd c{};
    c.xf = Mat2x3{};
    c.asset = asset;
    c.dst = dst;
    c.srcUv = Rect{0, 0, 1, 1};
    c.opacity = 1;
    c.smooth = 0;
    cl.images.push_back(c);
    Command cmd{};
    cmd.kind = CmdKind::DrawImage;
    cmd.index = static_cast<uint32_t>(cl.images.size() - 1);
    cl.commands.push_back(cmd);
}

void addDrawImageQuad(CommandList& cl, AssetId asset, const Vec2 p[4]) {
    ImageQuadCmd c{};
    c.asset = asset;
    for (int i = 0; i < 4; ++i) {
        c.p[i] = p[i];
    }
    c.srcUv = Rect{0, 0, 1, 1};
    c.opacity = 1;
    c.smooth = 0;
    cl.imageQuads.push_back(c);
    Command cmd{};
    cmd.kind = CmdKind::DrawImageQuad;
    cmd.index = static_cast<uint32_t>(cl.imageQuads.size() - 1);
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

constexpr int kW = 64, kH = 64;

}  // namespace

// 1. No projection: DrawImageQuad with an axis-aligned rect quad, GL vs SW.
TEST_CASE("gl 3D: unprojected DrawImageQuad matches SW") {
    TestAssetResolver resolver;
    AssetId asset = resolver.add(makeCheckerboard4x4());
    IrView view{};

    CommandList cl;
    cl.viewport = Rect{0, 0, kW, kH};
    cl.devicePixelRatio = 1;
    Vec2 quad[4] = {Vec2{8, 8}, Vec2{24, 8}, Vec2{24, 24}, Vec2{8, 24}};
    addDrawImageQuad(cl, asset, quad);

    std::vector<uint8_t> sw = renderSw(cl, kW, kH, &resolver, &view);
    std::vector<uint8_t> gl = renderGlTopDown(cl, kW, kH, &resolver, &view);
    golden::GoldenResult result = golden::compareRgba(gl.data(), sw.data(), kW, kH, 16, 0.03f);
    CHECK(result.pass);
}

// 2. translateZ = 100 under cssPerspective(200, (32,32)) magnifies a centered fill, GL vs SW.
TEST_CASE("gl 3D: translateZ=100 magnifies a centered fill, matches SW") {
    CommandList cl;
    cl.viewport = Rect{0, 0, kW, kH};
    cl.devicePixelRatio = 1;
    Mat4 viewProj = cssPerspectiveViewProj(200.0f, Vec2{32, 32}).viewProj;
    addPushProjection(cl, viewProj);
    addPushTransform3D(cl, mat4Translate(Vec3{0, 0, 100}));
    addFillRect(cl, 22, 22, 20, 20, Color{1, 1, 1, 1});
    addPopTransform3D(cl);
    addPopProjection(cl);

    std::vector<uint8_t> sw = renderSw(cl, kW, kH);
    std::vector<uint8_t> gl = renderGlTopDown(cl, kW, kH);
    golden::GoldenResult result = golden::compareRgba(gl.data(), sw.data(), kW, kH, 16, 0.03f);
    CHECK(result.pass);
}

// 3. translateZ = -200 shrinks the same square to half size, GL vs SW.
TEST_CASE("gl 3D: translateZ=-200 shrinks a centered fill, matches SW") {
    CommandList cl;
    cl.viewport = Rect{0, 0, kW, kH};
    cl.devicePixelRatio = 1;
    Mat4 viewProj = cssPerspectiveViewProj(200.0f, Vec2{32, 32}).viewProj;
    addPushProjection(cl, viewProj);
    addPushTransform3D(cl, mat4Translate(Vec3{0, 0, -200}));
    addFillRect(cl, 22, 22, 20, 20, Color{1, 1, 1, 1});
    addPopTransform3D(cl);
    addPopProjection(cl);

    std::vector<uint8_t> sw = renderSw(cl, kW, kH);
    std::vector<uint8_t> gl = renderGlTopDown(cl, kW, kH);
    golden::GoldenResult result = golden::compareRgba(gl.data(), sw.data(), kW, kH, 16, 0.03f);
    CHECK(result.pass);
}

// 4. rotateY(60deg) DrawImage, perspective-warped quad, GL vs SW.
TEST_CASE("gl 3D: rotateY(60deg) DrawImage matches SW") {
    TestAssetResolver resolver;
    AssetId asset = resolver.add(makeCheckerboard4x4());
    IrView view{};

    CommandList cl;
    cl.viewport = Rect{0, 0, kW, kH};
    cl.devicePixelRatio = 1;
    Mat4 viewProj = cssPerspectiveViewProj(150.0f, Vec2{0, 0}).viewProj;
    addPushProjection(cl, viewProj, translation(Vec2{32, 32}));
    addPushTransform3D(cl, mat4RotateY(60.0f));
    addDrawImage(cl, asset, Rect{-20, -20, 40, 40});
    addPopTransform3D(cl);
    addPopProjection(cl);

    std::vector<uint8_t> sw = renderSw(cl, kW, kH, &resolver, &view);
    std::vector<uint8_t> gl = renderGlTopDown(cl, kW, kH, &resolver, &view);
    golden::GoldenResult result = golden::compareRgba(gl.data(), sw.data(), kW, kH, 16, 0.03f);
    CHECK(result.pass);
}

// 5. A projected PushClipRect restricts a much larger fill, GL vs SW.
TEST_CASE("gl 3D: projected PushClipRect matches SW") {
    CommandList cl;
    cl.viewport = Rect{0, 0, kW, kH};
    cl.devicePixelRatio = 1;
    Mat4 viewProj = cssPerspectiveViewProj(200.0f, Vec2{32, 32}).viewProj;
    addPushProjection(cl, viewProj);
    addPushTransform3D(cl, mat4Translate(Vec3{0, 0, 100}));
    addPushClipRect(cl, Rect{22, 22, 20, 20});
    addFillRect(cl, -64, -64, 192, 192, Color{1, 1, 1, 1});  // much larger than the viewport
    addPopClip(cl);
    addPopTransform3D(cl);
    addPopProjection(cl);

    std::vector<uint8_t> sw = renderSw(cl, kW, kH);
    std::vector<uint8_t> gl = renderGlTopDown(cl, kW, kH);
    golden::GoldenResult result = golden::compareRgba(gl.data(), sw.data(), kW, kH, 16, 0.03f);
    CHECK(result.pass);
}

// 6. Geometry beyond the camera (w<=0 after the homogeneous divide) is dropped entirely on both
// backends: nothing is drawn, and rendering does not crash.
TEST_CASE("gl 3D: geometry past the camera is dropped on both backends, no crash") {
    CommandList cl;
    cl.viewport = Rect{0, 0, kW, kH};
    cl.devicePixelRatio = 1;
    Mat4 viewProj = cssPerspectiveViewProj(200.0f, Vec2{32, 32}).viewProj;
    addPushProjection(cl, viewProj);
    addPushTransform3D(cl, mat4Translate(Vec3{0, 0, 300}));
    addFillRect(cl, 22, 22, 20, 20, Color{1, 1, 1, 1});
    addPopTransform3D(cl);
    addPopProjection(cl);

    std::vector<uint8_t> sw = renderSw(cl, kW, kH);
    std::vector<uint8_t> gl = renderGlTopDown(cl, kW, kH);
    CHECK(countFilled(sw, kW, kH) == 0);
    CHECK(countFilled(gl, kW, kH) == 0);
    golden::GoldenResult result = golden::compareRgba(gl.data(), sw.data(), kW, kH, 16, 0.03f);
    CHECK(result.pass);
}

// 7. Content drawn after PopProjection (and PopTransform3D) matches SW, which in turn is
// pixel-identical to unprojected rendering (tests/unit/test_sw_3d.cpp's case 7) -- so this also
// exercises the "mapping stack fully unwound" rule on the GL side.
TEST_CASE("gl 3D: content after PopProjection matches SW") {
    CommandList cl;
    cl.viewport = Rect{0, 0, kW, kH};
    cl.devicePixelRatio = 1;
    Mat4 viewProj = cssPerspectiveViewProj(200.0f, Vec2{32, 32}).viewProj;
    addPushProjection(cl, viewProj);
    addPushTransform3D(cl, mat4Translate(Vec3{0, 0, 100}));
    addPopTransform3D(cl);
    addPopProjection(cl);
    addFillRect(cl, 10, 10, 20, 20, Color{0, 1, 0, 1});

    std::vector<uint8_t> sw = renderSw(cl, kW, kH);
    std::vector<uint8_t> gl = renderGlTopDown(cl, kW, kH);
    golden::GoldenResult result = golden::compareRgba(gl.data(), sw.data(), kW, kH, 16, 0.03f);
    CHECK(result.pass);
}
