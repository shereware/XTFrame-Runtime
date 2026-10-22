#include <cmath>
#include <cstdint>
#include <vector>

#include "camera.h"  // Code/Runtime/core/src internal header: cssPerspectiveViewProj (Tests/CMakeLists.txt adds Code/Runtime/core/src).
#include "doctest.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/ir.h"
#include "xt/node.h"
#include "xt/sw/sw_renderer.h"
#include "xt/types.h"

/// SwRenderer 3D projection integration tests (PushProjection/PopProjection, PushTransform3D/
/// PopTransform3D, DrawImageQuad, and Fill/Stroke/Clip/DrawImage under projection). Hand-filled
/// CommandLists rendered to a 64x64 target, quantized pixel assertions (no goldens). See
/// Docs/design/19-3d-space.md §19.7, Docs/tasks/T-45.md "測試".

using namespace xt;
using namespace xt::sw;

namespace {

struct Rgba {
    uint8_t r, g, b, a;
};

Rgba pixelAt(const std::vector<uint8_t>& buf, int width, int x, int y) {
    size_t i = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
    return Rgba{buf[i], buf[i + 1], buf[i + 2], buf[i + 3]};
}

struct Fixture {
    static constexpr int w = 64, h = 64;
    std::vector<uint8_t> buf;
    SwRenderer renderer;
    SwTarget target;

    Fixture() : buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0), target{buf.data(), w, h, w * 4} {
        renderer.setTarget(target);
        renderer.clear(Color{0, 0, 0, 0});
    }
};

/// A trivial in-test AssetResolver, identical in spirit to test_sw_stroke_image.cpp's: looks a
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
/// black otherwise. Same fixture as test_sw_stroke_image.cpp's makeCheckerboard4x4.
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

int countFilled(const std::vector<uint8_t>& buf, int w, int h) {
    int n = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (pixelAt(buf, w, x, y).a > 128) {
                ++n;
            }
        }
    }
    return n;
}

int columnHeight(const std::vector<uint8_t>& buf, int w, int h, int x) {
    int n = 0;
    for (int y = 0; y < h; ++y) {
        if (pixelAt(buf, w, x, y).a > 128) {
            ++n;
        }
    }
    return n;
}

}  // namespace

// 1. No projection: DrawImageQuad with an axis-aligned rect quad matches the same-size DrawImage,
// pixel for pixel.
TEST_CASE("SwRenderer 3D: unprojected DrawImageQuad matches DrawImage for an axis-aligned rect") {
    TestAssetResolver resolver;
    AssetId asset = resolver.add(makeCheckerboard4x4());
    IrView view{};

    Fixture fxImage;
    CommandList clImage;
    clImage.viewport = Rect{0, 0, 64, 64};
    clImage.devicePixelRatio = 1;
    addDrawImage(clImage, asset, Rect{8, 8, 16, 16});
    fxImage.renderer.render(clImage, nullptr, &resolver, &view);

    Fixture fxQuad;
    CommandList clQuad;
    clQuad.viewport = Rect{0, 0, 64, 64};
    clQuad.devicePixelRatio = 1;
    Vec2 quad[4] = {Vec2{8, 8}, Vec2{24, 8}, Vec2{24, 24}, Vec2{8, 24}};
    addDrawImageQuad(clQuad, asset, quad);
    fxQuad.renderer.render(clQuad, nullptr, &resolver, &view);

    for (int y = 0; y < Fixture::h; ++y) {
        for (int x = 0; x < Fixture::w; ++x) {
            Rgba a = pixelAt(fxImage.buf, Fixture::w, x, y);
            Rgba b = pixelAt(fxQuad.buf, Fixture::w, x, y);
            CHECK(a.r == b.r);
            CHECK(a.g == b.g);
            CHECK(a.b == b.b);
            CHECK(a.a == b.a);
        }
    }
}

