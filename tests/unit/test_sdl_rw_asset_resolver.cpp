#define SDL_MAIN_HANDLED  // doctest owns main(); SDL must not rename it.

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "test_temp.h"
#include "xt/shell/sdl_rw_asset_resolver.h"

/// T-240 (docs/design/23-host-integration.md §23.11.1): SdlRwAssetResolver reads through SDL_RWops
/// (the APK's assets on Android). The in-memory cases use SDL_RWFromConstMem through the resolver's
/// open hook, the way AAssetManager-backed RWops behave.

using namespace xt;
using namespace xt::shell;

namespace {

struct MemFile {
    const char* path;
    const char* bytes;
};

/// A tiny in-memory "APK": `bundle/manifest.json` and two roots holding `a.txt`.
const MemFile kFiles[] = {
    {"bundle/manifest.json", "{\"version\":1}"},
    {"second/a.txt", "from second"},
    {"bundle/b.txt", "from bundle"},
};

int g_opens = 0;

SDL_RWops* memOpen(const char* path, void* user) {
    ++g_opens;
    CHECK(user == &g_opens);
    for (const MemFile& file : kFiles) {
        if (std::strcmp(file.path, path) == 0) {
            return SDL_RWFromConstMem(file.bytes, static_cast<int>(std::strlen(file.bytes)));
        }
    }
    return nullptr;
}

std::string asText(const std::vector<uint8_t>& bytes) { return std::string(bytes.begin(), bytes.end()); }

}  // namespace

TEST_CASE("SdlRwAssetResolver: readBytes tries the roots in order through the open hook") {
    g_opens = 0;
    SdlRwAssetResolver resolver(std::vector<std::string>{"bundle", "second/"}, nullptr, &memOpen, &g_opens);
    std::vector<uint8_t> out;
    REQUIRE(resolver.readBytes("b.txt", out));
    CHECK(asText(out) == "from bundle");
    CHECK(g_opens == 1);

    REQUIRE(resolver.readBytes("a.txt", out));  // Missing in "bundle", found in "second/".
    CHECK(asText(out) == "from second");
    CHECK(g_opens == 3);

    out.assign({1, 2, 3});
    CHECK_FALSE(resolver.readBytes("missing.png", out));
    CHECK(out == std::vector<uint8_t>{1, 2, 3});  // Unchanged on failure.
}

TEST_CASE("SdlRwAssetResolver: readFile reads a path beside the assets (the manifest)") {
    SdlRwAssetResolver resolver("bundle", nullptr, &memOpen, &g_opens);
    std::vector<uint8_t> out;
    REQUIRE(resolver.readFile("bundle/manifest.json", out));
    CHECK(asText(out) == "{\"version\":1}");
    CHECK_FALSE(resolver.readFile("bundle/nope.json", out));
    CHECK(resolver.roots() == std::vector<std::string>{"bundle"});
}

TEST_CASE("SdlRwAssetResolver: the default open reads the file system with SDL_RWFromFile") {
    namespace fs = std::filesystem;
    const fs::path dir = xt::test::tempDir() / "xt_sdl_rw_resolver_test";
    fs::create_directories(dir);
    {
        std::ofstream file(dir / "c.bin", std::ios::binary);
        file << "abc";
    }
    SdlRwAssetResolver resolver(dir.generic_string(), nullptr);
    std::vector<uint8_t> out;
    REQUIRE(resolver.readBytes("c.bin", out));
    CHECK(asText(out) == "abc");
    CHECK_FALSE(resolver.readBytes("d.bin", out));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("readAllAndClose: a null RWops fails and leaves the output alone") {
    std::vector<uint8_t> out{9};
    CHECK_FALSE(readAllAndClose(nullptr, out));
    CHECK(out == std::vector<uint8_t>{9});
    static const char kOne[1] = {'z'};
    REQUIRE(readAllAndClose(SDL_RWFromConstMem(kOne, 1), out));
    CHECK(asText(out) == "z");
}
