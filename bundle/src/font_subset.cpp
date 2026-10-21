#include "font_subset.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <utility>

/// See font_subset.h. Byte order: every sfnt field is big-endian. Offsets are validated against
/// the file size before use, so a truncated font parses as "not a font" instead of reading past
/// the buffer.

namespace xt::bundle {

namespace {

constexpr uint32_t tagOf(char a, char b, char c, char d) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(a)) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(b)) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(c)) << 8) |
           static_cast<uint32_t>(static_cast<unsigned char>(d));
}

constexpr uint32_t kCmap = tagOf('c', 'm', 'a', 'p');
constexpr uint32_t kHead = tagOf('h', 'e', 'a', 'd');
constexpr uint32_t kMaxp = tagOf('m', 'a', 'x', 'p');
constexpr uint32_t kLoca = tagOf('l', 'o', 'c', 'a');
constexpr uint32_t kGlyf = tagOf('g', 'l', 'y', 'f');

uint16_t be16FontSubset(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t be32FontSubset(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
void put16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}
void put32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}
void set32(std::vector<uint8_t>& out, size_t at, uint32_t v) {
    out[at] = static_cast<uint8_t>(v >> 24);
    out[at + 1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    out[at + 2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    out[at + 3] = static_cast<uint8_t>(v & 0xFF);
}

uint32_t checksum(const uint8_t* data, size_t length) {
    uint32_t sum = 0;
    size_t i = 0;
    for (; i + 4 <= length; i += 4) {
        sum += be32FontSubset(data + i);
    }
    if (i < length) {
        uint8_t tail[4] = {0, 0, 0, 0};
        std::memcpy(tail, data + i, length - i);
        sum += be32FontSubset(tail);
    }
    return sum;
}

/// Builds a cmap format 4 subtable for the BMP entries of `mapping` (sorted, no surrogates).
std::vector<uint8_t> buildFormat4(const std::vector<std::pair<uint32_t, uint16_t>>& mapping) {
    struct Segment {
        uint16_t start, end;
        std::vector<uint16_t> gids;
    };
    std::vector<Segment> segments;
    for (const auto& [cp, gid] : mapping) {
        if (cp > 0xFFFF || (cp >= 0xD800 && cp <= 0xDFFF) || cp == 0xFFFF) {
            continue;
        }
        if (!segments.empty() && static_cast<uint32_t>(segments.back().end) + 1 == cp) {
            segments.back().end = static_cast<uint16_t>(cp);
            segments.back().gids.push_back(gid);
        } else {
            segments.push_back(Segment{static_cast<uint16_t>(cp), static_cast<uint16_t>(cp), {gid}});
        }
    }
    segments.push_back(Segment{0xFFFF, 0xFFFF, {}});
    const uint16_t segCount = static_cast<uint16_t>(segments.size());
    uint16_t entrySelector = 0;
    while ((2u << (entrySelector + 1)) <= segCount) {
        ++entrySelector;
    }
    const uint16_t searchRange = static_cast<uint16_t>(2 << entrySelector);
    std::vector<uint16_t> glyphIdArray;
    std::vector<uint16_t> rangeOffsets(segCount, 0);
    for (size_t i = 0; i + 1 < segments.size(); ++i) {
        rangeOffsets[i] = static_cast<uint16_t>(2 * ((segCount - i) + glyphIdArray.size()));
        glyphIdArray.insert(glyphIdArray.end(), segments[i].gids.begin(), segments[i].gids.end());
    }
    std::vector<uint8_t> out;
    put16(out, 4);
    const size_t lengthAt = out.size();
    put16(out, 0);  // length, patched below
    put16(out, 0);  // language
    put16(out, static_cast<uint16_t>(segCount * 2));
    put16(out, searchRange);
    put16(out, entrySelector);
    put16(out, static_cast<uint16_t>(segCount * 2 - searchRange));
    for (const Segment& s : segments) {
        put16(out, s.end);
    }
    put16(out, 0);  // reservedPad
    for (const Segment& s : segments) {
        put16(out, s.start);
    }
    for (size_t i = 0; i < segments.size(); ++i) {
        put16(out, i + 1 == segments.size() ? 1 : 0);  // idDelta
    }
    for (uint16_t offset : rangeOffsets) {
        put16(out, offset);
    }
    for (uint16_t gid : glyphIdArray) {
        put16(out, gid);
    }
    out[lengthAt] = static_cast<uint8_t>(out.size() >> 8);
    out[lengthAt + 1] = static_cast<uint8_t>(out.size() & 0xFF);
    return out;
}

/// Builds a cmap format 12 subtable of every entry of `mapping`.
std::vector<uint8_t> buildFormat12(const std::vector<std::pair<uint32_t, uint16_t>>& mapping) {
    struct Group {
        uint32_t start, end, gid;
    };
    std::vector<Group> groups;
    for (const auto& [cp, gid] : mapping) {
        if (!groups.empty() && groups.back().end + 1 == cp &&
            groups.back().gid + (cp - groups.back().start) == gid) {
            groups.back().end = cp;
        } else {
            groups.push_back(Group{cp, cp, gid});
        }
    }
    std::vector<uint8_t> out;
    put16(out, 12);
    put16(out, 0);
    put32(out, static_cast<uint32_t>(16 + groups.size() * 12));
    put32(out, 0);  // language
    put32(out, static_cast<uint32_t>(groups.size()));
    for (const Group& g : groups) {
        put32(out, g.start);
        put32(out, g.end);
        put32(out, g.gid);
    }
    return out;
}

}  // namespace

const FontFile::Table* FontFile::find(uint32_t tag) const {
    for (const Table& t : m_tables) {
        if (t.tag == tag) {
            return &t;
        }
    }
    return nullptr;
}

bool FontFile::parseCmap(const Table& cmap) {
    const uint8_t* base = m_bytes.data() + cmap.offset;
    if (cmap.length < 4) {
        return false;
    }
    const uint16_t count = be16FontSubset(base + 2);
    if (4u + count * 8u > cmap.length) {
        return false;
    }
    // Prefer (3,10) format 12, then (3,1) / (0,x) format 4, then any format 4 / 12.
    int bestScore = -1;
    uint32_t bestOffset = 0;
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* rec = base + 4 + i * 8;
        const uint16_t platform = be16FontSubset(rec);
        const uint16_t encoding = be16FontSubset(rec + 2);
        const uint32_t offset = be32FontSubset(rec + 4);
        if (offset > cmap.length - 4) {  // cmap.length >= 4 here; never `offset + 4` (T-257: wraps)
            continue;
        }
        const uint16_t format = be16FontSubset(base + offset);
        int score = -1;
        if (format == 12) {
            score = (platform == 3 && encoding == 10) ? 4 : 3;
        } else if (format == 4) {
            score = (platform == 3 && encoding == 1) ? 2 : (platform == 0 ? 1 : 0);
        }
        if (score > bestScore) {
            bestScore = score;
            bestOffset = offset;
        }
    }
    if (bestScore < 0) {
        return false;
    }
    const uint8_t* sub = base + bestOffset;
    const size_t avail = cmap.length - bestOffset;
    std::map<uint32_t, uint16_t> entries;
    if (be16FontSubset(sub) == 4) {
        if (avail < 14) {
            return false;
        }
        const uint16_t segCount = be16FontSubset(sub + 6) / 2;
        if (14u + segCount * 8u + 2u > avail) {
            return false;
        }
        const uint8_t* endCodes = sub + 14;
        const uint8_t* startCodes = endCodes + segCount * 2 + 2;
        const uint8_t* deltas = startCodes + segCount * 2;
        const uint8_t* rangeOffsets = deltas + segCount * 2;
        for (uint16_t s = 0; s < segCount; ++s) {
            const uint32_t end = be16FontSubset(endCodes + s * 2);
            const uint32_t start = be16FontSubset(startCodes + s * 2);
            const uint16_t delta = be16FontSubset(deltas + s * 2);
            const uint16_t rangeOffset = be16FontSubset(rangeOffsets + s * 2);
            if (start > end || start == 0xFFFF) {
                continue;
            }
            for (uint32_t cp = start; cp <= end && cp != 0xFFFF; ++cp) {
                uint16_t gid = 0;
                if (rangeOffset == 0) {
                    gid = static_cast<uint16_t>((cp + delta) & 0xFFFF);
                } else {
                    // Checked as an offset into the cmap table before any pointer is formed.
                    const size_t at = static_cast<size_t>(rangeOffsets - base) + s * 2u + rangeOffset +
                                      static_cast<size_t>(cp - start) * 2u;
                    if (at + 2 > cmap.length) {
                        continue;
                    }
                    gid = be16FontSubset(base + at);
                    if (gid != 0) {
                        gid = static_cast<uint16_t>((gid + delta) & 0xFFFF);
                    }
                }
                if (gid != 0) {
                    entries.emplace(cp, gid);
                }
            }
        }
    } else {
        if (avail < 16) {
            return false;
        }
        const uint32_t groups = be32FontSubset(sub + 12);
        if (16u + static_cast<uint64_t>(groups) * 12u > avail) {
            return false;
        }
        for (uint32_t g = 0; g < groups; ++g) {
            const uint8_t* rec = sub + 16 + g * 12;
            const uint32_t start = be32FontSubset(rec);
            const uint32_t end = be32FontSubset(rec + 4);
            const uint32_t gid = be32FontSubset(rec + 8);
            if (start > end || end - start > 0x10FFFF || gid >= m_numGlyphs) {
                continue;
            }
            // T-257: only code points whose glyph id `gid + (cp - start)` exists are walked, so a
            // group spanning all of Unicode costs at most numGlyphs steps.
            const uint64_t last =
                std::min<uint64_t>(end, static_cast<uint64_t>(start) + (m_numGlyphs - 1u - gid));
            for (uint64_t cp = start; cp <= last; ++cp) {
                const uint32_t id = gid + static_cast<uint32_t>(cp - start);
                if (id != 0) {
                    entries.emplace(static_cast<uint32_t>(cp), static_cast<uint16_t>(id));
                }
            }
        }
    }
    m_cmap.assign(entries.begin(), entries.end());
    return true;
}

