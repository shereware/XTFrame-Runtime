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
/// the root SwTarget (uint8) or a pooled Surface (float). See docs/design/10-backends.md §10.2.4,
/// docs/tasks/T-12.md "Layer 與 Mask（sw_layer）".

namespace xt::sw {

/// A **premultiplied** float RGBA offscreen buffer, tightly packed (row pitch == w * 4 floats).
/// Premultiplied since T-102 (docs/design/23-host-integration.md §23.5): source-over is then
/// `dst = src + dst * (1 - srcA)`, with no divide and no `outA == 0` branch, and it vectorizes.
/// `sw_blend.h`'s `premultiply`/`unpremultiply` convert at the few places that need W3C's
/// non-premultiplied channels (the 16 non-Normal blend modes, ColorAdjust's colour matrix).
struct Surface {
    int w = 0;
    int h = 0;
    std::vector<float> rgba;

    /// Inline (docs/tasks/T-82.md): the effect passes call these once per pixel, and a cross-TU
    /// call per pixel was a measurable share of the frame.
    Color get(int x, int y) const {
        const float* p =
            rgba.data() + (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
        return Color{p[0], p[1], p[2], p[3]};
    }
    /// Start of row `y` (4 floats per pixel, tightly packed), indexed by `x * 4`. The effect
    /// passes walk whole rows, so hoisting the row base out of the inner loop removes the index
    /// arithmetic per pixel and lets the span kernels (sw_simd.h) run over a whole row
    /// (docs/tasks/T-102b.md). Unlike `Target::rowF` there is no bounds check: `y` must be in
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
    /// Device-px offset of this target's (0,0) from the caller's coordinate space (docs/tasks/
    /// T-53b.md's bounded effect buffer: draw calls keep addressing full-viewport device
    /// coordinates, `get`/`set` subtract this before indexing). 0 for the root target and every
    /// full-viewport layer/mask surface (unchanged behavior).
    int originX = 0;
    int originY = 0;

    static Target fromSwTarget(const SwTarget& target);
    static Target fromSurface(Surface& surface);

    Color get(int x, int y) const;
    void set(int x, int y, const Color& c);

    /// Start of the float row holding caller-space row `y`, or null when this target is RGBA8 or
    /// `y` lies outside it. The returned pointer is indexed by *local* x (`x - originX`) times 4.
    /// Lets hot per-pixel loops hoist the row lookup and the bounds check out of the inner loop
    /// (docs/tasks/T-82.md); `get`/`set` remain the safe per-pixel path.
    float* rowF(int y) const {
        int ly = y - originY;
        if (f == nullptr || ly < 0 || ly >= h) {
            return nullptr;
        }
        return f + static_cast<size_t>(ly) * static_cast<size_t>(w) * 4;
    }
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
    /// docs/design/09-command-list.md §9.2) and marks every pooled surface free again. Retains the
    /// surface pool.
    void resetStack();

    bool empty() const { return m_stack.empty(); }

    /// The target that FillPath/StrokePath/DrawImage should currently write into.
    Target currentTarget(const Target& root) const;

    /// `PushLayer` (docs/design/09-command-list.md's LayerCmd line, T-133): opens a bounded
    /// offscreen layer covering `boundsPx` (device px, same coordinate space as `root`, rounded
    /// outwards and clamped to `root`), exactly as `pushEffectLayer` does. An empty rectangle
    /// still allocates a 1x1 surface so every push has a surface to pop. Draws issued while the
    /// frame is on top keep addressing full-viewport device coordinates; `currentTarget` returns a
    /// `Target` whose `originX`/`originY` remap them.
    void pushLayer(const Target& root, float opacity, BlendMode blend, Rect boundsPx);
    /// Composites the top layer onto the target below it (root, or the next stack entry) and
    /// releases its surface.
    void popLayer(const Target& root);

    /// `PushEffect` (docs/tasks/T-53b.md): opens a bounded (not necessarily full-viewport) offscreen
    /// layer sized/positioned by `boundsPx` (device px, in the same coordinate space as `root`),
    /// clamped to `root`'s bounds. Opacity 1, BlendMode::Normal (the node's own opacity/blend is
    /// handled by an outer PushLayer per docs/design/18-effects.md §18.1). Draws issued while this
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
    /// the target below the top frame via src-over (opacity 1, Normal, per docs/design/
    /// 18-effects.md §18.5), at the frame's origin, then releases its pooled surface. Used by
    /// PopEffect instead of `popLayer` (which composites the top surface unfiltered).
    void popEffectLayer(const Target& root, const Surface& filtered);
    /// T-343: composites `surface` (premultiplied, its top-left at `originX`/`originY` in root
    /// px) onto the current target with SrcOver at opacity 1 -- a kept band pasted back
    /// (docs/design/23-host-integration.md §23.aa.2). Honours the root write mask.
    void compositeSurface(const Target& root, const Surface& surface, int originX, int originY);

    void pushMask(const Target& root, MaskMode mode);
    /// Switches the current mask frame's target from "mask" to "content".
    void endMaskContent(const Target& root);
    /// Computes the mask coefficient, composites content onto the target below, and releases both
    /// surfaces.
    void popMask(const Target& root);

    /// Partial redraw (docs/tasks/T-99.md): while `mask` is non-null, every composite that lands
    /// on the *root* target (the only RGBA8 target there is) leaves pixels whose mask byte is 0
    /// untouched. `mask` is `strideInPixels * root height` bytes in root device coordinates and is
    /// borrowed, not copied. Compositing onto an offscreen surface is never masked -- an effect
    /// reads its own neighborhood, so the buffers below the root stay complete.
    void setRootWriteMask(const uint8_t* mask, int strideInPixels) {
        m_rootWriteMask = mask;
        m_rootWriteMaskStride = strideInPixels;
    }

    /// Exposed for tests: number of surfaces held in the pool (checked-out or free).
    size_t poolSize() const { return m_pool.size(); }

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

    /// True when the pixel at root device coordinates (`x`, `y`) must not be written because a
    /// root write mask is active and masks it out. `dstU8 != nullptr` is what identifies the root
    /// target in the composite loops (only the root is RGBA8).
    bool rootWriteMasked(int x, int y) const {
        if (m_rootWriteMask == nullptr) {
            return false;
        }
        return m_rootWriteMask[static_cast<size_t>(y) * static_cast<size_t>(m_rootWriteMaskStride) +
                               static_cast<size_t>(x)] == 0;
    }

    /// Checks out a cleared `w` x `h` surface from the pool. Bounded layers make the requested
    /// size vary from push to push (T-133), so the search prefers the smallest free buffer whose
    /// storage already fits; a new one is added only while the pool is below `kMaxPooledSurfaces`
    /// (beyond that an ill-fitting free buffer is grown instead, so a deep stack cannot make the
    /// pool grow without bound). Only the first `w * h` pixels are cleared.
    size_t acquire(int w, int h);
    void release(size_t index);
    Target surfaceTarget(size_t index) { return Target::fromSurface(m_pool[index]); }

    const uint8_t* m_rootWriteMask = nullptr;
    int m_rootWriteMaskStride = 0;

    std::vector<Surface> m_pool;
    std::vector<uint8_t> m_inUse;
    std::vector<Frame> m_stack;
};

/// Per-renderer scratch for drawImage/drawImageQuad (docs/tasks/T-149.md): `span` holds one
/// device row of premultiplied float samples (target width * 4), `row` one pre-blended source row
/// (image width * 4). Owned by SwRenderer so no draw allocates.
struct ImageScratch {
    std::vector<float> span;
    std::vector<float> row;
};

/// Rasterizes `cmd.dst` (transformed by `deviceXf`) as a quad and, for every covered device pixel,
/// samples `image` (nearest or bilinear per `cmd.smooth`) and composites it into `target` via
/// src-over. `clipMask`/`clipWidth` follow SwRenderer::Impl::clipAt's convention (`clipMask.empty()`
/// means "no clip", i.e. `clipMask == nullptr` here). An axis-aligned `deviceXf` (no rotation or
/// shear) takes the row-incremental fast path, anything else the per-pixel inverse mapping
/// (docs/tasks/T-149.md). See docs/design/10-backends.md §10.2.5, docs/tasks/T-12.md
/// "描邊、Dash、ClipPath、圖片".
void drawImage(Target& target, Rasterizer& raster, const Mat2x3& deviceXf, const ImageCmd& cmd,
               const ImageData& image, const uint8_t* clipMask, int clipWidth, ImageScratch& scratch);

/// Draws `image` warped into device-space quad `q` (4 corners, clockwise from top-left) via a
/// perspective homography: `homographyFromQuad` maps `q` to the unit square, then every covered
/// device pixel's center is mapped back to (u, v) (with the homogeneous divide) for sampling.
/// `srcUv`/`opacity`/`smooth` follow drawImage's conventions; edge coverage comes from rasterizing
/// the quad itself, same as drawImage's rotated-rect handling. A degenerate quad (homographyFromQuad
/// fails) is skipped. See docs/design/19-3d-space.md §19.7, docs/tasks/T-45.md.
void drawImageQuad(Target& target, Rasterizer& raster, const Vec2 q[4], Rect srcUv, float opacity,
                   uint8_t smooth, const ImageData& image, const uint8_t* clipMask, int clipWidth,
                   ImageScratch& scratch);

/// Draws the triangles of a DrawImageMesh (docs/design/32-rigging-and-deformation.md §32.2.4,
/// T-245): `indices` (3 per triangle, relative to `vertices`) over `vertices` already in *device*
/// coordinates. Each triangle is textured affinely (no perspective correction), covers the pixel
/// centers inside it under the top-left rule (no antialiasing, so adjacent triangles neither leave
/// a seam nor blend a pixel twice) and samples like drawImage (nearest / bilinear per `smooth`,
/// premultiplied, `opacity`, `clipMask`). Degenerate triangles and out-of-range indices are
/// skipped.
void drawImageMesh(Target& target, const MeshVertex* vertices, uint32_t vertexCount, const uint16_t* indices,
                   uint32_t indexCount, float opacity, uint8_t smooth, const ImageData& image,
                   const uint8_t* clipMask, int clipWidth, ImageScratch& scratch);

/// Draws the instances of a DrawParticles command (docs/tasks/T-202.md item 4): one rotated quad
/// per instance, mapped through `deviceXf`, in instance order. With `image` each covered pixel is
/// the bilinear texel at the instance's [uv0, uv1] (premultiplied) times the instance color;
/// without, the instance color alone (a filled square). `additive` adds the premultiplied source
/// to the destination (clamped to 1) instead of source-over. Coverage and the clip mask apply as
/// for drawImage.
void drawParticles(Target& target, Rasterizer& raster, const Mat2x3& deviceXf,
                   const ParticleInstance* instances, uint32_t count, bool additive, const ImageData* image,
                   const uint8_t* clipMask, int clipWidth, ImageScratch& scratch);

}  // namespace xt::sw
