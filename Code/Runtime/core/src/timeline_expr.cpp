#include "timeline_expr.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "binding_runtime.h"
#include "expr_op_info.h"
#include "timeline_group.h"
#include "xt/animator.h"
#include "xt/expr_noise.h"
#include "xt/node_arena.h"
#include "xt/property.h"
#include "xt/timeline.h"

/// T-776: timeline expression programs at run time. See timeline_expr.h and Docs/design/
/// 58-value-functions-and-drivers.md §58.5. The ops mirror the compiler's evaluator
/// (Code/Runtime/compile/src/expr: aeLinear / aeEase, wiggle, random, valueAtTime) in single
/// precision; noise, wiggle and random call xt/expr_noise.h, the very code the compiler runs.

namespace xt {

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/// AE linear / ease (compile's aeLinear / aeEase): clamped, tMin / tMax swapped when reversed.
float aeMap(ExprOp op, float t, float tMin, float tMax, float v1, float v2) {
    if (tMax < tMin) {
        std::swap(tMin, tMax);
    }
    if (t <= tMin) {
        return v1;
    }
    if (t >= tMax) {
        return v2;
    }
    float p = (t - tMin) / (tMax - tMin);
    if (op != ExprOp::Linear) {
        const float* c =
            op == ExprOp::EaseIn ? kExprEaseIn : (op == ExprOp::EaseOut ? kExprEaseOut : kExprEase);
        p = evalEasing(Easing::bezier(c[0], c[1], c[2], c[3]), p);
    }
    return v1 + (v2 - v1) * p;
}

float truth(bool b) { return b ? 1.0f : 0.0f; }

}  // namespace

float sampleExprCurve(std::span<const IrExprKey> keys, float t) {
    if (keys.empty()) {
        return 0.0f;
    }
    if (!(t > keys.front().time)) {  // Also NaN.
        return keys.front().value;
    }
    if (t >= keys.back().time) {
        return keys.back().value;
    }
    // The last key at or before t (keys are non-decreasing, so upper_bound finds the segment).
    const auto it =
        std::upper_bound(keys.begin(), keys.end(), t, [](float x, const IrExprKey& k) { return x < k.time; });
    const IrExprKey& a = *(it - 1);
    const IrExprKey& b = *it;
    if (a.interp == 1 || !(b.time > a.time)) {
        return a.value;
    }
    return a.value + (b.value - a.value) * ((t - a.time) / (b.time - a.time));
}

float exprValueComponent(const Value& v, uint8_t c) {
    switch (v.type) {
        case ValueType::Float:
            return v.f;
        case ValueType::Length:
            return v.len.value;
        case ValueType::Vec2:
            return c == 0 ? v.v2.x : v.v2.y;
        case ValueType::Vec3:
            return c == 0 ? v.v3.x : (c == 1 ? v.v3.y : v.v3.z);
        case ValueType::Color:
            return c == 0 ? v.c.r : (c == 1 ? v.c.g : (c == 2 ? v.c.b : v.c.a));
        default:
            return 0.0f;
    }
}

void setExprValueComponent(Value& v, uint8_t c, float x) {
    switch (v.type) {
        case ValueType::Float:
            v.f = x;
            break;
        case ValueType::Length:
            v.len.value = x;
            break;
        case ValueType::Vec2:
            (c == 0 ? v.v2.x : v.v2.y) = x;
            break;
        case ValueType::Vec3:
            (c == 0 ? v.v3.x : (c == 1 ? v.v3.y : v.v3.z)) = x;
            break;
        case ValueType::Color:
            (c == 0 ? v.c.r : (c == 1 ? v.c.g : (c == 2 ? v.c.b : v.c.a))) = x;
            break;
        default:
            break;
    }
}

