#include "binding_runtime.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>

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

/// docs/design/27-interaction.md §27.3, 03 §3.12.3, 04 §4.10.4, 13 §13.8.1, docs/tasks/T-175.md.
///
/// Not covered by the design/task sheet (conservative choices made here):
///   - Ports start at their declared default (T-358, PORT `defaults[]`), else at 0 / "" / false /
///     transparent / the first enum value / empty. Every binding
///     is evaluated once at load (so bound text shows its format with the defaults); a `tween` or
///     `delay` step passes its first value straight through.
///   - Comparisons give a bool (1/0). `eq`/`ne` compare as text when either side is text (enum
///     ports read as their value name), else as numbers. Text reads as a number via strtod (0 when
///     it is none); truthiness: non-zero, non-empty text other than "false"/"0", a non-empty list.
///   - Numbers print in the shortest fixed form (up to 6 decimals); `thousands` rounds to an
///     integer; `percent` multiplies by 100; `time` floors to whole seconds, `mm` keeps counting
///     past 59 when the pattern has no hours.
///   - `range()` clamps; `%` outputs give a percent length. `N%` in a comparison / range() input
///     is N/100 of the input port's `<port>_max` companion (N/100 when there is none).
///   - `select` / `step` without a match (or below the first threshold) give `default:`, else
///     (select) nothing, (step) the first value. A binding whose chain gives nothing is not
///     applied.
///   - `fromToken(name)` gives `var(--name)` (`{0}` in the name = the input as text), which a
///     property target resolves through the theme tokens (T-170) -- so it follows setTheme().
///   - `localize` resolves its input (a key, a leading `@` dropped) through the Document's
///     localization provider and language (T-179), showing the key when it is untranslated, and
///     follows setLanguage(); `glyph` passes the action name through as text (T-178's provider
///     resolves images for `<glyph>` inline objects, not text); `index` is 0 until `<repeat>`
///     (T-177); `item(field)` reads a Struct field.
///   - A final `tween` on an animatable Float/Length/Color property is an Animator transition
///     (04 §4.6: from the current final value to the new one); a `tween` elsewhere in the chain
///     (a counter feeding `thousands`) interpolates inside the binding with the same easing. A
///     change during a tween restarts it from the current interpolated value.
///   - `delay(t)` keeps every change and releases each t seconds later (a delay line).
///   - Property targets: a typed value becomes the element's runtime declaration (as setStyle's);
///     text (keywords, `var()`) goes through ElementRef::setStyle. `visible` is `visibility`;
///     `src` looks the text up among the IR's image assets (unknown: a warning, unchanged).
///   - A host converter the host has not registered passes its input through (one warning).
///   - An `emit(<call port>)` with a host handler calls it with the port's value and stores the
///     result (a Changed notification) instead of sending a HostEvent.
///   - Binding evaluation nested more than 16 deep (a binding whose target fires a trigger that
///     sets a port ...) is dropped.
///   - T-177: setText() reads a struct port from a JSON object and a list port from a JSON array
///     of objects (fields by name, unknown ones ignored; strings, numbers, true/false, a color as
///     CSS color text, a vec2/vec3 as a number array).
///   - T-391 (27 §27.3.4): a struct field may be a list<T>; its value is a List Value over storage
///     the port owns (Slot::entryTrees / fieldTrees, immutable, replaced as a whole on a change).
///     A nested repeat over `item(<field>)` reads a nested list slot holding a copy of the field
///     (lookup() name `<enclosing repeat id>/<i>.<field>`); the list API on it writes the list back
///     into the enclosing entry (and on up), which then notifies like listUpdate() of that entry.
///   - T-177: a `<repeat>` template's bindings are never evaluated; each instance evaluates its
///     own copies (RepeatController). `index` / `count` read the instance's entry index and the
///     list's entry count (outside an instance: 0, and a list input's count); a list change
///     re-evaluates the instances' copies through the controller, not through the list port.

