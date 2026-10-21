#pragma once

#include <cstddef>
#include <cstdint>

/// IEEE 802.3 CRC-32 (poly 0xEDB88320, init 0xFFFFFFFF, result inverted). Table-based.
/// See docs/tasks/T-03.md.

namespace xt {

uint32_t crc32(const uint8_t* data, size_t size);

}  // namespace xt
