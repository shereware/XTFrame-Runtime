#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "migrate_v2.h"    // migrate::appendJsonString().
#include "style_decode.h"  // style_decode::styleprop numbers (Code/Runtime/core/src).
#include "xt/compile/migrate.h"
#include "xt/ir.h"
#include "xt/ir_pod.h"
#include "xt/node.h"
#include "xt/property.h"
#include "xts/xts_props.h"

/// T-589 (Docs/design/54-source-format-v2.md §54.11 layer two): dumpComputedStyle(), see
/// xt/compile/migrate.h. The cascade is the runtime's (StyleEngine::resolve, 05 §5.7 step 1-2):
/// every matching STYL rule and the element's inline declarations, ordered by importance,
/// specificity and order (rule index, then declaration order; inline after every rule); the dump
/// writes the winner of each property. Values are decoded so that two IRs that differ only in pool
/// order and string ids give the same text (strings by content, pool slices expanded, calc programs
/// written out); anything else is written as hex.
///
/// Not covered by the design (choices made here):
///   - A pseudo-class combination is one compound's state bits plus its positive `:state(X)` names;
///     it is applied to the element and every ancestor (a pointer over an element hovers its
///     ancestors too), `:focus-within` holds when the combination has focus, `:focus-player()`
///     never holds and `:lang()` reads the inherited `lang` attribute.
///   - `@media`: the base context has no condition holding; each distinct query list (by its
///     decoded text) is then dumped alone, as the properties that differ from the base.
///   - Scene-target rules (`#Icon/Blade { ... }`) are dumped as `<prop>@<target>` entries.

namespace xt::compile {

namespace {

namespace sp = style_decode::styleprop;
using migrate::appendJsonString;

uint32_t u32At(const uint8_t* bytes, int slot) {
    uint32_t v = 0;
    std::memcpy(&v, bytes + slot * 4, 4);
    return v;
}

float f32At(const uint8_t* bytes, int slot) {
    float v = 0;
    std::memcpy(&v, bytes + slot * 4, 4);
    return v;
}

std::string num(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
    return buf;
}

std::string hexOf(const void* data, size_t size) {
    static const char* const kDigits = "0123456789abcdef";
    const auto* p = static_cast<const uint8_t*>(data);
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out.push_back(kDigits[p[i] >> 4]);
        out.push_back(kDigits[p[i] & 15]);
    }
    return out;
}

const char* unitName(uint8_t unit) {
    static const char* const kNames[] = {"px",          "%",         "em",      "auto",     "vw",
                                         "vh",          "vmin",      "vmax",    "safe-top", "safe-right",
                                         "safe-bottom", "safe-left", "uiscale", "calc"};
    return unit < sizeof(kNames) / sizeof(kNames[0]) ? kNames[unit] : "unit?";
}

/// One pseudo-class combination.
struct Combo {
    uint16_t mask = 0;
    std::vector<uint32_t> custom;  ///< StringIds of positive `:state(X)` names, sorted.
    std::string key;
};

class Dumper {
public:
    explicit Dumper(const IrView& ir) : m_ir(ir), m_style(ir.style()), m_doc(ir.doc()) {}

    std::string run() {
        prepareTree();
        prepareMedia();
        prepareCombos();
        std::string out = "{\n  \"format\": \"xtframe-computed-style\",\n  \"version\": 1,\n  \"tokens\": {";
        writeTokens(out);
        out += "},\n  \"combinations\": [";
        for (size_t i = 0; i < m_combos.size(); ++i) {
            out += i == 0 ? "" : ", ";
            appendJsonString(out, m_combos[i].key);
        }
        out += "],\n  \"elements\": [";
        const auto elements = m_doc.elements();
        for (uint32_t e = 0; e < elements.size(); ++e) {
            out += e == 0 ? "\n" : ",\n";
            writeElement(out, e);
        }
        out += "\n  ]\n}\n";
        return out;
    }

private:
    using Props = std::map<std::string, std::string>;

    std::string_view str(uint32_t id) const { return m_ir.string(id); }

    // ---- tree ------------------------------------------------------------------------------

