#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "compile_check.h"
#include "doctest.h"
#include "xt/asset_resolver.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/perf.h"
#include "xt/types.h"

/// T-746 (Docs/design/23-host-integration.md §23.ah): an external (`engine://`) texture's content
/// version and the query of how the last render used it. A name whose host never sets a version
/// behaves as before (it may change every frame: its document is never skipped); a versioned one
/// counts as still while the version stays -- the paint is skipped, the damage is empty -- and the
/// frame whose version (or binding) moved paints and reports the texture's draws as damage.

using namespace xt;
using namespace xt::compile;

namespace {

std::vector<uint8_t> compileOk(std::string_view style, std::string_view body) {
    CompileOptions options;
    options.emitMeta = false;
    const std::string text = "<xt><head><style>body { margin: 0; padding: 0; } " + std::string(style) +
                             "</style></head>\n<body id=\"Root\">" + std::string(body) + "</body></xt>";
    return test::compileMainOk(text, options);
}

std::string dump(const CommandList& list) {
    std::string out;
    dumpCommandList(list, out);
    return out;
}

struct Ui {
    FileAssetResolver assets{std::string(XT_TEST_XT_DIR), nullptr};
    std::unique_ptr<Context> ctx;
    std::vector<uint8_t> ir;
    DocumentHandle handle = 0;
    CommandList list;
    DamageList damage;

    Document& doc() { return *ctx->document(handle); }
    FrameStats frame() {
        ctx->update(1.0f / 60.0f);
        ctx->render(list, &damage);
        return ctx->perf().current();
    }
    /// `write` between this frame's update and render.
    FrameStats step(const std::function<void()>& write) {
        ctx->update(1.0f / 60.0f);
        write();
        ctx->render(list, &damage);
        return ctx->perf().current();
    }
    std::string fresh() {
        CommandList ref;
        ref.sources.push_back(CommandSource{&doc().ir(), doc().ir().loadSerial()});  // as render() names it
        doc().paint(ref);
        const std::string a = dump(ref), b = dump(list);
        if (a != b) {
            size_t i = 0;
            while (i < a.size() && i < b.size() && a[i] == b[i]) {
                ++i;
            }
            const size_t from = i > 200 ? i - 200 : 0;
            MESSAGE("fresh: " << a.substr(from, 400) << "\n---- list: " << b.substr(from, 400));
        }
        return a;
    }
    bool damageEmpty() const { return !damage.full && damage.rects.empty(); }
    /// The logical box of element `id`.
    Rect box(std::string_view id) {
        const LayoutBox& b = doc().arena().element(doc().byId(id).node).box;
        return Rect{b.x, b.y, b.width, b.height};
    }
};

/// `bind` runs on the resolver before the document is loaded (a host binds before the first update).
std::unique_ptr<Ui> open(std::string_view style, std::string_view body, const std::function<void(Ui&)>& bind,
                         float dpr = 1.0f) {
    auto ui = std::make_unique<Ui>();
    ui->ir = compileOk(style, body);
    bind(*ui);
    ContextConfig config;
    config.assets = &ui->assets;
    ui->ctx = std::make_unique<Context>(config);
    ui->ctx->setVerifyStaticSkip(false);  // these cases test the skip itself
    ui->ctx->perf().setEnabled(true);
    ui->ctx->setViewport(Rect{0, 0, 400, 300}, dpr);
    Result<DocumentHandle> handle = ui->ctx->loadDocument(IrBlob{ui->ir.data(), ui->ir.size()}, false);
    REQUIRE(handle.ok());
    ui->handle = handle.value();
    for (int i = 0; i < 3; ++i) {
        ui->frame();
    }
    return ui;
}

int settle(Ui& ui, int limit = 20) {
    for (int i = 1; i <= limit; ++i) {
        if (ui.frame().paintSkipped == 1) {
            return i;
        }
    }
    return -1;
}

/// True when the damage is partial and its rectangles' bounds contain `r` (device px).
bool damageCovers(const DamageList& damage, Rect r) {
    if (damage.full || damage.rects.empty()) {
        return false;
    }
    float x0 = damage.rects[0].x, y0 = damage.rects[0].y;
    float x1 = x0 + damage.rects[0].w, y1 = y0 + damage.rects[0].h;
    for (const Rect& d : damage.rects) {
        x0 = std::min(x0, d.x);
        y0 = std::min(y0, d.y);
        x1 = std::max(x1, d.x + d.w);
        y1 = std::max(y1, d.y + d.h);
    }
    return x0 <= r.x && y0 <= r.y && x1 >= r.x + r.w && y1 >= r.y + r.h;
}

constexpr std::string_view kImg =
    "#a { position: absolute; left: 10px; top: 20px; width: 64px; height: 32px; }";
constexpr std::string_view kImgBody = "<img id=\"a\" src=\"engine://scene\"/>";

}  // namespace

