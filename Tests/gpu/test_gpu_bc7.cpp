// T-686 (Docs/design/10-backends.md §10.13 rulings 1, 2, 3): a block-compressed DDS stays blocks on
// the GPU path -- the frame builder uploads them and nothing decodes them to RGBA -- until something
// asks imageView() for pixels; hidden, the blocks and the pixels both go to the resolver's idle list;
// the 'XTLS' logical size sizes the image and maps the texture's uv, and an out-of-range marker is
// ignored. Fixtures: Tests/golden/ui/bc7_64.dds and bc7_66x50.dds (see Tests/bundle/
// test_asset_pipeline.cpp "T-686: the BC7 golden fixtures ...").

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
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
#include "xt/dds.h"
#include "xt/document.h"
#include "xt/gpu/frame_builder.h"
#include "xt/gpu/gpu_device.h"
#include "xt/gpu/gpu_frame.h"
#include "xt/sw/sw_png.h"
#include "xt/text_shaper.h"

using namespace xt;
using namespace xt::gpu;

namespace {

namespace fs = std::filesystem;
using Access = FileAssetResolverTestAccess;

int g_decodes = 0;
bool countingDecode(const uint8_t* bytes, size_t size, ImageData& out) {
    ++g_decodes;
    return sw::decodeImageBytes(bytes, size, out);
}

/// Takes every texture (block-compressed ones included) and gives each a chain of the levels it is
/// handed, as a device with TextureCompressionBC does.
class BcDevice : public GpuDevice {
public:
    // T-691: stands in for xt_gl / xt_wgpu, which draw coverage quads and Shading::BoxShadow.
    bool supportsCoverageDraws() const override { return true; }
    bool supportsBoxShadowShading() const override { return true; }
    struct Made {
        TextureHandle tex = 0;
        uint16_t w = 0;
        uint16_t h = 0;
        TextureFormat fmt = TextureFormat::Rgba8;
    };
    std::vector<Made> made;
    TextureHandle next = 1;
    TextureHandle createTexture(const char*, uint16_t w, uint16_t h, TextureFormat fmt,
                                const void*) override {
        made.push_back(Made{next, w, h, fmt});
        return next++;
    }
    void updateTexture(TextureHandle, uint16_t, uint16_t, uint16_t, uint16_t, const void*, size_t) override {}
    void destroyTexture(TextureHandle) override {}
    void submit(const GpuFrame&) override {}
    uint32_t requestMipChain(TextureHandle, const void* stored, uint32_t storedCount) override {
        return stored != nullptr ? storedCount : 1;
    }
    const Made* find(uint16_t w, uint16_t h) const {
        for (const Made& m : made) {
            if (m.w == w && m.h == h) return &m;
        }
        return nullptr;
    }
};

std::vector<uint8_t> readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeFile(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(out.good());
}

fs::path freshDir(const char* tag) {
    const fs::path dir = xt::test::tempDir() / tag;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

/// The id of the asset `name` names in `view`.
AssetId assetNamed(const IrView& view, std::string_view name) {
    const auto assets = view.assets().assets();
    for (size_t i = 0; i < assets.size(); ++i) {
        if (view.string(assets[i].name) == name) return static_cast<AssetId>(i);
    }
    return kInvalidAsset;
}

std::vector<uint8_t> compileDoc(const fs::path& dir, const std::string& text) {
    const compile::SourceFile file{"doc.xt", text};
    compile::CompileOptions options;
    options.rootDir = dir.string();
    options.embedImages = false;
    options.emitMeta = false;
    compile::CompileResult result = compile::compileDocumentFromStrings({&file, 1}, "doc.xt", options);
    REQUIRE(result.ok());
    return result.ir;
}

}  // namespace

TEST_CASE("gpu: T-686 a BC7 DDS is uploaded as blocks without an RGBA decode, which imageView makes once") {
    const fs::path dir = freshDir("xt_t686_bc7_gpu");
    fs::copy_file(fs::path(XT_GPU_UI_DIR) / "bc7_64.dds", dir / "bc7_64.dds");
    fs::copy_file(fs::path(XT_GPU_UI_DIR) / "bc7_66x50.dds", dir / "bc7_66x50.dds");
    g_decodes = 0;
    FileAssetResolver assets(dir.string(), countingDecode);
    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, &assets});
    // The padded image at its intrinsic size, the other minified (its stored levels are asked for).
    const std::vector<uint8_t> ir = compileDoc(
        dir,
        "<xt><body style=\"padding: 0\">"
        "<img src=\"bc7_66x50.dds\" style=\"position: absolute; left: 0; top: 0\"/>"
        "<img src=\"bc7_64.dds\" style=\"position: absolute; left: 80px; top: 0; width: 19.2px; height: "
        "19.2px\"/></body></xt>");
    Result<DocumentHandle> loaded = ctx.loadDocument(IrBlob{ir.data(), ir.size()}, false);
    REQUIRE(loaded.ok());
    const DocumentHandle handle = loaded.value();
    ctx.setViewport(Rect{0, 0, 128, 64}, 1.0f);
    ctx.update(0.0f);
    const IrView& view = ctx.document(handle)->ir();
    const AssetId padded = assetNamed(view, "bc7_66x50.dds");
    const AssetId square = assetNamed(view, "bc7_64.dds");
    REQUIRE(padded != kInvalidAsset);
    REQUIRE(square != kInvalidAsset);

    // Layout sized the padded image by its logical size, from the header.
    const Vec2 size = assets.imageSize(padded, view);
    CHECK(size.x == 66.0f);
    CHECK(size.y == 50.0f);

    BcDevice device;
    FrameBuilder builder;
    builder.init(device);
    builder.resize(128, 64);
    CommandList list;
    ctx.render(list);
    GpuFrame frame;
    builder.build(list, nullptr, &assets, &view, frame);

    // Ruling 1: both uploaded as BC7 blocks; no RGBA decode, the decode function never called.
    const BcDevice::Made* tex66 = device.find(68, 52);
    const BcDevice::Made* tex64 = device.find(64, 64);
    REQUIRE(tex66 != nullptr);
    REQUIRE(tex64 != nullptr);
    CHECK(tex66->fmt == TextureFormat::Bc7);
    CHECK(tex64->fmt == TextureFormat::Bc7);
    CHECK(device.made.size() >= 2);
    CHECK(Access::ddsRgbaDecodeCount(assets) == 0);
    CHECK(Access::ddsLevelDecodeCount(assets) == 0);
    CHECK(g_decodes == 0);
    CHECK(Access::rgbaBytes(assets) == 0);
    const size_t blocks = Access::blockBytes(assets);
    CHECK(blocks > 0);

    // Ruling 2: the padded image's draw covers 0..66/68 x 0..50/52 of its texture.
    float maxU = 0.0f, maxV = 0.0f;
    bool drawn = false;
    for (const DrawCmd& c : frame.cmds) {
        if (c.texture != tex66->tex) continue;
        drawn = true;
        for (uint32_t i = 0; i < c.vertexCount; ++i) {
            const Vertex& v = frame.vertices[c.baseVertex + i];
            maxU = std::max(maxU, v.u);
            maxV = std::max(maxV, v.v);
        }
    }
    REQUIRE(drawn);
    CHECK(maxU == doctest::Approx(66.0 / 68.0).epsilon(1e-6));
    CHECK(maxV == doctest::Approx(50.0 / 52.0).epsilon(1e-6));

    // Pixels asked for: decoded once (cropped to the logical size), then the cached decode.
    const ImageData* pixels = assets.imageView(padded, view);
    REQUIRE(pixels != nullptr);
    CHECK(pixels->width == 66);
    CHECK(pixels->height == 50);
    CHECK(assets.imageView(padded, view) == pixels);
    CHECK(Access::ddsRgbaDecodeCount(assets) == 1);
    CHECK(Access::rgbaBytes(assets) == size_t{66} * 50 * 4);
    CHECK(Access::blockBytes(assets) == blocks);  // the blocks stay for the GPU

    // Ruling 3: the stored levels for the software renderer, decoded once, straight, logical uv.
    const ImageMipChain* chain = assets.imageMipChain(padded, view, 1.0f);
    REQUIRE(chain != nullptr);
    CHECK(chain->straight);
    CHECK(chain->levels.size() == 6);
    CHECK(chain->levels[0].width == 34);
    CHECK(chain->levels[0].height == 26);
    CHECK(chain->levelUvScaleX == 66.0f / 68.0f);
    CHECK(chain->levelUvScaleY == 50.0f / 52.0f);
    CHECK(assets.imageMipChain(padded, view, 1.0f) == chain);
    CHECK(Access::ddsLevelDecodeCount(assets) == 6);

    // Hidden (trim-on-hide releases the document's images): blocks, pixels and levels all go idle.
    builder.shutdown();
    REQUIRE(ctx.setVisible(handle, false) == Status::kOk);
    ctx.update(0.0f);
    CHECK(Access::idleCount(assets) == 2);
    CHECK(Access::idleBytes(assets) == Access::decodedBytes(assets));
    CHECK(Access::idleBytes(assets) >= blocks + size_t{66} * 50 * 4);
    // Budget 0: everything is freed.
    assets.setIdleDecodeBudget(0);
    CHECK(Access::decodedBytes(assets) == 0);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("gpu: T-686 a logical-size marker out of range is ignored (and reported once)") {
    const std::vector<uint8_t> original = readFile(fs::path(XT_GPU_UI_DIR) / "bc7_66x50.dds");
    REQUIRE(original.size() > 148);
    struct Case {
        uint32_t tag, w, h;
        int expectW, expectH;
        bool rejected;
    };
    const Case cases[] = {
        {kDdsLogicalSizeTag, 66, 50, 66, 50, false},  // the fixture's own marker
        {kDdsLogicalSizeTag, 68, 52, 68, 52, false},  // equal to the stored size
        {0, 66, 50, 68, 52, false},                   // no tag: no marker
        {kDdsLogicalSizeTag, 0, 50, 68, 52, true},    // zero
        {kDdsLogicalSizeTag, 66, 0, 68, 52, true},
        {kDdsLogicalSizeTag, 69, 50, 68, 52, true},  // larger than stored
        {kDdsLogicalSizeTag, 66, 53, 68, 52, true},
        {kDdsLogicalSizeTag, 64, 50, 68, 52, true},  // 4 texels below
        {kDdsLogicalSizeTag, 66, 48, 68, 52, true},
    };
    int index = 0;
    for (const Case& c : cases) {
        CAPTURE(index);
        const fs::path dir = freshDir(("xt_t686_marker_" + std::to_string(index++)).c_str());
        std::vector<uint8_t> bytes = original;
        const uint32_t marker[3] = {c.tag, c.w, c.h};
        for (int i = 0; i < 3; ++i) {
            for (int b = 0; b < 4; ++b) {
                bytes[static_cast<size_t>(4 + 28 + i * 4 + b)] = static_cast<uint8_t>(marker[i] >> (8 * b));
            }
        }
        writeFile(dir / "x.dds", bytes);
        DdsInfo info;
        REQUIRE(parseDdsHeader(bytes.data(), bytes.size(), info));
        CHECK(info.logicalWidth == c.expectW);
        CHECK(info.logicalHeight == c.expectH);
        CHECK(info.logicalRejected == c.rejected);

        FileAssetResolver assets(dir.string(), countingDecode);
        const std::vector<uint8_t> ir = compileDoc(dir, "<xt><body><img src=\"x.dds\"/></body></xt>");
        std::unique_ptr<Document> doc;
        REQUIRE(Document::load(IrBlob{ir.data(), ir.size()}, false, nullptr, &assets, doc) == Status::kOk);
        const AssetId asset = assetNamed(doc->ir(), "x.dds");
        REQUIRE(asset != kInvalidAsset);
        // Header-only size, then the kept blocks' size, then the decoded pixels.
        const Vec2 headerSize = assets.imageSize(asset, doc->ir());
        CHECK(headerSize.x == static_cast<float>(c.expectW));
        CHECK(headerSize.y == static_cast<float>(c.expectH));
        const CompressedImage* compressed = assets.compressedImage(asset, doc->ir(), 1.0f);
        REQUIRE(compressed != nullptr);
        CHECK(compressed->width == 68);
        CHECK(compressed->logicalWidth == c.expectW);
        CHECK(compressed->logicalHeight == c.expectH);
        CHECK(Access::loggedLogicalSize(assets) == c.rejected);
        const Vec2 keptSize = assets.imageSize(asset, doc->ir());
        CHECK(keptSize.x == static_cast<float>(c.expectW));
        const ImageData* pixels = assets.imageView(asset, doc->ir());
        REQUIRE(pixels != nullptr);
        CHECK(pixels->width == c.expectW);
        CHECK(pixels->height == c.expectH);
        doc.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
}
