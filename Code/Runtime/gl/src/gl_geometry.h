#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "gl_loader.h"
#include "gl_state_cache.h"
#include "xt/gpu/gpu_frame.h"

/// Vertex/index buffers for xt_gl as a GpuDevice (T-147): one VAO over the frame's
/// `xt::gpu::Vertex` layout (location 0 = vec3 `x, y, w`, location 1 = vec2 `u, v`, location 2 =
/// normalized ubyte4 `rgba`) plus its element buffer. Composite/Effect commands draw their own
/// window quad from the same buffers (T-151).
///
/// T-159: the buffers persist across frames and are uploaded incrementally. `kSlots` buffer sets
/// rotate per frame so a `glBufferSubData` never touches a buffer the GPU may still be reading
/// (the driver would otherwise stall or copy); each set keeps a CPU shadow of what it holds, and
/// `uploadFrame` sends only the runs of vertices/indices that differ from that shadow (an
/// animated frame changes a few percent of the buffer; a resize past the capacity re-uploads
/// everything).

namespace xt::gl {

class GlGeometry {
public:
    bool init();
    void shutdown();

    /// Rotates to the next buffer set, uploads what differs from that set's shadow and binds the
    /// set's VAO through `cache`. Upload calls and bytes are counted in `cache.stats`.
    void uploadFrame(std::span<const gpu::Vertex> verts, std::span<const uint32_t> indices,
                     GlStateCache& cache);
    /// Binds the current set's VAO (after `uploadFrame`).
    void bindFrame(GlStateCache& cache);
    /// Draws `count` indices starting at index `first` with `baseVertex` added (GL_TRIANGLES).
    void drawRange(uint32_t first, uint32_t count, uint32_t baseVertex, GlDeviceStats& stats);

private:
    static constexpr int kSlots = 3;
    struct Slot {
        GLuint vao = 0;
        GLuint vbo = 0;
        GLuint ebo = 0;
        size_t vboCapacity = 0;  ///< bytes allocated
        size_t eboCapacity = 0;
        std::vector<uint8_t> vertexShadow;  ///< what the VBO holds, byte for byte
        std::vector<uint8_t> indexShadow;
    };
    Slot m_slots[kSlots];
    int m_current = 0;

    /// Uploads `bytes` of `data` into `target`'s buffer (`capacity`, `shadow`): a full
    /// `glBufferData` when the capacity is too small, else `glBufferSubData` per changed run.
    static void uploadBuffer(GLenum target, const uint8_t* data, size_t bytes, size_t elementSize,
                             size_t& capacity, std::vector<uint8_t>& shadow, GlDeviceStats& stats);
};

}  // namespace xt::gl
