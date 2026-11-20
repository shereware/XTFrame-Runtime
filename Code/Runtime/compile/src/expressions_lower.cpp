#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "expr/expr.h"
#include "expr/expr_runtime.h"
#include "expressions_section.h"
#include "style_emit.h"     // staticWinningDecl()
#include "symbol_expand.h"  // ExpandedSymbolInstance
#include "xt/compile/diagnostics.h"
#include "xt/compile/dom.h"
#include "xt/compile/style_model.h"
#include "xt/property.h"
#include "xts/xts_props.h"  // findProp(), findPropById(), initialValue()

/// T-767 (Docs/design/58-value-functions-and-drivers.md §58.3-§58.4, Docs/tasks/T-767.md): lowers
/// the `<expressions>` entries into the `@timeline` tracks of the RuleSet. See expressions_section.h.

namespace xt::compile {

// xts_values.cpp (as style_emit.cpp declares it): an effect parameter's value text by type.
bool parseEffectParamText(std::string_view text, ValueType type, StyleValue& out);

namespace {

namespace xe = expr;

/// A row name with components (`translate.x`), mapped onto the per-component PropIds the
/// stylesheet parser already splits these properties into: replacing one component's keys leaves
/// the others' untouched.
struct VecRowDef {
    const char* name;
    uint16_t props[3];  ///< x, y, z (0xFFFF = none).
    xe::ExprPropertyKind kind;
    uint16_t bare;  ///< The PropId of the row without a component (0xFFFF = a component is needed).
    bool valueVec;  ///< The components form a vector `valueVec` reads.
};

constexpr uint16_t kNone = 0xFFFF;

constexpr VecRowDef kVecRows[] = {
    {"translate",
     {prop::TranslateX, prop::TranslateY, prop::TranslateZ},
     xe::ExprPropertyKind::Position,
     kNone,
     true},
    {"scale", {prop::ScaleX, prop::ScaleY, kNone}, xe::ExprPropertyKind::Other, kNone, true},
    {"skew", {prop::SkewX, prop::SkewY, kNone}, xe::ExprPropertyKind::Angle, kNone, true},
    {"rotate",
     {prop::RotationX, prop::RotationY, prop::Rotation},
     xe::ExprPropertyKind::Angle,
     prop::Rotation,
     false},
    {"transform-origin", {prop::AnchorX, prop::AnchorY, kNone}, xe::ExprPropertyKind::Position, kNone, true},
    {"perspective-origin",
     {prop::PerspectiveOriginX, prop::PerspectiveOriginY, kNone},
     xe::ExprPropertyKind::Position,
     kNone,
     true},
    {"background-position",
     {prop::BackgroundPositionX, prop::BackgroundPositionY, kNone},
     xe::ExprPropertyKind::Position,
     kNone,
     true},
    {"background-size",
     {prop::BackgroundSizeX, prop::BackgroundSizeY, kNone},
     xe::ExprPropertyKind::Position,
     kNone,
     true},
};

/// The row an entry or a `layer(...).<row>` reference addresses: a PropId (a style property, a
/// transform component, or -- on inline `<svg>` content -- a vector / shape parameter), or an
/// effect parameter `effect.<effect>.<param>` (prop == kNone; the address stays symbolic, as in
/// TimelineEffectDecl, and is matched as written).
struct ResolvedRow {
    uint16_t prop = kNone;
    xe::ExprPropertyKind kind = xe::ExprPropertyKind::Other;
    int component = 0;               ///< Array element the row takes (x 0, y 1, z 2).
    const VecRowDef* vec = nullptr;  ///< The component group, when there is one.
    std::string effect;              ///< Effect rows: the stack index or effect name.
    std::string param;               ///< Effect rows: the parameter, with its `.x` / `.y`.

