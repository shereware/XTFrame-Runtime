#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "file_asset_resolver_access.h"
#include "test_temp.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/sw/sw_png.h"
#include "xt/sw/sw_renderer.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// Context::reloadDocument(): same handle, same viewport and clock, a completely new Document.
/// See Docs/design/22-bundle.md §22.5, Docs/tasks/T-93.md "測試".

using namespace xt;

namespace {

/// Compiles `<xt><body>...</body></xt>` in memory; the sources here are known good.
std::vector<uint8_t> compileBody(const std::string& body) {
    const std::string text = "<xt><body>" + body + "</body></xt>";
    const compile::SourceFile file{"reload.xt", text};
    compile::CompileOptions options;
    compile::CompileResult result = compile::compileDocumentFromStrings({&file, 1}, "reload.xt", options);
    for (const compile::Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    return result.ir;
}

/// Fixture A: one `#a`, 40 wide.
std::vector<uint8_t> fixtureA() {
    return compileBody(R"(<div id="a" style="width: 40px; height: 10px"></div>)");
}

/// Fixture B: `#a` grown to 60, plus a second element `#b`.
std::vector<uint8_t> fixtureB() {
    return compileBody(R"(<div id="a" style="width: 60px; height: 10px"></div>)"
                       R"(<div id="b" style="width: 5px; height: 5px"></div>)");
}

float widthOf(Document& doc, const char* id) {
    ElementRef ref = doc.byId(id);
    REQUIRE(ref.valid());
    return ref.boundingBox().w;
}

}  // namespace

TEST_CASE("Context::reloadDocument: same handle, new content, viewport and clock kept") {
    const std::vector<uint8_t> irA = fixtureA();
    const std::vector<uint8_t> irB = fixtureB();

    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, nullptr});
    Result<DocumentHandle> loaded = ctx.loadDocument(IrBlob{irA.data(), irA.size()}, true);
    REQUIRE(loaded.ok());
    const DocumentHandle handle = loaded.value();

    ctx.setViewport(Rect{0, 0, 300, 200}, 1.0f);
    ctx.update(0.0f);
    ctx.update(0.5f);
    REQUIRE(ctx.document(handle) != nullptr);
    CHECK(widthOf(*ctx.document(handle), "a") == doctest::Approx(40));
    const float clockBefore = ctx.clock();

    // The old Document dies here, so the pointer above must not be reused afterwards.
    Result<DocumentHandle> reloaded = ctx.reloadDocument(handle, IrBlob{irB.data(), irB.size()}, true);
    REQUIRE(reloaded.ok());
    CHECK(reloaded.value() == handle);

    ctx.update(0.0f);
    Document* doc = ctx.document(handle);
    REQUIRE(doc != nullptr);
    CHECK(widthOf(*doc, "a") == doctest::Approx(60));
    CHECK(doc->byId("b").valid());
    // The viewport survived the reload: layout still runs against the 300x200 Context viewport
    // (reloadDocument() hands it to the new Document itself, without a setViewport() call).
    CHECK(doc->root().boundingBox().w == doctest::Approx(300));
    CHECK(ctx.clock() == doctest::Approx(clockBefore));
}

TEST_CASE("Context::reloadDocument: a bad blob keeps the current Document") {
    const std::vector<uint8_t> irA = fixtureA();

    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, nullptr});
    Result<DocumentHandle> loaded = ctx.loadDocument(IrBlob{irA.data(), irA.size()}, true);
    REQUIRE(loaded.ok());
    const DocumentHandle handle = loaded.value();
    ctx.setViewport(Rect{0, 0, 300, 200}, 1.0f);
    ctx.update(0.0f);

    const std::vector<uint8_t> junk(32, 0xABu);
    Result<DocumentHandle> reloaded = ctx.reloadDocument(handle, IrBlob{junk.data(), junk.size()}, true);
    CHECK_FALSE(reloaded.ok());

    ctx.update(0.0f);
    REQUIRE(ctx.document(handle) != nullptr);
    CHECK(widthOf(*ctx.document(handle), "a") == doctest::Approx(40));
}

TEST_CASE("Context::reloadDocument: an unloaded handle is kInvalidArgument") {
    const std::vector<uint8_t> irA = fixtureA();
    const std::vector<uint8_t> irB = fixtureB();

    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, nullptr});
    Result<DocumentHandle> loaded = ctx.loadDocument(IrBlob{irA.data(), irA.size()}, true);
    REQUIRE(loaded.ok());
    const DocumentHandle handle = loaded.value();
    ctx.unloadDocument(handle);

    Result<DocumentHandle> reloaded = ctx.reloadDocument(handle, IrBlob{irB.data(), irB.size()}, true);
    CHECK(reloaded.status() == Status::kInvalidArgument);
    CHECK(ctx.document(handle) == nullptr);
    CHECK(ctx.reloadDocument(0, IrBlob{irB.data(), irB.size()}, true).status() == Status::kInvalidArgument);
}

