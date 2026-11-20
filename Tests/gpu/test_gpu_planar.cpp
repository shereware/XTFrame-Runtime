// T-806 (Docs/design/23-host-integration.md §23.aj, Docs/tasks/T-806.md "測試"): planar external
// textures in the frame builder, against a recording device that answers supportsPlanarExternal()
// either way -- the pure conversion function (matrix x range on standard code values, chroma siting,
// the per-plane uv mapping and clamp), what a frame holds with and without the capability, that a
// frame without planar bindings is the frame of before, one PlanarParams block per binding (a
// nine-slice and a tiled background stay one command), the reuse checks and the not-drawn cases.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"
#include "planar_params.h"  // Code/Runtime/gpu/src: the pure function under test
#include "planar_scenes.h"  // Tests/common
#include "xt/command_list.h"
#include "xt/gpu/frame_builder.h"
#include "xt/gpu/gpu_device.h"
#include "xt/gpu/gpu_frame.h"

using namespace xt;
using namespace xt::gpu;
namespace pt = xt::planar_test;

namespace {

using Planes = AssetResolver::ExternalPlanes;
using Matrix = AssetResolver::ExternalMatrix;
using Range = AssetResolver::ExternalRange;
using Siting = AssetResolver::ChromaSiting;
using Layout = AssetResolver::ExternalLayout;

/// RGB of 8-bit codes (y, u, v) through planarConversion, clamped like the shaders.
void decode(Matrix m, Range r, int y, int u, int v, float rgb[3]) {
    const gpu::detail::PlanarMatrix pm = gpu::detail::planarConversion(m, r);
    const float in[4] = {static_cast<float>(y) / 255.0f, static_cast<float>(u) / 255.0f,
                         static_cast<float>(v) / 255.0f, 1.0f};
    for (int row = 0; row < 3; ++row) {
        float s = 0;
        for (int c = 0; c < 4; ++c) {
            s += pm.m[row][c] * in[c];
        }
        rgb[row] = std::fmin(std::fmax(s, 0.0f), 1.0f);
    }
}

struct Code {
    int y, u, v;
    float r, g, b;
};

/// Hands out texture handles; executes nothing; answers the planar capability as told.
class PlanarDevice : public GpuDevice {
public:
    explicit PlanarDevice(bool planar) : m_planar(planar) {}
    bool supportsPlanarExternal() const override { return m_planar; }
    bool supportsCoverageDraws() const override { return true; }
    bool supportsBoxShadowShading() const override { return true; }
    bool supportsAnalyticClip() const override { return true; }
    TextureHandle createTexture(const char*, uint16_t, uint16_t, TextureFormat, const void*) override {
        return m_next++;
    }
    void updateTexture(TextureHandle, uint16_t, uint16_t, uint16_t, uint16_t, const void*, size_t) override {}
    void destroyTexture(TextureHandle) override {}
    void submit(const GpuFrame&) override {}

private:
    bool m_planar;
    TextureHandle m_next = 1000;
};

constexpr TextureHandle kHostTexture = 77;

struct Build {
    PlanarDevice device;
    FrameBuilder builder;
    GpuFrame frame;
    explicit Build(bool planar) : device(planar) {
        builder.init(device);
        builder.resize(pt::kFrameW, pt::kFrameH);
    }
    ~Build() { builder.shutdown(); }
    void run(const pt::Painted& p, AssetResolver& assets) {
        builder.build(p.list, nullptr, &assets, &p.doc->ir(), frame);
    }
};

std::vector<const DrawCmd*> withShading(const GpuFrame& f, Shading s) {
    std::vector<const DrawCmd*> out;
    for (const DrawCmd& c : f.cmds) {
        if (c.shading == s) out.push_back(&c);
    }
    return out;
}

std::string dumpOf(const GpuFrame& f) {
    std::string s;
    dumpGpuFrame(f, s);
    return s;
}

/// `scene` painted with `engine://scene` bound to kHostTexture with the planes of the smooth picture.
void bindPlanar(pt::SceneResolver& assets, const Planes& planes) {
    assets.setExternalTexture("scene", kHostTexture, pt::kPicW, pt::kPicH);
    assets.setExternalTexturePlanes("scene", planes);
}

Planes smoothPlanes() { return pt::planesOf(pt::smoothPicture(), Layout::PackedYuv420).planes; }

const pt::Scene& scene(const char* name) {
    for (const pt::Scene& s : pt::kScenes) {
        if (std::string(s.name) == name) return s;
    }
    return pt::kScenes[0];
}

bool paintScene(const char* name, AssetResolver& assets, pt::Painted& out) {
    const pt::Scene& s = scene(name);
    return pt::paint(s.body, assets, out, s.style);
}

}  // namespace

