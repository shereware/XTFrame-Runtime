#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <json.hpp>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "doctest.h"
#include "test_temp.h"
#include "xt/asset_resolver.h"
#include "xt/bundle/bundle.h"
#include "xt/bundle/pack.h"
#include "xt/bundle/project_file.h"
#include "xt/bundle/ui_project.h"
#include "xt/command_list.h"
#include "xt/compile/utf8_path.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/node_arena.h"
#include "xt/sw/sw_png.h"
#include "xt/text_shaper.h"
#ifdef XT_TEST_HAS_TEXT
#include "xt/text/ft_text_shaper.h"
#endif

/// pack(): compiling a bundle's source form into its published form -- asset deduplication,
/// thumbnails, connection points, `requires`, the incremental `--ui` path and `--prune`. See
/// Docs/design/22-bundle.md §22.3/§22.4, Docs/tasks/T-92.md "測試".

using namespace xt;
using namespace xt::bundle;

namespace {

namespace fs = std::filesystem;

std::string sampleFixture() { return std::string(XT_TEST_BUNDLE_DIR) + "/fixtures/sample"; }

/// A fresh, writable copy of the `sample` source bundle under the system temp dir (UTF-8, T-261f).
std::string copiedSample(const std::string& name) {
    const fs::path dir = xt::test::tempDir() / compile::pathFromUtf8("xt_pack_src_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::copy(compile::pathFromUtf8(sampleFixture()), dir, fs::copy_options::recursive, ec);
    REQUIRE(ec.value() == 0);
    return compile::genericPathToUtf8(dir);
}

/// An empty output directory under the system temp dir.
std::string outDir(const std::string& name) {
    const fs::path dir = xt::test::tempDir() / ("xt_pack_out_" + name);
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

/// Number of regular files directly inside `dir`; 0 when it does not exist.
size_t fileCount(const std::string& dir) {
    std::error_code ec;
    size_t count = 0;
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        if (it->is_regular_file(ec)) {
            ++count;
        }
    }
    return count;
}

/// Number of pixels inside the [x, x+w) x [y, y+h) box of an RGBA8 thumbnail that differ from the
/// fully transparent background by more than 40/255 in any channel -- i.e. glyph coverage.
int textPixels(const std::vector<uint8_t>& rgba, int stride, int x, int y, int w, int h) {
    int count = 0;
    for (int py = y; py < y + h; ++py) {
        for (int px = x; px < x + w; ++px) {
            const size_t offset =
                (static_cast<size_t>(py) * static_cast<size_t>(stride) + static_cast<size_t>(px)) * 4;
            for (size_t channel = 0; channel < 4; ++channel) {
                if (rgba[offset + channel] > 40) {
                    ++count;
                    break;
                }
            }
        }
    }
    return count;
}

const PortInfo* findPort(const UiInfo& ui, const std::string& name) {
    for (const PortInfo& port : ui.ports) {
        if (port.name == name) {
            return &port;
        }
    }
    return nullptr;
}

const UiInfo* findUi(const std::vector<UiInfo>& uis, const std::string& id) {
    for (const UiInfo& ui : uis) {
        if (ui.id == id) {
            return &ui;
        }
    }
    return nullptr;
}

/// Every UiInfo of a bundle opened at `dir`, copied out so the Bundle can be closed.
std::vector<UiInfo> openUis(const std::string& dir) {
    std::string error = "unchanged";
    Result<std::unique_ptr<Bundle>> bundle = Bundle::open(dir, nullptr, &error);
    REQUIRE_MESSAGE(bundle.ok(), error);
    const std::span<const UiInfo> uis = bundle.value()->uis();
    return std::vector<UiInfo>(uis.begin(), uis.end());
}

}  // namespace

TEST_CASE("pack: full run deduplicates assets and writes a readable manifest") {
    const std::string source = copiedSample("full");
    const std::string out = outDir("full");

    PackOptions options;
    options.outDir = out;
    const PackReport report = pack(loadProject(source), options);

    CHECK(report.ok);
    REQUIRE(report.packedUis.size() == 2);
    CHECK(report.packedUis[0] == "hud");
    CHECK(report.packedUis[1] == "pause");
    // The same bytes are referenced from hud/img/a.png and shared/img/a.png: one file, one reuse.
    CHECK(report.assetsWritten == 1);
    CHECK(report.assetsDeduplicated == 1);
    CHECK(fileCount(out + "/assets") == 1);

    const std::vector<UiInfo> uis = openUis(out);
    REQUIRE(uis.size() == 2);
    const UiInfo* hud = findUi(uis, "hud");
    const UiInfo* pause = findUi(uis, "pause");
    REQUIRE(hud != nullptr);
    REQUIRE(pause != nullptr);

    REQUIRE(hud->assets.size() == 1);
    REQUIRE(pause->assets.size() == 1);
    CHECK(hud->assets[0] == pause->assets[0]);
    CHECK(hud->assets[0].rfind("assets/", 0) == 0);
    CHECK(hud->file == "ui/hud.xtir");
    CHECK(hud->flow == "ui/hud.xtflow");
    CHECK(hud->fileHash != 0);
    CHECK(hud->size.x == 1280.0f);
    CHECK(hud->requiresText);

    const PortInfo* click = findPort(*hud, "#fire.Click");
    REQUIRE(click != nullptr);
    CHECK_FALSE(click->isInput);
    CHECK(click->type == "pulse");
    const PortInfo* setText = findPort(*hud, "#fire.setText");
    REQUIRE(setText != nullptr);
    CHECK(setText->isInput);
    CHECK(setText->type == "string");

    CHECK(hud->thumb == "thumbs/hud.png");
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
    REQUIRE(sw::readPng(out + "/thumbs/hud.png", rgba, width, height));
    CHECK(width == 320);
    CHECK(height == 180);
    // The thumbnail must show the button's label, which only happens when the shaper found a
    // font: pack() registers the system fonts, the fixture names no family (Docs/tasks/T-97.md).
    // The label sits at roughly (0..64, 20..48) of the 1280x720 render, i.e. a quarter of that
    // once downscaled to 320x180; the clear colour is fully transparent.
    CHECK(textPixels(rgba, width, 0, 2, 80, 20) >= 20);
}

TEST_CASE("pack: each UI's fit is carried into the manifest (T-156)") {
    const std::string source = copiedSample("fit");
    const std::string out = outDir("fit");

    ProjectFile project = loadProject(source);
    REQUIRE(project.uis.size() == 2);
    project.uis[0].fit = "contain";  // hud; pause keeps the default.
    PackOptions options;
    options.outDir = out;
    options.thumbnails = false;
    REQUIRE(pack(project, options).ok);

    const std::vector<UiInfo> uis = openUis(out);
    REQUIRE(findUi(uis, "hud") != nullptr);
    REQUIRE(findUi(uis, "pause") != nullptr);
    CHECK(findUi(uis, "hud")->fit == "contain");
    CHECK(findUi(uis, "pause")->fit == "native");

    std::ifstream in(out + "/manifest.json", std::ios::binary);
    const std::string manifest((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(manifest.find("\"fit\": \"contain\"") != std::string::npos);
    CHECK(manifest.find("\"fit\": \"native\"") != std::string::npos);
}

TEST_CASE("pack: the packed .xtir references the deduplicated asset name") {
    const std::string source = copiedSample("assets");
    const std::string out = outDir("assets");

    PackOptions options;
    options.outDir = out;
    options.thumbnails = false;
    REQUIRE(pack(loadProject(source), options).ok);

    NullTextShaper shaper;
    FileAssetResolver assets(out, sw::decodeImageBytes);
    Context ctx(ContextConfig{&shaper, &assets});
    Result<DocumentHandle> handle = ctx.loadDocumentFile(out + "/ui/hud.xtir");
    REQUIRE(handle.ok());
    const Document* doc = ctx.document(handle.value());
    REQUIRE(doc != nullptr);

    const IrView& view = doc->ir();
    REQUIRE(view.assets().assets().size() == 1);
    const std::string name(view.string(view.assets().assets()[0].name));
    CHECK(name.rfind("assets/", 0) == 0);
    CHECK(name.size() == std::string("assets/0123456789abcdef.png").size());

    std::vector<uint8_t> bytes;
    CHECK(assets.readBytes(name, bytes));
    CHECK_FALSE(bytes.empty());
}

TEST_CASE("pack: --ui only rewrites the named UI") {
    const std::string source = copiedSample("incremental");
    const std::string out = outDir("incremental");

    PackOptions options;
    options.outDir = out;
    options.thumbnails = false;
    REQUIRE(pack(loadProject(source), options).ok);
    const std::vector<UiInfo> before = openUis(out);
    const UiInfo* hudBefore = findUi(before, "hud");
    const UiInfo* pauseBefore = findUi(before, "pause");
    REQUIRE(hudBefore != nullptr);
    REQUIRE(pauseBefore != nullptr);

    {
        std::ofstream edited(source + "/menus/pause.xt", std::ios::binary);
        edited << "<xt>\n  <body>\n    <div id=\"pausePanel\">\n      <img src=\"img/a.png\"/>\n"
                  "      <span id=\"resume\">Resume</span>\n    </div>\n  </body>\n</xt>\n";
    }

    PackOptions incremental = options;
    incremental.onlyUis = {"pause"};
    const PackReport report = pack(loadProject(source), incremental);
    CHECK(report.ok);
    REQUIRE(report.packedUis.size() == 1);
    CHECK(report.packedUis[0] == "pause");

    const std::vector<UiInfo> after = openUis(out);
    REQUIRE(after.size() == 2);
    const UiInfo* hudAfter = findUi(after, "hud");
    const UiInfo* pauseAfter = findUi(after, "pause");
    REQUIRE(hudAfter != nullptr);
    REQUIRE(pauseAfter != nullptr);
    CHECK(hudAfter->fileHash == hudBefore->fileHash);
    CHECK(hudAfter->assets == hudBefore->assets);
    CHECK(pauseAfter->fileHash != pauseBefore->fileHash);
}

TEST_CASE("pack: --prune deletes files the manifest no longer references") {
    const std::string source = copiedSample("prune");

    SUBCASE("kept without --prune") {
        const std::string out = outDir("prune_off");
        PackOptions options;
        options.outDir = out;
        options.thumbnails = false;
        REQUIRE(pack(loadProject(source), options).ok);
        {
            std::ofstream junk(out + "/assets/deadbeefdeadbeef.png", std::ios::binary);
            junk << "junk";
        }
        const PackReport report = pack(loadProject(source), options);
        CHECK(report.assetsPruned == 0);
        CHECK(fs::exists(out + "/assets/deadbeefdeadbeef.png"));
    }

    SUBCASE("deleted with --prune") {
        const std::string out = outDir("prune_on");
        PackOptions options;
        options.outDir = out;
        options.thumbnails = false;
        REQUIRE(pack(loadProject(source), options).ok);
        {
            std::ofstream junk(out + "/assets/deadbeefdeadbeef.png", std::ios::binary);
            junk << "junk";
        }
        options.prune = true;
        const PackReport report = pack(loadProject(source), options);
        CHECK(report.assetsPruned == 1);
        CHECK_FALSE(fs::exists(out + "/assets/deadbeefdeadbeef.png"));
    }
}

TEST_CASE("pack: a broken UI fails the run but the others are still written") {
    const std::string source = copiedSample("broken");
    const std::string out = outDir("broken");
    {
        std::ofstream broken(source + "/menus/pause.xt", std::ios::binary);
        broken << "<xt><body><div id=\"oops\"></body></xt>\n";
    }

    PackOptions options;
    options.outDir = out;
    options.thumbnails = false;
    const PackReport report = pack(loadProject(source), options);

    CHECK_FALSE(report.ok);
    REQUIRE(report.packedUis.size() == 1);
    CHECK(report.packedUis[0] == "hud");
    bool sawPauseError = false;
    for (const compile::Diagnostic& d : report.diagnostics) {
        if (d.level == compile::Diagnostic::Level::Error && d.file.rfind("pause: ", 0) == 0) {
            sawPauseError = true;
        }
    }
    CHECK(sawPauseError);
    CHECK(fs::exists(out + "/ui/hud.xtir"));
    CHECK_FALSE(fs::exists(out + "/ui/pause.xtir"));
}

TEST_CASE("pack: --no-thumbs leaves the thumbnail out of the manifest") {
    const std::string source = copiedSample("nothumbs");
    const std::string out = outDir("nothumbs");

    PackOptions options;
    options.outDir = out;
    options.thumbnails = false;
    REQUIRE(pack(loadProject(source), options).ok);

    CHECK_FALSE(fs::exists(out + "/thumbs"));
    const std::vector<UiInfo> uis = openUis(out);
    for (const UiInfo& ui : uis) {
        CHECK(ui.thumb.empty());
    }
}

// Docs/tasks/T-150.md: an `<img src="engine://scene">` is a host texture. The packer never sees
// it (the compiler does not call assetNameRewrite for it), so it is not written under assets/,
// not listed in the UI's `assets`, and --strict has nothing to complain about.
TEST_CASE("pack: an engine:// image is neither packed nor listed, and passes --strict") {
    const std::string source = copiedSample("engine");
    const std::string out = outDir("engine");
    {
        std::ofstream edited(source + "/hud/hud.xt", std::ios::binary);
        edited << "<xt>\n  <body>\n    <div id=\"panel\">\n      <img src=\"img/a.png\"/>\n"
                  "      <img src=\"engine://scene\"/>\n"
                  "      <button id=\"fire\" style=\"font-size: 48px\">Fire</button>\n"
                  "    </div>\n  </body>\n</xt>\n";
    }

    PackOptions options;
    options.outDir = out;
    options.strict = true;
    // Only hud: the fixture's pause.xt finds img/a.png through shared.assetRoot, which the
    // compiler (correctly, Docs/design/22-bundle.md §22.2) still reports as a warning.
    options.onlyUis = {"hud"};
    options.thumbnails = false;
    const PackReport report = pack(loadProject(source), options);

    std::string diagnostics;
    for (const compile::Diagnostic& d : report.diagnostics) {
        diagnostics += d.file + ": " + d.message + "\n";
    }
    INFO("diagnostics:\n", diagnostics);
    CHECK(report.ok);
    REQUIRE(report.packedUis.size() == 1);
    CHECK(report.assetsWritten == 1);
    CHECK(fileCount(out + "/assets") == 1);

    const std::vector<UiInfo> uis = openUis(out);
    const UiInfo* hud = findUi(uis, "hud");
    REQUIRE(hud != nullptr);
    REQUIRE(hud->assets.size() == 1);
    CHECK(hud->assets[0].rfind("assets/", 0) == 0);
    CHECK(hud->assets[0].find("engine") == std::string::npos);
}

TEST_CASE("pack: tokens and themes reach the manifest, and a theme switches a loaded UI (T-170)") {
    const std::string source = copiedSample("tokens");
    const std::string out = outDir("tokens");
    {
        std::ofstream base(source + "/shared/base.xts", std::ios::app);
        base << "\n:root { --accent: #ff0000; --gap: 8px; }\n#fire { color: var(--accent); }\n";
        fs::create_directories(source + "/themes");
        std::ofstream dark(source + "/themes/dark.xts");
        dark << ":root { --accent: #0000ff; --gap: 16px; }\n";
        std::ofstream light(source + "/themes/light.xts");
        light << ":root { --accent: #00ff00; }\n.stray { width: 1px; }\n";
    }

    PackOptions options;
    options.outDir = out;
    options.thumbnails = false;
    const PackReport report = pack(loadProject(source), options);
    CHECK(report.ok);
    // A theme only sets tokens; light.xts's extra rule is reported, not packed into anything.
    bool warnedLight = false;
    for (const compile::Diagnostic& d : report.diagnostics) {
        warnedLight = warnedLight || (d.file == "themes/light.xts" &&
                                      d.message.find("only sets tokens") != std::string::npos);
    }
    CHECK(warnedLight);

    std::string error;
    Result<std::unique_ptr<Bundle>> opened = Bundle::open(out, nullptr, &error);
    REQUIRE_MESSAGE(opened.ok(), error);
    Bundle& bundle = *opened.value();
    REQUIRE(bundle.tokens().size() == 2);
    CHECK(bundle.tokens()[0].name == "--accent");
    CHECK(bundle.tokens()[0].value == "#ff0000");
    CHECK(bundle.tokens()[1].name == "--gap");
    CHECK(bundle.tokens()[1].value == "8px");
    REQUIRE(bundle.themes().size() == 2);
    CHECK(bundle.themes()[0].name == "dark");
    CHECK(bundle.themes()[0].file == "themes/dark.xts");
    CHECK(bundle.themes()[1].name == "light");
    CHECK(fs::exists(out + "/themes/dark.xts"));
    CHECK(fs::exists(out + "/themes/light.xts"));
    const std::vector<TokenDef> darkTokens = parseThemeTokens(":root { --accent: #0000ff; --gap: 16px; }");
    REQUIRE(darkTokens.size() == 2);
    CHECK(darkTokens[1].value == "16px");

    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, &bundle.assets()});
    ctx.setViewport(Rect{0, 0, 1280, 720}, 1.0f);
    Result<DocumentHandle> handle = loadUi(ctx, bundle, "hud");
    REQUIRE(handle.ok());
    REQUIRE(loadThemes(ctx, bundle) == Status::kOk);
    ctx.update(0.0f);
    Document* doc = ctx.document(handle.value());
    REQUIRE(doc != nullptr);
    ElementRef fire = doc->byId("fire");
    REQUIRE(fire.valid());
    CHECK(doc->arena().element(fire.node).style.base[prop::Color].c == Color{1, 0, 0, 1});
    REQUIRE(ctx.setTheme("dark") == Status::kOk);
    ctx.update(0.0f);
    CHECK(doc->arena().element(fire.node).style.base[prop::Color].c == Color{0, 0, 1, 1});
    REQUIRE(ctx.setTheme("light") == Status::kOk);
    ctx.update(0.0f);
    CHECK(doc->arena().element(fire.node).style.base[prop::Color].c == Color{0, 1, 0, 1});
}

TEST_CASE("pack: a .xtfx custom effect is packed as a shader asset (T-203)") {
    const std::string source = copiedSample("xtfx");
    const std::string out = outDir("xtfx");
    {
        std::ofstream edited(source + "/hud/hud.xt", std::ios::binary);
        edited << "<xt>\n  <body>\n    <div id=\"panel\" style=\"width: 64px; height: 64px; "
                  "effect: custom(&quot;fx/ripple.xtfx&quot;, amplitude 2)\">\n"
                  "      <img src=\"img/a.png\"/>\n"
                  "      <button id=\"fire\" style=\"font-size: 48px\">Fire</button>\n"
                  "    </div>\n  </body>\n</xt>\n";
    }
    fs::create_directories(source + "/hud/fx");
    {
        std::ofstream fx(source + "/hud/fx/ripple.xtfx", std::ios::binary);
        fx << "name: Ripple\nparam amplitude float 4 [0, 32]\nfallback: blur(2px)\nexpand: 8px\n---\n"
              "float4 xtMain(XtInput i) { return xtSample(i.uv + sin(i.uv.yx * 40) * amplitude * "
              "i.texelSize); }\n";
    }

    PackOptions options;
    options.outDir = out;
    options.onlyUis = {"hud"};
    options.thumbnails = false;
    const PackReport report = pack(loadProject(source), options);
    std::string diagnostics;
    for (const compile::Diagnostic& d : report.diagnostics) {
        diagnostics += d.file + ": " + d.message + "\n";
    }
    INFO("diagnostics:\n", diagnostics);
    CHECK(report.ok);
    CHECK(report.assetsWritten == 2);

    std::string error = "unchanged";
    Result<std::unique_ptr<Bundle>> bundle = Bundle::open(out, nullptr, &error);
    REQUIRE_MESSAGE(bundle.ok(), error);
    const AssetInfo* shader = nullptr;
    for (const AssetInfo& info : bundle.value()->assetTable()) {
        if (info.kind == "shader") {
            shader = &info;
        }
    }
    REQUIRE(shader != nullptr);
    CHECK(shader->file.rfind("assets/", 0) == 0);
    CHECK(shader->file.size() > 5);
    CHECK(shader->file.substr(shader->file.size() - 5) == ".xtfx");
    CHECK(fs::exists(out + "/" + shader->file));
    const UiInfo* hud = nullptr;
    for (const UiInfo& ui : bundle.value()->uis()) {
        if (ui.id == "hud") {
            hud = &ui;
        }
    }
    REQUIRE(hud != nullptr);
    CHECK(std::find(hud->assets.begin(), hud->assets.end(), shader->file) != hud->assets.end());

    // The packed UI carries the compiled shader (SHDR), so it needs nothing but itself at runtime.
    std::ifstream in(out + "/" + hud->file, std::ios::binary);
    const std::vector<uint8_t> ir((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Result<IrView> view = IrView::open(IrBlob{ir.data(), ir.size()}, IrReadOptions{});
    REQUIRE(view.ok());
    REQUIRE(view.value().shaders().shaders().size() == 1);
    const uint32_t asset = view.value().shaders().shaders()[0].asset;
    CHECK(view.value().assets().assets()[asset].kind == 2);
    CHECK(view.value().string(view.value().assets().assets()[asset].name) == shader->file);
}

TEST_CASE("pack: an image under shared.assetRoot resolves at compile time, no false 'not found'") {
    // T-204 (§22.2 "UI dir -> shared.assetRoot -> bundle root"): the compile ran without that
    // search order, so `marker.png` / `glyphs/mouse/w.png` (both under `shared/`) were reported
    // "image asset not found" although rewriteAsset()'s own fallback then packed them.
    const std::string source = copiedSample("shared_assets");
    std::vector<uint8_t> rgba(8 * 8 * 4, 200);
    REQUIRE(sw::writePng(source + "/shared/marker.png", rgba.data(), 8, 8, 8 * 4));
    std::error_code ec;
    fs::create_directories(source + "/shared/glyphs/mouse", ec);
    rgba[0] = 10;  // Different bytes, so the two do not deduplicate.
    REQUIRE(sw::writePng(source + "/shared/glyphs/mouse/w.png", rgba.data(), 8, 8, 8 * 4));
    auto writeHud = [&](const std::string& extra) {
        std::ofstream(source + "/hud/hud.xt", std::ios::trunc)
            << "<xt>\n  <body>\n    <div id=\"panel\">\n      <img src=\"img/a.png\"/>\n"
               "      <img id=\"marker\" src=\"marker.png\"/>\n"
               "      <img id=\"key\" src=\"glyphs/mouse/w.png\"/>\n"
            << extra << "    </div>\n  </body>\n</xt>\n";
    };
    writeHud("");
    PackOptions options;
    options.outDir = outDir("shared_assets");
    options.thumbnails = false;
    const PackReport report = pack(loadProject(source), options);
    REQUIRE(report.ok);
    for (const compile::Diagnostic& d : report.diagnostics) {
        CHECK_MESSAGE(d.message.find("not found") == std::string::npos, d.file, ": ", d.message);
    }
    Result<std::unique_ptr<Bundle>> bundle = Bundle::open(options.outDir, sw::decodeImageBytes);
    REQUIRE(bundle.ok());
    const UiInfo* hud = bundle.value()->ui("hud");
    REQUIRE(hud != nullptr);
    CHECK(hud->assets.size() == 3);  // img/a.png, marker.png, glyphs/mouse/w.png
    for (const std::string& file : hud->assets) {
        CHECK(fs::exists(options.outDir + "/" + file));
    }

    // A file that really is missing still warns (once).
    writeHud("      <img src=\"nope.png\"/>\n");
    options.outDir = outDir("shared_assets_missing");
    const PackReport missing = pack(loadProject(source), options);
    int notFound = 0;
    for (const compile::Diagnostic& d : missing.diagnostics) {
        notFound += d.message.find("\"nope.png\" not found") != std::string::npos ? 1 : 0;
        CHECK(d.message.find("\"marker.png\" not found") == std::string::npos);
    }
    CHECK(notFound == 1);
}

namespace {

/// T-204: counts the `<glyph>` inline objects of `doc` that resolved to an asset.
int resolvedGlyphs(const Document& doc) {
    int count = 0;
    const NodeArena& arena = doc.arena();
    for (NodeId node = 0; node < arena.capacity(); ++node) {
        if (!arena.alive(node) || arena.kind(node) != NodeKind::Text) {
            continue;
        }
        for (const TextInline& object : arena.text(node).inlines) {
            count += (object.kind == TextInlineKind::Glyph && object.asset != kInvalidAsset) ? 1 : 0;
        }
    }
    return count;
}

struct MapGlyphs : IGlyphProvider {
    int calls = 0;
    AssetRef resolve(std::string_view action, InputKind) override {
        ++calls;
        return AssetRef{action == "host" ? std::string("assets/host.png") : std::string()};
    }
};

}  // namespace

TEST_CASE("pack: manifest glyphs[device] from glyphs/<device>/<key>.<ext>, Bundle::glyphAsset, provider") {
    // T-204 (§22.10 `glyphs[device]`, 13 §13.8.8).
    const std::string source = copiedSample("glyphs");
    std::error_code ec;
    for (const char* dir : {"mouse", "gamepad", "keyboard", "bogus"}) {
        fs::create_directories(source + "/shared/glyphs/" + dir, ec);
    }
    std::vector<uint8_t> rgba(8 * 8 * 4, 255);
    auto png = [&](const std::string& path, uint8_t tint) {
        rgba[0] = tint;  // Distinct bytes per file, so none deduplicate.
        REQUIRE(sw::writePng(source + path, rgba.data(), 8, 8, 8 * 4));
    };
    png("/shared/glyphs/mouse/w.png", 1);
    png("/shared/glyphs/gamepad/a.png", 2);
    png("/shared/glyphs/keyboard/esc.png", 3);
    png("/shared/glyphs/bogus/x.png", 4);
    std::ofstream(source + "/shared/glyphs/mouse/readme.txt") << "not an image";
    std::ofstream(source + "/hud/hud.xt", std::ios::trunc)
        << "<xt>\n  <body>\n    <div id=\"panel\">\n      <img src=\"img/a.png\"/>\n"
           "      <img id=\"key\" src=\"glyphs/mouse/w.png\"/>\n"
           "      <p id=\"prompt\">Press <glyph key=\"w\"/></p>\n"
           "    </div>\n  </body>\n</xt>\n";

    for (const bool atlas : {false, true}) {
        CAPTURE(atlas);
        PackOptions options;
        options.outDir = outDir(atlas ? "glyphs_atlas" : "glyphs");
        options.thumbnails = false;
        options.atlas = atlas;
        options.atlasScope = "bundle";
        const PackReport report = pack(loadProject(source), options);
        REQUIRE(report.ok);
        int bogus = 0;
        for (const compile::Diagnostic& d : report.diagnostics) {
            bogus += d.message.find("unknown device 'bogus'") != std::string::npos ? 1 : 0;
        }
        CHECK(bogus == 1);

        Result<std::unique_ptr<Bundle>> opened = Bundle::open(options.outDir, sw::decodeImageBytes);
        REQUIRE(opened.ok());
        Bundle& bundle = *opened.value();
        REQUIRE(bundle.glyphs().size() == 3);
        CHECK(bundle.glyphs()[0].device == "gamepad");
        CHECK(bundle.glyphs()[1].device == "keyboard");
        CHECK(bundle.glyphs()[2].device == "mouse");
        const std::string w(bundle.glyphAsset("mouse", "w"));
        CHECK(w.rfind("assets/", 0) == 0);
        CHECK(fs::exists(options.outDir + "/" + w));  // Never atlased: resolved by name at run time.
        CHECK(fs::exists(options.outDir + "/" + std::string(bundle.glyphAsset("gamepad", "a"))));
        CHECK(fs::exists(options.outDir + "/" + std::string(bundle.glyphAsset("keyboard", "esc"))));
        CHECK(bundle.glyphAsset("mouse", "a").empty());
        CHECK(bundle.glyphAsset("bogus", "x").empty());
        const std::string manifestText = [&] {
            std::ifstream in(options.outDir + "/manifest.json");
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }();
        CHECK(manifestText.find("\"glyphs\": {") != std::string::npos);

        // The default provider: manifest first (Mouse falls back to "keyboard"), then the host's.
        MapGlyphs host;
        BundleGlyphProvider provider(bundle, &host);
        CHECK(provider.resolve("w", InputKind::Mouse).name == w);
        CHECK(provider.resolve("esc", InputKind::Mouse).name == bundle.glyphAsset("keyboard", "esc"));
        CHECK(provider.resolve("a", InputKind::Gamepad).name == bundle.glyphAsset("gamepad", "a"));
        CHECK(host.calls == 0);
        CHECK(provider.resolve("host", InputKind::Touch).name == "assets/host.png");
        CHECK(host.calls == 1);
        BundleGlyphProvider alone(bundle);
        CHECK(alone.resolve("w", InputKind::Gamepad).name.empty());

        // Through a loaded UI: the `<glyph key="w">` draws the packed image on mouse only.
        NullTextShaper shaper;
        Context ctx(ContextConfig{&shaper, &bundle.assets()});
        ctx.setGlyphProvider(&alone);
        Result<DocumentHandle> handle = loadUi(ctx, bundle, "hud");
        REQUIRE(handle.ok());
        Document* doc = ctx.document(handle.value());
        REQUIRE(doc != nullptr);
        CHECK(resolvedGlyphs(*doc) == 1);
        REQUIRE(ctx.setInputDevice(InputKind::Gamepad) == Status::kOk);
        CHECK(resolvedGlyphs(*doc) == 0);
    }
}

TEST_CASE("pack: an xt-standard: symbol link resolves in the standard symbol library (T-235)") {
    const std::string src = copiedSample("xt_standard");
    {
        std::ofstream out(src + "/hud/hud.xt", std::ios::binary | std::ios::trunc);
        out << "<xt>\n  <head>\n    <link rel=\"symbols\" href=\"xt-standard:Checkbox\"/>\n  </head>\n"
               "  <body>\n    <div id=\"panel\">\n      <use symbol=\"Checkbox\" id=\"cb\"/>\n    </div>\n"
               "  </body>\n</xt>\n";
    }
    const ProjectFile project = loadProject(src);
    PackOptions options;
    options.outDir = outDir("xt_standard");
    options.onlyUis.push_back("hud");
    options.standardSymbolDir =
        std::string(XT_TEST_BUNDLE_DIR) + "/../../Assets/Symbols/groups/broadsheet-2026";
    const PackReport report = pack(project, options);
    CHECK(report.ok);
    CHECK(report.standardSymbolsCopied.empty());  // Nothing is copied for the scheme.
    CHECK_FALSE(fs::exists(fs::path(src) / "shared" / "symbols" / "Checkbox.xt"));
    CHECK(fs::exists(fs::path(options.outDir) / "ui" / "hud.xtir"));

    // Without a standard library the link is an error.
    PackOptions none = options;
    none.outDir = outDir("xt_standard_none");
    none.standardSymbolDir.clear();
    const PackReport failed = pack(project, none);
    CHECK_FALSE(failed.ok);
    bool reported = false;
    for (const xt::compile::Diagnostic& d : failed.diagnostics) {
        reported = reported || d.message.find("no standard symbol library") != std::string::npos;
    }
    CHECK(reported);
}

TEST_CASE("pack: a UI that fails to compile keeps its assets through a full --prune run (T-262a)") {
    const std::string source = copiedSample("failed_assets");
    const std::string out = outDir("failed_assets");
    // pause gets an image no other UI uses.
    std::vector<uint8_t> rgba(static_cast<size_t>(4) * 4 * 4);
    for (size_t i = 0; i < rgba.size(); ++i) {
        rgba[i] = static_cast<uint8_t>(17 + i * 7);
    }
    REQUIRE(sw::writePng(source + "/menus/only.png", rgba.data(), 4, 4, 4 * 4));
    {
        std::ofstream pause(source + "/menus/pause.xt", std::ios::binary | std::ios::trunc);
        pause << "<xt>\n  <body>\n    <div id=\"pausePanel\">\n      <img src=\"only.png\"/>\n    </div>\n"
                 "  </body>\n</xt>\n";
    }
    PackOptions options;
    options.outDir = out;
    options.thumbnails = false;
    REQUIRE(pack(loadProject(source), options).ok);
    const std::vector<UiInfo> before = openUis(out);
    const UiInfo* pauseBefore = findUi(before, "pause");
    REQUIRE(pauseBefore != nullptr);
    REQUIRE(pauseBefore->assets.size() == 1);
    const std::string only = pauseBefore->assets[0];
    REQUIRE(fs::exists(out + "/" + only));

    {
        std::ofstream broken(source + "/menus/pause.xt", std::ios::binary | std::ios::trunc);
        broken << "<xt><body><div id=\"oops\"></body></xt>\n";
    }
    options.prune = true;
    const PackReport report = pack(loadProject(source), options);
    CHECK_FALSE(report.ok);
    CHECK(fs::exists(out + "/" + only));
    CHECK(fs::exists(out + "/ui/pause.xtir"));
    const std::vector<UiInfo> after = openUis(out);
    const UiInfo* pauseAfter = findUi(after, "pause");
    REQUIRE(pauseAfter != nullptr);
    CHECK(pauseAfter->assets == pauseBefore->assets);
    // The manifest's assets[] still lists the file the kept entry names.
    std::string error;
    Result<std::unique_ptr<Bundle>> bundle = Bundle::open(out, nullptr, &error);
    REQUIRE_MESSAGE(bundle.ok(), error);
    bool listed = false;
    for (const AssetInfo& info : bundle.value()->assetTable()) {
        listed = listed || info.file == only;
    }
    CHECK(listed);
}

TEST_CASE("pack: a UI whose flow file cannot be read fails the run (T-262a)") {
    const std::string source = copiedSample("flow_missing");
    ProjectFile project = loadProject(source);
    REQUIRE(project.uis.size() == 2);
    REQUIRE(project.uis[0].id == "hud");
    project.uis[0].flow = "hud/missing.xtflow";
    PackOptions options;
    options.outDir = outDir("flow_missing");
    options.thumbnails = false;
    const PackReport report = pack(project, options);
    CHECK_FALSE(report.ok);
    bool named = false;
    for (const compile::Diagnostic& d : report.diagnostics) {
        named = named || (d.level == compile::Diagnostic::Level::Error &&
                          d.message.find("missing.xtflow") != std::string::npos);
    }
    CHECK(named);
    CHECK(std::find(report.packedUis.begin(), report.packedUis.end(), "hud") == report.packedUis.end());
}

TEST_CASE("T-359 pack: a UI listing no flow packs the sibling <stem>.xtflow with a warning") {
    const std::string source = copiedSample("flow_sibling");
    ProjectFile project = loadProject(source);
    REQUIRE(project.uis.size() == 2);
    REQUIRE(project.uis[0].id == "hud");
    REQUIRE(fs::is_regular_file(source + "/hud/hud.xtflow"));
    project.uis[0].flow.clear();
    PackOptions options;
    options.outDir = outDir("flow_sibling");
    options.thumbnails = false;
    const PackReport report = pack(project, options);
    REQUIRE(report.ok);
    bool warned = false;
    for (const compile::Diagnostic& d : report.diagnostics) {
        warned = warned || (d.level == compile::Diagnostic::Level::Warning && d.file == "hud" &&
                            d.message == "flow not listed in the .xtui; using the sibling hud.xtflow");
    }
    CHECK(warned);
    const std::vector<UiInfo> uis = openUis(options.outDir);
    const UiInfo* hud = findUi(uis, "hud");
    REQUIRE(hud != nullptr);
    CHECK(hud->flow == "ui/hud.xtflow");
    CHECK(fs::is_regular_file(options.outDir + "/ui/hud.xtflow"));
}

TEST_CASE("pack: a UI whose flow does not parse fails the run with the parse error (T-262a)") {
    const std::string source = copiedSample("flow_malformed");
    {
        std::ofstream flow(source + "/hud/hud.xtflow", std::ios::binary | std::ios::trunc);
        flow << "{\"nodes\": 5}\n";
    }
    PackOptions options;
    options.outDir = outDir("flow_malformed");
    options.thumbnails = false;
    const PackReport report = pack(loadProject(source), options);
    CHECK_FALSE(report.ok);
    bool parseError = false;
    for (const compile::Diagnostic& d : report.diagnostics) {
        parseError =
            parseError || (d.level == compile::Diagnostic::Level::Error && d.file == "hud: hud/hud.xtflow" &&
                           d.message.find("flow: malformed .xtflow document") == 0);
    }
    CHECK(parseError);
    CHECK(std::find(report.packedUis.begin(), report.packedUis.end(), "hud") == report.packedUis.end());
    CHECK_FALSE(fs::exists(options.outDir + "/ui/hud.xtir"));
    CHECK_FALSE(fs::exists(options.outDir + "/ui/hud.xtflow"));
}

// T-258b (PR #269 review): the run time does not decode an image over 16384 px per side or 64M
// pixels (T-258a), so the pack warns about one when it takes it into the bundle.
TEST_CASE("pack: an image the run time will not decode is a warning; the pack still succeeds") {
    const std::string src = copiedSample("huge_image");
    {
        // A PNG whose header declares 16385 x 1 (no pixel data; the warning reads the header).
        std::vector<uint8_t> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
                                 0,    0,   0,   13,  'I',  'H',  'D',  'R'};
        for (uint32_t v : {16385u, 1u}) {
            png.push_back(static_cast<uint8_t>(v >> 24));
            png.push_back(static_cast<uint8_t>(v >> 16));
            png.push_back(static_cast<uint8_t>(v >> 8));
            png.push_back(static_cast<uint8_t>(v));
        }
        png.insert(png.end(), {8, 6, 0, 0, 0, 0, 0, 0, 0});  // depth, RGBA, deflate, filter, interlace, CRC
        png.insert(png.end(), {0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82});
        std::ofstream out(src + "/hud/img/a.png", std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    }
    PackOptions options;
    options.outDir = outDir("huge_image");
    options.thumbnails = false;
    const PackReport report = pack(loadProject(src), options);
    CHECK(report.ok);
    int warnings = 0;
    for (const xt::compile::Diagnostic& d : report.diagnostics) {
        if (d.message.find("is not decoded at run time") != std::string::npos) {
            ++warnings;
            CHECK(d.level == xt::compile::Diagnostic::Level::Warning);
            CHECK(
                d.message ==
                "image \"img/a.png\" is 16385x1; larger than 16384 px per side or 64M pixels is not decoded "
                "at run time");
        }
    }
    CHECK(warnings == 1);  // shared/img/a.png (pause) is an ordinary image: no warning.
}

namespace {

bool endsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// T-273: writes a 4 x 4 opaque image and copies the test font into a directory outside the
/// project (`xt_pack_outside_<name>`), then rewrites `<source>/hud/hud.xt` to reference both by
/// their absolute paths. Returns that directory ('/' separators).
std::string writeOutsideHud(const std::string& source, const std::string& name) {
    const fs::path outside = xt::test::tempDir() / ("xt_pack_outside_" + name);
    std::error_code ec;
    fs::remove_all(outside, ec);
    fs::create_directories(outside, ec);
    REQUIRE(ec.value() == 0);
    std::vector<uint8_t> rgba(4 * 4 * 4, 0);
    for (size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i] = 200;
        rgba[i + 3] = 255;
    }
    REQUIRE(sw::writePng((outside / "outside.png").string(), rgba.data(), 4, 4, 4 * 4));
    fs::copy_file(std::string(XT_TEST_BUNDLE_DIR) + "/../fonts/NotoSans-Regular.subset.ttf",
                  outside / "outside.ttf", fs::copy_options::overwrite_existing, ec);
    REQUIRE(ec.value() == 0);
    const std::string dir = outside.generic_string();
    std::ofstream xt(source + "/hud/hud.xt", std::ios::binary | std::ios::trunc);
    xt << "<xt>\n<head><style>\n@font-face { font-family: \"Outside Sans\"; src: \"" << dir
       << "/outside.ttf\"; }\n#panel { font-family: \"Outside Sans\"; }\n</style></head>\n"
          "<body>\n<div id=\"panel\">\n<img id=\"pic\" src=\""
       << dir
       << "/outside.png\"/>\n<button id=\"fire\" style=\"font-size: 48px\">Fire</button>\n"
          "</div>\n</body>\n</xt>\n";
    return dir;
}

/// T-273: the packed hud carries both outside files as bundle-relative `assets/` names -- listed in
/// the manifest, present on disk, absent from the `.xtir` as absolute paths -- and its image
/// decodes through the opened bundle.
void checkOutsideAssetsPacked(const std::string& out, const std::string& outsideDir) {
    std::string error = "unchanged";
    Result<std::unique_ptr<Bundle>> opened = Bundle::open(out, sw::decodeImageBytes, &error);
    REQUIRE_MESSAGE(opened.ok(), error);
    Bundle& bundle = *opened.value();
    bool image = false;
    bool font = false;
    for (const AssetInfo& info : bundle.assetTable()) {
        CHECK(info.file.rfind("assets/", 0) == 0);
        CHECK(fs::is_regular_file(fs::path(out) / info.file));
        image = image || (info.kind == "image" && endsWith(info.file, ".png"));
        font = font || (info.kind == "font" && endsWith(info.file, ".ttf"));
    }
    CHECK(image);
    CHECK(font);

    std::ifstream in(out + "/ui/hud.xtir", std::ios::binary);
    const std::string ir((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE_FALSE(ir.empty());
    CHECK(ir.find(outsideDir) == std::string::npos);
    CHECK(ir.find("outside.png") == std::string::npos);
    CHECK(ir.find("outside.ttf") == std::string::npos);

    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, &bundle.assets()});
    Result<DocumentHandle> handle = loadUi(ctx, bundle, "hud");
    REQUIRE(handle.ok());
    const IrView& view = ctx.document(handle.value())->ir();
    int images = 0;
    int fonts = 0;
    for (size_t i = 0; i < view.assets().assets().size(); ++i) {
        const IrAsset& asset = view.assets().assets()[i];
        const std::string name(view.string(asset.name));
        CHECK(name.rfind("assets/", 0) == 0);
        if (asset.kind == 1) {
            ++fonts;
        }
        if (asset.kind == 0) {
            ++images;
            ImageData decoded;
            CHECK(bundle.assets().decodeImage(static_cast<AssetId>(i), view, decoded));
            CHECK(decoded.width == 4);
            CHECK(decoded.height == 4);
        }
    }
    CHECK(images == 1);
    CHECK(fonts == 1);
}

}  // namespace

TEST_CASE("pack: an image and a font referenced by absolute paths outside the project are packed (T-273)") {
    const std::string source = copiedSample("outside_full");
    const std::string outsideDir = writeOutsideHud(source, "full");
    const std::string out = outDir("outside_full");
    PackOptions options;
    options.outDir = out;
    options.thumbnails = false;
    const PackReport report = pack(loadProject(source), options);
    for (const compile::Diagnostic& d : report.diagnostics) {
        MESSAGE(d.file << ": " << d.message);
    }
    REQUIRE(report.ok);
    checkOutsideAssetsPacked(out, outsideDir);
}

TEST_CASE("pack --ui: absolute-path assets outside the project are packed on an incremental run (T-273)") {
    const std::string source = copiedSample("outside_ui");
    const std::string out = outDir("outside_ui");
    PackOptions options;
    options.outDir = out;
    options.thumbnails = false;
    REQUIRE(pack(loadProject(source), options).ok);

    const std::string outsideDir = writeOutsideHud(source, "ui");
    PackOptions incremental = options;
    incremental.onlyUis = {"hud"};
    const PackReport report = pack(loadProject(source), incremental);
    REQUIRE(report.ok);
    REQUIRE(report.packedUis.size() == 1);
    CHECK(report.packedUis[0] == "hud");
    checkOutsideAssetsPacked(out, outsideDir);
}

namespace {

/// T-273b: `path`'s bytes (empty when it cannot be read).
std::vector<uint8_t> readFileBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// T-273b: how many string-table entries of `view` read `text`.
int stringEntries(const IrView& view, std::string_view text) {
    int n = 0;
    for (uint32_t i = 0; i < view.strings().count(); ++i) {
        n += view.string(i) == text ? 1 : 0;
    }
    return n;
}

/// T-273b: the StringId of the attribute `name` of the DOCT element whose id is `elementId`;
/// kInvalidString when there is none.
StringId attributeValue(const IrView& view, std::string_view elementId, std::string_view name) {
    for (const IrDocElement& el : view.doc().elements()) {
        if (el.id == kInvalidString || view.string(el.id) != elementId) {
            continue;
        }
        for (uint32_t a = 0; a < el.attrCount; ++a) {
            const IrAttr& attr = view.doc().attrPool()[el.attrOffset + a];
            if (view.string(attr.name) == name) {
                return attr.value;
            }
        }
    }
    return kInvalidString;
}

/// T-273b: a 4 x 4 opaque PNG at `path`.
void writeSmallPng(const fs::path& path) {
    std::vector<uint8_t> rgba(4 * 4 * 4, 0);
    for (size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i + 1] = 180;
        rgba[i + 3] = 255;
    }
    REQUIRE(sw::writePng(path.string(), rgba.data(), 4, 4, 4 * 4));
}

/// T-273b: the `ui/hud.xtir` of the packed bundle at `out` names no machine path, no `..` and
/// nothing of `outsideDir`; every ASST name of it is exactly one string-table entry, and the `src`
/// of the element `#pic` is that very entry (one StringId for one name).
void checkPublishedNames(const std::string& out, const std::string& outsideDir) {
    const std::vector<uint8_t> ir = readFileBytes(out + "/ui/hud.xtir");
    REQUIRE_FALSE(ir.empty());
    Result<IrView> opened = IrView::open(IrBlob{ir.data(), ir.size()});
    REQUIRE(opened.ok());
    const IrView& view = opened.value();
    for (uint32_t i = 0; i < view.strings().count(); ++i) {
        const std::string_view text = view.string(i);
        CAPTURE(text);
        CHECK(text.find(outsideDir) == std::string_view::npos);
        CHECK(text.rfind("../", 0) != 0);
        CHECK(text.find("/../") == std::string_view::npos);
        CHECK_FALSE((text.size() > 2 && text[1] == ':' && (text[2] == '/' || text[2] == '\\')));
    }
    StringId imageName = kInvalidString;
    for (const IrAsset& asset : view.assets().assets()) {
        const std::string name(view.string(asset.name));
        CAPTURE(name);
        CHECK(stringEntries(view, name) == 1);
        if (asset.kind == 0 && imageName == kInvalidString) {
            imageName = asset.name;
        }
    }
    REQUIRE(imageName != kInvalidString);
    CHECK(attributeValue(view, "pic", "src") == imageName);
}

/// T-273b: the manifest of the bundle at `out` lists an asset whose source lies outside the
/// project by its file name alone.
void checkManifestSources(const std::string& out, const std::string& outsideDir) {
    std::string error;
    Result<std::unique_ptr<Bundle>> opened = Bundle::open(out, sw::decodeImageBytes, &error);
    REQUIRE_MESSAGE(opened.ok(), error);
    for (const AssetInfo& info : opened.value()->assetTable()) {
        for (const std::string& source : info.sources) {
            CAPTURE(source);
            CHECK(source.find(outsideDir) == std::string::npos);
            CHECK(source.find("..") == std::string::npos);
            CHECK(source.find(':') == std::string::npos);
        }
    }
    std::ifstream in(out + "/manifest.json", std::ios::binary);
    const std::string manifest((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(manifest.find(outsideDir) == std::string::npos);
    CHECK(manifest.find("\"outside.png\"") != std::string::npos);
}

/// T-273b (review of PR #280, item 1b): opens the bundle at `out` with a Context and renders the
/// hud: the outside image is drawn (a DrawImage whose asset decodes to 4 x 4) and, with the
/// FreeType shaper, the button text set in the outside font has real glyphs.
void checkOutsideAssetsDrawn(const std::string& out) {
    std::string error;
    Result<std::unique_ptr<Bundle>> opened = Bundle::open(out, sw::decodeImageBytes, &error);
    REQUIRE_MESSAGE(opened.ok(), error);
    Bundle& bundle = *opened.value();
#ifdef XT_TEST_HAS_TEXT
    text::FtTextShaper shaper;  // No system fonts: only the bundle's own font can give glyphs.
#else
    NullTextShaper shaper;
#endif
    Context ctx(ContextConfig{&shaper, &bundle.assets()});
    ctx.setViewport(Rect{0, 0, 1280, 720}, 1.0f);
    Result<DocumentHandle> handle = loadUi(ctx, bundle, "hud");
    REQUIRE(handle.ok());
    ctx.update(0.0f);
    CommandList commands;
    ctx.render(commands);
    const IrView& view = ctx.document(handle.value())->ir();
    int drawnImages = 0;
    int glyphRuns = 0;
    for (const Command& command : commands.commands) {
        if (command.kind == CmdKind::DrawImage) {
            const ImageCmd& image = commands.images[command.index];
            ImageData decoded;
            REQUIRE(bundle.assets().decodeImage(image.asset, view, decoded));
            CHECK(decoded.width == 4);
            CHECK(decoded.height == 4);
            ++drawnImages;
        } else if (command.kind == CmdKind::DrawGlyphs) {
            const GlyphsCmd& run = commands.glyphRuns[command.index];
            CHECK(run.font != kInvalidFont);
            CHECK(run.glyphCount > 0);
            for (uint32_t g = 0; g < run.glyphCount; ++g) {
                CHECK(commands.glyphs[run.glyphOffset + g] != 0);  // 0 = .notdef, the missing-glyph box.
            }
            ++glyphRuns;
        }
    }
    CHECK(drawnImages == 1);
#ifdef XT_TEST_HAS_TEXT
    CHECK(glyphRuns >= 1);
#else
    (void)glyphRuns;  // NullTextShaper: no font, so the run count is not checked.
#endif
}

}  // namespace

TEST_CASE("pack: an outside asset is published under one name: one string, one StringId (T-273b)") {
    const std::string source = copiedSample("published_names");
    const std::string outsideDir = writeOutsideHud(source, "published_names");
    const std::string out = outDir("published_names");
    PackOptions options;
    options.outDir = out;
    options.thumbnails = false;
    REQUIRE(pack(loadProject(source), options).ok);
    checkPublishedNames(out, outsideDir);
    checkManifestSources(out, outsideDir);
}

TEST_CASE("pack: the outside image and font are drawn from the packed bundle, full and --ui (T-273b)") {
    const std::string source = copiedSample("outside_drawn");
    const std::string out = outDir("outside_drawn");
    PackOptions options;
    options.outDir = out;
    options.thumbnails = false;
    REQUIRE(pack(loadProject(source), options).ok);
    writeOutsideHud(source, "drawn");
    REQUIRE(pack(loadProject(source), options).ok);
    checkOutsideAssetsDrawn(out);

    PackOptions incremental = options;
    incremental.onlyUis = {"hud"};
    const PackReport report = pack(loadProject(source), incremental);
    REQUIRE(report.ok);
    REQUIRE(report.packedUis.size() == 1);
    checkOutsideAssetsDrawn(out);
}

TEST_CASE(
    "pack: a relative reference escaping the project through .. is published by its bundle name (T-273b)") {
    // <base>/x/proj is the project and <base>/outside holds the files: from <proj>/hud,
    // `../../../outside/` reaches them.
    const fs::path base = xt::test::tempDir() / "xt_pack_escape";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base / "x", ec);
    fs::create_directories(base / "outside", ec);
    fs::copy(sampleFixture(), base / "x" / "proj", fs::copy_options::recursive, ec);
    REQUIRE(ec.value() == 0);
    writeSmallPng(base / "outside" / "outside.png");
    writeSmallPng(base / "outside" / "back.png");
    const std::string source = (base / "x" / "proj").generic_string();
    {
        std::ofstream xt(source + "/hud/hud.xt", std::ios::binary | std::ios::trunc);
        xt << "<xt>\n<head><style>\n#bg { width: 8px; height: 8px; background-image: "
              "url(\"../../../outside/back.png\"); }\n</style></head>\n<body>\n<div id=\"panel\">\n"
              "<img id=\"pic\" src=\"../../../outside/outside.png\"/>\n<div id=\"bg\"></div>\n"
              "</div>\n</body>\n</xt>\n";
    }
    const std::string out = outDir("escape");
    PackOptions options;
    options.outDir = out;
    options.thumbnails = false;
    const PackReport report = pack(loadProject(source), options);
    for (const compile::Diagnostic& d : report.diagnostics) {
        MESSAGE(d.file << ": " << d.message);
    }
    REQUIRE(report.ok);
    checkPublishedNames(out, (base / "outside").generic_string());
    checkManifestSources(out, (base / "outside").generic_string());
    const std::vector<uint8_t> ir = readFileBytes(out + "/ui/hud.xtir");
    const std::string text(ir.begin(), ir.end());
    CHECK(text.find("outside/outside.png") == std::string::npos);
    CHECK(text.find("outside/back.png") == std::string::npos);
}

TEST_CASE("pack --embed: an outside asset is embedded under its file name; the reference follows (T-273b)") {
    const std::string source = copiedSample("published_embed");
    const std::string outsideDir = writeOutsideHud(source, "published_embed");
    const std::string out = outDir("published_embed");
    PackOptions options;
    options.outDir = out;
    options.thumbnails = false;
    options.embed = true;
    REQUIRE(pack(loadProject(source), options).ok);
    checkPublishedNames(out, outsideDir);
    const std::vector<uint8_t> ir = readFileBytes(out + "/ui/hud.xtir");
    Result<IrView> opened = IrView::open(IrBlob{ir.data(), ir.size()});
    REQUIRE(opened.ok());
    std::vector<std::string> names;
    for (const IrAsset& asset : opened.value().assets().assets()) {
        names.emplace_back(opened.value().string(asset.name));
    }
    CHECK(std::find(names.begin(), names.end(), "outside.png") != names.end());
    CHECK(std::find(names.begin(), names.end(), "outside.ttf") != names.end());
}

namespace {

/// T-261e: the sample project with its hud drawing `img/<image>` and text in `fonts/<font>` (UTF-8
/// names, made at run time), packed into `<temp>/<outName>`; the bundle is then opened and drawn.
void checkNonAsciiAssetsDrawn(const std::string& name, const std::string& image, const std::string& font,
                              const std::string& outName) {
    using compile::genericPathToUtf8;
    using compile::pathFromUtf8;
    const std::string source = copiedSample(name);
    const fs::path src = pathFromUtf8(source);
    // Written under ASCII names, then moved: the writers are not what this test is about.
    std::vector<uint8_t> rgba(4 * 4 * 4, 0);
    for (size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i + 2] = 200;
        rgba[i + 3] = 255;
    }
    const fs::path asciiPng = xt::test::tempDir() / pathFromUtf8("xt_pack_t261e_" + name + ".png");
    REQUIRE(sw::writePng(compile::pathToUtf8(asciiPng), rgba.data(), 4, 4, 4 * 4));
    std::error_code ec;
    fs::create_directories(src / "hud" / "img", ec);
    fs::create_directories(src / "hud" / "fonts", ec);
    fs::rename(asciiPng, src / "hud" / "img" / pathFromUtf8(image), ec);
    REQUIRE(ec.value() == 0);
    fs::copy_file(pathFromUtf8(std::string(XT_TEST_BUNDLE_DIR) + "/../fonts/NotoSans-Regular.subset.ttf"),
                  src / "hud" / "fonts" / pathFromUtf8(font), fs::copy_options::overwrite_existing, ec);
    REQUIRE(ec.value() == 0);
    {
        std::ofstream xt(src / "hud" / "hud.xt", std::ios::binary | std::ios::trunc);
        xt << "<xt>\n<head><style>\n@font-face { font-family: \"Project Sans\"; src: \"fonts/" << font
           << "\"; }\n#panel { font-family: \"Project Sans\"; }\n</style></head>\n"
              "<body>\n<div id=\"panel\">\n<img id=\"pic\" src=\"img/"
           << image
           << "\"/>\n<button id=\"fire\" style=\"font-size: 48px\">Fire</button>\n"
              "</div>\n</body>\n</xt>\n";
    }
    const fs::path outPath = xt::test::tempDir() / pathFromUtf8(outName);
    fs::remove_all(outPath, ec);
    PackOptions options;
    options.outDir = genericPathToUtf8(outPath);
    options.thumbnails = false;
    const PackReport report = pack(loadProject(source), options);
    std::string diagnostics;
    for (const auto& d : report.diagnostics) {
        diagnostics += d.message + "\n";
    }
    INFO(diagnostics);
    REQUIRE(report.ok);
    checkOutsideAssetsDrawn(options.outDir);
    fs::remove_all(outPath, ec);
    fs::remove_all(src, ec);
}

}  // namespace

TEST_CASE(
    "pack: image and font names the ANSI code page can spell are drawn from the packed bundle (T-261e)") {
    // 角色頭像.png, 字型.ttf, out dir xt_pack_out_t261e_角色.
    checkNonAsciiAssetsDrawn("t261e_cp950", "\xE8\xA7\x92\xE8\x89\xB2\xE9\xA0\xAD\xE5\x83\x8F.png",
                             "\xE5\xAD\x97\xE5\x9E\x8B.ttf", "xt_pack_out_t261e_\xE8\xA7\x92\xE8\x89\xB2");
}

TEST_CASE(
    "pack: image and font names the ANSI code page cannot spell are drawn from the packed bundle (T-261e)") {
    // アイコン.png, 글꼴.ttf, out dir xt_pack_out_t261e_アイコン.
    checkNonAsciiAssetsDrawn("t261e_kana", "\xE3\x82\xA2\xE3\x82\xA4\xE3\x82\xB3\xE3\x83\xB3.png",
                             "\xEA\xB8\x80\xEA\xBC\xB4.ttf",
                             "xt_pack_out_t261e_\xE3\x82\xA2\xE3\x82\xA4\xE3\x82\xB3\xE3\x83\xB3");
}

namespace {

/// T-273c: the directory every outside file of the leak project lives in; its name is the marker
/// no published file may contain.
constexpr const char* kLeakMarker = "xt-outside-7f3a";
/// T-273c: the project directory's own name: a hit means a machine path into the project.
constexpr const char* kProjectMarker = "xt-project-7f3a";

/// T-273c: a hit of the marker that is still allowed, by the name of the outside file the hit
/// names, with the reason it is still there. Empty when every kind is fixed.
struct AllowedLeak {
    const char* file;
    const char* reason;
};
const std::vector<AllowedLeak> kAllowedLeaks = {
    // A video is not packed at all: the host plays the file `src` names (ExternalRequest::src) and
    // binds `engine://video:<id or src>`. T-273d (the design owner's ruling): whether a bundle
    // carries video files, as which manifest kind, and what `src` and the key say in a published
    // build is a format and host-contract decision not made yet; until then pack only warns ("video
    // ... is outside the project", checked in collectLeaks()), and exactly these two files stay
    // allowed. A third outside file showing up here fails the test.
    {"clip.webm", "<video id src>: the DOCT attribute"},
    {"intro.webm", "<video src> without id: the DOCT attribute and its engine://video:<src> name"},
};

/// T-273c: one occurrence of the marker in a published file: the file (relative to the bundle) and
/// the printable text around it.
struct LeakHit {
    std::string file;
    std::string context;
};

/// T-273c: the sample project at `<base>/xt-project-7f3a` with its hud referencing an outside file
/// of every kind that can name one, all under `<base>/xt-outside-7f3a/`: absolute paths, and `..`
/// escapes from the hud's directory. Returns the project directory.
std::string writeLeakProject(const fs::path& base) {
    std::error_code ec;
    fs::remove_all(base, ec);
    const fs::path outside = base / kLeakMarker;
    fs::create_directories(outside / "frames", ec);
    fs::create_directories(outside / "img", ec);
    fs::copy(sampleFixture(), base / kProjectMarker, fs::copy_options::recursive, ec);
    REQUIRE(ec.value() == 0);
    for (const char* png : {"pic.png", "back.png", "border.png", "sheet.png", "spark.png", "frames/f1.png",
                            "frames/f2.png", "img/pix.png"}) {
        writeSmallPng(outside / png);
    }
    writeSmallPng(outside / "img" / "mesh.png");  // T-273d: the `<xt-mesh href>` of rig.svg.
    fs::copy_file(std::string(XT_TEST_BUNDLE_DIR) + "/../fonts/NotoSans-Regular.subset.ttf",
                  outside / "font.ttf", fs::copy_options::overwrite_existing, ec);
    REQUIRE(ec.value() == 0);
    auto write = [&](const char* name, const std::string& text) {
        std::ofstream file(outside / name, std::ios::binary | std::ios::trunc);
        file << text;
    };
    write("clip.webm", "WEBM-CLIP");
    write("intro.webm", "WEBM-INTRO");
    write("anim.json",
          "{\"v\": \"5.6.5\", \"fr\": 30, \"ip\": 0, \"op\": 30, \"w\": 100, \"h\": 100,\n"
          " \"assets\": [{\"id\": \"image_0\", \"w\": 2, \"h\": 2, \"u\": \"img/\", \"p\": \"pix.png\", "
          "\"e\": 0}],\n"
          " \"layers\": [{\"ty\": 2, \"ind\": 0, \"nm\": \"Img\", \"ip\": 0, \"op\": 30, \"st\": 0, \"sr\": "
          "1,\n"
          "   \"refId\": \"image_0\", \"ks\": {\"a\": {\"a\": 0, \"k\": [0, 0, 0]}, \"p\": {\"a\": 0, \"k\": "
          "[0, 0, 0]},\n"
          "   \"s\": {\"a\": 0, \"k\": [100, 100, 100]}, \"r\": {\"a\": 0, \"k\": 0}, \"o\": {\"a\": 0, "
          "\"k\": 100}}}]}\n");
    write("motion.xtanim.json",
          "{\"v\": 1, \"timelines\": [{\"name\": \"slide\", \"duration\": 1, \"markers\": [], \"tracks\": "
          "[]}]}\n");
    write("style.xts", "#pic { width: 4px; height: 4px; }\n");
    write("shape.svg",
          "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"8\" height=\"8\" viewBox=\"0 0 8 8\">"
          "<rect x=\"0\" y=\"0\" width=\"8\" height=\"8\" fill=\"#ff0000\"/></svg>\n");
    write("lib.xt",
          "<xt>\n<head>\n<style>\nsymbol(Badge) { #dot { width: 4px; height: 4px; } }\n</style>\n"
          "<symbol name=\"Badge\"><div id=\"dot\"></div></symbol>\n</head>\n</xt>\n");
    write("ripple.xtfx",
          "name: Ripple\nparam amplitude float 4 [0, 32]\nfallback: blur(2px)\nexpand: 8px\n---\n"
          "float4 xtMain(XtInput i) { return xtSample(i.uv + sin(i.uv.yx * 40) * amplitude * "
          "i.texelSize); }\n");

    const std::string abs = outside.generic_string() + "/";
    const std::string esc = std::string("../../") + kLeakMarker + "/";
    const std::string source = (base / kProjectMarker).generic_string();
    // T-273d: an outside SVG file naming its mesh image by machine path.
    write("rig.svg",
          "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"8\" height=\"8\" viewBox=\"0 0 8 8\">"
          "<xt-mesh id=\"Face\" href=\"" +
              abs + "img/mesh.png\" width=\"8\" height=\"8\"/></svg>\n");
    std::ofstream xt(source + "/hud/hud.xt", std::ios::binary | std::ios::trunc);
    xt << "<xt>\n<head>\n<link rel=\"stylesheet\" href=\"" << abs << "style.xts\"/>\n"
       << "<link rel=\"symbols\" href=\"" << esc << "lib.xt\"/>\n<style>\n"
       << "@font-face { font-family: \"Leak Sans\"; src: \"" << abs << "font.ttf\"; }\n"
       << "#panel { font-family: \"Leak Sans\"; }\n"
       << "#bg { width: 8px; height: 8px; background-image: url(\"" << esc << "back.png\"); }\n"
       << "#frame { width: 8px; height: 8px; border-image-source: url(\"" << abs << "border.png\"); }\n"
       << "#fx { width: 8px; height: 8px; effect: custom(\"" << abs << "ripple.xtfx\", amplitude 2); }\n"
       << "@timeline slide from \"" << abs << "motion.xtanim.json#slide\";\n"
       << "</style>\n</head>\n<body>\n<div id=\"panel\">\n"
       << "<img id=\"pic\" src=\"" << abs << "pic.png\"/>\n"
       << "<img id=\"gone\" src=\"" << abs
       << "missing.png\"/>\n"
       // T-273d: the outside `<img src="*.svg">` files (and `<anim src>` below) are published as
       // their file name only and deliberately not copied into the bundle: their content is
       // compiled into the `.xtir` at pack time, so the run time never reads the file again. The
       // mesh image rig.svg names is an ordinary image asset, packed under its hashed name.
       << "<img id=\"vec\" src=\"" << esc << "shape.svg\"/>\n"
       << "<img id=\"rigged\" src=\"" << esc << "rig.svg\"/>\n"
       << "<div id=\"bg\"></div>\n<div id=\"frame\"></div>\n<div id=\"fx\"></div>\n"
       << "<sequence id=\"sheet\" src=\"" << abs << "sheet.png\" frame-size=\"2x2\"/>\n"
       << "<sequence id=\"frames\" src=\"" << esc << "frames\"/>\n"
       << "<particles id=\"sparks\" texture=\"" << esc
       << "spark.png\"/>\n"
       // T-273d: compiled into the `.xtir`, published by file name, not copied (see `#vec`).
       << "<anim id=\"lottie\" src=\"" << abs << "anim.json\"/>\n"
       << "<video id=\"clip\" src=\"" << abs << "clip.webm\"/>\n"
       << "<video src=\"" << esc << "intro.webm\"/>\n"
       << "<use symbol=\"Badge\" id=\"badge\"></use>\n"
       << "<button id=\"fire\" style=\"font-size: 48px\">Fire</button>\n"
       << "</div>\n</body>\n</xt>\n";
    return source;
}

/// T-273c: every occurrence of either marker in the bytes (and the names) of every file under
/// `out`.
std::vector<LeakHit> findLeaks(const std::string& out) {
    std::vector<LeakHit> hits;
    const fs::path root = compile::pathFromUtf8(out);
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        const std::string rel = compile::genericPathToUtf8(it->path().lexically_relative(root));
        const bool regular = it->is_regular_file(ec);
        const std::vector<uint8_t> bytes =
            regular ? readFileBytes(compile::pathToUtf8(it->path())) : std::vector<uint8_t>();
        const std::string text(bytes.begin(), bytes.end());
        auto printable = [&](size_t i) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            return c >= 0x20 && c != '"' && c != 0x7F;
        };
        for (const std::string marker : {kLeakMarker, kProjectMarker}) {
            if (rel.find(marker) != std::string::npos) {
                hits.push_back(LeakHit{rel, "(the file name)"});
            }
            for (size_t at = text.find(marker); at != std::string::npos; at = text.find(marker, at + 1)) {
                size_t begin = at;
                size_t stop = at + marker.size();
                while (begin > 0 && printable(begin - 1)) {
                    --begin;
                }
                while (stop < text.size() && printable(stop)) {
                    ++stop;
                }
                hits.push_back(LeakHit{rel, text.substr(begin, stop - begin)});
            }
        }
    }
    return hits;
}

