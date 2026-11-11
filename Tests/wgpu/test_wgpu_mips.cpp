#include <webgpu/webgpu.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "doctest.h"
#include "test_wgpu_draw_util.h"
#include "xt/image_mips.h"

/// T-665 (Docs/design/10-backends.md §10.11 item 7, Docs/tasks/T-665.md): the GPU mip chain against
/// the CPU one, and minified image draws at 1x and 4x against SW.

namespace xt::wgpu::test {
namespace {

ImageData patterned(int w, int h) {
    ImageData img;
    img.width = w;
    img.height = h;
    img.rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* p = img.rgba.data() +
                         (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
            p[0] = static_cast<uint8_t>((x * 37 + y * 11) & 0xFF);
            p[1] = static_cast<uint8_t>(((x + y) % 2) * 255);
            p[2] = static_cast<uint8_t>((x * y * 7) & 0xFF);
            p[3] = static_cast<uint8_t>(x % 3 == 0 ? 255 : 96 + ((x * 13 + y * 5) & 0x7F));
        }
    }
    return img;
}

class ImageResolver : public AssetResolver {
public:
    explicit ImageResolver(ImageData image) : m_image(std::move(image)) {}
    bool readBytes(std::string_view, std::vector<uint8_t>&) override { return false; }
    bool decodeImage(AssetId asset, const IrView&, ImageData& out) override {
        if (asset != 0) return false;
        out = m_image;
        return true;
    }
    Vec2 imageSize(AssetId, const IrView&) override {
        return Vec2{static_cast<float>(m_image.width), static_cast<float>(m_image.height)};
    }
    const ImageData* imageView(AssetId asset, const IrView&) override {
        return asset == 0 ? &m_image : nullptr;
    }

private:
    ImageData m_image;
};

void addImage(CommandList& cl, Rect dst, Mat2x3 xf) {
    ImageCmd c{};
    c.xf = xf;
    c.asset = 0;
    c.dst = dst;
    c.srcUv = Rect{0, 0, 1, 1};
    c.opacity = 1;
    c.smooth = 1;
    cl.images.push_back(c);
    cl.commands.push_back(Command{CmdKind::DrawImage, {}, static_cast<uint32_t>(cl.images.size() - 1)});
}

/// Level `level` of `texture` (RGBA8) read back through a copy into a one-level texture.
std::vector<uint8_t> readLevel(WgpuRenderer& renderer, WGPUTexture texture, uint32_t level, uint32_t w,
                               uint32_t h) {
    WGPUDevice dev = sharedInit().device;
    WGPUTextureDescriptor d = WGPU_TEXTURE_DESCRIPTOR_INIT;
    d.usage = WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst;
    d.size = WGPUExtent3D{w, h, 1};
    d.format = WGPUTextureFormat_RGBA8Unorm;
    WGPUTexture tmp = wgpuDeviceCreateTexture(dev, &d);
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(dev, nullptr);
    WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    src.texture = texture;
    src.mipLevel = level;
    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    dst.texture = tmp;
    const WGPUExtent3D size{w, h, 1};
    wgpuCommandEncoderCopyTextureToTexture(enc, &src, &dst, &size);
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(sharedInit().queue, 1, &cb);
    wgpuCommandBufferRelease(cb);
    wgpuCommandEncoderRelease(enc);
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    CHECK(renderer.readback(tmp, px));
    wgpuTextureDestroy(tmp);
    wgpuTextureRelease(tmp);
    return px;
}

}  // namespace

