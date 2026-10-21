#include "gl_fbo.h"

namespace xt::gl {

namespace {
/// Whether a stencil attachment is tried as GL_STENCIL_INDEX8 before GL_DEPTH24_STENCIL8
/// (depth is never tested, gpu_frame.h). Off (T-163): on NVIDIA (RTX 4070 Ti, 616.92) a
/// multisample stencil-only renderbuffer completes and takes a quarter of the memory
/// (challenge-menu 1280x720 4x: process private 276 vs 323 MB) but costs GPU time -- steady
/// 0.165 -> 0.180 ms, static x16 1.40 -> 1.61 ms -- so the packed format stays the default.
constexpr bool kStencilOnlyRenderbuffers = false;
}  // namespace

void FboPool::destroyFbo(GlFbo& fbo) {
    // glDelete* silently ignores a 0 name, so an entry without a stencil attachment needs no
    // branch.
    glDeleteFramebuffers(1, &fbo.fbo);
    glDeleteRenderbuffers(1, &fbo.depthStencilRbo);
    glDeleteTextures(1, &fbo.colorTex);
    fbo = GlFbo{};
}

void FboPool::destroyEntry(Entry& e) {
    m_liveBytes -= e.fbo.bytes;
    destroyFbo(e.fbo);
    e.inUse = false;
}

int FboPool::createEntry(int w, int h, int samples, GLenum internalFormat, bool stencil) {
    Entry entry;
    entry.fbo.width = w;
    entry.fbo.height = h;
    entry.fbo.samples = samples;
    entry.fbo.internalFormat = internalFormat;
    entry.fbo.stencil = stencil;

    glGenFramebuffers(1, &entry.fbo.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, entry.fbo.fbo);

    if (samples > 0) {
        // A multisample texture: every pass that reads a pooled buffer wants a plain sampler2D,
        // so these entries are resolved into a single-sample entry before they are read
        // (GlDevice::textureOf draws a full-screen triangle through a sampler2DMS; T-161).
        // Fixed sample locations, so a multisample -> multisample copy-pass blit from the host's
        // target is legal.
        glGenTextures(1, &entry.fbo.colorTex);
        glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, entry.fbo.colorTex);
        glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, GL_RGBA8, w, h, GL_TRUE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE,
                               entry.fbo.colorTex, 0);
    } else {
        const bool r32f = internalFormat == GL_R32F;
        glGenTextures(1, &entry.fbo.colorTex);
        glBindTexture(GL_TEXTURE_2D, entry.fbo.colorTex);
        if (r32f) {
            glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_R32F), w, h, 0, GL_RED, GL_FLOAT, nullptr);
        } else {
            glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA8), w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         nullptr);
        }
        const GLenum filter = r32f ? GL_NEAREST : GL_LINEAR;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(filter));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(filter));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(GL_CLAMP_TO_EDGE));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(GL_CLAMP_TO_EDGE));
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, entry.fbo.colorTex, 0);
    }

    const uint64_t px = uint64_t(w) * uint64_t(h);
    const uint64_t perSample = samples > 0 ? uint64_t(samples) : 1u;
    entry.fbo.bytes = px * perSample * 4u;
    bool ok = true;
    if (stencil) {
        // Depth is never tested (gpu_frame.h): a stencil-only renderbuffer where the driver
        // completes one (a quarter of GL_DEPTH24_STENCIL8) when kStencilOnlyRenderbuffers, else
        // the packed format.
        glGenRenderbuffers(1, &entry.fbo.depthStencilRbo);
        glBindRenderbuffer(GL_RENDERBUFFER, entry.fbo.depthStencilRbo);
        const GLenum formats[2] = {GL_STENCIL_INDEX8, GL_DEPTH24_STENCIL8};
        ok = false;
        for (int attempt = (kStencilOnlyRenderbuffers && !m_stencilOnlyUnsupported) ? 0 : 1;
             attempt < 2 && !ok; ++attempt) {
            const GLenum fmt = formats[attempt];
            if (samples > 0) {
                glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, fmt, w, h);
            } else {
                glRenderbufferStorage(GL_RENDERBUFFER, fmt, w, h);
            }
            // A stencil-only renderbuffer must not stay on the depth attachment point.
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
            if (fmt == GL_STENCIL_INDEX8) {
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                                          entry.fbo.depthStencilRbo);
            } else {
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                                          entry.fbo.depthStencilRbo);
            }
            ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
            while (glGetError() != GL_NO_ERROR) {
                // an unsupported format raises GL_INVALID_ENUM/OPERATION; swallowed here
            }
            if (ok) {
                entry.fbo.bytes += px * perSample * (fmt == GL_STENCIL_INDEX8 ? 1u : 4u);
            } else if (attempt == 0) {
                m_stencilOnlyUnsupported = true;
            }
        }
    } else {
        ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }
    if (ok && samples > 0) {
        // Multisample storage is allowed to come back with *more* samples than requested, and
        // the copy-pass blit from the parent target (multisample -> multisample) requires an
        // exact match, so an entry that came back with a different count is rejected; the
        // caller then falls back to a single-sample entry.
        GLint actual = 0;
        glGetIntegerv(GL_SAMPLES, &actual);
        ok = actual == samples;
    }
    if (!ok) {
        destroyFbo(entry.fbo);
        return -1;
    }

    entry.inUse = true;
    entry.lastUsed = m_frame;
    m_liveBytes += entry.fbo.bytes;
    // Reuse a slot emptied by purgeUnused()/endFrame() before growing the pool.
    for (size_t i = 0; i < m_entries.size(); ++i) {
        if (!m_entries[i].inUse && m_entries[i].fbo.fbo == 0) {
            m_entries[i] = entry;
            return static_cast<int>(i);
        }
    }
    m_entries.push_back(entry);
    return static_cast<int>(m_entries.size()) - 1;
}

