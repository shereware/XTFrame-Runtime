#pragma once

#include <cstddef>
#include <cstdint>

#include "xt/asset_resolver.h"

/// T-199 (docs/design/26-layers-symbols-library.md §26.3.4, 23 §23.8): software decoding of a
/// DDS file (BC1 / BC3 / BC7 blocks or plain RGBA8) into straight-alpha RGBA8, for the software
/// backend and every host that decodes through xt::sw::decodeImageBytes. Uses the vendored
/// rgbcx / bc7decomp decoders (third_party/bc7enc).

namespace xt::sw {

/// Decodes level 0 of `bytes` (a whole DDS file). False when the header is not one of
/// xt::DdsFormat or the data is truncated.
bool decodeDdsBytes(const uint8_t* bytes, size_t size, ImageData& out);

}  // namespace xt::sw
