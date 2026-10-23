// XTFrame reference backend -- MIT License (see LICENSE). Copy and edit (Code/Backends/README.md).
// Host API: none (CPU rendering; the platform presents the buffer, e.g. PlatformSdl2::blitRgba).
#include "xt_renderer_sw.h"

#include <span>

namespace xt::backend {

void RendererSw::resize(int width, int height) {
    if (width <= 0 || height <= 0 || (width == m_width && height == m_height && !m_pixels.empty())) return;
    m_width = width;
    m_height = height;
    m_pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0);
    xt::sw::SwTarget target{m_pixels.data(), width, height, width * 4};
    target.premultiplied = true;  // what a GPU texture upload takes as is (the shell's SW path)
    m_renderer.setTarget(target);
    m_fresh = true;
}

void RendererSw::setClearColor(Color color) {
    m_clear = color;
    m_fresh = true;
}

void RendererSw::render(const CommandList& list, TextShaper* shaper, AssetResolver* assets, const IrView* ir,
                        PerfTracker* perf, const DamageList* damage) {
    if (m_pixels.empty()) return;
    if (m_fresh || damage == nullptr || damage->full) {
        m_renderer.clear(m_clear);
        m_renderer.render(list, shaper, assets, ir, perf);
        m_fresh = false;
        m_dirty = true;
        return;
    }
    if (damage->rects.empty()) return;  // nothing changed: the buffer still holds the frame
    // Damage rectangles are device pixels, like the buffer (damage.h).
    m_renderer.clearRects(m_clear, std::span<const Rect>(damage->rects));
    m_renderer.render(list, shaper, assets, ir, perf, damage);
    m_dirty = true;
}

bool RendererSw::takeDirty() {
    const bool d = m_dirty;
    m_dirty = false;
    return d;
}

}  // namespace xt::backend
