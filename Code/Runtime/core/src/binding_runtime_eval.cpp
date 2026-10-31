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

/// BindingRuntime (split from binding_runtime.cpp by T-412): binding evaluation and world anchors.

namespace xt {

using namespace binding_runtime_detail;  // T-412: helpers shared between the split files

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
            uint32_t port = ctx.binding != nullptr ? mapPort(ctx.binding->portMap, a.u) : a.u;
            if (ctx.node != kInvalidNode && port < m_slots.size() && m_slots[port].templateOnly) {
                port = m_doc.m_repeats->instancePortOf(ctx.node, port);  // T-411: a trigger value.
            }
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
                // T-411: a trigger set()'s Item value runs with an IR-less instance context.
                const bool own = ctx.binding != nullptr && ctx.binding->ir != nullptr;
                StringGuard& guard = own ? ctx.binding->strings : m_conditionStrings;
                const std::string_view source = own ? m_doc.m_ir.string(ctx.binding->ir->targetName) : name;
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
            if (isEffectProp(prop)) {
                // T-545 (18 §18.16, 04 §4.7): `bind:effect.<effect>.<param>` is a data-driven write,
                // so it lives in the Animator's override layer: it survives a restyle (which
                // rebuilds the effect stack), is re-applied every frame (a missing effect is a
                // no-op plus one kWarnEffectMissing), and `| tween(d)` tweens the override itself.
                Value ev;
                if (vt == ValueType::Float && (r.kind == Kind::Num || r.kind == Kind::Len)) {
                    ev = Value::ofFloat(static_cast<float>(toNum(r)));
                } else if (vt == ValueType::Color && r.kind == Kind::Color) {
                    ev = r.v;
                } else {
                    return;
                }
                const float duration = b.transitionTween && !initial ? b.tweenDuration : 0.0f;
                doc.m_animator.setOverride(b.node, prop, ev, duration, b.tweenEasing);
                return;
            }
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
            // T-546 (27 §27.3.3): the first write of a `tween` binding applies directly.
            // Initial evaluation and the first port-driven write are both direct; tweening starts
            // from the second port-driven change.
            const bool firstTweenWrite = b.transitionTween && !b.tweened;
            if (b.transitionTween && !initial) {
                b.tweened = true;
            }
            if (b.transitionTween && !initial && !firstTweenWrite) {
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

namespace {

// -- World anchors (T-200) ----------------------------------------------------------------------

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

}  // namespace xt
