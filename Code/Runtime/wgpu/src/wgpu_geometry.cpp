#include <algorithm>
#include <cstring>

#include "wgpu_internal.h"

/// The frame's geometry buffers (T-438, Docs/design/10-backends.md §10.10.2 first row): three
/// rotating sets, each written against its own CPU shadow so only changed element runs go through
/// wgpuQueueWriteBuffer -- gl_geometry.cpp's T-159 strategy with WebGPU writes.

namespace xt::wgpu::detail {

namespace {
/// Two changed runs closer than this many bytes are written as one (gl_geometry.cpp).
constexpr size_t kMergeGapBytes = 1024;
/// Buffers grow in this many bytes.
constexpr uint64_t kCapacityStep = 64 * 1024;
}  // namespace

void ShadowBuffer::release() {
    if (buffer != nullptr) {
        wgpuBufferDestroy(buffer);
        wgpuBufferRelease(buffer);
        buffer = nullptr;
    }
    capacity = 0;
    shadow.clear();
}

size_t ShadowBuffer::upload(WGPUDevice device, WGPUQueue queue, WGPUBufferUsage usage, const void* dataIn,
                            size_t bytes, size_t elementSize, uint32_t& ranges) {
    if (bytes == 0) {
        return 0;
    }
    const auto* data = static_cast<const uint8_t*>(dataIn);
    if (bytes > capacity || buffer == nullptr) {
        release();
        capacity = (bytes + kCapacityStep - 1) / kCapacityStep * kCapacityStep;
        WGPUBufferDescriptor d = WGPU_BUFFER_DESCRIPTOR_INIT;
        d.label = label("xt_wgpu geometry");
        d.usage = usage | WGPUBufferUsage_CopyDst;
        d.size = capacity;
        buffer = wgpuDeviceCreateBuffer(device, &d);
        ++generation;
        if (buffer == nullptr) {
            capacity = 0;
            return 0;
        }
        wgpuQueueWriteBuffer(queue, buffer, 0, data, bytes);
        shadow.assign(data, data + bytes);
        ++ranges;
        return bytes;
    }
    // Element-wise diff against the shadow; runs closer than kMergeGapBytes are merged.
    size_t written = 0;
    const size_t common = std::min(bytes, shadow.size());
    const size_t commonElems = common / elementSize;
    size_t runStart = 0, runEnd = 0;
    bool open = false;
    auto flush = [&]() {
        if (!open) return;
        wgpuQueueWriteBuffer(queue, buffer, runStart, data + runStart, runEnd - runStart);
        ++ranges;
        written += runEnd - runStart;
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
    return written;
}

void Geometry::shutdown() {
    for (Set& s : m_sets) {
        s.vertices.release();
        s.indices.release();
        s.instances.release();
    }
    m_current = 0;
    m_vertexBytes = m_indexBytes = m_instanceBytes = 0;
}

size_t Geometry::upload(WGPUDevice device, WGPUQueue queue, const gpu::GpuFrame& frame, uint32_t& ranges) {
    m_current = (m_current + 1) % kSets;
    Set& s = m_sets[m_current];
    m_vertexBytes = frame.vertices.size() * sizeof(gpu::Vertex);
    m_indexBytes = frame.indices.size() * sizeof(uint32_t);
    m_instanceBytes = frame.particleInstances.size() * sizeof(gpu::ParticleInstance);
    size_t written = 0;
    written += s.vertices.upload(device, queue, WGPUBufferUsage_Vertex, frame.vertices.data(), m_vertexBytes,
                                 sizeof(gpu::Vertex), ranges);
    written += s.indices.upload(device, queue, WGPUBufferUsage_Index, frame.indices.data(), m_indexBytes,
                                sizeof(uint32_t), ranges);
    written += s.instances.upload(device, queue, WGPUBufferUsage_Vertex, frame.particleInstances.data(),
                                  m_instanceBytes, sizeof(gpu::ParticleInstance), ranges);
    return written;
}

}  // namespace xt::wgpu::detail
