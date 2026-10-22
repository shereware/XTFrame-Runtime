#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "doctest.h"
#include "file_open_utf8.h"
#include "test_temp.h"
#include "xt/asset_resolver.h"
#include "xt/compile/ir_writer.h"
#include "xt/compile/utf8_path.h"
#include "xt/ir.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

/// Tests for `FileAssetResolver`'s multi-root form (Docs/design/22-bundle.md §22.2,
/// Docs/design/13-api.md, Docs/tasks/T-138.md) and `AssetResolver::imageView` (Docs/tasks/T-148.md).

using namespace xt;

namespace {

namespace fs = std::filesystem;

void writeText(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

std::string asText(const std::vector<uint8_t>& bytes) { return std::string(bytes.begin(), bytes.end()); }

/// Fake decoder: a 1-pixel-high image whose width is the byte count and whose pixels repeat the
/// bytes as grey levels. Fails on empty input.
bool fakeDecode(const uint8_t* bytes, size_t size, xt::ImageData& out) {
    if (size == 0) {
        return false;
    }
    out.width = static_cast<int>(size);
    out.height = 1;
    out.rgba.clear();
    for (size_t i = 0; i < size; ++i) {
        out.rgba.insert(out.rgba.end(), {bytes[i], bytes[i], bytes[i], 255});
    }
    return true;
}

/// A resolver that implements only the pure virtuals, so imageView() uses the base default.
class DecodeOnlyResolver : public xt::AssetResolver {
public:
    bool readBytes(std::string_view, std::vector<uint8_t>&) override { return false; }
    bool decodeImage(xt::AssetId asset, const xt::IrView&, xt::ImageData& out) override {
        if (asset != 0) {
            return false;
        }
        out.width = 2;
        out.height = 1;
        out.rgba = {1, 2, 3, 4, 5, 6, 7, 8};
        return true;
    }
    xt::Vec2 imageSize(xt::AssetId, const xt::IrView&) override { return xt::Vec2{2, 1}; }
};

/// An IR blob with one embedded image asset (bytes {10, 20, 30}).
std::vector<uint8_t> irWithEmbeddedAsset(xt::AssetId& id) {
    xt::compile::IrWriter writer;
    xt::compile::AssetBuilder asset;
    asset.name = writer.intern("img/a.png");
    asset.kind = 0;
    asset.embedded = {10, 20, 30};
    id = writer.addAsset(asset);
    return writer.finish();
}

}  // namespace

TEST_CASE("FileAssetResolver: multiple roots are tried in order, nearer first") {
    fs::path base = xt::test::tempDir() / "xt_t138_resolver";
    std::error_code ec;
    fs::remove_all(base, ec);
    writeText(base / "ui" / "both.txt", "near");
    writeText(base / "shared" / "both.txt", "far");
    writeText(base / "shared" / "img" / "only-shared.txt", "shared");
    writeText(base / "root-only.txt", "root");

    FileAssetResolver resolver(
        std::vector<std::string>{(base / "ui").string(), (base / "shared").string(), base.string()}, nullptr);
    std::vector<uint8_t> out;
    REQUIRE(resolver.readBytes("both.txt", out));
    CHECK(asText(out) == "near");
    REQUIRE(resolver.readBytes("img/only-shared.txt", out));
    CHECK(asText(out) == "shared");
    REQUIRE(resolver.readBytes("root-only.txt", out));
    CHECK(asText(out) == "root");
    // A name relative to the first root that climbs out of it (the importer's `../shared/img/x`).
    REQUIRE(resolver.readBytes("../shared/img/only-shared.txt", out));
    CHECK(asText(out) == "shared");

    std::vector<uint8_t> untouched{1, 2, 3};
    CHECK(!resolver.readBytes("missing.txt", untouched));
    CHECK(untouched == std::vector<uint8_t>{1, 2, 3});
}

TEST_CASE("FileAssetResolver: single-root constructor keeps its behavior") {
    fs::path base = xt::test::tempDir() / "xt_t138_resolver_single";
    std::error_code ec;
    fs::remove_all(base, ec);
    writeText(base / "a.txt", "a");
    FileAssetResolver resolver(base.string(), nullptr);
    std::vector<uint8_t> out;
    REQUIRE(resolver.readBytes("a.txt", out));
    CHECK(asText(out) == "a");
    CHECK(!resolver.readBytes("b.txt", out));
}

TEST_CASE("FileAssetResolver::imageView: same pointer and contents on repeat, nullptr when unknown") {
    AssetId id = 0;
    std::vector<uint8_t> irBytes = irWithEmbeddedAsset(id);
    Result<IrView> viewResult = IrView::open(IrBlob{irBytes.data(), irBytes.size()});
    REQUIRE(viewResult.ok());
    IrView view = viewResult.value();

    FileAssetResolver resolver(std::string("."), fakeDecode);
    const ImageData* first = resolver.imageView(id, view);
    REQUIRE(first != nullptr);
    CHECK(first->width == 3);
    CHECK(first->height == 1);
    std::vector<uint8_t> firstPixels = first->rgba;
    const ImageData* second = resolver.imageView(id, view);
    CHECK(second == first);
    CHECK(second->rgba == firstPixels);
    CHECK(resolver.imageSize(id, view).x == 3.0f);

    // decodeImage still copies the same cached result.
    ImageData copy;
    REQUIRE(resolver.decodeImage(id, view, copy));
    CHECK(copy.rgba == firstPixels);

    // An asset id the IR does not have fails, and leaves earlier views intact.
    CHECK(resolver.imageView(static_cast<AssetId>(id + 5), view) == nullptr);
    CHECK(resolver.imageView(id, view) == first);
    CHECK(first->rgba == firstPixels);
}

TEST_CASE("AssetResolver::imageView default: works for a resolver that only implements decodeImage") {
    AssetId id = 0;
    std::vector<uint8_t> irBytes = irWithEmbeddedAsset(id);
    Result<IrView> viewResult = IrView::open(IrBlob{irBytes.data(), irBytes.size()});
    REQUIRE(viewResult.ok());
    IrView view = viewResult.value();

    DecodeOnlyResolver resolver;
    const ImageData* image = resolver.imageView(0, view);
    REQUIRE(image != nullptr);
    CHECK(image->width == 2);
    CHECK(image->rgba == std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7, 8});
    CHECK(resolver.imageView(1, view) == nullptr);
}

