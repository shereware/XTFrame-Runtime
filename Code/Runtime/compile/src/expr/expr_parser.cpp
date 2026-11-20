#include "expr_parser.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>

#include "expr.h"
#include "expr_builtins.h"

/// Recursive-descent parser of timeline expressions. Docs/design/58-value-functions-and-drivers.md
/// §58.2: AE's JavaScript subset -- numbers, `var`/`let`/`const`, assignment (compound, ++/--),
/// if/else, for/while with break/continue, top-level local functions, arrays of up to 4 numbers,
/// the conditional operator; the result is the last expression statement run. Constructs outside the
/// subset get a diagnostic that names them (`expr.unsupported`), not a bare syntax error.

namespace xt::compile::expr {

namespace {

struct BuiltinName {
    const char* name;
    Builtin builtin;
};

// Global functions (58 §58.2's table; port / randomPerPlay are 58 §58.5's run-time leaves).
constexpr BuiltinName kGlobalBuiltins[] = {
    {"wiggle", Builtin::Wiggle},
    {"loopOut", Builtin::LoopOut},
    {"loopIn", Builtin::LoopIn},
    {"valueAtTime", Builtin::ValueAtTime},
    {"linear", Builtin::Linear},
    {"ease", Builtin::Ease},
    {"easeIn", Builtin::EaseIn},
    {"easeOut", Builtin::EaseOut},
    {"clamp", Builtin::Clamp},
    {"random", Builtin::Random},
    {"seedRandom", Builtin::SeedRandom},
    {"noise", Builtin::Noise},
    {"posterizeTime", Builtin::PosterizeTime},
    {"degreesToRadians", Builtin::DegreesToRadians},
    {"radiansToDegrees", Builtin::RadiansToDegrees},
    {"length", Builtin::Length},
    {"port", Builtin::Port},
    {"randomPerPlay", Builtin::RandomPerPlay},
};

constexpr BuiltinName kMathBuiltins[] = {
    {"sin", Builtin::MathSin},     {"cos", Builtin::MathCos},   {"tan", Builtin::MathTan},
    {"asin", Builtin::MathAsin},   {"acos", Builtin::MathAcos}, {"atan", Builtin::MathAtan},
    {"atan2", Builtin::MathAtan2}, {"abs", Builtin::MathAbs},   {"round", Builtin::MathRound},
    {"floor", Builtin::MathFloor}, {"ceil", Builtin::MathCeil}, {"min", Builtin::MathMin},
    {"max", Builtin::MathMax},     {"pow", Builtin::MathPow},   {"sqrt", Builtin::MathSqrt},
    {"exp", Builtin::MathExp},     {"log", Builtin::MathLog},
};

/// AE names this subset leaves out, with the replacement to suggest.
struct Unsupported {
    const char* name;
    const char* message;
};
constexpr Unsupported kUnsupportedNames[] = {
    {"effect", "AE effect controls are not supported: use port(\"name\") for a value the game sends in"},
    {"sourceRectAtTime",
     "sourceRectAtTime() is not supported: use layer(\"name\").width / .height / .left / .top"},
    {"thisLayer", "thisLayer is not supported: write the row's value as value, or layer(\"name\").<row>"},
    {"thisProperty", "thisProperty is not supported: write value or valueAtTime(t)"},
    {"this", "'this' is not supported: write value or valueAtTime(t)"},
    {"comp", "comp(\"...\") is not supported: only rows of the same timeline can be referenced"},
    {"transform", "transform.<property> is not supported: write layer(\"name\").<row>"},
    {"gaussRandom", "gaussRandom() is not supported: use random()"},
    {"timeToFrames", "timeToFrames() is not supported: multiply time by the frame rate"},
    {"framesToTime", "framesToTime() is not supported: divide by the frame rate"},
    {"key", "key(n) is not supported: use valueAtTime(t)"},
    {"numKeys", "numKeys is not supported"},
    {"nearestKey", "nearestKey() is not supported"},
    {"velocity", "velocity is not supported"},
    {"speed", "speed is not supported"},
    {"loopOutDuration", "loopOutDuration() is not supported: use loopOut()"},
    {"loopInDuration", "loopInDuration() is not supported: use loopIn()"},
    {"smooth", "smooth() is not supported"},
    {"temporalWiggle", "temporalWiggle() is not supported: use wiggle()"},
};

constexpr const char* kUnsupportedKeywords[][2] = {
    {"new", "'new' and objects are not supported"},
    {"switch", "switch is not supported: use if / else"},
    {"case", "switch is not supported: use if / else"},
    {"try", "try / catch is not supported"},
    {"catch", "try / catch is not supported"},
    {"throw", "throw is not supported"},
    {"do", "do ... while is not supported: use while"},
    {"class", "classes are not supported"},
    {"typeof", "typeof is not supported"},
    {"delete", "delete is not supported"},
    {"instanceof", "instanceof is not supported"},
    {"in", "'in' is not supported"},
    {"of", "for ... of is not supported: use for (var i = 0; i < n; i++)"},
    {"null", "null is not supported: every value is a number or an array"},
    {"undefined", "undefined is not supported: every value is a number or an array"},
    {"with", "with is not supported"},
};

const char* unsupportedKeyword(const std::string& s) {
    for (const auto& k : kUnsupportedKeywords) {
        if (s == k[0]) {
            return k[1];
        }
    }
    return nullptr;
}

const char* unsupportedName(const std::string& s) {
    for (const auto& u : kUnsupportedNames) {
        if (s == u.name) {
            return u.message;
        }
    }
    return nullptr;
}

Builtin findBuiltin(const BuiltinName* table, size_t n, const std::string& s) {
    for (size_t i = 0; i < n; ++i) {
        if (s == table[i].name) {
            return table[i].builtin;
        }
    }
    return Builtin::None;
}

constexpr const char* kKeywords[] = {"var",   "let",      "const",  "if",       "else", "for",  "while",
                                     "break", "continue", "return", "function", "true", "false"};

bool isKeyword(const std::string& s) {
    for (const char* k : kKeywords) {
        if (s == k) {
            return true;
        }
    }
    return false;
}

class Parser {
public:
    Parser(const std::vector<Token>& toks, Program& prog, std::vector<Diagnostic>& diags)
        : m_toks(toks), m_prog(prog), m_diags(diags) {}

