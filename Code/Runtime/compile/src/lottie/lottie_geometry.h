#pragma once

#include <cstdint>
#include <vector>

#include "lottie_model.h"
#include "lottie_shapes.h"
#include "xt/node.h"
#include "xt/types.h"

/// Geometry generation for Lottie shape items (rc/el/sr/sh -> cubic-bezier paths), the RoundCorners
/// ("rd") corner-cutting algorithm, and compile-time keyframe sampling used to expand an animated
/// geometry item into a PathData Track. See Docs/design/11-lottie-mapping.md §11.3.2,
/// Docs/tasks/T-15.md "幾何生成".

namespace xt::compile {

/// One generated path: `verbs`/`points` follow the same encoding as IrPathData's verb/point pools
/// (Docs/design/03-scene-and-properties.md §3.4): Move/Line consume 2 floats, Cubic 6, Close 0.
struct GeneratedPath {
    std::vector<uint8_t> verbs;
    std::vector<float> points;
};

inline constexpr float kBezierKappa = 0.5522847498f;

/// Rect ("rc"): center `p`, size `s`, corner radius `r` (already clamped to min(w,h)/2 by the
/// caller when needed -- see clampRectRadius). `radius <= 0` produces the unrounded 5-verb path
/// (Move + 3 Line + Close, starting top-right, clockwise); otherwise the rounded 10-verb path
/// (Move + 4x(Line + Cubic) + Close). `direction == 3` reverses vertex order. `forceRounded` keeps
/// the 10-verb topology even when `radius == 0` (degenerate, zero-length corner curves), used to
/// keep verb/point counts identical across an animated Rect's sampled keyframes when any frame has
/// a positive radius (Docs/tasks/T-15.md "動畫幾何").
GeneratedPath buildRectPath(Vec2 center, Vec2 size, float radius, int direction, bool forceRounded = false);

/// Clamps a rect corner radius to `min(|size.x|, |size.y|) / 2`, per Docs/tasks/T-15.md.
float clampRectRadius(Vec2 size, float radius);

/// Ellipse ("el"): center `p`, size `s` (full width/height). 4 cubics, starting at the top
/// `(cx, cy - ry)`, clockwise. `direction == 3` reverses vertex order.
GeneratedPath buildEllipsePath(Vec2 center, Vec2 size, int direction);

/// Star / polygon ("sr"): `numPoints` point count, `innerRadius`/`outerRadius`, `innerRoundness`/
/// `outerRoundness` (0..100), `rotationDeg` (Lottie "r"), `center` (Lottie "p"). `starType == 2`
/// builds a regular polygon (outerRadius only, no inner vertices). `direction == 3` reverses vertex
/// order. See Docs/tasks/T-15.md "Star / Polygon". T-258b (D3): `numPoints` is clamped to
/// [3, modifiers::kMaxGeneratedPoints] (NaN = 3); the caller warns.
GeneratedPath buildStarPath(float numPoints, float innerRadius, float outerRadius, float innerRoundness,
                            float outerRoundness, float rotationDeg, Vec2 center, int starType,
                            int direction);

/// Path ("sh"): `v`/`i`/`o` (`in`/`out`, tangents relative to `v`) -> Move + Cubic* (+ Close if
/// `path.closed`). `direction == 3` reverses vertex order.
GeneratedPath buildPathFromLottiePath(const LottiePath& path, int direction);

/// Applies lottie-web's per-vertex corner-cutting RoundCorners algorithm to a closed LottiePath:
/// every vertex is replaced by two vertices (cut towards the previous/next vertex by
/// `min(radius, half the adjacent edge length)`), connected by a rounding Cubic; the point count of
/// the result is exactly `2x` the source's. Only meaningful for polygonal (straight-edge) paths;
/// existing curvature (non-zero `in`/`out`) on the source is discarded at the cut vertices (not
/// covered by the spec -- conservative: corners are always treated as straight-line joints).
LottiePath applyRoundCornersToPath(const LottiePath& src, float radius);

/// Samples an AnimatedValue<T> at a raw Lottie `frame` using linear+cubic-bezier interpolation
/// (component 0 easing only, matching Docs/design/04-timeline.md §4.3), evaluated purely at compile
/// time (not tied to any IR Easing/Track). Used to expand animated rc/el/sr geometry into per-frame
/// PathData keyframes. Spatial (Vec2) tangents are not applied here (not needed for pure geometry
/// sampling; see Docs/tasks/T-15.md).
template <class T>
T sampleAnimated(const AnimatedValue<T>& av, float frame);

}  // namespace xt::compile
