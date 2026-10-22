#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "atlas_pack.h"
#include "doctest.h"
#include "font_subset.h"
#include "test_temp.h"
#include "texture_compress.h"
#include "xt/asset_resolver.h"
#include "xt/bundle/bundle.h"
#include "xt/bundle/pack.h"
#include "xt/bundle/project_file.h"
#include "xt/command_list.h"
#include "xt/context.h"
#include "xt/dds.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/sw/sw_png.h"
#include "xt/sw/sw_renderer.h"
#include "xt/text_shaper.h"

/// T-199 (Docs/design/26-layers-symbols-library.md §26.3.4, 22 §22.10, 12 §12.7): the asset
/// pipeline of pack() -- MaxRects atlas pages and the `atlas:<page>#<asset>` rewrite (pixel-equal
/// to the un-atlased bundle), `@2x` variants, `--compress` DDS output and its round trip, and
/// `--font-subset`.

using namespace xt;
using namespace xt::bundle;

namespace {

namespace fs = std::filesystem;

/// A fresh copy of the `sample` fixture with a nine-slice image, its @2x variant, a font and a
/// string table added; `hud.xt` uses a stretched border-image, a background-image and an <img>.
std::string makeProject(const std::string& name) {
    const fs::path dir = xt::test::tempDir() / ("xt_t199_src_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::copy(std::string(XT_TEST_BUNDLE_DIR) + "/fixtures/sample", dir, fs::copy_options::recursive, ec);
    REQUIRE(ec.value() == 0);
    // 24 x 24 nine-slice: distinct corner / edge / center colours.
    auto nineSlice = [](int scale) {
        const int size = 24 * scale;
        std::vector<uint8_t> rgba(static_cast<size_t>(size) * static_cast<size_t>(size) * 4);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const int cx = x < 8 * scale ? 0 : (x >= size - 8 * scale ? 2 : 1);
                const int cy = y < 8 * scale ? 0 : (y >= size - 8 * scale ? 2 : 1);
                uint8_t* p =
                    rgba.data() +
                    (static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)) * 4;
                p[0] = static_cast<uint8_t>(60 + cx * 90);
                p[1] = static_cast<uint8_t>(60 + cy * 90);
                p[2] = static_cast<uint8_t>((cx + cy) == 2 ? 200 : 40);
                p[3] = (cx == 1 && cy == 1) ? 128 : 255;
            }
        }
        return rgba;
    };
    const std::vector<uint8_t> one = nineSlice(1);
    const std::vector<uint8_t> two = nineSlice(2);
    REQUIRE(sw::writePng((dir / "hud" / "img" / "nine.png").string(), one.data(), 24, 24, 24 * 4));
    REQUIRE(sw::writePng((dir / "hud" / "img" / "nine@2x.png").string(), two.data(), 48, 48, 48 * 4));
    fs::create_directories(dir / "shared" / "fonts", ec);
    fs::copy_file(std::string(XT_TEST_BUNDLE_DIR) + "/../fonts/NotoSans-Regular.subset.ttf",
                  dir / "shared" / "fonts" / "NotoSans-Regular.subset.ttf",
                  fs::copy_options::overwrite_existing, ec);
    REQUIRE(ec.value() == 0);
    {
        std::ofstream xts(dir / "shared" / "base.xts", std::ios::app);
        xts << "\n@font-face { font-family: \"Noto Sans\"; src: \"fonts/NotoSans-Regular.subset.ttf\"; "
               "}\n"
               "#panel { font-family: \"Noto Sans\"; }\n";
    }
    {
        std::ofstream xt(dir / "hud" / "hud.xt", std::ios::trunc);
        xt << "<xt>\n<head><style>\n"
              "#panel { position: absolute; left: 10px; top: 10px; width: 120px; height: 80px;\n"
              "         border-image: url(\"img/nine.png\") 8 fill; }\n"
              "#bg { position: absolute; left: 140px; top: 10px; width: 48px; height: 48px; "
              "background-image: url(\"img/nine.png\"); }\n"
              "#pic { position: absolute; left: 10px; top: 100px; width: 64px; height: 32px; object-fit: "
              "cover; }\n"
              "</style></head>\n"
              "<body>\n<div id=\"panel\"><button id=\"fire\">Fire</button></div>\n"
              "<div id=\"bg\"></div>\n<img id=\"pic\" src=\"img/a.png\"/>\n"
              "</body>\n</xt>\n";
    }
    fs::create_directories(dir / "strings", ec);
    {
        std::ofstream csv(dir / "strings" / "en.csv", std::ios::trunc);
        csv << "key,text\nfire,Fire \xE4\xB8\xAD\n";  // U+4E2D: not in the Latin test font.
    }
    return dir.generic_string();
}

