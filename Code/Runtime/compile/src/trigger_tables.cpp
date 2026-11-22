#include "trigger_tables.h"

#include <array>
#include <cstddef>

/// T-844 (Docs/design/27-interaction.md §27.2.2-27.2.3): the one list of the trigger events and
/// actions the compiler accepts. parseEvent() (trigger_emit.cpp) takes its plain names and button
/// filters from the event rows; the editor's trigger panel lists the entries that are not aliases.
/// The action table gives the names and which of them may be written without parentheses
/// (makeAction() and validateTriggerActions() both read that flag).

namespace xt::compile {

namespace {

using K = TriggerEventArgKind;
using F = TriggerEventForm;
using E = TriggerEvent;

constexpr TriggerEventRow kEventRows[] = {
    {{"hover", 0, {K::Text, K::Text}, false}, F::Simple, E::Hover},
    {{"leave", 0, {K::Text, K::Text}, false}, F::Simple, E::Leave},
    {{"press", 0, {K::Text, K::Text}, false}, F::Simple, E::Press},
    {{"release", 0, {K::Text, K::Text}, false}, F::Simple, E::Release},
    {{"click", 0, {K::Text, K::Text}, false}, F::Simple, E::Click},
    {{"dblclick", 0, {K::Text, K::Text}, false}, F::Simple, E::DoubleClick},  // T-379 (08.10).
    // T-390 (08.13): the button filters.
    {{"press({0})", 1, {K::Text, K::Text}, false}, F::Button, E::None},
    {{"release({0})", 1, {K::Text, K::Text}, false}, F::Button, E::None},
    {{"click({0})", 1, {K::Text, K::Text}, false}, F::Button, E::None},
    {{"dblclick({0})", 1, {K::Text, K::Text}, false}, F::Button, E::None},
    {{"pointermove", 0, {K::Text, K::Text}, false}, F::Simple, E::PointerMove},  // T-386 (08.11).
    {{"pointermove({0})", 1, {K::Text, K::Text}, false}, F::Button, E::None},
    {{"pointercancel", 0, {K::Text, K::Text}, false}, F::Simple, E::PointerCancel},
    {{"pointerdrag", 0, {K::Text, K::Text}, false}, F::Simple, E::PointerDrag},    // T-394 (08.15).
    {{"scroll", 0, {K::Text, K::Text}, false}, F::Simple, E::Scroll},              // T-387 (08.12).
    {{"outsidepress", 0, {K::Text, K::Text}, false}, F::Simple, E::OutsidePress},  // T-387.
    {{"focus", 0, {K::Text, K::Text}, false}, F::Simple, E::Focus},
    {{"blur", 0, {K::Text, K::Text}, false}, F::Simple, E::Blur},
    {{"focusin", 0, {K::Text, K::Text}, false}, F::Simple, E::FocusIn},    // T-397 (08.17).
    {{"focusout", 0, {K::Text, K::Text}, false}, F::Simple, E::FocusOut},  // T-397 (08.17).
    {{"check", 0, {K::Text, K::Text}, false}, F::Simple, E::Check},
    {{"uncheck", 0, {K::Text, K::Text}, false}, F::Simple, E::Uncheck},
    {{"change", 0, {K::Text, K::Text}, false}, F::Simple, E::Change},  // T-392 (27.3.5).
    {{"input", 0, {K::Text, K::Text}, false}, F::Simple, E::Input},    // T-392 (27.3.5).
    {{"show", 0, {K::Text, K::Text}, false}, F::Simple, E::Show},
    {{"hide", 0, {K::Text, K::Text}, false}, F::Simple, E::Hide},
    {{"port({0})", 1, {K::Port, K::Text}, false}, F::Coded, E::None},
    {{"timeline({0}).end", 1, {K::Timeline, K::Text}, false}, F::Coded, E::None},
    {{"timeline({0}).marker({1})", 2, {K::Timeline, K::Text}, false}, F::Coded, E::None},
    {{"state({0}).enter", 1, {K::State, K::Text}, false}, F::Coded, E::None},
    {{"state({0}).exit", 1, {K::State, K::Text}, false}, F::Coded, E::None},
    {{"key({0})", 1, {K::Text, K::Text}, false}, F::Coded, E::None},
    {{"key({0}).release", 1, {K::Text, K::Text}, false}, F::Coded, E::None},  // T-871 (60 §60.3.2).
    {{"wheel", 0, {K::Text, K::Text}, false}, F::Simple, E::Wheel},           // T-871 (60 §60.3.2).
    // T-873 (60 §60.3.2): input channels.
    {{"input({0})", 1, {K::Text, K::Text}, false}, F::Coded, E::None},
    {{"input({0}).press", 1, {K::Text, K::Text}, false}, F::Coded, E::None},
    {{"input({0}).release", 1, {K::Text, K::Text}, false}, F::Coded, E::None},
    {{"media({0}).end", 1, {K::Media, K::Text}, false}, F::Coded, E::None},   // T-191 (13.8.3).
    {{"item-enter", 0, {K::Text, K::Text}, false}, F::Simple, E::ItemEnter},  // T-177.
    {{"item-exit", 0, {K::Text, K::Text}, false}, F::Simple, E::ItemExit},    // T-177.
    // T-194 (27.2.2, 27.7); T-388: drag.cancel; T-201: drag.value / nav.value.
    {{"drag.start", 0, {K::Text, K::Text}, false}, F::Coded, E::None},
    {{"drag.over", 0, {K::Text, K::Text}, false}, F::Coded, E::None},
    {{"drag.leave", 0, {K::Text, K::Text}, false}, F::Coded, E::None},
    {{"drop", 0, {K::Text, K::Text}, false}, F::Coded, E::None},
    {{"drag.cancel", 0, {K::Text, K::Text}, false}, F::Coded, E::None},
    {{"drag.value", 0, {K::Text, K::Text}, false}, F::Coded, E::None},
    {{"back", 0, {K::Text, K::Text}, false}, F::Coded, E::None},
    {{"nav.value", 0, {K::Text, K::Text}, false}, F::Coded, E::None},
    {{"nav.blocked", 0, {K::Text, K::Text}, false}, F::Coded, E::None},
    {{"nav.blocked({0})", 1, {K::Text, K::Text}, false}, F::Coded, E::None},
    {{"gesture({0})", 1, {K::Text, K::Text}, false}, F::Coded, E::None},
    {{"player({0}).{1}", 2, {K::Text, K::Text}, false}, F::Coded, E::None},
    // Aliases: normalized to an entry above (T-386, T-392, T-408; `drag.drop` = `drop`, T-194).
    {{"pointerdown", 0, {K::Text, K::Text}, true}, F::Simple, E::Press},
    {{"pointerup", 0, {K::Text, K::Text}, true}, F::Simple, E::Release},
    {{"pointerdown({0})", 1, {K::Text, K::Text}, true}, F::Button, E::None},
    {{"pointerup({0})", 1, {K::Text, K::Text}, true}, F::Button, E::None},
    {{"drag.drop", 0, {K::Text, K::Text}, true}, F::Coded, E::None},
    {{"state-enter({0})", 1, {K::State, K::Text}, true}, F::Coded, E::None},
    {{"state-exit({0})", 1, {K::State, K::Text}, true}, F::Coded, E::None},
};

constexpr std::array<TriggerEventInfo, std::size(kEventRows)> eventInfos() {
    std::array<TriggerEventInfo, std::size(kEventRows)> out{};
    for (size_t i = 0; i < std::size(kEventRows); ++i) {
        out[i] = kEventRows[i].info;
    }
    return out;
}

constexpr std::array<TriggerEventInfo, std::size(kEventRows)> kEventInfos = eventInfos();

constexpr TriggerActionInfo kActions[] = {
    {"play", false},   {"stop", false},   {"seek", false},    {"pause", false},  // T-573: pause/resume.
    {"resume", false}, {"state", false},  {"class", false},   {"show", true},
    {"hide", true},    {"sound", false},  {"set", false},     {"emit", false},
    {"focus", false},  {"haptic", false}, {"spawn", false},   {"layer", false},
    {"remove", true},  {"reset", false},  {"consume", false},  // T-177, T-399, T-394.
    {"add", false},                                            // T-871 (60 §60.3.4).
};

}  // namespace

std::span<const TriggerEventInfo> triggerEventTable() { return kEventInfos; }

std::span<const TriggerEventRow> triggerEventRows() { return kEventRows; }

std::span<const TriggerActionInfo> triggerActionTable() { return kActions; }

bool triggerActionBare(std::string_view name) {
    for (const TriggerActionInfo& a : kActions) {
        if (name == a.name) {
            return a.bare;
        }
    }
    return false;
}

}  // namespace xt::compile
