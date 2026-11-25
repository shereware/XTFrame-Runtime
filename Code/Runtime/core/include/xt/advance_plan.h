#pragma once

#include <cstdint>

/// T-817 / T-879: how a run reaches a clock value one frame at a time -- shared by `xtframe-view
/// --advance` and the Editor Service's `view.capture {clock}` (Docs/design/59-visual-feedback.md
/// §59.3: "場景從 0 起以 1/60 推進到該值（與 xtframe-view --advance 相同，可重現）"). Moved here from
/// Code/Tools/view/main.cpp by T-879 unchanged.

namespace xt {

/// The first painted frame and its dt. A plain run's frame k shows time k/60 (frame 0 has dt 0, every
/// later one `frameDt`), so `seconds` = N frames of `frameDt` plus a remainder r: with no remainder the
/// frames 0..N-1 run unpainted and frame N (dt `frameDt`) is the first painted one; with one, the
/// frames 0..N run unpainted and frame N+1 takes the remainder as its dt. `seconds` 0 = none.
struct AdvancePlan {
    uint32_t firstFrame = 0;  ///< Frames 0..firstFrame-1 run without painting.
    float firstDt = 0.0f;     ///< The dt of the first painted frame.
};

/// The plan for `seconds` at `frameDt` (see AdvancePlan). A float `seconds` within 1/1000 of a frame of
/// a whole number of frames (0.1 -> 0.100000001) counts as that whole number.
AdvancePlan planAdvance(float seconds, float frameDt);

}  // namespace xt
