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

namespace {

using minijson::Json;
using minijson::JsonField;
using minijson::JsonReader;

/// T-394 (08.15): sets BindingRuntime::m_documentWrite for one write, restoring it after.
struct DocumentWriteScope {
    bool& slot;
    bool saved;
    DocumentWriteScope(bool& s, bool on) : slot(s), saved(s) { slot = on; }
    ~DocumentWriteScope() { slot = saved; }
    DocumentWriteScope(const DocumentWriteScope&) = delete;
    DocumentWriteScope& operator=(const DocumentWriteScope&) = delete;
};

/// A binding target numbered as a compiler StyleProp (160-207, below the shape-modifier /
/// parametric-shape PropIds 208+, T-225b; and from 256 on, below the effect PropIds, T-331)
/// rather than an animatable PropId.
bool isStylePropId(uint16_t id) {
    return (id >= 160 && id < prop::ModTrimStart) ||
           (id > prop::ConstraintEnabled && id < prop::kEffectPropBase);
}

using BVal = BindingRuntime::BVal;
using Kind = BindingRuntime::BVal::Kind;

void markStyleDirtySubtree(NodeArena& arena, NodeId node) {
    arena.markDirty(node, static_cast<uint8_t>(kDirtyStyle));
    arena.traversePreorder(node, [&arena](NodeId n) {
        arena.markDirty(n, static_cast<uint8_t>(kDirtyStyle));
        return true;
    });
}

BVal num(double v, bool isBool = false) {
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

}  // namespace

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

// -- Evaluation ---------------------------------------------------------------------------------

size_t BindingRuntime::gather(std::span<const IrConverterStep> steps, size_t i, std::vector<Arg>& out) const {
    out.clear();
    const IrConverterStep& head = steps[i];
    // T-174 conditions (comparisons, ops 7-13) carry argCount 0: their four slots are all there
    // is. Any other op with argCount 0 takes no argument.
    const bool legacyCompare = head.op >= static_cast<uint8_t>(ConverterOp::Lt) &&
                               head.op <= static_cast<uint8_t>(ConverterOp::Between);
    const uint32_t total = head.argCount == 0 ? (legacyCompare ? 4u : 0u) : head.argCount;
    size_t k = i;
    uint32_t taken = 0;
    while (taken < total && k < steps.size()) {
        const IrConverterStep& step = steps[k];
        if (k != i && step.op != static_cast<uint8_t>(ConverterOp::Operands)) {
            break;
        }
        for (uint32_t a = 0; a < 4 && taken < total; ++a, ++taken) {
            Arg arg;
            arg.kind = converterArgKind(step, a);
            arg.f = step.args[a];
            std::memcpy(&arg.u, &step.args[a], sizeof(uint32_t));
            out.push_back(arg);
        }
        ++k;
    }
    if (k == i) {
        k = i + 1;  // An op without arguments still consumes its own step.
    }
    while (k < steps.size() && steps[k].op == static_cast<uint8_t>(ConverterOp::Operands)) {
        ++k;
    }
    return k;
}

BVal BindingRuntime::argValue(const Arg& a, bool relativePercent, const EvalCtx& ctx) const {
    switch (a.kind) {
        case ConverterArg::Number:
            return num(a.f);
        case ConverterArg::String:
            return str(std::string(m_doc.m_ir.string(a.u)));
        case ConverterArg::Port: {
            // T-380: a repeat instance's copy reads its own copies of template ports.
            const uint32_t port = ctx.binding != nullptr ? mapPort(ctx.binding->portMap, a.u) : a.u;
            return port < m_slots.size() ? portValue(port) : BVal{};
        }
        case ConverterArg::Item: {  // T-177: a field of the instance's entry.
            const Binding* b = ctx.binding;
            if (b == nullptr || !b->instance || b->listPort >= m_slots.size()) {
                return BVal{};
            }
            const Slot& s = m_slots[b->listPort];
            if (b->item >= s.entries.size()) {
                return BVal{};
            }
            std::string_view field = m_doc.m_ir.string(a.u);
            if (field.empty()) {  // T-381: `item()`, the whole entry of a list<number|string|...>.
                return b->item < s.entryValues.size() ? entryToB(s, s.entryValues[b->item]) : BVal{};
            }
            for (size_t f = 0; f < s.fieldNames.size() && f < s.entries[b->item].size(); ++f) {
                if (m_doc.string(s.fieldNames[f]) == field) {
                    return valueToB(s.entries[b->item][f]);
                }
            }
            return BVal{};
        }
        case ConverterArg::Index:
            return num(ctx.binding != nullptr && ctx.binding->instance && ctx.binding->item != kNoItem
                           ? ctx.binding->item
                           : 0);
        case ConverterArg::Count:
            return num(ctx.binding != nullptr && ctx.binding->instance ? entryCount(ctx.binding->listPort)
                                                                       : 0);
        case ConverterArg::OuterItem: {  // T-391: a field of the enclosing repeat's entry.
            const Binding* b = ctx.binding;
            if (b == nullptr || !b->instance || b->outerPort >= m_slots.size()) {
                return BVal{};
            }
            const Slot& s = m_slots[b->outerPort];
            if (b->outerItem >= s.entries.size()) {
                return BVal{};
            }
            const std::string_view field = m_doc.m_ir.string(a.u);
            for (size_t f = 0; f < s.fieldNames.size() && f < s.entries[b->outerItem].size(); ++f) {
                if (m_doc.string(s.fieldNames[f]) == field) {
                    return valueToB(s.entries[b->outerItem][f]);
                }
            }
            return BVal{};
        }
        case ConverterArg::OuterIndex:  // T-391
            return num(ctx.binding != nullptr && ctx.binding->instance && ctx.binding->outerItem != kNoItem
                           ? ctx.binding->outerItem
                           : 0);
        case ConverterArg::Percent:
            if (!relativePercent) {
                return ofValue(Kind::Len, Value::ofLength(a.f, LengthUnit::Percent));
            }
            if (ctx.maxPort >= 0) {
                return num(a.f / 100.0 * toNum(portValue(static_cast<uint32_t>(ctx.maxPort))));
            }
            return num(a.f / 100.0);
        default:
            return BVal{};
    }
}

BVal BindingRuntime::run(std::span<const IrConverterStep> steps, BVal v, EvalCtx& ctx) {
    std::vector<Arg> args;
    for (size_t i = 0; i < steps.size();) {
        const size_t stepIndex = i;
        const ConverterOp op = static_cast<ConverterOp>(steps[i].op);
        i = gather(steps, i, args);
        if (op == ConverterOp::Operands) {
            continue;  // Stray operands (never written by the compiler).
        }
        // The final tween of a transitionTween binding is the Animator's job (apply()).
        if (op == ConverterOp::Tween && ctx.binding != nullptr && ctx.binding->transitionTween &&
            i >= steps.size()) {
            continue;
        }
        v = applyStep(op, args, stepIndex, std::move(v), ctx);
        if (op == ConverterOp::Item && v.kind == Kind::None && !args.empty() &&
            args[0].kind == ConverterArg::Number) {
            // T-383 (27 §27.3.4, was T-381's "not applied"): `item(<n>)` out of range yields the
            // entry type's empty value; a list<struct> entry's `| item(<field>)` that the compiler
            // requires next yields that field's empty value instead.
            const uint32_t port =
                ctx.binding != nullptr && ctx.binding->primary >= 0
                    ? mapPort(ctx.binding->portMap, static_cast<uint32_t>(ctx.binding->primary))
                    : kNoPort;
            if (port >= m_slots.size()) {
                return v;
            }
            const Slot& s = m_slots[port];
            if (s.entryType != static_cast<PortValueType>(0)) {
                v = emptyEntryValue(s, {});
                continue;
            }
            if (i >= steps.size() || static_cast<ConverterOp>(steps[i].op) != ConverterOp::Item) {
                return BVal{};
            }
            i = gather(steps, i, args);
            v = args.empty() ? BVal{} : emptyEntryValue(s, toStr(argValue(args[0], false, ctx)));
            if (v.kind == Kind::None) {
                return v;
            }
        }
    }
    return v;
}

BVal BindingRuntime::applyStep(ConverterOp op, const std::vector<Arg>& args, size_t stepIndex, BVal v,
                               EvalCtx& ctx) {
    auto a = [&](size_t i, bool relative = false) {
        return i < args.size() ? argValue(args[i], relative, ctx) : BVal{};
    };
    auto an = [&](size_t i, double def, bool relative = false) {
        return i < args.size() ? toNum(argValue(args[i], relative, ctx)) : def;
    };
    StepState* state = ctx.binding != nullptr && stepIndex < ctx.binding->state.size()
                           ? &ctx.binding->state[stepIndex]
                           : nullptr;
    const float clock = m_doc.m_clock;
    switch (op) {
        case ConverterOp::Range: {
            double inMin = an(0, 0, true), inMax = an(1, 1, true);
            double t = inMax != inMin ? (toNum(v) - inMin) / (inMax - inMin) : 0.0;
            t = std::clamp(t, 0.0, 1.0);
            BVal o0 = a(2), o1 = a(3);
            double out = toNum(o0) + (toNum(o1) - toNum(o0)) * t;
            if (o0.kind == Kind::Len || o1.kind == Kind::Len) {
                LengthUnit u = o0.kind == Kind::Len ? o0.v.len.unit : o1.v.len.unit;
                return ofValue(Kind::Len, Value::ofLength(static_cast<float>(out), u));
            }
            return num(out);
        }
        case ConverterOp::Clamp:
            return num(std::clamp(toNum(v), an(0, 0), std::max(an(0, 0), an(1, 0))));
        case ConverterOp::Round: {
            double p = std::pow(10.0, std::clamp(an(0, 0), 0.0, 10.0));
            return num(std::round(toNum(v) * p) / p);
        }
        case ConverterOp::Abs:
            return num(std::fabs(toNum(v)));
        case ConverterOp::Scale:
            return num(toNum(v) * an(0, 1));
        case ConverterOp::Offset:
            return num(toNum(v) + an(0, 0));
        case ConverterOp::Lt:
            return num(toNum(v) < an(0, 0, true) ? 1 : 0, true);
        case ConverterOp::Le:
            return num(toNum(v) <= an(0, 0, true) ? 1 : 0, true);
        case ConverterOp::Gt:
            return num(toNum(v) > an(0, 0, true) ? 1 : 0, true);
        case ConverterOp::Ge:
            return num(toNum(v) >= an(0, 0, true) ? 1 : 0, true);
        case ConverterOp::Eq:
        case ConverterOp::Ne: {
            BVal rhs = a(0, true);
            bool eq = (v.kind == Kind::Str || rhs.kind == Kind::Str) ? toStr(v) == toStr(rhs)
                                                                     : toNum(v) == toNum(rhs);
            return num((op == ConverterOp::Eq) == eq ? 1 : 0, true);
        }
        case ConverterOp::Between: {
            double x = toNum(v);
            return num(x >= an(0, 0, true) && x <= an(1, 0, true) ? 1 : 0, true);
        }
        case ConverterOp::Not:
            return num(truthy(v) ? 0 : 1, true);
        case ConverterOp::And:
            return num(truthy(v) && truthy(a(0)) ? 1 : 0, true);
        case ConverterOp::Or:
            return num(truthy(v) || truthy(a(0)) ? 1 : 0, true);
        case ConverterOp::Format: {
            const std::string pattern = toStr(a(0));
            std::string out;
            for (size_t i = 0; i < pattern.size(); ++i) {
                if (pattern[i] == '{') {
                    size_t close = pattern.find('}', i);
                    if (close != std::string::npos && close > i + 1) {
                        std::string idx = pattern.substr(i + 1, close - i - 1);
                        bool digits =
                            std::all_of(idx.begin(), idx.end(), [](char c) { return c >= '0' && c <= '9'; });
                        if (digits) {
                            size_t n = static_cast<size_t>(std::strtoul(idx.c_str(), nullptr, 10));
                            out += n == 0 ? toStr(v) : toStr(a(n));
                            i = close;
                            continue;
                        }
                    }
                }
                out.push_back(pattern[i]);
            }
            return str(std::move(out));
        }
        case ConverterOp::Thousands:
            return str(thousandsText(toNum(v)));
        case ConverterOp::Fixed:
            return str(fixedText(toNum(v), toInt32Sat(an(0, 0))));
        case ConverterOp::Percent:
            return str(fixedText(toNum(v) * 100.0, toInt32Sat(an(0, 0))) + "%");
        case ConverterOp::Time:
            return str(timeText(toNum(v), args.empty() ? std::string("mm:ss") : toStr(a(0))));
        case ConverterOp::Upper:
        case ConverterOp::Lower: {
            std::string s = toStr(v);
            for (char& c : s) {
                if (c >= 'a' && c <= 'z' && op == ConverterOp::Upper) {
                    c = static_cast<char>(c - 'a' + 'A');
                } else if (c >= 'A' && c <= 'Z' && op == ConverterOp::Lower) {
                    c = static_cast<char>(c - 'A' + 'a');
                }
            }
            return str(std::move(s));
        }
        case ConverterOp::Select: {
            for (size_t i = 1; i + 1 < args.size(); i += 2) {
                BVal key = a(i);
                bool match = key.kind == Kind::Num && v.kind != Kind::Str ? toNum(key) == toNum(v)
                                                                          : toStr(key) == toStr(v);
                if (match) {
                    return a(i + 1);
                }
            }
            return a(0);
        }
        case ConverterOp::Step: {
            BVal result = args.empty() || args[0].kind == ConverterArg::None ? a(2) : a(0);
            double x = toNum(v);
            for (size_t i = 1; i + 1 < args.size(); i += 2) {
                if (x >= an(i, 0)) {
                    result = a(i + 1);
                }
            }
            return result;
        }
        case ConverterOp::LerpColor: {
            auto color = [&](size_t i) {
                BVal c = a(i);
                if (c.kind == Kind::Color) {
                    return c.v.c;
                }
                Color out{0, 0, 0, 0};
                parseColorText(toStr(c), out);
                return out;
            };
            Value c0 = Value::ofColor(color(0));
            Value c1 = Value::ofColor(color(1));
            float t = static_cast<float>(std::clamp(toNum(v), 0.0, 1.0));
            return ofValue(Kind::Color, lerpValue(c0, c1, t, nullptr, nullptr));
        }
        case ConverterOp::FromToken: {
            std::string name = toStr(a(0));
            size_t at = name.find("{0}");
            if (at != std::string::npos) {
                name.replace(at, 3, toStr(v));
            }
            return str("var(--" + name + ")");
        }
        case ConverterOp::Localize: {
            std::string key = toStr(v);
            if (!key.empty() && key.front() == '@') {
                key.erase(0, 1);
            }
            std::string_view text;
            if (m_doc.m_l10nProvider != nullptr && !m_doc.m_unloading) {  // T-260a: no host call.
                text = m_doc.m_l10nProvider->resolve(key, m_doc.m_language);
            }
            return str(text.empty() ? key : std::string(text));
        }
        case ConverterOp::Concat: {
            std::string s = toStr(v);
            for (size_t i = 0; i < args.size(); ++i) {
                s += toStr(a(i));
            }
            return str(std::move(s));
        }
        case ConverterOp::Glyph:
            return str(args.empty() ? toStr(v) : toStr(a(0)));
        case ConverterOp::Tween: {
            if (state == nullptr) {
                return v;
            }
            const float duration = static_cast<float>(an(0, 0));
            Easing easing = Easing::linear();
            if (args.size() >= 6) {
                easing.kind = static_cast<EasingKind>(toInt32Sat(args[1].f));
                if (easing.kind == EasingKind::Steps) {
                    easing.stepCount = std::max<uint16_t>(1, toUint16Sat(args[2].f));
                    easing.stepsJumpEnd =
                        static_cast<uint8_t>(std::min<uint32_t>(toUint32Sat(args[3].f), 255));
                } else {
                    easing.x1 = args[2].f;
                    easing.y1 = args[3].f;
                    easing.x2 = args[4].f;
                    easing.y2 = args[5].f;
                }
            }
            auto at = [&](float now) {
                float p = duration > 0 ? std::clamp((now - state->start) / duration, 0.0f, 1.0f) : 1.0f;
                return std::make_pair(lerpB(state->from, state->to, evalEasing(easing, p)), p);
            };
            if (!state->init) {
                state->init = true;
                state->from = v;
                state->to = v;
                state->start = clock;
                return v;
            }
            if (!same(v, state->to)) {
                state->from = at(clock).first;
                state->to = v;
                state->start = clock;
            }
            auto [out, p] = at(clock);
            if (p < 1.0f) {
                ctx.ticking = true;
            }
            return out;
        }
        case ConverterOp::Delay: {
            if (state == nullptr) {
                return v;
            }
            const float d = static_cast<float>(an(0, 0));
            if (!state->init) {
                state->init = true;
                state->to = v;
                state->out = v;
                return v;
            }
            if (!same(v, state->to)) {
                state->to = v;
                state->queue.emplace_back(clock + d, v);
            }
            while (!state->queue.empty() && state->queue.front().first <= clock) {
                state->out = state->queue.front().second;
                state->queue.pop_front();
            }
            if (!state->queue.empty()) {
                ctx.ticking = true;
            }
            return state->out;
        }
        case ConverterOp::Count:
            if (v.kind == Kind::List) {
                return num(v.v.itemCount);
            }
            // T-177: in a `<repeat>` instance, the list's entry count.
            return num(ctx.binding != nullptr && ctx.binding->instance ? entryCount(ctx.binding->listPort)
                                                                       : 0);
        case ConverterOp::Index:
            if (!args.empty() && args[0].kind == ConverterArg::OuterIndex) {
                return a(0);  // T-391: `outer.index` as a chain head.
            }
            // T-177: the `<repeat>` instance's entry index (0 outside an instance).
            return num(ctx.binding != nullptr && ctx.binding->instance && ctx.binding->item != kNoItem
                           ? ctx.binding->item
                           : 0);
        case ConverterOp::Item: {
            if (!args.empty() && args[0].kind == ConverterArg::OuterItem) {
                return a(0);  // T-391: `outer.item(<field>)` as a chain head.
            }
            if (!args.empty() && args[0].kind == ConverterArg::Number) {
                // T-381 (27 §27.3.4): `<list port> | item(<n>)`, entry n (none when out of range).
                const double n = args[0].f;
                if (v.kind != Kind::List || !(n >= 0) || n >= static_cast<double>(v.v.itemCount)) {
                    return BVal{};
                }
                const Value& entry = v.v.items[static_cast<uint32_t>(n)];
                const uint32_t port =
                    ctx.binding != nullptr && ctx.binding->primary >= 0
                        ? mapPort(ctx.binding->portMap, static_cast<uint32_t>(ctx.binding->primary))
                        : kNoPort;
                return port < m_slots.size() ? entryToB(m_slots[port], entry) : valueToB(entry);
            }
            const std::string field = toStr(a(0));
            if (field.empty()) {
                return v;  // T-381: `item()` (a repeat chain head): the entry itself.
            }
            if (v.kind != Kind::Struct) {
                return BVal{};
            }
            for (uint32_t i = 0; i < v.v.itemCount; ++i) {
                if (m_doc.string(v.v.fieldNames[i]) == field) {
                    return valueToB(v.v.items[i]);
                }
            }
            return BVal{};
        }
        case ConverterOp::ElemProp:
            return elemProp(args, state);
        case ConverterOp::X:
        case ConverterOp::Y:
        case ConverterOp::Z:
        case ConverterOp::W: {  // T-200: one component.
            const int i = static_cast<int>(op) - static_cast<int>(ConverterOp::X);
            switch (v.kind) {
                case Kind::Vec2:
                    return num(i == 0 ? v.v.v2.x : i == 1 ? v.v.v2.y : 0.0f);
                case Kind::Vec3:
                    return num(i == 0 ? v.v.v3.x : i == 1 ? v.v.v3.y : i == 2 ? v.v.v3.z : 0.0f);
                case Kind::Color:
                    return num(i == 0 ? v.v.c.r : i == 1 ? v.v.c.g : i == 2 ? v.v.c.b : v.v.c.a);
                default:
                    return num(i == 0 ? toNum(v) : 0.0);
            }
        }
        case ConverterOp::Vec: {  // T-200: vec2(...) / vec3(...).
            float c[3] = {0, 0, 0};
            for (size_t i = 0; i < args.size() && i < 3; ++i) {
                c[i] = static_cast<float>(args[i].kind == ConverterArg::None ? toNum(v) : an(i, 0));
            }
            return args.size() >= 3 ? ofValue(Kind::Vec3, Value::ofVec3(Vec3{c[0], c[1], c[2]}))
                                    : ofValue(Kind::Vec2, Value::ofVec2(Vec2{c[0], c[1]}));
        }
        case ConverterOp::Custom: {
            if (m_doc.m_unloading) {
                return v;  // T-260a: an unloading Document calls no host converter (pass-through).
            }
            const std::string name = toStr(a(0));
            for (const auto& [n, fn] : m_converters) {
                if (n != name) {
                    continue;
                }
                // T-270c: a host converter takes Values, so a string argument needs a StringId;
                // the new ones count against this binding's guard.
                StringGuard& guard = ctx.binding != nullptr ? ctx.binding->strings : m_conditionStrings;
                const std::string_view source =
                    ctx.binding != nullptr ? m_doc.m_ir.string(ctx.binding->ir->targetName) : name;
                std::vector<Value> values;
                for (size_t i = 1; i < args.size(); ++i) {
                    values.push_back(bToValue(a(i), guard, source));
                }
                ConverterFn call = fn;  // A copy: the host may register again inside.
                return valueToB(call(bToValue(v, guard, source), values));
            }
            if (std::find(m_warnedConverters.begin(), m_warnedConverters.end(), name) ==
                m_warnedConverters.end()) {
                m_warnedConverters.push_back(name);
                m_doc.addWarning("binding: host converter \"" + name +
                                 "\" is not registered; passing through");
            }
            return v;
        }
        default:
            return v;
    }
}

BVal BindingRuntime::elemProp(const std::vector<Arg>& args, StepState* state) {
    if (args.size() < 2) {
        return BVal{};
    }
    NodeId node = kInvalidNode;
    if (state != nullptr && state->elem != kInvalidNode && m_doc.m_arena.alive(state->elem) &&
        m_doc.m_arena.generation(state->elem) == state->elemGeneration) {
        node = state->elem;
    } else {
        ElementRef ref = m_doc.byId(m_doc.m_ir.string(args[0].u));
        if (!ref.valid()) {
            return BVal{};
        }
        node = ref.node;
        if (state != nullptr) {
            state->elem = node;
            state->elemGeneration = ref.generation;
        }
    }
    ElementRef ref{&m_doc, node, m_doc.m_arena.generation(node)};
    std::string_view prop = m_doc.m_ir.string(args[1].u);
    const LayoutBox& box = m_doc.m_arena.element(node).box;
    if (prop == "width") {
        return num(box.width);
    }
    if (prop == "height") {
        return num(box.height);
    }
    if (prop == "left") {
        return num(box.x);
    }
    if (prop == "top") {
        return num(box.y);
    }
    if (prop == "text") {
        return str(std::string(ref.text()));
    }
    uint16_t id = mini_css::findProp(prop);
    if (id == prop::kInvalidProp || id >= mini_css::kTransformProp) {
        return BVal{};
    }
    return valueToB(id < 160 ? ref.property(static_cast<PropId>(id)) : ref.computedStyleValue(id));
}

void BindingRuntime::evaluate(uint32_t index, bool initial) {
    Binding& b = m_bindings[index];
    if (b.templateOnly || b.free || (b.instance && b.item == kNoItem) || b.node == kInvalidNode ||
        !m_doc.m_arena.alive(b.node) || m_doc.m_arena.generation(b.node) != b.generation) {
        b.ticking = false;
        return;
    }
    if (m_depth > 16) {
        return;
    }
    ++m_depth;
    EvalCtx ctx;
    ctx.binding = &b;
    ctx.maxPort = b.maxPort;
    BVal v = b.primary >= 0 ? portValue(static_cast<uint32_t>(b.primary)) : BVal{};
    if ((b.ir->flags & kBindRepeatItem) != 0) {
        // T-177: the chain starts at the instance's list entry.
        v = b.instance && b.listPort < m_slots.size() && b.item < m_slots[b.listPort].entryValues.size()
                ? entryToB(m_slots[b.listPort], m_slots[b.listPort].entryValues[b.item])
                : BVal{};
    }
    BVal r = run(b.steps, std::move(v), ctx);
    b.ticking = ctx.ticking;
    // T-200: a world anchor also depends on the layout, so it is re-applied on every evaluation
    // (applyWorldAnchor() only touches the element when its translate actually changes).
    const bool anchor = static_cast<BindTarget>(b.ir->target) == BindTarget::WorldAnchor;
    if (r.kind != Kind::None && (anchor || !b.applied || !same(r, b.last))) {
        apply(b, r, initial || !b.applied);
        b.last = r;
        b.applied = true;
    }
    --m_depth;
}

NodeId BindingRuntime::sceneNodeOf(NodeId element, uint32_t sceneNode) const {
    const NodeArena& arena = m_doc.m_arena;
    if (element == kInvalidNode || !arena.alive(element) || arena.kind(element) != NodeKind::Element) {
        return kInvalidNode;
    }
    const ElementData& data = arena.element(element);
    if (data.scene == kInvalidScene || data.sceneInstanceIndex >= m_doc.m_instances.size()) {
        return kInvalidNode;
    }
    const SceneInstance& inst = m_doc.m_instances[data.sceneInstanceIndex];
    return sceneNode < inst.localToArena.size() ? inst.localToArena[sceneNode] : kInvalidNode;
}

NodeId BindingRuntime::resolveSceneBinding(const IrBinding& ir, NodeId element) {
    const BindTarget target = static_cast<BindTarget>(ir.target);
    const NodeId node = sceneNodeOf(element, ir.sceneNode);
    const char* why = nullptr;
    if (target == BindTarget::WorldAnchor && node != kInvalidNode &&
        m_doc.m_arena.kind(node) == NodeKind::Bone) {
        return node;  // T-244 (32 §32.8): `world-anchor` on a bone.
    }
    if (target != BindTarget::Prop) {
        why = "only a property binds on <svg> content";
    } else if (isStylePropId(ir.targetId) || !propertyDefined(static_cast<PropId>(ir.targetId))) {
        why = "not a scene node property";
    } else if (node == kInvalidNode) {
        why = "the element has no such scene node";
    } else if (!propertyApplies(m_doc.m_arena.kind(node), static_cast<PropId>(ir.targetId))) {
        why = "the property does not apply to the scene node";
    }
    if (why == nullptr) {
        return node;
    }
    m_doc.addWarning(std::string("kInvalidArgument: binding on scene node ") + std::to_string(ir.sceneNode) +
                     " ignored: " + why);
    return kInvalidNode;
}

void BindingRuntime::applySceneProp(const Binding& b, const BVal& r) {
    const PropId prop = static_cast<PropId>(b.ir->targetId);
    Value pv;
    switch (propertyType(prop)) {
        case ValueType::Float:
            if (r.kind != Kind::Num && r.kind != Kind::Len) {
                return;
            }
            pv = Value::ofFloat(static_cast<float>(toNum(r)));
            break;
        case ValueType::Enum:
            if (r.kind != Kind::Num || !isEnumIndex(r.num)) {
                return;  // T-258a: a negative or non-finite index is not written.
            }
            pv = Value::ofEnum(toUint32Sat(r.num));
            break;
        case ValueType::Vec2:
        case ValueType::Color:
            if ((r.kind != Kind::Vec2 && r.kind != Kind::Color) || r.v.type != propertyType(prop)) {
                return;
            }
            pv = r.v;
            break;
        default:
            return;  // No scene node property of another type binds (T-243).
    }
    setProperty(m_doc.m_arena, b.node, prop, pv);
}

void BindingRuntime::apply(Binding& b, const BVal& r, bool initial) {
    Document& doc = m_doc;
    const IrBinding& ir = *b.ir;
    if (ir.sceneNode != kBindElementSelf) {
        if (static_cast<BindTarget>(ir.target) == BindTarget::WorldAnchor) {
            applyBoneWorldAnchor(b.node, r);  // T-244: load() kept it only on a bone.
            return;
        }
        applySceneProp(b, r);  // T-243 (32 §32.4): load() kept only Prop targets.
        return;
    }
    ElementRef ref{&doc, b.node, b.generation};
    switch (static_cast<BindTarget>(ir.target)) {
        case BindTarget::Prop: {
            std::string_view css = doc.m_ir.string(ir.targetName);
            // T-270c: "<prop>: <value>" in a reused buffer; setStyle keeps a text value in the
            // element's own slot, so a new string every frame interns nothing.
            auto styleText = [&]() -> std::string_view {
                m_styleScratch.assign(css);
                m_styleScratch += ": ";
                m_styleScratch += toStr(r);
                return m_styleScratch;
            };
            if (isStylePropId(ir.targetId)) {
                ref.setStyle(styleText());
                return;
            }
            const PropId prop = static_cast<PropId>(ir.targetId);
            const ValueType vt = propertyType(prop);
            Value pv;
            bool typed = false;
            if ((prop == xt::prop::TranslateX || prop == xt::prop::TranslateY) &&
                (r.kind == Kind::Num || r.kind == Kind::Len)) {
                // T-201: `bind:translate-x/y` -- an element's translate is a Length declaration.
                pv = r.kind == Kind::Len ? r.v : Value::ofLength(static_cast<float>(r.num), LengthUnit::Px);
                typed = true;
            } else if (vt == ValueType::Float && (r.kind == Kind::Num || r.kind == Kind::Len)) {
                pv = Value::ofFloat(static_cast<float>(toNum(r)));
                typed = true;
            } else if (vt == ValueType::Length && r.kind == Kind::Num) {
                pv = Value::ofLength(static_cast<float>(r.num), LengthUnit::Px);
                typed = true;
            } else if (vt == ValueType::Length && r.kind == Kind::Len) {
                pv = r.v;
                typed = true;
            } else if (vt == ValueType::Color && r.kind == Kind::Color) {
                pv = r.v;
                typed = true;
            } else if (vt == ValueType::Enum && r.kind == Kind::Num) {
                if (!isEnumIndex(r.num)) {
                    return;  // T-258a: a negative or non-finite index is not written.
                }
                pv = Value::ofEnum(toUint32Sat(r.num));
                typed = true;
            } else if (r.kind != Kind::Str && r.kind != Kind::None) {
                typed = mini_css::parseTokenText(prop, toStr(r), pv);
            }
            if (!typed) {
                ref.setStyle(styleText());
                return;
            }
            if (b.transitionTween && !initial) {
                // 04 §4.10.4: a tween on the property is a transition from its current final value.
                Value from = ref.property(prop);
                const bool sameShape =
                    from.type == pv.type && (from.type != ValueType::Length || from.len.unit == pv.len.unit);
                if (sameShape && b.tweenDuration > 0) {
                    Transition tr;
                    tr.node = b.node;
                    tr.prop = prop;
                    tr.from = from;
                    tr.to = pv;
                    tr.startClock = doc.m_clock;
                    tr.delay = 0;
                    tr.duration = b.tweenDuration;
                    tr.easing = b.tweenEasing;
                    doc.m_animator.addTransition(tr);
                }
            }
            ElementData& data = doc.m_arena.element(b.node);
            data.setStyleHash = 0;  // T-389: runtimeDecls changed outside setStyle().
            RuntimeStyleDecl decl;
            decl.prop = prop;
            decl.value = pv;
            bool replaced = false;
            for (RuntimeStyleDecl& existing : data.runtimeDecls) {
                if (existing.prop == decl.prop) {
                    existing = decl;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                data.runtimeDecls.push_back(decl);
            }
            markStyleDirtySubtree(doc.m_arena, b.node);
            return;
        }
        case BindTarget::Text:
            if (ref.text() != toStr(r)) {
                ref.setText(toStr(r));
            }
            return;
        case BindTarget::Src: {
            const std::string name = toStr(r);
            std::span<const IrAsset> assets = doc.m_ir.assets().assets();
            for (size_t i = 0; i < assets.size(); ++i) {
                if (assets[i].kind == 0 && doc.m_ir.string(assets[i].name) == name) {
                    doc.m_arena.element(b.node).imageAsset = static_cast<AssetId>(i);
                    doc.m_arena.markDirty(b.node, static_cast<uint8_t>(kDirtyLayout | kDirtyPaint));
                    return;
                }
            }
            doc.addWarning("bind:src: no image asset \"" + name + "\" in the document");
            return;
        }
        case BindTarget::Visible:
            ref.setStyle(truthy(r) ? "visibility: visible" : "visibility: hidden");
            return;
        case BindTarget::State:
            doc.applyCustomState(b.node, ir.targetId,
                                 static_cast<uint8_t>(truthy(r) ? TriggerMode::On : TriggerMode::Off));
            return;
        case BindTarget::Class: {
            std::string_view cls = doc.m_ir.string(ir.targetName);
            if (truthy(r)) {
                ref.addClass(cls);
            } else {
                ref.removeClass(cls);
            }
            return;
        }
        case BindTarget::WorldAnchor:
            applyWorldAnchor(b.node, r);
            return;
        case BindTarget::Port:  // T-380 (27 §27.3.4): a symbol instance's in-port.
            writePort(mapPort(b.portMap, ir.targetId), r, initial);
            return;
        case BindTarget::Value:
            // T-392 (27 §27.3.5): `bind:value`, the input's runtime value without a Change event.
            doc.m_events->setEditValue(doc.m_arena, b.node, toStr(r));
            return;
        case BindTarget::Checked:
        case BindTarget::Disabled:
        case BindTarget::BuiltinState: {
            // T-204: the state bit only -- no Change event, no check / uncheck triggers.
            // T-385: `bind:disabled` the same way with ElementState Disabled; T-392:
            // `bind:state.Focused|Hover|Active|Checked` with the bit in targetId.
            ElementData& data = doc.m_arena.element(b.node);
            const BindTarget kind = static_cast<BindTarget>(ir.target);
            const uint16_t bit = kind == BindTarget::BuiltinState ? ir.targetId
                                 : kind == BindTarget::Checked    ? uint16_t{kStateChecked}
                                                                  : uint16_t{kStateDisabled};
            const uint16_t next = truthy(r) ? static_cast<uint16_t>(data.stateBits | bit)
                                            : static_cast<uint16_t>(data.stateBits & ~bit);
            if (next != data.stateBits) {
                data.stateBits = next;
                markStyleDirtySubtree(doc.m_arena, b.node);
            }
            return;
        }
        case BindTarget::Display: {
            // T-385 (27 §27.3.2): falsy -> `display: none`, truthy -> the style sheet's value again.
            // A string that is not a bool spelling is the display value itself (the older form).
            if (r.kind == Kind::Str && !r.str.empty() && r.str != "true" && r.str != "false" &&
                r.str != "0" && r.str != "1") {
                ref.setStyle("display: " + r.str);
                return;
            }
            if (!truthy(r)) {
                ref.setStyle("display: none");
                return;
            }
            ElementData& data = doc.m_arena.element(b.node);
            data.setStyleHash = 0;  // T-389: runtimeDecls changed outside setStyle().
            const size_t before = data.runtimeDecls.size();
            std::erase_if(data.runtimeDecls, [](const RuntimeStyleDecl& d) {
                return d.prop == style_decode::styleprop::Display;
            });
            if (data.runtimeDecls.size() != before) {
                markStyleDirtySubtree(doc.m_arena, b.node);
            }
            return;
        }
        case BindTarget::Variant: {
            const std::string prefix(doc.m_ir.string(ir.targetName));
            std::vector<std::string> drop;
            for (StringId cls : doc.m_arena.element(b.node).classes) {
                std::string_view name = doc.string(cls);
                if (name.substr(0, prefix.size()) == prefix) {
                    drop.emplace_back(name);
                }
            }
            for (const std::string& cls : drop) {
                ref.removeClass(cls);
            }
            const std::string value = toStr(r);
            if (!value.empty()) {
                const std::string cls = prefix + value;
                countString(cls, b.strings, prefix);  // T-270c: a class name needs a StringId.
                ref.addClass(cls);
            }
            return;
        }
    }
}

// -- World anchors (T-200) ----------------------------------------------------------------------

namespace {

/// `node`'s border-box top-left in viewport logical coordinates from the layout alone (every
/// transform and scroll offset left out): each box is relative to its parent's content box.
Vec2 layoutOrigin(const NodeArena& arena, NodeId node) {
    Vec2 p{0, 0};
    NodeId n = node;
    while (n != kInvalidNode && arena.alive(n) && arena.kind(n) == NodeKind::Element) {
        const LayoutBox& box = arena.element(n).box;
        p.x += box.x;
        p.y += box.y;
        const NodeId parent = arena.parent(n);
        if (parent != kInvalidNode && arena.alive(parent) && arena.kind(parent) == NodeKind::Element) {
            const Rect content = arena.element(parent).box.contentBox();
            p.x += content.x;
            p.y += content.y;
        }
        n = parent;
    }
    return p;
}

}  // namespace

void BindingRuntime::applyWorldAnchor(NodeId node, const BVal& r) {
    if (r.kind != Kind::Vec2 && r.kind != Kind::Vec3) {
        return;
    }
    const float ax = r.kind == Kind::Vec2 ? r.v.v2.x : r.v.v3.x;
    const float ay = r.kind == Kind::Vec2 ? r.v.v2.y : r.v.v3.y;
    NodeArena& arena = m_doc.m_arena;
    ElementData& data = arena.element(node);
    data.setStyleHash = 0;  // T-389: runtimeDecls changed outside setStyle().
    // `left` / `top` semantics: the margin box's top-left lands on the anchor; the run-time
    // translate moves the element there without relaying anything else out.
    const Vec2 origin = layoutOrigin(arena, node);
    const float target[2] = {ax + data.box.margin.left - origin.x, ay + data.box.margin.top - origin.y};
    const PropId props[2] = {prop::TranslateX, prop::TranslateY};
    bool moved = false;
    for (int i = 0; i < 2; ++i) {
        RuntimeStyleDecl* found = nullptr;
        for (RuntimeStyleDecl& existing : data.runtimeDecls) {
            if (existing.prop == props[i]) {
                found = &existing;
                break;
            }
        }
        if (found != nullptr && found->value.type == ValueType::Length &&
            found->value.len.unit == LengthUnit::Px && found->value.len.value == target[i]) {
            continue;
        }
        RuntimeStyleDecl decl;
        decl.prop = props[i];
        decl.value = Value::ofLength(target[i], LengthUnit::Px);  // The style engine wants a length.
        if (found != nullptr) {
            *found = decl;
        } else {
            data.runtimeDecls.push_back(decl);
        }
        moved = true;
    }
    if (moved) {
        markStyleDirtySubtree(arena, node);
        m_anchorMoved = true;
    }
}

void BindingRuntime::applyBoneWorldAnchor(NodeId bone, const BVal& r) {
    if (r.kind != Kind::Vec2 && r.kind != Kind::Vec3) {
        return;
    }
    NodeArena& arena = m_doc.m_arena;
    if (bone == kInvalidNode || !arena.alive(bone) || arena.kind(bone) != NodeKind::Bone) {
        return;
    }
    const NodeId skeleton = rig::skeletonOf(arena, bone);
    if (skeleton == kInvalidNode) {
        return;
    }
    // Screen (viewport logical, like the element version) -> the skeleton group's space, through
    // the group's last world matrix (the element's layout position and every transform above).
    Mat2x3 inv;
    if (!invert(arena.common(skeleton).world, inv)) {
        return;
    }
    const Vec2 screen{r.kind == Kind::Vec2 ? r.v.v2.x : r.v.v3.x, r.kind == Kind::Vec2 ? r.v.v2.y : r.v.v3.y};
    rig::applyBoneAnchor(arena, bone, xt::apply(inv, screen));
}

void BindingRuntime::addBonePorts() {
    m_bonePorts.clear();
    const NodeArena& arena = m_doc.m_arena;
    for (const SceneInstance& inst : m_doc.m_instances) {
        for (NodeId node : inst.localToArena) {
            if (node == kInvalidNode || !arena.alive(node) || arena.kind(node) != NodeKind::Bone ||
                arena.name(node) == kInvalidString) {
                continue;
            }
            const std::string id(m_doc.string(arena.name(node)));
            for (int k = 0; k < 2; ++k) {
                const std::string name = id + (k == 0 ? ".world" : ".angle");
                const PortValueType type = k == 0 ? PortValueType::Vec2 : PortValueType::Number;
                if (const PortId have = lookup(name); have != kInvalidPort) {
                    // T-243b: the compiler declares the bone ports (PORT); the first bone with
                    // the id takes the declared one. Another bone with the same id, or a port of
                    // another kind with that name, keeps what is there.
                    Slot& d = m_slots[have - 1];
                    if (d.bone == kInvalidNode && d.name == name && d.ir.scope == kNoPortScope &&
                        static_cast<PortDir>(d.ir.dir) == PortDir::Out &&
                        static_cast<PortValueType>(d.ir.type) == type) {
                        d.bone = node;
                        m_bonePorts.push_back(have - 1);
                    }
                    continue;
                }
                Slot s;
                s.ir = IrPort{};
                s.ir.name = kInvalidString;
                s.ir.type = static_cast<uint8_t>(type);
                s.ir.dir = static_cast<uint8_t>(PortDir::Out);
                s.ir.element = 0xFFFFFFFFu;
                s.ir.scope = kNoPortScope;
                s.name = name;
                s.qualifiedName = name;
                s.bone = node;
                if (k == 0) {
                    s.value = Value::ofVec2(Vec2{0, 0});
                }
                m_bonePorts.push_back(static_cast<uint32_t>(m_slots.size()));
                m_slots.push_back(std::move(s));
            }
        }
    }
}

void BindingRuntime::publishBone(NodeId bone, Vec2 screen, float angleDeg) {
    constexpr float kTolerance = 1e-3f;
    for (uint32_t index : m_bonePorts) {
        Slot& s = m_slots[index];
        if (s.bone != bone) {
            continue;
        }
        if (static_cast<PortValueType>(s.ir.type) == PortValueType::Vec2) {
            const Vec2 have = s.value.v2;
            if (std::fabs(have.x - screen.x) <= kTolerance && std::fabs(have.y - screen.y) <= kTolerance) {
                continue;
            }
            s.value = Value::ofVec2(screen);
        } else {
            if (std::fabs(s.num - static_cast<double>(angleDeg)) <= kTolerance) {
                continue;
            }
            s.num = angleDeg;
        }
        notify(index, PortEventKind::Changed, 0);
    }
}

Status BindingRuntime::setWorldAnchor(PortId port, float x, float y, float depth, bool onScreen) {
    Slot* s = slot(port);
    if (s == nullptr) {
        return Status::kNotFound;
    }
    const PortValueType type = static_cast<PortValueType>(s->ir.type);
    if ((type != PortValueType::Vec2 && type != PortValueType::Vec3) || !std::isfinite(x) ||
        !std::isfinite(y) || !std::isfinite(depth)) {
        return Status::kInvalidArgument;
    }
    const Status status =
        set(port, type == PortValueType::Vec2 ? Value::ofVec2(Vec2{x, y}) : Value::ofVec3(Vec3{x, y, depth}));
    if (status != Status::kOk) {
        return status;
    }
    // 13.8.3: an anchor off screen turns the custom state `Offscreen` on for every element
    // anchored to the port (when the document declares that state), for off-screen indicators.
    for (uint32_t index : std::vector<uint32_t>(m_slots[port - 1].bindings)) {
        const Binding& b = m_bindings[index];
        if (static_cast<BindTarget>(b.ir->target) != BindTarget::WorldAnchor || b.node == kInvalidNode ||
            !m_doc.m_arena.alive(b.node) || m_doc.m_arena.generation(b.node) != b.generation) {
            continue;
        }
        (void)m_doc.setCustomState(ElementRef{&m_doc, b.node, b.generation}, "Offscreen", !onScreen);
    }
    return Status::kOk;
}

bool BindingRuntime::evalCondition(std::span<const IrConverterStep> steps, float value, uint32_t port) {
    EvalCtx ctx;
    BVal v = num(value);
    if (port < m_slots.size()) {
        v = portValue(port);
        ctx.maxPort = m_slots[port].maxPort;
    }
    std::vector<Arg> args;
    for (size_t i = 0; i < steps.size();) {
        const size_t head = i;
        i = gather(steps, i, args);
        const ConverterOp op = static_cast<ConverterOp>(steps[head].op);
        if (op == ConverterOp::Operands) {
            continue;
        }
        if (!truthy(applyStep(op, args, head, v, ctx))) {
            return false;
        }
    }
    return true;
}

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
                // T-391: a list<T> field: converted into storage of its own.
                if (trees == nullptr || f >= s.fieldSubs.size() || s.fieldSubs[f] == nullptr) {
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
                    if (f >= sc.subs.size() || sc.subs[f] == nullptr ||
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
    std::vector<std::pair<uint32_t, PortListener>> listeners = m_listeners;  // Callbacks may edit the list.
    for (const auto& [id, fn] : listeners) {
        if (m_doc.m_unloading) {
            break;  // T-260a: an earlier listener unloaded this Document.
        }
        fn(event);
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
    // T-389 (27 §27.4.1): marked, evaluated once by the next Document::update() (flushDirty()).
    for (uint32_t b : m_slots[index].bindings) {
        markDirty(b);
    }
    notifyListeners(kind, index, listIndex);
    fireTriggers(index);
}

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
                            const bool ok = sc.fieldTypes[f] == PortValueType::List
                                                ? f < sc.subs.size() && sc.subs[f] != nullptr &&
                                                      jsonList(fv, *sc.subs[f], fields[f])
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
                    Value out;
                    const bool ok = s->fieldTypes[f] == PortValueType::List  // T-391
                                        ? f < s->fieldSubs.size() && s->fieldSubs[f] != nullptr &&
                                              jsonList(v, *s->fieldSubs[f], out)
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
    switch (static_cast<SetSource>(action.flags)) {
        case SetSource::Number:
            v = num(action.arg2);
            break;
        case SetSource::String: {
            uint32_t bits = 0;
            std::memcpy(&bits, &action.arg2, sizeof(bits));
            v.kind = Kind::Str;
            v.str = std::string(m_doc.m_ir.string(bits));
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
