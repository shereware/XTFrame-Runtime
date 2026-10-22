#pragma once

#include <array>
#include <cstdint>
#include <json.hpp>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xt/bundle/design_system.h"
#include "xt/bundle/ui_project.h"
#include "xt/types.h"

/// Asset identity and the import pipeline (Docs/design/35-design-system.md §35.15.1, 40 §40.2.1,
/// 26 §26.3.3 / §26.8; Docs/tasks/T-312a.md): one `<file>.meta` per asset, the collections (one
/// folder each), importing / replacing / deduplicating, linked source folders, `manifest/assets.yaml`
/// generated from the metas, the cleanup numbers and promoting a reference to a placeholder asset.
///
/// Paths: `relFile` arguments and results are relative to the project root, '/' separators. An
/// asset lives at `<assetRoot>/<type dir>/<collection>/<file>`; the type directories are `img`
/// (image, vector), `fonts`, `anim`, `video` and `fx` (effect), the same as the editor's asset
/// import (assetImportPath()). A collection may have several segments (`hud/frames`); an asset
/// directly in its type directory has the empty collection.
///
/// Derived files (`.xtcache/`, 40 §40.2.3) are not produced here; the meta only has the fields.

namespace xt::bundle::assets {

/// The asset statuses (§35.15: 26 §26.3.3's four plus `placeholder`).
inline constexpr std::array<std::string_view, 5> kStatuses = {"draft", "approved", "stale", "pending",
                                                              "placeholder"};
/// True when `status` is one of kStatuses.
bool isStatus(std::string_view status);

/// `<file>.meta` of an asset (40 §40.2.1). Written as YAML in the fixed key order id, type, title,
/// collection, status, source, approved, license, content, tags, measured, import, links, note,
/// then the unknown keys alphabetically. Empty strings, lists and mappings are not written, except
/// `status`, which always is.
struct AssetMeta {
    /// GUID; never changes (replaceAsset() keeps it).
    std::string id;
    /// image | vector | font | anim | video | effect (typeForExtension()).
    std::string type;
    std::string title;
    /// The collection the file lies in (collectionOf()); informative, the folder is authoritative.
    std::string collection;
    /// Meta `content`: `sha256:<hex>` of the file.
    std::string contentHash;
    /// One of kStatuses.
    std::string status = "draft";
    std::string license;
    design::Source source;
    design::Approval approved;
    std::vector<std::string> tags;
    /// `w`, `h`, `alpha` measured from the file (PNG / JPEG / SVG); a `slice` read from a meta is
    /// kept, but the nine-slice margins are an import setting (`import.slice`).
    AssetMeasured measured;
    /// Import settings with the keys of 26 §26.8: `slice: [L, R, T, B]`, `scale2x: true`,
    /// `compress: bc7|bc3|bc1|none`, ...; null when there are none.
    nlohmann::json import;
    std::vector<design::Link> links;
    std::string note;
    /// Unknown keys (an object), written back after the known ones; null when there are none.
    nlohmann::json extra;
};

/// The YAML text writeMeta() writes for `meta` (byte-stable, LF).
std::string metaText(const AssetMeta& meta);
/// Parses a meta text. false (with `*error`) when it is not YAML or not a mapping.
bool parseMeta(std::string_view text, AssetMeta& out, std::string* error = nullptr);
/// Reads `<filePath>.meta` (`filePath` is the asset file, UTF-8). kNotFound when there is no meta,
/// kInvalidArgument when it is malformed.
Status readMeta(std::string_view filePath, AssetMeta& out, std::string* error = nullptr);
/// Writes `<filePath>.meta` (only when its bytes differ). kIoError when it cannot be written.
Status writeMeta(std::string_view filePath, const AssetMeta& meta, std::string* error = nullptr);

/// The asset type of a file extension (any case, with the dot): png / jpg / jpeg / webp -> image,
/// svg -> vector, ttf / otf / ttc / woff / woff2 -> font, json / lottie -> anim, mp4 / webm / mov /
/// ogv / mkv / avi / bik -> video, xtfx / xtfxp -> effect; "" otherwise.
std::string typeForExtension(std::string_view ext);
/// The type directory of an asset type: img, fonts, anim, video or fx (an unknown type -> img).
std::string typeDirectory(std::string_view type);
/// The R-layer copy of the editor's kebabAssetId(): ASCII letters lowercased, ASCII digits kept,
/// UTF-8 bytes (>= 0x80) kept, every other ASCII run one '-' (none at either end).
std::string kebabAssetId(std::string_view stem);

/// The collection of the asset `relFile`: the directories between its type directory and the file
/// (`shared/img/icons/a.png` -> `icons`); "" directly in the type directory or outside
/// `<assetRoot>/`.
std::string collectionOf(const UiProject& project, std::string_view relFile);
/// Where an import of `fileName` into `collection` lands: `<assetRoot>/<typeDirectory(type)>/
/// <collection>/<name>` (no collection segment for ""), `<name>` being the stem made
/// kebabAssetId() ("asset" when nothing is left; a trailing `@2x` kept) and the extension
/// lowercased. "" when `collection` is not a plain relative path (an empty, `.` or `..` segment, a
/// '\\' or ':', a leading '/').
std::string importPathFor(const UiProject& project, std::string_view type, std::string_view collection,
                          std::string_view fileName);

/// True when the project has been given asset identity: its design directory exists or a `.meta`
/// lies under `<assetRoot>/`. Only then does xtframe-pack regenerate `manifest/assets.yaml`.
bool assetsUpgraded(const UiProject& project);

/// Returns the meta of `relFile`, creating it when missing: a new GUID, the type from the
/// extension, the collection from the folder, the content hash, the measured size / alpha, `source:
/// { kind: manual }`, status draft. An existing meta whose content hash no longer matches the file
/// gets the new hash and measurements written back (identity, status and settings unchanged).
/// `*created` (may be null) says a meta was written for the first time. kInvalidArgument for an
/// unnamed project, kNotFound when the file does not exist, kInvalidArgument for a malformed meta,
/// kIoError when the meta cannot be written.
Status ensureMeta(const UiProject& project, std::string_view relFile, AssetMeta& meta, bool* created);

/// One import (§35.15.1 item 3).
struct ImportRequest {
    /// The file to import (UTF-8; absolute, or relative to the working directory).
    std::string sourcePath;
    std::string collection;
    std::string title;
    std::vector<std::string> tags;
    std::string status = "draft";
    /// Import settings (AssetMeta::import); `scale2x` is added from the file name.
    nlohmann::json import;
    /// Empty kind = `manual`.
    design::Source source;
};

struct ImportResult {
    /// The asset, relative to the project root (the existing one when deduplicated).
    std::string relFile;
    AssetMeta meta;
    /// The same content already was in the project: nothing was copied.
    bool deduplicated = false;
    /// The asset with the same content (= relFile) when deduplicated.
    std::string duplicateOf;
    std::vector<std::string> warnings;
};

/// Imports one file: its content hash is looked up among the project's assets first (same content
/// -> `deduplicated`, `duplicateOf` = that asset, nothing copied, a warning); otherwise it is copied
/// to importPathFor() (an existing name gets `-2`, `-3`, ... before the extension and any `@2x`),
/// `import.scale2x: true` is set when its stem ends with `@2x`, it is measured and its meta written.
/// kInvalidArgument for an unnamed project, a bad collection or status; kNotFound when the source
/// cannot be read; kUnsupported for an extension typeForExtension() does not know; kIoError when a
/// file cannot be written.
Status importAsset(const UiProject& project, const ImportRequest& request, ImportResult& result);
/// importAsset() for each request, in order (an earlier import counts for the deduplication of a
/// later one). Stops at the first failure (its status is returned; `results` holds the ones done).
Status importAssets(const UiProject& project, const std::vector<ImportRequest>& requests,
                    std::vector<ImportResult>& results);

/// §35.15.1 item 4: replaces the content of `relFile` by `newSourcePath`, keeping its identity (id,
/// path, title, collection, tags, settings): the hash and measurements are updated, `source.at`
/// becomes the current UTC time (ISO 8601) and the status draft. No copy of the old file is kept
/// (history is chapter 36's). kInvalidArgument for an unnamed project or a source with another
/// extension, kNotFound when either file is missing, kIoError when a file cannot be written.
Status replaceAsset(const UiProject& project, std::string_view relFile, std::string_view newSourcePath,
                    AssetMeta& meta);

/// A linked source folder (`.xtui:assets.sourceFolders[]`, sourceFolders()).
struct SourceFolder {
    /// Absolute directory, '/' separators.
    std::string path;
    std::string collection;
    /// Import settings given to every file imported from it; null when none.
    nlohmann::json import;
};
/// The project's source folders, parsed from UiProject::assets (an `import` that is not JSON reads
/// as null).
std::vector<SourceFolder> sourceFolders(const UiProject& project);

/// What syncSourceFolders() did; every list holds project-relative asset paths, sorted.
struct SyncReport {
    std::vector<std::string> imported;
    /// Content changed: replaced and marked `stale`.
    std::vector<std::string> replaced;
    std::vector<std::string> unchanged;
    /// Assets imported from a folder whose file is gone (never deleted).
    std::vector<std::string> missing;
    std::vector<std::string> warnings;
};
/// Scans every source folder (its own files, not subdirectories, in name order) for files with an
/// asset extension and matches each by name to importPathFor(type, collection, name): a new name is
/// imported (`source: { kind: import:folder, ref: <absolute source file> }`), a changed one (other
/// hash) replaced and set to `stale`, an equal one left alone. An asset of the folder's collection
/// whose meta has `source.kind import:folder` and a `ref` inside that folder that no longer exists
/// is listed in `missing`. Deterministic; a second call with nothing changed reports everything
/// unchanged. A folder that does not exist is a warning. kInvalidArgument for an unnamed project or
/// a bad collection, else the first failing import's status.
Status syncSourceFolders(const UiProject& project, SyncReport& report);

/// What regenerateAssetsManifest() did.
struct RegenerateReport {
    /// `manifest/assets.yaml` was written (its bytes changed).
    bool written = false;
    /// Metas created on the way.
    int metasCreated = 0;
    std::vector<std::string> warnings;
};
/// §35.15.1 item 1 / 40 §40.2.1: builds `manifest/assets.yaml` from the metas. Every asset file
/// under `<assetRoot>/` (img: png jpg jpeg svg; fonts: ttf otf ttc woff woff2; anim: json lottie;
/// video: mp4 webm mov ogv mkv avi bik; fx: xtfx xtfxp -- the editor's asset list; an `@2x` file
/// next to its 1x file is a variant, not an asset) gets ensureMeta() and one entry: `type`,
/// `source` (= `source.kind`, `manual` when empty), `status`, `license`, `measured` (w, h, alpha
/// from the meta, `slice` from `import.slice`) and `compress` (from `import.compress`) from the
/// meta. An entry of the existing manifest for the same file keeps its `id` and its other keys; a
/// new entry's id is the file stem (made unique with the collection, then `-2`, ...). A meta created
/// for a file the existing manifest lists is first seeded from that entry (source, status, license,
/// slice, compress). Entries of the existing manifest whose file the scan does not produce stay as
/// they are, after the generated ones. The file is written with AssetsManifest::save() only when its
/// bytes change, and not at all (a warning) when the existing one has content save() cannot
/// reproduce. `out` receives the manifest. kInvalidArgument for an unnamed project or a malformed
/// existing manifest (`*error`), ensureMeta()'s failures, kIoError when it cannot be written.
Status regenerateAssetsManifest(const UiProject& project, AssetsManifest& out,
                                RegenerateReport* report = nullptr, std::string* error = nullptr);

/// One collection's numbers (§35.15.3 "集合清單").
struct CollectionStat {
    /// The type directory (img, fonts, ...).
    std::string typeDir;
    /// "" for the files directly in the type directory.
    std::string collection;
    int assets = 0;
    uint64_t bytes = 0;
};

/// §35.15.1 item 5: the cleanup numbers.
struct AssetHygiene {
    int unused = 0;
    int duplicates = 0;
    int oversize = 0;
    int missing2x = 0;
    int badSlice = 0;
    int badName = 0;
    int placeholders = 0;
    /// (first, duplicate) by content hash, sorted.
    std::vector<std::pair<std::string, std::string>> duplicatePairs;
    std::vector<std::string> unusedFiles, oversizeFiles, placeholderFiles;
    /// UI id -> bytes of the assets its files name.
    std::map<std::string, uint64_t> bytesPerUi;
    /// T-312a addition (for the document's section 13): every collection, sorted by type
    /// directory then collection, and the totals.
    std::vector<CollectionStat> collections;
    int totalAssets = 0;
    uint64_t totalBytes = 0;
};
/// Computes the cleanup numbers without writing anything (a missing meta is computed in memory):
/// unused = the file name occurs in no `.xt` / `.xts` file of the project (T-309's rule; with a
/// `graph`, a node or edge naming the asset path or GUID counts as a use too); duplicates = same
/// content hash; oversize = larger than `spec.budget.assetBytes` (0 = not checked); missing2x = a
/// bitmap without `@2x` in a collection where some bitmap has one; badSlice = checkAssets()'s
/// nine-slice errors; badName = a stem (without `@2x`) that kebabAssetId() changes; placeholders =
/// status `placeholder`. bytesPerUi: the assets named by a UI's document and the `.xt` / `.xts`
/// files under its directory. kInvalidArgument for an unnamed project.
Status assetHygiene(const UiProject& project, const design::Graph* graph, AssetHygiene& out);

/// §35.15 / §35.3.1: copies the reference `refId` (`moodboard/key-art-01`, or `ref:moodboard/...`)
/// into `collection` as an asset with status `placeholder`, `links: [{derived-from, ref:<id>}]`, the
/// reference's license and title. With `cropRect` ({x, y, w, h} in fractions of the picture, as an
/// annotation's rect) a PNG or JPEG reference is decoded (sw::decodeImageBytes), cut and written as
/// PNG (`<stem>.png`, stb_image_write through sw::encodePng); any other reference is copied whole
/// with a warning. Deduplication as importAsset() (an existing asset is never made a placeholder).
/// kNotFound when the reference or its file is missing, kInvalidArgument for an empty crop or a bad
/// collection, kUnsupported for a file type that is not an asset type, else importAsset()'s.
Status promoteReference(const UiProject& project, std::string_view refId,
                        const std::array<float, 4>* cropRect, std::string_view collection,
                        ImportResult& result);

}  // namespace xt::bundle::assets
