// XTFrame reference backend -- MIT License (see LICENSE). Copy and edit (Code/Backends/README.md).
// Host API: webgpu.h of wgpu-native v29.0.1.1 (Code/SDKs/versions.cmake), through xt_wgpu.
#include "xt_renderer_wgpu.h"

#include <span>

#ifndef XT_BACKEND_NO_TEXT
#include "xt/text/glyph_atlas.h"
#endif

namespace xt::backend {

namespace {

bool hasFormat(const WGPUSurfaceCapabilities& caps, WGPUTextureFormat format) {
    for (size_t i = 0; i < caps.formatCount; ++i) {
        if (caps.formats[i] == format) return true;
    }
    return false;
}

bool hasPresentMode(const WGPUSurfaceCapabilities& caps, WGPUPresentMode mode) {
    for (size_t i = 0; i < caps.presentModeCount; ++i) {
        if (caps.presentModes[i] == mode) return true;
    }
    return false;
}

}  // namespace

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
    WGPUSurfaceDescriptor desc = WGPU_SURFACE_DESCRIPTOR_INIT;
    WGPUSurfaceSourceWindowsHWND win32 = WGPU_SURFACE_SOURCE_WINDOWS_HWND_INIT;
    WGPUSurfaceSourceXlibWindow xlib = WGPU_SURFACE_SOURCE_XLIB_WINDOW_INIT;
    WGPUSurfaceSourceWaylandSurface wayland = WGPU_SURFACE_SOURCE_WAYLAND_SURFACE_INIT;
    WGPUSurfaceSourceMetalLayer metal = WGPU_SURFACE_SOURCE_METAL_LAYER_INIT;
    switch (source.kind) {
        case WgpuSurfaceSource::Kind::Win32:
            win32.hinstance = source.display;
            win32.hwnd = source.window;
            desc.nextInChain = &win32.chain;
            break;
        case WgpuSurfaceSource::Kind::X11:
            xlib.display = source.display;
            xlib.window = source.x11Window;
            desc.nextInChain = &xlib.chain;
            break;
        case WgpuSurfaceSource::Kind::Wayland:
            wayland.display = source.display;
            wayland.surface = source.window;
            desc.nextInChain = &wayland.chain;
            break;
        case WgpuSurfaceSource::Kind::Metal:
            metal.layer = source.window;
            desc.nextInChain = &metal.chain;
            break;
        case WgpuSurfaceSource::Kind::None:
            m_error = "the window has no native handle for a WebGPU surface";
            destroy();
            return false;
    }
    m_surface = wgpuInstanceCreateSurface(m_init.instance, &desc);
    if (m_surface == nullptr) {
        m_error = "wgpuInstanceCreateSurface failed";
        destroy();
        return false;
    }
    WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
    if (wgpuSurfaceGetCapabilities(m_surface, m_init.adapter, &caps) != WGPUStatus_Success ||
        caps.formatCount == 0) {
        wgpuSurfaceCapabilitiesFreeMembers(caps);
        m_error = "the adapter cannot present to this window";
        destroy();
        return false;
    }
    // Non-sRGB 8-bit: the pipelines write premultiplied sRGB-encoded values as they are (like GL).
    m_format = hasFormat(caps, WGPUTextureFormat_BGRA8Unorm)   ? WGPUTextureFormat_BGRA8Unorm
               : hasFormat(caps, WGPUTextureFormat_RGBA8Unorm) ? WGPUTextureFormat_RGBA8Unorm
                                                               : caps.formats[0];
    m_usage = WGPUTextureUsage_RenderAttachment;
    if ((caps.usages & WGPUTextureUsage_CopySrc) != 0) m_usage |= WGPUTextureUsage_CopySrc;
    m_presentMode = WGPUPresentMode_Fifo;
    if (!vsync) {
        if (hasPresentMode(caps, WGPUPresentMode_Immediate)) {
            m_presentMode = WGPUPresentMode_Immediate;
        } else if (hasPresentMode(caps, WGPUPresentMode_Mailbox)) {
            m_presentMode = WGPUPresentMode_Mailbox;
        }
    }
    wgpuSurfaceCapabilitiesFreeMembers(caps);
    m_width = width > 0 ? width : 1;
    m_height = height > 0 ? height : 1;
    configureSurface();
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
    m_renderer.shutdown();
    releaseOffscreen();
    if (m_surface != nullptr) {
        wgpuSurfaceUnconfigure(m_surface);
        wgpuSurfaceRelease(m_surface);
        m_surface = nullptr;
    }
    m_init.release();
}

