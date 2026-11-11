// T-687 (Docs/design/10-backends.md §10.13 rulings 2-7): end to end -- a UI Project packed with
// `publish.images.compress` none / bc7 / auto, opened as a bundle and drawn by wgpu and SW. The project
// (Tests/bundle/t687_project.h) has a padded image with alpha (302 x 300) drawn at full size and
// minified (so the stored mip levels are sampled), a background, and two images `auto` keeps as PNG.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "t687_project.h"
#include "test_temp.h"
#include "test_wgpu_draw_util.h"
#include "xt/bundle/bundle.h"
#include "xt/bundle/pack.h"
#include "xt/bundle/ui_project.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/sw/sw_png.h"
#include "xt/text_shaper.h"

using namespace xt;
using namespace xt::wgpu::test;

namespace {

namespace fs = std::filesystem;

/// Packs the T-687 project with `publishJson` into a fresh bundle directory; returns it.
std::string packT687(const std::string& name, const std::string& publishJson) {
    const fs::path src = xt::test::tempDir() / ("xt_t687_wgpu_src_" + name);
    const fs::path out = xt::test::tempDir() / ("xt_t687_wgpu_out_" + name);
    std::error_code ec;
    fs::remove_all(src, ec);
    fs::remove_all(out, ec);
    fs::create_directories(src, ec);
    REQUIRE(xt::test::t687::writeProject(src, publishJson));
    std::string error;
    Result<bundle::UiProject> project = bundle::UiProject::load(src.generic_string(), &error);
    REQUIRE_MESSAGE(project.ok(), error);
    bundle::PackOptions options;
    options.outDir = out.generic_string();
    options.thumbnails = false;
    options.encodeCache = false;
    const bundle::PackReport report = bundle::pack(project.value(), options);
    for (const compile::Diagnostic& d : report.diagnostics) {
        MESSAGE(d.file, ": ", d.message);
    }
    REQUIRE(report.ok);
    return out.generic_string();
}

}  // namespace

TEST_CASE("T-687 wgpu: packed none / bc7 / auto bundles draw like SW (padded, minified, alpha edges)") {
    constexpr int kW = 640, kH = 320;
    const Color clear{0.5f, 0.5f, 0.5f, 1};
    struct Case {
        const char* name;
        const char* publish;
        uint32_t compressed;
    };
    for (const Case c : {Case{"none", "", 0}, Case{"bc7", R"({ "images": { "compress": "bc7" } })", 4},
                         Case{"auto", R"({ "images": { "compress": "auto" } })", 2}}) {
        CAPTURE(c.name);
        const std::string out = packT687(c.name, c.publish);
        std::string error;
        Result<std::unique_ptr<bundle::Bundle>> opened =
            bundle::Bundle::open(out, sw::decodeImageBytes, &error);
        REQUIRE_MESSAGE(opened.ok(), error);
        bundle::Bundle& b = *opened.value();
        uint32_t dds = 0;
        for (const bundle::AssetInfo& info : b.assetTable()) {
            dds += info.format == "bc7" ? 1u : 0u;
        }
        CHECK(dds == c.compressed);
        NullTextShaper shaper;
        Context ctx(ContextConfig{&shaper, &b.assets()});
        Result<DocumentHandle> handle = bundle::loadUi(ctx, b, "m");
        REQUIRE(handle.ok());
        ctx.setViewport(Rect{0, 0, kW, kH}, 1.0f);
        ctx.update(0.0f);
        CommandList list;
        ctx.render(list);
        const IrView& ir = ctx.document(handle.value())->ir();
        Session s(kW, kH);
        const std::vector<uint8_t> gpu = s.render(list, clear, &b.assets(), &ir);
        CHECK_MESSAGE(s.error.empty(), s.error);
        const std::vector<uint8_t> swPx = renderSw(list, kW, kH, clear, &b.assets(), &ir);
        CHECK(matches(gpu, swPx, kW, kH, c.name));
    }
}
