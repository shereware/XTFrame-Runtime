#include "xt/text/ft_text_shaper.h"

#include <ft2build.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>
#include FT_FREETYPE_H
#include <hb-ft.h>
#include <hb.h>

#include "ft_fonts.h"
#include "vo_table.h"
#include "xt/node.h"

/// FtTextShaper::shape() pipeline (docs/design/07-text.md §7.3) and the class's other TextShaper
/// methods (match/metrics/glyphOutline/glyphBitmap). Font management is ft_fonts.h/.cpp; the
/// per-subsystem wrappers below (bidi/breaks/script/glyph raster) are free functions defined in
/// ft_bidi.cpp / ft_breaks.cpp / ft_script.cpp / ft_glyphs.cpp -- declared here rather than via a
/// shared internal header, per docs/tasks/T-29.md's file list (no files beyond the sheet).

namespace xt::text {

// ft_bidi.cpp
std::vector<uint8_t> computeBidiLevels(std::string_view utf8, TextDirection paragraphDirection);
// ft_breaks.cpp
void computeBreaks(std::string_view utf8, std::string_view lang, std::vector<uint8_t>& breakAfter);
void computeGraphemes(std::string_view utf8, std::vector<uint8_t>& graphemeStart);
// ft_script.cpp
hb_script_t scriptForCodepoint(uint32_t codepoint);
VerticalOrientation verticalOrientationFor(uint32_t codepoint);
// ft_glyphs.cpp
bool ftDecomposeOutline(FT_Face face, uint32_t glyphId, std::vector<PathVerb>& outVerbs,
                        std::vector<float>& outPoints);
bool ftRenderBitmap(FT_Face face, uint32_t glyphId, float sizePx, float subpixelX, GlyphBitmap& out);

namespace {

/// T-261e: font directories and files are named in UTF-8 (xt_text does not link xt_compile, whose
/// utf8_path.h does the same through `char8_t`). `path::string()` would use the ANSI code page --
/// and throw on a name it cannot spell.
std::filesystem::path pathFromUtf8(std::string_view utf8) {
    return std::filesystem::path(
        std::u8string_view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string pathToUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

constexpr uint32_t kReplacementChar = 0xFFFD;
constexpr size_t kGlyphCacheLimit = 4096;

struct DecodedCodepoint {
    uint32_t codepoint;
    uint32_t byteOffset;
    uint32_t byteLength;
};

// Decodes `utf8` into codepoints. An invalid byte becomes a single U+FFFD occupying that one
// byte (docs/tasks/T-29.md: "無效序列 -> U+FFFD，佔原位元組"); decoding then resumes at the next
// byte, so a run of N invalid bytes yields N replacement codepoints, never crashing regardless of
// input.
std::vector<DecodedCodepoint> decodeUtf8(std::string_view utf8) {
    std::vector<DecodedCodepoint> out;
    out.reserve(utf8.size());
    size_t i = 0;
    const size_t n = utf8.size();
    while (i < n) {
        const auto b0 = static_cast<unsigned char>(utf8[i]);
        uint32_t cp = 0;
        uint32_t len = 0;
        if (b0 < 0x80) {
            cp = b0;
            len = 1;
        } else if ((b0 & 0xE0) == 0xC0 && b0 >= 0xC2) {
            len = 2;
        } else if ((b0 & 0xF0) == 0xE0) {
            len = 3;
        } else if ((b0 & 0xF8) == 0xF0 && b0 <= 0xF4) {
            len = 4;
        } else {
            out.push_back({kReplacementChar, static_cast<uint32_t>(i), 1});
            ++i;
            continue;
        }
        if (len > 1) {
            if (i + len > n) {
                out.push_back({kReplacementChar, static_cast<uint32_t>(i), 1});
                ++i;
                continue;
            }
            bool ok = true;
            cp = b0 & (0xFFu >> (len + 1));
            for (uint32_t k = 1; k < len; ++k) {
                const auto bk = static_cast<unsigned char>(utf8[i + k]);
                if ((bk & 0xC0) != 0x80) {
                    ok = false;
                    break;
                }
                cp = (cp << 6) | (bk & 0x3Fu);
            }
            if (!ok || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                out.push_back({kReplacementChar, static_cast<uint32_t>(i), 1});
                ++i;
                continue;
            }
        }
        out.push_back({cp, static_cast<uint32_t>(i), len});
        i += len;
    }
    return out;
}

// Unicode White_Space, excluding U+00A0 (NBSP), U+202F (narrow NBSP), U+2007 (figure space) --
// docs/design/07-text.md §7.2 ShapedGlyph::isSpace.
bool isCollapsibleSpace(uint32_t cp) {
    switch (cp) {
        case 0x0009:
        case 0x000A:
        case 0x000B:
        case 0x000C:
        case 0x000D:
        case 0x0020:
        case 0x0085:
        case 0x1680:
        case 0x2000:
        case 0x2001:
        case 0x2002:
        case 0x2003:
        case 0x2004:
        case 0x2005:
        case 0x2006:
        case 0x2008:
        case 0x2009:
        case 0x200A:
        case 0x2028:
        case 0x2029:
        case 0x205F:
        case 0x3000:
            return true;
        default:
            return false;
    }
}

struct GlyphCacheKey {
    FontId font;
    uint32_t glyphId;
    bool operator==(const GlyphCacheKey& o) const { return font == o.font && glyphId == o.glyphId; }
};
struct GlyphCacheKeyHash {
    size_t operator()(const GlyphCacheKey& k) const {
        return std::hash<uint32_t>()(k.font) * 1000003u ^ std::hash<uint32_t>()(k.glyphId);
    }
};
struct BitmapCacheKey {
    FontId font;
    uint32_t glyphId;
    uint32_t roundedSize;
    /// Horizontal subpixel variant, 0..3 == 0/0.25/0.5/0.75 px (T-101).
    uint32_t subpixel;
    bool operator==(const BitmapCacheKey& o) const {
        return font == o.font && glyphId == o.glyphId && roundedSize == o.roundedSize &&
               subpixel == o.subpixel;
    }
};
struct BitmapCacheKeyHash {
    size_t operator()(const BitmapCacheKey& k) const {
        size_t h = std::hash<uint32_t>()(k.font);
        h = h * 1000003u ^ std::hash<uint32_t>()(k.glyphId);
        h = h * 1000003u ^ std::hash<uint32_t>()(k.roundedSize);
        h = h * 1000003u ^ std::hash<uint32_t>()(k.subpixel);
        return h;
    }
};
struct OutlineEntry {
    std::vector<PathVerb> verbs;
    std::vector<float> points;
};

}  // namespace

struct FtTextShaper::Impl {
    std::mutex mutex;
    FontTable fonts;
    std::unordered_map<FontId, hb_font_t*> hbFonts;
    std::unordered_map<GlyphCacheKey, OutlineEntry, GlyphCacheKeyHash> outlineCache;
    std::unordered_map<BitmapCacheKey, GlyphBitmap, BitmapCacheKeyHash> bitmapCache;

    ~Impl() {
        for (auto& [id, font] : hbFonts) hb_font_destroy(font);
    }

    hb_font_t* hbFontFor(FontId font) {
        auto it = hbFonts.find(font);
        if (it != hbFonts.end()) return it->second;
        FT_Face face = fonts.face(font);
        if (face == nullptr) return nullptr;
        hb_font_t* hbFont = hb_ft_font_create_referenced(face);
        hbFonts.emplace(font, hbFont);
        return hbFont;
    }
};

FtTextShaper::FtTextShaper() : impl_(std::make_unique<Impl>()) {}
FtTextShaper::~FtTextShaper() = default;

bool FtTextShaper::addFontFile(std::string_view path) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->fonts.addFontFile(path);
}

bool FtTextShaper::addFontMemory(std::vector<uint8_t> bytes) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->fonts.addFontMemory(std::move(bytes))) {
        return false;
    }
    // docs/design/07-text.md §7.3 step 3 (T-35 "附帶修正"): a font registered from memory -- i.e.
    // every `@font-face` asset a Document loads -- also joins the fallback candidate list, so a
    // codepoint none of the `font-family` fonts covers can still be shaped from it instead of
    // becoming .notdef.
    impl_->fonts.addFallbackFont(impl_->fonts.fontCount() - 1);
    return true;
}