namespace xt {

using namespace binding_runtime_detail;  // T-412: helpers shared between the split files

namespace {

using minijson::JsonField;

}  // namespace

namespace binding_runtime_detail {

/// A binding target numbered as a compiler StyleProp (160-207, below the shape-modifier /
/// parametric-shape PropIds 208+, T-225b; and from 256 on, below the effect PropIds, T-331)
/// rather than an animatable PropId.
bool isStylePropId(uint16_t id) {
    return (id >= 160 && id < prop::ModTrimStart) ||
           (id > prop::ConstraintEnabled && id < prop::kEffectPropBase);
}

void markStyleDirtySubtree(NodeArena& arena, NodeId node) {
    arena.markDirty(node, static_cast<uint8_t>(kDirtyStyle));
    arena.traversePreorder(node, [&arena](NodeId n) {
        arena.markDirty(n, static_cast<uint8_t>(kDirtyStyle));
        return true;
    });
}

BVal num(double v, bool isBool) {
    BVal b;
    b.kind = Kind::Num;
    b.num = v;
    b.isBool = isBool;
    return b;
}

BVal str(std::string s) {
    BVal b;
    b.kind = Kind::Str;
    b.str = std::move(s);
    return b;
}

BVal ofValue(Kind kind, const Value& v) {
    BVal b;
    b.kind = kind;
    b.v = v;
    return b;
}

/// Shortest fixed form, up to 6 decimals.
std::string numText(double v) {
    if (!std::isfinite(v)) {
        return "0";
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", v);
    std::string s(buf);
    size_t dot = s.find('.');
    if (dot != std::string::npos) {
        while (!s.empty() && s.back() == '0') {
            s.pop_back();
        }
        if (!s.empty() && s.back() == '.') {
            s.pop_back();
        }
    }
    if (s == "-0") {
        s = "0";
    }
    return s;
}

}  // namespace binding_runtime_detail

namespace {

const char* unitText(LengthUnit u) {
    switch (u) {
        case LengthUnit::Px:
            return "px";
        case LengthUnit::Percent:
            return "%";
        case LengthUnit::Em:
            return "em";
        case LengthUnit::Rem:  // T-335.
            return "rem";
        case LengthUnit::Vw:
            return "vw";
        case LengthUnit::Vh:
            return "vh";
        case LengthUnit::Vmin:
            return "vmin";
        case LengthUnit::Vmax:
            return "vmax";
        case LengthUnit::UiScale:
            return "uiscale";
        default:
            return "px";
    }
}

std::string colorText(const Color& c) {
    auto byte = [](float f) { return static_cast<int>(std::lround(std::clamp(f, 0.0f, 1.0f) * 255.0f)); };
    char buf[16];
    if (byte(c.a) == 255) {
        std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", byte(c.r), byte(c.g), byte(c.b));
    } else {
        std::snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", byte(c.r), byte(c.g), byte(c.b), byte(c.a));
    }
    return buf;
}

}  // namespace

namespace binding_runtime_detail {

std::string toStr(const BVal& b) {
    switch (b.kind) {
        case Kind::Num:
            return b.isBool ? (b.num != 0 ? "true" : "false") : numText(b.num);
        case Kind::Str:
            return b.str;
        case Kind::Color:
            return colorText(b.v.c);
        case Kind::Len:
            if (b.v.len.unit == LengthUnit::Auto) {
                return "auto";
            }
            return numText(b.v.len.value) + unitText(b.v.len.unit);
        case Kind::Vec2:
            return numText(b.v.v2.x) + "," + numText(b.v.v2.y);
        case Kind::Vec3:
            return numText(b.v.v3.x) + "," + numText(b.v.v3.y) + "," + numText(b.v.v3.z);
        default:
            return std::string();
    }
}

double toNum(const BVal& b) {
    switch (b.kind) {
        case Kind::Num:
            return b.num;
        case Kind::Str: {
            const char* s = b.str.c_str();
            char* end = nullptr;
            double v = std::strtod(s, &end);
            return end == s ? 0.0 : v;
        }
        case Kind::Len:
            return b.v.len.value;
        case Kind::List:
            return b.v.itemCount;
        default:
            return 0.0;
    }
}

bool truthy(const BVal& b) {
    switch (b.kind) {
        case Kind::Num:
            return b.num != 0;
        case Kind::Str:
            return !b.str.empty() && b.str != "false" && b.str != "0";
        case Kind::Len:
            return b.v.len.value != 0;
        case Kind::Color:
            return b.v.c.a > 0;
        case Kind::Struct:
            return true;
        case Kind::List:
            return b.v.itemCount > 0;
        default:
            return false;
    }
}

bool same(const BVal& a, const BVal& b) {
    if (a.kind != b.kind) {
        return false;
    }
    switch (a.kind) {
        case Kind::None:
            return true;
        case Kind::Num:
            return a.num == b.num && a.isBool == b.isBool;
        case Kind::Str:
            return a.str == b.str;
        case Kind::Color:
        case Kind::Len:
        case Kind::Vec2:
        case Kind::Vec3:
            return valueEquals(a.v, b.v);
        default:
            return false;  // Struct / List point into mutable storage: always "changed".
    }
}

/// Interpolation for tween: numbers, lengths of one unit, colors; anything else jumps.
BVal lerpB(const BVal& a, const BVal& b, float t) {
    if (a.kind == Kind::Num && b.kind == Kind::Num && !a.isBool && !b.isBool) {
        return num(a.num + (b.num - a.num) * t);
    }
    if ((a.kind == Kind::Len && b.kind == Kind::Len && a.v.len.unit == b.v.len.unit) ||
        (a.kind == Kind::Color && b.kind == Kind::Color)) {
        return ofValue(a.kind, lerpValue(a.v, b.v, t, nullptr, nullptr));
    }
    return b;
}

/// `time()`'s pattern: runs of h / m / s; hours only when the pattern has them.
std::string timeText(double seconds, const std::string& pattern) {
    long long total = toInt64Sat(std::floor(std::max(0.0, seconds)));
    const bool hasHours = pattern.find('h') != std::string::npos;
    const bool hasMinutes = pattern.find('m') != std::string::npos;
    long long h = hasHours ? total / 3600 : 0;
    long long rest = hasHours ? total % 3600 : total;
    long long m = hasMinutes ? rest / 60 : 0;
    long long s = hasMinutes ? rest % 60 : rest;
    std::string out;
    for (size_t i = 0; i < pattern.size();) {
        char c = pattern[i];
        size_t run = 1;
        while (i + run < pattern.size() && pattern[i + run] == c) {
            ++run;
        }
        if (c == 'h' || c == 'm' || c == 's') {
            long long v = c == 'h' ? h : c == 'm' ? m : s;
            char buf[32];
            std::snprintf(buf, sizeof(buf), run >= 2 ? "%02lld" : "%lld", v);
            out += buf;
        } else {
            out.append(run, c);
        }
        i += run;
    }
    return out;
}

std::string thousandsText(double v) {
    long long n = std::llround(v);
    bool neg = n < 0;
    unsigned long long u =
        neg ? static_cast<unsigned long long>(-(n + 1)) + 1ull : static_cast<unsigned long long>(n);
    std::string digits = std::to_string(u);
    std::string out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i != 0 && (digits.size() - i) % 3 == 0) {
            out.push_back(',');
        }
        out.push_back(digits[i]);
    }
    return neg ? "-" + out : out;
}

std::string fixedText(double v, int decimals) {
    decimals = std::clamp(decimals, 0, 20);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    std::string s(buf);
    return s == "-0" ? std::string("0") : s;
}

bool parseColorText(std::string_view text, Color& out) {
    Value v;
    if (mini_css::classifyTokenText(text, v) && v.type == ValueType::Color) {
        out = v.c;
        return true;
    }
    return false;
}

/// T-258a: a number usable as an enum index or entity id -- finite and not negative (a value past
/// the uint32 range saturates when converted).
bool isEnumIndex(double v) { return std::isfinite(v) && v >= 0.0; }

bool parseDouble(std::string_view text, double& out) {
    std::string s(text);
    const char* p = s.c_str();
    char* end = nullptr;
    double v = std::strtod(p, &end);
    if (s.empty() || end == p) {
        return false;
    }
    while (*end == ' ') {
        ++end;
    }
    if (*end != '\0') {
        return false;
    }
    out = v;
    return true;
}

}  // namespace binding_runtime_detail

