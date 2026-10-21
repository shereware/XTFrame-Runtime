#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

/// The compiler's name alias registry (docs/design/43-compatibility-and-versions.md §43.6,
/// docs/tasks/T-408.md item 2): once a trigger, action, property, converter or binding name is in
/// beta it is never removed; a rename keeps the old spelling as an alias of the new one. An alias
/// compiles exactly like its replacement; with CompileOptions::strict or
/// CompileOptions::reportDeprecated (the editor preview, so `lint.run` sees it) each use is a
/// `deprecated: use <new> instead of <old>` warning that never fails a strict compile.

namespace xt::compile {

/// Which name table an alias belongs to.
enum class AliasKind : uint8_t {
    Trigger = 0,    ///< An `on:<event>` / `on-<event>` trigger event name.
    Action = 1,     ///< A trigger action name.
    Property = 2,   ///< A `.xts` property name.
    Converter = 3,  ///< A binding converter name.
    Binding = 4,    ///< A `bind:<target>` attribute name.
};

/// One registered alias.
struct NameAlias {
    AliasKind kind;
    /// The old spelling as written (`pointerdown`; a call form as `state-enter(<Name>)`).
    const char* alias;
    /// What to write instead (`press`; `state(<Name>).enter`).
    const char* replacement;
    /// The task that introduced the replacement (the alias stays at least one product MINOR).
    const char* since;
};

/// Every registered alias, grouped by kind.
std::span<const NameAlias> nameAliases();

/// The alias `name` of table `kind` (the spelling as registered), or null.
const NameAlias* findNameAlias(AliasKind kind, std::string_view name);

/// `deprecated: use <replacement> instead of <alias>`: the warning text of one use.
std::string deprecatedMessage(const NameAlias& alias);

/// The prefix every deprecation warning starts with (`deprecated: use `).
inline constexpr std::string_view kDeprecatedPrefix = "deprecated: use ";

}  // namespace xt::compile
