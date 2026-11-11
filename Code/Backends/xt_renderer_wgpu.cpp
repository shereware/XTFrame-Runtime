// XTFrame reference backend -- MIT License (see LICENSE). Copy and edit (Code/Backends/README.md).
// Host API: webgpu.h of wgpu-native v29.0.1.1 (Code/SDKs/versions.cmake), through xt_wgpu.
#include "xt_renderer_wgpu.h"

#include <span>

#ifndef XT_BACKEND_NO_TEXT
#include "xt/text/glyph_atlas.h"
#endif

namespace xt::backend {

RendererWgpu::RendererWgpu() = default;

RendererWgpu::~RendererWgpu() { destroy(); }

bool RendererWgpu::initDevice(uint32_t sampleCount, WGPUTextureFormat format) {
    xt::wgpu::WgpuInit in = m_init;
    in.targetFormat = format;
    in.sampleCount = sampleCount == 4 ? 4u : 1u;
    if (!m_renderer.init(in)) {
        m_error = std::string("WgpuRenderer::init failed: ") + m_renderer.lastError();
        return false;
    }
#ifndef XT_BACKEND_NO_TEXT
    m_renderer.setGlyphAtlas(m_atlas.get());
#endif
    m_renderer.resize(m_width, m_height);
    return true;
}

bool RendererWgpu::create(const WgpuSurfaceSource& source, int width, int height, uint32_t sampleCount,
                          bool vsync) {
    destroy();
    m_init = xt::wgpu::WgpuInit::headless();
    if (m_init.device == nullptr) {
        m_error = "no WebGPU adapter (" + m_init.error + ")";
        destroy();
        return false;
    }
    xt::wgpu::WgpuNativeWindow native;
    switch (source.kind) {
        case WgpuSurfaceSource::Kind::Win32:
            native.kind = xt::wgpu::WgpuNativeWindow::Kind::Win32;
            break;
        case WgpuSurfaceSource::Kind::X11:
            native.kind = xt::wgpu::WgpuNativeWindow::Kind::X11;
            break;
        case WgpuSurfaceSource::Kind::Wayland:
            native.kind = xt::wgpu::WgpuNativeWindow::Kind::Wayland;
            break;
        case WgpuSurfaceSource::Kind::Metal:
            native.kind = xt::wgpu::WgpuNativeWindow::Kind::MetalLayer;
            break;
        case WgpuSurfaceSource::Kind::None:
            break;
    }
    native.display = source.display;
    native.window = source.window;
    native.x11Window = source.x11Window;
    if (!m_surface.create(m_init, native, vsync, m_error)) {
        destroy();
        return false;
    }
    m_format = m_surface.format();
    m_width = width > 0 ? width : 1;
    m_height = height > 0 ? height : 1;
    m_surface.configure(m_init.device, m_width, m_height);
    if (!initDevice(sampleCount, m_format)) {
        destroy();
        return false;
    }
    m_error.clear();
    return true;
}

bool RendererWgpu::createOffscreen(int width, int height, uint32_t sampleCount) {
    destroy();
    m_init = xt::wgpu::WgpuInit::headless();
    if (m_init.device == nullptr) {
        m_error = "no WebGPU adapter (" + m_init.error + ")";
        destroy();
        return false;
    }
    m_format = WGPUTextureFormat_RGBA8Unorm;
    m_width = width > 0 ? width : 1;
    m_height = height > 0 ? height : 1;
    resize(m_width, m_height);  // creates the texture
    if (!initDevice(sampleCount, m_format)) {
        destroy();
        return false;
    }
    m_error.clear();
    return true;
}

void RendererWgpu::destroy() {
    releaseFrame();
    m_renderer.framesInFlight().waitIdle(m_init.device);  // T-674: the GPU is done before anything is freed
    m_renderer.shutdown();
    releaseOffscreen();
    m_surface.release();
    m_init.release();
}

void RendererWgpu::releaseOffscreen() {
    if (m_offscreen != nullptr) {
        wgpuTextureDestroy(m_offscreen);
        wgpuTextureRelease(m_offscreen);
        m_offscreen = nullptr;
    }
}

void RendererWgpu::resize(int width, int height) {
    if (width <= 0 || height <= 0) return;  // minimized: keep the old size
    const bool changed = width != m_width || height != m_height;
    m_width = width;
    m_height = height;
    if (m_surface.valid()) {
        if (changed) m_surface.configure(m_init.device, m_width, m_height);
    } else if (m_init.device != nullptr && (changed || m_offscreen == nullptr)) {
        releaseOffscreen();
        m_offscreen = xt::wgpu::createRenderTexture(m_init.device, width, height, m_format);
    }
    if (m_renderer.device() != nullptr) m_renderer.resize(width, height);
}

void RendererWgpu::setClearColor(Color color) { m_clear = color; }

#ifndef XT_BACKEND_NO_TEXT
void RendererWgpu::setTextShaper(text::FtTextShaper* shaper) {
    m_renderer.setGlyphAtlas(nullptr);
    m_atlas.reset(shaper != nullptr ? new text::GlyphAtlas(*shaper) : nullptr);
    m_renderer.setGlyphAtlas(m_atlas.get());
}
#endif

void RendererWgpu::releaseFrame() {
    if (m_frameFromSurface) {
        m_surface.releaseFrame();
    } else if (m_frameView != nullptr) {
        wgpuTextureViewRelease(m_frameView);
    }
    m_frameView = nullptr;
    m_frameTexture = nullptr;
    m_frameFromSurface = false;
}

void RendererWgpu::beginFrame() {
    releaseFrame();
    // T-674 (Docs/design/50-c-abi.md §50.ac item 1): at most two frames in flight -- waits only while
    // the GPU is more than two frames behind (an offscreen loop has no present to hold it back).
    m_renderer.framesInFlight().beginFrame(m_init.device, m_init.queue);
    const std::string inflightWarning = m_renderer.framesInFlight().takeWarning();
    if (!inflightWarning.empty()) m_error = inflightWarning;
    if (!m_surface.valid()) {
        if (m_offscreen != nullptr) {
            m_frameTexture = m_offscreen;
            m_frameView = wgpuTextureCreateView(m_offscreen, nullptr);
        }
        return;
    }
    if (m_surface.acquire()) {  // timeout / error: this frame is skipped
        m_frameTexture = m_surface.frameTexture();
        m_frameView = m_surface.frameView();
        m_frameFromSurface = true;
    }
}

void RendererWgpu::render(const CommandList& list, TextShaper* shaper, AssetResolver* assets,
                          const IrView* ir, PerfTracker* perf, const DamageList* damage) {
    if (m_frameView == nullptr) return;
    m_renderer.setClearColor(m_clear.r * m_clear.a, m_clear.g * m_clear.a, m_clear.b * m_clear.a, m_clear.a);
    m_renderer.render(list, shaper, assets, m_frameView, ir, perf, damage);
    if (*m_renderer.lastError() != '\0') m_error = m_renderer.lastError();
}

bool RendererWgpu::readPixels(std::vector<uint8_t>& rgba, int& width, int& height) {
    // Offscreen: the texture keeps the last frame after present(); a surface texture only until then.
    WGPUTexture texture = m_frameTexture != nullptr ? m_frameTexture : m_offscreen;
    if (texture == nullptr || (m_frameFromSurface && (m_surface.usage() & WGPUTextureUsage_CopySrc) == 0)) {
        return false;
    }
    width = static_cast<int>(wgpuTextureGetWidth(texture));
    height = static_cast<int>(wgpuTextureGetHeight(texture));
    rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    if (!m_renderer.readback(texture, std::span<uint8_t>(rgba))) {
        m_error = m_renderer.lastError();
        return false;
    }
    return true;
}

void RendererWgpu::present() {
    m_renderer.framesInFlight().endFrame(m_init.queue);  // T-674
    if (m_frameFromSurface) {
        m_surface.present();  // releases the surface's frame
        m_frameView = nullptr;
        m_frameTexture = nullptr;
        m_frameFromSurface = false;
    }
    releaseFrame();
}

}  // namespace xt::backend
