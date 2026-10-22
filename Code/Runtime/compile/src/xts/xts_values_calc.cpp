#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include "xt/charconv.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/parse_value.h"
#include "xt/compile/parse_xts.h"
#include "xt/compile/style_model.h"
#include "xt/num_guard.h"
#include "xt/property.h"
#include "xts_lexer.h"
#include "xts_props.h"
#include "xts_values_internal.h"

/// Per-property value parsing (split from xts_values.cpp by T-412): calc().

namespace xt::compile {

using namespace xts_values_detail;  // T-412: helpers shared between the split files

namespace {

// -------------------------------------------------------------------------------------------
// T-176: `calc()` (Docs/design/05-style-and-css.md §5.9.2, 06-layout.md §6.4).
// -------------------------------------------------------------------------------------------

/// The operator character a token stands for inside `calc()`, or 0. `*` has its own token; a
/// spaced `-` lexes as the identifier "-"; `+` and `/` have no token and arrive as Unknown
/// "unexpected character" tokens.
char calcOperatorChar(const Tok& t) {
    if (t.kind == TokKind::Star) {
        return '*';
    }
    if (t.kind == TokKind::Ident && t.text == "-") {
        return '-';
    }
    if (t.kind == TokKind::Unknown) {
        const std::string_view prefix = "unexpected character '";
        if (t.text.size() == prefix.size() + 2 && t.text.compare(0, prefix.size(), prefix) == 0) {
            const char c = t.text[prefix.size()];
            if (c == '+' || c == '/') {
                return c;
            }
        }
    }
    return 0;
}

int calcPrecedence(char op) { return (op == '*' || op == '/') ? 2 : 1; }

CalcOpKind calcOpKind(char op) {
    switch (op) {
        case 'm':
            return CalcOpKind::Min;
        case 'M':
            return CalcOpKind::Max;
        case '+':
            return CalcOpKind::Add;
        case '-':
            return CalcOpKind::Sub;
        case '*':
            return CalcOpKind::Mul;
        default:
            return CalcOpKind::Div;
    }
}

/// A calc() operand token: a number (unitless) or a length. `%`, px, em and the T-176 units.
bool parseCalcOperand(const Tok& t, CalcOpDecl& out) {
    if (t.kind != TokKind::Number) {
        return false;
    }
    if (t.unit.empty()) {
        out.op = CalcOpKind::PushNumber;
        out.value = t.num;
        return true;
    }
    Length len;
    if (!parseLengthTok(t, len) || len.unit == LengthUnit::Auto) {
        return false;
    }
    out.op = CalcOpKind::PushLength;
    out.unit = len.unit;
    out.value = len.value;
    return true;
}

/// T-330: what an identifier opens in operand position: '(' for `calc(` (a plain group), 'm' for
/// `min(`, 'M' for `max(`, 'c' for `clamp(`; 0 for anything else.
char calcFunctionMarker(const Tok& t) {
    if (t.kind != TokKind::Ident) {
        return 0;
    }
    if (css_values::iequals(t.text, "calc")) {
        return '(';
    }
    if (css_values::iequals(t.text, "min")) {
        return 'm';
    }
    if (css_values::iequals(t.text, "max")) {
        return 'M';
    }
    return css_values::iequals(t.text, "clamp") ? 'c' : 0;
}

}  // namespace

namespace xts_values_detail {

bool isCalcFunctionValue(const std::vector<Tok>& tokens) {
    if (tokens.size() < 3 || calcFunctionMarker(tokens[0]) == 0 || tokens[1].kind != TokKind::LParen ||
        tokens.back().kind != TokKind::RParen) {
        return false;
    }
    if (css_values::iequals(tokens[0].text, "calc")) {
        return true;
    }
    // T-330: `min(...)`, `max(...)`, `clamp(...)` -- one function call whose `)` is the last token.
    int depth = 0;
    for (size_t i = 1; i < tokens.size(); ++i) {
        depth += tokens[i].kind == TokKind::LParen ? 1 : (tokens[i].kind == TokKind::RParen ? -1 : 0);
        if (depth == 0 && i + 1 < tokens.size()) {
            return false;
        }
    }
    return depth == 0;
}

bool parseCalcValue(const std::vector<Tok>& tokens, std::vector<CalcOpDecl>& out, std::string& error) {
    out.clear();
    if (!isCalcFunctionValue(tokens)) {
        error = "expected calc(...), min(...), max(...) or clamp(...)";
        return false;
    }
    // Shunting-yard over the tokens between `calc(` and the final `)` (T-330: over the whole
    // `min(...)` / `max(...)` / `clamp(...)`); a nested `(` or `calc(` groups. Operators: + - * /,
    // left-associative, * / binding tighter. T-330: `min(` / `max(` / `clamp(` open a function
    // whose comma-separated arguments are full expressions; min/max of n arguments emit n - 1
    // Min/Max ops, clamp(a, b, c) emits `a b c Min Max` = max(a, min(b, c)).
    std::vector<char> ops;       // '(' marks a group, 'm' / 'M' / 'c' a function.
    std::vector<int> argCounts;  // Arguments so far, one entry per open function.
    bool expectOperand = true;
    const bool calcWrapped = css_values::iequals(tokens[0].text, "calc");
    const size_t begin = calcWrapped ? 2 : 0;
    const size_t end = calcWrapped ? tokens.size() - 1 : tokens.size();
    constexpr size_t kMaxOps = 64;
    auto isMarker = [](char c) { return c == '(' || c == 'm' || c == 'M' || c == 'c'; };
    auto popOperators = [&] {
        while (!ops.empty() && !isMarker(ops.back())) {
            CalcOpDecl op;
            op.op = calcOpKind(ops.back());
            out.push_back(op);
            ops.pop_back();
        }
    };
    for (size_t i = begin; i < end; ++i) {
        const Tok& t = tokens[i];
        if (expectOperand) {
            const char marker = calcFunctionMarker(t);
            if (t.kind == TokKind::LParen ||
                (marker != 0 && i + 1 < end && tokens[i + 1].kind == TokKind::LParen)) {
                if (t.kind == TokKind::Ident) {
                    ++i;  // `calc(` nested: same as a plain group; `min(` etc.: a function.
                }
                const char open = t.kind == TokKind::LParen ? '(' : marker;
                ops.push_back(open);
                if (open != '(') {
                    argCounts.push_back(1);
                }
                continue;
            }
            if (isKeyword(t, "var") && i + 1 < end && tokens[i + 1].kind == TokKind::LParen) {
                // var(--name) or var(--name, <number | length>).
                CalcOpDecl v;
                v.op = CalcOpKind::PushVar;
                if (i + 3 >= end || tokens[i + 2].kind != TokKind::Ident || tokens[i + 2].text.size() < 3 ||
                    tokens[i + 2].text.compare(0, 2, "--") != 0) {
                    error = "expected var(--name) in calc()";
                    return false;
                }
                v.varName = tokens[i + 2].text;
                size_t j = i + 3;
                if (tokens[j].kind == TokKind::Comma) {
                    CalcOpDecl fb;
                    if (j + 2 >= tokens.size() || !parseCalcOperand(tokens[j + 1], fb) ||
                        tokens[j + 2].kind != TokKind::RParen) {
                        error = "a var() fallback inside calc() must be one number or length";
                        return false;
                    }
                    v.hasFallback = true;
                    v.fallbackIsNumber = fb.op == CalcOpKind::PushNumber;
                    v.unit = fb.unit;
                    v.value = fb.value;
                    j += 2;
                }
                if (j >= end || tokens[j].kind != TokKind::RParen) {
                    error = "expected ')' after var() in calc()";
                    return false;
                }
                out.push_back(v);
                i = j;
                expectOperand = false;
                continue;
            }
            CalcOpDecl operand;
            if (!parseCalcOperand(t, operand)) {
                error = "expected a number or a length in calc()";
                return false;
            }
            out.push_back(operand);
            expectOperand = false;
            continue;
        }
        if (t.kind == TokKind::Comma) {
            // T-330: the next argument of the innermost min() / max() / clamp().
            popOperators();
            if (ops.empty() || ops.back() == '(') {
                error = "',' outside min(), max() or clamp() in calc()";
                return false;
            }
            ++argCounts.back();
            expectOperand = true;
            continue;
        }
        if (t.kind == TokKind::RParen) {
            popOperators();
            if (ops.empty()) {
                error = "unbalanced ')' in calc()";
                return false;
            }
            const char open = ops.back();
            ops.pop_back();
            if (open == '(') {
                continue;
            }
            const int args = argCounts.back();
            argCounts.pop_back();
            CalcOpDecl op;
            if (open == 'c') {
                if (args != 3) {
                    error = "clamp() takes exactly three arguments";
                    return false;
                }
                op.op = CalcOpKind::Min;
                out.push_back(op);
                op.op = CalcOpKind::Max;
                out.push_back(op);
                continue;
            }
            op.op = calcOpKind(open);
            for (int a = 1; a < args; ++a) {
                out.push_back(op);
            }
            continue;
        }
        const char c = calcOperatorChar(t);
        if (c == 0) {
            error = "expected an operator (+ - * /, with spaces around + and -) in calc()";
            return false;
        }
        while (!ops.empty() && !isMarker(ops.back()) && calcPrecedence(ops.back()) >= calcPrecedence(c)) {
            CalcOpDecl op;
            op.op = calcOpKind(ops.back());
            out.push_back(op);
            ops.pop_back();
        }
        ops.push_back(c);
        expectOperand = true;
    }
    if (expectOperand) {
        error = "calc() ends with an operator or is empty";
        return false;
    }
    while (!ops.empty()) {
        if (isMarker(ops.back())) {
            error = "unbalanced '(' in calc()";
            return false;
        }
        CalcOpDecl op;
        op.op = calcOpKind(ops.back());
        out.push_back(op);
        ops.pop_back();
    }
    if (out.size() > kMaxOps) {
        error = "calc() is too long";
        return false;
    }
    // Type check: 0 = number, 1 = length, 2 = unknown (a var()). + and - need matching types, *
    // at most one length, / a number divisor (a literal 0 is an error); the result is a length.
    std::vector<int> stack;
    std::vector<const CalcOpDecl*> producers;
    for (const CalcOpDecl& op : out) {
        switch (op.op) {
            case CalcOpKind::PushLength:
                stack.push_back(1);
                producers.push_back(&op);
                break;
            case CalcOpKind::PushNumber:
                stack.push_back(0);
                producers.push_back(&op);
                break;
            case CalcOpKind::PushVar:
                stack.push_back(2);
                producers.push_back(&op);
                break;
            default: {
                if (stack.size() < 2 || stack.size() > 32) {
                    error = "malformed calc()";
                    return false;
                }
                const int b = stack.back();
                const CalcOpDecl* bOp = producers.back();
                stack.pop_back();
                producers.pop_back();
                int& a = stack.back();
                producers.back() = nullptr;
                if (op.op == CalcOpKind::Add || op.op == CalcOpKind::Sub) {
                    if (a != 2 && b != 2 && a != b) {
                        error = "calc() cannot add or subtract a number and a length";
                        return false;
                    }
                    a = (a == 2) ? b : a;
                } else if (op.op == CalcOpKind::Min || op.op == CalcOpKind::Max) {
                    if (a != 2 && b != 2 && a != b) {
                        error = "min(), max() and clamp() cannot mix numbers and lengths";
                        return false;
                    }
                    a = (a == 2) ? b : a;
                } else if (op.op == CalcOpKind::Mul) {
                    if (a == 1 && b == 1) {
                        error = "calc() cannot multiply two lengths";
                        return false;
                    }
                    a = (a == 1 || b == 1) ? 1 : ((a == 2 || b == 2) ? 2 : 0);
                } else {
                    if (b == 1) {
                        error = "calc() cannot divide by a length";
                        return false;
                    }
                    if (bOp != nullptr && bOp->op == CalcOpKind::PushNumber && bOp->value == 0.0f) {
                        error = "calc() divides by zero";
                        return false;
                    }
                }
                break;
            }
        }
    }
    if (stack.size() != 1 || stack.back() == 0) {
        error = "calc() must produce a length";
        return false;
    }
    return true;
}

}  // namespace xts_values_detail

}  // namespace xt::compile