bool FontFile::parse(const std::vector<uint8_t>& bytes) {
    m_parsed = false;
    m_bytes = bytes;
    m_tables.clear();
    m_cmap.clear();
    m_loca.clear();
    if (bytes.size() < 12) {
        return false;
    }
    const uint32_t version = be32FontSubset(bytes.data());
    m_cff = version == tagOf('O', 'T', 'T', 'O');
    if (version != 0x00010000u && version != tagOf('t', 'r', 'u', 'e') && !m_cff) {
        return false;
    }
    const uint16_t numTables = be16FontSubset(bytes.data() + 4);
    if (12u + numTables * 16u > bytes.size()) {
        return false;
    }
    for (uint16_t i = 0; i < numTables; ++i) {
        const uint8_t* rec = bytes.data() + 12 + i * 16;
        Table t;
        t.tag = be32FontSubset(rec);
        t.offset = be32FontSubset(rec + 8);
        t.length = be32FontSubset(rec + 12);
        if (static_cast<uint64_t>(t.offset) + t.length > bytes.size()) {
            return false;
        }
        m_tables.push_back(t);
    }
    const Table* cmap = find(kCmap);
    const Table* maxp = find(kMaxp);
    if (cmap == nullptr || maxp == nullptr || maxp->length < 6) {
        return false;
    }
    m_numGlyphs = be16FontSubset(bytes.data() + maxp->offset + 4);  // before parseCmap(), which clamps to it
    if (!parseCmap(*cmap)) {
        return false;
    }
    if (!m_cff) {
        const Table* head = find(kHead);
        const Table* loca = find(kLoca);
        const Table* glyf = find(kGlyf);
        if (head == nullptr || loca == nullptr || glyf == nullptr || head->length < 54) {
            return false;
        }
        m_longLoca = be16FontSubset(bytes.data() + head->offset + 50) != 0;
        const size_t entrySize = m_longLoca ? 4 : 2;
        if ((static_cast<size_t>(m_numGlyphs) + 1) * entrySize > loca->length) {
            return false;
        }
        m_loca.resize(static_cast<size_t>(m_numGlyphs) + 1);
        for (size_t i = 0; i < m_loca.size(); ++i) {
            const uint8_t* p = bytes.data() + loca->offset + i * entrySize;
            m_loca[i] = m_longLoca ? be32FontSubset(p) : static_cast<uint32_t>(be16FontSubset(p)) * 2;
        }
        m_glyfOffset = glyf->offset;
        m_glyfLength = glyf->length;
    }
    m_parsed = true;
    return true;
}