float evalTimelineExpr(const ExprProgram& p, const ExprEnv& env, bool* fellBack) {
    float st[kExprMaxStack];
    // Per stack slot: the value is a constant of the program (PushConst / PushDuration / PushIndex
    // and pure ops over them) -- a wiggle with a constant frequency is periodic over the duration.
    bool konst[kExprMaxStack];
    uint32_t sp = 0;
    const ExprInputs* in = env.inputs;
    auto fallback = [&]() {
        if (fellBack != nullptr) {
            *fellBack = true;
        }
        return env.value;
    };
    for (const IrExprOp& op : p.ops) {
        const uint32_t argc = op.argc;
        if (sp < argc || sp - argc >= kExprMaxStack) {
            return fallback();  // Unreachable for a validated program.
        }
        const float* a = st + (sp - argc);
        bool allConst = true;
        for (uint32_t i = 0; i < argc; ++i) {
            allConst = allConst && konst[sp - argc + i];
        }
        float r = 0.0f;
        bool rConst = allConst;
        switch (static_cast<ExprOp>(op.op)) {
            case ExprOp::PushConst:
                r = op.imm;
                rConst = true;
                break;
            case ExprOp::PushValue:
                r = env.value;
                rConst = false;
                break;
            case ExprOp::PushCurve:
                if (op.ref < p.curves.size() &&
                    p.curves[op.ref].keyOffset + uint64_t{p.curves[op.ref].keyCount} <= p.keys.size()) {
                    r = sampleExprCurve(p.keys.subspan(p.curves[op.ref].keyOffset, p.curves[op.ref].keyCount),
                                        env.time);
                }
                rConst = false;
                break;
            case ExprOp::PushPort:
                r = in != nullptr ? in->portNumber(op.ref) : 0.0f;
                rConst = false;
                break;
            case ExprOp::PushProp: {
                const uint32_t code = static_cast<uint32_t>(op.imm);
                r = in != nullptr ? in->propValue(op.ref, static_cast<PropId>(code >> 2),
                                                  static_cast<uint8_t>(code & 3u))
                                  : 0.0f;
                rConst = false;
                break;
            }
            case ExprOp::PushLayout:
                r = in != nullptr ? in->layoutValue(op.ref, static_cast<uint8_t>(op.imm)) : 0.0f;
                rConst = false;
                break;
            case ExprOp::PushPlayRandom:
                r = exprRandom01(exprMixSeed(exprMixSeed(env.playSeed, p.seed), op.ref));
                rConst = false;
                break;
            case ExprOp::PushTime:
                r = env.time;
                rConst = false;
                break;
            case ExprOp::PushDuration:
                r = env.duration;
                rConst = true;
                break;
            case ExprOp::PushIndex:
                r = p.index;
                rConst = true;
                break;
            case ExprOp::Add:
                r = a[0] + a[1];
                break;
            case ExprOp::Sub:
                r = a[0] - a[1];
                break;
            case ExprOp::Mul:
                r = a[0] * a[1];
                break;
            case ExprOp::Div:
                r = a[1] == 0.0f ? kNaN : a[0] / a[1];
                break;
            case ExprOp::Mod:
                r = a[1] == 0.0f ? kNaN : std::fmod(a[0], a[1]);
                break;
            case ExprOp::Neg:
                r = -a[0];
                break;
            case ExprOp::Min:
                r = std::min(a[0], a[1]);
                break;
            case ExprOp::Max:
                r = std::max(a[0], a[1]);
                break;
            case ExprOp::Abs:
                r = std::fabs(a[0]);
                break;
            case ExprOp::Round:
                r = std::floor(a[0] + 0.5f);
                break;
            case ExprOp::Floor:
                r = std::floor(a[0]);
                break;
            case ExprOp::Ceil:
                r = std::ceil(a[0]);
                break;
            case ExprOp::Sqrt:
                r = std::sqrt(a[0]);
                break;
            case ExprOp::Pow:
                r = std::pow(a[0], a[1]);
                break;
            case ExprOp::Exp:
                r = std::exp(a[0]);
                break;
            case ExprOp::Log:
                r = std::log(a[0]);
                break;
            case ExprOp::Sin:
                r = std::sin(a[0]);
                break;
            case ExprOp::Cos:
                r = std::cos(a[0]);
                break;
            case ExprOp::Tan:
                r = std::tan(a[0]);
                break;
            case ExprOp::Asin:
                r = std::asin(a[0]);
                break;
            case ExprOp::Acos:
                r = std::acos(a[0]);
                break;
            case ExprOp::Atan:
                r = std::atan(a[0]);
                break;
            case ExprOp::Atan2:
                r = std::atan2(a[0], a[1]);
                break;
            case ExprOp::Clamp:
                r = clampf(a[0], std::min(a[1], a[2]), std::max(a[1], a[2]));
                break;
            case ExprOp::Lerp:
                r = a[0] + (a[1] - a[0]) * a[2];
                break;
            case ExprOp::Linear:
            case ExprOp::Ease:
            case ExprOp::EaseIn:
            case ExprOp::EaseOut:
                r = aeMap(static_cast<ExprOp>(op.op), a[0], a[1], a[2], a[3], a[4]);
                break;
            case ExprOp::Lt:
                r = truth(a[0] < a[1]);
                break;
            case ExprOp::Le:
                r = truth(a[0] <= a[1]);
                break;
            case ExprOp::Gt:
                r = truth(a[0] > a[1]);
                break;
            case ExprOp::Ge:
                r = truth(a[0] >= a[1]);
                break;
            case ExprOp::Eq:
                r = truth(a[0] == a[1]);
                break;
            case ExprOp::Ne:
                r = truth(a[0] != a[1]);
                break;
            case ExprOp::And:
                r = truth(a[0] != 0.0f && a[1] != 0.0f);
                break;
            case ExprOp::Or:
                r = truth(a[0] != 0.0f || a[1] != 0.0f);
                break;
            case ExprOp::Not:
                r = truth(a[0] == 0.0f);
                break;
            case ExprOp::Select:
                r = a[0] != 0.0f ? a[1] : a[2];
                break;
            case ExprOp::Noise:
                r = exprNoise(a[0], exprMixSeed(p.seed, op.ref));
                break;
            case ExprOp::Wiggle: {
                // compile's wiggle(freq, amp, octaves = 1, amp_mult = 0.5, t = time): the row at t
                // plus amp x the noise; periodic over the duration only for a constant frequency.
                const int32_t oct =
                    argc > 2 ? std::clamp(exprFloorToInt(a[2]), int32_t{1}, kExprMaxWiggleOctaves) : 1;
                const float ampMult = argc > 3 ? a[3] : 0.5f;
                const float t = argc > 4 ? a[4] : env.time;
                float base = env.value;
                if (argc > 4) {
                    base = env.set != nullptr ? env.set->rowValueAt(env.self, t) : env.value;
                }
                const float duration = konst[sp - argc] ? env.duration : 0.0f;
                r = base +
                    a[1] * exprWiggleNoise(t, a[0], duration, exprMixSeed(p.seed, op.ref), oct, ampMult);
                rConst = false;
                break;
            }
            case ExprOp::Random: {
                const bool timeless = op.imm == 1.0f;
                const float t = argc > 2 ? a[2] : env.time;
                const uint32_t frame = timeless ? 0u : exprRandomFrame(t);
                const uint32_t seed =
                    exprRandomSeed(exprMixSeed(p.seed, kExprRandomSalt), frame, op.ref, timeless);
                r = a[0] + (a[1] - a[0]) * exprRandom01(seed);
                rConst = false;
                break;
            }
            case ExprOp::ValueAt:
                r = env.set != nullptr ? env.set->rowValueAt(env.self, a[0]) : env.value;
                rConst = false;
                break;
            case ExprOp::RefValueAt:
                r = env.set != nullptr
                        ? env.set->refValueAt(env.self, op.ref, a[0], env.playSeed, env.depth + 1)
                        : 0.0f;
                rConst = false;
                break;
            case ExprOp::PushPortDelayed:
                r = in != nullptr ? in->portDelayed(op.ref, a[0]) : 0.0f;
                rConst = false;
                break;
            case ExprOp::CurveAt:  // T-777: PushCurve at time a[0] (posterizeTime()).
                if (op.ref < p.curves.size() &&
                    p.curves[op.ref].keyOffset + uint64_t{p.curves[op.ref].keyCount} <= p.keys.size()) {
                    r = sampleExprCurve(p.keys.subspan(p.curves[op.ref].keyOffset, p.curves[op.ref].keyCount),
                                        a[0]);
                }
                rConst = false;
                break;
            case ExprOp::PushPropDelayed: {  // T-781: PushProp `a[0]` seconds ago.
                const uint32_t code = static_cast<uint32_t>(op.imm);
                r = in != nullptr ? in->propDelayed(op.ref, code, a[0]) : 0.0f;
                rConst = false;
                break;
            }
            case ExprOp::RandomSeeded: {
                // T-777: Random after seedRandom(a[0]): the compiler's exprMixSeed(seed,
                // exprSeedFromNumber(n)) in place of the default random seed.
                const bool timeless = op.imm == 1.0f;
                const float t = argc > 3 ? a[3] : env.time;
                const uint32_t frame = timeless ? 0u : exprRandomFrame(t);
                const uint32_t seed =
                    exprRandomSeed(exprMixSeed(p.seed, exprSeedFromNumber(a[0])), frame, op.ref, timeless);
                r = a[1] + (a[2] - a[1]) * exprRandom01(seed);
                rConst = false;
                break;
            }
            default:
                return fallback();  // Unreachable for a validated program.
        }
        sp -= argc;
        st[sp] = r;
        konst[sp] = rConst;
        ++sp;
    }
    if (sp != 1 || !std::isfinite(st[0])) {
        return fallback();
    }
    return st[0];
}