TEST_CASE("external texture version: a name without a version still paints every frame") {
    auto ui = open(kImg, kImgBody, [](Ui& u) { u.assets.setExternalTexture("scene", 7, 64, 32); });
    for (int i = 0; i < 4; ++i) {
        CHECK(ui->frame().paintSkipped == 0);
        CHECK(ui->list.clockDependent);
    }
    // A rebind on the resolver changes nothing about that (and reports no damage, as before).
    ui->assets.setExternalTexture("scene", 8, 64, 32);
    CHECK(ui->frame().paintSkipped == 0);
    CHECK(ui->damageEmpty());
    CHECK(dump(ui->list) == ui->fresh());
}

TEST_CASE("external texture version: an unchanged version lets the document be skipped with empty damage") {
    auto ui = open(kImg, kImgBody, [](Ui& u) {
        u.assets.setExternalTexture("scene", 7, 64, 32);
        u.assets.setExternalTextureVersion("scene", 1);
    });
    REQUIRE(settle(*ui) > 0);
    const std::string shown = ui->fresh();
    for (int i = 0; i < 3; ++i) {
        const FrameStats f = ui->frame();
        CHECK(f.paintSkipped == 1);
        CHECK(ui->damageEmpty());
        CHECK_FALSE(ui->list.clockDependent);
        CHECK(dump(ui->list) == shown);
    }
    // The version survives a rebind of the same name (and the rebind itself is a change).
    ui->assets.setExternalTexture("scene", 9, 64, 32);
    REQUIRE(ui->assets.externalTexture("scene") != nullptr);
    CHECK(ui->assets.externalTexture("scene")->hasVersion);
    CHECK(ui->assets.externalTexture("scene")->version == 1u);
}

TEST_CASE("external texture version: a raised version paints its frame and damages the texture's draw") {
    auto ui = open(kImg, kImgBody, [](Ui& u) {
        u.assets.setExternalTexture("scene", 7, 64, 32);
        u.assets.setExternalTextureVersion("scene", 1);
    });
    SUBCASE("raised between update and render") {
        REQUIRE(settle(*ui) > 0);
        const FrameStats f = ui->step([&] { ui->assets.setExternalTextureVersion("scene", 2); });
        CHECK(f.paintSkipped == 0);
        CHECK(damageCovers(ui->damage, Rect{10, 20, 64, 32}));
        CHECK(dump(ui->list) == ui->fresh());
        CHECK(ui->frame().paintSkipped == 1);  // still again
        CHECK(ui->damageEmpty());
    }
    SUBCASE("raised before update: the visual generation moves at that update") {
        REQUIRE(settle(*ui) > 0);
        const uint64_t before = ui->doc().visualGeneration();
        ui->assets.setExternalTextureVersion("scene", 2);
        const FrameStats f = ui->frame();
        CHECK(ui->doc().visualGeneration() != before);
        CHECK(f.paintSkipped == 0);
        CHECK(damageCovers(ui->damage, Rect{10, 20, 64, 32}));
        CHECK(ui->frame().paintSkipped == 1);
        CHECK(ui->damageEmpty());
    }
    SUBCASE("the same version set again is no change") {
        REQUIRE(settle(*ui) > 0);
        CHECK(ui->step([&] { ui->assets.setExternalTextureVersion("scene", 1); }).paintSkipped == 1);
        CHECK(ui->damageEmpty());
    }
    SUBCASE("a scaled output: the damage is in device pixels") {
        auto hi = open(
            kImg, kImgBody,
            [](Ui& u) {
                u.assets.setExternalTexture("scene", 7, 64, 32);
                u.assets.setExternalTextureVersion("scene", 1);
            },
            2.0f);
        REQUIRE(settle(*hi) > 0);
        CHECK(hi->step([&] { hi->assets.setExternalTextureVersion("scene", 5); }).paintSkipped == 0);
        CHECK(damageCovers(hi->damage, Rect{20, 40, 128, 64}));
    }
}