TEST_CASE("planar params: the conversion matrix decodes the standard codes of four matrix x range") {
    struct Case {
        Matrix m;
        Range r;
        Code codes[5];
    };
    // White, black and the three primaries as 8-bit codes (rounded from the definitions).
    const Case cases[] = {
        {Matrix::Bt709,
         Range::Limited,
         {{235, 128, 128, 1, 1, 1},
          {16, 128, 128, 0, 0, 0},
          {63, 102, 240, 1, 0, 0},
          {173, 42, 26, 0, 1, 0},
          {32, 240, 118, 0, 0, 1}}},
        {Matrix::Bt601,
         Range::Limited,
         {{235, 128, 128, 1, 1, 1},
          {16, 128, 128, 0, 0, 0},
          {81, 90, 240, 1, 0, 0},
          {145, 54, 34, 0, 1, 0},
          {41, 240, 110, 0, 0, 1}}},
        {Matrix::Bt709,
         Range::Full,
         {{255, 128, 128, 1, 1, 1},
          {0, 128, 128, 0, 0, 0},
          {54, 99, 255, 1, 0, 0},
          {182, 30, 12, 0, 1, 0},
          {18, 255, 116, 0, 0, 1}}},
        {Matrix::Bt601,
         Range::Full,
         {{255, 128, 128, 1, 1, 1},
          {0, 128, 128, 0, 0, 0},
          {76, 85, 255, 1, 0, 0},
          {150, 44, 21, 0, 1, 0},
          {29, 255, 107, 0, 0, 1}}},
    };
    for (const Case& c : cases) {
        for (const Code& k : c.codes) {
            CAPTURE(int(c.m));
            CAPTURE(int(c.r));
            CAPTURE(k.y);
            CAPTURE(k.u);
            CAPTURE(k.v);
            float rgb[3];
            decode(c.m, c.r, k.y, k.u, k.v, rgb);
            // Half a code of rounding in each of Y, U, V: at most about 0.008 after the matrix.
            CHECK(std::fabs(rgb[0] - k.r) <= 0.01f);
            CHECK(std::fabs(rgb[1] - k.g) <= 0.01f);
            CHECK(std::fabs(rgb[2] - k.b) <= 0.01f);
        }
    }
    // Neutral chroma is exactly grey, and the matrix has no branch-dependent shape: G row reads both.
    const gpu::detail::PlanarMatrix m = gpu::detail::planarConversion(Matrix::Bt709, Range::Limited);
    CHECK(m.m[0][1] == 0.0f);
    CHECK(m.m[2][2] == 0.0f);
    CHECK(m.m[1][1] < 0.0f);
    CHECK(m.m[1][2] < 0.0f);
}