namespace {

int g_decodeCalls = 0;

/// Counting decoder: always "decodes" a 1x1 image, so a caller that reads the size from the
/// decode result (instead of the header) gets 1x1.
bool countingDecode(const uint8_t*, size_t, xt::ImageData& out) {
    ++g_decodeCalls;
    out.width = 1;
    out.height = 1;
    out.rgba = {0, 0, 0, 255};
    return true;
}

/// An IR blob with one embedded asset holding `bytes`.
std::vector<uint8_t> irWithEmbeddedBytes(const std::vector<uint8_t>& bytes, xt::AssetId& id) {
    xt::compile::IrWriter writer;
    xt::compile::AssetBuilder asset;
    asset.name = writer.intern("img/x");
    asset.kind = 0;
    asset.embedded = bytes;
    id = writer.addAsset(asset);
    return writer.finish();
}

Vec2 sizeWithoutDecode(const std::vector<uint8_t>& bytes, int& decodeCalls) {
    AssetId id = 0;
    std::vector<uint8_t> irBytes = irWithEmbeddedBytes(bytes, id);
    Result<IrView> viewResult = IrView::open(IrBlob{irBytes.data(), irBytes.size()});
    REQUIRE(viewResult.ok());
    FileAssetResolver resolver(std::string("."), countingDecode);
    g_decodeCalls = 0;
    Vec2 size = resolver.imageSize(id, viewResult.value());
    decodeCalls = g_decodeCalls;
    return size;
}

}  // namespace

