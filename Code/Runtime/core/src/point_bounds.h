#pragma once

#include <algorithm>
#include <span>

#include "xt/types.h"

/// The axis-aligned box of a point set, shared by the clip shapes (clip_shape.cpp), the external
/// texture report (external_texture_use.cpp) and the GPU clip scan (Code/Runtime/gpu/src/
/// clip_bounds.cpp) -- T-800. Internal to xt_core (xt_gpu sees Code/Runtime/core/src privately).

namespace xt {

/// The AABB of `points` (not empty): seeded with the first point, then std::min / std::max per
/// point in order, so a NaN coordinate after the first is skipped and a NaN first one stays.
inline Rect pointBounds(std::span<const Vec2> points) {
    float x0 = points[0].x, y0 = points[0].y, x1 = x0, y1 = y0;
    for (const Vec2& p : points) {
        x0 = std::min(x0, p.x);
        y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x);
        y1 = std::max(y1, p.y);
    }
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

}  // namespace xt
