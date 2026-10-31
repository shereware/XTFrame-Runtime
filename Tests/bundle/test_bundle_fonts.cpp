// T-555 (Docs/design/22-bundle.md §22.10.3, 50 §50.8 T-507): bundle::addBundleFonts() registers a
// UI's bundled font assets with a shaper -- the runtime half of `xtframe-pack --fonts bundled`.

#include <filesystem>
#include <fstream>
#include <iterator>
#include <json.hpp>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "test_temp.h"
#include "xt/bundle/bundle.h"
#include "xt/bundle/pack.h"
#include "xt/bundle/ui_project.h"
#include "xt/bundle/zip_archive.h"
#include "xt/command_list.h"
#include "xt/compile/utf8_path.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/sw/sw_png.h"
#include "xt/sw/sw_renderer.h"
#include "xt/text_shaper.h"
#ifdef XT_TEST_HAS_TEXT
#include "xt/text/ft_text_shaper.h"
#endif

using namespace xt;
using namespace xt::bundle;
namespace fs = std::filesystem;

#ifdef XT_TEST_HAS_TEXT

namespace {

const fs::path kRepoT555 = fs::path(XT_TEST_BUNDLE_DIR) / ".." / "..";

std::string readTextT555(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// The corpus project `Tests/corpus/game-ui` packed as `Tools/Scripts/corpus_check.py` packs it
/// (`--strict --fonts bundled`, the standard fonts and symbols of `Assets/`), once per run; returns
/// the bundle directory.
const std::string& corpusBundleT555() {
    static const std::string out = [] {
        const nlohmann::json spec =
            nlohmann::json::parse(readTextT555(kRepoT555 / "Tests" / "corpus" / "game-ui" / "project.json"));
        const fs::path work = xt::test::tempDir() / "xt_t555_game_ui";
        std::error_code ec;
        fs::remove_all(work, ec);
        fs::create_directories(work, ec);
        // The pack may write into its project (standard symbols, measured asset fields): pack a copy.
        fs::copy(kRepoT555 / compile::pathFromUtf8(spec["source"].get<std::string>()), work / "source",
                 fs::copy_options::recursive, ec);
        REQUIRE(ec.value() == 0);
        std::string error;
        Result<UiProject> project = UiProject::load(compile::genericPathToUtf8(work / "source"), &error);
        REQUIRE_MESSAGE(project.ok(), error);
        PackOptions options;
        options.outDir = compile::genericPathToUtf8(work / "bundle");
        options.strict = true;
        options.thumbnails = false;
        options.fontsMode = FontsMode::kBundled;
        options.standardFontDir = compile::genericPathToUtf8(kRepoT555 / "Assets" / "Fonts");
        options.standardSymbolDir = compile::genericPathToUtf8(kRepoT555 / "Assets" / "Symbols");
        const PackReport report = pack(project.value(), options);
        std::string diagnostics;
        for (const compile::Diagnostic& d : report.diagnostics) {
            diagnostics += d.file + ": " + d.message + "\n";
        }
        REQUIRE_MESSAGE(report.ok, diagnostics);
        return options.outDir;
    }();
    return out;
}

std::unique_ptr<Bundle> openT555(const std::string& dir) {
    std::string error;
    Result<std::unique_ptr<Bundle>> opened = Bundle::open(dir, sw::decodeImageBytes, &error);
    REQUIRE_MESSAGE(opened.ok(), error);
    return std::move(opened.value());
}

/// How many of UI `uiId`'s manifest assets are fonts.
size_t uiFontAssetsT555(const Bundle& bundle, std::string_view uiId) {
    const UiInfo* ui = bundle.ui(uiId);
    REQUIRE(ui != nullptr);
    size_t n = 0;
    for (const std::string& file : ui->assets) {
        for (const AssetInfo& asset : bundle.assetTable()) {
            n += (asset.file == file && asset.kind == "font") ? 1u : 0u;
        }
    }
    return n;
}

size_t fontCountT555(const TextShaper& shaper) {
    std::vector<FontDesc> fonts;
    shaper.fonts(fonts);
    return fonts.size();
}

/// One rendered frame (frame 0, 1280 x 720) of a loaded Document, plus the family of every glyph
/// run's font.
struct FrameT555 {
    std::vector<uint8_t> pixels;
    std::vector<std::string> runFamilies;
};

FrameT555 renderT555(Context& ctx, DocumentHandle handle, TextShaper& shaper, AssetResolver& assets) {
    constexpr int kW = 1280;
    constexpr int kH = 720;
    ctx.setViewport(Rect{0, 0, kW, kH}, 1.0f);
    ctx.update(0.0f);
    CommandList list;
    ctx.render(list);
    FrameT555 frame;
    std::vector<FontDesc> fonts;
    shaper.fonts(fonts);
    for (const Command& command : list.commands) {
        if (command.kind != CmdKind::DrawGlyphs) {
            continue;
        }
        const GlyphsCmd& run = list.glyphRuns[command.index];
        std::string family = "<unknown font>";
        for (const FontDesc& desc : fonts) {
            if (desc.id == run.font) {
                family = desc.family;
            }
        }
        frame.runFamilies.push_back(family);
    }
    frame.pixels.assign(static_cast<size_t>(kW) * kH * 4, 0);
    sw::SwTarget target{frame.pixels.data(), kW, kH, kW * 4};
    sw::SwRenderer renderer;
    renderer.setTarget(target);
    renderer.clear(Color{1, 1, 1, 1});
    renderer.render(list, &shaper, &assets, &ctx.document(handle)->ir(), nullptr);
    return frame;
}

/// Renders `uiId` with a shaper that has only the bundle's fonts (no system font at all).
FrameT555 renderBundleOnlyT555(Bundle& bundle, std::string_view uiId) {
    text::FtTextShaper shaper;
    Result<uint32_t> added = addBundleFonts(bundle, uiId, shaper);
    REQUIRE(added.ok());
    CHECK(added.value() == uiFontAssetsT555(bundle, uiId));
    Context ctx(ContextConfig{&shaper, &bundle.assets()});
    Result<DocumentHandle> handle = loadUi(ctx, bundle, uiId);
    REQUIRE(handle.ok());
    CHECK(fontCountT555(shaper) == uiFontAssetsT555(bundle, uiId));  // loadUi() registered none again.
    return renderT555(ctx, handle.value(), shaper, bundle.assets());
}

}  // namespace

TEST_CASE("addBundleFonts: a shaper without system fonts draws the bundled Noto like a system Noto (T-555)") {
    std::unique_ptr<Bundle> bundle = openT555(corpusBundleT555());
    for (const char* uiId : {"hud", "pause"}) {
        CAPTURE(uiId);
        REQUIRE(uiFontAssetsT555(*bundle, uiId) == 4);  // Noto Sans Regular / Bold / Italic / BoldItalic.
        const FrameT555 bundled = renderBundleOnlyT555(*bundle, uiId);
        REQUIRE_FALSE(bundled.runFamilies.empty());
        for (const std::string& family : bundled.runFamilies) {
            CHECK(family == "Noto Sans");
        }

        // The same `.xtir` with "Noto Sans" registered the way system fonts are (a font directory,
        // Assets/Fonts) and no bundle font: loaded through Context::loadDocument, not loadUi().
        text::FtTextShaper system;
        REQUIRE(system.addFontDir(compile::genericPathToUtf8(kRepoT555 / "Assets" / "Fonts"), 100) > 0);
        const std::string ir = readTextT555(fs::path(compile::pathFromUtf8(std::string(bundle->dir()))) /
                                            compile::pathFromUtf8(bundle->ui(uiId)->file));
        REQUIRE_FALSE(ir.empty());
        Context ctx(ContextConfig{&system, &bundle->assets()});
        Result<DocumentHandle> handle =
            ctx.loadDocument(IrBlob{reinterpret_cast<const uint8_t*>(ir.data()), ir.size()}, true);
        REQUIRE(handle.ok());
        const FrameT555 fromSystem = renderT555(ctx, handle.value(), system, bundle->assets());
        CHECK(fromSystem.runFamilies == bundled.runFamilies);
        CHECK(fromSystem.pixels == bundled.pixels);
    }
}

TEST_CASE("addBundleFonts: loading and reloading again registers nothing twice (T-555)") {
    std::unique_ptr<Bundle> bundle = openT555(corpusBundleT555());
    text::FtTextShaper shaper;
    Result<uint32_t> first = addBundleFonts(*bundle, "hud", shaper);
    REQUIRE(first.ok());
    CHECK(first.value() == 4);
    CHECK(fontCountT555(shaper) == 4);

    Result<uint32_t> again = addBundleFonts(*bundle, "hud", shaper);
    REQUIRE(again.ok());
    CHECK(again.value() == 0);
    // "pause" names the same four files.
    Result<uint32_t> pause = addBundleFonts(*bundle, "pause", shaper);
    REQUIRE(pause.ok());
    CHECK(pause.value() == 0);

    Context ctx(ContextConfig{&shaper, &bundle->assets()});
    Result<DocumentHandle> handle = loadUi(ctx, *bundle, "hud");
    REQUIRE(handle.ok());
    CHECK(reloadUi(ctx, *bundle, handle.value(), "hud").ok());
    (void)bundle->reload();
    Result<uint32_t> afterReload = addBundleFonts(*bundle, "hud", shaper);
    REQUIRE(afterReload.ok());
    CHECK(afterReload.value() == 0);
    CHECK(fontCountT555(shaper) == 4);

    // Another shaper has its own record.
    text::FtTextShaper other;
    Result<uint32_t> fresh = addBundleFonts(*bundle, "hud", other);
    REQUIRE(fresh.ok());
    CHECK(fresh.value() == 4);

    CHECK(addBundleFonts(*bundle, "no-such-ui", shaper).status() == Status::kNotFound);
}

TEST_CASE("addBundleFonts: an .xtb container is read from its unpacked copy (T-555)") {
    const fs::path xtb = xt::test::tempDir() / "xt_t555_game_ui.xtb";
    std::error_code ec;
    fs::remove(xtb, ec);
    std::string error;
    REQUIRE_MESSAGE(zipDirectory(compile::pathFromUtf8(corpusBundleT555()), xtb, {}, &error) == Status::kOk,
                    error);
    std::unique_ptr<Bundle> bundle = openT555(compile::genericPathToUtf8(xtb));
    const FrameT555 fromXtb = renderBundleOnlyT555(*bundle, "hud");
    REQUIRE_FALSE(fromXtb.runFamilies.empty());
    for (const std::string& family : fromXtb.runFamilies) {
        CHECK(family == "Noto Sans");
    }
    std::unique_ptr<Bundle> directory = openT555(corpusBundleT555());
    CHECK(renderBundleOnlyT555(*directory, "hud").pixels == fromXtb.pixels);
}

TEST_CASE("Bundle::fontFallbacks keeps the default chain under the empty language key (T-555, T-527c)") {
    const fs::path dir = xt::test::tempDir() / "xt_t555_default_chain";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::copy(compile::pathFromUtf8(corpusBundleT555()), dir, fs::copy_options::recursive, ec);
    REQUIRE(ec.value() == 0);
    nlohmann::json manifest = nlohmann::json::parse(readTextT555(dir / "manifest.json"));
    manifest["fonts"]["fallback"][""] = {"Noto Sans", "Nowhere Sans"};
    manifest["fonts"]["fallback"]["zh-TW"] = {"Noto Sans TC"};
    {
        std::ofstream out(dir / "manifest.json", std::ios::binary);
        out << manifest.dump(2);
    }
    std::unique_ptr<Bundle> bundle = openT555(compile::genericPathToUtf8(dir));
    REQUIRE(bundle->fontFallbacks().size() == 2);
    bool sawDefault = false;
    for (const FontFallbackInfo& chain : bundle->fontFallbacks()) {
        if (chain.lang.empty()) {
            sawDefault = true;
            CHECK(chain.families == std::vector<std::string>{"Noto Sans", "Nowhere Sans"});
        }
    }
    CHECK(sawDefault);
    text::FtTextShaper shaper;
    Result<uint32_t> added = addBundleFonts(*bundle, "hud", shaper);
    REQUIRE(added.ok());
    CHECK(added.value() == 4);
}

#endif  // XT_TEST_HAS_TEXT
