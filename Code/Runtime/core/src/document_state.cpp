#include "document_state.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <memory>
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
#include "xt/charconv.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/property.h"
#include "xt/runtime_state.h"

/// Document::snapshotState / restoreState and the collect / check / apply code Context::hotReload
/// shares (T-398; Docs/design/23-host-integration.md §23.ad.1, 22-bundle.md §22.10).
///
/// Snapshot JSON (version 4; versions 1 to 3 are read too), every member optional on restore:
///   {"version":4,
///    "ports":{"<qualified name>":"<setPortText() text>", ...},
///    "states":{"<element id>":["<state>", ...], ...},
///    "inputs":{"<element id>":{"value":"...","cursor":N,"anchor":N}, ...},
///    "focus":{"<player>":"<element id>", ...},
///    "scroll":{"<element id>":[x,y], ...},
///    "timelines":[{"scope":"<id>","name":"...","t":S,"rate":R,"playing":B,"reverse":B,
///                  "loop":B,"hold":B}, ...],
///    "animations":[{"id":"<element id>","index":N,"name":"<@keyframes>","elapsed":S,"paused":B,
///                   "ended":B,"direction":"reverse"|"alternate"|"alternate-reverse"}, ...],
///    "classes":{"<element id>":{"add":["<class>", ...],"remove":["<class>", ...]}, ...},
///    "clock":{"seconds":S,"frame":N},
///    "shown":B,
///    "transitions":[{"id":"<element id>","property":"<PropId name>","from":"<value>","to":"<value>",
///                    "elapsed":S,"duration":S,"easing":"<timing function>"}, ...],
///    "pointer":{"<player>":{"x":X,"y":Y,"buttons":N,"hover":"<element id>","pressed":"<element id>",
///               "inside":B}}}
/// T-650 (23 XF-097 "快照版本 2"): `classes` lists the id'd DOCT elements whose runtime classes differ
/// from their IR classes (both sides sorted); `clock` is the document clock (%.17g, so the float comes
/// back bit for bit) and its update() count; `shown` says the initial show was done. A member left out
/// (any version 1 snapshot) restores as before: classes untouched, the clock kept, the initial show
/// as usual.
/// T-651 (23 XF-097 "快照版本 3"): `animations` are the CSS `animation` items of id'd elements
/// (DocumentState::Animation): `index` is the item's position in the element's `animation` list,
/// `elapsed` Animator::elapsedSeconds() (%.17g); `direction` is written when not `normal` and only
/// for reading. restoreState() matches them by (id, index, name) after a style pass at the restored
/// clock, quietly; items it does not list start as usual.
/// T-654 (23 XF-097 "快照版本 4"): `transitions` are the in-flight CSS `transition`s of id'd elements
/// (one per element and property; the property by its propertyName(), `from` / `to` as style text:
/// a number, a number and its unit (`12px`, `50%`, `auto`), `color(srgb r g b / a)`, or two / three
/// numbers for a vec2 / vec3 -- 9 significant digits, so a float comes back bit for bit), `elapsed`
/// the seconds since the transition's delay ended (%.17g; negative while still in the delay),
/// `easing` `linear`, `cubic-bezier(...)` or `steps(n,start|end)`, left out for the other timing
/// functions (restored from the element's own `transition`). restoreState() rebuilds them after the
/// style pass at the restored clock, quietly (no transition events); entries without their element
/// or property are ignored; transitions the document has and the snapshot does not list run on.
/// `pointer` is the pointer state machine's state, keyed by the player of the last pointer input
/// (the document has one pointer): position, held buttons (bit = PointerButton), hover and pressed
/// element ids, inside the viewport. restoreState() sets it directly, dispatching nothing.
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

/// T-616: the `@keyframes` name compiled into `timeline` ("" for none).
std::string_view keyframesName(const IrView& ir, TimelineId timeline) {
    for (const IrKeyframesRef& ref : ir.style().keyframesRefs()) {
        if (ref.timeline == timeline) {
            return ir.string(ref.name);
        }
    }
    return {};
}

void markStyleDirtySubtree(NodeArena& arena, NodeId node) {
    arena.markDirty(node, static_cast<uint8_t>(kDirtyStyle));
    arena.traversePreorder(node, [&arena](NodeId n) {
        arena.markDirty(n, static_cast<uint8_t>(kDirtyStyle));
        return true;
    });
}

std::vector<std::string> stringItems(const Json* arr) {
    std::vector<std::string> out;
    if (arr != nullptr && arr->kind == Json::Kind::Arr) {
        for (const Json& item : arr->items) {
            if (item.kind == Json::Kind::Str && !item.s.empty()) {
                out.push_back(item.s);
            }
        }
    }
    return out;
}

