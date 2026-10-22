#pragma once

#include <string_view>

/// T-262b (F15): the containment rule for the file paths a bundle manifest names. Internal header
/// next to bundle_json.h; readManifest() rejects a manifest with any path this returns false for.

namespace xt::bundle::detail {

/// True when `path` stays inside the bundle directory once joined to it: a relative path with no
/// `..` segment, no drive letter (`C:`), no UNC prefix and no leading `/` or `\`. Both `/` and `\`
/// separate segments. An empty path is not checked here (the optional fields leave it empty).
inline bool isSafeBundlePath(std::string_view path) {
    if (!path.empty() && (path.front() == '/' || path.front() == '\\')) {
        return false;  // Rooted, which includes `//server` and `\\server`.
    }
    if (path.size() >= 2 && path[1] == ':') {
        return false;  // A drive letter, absolute (`C:/`) or drive-relative (`C:x`).
    }
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find_first_of("/\\", start);
        if (end == std::string_view::npos) {
            end = path.size();
        }
        if (path.substr(start, end - start) == "..") {
            return false;
        }
        start = end + 1;
    }
    return true;
}

}  // namespace xt::bundle::detail