std::string outDir(const std::string& name) {
    const fs::path dir = xt::test::tempDir() / ("xt_t199_out_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    return dir.generic_string();
}

ProjectFile loadProject(const std::string& dir) {
    std::string error;
    Result<ProjectFile> project = ProjectFile::load(dir, &error);
    REQUIRE_MESSAGE(project.ok(), error);
    return std::move(project.value());
}

PackReport packWith(const std::string& source, const std::string& out, PackOptions options) {
    options.outDir = out;
    options.thumbnails = false;
    return pack(loadProject(source), options);
}

std::unique_ptr<Bundle> openBundle(const std::string& dir) {
    std::string error = "unchanged";
    Result<std::unique_ptr<Bundle>> bundle = Bundle::open(dir, sw::decodeImageBytes, &error);
    REQUIRE_MESSAGE(bundle.ok(), error);
    return std::move(bundle.value());
}

/// Renders the bundle's `hud` at 200 x 150 (dpr 1) through the software backend.
std::vector<uint8_t> renderHud(Bundle& bundle, float dpr = 1.0f) {
    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, &bundle.assets()});
    Result<DocumentHandle> handle = loadUi(ctx, bundle, "hud");
    REQUIRE(handle.ok());
    const int w = static_cast<int>(200 * dpr);
    const int h = static_cast<int>(150 * dpr);
    ctx.setViewport(Rect{0, 0, 200, 150}, dpr);
    ctx.update(0.0f);
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    sw::SwTarget target{pixels.data(), w, h, w * 4};
    CommandList list;
    ctx.render(list);
    sw::SwRenderer renderer;
    renderer.setTarget(target);
    renderer.clear(Color{1, 1, 1, 1});
    renderer.render(list, &shaper, &bundle.assets(), &ctx.document(handle.value())->ir(), nullptr);
    return pixels;
}

int maxChannelDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int* differing) {
    REQUIRE(a.size() == b.size());
    int worst = 0;
    int count = 0;
    for (size_t i = 0; i < a.size(); i += 4) {
        int d = 0;
        for (size_t c = 0; c < 4; ++c) {
            d = std::max(d, std::abs(static_cast<int>(a[i + c]) - static_cast<int>(b[i + c])));
        }
        worst = std::max(worst, d);
        count += d > 0 ? 1 : 0;
    }
    if (differing != nullptr) {
        *differing = count;
    }
    return worst;
}

double psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    REQUIRE(a.size() == b.size());
    double mse = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        const double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
        mse += d * d;
    }
    mse /= static_cast<double>(a.size());
    return mse <= 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

std::string readText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/// T-262a: every regular file below `dir` (relative generic path -> bytes).
std::map<std::string, std::string> snapshot(const std::string& dir) {
    std::map<std::string, std::string> files;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            files[fs::relative(entry.path(), dir).generic_string()] = readText(entry.path());
        }
    }
    return files;
}

}  // namespace

TEST_CASE("packAtlas: MaxRects places every input without overlap, inside power-of-two pages") {
    std::vector<AtlasInput> inputs;
    for (int i = 0; i < 40; ++i) {
        inputs.push_back(AtlasInput{"r" + std::to_string(i), 16 + (i * 37) % 90, 12 + (i * 53) % 70});
    }
    inputs.push_back(AtlasInput{"huge", 5000, 10});
    std::vector<AtlasPage> pages;
    std::vector<std::string> tooLarge;
    packAtlas(inputs, 256, 2, pages, tooLarge);
    REQUIRE(tooLarge.size() == 1);
    CHECK(tooLarge[0] == "huge");
    size_t placed = 0;
    for (const AtlasPage& page : pages) {
        CHECK((page.width & (page.width - 1)) == 0);
        CHECK((page.height & (page.height - 1)) == 0);
        CHECK(page.width <= 256);
        CHECK(page.height <= 256);
        for (size_t i = 0; i < page.entries.size(); ++i) {
            const AtlasPlacement& a = page.entries[i];
            CHECK(a.x >= 2);
            CHECK(a.y >= 2);
            CHECK(a.x + a.width + 2 <= page.width);
            CHECK(a.y + a.height + 2 <= page.height);
            for (size_t j = i + 1; j < page.entries.size(); ++j) {
                const AtlasPlacement& b = page.entries[j];
                const bool apart = a.x + a.width + 2 <= b.x || b.x + b.width + 2 <= a.x ||
                                   a.y + a.height + 2 <= b.y || b.y + b.height + 2 <= a.y;
                CHECK(apart);
            }
            ++placed;
        }
    }
    CHECK(placed == 40);
    CHECK(pages.size() >= 2);
}