    bool isEffect() const { return prop == kNone; }
    /// Identifies the row among an element's rows.
    std::string id() const { return isEffect() ? "fx:" + effect + "." + param : std::to_string(prop); }
};

int componentIndex(std::string_view c) { return c == "y" ? 1 : c == "z" ? 2 : 0; }

/// Resolves the row `property[.component]`; `svg`: the element is a shape inside an inline `<svg>`
/// (vector and shape parameters allowed, no effects, tolerance "other"). False with `message` when
/// it is no numeric animatable row.
bool resolveRow(const std::string& property, const std::string& component, bool svg, ResolvedRow& out,
                std::string& message) {
    if (property.compare(0, 7, "effect.") == 0) {
        if (svg) {
            message = "inline <svg> content has no effects";
            return false;
        }
        const std::string rest = property.substr(7);
        const size_t dot = rest.find('.');
        if (dot == std::string::npos || dot == 0 || dot + 1 == rest.size()) {
            message = "write an effect row as effect.<stack index or name>.<param>";
            return false;
        }
        out.effect = rest.substr(0, dot);
        out.param = rest.substr(dot + 1) + (component.empty() ? std::string() : "." + component);
        out.kind = xe::ExprPropertyKind::Other;
        return true;
    }
    for (const VecRowDef& def : kVecRows) {
        if (property != def.name) {
            continue;
        }
        out.kind = svg ? xe::ExprPropertyKind::Other : def.kind;
        out.vec = def.valueVec ? &def : nullptr;
        if (component.empty()) {
            if (def.bare == kNone) {
                message = "\"" + property + "\" needs a component (.x or .y)";
                return false;
            }
            out.prop = def.bare;
            return true;
        }
        out.component = componentIndex(component);
        out.prop = def.props[out.component];
        if (out.prop == kNone) {
            message = "\"" + property + "\" has no ." + component + " component";
            return false;
        }
        return true;
    }
    const PropInfo* info = findProp(property);
    if (info == nullptr) {
        message = "unknown row \"" + property + "\"";
        return false;
    }
    if (info->kind == StyleValueKind::Shorthand) {
        message = "\"" + property + "\" is a shorthand; write the expression on one of its rows";
        return false;
    }
    if (!component.empty()) {
        message = "\"" + property + "\" has no components";
        return false;
    }
    if (!info->animatable || (info->kind != StyleValueKind::Float && info->kind != StyleValueKind::Length)) {
        message = "\"" + property + "\" is not a numeric animatable property";
        return false;
    }
    if (isVectorPropInfo(*info) && !svg) {
        message = "\"" + property + "\" is a row of shapes inside an inline <svg> (#<svg id>/<id>)";
        return false;
    }
    out.prop = info->prop;
    out.kind = info->kind == StyleValueKind::Length && !svg ? xe::ExprPropertyKind::Position
                                                            : xe::ExprPropertyKind::Other;
    return true;
}

/// T-770 (58 §58.2): the names an AE user writes after `layer(...).`, mapped onto the timeline row
/// names. The one table; any other camelCase name is read as the kebab-case row (`letterSpacing`).
struct RowAlias {
    const char* alias;
    const char* row;  ///< A row with or without its component.
};

constexpr RowAlias kRowAliases[] = {
    {"x", "translate.x"},         {"y", "translate.y"},         {"z", "translate.z"},
    {"position", "translate"},    {"xPosition", "translate.x"}, {"yPosition", "translate.y"},
    {"zPosition", "translate.z"}, {"rotation", "rotate"},       {"xRotation", "rotate.x"},
    {"yRotation", "rotate.y"},    {"zRotation", "rotate"},      {"anchorPoint", "transform-origin"},
    {"opacity", "opacity"},       {"scale", "scale"},
};

/// `layer(...).<name>` as a row: `property` and `component`. False with `message` for a name like
/// `x.y` (an alias that already names a component, given another).
bool referencedRow(std::string_view name, std::string& property, std::string& component,
                   std::string& message) {
    std::string_view base = name;
    component.clear();
    const size_t dot = name.rfind('.');
    if (dot != std::string_view::npos) {
        base = name.substr(0, dot);
        component = std::string(name.substr(dot + 1));
    }
    property.clear();
    for (const RowAlias& a : kRowAliases) {
        if (base == a.alias) {
            property = a.row;
            break;
        }
    }
    if (property.empty()) {
        for (char c : base) {
            if (c >= 'A' && c <= 'Z') {
                property += '-';
                property += static_cast<char>(c - 'A' + 'a');
            } else {
                property += c;
            }
        }
    }
    const size_t aliasDot = property.find('.');
    if (aliasDot != std::string::npos) {
        if (!component.empty()) {
            message = "\"" + std::string(name) + "\" names a component twice";
            return false;
        }
        component = property.substr(aliasDot + 1);
        property.resize(aliasDot);
    }
    return true;
}

/// A number and its unit (-1 = a plain Float).
struct Numeric {
    double value = 0;
    int unit = -1;
};

bool numericOf(const StyleValue& v, Numeric& out) {
    if (v.kind == StyleValueKind::Float) {
        out = Numeric{v.f, -1};
        return true;
    }
    if (v.kind == StyleValueKind::Length && v.len.unit != LengthUnit::Auto) {
        out = Numeric{v.len.value, static_cast<int>(v.len.unit)};
        return true;
    }
    return false;
}

StyleValue valueOf(double value, int unit) {
    StyleValue v;
    if (unit < 0) {
        v.kind = StyleValueKind::Float;
        v.f = static_cast<float>(value);
    } else {
        v.kind = StyleValueKind::Length;
        v.len = Length{static_cast<float>(value), static_cast<LengthUnit>(unit)};
    }
    return v;
}

/// The static value of `prop` when no rule sets it: the property's initial value, and for the
/// transform components (no CSS longhand) 1 for a scale and 0 (px for a translate) otherwise.
StyleValue defaultValue(uint16_t prop) {
    if (findPropById(prop) != nullptr) {
        return initialValue(prop);
    }
    if (prop == prop::ScaleX || prop == prop::ScaleY) {
        return valueOf(1, -1);
    }
    if (prop == prop::TranslateX || prop == prop::TranslateY || prop == prop::TranslateZ) {
        return valueOf(0, static_cast<int>(LengthUnit::Px));
    }
    return valueOf(0, -1);
}

std::string idOf(const DomNode& node) {
    const DomAttr* id = node.attr("id");
    return id != nullptr ? id->value : std::string();
}

uint32_t findElement(const DomTree& dom, std::string_view path) {
    if (path.empty()) {
        return 0xFFFFFFFFu;
    }
    for (uint32_t i = 0; i < dom.nodes.size(); ++i) {
        const DomAttr* id = dom.nodes[i].attr("id");
        if (id != nullptr && id->value == path) {
            return i;
        }
    }
    return 0xFFFFFFFFu;
}

/// AE's `index`: the element's 1-based position among its parent's children; 1 for the root of a
/// symbol instance (the instance holds nothing before it).
int siblingIndex(const DomTree& dom, uint32_t element, uint32_t scopeRoot) {
    const uint32_t parent = dom.nodes[element].parent;
    if (element == scopeRoot || parent >= dom.nodes.size()) {
        return 1;
    }
    const std::vector<uint32_t>& children = dom.nodes[parent].children;
    const auto it = std::find(children.begin(), children.end(), element);
    return it == children.end() ? 1 : static_cast<int>(it - children.begin()) + 1;
}

/// T-770 (11 §11.9): the inline `<svg>` a content path `<svg path>/<inner id>` addresses (the
/// `@timeline` track spelling; the inner id is checked when the scene is built), or 0xFFFFFFFF.
uint32_t findSvgOwner(const DomTree& dom, std::string_view path) {
    const size_t slash = path.rfind('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 == path.size()) {
        return 0xFFFFFFFFu;
    }
    const uint32_t svg = findElement(dom, path.substr(0, slash));
    return svg < dom.nodes.size() && dom.nodes[svg].tag == "svg" && !dom.nodes[svg].svgSource.empty()
               ? svg
               : 0xFFFFFFFFu;
}

/// The duration the timeline group will get (style_emit.cpp's emitTimelineGroups): `duration`, else
/// the latest key, marker or sound cue.
double timelineDuration(const TimelineDef& def) {
    if (def.duration >= 0) {
        return def.duration;
    }
    float d = 0;
    for (const TimelineTrackDef& track : def.tracks) {
        for (const TimelineKey& key : track.keys) {
            d = std::max(d, key.time);
        }
    }
    for (const auto& marker : def.markers) {
        d = std::max(d, marker.second);
    }
    for (const auto& sound : def.sounds) {
        d = std::max(d, sound.first);
    }
    return d;
}

/// One key of a row as written in the timeline.
struct RowSample {
    float time = 0;
    Numeric value;
    Easing arrival;  ///< The easing of the segment arriving at this key.
};

enum class RowRead { Ok, Empty, NotNumeric, MixedUnits };

/// The keys `prop` has on `path` in `def`, in time order.
RowRead readRow(const TimelineDef& def, const std::string& path, uint16_t prop, std::vector<RowSample>& out) {
    out.clear();
    for (const TimelineTrackDef& track : def.tracks) {
        if (track.path != path) {
            continue;
        }
        for (const TimelineKey& key : track.keys) {
            for (const StyleDecl& decl : key.decls) {
                if (decl.prop != prop) {
                    continue;
                }
                RowSample sample;
                sample.time = key.time;
                sample.arrival = key.hasEasing ? key.easing : Easing::linear();
                if (!numericOf(decl.value, sample.value)) {
                    return RowRead::NotNumeric;
                }
                out.push_back(sample);
            }
        }
    }
    if (out.empty()) {
        return RowRead::Empty;
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const RowSample& a, const RowSample& b) { return a.time < b.time; });
    for (const RowSample& s : out) {
        if (s.value.unit != out.front().value.unit) {
            return RowRead::MixedUnits;
        }
    }
    return RowRead::Ok;
}

/// T-770: the keys of the effect parameter `effect.<effect>.<param>` on `path` in `def`, matched as
/// written (28 §28.1.4); each value must be one number.
RowRead readEffectRow(const TimelineDef& def, const std::string& path, const ResolvedRow& row,
                      std::vector<RowSample>& out) {
    out.clear();
    for (const TimelineTrackDef& track : def.tracks) {
        if (track.path != path) {
            continue;
        }
        for (const TimelineKey& key : track.keys) {
            for (const TimelineEffectDecl& fx : key.effectDecls) {
                if (fx.effect != row.effect || fx.param != row.param) {
                    continue;
                }
                StyleValue v;
                if (!parseEffectParamText(fx.valueText, ValueType::Float, v)) {
                    return RowRead::NotNumeric;
                }
                RowSample sample;
                sample.time = key.time;
                sample.arrival = key.hasEasing ? key.easing : Easing::linear();
                sample.value = Numeric{v.f, -1};
                out.push_back(sample);
            }
        }
    }
    if (out.empty()) {
        return RowRead::Empty;
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const RowSample& a, const RowSample& b) { return a.time < b.time; });
    return RowRead::Ok;
}

