#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "doctest.h"
#include "test_temp.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/compile/ir_writer.h"
#include "xt/ir.h"
#include "xt/node.h"
#include "xt/sw/sw_png.h"
#include "xt/sw/sw_renderer.h"
#include "xt/types.h"

/// SwRenderer StrokePath/dash and DrawImage integration tests, plus FileAssetResolver. See
/// docs/tasks/T-12.md "測試" / "test_sw_stroke_image.cpp".

using namespace xt;
using namespace xt::sw;

namespace {

PathRef addOpenLine(CommandList& cl, Vec2 p0, Vec2 p1) {
    PathRef ref{};
    ref.verbOffset = static_cast<uint32_t>(cl.verbs.size());
    ref.pointOffset = static_cast<uint32_t>(cl.points.size());
    cl.verbs.push_back(PathVerb::Move);
    cl.verbs.push_back(PathVerb::Line);
    float pts[4] = {p0.x, p0.y, p1.x, p1.y};
    cl.points.insert(cl.points.end(), pts, pts + 4);
    ref.verbCount = 2;
    ref.pointCount = 4;
    return ref;
}

void addStroke(CommandList& cl, const PathRef& path, Color color, float width, LineCap cap = LineCap::Butt,
               std::vector<float> dashes = {}, float dashPhase = 0) {
    StrokeCmd s{};
    s.xf = Mat2x3{};
    s.path = path;
    s.paint.kind = PaintKind::Solid;
    s.paint.color = color;
    s.width = width;
    s.cap = cap;
    s.join = LineJoin::Miter;
    s.miterLimit = 4;
    s.dashPhase = dashPhase;
    if (!dashes.empty()) {
        s.dashOffset = static_cast<uint32_t>(cl.dashes.size());
        s.dashCount = static_cast<uint32_t>(dashes.size());
        cl.dashes.insert(cl.dashes.end(), dashes.begin(), dashes.end());
    }
    cl.strokes.push_back(s);
    Command cmd{};
    cmd.kind = CmdKind::StrokePath;
    cmd.index = static_cast<uint32_t>(cl.strokes.size() - 1);
    cl.commands.push_back(cmd);
}

void addDrawImage(CommandList& cl, AssetId asset, Rect dst, Rect srcUv, bool smooth, Mat2x3 xf = Mat2x3{}) {
    ImageCmd c{};
    c.xf = xf;
    c.asset = asset;
    c.dst = dst;
    c.srcUv = srcUv;
    c.opacity = 1;
    c.smooth = smooth ? 1 : 0;
    cl.images.push_back(c);
    Command cmd{};
    cmd.kind = CmdKind::DrawImage;
    cmd.index = static_cast<uint32_t>(cl.images.size() - 1);
    cl.commands.push_back(cmd);
}

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

/// A trivial in-test AssetResolver that ignores the IrView entirely and looks the decoded image up
/// by AssetId directly; used so most DrawImage tests below don't need a real .xtir blob (only the
/// FileAssetResolver test at the bottom does).
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

/// A 4x4 checkerboard: texel (x, y) is white when (x + y) is even, black otherwise.
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

}  // namespace

TEST_CASE("StrokePath: width 4 horizontal line produces an exact 4-pixel-tall band") {
    Fixture fx;
    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(fx.w), static_cast<float>(fx.h)};
    cl.devicePixelRatio = 1;
    addStroke(cl, addOpenLine(cl, Vec2{10, 32}, Vec2{54, 32}), Color{1, 1, 1, 1}, 4);
    fx.renderer.render(cl, nullptr, nullptr);

    // Centerline at y=32, half-width 2 -> band covers device rows [30, 34).
    CHECK(pixelAt(fx.buf, fx.w, 32, 30).a > 200);
    CHECK(pixelAt(fx.buf, fx.w, 32, 31).a > 200);
    CHECK(pixelAt(fx.buf, fx.w, 32, 32).a > 200);
    CHECK(pixelAt(fx.buf, fx.w, 32, 33).a > 200);
    CHECK(pixelAt(fx.buf, fx.w, 32, 29).a == 0);
    CHECK(pixelAt(fx.buf, fx.w, 32, 34).a == 0);
}

TEST_CASE("StrokePath: dash {4, 4} alternates on/off along the line") {
    Fixture fx;
    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(fx.w), static_cast<float>(fx.h)};
    cl.devicePixelRatio = 1;
    addStroke(cl, addOpenLine(cl, Vec2{0, 32}, Vec2{40, 32}), Color{1, 1, 1, 1}, 4, LineCap::Butt, {4, 4});
    fx.renderer.render(cl, nullptr, nullptr);

    // First "on" run: x in [0, 4). First "off" run: x in [4, 8). Second "on": x in [8, 12).
    CHECK(pixelAt(fx.buf, fx.w, 2, 32).a > 200);
    CHECK(pixelAt(fx.buf, fx.w, 6, 32).a == 0);
    CHECK(pixelAt(fx.buf, fx.w, 10, 32).a > 200);
}