TEST_CASE("pack --atlas: pages, manifest atlas[], atlas: names, pixels equal to the plain bundle") {
    const std::string source = makeProject("atlas");
    const std::string plainOut = outDir("atlas_plain");
    const std::string atlasOut = outDir("atlas_pages");
    REQUIRE(packWith(source, plainOut, PackOptions{}).ok);
    PackOptions options;
    options.atlas = true;
    options.atlasScope = "bundle";
    const PackReport report = packWith(source, atlasOut, options);
    for (const compile::Diagnostic& d : report.diagnostics) {
        MESSAGE(d.file, ": ", d.message);
    }
    REQUIRE(report.ok);
    CHECK(report.atlasPages == 1);
    CHECK(report.atlasEntries == 2);  // nine.png (border-image + background) and a.png (<img>).
    CHECK(report.atlasPixelsUsed > 0);
    CHECK(report.atlasPixelsTotal >= report.atlasPixelsUsed);
    CHECK(report.variants2x == 1);

    std::unique_ptr<Bundle> bundle = openBundle(atlasOut);
    REQUIRE(bundle->atlases().size() == 1);
    const AtlasInfo& page = bundle->atlases()[0];
    CHECK(page.name == "0");
    CHECK(page.file == "atlas/0.png");
    CHECK(page.file2x == "atlas/0@2x.png");  // nine@2x.png exists, so the page has a 2x twin.
    CHECK(page.entries.size() == 2);
    CHECK(fs::exists(fs::path(atlasOut) / page.file));
    CHECK(fs::exists(fs::path(atlasOut) / page.file2x));
    int atlasedRows = 0;
    for (const AssetInfo& info : bundle->assetTable()) {
        if (!info.atlases.empty()) {
            ++atlasedRows;
            CHECK(info.atlases[0] == "0");
            CHECK_FALSE(fs::exists(fs::path(atlasOut) / info.file));  // Its own file is gone.
        }
    }
    CHECK(atlasedRows == 2);
    // The .xtir names every atlased image `atlas:0#<asset>` (kind 3) and its cell resolves.
    {
        NullTextShaper shaper;
        Context ctx(ContextConfig{&shaper, &bundle->assets()});
        Result<DocumentHandle> handle = loadUi(ctx, *bundle, "hud");
        REQUIRE(handle.ok());
        const IrView& view = ctx.document(handle.value())->ir();
        int atlasNames = 0;
        // nine.png is the one atlased asset with a @2x variant; its IR name is `atlas:0#<file>`.
        std::string nineFile;
        for (const AssetInfo& info : bundle->assetTable()) {
            if (!info.variant2x.empty() && !info.atlases.empty()) {
                nineFile = info.file;
            }
        }
        REQUIRE_FALSE(nineFile.empty());
        AssetId nineId = kInvalidAsset;
        AssetId nextId = 0;
        for (const IrAsset& asset : view.assets().assets()) {
            const std::string name(view.string(asset.name));
            const AssetId id = nextId++;
            if (name == "atlas:0#" + nineFile) {
                nineId = id;
            }
            if (isAtlasAssetName(name)) {
                ++atlasNames;
                CHECK(asset.kind == 3);
                CHECK(name.rfind("atlas:0#assets/", 0) == 0);
            }
        }
        CHECK(atlasNames == 2);
        // T-267a (review L21): an atlased image reports its own cell (nine.png is 24 x 24), never
        // the atlas page it was packed into.
        REQUIRE(nineId != kInvalidAsset);
        const Vec2 nineSize = bundle->assets().imageSize(nineId, view);
        CHECK(nineSize.x == 24.0f);
        CHECK(nineSize.y == 24.0f);
    }
    std::unique_ptr<Bundle> plain = openBundle(plainOut);
    const std::vector<uint8_t> a = renderHud(*plain);
    const std::vector<uint8_t> b = renderHud(*bundle);
    int differing = 0;
    const int worst = maxChannelDifference(a, b, &differing);
    CHECK(worst <= 1);
    (void)differing;
    // dpr 2 samples the 2x page: the nine-slice panel (whose source has a @2x variant) draws the
    // same pixels as the plain bundle's nine@2x.png; a.png (no variant) is upscaled differently
    // (nearest into the page vs. bilinear from the 1x file), so only the panel is compared.
    const std::vector<uint8_t> a2 = renderHud(*plain, 2.0f);
    const std::vector<uint8_t> b2 = renderHud(*bundle, 2.0f);
    int worst2 = 0;
    for (int y = 20; y < 180; ++y) {
        for (int x = 20; x < 260; ++x) {
            const size_t at = (static_cast<size_t>(y) * 400 + static_cast<size_t>(x)) * 4;
            for (size_t c = 0; c < 4; ++c) {
                worst2 =
                    std::max(worst2, std::abs(static_cast<int>(a2[at + c]) - static_cast<int>(b2[at + c])));
            }
        }
    }
    CHECK(worst2 <= 2);
}

