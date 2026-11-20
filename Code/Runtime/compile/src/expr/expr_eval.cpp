#include "expr_eval.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "expr_lexer.h"
#include "xt/expr_noise.h"

/// See expr_eval.h. Docs/design/58-value-functions-and-drivers.md §58.2 (names and functions),
/// §58.3 (seeds). Values are numbers or arrays of up to 4 numbers; booleans are 1 / 0. Any
/// non-finite intermediate result is an error (`expr.eval`), never a silent NaN.

namespace xt::compile::expr {

namespace {

/// The salt mixed into the expression seed for random() (so random and wiggle at the same site differ).
constexpr uint32_t kRandomSalt = 0x52414E44u;

int32_t toIntSat(double v) {
    if (!(v == v)) {
        return 0;
    }
    if (v <= -2147483648.0) {
        return INT32_MIN;
    }
    if (v >= 2147483647.0) {
        return INT32_MAX;
    }
    return static_cast<int32_t>(v);
}

}  // namespace

struct Evaluator::DepthGuard {
    Evaluator& e;
    bool ok;
    explicit DepthGuard(Evaluator& ev) : e(ev), ok(++ev.m_depth <= kMaxEvalDepth) {}
    ~DepthGuard() { --e.m_depth; }
    DepthGuard(const DepthGuard&) = delete;
    DepthGuard& operator=(const DepthGuard&) = delete;
};

/// A row of another element (layer(...).<row>) the resolver returned, sampled like this row.
struct Evaluator::RefSampler {
    int32_t node = -1;
    ExprLayerKey key;
    ExprRefKind kind = ExprRefKind::NotFound;
    RowSampler sampler;
};

Evaluator::Evaluator(const Program& prog, const ExprContext& ctx, const RowSampler& row,
                     const std::vector<RowSampler>& vec, std::vector<Diagnostic>& diags)
    : m_prog(prog), m_ctx(ctx), m_row(row), m_vec(vec), m_diags(diags) {}

Evaluator::~Evaluator() = default;

void Evaluator::reset(double t) {
    m_slots.assign(m_prog.globalNames.size(), Val());
    m_frameBase = 0;
    m_callDepth = 0;
    m_depth = 0;
    m_iterations = 0;
    m_time = t;
    m_postFps = -1;
    m_posterized = false;
    m_randSeed = exprMixSeed(m_ctx.seed, kRandomSalt);
    m_randCalls = 0;
    m_timeless = false;
    m_framePoison = false;
    m_last = Val();
    m_ret = Val();
    m_status = ExprStatus::Ok;
}

ExprStatus Evaluator::run(double t, Val& result) {
    m_poisonMode = false;
    reset(t);
    if (m_prog.root < 0) {
        return ExprStatus::Error;
    }
    exec(m_prog.root);
    if (m_status != ExprStatus::Ok) {
        return m_status;
    }
    if (m_last.kind == VKind::Undefined) {
        fail(m_prog.nodes[static_cast<size_t>(m_prog.root)], "expr.eval",
             "the expression produces no value: end it with the value to use");
        return m_status;
    }
    result = m_last;
    return ExprStatus::Ok;
}

ExprStatus Evaluator::runPrefixValue(double t, int32_t stmtCount, int32_t slot, Val& out) {
    m_poisonMode = true;
    reset(t);
    const Node& root = m_prog.nodes[static_cast<size_t>(m_prog.root)];
    const int32_t n = std::min(stmtCount, root.count);
    for (int32_t i = 0; i < n && m_status == ExprStatus::Ok; ++i) {
        exec(m_prog.lists[static_cast<size_t>(root.first + i)]);
    }
    m_poisonMode = false;
    if (m_status == ExprStatus::Ok) {
        out = slot < 0 ? m_last : m_slots[static_cast<size_t>(slot)];
    }
    return m_status;
}

ExprStatus Evaluator::runPrefix(double t, int32_t stmtCount, int32_t slot, int comp, double& out) {
    Val v;
    const ExprStatus st = runPrefixValue(t, stmtCount, slot, v);
    if (st != ExprStatus::Ok) {
        return st;
    }
    if (v.kind == VKind::Number && comp == 0) {
        out = v.v[0];
        return ExprStatus::Ok;
    }
    if (v.kind == VKind::Array && comp >= 0 && comp < v.len) {
        out = v.v[comp];
        return ExprStatus::Ok;
    }
    fail(m_prog.nodes[static_cast<size_t>(m_prog.root)], "expr.eval",
         "a loop's result depends on a run-time input");
    return m_status;
}

ExprStatus Evaluator::evalAfterPrefix(double t, int32_t stmtCount, int32_t node, Val& out) {
    m_poisonMode = false;
    reset(t);
    const Node& root = m_prog.nodes[static_cast<size_t>(m_prog.root)];
    const int32_t n = std::min(stmtCount, root.count);
    for (int32_t i = 0; i < n && m_status == ExprStatus::Ok; ++i) {
        exec(m_prog.lists[static_cast<size_t>(root.first + i)]);
    }
    if (m_status == ExprStatus::Ok) {
        eval(node, out);
    }
    return m_status;
}

bool Evaluator::fail(const Node& at, const char* code, std::string msg) {
    if (m_status == ExprStatus::Ok) {
        m_status = ExprStatus::Error;
        if (!m_reported) {
            m_reported = true;
            addExprDiagnostic(m_diags, code, std::move(msg), at.line, at.col);
        }
    }
    return false;
}

double Evaluator::effTime() const {
    if (m_postFps < 0) {
        return m_time;
    }
    if (m_postFps == 0) {
        return 0.0;
    }
    return std::floor(m_time * m_postFps + 1e-9) / m_postFps;
}

Val& Evaluator::slotRef(const Node& varNode) {
    const size_t i = (varNode.flags & kFlagLocal) != 0 ? m_frameBase + static_cast<size_t>(varNode.slot)
                                                       : static_cast<size_t>(varNode.slot);
    return m_slots[i];
}

bool Evaluator::finite(const Node& at, Val& v) {
    for (int i = 0; i < v.len; ++i) {
        if (!std::isfinite(v.v[i])) {
            return fail(at, "expr.eval", "the result is not a finite number");
        }
    }
    return true;
}

bool Evaluator::truthy(const Node& at, const Val& v, bool& out) {
    if (v.kind == VKind::Number) {
        out = v.v[0] != 0.0;
        return true;
    }
    if (v.kind == VKind::Array) {
        return fail(at, "expr.eval", "an array cannot be a condition");
    }
    return fail(at, "expr.eval", "a condition has no value");
}

// ---- statements ------------------------------------------------------------------------------

Evaluator::Flow Evaluator::exec(int32_t id) {
    DepthGuard guard(*this);
    const Node& n = m_prog.nodes[static_cast<size_t>(id)];
    if (!guard.ok) {
        fail(n, "expr.limit", "the expression nests too deeply when run");
        return Flow::Stop;
    }
    if (m_status != ExprStatus::Ok) {
        return Flow::Stop;
    }
    switch (n.kind) {
        case NodeKind::Block:
            for (int32_t i = 0; i < n.count; ++i) {
                const Flow f = exec(m_prog.lists[static_cast<size_t>(n.first + i)]);
                if (f != Flow::Normal) {
                    return f;
                }
            }
            return Flow::Normal;
        case NodeKind::VarDecl:
            if (n.a >= 0) {
                Val v;
                if (!eval(n.a, v)) {
                    return Flow::Stop;
                }
                slotRef(n) = v;
            }
            return Flow::Normal;
        case NodeKind::ExprStmt: {
            Val v;
            if (!eval(n.a, v)) {
                return Flow::Stop;
            }
            if (m_callDepth == 0 && v.kind != VKind::Undefined) {
                m_last = v;
            }
            return Flow::Normal;
        }
        case NodeKind::If: {
            Val c;
            if (!eval(n.a, c)) {
                return Flow::Stop;
            }
            if (c.kind == VKind::Poison) {
                poisonAssignments(id);
                return Flow::Normal;
            }
            bool yes = false;
            if (!truthy(n, c, yes)) {
                return Flow::Stop;
            }
            if (yes) {
                return exec(n.b);
            }
            return n.c >= 0 ? exec(n.c) : Flow::Normal;
        }
        case NodeKind::For:
        case NodeKind::While:
            return execLoop(n);
        case NodeKind::Break:
            return Flow::Break;
        case NodeKind::Continue:
            return Flow::Continue;
        case NodeKind::Return:
            if (n.a >= 0) {
                Val r;
                if (!eval(n.a, r)) {
                    return Flow::Stop;
                }
                m_ret = r;
            } else {
                m_ret = Val();
            }
            return Flow::Return;
        case NodeKind::Function:
        case NodeKind::Empty:
            return Flow::Normal;
        default:
            fail(n, "expr.syntax", "not a statement");
            return Flow::Stop;
    }
}

Evaluator::Flow Evaluator::execLoop(const Node& n) {
    const int32_t self = static_cast<int32_t>(&n - m_prog.nodes.data());
    if (n.kind == NodeKind::For && n.a >= 0) {
        const Flow f = exec(n.a);
        if (f != Flow::Normal) {
            return f;
        }
    }
    const int32_t cond = n.kind == NodeKind::For ? n.b : n.a;
    while (true) {
        if (cond >= 0) {
            Val c;
            if (!eval(cond, c)) {
                return Flow::Stop;
            }
            if (c.kind == VKind::Poison) {
                poisonAssignments(self);
                return Flow::Normal;
            }
            bool yes = false;
            if (!truthy(n, c, yes)) {
                return Flow::Stop;
            }
            if (!yes) {
                break;
            }
        }
        if (++m_iterations > kExprMaxIterations) {
            fail(n, "expr.limit", "the loops run more than 10000 iterations");
            return Flow::Stop;
        }
        const Flow f = exec(n.d);
        if (f == Flow::Break) {
            break;
        }
        if (f == Flow::Return || f == Flow::Stop) {
            return f;
        }
        if (n.kind == NodeKind::For && n.c >= 0) {
            Val ignored;
            if (!eval(n.c, ignored)) {
                return Flow::Stop;
            }
        }
    }
    return Flow::Normal;
}

void Evaluator::poisonAssignments(int32_t id) {
    const Node& n = m_prog.nodes[static_cast<size_t>(id)];
    if (m_callDepth > 0) {
        m_framePoison = true;
    }
    switch (n.kind) {
        case NodeKind::Assign:
        case NodeKind::IncDec: {
            const Node* t = &m_prog.nodes[static_cast<size_t>(n.a)];
            if (t->kind == NodeKind::Element) {
                t = &m_prog.nodes[static_cast<size_t>(t->a)];
            }
            if (t->kind == NodeKind::Var) {
                slotRef(*t) = Val::poison();
            }
            break;
        }
        case NodeKind::VarDecl:
            slotRef(n) = Val::poison();
            break;
        case NodeKind::ExprStmt:
            if (m_callDepth == 0) {
                m_last = Val::poison();
            }
            break;
        case NodeKind::LocalCall: {
            // Globals the callee assigns (its locals do not outlive the call).
            const Function& f = m_prog.functions[static_cast<size_t>(n.slot)];
            std::vector<int32_t> stack{f.body};
            while (!stack.empty()) {
                const Node& c = m_prog.nodes[static_cast<size_t>(stack.back())];
                stack.pop_back();
                if (c.kind == NodeKind::Assign || c.kind == NodeKind::IncDec) {
                    const Node* t = &m_prog.nodes[static_cast<size_t>(c.a)];
                    if (t->kind == NodeKind::Element) {
                        t = &m_prog.nodes[static_cast<size_t>(t->a)];
                    }
                    if (t->kind == NodeKind::Var && (t->flags & kFlagLocal) == 0) {
                        m_slots[static_cast<size_t>(t->slot)] = Val::poison();
                    }
                }
                for (int32_t k : {c.a, c.b, c.c, c.d}) {
                    if (k >= 0) {
                        stack.push_back(k);
                    }
                }
                if (c.kind != NodeKind::Function) {
                    for (int32_t i = 0; i < c.count; ++i) {
                        stack.push_back(m_prog.lists[static_cast<size_t>(c.first + i)]);
                    }
                }
            }
            break;
        }
        default:
            break;
    }
    for (int32_t k : {n.a, n.b, n.c, n.d}) {
        if (k >= 0) {
            poisonAssignments(k);
        }
    }
    if (n.kind != NodeKind::Function) {
        for (int32_t i = 0; i < n.count; ++i) {
            poisonAssignments(m_prog.lists[static_cast<size_t>(n.first + i)]);
        }
    }
}

// ---- expressions -----------------------------------------------------------------------------

bool Evaluator::evalNumber(int32_t id, double& out, const char* what) {
    Val v;
    if (!eval(id, v)) {
        return false;
    }
    if (v.kind != VKind::Number) {
        const Node& n = m_prog.nodes[static_cast<size_t>(id)];
        if (v.kind == VKind::Poison) {
            return fail(n, "expr.eval", std::string(what) + " depends on a run-time input");
        }
        return fail(n, "expr.eval",
                    std::string(what) +
                        (v.kind == VKind::Array ? " must be a number, not an array" : " has no value"));
    }
    out = v.v[0];
    return true;
}

bool Evaluator::readTarget(const Node& target, Val& out) {
    if (target.kind == NodeKind::Var) {
        out = slotRef(target);
        return true;
    }
    return eval(static_cast<int32_t>(&target - m_prog.nodes.data()), out);
}

bool Evaluator::writeTarget(const Node& target, const Val& v) {
    if (target.kind == NodeKind::Var) {
        slotRef(target) = v;
        return true;
    }
    // Element: `name[i] = v` on an array variable.
    const Node& arr = m_prog.nodes[static_cast<size_t>(target.a)];
    if (arr.kind != NodeKind::Var) {
        return fail(target, "expr.unsupported", "only an element of an array variable can be assigned");
    }
    double idx = 0;
    if (!evalNumber(target.b, idx, "an index")) {
        return false;
    }
    Val& slot = slotRef(arr);
    if (slot.kind == VKind::Poison || v.kind == VKind::Poison) {
        slot = Val::poison();
        return true;
    }
    if (slot.kind != VKind::Array) {
        return fail(target, "expr.eval", "only an array has elements");
    }
    if (v.kind != VKind::Number) {
        return fail(target, "expr.eval", "an array element must be a number");
    }
    if (idx != std::floor(idx) || idx < 0 || idx >= slot.len) {
        return fail(target, "expr.eval", "the index is outside the array");
    }
    slot.v[static_cast<int>(idx)] = v.v[0];
    return true;
}

bool Evaluator::evalBinary(const Node& n, BinaryOp op, const Val& a, const Val& b, Val& out) {
    if (a.kind == VKind::Poison || b.kind == VKind::Poison) {
        out = Val::poison();
        return true;
    }
    if (a.kind == VKind::Undefined || b.kind == VKind::Undefined) {
        return fail(n, "expr.eval", "a value is used before it is set");
    }
    const bool aArr = a.kind == VKind::Array;
    const bool bArr = b.kind == VKind::Array;
    if (aArr || bArr) {
        out = Val();
        out.kind = VKind::Array;
        if ((op == BinaryOp::Add || op == BinaryOp::Sub) && aArr && bArr) {
            out.len = std::max(a.len, b.len);
            for (int i = 0; i < out.len; ++i) {
                const double x = i < a.len ? a.v[i] : 0.0;
                const double y = i < b.len ? b.v[i] : 0.0;
                out.v[i] = op == BinaryOp::Add ? x + y : x - y;
            }
            return finite(n, out);
        }
        if (op == BinaryOp::Mul && aArr != bArr) {
            const Val& arr = aArr ? a : b;
            const double k = aArr ? b.v[0] : a.v[0];
            out.len = arr.len;
            for (int i = 0; i < out.len; ++i) {
                out.v[i] = arr.v[i] * k;
            }
            return finite(n, out);
        }
        if (op == BinaryOp::Div && aArr && !bArr) {
            if (b.v[0] == 0.0) {
                return fail(n, "expr.eval", "division by zero");
            }
            out.len = a.len;
            for (int i = 0; i < out.len; ++i) {
                out.v[i] = a.v[i] / b.v[0];
            }
            return finite(n, out);
        }
        return fail(n, "expr.eval",
                    "arrays can be added to / subtracted from arrays, and multiplied or divided by a number");
    }
    const double x = a.v[0];
    const double y = b.v[0];
    double r = 0;
    switch (op) {
        case BinaryOp::Add:
            r = x + y;
            break;
        case BinaryOp::Sub:
            r = x - y;
            break;
        case BinaryOp::Mul:
            r = x * y;
            break;
        case BinaryOp::Div:
            if (y == 0.0) {
                return fail(n, "expr.eval", "division by zero");
            }
            r = x / y;
            break;
        case BinaryOp::Mod:
            if (y == 0.0) {
                return fail(n, "expr.eval", "remainder of a division by zero");
            }
            r = std::fmod(x, y);
            break;
        case BinaryOp::Lt:
            r = x < y ? 1 : 0;
            break;
        case BinaryOp::Le:
            r = x <= y ? 1 : 0;
            break;
        case BinaryOp::Gt:
            r = x > y ? 1 : 0;
            break;
        case BinaryOp::Ge:
            r = x >= y ? 1 : 0;
            break;
        case BinaryOp::Eq:
            r = x == y ? 1 : 0;
            break;
        case BinaryOp::Ne:
            r = x != y ? 1 : 0;
            break;
    }
    out = Val::number(r);
    return finite(n, out);
}

bool Evaluator::eval(int32_t id, Val& out) {
    DepthGuard guard(*this);
    const Node& n = m_prog.nodes[static_cast<size_t>(id)];
    if (!guard.ok) {
        return fail(n, "expr.limit", "the expression nests too deeply when run");
    }
    if (m_status != ExprStatus::Ok) {
        return false;
    }
    switch (n.kind) {
        case NodeKind::Number:
            out = Val::number(n.number);
            return true;
        case NodeKind::String:
            return fail(n, "expr.eval", "a string is not a number");
        case NodeKind::Var: {
            out = slotRef(n);
            if (out.kind == VKind::Undefined) {
                const std::string& name = (n.flags & kFlagLocal) != 0
                                              ? std::string("a local variable")
                                              : "'" + m_prog.globalNames[static_cast<size_t>(n.slot)] + "'";
                return fail(n, "expr.eval", name + " is used before it is set");
            }
            return true;
        }
        case NodeKind::Time:
            out = Val::number(effTime());
            return true;
        case NodeKind::Value:
            out = Val::number(m_row.sample(effTime()));
            return true;
        case NodeKind::ValueVec: {
            if (m_vec.size() < 2) {
                return fail(n, "expr.eval", "valueVec needs a property with X and Y rows");
            }
            out = Val();
            out.kind = VKind::Array;
            out.len = static_cast<uint8_t>(std::min<size_t>(m_vec.size(), 4));
            for (int i = 0; i < out.len; ++i) {
                out.v[i] = m_vec[static_cast<size_t>(i)].sample(effTime());
            }
            return true;
        }
        case NodeKind::Index:
            out = Val::number(static_cast<double>(m_ctx.index));
            return true;
        case NodeKind::Duration:
            out = Val::number(m_ctx.duration);
            return true;
        case NodeKind::Unary: {
            Val a;
            if (!eval(n.a, a)) {
                return false;
            }
            if (a.kind == VKind::Poison) {
                out = a;
                return true;
            }
            const UnaryOp op = static_cast<UnaryOp>(n.op);
            if (op == UnaryOp::Not) {
                bool yes = false;
                if (!truthy(n, a, yes)) {
                    return false;
                }
                out = Val::number(yes ? 0 : 1);
                return true;
            }
            if (a.kind == VKind::Undefined) {
                return fail(n, "expr.eval", "a value is used before it is set");
            }
            out = a;
            if (op == UnaryOp::Neg) {
                for (int i = 0; i < out.len; ++i) {
                    out.v[i] = -out.v[i];
                }
            }
            return true;
        }
        case NodeKind::Binary: {
            Val a;
            Val b;
            if (!eval(n.a, a) || !eval(n.b, b)) {
                return false;
            }
            return evalBinary(n, static_cast<BinaryOp>(n.op), a, b, out);
        }
        case NodeKind::Logical: {
            Val a;
            if (!eval(n.a, a)) {
                return false;
            }
            if (a.kind == VKind::Poison) {
                out = a;
                return true;
            }
            bool yes = false;
            if (!truthy(n, a, yes)) {
                return false;
            }
            const bool isOr = static_cast<LogicalOp>(n.op) == LogicalOp::Or;
            if (yes == isOr) {
                out = a;
                return true;
            }
            return eval(n.b, out);
        }
        case NodeKind::Conditional: {
            Val c;
            if (!eval(n.a, c)) {
                return false;
            }
            if (c.kind == VKind::Poison) {
                out = c;
                return true;
            }
            bool yes = false;
            if (!truthy(n, c, yes)) {
                return false;
            }
            return eval(yes ? n.b : n.c, out);
        }
        case NodeKind::Call:
            return evalCall(n, out);
        case NodeKind::LocalCall:
            return evalLocalCall(n, out);
        case NodeKind::Array: {
            out = Val();
            out.kind = VKind::Array;
            out.len = static_cast<uint8_t>(n.count);
            for (int32_t i = 0; i < n.count; ++i) {
                Val e;
                if (!eval(m_prog.lists[static_cast<size_t>(n.first + i)], e)) {
                    return false;
                }
                if (e.kind == VKind::Poison) {
                    out = e;
                    return true;
                }
                if (e.kind != VKind::Number) {
                    return fail(n, "expr.eval", "an array element must be a number");
                }
                out.v[i] = e.v[0];
            }
            return true;
        }
        case NodeKind::Element: {
            Val a;
            if (!eval(n.a, a)) {
                return false;
            }
            double idx = 0;
            if (!evalNumber(n.b, idx, "an index")) {
                return false;
            }
            if (a.kind == VKind::Poison) {
                out = a;
                return true;
            }
            if (a.kind != VKind::Array) {
                return fail(n, "expr.eval", "only an array has elements");
            }
            if (idx != std::floor(idx) || idx < 0 || idx >= a.len) {
                return fail(n, "expr.eval", "the index is outside the array");
            }
            out = Val::number(a.v[static_cast<int>(idx)]);
            return true;
        }
        case NodeKind::ArrayLength: {
            Val a;
            if (!eval(n.a, a)) {
                return false;
            }
            if (a.kind == VKind::Poison) {
                out = a;
                return true;
            }
            if (a.kind != VKind::Array) {
                return fail(n, "expr.eval", ".length needs an array");
            }
            out = Val::number(a.len);
            return true;
        }
        case NodeKind::Assign: {
            const Node& target = m_prog.nodes[static_cast<size_t>(n.a)];
            Val rhs;
            if (!eval(n.b, rhs)) {
                return false;
            }
            const AssignOp op = static_cast<AssignOp>(n.op);
            if (op != AssignOp::Set) {
                Val old;
                if (!readTarget(target, old)) {
                    return false;
                }
                static constexpr BinaryOp kMap[] = {BinaryOp::Add, BinaryOp::Add, BinaryOp::Sub,
                                                    BinaryOp::Mul, BinaryOp::Div, BinaryOp::Mod};
                Val r;
                if (!evalBinary(n, kMap[static_cast<int>(op)], old, rhs, r)) {
                    return false;
                }
                rhs = r;
            }
            if (!writeTarget(target, rhs)) {
                return false;
            }
            out = rhs;
            return true;
        }
        case NodeKind::IncDec: {
            const Node& target = m_prog.nodes[static_cast<size_t>(n.a)];
            Val old;
            if (!readTarget(target, old)) {
                return false;
            }
            if (old.kind == VKind::Poison) {
                out = old;
                return writeTarget(target, old);
            }
            if (old.kind != VKind::Number) {
                return fail(n, "expr.eval", "++ / -- need a number");
            }
            const IncDecOp op = static_cast<IncDecOp>(n.op);
            const bool inc = op == IncDecOp::PreInc || op == IncDecOp::PostInc;
            const Val updated = Val::number(old.v[0] + (inc ? 1.0 : -1.0));
            if (!writeTarget(target, updated)) {
                return false;
            }
            out = (op == IncDecOp::PreInc || op == IncDecOp::PreDec) ? updated : old;
            return true;
        }
        case NodeKind::LayerProp:
        case NodeKind::LayerPropAt:
            return evalLayer(n, out);
        case NodeKind::LayerLayout:
        case NodeKind::PortAt:
            return runtimeLeaf(n, out);
        default:
            return fail(n, "expr.syntax", "not an expression");
    }
}

bool Evaluator::evalLocalCall(const Node& n, Val& out) {
    const Function& f = m_prog.functions[static_cast<size_t>(n.slot)];
    std::vector<Val> args(static_cast<size_t>(n.count));
    for (int32_t i = 0; i < n.count; ++i) {
        if (!eval(m_prog.lists[static_cast<size_t>(n.first + i)], args[static_cast<size_t>(i)])) {
            return false;
        }
    }
    const size_t saveBase = m_frameBase;
    const bool savePoison = m_framePoison;
    const size_t base = m_slots.size();
    m_slots.resize(base + static_cast<size_t>(f.localCount));
    for (size_t i = 0; i < args.size(); ++i) {
        m_slots[base + i] = args[i];
    }
    m_frameBase = base;
    m_framePoison = false;
    ++m_callDepth;
    m_ret = Val();
    const Flow flow = exec(f.body);
    --m_callDepth;
    const bool poisoned = m_framePoison;
    m_frameBase = saveBase;
    m_framePoison = savePoison;
    m_slots.resize(base);
    if (flow == Flow::Stop) {
        return false;
    }
    out = poisoned ? Val::poison() : (flow == Flow::Return ? m_ret : Val());
    return true;
}

bool Evaluator::layerKey(const Node& keyNode, ExprLayerKey& key) {
    if (keyNode.kind == NodeKind::String) {
        key.byIndex = false;
        key.name = m_prog.strings[static_cast<size_t>(keyNode.slot)];
        return true;
    }
    double idx = 0;
    if (!evalNumber(static_cast<int32_t>(&keyNode - m_prog.nodes.data()), idx, "a layer index")) {
        return false;
    }
    key.byIndex = true;
    key.index = toIntSat(std::floor(idx));
    return true;
}

bool Evaluator::evalLayer(const Node& n, Val& out) {
    ExprLayerKey key;
    if (!layerKey(m_prog.nodes[static_cast<size_t>(n.a)], key)) {
        return false;
    }
    const int32_t self = static_cast<int32_t>(&n - m_prog.nodes.data());
    const std::string& prop = m_prog.strings[static_cast<size_t>(n.slot)];
    RefSampler* ref = nullptr;
    for (auto& r : m_refs) {
        if (r->node == self && r->key.byIndex == key.byIndex && r->key.name == key.name &&
            r->key.index == key.index) {
            ref = r.get();
            break;
        }
    }
    if (ref == nullptr) {
        auto made = std::make_unique<RefSampler>();
        made->node = self;
        made->key = key;
        if (m_ctx.otherRows) {
            const ExprRowRef rr = m_ctx.otherRows(key, prop);
            made->kind = rr.kind;
            if (rr.kind == ExprRefKind::Row) {
                std::vector<Diagnostic> ignored;
                if (!made->sampler.init(rr.row, ignored)) {
                    made->kind = ExprRefKind::NotFound;
                }
            }
        }
        ref = made.get();
        m_refs.push_back(std::move(made));
    }
    double t = effTime();
    if (n.kind == NodeKind::LayerPropAt && !evalNumber(n.b, t, "valueAtTime's time")) {
        return false;
    }
    switch (ref->kind) {
        case ExprRefKind::Row:
            out = Val::number(ref->sampler.sample(t));
            return true;
        case ExprRefKind::Runtime:
            if (m_poisonMode) {
                out = Val::poison();
                return true;
            }
            if (m_ctx.runtimeInputs != nullptr && m_ctx.runtimeInputs->layerProp) {
                out = Val::number(m_ctx.runtimeInputs->layerProp(key, prop, t));
                return finite(n, out);
            }
            m_status = ExprStatus::NeedsRuntime;
            return false;
        case ExprRefKind::NotFound:
            break;
    }
    return fail(n, "expr.ref",
                m_ctx.otherRows
                    ? "layer(...)." + prop + " does not name a row of this timeline"
                    : std::string("references to other rows (layer(...)) are not supported here"));
}

bool Evaluator::runtimeLeaf(const Node& n, Val& out) {
    if (m_poisonMode) {
        out = Val::poison();
        return true;
    }
    const ExprRuntimeInputs* in = m_ctx.runtimeInputs;
    if (n.kind == NodeKind::PortAt) {
        double t = 0;
        if (!evalNumber(n.b, t, "valueAtTime's time")) {
            return false;
        }
        if (in != nullptr && in->port) {
            out = Val::number(in->port(
                m_prog.strings[static_cast<size_t>(m_prog.nodes[static_cast<size_t>(n.a)].slot)], t));
            return finite(n, out);
        }
    } else if (n.kind == NodeKind::LayerLayout) {
        ExprLayerKey key;
        if (!layerKey(m_prog.nodes[static_cast<size_t>(n.a)], key)) {
            return false;
        }
        if (in != nullptr && in->layout) {
            out = Val::number(in->layout(key, n.op));
            return finite(n, out);
        }
    } else if (n.kind == NodeKind::Call && static_cast<Builtin>(n.op) == Builtin::Port) {
        if (in != nullptr && in->port) {
            const Node& name = m_prog.nodes[static_cast<size_t>(m_prog.lists[static_cast<size_t>(n.first)])];
            out = Val::number(in->port(m_prog.strings[static_cast<size_t>(name.slot)], m_time));
            return finite(n, out);
        }
    } else if (n.kind == NodeKind::Call && static_cast<Builtin>(n.op) == Builtin::RandomPerPlay) {
        double lo = 0;
        double hi = 1;
        if (n.count == 2 &&
            (!evalNumber(m_prog.lists[static_cast<size_t>(n.first)], lo, "randomPerPlay's minimum") ||
             !evalNumber(m_prog.lists[static_cast<size_t>(n.first + 1)], hi, "randomPerPlay's maximum"))) {
            return false;
        }
        if (in != nullptr && in->playRandom) {
            out = Val::number(in->playRandom(n.site, lo, hi));
            return finite(n, out);
        }
    }
    m_status = ExprStatus::NeedsRuntime;
    return false;
}

double Evaluator::randomDraw() {
    const uint32_t frame = exprRandomFrame(static_cast<float>(effTime()));
    const uint32_t seed = exprRandomSeed(m_randSeed, frame, m_randCalls++, m_timeless);
    return static_cast<double>(exprRandom01(seed));
}

}  // namespace xt::compile::expr