/// T-650: the version 2 members (`classes`, `clock`, `shown`), each only when present.
void parseVersion2(const Json& root, DocumentState& out) {
    if (const Json* classes = member(root, "classes");
        classes != nullptr && classes->kind == Json::Kind::Obj) {
        out.hasClasses = true;
        for (const auto& [id, v] : classes->fields) {
            DocumentState::Classes entry{id, stringItems(member(v, "add")), stringItems(member(v, "remove"))};
            if (!entry.add.empty() || !entry.remove.empty()) {
                out.classes.push_back(std::move(entry));
            }
        }
    }
    if (const Json* clock = member(root, "clock"); clock != nullptr && clock->kind == Json::Kind::Obj) {
        const double seconds = numberOr(member(*clock, "seconds"), -1);
        if (std::isfinite(seconds) && seconds >= 0) {
            out.hasClock = true;
            out.clockSeconds = seconds;
            const double frame = numberOr(member(*clock, "frame"), 0);
            out.clockFrame = frame > 0 && frame < 9007199254740992.0 ? static_cast<uint64_t>(frame) : 0u;
        }
    }
    if (const Json* shown = member(root, "shown"); shown != nullptr && shown->kind == Json::Kind::Bool) {
        out.shown = shown->b ? 1 : 0;
    }
}

/// T-651: the version 3 `animations` member, when present.
void parseAnimations(const Json& root, DocumentState& out) {
    const Json* animations = member(root, "animations");
    if (animations == nullptr || animations->kind != Json::Kind::Arr) {
        return;
    }
    for (const Json& v : animations->items) {
        const Json* id = member(v, "id");
        const Json* name = member(v, "name");
        const double elapsed = numberOr(member(v, "elapsed"), 0);
        if (id == nullptr || id->kind != Json::Kind::Str || name == nullptr ||
            name->kind != Json::Kind::Str || !std::isfinite(elapsed)) {
            continue;
        }
        DocumentState::Animation entry;
        entry.id = id->s;
        entry.position = offsetOr(member(v, "index"));
        entry.name = name->s;
        entry.elapsed = elapsed;
        entry.paused = boolOr(member(v, "paused"), false);
        entry.ended = boolOr(member(v, "ended"), false);
        out.animations.push_back(std::move(entry));
    }
}

void appendStringList(std::string& out, const std::vector<std::string>& items) {
    out += '[';
    for (size_t i = 0; i < items.size(); ++i) {
        out += i > 0 ? "," : "";
        appendString(out, items[i]);
    }
    out += ']';
}

// -- T-654 (23 XF-097 version 4): in-flight CSS transitions and the pointer state ------------------

constexpr struct {
    const char* name;
    LengthUnit unit;
} kLengthUnits[] = {
    {"px", LengthUnit::Px},
    {"%", LengthUnit::Percent},
    {"em", LengthUnit::Em},
    {"rem", LengthUnit::Rem},
    {"vw", LengthUnit::Vw},
    {"vh", LengthUnit::Vh},
    {"vmin", LengthUnit::Vmin},
    {"vmax", LengthUnit::Vmax},
    {"safe-top", LengthUnit::SafeTop},
    {"safe-right", LengthUnit::SafeRight},
    {"safe-bottom", LengthUnit::SafeBottom},
    {"safe-left", LengthUnit::SafeLeft},
    {"uiscale", LengthUnit::UiScale},
};

/// `v` with `precision` significant digits in the C locale (9: a float comes back bit for bit).
void appendDigits(std::string& out, double v, int precision) {
    char buf[40];
    const size_t n = formatFloat(buf, sizeof(buf), std::isfinite(v) ? v : 0.0, precision);
    out.append(buf, n);
}

/// A transition end as style text; false for a value that has none (a path, stops, a calc()).
bool valueText(const Value& v, std::string& out) {
    out.clear();
    switch (v.type) {
        case ValueType::Float:
            appendDigits(out, v.f, 9);
            return true;
        case ValueType::Length:
            if (v.len.unit == LengthUnit::Auto) {
                out = "auto";
                return true;
            }
            for (const auto& u : kLengthUnits) {
                if (u.unit == v.len.unit) {
                    appendDigits(out, v.len.value, 9);
                    out += u.name;
                    return true;
                }
            }
            return false;
        case ValueType::Color:
            out = "color(srgb ";
            appendDigits(out, v.c.r, 9);
            out += ' ';
            appendDigits(out, v.c.g, 9);
            out += ' ';
            appendDigits(out, v.c.b, 9);
            out += " / ";
            appendDigits(out, v.c.a, 9);
            out += ')';
            return true;
        case ValueType::Vec2:
            appendDigits(out, v.v2.x, 9);
            out += ' ';
            appendDigits(out, v.v2.y, 9);
            return true;
        case ValueType::Vec3:
            appendDigits(out, v.v3.x, 9);
            out += ' ';
            appendDigits(out, v.v3.y, 9);
            out += ' ';
            appendDigits(out, v.v3.z, 9);
            return true;
        default:
            return false;
    }
}