/// True when `def` keys the vec2 effect parameter of the component row `row` (`offset.x`) whole.
bool effectKeyedWhole(const TimelineDef& def, const std::string& path, const ResolvedRow& row) {
    const size_t dot = row.param.rfind('.');
    if (dot == std::string::npos) {
        return false;
    }
    const std::string whole = row.param.substr(0, dot);
    for (const TimelineTrackDef& track : def.tracks) {
        for (const TimelineKey& key : track.keys) {
            for (const TimelineEffectDecl& fx : key.effectDecls) {
                if (track.path == path && fx.effect == row.effect && fx.param == whole) {
                    return true;
                }
            }
        }
    }
    return false;
}

/// A lowered effect-parameter value as `@timeline` value text (shortest round trip, no exponent).
std::string effectValueText(double value) {
    char buf[64];
    const auto res =
        std::to_chars(buf, buf + sizeof buf, static_cast<float>(value), std::chars_format::fixed);
    return res.ec == std::errc() ? std::string(buf, res.ptr) : std::string("0");
}

/// The expression engine's view of a row (each key's easing is the segment leaving it).
std::vector<xe::ExprKey> exprKeys(const std::vector<RowSample>& samples) {
    std::vector<xe::ExprKey> keys;
    keys.reserve(samples.size());
    for (size_t i = 0; i < samples.size(); ++i) {
        const Easing leaving = i + 1 < samples.size() ? samples[i + 1].arrival : Easing::linear();
        keys.push_back(xe::ExprKey{samples[i].time, samples[i].value.value, leaving});
    }
    return keys;
}

/// Lowers every entry of every source.
///
/// T-770 (58 §58.2-§58.3): an entry may read other rows of its timeline (`layer(...).<row>`), and
/// reads their final value -- lowered first when they have an expression too. Every entry of every
/// timeline becomes a Unit first (in file order; the outcome slots keep that order); then each
/// pending Unit is lowered by an iterative depth-first walk: a lowering attempt that asks for a row
/// whose own Unit is still pending is abandoned, that Unit is pushed and lowered, and the attempt is
/// made again (a topological order, discovered from the references the evaluation actually makes,
/// so `index > 1 ? layer(index - 1).y : value` depends on nothing on the first sibling). Asking for a
/// Unit already on the walk's stack closes a cycle: every Unit of the stack from that one up is
/// skipped with an `expr.cycle` note, and Units outside the cycle read those rows' own keys.
class Lowerer {
public:
    Lowerer(RuleSet& ruleSet, const DomTree& dom, const std::vector<ExpandedSymbolInstance>& instances,
            DiagnosticSink& sink, std::vector<ExpressionOutcome>* outcomes,
            std::vector<RuntimeExpression>* runtime, const ExpressionOptions& options)
        : m_rules(ruleSet),
          m_dom(dom),
          m_instances(instances),
          m_sink(sink),
          m_out(outcomes != nullptr ? outcomes : &m_ownOutcomes),
          m_runtime(runtime),
          m_options(options) {}

    void run(const std::vector<ExpressionSource>& sources) {
        m_entries.resize(sources.size());
        for (size_t s = 0; s < sources.size(); ++s) {
            const ExpressionSource& source = sources[s];
            parseExpressionsSection(source.text, source.line, source.col, source.file, m_sink, m_entries[s]);
            for (const ExpressionEntry& entry : m_entries[s]) {
                collect(source, entry);
            }
        }
        for (size_t u = 0; u < m_units.size(); ++u) {
            if (m_units[u].state == State::Pending) {
                walk(u);
            }
        }
        // A timeline whose length came from its keys keeps it, whatever the lowered rows end on.
        for (const auto& [def, duration] : m_durations) {
            if (m_lowered.count(def) != 0 && def->duration < 0) {
                def->duration = static_cast<float>(duration);
            }
        }
    }

private:
    enum class State : uint8_t { Pending, Active, Done };

    /// One entry on one timeline (a symbol's entry once per instance) that is to be lowered.
    struct Unit {
        const ExpressionSource* source = nullptr;
        const ExpressionEntry* entry = nullptr;
        TimelineDef* def = nullptr;
        uint32_t scopeRoot = 0xFFFFFFFFu;
        std::string scopePath;
        std::string path;      ///< The element's full path (the track path).
        uint32_t element = 0;  ///< Its DomTree node; the `<svg>` for inline `<svg>` content.
        bool svg = false;      ///< A shape inside an inline `<svg>`.
        ResolvedRow row;
        size_t outcome = 0;  ///< Index into *m_out.
        const xe::Program* program = nullptr;
        State state = State::Pending;
        bool cycle = false;
        std::string cycleText;  ///< The cycle, `#A translate.y -> #B translate.y -> #A translate.y`.
    };

    /// What one lowering attempt learned from its references.
    struct Attempt {
        size_t deferTo = SIZE_MAX;    ///< A pending Unit to lower first.
        size_t cycleWith = SIZE_MAX;  ///< A Unit on the walk's stack (a cycle closes there).
        std::vector<std::pair<std::string, std::string>> problems;  ///< (code, message) of failed references.
        std::vector<ExpressionOutcome::RuntimeRef> runtimeRefs;
        std::deque<std::vector<xe::ExprKey>> rows;  ///< Keeps the referenced rows' keys alive.
        bool abandoned() const { return deferTo != SIZE_MAX || cycleWith != SIZE_MAX; }
    };

    RuleSet& m_rules;
    const DomTree& m_dom;
    const std::vector<ExpandedSymbolInstance>& m_instances;
    DiagnosticSink& m_sink;
    std::vector<ExpressionOutcome> m_ownOutcomes;  ///< When the caller wants none.
    std::vector<ExpressionOutcome>* m_out;
    std::vector<std::vector<ExpressionEntry>> m_entries;  ///< Per source; filled once, then stable.
    std::deque<xe::ParseResult> m_parsed;                 ///< One per entry that is lowered.
    std::vector<Unit> m_units;
    std::map<std::string, size_t> m_unitByRow;   ///< `<def>|<path>|<row id>` -> Unit.
    std::map<TimelineDef*, double> m_durations;  ///< Before any row was replaced.
    std::set<TimelineDef*> m_lowered;
    std::set<std::string> m_declared;           ///< `<scope>|<timeline>|<path>|<row id>` of every entry seen.
    std::vector<size_t> m_stack;                ///< The walk's stack.
    std::vector<RuntimeExpression>* m_runtime;  ///< T-777: the entries compiled to run-time programs.
    ExpressionOptions m_options;
    std::map<size_t, size_t> m_runtimeOfUnit;  ///< T-777: Unit -> its RuntimeExpression.

    static std::string rowKey(const TimelineDef* def, const std::string& path, const ResolvedRow& row) {
        return std::to_string(reinterpret_cast<uintptr_t>(def)) + "|" + path + "|" + row.id();
    }

    /// A new outcome slot for `entry`.
    size_t newOutcome(const ExpressionSource& source, const ExpressionEntry& entry,
                      const std::string& scopePath) {
        ExpressionOutcome o;
        o.file = source.file;
        o.line = entry.line;
        o.symbol = source.symbol;
        o.scopePath = scopePath;
        o.timeline = entry.timeline;
        o.element = entry.element;
        o.property = entry.row();
        m_out->push_back(std::move(o));
        return m_out->size() - 1;
    }

