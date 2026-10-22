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

}  // namespace xt::project3d
