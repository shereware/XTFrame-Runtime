#include "xt/advance_plan.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

/// T-879: see xt/advance_plan.h (moved from Code/Tools/view/main.cpp, T-817).

namespace xt {

AdvancePlan planAdvance(float seconds, float frameDt) {
    AdvancePlan plan;
    if (!(seconds > 0.0f)) {
        return plan;
    }
    // A float --advance (0.1 -> 0.100000001) is a whole number of frames within 1/1000 of a frame.
    const double steps = std::min(static_cast<double>(seconds) / static_cast<double>(frameDt),
                                  static_cast<double>(UINT32_MAX / 2u));
    const double nearest = std::floor(steps + 0.5);
    if (std::fabs(steps - nearest) < 1e-3) {
        if (nearest >= 1.0) {
            plan.firstFrame = static_cast<uint32_t>(nearest);
            plan.firstDt = frameDt;
        }
        return plan;
    }
    const double whole = std::floor(steps);
    plan.firstFrame = static_cast<uint32_t>(whole) + 1u;
    plan.firstDt = static_cast<float>((steps - whole) * static_cast<double>(frameDt));
    return plan;
}

}  // namespace xt
