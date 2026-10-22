#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "test_temp.h"
#include "xt/asset_resolver.h"
#include "xt/bundle/bundle.h"
#include "xt/bundle/pack.h"
#include "xt/bundle/project_file.h"
#include "xt/compile/utf8_path.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/sw/sw_png.h"
#include "xt/text_shaper.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

/// loadUi()/reloadUi(): loading a published bundle's UI into a Context and hot-reloading it after
/// a re-pack. See Docs/design/22-bundle.md §22.5, Docs/tasks/T-93.md "測試".

using namespace xt;
using namespace xt::bundle;

namespace {

namespace fs = std::filesystem;

/// A fresh, writable copy of the `sample` source bundle under the system temp dir.
std::string copiedSample(const std::string& name) {
    const fs::path dir = xt::test::tempDir() / ("xt_load_src_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::copy(std::string(XT_TEST_BUNDLE_DIR) + "/fixtures/sample", dir, fs::copy_options::recursive, ec);
    REQUIRE(ec.value() == 0);
    return dir.generic_string();
}

/// An empty output directory under the system temp dir.
std::string outDir(const std::string& name) {
    const fs::path dir = xt::test::tempDir() / ("xt_load_out_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    return dir.generic_string();
}

/// Loads `<dir>/bundle.xtproj`; the fixture is known good, so a failure aborts the test.
ProjectFile loadProject(const std::string& dir) {
    std::string error;
    Result<ProjectFile> project = ProjectFile::load(dir, &error);
    REQUIRE_MESSAGE(project.ok(), error);
    return std::move(project.value());
}

/// Packs `source` into `out`; `onlyUis` empty means the whole project.
void packInto(const std::string& source, const std::string& out, std::vector<std::string> onlyUis) {
    PackOptions options;
    options.outDir = out;
    options.onlyUis = std::move(onlyUis);
    options.thumbnails = false;
    REQUIRE(pack(loadProject(source), options).ok);
}

/// Opens the published bundle at `dir`; a failure aborts the test.
std::unique_ptr<Bundle> openBundle(const std::string& dir) {
    std::string error = "unchanged";
    Result<std::unique_ptr<Bundle>> bundle = Bundle::open(dir, sw::decodeImageBytes, &error);
    REQUIRE_MESSAGE(bundle.ok(), error);
    return std::move(bundle.value());
}

}  // namespace

TEST_CASE("loadUi: loads a packed UI and resolves its assets through the bundle") {
    const std::string source = copiedSample("load");
    const std::string out = outDir("load");
    packInto(source, out, {});

    std::unique_ptr<Bundle> bundle = openBundle(out);
    NullTextShaper shaper;
    // §22.5: the Context must use the bundle's own resolver, otherwise "assets/<hash>.<ext>"
    // resolves against the wrong root.
    Context ctx(ContextConfig{&shaper, &bundle->assets()});

    Result<DocumentHandle> handle = loadUi(ctx, *bundle, "hud");
    REQUIRE(handle.ok());
    ctx.setViewport(Rect{0, 0, 1280, 720}, 1.0f);
    ctx.update(0.0f);

    Document* doc = ctx.document(handle.value());
    REQUIRE(doc != nullptr);
    CHECK(doc->byId("fire").valid());

    // The <img> asset: packed as assets/<hash>.png and decodable through bundle.assets().
    const IrView& view = doc->ir();
    REQUIRE(view.assets().assets().size() == 1);
    CHECK(std::string(view.string(view.assets().assets()[0].name)).rfind("assets/", 0) == 0);
    const Vec2 size = bundle->assets().imageSize(0, view);
    CHECK(size.x > 0);
    CHECK(size.y > 0);

    CHECK(loadUi(ctx, *bundle, "nope").status() == Status::kNotFound);
}

TEST_CASE("reloadUi: a re-packed UI replaces the Document behind the same handle") {
    const std::string source = copiedSample("reload");
    const std::string out = outDir("reload");
    packInto(source, out, {});

    std::unique_ptr<Bundle> bundle = openBundle(out);
    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, &bundle->assets()});
    Result<DocumentHandle> handle = loadUi(ctx, *bundle, "hud");
    REQUIRE(handle.ok());
    ctx.setViewport(Rect{0, 0, 1280, 720}, 1.0f);
    ctx.update(0.0f);
    REQUIRE(ctx.document(handle.value()) != nullptr);
    CHECK_FALSE(ctx.document(handle.value())->byId("added").valid());

    // Edit the source UI the way the editor's "save" would, then re-pack that one UI.
    {
        std::ofstream file(source + "/hud/hud.xt", std::ios::binary | std::ios::trunc);
        REQUIRE(file.good());
        file << "<xt>\n  <body>\n    <div id=\"panel\">\n      <img src=\"img/a.png\"/>\n"
             << "      <button id=\"fire\">Fire</button>\n      <span id=\"added\">new</span>\n"
             << "    </div>\n  </body>\n</xt>\n";
    }
    packInto(source, out, {"hud"});

