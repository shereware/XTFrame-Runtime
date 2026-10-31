#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xt/bundle/bundle.h"  // SymbolContract (T-200).
#include "xt/context.h"        // TokenDef (T-170).
#include "xt/types.h"

/// Internal helpers shared by project_file.cpp and bundle.cpp: type-checked JSON accessors (a
/// wrongly typed key reads as the default instead of throwing), plus the tiny file I/O both
/// readers need. Not a public header; nlohmann never reaches include/xt/bundle/.

namespace xt::bundle::detail {

using json = nlohmann::json;

/// T-262c: the deepest JSON nesting a reader accepts (the root object or array is level 1).
inline constexpr int kMaxJsonDepth = 32;

/// T-262c: `json::parse(text)` through nlohmann's callback parser, refusing every object or array
/// deeper than `maxDepth` before it is built (nlohmann's copy constructor and dump() recurse).
/// Returns a discarded value when `text` is not JSON (`error` empty) or nests deeper than
/// `maxDepth` (`error` = "nesting too deep"; the caller names the file).
json parseBounded(std::string_view text, int maxDepth, std::string& error);

/// T-500: fopen for a UTF-8 `path` (the wide API on Windows, never the ANSI code page); nullptr
/// when it cannot be opened. The caller closes it. zip_archive.cpp streams archives through it.
std::FILE* openFileUtf8(const std::string& path, const char* mode);
/// Reads a whole file into `out`. Returns false when it cannot be opened or read.
bool readFile(const std::string& path, std::string& out);
/// Writes `text` verbatim (binary mode, so no CRLF translation). Returns false on failure.
/// T-262b: atomic -- written to `<path>.tmp` and moved over `path` (MoveFileExW / rename); on any
/// failure the temporary file is removed and `path` is left untouched.
bool writeFile(const std::string& path, const std::string& text);

/// T-241 / T-382 (23 §23.11.2): the standard-library copy of symbol file `fileName` -- the
/// library root first, then its `mobile/` and `list/` subsets; empty when none is a regular file.
/// Shared by pack() and pullStandardSymbols() (T-414).
std::filesystem::path findStandardSymbol(const std::filesystem::path& library,
                                         const std::filesystem::path& fileName);

/// Joins `dir` and `name` with '/', tolerating an empty or already-slash-terminated `dir`.
std::string joinPath(std::string_view dir, std::string_view name);

/// `obj[key]` as a string, or "" when absent or not a string.
std::string getString(const json& obj, const char* key);
/// `obj[key]` as a bool, or `fallback` when absent or not a bool.
bool getBool(const json& obj, const char* key, bool fallback);
/// `obj[key]` as a float, or `fallback` when absent or not a number.
float getFloat(const json& obj, const char* key, float fallback);
/// `obj[key]` as a two-element `[x, y]` number array, or `fallback` otherwise.
Vec2 getVec2(const json& obj, const char* key, Vec2 fallback);
/// `value` as an integer when integral, else the float's shortest round-trip spelling (0.8f is
/// written as 0.8, not as the widened double). T-414: also what manifest_writer.cpp and
/// ui_project.cpp write.
nlohmann::ordered_json shortNumber(float value);
/// T-258a: the largest UI `size` component, px.
inline constexpr float kMaxUiSize = 16384.0f;
/// T-258a: the largest `publish.thumbnails` component, px.
inline constexpr float kMaxThumbnailSize = 1024.0f;
/// T-258a: true when both components of a UI `size` are finite and in [1, kMaxUiSize]; the
/// project readers reject anything else.
bool isValidUiSize(Vec2 size);
/// T-258a: `publish.thumbnails` clamped to [0, kMaxThumbnailSize] per component; a non-finite
/// component takes `fallback`'s.
Vec2 clampThumbnail(Vec2 size, Vec2 fallback);
/// T-345 (25 §25.4.2): reads `spec.uiscaleClamp` ([min, max], max `null` = none; anything else,
/// a negative min or max < min = no clamp) and `spec.uiscaleDpiFloor` (a finite number >= 0, else
/// 0) from the spec object `spec` into `out`.
void readUiScaleLimits(const json& spec, UiScaleLimits& out);
/// T-345: writes `limits` into the spec object `spec`: `uiscaleClamp` only when clamped,
/// `uiscaleDpiFloor` only when positive.
void writeUiScaleLimits(nlohmann::ordered_json& spec, const UiScaleLimits& limits);

/// `obj[key]` as an array of strings; non-string entries are skipped. Empty when absent.
std::vector<std::string> getStringArray(const json& obj, const char* key);
/// `obj[key]` as a uint64: an unsigned number, or a string parsed by strtoull (base 0, so "0x.."
/// works). 0 when absent or unparsable.
uint64_t getUint64(const json& obj, const char* key);

/// T-170 (§22.10): `root["tokens"]` (an object of `--name` -> value text) as TokenDefs, in the
/// object's order; non-string values are skipped. Empty when absent.
std::vector<TokenDef> getTokens(const json& root);

/// T-170 (§22.10): one `root["themes"]` entry, `{ "name", "file" }`.
struct ThemeEntry {
    std::string name;
    std::string file;
};
/// `root["themes"]` as ThemeEntries; entries without a name or file are skipped.
std::vector<ThemeEntry> getThemes(const json& root);

/// T-179 (§22.10): `root["strings"]` (an object of lang -> file) in the object's order; non-string
/// values and empty names are skipped.
std::vector<std::pair<std::string, std::string>> getStringsMap(const json& root);
/// T-179 (§22.10): `root["fonts"]["fallback"]` (an object of lang -> family array) in the object's
/// order; empty chains are skipped.
std::vector<std::pair<std::string, std::vector<std::string>>> getFontFallbacks(const json& root);

/// T-204 (§22.10): `root["glyphs"]` (an object of device -> {key: asset}) as GlyphInfo rows,
/// sorted by device then key; non-string or empty values are skipped. Empty when absent.
std::vector<GlyphInfo> getGlyphs(const json& root);

/// T-207 (§22.10 `spec`): `root["spec"]` into `out` (missing members keep their defaults); false
/// when the manifest has no `spec` object.
bool getSpec(const json& root, BundleSpec& out);

/// T-200 (§22.10): `root["symbols"]` (an array of contract objects) as SymbolContracts; entries
/// without a name are skipped. Empty when absent.
std::vector<SymbolContract> getSymbolContracts(const json& root);

/// True when `id` is non-empty and matches `[a-z0-9-]+` (§22.2's stable-id rule).
bool isValidId(std::string_view id);

/// T-199 (§22.10 `atlas[]`): one page as the manifest spells it.
struct AtlasEntryJson {
    std::string asset;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};
struct AtlasJson {
    std::string name;
    std::string file;
    std::string file2x;
    std::string format;
    int w = 0;
    int h = 0;
    std::vector<AtlasEntryJson> entries;
};
/// `root.atlas[]`; empty when absent. Malformed rows are skipped.
std::vector<AtlasJson> getAtlases(const json& root);

}  // namespace xt::bundle::detail
