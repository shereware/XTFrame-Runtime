#pragma once

#include <ctime>
#include <string>

/// Wall-clock stamps written into project files (T-414 C2d; bundle/src/pack.cpp and asset_import.cpp
/// each had a copy).

namespace xt::util {

/// The current time as ISO-8601 UTC, e.g. "2026-09-22T10:00:00Z"; "" when the clock cannot be read.
inline std::string isoUtcNow() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#if defined(_MSC_VER)
    if (gmtime_s(&utc, &now) != 0) {
        return std::string();
    }
#else
    if (gmtime_r(&now, &utc) == nullptr) {
        return std::string();
    }
#endif
    char buffer[32] = {};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
        return std::string();
    }
    return std::string(buffer);
}

}  // namespace xt::util