TEST_CASE("external texture version: a changed binding repaints whatever the version says") {
    auto ui = open(kImg, kImgBody, [](Ui& u) {
        u.assets.setExternalTexture("scene", 7, 64, 32);
        u.assets.setExternalTextureVersion("scene", 1);
    });
    SUBCASE("another texture handle") {
        REQUIRE(settle(*ui) > 0);
        CHECK(ui->step([&] { ui->assets.setExternalTexture("scene", 8, 64, 32); }).paintSkipped == 0);
        CHECK(damageCovers(ui->damage, Rect{10, 20, 64, 32}));
        CHECK(ui->frame().paintSkipped == 1);
    }
    SUBCASE("straight alpha") {
        REQUIRE(settle(*ui) > 0);
        CHECK(ui->step([&] { ui->assets.setExternalTexture("scene", 7, 64, 32, true); }).paintSkipped == 0);
        CHECK(damageCovers(ui->damage, Rect{10, 20, 64, 32}));
    }
    SUBCASE("T-806: a plane description, set, changed and removed") {
        AssetResolver::ExternalPlanes planes;
        planes.layout = AssetResolver::ExternalLayout::PackedYuv420;
        planes.textureWidth = 64;
        planes.textureHeight = 48;
        planes.y = {0, 0, 64, 32};
        planes.u = {0, 32, 32, 16};
        planes.v = {32, 32, 32, 16};
        REQUIRE(settle(*ui) > 0);
        CHECK(ui->step([&] { ui->assets.setExternalTexturePlanes("scene", planes); }).paintSkipped == 0);
        CHECK(damageCovers(ui->damage, Rect{10, 20, 64, 32}));
        REQUIRE(settle(*ui) > 0);
        planes.matrix = AssetResolver::ExternalMatrix::Bt601;
        CHECK(ui->step([&] { ui->assets.setExternalTexturePlanes("scene", planes); }).paintSkipped == 0);
        REQUIRE(settle(*ui) > 0);
        CHECK(ui->step([&] { ui->assets.setExternalTexturePlanes("scene", {}); }).paintSkipped == 0);
        CHECK(ui->frame().paintSkipped == 1);
    }
    SUBCASE("cleared: the version goes with the binding, the name changes every frame again") {
        REQUIRE(settle(*ui) > 0);
        ui->assets.clearExternalTexture("scene");
        CHECK(ui->frame().paintSkipped == 0);
        ui->assets.setExternalTexture("scene", 7, 64, 32);
        for (int i = 0; i < 3; ++i) {
            CHECK(ui->frame().paintSkipped == 0);
        }
    }
}

TEST_CASE("external texture version: an effect over a raised texture is damaged over the effect's reach") {
    auto ui = open(
        "#fx { position: absolute; left: 40px; top: 40px; width: 64px; height: 32px; "
        "effect: blur(8px); } #fx img { width: 64px; height: 32px; }",
        "<div id=\"fx\"><img src=\"engine://scene\"/></div>", [](Ui& u) {
            u.assets.setExternalTexture("scene", 7, 64, 32);
            u.assets.setExternalTextureVersion("scene", 1);
        });
    REQUIRE(settle(*ui) > 0);
    CHECK(ui->step([&] { ui->assets.setExternalTextureVersion("scene", 2); }).paintSkipped == 0);
    // The blur spreads the image's pixels past its box: the damage covers the effect's bounds.
    CHECK(damageCovers(ui->damage, Rect{36, 36, 72, 40}));
}

TEST_CASE("external texture version: a new binding size lays the users out again") {
    for (const bool versioned : {false, true}) {
        CAPTURE(versioned);
        auto ui = open(
            "#a { position: absolute; left: 0; top: 0; } #s { position: absolute; left: 0; "
            "top: 100px; width: 200px; font-size: 16px; }",
            "<img id=\"a\" src=\"engine://scene\"/>", [versioned](Ui& u) {
                u.assets.setExternalTexture("scene", 7, 64, 32);
                if (versioned) {
                    u.assets.setExternalTextureVersion("scene", 1);
                }
            });
        CHECK(ui->box("a").w == 64.0f);
        CHECK(ui->box("a").h == 32.0f);
        ui->assets.setExternalTexture("scene", 7, 100, 50);
        ui->frame();
        CHECK(ui->box("a").w == 100.0f);
        CHECK(ui->box("a").h == 50.0f);
        CHECK(dump(ui->list) == ui->fresh());
        // CPU pixels of another size do the same.
        ImageData pixels;
        pixels.width = 20;
        pixels.height = 10;
        pixels.rgba.assign(20 * 10 * 4, 255);
        ui->assets.setExternalPixels("scene", &pixels);
        ui->frame();
        CHECK(ui->box("a").w == 20.0f);
        CHECK(ui->box("a").h == 10.0f);
        ui->assets.setExternalPixels("scene", nullptr);
    }
}

