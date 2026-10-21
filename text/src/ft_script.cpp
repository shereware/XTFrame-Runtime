#include <hb.h>

#include <cstdint>

#include "vo_table.h"

/// Script-per-codepoint (HarfBuzz's own Unicode data, `hb_unicode_script`) and the UAX#50
/// Vertical_Orientation lookup used for `TextOrientation::Mixed`. See docs/design/07-text.md
/// §7.3 steps 2 and 5.

namespace xt::text {

// hb_unicode_script() classifies a single codepoint; Common/Inherited carry-forward across a run
// (per docs/design/07-text.md §7.3 step 2, "Common/Inherited 繼承前一個 script") is done by the
// caller (ft_text_shaper.cpp), which is the only place that has the "previous script" state.
hb_script_t scriptForCodepoint(uint32_t codepoint) {
    hb_unicode_funcs_t* ufuncs = hb_unicode_funcs_get_default();
    return hb_unicode_script(ufuncs, codepoint);
}

VerticalOrientation verticalOrientationFor(uint32_t codepoint) { return verticalOrientation(codepoint); }

}  // namespace xt::text
