#include "gl_state_cache.h"

#include <cstring>

namespace xt::gl {

void GlStateCache::reset() {
    m_program = m_vao = m_drawFbo = m_readFbo = kUnknown;
    m_viewportValid = false;
    m_scissorTest = -1;
    m_scissorValid = false;
    m_blend = -1;
    m_blendFunc = 0;
    m_stencilTest = -1;
    m_stencilFuncValid = false;
    m_stencilMaskValid = false;
    m_stencilOpValid[0] = m_stencilOpValid[1] = false;
    m_colorMask = -1;
    m_clearValid = false;
    m_activeUnit = -1;
    for (GLuint& t : m_texture) t = kUnknown;
    for (GLuint& t : m_textureMs) t = kUnknown;
}

void GlStateCache::useProgram(GLuint program) {
    if (m_program == program) return;
    m_program = program;
    glUseProgram(program);
    ++stats.programBinds;
}

void GlStateCache::bindVertexArray(GLuint vao) {
    if (m_vao == vao) return;
    m_vao = vao;
    glBindVertexArray(vao);
}

void GlStateCache::bindDrawFramebuffer(GLuint fbo) {
    if (m_drawFbo == fbo) return;
    m_drawFbo = fbo;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    ++stats.fboBinds;
}

void GlStateCache::bindReadFramebuffer(GLuint fbo) {
    if (m_readFbo == fbo) return;
    m_readFbo = fbo;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    ++stats.fboBinds;
}

void GlStateCache::invalidateFramebuffers() { m_drawFbo = m_readFbo = kUnknown; }

void GlStateCache::invalidateTextures() {
    m_activeUnit = -1;
    for (GLuint& t : m_texture) t = kUnknown;
    for (GLuint& t : m_textureMs) t = kUnknown;
}

void GlStateCache::viewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    if (m_viewportValid && m_viewport[0] == x && m_viewport[1] == y && m_viewport[2] == w &&
        m_viewport[3] == h) {
        return;
    }
    m_viewportValid = true;
    m_viewport[0] = x;
    m_viewport[1] = y;
    m_viewport[2] = w;
    m_viewport[3] = h;
    glViewport(x, y, w, h);
}

void GlStateCache::scissorTest(bool enable) {
    const int v = enable ? 1 : 0;
    if (m_scissorTest == v) return;
    m_scissorTest = v;
    if (enable) {
        glEnable(GL_SCISSOR_TEST);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
}

void GlStateCache::scissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    if (m_scissorValid && m_scissor[0] == x && m_scissor[1] == y && m_scissor[2] == w && m_scissor[3] == h) {
        return;
    }
    m_scissorValid = true;
    m_scissor[0] = x;
    m_scissor[1] = y;
    m_scissor[2] = w;
    m_scissor[3] = h;
    glScissor(x, y, w, h);
}

void GlStateCache::blendSrcOver(bool enable) {
    if (enable && m_blendFunc != 1) {
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glBlendEquation(GL_FUNC_ADD);
        m_blendFunc = 1;
    }
    enableBlend(enable);
}

void GlStateCache::blendAdditive() {
    if (m_blendFunc != 2) {
        glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
        glBlendEquation(GL_FUNC_ADD);
        m_blendFunc = 2;
    }
    enableBlend(true);
}

void GlStateCache::enableBlend(bool enable) {
    const int v = enable ? 1 : 0;
    if (m_blend == v) return;
    m_blend = v;
    if (enable) {
        glEnable(GL_BLEND);
    } else {
        glDisable(GL_BLEND);
    }
}

void GlStateCache::stencilTest(bool enable) {
    const int v = enable ? 1 : 0;
    if (m_stencilTest == v) return;
    m_stencilTest = v;
    if (enable) {
        glEnable(GL_STENCIL_TEST);
    } else {
        glDisable(GL_STENCIL_TEST);
    }
}

void GlStateCache::stencilFunc(GLenum func, GLint ref, GLuint mask) {
    if (m_stencilFuncValid && m_stencilFuncE == func && m_stencilRef == ref && m_stencilValueMask == mask) {
        return;
    }
    m_stencilFuncValid = true;
    m_stencilFuncE = func;
    m_stencilRef = ref;
    m_stencilValueMask = mask;
    glStencilFunc(func, ref, mask);
}

void GlStateCache::stencilMask(GLuint mask) {
    if (m_stencilMaskValid && m_stencilMask == mask) return;
    m_stencilMaskValid = true;
    m_stencilMask = mask;
    glStencilMask(mask);
}