// -- TimelineExprSet ------------------------------------------------------------------------------

void TimelineExprSet::load(const IrView& ir) {
    const TimelineExprChunk& chunk = ir.timelineExprs();
    m_exprs = {};
    m_masks.clear();
    m_byMember.clear();
    m_memberScene.clear();
    m_rings.clear();
    m_historyTimes.clear();
    m_historyValues.clear();
    m_watched.clear();
    m_historyGeneration = 0;
    if (!chunk.present() || chunk.exprs().empty() || !ir.timelineGroups().present()) {
        return;
    }
    m_exprs = chunk.exprs();
    m_ops = chunk.ops();
    m_curves = chunk.curves();
    m_keys = chunk.keys();
    m_groups = ir.timelineGroups().groups();
    m_members = ir.timelineGroups().members();
    m_memberScene.assign(m_members.size(), 0);
    // The inputs: declared, read by the ops, then those of the programs RefValueAt runs (at most
    // kExprMaxRefDepth deep, so that many passes reach every one).
    m_masks.resize(m_exprs.size());
    for (size_t i = 0; i < m_exprs.size(); ++i) {
        uint16_t mask = m_exprs[i].inputMask;
        for (const IrExprOp& op : program(static_cast<uint32_t>(i)).ops) {
            mask |= exprOpInput(op.op);
        }
        m_masks[i] = mask;
    }
    for (uint32_t pass = 0; pass < kExprMaxRefDepth; ++pass) {
        for (size_t i = 0; i < m_exprs.size(); ++i) {
            for (const IrExprOp& op : program(static_cast<uint32_t>(i)).ops) {
                if (op.op == static_cast<uint8_t>(ExprOp::RefValueAt) && (op.ref & kExprRefRow) == 0 &&
                    op.ref < m_masks.size()) {
                    m_masks[i] |= m_masks[op.ref];
                }
            }
        }
    }
    m_byMember.resize(m_exprs.size());
    for (size_t i = 0; i < m_exprs.size(); ++i) {
        m_byMember[i] = static_cast<uint32_t>(i);
    }
    std::stable_sort(m_byMember.begin(), m_byMember.end(), [this](uint32_t a, uint32_t b) {
        const IrTimelineExpr& x = m_exprs[a];
        const IrTimelineExpr& y = m_exprs[b];
        return x.group != y.group ? x.group < y.group : x.member < y.member;
    });
    // The boxes PushLayout reads and the ports PushPortDelayed (T-781: the property components
    // PushPropDelayed) reads, with the longest delay.
    for (size_t i = 0; i < m_exprs.size(); ++i) {
        for (const IrExprOp& op : program(static_cast<uint32_t>(i)).ops) {
            if (op.op == static_cast<uint8_t>(ExprOp::PushLayout)) {
                const bool known = std::any_of(m_watched.begin(), m_watched.end(),
                                               [&](const Watched& w) { return w.element == op.ref; });
                if (!known) {
                    Watched w;
                    w.element = op.ref;
                    std::fill(std::begin(w.box), std::end(w.box), kNaN);  // The first layout counts.
                    m_watched.push_back(w);
                }
            }
            const bool prop = op.op == static_cast<uint8_t>(ExprOp::PushPropDelayed);
            if (op.op != static_cast<uint8_t>(ExprOp::PushPortDelayed) && !prop) {
                continue;
            }
            const uint32_t code = prop ? static_cast<uint32_t>(op.imm) : 0u;
            // A delay from a run-time input has no compile-time length: the 4 s ceiling (58 §58.5).
            const float declared = m_exprs[i].portHistorySeconds;
            const float seconds =
                std::min(declared > 0.0f ? declared : kExprMaxPortHistorySeconds, kExprMaxPortHistorySeconds);
            const uint32_t capacity = static_cast<uint32_t>(std::ceil(seconds * 120.0f)) + 2u;
            auto ring = std::find_if(m_rings.begin(), m_rings.end(), [&](const Ring& r) {
                return r.prop == prop && r.ref == op.ref && r.code == code;
            });
            if (ring == m_rings.end()) {
                m_rings.push_back(Ring{});
                ring = m_rings.end() - 1;
                ring->prop = prop;
                ring->ref = op.ref;
                ring->code = code;
            }
            ring->capacity = std::max(ring->capacity, capacity);
            if (std::find(ring->groups.begin(), ring->groups.end(), m_exprs[i].group) == ring->groups.end()) {
                ring->groups.push_back(m_exprs[i].group);
            }
        }
    }
    uint32_t total = 0;
    for (Ring& r : m_rings) {
        r.offset = total;
        total += r.capacity;
    }
    m_historyTimes.assign(total, 0.0f);
    m_historyValues.assign(total, 0.0f);
}

