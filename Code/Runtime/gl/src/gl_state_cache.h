#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "gl_loader.h"

/// GL state de-duplication for `GlDevice::submit` (T-159, Docs/design/10-backends.md §10.3.5).
///
/// `GlStateCache` shadows every piece of context state the device touches while replaying a
/// frame (program, framebuffer bindings, VAO, viewport, scissor, blend, stencil, color mask,
/// texture units) and only issues a GL call when the requested value differs from the shadow.
/// The shadow is reset to "unknown" at the start of every submit, so `saveState`/`restoreState`
/// in GlRenderer keep their meaning: whatever the host left behind is never assumed.
///
/// `UniformShadow` does the same for one program's uniforms. Uniforms are *program* state, not
/// context state, so its shadow survives across frames: nobody else can change our programs'
/// uniforms. A `-1` location is a no-op, as with the raw `glUniform*` calls it replaces.
///
/// `GlDeviceStats` counts the GL calls that actually went out (plus resolves, clears and buffer
/// upload bytes, counted by their callers); `GlRenderer::frameStats()` exposes the last frame's
/// numbers and Code/Tools/gpu-probe prints them.

namespace xt::gl {

struct GlDeviceStats {
    uint32_t drawCalls = 0;
    uint32_t programBinds = 0;
    uint32_t fboBinds = 0;      ///< glBindFramebuffer calls (draw, read or both)
    uint32_t textureBinds = 0;  ///< glBindTexture calls
    uint32_t uniformSets = 0;   ///< glUniform* calls
    uint32_t resolves = 0;      ///< multisample -> single-sample resolve draws of a transient (T-161)
    uint32_t blits = 0;         ///< every glBlitFramebuffer (copy passes)
    uint32_t clears = 0;        ///< glClear calls
    uint32_t passes = 0;
    uint32_t uploadRanges = 0;      ///< glBufferData/glBufferSubData calls for vertices + indices
    uint64_t uploadBytes = 0;       ///< bytes those calls carried
    uint32_t transientCreates = 0;  ///< pooled FBOs created (not recycled) this submit (T-161)
    uint32_t cacheStores = 0;       ///< `GpuFrame::cacheStores` executed (effect result cache, T-162)
    // -- T-163: per-phase CPU time of the last frame (ms, steady_clock) and GL object churn. --
    float buildMs = 0;     ///< FrameBuilder::build (filled in by GlRenderer; 0 when the frame was reused)
    float uploadMs = 0;    ///< vertex/index upload
    float targetMs = 0;    ///< transient acquisition (pool lookups and any creation)
    float drawMs = 0;      ///< the passes: clears, copies, commands, resolves, cache stores
    float endFrameMs = 0;  ///< pool release and eviction at the end of submit
    float stateMs = 0;     ///< GlRenderer's GL state save (glGet*) + restore around the frame
    float effectsMs = 0;   ///< T-457: CPU time drawing Shading::Effect commands (part of drawMs)
    float textMs = 0;      ///< T-457: CPU time drawing Shading::Glyph commands (part of drawMs)
    uint32_t cachedTargetCreates = 0;   ///< `acquireCachedTarget` calls that created GL objects
    uint32_t cachedTargetReleases = 0;  ///< `releaseCachedTarget` calls
    uint32_t transientDeletes = 0;      ///< pooled FBOs deleted (idle eviction or resize purge)
    uint32_t poolEntries = 0;           ///< pooled FBOs alive after the frame
    uint64_t poolBytes = 0;             ///< their color + depth/stencil bytes (4 per sample each)
    uint32_t cachedTargets = 0;         ///< effect-cache targets alive after the frame
    uint64_t cachedBytes = 0;           ///< their RGBA8 bytes
};

class GlStateCache {
public:
    /// Forgets every shadow (the next call of each kind always goes to GL). Called at the start
    /// of a submit, when the host may have changed anything.
    void reset();