/// The numbers of `text` separated by single spaces (at most `max`); false on anything else.
bool numbersOf(std::string_view text, float* out, size_t max, size_t& count) {
    count = 0;
    const char* p = text.data();
    const char* end = p + text.size();
    while (p < end) {
        if (count == max) {
            return false;
        }
        const std::from_chars_result r = fromChars(p, end, out[count]);
        if (r.ec != std::errc() || !std::isfinite(out[count])) {
            return false;
        }
        ++count;
        p = r.ptr;
        if (p < end) {
            if (*p != ' ' || p + 1 == end) {
                return false;
            }
            ++p;
        }
    }
    return count > 0;
}

/// valueText()'s inverse.
bool parseValueText(std::string_view text, Value& out) {
    if (text == "auto") {
        out = Value::ofLength(0, LengthUnit::Auto);
        return true;
    }
    constexpr std::string_view kColor = "color(srgb ";
    if (text.size() > kColor.size() && text.substr(0, kColor.size()) == kColor && text.back() == ')') {
        const std::string_view inner = text.substr(kColor.size(), text.size() - kColor.size() - 1);
        const size_t slash = inner.find(" / ");
        float rgb[3] = {};
        float alpha[1] = {};
        size_t n = 0;
        size_t na = 0;
        if (slash == std::string_view::npos || !numbersOf(inner.substr(0, slash), rgb, 3, n) || n != 3 ||
            !numbersOf(inner.substr(slash + 3), alpha, 1, na)) {
            return false;
        }
        out = Value::ofColor(Color{rgb[0], rgb[1], rgb[2], alpha[0]});
        return true;
    }
    if (text.find(' ') != std::string_view::npos) {
        float v[3] = {};
        size_t n = 0;
        if (!numbersOf(text, v, 3, n) || n < 2) {
            return false;
        }
        out = n == 2 ? Value::ofVec2(Vec2{v[0], v[1]}) : Value::ofVec3(Vec3{v[0], v[1], v[2]});
        return true;
    }
    float f = 0;
    const std::from_chars_result r = fromChars(text.data(), text.data() + text.size(), f);
    if (r.ec != std::errc() || !std::isfinite(f)) {
        return false;
    }
    const std::string_view unit(r.ptr, static_cast<size_t>(text.data() + text.size() - r.ptr));
    if (unit.empty()) {
        out = Value::ofFloat(f);
        return true;
    }
    for (const auto& u : kLengthUnits) {
        if (unit == u.name) {
            out = Value::ofLength(f, u.unit);
            return true;
        }
    }
    return false;
}

/// `linear`, `cubic-bezier(x1,y1,x2,y2)` or `steps(n,start|end)`; "" for a timing function the
/// restore takes from the element's own `transition` (hold, `linear()` points).
std::string easingText(const Easing& e) {
    std::string out;
    if (e.kind == EasingKind::Linear) {
        out = "linear";
    } else if (e.kind == EasingKind::CubicBezier) {
        out = "cubic-bezier(";
        const float points[4] = {e.x1, e.y1, e.x2, e.y2};
        for (int i = 0; i < 4; ++i) {
            out += i > 0 ? "," : "";
            appendDigits(out, points[i], 9);
        }
        out += ')';
    } else if (e.kind == EasingKind::Steps) {
        out = "steps(" + std::to_string(e.stepCount) + (e.stepsJumpEnd != 0 ? ",end)" : ",start)");
    }
    return out;
}

/// The arguments of `fn(...)` when `text` is that call.
bool callArgs(std::string_view text, std::string_view fn, std::string_view& args) {
    if (text.size() <= fn.size() + 1 || text.substr(0, fn.size()) != fn || text.back() != ')') {
        return false;
    }
    args = text.substr(fn.size(), text.size() - fn.size() - 1);
    return true;
}

