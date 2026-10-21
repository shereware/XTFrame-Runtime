#pragma once

#include <vector>

#include "xt/node.h"
#include "xt/types.h"

/// `clip-path` basic-shape geometry shared by the Painter (the PushClipPath path), the hit test
/// (08 §8.2's point-in-shape test) and the paint-bounds / damage code (the shape's bounding box).
/// See docs/design/05-style-and-css.md §5.9.8 (T-332). Every function resolves the shape's
/// lengths against `box`, the element's border box in its own local space (so normally
/// `{0, 0, width, height}`): a `%` of an x-ish parameter is of `box.w`, of a y-ish one of
/// `box.h`, a circle radius `%` is of the CSS reference length sqrt((w^2 + h^2) / 2), and the
/// `closest-side` / `farthest-side` keywords are taken from the center's distance to the box's
/// edges, as CSS Shapes 1 defines them.

namespace xt::clip_shape {

/// The resolved inset: the inset rectangle and its 8 corner radii (x, y per corner, in the
/// order top-left, top-right, bottom-right, bottom-left), already clamped so no edge's adjacent
/// radii exceed its length.
struct ResolvedInset {
    Rect rect;
    float radii[8];
};

/// A resolved circle or ellipse: center and the two half-axes (equal for a circle).
struct ResolvedEllipse {
    Vec2 center;
    float rx = 0;
    float ry = 0;
};

/// Resolves an Inset shape. False for any other kind.
bool resolveInset(const ElementClipShape& shape, Rect box, ResolvedInset& out);
/// Resolves a Circle or Ellipse shape. False for any other kind.
bool resolveEllipse(const ElementClipShape& shape, Rect box, ResolvedEllipse& out);
/// Resolves a Polygon shape's vertices. False for any other kind.
bool resolvePolygon(const ElementClipShape& shape, Rect box, std::vector<Vec2>& out);

/// Appends the shape as one closed subpath (in `box`'s space) to `verbs` / `points` and sets
/// `rule` (NonZero for inset / circle / ellipse, EvenOdd for a polygon). Returns false when the
/// shape is None, or when it is empty (a zero-area inset, a zero radius, fewer than 3 vertices):
/// the caller then clips everything away, since an empty shape shows nothing.
bool appendPath(const ElementClipShape& shape, Rect box, std::vector<PathVerb>& verbs,
                std::vector<float>& points, FillRule& rule);

/// The shape's bounding box in `box`'s space; an empty Rect (w = h = 0) for None or an empty
/// shape.
Rect bounds(const ElementClipShape& shape, Rect box);

/// 08 §8.2 (T-332): whether `p` (in `box`'s space) lies inside the shape. A None shape contains
/// every point (no clip); an empty shape contains none. Polygons use the even-odd rule.
bool contains(const ElementClipShape& shape, Rect box, Vec2 p);

}  // namespace xt::clip_shape
