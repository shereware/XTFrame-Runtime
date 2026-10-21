#include "document_state.h"

#include <cmath>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "binding_runtime.h"
#include "dispatch_guard.h"
#include "event_dispatcher.h"
#include "mini_json.h"
#include "text_edit.h"
#include "timeline_group.h"
#include "xt/document.h"
#include "xt/property.h"

/// Document::snapshotState / restoreState and the collect / check / apply code Context::hotReload
/// shares (T-398; docs/design/23-host-integration.md §23.ad.1, 22-bundle.md §22.10).
///
/// Snapshot JSON (version 1), every member optional on restore:
///   {"version":1,
///    "ports":{"<qualified name>":"<setPortText() text>", ...},
///    "states":{"<element id>":["<state>", ...], ...},
///    "inputs":{"<element id>":{"value":"...","cursor":N,"anchor":N}, ...},
///    "focus":{"<player>":"<element id>", ...},
///    "scroll":{"<element id>":[x,y], ...},
///    "timelines":[{"scope":"<id>","name":"...","t":S,"rate":R,"playing":B,"reverse":B,
///                  "loop":B,"hold":B}, ...]}
/// Port values are text so restoreState() can hand them to setPortText() as they are: numbers with
/// 9 significant digits (a float round-trips), Bool `true`/`false`, Enum the value name, Color
/// `#rrggbb[aa]`, vec2/vec3 `x,y[,z]`, Struct / List the JSON setPortText() reads.