namespace {

/// A 2x2 uncompressed 24-bit BMP (70 bytes whatever the pixels are) whose top-left pixel is
/// (`r`, 0, 0) and the rest black.
std::vector<uint8_t> bmp2x2(uint8_t r) {
    std::vector<uint8_t> b(70, 0);
    b[0] = 'B', b[1] = 'M', b[2] = 70, b[10] = 54;  // file size, pixel data offset
    b[14] = 40, b[18] = 2, b[22] = 2;               // DIB header size, width, height (bottom-up)
    b[26] = 1, b[28] = 24, b[34] = 16;              // planes, bits per pixel, image size
    b[54 + 8 + 2] = r;                              // second stored row = top row; BGR
    return b;
}

/// An IR whose asset 0 is `image`, embedded, drawn by one `<img>`.
std::vector<uint8_t> compileEmbedded(const std::filesystem::path& dir, const std::vector<uint8_t>& image) {
    {
        std::ofstream out(dir / "p.bmp", std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
    }
    const std::string text = "<xt><body><img src=\"p.bmp\"/></body></xt>";
    const compile::SourceFile file{"reload.xt", text};
    compile::CompileOptions options;
    options.rootDir = dir.string();
    options.embedImages = true;
    options.emitMeta = false;
    compile::CompileResult result = compile::compileDocumentFromStrings({&file, 1}, "reload.xt", options);
    REQUIRE(result.ok());
    REQUIRE(result.assets.size() == 1);
    REQUIRE(result.assets[0].embedded);
    return result.ir;
}

}  // namespace

// T-263c B: FileAssetResolver finds an embedded asset's decode by where its bytes are. A reload
// frees the old blob, and the next one may sit at the same address with the same size and another
// image inside.
TEST_CASE(
    "Context::reloadDocument: T-263c an embedded image changed in a blob of the same address and size") {
    namespace fs = std::filesystem;
    const fs::path dir = xt::test::tempDir() / "xt_t263c_reload_embedded";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const std::vector<uint8_t> v1 = compileEmbedded(dir, bmp2x2(10));
    const std::vector<uint8_t> v2 = compileEmbedded(dir, bmp2x2(99));
    const std::vector<uint8_t> v3 = compileEmbedded(dir, bmp2x2(200));
    REQUIRE(v1.size() == v2.size());
    REQUIRE(v1.size() == v3.size());

    FileAssetResolver assets(dir.string(), sw::decodeImageBytes);
    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, &assets});
    auto topLeftRed = [&](DocumentHandle handle) {
        Document* doc = ctx.document(handle);
        REQUIRE(doc != nullptr);
        const ImageData* image = assets.imageView(0, doc->ir());
        REQUIRE(image != nullptr);
        REQUIRE(image->width == 2);
        return static_cast<int>(image->rgba[0]);
    };

    // The host owns the blobs (takeOwnership = false) and reuses the first one's memory.
    std::vector<uint8_t> blobX = v1;
    std::vector<uint8_t> blobY = v2;
    Result<DocumentHandle> loaded = ctx.loadDocument(IrBlob{blobX.data(), blobX.size()}, false);
    REQUIRE(loaded.ok());
    const DocumentHandle handle = loaded.value();
    CHECK(topLeftRed(handle) == 10);
    REQUIRE(ctx.reloadDocument(handle, IrBlob{blobY.data(), blobY.size()}, false).ok());
    CHECK(topLeftRed(handle) == 99);
    // Nothing reads blobX any more: the third version is written over it and loaded from there.
    std::copy(v3.begin(), v3.end(), blobX.begin());
    REQUIRE(ctx.reloadDocument(handle, IrBlob{blobX.data(), blobX.size()}, false).ok());
    CHECK(topLeftRed(handle) == 200);
    // The same bytes loaded again resolve to the same decode (no new entry per reload).
    const ImageData* before = assets.imageView(0, ctx.document(handle)->ir());
    std::copy(v3.begin(), v3.end(), blobY.begin());
    REQUIRE(ctx.reloadDocument(handle, IrBlob{blobY.data(), blobY.size()}, false).ok());
    REQUIRE(ctx.reloadDocument(handle, IrBlob{blobX.data(), blobX.size()}, false).ok());
    CHECK(assets.imageView(0, ctx.document(handle)->ir()) == before);
    fs::remove_all(dir, ec);
}

// T-263e must-fix 1: since T-263b an embedded asset's slot is found by its bytes' address, so a
// reload whose blob sits elsewhere used to leave the previous slot and decode behind for the
// resolver's lifetime. Context now tells the resolver when it drops a Document's IR.
TEST_CASE(
    "Context::reloadDocument: T-263e 200 reloads of a changing embedded image keep one document's worth") {
    namespace fs = std::filesystem;
    using Access = FileAssetResolverTestAccess;
    const fs::path dir = xt::test::tempDir() / "xt_t263e_reload_growth";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    FileAssetResolver assets(dir.string(), sw::decodeImageBytes);
    assets.setIdleDecodeBudget(0);  // T-263g: nothing idle is kept; the default is tested below.
    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, &assets});
    auto topLeftRed = [&](DocumentHandle handle) {
        Document* doc = ctx.document(handle);
        REQUIRE(doc != nullptr);
        const ImageData* image = assets.imageView(0, doc->ir());
        REQUIRE(image != nullptr);
        REQUIRE(image->width == 2);
        return static_cast<int>(image->rgba[0]);
    };

    // The host owns every blob and keeps them all alive, so each reload's blob is at an address
    // of its own (a slot per reload before the fix, whatever the allocator does).
    std::vector<std::vector<uint8_t>> blobs;
    blobs.push_back(compileEmbedded(dir, bmp2x2(1)));
    Result<DocumentHandle> loaded = ctx.loadDocument(IrBlob{blobs[0].data(), blobs[0].size()}, false);
    REQUIRE(loaded.ok());
    const DocumentHandle handle = loaded.value();
    CHECK(topLeftRed(handle) == 1);
    const size_t slotTotal = Access::slotCount(assets);
    const size_t bytes = Access::decodedBytes(assets);
    const size_t entries = Access::entryCount(assets);
    const size_t keys = Access::fileKeyCount(assets);
    CHECK(slotTotal == 1);
    CHECK(bytes >= 16);  // one 2x2 RGBA image
    MESSAGE("one document: slots " << slotTotal << ", decoded bytes " << bytes << ", entries " << entries);

    for (int i = 0; i < 200; ++i) {
        const uint8_t red = static_cast<uint8_t>(2 + i);
        blobs.push_back(compileEmbedded(dir, bmp2x2(red)));
        REQUIRE(ctx.reloadDocument(handle, IrBlob{blobs.back().data(), blobs.back().size()}, false).ok());
        REQUIRE(topLeftRed(handle) == red);
    }
    MESSAGE("after 200 reloads: slots " << Access::slotCount(assets) << ", decoded bytes "
                                        << Access::decodedBytes(assets) << ", entries "
                                        << Access::entryCount(assets));
    CHECK(Access::slotCount(assets) == slotTotal);
    CHECK(Access::decodedBytes(assets) == bytes);
    CHECK(Access::entryCount(assets) == entries);
    CHECK(Access::fileKeyCount(assets) == keys);

    // The Document's own copy of the blob (takeOwnership) and hotReload() go the same way.
    for (int i = 0; i < 8; ++i) {
        const std::vector<uint8_t> ir = compileEmbedded(dir, bmp2x2(static_cast<uint8_t>(210 + i)));
        if (i % 2 == 0) {
            REQUIRE(ctx.reloadDocument(handle, IrBlob{ir.data(), ir.size()}, true).ok());
        } else {
            BundleDelta delta;
            delta.ir.push_back(IrReload{handle, IrBlob{ir.data(), ir.size()}, true});
            REQUIRE(ctx.hotReload(delta) == Status::kOk);
        }
        REQUIRE(topLeftRed(handle) == 210 + i);
    }
    CHECK(Access::slotCount(assets) == slotTotal);
    CHECK(Access::decodedBytes(assets) == bytes);
    CHECK(Access::entryCount(assets) == entries);

    // An unloaded Document leaves nothing behind.
    ctx.unloadDocument(handle);
    CHECK(Access::slotCount(assets) == 0);
    CHECK(Access::decodedBytes(assets) == 0);
    CHECK(Access::fileKeyCount(assets) == 0);
    fs::remove_all(dir, ec);
}

