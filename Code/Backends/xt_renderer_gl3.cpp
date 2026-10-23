// XTFrame reference backend -- MIT License (see LICENSE). Copy and edit (Code/Backends/README.md).
// Host API: OpenGL 3.3 core (function pointers through the platform's GetProcAddress).
#include "xt_renderer_gl3.h"

#include <cstring>
#include <type_traits>

#ifndef XT_BACKEND_NO_TEXT
#include "xt/text/glyph_atlas.h"
#endif

#if defined(_WIN32)
#define XT_GLAPI __stdcall
#else
#define XT_GLAPI
#endif

namespace xt::backend {

namespace {

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLbitfield = unsigned int;
using GLfloat = float;
using GLboolean = unsigned char;

constexpr GLenum kFramebuffer = 0x8D40;
constexpr GLenum kReadFramebuffer = 0x8CA8;
constexpr GLenum kDrawFramebuffer = 0x8CA9;
constexpr GLenum kRenderbuffer = 0x8D41;
constexpr GLenum kColorAttachment0 = 0x8CE0;
constexpr GLenum kDepthStencilAttachment = 0x821A;
constexpr GLenum kFramebufferComplete = 0x8CD5;
constexpr GLenum kRgba8 = 0x8058;
constexpr GLenum kDepth24Stencil8 = 0x88F0;
constexpr GLbitfield kColorBufferBit = 0x4000;
constexpr GLbitfield kStencilBufferBit = 0x0400;
constexpr GLenum kNearest = 0x2600;
constexpr GLenum kScissorTest = 0x0C11;
constexpr GLenum kRgba = 0x1908;
constexpr GLenum kUnsignedByte = 0x1401;
constexpr GLenum kMaxSamples = 0x8D57;
constexpr GLenum kPackAlignment = 0x0D05;

}  // namespace

struct RendererGl3::Gl {
    void(XT_GLAPI* genFramebuffers)(GLsizei, GLuint*) = nullptr;
    void(XT_GLAPI* deleteFramebuffers)(GLsizei, const GLuint*) = nullptr;
    void(XT_GLAPI* bindFramebuffer)(GLenum, GLuint) = nullptr;
    void(XT_GLAPI* framebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint) = nullptr;
    GLenum(XT_GLAPI* checkFramebufferStatus)(GLenum) = nullptr;
    void(XT_GLAPI* genRenderbuffers)(GLsizei, GLuint*) = nullptr;
    void(XT_GLAPI* deleteRenderbuffers)(GLsizei, const GLuint*) = nullptr;
    void(XT_GLAPI* bindRenderbuffer)(GLenum, GLuint) = nullptr;
    void(XT_GLAPI* renderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei) = nullptr;
    void(XT_GLAPI* renderbufferStorageMultisample)(GLenum, GLsizei, GLenum, GLsizei, GLsizei) = nullptr;
    void(XT_GLAPI* blitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield,
                                    GLenum) = nullptr;
    void(XT_GLAPI* viewport)(GLint, GLint, GLsizei, GLsizei) = nullptr;
    void(XT_GLAPI* clearColor)(GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
    void(XT_GLAPI* clearStencil)(GLint) = nullptr;
    void(XT_GLAPI* clear)(GLbitfield) = nullptr;
    void(XT_GLAPI* disable)(GLenum) = nullptr;
    void(XT_GLAPI* colorMask)(GLboolean, GLboolean, GLboolean, GLboolean) = nullptr;
    void(XT_GLAPI* readPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*) = nullptr;
    void(XT_GLAPI* getIntegerv)(GLenum, GLint*) = nullptr;
    void(XT_GLAPI* pixelStorei)(GLenum, GLint) = nullptr;

    /// Loads every entry point; the name of the first missing one, or null.
    const char* load(xt::gl::GlLoadProc proc) {
        const char* missing = nullptr;
        auto get = [&](auto& fn, const char* name) {
            void* p = proc(name);
            if (p == nullptr && missing == nullptr) missing = name;
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(p);
        };
        get(genFramebuffers, "glGenFramebuffers");
        get(deleteFramebuffers, "glDeleteFramebuffers");
        get(bindFramebuffer, "glBindFramebuffer");
        get(framebufferRenderbuffer, "glFramebufferRenderbuffer");
        get(checkFramebufferStatus, "glCheckFramebufferStatus");
        get(genRenderbuffers, "glGenRenderbuffers");
        get(deleteRenderbuffers, "glDeleteRenderbuffers");
        get(bindRenderbuffer, "glBindRenderbuffer");
        get(renderbufferStorage, "glRenderbufferStorage");
        get(renderbufferStorageMultisample, "glRenderbufferStorageMultisample");
        get(blitFramebuffer, "glBlitFramebuffer");
        get(viewport, "glViewport");
        get(clearColor, "glClearColor");
        get(clearStencil, "glClearStencil");
        get(clear, "glClear");
        get(disable, "glDisable");
        get(colorMask, "glColorMask");
        get(readPixels, "glReadPixels");
        get(getIntegerv, "glGetIntegerv");
        get(pixelStorei, "glPixelStorei");
        return missing;
    }
};

RendererGl3::RendererGl3() = default;

RendererGl3::~RendererGl3() { destroy(); }

bool RendererGl3::create(xt::gl::GlLoadProc load, int width, int height, uint32_t sampleCount) {
    destroy();
    m_gl = std::make_unique<Gl>();
    if (const char* missing = m_gl->load(load)) {
        m_error = std::string("OpenGL 3.3 entry point missing: ") + missing;
        m_gl.reset();
        return false;
    }
    if (!m_renderer.init(load)) {
        m_error = std::string("GlRenderer::init failed: ") + m_renderer.lastError();
        m_gl.reset();
        return false;
    }
#ifndef XT_BACKEND_NO_TEXT
    m_renderer.setGlyphAtlas(m_atlas.get());
#endif
    m_gl->getIntegerv(kMaxSamples, &m_maxSamples);
    m_samples = (sampleCount >= 4 && m_maxSamples >= 4) ? 4 : 0;
    m_width = width > 0 ? width : 1;
    m_height = height > 0 ? height : 1;
    if (!createTargets()) {
        destroy();
        return false;
    }
    m_error.clear();
    return true;
}

void RendererGl3::destroy() {
    if (m_gl) {
        m_renderer.shutdown();
        releaseTargets();
    }
    m_gl.reset();
}

bool RendererGl3::createTargets() {
    Gl& gl = *m_gl;
    auto makeRb = [&](GLenum format, int samples) {
        GLuint rb = 0;
        gl.genRenderbuffers(1, &rb);
        gl.bindRenderbuffer(kRenderbuffer, rb);
        if (samples > 0) {
            gl.renderbufferStorageMultisample(kRenderbuffer, samples, format, m_width, m_height);
        } else {
            gl.renderbufferStorage(kRenderbuffer, format, m_width, m_height);
        }
        gl.bindRenderbuffer(kRenderbuffer, 0);
        return rb;
    };
    m_color = makeRb(kRgba8, m_samples);
    m_depthStencil = makeRb(kDepth24Stencil8, m_samples);
    gl.genFramebuffers(1, &m_fbo);
    gl.bindFramebuffer(kFramebuffer, m_fbo);
    gl.framebufferRenderbuffer(kFramebuffer, kColorAttachment0, kRenderbuffer, m_color);
    gl.framebufferRenderbuffer(kFramebuffer, kDepthStencilAttachment, kRenderbuffer, m_depthStencil);
    bool ok = gl.checkFramebufferStatus(kFramebuffer) == kFramebufferComplete;
    if (ok && m_samples > 0) {
        m_resolveColor = makeRb(kRgba8, 0);
        gl.genFramebuffers(1, &m_resolveFbo);
        gl.bindFramebuffer(kFramebuffer, m_resolveFbo);
        gl.framebufferRenderbuffer(kFramebuffer, kColorAttachment0, kRenderbuffer, m_resolveColor);
        ok = gl.checkFramebufferStatus(kFramebuffer) == kFramebufferComplete;
    }
    if (!ok) {
        m_error = "the OpenGL framebuffer object is incomplete";
        gl.bindFramebuffer(kFramebuffer, 0);
        return false;
    }
    // GlRenderer::resize reads GL_SAMPLES of the bound framebuffer.
    gl.bindFramebuffer(kFramebuffer, m_fbo);
    m_renderer.resize(m_width, m_height);
    m_resolved = false;
    return true;
}

void RendererGl3::releaseTargets() {
    Gl& gl = *m_gl;
    const GLuint fbos[2] = {m_fbo, m_resolveFbo};
    const GLuint rbs[3] = {m_color, m_depthStencil, m_resolveColor};
    gl.bindFramebuffer(kFramebuffer, 0);
    gl.deleteFramebuffers(2, fbos);
    gl.deleteRenderbuffers(3, rbs);
    m_fbo = m_color = m_depthStencil = m_resolveFbo = m_resolveColor = 0;
}

void RendererGl3::resize(int width, int height) {
    if (!m_gl || width <= 0 || height <= 0 || (width == m_width && height == m_height)) return;
    releaseTargets();
    m_width = width;
    m_height = height;
    createTargets();
}

void RendererGl3::setClearColor(Color color) { m_clear = color; }

#ifndef XT_BACKEND_NO_TEXT
void RendererGl3::setTextShaper(text::FtTextShaper* shaper) {
    m_renderer.setGlyphAtlas(nullptr);
    m_atlas.reset(shaper != nullptr ? new text::GlyphAtlas(*shaper) : nullptr);
    m_renderer.setGlyphAtlas(m_atlas.get());
}
#endif

void RendererGl3::beginFrame() {
    if (!m_gl) return;
    Gl& gl = *m_gl;
    gl.bindFramebuffer(kFramebuffer, m_fbo);
    gl.viewport(0, 0, m_width, m_height);
    gl.disable(kScissorTest);
    gl.colorMask(1, 1, 1, 1);
    gl.clearColor(m_clear.r * m_clear.a, m_clear.g * m_clear.a, m_clear.b * m_clear.a, m_clear.a);
    gl.clearStencil(0);
    gl.clear(kColorBufferBit | kStencilBufferBit);
    m_resolved = false;
}

void RendererGl3::render(const CommandList& list, TextShaper* shaper, AssetResolver* assets, const IrView* ir,
                         PerfTracker* perf, const DamageList* damage) {
    if (!m_gl) return;
    m_gl->bindFramebuffer(kFramebuffer, m_fbo);
    m_renderer.render(list, shaper, assets, ir, perf, damage);
    if (*m_renderer.lastError() != '\0') m_error = m_renderer.lastError();
    m_resolved = false;
}

void RendererGl3::resolve() {
    if (m_resolved || m_samples == 0) return;
    Gl& gl = *m_gl;
    gl.bindFramebuffer(kReadFramebuffer, m_fbo);
    gl.bindFramebuffer(kDrawFramebuffer, m_resolveFbo);
    gl.blitFramebuffer(0, 0, m_width, m_height, 0, 0, m_width, m_height, kColorBufferBit, kNearest);
    m_resolved = true;
}

bool RendererGl3::readPixels(std::vector<uint8_t>& rgba, int& width, int& height) {
    if (!m_gl) return false;
    Gl& gl = *m_gl;
    resolve();
    width = m_width;
    height = m_height;
    const size_t row = static_cast<size_t>(m_width) * 4u;
    std::vector<uint8_t> flipped(row * static_cast<size_t>(m_height));
    gl.bindFramebuffer(kReadFramebuffer, m_samples > 0 ? m_resolveFbo : m_fbo);
    gl.pixelStorei(kPackAlignment, 1);
    gl.readPixels(0, 0, m_width, m_height, kRgba, kUnsignedByte, flipped.data());
    rgba.resize(flipped.size());
    for (int y = 0; y < m_height; ++y) {  // GL rows start at the bottom
        std::memcpy(rgba.data() + static_cast<size_t>(y) * row,
                    flipped.data() + static_cast<size_t>(m_height - 1 - y) * row, row);
    }
    return true;
}

void RendererGl3::present(int windowWidth, int windowHeight) {
    if (!m_gl) return;
    Gl& gl = *m_gl;
    resolve();
    gl.bindFramebuffer(kReadFramebuffer, m_samples > 0 ? m_resolveFbo : m_fbo);
    gl.bindFramebuffer(kDrawFramebuffer, 0);
    gl.blitFramebuffer(0, 0, m_width, m_height, 0, 0, windowWidth, windowHeight, kColorBufferBit, kNearest);
    gl.bindFramebuffer(kFramebuffer, 0);
}

}  // namespace xt::backend
