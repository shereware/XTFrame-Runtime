#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "compile_check.h"
#include "doctest.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/image_mips.h"
#include "xt/ir.h"
#include "xt/sw/sw_renderer.h"
#include "xt/types.h"

/// T-835 (Docs/design/10-backends.md, "10.2.5 補正"): the software renderer's bilinear and trilinear
/// filters interpolate premultiplied texels, as the GPU does, so a translucent edge never shows the
/// colour stored in the transparent texels next to it. The oracle: the same image with its
/// transparent texels holding the opaque neighbour's colour (red) draws the premultiplied result
/// under either order of filter and premultiply; the image whose transparent texels hold green must
/// draw the same pixels (within 1), and green may never exceed blue over the grey backdrop.

using namespace xt;
using namespace xt::sw;

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr int kW = 96;
constexpr int kH = 64;
const Color kGrey{0.5f, 0.5f, 0.5f, 1.0f};

class PremulTestResolver : public AssetResolver {
public:
    AssetId add(ImageData data) {
        m_images.push_back(std::move(data));
        return static_cast<AssetId>(m_images.size() - 1);
    }
    bool readBytes(std::string_view, std::vector<uint8_t>&) override { return false; }
    bool decodeImage(AssetId asset, const IrView&, ImageData& out) override {
        if (asset >= m_images.size()) {
            return false;
        }
        out = m_images[asset];
        return true;
    }
    Vec2 imageSize(AssetId asset, const IrView&) override {
        return asset < m_images.size() ? Vec2{static_cast<float>(m_images[asset].width),
                                              static_cast<float>(m_images[asset].height)}
                                       : Vec2{0, 0};
    }
    const ImageData* imageView(AssetId asset, const IrView&) override {
        return asset < m_images.size() ? &m_images[asset] : nullptr;
    }
    const CompressedImage* compressedImage(AssetId asset, const IrView&, float) override {
        return asset == compressedAsset ? &compressed : nullptr;
    }
    AssetId compressedAsset = kInvalidAsset;
    CompressedImage compressed;

private:
    std::vector<ImageData> m_images;
};

void setTexel(ImageData& img, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    uint8_t* p = img.rgba.data() +
                 (static_cast<size_t>(y) * static_cast<size_t>(img.width) + static_cast<size_t>(x)) * 4;
    p[0] = r;
    p[1] = g;
    p[2] = b;
    p[3] = a;
}

/// The task's 2x1 image: left opaque red, right transparent holding `transparentRgb` (green for the
/// case under test, red for the reference).
ImageData edge2x1(bool greenInTransparent) {
    ImageData img;
    img.width = 2;
    img.height = 1;
    img.rgba.assign(2 * 4, 0);
    setTexel(img, 0, 0, 255, 0, 0, 255);
    if (greenInTransparent) {
        setTexel(img, 1, 0, 0, 255, 0, 0);
    } else {
        setTexel(img, 1, 0, 255, 0, 0, 0);
    }
    return img;
}

/// 32x32 columns alternating opaque red / transparent (green or red): every minified pixel is an edge.
ImageData stripes(bool greenInTransparent) {
    ImageData img;
    img.width = 32;
    img.height = 32;
    img.rgba.assign(32 * 32 * 4, 0);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            if (x % 2 == 0) {
                setTexel(img, x, y, 255, 0, 0, 255);
            } else if (greenInTransparent) {
                setTexel(img, x, y, 0, 255, 0, 0);
            } else {
                setTexel(img, x, y, 255, 0, 0, 0);
            }
        }
    }
    return img;
}

CommandList newList() {
    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(kW), static_cast<float>(kH)};
    cl.devicePixelRatio = 1;
    return cl;
}

void addImage(CommandList& cl, AssetId asset, Rect dst, Mat2x3 xf, uint8_t smooth = 1) {
    ImageCmd c{};
    c.xf = xf;
    c.asset = asset;
    c.dst = dst;
    c.srcUv = Rect{0, 0, 1, 1};
    c.opacity = 1;
    c.smooth = smooth;
    cl.images.push_back(c);
    cl.commands.push_back(Command{CmdKind::DrawImage, {}, static_cast<uint32_t>(cl.images.size() - 1)});
}