bool FontFile::hasGlyph(uint32_t codePoint) const {
    auto it =
        std::lower_bound(m_cmap.begin(), m_cmap.end(), codePoint,
                         [](const std::pair<uint32_t, uint16_t>& e, uint32_t cp) { return e.first < cp; });
    return it != m_cmap.end() && it->first == codePoint && it->second != 0 && it->second < m_numGlyphs;
}

uint32_t FontFile::outlineCount() const {
    uint32_t n = 0;
    for (size_t i = 0; i + 1 < m_loca.size(); ++i) {
        if (m_loca[i + 1] > m_loca[i]) {
            ++n;
        }
    }
    return n;
}

void FontFile::collectComponents(uint16_t root, std::vector<uint8_t>& keep) const {
    // T-257: an explicit work list instead of recursion (`keep` is the visited mark), so a
    // composite chain as long as the font never deepens the stack.
    std::vector<uint16_t> pending(1, root);
    while (!pending.empty()) {
        const uint16_t gid = pending.back();
        pending.pop_back();
        if (gid >= m_numGlyphs || keep[gid] != 0) {
            continue;
        }
        keep[gid] = 1;
        const uint32_t start = m_loca[gid];
        const uint32_t end = m_loca[gid + 1];
        if (end <= start || end - start < 10 || end > m_glyfLength) {
            continue;
        }
        const uint8_t* g = m_bytes.data() + m_glyfOffset + start;
        const int16_t contours = static_cast<int16_t>(be16FontSubset(g));
        if (contours >= 0) {
            continue;  // Simple glyph.
        }
        size_t pos = 10;
        const size_t length = end - start;
        for (;;) {
            if (pos + 4 > length) {
                break;
            }
            const uint16_t flags = be16FontSubset(g + pos);
            const uint16_t component = be16FontSubset(g + pos + 2);
            pos += 4;
            pending.push_back(component);
            pos += (flags & 0x1) != 0 ? 4 : 2;  // ARG_1_AND_2_ARE_WORDS
            if ((flags & 0x8) != 0) {           // WE_HAVE_A_SCALE
                pos += 2;
            } else if ((flags & 0x40) != 0) {  // WE_HAVE_AN_X_AND_Y_SCALE
                pos += 4;
            } else if ((flags & 0x80) != 0) {  // WE_HAVE_A_TWO_BY_TWO
                pos += 8;
            }
            if ((flags & 0x20) == 0) {  // MORE_COMPONENTS
                break;
            }
        }
    }
}