uint32_t FtTextShaper::addFontDir(std::string_view dir, uint32_t maxFiles) {
    std::error_code ec;
    const std::filesystem::path root = pathFromUtf8(dir);  // T-261e: `dir` is UTF-8.
    if (!std::filesystem::is_directory(root, ec)) {
        return 0;
    }
    uint32_t added = 0;
    std::filesystem::directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied,
                                           ec);
    if (ec) {
        return 0;
    }
    for (const std::filesystem::directory_entry& entry : it) {
        if (added >= maxFiles) {
            break;
        }
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        std::string ext = pathToUtf8(entry.path().extension());
        for (char& c : ext) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (ext != ".ttf" && ext != ".otf") {
            continue;
        }
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->fonts.addFontFileDeferred(pathToUtf8(entry.path()))) {
            // Directory fonts (system fonts) are fallback candidates so that any family request
            // and any script can resolve to a font that has the glyph (07.3 step 3).
            impl_->fonts.addFallbackFont(impl_->fonts.fontCount() - 1);
            ++added;
        }
    }
    return added;
}

uint32_t FtTextShaper::addSystemFonts(uint32_t maxFiles) {
    static const char* const kDirs[] = {
#if defined(_WIN32)
        "C:/Windows/Fonts",
#elif defined(__APPLE__)
        "/System/Library/Fonts",
        "/Library/Fonts",
#elif defined(__ANDROID__)
        "/system/fonts",  // T-240.
#else
        "/usr/share/fonts",
        "/usr/local/share/fonts",
#endif
    };
    // System font trees are nested (Linux: /usr/share/fonts/truetype/<family>/, macOS mixes files
    // and folders), so walk them recursively; addFontDir() itself stays a flat scan.
    uint32_t added = 0;
    for (const char* dir : kDirs) {
        std::error_code ec;
        std::filesystem::path root(dir);
        if (!std::filesystem::is_directory(root, ec)) {
            continue;
        }
        std::filesystem::recursive_directory_iterator it(
            root, std::filesystem::directory_options::skip_permission_denied, ec);
        if (ec) {
            continue;
        }
        std::vector<std::string> dirs;
        dirs.push_back(pathToUtf8(root));
        for (const std::filesystem::directory_entry& entry : it) {
            if (entry.is_directory(ec)) {
                dirs.push_back(pathToUtf8(entry.path()));
            }
        }
        for (const std::string& d : dirs) {
            if (added >= maxFiles) {
                return added;
            }
            added += addFontDir(d, maxFiles - added);
        }
    }
    return added;
}

