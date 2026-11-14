// T-703 (Docs/design/33-world-layers.md §33.13 items 1, 3, 4; Docs/tasks/T-703.md): gpu::
// WorldLayerBuilder against recording devices -- no GPU needed (the pixel checks are in
// Tests/wgpu/test_wgpu_world_layers.cpp and Tests/gl/test_gl_world_layers.cpp). The build status, a
// layer target with a stencil attachment only when the layer's frame uses the stencil, a device that
// implements only T-701's three layer functions, and the world-layers demo's numbers.

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "doctest.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/element_ref.h"
#include "xt/gpu/gpu_device.h"
#include "xt/gpu/world_layers.h"
#include "xt/text_shaper.h"

using namespace xt;
using namespace xt::gpu;

namespace {

/// Hands out handles and executes nothing; answers both draw capabilities like the wgpu and GL
/// devices. Records the layer targets: T-701's functions only when `legacy` (a host written before
/// T-703, which does not override acquireLayerTargetWithStencil).
class LayerDevice : public GpuDevice {
public:
    explicit LayerDevice(bool legacy) : m_legacy(legacy) {}
    struct Acquired {
        TextureHandle handle;
        uint32_t layerId;
        bool stencil;
    };
    std::vector<Acquired> acquired;
    std::vector<TextureHandle> released;
    uint32_t submits = 0;
    uint32_t legacyAcquires = 0;

    TextureHandle createTexture(const char*, uint16_t, uint16_t, TextureFormat, const void*) override {
        return m_next++;
    }
    void updateTexture(TextureHandle, uint16_t, uint16_t, uint16_t, uint16_t, const void*, size_t) override {}
    void destroyTexture(TextureHandle) override {}
    void submit(const GpuFrame&) override {}
    bool supportsCoverageDraws() const override { return true; }
    bool supportsBoxShadowShading() const override { return true; }

    TextureHandle acquireLayerTarget(uint32_t layerId, uint16_t w, uint16_t h) override {
        (void)w;
        (void)h;
        ++legacyAcquires;
        const TextureHandle t = m_next++;
        acquired.push_back(Acquired{t, layerId, true});
        return t;
    }
    TextureHandle acquireLayerTargetWithStencil(uint32_t layerId, uint16_t w, uint16_t h,
                                                bool stencil) override {
        if (m_legacy) {
            return GpuDevice::acquireLayerTargetWithStencil(layerId, w, h, stencil);  // the default
        }
        const TextureHandle t = m_next++;
        acquired.push_back(Acquired{t, layerId, stencil});
        return t;
    }
    void releaseLayerTarget(TextureHandle target) override { released.push_back(target); }
    void submitLayer(TextureHandle, const GpuFrame&) override { ++submits; }

private:
    bool m_legacy;
    TextureHandle m_next = 1;
};

/// A device of T-701's time that says nothing about world layers.
class PlainDevice : public GpuDevice {
public:
    TextureHandle createTexture(const char*, uint16_t, uint16_t, TextureFormat, const void*) override {
        return 1;
    }
    void updateTexture(TextureHandle, uint16_t, uint16_t, uint16_t, uint16_t, const void*, size_t) override {}
    void destroyTexture(TextureHandle) override {}
    void submit(const GpuFrame&) override {}
};

struct Ui {
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    Document* doc = nullptr;
    std::vector<uint8_t> ir;

    Ui(compile::CompileResult result, float w, float h, bool layers) {
        for (const compile::Diagnostic& d : result.diagnostics) {
            CAPTURE(d.message);
            CHECK(d.level != compile::Diagnostic::Level::Error);
        }
        REQUIRE(result.ok());
        ir = std::move(result.ir);
        Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{ir.data(), ir.size()}, false);
        REQUIRE(handle.ok());
        doc = ctx.document(handle.value());
        REQUIRE(doc != nullptr);
        ctx.setViewport(Rect{0, 0, w, h}, 1.0f);
        ctx.setWorldLayerOutput(layers);
        ctx.update(0.0f);
    }
    CommandList render() {
        CommandList list;
        ctx.render(list);
        return list;
    }
    uint32_t node(const char* id) {
        const ElementRef ref = doc->byId(id);
        REQUIRE(ref.valid());
        return ref.node;
    }
};