/// T-273d: the warning pack gives for a `<video src>` outside the project, `path` as written.
std::string videoOutsideWarning(const std::string& path) {
    return "video \"" + path +
           "\" is outside the project; a bundle does not carry video files, so the published UI "
           "names it by this path";
}

/// T-273d: the number of "video ... is outside the project" warnings in `report`.
int countVideoWarnings(const PackReport& report) {
    int count = 0;
    for (const compile::Diagnostic& d : report.diagnostics) {
        if (d.message.rfind("video \"", 0) == 0 &&
            d.message.find("\" is outside the project") != std::string::npos) {
            ++count;
        }
    }
    return count;
}

/// T-273d: true when some diagnostic of `report` says exactly `message`.
bool hasMessage(const PackReport& report, const std::string& message) {
    for (const compile::Diagnostic& d : report.diagnostics) {
        if (d.message == message) {
            return true;
        }
    }
    return false;
}

/// T-273c: packs the leak project (`options` decides full, `--ui` or `--embed`) and appends every
/// marker hit kAllowedLeaks does not allow to `leaks` (one line: `[<run>] <file>: <text>; ...`);
/// `used` collects the allow-list entries that matched.
void collectLeaks(const char* run, const std::string& source, const PackOptions& options,
                  std::set<std::string>& used, std::string& leaks) {
    const PackReport report = pack(loadProject(source), options);
    std::string diagnostics;
    for (const compile::Diagnostic& d : report.diagnostics) {
        diagnostics += d.file + ": " + d.message + "\n";
    }
    INFO("diagnostics:\n", diagnostics);
    REQUIRE(report.ok);
    CHECK(diagnostics.find("missing.png\" not found") != std::string::npos);  // Still reported.
    // T-273d: one warning per outside video, by the path as the document writes it.
    const std::string abs = (fs::path(source).parent_path() / kLeakMarker).generic_string() + "/";
    const std::string esc = std::string("../../") + kLeakMarker + "/";
    CHECK(countVideoWarnings(report) == 2);
    CHECK(hasMessage(report, videoOutsideWarning(abs + "clip.webm")));
    CHECK(hasMessage(report, videoOutsideWarning(esc + "intro.webm")));
    for (const LeakHit& hit : findLeaks(options.outDir)) {
        const AllowedLeak* allowed = nullptr;
        for (const AllowedLeak& entry : kAllowedLeaks) {
            if (hit.context.find(entry.file) != std::string::npos) {
                allowed = &entry;
            }
        }
        if (allowed != nullptr) {
            used.insert(allowed->file);
        } else {
            leaks += std::string("[") + run + "] " + hit.file + ": " + hit.context + "; ";
        }
    }
}

}  // namespace

