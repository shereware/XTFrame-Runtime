#include "view_renderer.h"

#include <cstdio>

#include "xt_platform_sdl2.h"
#include "xt_renderer_sw.h"
#ifdef XT_VIEW_GL
#include "xt_renderer_gl3.h"
#endif
#ifdef XT_VIEW_WGPU
#include "xt_renderer_wgpu.h"
#endif

/// T-439: the --renderer selection of xtframe-view (view_renderer.h).

namespace xt::view {

namespace {

/// Premultiplied -> straight alpha, in place (what writePng and the SW headless buffer hold).
void unpremultiply(std::vector<uint8_t>& rgba) {
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        const unsigned a = rgba[i + 3];
        if (a == 0 || a == 255) continue;
        for (size_t c = 0; c < 3; ++c) {
            const unsigned v = (static_cast<unsigned>(rgba[i + c]) * 255u + a / 2u) / a;
            rgba[i + c] = static_cast<uint8_t>(v > 255u ? 255u : v);
        }
    }
}

backend::Sdl2Options windowOptions(const FrameTargetConfig& config, backend::Sdl2Graphics graphics) {
    backend::Sdl2Options o;
    o.title = config.title;
    o.width = config.width;
    o.height = config.height;
    o.hidden = config.hidden || !config.windowed;
    o.resizable = config.windowed;
    o.graphics = graphics;
    return o;
}

/// The window's pixel size (offscreen targets keep the configured size). Only the GL and WebGPU
/// targets size themselves this way, so a build with both off (the SW shell) does not call it.
[[maybe_unused]] void targetSize(const backend::PlatformSdl2& platform, const FrameTargetConfig& config,
                                 int& w, int& h) {
    if (config.windowed) {
        platform.pixelSize(w, h);
    } else {
        w = config.width;
        h = config.height;
    }
}

/// sw in a window: xt_sw into a buffer, SDL_Renderer blit.
class SwWindowTarget final : public FrameTarget {
public:
    bool create(const FrameTargetConfig& config, std::string& error) {
        if (!m_platform.create(windowOptions(config, backend::Sdl2Graphics::None))) {
            error = m_platform.lastError();
            return false;
        }
        int w = 0, h = 0;
        m_platform.pixelSize(w, h);
        m_renderer.resize(w, h);
        m_renderer.setClearColor(config.clear);
        return true;
    }
    bool windowed() const override { return true; }
    bool pump(Context& context) override {
        const bool open = m_platform.pumpEvents(context);
        if (m_platform.takeResized()) {
            int w = 0, h = 0;
            m_platform.pixelSize(w, h);
            m_renderer.resize(w, h);
        }
        return open;
    }
    void render(const CommandList& list, TextShaper* shaper, AssetResolver* assets, const IrView* ir,
                PerfTracker* perf, const DamageList* damage) override {
        m_renderer.render(list, shaper, assets, ir, perf, damage);
    }
    bool read(std::vector<uint8_t>& rgba, int& width, int& height) override {
        width = m_renderer.width();
        height = m_renderer.height();
        rgba.assign(m_renderer.pixels(), m_renderer.pixels() + static_cast<size_t>(m_renderer.stride()) *
                                                                   static_cast<size_t>(height));
        unpremultiply(rgba);
        return width > 0 && height > 0;
    }
    void present() override {
        if (m_renderer.takeDirty()) {
            m_platform.blitRgba(m_renderer.pixels(), m_renderer.width(), m_renderer.height(),
                                m_renderer.stride());
        }
    }
    double seconds() const override { return m_platform.seconds(); }
#ifndef XT_BACKEND_NO_TEXT
    void setTextShaper(text::FtTextShaper*) override {}
#endif
    std::string description() const override { return "sw (sdl2 window)"; }

private:
    backend::PlatformSdl2 m_platform;
    backend::RendererSw m_renderer;
};

#ifdef XT_VIEW_GL
/// gl: an SDL window's GL 3.3 context (hidden offscreen) and RendererGl3's FBO.
class GlTarget final : public FrameTarget {
public:
    bool create(const FrameTargetConfig& config, std::string& error) {
        m_windowed = config.windowed;
        if (!m_platform.create(windowOptions(config, backend::Sdl2Graphics::OpenGL))) {
            error = m_platform.lastError();
            return false;
        }
        int w = 0, h = 0;
        targetSize(m_platform, config, w, h);
        if (!m_renderer.create(&backend::PlatformSdl2::glGetProcAddress, w, h, config.sampleCount)) {
            error = m_renderer.lastError();
            return false;
        }
        m_renderer.setClearColor(config.clear);
        return true;
    }
    bool windowed() const override { return m_windowed; }
    bool pump(Context& context) override {
        if (!m_windowed) return true;
        const bool open = m_platform.pumpEvents(context);
        if (m_platform.takeResized()) {
            int w = 0, h = 0;
            m_platform.pixelSize(w, h);
            m_renderer.resize(w, h);
        }
        return open;
    }
    void render(const CommandList& list, TextShaper* shaper, AssetResolver* assets, const IrView* ir,
                PerfTracker* perf, const DamageList* damage) override {
        m_platform.glMakeCurrent();
        m_renderer.beginFrame();
        m_renderer.render(list, shaper, assets, ir, perf, damage);
    }
    bool read(std::vector<uint8_t>& rgba, int& width, int& height) override {
        if (!m_renderer.readPixels(rgba, width, height)) return false;
        unpremultiply(rgba);
        return true;
    }
    void present() override {
        if (!m_windowed) return;
        int w = 0, h = 0;
        m_platform.pixelSize(w, h);
        m_renderer.present(w, h);
        m_platform.glSwap();
    }
    double seconds() const override { return m_platform.seconds(); }
#ifndef XT_BACKEND_NO_TEXT
    void setTextShaper(text::FtTextShaper* shaper) override { m_renderer.setTextShaper(shaper); }
#endif
    std::string description() const override {
        return m_windowed ? "gl (sdl2 window)" : "gl (offscreen FBO)";
    }

private:
    // The renderer is destroyed (its GL objects) while the platform's context still exists.
    backend::PlatformSdl2 m_platform;
    backend::RendererGl3 m_renderer;
    bool m_windowed = false;
};
#endif

#ifdef XT_VIEW_WGPU
/// wgpu: a headless device and texture, or an SDL window's surface.
class WgpuTarget final : public FrameTarget {
public:
    bool create(const FrameTargetConfig& config, std::string& error) {
        m_windowed = config.windowed;
        if (!m_windowed) {
            if (!m_renderer.createOffscreen(config.width, config.height, config.sampleCount)) {
                error = m_renderer.lastError();
                return false;
            }
        } else {
            if (!m_platform.create(windowOptions(config, backend::Sdl2Graphics::Surface))) {
                error = m_platform.lastError();
                return false;
            }
            const backend::Sdl2NativeWindow native = m_platform.nativeWindow();
            backend::WgpuSurfaceSource source;
            switch (native.kind) {
                case backend::Sdl2NativeWindow::Kind::Win32:
                    source.kind = backend::WgpuSurfaceSource::Kind::Win32;
                    break;
                case backend::Sdl2NativeWindow::Kind::X11:
                    source.kind = backend::WgpuSurfaceSource::Kind::X11;
                    break;
                case backend::Sdl2NativeWindow::Kind::Wayland:
                    source.kind = backend::WgpuSurfaceSource::Kind::Wayland;
                    break;
                case backend::Sdl2NativeWindow::Kind::Metal:
                    source.kind = backend::WgpuSurfaceSource::Kind::Metal;
                    break;
                case backend::Sdl2NativeWindow::Kind::None:
                    break;
            }
            source.display = native.display;
            source.window = native.window;
            source.x11Window = native.x11Window;
            int w = 0, h = 0;
            m_platform.pixelSize(w, h);
            if (!m_renderer.create(source, w, h, config.sampleCount, /*vsync=*/true)) {
                error = m_renderer.lastError();
                return false;
            }
        }
        m_renderer.setClearColor(config.clear);
        return true;
    }
    bool windowed() const override { return m_windowed; }
    bool pump(Context& context) override {
        if (!m_windowed) return true;
        const bool open = m_platform.pumpEvents(context);
        if (m_platform.takeResized()) {
            int w = 0, h = 0;
            m_platform.pixelSize(w, h);
            m_renderer.resize(w, h);
        }
        return open;
    }
    void render(const CommandList& list, TextShaper* shaper, AssetResolver* assets, const IrView* ir,
                PerfTracker* perf, const DamageList* damage) override {
        m_renderer.beginFrame();
        m_renderer.render(list, shaper, assets, ir, perf, damage);
    }
    bool read(std::vector<uint8_t>& rgba, int& width, int& height) override {
        if (!m_renderer.readPixels(rgba, width, height)) return false;
        unpremultiply(rgba);
        return true;
    }
    void present() override { m_renderer.present(); }
    double seconds() const override { return m_windowed ? m_platform.seconds() : 0.0; }
#ifndef XT_BACKEND_NO_TEXT
    void setTextShaper(text::FtTextShaper* shaper) override { m_renderer.setTextShaper(shaper); }
#endif
    std::string description() const override {
        return std::string("wgpu ") + (m_windowed ? "(sdl2 window, " : "(offscreen, ") +
               m_renderer.adapterName() + ")";
    }

private:
    backend::PlatformSdl2 m_platform;
    backend::RendererWgpu m_renderer;  // destroyed first (declared last): the surface before the window
    bool m_windowed = false;
};
#endif

}  // namespace