    void prepareTree() {
        const auto elements = m_doc.elements();
        m_children.assign(elements.size(), {});
        m_childIndex.assign(elements.size(), 1);
        for (uint32_t e = 0; e < elements.size(); ++e) {
            const uint32_t parent = elements[e].parent;
            if (parent < elements.size()) {
                m_children[parent].push_back(e);
                m_childIndex[e] = static_cast<uint32_t>(m_children[parent].size());
            }
        }
        m_paths.assign(elements.size(), std::string());
        for (uint32_t e = 0; e < elements.size(); ++e) {
            const uint32_t parent = elements[e].parent;
            std::string step = std::string(str(elements[e].tag));
            if (parent < elements.size()) {
                step += "[" + std::to_string(m_childIndex[e]) + "]";
                m_paths[e] = m_paths[parent] + ">" + step;
            } else {
                m_paths[e] = step;
            }
        }
    }

    bool hasClass(uint32_t e, uint32_t cls) const {
        const IrDocElement& el = m_doc.elements()[e];
        const auto pool = m_doc.classPool();
        for (uint32_t i = 0; i < el.classCount; ++i) {
            if (el.classOffset + i < pool.size() && pool[el.classOffset + i] == cls) {
                return true;
            }
        }
        return false;
    }

    bool findAttr(uint32_t e, uint32_t name, std::string_view& value) const {
        const IrDocElement& el = m_doc.elements()[e];
        const auto pool = m_doc.attrPool();
        for (uint32_t i = 0; i < el.attrCount; ++i) {
            if (el.attrOffset + i < pool.size() && str(pool[el.attrOffset + i].name) == str(name)) {
                value = str(pool[el.attrOffset + i].value);
                return true;
            }
        }
        if (el.id != kInvalidString && str(name) == "id") {
            value = str(el.id);
            return true;
        }
        return false;
    }

    std::string_view langOf(uint32_t e) const {
        const auto elements = m_doc.elements();
        for (uint32_t n = e; n < elements.size(); n = elements[n].parent) {
            const auto pool = m_doc.attrPool();
            for (uint32_t i = 0; i < elements[n].attrCount; ++i) {
                const IrAttr& a = pool[elements[n].attrOffset + i];
                if (str(a.name) == "lang") {
                    return str(a.value);
                }
            }
        }
        return {};
    }

    // ---- matching (selector_match.h's rules over DOCT, the combination on the whole chain) --

    bool matchCondition(uint32_t e, const IrSelPart& part, const Combo& combo) const {
        const auto raw = static_cast<uint8_t>(part.classCount);
        const bool negated = (raw & kIrSelCondNegated) != 0;
        const auto code = static_cast<IrSelCond>(raw & ~kIrSelCondNegated);
        const IrDocElement& el = m_doc.elements()[e];
        bool result = false;
        switch (code) {
            case IrSelCond::Tag:
                result = str(el.tag) == str(part.tag);
                break;
            case IrSelCond::Id:
                result = el.id != kInvalidString && str(el.id) == str(part.id);
                break;
            case IrSelCond::Class:
                result = hasClass(e, part.tag);
                break;
            case IrSelCond::State:
                result = part.stateMask != 0 && (part.stateMask & ~combo.mask) == 0;
                break;
            case IrSelCond::Any:
                result = true;
                break;
            case IrSelCond::AttrExists:
            case IrSelCond::AttrEquals:
            case IrSelCond::AttrPrefix:
            case IrSelCond::AttrSuffix:
            case IrSelCond::AttrSubstring: {
                std::string_view have;
                if (!findAttr(e, part.tag, have)) {
                    break;
                }
                const std::string_view want =
                    code == IrSelCond::AttrExists ? std::string_view() : str(part.id);
                if (code == IrSelCond::AttrExists) {
                    result = true;
                } else if (code == IrSelCond::AttrEquals) {
                    result = have == want;
                } else if (want.empty()) {
                    result = false;
                } else if (code == IrSelCond::AttrPrefix) {
                    result = have.substr(0, want.size()) == want;
                } else if (code == IrSelCond::AttrSuffix) {
                    result = have.size() >= want.size() && have.substr(have.size() - want.size()) == want;
                } else {
                    result = have.find(want) != std::string_view::npos;
                }
                break;
            }
            case IrSelCond::NthChild: {
                const int64_t a = static_cast<int32_t>(part.tag);
                const int64_t b = static_cast<int32_t>(part.id);
                const int64_t index = m_childIndex[e];
                if (a == 0) {
                    result = index == b;
                } else {
                    const int64_t diff = index - b;
                    result = diff % a == 0 && diff / a >= 0;
                }
                break;
            }
            case IrSelCond::FirstChild:
                result = m_childIndex[e] == 1;
                break;
            case IrSelCond::LastChild: {
                const uint32_t parent = el.parent;
                result = parent >= m_children.size() || m_children[parent].back() == e;
                break;
            }
            case IrSelCond::FocusWithin:
                result = (combo.mask & kStateFocus) != 0;
                break;
            case IrSelCond::Lang: {
                const std::string_view lang = langOf(e);
                const std::string_view prefix = str(part.tag);
                result =
                    !prefix.empty() && lang.size() >= prefix.size() &&
                    std::equal(prefix.begin(), prefix.end(), lang.begin(),
                               [](char x, char y) {
                                   auto lower = [](char c) {
                                       return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
                                   };
                                   return lower(x) == lower(y);
                               }) &&
                    (lang.size() == prefix.size() || lang[prefix.size()] == '-');
                break;
            }
            case IrSelCond::CustomState:
                result = std::find(combo.custom.begin(), combo.custom.end(), part.tag) != combo.custom.end();
                break;
            case IrSelCond::FocusPlayer:
                result = false;
                break;
        }
        return result != negated;
    }

