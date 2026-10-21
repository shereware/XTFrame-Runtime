#include "frame_emit.h"

#include <algorithm>

#include "xt/assert.h"

namespace xt::gpu::detail {

namespace {

/// Commands whose triangles may share one draw with a neighbour of identical state without
/// changing the result: in-order blending of color draws that only test the stencil, and the
/// idempotent winding-nibble clear.
bool batchable(const DrawCmd& c) {
    switch (c.shading) {
        case Shading::Solid:
        case Shading::Gradient:
        case Shading::Texture:
        case Shading::TextureStraight:
        case Shading::Glyph:
            return c.stencilOp == StencilOp::None || c.stencilOp == StencilOp::TestEqualRef;
        case Shading::StencilOnly:
            return c.stencilOp == StencilOp::ClearRect;
        default:
            return false;
    }
}

bool sameState(const DrawCmd& a, const DrawCmd& b) {
    return a.texture == b.texture && a.src == b.src && a.aux == b.aux && a.paramIndex == b.paramIndex &&
           a.shading == b.shading && a.blend == b.blend && a.stencilOp == b.stencilOp &&
           a.stencilRef == b.stencilRef && a.sampler == b.sampler && a.program == b.program &&
           a.clipX == b.clipX && a.clipY == b.clipY && a.clipW == b.clipW && a.clipH == b.clipH;
}

}  // namespace

Scissor intersect(const Scissor& a, const Scissor& b) {
    int x0 = std::max(a.x, b.x);
    int y0 = std::max(a.y, b.y);
    int x1 = std::min(a.x + a.w, b.x + b.w);
    int y1 = std::min(a.y + a.h, b.y + b.h);
    if (x1 <= x0 || y1 <= y0) {
        return Scissor{0, 0, 0, 0};
    }
    return Scissor{x0, y0, x1 - x0, y1 - y0};
}

void FrameEmitter::begin(GpuFrame& frame, int width, int height) {
    m_frame = &frame;
    m_frame->clear();
    m_width = width;
    m_height = height;
    m_frame->targetWidth = width;
    m_frame->targetHeight = height;
    m_currentTarget = kSurfaceTarget;
    m_passOpen = false;
    m_scissor = fullScissor();
    m_slots.clear();
    m_windowQuads.clear();
    beginPass(kSurfaceTarget, false);
}

void FrameEmitter::closePass() {
    if (!m_passOpen) {
        return;
    }
    Pass& p = m_frame->passes.back();
    p.cmdCount = static_cast<uint32_t>(m_frame->cmds.size()) - p.firstCmd;
    // T-162: a pass a cache store is attached to stays, however empty, so the store's pass index
    // keeps pointing at it.
    const bool pinned = !m_frame->cacheStores.empty() &&
                        m_frame->cacheStores.back().pass + 1 == static_cast<uint32_t>(m_frame->passes.size());
    if (p.cmdCount == 0 && p.clear == 0 && p.copy == 0 && !pinned) {
        m_frame->passes.pop_back();  // a target switch that drew nothing
    }
    m_passOpen = false;
}

void FrameEmitter::end() {
    closePass();
    for (const Slot& s : m_slots) {
        XT_ASSERT(!s.inUse);
        (void)s;
    }
}

void FrameEmitter::beginPass(TargetHandle target, bool clear, bool copy, TargetHandle copySource) {
    closePass();
    const Scissor window = windowOf(target);
    Pass p;
    p.target = target;
    p.firstCmd = static_cast<uint32_t>(m_frame->cmds.size());
    p.cmdCount = 0;
    p.clear = clear ? 1 : 0;
    p.copy = copy ? 1 : 0;
    p.copySource = copy ? copySource : 0;
    p.viewX = window.x;
    p.viewY = window.y;
    p.viewW = window.w;
    p.viewH = window.h;
    m_frame->passes.push_back(p);
    m_currentTarget = target;
    m_passOpen = true;
}

Scissor FrameEmitter::windowOf(TargetHandle target) const {
    if (target == kSurfaceTarget || target > m_slots.size()) {
        return fullScissor();
    }
    return m_slots[target - 1].window;
}

Scissor FrameEmitter::targetWindow(const Scissor& wanted) const {
    const Scissor parent = currentWindow();
    Scissor r = intersect(wanted, parent);
    if (r.empty()) {
        // Nothing can be drawn (an empty scissor or hint): the smallest window at that spot so
        // the push/pop still has a target.
        r = Scissor{std::clamp(wanted.x, 0, std::max(0, m_width)),
                    std::clamp(wanted.y, 0, std::max(0, m_height)), 0, 0};
    }
    auto pad = [](int v) {
        v = std::max(v, kMinWindow);
        return (v + kWindowGranularity - 1) / kWindowGranularity * kWindowGranularity;
    };
    // Padded up, but never larger than the surface (a full-height hint on a 750 px surface stays
    // 750, the size the surface-sized targets have always had).
    const int w = std::min(pad(r.w), std::max(1, m_width));
    const int h = std::min(pad(r.h), std::max(1, m_height));
    // Slide the window left/up when the padding would leave the surface; `r` stays covered
    // because `w >= r.w` and `r` lies inside the surface.
    const int x = std::clamp(r.x, 0, std::max(0, m_width - w));
    const int y = std::clamp(r.y, 0, std::max(0, m_height - h));
    return Scissor{x, y, w, h};
}

TargetHandle FrameEmitter::acquireTransient(TextureFormat fmt, bool needStencil, const Scissor& window,
                                            int scaleShift) {
    TransientTargetDesc desc;
    const int shift = std::clamp(scaleShift, 0, 8);
    const int div = 1 << shift;
    desc.width = static_cast<uint16_t>(std::clamp((window.w + div - 1) / div, 0, 65535));
    desc.height = static_cast<uint16_t>(std::clamp((window.h + div - 1) / div, 0, 65535));
    desc.format = fmt;
    desc.needStencil = needStencil ? 1 : 0;
    for (size_t i = 0; i < m_slots.size(); ++i) {
        Slot& s = m_slots[i];
        if (!s.inUse && s.desc.format == desc.format && s.desc.needStencil == desc.needStencil &&
            s.desc.width == desc.width && s.desc.height == desc.height) {
            s.inUse = true;
            s.window = window;
            return static_cast<TargetHandle>(i + 1);
        }
    }
    m_slots.push_back(Slot{desc, window, true});
    m_frame->transients.push_back(desc);
    return static_cast<TargetHandle>(m_slots.size());
}

void FrameEmitter::sizeOf(TargetHandle target, int& w, int& h) const {
    if (target == kSurfaceTarget || target > m_slots.size()) {
        w = m_width;
        h = m_height;
        return;
    }
    w = m_slots[target - 1].desc.width;
    h = m_slots[target - 1].desc.height;
}

void FrameEmitter::releaseTransient(TargetHandle target) {
    if (target == kSurfaceTarget || target > m_slots.size()) {
        return;
    }
    m_slots[target - 1].inUse = false;
}

Vertex* FrameEmitter::appendVertices(uint32_t count, uint32_t& base) {
    base = static_cast<uint32_t>(m_frame->vertices.size());
    m_frame->vertices.resize(m_frame->vertices.size() + count);
    return m_frame->vertices.data() + base;
}

uint32_t* FrameEmitter::appendIndices(uint32_t count, uint32_t& first) {
    first = static_cast<uint32_t>(m_frame->indices.size());
    m_frame->indices.resize(m_frame->indices.size() + count);
    return m_frame->indices.data() + first;
}

Geometry FrameEmitter::quad(const Vertex v[4]) {
    Geometry g;
    Vertex* out = appendVertices(4, g.baseVertex);
    for (int i = 0; i < 4; ++i) {
        out[i] = v[i];
    }
    g.vertexCount = 4;
    uint32_t* idx = appendIndices(6, g.firstIndex);
    idx[0] = 0;
    idx[1] = 1;
    idx[2] = 2;
    idx[3] = 0;
    idx[4] = 2;
    idx[5] = 3;
    g.indexCount = 6;
    return g;
}

Geometry FrameEmitter::windowQuad(const Scissor& window) {
    for (const WindowQuad& q : m_windowQuads) {
        if (q.window == window) {
            return q.geometry;
        }
    }
    const float x0 = static_cast<float>(window.x);
    const float y0 = static_cast<float>(window.y);
    const float x1 = static_cast<float>(window.x + window.w);
    const float y1 = static_cast<float>(window.y + window.h);
    const uint32_t white = 0xFFFFFFFFu;
    Vertex v[4] = {
        Vertex{x0, y0, 1.0f, 0.0f, 0.0f, white},
        Vertex{x1, y0, 1.0f, 1.0f, 0.0f, white},
        Vertex{x1, y1, 1.0f, 1.0f, 1.0f, white},
        Vertex{x0, y1, 1.0f, 0.0f, 1.0f, white},
    };
    Geometry g = quad(v);
    m_windowQuads.push_back(WindowQuad{window, g});
    return g;
}

DrawCmd& FrameEmitter::pushCmd(DrawCmd cmd, const Geometry& g) {
    XT_ASSERT(m_passOpen);
    cmd.firstIndex = g.firstIndex;
    cmd.indexCount = g.indexCount;
    cmd.baseVertex = g.baseVertex;
    cmd.vertexCount = g.vertexCount;
    auto clampI16 = [](int v) { return static_cast<int16_t>(std::clamp(v, 0, 32767)); };
    cmd.clipX = clampI16(m_scissor.x);
    cmd.clipY = clampI16(m_scissor.y);
    cmd.clipW = clampI16(m_scissor.w);
    cmd.clipH = clampI16(m_scissor.h);
    const Pass& pass = m_frame->passes.back();
    if (m_frame->cmds.size() > pass.firstCmd && g.indexCount > 0 && batchable(cmd)) {
        DrawCmd& prev = m_frame->cmds.back();
        if (prev.firstIndex + prev.indexCount == g.firstIndex &&
            prev.baseVertex + prev.vertexCount == g.baseVertex && sameState(prev, cmd)) {
            const uint32_t shift = g.baseVertex - prev.baseVertex;
            uint32_t* idx = m_frame->indices.data() + g.firstIndex;
            for (uint32_t i = 0; i < g.indexCount; ++i) {
                idx[i] += shift;
            }
            prev.indexCount += g.indexCount;
            prev.vertexCount += g.vertexCount;
            return prev;
        }
    }
    m_frame->cmds.push_back(cmd);
    return m_frame->cmds.back();
}

void FrameEmitter::pushEffect(EffectProgram program, uint32_t paramIndex, TargetHandle src, TargetHandle aux,
                              TextureHandle texture) {
    DrawCmd cmd;
    cmd.shading = Shading::Effect;
    cmd.program = program;
    cmd.paramIndex = paramIndex;
    cmd.src = src;
    cmd.aux = aux;
    cmd.texture = texture;
    cmd.blend = Blend::Replace;
    cmd.stencilOp = StencilOp::None;
    cmd.sampler = Sampler::LinearClamp;
    // Effect passes always write the whole target: their quad and scissor are the target's
    // window regardless of the clip state the caller is in.
    const Scissor window = currentWindow();
    Scissor saved = m_scissor;
    m_scissor = window;
    pushCmd(cmd, windowQuad(window));
    m_scissor = saved;
}

}  // namespace xt::gpu::detail
