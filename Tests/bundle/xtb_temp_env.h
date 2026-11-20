// T-807: points the system temp directory of this test process at a directory the test owns while
// an `.xtb` is opened in-process, so Bundle::open's unpack cache (`<temp>/xtframe-bundle/`) and its
// eviction never touch the machine's real temp directory.
#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#include "test_temp.h"

namespace xt::test {

/// Sets TMP, TEMP and TMPDIR to `root` (created) for its lifetime and restores them afterwards
/// (a variable that was unset or empty is removed). Calls tempDir() first, so the per-process test
/// root is fixed before the temp directory moves.
class ScopedTempEnv {
public:
    explicit ScopedTempEnv(const std::filesystem::path& root) {
        (void)tempDir();
        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        for (int i = 0; i < kCount; ++i) {
            m_saved[i] = tempEnv(kNames[i]);
            set(kNames[i], root.string());
        }
    }
    ~ScopedTempEnv() {
        for (int i = 0; i < kCount; ++i) {
            set(kNames[i], m_saved[i]);
        }
    }
    ScopedTempEnv(const ScopedTempEnv&) = delete;
    ScopedTempEnv& operator=(const ScopedTempEnv&) = delete;

private:
    static constexpr int kCount = 3;
    static constexpr const char* kNames[kCount] = {"TMP", "TEMP", "TMPDIR"};

    static void set(const char* name, const std::string& value) {
#if defined(_WIN32)
        _putenv_s(name, value.c_str());  // an empty value removes the variable
#else
        if (value.empty()) {
            unsetenv(name);
        } else {
            setenv(name, value.c_str(), 1);
        }
#endif
    }

    std::string m_saved[kCount];
};

}  // namespace xt::test
