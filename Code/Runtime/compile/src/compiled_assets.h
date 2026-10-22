#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "xt/asset_resolver.h"
#include "xt/compile/compile.h"
#include "xt/compile/ir_writer.h"
#include "xt/compile/utf8_path.h"

/// `IrWriter`'s ASST entries -> `CompileResult::assets` (Docs/design/22-bundle.md §22.3,
/// Docs/tasks/T-91.md). Shared by compile_document.cpp and compile_lottie.cpp so both report the
/// list the same way.

namespace xt::compile {

/// Resolves an external asset reference (`<img src>`, `<anim src>`, `@font-face src`) on disk
/// (Docs/design/22-bundle.md §22.2, Docs/tasks/T-138.md): an absolute `rel` is returned as is;
/// otherwise each `options.searchDirs[i] / rel` is tried in order and the first that exists wins;
/// failing all of them, `baseDir / rel` (the `.xt` file's own directory for `compileDocument()`,
/// `options.rootDir` for `compileDocumentFromStrings()`) -- the pre-T-138 behavior -- is returned
/// whether or not it exists, so a miss reports exactly as before.
inline std::filesystem::path resolveAsset(const CompileOptions& options, std::string_view baseDir,
                                          std::string_view rel) {
    namespace fs = std::filesystem;
    fs::path relPath = pathFromUtf8(rel);
    if (relPath.is_absolute()) {
        return relPath;
    }
    for (const std::string& dir : options.searchDirs) {
        if (dir.empty()) {
            continue;
        }
        fs::path candidate = pathFromUtf8(dir) / relPath;
        std::error_code ec;
        if (fs::exists(candidate, ec)) {
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
