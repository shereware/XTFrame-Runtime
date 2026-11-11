// T-657 (Docs/design/50-c-abi.md §50.ab item 2): a hidden (trimmed) surface lets go of its decoded
// images, and a builder made for it again finds the shared image textures by the content keys the
// resolver kept -- without decoding the images to hash them. A device that shares image textures by
// content key, as the C ABI's GPU target does (T-648), stands in for the target; each new
// FrameBuilder stands in for the surface's renderer rebuilt after a trim (T-641).

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../unit/file_asset_resolver_access.h"
#include "doctest.h"
#include "test_temp.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/gpu/frame_builder.h"
#include "xt/gpu/gpu_device.h"
#include "xt/gpu/gpu_frame.h"
#include "xt/sw/sw_png.h"
#include "xt/text_shaper.h"

using namespace xt;
using namespace xt::gpu;

namespace {

int g_decodes = 0;
bool countingDecode(const uint8_t* bytes, size_t size, ImageData& out) {
    ++g_decodes;
    return sw::decodeImageBytes(bytes, size, out);
}

/// The file-status function a host injects (Bundle::open sets xt::fileStatusUtf8). ASCII paths.
bool testFileStatus(std::string_view utf8Path, uint64_t& size, uint64_t& modifiedTicks) {
    const std::filesystem::path path{std::string(utf8Path)};
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        return false;
    }
    size = static_cast<uint64_t>(std::filesystem::file_size(path, ec));
    modifiedTicks =
        static_cast<uint64_t>(std::filesystem::last_write_time(path, ec).time_since_epoch().count());
    return !ec;
}

/// Image textures shared by (content key, size, format), each handle one use (T-648); an unused
/// texture stays while `keepUnused` (the target's idle list with room), else it goes at once.
class SharingDevice : public GpuDevice {
public:
    struct Image {
        ImageKey key;
        uint16_t w = 0;
        uint16_t h = 0;
        TextureFormat fmt = TextureFormat::Rgba8;
        int uses = 0;
        bool alive = true;
    };
    std::vector<Image> images;
    std::map<TextureHandle, size_t> uses;  // handle -> index into `images`
    TextureHandle next = 1;
    int uploads = 0;  // createSharedImage calls
    int found = 0;    // findSharedImage hits
    bool keepUnused = true;

    TextureHandle createTexture(const char*, uint16_t, uint16_t, TextureFormat, const void*) override {
        return next++;
    }
    void updateTexture(TextureHandle, uint16_t, uint16_t, uint16_t, uint16_t, const void*, size_t) override {}
    void destroyTexture(TextureHandle) override {}
    void submit(const GpuFrame&) override {}
    bool sharesImageTextures() const override { return true; }
    TextureHandle findSharedImage(const ImageKey& key, uint16_t w, uint16_t h, TextureFormat fmt) override {
        for (size_t i = 0; i < images.size(); ++i) {
            const Image& image = images[i];
            if (image.alive && image.key == key && image.w == w && image.h == h && image.fmt == fmt) {
                ++images[i].uses;
                ++found;
                uses[next] = i;
                return next++;
            }
        }
        return 0;
    }
    TextureHandle createSharedImage(const ImageKey& key, const char*, uint16_t w, uint16_t h,
                                    TextureFormat fmt, const void*) override {
        ++uploads;
        images.push_back(Image{key, w, h, fmt, 1, true});
        uses[next] = images.size() - 1;
        return next++;
    }
    void releaseSharedImage(TextureHandle tex, bool keep) override {
        auto it = uses.find(tex);
        if (it == uses.end()) {
            return;
        }
        Image& image = images[it->second];
        --image.uses;
        uses.erase(it);
        if (image.uses == 0 && !(keep && keepUnused)) {
            image.alive = false;
        }
    }
    /// The target's trim (xt_gpu_target_trim): every unused texture goes.
    void evictUnused() {
        for (Image& image : images) {
            if (image.uses == 0) {
                image.alive = false;
            }
        }
    }
    int liveImages() const {
        int n = 0;
        for (const Image& image : images) {
            n += image.alive ? 1 : 0;
        }
        return n;
    }
};

