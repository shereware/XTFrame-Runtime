#pragma once

#include <cstdio>

/// T-261e: the one place the runtime layers open a file a caller named. Every path a public
/// interface of xt_core, xt_sw and xt_text takes is UTF-8; on Windows it is opened through the
/// wide API, so a name the ANSI code page cannot spell (`アイコン.png` under CP950) opens too.
/// Internal: other runtime modules reach it by adding Code/Runtime/core/src to their private include path.

namespace xt {

/// `std::fopen(utf8Path, mode)`, except that on Windows `utf8Path` is converted from UTF-8 and
/// opened with `_wfopen_s`. nullptr when it cannot be opened; T-261f: a path that is not valid
/// UTF-8 is treated as missing (nullptr, as for a file that does not exist).
std::FILE* openFileUtf8(const char* utf8Path, const char* mode);

}  // namespace xt
