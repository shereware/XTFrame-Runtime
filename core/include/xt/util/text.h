#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

/// ASCII character classes, case folding, trimming, prefix tests and UTF-8 encoding shared by the
/// runtime-side tools and the editor (T-414 C2d; each of these used to be written out in several
/// modules -- compile's `.xt` parser and editor/core's source model, bundle, editor/history,
/// editor/qt). Locale-independent: ASCII only.

namespace xt::util {

inline bool isAsciiAlpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
inline bool isAsciiDigit(char c) { return c >= '0' && c <= '9'; }
inline bool isAsciiHexDigit(char c) {
    return isAsciiDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
/// Space, tab, CR or LF.
inline bool isWs(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/// `c` lowercased when it is an ASCII capital, else unchanged.
inline char toLowerChar(char c) {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return c;
}

inline std::string toLowerCopy(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(toLowerChar(c));
    }
    return out;
}

/// ASCII case-insensitive equality.
inline bool iequalsAscii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (toLowerChar(a[i]) != toLowerChar(b[i])) {
            return false;
        }
    }
    return true;
}

/// `s` without leading and trailing spaces, tabs, CRs and LFs, as a view into `s`.
inline std::string_view trimView(std::string_view s) {
    while (!s.empty() && isWs(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && isWs(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

/// `s` without leading and trailing spaces, tabs, CRs and LFs.
inline std::string trim(std::string_view s) { return std::string(trimView(s)); }

inline bool startsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

inline bool endsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// Encodes a Unicode code point as UTF-8.
inline std::string utf8Encode(uint32_t cp) {
    std::string out;
    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
    return out;
}

}  // namespace xt::util
