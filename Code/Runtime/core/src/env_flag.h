#pragma once

#include <cstdlib>

/// T-755: the one-character environment switches of the core's verify modes (XT_VERIFY_VISUAL_GENERATION,
/// XT_STATIC_SKIP, XT_PARTIAL_LAYOUT), read when a Context or a LayoutEngine is constructed. Moved out
/// of context_visual.cpp (T-747) so the layout engine reads them the same way.

namespace xt {

/// The environment variable `name`'s value when it is exactly one character, '?' when it is longer,
/// '\0' when it is not set (or empty).
inline char envFlag(const char* name) {
#ifdef _MSC_VER
    char* value = nullptr;
    size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return '\0';
    }
    const char first = value[0] != '\0' && value[1] == '\0' ? value[0] : (value[0] == '\0' ? '\0' : '?');
    std::free(value);
    return first;
#else
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return '\0';
    }
    return value[1] == '\0' ? value[0] : '?';
#endif
}

}  // namespace xt
