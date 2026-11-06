#pragma once

#include <cstdint>
#include <vector>

#include "sw_raster.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/node.h"
#include "xt/sw/sw_renderer.h"
#include "xt/types.h"

/// Offscreen buffer pool, Layer/Mask stack, and the Target abstraction unifying writes to either
/// the root SwTarget (uint8) or a pooled Surface (float). See Docs/design/10-backends.md §10.2.4,
/// Docs/tasks/T-12.md "Layer 與 Mask（sw_layer）".

namespace xt::sw {

/// Half-open device-px box, `[x0, x1) x [y0, y1)`, empty when either extent is non-positive
/// (T-133's clip bounds; T-642's write regions).
struct Box {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;

    bool empty() const { return x1 <= x0 || y1 <= y0; }
};

/// The two boxes' intersection, normalized to `Box{}` when it is empty.
inline Box intersectBox(const Box& a, const Box& b) {
    Box r{a.x0 > b.x0 ? a.x0 : b.x0, a.y0 > b.y0 ? a.y0 : b.y0, a.x1 < b.x1 ? a.x1 : b.x1,
          a.y1 < b.y1 ? a.y1 : b.y1};
    return r.empty() ? Box{} : r;
}

/// T-642 (Docs/design/23-host-integration.md §23.2 revision, §23.3): a union of pixel boxes (the
/// damage rectangles of a partial redraw) stored as disjoint row bands, each holding sorted,
/// disjoint, non-touching x intervals. Every pixel of the union lies in exactly one interval, so a
/// draw that walks the intervals writes each pixel once. Rebuilt every frame; the vectors keep
/// their capacity, so a warm renderer allocates nothing here.
class RectRegion {
public:
    struct Interval {
        int x0;
        int x1;
    };
    struct Band {
        int y0;
        int y1;
        uint32_t first;  ///< index of the band's first interval
        uint32_t count;
    };

    /// Replaces the region with the union of `boxes` (empty boxes are ignored).
    void build(const std::vector<Box>& boxes);
    void clear() {
        m_bands.clear();
        m_intervals.clear();
    }
    bool empty() const { return m_bands.empty(); }
    const std::vector<Band>& bands() const { return m_bands; }
    const Interval* intervals(const Band& band) const { return m_intervals.data() + band.first; }
    /// The band holding row `y`, or null. `hint` (a band index, updated) makes a top-to-bottom
    /// walk O(1) per row.
    const Band* bandAt(int y, size_t& hint) const;
    /// The bounding box of the region's part inside `box` (empty when they do not meet).
    Box boundsWithin(const Box& box) const;
    /// Frees the storage (T-641's releaseScratch).
    void release();

private:
    std::vector<Band> m_bands;
    std::vector<Interval> m_intervals;
    // build() scratch.
    std::vector<int> m_rows;
    std::vector<Interval> m_rowIntervals;
};

/// T-642: the pixels one draw may write besides what its clip mask lets through: always `box`
/// (the clip's bounds, the destination buffer, the shape's own box) and, when `rects` is set,
/// only the parts of it inside that region (the damage of a partial redraw). A draw sweeps only
/// the rows forEachRowRange() hands it and writes only the pieces forEachPiece() hands it, so the
/// work is proportional to the region, not to the shape.
struct WriteRegion {
    Box box;
    const RectRegion* rects = nullptr;

    /// Calls `fn(y0, y1)` for every row range (top to bottom) that can hold a writable pixel.
    template <class Fn>
    void forEachRowRange(Fn&& fn) const {
        if (box.empty()) {
            return;
        }
        if (rects == nullptr) {
            fn(box.y0, box.y1);
            return;
        }
        for (const RectRegion::Band& b : rects->bands()) {
            const int y0 = b.y0 > box.y0 ? b.y0 : box.y0;
            const int y1 = b.y1 < box.y1 ? b.y1 : box.y1;
            if (y0 < y1) {
                fn(y0, y1);
            }
        }
    }

    /// Calls `fn(a, b)` for every writable piece of `[x0, x1)` on row `y`, left to right. `hint`
    /// is RectRegion::bandAt's cursor.
    template <class Fn>
    void forEachPiece(int y, int x0, int x1, size_t& hint, Fn&& fn) const {
        if (y < box.y0 || y >= box.y1) {
            return;
        }
        if (x0 < box.x0) {
            x0 = box.x0;
        }
        if (x1 > box.x1) {
            x1 = box.x1;
        }
        if (x0 >= x1) {
            return;
        }
        if (rects == nullptr) {
            fn(x0, x1);
            return;
        }
        const RectRegion::Band* band = rects->bandAt(y, hint);
        if (band == nullptr) {
            return;
        }
        const RectRegion::Interval* iv = rects->intervals(*band);
        for (uint32_t i = 0; i < band->count; ++i) {
            const int a = iv[i].x0 > x0 ? iv[i].x0 : x0;
            const int b = iv[i].x1 < x1 ? iv[i].x1 : x1;
            if (a < b) {
                fn(a, b);
            }
        }
    }