TEST_CASE("pack --atlas ui scope names pages per UI; incremental runs are refused") {
    const std::string source = makeProject("atlas_ui");
    const std::string out = outDir("atlas_ui");
    PackOptions options;
    options.atlas = true;
    options.atlasScope = "ui";
    REQUIRE(packWith(source, out, options).ok);
    std::unique_ptr<Bundle> bundle = openBundle(out);
    REQUIRE(bundle->atlases().size() == 2);
    CHECK(bundle->atlases()[0].name == "hud-0");
    CHECK(bundle->atlases()[1].name == "pause-0");
    PackOptions incremental;
    incremental.atlas = true;
    incremental.onlyUis = {"hud"};
    CHECK_FALSE(packWith(source, outDir("atlas_ui_inc"), incremental).ok);
}

TEST_CASE("T-262a: an incremental pack into a bundle built with --atlas or --font-subset is refused") {
    const std::string source = makeProject("inc_refused");
    for (int run = 0; run < 2; ++run) {
        CAPTURE(run);
        const std::string out = outDir(run == 0 ? "inc_refused_atlas" : "inc_refused_subset");
        PackOptions full;
        full.atlas = run == 0;
        full.fontSubset = run == 1;
        REQUIRE(packWith(source, out, full).ok);
        const std::map<std::string, std::string> before = snapshot(out);
        // The editor's "Publish Current UI": --ui without the pipeline options.
        PackOptions incremental;
        incremental.onlyUis = {"hud"};
        const PackReport report = packWith(source, out, incremental);
        CHECK_FALSE(report.ok);
        bool refused = false;
        for (const compile::Diagnostic& d : report.diagnostics) {
            refused = refused || d.message ==
                                     "incremental pack is not supported for a bundle built with --atlas or "
                                     "--font-subset; run a full pack";
        }
        CHECK(refused);
        CHECK(snapshot(out) == before);
    }
}

TEST_CASE("T-262a: Bundle::reload() after a re-pack refreshes the asset table, atlases and cell meta") {
    const std::string source = makeProject("reload");
    const std::string out = outDir("reload");
    REQUIRE(packWith(source, out, PackOptions{}).ok);
    std::unique_ptr<Bundle> bundle = openBundle(out);
    CHECK(bundle->atlases().empty());

    PackOptions options;
    options.atlas = true;
    options.atlasScope = "bundle";
    REQUIRE(packWith(source, out, options).ok);
    bundle->reload();
    std::unique_ptr<Bundle> fresh = openBundle(out);
    REQUIRE(fresh->atlases().size() == 1);
    REQUIRE(bundle->atlases().size() == 1);
    REQUIRE(bundle->assetTable().size() == fresh->assetTable().size());
    for (size_t i = 0; i < fresh->assetTable().size(); ++i) {
        CHECK(bundle->assetTable()[i].file == fresh->assetTable()[i].file);
        CHECK(bundle->assetTable()[i].atlases == fresh->assetTable()[i].atlases);
    }
    const AtlasInfo& page = bundle->atlases()[0];
    REQUIRE(page.entries.size() == 2);
    for (const AtlasEntryInfo& entry : page.entries) {
        const AssetResolver::ImageMeta* meta =
            bundle->assets().imageMeta(std::string(kAtlasAssetScheme) + page.name + "#" + entry.asset);
        REQUIRE(meta != nullptr);
        CHECK(meta->inAtlas);
        CHECK(meta->atlasFile == page.file);
        CHECK(meta->x == entry.x);
        CHECK(meta->y == entry.y);
        CHECK(meta->w == entry.w);
        CHECK(meta->h == entry.h);
    }
    // The nine-slice image keeps its @2x meta.
    int variants = 0;
    for (const AssetInfo& info : bundle->assetTable()) {
        if (info.variant2x.empty()) {
            continue;
        }
        const AssetResolver::ImageMeta* meta = bundle->assets().imageMeta(info.file);
        REQUIRE(meta != nullptr);
        CHECK(meta->variant2x == info.variant2x);
        ++variants;
    }
    CHECK(variants == 1);
}

