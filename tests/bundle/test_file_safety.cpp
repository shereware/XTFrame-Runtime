#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#include "bundle_json.h"
#include "bundle_paths.h"
#include "doctest.h"
#include "test_temp.h"
#include "xt/bundle/bundle.h"

/// T-262b (docs/tasks/T-262b.md F15, F17): every file path a bundle manifest names stays inside
/// the bundle directory, and bundle::detail::writeFile() replaces its target atomically.

using namespace xt;
using namespace xt::bundle;

namespace {

namespace fs = std::filesystem;

/// A fresh temp directory `xt_t262b_<name>` (removed first).
fs::path freshDir(const std::string& name) {
    const fs::path dir = xt::test::tempDir() / ("xt_t262b_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

std::string readText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeText(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

/// A minimal valid manifest whose one UI entry carries `uiFields` (JSON members after `"id"`) and
/// whose top level carries `extra` (JSON members after `"uis"`).
std::string manifestWith(const std::string& uiFields, const std::string& extra = std::string()) {
    return R"({"version": 1, "bundle": {"id": "b", "name": "B", "irMajor": )" +
           std::to_string(static_cast<int>(kIrFormatMajor)) + R"(},
  "uis": [{"id": "hud")" +
           (uiFields.empty() ? std::string() : ", " + uiFields) + "}]" +
           (extra.empty() ? std::string() : ", " + extra) + "}";
}

/// Opens a bundle whose manifest.json is `manifest`; returns the status and sets `error`.
Status openWith(const std::string& name, const std::string& manifest, std::string& error) {
    const fs::path dir = freshDir(name);
    writeText(dir / "manifest.json", manifest);
    error.clear();
    return Bundle::open(dir.generic_string(), nullptr, &error).status();
}

}  // namespace

TEST_CASE("T-262b isSafeBundlePath: relative paths inside the bundle only") {
    CHECK(bundle::detail::isSafeBundlePath("ui/hud.xtir"));
    CHECK(bundle::detail::isSafeBundlePath("assets/a/b/c.png"));
    CHECK(bundle::detail::isSafeBundlePath("a..b/c.png"));  // ".." only as a whole segment
    CHECK(bundle::detail::isSafeBundlePath("./ui/hud.xtir"));
    CHECK_FALSE(bundle::detail::isSafeBundlePath("../secret.bin"));
    CHECK_FALSE(bundle::detail::isSafeBundlePath("ui/../../secret.bin"));
    CHECK_FALSE(bundle::detail::isSafeBundlePath("ui\\..\\..\\secret.bin"));
    CHECK_FALSE(bundle::detail::isSafeBundlePath(".."));
    CHECK_FALSE(bundle::detail::isSafeBundlePath("/etc/passwd"));
    CHECK_FALSE(bundle::detail::isSafeBundlePath("\\secret.bin"));
    CHECK_FALSE(bundle::detail::isSafeBundlePath("C:/secret.bin"));
    CHECK_FALSE(bundle::detail::isSafeBundlePath("c:secret.bin"));
    CHECK_FALSE(bundle::detail::isSafeBundlePath("\\\\server\\share\\x.bin"));
    CHECK_FALSE(bundle::detail::isSafeBundlePath("//server/share/x.bin"));
}

TEST_CASE("T-262b manifest: a '..' path is rejected") {
    std::string error;
    CHECK(openWith("dotdot", manifestWith(R"("file": "../../secret.bin")"), error) ==
          Status::kInvalidArgument);
    CHECK(error.find("manifest path escapes the bundle: ../../secret.bin") != std::string::npos);

    // Every path field is checked, not only uis[].file.
    CHECK(openWith("dotdot_flow", manifestWith(R"("file": "ui/hud.xtir", "flow": "ui/../../f.xtflow")"),
                   error) == Status::kInvalidArgument);
    CHECK(openWith("dotdot_thumb", manifestWith(R"("thumb": "../t.png")"), error) ==
          Status::kInvalidArgument);
    CHECK(openWith("dotdot_uiasset", manifestWith(R"("assets": ["../a.png"])"), error) ==
          Status::kInvalidArgument);
    CHECK(openWith("dotdot_asset", manifestWith("", R"("assets": [{"file": "../a.png", "kind": "image"}])"),
                   error) == Status::kInvalidArgument);
    CHECK(openWith("dotdot_variant",
                   manifestWith("", R"("assets": [{"file": "a.png", "variants": {"2x": "../a@2x.png"}}])"),
                   error) == Status::kInvalidArgument);
    CHECK(openWith("dotdot_atlas",
                   manifestWith(
                       "", R"("atlas": [{"name": "p", "file": "../p.png", "w": 4, "h": 4, "entries": []}])"),
                   error) == Status::kInvalidArgument);
    CHECK(openWith("dotdot_theme", manifestWith("", R"("themes": [{"name": "dark", "file": "../dark.xts"}])"),
                   error) == Status::kInvalidArgument);
    CHECK(openWith("dotdot_strings", manifestWith("", R"("strings": {"en": "../en.csv"})"), error) ==
          Status::kInvalidArgument);
    CHECK(openWith("dotdot_global", manifestWith("", R"("flows": {"global": "../g.xtflow"})"), error) ==
          Status::kInvalidArgument);
    CHECK(openWith("dotdot_glyph", manifestWith("", R"("glyphs": {"pad": {"a": "../a.png"}})"), error) ==
          Status::kInvalidArgument);
}

TEST_CASE("T-262b manifest: an absolute path is rejected") {
    std::string error;
    CHECK(openWith("absolute", manifestWith(R"("file": "/etc/passwd")"), error) == Status::kInvalidArgument);
    CHECK(error.find("manifest path escapes the bundle: /etc/passwd") != std::string::npos);
    CHECK(openWith("absolute_bs", manifestWith(R"("file": "\\secret.bin")"), error) ==
          Status::kInvalidArgument);
    CHECK(openWith("unc", manifestWith(R"("file": "\\\\server\\share\\x.xtir")"), error) ==
          Status::kInvalidArgument);
}

TEST_CASE("T-262b manifest: a drive-letter path is rejected") {
    std::string error;
    CHECK(openWith("drive", manifestWith(R"("file": "C:/Windows/win.ini")"), error) ==
          Status::kInvalidArgument);
    CHECK(error.find("manifest path escapes the bundle: C:/Windows/win.ini") != std::string::npos);
    CHECK(openWith("drive_relative", manifestWith(R"("file": "d:x.xtir")"), error) ==
          Status::kInvalidArgument);
}

TEST_CASE("T-262b manifest: nested relative paths are accepted") {
    std::string error;
    const std::string manifest = manifestWith(
        R"("file": "ui/sub/hud.xtir", "flow": "ui/sub/hud.xtflow", "thumb": "thumbs/hud.png",
           "assets": ["assets/x/a.png"])",
        R"("assets": [{"file": "assets/x/a.png", "kind": "image", "variants": {"2x": "assets/x/a@2x.png"}}],
           "themes": [{"name": "dark", "file": "themes/dark.xts"}],
           "strings": {"en": "strings/en.csv"},
           "flows": {"global": "flows/global.xtflow"})");
    CHECK(openWith("nested", manifest, error) == Status::kOk);
    CHECK(error.empty());
}

TEST_CASE("T-262b writeFile: replaces the target and leaves no temporary file") {
    const fs::path dir = freshDir("write_ok");
    const fs::path target = dir / "file.txt";
    const std::string old = "old contents that are longer than the new ones\n";
    writeText(target, old);
    // A hard link shows whether the file was rewritten in place (the link sees the new bytes) or
    // replaced by a new file (the link keeps the old ones).
    std::error_code ec;
    fs::create_hard_link(target, dir / "alias.txt", ec);
    const bool linked = !ec;
    REQUIRE(bundle::detail::writeFile(target.generic_string(), "new\n"));
    CHECK(readText(target) == "new\n");
    if (linked) {
        CHECK(readText(dir / "alias.txt") == old);
        fs::remove(dir / "alias.txt", ec);
    }
    CHECK_FALSE(fs::exists(dir / "file.txt.tmp"));
    // A new file is written the same way.
    REQUIRE(bundle::detail::writeFile((dir / "fresh.txt").generic_string(), "fresh"));
    CHECK(readText(dir / "fresh.txt") == "fresh");
    CHECK_FALSE(fs::exists(dir / "fresh.txt.tmp"));
    int entries = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir)) {
        (void)entry;
        ++entries;
    }
    CHECK(entries == 2);
}

TEST_CASE("T-262b writeFile: a missing directory fails and leaves no temporary file") {
    const fs::path dir = freshDir("write_missing");
    const fs::path target = dir / "absent" / "file.txt";
    CHECK_FALSE(bundle::detail::writeFile(target.generic_string(), "text"));
    CHECK_FALSE(fs::exists(target));
    CHECK_FALSE(fs::exists(dir / "absent" / "file.txt.tmp"));
    CHECK(fs::is_empty(dir));
}

TEST_CASE("T-262b writeFile: a target that cannot be replaced keeps its contents") {
    // The target is a directory: the temporary file is written, the replace fails, and the
    // temporary file is removed again.
    const fs::path dir = freshDir("write_blocked");
    const fs::path target = dir / "blocked";
    fs::create_directories(target / "inner");
    CHECK_FALSE(bundle::detail::writeFile(target.generic_string(), "text"));
    CHECK(fs::is_directory(target / "inner"));
    CHECK_FALSE(fs::exists(dir / "blocked.tmp"));
}