// T-160 (bench BUG-8): layout asks every <img> for its intrinsic size during the first
// Context::update; decoding a 1280x720 PNG just to learn its size cost the bench ~28 ms there. The
// size comes from the file header now (PNG/JPEG/GIF/BMP); the decode waits for the renderer.
TEST_CASE("FileAssetResolver::imageSize: reads PNG/JPEG/GIF/BMP headers without decoding") {
    int calls = -1;
    // PNG: signature, IHDR length, "IHDR", width 640, height 360 (big-endian); no pixel data.
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G',  0x0D, 0x0A, 0x1A, 0x0A, 0,    0, 0, 13, 'I', 'H', 'D',
                                'R',  0,   0,   0x02, 0x80, 0,    0,    0x01, 0x68, 8, 6, 0,  0,   0};
    Vec2 s = sizeWithoutDecode(png, calls);
    CHECK(s.x == 640.0f);
    CHECK(s.y == 360.0f);
    CHECK(calls == 0);

    // JPEG: SOI, an APP0 segment to skip, then SOF0 (precision 8, height 200, width 300).
    std::vector<uint8_t> jpg = {0xFF, 0xD8, 0xFF, 0xE0, 0,    4,    0xAA, 0xBB, 0xFF, 0xC0, 0,
                                11,   8,    0,    200,  0x01, 0x2C, 3,    1,    0x22, 0};
    s = sizeWithoutDecode(jpg, calls);
    CHECK(s.x == 300.0f);
    CHECK(s.y == 200.0f);
    CHECK(calls == 0);

    // GIF: logical screen 17x9 (little-endian).
    std::vector<uint8_t> gif = {'G', 'I', 'F', '8', '9', 'a', 17, 0, 9, 0, 0, 0, 0};
    s = sizeWithoutDecode(gif, calls);
    CHECK(s.x == 17.0f);
    CHECK(s.y == 9.0f);
    CHECK(calls == 0);

    // BMP: BITMAPINFOHEADER, width 5, height -7 (top-down).
    std::vector<uint8_t> bmp(26, 0);
    bmp[0] = 'B';
    bmp[1] = 'M';
    bmp[14] = 40;
    bmp[18] = 5;
    bmp[22] = 0xF9;
    bmp[23] = 0xFF;
    bmp[24] = 0xFF;
    bmp[25] = 0xFF;
    s = sizeWithoutDecode(bmp, calls);
    CHECK(s.x == 5.0f);
    CHECK(s.y == 7.0f);
    CHECK(calls == 0);

    // Anything else still falls back to a decode (the counting decoder says 1x1).
    s = sizeWithoutDecode(std::vector<uint8_t>{1, 2, 3}, calls);
    CHECK(s.x == 1.0f);
    CHECK(calls == 1);
}

TEST_CASE("FileAssetResolver::imageSize: header size is cached; a later decode takes over") {
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G',  0x0D, 0x0A, 0x1A, 0x0A, 0,    0, 0, 13, 'I', 'H', 'D',
                                'R',  0,   0,   0x02, 0x80, 0,    0,    0x01, 0x68, 8, 6, 0,  0,   0};
    AssetId id = 0;
    std::vector<uint8_t> irBytes = irWithEmbeddedBytes(png, id);
    Result<IrView> viewResult = IrView::open(IrBlob{irBytes.data(), irBytes.size()});
    REQUIRE(viewResult.ok());
    FileAssetResolver resolver(std::string("."), countingDecode);
    g_decodeCalls = 0;
    CHECK(resolver.imageSize(id, viewResult.value()).x == 640.0f);
    CHECK(resolver.imageSize(id, viewResult.value()).y == 360.0f);
    CHECK(g_decodeCalls == 0);
    REQUIRE(resolver.imageView(id, viewResult.value()) != nullptr);
    CHECK(g_decodeCalls == 1);
    // Once decoded, the decoded image is the authority (the counting decoder says 1x1).
    CHECK(resolver.imageSize(id, viewResult.value()).x == 1.0f);
    CHECK(g_decodeCalls == 1);
}

namespace {

/// T-215: an IR whose external image assets are `names`, in that order (ids 0, 1, ...).
std::vector<uint8_t> irWithExternalAssets(const std::vector<std::string>& names) {
    xt::compile::IrWriter writer;
    for (const std::string& name : names) {
        xt::compile::AssetBuilder asset;
        asset.name = writer.intern(name);
        asset.kind = 0;
        writer.addAsset(asset);
    }
    return writer.finish();
}

}  // namespace

