#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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
#include "t687_project.h"
#include "test_temp.h"
#include "texture_compress.h"
#include "xt/asset_resolver.h"
#include "xt/bundle/bundle.h"
#include "xt/bundle/pack.h"
#include "xt/bundle/project_file.h"
#include "xt/bundle/ui_project.h"
#include "xt/command_list.h"
#include "xt/context.h"
#include "xt/dds.h"
#include "xt/document.h"
#include "xt/image_mips.h"
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
        // T-687 (10 §10.13 ruling 3): every packed DDS carries its full chain.
        CHECK(compressed->levels == imageMipLevelCount(compressed->width, compressed->height));
        size_t chain = 0;
        for (int k = 0; k < compressed->levels; ++k) {
            chain += ddsLevelSize(DdsFormat::Bc7, std::max(1, compressed->width >> k),
                                  std::max(1, compressed->height >> k));
        }
        CHECK(compressed->blocks.size() == chain);
        const ImageData* pixels = bundle->assets().imageView(i, view, 1.0f);
        REQUIRE(pixels != nullptr);
        CHECK(pixels->width == compressed->logicalWidth);
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

namespace {

/// T-686 (Docs/design/10-backends.md §10.13 rulings 2, 3, 7): the synthetic RGBA of the golden
/// fixture ui_image_bc7 at its logical size -- a 2 px checker of two colours carrying a horizontal and
/// a vertical gradient, cut by a disc whose edge ramps alpha over 4 px (transparent texels keep the
/// colour they would have).
std::vector<uint8_t> bc7FixturePattern(int w, int h) {
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    const double cx = w * 0.5, cy = h * 0.5, r = std::min(w, h) * 0.46;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* p =
                rgba.data() + (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
            const bool odd = ((x / 2) + (y / 2)) % 2 != 0;
            p[0] = odd ? 230 : 30;
            p[1] = static_cast<uint8_t>(odd ? 40 + x * 3 : 90);
            p[2] = static_cast<uint8_t>(odd ? 60 : 140 + y);
            const double d = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
            p[3] = static_cast<uint8_t>(std::clamp((r - d) * 64.0, 0.0, 255.0));
        }
    }
    return rgba;
}

/// The DDS file of a fixture: the pattern padded to a multiple of 4 by repeating the edge texels,
/// encoded as BC7 with its full chain (encodeDds), and -- when padded -- the logical-size marker
/// (`DDS_HEADER.dwReserved1[0..2]` = 'XTLS', logical width, logical height) written into the header.
std::vector<uint8_t> makeBc7Fixture(int w, int h) {
    const std::vector<uint8_t> src = bc7FixturePattern(w, h);
    const int pw = (w + 3) / 4 * 4, ph = (h + 3) / 4 * 4;
    std::vector<uint8_t> padded(static_cast<size_t>(pw) * static_cast<size_t>(ph) * 4);
    for (int y = 0; y < ph; ++y) {
        for (int x = 0; x < pw; ++x) {
            const size_t from = (static_cast<size_t>(std::min(y, h - 1)) * static_cast<size_t>(w) +
                                 static_cast<size_t>(std::min(x, w - 1))) *
                                4;
            std::copy_n(src.data() + from, 4,
                        padded.data() +
                            (static_cast<size_t>(y) * static_cast<size_t>(pw) + static_cast<size_t>(x)) * 4);
        }
    }
    std::vector<uint8_t> dds = encodeDds(padded.data(), pw, ph, BcFormat::Bc7, /*mipmaps=*/true);
    if (!dds.empty() && (pw != w || ph != h)) {
        const uint32_t marker[3] = {kDdsLogicalSizeTag, static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
        for (int i = 0; i < 3; ++i) {
            for (int b = 0; b < 4; ++b) {
                dds[static_cast<size_t>(4 + 28 + i * 4 + b)] = static_cast<uint8_t>(marker[i] >> (8 * b));
            }
        }
    }
    return dds;
}

/// XT_REGEN_BC7_FIXTURES=1 is set.
bool regenBc7Fixtures() {
    std::string value;
#ifdef _MSC_VER
    char* raw = nullptr;
    size_t length = 0;
    if (_dupenv_s(&raw, &length, "XT_REGEN_BC7_FIXTURES") == 0 && raw != nullptr) {
        value = raw;
        std::free(raw);
    }
#else
    if (const char* raw = std::getenv("XT_REGEN_BC7_FIXTURES")) {
        value = raw;
    }
#endif
    return value == "1";
}

}  // namespace

