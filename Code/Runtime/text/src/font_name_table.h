#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// Minimal sfnt `name` table reader, shared by FtTextShaper's deferred system-font registration
/// (Docs/tasks/T-104.md, Docs/design/23-host-integration.md §23.4) and the Figma importer's font
/// matcher (Code/Editor/Import/src/figma_fonts.cpp), which used to carry its own copy of this parser.
/// T-569 (Docs/design/07-text.md §7.20): also the face count of a `.ttc`/`.otc` collection, the
/// OS/2 weight class, italic bit and CJK coverage bits, the `fvar` `wght` axis, and the "first CJK
/// family of a stack is not installed" rule the importer, the viewer and the packer report.
/// T-853 (07 §7.21.1-§7.21.2): the legacy family/subfamily (nameID 1/2), OS/2 usWidthClass, the
/// `fvar` `wdth` axis and the face width computed from them.

namespace xt::text {

/// The `name` table entries font matching needs, plus the OS/2 and `fvar` facts weight matching
/// uses. `family`/`subfamily` prefer the typographic names (nameID 16/17) over the legacy ones
/// (nameID 1/2) when both are present, since the legacy pair is restricted to the four
/// Regular/Bold/Italic/BoldItalic subfamilies (Docs/tasks/T-104.md "family nameID 1/16、subfamily
/// 2/17、PostScript 6"). Among several records of one nameID the US-English Windows one wins
/// (T-569: a CJK font's first record is often its localized name).
struct FontNameInfo {
    std::string family;
    std::string subfamily;
    std::string postScriptName;
    /// OS/2 usWeightClass (1..1000); 0 when the face has no OS/2 table.
    uint16_t weightClass = 0;
    /// OS/2 fsSelection ITALIC or OBLIQUE bit.
    bool italic = false;
    /// OS/2 ulUnicodeRange (CJK Unified Ideographs, Hiragana, Hangul Syllables) or ulCodePageRange
    /// (JIS, GB 2312, Wansung, Big5, Johab) says the face covers a CJK block.
    bool cjk = false;
    /// The face has an `fvar` `wght` axis; its range and default (design units == CSS weight).
    bool variableWeight = false;
    uint16_t weightMin = 0;
    uint16_t weightMax = 0;
    uint16_t weightDefault = 0;
    /// T-853 (07 §7.21.1-§7.21.2): the legacy names, nameID 1 and 2, as they are (empty when the
    /// face has none). `family`/`subfamily` above stay the typographic-first pair.
    std::string legacyFamily;
    std::string legacySubfamily;
    /// T-853: OS/2 usWidthClass (1..9 when valid); 0 when the face has no OS/2 table.
    uint16_t widthClass = 0;
    /// T-853: the face has an `fvar` `wdth` axis; its range and default in tenths of a percent
    /// (1000 = 100% = normal width).
    bool variableWidth = false;
    uint16_t widthMin = 0;
    uint16_t widthMax = 0;
    uint16_t widthDefault = 0;
};

/// T-853 (07 §7.21.1): the width words a face name can carry. kNarrow: Narrow, Condensed, Cond,
/// Compressed, Compact; kWide: Extended, Expanded, Wide -- each optionally prefixed by Semi, Extra
/// or Ultra, joined (`SemiCondensed`) or as a word of its own (`Semi Condensed`).
enum class WidthWord : uint8_t { kNone, kNarrow, kWide };

/// T-853: the width word in `name`, compared case-insensitively and as a whole word (words are
/// runs of ASCII letters and digits; `Condensed` in `Ultra-Condensed` counts, `Cond` in
/// `Second` or `Condor` does not). The first width word found decides; kNone when there is none.
WidthWord widthWordIn(std::string_view name);

/// T-853 (07 §7.21.1): the width of a face, in tenths of a percent (the CSS `font-stretch` scale
/// times 10: 500..2000, 1000 = normal). `min == max` for a static face.
struct FaceWidth {
    uint16_t min = 1000;
    uint16_t max = 1000;
    /// The width the face draws at without a request: the `wdth` axis default, or the static width.
    uint16_t def = 1000;
};

/// T-853 (07 §7.21.1): a variable face with a `wdth` axis covers the axis range. A static face is
/// as wide as its usWidthClass says (1 -> 50%, 2 -> 62.5% ... 5 -> 100% ... 9 -> 200%) only when
/// its subfamily (nameID 17, else 2) or its legacy family (nameID 1) carries a width word
/// (widthWordIn()); otherwise it is 100%. A width word with a class of 5 (or no valid class) gives
/// 75% for a narrow word and 125% for a wide one.
FaceWidth faceWidth(const FontNameInfo& info);

/// Number of faces in the font file at `path`: the `ttcf` header's count for a `.ttc`/`.otc`
/// collection, 1 for a single sfnt font, 0 when the file cannot be read or is neither.
uint32_t fontFaceCount(std::string_view path);

/// Reads the sfnt table directory, `name`, `OS/2` and `fvar` tables of face `faceIndex` of the
/// TrueType/OpenType file (or collection) at `path` -- only those spans, never the rest of the
/// file (glyph/outline data can be tens of megabytes; Docs/tasks/T-104.md "只讀 table directory +
/// name 表，不載整檔"). Returns false if the file cannot be opened, is not sfnt/ttc, `faceIndex` is
/// out of range, or the `name` table has none of nameID 1/2/6/16/17. `path` is UTF-8 (T-261e;
/// opened through the wide API on Windows). Names are decoded from UTF-16BE to UTF-8.
bool readFontNameTable(std::string_view path, FontNameInfo& out, uint32_t faceIndex = 0);

/// T-853: the same over a font file already in memory (FontTable::addFontMemory(), tests).
bool readFontNameTable(std::span<const uint8_t> bytes, FontNameInfo& out, uint32_t faceIndex = 0);

/// True for the CSS generic families (`serif`, `sans-serif`, `monospace`, `cursive`, `fantasy`,
/// `system-ui`, `ui-*`, `emoji`, `math`, `fangsong`), case-insensitively.
bool isGenericFamily(std::string_view family);

/// True when the family name itself says CJK (07 §7.20 "名稱表含 CJK"): a CJK character in the name,
/// or a well-known CJK family (Microsoft JhengHei, PingFang, Noto Sans CJK / TC / SC / JP / KR / HK,
/// Source Han, SimSun, MingLiU, Meiryo, Malgun Gothic...).
bool isCjkFamilyName(std::string_view family);

/// Whether a family of a stack is installed, and whether the installed face covers CJK.
enum class FamilyPresence : uint8_t { kAbsent, kInstalled, kInstalledCjk };

/// 07 §7.20: the first CJK family of `stack` (generic families skipped). An installed family is
/// CJK when its face covers a CJK block (kInstalledCjk) or its name says so; an absent one when its
/// name says so. Returns that family when it is not installed, or "" when the first CJK family is
/// installed or the stack has none. `presence(family)` answers for one family name.
template <typename Presence>
std::string firstMissingCjkFamily(const std::vector<std::string>& stack, Presence&& presence) {
    for (const std::string& family : stack) {
        if (family.empty() || isGenericFamily(family)) continue;
        const FamilyPresence state = presence(std::string_view(family));
        const bool cjk = state == FamilyPresence::kInstalledCjk || isCjkFamilyName(family);
        if (!cjk) continue;
        return state == FamilyPresence::kAbsent ? family : std::string();
    }
    return std::string();
}

/// Splits a CSS `font-family` value (`'Microsoft JhengHei', "Segoe UI", sans-serif`) into its
/// family names, quotes removed and whitespace trimmed.
std::vector<std::string> splitFontFamilyList(std::string_view value);

}  // namespace xt::text
