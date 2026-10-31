#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "capi_internal.h"
#include "xt/document.h"
#include "xt/value.h"

/// Declared ports, connection points and the callbacks of a surface (Docs/design/50-c-abi.md
/// §50.3 "declared ports" .. "callbacks"; Docs/tasks/T-504.md item 7).

namespace xt::capi {

namespace {

/// PortValue (connection points, in-port listeners) -> the event's value fields. `text` receives
/// the text form the event points at.
void fillValue(const flow::PortValue& value, xt_port_event& e, std::string& text) {
    switch (value.type) {
        case flow::PortType::Pulse:
            break;
        case flow::PortType::Bool:
            e.boolean = value.b ? 1 : 0;
            e.number = value.b ? 1.0 : 0.0;
            text = value.b ? "true" : "false";
            break;
        case flow::PortType::Number:
            e.number = value.n;
            text = numberText(value.n);
            break;
        case flow::PortType::String:
            text = value.s;
            break;
    }
    e.text = text.c_str();
}

void dispatch(xt_surface& s, const xt_port_event& e) {
    if (s.portFn != nullptr) {
        s.portFn(&s, &e, s.portUser);
    }
}

/// One declared port row of the bound Document; a default record (id 0) when unknown.
PortRecord portRow(xt_surface& s, PortId port) {
    Document* doc = s.ctx->document(s.handle);
    return doc != nullptr ? doc->portInfo(port) : PortRecord{};
}

/// INVALID_ARGUMENT (with the last error) unless `port` is a declared port of the bound Document.
xt_status checkPort(xt_surface& s, PortId port, const char* function) {
    if (port == kInvalidPort || portRow(s, port).id == kInvalidPort) {
        return fail(XT_STATUS_INVALID_ARGUMENT,
                    std::string(function) + ": no declared port " + std::to_string(port));
    }
    return XT_STATUS_OK;
}

xt_status portResult(Status status, const char* function, PortId port) {
    if (status != Status::kOk) {
        return fail(toCapi(status), std::string(function) + ": port " + std::to_string(port) +
                                        " rejected the value (" + statusName(status) + ")");
    }
    return XT_STATUS_OK;
}

xt_status connectionResult(xt_surface& s, Status status, const char* function) {
    if (status != Status::kOk) {
        return fail(toCapi(status), std::string(function) + ": " + s.ports->lastError());
    }
    return XT_STATUS_OK;
}

}  // namespace

void attachEmitListener(xt_surface& s) {
    xt_surface* raw = &s;
    s.ports->addEmitListener([raw](PortId port, const Value& value, uint32_t instanceId) {
        if (raw->portFn == nullptr) {
            return;
        }
        callbackGuard("the port callback (emit)", [&] {
            Document* doc = raw->ctx->document(raw->handle);
            const PortRecord info = doc != nullptr ? doc->portInfo(port) : PortRecord{};
            const std::string name = info.qualifiedName.empty() ? info.name : info.qualifiedName;
            xt_port_event e{};
            e.kind = XT_PORT_EMIT;
            e.port = port;
            e.name = name.c_str();
            e.value_type = static_cast<int>(info.type);
            e.instance_id = instanceId;
            // HostPorts' emit listener carries no PortEvent::itemIndex.
            e.item_index = 0xFFFFFFFFu;
            // 50.3: Float -> the number text (Document::portText's form), Bool -> "true"/"false",
            // String -> the text, anything else -> Document::portText(port).
            std::string text;
            if (info.type == PortValueType::Bool) {
                const bool on = value.type == ValueType::Float && value.f != 0.0f;
                e.boolean = on ? 1 : 0;
                e.number = on ? 1.0 : 0.0;
                text = on ? "true" : "false";
            } else if (value.type == ValueType::Float) {
                e.number = static_cast<double>(value.f);
                text = numberText(e.number);
            } else if (value.type == ValueType::StringRef && doc != nullptr) {
                text = std::string(doc->string(value.sid));
            } else if (doc != nullptr) {
                text = doc->portText(port);
            }
            e.text = text.c_str();
            dispatch(*raw, e);
        });
    });
}

void attachPortListeners(xt_surface& s) {
    for (uint32_t id : s.inputListeners) {
        s.ports->removeInputListener(id);
    }
    s.inputListeners.clear();
    s.portTable = s.ports->portTable();
    xt_surface* raw = &s;
    for (const PortRecord& row : s.portTable) {
        if (row.dir != PortDir::In) {
            continue;
        }
        const PortId port = row.id;
        const int valueType = static_cast<int>(row.type);
        const std::string name = row.qualifiedName.empty() ? row.name : row.qualifiedName;
        const uint32_t id =
            s.ports->addInputListener(port, [raw, port, valueType, name](const flow::PortValue& v) {
                if (raw->portFn == nullptr) {
                    return;
                }
                callbackGuard("the port callback (input changed)", [&] {
                    xt_port_event e{};
                    e.kind = XT_PORT_INPUT_CHANGED;
                    e.port = port;
                    e.name = name.c_str();
                    e.value_type = valueType;
                    e.instance_id = raw->ctx->instanceId(raw->handle);
                    e.item_index = 0xFFFFFFFFu;
                    std::string text;
                    fillValue(v, e, text);
                    dispatch(*raw, e);
                });
            });
        if (id != 0) {
            s.inputListeners.push_back(id);
        }
    }
}

}  // namespace xt::capi

