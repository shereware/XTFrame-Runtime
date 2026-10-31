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

/// BindingRuntime (split from binding_runtime.cpp by T-412): port slots and values, `<repeat>` instances and
/// nested lists.

namespace xt {

using namespace binding_runtime_detail;  // T-412: helpers shared between the split files

// -- Port values --------------------------------------------------------------------------------

BindingRuntime::Slot* BindingRuntime::slot(PortId port) {
    if (port == kInvalidPort || port > m_slots.size()) {
        return nullptr;
    }
    Slot& s = m_slots[port - 1];
    return s.templateOnly || s.free ? nullptr : &s;  // T-380
}

const BindingRuntime::Slot* BindingRuntime::slot(PortId port) const {
    if (port == kInvalidPort || port > m_slots.size()) {
        return nullptr;
    }
    const Slot& s = m_slots[port - 1];
    return s.templateOnly || s.free ? nullptr : &s;  // T-380
}

uint32_t BindingRuntime::mapPort(std::span<const std::pair<uint32_t, uint32_t>> portMap, uint32_t port) {
    for (const auto& [from, to] : portMap) {
        if (from == port) {
            return to;
        }
    }
    return port;
}

std::string_view BindingRuntime::nodeIdText(NodeId node) const {
    if (node == kInvalidNode || !m_doc.m_arena.alive(node)) {
        return {};
    }
    const StringId id = m_doc.m_arena.element(node).id;
    return id != kInvalidString ? m_doc.string(id) : std::string_view{};
}

bool BindingRuntime::pathNameIs(NodeId node, std::string_view port, std::string_view name) const {
    const std::string_view id = nodeIdText(node);
    return !id.empty() && name.size() == id.size() + 1 + port.size() && name.substr(0, id.size()) == id &&
           name[id.size()] == '.' && name.substr(id.size() + 1) == port;
}

void BindingRuntime::writePort(uint32_t index, const BVal& r, bool initial) {
    if (index >= m_slots.size() || m_slots[index].free || m_slots[index].templateOnly) {
        return;
    }
    const DocumentWriteScope documentWrite(m_documentWrite, true);  // T-394: a binding writes.
    const PortValueType type = static_cast<PortValueType>(m_slots[index].ir.type);
    bool changed = false;
    Status status = Status::kOk;
    if (type == PortValueType::String || type == PortValueType::Texture || type == PortValueType::Enum) {
        status = assignText(index, toStr(r), changed);  // T-270c: no interning (setAction()'s way).
    } else if (type == PortValueType::Number || type == PortValueType::Bool ||
               type == PortValueType::Entity) {
        status = assign(index, Value::ofFloat(static_cast<float>(toNum(r))), changed);
    } else {
        status = assign(index, bToValue(r, m_slots[index].strings, m_slots[index].qualifiedName), changed);
    }
    if (status != Status::kOk || !changed || type == PortValueType::List) {
        return;  // A list port notifies through the list API only (set()'s rule).
    }
    if (!initial) {
        notify(index, PortEventKind::Changed, 0);
        return;
    }
    std::vector<uint32_t> readers = m_slots[index].bindings;  // The evaluations may add bindings.
    for (uint32_t reader : readers) {
        evaluate(reader, /*initial=*/true);
    }
}

BVal BindingRuntime::portValue(uint32_t index) const {
    const Slot& s = m_slots[index];
    switch (static_cast<PortValueType>(s.ir.type)) {
        case PortValueType::Number:
        case PortValueType::Entity:
            return num(s.num);
        case PortValueType::Bool:
            return num(s.num, true);
        case PortValueType::String:
        case PortValueType::Texture:
            return str(s.text);
        case PortValueType::Enum: {
            size_t i = static_cast<size_t>(s.num);
            return str(i < s.enumValues.size() ? s.enumValues[i] : std::string());
        }
        case PortValueType::Color:
            return ofValue(Kind::Color, s.value);
        case PortValueType::Vec2:
            return ofValue(Kind::Vec2, s.value);
        case PortValueType::Vec3:
            return ofValue(Kind::Vec3, s.value);
        case PortValueType::Struct:
            return ofValue(Kind::Struct, Value::ofStruct(s.fieldNames.data(), s.fields.data(),
                                                         static_cast<uint32_t>(s.fields.size())));
        case PortValueType::List:
            return ofValue(Kind::List,
                           Value::ofList(s.entryValues.data(), static_cast<uint32_t>(s.entryValues.size())));
        case PortValueType::Event:
            return num(0);
    }
    return BVal{};
}

Value BindingRuntime::publicValue(uint32_t index) {
    const Slot& s = m_slots[index];
    switch (static_cast<PortValueType>(s.ir.type)) {
        case PortValueType::Number:
        case PortValueType::Bool:
            return Value::ofFloat(static_cast<float>(s.num));
        case PortValueType::Entity:
        case PortValueType::Enum:
            return Value::ofEnum(toUint32Sat(s.num));
        case PortValueType::String:
        case PortValueType::Texture:
            // T-270c: the host's Value needs a StringId; new ones count against the port.
            return Value::ofStringId(internCounted(s.text, m_slots[index].strings, s.qualifiedName));
        case PortValueType::Color:
        case PortValueType::Vec2:
        case PortValueType::Vec3:
            return s.value;
        case PortValueType::Struct:
            return Value::ofStruct(s.fieldNames.data(), s.fields.data(),
                                   static_cast<uint32_t>(s.fields.size()));
        case PortValueType::List:
            return Value::ofList(s.entryValues.data(), static_cast<uint32_t>(s.entryValues.size()));
        case PortValueType::Event:
            return Value::ofFloat(0);
    }
    return Value{};
}

void BindingRuntime::countString(std::string_view s, StringGuard& guard, std::string_view source) {
    if (m_doc.findString(s) != kInvalidString) {
        return;
    }
    // T-270d: counted past the warning too, so the perf data's maximum stays exact.
    m_maxDistinctStrings = std::max(m_maxDistinctStrings, ++guard.distinct);
    if (guard.distinct > kMaxDistinctStrings && !guard.warned) {
        guard.warned = true;
        m_doc.addWarning(
            "binding \"" + std::string(source) +
            "\" produces unbounded distinct strings; bind it to text or a String port instead of "
            "a class / converter argument");
    }
}

StringId BindingRuntime::internCounted(std::string_view s, StringGuard& guard, std::string_view source) {
    countString(s, guard, source);
    return m_doc.intern(s);
}

BVal BindingRuntime::valueToB(const Value& v) const {
    switch (v.type) {
        case ValueType::Float:
            return num(v.f);
        case ValueType::Enum:
            return num(v.e);
        case ValueType::StringRef:
            return str(std::string(m_doc.string(v.sid)));
        case ValueType::Color:
            return ofValue(Kind::Color, v);
        case ValueType::Length:
            return ofValue(Kind::Len, v);
        case ValueType::Vec2:
            return ofValue(Kind::Vec2, v);
        case ValueType::Vec3:
            return ofValue(Kind::Vec3, v);
        case ValueType::Struct:
            return ofValue(Kind::Struct, v);
        case ValueType::List:
            return ofValue(Kind::List, v);
        default:
            return BVal{};
    }
}

Value BindingRuntime::bToValue(const BVal& b, StringGuard& guard, std::string_view source) {
    switch (b.kind) {
        case Kind::Num:
            return Value::ofFloat(static_cast<float>(b.num));
        case Kind::Str:
            return Value::ofStringId(internCounted(b.str, guard, source));
        case Kind::None:
            return Value::ofFloat(0);
        default:
            return b.v;
    }
}

BVal BindingRuntime::entryToB(const Slot& s, const Value& v) const {
    BVal b = valueToB(v);
    if (s.entryType == PortValueType::Bool && b.kind == Kind::Num) {
        b.isBool = true;
    }
    return b;
}

BVal BindingRuntime::emptyEntryValue(const Slot& s, std::string_view field) const {
    auto empty = [&](PortValueType t) {
        switch (t) {
            case PortValueType::String:
            case PortValueType::Texture:
                return str(std::string());
            case PortValueType::Bool:
                return num(0, /*isBool=*/true);
            case PortValueType::Color:
                return ofValue(Kind::Color, Value::ofColor(Color{0, 0, 0, 0}));
            case PortValueType::Vec2:
                return ofValue(Kind::Vec2, Value::ofVec2(Vec2{0, 0}));
            case PortValueType::Vec3:
                return ofValue(Kind::Vec3, Value::ofVec3(Vec3{0, 0, 0}));
            case PortValueType::Enum:  // A list<enum> entry reads as its value name.
                return str(s.enumValues.empty() ? std::string() : s.enumValues[0]);
            case PortValueType::List:  // T-391: a list field.
                return ofValue(Kind::List, Value::ofList(nullptr, 0));
            default:
                return num(0);
        }
    };
    if (s.entryType != static_cast<PortValueType>(0)) {
        return empty(s.entryType);
    }
    for (size_t f = 0; f < s.fieldNames.size() && f < s.fieldTypes.size(); ++f) {
        if (m_doc.string(s.fieldNames[f]) == field) {
            return empty(s.fieldTypes[f]);
        }
    }
    return BVal{};
}

std::vector<Value> BindingRuntime::blankEntry(const Slot& s) {
    auto blank = [](PortValueType t) {
        return t == PortValueType::List    ? Value::ofList(nullptr, 0)  // T-391
               : t == PortValueType::Color ? Value::ofColor(Color{0, 0, 0, 0})
               : t == PortValueType::String || t == PortValueType::Texture ? Value::ofStringId(kInvalidString)
               : t == PortValueType::Entity                                ? Value::ofEnum(0)
               : t == PortValueType::Vec2                                  ? Value::ofVec2(Vec2{0, 0})
               : t == PortValueType::Vec3                                  ? Value::ofVec3(Vec3{0, 0, 0})
                                                                           : Value::ofFloat(0);
    };
    std::vector<Value> fields;
    if (s.entryType != static_cast<PortValueType>(0)) {
        // An Enum entry is stored as its value name.
        fields.push_back(s.entryType == PortValueType::Enum ? Value::ofStringId(kInvalidString)
                                                            : blank(s.entryType));
        return fields;
    }
    for (PortValueType t : s.fieldTypes) {
        fields.push_back(blank(t));
    }
    return fields;
}

bool BindingRuntime::convertEntry(const Slot& s, const Value& entry, std::vector<Value>& out,
                                  std::vector<TreePtr>* trees) const {
    if (s.entryType != static_cast<PortValueType>(0)) {
        // T-381: a list<number|string|bool|color|vec2|vec3|enum> entry is one Value.
        if (out.size() != 1) {
            return false;
        }
        const Value& v = entry;
        switch (s.entryType) {
            case PortValueType::Number:
                if (v.type != ValueType::Float && v.type != ValueType::Enum) {
                    return false;
                }
                out[0] = Value::ofFloat(v.type == ValueType::Float ? v.f : static_cast<float>(v.e));
                return true;
            case PortValueType::Bool:
                if (v.type != ValueType::Float && v.type != ValueType::Enum) {
                    return false;
                }
                out[0] = Value::ofFloat((v.type == ValueType::Float ? v.f != 0.0f : v.e != 0) ? 1.0f : 0.0f);
                return true;
            case PortValueType::String:
                if (v.type != ValueType::StringRef) {
                    return false;
                }
                out[0] = v;
                return true;
            case PortValueType::Color:
            case PortValueType::Vec2:
            case PortValueType::Vec3: {
                const ValueType want = s.entryType == PortValueType::Color  ? ValueType::Color
                                       : s.entryType == PortValueType::Vec2 ? ValueType::Vec2
                                                                            : ValueType::Vec3;
                if (v.type != want) {
                    return false;
                }
                out[0] = v;
                return true;
            }
            case PortValueType::Enum: {
                // The ordinal or the value name; stored as the name.
                size_t i = s.enumValues.size();
                if (v.type == ValueType::Enum) {
                    i = v.e;
                } else if (v.type == ValueType::Float && isEnumIndex(v.f)) {
                    i = toUint32Sat(v.f);
                } else if (v.type == ValueType::StringRef) {
                    const std::string_view name = m_doc.string(v.sid);
                    i = static_cast<size_t>(std::find(s.enumValues.begin(), s.enumValues.end(), name) -
                                            s.enumValues.begin());
                }
                if (i >= s.enumValues.size()) {
                    return false;
                }
                out[0] = Value::ofStringId(m_doc.intern(s.enumValues[i]));
                return true;
            }
            default:
                return false;
        }
    }
    if (entry.type != ValueType::Struct) {
        return false;
    }
    for (uint32_t i = 0; i < entry.itemCount; ++i) {
        for (size_t f = 0; f < s.fieldNames.size(); ++f) {
            if (s.fieldNames[f] != entry.fieldNames[i] &&
                m_doc.string(s.fieldNames[f]) != m_doc.string(entry.fieldNames[i])) {
                continue;
            }
            const Value& v = entry.items[i];
            const PortValueType t = s.fieldTypes[f];
            Value stored;
            if (t == PortValueType::List) {
                // T-391: a list<T> field: converted into storage of its own. T-405 (02 §2.s.3): a
                // field without an IrPortNested entry has no nesting; its value is dropped.
                if (f >= s.fieldSubs.size() || s.fieldSubs[f] == nullptr) {
                    continue;
                }
                if (trees == nullptr) {
                    return false;
                }
                auto tree = std::make_shared<ValueTree>();
                if (!convertNestedList(*s.fieldSubs[f], v, m_doc, *tree, stored)) {
                    return false;
                }
                if (trees->size() != s.fieldNames.size()) {
                    trees->resize(s.fieldNames.size());
                }
                (*trees)[f] = std::move(tree);
            } else if (t == PortValueType::Number || t == PortValueType::Bool) {
                if (v.type != ValueType::Float && v.type != ValueType::Enum) {
                    return false;
                }
                stored = Value::ofFloat(v.type == ValueType::Float ? v.f : static_cast<float>(v.e));
            } else if (t == PortValueType::Entity) {
                if (v.type != ValueType::Float && v.type != ValueType::Enum) {
                    return false;
                }
                if (v.type == ValueType::Float && !isEnumIndex(v.f)) {
                    return false;  // T-258a: a negative or non-finite entity id.
                }
                stored = Value::ofEnum(v.type == ValueType::Enum ? v.e : toUint32Sat(v.f));
            } else if (t == PortValueType::String || t == PortValueType::Texture) {
                if (v.type != ValueType::StringRef) {
                    return false;
                }
                stored = v;
            } else if ((t == PortValueType::Color && v.type == ValueType::Color) ||
                       (t == PortValueType::Vec2 && v.type == ValueType::Vec2) ||
                       (t == PortValueType::Vec3 && v.type == ValueType::Vec3)) {
                stored = v;
            } else {
                return false;
            }
            out[f] = stored;
        }
    }
    return true;
}

bool BindingRuntime::convertScalar(PortValueType t, const std::vector<std::string>& enums, const Value& v,
                                   const Document& src, Value& out) const {
    switch (t) {
        case PortValueType::Number:
        case PortValueType::Bool: {
            if (v.type != ValueType::Float && v.type != ValueType::Enum) {
                return false;
            }
            const float f = v.type == ValueType::Float ? v.f : static_cast<float>(v.e);
            out = Value::ofFloat(t == PortValueType::Bool ? (f != 0.0f ? 1.0f : 0.0f) : f);
            return true;
        }
        case PortValueType::Entity:
            if ((v.type != ValueType::Float && v.type != ValueType::Enum) ||
                (v.type == ValueType::Float && !isEnumIndex(v.f))) {
                return false;
            }
            out = Value::ofEnum(v.type == ValueType::Enum ? v.e : toUint32Sat(v.f));
            return true;
        case PortValueType::String:
        case PortValueType::Texture:
            if (v.type != ValueType::StringRef) {
                return false;
            }
            out = &src == &m_doc ? v : Value::ofStringId(m_doc.intern(src.string(v.sid)));
            return true;
        case PortValueType::Color:
        case PortValueType::Vec2:
        case PortValueType::Vec3: {
            const ValueType want = t == PortValueType::Color  ? ValueType::Color
                                   : t == PortValueType::Vec2 ? ValueType::Vec2
                                                              : ValueType::Vec3;
            if (v.type != want) {
                return false;
            }
            out = v;
            return true;
        }
        case PortValueType::Enum: {  // A list<enum> entry: the ordinal or the name; stored as the name.
            size_t i = enums.size();
            if (v.type == ValueType::Enum) {
                i = v.e;
            } else if (v.type == ValueType::Float && isEnumIndex(v.f)) {
                i = toUint32Sat(v.f);
            } else if (v.type == ValueType::StringRef) {
                const std::string_view name = src.string(v.sid);
                i = static_cast<size_t>(std::find(enums.begin(), enums.end(), name) - enums.begin());
            }
            if (i >= enums.size()) {
                return false;
            }
            out = Value::ofStringId(m_doc.intern(enums[i]));
            return true;
        }
        default:
            return false;
    }
}

bool BindingRuntime::convertNestedList(const ListSchema& sc, const Value& list, const Document& src,
                                       ValueTree& tree, Value& out) const {
    if (list.type != ValueType::List || (list.itemCount != 0 && list.items == nullptr)) {
        return false;
    }
    std::vector<Value> entries;
    entries.reserve(list.itemCount);
    for (uint32_t e = 0; e < list.itemCount; ++e) {
        const Value& entry = list.items[e];
        if (sc.entryType != static_cast<PortValueType>(0)) {
            Value v;
            if (!convertScalar(sc.entryType, sc.enumValues, entry, src, v)) {
                return false;
            }
            entries.push_back(v);
            continue;
        }
        if (entry.type != ValueType::Struct) {
            return false;
        }
        std::vector<Value> fields;
        fields.reserve(sc.fieldTypes.size());
        for (PortValueType t : sc.fieldTypes) {
            fields.push_back(t == PortValueType::List    ? Value::ofList(nullptr, 0)
                             : t == PortValueType::Color ? Value::ofColor(Color{0, 0, 0, 0})
                             : t == PortValueType::String || t == PortValueType::Texture
                                 ? Value::ofStringId(kInvalidString)
                             : t == PortValueType::Entity ? Value::ofEnum(0)
                             : t == PortValueType::Vec2   ? Value::ofVec2(Vec2{0, 0})
                             : t == PortValueType::Vec3   ? Value::ofVec3(Vec3{0, 0, 0})
                                                          : Value::ofFloat(0));
        }
        for (uint32_t i = 0; i < entry.itemCount; ++i) {
            const std::string_view name = src.string(entry.fieldNames[i]);
            for (size_t f = 0; f < sc.fieldNames.size(); ++f) {
                if (!(&src == &m_doc && sc.fieldNames[f] == entry.fieldNames[i]) &&
                    m_doc.string(sc.fieldNames[f]) != name) {
                    continue;
                }
                if (sc.fieldTypes[f] == PortValueType::List) {
                    // T-405 (02 §2.s.3): a List field without an IrPortNested entry has no nesting;
                    // its value is dropped and the field stays an empty list.
                    if (f < sc.subs.size() && sc.subs[f] != nullptr &&
                        !convertNestedList(*sc.subs[f], entry.items[i], src, tree, fields[f])) {
                        return false;
                    }
                } else if (!convertScalar(sc.fieldTypes[f], {}, entry.items[i], src, fields[f])) {
                    return false;
                }
                break;
            }
        }
        tree.arrays.push_back(std::move(fields));
        const std::vector<Value>& stored = tree.arrays.back();
        entries.push_back(
            Value::ofStruct(sc.fieldNames.data(), stored.data(), static_cast<uint32_t>(stored.size())));
    }
    tree.arrays.push_back(std::move(entries));
    const std::vector<Value>& stored = tree.arrays.back();
    out = Value::ofList(stored.data(), static_cast<uint32_t>(stored.size()));
    return true;
}

void BindingRuntime::rebuildEntries(Slot& s) {
    s.entryValues.clear();
    s.entryValues.reserve(s.entries.size());
    for (const std::vector<Value>& e : s.entries) {
        if (s.entryType != static_cast<PortValueType>(0)) {
            s.entryValues.push_back(e.empty() ? Value::ofFloat(0) : e[0]);  // T-381
            continue;
        }
        s.entryValues.push_back(
            Value::ofStruct(s.fieldNames.data(), e.data(), static_cast<uint32_t>(e.size())));
    }
}

Status BindingRuntime::assignText(uint32_t index, std::string_view text, bool& changed) {
    // assign()'s String / Texture and Enum-from-StringRef cases, from the text itself.
    Slot& s = m_slots[index];
    changed = false;
    switch (static_cast<PortValueType>(s.ir.type)) {
        case PortValueType::String:
        case PortValueType::Texture:
            changed = s.text != text;
            s.text.assign(text);
            return Status::kOk;
        case PortValueType::Enum:
            for (size_t i = 0; i < s.enumValues.size(); ++i) {
                if (s.enumValues[i] == text) {
                    changed = s.num != static_cast<double>(i);
                    s.num = static_cast<double>(i);
                    return Status::kOk;
                }
            }
            return Status::kInvalidArgument;
        default:
            return Status::kInvalidArgument;
    }
}

Status BindingRuntime::assign(uint32_t index, const Value& v, bool& changed) {
    Slot& s = m_slots[index];
    changed = false;
    auto setNum = [&](double n) {
        changed = s.num != n;
        s.num = n;
        return Status::kOk;
    };
    switch (static_cast<PortValueType>(s.ir.type)) {
        case PortValueType::Number:
            if (v.type != ValueType::Float) {
                return Status::kInvalidArgument;
            }
            return setNum(v.f);
        case PortValueType::Bool:
            if (v.type != ValueType::Float && v.type != ValueType::Enum) {
                return Status::kInvalidArgument;
            }
            return setNum((v.type == ValueType::Float ? v.f != 0.0f : v.e != 0) ? 1.0 : 0.0);
        case PortValueType::Entity:
            if ((v.type != ValueType::Float && v.type != ValueType::Enum) ||
                (v.type == ValueType::Float && !isEnumIndex(v.f))) {
                return Status::kInvalidArgument;  // T-258a: negative or non-finite ids too.
            }
            return setNum(v.type == ValueType::Enum ? static_cast<double>(v.e) : std::floor(v.f));
        case PortValueType::Enum:
            if (v.type == ValueType::Enum) {
                if (v.e >= s.enumValues.size()) {
                    return Status::kInvalidArgument;
                }
                return setNum(v.e);
            }
            if (v.type == ValueType::StringRef) {
                std::string_view name = m_doc.string(v.sid);
                for (size_t i = 0; i < s.enumValues.size(); ++i) {
                    if (s.enumValues[i] == name) {
                        return setNum(static_cast<double>(i));
                    }
                }
            }
            return Status::kInvalidArgument;
        case PortValueType::String:
        case PortValueType::Texture: {
            if (v.type != ValueType::StringRef) {
                return Status::kInvalidArgument;
            }
            std::string_view text = m_doc.string(v.sid);
            changed = s.text != text;
            s.text.assign(text);
            return Status::kOk;
        }
        case PortValueType::Color:
        case PortValueType::Vec2:
        case PortValueType::Vec3: {
            const ValueType want =
                static_cast<PortValueType>(s.ir.type) == PortValueType::Color  ? ValueType::Color
                : static_cast<PortValueType>(s.ir.type) == PortValueType::Vec2 ? ValueType::Vec2
                                                                               : ValueType::Vec3;
            if (v.type != want) {
                return Status::kInvalidArgument;
            }
            changed = !valueEquals(s.value, v);
            s.value = v;
            return Status::kOk;
        }
        case PortValueType::Struct: {
            std::vector<Value> next = s.fields;
            std::vector<TreePtr> trees = s.fieldTrees;  // T-391: the List fields' storage.
            if (!convertEntry(s, v, next, &trees)) {
                return Status::kInvalidArgument;
            }
            for (size_t i = 0; i < next.size(); ++i) {
                changed = changed || !valueEquals(next[i], s.fields[i]);
            }
            s.fields = std::move(next);
            s.fieldTrees = std::move(trees);
            return Status::kOk;
        }
        case PortValueType::List: {
            if (v.type != ValueType::List) {
                return Status::kInvalidArgument;
            }
            return listSet(static_cast<PortId>(index + 1), std::span<const Value>(v.items, v.itemCount));
        }
        case PortValueType::Event:
            changed = true;  // An event always fires.
            return Status::kOk;
    }
    return Status::kInvalidArgument;
}

void BindingRuntime::notifyListeners(PortEventKind kind, uint32_t index, uint32_t listIndex,
                                     uint32_t itemIndex, const Value* item) {
    if (m_listeners.empty() || m_doc.m_unloading) {  // T-260a: an unloading Document is silent.
        return;
    }
    PortEvent event;
    event.kind = kind;
    event.port = index + 1;
    event.value = publicValue(index);
    event.index = listIndex;
    event.instanceId = m_doc.m_instanceId;
    event.itemIndex = itemIndex;           // T-177.
    event.fromDocument = m_documentWrite;  // T-394 (08.15).
    if (item != nullptr) {
        event.item = *item;
    }
    // T-478: in place, by index, without copying the list (the copy cloned every std::function on
    // each port change). Callbacks may edit the list: a listener added meanwhile (appended; the
    // deque keeps the running callback where it is) is first called by the next notification, one
    // removed meanwhile (tombstoned, id 0) is not called any more; the outermost call erases the
    // tombstones when it returns.
    const size_t count = m_listeners.size();
    ++m_notifyDepth;
    for (size_t i = 0; i < count; ++i) {
        if (m_doc.m_unloading) {
            break;  // T-260a: an earlier listener unloaded this Document.
        }
        if (m_listeners[i].first != 0) {
            m_listeners[i].second(event);
        }
    }
    if (--m_notifyDepth == 0 && m_listenerTombstones) {
        m_listenerTombstones = false;
        m_listeners.erase(
            std::remove_if(m_listeners.begin(), m_listeners.end(),
                           [](const std::pair<uint32_t, PortListener>& l) { return l.first == 0; }),
            m_listeners.end());
    }
}

void BindingRuntime::fireTriggers(uint32_t index) {
    const Slot& s = m_slots[index];
    if (s.triggerNodes.empty()) {
        return;
    }
    const float value = static_cast<float>(toNum(portValue(index)));
    std::vector<NodeId> nodes = s.triggerNodes;
    // T-380: the IR trigger names the template port; the condition reads the instance's copy.
    const uint32_t irPort = s.instance ? s.templatePort : index;
    for (NodeId node : nodes) {
        m_doc.m_triggers->fire(node, TriggerEvent::Port, kInvalidString, irPort + 1, value, index);
    }
}

void BindingRuntime::notify(uint32_t index, PortEventKind kind, uint32_t listIndex) {
    if (m_doc.behavior() >= kBehaviorFlowSignalCoalescing) {
        // T-389 (27 §27.4.1): marked, evaluated once by the next Document::update() (flushDirty()).
        for (uint32_t b : m_slots[index].bindings) {
            markDirty(b);
        }
    } else {
        // T-407 (43 §43.3): behavior 0 re-evaluates every binding that reads the port at once,
        // as before T-389 (by copy: an evaluation may set another port).
        std::vector<uint32_t> bindings = m_slots[index].bindings;
        for (uint32_t b : bindings) {
            evaluate(b, /*initial=*/false);
        }
    }
    notifyListeners(kind, index, listIndex);
    fireTriggers(index);
}

// -- `<repeat>` instances (T-177) -----------------------------------------------------------------

uint32_t BindingRuntime::addInstanceBinding(uint32_t irBinding, NodeId node, uint32_t listPort, uint32_t item,
                                            std::span<const std::pair<uint32_t, uint32_t>> portMap) {
    uint32_t index = 0;
    if (!m_freeBindings.empty()) {
        index = m_freeBindings.back();
        m_freeBindings.pop_back();
    } else {
        index = static_cast<uint32_t>(m_bindings.size());
        m_bindings.emplace_back();
    }
    const Binding& tpl = m_bindings[irBinding];
    Binding b;
    b.ir = tpl.ir;
    b.steps = tpl.steps;
    b.sources = tpl.sources;
    b.primary = tpl.primary;
    b.maxPort = tpl.maxPort;
    b.transitionTween = tpl.transitionTween;
    b.tweenDuration = tpl.tweenDuration;
    b.tweenEasing = tpl.tweenEasing;
    b.usesCustom = tpl.usesCustom;
    b.usesLocalize = tpl.usesLocalize;
    b.state.resize(b.steps.size());
    b.node = node;
    if (b.ir->sceneNode != kBindElementSelf) {
        b.node = resolveSceneBinding(*b.ir, node);  // T-243: the copy's own scene node.
    }
    b.generation = b.node != kInvalidNode ? m_doc.m_arena.generation(b.node) : 0;
    b.instance = true;
    b.listPort = listPort;
    b.item = item;
    for (const IrConverterStep& step : b.steps) {  // T-391: `outer.item(...)` / `outer.index`.
        for (uint32_t k = 0; k < 4; ++k) {
            const ConverterArg kind = converterArgKind(step, k);
            b.usesOuter = b.usesOuter || kind == ConverterArg::OuterItem || kind == ConverterArg::OuterIndex;
        }
    }
    if (!portMap.empty()) {
        // T-380: the copy reads (and targets) the instance's own copies of template ports.
        b.portMap.assign(portMap.begin(), portMap.end());
        b.ownSources.reserve(tpl.sources.size());
        for (uint32_t source : tpl.sources) {
            b.ownSources.push_back(mapPort(portMap, source));
        }
        b.primary =
            tpl.primary >= 0 ? static_cast<int>(mapPort(portMap, static_cast<uint32_t>(tpl.primary))) : -1;
        b.maxPort =
            tpl.maxPort >= 0 ? static_cast<int>(mapPort(portMap, static_cast<uint32_t>(tpl.maxPort))) : -1;
    }
    m_bindings[index] = std::move(b);
    if (!m_bindings[index].ownSources.empty()) {
        m_bindings[index].sources = m_bindings[index].ownSources;
    }
    for (uint32_t source : m_bindings[index].sources) {
        std::vector<uint32_t>& list = m_slots[source].bindings;
        if (std::find(list.begin(), list.end(), index) == list.end()) {
            list.push_back(index);
        }
    }
    if ((m_bindings[index].ir->flags & kBindAfterLayout) != 0) {
        m_afterLayout.push_back(index);
    }
    return index;
}

void BindingRuntime::setInstanceItem(uint32_t binding, uint32_t item) {
    if (binding >= m_bindings.size() || !m_bindings[binding].instance) {
        return;
    }
    Binding& b = m_bindings[binding];
    b.item = item;
    b.applied = false;
    if (item == kNoItem) {
        b.tweened = false;  // T-546: a retired / pooled instance's next first write is direct.
    }
    b.ticking = false;
    b.last = BVal{};
    b.state.assign(b.steps.size(), StepState{});
    if (b.node != kInvalidNode && m_doc.m_arena.alive(b.node)) {
        b.generation = m_doc.m_arena.generation(b.node);
    }
}

void BindingRuntime::removeBinding(uint32_t binding) {
    if (binding >= m_bindings.size() || !m_bindings[binding].instance || m_bindings[binding].free) {
        return;
    }
    for (Slot& s : m_slots) {
        s.bindings.erase(std::remove(s.bindings.begin(), s.bindings.end(), binding), s.bindings.end());
    }
    m_afterLayout.erase(std::remove(m_afterLayout.begin(), m_afterLayout.end(), binding),
                        m_afterLayout.end());
    Binding& b = m_bindings[binding];
    b = Binding{};
    b.instance = true;
    b.free = true;
    m_freeBindings.push_back(binding);
}

void BindingRuntime::bindTriggerNode(NodeId node, std::span<const std::pair<uint32_t, uint32_t>> portMap) {
    if (node == kInvalidNode || !m_doc.m_arena.alive(node)) {
        return;
    }
    const uint32_t table = m_doc.m_arena.common(node).triggerTable;
    const TriggerChunk& tc = m_doc.m_ir.triggers();
    if (table == 0 || table > tc.tables().size()) {
        return;
    }
    const IrTriggerTable& t = tc.tables()[table - 1];
    for (uint32_t k = 0; k < t.triggerCount; ++k) {
        const IrTrigger& trig = tc.triggers()[t.triggerOffset + k];
        if (static_cast<TriggerEvent>(trig.event) != TriggerEvent::Port || trig.arg1 == 0 ||
            trig.arg1 == kInvalidString || trig.arg1 > m_slots.size()) {
            continue;
        }
        const uint32_t port = mapPort(portMap, trig.arg1 - 1);  // T-380
        if (port >= m_slots.size()) {
            continue;
        }
        std::vector<NodeId>& nodes = m_slots[port].triggerNodes;
        if (std::find(nodes.begin(), nodes.end(), node) == nodes.end()) {
            nodes.push_back(node);
        }
    }
}

uint32_t BindingRuntime::addInstancePort(uint32_t templatePort, NodeId element, NodeId scope) {
    if (templatePort >= m_slots.size()) {
        return kNoPort;
    }
    Slot s = m_slots[templatePort];  // A template slot is never set: it holds the declared default.
    s.strings = StringGuard{};
    s.bindings.clear();
    s.triggerNodes.clear();
    s.callHandler = nullptr;
    s.maxPort = -1;  // linkInstancePorts().
    s.bone = kInvalidNode;
    s.templateOnly = false;
    s.instance = true;
    s.free = false;
    s.templatePort = templatePort;
    s.element = element;
    s.scopeNode = scope;
    s.elementId = std::string(nodeIdText(element));
    s.qualifiedName = s.elementId.empty() ? s.name : s.elementId + "." + s.name;
    s.exposedName.clear();
    if ((s.ir.flags & kPortExpose) != 0 && !nodeIdText(scope).empty()) {
        s.exposedName = std::string(nodeIdText(scope)) + "." + s.name;
    }
    rebuildEntries(s);  // The copied Struct Values pointed into the template's storage.
    uint32_t index = 0;
    if (!m_freePorts.empty()) {
        index = m_freePorts.back();
        m_freePorts.pop_back();
        m_slots[index] = std::move(s);
    } else {
        index = static_cast<uint32_t>(m_slots.size());
        m_slots.push_back(std::move(s));
    }
    return index;
}

void BindingRuntime::linkInstancePorts(std::span<const std::pair<uint32_t, uint32_t>> portMap) {
    for (const auto& [from, to] : portMap) {
        if (from >= m_slots.size() || to >= m_slots.size()) {
            continue;
        }
        const int max = m_slots[from].maxPort;
        m_slots[to].maxPort = max >= 0 ? static_cast<int>(mapPort(portMap, static_cast<uint32_t>(max))) : -1;
    }
}

void BindingRuntime::resetInstancePort(uint32_t index) {
    if (index >= m_slots.size() || !m_slots[index].instance || m_slots[index].free ||
        m_slots[index].templatePort >= m_slots.size()) {
        return;
    }
    const Slot& t = m_slots[m_slots[index].templatePort];
    Slot& s = m_slots[index];
    s.num = t.num;
    s.text = t.text;
    s.value = t.value;
    s.fields = t.fields;
    s.fieldTrees = t.fieldTrees;  // T-391: immutable storage, shared.
    s.entries = t.entries;
    s.entryTrees = t.entryTrees;
    rebuildEntries(s);
}

void BindingRuntime::removeInstancePort(uint32_t index) {
    if (index >= m_slots.size() || !m_slots[index].instance || m_slots[index].free) {
        return;
    }
    m_slots[index] = Slot{};
    m_slots[index].instance = true;
    m_slots[index].free = true;
    m_freePorts.push_back(index);
}

void BindingRuntime::forgetTriggerNode(NodeId node) {
    for (Slot& s : m_slots) {
        s.triggerNodes.erase(std::remove(s.triggerNodes.begin(), s.triggerNodes.end(), node),
                             s.triggerNodes.end());
    }
}

Value BindingRuntime::entryValue(uint32_t listPort, uint32_t item) const {
    if (listPort >= m_slots.size() || item >= m_slots[listPort].entryValues.size()) {
        return Value::ofFloat(0);
    }
    return m_slots[listPort].entryValues[item];
}

uint32_t BindingRuntime::entryCount(uint32_t listPort) const {
    return listPort < m_slots.size() ? static_cast<uint32_t>(m_slots[listPort].entries.size()) : 0;
}

// -- Nested lists (T-391) -------------------------------------------------------------------------

void BindingRuntime::setInstanceOuter(uint32_t binding, uint32_t outerPort, uint32_t outerItem) {
    if (binding >= m_bindings.size() || !m_bindings[binding].instance || m_bindings[binding].free) {
        return;
    }
    m_bindings[binding].outerPort = outerPort;
    m_bindings[binding].outerItem = outerItem;
}

uint32_t BindingRuntime::addNestedList(uint32_t parent, uint32_t field, uint32_t item, NodeId repeatNode) {
    if (parent >= m_slots.size() || m_slots[parent].free ||
        static_cast<PortValueType>(m_slots[parent].ir.type) != PortValueType::List ||
        m_slots[parent].entryType != static_cast<PortValueType>(0) ||
        field >= m_slots[parent].fieldNames.size() || field >= m_slots[parent].fieldSubs.size() ||
        m_slots[parent].fieldSubs[field] == nullptr) {
        return kNoPort;
    }
    const ListSchema& sc = *m_slots[parent].fieldSubs[field];
    Slot s;
    s.ir.name = m_slots[parent].fieldNames[field];
    s.ir.type = static_cast<uint8_t>(PortValueType::List);
    s.ir.dir = static_cast<uint8_t>(PortDir::In);
    s.ir.entryType = static_cast<uint8_t>(sc.entryType);
    s.ir.scope = m_slots[parent].ir.scope;
    s.name = std::string(m_doc.string(m_slots[parent].fieldNames[field]));
    s.entryType = sc.entryType;
    s.fieldNames = sc.fieldNames;
    s.fieldTypes = sc.fieldTypes;
    s.enumValues = sc.enumValues;
    s.fieldSubs = sc.subs;
    s.nested = true;
    s.nestedParent = parent;
    s.nestedField = field;
    s.nestedItem = item;
    s.nestedRepeat = repeatNode;
    uint32_t index = 0;
    if (!m_freePorts.empty()) {
        index = m_freePorts.back();
        m_freePorts.pop_back();
        m_slots[index] = std::move(s);
    } else {
        index = static_cast<uint32_t>(m_slots.size());
        m_slots.push_back(std::move(s));
    }
    m_slots[index].qualifiedName = nestedName(m_slots[index]);
    refreshNestedList(index);
    return index;
}

void BindingRuntime::setNestedItem(uint32_t slot, uint32_t item) {
    if (slot < m_slots.size() && m_slots[slot].nested && !m_slots[slot].free) {
        m_slots[slot].nestedItem = item;
        m_slots[slot].qualifiedName = nestedName(m_slots[slot]);
    }
}

bool BindingRuntime::refreshNestedList(uint32_t slot) {
    if (slot >= m_slots.size() || !m_slots[slot].nested || m_slots[slot].free) {
        return false;
    }
    Value from = Value::ofList(nullptr, 0);
    {
        const Slot& s = m_slots[slot];
        if (s.nestedParent < m_slots.size() && s.nestedItem != kNoItem) {
            const Slot& p = m_slots[s.nestedParent];
            if (s.nestedItem < p.entries.size() && s.nestedField < p.entries[s.nestedItem].size() &&
                p.entries[s.nestedItem][s.nestedField].type == ValueType::List) {
                from = p.entries[s.nestedItem][s.nestedField];
            }
        }
        const Value current =
            Value::ofList(s.entryValues.data(), static_cast<uint32_t>(s.entryValues.size()));
        if (valueEquals(current, from)) {
            return false;
        }
    }
    Slot& s = m_slots[slot];
    std::vector<std::vector<Value>> next;
    std::vector<std::vector<TreePtr>> nextTrees;
    for (uint32_t i = 0; i < from.itemCount; ++i) {
        std::vector<Value> fields = blankEntry(s);
        std::vector<TreePtr> trees;
        if (!convertEntry(s, from.items[i], fields, &trees)) {
            continue;  // Cannot happen: the enclosing port converted it with the same schema.
        }
        next.push_back(std::move(fields));
        nextTrees.push_back(std::move(trees));
    }
    s.entries = std::move(next);
    s.entryTrees = std::move(nextTrees);
    rebuildEntries(s);
    return true;
}

void BindingRuntime::removeNestedList(uint32_t slot) {
    if (slot >= m_slots.size() || !m_slots[slot].nested || m_slots[slot].free) {
        return;
    }
    m_slots[slot] = Slot{};
    m_slots[slot].nested = true;
    m_slots[slot].free = true;
    m_freePorts.push_back(slot);
}

void BindingRuntime::writeBackNested(uint32_t slot) {
    // Up the chain of enclosing entries: each takes its nested slot's list as the field's value and
    // then notifies as listUpdate() of that entry would (which re-evaluates the enclosing instance;
    // its nested repeats find their lists unchanged, or, for a sibling repeat over the same field,
    // refresh).
    for (uint32_t depth = 0; depth < 64; ++depth) {
        if (slot >= m_slots.size() || !m_slots[slot].nested || m_slots[slot].free) {
            return;
        }
        const uint32_t parent = m_slots[slot].nestedParent;
        const uint32_t item = m_slots[slot].nestedItem;
        const uint32_t field = m_slots[slot].nestedField;
        if (parent >= m_slots.size() || m_slots[parent].free || item >= m_slots[parent].entries.size() ||
            field >= m_slots[parent].fieldSubs.size() || m_slots[parent].fieldSubs[field] == nullptr ||
            field >= m_slots[parent].entries[item].size()) {
            return;
        }
        const Slot& c = m_slots[slot];
        auto tree = std::make_shared<ValueTree>();
        Value out;
        if (!convertNestedList(
                *m_slots[parent].fieldSubs[field],
                Value::ofList(c.entryValues.data(), static_cast<uint32_t>(c.entryValues.size())), m_doc,
                *tree, out)) {
            return;
        }
        Slot& p = m_slots[parent];
        fitTrees(p);
        if (p.entryTrees[item].size() != p.fieldNames.size()) {
            p.entryTrees[item].resize(p.fieldNames.size());
        }
        p.entries[item][field] = out;
        p.entryTrees[item][field] = std::move(tree);
        rebuildEntries(p);
        const bool parentNested = p.nested;
        m_doc.m_repeats->onListChanged(parent, RepeatListOp::Update, item);
        notify(parent, PortEventKind::ListChanged, item);
        if (!parentNested) {
            return;
        }
        slot = parent;
    }
}

}  // namespace xt
