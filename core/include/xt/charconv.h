#pragma once

#include <charconv>
#include <cstddef>

/// T-240: floating-point std::from_chars for every standard library XTFrame builds with. The
/// Android NDK's libc++ (r27, LLVM 18) declares the floating-point overloads deleted, so the
/// runtime calls these instead; where the library has them (MSVC STL, libstdc++, a newer libc++)
/// they forward to std::from_chars.

namespace xt {

/// std::from_chars(first, last, value) with std::chars_format::general semantics: an optional
/// '-', decimal digits with an optional '.', an optional exponent, or "inf"/"infinity"/"nan"
/// (any case); no leading whitespace or '+'. On success `value` is set and `ptr` points past the
/// number; on failure `ec` is std::errc::invalid_argument (`ptr` = `first`) or
/// std::errc::result_out_of_range, and `value` is unchanged.
std::from_chars_result fromChars(const char* first, const char* last, float& value);
/// The double form of fromChars(first, last, float&).
std::from_chars_result fromChars(const char* first, const char* last, double& value);

/// T-268: writes `value` like printf("%.<precision>g") in the C locale: '.' as the decimal separator
/// whatever the process locale is. Returns the number of characters written (0 if the buffer
/// is too small); no terminating NUL.
size_t formatFloat(char* buffer, size_t size, double value, int precision);

/// T-268: writes `value` like printf("%.<decimals>f") in the C locale. Same contract as formatFloat:
/// returns the number of characters written (0 if the buffer is too small); no terminating NUL.
size_t formatFixed(char* buffer, size_t size, double value, int decimals);

}  // namespace xt
