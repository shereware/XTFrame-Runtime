#include <cstdint>
#include <string>
#include <string_view>

#include "xt/bundle/bundle.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

/// xt::fileStatusUtf8 (docs/tasks/T-263e.md ruling B): the file-status function a host injects
/// into FileAssetResolver. It lives here, not in xt_core, which asks the file system nothing by
/// itself (T-261e's statFileUtf8, moved). A file of its own so <windows.h> stays out of bundle.cpp.

namespace xt {

bool fileStatusUtf8(std::string_view utf8Path, uint64_t& size, uint64_t& modifiedTicks) {
#if defined(_WIN32)
    if (utf8Path.empty()) {
        return false;
    }
    // T-261f: a path that is not valid UTF-8 is a missing file, as core's openFileUtf8() takes it.
    const int length = static_cast<int>(utf8Path.size());
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Path.data(), length, nullptr, 0);
    if (n <= 0) {
        return false;
    }
    std::wstring widePath(static_cast<size_t>(n), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Path.data(), length, widePath.data(), n) !=
        n) {
        return false;
    }
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(widePath.c_str(), GetFileExInfoStandard, &data) == 0 ||
        (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }
    size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    modifiedTicks = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                    data.ftLastWriteTime.dwLowDateTime;
    return true;
#else
    const std::string path(utf8Path);
    struct stat info{};
    if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
        return false;
    }
    size = static_cast<uint64_t>(info.st_size);
    modifiedTicks = static_cast<uint64_t>(info.st_mtime);
    return true;
#endif
}

}  // namespace xt