TEST_CASE("T-665 wgpu: the GPU mip chain equals the CPU chain within one step (odd sizes, 1xN)") {
    Session s(8, 8);
    WgpuDevice& dev = *s.renderer.device();
    for (const auto& wh :
         {std::pair<int, int>{13, 7}, std::pair<int, int>{1, 9}, std::pair<int, int>{64, 48}}) {
        const ImageData img = patterned(wh.first, wh.second);
        std::vector<uint8_t> premul(img.rgba.size());
        premultiplyRgba8(img.rgba.data(), static_cast<size_t>(img.width) * img.height, premul.data());
        const gpu::TextureHandle tex =
            dev.createTexture("t665", static_cast<uint16_t>(img.width), static_cast<uint16_t>(img.height),
                              gpu::TextureFormat::Rgba8, premul.data());
        REQUIRE(tex != 0);
        const uint32_t made = dev.mipChainsMade();
        const uint32_t levels = dev.requestMipChain(tex);
        CHECK(levels == static_cast<uint32_t>(imageMipLevelCount(img.width, img.height)));
        CHECK(dev.mipChainsMade() == made + 1);
        CHECK(dev.requestMipChain(tex) == levels);  // made once
        CHECK(dev.mipChainsMade() == made + 1);
        ImageMipChain chain;
        REQUIRE(buildImageMipChain(img, chain));
        REQUIRE(chain.levels.size() + 1 == levels);
        // Level 0 is the upload, copied as it is.
        CHECK(readLevel(s.renderer, dev.texture(tex), 0, static_cast<uint32_t>(img.width),
                        static_cast<uint32_t>(img.height)) == premul);
        int worst = 0;
        for (uint32_t k = 1; k < levels; ++k) {
            const ImageMipLevel& lv = chain.levels[k - 1];
            const auto gpuLevel = readLevel(s.renderer, dev.texture(tex), k, static_cast<uint32_t>(lv.width),
                                            static_cast<uint32_t>(lv.height));
            for (size_t i = 0; i < gpuLevel.size(); ++i) {
                worst = std::max(worst, std::abs(int(gpuLevel[i]) - int(lv.rgba[i])));
            }
        }
        MESSAGE("mip chain " << img.width << "x" << img.height << ": " << levels << " levels, max diff "
                             << worst);
        CHECK(worst <= 1);
        dev.destroyTexture(tex);
    }
}

TEST_CASE("T-665 wgpu: minified draws match SW at 1x and 4x; lod 0 makes no chain") {
    ImageResolver resolver(patterned(128, 96));
    IrView view{};
    for (uint32_t samples : {1u, 4u}) {
        {
            // 1:1 and magnified: no chain, the single-level draw of before.
            Session s(160, 120, samples);
            CommandList cl = makeList(160, 120);
            addImage(cl, Rect{0, 0, 128, 96}, Mat2x3{});
            addImage(cl, Rect{0, 0, 128, 96}, Mat2x3{1.25f, 0, 0, 1.25f, 0, 0});
            // (No SW comparison here: magnifying this image's per-texel alpha differs between the
            // backends' straight / premultiplied bilinear filters since before T-665.)
            s.render(cl, Color{1, 1, 1, 1}, &resolver, &view);
            CHECK(s.error.empty());
            CHECK(s.renderer.device()->mipChainsMade() == 0);
        }
        Session s(160, 120, samples);
        CommandList cl = makeList(160, 120);
        addImage(cl, Rect{0, 0, 128, 96}, Mat2x3{0.37f, 0, 0, 0.37f, 2, 2});               // lod 1.43
        addImage(cl, Rect{0, 0, 128, 96}, Mat2x3{0.2f, 0, 0, 0.2f, 60, 4});                // lod 2.32
        addImage(cl, Rect{0, 0, 128, 96}, Mat2x3{0.3f, 0, 0, 0.8f, 100, 4});               // non-uniform
        addImage(cl, Rect{0, 0, 128, 96}, Mat2x3{0.29f, 0.078f, -0.078f, 0.29f, 30, 60});  // rotated 15 deg
        const auto gpu = s.render(cl, Color{1, 1, 1, 1}, &resolver, &view);
        CHECK(s.error.empty());
        CHECK(s.renderer.device()->mipChainsMade() == 1);
        const auto sw = renderSw(cl, 160, 120, Color{1, 1, 1, 1}, &resolver, &view);
        CHECK(matches(gpu, sw, 160, 120, samples == 1 ? "minified (1x)" : "minified (4x)"));
        // The next frame draws with the chain made: nothing more is made.
        s.render(cl, Color{1, 1, 1, 1}, &resolver, &view);
        CHECK(s.renderer.device()->mipChainsMade() == 1);
    }
}

}  // namespace xt::wgpu::test