void FtTextShaper::addFallbackFont(FontId id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->fonts.addFallbackFont(id);
}

uint32_t FtTextShaper::fontCount() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->fonts.fontCount();
}

uint32_t FtTextShaper::openedFontCount() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->fonts.openedFontCount();
}

void FtTextShaper::fonts(std::vector<FontDesc>& out) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->fonts.fonts(out);
}

std::string FtTextShaper::fontPath(FontId id) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->fonts.fontPath(id);
}

FontId FtTextShaper::match(const FontRequest& request) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->fonts.match(request);
}

FontMetrics FtTextShaper::metrics(FontId font) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->fonts.metrics(font);
}

namespace {

// Per-codepoint attributes computed before run segmentation (docs/design/07-text.md §7.3 steps
// 1-3, 5).
struct CpAttrs {
    uint32_t codepoint;
    uint32_t byteStart;
    uint32_t byteLength;
    uint8_t level;
    hb_script_t script;
    FontId font;
    uint8_t vertical;
    uint8_t rotated;
};

}  // namespace

void FtTextShaper::shape(std::string_view utf8, const ShapeParams& params, ShapeResult& out) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    out.runs.clear();
    out.glyphs.clear();
    out.breakAfter.clear();
    out.graphemeStart.clear();

    if (utf8.empty()) return;

    // Steps 7-8: byte-indexed break/grapheme tables cover the whole string independent of runs.
    computeBreaks(utf8, params.lang, out.breakAfter);
    computeGraphemes(utf8, out.graphemeStart);

    const std::vector<DecodedCodepoint> codepoints = decodeUtf8(utf8);
    if (codepoints.empty()) return;

    // Step 1: bidi levels (one entry per byte; the value at a codepoint's first byte applies to
    // the whole codepoint).
    const std::vector<uint8_t> byteLevels = computeBidiLevels(utf8, params.paragraphDirection);

    const bool isVerticalMode = params.writingMode != WritingMode::HorizontalTb;

    std::vector<CpAttrs> attrs;
    attrs.reserve(codepoints.size());
    hb_script_t prevScript = HB_SCRIPT_COMMON;
    for (const DecodedCodepoint& dc : codepoints) {
        CpAttrs a{};
        a.codepoint = dc.codepoint;
        a.byteStart = dc.byteOffset;
        a.byteLength = dc.byteLength;
        a.level = byteLevels[dc.byteOffset];

        // Step 2: Common/Inherited scripts carry the previous run's script forward.
        hb_script_t s = scriptForCodepoint(dc.codepoint);
        if (s == HB_SCRIPT_COMMON || s == HB_SCRIPT_INHERITED) s = prevScript;
        a.script = s;
        prevScript = s;

        // Step 3: font-fallback segmentation.
        a.font = impl_->fonts.fontForCodepoint(params.font, dc.codepoint);

        // Step 5: vertical writing-mode orientation.
        a.vertical = 0;
        a.rotated = 0;
        if (isVerticalMode) {
            if (params.orientation == TextOrientation::Upright) {
                a.vertical = 1;
            } else if (params.orientation == TextOrientation::Sideways) {
                a.rotated = 1;
            } else {
                const VerticalOrientation vo = verticalOrientationFor(dc.codepoint);
                if (vo == VerticalOrientation::Upright || vo == VerticalOrientation::TransformedUpright) {
                    a.vertical = 1;
                } else {
                    a.rotated = 1;
                }
            }
        }
        attrs.push_back(a);
    }

    // Step 4: run splitting on (level, script, font, vertical, rotated).
    struct RunSpan {
        size_t firstCp;
        size_t lastCp;  // inclusive
    };
    std::vector<RunSpan> spans;
    size_t runStart = 0;
    for (size_t i = 1; i <= attrs.size(); ++i) {
        const bool boundary =
            i == attrs.size() || attrs[i].level != attrs[runStart].level ||
            attrs[i].script != attrs[runStart].script || attrs[i].font != attrs[runStart].font ||
            attrs[i].vertical != attrs[runStart].vertical || attrs[i].rotated != attrs[runStart].rotated;
        if (boundary) {
            spans.push_back({runStart, i - 1});
            runStart = i;
        }
    }

    hb_language_t language =
        params.lang.empty()
            ? hb_language_get_default()
            : hb_language_from_string(params.lang.data(), static_cast<int>(params.lang.size()));

    for (const RunSpan& span : spans) {
        const CpAttrs& first = attrs[span.firstCp];
        const CpAttrs& last = attrs[span.lastCp];
        const uint32_t byteStart = first.byteStart;
        const uint32_t byteEnd = last.byteStart + last.byteLength;

        ShapedRun run;
        run.font = first.font;
        run.bidiLevel = first.level;
        run.vertical = first.vertical;
        run.rotated = first.rotated;
        run.byteStart = byteStart;
        run.byteEnd = byteEnd;
        run.glyphStart = static_cast<uint32_t>(out.glyphs.size());

        hb_font_t* hbFont = impl_->hbFontFor(first.font);
        if (hbFont == nullptr) {
            run.glyphCount = 0;
            out.runs.push_back(run);
            continue;
        }
        // hb_ft_font_create_referenced's FreeType glyph funcs load outlines/extents through the
        // FT_Face's *current* pixel size (never set otherwise, since glyphOutline/glyphBitmap set
        // their own sizes independently -- see ft_glyphs.cpp); without a nonzero size here every
        // advance comes back 0 regardless of the hb_font_set_scale() below. hb_font_set_scale is
        // still what actually determines the output units (px = value / 64, docs/tasks/T-29.md).
        FT_Face hbFace = impl_->fonts.face(first.font);
        if (hbFace != nullptr) {
            FT_Set_Char_Size(hbFace, 0, static_cast<FT_F26Dot6>(std::lround(params.size * 64.0f)), 0, 0);
        }
        hb_ft_font_changed(hbFont);
        hb_font_set_scale(hbFont, static_cast<int>(std::lround(params.size * 64.0f)),
                          static_cast<int>(std::lround(params.size * 64.0f)));

        hb_buffer_t* buf = hb_buffer_create();
        hb_buffer_add_utf8(buf, utf8.data(), static_cast<int>(utf8.size()),
                           static_cast<unsigned int>(byteStart), static_cast<int>(byteEnd - byteStart));
        const hb_direction_t direction = run.vertical != 0          ? HB_DIRECTION_TTB
                                         : (run.bidiLevel & 1) != 0 ? HB_DIRECTION_RTL
                                                                    : HB_DIRECTION_LTR;
        hb_buffer_set_direction(buf, direction);
        hb_buffer_set_script(buf, first.script);
        hb_buffer_set_language(buf, language);

        std::vector<hb_feature_t> features;
        if (run.vertical != 0) {
            features.push_back({HB_TAG('v', 'e', 'r', 't'), 1, 0, static_cast<unsigned int>(-1)});
            features.push_back({HB_TAG('v', 'r', 't', '2'), 1, 0, static_cast<unsigned int>(-1)});
        }
        if ((params.features & kShapeFeatureTnum) != 0) {  // T-331: font-variant-numeric.
            features.push_back({HB_TAG('t', 'n', 'u', 'm'), 1, 0, static_cast<unsigned int>(-1)});
        }
        hb_shape(hbFont, buf, features.empty() ? nullptr : features.data(),
                 static_cast<unsigned int>(features.size()));

        unsigned int glyphCount = 0;
        hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buf, &glyphCount);
        hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buf, &glyphCount);
        for (unsigned int i = 0; i < glyphCount; ++i) {
            ShapedGlyph g;
            g.glyphId = infos[i].codepoint;
            g.cluster = infos[i].cluster;
            g.xAdvance = static_cast<float>(positions[i].x_advance) / 64.0f;
            g.xOffset = static_cast<float>(positions[i].x_offset) / 64.0f;
            // HarfBuzz's coordinate space has +y pointing up (so HB_DIRECTION_TTB glyphs get a
            // *negative* y_advance as the pen moves down); ShapedGlyph::yAdvance is documented as
            // positive-down (docs/design/07-text.md §7.2), so both are negated here.
            g.yAdvance = -static_cast<float>(positions[i].y_advance) / 64.0f;
            g.yOffset = -static_cast<float>(positions[i].y_offset) / 64.0f;
            // The codepoint that produced this glyph may not be the one at g.cluster's byte for
            // multi-codepoint clusters (ligatures); White_Space is always single-codepoint, so
            // looking up the codepoint at cluster's byte offset is exact for space runs and a
            // reasonable (never-space) default otherwise.
            uint32_t cp = 0;
            for (size_t ci = span.firstCp; ci <= span.lastCp; ++ci) {
                if (attrs[ci].byteStart == g.cluster) {
                    cp = attrs[ci].codepoint;
                    break;
                }
            }
            g.isSpace = isCollapsibleSpace(cp) ? 1 : 0;
            if (run.vertical != 0) {
                g.yAdvance += params.letterSpacing;
            } else {
                g.xAdvance += params.letterSpacing;
            }
            out.glyphs.push_back(g);
        }
        run.glyphCount = static_cast<uint32_t>(out.glyphs.size()) - run.glyphStart;

        hb_buffer_destroy(buf);
        out.runs.push_back(run);
    }

    // "letterSpacing added to every advance except the last glyph" -- undo the extra spacing
    // just added to the very last glyph of the whole result.
    if (params.letterSpacing != 0 && !out.glyphs.empty() && !out.runs.empty()) {
        ShapedGlyph& lastGlyph = out.glyphs.back();
        const ShapedRun& lastRun = out.runs.back();
        if (lastRun.vertical != 0) {
            lastGlyph.yAdvance -= params.letterSpacing;
        } else {
            lastGlyph.xAdvance -= params.letterSpacing;
        }
    }
}

