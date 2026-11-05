// T-623 (Docs/design/22-bundle.md §22.16): `spec.defaultLanguage` -- read from and written back to
// the `.xtui`, written by pack() to the manifest (the project's value, else the first strings table
// by file name), read back as BundleSpec::defaultLanguage, and the defaultLanguage() helpers the
// tools start a project or bundle in.

#include <filesystem>
#include <fstream>
#include <iterator>
#include <json.hpp>
#include <memory>
#include <string>

#include "doctest.h"
#include "test_temp.h"
#include "xt/bundle/bundle.h"
#include "xt/bundle/pack.h"
#include "xt/bundle/ui_project.h"

using namespace xt;
using namespace xt::bundle;

namespace {

namespace fs = std::filesystem;

/// A writable copy of `Tests/bundle/fixtures/default_language` (strings en / zh-TW,
/// `spec.defaultLanguage: zh-TW`).
std::string copyFixtureT623(const std::string& name) {
    const fs::path dir = xt::test::tempDir() / ("xt_t623_src_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::copy(fs::path(XT_TEST_BUNDLE_DIR) / "fixtures" / "default_language", dir, fs::copy_options::recursive,
             ec);
    REQUIRE(ec.value() == 0);
    return dir.generic_string();
}

std::string outDirT623(const std::string& name) {
    const fs::path dir = xt::test::tempDir() / ("xt_t623_out_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    return dir.generic_string();
}

UiProject loadT623(const std::string& dir) {
    std::string error;
    Result<UiProject> project = UiProject::load(dir, &error);
    REQUIRE_MESSAGE(project.ok(), error);
    return std::move(project.value());
}

/// Packs `dir` into `out` and returns the raw manifest JSON.
nlohmann::json packT623(const std::string& dir, const std::string& out) {
    PackOptions options;
    options.thumbnails = false;
    options.outDir = out;
    const PackReport report = pack(loadT623(dir), options);
    REQUIRE(report.ok);
    std::ifstream in(fs::path(out) / "manifest.json", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return nlohmann::json::parse(text);
}

std::unique_ptr<Bundle> openT623(const std::string& out) {
    std::string error;
    Result<std::unique_ptr<Bundle>> bundle = Bundle::open(out, nullptr, &error);
    REQUIRE_MESSAGE(bundle.ok(), error);
    return std::move(bundle.value());
}

}  // namespace

TEST_CASE("T-623: the .xtui's spec.defaultLanguage is read, written back only when set") {
    const std::string dir = copyFixtureT623("roundtrip");
    UiProject project = loadT623(dir);
    CHECK(project.spec.defaultLanguage == "zh-TW");
    CHECK(project.extraSpecJson.find("defaultLanguage") == std::string::npos);
    REQUIRE(project.save() == Status::kOk);
    CHECK(loadT623(dir).spec.defaultLanguage == "zh-TW");

    project.spec.defaultLanguage.clear();
    REQUIRE(project.save() == Status::kOk);
    std::ifstream in(fs::path(findUiProjectFile(dir)), std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.find("defaultLanguage") == std::string::npos);
    CHECK(loadT623(dir).spec.defaultLanguage.empty());
}

TEST_CASE("T-623: pack writes the project's spec.defaultLanguage to the manifest") {
    const std::string dir = copyFixtureT623("explicit");
    const std::string out = outDirT623("explicit");
    const nlohmann::json manifest = packT623(dir, out);
    REQUIRE(manifest.contains("spec"));
    CHECK(manifest["spec"].value("defaultLanguage", std::string()) == "zh-TW");
    const std::unique_ptr<Bundle> bundle = openT623(out);
    REQUIRE(bundle->spec() != nullptr);
    CHECK(bundle->spec()->defaultLanguage == "zh-TW");
    CHECK(defaultLanguage(*bundle) == "zh-TW");
    CHECK(defaultLanguage(loadT623(dir)) == "zh-TW");
}

TEST_CASE("T-623: without spec.defaultLanguage pack writes the first strings table by file name") {
    const std::string dir = copyFixtureT623("fallback");
    {
        UiProject project = loadT623(dir);
        project.spec.defaultLanguage.clear();
        REQUIRE(project.save() == Status::kOk);
    }
    CHECK(defaultLanguage(loadT623(dir)) == "en");
    const std::string out = outDirT623("fallback");
    const nlohmann::json manifest = packT623(dir, out);
    CHECK(manifest["spec"].value("defaultLanguage", std::string()) == "en");
    CHECK(defaultLanguage(*openT623(out)) == "en");
}

TEST_CASE("T-623: a project without strings tables writes no spec.defaultLanguage") {
    const std::string dir = copyFixtureT623("nostrings");
    std::error_code ec;
    fs::remove_all(fs::path(dir) / "shared" / "strings", ec);
    {
        UiProject project = loadT623(dir);
        project.spec.defaultLanguage.clear();
        REQUIRE(project.save() == Status::kOk);
    }
    CHECK(defaultLanguage(loadT623(dir)).empty());
    const std::string out = outDirT623("nostrings");
    const nlohmann::json manifest = packT623(dir, out);
    REQUIRE(manifest.contains("spec"));
    CHECK_FALSE(manifest["spec"].contains("defaultLanguage"));
    const std::unique_ptr<Bundle> bundle = openT623(out);
    CHECK(bundle->strings().empty());
    CHECK(defaultLanguage(*bundle).empty());
}

TEST_CASE("T-623: a bundle packed before T-623 (no spec.defaultLanguage) starts in its first table") {
    const std::string dir = copyFixtureT623("old");
    const std::string out = outDirT623("old");
    nlohmann::json manifest = packT623(dir, out);
    manifest["spec"].erase("defaultLanguage");
    {
        std::ofstream file(fs::path(out) / "manifest.json", std::ios::binary | std::ios::trunc);
        file << manifest.dump(2);
    }
    const std::unique_ptr<Bundle> bundle = openT623(out);
    REQUIRE(bundle->spec() != nullptr);
    CHECK(bundle->spec()->defaultLanguage.empty());
    CHECK(defaultLanguage(*bundle) == "en");
}