    /// An `expr.*` note for the entry (not counted by `--strict`), also kept in outcome `o`.
    void note(size_t o, const std::string& file, int line, int col, const std::string& code,
              const std::string& message) {
        m_sink.note(file, message, line, col, code);
        (*m_out)[o].diagnostics.push_back(
            Diagnostic{Diagnostic::Level::Warning, file, line, col, message, code});
    }

    void entryNote(size_t o, const ExpressionSource& source, const ExpressionEntry& entry,
                   const std::string& code, const std::string& message) {
        note(o, source.file, entry.line, entry.col, code, message);
    }

    static std::string label(const ExpressionEntry& entry) {
        return "expression \"" + entry.timeline + " #" + entry.element + " " + entry.row() + "\"";
    }

    /// The timelines `entry` applies to, as (TimelineDef, scope root, instance path) triples.
    struct Target {
        TimelineDef* def;
        uint32_t scopeRoot;
        std::string scopePath;
    };

    TimelineDef* findTimeline(const std::string& name, const std::string& scopePath) {
        for (TimelineDef& def : m_rules.timelines) {
            if (def.name == name && def.scopePath == scopePath) {
                return &def;
            }
        }
        return nullptr;
    }

    // ---- collecting ------------------------------------------------------------------------------

    void collect(const ExpressionSource& source, const ExpressionEntry& entry) {
        // The row is checked once without knowing the element (a shape inside an inline <svg> takes
        // more rows), and again per target.
        ResolvedRow probe;
        std::string message;
        bool rowOk = resolveRow(entry.property, entry.component, false, probe, message);
        if (!rowOk) {
            std::string unused;
            rowOk = resolveRow(entry.property, entry.component, true, probe, unused);
        }
        std::vector<Target> targets;
        bool anyInstance = false;
        if (source.symbol.empty()) {
            if (TimelineDef* def = findTimeline(entry.timeline, std::string())) {
                targets.push_back(Target{def, 0xFFFFFFFFu, std::string()});
            }
        } else {
            for (const ExpandedSymbolInstance& inst : m_instances) {
                if (inst.symbol != source.symbol || inst.instanceNode >= m_dom.nodes.size()) {
                    continue;
                }
                anyInstance = true;
                const std::string instanceId = idOf(m_dom.nodes[inst.instanceNode]);
                if (TimelineDef* def = findTimeline(entry.timeline, instanceId)) {
                    targets.push_back(Target{def, inst.instanceNode, instanceId});
                }
            }
            if (!anyInstance) {
                return;  // No instance of the symbol: nothing to animate (as for its timelines).
            }
        }
        if (!rowOk) {
            entryNote(newOutcome(source, entry, std::string()), source, entry, "expr.property",
                      label(entry) + ": " + message + "; the entry is ignored");
            return;
        }
        if (targets.empty()) {
            entryNote(newOutcome(source, entry, std::string()), source, entry, "expr.timeline",
                      label(entry) + ": no @timeline \"" + entry.timeline + "\"" +
                          (source.symbol.empty() ? std::string() : " in symbol " + source.symbol) +
                          "; the entry is ignored");
            return;
        }
        const xe::ParseResult* parsed = nullptr;
        for (const Target& target : targets) {
            const size_t o = newOutcome(source, entry, target.scopePath);
            Unit unit;
            unit.source = &source;
            unit.entry = &entry;
            unit.def = target.def;
            unit.scopeRoot = target.scopeRoot;
            unit.scopePath = target.scopePath;
            unit.outcome = o;
            unit.path = target.scopePath.empty() ? entry.element : target.scopePath + "/" + entry.element;
            unit.element = findElement(m_dom, unit.path);
            if (unit.element == 0xFFFFFFFFu) {
                unit.element = findSvgOwner(m_dom, unit.path);
                unit.svg = unit.element != 0xFFFFFFFFu;
            }
            if (unit.element == 0xFFFFFFFFu) {
                entryNote(
                    o, source, entry, "expr.element",
                    label(entry) + ": no element with id \"" + entry.element + "\"; the entry is ignored");
                continue;
            }
            if (!resolveRow(entry.property, entry.component, unit.svg, unit.row, message)) {
                entryNote(o, source, entry, "expr.property",
                          label(entry) + ": " + message + "; the entry is ignored");
                continue;
            }
            const std::string key =
                target.scopePath + "|" + entry.timeline + "|" + unit.path + "|" + unit.row.id();
            if (!m_declared.insert(key).second) {
                entryNote(o, source, entry, "expr.duplicate",
                          label(entry) + ": the row already has an expression; this one is ignored");
                continue;
            }
            if (entry.off) {
                (*m_out)[o].status = ExpressionOutcome::Status::Off;
                continue;
            }
            if (parsed == nullptr) {
                m_parsed.push_back(xe::parseExpression(entry.text));
                parsed = &m_parsed.back();
            }
            if (!parsed->ok()) {
                for (const Diagnostic& d : parsed->diagnostics) {
                    engineNote(o, source, entry, d);
                }
                continue;
            }
            unit.program = &parsed->program;
            auto [it, inserted] = m_durations.try_emplace(target.def, 0.0);
            if (inserted) {
                it->second = timelineDuration(*target.def);
            }
            m_unitByRow.emplace(rowKey(target.def, unit.path, unit.row), m_units.size());
            m_units.push_back(std::move(unit));
        }
    }

    /// An engine diagnostic, moved from the expression text to the file.
    void engineNote(size_t o, const ExpressionSource& source, const ExpressionEntry& entry,
                    const Diagnostic& d, const std::string* code = nullptr,
                    const std::string* message = nullptr) {
        const int line = entry.textLine + std::max(d.line, 1) - 1;
        const int col = d.line <= 1 ? entry.textCol + std::max(d.col, 1) - 1 : std::max(d.col, 1);
        note(o, source.file, line, col,
             code != nullptr  ? *code
             : d.code.empty() ? std::string("expr.eval")
                              : d.code,
             label(entry) + ": " + (message != nullptr ? *message : d.message) +
                 "; the row keeps its keyframes");
    }

    // ---- ordering --------------------------------------------------------------------------------

    void walk(size_t root) {
        m_stack.assign(1, root);
        m_units[root].state = State::Active;
        while (!m_stack.empty()) {
            const size_t u = m_stack.back();
            Unit& unit = m_units[u];
            if (unit.cycle) {
                entryNote(unit.outcome, *unit.source, *unit.entry, "expr.cycle",
                          label(*unit.entry) + ": its references form a cycle (" + unit.cycleText +
                              "); every entry in the cycle is skipped and its row keeps its keyframes");
                finish(u);
                continue;
            }
            Attempt attempt;
            lowerUnit(unit, attempt);
            if (attempt.deferTo != SIZE_MAX) {
                m_units[attempt.deferTo].state = State::Active;
                m_stack.push_back(attempt.deferTo);
                continue;
            }
            if (attempt.cycleWith != SIZE_MAX) {
                markCycle(attempt.cycleWith);
                continue;
            }
            finish(u);
        }
    }

    void finish(size_t u) {
        m_units[u].state = State::Done;
        m_stack.pop_back();
    }

    /// Marks every Unit of the stack from `from` up as part of one cycle.
    void markCycle(size_t from) {
        const auto first = std::find(m_stack.begin(), m_stack.end(), from);
        std::string text;
        for (auto it = first; it != m_stack.end(); ++it) {
            const Unit& unit = m_units[*it];
            text += "#" + unit.entry->element + " " + unit.entry->row() + " -> ";
        }
        text += "#" + m_units[from].entry->element + " " + m_units[from].entry->row();
        for (auto it = first; it != m_stack.end(); ++it) {
            m_units[*it].cycle = true;
            m_units[*it].cycleText = text;
        }
    }

