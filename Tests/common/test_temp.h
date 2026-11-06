// Per-process temp directory shared by every test suite (the editor_qt helper, moved here by
// T-267a). The tests used fixed names directly under the system temp directory and remove_all()
// them, so two worktrees running the same suite at the same time (a merge gate and an agent, say)
// deleted or overwrote each other's fixtures and failed spuriously. Every temp path a test uses
// goes under tempDir() (std::filesystem) or tempRoot() (Qt), which is unique to this process.
#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#else
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#endif

#if defined(QT_CORE_LIB)
#include <QtCore/QDir>
#include <QtCore/QString>
#endif

#if defined(_WIN32)
// T-632: the four kernel32 calls tempOwnerAlive() needs, declared exactly as <windows.h> declares
// them (so a file including both compiles) instead of including it: this header is in every test
// file, and <windows.h> macros (`small`, `ERROR`, `DrawText`...) break code that never asked for them.
extern "C" {
__declspec(dllimport) void* __stdcall OpenProcess(unsigned long access, int inherit, unsigned long pid);
__declspec(dllimport) int __stdcall GetExitCodeProcess(void* process, unsigned long* code);
__declspec(dllimport) int __stdcall CloseHandle(void* handle);
__declspec(dllimport) unsigned long __stdcall GetLastError(void);
}
#endif

namespace xt::test {

/// `<system temp>/xt_tests_<pid>`, without creating it.
inline std::filesystem::path tempDirPath() {
    static const std::filesystem::path root = [] {
#if defined(_WIN32)
        const long pid = static_cast<long>(_getpid());
#else
        const long pid = static_cast<long>(getpid());
#endif
        return std::filesystem::temp_directory_path() / ("xt_tests_" + std::to_string(pid));
    }();
    return root;
}

/// The value of the environment variable `name`, or "" when it is not set.
inline std::string tempEnv(const char* name) {
    std::string text;
#if defined(_MSC_VER)
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, name) == 0 && value != nullptr) {
        text = value;
        std::free(value);
    }
#else
    if (const char* value = std::getenv(name)) {
        text = value;
    }
#endif
    return text;
}

/// T-632: false only when the process `pid` certainly does not run (any doubt, e.g. no access to
/// it, counts as running).
inline bool tempOwnerAlive(unsigned long pid) {
#if defined(_WIN32)
    constexpr unsigned long kQueryLimitedInformation = 0x1000;  // PROCESS_QUERY_LIMITED_INFORMATION
    constexpr unsigned long kInvalidParameter = 87;             // ERROR_INVALID_PARAMETER: no such pid
    constexpr unsigned long kStillActive = 259;                 // STILL_ACTIVE
    void* process = ::OpenProcess(kQueryLimitedInformation, 0, pid);
    if (process == nullptr) {
        return ::GetLastError() != kInvalidParameter;
    }
    unsigned long code = 0;
    const bool exited = ::GetExitCodeProcess(process, &code) != 0 && code != kStillActive;
    ::CloseHandle(process);
    return !exited;
#else
    return kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
#endif
}

/// T-632: removes the `xt_tests_*` directories in `parent` (the system temp directory) other than
/// `self` that a test process left behind: last written more than 24 hours ago, or named
/// `xt_tests_<pid>` with no process `<pid>` running. Nothing else in `parent` is touched; every
/// error is ignored. tempDir() calls it once per process (not with XT_TEST_KEEP_TEMP=1).
inline void sweepStaleTempDirs(const std::filesystem::path& parent, const std::filesystem::path& self) {
    namespace fs = std::filesystem;
    using Name = fs::path::string_type;  // native: no conversion, so no throw on any file name
    static const Name prefix = fs::path("xt_tests_").native();
    static const Name digits = fs::path("0123456789").native();
    const auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(24);
    std::error_code ec;
    fs::directory_iterator it(parent, ec);
    for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const fs::path path = it->path();
        const Name name = path.filename().native();
        std::error_code entryEc;
        if (name.compare(0, prefix.size(), prefix) != 0 || name == self.filename().native() ||
            !it->is_directory(entryEc)) {
            continue;
        }
        const auto written = fs::last_write_time(path, entryEc);
        bool stale = !entryEc && written < cutoff;
        const Name suffix = name.substr(prefix.size());
        if (!stale && !suffix.empty() && suffix.size() <= 9 &&
            suffix.find_first_not_of(digits) == Name::npos) {
            unsigned long pid = 0;
            for (const auto digit : suffix) {
                pid = pid * 10UL + static_cast<unsigned long>(digit - digits[0]);
            }
            stale = !tempOwnerAlive(pid);
        }
        if (stale) {
            fs::remove_all(path, entryEc);
        }
    }
}

/// `<system temp>/xt_tests_<pid>`, created on first use. T-632: the first use also sweeps the
/// temp roots that crashed or killed test processes, or a failed removal, left behind
/// (sweepStaleTempDirs).
inline std::filesystem::path tempDir() {
    static const std::filesystem::path root = [] {
        std::filesystem::path path = tempDirPath();
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        if (tempEnv("XT_TEST_KEEP_TEMP") != "1") {
            sweepStaleTempDirs(path.parent_path(), path);
        }
        return path;
    }();
    return root;
}

/// T-267d: every test `main` ends with `return xt::test::finishTempDir(<doctest result>);`. A run
/// that passed (`result == 0`) removes this process's temp root, so `xt_tests_<pid>` directories no
/// longer pile up; a failing run keeps it and prints `temp files kept: <path>` as its last line, and
/// XT_TEST_KEEP_TEMP=1 keeps it always. A removal that fails (a file still open) is retried 5 times
/// 200 ms apart (T-632), then prints a warning and does not change the result. Returns `result`.
inline int finishTempDir(int result) {
    namespace fs = std::filesystem;
    const fs::path root = tempDirPath();
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        return result;
    }
    if (result != 0 || tempEnv("XT_TEST_KEEP_TEMP") == "1") {
        std::printf("temp files kept: %s\n", root.string().c_str());
        std::fflush(stdout);
        return result;
    }
    // T-632: a virus scanner (Norton) holds a just-written file for a moment; retry before warning.
    for (int attempt = 0; attempt < 6; ++attempt) {
        if (attempt > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        ec.clear();
        fs::remove_all(root, ec);
        if (!ec) {
            break;
        }
    }
    if (ec) {
        std::fprintf(stderr, "warning: could not remove the test temp directory %s: %s\n",
                     root.string().c_str(), ec.message().c_str());
    }
    return result;
}

#if defined(QT_CORE_LIB)
/// tempDir() as a QString with '/' separators, for the Qt suites.
inline QString tempRoot() {
    static const QString root = QDir::fromNativeSeparators(QString::fromStdWString(tempDir().wstring()));
    return root;
}
#endif

}  // namespace xt::test