// T-263f item 13: the editor's recompile and xtframe-view's reload key call resetAssetIds() before
// loading the new document; that used to clear the slots only, so releaseSource() of the old IR
// found nothing in its blob and the decode of every embedded image that had changed stayed until
// the resolver went. resetAssetIds() now frees what its embedded slots resolved to.
TEST_CASE(
    "FileAssetResolver::resetAssetIds: T-263f 200 editor-style recompiles of a changing embedded "
    "image keep one document's worth") {
    namespace fs = std::filesystem;
    using Access = FileAssetResolverTestAccess;
    const fs::path dir = xt::test::tempDir() / "xt_t263f_reset_growth";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    FileAssetResolver assets(dir.string(), sw::decodeImageBytes);
    assets.setIdleDecodeBudget(0);  // T-263g: nothing idle is kept; the default is tested below.
    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, &assets});
    auto topLeftRed = [&](DocumentHandle handle) {
        Document* doc = ctx.document(handle);
        REQUIRE(doc != nullptr);
        const ImageData* image = assets.imageView(0, doc->ir());
        REQUIRE(image != nullptr);
        REQUIRE(image->width == 2);
        return static_cast<int>(image->rgba[0]);
    };

    // The host owns every blob and keeps them all alive: each document's blob has its own address.
    std::vector<std::vector<uint8_t>> blobs;
    blobs.push_back(compileEmbedded(dir, bmp2x2(1)));
    Result<DocumentHandle> loaded = ctx.loadDocument(IrBlob{blobs[0].data(), blobs[0].size()}, false);
    REQUIRE(loaded.ok());
    DocumentHandle handle = loaded.value();
    CHECK(topLeftRed(handle) == 1);
    const size_t slotTotal = Access::slotCount(assets);
    const size_t bytes = Access::decodedBytes(assets);
    const size_t entries = Access::entryCount(assets);
    const size_t keys = Access::fileKeyCount(assets);
    CHECK(slotTotal == 1);
    CHECK(bytes >= 16);  // one 2x2 RGBA image
    CHECK(entries == 1);

    // What the editor's recompile does: resetAssetIds(), then the new document is loaded (and
    // drawn), then the old one is dropped.
    int red = 1;
    for (int i = 0; i < 200; ++i) {
        red = 2 + i;
        assets.resetAssetIds();
        blobs.push_back(compileEmbedded(dir, bmp2x2(static_cast<uint8_t>(red))));
        Result<DocumentHandle> next =
            ctx.loadDocument(IrBlob{blobs.back().data(), blobs.back().size()}, false);
        REQUIRE(next.ok());
        REQUIRE(topLeftRed(next.value()) == red);
        ctx.unloadDocument(handle);
        handle = next.value();
    }
    MESSAGE("after 200 recompiles: slots " << Access::slotCount(assets) << ", decoded bytes "
                                           << Access::decodedBytes(assets) << ", entries "
                                           << Access::entryCount(assets));
    CHECK(Access::slotCount(assets) == slotTotal);
    CHECK(Access::decodedBytes(assets) == bytes);
    CHECK(Access::entryCount(assets) == entries);
    CHECK(Access::fileKeyCount(assets) == keys);

    // A document still loaded when the ids are reset looks its image up again, and its unload
    // frees that decode too.
    assets.resetAssetIds();
    CHECK(Access::slotCount(assets) == 0);
    CHECK(Access::decodedBytes(assets) == 0);
    CHECK(Access::fileKeyCount(assets) == 0);
    CHECK(topLeftRed(handle) == red);
    CHECK(Access::slotCount(assets) == slotTotal);
    CHECK(Access::decodedBytes(assets) == bytes);
    CHECK(Access::entryCount(assets) == entries);
    ctx.unloadDocument(handle);
    CHECK(Access::slotCount(assets) == 0);
    CHECK(Access::decodedBytes(assets) == 0);
    CHECK(Access::fileKeyCount(assets) == 0);
    fs::remove_all(dir, ec);
}

