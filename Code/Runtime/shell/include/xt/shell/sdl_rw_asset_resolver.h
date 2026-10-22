#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xt/asset_resolver.h"

// SDL2 is a real third-party dependency of xt_shell (see Tools/CMake/xt_sdl2.cmake).
#include <SDL.h>

/// T-240 (Docs/design/23-host-integration.md §23.11.1): an AssetResolver that reads every file
/// through SDL_RWops. On Android SDL_RWFromFile() resolves a relative path inside the APK's
/// `assets/` (AAssetManager) and an absolute one on the file system, so a bundle shipped in the APK
/// and one pushed to `/sdcard/...` are read the same way.

namespace xt::shell {

/// Opens `path` for reading; nullptr when it cannot be opened. `user` is the pointer given to the
/// resolver's constructor. The resolver closes the returned SDL_RWops.
using SdlRwOpenFn = SDL_RWops* (*)(const char* path, void* user);

/// Everything FileAssetResolver does (decode cache, atlas pages, `@2x`, DDS, external textures),
/// with its file reads replaced by SDL_RWops: readBytes() -- which every other read goes through --
/// tries `root/name` for each root in order, opening it with `open` (SDL_RWFromFile by default).
class SdlRwAssetResolver : public FileAssetResolver {
public:
    /// `open` nullptr = SDL_RWFromFile(path, "rb").
    SdlRwAssetResolver(std::vector<std::string> rootDirs, ImageDecodeFn decode, SdlRwOpenFn open = nullptr,
                       void* user = nullptr);
    /// Single-root form.
    SdlRwAssetResolver(std::string rootDir, ImageDecodeFn decode, SdlRwOpenFn open = nullptr,
                       void* user = nullptr);

    /// See AssetResolver::readBytes: the first root where `root/name` opens and reads completely.
    bool readBytes(std::string_view name, std::vector<uint8_t>& out) override;

    /// Reads the whole file `path` (not joined to any root) through `open`; false when it cannot be
    /// opened or read, leaving `out` unchanged. Hosts use it for files beside the assets (the
    /// bundle manifest, a flow graph) so they take the same route.
    bool readFile(const std::string& path, std::vector<uint8_t>& out) const;

    /// The roots, in lookup order.
    const std::vector<std::string>& roots() const { return m_roots; }

private:
    std::vector<std::string> m_roots;
    SdlRwOpenFn m_open = nullptr;
    void* m_user = nullptr;
};

/// Reads the whole of `rw` into `out` and closes it; false (leaving `out` unchanged) on a read
/// error or a null `rw`.
bool readAllAndClose(SDL_RWops* rw, std::vector<uint8_t>& out);

}  // namespace xt::shell
