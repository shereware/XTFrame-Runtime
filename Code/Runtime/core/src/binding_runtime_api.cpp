#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>

#include "binding_runtime.h"
#include "binding_runtime_internal.h"
#include "dispatch_guard.h"
#include "mini_css.h"
#include "mini_json.h"
#include "repeat_controller.h"
#include "rig_eval.h"
#include "style_decode.h"
#include "trigger_runtime.h"
#include "xt/animator.h"
#include "xt/num_guard.h"
#include "xt/property.h"
#include "xt/version.h"

/// BindingRuntime (split from binding_runtime.cpp by T-412): the public port API, converters, listeners and
/// trigger actions.

namespace xt {

using namespace binding_runtime_detail;  // T-412: helpers shared between the split files

// -- Public port API ------------------------------------------------------------------------------

PortId BindingRuntime::lookup(std::string_view name) const {
    if (name.find('#') != std::string_view::npos && name.front() != '#') {
        return kInvalidPort;  // `ui#id.port`: another UI (bundle-level, not this task).
    }
    if (!name.empty() && name.front() == '#') {
        name.remove_prefix(1);
    }
    const bool qualified = name.find('.') != std::string_view::npos;
    // T-380: a repeat instance's port copy answers to its copies' current path ids while the
    // instance is not pooled; template-only and released slots answer to nothing.
    auto instanceVisible = [&](const Slot& s) {
        return s.element != kInvalidNode && m_doc.m_arena.alive(s.element) &&
               (m_doc.m_arena.common(s.element).flags2 & kPooled) == 0;
    };
    for (size_t i = 0; i < m_slots.size(); ++i) {
        const Slot& s = m_slots[i];
        if (s.templateOnly || s.free) {
            continue;
        }
        if (s.nested) {  // T-391: `<enclosing repeat id>/<i>.<field>`.
            if (qualified && nestedName(s) == name) {
                return static_cast<PortId>(i + 1);
            }
            continue;
        }
        if (s.instance) {
            if (qualified && instanceVisible(s) && pathNameIs(s.element, s.name, name)) {
                return static_cast<PortId>(i + 1);
            }
            continue;
        }
        if (qualified
                ? (s.qualifiedName == name || (!s.elementId.empty() && s.elementId + "." + s.name == name))
                : (s.ir.scope == kNoPortScope && s.name == name)) {
            return static_cast<PortId>(i + 1);
        }
    }
    if (qualified) {
        for (size_t i = 0; i < m_slots.size(); ++i) {
            const Slot& s = m_slots[i];
            if (s.templateOnly || s.free || s.nested) {
                continue;
            }
            if (s.instance) {
                if ((s.ir.flags & kPortExpose) != 0 && instanceVisible(s) &&
                    pathNameIs(s.scopeNode, s.name, name)) {
                    return static_cast<PortId>(i + 1);
                }
                continue;
            }
            if (!s.exposedName.empty() && s.exposedName == name) {
                return static_cast<PortId>(i + 1);
            }
        }
    }
    return kInvalidPort;
}

PortRecord BindingRuntime::infoOf(uint32_t index) const {
    const Slot& s = m_slots[index];
    PortRecord info;
    info.id = index + 1;
    info.name = s.name;
    info.qualifiedName = s.qualifiedName;
    info.exposedName = s.exposedName;
    info.type = static_cast<PortValueType>(s.ir.type);
    info.dir = static_cast<PortDir>(s.ir.dir);
    info.exposed = (s.ir.flags & kPortExpose) != 0;
    info.documentLevel = s.ir.scope == kNoPortScope;
    info.elementId = s.elementId;
    if (s.instance) {  // T-380: the copies' current path ids.
        info.elementId = std::string(nodeIdText(s.element));
        info.qualifiedName = info.elementId.empty() ? s.name : info.elementId + "." + s.name;
        const std::string_view root = nodeIdText(s.scopeNode);
        info.exposedName = info.exposed && !root.empty() ? std::string(root) + "." + s.name : std::string();
    }
    if (s.nested) {  // T-391: `<enclosing repeat id>/<i>.<field>`.
        info.qualifiedName = nestedName(s);
        info.elementId = info.qualifiedName.substr(0, info.qualifiedName.rfind('.'));
        info.documentLevel = false;
        info.exposed = false;
        info.exposedName.clear();
    }
    for (size_t f = 0; f < s.fieldNames.size(); ++f) {
        info.fields.emplace_back(std::string(m_doc.m_ir.string(s.fieldNames[f])), s.fieldTypes[f]);
    }
    info.enumValues = s.enumValues;
    info.entryType = s.entryType;  // T-381
    // T-391: the entry types of the list<T> fields.
    struct Describe {
        const BindingRuntime& rt;
        void operator()(const ListSchema& sc, const std::string& name, PortRecord& out) const {
            out.name = name;
            out.type = PortValueType::List;
            out.entryType = sc.entryType;
            out.enumValues = sc.enumValues;
            for (size_t f = 0; f < sc.fieldNames.size() && f < sc.fieldTypes.size(); ++f) {
                const std::string field(rt.m_doc.string(sc.fieldNames[f]));
                out.fields.emplace_back(field, sc.fieldTypes[f]);
                if (sc.fieldTypes[f] == PortValueType::List && f < sc.subs.size() && sc.subs[f] != nullptr) {
                    out.fieldLists.emplace_back();
                    (*this)(*sc.subs[f], field, out.fieldLists.back());
                }
            }
        }
    };
    for (size_t f = 0; f < s.fieldNames.size() && f < s.fieldSubs.size(); ++f) {
        if (s.fieldTypes[f] == PortValueType::List && s.fieldSubs[f] != nullptr) {
            info.fieldLists.emplace_back();
            Describe{*this}(*s.fieldSubs[f], std::string(m_doc.string(s.fieldNames[f])),
                            info.fieldLists.back());
        }
    }
    return info;
}

std::string BindingRuntime::nestedName(const Slot& s) const {
    const std::string_view rid = nodeIdText(s.nestedRepeat);
    if (!s.nested || rid.empty() || s.nestedItem == kNoItem ||
        (m_doc.m_arena.common(s.nestedRepeat).flags2 & kPooled) != 0) {
        return std::string();
    }
    return std::string(rid) + "/" + std::to_string(s.nestedItem) + "." + s.name;
}

std::vector<PortRecord> BindingRuntime::ports() const {
    std::vector<PortRecord> out;
    out.reserve(m_slots.size());
    for (uint32_t i = 0; i < m_slots.size(); ++i) {
        if (m_slots[i].templateOnly || m_slots[i].instance || m_slots[i].free || m_slots[i].nested) {
            continue;  // T-380 / T-391: repeat instance copies come and go; lookup() finds them by path id.
        }
        out.push_back(infoOf(i));
    }
    return out;
}

PortRecord BindingRuntime::info(PortId port) const {
    return slot(port) != nullptr ? infoOf(port - 1) : PortRecord{};
}

Status BindingRuntime::set(PortId port, const Value& value) {
    if (slot(port) == nullptr) {
        return Status::kNotFound;
    }
    const DocumentWriteScope hostWrite(m_documentWrite, false);  // T-394: the host writes.
    bool changed = false;
    if (static_cast<PortValueType>(m_slots[port - 1].ir.type) == PortValueType::List) {
        return assign(port - 1, value, changed);  // listSet() notifies.
    }
    Status status = assign(port - 1, value, changed);
    if (status == Status::kOk && changed) {
        notify(port - 1, PortEventKind::Changed, 0);
    }
    return status;
}

Status BindingRuntime::setText(PortId port, std::string_view text) {
    const DocumentWriteScope hostWrite(m_documentWrite, false);  // T-394: the host writes.
    const Slot* s = slot(port);
    if (s == nullptr) {
        return Status::kNotFound;
    }
    double n = 0;
    switch (static_cast<PortValueType>(s->ir.type)) {
        case PortValueType::Number:
            return parseDouble(text, n) ? set(port, Value::ofFloat(static_cast<float>(n)))
                                        : Status::kInvalidArgument;
        case PortValueType::Entity:
            return parseDouble(text, n) && isEnumIndex(n) ? set(port, Value::ofEnum(toUint32Sat(n)))
                                                          : Status::kInvalidArgument;
        case PortValueType::Bool:
            if (text == "true" || text == "1" || text == "on") {
                return set(port, Value::ofFloat(1));
            }
            if (text == "false" || text == "0" || text == "off") {
                return set(port, Value::ofFloat(0));
            }
            return Status::kInvalidArgument;
        case PortValueType::String:
        case PortValueType::Texture: {
            Slot& w = m_slots[port - 1];
            if (w.text == text) {
                return Status::kOk;
            }
            w.text.assign(text);
            notify(port - 1, PortEventKind::Changed, 0);
            return Status::kOk;
        }
        case PortValueType::Enum:
            for (size_t i = 0; i < s->enumValues.size(); ++i) {
                if (s->enumValues[i] == text) {
                    return set(port, Value::ofEnum(static_cast<uint32_t>(i)));
                }
            }
            if (parseDouble(text, n) && isEnumIndex(n)) {
                return set(port, Value::ofEnum(toUint32Sat(n)));
            }
            return Status::kInvalidArgument;
        case PortValueType::Color: {
            Color c{};
            return parseColorText(text, c) ? set(port, Value::ofColor(c)) : Status::kInvalidArgument;
        }
        case PortValueType::Vec2:
        case PortValueType::Vec3: {
            // `x, y[, z]` or, T-200, the JSON-array form `[x, y]` (`xtframe-view --port pos=[1,2]`).
            std::string t(text);
            std::replace(t.begin(), t.end(), ',', ' ');
            if (const size_t open = t.find_first_not_of(' '); open != std::string::npos && t[open] == '[') {
                const size_t close = t.find_last_not_of(' ');
                if (t[close] != ']') {
                    return Status::kInvalidArgument;
                }
                t[open] = ' ';
                t[close] = ' ';
            }
            float c[3] = {0, 0, 0};
            const bool three = static_cast<PortValueType>(s->ir.type) == PortValueType::Vec3;
            const char* p = t.c_str();
            int got = 0;
            for (; got < 3; ++got) {
                char* end = nullptr;
                float v = std::strtof(p, &end);
                if (end == p) {
                    break;
                }
                c[got] = v;
                p = end;
            }
            while (*p == ' ') {
                ++p;
            }
            if (got != (three ? 3 : 2) || *p != '\0') {
                return Status::kInvalidArgument;
            }
            const float x = c[0], y = c[1], z = c[2];
            return set(port, three ? Value::ofVec3(Vec3{x, y, z}) : Value::ofVec2(Vec2{x, y}));
        }
        case PortValueType::Event:
            return set(port, Value::ofFloat(0));
        case PortValueType::Struct:
        case PortValueType::List: {
            // T-177: a JSON object (struct) or an array of objects (list); fields by name.
            Json json;
            if (!JsonReader(text).parse(json)) {
                return Status::kInvalidArgument;
            }
            const bool list = static_cast<PortValueType>(s->ir.type) == PortValueType::List;
            if ((list && json.kind != Json::Kind::Arr) || (!list && json.kind != Json::Kind::Obj)) {
                return Status::kInvalidArgument;
            }
            // One JSON value of type `t` (a struct field or, T-381, a list<T> entry) -> `out`;
            // `enums` = an Enum entry's value names.
            auto jsonValue = [&](const Json& v, PortValueType t, Value& out,
                                 const std::vector<std::string>& enums) {
                if (t == PortValueType::Enum) {  // T-381: a value name or an ordinal.
                    if (v.kind == Json::Kind::Str) {
                        const auto it = std::find(enums.begin(), enums.end(), v.s);
                        if (it == enums.end()) {
                            return false;
                        }
                        out = Value::ofEnum(static_cast<uint32_t>(it - enums.begin()));
                        return true;
                    }
                    if (v.kind != Json::Kind::Num || !isEnumIndex(v.n)) {
                        return false;
                    }
                    out = Value::ofEnum(toUint32Sat(v.n));
                    return true;
                }
                if (t == PortValueType::String || t == PortValueType::Texture) {
                    std::string str = v.kind == Json::Kind::Str    ? v.s
                                      : v.kind == Json::Kind::Num  ? numText(v.n)
                                      : v.kind == Json::Kind::Bool ? std::string(v.b ? "true" : "false")
                                                                   : std::string();
                    // T-270c: a struct field Value needs a StringId; counted per port.
                    out = Value::ofStringId(internCounted(str, m_slots[port - 1].strings, s->qualifiedName));
                } else if (t == PortValueType::Color) {
                    Color c{};
                    if (v.kind != Json::Kind::Str || !parseColorText(v.s, c)) {
                        return false;
                    }
                    out = Value::ofColor(c);
                } else if (t == PortValueType::Vec2 || t == PortValueType::Vec3) {
                    const size_t want = t == PortValueType::Vec2 ? 2 : 3;
                    if (v.kind != Json::Kind::Arr || v.items.size() != want) {
                        return false;
                    }
                    float c[3] = {0, 0, 0};
                    for (size_t k = 0; k < want; ++k) {
                        if (v.items[k].kind != Json::Kind::Num) {
                            return false;
                        }
                        c[k] = static_cast<float>(v.items[k].n);
                    }
                    out = want == 2 ? Value::ofVec2(Vec2{c[0], c[1]}) : Value::ofVec3(Vec3{c[0], c[1], c[2]});
                } else {
                    double d = 0;
                    if (v.kind == Json::Kind::Num) {
                        d = v.n;
                    } else if (v.kind == Json::Kind::Bool) {
                        d = v.b ? 1 : 0;
                    } else if (v.kind != Json::Kind::Str || !parseDouble(v.s, d)) {
                        return false;
                    }
                    out = Value::ofFloat(static_cast<float>(d));
                }
                return true;
            };
            // T-391: a nested JSON array of a list<T> field (schema `sc`) -> `out`, over `tree`
            // (every field present, in the schema's order; convertEntry() copies it afterwards).
            ValueTree jsonTree;
            std::function<bool(const Json&, const ListSchema&, Value&)> jsonList =
                [&](const Json& v, const ListSchema& sc, Value& out) {
                    if (v.kind != Json::Kind::Arr) {
                        return false;
                    }
                    std::vector<Value> entries;
                    for (const Json& item : v.items) {
                        if (sc.entryType != static_cast<PortValueType>(0)) {
                            Value e;
                            if (!jsonValue(item, sc.entryType, e, sc.enumValues)) {
                                return false;
                            }
                            entries.push_back(e);
                            continue;
                        }
                        if (item.kind != Json::Kind::Obj) {
                            return false;
                        }
                        std::vector<Value> fields(sc.fieldTypes.size(), Value::ofFloat(0));
                        for (size_t f = 0; f < sc.fieldTypes.size(); ++f) {
                            fields[f] =
                                sc.fieldTypes[f] == PortValueType::List    ? Value::ofList(nullptr, 0)
                                : sc.fieldTypes[f] == PortValueType::Color ? Value::ofColor(Color{0, 0, 0, 0})
                                : sc.fieldTypes[f] == PortValueType::String ||
                                        sc.fieldTypes[f] == PortValueType::Texture
                                    ? Value::ofStringId(kInvalidString)
                                : sc.fieldTypes[f] == PortValueType::Vec2 ? Value::ofVec2(Vec2{0, 0})
                                : sc.fieldTypes[f] == PortValueType::Vec3 ? Value::ofVec3(Vec3{0, 0, 0})
                                                                          : Value::ofFloat(0);
                        }
                        for (const auto& [key, fv] : item.fields) {
                            size_t f = 0;
                            while (f < sc.fieldNames.size() && m_doc.string(sc.fieldNames[f]) != key) {
                                ++f;
                            }
                            if (f == sc.fieldNames.size()) {
                                continue;  // Unknown fields are ignored.
                            }
                            // T-405 (02 §2.s.3): a List field without an IrPortNested entry has no
                            // nesting; its value is ignored like an unknown field's (empty list).
                            const bool noNesting = sc.fieldTypes[f] == PortValueType::List &&
                                                   (f >= sc.subs.size() || sc.subs[f] == nullptr);
                            if (noNesting) {
                                continue;
                            }
                            const bool ok = sc.fieldTypes[f] == PortValueType::List
                                                ? jsonList(fv, *sc.subs[f], fields[f])
                                                : jsonValue(fv, sc.fieldTypes[f], fields[f], {});
                            if (!ok) {
                                return false;
                            }
                        }
                        jsonTree.arrays.push_back(std::move(fields));
                        const std::vector<Value>& stored = jsonTree.arrays.back();
                        entries.push_back(Value::ofStruct(sc.fieldNames.data(), stored.data(),
                                                          static_cast<uint32_t>(stored.size())));
                    }
                    jsonTree.arrays.push_back(std::move(entries));
                    const std::vector<Value>& stored = jsonTree.arrays.back();
                    out = Value::ofList(stored.data(), static_cast<uint32_t>(stored.size()));
                    return true;
                };
            if (list && s->entryType != static_cast<PortValueType>(0)) {
                // T-381: a list<T> port takes a JSON array of T values (`["a","b"]`, `[1,2]`).
                std::vector<Value> entries;
                entries.reserve(json.items.size());
                for (const Json& item : json.items) {
                    Value out;
                    if (!jsonValue(item, s->entryType, out, s->enumValues)) {
                        return Status::kInvalidArgument;
                    }
                    entries.push_back(out);
                }
                return listSet(port, std::span<const Value>(entries.data(), entries.size()));
            }
            std::vector<std::vector<StringId>> names;
            std::vector<std::vector<Value>> values;
            const std::vector<Json> single = list ? std::vector<Json>{} : std::vector<Json>{json};
            const std::vector<Json>& objects = list ? json.items : single;
            for (const Json& obj : objects) {
                if (obj.kind != Json::Kind::Obj) {
                    return Status::kInvalidArgument;
                }
                names.emplace_back();
                values.emplace_back();
                for (const auto& [key, v] : obj.fields) {
                    size_t f = 0;
                    while (f < s->fieldNames.size() && m_doc.string(s->fieldNames[f]) != key) {
                        ++f;
                    }
                    if (f == s->fieldNames.size()) {
                        continue;  // Unknown fields are ignored (as convertEntry does).
                    }
                    // T-405 (02 §2.s.3): a List field without an IrPortNested entry has no nesting;
                    // its value is ignored like an unknown field's.
                    if (s->fieldTypes[f] == PortValueType::List &&
                        (f >= s->fieldSubs.size() || s->fieldSubs[f] == nullptr)) {
                        continue;
                    }
                    Value out;
                    const bool ok = s->fieldTypes[f] == PortValueType::List  // T-391
                                        ? jsonList(v, *s->fieldSubs[f], out)
                                        : jsonValue(v, s->fieldTypes[f], out, s->enumValues);
                    if (!ok) {
                        return Status::kInvalidArgument;
                    }
                    names.back().push_back(s->fieldNames[f]);
                    values.back().push_back(out);
                }
            }
            std::vector<Value> entries;
            for (size_t i = 0; i < values.size(); ++i) {
                entries.push_back(Value::ofStruct(names[i].data(), values[i].data(),
                                                  static_cast<uint32_t>(values[i].size())));
            }
            if (!list) {
                return set(port, entries[0]);
            }
            return listSet(port, std::span<const Value>(entries.data(), entries.size()));
        }
    }
    return Status::kInvalidArgument;
}

Value BindingRuntime::get(PortId port) { return slot(port) != nullptr ? publicValue(port - 1) : Value{}; }

std::string BindingRuntime::text(PortId port) const {
    const Slot* s = slot(port);
    if (s == nullptr) {
        return std::string();
    }
    switch (static_cast<PortValueType>(s->ir.type)) {
        case PortValueType::Struct:
        case PortValueType::List:
        case PortValueType::Event:
            return std::string();
        default:
            return toStr(portValue(port - 1));
    }
}

Result<Value> BindingRuntime::call(PortId port, std::span<const Value> args) {
    Slot* s = slot(port);
    if (s == nullptr) {
        return Result<Value>::error(Status::kNotFound);
    }
    if (static_cast<PortDir>(s->ir.dir) != PortDir::Call) {
        return Result<Value>::error(Status::kInvalidArgument);
    }
    if (!args.empty() && static_cast<PortValueType>(s->ir.type) != PortValueType::Event) {
        bool changed = false;
        if (assign(port - 1, args[0], changed) != Status::kOk) {
            return Result<Value>::error(Status::kInvalidArgument);
        }
        if (changed) {
            // Bindings and listeners follow the new value; the triggers run once, below.
            std::vector<uint32_t> bindings = s->bindings;
            for (uint32_t b : bindings) {
                evaluate(b, /*initial=*/false);
            }
            notifyListeners(PortEventKind::Changed, port - 1, 0);
        }
    }
    const bool outer = m_capturing;
    const bool outerCaptured = m_captured;
    const Value outerValue = m_capture;
    m_capturing = true;
    m_captured = false;
    m_capture = Value::ofFloat(0);
    fireTriggers(port - 1);
    Value result = m_captured ? m_capture : Value::ofFloat(0);
    m_capturing = outer;
    m_captured = outerCaptured;
    m_capture = outerValue;
    return Result<Value>::ok(result);
}

Status BindingRuntime::setCallHandler(PortId port, PortCallHandler handler) {
    Slot* s = slot(port);
    if (s == nullptr) {
        return Status::kNotFound;
    }
    if (static_cast<PortDir>(s->ir.dir) != PortDir::Call) {
        return Status::kInvalidArgument;
    }
    s->callHandler = std::move(handler);
    return Status::kOk;
}

Status BindingRuntime::listSet(PortId port, std::span<const Value> entries) {
    Slot* s = slot(port);
    if (s == nullptr) {
        return Status::kNotFound;
    }
    if (static_cast<PortValueType>(s->ir.type) != PortValueType::List) {
        return Status::kInvalidArgument;
    }
    std::vector<std::vector<Value>> next;
    std::vector<std::vector<TreePtr>> nextTrees;  // T-391
    next.reserve(entries.size());
    nextTrees.reserve(entries.size());
    for (const Value& e : entries) {
        std::vector<Value> fields = blankEntry(*s);
        std::vector<TreePtr> trees;
        if (!convertEntry(*s, e, fields, &trees)) {
            return Status::kInvalidArgument;
        }
        next.push_back(std::move(fields));
        nextTrees.push_back(std::move(trees));
    }
    s->entries = std::move(next);
    s->entryTrees = std::move(nextTrees);
    rebuildEntries(*s);
    if (m_quietList) {
        return Status::kOk;  // T-383: a declared default applied at load.
    }
    const bool nested = s->nested;
    m_doc.m_repeats->onListChanged(port - 1, RepeatListOp::Set, 0);  // T-177.
    notify(port - 1, PortEventKind::ListChanged, 0);
    if (nested) {
        writeBackNested(port - 1);  // T-391
    }
    return Status::kOk;
}

Status BindingRuntime::listInsert(PortId port, uint32_t index, const Value& entry) {
    Slot* s = slot(port);
    if (s == nullptr) {
        return Status::kNotFound;
    }
    if (static_cast<PortValueType>(s->ir.type) != PortValueType::List || index > s->entries.size()) {
        return Status::kInvalidArgument;
    }
    std::vector<Value> fields = blankEntry(*s);
    std::vector<TreePtr> trees;  // T-391
    if (!convertEntry(*s, entry, fields, &trees)) {
        return Status::kInvalidArgument;
    }
    fitTrees(*s);
    s->entries.insert(s->entries.begin() + index, std::move(fields));
    s->entryTrees.insert(s->entryTrees.begin() + index, std::move(trees));
    rebuildEntries(*s);
    const bool nested = s->nested;
    m_doc.m_repeats->onListChanged(port - 1, RepeatListOp::Insert, index);  // T-177.
    notify(port - 1, PortEventKind::ListChanged, index);
    if (nested) {
        writeBackNested(port - 1);  // T-391
    }
    return Status::kOk;
}

Status BindingRuntime::listRemove(PortId port, uint32_t index) {
    Slot* s = slot(port);
    if (s == nullptr) {
        return Status::kNotFound;
    }
    if (static_cast<PortValueType>(s->ir.type) != PortValueType::List || index >= s->entries.size()) {
        return Status::kInvalidArgument;
    }
    fitTrees(*s);
    s->entries.erase(s->entries.begin() + index);
    s->entryTrees.erase(s->entryTrees.begin() + index);
    rebuildEntries(*s);
    const bool nested = s->nested;
    m_doc.m_repeats->onListChanged(port - 1, RepeatListOp::Remove, index);  // T-177.
    notify(port - 1, PortEventKind::ListChanged, index);
    if (nested) {
        writeBackNested(port - 1);  // T-391
    }
    return Status::kOk;
}

Status BindingRuntime::listUpdate(PortId port, uint32_t index, const Value& entry) {
    Slot* s = slot(port);
    if (s == nullptr) {
        return Status::kNotFound;
    }
    if (static_cast<PortValueType>(s->ir.type) != PortValueType::List || index >= s->entries.size()) {
        return Status::kInvalidArgument;
    }
    fitTrees(*s);
    std::vector<Value> fields = s->entries[index];
    std::vector<TreePtr> trees = s->entryTrees[index];  // T-391: a field not given keeps its list.
    if (!convertEntry(*s, entry, fields, &trees)) {
        return Status::kInvalidArgument;
    }
    s->entries[index] = std::move(fields);
    s->entryTrees[index] = std::move(trees);
    rebuildEntries(*s);
    const bool nested = s->nested;
    m_doc.m_repeats->onListChanged(port - 1, RepeatListOp::Update, index);  // T-177.
    notify(port - 1, PortEventKind::ListChanged, index);
    if (nested) {
        writeBackNested(port - 1);  // T-391
    }
    return Status::kOk;
}

Result<uint32_t> BindingRuntime::registerConverter(std::string_view name, ConverterFn fn) {
    if (name.empty() || !fn) {
        return Result<uint32_t>::error(Status::kInvalidArgument);
    }
    uint32_t op = 0;
    bool found = false;
    for (size_t i = 0; i < m_converters.size(); ++i) {
        if (m_converters[i].first == name) {
            m_converters[i].second = std::move(fn);
            op = static_cast<uint32_t>(ConverterOp::Custom) + static_cast<uint32_t>(i);
            found = true;
            break;
        }
    }
    if (!found) {
        if (m_converters.size() >= 128) {
            return Result<uint32_t>::error(Status::kInvalidArgument);
        }
        m_converters.emplace_back(std::string(name), std::move(fn));
        op = static_cast<uint32_t>(ConverterOp::Custom) + static_cast<uint32_t>(m_converters.size() - 1);
    }
    for (uint32_t i = 0; i < m_bindings.size(); ++i) {
        if (m_bindings[i].usesCustom) {
            evaluate(i, /*initial=*/false);
        }
    }
    return Result<uint32_t>::ok(op);
}

uint32_t BindingRuntime::addListener(PortListener listener) {
    uint32_t id = m_nextListener++;
    m_listeners.emplace_back(id, std::move(listener));
    return id;
}

void BindingRuntime::removeListener(uint32_t id) {
    if (id == 0) {
        return;  // Never issued (0 marks a tombstone).
    }
    if (m_notifyDepth > 0) {
        // T-478: a notification is iterating the list; notifyListeners() erases it afterwards.
        for (auto& entry : m_listeners) {
            if (entry.first == id) {
                entry.first = 0;
                m_listenerTombstones = true;
            }
        }
        return;
    }
    m_listeners.erase(
        std::remove_if(m_listeners.begin(), m_listeners.end(),
                       [id](const std::pair<uint32_t, PortListener>& l) { return l.first == id; }),
        m_listeners.end());
}

void BindingRuntime::emit(NodeId node, const IrTriggerAction& action) {
    Document& doc = m_doc;
    uint32_t index = action.arg1 != 0 && action.arg1 <= m_slots.size() ? action.arg1 - 1 : kNoPort;
    if (index != kNoPort && m_slots[index].templateOnly) {
        index = doc.m_repeats->instancePortOf(node, index);  // T-380: the instance's copy.
        if (index >= m_slots.size() || m_slots[index].templateOnly || m_slots[index].free) {
            index = kNoPort;
        }
    }
    // T-177 (27.3.4): an emit from inside a `<repeat>` instance carries its entry index and entry.
    uint32_t itemPort = kNoPort;
    // T-391: inside nested repeats, the outermost entry (the inner indices are in elementId's path).
    const uint32_t itemIndex = doc.m_repeats->itemIndexOf(node, &itemPort, /*outermost=*/true);
    Value item = itemIndex != kNoItem ? entryValue(itemPort, itemIndex) : Value{};
    // T-260a: an unloading Document calls no host (call handler, listeners, host event).
    if (doc.m_unloading) {
        return;
    }
    if (index != kNoPort && static_cast<PortDir>(m_slots[index].ir.dir) == PortDir::Call &&
        m_slots[index].callHandler) {
        // UI -> host call: the handler's result becomes the port's value.
        Value arg = publicValue(index);
        PortCallHandler handler = m_slots[index].callHandler;  // A copy: the call may replace it.
        Value result = handler(std::span<const Value>(&arg, 1));
        bool changed = false;
        if (assign(index, result, changed) == Status::kOk && changed) {
            notify(index, PortEventKind::Changed, 0);
        }
        if (m_capturing) {
            m_capture = publicValue(index);
            m_captured = true;
        }
        return;
    }
    Value value = index != kNoPort ? publicValue(index) : Value::ofFloat(0);
    if (m_capturing) {
        m_capture = value;
        m_captured = true;
    }
    if (index != kNoPort) {
        notifyListeners(PortEventKind::Emit, index, 0, itemIndex, itemIndex != kNoItem ? &item : nullptr);
    }
    if (!doc.m_hostEventListener) {
        return;
    }
    HostEvent event;
    event.kind = HostEventKind::Emit;
    event.name = doc.m_ir.string(action.arg0);
    event.element = node;
    StringId id = doc.m_arena.element(node).id;
    event.elementId = id != kInvalidString ? doc.string(id) : std::string_view{};
    event.value = value;
    event.port = index != kNoPort ? index + 1 : 0;
    event.instanceId = doc.m_instanceId;
    event.itemIndex = itemIndex;
    HostEventListener listener = doc.m_hostEventListener;  // The callback may replace it.
    listener(event);
}

void BindingRuntime::setAction(NodeId node, const IrTriggerAction& action, float value, uint32_t port) {
    if (action.arg1 == 0 || action.arg1 > m_slots.size()) {
        return;
    }
    const DocumentWriteScope documentWrite(m_documentWrite, true);  // T-394: a trigger `set()`.
    // T-380: inside a repeat instance a template port means the instance's copy.
    const uint32_t index = m_slots[action.arg1 - 1].templateOnly
                               ? m_doc.m_repeats->instancePortOf(node, action.arg1 - 1)
                               : action.arg1 - 1;
    if (index >= m_slots.size() || m_slots[index].templateOnly || m_slots[index].free) {
        return;
    }
    BVal v;
    // T-411 (27 §27.2.3): the converter steps of a constant / Port / Item value.
    auto convert = [&](BVal in, EvalCtx& ctx) {
        ctx.node = node;
        return action.extra != 0 ? run(m_doc.m_ir.triggers().conditions().subspan(action.arg0, action.extra),
                                       std::move(in), ctx)
                                 : in;
    };
    switch (static_cast<SetSource>(action.flags)) {
        case SetSource::Number: {
            EvalCtx ctx;
            v = convert(num(action.arg2), ctx);
            break;
        }
        case SetSource::String: {
            uint32_t bits = 0;
            std::memcpy(&bits, &action.arg2, sizeof(bits));
            v.kind = Kind::Str;
            v.str = std::string(m_doc.m_ir.string(bits));
            EvalCtx ctx;
            v = convert(std::move(v), ctx);
            break;
        }
        case SetSource::Port: {
            // T-411: the port's value now (after any `then` wait), its instance copy in a repeat.
            uint32_t bits = 0;
            std::memcpy(&bits, &action.arg2, sizeof(bits));
            const uint32_t src = bits < m_slots.size() && m_slots[bits].templateOnly
                                     ? m_doc.m_repeats->instancePortOf(node, bits)
                                     : bits;
            if (src >= m_slots.size() || m_slots[src].templateOnly || m_slots[src].free) {
                return;
            }
            EvalCtx ctx;
            ctx.maxPort = m_slots[src].maxPort;
            v = convert(portValue(src), ctx);
            break;
        }
        case SetSource::Item: {
            // T-411: the entry of the live `<repeat>` instance holding `node`, now.
            Binding entry;
            entry.instance = true;
            entry.item = m_doc.m_repeats->itemIndexOf(node, &entry.listPort);
            if (entry.item == kNoItem || entry.listPort >= m_slots.size() ||
                entry.item >= m_slots[entry.listPort].entryValues.size()) {
                return;
            }
            EvalCtx ctx;
            ctx.binding = &entry;
            v = convert(entryToB(m_slots[entry.listPort], m_slots[entry.listPort].entryValues[entry.item]),
                        ctx);
            break;
        }
        case SetSource::Toggle:
            v = num(truthy(portValue(index)) ? 0.0 : 1.0, true);
            break;
        case SetSource::EventValue: {
            EvalCtx ctx;
            v = num(value);
            if (port < m_slots.size()) {
                v = portValue(port);
                ctx.maxPort = m_slots[port].maxPort;
            }
            if (action.extra != 0) {
                v = run(m_doc.m_ir.triggers().conditions().subspan(action.arg0, action.extra), v, ctx);
            }
            break;
        }
        case SetSource::ElementValue: {
            // T-392 (27 §27.3.5): `value`, the trigger element's current text (an input's value).
            v.kind = Kind::Str;
            v.str = std::string(ElementRef{&m_doc, node, m_doc.m_arena.generation(node)}.text());
            if (action.extra != 0) {
                EvalCtx ctx;
                v = run(m_doc.m_ir.triggers().conditions().subspan(action.arg0, action.extra), v, ctx);
            }
            break;
        }
        default:
            return;  // The pointer sources arrive as EventValue (TriggerRuntime).
    }
    const Slot& s = m_slots[index];
    const PortValueType type = static_cast<PortValueType>(s.ir.type);
    bool changed = false;
    if (type == PortValueType::String || type == PortValueType::Texture || type == PortValueType::Enum) {
        // T-270c: the text goes straight into the port (what set()/assign() of an interned
        // StringRef did), so a new string per event interns nothing.
        const Status status = assignText(index, toStr(v), changed);
        if (static_cast<PortDir>(s.ir.dir) == PortDir::In) {
            if (status == Status::kOk && changed) {
                notify(index, PortEventKind::Changed, 0);
            }
            return;
        }
        if (status != Status::kOk) {
            return;
        }
    } else {
        Value out;
        if (type == PortValueType::Number || type == PortValueType::Bool || type == PortValueType::Entity) {
            out = Value::ofFloat(static_cast<float>(toNum(v)));
        } else {
            out = bToValue(v, m_slots[index].strings, s.qualifiedName);
        }
        if (static_cast<PortDir>(s.ir.dir) == PortDir::In) {
            // set()'s write, without its host scope (T-394: this is a document write).
            const bool list = type == PortValueType::List;
            if (assign(index, out, changed) == Status::kOk && changed && !list) {
                notify(index, PortEventKind::Changed, 0);
            }
            return;
        }
        if (type != PortValueType::Event && assign(index, out, changed) != Status::kOk) {
            return;
        }
    }
    IrTriggerAction emitAction = action;
    emitAction.kind = static_cast<uint8_t>(TriggerAction::Emit);
    emitAction.arg0 = s.ir.name;
    emit(node, emitAction);
}

}  // namespace xt
