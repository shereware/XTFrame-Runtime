#include "ft_fonts.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include FT_MULTIPLE_MASTERS_H
#include FT_TRUETYPE_TABLES_H

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

// T-569 (07 §7.20): CSS Fonts Level 4 §5.2 font-weight matching over a face's weight range
// [lo, hi] (equal for a static face). A range containing `target` wins; otherwise, for a target in
// [400, 500], heavier faces up to 500 (ascending), then lighter ones (descending), then heavier ones
// above 500 (ascending); below 400 lighter first (descending) then heavier (ascending); above 500
// heavier first (ascending) then lighter (descending). Returns a key, lower = better.
int weightRank(uint16_t lo, uint16_t hi, uint16_t target) {
    if (lo <= target && target <= hi) return 0;
    const bool heavier = lo > target;
    const int distance = heavier ? lo - target : target - hi;
    if (target >= 400 && target <= 500) {
        if (heavier && lo <= 500) return 1000 + distance;
        if (!heavier) return 2000 + distance;
        return 3000 + distance;
    }
    if (target < 400) return heavier ? 2000 + distance : 1000 + distance;
    return heavier ? 1000 + distance : 2000 + distance;
}

// T-569: synthetic-bold strength (fraction of the em) for a request `requested` >= 600 on a family
// whose heaviest face is `heaviest` < 600. 400 -> 700 gives em / 36 (FreeType's own
// FT_GlyphSlot_Embolden uses em / 24, which came out clearly heavier than a real Bold face of Noto
// Sans CJK TC); the strength scales with the weight difference, at most 500.
float emboldenFor(uint16_t requested, uint16_t heaviest) {
    if (requested < 600 || heaviest >= 600 || requested <= heaviest) return 0.0f;
    const int difference = std::min(500, requested - heaviest);
    return static_cast<float>(difference) / 300.0f / 36.0f;
}

// Coarse weight/italic guess from a `name`-table subfamily string (e.g. "Bold Italic"), used only
// while a font is still deferred; FreeType's own style_flags (more reliable) replace this the
// moment the font is actually opened (Docs/tasks/T-104.md).
// T-569: only for a face without an OS/2 table; one with it uses usWeightClass.
uint16_t weightFromSubfamily(std::string_view subfamily) {
    if (containsCI(subfamily, "thin") || containsCI(subfamily, "hairline")) return 100;
    if (containsCI(subfamily, "extralight") || containsCI(subfamily, "ultralight")) return 200;
    if (containsCI(subfamily, "semilight") || containsCI(subfamily, "demilight")) return 350;
    if (containsCI(subfamily, "light")) return 300;
    if (containsCI(subfamily, "medium")) return 500;
    if (containsCI(subfamily, "semibold") || containsCI(subfamily, "demibold")) return 600;
    if (containsCI(subfamily, "extrabold") || containsCI(subfamily, "ultrabold")) return 800;
    if (containsCI(subfamily, "black") || containsCI(subfamily, "heavy")) return 900;
    return containsCI(subfamily, "bold") ? 700 : 400;
}
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
    // T-569 (report R4): CJK and fullwidth punctuation look for a CJK font first, so a `，` or `。`
    // no requested family has is not drawn from whichever font sorts first.
    if (codepoint >= 0x3000 && codepoint <= 0x303F) return han;      // CJK Symbols and Punctuation
    if (codepoint >= 0xFF00 && codepoint <= 0xFFEF) return han;      // Halfwidth and Fullwidth Forms
    if (codepoint >= 0xAC00 && codepoint <= 0xD7A3) return hangul;   // Hangul syllables
    if (codepoint >= 0x0590 && codepoint <= 0x05FF) return hebrew;   // Hebrew
    if (codepoint >= 0x0600 && codepoint <= 0x06FF) return arabic;   // Arabic
    if (codepoint >= 0x0E00 && codepoint <= 0x0E7F) return thai;     // Thai
    if (codepoint >= 0x1F300 && codepoint <= 0x1FAFF) return emoji;  // Misc. emoji blocks
    if (codepoint >= 0x2600 && codepoint <= 0x27BF) return emoji;
    return empty;
}

// A 16.16 `wght` axis value as a CSS weight (1..1000).
uint16_t fixedWeight(FT_Fixed value) {
    return static_cast<uint16_t>(std::clamp<long>(static_cast<long>((value + 0x8000) >> 16), 1, 1000));
}

bool isCollectionPath(std::string_view path) {
    if (path.size() < 4) return false;
    const std::string_view ext = path.substr(path.size() - 4);
    return iequals(ext, ".ttc") || iequals(ext, ".otc");
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
    readFaceFacts(face, out);
    return true;
}

