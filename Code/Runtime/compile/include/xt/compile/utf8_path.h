#pragma once

#include <filesystem>
#include <string>
#include <string_view>

/// UTF-8 text <-> `std::filesystem::path` (Docs/tasks/T-237.md item 7). Every path the bundle
/// library, the compiler, the packer and the editor keep in a `std::string` is UTF-8; on Windows,
/// `std::filesystem::path(std::string)` and `path::string()` go through the ANSI code page instead,
/// which mangles (or throws on) a project directory such as `測試專案/`. These two convert through
/// `char8_t`, which is UTF-8 on every platform.

namespace xt::compile {

/// The path a UTF-8 string names.
inline std::filesystem::path pathFromUtf8(std::string_view utf8) {
    return std::filesystem::path(
        std::u8string_view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

/// `path` as UTF-8 (native separators, as `path::string()`).
inline std::string pathToUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

/// `path` as UTF-8 with '/' separators (as `path::generic_string()`).
inline std::string genericPathToUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

}  // namespace xt::compile
