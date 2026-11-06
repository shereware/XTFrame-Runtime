#include "xt/wgpu/wgpu_window.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if defined(__APPLE__)
#include <objc/message.h>
#include <objc/runtime.h>
#endif

/// WgpuWindowSurface (T-636): the native window -> WGPUSurface code of Code/Backends' RendererWgpu
/// (T-439), moved here so the C ABI GPU target (50 §50.z) uses the same one.

namespace xt::wgpu {

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

#if defined(__APPLE__)
/// A new CAMetalLayer set as the layer of `nsWindow`'s content view (a layer-hosting view), through
/// the Objective-C runtime so this file stays C++. Null when the window has no content view or
/// QuartzCore is not loaded. Untested (no macOS machine, T-636).
void* metalLayerOfWindow(void* nsWindow) {
    using SendId = void* (*)(void*, SEL);
    using SendSetId = void (*)(void*, SEL, void*);
    using SendSetBool = void (*)(void*, SEL, signed char);
    const auto sendId = reinterpret_cast<SendId>(objc_msgSend);
    void* view = sendId(nsWindow, sel_registerName("contentView"));
    Class layerClass = objc_getClass("CAMetalLayer");
    if (view == nullptr || layerClass == nullptr) return nullptr;
    void* layer = sendId(reinterpret_cast<void*>(layerClass), sel_registerName("layer"));
    if (layer == nullptr) return nullptr;
    reinterpret_cast<SendSetId>(objc_msgSend)(view, sel_registerName("setLayer:"), layer);
    reinterpret_cast<SendSetBool>(objc_msgSend)(view, sel_registerName("setWantsLayer:"), 1);
    return layer;
}
#endif

}  // namespace

