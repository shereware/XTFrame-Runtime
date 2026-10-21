#pragma once

#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

#include "xt/node.h"
#include "xt/types.h"

/// Geometry utilities shared by backends: path flattening, stroke-to-fill, dashing and trim.
/// See docs/design/10-backends.md §10.1, docs/design/03-scene-and-properties.md §3.4, §3.8

namespace xt {

/// Default bezier flattening tolerance (px, in transformed/device space).
inline constexpr float kFlattenTolerance = 0.25f;

/// Flattens a path (verbs/points in the §3.4 layout) into polylines, applying `xf` to every point.
/// `tolerance` is the max chord error in the transformed space.
///
/// Output is appended (callers must clear the vectors themselves): `outPoints` holds the flattened
/// vertices of every contour back to back; `outContourStarts[i]` is the index into `outPoints` where
/// contour i begins; `outClosed[i]` is non-zero if contour i ended with a Close verb.
///
/// Two consecutive Move verbs, or a Move immediately followed by Close, produce a contour with a
/// single point (kept, so callers such as strokeToFill can still draw a Round cap dot there).
/// A Line/Cubic verb that is not preceded by a Move in the current contour is treated as if a
/// Move to (0, 0) (in the node's local space, before `xf`) had occurred first; this is a malformed
/// input and triggers XT_ASSERT in Debug builds while still degrading gracefully in Release.
void flattenPath(std::span<const PathVerb> verbs, std::span<const float> points, const Mat2x3& xf,
                 float tolerance, std::vector<Vec2>& outPoints, std::vector<uint32_t>& outContourStarts,
                 std::vector<uint8_t>& outClosed);

/// Number of line segments to approximate a cubic bezier (p0, p1, p2, p3) within `tolerance`
/// (Wang's formula variant, §10.1): `dd` is the larger of the two control points' squared distance
/// to the chord line p0-p3, clamped to [1, 64] subdivisions.
uint32_t cubicSubdivisions(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, float tolerance);

/// Converts a stroke into one or more closed fill contours (NonZero winding), from the flattened
/// polyline representation produced by flattenPath. Every input contour yields zero or more closed
/// output contours (appended to outPoints/outContourStarts).
///
/// Algorithm: each segment becomes a quad offset by +-width/2 along its normal; interior vertices get
/// a join shape; open contours get a cap shape at each end. All output contours are wound clockwise
/// (polygonSignedArea > 0) regardless of input winding, so overlapping quads/joins/caps add up
/// correctly under NonZero fill.
///
/// Round join/cap arcs are approximated with `segments = max(2, ceil(angle / acos(1 - tolerance/r)))`
/// (r = width/2, angle in radians; segments = 2 when r <= tolerance).
/// Miter joins degenerate to Bevel when the miter length ratio `1 / sin(theta/2)` exceeds
/// `miterLimit` (theta = interior angle at the joint).
/// Single-point contours: Round cap draws a full circle; Square cap draws a `width`-side square;
/// Butt produces no output for that contour.
void strokeToFill(std::span<const Vec2> points, std::span<const uint32_t> contourStarts,
                  std::span<const uint8_t> closed, float width, LineCap cap, LineJoin join, float miterLimit,
                  float tolerance, std::vector<Vec2>& outPoints, std::vector<uint32_t>& outContourStarts);

/// How far a stroke of `width` can reach past its path, in the path's own units: `0.5 * width * k`
/// with `k = max(miterLimit when join is Miter, sqrt(2) when cap is Square, 1)` -- a miter tip is
/// at most `miterLimit` half-widths from its vertex (beyond that strokeToFill bevels), a square
/// cap's corner `sqrt(2)` half-widths from its endpoint, everything else one half-width. Shared
/// by the Painter's node bounds and the damage list's StrokePath bounds (docs/tasks/T-263a.md B7).
float strokeOutset(float width, LineCap cap, LineJoin join, float miterLimit);

/// T-233 (31 §31.4.7): a non-owning reference to a `float(float u)` callable giving the full stroke
/// width at arc-length fraction `u` (0..1) of a contour. The referenced callable must outlive the
/// WidthFn (pass a named lambda or function object, not a temporary stored for later).
class WidthFn {
public:
    template <class F, class = std::enable_if_t<!std::is_same_v<std::decay_t<F>, WidthFn>>>
    WidthFn(const F& f)  // NOLINT(google-explicit-constructor): used like a function_ref.
        : m_ctx(&f), m_call([](const void* ctx, float u) { return (*static_cast<const F*>(ctx))(u); }) {}
    /// The width at arc-length fraction `u`.
    float operator()(float u) const { return m_call(m_ctx, u); }

private:
    const void* m_ctx;
    float (*m_call)(const void*, float);
};

/// T-233 (31 §31.4.7): variable-width strokeToFill. Same inputs, output and winding as the
/// constant-width overload, but the width at every point is `width(u)`, `u` being the point's
/// arc-length fraction along its own contour (0 at the first point, 1 at the last; a closed
/// contour's closing segment ends at u = 1).
///
/// Every flattened segment is resampled (at least 64 samples per contour length, refined where the
/// width deviates from linear by more than `tolerance`) and each sample step becomes a trapezoid
/// offset by +-w/2 along the segment normal at each end. Joins sit only at the input vertices, with
/// half-width = half the larger of the two adjacent segment widths there (they differ only at a
/// closed contour's seam), using the same miter / round / bevel shapes as the constant overload.
/// Caps use the width at u = 0 / u = 1; a cap whose width is 0 degenerates to a point (no output).
/// Single-point contours draw the point cap of width(0). Tapering closed contours is the caller's
/// business (the Painter passes a width function without taper for them, 31 §31.4.7).
void strokeToFill(std::span<const Vec2> points, std::span<const uint32_t> contourStarts,
                  std::span<const uint8_t> closed, WidthFn width, LineCap cap, LineJoin join,
                  float miterLimit, float tolerance, std::vector<Vec2>& outPoints,
                  std::vector<uint32_t>& outContourStarts);

/// Splits every input contour into open dash segments according to `dashes` (on, off, on, off, ...
/// lengths; an odd count is repeated once to make it even) and `phase` (appended to outPoints /
/// outContourStarts / outClosed).
///
/// A closed input contour is treated as an open contour starting at its first point (the dash
/// pattern does not wrap across the seam; accepted as a first-version limitation).
/// If `dashes` is empty, or every effective dash length is <= 0 in total, the contour is copied
/// through unchanged (equivalent to no dashing). Negative dash lengths are clamped to 0. `phase` may
/// be negative and is taken modulo the pattern's total length.
/// A trailing "on" run that starts exactly at the contour's end still emits a single-point contour
/// (see test_geom_dash.cpp).
void applyDash(std::span<const Vec2> points, std::span<const uint32_t> contourStarts,
               std::span<const uint8_t> closed, std::span<const float> dashes, float phase,
               std::vector<Vec2>& outPoints, std::vector<uint32_t>& outContourStarts,
               std::vector<uint8_t>& outClosed);

/// Trim path (§3.8): operates on raw (unflattened) verbs/points, outputs verbs/points.
///
/// `start`/`end` are arc-length fractions in [0, 1]; `offset` is an arbitrary fraction (turns).
/// The effective interval is `[start+offset, end+offset] mod 1`. `start == end` produces empty
/// output; `(end - start) >= 1` copies the input through unchanged. All subpaths are treated as one
/// continuous path for total arc length purposes (a Close verb counts as a line back to its
/// subpath's start point). Arc length is measured by approximating each Cubic with 16 line segments;
/// splitting a Cubic uses de Casteljau on a parameter `t` found by interpolating in that 16-segment
/// table (arc-length fraction -> t). When the interval wraps past 1, two fragments are produced,
/// each starting with its own Move verb.
void trimPath(std::span<const PathVerb> verbs, std::span<const float> points, float start, float end,
              float offset, std::vector<PathVerb>& outVerbs, std::vector<float>& outPoints);

/// Signed area of a polyline treated as a closed polygon. Coordinate system is y-down; a clockwise
/// polygon has positive area.
float polygonSignedArea(std::span<const Vec2> pts);

/// Total length of a polyline; if `closed`, includes the segment from the last point back to the
/// first.
float polylineLength(std::span<const Vec2> pts, bool closed);

/// T-218 (docs/design/31-vector-authoring.md §31.4.1): public bezier helpers, shared by trimPath
/// and the editor's vector tools.
namespace geom {

/// De Casteljau split of cubic (p0,p1,p2,p3) at parameter `t`: `left` = [0,t], `right` = [t,1].
void splitCubic(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, float t, Vec2 left[4], Vec2 right[4]);

/// The sub-curve of (p0,p1,p2,p3) for the bezier-parameter range [tA, tB] (0 <= tA <= tB <= 1).
void subCubic(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, float tA, float tB, Vec2 out[4]);

/// 16-segment linear approximation table of a cubic: sample points, cumulative chord lengths and
/// the total (approximate arc) length.
struct CubicTable {
    static constexpr int kSteps = 16;  ///< Number of line segments.
    Vec2 pts[kSteps + 1];              ///< Samples at t = i / kSteps.
    float cum[kSteps + 1];             ///< Cumulative length up to sample i.
    float length = 0.0f;               ///< Total length (== cum[kSteps]).
};

/// Builds the 16-segment table of cubic (p0,p1,p2,p3).
CubicTable buildCubicTable(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3);

/// Maps an arc-length fraction (0..1 of the table's length) to a bezier parameter t, interpolating
/// in the 16-segment table.
float lengthFractionToT(const CubicTable& table, float lengthFraction);

/// Exact bounding box of a path (verbs/points in the §3.4 layout): every on-curve point plus the
/// extrema of each cubic (roots of its derivative), not the control-point hull. An empty path
/// returns an all-zero Rect.
Rect pathBounds(std::span<const PathVerb> verbs, std::span<const float> points);

/// Whether `p` is inside the filled path under `rule`. The path is flattened with a 0.25 px
/// tolerance (kFlattenTolerance); every contour is treated as closed, as filling does.
bool pathContains(std::span<const PathVerb> verbs, std::span<const float> points, Vec2 p, FillRule rule);

}  // namespace geom

}  // namespace xt
