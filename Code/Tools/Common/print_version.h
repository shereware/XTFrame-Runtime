#pragma once

/// T-420: the command-line tools' `--version` text (Docs/COMPATIBILITY.md §7), three fixed lines on
/// `out`:
///   <tool> <XT_VERSION_MAJOR>.<XT_VERSION_MINOR>.<XT_VERSION_PATCH>
///   ir-format <kIrFormatMajor>.<kIrFormatMinor>
///   behavior <kLatestBehavior>
/// The values come only from xt/version.h and xt/ir_pod.h. Header-only (Code/Tools/Common has no
/// library); needs xt_core.

#include <cstdio>

#include "xt/ir_pod.h"
#include "xt/version.h"

namespace xt::tools {

inline void printVersion(const char* tool, std::FILE* out) {
    std::fprintf(out, "%s %d.%d.%d\n", tool, XT_VERSION_MAJOR, XT_VERSION_MINOR, XT_VERSION_PATCH);
    std::fprintf(out, "ir-format %u.%u\n", static_cast<unsigned>(xt::kIrFormatMajor),
                 static_cast<unsigned>(xt::kIrFormatMinor));
    std::fprintf(out, "behavior %u\n", static_cast<unsigned>(xt::kLatestBehavior));
}

}  // namespace xt::tools