    /// parts[idx] (a compound) and its condition parts against element `e`; `end` = next compound.
    bool matchCompound(uint32_t e, std::span<const IrSelPart> parts, uint32_t idx, const Combo& combo,
                       uint32_t& end) const {
        end = idx + 1;
        while (end < parts.size() &&
               (parts[end].combinator == kIrSelCondition || parts[end].combinator == kIrSelSceneTarget)) {
            ++end;
        }
        const IrSelPart& part = parts[idx];
        const IrDocElement& el = m_doc.elements()[e];
        if (part.tag != kInvalidString && str(part.tag) != str(el.tag)) {
            return false;
        }
        if (part.id != kInvalidString && (el.id == kInvalidString || str(part.id) != str(el.id))) {
            return false;
        }
        const auto classIds = m_style.classIds();
        for (uint32_t i = 0; i < part.classCount; ++i) {
            if (part.classOffset + i >= classIds.size() || !hasClass(e, classIds[part.classOffset + i])) {
                return false;
            }
        }
        if ((part.stateMask & ~combo.mask) != 0) {
            return false;
        }
        for (uint32_t i = idx + 1; i < end; ++i) {
            if (parts[i].combinator == kIrSelCondition && !matchCondition(e, parts[i], combo)) {
                return false;
            }
        }
        return true;
    }

    bool matchFrom(uint32_t e, std::span<const IrSelPart> parts, uint32_t idx, const Combo& combo) const {
        uint32_t end = 0;
        if (!matchCompound(e, parts, idx, combo, end)) {
            return false;
        }
        if (end >= parts.size()) {
            return true;
        }
        const uint8_t comb = parts[end].combinator;
        const auto elements = m_doc.elements();
        for (uint32_t a = elements[e].parent; a < elements.size(); a = elements[a].parent) {
            if (matchFrom(a, parts, end, combo)) {
                return true;
            }
            if (comb == kCombinatorChild2) {
                return false;
            }
        }
        return false;
    }

    static constexpr uint8_t kCombinatorChild2 = 2;

    // ---- media and combinations --------------------------------------------------------------

    void prepareMedia() {
        const MediaChunk& media = m_ir.media();
        std::vector<std::string> keys(media.media().size());
        for (size_t m = 0; m < media.media().size(); ++m) {
            const IrMedia& list = media.media()[m];
            std::string key;
            for (uint32_t q = 0; q < list.queryCount; ++q) {
                const IrMediaQuery& query = media.queries()[list.queryOffset + q];
                key += q == 0 ? "" : ", ";
                for (uint32_t c = 0; c < query.condCount; ++c) {
                    const IrMediaCond& cond = media.conds()[query.condOffset + c];
                    key += (c == 0 ? "(" : " and (") + std::to_string(cond.feature) + ":" + num(cond.value) +
                           ")";
                }
            }
            keys[m] = key;
        }
        m_ruleMedia.assign(m_style.rules().size(), std::string());
        m_ruleHasMedia.assign(m_style.rules().size(), 0);
        std::set<std::string> distinct;
        for (const IrMediaRule& r : media.rules()) {
            if (r.rule < m_ruleMedia.size() && r.media < keys.size()) {
                m_ruleMedia[r.rule] = keys[r.media];
                m_ruleHasMedia[r.rule] = 1;
                distinct.insert(keys[r.media]);
            }
        }
        m_mediaKeys.assign(distinct.begin(), distinct.end());
    }