/// easingText()'s inverse.
bool parseEasingText(std::string_view text, Easing& out) {
    if (text == "linear") {
        out = Easing::linear();
        return true;
    }
    std::string_view args;
    if (callArgs(text, "cubic-bezier(", args)) {
        float v[4] = {};
        for (int i = 0; i < 4; ++i) {
            const std::from_chars_result r = fromChars(args.data(), args.data() + args.size(), v[i]);
            if (r.ec != std::errc() || !std::isfinite(v[i])) {
                return false;
            }
            args.remove_prefix(static_cast<size_t>(r.ptr - args.data()));
            if (i < 3) {
                if (args.empty() || args.front() != ',') {
                    return false;
                }
                args.remove_prefix(1);
            }
        }
        if (!args.empty()) {
            return false;
        }
        out = Easing::bezier(v[0], v[1], v[2], v[3]);
        return true;
    }
    if (callArgs(text, "steps(", args)) {
        const size_t comma = args.find(',');
        if (comma == std::string_view::npos) {
            return false;
        }
        const std::string_view count = args.substr(0, comma);
        const std::string_view jump = args.substr(comma + 1);
        uint32_t n = 0;
        const std::from_chars_result r = std::from_chars(count.data(), count.data() + count.size(), n);
        if (r.ec != std::errc() || r.ptr != count.data() + count.size() || n < 1 || n > 0xFFFF ||
            (jump != "end" && jump != "start")) {
            return false;
        }
        out = Easing::steps(static_cast<uint16_t>(n), jump == "end");
        return true;
    }
    return false;
}

/// T-654: the version 4 members (`transitions`, `pointer`), each only when present.
void parseVersion4(const Json& root, DocumentState& out) {
    if (const Json* transitions = member(root, "transitions");
        transitions != nullptr && transitions->kind == Json::Kind::Arr) {
        for (const Json& v : transitions->items) {
            const Json* id = member(v, "id");
            const Json* property = member(v, "property");
            const Json* from = member(v, "from");
            const Json* to = member(v, "to");
            const Json* easing = member(v, "easing");
            const double elapsed = numberOr(member(v, "elapsed"), 0);
            const double duration = numberOr(member(v, "duration"), 0);
            if (id == nullptr || id->kind != Json::Kind::Str || property == nullptr ||
                property->kind != Json::Kind::Str || from == nullptr || from->kind != Json::Kind::Str ||
                to == nullptr || to->kind != Json::Kind::Str || !std::isfinite(elapsed) ||
                !std::isfinite(duration) || duration <= 0) {
                continue;
            }
            out.transitions.push_back(DocumentState::Transition{
                id->s, property->s, from->s, to->s, elapsed, static_cast<float>(duration),
                easing != nullptr && easing->kind == Json::Kind::Str ? easing->s : std::string()});
        }
    }
    const Json* pointer = member(root, "pointer");
    if (pointer == nullptr || pointer->kind != Json::Kind::Obj) {
        return;
    }
    for (const auto& [player, v] : pointer->fields) {
        if (player.size() != 1 || player[0] < '0' || player[0] >= static_cast<char>('0' + kMaxPlayers) ||
            v.kind != Json::Kind::Obj) {
            continue;
        }
        const Json* hover = member(v, "hover");
        const Json* pressed = member(v, "pressed");
        const double buttons = numberOr(member(v, "buttons"), 0);
        DocumentState::Pointer entry;
        entry.player = static_cast<uint8_t>(player[0] - '0');
        entry.x = static_cast<float>(numberOr(member(v, "x"), 0));
        entry.y = static_cast<float>(numberOr(member(v, "y"), 0));
        entry.buttons = buttons > 0 && buttons < 256 ? static_cast<uint8_t>(buttons) : uint8_t{0};
        entry.hover = hover != nullptr && hover->kind == Json::Kind::Str ? hover->s : std::string();
        entry.pressed = pressed != nullptr && pressed->kind == Json::Kind::Str ? pressed->s : std::string();
        entry.inside = boolOr(member(v, "inside"), false);
        out.hasPointer = true;
        out.pointer = std::move(entry);  // One pointer: the last entry wins.
    }
}

