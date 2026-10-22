#pragma once

#include <unistd.h>  // getpid; fuzz targets only build under clang on Linux/WSL (Docs/tasks/T-39.md).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <utility>

/// Scratch files for the fuzz targets whose entry point reads from disk (a path or a directory),
/// see Docs/tasks/T-267b.md. One directory per target and process (libFuzzer runs one process'
/// inputs sequentially; -jobs/-workers processes each get their own pid-suffixed directory).

namespace xt::fuzz {

/// `<temp>/xt_fuzz_<name>_<pid>/`, created on first use. One directory per distinct `name`.
inline const std::filesystem::path& scratchDir(const std::string& name) {
    static std::map<std::string, std::filesystem::path> dirs;
    auto it = dirs.find(name);
    if (it == dirs.end()) {
        std::error_code ec;
        std::filesystem::path d =
            std::filesystem::temp_directory_path(ec) /
            ("xt_fuzz_" + name + "_" + std::to_string(static_cast<unsigned long>(getpid())));
        std::filesystem::create_directories(d, ec);
        it = dirs.emplace(name, std::move(d)).first;
    }
    return it->second;
}

/// Writes `size` bytes at `data` to `path` (truncating). False when the file cannot be written.
inline bool writeScratchFile(const std::filesystem::path& path, const uint8_t* data, size_t size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.good()) {
        return false;
    }
    if (size > 0) {
        out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
    return out.good();
}

/// writeScratchFile() for text.
inline bool writeScratchText(const std::filesystem::path& path, const std::string& text) {
    return writeScratchFile(path, reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

}  // namespace xt::fuzz
