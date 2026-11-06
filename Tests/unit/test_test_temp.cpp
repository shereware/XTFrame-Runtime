// T-632: the first tempDir() of a test process sweeps the `xt_tests_*` roots that crashed or killed
// test processes left in the system temp directory (Tests/common/test_temp.h). tempDir() sweeps once
// per process, so these cases call the sweep it runs, sweepStaleTempDirs(), on fake roots placed
// next to this process's real one.
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "doctest.h"
#include "test_temp.h"

namespace fs = std::filesystem;
using xt::test::sweepStaleTempDirs;
using xt::test::tempDir;

namespace {

fs::path makeFakeRoot(const fs::path& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path / "sub", ec);
    std::ofstream(path / "sub" / "file.txt") << "left behind";
    return path;
}

void makeOld(const fs::path& path) {
    std::error_code ec;
    fs::last_write_time(path, fs::file_time_type::clock::now() - std::chrono::hours(48), ec);
    REQUIRE_FALSE(ec);
}

std::string selfSuffix() { return tempDir().filename().string().substr(std::string("xt_tests_").size()); }

}  // namespace

TEST_CASE("test temp: a root last written more than 24 hours ago is swept") {
    const fs::path parent = tempDir().parent_path();
    const fs::path fake = makeFakeRoot(parent / "xt_tests_999999");
    makeOld(fake);
    sweepStaleTempDirs(parent, tempDir());
    CHECK_FALSE(fs::exists(fake));
    CHECK(fs::is_directory(tempDir()));
}

TEST_CASE("test temp: a recent root whose process is gone is swept") {
    if (xt::test::tempOwnerAlive(999999UL)) {
        return;  // pid 999999 happens to run here; the age case above covers the sweep
    }
    const fs::path parent = tempDir().parent_path();
    const fs::path fake = makeFakeRoot(parent / "xt_tests_999999");
    sweepStaleTempDirs(parent, tempDir());
    CHECK_FALSE(fs::exists(fake));
}

TEST_CASE("test temp: a recent root of a running process stays") {
    // This process's own root, not passed as `self`: only the pid check keeps it.
    const fs::path parent = tempDir().parent_path();
    const fs::path marker = tempDir() / "t632_marker.txt";
    std::ofstream(marker) << "mine";
    CHECK(xt::test::tempOwnerAlive(std::stoul(selfSuffix())));
    sweepStaleTempDirs(parent, parent / "xt_tests_not_this_process");
    CHECK(fs::exists(marker));
}

TEST_CASE("test temp: directories without the xt_tests_ prefix and non-pid names stay") {
    const fs::path parent = tempDir().parent_path();
    const fs::path other = makeFakeRoot(parent / ("xt_testsx_" + selfSuffix()));
    makeOld(other);
    const fs::path named = makeFakeRoot(parent / ("xt_tests_" + selfSuffix() + "_named"));
    sweepStaleTempDirs(parent, tempDir());
    CHECK(fs::exists(other / "sub" / "file.txt"));
    CHECK(fs::exists(named / "sub" / "file.txt"));
    std::error_code ec;
    fs::remove_all(other, ec);
    fs::remove_all(named, ec);
}