namespace {

/// T-263g: the decode function of the tests below; counts what is really decoded.
int g_t263gDecodes = 0;
bool countingDecode(const uint8_t* bytes, size_t size, ImageData& out) {
    ++g_t263gDecodes;
    return sw::decodeImageBytes(bytes, size, out);
}

void writeImage(const std::filesystem::path& file, const std::vector<uint8_t>& bytes) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

/// An IR that draws each of the files `names` (present under `dir`) with one `<img>`; none is
/// embedded, so every asset is a named one.
std::vector<uint8_t> compileNamed(const std::filesystem::path& dir, const std::vector<std::string>& names) {
    std::string text = "<xt><body>";
    for (const std::string& name : names) {
        text += "<img src=\"" + name + "\"/>";
    }
    text += "</body></xt>";
    const compile::SourceFile file{"named.xt", text};
    compile::CompileOptions options;
    options.rootDir = dir.string();
    options.embedImages = false;
    options.emitMeta = false;
    compile::CompileResult result = compile::compileDocumentFromStrings({&file, 1}, "named.xt", options);
    REQUIRE(result.ok());
    REQUIRE(result.assets.size() == names.size());
    return result.ir;
}

/// The decoded image of the asset `view` calls `name` (nullptr: no such asset, or no decode).
const ImageData* imageNamed(FileAssetResolver& assets, const IrView& view, std::string_view name) {
    const std::span<const IrAsset> table = view.assets().assets();
    for (size_t i = 0; i < table.size(); ++i) {
        if (view.string(table[i].name) == name) {
            return assets.imageView(static_cast<AssetId>(i), view);
        }
    }
    return nullptr;
}

/// As Tests/unit/test_file_asset_resolver.cpp: the file-status function a host injects (xt_tests
/// does not link xt_bundle's). The test paths are ASCII.
bool reloadTestFileStatus(std::string_view utf8Path, uint64_t& size, uint64_t& modifiedTicks) {
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

/// What the T-263g tests share: a directory, a counting resolver over it and a Context.
struct IdleFixture {
    std::filesystem::path dir;
    FileAssetResolver assets;
    NullTextShaper shaper;
    Context ctx;

    explicit IdleFixture(const char* tag)
        : dir(makeDir(tag)), assets(dir.string(), countingDecode), ctx(ContextConfig{&shaper, &assets}) {
        g_t263gDecodes = 0;
    }
    ~IdleFixture() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    static std::filesystem::path makeDir(const char* tag) {
        const std::filesystem::path dir = xt::test::tempDir() / tag;
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        return dir;
    }
    DocumentHandle load(const std::vector<uint8_t>& ir) {
        Result<DocumentHandle> loaded = ctx.loadDocument(IrBlob{ir.data(), ir.size()}, true);
        REQUIRE(loaded.ok());
        return loaded.value();
    }
    /// The red of the top-left pixel of the image `handle`'s document calls `name`.
    int red(DocumentHandle handle, std::string_view name) {
        Document* doc = ctx.document(handle);
        REQUIRE(doc != nullptr);
        const ImageData* image = imageNamed(assets, doc->ir(), name);
        REQUIRE(image != nullptr);
        REQUIRE(image->width == 2);
        return static_cast<int>(image->rgba[0]);
    }
};

}  // namespace

// T-263f item 13, T-263g items 8 and 10: resetAssetIds() forgets every slot, so no live source
// holds any decode; a file's decode and an embedded image's both move to the idle list and the
// next lookup takes each back -- by file path, by content -- without decoding. With an idle
// budget of 0 both are freed (and decoded again).
TEST_CASE("FileAssetResolver::resetAssetIds: T-263g decodes move to the idle list and come back undecoded") {
    namespace fs = std::filesystem;
    using Access = FileAssetResolverTestAccess;
    const fs::path dir = xt::test::tempDir() / "xt_t263f_reset_named";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const std::vector<uint8_t> embeddedIr = compileEmbedded(dir, bmp2x2(7));
    writeImage(dir / "named.bmp", bmp2x2(50));
    const std::vector<uint8_t> namedIr = compileNamed(dir, {"named.bmp"});

    g_t263gDecodes = 0;
    FileAssetResolver assets(dir.string(), countingDecode);
    Result<IrView> embeddedView = IrView::open(IrBlob{embeddedIr.data(), embeddedIr.size()});
    Result<IrView> namedView = IrView::open(IrBlob{namedIr.data(), namedIr.size()});
    REQUIRE(embeddedView.ok());
    REQUIRE(namedView.ok());
    const ImageData* file = assets.imageView(0, namedView.value());
    REQUIRE(file != nullptr);
    REQUIRE(file->rgba[0] == 50);
    const size_t namedBytes = Access::decodedBytes(assets);
    REQUIRE(assets.imageView(0, embeddedView.value()) != nullptr);
    const size_t allBytes = Access::decodedBytes(assets);
    CHECK(allBytes > namedBytes);
    CHECK(Access::fileKeyCount(assets) == 2);
    CHECK(g_t263gDecodes == 2);

    assets.resetAssetIds();
    CHECK(Access::slotCount(assets) == 0);
    CHECK(Access::decodedBytes(assets) == allBytes);  // nothing was freed:
    CHECK(Access::idleCount(assets) == 2);            // both decodes are idle
    CHECK(Access::idleBytes(assets) == allBytes);
    CHECK(Access::fileKeyCount(assets) == 2);
    const ImageData* fileAgain = assets.imageView(0, namedView.value());
    REQUIRE(fileAgain != nullptr);
    CHECK(fileAgain->rgba[0] == 50);
    const ImageData* again = assets.imageView(0, embeddedView.value());
    REQUIRE(again != nullptr);
    CHECK(again->rgba[0] == 7);
    CHECK(g_t263gDecodes == 2);  // found by path / by content, not decoded
    CHECK(Access::idleCount(assets) == 0);
    CHECK(Access::idleBytes(assets) == 0);
    CHECK(Access::decodedBytes(assets) == allBytes);
    CHECK(Access::entryCount(assets) == 2);

    assets.setIdleDecodeBudget(0);
    assets.resetAssetIds();
    CHECK(Access::slotCount(assets) == 0);
    CHECK(Access::decodedBytes(assets) == 0);
    CHECK(Access::fileKeyCount(assets) == 0);
    CHECK(Access::idleCount(assets) == 0);
    fileAgain = assets.imageView(0, namedView.value());
    REQUIRE(fileAgain != nullptr);
    CHECK(fileAgain->rgba[0] == 50);
    again = assets.imageView(0, embeddedView.value());
    REQUIRE(again != nullptr);
    CHECK(again->rgba[0] == 7);
    CHECK(g_t263gDecodes == 4);
    CHECK(Access::entryCount(assets) == 2);  // the freed entries were taken again
    fs::remove_all(dir, ec);
}

// T-263g ruling 8: a re-packed bundle gives a changed image a new content-hash name, and hot
// reload after a re-pack is a host editor's loop. Named decodes used to stay for the resolver's
// lifetime; now each is held by the live sources that looked it up, and what no source holds is
// kept on an idle list bounded by bytes.
TEST_CASE(
    "FileAssetResolver: T-263g 200 hot reloads where one image's name changes stay within the live "
    "set plus the idle budget") {
    using Access = FileAssetResolverTestAccess;
    IdleFixture f("xt_t263g_named_reloads");
    writeImage(f.dir / "keep.bmp", bmp2x2(250));
    writeImage(f.dir / "v0.bmp", bmp2x2(1));
    const DocumentHandle handle = f.load(compileNamed(f.dir, {"keep.bmp", "v0.bmp"}));
    CHECK(f.red(handle, "keep.bmp") == 250);
    CHECK(f.red(handle, "v0.bmp") == 1);
    const size_t live = Access::decodedBytes(f.assets);  // two 2x2 images
    const size_t one = live / 2;
    REQUIRE(one >= 16);
    const size_t budget = 3 * one;
    f.assets.setIdleDecodeBudget(budget);

    size_t most = 0;
    for (int i = 1; i <= 200; ++i) {
        const std::string name = "v" + std::to_string(i) + ".bmp";
        const int red = 1 + i % 200;
        writeImage(f.dir / name, bmp2x2(static_cast<uint8_t>(red)));
        const std::vector<uint8_t> ir = compileNamed(f.dir, {"keep.bmp", name});
        REQUIRE(f.ctx.reloadDocument(handle, IrBlob{ir.data(), ir.size()}, true).ok());
        REQUIRE(f.red(handle, name) == red);
        REQUIRE(f.red(handle, "keep.bmp") == 250);
        most = std::max(most, Access::decodedBytes(f.assets));
    }
    MESSAGE("after 200 reloads: decoded bytes " << Access::decodedBytes(f.assets) << " (most " << most
                                                << "), live " << live << ", budget " << budget << ", slots "
                                                << Access::slotCount(f.assets) << ", entries "
                                                << Access::entryCount(f.assets));
    CHECK(most <= live + budget);
    CHECK(Access::decodedBytes(f.assets) == live + Access::idleBytes(f.assets));
    CHECK(Access::idleBytes(f.assets) <= budget);
    CHECK(Access::idleCount(f.assets) >= 1);  // the last versions released are kept,
    CHECK(Access::idleCount(f.assets) <= 3);  // (keep.bmp passes through the list on each reload)
    CHECK(Access::slotCount(f.assets) == 2 + Access::idleCount(f.assets));
    CHECK(Access::fileKeyCount(f.assets) == 2 + Access::idleCount(f.assets));
    CHECK(Access::entryCount(f.assets) <= 2 + 3 + 1);
    CHECK(g_t263gDecodes == 202);  // keep.bmp once, every version once
}

TEST_CASE(
    "FileAssetResolver: T-263g unload then reload of the same UI within the budget decodes nothing "
    "again") {
    using Access = FileAssetResolverTestAccess;
    IdleFixture f("xt_t263g_reopen");
    writeImage(f.dir / "keep.bmp", bmp2x2(250));
    const std::vector<uint8_t> ir = compileNamed(f.dir, {"keep.bmp"});
    DocumentHandle handle = f.load(ir);
    CHECK(f.red(handle, "keep.bmp") == 250);
    CHECK(g_t263gDecodes == 1);
    const size_t one = Access::decodedBytes(f.assets);
    REQUIRE(one >= 16);

    f.ctx.unloadDocument(handle);
    CHECK(Access::decodedBytes(f.assets) == one);  // not freed at once (the default budget)
    CHECK(Access::idleCount(f.assets) == 1);
    CHECK(Access::idleBytes(f.assets) == one);
    CHECK(Access::slotCount(f.assets) == 1);  // the slot waits with its decode

    // Not read again either: the file is gone, the slot still leads to its decode.
    std::error_code ec;
    std::filesystem::remove(f.dir / "keep.bmp", ec);
    handle = f.load(ir);
    CHECK(f.red(handle, "keep.bmp") == 250);
    CHECK(g_t263gDecodes == 1);
    CHECK(Access::idleCount(f.assets) == 0);
    CHECK(Access::idleBytes(f.assets) == 0);
    CHECK(Access::decodedBytes(f.assets) == one);
    CHECK(Access::entryCount(f.assets) == 1);
}

TEST_CASE("FileAssetResolver: T-263g an idle budget of 0 frees a named decode at unload") {
    using Access = FileAssetResolverTestAccess;
    IdleFixture f("xt_t263g_budget0");
    f.assets.setIdleDecodeBudget(0);
    writeImage(f.dir / "keep.bmp", bmp2x2(250));
    const std::vector<uint8_t> ir = compileNamed(f.dir, {"keep.bmp"});
    DocumentHandle handle = f.load(ir);
    CHECK(f.red(handle, "keep.bmp") == 250);
    CHECK(g_t263gDecodes == 1);
    CHECK(Access::decodedBytes(f.assets) >= 16);

    f.ctx.unloadDocument(handle);
    CHECK(Access::decodedBytes(f.assets) == 0);
    CHECK(Access::slotCount(f.assets) == 0);
    CHECK(Access::fileKeyCount(f.assets) == 0);
    CHECK(Access::idleCount(f.assets) == 0);

    handle = f.load(ir);
    CHECK(f.red(handle, "keep.bmp") == 250);
    CHECK(g_t263gDecodes == 2);
    CHECK(Access::entryCount(f.assets) == 1);  // the freed entry was taken again
}

TEST_CASE("FileAssetResolver: T-263g a named decode two live sources share stays live when one is released") {
    using Access = FileAssetResolverTestAccess;
    IdleFixture f("xt_t263g_shared");
    writeImage(f.dir / "keep.bmp", bmp2x2(250));
    const std::vector<uint8_t> ir = compileNamed(f.dir, {"keep.bmp"});
    const DocumentHandle a = f.load(ir);
    const DocumentHandle b = f.load(ir);
    const ImageData* viaA = imageNamed(f.assets, f.ctx.document(a)->ir(), "keep.bmp");
    const ImageData* viaB = imageNamed(f.assets, f.ctx.document(b)->ir(), "keep.bmp");
    REQUIRE(viaA != nullptr);
    CHECK(viaA == viaB);
    CHECK(g_t263gDecodes == 1);
    const size_t one = Access::decodedBytes(f.assets);

    f.ctx.unloadDocument(a);
    CHECK(Access::idleCount(f.assets) == 0);  // still held by the other document
    CHECK(Access::idleBytes(f.assets) == 0);
    CHECK(Access::decodedBytes(f.assets) == one);
    CHECK(imageNamed(f.assets, f.ctx.document(b)->ir(), "keep.bmp") == viaB);
    CHECK(f.red(b, "keep.bmp") == 250);
    CHECK(g_t263gDecodes == 1);

    f.ctx.unloadDocument(b);
    CHECK(Access::idleCount(f.assets) == 1);  // nobody holds it now
    CHECK(Access::idleBytes(f.assets) == one);
    CHECK(Access::decodedBytes(f.assets) == one);
}

TEST_CASE(
    "FileAssetResolver::setIdleDecodeBudget: T-263g lowering the budget frees the least recently "
    "released decodes at once") {
    using Access = FileAssetResolverTestAccess;
    IdleFixture f("xt_t263g_lower_budget");
    const char* const names[3] = {"a.bmp", "b.bmp", "c.bmp"};
    std::vector<uint8_t> irs[3];
    DocumentHandle handles[3] = {};
    for (int i = 0; i < 3; ++i) {
        writeImage(f.dir / names[i], bmp2x2(static_cast<uint8_t>(10 + i)));
        irs[i] = compileNamed(f.dir, {names[i]});
        handles[i] = f.load(irs[i]);
        CHECK(f.red(handles[i], names[i]) == 10 + i);
    }
    CHECK(g_t263gDecodes == 3);
    const size_t one = Access::decodedBytes(f.assets) / 3;
    REQUIRE(one >= 16);
    for (int i = 0; i < 3; ++i) {
        f.ctx.unloadDocument(handles[i]);  // a first, c last
    }
    CHECK(Access::idleCount(f.assets) == 3);
    CHECK(Access::idleBytes(f.assets) == 3 * one);
    CHECK(Access::decodedBytes(f.assets) == 3 * one);

    f.assets.setIdleDecodeBudget(2 * one);
    CHECK(Access::idleCount(f.assets) == 2);
    CHECK(Access::idleBytes(f.assets) == 2 * one);
    CHECK(Access::decodedBytes(f.assets) == 2 * one);
    CHECK(Access::slotCount(f.assets) == 2);
    CHECK(Access::fileKeyCount(f.assets) == 2);

    // a.bmp, released first, is the one that went; c.bmp is still there.
    handles[2] = f.load(irs[2]);
    CHECK(f.red(handles[2], "c.bmp") == 12);
    CHECK(g_t263gDecodes == 3);
    handles[0] = f.load(irs[0]);
    CHECK(f.red(handles[0], "a.bmp") == 10);
    CHECK(g_t263gDecodes == 4);
    CHECK(Access::idleCount(f.assets) == 1);  // b.bmp
}

// T-263g ruling 8 (f), as ruling 10 rewrote it: resetAssetIds() does not empty the idle list --
// what it forgets moves onto it -- and setIdleDecodeBudget(0) does.
TEST_CASE(
    "FileAssetResolver::setIdleDecodeBudget: T-263g 0 empties the idle list, resetAssetIds() fills it") {
    using Access = FileAssetResolverTestAccess;
    IdleFixture f("xt_t263g_reset_idle");
    writeImage(f.dir / "keep.bmp", bmp2x2(250));
    const std::vector<uint8_t> ir = compileNamed(f.dir, {"keep.bmp"});
    const DocumentHandle handle = f.load(ir);
    CHECK(f.red(handle, "keep.bmp") == 250);
    const size_t one = Access::decodedBytes(f.assets);
    REQUIRE(one >= 16);

    // The document is still loaded: its decode waits on the idle list until it is asked for again.
    f.assets.resetAssetIds();
    CHECK(Access::slotCount(f.assets) == 0);
    CHECK(Access::idleCount(f.assets) == 1);
    CHECK(Access::idleBytes(f.assets) == one);
    CHECK(Access::decodedBytes(f.assets) == one);
    CHECK(f.red(handle, "keep.bmp") == 250);
    CHECK(g_t263gDecodes == 1);
    CHECK(Access::idleCount(f.assets) == 0);

    f.ctx.unloadDocument(handle);
    CHECK(Access::idleCount(f.assets) == 1);
    f.assets.resetAssetIds();  // nothing live to forget; the idle decode stays
    CHECK(Access::idleCount(f.assets) == 1);
    CHECK(Access::idleBytes(f.assets) == one);

    f.assets.setIdleDecodeBudget(0);
    CHECK(Access::idleCount(f.assets) == 0);
    CHECK(Access::idleBytes(f.assets) == 0);
    CHECK(Access::decodedBytes(f.assets) == 0);
    CHECK(Access::slotCount(f.assets) == 0);
    CHECK(Access::fileKeyCount(f.assets) == 0);
}

// T-263g (ruling 8, dispatcher's addition): revalidate() checks an idle decode like any other;
// one whose file changed is freed at once.
TEST_CASE("FileAssetResolver::revalidate: T-263g an idle decode whose file changed is freed") {
    using Access = FileAssetResolverTestAccess;
    IdleFixture f("xt_t263g_revalidate_idle");
    f.assets.setFileStatusFn(reloadTestFileStatus);
    writeImage(f.dir / "keep.bmp", bmp2x2(250));
    writeImage(f.dir / "other.bmp", bmp2x2(40));
    const std::vector<uint8_t> ir = compileNamed(f.dir, {"keep.bmp", "other.bmp"});
    DocumentHandle handle = f.load(ir);
    CHECK(f.red(handle, "keep.bmp") == 250);
    CHECK(f.red(handle, "other.bmp") == 40);
    CHECK(g_t263gDecodes == 2);
    const size_t one = Access::decodedBytes(f.assets) / 2;
    f.ctx.unloadDocument(handle);
    CHECK(Access::idleCount(f.assets) == 2);

    writeImage(f.dir / "keep.bmp", bmp2x2(99));
    std::filesystem::last_write_time(
        f.dir / "keep.bmp", std::filesystem::last_write_time(f.dir / "keep.bmp") + std::chrono::seconds(2));
    CHECK(f.assets.revalidate() == 1);
    CHECK(Access::idleCount(f.assets) == 1);
    CHECK(Access::idleBytes(f.assets) == one);
    CHECK(Access::decodedBytes(f.assets) == one);
    CHECK(Access::slotCount(f.assets) == 1);
    CHECK(Access::fileKeyCount(f.assets) == 1);

    handle = f.load(ir);
    CHECK(f.red(handle, "other.bmp") == 40);
    CHECK(g_t263gDecodes == 2);  // taken back
    CHECK(f.red(handle, "keep.bmp") == 99);
    CHECK(g_t263gDecodes == 3);  // the rewritten file
    CHECK(Access::idleCount(f.assets) == 0);
}

// T-263g ruling 10: the editor recompiles on every edit, and resetAssetIds() used to free every
// embedded decode, so each keystroke decoded every embedded image again. The idle list takes
// embedded decodes too, under their content key.
TEST_CASE(
    "FileAssetResolver::resetAssetIds: T-263g 200 editor-style recompiles of an unchanged embedded "
    "image decode it once") {
    using Access = FileAssetResolverTestAccess;
    IdleFixture f("xt_t263g_recompile_unchanged");
    // The host owns every blob and keeps them all alive: each document's blob has its own address.
    std::vector<std::vector<uint8_t>> blobs;
    blobs.push_back(compileEmbedded(f.dir, bmp2x2(77)));
    auto loadBlob = [&](const std::vector<uint8_t>& blob) {
        Result<DocumentHandle> loaded = f.ctx.loadDocument(IrBlob{blob.data(), blob.size()}, false);
        REQUIRE(loaded.ok());
        return loaded.value();
    };
    auto red = [&](DocumentHandle handle) {
        const ImageData* image = f.assets.imageView(0, f.ctx.document(handle)->ir());
        REQUIRE(image != nullptr);
        return static_cast<int>(image->rgba[0]);
    };
    DocumentHandle handle = loadBlob(blobs[0]);
    CHECK(red(handle) == 77);
    CHECK(g_t263gDecodes == 1);
    const size_t bytes = Access::decodedBytes(f.assets);
    REQUIRE(bytes >= 16);

    // What the editor's recompile does: resetAssetIds(), then the new document is loaded (and
    // drawn), then the old one is dropped.
    for (int i = 0; i < 200; ++i) {
        f.assets.resetAssetIds();
        blobs.push_back(compileEmbedded(f.dir, bmp2x2(77)));
        const DocumentHandle next = loadBlob(blobs.back());
        REQUIRE(red(next) == 77);
        f.ctx.unloadDocument(handle);
        handle = next;
    }
    CHECK(g_t263gDecodes == 1);
    CHECK(Access::decodedBytes(f.assets) == bytes);  // one document's worth, nothing beside it
    CHECK(Access::idleCount(f.assets) == 0);
    CHECK(Access::slotCount(f.assets) == 1);
    CHECK(Access::entryCount(f.assets) == 1);

    // Reloads (releaseSource() without a reset) go the same way.
    for (int i = 0; i < 200; ++i) {
        blobs.push_back(compileEmbedded(f.dir, bmp2x2(77)));
        REQUIRE(f.ctx.reloadDocument(handle, IrBlob{blobs.back().data(), blobs.back().size()}, false).ok());
        REQUIRE(red(handle) == 77);
    }
    CHECK(g_t263gDecodes == 1);
    CHECK(Access::decodedBytes(f.assets) == bytes);
    CHECK(Access::slotCount(f.assets) == 1);
    CHECK(Access::entryCount(f.assets) == 1);

    // Closed and opened again: still the one decode.
    f.ctx.unloadDocument(handle);
    CHECK(Access::idleCount(f.assets) == 1);
    CHECK(Access::idleBytes(f.assets) == bytes);
    CHECK(Access::slotCount(f.assets) == 0);
    handle = loadBlob(blobs.back());
    CHECK(red(handle) == 77);
    CHECK(g_t263gDecodes == 1);
    CHECK(Access::idleCount(f.assets) == 0);
}

// T-263g ruling 10: with a changing embedded image the T-263e / T-263f bound becomes one
// document's worth plus the idle budget -- a changed image's old decode ages out.
TEST_CASE(
    "FileAssetResolver: T-263g 200 recompiles and 200 reloads of a changing embedded image stay "
    "within the live set plus the idle budget") {
    using Access = FileAssetResolverTestAccess;
    IdleFixture f("xt_t263g_recompile_changing");
    std::vector<std::vector<uint8_t>> blobs;
    blobs.push_back(compileEmbedded(f.dir, bmp2x2(1)));
    auto loadBlob = [&](const std::vector<uint8_t>& blob) {
        Result<DocumentHandle> loaded = f.ctx.loadDocument(IrBlob{blob.data(), blob.size()}, false);
        REQUIRE(loaded.ok());
        return loaded.value();
    };
    auto red = [&](DocumentHandle handle) {
        const ImageData* image = f.assets.imageView(0, f.ctx.document(handle)->ir());
        REQUIRE(image != nullptr);
        return static_cast<int>(image->rgba[0]);
    };
    DocumentHandle handle = loadBlob(blobs[0]);
    CHECK(red(handle) == 1);
    const size_t live = Access::decodedBytes(f.assets);
    REQUIRE(live >= 16);
    const size_t defaultBudget = size_t{32} * 1024 * 1024;

    // The default budget (32 MiB): every old decode of this small image fits, none is freed.
    for (int i = 0; i < 200; ++i) {
        f.assets.resetAssetIds();
        blobs.push_back(compileEmbedded(f.dir, bmp2x2(static_cast<uint8_t>(2 + i))));
        const DocumentHandle next = loadBlob(blobs.back());
        REQUIRE(red(next) == 2 + i);
        f.ctx.unloadDocument(handle);
        handle = next;
        REQUIRE(Access::decodedBytes(f.assets) <= live + defaultBudget);
        REQUIRE(Access::decodedBytes(f.assets) == live + Access::idleBytes(f.assets));
    }
    CHECK(g_t263gDecodes == 201);
    CHECK(Access::idleCount(f.assets) == 200);
    CHECK(Access::slotCount(f.assets) == 1);

    // A budget of three images: the old decodes age out, least recently released first.
    const size_t budget = 3 * live;
    f.assets.setIdleDecodeBudget(budget);
    CHECK(Access::idleCount(f.assets) == 3);
    CHECK(Access::decodedBytes(f.assets) == live + budget);
    size_t most = 0;
    for (int i = 0; i < 200; ++i) {
        const uint8_t value = static_cast<uint8_t>(200 - i);
        if (i % 2 == 0) {
            f.assets.resetAssetIds();
            blobs.push_back(compileEmbedded(f.dir, bmp2x2(value)));
            const DocumentHandle next = loadBlob(blobs.back());
            REQUIRE(red(next) == value);
            f.ctx.unloadDocument(handle);
            handle = next;
        } else {
            blobs.push_back(compileEmbedded(f.dir, bmp2x2(value)));
            REQUIRE(
                f.ctx.reloadDocument(handle, IrBlob{blobs.back().data(), blobs.back().size()}, false).ok());
            REQUIRE(red(handle) == value);
        }
        most = std::max(most, Access::decodedBytes(f.assets));
    }
    MESSAGE("changing embedded image: most decoded bytes " << most << ", live " << live << ", budget "
                                                           << budget);
    CHECK(most <= live + budget);
    CHECK(Access::decodedBytes(f.assets) == live + budget);
    CHECK(Access::idleCount(f.assets) == 3);
    CHECK(Access::slotCount(f.assets) == 1);
    CHECK(Access::fileKeyCount(f.assets) == 4);
    CHECK(Access::entryCount(f.assets) <= 201);  // no entry was added after the budget was lowered

    // The last one drawn is 1 and the list holds 4, 3 and 2; the reload releases 1, which pushes
    // 4 out, and going back to 3 decodes nothing.
    const int decodes = g_t263gDecodes;
    blobs.push_back(compileEmbedded(f.dir, bmp2x2(3)));
    REQUIRE(f.ctx.reloadDocument(handle, IrBlob{blobs.back().data(), blobs.back().size()}, false).ok());
    CHECK(red(handle) == 3);
    CHECK(g_t263gDecodes == decodes);
    CHECK(Access::idleCount(f.assets) == 2);  // 2 and 1
}

// T-263g ruling 9: two Documents loaded from one host-owned blob are two live views (two load
// serials) over the same embedded bytes, which share one slot found by the bytes' address. The
// slot remembered the one serial it had last checked the bytes for, so with both documents drawn
// every frame the bytes were hashed again on every lookup that came from the other view.
TEST_CASE(
    "FileAssetResolver: T-263g two documents over one blob hash the embedded bytes once per view, not "
    "per frame") {
    using Access = FileAssetResolverTestAccess;
    IdleFixture f("xt_t263g_two_views_hash");
    const std::vector<uint8_t> blob = compileEmbedded(f.dir, bmp2x2(66));
    f.ctx.setViewport(Rect{0, 0, 64, 32}, 1.0f);
    Result<DocumentHandle> a = f.ctx.loadDocument(IrBlob{blob.data(), blob.size()}, false);
    Result<DocumentHandle> b = f.ctx.loadDocument(IrBlob{blob.data(), blob.size()}, false);
    REQUIRE(a.ok());
    REQUIRE(b.ok());

    std::vector<uint8_t> px(static_cast<size_t>(64 * 32 * 4), 0);
    sw::SwRenderer renderer;
    renderer.setTarget(sw::SwTarget{px.data(), 64, 32, 64 * 4});
    CommandList list;
    size_t perFrame[10] = {};
    for (int frame = 0; frame < 10; ++frame) {
        const size_t before = Access::embeddedHashCount(f.assets);
        f.ctx.update(frame == 0 ? 0.0f : 1.0f / 60.0f);
        f.ctx.render(list);
        renderer.clear(Color{0, 0, 0, 0});
        renderer.render(list, nullptr, &f.assets, &f.ctx.document(a.value())->ir());
        perFrame[frame] = Access::embeddedHashCount(f.assets) - before;
    }
    MESSAGE("embedded hashes per frame: " << perFrame[0] << " " << perFrame[1] << " " << perFrame[2] << " "
                                          << perFrame[3] << " ... " << perFrame[9] << "; total "
                                          << Access::embeddedHashCount(f.assets));
    CHECK(px[0] == 66);  // both documents drew the image
    CHECK(g_t263gDecodes == 1);
    CHECK(Access::embeddedSlotCount(f.assets) == 1);
    // The first frame: the bytes are checked once for each view, and hashed once more for the
    // decode's content key. After that nothing is hashed again.
    CHECK(perFrame[0] == 3);
    for (int frame = 1; frame < 10; ++frame) {
        CAPTURE(frame);
        CHECK(perFrame[frame] == 0);
    }

    // The view that is released takes its serial with it; the other one is still verified.
    const size_t beforeRelease = Access::embeddedHashCount(f.assets);
    f.ctx.unloadDocument(b.value());
    f.ctx.update(1.0f / 60.0f);
    f.ctx.render(list);
    renderer.render(list, nullptr, &f.assets, &f.ctx.document(a.value())->ir());
    CHECK(g_t263gDecodes == 1);
    MESSAGE("hashes after releasing one view: " << Access::embeddedHashCount(f.assets) - beforeRelease);
}

TEST_CASE("Context::loadDocument: a Document loaded after setViewport gets the Context viewport") {
    // 13 §13.8.14 ruling (T-236): no second setViewport() call is needed after the load.
    const std::vector<uint8_t> ir = fixtureA();
    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, nullptr});
    ctx.setViewport(Rect{0, 0, 300, 200}, 1.0f);
    Result<DocumentHandle> loaded = ctx.loadDocument(IrBlob{ir.data(), ir.size()}, true);
    REQUIRE(loaded.ok());
    ctx.update(0.0f);
    Document* doc = ctx.document(loaded.value());
    REQUIRE(doc != nullptr);
    CHECK(doc->root().boundingBox().w == doctest::Approx(300));
}

