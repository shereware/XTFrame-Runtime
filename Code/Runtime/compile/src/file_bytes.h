#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <system_error>
#include <vector>

/// Private: a whole file read into memory, shared by the compiler's sources (compile_document.cpp,
/// compile_lottie.cpp, style_emit.cpp, lottie/lottie_assets.cpp each had a copy; T-414 C2d). It
/// lives in compile, not Code/Runtime/core/include/xt/util, because core never includes <filesystem> (14
/// §14.10).

namespace xt::compile::detail {

/// Reads `path` into `out` (resized to the file size). Returns false when the file cannot be opened
/// or read.
/// Only regular files are read: on POSIX an ifstream opens a directory successfully and tellg()
/// then reports a nonsense size (the fuzz xt target hit an allocation-size-too-big in T-442's full
/// gate; T-461). Sizes above kMaxFileBytes are refused for the same reason.
inline constexpr std::streamoff kMaxFileBytes = std::streamoff{1} << 30;

inline bool readFileBytes(const std::filesystem::path& path, std::vector<uint8_t>& out) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.good()) {
        return false;
    }
    in.seekg(0, std::ios::end);
    std::streamoff size = in.tellg();
    if (size < 0 || size > kMaxFileBytes) {
        return false;
    }
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0) {
        in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    }
    return static_cast<bool>(in) || in.eof();
}

}  // namespace xt::compile::detail
