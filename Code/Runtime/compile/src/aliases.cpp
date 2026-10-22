#include "xt/compile/aliases.h"

/// The alias registry (Docs/design/43-compatibility-and-versions.md §43.6, Docs/tasks/T-408.md).
/// Add a row when a name is renamed; never remove one while the alias is in its grace period.

namespace xt::compile {

namespace {

constexpr NameAlias kNameAliases[] = {
    // T-386 (08 §8.11): the DOM pointer spellings of press / release.
    {AliasKind::Trigger, "pointerdown", "press", "T-386"},
    {AliasKind::Trigger, "pointerup", "release", "T-386"},
    // T-392 (27 §27.3.5): the call forms of the state enter / exit triggers.
    {AliasKind::Trigger, "state-enter(<Name>)", "state(<Name>).enter", "T-392"},
    {AliasKind::Trigger, "state-exit(<Name>)", "state(<Name>).exit", "T-392"},
    // T-385 (27 §27.3.2): the built-in Disabled state has its own binding target.
    {AliasKind::Binding, "bind:state.Disabled", "bind:disabled", "T-385"},
};

}  // namespace

std::span<const NameAlias> nameAliases() { return kNameAliases; }

const NameAlias* findNameAlias(AliasKind kind, std::string_view name) {
    for (const NameAlias& alias : kNameAliases) {
        if (alias.kind == kind && name == alias.alias) {
            return &alias;
        }
    }
    return nullptr;
}

std::string deprecatedMessage(const NameAlias& alias) {
    return std::string(kDeprecatedPrefix) + alias.replacement + " instead of " + alias.alias;
}

}  // namespace xt::compile
