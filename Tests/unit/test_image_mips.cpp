#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "doctest.h"
#include "sw_mip_cache.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/image_mips.h"
#include "xt/ir.h"
#include "xt/sw/sw_renderer.h"
#include "xt/types.h"

/// T-665 (Docs/design/10-backends.md §10.11 item 7, Docs/tasks/T-665.md): the explicit lod, the mip
/// chain and the software renderer's trilinear sampling.

using namespace xt;
using namespace xt::sw;

namespace {

constexpr float kPi = 3.14159265358979f;

ImageData makeChecker(int w, int h, int cell = 1) {
    ImageData img;
    img.width = w;
    img.height = h;
    img.rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint8_t v = ((x / cell + y / cell) % 2 == 0) ? 255 : 0;
            uint8_t* p = img.rgba.data() +
                         (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
            p[0] = v;
            p[1] = static_cast<uint8_t>(255 - v);
            p[2] = v;
            p[3] = 255;
        }
    }
    return img;
}

class MipTestResolver : public AssetResolver {
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
    const ImageMipChain* imageMipChain(AssetId asset, const IrView&, float) override {
        return asset == ownChainAsset ? &ownChain : nullptr;
    }
    AssetId compressedAsset = kInvalidAsset;
    CompressedImage compressed;
    AssetId ownChainAsset = kInvalidAsset;
    ImageMipChain ownChain;

private:
    std::vector<ImageData> m_images;
};

void addImage(CommandList& cl, AssetId asset, Rect dst, Mat2x3 xf) {
    ImageCmd c{};
    c.xf = xf;
    c.asset = asset;
    c.dst = dst;
    c.srcUv = Rect{0, 0, 1, 1};
    c.opacity = 1;
    c.smooth = 1;
    cl.images.push_back(c);
    Command cmd{};
    cmd.kind = CmdKind::DrawImage;
    cmd.index = static_cast<uint32_t>(cl.images.size() - 1);
    cl.commands.push_back(cmd);
}

std::vector<uint8_t> renderOne(MipTestResolver& resolver, AssetId asset, Rect dst, Mat2x3 xf, int w = 64,
                               int h = 64) {
    std::vector<uint8_t> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    SwRenderer r;
    SwTarget t{buf.data(), w, h, w * 4};
    r.setTarget(t);
    r.clear(Color{0, 0, 0, 0});
    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
    cl.devicePixelRatio = 1;
    addImage(cl, asset, dst, xf);
    IrView view{};
    r.render(cl, nullptr, &resolver, &view);
    return buf;
}

/// s / 4 rounded to nearest, ties to even (xt::downsampleBox).
uint32_t rne4(uint32_t s) {
    return (s >> 2) + (((s & 3u) > 2u || ((s & 3u) == 2u && ((s >> 2) & 1u) != 0)) ? 1u : 0u);
}

int maxDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    int m = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        m = std::max(m, std::abs(int(a[i]) - int(b[i])));
    }
    return m;
}

}  // namespace

TEST_CASE("T-665 imageMipLevelCount: hardware mip sizes down to 1x1") {
    CHECK(imageMipLevelCount(1, 1) == 1);
    CHECK(imageMipLevelCount(2, 1) == 2);
    CHECK(imageMipLevelCount(5, 3) == 3);  // 5x3 -> 2x1 -> 1x1
    CHECK(imageMipLevelCount(1024, 1536) == 11);
    CHECK(imageMipLevelCount(1, 7) == 3);  // 1x7 -> 1x3 -> 1x1
    CHECK(imageMipLevelCount(0, 4) == 0);
}

