#pragma once

#include <string>
#include <string_view>

/// Minimal sfnt `name` table reader, shared by FtTextShaper's deferred system-font registration
/// (Docs/tasks/T-104.md, Docs/design/23-host-integration.md §23.4) and the Figma importer's font
/// matcher (Code/Editor/Import/src/figma_fonts.cpp), which used to carry its own copy of this parser.

namespace xt::text {

/// The three `name` table entries font matching needs. `family`/`subfamily` prefer the
/// typographic names (nameID 16/17) over the legacy ones (nameID 1/2) when both are present, since
/// the legacy pair is restricted to the four Regular/Bold/Italic/BoldItalic subfamilies (Docs/tasks
/// /T-104.md "family nameID 1/16、subfamily 2/17、PostScript 6").
struct FontNameInfo {
    std::string family;
    std::string subfamily;
    std::string postScriptName;
};

/// Reads the sfnt table directory and `name` table of the TrueType/OpenType file at `path` --
/// only those two spans, never the rest of the file (glyph/outline data can be tens of megabytes;
/// Docs/tasks/T-104.md "只讀 table directory + name 表，不載整檔"). A `.ttc` collection reads its
/// first face's tables. Returns false if the file cannot be opened, is not sfnt/ttc, or the `name`
/// table has none of nameID 1/2/6/16/17. `path` is UTF-8 (T-261e; opened through the wide API on
/// Windows).
bool readFontNameTable(std::string_view path, FontNameInfo& out);

}  // namespace xt::text