// -- Load ---------------------------------------------------------------------------------------

BindingRuntime::BindingRuntime(Document& doc) : m_doc(doc) {}

BindingRuntime::~BindingRuntime() = default;

std::shared_ptr<const BindingRuntime::ListSchema> BindingRuntime::schemaFromIr(uint32_t field,
                                                                               int depth) const {
    const PortChunk& pc = m_doc.m_ir.ports();
    const IrPortNested* n = pc.nestedOf(field);
    if (n == nullptr || depth > 64) {
        return nullptr;
    }
    auto sc = std::make_shared<ListSchema>();
    sc->entryType = static_cast<PortValueType>(n->entryType);
    for (uint32_t k = 0; k < n->fieldCount && n->fieldOffset + k < pc.fields().size(); ++k) {
        const IrPortField& f = pc.fields()[n->fieldOffset + k];
        if (sc->entryType == PortValueType::Enum) {
            sc->enumValues.emplace_back(m_doc.m_ir.string(f.name));
            continue;
        }
        sc->fieldNames.push_back(f.name);
        sc->fieldTypes.push_back(static_cast<PortValueType>(f.type));
        sc->subs.push_back(static_cast<PortValueType>(f.type) == PortValueType::List
                               ? schemaFromIr(n->fieldOffset + k, depth + 1)
                               : nullptr);
    }
    return sc;
}

