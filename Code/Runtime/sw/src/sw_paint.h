#pragma once

#include <span>
#include <vector>

#include "xt/command_list.h"
#include "xt/types.h"
#include "xt/value.h"

/// Per-pixel paint (gradient/solid) evaluation. See Docs/design/10-backends.md §10.2.2,
/// Docs/tasks/T-12.md "漸層（sw_paint）", Docs/tasks/T-128.md step 2.

namespace xt::sw {

/// Number of entries a gradient's colour ramp is expanded into (Docs/tasks/T-128.md step 2:
/// "stop <= 16 -> 預先展開成 256 項 LUT"). `t` in [0, 1] maps to the nearest entry, so a colour is
/// never more than half a step -- at most 1/255 of the ramp's own colour range -- away from the
/// exact interpolation the pre-T-128 per-pixel stop search produced.
inline constexpr int kGradientLutSize = 256;

/// One render's cache of expanded gradient ramps, owned by the renderer. A ramp is
/// `kGradientLutSize` **premultiplied** RGBA float quads (the paint's own alpha already folded in),
/// which is the form the T-102 span kernels consume, so a fully covered gradient pixel is a plain
/// 16-byte copy out of the ramp.
///
/// Keyed by the stop list's contents plus the paint's alpha (Docs/tasks/T-128.md step 2's
/// "per-paint 快取，鍵 = stopOffset/stopCount/color"): keying on the stop *values* rather than on
/// their offset in the CommandList is what makes the 45 identical rain streaks -- each with its own
/// stop range -- share one ramp, and it cannot go stale when a producer rewrites the stops behind a
/// reused offset.
///
/// Entry storage is reused across frames (`reset()` only drops the live count), so a steady-state
/// frame allocates nothing (Docs/design/01-architecture.md §1.7, Tests/perf/test_zero_alloc.cpp).
class GradientLutCache {
public:
    /// Forgets every cached ramp but keeps the storage. Called once per render().
    void reset() { m_live = 0; }

    /// The ramp for `stops` scaled by `alpha`: `kGradientLutSize * 4` premultiplied floats,
    /// building it on a miss. Never null; valid until the next `reset()`.
    const float* get(std::span<const GradientStop> stops, float alpha);

private:
    /// Beyond this many distinct ramps the linear scan stops paying for itself; the cache is then
    /// dropped and refilled. A frame with more distinct gradients than this is not the shape
    /// T-128 measures, and rebuilding one ramp is only kGradientLutSize stop lookups.
    static constexpr size_t kMaxEntries = 64;

    struct Entry {
        float alpha = 0;
        std::vector<GradientStop> stops;
        std::vector<float> lut;
    };

    std::vector<Entry> m_entries;
    size_t m_live = 0;
};

/// Evaluates a PaintRef at any device pixel. Built once per FillPath/StrokePath command.
class PaintEval {
public:
    /// `stops` is the gradient's stop range (ignored for Solid paints). `xfDevice` is the same
    /// device transform (xf * DPR) used to flatten the path being painted; PaintRef::p0/p1/p2 are
    /// expressed in that path's own (pre-xf) coordinate system, so `at` maps device pixels back
    /// through `xfDevice`'s inverse before evaluating gradient math. `luts` expands the stops once
    /// per distinct ramp instead of searching them per pixel; a Solid paint never touches it.
    ///
    /// `screenToPlane` (T-160) is set for a gradient drawn under a 3D projection (19.7): the path
    /// was flattened in plane space (through `xfDevice`) and then projected per point, so a device
    /// pixel has to go back through this homography into plane space before `xfDevice`'s inverse
    /// applies. Null (the default) means no projection: device pixels are plane points.
    static PaintEval make(const PaintRef& paint, std::span<const GradientStop> stops, const Mat2x3& xfDevice,
                          GradientLutCache& luts, const Mat3* screenToPlane = nullptr);

    /// Returns the non-premultiplied color at device pixel center (px + 0.5, py + 0.5). Only the
    /// partially-clipped pixels at a span's edge still go through it; whole runs take `spanPremul`.
    Color at(float px, float py) const;

