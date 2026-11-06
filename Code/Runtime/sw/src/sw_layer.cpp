#include "sw_layer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "sw_blend.h"
#include "sw_simd.h"
#include "xt/assert.h"

/// See Docs/tasks/T-12.md "Layer 與 Mask（sw_layer）".

namespace xt::sw {

void RectRegion::build(const std::vector<Box>& boxes) {
    clear();
    m_rows.clear();
    for (const Box& b : boxes) {
        if (!b.empty()) {
            m_rows.push_back(b.y0);
            m_rows.push_back(b.y1);
        }
    }
    std::sort(m_rows.begin(), m_rows.end());
    m_rows.erase(std::unique(m_rows.begin(), m_rows.end()), m_rows.end());
    for (size_t r = 0; r + 1 < m_rows.size(); ++r) {
        const int y0 = m_rows[r];
        const int y1 = m_rows[r + 1];
        m_rowIntervals.clear();
        for (const Box& b : boxes) {
            if (!b.empty() && b.y0 <= y0 && y0 < b.y1) {
                m_rowIntervals.push_back(Interval{b.x0, b.x1});
            }
        }
        if (m_rowIntervals.empty()) {
            continue;
        }
        std::sort(m_rowIntervals.begin(), m_rowIntervals.end(),
                  [](const Interval& a, const Interval& b) { return a.x0 < b.x0; });
        const auto first = static_cast<uint32_t>(m_intervals.size());
        for (const Interval& iv : m_rowIntervals) {
            if (m_intervals.size() > first && iv.x0 <= m_intervals.back().x1) {
                m_intervals.back().x1 = std::max(m_intervals.back().x1, iv.x1);  // Overlapping or touching.
            } else {
                m_intervals.push_back(iv);
            }
        }
        const auto count = static_cast<uint32_t>(m_intervals.size()) - first;
        // A band whose intervals equal the adjacent band above is merged into it.
        if (!m_bands.empty()) {
            Band& prev = m_bands.back();
            bool same = prev.y1 == y0 && prev.count == count;
            for (uint32_t i = 0; same && i < count; ++i) {
                same = m_intervals[prev.first + i].x0 == m_intervals[first + i].x0 &&
                       m_intervals[prev.first + i].x1 == m_intervals[first + i].x1;
            }
            if (same) {
                prev.y1 = y1;
                m_intervals.resize(first);
                continue;
            }
        }
        m_bands.push_back(Band{y0, y1, first, count});
    }
}

const RectRegion::Band* RectRegion::bandAt(int y, size_t& hint) const {
    if (m_bands.empty()) {
        return nullptr;
    }
    if (hint >= m_bands.size() || m_bands[hint].y0 > y) {
        hint = 0;
    }
    while (hint + 1 < m_bands.size() && m_bands[hint].y1 <= y) {
        ++hint;
    }
    const Band& b = m_bands[hint];
    return (y >= b.y0 && y < b.y1) ? &b : nullptr;
}

Box RectRegion::boundsWithin(const Box& box) const {
    Box out{box.x1, box.y1, box.x0, box.y0};
    for (const Band& b : m_bands) {
        const int y0 = std::max(b.y0, box.y0);
        const int y1 = std::min(b.y1, box.y1);
        if (y0 >= y1) {
            continue;
        }
        const Interval* iv = intervals(b);
        for (uint32_t i = 0; i < b.count; ++i) {
            const int x0 = std::max(iv[i].x0, box.x0);
            const int x1 = std::min(iv[i].x1, box.x1);
            if (x0 >= x1) {
                continue;
            }
            out.x0 = std::min(out.x0, x0);
            out.x1 = std::max(out.x1, x1);
            out.y0 = std::min(out.y0, y0);
            out.y1 = std::max(out.y1, y1);
        }
    }
    return out.empty() ? Box{} : out;
}

void RectRegion::release() {
    std::vector<Band>().swap(m_bands);
    std::vector<Interval>().swap(m_intervals);
    std::vector<int>().swap(m_rows);
    std::vector<Interval>().swap(m_rowIntervals);
}

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
        if (rowDefined != nullptr && rowDefined[ly] == 0) {
            return Color{0, 0, 0, 0};  // T-643: a row nothing has drawn into yet is transparent.
        }
        const float* p = f + (static_cast<size_t>(ly) * static_cast<size_t>(w) + static_cast<size_t>(lx)) * 4;
        return Color{p[0], p[1], p[2], p[3]};
    }
    const uint8_t* p =
        u8 + static_cast<size_t>(ly) * static_cast<size_t>(strideBytes) + static_cast<size_t>(lx) * 4;
    return loadPixelU8(p, u8Premultiplied);
}