TEST_CASE("Context::documentSerial: T-260c one serial per installed Document, 0 for none") {
    const std::vector<uint8_t> irA = fixtureA();
    const std::vector<uint8_t> irB = fixtureB();
    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, nullptr});
    CHECK(ctx.documentSerial(0) == 0);
    CHECK(ctx.documentSerial(1) == 0);  // Nothing loaded yet.
    Result<DocumentHandle> loaded = ctx.loadDocument(IrBlob{irA.data(), irA.size()}, false);
    REQUIRE(loaded.ok());
    const DocumentHandle handle = loaded.value();
    const uint32_t first = ctx.documentSerial(handle);
    CHECK(first != 0);
    CHECK(ctx.document(handle)->loadSerial() == first);
    // The same handle reloaded twice in a row from the same blob: three Documents, three serials,
    // whatever addresses they get.
    REQUIRE(ctx.reloadDocument(handle, IrBlob{irB.data(), irB.size()}, false).ok());
    const uint32_t second = ctx.documentSerial(handle);
    REQUIRE(ctx.reloadDocument(handle, IrBlob{irB.data(), irB.size()}, false).ok());
    const uint32_t third = ctx.documentSerial(handle);
    CHECK(second > first);
    CHECK(third > second);
    CHECK(ctx.document(handle)->loadSerial() == third);
    // A failed reload installs nothing, so the serial stays.
    const uint8_t junk[4] = {1, 2, 3, 4};
    CHECK_FALSE(ctx.reloadDocument(handle, IrBlob{junk, sizeof junk}, false).ok());
    CHECK(ctx.documentSerial(handle) == third);
    ctx.unloadDocument(handle);
    CHECK(ctx.documentSerial(handle) == 0);
}