TEST_CASE("pack: no outside path in any published file, full, --ui and --embed (T-273c)") {
    std::set<std::string> used;
    std::string leaks;
    const std::string source = writeLeakProject(xt::test::tempDir() / "xt_pack_leak");
    PackOptions options;
    options.thumbnails = false;
    options.outDir = outDir("leak_full");
    collectLeaks("full", source, options, used, leaks);

    // `--ui`: a full run with a hud that names nothing outside, then `--ui hud` with the leak hud.
    options.outDir = outDir("leak_ui");
    const std::string hud = source + "/hud/hud.xt";
    std::string saved;
    {
        std::ifstream in(hud, std::ios::binary);
        saved.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::ofstream(hud, std::ios::binary | std::ios::trunc)
        << "<xt>\n<body>\n<div id=\"panel\"><img src=\"img/a.png\"/></div>\n</body>\n</xt>\n";
    REQUIRE(pack(loadProject(source), options).ok);
    std::ofstream(hud, std::ios::binary | std::ios::trunc) << saved;
    PackOptions incremental = options;
    incremental.onlyUis = {"hud"};
    collectLeaks("--ui", source, incremental, used, leaks);

    options.outDir = outDir("leak_embed");
    options.embed = true;
    collectLeaks("--embed", source, options, used, leaks);

    if (!leaks.empty()) {
        FAIL_CHECK("machine paths in published files: " << leaks);
    }
    for (const std::string& file : used) {
        MESSAGE("allowed leak still present: " << file);
    }
}

TEST_CASE("pack: a video outside the project warns once per path and fails under --strict (T-273d)") {
    const std::string source = copiedSample("t273d_video");
    const fs::path outside = xt::test::tempDir() / "xt_t273d_outside";
    std::error_code ec;
    fs::create_directories(outside, ec);
    std::ofstream(outside / "clip.webm", std::ios::binary | std::ios::trunc) << "WEBM";
    const std::string clip = outside.generic_string() + "/clip.webm";
    // The same outside path twice in the hud (with an id, and without one: its engine://video:<src>
    // key holds the path too) and once in the pause menu; an inside video in the hud.
    std::ofstream(source + "/hud/hud.xt", std::ios::binary | std::ios::trunc)
        << "<xt>\n<body>\n<div id=\"panel\">\n<video id=\"clip\" src=\"" << clip << "\"/>\n"
        << "<video src=\"" << clip << "\"/>\n<video src=\"img/inside.webm\"/>\n</div>\n</body>\n</xt>\n";
    std::ofstream(source + "/menus/pause.xt", std::ios::binary | std::ios::trunc)
        << "<xt>\n<body>\n<div id=\"pausePanel\"><video src=\"" << clip << "\"/></div>\n</body>\n</xt>\n";

    PackOptions options;
    options.thumbnails = false;
    options.outDir = outDir("t273d_video");
    const PackReport report = pack(loadProject(source), options);
    std::string diagnostics;
    for (const compile::Diagnostic& d : report.diagnostics) {
        diagnostics += d.file + ": " + d.message + "\n";
    }
    INFO("diagnostics:\n", diagnostics);
    REQUIRE(report.ok);
    CHECK(countVideoWarnings(report) == 1);
    CHECK(hasMessage(report, videoOutsideWarning(clip)));
    CHECK(diagnostics.find("inside.webm") == std::string::npos);

    // --strict: the warning is an error, as the other publish warnings are.
    options.outDir = outDir("t273d_video_strict");
    options.strict = true;
    const PackReport strict = pack(loadProject(source), options);
    CHECK_FALSE(strict.ok);
    CHECK(hasMessage(strict, videoOutsideWarning(clip)));
}

TEST_CASE("pack: a video inside the project gives no outside warning (T-273d)") {
    const std::string source = copiedSample("t273d_video_inside");
    std::ofstream(source + "/hud/hud.xt", std::ios::binary | std::ios::trunc)
        << "<xt>\n<body>\n<div id=\"panel\"><video id=\"v\" src=\"img/inside.webm\"/>"
           "<video src=\"../shared/intro.webm\"/></div>\n</body>\n</xt>\n";
    PackOptions options;
    options.thumbnails = false;
    options.strict = true;
    options.outDir = outDir("t273d_video_inside");
    const PackReport report = pack(loadProject(source), options);
    CHECK(report.ok);
    CHECK(countVideoWarnings(report) == 0);
}

namespace {

/// T-273d: every `assets[].sources` entry of the bundle at `out`.
std::set<std::string> manifestSources(const std::string& out) {
    std::set<std::string> sources;
    std::string error;
    Result<std::unique_ptr<Bundle>> opened = Bundle::open(out, sw::decodeImageBytes, &error);
    REQUIRE_MESSAGE(opened.ok(), error);
    for (const AssetInfo& info : opened.value()->assetTable()) {
        sources.insert(info.sources.begin(), info.sources.end());
    }
    return sources;
}

/// T-273d: the image ASST names (kind 0) of `ui/hud.xtir` in the bundle at `out`.
std::vector<std::string> hudImageAssets(const std::string& out) {
    std::vector<std::string> names;
    const std::vector<uint8_t> ir = readFileBytes(out + "/ui/hud.xtir");
    REQUIRE_FALSE(ir.empty());
    Result<IrView> opened = IrView::open(IrBlob{ir.data(), ir.size()});
    REQUIRE(opened.ok());
    for (const IrAsset& asset : opened.value().assets().assets()) {
        if (asset.kind == 0) {
            names.emplace_back(opened.value().string(asset.name));
        }
    }
    return names;
}

#ifdef _WIN32  // Only the case-insensitive root test below uses it.
/// T-273d: swaps the case of every ASCII letter of `text`.
std::string swapCase(std::string text) {
    for (char& c : text) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        } else if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}
#endif

}  // namespace