    // ---- references ------------------------------------------------------------------------------

    /// What `layer(key)[.<name>]` names for `unit`'s expression.
    struct Located {
        std::string path;                ///< The element's path.
        uint32_t element = 0xFFFFFFFFu;  ///< Its DomTree node (the `<svg>` for inline `<svg>` content).
        bool svg = false;                ///< A shape inside an inline `<svg>`.
        ResolvedRow row;                 ///< With `name`: the row.
        std::string rowName;             ///< Its canonical name.
    };

    /// Finds `layer(key)` and (when `name` is not empty) its row `name`; false with `message`.
    bool locate(const Unit& unit, const xe::ExprLayerKey& key, std::string_view name, Located& out,
                std::string& message) {
        if (key.byIndex) {
            if (unit.svg) {
                message =
                    "a shape inside an inline <svg> has no sibling index; name the row, e.g. "
                    "layer(\"<svg id>/<id>\")";
                return false;
            }
            const uint32_t parent = m_dom.nodes[unit.element].parent;
            const bool alone = unit.element == unit.scopeRoot || parent >= m_dom.nodes.size();
            const size_t count = alone ? 1 : m_dom.nodes[parent].children.size();
            if (key.index < 1) {
                message = key.index == 0 ? "there is no sibling before the first one; guard it, e.g. "
                                           "index > 1 ? layer(index - 1).y : value"
                                         : "sibling indices start at 1";
                return false;
            }
            if (static_cast<size_t>(key.index) > count) {
                message = "the element has " + std::to_string(count) +
                          (count == 1 ? " sibling (itself)" : " siblings, counting itself");
                return false;
            }
            out.element =
                alone ? unit.element : m_dom.nodes[parent].children[static_cast<size_t>(key.index) - 1];
            out.path = idOf(m_dom.nodes[out.element]);
            if (out.path.empty()) {
                message = "sibling " + std::to_string(key.index) + " has no id, so no rows";
                return false;
            }
        } else {
            out.path = unit.scopePath.empty() ? key.name : unit.scopePath + "/" + key.name;
            out.element = findElement(m_dom, out.path);
            if (out.element == 0xFFFFFFFFu) {
                out.element = findSvgOwner(m_dom, out.path);
                out.svg = out.element != 0xFFFFFFFFu;
            }
            if (out.element == 0xFFFFFFFFu && !unit.scopePath.empty()) {
                // Outside the symbol instance: never a row of the instance's timeline.
                out.element = findElement(m_dom, key.name);
                out.path = key.name;
            }
            if (out.element == 0xFFFFFFFFu) {
                message = "no element with id \"" + key.name + "\"";
                return false;
            }
        }
        if (name.empty()) {
            return true;
        }
        std::string property;
        std::string component;
        if (!referencedRow(name, property, component, message) ||
            !resolveRow(property, component, out.svg, out.row, message)) {
            return false;
        }
        out.rowName = component.empty() ? property : property + "." + component;
        return true;
    }

    /// T-777: the Unit of `layer(key).<name>` when it is a row of `unit`'s timeline compiled to a
    /// run-time program; SIZE_MAX otherwise.
    size_t runtimeRowUnit(const Unit& unit, const xe::ExprLayerKey& key, std::string_view name) {
        Located loc;
        std::string message;
        if (!locate(unit, key, name, loc, message)) {
            return SIZE_MAX;
        }
        const auto dep = m_unitByRow.find(rowKey(unit.def, loc.path, loc.row));
        return dep != m_unitByRow.end() && m_runtimeOfUnit.count(dep->second) != 0 ? dep->second : SIZE_MAX;
    }

    /// `layer(key).<name>` for `unit`'s expression (ExprContext::otherRows).
    xe::ExprRowRef resolveRef(const Unit& unit, Attempt& a, const xe::ExprLayerKey& key,
                              std::string_view name) {
        if (a.abandoned()) {
            return {};
        }
        const std::string written = "layer(" +
                                    (key.byIndex ? std::to_string(key.index) : "\"" + key.name + "\"") +
                                    ")." + std::string(name);
        auto problem = [&](const std::string& message) {
            a.problems.emplace_back("expr.ref", written + ": " + message);
            return xe::ExprRowRef{};
        };
        Located loc;
        std::string message;
        if (!locate(unit, key, name, loc, message)) {
            return problem(message);
        }
        const std::string& refPath = loc.path;
        const ResolvedRow& row = loc.row;
        const std::string& rowName = loc.rowName;
        const auto dep = m_unitByRow.find(rowKey(unit.def, refPath, row));
        if (dep != m_unitByRow.end()) {
            const Unit& other = m_units[dep->second];
            if (&other == &unit || other.state == State::Active) {
                a.cycleWith = dep->second;
                return {};
            }
            if (other.state == State::Pending) {
                a.deferTo = dep->second;
                return {};
            }
            if (m_runtimeOfUnit.count(dep->second) != 0) {
                // T-777: a row whose own expression runs at run time is a run-time input too.
                xe::ExprRowRef runtime;
                runtime.kind = xe::ExprRefKind::Runtime;
                return runtime;
            }
        }
        std::vector<RowSample> samples;
        const RowRead read = row.isEffect() ? readEffectRow(*unit.def, refPath, row, samples)
                                            : readRow(*unit.def, refPath, row.prop, samples);
        if (read == RowRead::NotNumeric) {
            return problem("#" + refPath + "'s " + rowName + " is not a plain number");
        }
        if (read == RowRead::MixedUnits) {
            return problem("#" + refPath + "'s " + rowName + " keys mix units");
        }
        if (read == RowRead::Empty) {
            // T-770: not keyed in this timeline; T-777 reads the property at run time (PushProp).
            const ExpressionOutcome::RuntimeRef ref{
                key.byIndex ? std::to_string(key.index) : "\"" + key.name + "\"", refPath, rowName};
            const bool seen = std::any_of(a.runtimeRefs.begin(), a.runtimeRefs.end(), [&](const auto& r) {
                return r.key == ref.key && r.element == ref.element && r.row == ref.row;
            });
            if (!seen) {
                a.runtimeRefs.push_back(ref);
            }
            xe::ExprRowRef runtime;
            runtime.kind = xe::ExprRefKind::Runtime;
            return runtime;
        }
        a.rows.push_back(exprKeys(samples));
        xe::ExprRowRef found;
        found.kind = xe::ExprRefKind::Row;
        found.row = xe::ExprRow{a.rows.back(), m_rules.easingPoints};
        return found;
    }

    /// Reports an entry the engine could not lower or compile (the row keeps its keys).
    void reportFailure(const Unit& unit, const Attempt& a, const std::vector<Diagnostic>& diags) {
        size_t next = 0;  // The engine's expr.ref diagnostics, told with what the resolver found.
        for (const Diagnostic& d : diags) {
            if (d.code == "expr.ref" && next < a.problems.size()) {
                engineNote(unit.outcome, *unit.source, *unit.entry, d, &a.problems[next].first,
                           &a.problems[next].second);
                ++next;
            } else {
                engineNote(unit.outcome, *unit.source, *unit.entry, d);
            }
        }
    }

    static RuntimeRowAddr addrOf(const ResolvedRow& row) {
        return RuntimeRowAddr{row.prop, row.effect, row.param};
    }

