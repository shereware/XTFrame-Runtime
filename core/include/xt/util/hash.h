#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

/// Small content-hash helpers shared by the packer (bundle/src/pack.cpp) and the importer
/// (import/src/asset_store.cpp, reimport.cpp) -- T-414 C2d; each used to keep its own copy.

namespace xt::util {

/// FNV-1a, 64-bit: the content hash both `assets/<hash16>.<ext>` and `uis[].fileHash` use.
inline uint64_t fnv1a64(std::string_view bytes) {
    uint64_t hash = 14695981039346656037ULL;
    for (const char byte : bytes) {
        hash ^= static_cast<uint64_t>(static_cast<unsigned char>(byte));
        hash *= 1099511628211ULL;
    }
    return hash;
}

/// `hash` as exactly 16 lowercase hex digits.
inline std::string hex16(uint64_t hash) {
    char buffer[17] = {};
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
    return std::string(buffer);
}

}  // namespace xt::util
