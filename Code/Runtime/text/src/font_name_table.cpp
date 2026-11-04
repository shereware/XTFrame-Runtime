#include "font_name_table.h"

#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "file_open_utf8.h"  // Code/Runtime/core/src (T-261e)

namespace xt::text {

namespace {

/// The open font file; closed when it goes out of scope.
struct FontFile {
    std::FILE* file = nullptr;
    ~FontFile() {
        if (file != nullptr) std::fclose(file);
    }
};

// Reads `length` bytes at `offset` into `out`. False if the file can't be seeked there or runs
// out before `length` bytes are read (a truncated/corrupt file).
bool readBytes(FontFile& in, uint64_t offset, size_t length, std::vector<unsigned char>& out) {
    out.assign(length, 0);
#if defined(_WIN32)
    if (offset > static_cast<uint64_t>(INT64_MAX) ||
        _fseeki64(in.file, static_cast<int64_t>(offset), SEEK_SET) != 0)
        return false;
#else
    if (offset > static_cast<uint64_t>(LONG_MAX) ||
        std::fseek(in.file, static_cast<long>(offset), SEEK_SET) != 0)
        return false;
#endif
    return length == 0 || std::fread(out.data(), 1, length, in.file) == length;
}

uint16_t be16(const std::vector<unsigned char>& b, size_t off) {
    return static_cast<uint16_t>((b[off] << 8) | b[off + 1]);
}

uint32_t be32(const std::vector<unsigned char>& b, size_t off) {
    return (static_cast<uint32_t>(b[off]) << 24) | (static_cast<uint32_t>(b[off + 1]) << 16) |
           (static_cast<uint32_t>(b[off + 2]) << 8) | static_cast<uint32_t>(b[off + 3]);
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// One `name` table record's string as UTF-8: platform 3 (Windows) and 0 (Unicode) store UTF-16BE
// (T-569: decoded in full, so a localized CJK name is no longer mangled), platform 1 (Mac) single
// bytes, copied as they are.
std::string decodeNameString(const std::vector<unsigned char>& bytes, uint16_t platform) {
    std::string out;
    if (platform == 3 || platform == 0) {
        out.reserve(bytes.size());
        for (size_t i = 0; i + 1 < bytes.size(); i += 2) {
            uint32_t unit = (static_cast<uint32_t>(bytes[i]) << 8) | bytes[i + 1];
            if (unit >= 0xD800 && unit <= 0xDBFF && i + 3 < bytes.size()) {
                const uint32_t low = (static_cast<uint32_t>(bytes[i + 2]) << 8) | bytes[i + 3];
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                    i += 2;
                }
            }
            if (unit >= 0xD800 && unit <= 0xDFFF) unit = 0xFFFD;
            if (unit != 0) appendUtf8(out, unit);
        }
    } else {
        out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    return out;
}

// How strongly a record's (platform, language) is preferred for matching: US-English Windows,
// then any English Windows, then Mac Roman English / Unicode, then any other Windows language.
int namePriority(uint16_t platform, uint16_t language) {
    if (platform == 3 && language == 0x0409) return 5;
    if (platform == 3 && (language & 0x3FF) == 0x09) return 4;
    if (platform == 1 && language == 0) return 3;
    if (platform == 0) return 2;
    if (platform == 3) return 1;
    return 0;
}

int fixedToInt(uint32_t fixed) {
    const auto value = static_cast<int32_t>(fixed);
    return static_cast<int>((value + 0x8000) >> 16);
}

uint16_t clampWeight(int weight) {
    return static_cast<uint16_t>(weight < 1 ? 1 : (weight > 1000 ? 1000 : weight));
}

std::string lowerAscii(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool endsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

// The offset of face `faceIndex`'s sfnt table directory, or false when the file is not sfnt/ttc
// or the index is out of range. `header` receives that directory's first 12 bytes.
bool locateFace(FontFile& in, uint32_t faceIndex, uint64_t& sfntOffset, std::vector<unsigned char>& header) {
    if (!readBytes(in, 0, 12, header)) return false;
    sfntOffset = 0;
    // A `.ttc`/`.otc` collection starts with the 'ttcf' tag, a version, a face count, then that
    // many absolute file offsets to each face's own sfnt table directory. Everything else (the
    // tables themselves, including `name`) is addressed by absolute file offset regardless of
    // whether the file is a single font or a collection, so only the directory offset differs.
    if (header[0] == 't' && header[1] == 't' && header[2] == 'c' && header[3] == 'f') {
        const uint32_t count = be32(header, 8);
        if (faceIndex >= count) return false;
        std::vector<unsigned char> offset;
        if (!readBytes(in, 12 + static_cast<uint64_t>(faceIndex) * 4, 4, offset)) return false;
        sfntOffset = be32(offset, 0);
        return readBytes(in, sfntOffset, 12, header);
    }
    if (faceIndex != 0) return false;
    const uint32_t tag = be32(header, 0);
    return tag == 0x00010000u || tag == 0x4F54544Fu /* OTTO */ || tag == 0x74727565u /* true */;
}

}  // namespace

uint32_t fontFaceCount(std::string_view path) {
    FontFile in;
    in.file = openFileUtf8(std::string(path).c_str(), "rb");
    if (in.file == nullptr) return 0;
    std::vector<unsigned char> header;
    if (!readBytes(in, 0, 12, header)) return 0;
    if (header[0] == 't' && header[1] == 't' && header[2] == 'c' && header[3] == 'f') {
        const uint32_t count = be32(header, 8);
        return count > 4096 ? 0 : count;  // A corrupt count is not a collection.
    }
    const uint32_t tag = be32(header, 0);
    return tag == 0x00010000u || tag == 0x4F54544Fu || tag == 0x74727565u ? 1 : 0;
}

bool readFontNameTable(std::string_view path, FontNameInfo& out, uint32_t faceIndex) {
    FontFile in;
    in.file = openFileUtf8(std::string(path).c_str(), "rb");  // T-261e: `path` is UTF-8.
    if (in.file == nullptr) return false;

    std::vector<unsigned char> header;
    uint64_t sfntOffset = 0;
    if (!locateFace(in, faceIndex, sfntOffset, header)) return false;

    const uint16_t tableCount = be16(header, 4);
    uint64_t nameOffset = 0, os2Offset = 0, fvarOffset = 0;
    uint32_t os2Length = 0, fvarLength = 0;
    for (uint16_t i = 0; i < tableCount; ++i) {
        std::vector<unsigned char> record;
        if (!readBytes(in, sfntOffset + 12 + static_cast<uint64_t>(i) * 16, 16, record)) return false;
        const uint32_t tag = be32(record, 0);
        if (tag == 0x6E616D65u) {  // name
            nameOffset = be32(record, 8);
        } else if (tag == 0x4F532F32u) {  // OS/2
            os2Offset = be32(record, 8);
            os2Length = be32(record, 12);
        } else if (tag == 0x66766172u) {  // fvar
            fvarOffset = be32(record, 8);
            fvarLength = be32(record, 12);
        }
    }
    if (nameOffset == 0) return false;

    std::vector<unsigned char> nameHeader;
    if (!readBytes(in, nameOffset, 6, nameHeader)) return false;
    const uint16_t recordCount = be16(nameHeader, 2);
    const uint16_t stringAreaOffset = be16(nameHeader, 4);

    // nameID 1, 2, 6, 16, 17 -> slot 0..4; the best-priority record of each wins.
    std::string values[5];
    int priorities[5] = {-1, -1, -1, -1, -1};
    for (uint16_t i = 0; i < recordCount; ++i) {
        std::vector<unsigned char> record;
        if (!readBytes(in, nameOffset + 6 + static_cast<uint64_t>(i) * 12, 12, record)) break;
        const uint16_t platform = be16(record, 0);
        const uint16_t language = be16(record, 4);
        const uint16_t nameId = be16(record, 6);
        const uint16_t length = be16(record, 8);
        const uint16_t strOffset = be16(record, 10);
        int slot = -1;
        switch (nameId) {
            case 1:
                slot = 0;
                break;
            case 2:
                slot = 1;
                break;
            case 6:
                slot = 2;
                break;
            case 16:
                slot = 3;
                break;
            case 17:
                slot = 4;
                break;
            default:
                break;
        }
        if (slot < 0 || length == 0) continue;
        const int priority = namePriority(platform, language);
        if (priority <= priorities[slot]) continue;

        std::vector<unsigned char> raw;
        if (!readBytes(in, nameOffset + stringAreaOffset + strOffset, length, raw)) continue;
        std::string value = decodeNameString(raw, platform);
        if (value.empty()) continue;
        values[slot] = std::move(value);
        priorities[slot] = priority;
    }

    out.postScriptName = values[2];
    out.family = !values[3].empty() ? values[3] : values[0];
    out.subfamily = !values[4].empty() ? values[4] : values[1];

    // OS/2: usWeightClass at 4, ulUnicodeRange1..4 at 42, fsSelection at 62, ulCodePageRange1 at
    // 78 (version >= 1).
    if (os2Offset != 0 && os2Length >= 64) {
        std::vector<unsigned char> os2;
        const size_t length = os2Length >= 82 ? 82 : 64;
        if (readBytes(in, os2Offset, length, os2)) {
            int weight = be16(os2, 4);
            if (weight > 0 && weight < 10) weight *= 100;  // A few old fonts store 1..9.
            out.weightClass = weight == 0 ? 0 : clampWeight(weight);
            const uint16_t selection = be16(os2, 62);
            out.italic = (selection & 0x0001) != 0 || (selection & 0x0200) != 0;
            const uint32_t range2 = be32(os2, 46);
            // Bits 49 Hiragana, 56 Hangul Syllables, 59 CJK Unified Ideographs.
            bool cjk = (range2 & ((1u << (49 - 32)) | (1u << (56 - 32)) | (1u << (59 - 32)))) != 0;
            if (length >= 82) {
                const uint32_t codePages = be32(os2, 78);
                cjk = cjk || (codePages & (0x1Fu << 17)) != 0;  // JIS, GB 2312, Wansung, Big5, Johab
            }
            out.cjk = cjk;
        }
    }

    // fvar: axesArrayOffset at 4, axisCount at 8, axisSize at 10; an axis record is tag, min,
    // default, max (16.16 Fixed).
    if (fvarOffset != 0 && fvarLength >= 16) {
        std::vector<unsigned char> fvar;
        if (readBytes(in, fvarOffset, 16, fvar)) {
            const uint16_t axesOffset = be16(fvar, 4);
            const uint16_t axisCount = be16(fvar, 8);
            const uint16_t axisSize = be16(fvar, 10);
            for (uint16_t a = 0; a < axisCount && axisSize >= 16; ++a) {
                std::vector<unsigned char> axis;
                if (!readBytes(in, fvarOffset + axesOffset + static_cast<uint64_t>(a) * axisSize, 16, axis)) {
                    break;
                }
                if (be32(axis, 0) != 0x77676874u) continue;  // wght
                out.variableWeight = true;
                out.weightMin = clampWeight(fixedToInt(be32(axis, 4)));
                out.weightDefault = clampWeight(fixedToInt(be32(axis, 8)));
                out.weightMax = clampWeight(fixedToInt(be32(axis, 12)));
                break;
            }
        }
    }
    return !out.family.empty() || !out.postScriptName.empty();
}

bool isGenericFamily(std::string_view family) {
    static const char* const kGeneric[] = {
        "serif", "sans-serif", "monospace", "cursive",       "fantasy",      "system-ui", "emoji",
        "math",  "fangsong",   "ui-serif",  "ui-sans-serif", "ui-monospace", "ui-rounded"};
    const std::string lower = lowerAscii(family);
    for (const char* generic : kGeneric) {
        if (lower == generic) return true;
    }
    return false;
}

bool isCjkFamilyName(std::string_view family) {
    // A CJK character in the name itself (a localized family such as the Chinese name of
    // Microsoft JhengHei).
    for (size_t i = 0; i < family.size();) {
        const auto b0 = static_cast<unsigned char>(family[i]);
        uint32_t cp = b0;
        size_t len = 1;
        if (b0 >= 0xF0 && i + 3 < family.size()) {
            cp = ((b0 & 0x07u) << 18) | ((static_cast<unsigned char>(family[i + 1]) & 0x3Fu) << 12) |
                 ((static_cast<unsigned char>(family[i + 2]) & 0x3Fu) << 6) |
                 (static_cast<unsigned char>(family[i + 3]) & 0x3Fu);
            len = 4;
        } else if (b0 >= 0xE0 && i + 2 < family.size()) {
            cp = ((b0 & 0x0Fu) << 12) | ((static_cast<unsigned char>(family[i + 1]) & 0x3Fu) << 6) |
                 (static_cast<unsigned char>(family[i + 2]) & 0x3Fu);
            len = 3;
        }
        if ((cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) ||
            (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x20000 && cp <= 0x3FFFF)) {
            return true;
        }
        i += len;
    }
    static const char* const kHints[] = {"jhenghei",      "yahei",          "mingliu",
                                         "simsun",        "simhei",         "dengxian",
                                         "kaiti",         "pingfang",       "hiragino",
                                         "heiti",         "songti",         "meiryo",
                                         "ms gothic",     "ms pgothic",     "ms mincho",
                                         "yu gothic",     "yu mincho",      "malgun",
                                         "gulim",         "batang",         "dotum",
                                         "gungsuh",       "applegothic",    "apple sd gothic",
                                         "noto sans cjk", "noto serif cjk", "noto sans mono cjk",
                                         "source han",    "wenquanyi",      "droid sans fallback",
                                         "gen jyuu",      "genjyuu",        "biz ud",
                                         "nanum"};
    const std::string lower = lowerAscii(family);
    for (const char* hint : kHints) {
        if (lower.find(hint) != std::string::npos) return true;
    }
    // Noto's regional families: "Noto Sans TC", "Noto Serif JP", "Noto Sans Mono CJK HK"...
    if (lower.rfind("noto ", 0) == 0) {
        for (const char* region : {" tc", " sc", " jp", " kr", " hk"}) {
            if (endsWith(lower, region)) return true;
        }
    }
    return false;
}

std::vector<std::string> splitFontFamilyList(std::string_view value) {
    std::vector<std::string> out;
    std::string current;
    char quote = 0;
    auto flush = [&]() {
        size_t begin = 0;
        size_t end = current.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(current[begin])) != 0) ++begin;
        while (end > begin && std::isspace(static_cast<unsigned char>(current[end - 1])) != 0) --end;
        std::string name = current.substr(begin, end - begin);
        if (name.size() >= 2 && (name.front() == '"' || name.front() == '\'') &&
            name.back() == name.front()) {
            name = name.substr(1, name.size() - 2);
        }
        if (!name.empty()) out.push_back(std::move(name));
        current.clear();
    };
    for (const char c : value) {
        if (quote != 0) {
            if (c == quote) quote = 0;
            current.push_back(c);
        } else if (c == '"' || c == '\'') {
            quote = c;
            current.push_back(c);
        } else if (c == ',') {
            flush();
        } else {
            current.push_back(c);
        }
    }
    flush();
    return out;
}

}  // namespace xt::text