void BindingRuntime::load() {
    const IrView& ir = m_doc.m_ir;
    const PortChunk& pc = ir.ports();
    auto idOf = [&](uint32_t element) -> std::string {
        if (element >= m_doc.m_docElementNodes.size()) {
            return std::string();
        }
        NodeId node = m_doc.m_docElementNodes[element];
        if (node == kInvalidNode || !m_doc.m_arena.alive(node)) {
            return std::string();
        }
        StringId id = m_doc.m_arena.element(node).id;
        return id != kInvalidString ? std::string(m_doc.string(id)) : std::string();
    };
    m_slots.resize(pc.ports().size());
    for (size_t i = 0; i < m_slots.size(); ++i) {
        Slot& s = m_slots[i];
        s.ir = pc.ports()[i];
        s.name = std::string(ir.string(s.ir.name));
        s.element = s.ir.element < m_doc.m_docElementNodes.size() ? m_doc.m_docElementNodes[s.ir.element]
                                                                  : kInvalidNode;
        s.elementId = idOf(s.ir.element);
        // T-380: a port declared inside a `<repeat>` template; only its instance copies are live.
        s.templateOnly = m_doc.m_repeats->isTemplateElement(s.ir.element);
        s.qualifiedName =
            s.ir.scope == kNoPortScope || s.elementId.empty() ? s.name : s.elementId + "." + s.name;
        if (s.ir.scope != kNoPortScope && (s.ir.flags & kPortExpose) != 0) {
            std::string root = idOf(s.ir.scope);
            if (!root.empty()) {
                s.exposedName = root + "." + s.name;
            }
        }
        if (static_cast<PortValueType>(s.ir.type) == PortValueType::List) {
            s.entryType = static_cast<PortValueType>(s.ir.entryType);  // T-381
        }
        for (uint32_t f = 0; f < s.ir.fieldCount; ++f) {
            const IrPortField& field = pc.fields()[s.ir.fieldOffset + f];
            if (static_cast<PortValueType>(s.ir.type) == PortValueType::Enum ||
                s.entryType == PortValueType::Enum) {
                s.enumValues.emplace_back(ir.string(field.name));
            } else {
                s.fieldNames.push_back(field.name);
                s.fieldTypes.push_back(static_cast<PortValueType>(field.type));
                // T-391: a list<T> field's entry type (IrView::open validated the nesting).
                s.fieldSubs.push_back(static_cast<PortValueType>(field.type) == PortValueType::List
                                          ? schemaFromIr(s.ir.fieldOffset + f)
                                          : nullptr);
            }
        }
        switch (static_cast<PortValueType>(s.ir.type)) {
            case PortValueType::Color:
                s.value = Value::ofColor(Color{0, 0, 0, 0});
                break;
            case PortValueType::Vec2:
                s.value = Value::ofVec2(Vec2{0, 0});
                break;
            case PortValueType::Vec3:
                s.value = Value::ofVec3(Vec3{0, 0, 0});
                break;
            case PortValueType::Struct:
                for (PortValueType t : s.fieldTypes) {
                    s.fields.push_back(t == PortValueType::List    ? Value::ofList(nullptr, 0)  // T-391
                                       : t == PortValueType::Color ? Value::ofColor(Color{0, 0, 0, 0})
                                       : t == PortValueType::String || t == PortValueType::Texture
                                           ? Value::ofStringId(kInvalidString)
                                       : t == PortValueType::Entity ? Value::ofEnum(0)
                                       : t == PortValueType::Vec2   ? Value::ofVec2(Vec2{0, 0})
                                       : t == PortValueType::Vec3   ? Value::ofVec3(Vec3{0, 0, 0})
                                                                    : Value::ofFloat(0));
                }
                break;
            default:
                break;
        }
    }
    // T-358: declared defaults (IrView::open validated the port index, type and String / Enum ids).
    for (const IrPortDefault& d : pc.defaults()) {
        if (d.port >= m_slots.size()) {
            continue;
        }
        Slot& s = m_slots[d.port];
        float f[3] = {0, 0, 0};
        std::memcpy(f, d.value, sizeof(f));
        uint32_t u = 0;
        std::memcpy(&u, d.value, sizeof(u));
        switch (static_cast<PortValueType>(s.ir.type)) {
            case PortValueType::Number:
                s.num = finiteOr(f[0], 0.0f);
                break;
            case PortValueType::Bool:
                s.num = d.value[0] != 0 ? 1.0 : 0.0;
                break;
            case PortValueType::Color:
                s.value = Value::ofColor(Color{d.value[0] / 255.0f, d.value[1] / 255.0f, d.value[2] / 255.0f,
                                               d.value[3] / 255.0f});
                break;
            case PortValueType::String:
                s.text = std::string(ir.string(u));
                break;
            case PortValueType::Vec2:
                s.value = Value::ofVec2(Vec2{finiteOr(f[0], 0.0f), finiteOr(f[1], 0.0f)});
                break;
            case PortValueType::Vec3:
                s.value =
                    Value::ofVec3(Vec3{finiteOr(f[0], 0.0f), finiteOr(f[1], 0.0f), finiteOr(f[2], 0.0f)});
                break;
            case PortValueType::Enum:
                for (uint32_t k = 0; k < s.ir.fieldCount; ++k) {
                    if (pc.fields()[s.ir.fieldOffset + k].name == u) {
                        s.num = static_cast<double>(k);
                        break;
                    }
                }
                break;
            case PortValueType::Struct:  // T-385: a `<use>` literal's JSON object, likewise.
            case PortValueType::List: {
                // T-383 (27 §27.3.4): the JSON array, parsed as setPortText() does; at load nothing
                // listens yet and the repeats build their first instances afterwards (load()).
                const bool templateOnly = s.templateOnly;
                s.templateOnly = false;
                m_quietList = true;
                (void)setText(static_cast<PortId>(d.port + 1), ir.string(u));
                m_quietList = false;
                m_slots[d.port].templateOnly = templateOnly;
                break;
            }
            default:
                break;
        }
    }
    // `<name>_max` companions.
    for (size_t i = 0; i < m_slots.size(); ++i) {
        const std::string want = m_slots[i].name + "_max";
        for (size_t k = 0; k < m_slots.size(); ++k) {
            if (m_slots[k].ir.scope == m_slots[i].ir.scope && m_slots[k].name == want) {
                m_slots[i].maxPort = static_cast<int>(k);
                break;
            }
        }
    }
    addBonePorts();  // T-244: after the IR's ports, so PORT indices stay slot indices.
    // `port(<name>)` triggers.
    const TriggerChunk& tc = ir.triggers();
    for (const IrTriggerTable& table : tc.tables()) {
        if (table.element >= m_doc.m_docElementNodes.size() ||
            m_doc.m_repeats->isTemplateElement(table.element)) {
            continue;  // T-177: template elements never fire; their copies register themselves.
        }
        NodeId node = m_doc.m_docElementNodes[table.element];
        for (uint32_t k = 0; k < table.triggerCount; ++k) {
            const IrTrigger& t = tc.triggers()[table.triggerOffset + k];
            if (static_cast<TriggerEvent>(t.event) != TriggerEvent::Port || t.arg1 == 0 ||
                t.arg1 == kInvalidString || t.arg1 > m_slots.size()) {
                continue;
            }
            std::vector<NodeId>& nodes = m_slots[t.arg1 - 1].triggerNodes;
            if (std::find(nodes.begin(), nodes.end(), node) == nodes.end()) {
                nodes.push_back(node);
            }
        }
    }
    // Bindings.
    const BindingChunk& bc = ir.bindings();
    m_bindings.resize(bc.bindings().size());
    for (size_t i = 0; i < m_bindings.size(); ++i) {
        Binding& b = m_bindings[i];
        b.ir = &bc.bindings()[i];
        if (b.ir->element < m_doc.m_docElementNodes.size()) {
            b.node = m_doc.m_docElementNodes[b.ir->element];
        }
        b.templateOnly = m_doc.m_repeats->isTemplateElement(b.ir->element);
        if (b.ir->sceneNode != kBindElementSelf && !b.templateOnly) {
            b.node = resolveSceneBinding(*b.ir, b.node);  // T-243
        }
        if (b.node != kInvalidNode && m_doc.m_arena.alive(b.node)) {
            b.generation = m_doc.m_arena.generation(b.node);
        }
        b.steps = bc.steps().subspan(b.ir->stepOffset, b.ir->stepCount);
        b.sources = bc.sources().subspan(b.ir->sourceOffset, b.ir->sourceCount);
        b.state.resize(b.steps.size());
        if ((b.ir->flags & kBindNoPrimary) == 0 && !b.sources.empty()) {
            b.primary = static_cast<int>(b.sources[0]);
            b.maxPort = m_slots[b.sources[0]].maxPort;
        }
        for (uint32_t source : b.sources) {
            if (b.templateOnly) {
                break;
            }
            std::vector<uint32_t>& list = m_slots[source].bindings;
            if (std::find(list.begin(), list.end(), static_cast<uint32_t>(i)) == list.end()) {
                list.push_back(static_cast<uint32_t>(i));
            }
        }
        for (const IrConverterStep& step : b.steps) {
            b.usesCustom = b.usesCustom || step.op == static_cast<uint8_t>(ConverterOp::Custom);
            b.usesLocalize = b.usesLocalize || step.op == static_cast<uint8_t>(ConverterOp::Localize);
        }
        // A final tween on an animatable Float/Length/Color property: an Animator transition.
        if (static_cast<BindTarget>(b.ir->target) == BindTarget::Prop && !isStylePropId(b.ir->targetId) &&
            b.ir->sceneNode == kBindElementSelf) {
            std::vector<Arg> args;
            size_t lastHead = 0;
            for (size_t k = 0; k < b.steps.size(); k = gather(b.steps, k, args)) {
                lastHead = k;
            }
            ValueType vt = propertyType(static_cast<PropId>(b.ir->targetId));
            if (!b.steps.empty() && b.steps[lastHead].op == static_cast<uint8_t>(ConverterOp::Tween) &&
                (vt == ValueType::Float || vt == ValueType::Length || vt == ValueType::Color)) {
                gather(b.steps, lastHead, args);
                b.transitionTween = true;
                b.tweenDuration = args.empty() ? 0.0f : args[0].f;
                b.tweenEasing = Easing::linear();
                if (args.size() >= 6) {
                    b.tweenEasing.kind = static_cast<EasingKind>(toInt32Sat(args[1].f));
                    if (b.tweenEasing.kind == EasingKind::Steps) {
                        b.tweenEasing.stepCount = std::max<uint16_t>(1, toUint16Sat(args[2].f));
                        b.tweenEasing.stepsJumpEnd =
                            static_cast<uint8_t>(std::min<uint32_t>(toUint32Sat(args[3].f), 255));
                    } else {
                        b.tweenEasing.x1 = args[2].f;
                        b.tweenEasing.y1 = args[3].f;
                        b.tweenEasing.x2 = args[4].f;
                        b.tweenEasing.y2 = args[5].f;
                    }
                }
            }
        }
        if ((b.ir->flags & kBindAfterLayout) != 0 && !b.templateOnly) {
            m_afterLayout.push_back(static_cast<uint32_t>(i));
        }
    }
    for (uint32_t i = 0; i < m_bindings.size(); ++i) {
        if ((m_bindings[i].ir->flags & kBindAfterLayout) == 0) {
            evaluate(i, /*initial=*/true);
        }
    }
}

