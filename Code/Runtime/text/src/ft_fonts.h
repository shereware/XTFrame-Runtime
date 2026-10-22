#pragma once

#include <ft2build.h>
#include FT_FREETYPE_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xt/text_shaper.h"
#include "xt/types.h"

/// FT_Library/FT_Face management: font registration, weight/italic matching, per-codepoint
/// coverage queries. See Docs/tasks/T-29.md "FtTextShaper", Docs/design/07-text.md §7.3
/// "匹配".

namespace xt::text {

/// One registered font: an FT_Face plus (if loaded from addFontMemory) the memory buffer FT_Face
/// points into, which must outlive the face. A font registered via addFontFileDeferred()
/// (addFontDir/addSystemFonts, Docs/tasks/T-104.md) starts with `face == nullptr` and `deferred
/// == true`: `family`/`subfamily`/`postScriptName`/`weight`/`italic` come from a `name`-table scan
/// only (font_name_table.h), and `face` is opened from `path` the first time it is actually needed
/// (FontTable::face()).
struct RegisteredFont {
    FT_Face face = nullptr;
    std::vector<uint8_t> memory;
    std::string family;
    std::string subfamily;
    std::string postScriptName;
    uint16_t weight = 400;
    bool italic = false;
    /// True until FontTable::face() opens `face` from `path` on first use.
    bool deferred = false;
    /// True once opening a deferred font has failed; the entry stays in place (FontId is a stable
    /// index) but is skipped by matching/fallback and its FontId maps to a null face from then on
    /// (Docs/tasks/T-104.md "失敗則移除並警告一次" -- removed from `fallback_`, not erased here).
    bool broken = false;
    /// File path a deferred font was registered from; kept after the face is opened (reported by
    /// FontTable::fontPath(), T-429). Empty for addFontFile()/addFontMemory() fonts.
    std::string path;
    /// Sub-face index within `path`; always 0 -- only a `.ttc`'s first face is read
    /// (Docs/tasks/T-104.md).
    long faceIndex = 0;
};

/// Owns the shared FT_Library and the registered-font list. Not internally synchronized;
/// FtTextShaper serializes all access with its own mutex (§7.3 "執行緒安全").
class FontTable {
public:
    FontTable();
    ~FontTable();
    FontTable(const FontTable&) = delete;
    FontTable& operator=(const FontTable&) = delete;

    bool addFontFile(std::string_view path);
    bool addFontMemory(std::vector<uint8_t> bytes);
    /// Registers a font from `path` without opening it: only the sfnt `name` table is read
    /// (font_name_table.h). Used by addFontDir/addSystemFonts (Docs/tasks/T-104.md); addFontFile()
    /// stays eager for explicit single-file registration. Returns false if `path` has no readable
    /// `name` table.
    bool addFontFileDeferred(std::string_view path);
    void addFallbackFont(FontId id);
    uint32_t fontCount() const { return static_cast<uint32_t>(fonts_.size()); }
    /// Number of deferred fonts actually opened so far (Docs/tasks/T-104.md's perf metric).
    uint32_t openedFontCount() const { return openedCount_; }
    /// Appends every registered font's `FontDesc` (id, family, weight, italic, whether it is a
    /// fallback candidate) to `out`; see `TextShaper::fonts()`.
    void fonts(std::vector<FontDesc>& out) const;
    /// The file path font `id` was registered from (UTF-8); empty for an addFontMemory() font or
    /// an out-of-range id (T-429).
    std::string fontPath(FontId id) const;

    /// CSS Fonts Level 4 style family/weight/italic match. No family in `request.families`
    /// matches a registered family name -> first registered font. No fonts registered ->
    /// kInvalidFont.
    FontId match(const FontRequest& request) const;
    FontMetrics metrics(FontId font) const;
    FT_Face face(FontId font) const;

    /// Builds the candidate list for a shape() run's font-fallback segmentation: the best match
    /// within each of `request.families` (family names that exist), in order, followed by the
    /// fallback list (registration order). Returns the first candidate whose face has a glyph
    /// for `codepoint` (FT_Get_Char_Index != 0), or `match(request)` if none does.
    FontId fontForCodepoint(const FontRequest& request, uint32_t codepoint) const;

private:
    FT_Library library_ = nullptr;
    // mutable: face()/metrics()/match()/fontForCodepoint() stay logically const (they don't change
    // what the table reports, only whether a deferred font has been opened yet) while opening a
    // deferred FT_Face on first use (Docs/tasks/T-104.md).
    mutable std::vector<RegisteredFont> fonts_;
    mutable std::vector<FontId> fallback_;
    mutable uint32_t openedCount_ = 0;

    bool loadFace(std::vector<uint8_t> memory, RegisteredFont& out) const;
    // Best-matching registered font whose family name equals `family` (case-insensitive), or
    // kInvalidFont if no font has that family name. Skips broken entries.
    FontId matchFamily(std::string_view family, uint16_t weight, bool italic) const;
    // Opens fonts_[index].face if still deferred, updating openedCount_/broken as it goes. Returns
    // the (possibly null, if opening failed) face.
    FT_Face openDeferred(size_t index) const;
};

}  // namespace xt::text
