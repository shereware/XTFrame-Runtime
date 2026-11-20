#pragma once

// T-806 (Docs/design/23-host-integration.md §23.aj, Docs/tasks/T-806.md "測試"): what the planar
// external texture pixel cases of Tests/wgpu and Tests/gl share -- the test pictures, the documents that
// draw `engine://scene` through every leaf image path (a plain and a scaled image, object-fit: cover, a
// border-image nine-slice, a tiled background, a 3D-projected image, a rounded clip, pixelated
// sampling), the resolver that sizes them and the difference measure. Each case draws a document once
// with the picture bound as an RGBA8 texture (straight alpha: Shading::TextureStraight) and once as the
// planes Code/Tools/Common/planar_pack.h makes of it (Shading::TexturePlanar), and compares.
//
// Tolerances (the reasons the numbers are what they are):
//  - 8-bit Y'CbCr codes: one code step of Y is 255/219 = 1.16 RGB codes, of Cb / Cr up to 2 (1 - Kb) *
//    255/224 = 2.11 (BT.709 blue); rounding to the nearest code is half of that each, and the two add
//    -- up to about 1.6 per channel, plus the GPU's 8-bit bilinear weights: 3 in all.
//  - 4:2:0: a chroma sample stands for 2 x 2 pixels. On the smooth picture (every channel linear, at
//    most 1.5 codes per pixel) interpolating the half-size chroma back reproduces the colour, except at
//    the picture's last column / row, which the last chroma sample does not reach (Left siting) -- one
//    pixel of slope, within the same 3. On the block picture the colour edges are sharp: the chroma of
//    an edge pixel mixes both sides (unbounded at that pixel, as any 4:2:0 source), so it is bounded by
//    the mean over the frame instead: 1.5 per channel (measured about 1.0: the pixels next to a block
//    edge are some 15 % of the picture, the picture a third of the frame, their error tens of codes).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "../../Code/Tools/Common/planar_pack.h"
#include "doctest.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/types.h"

