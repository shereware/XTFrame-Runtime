#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "file_bytes.h"
#include "xt/asset_resolver.h"
#include "xt/compile/compile.h"
#include "xt/compile/ir_writer.h"
#include "xt/compile/utf8_path.h"

/// `IrWriter`'s ASST entries -> `CompileResult::assets` (Docs/design/22-bundle.md §22.3,
/// Docs/tasks/T-91.md). Shared by compile_document.cpp and compile_lottie.cpp so both report the
/// list the same way.

namespace xt::compile {

/// T-452: whether a file system entry exists at `path` -- asked through
/// `options.fileExists` when the host set one, else the disk.
inline bool assetPathExists(const CompileOptions& options, const std::filesystem::path& path) {
    if (options.fileExists) {
        return options.fileExists(pathToUtf8(path));
    }
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

/// T-452: the whole file at `path` (an `<anim src>` or `<img src="*.svg">` file) -- through
/// `options.readAssetFile` when the host set one, else from disk.
inline bool readAssetFileBytes(const CompileOptions& options, const std::filesystem::path& path,
                               std::vector<uint8_t>& out) {
    if (options.readAssetFile) {
        return options.readAssetFile(pathToUtf8(path), out);
    }
    return detail::readFileBytes(path, out);
}

/// Resolves an external asset reference (`<img src>`, `<anim src>`, `@font-face src`,
/// `background-image`) on disk (Docs/design/22-bundle.md §22.2, 05 §5.9.12, Docs/tasks/T-138.md,
/// T-490): an absolute `rel` is returned as is; otherwise `sheetDir / rel` -- the directory of the
/// stylesheet that declared the reference (a `<link href>`ed `.xts`), empty for an inline `<style>`,
/// an element attribute or an in-memory sheet -- is taken when it exists; then each
/// `options.searchDirs[i] / rel` is tried in order and the first that exists wins; failing all of
/// them, `baseDir / rel` (the `.xt` file's own directory for `compileDocument()`,
/// `options.rootDir` for `compileDocumentFromStrings()`) -- the pre-T-138 behavior -- is returned
/// whether or not it exists, so a miss reports exactly as before. The probes go through
/// assetPathExists() (T-452).
inline std::filesystem::path resolveAsset(const CompileOptions& options, std::string_view baseDir,
                                          std::string_view rel, std::string_view sheetDir) {
    namespace fs = std::filesystem;
    fs::path relPath = pathFromUtf8(rel);
    if (relPath.is_absolute()) {
        return relPath;
    }
    if (!sheetDir.empty()) {
        fs::path candidate = pathFromUtf8(sheetDir) / relPath;
        if (assetPathExists(options, candidate)) {
            return candidate;
        }
    }
    for (const std::string& dir : options.searchDirs) {
        if (dir.empty()) {
            continue;
        }
        fs::path candidate = pathFromUtf8(dir) / relPath;
        if (assetPathExists(options, candidate)) {
            return candidate;
        }
    }
    return baseDir.empty() ? relPath : pathFromUtf8(baseDir) / relPath;
}

/// Every asset the writer holds, in AssetId order. `sourceName`/`resolvedPath` come from the
/// AssetBuilder metadata the `.xt` pipeline fills in; assets added without it (Lottie's own image
/// assets) report their ASST name as the source name and an empty path.
inline std::vector<CompiledAsset> collectCompiledAssets(const IrWriter& writer) {
    std::vector<CompiledAsset> out;
    out.reserve(writer.assets().size());
    for (const AssetBuilder& ab : writer.assets()) {
        CompiledAsset asset;
        asset.name = std::string(writer.stringAt(ab.name));
        asset.sourceName = ab.sourceName.empty() ? asset.name : ab.sourceName;
        asset.resolvedPath = ab.resolvedPath;
        asset.kind = ab.kind;
        asset.embedded = !ab.embedded.empty();
        asset.external = !asset.embedded && isExternalAssetName(asset.name);
        out.push_back(std::move(asset));
    }
    return out;
}

}  // namespace xt::compile