void appendVersion4(std::string& out, const DocumentState& state) {
    out += ",\"transitions\":[";
    for (size_t i = 0; i < state.transitions.size(); ++i) {
        const DocumentState::Transition& t = state.transitions[i];
        out += i > 0 ? ",{\"id\":" : "{\"id\":";
        appendString(out, t.id);
        out += ",\"property\":";
        appendString(out, t.property);
        out += ",\"from\":";
        appendString(out, t.from);
        out += ",\"to\":";
        appendString(out, t.to);
        out += ",\"elapsed\":";
        appendDigits(out, t.elapsed, 17);
        out += ",\"duration\":";
        appendDigits(out, t.duration, 9);
        if (!t.easing.empty()) {
            out += ",\"easing\":";
            appendString(out, t.easing);
        }
        out += '}';
    }
    out += ']';
    if (!state.hasPointer) {
        return;
    }
    const DocumentState::Pointer& p = state.pointer;
    out += ",\"pointer\":{\"" + std::to_string(p.player) + "\":{\"x\":";
    appendDigits(out, p.x, 9);
    out += ",\"y\":";
    appendDigits(out, p.y, 9);
    out += ",\"buttons\":" + std::to_string(p.buttons) + ",\"hover\":";
    appendString(out, p.hover);
    out += ",\"pressed\":";
    appendString(out, p.pressed);
    out += std::string(",\"inside\":") + (p.inside ? "true" : "false") + "}}";
}

/// The `transition` entry of `style` that covers `prop` (the first one, as StyleEngine picks), or
/// null.
const IrTransitionSpec* transitionSpecFor(const IrView& ir, const ComputedStyle& style, PropId prop) {
    const std::span<const IrTransitionSpec> all = ir.style().transitions();
    if (uint64_t{style.transitionOffset} + style.transitionCount > all.size()) {
        return nullptr;
    }
    for (const IrTransitionSpec& spec : all.subspan(style.transitionOffset, style.transitionCount)) {
        if (spec.prop == prop || spec.prop == 0xFFFFu) {
            return &spec;
        }
    }
    return nullptr;
}

/// T-654: the in-flight transitions of id'd elements on their own properties (not a scene node's,
/// not an effect parameter's), sorted by (id, property) so equal states write equal text.
void collectTransitions(const Document& doc, const NodeArena& arena, const Animator& animator, float clock,
                        std::vector<DocumentState::Transition>& out) {
    for (const Transition& tr : animator.transitions()) {
        if (tr.written() != tr.node || !arena.alive(tr.node) || arena.kind(tr.node) != NodeKind::Element ||
            arena.element(tr.node).id == kInvalidString || isEffectProp(tr.prop)) {
            continue;
        }
        DocumentState::Transition entry;
        entry.property = propertyName(tr.prop);
        if (propertyByName(entry.property) != tr.prop || !valueText(tr.from, entry.from) ||
            !valueText(tr.to, entry.to)) {
            continue;
        }
        entry.id = std::string(doc.string(arena.element(tr.node).id));
        entry.elapsed =
            static_cast<double>(clock) - static_cast<double>(tr.startClock) - static_cast<double>(tr.delay);
        entry.duration = tr.duration;
        entry.easing = easingText(tr.easing);
        out.push_back(std::move(entry));
    }
    std::sort(out.begin(), out.end(),
              [](const DocumentState::Transition& a, const DocumentState::Transition& b) {
                  return a.id != b.id ? a.id < b.id : a.property < b.property;
              });
}

/// True when `v` can be written to an element's `prop` (setProperty()'s rule).
bool fitsProperty(PropId prop, const Value& v) {
    if (v.type == propertyType(prop)) {
        return true;
    }
    return (prop == prop::TranslateX || prop == prop::TranslateY) &&
           (v.type == ValueType::Length || v.type == ValueType::Vec2);
}

/// T-654: rebuilds the snapshot's transitions on the restored clock, after the style pass: each one
/// gets the delay (and, when the snapshot has none, the easing) of the element's own `transition`
/// for the property, and the start that puts it `elapsed` seconds past that delay at `clock`.
void applyTransitions(const Document& doc, const NodeArena& arena, const IrView& ir, Animator& animator,
                      float clock, const std::vector<DocumentState::Transition>& transitions) {
    for (const DocumentState::Transition& snap : transitions) {
        const NodeId node = doc.byId(snap.id).node;
        const PropId prop = propertyByName(snap.property);
        if (node == kInvalidNode || prop == prop::kInvalidProp || isEffectProp(prop) ||
            !propertyApplies(NodeKind::Element, prop)) {
            continue;
        }
        Transition tr;
        tr.node = node;
        tr.target = node;
        tr.prop = prop;
        if (!parseValueText(snap.from, tr.from) || !parseValueText(snap.to, tr.to) ||
            tr.from.type != tr.to.type || !fitsProperty(prop, tr.from)) {
            continue;
        }
        const IrTransitionSpec* spec = transitionSpecFor(ir, arena.element(node).style, prop);
        tr.delay = spec != nullptr ? spec->delay : 0.0f;
        tr.duration = snap.duration;
        if (!parseEasingText(snap.easing, tr.easing)) {
            tr.easing = spec != nullptr ? easingFromIr(spec->easing) : Easing::linear();
        }
        tr.startClock =
            static_cast<float>(static_cast<double>(clock) - snap.elapsed - static_cast<double>(tr.delay));
        animator.addTransition(tr);  // Replaces one the style pass started on the same property.
    }
}

