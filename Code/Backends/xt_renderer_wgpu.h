// XTFrame reference backend -- MIT License (see LICENSE). Copy and edit (Code/Backends/README.md).
// Host API: webgpu.h of wgpu-native v29.0.1.1 (Code/SDKs/versions.cmake), through xt_wgpu.
#pragma once

#include <webgpu/webgpu.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "xt/command_list.h"
#include "xt/damage.h"
#include "xt/ir.h"
#include "xt/perf.h"
#include "xt/types.h"
#include "xt/wgpu/wgpu_renderer.h"
#include "xt/wgpu/wgpu_window.h"

namespace xt {
class TextShaper;
class AssetResolver;
}  // namespace xt

#ifndef XT_BACKEND_NO_TEXT
namespace xt::text {
class FtTextShaper;
class GlyphAtlas;
}  // namespace xt::text
#endif

/// WebGPU renderer (45 §45.7, 10 §10.10): `xt::wgpu::WgpuRenderer` drawing into a `WGPUSurface`
/// made from the platform's native window, or (offscreen) into a texture it owns and can read back.
///
/// Device: `WgpuInit::headless()` -- the one place that reads XT_WGPU_BACKEND and XT_WGPU_FALLBACK
/// (10 §10.10.1) -- then the surface is created on that instance and checked against the adapter.
/// Surface: `xt::wgpu::WgpuWindowSurface` (format, usage and present mode as its header says;
/// COPY_SRC when offered, so `readPixels` works on a window), reconfigured on resize and on an
/// outdated / lost texture.
/// 4x MSAA is done inside WgpuDevice (a 4x attachment resolving into the frame's view); every frame
/// clears, so a 4x surface never needs the host-content load that would ask for TEXTURE_BINDING.

namespace xt::backend {

/// The native window a surface is made from (filled by the combination file from its platform).
struct WgpuSurfaceSource {
    enum class Kind { None, Win32, X11, Wayland, Metal };
    Kind kind = Kind::None;
    void* display = nullptr;  ///< HINSTANCE, Display*, wl_display*
    void* window = nullptr;   ///< HWND, wl_surface*, CAMetalLayer*
    uint64_t x11Window = 0;
};

class RendererWgpu {
public:
    RendererWgpu();
    ~RendererWgpu();
    RendererWgpu(const RendererWgpu&) = delete;
    RendererWgpu& operator=(const RendererWgpu&) = delete;

    /// Device + surface for `source`, `width` x `height` pixels. False and `lastError()` when there
    /// is no adapter, the window gives no surface, or the adapter cannot present to it.
    bool create(const WgpuSurfaceSource& source, int width, int height, uint32_t sampleCount, bool vsync);
    /// Device + an RGBA8Unorm texture of `width` x `height` (no window): `present` does nothing and
    /// `readPixels` reads the texture.
    bool createOffscreen(int width, int height, uint32_t sampleCount);
    void destroy();

    /// New pixel size (reconfigures the surface / recreates the texture).
    void resize(int width, int height);
    /// Straight-alpha clear colour of every frame.
    void setClearColor(Color color);
#ifndef XT_BACKEND_NO_TEXT
    /// Glyphs from an atlas of `shaper` (null: none, glyphs from outlines). Not owned.
    void setTextShaper(text::FtTextShaper* shaper);
#endif

    /// Acquires the frame's view (surface texture, or the offscreen texture). A frame whose
    /// texture cannot be had (minimized, timeout) is skipped by render / present.
    void beginFrame();
    void render(const CommandList& list, TextShaper* shaper, AssetResolver* assets,
                const IrView* ir = nullptr, PerfTracker* perf = nullptr, const DamageList* damage = nullptr);
    /// Copies the frame's texture into `rgba` (premultiplied RGBA8, row 0 = top): a surface between
    /// render and present (false when it has no COPY_SRC), the offscreen texture at any time.
    bool readPixels(std::vector<uint8_t>& rgba, int& width, int& height);
    void present();

    int width() const { return m_width; }
    int height() const { return m_height; }
    const char* adapterName() const { return m_renderer.adapterName(); }
    const char* lastError() const { return m_error.c_str(); }
    xt::wgpu::WgpuRenderer& renderer() { return m_renderer; }

private:
    bool initDevice(uint32_t sampleCount, WGPUTextureFormat format);
    void releaseFrame();
    void releaseOffscreen();

    xt::wgpu::WgpuInit m_init;
    xt::wgpu::WgpuRenderer m_renderer;
    xt::wgpu::WgpuWindowSurface m_surface;
    WGPUTextureFormat m_format = WGPUTextureFormat_BGRA8Unorm;
    WGPUTexture m_offscreen = nullptr;
    WGPUTexture m_frameTexture = nullptr;  // the current frame's texture (the surface's or m_offscreen)
    WGPUTextureView m_frameView = nullptr;
    bool m_frameFromSurface = false;
    int m_width = 0, m_height = 0;
    Color m_clear{0.1f, 0.1f, 0.1f, 1.0f};
#ifndef XT_BACKEND_NO_TEXT
    std::unique_ptr<text::GlyphAtlas> m_atlas;
#endif
    std::string m_error;
};

}  // namespace xt::backend