#ifdef _WIN32
TEST_CASE(
    "pack: the manifest names an absolute reference into the project relative to it, root case "
    "aside (T-273d)") {
    const std::string source = copiedSample("t273d_case");
    writeSmallPng(compile::pathFromUtf8(source + "/hud/img/b.png"));
    std::ofstream(source + "/hud/hud.xt", std::ios::binary | std::ios::trunc)
        << "<xt>\n<body>\n<div id=\"panel\"><img src=\"" << source
        << "/hud/img/b.png\"/></div>\n</body>\n</xt>\n";
    PackOptions options;
    options.thumbnails = false;
    options.outDir = outDir("t273d_case");
    REQUIRE(pack(loadProject(swapCase(source)), options).ok);
    const std::set<std::string> sources = manifestSources(options.outDir);
    CHECK(sources.count("hud/img/b.png") == 1);
    CHECK(sources.count("b.png") == 0);
}
#endif

TEST_CASE("pack: a file in a sibling directory whose name starts with the project's is outside (T-273d)") {
    const std::string source = copiedSample("t273d_sib");
    const std::string sibling = source + "2";  // `.../xt_pack_src_t273d_sib2`
    std::error_code ec;
    fs::remove_all(compile::pathFromUtf8(sibling), ec);
    fs::create_directories(compile::pathFromUtf8(sibling + "/img"), ec);
    writeSmallPng(compile::pathFromUtf8(sibling + "/img/sib.png"));
    std::ofstream(source + "/hud/hud.xt", std::ios::binary | std::ios::trunc)
        << "<xt>\n<body>\n<div id=\"panel\"><img src=\"" << sibling
        << "/img/sib.png\"/></div>\n</body>\n</xt>\n";
    PackOptions options;
    options.thumbnails = false;
    options.outDir = outDir("t273d_sib");
    REQUIRE(pack(loadProject(source), options).ok);
    const std::set<std::string> sources = manifestSources(options.outDir);
    std::string all;
    for (const std::string& name : sources) {
        all += name + "; ";
    }
    INFO("sources: ", all);
    CHECK(sources.count("sib.png") == 1);
    CHECK(all.find("2/img") == std::string::npos);
}