    bool run() {
        std::vector<int32_t> stmts;
        while (!m_failed && peek().kind != TokKind::End) {
            statement(stmts, /*topLevel=*/true);
        }
        if (m_failed) {
            return false;
        }
        const int32_t root = makeNode(NodeKind::Block, peek());
        setList(root, stmts);
        if (m_failed) {
            return false;
        }
        m_prog.root = root;
        return true;
    }

private:
    // ---- tokens ------------------------------------------------------------------------------
    const Token& peek(size_t ahead = 0) const {
        const size_t i = std::min(m_pos + ahead, m_toks.size() - 1);
        return m_toks[i];
    }
    const Token& next() {
        const Token& t = peek();
        if (m_pos < m_toks.size() - 1) {
            ++m_pos;
        }
        return t;
    }
    bool isPunct(const char* p, size_t ahead = 0) const {
        const Token& t = peek(ahead);
        return t.kind == TokKind::Punct && t.text == p;
    }
    bool isIdent(const char* s, size_t ahead = 0) const {
        const Token& t = peek(ahead);
        return t.kind == TokKind::Ident && t.text == s;
    }
    bool accept(const char* p) {
        if (isPunct(p)) {
            next();
            return true;
        }
        return false;
    }
    bool expect(const char* p, const char* what) {
        if (accept(p)) {
            return true;
        }
        std::string msg = "expected '";
        msg += p;
        msg += "' ";
        msg += what;
        failAt(peek(), "expr.syntax", std::move(msg));
        return false;
    }

    void failAt(const Token& t, const char* code, std::string msg) {
        if (!m_failed) {
            addExprDiagnostic(m_diags, code, std::move(msg), t.line, t.col);
        }
        m_failed = true;
    }

    // ---- nodes -------------------------------------------------------------------------------
    int32_t makeNode(NodeKind kind, const Token& at) {
        Node n;
        n.kind = kind;
        n.line = at.line;
        n.col = at.col;
        m_prog.nodes.push_back(n);
        m_depth.push_back(1);
        return static_cast<int32_t>(m_prog.nodes.size() - 1);
    }

    int depthOf(int32_t id) const { return id >= 0 ? m_depth[static_cast<size_t>(id)] : 0; }

    /// Recomputes the tree depth of `id` from its children; fails past kMaxTreeDepth.
    void finish(int32_t id) {
        const Node& n = m_prog.nodes[static_cast<size_t>(id)];
        int d = std::max(std::max(depthOf(n.a), depthOf(n.b)), std::max(depthOf(n.c), depthOf(n.d)));
        for (int32_t i = 0; i < n.count; ++i) {
            d = std::max(d, depthOf(m_prog.lists[static_cast<size_t>(n.first + i)]));
        }
        m_depth[static_cast<size_t>(id)] = d + 1;
        if (d + 1 > kMaxTreeDepth) {
            failAt(peek(), "expr.limit", "the expression is nested too deeply");
        }
    }

    int32_t make(NodeKind kind, const Token& at, int32_t a, int32_t b = -1, int32_t c = -1, int32_t d = -1,
                 uint8_t op = 0) {
        const int32_t id = makeNode(kind, at);
        Node& n = m_prog.nodes[static_cast<size_t>(id)];
        n.a = a;
        n.b = b;
        n.c = c;
        n.d = d;
        n.op = op;
        finish(id);
        return id;
    }

    void setList(int32_t id, const std::vector<int32_t>& items) {
        Node& n = m_prog.nodes[static_cast<size_t>(id)];
        n.first = static_cast<int32_t>(m_prog.lists.size());
        n.count = static_cast<int32_t>(items.size());
        m_prog.lists.insert(m_prog.lists.end(), items.begin(), items.end());
        finish(id);
    }