TEST_CASE("FileAssetResolver::resetAssetIds: ids of a new IR resolve afresh, decodes are reused by path") {
    fs::path base = xt::test::tempDir() / "xt_t215_reset_ids";
    std::error_code ec;
    fs::remove_all(base, ec);
    writeText(base / "a.png", "aaa");
    writeText(base / "b.png", "bbbbb");
    std::vector<uint8_t> firstBytes = irWithExternalAssets({"a.png"});
    std::vector<uint8_t> secondBytes = irWithExternalAssets({"b.png", "a.png"});  // An image inserted first.
    Result<IrView> first = IrView::open(IrBlob{firstBytes.data(), firstBytes.size()});
    Result<IrView> second = IrView::open(IrBlob{secondBytes.data(), secondBytes.size()});
    REQUIRE(first.ok());
    REQUIRE(second.ok());

    FileAssetResolver resolver(base.string(), fakeDecode);
    const ImageData* a = resolver.imageView(0, first.value());
    REQUIRE(a != nullptr);
    CHECK(a->width == 3);
    CHECK(resolver.imageSize(0, first.value()).x == 3.0f);
    // T-263b A6: the cache is keyed by the asset's name, so id 0 of the second IR is its own
    // image even without a reset (it used to be the first IR's).
    REQUIRE(resolver.imageView(0, second.value()) != nullptr);
    CHECK(resolver.imageView(0, second.value())->width == 5);

    resolver.resetAssetIds();
    const ImageData* b = resolver.imageView(0, second.value());
    REQUIRE(b != nullptr);
    CHECK(b->width == 5);
    CHECK(resolver.imageSize(0, second.value()).x == 5.0f);
    CHECK(resolver.imageView(1, second.value()) == a);  // Same file: the decode is reused.
    CHECK(a->width == 3);                               // Earlier pointers stay valid.

    // An embedded image is found again by its bytes rather than decoded into a new entry.
    AssetId embeddedId = 0;
    std::vector<uint8_t> embeddedBytes = irWithEmbeddedAsset(embeddedId);
    Result<IrView> embedded = IrView::open(IrBlob{embeddedBytes.data(), embeddedBytes.size()});
    REQUIRE(embedded.ok());
    resolver.resetAssetIds();
    const ImageData* e1 = resolver.imageView(embeddedId, embedded.value());
    REQUIRE(e1 != nullptr);
    resolver.resetAssetIds();
    CHECK(resolver.imageView(embeddedId, embedded.value()) == e1);
    fs::remove_all(base, ec);
}

// T-263b A6: one resolver serves every Document of a Context or bundle, and each IR numbers its
// assets from 0, so nothing may be cached under the bare AssetId.
TEST_CASE("FileAssetResolver: two IRs with the same AssetIds for other images share one resolver") {
    fs::path base = xt::test::tempDir() / "xt_t263b_two_irs";
    std::error_code ec;
    fs::remove_all(base, ec);
    writeText(base / "hp.png", "aaa");
    writeText(base / "hp@2x.png", "aaaaaa");
    writeText(base / "logo.png", "bbbbb");
    std::vector<uint8_t> hudBytes = irWithExternalAssets({"hp.png"});
    std::vector<uint8_t> menuBytes = irWithExternalAssets({"logo.png"});
    Result<IrView> hud = IrView::open(IrBlob{hudBytes.data(), hudBytes.size()});
    Result<IrView> menu = IrView::open(IrBlob{menuBytes.data(), menuBytes.size()});
    REQUIRE(hud.ok());
    REQUIRE(menu.ok());

    SUBCASE("decoded images and their sizes") {
        FileAssetResolver resolver(base.string(), fakeDecode);
        const ImageData* hp = resolver.imageView(0, hud.value());
        REQUIRE(hp != nullptr);
        CHECK(hp->width == 3);
        const ImageData* logo = resolver.imageView(0, menu.value());
        REQUIRE(logo != nullptr);
        CHECK(logo->width == 5);
        CHECK(resolver.imageSize(0, hud.value()).x == 3.0f);
        CHECK(resolver.imageSize(0, menu.value()).x == 5.0f);
        // Both stay cached: alternating between the documents decodes nothing again.
        CHECK(resolver.imageView(0, hud.value()) == hp);
        CHECK(resolver.imageView(0, menu.value()) == logo);
        ImageData copy;
        REQUIRE(resolver.decodeImage(0, menu.value(), copy));
        CHECK(copy.width == 5);
    }
    SUBCASE("the 2x variant") {
        FileAssetResolver resolver(base.string(), fakeDecode);
        const ImageData* hp2x = resolver.imageView(0, hud.value(), 2.0f);
        REQUIRE(hp2x != nullptr);
        CHECK(hp2x->width == 6);
        const ImageData* logo = resolver.imageView(0, menu.value(), 2.0f);  // no logo@2x.png: the 1x file
        REQUIRE(logo != nullptr);
        CHECK(logo->width == 5);
        CHECK(resolver.imageView(0, hud.value(), 2.0f) == hp2x);
    }
    SUBCASE("header-only sizes") {
        // PNG 640x360 and GIF 17x9 headers; the counting decoder would say 1x1.
        const std::string png = {'\x89', 'P', 'N', 'G', '\x0D', '\x0A', '\x1A', '\x0A', 0,      0,
                                 0,      13,  'I', 'H', 'D',    'R',    0,      0,      '\x02', '\x80',
                                 0,      0,   1,   'h', 8,      6,      0,      0,      0};
        const std::string gif = {'G', 'I', 'F', '8', '9', 'a', 17, 0, 9, 0, 0, 0, 0};
        writeText(base / "hp.png", png);
        writeText(base / "logo.png", gif);
        FileAssetResolver resolver(base.string(), countingDecode);
        g_decodeCalls = 0;
        CHECK(resolver.imageSize(0, hud.value()).x == 640.0f);
        CHECK(resolver.imageSize(0, menu.value()).x == 17.0f);
        CHECK(resolver.imageSize(0, hud.value()).y == 360.0f);
        CHECK(g_decodeCalls == 0);
    }
    SUBCASE("embedded assets") {
        AssetId idA = 0;
        AssetId idB = 0;
        std::vector<uint8_t> bytesA = irWithEmbeddedBytes({10, 20, 30}, idA);
        std::vector<uint8_t> bytesB = irWithEmbeddedBytes({1, 2, 3, 4}, idB);
        Result<IrView> a = IrView::open(IrBlob{bytesA.data(), bytesA.size()});
        Result<IrView> b = IrView::open(IrBlob{bytesB.data(), bytesB.size()});
        REQUIRE(a.ok());
        REQUIRE(b.ok());
        REQUIRE(idA == idB);
        FileAssetResolver resolver(base.string(), fakeDecode);
        const ImageData* imgA = resolver.imageView(idA, a.value());
        REQUIRE(imgA != nullptr);
        CHECK(imgA->width == 3);
        const ImageData* imgB = resolver.imageView(idB, b.value());
        REQUIRE(imgB != nullptr);
        CHECK(imgB->width == 4);
        CHECK(resolver.imageSize(idA, a.value()).x == 3.0f);
        CHECK(resolver.imageSize(idB, b.value()).x == 4.0f);
        CHECK(resolver.imageView(idA, a.value()) == imgA);
    }
    fs::remove_all(base, ec);
}

