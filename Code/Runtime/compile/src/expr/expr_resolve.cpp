#include <string>
#include <unordered_map>
#include <vector>

#include "expr.h"
#include "expr_parser.h"

/// Name resolution of a parsed timeline expression, and parseExpression().
/// Docs/design/58-value-functions-and-drivers.md §58.2: `var` / `let` / `const` and plain `x = ...`
/// at the top level make globals; inside a local function they make locals (globals stay readable
/// and assignable there); functions are declared at the top level and may not call themselves,
/// directly or through others.

namespace xt::compile::expr {

namespace {

using NameMap = std::unordered_map<std::string, int32_t>;

class Resolver {
public:
    Resolver(Program& prog, std::vector<Diagnostic>& diags) : m_prog(prog), m_diags(diags) {}

    bool run() {
        // Functions first: they may be called before their declaration.
        for (size_t i = 0; i < m_prog.functions.size(); ++i) {
            const std::string& name = m_prog.functions[i].name;
            if (m_functions.count(name) != 0) {
                fail(m_prog.functions[i].line, 1, "expr.syntax", "function '" + name + "' is declared twice");
                return false;
            }
            m_functions[name] = static_cast<int32_t>(i);
        }
        // Globals: every top-level declaration and plain assignment, wherever it sits.
        collect(m_prog.root, m_globals, nullptr);
        if (m_failed) {
            return false;
        }
        m_prog.globalNames.assign(m_globals.size(), std::string());
        for (const auto& [name, slot] : m_globals) {
            m_prog.globalNames[static_cast<size_t>(slot)] = name;
        }
        checkStrings(m_prog.root, -1);
        resolve(m_prog.root, nullptr, -1);
        for (size_t i = 0; i < m_prog.functions.size() && !m_failed; ++i) {
            resolveFunction(static_cast<int32_t>(i));
        }
        if (!m_failed) {
            checkRecursion();
        }
        return !m_failed;
    }

private:
    Node& node(int32_t id) { return m_prog.nodes[static_cast<size_t>(id)]; }

    void fail(int line, int col, const char* code, std::string msg) {
        if (!m_failed) {
            addExprDiagnostic(m_diags, code, std::move(msg), line, col);
        }
        m_failed = true;
    }

    template <class F>
    void forEachChild(int32_t id, F&& f) {
        const Node n = node(id);
        if (n.kind == NodeKind::Function) {
            return;  // its list holds parameter names, its body is in Program::functions
        }
        for (int32_t c : {n.a, n.b, n.c, n.d}) {
            if (c >= 0) {
                f(c);
            }
        }
        for (int32_t i = 0; i < n.count; ++i) {
            f(m_prog.lists[static_cast<size_t>(n.first + i)]);
        }
    }

    /// Declares the names `id`'s subtree declares into `scope` (VarDecl and plain assignment to a
    /// name); inside a function (`outer` = the globals) a plain assignment to a global stays global.
    void collect(int32_t id, NameMap& scope, const NameMap* outer) {
        const Node& n = node(id);
        if (n.kind == NodeKind::VarDecl) {
            declare(scope, m_prog.strings[static_cast<size_t>(n.slot)]);
        } else if (n.kind == NodeKind::Assign && static_cast<AssignOp>(n.op) == AssignOp::Set &&
                   node(n.a).kind == NodeKind::Var) {
            const std::string& name = m_prog.strings[static_cast<size_t>(node(n.a).slot)];
            if (isReservedName(name)) {
                fail(n.line, n.col, "expr.syntax",
                     "'" + name + "' is a built-in name and cannot be assigned");
                return;
            }
            if (outer == nullptr || outer->count(name) == 0) {
                declare(scope, name);
            }
        }
        forEachChild(id, [&](int32_t c) { collect(c, scope, outer); });
    }

    static void declare(NameMap& scope, const std::string& name) {
        if (scope.count(name) == 0) {
            const int32_t slot = static_cast<int32_t>(scope.size());
            scope[name] = slot;
        }
    }

    /// Strings are only arguments of loopOut / loopIn / port, or the key of layer(...).
    void checkStrings(int32_t id, int32_t parent) {
        const Node& n = node(id);
        if (n.kind == NodeKind::String) {
            bool ok = false;
            if (parent >= 0) {
                const Node& p = node(parent);
                if (p.kind == NodeKind::Call) {
                    const Builtin b = static_cast<Builtin>(p.op);
                    ok = b == Builtin::LoopOut || b == Builtin::LoopIn || b == Builtin::Port;
                } else if (p.kind == NodeKind::LayerProp || p.kind == NodeKind::LayerPropAt ||
                           p.kind == NodeKind::LayerLayout || p.kind == NodeKind::PortAt) {
                    ok = p.a == id;
                }
            }
            if (!ok) {
                fail(n.line, n.col, "expr.unsupported",
                     "strings can only be arguments of loopOut(), loopIn(), layer() and port()");
            }
            return;
        }
        forEachChild(id, [&](int32_t c) { checkStrings(c, id); });
    }