TEST_CASE("T-262a: a failing font subset after the atlas pass deletes none of the files the .xtir names") {
    const std::string source = makeProject("subset_fail");
    const std::string out = outDir("subset_fail");
    // A directory where the subset font goes makes its write fail after the pages were built.
    std::error_code ec;
    fs::create_directories(fs::path(out) / "fonts" / "NotoSans-Regular.subset.ttf", ec);
    REQUIRE(ec.value() == 0);
    PackOptions options;
    options.atlas = true;
    options.atlasScope = "bundle";
    options.fontSubset = true;
    const PackReport report = packWith(source, out, options);
    CHECK_FALSE(report.ok);
    CHECK(report.atlasPages == 1);
    std::unique_ptr<Bundle> bundle = openBundle(out);
    int named = 0;
    for (const UiInfo& ui : bundle->uis()) {
        for (const std::string& asset : ui.assets) {
            CHECK_MESSAGE(fs::exists(fs::path(out) / asset), asset);
            ++named;
        }
    }
    CHECK(named > 0);
}

TEST_CASE("T-262a: a run that fails before the rewrite pass keeps the PNG --compress replaced") {
    const std::string source = makeProject("compress_fail");
    {
        // A tiled border-image is not an atlas candidate, so the deferred compression turns it into
        // a DDS after the pages are built (as in the --atlas --compress bc3 case above).
        std::vector<uint8_t> tile(static_cast<size_t>(16) * 16 * 4);
        for (size_t i = 0; i < tile.size(); i += 4) {
            tile[i] = static_cast<uint8_t>(i % 251);
            tile[i + 1] = 90;
            tile[i + 2] = 30;
            tile[i + 3] = 255;
        }
        REQUIRE(sw::writePng((fs::path(source) / "hud" / "img" / "tile.png").string(), tile.data(), 16, 16,
                             16 * 4));
        std::string xt = readText(fs::path(source) / "hud" / "hud.xt");
        const size_t at = xt.find("</style>");
        REQUIRE(at != std::string::npos);
        xt.insert(at,
                  "#tiled { position: absolute; left: 100px; top: 100px; width: 48px; height: 32px; "
                  "border-image: url(\"img/tile.png\") 4 fill; border-image-repeat: repeat; }\n");
        const size_t body = xt.find("</body>");
        REQUIRE(body != std::string::npos);
        xt.insert(body, "<div id=\"tiled\"></div>\n");
        std::ofstream(fs::path(source) / "hud" / "hud.xt", std::ios::trunc) << xt;
    }
    const std::string out = outDir("compress_fail");
    // The subset font's write fails after the compression, so the UIs never reach pass 2 and their
    // `.xtir` files keep the pass-1 names.
    std::error_code ec;
    fs::create_directories(fs::path(out) / "fonts" / "NotoSans-Regular.subset.ttf", ec);
    REQUIRE(ec.value() == 0);
    PackOptions options;
    options.atlas = true;
    options.atlasScope = "bundle";
    options.compress = "bc3";
    options.fontSubset = true;
    const PackReport report = packWith(source, out, options);
    CHECK_FALSE(report.ok);
    CHECK(report.assetsCompressed >= 1);
    // Every file the packed hud.xtir names is still there, the tile's PNG included.
    std::unique_ptr<Bundle> bundle = openBundle(out);
    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, &bundle->assets()});
    Result<DocumentHandle> handle = loadUi(ctx, *bundle, "hud");
    REQUIRE(handle.ok());
    const IrView& view = ctx.document(handle.value())->ir();
    int files = 0;
    for (const IrAsset& asset : view.assets().assets()) {
        const std::string name(view.string(asset.name));
        if (asset.embedded != 0 || name.rfind("assets/", 0) != 0) {
            continue;
        }
        CHECK_MESSAGE(fs::exists(fs::path(out) / name), name);
        ++files;
    }
    CHECK(files >= 3);  // nine.png, a.png and tile.png (plus the font).
}

TEST_CASE("@2x variant: manifest variants.2x and imageView(dpr) selection through the bundle") {
    const std::string source = makeProject("variant");
    const std::string out = outDir("variant");
    const PackReport report = packWith(source, out, PackOptions{});
    REQUIRE(report.ok);
    CHECK(report.variants2x == 1);
    std::unique_ptr<Bundle> bundle = openBundle(out);
    const AssetInfo* nine = nullptr;
    for (const AssetInfo& info : bundle->assetTable()) {
        if (!info.variant2x.empty()) {
            nine = &info;
        }
    }
    REQUIRE(nine != nullptr);
    CHECK(nine->variant2x.find("@2x.png") != std::string::npos);
    CHECK(fs::exists(fs::path(out) / nine->variant2x));
    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, &bundle->assets()});
    Result<DocumentHandle> handle = loadUi(ctx, *bundle, "hud");
    REQUIRE(handle.ok());
    const IrView& view = ctx.document(handle.value())->ir();
    AssetId nineId = kInvalidAsset;
    for (uint32_t i = 0; i < view.assets().assets().size(); ++i) {
        if (view.string(view.assets().assets()[i].name) == nine->file) {
            nineId = i;
        }
    }
    REQUIRE(nineId != kInvalidAsset);
    CHECK(bundle->assets().imageView(nineId, view, 1.0f)->width == 24);
    CHECK(bundle->assets().imageView(nineId, view, 2.0f)->width == 48);
    CHECK(bundle->assets().imageSize(nineId, view).x == 24.0f);
}

