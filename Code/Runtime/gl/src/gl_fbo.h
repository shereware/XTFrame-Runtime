#pragma once

#include <cstdint>
#include <vector>

#include "gl_loader.h"

/// FBO pool for PushLayer/PushMask/PushEffect offscreen targets: an RGBA8 color texture
/// (single-sample entries, the ones the composite and effect passes sample) or an RGBA8
/// multisample color texture (T-80: the entries drawn into, so offscreen content gets the same
/// MSAA edges as content drawn straight into the render target; T-161: a
/// GL_TEXTURE_2D_MULTISAMPLE rather than a renderbuffer, so the resolve into a single-sample
/// entry is a `sampler2DMS` draw instead of a glBlitFramebuffer), plus a stencil renderbuffer
/// only when the entry is acquired with `stencil` (T-163: effect intermediates and resolve
/// targets carry none; GL_DEPTH24_STENCIL8, or GL_STENCIL_INDEX8 when gl_fbo.cpp's
/// `kStencilOnlyRenderbuffers` is on). Entries are sized on demand and recycled by (size,
/// sample count, format, stencil).
///
/// Retention (T-163): entries are kept while the pool's bytes stay within `setBudget`'s budget;
/// when a creation would exceed it, the least recently used released entries are deleted first.
/// `endFrame`'s idle eviction is a slow safety net that deletes at most one entry per frame:
/// deleting GL objects is what the driver's worker thread pays for in the next frame's first
/// `glGet*` (Docs/tasks/T-163.md), so the pool never deletes a burst of entries on a schedule
/// (T-161's 120-frame window did, and every such frame was a 1-5 ms spike). See
/// Docs/design/10-backends.md §10.5.

namespace xt::gl {

struct GlFbo {
    GLuint fbo = 0;
    /// GL_TEXTURE_2D for a single-sample entry, GL_TEXTURE_2D_MULTISAMPLE when `samples` > 0.
    GLuint colorTex = 0;
    /// The stencil (or depth/stencil) renderbuffer, 0 for an entry acquired without stencil.
    GLuint depthStencilRbo = 0;
    int width = 0;
    int height = 0;
    /// 0 for a plain single-sample entry (sampleable through `colorTex` with a sampler2D), else
    /// the sample count (`colorTex` is then a sampler2DMS source -- resolve it into a
    /// single-sample entry before any pass samples it).
    int samples = 0;
    /// Color format: GL_RGBA8, or GL_R32F (T-147: the Outline effect's signed distance field,
    /// single-sample only, GL_NEAREST).
    GLenum internalFormat = GL_RGBA8;
    /// True when the entry has a stencil attachment.
    bool stencil = false;
    /// Bytes the entry's storage takes (color at 4 bytes per sample, stencil at 1 or 4).
    uint64_t bytes = 0;
};

class FboPool {
public:
    void shutdown();

    /// Bytes the pool may hold in released + in-use entries before `acquire` starts deleting
    /// released ones (LRU first) to make room. Entries in use are never deleted, so a frame
    /// that needs more than the budget simply exceeds it for that frame.
    void setBudget(uint64_t bytes) { m_budget = bytes; }

    /// Returns a handle (index into the pool) to a complete FBO of exactly `w`x`h` with exactly
    /// `samples` samples (0 = single-sample, with a sampleable color texture) and a stencil
    /// attachment when `stencil`. Reuses a released entry of matching size, sample count, format
    /// and stencil if one exists, touching no GL state; otherwise it creates one (T-159:
    /// `*created` is set to true, and the framebuffer, texture and renderbuffer bindings are
    /// left pointing at the new objects, so a caller shadowing GL state must invalidate those).
    /// Returns -1 on failure (framebuffer incomplete, or the driver gave the multisample storage
    /// a sample count other than the one asked for). `internalFormat` is GL_RGBA8 or GL_R32F
    /// (single-sample only).
    int acquire(int w, int h, int samples = 0, GLenum internalFormat = GL_RGBA8, bool stencil = true,
                bool* created = nullptr);

    /// Marks `handle` free for reuse; does not delete the underlying GL objects.
    void release(int handle);

    /// Ends the current frame: deletes the least recently used released entry if it has gone
    /// unused for more than `idleFrames` frames (at most one entry per call, see above), then
    /// advances the frame counter `acquire` stamps entries with. Entries in use are never
    /// touched. Returns the number of entries deleted (0 or 1).
    uint32_t endFrame(uint32_t idleFrames);

    /// Deletes every released entry whose size differs from `keepWidth`x`keepHeight`, so a resize
    /// does not keep the old size's buffers alive (a 1280x720 RGBA8 4x entry is ~14 MB of color
    /// plus its stencil). Entries still in use are left alone. Handles stay valid: a purged slot
    /// is emptied, never removed. Returns the number of entries deleted.
    uint32_t purgeUnused(int keepWidth, int keepHeight);

    const GlFbo& get(int handle) const { return m_entries[static_cast<size_t>(handle)].fbo; }

    /// Live entries (in use or released but not deleted) and their `bytes` summed (T-163
    /// memory accounting).
    uint32_t liveCount() const;
    uint64_t liveBytes() const { return m_liveBytes; }
    /// True when a stencil-only (GL_STENCIL_INDEX8) attachment was tried and did not complete,
    /// so every later entry uses GL_DEPTH24_STENCIL8 (diagnostics).
    bool stencilOnlyUnsupported() const { return m_stencilOnlyUnsupported; }

private:
    struct Entry {
        GlFbo fbo;
        bool inUse = false;
        uint32_t lastUsed = 0;  ///< frame counter value of the last acquire
    };
    std::vector<Entry> m_entries;
    uint32_t m_frame = 0;
    uint64_t m_budget = ~uint64_t(0);
    uint64_t m_liveBytes = 0;
    bool m_stencilOnlyUnsupported = false;

    int createEntry(int w, int h, int samples, GLenum internalFormat, bool stencil);
    /// Deletes released entries, least recently used first, until `m_liveBytes + wanted` fits
    /// the budget or none is left.
    void makeRoom(uint64_t wanted);
    void destroyEntry(Entry& e);
    static void destroyFbo(GlFbo& fbo);
};

}  // namespace xt::gl
