#include "gl_device.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "gl_shaders.h"
#include "gl_state.h"
#include "gl_util.h"
#include "xt/assert.h"

namespace xt::gl {

namespace {

/// A pooled transient no frame has used for this many frames is deleted (FboPool::endFrame,
/// at most one per frame). Half a minute at 60 Hz (T-163): the pool's byte budget is what
/// bounds memory; this only lets a size a scene stopped using go eventually. T-161's 120-frame
/// window deleted every size a looping animation revisits less often than that, and each such
/// delete burst was a 1-5 ms spike in the next frame.
constexpr uint32_t kTransientIdleFrames = 1800;
/// Floor of the transient pool's byte budget (FboPool::setBudget); `resize` raises it to two
/// full-surface draw targets when those are larger.
constexpr uint64_t kPoolBudgetFloorBytes = 64ull * 1024u * 1024u;
/// Released effect-cache targets kept for reuse (T-163): trimmed beyond this many bytes.
constexpr uint64_t kCachedFreeBudgetBytes = 16ull * 1024u * 1024u;

/// Column-major 3x3 matrix mapping surface-pixel space (origin top-left, y-down) to NDC (y-up)
/// so that the window `(x, y, w, h)` covers the whole clip volume (T-151):
/// NDC.x = 2 (px - x) / w - 1, NDC.y = 1 - 2 (py - y) / h.
void ndcMatrix(int x, int y, int w, int h, float out9[9]) {
    float sx = w > 0 ? 2.0f / static_cast<float>(w) : 0.0f;
    float sy = h > 0 ? 2.0f / static_cast<float>(h) : 0.0f;
    out9[0] = sx;
    out9[1] = 0.0f;
    out9[2] = 0.0f;
    out9[3] = 0.0f;
    out9[4] = -sy;
    out9[5] = 0.0f;
    out9[6] = -1.0f - sx * static_cast<float>(x);
    out9[7] = 1.0f + sy * static_cast<float>(y);
    out9[8] = 1.0f;
}

}  // namespace

bool GlDevice::linkMain(Program& p, const char* frag, std::string& err) {
    p = Program{};
    p.id = linkProgram(shaders::kMainVert, frag, err);
    if (p.id == 0) return false;
    p.uNdc = glGetUniformLocation(p.id, "uNdc");
    p.uKind = glGetUniformLocation(p.id, "uKind");
    p.uP0 = glGetUniformLocation(p.id, "uP0");
    p.uP1 = glGetUniformLocation(p.id, "uP1");
    p.uRadialAxes = glGetUniformLocation(p.id, "uRadialAxes");
    p.uPaintAlpha = glGetUniformLocation(p.id, "uPaintAlpha");
    p.uGamma = glGetUniformLocation(p.id, "uGamma");
    p.uMode = glGetUniformLocation(p.id, "uMode");
    p.uOpacity = glGetUniformLocation(p.id, "uOpacity");
    p.uEffect = glGetUniformLocation(p.id, "uEffect");  // T-178 (kGlyphFrag only).
    p.uRadius = glGetUniformLocation(p.id, "uRadius");
    p.uRect = glGetUniformLocation(p.id, "uRect");
    p.uTexSize = glGetUniformLocation(p.id, "uTexSize");
    // Sampler units and the LUT width never change (T-159): set once. A -1 location is a no-op.
    glUseProgram(p.id);
    glUniform1i(glGetUniformLocation(p.id, "uTex"), 0);
    glUniform1i(glGetUniformLocation(p.id, "uLut"), 0);
    glUniform1i(glGetUniformLocation(p.id, "uSrc"), 0);
    glUniform1i(glGetUniformLocation(p.id, "uDst"), 1);
    glUniform1f(glGetUniformLocation(p.id, "uLutWidth"), static_cast<float>(gpu::kGradientLutWidth));
    return true;
}

bool GlDevice::linkParticle(ParticleProgram& p, const char* frag, std::string& err) {
    p = ParticleProgram{};
    p.id = linkProgram(shaders::kParticleVert, frag, err);
    if (p.id == 0) return false;
    p.uNdc = glGetUniformLocation(p.id, "uNdc");
    p.uXf = glGetUniformLocation(p.id, "uXf");
    p.uStraight = glGetUniformLocation(p.id, "uStraight");
    glUseProgram(p.id);
    glUniform1i(glGetUniformLocation(p.id, "uTex"), 0);
    return true;
}

bool GlDevice::init(std::string& err) {
    shutdown();
    GLint previous = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &previous);
    const bool ok = linkMain(m_solid, shaders::kSolidFrag, err) &&
                    linkMain(m_gradient, shaders::kGradientFrag, err) &&
                    linkMain(m_texture, shaders::kTextureFrag, err) &&
                    linkMain(m_textureStraight, shaders::kTextureStraightFrag, err) &&
                    linkMain(m_glyph, shaders::kGlyphFrag, err) &&
                    linkParticle(m_particleTexture, shaders::kParticleTextureFrag, err) &&
                    linkParticle(m_particleSolid, shaders::kParticleSolidFrag, err);
    bool compositeOk = false;
    if (ok) {
        m_composite = Program{};
        m_composite.id = linkProgram(shaders::kQuadVert, shaders::kCompositeFrag, err);
        if (m_composite.id != 0) {
            m_composite.uNdc = glGetUniformLocation(m_composite.id, "uNdc");
            m_composite.uMode = glGetUniformLocation(m_composite.id, "uMode");
            m_composite.uOpacity = glGetUniformLocation(m_composite.id, "uOpacity");
            glUseProgram(m_composite.id);
            glUniform1i(glGetUniformLocation(m_composite.id, "uSrc"), 0);
            glUniform1i(glGetUniformLocation(m_composite.id, "uDst"), 1);
            compositeOk = true;
        }
    }
    bool resolveOk = false;
    if (ok && compositeOk) {
        m_resolve = Program{};
        m_resolve.id = linkProgram(shaders::kResolveVert, shaders::kResolveFrag, err);
        if (m_resolve.id != 0) {
            m_resolve.uSamples = glGetUniformLocation(m_resolve.id, "uSamples");
            glUseProgram(m_resolve.id);
            glUniform1i(glGetUniformLocation(m_resolve.id, "uSrc"), 0);
            resolveOk = true;
        }
    }
    glUseProgram(static_cast<GLuint>(previous));
    if (!ok || !compositeOk || !resolveOk) return false;
    m_maxTextureSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &m_maxTextureSize);
    if (!m_geometry.init()) {
        err = "geometry init failed";
        return false;
    }
    if (!m_effects.init(err)) {
        return false;
    }
    // T-202: the per-instance attributes of kParticleVert (pointers are set per draw, since GL
    // 3.3 has no base instance; the divisor is VAO state).
    glGenVertexArrays(1, &m_particleVao);
    glGenBuffers(1, &m_particleVbo);
    glBindVertexArray(m_particleVao);
    for (GLuint a = 0; a < 4; ++a) {
        glEnableVertexAttribArray(a);
        glVertexAttribDivisor(a, 1);
    }
    glBindVertexArray(0);
    m_particleCapacity = 0;
    err.clear();
    return true;
}