using xt::capi::fail;
using xt::capi::toCapi;

extern "C" {

XT_CAPI uint32_t xt_surface_port_count(const xt_surface* s) {
    XT_CAPI_GUARD_OR("xt_surface_port_count", uint32_t, 0u,
                     { return s == nullptr ? 0u : static_cast<uint32_t>(s->portTable.size()); });
}

XT_CAPI xt_status xt_surface_port_record(const xt_surface* s, uint32_t index, xt_port_record* out) {
    XT_CAPI_GUARD("xt_surface_port_record", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_port_record");
        if (out == nullptr) {
            return fail(XT_STATUS_INVALID_ARGUMENT, "xt_surface_port_record: out is NULL");
        }
        if (index >= s->portTable.size()) {
            return fail(XT_STATUS_OUT_OF_RANGE, "xt_surface_port_record: index " + std::to_string(index) +
                                                    " >= port count " + std::to_string(s->portTable.size()));
        }
        auto* self = const_cast<xt_surface*>(s);
        const xt::PortRecord& row = s->portTable[index];
        out->id = row.id;
        out->name = self->scratch.put(row.name);
        out->qualified_name = self->scratch.put(row.qualifiedName);
        out->element_id = self->scratch.put(row.elementId);
        out->value_type = static_cast<int>(row.type);
        out->dir = static_cast<int>(row.dir);
        out->document_level = row.documentLevel ? 1 : 0;
        return XT_STATUS_OK;
    });
}

XT_CAPI uint32_t xt_surface_port_lookup(const xt_surface* s, const char* name) {
    XT_CAPI_GUARD_OR("xt_surface_port_lookup", uint32_t, XT_INVALID_PORT, {
        if (s == nullptr || name == nullptr) {
            return XT_INVALID_PORT;
        }
        return s->ports->lookup(name);
    });
}

XT_CAPI xt_status xt_surface_port_set_number(xt_surface* s, uint32_t port, double value) {
    XT_CAPI_GUARD("xt_surface_port_set_number", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_port_set_number");
        if (xt_status st = xt::capi::checkPort(*s, port, "xt_surface_port_set_number"); st != XT_STATUS_OK) {
            return st;
        }
        xt::capi::DispatchScope scope(*s);
        return xt::capi::portResult(s->ports->set(port, xt::Value::ofFloat(static_cast<float>(value))),
                                    "xt_surface_port_set_number", port);
    });
}

XT_CAPI xt_status xt_surface_port_set_bool(xt_surface* s, uint32_t port, int value) {
    XT_CAPI_GUARD("xt_surface_port_set_bool", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_port_set_bool");
        if (xt_status st = xt::capi::checkPort(*s, port, "xt_surface_port_set_bool"); st != XT_STATUS_OK) {
            return st;
        }
        xt::capi::DispatchScope scope(*s);
        return xt::capi::portResult(s->ports->set(port, xt::Value::ofFloat(value != 0 ? 1.0f : 0.0f)),
                                    "xt_surface_port_set_bool", port);
    });
}

XT_CAPI xt_status xt_surface_port_set_text(xt_surface* s, uint32_t port, const char* text) {
    XT_CAPI_GUARD("xt_surface_port_set_text", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_port_set_text");
        if (text == nullptr) {
            return fail(XT_STATUS_INVALID_ARGUMENT, "xt_surface_port_set_text: text is NULL");
        }
        if (xt_status st = xt::capi::checkPort(*s, port, "xt_surface_port_set_text"); st != XT_STATUS_OK) {
            return st;
        }
        xt::capi::DispatchScope scope(*s);
        return xt::capi::portResult(s->ports->setText(port, text), "xt_surface_port_set_text", port);
    });
}