    void prepareCombos() {
        std::map<std::string, Combo> combos;
        Combo base;
        combos.emplace(std::string(), base);
        const auto parts = m_style.selectorParts();
        for (size_t i = 0; i < parts.size();) {
            Combo combo;
            combo.mask = parts[i].combinator == kIrSelCondition ? 0 : parts[i].stateMask;
            size_t end = i + 1;
            while (end < parts.size() &&
                   (parts[end].combinator == kIrSelCondition || parts[end].combinator == kIrSelSceneTarget)) {
                const auto raw = static_cast<uint8_t>(parts[end].classCount);
                const bool negated = (raw & kIrSelCondNegated) != 0;
                const auto code = static_cast<IrSelCond>(raw & ~kIrSelCondNegated);
                if (parts[end].combinator == kIrSelCondition && !negated) {
                    if (code == IrSelCond::State) {
                        combo.mask = static_cast<uint16_t>(combo.mask | parts[end].stateMask);
                    } else if (code == IrSelCond::CustomState) {
                        combo.custom.push_back(parts[end].tag);
                    } else if (code == IrSelCond::FocusWithin) {
                        combo.mask = static_cast<uint16_t>(combo.mask | kStateFocus);
                    }
                }
                ++end;
            }
            i = end;
            std::sort(combo.custom.begin(), combo.custom.end());
            combo.custom.erase(std::unique(combo.custom.begin(), combo.custom.end()), combo.custom.end());
            if (combo.mask == 0 && combo.custom.empty()) {
                continue;
            }
            static const char* const kStates[] = {":hover", ":active", ":focus", ":disabled", ":checked"};
            for (int bit = 0; bit < 16; ++bit) {
                if ((combo.mask & (1u << bit)) != 0) {
                    combo.key += bit < 5 ? std::string(kStates[bit]) : ":state-bit" + std::to_string(bit);
                }
            }
            std::vector<std::string> names;
            for (uint32_t id : combo.custom) {
                names.emplace_back(str(id));
            }
            std::sort(names.begin(), names.end());
            for (const std::string& n : names) {
                combo.key += ":state(" + n + ")";
            }
            combos.emplace(combo.key, combo);
        }
        for (auto& [key, combo] : combos) {
            m_combos.push_back(combo);
        }
    }

    // ---- values ------------------------------------------------------------------------------

    std::string easingText(const IrEasing& easing) const { return hexOf(&easing, sizeof(easing)); }

    std::string stopsText(std::span<const IrGradientStop> pool, uint32_t offset, uint32_t count) const {
        std::string out = "[";
        for (uint32_t i = 0; i < count && offset + i < pool.size(); ++i) {
            const IrGradientStop& s = pool[offset + i];
            out += (i == 0 ? "" : ",") + num(s.offset) + " " + num(s.color.r) + " " + num(s.color.g) + " " +
                   num(s.color.b) + " " + num(s.color.a);
        }
        return out + "]";
    }

    std::string calcText(uint32_t index) const {
        const CalcChunk& calcs = m_ir.calcs();
        if (index >= calcs.calcs().size()) {
            return "calc(?)";
        }
        const IrCalc& calc = calcs.calcs()[index];
        std::string out = "calc[";
        for (uint32_t i = 0; i < calc.opCount && calc.opOffset + i < calcs.ops().size(); ++i) {
            const IrCalcOp& op = calcs.ops()[calc.opOffset + i];
            out += i == 0 ? "" : " ";
            switch (static_cast<CalcOpKind>(op.op)) {
                case CalcOpKind::PushLength:
                    out += num(op.value) + unitName(op.unit);
                    break;
                case CalcOpKind::PushNumber:
                    out += num(op.value);
                    break;
                case CalcOpKind::PushVar:
                    out += "var(" + std::string(str(op.name));
                    if (op.hasFallback != 0) {
                        out += "," + num(op.value) + (op.unit == kCalcUnitNumber ? "" : unitName(op.unit));
                    }
                    out += ")";
                    break;
                default:
                    out += "op" + std::to_string(op.op);
                    break;
            }
        }
        return out + "]";
    }