void addQuad(CommandList& cl, AssetId asset, const Vec2 (&p)[4]) {
    ImageQuadCmd q{};
    q.asset = asset;
    for (int i = 0; i < 4; ++i) {
        q.p[i] = p[i];
    }
    q.srcUv = Rect{0, 0, 1, 1};
    q.opacity = 1;
    q.smooth = 1;
    cl.imageQuads.push_back(q);
    cl.commands.push_back(
        Command{CmdKind::DrawImageQuad, {}, static_cast<uint32_t>(cl.imageQuads.size() - 1)});
}

void addMesh(CommandList& cl, AssetId asset, const std::vector<MeshVertex>& verts) {
    const std::vector<uint16_t> indices{0, 1, 2, 0, 2, 3};
    ImageMeshCmd mc{};
    mc.asset = asset;
    mc.vertexOffset = static_cast<uint32_t>(cl.meshVertices.size());
    mc.vertexCount = static_cast<uint32_t>(verts.size());
    mc.indexOffset = static_cast<uint32_t>(cl.meshIndices.size());
    mc.indexCount = static_cast<uint32_t>(indices.size());
    mc.opacity = 1;
    mc.smooth = 1;
    cl.meshVertices.insert(cl.meshVertices.end(), verts.begin(), verts.end());
    cl.meshIndices.insert(cl.meshIndices.end(), indices.begin(), indices.end());
    cl.imageMeshes.push_back(mc);
    cl.commands.push_back(
        Command{CmdKind::DrawImageMesh, {}, static_cast<uint32_t>(cl.imageMeshes.size() - 1)});
}

void addParticle(CommandList& cl, AssetId asset) {
    ParticleInstance p{};
    p.x = 48;
    p.y = 32;
    p.w = 56;
    p.h = 40;
    p.rot = 0.3f;
    p.rgba = Color{1, 1, 1, 1};
    p.uv0 = Vec2{0, 0};
    p.uv1 = Vec2{1, 1};
    cl.particleInstances.push_back(p);
    ParticlesCmd pc{};
    pc.xf = Mat2x3{};
    pc.texture = asset;
    pc.blend = ParticleBlend::Normal;
    pc.instanceOffset = static_cast<uint32_t>(cl.particleInstances.size() - 1);
    pc.count = 1;
    cl.particles.push_back(pc);
    cl.commands.push_back(
        Command{CmdKind::DrawParticles, {}, static_cast<uint32_t>(cl.particles.size() - 1)});
}

std::vector<uint8_t> render(const CommandList& cl, AssetResolver& resolver) {
    std::vector<uint8_t> buf(static_cast<size_t>(kW) * kH * 4, 0);
    SwRenderer r;
    SwTarget t{buf.data(), kW, kH, kW * 4};
    r.setTarget(t);
    r.clear(kGrey);
    IrView view{};
    r.render(cl, nullptr, &resolver, &view);
    return buf;
}

Mat2x3 rotateAbout(float deg, float cx, float cy, float scale = 1.0f) {
    const float c = std::cos(deg * kPi / 180.0f) * scale;
    const float s = std::sin(deg * kPi / 180.0f) * scale;
    // translate(cx, cy) * rotate * scale * translate(-cx, -cy)
    return Mat2x3{c, s, -s, c, cx - (c * cx - s * cy), cy - (s * cx + c * cy)};
}

struct EdgeReport {
    int leaks = 0;       // pixels with G > B + 1
    int maxLeak = 0;     // largest G - B
    int refDiff = 0;     // largest channel difference to the reference
    int edgePixels = 0;  // translucent-edge pixels (red neither backdrop nor full)
};

EdgeReport compare(const std::vector<uint8_t>& actual, const std::vector<uint8_t>& reference) {
    EdgeReport r;
    for (size_t i = 0; i < actual.size(); i += 4) {
        const int g = actual[i + 1];
        const int b = actual[i + 2];
        if (g > b + 1) {
            ++r.leaks;
        }
        r.maxLeak = std::max(r.maxLeak, g - b);
        for (size_t k = 0; k < 4; ++k) {
            r.refDiff = std::max(r.refDiff, std::abs(int(actual[i + k]) - int(reference[i + k])));
        }
        const int red = reference[i];
        if (red > 140 && red < 245) {
            ++r.edgePixels;
        }
    }
    return r;
}

