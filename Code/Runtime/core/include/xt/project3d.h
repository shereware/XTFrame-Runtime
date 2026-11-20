#pragma once

#include <span>
#include <vector>

#include "xt/node.h"
#include "xt/types.h"

/// CPU projection utilities shared by the SW backend, the GL backend (path/glyph/clip geometry)
/// and the command-list simplifier (19.8's flatten3D): mapping-stack point/polyline projection,
/// the SW flattening-tolerance scale estimate and backface determinant. See
/// Docs/design/19-3d-space.md §19.6, §19.7.

namespace xt::project3d {

/// One level of the "plane to screen" mapping stack (19.7). `hasProjection == false` means this
/// level contributes no projection (screen = outer(xf * p), where `outer` is the remaining stack).
struct Mapping {
    /// False when this level is a no-op (see the struct comment above).
    bool hasProjection = false;
    /// The root's camera/CSS-perspective view-projection matrix (19.4 step 5).
    Mat4 viewProj = mat4Identity();
    /// The root's own `world` (2D), used to map the projected point back into the outer mapping's
    /// space (§19.7's `rootXf`).
    Mat2x3 rootXf{};
    /// The current 3D layer's `world3` inside this projection (identity at the root plane).
    Mat4 planeWorld3 = mat4Identity();
};

/// Projects `p` (in `xf`'s local coordinates) through `xf` and then `stack` (outermost first,
/// innermost/current last) to a screen point. `ok` is set to false when any level's homogeneous
/// divide has w <= 1e-6 (the geometry must be discarded by the caller); the returned point is then
/// unspecified.
Vec2 projectPoint(std::span<const Mapping> stack, const Mat2x3& xf, Vec2 p, bool& ok);

/// T-761 (Docs/design/19-3d-space.md §19.y.1): `p` (current plane coordinates, logical px) through
/// every level of `stack` as one homogeneous point (x, y, w), screen = (x / w, y / w), with no
/// divide between the levels: the result is H * (p, 1) for the whole chain's homography H (the
/// planeHomography() product), so a GPU interpolating vertex attributes over it is perspective
/// correct whichever levels carry the perspective. Computed level by level in double (no composed
/// float matrix, so large translations do not cancel). `ok` is false under projectPoint()'s rule
/// (some level's own homogeneous term -- its w relative to the w it was given -- is not above
/// 1e-6) or when a value is not finite. x / w, y / w equals projectPoint(stack, identity, p) up to
/// rounding. Does not allocate.
Vec3 projectPointHomogeneous(std::span<const Mapping> stack, Vec2 p, bool& ok);

/// Projects an already-flattened path (verbs must be only Move/Line/Close) point by point.
/// Returns false (leaving outVerbs/outPoints empty) if any point's projection fails (w <= 1e-6);
/// the caller then discards the whole path, per 19.7's SW rule.
bool projectPolyline(std::span<const Mapping> stack, const Mat2x3& xf, std::span<const PathVerb> verbs,
                     std::span<const float> points, std::vector<PathVerb>& outVerbs,
                     std::vector<float>& outPoints);

/// 19.7's SW flattening scale estimate: the largest ratio between an edge of `bounds`'s corner
/// quad after projection and before, floored at 1. Corners whose projection fails are ignored;
/// returns 1 if every corner fails.
float projectedScaleMax(std::span<const Mapping> stack, const Mat2x3& xf, Rect bounds);

/// 19.6's backface test: true when the projected mapping's 2x2 Jacobian at `anchor` (central
/// difference, +/-1 px) has a negative determinant. Returns false (not backface) if any of the
/// four sample projections fails.
bool isBackface(std::span<const Mapping> stack, const Mat2x3& xf, Vec2 anchor);

/// Docs/design/18-effects.md §18.5's "node plane -> screen" transform for a 3D layer or one of its
/// 2D descendants: a Mat2x3 whose linear part is the projected mapping's 2x2 Jacobian at `anchor`
/// (central difference, +/-0.5 px) and whose translation is `anchor`'s own projected position.
/// Returns identity if any of the five sample projections (center + 4 offsets) fails.
Mat2x3 jacobianAt(std::span<const Mapping> stack, const Mat2x3& xf, Vec2 anchor);

/// Docs/design/10-backends.md §10.15 item 1 (T-700): the largest screen deviation, in device px,
/// that still lets a plane count as affine.
inline constexpr float kAffinePlaneTolerancePx = 1.0f / 64.0f;

/// §10.15 item 1 (T-700): the current plane's mapping -- the whole `stack` (outermost first) with
/// every level's `viewProj * planeWorld3` restricted to its plane's z = 0 and followed by its
/// `rootXf` -- as one 3x3 homography from plane coordinates (`xf` space, logical px) to the screen
/// (logical px), column vectors (x, y, 1). Returns false (`out` unspecified) when a level's own
/// homogeneous term at its plane origin is not above 1e-6 or a value is not finite; such a plane
/// is never treated as affine. Does not allocate.
bool planeHomography(std::span<const Mapping> stack, Mat3& out);

/// §10.15 item 1 (T-700): true when the homography `h` (planeHomography's) is affine over
/// `viewport` (screen rectangle, the units of `h`'s output): its homogeneous term w is positive and,
/// for every screen point of `viewport`, the variation of w across the plane moves the projected
/// point by less than `tolerance` (same units) from where the constant-w affine map puts it. An
/// exactly constant w (no x, y terms: the root plane, translateZ / scale / rotateZ layers) is
/// affine whatever `viewport` is. On true, `out` is `h` divided by its constant w (the folded 2D
/// transform). Does not allocate.
bool affinePlane(const Mat3& h, Rect viewport, float tolerance, Mat2x3& out);

/// T-716 (§10.16 W3): the inputs of §10.15's affine test for the planes of one CommandList. The Painter
/// (which decides whether a box shadow on the plane becomes a DrawBoxShadow) and the consumers of the
/// list (xt_gpu's frame builder, xt::sw, the simplifier: they fold an affine plane and project the
/// others) all take them from planeTest(), so they take the same decision for the same plane.
struct PlaneTest {
    /// The screen rectangle the plane must be affine over (logical px).
    Rect viewport{};
    /// The largest deviation allowed inside `viewport` (logical px).
    float tolerance = kAffinePlaneTolerancePx;
};

/// T-716: the viewport of a caller that knows neither the list's viewport nor a target size. Over it
/// only a plane whose w is constant (the root plane, translateZ / scale / rotateZ layers) is affine,
/// and such a plane is affine over every other viewport and at every tolerance too.
inline constexpr Rect kUnboundedPlaneViewport{-1e6f, -1e6f, 2e6f, 2e6f};

/// T-716: the test of a list whose CommandList::viewport is `listViewport` and whose pixel ratio is
/// `dpr` (<= 0 counts as 1): `listViewport` when it is not empty, else `fallback` (the target's
/// rectangle in logical px, or kUnboundedPlaneViewport); tolerance kAffinePlaneTolerancePx / dpr.
PlaneTest planeTest(Rect listViewport, Rect fallback, float dpr);

/// T-716: planeHomography() and affinePlane() in one call: true when the current plane of `stack` is
/// affine under `test`, with `fold` the folded 2D transform (plane -> logical screen px). `fold` is
/// unspecified on false. Does not allocate.
bool stackPlaneAffine(std::span<const Mapping> stack, const PlaneTest& test, Mat2x3& fold);

}  // namespace xt::project3d