namespace xt::planar_test {

/// The pictures are 48 x 32; the frames 160 x 120.
inline constexpr int kPicW = 48, kPicH = 32;
inline constexpr int kFrameW = 160, kFrameH = 120;
inline constexpr int kMaxSmooth = 3;        ///< per channel, smooth picture (see the file comment)
inline constexpr double kMeanBlocks = 1.5;  ///< mean per channel over the frame, block picture

/// Straight RGBA8 rows, row 0 = top.
using Picture = std::vector<uint8_t>;

/// Every channel linear in x and y (at most 1.5 codes per pixel), opaque.
inline Picture smoothPicture() {
    Picture p;
    for (int y = 0; y < kPicH; ++y) {
        for (int x = 0; x < kPicW; ++x) {
            p.insert(p.end(), {uint8_t(80 + x), uint8_t(60 + y * 3 / 2), uint8_t(200 - x), uint8_t(255)});
        }
    }
    return p;
}

/// The smooth colours with a smooth alpha (255 at the left, 63 at the right).
inline Picture alphaPicture() {
    Picture p = smoothPicture();
    for (int y = 0; y < kPicH; ++y) {
        for (int x = 0; x < kPicW; ++x) {
            p[(size_t(y) * kPicW + size_t(x)) * 4 + 3] = uint8_t(255 - x * 4);
        }
    }
    return p;
}

/// Four large saturated blocks (16 px grid), opaque: sharp colour edges.
inline Picture blockPicture() {
    static const uint8_t kColors[4][3] = {{230, 40, 30}, {30, 200, 60}, {40, 60, 230}, {240, 220, 40}};
    Picture p;
    for (int y = 0; y < kPicH; ++y) {
        for (int x = 0; x < kPicW; ++x) {
            const uint8_t* c = kColors[((x / 16) + (y / 16)) % 4];
            p.insert(p.end(), {c[0], c[1], c[2], uint8_t(255)});
        }
    }
    return p;
}

/// One document drawing `engine://scene`: its <style> rules (may be empty) and its body. A CSS
/// `url()` naming `engine://` is quoted here and lives in a <style> rule (T-808: unquoted, or in a
/// `style` attribute, it compiles the same).
struct Scene {
    const char* name;
    const char* style;
    const char* body;
};

/// Every leaf image path of §23.aj "繪製" (the rounded image is the frame builder's clipImageDirect).
inline const Scene kScenes[] = {
    {"image", "",
     R"X(<img src="engine://scene" style="position: absolute; left: 8px; top: 8px; width: 48px; height: 32px"/>
<img src="engine://scene" style="position: absolute; left: 64px; top: 12px; width: 80px; height: 53px"/>)X"},
    {"cover", "",
     R"X(<img src="engine://scene" style="position: absolute; left: 10px; top: 10px; width: 64px; height: 64px; object-fit: cover"/>)X"},
    {"nine-slice",
     R"X(#n { position: absolute; left: 8px; top: 8px; width: 100px; height: 70px; border-width: 10px; border-image: url("engine://scene") 10 fill; })X",
     R"X(<div id="n"></div>)X"},
    {"tiled",
     R"X(#t { position: absolute; left: 6px; top: 6px; width: 140px; height: 100px; background-image: url("engine://scene"); background-repeat: repeat; })X",
     R"X(<div id="t"></div>)X"},
    {"projected", "",
     R"X(<div style="position: absolute; left: 0px; top: 0px; width: 160px; height: 120px; perspective: 300px"><img src="engine://scene" style="position: absolute; left: 40px; top: 30px; width: 80px; height: 54px; transform: rotateY(30deg)"/></div>)X"},
    {"rounded", "",
     R"X(<img src="engine://scene" style="position: absolute; left: 12px; top: 12px; width: 72px; height: 48px; border-radius: 12px"/>)X"},
    {"pixelated", "",
     R"X(<img src="engine://scene" style="position: absolute; left: 8px; top: 8px; width: 96px; height: 64px; image-rendering: pixelated"/>)X"},
};

/// A resolver of external bindings only, which also sizes a `border-image` / `background-image` that
/// names one (as FileAssetResolver::imageSize does since T-808).
class SceneResolver : public AssetResolver {
public:
    bool readBytes(std::string_view, std::vector<uint8_t>&) override { return false; }
    bool decodeImage(AssetId, const IrView&, ImageData&) override { return false; }
    Vec2 imageSize(AssetId asset, const IrView& view) override {
        const ExternalTexture* ext = externalTexture(asset, view);
        return ext != nullptr ? Vec2{float(ext->width), float(ext->height)} : Vec2{0, 0};
    }
};

/// A document of `body` loaded against `assets` (bind `scene` first: layout reads its size) and
/// painted at kFrameW x kFrameH.
struct Painted {
    std::vector<uint8_t> ir;
    std::unique_ptr<Document> doc;
    CommandList list;
};

inline bool paint(std::string_view body, AssetResolver& assets, Painted& out, std::string_view style = {}) {
    const std::string text = "<xt><head><style>body { margin: 0; padding: 0; } " + std::string(style) +
                             "</style></head><body>" + std::string(body) + "</body></xt>";
    compile::CompileOptions options;
    options.emitMeta = false;
    std::vector<compile::SourceFile> files{{"planar.xt", text}};
    compile::CompileResult result = compile::compileDocumentFromStrings(files, "planar.xt", options);
    if (!result.ok()) {
        return false;
    }
    out.ir = std::move(result.ir);
    if (Document::load(IrBlob{out.ir.data(), out.ir.size()}, false, nullptr, &assets, out.doc) !=
        Status::kOk) {
        return false;
    }
    out.doc->setViewport(Rect{0, 0, float(kFrameW), float(kFrameH)});
    out.doc->update(0);
    out.doc->paint(out.list);
    out.list.viewport = Rect{0, 0, float(kFrameW), float(kFrameH)};
    out.list.devicePixelRatio = 1;
    return true;
}

/// The planes of `picture` with the given description fields.
inline tools::PackedPlanes planesOf(
    const Picture& picture, AssetResolver::ExternalLayout layout,
    AssetResolver::ExternalMatrix matrix = AssetResolver::ExternalMatrix::Bt709,
    AssetResolver::ExternalRange range = AssetResolver::ExternalRange::Limited,
    AssetResolver::ChromaSiting siting = AssetResolver::ChromaSiting::Left, bool premultiplied = false) {
    return tools::packPlanes(picture.data(), kPicW, kPicH, layout, matrix, range, siting, premultiplied);
}

struct Diff {
    int maxChannel = 0;
    double mean = 0;  ///< over every channel of every pixel
};

inline Diff diff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    Diff d;
    double sum = 0;
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const int v = std::abs(int(a[i]) - int(b[i]));
        d.maxChannel = std::max(d.maxChannel, v);
        sum += v;
    }
    d.mean = n > 0 ? sum / double(n) : 0.0;
    return d;
}

/// Prints one line for the report: `<backend> <case>: max <m>, mean <x>`.
inline Diff report(const char* backend, const std::string& what, const std::vector<uint8_t>& planar,
                   const std::vector<uint8_t>& rgba) {
    const Diff d = diff(planar, rgba);
    std::printf("T-806 %s %s: max channel diff %d, mean %.4f\n", backend, what.c_str(), d.maxChannel, d.mean);
    return d;
}

/// The plane-bleed texture (§23.aj: "平面邊界不滲色"): a 48 x 32 picture whose Y plane is all 128, U
/// all 96 and V all 176, packed with no gap (U | V side by side under Y) -- far apart values whose
/// colour (BT.709 full range: about 204, 112, 69) clamps no channel, so any read across a plane border
/// changes a pixel's colour.
inline tools::PackedPlanes bleedPlanes() {
    tools::PackedPlanes out;
    AssetResolver::ExternalPlanes& p = out.planes;
    p.layout = AssetResolver::ExternalLayout::PackedYuv420;
    p.matrix = AssetResolver::ExternalMatrix::Bt709;
    p.range = AssetResolver::ExternalRange::Full;
    p.textureWidth = kPicW;
    p.textureHeight = kPicH + kPicH / 2;
    p.y = {0, 0, kPicW, kPicH};
    p.u = {0, kPicH, kPicW / 2, kPicH / 2};
    p.v = {kPicW / 2, kPicH, kPicW / 2, kPicH / 2};
    out.texels.assign(size_t(p.textureWidth) * size_t(p.textureHeight), 0);
    for (int y = 0; y < p.textureHeight; ++y) {
        for (int x = 0; x < p.textureWidth; ++x) {
            uint8_t v = 128;  // Y
            if (y >= kPicH) {
                v = x < kPicW / 2 ? 96 : 176;  // U | V
            }
            out.texels[size_t(y) * size_t(p.textureWidth) + size_t(x)] = v;
        }
    }
    return out;
}

/// What a backend's test file provides: host textures it creates the way a host would, and frames.
class Backend {
public:
    virtual ~Backend() = default;
    virtual const char* name() const = 0;
    /// A `w` x `h` host texture of `data` (R8 when `r8`, else RGBA8 as given), its handle for
    /// AssetResolver::setExternalTexture; 0 on failure.
    virtual uint32_t texture(int w, int h, bool r8, const uint8_t* data) = 0;
    /// `list` drawn over an opaque dark clear, read back top-down (kFrameW x kFrameH RGBA8).
    virtual std::vector<uint8_t> render(const CommandList& list, AssetResolver& assets, const IrView& ir) = 0;
    /// The renderer's last error (empty when none).
    virtual std::string error() = 0;
    /// T-812: true on a CPU rasterizer (Mesa llvmpipe / softpipe, SwiftShader; lavapipe, WARP), told by
    /// the backend's existing T-733 / T-735 check.
    virtual bool softwareAdapter() { return false; }
};

/// `body` drawn twice on `backend`: with `picture` as an RGBA8 texture (straight alpha) and as `planes`.
/// Returns the difference after printing it.
inline Diff compareOnce(Backend& backend, const Scene& scene, const Picture& picture,
                        const tools::PackedPlanes& planes, const std::string& what) {
    SceneResolver assets;
    const uint32_t rgbaTex = backend.texture(kPicW, kPicH, false, picture.data());
    const uint32_t planarTex =
        backend.texture(planes.planes.textureWidth, planes.planes.textureHeight, true, planes.texels.data());
    REQUIRE(rgbaTex != 0);
    REQUIRE(planarTex != 0);
    assets.setExternalTexture("scene", rgbaTex, kPicW, kPicH, /*straightAlpha=*/true);
    Painted p;
    REQUIRE(paint(scene.body, assets, p, scene.style));
    REQUIRE(!p.list.commands.empty());
    const std::vector<uint8_t> rgba = backend.render(p.list, assets, p.doc->ir());
    CHECK_MESSAGE(backend.error().empty(), backend.error());
    assets.setExternalTexture("scene", planarTex, kPicW, kPicH);
    assets.setExternalTexturePlanes("scene", planes.planes);
    const std::vector<uint8_t> planar = backend.render(p.list, assets, p.doc->ir());
    CHECK_MESSAGE(backend.error().empty(), backend.error());
    CHECK(planar != std::vector<uint8_t>(planar.size(), 0));
    return report(backend.name(), what, planar, rgba);
}

/// Every pixel case of Docs/tasks/T-806.md "測試" on `backend`.
inline void runPixelCases(Backend& backend) {
    using L = AssetResolver::ExternalLayout;
    using M = AssetResolver::ExternalMatrix;
    using R = AssetResolver::ExternalRange;
    using S = AssetResolver::ChromaSiting;
    const Picture smooth = smoothPicture();
    // Every leaf image path, smooth picture, the default description (BT.709, limited, left).
    for (const Scene& scene : kScenes) {
        CAPTURE(scene.name);
        CHECK(compareOnce(backend, scene, smooth, planesOf(smooth, L::PackedYuv420), scene.name).maxChannel <=
              kMaxSmooth);
    }
    const Scene& image = kScenes[0];
    // T-812 (2026-11-20): on a CPU rasterizer the colour matrices, the sitings and the premultiplied alpha
    // plane below reach 4 (its bilinear sampling rounds differently from a GPU's 8-bit weights); a
    // hardware adapter keeps kMaxSmooth.
    const int maxSmoothMatrix = kMaxSmooth + (backend.softwareAdapter() ? 1 : 0);
    // Sharp colour edges: bounded on average (see the file comment), the maximum printed.
    const Picture blocks = blockPicture();
    CHECK(compareOnce(backend, image, blocks, planesOf(blocks, L::PackedYuv420), "blocks").mean <=
          kMeanBlocks);
    // The four matrix x range and the three sitings.
    const M matrices[] = {M::Bt601, M::Bt709};
    const R ranges[] = {R::Limited, R::Full};
    for (M m : matrices) {
        for (R r : ranges) {
            const std::string what =
                std::string(m == M::Bt601 ? "601" : "709") + (r == R::Full ? " full" : " limited");
            CAPTURE(what);
            CHECK(compareOnce(backend, image, smooth, planesOf(smooth, L::PackedYuv420, m, r), what)
                      .maxChannel <= maxSmoothMatrix);
        }
    }
    const S sitings[] = {S::Left, S::Center, S::TopLeft};
    const char* sitingNames[] = {"siting left", "siting center", "siting topleft"};
    for (int i = 0; i < 3; ++i) {
        CAPTURE(sitingNames[i]);
        CHECK(compareOnce(backend, image, smooth,
                          planesOf(smooth, L::PackedYuv420, M::Bt709, R::Limited, sitings[i]), sitingNames[i])
                  .maxChannel <= maxSmoothMatrix);
    }
    // An alpha plane, with straight and with premultiplied colour planes (and under a rounded clip).
    const Picture alpha = alphaPicture();
    for (bool premul : {false, true}) {
        for (const Scene* scene : {&kScenes[0], &kScenes[5]}) {  // image, rounded
            const std::string what =
                std::string("alpha ") + (premul ? "premultiplied " : "straight ") + scene->name;
            CAPTURE(what);
            CHECK(compareOnce(backend, *scene, alpha,
                              planesOf(alpha, L::PackedYuva420, M::Bt709, R::Limited, S::Left, premul), what)
                      .maxChannel <= maxSmoothMatrix);
        }
    }
}

/// The plane-bleed case: the uniform planes of bleedPlanes() drawn by the "image" (its 1:1 copy's last
/// column and row sit on the texture's last column and on the U / V planes' first row), "nine-slice"
/// (every piece's edges are sub-rectangles of the planes), "tiled" and "pixelated" scenes, next to the
/// same scenes drawn from an RGBA8 picture of the one colour the planes decode to -- every pixel, edge
/// coverage included, must agree within 1: any read across a plane border would move a pixel by tens.
inline void runBleedCase(Backend& backend) {
    const tools::PackedPlanes planes = bleedPlanes();
    const uint32_t planarTex =
        backend.texture(planes.planes.textureWidth, planes.planes.textureHeight, true, planes.texels.data());
    // Y 128, U 96, V 176, full range, BT.709: R' = 0.5020 + 1.5748 (48 / 255) = 0.7984, B' = 0.5020 -
    // 1.8556 (32 / 255) = 0.2691, G' = (0.5020 - 0.2126 R' - 0.0722 B') / 0.7152 = 0.4374.
    const uint8_t colour[4] = {204, 112, 69, 255};
    Picture uniform;
    for (int i = 0; i < kPicW * kPicH; ++i) {
        uniform.insert(uniform.end(), colour, colour + 4);
    }
    const uint32_t rgbaTex = backend.texture(kPicW, kPicH, false, uniform.data());
    REQUIRE(planarTex != 0);
    REQUIRE(rgbaTex != 0);
    for (const char* name : {"image", "nine-slice", "tiled", "pixelated"}) {
        CAPTURE(name);
        const Scene* scene = nullptr;
        for (const Scene& s : kScenes) {
            if (std::string(s.name) == name) scene = &s;
        }
        REQUIRE(scene != nullptr);
        SceneResolver assets;
        assets.setExternalTexture("scene", rgbaTex, kPicW, kPicH, /*straightAlpha=*/true);
        Painted p;
        REQUIRE(paint(scene->body, assets, p, scene->style));
        REQUIRE(!p.list.commands.empty());
        const std::vector<uint8_t> reference = backend.render(p.list, assets, p.doc->ir());
        assets.setExternalTexture("scene", planarTex, kPicW, kPicH);
        assets.setExternalTexturePlanes("scene", planes.planes);
        const std::vector<uint8_t> px = backend.render(p.list, assets, p.doc->ir());
        CHECK_MESSAGE(backend.error().empty(), backend.error());
        int worst = 0;
        size_t at = 0;
        for (size_t i = 0; i < px.size() && i < reference.size(); ++i) {
            const int d = std::abs(int(px[i]) - int(reference[i]));
            if (d > worst) {
                worst = d;
                at = i / 4;
            }
        }
        std::printf("T-806 %s bleed %s: max channel diff %d (at %d, %d)\n", backend.name(), name, worst,
                    int(at % kFrameW), int(at / kFrameW));
        CHECK(worst <= 1);
        CHECK(reference != std::vector<uint8_t>(reference.size(), reference[0]));  // something was drawn
    }
}

}  // namespace xt::planar_test