// -- Per frame ----------------------------------------------------------------------------------

void BindingRuntime::markDirty(uint32_t b) {
    if (b >= m_dirtyFlag.size()) {
        m_dirtyFlag.resize(static_cast<size_t>(b) + 1, 0);
    }
    if (m_dirtyFlag[b] == 0) {
        m_dirtyFlag[b] = 1;
        m_dirty.push_back(b);
    }
}

void BindingRuntime::flushDirty() {
    // A binding that writes a port marks that port's readers again: they are picked up by the
    // next round (bounded, so a cycle between port bindings cannot spin here).
    for (int round = 0; round < 16 && !m_dirty.empty(); ++round) {
        std::vector<uint32_t> batch;
        batch.swap(m_dirty);
        for (uint32_t b : batch) {
            if (b < m_dirtyFlag.size()) {
                m_dirtyFlag[b] = 0;
            }
        }
        for (uint32_t b : batch) {
            if (b < m_bindings.size()) {
                evaluate(b, /*initial=*/false);
            }
        }
    }
}

void BindingRuntime::tick() {
    flushDirty();  // T-389: the port changes since the last frame, one evaluation per binding.
    for (uint32_t i = 0; i < m_bindings.size(); ++i) {
        if (m_bindings[i].ticking) {
            evaluate(i, /*initial=*/false);
        }
    }
}