// 2. translateZ = 100 under cssPerspective(200, (32,32)) doubles a 20x20 square centered at
// (32, 32) into a 40x40 square (still centered at (32, 32)): 1 / (1 - 100/200) = 2.
TEST_CASE("SwRenderer 3D: translateZ=100 magnifies a centered fill 2x") {
    Fixture fx;
    CommandList cl;
    cl.viewport = Rect{0, 0, 64, 64};
    cl.devicePixelRatio = 1;
    Mat4 viewProj = cssPerspectiveViewProj(200.0f, Vec2{32, 32}).viewProj;
    addPushProjection(cl, viewProj);
    addPushTransform3D(cl, mat4Translate(Vec3{0, 0, 100}));
    addFillRect(cl, 22, 22, 20, 20, Color{1, 1, 1, 1});
    addPopTransform3D(cl);
    addPopProjection(cl);
    fx.renderer.render(cl, nullptr, nullptr);

    int filled = countFilled(fx.buf, Fixture::w, Fixture::h);
    CHECK(filled >= static_cast<int>(1600 * 0.95));
    CHECK(filled <= static_cast<int>(1600 * 1.05));
    CHECK(pixelAt(fx.buf, Fixture::w, 32, 32).r == 255);
}

// 3. translateZ = -200 shrinks the same square to 10x10 (+/- 1px): 1 / (1 - (-200)/200) = 1/2.
TEST_CASE("SwRenderer 3D: translateZ=-200 shrinks a centered fill to half size") {
    Fixture fx;
    CommandList cl;
    cl.viewport = Rect{0, 0, 64, 64};
    cl.devicePixelRatio = 1;
    Mat4 viewProj = cssPerspectiveViewProj(200.0f, Vec2{32, 32}).viewProj;
    addPushProjection(cl, viewProj);
    addPushTransform3D(cl, mat4Translate(Vec3{0, 0, -200}));
    addFillRect(cl, 22, 22, 20, 20, Color{1, 1, 1, 1});
    addPopTransform3D(cl);
    addPopProjection(cl);
    fx.renderer.render(cl, nullptr, nullptr);

    int minX = -1, maxX = -1;
    for (int x = 0; x < Fixture::w; ++x) {
        if (pixelAt(fx.buf, Fixture::w, x, 32).a > 128) {
            if (minX < 0) {
                minX = x;
            }
            maxX = x;
        }
    }
    REQUIRE(minX >= 0);
    int width = maxX - minX + 1;
    CHECK(width >= 9);
    CHECK(width <= 11);
    CHECK(pixelAt(fx.buf, Fixture::w, 32, 32).r == 255);
}

// 4. rotateY(60deg) applied to a DrawImage centered at the local origin, projected through
// cssPerspective(150, (0,0)) then shifted to screen center by rootXf: the near (left, +x screen)
// side is magnified and the far (right) side shrunk, so the right column is visibly narrower than
// the left; the quad's interior has no unfilled holes.
TEST_CASE("SwRenderer 3D: rotateY(60deg) DrawImage is narrower on the far (right) side") {
    TestAssetResolver resolver;
    AssetId asset = resolver.add(makeCheckerboard4x4());
    IrView view{};

    Fixture fx;
    CommandList cl;
    cl.viewport = Rect{0, 0, 64, 64};
    cl.devicePixelRatio = 1;
    Mat4 viewProj = cssPerspectiveViewProj(150.0f, Vec2{0, 0}).viewProj;
    addPushProjection(cl, viewProj, translation(Vec2{32, 32}));
    addPushTransform3D(cl, mat4RotateY(60.0f));
    addDrawImage(cl, asset, Rect{-20, -20, 40, 40});
    addPopTransform3D(cl);
    addPopProjection(cl);
    fx.renderer.render(cl, nullptr, &resolver, &view);

    // Analytic quad corners (see Docs/tasks/T-45.md worker report): left edge x ~= 20.7, right
    // edge x ~= 41.0; left column taller (~45px) than right column (~36px).
    int leftHeight = columnHeight(fx.buf, Fixture::w, Fixture::h, 24);
    int rightHeight = columnHeight(fx.buf, Fixture::w, Fixture::h, 38);
    CHECK(leftHeight - rightHeight > 4);

    // No holes: 10 interior sample points, all clearly inside the projected quad.
    int xs[] = {26, 30, 34, 26, 30, 34, 26, 30, 34, 30};
    int ys[] = {20, 20, 20, 30, 30, 30, 40, 40, 40, 45};
    for (int i = 0; i < 10; ++i) {
        CHECK(pixelAt(fx.buf, Fixture::w, xs[i], ys[i]).a > 128);
    }
}

