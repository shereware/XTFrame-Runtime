#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "xt/bundle/design_system.h"

/// Helpers shared by design_system.cpp, design_io.cpp, design_graph.cpp and design_diff.cpp
/// (docs/tasks/T-305.md). Internal to xt_bundle.

namespace xt::bundle::design::detail {

/// `sha256:<64 lowercase hex digits>` of `bytes`.
std::string contentHash(std::string_view bytes);

/// A random version-4 GUID, `xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx`.
std::string newGuid();

/// The anchor of a Markdown heading text (§35.3.2 `text:voice#numbers`): ASCII letters lowercased,
/// digits, '_' and non-ASCII bytes kept, every other run of characters one '-', no '-' at either end.
std::string headingSlug(std::string_view heading);

/// The anchors of every ATX heading (`#`..`######`) of a Markdown body, outside fenced code blocks,
/// in order.
std::vector<std::string> headingSlugs(std::string_view body);

/// The status as written: the raw `status` text kept in `extra` (e.g. a decision's `accepted`),
/// else draft / approved / deprecated.
std::string statusText(const Entry& entry);

/// `ref` with a `token:--name` id rewritten to `token:name`; other references unchanged.
std::string normalizeRef(std::string_view ref);

/// The `:root` variable names (with `--`) of `project`'s `shared.tokens` stylesheet, in source
/// order, through the compiler's .xts parser (as pack's packTokens()); empty when there is none.
std::vector<std::string> sharedTokenNames(const UiProject& project);

}  // namespace xt::bundle::design::detail
