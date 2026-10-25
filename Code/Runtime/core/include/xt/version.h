#pragma once

#include <cstdint>

/// Library version macros and query function.
/// 見 Docs/tasks/T-01.md

#define XT_VERSION_MAJOR 0
#define XT_VERSION_MINOR 11
#define XT_VERSION_PATCH 0

namespace xt {

/// Returns "0.11.0".
const char* versionString();

/// T-407 (Docs/design/43-compatibility-and-versions.md §43.3): the behavior version a project's
/// `spec.behavior` (and its IR's META `behavior`) selects. Each value is every flag before it; code
/// tests `behavior >= kBehavior<Flag>`, never a bit set. 0 = the pre-beta behavior.
/// 1: `flow-signal-coalescing` -- value-type connection points, binding re-evaluation and
/// continuous-event triggers are coalesced per frame (27 §27.4.1, T-389).
inline constexpr uint32_t kBehaviorFlowSignalCoalescing = 1;
/// T-407: the newest behavior version; what a new UI Project writes.
inline constexpr uint32_t kLatestBehavior = 1;

}  // namespace xt