/// `side` x `side` opaque pixels of one color.
std::vector<uint8_t> solid(int side, uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> px(static_cast<size_t>(side) * static_cast<size_t>(side) * 4u);
    for (size_t i = 0; i < px.size(); i += 4) {
        px[i] = r, px[i + 1] = g, px[i + 2] = b, px[i + 3] = 255;
    }
    return px;
}

/// One Context with one Document drawing a.png (8 x 8) and b.png (4 x 4), both named (files), over
/// a counting resolver with a file-status function (as a bundle's).
struct Scene {
    std::filesystem::path dir;
    FileAssetResolver assets;
    NullTextShaper shaper;
    Context ctx;
    std::vector<uint8_t> ir;
    DocumentHandle handle = 0;

    static std::filesystem::path makeDir(const char* tag) {
        const std::filesystem::path dir = xt::test::tempDir() / tag;
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        return dir;
    }
    explicit Scene(const char* tag)
        : dir(makeDir(tag)), assets(dir.string(), countingDecode), ctx(ContextConfig{&shaper, &assets}) {
        g_decodes = 0;
        assets.setFileStatusFn(testFileStatus);
        writeImage("a.png", solid(8, 255, 0, 0));
        writeImage("b.png", solid(4, 0, 0, 255));
        const std::string text =
            "<xt><body style=\"padding: 0\">"
            "<img src=\"a.png\" style=\"position: absolute; left: 0; top: 0; width: 8px; height: 8px\"/>"
            "<img src=\"b.png\" style=\"position: absolute; left: 16px; top: 0; width: 4px; height: 4px\"/>"
            "</body></xt>";
        const compile::SourceFile file{"doc.xt", text};
        compile::CompileOptions options;
        options.rootDir = dir.string();
        options.embedImages = false;
        options.emitMeta = false;
        compile::CompileResult result = compile::compileDocumentFromStrings({&file, 1}, "doc.xt", options);
        REQUIRE(result.ok());
        ir = result.ir;
        Result<DocumentHandle> loaded = ctx.loadDocument(IrBlob{ir.data(), ir.size()}, false);
        REQUIRE(loaded.ok());
        handle = loaded.value();
        ctx.setViewport(Rect{0, 0, 32, 16}, 1.0f);
    }
    ~Scene() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    /// Writes `name` with `rgba` (square) and moves its modification time on by `secondsLater`
    /// (a rewrite within the file system's time resolution must still change the stamp).
    void writeImage(const char* name, const std::vector<uint8_t>& rgba, int secondsLater = 0) {
        int side = 1;
        while (static_cast<size_t>(side) * static_cast<size_t>(side) * 4u < rgba.size()) ++side;
        REQUIRE(sw::writePng((dir / name).string(), rgba.data(), side, side, side * 4));
        if (secondsLater != 0) {
            std::filesystem::last_write_time(dir / name, std::filesystem::last_write_time(dir / name) +
                                                             std::chrono::seconds(secondsLater));
        }
    }
    /// Hidden for one update: trim-on-hide (T-641) releases, and the surface's renderer goes
    /// (its builder shuts down: the shared textures it used stay with the device).
    void hide(FrameBuilder& builder) {
        REQUIRE(ctx.setVisible(handle, false) == Status::kOk);
        ctx.update(0.0f);
        builder.shutdown();
    }
    /// Shown again: one frame built by a new renderer (`builder`) on `device`.
    GpuFrame show(FrameBuilder& builder, SharingDevice& device) {
        REQUIRE(ctx.setVisible(handle, true) == Status::kOk);
        return frame(builder, device);
    }
    GpuFrame frame(FrameBuilder& builder, SharingDevice& device) {
        builder.init(device);
        builder.resize(32, 16);
        ctx.update(0.0f);
        CommandList list;
        ctx.render(list);
        GpuFrame out;
        builder.build(list, nullptr, &assets, &ctx.document(handle)->ir(), out);
        return out;
    }
};