    const std::vector<std::string> changed = bundle->reload();
    REQUIRE(changed.size() == 1);
    CHECK(changed[0] == "hud");

    Result<DocumentHandle> reloaded = reloadUi(ctx, *bundle, handle.value(), "hud");
    REQUIRE(reloaded.ok());
    CHECK(reloaded.value() == handle.value());

    ctx.update(0.0f);
    Document* doc = ctx.document(handle.value());
    REQUIRE(doc != nullptr);
    CHECK(doc->byId("added").valid());
    CHECK(doc->byId("fire").valid());

    CHECK(reloadUi(ctx, *bundle, handle.value(), "nope").status() == Status::kNotFound);
}

namespace xt {

/// T-263e: what a FileAssetResolver holds for embedded assets (Docs/tasks/T-263e.md must-fix 1).
struct FileAssetResolverTestAccess {
    static size_t embeddedSlotCount(const FileAssetResolver& r) {
        return r.m_embedded[0].size() + r.m_embedded[1].size();
    }
    static size_t decodedBytes(const FileAssetResolver& r) {
        size_t bytes = 0;
        for (const FileAssetResolver::Entry& entry : r.m_entries) {
            bytes += entry.image.rgba.capacity() + entry.compressed.blocks.capacity();
        }
        return bytes;
    }
};

}  // namespace xt

namespace {

/// A 2x2 uncompressed 24-bit BMP whose top-left pixel is (`r`, 0, 0) and the rest black.
std::vector<uint8_t> bmp2x2(uint8_t r) {
    std::vector<uint8_t> b(70, 0);
    b[0] = 'B', b[1] = 'M', b[2] = 70, b[10] = 54;  // file size, pixel data offset
    b[14] = 40, b[18] = 2, b[22] = 2;               // DIB header size, width, height (bottom-up)
    b[26] = 1, b[28] = 24, b[34] = 16;              // planes, bits per pixel, image size
    b[54 + 8 + 2] = r;                              // second stored row = top row; BGR
    return b;
}

void writeBytes(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    REQUIRE(file.good());
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

// T-263e must-fix 1: the bundle's reload of a UI goes through Context::reloadDocument, which tells
// the bundle's resolver (the Context's, §22.5) that the replaced Document's IR is dropped.
TEST_CASE("reloadUi: T-263e re-packed UIs with a changing embedded image keep one document's worth") {
    using Access = FileAssetResolverTestAccess;
    const std::string source = copiedSample("reload_embedded");
    const std::string out = outDir("reload_embedded");
    {
        std::ofstream file(source + "/bundle.xtproj", std::ios::binary | std::ios::trunc);
        REQUIRE(file.good());
        file << R"({ "version": 2, "name": "MyGame UI", "id": "mygame",
  "uis": [ { "id": "hud", "doc": "hud/hud.xt", "size": [1280, 720] } ],
  "shared": { "assetRoot": "shared", "styles": ["shared/base.xts"] },
  "publish": { "embedImages": true, "embedFonts": false } })";
    }
    writeBytes(source + "/hud/img/a.png", bmp2x2(10));
    packInto(source, out, {});

    std::unique_ptr<Bundle> bundle = openBundle(out);
    FileAssetResolver& assets = static_cast<FileAssetResolver&>(bundle->assets());
    // T-263g: nothing idle is kept, so the count below is the live document's alone (the idle
    // list and its budget are tested in Tests/unit/test_context_reload.cpp).
    assets.setIdleDecodeBudget(0);
    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, &bundle->assets()});
    Result<DocumentHandle> handle = loadUi(ctx, *bundle, "hud");
    REQUIRE(handle.ok());
    auto topLeftRed = [&]() {
        Document* doc = ctx.document(handle.value());
        REQUIRE(doc != nullptr);
        REQUIRE(doc->ir().assets().assets().size() == 1);
        REQUIRE(doc->ir().assets().assets()[0].embedded != 0);
        const ImageData* image = assets.imageView(0, doc->ir());
        REQUIRE(image != nullptr);
        return static_cast<int>(image->rgba[0]);
    };
    CHECK(topLeftRed() == 10);
    const size_t slotTotal = Access::embeddedSlotCount(assets);
    const size_t bytes = Access::decodedBytes(assets);
    CHECK(slotTotal == 1);
    CHECK(bytes >= 16);

    for (int i = 0; i < 6; ++i) {
        writeBytes(source + "/hud/img/a.png", bmp2x2(static_cast<uint8_t>(20 + i)));
        packInto(source, out, {"hud"});
        REQUIRE(bundle->reload().size() == 1);
        REQUIRE(reloadUi(ctx, *bundle, handle.value(), "hud").ok());
        CHECK(topLeftRed() == 20 + i);
    }
    CHECK(Access::embeddedSlotCount(assets) == slotTotal);
    CHECK(Access::decodedBytes(assets) == bytes);
}

