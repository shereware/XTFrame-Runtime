#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xt/types.h"

/// The design system's colour tools (docs/design/35-design-system.md §35.3.3, §35.7.1
/// `design.extractPalette` / `design.deriveScale` / `design.contrast`; docs/tasks/T-309.md item 2).
/// Deterministic: the same input always gives the same output. They only produce knowledge for the
/// palette; nothing here touches a stylesheet.

namespace xt::bundle::design {

/// One dominant colour of a picture: `#rrggbb` and the fraction of the counted pixels it covers.
struct ExtractedColor {
    std::string value;
    float share = 0.0f;
};

/// The dominant colours of an RGBA8 picture (`w * h * 4` bytes, row-major, straight alpha). The
/// picture is first sampled down so its long side is at most 256 pixels (nearest sample at each
/// cell centre); pixels with alpha < 128 are not counted. k-means in CIELAB (k-means++ seeding from
/// a fixed seed, at most 20 iterations); each colour is the mean sRGB of its cluster. At most
/// `count` colours (1..32; fewer when the picture has fewer distinct colours), sorted by share
/// (largest first), ties by value; shares sum to 1. Empty for a null / empty picture or count < 1.
std::vector<ExtractedColor> extractPalette(const uint8_t* rgba, int w, int h, int count = 8);

/// Decodes the image file at `path` (UTF-8; PNG / JPEG / ... through sw::decodeImageBytes) and runs
/// extractPalette() on it. kNotFound when it cannot be read, kInvalidArgument when it does not
/// decode (`*error` says which; may be null).
Status extractPaletteFromFile(std::string_view path, int count, std::vector<ExtractedColor>& out,
                              std::string* error = nullptr);

/// A lightness scale of `steps` colours (1..64) through `hex`'s hue and chroma: OKLCH lightness
/// evenly spaced from 0.97 (first, lightest) to 0.20 (last, darkest); a colour outside the sRGB gamut
/// has its chroma reduced (hue and lightness kept) until it fits. `#rrggbb` each. Only the
/// `oklch-lightness` method exists; an unknown method, an unparsable colour or steps outside 1..64
/// give an empty list. steps == 1 gives the colour itself.
std::vector<std::string> deriveScale(std::string_view hex, int steps,
                                     std::string_view method = "oklch-lightness");

/// The WCAG 2.x contrast ratio (1..21) of two hex colours (alpha ignored); 0 when either does not
/// parse.
float contrastRatio(std::string_view hexA, std::string_view hexB);

/// CIEDE2000 between two hex colours (alpha ignored); -1 when either does not parse.
float deltaE2000(std::string_view hexA, std::string_view hexB);

/// `text` (`#rgb`, `#rgba`, `#rrggbb`, `#rrggbbaa`, any case) as lowercase `#rrggbb`, or
/// `#rrggbbaa` when the alpha is not 255; empty when it does not parse.
std::string normalizeHex(std::string_view text);

}  // namespace xt::bundle::design