void TimelineExprSet::bind(const NodeArena* arena, const std::vector<NodeId>* docElementNodes,
                           const BindingRuntime* bindings, const Animator* animator) {
    m_arena = arena;
    m_docNodes = docElementNodes;
    m_bindings = bindings;
    m_animator = animator;
}

std::span<const uint32_t> TimelineExprSet::exprsOf(uint32_t group, uint32_t member) const {
    auto key = [this](uint32_t i) { return uint64_t{m_exprs[i].group} << 32 | m_exprs[i].member; };
    const uint64_t want = uint64_t{group} << 32 | member;
    const auto lo = std::lower_bound(m_byMember.begin(), m_byMember.end(), want,
                                     [&](uint32_t i, uint64_t w) { return key(i) < w; });
    const auto hi =
        std::upper_bound(lo, m_byMember.end(), want, [&](uint64_t w, uint32_t i) { return w < key(i); });
    return std::span<const uint32_t>(m_byMember.data() + (lo - m_byMember.begin()),
                                     static_cast<size_t>(hi - lo));
}

void TimelineExprSet::setMemberScene(uint32_t group, uint32_t member, bool scene) {
    if (group < m_groups.size() && member < m_groups[group].memberCount) {
        const size_t index = size_t{m_groups[group].memberOffset} + member;
        if (index < m_memberScene.size()) {
            m_memberScene[index] = scene ? 1 : 0;
        }
    }
}

