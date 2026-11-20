#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "symbol_internal.h"
#include "xt/bundle/project_style.h"
#include "xt/bundle/symbol_lint.h"

/// T-743: the helpers project_style.cpp, symbol_detach.cpp and symbol_lint_tokens.cpp share
/// (Docs/design/56-symbol-identity-style-scope.md 56.2). Internal to xt_bundle.

namespace xt::bundle::detail::style {

/// The path a `url("...")` / `url(...)` value names; empty when the value has none.
std::string urlPath(std::string_view value);

/// The custom properties `text` reads through `var(--name)`, with the dashes (`--name`), in
/// first-use order, unique.
std::vector<std::string> varRefs(std::string_view text);

/// `text` with the value of every declaration of the custom property `name` (`--x: value;` outside
/// comments and strings) replaced by `value`; `*found` (may be null) says whether there was one.
std::string replaceTokenValue(std::string text, std::string_view name, std::string_view value,
                              bool* found = nullptr);

/// `text` (the file's content; empty when it does not exist yet, `created` is then its first line)
/// with one section appended: `/* <title> */`, `faces` (complete `@font-face { ... }` lines), then
/// `:root { <name>: <value>; ... }` when `tokens` is not empty.
std::string appendTokenSection(std::string text, bool existed, const std::string& created,
                               const std::string& title, const std::vector<std::string>& faces,
                               const std::vector<std::pair<std::string, std::string>>& tokens);

/// For each custom property `text` declares inside a `/* added with <Name> from <group> */`
/// section (until the next such comment), `<group>`; a declaration before any section is `project`.
std::map<std::string, std::string> sectionOrigins(std::string_view text);

/// One symbol file of the project: `<shared.symbols>/<Name>.xt` or `<shared.symbols>/<ns>/<Name>.xt`.
struct ProjectSymbolFile {
    std::string qualified;
    std::filesystem::path path;
};
/// The project's symbol files, flat ones first, each set sorted by name.
std::vector<ProjectSymbolFile> projectSymbolFiles(const UiProject& project);

/// The project root, absolute and normalised; empty for an unnamed project.
std::filesystem::path projectRoot(const UiProject& project);

/// The value text of every token the files declare (a later file and a later declaration win, as
/// the compiler resolves them), each trimmed.
std::map<std::string, std::string> tokenValues(const std::vector<std::filesystem::path>& files);

/// `value` with each `var(--x)` (and `var(--x, fallback)`) replaced by `values[--x]`, recursively
/// (at most 8 levels); `*ok` false when a reference has no value.
std::string resolveVars(const std::string& value, const std::map<std::string, std::string>& values, bool* ok);

/// The `index.json` path of `project` (`<shared.symbols>/index.json`); false when it leaves the root.
bool projectIndexPath(const UiProject& project, std::filesystem::path& out);

/// The content of a file or empty; `*existed` (may be null) says whether it could be read.
std::string readText(const std::filesystem::path& file, bool* existed = nullptr);

}  // namespace xt::bundle::detail::style

namespace xt::bundle::detail::lint {

/// T-743 (47 rules 1-3): the token findings of a group whose base token sheet is `base` and theme
/// sheets `themes` -- `token.core` (error) for a missing core token or one of the wrong type,
/// `token.role` (warning) for an extension colour token not expressed with core tokens and not set
/// in every theme file, and for an extension `--tex-*` / `--font-*` token the preset group
/// (`presetGroupDir`, skipped when empty) also declares with another file / family.
std::vector<LintIssue> tokenVocabularyIssues(const TokenSheet& base, const std::vector<TokenSheet>& themes,
                                             const std::string& presetGroupDir);

}  // namespace xt::bundle::detail::lint