namespace {

/// T-263e ruling B: the file-status function a host injects (xt_bundle's xt::fileStatusUtf8 in
/// the tools; xt_tests does not link xt_bundle, so this one stands in). The test paths are ASCII.
bool testFileStatus(std::string_view utf8Path, uint64_t& size, uint64_t& modifiedTicks) {
    const fs::path path{std::string(utf8Path)};
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec) {
        return false;
    }
    size = static_cast<uint64_t>(fs::file_size(path, ec));
    modifiedTicks = static_cast<uint64_t>(fs::last_write_time(path, ec).time_since_epoch().count());
    return !ec;
}

}  // namespace

// T-263c ruling 5: a decode is cached by file name for the resolver's lifetime and no lookup asks
// the disk again; revalidate() is the host's "the files may have changed" (a reload key).
// T-263e: through the file-status function the host passed.
TEST_CASE("FileAssetResolver::revalidate: a file rewritten under the same name is decoded again") {
    fs::path base = xt::test::tempDir() / "xt_t263c_revalidate";
    std::error_code ec;
    fs::remove_all(base, ec);
    writeText(base / "a.png", "aaa");
    writeText(base / "b.png", "bbbbb");
    std::vector<uint8_t> irBytes = irWithExternalAssets({"a.png", "b.png"});
    Result<IrView> opened = IrView::open(IrBlob{irBytes.data(), irBytes.size()});
    REQUIRE(opened.ok());
    const IrView& view = opened.value();
    // The same content length written again moves the modification time on by itself only when
    // the clock ticked in between; the test sets it explicitly.
    auto touchLater = [&](const fs::path& file) {
        fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(2));
    };

    SUBCASE("rewritten, removed, unchanged") {
        FileAssetResolver resolver(base.string(), fakeDecode);
        resolver.setFileStatusFn(testFileStatus);
        const ImageData* a = resolver.imageView(0, view);
        const ImageData* b = resolver.imageView(1, view);
        REQUIRE(a != nullptr);
        REQUIRE(b != nullptr);
        CHECK(a->rgba[0] == 'a');
        CHECK(resolver.revalidate() == 0);  // nothing changed on disk
        CHECK(resolver.imageView(0, view) == a);

        writeText(base / "a.png", "xyz");  // same size, other pixels
        touchLater(base / "a.png");
        // Not asked to look: the cached decode is still what an asset gets.
        CHECK(resolver.imageView(0, view) == a);
        CHECK(resolver.revalidate() == 1);
        const ImageData* a2 = resolver.imageView(0, view);
        REQUIRE(a2 != nullptr);
        CHECK(a2 != a);
        CHECK(a2->rgba[0] == 'x');
        CHECK(a->rgba[0] == 'a');                 // a pointer handed out earlier stays valid
        CHECK(resolver.imageView(1, view) == b);  // the other file was left alone
        CHECK(resolver.revalidate() == 0);
        CHECK(resolver.imageView(0, view) == a2);

        // Another size at the same modification time is a change too; a removed file is one.
        const fs::file_time_type stamp = fs::last_write_time(base / "a.png");
        writeText(base / "a.png", "xyzw");
        fs::last_write_time(base / "a.png", stamp);
        fs::remove(base / "b.png");
        CHECK(resolver.revalidate() == 2);
        REQUIRE(resolver.imageView(0, view) != nullptr);
        CHECK(resolver.imageView(0, view)->width == 4);
        CHECK(resolver.imageView(1, view) == nullptr);
    }
    SUBCASE("a file that appears in a nearer root") {
        writeText(base / "near" / "placeholder.txt", "-");
        FileAssetResolver resolver(std::vector<std::string>{(base / "near").string(), base.string()},
                                   fakeDecode);
        resolver.setFileStatusFn(testFileStatus);
        REQUIRE(resolver.imageView(0, view) != nullptr);
        CHECK(resolver.imageView(0, view)->width == 3);
        writeText(base / "near" / "a.png", "nnnnnnn");
        touchLater(base / "near" / "a.png");
        CHECK(resolver.revalidate() == 1);
        REQUIRE(resolver.imageView(0, view) != nullptr);
        CHECK(resolver.imageView(0, view)->width == 7);
    }
    SUBCASE("a size read from the header only") {
        const std::string gif17 = {'G', 'I', 'F', '8', '9', 'a', 17, 0, 9, 0, 0, 0, 0};
        const std::string gif21 = {'G', 'I', 'F', '8', '9', 'a', 21, 0, 9, 0, 0, 0, 0};
        writeText(base / "a.png", gif17);
        FileAssetResolver resolver(base.string(), countingDecode);
        resolver.setFileStatusFn(testFileStatus);
        g_decodeCalls = 0;
        CHECK(resolver.imageSize(0, view).x == 17.0f);
        writeText(base / "a.png", gif21);
        CHECK(resolver.imageSize(0, view).x == 17.0f);  // cached
        CHECK(resolver.revalidate() == 0);              // nothing decoded, nothing discarded
        CHECK(resolver.imageSize(0, view).x == 21.0f);
        CHECK(g_decodeCalls == 0);
    }
    SUBCASE("an embedded asset is not a file") {
        AssetId id = 0;
        std::vector<uint8_t> embeddedBytes = irWithEmbeddedBytes({10, 20, 30}, id);
        Result<IrView> embedded = IrView::open(IrBlob{embeddedBytes.data(), embeddedBytes.size()});
        REQUIRE(embedded.ok());
        FileAssetResolver resolver(base.string(), fakeDecode);
        resolver.setFileStatusFn(testFileStatus);
        const ImageData* image = resolver.imageView(id, embedded.value());
        REQUIRE(image != nullptr);
        CHECK(resolver.revalidate() == 0);
        CHECK(resolver.imageView(id, embedded.value()) == image);
    }
    fs::remove_all(base, ec);
}

