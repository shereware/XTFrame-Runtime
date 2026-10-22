#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "xt/text_shaper.h"
#include "xt/types.h"

/// FreeType + HarfBuzz + SheenBidi + libunibreak based TextShaper. See Docs/tasks/T-29.md,
/// Docs/design/07-text.md §7.3.

namespace xt::text {

/// TextShaper implementation built on FreeType (outlines/bitmaps), HarfBuzz (shaping), SheenBidi
/// (bidirectional analysis), and libunibreak (line/grapheme breaking).
class FtTextShaper : public TextShaper {
public:
    /// Initializes the FreeType library and internal state; no fonts are registered yet.
    FtTextShaper();
    /// Destroys the shaper, releasing every registered FT_Face and the FreeType library handle.
    ~FtTextShaper() override;
    /// Non-copyable (owns FreeType/HarfBuzz handles).
    FtTextShaper(const FtTextShaper&) = delete;
    /// Non-copyable (owns FreeType/HarfBuzz handles).
    FtTextShaper& operator=(const FtTextShaper&) = delete;

    /// Registers a font from a file path. Returns false if the file cannot be read or FreeType
    /// cannot parse it. Paths here (and addFontDir()'s `dir`) are UTF-8 (T-261e), opened through
    /// the wide API on Windows; a path that is not valid UTF-8 is treated as missing (T-261f).
    bool addFontFile(std::string_view path);
    /// Registers a font from an in-memory buffer (which this call takes ownership of; the
    /// FT_Face keeps pointing into it for its lifetime). Returns false if FreeType cannot parse
    /// it. Overrides TextShaper's defaulted hook so Document can register `@font-face` assets
    /// without knowing this concrete type (Docs/tasks/T-30.md "字型資產"). The font also joins the
    /// fallback candidate list (07.3 step 3, Docs/tasks/T-35.md "附帶修正"), unlike addFontFile().
    bool addFontMemory(std::vector<uint8_t> bytes) override;

    /// Registers `.ttf`/`.otf` files found directly inside `dir` (non-recursive, in directory
    /// order) until `maxFiles` of them have been registered successfully. Returns how many were
    /// registered; a missing/unreadable directory registers nothing and returns 0. Used by hosts to pick up
    /// the system font directories (Docs/design/12-compiler.md §12.6). Each font is registered
    /// lazily (Docs/tasks/T-104.md): only its `name` table is read here; FreeType opens it the
    /// first time match() or shape()'s font-fallback segmentation actually needs it.
    uint32_t addFontDir(std::string_view dir, uint32_t maxFiles);
    /// Default budget for addSystemFonts(). Directory order is alphabetical, so a small budget
    /// would miss late names such as segoeui.ttf; scanning a whole system directory only reads
    /// each file's `name` table (T-104), so it stays cheap even at this size.
    static constexpr uint32_t kMaxSystemFonts = 2000;
    /// Registers the platform's system font directories through addFontDir() -- C:/Windows/Fonts
    /// on Windows, /System/Library/Fonts and /Library/Fonts on macOS, /usr/share/fonts and
    /// /usr/local/share/fonts elsewhere -- until `maxFiles` fonts have been registered in total.
    /// Returns how many were registered; missing directories are skipped. Hosts that ship their
    /// own fonts can use addFontMemory() instead (Docs/design/23-host-integration.md §23.4).
    uint32_t addSystemFonts(uint32_t maxFiles = kMaxSystemFonts);
    /// Appends `id` to the system-fallback candidate list used by shape()'s font-fallback
    /// segmentation (Docs/design/07-text.md §7.3 step 3), in call order.
    void addFallbackFont(FontId id);
    /// Number of fonts registered so far (via addFontFile/addFontMemory/addFontDir), including
    /// ones still deferred (not yet opened).
    uint32_t fontCount() const;
    /// Number of registered fonts whose FT_Face has actually been opened so far -- i.e. `fontCount()`
    /// minus the ones still deferred (Docs/tasks/T-104.md).
    uint32_t openedFontCount() const;
    /// Every registered font's `FontDesc`, appended to `out`; see `TextShaper::fonts()`.
    void fonts(std::vector<FontDesc>& out) const override;
    /// The file path font `id` was registered from by addFontDir()/addSystemFonts() (UTF-8);
    /// empty for a font registered from memory or a single addFontFile() call, or an unknown id.
    /// Tooling only (T-429: `xtframe-view --list-fonts`).
    std::string fontPath(FontId id) const;

    /// Finds the best registered font for `request`, falling back through the system-fallback
    /// candidate list (Docs/design/07-text.md §7.3 step 3); returns kInvalidFont only if no font
    /// is registered at all.
    FontId match(const FontRequest& request) override;
    /// Metrics for a previously matched font, read from its FreeType face (1-em units).
    FontMetrics metrics(FontId font) override;
    /// Shapes `utf8` per §7.3: bidi, script segmentation, font-fallback segmentation, HarfBuzz
    /// shaping, line-break opportunities, and grapheme boundaries, writing the result into `out`.
    void shape(std::string_view utf8, const ShapeParams& params, ShapeResult& out) override;
    /// Loads `glyphId`'s outline from FreeType and converts it to `outVerbs`/`outPoints` in em
    /// units. Returns false if the glyph has no outline.
    bool glyphOutline(FontId font, uint32_t glyphId, std::vector<PathVerb>& outVerbs,
                      std::vector<float>& outPoints) override;
    /// Rasterizes `glyphId` at `size` px via FreeType into `out`, with the glyph outline shifted
    /// right by `subpixelX` px (quantized to 0/0.25/0.5/0.75; cached per variant). Returns false
    /// if the glyph cannot be rasterized.
    bool glyphBitmap(FontId font, uint32_t glyphId, float size, float subpixelX, GlyphBitmap& out) override;
    using TextShaper::glyphBitmap;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace xt::text