void Target::set(int x, int y, const Color& c) {
    int lx = x - originX;
    int ly = y - originY;
    if (lx < 0 || ly < 0 || lx >= w || ly >= h) {
        return;  // Outside a bounded effect buffer: silently dropped, see Target::get's comment.
    }
    if (f != nullptr) {
        if (rowDefined != nullptr && rowDefined[ly] == 0) {
            defineRow(ly);
        }
        float* p = f + (static_cast<size_t>(ly) * static_cast<size_t>(w) + static_cast<size_t>(lx)) * 4;
        p[0] = c.r;
        p[1] = c.g;
        p[2] = c.b;
        p[3] = c.a;
        return;
    }
    uint8_t* p =
        u8 + static_cast<size_t>(ly) * static_cast<size_t>(strideBytes) + static_cast<size_t>(lx) * 4;
    storePixelU8(p, c, u8Premultiplied);
}

void Target::defineRow(int ly) const {
    std::memset(f + static_cast<size_t>(ly) * static_cast<size_t>(w) * 4, 0,
                static_cast<size_t>(w) * 4 * sizeof(float));
    rowDefined[ly] = 1;
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
    storePixelU8(rowU8 + lx * 4, c, u8Premul);
}

/// Companion to storeRow: reads the backdrop sample (premultiplied), matching `Target::get`.
inline Color loadRow(const float* rowF, const uint8_t* rowU8, bool u8Premul, size_t lx) {
    if (rowF != nullptr) {
        const float* p = rowF + lx * 4;
        return Color{p[0], p[1], p[2], p[3]};
    }
    return loadPixelU8(rowU8 + lx * 4, u8Premul);
}

/// Half-open source-x range whose pixels land inside `below` once shifted by `originX`. Lets the
/// compositing loops drop the per-pixel horizontal bounds check.
inline void visibleXRange(int srcW, int originX, const Target& below, int& x0, int& x1) {
    const int dx = originX - below.originX;
    x0 = std::max(0, -dx);
    x1 = std::min(srcW, below.w - dx);
}

/// Calls `run(x, n)` for every maximal run of source-x in `[x0, x1)` whose destination pixels
/// (`x + dx` on row `y`) lie inside `region` (Docs/tasks/T-99.md, T-642); the whole range is one
/// run when no region applies. Handing the SIMD span kernels runs instead of single pixels is what
/// keeps partial redraw and vectorization compatible (Docs/tasks/T-102.md step 4a).
template <class Fn>
inline void forEachWritableRun(const RectRegion* region, int dx, int y, int x0, int x1, size_t& hint,
                               Fn&& run) {
    if (x1 <= x0) {
        return;
    }
    if (region == nullptr) {
        run(x0, x1 - x0);
        return;
    }
    const RectRegion::Band* band = region->bandAt(y, hint);
    if (band == nullptr) {
        return;
    }
    const RectRegion::Interval* iv = region->intervals(*band);
    for (uint32_t i = 0; i < band->count; ++i) {
        const int a = std::max(x0, iv[i].x0 - dx);
        const int b = std::min(x1, iv[i].x1 - dx);
        if (a < b) {
            run(a, b - a);
        }
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

/// T-643: what `compositeRun` does to an RGBA8 row when every source pixel is transparent -- the
/// case of a layer row nothing drew into, which popLayer no longer clears and composites. Source-
/// over of zero leaves every pixel's bytes as they are (the premultiply / un-premultiply round trip
/// lands back on the same byte) except where alpha is 0: un-premultiplying zeroes the colour. A
/// premultiplied target keeps every byte.
inline void compositeTransparentRun(uint8_t* dstU8, bool u8Premul, size_t lx, int n) {
    if (u8Premul) {
        return;
    }
    uint8_t* p = dstU8 + lx * 4;
    for (int i = 0; i < n; ++i, p += 4) {
        if (p[3] == 0) {
            p[0] = 0;
            p[1] = 0;
            p[2] = 0;
        }
    }
}

}  // namespace

void LayerStack::resetStack() {
    m_stack.clear();
    // Every surface the dropped frames held goes back to the pool (Docs/tasks/T-263a.md E7).
    std::fill(m_inUse.begin(), m_inUse.end(), uint8_t{0});
    m_rootRegion = nullptr;
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
        m_age.emplace_back();
        m_rowDefined.emplace_back();
    }
    m_inUse[best] = 1;
    SlotAge& age = m_age[best];
    age.acquired = true;
    age.windowPeak = std::max(age.windowPeak, need);
    Surface& s = m_pool[best];
    s.w = w;
    s.h = h;
    if (s.rgba.capacity() < need) {
        // T-641: exactly the requested size; `resize` alone would grow geometrically.
        std::vector<float>().swap(s.rgba);
        s.rgba.reserve(need);
    }
    // `resize` keeps the vector's capacity when shrinking, so a smaller layer reuses the same
    // storage. Nothing is cleared here (T-643): a row is zeroed when a draw first reaches it
    // (Target::rowDefined), and one never drawn into is never touched.
    s.rgba.resize(need);
    m_rowDefined[best].assign(static_cast<size_t>(h), uint8_t{0});
    return best;
}

