#pragma once

#include <span>
#include <string_view>

#include "xt/compile/validate.h"
#include "xt/ir_pod.h"

/// T-844: the compiler's side of the trigger event / action tables (validate.h's
/// triggerEventTable() / triggerActionTable(), Docs/design/27-interaction.md §27.2.2-27.2.3).

namespace xt::compile {

/// How parseEvent() reads an event row: a plain name, a `<name>(<button>)` filter of a plain name,
/// or a form with its own parsing code in parseEvent().
enum class TriggerEventForm : uint8_t { Simple, Button, Coded };

/// One row of the event table: the public entry plus what parseEvent() needs (`event` is set for
/// Simple rows only).
struct TriggerEventRow {
    TriggerEventInfo info;
    TriggerEventForm form;
    TriggerEvent event;
};

/// The rows, in the order of triggerEventTable() (row i is entry i).
std::span<const TriggerEventRow> triggerEventRows();

/// True when the action `name` may be written without parentheses (triggerActionTable()).
bool triggerActionBare(std::string_view name);

}  // namespace xt::compile