    /// True when `at` returns the same color for every pixel, i.e. a Solid paint. Lets the span
    /// painter hoist the color out of its inner loop (Docs/tasks/T-82.md's solid-fill fast path).
    bool isSolid() const { return m_kind == PaintKind::Solid; }
    /// The constant color of a Solid paint; meaningless unless `isSolid()`.
    const Color& solid() const { return m_solid; }

    /// What `spanPremul` found while writing one run (Docs/tasks/T-128.md step 2).
    struct SpanInfo {
        /// First index of the run whose effective alpha is greater than 0, or `n` when the whole
        /// run came out fully transparent. Blending only [first, last) skips the transparent
        /// margin a radial gradient leaves on most of its spans without changing a pixel:
        /// source-over with alpha 0 is the identity. Only [first, last) of `dst` was written.
        int first = 0;
        /// One past the last index whose effective alpha is greater than 0.
        int last = 0;
        /// True when every pixel in [first, last) came out fully opaque, which reduces
        /// source-over to a plain store (Docs/tasks/T-128.md step 2's opaque fast path).
        bool allOpaque = false;
    };

    /// Evaluates `n` consecutive device pixels starting at (x, y) into `dst` as **premultiplied**
    /// RGBA floats (4 per pixel), each already multiplied by `alphaScale` (the span's coverage).
    /// Only valid for a gradient paint. `t` is stepped incrementally along the span -- it is
    /// affine in x for a linear gradient, and so are the ellipse-normalized offsets of a radial one
    /// -- so the inner loop is a ramp copy plus, for a radial gradient, one sqrt. Indices outside
    /// the returned [first, last) are left untouched in `dst`.
    SpanInfo spanPremul(int x, int y, int n, float alphaScale, float* dst) const;

private:
    /// The gradient parameter `t` (clamped to [0, 1]) at a device-space point, going through the
    /// projection homography first when one is set. Not valid for a Solid or degenerate paint.
    float tAt(Vec2 devicePoint) const;

    /// Shared by `at` and `spanPremul`: the premultiplied ramp entry nearest `t` (in [0, 1]).
    const float* sample(float t) const {
        int i = static_cast<int>(t * static_cast<float>(kGradientLutSize - 1) + 0.5f);
        if (i < 0) {
            i = 0;
        } else if (i > kGradientLutSize - 1) {
            i = kGradientLutSize - 1;
        }
        return m_lut + static_cast<size_t>(i) * 4;
    }

    /// The `[i0, i1)` sub-range of a span of `n` pixels that can hold a visible pixel, given that
    /// the pixels outside the gradient's own extent clamp to a fully transparent ramp end. `a`,
    /// `b`, `c` are the coefficients of `a*i^2 + b*i + c <= 0`, the "inside the extent" test.
    /// Widened by a pixel on each side, so a pixel the loop would have found visible is never
    /// dropped by rounding. Returns false when nothing in the span can be visible.
    static bool visibleRange(float a, float b, float c, int n, int& i0, int& i1);
    /// T-201: a conic gradient's t for offset `v` from the centre and start ray `d`.
    static float conicT(Vec2 v, Vec2 d);

    PaintKind m_kind = PaintKind::Solid;
    Color m_solid{};
    Mat2x3 m_inverse{};
    Vec2 m_p0{};
    Vec2 m_p1{};
    Vec2 m_p2{};
    /// Premultiplied ramp for a gradient paint (`kGradientLutSize * 4` floats); null for Solid.
    const float* m_lut = nullptr;
    /// Set when |p1 - p0| == 0, i.e. a degenerate gradient that paints the last stop's colour
    /// everywhere. Kept as a flag so neither `at` nor the span loop divides by zero.
    bool m_degenerate = false;
    /// T-160: set when the paint is evaluated under a 3D projection; `m_screenToPlane` then maps a
    /// device pixel back into plane space (t is no longer affine along a span there).
    bool m_projective = false;
    Mat3 m_screenToPlane{};
};

}  // namespace xt::sw
