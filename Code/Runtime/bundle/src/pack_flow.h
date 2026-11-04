#pragma once

#include <string>
#include <vector>

#include "xt/bundle/bundle.h"
#include "xt/document.h"
#include "xt/flow/flow_graph.h"

/// T-580 (Docs/design/22-bundle.md §22.15, 53-flow-language.md §53.101): the manifest's flow
/// interface as pack() builds it -- `ports.out[].origin`, the undeclared trigger / flow emit names
/// added to `ports.out`, `uis[].flowInterface` and the top-level `flowInterface`. Internal to
/// xt_bundle (pack.cpp is its only caller).

namespace xt::bundle::detail {

/// Adds every undeclared name a document trigger `emit(<name>)` sends (IR TRIG Emit actions with
/// no PORT index) to `ui.ports` as an output `document.<name>`, type "pulse", origin "trigger", in
/// action order. A name already listed keeps its first entry and adds one line to `infos`.
void addTriggerEmitPorts(const Document& doc, UiInfo& ui, std::vector<std::string>& infos);

/// Sets `ui.flowInterface` from the UI's own flow (`present` = true) and adds each emit name the
/// Document does not declare to `ui.ports` as an output `document.<name>` of the emit's type,
/// origin "flow" (a name already listed: first kept, one line to `infos`). An emit whose value
/// comes from a declared port takes that port's manifest type (so call after the ports are
/// reflected); `#<ui>.<port>` addresses use `ui.id`.
void applyUiFlowInterface(const flow::FlowHostInterface& flow, const Document& doc, UiInfo& ui,
                          std::vector<std::string>& infos);

/// Distributes the global flow's interface: a write / read addressed `ui#<id>` goes to that UI's
/// `flowInterface` (appended once, `present` set); everything else -- emits, system events and
/// addresses of a UI not in `uis` -- to `global` (`present` set). Returns the ids of the UIs that
/// received an entry, in `uis` order.
std::vector<std::string> applyGlobalFlowInterface(const flow::FlowHostInterface& flow,
                                                  std::vector<UiInfo>& uis, FlowInterface& global);

}  // namespace xt::bundle::detail
