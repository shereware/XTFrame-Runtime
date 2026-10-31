#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xt/bundle/design_system.h"

/// Helpers shared by design_system.cpp, design_io.cpp, design_graph.cpp and design_diff.cpp
/// (Docs/tasks/T-305.md). Internal to xt_bundle.

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

/// A CSS name character as the design tools scan it: ASCII letters, digits, `-`, `_` and every
/// non-ASCII byte (T-414: design_audit.cpp and design_graph.cpp had copies).
inline bool isNameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '_' || static_cast<unsigned char>(c) >= 0x80;
}

/// The last `#id` of each comma-separated selector of a rule prelude (design_graph.cpp).
std::vector<std::string> preludeIds(std::string_view prelude);

/// The `--name: value` declarations of the stylesheet at `path` (UTF-8): (name without `--`,
/// value), first declaration's order, last declaration's value; empty when `path` is empty or
/// cannot be read. `label` names the file in diagnostics (design_doc.cpp).
std::vector<std::pair<std::string, std::string>> readTokenDecls(const std::string& path,
                                                                const std::string& label);

/// The theme files of `project` (`shared.themes/*.xts`, sorted by path): (stem, file)
/// (design_export.cpp).
std::vector<std::pair<std::string, std::filesystem::path>> themeFiles(const UiProject& project);

/// §35.3.4: the category of a variable without metadata, from its name without `--` (the rule
/// syncTokenMeta() applies; design_io.cpp). T-498c: design_doc.cpp and design_export.cpp place the
/// inherited variables with it.
std::string guessTokenCategory(const std::string& name);

/// T-498c (49 §49.12 ruling): the implied default layer as the design tools see it -- the group at
/// BuildOptions::standardSymbolDir (loadGroup()), read with readTokenDecls(). Empty `id` when
/// `groupDir` is empty or holds no readable group with a `tokens.xts`.
struct ImpliedLayer {
    /// The group id (`broadsheet-2026`).
    std::string id;
    /// Every `--name: value` of the group's `tokens.xts` (name without `--`, source order).
    std::vector<std::pair<std::string, std::string>> tokens;
    /// The group's theme files (`group.json` `themes[]`, in that order) by theme name
    /// (themeNameOf(): `tokens-night.xts` -> `night`): name -> value.
    std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> themes;
    /// Token name (without `--`) -> the first `index.json` symbol that reads it.
    std::vector<std::pair<std::string, std::string>> firstUser;
};

/// Reads the implied layer of `groupDir` (UTF-8; may be empty). Never fails: an unreadable group
/// gives an empty `id`.
ImpliedLayer readImpliedLayer(const std::string& groupDir);

}  // namespace xt::bundle::design::detail