    /// T-777: records `unit`'s run-time program (or reports why it has none). The row keeps its keys
    /// (a row without keys gets one with its static value, so the program has a track to run on).
    void finishRuntime(const Unit& unit, const Attempt& a, bool inputs, xe::RuntimeProgram program,
                       const std::vector<RowSample>& samples, bool synth) {
        const ExpressionSource& source = *unit.source;
        const ExpressionEntry& entry = *unit.entry;
        const size_t o = unit.outcome;
        ExpressionOutcome& out = (*m_out)[o];
        out.runtimeInputs = inputs;
        out.ports = program.ports;
        out.runtimeRefs = a.runtimeRefs;
        out.status = ExpressionOutcome::Status::Skipped;
        auto skip = [&](const std::string& code, const std::string& message) {
            entryNote(o, source, entry, code,
                      label(entry) + ": " + message + "; the row keeps its keyframes");
        };
        if (program.status != xe::ExprStatus::Ok) {
            reportFailure(unit, a, program.diagnostics);
            return;
        }
        RuntimeExpression r;
        r.outcome = o;
        r.file = source.file;
        r.line = entry.line;
        r.col = entry.col;
        r.label = label(entry);
        r.def = unit.def;
        r.element = unit.element;
        r.row = addrOf(unit.row);
        // T-781: a shape inside an inline <svg> runs on its scene track (TimelineDef::sceneRows).
        r.svgPath = unit.svg ? unit.path : std::string();
        r.seed = expressionSeed(entry.timeline, unit.path, entry.property, entry.component);
        r.index = unit.svg ? 1.0f : static_cast<float>(siblingIndex(m_dom, unit.element, unit.scopeRoot));
        for (const xe::RuntimeSymbol& sym : program.symbols) {
            RuntimeExpression::Target t;
            if (sym.kind != xe::RuntimeSymbol::Kind::Port) {
                Located loc;
                std::string message;
                const bool layout = sym.kind == xe::RuntimeSymbol::Kind::Layout;
                if (!locate(unit, sym.key, layout ? std::string_view() : std::string_view(sym.name), loc,
                            message)) {
                    skip("expr.ref", message);
                    return;
                }
                if (loc.svg) {
                    skip("expr.limit",
                         "a shape inside an inline <svg> (" + loc.path + ") cannot be read at run time");
                    return;
                }
                t.element = loc.element;
                t.row = addrOf(loc.row);
                if (sym.kind == xe::RuntimeSymbol::Kind::RuntimeRow) {
                    const auto it = m_runtimeOfUnit.find(runtimeRowUnit(unit, sym.key, sym.name));
                    if (it == m_runtimeOfUnit.end()) {
                        skip("expr.ref", "layer(...)." + sym.name + " has no run-time program");
                        return;
                    }
                    t.runtimeExpr = it->second;
                }
            }
            r.targets.push_back(t);
        }
        r.program = std::move(program);
        if (synth) {
            replaceRow(*unit.def, unit.path, unit.row.prop, samples.front().value.unit,
                       {xe::LoweredKey{0.0, samples.front().value.value, xe::LoweredInterp::Linear, {}}},
                       entry.line);
        }
        out.status = ExpressionOutcome::Status::NeedsRuntime;
        m_runtimeOfUnit[static_cast<size_t>(&unit - m_units.data())] = m_runtime->size();
        m_runtime->push_back(std::move(r));
    }

    // ---- lowering --------------------------------------------------------------------------------

    /// Reads the keys of `prop` on `path`, or the static value at 0 s when it has none (`synth`; not
    /// for inline `<svg>` content, whose static values the compiler does not read here).
    RowRead rowKeys(TimelineDef& def, const std::string& path, uint32_t element, bool svg, uint16_t prop,
                    std::vector<RowSample>& samples, bool& synth) {
        synth = false;
        const RowRead read = readRow(def, path, prop, samples);
        if (read != RowRead::Empty || svg) {
            return read;
        }
        std::vector<StyleDecl> scratch;
        const StyleDecl* decl = staticWinningDecl(m_rules, m_dom, element, prop, def.fileName, scratch, true);
        const StyleValue value = decl != nullptr ? decl->value : defaultValue(prop);
        RowSample sample;
        sample.arrival = Easing::linear();
        if (!numericOf(value, sample.value)) {
            return RowRead::NotNumeric;
        }
        samples.push_back(sample);
        synth = true;
        return RowRead::Ok;
    }

