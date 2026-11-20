#pragma once

/// T-754 (Docs/design/10-backends.md §10.20 rule 7): the safe area a command-line tool hands to
/// `Context::setDevice()` for a Bundle's manifest `spec` -- one rule for xtframe-view and
/// xtframe-gpu-probe, so the two measure the same layout. `spec.safeArea.console` on a console
/// platform, `spec.safeArea.default` otherwise, the same fraction on all four sides. Header-only
/// (Code/Tools/Common has no library); needs xt_core and xt_bundle.

#include <charconv>
#include <string_view>
#include <system_error>

#include "xt/bundle/bundle.h"
#include "xt/layout.h"

namespace xt::tools {

/// The safe-area insets (fractions of the viewport) `spec` asks for on `platform`.
inline xt::Insets specSafeArea(const xt::bundle::BundleSpec& spec, xt::DeviceKind platform) {
    const float safe = platform == xt::DeviceKind::Console ? spec.safeAreaConsole : spec.safeAreaDefault;
    return xt::Insets{safe, safe, safe, safe};
}

/// `--safe <ratio>`: one fraction of the viewport in [0, 0.5] for all four sides. False (and `out`
/// untouched) for anything else.
inline bool parseSafeRatio(std::string_view text, xt::Insets& out) {
    float value = 0.0f;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const std::from_chars_result r = std::from_chars(first, last, value);
    if (text.empty() || r.ec != std::errc() || r.ptr != last || !(value >= 0.0f) || value > 0.5f) {
        return false;
    }
    out = xt::Insets{value, value, value, value};
    return true;
}

}  // namespace xt::tools
