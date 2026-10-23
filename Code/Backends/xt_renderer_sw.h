// XTFrame reference backend -- MIT License (see LICENSE). Copy and edit (Code/Backends/README.md).
// Host API: none (CPU rendering; the platform presents the buffer, e.g. PlatformSdl2::blitRgba).
#pragma once

#include <cstdint>
#include <vector>

#include "xt/command_list.h"
#include "xt/damage.h"
#include "xt/ir.h"
#include "xt/perf.h"
#include "xt/sw/sw_renderer.h"
#include "xt/types.h"

namespace xt {
class TextShaper;
class AssetResolver;
}  // namespace xt

/// Software renderer (45 §45.7): `xt::sw::SwRenderer` into a premultiplied RGBA8 buffer the
/// combination file hands to the platform's blit. With a `DamageList` only its rectangles are
/// cleared and redrawn (the shell's SW path, 23 §23.11.1); a full or missing damage list redraws
/// everything. `dirty()` tells the platform whether anything changed since the last present.

namespace xt::backend {

class RendererSw {
public:
    /// The buffer is `width` x `height` pixels (cleared on the next frame).
    void resize(int width, int height);
    void setClearColor(Color color);

    void beginFrame() {}
    void render(const CommandList& list, TextShaper* shaper, AssetResolver* assets,
                const IrView* ir = nullptr, PerfTracker* perf = nullptr, const DamageList* damage = nullptr);

    const uint8_t* pixels() const { return m_pixels.data(); }
    int width() const { return m_width; }
    int height() const { return m_height; }
    int stride() const { return m_width * 4; }
    /// True when the last render changed pixels (cleared by takeDirty()).
    bool takeDirty();

private:
    xt::sw::SwRenderer m_renderer;
    std::vector<uint8_t> m_pixels;
    int m_width = 0, m_height = 0;
    bool m_fresh = true;  // the next frame must redraw everything
    bool m_dirty = false;
    Color m_clear{0.1f, 0.1f, 0.1f, 1.0f};
};

}  // namespace xt::backend
