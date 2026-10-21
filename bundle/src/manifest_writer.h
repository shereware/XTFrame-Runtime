#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xt/bundle/bundle.h"
#include "xt/bundle/ui_project.h"
#include "xt/types.h"

/// manifest.json serialization for pack() (docs/design/22-bundle.md §22.3, docs/tasks/T-92.md).
/// Internal to xt_bundle: `Bundle` reads a manifest, this writes one, and both stay out of each
/// other's way. nlohmann never reaches include/xt/bundle/.

namespace xt::bundle {

/// One `assets[]` row: the deduplicated file plus the source names that reached it.
struct ManifestAsset {
    /// `assets/<hash16>.<ext>`, relative to the bundle root.
    std::string file;
    /// "image", "font" or "shader".
    std::string kind;
    /// Size of the written file in bytes.
    uint64_t bytes = 0;
    /// Every distinct source name this file was referenced by, in first-seen order.
    std::vector<std::string> sources;
    /// T-183 (§22.10 `assets.manifest`): the `manifest/assets.yaml` entry of the source file,
    /// written as `manifest {id, w, h, alpha, slice}` (present fields only).
    bool hasManifest = false;
    std::string manifestId;
    AssetMeasured measured;
    /// T-199 (§22.10): `format` -- "bc1" / "bc3" / "bc7" when `file` is a DDS written by
    /// `--compress`; empty for a file kept as it was.
    std::string format;
    /// T-199: `variants.2x` -- the `@2x` variant's file, or empty.
    std::string variant2x;
    /// T-345: `variants.3x` -- the `@3x` variant's file, or empty.
    std::string variant3x;
    /// T-199: `atlas` -- the atlas page names holding this asset (its own file is then not
    /// written); empty when it is not atlased.
    std::vector<std::string> atlases;
};

/// T-199 (§22.10 `atlas[]`): one atlas page.
struct ManifestAtlasEntry {
    /// The packed asset name the entry holds (`assets/<hash>.<ext>`).
    std::string asset;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};
struct ManifestAtlas {
    /// Page name: "<n>" (bundle scope) or "<ui id>-<n>" (ui scope); the IR refers to it as
    /// `atlas:<name>#<asset>`.
    std::string name;
    /// `atlas/<name>.png` or `.dds`.
    std::string file;
    /// The 2x variant of the page (same layout at twice the size), or empty.
    std::string file2x;
    /// "bc1" / "bc3" / "bc7" when `file` is a DDS; empty for PNG.
    std::string format;
    int w = 0;
    int h = 0;
    std::vector<ManifestAtlasEntry> entries;
};

/// A whole manifest.json, as pack() builds it and readManifest() parses it back for an incremental
/// run. `uis` reuses UiInfo, the same record Bundle hands to hosts.
struct ManifestData {
    /// `bundle.id`.
    std::string bundleId;
    /// `bundle.name`.
    std::string bundleName;
    /// `bundle.builtAt`, ISO-8601 UTC.
    std::string builtAt;
    /// `bundle.xtframe`, i.e. xt::versionString().
    std::string xtframe;
    /// `bundle.irMajor`, i.e. kIrFormatMajor.
    int irMajor = 0;
    /// The UIs, in manifest order.
    std::vector<UiInfo> uis;
    /// The deduplicated assets, in first-seen order.
    std::vector<ManifestAsset> assets;
    /// T-199 (§22.10 `atlas[]`): the atlas pages, written after `assets`; omitted when empty.
    std::vector<ManifestAtlas> atlases;
    /// T-170 (§22.10 `tokens`): the shared stylesheets' tokens, by name; omitted when empty.
    std::vector<TokenDef> tokens;
    /// T-170 (§22.10 `themes[]`): the bundle's themes; omitted when empty.
    std::vector<ThemeInfo> themes;
    /// T-179 (§22.10 `strings`): lang -> `strings/<lang>.csv`; omitted when empty.
    std::vector<StringsInfo> strings;
    /// T-179 (§22.10 `fonts.fallback`): lang -> families; omitted when empty.
    std::vector<FontFallbackInfo> fontFallbacks;
    /// T-183 (§22.10 `project`): written after `bundle` when set.
    bool hasProject = false;
    ProjectInfo project;
    /// T-207 (§22.10 `spec`): written after `project` when set.
    bool hasSpec = false;
    BundleSpec spec;
    /// T-193 (§22.10 `flows.global`): the global flow file, relative to the bundle root, written
    /// as `"flows": {"global": ...}` just before `motionSpec`; omitted when empty.
    std::string globalFlow;
    /// T-183 (§22.10 `motionSpec`): JSON object text, written last; omitted when empty.
    std::string motionSpecJson;
    /// T-200 (§22.10 `symbols[]`): written after `fonts`, before `flows`; omitted when empty.
    std::vector<SymbolContract> symbols;
    /// T-204 (§22.10 `glyphs[device]`): sorted by device then key, written after `fonts`, before
    /// `symbols`, as `{<device>: {<key>: <asset>}}`; omitted when empty.
    std::vector<GlyphInfo> glyphs;
};

/// Writes `<dir>/manifest.json`: §22.3's key order, 2-space indent, UTF-8, LF endings, trailing
/// newline. Returns kIoError when the file cannot be written.
Status writeManifest(std::string_view dir, const ManifestData& data);

/// Reads `<dir>/manifest.json` back into `out`. Returns false when it is missing or not a JSON
/// object; unknown or wrongly typed keys degrade to defaults, as in bundle.cpp.
bool readManifest(std::string_view dir, ManifestData& out);

}  // namespace xt::bundle
