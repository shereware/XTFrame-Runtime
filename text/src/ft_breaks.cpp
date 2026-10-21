#include <graphemebreak.h>
#include <linebreak.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/// libunibreak wrapper: UAX#14 line-break opportunities and UAX#29 grapheme-cluster boundaries,
/// both per UTF-8 byte. See docs/design/07-text.md §7.3 steps 7-8, docs/tasks/T-29.md.

namespace xt::text {

// set_linebreaks_utf8's raw output places the real LINEBREAK_* classification for character N at
// N's *last* UTF-8 byte (continuation bytes of a multi-byte character get LINEBREAK_INSIDEACHAR);
// this function copies that raw classification through 1:1 (mapped per docs/tasks/T-29.md):
// MUSTBREAK -> 2, ALLOWBREAK -> 1, everything else (NOBREAK/INSIDEACHAR/INDETERMINATE) -> 0.
void computeBreaks(std::string_view utf8, std::string_view lang, std::vector<uint8_t>& breakAfter) {
    breakAfter.assign(utf8.size(), 0);
    if (utf8.empty()) return;

    std::string langStr(lang);
    std::vector<char> raw(utf8.size());
    set_linebreaks_utf8(reinterpret_cast<const utf8_t*>(utf8.data()), utf8.size(),
                        lang.empty() ? nullptr : langStr.c_str(), raw.data());

    for (size_t i = 0; i < raw.size(); ++i) {
        switch (raw[i]) {
            case LINEBREAK_MUSTBREAK:
                breakAfter[i] = 2;
                break;
            case LINEBREAK_ALLOWBREAK:
                breakAfter[i] = 1;
                break;
            default:
                breakAfter[i] = 0;
                break;
        }
    }
}

// set_graphemebreaks_utf8's raw output places GRAPHEMEBREAK_BREAK/NOBREAK for character N at N's
// last UTF-8 byte, meaning raw[i-1] tells whether a grapheme boundary occurs between the
// character ending at byte i-1 and the one starting at byte i. Per docs/tasks/T-29.md: byte i
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