bool BindingRuntime::reapplyValue(NodeId node) {
    bool found = false;
    for (uint32_t i = 0; i < m_bindings.size(); ++i) {
        Binding& b = m_bindings[i];
        if (b.node != node || b.templateOnly || b.free ||
            static_cast<BindTarget>(b.ir->target) != BindTarget::Value) {
            continue;
        }
        found = true;
        b.applied = false;  // Applied again even when the port value did not change.
        evaluate(i, /*initial=*/false);
    }
    return found;
}

void BindingRuntime::reapplyAllValues() {
    for (uint32_t i = 0; i < m_bindings.size(); ++i) {
        Binding& b = m_bindings[i];
        if (!b.templateOnly && !b.free && static_cast<BindTarget>(b.ir->target) == BindTarget::Value) {
            b.applied = false;
            evaluate(i, /*initial=*/false);
        }
    }
}

void BindingRuntime::reapplyShownValues() {
    const NodeArena& arena = m_doc.m_arena;
    for (uint32_t i = 0; i < m_bindings.size(); ++i) {
        Binding& b = m_bindings[i];
        if (b.templateOnly || b.free || static_cast<BindTarget>(b.ir->target) != BindTarget::Value ||
            !arena.alive(b.node) || arena.generation(b.node) != b.generation) {
            continue;
        }
        bool hidden = false;
        for (NodeId n = b.node; n != kInvalidNode && !hidden; n = arena.parent(n)) {
            hidden = arena.kind(n) == NodeKind::Element && arena.element(n).style.display == Display::None;
        }
        const bool shown = b.valueHidden && !hidden;
        b.valueHidden = hidden;
        if (shown) {
            b.applied = false;  // The user's leftover edit gives way to the port value.
            evaluate(i, /*initial=*/false);
        }
    }
}