ExprProgram TimelineExprSet::program(uint32_t expr) const {
    const IrTimelineExpr& e = m_exprs[expr];
    ExprProgram p;
    p.ops = m_ops.subspan(e.opOffset, e.opCount);
    p.curves = m_curves.subspan(e.curveOffset, e.curveCount);
    p.keys = m_keys;
    p.seed = e.seed;
    p.index = e.index;
    return p;
}

float TimelineExprSet::evaluate(uint32_t expr, float t, float value, uint32_t playSeed, uint32_t depth,
                                bool* fellBack) const {
    const IrTimelineExpr& e = m_exprs[expr];
    ExprEnv env;
    env.value = value;
    env.time = t;
    env.duration = e.group < m_groups.size() ? m_groups[e.group].duration : 0.0f;
    env.playSeed = playSeed;
    env.inputs = this;
    env.set = this;
    env.self = expr;
    env.depth = depth;
    return evalTimelineExpr(program(expr), env, fellBack);
}

float TimelineExprSet::rowKeysAt(uint32_t group, uint32_t member, uint32_t track, uint8_t component,
                                 float t) const {
    if (m_animator == nullptr || group >= m_groups.size() || member >= m_groups[group].memberCount) {
        return 0.0f;
    }
    const size_t index = size_t{m_groups[group].memberOffset} + member;
    const IrTimelineGroupMember& m = m_members[index];
    const Timeline* tl = m_animator->timeline(m.timeline);
    if (tl == nullptr || track >= tl->tracks.size() || tl->tracks[track].kfCount == 0 ||
        exprTrackComponents(static_cast<uint8_t>(tl->tracks[track].type)) == 0) {
        return 0.0f;
    }
    // The member's own time for group time t (Document::applyTimelineGroupTime's mapping).
    float u = 0.0f;
    if (index < m_memberScene.size() && m_memberScene[index] != 0) {
        u = clampf(t, 0.0f, tl->duration);
    } else if (m.duration > 0.0f) {
        u = clampf((t - m.delay) / m.duration, 0.0f, 1.0f);
    } else {
        u = t >= m.delay ? 1.0f : 0.0f;
    }
    if (u != u) {
        u = 0.0f;
    }
    const Value v =
        evalTrack(*tl, tl->tracks[track], u, nullptr, nullptr, nullptr, m_animator->easingPoints());
    return exprValueComponent(v, component);
}

