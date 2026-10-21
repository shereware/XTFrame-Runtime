#include "base64.h"

namespace xt::compile {

namespace {

/// Returns the 0..63 value of a base64 alphabet character, or -1 if `c` is not in the alphabet.
int decodeChar(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return (c - 'a') + 26;
    }
    if (c >= '0' && c <= '9') {
        return (c - '0') + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

bool isWhitespace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

}  // namespace

bool base64Decode(std::string_view in, std::vector<uint8_t>& out) {
    out.clear();
    std::vector<int> sextets;
    sextets.reserve(in.size());
    bool sawPadding = false;
    for (char c : in) {
        if (isWhitespace(c)) {
            continue;
        }
        if (c == '=') {
            sawPadding = true;
            continue;
        }
        if (sawPadding) {
            // A non-padding, non-whitespace character after padding started: malformed.
            return false;
        }
        int v = decodeChar(c);
        if (v < 0) {
            return false;
        }
        sextets.push_back(v);
    }

    size_t n = sextets.size();
    size_t full = n / 4u * 4u;
    for (size_t i = 0; i < full; i += 4) {
        uint32_t v = (static_cast<uint32_t>(sextets[i]) << 18) |
                     (static_cast<uint32_t>(sextets[i + 1]) << 12) |
                     (static_cast<uint32_t>(sextets[i + 2]) << 6) | static_cast<uint32_t>(sextets[i + 3]);
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(v & 0xFF));
    }

    size_t rem = n - full;
    if (rem == 1) {
        // A single leftover sextet cannot decode to any byte: malformed.
        return false;
    }
    if (rem == 2) {
        uint32_t v =
            (static_cast<uint32_t>(sextets[full]) << 18) | (static_cast<uint32_t>(sextets[full + 1]) << 12);
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    } else if (rem == 3) {
        uint32_t v = (static_cast<uint32_t>(sextets[full]) << 18) |
                     (static_cast<uint32_t>(sextets[full + 1]) << 12) |
                     (static_cast<uint32_t>(sextets[full + 2]) << 6);
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    }
    return true;
}

}  // namespace xt::compile