void FboPool::makeRoom(uint64_t wanted) {
    while (m_liveBytes + wanted > m_budget) {
        Entry* victim = nullptr;
        for (Entry& e : m_entries) {
            if (e.inUse || e.fbo.fbo == 0) continue;
            if (victim == nullptr || e.lastUsed < victim->lastUsed) victim = &e;
        }
        if (victim == nullptr) return;
        destroyEntry(*victim);
    }
}

int FboPool::acquire(int w, int h, int samples, GLenum internalFormat, bool stencil, bool* created) {
    if (created != nullptr) *created = false;
    for (size_t i = 0; i < m_entries.size(); ++i) {
        Entry& e = m_entries[i];
        if (!e.inUse && e.fbo.fbo != 0 && e.fbo.width == w && e.fbo.height == h && e.fbo.samples == samples &&
            e.fbo.internalFormat == internalFormat && e.fbo.stencil == stencil) {
            e.inUse = true;
            e.lastUsed = m_frame;
            return static_cast<int>(i);
        }
    }
    // Upper bound of the new entry's bytes (a stencil-only attachment ends up smaller).
    const uint64_t perSample = samples > 0 ? uint64_t(samples) : 1u;
    makeRoom(uint64_t(w) * uint64_t(h) * perSample * (stencil ? 8u : 4u));
    // createEntry leaves the new FBO bound (or, on failure, whatever was bound before its own
    // glBindFramebuffer calls -- restoring the caller's binding is the caller's job either way).
    if (created != nullptr) *created = true;
    return createEntry(w, h, samples, internalFormat, stencil);
}

void FboPool::release(int handle) {
    if (handle >= 0 && static_cast<size_t>(handle) < m_entries.size()) {
        m_entries[static_cast<size_t>(handle)].inUse = false;
    }
}

uint32_t FboPool::purgeUnused(int keepWidth, int keepHeight) {
    uint32_t deleted = 0;
    for (Entry& e : m_entries) {
        if (e.inUse || e.fbo.fbo == 0) {
            continue;
        }
        if (e.fbo.width == keepWidth && e.fbo.height == keepHeight) {
            continue;
        }
        destroyEntry(e);
        ++deleted;
    }
    return deleted;
}

uint32_t FboPool::endFrame(uint32_t idleFrames) {
    Entry* oldest = nullptr;
    for (Entry& e : m_entries) {
        if (e.inUse || e.fbo.fbo == 0) continue;
        if (oldest == nullptr || e.lastUsed < oldest->lastUsed) oldest = &e;
    }
    uint32_t deleted = 0;
    if (oldest != nullptr && m_frame - oldest->lastUsed > idleFrames) {
        destroyEntry(*oldest);
        deleted = 1;
    }
    ++m_frame;
    return deleted;
}

uint32_t FboPool::liveCount() const {
    uint32_t n = 0;
    for (const Entry& e : m_entries) {
        if (e.fbo.fbo != 0) ++n;
    }
    return n;
}

void FboPool::shutdown() {
    for (Entry& e : m_entries) {
        destroyFbo(e.fbo);
    }
    m_entries.clear();
    m_liveBytes = 0;
}

}  // namespace xt::gl