float TimelineExprSet::rowValueAt(uint32_t expr, float t) const {
    const IrTimelineExpr& e = m_exprs[expr];
    return rowKeysAt(e.group, e.member, e.track, e.component, t);
}

float TimelineExprSet::refValueAt(uint32_t expr, uint32_t ref, float t, uint32_t playSeed,
                                  uint32_t depth) const {
    const IrTimelineExpr& e = m_exprs[expr];
    if ((ref & kExprRefRow) != 0) {
        return rowKeysAt(e.group, (ref >> 16) & 0x7FFFu, (ref >> 4) & 0xFFFu,
                         static_cast<uint8_t>(ref & 0xFu), t);
    }
    if (ref >= m_exprs.size()) {
        return 0.0f;
    }
    const float keyed = rowValueAt(ref, t);
    if (depth > kExprMaxRefDepth) {
        return keyed;  // Deeper than the limit: the row's keys, its program not run.
    }
    return evaluate(ref, t, keyed, playSeed, depth);
}

uint32_t TimelineExprSet::drawPlaySeed(uint32_t group, StringId name) {
    ++m_playCount;
    return exprMixSeed(exprMixSeed(exprMixSeed(m_seedBase, m_playCount), group), name);
}

void TimelineExprSet::recordPortHistory(float clock, std::span<const TimelineGroupRuntime> groups) {
    m_now = clock;
    bool recorded = false;
    for (Ring& r : m_rings) {
        const bool playing = std::any_of(r.groups.begin(), r.groups.end(), [&](uint32_t g) {
            return g < groups.size() && groups[g].active && groups[g].playing;
        });
        if (!playing || r.capacity == 0) {
            continue;  // Not recorded while no timeline reading it plays (58 §58.5).
        }
        m_historyTimes[r.offset + r.head] = clock;
        // T-781: a property's value now is the one the last update wrote (or the host set since).
        m_historyValues[r.offset + r.head] =
            r.prop ? propValue(r.ref, static_cast<PropId>(r.code >> 2), static_cast<uint8_t>(r.code & 3u))
                   : portNumber(r.ref);
        r.head = (r.head + 1) % r.capacity;
        r.count = std::min(r.count + 1, r.capacity);
        recorded = true;
    }
    if (recorded) {
        ++m_historyGeneration;
    }
}

void TimelineExprSet::checkLayout() {
    bool changed = false;
    for (Watched& w : m_watched) {
        float box[4] = {0, 0, 0, 0};
        if (m_arena != nullptr && m_docNodes != nullptr && w.element < m_docNodes->size()) {
            const NodeId node = (*m_docNodes)[w.element];
            if (node != kInvalidNode && m_arena->alive(node) && m_arena->kind(node) == NodeKind::Element) {
                const LayoutBox& b = m_arena->element(node).box;
                box[0] = b.x;
                box[1] = b.y;
                box[2] = b.width;
                box[3] = b.height;
            }
        }
        for (int i = 0; i < 4; ++i) {
            if (!(box[i] == w.box[i])) {
                changed = true;
                w.box[i] = box[i];
            }
        }
    }
    if (changed) {
        ++m_layoutGeneration;
    }
}

