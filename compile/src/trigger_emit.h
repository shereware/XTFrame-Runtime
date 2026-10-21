#pragma once

#include <cstdint>
#include <string_view>

#include "bind_emit.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/dom.h"
#include "xt/compile/ir_writer.h"
#include "xt/compile/style_model.h"

/// Custom states and triggers -> the `STAT` and `TRIG` chunks (docs/design/27-interaction.md
/// §27.1-27.2, 03 §3.12.2-3.12.3, 12 §12.7, docs/tasks/T-174.md).

namespace xt::compile {

/// Compiles every custom state name (from `state-groups` attributes, `:state()` selectors and
/// trigger events/actions; at most kMaxCustomStates), the document-wide state groups, and one
/// trigger table per element: its `on:<event>` attributes plus the `.xts` `on-<event>` /
/// `state-enter` / `state-exit` declarations of the rules that match it statically (the
/// highest-specificity, then latest, declaration per event; an `.xt` attribute always wins).
/// Unknown events, actions, timelines, markers, focus targets or converters are errors. Returns
/// false if any error was reported. T-175: `port(<name>)` events, `emit(<port>)` actions and port
/// / percentage `if` arguments are resolved against `ports` in each element's scope; every
/// `bind:state.<Name>` attribute also registers its state name. T-411: `repeats` (may be null)
/// lets a `set()` value read `item(...)` / `repeat.index` / `repeat.count` inside a template.
bool emitTriggers(const DomTree& dom, const RuleSet& ruleSet, const PortScopes& ports, IrWriter& writer,
                  std::string_view fileName, DiagnosticSink& sink, CustomStateBuilder& states,
                  TriggerBuilder& triggers, const RepeatScopes* repeats = nullptr);

}  // namespace xt::compile
