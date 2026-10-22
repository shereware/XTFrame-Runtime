#include "sw_layer.h"

#include <algorithm>
#include <cmath>

#include "sw_blend.h"
#include "sw_simd.h"
#include "xt/assert.h"

/// See Docs/tasks/T-12.md "Layer 與 Mask（sw_layer）".

namespace xt::sw {

Target Target::fromSwTarget(const SwTarget& target) {
    Target t;
    t.u8 = target.rgba;
    t.w = target.width;
    t.h = target.height;
    t.strideBytes = target.strideBytes;
    t.u8Premultiplied = target.premultiplied;
    return t;
}

Target Target::fromSurface(Surface& surface) {
    Target t;
    t.f = surface.rgba.data();
    t.w = surface.w;
    t.h = surface.h;
    return t;
}

Color Target::get(int x, int y) const {
    int lx = x - originX;
    int ly = y - originY;
    if (lx < 0 || ly < 0 || lx >= w || ly >= h) {
        // Outside a bounded effect buffer (Docs/tasks/T-53b.md's pushEffectLayer): not covered
        // explicitly by Docs/design/10-backends.md; most conservative choice per
        // Docs/design/14-coding-standards.md §14.8 (treat as fully transparent, never read out of
        // bounds). originX/originY are 0 for every other target, so this never triggers there.
        return Color{0, 0, 0, 0};
    }
    if (f != nullptr) {
        const float* p = f + (static_cast<size_t>(ly) * static_cast<size_t>(w) + static_cast<size_t>(lx)) * 4;
        return Color{p[0], p[1], p[2], p[3]};
    }
    const uint8_t* p =
        u8 + static_cast<size_t>(ly) * static_cast<size_t>(strideBytes) + static_cast<size_t>(lx) * 4;
    Color c{p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f};
    return u8Premultiplied ? c : premultiply(c);
}

void Target::set(int x, int y, const Color& c) {
    int lx = x - originX;
    int ly = y - originY;
    if (lx < 0 || ly < 0 || lx >= w || ly >= h) {
        return;  // Outside a bounded effect buffer: silently dropped, see Target::get's comment.
    }
    if (f != nullptr) {
        float* p = f + (static_cast<size_t>(ly) * static_cast<size_t>(w) + static_cast<size_t>(lx)) * 4;
        p[0] = c.r;
        p[1] = c.g;
        p[2] = c.b;
        p[3] = c.a;
        return;
    }
    uint8_t* p =
        u8 + static_cast<size_t>(ly) * static_cast<size_t>(strideBytes) + static_cast<size_t>(lx) * 4;
    const Color out = u8Premultiplied ? c : unpremultiply(c);
    p[0] = floatToByte(out.r);
    p[1] = floatToByte(out.g);
    p[2] = floatToByte(out.b);
    p[3] = floatToByte(out.a);
}

void Target::blendSrcOver(int x, int y, float r, float g, float b, float a) {
    if (a <= 0.0f) {
        return;
    }
    set(x, y, blendSrcOverPremul(Color{r * a, g * a, b * a, a}, get(x, y)));
}

namespace {

/// Writes one composited premultiplied sample into `below`'s row at local index `lx`. Exactly
/// what `Target::set` does, minus the per-pixel bounds check and row lookup the compositing loops
/// below have already hoisted out (Docs/tasks/T-82.md).
inline void storeRow(float* rowF, uint8_t* rowU8, bool u8Premul, size_t lx, const Color& c) {
    if (rowF != nullptr) {
        float* p = rowF + lx * 4;
        p[0] = c.r;
        p[1] = c.g;
        p[2] = c.b;
        p[3] = c.a;
        return;
    }
    uint8_t* p = rowU8 + lx * 4;
    const Color out = u8Premul ? c : unpremultiply(c);
    p[0] = floatToByte(out.r);
    p[1] = floatToByte(out.g);
    p[2] = floatToByte(out.b);
    p[3] = floatToByte(out.a);
}

/// Companion to storeRow: reads the backdrop sample (premultiplied), matching `Target::get`.
inline Color loadRow(const float* rowF, const uint8_t* rowU8, bool u8Premul, size_t lx) {
    if (rowF != nullptr) {
        const float* p = rowF + lx * 4;
        return Color{p[0], p[1], p[2], p[3]};
    }
    const uint8_t* p = rowU8 + lx * 4;
    Color c{static_cast<float>(p[0]) / 255.0f, static_cast<float>(p[1]) / 255.0f,
            static_cast<float>(p[2]) / 255.0f, static_cast<float>(p[3]) / 255.0f};
    return u8Premul ? c : premultiply(c);
}

/// Half-open source-x range whose pixels land inside `below` once shifted by `originX`. Lets the
/// compositing loops drop the per-pixel horizontal bounds check.
inline void visibleXRange(int srcW, int originX, const Target& below, int& x0, int& x1) {
    const int dx = originX - below.originX;
    x0 = std::max(0, -dx);
    x1 = std::min(srcW, below.w - dx);
}

/// Calls `run(x, n)` for every maximal run of source-x in `[x0, x1)` whose destination pixels the
/// root write mask lets through (Docs/tasks/T-99.md); the whole range is one run when no mask
/// applies. Handing the SIMD span kernels runs instead of single pixels is what keeps partial
/// redraw and vectorization compatible (Docs/tasks/T-102.md step 4a).
template <class Fn>
inline void forEachWritableRun(const uint8_t* mask, int maskStride, int dx, int y, int x0, int x1, Fn&& run) {
    if (mask == nullptr) {
        if (x1 > x0) {
            run(x0, x1 - x0);
        }
        return;
    }
    const uint8_t* maskRow = mask + static_cast<size_t>(y) * static_cast<size_t>(maskStride);
    int x = x0;
    while (x < x1) {
        if (maskRow[static_cast<size_t>(x + dx)] == 0) {
            ++x;
            continue;
        }
        const int start = x;
        while (x < x1 && maskRow[static_cast<size_t>(x + dx)] != 0) {
            ++x;
        }
        run(start, x - start);
    }
}

/// One Normal, opacity-1 composite of `src` (premultiplied float pixels) onto a destination row,
/// through the dispatched span kernels (Docs/tasks/T-102.md step 4a).
inline void compositeRun(float* dstF, uint8_t* dstU8, bool u8Premul, size_t lx, const float* src, int n) {
    const SwSimdOps& ops = swSimd();
    if (dstF != nullptr) {
        ops.spanSrcOverF(dstF + lx * 4, src, n);
        return;
    }
    ops.spanSrcOverU8(dstU8 + lx * 4, src, n, u8Premul);
}

}  // namespace

void LayerStack::resetStack() {
    m_stack.clear();
    // Every surface the dropped frames held goes back to the pool (Docs/tasks/T-263a.md E7).
    std::fill(m_inUse.begin(), m_inUse.end(), uint8_t{0});
    m_rootWriteMask = nullptr;
    m_rootWriteMaskStride = 0;
}

namespace {

/// Upper bound on pooled surfaces; see LayerStack::acquire. The Layer/Mask/Effect stack is far
/// shallower than this in practice (Docs/design/10-backends.md caps nesting well below it).
constexpr size_t kMaxPooledSurfaces = 16;

}  // namespace

size_t LayerStack::acquire(int w, int h) {
    const size_t need = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    size_t best = m_pool.size();
    size_t bestCap = 0;
    size_t anyFree = m_pool.size();
    for (size_t i = 0; i < m_pool.size(); ++i) {
        if (m_inUse[i] != 0) {
            continue;
        }
        if (anyFree == m_pool.size()) {
            anyFree = i;
        }
        const size_t cap = m_pool[i].rgba.capacity();
        if (cap >= need && (best == m_pool.size() || cap < bestCap)) {
            best = i;
            bestCap = cap;
        }
    }
    if (best == m_pool.size() && anyFree != m_pool.size() && m_pool.size() >= kMaxPooledSurfaces) {
        best = anyFree;  // Pool is full: grow an existing buffer rather than add another.
    }
    if (best == m_pool.size()) {
        m_pool.emplace_back();
        m_inUse.push_back(0);
    }
    m_inUse[best] = 1;
    Surface& s = m_pool[best];
    s.w = w;
    s.h = h;
    // `resize` keeps the vector's capacity when shrinking, so a smaller layer reuses the same
    // storage; only the pixels this layer owns are cleared.
    s.rgba.resize(need);
    std::fill(s.rgba.begin(), s.rgba.end(), 0.0f);
    return best;
}

void LayerStack::release(size_t index) { m_inUse[index] = 0; }

Target LayerStack::currentTarget(const Target& root) const {
    if (m_stack.empty()) {
        return root;
    }
    const Frame& top = m_stack.back();
    if (top.kind == FrameKind::Layer) {
        Target t = const_cast<LayerStack*>(this)->surfaceTarget(top.surfaceA);
        t.originX = top.originX;
        t.originY = top.originY;
        return t;
    }
    size_t idx = top.contentStarted ? top.surfaceB : top.surfaceA;
    return const_cast<LayerStack*>(this)->surfaceTarget(idx);
}

namespace {

/// Rounds a device-px rectangle outwards and clamps it to `root`, the shared rule for every
/// bounded offscreen buffer (T-53b's pushEffectLayer, T-133's pushLayer).
void boundsToBox(const Target& root, Rect boundsPx, int& x0, int& y0, int& w, int& h) {
    x0 = std::clamp(static_cast<int>(std::floor(boundsPx.x)), 0, root.w);
    y0 = std::clamp(static_cast<int>(std::floor(boundsPx.y)), 0, root.h);
    const int x1 = std::clamp(static_cast<int>(std::ceil(boundsPx.x + boundsPx.w)), 0, root.w);
    const int y1 = std::clamp(static_cast<int>(std::ceil(boundsPx.y + boundsPx.h)), 0, root.h);
    w = std::max(0, x1 - x0);
    h = std::max(0, y1 - y0);
}

}  // namespace

void LayerStack::pushLayer(const Target& root, float opacity, BlendMode blend, Rect boundsPx) {
    int x0 = 0;
    int y0 = 0;
    int w = 0;
    int h = 0;
    boundsToBox(root, boundsPx, x0, y0, w, h);
    if (w <= 0 || h <= 0) {
        // An empty hint (a node whose subtree covers nothing on screen) still needs a surface so
        // the matching popLayer stays balanced; 1x1 is the cheapest one that composites to nothing.
        x0 = std::min(x0, std::max(0, root.w - 1));
        y0 = std::min(y0, std::max(0, root.h - 1));
        w = 1;
        h = 1;
    }
    Frame f;
    f.kind = FrameKind::Layer;
    f.surfaceA = acquire(w, h);
    f.opacity = opacity;
    f.blend = blend;
    f.originX = x0;
    f.originY = y0;
    m_stack.push_back(f);
}

void LayerStack::popLayer(const Target& root) {
    XT_ASSERT(!m_stack.empty() && m_stack.back().kind == FrameKind::Layer);
    if (m_stack.empty()) {
        return;
    }
    Frame top = m_stack.back();
    m_stack.pop_back();
    Target below = currentTarget(root);
    Surface& layer = m_pool[top.surfaceA];
    // Row pointers and the horizontal clip are hoisted out of the inner loop, and Normal (by far
    // the most common mode) uses the inlined blendNormal instead of a cross-TU blendPixel call
    // (Docs/tasks/T-82.md). The arithmetic is unchanged.
    int x0 = 0;
    int x1 = 0;
    visibleXRange(layer.w, top.originX, below, x0, x1);
    const int dx = top.originX - below.originX;
    // Normal at full opacity -- the overwhelmingly common case -- goes through the SIMD span
    // kernel a run at a time; anything else stays per pixel on blendPixelPremul, where the mode
    // arithmetic dominates anyway (Docs/tasks/T-102.md step 4a).
    const bool fast = top.blend == BlendMode::Normal && top.opacity == 1.0f;
    for (int y = 0; y < layer.h; ++y) {
        const int sy = y + top.originY;
        float* dstF = below.rowF(sy);
        uint8_t* dstU8 = dstF != nullptr ? nullptr : below.rowU8(sy);
        if (dstF == nullptr && dstU8 == nullptr) {
            continue;
        }
        const float* srcRow = layer.rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(layer.w) * 4;
        const uint8_t* mask = dstU8 != nullptr ? m_rootWriteMask : nullptr;
        forEachWritableRun(mask, m_rootWriteMaskStride, dx, sy, x0, x1, [&](int rx, int n) {
            const size_t lx = static_cast<size_t>(rx + dx);
            if (fast) {
                compositeRun(dstF, dstU8, below.u8Premultiplied, lx, srcRow + static_cast<size_t>(rx) * 4, n);
                return;
            }
            for (int i = 0; i < n; ++i) {
                const float* sp = srcRow + static_cast<size_t>(rx + i) * 4;
                // Scaling every channel of a premultiplied colour by `opacity` is exactly what
                // scaling its alpha did in the non-premultiplied pipeline.
                const float o = top.opacity;
                Color src{sp[0] * o, sp[1] * o, sp[2] * o, sp[3] * o};
                if (src.a <= 0.0f) {
                    continue;
                }
                const size_t px = lx + static_cast<size_t>(i);
                Color dst = loadRow(dstF, dstU8, below.u8Premultiplied, px);
                storeRow(dstF, dstU8, below.u8Premultiplied, px, blendPixelPremul(top.blend, src, dst));
            }
        });
    }
    release(top.surfaceA);
}

void LayerStack::compositeSurface(const Target& root, const Surface& layer, int originX, int originY) {
    Target below = currentTarget(root);
    int x0 = 0;
    int x1 = 0;
    visibleXRange(layer.w, originX, below, x0, x1);
    const int dx = originX - below.originX;
    for (int y = 0; y < layer.h; ++y) {
        const int sy = y + originY;
        float* dstF = below.rowF(sy);
        uint8_t* dstU8 = dstF != nullptr ? nullptr : below.rowU8(sy);
        if (dstF == nullptr && dstU8 == nullptr) {
            continue;
        }
        const float* srcRow = layer.rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(layer.w) * 4;
        const uint8_t* mask = dstU8 != nullptr ? m_rootWriteMask : nullptr;
        forEachWritableRun(mask, m_rootWriteMaskStride, dx, sy, x0, x1, [&](int rx, int n) {
            compositeRun(dstF, dstU8, below.u8Premultiplied, static_cast<size_t>(rx + dx),
                         srcRow + static_cast<size_t>(rx) * 4, n);
        });
    }
}

void LayerStack::pushEffectLayer(const Target& root, Rect boundsPx) {
    int x0 = 0;
    int y0 = 0;
    int w = 0;
    int h = 0;
    boundsToBox(root, boundsPx, x0, y0, w, h);

    Frame f;
    f.kind = FrameKind::Layer;
    f.surfaceA = acquire(w, h);
    f.opacity = 1.0f;
    f.blend = BlendMode::Normal;
    f.originX = x0;
    f.originY = y0;
    m_stack.push_back(f);
}

Surface& LayerStack::topLayerSurface() {
    XT_ASSERT(!m_stack.empty() && m_stack.back().kind == FrameKind::Layer);
    return m_pool[m_stack.back().surfaceA];
}

Vec2 LayerStack::topLayerOrigin() const {
    XT_ASSERT(!m_stack.empty() && m_stack.back().kind == FrameKind::Layer);
    if (m_stack.empty()) {
        return Vec2{};
    }
    const Frame& top = m_stack.back();
    return Vec2{static_cast<float>(top.originX), static_cast<float>(top.originY)};
}

void LayerStack::popEffectLayer(const Target& root, const Surface& filtered) {
    XT_ASSERT(!m_stack.empty() && m_stack.back().kind == FrameKind::Layer);
    if (m_stack.empty()) {
        return;
    }
    Frame top = m_stack.back();
    m_stack.pop_back();
    Target below = currentTarget(root);
    // Same hoisting as popLayer; this loop is the SW backend's single biggest effect cost
    // (Docs/tasks/T-82.md).
    int x0 = 0;
    int x1 = 0;
    visibleXRange(filtered.w, top.originX, below, x0, x1);
    const int dx = top.originX - below.originX;
    for (int y = 0; y < filtered.h; ++y) {
        const int sy = y + top.originY;
        float* dstF = below.rowF(sy);
        uint8_t* dstU8 = dstF != nullptr ? nullptr : below.rowU8(sy);
        if (dstF == nullptr && dstU8 == nullptr) {
            continue;
        }
        const float* srcRow =
            filtered.rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(filtered.w) * 4;
        const uint8_t* mask = dstU8 != nullptr ? m_rootWriteMask : nullptr;
        forEachWritableRun(mask, m_rootWriteMaskStride, dx, sy, x0, x1, [&](int rx, int n) {
            compositeRun(dstF, dstU8, below.u8Premultiplied, static_cast<size_t>(rx + dx),
                         srcRow + static_cast<size_t>(rx) * 4, n);
        });
    }
    release(top.surfaceA);
}

void LayerStack::pushMask(const Target& root, MaskMode mode) {
    Frame f;
    f.kind = FrameKind::Mask;
    f.surfaceA = acquire(root.w, root.h);
    f.maskMode = mode;
    m_stack.push_back(f);
}

void LayerStack::endMaskContent(const Target& root) {
    XT_ASSERT(!m_stack.empty() && m_stack.back().kind == FrameKind::Mask && !m_stack.back().contentStarted);
    if (m_stack.empty()) {
        return;
    }
    Frame& top = m_stack.back();
    top.surfaceB = acquire(root.w, root.h);
    top.contentStarted = true;
}

namespace {

/// See Docs/tasks/T-12.md "Layer 與 Mask（sw_layer）": Add/Subtract/Intersect are already
/// expanded by the Painter into nested Mask commands, so here they collapse to Alpha /
/// AlphaInverted / Alpha respectively.
float maskCoefficient(MaskMode mode, const Color& m) {
    switch (mode) {
        case MaskMode::Alpha:
        case MaskMode::Add:
        case MaskMode::Intersect:
            return m.a;
        case MaskMode::AlphaInverted:
        case MaskMode::Subtract:
            return 1.0f - m.a;
        // `m` is premultiplied, so each stored channel is already `Cn * a`: the Rec. 709 weighted
        // sum of the premultiplied RGB equals `luma(nonPremultiplied) * a`.
        case MaskMode::Luma:
            return 0.2126f * m.r + 0.7152f * m.g + 0.0722f * m.b;
        case MaskMode::LumaInverted:
            return 1.0f - (0.2126f * m.r + 0.7152f * m.g + 0.0722f * m.b);
    }
    return m.a;
}

}  // namespace

void LayerStack::popMask(const Target& root) {
    XT_ASSERT(!m_stack.empty() && m_stack.back().kind == FrameKind::Mask && m_stack.back().contentStarted);
    if (m_stack.empty()) {
        return;
    }
    Frame top = m_stack.back();
    m_stack.pop_back();
    Target below = currentTarget(root);
    Surface& mask = m_pool[top.surfaceA];
    Surface& content = m_pool[top.surfaceB];
    // A mask frame's surfaces are always full-viewport and `below` is never an effect buffer, so
    // the rows line up one to one; same hoisting as popLayer (Docs/tasks/T-82.md).
    int x0 = 0;
    int x1 = 0;
    visibleXRange(mask.w, 0, below, x0, x1);
    for (int y = 0; y < mask.h; ++y) {
        float* dstF = below.rowF(y);
        uint8_t* dstU8 = dstF != nullptr ? nullptr : below.rowU8(y);
        if (dstF == nullptr && dstU8 == nullptr) {
            continue;
        }
        const size_t rowOff = static_cast<size_t>(y) * static_cast<size_t>(mask.w) * 4;
        const float* maskRow = mask.rgba.data() + rowOff;
        const float* contentRow = content.rgba.data() + rowOff;
        const int dx = -below.originX;
        for (int x = x0; x < x1; ++x) {
            const float* mp = maskRow + static_cast<size_t>(x) * 4;
            float m = maskCoefficient(top.maskMode, Color{mp[0], mp[1], mp[2], mp[3]});
            const float* cp = contentRow + static_cast<size_t>(x) * 4;
            // Premultiplied: scaling all four channels by the mask coefficient is what scaling
            // alpha alone did before T-102.
            Color c{cp[0] * m, cp[1] * m, cp[2] * m, cp[3] * m};
            if (c.a <= 0.0f) {
                continue;
            }
            const size_t lx = static_cast<size_t>(x + dx);
            if (dstU8 != nullptr && rootWriteMasked(static_cast<int>(lx), y)) {
                continue;  // Partial redraw: outside every damage rectangle (Docs/tasks/T-99.md).
            }
            Color dst = loadRow(dstF, dstU8, below.u8Premultiplied, lx);
            storeRow(dstF, dstU8, below.u8Premultiplied, lx, blendSrcOverPremul(c, dst));
        }
    }
    release(top.surfaceA);
    release(top.surfaceB);
}

}  // namespace xt::sw