void GlStateCache::stencilOp(GLenum sfail, GLenum dpfail, GLenum dppass) {
    bool same = true;
    for (int face = 0; face < 2; ++face) {
        if (!m_stencilOpValid[face] || m_stencilOps[face][0] != sfail || m_stencilOps[face][1] != dpfail ||
            m_stencilOps[face][2] != dppass) {
            same = false;
        }
    }
    if (same) return;
    for (int face = 0; face < 2; ++face) {
        m_stencilOpValid[face] = true;
        m_stencilOps[face][0] = sfail;
        m_stencilOps[face][1] = dpfail;
        m_stencilOps[face][2] = dppass;
    }
    glStencilOp(sfail, dpfail, dppass);
}

void GlStateCache::stencilOpSeparate(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass) {
    if (face == GL_FRONT_AND_BACK) {
        stencilOp(sfail, dpfail, dppass);
        return;
    }
    const int i = face == GL_BACK ? 1 : 0;
    if (m_stencilOpValid[i] && m_stencilOps[i][0] == sfail && m_stencilOps[i][1] == dpfail &&
        m_stencilOps[i][2] == dppass) {
        return;
    }
    m_stencilOpValid[i] = true;
    m_stencilOps[i][0] = sfail;
    m_stencilOps[i][1] = dpfail;
    m_stencilOps[i][2] = dppass;
    glStencilOpSeparate(face, sfail, dpfail, dppass);
}

void GlStateCache::colorMask(bool writes) {
    const int v = writes ? 1 : 0;
    if (m_colorMask == v) return;
    m_colorMask = v;
    const GLboolean b = writes ? GL_TRUE : GL_FALSE;
    glColorMask(b, b, b, b);
}

void GlStateCache::clearValuesZero() {
    if (m_clearValid) return;
    m_clearValid = true;
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearStencil(0);
}

void GlStateCache::activeTexture(int unit) {
    if (m_activeUnit == unit) return;
    m_activeUnit = unit;
    glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
}

bool GlStateCache::bindTexture(int unit, GLuint tex) {
    if (unit < 0 || unit >= kUnits) return false;
    if (m_texture[unit] == tex) return false;
    activeTexture(unit);
    m_texture[unit] = tex;
    glBindTexture(GL_TEXTURE_2D, tex);
    ++stats.textureBinds;
    return true;
}

void GlStateCache::bindTextureMs(int unit, GLuint tex) {
    if (unit < 0 || unit >= kUnits) return;
    if (m_textureMs[unit] == tex) return;
    activeTexture(unit);
    m_textureMs[unit] = tex;
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, tex);
    ++stats.textureBinds;
}

// -------------------------------------------------------------------------------------------
// UniformShadow.
// -------------------------------------------------------------------------------------------

bool UniformShadow::update(GLint loc, const void* data, size_t bytes) {
    if (loc < 0) return false;
    const size_t i = static_cast<size_t>(loc);
    if (i >= m_slots.size()) {
        m_slots.resize(i + 1);
    }
    std::vector<uint8_t>& slot = m_slots[i];
    if (slot.size() == bytes && std::memcmp(slot.data(), data, bytes) == 0) {
        return false;
    }
    slot.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + bytes);
    if (stats != nullptr) ++stats->uniformSets;
    return true;
}

void UniformShadow::set1i(GLint loc, GLint v) {
    if (update(loc, &v, sizeof(v))) glUniform1i(loc, v);
}

void UniformShadow::set1f(GLint loc, float v) {
    if (update(loc, &v, sizeof(v))) glUniform1f(loc, v);
}

void UniformShadow::set2f(GLint loc, float a, float b) {
    const float v[2] = {a, b};
    if (update(loc, v, sizeof(v))) glUniform2f(loc, a, b);
}

void UniformShadow::set3f(GLint loc, float a, float b, float c) {
    const float v[3] = {a, b, c};
    if (update(loc, v, sizeof(v))) glUniform3f(loc, a, b, c);
}

void UniformShadow::set4f(GLint loc, float a, float b, float c, float d) {
    const float v[4] = {a, b, c, d};
    if (update(loc, v, sizeof(v))) glUniform4f(loc, a, b, c, d);
}

void UniformShadow::setMat3(GLint loc, const float* m9) {
    if (update(loc, m9, 9 * sizeof(float))) glUniformMatrix3fv(loc, 1, GL_FALSE, m9);
}

void UniformShadow::setMat4(GLint loc, const float* m16) {
    if (update(loc, m16, 16 * sizeof(float))) glUniformMatrix4fv(loc, 1, GL_FALSE, m16);
}

void UniformShadow::set1fv(GLint loc, int count, const float* v) {
    if (count <= 0) return;
    if (update(loc, v, static_cast<size_t>(count) * sizeof(float))) glUniform1fv(loc, count, v);
}

}  // namespace xt::gl