// T-263e ruling B: xt_core has no file-status call of its own; xt_bundle provides the function
// hosts inject into FileAssetResolver, and Bundle::open sets it on its own resolver.
TEST_CASE("fileStatusUtf8: T-263e size and modification ticks of a UTF-8 path") {
    const fs::path dir = xt::test::tempDir() / "xt_t263e_file_status";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path file = dir / fs::path(u8"アイコン.png");  // "アイコン.png"
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        out << "abc";
    }
    const std::string utf8 = compile::genericPathToUtf8(file);
    uint64_t size = 0;
    uint64_t ticks = 0;
    REQUIRE(xt::fileStatusUtf8(utf8, size, ticks));
    CHECK(size == 3);
    uint64_t sizeAgain = 0;
    uint64_t ticksAgain = 0;
    REQUIRE(xt::fileStatusUtf8(utf8, sizeAgain, ticksAgain));
    CHECK(ticksAgain == ticks);  // an untouched file reports the same ticks

    fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(2));
    uint64_t ticksLater = 0;
    REQUIRE(xt::fileStatusUtf8(utf8, sizeAgain, ticksLater));
    CHECK(ticksLater != ticks);

    uint64_t untouched = 77;
    CHECK_FALSE(xt::fileStatusUtf8(compile::genericPathToUtf8(dir), untouched, ticksLater));  // a directory
    CHECK_FALSE(xt::fileStatusUtf8(utf8 + ".missing", untouched, ticksLater));
    CHECK_FALSE(xt::fileStatusUtf8(std::string_view(), untouched, ticksLater));
    CHECK(untouched == 77);
    fs::remove_all(dir, ec);
}

#if defined(_WIN32)
// T-261f item 3: a path that is not valid UTF-8 (a narrow path in the ANSI code page, here CP950's
// bytes for the name) is a missing file, never read through the ANSI code page.
TEST_CASE("fileStatusUtf8: T-261f a path that is not UTF-8 is a missing file") {
    const std::wstring wideName = L"測試.png";                    // "測試.png", which CP950 spells
    const std::string utf8Name = "\xe6\xb8\xac\xe8\xa9\xa6.png";  // the same name as UTF-8
    BOOL usedDefault = FALSE;
    char ansi[64] = {};
    const int n =
        WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wideName.c_str(), static_cast<int>(wideName.size()),
                            ansi, sizeof(ansi), nullptr, &usedDefault);
    const std::string ansiName(ansi, n > 0 ? static_cast<size_t>(n) : 0);
    if (n <= 0 || usedDefault ||
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ansi, n, nullptr, 0) > 0) {
        MESSAGE("skipped: the ANSI code page cannot spell the name, or is UTF-8");
        return;
    }
    const fs::path dir = xt::test::tempDir() / "xt_t261f_file_status";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    {
        std::ofstream out(dir / wideName, std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        out << "abc";
    }
    const std::string dirUtf8 = compile::genericPathToUtf8(dir);
    uint64_t size = 0;
    uint64_t ticks = 0;
    CHECK(xt::fileStatusUtf8(dirUtf8 + "/" + utf8Name, size, ticks));
    CHECK(size == 3);
    uint64_t untouched = 77;
    CHECK_FALSE(xt::fileStatusUtf8(dirUtf8 + "/" + ansiName, untouched, ticks));
    CHECK(untouched == 77);
    fs::remove_all(dir, ec);
}
#endif

TEST_CASE(
    "Bundle::assets: T-263e revalidate() works on the bundle's own resolver (file status set by open)") {
    const std::string source = copiedSample("revalidate");
    const std::string out = outDir("revalidate");
    packInto(source, out, {});

    std::unique_ptr<Bundle> bundle = openBundle(out);
    FileAssetResolver& assets = static_cast<FileAssetResolver&>(bundle->assets());
    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, &bundle->assets()});
    Result<DocumentHandle> handle = loadUi(ctx, *bundle, "hud");
    REQUIRE(handle.ok());
    const IrView& view = ctx.document(handle.value())->ir();
    REQUIRE(view.assets().assets().size() == 1);
    const std::string name(view.string(view.assets().assets()[0].name));
    const ImageData* before = assets.imageView(0, view);
    REQUIRE(before != nullptr);
    CHECK(assets.revalidate() == 0);  // nothing changed on disk

    // The packed file rewritten under the same name (other pixels, another size).
    writeBytes(out + "/" + name, bmp2x2(40));
    const fs::path packed = compile::pathFromUtf8(out + "/" + name);
    fs::last_write_time(packed, fs::last_write_time(packed) + std::chrono::seconds(2));
    CHECK(assets.revalidate() == 1);
    const ImageData* after = assets.imageView(0, view);
    REQUIRE(after != nullptr);
    CHECK(after != before);
    CHECK(after->width == 2);
    CHECK(after->rgba[0] == 40);
}