// T-686: how Tests/golden/ui/bc7_64.dds and bc7_66x50.dds (golden ui_image_bc7, the GL / gpu BC7 tests)
// were made, and a check that the committed files are still that. XT_REGEN_BC7_FIXTURES=1 writes them
// again (only the encoder's float arithmetic could make a fresh encode differ by a few code values).
TEST_CASE("T-686: the BC7 golden fixtures are encodeDds of the synthetic pattern, padded and marked") {
    struct Fixture {
        int w, h;
        const char* name;
    };
    for (const Fixture f : {Fixture{64, 64, "bc7_64.dds"}, Fixture{66, 50, "bc7_66x50.dds"}}) {
        CAPTURE(f.name);
        const std::vector<uint8_t> fresh = makeBc7Fixture(f.w, f.h);
        REQUIRE_FALSE(fresh.empty());
        const fs::path path = fs::path(std::string(XT_TEST_BUNDLE_DIR)) / ".." / "golden" / "ui" / f.name;
        if (regenBc7Fixtures()) {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(fresh.data()),
                      static_cast<std::streamsize>(fresh.size()));
            REQUIRE(out.good());
        }
        std::ifstream in(path, std::ios::binary);
        REQUIRE(in.good());
        const std::vector<uint8_t> committed((std::istreambuf_iterator<char>(in)),
                                             std::istreambuf_iterator<char>());
        DdsInfo info;
        REQUIRE(parseDdsHeader(committed.data(), committed.size(), info));
        CHECK(info.format == DdsFormat::Bc7);
        CHECK(info.width == (f.w + 3) / 4 * 4);
        CHECK(info.height == (f.h + 3) / 4 * 4);
        CHECK(info.logicalWidth == f.w);
        CHECK(info.logicalHeight == f.h);
        CHECK_FALSE(info.logicalRejected);
        CHECK(info.mipCount == imageMipLevelCount(info.width, info.height));
        CHECK(committed.size() == fresh.size());
        ImageData a, b;
        REQUIRE(sw::decodeImageBytes(committed.data(), committed.size(), a));
        REQUIRE(sw::decodeImageBytes(fresh.data(), fresh.size(), b));
        CHECK(a.width == f.w);  // cut to the logical size
        CHECK(a.height == f.h);
        CHECK(psnr(a.rgba, b.rgba) > 45.0);
        // What shows: premultiplied (BC7 spends little on the colour of transparent texels).
        std::vector<uint8_t> shown = a.rgba, source = bc7FixturePattern(f.w, f.h);
        premultiplyRgba8(shown.data(), shown.size() / 4, shown.data());
        premultiplyRgba8(source.data(), source.size() / 4, source.data());
        const double quality = psnr(shown, source);
        MESSAGE(f.name, ": premultiplied PSNR against the pattern ", quality, " dB");
        CHECK(quality > 30.0);
    }
}

// ---------------------------------------------------------------------------------------------------
// T-687 (Docs/design/10-backends.md §10.13 rulings 2-6): padding with the logical size, the mip chain,
// the alpha bleed, `auto`, threads and the encode cache.