void GlDevice::uploadParticles(const gpu::GpuFrame& frame) {
    if (frame.particleInstances.empty()) {
        return;
    }
    const size_t bytes = frame.particleInstances.size() * sizeof(gpu::ParticleInstance);
    glBindBuffer(GL_ARRAY_BUFFER, m_particleVbo);
    if (bytes > m_particleCapacity) {
        m_particleCapacity = (bytes + 16383) / 16384 * 16384;
    }
    // Orphan the buffer every frame: the GPU may still read last frame's instances.
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(m_particleCapacity), nullptr, GL_DYNAMIC_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(bytes), frame.particleInstances.data());
    ++m_cache.stats.uploadRanges;
    m_cache.stats.uploadBytes += bytes;
}

void GlDevice::drawParticles(const gpu::DrawCmd& cmd) {
    if (cmd.instanceCount == 0 ||
        static_cast<size_t>(cmd.firstInstance) + cmd.instanceCount > m_frame->particleInstances.size()) {
        return;
    }
    size_t avail = 0;
    const float* p = paramsAt(cmd.paramIndex, avail);
    gpu::ParticleParams pp{};
    if (p == nullptr || avail * sizeof(float) < sizeof(pp)) {
        return;
    }
    std::memcpy(static_cast<void*>(&pp), p, sizeof(pp));
    const bool textured = cmd.shading == gpu::Shading::ParticleTexture;
    ParticleProgram& prog = textured ? m_particleTexture : m_particleSolid;
    m_cache.useProgram(prog.id);
    prog.shadow.stats = &m_cache.stats;
    prog.shadow.setMat3(prog.uNdc, m_ndc);
    const float xf[9] = {pp.xf[0], pp.xf[1], 0.0f, pp.xf[2], pp.xf[3], 0.0f, pp.xf[4], pp.xf[5], 1.0f};
    prog.shadow.setMat3(prog.uXf, xf);
    if (textured) {
        bindSampled(cmd.texture, cmd.sampler);
        prog.shadow.set1f(prog.uStraight, pp.straight);
    }
    m_cache.bindVertexArray(m_particleVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_particleVbo);
    constexpr GLsizei kStride = static_cast<GLsizei>(sizeof(gpu::ParticleInstance));
    const size_t base = static_cast<size_t>(cmd.firstInstance) * sizeof(gpu::ParticleInstance);
    auto at = [base](size_t offset) { return reinterpret_cast<const void*>(base + offset); };
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, kStride, at(offsetof(gpu::ParticleInstance, x)));
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, kStride, at(offsetof(gpu::ParticleInstance, rot)));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, kStride,
                          at(offsetof(gpu::ParticleInstance, rgba)));
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, kStride, at(offsetof(gpu::ParticleInstance, u0)));
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, static_cast<GLsizei>(cmd.instanceCount));
    ++m_cache.stats.drawCalls;
}

void GlDevice::shutdown() {
    for (CachedTarget& c : m_cached) {
        glDeleteFramebuffers(1, &c.fbo);
    }
    m_cached.clear();
    for (FreeCachedTarget& f : m_cachedFree) {
        glDeleteFramebuffers(1, &f.target.fbo);
        glDeleteTextures(1, &f.target.texture);
    }
    m_cachedFree.clear();
    m_cachedFreeBytes = 0;
    for (Texture& t : m_textures) {
        glDeleteTextures(1, &t.name);
    }
    m_textures.clear();
    m_pool.shutdown();
    m_geometry.shutdown();
    m_effects.shutdown();
    m_customEffects.shutdown();
    for (ParticleProgram* p : {&m_particleTexture, &m_particleSolid}) {
        if (p->id != 0) glDeleteProgram(p->id);
        *p = ParticleProgram{};
    }
    if (m_particleVbo != 0) glDeleteBuffers(1, &m_particleVbo);
    if (m_particleVao != 0) glDeleteVertexArrays(1, &m_particleVao);
    m_particleVbo = m_particleVao = 0;
    m_particleCapacity = 0;
    Program* progs[] = {&m_solid, &m_gradient,  &m_texture, &m_textureStraight,
                        &m_glyph, &m_composite, &m_resolve};
    for (Program* p : progs) {
        if (p->id != 0) glDeleteProgram(p->id);
        *p = Program{};
    }
    m_poolSamples = 0;
    m_poolMsaaFailed = false;
}

void GlDevice::resize(int width, int height, int samples, int maxSamples) {
    m_width = width;
    m_height = height;
    m_poolSamples = (samples > 1 && samples <= maxSamples) ? samples : 0;
    // Two full-surface draw targets (color at 4 bytes per sample plus a 4-byte stencil), or the
    // floor: enough that a looping scene's transient sizes all stay resident (docs/tasks/T-163.md:
    // challenge-menu at 1280x720 4x settles at ~60 MB; a 32 MB budget churns).
    const uint64_t perSample = m_poolSamples > 0 ? uint64_t(m_poolSamples) : 1u;
    const uint64_t surfaceEntry = uint64_t(width) * uint64_t(height) * perSample * 8u;
    m_pool.setBudget(std::max(kPoolBudgetFloorBytes, 2u * surfaceEntry));
    m_cache.stats.transientDeletes += m_pool.purgeUnused(width, height);
    // Cached targets are window-sized: a resize invalidates every size (the builder flushes).
    for (FreeCachedTarget& f : m_cachedFree) {
        glDeleteFramebuffers(1, &f.target.fbo);
        glDeleteTextures(1, &f.target.texture);
    }
    m_cachedFree.clear();
    m_cachedFreeBytes = 0;
}