TEST_CASE("T-665 imageMinifyLod: hand-computed rotation, non-uniform scale, mirror and skew") {
    const Rect unit{0, 0, 64, 64};
    const Rect uv{0, 0, 1, 1};
    // 1:1 and magnification: lod 0.
    CHECK(imageDrawLod(Mat2x3{}, unit, uv, 64, 64) == 0.0f);
    CHECK(imageDrawLod(Mat2x3{2, 0, 0, 2, 0, 0}, unit, uv, 64, 64) == 0.0f);
    // Uniform 0.5: rho 2, lod 1; 0.25: lod 2.
    CHECK(imageDrawLod(Mat2x3{0.5f, 0, 0, 0.5f, 0, 0}, unit, uv, 64, 64) == doctest::Approx(1.0f));
    CHECK(imageDrawLod(Mat2x3{0.25f, 0, 0, 0.25f, 0, 0}, unit, uv, 64, 64) == doctest::Approx(2.0f));
    // A 128-texel image into 64 local px at identity: rho 2.
    CHECK(imageDrawLod(Mat2x3{}, unit, uv, 128, 128) == doctest::Approx(1.0f));
    // Rotation 30 deg at 0.25: the columns keep their length 4 -> lod 2.
    const float c = std::cos(30.0f * kPi / 180.0f) * 0.25f;
    const float s = std::sin(30.0f * kPi / 180.0f) * 0.25f;
    CHECK(imageDrawLod(Mat2x3{c, s, -s, c, 0, 0}, unit, uv, 64, 64) == doctest::Approx(2.0f));
    // Non-uniform 0.5 x 0.25: columns 2 and 4 -> rho 4, lod 2 (the larger axis decides).
    CHECK(imageDrawLod(Mat2x3{0.5f, 0, 0, 0.25f, 0, 0}, unit, uv, 64, 64) == doctest::Approx(2.0f));
    // Non-uniform 0.3 x 0.8: rho = 1 / 0.3.
    CHECK(imageDrawLod(Mat2x3{0.3f, 0, 0, 0.8f, 0, 0}, unit, uv, 64, 64) ==
          doctest::Approx(std::log2(1.0f / 0.3f)));
    // Mirror: scale(-0.5, 0.5) -> lod 1.
    CHECK(imageDrawLod(Mat2x3{-0.5f, 0, 0, 0.5f, 0, 0}, unit, uv, 64, 64) == doctest::Approx(1.0f));
    // skewX(45 deg) = [1 1; 0 1]: inverse [1 -1; 0 1], columns (1, 0) and (-1, 1) -> rho sqrt 2.
    CHECK(imageDrawLod(Mat2x3{1, 0, 1, 1, 0, 0}, unit, uv, 64, 64) == doctest::Approx(0.5f));
    // A sub-rectangle of the texture shown 1:1 stays at lod 0.
    CHECK(imageDrawLod(Mat2x3{}, Rect{0, 0, 32, 32}, Rect{0, 0, 0.5f, 0.5f}, 64, 64) == 0.0f);
    // Singular: lod 0.
    CHECK(imageDrawLod(Mat2x3{0, 0, 0, 0, 0, 0}, unit, uv, 64, 64) == 0.0f);
    // Quads: a parallelogram has a lod, a projective quad has none.
    const Vec2 para[4] = {Vec2{0, 0}, Vec2{32, 0}, Vec2{32, 32}, Vec2{0, 32}};
    float lod = -1.0f;
    CHECK(imageQuadLod(para, uv, 64, 64, lod));
    CHECK(lod == doctest::Approx(1.0f));
    const Vec2 persp[4] = {Vec2{0, 0}, Vec2{32, 4}, Vec2{32, 28}, Vec2{0, 32}};
    CHECK_FALSE(imageQuadLod(persp, uv, 64, 64, lod));
}