/// The DocumentState a snapshot JSON describes; false when it is not one.
bool parseState(std::string_view text, DocumentState& out) {
    Json root;
    if (!JsonReader(text).parse(root) || root.kind != Json::Kind::Obj) {
        return false;
    }
    const double version = numberOr(member(root, "version"), 0);
    if (version != 1 && version != 2 && version != 3 && version != 4) {
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
    parseVersion2(root, out);
    parseAnimations(root, out);
    parseVersion4(root, out);
    return true;
}

std::string serializeState(const DocumentState& state) {
    std::string out = "{\"version\":4,\"ports\":{";
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
    out += "],\"animations\":[";
    for (size_t i = 0; i < state.animations.size(); ++i) {
        static constexpr const char* kDirections[] = {"", "reverse", "alternate", "alternate-reverse"};
        const DocumentState::Animation& a = state.animations[i];
        out += i > 0 ? ",{\"id\":" : "{\"id\":";
        appendString(out, a.id);
        out += ",\"index\":" + std::to_string(a.position) + ",\"name\":";
        appendString(out, a.name);
        char elapsed[48];
        std::snprintf(elapsed, sizeof(elapsed), ",\"elapsed\":%.17g",
                      std::isfinite(a.elapsed) ? a.elapsed : 0.0);
        out += elapsed;
        out += std::string(",\"paused\":") + (a.paused ? "true" : "false");
        out += std::string(",\"ended\":") + (a.ended ? "true" : "false");
        if (a.direction > 0 && a.direction < 4) {
            out += std::string(",\"direction\":\"") + kDirections[a.direction] + "\"";
        }
        out += '}';
    }
    out += "],\"classes\":{";
    for (size_t i = 0; i < state.classes.size(); ++i) {
        out += i > 0 ? "," : "";
        appendString(out, state.classes[i].id);
        out += ":{\"add\":";
        appendStringList(out, state.classes[i].add);
        out += ",\"remove\":";
        appendStringList(out, state.classes[i].remove);
        out += '}';
    }
    char clock[80];
    std::snprintf(clock, sizeof(clock), "},\"clock\":{\"seconds\":%.17g,\"frame\":%llu},\"shown\":",
                  std::isfinite(state.clockSeconds) ? state.clockSeconds : 0.0,
                  static_cast<unsigned long long>(state.clockFrame));
    out += clock;
    out += state.shown == 1 ? "true" : "false";
    appendVersion4(out, state);
    out += '}';
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
            // T-616: the CSS `animation` items, by position and `@keyframes` name.
            const std::span<const IrAnimationSpec> specs =
                m_styleEngine.animationSpecs(data.style.lastAnimationOffset, data.style.lastAnimationCount);
            for (size_t i = 0; i < specs.size() && specs.size() == data.cssAnimations.size(); ++i) {
                const uint32_t anim = data.cssAnimations[i];
                if (anim == 0) {
                    continue;
                }
                DocumentState::Animation entry;
                entry.id = id;
                entry.position = static_cast<uint32_t>(i);
                entry.name = std::string(keyframesName(m_ir, specs[i].timeline));
                entry.ended = !m_animator.isActive(anim) && !m_animator.isFinished(anim);
                entry.elapsed = m_animator.elapsedSeconds(anim, m_clock);
                entry.paused = m_animator.isPaused(anim);
                entry.direction = specs[i].direction;
                out.animations.push_back(std::move(entry));
            }
        }
    }
    for (uint8_t p = 0; p < kMaxPlayers; ++p) {
        out.focus[p] = idOf(focus(p).node);
    }
    collectClasses(out);
    out.hasClock = true;
    out.clockSeconds = m_clock;
    out.clockFrame = m_frame;
    out.shown = m_initialShown ? 1 : 0;
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
    // T-654 (23 XF-097 version 4): in-flight CSS transitions and the pointer state.
    collectTransitions(*this, m_arena, m_animator, m_clock, out.transitions);
    const EventDispatcher::PointerState pointer = m_events->pointerState();
    out.hasPointer = true;
    out.pointer.player = pointer.player;
    out.pointer.x = pointer.pos.x;
    out.pointer.y = pointer.pos.y;
    out.pointer.buttons = pointer.buttons;
    out.pointer.hover = idOf(pointer.hover);
    out.pointer.pressed = idOf(pointer.pressed);
    out.pointer.inside = pointer.inside;
}