void LayerStack::defineAllRows(size_t index) {
    const Target t = surfaceTarget(index);
    for (int y = 0; y < t.h; ++y) {
        if (t.rowDefined[y] == 0) {
            t.defineRow(y);
        }
    }
}

size_t LayerStack::definedRows(size_t index) const {
    return static_cast<size_t>(
        std::count(m_rowDefined[index].begin(), m_rowDefined[index].end(), uint8_t{1}));
}

void LayerStack::release(size_t index) { m_inUse[index] = 0; }

void LayerStack::endFrame() {
    if (!m_stack.empty() || std::find(m_inUse.begin(), m_inUse.end(), uint8_t{1}) != m_inUse.end()) {
        return;
    }
    const bool windowEnds = ++m_windowFrames >= kPoolIdleFrames;
    if (windowEnds) {
        m_windowFrames = 0;
    }
    size_t kept = 0;
    for (size_t i = 0; i < m_pool.size(); ++i) {
        SlotAge& age = m_age[i];
        age.idleFrames = age.acquired ? 0 : age.idleFrames + 1;
        age.acquired = false;
        if (age.idleFrames >= kPoolIdleFrames) {
            continue;  // Freed: not moved into the kept range below.
        }
        if (windowEnds) {
            Surface& s = m_pool[i];
            if (age.windowPeak > 0 && s.rgba.capacity() / 2 > age.windowPeak) {
                std::vector<float>().swap(s.rgba);
                s.rgba.reserve(age.windowPeak);
                s.w = 0;
                s.h = 0;
            }
            age.windowPeak = 0;
        }
        if (kept != i) {
            m_pool[kept] = std::move(m_pool[i]);
            m_age[kept] = m_age[i];
            m_rowDefined[kept].swap(m_rowDefined[i]);
        }
        ++kept;
    }
    m_pool.resize(kept);
    m_inUse.resize(kept);
    m_age.resize(kept);
    m_rowDefined.resize(kept);
}

