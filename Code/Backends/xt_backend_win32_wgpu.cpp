// XTFrame reference backend -- MIT License (see LICENSE). Copy and edit (Code/Backends/README.md).
// Host API: Win32 (window, input) and webgpu.h of wgpu-native v29.0.1.1 (rendering).
#include "xt_backend.h"
#include "xt_platform_win32.h"
#include "xt_renderer_wgpu.h"

/// Win32 HWND -> WGPUSurface -> RendererWgpu. Docs/design/45-repository-layout.md §45.7,
/// 10 §10.10.4 (T-439).

namespace xt::backend {

struct Backend::Impl {
    PlatformWin32 platform;
    RendererWgpu renderer;
    std::string error;
};

Backend::Backend() : m_impl(std::make_unique<Impl>()) {}
Backend::~Backend() { destroy(); }

bool Backend::create(const BackendOptions& options) {
    destroy();
    Win32Options po;
    po.title = options.title;
    po.width = options.width;
    po.height = options.height;
    po.resizable = options.resizable;
    po.hidden = options.hidden;
    if (!m_impl->platform.create(po)) {
        m_impl->error = m_impl->platform.lastError();
        return false;
    }
    WgpuSurfaceSource source;
    source.kind = WgpuSurfaceSource::Kind::Win32;
    source.display = m_impl->platform.hinstance();
    source.window = m_impl->platform.hwnd();
    int w = 0, h = 0;
    m_impl->platform.pixelSize(w, h);
    if (!m_impl->renderer.create(source, w, h, options.sampleCount, options.vsync)) {
        m_impl->error = m_impl->renderer.lastError();
        m_impl->platform.destroy();
        return false;
    }
    m_impl->renderer.setClearColor(options.clearColor);
    m_impl->error.clear();
    return true;
}

void Backend::destroy() {
    m_impl->renderer.destroy();
    m_impl->platform.destroy();
}

bool Backend::pumpEvents(Context& context) { return m_impl->platform.pumpEvents(context); }

void Backend::beginFrame() {
    if (m_impl->platform.takeResized()) {
        int w = 0, h = 0;
        m_impl->platform.pixelSize(w, h);
        m_impl->renderer.resize(w, h);
    }
    m_impl->renderer.beginFrame();
}

void Backend::render(const CommandList& list, TextShaper* shaper, AssetResolver* assets, const IrView* ir,
                     PerfTracker* perf, const DamageList* damage) {
    m_impl->renderer.render(list, shaper, assets, ir, perf, damage);
}

void Backend::present() { m_impl->renderer.present(); }

#ifndef XT_BACKEND_NO_TEXT
void Backend::setTextShaper(text::FtTextShaper* shaper) { m_impl->renderer.setTextShaper(shaper); }
#endif

int Backend::width() const {
    int w = 0, h = 0;
    m_impl->platform.logicalSize(w, h);
    return w;
}

int Backend::height() const {
    int w = 0, h = 0;
    m_impl->platform.logicalSize(w, h);
    return h;
}

float Backend::devicePixelRatio() const { return m_impl->platform.devicePixelRatio(); }
double Backend::seconds() const { return m_impl->platform.seconds(); }
std::string Backend::clipboardText() const { return m_impl->platform.clipboardText(); }
void Backend::setClipboardText(std::string_view utf8) { m_impl->platform.setClipboardText(utf8); }
const char* Backend::name() const { return "win32_wgpu"; }

const char* Backend::lastError() const {
    return !m_impl->error.empty() ? m_impl->error.c_str() : m_impl->renderer.lastError();
}

}  // namespace xt::backend
