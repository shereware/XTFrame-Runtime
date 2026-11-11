#include "golden_wgpu.h"

#include <algorithm>
#include <memory>
#include <vector>

#include "xt/command_list.h"
#include "xt/wgpu/wgpu_renderer.h"
#ifdef XT_GOLDEN_TEXT
#include "xt/text/ft_text_shaper.h"
#include "xt/text/glyph_atlas.h"
#endif

/// xt_golden's WebGPU backend (T-438): see golden_wgpu.h.

namespace xt::golden {

namespace {

struct WgpuSession {
    xt::wgpu::WgpuInit init;
    xt::wgpu::WgpuRenderer renderer;
    std::string adapter;
};

std::unique_ptr<WgpuSession> g_session;

/// A render target of the fixture's size: RENDER_ATTACHMENT for drawing, COPY_SRC for the readback.
struct Target {
    WGPUTexture texture = nullptr;
    WGPUTextureView view = nullptr;
    Target(WGPUDevice device, int w, int h) {
        WGPUTextureDescriptor d = WGPU_TEXTURE_DESCRIPTOR_INIT;
        d.usage =
            WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc | WGPUTextureUsage_TextureBinding;
        d.size = WGPUExtent3D{static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
        d.format = WGPUTextureFormat_RGBA8Unorm;
        texture = wgpuDeviceCreateTexture(device, &d);
        view = wgpuTextureCreateView(texture, nullptr);
    }
    ~Target() {
        wgpuTextureViewRelease(view);
        wgpuTextureDestroy(texture);
        wgpuTextureRelease(texture);
    }
    Target(const Target&) = delete;
    Target& operator=(const Target&) = delete;
};

std::vector<uint8_t> drawWgpu(int width, int height, const xt::Color& clear, const xt::CommandList& cmds,
                              xt::TextShaper* shaper, xt::AssetResolver* assets, const xt::IrView* ir) {
    WgpuSession& s = *g_session;
    // One renderer for the run (pipelines compile once); each fixture starts with no image textures.
    xt::wgpu::WgpuRenderer& renderer = s.renderer;
    renderer.invalidateImages();
    Target target(s.init.device, width, height);
    renderer.resize(width, height);
    // The SW backend clears to the premultiplied colour (SwRenderer::clear).
    renderer.setClearColor(clear.r * clear.a, clear.g * clear.a, clear.b * clear.a, clear.a);
#ifdef XT_GOLDEN_TEXT
    // A glyph atlas, as every GPU host has one (text effects are atlas-only, 10 section 10.8). The
    // UI fixtures' shaper is always an FtTextShaper (golden_ui.cpp); the others pass none.
    std::unique_ptr<xt::text::GlyphAtlas> atlas;
    if (shaper != nullptr) {
        atlas = std::make_unique<xt::text::GlyphAtlas>(*static_cast<xt::text::FtTextShaper*>(shaper));
    }
    renderer.setGlyphAtlas(atlas.get());
#endif
    renderer.render(cmds, shaper, assets, target.view, ir);
#ifdef XT_GOLDEN_TEXT
    renderer.setGlyphAtlas(nullptr);
#endif
    if (renderer.lastError()[0] != '\0') {
        report("xt_golden: wgpu render: %s\n", renderer.lastError());
    }
    if (renderer.lastWarning()[0] != '\0') {
        report("xt_golden: wgpu warning: %s\n", renderer.lastWarning());
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    if (!renderer.readback(target.texture, pixels)) {
        report("xt_golden: wgpu readback failed: %s\n", renderer.lastError());
        return {};
    }
    return pixels;
}

}  // namespace

bool makeWgpuBackend(FrameBackend& out, std::string& err) {
    g_session = std::make_unique<WgpuSession>();
    g_session->init = xt::wgpu::WgpuInit::headless();
    if (g_session->init.device == nullptr) {
        err = "no WebGPU adapter (" + g_session->init.error + ")";
        g_session.reset();
        return false;
    }
    if (!g_session->renderer.init(g_session->init)) {
        err = g_session->renderer.lastError();
        releaseWgpuBackend();
        return false;
    }
    g_session->adapter = g_session->renderer.adapterName();
    out = &drawWgpu;
    return true;
}

std::string wgpuAdapterName() { return g_session ? g_session->adapter : std::string(); }

int runWgpuOrderPairs(const std::vector<UiFixture>& fixtures, const std::string& uiDir, std::string& err) {
    FrameBackend backend;
    // A fresh renderer (and device): nothing of an earlier fixture survives.
    auto fresh = [&backend, &err]() {
        releaseWgpuBackend();
        return makeWgpuBackend(backend, err);
    };
    const UiBackend ui = [&backend](const UiFixture& f, const xt::CommandList& cmds, xt::TextShaper* shaper,
                                    xt::AssetResolver* assets, const xt::IrView* ir) {
        return backend(f.width, f.height, f.clearColor, cmds, shaper, assets, ir);
    };
    std::vector<std::vector<uint8_t>> alone;
    alone.reserve(fixtures.size());
    for (const UiFixture& fx : fixtures) {
        if (!fresh()) {
            return -1;
        }
        alone.push_back(renderUiFixture(fx, uiDir, ui));
    }
    int failed = 0;
    for (size_t a = 0; a < fixtures.size(); ++a) {
        for (size_t b = 0; b < fixtures.size(); ++b) {
            if (a == b) {
                continue;
            }
            if (!fresh()) {
                return -1;
            }
            (void)renderUiFixture(fixtures[a], uiDir, ui);
            const std::vector<uint8_t> after = renderUiFixture(fixtures[b], uiDir, ui);
            size_t differ = 0;
            if (after.empty() || after.size() != alone[b].size()) {
                differ = std::max(after.size(), alone[b].size());
            } else {
                for (size_t i = 0; i < after.size(); ++i) {
                    differ += after[i] != alone[b][i] ? 1 : 0;
                }
            }
            if (differ == 0 && !after.empty()) {
                report("[PASS] order %s -> %s\n", fixtures[a].name.c_str(), fixtures[b].name.c_str());
            } else {
                report("[FAIL] order %s -> %s: %zu bytes differ from %s drawn alone\n",
                       fixtures[a].name.c_str(), fixtures[b].name.c_str(), differ, fixtures[b].name.c_str());
                ++failed;
            }
        }
    }
    return failed;
}

void releaseWgpuBackend() {
    if (!g_session) {
        return;
    }
    g_session->renderer.shutdown();
    g_session->init.release();
    g_session.reset();
}

}  // namespace xt::golden