TEST_CASE("external texture use: drawn and the largest device size, per name") {
    constexpr std::string_view kStyle =
        "#clip { position: absolute; left: 0; top: 0; width: 100px; height: 100px; overflow: hidden; } "
        ".i { position: absolute; top: 10px; width: 40px; height: 20px; } "
        "#in { left: 10px; } #half { left: 80px; } #out { left: 150px; } "
        "#none { left: 10px; top: 150px; width: 40px; height: 20px; position: absolute; display: none; } "
        "#zero { left: 60px; top: 150px; width: 40px; height: 20px; position: absolute; opacity: 0; } "
        "#off { left: 500px; top: 150px; width: 40px; height: 20px; position: absolute; } "
        "#two1 { left: 10px; top: 200px; width: 30px; height: 10px; position: absolute; } "
        "#two2 { left: 100px; top: 200px; width: 60px; height: 30px; position: absolute; }";
    constexpr std::string_view kBody =
        "<div id=\"clip\"><img id=\"in\" class=\"i\" src=\"engine://in\"/>"
        "<img id=\"half\" class=\"i\" src=\"engine://half\"/><img id=\"out\" class=\"i\" "
        "src=\"engine://out\"/>"
        "</div><img id=\"none\" src=\"engine://none\"/><img id=\"zero\" src=\"engine://zero\"/>"
        "<img id=\"off\" src=\"engine://off\"/><img id=\"two1\" src=\"engine://two\"/>"
        "<img id=\"two2\" src=\"engine://two\"/>";
    const auto bindAll = [](Ui& u) {
        uint32_t texture = 1;
        for (const char* name : {"in", "half", "out", "none", "zero", "off", "two"}) {
            u.assets.setExternalTexture(name, texture++, 64, 32);
        }
    };
    for (const float dpr : {1.0f, 2.0f}) {
        CAPTURE(dpr);
        auto ui = open(kStyle, kBody, bindAll, dpr);
        const auto use = [&](const char* name) { return ui->assets.externalTextureUse(name); };
        const int s = static_cast<int>(dpr);
        CHECK(use("in").drawn);
        CHECK(use("in").maxPixelWidth == 40 * s);
        CHECK(use("in").maxPixelHeight == 20 * s);
        CHECK(use("half").drawn);  // half of it is clipped away: the size is the whole draw's
        CHECK(use("half").maxPixelWidth == 40 * s);
        CHECK(use("half").maxPixelHeight == 20 * s);
        for (const char* hidden : {"out", "none", "zero", "off"}) {
            CAPTURE(hidden);
            CHECK_FALSE(use(hidden).drawn);
            CHECK(use(hidden).maxPixelWidth == 0);
            CHECK(use(hidden).maxPixelHeight == 0);
        }
        CHECK(use("two").drawn);
        CHECK(use("two").maxPixelWidth == 60 * s);
        CHECK(use("two").maxPixelHeight == 30 * s);
        CHECK_FALSE(use("never-bound").drawn);
    }
}

TEST_CASE("external texture use: reset by every paint, kept by a skipped frame") {
    auto ui = open("#a { position: absolute; left: 10px; top: 20px; width: 64px; height: 32px; }",
                   "<img id=\"a\" src=\"engine://scene\"/>", [](Ui& u) {
                       u.assets.setExternalTexture("scene", 7, 64, 32);
                       u.assets.setExternalTextureVersion("scene", 1);
                   });
    REQUIRE(settle(*ui) > 0);
    CHECK(ui->frame().paintSkipped == 1);
    CHECK(ui->assets.externalTextureUse("scene").drawn);
    CHECK(ui->assets.externalTextureUse("scene").maxPixelWidth == 64);
    // Hidden: the next paint no longer draws it.
    REQUIRE(ui->doc().byId("a").setStyle("display: none") == Status::kOk);
    ui->frame();
    CHECK_FALSE(ui->assets.externalTextureUse("scene").drawn);
    CHECK(ui->assets.externalTextureUse("scene").maxPixelWidth == 0);
}

TEST_CASE("external texture version: Context::needsFrame wakes for a raised version, not for a held one") {
    auto ui = open(kImg, kImgBody, [](Ui& u) {
        u.assets.setExternalTexture("scene", 7, 64, 32);
        u.assets.setExternalTextureVersion("scene", 1);
    });
    REQUIRE(settle(*ui) > 0);
    CHECK_FALSE(ui->ctx->needsFrame());
    ui->assets.setExternalTextureVersion("scene", 1);
    CHECK_FALSE(ui->ctx->needsFrame());
    ui->assets.setExternalTextureVersion("scene", 2);
    CHECK(ui->ctx->needsFrame());
    CHECK(ui->frame().paintSkipped == 0);
    CHECK_FALSE(ui->ctx->needsFrame());
    // Without a version the texture is clock-dependent: always a frame.
    auto plain = open(kImg, kImgBody, [](Ui& u) { u.assets.setExternalTexture("scene", 7, 64, 32); });
    CHECK(plain->ctx->needsFrame());
}
