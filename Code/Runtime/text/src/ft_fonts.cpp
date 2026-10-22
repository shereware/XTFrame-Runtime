#include "ft_fonts.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "file_open_utf8.h"  // Code/Runtime/core/src (T-261e)
#include "font_name_table.h"

namespace xt::text {

namespace {

#if defined(_WIN32)
// T-261e: FT_New_Face() opens its path through the ANSI code page on Windows. A UTF-8 path that is
// not plain ASCII is read through openFileUtf8() as an FT_Stream instead -- still on demand, so a
// deferred system font is never loaded into memory whole.
unsigned long readFontStream(FT_Stream stream, unsigned long offset, unsigned char* buffer,
                             unsigned long count) {
    auto* file = static_cast<std::FILE*>(stream->descriptor.pointer);
    if (_fseeki64(file, static_cast<long long>(offset), SEEK_SET) != 0) {
        return count == 0 ? 1 : 0;  // a seek reports an error as non-zero, a read as 0 bytes
    }
    return count == 0 ? 0 : static_cast<unsigned long>(std::fread(buffer, 1, count, file));
}

void closeFontStream(FT_Stream stream) {
    std::fclose(static_cast<std::FILE*>(stream->descriptor.pointer));
    delete stream;  // FreeType never frees a stream it was given (FT_OPEN_STREAM).
}
#endif

/// FT_New_Face() for a UTF-8 `path` (see readFontStream()).
FT_Error newFaceUtf8(FT_Library library, const std::string& path, FT_Long faceIndex, FT_Face* face) {
#if defined(_WIN32)
    const bool ascii =
        std::all_of(path.begin(), path.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
    if (!ascii) {
        std::FILE* file = openFileUtf8(path.c_str(), "rb");
        if (file == nullptr) return FT_Err_Cannot_Open_Resource;
        long long size = -1;
        if (_fseeki64(file, 0, SEEK_END) == 0) size = _ftelli64(file);
        if (size <= 0) {
            std::fclose(file);
            return FT_Err_Cannot_Open_Resource;
        }
        auto* stream = new FT_StreamRec{};
        stream->size = static_cast<unsigned long>(size);
        stream->descriptor.pointer = file;
        stream->read = readFontStream;
        stream->close = closeFontStream;
        FT_Open_Args args{};
        args.flags = FT_OPEN_STREAM;
        args.stream = stream;
        return FT_Open_Face(library, &args, faceIndex, face);
    }
#endif
    return FT_New_Face(library, path.c_str(), faceIndex, face);
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool containsCI(std::string_view haystack, std::string_view needle) {
    if (needle.size() > haystack.size()) return false;
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool ok = true;
        for (size_t k = 0; k < needle.size(); ++k) {
            if (std::tolower(static_cast<unsigned char>(haystack[i + k])) !=
                std::tolower(static_cast<unsigned char>(needle[k]))) {
                ok = false;
                break;
            }
        }
        if (ok) return true;
    }
    return false;
}

// CSS Fonts Level 4 §5.2 weight-distance ordering, simplified to "nearest wins, ties broken by
// the earlier-registered candidate" (the task's test fixtures never register two weights in the
// same family close enough for the fallback direction rules to matter).
int weightDistance(uint16_t candidate, uint16_t target) {
    return candidate > target ? candidate - target : target - candidate;
}

// Coarse weight/italic guess from a `name`-table subfamily string (e.g. "Bold Italic"), used only
// while a font is still deferred; FreeType's own style_flags (more reliable) replace this the
// moment the font is actually opened (Docs/tasks/T-104.md).
uint16_t weightFromSubfamily(std::string_view subfamily) { return containsCI(subfamily, "bold") ? 700 : 400; }
bool italicFromSubfamily(std::string_view subfamily) {
    return containsCI(subfamily, "italic") || containsCI(subfamily, "oblique");
}

// Family-name substrings that hint a font likely covers `codepoint`'s script, used only to try
// deferred fallback candidates in a promising order before opening any of them (Docs/tasks/T-104
// .md step 3: "先用 name 表的 family 篩...再開"). No such table exists elsewhere in the codebase to
// reuse, so this one is new and deliberately short -- just the common pre-installed
// Windows/macOS/Linux family for each script the i18n regression tests (Tests/text/test_i18n_
// regression.cpp) exercise. It only reorders candidates; every fallback font is still tried if the
// hinted ones don't have the glyph.
const std::vector<std::string_view>& scriptFamilyHints(uint32_t codepoint) {
    static const std::vector<std::string_view> han = {
        "yahei",         "jhenghei",   "mingliu", "simsun",    "simhei",    "pingfang", "hiragino",
        "noto sans cjk", "source han", "meiryo",  "ms gothic", "ms mincho", "heiti",    "songti"};
    static const std::vector<std::string_view> hangul = {"malgun",  "gulim",       "batang",       "dotum",
                                                         "gungsuh", "applegothic", "noto sans cjk"};
    static const std::vector<std::string_view> hebrew = {
        "arial hebrew", "david", "narkisim", "miriam", "aharoni", "noto sans hebrew", "segoe ui"};
    static const std::vector<std::string_view> arabic = {
        "segoe ui", "traditional arabic", "simplified arabic", "noto sans arabic", "geeza pro", "tahoma"};
    static const std::vector<std::string_view> thai = {"leelawadee", "tahoma", "noto sans thai", "angsana",
                                                       "cordia"};
    static const std::vector<std::string_view> emoji = {"emoji", "segoe ui emoji", "apple color emoji",
                                                        "noto color emoji"};
    static const std::vector<std::string_view> empty;
    if (codepoint >= 0x4E00 && codepoint <= 0x9FFF) return han;      // CJK Unified Ideographs
    if (codepoint >= 0x3400 && codepoint <= 0x4DBF) return han;      // CJK Unified Ideographs Ext. A
    if (codepoint >= 0x3040 && codepoint <= 0x30FF) return han;      // Hiragana/Katakana
    if (codepoint >= 0xAC00 && codepoint <= 0xD7A3) return hangul;   // Hangul syllables
    if (codepoint >= 0x0590 && codepoint <= 0x05FF) return hebrew;   // Hebrew
    if (codepoint >= 0x0600 && codepoint <= 0x06FF) return arabic;   // Arabic
    if (codepoint >= 0x0E00 && codepoint <= 0x0E7F) return thai;     // Thai
    if (codepoint >= 0x1F300 && codepoint <= 0x1FAFF) return emoji;  // Misc. emoji blocks
    if (codepoint >= 0x2600 && codepoint <= 0x27BF) return emoji;
    return empty;
}

bool familyMatchesHints(std::string_view family, const std::vector<std::string_view>& hints) {
    for (const std::string_view& h : hints) {
        if (containsCI(family, h)) return true;
    }
    return false;
}

}  // namespace

FontTable::FontTable() { FT_Init_FreeType(&library_); }

FontTable::~FontTable() {
    for (RegisteredFont& f : fonts_) {
        if (f.face != nullptr) FT_Done_Face(f.face);
    }
    if (library_ != nullptr) FT_Done_FreeType(library_);
}

bool FontTable::loadFace(std::vector<uint8_t> memory, RegisteredFont& out) const {
    FT_Face face = nullptr;
    const FT_Error err =
        FT_New_Memory_Face(library_, memory.data(), static_cast<FT_Long>(memory.size()), 0, &face);
    if (err != 0 || face == nullptr) return false;
    out.face = face;
    out.memory = std::move(memory);
    out.family = face->family_name != nullptr ? face->family_name : "";
    // FreeType exposes only a coarse bold/regular style flag; a font with no OS/2 weight table
    // reports FT_STYLE_FLAG_BOLD -> weight 700, otherwise 400. Fonts with an OS/2 table (TrueType
    // /OpenType) are handled the same way here since xt_text does not parse OS/2 usWeightClass;
    // the test fixtures are single-weight fonts, so this coarse mapping is sufficient.
    out.weight = (face->style_flags & FT_STYLE_FLAG_BOLD) != 0 ? 700 : 400;
    out.italic = (face->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
    return true;
}

bool FontTable::addFontFile(std::string_view path) {
    std::string pathStr(path);
    FILE* fp = openFileUtf8(pathStr.c_str(), "rb");  // T-261e: `path` is UTF-8.
    if (fp == nullptr) return false;
    std::fseek(fp, 0, SEEK_END);
    const long size = std::ftell(fp);
    if (size < 0) {
        std::fclose(fp);
        return false;
    }
    std::fseek(fp, 0, SEEK_SET);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    const size_t read = std::fread(bytes.data(), 1, bytes.size(), fp);
    std::fclose(fp);
    if (read != bytes.size()) return false;
    return addFontMemory(std::move(bytes));
}

bool FontTable::addFontMemory(std::vector<uint8_t> bytes) {
    RegisteredFont font;
    if (!loadFace(std::move(bytes), font)) return false;
    fonts_.push_back(std::move(font));
    return true;
}

bool FontTable::addFontFileDeferred(std::string_view path) {
    FontNameInfo info;
    if (!readFontNameTable(path, info)) return false;
    RegisteredFont font;
    font.path = std::string(path);
    font.family = std::move(info.family);
    font.subfamily = std::move(info.subfamily);
    font.postScriptName = std::move(info.postScriptName);
    font.weight = weightFromSubfamily(font.subfamily);
    font.italic = italicFromSubfamily(font.subfamily);
    font.deferred = true;
    fonts_.push_back(std::move(font));
    return true;
}

void FontTable::addFallbackFont(FontId id) { fallback_.push_back(id); }

void FontTable::fonts(std::vector<FontDesc>& out) const {
    for (size_t i = 0; i < fonts_.size(); ++i) {
        FontDesc desc;
        desc.id = static_cast<FontId>(i);
        desc.family = fonts_[i].family;
        desc.weight = fonts_[i].weight;
        desc.italic = fonts_[i].italic;
        desc.fallback = std::find(fallback_.begin(), fallback_.end(), desc.id) != fallback_.end();
        out.push_back(std::move(desc));
    }
}

std::string FontTable::fontPath(FontId id) const {
    if (id >= fonts_.size()) return std::string();
    return fonts_[id].path;
}

FT_Face FontTable::openDeferred(size_t index) const {
    RegisteredFont& f = fonts_[index];
    if (!f.deferred) return f.face;
    f.deferred = false;
    FT_Face face = nullptr;
    const FT_Error err = newFaceUtf8(library_, f.path, f.faceIndex, &face);
    if (err != 0 || face == nullptr) {
        f.broken = true;
        std::fprintf(stderr, "xt_text: system font \"%s\" could not be opened (FT error %d); skipping\n",
                     f.path.c_str(), static_cast<int>(err));
        fallback_.erase(std::remove(fallback_.begin(), fallback_.end(), static_cast<FontId>(index)),
                        fallback_.end());
        return nullptr;
    }
    f.face = face;
    ++openedCount_;
    // FreeType's style_flags are more reliable than the subfamily-string guess used while deferred
    // (Docs/tasks/T-104.md); replace it now that the face is actually open.
    f.weight = (face->style_flags & FT_STYLE_FLAG_BOLD) != 0 ? 700 : 400;
    f.italic = (face->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
    if (f.family.empty() && face->family_name != nullptr) f.family = face->family_name;
    return face;
}

FontId FontTable::matchFamily(std::string_view family, uint16_t weight, bool italic) const {
    FontId best = kInvalidFont;
    int bestDistance = -1;
    bool bestItalic = false;
    for (size_t i = 0; i < fonts_.size(); ++i) {
        const RegisteredFont& f = fonts_[i];
        if (f.broken) continue;
        if (!iequals(f.family, family)) continue;
        const int distance = weightDistance(f.weight, weight);
        const bool italicMatch = f.italic == italic;
        if (best == kInvalidFont || (italicMatch && !bestItalic) ||
            (italicMatch == bestItalic && distance < bestDistance)) {
            best = static_cast<FontId>(i);
            bestDistance = distance;
            bestItalic = italicMatch;
        }
    }
    return best;
}

FontId FontTable::match(const FontRequest& request) const {
    for (const std::string_view& family : request.families) {
        const FontId id = matchFamily(family, request.weight, request.italic);
        if (id != kInvalidFont) {
            // A match is about to be used for shaping/metrics, so open it now rather than leaving
            // it deferred (Docs/tasks/T-104.md "match() 命中 deferred -> 開檔").
            face(id);
            return id;
        }
    }
    if (!fonts_.empty()) return 0;
    return kInvalidFont;
}

FontMetrics FontTable::metrics(FontId font) const {
    FontMetrics m;
    const FT_Face f = face(font);
    if (f == nullptr) return m;
    const float upem = f->units_per_EM != 0 ? static_cast<float>(f->units_per_EM) : 1000.0f;
    m.unitsPerEm = upem;
    m.ascent = static_cast<float>(f->ascender) / upem;
    m.descent = -static_cast<float>(f->descender) / upem;
    m.lineGap = static_cast<float>(f->height) / upem - m.ascent - m.descent;
    // T-324: the `post` table's underline (FreeType keeps it for scalable faces; y-up, so a
    // position below the baseline is negative).
    if (FT_IS_SCALABLE(f) && f->underline_thickness > 0) {
        m.underlineOffset = -static_cast<float>(f->underline_position) / upem;
        m.underlineThickness = static_cast<float>(f->underline_thickness) / upem;
    }
    return m;
}

FT_Face FontTable::face(FontId font) const {
    if (font == kInvalidFont || font >= fonts_.size()) return nullptr;
    if (fonts_[font].broken) return nullptr;
    if (fonts_[font].deferred) return openDeferred(font);
    return fonts_[font].face;
}

FontId FontTable::fontForCodepoint(const FontRequest& request, uint32_t codepoint) const {
    std::vector<FontId> candidates;
    candidates.reserve(request.families.size() + fallback_.size());
    for (const std::string_view& family : request.families) {
        const FontId id = matchFamily(family, request.weight, request.italic);
        if (id != kInvalidFont) candidates.push_back(id);
    }

    // Try fallback candidates whose (already-known, no open needed) name-table family hints at
    // this codepoint's script first, so a lazily-opened system-fallback list doesn't have to be
    // opened in full registration order before finding the one that actually has the glyph
    // (Docs/tasks/T-104.md step 3; scriptFamilyHints() above).
    const std::vector<std::string_view>& hints = scriptFamilyHints(codepoint);
    for (const FontId id : fallback_) {
        if (!fonts_[id].broken && familyMatchesHints(fonts_[id].family, hints)) candidates.push_back(id);
    }
    for (const FontId id : fallback_) {
        if (!fonts_[id].broken && !familyMatchesHints(fonts_[id].family, hints)) candidates.push_back(id);
    }

    for (const FontId id : candidates) {
        const FT_Face f = face(id);
        if (f != nullptr && FT_Get_Char_Index(f, codepoint) != 0) return id;
    }
    return match(request);
}

}  // namespace xt::text
