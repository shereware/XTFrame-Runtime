// T-806 (Docs/design/23-host-integration.md §23.aj rules 1-5): the plane description of an external
// (`engine://`) binding on the resolver -- kept by a rebind like the content version, dropped with the
// binding, exclusive with CPU pixels, part of the binding's stamp, and checked by externalPlanesValid.

#include <cstdint>

#include "doctest.h"
#include "xt/asset_resolver.h"

using namespace xt;

namespace {

using Planes = AssetResolver::ExternalPlanes;
using Layout = AssetResolver::ExternalLayout;

/// A 64 x 32 picture: Y 64 x 32 on top, U and V 32 x 16 side by side below (a 64 x 48 texture).
Planes packed() {
    Planes p;
    p.layout = Layout::PackedYuv420;
    p.textureWidth = 64;
    p.textureHeight = 48;
    p.y = {0, 0, 64, 32};
    p.u = {0, 32, 32, 16};
    p.v = {32, 32, 32, 16};
    return p;
}

/// A resolver with nothing to read: only the external registry is used.
class NoFiles : public AssetResolver {
public:
    bool readBytes(std::string_view, std::vector<uint8_t>&) override { return false; }
    bool decodeImage(AssetId, const IrView&, ImageData&) override { return false; }
    Vec2 imageSize(AssetId, const IrView&) override { return Vec2{0, 0}; }
};

}  // namespace

TEST_CASE("external planes: the description stays with the name across rebinds and goes with clear") {
    NoFiles assets;
    assets.setExternalTexture("video", 5, 64, 32);
    const uint64_t rgbaStamp = externalTextureStamp(assets.externalTexture("video"));
    assets.setExternalTexturePlanes("video", packed());
    REQUIRE(assets.externalTexture("video") != nullptr);
    CHECK(assets.externalTexture("video")->planes == packed());
    CHECK(assets.externalTexture("video")->width == 64);  // rule 1: the picture size stays the binding's
    const uint64_t planarStamp = externalTextureStamp(assets.externalTexture("video"));
    CHECK(planarStamp != rgbaStamp);  // rule 4: the description is part of the binding

    // A rebind (a new texture each frame of a video) keeps it, and the version alongside.
    assets.setExternalTextureVersion("video", 3);
    assets.setExternalTexture("video", 6, 64, 32);
    CHECK(assets.externalTexture("video")->planes == packed());
    CHECK(assets.externalTexture("video")->version == 3);

    // Every field moves the stamp.
    const uint64_t before = externalTextureStamp(assets.externalTexture("video"));
    Planes other = packed();
    other.siting = AssetResolver::ChromaSiting::Center;
    assets.setExternalTexturePlanes("video", other);
    CHECK(externalTextureStamp(assets.externalTexture("video")) != before);
    other = packed();
    other.v.x = 31;
    assets.setExternalTexturePlanes("video", other);
    CHECK(externalTextureStamp(assets.externalTexture("video")) != before);

    // Back to RGBA: the stamp of a binding that never had a description.
    assets.setExternalTexturePlanes("video", {});
    CHECK(assets.externalTexture("video")->planes.layout == Layout::Rgba);
    NoFiles plain;
    plain.setExternalTexture("video", 6, 64, 32);
    plain.setExternalTextureVersion("video", 3);
    CHECK(externalTextureStamp(assets.externalTexture("video")) ==
          externalTextureStamp(plain.externalTexture("video")));

    assets.setExternalTexturePlanes("video", packed());
    assets.clearExternalTexture("video");
    CHECK(assets.externalTexture("video") == nullptr);
    assets.setExternalTexture("video", 6, 64, 32);
    CHECK(assets.externalTexture("video")->planes.layout == Layout::Rgba);
}

TEST_CASE("external planes: a description before the texture, and CPU pixels exclude it") {
    NoFiles assets;
    assets.setExternalTexturePlanes("video", packed());  // rule 2: remembered for a name not bound yet
    REQUIRE(assets.externalTexture("video") != nullptr);
    CHECK(assets.externalTexture("video")->texture == 0);
    assets.setExternalTexture("video", 9, 64, 32);
    CHECK(assets.externalTexture("video")->planes == packed());

    // Rule 3: CPU pixels end the description; a description ends the CPU pixels.
    ImageData pixels;
    pixels.width = 64;
    pixels.height = 32;
    pixels.rgba.assign(64u * 32u * 4u, 255);
    assets.setExternalPixels("video", &pixels);
    CHECK(assets.externalTexture("video")->planes.layout == Layout::Rgba);
    CHECK(assets.externalTexture("video")->pixels == &pixels);
    assets.setExternalTexturePlanes("video", packed());
    CHECK(assets.externalTexture("video")->pixels == nullptr);
    CHECK(assets.externalTexture("video")->planes == packed());
}

TEST_CASE("external planes: externalPlanesValid") {
    CHECK(externalPlanesValid(packed()));
    CHECK_FALSE(externalPlanesValid(Planes{}));  // Rgba: nothing to draw as planes
    Planes p = packed();
    p.textureWidth = 0;
    CHECK_FALSE(externalPlanesValid(p));
    p = packed();
    p.v.x = 33;  // V reaches past the texture's right edge
    CHECK_FALSE(externalPlanesValid(p));
    p = packed();
    p.u.h = 0;
    CHECK_FALSE(externalPlanesValid(p));
    p = packed();
    p.y.y = -1;
    CHECK_FALSE(externalPlanesValid(p));
    p = packed();
    p.layout = Layout::PackedYuva420;  // an alpha plane is required ...
    CHECK_FALSE(externalPlanesValid(p));
    p.textureHeight = 80;
    p.a = {0, 48, 64, 32};  // ... and must fit too
    CHECK(externalPlanesValid(p));
    p.a.y = 49;
    CHECK_FALSE(externalPlanesValid(p));
    // PackedYuv420 ignores `a`.
    p = packed();
    p.a = {100, 100, 5, 5};
    CHECK(externalPlanesValid(p));
}
