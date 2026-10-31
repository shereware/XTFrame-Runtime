#pragma once

#include <cstdint>
#include <string>
#include <vector>

/// Font subsetting for pack() (T-199, Docs/design/26-layers-symbols-library.md §26.3.4 "字型打包",
/// 12 §12.7 `--font-subset`): a hand-written TrueType (`glyf`) subsetter -- no third party. Glyph
/// ids are kept as they are (so `GSUB`/`GPOS`/`kern`/`hmtx`/`post` stay valid untouched): only the
/// outlines of unused glyphs are dropped from `glyf`/`loca`, and `cmap` is rebuilt with the kept
/// code points. A CFF font (`OTTO`) is copied unchanged (the sheet's rule). Internal to xt_bundle.

namespace xt::bundle {

/// A parsed font file.
class FontFile {
public:
    /// Parses the sfnt directory, `cmap`, `head`, `maxp`, `loca` and `glyf` (the last two for
    /// TrueType only). False when the bytes are not an sfnt with a readable cmap.
    bool parse(const std::vector<uint8_t>& bytes);
    /// True for a CFF-flavoured OpenType font (`OTTO`): coverage works, subset() copies.
    bool isCff() const { return m_cff; }
    /// True when the font maps `codePoint` to a glyph other than .notdef.
    bool hasGlyph(uint32_t codePoint) const;
    /// T-527b (22 §22.10.3 standard fonts): the family name from the `name` table -- nameID 16
    /// (typographic family) when present, else nameID 1 -- preferring a Windows/Unicode record
    /// (UTF-16BE, high bytes dropped: the names compared are ASCII) over a Mac one. Empty when the
    /// font has no readable `name` table or neither record.
    std::string familyName() const;
    /// Number of glyphs (`maxp.numGlyphs`).
    uint16_t glyphCount() const { return m_numGlyphs; }
    /// Number of glyphs with an outline (non-empty `glyf` entry); 0 for CFF.
    uint32_t outlineCount() const;
    /// Writes the font keeping the outlines of .notdef and of every glyph `codePoints` reach
    /// (composite components included) and a `cmap` of those code points only. A CFF font (or a
    /// font whose tables could not be parsed) is written unchanged. False when nothing was
    /// parsed.
    bool subset(const std::vector<uint32_t>& codePoints, std::vector<uint8_t>& out) const;

private:
    struct Table {
        uint32_t tag = 0;
        uint32_t offset = 0;
        uint32_t length = 0;
    };
    const Table* find(uint32_t tag) const;
    bool parseCmap(const Table& cmap);
    void collectComponents(uint16_t gid, std::vector<uint8_t>& keep) const;

    std::vector<uint8_t> m_bytes;
    std::vector<Table> m_tables;
    bool m_cff = false;
    bool m_parsed = false;
    uint16_t m_numGlyphs = 0;
    bool m_longLoca = false;
    std::vector<uint32_t> m_loca;  // numGlyphs + 1 entries (TrueType only).
    uint32_t m_glyfOffset = 0;
    uint32_t m_glyfLength = 0;
    /// code point -> glyph id, sorted by code point.
    std::vector<std::pair<uint32_t, uint16_t>> m_cmap;
};

}  // namespace xt::bundle