TEST_CASE("T-665 mip chain: 2x2 premultiplied box, odd edges and 1xN") {
    ImageData img;
    img.width = 5;
    img.height = 3;
    img.rgba.assign(5 * 3 * 4, 0);
    for (int i = 0; i < 15; ++i) {
        img.rgba[static_cast<size_t>(i) * 4 + 0] = static_cast<uint8_t>(i * 16);
        img.rgba[static_cast<size_t>(i) * 4 + 3] = 255;
    }
    // Texel (1, 0) half transparent: premultiplied before averaging.
    img.rgba[1 * 4 + 0] = 200;
    img.rgba[1 * 4 + 3] = 128;
    ImageMipChain chain;
    REQUIRE(buildImageMipChain(img, chain));
    REQUIRE(chain.levels.size() == 2);
    CHECK(chain.levels[0].width == 2);
    CHECK(chain.levels[0].height == 1);
    CHECK(chain.levels[1].width == 1);
    CHECK(chain.levels[1].height == 1);
    // Level 1 texel (0, 0) = texels (0,0) (1,0) (0,1) (1,1); row 2 and column 4 are not sampled.
    const uint32_t p10 = (200u * 128u + 127u) / 255u;
    const uint32_t sumR = 0 + p10 + 80 + 96;
    CHECK(chain.levels[0].rgba[0] == rne4(sumR));
    CHECK(chain.levels[0].rgba[3] == rne4(255u + 128u + 255u + 255u));
    CHECK(chain.byteSize() == (2 * 1 + 1 * 1) * 4);

    ImageData col;
    col.width = 1;
    col.height = 5;
    col.rgba.assign(1 * 5 * 4, 255);
    for (int y = 0; y < 5; ++y) {
        col.rgba[static_cast<size_t>(y) * 4] = static_cast<uint8_t>(y * 50);
    }
    REQUIRE(buildImageMipChain(col, chain));
    REQUIRE(chain.levels.size() == 2);
    CHECK(chain.levels[0].width == 1);
    CHECK(chain.levels[0].height == 2);
    CHECK(chain.levels[0].rgba[0] == rne4(0 + 0 + 50 + 50));
    CHECK(chain.levels[0].rgba[4] == rne4(100 + 100 + 150 + 150));
    ImageData one = makeChecker(1, 1);
    REQUIRE(buildImageMipChain(one, chain));
    CHECK(chain.levels.empty());
}

TEST_CASE("T-665 SW: lod 0 is untouched, rho 1 and 1.0001 are continuous, lod 1 reads level 1") {
    MipTestResolver resolver;
    const AssetId asset = resolver.add(makeChecker(16, 16));
    // rho = 1 (lod 0) next to rho = 1.0001: at most one step apart.
    const auto a = renderOne(resolver, asset, Rect{0, 0, 16, 16}, Mat2x3{});
    const auto b =
        renderOne(resolver, asset, Rect{0, 0, 16, 16}, Mat2x3{1.0f / 1.0001f, 0, 0, 1.0f / 1.0001f, 0, 0});
    CHECK(maxDiff(a, b) <= 1);
    // 16x16 -> 8x8 at lod 1 exactly: each pixel is the level-1 texel (a 2x2 box of the checker).
    const auto c = renderOne(resolver, asset, Rect{0, 0, 16, 16}, Mat2x3{0.5f, 0, 0, 0.5f, 0, 0});
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const uint8_t* p = c.data() + (static_cast<size_t>(y) * 64 + static_cast<size_t>(x)) * 4;
            CHECK(std::abs(int(p[0]) - 128) <= 1);
            CHECK(std::abs(int(p[1]) - 128) <= 1);
            CHECK(p[3] == 255);
        }
    }
}

TEST_CASE("T-665 SwMipCache: built once, resolver chains, compressed without levels, invalidation") {
    MipTestResolver resolver;
    const AssetId a = resolver.add(makeChecker(32, 32));
    const AssetId b = resolver.add(makeChecker(8, 8));
    const AssetId comp = resolver.add(makeChecker(8, 8));
    resolver.compressedAsset = comp;
    resolver.compressed.levels = 1;
    resolver.ownChainAsset = b;
    IrView view{};
    SwMipCache cache;
    const ImageData* ia = resolver.imageView(a, view);
    const ImageMipChain* first = cache.chainFor(&resolver, view, a, 1.0f, *ia);
    REQUIRE(first != nullptr);
    CHECK(first->levels.size() == 5);
    CHECK(cache.chainFor(&resolver, view, a, 1.0f, *ia) == first);
    CHECK(cache.entries() == 1);
    CHECK(cache.bytes() == first->byteSize());
    // The resolver's own chain is used as it is, nothing built here.
    CHECK(cache.chainFor(&resolver, view, b, 1.0f, *resolver.imageView(b, view)) == &resolver.ownChain);
    CHECK(cache.entries() == 1);
    // A block-compressed GPU form without a stored chain keeps the single level.
    CHECK(cache.chainFor(&resolver, view, comp, 1.0f, *resolver.imageView(comp, view)) == nullptr);
    resolver.compressed.levels = 4;
    CHECK(cache.chainFor(&resolver, view, comp, 1.0f, *resolver.imageView(comp, view)) != nullptr);
    CHECK(cache.entries() == 2);
    cache.invalidateAsset(comp, nullptr);
    CHECK(cache.entries() == 1);
    // The 2x variant is another entry.
    cache.chainFor(&resolver, view, a, 2.0f, *ia);
    CHECK(cache.entries() == 2);
    cache.invalidateAll();
    CHECK(cache.entries() == 0);
    cache.chainFor(&resolver, view, a, 1.0f, *ia);
    cache.endRender();
    CHECK(cache.entries() == 1);
    cache.clear();
    CHECK(cache.entries() == 0);
    CHECK(cache.bytes() == 0);
}