XT_CAPI xt_status xt_surface_port_pulse(xt_surface* s, uint32_t port) {
    XT_CAPI_GUARD("xt_surface_port_pulse", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_port_pulse");
        if (xt_status st = xt::capi::checkPort(*s, port, "xt_surface_port_pulse"); st != XT_STATUS_OK) {
            return st;
        }
        if (xt::capi::portRow(*s, port).type != xt::PortValueType::Event) {
            return fail(XT_STATUS_INVALID_ARGUMENT,
                        "xt_surface_port_pulse: port " + std::to_string(port) + " is not an event port");
        }
        xt::capi::DispatchScope scope(*s);
        return xt::capi::portResult(s->ports->set(port, xt::Value::ofFloat(0.0f)), "xt_surface_port_pulse",
                                    port);
    });
}

XT_CAPI double xt_surface_port_get_number(const xt_surface* s, uint32_t port) {
    XT_CAPI_GUARD_OR("xt_surface_port_get_number", double, 0.0, {
        if (s == nullptr) {
            return 0.0;
        }
        const xt::Value v = s->ports->get(port);
        if (v.type == xt::ValueType::Float) {
            return static_cast<double>(v.f);
        }
        if (v.type == xt::ValueType::Enum) {
            return static_cast<double>(v.e);
        }
        return 0.0;
    });
}

XT_CAPI int xt_surface_port_get_bool(const xt_surface* s, uint32_t port) {
    XT_CAPI_GUARD_OR("xt_surface_port_get_bool", int, 0,
                     { return xt_surface_port_get_number(s, port) != 0.0 ? 1 : 0; });
}

XT_CAPI const char* xt_surface_port_get_text(const xt_surface* s, uint32_t port) {
    XT_CAPI_GUARD_OR("xt_surface_port_get_text", const char*, "", {
        if (s == nullptr) {
            return "";
        }
        auto* self = const_cast<xt_surface*>(s);
        xt::Document* doc = self->ctx->document(self->handle);
        return self->scratch.put(doc != nullptr ? doc->portText(port) : std::string());
    });
}

XT_CAPI xt_status xt_surface_connection_set_number(xt_surface* s, const char* port, double value,
                                                   const char* param) {
    XT_CAPI_GUARD("xt_surface_connection_set_number", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_connection_set_number");
        if (port == nullptr) {
            return fail(XT_STATUS_INVALID_ARGUMENT, "xt_surface_connection_set_number: port is NULL");
        }
        xt::flow::PortValue v;
        v.type = xt::flow::PortType::Number;
        v.n = value;
        xt::capi::DispatchScope scope(*s);
        return xt::capi::connectionResult(*s, s->ports->setInput(port, v, param != nullptr ? param : ""),
                                          "xt_surface_connection_set_number");
    });
}

XT_CAPI xt_status xt_surface_connection_set_bool(xt_surface* s, const char* port, int value,
                                                 const char* param) {
    XT_CAPI_GUARD("xt_surface_connection_set_bool", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_connection_set_bool");
        if (port == nullptr) {
            return fail(XT_STATUS_INVALID_ARGUMENT, "xt_surface_connection_set_bool: port is NULL");
        }
        xt::flow::PortValue v;
        v.type = xt::flow::PortType::Bool;
        v.b = value != 0;
        xt::capi::DispatchScope scope(*s);
        return xt::capi::connectionResult(*s, s->ports->setInput(port, v, param != nullptr ? param : ""),
                                          "xt_surface_connection_set_bool");
    });
}

XT_CAPI xt_status xt_surface_connection_set_text(xt_surface* s, const char* port, const char* text,
                                                 const char* param) {
    XT_CAPI_GUARD("xt_surface_connection_set_text", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_connection_set_text");
        if (port == nullptr || text == nullptr) {
            return fail(XT_STATUS_INVALID_ARGUMENT, "xt_surface_connection_set_text: port or text is NULL");
        }
        xt::flow::PortValue v;
        v.type = xt::flow::PortType::String;
        v.s = text;
        xt::capi::DispatchScope scope(*s);
        return xt::capi::connectionResult(*s, s->ports->setInput(port, v, param != nullptr ? param : ""),
                                          "xt_surface_connection_set_text");
    });
}