TEST_CASE(
    "pack: the mesh image of an <img src=\"*.svg\"> file is packed, inside and outside the project "
    "(T-273d)") {
    const std::string source = copiedSample("t273d_mesh");
    const fs::path outside = xt::test::tempDir() / "xt_t273d_mesh_outside";
    std::error_code ec;
    fs::remove_all(outside, ec);
    fs::create_directories(outside, ec);
    fs::create_directories(compile::pathFromUtf8(source + "/hud/vec"), ec);
    const std::string svg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"8\" height=\"8\" viewBox=\"0 0 8 8\">"
        "<xt-mesh id=\"Face\" href=\"tex.png\" width=\"8\" height=\"8\"/></svg>\n";
    std::ofstream(compile::pathFromUtf8(source + "/hud/vec/rig.svg"), std::ios::binary | std::ios::trunc)
        << svg;
    writeSmallPng(compile::pathFromUtf8(source + "/hud/vec/tex.png"));
    std::ofstream(outside / "rig.svg", std::ios::binary | std::ios::trunc) << svg;
    writeSmallPng(outside / "tex.png");

    for (const bool inside : {true, false}) {
        CAPTURE(inside);
        const std::string src = inside ? std::string("vec/rig.svg") : outside.generic_string() + "/rig.svg";
        std::ofstream(source + "/hud/hud.xt", std::ios::binary | std::ios::trunc)
            << "<xt>\n<body>\n<div id=\"panel\"><img id=\"rig\" src=\"" << src
            << "\"/></div>\n</body>\n</xt>\n";
        PackOptions options;
        options.thumbnails = false;
        options.outDir = outDir(inside ? "t273d_mesh_in" : "t273d_mesh_out");
        const PackReport report = pack(loadProject(source), options);
        std::string diagnostics;
        for (const compile::Diagnostic& d : report.diagnostics) {
            diagnostics += d.file + ": " + d.message + "\n";
        }
        INFO("diagnostics:\n", diagnostics);
        REQUIRE(report.ok);
        const std::vector<std::string> names = hudImageAssets(options.outDir);
        REQUIRE(names.size() == 1);
        CHECK(names[0].rfind("assets/", 0) == 0);
        CHECK(fs::is_regular_file(compile::pathFromUtf8(options.outDir + "/" + names[0])));
        CHECK(diagnostics.find("not found") == std::string::npos);
        // The run time resolves it: the bundle's resolver decodes the image.
        std::string error;
        Result<std::unique_ptr<Bundle>> opened = Bundle::open(options.outDir, sw::decodeImageBytes, &error);
        REQUIRE_MESSAGE(opened.ok(), error);
        const std::vector<uint8_t> ir = readFileBytes(options.outDir + "/ui/hud.xtir");
        Result<IrView> view = IrView::open(IrBlob{ir.data(), ir.size()});
        REQUIRE(view.ok());
        AssetId meshImage = kInvalidAsset;
        for (size_t i = 0; i < view.value().assets().assets().size(); ++i) {
            if (view.value().assets().assets()[i].kind == 0) {
                meshImage = static_cast<AssetId>(i);
            }
        }
        REQUIRE(meshImage != kInvalidAsset);
        ImageData image;
        REQUIRE(opened.value()->assets().decodeImage(meshImage, view.value(), image));
        CHECK(image.width == 4);
        if (!inside) {
            CHECK(manifestSources(options.outDir).count("tex.png") == 1);
        }
    }
}

