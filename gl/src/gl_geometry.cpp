#include "gl_geometry.h"

#include <algorithm>
#include <cstring>

namespace xt::gl {

namespace {
constexpr GLsizei kStride = static_cast<GLsizei>(sizeof(gpu::Vertex));
/// Two changed runs closer than this many bytes are uploaded as one call.
constexpr size_t kMergeGapBytes = 1024;
/// Capacity granularity: buffers grow in this many bytes, so a frame that fluctuates by a few
/// vertices does not reallocate.
constexpr size_t kCapacityStep = 64 * 1024;
}  // namespace

bool GlGeometry::init() {
    for (Slot& s : m_slots) {
        glGenVertexArrays(1, &s.vao);
        glGenBuffers(1, &s.vbo);
        glGenBuffers(1, &s.ebo);
        glBindVertexArray(s.vao);
        glBindBuffer(GL_ARRAY_BUFFER, s.vbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.ebo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, kStride,
                              reinterpret_cast<const void*>(offsetof(gpu::Vertex, x)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, kStride,
                              reinterpret_cast<const void*>(offsetof(gpu::Vertex, u)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, kStride,
                              reinterpret_cast<const void*>(offsetof(gpu::Vertex, rgba)));
        glBindVertexArray(0);
        s.vboCapacity = s.eboCapacity = 0;
        s.vertexShadow.clear();
        s.indexShadow.clear();
    }
    m_current = 0;
    return true;
}

void GlGeometry::shutdown() {
    for (Slot& s : m_slots) {
        if (s.vbo != 0) glDeleteBuffers(1, &s.vbo);
        if (s.ebo != 0) glDeleteBuffers(1, &s.ebo);
        if (s.vao != 0) glDeleteVertexArrays(1, &s.vao);
        s = Slot{};
    }
}

void GlGeometry::uploadBuffer(GLenum target, const uint8_t* data, size_t bytes, size_t elementSize,
                              size_t& capacity, std::vector<uint8_t>& shadow, GlDeviceStats& stats) {
    if (bytes == 0) {
        return;
    }
    if (bytes > capacity) {
        capacity = (bytes + kCapacityStep - 1) / kCapacityStep * kCapacityStep;
        glBufferData(target, static_cast<GLsizeiptr>(capacity), nullptr, GL_DYNAMIC_DRAW);
        glBufferSubData(target, 0, static_cast<GLsizeiptr>(bytes), data);
        shadow.assign(data, data + bytes);
        ++stats.uploadRanges;
        stats.uploadBytes += bytes;
        return;
    }
    // Element-wise diff against the shadow; runs closer than kMergeGapBytes are merged.
    const size_t common = std::min(bytes, shadow.size());
    const size_t commonElems = common / elementSize;
    size_t runStart = 0, runEnd = 0;  // bytes; runEnd == 0 means "no open run"
    bool open = false;
    auto flush = [&]() {
        if (!open) return;
        glBufferSubData(target, static_cast<GLintptr>(runStart), static_cast<GLsizeiptr>(runEnd - runStart),
                        data + runStart);
        ++stats.uploadRanges;
        stats.uploadBytes += runEnd - runStart;
        open = false;
    };
    for (size_t e = 0; e < commonElems; ++e) {
        const size_t off = e * elementSize;
        if (std::memcmp(data + off, shadow.data() + off, elementSize) == 0) {
            continue;
        }
        if (open && off - runEnd > kMergeGapBytes) {
            flush();
        }
        if (!open) {
            runStart = off;
            open = true;
        }
        runEnd = off + elementSize;
    }
    // Whatever lies past the shadow (or past the last whole common element) is new.
    const size_t tailStart = commonElems * elementSize;
    if (bytes > tailStart) {
        if (open && tailStart - runEnd > kMergeGapBytes) {
            flush();
        }
        if (!open) {
            runStart = tailStart;
            open = true;
        }
        runEnd = bytes;
    }
    flush();
    shadow.assign(data, data + bytes);
}

void GlGeometry::uploadFrame(std::span<const gpu::Vertex> verts, std::span<const uint32_t> indices,
                             GlStateCache& cache) {
    m_current = (m_current + 1) % kSlots;
    Slot& s = m_slots[m_current];
    cache.bindVertexArray(s.vao);
    // The VAO already holds the element buffer binding; the array buffer is bound explicitly.
    glBindBuffer(GL_ARRAY_BUFFER, s.vbo);
    uploadBuffer(GL_ARRAY_BUFFER, reinterpret_cast<const uint8_t*>(verts.data()), verts.size_bytes(),
                 sizeof(gpu::Vertex), s.vboCapacity, s.vertexShadow, cache.stats);
    uploadBuffer(GL_ELEMENT_ARRAY_BUFFER, reinterpret_cast<const uint8_t*>(indices.data()),
                 indices.size_bytes(), sizeof(uint32_t), s.eboCapacity, s.indexShadow, cache.stats);
}

void GlGeometry::bindFrame(GlStateCache& cache) { cache.bindVertexArray(m_slots[m_current].vao); }

void GlGeometry::drawRange(uint32_t first, uint32_t count, uint32_t baseVertex, GlDeviceStats& stats) {
    if (count == 0) {
        return;
    }
    glDrawElementsBaseVertex(GL_TRIANGLES, static_cast<GLsizei>(count), GL_UNSIGNED_INT,
                             reinterpret_cast<const void*>(static_cast<size_t>(first) * sizeof(uint32_t)),
                             static_cast<GLint>(baseVertex));
    ++stats.drawCalls;
}

}  // namespace xt::gl