bool FontFile::subset(const std::vector<uint32_t>& codePoints, std::vector<uint8_t>& out) const {
    if (!m_parsed) {
        return false;
    }
    if (m_cff) {
        out = m_bytes;
        return true;
    }
    std::vector<uint8_t> keep(m_numGlyphs, 0);
    std::vector<std::pair<uint32_t, uint16_t>> mapping;
    collectComponents(0, keep);
    for (uint32_t cp : codePoints) {
        auto it =
            std::lower_bound(m_cmap.begin(), m_cmap.end(), cp,
                             [](const std::pair<uint32_t, uint16_t>& e, uint32_t c) { return e.first < c; });
        if (it == m_cmap.end() || it->first != cp || it->second >= m_numGlyphs) {
            continue;
        }
        mapping.push_back(*it);
        collectComponents(it->second, keep);
    }
    std::sort(mapping.begin(), mapping.end());
    mapping.erase(std::unique(mapping.begin(), mapping.end()), mapping.end());

    // New glyf/loca (long format): kept outlines copied, the rest empty.
    std::vector<uint8_t> glyf;
    std::vector<uint8_t> loca;
    for (uint16_t gid = 0; gid < m_numGlyphs; ++gid) {
        put32(loca, static_cast<uint32_t>(glyf.size()));
        const uint32_t start = m_loca[gid];
        const uint32_t end = m_loca[gid + 1];
        if (keep[gid] != 0 && end > start && end <= m_glyfLength) {
            glyf.insert(glyf.end(), m_bytes.begin() + m_glyfOffset + start,
                        m_bytes.begin() + m_glyfOffset + end);
            while ((glyf.size() & 3) != 0) {
                glyf.push_back(0);
            }
        }
    }
    put32(loca, static_cast<uint32_t>(glyf.size()));

    // New cmap: (3,1) format 4 and, when needed, (3,10) format 12.
    std::vector<uint8_t> cmap;
    {
        bool beyondBmp = false;
        for (const auto& [cp, gid] : mapping) {
            beyondBmp = beyondBmp || cp > 0xFFFF;
        }
        const std::vector<uint8_t> f4 = buildFormat4(mapping);
        const std::vector<uint8_t> f12 = beyondBmp ? buildFormat12(mapping) : std::vector<uint8_t>();
        const uint16_t subtables = beyondBmp ? 2 : 1;
        put16(cmap, 0);
        put16(cmap, subtables);
        const uint32_t headerSize = 4 + subtables * 8u;
        put16(cmap, 3);
        put16(cmap, 1);
        put32(cmap, headerSize);
        if (beyondBmp) {
            put16(cmap, 3);
            put16(cmap, 10);
            put32(cmap, headerSize + static_cast<uint32_t>(f4.size()));
        }
        cmap.insert(cmap.end(), f4.begin(), f4.end());
        cmap.insert(cmap.end(), f12.begin(), f12.end());
    }

    // head with long loca and a zero checksum adjustment (patched at the end).
    const Table* headTable = find(kHead);
    std::vector<uint8_t> head(m_bytes.begin() + headTable->offset,
                              m_bytes.begin() + headTable->offset + headTable->length);
    set32(head, 8, 0);
    head[50] = 0;
    head[51] = 1;

    struct Out {
        uint32_t tag;
        const uint8_t* data;
        uint32_t length;
    };
    std::vector<Out> tables;
    for (const Table& t : m_tables) {
        if (t.tag == kCmap) {
            tables.push_back(Out{t.tag, cmap.data(), static_cast<uint32_t>(cmap.size())});
        } else if (t.tag == kGlyf) {
            tables.push_back(Out{t.tag, glyf.data(), static_cast<uint32_t>(glyf.size())});
        } else if (t.tag == kLoca) {
            tables.push_back(Out{t.tag, loca.data(), static_cast<uint32_t>(loca.size())});
        } else if (t.tag == kHead) {
            tables.push_back(Out{t.tag, head.data(), static_cast<uint32_t>(head.size())});
        } else {
            tables.push_back(Out{t.tag, m_bytes.data() + t.offset, t.length});
        }
    }
    std::sort(tables.begin(), tables.end(), [](const Out& a, const Out& b) { return a.tag < b.tag; });

    out.clear();
    const uint16_t numTables = static_cast<uint16_t>(tables.size());
    uint16_t entrySelector = 0;
    while ((2u << (entrySelector + 1)) <= numTables) {
        ++entrySelector;
    }
    const uint16_t searchRange = static_cast<uint16_t>((1 << entrySelector) * 16);
    put32(out, 0x00010000u);
    put16(out, numTables);
    put16(out, searchRange);
    put16(out, entrySelector);
    put16(out, static_cast<uint16_t>(numTables * 16 - searchRange));
    const size_t directory = out.size();
    out.resize(directory + static_cast<size_t>(numTables) * 16, 0);
    size_t headAt = 0;
    for (size_t i = 0; i < tables.size(); ++i) {
        while ((out.size() & 3) != 0) {
            out.push_back(0);
        }
        const uint32_t offset = static_cast<uint32_t>(out.size());
        if (tables[i].tag == kHead) {
            headAt = offset;
        }
        out.insert(out.end(), tables[i].data, tables[i].data + tables[i].length);
        const size_t rec = directory + i * 16;
        set32(out, rec, tables[i].tag);
        set32(out, rec + 4, checksum(tables[i].data, tables[i].length));
        set32(out, rec + 8, offset);
        set32(out, rec + 12, tables[i].length);
    }
    while ((out.size() & 3) != 0) {
        out.push_back(0);
    }
    if (headAt != 0) {
        set32(out, headAt + 8, 0xB1B0AFBAu - checksum(out.data(), out.size()));
    }
    return true;
}

}  // namespace xt::bundle