TEST_CASE("DDS round trip: BC1, BC3 and BC7 decode back within tolerance") {
    const int w = 37;
    const int h = 21;
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* p =
                rgba.data() + (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
            p[0] = static_cast<uint8_t>(x * 6);
            p[1] = static_cast<uint8_t>(y * 11);
            p[2] = static_cast<uint8_t>(200 - x * 3);
            p[3] = static_cast<uint8_t>(x < 20 ? 255 : 128);
        }
    }
    for (BcFormat format : {BcFormat::Bc1, BcFormat::Bc3, BcFormat::Bc7}) {
        const std::vector<uint8_t> dds = encodeDds(rgba.data(), w, h, format, /*mipmaps=*/true);
        REQUIRE_FALSE(dds.empty());
        DdsInfo info;
        REQUIRE(parseDdsHeader(dds.data(), dds.size(), info));
        CHECK(info.width == w);
        CHECK(info.height == h);
        CHECK(info.mipCount == 6);
        ImageData decoded;
        REQUIRE(sw::decodeImageBytes(dds.data(), dds.size(), decoded));
        CHECK(decoded.width == w);
        CHECK(decoded.height == h);
        // BC1 keeps 1-bit alpha only (the half-transparent column becomes 0 or 255), so its
        // colour PSNR is measured on the opaque part; BC3 / BC7 keep the alpha.
        if (format == BcFormat::Bc1) {
            std::vector<uint8_t> a, b;
            for (size_t i = 0; i < rgba.size(); i += 4) {
                if (rgba[i + 3] == 255) {
                    a.insert(a.end(), rgba.begin() + static_cast<std::ptrdiff_t>(i),
                             rgba.begin() + static_cast<std::ptrdiff_t>(i) + 3);
                    b.insert(b.end(), decoded.rgba.begin() + static_cast<std::ptrdiff_t>(i),
                             decoded.rgba.begin() + static_cast<std::ptrdiff_t>(i) + 3);
                }
            }
            CHECK(psnr(a, b) > 30.0);
        } else {
            CHECK(psnr(rgba, decoded.rgba) > (format == BcFormat::Bc7 ? 40.0 : 32.0));
        }
    }
    BcFormat parsed = BcFormat::None;
    CHECK(parseBcFormat("BC7", parsed));
    CHECK(parsed == BcFormat::Bc7);
    CHECK_FALSE(parseBcFormat("etc2", parsed));
}

TEST_CASE("pack --compress bc7: DDS assets, manifest format, decodable through the bundle") {
    const std::string source = makeProject("compress");
    const std::string out = outDir("compress");
    PackOptions options;
    options.compress = "bc7";
    const PackReport report = packWith(source, out, options);
    for (const compile::Diagnostic& d : report.diagnostics) {
        MESSAGE(d.file, ": ", d.message);
    }
    REQUIRE(report.ok);
    CHECK(report.assetsCompressed >= 2);
    std::unique_ptr<Bundle> bundle = openBundle(out);
    int dds = 0;
    for (const AssetInfo& info : bundle->assetTable()) {
        if (info.kind != "image") {
            continue;
        }
        CHECK(info.format == "bc7");
        CHECK(info.file.size() > 4);
        CHECK(info.file.substr(info.file.size() - 4) == ".dds");
        CHECK(fs::exists(fs::path(out) / info.file));
        ++dds;
    }
    CHECK(dds == 2);
    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, &bundle->assets()});
    Result<DocumentHandle> handle = loadUi(ctx, *bundle, "hud");
    REQUIRE(handle.ok());
    const IrView& view = ctx.document(handle.value())->ir();
    bool sawCompressed = false;
    for (uint32_t i = 0; i < view.assets().assets().size(); ++i) {
        if (view.assets().assets()[i].kind != 0) {
            continue;
        }
        const CompressedImage* compressed = bundle->assets().compressedImage(i, view, 1.0f);
        REQUIRE(compressed != nullptr);
        CHECK(compressed->format == DdsFormat::Bc7);
        CHECK(compressed->blocks.size() ==
              ddsLevelSize(DdsFormat::Bc7, compressed->width, compressed->height));
        const ImageData* pixels = bundle->assets().imageView(i, view, 1.0f);
        REQUIRE(pixels != nullptr);
        CHECK(pixels->width == compressed->width);
        sawCompressed = true;
    }
    CHECK(sawCompressed);
    // The compressed bundle draws like the plain one (BC7 on flat colours is near-lossless).
    const std::string plainOut = outDir("compress_plain");
    REQUIRE(packWith(source, plainOut, PackOptions{}).ok);
    std::unique_ptr<Bundle> plain = openBundle(plainOut);
    CHECK(psnr(renderHud(*plain), renderHud(*bundle)) > 35.0);
    // Per-asset override and the bc1 -> bc3 alpha rule.
    PackOptions bc1;
    bc1.compress = "bc1";
    const std::string bc1Out = outDir("compress_bc1");
    REQUIRE(packWith(source, bc1Out, bc1).ok);
    std::unique_ptr<Bundle> bc1Bundle = openBundle(bc1Out);
    bool sawBc3 = false;
    for (const AssetInfo& info : bc1Bundle->assetTable()) {
        sawBc3 = sawBc3 || info.format == "bc3";  // nine.png has a translucent centre.
    }
    CHECK(sawBc3);
}

