#include "expr_split.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "expr_lexer.h"
#include "expr_split_internal.h"
#include "xt/expr_noise.h"

/// splitRuntime(): symbolic execution of a program into expr_split.h's expanded form.
/// Docs/design/58-value-functions-and-drivers.md §58.5 ("loops and local functions are expanded at
/// compile time: what depends on time only is computed outright; with a run-time input the loop count
/// must be known at compile time ... arrays are split into components"). The expansion follows the
/// evaluator's order of evaluation step by step, so random() call numbers and the result agree with
/// evaluateExpression(); everything without time or a run-time input folds to a constant.

namespace xt::compile::expr {

namespace {

constexpr uint32_t kRandomSalt = 0x52414E44u;  // as in expr_eval.cpp

/// A symbolic value: up to four node ids. VKind::Poison marks a variable set in only one branch of a
/// condition that is not a compile-time constant (an error only when it is read).
struct SVal {
    VKind kind = VKind::Undefined;
    uint8_t len = 0;
    int32_t c[4] = {-1, -1, -1, -1};
};

bool sameSVal(const SVal& a, const SVal& b) {
    if (a.kind != b.kind || a.len != b.len) {
        return false;
    }
    for (int i = 0; i < a.len; ++i) {
        if (a.c[i] != b.c[i]) {
            return false;
        }
    }
    return true;
}

SVal scalar(int32_t id) {
    SVal v;
    v.kind = VKind::Number;
    v.len = 1;
    v.c[0] = id;
    return v;
}

/// The part of the state an `if` branch may change.
struct SState {
    std::vector<SVal> slots;
    SVal last;
    uint32_t randSeed = 0;
    uint32_t randCalls = 0;
    bool timeless = false;
    bool seeded = false;    ///< T-777: seedRandom() was called (randSeed came from seedNumber).
    double seedNumber = 0;  ///< T-777: its number.
    SVal postFps;           ///< Undefined: no posterizeTime() in force.
};

class Splitter {
public:
    Splitter(const Program& prog, const ExprContext& ctx, SplitResult& res, const RowSampler& row,
             const std::vector<RowSampler>& vec)
        : m_prog(prog),
          m_ctx(ctx),
          m_res(res),
          m_env(prog, ctx, row, vec),
          m_ev(prog, ctx, row, vec, m_evDiags) {}

    void run() {
        m_st.slots.assign(m_prog.globalNames.size(), SVal());
        m_st.randSeed = exprMixSeed(m_ctx.seed, kRandomSalt);
        const Node& root = node(m_prog.root);
        for (int32_t i = 0; i < root.count && !m_failed; ++i) {
            m_topIndex = i;
            m_topStmt = m_prog.lists[static_cast<size_t>(root.first + i)];
            exec(m_topStmt);
        }
        if (m_failed) {
            return;
        }
        if (m_st.last.kind == VKind::Poison) {
            fail(root, "expr.limit",
                 "the result is set in only one branch of a condition that is not known at compile time");
            return;
        }
        if (m_st.last.kind != VKind::Number && m_st.last.kind != VKind::Array) {
            fail(root, "expr.eval", "the expression produces no value: end it with the value to use");
            return;
        }
        m_res.isArray = m_st.last.kind == VKind::Array;
        for (int i = 0; i < m_st.last.len; ++i) {
            m_res.expanded.push_back(m_st.last.c[i]);
        }
        extractCurves();
    }

private:
    enum class Flow : uint8_t { Normal, Break, Continue, Return, Stop };

    const Node& node(int32_t id) const { return m_prog.nodes[static_cast<size_t>(id)]; }

    bool fail(const Node& at, const char* code, std::string msg) {
        if (!m_failed) {
            addExprDiagnostic(m_res.diagnostics, code, std::move(msg), at.line, at.col);
        }
        m_failed = true;
        return false;
    }

    // ---- nodes -------------------------------------------------------------------------------

    bool isConst(int32_t id) const { return m_res.nodes[static_cast<size_t>(id)].op == SplitOp::Const; }
    double constValue(int32_t id) const { return m_res.nodes[static_cast<size_t>(id)].value; }

    int32_t add(SplitNode n) {
        if (m_failed) {
            return -1;
        }
        if (m_res.nodes.size() >= kSplitMaxNodes) {
            fail(node(m_prog.root), "expr.limit", "the expression expands to too many operations");
            return -1;
        }
        bool runtime = isRuntimeLeaf(n.op);
        bool timeDep = n.op == SplitOp::Time || n.op == SplitOp::SubProgram || n.op == SplitOp::OtherRowAt;
        for (int i = 0; i < n.argc; ++i) {
            if (n.args[i] < 0) {
                return -1;
            }
            const SplitNode& a = m_res.nodes[static_cast<size_t>(n.args[i])];
            runtime = runtime || a.runtime;
            timeDep = timeDep || a.timeDep;
        }
        n.runtime = runtime;
        n.timeDep = timeDep;
        if (n.op == SplitOp::Wiggle) {
            n.aperiodic = runtime && !isConst(n.args[0]);  // T-777
        }
        if (!runtime && !timeDep && n.op != SplitOp::Const) {
            double a[5] = {0, 0, 0, 0, 0};
            for (int i = 0; i < n.argc; ++i) {
                a[i] = constValue(n.args[i]);
            }
            return konst(computeSplitNode(n, -1, a, m_env));
        }
        m_res.nodes.push_back(std::move(n));
        return static_cast<int32_t>(m_res.nodes.size() - 1);
    }

    int32_t konst(double v) {
        SplitNode n;
        n.op = SplitOp::Const;
        n.value = v;
        m_res.nodes.push_back(n);
        return static_cast<int32_t>(m_res.nodes.size() - 1);
    }

    int32_t mk(SplitOp op, std::initializer_list<int32_t> args) {
        SplitNode n;
        n.op = op;
        for (int32_t a : args) {
            n.args[n.argc++] = a;
        }
        return add(std::move(n));
    }