void RendererWgpu::releaseOffscreen() {
    if (m_offscreen != nullptr) {
        wgpuTextureDestroy(m_offscreen);
        wgpuTextureRelease(m_offscreen);
        m_offscreen = nullptr;
    }
}

void RendererWgpu::configureSurface() {
    if (m_surface == nullptr || m_init.device == nullptr) return;
    WGPUSurfaceConfiguration config = WGPU_SURFACE_CONFIGURATION_INIT;
    config.device = m_init.device;
    config.format = m_format;
    config.usage = m_usage;
    config.width = static_cast<uint32_t>(m_width);
    config.height = static_cast<uint32_t>(m_height);
    config.alphaMode = WGPUCompositeAlphaMode_Auto;
    config.presentMode = m_presentMode;
    wgpuSurfaceConfigure(m_surface, &config);
}

void RendererWgpu::resize(int width, int height) {
    if (width <= 0 || height <= 0) return;  // minimized: keep the old size
    const bool changed = width != m_width || height != m_height;
    m_width = width;
    m_height = height;
    if (m_surface != nullptr) {
        if (changed) configureSurface();
    } else if (m_init.device != nullptr && (changed || m_offscreen == nullptr)) {
        releaseOffscreen();
        WGPUTextureDescriptor d = WGPU_TEXTURE_DESCRIPTOR_INIT;
        d.usage =
            WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc | WGPUTextureUsage_TextureBinding;
        d.size = WGPUExtent3D{static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
        d.format = m_format;
        m_offscreen = wgpuDeviceCreateTexture(m_init.device, &d);
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
    if (m_frameView != nullptr) wgpuTextureViewRelease(m_frameView);
    if (m_frameTexture != nullptr && m_frameFromSurface) wgpuTextureRelease(m_frameTexture);
    m_frameView = nullptr;
    m_frameTexture = nullptr;
    m_frameFromSurface = false;
}

void RendererWgpu::beginFrame() {
    releaseFrame();
    if (m_surface == nullptr) {
        if (m_offscreen != nullptr) {
            m_frameTexture = m_offscreen;
            m_frameView = wgpuTextureCreateView(m_offscreen, nullptr);
        }
        return;
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        WGPUSurfaceTexture st = WGPU_SURFACE_TEXTURE_INIT;
        wgpuSurfaceGetCurrentTexture(m_surface, &st);
        if (st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal ||
            st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
            m_frameTexture = st.texture;
            m_frameFromSurface = true;
            m_frameView = wgpuTextureCreateView(st.texture, nullptr);
            return;
        }
        if (st.texture != nullptr) wgpuTextureRelease(st.texture);
        if (st.status != WGPUSurfaceGetCurrentTextureStatus_Outdated &&
            st.status != WGPUSurfaceGetCurrentTextureStatus_Lost) {
            return;  // timeout / error: skip this frame
        }
        configureSurface();
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
    if (texture == nullptr || (m_frameFromSurface && (m_usage & WGPUTextureUsage_CopySrc) == 0)) return false;
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
    if (m_surface != nullptr && m_frameFromSurface && m_frameView != nullptr) wgpuSurfacePresent(m_surface);
    releaseFrame();
}

}  // namespace xt::backend