TEST_CASE("pack --atlas --compress bc3: one DDS page with entries, the rest compressed after packing") {
    // T-204: the atlas candidates were collected from names `--compress` had already turned into
    // `.dds`, so no page was built. Now the pages come first and compression runs on what is left.
    const std::string source = makeProject("atlas_compress");
    {
        // A tiled border-image is not an atlas candidate: it must still come out as a DDS.
        std::vector<uint8_t> tile(static_cast<size_t>(16) * 16 * 4);
        for (size_t i = 0; i < tile.size(); i += 4) {
            tile[i] = static_cast<uint8_t>(i % 251);
            tile[i + 1] = 90;
            tile[i + 2] = 30;
            tile[i + 3] = 255;
        }
        REQUIRE(sw::writePng((fs::path(source) / "hud" / "img" / "tile.png").string(), tile.data(), 16, 16,
                             16 * 4));
        std::string xt = readText(fs::path(source) / "hud" / "hud.xt");
        const size_t at = xt.find("</style>");
        REQUIRE(at != std::string::npos);
        xt.insert(at,
                  "#tiled { position: absolute; left: 100px; top: 100px; width: 48px; height: 32px; "
                  "border-image: url(\"img/tile.png\") 4 fill; border-image-repeat: repeat; }\n");
        const size_t body = xt.find("</body>");
        REQUIRE(body != std::string::npos);
        xt.insert(body, "<div id=\"tiled\"></div>\n");
        std::ofstream(fs::path(source) / "hud" / "hud.xt", std::ios::trunc) << xt;
    }
    const std::string out = outDir("atlas_compress");
    PackOptions options;
    options.atlas = true;
    options.atlasScope = "bundle";
    options.compress = "bc3";
    const PackReport report = packWith(source, out, options);
    for (const compile::Diagnostic& d : report.diagnostics) {
        MESSAGE(d.file, ": ", d.message);
    }
    REQUIRE(report.ok);
    CHECK(report.atlasPages == 1);
    CHECK(report.atlasEntries == 2);

    std::unique_ptr<Bundle> bundle = openBundle(out);
    REQUIRE(bundle->atlases().size() == 1);
    const AtlasInfo& page = bundle->atlases()[0];
    CHECK(page.file == "atlas/0.dds");
    CHECK(page.file2x == "atlas/0@2x.dds");
    CHECK(page.format == "bc3");
    CHECK(page.entries.size() == 2);
    CHECK(fs::exists(fs::path(out) / page.file));
    CHECK(fs::exists(fs::path(out) / page.file2x));
    int atlased = 0;
    int compressed = 0;
    for (const AssetInfo& info : bundle->assetTable()) {
        if (info.kind != "image") {
            continue;
        }
        if (!info.atlases.empty()) {
            ++atlased;
            CHECK_FALSE(fs::exists(fs::path(out) / info.file));
            continue;
        }
        ++compressed;
        CHECK(info.format == "bc3");
        CHECK(info.file.substr(info.file.size() - 4) == ".dds");
        CHECK(fs::exists(fs::path(out) / info.file));
    }
    CHECK(atlased == 2);
    CHECK(compressed == 1);  // tile.png
    // No stale `.png` is left under assets/ and the .xtir names the page cells and the DDS.
    for (const fs::directory_entry& entry : fs::directory_iterator(fs::path(out) / "assets")) {
        CHECK(entry.path().extension() != ".png");
    }
    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, &bundle->assets()});
    Result<DocumentHandle> handle = loadUi(ctx, *bundle, "hud");
    REQUIRE(handle.ok());
    const IrView& view = ctx.document(handle.value())->ir();
    int atlasNames = 0;
    int ddsNames = 0;
    for (const IrAsset& asset : view.assets().assets()) {
        const std::string name(view.string(asset.name));
        atlasNames += isAtlasAssetName(name) ? 1 : 0;
        ddsNames +=
            (!isAtlasAssetName(name) && name.size() > 4 && name.substr(name.size() - 4) == ".dds") ? 1 : 0;
    }
    CHECK(atlasNames == 2);
    CHECK(ddsNames == 1);
}

