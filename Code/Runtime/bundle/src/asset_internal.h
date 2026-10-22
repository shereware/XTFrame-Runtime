#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "xt/bundle/asset_meta.h"

/// Helpers shared by asset_meta.cpp, asset_import.cpp and asset_hygiene.cpp (Docs/tasks/T-312a.md).
/// Internal to xt_bundle.

namespace xt::bundle::assets::detail {

/// One asset file found by scanAssets().
struct ScannedAsset {
    /// Relative to the project root, '/' separators.
    std::string rel;
    /// Lowercase, with the dot.
    std::string ext;
    /// typeForExtension(ext).
    std::string type;
    /// An `@2x` file whose 1x file lies next to it (a variant, not an asset of its own).
    bool variant2x = false;
};

/// Every asset file under `<project>/<assetRoot>/` (the extension sets of regenerateAssetsManifest()),
/// sorted by `rel`. `.meta` files are never listed.
std::vector<ScannedAsset> scanAssets(const UiProject& project);

/// `<assetRoot>` ("shared" when empty), without a trailing '/'.
std::string assetRootOf(const UiProject& project);
/// `<rootDir>/<rel>` (UTF-8, '/' separators).
std::string absolutePath(const UiProject& project, std::string_view rel);
/// The lowercase extension of `path`, with the dot ("" when none).
std::string lowerExt(std::string_view path);
/// The file name of `path` (after the last '/' or '\\').
std::string fileNameOf(std::string_view path);
/// The file name of `path` without its extension.
std::string stemOf(std::string_view path);
/// True when `stem` ends with "@2x".
bool has2xSuffix(std::string_view stem);

/// Sets `w`/`h`/`alpha` of `m` from `bytes` (PNG, JPEG, SVG); clears them for anything else. The
/// slice fields are left alone.
void measureInto(std::string_view bytes, std::string_view ext, AssetMeasured& m);

/// The meta ensureMeta() would create for `rel` with content `bytes`, without a GUID and without
/// writing anything.
AssetMeta freshMeta(const UiProject& project, std::string_view rel, std::string_view bytes);

/// The nine-slice margins of a meta: `import.slice` (four numbers), else `measured.slice`. False
/// when it has neither.
bool sliceOf(const AssetMeta& meta, int slice[4]);

}  // namespace xt::bundle::assets::detail