// 5. A projected PushClipRect restricts a much larger fill to the clip rect's own projected area
// (same doubling setup as test 2: 20x20 -> 40x40 -> 1600px, +/- 5%).
TEST_CASE("SwRenderer 3D: projected PushClipRect clips to the projected rect's area") {
    Fixture fx;
    CommandList cl;
    cl.viewport = Rect{0, 0, 64, 64};
    cl.devicePixelRatio = 1;
    Mat4 viewProj = cssPerspectiveViewProj(200.0f, Vec2{32, 32}).viewProj;
    addPushProjection(cl, viewProj);
    addPushTransform3D(cl, mat4Translate(Vec3{0, 0, 100}));
    addPushClipRect(cl, Rect{22, 22, 20, 20});
    addFillRect(cl, -64, -64, 192, 192, Color{1, 1, 1, 1});  // much larger than the viewport
    addPopClip(cl);
    addPopTransform3D(cl);
    addPopProjection(cl);
    fx.renderer.render(cl, nullptr, nullptr);

    int filled = countFilled(fx.buf, Fixture::w, Fixture::h);
    CHECK(filled >= static_cast<int>(1600 * 0.95));
    CHECK(filled <= static_cast<int>(1600 * 1.05));
}

// 6. Geometry beyond the camera (translateZ=300 > cssPerspective's d=200, so w <= 0 after the
// homogeneous divide) is dropped entirely: nothing is drawn, and rendering does not crash.
TEST_CASE("SwRenderer 3D: geometry past the camera (w<=0) is dropped, not drawn, no crash") {
    Fixture fx;
    CommandList cl;
    cl.viewport = Rect{0, 0, 64, 64};
    cl.devicePixelRatio = 1;
    Mat4 viewProj = cssPerspectiveViewProj(200.0f, Vec2{32, 32}).viewProj;
    addPushProjection(cl, viewProj);
    addPushTransform3D(cl, mat4Translate(Vec3{0, 0, 300}));
    addFillRect(cl, 22, 22, 20, 20, Color{1, 1, 1, 1});
    addPopTransform3D(cl);
    addPopProjection(cl);
    fx.renderer.render(cl, nullptr, nullptr);

    CHECK(countFilled(fx.buf, Fixture::w, Fixture::h) == 0);
}

// 7. Content drawn after PopProjection (and PopTransform3D) is pixel-identical to the same content
// rendered with no projection at all: the mapping stack must be fully unwound.
TEST_CASE("SwRenderer 3D: content after PopProjection matches unprojected rendering exactly") {
    Fixture fxProjected;
    CommandList clProjected;
    clProjected.viewport = Rect{0, 0, 64, 64};
    clProjected.devicePixelRatio = 1;
    Mat4 viewProj = cssPerspectiveViewProj(200.0f, Vec2{32, 32}).viewProj;
    addPushProjection(clProjected, viewProj);
    addPushTransform3D(clProjected, mat4Translate(Vec3{0, 0, 100}));
    addPopTransform3D(clProjected);
    addPopProjection(clProjected);
    addFillRect(clProjected, 10, 10, 20, 20, Color{0, 1, 0, 1});
    fxProjected.renderer.render(clProjected, nullptr, nullptr);

    Fixture fxPlain;
    CommandList clPlain;
    clPlain.viewport = Rect{0, 0, 64, 64};
    clPlain.devicePixelRatio = 1;
    addFillRect(clPlain, 10, 10, 20, 20, Color{0, 1, 0, 1});
    fxPlain.renderer.render(clPlain, nullptr, nullptr);

    CHECK(fxProjected.buf == fxPlain.buf);
}