    /// True when pixel (`x`, `y`) may be written.
    bool contains(int x, int y, size_t& hint) const {
        if (x < box.x0 || x >= box.x1 || y < box.y0 || y >= box.y1) {
            return false;
        }
        if (rects == nullptr) {
            return true;
        }
        const RectRegion::Band* band = rects->bandAt(y, hint);
        if (band == nullptr) {
            return false;
        }
        const RectRegion::Interval* iv = rects->intervals(*band);
        for (uint32_t i = 0; i < band->count; ++i) {
            if (x >= iv[i].x0 && x < iv[i].x1) {
                return true;
            }
        }
        return false;
    }
};

/// A **premultiplied** float RGBA offscreen buffer, tightly packed (row pitch == w * 4 floats).
/// Premultiplied since T-102 (Docs/design/23-host-integration.md §23.5): source-over is then
/// `dst = src + dst * (1 - srcA)`, with no divide and no `outA == 0` branch, and it vectorizes.
/// `sw_blend.h`'s `premultiply`/`unpremultiply` convert at the few places that need W3C's
/// non-premultiplied channels (the 16 non-Normal blend modes, ColorAdjust's colour matrix).
struct Surface {
    int w = 0;
    int h = 0;
    std::vector<float> rgba;

    /// Inline (Docs/tasks/T-82.md): the effect passes call these once per pixel, and a cross-TU
    /// call per pixel was a measurable share of the frame.
    Color get(int x, int y) const {
        const float* p =
            rgba.data() + (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
        return Color{p[0], p[1], p[2], p[3]};
    }
    /// Start of row `y` (4 floats per pixel, tightly packed), indexed by `x * 4`. The effect
    /// passes walk whole rows, so hoisting the row base out of the inner loop removes the index
    /// arithmetic per pixel and lets the span kernels (sw_simd.h) run over a whole row
    /// (Docs/tasks/T-102b.md). Unlike `Target::rowF` there is no bounds check: `y` must be in
    /// `[0, h)`.
    float* row(int y) { return rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 4; }
    /// See the non-const `row`.
    const float* row(int y) const {
        return rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 4;
    }

    /// Premultiplied, like `get`.
    void set(int x, int y, const Color& c) {
        float* p =
            rgba.data() + (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
        p[0] = c.r;
        p[1] = c.g;
        p[2] = c.b;
        p[3] = c.a;
    }
};

/// Unifies writes to either the root SwTarget (RGBA8, non-premultiplied unless
/// `SwTarget::premultiplied`) or a Surface (premultiplied float RGBA); exactly one of `u8`/`f` is
/// non-null. Every `Color` crossing this interface is premultiplied, whichever backing store is
/// in use: the RGBA8 path converts on the way in and out.
struct Target {
    uint8_t* u8 = nullptr;
    float* f = nullptr;
    int w = 0;
    int h = 0;
    int strideBytes = 0;  // only meaningful when u8 != nullptr; f is always tightly packed.
    /// Mirrors `SwTarget::premultiplied`; only meaningful when `u8 != nullptr`. True skips the
    /// premultiply/un-premultiply around every RGBA8 read and write.
    bool u8Premultiplied = false;
    /// Device-px offset of this target's (0,0) from the caller's coordinate space (Docs/tasks/
    /// T-53b.md's bounded effect buffer: draw calls keep addressing full-viewport device
    /// coordinates, `get`/`set` subtract this before indexing). 0 for the root target and every
    /// full-viewport layer/mask surface (unchanged behavior).
    int originX = 0;
    int originY = 0;
    /// T-643: per local row of a pooled layer surface (LayerStack), nonzero once the row holds
    /// defined pixels. A pooled surface is not cleared when it is acquired; a row is zeroed the first
    /// time a draw reaches it (`rowF`, `set`), and `get` reads an undefined row as transparent, so
    /// only the rows a layer actually draws into are ever cleared. Null: every row is defined (the
    /// root target, every Surface outside the pool).
    uint8_t* rowDefined = nullptr;

    static Target fromSwTarget(const SwTarget& target);
    static Target fromSurface(Surface& surface);

    Color get(int x, int y) const;
    void set(int x, int y, const Color& c);

    /// Start of the float row holding caller-space row `y`, or null when this target is RGBA8 or
    /// `y` lies outside it. The returned pointer is indexed by *local* x (`x - originX`) times 4.
    /// Lets hot per-pixel loops hoist the row lookup and the bounds check out of the inner loop
    /// (Docs/tasks/T-82.md); `get`/`set` remain the safe per-pixel path.
    /// The row is defined (zeroed if it was not, T-643) before it is returned: every caller writes
    /// through it.
    float* rowF(int y) const {
        int ly = y - originY;
        if (f == nullptr || ly < 0 || ly >= h) {
            return nullptr;
        }
        if (rowDefined != nullptr && rowDefined[ly] == 0) {
            defineRow(ly);
        }
        return f + static_cast<size_t>(ly) * static_cast<size_t>(w) * 4;
    }
    /// Zeroes local row `ly` and marks it defined (T-643; `rowDefined` is non-null).
    void defineRow(int ly) const;
    /// Start of the RGBA8 row holding caller-space row `y`, or null when this target is float or
    /// `y` lies outside it. Indexed by local x times 4, same convention as `rowF`.
    uint8_t* rowU8(int y) const {
        int ly = y - originY;
        if (u8 == nullptr || ly < 0 || ly >= h) {
            return nullptr;
        }
        return u8 + static_cast<size_t>(ly) * static_cast<size_t>(strideBytes);
    }

    /// Src-over of one sample onto this target's (x, y). `r`, `g`, `b` are *non*-premultiplied
    /// (that is what DrawImage's texels and the paint colours are) and `a` is the effective alpha
    /// already combining coverage/paint-alpha/clip; the premultiply happens here.
    void blendSrcOver(int x, int y, float r, float g, float b, float a);
};

/// Manages the offscreen buffer pool and the Push/Pop Layer/Mask stack. `root` (the SwTarget
/// currently bound to the renderer) is passed to every method that needs to know what lies below
/// an empty stack.
class LayerStack {
public:
    /// Clears the stack (defensive: a well-formed CommandList always balances pushes/pops per
    /// Docs/design/09-command-list.md §9.2) and marks every pooled surface free again. Retains the
    /// surface pool.
    void resetStack();

    bool empty() const { return m_stack.empty(); }

    /// The target that FillPath/StrokePath/DrawImage should currently write into.
    Target currentTarget(const Target& root) const;

    /// `PushLayer` (Docs/design/09-command-list.md's LayerCmd line, T-133): opens a bounded
    /// offscreen layer covering `boundsPx` (device px, same coordinate space as `root`, rounded
    /// outwards and clamped to `root`), exactly as `pushEffectLayer` does. An empty rectangle
    /// still allocates a 1x1 surface so every push has a surface to pop. Draws issued while the
    /// frame is on top keep addressing full-viewport device coordinates; `currentTarget` returns a
    /// `Target` whose `originX`/`originY` remap them.
    void pushLayer(const Target& root, float opacity, BlendMode blend, Rect boundsPx);
    /// Composites the top layer onto the target below it (root, or the next stack entry) and
    /// releases its surface.
    void popLayer(const Target& root);

    /// `PushEffect` (Docs/tasks/T-53b.md): opens a bounded (not necessarily full-viewport) offscreen
    /// layer sized/positioned by `boundsPx` (device px, in the same coordinate space as `root`),
    /// clamped to `root`'s bounds. Opacity 1, BlendMode::Normal (the node's own opacity/blend is
    /// handled by an outer PushLayer per Docs/design/18-effects.md §18.1). Draws issued while this
    /// frame is on top keep addressing full-viewport device coordinates -- `currentTarget` returns a
    /// `Target` with `originX`/`originY` set so `Target::set`/`get` remap them into the smaller
    /// buffer.
    void pushEffectLayer(const Target& root, Rect boundsPx);
    /// The current (just-pushed) effect layer's backing surface, for the caller (sw_renderer.cpp's
    /// PopEffect) to run the effect stack over before compositing. Only valid when the top frame was
    /// opened by `pushEffectLayer`.
    Surface& topLayerSurface();
    /// The current effect layer's device-px origin (`Target::originX/Y`, as a `Vec2`).
    Vec2 topLayerOrigin() const;
    /// Composites `filtered` (same dimensions as `topLayerSurface()`, already effect-processed) onto
    /// the target below the top frame via src-over (opacity 1, Normal, per Docs/design/
    /// 18-effects.md §18.5), at the frame's origin, then releases its pooled surface. Used by
    /// PopEffect instead of `popLayer` (which composites the top surface unfiltered).
    void popEffectLayer(const Target& root, const Surface& filtered);
    /// T-343: composites `surface` (premultiplied, its top-left at `originX`/`originY` in root
    /// px) onto the current target with SrcOver at opacity 1 -- a kept band pasted back
    /// (Docs/design/23-host-integration.md §23.aa.2). Honours the root write mask.
    void compositeSurface(const Target& root, const Surface& surface, int originX, int originY);

    void pushMask(const Target& root, MaskMode mode);
    /// Switches the current mask frame's target from "mask" to "content".
    void endMaskContent(const Target& root);
    /// Computes the mask coefficient, composites content onto the target below, and releases both
    /// surfaces.
    void popMask(const Target& root);

    /// Partial redraw (Docs/tasks/T-99.md, T-642): while `region` is non-null, every composite
    /// that lands on the *root* target (the only RGBA8 target there is) writes only the pixels
    /// inside it (root device coordinates; borrowed, not copied) and leaves every other pixel
    /// untouched. Compositing onto an offscreen surface is never restricted here.
    void setRootRegion(const RectRegion* region) { m_rootRegion = region; }

    /// Exposed for tests: number of surfaces held in the pool (checked-out or free).
    size_t poolSize() const { return m_pool.size(); }
    /// Exposed for tests: the float capacity of pool slot `index` (< poolSize()).
    size_t slotCapacity(size_t index) const { return m_pool[index].rgba.capacity(); }
    /// Exposed for tests (T-643): rows of pool slot `index` cleared or drawn since its last acquire.
    size_t definedRows(size_t index) const;

    /// T-641 (Docs/design/50-c-abi.md §50.aa item 3): closes one frame of the pool's ageing; the
    /// SW renderer calls it once at the end of every render(). A slot not acquired for
    /// `kPoolIdleFrames` consecutive frames is freed. Every `kPoolIdleFrames` frames, a slot whose
    /// capacity is more than twice the largest size it served in that window is reallocated to
    /// exactly that size (the factor keeps an animated layer from reallocating every window). Does
    /// nothing while a frame is still open (an unbalanced list): the slots are in use.
    void endFrame();
    /// T-641 (§50.aa items 1-2): frees every pooled surface (the stack is reset first). The next
    /// acquire allocates again.
    void releasePool();

    /// Frames a pool slot may go unacquired before endFrame() frees it (§50.aa item 3).
    static constexpr uint32_t kPoolIdleFrames = 120;

private:
    enum class FrameKind : uint8_t { Layer, Mask };

    struct Frame {
        FrameKind kind;
        size_t surfaceA = 0;
        size_t surfaceB = 0;
        bool contentStarted = false;
        float opacity = 1;
        BlendMode blend = BlendMode::Normal;
        MaskMode maskMode = MaskMode::Alpha;
        /// Device-px origin of `surfaceA` within `root` (0,0 for every ordinary pushLayer/pushMask
        /// frame; only pushEffectLayer sets this to a non-zero bounded region's top-left corner).
        int originX = 0;
        int originY = 0;
    };

    /// Checks out a `w` x `h` surface from the pool, every row undefined (T-643: nothing is cleared
    /// here; see Target::rowDefined). Bounded layers make the requested
    /// size vary from push to push (T-133), so the search prefers the smallest free buffer whose
    /// storage already fits; a new one is added only while the pool is below `kMaxPooledSurfaces`
    /// (beyond that an ill-fitting free buffer is grown instead, so a deep stack cannot make the
    /// pool grow without bound). A buffer that must grow is reallocated to exactly `w * h` pixels
    /// (T-641: no geometric slack).
    size_t acquire(int w, int h);
    void release(size_t index);
    Target surfaceTarget(size_t index) {
        Target t = Target::fromSurface(m_pool[index]);
        t.rowDefined = m_rowDefined[index].data();
        return t;
    }
    /// Zeroes every undefined row of pool slot `index` (T-643), for the callers that read the
    /// whole surface (the effect passes, the direct-mode band copy).
    void defineAllRows(size_t index);

    const RectRegion* m_rootRegion = nullptr;

    /// T-641: a pool slot's ageing state, parallel to `m_pool`.
    struct SlotAge {
        uint32_t idleFrames = 0;  ///< consecutive endFrame()s without an acquire
        size_t windowPeak = 0;    ///< the largest `need` (floats) served in the current window
        bool acquired = false;    ///< acquired since the last endFrame()
    };

    std::vector<Surface> m_pool;
    std::vector<uint8_t> m_inUse;
    std::vector<SlotAge> m_age;
    /// T-643: per pool slot, one flag per row of its current surface (Target::rowDefined).
    std::vector<std::vector<uint8_t>> m_rowDefined;
    std::vector<Frame> m_stack;
    /// endFrame()s since the current capacity window began.
    uint32_t m_windowFrames = 0;
};

/// Per-renderer scratch for drawImage/drawImageQuad (Docs/tasks/T-149.md): `span` holds one
/// device row of premultiplied float samples (target width * 4), `row` one pre-blended source row
/// (image width * 4). Owned by SwRenderer so no draw allocates.
struct ImageScratch {
    std::vector<float> span;
    std::vector<float> row;
};

/// Rasterizes `cmd.dst` (transformed by `deviceXf`) as a quad and, for every covered device pixel,
/// samples `image` (nearest or bilinear per `cmd.smooth`) and composites it into `target` via
/// src-over. `clipMask`/`clipWidth` follow SwRenderer::Impl::clipData's convention (null means "no
/// clip mask"). `region` (T-642), when non-null, restricts the rows swept and the pixels sampled
/// and written to WriteRegion's pixels; every pixel it lets through comes out byte-identical to an
/// unrestricted draw (every span keeps sampling from the same anchor). An axis-aligned `deviceXf` (no
/// rotation or shear) takes the row-incremental fast path, anything else the per-pixel inverse mapping
/// (Docs/tasks/T-149.md). See Docs/design/10-backends.md §10.2.5, Docs/tasks/T-12.md
/// "描邊、Dash、ClipPath、圖片".
void drawImage(Target& target, Rasterizer& raster, const Mat2x3& deviceXf, const ImageCmd& cmd,
               const ImageData& image, const uint8_t* clipMask, int clipWidth, ImageScratch& scratch,
               const WriteRegion* region = nullptr);

/// Draws `image` warped into device-space quad `q` (4 corners, clockwise from top-left) via a
/// perspective homography: `homographyFromQuad` maps `q` to the unit square, then every covered
/// device pixel's center is mapped back to (u, v) (with the homogeneous divide) for sampling.
/// `srcUv`/`opacity`/`smooth` follow drawImage's conventions; edge coverage comes from rasterizing
/// the quad itself, same as drawImage's rotated-rect handling. A degenerate quad (homographyFromQuad
/// fails) is skipped. `region` as for drawImage. See Docs/design/19-3d-space.md §19.7,
/// Docs/tasks/T-45.md.
void drawImageQuad(Target& target, Rasterizer& raster, const Vec2 q[4], Rect srcUv, float opacity,
                   uint8_t smooth, const ImageData& image, const uint8_t* clipMask, int clipWidth,
                   ImageScratch& scratch, const WriteRegion* region = nullptr);

/// Draws the triangles of a DrawImageMesh (Docs/design/32-rigging-and-deformation.md §32.2.4,
/// T-245): `indices` (3 per triangle, relative to `vertices`) over `vertices` already in *device*
/// coordinates. Each triangle is textured affinely (no perspective correction), covers the pixel
/// centers inside it under the top-left rule (no antialiasing, so adjacent triangles neither leave
/// a seam nor blend a pixel twice) and samples like drawImage (nearest / bilinear per `smooth`,
/// premultiplied, `opacity`, `clipMask`, `region`). Degenerate triangles and out-of-range indices
/// are skipped.
void drawImageMesh(Target& target, const MeshVertex* vertices, uint32_t vertexCount, const uint16_t* indices,
                   uint32_t indexCount, float opacity, uint8_t smooth, const ImageData& image,
                   const uint8_t* clipMask, int clipWidth, ImageScratch& scratch,
                   const WriteRegion* region = nullptr);

/// Draws the instances of a DrawParticles command (Docs/tasks/T-202.md item 4): one rotated quad
/// per instance, mapped through `deviceXf`, in instance order. With `image` each covered pixel is
/// the bilinear texel at the instance's [uv0, uv1] (premultiplied) times the instance color;
/// without, the instance color alone (a filled square). `additive` adds the premultiplied source
/// to the destination (clamped to 1) instead of source-over. Coverage, the clip mask and `region`
/// apply as for drawImage.
void drawParticles(Target& target, Rasterizer& raster, const Mat2x3& deviceXf,
                   const ParticleInstance* instances, uint32_t count, bool additive, const ImageData* image,
                   const uint8_t* clipMask, int clipWidth, ImageScratch& scratch,
                   const WriteRegion* region = nullptr);

}  // namespace xt::sw