TEST_CASE("T-665 SwMipCache: sources beyond the two most recent go with their chains") {
    MipTestResolver r1, r2, r3;
    const AssetId a1 = r1.add(makeChecker(16, 16));
    const AssetId a2 = r2.add(makeChecker(16, 16));
    const AssetId a3 = r3.add(makeChecker(16, 16));
    IrView view{};
    SwMipCache cache;
    cache.chainFor(&r1, view, a1, 1.0f, *r1.imageView(a1, view));
    cache.endRender();
    cache.chainFor(&r2, view, a2, 1.0f, *r2.imageView(a2, view));
    cache.endRender();
    CHECK(cache.entries() == 2);
    cache.chainFor(&r3, view, a3, 1.0f, *r3.imageView(a3, view));
    cache.endRender();
    CHECK(cache.entries() == 2);  // r1's chain went
}

namespace {

/// The procedural stand-in of Tests/golden/ui/minify.png's high-frequency quadrants: a 1 px checker, a
/// 2 px coloured checker, a 1 px grid every 4 px and 1 px diagonals every 5 px.
ImageData makeHighFrequency() {
    ImageData img;
    img.width = 128;
    img.height = 128;
    img.rgba.resize(128 * 128 * 4);
    for (int y = 0; y < 128; ++y) {
        for (int x = 0; x < 128; ++x) {
            uint8_t* p = img.rgba.data() + (static_cast<size_t>(y) * 128 + static_cast<size_t>(x)) * 4;
            uint8_t r = 255, g = 255, b = 255;
            if (y < 64 && x < 64) {
                r = g = b = ((x + y) % 2) != 0 ? 0 : 255;
            } else if (y < 64) {
                const bool c = ((x / 2 + y / 2) % 2) != 0;
                r = c ? 200 : 20;
                g = c ? 30 : 40;
                b = c ? 40 : 180;
            } else if (x < 64) {
                const bool line = x % 4 == 0 || y % 4 == 0;
                r = line ? 0 : 240;
                g = line ? 0 : 240;
                b = line ? 0 : 230;
            } else if ((x - y + 640) % 5 == 0) {
                r = 10;
                g = 120;
                b = 30;
            }
            p[0] = r;
            p[1] = g;
            p[2] = b;
            p[3] = 255;
        }
    }
    return img;
}

/// The exact area resample of `img` drawn at `scale` (from the origin) into a `w` x `h` canvas: each
/// destination pixel integrates the source (texels as boxes) with 16 x 16 samples. RGB 0..255.
std::vector<double> referenceResample(const ImageData& img, float scale, int w, int h) {
    std::vector<double> out(static_cast<size_t>(w) * static_cast<size_t>(h) * 3, 0.0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double acc[3] = {0, 0, 0};
            for (int j = 0; j < 16; ++j) {
                for (int i = 0; i < 16; ++i) {
                    const double sx = (x + (i + 0.5) / 16.0) / scale;
                    const double sy = (y + (j + 0.5) / 16.0) / scale;
                    const int tx = std::min(img.width - 1, static_cast<int>(sx));
                    const int ty = std::min(img.height - 1, static_cast<int>(sy));
                    const uint8_t* p =
                        img.rgba.data() +
                        (static_cast<size_t>(ty) * static_cast<size_t>(img.width) + static_cast<size_t>(tx)) *
                            4;
                    for (int k = 0; k < 3; ++k) acc[k] += p[k];
                }
            }
            for (int k = 0; k < 3; ++k) {
                out[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 3 +
                    static_cast<size_t>(k)] = acc[k] / 256.0;
            }
        }
    }
    return out;
}