TEST_CASE("pack --font-subset: fonts/<stem>.subset.ttf keeps the used glyphs, lints the missing ones") {
    const std::string source = makeProject("subset");
    const std::string out = outDir("subset");
    PackOptions options;
    options.fontSubset = true;
    const PackReport report = packWith(source, out, options);
    REQUIRE(report.ok);
    CHECK(report.fontsSubset == 1);
    bool lint = false;
    for (const compile::Diagnostic& d : report.diagnostics) {
        lint = lint || d.message.find("U+4E2D") != std::string::npos;
    }
    CHECK(lint);
    std::unique_ptr<Bundle> bundle = openBundle(out);
    const AssetInfo* font = nullptr;
    for (const AssetInfo& info : bundle->assetTable()) {
        if (info.kind == "font") {
            font = &info;
        }
    }
    REQUIRE(font != nullptr);
    CHECK(font->file == "fonts/NotoSans-Regular.subset.ttf");
    const std::string subsetBytes = readText(fs::path(out) / font->file);
    const std::string originalBytes =
        readText(fs::path(source) / "shared" / "fonts" / "NotoSans-Regular.subset.ttf");
    REQUIRE_FALSE(subsetBytes.empty());
    CHECK(subsetBytes.size() < originalBytes.size());
    FontFile original;
    FontFile subset;
    REQUIRE(original.parse(std::vector<uint8_t>(originalBytes.begin(), originalBytes.end())));
    REQUIRE(subset.parse(std::vector<uint8_t>(subsetBytes.begin(), subsetBytes.end())));
    CHECK(subset.glyphCount() == original.glyphCount());  // Ids are stable.
    CHECK(subset.outlineCount() < original.outlineCount());
    for (uint32_t cp : {static_cast<uint32_t>('F'), static_cast<uint32_t>('i'), static_cast<uint32_t>('r'),
                        static_cast<uint32_t>('e'), static_cast<uint32_t>('0')}) {
        CHECK(original.hasGlyph(cp));
        CHECK(subset.hasGlyph(cp));
    }
    // A code point the original maps but no text uses is dropped from the subset's cmap.
    uint32_t dropped = 0;
    for (uint32_t cp = 0x0080; cp < 0xFFFF && dropped == 0; ++cp) {
        if (cp == 0x4E2D) {
            continue;
        }
        if (original.hasGlyph(cp)) {
            dropped = cp;
        }
    }
    REQUIRE(dropped != 0);
    CHECK_FALSE(subset.hasGlyph(dropped));
    // The UI still loads its font from the subset file.
    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, &bundle->assets()});
    Result<DocumentHandle> handle = loadUi(ctx, *bundle, "hud");
    REQUIRE(handle.ok());
    const IrView& view = ctx.document(handle.value())->ir();
    bool named = false;
    for (const IrAsset& asset : view.assets().assets()) {
        named = named || (asset.kind == 1 && view.string(asset.name) == "fonts/NotoSans-Regular.subset.ttf");
    }
    CHECK(named);
}

namespace {

std::string g_virtualRoot;  // The packed bundle a virtual "apk/bundle/" prefix maps onto.

/// T-240: xt_bundle's file reads answered from `g_virtualRoot`; any other path is unreadable.
bool virtualReader(const std::string& path, std::string& out, void* /*user*/) {
    const std::string prefix = "apk/bundle/";
    if (path.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    std::ifstream in(g_virtualRoot + "/" + path.substr(prefix.size()), std::ios::binary);
    if (!in.good()) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

}  // namespace

TEST_CASE("loadUi/reloadUi read the .xtir through the bundle file reader (T-240)") {
    const std::string source = makeProject("reader");
    g_virtualRoot = outDir("reader_out");
    REQUIRE(packWith(source, g_virtualRoot, PackOptions{}).ok);
    setBundleFileReader(&virtualReader);
    Result<std::unique_ptr<Bundle>> opened = Bundle::open("apk/bundle", nullptr, nullptr);
    REQUIRE(opened.ok());
    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, &opened.value()->assets()});
    // "apk/bundle/ui/hud.xtir" exists only through the reader, never on disk.
    Result<DocumentHandle> loaded = loadUi(ctx, *opened.value(), "hud");
    CHECK_MESSAGE(loaded.ok(), std::string(statusName(loaded.status())));
    if (loaded.ok()) {
        CHECK(reloadUi(ctx, *opened.value(), loaded.value(), "hud").ok());
    }
    setBundleFileReader(nullptr);
}