namespace {

namespace t687 = xt::test::t687;

uint32_t le32At(const std::vector<uint8_t>& bytes, size_t at) {
    return static_cast<uint32_t>(bytes[at]) | (static_cast<uint32_t>(bytes[at + 1]) << 8) |
           (static_cast<uint32_t>(bytes[at + 2]) << 16) | (static_cast<uint32_t>(bytes[at + 3]) << 24);
}

const uint8_t* texel(const std::vector<uint8_t>& rgba, int w, int x, int y) {
    return rgba.data() + (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
}

/// A fresh T-687 project (Tests/bundle/t687_project.h) with `publishJson` as its `publish` object.
std::string t687Project(const std::string& name, const std::string& publishJson) {
    const fs::path dir = xt::test::tempDir() / ("xt_t687_src_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    REQUIRE(t687::writeProject(dir, publishJson));
    return dir.generic_string();
}

PackReport t687Pack(const std::string& source, const std::string& out, PackOptions options) {
    std::string error;
    Result<UiProject> project = UiProject::load(source, &error);
    REQUIRE_MESSAGE(project.ok(), error);
    options.outDir = out;
    options.thumbnails = false;
    PackReport report = pack(project.value(), options);
    for (const compile::Diagnostic& d : report.diagnostics) {
        MESSAGE(d.file, ": ", d.message);
    }
    return report;
}

/// Manifest `assets[]`: first source file name (e.g. "bg.png") -> format ("" = as it is).
std::map<std::string, std::string> formatsBySource(const std::string& out) {
    std::map<std::string, std::string> formats;
    std::unique_ptr<Bundle> bundle = openBundle(out);
    for (const AssetInfo& info : bundle->assetTable()) {
        if (info.kind == "image" && !info.sources.empty()) {
            formats[fs::path(info.sources[0]).filename().string()] = info.format;
        }
    }
    return formats;
}

/// The packed file of source `name` (by the manifest).
std::vector<uint8_t> packedFile(const std::string& out, const std::string& name) {
    std::unique_ptr<Bundle> bundle = openBundle(out);
    for (const AssetInfo& info : bundle->assetTable()) {
        if (!info.sources.empty() && fs::path(info.sources[0]).filename().string() == name) {
            const std::string text = readText(fs::path(out) / info.file);
            return std::vector<uint8_t>(text.begin(), text.end());
        }
    }
    FAIL("no packed file for " << name);
    return {};
}

/// Renders the bundle's UI `m` at 640 x 320 through the software backend.
std::vector<uint8_t> renderT687(Bundle& bundle) {
    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, &bundle.assets()});
    Result<DocumentHandle> handle = loadUi(ctx, bundle, "m");
    REQUIRE(handle.ok());
    ctx.setViewport(Rect{0, 0, 640, 320}, 1.0f);
    ctx.update(0.0f);
    std::vector<uint8_t> pixels(static_cast<size_t>(640) * 320 * 4, 0);
    sw::SwTarget target{pixels.data(), 640, 320, 640 * 4};
    CommandList list;
    ctx.render(list);
    sw::SwRenderer renderer;
    renderer.setTarget(target);
    renderer.clear(Color{0.5f, 0.5f, 0.5f, 1});
    renderer.render(list, &shaper, &bundle.assets(), &ctx.document(handle.value())->ir(), nullptr);
    return pixels;
}

/// What a pack writes that does not depend on the clock: every file under `assets/` and `atlas/`, and
/// `manifest.json` without its `builtAt` line (`ui/*.xtir` carry their compile time in META; the
/// manifest's `fileHash` leaves it out).
std::map<std::string, std::string> stableFiles(const std::string& dir) {
    std::map<std::string, std::string> files;
    for (auto& [name, bytes] : snapshot(dir)) {
        if (name.rfind("assets/", 0) == 0 || name.rfind("atlas/", 0) == 0) {
            files[name] = bytes;
        } else if (name == "manifest.json") {
            std::string kept;
            size_t start = 0;
            while (start < bytes.size()) {
                size_t end = bytes.find('\n', start);
                end = end == std::string::npos ? bytes.size() : end + 1;
                const std::string line = bytes.substr(start, end - start);
                if (line.find("\"builtAt\"") == std::string::npos) {
                    kept += line;
                }
                start = end;
            }
            files[name] = kept;
        }
    }
    return files;
}

/// One channel (0..3) of an RGBA buffer.
std::vector<uint8_t> channel(const std::vector<uint8_t>& rgba, size_t c) {
    std::vector<uint8_t> out;
    for (size_t i = c; i < rgba.size(); i += 4) {
        out.push_back(rgba[i]);
    }
    return out;
}

/// The RGB of an RGBA buffer.
std::vector<uint8_t> rgbOf(const std::vector<uint8_t>& rgba) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i < rgba.size(); i += 4) {
        out.insert(out.end(), rgba.begin() + static_cast<std::ptrdiff_t>(i),
                   rgba.begin() + static_cast<std::ptrdiff_t>(i) + 3);
    }
    return out;
}

}  // namespace