void LayerStack::releasePool() {
    resetStack();
    std::vector<Surface>().swap(m_pool);
    std::vector<uint8_t>().swap(m_inUse);
    std::vector<SlotAge>().swap(m_age);
    std::vector<std::vector<uint8_t>>().swap(m_rowDefined);
    m_windowFrames = 0;
}

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
    // Normal at full opacity -- the overwhelmingly common case -- goes through the source-over
    // span kernel a run at a time; any other opacity or mode through the layer blend kernel
    // (T-643), byte-identical to the per-pixel blendPixelPremul loop it replaced.
    const bool fast = top.blend == BlendMode::Normal && top.opacity == 1.0f;
    const SwSimdOps& ops = swSimd();
    const uint8_t* defined = m_rowDefined[top.surfaceA].data();
    size_t hint = 0;
    for (int y = 0; y < layer.h; ++y) {
        const int sy = y + top.originY;
        if (defined[y] == 0) {
            // T-643: a row nothing drew into is transparent. Only a full-opacity Normal composite
            // onto the root changes anything for it (compositeTransparentRun); every other path
            // skips transparent pixels, and source-over of zero leaves a float row as it is.
            uint8_t* dstU8 = fast && below.f == nullptr ? below.rowU8(sy) : nullptr;
            if (dstU8 != nullptr) {
                forEachWritableRun(m_rootRegion, dx, sy, x0, x1, hint, [&](int rx, int n) {
                    compositeTransparentRun(dstU8, below.u8Premultiplied, static_cast<size_t>(rx + dx), n);
                });
            }
            continue;
        }
        float* dstF = below.rowF(sy);
        uint8_t* dstU8 = dstF != nullptr ? nullptr : below.rowU8(sy);
        if (dstF == nullptr && dstU8 == nullptr) {
            continue;
        }
        const float* srcRow = layer.rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(layer.w) * 4;
        const RectRegion* region = dstU8 != nullptr ? m_rootRegion : nullptr;
        forEachWritableRun(region, dx, sy, x0, x1, hint, [&](int rx, int n) {
            const size_t lx = static_cast<size_t>(rx + dx);
            const float* src = srcRow + static_cast<size_t>(rx) * 4;
            if (fast) {
                compositeRun(dstF, dstU8, below.u8Premultiplied, lx, src, n);
            } else if (dstF != nullptr) {
                ops.spanLayerBlendF(dstF + lx * 4, src, n, top.opacity, top.blend);
            } else {
                ops.spanLayerBlendU8(dstU8 + lx * 4, src, n, top.opacity, top.blend, below.u8Premultiplied);
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
    size_t hint = 0;
    for (int y = 0; y < layer.h; ++y) {
        const int sy = y + originY;
        float* dstF = below.rowF(sy);
        uint8_t* dstU8 = dstF != nullptr ? nullptr : below.rowU8(sy);
        if (dstF == nullptr && dstU8 == nullptr) {
            continue;
        }
        const float* srcRow = layer.rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(layer.w) * 4;
        const RectRegion* region = dstU8 != nullptr ? m_rootRegion : nullptr;
        forEachWritableRun(region, dx, sy, x0, x1, hint, [&](int rx, int n) {
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
    // The caller reads the whole surface (T-643): rows never drawn into are zeroed now.
    defineAllRows(m_stack.back().surfaceA);
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
    size_t hint = 0;
    for (int y = 0; y < filtered.h; ++y) {
        const int sy = y + top.originY;
        float* dstF = below.rowF(sy);
        uint8_t* dstU8 = dstF != nullptr ? nullptr : below.rowU8(sy);
        if (dstF == nullptr && dstU8 == nullptr) {
            continue;
        }
        const float* srcRow =
            filtered.rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(filtered.w) * 4;
        const RectRegion* region = dstU8 != nullptr ? m_rootRegion : nullptr;
        forEachWritableRun(region, dx, sy, x0, x1, hint, [&](int rx, int n) {
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
    size_t hint = 0;
    const Target maskTarget = surfaceTarget(top.surfaceA);
    const uint8_t* contentDefined = m_rowDefined[top.surfaceB].data();
    for (int y = 0; y < mask.h; ++y) {
        // T-643: a content row nothing drew into is transparent, so every pixel of it is skipped
        // below whatever the mask holds; a mask row nothing drew into reads as transparent.
        if (contentDefined[y] == 0) {
            continue;
        }
        if (maskTarget.rowDefined[y] == 0) {
            maskTarget.defineRow(y);
        }
        float* dstF = below.rowF(y);
        uint8_t* dstU8 = dstF != nullptr ? nullptr : below.rowU8(y);
        if (dstF == nullptr && dstU8 == nullptr) {
            continue;
        }
        const size_t rowOff = static_cast<size_t>(y) * static_cast<size_t>(mask.w) * 4;
        const float* maskRow = mask.rgba.data() + rowOff;
        const float* contentRow = content.rgba.data() + rowOff;
        const int dx = -below.originX;
        // Partial redraw: only the pixels inside the damage region (Docs/tasks/T-99.md, T-642).
        const RectRegion* region = dstU8 != nullptr ? m_rootRegion : nullptr;
        forEachWritableRun(region, dx, y, x0, x1, hint, [&](int rx, int n) {
            for (int x = rx; x < rx + n; ++x) {
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
                Color dst = loadRow(dstF, dstU8, below.u8Premultiplied, lx);
                storeRow(dstF, dstU8, below.u8Premultiplied, lx, blendSrcOverPremul(c, dst));
            }
        });
    }
    release(top.surfaceA);
    release(top.surfaceB);
}

}  // namespace xt::sw