    void useProgram(GLuint program);
    void bindVertexArray(GLuint vao);
    /// Binds `fbo` for drawing only (GL_DRAW_FRAMEBUFFER), leaving the read binding alone.
    void bindDrawFramebuffer(GLuint fbo);
    void bindReadFramebuffer(GLuint fbo);
    /// After code that binds framebuffers/textures behind the cache's back (FboPool creating an
    /// entry): the affected shadows become unknown.
    void invalidateFramebuffers();
    void invalidateTextures();

    void viewport(GLint x, GLint y, GLsizei w, GLsizei h);
    void scissorTest(bool enable);
    void scissor(GLint x, GLint y, GLsizei w, GLsizei h);
    /// `Blend::SrcOver` on/off; the blend function and equation are set (once) when enabling.
    void blendSrcOver(bool enable);
    /// T-202: `Blend::Additive` (ONE, ONE for color and alpha, FUNC_ADD); blending on.
    void blendAdditive();
    void stencilTest(bool enable);
    void stencilFunc(GLenum func, GLint ref, GLuint mask);
    void stencilMask(GLuint mask);
    /// Both faces (`glStencilOp`).
    void stencilOp(GLenum sfail, GLenum dpfail, GLenum dppass);
    void stencilOpSeparate(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass);
    void colorMask(bool writes);
    /// Clear color (0,0,0,0) and clear stencil 0, set once per submit.
    void clearValuesZero();

    /// Binds `tex` (GL_TEXTURE_2D) on texture unit `unit` (0-based), switching the active unit
    /// when needed. Returns true when a bind was issued.
    bool bindTexture(int unit, GLuint tex);
    /// Makes `unit` the active unit (for glTexParameteri after bindTexture).
    void activeTexture(int unit);
    /// Binds `tex` (GL_TEXTURE_2D_MULTISAMPLE) on unit `unit`; a separate binding point from
    /// the 2D one, shadowed separately (T-161: the resolve source).
    void bindTextureMs(int unit, GLuint tex);

    GlDeviceStats stats;

private:
    static constexpr int kUnits = 4;
    static constexpr GLuint kUnknown = 0xFFFFFFFFu;

    GLuint m_program = kUnknown;
    GLuint m_vao = kUnknown;
    GLuint m_drawFbo = kUnknown;
    GLuint m_readFbo = kUnknown;
    GLint m_viewport[4] = {-1, -1, -1, -1};
    bool m_viewportValid = false;
    int m_scissorTest = -1;
    GLint m_scissor[4] = {-1, -1, -1, -1};
    bool m_scissorValid = false;
    int m_blend = -1;
    int m_blendFunc = 0;  ///< 0 unknown, 1 source-over, 2 additive (T-202).
    /// Enables/disables GL_BLEND through the shadow.
    void enableBlend(bool enable);
    int m_stencilTest = -1;
    bool m_stencilFuncValid = false;
    GLenum m_stencilFuncE = 0;
    GLint m_stencilRef = 0;
    GLuint m_stencilValueMask = 0;
    bool m_stencilMaskValid = false;
    GLuint m_stencilMask = 0;
    bool m_stencilOpValid[2] = {false, false};  ///< front, back
    GLenum m_stencilOps[2][3] = {};
    int m_colorMask = -1;
    bool m_clearValid = false;
    int m_activeUnit = -1;
    GLuint m_texture[kUnits] = {kUnknown, kUnknown, kUnknown, kUnknown};
    GLuint m_textureMs[kUnits] = {kUnknown, kUnknown, kUnknown, kUnknown};
};

class UniformShadow {
public:
    void set1i(GLint loc, GLint v);
    void set1f(GLint loc, float v);
    void set2f(GLint loc, float a, float b);
    void set3f(GLint loc, float a, float b, float c);
    void set4f(GLint loc, float a, float b, float c, float d);
    void setMat3(GLint loc, const float* m9);
    void setMat4(GLint loc, const float* m16);
    void set1fv(GLint loc, int count, const float* v);

    /// Where to count issued glUniform* calls (may stay null).
    GlDeviceStats* stats = nullptr;

private:
    /// Returns true (and stores `bytes` of `data`) when the value differs from the shadow.
    bool update(GLint loc, const void* data, size_t bytes);
    std::vector<std::vector<uint8_t>> m_slots;  ///< indexed by location
};

}  // namespace xt::gl