const TimelineExprSet::Ring* TimelineExprSet::ringOf(bool prop, uint32_t ref, uint32_t code) const {
    for (const Ring& r : m_rings) {
        if (r.prop == prop && r.ref == ref && r.code == code) {
            return &r;
        }
    }
    return nullptr;
}

uint32_t TimelineExprSet::portHistoryCount(uint32_t port) const {
    const Ring* r = ringOf(false, port, 0);
    return r != nullptr ? r->count : 0u;
}

uint32_t TimelineExprSet::propHistoryCount(uint32_t element, uint32_t code) const {
    const Ring* r = ringOf(true, element, code);
    return r != nullptr ? r->count : 0u;
}

float TimelineExprSet::portNumber(uint32_t port) const {
    return m_bindings != nullptr ? m_bindings->portNumber(port) : 0.0f;
}

uint64_t TimelineExprSet::portGeneration() const {
    return m_bindings != nullptr ? m_bindings->numberGeneration() : 0u;
}

float TimelineExprSet::propValue(uint32_t element, PropId prop, uint8_t component) const {
    if (m_arena == nullptr || m_docNodes == nullptr || element >= m_docNodes->size()) {
        return 0.0f;
    }
    const NodeId node = (*m_docNodes)[element];
    if (node == kInvalidNode || !m_arena->alive(node) || !propertyDefined(prop) ||
        !propertyApplies(m_arena->kind(node), prop) ||
        (isEffectProp(prop) &&
         ((static_cast<uint32_t>(prop) >> 8) & 0xFu) >= m_arena->common(node).effectCount)) {
        return 0.0f;
    }
    const float v = exprValueComponent(getProperty(*m_arena, node, prop), component);
    return std::isfinite(v) ? v : 0.0f;
}

float TimelineExprSet::layoutValue(uint32_t element, uint8_t which) const {
    if (m_arena == nullptr || m_docNodes == nullptr || element >= m_docNodes->size()) {
        return 0.0f;
    }
    const NodeId node = (*m_docNodes)[element];
    if (node == kInvalidNode || !m_arena->alive(node) || m_arena->kind(node) != NodeKind::Element) {
        return 0.0f;
    }
    const LayoutBox& b = m_arena->element(node).box;
    switch (which) {
        case 0:
            return b.x;
        case 1:
            return b.y;
        case 2:
            return b.width;
        default:
            return b.height;
    }
}

float TimelineExprSet::portDelayed(uint32_t port, float seconds) const {
    return delayed(ringOf(false, port, 0), seconds, portNumber(port));
}

float TimelineExprSet::propDelayed(uint32_t element, uint32_t code, float seconds) const {
    const float now = propValue(element, static_cast<PropId>(code >> 2), static_cast<uint8_t>(code & 3u));
    return delayed(ringOf(true, element, code), seconds, now);
}

float TimelineExprSet::delayed(const Ring* r, float seconds, float now) const {
    if (r == nullptr || r->count == 0 || !(seconds > 0.0f)) {
        return now;
    }
    const float target = m_now - std::min(seconds, kExprMaxPortHistorySeconds);
    auto slot = [r](uint32_t k) { return r->offset + (r->head + r->capacity - 1 - k) % r->capacity; };
    const uint32_t newest = slot(0);
    if (target >= m_historyTimes[newest]) {
        return m_historyValues[newest];
    }
    for (uint32_t k = 1; k < r->count; ++k) {
        const uint32_t older = slot(k);
        if (m_historyTimes[older] <= target) {
            const uint32_t newer = slot(k - 1);
            const float t0 = m_historyTimes[older];
            const float t1 = m_historyTimes[newer];
            if (!(t1 > t0)) {
                return m_historyValues[newer];
            }
            return m_historyValues[older] +
                   (m_historyValues[newer] - m_historyValues[older]) * ((target - t0) / (t1 - t0));
        }
    }
    return m_historyValues[slot(r->count - 1)];  // Not enough history: the oldest record.
}

}  // namespace xt
