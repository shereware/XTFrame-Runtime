#include "xt/shell/sdl_rw_asset_resolver.h"

#include <utility>

/// T-240: SdlRwAssetResolver. See the header.

namespace xt::shell {

namespace {

SDL_RWops* defaultOpen(const char* path, void* /*user*/) { return SDL_RWFromFile(path, "rb"); }

}  // namespace

bool readAllAndClose(SDL_RWops* rw, std::vector<uint8_t>& out) {
    if (rw == nullptr) {
        return false;
    }
    std::vector<uint8_t> bytes;
    const Sint64 size = SDL_RWsize(rw);
    if (size > 0) {
        bytes.resize(static_cast<size_t>(size));
        size_t done = 0;
        while (done < bytes.size()) {
            const size_t n = SDL_RWread(rw, bytes.data() + done, 1, bytes.size() - done);
            if (n == 0) {
                break;
            }
            done += n;
        }
        if (done != bytes.size()) {
            SDL_RWclose(rw);
            return false;
        }
    } else {
        // Unknown size (a stream): read in chunks until EOF.
        uint8_t chunk[16384];
        for (;;) {
            const size_t n = SDL_RWread(rw, chunk, 1, sizeof(chunk));
            if (n == 0) {
                break;
            }
            bytes.insert(bytes.end(), chunk, chunk + n);
        }
    }
    SDL_RWclose(rw);
    out = std::move(bytes);
    return true;
}

SdlRwAssetResolver::SdlRwAssetResolver(std::vector<std::string> rootDirs, ImageDecodeFn decode,
                                       SdlRwOpenFn open, void* user)
    : FileAssetResolver(rootDirs, decode),
      m_roots(std::move(rootDirs)),
      m_open(open != nullptr ? open : &defaultOpen),
      m_user(user) {}

SdlRwAssetResolver::SdlRwAssetResolver(std::string rootDir, ImageDecodeFn decode, SdlRwOpenFn open,
                                       void* user)
    : SdlRwAssetResolver(std::vector<std::string>{std::move(rootDir)}, decode, open, user) {}

bool SdlRwAssetResolver::readFile(const std::string& path, std::vector<uint8_t>& out) const {
    return readAllAndClose(m_open(path.c_str(), m_user), out);
}

bool SdlRwAssetResolver::readBytes(std::string_view name, std::vector<uint8_t>& out) {
    for (const std::string& root : m_roots) {
        std::string path = root;
        if (!path.empty() && path.back() != '/' && path.back() != '\\') {
            path += '/';
        }
        path += name;
        if (readFile(path, out)) {
            return true;
        }
    }
    return false;
}

}  // namespace xt::shell