    int32_t internString(const std::string& s) {
        const auto it = m_stringIds.find(s);
        if (it != m_stringIds.end()) {
            return it->second;
        }
        m_prog.strings.push_back(s);
        const int32_t id = static_cast<int32_t>(m_prog.strings.size() - 1);
        m_stringIds.emplace(s, id);
        return id;
    }

    int32_t number(const Token& at, double v) {
        const int32_t id = makeNode(NodeKind::Number, at);
        m_prog.nodes[static_cast<size_t>(id)].number = v;
        return id;
    }

    // ---- statements --------------------------------------------------------------------------
    struct DepthGuard {
        Parser& p;
        explicit DepthGuard(Parser& parser) : p(parser) {
            if (++p.m_nest > kMaxParseDepth) {
                p.failAt(p.peek(), "expr.limit", "the expression is nested too deeply");
            }
        }
        ~DepthGuard() { --p.m_nest; }
        DepthGuard(const DepthGuard&) = delete;
        DepthGuard& operator=(const DepthGuard&) = delete;
    };

    /// A statement ends at ';', before '}', at the end, or at a line break (AE accepts all four).
    void terminator() {
        if (accept(";")) {
            return;
        }
        if (isPunct("}") || peek().kind == TokKind::End || peek().newlineBefore) {
            return;
        }
        if (isPunct(",")) {
            failAt(peek(), "expr.unsupported", "the comma operator is not supported");
            return;
        }
        failAt(peek(), "expr.syntax", "expected ';' or a line break");
    }

    void statement(std::vector<int32_t>& out, bool topLevel) {
        DepthGuard guard(*this);
        if (m_failed) {
            return;
        }
        const Token& t = peek();
        if (t.kind == TokKind::Punct && t.text == ";") {
            next();
            return;
        }
        if (t.kind == TokKind::Punct && t.text == "{") {
            out.push_back(block());
            return;
        }
        if (t.kind == TokKind::Ident) {
            if (t.text == "var" || t.text == "let" || t.text == "const") {
                next();
                varDecls(out);
                terminator();
                return;
            }
            if (t.text == "if") {
                out.push_back(ifStatement());
                return;
            }
            if (t.text == "for") {
                out.push_back(forStatement());
                return;
            }
            if (t.text == "while") {
                const Token& at = next();
                expect("(", "after while");
                const int32_t cond = expression();
                expect(")", "after the while condition");
                ++m_loopDepth;
                const int32_t body = statementAsNode();
                --m_loopDepth;
                out.push_back(make(NodeKind::While, at, cond, -1, -1, body));
                return;
            }
            if (t.text == "break" || t.text == "continue") {
                const Token& at = next();
                if (m_loopDepth == 0) {
                    failAt(at, "expr.syntax", "'" + at.text + "' outside a loop");
                    return;
                }
                out.push_back(makeNode(at.text == "break" ? NodeKind::Break : NodeKind::Continue, at));
                terminator();
                return;
            }
            if (t.text == "return") {
                const Token& at = next();
                if (!m_inFunction) {
                    failAt(at, "expr.syntax",
                           "'return' outside a function: the result is the last expression, write it alone");
                    return;
                }
                int32_t value = -1;
                if (!isPunct(";") && !isPunct("}") && peek().kind != TokKind::End && !peek().newlineBefore) {
                    value = expression();
                }
                out.push_back(make(NodeKind::Return, at, value));
                terminator();
                return;
            }
            if (t.text == "function") {
                if (!topLevel || m_inFunction) {
                    failAt(t, "expr.unsupported", "functions can only be declared at the top level");
                    return;
                }
                out.push_back(functionDecl());
                return;
            }
            if (t.text == "else") {
                failAt(t, "expr.syntax", "'else' without 'if'");
                return;
            }
            if (const char* msg = unsupportedKeyword(t.text)) {
                failAt(t, "expr.unsupported", msg);
                return;
            }
        }
        const Token& at = peek();
        const int32_t e = expression();
        if (m_failed) {
            return;
        }
        out.push_back(make(NodeKind::ExprStmt, at, e));
        terminator();
    }

    int32_t statementAsNode() {
        std::vector<int32_t> stmts;
        const Token& at = peek();
        statement(stmts, false);
        if (stmts.size() == 1) {
            return stmts[0];
        }
        const int32_t b = makeNode(NodeKind::Block, at);
        setList(b, stmts);
        return b;
    }

    int32_t block() {
        const Token& at = next();  // '{'
        std::vector<int32_t> stmts;
        while (!m_failed && !isPunct("}")) {
            if (peek().kind == TokKind::End) {
                failAt(peek(), "expr.syntax", "missing '}'");
                break;
            }
            statement(stmts, false);
        }
        accept("}");
        const int32_t b = makeNode(NodeKind::Block, at);
        setList(b, stmts);
        return b;
    }

