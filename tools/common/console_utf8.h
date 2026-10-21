#pragma once

/// T-261e: the command-line tools' text encoding on Windows, header-only so every tool (the runtime
/// ones included) and xtframe-editor's windowless modes can use it without a library.
///   - useUtf8Console(): the tools print UTF-8 (paths, diagnostics); a console left on its ANSI
///     code page (CP950, ...) would show those bytes as mojibake. T-261f: restored at exit
///     (restoreConsole()).
///   - wideToUtf8(): a `wmain` argument as UTF-8 text -- a plain conversion, never through
///     `std::filesystem::path`, since option values (`--name`, a URL) are not paths.
///   - Utf8Args (T-261f): every `wmain` argument as UTF-8, in the `argc` / `argv` shape the tools'
///     `run(int, char**)` takes.

#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace xt::tools {

namespace detail {
#if defined(_WIN32)
/// The output code page useUtf8Console() found and changed; 0 when there is nothing to restore.
inline UINT& savedConsoleOutputCp() {
    static UINT codePage = 0;
    return codePage;
}
#endif
}  // namespace detail

/// T-261f: puts the console's output code page back to the one useUtf8Console() changed (once;
/// later calls do nothing). Registered with std::atexit by useUtf8Console(); a tool that ends the
/// process some other way (`_exit`, `ExitProcess`, ...) calls it first. Elsewhere it does nothing.
inline void restoreConsole() {
#if defined(_WIN32)
    UINT& saved = detail::savedConsoleOutputCp();
    if (saved != 0) {
        SetConsoleOutputCP(saved);
        saved = 0;
    }
#endif
}

/// On Windows, switches the console's output code page to UTF-8 when stdout or stderr is a console
/// window; output redirected to a file or a pipe is left alone (its bytes are UTF-8 either way).
/// T-261f: the code page it replaced is restored at exit (std::atexit, registered once, only when
/// it was changed); not when the process crashes. Elsewhere it does nothing.
inline void useUtf8Console() {
#if defined(_WIN32)
    DWORD mode = 0;
    if (GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) == 0 &&
        GetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), &mode) == 0) {
        return;
    }
    const UINT current = GetConsoleOutputCP();
    if (current == 0 || current == CP_UTF8 || SetConsoleOutputCP(CP_UTF8) == 0) {
        return;
    }
    if (detail::savedConsoleOutputCp() == 0) {
        detail::savedConsoleOutputCp() = current;
    }
    static bool registered = false;
    if (!registered) {
        registered = true;
        std::atexit(&restoreConsole);
    }
#endif
}

#if defined(_WIN32)
/// `text` (UTF-16, as `wmain` receives it) as UTF-8; an unpaired surrogate becomes U+FFFD.
inline std::string wideToUtf8(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') {
        return std::string();
    }
    const int length = static_cast<int>(std::wcslen(text));
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    if (n <= 0) {
        return std::string();
    }
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), n, nullptr, nullptr);
    return out;
}

/// T-261f: the arguments of `wmain` as UTF-8 (wideToUtf8(), a text conversion), held for the
/// process: `argv()` points into this object and ends with a nullptr, as `main`'s does. Neither
/// copied nor moved, so the pointers stay valid.
class Utf8Args {
public:
    Utf8Args(int argc, wchar_t** wargv) {
        m_args.reserve(argc > 0 ? static_cast<size_t>(argc) : 0);
        for (int i = 0; i < argc; ++i) {
            m_args.push_back(wideToUtf8(wargv[i]));
        }
        m_argv.reserve(m_args.size() + 1);
        for (std::string& arg : m_args) {
            m_argv.push_back(arg.data());
        }
        m_argv.push_back(nullptr);
    }
    Utf8Args(const Utf8Args&) = delete;
    Utf8Args& operator=(const Utf8Args&) = delete;

    int argc() const { return static_cast<int>(m_args.size()); }
    char** argv() { return m_argv.data(); }

private:
    std::vector<std::string> m_args;
    std::vector<char*> m_argv;
};
#endif

}  // namespace xt::tools
