#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "xt/assert.h"
#include "xt/gpu/gpu_frame.h"

/// FrameEmitter: the low-level writer behind FrameBuilder (frame_builder.cpp) and the effect
/// scheduler (gpu_effects.cpp). Owns nothing; it appends to the caller's GpuFrame and tracks the
/// open pass, the scissor every emitted command inherits, the per-frame transient target slots
/// (reused once released, Docs/tasks/T-146.md "同一幀內 handle 有效") and the window quads.
///
/// Windows (Docs/tasks/T-151.md): every transient target is created for a *window*, a rectangle
/// of the surface in surface pixels; the transient is exactly the window's size and a pass into
/// it carries the window in `Pass::view*`. Vertices and scissors stay in surface coordinates
/// throughout; the device maps them into the window (gpu_frame.h). The surface target's window
/// is the whole surface.

namespace xt::gpu::detail {

/// A vertex/index range of the frame, in DrawCmd's own terms.
struct Geometry {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    uint32_t baseVertex = 0;
    uint32_t vertexCount = 0;
};

/// Integer rectangle in surface pixels (a scissor, or a target window).
struct Scissor {
    int x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
    bool operator==(const Scissor& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};

/// Intersection of two scissors (empty when disjoint).
Scissor intersect(const Scissor& a, const Scissor& b);

/// Transient window granularity: sizes are padded up to a multiple of this and to at least
/// `kMinWindow` on each side (never beyond the surface), so a host's exact-size target pool sees
/// few distinct sizes (Docs/tasks/T-151.md point 1).
inline constexpr int kWindowGranularity = 32;
inline constexpr int kMinWindow = 64;

class FrameEmitter {
public:
    /// Clears `frame`, records the surface size and opens the first pass (the surface, no clear).
    void begin(GpuFrame& frame, int width, int height);
    /// Closes the open pass and drops passes that do nothing.
    void end();

    GpuFrame& frame() { return *m_frame; }
    int width() const { return m_width; }
    int height() const { return m_height; }

    // -- Passes --------------------------------------------------------------------------

    /// Closes the current pass and opens one on `target`, with `Pass::view*` = `target`'s
    /// window. `copySource` (with `copy`) is applied after `clear`, before the pass's commands;
    /// the device copies the overlap of the two windows.
    void beginPass(TargetHandle target, bool clear, bool copy = false, TargetHandle copySource = 0);
    TargetHandle currentTarget() const { return m_currentTarget; }

    // -- Transient targets ---------------------------------------------------------------

    /// The window of `target` in surface pixels (the whole surface for `kSurfaceTarget`).
    Scissor windowOf(TargetHandle target) const;
    Scissor currentWindow() const { return windowOf(m_currentTarget); }

    /// Turns a wanted rectangle into a window for a new transient drawn while `currentTarget()`
    /// is the parent: the intersection with the parent's window, padded to the granularity above
    /// and kept inside the surface. An empty request yields a minimal window at its position.
    Scissor targetWindow(const Scissor& wanted) const;

    /// Returns a transient of `fmt` sized to `window` (a `targetWindow` result) whose content is
    /// dead, reusing a released slot with the same size/format/stencil when there is one; the
    /// slot takes on `window` as its position. T-343: `scaleShift` > 0 asks for a *scaled*
    /// transient (gpu_frame.h `TransientTargetDesc`): `ceil(window / 2^scaleShift)` texels over
    /// the same window.
    TargetHandle acquireTransient(TextureFormat fmt, bool needStencil, const Scissor& window,
                                  int scaleShift = 0);
    /// The texel size of `target` (T-343: a scaled transient is smaller than its window); the
    /// surface size for `kSurfaceTarget`.
    void sizeOf(TargetHandle target, int& w, int& h) const;
    /// Marks `target`'s content dead so a later acquire may reuse the slot.
    void releaseTransient(TargetHandle target);