    void varDecls(std::vector<int32_t>& out) {
        while (!m_failed) {
            const Token& name = next();
            if (name.kind != TokKind::Ident) {
                failAt(name, "expr.syntax", "expected a variable name");
                return;
            }
            if (isReservedName(name.text) || isKeyword(name.text) || unsupportedKeyword(name.text)) {
                failAt(name, "expr.syntax", "'" + name.text + "' is a built-in name and cannot be declared");
                return;
            }
            int32_t init = -1;
            if (accept("=")) {
                init = assignment();
            }
            const int32_t decl = make(NodeKind::VarDecl, name, init);
            m_prog.nodes[static_cast<size_t>(decl)].slot = internString(name.text);
            out.push_back(decl);
            if (!accept(",")) {
                return;
            }
        }
    }

    int32_t ifStatement() {
        const Token& at = next();
        expect("(", "after if");
        const int32_t cond = expression();
        expect(")", "after the if condition");
        const int32_t then = statementAsNode();
        int32_t otherwise = -1;
        if (isIdent("else")) {
            next();
            otherwise = statementAsNode();
        }
        return make(NodeKind::If, at, cond, then, otherwise);
    }

    int32_t forStatement() {
        const Token& at = next();
        expect("(", "after for");
        int32_t init = -1;
        if (isIdent("var") || isIdent("let") || isIdent("const")) {
            next();
            std::vector<int32_t> decls;
            varDecls(decls);
            if (decls.size() == 1) {
                init = decls[0];
            } else {
                init = makeNode(NodeKind::Block, at);
                setList(init, decls);
            }
        } else if (!isPunct(";")) {
            const Token& eat = peek();
            init = make(NodeKind::ExprStmt, eat, expression());
        }
        if (isIdent("in") || isIdent("of")) {
            failAt(peek(), "expr.unsupported",
                   "for ... in / for ... of is not supported: use for (var i = 0; i < n; i++)");
            return -1;
        }
        expect(";", "after the for initialiser");
        int32_t cond = -1;
        if (!isPunct(";")) {
            cond = expression();
        }
        expect(";", "after the for condition");
        int32_t update = -1;
        if (!isPunct(")")) {
            update = expression();
        }
        expect(")", "after the for clauses");
        ++m_loopDepth;
        const int32_t body = statementAsNode();
        --m_loopDepth;
        return make(NodeKind::For, at, init, cond, update, body);
    }

    int32_t functionDecl() {
        const Token& at = next();  // 'function'
        const Token& name = next();
        if (name.kind != TokKind::Ident) {
            failAt(name, "expr.syntax", "expected a function name");
            return -1;
        }
        if (isReservedName(name.text) || isKeyword(name.text)) {
            failAt(name, "expr.syntax", "'" + name.text + "' is a built-in name and cannot be redefined");
            return -1;
        }
        Function fn;
        fn.name = name.text;
        fn.line = at.line;
        expect("(", "after the function name");
        std::vector<std::string> params;
        if (!isPunct(")")) {
            while (!m_failed) {
                const Token& p = next();
                if (p.kind != TokKind::Ident || isReservedName(p.text) || isKeyword(p.text)) {
                    failAt(p, "expr.syntax", "expected a parameter name");
                    return -1;
                }
                if (std::find(params.begin(), params.end(), p.text) != params.end()) {
                    failAt(p, "expr.syntax", "duplicate parameter '" + p.text + "'");
                    return -1;
                }
                params.push_back(p.text);
                if (!accept(",")) {
                    break;
                }
            }
        }
        expect(")", "after the parameters");
        if (!isPunct("{")) {
            failAt(peek(), "expr.syntax", "expected '{' to start the function body");
            return -1;
        }
        m_inFunction = true;
        const int saveLoop = m_loopDepth;
        m_loopDepth = 0;
        fn.body = block();
        m_loopDepth = saveLoop;
        m_inFunction = false;
        fn.paramCount = static_cast<int32_t>(params.size());
        // The parameter names travel to the resolver as the first strings of a VarDecl-free list:
        // record them in m_prog.strings and remember their indices in the body's Block `slot`.
        const int32_t node = makeNode(NodeKind::Function, at);
        m_prog.nodes[static_cast<size_t>(node)].slot = static_cast<int32_t>(m_prog.functions.size());
        std::vector<int32_t> paramIds;
        for (const std::string& p : params) {
            paramIds.push_back(internString(p));
        }
        // Parameter string indices live in the Function node's list (they are not node ids).
        Node& fnNode = m_prog.nodes[static_cast<size_t>(node)];
        fnNode.first = static_cast<int32_t>(m_prog.lists.size());
        fnNode.count = static_cast<int32_t>(paramIds.size());
        m_prog.lists.insert(m_prog.lists.end(), paramIds.begin(), paramIds.end());
        m_prog.functions.push_back(std::move(fn));
        return node;
    }

    // ---- expressions -------------------------------------------------------------------------
    int32_t expression() {
        const int32_t e = assignment();
        if (!m_failed && isPunct(",")) {
            failAt(peek(), "expr.unsupported", "the comma operator is not supported");
        }
        return e;
    }

    static bool isAssignable(const Node& n) { return n.kind == NodeKind::Var || n.kind == NodeKind::Element; }

