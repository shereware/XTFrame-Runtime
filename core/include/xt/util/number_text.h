#pragma once

#include <cmath>
#include <string>

#include "xt/charconv.h"

/// Locale-independent number text with trailing zeros trimmed, through xt::formatFixed
/// (14 §14.10). T-414 C2d: the editor wrote each of these out in several files.

namespace xt::util {

/// Two decimals, trailing zeros (and a bare '.') trimmed; "0" near zero (|value| < 0.005) -- the
/// numbers the vector tools write into `d` and shape attributes.
inline std::string formatNum2(float value) {
    if (std::fabs(value) < 0.005f) {
        return "0";
    }
    char buffer[48];
    std::string text(buffer, xt::formatFixed(buffer, sizeof(buffer), static_cast<double>(value), 2));
    while (!text.empty() && text.back() == '0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    return text;
}

/// Rounded to three decimals, trailing zeros (and a bare '.') trimmed; "-0" becomes "0".
inline std::string formatFloat3(float v) {
    char buf[64];
    std::string s(buf, xt::formatFixed(buf, sizeof(buf), static_cast<double>(v), 3));
    size_t dot = s.find('.');
    if (dot != std::string::npos) {
        size_t last = s.find_last_not_of('0');
        if (last == dot) {
            last -= 1;  // Strip the '.' too when nothing follows it.
        }
        s.erase(last + 1);
    }
    if (s == "-0") {
        s = "0";
    }
    return s;
}

}  // namespace xt::util