    /// T-728 (Docs/design/10-backends.md §10.18 A item 3): true only when `target` is a transient
    /// whose first pass since it was acquired cleared it (and copied nothing into it), `region`
    /// lies inside its window, and nothing written to it since -- a copy into it, or any color
    /// command of its passes (a command's vertex box intersected with its scissor; particles, and
    /// projected vertices with w <= 0, count as the whole window) -- touches `region`: the region
    /// then still holds the clear's transparent black. Stencil-only commands write no color and
    /// are not counted. Conservative: any doubt answers false.
    bool untouchedSinceClear(TargetHandle target, const Scissor& region) const;

    // -- Scissor ---------------------------------------------------------------------------

    void setScissor(const Scissor& s) { m_scissor = s; }
    const Scissor& scissor() const { return m_scissor; }
    Scissor fullScissor() const { return Scissor{0, 0, m_width, m_height}; }

    // -- Analytic clip (T-718, Docs/design/10-backends.md §10.16 W1) ---------------------------

    /// The `AnalyticClipParams` block (its index in the frame's params, 0 = none) every leaf draw
    /// appended from now on carries in `DrawCmd::clipParamIndex`, whatever the pass: Solid,
    /// Gradient, Texture, TextureStraight, Glyph, BoxShadow and the particle shadings. StencilOnly,
    /// Composite and Effect commands never carry one. A group's result drawn as a textured quad
    /// (a cached effect, a band) is not a leaf: its caller sets 0 around it.
    void setAnalyticClip(uint32_t paramIndex) { m_analyticClip = paramIndex; }
    uint32_t analyticClip() const { return m_analyticClip; }

    // -- Channel reads (T-728, Docs/design/10-backends.md §10.18 A item 1) ---------------------

    /// Whether the device reads the trailing `channel` field of DownsampleParams / GaussParams
    /// (GpuDevice::supportsAlphaChannelReads, set by the builder once per build; kept across
    /// `begin`). Off: `pushChannelParams` leaves the field out, so the frame is the one of before.
    void setChannelReads(bool on) { m_channelReads = on; }
    bool channelReads() const { return m_channelReads; }

    // -- Backdrop mask (T-729, Docs/design/10-backends.md §10.18 B) ----------------------------

    /// Whether the device runs EffectProgram::BackdropMasked (GpuDevice::supportsBackdropMask, set
    /// by the builder once per build; kept across `begin`).
    void setBackdropMask(bool on) { m_backdropMask = on; }
    bool backdropMask() const { return m_backdropMask; }
    /// T-764: whether the device reads BackdropParams' texture inputs (backdropMask() and
    /// GpuDevice::supportsBackdropMaskTextures, set by the builder once per build; kept across `begin`).
    void setBackdropMaskTextures(bool on) { m_backdropMaskTextures = on; }
    bool backdropMaskTextures() const { return m_backdropMaskTextures; }

    // -- Geometry --------------------------------------------------------------------------

    /// Appends `count` vertices (returned pointer valid until the next append) whose
    /// `baseVertex` is written to `base`.
    Vertex* appendVertices(uint32_t count, uint32_t& base);
    /// Appends `count` indices (relative to their command's baseVertex).
    uint32_t* appendIndices(uint32_t count, uint32_t& first);
    /// A quad `v[0..3]` in clockwise (or any consistent) corner order, two triangles.
    Geometry quad(const Vertex v[4]);
    /// The quad covering `window` (surface pixels; u,v from (0,0) at its top-left to (1,1) at
    /// its bottom-right, w = 1, rgba opaque white), shared per distinct window within a frame.
    Geometry windowQuad(const Scissor& window);

    // -- Parameters ------------------------------------------------------------------------

    template <class T>
    uint32_t pushParams(const T& p) {
        static_assert(sizeof(T) % sizeof(float) == 0, "params must be whole floats");
        const uint32_t index = static_cast<uint32_t>(m_frame->params.size());
        m_frame->params.resize(m_frame->params.size() + sizeof(T) / sizeof(float));
        std::memcpy(m_frame->params.data() + index, &p, sizeof(T));
        return index;
    }