void Document::collectClasses(DocumentState& out) const {
    // T-650: each id'd DOCT element's runtime classes against its IR element's, by text.
    const std::span<const IrDocElement> elements = m_ir.doc().elements();
    const std::span<const uint32_t> classPool = m_ir.doc().classPool();
    out.hasClasses = true;
    for (size_t i = 0; i < m_docElementNodes.size() && i < elements.size(); ++i) {
        const NodeId n = m_docElementNodes[i];
        if (n == kInvalidNode || !m_arena.alive(n) || m_arena.element(n).id == kInvalidString) {
            continue;
        }
        std::vector<std::string_view> irClasses;
        for (uint32_t k = 0; k < elements[i].classCount; ++k) {
            irClasses.push_back(m_ir.string(classPool[elements[i].classOffset + k]));
        }
        std::vector<std::string_view> runtime;
        for (const StringId sid : m_arena.element(n).classes) {
            runtime.push_back(string(sid));
        }
        std::sort(irClasses.begin(), irClasses.end());
        std::sort(runtime.begin(), runtime.end());
        DocumentState::Classes entry;
        entry.id = std::string(string(m_arena.element(n).id));
        std::vector<std::string_view> added;
        std::vector<std::string_view> removed;
        std::set_difference(runtime.begin(), runtime.end(), irClasses.begin(), irClasses.end(),
                            std::back_inserter(added));
        std::set_difference(irClasses.begin(), irClasses.end(), runtime.begin(), runtime.end(),
                            std::back_inserter(removed));
        entry.add.assign(added.begin(), added.end());
        entry.remove.assign(removed.begin(), removed.end());
        if (!entry.add.empty() || !entry.remove.empty()) {
            out.classes.push_back(std::move(entry));
        }
    }
}