compile::CompileResult compileText(const std::string& css, const std::string& body) {
    const std::string text = "<xt><head><style>" + css + "</style></head><body>" + body + "</body></xt>";
    const compile::SourceFile file{"layers.xt", text};
    return compile::compileDocumentFromStrings({&file, 1}, "layers.xt", compile::CompileOptions{});
}

compile::CompileResult compileDemo() {
    return compile::compileDocument(std::string(XT_EXAMPLES_DIR) + "/demos/world-layers/stage/stage.xt",
                                    compile::CompileOptions{});
}

const char* kCss =
    "#root { position: absolute; left: 0px; top: 0px; width: 320px; height: 200px; "
    "background-color: #18222e; }"
    "#panel { position: absolute; left: 64px; top: 40px; width: 160px; height: 96px; "
    "background-color: #2f6fed; border-radius: 16px; world-layer: on; }"
    "#chip { position: absolute; left: -20px; top: 20px; width: 80px; height: 24px; "
    "background-color: #ffd34d; }";
const char* kBody = "<div id=\"root\"><div id=\"panel\"><div id=\"chip\"></div></div></div>";

/// What clips #panel's content to its rounded box: a path clip. With the edge fringe off (a 4x
/// target's setting) the builder draws it through the stencil (with the fringe on, a clip like this
/// one becomes a mask layer: T-663).
const char* kClip = "overflow: hidden";

