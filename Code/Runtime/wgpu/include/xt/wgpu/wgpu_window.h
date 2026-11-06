#pragma once

#include <webgpu/webgpu.h>

#include <cstdint>
#include <string>

#include "xt/wgpu/wgpu_renderer.h"

/// A `WGPUSurface` made from a native window and presented to (Docs/design/10-backends.md §10.10,
/// T-439; shared by Code/Backends' RendererWgpu and the C ABI GPU target of 50 §50.z, T-636).
///
/// Format: the preferred non-sRGB 8-bit format (BGRA8Unorm, else RGBA8Unorm, else the first one
/// offered) -- the pipelines write premultiplied sRGB-encoded values as they are, like GL. Usage:
/// RENDER_ATTACHMENT, plus COPY_SRC when offered (so a frame can be read back before present).
/// Present mode: Fifo with vsync, else Immediate, else Mailbox, else Fifo. A texture that comes back
/// outdated or lost is reconfigured once and asked for again.

namespace xt::wgpu {

/// The native window a surface is made from.
struct WgpuNativeWindow {
    enum class Kind { None, Win32, X11, Wayland, MetalLayer, CocoaWindow };
    Kind kind = Kind::None;
    /// Win32: HINSTANCE (null = the process's own module); X11: Display*; Wayland: wl_display*.
    void* display = nullptr;
    /// Win32: HWND; Wayland: wl_surface*; MetalLayer: CAMetalLayer*; CocoaWindow: NSWindow* (a
    /// CAMetalLayer is made the layer of its content view; call on the main thread).
    void* window = nullptr;
    /// X11: the Window id.
    uint64_t x11Window = 0;
};

/// A single-sample `width` x `height` texture of `format` that can be drawn into, sampled and read
/// back (RENDER_ATTACHMENT | TEXTURE_BINDING | COPY_SRC): the offscreen target of a window-less
/// renderer. Null on failure. The caller destroys and releases it.
WGPUTexture createRenderTexture(WGPUDevice device, int width, int height, WGPUTextureFormat format,
                                const char* label = nullptr);

class WgpuWindowSurface {
public:
    WgpuWindowSurface() = default;
    ~WgpuWindowSurface();
    WgpuWindowSurface(const WgpuWindowSurface&) = delete;
    WgpuWindowSurface& operator=(const WgpuWindowSurface&) = delete;

    /// Creates the surface on `init.instance` and checks that `init.adapter` can present to it;
    /// picks the format, usage and present mode (file comment). Not configured yet. False and
    /// `error` when the window has no handle, the surface cannot be made or the adapter cannot
    /// present to it.
    bool create(const WgpuInit& init, const WgpuNativeWindow& window, bool vsync, std::string& error);
    /// (Re)configures the surface for `device` at `width` x `height` pixels (sizes <= 0: 1).
    void configure(WGPUDevice device, int width, int height);
    /// Acquires the frame's texture (and a view of it). False when no texture can be had this frame
    /// (minimized, timeout, error): the caller skips the frame.
    bool acquire();
    /// Presents the acquired texture (nothing when none) and releases it.
    void present();
    /// Releases the acquired texture without presenting it.
    void releaseFrame();
    /// Releases everything (unconfigures the surface). Safe to call twice.
    void release();

    bool valid() const { return m_surface != nullptr; }
    WGPUTextureFormat format() const { return m_format; }
    WGPUTextureUsage usage() const { return m_usage; }
    /// The acquired texture and its view (null outside acquire / present).
    WGPUTexture frameTexture() const { return m_frameTexture; }
    WGPUTextureView frameView() const { return m_frameView; }

private:
    WGPUSurface m_surface = nullptr;
    WGPUDevice m_device = nullptr;  ///< of the last configure (not owned)
    WGPUTextureFormat m_format = WGPUTextureFormat_BGRA8Unorm;
    WGPUTextureUsage m_usage = WGPUTextureUsage_RenderAttachment;
    WGPUPresentMode m_presentMode = WGPUPresentMode_Fifo;
    int m_width = 0, m_height = 0;
    WGPUTexture m_frameTexture = nullptr;
    WGPUTextureView m_frameView = nullptr;
};

}  // namespace xt::wgpu