    /// Resolves the names under `id`. `locals` is null at the top level; `fn` is the function index.
    void resolve(int32_t id, const NameMap* locals, int32_t fn) {
        if (m_failed) {
            return;
        }
        Node& n = node(id);
        if (n.kind == NodeKind::Var || n.kind == NodeKind::VarDecl) {
            const std::string name = m_prog.strings[static_cast<size_t>(n.slot)];
            if (locals != nullptr) {
                auto it = locals->find(name);
                if (it != locals->end()) {
                    n.slot = it->second;
                    n.flags |= kFlagLocal;
                    resolveChildren(id, locals, fn);
                    return;
                }
            }
            auto g = m_globals.find(name);
            if (g == m_globals.end()) {
                fail(n.line, n.col, "expr.name", "'" + name + "' is not defined");
                return;
            }
            n.slot = g->second;
        } else if (n.kind == NodeKind::LocalCall) {
            const std::string name = m_prog.strings[static_cast<size_t>(n.slot)];
            auto it = m_functions.find(name);
            if (it == m_functions.end()) {
                fail(n.line, n.col, "expr.name", "'" + name + "' is not a function");
                return;
            }
            n.slot = it->second;
            const Function& callee = m_prog.functions[static_cast<size_t>(it->second)];
            if (n.count > callee.paramCount) {
                fail(n.line, n.col, "expr.syntax",
                     "'" + name + "' takes " + std::to_string(callee.paramCount) + " argument(s)");
                return;
            }
            if (fn >= 0) {
                m_calls.push_back({fn, it->second, n.line, n.col});
            }
        }
        resolveChildren(id, locals, fn);
    }

    void resolveChildren(int32_t id, const NameMap* locals, int32_t fn) {
        forEachChild(id, [&](int32_t c) { resolve(c, locals, fn); });
    }

    void resolveFunction(int32_t fi) {
        Function& f = m_prog.functions[static_cast<size_t>(fi)];
        NameMap locals;
        // The parameters are the first locals; their names sit in the Function node's list.
        for (const Node& n : m_prog.nodes) {
            if (n.kind == NodeKind::Function && n.slot == fi) {
                for (int32_t i = 0; i < n.count; ++i) {
                    declare(
                        locals,
                        m_prog.strings[static_cast<size_t>(m_prog.lists[static_cast<size_t>(n.first + i)])]);
                }
                break;
            }
        }
        collect(f.body, locals, &m_globals);
        f.localCount = static_cast<int32_t>(locals.size());
        resolve(f.body, &locals, fi);
    }

    void checkRecursion() {
        const size_t n = m_prog.functions.size();
        std::vector<std::vector<size_t>> callees(n);
        for (const Call& c : m_calls) {
            callees[static_cast<size_t>(c.caller)].push_back(static_cast<size_t>(c.callIndex));
        }
        // Iterative DFS (no recursion on user-controlled depth): 0 white, 1 grey, 2 black.
        std::vector<int> color(n, 0);
        for (size_t start = 0; start < n && !m_failed; ++start) {
            if (color[start] != 0) {
                continue;
            }
            std::vector<std::pair<size_t, size_t>> stack{{start, 0}};
            color[start] = 1;
            while (!stack.empty() && !m_failed) {
                auto& [fnIdx, next] = stack.back();
                if (next < callees[fnIdx].size()) {
                    const size_t callee = callees[fnIdx][next++];
                    if (color[callee] == 1) {
                        const Function& f = m_prog.functions[callee];
                        fail(f.line, 1, "expr.recursion",
                             "function '" + f.name + "' calls itself (recursion is not supported)");
                    } else if (color[callee] == 0) {
                        color[callee] = 1;
                        stack.push_back({callee, 0});
                    }
                } else {
                    color[fnIdx] = 2;
                    stack.pop_back();
                }
            }
        }
    }

    struct Call {
        int32_t caller;
        int32_t callIndex;
        int line;
        int col;
    };

    Program& m_prog;
    std::vector<Diagnostic>& m_diags;
    NameMap m_globals;
    NameMap m_functions;
    std::vector<Call> m_calls;
    bool m_failed = false;
};

}  // namespace

bool resolveProgram(Program& program, std::vector<Diagnostic>& diags) {
    Resolver r(program, diags);
    return r.run();
}

ParseResult parseExpression(std::string_view text) {
    ParseResult result;
    std::vector<Token> tokens;
    if (!lexExpression(text, tokens, result.diagnostics)) {
        return result;
    }
    Program prog;
    prog.text.assign(text);
    if (!parseTokens(tokens, prog, result.diagnostics) || !resolveProgram(prog, result.diagnostics)) {
        return result;
    }
    // An expression must produce something: at least one expression statement.
    result.program = std::move(prog);
    return result;
}

}  // namespace xt::compile::expr