    std::string listText(uint16_t prop, const uint8_t* value) const {
        const uint32_t offset = u32At(value, 0);
        const uint32_t count = u32At(value, 1);
        std::string out = "[";
        if (prop == sp::FontFamily) {
            const auto pool = m_style.fontFamilies();
            for (uint32_t i = 0; i < count && offset + i < pool.size(); ++i) {
                out += (i == 0 ? "\"" : ",\"") + std::string(str(pool[offset + i])) + "\"";
            }
        } else if (prop == sp::Transition) {
            const auto pool = m_style.transitions();
            for (uint32_t i = 0; i < count && offset + i < pool.size(); ++i) {
                const IrTransitionSpec& t = pool[offset + i];
                out += (i == 0 ? "" : ",") + std::to_string(t.prop) + " " + num(t.duration) + " " +
                       num(t.delay) + " " + easingText(t.easing);
            }
        } else if (prop == sp::Animation) {
            const auto pool = m_style.animations();
            for (uint32_t i = 0; i < count && offset + i < pool.size(); ++i) {
                const IrAnimationSpec& a = pool[offset + i];
                const std::string name = a.timeline < m_ir.timelineCount()
                                             ? std::string(str(m_ir.timeline(a.timeline).name()))
                                             : "?";
                IrAnimationSpec rest = a;
                rest.timeline = 0;
                out += (i == 0 ? "" : ",") + name + " " + hexOf(&rest, sizeof(rest));
            }
        } else if (prop == sp::Effect || prop == sp::TextEffect) {
            const auto pool = m_style.effects();
            for (uint32_t i = 0; i < count && offset + i < pool.size(); ++i) {
                IrEffect fx = pool[offset + i];
                const std::string stops = stopsText(m_style.effectStops(), fx.stopOffset, fx.stopCount);
                fx.stopOffset = 0;
                out += (i == 0 ? "" : ",") + hexOf(&fx, sizeof(fx)) + stops;
            }
        } else if (prop == sp::BackgroundGradient) {
            const auto pool = m_style.gradients();
            if (count != 0 && offset < pool.size()) {
                IrGradient g = pool[offset];
                const std::string stops = stopsText(m_style.effectStops(), g.stopOffset, g.stopCount);
                g.stopOffset = 0;
                out += hexOf(&g, sizeof(g)) + stops;
            }
        } else {
            // A float pool slice (scene-target `d`, `stroke-dasharray`).
            const auto pool = m_style.floatPool();
            const uint32_t total = prop == prop::PathData ? count + u32At(value, 3) : count;
            const uint32_t start = offset;
            for (uint32_t i = 0; i < total && start + i < pool.size(); ++i) {
                out += (i == 0 ? "" : ",") + num(pool[start + i]);
            }
        }
        return out + "]";
    }

    std::string clipText(uint32_t index) const {
        const ClipShapeChunk& clips = m_ir.clipShapes();
        if (index == 0) {
            return "none";
        }
        if (index - 1 >= clips.shapes().size()) {
            return "clip?";
        }
        const IrClipShape& shape = clips.shapes()[index - 1];
        std::string out = "clip" + std::to_string(shape.kind) + "[";
        for (uint32_t i = 0; i < shape.paramCount && shape.paramOffset + i < clips.params().size(); ++i) {
            const IrClipParam& p = clips.params()[shape.paramOffset + i];
            out += (i == 0 ? "" : ",") + hexOf(&p, sizeof(p));
        }
        return out + "]";
    }

    std::string gridText(uint16_t prop, uint32_t index) const {
        if (index == 0) {
            return "none";
        }
        const GridChunk& grid = m_ir.grid();
        std::string out;
        if (prop == sp::GridTemplateAreas) {
            if (index - 1 >= grid.areaLists().size()) {
                return "areas?";
            }
            const IrGridList& list = grid.areaLists()[index - 1];
            out = "areas " + std::to_string(list.rows) + "x" + std::to_string(list.cols) + "[";
            for (uint32_t i = 0; i < list.count && list.offset + i < grid.areas().size(); ++i) {
                const IrGridArea& a = grid.areas()[list.offset + i];
                out += (i == 0 ? "" : ",") + std::string(str(a.name)) + " " + std::to_string(a.rowStart) +
                       " " + std::to_string(a.rowEnd) + " " + std::to_string(a.colStart) + " " +
                       std::to_string(a.colEnd);
            }
            return out + "]";
        }
        if (index - 1 >= grid.trackLists().size()) {
            return "tracks?";
        }
        const IrGridList& list = grid.trackLists()[index - 1];
        out = "tracks[";
        for (uint32_t i = 0; i < list.count && list.offset + i < grid.tracks().size(); ++i) {
            const IrGridTrack& t = grid.tracks()[list.offset + i];
            out += (i == 0 ? "" : ",") + num(t.value) + "/" + std::to_string(t.kind);
        }
        return out + "]";
    }