/// Renders `build(cl, asset)` once with the green-in-transparent image and once with the reference,
/// and checks the task's two conditions.
template <class Build>
EdgeReport checkPath(const char* name, Build build, bool useStripes = false) {
    PremulTestResolver resolver;
    const AssetId green = resolver.add(useStripes ? stripes(true) : edge2x1(true));
    const AssetId red = resolver.add(useStripes ? stripes(false) : edge2x1(false));
    CommandList a = newList();
    build(a, green);
    CommandList b = newList();
    build(b, red);
    const EdgeReport r = compare(render(a, resolver), render(b, resolver));
    const std::string caseName(name);
    CAPTURE(caseName);
    CAPTURE(r.maxLeak);
    CAPTURE(r.refDiff);
    CHECK(r.edgePixels > 0);  // the case has translucent edge pixels at all
    CHECK(r.leaks == 0);      // G <= B + 1 everywhere
    CHECK(r.refDiff <= 1);    // the premultiplied result, within 1
    return r;
}

int px(const std::vector<uint8_t>& b, int x, int y, int c) {
    return b[(static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4 + static_cast<size_t>(c)];
}

}  // namespace

TEST_CASE("T-835 SW axis-aligned DrawImage: premultiplied bilinear, checked against the formula") {
    checkPath("axis", [](CommandList& cl, AssetId a) { addImage(cl, a, Rect{4, 4, 64, 8}, Mat2x3{}); });
    // The formula itself: texel coordinate fx = (x + 0.5 - 4) * 2 / 64 - 0.5, left weight t; the
    // premultiplied sample (t, 0, 0, t) over grey 128.
    PremulTestResolver resolver;
    const AssetId green = resolver.add(edge2x1(true));
    CommandList cl = newList();
    addImage(cl, green, Rect{4, 4, 64, 8}, Mat2x3{});
    const std::vector<uint8_t> out = render(cl, resolver);
    int worst = 0;
    for (int x = 4; x < 68; ++x) {
        const double fx = (static_cast<double>(x) + 0.5 - 4.0) * 2.0 / 64.0 - 0.5;
        const double t = fx <= 0.0 ? 1.0 : (fx >= 1.0 ? 0.0 : 1.0 - fx);
        const double r = 255.0 * t + 128.0 * (1.0 - t);
        const double gb = 128.0 * (1.0 - t);
        worst = std::max(worst, static_cast<int>(std::ceil(std::fabs(px(out, x, 8, 0) - r) - 0.5)));
        worst = std::max(worst, static_cast<int>(std::ceil(std::fabs(px(out, x, 8, 1) - gb) - 0.5)));
        worst = std::max(worst, static_cast<int>(std::ceil(std::fabs(px(out, x, 8, 2) - gb) - 0.5)));
    }
    CHECK(worst <= 1);
}

TEST_CASE("T-835 SW rotated DrawImage: premultiplied bilinear") {
    checkPath("rotated", [](CommandList& cl, AssetId a) {
        addImage(cl, a, Rect{16, 16, 64, 32}, rotateAbout(20.0f, 48.0f, 32.0f));
    });
}

TEST_CASE("T-835 SW DrawImageQuad (projective): premultiplied bilinear") {
    checkPath("quad", [](CommandList& cl, AssetId a) {
        const Vec2 p[4] = {Vec2{4, 4}, Vec2{88, 10}, Vec2{80, 58}, Vec2{8, 60}};
        addQuad(cl, a, p);
    });
}

TEST_CASE("T-835 SW DrawImageMesh: premultiplied bilinear, sheared and axis-aligned triangles") {
    checkPath("mesh sheared", [](CommandList& cl, AssetId a) {
        addMesh(cl, a,
                {MeshVertex{{10, 6}, {0, 0}}, MeshVertex{{70, 6}, {1, 0}}, MeshVertex{{86, 58}, {1, 1}},
                 MeshVertex{{26, 58}, {0, 1}}});
    });
    checkPath("mesh axis", [](CommandList& cl, AssetId a) {
        addMesh(cl, a,
                {MeshVertex{{4, 4}, {0, 0}}, MeshVertex{{68, 4}, {1, 0}}, MeshVertex{{68, 40}, {1, 1}},
                 MeshVertex{{4, 40}, {0, 1}}});
    });
}