void FontTable::readFaceFacts(FT_Face face, RegisteredFont& font) const {
    // T-569 (07 §7.20): the OS/2 usWeightClass; FreeType's coarse bold flag only for a face
    // without an OS/2 table.
    const auto* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2));
    if (os2 != nullptr && os2->version != 0xFFFF && os2->usWeightClass != 0) {
        int weight = os2->usWeightClass;
        if (weight < 10) weight *= 100;
        font.weight = static_cast<uint16_t>(std::clamp(weight, 1, 1000));
        font.italic = (os2->fsSelection & 0x0201) != 0 || (face->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
        font.cjk = (os2->ulUnicodeRange2 & ((1u << 17) | (1u << 24) | (1u << 27))) != 0 ||
                   (os2->version >= 1 && (os2->ulCodePageRange1 & (0x1Fu << 17)) != 0);
    } else {
        font.weight = (face->style_flags & FT_STYLE_FLAG_BOLD) != 0 ? 700 : 400;
        font.italic = (face->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
    }
    font.weightMin = font.weight;
    font.weightMax = font.weight;
    font.variable = false;
    FT_MM_Var* mm = nullptr;
    if (FT_HAS_MULTIPLE_MASTERS(face) && FT_Get_MM_Var(face, &mm) == 0 && mm != nullptr) {
        for (FT_UInt a = 0; a < mm->num_axis; ++a) {
            if (mm->axis[a].tag != FT_MAKE_TAG('w', 'g', 'h', 't')) continue;
            font.variable = true;
            font.weightMin = fixedWeight(mm->axis[a].minimum);
            font.weightMax = fixedWeight(mm->axis[a].maximum);
            font.weight = fixedWeight(mm->axis[a].def);
            break;
        }
        FT_Done_MM_Var(library_, mm);
    }
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

uint32_t FontTable::addFontFileDeferred(std::string_view path, uint32_t maxFaces) {
    // T-569 (07 §7.20): every face of a `.ttc`/`.otc` collection is its own candidate.
    const uint32_t faces = fontFaceCount(path);
    uint32_t added = 0;
    for (uint32_t index = 0; index < faces && added < maxFaces; ++index) {
        FontNameInfo info;
        if (!readFontNameTable(path, info, index)) continue;
        RegisteredFont font;
        font.path = std::string(path);
        font.faceIndex = static_cast<long>(index);
        font.collection = faces > 1 || isCollectionPath(path);
        font.family = std::move(info.family);
        font.subfamily = std::move(info.subfamily);
        font.postScriptName = std::move(info.postScriptName);
        if (info.weightClass != 0) {
            font.weight = info.weightClass;
            font.italic = info.italic || italicFromSubfamily(font.subfamily);
        } else {
            font.weight = weightFromSubfamily(font.subfamily);
            font.italic = italicFromSubfamily(font.subfamily);
        }
        font.cjk = info.cjk;
        font.weightMin = font.weight;
        font.weightMax = font.weight;
        if (info.variableWeight && info.weightMin <= info.weightMax) {
            font.variable = true;
            font.weightMin = info.weightMin;
            font.weightMax = info.weightMax;
            font.weight = std::clamp(info.weightDefault, info.weightMin, info.weightMax);
        }
        font.deferred = true;
        fonts_.push_back(std::move(font));
        ++added;
    }
    return added;
}

void FontTable::addFallbackFont(FontId id) { fallback_.push_back(id); }

void FontTable::fonts(std::vector<FontDesc>& out) const {
    for (size_t i = 0; i < fonts_.size(); ++i) {
        if (fonts_[i].base != kInvalidFont) continue;  // T-569: weight instances are not listed.
        FontDesc desc;
        desc.id = static_cast<FontId>(i);
        desc.family = fonts_[i].family;
        desc.weight = fonts_[i].weight;
        desc.italic = fonts_[i].italic;
        desc.cjk = fonts_[i].cjk;
        desc.fallback = std::find(fallback_.begin(), fallback_.end(), desc.id) != fallback_.end();
        out.push_back(std::move(desc));
    }
}

std::string FontTable::fontPath(FontId id) const {
    if (id >= fonts_.size()) return std::string();
    return fonts_[id].path;
}

long FontTable::fontFaceIndex(FontId id) const {
    if (id >= fonts_.size() || !fonts_[id].collection) return -1;
    return fonts_[id].faceIndex;
}

float FontTable::embolden(FontId id) const { return id < fonts_.size() ? fonts_[id].embolden : 0.0f; }

std::string FontTable::missingCjkFamily(const std::vector<std::string>& stack) const {
    return firstMissingCjkFamily(stack, [this](std::string_view family) {
        FamilyPresence state = FamilyPresence::kAbsent;
        for (const RegisteredFont& f : fonts_) {
            if (f.broken || f.base != kInvalidFont || !iequals(f.family, family)) continue;
            if (f.cjk) return FamilyPresence::kInstalledCjk;
            state = FamilyPresence::kInstalled;
        }
        return state;
    });
}

FontId FontTable::instanceOf(FontId base, uint16_t wght, float strength) const {
    if (wght == 0 && strength <= 0.0f) return base;
    const auto key = std::make_tuple(base, wght, static_cast<int>(std::lround(strength * 1000.0f)));
    const auto found = instances_.find(key);
    if (found != instances_.end()) return found->second;
    if (face(base) == nullptr) return base;  // Opens a deferred base; a broken one stays itself.
    FT_Face instanceFace = nullptr;
    const FT_Error err = !fonts_[base].path.empty()
                             ? newFaceUtf8(library_, fonts_[base].path, fonts_[base].faceIndex, &instanceFace)
                             : FT_New_Memory_Face(library_, fonts_[base].memory.data(),
                                                  static_cast<FT_Long>(fonts_[base].memory.size()),
                                                  fonts_[base].faceIndex, &instanceFace);
    if (err != 0 || instanceFace == nullptr) {
        instances_.emplace(key, base);
        return base;
    }
    if (wght != 0) {
        // T-569 (07 §7.20): a variable face's `wght` axis set to the matched weight, the other
        // axes left at their defaults.
        FT_MM_Var* mm = nullptr;
        if (FT_Get_MM_Var(instanceFace, &mm) == 0 && mm != nullptr) {
            std::vector<FT_Fixed> coords(mm->num_axis);
            for (FT_UInt a = 0; a < mm->num_axis; ++a) {
                const bool weightAxis = mm->axis[a].tag == FT_MAKE_TAG('w', 'g', 'h', 't');
                coords[a] = weightAxis ? static_cast<FT_Fixed>(wght) * 65536 : mm->axis[a].def;
            }
            FT_Set_Var_Design_Coordinates(instanceFace, mm->num_axis, coords.data());
            FT_Done_MM_Var(library_, mm);
        }
    }
    RegisteredFont inst;
    const RegisteredFont& b = fonts_[base];
    inst.face = instanceFace;
    inst.family = b.family;
    inst.subfamily = b.subfamily;
    inst.postScriptName = b.postScriptName;
    inst.path = b.path;
    inst.faceIndex = b.faceIndex;
    inst.collection = b.collection;
    inst.weight = wght != 0 ? wght : b.weight;
    inst.weightMin = inst.weight;
    inst.weightMax = inst.weight;
    inst.italic = b.italic;
    inst.cjk = b.cjk;
    inst.base = base;
    inst.instanceWeight = wght;
    inst.embolden = strength;
    fonts_.push_back(std::move(inst));
    const auto id = static_cast<FontId>(fonts_.size() - 1);
    instances_.emplace(key, id);
    return id;
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
    // T-569: the deferred scan already read OS/2 and `fvar`, the tables readFaceFacts() reads,
    // so the weight a match was made with stays put; only a face whose scan found no OS/2 table is
    // refined now (Docs/tasks/T-104.md: FreeType's style flags beat the subfamily guess).
    const auto* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2));
    if (os2 == nullptr || os2->version == 0xFFFF || os2->usWeightClass == 0) {
        f.weight = (face->style_flags & FT_STYLE_FLAG_BOLD) != 0 ? 700 : 400;
        f.italic = (face->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
        if (!f.variable) {
            f.weightMin = f.weight;
            f.weightMax = f.weight;
        }
    }
    if (f.family.empty() && face->family_name != nullptr) f.family = face->family_name;
    return face;
}

FontId FontTable::matchFamily(std::string_view family, uint16_t weight, bool italic) const {
    // T-569 (07 §7.20): italic match first, then CSS Fonts 4 weight ranking (weightRank()), ties
    // to the earlier-registered face. The family's heaviest face decides synthetic bold.
    FontId best = kInvalidFont;
    int bestRank = 0;
    bool bestItalic = false;
    for (size_t i = 0; i < fonts_.size(); ++i) {
        const RegisteredFont& f = fonts_[i];
        if (f.broken || f.base != kInvalidFont) continue;
        if (!iequals(f.family, family)) continue;
        const int rank = weightRank(f.weightMin, f.weightMax, weight);
        const bool italicMatch = f.italic == italic;
        if (best == kInvalidFont || (italicMatch && !bestItalic) ||
            (italicMatch == bestItalic && rank < bestRank)) {
            best = static_cast<FontId>(i);
            bestRank = rank;
            bestItalic = italicMatch;
        }
    }
    if (best == kInvalidFont) return best;
    uint16_t heaviest = 0;
    for (const RegisteredFont& f : fonts_) {
        if (!f.broken && f.base == kInvalidFont && f.italic == fonts_[best].italic &&
            iequals(f.family, family)) {
            heaviest = std::max(heaviest, f.weightMax);
        }
    }
    const RegisteredFont& chosen = fonts_[best];
    const uint16_t effective = std::clamp(weight, chosen.weightMin, chosen.weightMax);
    const uint16_t wght = chosen.variable && effective != chosen.weight ? effective : 0;
    return instanceOf(best, wght, emboldenFor(weight, heaviest));
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