WGPUTexture createRenderTexture(WGPUDevice device, int width, int height, WGPUTextureFormat format,
                                const char* label) {
    if (device == nullptr || width <= 0 || height <= 0) return nullptr;
    WGPUTextureDescriptor d = WGPU_TEXTURE_DESCRIPTOR_INIT;
    if (label != nullptr) d.label = WGPUStringView{label, WGPU_STRLEN};
    d.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc | WGPUTextureUsage_TextureBinding;
    d.size = WGPUExtent3D{static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    d.format = format;
    return wgpuDeviceCreateTexture(device, &d);
}

WgpuWindowSurface::~WgpuWindowSurface() { release(); }

bool WgpuWindowSurface::create(const WgpuInit& init, const WgpuNativeWindow& window, bool vsync,
                               std::string& error) {
    release();
    if (init.instance == nullptr || init.adapter == nullptr) {
        error = "no WebGPU instance / adapter for the window surface";
        return false;
    }
    WGPUSurfaceDescriptor desc = WGPU_SURFACE_DESCRIPTOR_INIT;
    WGPUSurfaceSourceWindowsHWND win32 = WGPU_SURFACE_SOURCE_WINDOWS_HWND_INIT;
    WGPUSurfaceSourceXlibWindow xlib = WGPU_SURFACE_SOURCE_XLIB_WINDOW_INIT;
    WGPUSurfaceSourceWaylandSurface wayland = WGPU_SURFACE_SOURCE_WAYLAND_SURFACE_INIT;
    WGPUSurfaceSourceMetalLayer metal = WGPU_SURFACE_SOURCE_METAL_LAYER_INIT;
    switch (window.kind) {
        case WgpuNativeWindow::Kind::Win32:
            win32.hinstance = window.display;
#if defined(_WIN32)
            if (win32.hinstance == nullptr) win32.hinstance = GetModuleHandleW(nullptr);
#endif
            win32.hwnd = window.window;
            desc.nextInChain = &win32.chain;
            break;
        case WgpuNativeWindow::Kind::X11:
            xlib.display = window.display;
            xlib.window = window.x11Window;
            desc.nextInChain = &xlib.chain;
            break;
        case WgpuNativeWindow::Kind::Wayland:
            wayland.display = window.display;
            wayland.surface = window.window;
            desc.nextInChain = &wayland.chain;
            break;
        case WgpuNativeWindow::Kind::MetalLayer:
            metal.layer = window.window;
            desc.nextInChain = &metal.chain;
            break;
        case WgpuNativeWindow::Kind::CocoaWindow:
#if defined(__APPLE__)
            metal.layer = window.window != nullptr ? metalLayerOfWindow(window.window) : nullptr;
            if (metal.layer == nullptr) {
                error = "the NSWindow gives no CAMetalLayer (no content view?)";
                return false;
            }
            desc.nextInChain = &metal.chain;
            break;
#else
            error = "a Cocoa window on a platform without Cocoa";
            return false;
#endif
        case WgpuNativeWindow::Kind::None:
            error = "the window has no native handle for a WebGPU surface";
            return false;
    }
    if (window.kind != WgpuNativeWindow::Kind::X11 && window.window == nullptr) {
        error = "the native window handle is null";
        return false;
    }
    m_surface = wgpuInstanceCreateSurface(init.instance, &desc);
    if (m_surface == nullptr) {
        error = "wgpuInstanceCreateSurface failed";
        return false;
    }
    WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
    if (wgpuSurfaceGetCapabilities(m_surface, init.adapter, &caps) != WGPUStatus_Success ||
        caps.formatCount == 0) {
        wgpuSurfaceCapabilitiesFreeMembers(caps);
        error = "the adapter cannot present to this window";
        release();
        return false;
    }
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
    return true;
}

void WgpuWindowSurface::configure(WGPUDevice device, int width, int height) {
    if (m_surface == nullptr || device == nullptr) return;
    m_device = device;
    m_width = width > 0 ? width : 1;
    m_height = height > 0 ? height : 1;
    WGPUSurfaceConfiguration config = WGPU_SURFACE_CONFIGURATION_INIT;
    config.device = device;
    config.format = m_format;
    config.usage = m_usage;
    config.width = static_cast<uint32_t>(m_width);
    config.height = static_cast<uint32_t>(m_height);
    config.alphaMode = WGPUCompositeAlphaMode_Auto;
    config.presentMode = m_presentMode;
    wgpuSurfaceConfigure(m_surface, &config);
}

bool WgpuWindowSurface::acquire() {
    releaseFrame();
    if (m_surface == nullptr || m_device == nullptr) return false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        WGPUSurfaceTexture st = WGPU_SURFACE_TEXTURE_INIT;
        wgpuSurfaceGetCurrentTexture(m_surface, &st);
        if (st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal ||
            st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
            m_frameTexture = st.texture;
            m_frameView = wgpuTextureCreateView(st.texture, nullptr);
            return true;
        }
        if (st.texture != nullptr) wgpuTextureRelease(st.texture);
        if (st.status != WGPUSurfaceGetCurrentTextureStatus_Outdated &&
            st.status != WGPUSurfaceGetCurrentTextureStatus_Lost) {
            return false;  // timeout / error: skip this frame
        }
        configure(m_device, m_width, m_height);
    }
    return false;
}

void WgpuWindowSurface::present() {
    if (m_surface != nullptr && m_frameView != nullptr) wgpuSurfacePresent(m_surface);
    releaseFrame();
}

void WgpuWindowSurface::releaseFrame() {
    if (m_frameView != nullptr) wgpuTextureViewRelease(m_frameView);
    if (m_frameTexture != nullptr) wgpuTextureRelease(m_frameTexture);
    m_frameView = nullptr;
    m_frameTexture = nullptr;
}

void WgpuWindowSurface::release() {
    releaseFrame();
    if (m_surface != nullptr) {
        if (m_device != nullptr) wgpuSurfaceUnconfigure(m_surface);
        wgpuSurfaceRelease(m_surface);
        m_surface = nullptr;
    }
    m_device = nullptr;
}

}  // namespace xt::wgpu