TEST_CASE("T-835 SW particles: the texture is filtered premultiplied") {
    checkPath("particles", [](CommandList& cl, AssetId a) { addParticle(cl, a); });
}

TEST_CASE("T-835 SW trilinear (lod 0.5): level 0 is premultiplied too") {
    const float s = std::pow(2.0f, -0.5f);  // rho sqrt 2 -> lod 0.5
    REQUIRE(imageDrawLod(Mat2x3{s, 0, 0, s, 0, 0}, Rect{0, 0, 32, 32}, Rect{0, 0, 1, 1}, 32, 32) ==
            doctest::Approx(0.5f).epsilon(1e-4));
    checkPath(
        "trilinear axis",
        [s](CommandList& cl, AssetId a) { addImage(cl, a, Rect{0, 0, 32, 32}, Mat2x3{s, 0, 0, s, 8, 8}); },
        true);
    checkPath(
        "trilinear rotated",
        [s](CommandList& cl, AssetId a) {
            addImage(cl, a, Rect{32, 16, 32, 32}, rotateAbout(15.0f, 48.0f, 32.0f, s));
        },
        true);
}

TEST_CASE("T-835 SW block-compressed source keeps straight filtering (as the GPU), nearest is unchanged") {
    // A compressed GPU form without stored levels: the GPU filters it straight, so SW does too --
    // the old formula (t*t, t*(1-t), 0, t) over grey.
    PremulTestResolver resolver;
    const AssetId green = resolver.add(edge2x1(true));
    resolver.compressedAsset = green;
    resolver.compressed.levels = 1;
    CommandList cl = newList();
    addImage(cl, green, Rect{4, 4, 64, 8}, Mat2x3{});
    const std::vector<uint8_t> out = render(cl, resolver);
    int worst = 0;
    int maxLeak = 0;
    for (int x = 4; x < 68; ++x) {
        const double fx = (static_cast<double>(x) + 0.5 - 4.0) * 2.0 / 64.0 - 0.5;
        const double t = fx <= 0.0 ? 1.0 : (fx >= 1.0 ? 0.0 : 1.0 - fx);
        const double r = 255.0 * t * t + 128.0 * (1.0 - t);
        const double g = 255.0 * t * (1.0 - t) + 128.0 * (1.0 - t);
        const double b = 128.0 * (1.0 - t);
        worst = std::max(worst, static_cast<int>(std::ceil(std::fabs(px(out, x, 8, 0) - r) - 0.5)));
        worst = std::max(worst, static_cast<int>(std::ceil(std::fabs(px(out, x, 8, 1) - g) - 0.5)));
        worst = std::max(worst, static_cast<int>(std::ceil(std::fabs(px(out, x, 8, 2) - b) - 0.5)));
        maxLeak = std::max(maxLeak, px(out, x, 8, 1) - px(out, x, 8, 2));
    }
    CHECK(worst <= 1);
    CHECK(maxLeak > 32);

    // Point sampling reads the straight texels as before: left half red, right half the backdrop.
    PremulTestResolver r2;
    const AssetId g2 = r2.add(edge2x1(true));
    CommandList c2 = newList();
    addImage(c2, g2, Rect{4, 4, 64, 8}, Mat2x3{}, 0);
    const std::vector<uint8_t> nearest = render(c2, r2);
    CHECK(px(nearest, 10, 8, 0) == 255);
    CHECK(px(nearest, 10, 8, 1) == 0);
    CHECK(px(nearest, 60, 8, 0) == 128);
    CHECK(px(nearest, 60, 8, 1) == 128);
}

TEST_CASE("T-835 SW image cache: the premultiplied level 0 is counted and built only for smooth draws") {
    PremulTestResolver resolver;
    const AssetId asset = resolver.add(stripes(true));
    std::vector<uint8_t> buf(static_cast<size_t>(kW) * kH * 4, 0);
    SwRenderer r;
    SwTarget t{buf.data(), kW, kH, kW * 4};
    r.setTarget(t);
    IrView view{};
    CommandList point = newList();
    addImage(point, asset, Rect{0, 0, 64, 64}, Mat2x3{}, 0);
    r.render(point, nullptr, &resolver, &view);
    CHECK(r.mipCacheBytes() == 0);
    CommandList smooth = newList();
    addImage(smooth, asset, Rect{0, 0, 64, 64}, Mat2x3{});
    r.render(smooth, nullptr, &resolver, &view);
    // Level 0 premultiplied (32x32) + levels 16x16 .. 1x1.
    const size_t chain = (16 * 16 + 8 * 8 + 4 * 4 + 2 * 2 + 1) * 4;
    CHECK(r.mipCacheBytes() == 32 * 32 * 4 + chain);
    r.invalidateAsset(asset);
    CHECK(r.mipCacheBytes() == 0);
}