/// The content keys (with width) of the shared images `frame` samples, in draw order.
std::vector<std::pair<uint64_t, uint16_t>> sampled(const GpuFrame& frame, const SharingDevice& device) {
    std::vector<std::pair<uint64_t, uint16_t>> keys;
    for (const DrawCmd& c : frame.cmds) {
        if (c.shading != Shading::Texture && c.shading != Shading::TextureStraight) continue;
        auto it = device.uses.find(c.texture);
        REQUIRE(it != device.uses.end());
        const SharingDevice::Image& image = device.images[it->second];
        keys.emplace_back(image.key.lo ^ image.key.hi, image.w);
    }
    return keys;
}

}  // namespace

TEST_CASE("gpu: T-657 a trimmed surface's new builder finds the shared textures by the retained keys") {
    using Access = FileAssetResolverTestAccess;
    Scene s("xt_t657_retained_gpu");
    SharingDevice device;
    FrameBuilder first;
    const GpuFrame shown = s.frame(first, device);
    const auto before = sampled(shown, device);
    REQUIRE(before.size() == 2);
    CHECK(device.uploads == 2);
    CHECK(g_decodes == 2);
    CHECK(Access::idleCount(s.assets) == 0);

    // Hidden: both decodes go to the idle list (32 MiB budget), none is live; the textures stay.
    s.hide(first);
    CHECK(Access::idleCount(s.assets) == 2);
    CHECK(Access::idleBytes(s.assets) == Access::decodedBytes(s.assets));
    CHECK(device.liveImages() == 2);

    // Shown again: found by the kept keys -- nothing decoded, nothing uploaded, the same images.
    FrameBuilder second;
    CHECK(sampled(s.show(second, device), device) == before);
    CHECK(g_decodes == 2);
    CHECK(device.uploads == 2);
    CHECK(device.found == 2);

    // Resolver budget 0: the pixels are freed when hidden, the keys stay -- still nothing decoded.
    s.assets.setIdleDecodeBudget(0);
    s.hide(second);
    CHECK(Access::decodedBytes(s.assets) == 0);
    FrameBuilder third;
    CHECK(sampled(s.show(third, device), device) == before);
    CHECK(g_decodes == 2);
    CHECK(device.uploads == 2);
    CHECK(device.found == 4);

    // The textures gone too (the target's trim; xt_gpu_target_set_image_budget(t, 0)): decoded
    // and uploaded again, the same content.
    s.hide(third);
    device.evictUnused();
    CHECK(device.liveImages() == 0);
    FrameBuilder fourth;
    CHECK(sampled(s.show(fourth, device), device) == before);
    CHECK(g_decodes == 4);
    CHECK(device.uploads == 4);
    fourth.shutdown();
}

TEST_CASE("gpu: T-657 an image rewritten while hidden is drawn with its new content, not the old key") {
    Scene s("xt_t657_retained_reload");
    s.assets.setIdleDecodeBudget(0);  // the pixels go when hidden; only the keys are kept
    SharingDevice device;
    FrameBuilder first;
    const auto before = sampled(s.frame(first, device), device);
    REQUIRE(before.size() == 2);
    s.hide(first);

    // a.png rewritten, then the hot-reload check (FileAssetResolver::revalidate, T-263c).
    s.writeImage("a.png", solid(8, 0, 255, 0), 2);
    CHECK(s.assets.revalidate() == 0);  // no decode was cached; the key of a.png goes
    FrameBuilder second;
    const auto afterReload = sampled(s.show(second, device), device);
    REQUIRE(afterReload.size() == 2);
    CHECK(afterReload[0] != before[0]);  // a: the new pixels, decoded and uploaded
    CHECK(afterReload[1] == before[1]);  // b: found by its key
    CHECK(g_decodes == 3);
    CHECK(device.uploads == 3);
    s.hide(second);

    // b.png rewritten with no revalidate(): its key is checked against the file it was computed
    // from when the pixels are gone, and does not match.
    s.writeImage("b.png", solid(4, 255, 255, 0), 2);
    FrameBuilder third;
    const auto afterRewrite = sampled(s.show(third, device), device);
    REQUIRE(afterRewrite.size() == 2);
    CHECK(afterRewrite[0] == afterReload[0]);
    CHECK(afterRewrite[1] != before[1]);
    CHECK(g_decodes == 4);
    CHECK(device.uploads == 4);
    third.shutdown();
}
