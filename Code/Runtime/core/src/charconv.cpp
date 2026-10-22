#include "xt/charconv.h"

#include <cerrno>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>

/// T-240: see xt/charconv.h. The fallback validates the span itself (so the accepted grammar is
/// exactly from_chars' general format) and converts the validated characters with strtod, which
/// the "C" locale every XTFrame host runs the runtime in reads with a '.' decimal point.

namespace xt {

namespace {

// XT_CHARCONV_FALLBACK forces the fallback (to test it where the library has the overloads).
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L && !defined(XT_CHARCONV_FALLBACK)
#define XT_HAVE_FLOAT_FROM_CHARS 1
#endif

#ifndef XT_HAVE_FLOAT_FROM_CHARS
bool isDigit(char c) { return c >= '0' && c <= '9'; }

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

/// True when [p, last) starts with `word` (lower case), ignoring case.
bool startsWithWord(const char* p, const char* last, const char* word) {
    for (; *word != '\0'; ++word, ++p) {
        if (p == last || lower(*p) != *word) {
            return false;
        }
    }
    return true;
}

/// The end of the longest from_chars-general number at `first`, or `first` when there is none.
const char* scanNumber(const char* first, const char* last) {
    const char* p = first;
    if (p != last && *p == '-') {
        ++p;
    }
    if (startsWithWord(p, last, "infinity")) {
        return p + 8;
    }
    if (startsWithWord(p, last, "inf") || startsWithWord(p, last, "nan")) {
        return p + 3;
    }
    const char* digits = p;
    while (p != last && isDigit(*p)) {
        ++p;
    }
    bool any = p != digits;
    if (p != last && *p == '.') {
        ++p;
        const char* fraction = p;
        while (p != last && isDigit(*p)) {
            ++p;
        }
        any = any || p != fraction;
    }
    if (!any) {
        return first;
    }
    if (p != last && (*p == 'e' || *p == 'E')) {
        const char* q = p + 1;
        if (q != last && (*q == '+' || *q == '-')) {
            ++q;
        }
        const char* exponent = q;
        while (q != last && isDigit(*q)) {
            ++q;
        }
        if (q != exponent) {
            p = q;
        }
    }
    return p;
}

template <typename T>
std::from_chars_result parse(const char* first, const char* last, T& value) {
    const char* end = scanNumber(first, last);
    if (end == first) {
        return {first, std::errc::invalid_argument};
    }
    const std::string text(first, end);
    errno = 0;
    char* stop = nullptr;
    const double parsed = std::strtod(text.c_str(), &stop);
    const bool overflow = errno == ERANGE && std::isinf(parsed);
    const auto result = static_cast<T>(parsed);
    if (overflow || (std::isinf(result) && !std::isinf(parsed))) {
        return {end, std::errc::result_out_of_range};
    }
    value = result;
    return {end, std::errc{}};
}
#endif

}  // namespace

std::from_chars_result fromChars(const char* first, const char* last, float& value) {
#ifdef XT_HAVE_FLOAT_FROM_CHARS
    return std::from_chars(first, last, value);
#else
    return parse(first, last, value);
#endif
}

std::from_chars_result fromChars(const char* first, const char* last, double& value) {
#ifdef XT_HAVE_FLOAT_FROM_CHARS
    return std::from_chars(first, last, value);
#else
    return parse(first, last, value);
#endif
}

#ifndef XT_HAVE_FLOAT_FROM_CHARS
namespace {

/// printf("%.*f") (`fixed`) or printf("%.*g") in the process locale, then the locale's decimal
/// separator back to '.'.
size_t formatPrintf(char* buffer, size_t size, double value, int precision, bool fixed) {
    const int needed = fixed ? std::snprintf(nullptr, 0, "%.*f", precision, value)
                             : std::snprintf(nullptr, 0, "%.*g", precision, value);
    if (needed <= 0) {
        return 0;
    }
    std::string text(static_cast<size_t>(needed) + 1, '\0');
    if (fixed) {
        std::snprintf(text.data(), text.size(), "%.*f", precision, value);
    } else {
        std::snprintf(text.data(), text.size(), "%.*g", precision, value);
    }
    text.resize(static_cast<size_t>(needed));
    const char* point = std::localeconv()->decimal_point;
    if (point != nullptr && point[0] != '\0' && std::strcmp(point, ".") != 0) {
        const size_t at = text.find(point);
        if (at != std::string::npos) {
            text.replace(at, std::strlen(point), ".");
        }
    }
    if (text.size() > size) {
        return 0;
    }
    std::memcpy(buffer, text.data(), text.size());
    return text.size();
}

}  // namespace
#endif

size_t formatFloat(char* buffer, size_t size, double value, int precision) {
#ifdef XT_HAVE_FLOAT_FROM_CHARS
    const std::to_chars_result r =
        std::to_chars(buffer, buffer + size, value, std::chars_format::general, precision);
    return r.ec == std::errc{} ? static_cast<size_t>(r.ptr - buffer) : 0;
#else
    return formatPrintf(buffer, size, value, precision, false);
#endif
}

size_t formatFixed(char* buffer, size_t size, double value, int decimals) {
#ifdef XT_HAVE_FLOAT_FROM_CHARS
    const std::to_chars_result r =
        std::to_chars(buffer, buffer + size, value, std::chars_format::fixed, decimals);
    return r.ec == std::errc{} ? static_cast<size_t>(r.ptr - buffer) : 0;
#else
    return formatPrintf(buffer, size, value, decimals, true);
#endif
}

}  // namespace xt