// T-263e ruling B: xt_core asks nothing about a file's status itself; without an injected
// FileStatusFn there is nothing to compare, and revalidate() does nothing.
TEST_CASE("FileAssetResolver::revalidate: without a file-status function it returns 0 and does nothing") {
    fs::path base = xt::test::tempDir() / "xt_t263e_revalidate_unset";
    std::error_code ec;
    fs::remove_all(base, ec);
    writeText(base / "a.png", "aaa");
    const std::string gif17 = {'G', 'I', 'F', '8', '9', 'a', 17, 0, 9, 0, 0, 0, 0};
    const std::string gif21 = {'G', 'I', 'F', '8', '9', 'a', 21, 0, 9, 0, 0, 0, 0};
    writeText(base / "b.png", gif17);
    std::vector<uint8_t> irBytes = irWithExternalAssets({"a.png", "b.png"});
    Result<IrView> opened = IrView::open(IrBlob{irBytes.data(), irBytes.size()});
    REQUIRE(opened.ok());
    const IrView& view = opened.value();

    FileAssetResolver resolver(base.string(), fakeDecode);
    const ImageData* a = resolver.imageView(0, view);
    REQUIRE(a != nullptr);
    CHECK(resolver.imageSize(1, view).x == 17.0f);  // header only
    writeText(base / "a.png", "xyzw");              // another size: a change by any stamp
    fs::last_write_time(base / "a.png", fs::last_write_time(base / "a.png") + std::chrono::seconds(2));
    writeText(base / "b.png", gif21);
    CHECK(resolver.revalidate() == 0);
    CHECK(resolver.imageView(0, view) == a);
    CHECK(a->width == 3);
    CHECK(resolver.imageSize(1, view).x == 17.0f);  // the header-only size was not dropped either
    fs::remove_all(base, ec);
}

