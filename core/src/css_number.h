#pragma once

#include <cmath>
#include <cstddef>
#include <string_view>
#include <system_error>
#include <type_traits>

#include "xt/charconv.h"

/// T-272: the number of CSS Syntax Level 3, exponent included, for every reader of a style or
/// attribute number: the runtime mini CSS tokenizer (mini_css.cpp) and, through compile/'s private
/// include path to core/src, the `.xts` lexer, binding and trigger arguments, `<camera>` attributes
/// and `.xtfx` parameters. Conversion goes through xt::fromChars, never the C locale.

namespace xt::css_number {

inline bool isDigit(char c) { return c >= '0' && c <= '9'; }

/// The length of the number at the start of `s`, or 0 when it does not start with one: an optional
/// sign, digits with an optional `.` and fraction digits (at least one digit in all), then an
/// optional exponent -- `e` or `E`, an optional sign and at least one digit. An `e` without a digit
/// after it is not an exponent: the number ends before it (`1em`, `2ex`, `1e` keep their unit).
inline size_t scan(std::string_view s) {
    size_t i = 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        ++i;
    }
    size_t digits = 0;
    while (i < s.size() && isDigit(s[i])) {
        ++i;
        ++digits;
    }
    if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && isDigit(s[i])) {
            ++i;
            ++digits;
        }
    }
    if (digits == 0) {
        return 0;
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        size_t j = i + 1;
        if (j < s.size() && (s[j] == '+' || s[j] == '-')) {
            ++j;
        }
        if (j < s.size() && isDigit(s[j])) {
            while (j < s.size() && isDigit(s[j])) {
                ++j;
            }
            i = j;
        }
    }
    return i;
}

/// True when the magnitude of `number` (a span scan() measured) is below 1: its leading non-zero
/// digit lands after the point once the exponent is applied. Tells an underflow from an overflow
/// when a conversion reports "out of range".
inline bool belowOne(std::string_view number) {
    size_t i = 0;
    if (i < number.size() && (number[i] == '+' || number[i] == '-')) {
        ++i;
    }
    long long integerDigits = 0;  // From the first non-zero digit before the point.
    long long fractionZeros = 0;  // Zeros between the point and the first non-zero digit.
    bool afterPoint = false;
    bool nonZero = false;
    for (; i < number.size() && (isDigit(number[i]) || number[i] == '.'); ++i) {
        if (number[i] == '.') {
            afterPoint = true;
        } else if (!afterPoint) {
            nonZero = nonZero || number[i] != '0';
            integerDigits += nonZero ? 1 : 0;
        } else if (!nonZero) {
            nonZero = number[i] != '0';
            fractionZeros += nonZero ? 0 : 1;
        }
    }
    long long exponent = 0;
    if (i < number.size()) {  // 'e' or 'E'.
        ++i;
        const bool negative = i < number.size() && number[i] == '-';
        if (i < number.size() && (number[i] == '+' || number[i] == '-')) {
            ++i;
        }
        for (; i < number.size() && exponent < 1000000; ++i) {
            exponent = exponent * 10 + (number[i] - '0');
        }
        exponent = negative ? -exponent : exponent;
    }
    const long long leading = integerDigits > 0 ? integerDigits - 1 : -(fractionZeros + 1);
    return leading + exponent < 0;
}

/// Converts `number` (exactly a span scan() measured) to float or double. A magnitude below the
/// type's range reads as zero; false (`out` untouched) when the value is beyond the range, that is
/// not finite -- the callers' "not a number" path.
template <typename T>
bool convert(std::string_view number, T& out) {
    static_assert(std::is_same_v<T, float> || std::is_same_v<T, double>);
    const char* first = number.data();
    const char* last = first + number.size();
    if (first != last && *first == '+') {
        ++first;  // std::from_chars rejects a leading '+'.
    }
    T value = 0;
    std::from_chars_result r = xt::fromChars(first, last, value);
    if (r.ec == std::errc::result_out_of_range) {
        if (!belowOne(number)) {
            return false;  // Overflow.
        }
        // Underflow (`1e-50`, `1e-999`): what the double reading rounds to, else zero.
        double wide = 0;
        const std::from_chars_result w = xt::fromChars(first, last, wide);
        out = w.ec == std::errc{} ? static_cast<T>(wide) : T(0);
        return true;
    }
    if (r.ec != std::errc{} || r.ptr != last || !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

/// scan() and convert() together: reads the number at the start of `s` into `out` and its length
/// into `length`. False (both untouched) when `s` does not start with a number or it is not finite.
template <typename T>
bool read(std::string_view s, T& out, size_t& length) {
    const size_t n = scan(s);
    if (n == 0 || !convert(s.substr(0, n), out)) {
        return false;
    }
    length = n;
    return true;
}

/// T-268: read() after the leading whitespace strtod skipped (space, \t \n \v \f \r), for the
/// editor readers that used to call strtod / strtof. `length` counts that whitespace too.
template <typename T>
bool readAfterSpace(std::string_view s, T& out, size_t& length) {
    size_t space = 0;
    while (space < s.size() && (s[space] == ' ' || (s[space] >= '\t' && s[space] <= '\r'))) {
        ++space;
    }
    size_t n = 0;
    if (!read(s.substr(space), out, n)) {
        return false;
    }
    length = space + n;
    return true;
}

}  // namespace xt::css_number