void BindingRuntime::relocalize() {
    for (uint32_t i = 0; i < m_bindings.size(); ++i) {
        if (m_bindings[i].usesLocalize) {
            evaluate(i, /*initial=*/false);
        }
    }
}

bool BindingRuntime::afterLayout() {
    m_anchorMoved = false;
    bool changed = false;
    for (uint32_t i : m_afterLayout) {
        Binding& b = m_bindings[i];
        const bool wasApplied = b.applied;
        BVal before = b.last;
        evaluate(i, /*initial=*/!wasApplied);
        changed = changed || (b.applied && (!wasApplied || !same(before, b.last)));
    }
    return changed || m_anchorMoved;
}

// -- Document forwarding -------------------------------------------------------------------------

PortId Document::lookupPort(std::string_view name) const { return m_bindings->lookup(name); }
std::vector<PortRecord> Document::ports() const { return m_bindings->ports(); }
PortRecord Document::portInfo(PortId port) const { return m_bindings->info(port); }
// T-260a: every call below that fires port listeners, `port()` triggers or binding state changes
// holds a DispatchGuard (its first local).
Status Document::setPort(PortId port, const Value& value) {
    DispatchGuard dispatch(m_context);
    return m_bindings->set(port, value);
}
Status Document::setPortText(PortId port, std::string_view text) {
    DispatchGuard dispatch(m_context);
    return m_bindings->setText(port, text);
}
Value Document::getPort(PortId port) { return m_bindings->get(port); }
std::string Document::portText(PortId port) const { return m_bindings->text(port); }
Result<Value> Document::callPort(PortId port, std::span<const Value> args) {
    DispatchGuard dispatch(m_context);
    return m_bindings->call(port, args);
}
Status Document::setPortCallHandler(PortId port, PortCallHandler handler) {
    return m_bindings->setCallHandler(port, std::move(handler));
}
Status Document::listSet(PortId port, std::span<const Value> entries) {
    DispatchGuard dispatch(m_context);
    return m_bindings->listSet(port, entries);
}
Status Document::listInsert(PortId port, uint32_t index, const Value& entry) {
    DispatchGuard dispatch(m_context);
    return m_bindings->listInsert(port, index, entry);
}
Status Document::listRemove(PortId port, uint32_t index) {
    DispatchGuard dispatch(m_context);
    return m_bindings->listRemove(port, index);
}
Status Document::listUpdate(PortId port, uint32_t index, const Value& entry) {
    DispatchGuard dispatch(m_context);
    return m_bindings->listUpdate(port, index, entry);
}
// -- Hot reload (T-200) ---------------------------------------------------------------------------

void BindingRuntime::missingPorts(const BindingRuntime& from, std::vector<std::string>& missing) const {
    for (const Slot& o : from.m_slots) {
        const PortValueType type = static_cast<PortValueType>(o.ir.type);
        if (type == PortValueType::Event || o.templateOnly || o.instance || o.free || o.nested) {
            continue;  // T-380 / T-391: repeat instance copies are rebuilt with the instances.
        }
        const PortId id = lookup(o.qualifiedName);
        const Slot* n = slot(id);
        if (n == nullptr || n->ir.type != o.ir.type) {
            missing.push_back("port " + o.qualifiedName);
            continue;
        }
        if (type == PortValueType::Enum) {
            const size_t i = static_cast<size_t>(o.num);
            if (i < o.enumValues.size() && std::find(n->enumValues.begin(), n->enumValues.end(),
                                                     o.enumValues[i]) == n->enumValues.end()) {
                missing.push_back("port " + o.qualifiedName + " (enum value " + o.enumValues[i] + ")");
            }
        }
    }
}