// -------------------------------------------------------------------------------------------
// Textures.
// -------------------------------------------------------------------------------------------

gpu::TextureHandle GlDevice::createTexture(const char* debugName, uint16_t w, uint16_t h,
                                           gpu::TextureFormat fmt, const void* initial) {
    (void)debugName;
    if (fmt == gpu::TextureFormat::R32F) {
        return 0;  // transient targets only
    }
    // docs/tasks/T-263b.md E10: glTexImage2D would raise GL_INVALID_VALUE and leave an incomplete
    // texture that samples as opaque black. No texture: the builder leaves the image out.
    if (m_maxTextureSize > 0 && (w > m_maxTextureSize || h > m_maxTextureSize)) {
        return 0;
    }
    // T-199 (gpu_frame.h TextureFormat::Bc*): upload the blocks as they are; a driver without the
    // extension raises GL_INVALID_ENUM and the builder falls back to the Rgba8 image.
    if (fmt == gpu::TextureFormat::Bc1 || fmt == gpu::TextureFormat::Bc3 || fmt == gpu::TextureFormat::Bc7) {
        if (initial == nullptr || w == 0 || h == 0) {
            return 0;
        }
        const GLenum internalFormat = fmt == gpu::TextureFormat::Bc1   ? GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
                                      : fmt == gpu::TextureFormat::Bc3 ? GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
                                                                       : GL_COMPRESSED_RGBA_BPTC_UNORM_ARB;
        const size_t blockBytes = fmt == gpu::TextureFormat::Bc1 ? 8 : 16;
        const GLsizei imageSize = static_cast<GLsizei>(((static_cast<size_t>(w) + 3) / 4) *
                                                       ((static_cast<size_t>(h) + 3) / 4) * blockBytes);
        while (glGetError() != 0) {
        }
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_LINEAR));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_LINEAR));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(GL_CLAMP_TO_EDGE));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(GL_CLAMP_TO_EDGE));
        setUploadUnpackState();  // E9: `initial` is memory, not an offset into a host's buffer
        glCompressedTexImage2D(GL_TEXTURE_2D, 0, internalFormat, w, h, 0, imageSize, initial);
        m_cache.invalidateTextures();
        if (glGetError() != 0) {
            glDeleteTextures(1, &tex);
            return 0;
        }
        m_textures.push_back(Texture{tex, fmt, GL_LINEAR});
        return tex;
    }
    while (glGetError() != 0) {
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_LINEAR));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_LINEAR));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(GL_CLAMP_TO_EDGE));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(GL_CLAMP_TO_EDGE));
    // docs/tasks/T-263b.md E9: `initial` is tightly packed client memory, whatever row length,
    // skips or pixel unpack buffer the host left set (GlRenderer restores them).
    setUploadUnpackState();
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (fmt == gpu::TextureFormat::R8) {
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_R8), w, h, 0, GL_RED, GL_UNSIGNED_BYTE, initial);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA8), w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     initial);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    m_cache.invalidateTextures();
    // E10: a failed upload (out of memory, a size the driver refuses after all) is no texture.
    if (glGetError() != 0) {
        glDeleteTextures(1, &tex);
        return 0;
    }
    m_textures.push_back(Texture{tex, fmt, GL_LINEAR});
    return tex;
}

void GlDevice::updateTexture(gpu::TextureHandle tex, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                             const void* data, size_t rowPitch) {
    gpu::TextureFormat fmt = gpu::TextureFormat::Rgba8;
    bool found = false;
    for (const Texture& t : m_textures) {
        if (t.name == tex) {
            fmt = t.format;
            found = true;
            break;
        }
    }
    if (!found || w == 0 || h == 0 || fmt == gpu::TextureFormat::Bc1 || fmt == gpu::TextureFormat::Bc3 ||
        fmt == gpu::TextureFormat::Bc7) {
        return;  // T-199: compressed textures are never updated in place.
    }
    const GLenum format = fmt == gpu::TextureFormat::R8 ? GL_RED : GL_RGBA;
    const size_t bpp = fmt == gpu::TextureFormat::R8 ? 1 : 4;
    glBindTexture(GL_TEXTURE_2D, tex);
    m_cache.invalidateTextures();
    setUploadUnpackState();  // E9, as in createTexture
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    if (rowPitch == static_cast<size_t>(w) * bpp) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, format, GL_UNSIGNED_BYTE, bytes);
    } else if (rowPitch % bpp == 0 && rowPitch > static_cast<size_t>(w) * bpp) {
        // docs/tasks/T-263c.md ruling 8: rows wider than the region (a glyph atlas page's dirty
        // rectangle) go up in one call; the row length is in texels.
        glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(rowPitch / bpp));
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, format, GL_UNSIGNED_BYTE, bytes);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    } else {
        // A pitch that is not a whole number of texels cannot be a row length: row by row.
        for (uint16_t row = 0; row < h; ++row) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, x, y + row, w, 1, format, GL_UNSIGNED_BYTE,
                            bytes + static_cast<size_t>(row) * rowPitch);
        }
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

void GlDevice::destroyTexture(gpu::TextureHandle tex) {
    if (tex == 0) {
        return;
    }
    for (size_t i = 0; i < m_textures.size(); ++i) {
        if (m_textures[i].name == tex) {
            GLuint name = tex;
            glDeleteTextures(1, &name);
            m_textures.erase(m_textures.begin() + static_cast<std::ptrdiff_t>(i));
            m_cache.invalidateTextures();
            return;
        }
    }
}

