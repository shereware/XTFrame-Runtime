#pragma once

#include <vector>

#include "xt/node.h"
#include "xt/types.h"

/// Rounded-rectangle path generation shared by Element background/border painting. See
/// Docs/tasks/T-27.md rules 2-3.

namespace xt {

/// Standard 4-segment cubic-bezier circular-arc approximation constant (matches
/// Code/Runtime/compile/src/lottie/lottie_geometry.h's kBezierKappa; core has no dependency on xt_compile so
/// this is a separate copy).
inline constexpr float kRoundedRectKappa = 0.5522847498f;

/// Clamps each corner radius to half of the shorter box dimension, then (per the CSS
/// border-radius overlap rule) uniformly scales all four radii down by the smallest factor needed
/// so that no edge's two adjacent radii sum exceeds that edge's length. `w`/`h` are the box's
/// border-box size; radii already <= 0 are left untouched by the scale step (clamped to 0 by the
/// first step). See Docs/tasks/T-27.md rule 2 ("半徑總和超過邊長，全部半徑等比縮小").
void clampRoundedRectRadii(float w, float h, float& tl, float& tr, float& br, float& bl);

/// Appends one closed rounded-rectangle subpath to `verbs`/`points`, in `box`'s local coordinate
/// system (i.e. callers wanting the border-box origin at world (0,0) pass `box.x == box.y == 0`).
/// Winding starts at the top edge (just after the top-left corner) and proceeds clockwise: top
/// edge -> TR corner -> right edge -> BR corner -> bottom edge -> BL corner -> left edge -> TL
/// corner -> Close. Each corner contributes exactly one Line (to the point where its arc begins)
/// followed by a Cubic (through the arc) when that corner's radius is > 0, or just the Line when
/// it is 0 (the "before" and "after" arc points then coincide with the corner itself). This means
/// an all-zero-radius box always emits Move + 4 Line + Close (6 verbs), and an all-rounded box
/// emits Move + 4x(Line + Cubic) + Close (10 verbs) -- see Docs/tasks/T-27.md's test table.
/// Radii are assumed already clamped (see clampRoundedRectRadii); this function does not re-clamp.
void appendRoundedRectPath(Rect box, float rTL, float rTR, float rBR, float rBL, std::vector<PathVerb>& verbs,
                           std::vector<float>& points);

}  // namespace xt