TEST_CASE("T-687: padding repeats the edge texels; the header has the padded size and the 'XTLS' marker") {
    // padToBlockMultiple: 5 x 3 -> 8 x 4, the last column and row repeated.
    std::vector<uint8_t> small(5 * 3 * 4);
    for (size_t i = 0; i < small.size(); ++i) {
        small[i] = static_cast<uint8_t>(i);
    }
    int pw = 0, ph = 0;
    const std::vector<uint8_t> padded = padToBlockMultiple(small.data(), 5, 3, pw, ph);
    REQUIRE(pw == 8);
    REQUIRE(ph == 4);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 8; ++x) {
            CAPTURE(x);
            CAPTURE(y);
            CHECK(std::equal(texel(padded, 8, x, y), texel(padded, 8, x, y) + 4,
                             texel(small, 5, std::min(x, 4), std::min(y, 2))));
        }
    }
    // A 37 x 21 image: stored 40 x 24 with the full chain, logical size in dwReserved1[0..2].
    const t687::Image img = t687::art(37, 21);
    const std::vector<uint8_t> dds = encodePublishDds(img.rgba.data(), 37, 21, BcFormat::Bc7, 1);
    DdsInfo info;
    REQUIRE(parseDdsHeader(dds.data(), dds.size(), info));
    CHECK(info.width == 40);
    CHECK(info.height == 24);
    CHECK(info.logicalWidth == 37);
    CHECK(info.logicalHeight == 21);
    CHECK_FALSE(info.logicalRejected);
    CHECK(info.mipCount == imageMipLevelCount(40, 24));
    CHECK(le32At(dds, 4 + 28) == kDdsLogicalSizeTag);
    CHECK(le32At(dds, 4 + 32) == 37u);
    CHECK(le32At(dds, 4 + 36) == 21u);
    size_t chain = 0;
    for (int k = 0; k < info.mipCount; ++k) {
        chain += ddsLevelSize(DdsFormat::Bc7, std::max(1, 40 >> k), std::max(1, 24 >> k));
    }
    CHECK(dds.size() == 148 + chain);
    ImageData decoded;
    REQUIRE(sw::decodeImageBytes(dds.data(), dds.size(), decoded));
    CHECK(decoded.width == 37);  // cut back to the logical size
    CHECK(decoded.height == 21);
    // Already a multiple of 4: no marker, and the reserved words stay zero.
    const t687::Image aligned = t687::art(32, 16);
    const std::vector<uint8_t> plain = encodePublishDds(aligned.rgba.data(), 32, 16, BcFormat::Bc7, 1);
    REQUIRE(parseDdsHeader(plain.data(), plain.size(), info));
    CHECK(info.width == 32);
    CHECK(info.logicalWidth == 32);
    CHECK(info.mipCount == imageMipLevelCount(32, 16));
    for (size_t at = 4 + 28; at < 4 + 72; at += 4) {
        CHECK(le32At(plain, at) == 0u);
    }
    // BC1 / BC3 (assets.yaml overrides) go through the same path.
    const std::vector<uint8_t> bc3 = encodePublishDds(img.rgba.data(), 37, 21, BcFormat::Bc3, 1);
    REQUIRE(parseDdsHeader(bc3.data(), bc3.size(), info));
    CHECK(info.format == DdsFormat::Bc3);
    CHECK(info.logicalWidth == 37);
    CHECK(info.mipCount == imageMipLevelCount(40, 24));
}