gpu::TextureHandle GlDevice::acquireCachedTarget(uint16_t w, uint16_t h, gpu::TextureFormat fmt) {
    if (fmt != gpu::TextureFormat::Rgba8 || w == 0 || h == 0) {
        return 0;
    }
    // T-163: a released target of the same size is reused as it is (its content is dead: the
    // store overwrites the whole texture).
    for (size_t i = 0; i < m_cachedFree.size(); ++i) {
        const CachedTarget c = m_cachedFree[i].target;  // by value: erased below
        if (c.width != int(w) || c.height != int(h)) continue;
        m_cachedFreeBytes -= uint64_t(c.width) * uint64_t(c.height) * 4u;
        m_cached.push_back(c);
        m_textures.push_back(Texture{c.texture, gpu::TextureFormat::Rgba8, GL_LINEAR});
        m_cachedFree.erase(m_cachedFree.begin() + static_cast<std::ptrdiff_t>(i));
        return c.texture;
    }
    // Called from FrameBuilder (init's probe, a build), outside submit: leave the host's
    // framebuffer binding as it was (GlRenderer::resize reads GL_SAMPLES from the bound FBO).
    GLint prevDraw = 0, prevRead = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
    CachedTarget c;
    c.width = w;
    c.height = h;
    glGenTextures(1, &c.texture);
    glBindTexture(GL_TEXTURE_2D, c.texture);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA8), w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_LINEAR));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_LINEAR));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(GL_CLAMP_TO_EDGE));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(GL_CLAMP_TO_EDGE));
    glGenFramebuffers(1, &c.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, c.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c.texture, 0);
    const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prevDraw));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevRead));
    m_cache.invalidateFramebuffers();
    m_cache.invalidateTextures();
    if (!ok) {
        glDeleteFramebuffers(1, &c.fbo);
        glDeleteTextures(1, &c.texture);
        return 0;
    }
    m_cached.push_back(c);
    m_textures.push_back(Texture{c.texture, gpu::TextureFormat::Rgba8, GL_LINEAR});
    ++m_cache.stats.cachedTargetCreates;
    return c.texture;
}

void GlDevice::releaseCachedTarget(gpu::TextureHandle target) {
    if (target == 0) {
        return;
    }
    for (size_t i = 0; i < m_cached.size(); ++i) {
        if (m_cached[i].texture != target) continue;
        const CachedTarget c = m_cached[i];
        m_cached.erase(m_cached.begin() + static_cast<std::ptrdiff_t>(i));
        // Drop the texture entry (no GL delete) so sampling state is forgotten with it.
        for (size_t t = 0; t < m_textures.size(); ++t) {
            if (m_textures[t].name == c.texture) {
                m_textures.erase(m_textures.begin() + static_cast<std::ptrdiff_t>(t));
                break;
            }
        }
        m_cache.invalidateTextures();
        m_cachedFree.push_back(FreeCachedTarget{c, m_frameCounter});
        m_cachedFreeBytes += uint64_t(c.width) * uint64_t(c.height) * 4u;
        ++m_cache.stats.cachedTargetReleases;
        trimCachedFree();
        return;
    }
}

void GlDevice::destroyCachedTarget(const CachedTarget& c) {
    GLuint fbo = c.fbo, tex = c.texture;
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    m_cache.invalidateFramebuffers();
    m_cache.invalidateTextures();
}

void GlDevice::trimCachedFree() {
    while (m_cachedFreeBytes > kCachedFreeBudgetBytes && !m_cachedFree.empty()) {
        size_t oldest = 0;
        for (size_t i = 1; i < m_cachedFree.size(); ++i) {
            if (m_cachedFree[i].releasedFrame < m_cachedFree[oldest].releasedFrame) oldest = i;
        }
        const CachedTarget c = m_cachedFree[oldest].target;
        m_cachedFree.erase(m_cachedFree.begin() + static_cast<std::ptrdiff_t>(oldest));
        m_cachedFreeBytes -= uint64_t(c.width) * uint64_t(c.height) * 4u;
        destroyCachedTarget(c);
    }
}

void GlDevice::bindSampled(GLuint tex, gpu::Sampler sampler) {
    const GLenum filter = sampler == gpu::Sampler::PointClamp ? GL_NEAREST : GL_LINEAR;
    m_cache.bindTexture(0, tex);
    bool ours = false;
    for (Texture& t : m_textures) {
        if (t.name != tex) continue;
        if (t.filter == filter) return;
        t.filter = filter;
        ours = true;
        break;
    }
    // Our own texture whose filter changed, or a host texture (T-150) whose parameters we do
    // not track: set the filter on the texture bound to unit 0.
    m_cache.activeTexture(0);
    if (!ours) {
        // docs/tasks/T-263b.md E5: the host's texture gets its own filter back after this draw
        // (restoreHostFilter()), so what it had is read first.
        GLint minFilter = 0, magFilter = 0;
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minFilter);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &magFilter);
        if (minFilter == static_cast<GLint>(filter) && magFilter == static_cast<GLint>(filter)) {
            return;
        }
        m_hostFilter = HostFilter{tex, minFilter, magFilter};
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(filter));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(filter));
}

void GlDevice::restoreHostFilter() {
    if (m_hostFilter.texture == 0) {
        return;
    }
    m_cache.bindTexture(0, m_hostFilter.texture);
    m_cache.activeTexture(0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, m_hostFilter.minFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, m_hostFilter.magFilter);
    m_hostFilter = HostFilter{};
}

// -------------------------------------------------------------------------------------------
// Targets.
// -------------------------------------------------------------------------------------------

int GlDevice::acquirePooled(int w, int h, int samples, GLenum internalFormat, bool stencil) {
    bool created = false;
    const int handle = m_pool.acquire(w, h, samples, internalFormat, stencil, &created);
    if (created) {
        // createEntry bound its framebuffer, texture and renderbuffers behind the cache's back.
        m_cache.invalidateFramebuffers();
        m_cache.invalidateTextures();
        ++m_cache.stats.transientCreates;
    }
    return handle;
}

int GlDevice::acquireDrawTarget(const gpu::TransientTargetDesc& desc) {
    const GLenum fmt = desc.format == gpu::TextureFormat::R32F ? GL_R32F : GL_RGBA8;
    const bool stencil = desc.needStencil != 0 && fmt == GL_RGBA8;
    if (stencil && m_poolSamples > 0 && !m_poolMsaaFailed) {
        int handle = acquirePooled(desc.width, desc.height, m_poolSamples, fmt, true);
        if (handle >= 0) {
            return handle;
        }
        m_poolMsaaFailed = true;
    }
    return acquirePooled(desc.width, desc.height, 0, fmt, stencil);
}

