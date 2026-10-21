#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

/// Block compression for pack() (T-199, docs/design/26-layers-symbols-library.md §26.3.4,
/// 23 §23.8, 12 §12.7 `--compress`): RGBA8 -> BC1 / BC3 / BC7 blocks (vendored bc7enc / rgbcx,
/// third_party/bc7enc) written as a DDS file the runtime reads back through xt::parseDdsHeader.
/// Internal to xt_bundle.

namespace xt::bundle {

/// The `--compress` choices.
enum class BcFormat : uint8_t { None = 0, Bc1 = 1, Bc3 = 2, Bc7 = 3 };

/// "none" | "bc1" | "bc3" | "bc7" (case-insensitive) -> format; false for anything else.
bool parseBcFormat(std::string_view name, BcFormat& out);
/// The manifest's `format` string: "bc1", "bc3", "bc7" ("png" is not this function's business).
const char* bcFormatName(BcFormat format);

/// True when any pixel of `rgba` (`w` x `h`, 4 bytes per pixel) has alpha < 255.
bool imageHasAlpha(const uint8_t* rgba, int w, int h);

/// Encodes `rgba` (straight alpha, row 0 = top) as a DDS file in `format` (never None). With
/// `mipmaps`, the full chain down to 1 x 1 (box filtered) follows level 0. Returns the file bytes;
/// empty on a degenerate size.
std::vector<uint8_t> encodeDds(const uint8_t* rgba, int w, int h, BcFormat format, bool mipmaps);

}  // namespace xt::bundle