// T-342 (03 §3.13, 22 §22.15): pack() drops `editor-only` subtrees -- the IR has no such element,
// an image only it uses is not packed or listed, the thumbnail does not show it, and a flow edge
// still naming one of its ids is a warning.
TEST_CASE("pack: editor-only subtrees are not published (T-342)") {
    const std::string source = copiedSample("editor_only");
    const std::string out = outDir("editor_only");
    {
        // An opaque red reference image covering the whole 1280x720 UI, under the panel.
        std::vector<uint8_t> red(static_cast<size_t>(16) * 16 * 4, 0);
        for (size_t i = 0; i < red.size(); i += 4) {
            red[i] = 255;
            red[i + 3] = 255;
        }
        REQUIRE(sw::writePng(source + "/hud/img/ref.png", red.data(), 16, 16, 16 * 4));
        std::ofstream xt(compile::pathFromUtf8(source + "/hud/hud.xt"), std::ios::binary | std::ios::trunc);
        xt << "<xt>\n  <body>\n"
              "    <div id=\"ref\" editor-only style=\"position: absolute; left: 0px; top: 0px; width: "
              "1280px; height: 720px\">\n"
              "      <img id=\"shot\" src=\"img/ref.png\" style=\"width: 1280px; height: 720px\"/>\n"
              "    </div>\n"
              "    <div id=\"panel\">\n      <img src=\"img/a.png\"/>\n"
              "      <button id=\"fire\" style=\"font-size: 48px\">Fire</button>\n    </div>\n"
              "  </body>\n</xt>\n";
        std::ofstream flow(compile::pathFromUtf8(source + "/hud/hud.xtflow"),
                           std::ios::binary | std::ios::trunc);
        flow << R"({"nodes": [], "edges": [{"from": "#fire.Click", "to": "#shot.toggleClass", )"
                R"("params": {"class": "on"}}]})";
    }

    PackOptions options;
    options.outDir = out;
    const PackReport report = pack(loadProject(source), options);
    CHECK(report.ok);
    bool warned = false;
    for (const compile::Diagnostic& d : report.diagnostics) {
        CAPTURE(d.message);
        warned = warned || (d.level == compile::Diagnostic::Level::Warning &&
                            d.message.find("editor-only element 'shot' is referenced by the flow") !=
                                std::string::npos);
    }
    CHECK(warned);

    // The manifest lists only the panel's image; the reference was never packed.
    const std::vector<UiInfo> uis = openUis(out);
    const UiInfo* hud = findUi(uis, "hud");
    REQUIRE(hud != nullptr);
    CHECK(hud->assets.size() == 1);
    CHECK(fileCount(out + "/assets") == 1);

    NullTextShaper shaper;
    FileAssetResolver assets(out, sw::decodeImageBytes);
    Context ctx(ContextConfig{&shaper, &assets});
    Result<DocumentHandle> handle = ctx.loadDocumentFile(out + "/ui/hud.xtir");
    REQUIRE(handle.ok());
    const Document* doc = ctx.document(handle.value());
    REQUIRE(doc != nullptr);
    CHECK_FALSE(doc->byId("ref").valid());
    CHECK_FALSE(doc->byId("shot").valid());
    CHECK(doc->byId("fire").valid());
    CHECK(doc->ir().assets().assets().size() == 1);

    // The thumbnail's bottom-right quarter is empty: no red reference under the HUD.
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
    REQUIRE(sw::readPng(out + "/thumbs/hud.png", rgba, width, height));
    REQUIRE(width == 320);
    CHECK(textPixels(rgba, width, 160, 90, 160, 90) == 0);
}