TEST_CASE("StrokePath: Round cap extends coverage past the open contour's endpoint") {
    Fixture fx;
    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(fx.w), static_cast<float>(fx.h)};
    cl.devicePixelRatio = 1;
    // Line ends at x=30; width 4 -> cap radius 2. A Butt cap would stop exactly at x=30.
    addStroke(cl, addOpenLine(cl, Vec2{10, 32}, Vec2{30, 32}), Color{1, 1, 1, 1}, 4, LineCap::Round);
    fx.renderer.render(cl, nullptr, nullptr);

    // Just past the endpoint, well inside the cap's radius-2 half disk.
    CHECK(pixelAt(fx.buf, fx.w, 31, 32).a > 200);
}

TEST_CASE("DrawImage: 4x4 checkerboard to 8x8, nearest neighbor, each cell becomes 2x2") {
    Fixture fx;
    TestAssetResolver resolver;
    AssetId asset = resolver.add(makeCheckerboard4x4());
    IrView view{};

    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(fx.w), static_cast<float>(fx.h)};
    cl.devicePixelRatio = 1;
    addDrawImage(cl, asset, Rect{0, 0, 8, 8}, Rect{0, 0, 1, 1}, false);
    fx.renderer.render(cl, nullptr, &resolver, &view);

    // Source texel (0,0) is white; occupies output pixels [0,2)x[0,2).
    CHECK(pixelAt(fx.buf, fx.w, 0, 0).r == 255);
    CHECK(pixelAt(fx.buf, fx.w, 1, 1).r == 255);
    // Source texel (1,0) is black; occupies output pixels [2,4)x[0,2).
    CHECK(pixelAt(fx.buf, fx.w, 2, 0).r == 0);
    CHECK(pixelAt(fx.buf, fx.w, 3, 1).r == 0);
}

TEST_CASE("DrawImage: smooth downscale 4x4 -> 2x2 averages each 2x2 neighborhood") {
    Fixture fx;
    TestAssetResolver resolver;
    AssetId asset = resolver.add(makeCheckerboard4x4());
    IrView view{};

    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(fx.w), static_cast<float>(fx.h)};
    cl.devicePixelRatio = 1;
    addDrawImage(cl, asset, Rect{0, 0, 2, 2}, Rect{0, 0, 1, 1}, true);
    fx.renderer.render(cl, nullptr, &resolver, &view);

    // Every 2x2 block of the checkerboard has 2 white + 2 black texels -> average mid-gray.
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            Rgba p = pixelAt(fx.buf, fx.w, x, y);
            CHECK(std::abs(static_cast<int>(p.r) - 128) <= 5);
        }
    }
}

TEST_CASE("DrawImage: rotated 90 degrees maps texels to the expected device position") {
    Fixture fx;
    ImageData img;
    img.width = 2;
    img.height = 2;
    img.rgba = {
        255, 0,   0,   255,  // (0,0) red
        0,   255, 0,   255,  // (1,0) green
        0,   0,   255, 255,  // (0,1) blue
        255, 255, 255, 255,  // (1,1) white
    };
    TestAssetResolver resolver;
    AssetId asset = resolver.add(img);
    IrView view{};

    // dst is a 2x2 square centered on the origin; xf rotates 90 degrees about the origin then
    // translates the whole thing to the canvas center.
    Rect dst{-1, -1, 2, 2};
    Mat2x3 xf = mul(translation(Vec2{32, 32}), rotationDeg(90));

    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(fx.w), static_cast<float>(fx.h)};
    cl.devicePixelRatio = 1;
    addDrawImage(cl, asset, dst, Rect{0, 0, 1, 1}, false, xf);
    fx.renderer.render(cl, nullptr, &resolver, &view);

    // Texel (0, 0) (red) is centered at local (-0.5, -0.5); map it through xf to find where it
    // lands on the device, and check that pixel is red.
    Vec2 texelCenterLocal{-0.5f, -0.5f};
    Vec2 device = apply(xf, texelCenterLocal);
    int px = static_cast<int>(std::floor(device.x));
    int py = static_cast<int>(std::floor(device.y));
    Rgba p = pixelAt(fx.buf, fx.w, px, py);
    CHECK(p.r == 255);
    CHECK(p.g == 0);
    CHECK(p.b == 0);
}

