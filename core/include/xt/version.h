#pragma once

/// Library version macros and query function.
/// 見 docs/tasks/T-01.md

#define XT_VERSION_MAJOR 0
#define XT_VERSION_MINOR 9
#define XT_VERSION_PATCH 0

namespace xt {

/// Returns "0.9.0".
const char* versionString();

}  // namespace xt