void BindingRuntime::adoptPorts(const BindingRuntime& from) {
    // A field value of `from` in this Document (StringRefs belong to the other string table).
    auto carry = [&](const Value& v) {
        return v.type == ValueType::StringRef ? Value::ofStringId(m_doc.intern(from.m_doc.string(v.sid))) : v;
    };
    // `to`'s fields from `oldFields` (named by `o.fieldNames`), by field name and type.
    // T-391: a list<T> field is converted into new storage (`trees`), its entries by field name.
    auto carryFields = [&](const Slot& o, const std::vector<Value>& oldFields, const Slot& n,
                           std::vector<Value>& to, std::vector<TreePtr>& trees) {
        for (size_t f = 0; f < n.fieldNames.size() && f < to.size(); ++f) {
            const std::string_view name = m_doc.string(n.fieldNames[f]);
            for (size_t k = 0; k < o.fieldNames.size() && k < oldFields.size(); ++k) {
                if (from.m_doc.string(o.fieldNames[k]) != name || o.fieldTypes[k] != n.fieldTypes[f]) {
                    continue;
                }
                if (n.fieldTypes[f] == PortValueType::List) {
                    auto tree = std::make_shared<ValueTree>();
                    Value list;
                    if (f < n.fieldSubs.size() && n.fieldSubs[f] != nullptr &&
                        convertNestedList(*n.fieldSubs[f], oldFields[k], from.m_doc, *tree, list)) {
                        to[f] = list;
                        trees.resize(n.fieldNames.size());
                        trees[f] = std::move(tree);
                    }
                } else {
                    to[f] = carry(oldFields[k]);
                }
                break;
            }
        }
    };
    for (const Slot& o : from.m_slots) {
        const PortValueType type = static_cast<PortValueType>(o.ir.type);
        if (o.templateOnly || o.instance || o.free || o.nested) {
            continue;  // T-380 / T-391: repeat instance copies are rebuilt with the instances.
        }
        const PortId id = lookup(o.qualifiedName);
        Slot* n = slot(id);
        if (type == PortValueType::Event || n == nullptr || n->ir.type != o.ir.type ||
            n->entryType != o.entryType) {
            continue;
        }
        switch (type) {
            case PortValueType::Enum: {
                const size_t i = static_cast<size_t>(o.num);
                auto it = i < o.enumValues.size()
                              ? std::find(n->enumValues.begin(), n->enumValues.end(), o.enumValues[i])
                              : n->enumValues.end();
                if (it != n->enumValues.end()) {
                    n->num = static_cast<double>(it - n->enumValues.begin());
                }
                break;
            }
            case PortValueType::String:
            case PortValueType::Texture:
                n->text = o.text;
                break;
            case PortValueType::Color:
            case PortValueType::Vec2:
            case PortValueType::Vec3:
                n->value = o.value;
                break;
            case PortValueType::Struct:
                carryFields(o, o.fields, *n, n->fields, n->fieldTrees);
                break;
            case PortValueType::List: {
                std::vector<std::vector<Value>> entries;
                std::vector<std::vector<TreePtr>> entryTrees;  // T-391
                entries.reserve(o.entries.size());
                for (const std::vector<Value>& entry : o.entries) {
                    std::vector<Value> fields = blankEntry(*n);
                    std::vector<TreePtr> trees;
                    if (n->entryType != static_cast<PortValueType>(0)) {
                        // T-381: a primitive entry carries over when it still converts (an Enum
                        // name the new declaration still lists).
                        if (entry.empty() || !convertEntry(*n, entry[0], fields)) {
                            continue;
                        }
                    } else {
                        carryFields(o, entry, *n, fields, trees);
                    }
                    entries.push_back(std::move(fields));
                    entryTrees.push_back(std::move(trees));
                }
                n->entries = std::move(entries);
                n->entryTrees = std::move(entryTrees);
                rebuildEntries(*n);
                m_doc.m_repeats->onListChanged(id - 1, RepeatListOp::Set, 0);
                break;
            }
            default:  // Number, Bool, Entity.
                n->num = o.num;
                break;
        }
    }
    for (uint32_t i = 0; i < m_bindings.size(); ++i) {
        if ((m_bindings[i].ir == nullptr || (m_bindings[i].ir->flags & kBindAfterLayout) == 0)) {
            evaluate(i, /*initial=*/true);
        }
    }
}

Status Document::setWorldAnchor(PortId port, float x, float y, float depth, bool onScreen) {
    DispatchGuard dispatch(m_context);  // T-260a: the port and `Offscreen` state triggers.
    return m_bindings->setWorldAnchor(port, x, y, depth, onScreen);
}

Result<uint32_t> Document::registerConverter(std::string_view name, ConverterFn fn) {
    DispatchGuard dispatch(m_context);  // T-260a: re-evaluated bindings may set states.
    return m_bindings->registerConverter(name, std::move(fn));
}
uint32_t Document::addPortListener(PortListener listener) {
    return m_bindings->addListener(std::move(listener));
}
void Document::removePortListener(uint32_t id) { m_bindings->removeListener(id); }

}  // namespace xt