    std::string valueText(const IrDecl& decl) const {
        const uint8_t* v = decl.value;
        std::string text;
        switch (decl.valueType) {
            case kDeclVarRef: {
                text = "var(" + std::string(str(u32At(v, 0)));
                const uint32_t fallback = u32At(v, 1);
                if (fallback != kNoVarFallback && fallback < m_ir.vars().fallbacks().size()) {
                    text += ", " + valueText(m_ir.vars().fallbacks()[fallback]);
                }
                text += ")";
                break;
            }
            case kDeclCalc:
                text = calcText(u32At(v, 0));
                break;
            case static_cast<uint8_t>(ValueType::Float):
                text = num(f32At(v, 0));
                break;
            case static_cast<uint8_t>(ValueType::Vec2):
                text = num(f32At(v, 0)) + " " + num(f32At(v, 1));
                break;
            case static_cast<uint8_t>(ValueType::Vec3):
                text = num(f32At(v, 0)) + " " + num(f32At(v, 1)) + " " + num(f32At(v, 2));
                break;
            case static_cast<uint8_t>(ValueType::Color):
                text = "rgba(" + num(f32At(v, 0)) + "," + num(f32At(v, 1)) + "," + num(f32At(v, 2)) + "," +
                       num(f32At(v, 3)) + ")";
                break;
            case static_cast<uint8_t>(ValueType::Length):
                text = v[4] == static_cast<uint8_t>(LengthUnit::Calc)
                           ? calcText(static_cast<uint32_t>(f32At(v, 0)))
                           : num(f32At(v, 0)) + unitName(v[4]);
                break;
            case static_cast<uint8_t>(ValueType::Enum): {
                const uint32_t e = u32At(v, 0);
                if (decl.prop == sp::ClipPath) {
                    text = clipText(e);
                } else if (decl.prop == sp::GridTemplateColumns || decl.prop == sp::GridTemplateRows ||
                           decl.prop == sp::GridTemplateAreas) {
                    text = gridText(decl.prop, e);
                } else if (const char* name = enumName(decl.prop, e)) {
                    text = name;
                } else {
                    text = "#" + std::to_string(e);
                }
                break;
            }
            case static_cast<uint8_t>(ValueType::StringRef):
                text = "\"" + std::string(str(u32At(v, 0))) + "\"";
                break;
            case static_cast<uint8_t>(ValueType::ListRef):
                text = listText(decl.prop, v);
                break;
            default:
                text = "raw" + std::to_string(decl.valueType) + ":" + hexOf(v, sizeof(decl.value));
                break;
        }
        return decl.important != 0 ? text + " !important" : text;
    }

    std::string propName(uint16_t prop) const {
        if (const PropInfo* info = findPropById(prop)) {
            return info->cssName;
        }
        return "prop" + std::to_string(prop);
    }

    std::string sceneTargetText(const IrSelPart& part) const {
        switch (static_cast<IrSceneTarget>(part.classCount)) {
            case IrSceneTarget::Name:
                return std::string(str(part.tag));
            case IrSceneTarget::Tag:
                return "<" + std::string(str(part.tag)) + ">";
            default:
                return "*";
        }
    }

    // ---- cascade -----------------------------------------------------------------------------

    struct Winner {
        uint64_t key = 0;
        const IrDecl* decl = nullptr;
    };