TEST_CASE("FileAssetResolver: decodeImage reads a PNG written by writePng, size is correct") {
    const int w = 6, h = 5;
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    for (size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i + 0] = 10;
        rgba[i + 1] = 20;
        rgba[i + 2] = 30;
        rgba[i + 3] = 255;
    }

    std::filesystem::path dir = xt::test::tempDir();
    std::string fileName = "xt_t12_test_asset.png";
    std::filesystem::path fullPath = dir / fileName;
    REQUIRE(writePng(fullPath.string(), rgba.data(), w, h, w * 4));

    compile::IrWriter writer;
    StringId nameId = writer.intern(fileName);
    compile::AssetBuilder assetBuilder;
    assetBuilder.name = nameId;
    assetBuilder.kind = 0;
    // `embedded` left empty: this asset is resolved externally by name.
    AssetId assetId = writer.addAsset(assetBuilder);
    std::vector<uint8_t> irBytes = writer.finish();

    IrBlob blob{irBytes.data(), irBytes.size()};
    Result<IrView> viewResult = IrView::open(blob);
    REQUIRE(viewResult.ok());
    IrView view = viewResult.value();

    FileAssetResolver resolver(dir.string(), decodeImageBytes);
    ImageData data;
    bool ok = resolver.decodeImage(assetId, view, data);
    CHECK(ok);
    CHECK(data.width == w);
    CHECK(data.height == h);

    std::error_code ec;
    std::filesystem::remove(fullPath, ec);
}

// --- docs/tasks/T-149.md: image sampling fast paths -------------------------------------------

namespace {

/// A 16x12 image with a deterministic, high-contrast pattern and varying alpha, so a one-texel
/// sampling slip shows up as a large channel difference.
ImageData makePatternImage() {
    ImageData img;
    img.width = 16;
    img.height = 12;
    img.rgba.resize(static_cast<size_t>(img.width) * static_cast<size_t>(img.height) * 4);
    uint32_t seed = 12345u;
    for (size_t i = 0; i < img.rgba.size(); i += 4) {
        seed = seed * 1664525u + 1013904223u;
        img.rgba[i + 0] = static_cast<uint8_t>(seed >> 24);
        img.rgba[i + 1] = static_cast<uint8_t>((seed >> 16) & 0xFF);
        img.rgba[i + 2] = static_cast<uint8_t>((seed >> 8) & 0xFF);
        // Mostly opaque with a few translucent texels so both the opaque store and the src-over
        // kernels are exercised.
        img.rgba[i + 3] = static_cast<uint8_t>((i / 4) % 7 == 0 ? 96 : 255);
    }
    return img;
}

/// Renders `img` with `xf` (dst = 40x30 at the origin, full srcUv) into a fresh 64x64 canvas
/// over a mid-gray backdrop and returns the RGBA8 buffer.
std::vector<uint8_t> renderImage(const ImageData& img, Mat2x3 xf, bool smooth, float opacity = 1.0f) {
    Fixture fx;
    fx.renderer.clear(Color{0.5f, 0.5f, 0.5f, 1.0f});
    TestAssetResolver resolver;
    AssetId asset = resolver.add(img);
    IrView view{};
    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(fx.w), static_cast<float>(fx.h)};
    cl.devicePixelRatio = 1;
    addDrawImage(cl, asset, Rect{0, 0, 40, 30}, Rect{0, 0, 1, 1}, smooth, xf);
    cl.images.back().opacity = opacity;
    fx.renderer.render(cl, nullptr, &resolver, &view);
    return fx.buf;
}

/// Largest per-channel difference between two equally sized RGBA8 buffers.
int maxChannelDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    int worst = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        worst = std::max(worst, std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
    }
    return worst;
}

/// The same transform with a 1e-7 shear: mathematically the same command (the shear moves a
/// pixel by well under 1e-5 px), but it routes DrawImage through the general per-pixel path
/// instead of the axis-aligned one.
Mat2x3 forceGeneralPath(Mat2x3 xf) {
    xf.b = 1e-7f;
    xf.c = 1e-7f;
    return xf;
}

}  // namespace

