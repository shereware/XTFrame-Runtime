#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>

#include "manifest_writer.h"

/// T-693 (Docs/tasks/T-693.md): removal of the files a directory bundle's earlier packs left behind
/// (Docs/design/22-bundle.md §22.3 layout). Internal to xt_bundle; pack() calls it at the end of a
/// successful run unless PackOptions::keepStale is set.

namespace xt::bundle::detail {

/// The subdirectories of the output directory pack() writes into (and nothing else): only regular
/// files directly inside them are candidates for removal.
inline constexpr const char* kManagedBundleDirs[] = {"ui",      "assets", "thumbs", "themes",
                                                     "strings", "atlas",  "fonts"};

/// The files at the root of the output directory pack() writes (besides `manifest.json`): the global
/// flow and the temporary files of an interrupted atomic write (detail::writeFile). No other file at
/// the root is ever removed.
inline constexpr const char* kKnownBundleRootFiles[] = {"bundle.xtflow", "manifest.json.tmp",
                                                        "bundle.xtflow.tmp"};

/// The set of bundle-relative names (generic '/' separators) that must not be removed. Names are
/// compared ASCII case-insensitively, so a file whose name differs from a kept one only in case is
/// kept as well.
class KeptBundleFiles {
public:
    void add(std::string_view relative);
    /// Every file `manifest` names: `uis[].file/flow/thumb/assets`, `assets[].file/variants`,
    /// `atlas[].file/file2x`, `themes[]`, `strings`, `flows.global` and `glyphs`.
    void addManifest(const ManifestData& manifest);
    bool contains(std::string_view relative) const;

private:
    std::unordered_set<std::string> m_names;
};

struct StaleRemoval {
    uint32_t files = 0;
    uint64_t bytes = 0;
};

/// Removes from `outDir` every regular file that is directly inside one of kManagedBundleDirs, or is
/// one of kKnownBundleRootFiles, and is not in `keep`. Never follows a symbolic link or junction: a
/// managed directory that is one is skipped, and a link inside one is not a regular file. Nothing
/// below a subdirectory of a managed directory is looked at. `unmanagedDir` (the encode cache
/// directory, when given) is skipped when it is one of the managed directories.
StaleRemoval removeStaleFiles(const std::filesystem::path& outDir, const KeptBundleFiles& keep,
                              const std::filesystem::path& unmanagedDir);

}  // namespace xt::bundle::detail
