// Per-process temp directory shared by every test suite (the editor_qt helper, moved here by
// T-267a). The tests used fixed names directly under the system temp directory and remove_all()
// them, so two worktrees running the same suite at the same time (a merge gate and an agent, say)
// deleted or overwrote each other's fixtures and failed spuriously. Every temp path a test uses
// goes under tempDir() (std::filesystem) or tempRoot() (Qt), which is unique to this process.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#if defined(QT_CORE_LIB)
#include <QtCore/QDir>
#include <QtCore/QString>
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

/// `<system temp>/xt_tests_<pid>`, created on first use.
inline std::filesystem::path tempDir() {
    static const std::filesystem::path root = [] {
        std::filesystem::path path = tempDirPath();
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        return path;
    }();
    return root;
}

/// T-267d: every test `main` ends with `return xt::test::finishTempDir(<doctest result>);`. A run
/// that passed (`result == 0`) removes this process's temp root, so `xt_tests_<pid>` directories no
/// longer pile up; a failing run keeps it and prints `temp files kept: <path>` as its last line, and
/// XT_TEST_KEEP_TEMP=1 keeps it always. A removal that fails (a file still open) prints a warning
/// and does not change the result. Returns `result`.
inline int finishTempDir(int result) {
    namespace fs = std::filesystem;
    const fs::path root = tempDirPath();
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        return result;
    }
    std::string keep;
#if defined(_MSC_VER)
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, "XT_TEST_KEEP_TEMP") == 0 && value != nullptr) {
        keep = value;
        std::free(value);
    }
#else
    if (const char* value = std::getenv("XT_TEST_KEEP_TEMP")) {
        keep = value;
    }
#endif
    if (result != 0 || keep == "1") {
        std::printf("temp files kept: %s\n", root.string().c_str());
        std::fflush(stdout);
        return result;
    }
    fs::remove_all(root, ec);
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