const WorldLayer* layerOf(std::span<const WorldLayer> layers, uint32_t id) {
    for (const WorldLayer& layer : layers) {
        if (layer.id == id) return &layer;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("T-703: WorldLayerBuilder::status tells the reasons a build fails apart") {
    Ui ui(compileText(kCss, kBody), 320, 200, /*layers=*/true);
    WorldLayerBuilder builder;
    CHECK(builder.status() == WorldLayerBuilder::BuildStatus::Ok);
    CHECK_FALSE(builder.build(ui.render(), nullptr, nullptr, nullptr));
    CHECK(builder.status() == WorldLayerBuilder::BuildStatus::NotInitialized);

    // A device of T-701's time without layer targets: Unsupported (the text stays as it was).
    PlainDevice plain;
    builder.init(plain);
    CHECK_FALSE(builder.build(ui.render(), nullptr, nullptr, nullptr));
    CHECK(builder.status() == WorldLayerBuilder::BuildStatus::Unsupported);
    CHECK(std::string(builder.lastError()).find("not supported by the device") != std::string::npos);
    CHECK(builder.layers().empty());
    CHECK_FALSE(builder.build(ui.render(), nullptr, nullptr, nullptr));
    CHECK(builder.status() == WorldLayerBuilder::BuildStatus::Unsupported);
    CHECK(std::string(builder.lastError()) == "world layers: no layer target");
    builder.shutdown();

    LayerDevice device(/*legacy=*/false);
    builder.init(device);
    Ui off(compileText(kCss, kBody), 320, 200, /*layers=*/false);
    CHECK_FALSE(builder.build(off.render(), nullptr, nullptr, nullptr));
    CHECK(builder.status() == WorldLayerBuilder::BuildStatus::NoLayers);
    CHECK(builder.build(ui.render(), nullptr, nullptr, nullptr));
    CHECK(builder.status() == WorldLayerBuilder::BuildStatus::Ok);
    CHECK(std::string(builder.lastError()).empty());
    // `device` is declared after `builder` and destroyed first: release the targets while it lives.
    builder.shutdown();
}

TEST_CASE("T-703: a layer target has a stencil attachment only while its frame uses the stencil") {
    Ui ui(compileText(kCss, kBody), 320, 200, /*layers=*/true);
    LayerDevice device(/*legacy=*/false);
    WorldLayerBuilder builder;
    builder.init(device);
    builder.setEdgeFringe(false);
    const uint32_t panelId = ui.node("panel");
    REQUIRE(builder.build(ui.render(), nullptr, nullptr, nullptr));
    builder.submit();
    REQUIRE(builder.layers().size() == 2);
    REQUIRE(device.acquired.size() == 2);
    for (const LayerDevice::Acquired& a : device.acquired) {
        CHECK_FALSE(a.stencil);  // plain fills: nothing reads or writes the stencil
    }
    CHECK(builder.stats().stencilLayers == 0u);
    CHECK(builder.stats().targetsCreated == 2u);
    const WorldLayer* panel = layerOf(builder.layers(), panelId);
    REQUIRE(panel != nullptr);
    CHECK(panel->stencil == 0);
    const TextureHandle first = panel->texture;

    // #panel starts clipping its child to its rounded box: its frame now uses the stencil, so its
    // target is acquired anew (the old one released) and drawn; the base layer is not touched.
    CHECK(ui.doc->byId("panel").setStyle(kClip) == Status::kOk);
    ui.ctx.update(0.016f);
    REQUIRE(builder.build(ui.render(), nullptr, nullptr, nullptr));
    builder.submit();
    panel = layerOf(builder.layers(), panelId);
    REQUIRE(panel != nullptr);
    CHECK(panel->stencil == 1);
    CHECK(panel->changed == 1);
    CHECK(panel->texture != first);
    CHECK(builder.stats().built == 1u);
    CHECK(builder.stats().targetsCreated == 1u);
    CHECK(builder.stats().stencilLayers == 1u);
    CHECK(builder.stats().submitted == 1u);
    REQUIRE(device.acquired.size() == 3);
    CHECK(device.acquired.back().stencil);
    CHECK(device.acquired.back().layerId == panelId);
    REQUIRE(device.released.size() == 1);
    CHECK(device.released[0] == first);

    // Unchanged: nothing acquired, nothing drawn.
    ui.ctx.update(0.032f);
    REQUIRE(builder.build(ui.render(), nullptr, nullptr, nullptr));
    CHECK(builder.stats().built == 0u);
    CHECK(device.acquired.size() == 3);

    // And back without the clip: a target without stencil again.
    CHECK(ui.doc->byId("panel").setStyle("overflow: visible") == Status::kOk);
    ui.ctx.update(0.048f);
    REQUIRE(builder.build(ui.render(), nullptr, nullptr, nullptr));
    REQUIRE(device.acquired.size() == 4);
    CHECK_FALSE(device.acquired.back().stencil);
    CHECK(layerOf(builder.layers(), panelId)->stencil == 0);
    builder.shutdown();
}

TEST_CASE("T-703: a host device with only T-701's three layer functions keeps working") {
    // The default acquireLayerTargetWithStencil forwards to acquireLayerTarget: every target is the
    // device's one kind (with whatever stencil it always had); WorldLayer::stencil still says what
    // the frame needs.
    Ui ui(compileText(std::string(kCss) + "#panel { overflow: hidden; }", kBody), 320, 200, /*layers=*/true);
    LayerDevice device(/*legacy=*/true);
    WorldLayerBuilder builder;
    builder.init(device);
    builder.setEdgeFringe(false);
    REQUIRE(builder.build(ui.render(), nullptr, nullptr, nullptr));
    builder.submit();
    CHECK(builder.status() == WorldLayerBuilder::BuildStatus::Ok);
    REQUIRE(builder.layers().size() == 2);
    CHECK(device.legacyAcquires == 2u);
    CHECK(device.submits == 2u);
    CHECK(layerOf(builder.layers(), ui.node("panel"))->stencil == 1);
    CHECK(layerOf(builder.layers(), 0)->stencil == 0);
    for (const WorldLayer& layer : builder.layers()) {
        CHECK(layer.texture != 0);
    }
    builder.shutdown();
}

TEST_CASE("T-703: the world-layers demo -- layers asking for stencil, #Tags drawn once in a full loop") {
    Ui ui(compileDemo(), 1280, 720, /*layers=*/true);
    LayerDevice device(/*legacy=*/false);
    WorldLayerBuilder builder;
    builder.init(device);
    REQUIRE(builder.build(ui.render(), nullptr, nullptr, nullptr));
    builder.submit();
    REQUIRE(builder.layers().size() == 6);
    uint32_t stencil = 0;
    for (const WorldLayer& layer : builder.layers()) {
        stencil += layer.stencil;
    }
    MESSAGE("world-layers demo: " << stencil << " of 6 layers ask for a stencil attachment");
    CHECK(stencil == builder.stats().stencilLayers);
    CHECK(stencil == 0u);

    const uint32_t tagsId = ui.node("Tags");
    const uint32_t ringsId = ui.node("Rings");
    uint32_t tagsRedraws = 1;  // the first build
    uint32_t ringsRedraws = 1;
    uint32_t othersRedraws = 0;
    for (int frame = 1; frame <= 360; ++frame) {  // 6 s at 60 frames a second: #Tags's whole loop
        ui.ctx.update(1.0f / 60.0f);
        REQUIRE(builder.build(ui.render(), nullptr, nullptr, nullptr));
        builder.submit();
        for (const WorldLayer& layer : builder.layers()) {
            if (layer.id == tagsId) {
                tagsRedraws += layer.changed;
            } else if (layer.id == ringsId) {
                ringsRedraws += layer.changed;
            } else {
                othersRedraws += layer.changed;
            }
        }
    }
    MESSAGE("#Tags drawn " << tagsRedraws << " time(s), #Rings " << ringsRedraws << " in 361 frames");
    CHECK(tagsRedraws == 1u);
    CHECK(ringsRedraws == 361u);
    CHECK(othersRedraws == 0u);
    builder.shutdown();
}

TEST_CASE("T-706: the world-layers demo -- #Rings's target is acquired twice in 600 frames, not five times") {
    // Docs/tasks/T-706.md: the turning #Rings switched between 480 and 512 four times in 600 frames
    // (xtframe-view --layers), each switch a new target (5 in all). 33 §33.14 item 1: the previous
    // rectangle is kept while the content fits in it -- the first target and the one for the first
    // growth to 512 (frame 78), which is then kept.
    Ui ui(compileDemo(), 1280, 720, /*layers=*/true);
    LayerDevice device(/*legacy=*/false);
    WorldLayerBuilder builder;
    builder.init(device);
    const uint32_t ringsId = ui.node("Rings");
    const auto ringsAcquired = [&] {
        uint32_t n = 0;
        for (const LayerDevice::Acquired& a : device.acquired) {
            n += a.layerId == ringsId ? 1u : 0u;
        }
        return n;
    };
    REQUIRE(builder.build(ui.render(), nullptr, nullptr, nullptr));
    builder.submit();
    CHECK(ringsAcquired() == 1u);
    uint32_t ringsRedraws = 1;
    for (int frame = 1; frame < 600; ++frame) {
        ui.ctx.update(1.0f / 60.0f);
        REQUIRE(builder.build(ui.render(), nullptr, nullptr, nullptr));
        builder.submit();
        const WorldLayer* rings = layerOf(builder.layers(), ringsId);
        REQUIRE(rings != nullptr);
        ringsRedraws += rings->changed;
    }
    MESSAGE("#Rings: target acquired " << ringsAcquired() << " time(s), drawn " << ringsRedraws
                                       << " time(s) in 600 frames");
    CHECK(ringsAcquired() == 2u);
    CHECK(ringsRedraws == 600u);  // its content turns: drawn every frame, into the same target
    builder.shutdown();
}