    /// T-780: `unit` (with run-time inputs) sampled with the stand-ins of ExpressionOptions::inputs at
    /// kExprSampleRate through the compile-time evaluator (evaluateExpression()) and merged where the
    /// samples are collinear within the row's tolerance; the keys replace the row's as for a lowered
    /// entry (outcome Lowered, `runtimeInputs` true, `ports` the ports read). T-781: when the
    /// evaluation at 0 s calls posterizeTime(fps) (lowerExpression()'s condition for its Hold keys),
    /// sampled at k / fps instead and written as Hold keys (equal neighbours merged).
    void lowerWithInputs(const Unit& unit, Attempt& a, xe::ExprContext ctx,
                         const std::vector<RowSample>& samples, bool synth) {
        const ExpressionInputs& in = *m_options.inputs;
        std::vector<std::string> ports;
        xe::ExprRuntimeInputs stand;
        stand.port = [&in, &ports](std::string_view name, double t) {
            if (std::find(ports.begin(), ports.end(), name) == ports.end()) {
                ports.emplace_back(name);
            }
            return in.port ? in.port(name, t) : 0.0;
        };
        stand.layout = [this, &unit, &in](const xe::ExprLayerKey& key, int which) {
            Located loc;
            std::string message;
            return in.layout && locate(unit, key, std::string_view(), loc, message)
                       ? in.layout(loc.path, which)
                       : 0.0;
        };
        stand.layerProp = [this, &unit, &in](const xe::ExprLayerKey& key, std::string_view name, double t) {
            Located loc;
            std::string message;
            return in.row && locate(unit, key, name, loc, message) ? in.row(loc.path, loc.rowName, t) : 0.0;
        };
        stand.playRandom = [&in](uint32_t /*site*/, double lo, double hi) {
            return in.playRandom ? in.playRandom(lo, hi) : lo;
        };
        ctx.runtimeInputs = &stand;
        const xe::EvalResult first = xe::evaluateExpression(*unit.program, ctx, 0.0);
        if (a.abandoned()) {
            return;
        }
        const bool hold = first.status == xe::ExprStatus::Ok && first.posterized;
        const double fps = hold ? first.posterizeFps : xe::kExprSampleRate;
        size_t count = 0;
        if (hold) {
            const double steps = fps > 0 ? std::floor(ctx.duration * fps + 1e-9) : 0.0;
            if (steps > 20000.0) {  // expr_lower.cpp's kMaxPosterizedKeys.
                entryNote(unit.outcome, *unit.source, *unit.entry, "expr.limit",
                          label(*unit.entry) +
                              ": posterizeTime's rate gives more than 20000 keys over this timeline; the row "
                              "keeps its keyframes");
                return;
            }
            count = static_cast<size_t>(steps);
        } else if (ctx.duration > 0) {
            count = static_cast<size_t>(std::ceil(ctx.duration * xe::kExprSampleRate - 1e-9));
        }
        std::vector<std::pair<double, double>> points;
        points.reserve(count + 1);
        for (size_t k = 0; k <= count; ++k) {
            // A posterized row: its steps (the last at or before the end); else the end itself last.
            const double t = hold ? (fps > 0 ? static_cast<double>(k) / fps : 0.0)
                                  : (k >= count ? ctx.duration : static_cast<double>(k) / fps);
            const xe::EvalResult r = xe::evaluateExpression(*unit.program, ctx, t);
            if (a.abandoned()) {
                return;
            }
            std::vector<Diagnostic> diags = r.diagnostics;
            double v = 0;
            if (r.status != xe::ExprStatus::Ok || !xe::componentOf(r.value, ctx.component, v, diags)) {
                reportFailure(unit, a, diags);
                if (diags.empty()) {
                    entryNote(unit.outcome, *unit.source, *unit.entry, "expr.eval",
                              label(*unit.entry) + ": produced no keys; the row keeps its keyframes");
                }
                return;
            }
            points.emplace_back(t, v);
        }
        // Ramer-Douglas-Peucker on the vertical error, as 58 §58.3's sampled lowering.
        const double tol = std::max(0.0, ctx.tolerance) * 0.95;
        if (hold) {
            // T-781: Hold keys, a step equal to the one before (within the tolerance) merged into it.
            std::vector<std::pair<double, double>> steps;
            for (const auto& point : points) {
                if (steps.empty() || std::fabs(point.second - steps.back().second) > tol) {
                    steps.push_back(point);
                }
            }
            points = std::move(steps);
        }
        std::vector<bool> keep(points.size(), hold || points.size() <= 2);
        if (!points.empty()) {
            keep.front() = true;
            keep.back() = true;
        }
        std::vector<std::pair<size_t, size_t>> spans;
        if (points.size() > 2 && !hold) {
            spans.emplace_back(0, points.size() - 1);
        }
        while (!spans.empty()) {
            const auto [i0, i1] = spans.back();
            spans.pop_back();
            double worst = -1;
            size_t at = i0;
            for (size_t i = i0 + 1; i < i1; ++i) {
                const double p = (points[i].first - points[i0].first) / (points[i1].first - points[i0].first);
                const double line = points[i0].second + (points[i1].second - points[i0].second) * p;
                const double err = std::fabs(points[i].second - line);
                if (err > worst) {
                    worst = err;
                    at = i;
                }
            }
            if (worst > tol) {
                keep[at] = true;
                if (at - i0 > 1) {
                    spans.emplace_back(i0, at);
                }
                if (i1 - at > 1) {
                    spans.emplace_back(at, i1);
                }
            }
        }
        const xe::LoweredInterp interp = hold ? xe::LoweredInterp::Hold : xe::LoweredInterp::Linear;
        std::vector<xe::LoweredKey> keys;
        for (size_t i = 0; i < points.size(); ++i) {
            if (keep[i]) {
                keys.push_back(xe::LoweredKey{points[i].first, points[i].second, interp,
                                              hold ? Easing::hold() : Easing::linear()});
            }
        }
        if (synth) {
            entryNote(unit.outcome, *unit.source, *unit.entry, "expr.static",
                      label(*unit.entry) +
                          ": the row has no keys in this timeline; its static value at 0 s is used");
        }
        TimelineDef& def = *unit.def;
        if (unit.row.isEffect()) {
            replaceEffectRow(def, unit.path, unit.row, keys, unit.entry->line);
        } else {
            replaceRow(def, unit.path, unit.row.prop, samples.front().value.unit, keys, unit.entry->line);
        }
        m_lowered.insert(&def);
        ExpressionOutcome& out = (*m_out)[unit.outcome];
        out.status = ExpressionOutcome::Status::Lowered;
        out.runtimeInputs = true;
        out.ports = std::move(ports);
        out.runtimeRefs = a.runtimeRefs;
        for (const xe::LoweredKey& k : keys) {
            ExpressionOutcome::Key key;
            key.time = k.time;
            key.value = k.value;
            key.interp = hold ? ExpressionOutcome::Interp::Hold : ExpressionOutcome::Interp::Linear;
            key.easing = hold ? Easing::hold() : Easing::linear();
            out.keys.push_back(std::move(key));
        }
    }