#if defined(_WIN32)
namespace {

/// `wide` in the ANSI code page; empty when that code page cannot spell it exactly.
std::string toAnsi(const std::wstring& wide) {
    BOOL usedDefault = FALSE;
    const int length = static_cast<int>(wide.size());
    const int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide.c_str(), length, nullptr, 0, nullptr,
                                      &usedDefault);
    if (n <= 0 || usedDefault) {
        return std::string();
    }
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide.c_str(), length, out.data(), n, nullptr,
                        &usedDefault);
    return usedDefault ? std::string() : out;
}

bool isUtf8(const std::string& text) {
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                               nullptr, 0) > 0;
}

}  // namespace

// T-261f item 3: every path the runtime layers take is UTF-8; one that is not valid UTF-8 (a
// narrow path in the ANSI code page, here CP950's bytes for the name) is a missing file, never
// opened through the ANSI code page.
TEST_CASE("openFileUtf8 / FileAssetResolver: T-261f a path that is not UTF-8 is a missing file") {
    const std::string utf8Name = "\xe6\xb8\xac\xe8\xa9\xa6.png";  // "測試.png", which CP950 spells
    const std::string ansiName = toAnsi(L"測試.png");
    if (ansiName.empty() || isUtf8(ansiName)) {
        MESSAGE("skipped: the ANSI code page cannot spell the name, or is UTF-8");
        return;
    }
    const fs::path base = xt::test::tempDir() / "xt_t261f_not_utf8";
    std::error_code ec;
    fs::remove_all(base, ec);
    writeText(base / compile::pathFromUtf8(utf8Name), "abc");
    const std::string dir = compile::genericPathToUtf8(base);
    REQUIRE(isUtf8(dir));

    std::FILE* utf8File = xt::openFileUtf8((dir + "/" + utf8Name).c_str(), "rb");
    CHECK(utf8File != nullptr);
    if (utf8File != nullptr) {
        std::fclose(utf8File);
    }
    std::FILE* ansiFile = xt::openFileUtf8((dir + "/" + ansiName).c_str(), "rb");
    CHECK(ansiFile == nullptr);
    if (ansiFile != nullptr) {
        std::fclose(ansiFile);
    }

    FileAssetResolver resolver(dir, countingDecode);
    std::vector<uint8_t> bytes;
    CHECK(resolver.readBytes(utf8Name, bytes));
    CHECK(asText(bytes) == "abc");
    std::vector<uint8_t> untouched;
    CHECK_FALSE(resolver.readBytes(ansiName, untouched));
    CHECK(untouched.empty());

    std::vector<uint8_t> irBytes = irWithExternalAssets({utf8Name, ansiName});
    Result<IrView> opened = IrView::open(IrBlob{irBytes.data(), irBytes.size()});
    REQUIRE(opened.ok());
    ImageData image;
    CHECK(resolver.decodeImage(0, opened.value(), image));
    ImageData missing;
    CHECK_FALSE(resolver.decodeImage(1, opened.value(), missing));
    fs::remove_all(base, ec);
}
#endif
