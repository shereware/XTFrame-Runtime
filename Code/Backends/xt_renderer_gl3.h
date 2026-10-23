// XTFrame reference backend -- MIT License (see LICENSE). Copy and edit (Code/Backends/README.md).
// Host API: OpenGL 3.3 core (function pointers through the platform's GetProcAddress).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "xt/command_list.h"
#include "xt/damage.h"
#include "xt/gl/gl_renderer.h"
#include "xt/ir.h"
#include "xt/perf.h"
#include "xt/types.h"

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

/// OpenGL 3.3 renderer (45 §45.7): `xt::gl::GlRenderer` drawing into a framebuffer object of its
/// own -- 4x multisampled when asked and available, with an 8-bit stencil -- that is resolved and
/// blitted to the window's default framebuffer by `present` (the platform then swaps). Drawing
/// into an FBO rather than the default framebuffer makes the result readable on every driver
/// (`readPixels`, also for a hidden window) and independent of the window's pixel format. The few
/// GL calls this file makes itself (FBOs, clear, blit, read) are loaded here, separately from
/// xt_gl's private loader.

namespace xt::backend {

class RendererGl3 {
public:
    RendererGl3();
    ~RendererGl3();
    RendererGl3(const RendererGl3&) = delete;
    RendererGl3& operator=(const RendererGl3&) = delete;

    /// With the platform's GL 3.3 context current: loads GL through `load`, initializes
    /// GlRenderer and the `width` x `height` target. False and `lastError()` on failure.
    bool create(xt::gl::GlLoadProc load, int width, int height, uint32_t sampleCount);
    void destroy();

    void resize(int width, int height);
    void setClearColor(Color color);
#ifndef XT_BACKEND_NO_TEXT
    void setTextShaper(text::FtTextShaper* shaper);
#endif

    /// Binds and clears the target.
    void beginFrame();
    void render(const CommandList& list, TextShaper* shaper, AssetResolver* assets,
                const IrView* ir = nullptr, PerfTracker* perf = nullptr, const DamageList* damage = nullptr);
    /// The frame as premultiplied RGBA8, row 0 = top.
    bool readPixels(std::vector<uint8_t>& rgba, int& width, int& height);
    /// Resolves and blits the frame to framebuffer 0 at `windowWidth` x `windowHeight` pixels.
    void present(int windowWidth, int windowHeight);

    int width() const { return m_width; }
    int height() const { return m_height; }
    const char* lastError() const { return m_error.c_str(); }
    xt::gl::GlRenderer& renderer() { return m_renderer; }

private:
    struct Gl;  // the loaded entry points
    bool createTargets();
    void releaseTargets();
    void resolve();

    std::unique_ptr<Gl> m_gl;
    xt::gl::GlRenderer m_renderer;
    uint32_t m_fbo = 0, m_color = 0, m_depthStencil = 0;  // draw target (multisampled when m_samples > 0)
    uint32_t m_resolveFbo = 0, m_resolveColor = 0;        // single-sample copy (m_samples > 0)
    int m_samples = 0;
    int m_maxSamples = 0;
    bool m_resolved = false;
    int m_width = 0, m_height = 0;
    Color m_clear{0.1f, 0.1f, 0.1f, 1.0f};
#ifndef XT_BACKEND_NO_TEXT
    std::unique_ptr<text::GlyphAtlas> m_atlas;
#endif
    std::string m_error;
};

}  // namespace xt::backend