void Document::applyClasses(const DocumentState& state) {
    // T-650: the snapshot is authoritative: a listed element gets its IR classes plus `add` minus
    // `remove`, an unlisted id'd DOCT element its IR classes. A change restyles the subtree (no
    // trigger reacts to a class).
    const std::span<const IrDocElement> elements = m_ir.doc().elements();
    const std::span<const uint32_t> classPool = m_ir.doc().classPool();
    for (size_t i = 0; i < m_docElementNodes.size() && i < elements.size(); ++i) {
        const NodeId n = m_docElementNodes[i];
        if (n == kInvalidNode || !m_arena.alive(n) || m_arena.element(n).id == kInvalidString) {
            continue;
        }
        const std::string_view id = string(m_arena.element(n).id);
        std::vector<StringId> want(classPool.begin() + elements[i].classOffset,
                                   classPool.begin() + elements[i].classOffset + elements[i].classCount);
        for (const DocumentState::Classes& entry : state.classes) {
            if (entry.id != id) {
                continue;
            }
            for (const std::string& name : entry.add) {
                want.push_back(intern(name));
            }
            for (const std::string& name : entry.remove) {
                want.erase(std::remove_if(want.begin(), want.end(),
                                          [&](StringId sid) { return string(sid) == name; }),
                           want.end());
            }
        }
        std::sort(want.begin(), want.end());
        want.erase(std::unique(want.begin(), want.end()), want.end());
        if (m_arena.element(n).classes != want) {
            m_arena.element(n).classes = std::move(want);
            markStyleDirtySubtree(m_arena, n);
        }
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
    // 2b. T-650: runtime classes (restoreState() only; a hot reload keeps the new IR's).
    if (exact && state.hasClasses) {
        applyClasses(state);
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
    // 4b. T-654: the pointer state, set directly (restoreState() only): no PointerEnter / Leave, a
    // press goes on to its Click. Before the style pass below, so `:hover` / `:active` styles are
    // part of it rather than a change that starts transitions.
    if (exact && state.hasPointer) {
        EventDispatcher::PointerState pointer;
        pointer.player = state.pointer.player;
        pointer.pos = Vec2{state.pointer.x, state.pointer.y};
        pointer.buttons = state.pointer.buttons;
        pointer.hover = state.pointer.hover.empty() ? kInvalidNode : byId(state.pointer.hover).node;
        pointer.pressed = state.pointer.pressed.empty() ? kInvalidNode : byId(state.pointer.pressed).node;
        pointer.inside = state.pointer.inside;
        m_events->setTime(m_clock);  // The press time of a restored press.
        m_events->restorePointerState(m_arena, pointer);
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
    // 5b. T-616 (04 §4.13): CSS `animation` items, by element id + position + `@keyframes` name,
    // after a style pass at this clock has played the new document's own (the custom states above
    // may have changed which `animation` declaration wins).
    // T-654: the in-flight CSS transitions (restoreState() only) at the same spot.
    const bool transitions = exact && !state.transitions.empty();
    if (!state.animations.empty() || transitions) {
        layoutIfDirty();
    }
    if (transitions) {
        applyTransitions(*this, m_arena, m_ir, m_animator, m_clock, state.transitions);
    }
    for (const DocumentState::Animation& snap : state.animations) {
        const NodeId node = byId(snap.id).node;
        if (node == kInvalidNode) {
            continue;
        }
        const ElementData& data = m_arena.element(node);
        const std::span<const IrAnimationSpec> specs =
            m_styleEngine.animationSpecs(data.style.lastAnimationOffset, data.style.lastAnimationCount);
        if (snap.position >= specs.size() || specs.size() != data.cssAnimations.size() ||
            keyframesName(m_ir, specs[snap.position].timeline) != snap.name) {
            continue;
        }
        const uint32_t anim = data.cssAnimations[snap.position];
        if (snap.ended) {
            m_animator.stop(anim);  // Quietly: it ended before.
            continue;
        }
        if (snap.paused && !m_animator.isPaused(anim)) {
            m_animator.pause(anim, m_clock);  // First, so the saved position lands in pausedAt exactly.
        }
        m_animator.setElapsedSeconds(anim, snap.elapsed, m_clock);
    }
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

RuntimeState::RuntimeState() = default;
RuntimeState::~RuntimeState() = default;
RuntimeState::RuntimeState(RuntimeState&& other) noexcept = default;
RuntimeState& RuntimeState::operator=(RuntimeState&& other) noexcept = default;

RuntimeState Document::captureRuntimeState() {
    RuntimeState out;
    out.m_state = std::make_unique<DocumentState>();
    collectState(*out.m_state, /*withPorts=*/false);
    out.m_clock = m_clock;
    return out;
}

void Document::restoreRuntimeState(const RuntimeState& state) {
    if (state.m_state == nullptr) {
        return;
    }
    // Context::hotReload's carry-over (hot_reload.cpp), without its all-or-nothing check: what this
    // document lacks is skipped and keeps its initial state.
    m_clock = state.m_clock;
    applyState(*state.m_state, /*exact=*/false);
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
    // This document's handle in its Context (0: none) and whether it is the Context's only one.
    DocumentHandle handle = 0;
    bool alone = true;
    for (size_t i = 0; m_context != nullptr && i < m_context->m_documents.size(); ++i) {
        const Document* other = m_context->liveAt(i);
        handle = other == this ? static_cast<DocumentHandle>(i + 1) : handle;
        alone = alone && (other == nullptr || other == this);
    }
    // T-650 (23 XF-097 version 2): the clock first, so the timeline groups and CSS animations below
    // are set against it. It is the Context's clock while this is the Context's only document (a
    // C ABI surface): the frames that follow run on the same clock values as the saved run's.
    if (state.hasClock) {
        m_clock = static_cast<float>(state.clockSeconds);
        m_frame = state.clockFrame;
        if (m_context != nullptr && alone) {
            m_context->m_clock = m_clock;
        }
    }
    // First rule: restored before the first update() with the initial show done, the document counts
    // as shown -- no lifecycle.shown / OnShown, no `show` triggers (their intro) and no autofocus;
    // the timeline groups are the snapshot's alone.
    if (state.shown == 1 && !m_shownOnce) {
        m_shownOnce = true;
        m_initialShown = true;
    }
    applyState(state, /*exact=*/true);
    // T-654: the Context routes the restored pointer here: the hovered surface (no PointerLeave from
    // a move inside it) and, for a press, the capture its PointerUp goes to.
    if (state.hasPointer && handle != 0) {
        if (state.pointer.inside && (m_context->m_hoverSurface == 0 || alone)) {
            m_context->m_hoverSurface = handle;
        } else if (!state.pointer.inside && m_context->m_hoverSurface == handle) {
            m_context->m_hoverSurface = 0;
        }
        const bool pressed = m_events->activeNode() != kInvalidNode;
        if (pressed && (m_context->m_pointerCapture == 0 || alone)) {
            m_context->m_pointerCapture = handle;
        } else if (!pressed && m_context->m_pointerCapture == handle) {
            m_context->m_pointerCapture = 0;
        }
    }
    // T-399 (08 §8.18): `bind:value` covers what is left of an edit with its port's value again.
    m_bindings->reapplyAllValues();
    return Status::kOk;
}

}  // namespace xt