const char* rendererName(Renderer renderer) {
    switch (renderer) {
        case Renderer::Gl:
            return "gl";
        case Renderer::Wgpu:
            return "wgpu";
        case Renderer::Sw:
            break;
    }
    return "sw";
}

namespace {

bool built(Renderer renderer) {
    switch (renderer) {
        case Renderer::Sw:
            return true;
#ifdef XT_VIEW_GL
        case Renderer::Gl:
            return true;
#endif
#ifdef XT_VIEW_WGPU
        case Renderer::Wgpu:
            return true;
#endif
        default:
            return false;
    }
}

}  // namespace

std::string builtRenderers() {
    std::string out = "sw";
    if (built(Renderer::Gl)) out += " gl";
    if (built(Renderer::Wgpu)) out += " wgpu";
    return out;
}

Renderer defaultRenderer(bool headless) {
#ifdef XT_SHELL_SW
    (void)headless;
    return Renderer::Sw;
#else
    return headless ? Renderer::Sw : Renderer::Gl;
#endif
}

Renderer resolveRenderer(std::string_view text, bool headless) {
    const Renderer fallback = defaultRenderer(headless);
    if (text.empty()) return fallback;
    Renderer r = fallback;
    if (text == "sw") {
        r = Renderer::Sw;
    } else if (text == "gl") {
        r = Renderer::Gl;
    } else if (text == "wgpu") {
        r = Renderer::Wgpu;
    } else {
        std::fprintf(stderr, "xtframe-view: unknown renderer '%.*s' (built in: %s); using %s\n",
                     static_cast<int>(text.size()), text.data(), builtRenderers().c_str(),
                     rendererName(fallback));
        return fallback;
    }
    if (!built(r)) {
        std::fprintf(
            stderr,
            "xtframe-view: renderer '%s' is not built into this xtframe-view (built in: %s); using %s\n",
            rendererName(r), builtRenderers().c_str(), rendererName(fallback));
        return fallback;
    }
    return r;
}

std::unique_ptr<FrameTarget> createFrameTarget(Renderer renderer, const FrameTargetConfig& config,
                                               std::string& error) {
    switch (renderer) {
        case Renderer::Sw: {
            if (!config.windowed) {
                error = "offscreen sw is the view's own buffer";
                return nullptr;
            }
            auto t = std::make_unique<SwWindowTarget>();
            if (!t->create(config, error)) return nullptr;
            return t;
        }
        case Renderer::Gl: {
#ifdef XT_VIEW_GL
            auto t = std::make_unique<GlTarget>();
            if (!t->create(config, error)) return nullptr;
            return t;
#else
            break;
#endif
        }
        case Renderer::Wgpu: {
#ifdef XT_VIEW_WGPU
            auto t = std::make_unique<WgpuTarget>();
            if (!t->create(config, error)) return nullptr;
            return t;
#else
            break;
#endif
        }
    }
    error = std::string("renderer ") + rendererName(renderer) + " is not built in";
    return nullptr;
}

}  // namespace xt::view