TEST_CASE("planar params: chroma siting offsets, per-plane uv mapping and the half-texel clamp") {
    CHECK(gpu::detail::chromaSitingOffset(Siting::Center).dx == 0.0f);
    CHECK(gpu::detail::chromaSitingOffset(Siting::Center).dy == 0.0f);
    CHECK(gpu::detail::chromaSitingOffset(Siting::Left).dx == 0.25f);
    CHECK(gpu::detail::chromaSitingOffset(Siting::Left).dy == 0.0f);
    CHECK(gpu::detail::chromaSitingOffset(Siting::TopLeft).dx == 0.25f);
    CHECK(gpu::detail::chromaSitingOffset(Siting::TopLeft).dy == 0.25f);

    // The packer's layout of a 48 x 32 picture: Y on top, U | V below, A below them (48 x 80).
    const Planes p = pt::planesOf(pt::alphaPicture(), Layout::PackedYuva420, Matrix::Bt601, Range::Full,
                                  Siting::TopLeft, true)
                         .planes;
    REQUIRE(p.textureWidth == 48);
    REQUIRE(p.textureHeight == 80);
    const PlanarParams pp = gpu::detail::planarParams(p);
    const float W = 48, H = 80;
    // Y: picture uv onto its own rectangle.
    CHECK(pp.uv[0][0] == doctest::Approx(48 / W));
    CHECK(pp.uv[0][1] == doctest::Approx(32 / H));
    CHECK(pp.uv[0][2] == doctest::Approx(0.0f));
    CHECK(pp.uv[0][3] == doctest::Approx(0.0f));
    // U: half the luma size per texel, the siting folded into the offset; V the same, 24 texels on.
    CHECK(pp.uv[1][0] == doctest::Approx(24 / W));
    CHECK(pp.uv[1][1] == doctest::Approx(16 / H));
    CHECK(pp.uv[1][2] == doctest::Approx(0.25f / W));
    CHECK(pp.uv[1][3] == doctest::Approx((32 + 0.25f) / H));
    CHECK(pp.uv[2][2] == doctest::Approx((24 + 0.25f) / W));
    CHECK(pp.uv[2][3] == doctest::Approx((32 + 0.25f) / H));
    // Luma pixel 0's centre lands on chroma texel 0's centre (top-left siting).
    const float u0 = 0.5f / 48.0f, v0 = 0.5f / 32.0f;
    CHECK((u0 * pp.uv[1][0] + pp.uv[1][2]) * W == doctest::Approx(0.5f));
    CHECK((v0 * pp.uv[1][1] + pp.uv[1][3]) * H == doctest::Approx(32.5f));
    // A: like Y, 48 rows lower.
    CHECK(pp.uv[3][3] == doctest::Approx(48 / H));
    // Clamps: each rectangle inset by half a texel.
    const float y0[4] = {0.5f / W, 0.5f / H, 47.5f / W, 31.5f / H};
    const float uc[4] = {0.5f / W, 32.5f / H, 23.5f / W, 47.5f / H};
    const float vc[4] = {24.5f / W, 32.5f / H, 47.5f / W, 47.5f / H};
    const float ac[4] = {0.5f / W, 48.5f / H, 47.5f / W, 79.5f / H};
    for (int i = 0; i < 4; ++i) {
        CHECK(pp.clampRect[0][i] == doctest::Approx(y0[i]));
        CHECK(pp.clampRect[1][i] == doctest::Approx(uc[i]));
        CHECK(pp.clampRect[2][i] == doctest::Approx(vc[i]));
        CHECK(pp.clampRect[3][i] == doctest::Approx(ac[i]));
    }
    CHECK(pp.hasAlpha == 1.0f);
    CHECK(pp.premultiplied == 1.0f);
    // Without an alpha plane: no A mapping, no flags (premultiplied means nothing then).
    Planes noAlpha = p;
    noAlpha.layout = Layout::PackedYuv420;
    const PlanarParams pn = gpu::detail::planarParams(noAlpha);
    CHECK(pn.hasAlpha == 0.0f);
    CHECK(pn.premultiplied == 0.0f);
    CHECK(pn.uv[3][0] == 0.0f);
    CHECK(pn.clampRect[3][2] == 0.0f);
}

TEST_CASE("planar frame: a device without the capability draws nothing for a planar binding") {
    pt::SceneResolver assets;
    bindPlanar(assets, smoothPlanes());
    pt::Painted p;
    REQUIRE(paintScene("image", assets, p));
    Build b(false);
    b.run(p, assets);
    CHECK(withShading(b.frame, Shading::TexturePlanar).empty());
    CHECK(withShading(b.frame, Shading::Texture).empty());
    CHECK(withShading(b.frame, Shading::TextureStraight).empty());
    CHECK(b.builder.lastBuildStats().planarSkipped == 2u);
    CHECK(std::string(b.builder.lastError()).empty());  // the host's part, not an error
}

