#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

/// Minimal RFC 4648 base64 decoder used by the Lottie image asset pipeline (embedded `data:` URIs).
/// See docs/tasks/T-16.md "base64.h".

namespace xt::compile {

/// Decodes standard-alphabet base64 text into `out` (appended; `out` is not cleared first by the
/// caller's convention -- callers pass a fresh vector). Whitespace (space/tab/CR/LF) anywhere in
/// `in` is ignored; a trailing `=`/`==` padding is tolerated (and not required -- an input whose
/// length after stripping whitespace is not a multiple of 4 is still accepted as long as the
/// trailing group has 2 or 3 data characters, matching common relaxed encoders). Any other
/// character (outside the base64 alphabet and not whitespace/`=`) makes this return false; `out`
/// is left in an unspecified state in that case.
bool base64Decode(std::string_view in, std::vector<uint8_t>& out);

}  // namespace xt::compile