/// Mean absolute RGB error of the `w` x `h` top-left part of a 64-wide render against `ref`.
double meanError(const std::vector<uint8_t>& px, const std::vector<double>& ref, int w, int h) {
    double sum = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            for (int k = 0; k < 3; ++k) {
                const double v =
                    px[(static_cast<size_t>(y) * 64 + static_cast<size_t>(x)) * 4 + static_cast<size_t>(k)];
                sum += std::fabs(
                    v - ref[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 3 +
                            static_cast<size_t>(k)]);
            }
        }
    }
    return sum / (static_cast<double>(w) * h * 3);
}

}  // namespace

TEST_CASE("T-665 reference: trilinear is closer to the exact area resample than single-level bilinear") {
    MipTestResolver resolver;
    const ImageData img = makeHighFrequency();
    const AssetId oldAsset = resolver.add(img);  // reported block-compressed without levels: one level
    const AssetId newAsset = resolver.add(img);
    resolver.compressedAsset = oldAsset;
    resolver.compressed.levels = 1;
    for (float scale : {0.5f, 0.33f, 0.2f}) {
        const int n = static_cast<int>(std::floor(128.0f * scale));  // whole pixels inside the image
        const auto ref = referenceResample(img, scale, n, n);
        const Mat2x3 xf{scale, 0, 0, scale, 0, 0};
        const double eOld = meanError(renderOne(resolver, oldAsset, Rect{0, 0, 128, 128}, xf), ref, n, n);
        const double eNew = meanError(renderOne(resolver, newAsset, Rect{0, 0, 128, 128}, xf), ref, n, n);
        MESSAGE("scale " << scale << ": mean |error| vs 16x16 area reference, single-level " << eOld
                         << ", trilinear " << eNew);
        if (scale == 0.5f) {
            // Exactly 2:1 from the origin, the single bilinear tap already falls between four texels
            // (= level 1's box): both samplings give the same pixels.
            CHECK(eNew <= eOld);
        } else {
            CHECK(eNew < eOld);
        }
    }
}

TEST_CASE("T-665 flicker: 20 frames scaling 0.30 -> 0.49, mean absolute difference of adjacent frames") {
    MipTestResolver resolver;
    const ImageData img = makeHighFrequency();
    const AssetId oldAsset = resolver.add(img);
    const AssetId newAsset = resolver.add(img);
    resolver.compressedAsset = oldAsset;
    resolver.compressed.levels = 1;
    double flicker[2] = {0, 0};
    for (int which = 0; which < 2; ++which) {
        std::vector<uint8_t> prev;
        for (int f = 0; f < 20; ++f) {
            const float s = 0.30f + 0.01f * static_cast<float>(f);
            const auto px = renderOne(resolver, which == 0 ? oldAsset : newAsset, Rect{0, 0, 128, 128},
                                      Mat2x3{s, 0, 0, s, 0, 0});
            if (!prev.empty()) {
                double sum = 0;
                for (int y = 0; y < 38; ++y) {  // inside the smallest frame (0.30 * 128 = 38.4)
                    for (int x = 0; x < 38; ++x) {
                        for (int k = 0; k < 3; ++k) {
                            const size_t i = (static_cast<size_t>(y) * 64 + static_cast<size_t>(x)) * 4 +
                                             static_cast<size_t>(k);
                            sum += std::abs(int(px[i]) - int(prev[i]));
                        }
                    }
                }
                flicker[which] += sum / (38.0 * 38.0 * 3.0);
            }
            prev = px;
        }
        flicker[which] /= 19.0;
    }
    MESSAGE("flicker (mean |frame f - frame f-1|, 19 pairs): single-level " << flicker[0] << ", trilinear "
                                                                            << flicker[1]);
    CHECK(flicker[1] < flicker[0]);
}