TEST_CASE("planar frame: without planar bindings a capable device gets the frame of before, byte for byte") {
    for (const pt::Scene& scene : pt::kScenes) {
        CAPTURE(scene.name);
        pt::SceneResolver assets;
        assets.setExternalTexture("scene", kHostTexture, pt::kPicW, pt::kPicH, /*straightAlpha=*/true);
        pt::Painted p;
        REQUIRE(pt::paint(scene.body, assets, p, scene.style));
        Build without(false), with(true);
        without.run(p, assets);
        with.run(p, assets);
        CHECK(dumpOf(with.frame) == dumpOf(without.frame));
        CHECK(with.frame.params == without.frame.params);
        REQUIRE(with.frame.vertices.size() == without.frame.vertices.size());
        CHECK(std::memcmp(with.frame.vertices.data(), without.frame.vertices.data(),
                          with.frame.vertices.size() * sizeof(Vertex)) == 0);
        CHECK(with.frame.indices == without.frame.indices);
        CHECK(with.builder.lastBuildStats().planarSkipped == 0u);
    }
}

TEST_CASE("planar frame: TexturePlanar with the host texture and one PlanarParams block per binding") {
    pt::SceneResolver assets;
    const Planes planes = smoothPlanes();
    bindPlanar(assets, planes);
    pt::Painted p;
    REQUIRE(paintScene("image", assets, p));
    Build b(true);
    b.run(p, assets);
    const auto planar = withShading(b.frame, Shading::TexturePlanar);
    // Two images of one binding: one state, one command (T-155's batching), one block.
    REQUIRE(planar.size() == 1);
    CHECK(planar[0]->texture == kHostTexture);
    CHECK(planar[0]->mipLod == 0);
    CHECK(planar[0]->sampler == Sampler::LinearClamp);
    REQUIRE(size_t(planar[0]->paramIndex) + sizeof(PlanarParams) / sizeof(float) <= b.frame.params.size());
    const PlanarParams expected = gpu::detail::planarParams(planes);
    CHECK(std::memcmp(b.frame.params.data() + planar[0]->paramIndex, &expected, sizeof(expected)) == 0);
    CHECK(b.frame.params.size() == sizeof(PlanarParams) / sizeof(float));  // nothing else pushed one
    CHECK(withShading(b.frame, Shading::Texture).empty());
    CHECK(withShading(b.frame, Shading::TextureStraight).empty());
    CHECK(dumpOf(b.frame).find("TexturePlanar SrcOver TestEqualRef ref=0 planar=ok alpha=0 premul=0") !=
          std::string::npos);
    CHECK(std::string(b.builder.lastError()).empty());
    // The picture's own uv on the vertices (the planes' mapping is the block's).
    const Vertex& v2 = b.frame.vertices[planar[0]->baseVertex + 2];
    CHECK(v2.u == 1.0f);
    CHECK(v2.v == 1.0f);

    // Image-rendering: pixelated samples the planes with the nearest-point sampler.
    pt::Painted px;
    REQUIRE(paintScene("pixelated", assets, px));
    b.run(px, assets);
    const auto nearest = withShading(b.frame, Shading::TexturePlanar);
    REQUIRE(nearest.size() == 1);
    CHECK(nearest[0]->sampler == Sampler::PointClamp);
}

TEST_CASE("planar frame: a nine-slice and a tiled background keep one command over one block") {
    pt::SceneResolver assets;
    bindPlanar(assets, smoothPlanes());
    for (const char* name : {"nine-slice", "tiled"}) {
        CAPTURE(name);
        pt::Painted p;
        REQUIRE(paintScene(name, assets, p));
        size_t draws = 0;
        for (const Command& c : p.list.commands) {
            draws += c.kind == CmdKind::DrawImage ? 1 : 0;
        }
        Build b(true);
        b.run(p, assets);
        const auto planar = withShading(b.frame, Shading::TexturePlanar);
        REQUIRE(planar.size() == 1);
        if (std::string(name) == "nine-slice") {
            CHECK(draws == 9);
        } else {
            CHECK(draws == 12);  // 140 x 100 over 48 x 32 tiles: 3 x 4
        }
        CHECK(planar[0]->vertexCount == 4 * draws);
        CHECK(b.frame.params.size() == sizeof(PlanarParams) / sizeof(float));
    }
}