namespace {

/// A left-to-right linear gradient fill, red (x) -> blue (x + w), in the path's own coordinates.
void addGradientRect(CommandList& cl, float x, float y, float w, float h) {
    PathRef path = addRectPath(cl, x, y, w, h);
    FillCmd f{};
    f.xf = Mat2x3{};
    f.path = path;
    f.paint.kind = PaintKind::Linear;
    f.paint.color = Color{1, 1, 1, 1};
    f.paint.p0 = Vec2{x, y};
    f.paint.p1 = Vec2{x + w, y};
    f.paint.stopOffset = static_cast<uint32_t>(cl.stops.size());
    cl.stops.push_back(GradientStop{0.0f, Color{1, 0, 0, 1}});
    cl.stops.push_back(GradientStop{1.0f, Color{0, 0, 1, 1}});
    f.paint.stopCount = 2;
    f.rule = FillRule::NonZero;
    f.antialias = 1;
    cl.fills.push_back(f);
    Command cmd{};
    cmd.kind = CmdKind::FillPath;
    cmd.index = static_cast<uint32_t>(cl.fills.size() - 1);
    cl.commands.push_back(cmd);
}

}  // namespace

// 8. T-160 (bench BUG-5): a gradient on a 3D layer is evaluated in the layer's plane, not in screen
// space. The layer is moved 30 px right by its world3; the fill spans plane x 0..20, so on screen
// it covers x 30..50 and its middle (screen x 40) is halfway between red and blue. Before the fix
// the paint was sampled at the screen position (x 40 > 20), so the whole rect came out blue.
TEST_CASE("SwRenderer 3D: a gradient fill on a translated 3D layer follows the plane") {
    Fixture fx;
    CommandList cl;
    cl.viewport = Rect{0, 0, 64, 64};
    cl.devicePixelRatio = 1;
    Mat4 viewProj = cssPerspectiveViewProj(200.0f, Vec2{32, 32}).viewProj;
    addPushProjection(cl, viewProj);
    addPushTransform3D(cl, mat4Translate(Vec3{30, 0, 0}));
    addGradientRect(cl, 0, 20, 20, 20);
    addPopTransform3D(cl);
    addPopProjection(cl);
    fx.renderer.render(cl, nullptr, nullptr);

    Rgba left = pixelAt(fx.buf, Fixture::w, 31, 30);
    Rgba mid = pixelAt(fx.buf, Fixture::w, 40, 30);
    Rgba right = pixelAt(fx.buf, Fixture::w, 48, 30);
    CHECK(left.a == 255);
    CHECK(left.r > 200);
    CHECK(left.b < 55);
    CHECK(mid.r > 100);
    CHECK(mid.r < 155);
    CHECK(mid.b > 100);
    CHECK(mid.b < 155);
    CHECK(right.r < 55);
    CHECK(right.b > 200);
}

// 9. T-160: under a real perspective (translateZ = 100 doubles the size, see test 2) the gradient
// is stretched with the geometry: plane x 22..42 lands on screen x 12..52, and screen x 32 (the
// center) is still the gradient's midpoint.
TEST_CASE("SwRenderer 3D: a gradient fill scales with a perspective-magnified layer") {
    Fixture fx;
    CommandList cl;
    cl.viewport = Rect{0, 0, 64, 64};
    cl.devicePixelRatio = 1;
    Mat4 viewProj = cssPerspectiveViewProj(200.0f, Vec2{32, 32}).viewProj;
    addPushProjection(cl, viewProj);
    addPushTransform3D(cl, mat4Translate(Vec3{0, 0, 100}));
    addGradientRect(cl, 22, 22, 20, 20);
    addPopTransform3D(cl);
    addPopProjection(cl);
    fx.renderer.render(cl, nullptr, nullptr);

    Rgba left = pixelAt(fx.buf, Fixture::w, 20, 32);
    Rgba mid = pixelAt(fx.buf, Fixture::w, 32, 32);
    // Screen x 20.5 is plane x 26.25 (t ~ 0.21, red ~ 200); sampled in screen space it would be
    // plane x 20.5, clamped to t = 0 (pure red).
    CHECK(left.r > 180);
    CHECK(left.r < 228);
    CHECK(mid.r > 100);
    CHECK(mid.r < 155);
}
