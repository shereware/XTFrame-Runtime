#pragma once

#include <string>
#include <string_view>

/// Colour math for the design system (Docs/design/35-design-system.md §35.3.3, §35.10;
/// Docs/tasks/T-305.md): hex parsing, sRGB -> CIELAB (D65) and the CIEDE2000 colour difference.
/// T-309 adds OKLab / OKLCH (deriveScale), the WCAG 2.x relative luminance and hex formatting; the
/// public face of these functions is xt/bundle/design_color.h. Internal to xt_bundle.

namespace xt::bundle::detail {

/// CIELAB coordinates (D65 white).
struct Lab {
    double l = 0.0;
    double a = 0.0;
    double b = 0.0;
};

/// Parses `#rgb`, `#rgba`, `#rrggbb` or `#rrggbbaa` (case-insensitive) into components 0..1.
/// False for anything else; `rgba` is then untouched.
bool parseHexColor(std::string_view text, float rgba[4]);

/// sRGB components 0..1 (gamma-encoded) -> CIELAB, D65 white.
Lab srgbToLab(double r, double g, double b);

/// CIEDE2000 (Sharma, Wu, Dalal 2005), kL = kC = kH = 1.
double deltaE2000(const Lab& x, const Lab& y);

/// ΔE2000 between two hex colours (alpha ignored). False when either does not parse.
bool hexDeltaE2000(std::string_view a, std::string_view b, double* out);

/// T-309: OKLab coordinates (Björn Ottosson 2020); L 0..1.
struct Oklab {
    double l = 0.0;
    double a = 0.0;
    double b = 0.0;
};

/// T-309: sRGB components 0..1 (gamma-encoded) -> OKLab.
Oklab srgbToOklab(double r, double g, double b);

/// T-309: OKLab -> sRGB components (gamma-encoded), not clamped: a component outside 0..1 means
/// the colour is outside the sRGB gamut.
void oklabToSrgb(const Oklab& lab, double rgb[3]);

/// T-309: the WCAG 2.x relative luminance of sRGB components 0..1 (gamma-encoded).
double relativeLuminance(double r, double g, double b);

/// T-309: `#rrggbb` (lowercase) of sRGB components 0..1, each clamped and rounded.
std::string formatHex(double r, double g, double b);

}  // namespace xt::bundle::detail