GLuint GlDevice::fboOf(gpu::TargetHandle target) {
    if (target == gpu::kSurfaceTarget) {
        return m_surface;
    }
    const int handle = m_handles[target - 1];
    return handle >= 0 ? m_pool.get(handle).fbo : 0;
}

GLuint GlDevice::textureOf(gpu::TargetHandle target) {
    if (target == gpu::kSurfaceTarget) {
        return 0;  // the surface is never sampled (the builder copies it into a transient first)
    }
    const size_t i = target - 1;
    const int handle = m_handles[i];
    if (handle < 0) {
        return 0;
    }
    // By value: acquiring the resolve target below may grow the pool's vector.
    const GlFbo entry = m_pool.get(handle);
    if (entry.samples == 0) {
        return entry.colorTex;
    }
    if (m_resolved[i] < 0) {
        int resolved = acquirePooled(entry.width, entry.height, 0, entry.internalFormat, false);
        if (resolved < 0) {
            m_lastError = "failed to acquire a pooled FBO for the multisample resolve";
            return 0;
        }
        // Resolve with a draw, not glBlitFramebuffer (T-161): a multisample blit carries a fixed
        // cost of ~30 us on both the driver thread and the GPU, and a frame has ~20 of them; the
        // triangle below is an ordinary draw. The whole transient is resolved: scissor off, no
        // blend, no stencil, all channels, viewport = the entry. Attributes are not read, so the
        // frame's VAO (already bound) serves. The caller re-applies its own draw framebuffer,
        // viewport and per-command state through the cache.
        m_cache.bindDrawFramebuffer(m_pool.get(resolved).fbo);
        m_cache.viewport(0, 0, entry.width, entry.height);
        m_cache.scissorTest(false);
        m_cache.blendSrcOver(false);
        m_cache.stencilTest(false);
        m_cache.colorMask(true);
        m_cache.useProgram(m_resolve.id);
        m_resolve.shadow.stats = &m_cache.stats;
        m_resolve.shadow.set1i(m_resolve.uSamples, entry.samples);
        m_cache.bindTextureMs(0, entry.colorTex);
        m_geometry.bindFrame(m_cache);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        // Leave no multisample source bound while its transient is drawn into again later
        // (rendering feedback loop rules).
        m_cache.bindTextureMs(0, 0);
        ++m_cache.stats.resolves;
        m_resolved[i] = resolved;
    }
    return m_pool.get(m_resolved[i]).colorTex;
}

GLuint GlDevice::readFboOf(gpu::TargetHandle target) {
    if (target == gpu::kSurfaceTarget || target > m_handles.size()) {
        return 0;
    }
    const size_t i = target - 1;
    const int handle = m_handles[i];
    if (handle < 0) {
        return 0;
    }
    if (m_pool.get(handle).samples == 0) {
        return m_pool.get(handle).fbo;
    }
    if (textureOf(target) == 0) {
        return 0;
    }
    return m_pool.get(m_resolved[i]).fbo;
}

