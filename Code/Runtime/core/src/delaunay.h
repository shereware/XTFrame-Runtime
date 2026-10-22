#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "xt/types.h"

/// Delaunay triangulation of a point set (Docs/design/32-rigging-and-deformation.md §32.2.4,
/// T-243): the compiler triangulates an `<xt-mesh>`'s pins when `<xt-tris>` is omitted (T-247's
/// Pin tool will rerun it). Private to core; the compiler links xt_core and includes it directly.

namespace xt {

/// Triangulates `points` (Bowyer-Watson, O(n^2); meant for <= 256 pins) into `outIndices`, a
/// triangle list of indices into `points`, each triangle counter-clockwise in the mathematical
/// sense (positive (b - a) x (c - a); clockwise on a y-down screen). Duplicate points are merged:
/// only the first of a set of coincident points is referenced. Returns false (and leaves
/// `outIndices` empty) when fewer than 3 distinct points remain, when they are all collinear, or
/// when there are more than 65536 points.
bool triangulate(std::span<const Vec2> points, std::vector<uint16_t>& outIndices);

}  // namespace xt