namespace {

struct ExternalUi {
    FileAssetResolver assets{std::string(XT_TEST_XT_DIR), nullptr};
    std::unique_ptr<Context> ctx;
    std::vector<uint8_t> ir;
    DocumentHandle handle = 0;
    CommandList list;
    SwRenderer renderer;
    std::vector<uint8_t> buf = std::vector<uint8_t>(static_cast<size_t>(kW) * kH * 4, 0);

    /// One frame: update, render the list, draw it with the same SwRenderer every time.
    void frame() {
        ctx->update(1.0f / 60.0f);
        ctx->render(list, nullptr);
        renderer.clear(kGrey);
        renderer.render(list, nullptr, &assets, nullptr);
    }
};

std::unique_ptr<ExternalUi> openExternal(const ImageData* pixels) {
    auto ui = std::make_unique<ExternalUi>();
    compile::CompileOptions options;
    options.emitMeta = false;
    const std::string text =
        "<xt><head><style>body { margin: 0; padding: 0; } #a { position: absolute; left: 4px; top: 4px; "
        "width: 64px; height: 8px; object-fit: fill; }</style></head>\n<body id=\"Root\"><img id=\"a\" "
        "src=\"engine://scene\"/></body></xt>";
    ui->ir = test::compileMainOk(text, options);
    ui->assets.setExternalPixels("scene", pixels);
    ContextConfig config;
    config.assets = &ui->assets;
    ui->ctx = std::make_unique<Context>(config);
    ui->ctx->setVerifyStaticSkip(false);
    ui->ctx->setViewport(Rect{0, 0, static_cast<float>(kW), static_cast<float>(kH)}, 1.0f);
    Result<DocumentHandle> handle = ui->ctx->loadDocument(IrBlob{ui->ir.data(), ui->ir.size()}, false);
    REQUIRE(handle.ok());
    ui->handle = handle.value();
    SwTarget t{ui->buf.data(), kW, kH, kW * 4};
    ui->renderer.setTarget(t);
    return ui;
}

int maxLeakRow(const std::vector<uint8_t>& b, int y) {
    int m = -255;
    for (int x = 0; x < kW; ++x) {
        m = std::max(m, px(b, x, y, 1) - px(b, x, y, 2));
    }
    return m;
}

}  // namespace

TEST_CASE("T-835 SW host CPU pixels: premultiplied like an image, rebuilt when the content may change") {
    ImageData pixels = edge2x1(true);
    auto ui = openExternal(&pixels);
    ui->frame();
    ui->frame();
    CHECK(maxLeakRow(ui->buf, 8) <= 1);
    CHECK(px(ui->buf, 30, 8, 0) > 140);  // the edge is drawn (red over grey)

    // Unversioned: the host may rewrite the pixels in place every frame -- the next render sees it.
    setTexel(pixels, 0, 0, 0, 0, 255, 255);  // left texel now opaque blue
    ui->frame();
    CHECK(px(ui->buf, 10, 8, 2) == 255);
    CHECK(px(ui->buf, 10, 8, 0) == 0);

    // Versioned: a new version is a new content.
    ui->assets.setExternalTextureVersion("scene", 1);
    ui->frame();
    setTexel(pixels, 0, 0, 255, 0, 0, 255);
    ui->assets.setExternalTextureVersion("scene", 2);
    ui->frame();
    CHECK(px(ui->buf, 10, 8, 0) == 255);
    CHECK(px(ui->buf, 10, 8, 2) == 0);
    CHECK(maxLeakRow(ui->buf, 8) <= 1);

    // A host texture flagged straight alpha is filtered straight by the GPU, so here too.
    ui->assets.setExternalTexture("scene", 0, 2, 1, true);
    ui->frame();
    CHECK(maxLeakRow(ui->buf, 8) > 32);
}