// --- T-498b: the implied default token layer (Docs/design/49-symbol-sets.md 49.3 ruling) -------

namespace {

const fs::path kPresetGroupT498b =
    fs::path(XT_TEST_BUNDLE_DIR) / ".." / ".." / "Assets" / "Symbols" / "groups" / "broadsheet-2026";

void writeTextT498b(const fs::path& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

std::string readTextT498b(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// A fresh UI Project whose one UI (`main`) is `document`, with `shared/tokens.xts` = `tokens`.
UiProject projectT498b(const std::string& name, const std::string& document, const std::string& tokens) {
    const fs::path dir = xt::test::tempDir() / ("xt_pack_t498b_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    UiProject project;
    std::string error;
    REQUIRE_MESSAGE(createUiProject(compile::genericPathToUtf8(dir), "T498b", "t498b", "", &project,
                                    &error) == Status::kOk,
                    error);
    writeTextT498b(dir / "main" / "main.xt", document);
    writeTextT498b(dir / "shared" / "tokens.xts", tokens);
    Result<UiProject> loaded = UiProject::load(compile::genericPathToUtf8(dir), &error);
    REQUIRE_MESSAGE(loaded.ok(), error);
    return loaded.value();
}

std::string diagnosticsT498b(const PackReport& report) {
    std::string text;
    for (const compile::Diagnostic& d : report.diagnostics) {
        text += d.file + ": " + d.message + "\n";
    }
    return text;
}

std::string tokenValueT498b(const Bundle& bundle, std::string_view name) {
    for (const TokenDef& token : bundle.tokens()) {
        if (token.name == name) {
            return token.value;
        }
    }
    return std::string();
}

constexpr const char* kButtonDocT498b =
    "<xt><head><link rel=\"symbols\" href=\"xt-standard:Button\"/></head>\n"
    "<body><use symbol=\"Button\" id=\"Go\" slot:label=\"Go\"/></body></xt>\n";

}  // namespace

TEST_CASE(
    "pack: the preset group is the implied token layer; the project declares only its overrides (T-498b)") {
    // No @font-face, no --tex-*, none of the 57 standard tokens but one override: the group supplies
    // the rest (tokens, fonts, textures) and the pack is strict-clean.
    const UiProject project =
        projectT498b("implied", kButtonDocT498b, ":root { --color-accent: #ff0000; }\n");
    PackOptions options;
    options.thumbnails = false;
    options.strict = true;
    options.outDir = outDir("t498b_implied");
    options.standardSymbolDir = compile::genericPathToUtf8(kPresetGroupT498b);
    const PackReport report = pack(project, options);
    REQUIRE_MESSAGE(report.ok, diagnosticsT498b(report));
    CHECK_MESSAGE(report.diagnostics.empty(), diagnosticsT498b(report));
    CHECK(report.impliedGroupDir == compile::genericPathToUtf8(kPresetGroupT498b.lexically_normal()));
    CHECK(report.standardSymbolsCopied.empty());  // An `xt-standard:` link copies nothing.

    std::string error;
    Result<std::unique_ptr<Bundle>> opened = Bundle::open(options.outDir, nullptr, &error);
    REQUIRE_MESSAGE(opened.ok(), error);
    const Bundle& bundle = *opened.value();
    CHECK(tokenValueT498b(bundle, "--color-accent") == "#ff0000");  // The project wins.
    CHECK(tokenValueT498b(bundle, "--tex-grain") == "url(\"textures/grain-paper.png\")");  // Implied.
    CHECK(tokenValueT498b(bundle, "--color-bg") == "#e6e2d6");
    CHECK(bundle.tokens().size() >= 57);
    // The group's theme file is the bundle's `night` theme, generated from its tokens only.
    REQUIRE(bundle.themes().size() == 1);
    CHECK(bundle.themes()[0].name == "night");
    CHECK(bundle.themes()[0].file == "themes/night.xts");
    const std::string night = readTextT498b(fs::path(options.outDir) / "themes" / "night.xts");
    CHECK(night.find("--color-bg: #262320;") != std::string::npos);
    CHECK(night.find("@font-face") == std::string::npos);
    // Button's fonts and texture come from the group through searchDirs / the sheet directory.
    size_t fonts = 0;
    size_t images = 0;
    for (const AssetInfo& asset : bundle.assetTable()) {
        if (asset.kind == "font") ++fonts;
        if (asset.kind == "image") ++images;
    }
    CHECK(fonts == 5);  // Every @font-face of the group's tokens.xts, as a project declaring them packed.
    CHECK(images >= 1);
    // Nothing was written into the project for it.
    const fs::path root = compile::pathFromUtf8(project.rootDir);
    CHECK_FALSE(fs::exists(root / "shared" / "fonts"));
    CHECK_FALSE(fs::exists(root / "shared" / "textures"));
    CHECK(readTextT498b(root / "shared" / "tokens.xts") == ":root { --color-accent: #ff0000; }\n");
}

TEST_CASE(
    "pack: a project theme of the same name merges over the implied theme; other names stay apart (T-498b)") {
    UiProject project = projectT498b("themes", kButtonDocT498b, ":root { --color-accent: #ff0000; }\n");
    const fs::path root = compile::pathFromUtf8(project.rootDir);
    writeTextT498b(root / "shared" / "themes" / "night.xts",
                   ":root { --color-accent: #00ff00; --mine: 1px; }\n");
    writeTextT498b(root / "shared" / "themes" / "dark.xts", "/* own */\n:root { --color-bg: #000000; }\n");
    PackOptions options;
    options.thumbnails = false;
    options.strict = true;
    options.outDir = outDir("t498b_themes");
    options.standardSymbolDir = compile::genericPathToUtf8(kPresetGroupT498b);
    const PackReport report = pack(project, options);
    REQUIRE_MESSAGE(report.ok, diagnosticsT498b(report));
    std::string error;
    Result<std::unique_ptr<Bundle>> opened = Bundle::open(options.outDir, nullptr, &error);
    REQUIRE_MESSAGE(opened.ok(), error);
    REQUIRE(opened.value()->themes().size() == 2);
    CHECK(opened.value()->themes()[0].name == "dark");
    CHECK(opened.value()->themes()[1].name == "night");
    // `dark` is the project's file as written; `night` is the implied theme with the project's
    // values in place and its extra token after them.
    CHECK(readTextT498b(fs::path(options.outDir) / "themes" / "dark.xts") ==
          "/* own */\n:root { --color-bg: #000000; }\n");
    const std::string night = readTextT498b(fs::path(options.outDir) / "themes" / "night.xts");
    CHECK(night.find("--color-accent: #00ff00;") != std::string::npos);
    CHECK(night.find("--color-accent: #d9d3c4;") == std::string::npos);
    CHECK(night.find("--color-bg: #262320;") != std::string::npos);
    CHECK(night.find("--mine: 1px;") != std::string::npos);
    CHECK(night.find("--mine") > night.find("--motion-normal"));
}

TEST_CASE(
    "pack: no standard symbol linked -> no implied layer; token.sameAsDefault warns on a repeated default "
    "(T-498b)") {
    {
        const UiProject project = projectT498b("plain", "<xt><body><div id=\"a\"/></body></xt>\n",
                                               ":root { --color-bg: #e6e2d6; }\n");
        PackOptions options;
        options.thumbnails = false;
        options.strict = true;
        options.outDir = outDir("t498b_plain");
        options.standardSymbolDir = compile::genericPathToUtf8(kPresetGroupT498b);
        const PackReport report = pack(project, options);
        REQUIRE_MESSAGE(report.ok, diagnosticsT498b(report));
        CHECK(report.impliedGroupDir.empty());
        CHECK(report.diagnostics.empty());  // No implied layer: nothing to repeat.
        std::string error;
        Result<std::unique_ptr<Bundle>> opened = Bundle::open(options.outDir, nullptr, &error);
        REQUIRE_MESSAGE(opened.ok(), error);
        CHECK(opened.value()->tokens().size() == 1);
        CHECK(opened.value()->themes().empty());
    }
    {
        // The same value as the group's default is the lint warning; the pack still succeeds.
        const UiProject project = projectT498b("same", kButtonDocT498b,
                                               ":root { --color-bg: #e6e2d6; --color-accent: #ff0000; }\n");
        PackOptions options;
        options.thumbnails = false;
        options.strict = true;
        options.outDir = outDir("t498b_same");
        options.standardSymbolDir = compile::genericPathToUtf8(kPresetGroupT498b);
        const PackReport report = pack(project, options);
        REQUIRE_MESSAGE(report.ok, diagnosticsT498b(report));
        REQUIRE(report.diagnostics.size() == 1);
        CHECK(report.diagnostics[0].level == compile::Diagnostic::Level::Warning);
        CHECK(report.diagnostics[0].file == "shared/tokens.xts");
        CHECK(report.diagnostics[0].message ==
              "token.sameAsDefault: --color-bg repeats the preset group's default; delete it to follow the "
              "group");
    }
}

TEST_CASE("pack: copyStandardSymbols off resolves the group but writes no symbol into the project (T-498b)") {
    const UiProject project =
        projectT498b("nocopy",
                     "<xt><head><link rel=\"symbols\" href=\"../shared/symbols/Button.xt\"/></head>\n"
                     "<body><use symbol=\"Button\" id=\"Go\" slot:label=\"Go\"/></body></xt>\n",
                     ":root { --color-accent: #ff0000; }\n");
    const fs::path root = compile::pathFromUtf8(project.rootDir);
    PackOptions options;
    options.thumbnails = false;
    options.outDir = outDir("t498b_nocopy");
    options.standardSymbolDir = compile::genericPathToUtf8(kPresetGroupT498b);
    options.copyStandardSymbols = false;
    const PackReport report = pack(project, options);
    CHECK_FALSE(report.ok);  // The link names a file that is not there (xtframe-view's T-263f note).
    CHECK(report.standardSymbolsCopied.empty());
    CHECK_FALSE(fs::exists(root / "shared" / "symbols" / "Button.xt"));
    CHECK(report.impliedGroupDir == compile::genericPathToUtf8(kPresetGroupT498b.lexically_normal()));
    // With the copy on, the same project packs and the file is pulled in.
    options.copyStandardSymbols = true;
    const PackReport again = pack(project, options);
    CHECK_MESSAGE(again.ok, diagnosticsT498b(again));
    CHECK(again.standardSymbolsCopied == std::vector<std::string>{"shared/symbols/Button.xt"});
}

// --- T-527: `--fonts bundled` (Docs/design/22-bundle.md §22.10.3) --------------------------------

namespace {

const fs::path kTestFontsT527 = fs::path(XT_TEST_BUNDLE_DIR) / ".." / "fonts";

/// A fresh UI Project whose one UI (`main`) is `document`; `withFont` copies
/// Tests/fonts/NotoSans-Regular.subset.ttf to `main/fonts/` for an `@font-face`.
UiProject projectT527(const std::string& name, const std::string& document, bool withFont = false) {
    const fs::path dir = xt::test::tempDir() / ("xt_pack_t527_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    std::string error;
    REQUIRE_MESSAGE(
        createUiProject(compile::genericPathToUtf8(dir), "T527", "t527", "", nullptr, &error) == Status::kOk,
        error);
    writeTextT498b(dir / "main" / "main.xt", document);
    if (withFont) {
        fs::create_directories(dir / "main" / "fonts", ec);
        fs::copy_file(kTestFontsT527 / "NotoSans-Regular.subset.ttf",
                      dir / "main" / "fonts" / "NotoSans-Regular.subset.ttf", ec);
        REQUIRE(ec.value() == 0);
    }
    Result<UiProject> loaded = UiProject::load(compile::genericPathToUtf8(dir), &error);
    REQUIRE_MESSAGE(loaded.ok(), error);
    return loaded.value();
}

/// The diagnostics whose message starts with "fonts: ", one per line.
std::string fontsDiagnosticsT527(const PackReport& report) {
    std::string text;
    for (const compile::Diagnostic& d : report.diagnostics) {
        if (d.message.rfind("fonts: ", 0) == 0) {
            text += std::string(d.level == compile::Diagnostic::Level::Error ? "error: " : "warning: ") +
                    d.message + "\n";
        }
    }
    return text;
}

constexpr const char* kPlainTextDocT527 =
    "<xt><head><style>\n.t { font-family: \"Nowhere Sans\"; }\n</style></head>\n"
    "<body><div class=\"t\">Hello</div></body></xt>\n";

std::string fontFaceDocT527(const std::string& text) {
    return "<xt><head><style>\n"
           "@font-face { font-family: \"Test Sans\"; src: \"fonts/NotoSans-Regular.subset.ttf\"; }\n"
           ".t { font-family: \"Test Sans\"; }\n</style></head>\n"
           "<body><div class=\"t\">" +
           text + "</div></body></xt>\n";
}

}  // namespace

TEST_CASE("pack --fonts bundled: text without any bundled font is an error; --fonts system is not (T-527)") {
    const UiProject project = projectT527("nofont", kPlainTextDocT527);
    PackOptions options;
    options.thumbnails = false;
    options.outDir = outDir("t527_nofont_system");
    const PackReport system = pack(project, options);
    CHECK_MESSAGE(system.ok, diagnosticsT498b(system));
    CHECK(fontsDiagnosticsT527(system).empty());
    CHECK(system.fontsErrors == 0);

    options.fontsMode = FontsMode::kBundled;
    options.outDir = outDir("t527_nofont_bundled");
    const PackReport bundled = pack(project, options);
    CHECK_FALSE(bundled.ok);
    CHECK(fontsDiagnosticsT527(bundled) ==
          "error: fonts: main has text but bundles no font (declare @font-face, or pack with --fonts "
          "system)\n");
    CHECK(bundled.fontsErrors == 1);
    CHECK(bundled.fontsFallbackWarnings == 0);
    CHECK(fs::exists(fs::path(options.outDir) / "manifest.json"));  // The bundle is still written.
}

TEST_CASE("pack --fonts bundled: a project @font-face covering all text is clean (T-527)") {
    const UiProject project = projectT527("covered", fontFaceDocT527("Hello"), /*withFont=*/true);
    PackOptions options;
    options.thumbnails = false;
    options.fontsMode = FontsMode::kBundled;
    options.outDir = outDir("t527_covered");
    const PackReport report = pack(project, options);
    REQUIRE_MESSAGE(report.ok, diagnosticsT498b(report));
    CHECK(fontsDiagnosticsT527(report).empty());
    CHECK(report.fontsErrors == 0);
    CHECK(report.fontsFallbackWarnings == 0);
}

TEST_CASE("pack --fonts bundled: a code point no bundled font has is listed (T-527)") {
    const UiProject project =
        projectT527("missing", fontFaceDocT527("Hello \xE4\xB8\xAD"), /*withFont=*/true);  // U+4E2D
    PackOptions options;
    options.thumbnails = false;
    options.fontsMode = FontsMode::kBundled;
    options.outDir = outDir("t527_missing");
    const PackReport report = pack(project, options);
    CHECK_FALSE(report.ok);
    CHECK(fontsDiagnosticsT527(report) == "error: fonts: main: U+4E2D has no glyph in any bundled font\n");
    CHECK(report.fontsErrors == 1);

    // Also with --font-subset: the check reads the subset fonts the bundle finally holds.
    options.fontSubset = true;
    options.outDir = outDir("t527_missing_subset");
    const PackReport subset = pack(project, options);
    CHECK(subset.fontsSubset == 1);
    CHECK(fontsDiagnosticsT527(subset) == "error: fonts: main: U+4E2D has no glyph in any bundled font\n");
}

TEST_CASE("pack --fonts bundled: the implied group's fonts are the last fallback, with one warning (T-527)") {
    const UiProject project = projectT527(
        "implied",
        "<xt><head><link rel=\"symbols\" href=\"xt-standard:Button\"/>\n"
        "<style>\n.t { font-family: \"Nowhere Sans\", sans-serif; }\n</style></head>\n"
        "<body><use symbol=\"Button\" id=\"Go\" slot:label=\"Go\"/><div class=\"t\">Hi</div></body></xt>\n");
    PackOptions options;
    options.thumbnails = false;
    options.fontsMode = FontsMode::kBundled;
    options.standardSymbolDir = compile::genericPathToUtf8(kPresetGroupT498b);
    options.outDir = outDir("t527_implied");
    const PackReport report = pack(project, options);
    REQUIRE_MESSAGE(report.ok, diagnosticsT498b(report));
    CHECK(fontsDiagnosticsT527(report) ==
          "warning: fonts: main: \"Nowhere Sans, sans-serif\" is not bundled; falling back to \"Noto Serif "
          "Display ExtraCondensed, Noto Serif\"\n");
    CHECK(report.fontsFallbackWarnings == 1);
    CHECK(report.fontsErrors == 0);

    std::string error;
    Result<std::unique_ptr<Bundle>> opened = Bundle::open(options.outDir, nullptr, &error);
    REQUIRE_MESSAGE(opened.ok(), error);
    const Bundle& bundle = *opened.value();
    size_t fonts = 0;
    for (const AssetInfo& asset : bundle.assetTable()) {
        fonts += asset.kind == "font" ? 1u : 0u;
    }
    CHECK(fonts == 5);  // The group's five @font-face files, each once.
    const std::span<const UiInfo> uis = bundle.uis();
    REQUIRE(uis.size() == 1);
    size_t uiFonts = 0;
    for (const std::string& file : uis[0].assets) {
        for (const AssetInfo& asset : bundle.assetTable()) {
            uiFonts += (asset.file == file && asset.kind == "font") ? 1u : 0u;
        }
    }
    CHECK(uiFonts == 5);
    // No chain declared: one "" chain is made (read from the manifest itself; Bundle::fontFallbacks()
    // lists language-tagged chains only).
    const nlohmann::json manifest =
        nlohmann::json::parse(readTextT498b(fs::path(options.outDir) / "manifest.json"));
    REQUIRE(manifest.contains("fonts"));
    CHECK(manifest["fonts"]["fallback"] ==
          nlohmann::json::parse(R"({"": ["Noto Serif Display ExtraCondensed", "Noto Serif"]})"));
}

TEST_CASE("pack: publish.fonts \"bundled\" is --fonts bundled; an unknown value fails the load (T-527)") {
    UiProject project = projectT527("publish", kPlainTextDocT527);
    project.publish.fonts = FontsMode::kBundled;
    std::string error;
    REQUIRE_MESSAGE(project.save(&error) == Status::kOk, error);
    Result<UiProject> reloaded = UiProject::load(project.rootDir, &error);
    REQUIRE_MESSAGE(reloaded.ok(), error);
    CHECK(reloaded.value().publish.fonts == FontsMode::kBundled);
    CHECK(readTextT498b(compile::pathFromUtf8(reloaded.value().path())).find("\"fonts\": \"bundled\"") !=
          std::string::npos);

    PackOptions options;  // fontsMode stays kSystem: the project's setting turns the check on.
    options.thumbnails = false;
    options.outDir = outDir("t527_publish");
    const PackReport report = pack(reloaded.value(), options);
    CHECK_FALSE(report.ok);
    CHECK(fontsDiagnosticsT527(report) ==
          "error: fonts: main has text but bundles no font (declare @font-face, or pack with --fonts "
          "system)\n");

    // The CLI's `--fonts system` clears the project's setting (main.cpp): the check is off again.
    UiProject systemCopy = reloaded.value();
    systemCopy.publish.fonts = FontsMode::kSystem;
    options.outDir = outDir("t527_publish_system");
    CHECK(pack(systemCopy, options).ok);

    std::string text = readTextT498b(compile::pathFromUtf8(reloaded.value().path()));
    const size_t at = text.find("\"bundled\"");
    REQUIRE(at != std::string::npos);
    text.replace(at, 9, "\"embedded\"");
    writeTextT498b(compile::pathFromUtf8(reloaded.value().path()), text);
    Result<UiProject> bad = UiProject::load(project.rootDir, &error);
    CHECK(bad.status() == Status::kInvalidArgument);
    CHECK(error.find("publish.fonts must be \"system\" or \"bundled\"") != std::string::npos);
}

// --- T-527b: the standard fonts (Docs/design/22-bundle.md §22.10.3 "standard fonts") -------------

namespace {

const fs::path kStandardFontsT527b = fs::path(XT_TEST_BUNDLE_DIR) / ".." / ".." / "Assets" / "Fonts";

/// The manifest `sources` of every font asset of the bundle at `outDir`, sorted.
std::vector<std::string> fontSourcesT527b(const std::string& outDir) {
    const nlohmann::json manifest = nlohmann::json::parse(readTextT498b(fs::path(outDir) / "manifest.json"));
    std::vector<std::string> sources;
    for (const nlohmann::json& asset : manifest["assets"]) {
        if (asset["kind"] == "font") {
            for (const nlohmann::json& source : asset["sources"]) {
                sources.push_back(source.get<std::string>());
            }
        }
    }
    std::sort(sources.begin(), sources.end());
    return sources;
}

constexpr const char* kNotoSansDocT527b =
    "<xt><head><style>\n.t { font-family: \"noto sans\", sans-serif; }\n</style></head>\n"
    "<body><div class=\"t\">Hello</div></body></xt>\n";

}  // namespace

TEST_CASE("pack --fonts bundled: a standard family named by the text is bundled, all its files (T-527b)") {
    const UiProject project = projectT527("standard", kNotoSansDocT527b);
    PackOptions options;
    options.thumbnails = false;
    options.fontsMode = FontsMode::kBundled;
    options.standardFontDir = compile::genericPathToUtf8(kStandardFontsT527b);
    options.outDir = outDir("t527b_standard");
    const PackReport report = pack(project, options);
    REQUIRE_MESSAGE(report.ok, diagnosticsT498b(report));
    CHECK(fontsDiagnosticsT527(report).empty());
    CHECK(report.fontsErrors == 0);
    CHECK(report.fontsFallbackWarnings == 0);
    CHECK(report.fontsCoverageWarnings == 0);
    CHECK(fontSourcesT527b(options.outDir) ==
          std::vector<std::string>{"fonts/NotoSans-Bold.ttf", "fonts/NotoSans-BoldItalic.ttf",
                                   "fonts/NotoSans-Italic.ttf", "fonts/NotoSans-Regular.ttf"});

    std::string error;
    Result<std::unique_ptr<Bundle>> opened = Bundle::open(options.outDir, nullptr, &error);
    REQUIRE_MESSAGE(opened.ok(), error);
    const std::span<const UiInfo> uis = opened.value()->uis();
    REQUIRE(uis.size() == 1);
    size_t uiFonts = 0;
    for (const std::string& file : uis[0].assets) {
        for (const AssetInfo& asset : opened.value()->assetTable()) {
            uiFonts += (asset.file == file && asset.kind == "font") ? 1u : 0u;
        }
    }
    CHECK(uiFonts == 4);

    // A text naming no bundled family falls back to the standard "Noto Sans", with one warning.
    const UiProject other = projectT527("standard_fallback", kPlainTextDocT527);
    options.outDir = outDir("t527b_standard_fallback");
    const PackReport fallback = pack(other, options);
    REQUIRE_MESSAGE(fallback.ok, diagnosticsT498b(fallback));
    CHECK(fontsDiagnosticsT527(fallback) ==
          "warning: fonts: main: \"Nowhere Sans\" is not bundled; falling back to \"Noto Sans\"\n");
    CHECK(fallback.fontsFallbackWarnings == 1);
    CHECK(fontSourcesT527b(options.outDir).size() == 4);
}

TEST_CASE(
    "pack --fonts bundled: code points Noto Sans lacks are covered by Noto Sans TC, with a warning "
    "(T-527b)") {
    const UiProject project =
        projectT527("cjk",
                    "<xt><head><style>\n.t { font-family: \"Noto Sans\"; }\n</style></head>\n"
                    "<body><div class=\"t\">Hello \xE4\xB8\xAD</div></body></xt>\n");  // U+4E2D
    PackOptions options;
    options.thumbnails = false;
    options.fontsMode = FontsMode::kBundled;
    options.standardFontDir = compile::genericPathToUtf8(kStandardFontsT527b);
    options.outDir = outDir("t527b_cjk");
    const PackReport report = pack(project, options);
    REQUIRE_MESSAGE(report.ok, diagnosticsT498b(report));
    CHECK(
        fontsDiagnosticsT527(report) ==
        "warning: fonts: main: 1 code points (U+4E2D) covered by standard font \"Noto Sans TC\", bundled\n");
    CHECK(report.fontsErrors == 0);
    CHECK(report.fontsCoverageWarnings == 1);
    CHECK(report.fontsFallbackWarnings == 0);
    const std::vector<std::string> sources = fontSourcesT527b(options.outDir);
    CHECK(sources.size() == 5);
    CHECK(std::find(sources.begin(), sources.end(), "fonts/NotoSansTC-Regular.ttf") != sources.end());

    // With --font-subset the standard fonts are subset too, and the coverage still holds.
    options.fontSubset = true;
    options.outDir = outDir("t527b_cjk_subset");
    const PackReport subset = pack(project, options);
    REQUIRE_MESSAGE(subset.ok, diagnosticsT498b(subset));
    CHECK(subset.fontsSubset == 5);
    CHECK(subset.fontsErrors == 0);
    CHECK(fs::exists(fs::path(options.outDir) / "fonts" / "NotoSansTC-Regular.subset.ttf"));
}

TEST_CASE("pack --fonts bundled: an empty standardFontDir keeps the T-527 error (T-527b)") {
    const UiProject project = projectT527("standard_off", kNotoSansDocT527b);
    PackOptions options;
    options.thumbnails = false;
    options.fontsMode = FontsMode::kBundled;
    options.outDir = outDir("t527b_standard_off");
    const PackReport report = pack(project, options);
    CHECK_FALSE(report.ok);
    CHECK(fontsDiagnosticsT527(report) ==
          "error: fonts: main has text but bundles no font (declare @font-face, or pack with --fonts "
          "system)\n");
    CHECK(report.fontsErrors == 1);
}