    int32_t assignment() {
        DepthGuard guard(*this);
        if (m_failed) {
            return -1;
        }
        const int32_t lhs = conditional();
        if (m_failed) {
            return -1;
        }
        static constexpr struct {
            const char* p;
            AssignOp op;
        } kOps[] = {{"=", AssignOp::Set},  {"+=", AssignOp::Add}, {"-=", AssignOp::Sub},
                    {"*=", AssignOp::Mul}, {"/=", AssignOp::Div}, {"%=", AssignOp::Mod}};
        for (const auto& o : kOps) {
            if (isPunct(o.p)) {
                const Token& at = next();
                if (!isAssignable(m_prog.nodes[static_cast<size_t>(lhs)])) {
                    failAt(at, "expr.syntax",
                           "the left side of an assignment must be a variable or an element");
                    return -1;
                }
                const int32_t rhs = assignment();
                return make(NodeKind::Assign, at, lhs, rhs, -1, -1, static_cast<uint8_t>(o.op));
            }
        }
        if (isPunct("=>")) {
            failAt(peek(), "expr.unsupported",
                   "arrow functions are not supported: use function f(x) { ... }");
            return -1;
        }
        return lhs;
    }

    int32_t conditional() {
        const int32_t cond = logicalOr();
        if (!m_failed && isPunct("?")) {
            const Token& at = next();
            const int32_t a = assignment();
            expect(":", "in the conditional operator");
            const int32_t b = assignment();
            return make(NodeKind::Conditional, at, cond, a, b);
        }
        return cond;
    }

    int32_t logicalOr() {
        int32_t lhs = logicalAnd();
        while (!m_failed && isPunct("||")) {
            const Token& at = next();
            lhs = make(NodeKind::Logical, at, lhs, logicalAnd(), -1, -1, static_cast<uint8_t>(LogicalOp::Or));
        }
        return lhs;
    }

    int32_t logicalAnd() {
        int32_t lhs = equality();
        while (!m_failed && isPunct("&&")) {
            const Token& at = next();
            lhs = make(NodeKind::Logical, at, lhs, equality(), -1, -1, static_cast<uint8_t>(LogicalOp::And));
        }
        return lhs;
    }