    /// One attempt at lowering `unit`; with `a` abandoned, nothing is reported or changed.
    void lowerUnit(const Unit& unit, Attempt& a) {
        const ExpressionSource& source = *unit.source;
        const ExpressionEntry& entry = *unit.entry;
        const size_t o = unit.outcome;
        TimelineDef& def = *unit.def;
        const ResolvedRow& row = unit.row;
        if (row.prop == prop::TranslateX || row.prop == prop::TranslateY) {
            for (const TimelineTrackDef& track : def.tracks) {
                for (const TimelineKey& key : track.keys) {
                    if (track.path == unit.path && key.hasMotion) {
                        entryNote(o, source, entry, "expr.motion",
                                  label(entry) +
                                      ": the element's translate keys carry a motion path "
                                      "(motion-in / motion-out); the entry is ignored");
                        return;
                    }
                }
            }
        }
        std::vector<RowSample> samples;
        bool synth = false;
        const RowRead read = row.isEffect()
                                 ? readEffectRow(def, unit.path, row, samples)
                                 : rowKeys(def, unit.path, unit.element, unit.svg, row.prop, samples, synth);
        if (read == RowRead::NotNumeric) {
            entryNote(o, source, entry, "expr.property",
                      label(entry) +
                          (row.isEffect()
                               ? ": the effect parameter's keys are not one number each (a color or a "
                                 "whole vec2: write the expression on .x or .y)"
                               : ": the row's value is not a plain number (a var(), calc() or keyword)") +
                          "; the entry is ignored");
            return;
        }
        if (read == RowRead::MixedUnits) {
            entryNote(o, source, entry, "expr.units",
                      label(entry) + ": the row's keys mix units; the entry is ignored");
            return;
        }
        if (read == RowRead::Empty) {
            std::string why;
            if (row.isEffect() && effectKeyedWhole(def, unit.path, row)) {
                why =
                    ": the timeline keys this vec2 parameter whole (<x> <y>); key its .x and .y separately "
                    "to "
                    "put an expression on one of them";
            } else if (row.isEffect()) {
                why =
                    ": the effect parameter has no keys in this timeline (rows are matched as written: "
                    "effect.<index> and effect.<name> are different spellings); key it once first";
            } else {
                why =
                    ": a shape inside an inline <svg> has no static value the compiler can read; key the row "
                    "once in this timeline first";
            }
            entryNote(o, source, entry, "expr.property", label(entry) + why + "; the entry is ignored");
            return;
        }
        const std::vector<xe::ExprKey> keys = exprKeys(samples);
        // The component rows `valueVec` reads (a missing one holds its static value, silently).
        std::vector<std::vector<xe::ExprKey>> vecKeys;
        std::vector<xe::ExprRow> vecRows;
        if (row.vec != nullptr) {
            for (uint16_t p : row.vec->props) {
                if (p == kNone) {
                    break;
                }
                std::vector<RowSample> s;
                bool unused = false;
                if (rowKeys(def, unit.path, unit.element, unit.svg, p, s, unused) != RowRead::Ok) {
                    Numeric fallback;
                    numericOf(defaultValue(p), fallback);
                    s.assign(1, RowSample{0, unit.svg ? fallback : Numeric{}, Easing::linear()});
                }
                vecKeys.push_back(exprKeys(s));
            }
            for (const std::vector<xe::ExprKey>& k : vecKeys) {
                vecRows.push_back(xe::ExprRow{k, m_rules.easingPoints});
            }
        }
        xe::ExprContext ctx;
        ctx.row = xe::ExprRow{keys, m_rules.easingPoints};
        ctx.duration = m_durations[unit.def];
        ctx.index = unit.svg ? 1 : siblingIndex(m_dom, unit.element, unit.scopeRoot);
        ctx.seed = expressionSeed(entry.timeline, unit.path, entry.property, entry.component);
        ctx.vecRows = vecRows;
        ctx.component = row.component;
        ctx.tolerance = xe::exprTolerance(row.kind);
        ctx.otherRows = [this, &unit, &a](const xe::ExprLayerKey& key, std::string_view name) {
            return resolveRef(unit, a, key, name);
        };
        const xe::LowerResult lowered = xe::lowerExpression(*unit.program, ctx);
        if (a.abandoned()) {
            return;
        }
        // T-780: the editor's bake compile gives stand-ins for the run-time inputs: keys, no program.
        if (lowered.status == xe::ExprStatus::NeedsRuntime && m_options.inputs != nullptr) {
            lowerWithInputs(unit, a, ctx, samples, synth);
            return;
        }
        // T-777 (58 §58.5): run-time inputs -- a program of the IR's TEXP chunk.
        const bool runtime =
            m_runtime != nullptr && (lowered.status == xe::ExprStatus::NeedsRuntime ||
                                     (m_options.allAtRunTime && lowered.status == xe::ExprStatus::Ok));
        xe::RuntimeProgram program;
        if (runtime) {
            xe::RuntimeCompileOptions options;
            options.forceOps = m_options.allAtRunTime;
            options.runtimeRow = [this, &unit](const xe::ExprLayerKey& key, std::string_view name) {
                return runtimeRowUnit(unit, key, name) != SIZE_MAX;
            };
            program = xe::compileRuntime(*unit.program, ctx, options);
            if (a.abandoned()) {
                return;
            }
        }
        if (synth) {
            entryNote(
                o, source, entry, "expr.static",
                label(entry) + ": the row has no keys in this timeline; its static value at 0 s is used");
        }
        ExpressionOutcome& out = (*m_out)[o];
        if (runtime) {
            finishRuntime(unit, a, lowered.status == xe::ExprStatus::NeedsRuntime, std::move(program),
                          samples, synth);
            return;
        }
        if (lowered.status == xe::ExprStatus::NeedsRuntime) {
            out.status = ExpressionOutcome::Status::Skipped;
            out.runtimeInputs = true;
            entryNote(
                o, source, entry, "expr.runtime",
                label(entry) +
                    ": reads a run-time input, which is not compiled here; the row keeps its keyframes");
            return;
        }
        if (lowered.status != xe::ExprStatus::Ok || lowered.keys.empty()) {
            reportFailure(unit, a, lowered.diagnostics);
            if (lowered.diagnostics.empty()) {
                entryNote(o, source, entry, "expr.eval",
                          label(entry) + ": produced no keys; the row keeps its keyframes");
            }
            return;
        }
        if (row.isEffect()) {
            replaceEffectRow(def, unit.path, row, lowered.keys, entry.line);
        } else {
            replaceRow(def, unit.path, row.prop, samples.front().value.unit, lowered.keys, entry.line);
        }
        m_lowered.insert(&def);
        out.status = ExpressionOutcome::Status::Lowered;
        for (const xe::LoweredKey& k : lowered.keys) {
            ExpressionOutcome::Key key;
            key.time = k.time;
            key.value = k.value;
            key.interp = static_cast<ExpressionOutcome::Interp>(k.interp);
            // T-773: the full easing, as replaceKeys() writes it (a LinearPoints easing with its points).
            key.easing = k.interp == xe::LoweredInterp::Hold     ? Easing::hold()
                         : k.interp == xe::LoweredInterp::Easing ? k.easing
                                                                 : Easing::linear();
            if (key.easing.kind == EasingKind::LinearPoints) {
                const size_t at = key.easing.pointOffset;
                const size_t n = key.easing.pointCount;
                if (at + n <= m_rules.easingPoints.size()) {
                    key.easingPoints.assign(
                        m_rules.easingPoints.begin() + static_cast<std::ptrdiff_t>(at),
                        m_rules.easingPoints.begin() + static_cast<std::ptrdiff_t>(at + n));
                }
                key.easing.pointOffset = 0;
                key.easing.pointCount = static_cast<uint16_t>(key.easingPoints.size());
            }
            out.keys.push_back(std::move(key));
        }
    }

    /// Removes the row from every key of `path` in `def` (`drop` says which declarations; a key left
    /// with nothing is dropped) and adds the lowered keys as one more track of `path` (`fill` writes
    /// one key's declaration).
    template <typename Drop, typename Fill>
    static void replaceKeys(TimelineDef& def, const std::string& path,
                            const std::vector<xe::LoweredKey>& lowered, int line, Drop drop, Fill fill) {
        for (TimelineTrackDef& track : def.tracks) {
            if (track.path != path) {
                continue;
            }
            for (TimelineKey& key : track.keys) {
                drop(key);
            }
            std::erase_if(track.keys,
                          [](const TimelineKey& k) { return k.decls.empty() && k.effectDecls.empty(); });
        }
        TimelineTrackDef track;
        track.path = path;
        track.line = line;
        for (size_t i = 0; i < lowered.size(); ++i) {
            TimelineKey key;
            key.time = static_cast<float>(lowered[i].time);
            key.line = line;
            if (i > 0) {
                const xe::LoweredKey& prev = lowered[i - 1];
                key.hasEasing = true;
                key.easing = prev.interp == xe::LoweredInterp::Hold     ? Easing::hold()
                             : prev.interp == xe::LoweredInterp::Easing ? prev.easing
                                                                        : Easing::linear();
            }
            fill(key, lowered[i].value);
            track.keys.push_back(std::move(key));
        }
        def.tracks.push_back(std::move(track));
    }

    static void replaceRow(TimelineDef& def, const std::string& path, uint16_t prop, int unit,
                           const std::vector<xe::LoweredKey>& lowered, int line) {
        replaceKeys(
            def, path, lowered, line,
            [prop](TimelineKey& key) {
                std::erase_if(key.decls, [prop](const StyleDecl& d) { return d.prop == prop; });
            },
            [prop, unit, line](TimelineKey& key, double value) {
                StyleDecl decl;
                decl.prop = prop;
                decl.value = valueOf(value, unit);
                decl.line = line;
                key.decls.push_back(std::move(decl));
            });
    }

    static void replaceEffectRow(TimelineDef& def, const std::string& path, const ResolvedRow& row,
                                 const std::vector<xe::LoweredKey>& lowered, int line) {
        replaceKeys(
            def, path, lowered, line,
            [&row](TimelineKey& key) {
                std::erase_if(key.effectDecls, [&row](const TimelineEffectDecl& d) {
                    return d.effect == row.effect && d.param == row.param;
                });
            },
            [&row, line](TimelineKey& key, double value) {
                key.effectDecls.push_back(
                    TimelineEffectDecl{row.effect, row.param, effectValueText(value), line});
            });
    }
};

}  // namespace

void applyExpressions(const std::vector<ExpressionSource>& sources, RuleSet& ruleSet, const DomTree& dom,
                      const std::vector<ExpandedSymbolInstance>& instances, DiagnosticSink& sink,
                      std::vector<ExpressionOutcome>* outcomes, std::vector<RuntimeExpression>* runtime,
                      const ExpressionOptions& options) {
    if (sources.empty()) {
        return;
    }
    Lowerer lowerer(ruleSet, dom, instances, sink, outcomes, runtime, options);
    lowerer.run(sources);
}

}  // namespace xt::compile
