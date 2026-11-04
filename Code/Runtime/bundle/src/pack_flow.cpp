#include "pack_flow.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "xt/ir.h"
#include "xt/ir_pod.h"

/// See pack_flow.h (T-580).

namespace xt::bundle::detail {

namespace {

const char* flowTypeName(flow::PortType type) {
    switch (type) {
        case flow::PortType::Bool:
            return "bool";
        case flow::PortType::Number:
            return "number";
        case flow::PortType::String:
            return "string";
        case flow::PortType::Pulse:
        default:
            return "pulse";
    }
}

/// `#<ui>.<port>` (document level) or `#<ui>.<element>.<port>`.
std::string addressOf(const std::string& uiId, const flow::FlowPortRef& ref) {
    if (uiId.empty()) {  // A `#<id>.<port>` endpoint in the global flow (refused when a host binds it).
        return "#" + (ref.owner.empty() ? std::string("document") : ref.owner) + "." + ref.port;
    }
    if (ref.owner.empty() || ref.owner == "document") {
        return "#" + uiId + "." + ref.port;
    }
    return "#" + uiId + "." + ref.owner + "." + ref.port;
}

void addOnce(std::vector<std::string>& list, const std::string& value) {
    if (std::find(list.begin(), list.end(), value) == list.end()) {
        list.push_back(value);
    }
}

/// The lookup names of the Document's declared ports (an emit of one of them is not undeclared).
std::set<std::string> declaredNames(const Document& doc) {
    std::set<std::string> names;
    for (const PortRecord& port : doc.ports()) {
        names.insert(port.qualifiedName.empty() ? port.name : port.qualifiedName);
        names.insert(port.name);
    }
    return names;
}

/// The manifest type of the output `ui` lists for a declared port `ref` refers to; "string" (a
/// value of some kind) when it is not listed.
std::string declaredType(const UiInfo& ui, const flow::FlowPortRef& ref) {
    const std::string exact = (ref.owner.empty() || ref.owner == "document")
                                  ? "document." + ref.port
                                  : "#" + ref.owner + "." + ref.port;
    const std::string suffix = "." + ref.port;
    const PortInfo* loose = nullptr;
    for (const PortInfo& port : ui.ports) {
        if (port.isInput) {
            continue;
        }
        if (port.name == exact) {
            return port.type;
        }
        if (loose == nullptr && port.name.size() > suffix.size() &&
            port.name.compare(port.name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            loose = &port;
        }
    }
    return loose != nullptr ? loose->type : std::string("string");
}

/// Adds the output `document.<name>` unless an output of that name is listed already.
void addEmitPort(UiInfo& ui, const std::string& name, const std::string& type, const char* origin,
                 std::vector<std::string>& infos) {
    const std::string full = "document." + name;
    for (const PortInfo& port : ui.ports) {
        if (!port.isInput && port.name == full) {
            infos.push_back(ui.id + ": port '" + full + "' (" + origin + " emit) is already listed (origin " +
                            port.origin + "); the first is kept");
            return;
        }
    }
    PortInfo port;
    port.name = full;
    port.type = type;
    port.isInput = false;
    port.origin = origin;
    ui.ports.push_back(std::move(port));
}

FlowEmit emitOf(const flow::FlowEmitInfo& emit, const UiInfo* ui) {
    FlowEmit out{emit.name, flowTypeName(emit.type)};
    if (!emit.typeKnown) {
        out.type = ui != nullptr ? declaredType(*ui, emit.typeSource) : std::string("string");
    }
    return out;
}

}  // namespace

void addTriggerEmitPorts(const Document& doc, UiInfo& ui, std::vector<std::string>& infos) {
    const IrView& ir = doc.ir();
    std::vector<std::string> names;
    for (const IrTriggerAction& action : ir.triggers().actions()) {
        if (action.kind != static_cast<uint8_t>(TriggerAction::Emit) || action.arg1 != 0) {
            continue;  // Not an emit, or an emit of a declared port (PORT index + 1).
        }
        const std::string name(ir.string(action.arg0));
        if (!name.empty()) {
            addOnce(names, name);
        }
    }
    if (names.empty()) {
        return;
    }
    const std::set<std::string> declared = declaredNames(doc);
    for (const std::string& name : names) {
        if (declared.count(name) == 0) {
            addEmitPort(ui, name, "pulse", "trigger", infos);
        }
    }
}

void applyUiFlowInterface(const flow::FlowHostInterface& flow, const Document& doc, UiInfo& ui,
                          std::vector<std::string>& infos) {
    FlowInterface& out = ui.flowInterface;
    out.present = true;
    const std::set<std::string> declared = declaredNames(doc);
    for (const flow::FlowEmitInfo& emit : flow.emits) {
        const FlowEmit entry = emitOf(emit, &ui);
        out.emits.push_back(entry);
        if (declared.count(emit.name) == 0) {
            addEmitPort(ui, entry.name, entry.type, "flow", infos);
        }
    }
    for (const flow::FlowPortRef& ref : flow.writes) {
        addOnce(out.writes, addressOf(ui.id, ref));
    }
    for (const flow::FlowPortRef& ref : flow.reads) {
        addOnce(out.reads, addressOf(ui.id, ref));
    }
    out.system = flow.system;
}

std::vector<std::string> applyGlobalFlowInterface(const flow::FlowHostInterface& flow,
                                                  std::vector<UiInfo>& uis, FlowInterface& global) {
    global.present = true;
    std::vector<std::string> touched;
    const auto uiOf = [&](const std::string& id) -> UiInfo* {
        for (UiInfo& ui : uis) {
            if (ui.id == id) {
                return &ui;
            }
        }
        return nullptr;
    };
    const auto place = [&](const flow::FlowPortRef& ref, bool write) {
        UiInfo* ui = ref.uiId.empty() ? nullptr : uiOf(ref.uiId);
        const std::string address = addressOf(ref.uiId, ref);
        if (ui == nullptr) {
            addOnce(write ? global.writes : global.reads, address);
            return;
        }
        ui->flowInterface.present = true;
        addOnce(write ? ui->flowInterface.writes : ui->flowInterface.reads, address);
        addOnce(touched, ui->id);
    };
    for (const flow::FlowPortRef& ref : flow.writes) {
        place(ref, true);
    }
    for (const flow::FlowPortRef& ref : flow.reads) {
        place(ref, false);
    }
    for (const flow::FlowEmitInfo& emit : flow.emits) {
        global.emits.push_back(emitOf(emit, emit.typeKnown ? nullptr : uiOf(emit.typeSource.uiId)));
    }
    global.system = flow.system;
    // `touched` in `uis` order, so the caller's per-UI work is deterministic.
    std::vector<std::string> ordered;
    for (const UiInfo& ui : uis) {
        if (std::find(touched.begin(), touched.end(), ui.id) != touched.end()) {
            ordered.push_back(ui.id);
        }
    }
    return ordered;
}

}  // namespace xt::bundle::detail
