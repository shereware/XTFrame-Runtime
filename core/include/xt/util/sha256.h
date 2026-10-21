#pragma once

#include <string>
#include <string_view>

/// SHA-256 (FIPS 180-4), written from the public specification. T-414 C2d: one copy for the
/// design-system / asset `.meta` content hash (bundle/src/design_system.cpp) and the importer's
/// `.meta` check (import/src/xt_writer.cpp), which each had their own.

namespace xt::util {

/// SHA-256 of `bytes` as 64 lowercase hex digits.
std::string sha256Hex(std::string_view bytes);

}  // namespace xt::util