    /// T-728: a DownsampleParams / GaussParams block -- whole when `channelReads()`, else without its
    /// trailing `channel` float (which must then be 0: no step asks for a channel on such a device).
    template <class T>
    uint32_t pushChannelParams(const T& p) {
        static_assert(offsetof(T, channel) + sizeof(float) == sizeof(T), "channel must be the last float");
        if (m_channelReads) {
            return pushParams(p);
        }
        XT_ASSERT(p.channel == 0.0f);
        const uint32_t index = static_cast<uint32_t>(m_frame->params.size());
        constexpr size_t floats = offsetof(T, channel) / sizeof(float);
        m_frame->params.resize(m_frame->params.size() + floats);
        std::memcpy(m_frame->params.data() + index, &p, floats * sizeof(float));
        return index;
    }

    /// T-764: a BackdropParams block -- whole when it names texture inputs (`mask` ==
    /// kBackdropMaskTextured, only on a device with backdropMaskTextures()), else its one float `mask`,
    /// the block of before.
    uint32_t pushBackdropParams(const BackdropParams& p) {
        if (p.mask == kBackdropMaskTextured) {
            XT_ASSERT(m_backdropMaskTextures);
            return pushParams(p);
        }
        const uint32_t index = static_cast<uint32_t>(m_frame->params.size());
        m_frame->params.push_back(p.mask);
        return index;
    }

    // -- Commands --------------------------------------------------------------------------

    /// Appends `cmd` to the open pass with the geometry and the current scissor filled in.
    /// Returns a reference valid until the next append.
    ///
    /// State batching (Docs/tasks/T-155.md point 2): when `cmd` is a color draw that neither
    /// writes the stencil nor cares about draw boundaries (Solid/Gradient/Texture/
    /// TextureStraight/Glyph/TexturePlanar with stencil None or TestEqualRef) or a ClearRect, the previous
    /// command of the open pass has identical state (shading, blend, stencil op/ref, texture,
    /// coverage (T-660b), sampler, program, paramIndex, src/aux, scissor, analytic clip (T-718)) and its
    /// geometry ends exactly where `g` begins, `g` is folded into that command (its indices rebased onto the
    /// earlier baseVertex) and the earlier command is returned instead of a new one.
    DrawCmd& pushCmd(DrawCmd cmd, const Geometry& g);

    /// Appends a `Shading::Effect` command covering the current target's window (its quad and
    /// its scissor) in the current pass.
    void pushEffect(EffectProgram program, uint32_t paramIndex, TargetHandle src, TargetHandle aux = 0,
                    TextureHandle texture = 0);

private:
    struct Slot {
        TransientTargetDesc desc;
        Scissor window;
        bool inUse = false;
        /// T-728: the index of the first pass on this slot since it was acquired (-1: none yet) and
        /// whether that pass cleared it without a copy (untouchedSinceClear).
        int firstPass = -1;
        bool freshClear = false;
    };
    struct WindowQuad {
        Scissor window;
        Geometry geometry;
    };
    GpuFrame* m_frame = nullptr;
    int m_width = 0;
    int m_height = 0;
    TargetHandle m_currentTarget = kSurfaceTarget;
    bool m_passOpen = false;
    Scissor m_scissor;
    uint32_t m_analyticClip = 0;          ///< T-718: setAnalyticClip
    bool m_channelReads = false;          ///< T-728: setChannelReads
    bool m_backdropMask = false;          ///< T-729: setBackdropMask
    bool m_backdropMaskTextures = false;  ///< T-764: setBackdropMaskTextures
    std::vector<Slot> m_slots;
    std::vector<WindowQuad> m_windowQuads;

    void closePass();
};

}  // namespace xt::gpu::detail
