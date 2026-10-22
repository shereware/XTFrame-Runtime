#include "file_open_utf8.h"

#include <cstring>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

/// See file_open_utf8.h (Docs/tasks/T-261e.md).

namespace xt {

namespace {

#if defined(_WIN32)
/// `text` (UTF-8) as UTF-16; false when it is not valid UTF-8.
bool widen(const char* text, std::wstring& out) {
    const int length = static_cast<int>(std::strlen(text));
    if (length == 0) {
        out.clear();
        return true;
    }
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, length, nullptr, 0);
    if (n <= 0) {
        return false;
    }
    out.assign(static_cast<size_t>(n), L'\0');
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, length, out.data(), n) == n;
}
#endif

}  // namespace

std::FILE* openFileUtf8(const char* utf8Path, const char* mode) {
    if (utf8Path == nullptr || mode == nullptr) {
        return nullptr;
    }
    std::FILE* file = nullptr;
#if defined(_WIN32)
    // T-261f: a path that is not valid UTF-8 is a missing file, never one in the ANSI code page.
    std::wstring widePath;
    if (!widen(utf8Path, widePath)) {
        return nullptr;
    }
    const std::wstring wideMode(mode, mode + std::strlen(mode));  // modes are ASCII
    if (_wfopen_s(&file, widePath.c_str(), wideMode.c_str()) != 0) {
        file = nullptr;
    }
#elif defined(_MSC_VER)
    if (fopen_s(&file, utf8Path, mode) != 0) {
        file = nullptr;
    }
#else
    file = std::fopen(utf8Path, mode);
#endif
    return file;
}

}  // namespace xt