TEST_CASE("DrawImage T-149: axis-aligned fast path matches the general path within 1/255") {
    const ImageData img = makePatternImage();
    struct Case {
        const char* name;
        Mat2x3 xf;
    };
    // Translations are chosen so no pixel centre lands within 0.01 texel of a texel boundary:
    // nearest-neighbour sampling is discontinuous there, and the two paths (one exact in 32.32
    // fixed point, one in float) may legitimately pick different sides of such a tie.
    const Case cases[] = {
        {"identity, sub-pixel offset", Mat2x3{1, 0, 0, 1, 0.137f, 0.211f}},
        {"upscale, sub-pixel offset", Mat2x3{1.3f, 0, 0, 1.1f, 5.637f, 7.711f}},
        {"negative x scale (flip)", Mat2x3{-1.0f, 0, 0, 1.0f, 45.387f, 3.113f}},
        {"negative y scale, partially off-canvas", Mat2x3{1.0f, 0, 0, -1.5f, -10.137f, 60.211f}},
        {"downscale", Mat2x3{0.35f, 0, 0, 0.5f, 2.2f, 2.7f}},
    };
    for (const Case& c : cases) {
        for (bool smooth : {true, false}) {
            for (float opacity : {1.0f, 0.6f}) {
                CAPTURE(c.name);
                CAPTURE(smooth);
                CAPTURE(opacity);
                const std::vector<uint8_t> fast = renderImage(img, c.xf, smooth, opacity);
                const std::vector<uint8_t> general =
                    renderImage(img, forceGeneralPath(c.xf), smooth, opacity);
                CHECK(maxChannelDiff(fast, general) <= 1);
                // Something was actually drawn (the backdrop is uniform gray).
                bool touched = false;
                for (size_t i = 0; i < fast.size(); i += 4) {
                    touched = touched || fast[i] != 128 || fast[i + 1] != 128 || fast[i + 2] != 128;
                }
                CHECK(touched);
            }
        }
    }
}

TEST_CASE("DrawImage T-149: minifying a solid-colour image outputs that colour everywhere") {
    ImageData img;
    img.width = 64;
    img.height = 64;
    img.rgba.resize(64 * 64 * 4);
    for (size_t i = 0; i < img.rgba.size(); i += 4) {
        img.rgba[i + 0] = 30;
        img.rgba[i + 1] = 200;
        img.rgba[i + 2] = 90;
        img.rgba[i + 3] = 255;
    }
    for (bool smooth : {true, false}) {
        CAPTURE(smooth);
        // dst 40x30 scaled by 0.2 -> 8x6 device pixels: every device pixel reads 8 texels along
        // x, which takes the strong-minification branch of the axis-aligned path.
        Fixture fx;
        TestAssetResolver resolver;
        AssetId asset = resolver.add(img);
        IrView view{};
        CommandList cl;
        cl.viewport = Rect{0, 0, static_cast<float>(fx.w), static_cast<float>(fx.h)};
        cl.devicePixelRatio = 1;
        addDrawImage(cl, asset, Rect{0, 0, 40, 30}, Rect{0, 0, 1, 1}, smooth, Mat2x3{0.2f, 0, 0, 0.2f, 0, 0});
        fx.renderer.render(cl, nullptr, &resolver, &view);
        for (int y = 0; y < 6; ++y) {
            for (int x = 0; x < 8; ++x) {
                Rgba p = pixelAt(fx.buf, fx.w, x, y);
                CHECK(p.r == 30);
                CHECK(p.g == 200);
                CHECK(p.b == 90);
                CHECK(p.a == 255);
            }
        }
        CHECK(pixelAt(fx.buf, fx.w, 8, 0).a == 0);
        CHECK(pixelAt(fx.buf, fx.w, 0, 6).a == 0);
    }
}

TEST_CASE("DrawImage T-149: a translucent solid image blends through the src-over kernel") {
    ImageData img;
    img.width = 8;
    img.height = 8;
    img.rgba.resize(8 * 8 * 4);
    for (size_t i = 0; i < img.rgba.size(); i += 4) {
        img.rgba[i + 0] = 255;
        img.rgba[i + 1] = 0;
        img.rgba[i + 2] = 0;
        img.rgba[i + 3] = 128;
    }
    Fixture fx;
    fx.renderer.clear(Color{0, 0, 1, 1});
    TestAssetResolver resolver;
    AssetId asset = resolver.add(img);
    IrView view{};
    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(fx.w), static_cast<float>(fx.h)};
    cl.devicePixelRatio = 1;
    addDrawImage(cl, asset, Rect{0, 0, 16, 16}, Rect{0, 0, 1, 1}, true);
    fx.renderer.render(cl, nullptr, &resolver, &view);
    // 50% red over opaque blue: (128, 0, 127).
    Rgba p = pixelAt(fx.buf, fx.w, 4, 4);
    CHECK(std::abs(static_cast<int>(p.r) - 128) <= 1);
    CHECK(p.g == 0);
    CHECK(std::abs(static_cast<int>(p.b) - 127) <= 1);
    CHECK(p.a == 255);
}