    Props cascade(uint32_t e, const Combo& combo, const std::string* mediaKey) const {
        std::map<std::string, Winner> winners;
        const auto rules = m_style.rules();
        const auto parts = m_style.selectorParts();
        const auto decls = m_style.decls();
        for (uint32_t ri = 0; ri < rules.size(); ++ri) {
            if (m_ruleHasMedia[ri] != 0 && (mediaKey == nullptr || m_ruleMedia[ri] != *mediaKey)) {
                continue;
            }
            const IrRule& rule = rules[ri];
            if (rule.selCount == 0 || rule.selOffset + rule.selCount > parts.size()) {
                continue;
            }
            const auto chain = parts.subspan(rule.selOffset, rule.selCount);
            if (!matchFrom(e, chain, 0, combo)) {
                continue;
            }
            const std::string suffix = rule.selCount >= 2 && chain[1].combinator == kIrSelSceneTarget
                                           ? "@" + sceneTargetText(chain[1])
                                           : std::string();
            for (uint32_t di = 0; di < rule.declCount && rule.declOffset + di < decls.size(); ++di) {
                const IrDecl& d = decls[rule.declOffset + di];
                const uint64_t key = (static_cast<uint64_t>(d.important != 0 ? 1 : 0) << 63) |
                                     (static_cast<uint64_t>(rule.specificity & 0x7FFFFFFFu) << 32) | ri;
                Winner& w = winners[propName(d.prop) + suffix];
                if (w.decl == nullptr || key >= w.key) {
                    w = Winner{key, &d};
                }
            }
        }
        const IrDocElement& el = m_doc.elements()[e];
        const auto inlineDecls = m_doc.inlineDecls();
        for (uint32_t i = 0; i < el.inlineDeclCount && el.inlineDeclOffset + i < inlineDecls.size(); ++i) {
            const IrDecl& d = inlineDecls[el.inlineDeclOffset + i];
            const uint64_t key = (static_cast<uint64_t>(d.important != 0 ? 1 : 0) << 63) |
                                 (static_cast<uint64_t>(1u << 24) << 32) | (0xFFFFFFFFull - 1);
            Winner& w = winners[propName(d.prop)];
            if (w.decl == nullptr || key >= w.key) {
                w = Winner{key, &d};
            }
        }
        Props props;
        for (const auto& [name, w] : winners) {
            props.emplace(name, valueText(*w.decl));
        }
        return props;
    }

    static void writeProps(std::string& out, const Props& props) {
        out += "{";
        bool first = true;
        for (const auto& [name, value] : props) {
            out += first ? "" : ", ";
            first = false;
            appendJsonString(out, name);
            out += ": ";
            appendJsonString(out, value);
        }
        out += "}";
    }

    void writeElement(std::string& out, uint32_t e) const {
        out += "    {\"index\": " + std::to_string(e) + ", \"path\": ";
        appendJsonString(out, m_paths[e]);
        out += ", \"states\": {";
        for (size_t c = 0; c < m_combos.size(); ++c) {
            const Props base = cascade(e, m_combos[c], nullptr);
            out += c == 0 ? "\n      " : ",\n      ";
            appendJsonString(out, m_combos[c].key);
            out += ": {\"base\": ";
            writeProps(out, base);
            for (const std::string& media : m_mediaKeys) {
                const Props with = cascade(e, m_combos[c], &media);
                Props delta;
                for (const auto& [name, value] : with) {
                    auto it = base.find(name);
                    if (it == base.end() || it->second != value) {
                        delta.emplace(name, value);
                    }
                }
                if (!delta.empty()) {
                    out += ", ";
                    appendJsonString(out, "@media " + media);
                    out += ": ";
                    writeProps(out, delta);
                }
            }
            out += "}";
        }
        out += "}}";
    }

    void writeTokens(std::string& out) const {
        std::map<std::string, std::string> tokens;
        for (const IrVar& var : m_ir.vars().tokens()) {
            IrDecl decl{};
            decl.valueType = var.valueType;
            std::memcpy(decl.value, var.value, sizeof(decl.value));
            tokens[std::string(str(var.name))] = valueText(decl);
        }
        bool first = true;
        for (const auto& [name, value] : tokens) {
            out += first ? "" : ", ";
            first = false;
            appendJsonString(out, name);
            out += ": ";
            appendJsonString(out, value);
        }
    }

    const IrView& m_ir;
    const StyleChunk& m_style;
    const DocChunk& m_doc;
    std::vector<std::vector<uint32_t>> m_children;
    std::vector<uint32_t> m_childIndex;
    std::vector<std::string> m_paths;
    std::vector<std::string> m_ruleMedia;
    std::vector<uint8_t> m_ruleHasMedia;
    std::vector<std::string> m_mediaKeys;
    std::vector<Combo> m_combos;
};

}  // namespace

bool dumpComputedStyle(std::span<const uint8_t> ir, std::string& json, std::string& error) {
    Result<IrView> view = IrView::open(IrBlob{ir.data(), ir.size()});
    if (!view.ok()) {
        error = "the IR does not open";
        return false;
    }
    Dumper dumper(view.value());
    json = dumper.run();
    return true;
}

}  // namespace xt::compile