TEST_CASE("T-687: a mip level is the alpha-weighted 2x2 mean, rounded with ties to even (hand-computed)") {
    auto level1 = [](std::vector<uint8_t> src, int w, int h, int ew, int eh) {
        int ow = 0, oh = 0;
        std::vector<uint8_t> out = downsampleMipLevel(src.data(), w, h, ow, oh);
        CHECK(ow == ew);
        CHECK(oh == eh);
        return out;
    };
    // Colour weighted by alpha, alpha the plain mean: sum(a) = 384.
    //   r = (10*255 + 250*0 + 0*128 + 1*1) / 384 = 2551/384 = 6.64 -> 7
    //   g = (20*255 + 1) / 384 = 5101/384 = 13.28 -> 13
    //   b = (30*255 + 200*128 + 1) / 384 = 33251/384 = 86.59 -> 87;  a = 384/4 = 96
    std::vector<uint8_t> out =
        level1({10, 20, 30, 255, 250, 0, 0, 0, 0, 0, 200, 128, 1, 1, 1, 1}, 2, 2, 1, 1);
    CHECK(out == std::vector<uint8_t>{7, 13, 87, 96});
    // Ties to even: alpha 2/4 = 0.5 -> 0, 6/4 = 1.5 -> 2; colour (1*1 + 2*1)/2 = 1.5 -> 2,
    // (2*1 + 3*1)/2 = 2.5 -> 2.
    out = level1({1, 2, 0, 1, 2, 3, 0, 1, 9, 9, 9, 0, 9, 9, 9, 0}, 2, 2, 1, 1);
    CHECK(out == std::vector<uint8_t>{2, 2, 0, 0});
    out = level1({0, 0, 0, 3, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0}, 2, 2, 1, 1);
    CHECK(out[3] == 2);
    // sum(a) = 0: the plain mean of the colours, (1+2+3+4)/4 = 2.5 -> 2, (4+4+4+6)/4 = 4.5 -> 4.
    out = level1({1, 4, 0, 0, 2, 4, 0, 0, 3, 4, 0, 0, 4, 6, 0, 0}, 2, 2, 1, 1);
    CHECK(out == std::vector<uint8_t>{2, 4, 0, 0});
    // Hardware sizes: an odd last column is not sampled, a 1-texel axis takes its texel twice.
    out = level1({100, 0, 0, 255, 50, 0, 0, 255, 7, 7, 7, 255}, 3, 1, 1, 1);
    CHECK(out == std::vector<uint8_t>{75, 0, 0, 255});
    // Opaque: the same as T-665's premultiplied box (xt::downsampleBox) exactly.
    const t687::Image bg = t687::background(37, 23);
    int ow = 0, oh = 0;
    const std::vector<uint8_t> mine = downsampleMipLevel(bg.rgba.data(), 37, 23, ow, oh);
    std::vector<uint8_t> box(static_cast<size_t>(ow) * static_cast<size_t>(oh) * 4);
    downsampleBox(bg.rgba.data(), 37, 23, box.data());
    CHECK(mine == box);
    // The stored chain of a flat colour decodes to that colour at every level.
    const t687::Image flat = t687::flat(20, 12, 30, 140, 220, 255);
    const std::vector<uint8_t> dds = encodePublishDds(flat.rgba.data(), 20, 12, BcFormat::Bc7, 1);
    DdsInfo info;
    REQUIRE(parseDdsHeader(dds.data(), dds.size(), info));
    REQUIRE(info.mipCount == imageMipLevelCount(20, 12));
    size_t at = info.dataOffset;
    for (int k = 0; k < info.mipCount; ++k) {
        const int lw = std::max(1, 20 >> k), lh = std::max(1, 12 >> k);
        const size_t size = ddsLevelSize(DdsFormat::Bc7, lw, lh);
        const std::vector<uint8_t> one = makeDdsLevelFile(DdsFormat::Bc7, lw, lh, dds.data() + at, size);
        ImageData level;
        REQUIRE(sw::decodeImageBytes(one.data(), one.size(), level));
        CAPTURE(k);
        CHECK(level.width == lw);
        CHECK(std::abs(static_cast<int>(level.rgba[0]) - 30) <= 1);
        CHECK(std::abs(static_cast<int>(level.rgba[1]) - 140) <= 1);
        CHECK(std::abs(static_cast<int>(level.rgba[2]) - 220) <= 1);
        CHECK(level.rgba[3] >= 254);  // bc7enc's mode 6 stores this opaque block as 254
        at += size;
    }
}