TEST_CASE("planar frame: a description change makes the frame not reusable, a held version keeps it") {
    pt::SceneResolver assets;
    Planes planes = smoothPlanes();
    bindPlanar(assets, planes);
    assets.setExternalTextureVersion("scene", 1);
    pt::Painted p;
    REQUIRE(paintScene("image", assets, p));
    Build b(true);
    b.run(p, assets);
    CHECK(b.builder.lastFrameReusable(&assets, &p.doc->ir()));
    planes.matrix = Matrix::Bt601;  // the version stays: the description alone moved
    assets.setExternalTexturePlanes("scene", planes);
    CHECK_FALSE(b.builder.lastFrameReusable(&assets, &p.doc->ir()));
    b.run(p, assets);
    CHECK(b.builder.lastFrameReusable(&assets, &p.doc->ir()));

    // Without a version (a texture that may change every frame) the description is checked too.
    pt::SceneResolver plain;
    bindPlanar(plain, smoothPlanes());
    pt::Painted q;
    REQUIRE(paintScene("image", plain, q));
    Build c(true);
    c.run(q, plain);
    CHECK(c.builder.lastFrameReusable(&plain, &q.doc->ir()));
    plain.setExternalTexturePlanes("scene", {});
    CHECK_FALSE(c.builder.lastFrameReusable(&plain, &q.doc->ir()));
}

TEST_CASE("planar frame: an invalid description and a planar particle texture draw nothing, said once") {
    pt::SceneResolver assets;
    Planes bad = smoothPlanes();
    bad.v.x = bad.textureWidth;  // V starts past the texture
    bindPlanar(assets, bad);
    pt::Painted p;
    REQUIRE(paintScene("image", assets, p));
    Build b(true);
    b.run(p, assets);
    CHECK(withShading(b.frame, Shading::TexturePlanar).empty());
    CHECK(b.builder.lastBuildStats().planarSkipped == 2u);
    CHECK(std::string(b.builder.lastError()).find("plane description") != std::string::npos);
    b.run(p, assets);
    CHECK(std::string(b.builder.lastError()).empty());  // once per device
    CHECK(b.builder.lastBuildStats().planarSkipped == 2u);

    // Particles over a (valid) planar binding: ParticleTexture's paramIndex is its own block.
    assets.setExternalTexturePlanes("scene", smoothPlanes());
    REQUIRE(!p.list.images.empty());
    CommandList cl;
    cl.viewport = Rect{0, 0, float(pt::kFrameW), float(pt::kFrameH)};
    cl.devicePixelRatio = 1;
    xt::ParticleInstance inst{};
    inst.x = 40;
    inst.y = 40;
    inst.w = 16;
    inst.h = 16;
    inst.rgba = Color{1, 1, 1, 1};
    inst.uv1 = Vec2{1, 1};
    cl.particleInstances.push_back(inst);
    ParticlesCmd pc{};
    pc.texture = p.list.images[0].asset;
    pc.blend = ParticleBlend::Normal;
    pc.instanceOffset = 0;
    pc.count = 1;
    cl.particles.push_back(pc);
    cl.commands.push_back(Command{CmdKind::DrawParticles, {}, 0});
    Build c(true);
    c.builder.build(cl, nullptr, &assets, &p.doc->ir(), c.frame);
    for (const DrawCmd& cmd : c.frame.cmds) {
        CHECK(cmd.kind != DrawKind::Particles);
    }
    CHECK(c.builder.lastBuildStats().planarSkipped == 1u);
    CHECK(std::string(c.builder.lastError()).find("particles") != std::string::npos);
}
