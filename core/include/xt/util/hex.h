#pragma once

/// One hex digit's value (T-414 C2d; it used to be written out in bundle, compile, editor/core and
/// editor/history).

namespace xt::util {

/// The value of hex digit `c` (0-9, a-f, A-F), or -1 when `c` is not one.
inline int hexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

}  // namespace xt::util