bool FtTextShaper::glyphOutline(FontId font, uint32_t glyphId, std::vector<PathVerb>& outVerbs,
                                std::vector<float>& outPoints) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const GlyphCacheKey key{font, glyphId};
    auto it = impl_->outlineCache.find(key);
    if (it != impl_->outlineCache.end()) {
        outVerbs = it->second.verbs;
        outPoints = it->second.points;
        return !outVerbs.empty();
    }

    FT_Face face = impl_->fonts.face(font);
    OutlineEntry entry;
    const bool ok = ftDecomposeOutline(face, glyphId, entry.verbs, entry.points);
    if (impl_->outlineCache.size() >= kGlyphCacheLimit) impl_->outlineCache.clear();
    outVerbs = entry.verbs;
    outPoints = entry.points;
    impl_->outlineCache.emplace(key, std::move(entry));
    return ok;
}

bool FtTextShaper::glyphBitmap(FontId font, uint32_t glyphId, float size, float subpixelX, GlyphBitmap& out) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const uint32_t roundedSize = static_cast<uint32_t>(std::lround(size * 4.0f));
    // Wrap into [0, 1): a caller passing a whole-pixel offset (or a tiny negative rounding error)
    // gets the 0 variant rather than a fifth cache entry.
    const uint32_t subpixel = static_cast<uint32_t>(std::lround(subpixelX * 4.0f)) & 3u;
    const BitmapCacheKey key{font, glyphId, roundedSize, subpixel};
    auto it = impl_->bitmapCache.find(key);
    if (it != impl_->bitmapCache.end()) {
        out = it->second;
        return out.width > 0 && out.height > 0;
    }

    FT_Face face = impl_->fonts.face(font);
    GlyphBitmap bitmap;
    const bool ok = ftRenderBitmap(face, glyphId, size, static_cast<float>(subpixel) * 0.25f, bitmap);
    if (impl_->bitmapCache.size() >= kGlyphCacheLimit) impl_->bitmapCache.clear();
    out = bitmap;
    impl_->bitmapCache.emplace(key, bitmap);
    return ok && out.width > 0 && out.height > 0;
}

}  // namespace xt::text
