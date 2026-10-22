#include "font_name_table.h"

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

// One `name` table record's string, decoded to ASCII: platform 3 (Windows) and 0 (Unicode) store
// UTF-16BE, platform 1 (Mac) single bytes. Font names used for matching are ASCII in every family
// this reader needs to compare, so the high byte of each UTF-16 code unit is simply dropped (same
// simplification the moved-from Code/Editor/Import/src/figma_fonts.cpp parser made).
std::string decodeNameString(const std::vector<unsigned char>& bytes, uint16_t platform) {
    std::string out;
    if (platform == 3 || platform == 0) {
        out.reserve(bytes.size() / 2);
        for (size_t i = 1; i < bytes.size(); i += 2) out.push_back(static_cast<char>(bytes[i]));
    } else {
        out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    return out;
}

}  // namespace

bool readFontNameTable(std::string_view path, FontNameInfo& out) {
    FontFile in;
    in.file = openFileUtf8(std::string(path).c_str(), "rb");  // T-261e: `path` is UTF-8.
    if (in.file == nullptr) return false;

    std::vector<unsigned char> header;
    if (!readBytes(in, 0, 12, header)) return false;

    // A `.ttc` collection starts with the 'ttcf' tag, a version, a face count, then that many
    // absolute file offsets to each face's own sfnt table directory. Everything else (the tables
    // themselves, including `name`) is addressed by absolute file offset regardless of whether
    // the file is a single font or a collection, so only the offset of face 0's directory differs.
    uint64_t sfntOffset = 0;
    if (header[0] == 't' && header[1] == 't' && header[2] == 'c' && header[3] == 'f') {
        std::vector<unsigned char> firstOffset;
        if (!readBytes(in, 12, 4, firstOffset)) return false;
        sfntOffset = be32(firstOffset, 0);
        if (!readBytes(in, sfntOffset, 12, header)) return false;
    }

    const uint16_t tableCount = be16(header, 4);
    uint64_t nameTableOffset = 0;
    bool foundName = false;
    for (uint16_t i = 0; i < tableCount && !foundName; ++i) {
        std::vector<unsigned char> record;
        if (!readBytes(in, sfntOffset + 12 + static_cast<uint64_t>(i) * 16, 16, record)) return false;
        if (record[0] == 'n' && record[1] == 'a' && record[2] == 'm' && record[3] == 'e') {
            nameTableOffset = be32(record, 8);
            foundName = true;
        }
    }
    if (!foundName) return false;

    std::vector<unsigned char> nameHeader;
    if (!readBytes(in, nameTableOffset, 6, nameHeader)) return false;
    const uint16_t recordCount = be16(nameHeader, 2);
    const uint16_t stringAreaOffset = be16(nameHeader, 4);

    std::string family1, subfamily2, family16, subfamily17;
    for (uint16_t i = 0; i < recordCount; ++i) {
        std::vector<unsigned char> record;
        if (!readBytes(in, nameTableOffset + 6 + static_cast<uint64_t>(i) * 12, 12, record)) break;
        const uint16_t platform = be16(record, 0);
        const uint16_t nameId = be16(record, 6);
        const uint16_t length = be16(record, 8);
        const uint16_t strOffset = be16(record, 10);
        if (nameId != 1 && nameId != 2 && nameId != 6 && nameId != 16 && nameId != 17) continue;
        if (length == 0) continue;

        std::vector<unsigned char> raw;
        if (!readBytes(in, nameTableOffset + stringAreaOffset + strOffset, length, raw)) continue;
        std::string value = decodeNameString(raw, platform);
        if (value.empty()) continue;

        switch (nameId) {
            case 6:
                if (out.postScriptName.empty()) out.postScriptName = std::move(value);
                break;
            case 1:
                if (family1.empty()) family1 = std::move(value);
                break;
            case 2:
                if (subfamily2.empty()) subfamily2 = std::move(value);
                break;
            case 16:
                if (family16.empty()) family16 = std::move(value);
                break;
            case 17:
                if (subfamily17.empty()) subfamily17 = std::move(value);
                break;
            default:
                break;
        }
    }

    out.family = !family16.empty() ? family16 : family1;
    out.subfamily = !subfamily17.empty() ? subfamily17 : subfamily2;
    return !out.family.empty() || !out.postScriptName.empty();
}

}  // namespace xt::text