void GlDevice::executeCacheStore(const gpu::CacheStore& store) {
    const CachedTarget* cached = nullptr;
    for (const CachedTarget& c : m_cached) {
        if (c.texture == store.texture) {
            cached = &c;
            break;
        }
    }
    if (cached == nullptr) {
        m_lastError = "cache store into a target this device did not acquire";
        return;
    }
    const GLuint readFbo = readFboOf(store.src);
    if (readFbo == 0) {
        m_lastError = "cache store from a transient without content";
        return;
    }
    int w = 0, h = 0;
    sizeOf(store.src, w, h);
    w = std::min(w, cached->width);
    h = std::min(h, cached->height);
    // Rows flipped (gpu_frame.h `CacheStore`): a later frame samples the copy as an uploaded,
    // top-down texture. The next pass re-binds its own draw framebuffer through the cache.
    m_cache.scissorTest(false);
    m_cache.bindReadFramebuffer(readFbo);
    m_cache.bindDrawFramebuffer(cached->fbo);
    glBlitFramebuffer(0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    ++m_cache.stats.blits;
    ++m_cache.stats.cacheStores;
}

void GlDevice::invalidateResolve(gpu::TargetHandle target) {
    if (target == gpu::kSurfaceTarget) {
        return;
    }
    int& r = m_resolved[target - 1];
    if (r >= 0) {
        m_pool.release(r);
        r = -1;
    }
}

GlDevice::Window GlDevice::windowOf(gpu::TargetHandle target) const {
    if (target == gpu::kSurfaceTarget || target > m_windows.size()) {
        return Window{0, 0, m_width, m_height};
    }
    return m_windows[target - 1];
}

void GlDevice::sizeOf(gpu::TargetHandle target, int& w, int& h) const {
    if (target == gpu::kSurfaceTarget || m_frame == nullptr || target > m_frame->transients.size()) {
        w = m_width;
        h = m_height;
        return;
    }
    const gpu::TransientTargetDesc& d = m_frame->transients[target - 1];
    w = d.width;
    h = d.height;
}

// -------------------------------------------------------------------------------------------
// Per-command state (gpu_frame.h's tables).
// -------------------------------------------------------------------------------------------

void GlDevice::applyScissor(const gpu::DrawCmd& cmd) {
    m_cache.scissorTest(true);
    // Surface pixels: clip to the pass window, then move into the target's own pixel space. GL's
    // scissor origin is bottom-left.
    const int x0 = std::max<int>(cmd.clipX, m_passWindow.x);
    const int y0 = std::max<int>(cmd.clipY, m_passWindow.y);
    const int x1 = std::min<int>(cmd.clipX + cmd.clipW, m_passWindow.x + m_passWindow.w);
    const int y1 = std::min<int>(cmd.clipY + cmd.clipH, m_passWindow.y + m_passWindow.h);
    if (x1 <= x0 || y1 <= y0) {
        m_cache.scissor(0, 0, 0, 0);
        return;
    }
    if (m_scaleX != 1.0f || m_scaleY != 1.0f) {
        // T-343: a scaled transient; the window maps onto the whole (smaller) target.
        const int sx0 = static_cast<int>(std::floor(static_cast<float>(x0 - m_originX) * m_scaleX));
        const int sx1 = static_cast<int>(std::ceil(static_cast<float>(x1 - m_originX) * m_scaleX));
        const int sy0 = static_cast<int>(std::floor(static_cast<float>(y0 - m_originY) * m_scaleY));
        const int sy1 = static_cast<int>(std::ceil(static_cast<float>(y1 - m_originY) * m_scaleY));
        m_cache.scissor(sx0, m_targetH - sy1, std::max(0, sx1 - sx0), std::max(0, sy1 - sy0));
        return;
    }
    m_cache.scissor(x0 - m_originX, m_targetH - (y0 - m_originY) - (y1 - y0), x1 - x0, y1 - y0);
}

void GlDevice::applyStencil(const gpu::DrawCmd& cmd) {
    using gpu::StencilOp;
    const GLint depthRef = static_cast<GLint>(cmd.stencilRef) << 4;
    if (cmd.stencilOp == StencilOp::None) {
        m_cache.stencilTest(false);
        return;
    }
    m_cache.stencilTest(true);
    switch (cmd.stencilOp) {
        case StencilOp::WriteIncrWrap:
            m_cache.stencilFunc(GL_EQUAL, depthRef, 0xF0u);
            m_cache.stencilMask(0x0Fu);
            m_cache.stencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, GL_INCR_WRAP);
            m_cache.stencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, GL_DECR_WRAP);
            break;
        case StencilOp::WriteInvert:
            m_cache.stencilFunc(GL_EQUAL, depthRef, 0xF0u);
            m_cache.stencilMask(0x0Fu);
            m_cache.stencilOp(GL_KEEP, GL_KEEP, GL_INVERT);
            break;
        case StencilOp::TestNotEqualZero:
            m_cache.stencilFunc(GL_NOTEQUAL, 0, 0x0Fu);
            m_cache.stencilMask(0x00u);
            m_cache.stencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
            break;
        case StencilOp::TestEqualRef:
            m_cache.stencilFunc(GL_EQUAL, depthRef, 0xF0u);
            m_cache.stencilMask(0x00u);
            m_cache.stencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
            break;
        case StencilOp::ClearRect:
            m_cache.stencilFunc(GL_ALWAYS, 0, 0xFFu);
            m_cache.stencilMask(0x0Fu);
            m_cache.stencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
            break;
        case StencilOp::ClipPush:
            // GL compares `(ref & mask) != (stencil & mask)`: with mask 0x0F and ref's low
            // nibble 0 this passes where the winding nibble is non-zero; REPLACE then writes the
            // full ref (`newDepth << 4`), clearing the winding nibble in the same draw.
            m_cache.stencilFunc(GL_NOTEQUAL, depthRef, 0x0Fu);
            m_cache.stencilMask(0xFFu);
            m_cache.stencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
            break;
        case StencilOp::ClipPop:
            m_cache.stencilFunc(GL_EQUAL, depthRef, 0xF0u);
            m_cache.stencilMask(0xF0u);
            m_cache.stencilOp(GL_KEEP, GL_KEEP, GL_DECR);
            break;
        case StencilOp::None:
        default:
            m_cache.stencilTest(false);
            break;
    }
}

const float* GlDevice::paramsAt(uint32_t index, size_t& available) const {
    if (index >= m_frame->params.size()) {
        available = 0;
        return nullptr;
    }
    available = m_frame->params.size() - index;
    return m_frame->params.data() + index;
}

