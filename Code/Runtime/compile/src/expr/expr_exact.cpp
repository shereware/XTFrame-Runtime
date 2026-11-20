#include "expr_exact.h"

#include <algorithm>
#include <cmath>
#include <vector>

/// See expr_exact.h. Docs/design/58-value-functions-and-drivers.md §58.3's exact rows.

namespace xt::compile::expr {

namespace {

bool sameValue(double a, double b) { return std::fabs(a - b) <= 1e-9 * std::max(1.0, std::fabs(a)); }

/// Appends a key at `t`; at the time of the last key (a block junction) an equal value continues
/// it, a different one jumps: the last key holds, the new key sits kExprJumpSeconds later.
void appendKey(KeyList& list, double t, double v, const Easing& e) {
    if (!list.ok) {
        return;
    }
    if (!list.keys.empty()) {
        LoweredKey& last = list.keys.back();
        if (std::fabs(t - last.time) <= 1e-9) {
            if (sameValue(v, last.value)) {
                const LoweredKey k = loweredFromRow(last.time, last.value, e);
                last.interp = k.interp;
                last.easing = k.easing;
                return;
            }
            last.interp = LoweredInterp::Hold;
            last.easing = Easing::hold();
            list.jumps.push_back(last.time);
            t = last.time + kExprJumpSeconds;
            list.jumpKeys.push_back(list.keys.size());
        }
        if (!(t > list.keys.back().time)) {
            list.ok = false;  // a segment shorter than the jump
            return;
        }
    }
    list.keys.push_back(loweredFromRow(t, v, e));
}

/// The easing of a segment played backwards: e'(q) = 1 - e(1 - q). False for linear() points; `hold`
/// for Hold (whose reverse jumps at the start, see expandLoop).
bool reverseEasing(const Easing& e, Easing& out, bool& hold) {
    hold = false;
    switch (e.kind) {
        case EasingKind::Linear:
            out = Easing::linear();
            return true;
        case EasingKind::CubicBezier:
            out = Easing::bezier(1.0f - e.x2, 1.0f - e.y2, 1.0f - e.x1, 1.0f - e.y1);
            return true;
        case EasingKind::Steps:
            out = Easing::steps(e.stepCount, e.stepsJumpEnd == 0);
            return true;
        case EasingKind::Hold:
            hold = true;
            out = Easing::hold();
            return true;
        case EasingKind::LinearPoints:
            return false;
    }
    return false;
}

/// Appends keys K[from..to] of the row forward, starting at time `base` (K[from] lands on it), values +
/// `offset`.
void appendForward(KeyList& list, const std::span<const ExprKey> k, size_t from, size_t to, double base,
                   double offset) {
    for (size_t i = from; i <= to; ++i) {
        appendKey(list, base + (k[i].time - k[from].time), k[i].value + offset, k[i].easing);
    }
}

/// Appends K[to..from] backwards starting at `base` (K[to] lands on it).
void appendReversed(KeyList& list, const std::span<const ExprKey> k, size_t from, size_t to, double base) {
    const double t1 = k[to].time;
    for (size_t i = to + 1; i-- > from;) {
        const double t = base + (t1 - k[i].time);
        if (i == from) {
            appendKey(list, t, k[i].value, Easing::linear());
            break;
        }
        Easing rev;
        bool hold = false;
        if (!reverseEasing(k[i - 1].easing, rev, hold)) {
            list.ok = false;
            return;
        }
        if (hold) {
            // Forward, the segment shows K[i-1] until K[i]: backwards it shows K[i] only at its start.
            appendKey(list, t, k[i].value, Easing::hold());
            if (!list.ok) {
                return;
            }
            list.jumps.push_back(list.keys.back().time);
            appendKey(list, list.keys.back().time + kExprJumpSeconds, k[i - 1].value, Easing::linear());
        } else {
            appendKey(list, t, k[i].value, rev);
        }
    }
}

// ---- cutting a segment ----------------------------------------------------------------------

double bez(double t, double c1, double c2) {
    const double mt = 1.0 - t;
    return 3.0 * mt * mt * t * c1 + 3.0 * mt * t * t * c2 + t * t * t;
}

struct Pt {
    double x;
    double y;
};
Pt lerpPt(Pt a, Pt b, double s) { return Pt{a.x + (b.x - a.x) * s, a.y + (b.y - a.y) * s}; }

/// Splits a cubic-bezier easing at progress `p`: the left / right parts as easings of their own and
/// the eased progress at the cut. False when a part is not a valid CSS bezier (x outside [0, 1]) or
/// degenerate.
bool splitBezier(const Easing& e, double p, Easing& left, Easing& right, double& y) {
    const double x1 = std::clamp(static_cast<double>(e.x1), 0.0, 1.0);
    const double x2 = std::clamp(static_cast<double>(e.x2), 0.0, 1.0);
    double lo = 0;
    double hi = 1;
    for (int i = 0; i < 60; ++i) {
        const double mid = (lo + hi) / 2;
        if (bez(mid, x1, x2) < p) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    const double s = (lo + hi) / 2;
    const Pt p0{0, 0};
    const Pt p1{x1, e.y1};
    const Pt p2{x2, e.y2};
    const Pt p3{1, 1};
    const Pt p01 = lerpPt(p0, p1, s);
    const Pt p12 = lerpPt(p1, p2, s);
    const Pt p23 = lerpPt(p2, p3, s);
    const Pt p012 = lerpPt(p01, p12, s);
    const Pt p123 = lerpPt(p12, p23, s);
    const Pt m = lerpPt(p012, p123, s);
    y = m.y;
    if (!(m.x > 1e-9) || !(m.x < 1 - 1e-9) || std::fabs(m.y) < 1e-9 || std::fabs(1 - m.y) < 1e-9) {
        return false;
    }
    auto valid = [](double v) { return v >= -1e-6 && v <= 1 + 1e-6; };
    const double lx1 = p01.x / m.x;
    const double lx2 = p012.x / m.x;
    const double rx1 = (p123.x - m.x) / (1 - m.x);
    const double rx2 = (p23.x - m.x) / (1 - m.x);
    if (!valid(lx1) || !valid(lx2) || !valid(rx1) || !valid(rx2)) {
        return false;
    }
    left = Easing::bezier(static_cast<float>(std::clamp(lx1, 0.0, 1.0)), static_cast<float>(p01.y / m.y),
                          static_cast<float>(std::clamp(lx2, 0.0, 1.0)), static_cast<float>(p012.y / m.y));
    right = Easing::bezier(
        static_cast<float>(std::clamp(rx1, 0.0, 1.0)), static_cast<float>((p123.y - m.y) / (1 - m.y)),
        static_cast<float>(std::clamp(rx2, 0.0, 1.0)), static_cast<float>((p23.y - m.y) / (1 - m.y)));
    return true;
}

/// Cuts segment a -> b at `tc` (strictly inside): the value there and the easings of both parts.
bool cutSegment(const LoweredKey& a, const LoweredKey& b, double tc, LoweredKey& leftOut, LoweredKey& cut) {
    const double p = (tc - a.time) / (b.time - a.time);
    leftOut = a;
    cut = a;
    cut.time = tc;
    switch (a.interp) {
        case LoweredInterp::Linear:
            cut.value = a.value + (b.value - a.value) * p;
            return true;
        case LoweredInterp::Hold:
            cut.value = a.value;
            return true;
        case LoweredInterp::Easing:
            break;
    }
    if (a.easing.kind != EasingKind::CubicBezier) {
        return false;
    }
    Easing left;
    Easing right;
    double y = 0;
    if (!splitBezier(a.easing, p, left, right, y)) {
        return false;
    }
    leftOut.easing = left;
    cut.easing = right;
    cut.value = a.value + (b.value - a.value) * y;
    return true;
}

/// A key placed kExprJumpSeconds after a jump starts a segment that really starts at the jump: cut
/// that segment there so the rest of it keeps its original timing (the jump itself is the only
/// approximation).
void fixJumps(KeyList& list) {
    for (size_t j : list.jumpKeys) {
        if (!list.ok || j + 1 >= list.keys.size()) {
            continue;
        }
        LoweredKey start = list.keys[j];
        start.time -= kExprJumpSeconds;
        LoweredKey left;
        LoweredKey cut;
        if (!cutSegment(start, list.keys[j + 1], list.keys[j].time, left, cut)) {
            list.ok = false;
            return;
        }
        list.keys[j] = cut;
    }
    list.jumpKeys.clear();
}

}  // namespace

LoweredKey loweredFromRow(double time, double value, const Easing& e) {
    LoweredKey k;
    k.time = time;
    k.value = value;
    k.easing = e;
    k.interp = e.kind == EasingKind::Linear
                   ? LoweredInterp::Linear
                   : (e.kind == EasingKind::Hold ? LoweredInterp::Hold : LoweredInterp::Easing);
    return k;
}

bool simpleShape(const Program& prog, int32_t& finalExpr, int32_t& prefixCount) {
    const Node& root = prog.nodes[static_cast<size_t>(prog.root)];
    int32_t last = -1;
    for (int32_t i = 0; i < root.count; ++i) {
        const Node& s = prog.nodes[static_cast<size_t>(prog.lists[static_cast<size_t>(root.first + i)])];
        if (s.kind == NodeKind::ExprStmt) {
            if (last >= 0) {
                return false;
            }
            last = i;
        } else if (s.kind == NodeKind::VarDecl || s.kind == NodeKind::Function || s.kind == NodeKind::Empty) {
            if (last >= 0) {
                return false;  // something after the final expression
            }
        } else {
            return false;
        }
    }
    if (last < 0) {
        return false;
    }
    // Every global assigned exactly once (by its declaration).
    std::vector<int> writes(prog.globalNames.size(), 0);
    for (const Node& n : prog.nodes) {
        const Node* target = nullptr;
        if (n.kind == NodeKind::VarDecl) {
            target = &n;
        } else if (n.kind == NodeKind::Assign || n.kind == NodeKind::IncDec) {
            target = &prog.nodes[static_cast<size_t>(n.a)];
            if (target->kind == NodeKind::Element) {
                target = &prog.nodes[static_cast<size_t>(target->a)];
            }
            if (target->kind != NodeKind::Var) {
                continue;
            }
        } else {
            continue;
        }
        if ((target->flags & kFlagLocal) == 0 && target->slot >= 0) {
            ++writes[static_cast<size_t>(target->slot)];
        }
    }
    for (int32_t i = 0; i < last; ++i) {
        const Node& s = prog.nodes[static_cast<size_t>(prog.lists[static_cast<size_t>(root.first + i)])];
        if (s.kind == NodeKind::VarDecl && (s.a < 0 || writes[static_cast<size_t>(s.slot)] != 1)) {
            return false;
        }
    }
    finalExpr = prog.nodes[static_cast<size_t>(prog.lists[static_cast<size_t>(root.first + last)])].a;
    prefixCount = last;
    return true;
}

namespace {

TimeClass classify(const Program& prog, int32_t id, bool constantRow, int depth) {
    if (depth > 64) {
        return TimeClass::Other;
    }
    const Node& n = prog.nodes[static_cast<size_t>(id)];
    auto sub = [&](int32_t c) { return classify(prog, c, constantRow, depth + 1); };
    auto worst = [](TimeClass a, TimeClass b) { return a > b ? a : b; };
    switch (n.kind) {
        case NodeKind::Number:
        case NodeKind::Index:
        case NodeKind::Duration:
            return TimeClass::Const;
        case NodeKind::Time:
            return TimeClass::Linear;
        case NodeKind::Value:
            return constantRow ? TimeClass::Const : TimeClass::Other;
        case NodeKind::Var: {
            if ((n.flags & kFlagLocal) != 0) {
                return TimeClass::Other;
            }
            const Node& root = prog.nodes[static_cast<size_t>(prog.root)];
            for (int32_t i = 0; i < root.count; ++i) {
                const Node& s =
                    prog.nodes[static_cast<size_t>(prog.lists[static_cast<size_t>(root.first + i)])];
                if (s.kind == NodeKind::VarDecl && s.slot == n.slot && s.a >= 0) {
                    return sub(s.a);
                }
            }
            return TimeClass::Other;
        }
        case NodeKind::Unary: {
            const TimeClass a = sub(n.a);
            if (static_cast<UnaryOp>(n.op) == UnaryOp::Not) {
                return a == TimeClass::Const ? TimeClass::Const : TimeClass::Other;
            }
            return a;
        }
        case NodeKind::Binary: {
            const TimeClass a = sub(n.a);
            const TimeClass b = sub(n.b);
            switch (static_cast<BinaryOp>(n.op)) {
                case BinaryOp::Add:
                case BinaryOp::Sub:
                    return worst(a, b);
                case BinaryOp::Mul:
                    if (a == TimeClass::Const || b == TimeClass::Const) {
                        return worst(a, b);
                    }
                    return TimeClass::Other;
                case BinaryOp::Div:
                    return b == TimeClass::Const ? a : TimeClass::Other;
                default:
                    return a == TimeClass::Const && b == TimeClass::Const ? TimeClass::Const
                                                                          : TimeClass::Other;
            }
        }
        case NodeKind::Logical:
        case NodeKind::Conditional: {
            TimeClass w = worst(sub(n.a), sub(n.b));
            if (n.c >= 0) {
                w = worst(w, sub(n.c));
            }
            return w == TimeClass::Const ? TimeClass::Const : TimeClass::Other;
        }
        case NodeKind::Call: {
            const Builtin b = static_cast<Builtin>(n.op);
            switch (b) {
                case Builtin::Wiggle:
                case Builtin::LoopOut:
                case Builtin::LoopIn:
                case Builtin::Random:
                case Builtin::SeedRandom:
                case Builtin::PosterizeTime:
                case Builtin::Port:
                case Builtin::RandomPerPlay:
                    return TimeClass::Other;
                default:
                    break;
            }
            TimeClass w = TimeClass::Const;
            for (int32_t i = 0; i < n.count; ++i) {
                w = worst(w, sub(prog.lists[static_cast<size_t>(n.first + i)]));
            }
            return w == TimeClass::Const ? TimeClass::Const : TimeClass::Other;
        }
        default:
            return TimeClass::Other;
    }
}

}  // namespace

TimeClass classifyTime(const Program& prog, int32_t id, bool constantRow) {
    return classify(prog, id, constantRow, 0);
}

bool expandLoop(const RowSampler& row, bool out, LoopType type, int numKeyframes, double duration,
                size_t maxKeys, KeyList& list) {
    const std::span<const ExprKey> k = row.row().keys;
    const size_t last = k.size() - 1;
    if (out) {
        appendForward(list, k, 0, last, k[0].time, 0.0);
        if (last == 0) {
            return list.ok;
        }
        const size_t s = row.loopOutStart(numKeyframes);
        const double t0 = k[s].time;
        const double t1 = k[last].time;
        const double period = t1 - t0;
        if (type == LoopType::Continue) {
            if (duration > t1) {
                list.keys.back().interp = LoweredInterp::Linear;
                list.keys.back().easing = Easing::linear();
                appendKey(list, duration, k[last].value + row.slopeInto(last) * (duration - t1),
                          Easing::linear());
            }
            return list.ok;
        }
        const double rise = k[last].value - k[s].value;
        for (int m = 1; list.ok && t1 + (m - 1) * period < duration; ++m) {
            const double base = t1 + (m - 1) * period;
            if (type == LoopType::PingPong && (m % 2) == 1) {
                appendReversed(list, k, s, last, base);
            } else {
                appendForward(list, k, s, last, base, type == LoopType::Offset ? m * rise : 0.0);
            }
            if (list.keys.size() > maxKeys) {
                return false;
            }
        }
        fixJumps(list);
        return list.ok;
    }
    // loopIn: the blocks before the first key, earliest first, then the row.
    const size_t e = row.loopInEnd(numKeyframes);
    const double t0 = k[0].time;
    const double t1 = k[e].time;
    const double period = t1 - t0;
    if (last == 0 || !(period > 0) || !(t0 > 0)) {
        appendForward(list, k, 0, last, k[0].time, 0.0);
        return list.ok;
    }
    if (type == LoopType::Continue) {
        appendKey(list, 0.0, k[0].value - row.slopeOutOf(0) * t0, Easing::linear());
        appendForward(list, k, 0, last, k[0].time, 0.0);
        return list.ok;
    }
    const double blocks = std::ceil(t0 / period);
    if (blocks * static_cast<double>(e + 1) > static_cast<double>(maxKeys)) {
        return false;
    }
    const double rise = k[e].value - k[0].value;
    for (int m = static_cast<int>(blocks); m >= 1 && list.ok; --m) {
        const double base = t0 - m * period;
        if (type == LoopType::PingPong && (m % 2) == 1) {
            appendReversed(list, k, 0, e, base);
        } else {
            appendForward(list, k, 0, e, base, type == LoopType::Offset ? -m * rise : 0.0);
        }
    }
    appendForward(list, k, 0, last, k[0].time, 0.0);
    fixJumps(list);
    return list.ok && list.keys.size() <= maxKeys;
}

void shiftedKeys(const RowSampler& row, double shift, KeyList& list) {
    for (const ExprKey& k : row.row().keys) {
        list.keys.push_back(loweredFromRow(k.time + shift, k.value, k.easing));
    }
}

bool clipKeys(KeyList& list, double duration, std::span<const EasingPoint> /*points*/) {
    std::vector<LoweredKey>& keys = list.keys;
    if (keys.empty() || !list.ok) {
        return false;
    }
    // Lower bound.
    size_t i = 0;
    while (i < keys.size() && keys[i].time < 0) {
        ++i;
    }
    if (i == keys.size()) {
        const double v = keys.back().value;
        keys.assign(1, loweredFromRow(0.0, v, Easing::linear()));
        return true;
    }
    if (i > 0) {
        if (keys[i].time > 0) {
            LoweredKey left;
            LoweredKey cut;
            if (!cutSegment(keys[i - 1], keys[i], 0.0, left, cut)) {
                return false;
            }
            keys[i - 1] = cut;
            keys.erase(keys.begin(), keys.begin() + static_cast<std::ptrdiff_t>(i - 1));
        } else {
            keys.erase(keys.begin(), keys.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    // Upper bound.
    if (keys.front().time > duration) {
        const double v = keys.front().value;
        keys.assign(1, loweredFromRow(0.0, v, Easing::linear()));
        return true;
    }
    size_t j = keys.size() - 1;
    while (keys[j].time > duration) {
        --j;
    }
    if (j + 1 < keys.size()) {
        if (keys[j].time < duration) {
            LoweredKey left;
            LoweredKey cut;
            if (!cutSegment(keys[j], keys[j + 1], duration, left, cut)) {
                return false;
            }
            keys[j] = left;
            keys[j + 1] = cut;
            keys[j + 1].interp = LoweredInterp::Linear;
            keys[j + 1].easing = Easing::linear();
            keys.resize(j + 2);
        } else {
            keys.resize(j + 1);
        }
    }
    keys.back().interp = LoweredInterp::Linear;  // the last key's easing is never used
    keys.back().easing = Easing::linear();
    return true;
}

}  // namespace xt::compile::expr