TEST_CASE("T-687: the bleed colours transparent texels from their nearest visible neighbours") {
    // One source: every transparent texel takes its colour; alpha is untouched.
    std::vector<uint8_t> row = {10, 20, 30, 255, 0, 255, 0, 0, 0, 255, 0, 0};
    bleedTransparentTexels(row.data(), 3, 1);
    CHECK(row == std::vector<uint8_t>{10, 20, 30, 255, 10, 20, 30, 0, 10, 20, 30, 0});
    // Two sources at the same distance: their mean (ties to even: (10+21)/2 = 15.5 -> 16).
    row = {10, 0, 0, 255, 0, 255, 0, 0, 21, 0, 0, 7};
    bleedTransparentTexels(row.data(), 3, 1);
    CHECK(row == std::vector<uint8_t>{10, 0, 0, 255, 16, 0, 0, 0, 21, 0, 0, 7});
    // Nothing visible, or nothing transparent: unchanged.
    std::vector<uint8_t> none = {1, 2, 3, 0, 4, 5, 6, 0};
    bleedTransparentTexels(none.data(), 2, 1);
    CHECK(none == std::vector<uint8_t>{1, 2, 3, 0, 4, 5, 6, 0});
    // Far texels are reached too (ring by ring, whole image).
    std::vector<uint8_t> wide(64 * 4, 0);
    wide[0] = 99, wide[3] = 255;
    bleedTransparentTexels(wide.data(), 64, 1);
    CHECK(wide[63 * 4] == 99);

    // Straight bilinear at an alpha edge: left half (200, 40, 40) opaque, right half transparent green.
    // Halfway between the last opaque and the first transparent texel the premultiplied result must be
    // (200, 40, 40) * 0.5 -- with the bleed; without it the green comes in.
    constexpr int kW = 16, kH = 8;
    t687::Image edge = t687::flat(kW, kH, 200, 40, 40, 255);
    for (int y = 0; y < kH; ++y) {
        for (int x = kW / 2; x < kW; ++x) {
            uint8_t* p = edge.rgba.data() + (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4;
            p[0] = 0, p[1] = 255, p[2] = 0, p[3] = 0;
        }
    }
    auto edgeError = [&](const std::vector<uint8_t>& dds) {
        ImageData d;
        REQUIRE(sw::decodeImageBytes(dds.data(), dds.size(), d));
        int worst = 0;
        for (int y = 0; y < kH; ++y) {
            const uint8_t* a = texel(d.rgba, kW, kW / 2 - 1, y);
            const uint8_t* b = texel(d.rgba, kW, kW / 2, y);
            const double alpha = (a[3] + b[3]) / 2.0 / 255.0;
            const double expect[3] = {200 * 0.5, 40 * 0.5, 40 * 0.5};
            for (int c = 0; c < 3; ++c) {
                const double shown = (a[c] + b[c]) / 2.0 * alpha;  // straight lerp, then premultiply
                worst = std::max(worst, static_cast<int>(std::lround(std::fabs(shown - expect[c]))));
            }
        }
        return worst;
    };
    const int bled = edgeError(encodePublishDds(edge.rgba.data(), kW, kH, BcFormat::Bc7, 1));
    const int raw = edgeError(encodeDds(edge.rgba.data(), kW, kH, BcFormat::Bc7, false));
    MESSAGE("edge colour error: bled ", bled, ", without the bleed ", raw);
    CHECK(bled <= 4);
    CHECK(raw > 40);
}

TEST_CASE("T-687: auto compresses an image only when both sides are >= 256 px; the CLI value wins") {
    const std::string source = t687Project("auto", R"({ "images": { "compress": "auto" } })");
    const std::string out = outDir("t687_auto");
    PackOptions options;
    options.encodeCache = false;
    const PackReport report = t687Pack(source, out, options);
    REQUIRE(report.ok);
    CHECK(report.compress == "auto");
    std::map<std::string, std::string> formats = formatsBySource(out);
    CHECK(formats["bg.png"] == "bc7");   // 512 x 288
    CHECK(formats["art.png"] == "bc7");  // 302 x 300
    CHECK(formats["wide.png"] == "");    // 300 x 255
    CHECK(formats["icon.png"] == "");    // 64 x 64
    CHECK(report.assetsCompressed == 2);
    CHECK(report.imagesEncoded == 2);
    // `--compress` overrides the project: none packs everything as it is, bc7 compresses everything.
    options.compress = "none";
    const std::string noneOut = outDir("t687_auto_none");
    REQUIRE(t687Pack(source, noneOut, options).ok);
    for (const auto& [name, format] : formatsBySource(noneOut)) {
        CAPTURE(name);
        CHECK(format.empty());
    }
    options.compress = "bc7";
    const std::string bc7Out = outDir("t687_auto_bc7");
    REQUIRE(t687Pack(source, bc7Out, options).ok);
    for (const auto& [name, format] : formatsBySource(bc7Out)) {
        CAPTURE(name);
        CHECK(format == "bc7");
    }
    // An atlas page follows the rule by its own size.
    options.compress = "auto";
    options.atlas = true;
    options.atlasScope = "bundle";
    const std::string atlasOut = outDir("t687_auto_atlas");
    const PackReport atlas = t687Pack(source, atlasOut, options);
    REQUIRE(atlas.ok);
    std::unique_ptr<Bundle> bundle = openBundle(atlasOut);
    REQUIRE_FALSE(bundle->atlases().empty());
    for (const AtlasInfo& page : bundle->atlases()) {
        CAPTURE(page.name);
        const bool big = page.width >= kAutoCompressMinSide && page.height >= kAutoCompressMinSide;
        CHECK(page.format == (big ? "bc7" : ""));
    }
}

TEST_CASE("T-687: publish.images.compress none or absent packs the same bytes; bc7 vs none quality") {
    // Absent, "none" and `--compress none` give the same bundle, without any DDS or cache.
    const std::string absent = t687Project("absent", "");
    const std::string none = t687Project("none", R"({ "images": { "compress": "none" } })");
    const std::string a = outDir("t687_absent");
    const std::string b = outDir("t687_none");
    const std::string c = outDir("t687_cli_none");
    const PackReport ra = t687Pack(absent, a, PackOptions{});
    REQUIRE(ra.ok);
    CHECK(ra.compress == "none");
    CHECK(ra.imagesEncoded == 0);
    CHECK(ra.encodeCacheDir.empty());
    REQUIRE(t687Pack(none, b, PackOptions{}).ok);
    PackOptions cliNone;
    cliNone.compress = "none";
    REQUIRE(t687Pack(absent, c, cliNone).ok);
    CHECK(stableFiles(a) == stableFiles(b));
    CHECK(stableFiles(a) == stableFiles(c));
    for (const auto& [file, bytes] : stableFiles(a)) {
        CHECK(file.find(".dds") == std::string::npos);
    }

    // bc7 against none (the thresholds of the T-687 sheet: backgrounds >= 38 dB RGB, a large image
    // with alpha >= 40 dB alpha; Sherefox measured 43-49 dB and 42-46.5 dB in the review of 2026-11-12).
    const std::string bc7 = t687Project("bc7", R"({ "images": { "compress": "bc7" } })");
    const std::string bc7Out = outDir("t687_bc7");
    PackOptions noCache;
    noCache.encodeCache = false;
    REQUIRE(t687Pack(bc7, bc7Out, noCache).ok);
    for (const auto& [name, format] : formatsBySource(bc7Out)) {
        CAPTURE(name);
        CHECK(format == "bc7");
    }
    ImageData bgDecoded, artDecoded;
    const std::vector<uint8_t> bgDds = packedFile(bc7Out, "bg.png");
    const std::vector<uint8_t> artDds = packedFile(bc7Out, "art.png");
    REQUIRE(sw::decodeImageBytes(bgDds.data(), bgDds.size(), bgDecoded));
    REQUIRE(sw::decodeImageBytes(artDds.data(), artDds.size(), artDecoded));
    const t687::Image bgSource = t687::background();
    const t687::Image artSource = t687::art();
    REQUIRE(artDecoded.width == 302);  // logical size, the file is 304 wide
    const double bgPsnr = psnr(rgbOf(bgSource.rgba), rgbOf(bgDecoded.rgba));
    const double artAlphaPsnr = psnr(channel(artSource.rgba, 3), channel(artDecoded.rgba, 3));
    std::unique_ptr<Bundle> plainBundle = openBundle(a);
    std::unique_ptr<Bundle> bc7Bundle = openBundle(bc7Out);
    const double framePsnr = psnr(rgbOf(renderT687(*plainBundle)), rgbOf(renderT687(*bc7Bundle)));
    MESSAGE("bc7 vs none: background RGB ", bgPsnr, " dB, art alpha ", artAlphaPsnr, " dB, SW frame RGB ",
            framePsnr, " dB");
    CHECK(bgPsnr >= 38.0);
    CHECK(artAlphaPsnr >= 40.0);
    CHECK(framePsnr >= 38.0);
}

TEST_CASE("T-687: the encode cache -- hits, a changed image misses, damaged or foreign entries miss") {
    const fs::path cacheRoot = xt::test::tempDir() / "xt_t687_cache_unit";
    std::error_code ec;
    fs::remove_all(cacheRoot, ec);
    EncodeCache cache(cacheRoot.generic_string());
    const t687::Image img = t687::art(40, 20);
    const std::string key = publishEncodeKey(img.rgba.data(), 40, 20, BcFormat::Bc7);
    const std::vector<uint8_t> dds = encodePublishDds(img.rgba.data(), 40, 20, BcFormat::Bc7, 1);
    std::vector<uint8_t> got;
    CHECK_FALSE(cache.load(key, got));
    REQUIRE(cache.store(key, dds));
    REQUIRE(cache.load(key, got));
    CHECK(got == dds);
    // The key names the format, the pixels and the encoder version.
    t687::Image changed = img;
    changed.rgba[5] ^= 1;
    CHECK(publishEncodeKey(changed.rgba.data(), 40, 20, BcFormat::Bc7) != key);
    CHECK(publishEncodeKey(img.rgba.data(), 40, 20, BcFormat::Bc3) != key);
    CHECK(key.find(kPublishEncoderVersion) != std::string::npos);
    // Damage the entry in four ways: each is a miss, and a store repairs it.
    const std::string file = cache.fileFor(key);
    const std::string good = readText(fs::path(file));
    auto damaged = [&](const std::string& bytes) {
        std::ofstream(fs::path(file), std::ios::binary | std::ios::trunc) << bytes;
        std::vector<uint8_t> out;
        return !cache.load(key, out);
    };
    std::string payloadFlip = good;
    payloadFlip.back() = static_cast<char>(payloadFlip.back() ^ 0x5A);
    CHECK(damaged(payloadFlip));
    std::string otherVersion = good;
    otherVersion[4] = 2;
    CHECK(damaged(otherVersion));
    CHECK(damaged(good.substr(0, good.size() / 2)));
    std::string otherKey = good;
    otherKey[12] = static_cast<char>(otherKey[12] ^ 1);  // first key byte
    CHECK(damaged(otherKey));
    REQUIRE(cache.store(key, dds));
    REQUIRE(cache.load(key, got));
    CHECK(got == dds);

    // Through pack(): cold, all hits, one image changed, `--no-cache`; the same bytes each time.
    const std::string source = t687Project("cache", R"({ "images": { "compress": "auto" } })");
    const std::string out = outDir("t687_cache");
    PackOptions options;
    options.encodeCacheDir = (xt::test::tempDir() / "xt_t687_cache_pack").generic_string();
    fs::remove_all(options.encodeCacheDir, ec);
    const PackReport cold = t687Pack(source, out, options);
    REQUIRE(cold.ok);
    CHECK(cold.imagesEncoded == 2);
    CHECK(cold.encodeCacheHits == 0);
    CHECK(cold.encodeCacheDir == fs::path(options.encodeCacheDir).string());
    const std::map<std::string, std::string> first = stableFiles(out);
    const PackReport warm = t687Pack(source, out, options);
    REQUIRE(warm.ok);
    CHECK(warm.encodeCacheHits == 2);
    CHECK(stableFiles(out) == first);
    REQUIRE(t687::writeImage(fs::path(source) / "m" / "img" / "art.png", t687::art(302, 296)));
    const PackReport oneChanged = t687Pack(source, out, options);
    REQUIRE(oneChanged.ok);
    CHECK(oneChanged.imagesEncoded == 2);
    CHECK(oneChanged.encodeCacheHits == 1);
    const std::string fresh = outDir("t687_cache_off");
    PackOptions off;
    off.encodeCache = false;
    const PackReport noCache = t687Pack(source, fresh, off);
    REQUIRE(noCache.ok);
    CHECK(noCache.encodeCacheHits == 0);
    CHECK(noCache.encodeCacheDir.empty());
    std::map<std::string, std::string> withCache = stableFiles(out);
    std::map<std::string, std::string> withoutCache = stableFiles(fresh);
    // Only the asset files are compared (the second pack of `out` left the first art DDS behind).
    for (const auto& [name, bytes] : withoutCache) {
        CAPTURE(name);
        CHECK(withCache[name] == bytes);
    }
    // The default directory is `<bundle>/../.xtframe-cache/bc/`.
    const std::string defaultOut = outDir("t687_cache_default");
    const PackReport byDefault = t687Pack(source, defaultOut, PackOptions{});
    REQUIRE(byDefault.ok);
    CHECK(fs::path(byDefault.encodeCacheDir) ==
          (fs::absolute(fs::path(defaultOut)).parent_path() / ".xtframe-cache" / "bc").lexically_normal());
    CHECK(fs::is_directory(fs::path(byDefault.encodeCacheDir)));
}

TEST_CASE("T-687: multi-threaded, single-threaded and cached encodes are the same bytes") {
    const t687::Image art = t687::art();
    const std::vector<uint8_t> one = encodePublishDds(art.rgba.data(), art.w, art.h, BcFormat::Bc7, 1);
    const std::vector<uint8_t> many = encodePublishDds(art.rgba.data(), art.w, art.h, BcFormat::Bc7, 7);
    CHECK(one == many);
    const std::string source = t687Project("threads", R"({ "images": { "compress": "bc7" } })");
    PackOptions single;
    single.encodeThreads = 1;
    single.encodeCache = false;
    const std::string singleOut = outDir("t687_threads_1");
    const PackReport singleReport = t687Pack(source, singleOut, single);
    REQUIRE(singleReport.ok);
    CHECK(singleReport.encodeThreads == 1);
    PackOptions threaded;
    threaded.encodeThreads = 8;
    threaded.encodeCacheDir = (xt::test::tempDir() / "xt_t687_cache_threads").generic_string();
    std::error_code ec;
    fs::remove_all(threaded.encodeCacheDir, ec);
    const std::string coldOut = outDir("t687_threads_8");
    const PackReport cold = t687Pack(source, coldOut, threaded);
    REQUIRE(cold.ok);
    CHECK(cold.encodeThreads == 8);
    const std::string warmOut = outDir("t687_threads_cached");
    const PackReport warm = t687Pack(source, warmOut, threaded);
    REQUIRE(warm.ok);
    CHECK(warm.encodeCacheHits == warm.imagesEncoded);
    CHECK(stableFiles(singleOut) == stableFiles(coldOut));
    CHECK(stableFiles(singleOut) == stableFiles(warmOut));
}