void GlDevice::drawCmd(const gpu::DrawCmd& cmd) {
    using gpu::Shading;
    // Inputs first: resolving a multisampled input changes the draw framebuffer, viewport and
    // draw state, and the per-command state below puts everything back only when it differs.
    GLuint srcTex = 0, auxTex = 0;
    if (cmd.shading == Shading::Composite || cmd.shading == Shading::Effect) {
        srcTex = textureOf(cmd.src);
        auxTex = cmd.aux != 0 ? textureOf(cmd.aux) : 0;
        m_cache.bindDrawFramebuffer(m_currentFbo);
        m_cache.viewport(m_passViewport[0], m_passViewport[1], m_passViewport[2], m_passViewport[3]);
    }
    applyScissor(cmd);
    if (cmd.blend == gpu::Blend::Additive) {
        m_cache.blendAdditive();  // T-202
    } else {
        m_cache.blendSrcOver(cmd.blend == gpu::Blend::SrcOver);
    }
    applyStencil(cmd);
    m_cache.colorMask(cmd.shading != Shading::StencilOnly);
    if (cmd.kind == gpu::DrawKind::Particles) {
        drawParticles(cmd);  // T-202: instanced, its own VAO.
        return;
    }
    m_geometry.bindFrame(m_cache);

    switch (cmd.shading) {
        case Shading::Solid:
        case Shading::StencilOnly:
            m_cache.useProgram(m_solid.id);
            m_solid.shadow.stats = &m_cache.stats;
            m_solid.shadow.setMat3(m_solid.uNdc, m_ndc);
            m_geometry.drawRange(cmd.firstIndex, cmd.indexCount, cmd.baseVertex, m_cache.stats);
            return;
        case Shading::Gradient: {
            size_t avail = 0;
            const float* p = paramsAt(cmd.paramIndex, avail);
            gpu::GradientParams gp{};
            if (p == nullptr || avail * sizeof(float) < sizeof(gp)) {
                return;
            }
            std::memcpy(&gp, p, sizeof(gp));
            Program& prog = m_gradient;
            m_cache.useProgram(prog.id);
            prog.shadow.stats = &m_cache.stats;
            prog.shadow.setMat3(prog.uNdc, m_ndc);
            bindSampled(cmd.texture, gpu::Sampler::LinearClamp);
            prog.shadow.set1i(prog.uKind, static_cast<int>(gp.kind));
            prog.shadow.set2f(prog.uP0, gp.p0x, gp.p0y);
            prog.shadow.set2f(prog.uP1, gp.p1x, gp.p1y);
            prog.shadow.set2f(prog.uRadialAxes, gp.rx, gp.ry);
            prog.shadow.set1f(prog.uPaintAlpha, gp.alpha);
            m_geometry.drawRange(cmd.firstIndex, cmd.indexCount, cmd.baseVertex, m_cache.stats);
            return;
        }
        case Shading::Texture:
        case Shading::TextureStraight: {
            // T-150: a host texture (TextureStraight, or Texture bound premultiplied) is a GL
            // texture name this device did not create; sampling it needs nothing more.
            Program& prog = cmd.shading == Shading::Texture ? m_texture : m_textureStraight;
            m_cache.useProgram(prog.id);
            prog.shadow.stats = &m_cache.stats;
            prog.shadow.setMat3(prog.uNdc, m_ndc);
            bindSampled(cmd.texture, cmd.sampler);
            m_geometry.drawRange(cmd.firstIndex, cmd.indexCount, cmd.baseVertex, m_cache.stats);
            return;
        }
        case Shading::Glyph: {
            size_t avail = 0;
            const float* p = paramsAt(cmd.paramIndex, avail);
            gpu::GlyphParams gp{1.0f};
            if (p != nullptr && avail * sizeof(float) >= sizeof(gp)) {
                std::memcpy(static_cast<void*>(&gp), p, sizeof(gp));
            }
            Program& prog = m_glyph;
            m_cache.useProgram(prog.id);
            prog.shadow.stats = &m_cache.stats;
            prog.shadow.setMat3(prog.uNdc, m_ndc);
            bindSampled(cmd.texture, gpu::Sampler::LinearClamp);
            prog.shadow.set1f(prog.uGamma, gp.gamma);
            // T-178: text effect (0 = plain coverage).
            prog.shadow.set1i(prog.uEffect, static_cast<GLint>(gp.effect));
            if (gp.effect != 0.0f) {
                prog.shadow.set1f(prog.uRadius, gp.radius);
                prog.shadow.set4f(prog.uRect, gp.rectX0, gp.rectY0, gp.rectX1, gp.rectY1);
                prog.shadow.set1f(prog.uTexSize, gp.texSize);
            }
            m_geometry.drawRange(cmd.firstIndex, cmd.indexCount, cmd.baseVertex, m_cache.stats);
            return;
        }
        case Shading::Composite: {
            size_t avail = 0;
            const float* p = paramsAt(cmd.paramIndex, avail);
            gpu::CompositeParams cp{0.0f, 1.0f};
            if (p != nullptr && avail * sizeof(float) >= sizeof(cp)) {
                std::memcpy(&cp, p, sizeof(cp));
            }
            Program& prog = m_composite;
            m_cache.useProgram(prog.id);
            prog.shadow.stats = &m_cache.stats;
            m_cache.bindTexture(1, auxTex);
            m_cache.bindTexture(0, srcTex);
            prog.shadow.set1i(prog.uMode, static_cast<int>(cp.mode));
            prog.shadow.set1f(prog.uOpacity, cp.opacity);
            prog.shadow.setMat3(prog.uNdc, m_ndc);
            m_geometry.drawRange(cmd.firstIndex, cmd.indexCount, cmd.baseVertex, m_cache.stats);
            return;
        }
        case Shading::Effect: {
            size_t avail = 0;
            const float* p = paramsAt(cmd.paramIndex, avail);
            if (cmd.program == gpu::EffectProgram::Custom) {
                // T-203: a `.xtfx` program prepared during the build.
                if (!m_customEffects.bind(p, avail, srcTex, m_ndc, m_cache)) {
                    m_lastError = "unknown custom effect program";
                    return;
                }
                m_geometry.drawRange(cmd.firstIndex, cmd.indexCount, cmd.baseVertex, m_cache.stats);
                return;
            }
            if (!m_effects.bind(cmd.program, p, avail, srcTex, auxTex, cmd.texture, m_ndc, m_cache)) {
                m_lastError = "unsupported effect program";
                return;
            }
            m_geometry.drawRange(cmd.firstIndex, cmd.indexCount, cmd.baseVertex, m_cache.stats);
            return;
        }
        case Shading::ParticleTexture:
        case Shading::ParticleSolid:
            return;  // T-202: only DrawKind::Particles uses them (drawn above).
    }
}

uint32_t GlDevice::prepareCustomEffect(std::string_view glsl, const char* names, const uint8_t* types,
                                       uint32_t count) {
    std::string warning;
    const uint32_t id = m_customEffects.prepare(glsl, names, types, count, warning);
    if (!warning.empty()) {
        if (!m_warning.empty()) {
            m_warning += "\n";
        }
        m_warning += warning;
    }
    return id;
}

// -------------------------------------------------------------------------------------------
// submit().
// -------------------------------------------------------------------------------------------