    int32_t binaryLevel(int level) {
        // 0 equality, 1 relational, 2 additive, 3 multiplicative.
        static constexpr struct {
            const char* p;
            BinaryOp op;
            int level;
        } kOps[] = {{"===", BinaryOp::Eq, 0}, {"!==", BinaryOp::Ne, 0}, {"==", BinaryOp::Eq, 0},
                    {"!=", BinaryOp::Ne, 0},  {"<=", BinaryOp::Le, 1},  {">=", BinaryOp::Ge, 1},
                    {"<", BinaryOp::Lt, 1},   {">", BinaryOp::Gt, 1},   {"+", BinaryOp::Add, 2},
                    {"-", BinaryOp::Sub, 2},  {"*", BinaryOp::Mul, 3},  {"/", BinaryOp::Div, 3},
                    {"%", BinaryOp::Mod, 3}};
        int32_t lhs = level == 3 ? unary() : binaryLevel(level + 1);
        while (!m_failed) {
            bool matched = false;
            for (const auto& o : kOps) {
                if (o.level == level && isPunct(o.p)) {
                    const Token& at = next();
                    const int32_t rhs = level == 3 ? unary() : binaryLevel(level + 1);
                    lhs = make(NodeKind::Binary, at, lhs, rhs, -1, -1, static_cast<uint8_t>(o.op));
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                break;
            }
        }
        if (!m_failed && level == 0) {
            for (const char* bad : {"&", "|", "^", "~", "<<", ">>"}) {
                if (isPunct(bad)) {
                    failAt(peek(), "expr.unsupported", "bitwise operators are not supported");
                }
            }
            if (isPunct("**")) {
                failAt(peek(), "expr.unsupported", "'**' is not supported: use Math.pow(a, b)");
            }
        }
        return lhs;
    }

    int32_t equality() { return binaryLevel(0); }

    int32_t unary() {
        DepthGuard guard(*this);
        if (m_failed) {
            return -1;
        }
        const Token& t = peek();
        if (t.kind == TokKind::Punct) {
            if (t.text == "-" || t.text == "+" || t.text == "!") {
                next();
                const UnaryOp op =
                    t.text == "-" ? UnaryOp::Neg : (t.text == "+" ? UnaryOp::Plus : UnaryOp::Not);
                const int32_t operand = unary();
                return make(NodeKind::Unary, t, operand, -1, -1, -1, static_cast<uint8_t>(op));
            }
            if (t.text == "++" || t.text == "--") {
                next();
                const int32_t target = unary();
                if (!m_failed && !isAssignable(m_prog.nodes[static_cast<size_t>(target)])) {
                    failAt(t, "expr.syntax", "'" + t.text + "' needs a variable or an element");
                    return -1;
                }
                const IncDecOp op = t.text == "++" ? IncDecOp::PreInc : IncDecOp::PreDec;
                return make(NodeKind::IncDec, t, target, -1, -1, -1, static_cast<uint8_t>(op));
            }
            if (t.text == "~") {
                failAt(t, "expr.unsupported", "bitwise operators are not supported");
                return -1;
            }
        }
        if (t.kind == TokKind::Ident) {
            if (const char* msg = unsupportedKeyword(t.text)) {
                failAt(t, "expr.unsupported", msg);
                return -1;
            }
        }
        int32_t e = postfix(primary());
        if (!m_failed && (isPunct("++") || isPunct("--")) && !peek().newlineBefore) {
            const Token& at = next();
            if (!isAssignable(m_prog.nodes[static_cast<size_t>(e)])) {
                failAt(at, "expr.syntax", "'" + at.text + "' needs a variable or an element");
                return -1;
            }
            const IncDecOp op = at.text == "++" ? IncDecOp::PostInc : IncDecOp::PostDec;
            e = make(NodeKind::IncDec, at, e, -1, -1, -1, static_cast<uint8_t>(op));
        }
        return e;
    }

    /// `[index]`, `.length`; any other member access is outside the subset.
    int32_t postfix(int32_t e) {
        while (!m_failed && e >= 0) {
            if (isPunct("[")) {
                const Token& at = next();
                const int32_t index = expression();
                expect("]", "after the index");
                e = make(NodeKind::Element, at, e, index);
            } else if (isPunct(".")) {
                const Token& at = next();
                const Token& member = next();
                if (member.kind != TokKind::Ident) {
                    failAt(member, "expr.syntax", "expected a name after '.'");
                    return -1;
                }
                if (member.text == "length") {
                    e = make(NodeKind::ArrayLength, at, e);
                } else if (member.text == "valueAtTime") {
                    failAt(member, "expr.unsupported",
                           "valueAtTime can be called as valueAtTime(t), "
                           "layer(\"name\").<row>.valueAtTime(t) or "
                           "port(\"name\").valueAtTime(t)");
                    return -1;
                } else {
                    failAt(member, "expr.unsupported",
                           "'." + member.text +
                               "' is not supported: member access is limited to Math.*, thisComp.duration, "
                               "layer(...).<row> and .length");
                    return -1;
                }
            } else if (isPunct("(")) {
                failAt(peek(), "expr.unsupported", "only named functions can be called");
                return -1;
            } else {
                break;
            }
        }
        return e;
    }

    std::vector<int32_t> arguments() {
        std::vector<int32_t> args;
        expect("(", "to start the arguments");
        if (!isPunct(")")) {
            while (!m_failed) {
                args.push_back(assignment());
                if (!accept(",")) {
                    break;
                }
            }
        }
        expect(")", "after the arguments");
        return args;
    }

    int32_t call(const Token& at, Builtin b, const std::vector<int32_t>& args) {
        if (m_failed) {
            return -1;
        }
        int minArgs = 0;
        int maxArgs = 0;
        builtinArity(b, minArgs, maxArgs);
        const int argc = static_cast<int>(args.size());
        const bool fourOfFive = argc == 4 && (b == Builtin::Linear || b == Builtin::Ease ||
                                              b == Builtin::EaseIn || b == Builtin::EaseOut);
        if (argc < minArgs || argc > maxArgs || fourOfFive || (b == Builtin::RandomPerPlay && argc == 1)) {
            std::string msg = std::string(builtinName(b)) + "() takes ";
            if (fourOfFive || b == Builtin::Linear || b == Builtin::Ease || b == Builtin::EaseIn ||
                b == Builtin::EaseOut) {
                msg += "3 or 5 arguments";
            } else if (b == Builtin::RandomPerPlay) {
                msg += "0 or 2 arguments";
            } else if (minArgs == maxArgs) {
                msg += std::to_string(minArgs) + (minArgs == 1 ? " argument" : " arguments");
            } else {
                msg += std::to_string(minArgs) + " to " + std::to_string(maxArgs) + " arguments";
            }
            failAt(at, "expr.syntax", std::move(msg));
            return -1;
        }
        if ((b == Builtin::LoopOut || b == Builtin::LoopIn) && argc >= 1) {
            const Node& type = m_prog.nodes[static_cast<size_t>(args[0])];
            LoopType lt = LoopType::Cycle;
            if (type.kind != NodeKind::String ||
                !parseLoopType(m_prog.strings[static_cast<size_t>(type.slot)], lt)) {
                failAt(at, "expr.syntax",
                       std::string(builtinName(b)) +
                           "() takes a type in quotes: \"cycle\", \"pingpong\", \"offset\" or \"continue\"");
                return -1;
            }
        }
        const int32_t id = makeNode(NodeKind::Call, at);
        Node& n = m_prog.nodes[static_cast<size_t>(id)];
        n.op = static_cast<uint8_t>(b);
        n.site = m_prog.siteCount++;
        setList(id, args);
        if (b == Builtin::Port || b == Builtin::RandomPerPlay) {
            m_prog.hasRuntimeLeaf = true;
        }
        if (b == Builtin::PosterizeTime) {
            m_prog.hasPosterize = true;
        }
        return id;
    }

    /// `layer(key)` followed by its member: `.width`/`.height`/`.left`/`.top` (a run-time leaf) or
    /// `.<row>` with an optional `.valueAtTime(t)` (T-770).
    int32_t layerRef(const Token& at) {
        std::vector<int32_t> args = arguments();
        if (m_failed) {
            return -1;
        }
        if (args.size() != 1) {
            failAt(at, "expr.syntax", "layer() takes one argument: a name or a sibling index");
            return -1;
        }
        if (!isPunct(".")) {
            failAt(peek(), "expr.syntax",
                   "layer(...) must be followed by a row name, e.g. layer(\"Logo\").y");
            return -1;
        }
        next();
        const Token& member = next();
        if (member.kind != TokKind::Ident) {
            failAt(member, "expr.syntax", "expected a row name after layer(...).");
            return -1;
        }
        static constexpr struct {
            const char* name;
            LayoutWhich which;
        } kLayout[] = {{"left", LayoutWhich::Left},
                       {"top", LayoutWhich::Top},
                       {"width", LayoutWhich::Width},
                       {"height", LayoutWhich::Height}};
        for (const auto& l : kLayout) {
            if (member.text == l.name) {
                if (isPunct(".") && isIdent("valueAtTime", 1)) {
                    // T-777 (58 §58.5a): no history of layout results is kept.
                    failAt(peek(), "expr.limit",
                           "layer(...)." + member.text +
                               " is read only as it is now: the history of another element's layout "
                               "result is not kept, so valueAtTime() cannot be used on it");
                    return -1;
                }
                m_prog.hasRuntimeLeaf = true;
                return make(NodeKind::LayerLayout, at, args[0], -1, -1, -1, static_cast<uint8_t>(l.which));
            }
        }
        if (member.text == "sourceRectAtTime") {
            failAt(member, "expr.unsupported",
                   "sourceRectAtTime() is not supported: use layer(\"name\").width / .height / .left / .top");
            return -1;
        }
        if (member.text == "transform" || member.text == "effect") {
            failAt(member, "expr.unsupported",
                   member.text == "effect" ? "AE effect controls are not supported: use port(\"name\")"
                                           : "write layer(\"name\").<row>, e.g. layer(\"Logo\").y");
            return -1;
        }
        m_prog.hasLayerRef = true;
        // T-770: a component of a two-dimensional row, `layer("A").translate.y` / `.position.x`.
        std::string rowName = member.text;
        if (isPunct(".") && (isIdent("x", 1) || isIdent("y", 1) || isIdent("z", 1))) {
            next();
            rowName += "." + next().text;
        }
        const int32_t prop = internString(rowName);
        if (isPunct(".") && isIdent("valueAtTime", 1)) {
            next();
            next();
            std::vector<int32_t> targs = arguments();
            if (targs.size() != 1) {
                failAt(at, "expr.syntax", "valueAtTime() takes one argument");
                return -1;
            }
            const int32_t id = make(NodeKind::LayerPropAt, at, args[0], targs[0]);
            m_prog.nodes[static_cast<size_t>(id)].slot = prop;
            return id;
        }
        const int32_t id = make(NodeKind::LayerProp, at, args[0]);
        m_prog.nodes[static_cast<size_t>(id)].slot = prop;
        return id;
    }

    int32_t portRef(const Token& at) {
        std::vector<int32_t> args = arguments();
        if (m_failed) {
            return -1;
        }
        if (args.size() != 1 || m_prog.nodes[static_cast<size_t>(args[0])].kind != NodeKind::String) {
            failAt(at, "expr.syntax", "port() takes one argument: the port's name in quotes");
            return -1;
        }
        m_prog.hasRuntimeLeaf = true;
        if (isPunct(".") && isIdent("valueAtTime", 1)) {
            next();
            next();
            std::vector<int32_t> targs = arguments();
            if (targs.size() != 1) {
                failAt(at, "expr.syntax", "valueAtTime() takes one argument");
                return -1;
            }
            return make(NodeKind::PortAt, at, args[0], targs[0]);
        }
        return call(at, Builtin::Port, args);
    }

    int32_t identifier(const Token& t) {
        const std::string& s = t.text;
        if (s == "true" || s == "false") {
            return number(t, s == "true" ? 1.0 : 0.0);
        }
        if (s == "time") {
            return makeNode(NodeKind::Time, t);
        }
        if (s == "value") {
            return makeNode(NodeKind::Value, t);
        }
        if (s == "valueVec") {
            return makeNode(NodeKind::ValueVec, t);
        }
        if (s == "index") {
            return makeNode(NodeKind::Index, t);
        }
        if (s == "Math") {
            expect(".", "after Math");
            const Token& member = next();
            if (member.kind != TokKind::Ident) {
                failAt(member, "expr.syntax", "expected a name after Math.");
                return -1;
            }
            if (member.text == "PI") {
                return number(member, 3.14159265358979323846);
            }
            const Builtin b = findBuiltin(kMathBuiltins, std::size(kMathBuiltins), member.text);
            if (b == Builtin::None) {
                failAt(member, "expr.unsupported", "Math." + member.text + " is not supported");
                return -1;
            }
            if (!isPunct("(")) {
                failAt(peek(), "expr.syntax", "Math." + member.text + " must be called");
                return -1;
            }
            return call(member, b, arguments());
        }
        if (s == "thisComp") {
            expect(".", "after thisComp");
            const Token& member = next();
            if (member.kind == TokKind::Ident && member.text == "duration") {
                return makeNode(NodeKind::Duration, t);
            }
            if (member.kind == TokKind::Ident && member.text == "layer") {
                failAt(member, "expr.unsupported",
                       "write layer(\"name\").<row> (the short form pick-whip writes)");
                return -1;
            }
            failAt(member, "expr.unsupported", "only thisComp.duration is supported");
            return -1;
        }
        if (s == "layer") {
            if (!isPunct("(")) {
                failAt(peek(), "expr.syntax", "layer must be called: layer(\"name\").<row>");
                return -1;
            }
            return layerRef(t);
        }
        if (s == "port") {
            if (!isPunct("(")) {
                failAt(peek(), "expr.syntax", "port must be called: port(\"name\")");
                return -1;
            }
            return portRef(t);
        }
        if (const char* msg = unsupportedName(s)) {
            failAt(t, "expr.unsupported", msg);
            return -1;
        }
        const Builtin b = findBuiltin(kGlobalBuiltins, std::size(kGlobalBuiltins), s);
        if (b != Builtin::None) {
            if (!isPunct("(")) {
                failAt(peek(), "expr.syntax", s + " must be called");
                return -1;
            }
            return call(t, b, arguments());
        }
        if (isKeyword(s)) {
            failAt(t, "expr.syntax", "unexpected '" + s + "'");
            return -1;
        }
        if (isPunct("(")) {
            const std::vector<int32_t> args = arguments();
            const int32_t id = makeNode(NodeKind::LocalCall, t);
            m_prog.nodes[static_cast<size_t>(id)].slot = internString(s);
            setList(id, args);
            return id;
        }
        const int32_t id = makeNode(NodeKind::Var, t);
        m_prog.nodes[static_cast<size_t>(id)].slot = internString(s);
        return id;
    }

    int32_t primary() {
        if (m_failed) {
            return -1;
        }
        const Token& t = next();
        switch (t.kind) {
            case TokKind::Number:
                return number(t, t.number);
            case TokKind::String: {
                const int32_t id = makeNode(NodeKind::String, t);
                m_prog.nodes[static_cast<size_t>(id)].slot = internString(t.text);
                return id;
            }
            case TokKind::Ident:
                return identifier(t);
            case TokKind::Punct:
                if (t.text == "(") {
                    const int32_t e = expression();
                    expect(")", "to close the parenthesis");
                    return e;
                }
                if (t.text == "[") {
                    std::vector<int32_t> items;
                    if (!isPunct("]")) {
                        while (!m_failed) {
                            items.push_back(assignment());
                            if (!accept(",")) {
                                break;
                            }
                        }
                    }
                    expect("]", "to close the array");
                    if (static_cast<int>(items.size()) > kMaxArrayLength) {
                        failAt(t, "expr.unsupported", "arrays hold at most 4 numbers");
                        return -1;
                    }
                    if (items.empty()) {
                        failAt(t, "expr.unsupported", "empty arrays are not supported");
                        return -1;
                    }
                    const int32_t id = makeNode(NodeKind::Array, t);
                    setList(id, items);
                    return id;
                }
                if (t.text == "{") {
                    failAt(t, "expr.unsupported", "objects are not supported");
                    return -1;
                }
                failAt(t, "expr.syntax", "unexpected '" + t.text + "'");
                return -1;
            case TokKind::End:
                failAt(t, "expr.syntax", "unexpected end of the expression");
                return -1;
        }
        return -1;
    }

    const std::vector<Token>& m_toks;
    Program& m_prog;
    std::vector<Diagnostic>& m_diags;
    std::vector<int> m_depth;
    std::unordered_map<std::string, int32_t> m_stringIds;
    size_t m_pos = 0;
    int m_nest = 0;
    int m_loopDepth = 0;
    bool m_inFunction = false;
    bool m_failed = false;
};

}  // namespace

bool isReservedName(const std::string& name) {
    static constexpr const char* kReserved[] = {"time", "value",    "valueVec", "index",
                                                "Math", "thisComp", "layer",    "port"};
    for (const char* r : kReserved) {
        if (name == r) {
            return true;
        }
    }
    return findBuiltin(kGlobalBuiltins, std::size(kGlobalBuiltins), name) != Builtin::None ||
           unsupportedName(name) != nullptr;
}

bool parseTokens(const std::vector<Token>& tokens, Program& program, std::vector<Diagnostic>& diags) {
    Parser parser(tokens, program, diags);
    return parser.run();
}

}  // namespace xt::compile::expr