    int32_t select(int32_t cond, int32_t a, int32_t b) {
        if (a == b) {
            return a;
        }
        if (cond >= 0 && isConst(cond)) {
            return constValue(cond) != 0 ? a : b;
        }
        return mk(SplitOp::Select, {cond, a, b});
    }

    int32_t timeNode() {
        if (m_timeId < 0) {
            m_timeId = mk(SplitOp::Time, {});
        }
        if (m_st.postFps.kind == VKind::Number) {
            return mk(SplitOp::Posterize, {m_timeId, m_st.postFps.c[0]});
        }
        return m_timeId;
    }

    // ---- state -------------------------------------------------------------------------------

    /// Merges two branch states under `cond` into m_st.
    bool merge(const Node& at, int32_t cond, const SState& a, const SState& b) {
        m_st.slots.resize(a.slots.size());
        for (size_t i = 0; i < a.slots.size(); ++i) {
            m_st.slots[i] = mergeVal(cond, a.slots[i], b.slots[i]);
        }
        m_st.last = mergeVal(cond, a.last, b.last);
        m_st.randCalls = std::max(a.randCalls, b.randCalls);
        if (a.randSeed != b.randSeed || a.timeless != b.timeless) {
            return fail(at, "expr.limit",
                        "seedRandom() inside a condition that is not known at compile time");
        }
        if (!sameSVal(a.postFps, b.postFps)) {
            return fail(at, "expr.limit",
                        "posterizeTime() inside a condition that is not known at compile time");
        }
        m_st.randSeed = a.randSeed;
        m_st.timeless = a.timeless;
        m_st.seeded = a.seeded;
        m_st.seedNumber = a.seedNumber;
        m_st.postFps = a.postFps;
        return !m_failed;
    }

    SVal mergeVal(int32_t cond, const SVal& a, const SVal& b) {
        if (sameSVal(a, b)) {
            return a;
        }
        if (a.kind != b.kind || a.len != b.len || a.kind == VKind::Undefined || a.kind == VKind::Poison) {
            SVal p;
            p.kind = VKind::Poison;
            return p;
        }
        SVal r = a;
        for (int i = 0; i < a.len; ++i) {
            r.c[i] = select(cond, a.c[i], b.c[i]);
        }
        return r;
    }

    SVal& slot(const Node& var) {
        const size_t i = (var.flags & kFlagLocal) != 0 ? m_frameBase + static_cast<size_t>(var.slot)
                                                       : static_cast<size_t>(var.slot);
        return m_st.slots[i];
    }

    // ---- statements --------------------------------------------------------------------------