XT_CAPI xt_status xt_surface_connection_pulse(xt_surface* s, const char* port, const char* param) {
    XT_CAPI_GUARD("xt_surface_connection_pulse", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_connection_pulse");
        if (port == nullptr) {
            return fail(XT_STATUS_INVALID_ARGUMENT, "xt_surface_connection_pulse: port is NULL");
        }
        xt::flow::PortValue v;
        v.type = xt::flow::PortType::Pulse;
        xt::capi::DispatchScope scope(*s);
        return xt::capi::connectionResult(*s, s->ports->setInput(port, v, param != nullptr ? param : ""),
                                          "xt_surface_connection_pulse");
    });
}

XT_CAPI xt_status xt_surface_connection_watch(xt_surface* s, const char* port, uint32_t* out_listener_id) {
    XT_CAPI_GUARD("xt_surface_connection_watch", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_connection_watch");
        if (port == nullptr || out_listener_id == nullptr) {
            return fail(XT_STATUS_INVALID_ARGUMENT,
                        "xt_surface_connection_watch: port or out_listener_id is NULL");
        }
        // The id is only known once addOutputListener() returns; the listener reads it from here.
        auto idBox = std::make_shared<uint32_t>(0u);
        const std::string name = port;
        xt_surface* raw = s;
        xt::Result<uint32_t> added =
            s->ports->addOutputListener(port, [raw, idBox, name](const xt::flow::PortValue& v) {
                if (raw->portFn == nullptr) {
                    return;
                }
                xt::capi::callbackGuard("the port callback (connection)", [&] {
                    xt_port_event e{};
                    e.kind = XT_PORT_CONNECTION;
                    e.port = 0;
                    e.listener_id = *idBox;
                    e.name = name.c_str();
                    e.value_type = static_cast<int>(v.type);
                    e.instance_id = raw->ctx->instanceId(raw->handle);
                    e.item_index = 0xFFFFFFFFu;
                    std::string text;
                    xt::capi::fillValue(v, e, text);
                    xt::capi::dispatch(*raw, e);
                });
            });
        if (!added.ok()) {
            return fail(toCapi(added.status()), "xt_surface_connection_watch: " + s->ports->lastError());
        }
        *idBox = added.value();
        s->watches.emplace_back(added.value(), name);
        *out_listener_id = added.value();
        return XT_STATUS_OK;
    });
}

XT_CAPI void xt_surface_connection_unwatch(xt_surface* s, uint32_t listener_id) {
    XT_CAPI_GUARD_VOID("xt_surface_connection_unwatch", {
        if (s == nullptr) {
            return;
        }
        s->ports->removeOutputListener(listener_id);
        s->watches.erase(std::remove_if(s->watches.begin(), s->watches.end(),
                                        [listener_id](const auto& w) { return w.first == listener_id; }),
                         s->watches.end());
    });
}

XT_CAPI xt_status xt_surface_set_port_listener(xt_surface* s, xt_port_event_fn fn, void* user) {
    XT_CAPI_GUARD("xt_surface_set_port_listener", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_set_port_listener");
        s->portFn = fn;
        s->portUser = fn != nullptr ? user : nullptr;
        return XT_STATUS_OK;
    });
}

XT_CAPI xt_status xt_surface_set_host_event_listener(xt_surface* s, xt_host_event_fn fn, void* user) {
    XT_CAPI_GUARD("xt_surface_set_host_event_listener", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_set_host_event_listener");
        s->hostFn = fn;
        s->hostUser = fn != nullptr ? user : nullptr;
        return XT_STATUS_OK;
    });
}

XT_CAPI xt_status xt_surface_set_timeline_listener(xt_surface* s, xt_timeline_event_fn fn, void* user) {
    XT_CAPI_GUARD("xt_surface_set_timeline_listener", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_set_timeline_listener");
        s->timelineFn = fn;
        s->timelineUser = fn != nullptr ? user : nullptr;
        return XT_STATUS_OK;
    });
}

XT_CAPI xt_status xt_surface_set_visibility_listener(xt_surface* s, xt_visibility_fn fn, void* user) {
    XT_CAPI_GUARD("xt_surface_set_visibility_listener", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_set_visibility_listener");
        s->visibilityFn = fn;
        s->visibilityUser = fn != nullptr ? user : nullptr;
        return XT_STATUS_OK;
    });
}

XT_CAPI xt_status xt_surface_set_update_hook(xt_surface* s, xt_update_hook_fn fn, void* user) {
    XT_CAPI_GUARD("xt_surface_set_update_hook", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_set_update_hook");
        s->updateHookFn = fn;
        s->updateHookUser = fn != nullptr ? user : nullptr;
        return XT_STATUS_OK;
    });
}

}  // extern "C"