namespace xt {

namespace {

using minijson::Json;
using minijson::JsonReader;

void appendNumber(std::string& out, double v) {
    if (!std::isfinite(v)) {
        v = 0;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    out += buf;
}

std::string numberText(double v) {
    std::string out;
    appendNumber(out, v);
    return out;
}

void appendString(std::string& out, std::string_view s) {
    out += '"';
    for (const char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (u < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(u));
            out += buf;
        } else {
            out += c;
        }
    }
    out += '"';
}

void appendColor(std::string& out, const Color& c) {
    auto byte = [](float f) {
        const float clamped = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
        return static_cast<int>(std::lround(clamped * 255.0f));
    };
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", byte(c.r), byte(c.g), byte(c.b), byte(c.a));
    appendString(out, buf);
}

/// A Struct / List port value (and its fields / entries) as the JSON setPortText() reads.
void appendValueJson(std::string& out, const Document& doc, const Value& v, int depth) {
    if (depth > 16) {
        out += "null";
        return;
    }
    switch (v.type) {
        case ValueType::Float:
            appendNumber(out, v.f);
            break;
        case ValueType::Enum:
            appendNumber(out, static_cast<double>(v.e));
            break;
        case ValueType::StringRef:
            appendString(out, v.sid != kInvalidString ? doc.string(v.sid) : std::string_view());
            break;
        case ValueType::Color:
            appendColor(out, v.c);
            break;
        case ValueType::Vec2:
            out += '[';
            appendNumber(out, v.v2.x);
            out += ',';
            appendNumber(out, v.v2.y);
            out += ']';
            break;
        case ValueType::Vec3:
            out += '[';
            appendNumber(out, v.v3.x);
            out += ',';
            appendNumber(out, v.v3.y);
            out += ',';
            appendNumber(out, v.v3.z);
            out += ']';
            break;
        case ValueType::Struct:
            out += '{';
            for (uint32_t i = 0; i < v.itemCount; ++i) {
                if (i > 0) {
                    out += ',';
                }
                appendString(out, v.fieldNames != nullptr ? doc.string(v.fieldNames[i]) : std::string_view());
                out += ':';
                appendValueJson(out, doc, v.items[i], depth + 1);
            }
            out += '}';
            break;
        case ValueType::List:
            out += '[';
            for (uint32_t i = 0; i < v.itemCount; ++i) {
                if (i > 0) {
                    out += ',';
                }
                appendValueJson(out, doc, v.items[i], depth + 1);
            }
            out += ']';
            break;
        default:
            out += "null";
            break;
    }
}

const Json* member(const Json& obj, std::string_view key) {
    if (obj.kind != Json::Kind::Obj) {
        return nullptr;
    }
    for (const auto& field : obj.fields) {
        if (field.first == key) {
            return &field.second;
        }
    }
    return nullptr;
}

double numberOr(const Json* v, double fallback) {
    return v != nullptr && v->kind == Json::Kind::Num ? v->n : fallback;
}

bool boolOr(const Json* v, bool fallback) {
    return v != nullptr && v->kind == Json::Kind::Bool ? v->b : fallback;
}

uint32_t offsetOr(const Json* v) {
    const double n = numberOr(v, 0);
    return n > 0 && n < 4294967295.0 ? static_cast<uint32_t>(n) : 0u;
}

void markStyleDirtySubtree(NodeArena& arena, NodeId node) {
    arena.markDirty(node, static_cast<uint8_t>(kDirtyStyle));
    arena.traversePreorder(node, [&arena](NodeId n) {
        arena.markDirty(n, static_cast<uint8_t>(kDirtyStyle));
        return true;
    });
}

/// The DocumentState a snapshot JSON describes; false when it is not one.
bool parseState(std::string_view text, DocumentState& out) {
    Json root;
    if (!JsonReader(text).parse(root) || root.kind != Json::Kind::Obj) {
        return false;
    }
    if (numberOr(member(root, "version"), 0) != 1) {
        return false;
    }
    if (const Json* ports = member(root, "ports"); ports != nullptr && ports->kind == Json::Kind::Obj) {
        for (const auto& [name, v] : ports->fields) {
            if (v.kind == Json::Kind::Str) {
                out.ports.push_back(DocumentState::Port{name, v.s});
            }
        }
    }
    if (const Json* states = member(root, "states"); states != nullptr && states->kind == Json::Kind::Obj) {
        for (const auto& [id, v] : states->fields) {
            if (v.kind != Json::Kind::Arr) {
                continue;
            }
            DocumentState::States entry;
            entry.id = id;
            for (const Json& name : v.items) {
                if (name.kind == Json::Kind::Str) {
                    entry.names.push_back(name.s);
                }
            }
            out.states.push_back(std::move(entry));
        }
    }
    if (const Json* inputs = member(root, "inputs"); inputs != nullptr && inputs->kind == Json::Kind::Obj) {
        for (const auto& [id, v] : inputs->fields) {
            const Json* value = member(v, "value");
            if (value == nullptr || value->kind != Json::Kind::Str) {
                continue;
            }
            out.inputs.push_back(DocumentState::Input{id, value->s, offsetOr(member(v, "cursor")),
                                                      offsetOr(member(v, "anchor"))});
        }
    }
    if (const Json* focus = member(root, "focus"); focus != nullptr && focus->kind == Json::Kind::Obj) {
        for (const auto& [player, v] : focus->fields) {
            if (player.size() == 1 && player[0] >= '0' && player[0] < static_cast<char>('0' + kMaxPlayers) &&
                v.kind == Json::Kind::Str) {
                out.focus[player[0] - '0'] = v.s;
            }
        }
    }
    if (const Json* scroll = member(root, "scroll"); scroll != nullptr && scroll->kind == Json::Kind::Obj) {
        for (const auto& [id, v] : scroll->fields) {
            if (v.kind == Json::Kind::Arr && v.items.size() == 2 && v.items[0].kind == Json::Kind::Num &&
                v.items[1].kind == Json::Kind::Num) {
                out.scrolls.push_back(DocumentState::Scroll{id, static_cast<float>(v.items[0].n),
                                                            static_cast<float>(v.items[1].n)});
            }
        }
    }
    if (const Json* timelines = member(root, "timelines");
        timelines != nullptr && timelines->kind == Json::Kind::Arr) {
        for (const Json& v : timelines->items) {
            const Json* scope = member(v, "scope");
            const Json* name = member(v, "name");
            if (name == nullptr || name->kind != Json::Kind::Str) {
                continue;
            }
            DocumentState::Timeline entry;
            entry.scope = scope != nullptr && scope->kind == Json::Kind::Str ? scope->s : std::string();
            entry.name = name->s;
            entry.t = static_cast<float>(numberOr(member(v, "t"), 0));
            entry.rate = static_cast<float>(numberOr(member(v, "rate"), 1));
            entry.playing = boolOr(member(v, "playing"), false);
            entry.reverse = boolOr(member(v, "reverse"), false);
            entry.loop = boolOr(member(v, "loop"), false);
            entry.hold = boolOr(member(v, "hold"), false);
            out.timelines.push_back(std::move(entry));
        }
    }
    return true;
}

std::string serializeState(const DocumentState& state) {
    std::string out = "{\"version\":1,\"ports\":{";
    for (size_t i = 0; i < state.ports.size(); ++i) {
        out += i > 0 ? "," : "";
        appendString(out, state.ports[i].name);
        out += ':';
        appendString(out, state.ports[i].text);
    }
    out += "},\"states\":{";
    for (size_t i = 0; i < state.states.size(); ++i) {
        out += i > 0 ? "," : "";
        appendString(out, state.states[i].id);
        out += ":[";
        for (size_t k = 0; k < state.states[i].names.size(); ++k) {
            out += k > 0 ? "," : "";
            appendString(out, state.states[i].names[k]);
        }
        out += ']';
    }
    out += "},\"inputs\":{";
    for (size_t i = 0; i < state.inputs.size(); ++i) {
        const DocumentState::Input& in = state.inputs[i];
        out += i > 0 ? "," : "";
        appendString(out, in.id);
        out += ":{\"value\":";
        appendString(out, in.value);
        out += ",\"cursor\":" + std::to_string(in.cursor) + ",\"anchor\":" + std::to_string(in.anchor) + "}";
    }
    out += "},\"focus\":{";
    bool first = true;
    for (uint8_t p = 0; p < kMaxPlayers; ++p) {
        if (state.focus[p].empty()) {
            continue;
        }
        out += first ? "" : ",";
        first = false;
        out += "\"" + std::to_string(p) + "\":";
        appendString(out, state.focus[p]);
    }
    out += "},\"scroll\":{";
    for (size_t i = 0; i < state.scrolls.size(); ++i) {
        out += i > 0 ? "," : "";
        appendString(out, state.scrolls[i].id);
        out += ":[";
        appendNumber(out, state.scrolls[i].x);
        out += ',';
        appendNumber(out, state.scrolls[i].y);
        out += ']';
    }
    out += "},\"timelines\":[";
    for (size_t i = 0; i < state.timelines.size(); ++i) {
        const DocumentState::Timeline& g = state.timelines[i];
        out += i > 0 ? "," : "";
        out += "{\"scope\":";
        appendString(out, g.scope);
        out += ",\"name\":";
        appendString(out, g.name);
        out += ",\"t\":";
        appendNumber(out, g.t);
        out += ",\"rate\":";
        appendNumber(out, g.rate);
        out += std::string(",\"playing\":") + (g.playing ? "true" : "false");
        out += std::string(",\"reverse\":") + (g.reverse ? "true" : "false");
        out += std::string(",\"loop\":") + (g.loop ? "true" : "false");
        out += std::string(",\"hold\":") + (g.hold ? "true" : "false");
        out += '}';
    }
    out += "]}";
    return out;
}

}  // namespace

void Document::collectState(DocumentState& out, bool withPorts) {
    auto idOf = [this](NodeId node) -> std::string {
        if (node == kInvalidNode || !m_arena.alive(node) || m_arena.kind(node) != NodeKind::Element) {
            return std::string();
        }
        const StringId id = m_arena.element(node).id;
        return id != kInvalidString ? std::string(string(id)) : std::string();
    };
    if (withPorts) {
        for (const PortRecord& record : ports()) {
            std::string text;
            switch (record.type) {
                case PortValueType::Event:
                    continue;
                case PortValueType::Number:
                    text = numberText(getPort(record.id).f);
                    break;
                case PortValueType::Vec2: {
                    const Value v = getPort(record.id);
                    text = numberText(v.v2.x) + "," + numberText(v.v2.y);
                    break;
                }
                case PortValueType::Vec3: {
                    const Value v = getPort(record.id);
                    text = numberText(v.v3.x) + "," + numberText(v.v3.y) + "," + numberText(v.v3.z);
                    break;
                }
                case PortValueType::Struct:
                case PortValueType::List:
                    appendValueJson(text, *this, getPort(record.id), 0);
                    break;
                default:  // String, Texture, Bool, Enum (the value name), Entity, Color.
                    text = portText(record.id);
                    break;
            }
            out.ports.push_back(DocumentState::Port{record.qualifiedName, std::move(text)});
        }
    }
    if (m_root != kInvalidNode) {
        const std::span<const StringId> names = m_ir.customStates().names();
        std::vector<NodeId> elements;
        m_arena.traversePreorder(m_root, [&](NodeId n) {
            if (m_arena.kind(n) == NodeKind::Element && m_arena.element(n).id != kInvalidString) {
                elements.push_back(n);
            }
            return true;
        });
        for (const NodeId n : elements) {
            const std::string id = idOf(n);
            const uint32_t bits = m_arena.common(n).customStates;
            if (bits != 0) {
                DocumentState::States entry;
                entry.id = id;
                for (uint32_t b = 0; b < kMaxCustomStates && b < names.size(); ++b) {
                    if ((bits & (1u << b)) != 0) {
                        entry.names.emplace_back(m_ir.string(names[b]));
                    }
                }
                out.states.push_back(std::move(entry));
            }
            if (const EditState* edit = editStateForElement(n); edit != nullptr) {
                out.inputs.push_back(DocumentState::Input{id, edit->value, edit->cursor, edit->anchor});
            }
            const ElementData& data = m_arena.element(n);
            const float sx = data.style.base[prop::ScrollX].f;
            const float sy = data.style.base[prop::ScrollY].f;
            if (sx != 0.0f || sy != 0.0f) {
                out.scrolls.push_back(DocumentState::Scroll{id, sx, sy});
            }
        }
    }
    for (uint8_t p = 0; p < kMaxPlayers; ++p) {
        out.focus[p] = idOf(focus(p).node);
    }
    for (const TimelineGroupRuntime& g : m_timelineGroups->groups) {
        if (!g.active) {
            continue;
        }
        DocumentState::Timeline entry;
        const NodeId scope =
            g.scopeElement < m_docElementNodes.size() ? m_docElementNodes[g.scopeElement] : kInvalidNode;
        entry.scope = scope == m_root ? std::string() : idOf(scope);
        entry.name = std::string(m_ir.string(g.name));
        entry.playing = g.playing;
        entry.reverse = g.reverse;
        entry.loop = g.loop;
        entry.hold = g.hold;
        entry.t = g.t;
        entry.rate = g.rate;
        out.timelines.push_back(std::move(entry));
    }
}

void Document::missingState(const DocumentState& state, std::vector<std::string>& missing) const {
    const std::span<const StringId> names = m_ir.customStates().names();
    auto hasState = [&](std::string_view name) {
        for (const StringId n : names) {
            if (m_ir.string(n) == name) {
                return true;
            }
        }
        return false;
    };
    for (const DocumentState::States& entry : state.states) {
        if (!byId(entry.id).valid()) {
            missing.push_back("element " + entry.id);
            continue;
        }
        for (const std::string& name : entry.names) {
            if (!hasState(name)) {
                missing.push_back("state " + entry.id + ":" + name);
            }
        }
    }
    for (const std::string& id : state.focus) {
        if (!id.empty() && !byId(id).valid()) {
            missing.push_back("element " + id);
        }
    }
    for (const DocumentState::Timeline& g : state.timelines) {
        if (findTimelineGroup(g.scope, g.name) < 0) {
            missing.push_back("timeline " + g.scope + ":" + g.name);
        }
    }
}

void Document::applyState(const DocumentState& state, bool exact) {
    DispatchGuard dispatch(m_context);  // Port listeners, triggers, Focus / Blur.
    // 1. Ports (bindings follow).
    for (const DocumentState::Port& port : state.ports) {
        const PortId id = lookupPort(port.name);
        if (id != kInvalidPort) {
            setPortText(id, port.text);  // A value this document's port cannot take is skipped.
        }
    }
    if (m_root == kInvalidNode) {
        return;
    }
    // 2. Custom states, by name; with `exact` the id'd elements not listed lose theirs.
    const std::span<const StringId> names = m_ir.customStates().names();
    auto stateBit = [&](std::string_view name) -> int {
        for (uint32_t b = 0; b < names.size() && b < kMaxCustomStates; ++b) {
            if (m_ir.string(names[b]) == name) {
                return static_cast<int>(b);
            }
        }
        return -1;
    };
    std::vector<std::pair<NodeId, uint32_t>> wanted;
    if (exact) {
        m_arena.traversePreorder(m_root, [&](NodeId n) {
            if (m_arena.kind(n) == NodeKind::Element && m_arena.element(n).id != kInvalidString &&
                m_arena.common(n).customStates != 0) {
                wanted.emplace_back(n, 0u);
            }
            return true;
        });
    }
    for (const DocumentState::States& entry : state.states) {
        const NodeId node = byId(entry.id).node;
        if (node == kInvalidNode) {
            continue;
        }
        uint32_t bits = 0;
        for (const std::string& name : entry.names) {
            const int bit = stateBit(name);
            if (bit >= 0) {
                bits |= 1u << static_cast<uint32_t>(bit);
            }
        }
        wanted.emplace_back(node, bits);  // After the clears, so it wins.
    }
    for (const auto& [node, bits] : wanted) {
        if (m_arena.alive(node) && m_arena.common(node).customStates != bits) {
            m_arena.common(node).customStates = bits;
            markStyleDirtySubtree(m_arena, node);
        }
    }
    // 3. Input values and selections (quiet, as a host restoring; no Change event).
    for (const DocumentState::Input& input : state.inputs) {
        const NodeId node = byId(input.id).node;
        EditState* edit = node != kInvalidNode ? editStateForElement(node) : nullptr;
        if (edit == nullptr) {
            continue;
        }
        m_events->setEditValue(m_arena, node, input.value);
        const uint32_t size = static_cast<uint32_t>(edit->value.size());
        edit->cursor = input.cursor < size ? input.cursor : size;
        edit->anchor = input.anchor < size ? input.anchor : size;
        m_arena.markDirty(node, static_cast<uint8_t>(kDirtyPaint));
    }
    // 4. Focus.
    for (uint8_t p = 0; p < kMaxPlayers; ++p) {
        if (!state.focus[p].empty()) {
            const ElementRef target = byId(state.focus[p]);
            if (target.valid() && focus(p).node != target.node) {
                setFocus(target, p);
            }
        } else if (exact && focus(p).valid()) {
            setFocus(ElementRef{}, p);
        }
    }
    // 5. Timeline groups: exactly the listed ones stay active, at their position.
    std::vector<bool> keep(m_timelineGroups->groups.size(), false);
    for (const DocumentState::Timeline& snap : state.timelines) {
        const int found = findTimelineGroup(snap.scope, snap.name);
        if (found < 0) {
            continue;
        }
        const size_t index = static_cast<size_t>(found);
        keep[index] = true;
        activateTimelineGroup(index);
        TimelineGroupRuntime& g = m_timelineGroups->groups[index];
        g.playing = snap.playing;
        g.reverse = snap.reverse;
        g.loop = snap.loop;
        g.hold = snap.hold;
        g.rate = snap.rate;
        g.t = snap.t < 0 ? 0 : (snap.t > g.duration ? g.duration : snap.t);
        g.startInclusive = false;
        g.lastClock = m_clock;
        applyTimelineGroupTime(index);
    }
    for (size_t i = 0; i < keep.size(); ++i) {
        if (!keep[i] && m_timelineGroups->groups[i].active) {
            releaseTimelineGroup(i);  // Started while restoring (a trigger), or not in the snapshot.
        }
    }
    m_timelineGroups->pending.clear();
    // 6. Scroll offsets, against a fresh layout so they clamp to the real content.
    std::vector<std::pair<NodeId, Vec2>> scrolls;
    if (exact) {
        m_arena.traversePreorder(m_root, [&](NodeId n) {
            if (m_arena.kind(n) == NodeKind::Element && m_arena.element(n).id != kInvalidString) {
                const ElementData& data = m_arena.element(n);
                if (data.style.base[prop::ScrollX].f != 0.0f || data.style.base[prop::ScrollY].f != 0.0f) {
                    scrolls.emplace_back(n, Vec2{0, 0});
                }
            }
            return true;
        });
    }
    for (const DocumentState::Scroll& entry : state.scrolls) {
        const NodeId node = byId(entry.id).node;
        if (node != kInvalidNode) {
            scrolls.emplace_back(node, Vec2{entry.x, entry.y});
        }
    }
    if (!scrolls.empty()) {
        layoutIfDirty();
        for (const auto& [node, pos] : scrolls) {
            m_events->setScroll(m_arena, node, pos.x, pos.y, /*notify=*/false);
        }
    }
}

std::string Document::snapshotState() {
    DocumentState state;
    collectState(state, /*withPorts=*/true);
    return serializeState(state);
}

Status Document::restoreState(std::string_view json) {
    DocumentState state;
    if (!parseState(json, state)) {
        return Status::kInvalidArgument;
    }
    applyState(state, /*exact=*/true);
    // T-399 (08 §8.18): `bind:value` covers what is left of an edit with its port's value again.
    m_bindings->reapplyAllValues();
    return Status::kOk;
}

}  // namespace xt
