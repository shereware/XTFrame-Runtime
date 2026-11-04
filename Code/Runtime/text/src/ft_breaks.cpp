#include <graphemebreak.h>
#include <linebreak.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/// libunibreak wrapper: UAX#14 line-break opportunities and UAX#29 grapheme-cluster boundaries,
/// both per UTF-8 byte. See Docs/design/07-text.md §7.3 steps 7-8, Docs/tasks/T-29.md.

namespace xt::text {

namespace {

// T-569 (07 §7.20 kinsoku): CJK punctuation that must not start a line (UAX#14 CL / EX / NS / IS,
// fullwidth and ideographic forms) ...
bool lineStartProhibited(uint32_t cp) {
    switch (cp) {
        case 0x3001:  // 、
        case 0x3002:  // 。
        case 0x3009:  // 〉
        case 0x300B:  // 》
        case 0x300D:  // 」
        case 0x300F:  // 』
        case 0x3011:  // 】
        case 0x3015:  // 〕
        case 0x3017:  // 〗
        case 0x3019:  // 〙
        case 0x301B:  // 〛
        case 0x30FB:  // ・
        case 0xFF01:  // ！
        case 0xFF09:  // ）
        case 0xFF0C:  // ，
        case 0xFF0E:  // ．
        case 0xFF1A:  // ：
        case 0xFF1B:  // ；
        case 0xFF1F:  // ？
        case 0xFF3D:  // ］
        case 0xFF5D:  // ｝
            return true;
        default:
            return false;
    }
}

// ... and the opening brackets that must not end one (UAX#14 OP).
bool lineEndProhibited(uint32_t cp) {
    switch (cp) {
        case 0x3008:  // 〈
        case 0x300A:  // 《
        case 0x300C:  // 「
        case 0x300E:  // 『
        case 0x3010:  // 【
        case 0x3014:  // 〔
        case 0x3016:  // 〖
        case 0x3018:  // 〘
        case 0x301A:  // 〚
        case 0xFF08:  // （
        case 0xFF3B:  // ［
        case 0xFF5B:  // ｛
            return true;
        default:
            return false;
    }
}

// The codepoint whose UTF-8 sequence starts at `utf8[i]` and its length (1 for a stray byte).
uint32_t decodeAt(std::string_view utf8, size_t i, size_t& length) {
    const auto b0 = static_cast<unsigned char>(utf8[i]);
    length = b0 < 0x80 ? 1 : (b0 >= 0xF0 ? 4 : (b0 >= 0xE0 ? 3 : (b0 >= 0xC0 ? 2 : 1)));
    if (i + length > utf8.size()) length = 1;
    if (length == 1) return b0;
    uint32_t cp = b0 & (0xFFu >> (length + 1));
    for (size_t k = 1; k < length; ++k) cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3Fu);
    return cp;
}

}  // namespace

// set_linebreaks_utf8's raw output places the real LINEBREAK_* classification for character N at
// N's *last* UTF-8 byte (continuation bytes of a multi-byte character get LINEBREAK_INSIDEACHAR).
// T-569: ShapeResult::breakAfter is indexed by the byte the codepoint *starts* at
// (xt/text_shaper.h; text_layout.cpp's breakLines() reads it at a glyph's cluster byte), so the
// classification is moved to N's first byte -- before T-569 every break opportunity after a
// multi-byte character (all of CJK) was invisible to line breaking, which then only ever made
// emergency breaks and put `。` at the start of a line (report R4 / F51). Mapping (Docs/tasks/
// T-29.md): MUSTBREAK -> 2, ALLOWBREAK -> 1, everything else -> 0, except a NOBREAK that kinsoku
// owns -- the next character must not start a line, or this one must not end it -- which is 3, so
// an emergency break avoids it too (text_layout.cpp).
void computeBreaks(std::string_view utf8, std::string_view lang, std::vector<uint8_t>& breakAfter) {
    breakAfter.assign(utf8.size(), 0);
    if (utf8.empty()) return;

    std::string langStr(lang);
    std::vector<char> raw(utf8.size());
    set_linebreaks_utf8(reinterpret_cast<const utf8_t*>(utf8.data()), utf8.size(),
                        lang.empty() ? nullptr : langStr.c_str(), raw.data());

    size_t i = 0;
    while (i < utf8.size()) {
        size_t length = 1;
        const uint32_t cp = decodeAt(utf8, i, length);
        const size_t last = i + length - 1;
        uint8_t value = 0;
        switch (raw[last]) {
            case LINEBREAK_MUSTBREAK:
                value = 2;
                break;
            case LINEBREAK_ALLOWBREAK:
                value = 1;
                break;
            default:
                value = 0;
                break;
        }
        if (value == 0 && last + 1 < utf8.size()) {
            size_t nextLength = 1;
            const uint32_t next = decodeAt(utf8, last + 1, nextLength);
            if (lineStartProhibited(next) || lineEndProhibited(cp)) value = 3;
        }
        breakAfter[i] = value;
        i += length;
    }
}

// set_graphemebreaks_utf8's raw output places GRAPHEMEBREAK_BREAK/NOBREAK for character N at N's
// last UTF-8 byte, meaning raw[i-1] tells whether a grapheme boundary occurs between the
// character ending at byte i-1 and the one starting at byte i. Per Docs/tasks/T-29.md: byte i
// starts a grapheme cluster (graphemeStart[i] = 1) iff i == 0 or raw[i-1] == GRAPHEMEBREAK_BREAK.
void computeGraphemes(std::string_view utf8, std::vector<uint8_t>& graphemeStart) {
    graphemeStart.assign(utf8.size(), 0);
    if (utf8.empty()) return;

    std::vector<char> raw(utf8.size());
    set_graphemebreaks_utf8(reinterpret_cast<const utf8_t*>(utf8.data()), utf8.size(), nullptr, raw.data());

    graphemeStart[0] = 1;
    for (size_t i = 1; i < utf8.size(); ++i) {
        graphemeStart[i] = raw[i - 1] == GRAPHEMEBREAK_BREAK ? 1 : 0;
    }
}

}  // namespace xt::text