void GlDevice::submit(const gpu::GpuFrame& frame) {
    using Clock = std::chrono::steady_clock;
    const auto msSince = [](Clock::time_point t0) {
        return std::chrono::duration<float, std::milli>(Clock::now() - t0).count();
    };
    m_frame = &frame;
    m_lastError.clear();
    // The host may have changed anything since the last submit: every shadow starts unknown.
    // The counters were zeroed by beginFrame (T-163); a host that calls submit directly keeps
    // accumulating until the next beginFrame.
    m_cache.reset();
    auto t0 = Clock::now();
    m_geometry.uploadFrame(frame.vertices, frame.indices, m_cache);
    uploadParticles(frame);  // T-202
    m_cache.stats.uploadMs += msSince(t0);
    t0 = Clock::now();

    m_handles.assign(frame.transients.size(), -1);
    m_resolved.assign(frame.transients.size(), -1);
    m_windows.assign(frame.transients.size(), Window{});
    for (size_t i = 0; i < frame.transients.size(); ++i) {
        m_windows[i] = Window{0, 0, frame.transients[i].width, frame.transients[i].height};
    }
    bool ok = true;
    for (size_t i = 0; i < frame.transients.size(); ++i) {
        m_handles[i] = acquireDrawTarget(frame.transients[i]);
        if (m_handles[i] < 0) {
            m_lastError = "failed to acquire a pooled FBO for a transient target";
            ok = false;
            break;
        }
    }
    m_cache.stats.targetMs += msSince(t0);
    t0 = Clock::now();

    if (ok) {
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        size_t nextStore = 0;  // frame.cacheStores is in pass order
        for (size_t passIndex = 0; passIndex < frame.passes.size(); ++passIndex) {
            const gpu::Pass& pass = frame.passes[passIndex];
            ++m_cache.stats.passes;
            invalidateResolve(pass.target);
            const GLuint fbo = fboOf(pass.target);
            m_currentFbo = fbo;
            m_cache.bindDrawFramebuffer(fbo);
            // The pass window (surface pixels; 0 = the whole target). A transient's own origin is
            // its window's top-left; the surface's is (0,0).
            sizeOf(pass.target, m_targetW, m_targetH);
            Window win{pass.viewX, pass.viewY, pass.viewW, pass.viewH};
            if (win.w <= 0 || win.h <= 0) {
                win = pass.target == gpu::kSurfaceTarget ? Window{0, 0, m_targetW, m_targetH}
                                                         : windowOf(pass.target);
            }
            if (pass.target != gpu::kSurfaceTarget && pass.target <= m_windows.size()) {
                m_windows[pass.target - 1] = win;
                m_originX = win.x;
                m_originY = win.y;
            } else {
                m_originX = 0;
                m_originY = 0;
            }
            m_passWindow = win;
            ndcMatrix(win.x, win.y, win.w, win.h, m_ndc);
            m_scaleX = m_scaleY = 1.0f;
            if (pass.target != gpu::kSurfaceTarget && (m_targetW != win.w || m_targetH != win.h)) {
                // T-343: a scaled transient covers its whole target with the window.
                m_scaleX = static_cast<float>(m_targetW) / static_cast<float>(std::max(1, win.w));
                m_scaleY = static_cast<float>(m_targetH) / static_cast<float>(std::max(1, win.h));
                m_passViewport[0] = 0;
                m_passViewport[1] = 0;
                m_passViewport[2] = m_targetW;
                m_passViewport[3] = m_targetH;
            } else {
                m_passViewport[0] = win.x - m_originX;
                m_passViewport[1] = m_targetH - (win.y - m_originY) - win.h;
                m_passViewport[2] = win.w;
                m_passViewport[3] = win.h;
            }
            m_cache.viewport(m_passViewport[0], m_passViewport[1], m_passViewport[2], m_passViewport[3]);
            if (pass.clear != 0) {
                m_cache.scissorTest(false);
                m_cache.colorMask(true);
                m_cache.stencilMask(0xFFu);
                m_cache.clearValuesZero();
                glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
                ++m_cache.stats.clears;
            }
            if (pass.copy != 0) {
                // Copy the overlap of the source's and this pass's windows (surface pixels),
                // each side expressed in its own target's bottom-up pixel space.
                const Window sw = windowOf(pass.copySource);
                int srcW = 0, srcH = 0;
                sizeOf(pass.copySource, srcW, srcH);
                const int sox = pass.copySource == gpu::kSurfaceTarget ? 0 : sw.x;
                const int soy = pass.copySource == gpu::kSurfaceTarget ? 0 : sw.y;
                const int x0 = std::max(win.x, sw.x), y0 = std::max(win.y, sw.y);
                const int x1 = std::min(win.x + win.w, sw.x + sw.w),
                          y1 = std::min(win.y + win.h, sw.y + sw.h);
                if (x1 > x0 && y1 > y0) {
                    const int sy0 = srcH - (y0 - soy) - (y1 - y0);
                    const int dy0 = m_targetH - (y0 - m_originY) - (y1 - y0);
                    m_cache.scissorTest(false);
                    m_cache.bindReadFramebuffer(fboOf(pass.copySource));
                    m_cache.bindDrawFramebuffer(fbo);
                    glBlitFramebuffer(x0 - sox, sy0, x1 - sox, sy0 + (y1 - y0), x0 - m_originX, dy0,
                                      x1 - m_originX, dy0 + (y1 - y0), GL_COLOR_BUFFER_BIT, GL_NEAREST);
                    ++m_cache.stats.blits;
                }
            }
            for (uint32_t c = pass.firstCmd; c < pass.firstCmd + pass.cmdCount && c < frame.cmds.size();
                 ++c) {
                drawCmd(frame.cmds[c]);
                restoreHostFilter();  // E5: only after a draw that changed a host texture's filter
            }
            while (nextStore < frame.cacheStores.size() && frame.cacheStores[nextStore].pass <= passIndex) {
                if (frame.cacheStores[nextStore].pass == passIndex) {
                    executeCacheStore(frame.cacheStores[nextStore]);
                }
                ++nextStore;
            }
#ifndef NDEBUG
            GLenum err = glGetError();
            XT_ASSERT(err == GL_NO_ERROR);
            (void)err;
#endif
        }
    }

    m_cache.stats.drawMs += msSince(t0);
    t0 = Clock::now();
    for (size_t i = 0; i < m_handles.size(); ++i) {
        if (m_resolved[i] >= 0) m_pool.release(m_resolved[i]);
        if (m_handles[i] >= 0) m_pool.release(m_handles[i]);
    }
    m_cache.stats.transientDeletes += m_pool.endFrame(kTransientIdleFrames);
    ++m_frameCounter;
    m_cache.stats.endFrameMs += msSince(t0);
    m_cache.stats.poolEntries = m_pool.liveCount();
    m_cache.stats.poolBytes = m_pool.liveBytes();
    m_cache.stats.cachedTargets = static_cast<uint32_t>(m_cached.size() + m_cachedFree.size());
    uint64_t cachedBytes = m_cachedFreeBytes;
    for (const CachedTarget& c : m_cached) {
        cachedBytes += uint64_t(c.width) * uint64_t(c.height) * 4u;
    }
    m_cache.stats.cachedBytes = cachedBytes;
    m_handles.clear();
    m_resolved.clear();
    m_windows.clear();
    m_frame = nullptr;
}

}  // namespace xt::gl