    Flow exec(int32_t id) {
        if (m_failed) {
            return Flow::Stop;
        }
        const Node& n = node(id);
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
                    SVal v;
                    if (!eval(n.a, v)) {
                        return Flow::Stop;
                    }
                    slot(n) = v;
                }
                return Flow::Normal;
            case NodeKind::ExprStmt: {
                SVal v;
                if (!eval(n.a, v)) {
                    return Flow::Stop;
                }
                if (m_callDepth == 0 && v.kind != VKind::Undefined) {
                    m_st.last = v;
                }
                return Flow::Normal;
            }
            case NodeKind::If: {
                int32_t cond = -1;
                if (!condition(n.a, cond)) {
                    return Flow::Stop;
                }
                if (isConst(cond)) {
                    if (constValue(cond) != 0) {
                        return exec(n.b);
                    }
                    return n.c >= 0 ? exec(n.c) : Flow::Normal;
                }
                ++m_symDepth;
                const SState before = m_st;
                exec(n.b);
                const SState thenState = m_st;
                m_st = before;
                if (n.c >= 0) {
                    exec(n.c);
                }
                const SState elseState = m_st;
                --m_symDepth;
                if (m_failed || !merge(n, cond, thenState, elseState)) {
                    return Flow::Stop;
                }
                return Flow::Normal;
            }
            case NodeKind::For:
            case NodeKind::While:
                return execLoop(n, id);
            case NodeKind::Break:
            case NodeKind::Continue:
                if (m_symDepth > m_loopSymDepth) {
                    fail(n, "expr.limit",
                         "break / continue under a condition that is not known at compile time is not "
                         "supported "
                         "with run-time inputs");
                    return Flow::Stop;
                }
                return n.kind == NodeKind::Break ? Flow::Break : Flow::Continue;
            case NodeKind::Return:
                if (m_symDepth > m_callSymDepth) {
                    fail(n, "expr.limit",
                         "return under a condition that is not known at compile time is not supported with "
                         "run-time inputs");
                    return Flow::Stop;
                }
                m_ret = SVal();
                if (n.a >= 0) {
                    SVal r;
                    if (!eval(n.a, r)) {
                        return Flow::Stop;
                    }
                    m_ret = r;
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

    bool condition(int32_t id, int32_t& out) {
        SVal c;
        if (!eval(id, c)) {
            return false;
        }
        if (c.kind != VKind::Number) {
            return fail(
                node(id), "expr.eval",
                c.kind == VKind::Array ? "an array cannot be a condition" : "a condition has no value");
        }
        out = c.c[0];
        return true;
    }

    Flow execLoop(const Node& n, int32_t id) {
        const SState entry = m_st;
        const size_t entryNodes = m_res.nodes.size();
        const int32_t entryTime = m_timeId;
        const int saveLoopSym = m_loopSymDepth;
        m_loopSymDepth = m_symDepth;
        Flow result = Flow::Normal;
        if (n.kind == NodeKind::For && n.a >= 0) {
            result = exec(n.a);
        }
        const int32_t condNode = n.kind == NodeKind::For ? n.b : n.a;
        while (result == Flow::Normal) {
            if (condNode >= 0) {
                int32_t cond = -1;
                if (!condition(condNode, cond)) {
                    result = Flow::Stop;
                    break;
                }
                if (!isConst(cond)) {
                    m_loopSymDepth = saveLoopSym;
                    return subProgram(n, id, entry, entryNodes, entryTime);
                }
                if (constValue(cond) == 0) {
                    break;
                }
            }
            if (++m_iterations > kExprMaxIterations) {
                fail(n, "expr.limit", "the loops run more than 10000 iterations");
                result = Flow::Stop;
                break;
            }
            const Flow f = exec(n.d);
            if (f == Flow::Break) {
                break;
            }
            if (f == Flow::Return || f == Flow::Stop) {
                result = f;
                break;
            }
            if (n.kind == NodeKind::For && n.c >= 0) {
                SVal ignored;
                if (!eval(n.c, ignored)) {
                    result = Flow::Stop;
                }
            }
        }
        m_loopSymDepth = saveLoopSym;
        return result;
    }

    // ---- loops whose count depends on time: a sub-program of the compile-time evaluator ------

    /// Visits `id`'s subtree and the bodies of the local functions it calls.
    template <class F>
    void walk(int32_t id, F&& f) {
        std::vector<int32_t> stack{id};
        std::vector<char> seenFn(m_prog.functions.size(), 0);
        while (!stack.empty()) {
            const int32_t cur = stack.back();
            stack.pop_back();
            const Node& c = node(cur);
            f(c);
            if (c.kind == NodeKind::LocalCall && seenFn[static_cast<size_t>(c.slot)] == 0) {
                seenFn[static_cast<size_t>(c.slot)] = 1;
                stack.push_back(m_prog.functions[static_cast<size_t>(c.slot)].body);
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
    }

    Flow subProgram(const Node& n, int32_t id, const SState& entry, size_t entryNodes, int32_t entryTime) {
        static const char* kWhy =
            "a loop whose count is not known at compile time cannot be combined with run-time inputs";
        if (m_callDepth != 0 || m_symDepth != 0 || id != m_topStmt) {
            fail(n, "expr.limit", std::string(kWhy) + " (inside a function or a condition)");
            return Flow::Stop;
        }
        bool runtime = false;
        bool hasResult = false;
        std::vector<int32_t> assigned;
        std::vector<int32_t> reads;
        walk(id, [&](const Node& c) {
            if (c.kind == NodeKind::LayerLayout || c.kind == NodeKind::PortAt ||
                c.kind == NodeKind::LayerProp || c.kind == NodeKind::LayerPropAt ||
                (c.kind == NodeKind::Call && (static_cast<Builtin>(c.op) == Builtin::Port ||
                                              static_cast<Builtin>(c.op) == Builtin::RandomPerPlay))) {
                runtime = true;
            }
            if (c.kind == NodeKind::ExprStmt) {
                hasResult = true;
            }
            if (c.kind == NodeKind::Var && (c.flags & kFlagLocal) == 0) {
                reads.push_back(c.slot);
            }
            if (c.kind == NodeKind::VarDecl && (c.flags & kFlagLocal) == 0) {
                assigned.push_back(c.slot);
            }
            if (c.kind == NodeKind::Assign || c.kind == NodeKind::IncDec) {
                const Node* t = &node(c.a);
                if (t->kind == NodeKind::Element) {
                    t = &node(t->a);
                }
                if (t->kind == NodeKind::Var && (t->flags & kFlagLocal) == 0) {
                    assigned.push_back(t->slot);
                }
            }
        });
        if (runtime) {
            fail(n, "expr.limit", std::string(kWhy) + " (the loop reads one)");
            return Flow::Stop;
        }
        m_st = entry;
        m_res.nodes.resize(entryNodes);
        m_timeId = entryTime;
        for (int32_t s : reads) {
            const SVal& v = m_st.slots[static_cast<size_t>(s)];
            for (int i = 0; i < v.len; ++i) {
                if (m_res.nodes[static_cast<size_t>(v.c[i])].runtime) {
                    fail(n, "expr.limit", std::string(kWhy) + " (the loop reads a value derived from one)");
                    return Flow::Stop;
                }
            }
        }
        const int32_t stmtCount = m_topIndex + 1;
        auto shapeOf = [&](int32_t slotIndex, SVal& out) {
            Val v;
            if (m_ev.runPrefixValue(0.0, stmtCount, slotIndex, v) != ExprStatus::Ok) {
                return fail(n, "expr.eval",
                            m_evDiags.empty() ? std::string("the loop cannot be evaluated")
                                              : m_evDiags.front().message);
            }
            out = SVal();
            if (v.kind == VKind::Number || v.kind == VKind::Array) {
                out.kind = v.kind;
                out.len = v.len;
                for (int i = 0; i < v.len; ++i) {
                    SplitNode sp;
                    sp.op = SplitOp::SubProgram;
                    sp.ref = stmtCount;
                    sp.slot = slotIndex;
                    sp.comp = i;
                    out.c[i] = add(std::move(sp));
                }
            }
            return !m_failed;
        };
        std::sort(assigned.begin(), assigned.end());
        assigned.erase(std::unique(assigned.begin(), assigned.end()), assigned.end());
        for (int32_t s : assigned) {
            if (!shapeOf(s, m_st.slots[static_cast<size_t>(s)])) {
                return Flow::Stop;
            }
        }
        if (hasResult) {
            SVal last;
            if (!shapeOf(-1, last)) {
                return Flow::Stop;
            }
            if (last.kind != VKind::Undefined) {
                m_st.last = last;
            }
        }
        return Flow::Normal;
    }

    // ---- expressions -------------------------------------------------------------------------

    bool number(int32_t exprId, int32_t& out, const char* what) {
        SVal v;
        if (!eval(exprId, v)) {
            return false;
        }
        if (v.kind != VKind::Number) {
            return fail(node(exprId), "expr.eval",
                        std::string(what) +
                            (v.kind == VKind::Array ? " must be a number, not an array" : " has no value"));
        }
        out = v.c[0];
        return true;
    }

    bool constant(int32_t exprId, double& out, const char* what) {
        int32_t id = -1;
        if (!number(exprId, id, what)) {
            return false;
        }
        if (!isConst(id)) {
            return fail(
                node(exprId), "expr.limit",
                std::string(what) + " must be known at compile time in an expression with run-time inputs");
        }
        out = constValue(id);
        return true;
    }

    bool binary(const Node& at, BinaryOp op, const SVal& a, const SVal& b, SVal& out) {
        if (a.kind == VKind::Undefined || b.kind == VKind::Undefined || a.kind == VKind::Poison ||
            b.kind == VKind::Poison) {
            return fail(at, "expr.eval", "a value is used before it is set");
        }
        static constexpr SplitOp kOps[] = {SplitOp::Add, SplitOp::Sub, SplitOp::Mul, SplitOp::Div,
                                           SplitOp::Mod, SplitOp::Lt,  SplitOp::Le,  SplitOp::Gt,
                                           SplitOp::Ge,  SplitOp::Eq,  SplitOp::Ne};
        const SplitOp sop = kOps[static_cast<int>(op)];
        const bool aArr = a.kind == VKind::Array;
        const bool bArr = b.kind == VKind::Array;
        out = SVal();
        if (!aArr && !bArr) {
            out = scalar(mk(sop, {a.c[0], b.c[0]}));
            return !m_failed;
        }
        out.kind = VKind::Array;
        if ((op == BinaryOp::Add || op == BinaryOp::Sub) && aArr && bArr) {
            out.len = std::max(a.len, b.len);
            for (int i = 0; i < out.len; ++i) {
                const int32_t x = i < a.len ? a.c[i] : konst(0.0);
                const int32_t y = i < b.len ? b.c[i] : konst(0.0);
                out.c[i] = mk(sop, {x, y});
            }
            return !m_failed;
        }
        if ((op == BinaryOp::Mul && aArr != bArr) || (op == BinaryOp::Div && aArr && !bArr)) {
            const SVal& arr = aArr ? a : b;
            const int32_t k = aArr ? b.c[0] : a.c[0];
            out.len = arr.len;
            for (int i = 0; i < out.len; ++i) {
                out.c[i] = aArr ? mk(sop, {arr.c[i], k}) : mk(sop, {k, arr.c[i]});
            }
            return !m_failed;
        }
        return fail(at, "expr.eval",
                    "arrays can be added to / subtracted from arrays, and multiplied or divided by a number");
    }

    /// Evaluates `exprId` as one side of a condition that is not a compile-time constant: its side
    /// effects merge with "not evaluated" under `cond` (taken when `whenTrue` matches).
    bool branchEval(const Node& at, int32_t cond, bool whenTrue, int32_t exprId, SVal& out) {
        ++m_symDepth;
        const SState before = m_st;
        const bool ok = eval(exprId, out);
        const SState after = m_st;
        --m_symDepth;
        if (!ok) {
            return false;
        }
        return whenTrue ? merge(at, cond, after, before) : merge(at, cond, before, after);
    }

    bool eval(int32_t id, SVal& out);
    bool evalCall(const Node& n, SVal& out);
    bool evalMap(const Node& n, Builtin b, SVal& out);
    bool evalLocalCall(const Node& n, SVal& out);
    bool layerKey(const Node& keyNode, ExprLayerKey& key);
    bool assign(const Node& target, const SVal& v);

    void extractCurves();

    const Program& m_prog;
    const ExprContext& m_ctx;
    SplitResult& m_res;
    SplitEnv m_env;
    std::vector<Diagnostic> m_evDiags;
    Evaluator m_ev;
    SState m_st;
    SVal m_ret;
    size_t m_frameBase = 0;
    int m_callDepth = 0;
    int m_symDepth = 0;
    int m_loopSymDepth = 0;
    int m_callSymDepth = 0;
    int m_iterations = 0;
    int32_t m_topIndex = 0;
    int32_t m_topStmt = -1;
    int32_t m_timeId = -1;
    bool m_failed = false;
};

bool Splitter::assign(const Node& target, const SVal& v) {
    if (target.kind == NodeKind::Var) {
        slot(target) = v;
        return true;
    }
    const Node& arr = node(target.a);
    if (arr.kind != NodeKind::Var) {
        return fail(target, "expr.unsupported", "only an element of an array variable can be assigned");
    }
    double idx = 0;
    if (!constant(target.b, idx, "an index")) {
        return false;
    }
    SVal& s = slot(arr);
    if (s.kind != VKind::Array) {
        return fail(target, "expr.eval", "only an array has elements");
    }
    if (v.kind != VKind::Number) {
        return fail(target, "expr.eval", "an array element must be a number");
    }
    if (idx != std::floor(idx) || idx < 0 || idx >= s.len) {
        return fail(target, "expr.eval", "the index is outside the array");
    }
    s.c[static_cast<int>(idx)] = v.c[0];
    return true;
}

bool Splitter::layerKey(const Node& keyNode, ExprLayerKey& key) {
    if (keyNode.kind == NodeKind::String) {
        key.byIndex = false;
        key.name = m_prog.strings[static_cast<size_t>(keyNode.slot)];
        return true;
    }
    double idx = 0;
    if (!constant(static_cast<int32_t>(&keyNode - m_prog.nodes.data()), idx, "a layer index")) {
        return false;
    }
    key.byIndex = true;
    key.index = static_cast<int>(std::clamp(std::floor(idx), -1e9, 1e9));
    return true;
}

bool Splitter::eval(int32_t id, SVal& out) {
    if (m_failed) {
        return false;
    }
    const Node& n = node(id);
    out = SVal();
    switch (n.kind) {
        case NodeKind::Number:
            out = scalar(konst(n.number));
            return true;
        case NodeKind::String:
            return fail(n, "expr.eval", "a string is not a number");
        case NodeKind::Var:
            out = slot(n);
            if (out.kind == VKind::Poison) {
                return fail(
                    n, "expr.limit",
                    "a variable is set in only one branch of a condition that is not known at compile time");
            }
            if (out.kind == VKind::Undefined) {
                return fail(n, "expr.eval", "a variable is used before it is set");
            }
            return true;
        case NodeKind::Time:
            out = scalar(timeNode());
            return !m_failed;
        case NodeKind::Value:
            out = scalar(mk(SplitOp::ValueAt, {timeNode()}));
            return !m_failed;
        case NodeKind::ValueVec: {
            if (m_env.vec.size() < 2) {
                return fail(n, "expr.eval", "valueVec needs a property with X and Y rows");
            }
            out.kind = VKind::Array;
            out.len = static_cast<uint8_t>(std::min<size_t>(m_env.vec.size(), 4));
            for (int i = 0; i < out.len; ++i) {
                SplitNode v;
                v.op = SplitOp::VecAt;
                v.comp = i;
                v.args[v.argc++] = timeNode();
                out.c[i] = add(std::move(v));
            }
            return !m_failed;
        }
        case NodeKind::Index:
            out = scalar(konst(static_cast<double>(m_ctx.index)));
            return true;
        case NodeKind::Duration:
            out = scalar(konst(m_ctx.duration));
            return true;
        case NodeKind::Unary: {
            SVal a;
            if (!eval(n.a, a)) {
                return false;
            }
            const UnaryOp op = static_cast<UnaryOp>(n.op);
            if (op == UnaryOp::Not) {
                if (a.kind != VKind::Number) {
                    return fail(n, "expr.eval", "an array cannot be a condition");
                }
                out = scalar(mk(SplitOp::Not, {a.c[0]}));
                return !m_failed;
            }
            if (a.kind != VKind::Number && a.kind != VKind::Array) {
                return fail(n, "expr.eval", "a value is used before it is set");
            }
            out = a;
            if (op == UnaryOp::Neg) {
                for (int i = 0; i < a.len; ++i) {
                    out.c[i] = mk(SplitOp::Neg, {a.c[i]});
                }
            }
            return !m_failed;
        }
        case NodeKind::Binary: {
            SVal a;
            SVal b;
            if (!eval(n.a, a) || !eval(n.b, b)) {
                return false;
            }
            return binary(n, static_cast<BinaryOp>(n.op), a, b, out);
        }
        case NodeKind::Logical: {
            int32_t a = -1;
            if (!condition(n.a, a)) {
                return false;
            }
            const bool isOr = static_cast<LogicalOp>(n.op) == LogicalOp::Or;
            if (isConst(a)) {
                if ((constValue(a) != 0) == isOr) {
                    out = scalar(a);
                    return true;
                }
                return eval(n.b, out);
            }
            SVal b;
            // `a || b` evaluates b when a is false; `a && b` when a is true.
            if (!branchEval(n, a, !isOr, n.b, b)) {
                return false;
            }
            if (b.kind != VKind::Number) {
                return fail(n, "expr.limit", "&& / || with an array operand under a run-time condition");
            }
            out = scalar(isOr ? select(a, a, b.c[0]) : select(a, b.c[0], a));
            return !m_failed;
        }
        case NodeKind::Conditional: {
            int32_t c = -1;
            if (!condition(n.a, c)) {
                return false;
            }
            if (isConst(c)) {
                return eval(constValue(c) != 0 ? n.b : n.c, out);
            }
            ++m_symDepth;
            const SState before = m_st;
            SVal yes;
            SVal no;
            const bool okYes = eval(n.b, yes);
            const SState yesState = m_st;
            m_st = before;
            const bool okNo = okYes && eval(n.c, no);
            const SState noState = m_st;
            --m_symDepth;
            if (!okYes || !okNo || !merge(n, c, yesState, noState)) {
                return false;
            }
            if (yes.kind != no.kind || yes.len != no.len ||
                (yes.kind != VKind::Number && yes.kind != VKind::Array)) {
                return fail(
                    n, "expr.limit",
                    "the two sides of a condition not known at compile time give different kinds of value");
            }
            out = yes;
            for (int i = 0; i < yes.len; ++i) {
                out.c[i] = select(c, yes.c[i], no.c[i]);
            }
            return !m_failed;
        }
        case NodeKind::Call:
            return evalCall(n, out);
        case NodeKind::LocalCall:
            return evalLocalCall(n, out);
        case NodeKind::Array: {
            out.kind = VKind::Array;
            out.len = static_cast<uint8_t>(n.count);
            for (int32_t i = 0; i < n.count; ++i) {
                int32_t e = -1;
                if (!number(m_prog.lists[static_cast<size_t>(n.first + i)], e, "an array element")) {
                    return false;
                }
                out.c[i] = e;
            }
            return true;
        }
        case NodeKind::Element: {
            SVal a;
            if (!eval(n.a, a)) {
                return false;
            }
            double idx = 0;
            if (!constant(n.b, idx, "an index")) {
                return false;
            }
            if (a.kind != VKind::Array) {
                return fail(n, "expr.eval", "only an array has elements");
            }
            if (idx != std::floor(idx) || idx < 0 || idx >= a.len) {
                return fail(n, "expr.eval", "the index is outside the array");
            }
            out = scalar(a.c[static_cast<int>(idx)]);
            return true;
        }
        case NodeKind::ArrayLength: {
            SVal a;
            if (!eval(n.a, a)) {
                return false;
            }
            if (a.kind != VKind::Array) {
                return fail(n, "expr.eval", ".length needs an array");
            }
            out = scalar(konst(a.len));
            return true;
        }
        case NodeKind::Assign: {
            const Node& target = node(n.a);
            SVal rhs;
            if (!eval(n.b, rhs)) {
                return false;
            }
            const AssignOp op = static_cast<AssignOp>(n.op);
            if (op != AssignOp::Set) {
                SVal old;
                if (!eval(n.a, old)) {
                    return false;
                }
                static constexpr BinaryOp kMap[] = {BinaryOp::Add, BinaryOp::Add, BinaryOp::Sub,
                                                    BinaryOp::Mul, BinaryOp::Div, BinaryOp::Mod};
                SVal r;
                if (!binary(n, kMap[static_cast<int>(op)], old, rhs, r)) {
                    return false;
                }
                rhs = r;
            }
            if (!assign(target, rhs)) {
                return false;
            }
            out = rhs;
            return true;
        }
        case NodeKind::IncDec: {
            const Node& target = node(n.a);
            SVal old;
            if (!eval(n.a, old)) {
                return false;
            }
            if (old.kind != VKind::Number) {
                return fail(n, "expr.eval", "++ / -- need a number");
            }
            const IncDecOp op = static_cast<IncDecOp>(n.op);
            const bool inc = op == IncDecOp::PreInc || op == IncDecOp::PostInc;
            const SVal updated = scalar(mk(inc ? SplitOp::Add : SplitOp::Sub, {old.c[0], konst(1.0)}));
            if (!assign(target, updated)) {
                return false;
            }
            out = (op == IncDecOp::PreInc || op == IncDecOp::PreDec) ? updated : old;
            return !m_failed;
        }
        case NodeKind::LayerProp:
        case NodeKind::LayerPropAt: {
            ExprLayerKey key;
            if (!layerKey(node(n.a), key)) {
                return false;
            }
            int32_t t = -1;
            if (n.kind == NodeKind::LayerPropAt) {
                if (!number(n.b, t, "valueAtTime's time")) {
                    return false;
                }
            } else {
                t = timeNode();
            }
            const std::string& prop = m_prog.strings[static_cast<size_t>(n.slot)];
            const ExprRowRef ref = m_ctx.otherRows ? m_ctx.otherRows(key, prop) : ExprRowRef{};
            if (ref.kind == ExprRefKind::NotFound) {
                return fail(
                    n, "expr.ref",
                    m_ctx.otherRows
                        ? "layer(...)." + prop + " does not name a row of this timeline"
                        : std::string("references to other rows (layer(...)) are not supported here"));
            }
            SplitNode s;
            s.op = ref.kind == ExprRefKind::Row ? SplitOp::OtherRowAt : SplitOp::LayerProp;
            s.key = key;
            s.name = prop;
            s.args[s.argc++] = t;
            out = scalar(add(std::move(s)));
            return !m_failed;
        }
        case NodeKind::LayerLayout: {
            SplitNode s;
            if (!layerKey(node(n.a), s.key)) {
                return false;
            }
            s.op = SplitOp::Layout;
            s.comp = n.op;
            out = scalar(add(std::move(s)));
            return !m_failed;
        }
        case NodeKind::PortAt: {
            int32_t t = -1;
            if (!number(n.b, t, "valueAtTime's time")) {
                return false;
            }
            SplitNode s;
            s.op = SplitOp::PortAt;
            s.name = m_prog.strings[static_cast<size_t>(node(n.a).slot)];
            s.args[s.argc++] = t;
            out = scalar(add(std::move(s)));
            return !m_failed;
        }
        default:
            return fail(n, "expr.syntax", "not an expression");
    }
}

bool Splitter::evalLocalCall(const Node& n, SVal& out) {
    const Function& f = m_prog.functions[static_cast<size_t>(n.slot)];
    std::vector<SVal> args(static_cast<size_t>(n.count));
    for (int32_t i = 0; i < n.count; ++i) {
        if (!eval(m_prog.lists[static_cast<size_t>(n.first + i)], args[static_cast<size_t>(i)])) {
            return false;
        }
    }
    const size_t saveBase = m_frameBase;
    const int saveCallSym = m_callSymDepth;
    const int saveLoopSym = m_loopSymDepth;
    const size_t base = m_st.slots.size();
    m_st.slots.resize(base + static_cast<size_t>(f.localCount));
    for (size_t i = 0; i < args.size(); ++i) {
        m_st.slots[base + i] = args[i];
    }
    m_frameBase = base;
    m_callSymDepth = m_symDepth;
    m_loopSymDepth = m_symDepth;
    ++m_callDepth;
    m_ret = SVal();
    const Flow flow = exec(f.body);
    --m_callDepth;
    m_frameBase = saveBase;
    m_callSymDepth = saveCallSym;
    m_loopSymDepth = saveLoopSym;
    m_st.slots.resize(base);
    if (flow == Flow::Stop || m_failed) {
        return false;
    }
    out = flow == Flow::Return ? m_ret : SVal();
    return true;
}

bool Splitter::evalMap(const Node& n, Builtin b, SVal& out) {
    const int32_t* args = m_prog.list(n);
    SVal vals[5];
    for (int32_t i = 0; i < n.count; ++i) {
        if (!eval(args[i], vals[i])) {
            return false;
        }
        if (vals[i].kind != VKind::Number && vals[i].kind != VKind::Array) {
            return fail(n, "expr.eval",
                        std::string(builtinName(b)) + "() needs values for all its arguments");
        }
    }
    int firstVec = 0;
    int len = 1;
    bool isArray = false;
    const std::string shapeError = mapOperandShape(b, vals, n.count, firstVec, len, isArray);
    if (!shapeError.empty()) {
        return fail(n, "expr.eval", shapeError);
    }
    auto comp = [&](int arg, int i) {
        return vals[arg].kind == VKind::Array ? vals[arg].c[i] : vals[arg].c[0];
    };
    out.kind = isArray ? VKind::Array : VKind::Number;
    out.len = static_cast<uint8_t>(len);
    for (int i = 0; i < len; ++i) {
        SplitNode s;
        if (b == Builtin::Clamp) {
            s.op = SplitOp::Clamp;
            s.argc = 3;
            s.args[0] = comp(0, i);
            s.args[1] = comp(1, i);
            s.args[2] = comp(2, i);
        } else {
            s.op = b == Builtin::Linear ? SplitOp::Linear : SplitOp::Ease;
            s.builtin = b;
            s.argc = 5;
            s.args[0] = vals[0].c[0];
            s.args[1] = n.count == 5 ? vals[1].c[0] : konst(0.0);
            s.args[2] = n.count == 5 ? vals[2].c[0] : konst(1.0);
            s.args[3] = comp(n.count - 2, i);
            s.args[4] = comp(n.count - 1, i);
        }
        out.c[i] = add(std::move(s));
    }
    return !m_failed;
}

bool Splitter::evalCall(const Node& n, SVal& out) {
    const Builtin b = static_cast<Builtin>(n.op);
    const int32_t* args = m_prog.list(n);
    switch (b) {
        case Builtin::Port: {
            SplitNode s;
            s.op = SplitOp::Port;
            s.name = m_prog.strings[static_cast<size_t>(node(args[0]).slot)];
            out = scalar(add(std::move(s)));
            return !m_failed;
        }
        case Builtin::RandomPerPlay: {
            SplitNode s;
            s.op = SplitOp::PlayRandom;
            s.site = n.site;
            s.argc = 2;
            if (n.count == 2) {
                if (!number(args[0], s.args[0], "randomPerPlay's minimum") ||
                    !number(args[1], s.args[1], "randomPerPlay's maximum")) {
                    return false;
                }
            } else {
                s.args[0] = konst(0.0);
                s.args[1] = konst(1.0);
            }
            out = scalar(add(std::move(s)));
            return !m_failed;
        }
        case Builtin::Wiggle: {
            SplitNode s;
            s.op = SplitOp::Wiggle;
            s.site = n.site;
            s.argc = 5;
            if (!number(args[0], s.args[0], "wiggle's frequency") ||
                !number(args[1], s.args[1], "wiggle's amplitude")) {
                return false;
            }
            s.args[2] = konst(1.0);
            s.args[3] = konst(0.5);
            if (n.count > 2 && !number(args[2], s.args[2], "wiggle's octaves")) {
                return false;
            }
            if (n.count > 3 && !number(args[3], s.args[3], "wiggle's amplitude multiplier")) {
                return false;
            }
            if (n.count > 4) {
                if (!number(args[4], s.args[4], "wiggle's time")) {
                    return false;
                }
            } else {
                s.args[4] = timeNode();
            }
            out = scalar(add(std::move(s)));
            return !m_failed;
        }
        case Builtin::LoopOut:
        case Builtin::LoopIn: {
            SplitNode s;
            s.op = b == Builtin::LoopOut ? SplitOp::LoopOut : SplitOp::LoopIn;
            LoopType type = LoopType::Cycle;
            if (n.count > 0) {
                parseLoopType(m_prog.strings[static_cast<size_t>(node(args[0]).slot)], type);
            }
            s.loopType = static_cast<uint8_t>(type);
            double keys = 0;
            if (n.count > 1 && !constant(args[1], keys, "the number of keyframes")) {
                return false;
            }
            s.value = std::floor(keys);
            s.args[s.argc++] = timeNode();
            out = scalar(add(std::move(s)));
            return !m_failed;
        }
        case Builtin::ValueAtTime: {
            int32_t t = -1;
            if (!number(args[0], t, "valueAtTime's time")) {
                return false;
            }
            out = scalar(mk(SplitOp::ValueAt, {t}));
            return !m_failed;
        }
        case Builtin::Linear:
        case Builtin::Ease:
        case Builtin::EaseIn:
        case Builtin::EaseOut:
        case Builtin::Clamp:
            return evalMap(n, b, out);
        case Builtin::Random: {
            SVal lo = scalar(konst(0.0));
            SVal hi = scalar(konst(1.0));
            if (n.count == 1 && !eval(args[0], hi)) {
                return false;
            }
            if (n.count == 2 && (!eval(args[0], lo) || !eval(args[1], hi))) {
                return false;
            }
            for (const SVal* v : {&lo, &hi}) {
                if (v->kind != VKind::Number && v->kind != VKind::Array) {
                    return fail(n, "expr.eval", "random() needs values for its arguments");
                }
            }
            if (lo.kind == VKind::Array && hi.kind == VKind::Array && lo.len != hi.len) {
                return fail(n, "expr.eval", "random() needs arrays of the same length");
            }
            const bool isArray = lo.kind == VKind::Array || hi.kind == VKind::Array;
            out.kind = isArray ? VKind::Array : VKind::Number;
            out.len = isArray ? (lo.kind == VKind::Array ? lo.len : hi.len) : uint8_t{1};
            for (int i = 0; i < out.len; ++i) {
                SplitNode s;
                s.op = SplitOp::Random;
                s.argc = 3;
                s.args[0] = lo.kind == VKind::Array ? lo.c[i] : lo.c[0];
                s.args[1] = hi.kind == VKind::Array ? hi.c[i] : hi.c[0];
                s.args[2] = m_st.timeless ? konst(0.0) : timeNode();
                s.seed = m_st.randSeed;
                s.site = m_st.randCalls++;
                s.timeless = m_st.timeless;
                s.seeded = m_st.seeded;  // T-777: RandomSeeded at run time.
                s.seedNumber = m_st.seedNumber;
                out.c[i] = add(std::move(s));
            }
            return !m_failed;
        }
        case Builtin::SeedRandom: {
            double seed = 0;
            if (!constant(args[0], seed, "seedRandom's seed")) {
                return false;
            }
            double timeless = 0;
            if (n.count > 1 && !constant(args[1], timeless, "seedRandom's timeless flag")) {
                return false;
            }
            m_st.randSeed = exprMixSeed(m_ctx.seed, exprSeedFromNumber(static_cast<float>(seed)));
            m_st.timeless = timeless != 0;
            m_st.randCalls = 0;
            m_st.seeded = true;  // T-777
            m_st.seedNumber = seed;
            return true;
        }
        case Builtin::Noise: {
            SplitNode s;
            s.op = SplitOp::Noise;
            s.site = n.site;
            s.argc = 1;
            if (!number(args[0], s.args[0], "noise's argument")) {
                return false;
            }
            out = scalar(add(std::move(s)));
            return !m_failed;
        }
        case Builtin::PosterizeTime: {
            int32_t fps = -1;
            if (!number(args[0], fps, "posterizeTime's rate")) {
                return false;
            }
            if (isConst(fps) && constValue(fps) < 0) {
                return fail(n, "expr.eval", "posterizeTime's rate cannot be negative");
            }
            m_st.postFps = scalar(fps);
            return true;
        }
        case Builtin::Length: {
            SVal a;
            if (!eval(args[0], a)) {
                return false;
            }
            SVal c = a;
            if (n.count > 1) {
                SVal bb;
                if (!eval(args[1], bb) || !binary(n, BinaryOp::Sub, a, bb, c)) {
                    return false;
                }
            }
            if (c.kind != VKind::Number && c.kind != VKind::Array) {
                return fail(n, "expr.eval", "length() needs a value");
            }
            int32_t sum = konst(0.0);
            for (int i = 0; i < c.len; ++i) {
                sum = mk(SplitOp::Add, {sum, mk(SplitOp::Mul, {c.c[i], c.c[i]})});
            }
            SplitNode s;
            s.op = SplitOp::Math1;
            s.builtin = Builtin::MathSqrt;
            s.args[s.argc++] = sum;
            out = scalar(add(std::move(s)));
            return !m_failed;
        }
        case Builtin::MathMin:
        case Builtin::MathMax: {
            int32_t r = -1;
            for (int32_t i = 0; i < n.count; ++i) {
                int32_t x = -1;
                if (!number(args[i], x, builtinName(b))) {
                    return false;
                }
                r = i == 0 ? x : mk(b == Builtin::MathMin ? SplitOp::Min : SplitOp::Max, {r, x});
            }
            out = scalar(r);
            return !m_failed;
        }
        default: {
            SplitNode s;
            s.builtin = b;
            if (!number(args[0], s.args[0], builtinName(b))) {
                return false;
            }
            s.argc = 1;
            s.op = SplitOp::Math1;
            if (b == Builtin::MathAtan2 || b == Builtin::MathPow) {
                if (!number(args[1], s.args[1], builtinName(b))) {
                    return false;
                }
                s.argc = 2;
                s.op = SplitOp::Math2;
            }
            out = scalar(add(std::move(s)));
            return !m_failed;
        }
    }
}

// The remainder of each component: run-time nodes are copied with their time-only arguments replaced by
// CurveRef leaves (one curve per distinct maximal time-only node), constants are kept.
void Splitter::extractCurves() {
    std::vector<char> reach(m_res.nodes.size(), 0);
    std::vector<int32_t> stack(m_res.expanded.begin(), m_res.expanded.end());
    while (!stack.empty()) {
        const int32_t id = stack.back();
        stack.pop_back();
        if (reach[static_cast<size_t>(id)] != 0) {
            continue;
        }
        reach[static_cast<size_t>(id)] = 1;
        const SplitNode& n = m_res.nodes[static_cast<size_t>(id)];
        if (!n.runtime) {
            continue;  // a curve or a constant: its inside is not part of the remainder
        }
        for (int i = 0; i < n.argc; ++i) {
            stack.push_back(n.args[i]);
        }
    }
    const size_t count = m_res.nodes.size();
    std::vector<int32_t> mapped(count, -1);
    std::vector<int32_t> curveOf(count, -1);
    auto mapNode = [&](int32_t id) -> int32_t {
        const SplitNode& n = m_res.nodes[static_cast<size_t>(id)];
        if (n.runtime) {
            return mapped[static_cast<size_t>(id)];
        }
        if (!n.timeDep) {
            return id;  // a constant
        }
        if (curveOf[static_cast<size_t>(id)] < 0) {
            curveOf[static_cast<size_t>(id)] = static_cast<int32_t>(m_res.curves.size());
            m_res.curves.push_back(SplitCurve{id});
        }
        SplitNode ref;
        ref.op = SplitOp::CurveRef;
        ref.ref = curveOf[static_cast<size_t>(id)];
        ref.timeDep = true;
        m_res.nodes.push_back(ref);
        return static_cast<int32_t>(m_res.nodes.size() - 1);
    };
    for (size_t id = 0; id < count; ++id) {
        if (reach[id] == 0 || !m_res.nodes[id].runtime) {
            continue;
        }
        SplitNode copy = m_res.nodes[id];
        for (int i = 0; i < copy.argc; ++i) {
            copy.args[i] = mapNode(copy.args[i]);
        }
        m_res.nodes.push_back(std::move(copy));
        mapped[id] = static_cast<int32_t>(m_res.nodes.size() - 1);
    }
    for (int32_t root : m_res.expanded) {
        m_res.components.push_back(mapNode(root));
    }
    // Tree sizes (a run-time program evaluates a shared node once per use).
    std::vector<size_t> size(m_res.nodes.size(), 1);
    for (size_t id = count; id < m_res.nodes.size(); ++id) {
        const SplitNode& n = m_res.nodes[id];
        size_t s = 1;
        for (int i = 0; i < n.argc; ++i) {
            const size_t arg = static_cast<size_t>(n.args[i]);
            s += arg >= count ? size[arg] : 1;
            s = std::min<size_t>(s, 1u << 30);
        }
        size[id] = s;
    }
    for (int32_t root : m_res.components) {
        const size_t ops = static_cast<size_t>(root) >= count ? size[static_cast<size_t>(root)] : 1;
        m_res.remainderOps.push_back(ops);
        if (ops > kSplitMaxOps) {
            fail(node(m_prog.root), "expr.limit",
                 "after expansion the expression needs " + std::to_string(ops) +
                     " operations at run time (the limit is 256)");
            return;
        }
    }
    m_res.status = ExprStatus::Ok;
}

}  // namespace

SplitResult splitRuntime(const Program& program, const ExprContext& ctx) {
    SplitResult result;
    if (program.root < 0) {
        addExprDiagnostic(result.diagnostics, "expr.syntax", "the expression did not parse", 1, 1);
        return result;
    }
    RowSampler row;
    if (!row.init(ctx.row, result.diagnostics)) {
        return result;
    }
    std::vector<RowSampler> vec(ctx.vecRows.size());
    for (size_t i = 0; i < vec.size(); ++i) {
        if (!vec[i].init(ctx.vecRows[i], result.diagnostics)) {
            return result;
        }
    }
    Splitter splitter(program, ctx, result, row, vec);
    splitter.run();
    if (result.status != ExprStatus::Ok) {
        result.status = ExprStatus::Error;
    }
    return result;
}

}  // namespace xt::compile::expr
