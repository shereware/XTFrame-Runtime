#pragma once

#include <vector>

#include "expr_ast.h"
#include "expr_lexer.h"
#include "xt/compile/diagnostics.h"

/// The parser and the name resolver behind parseExpression() (expr.h). Internal to
/// Code/Runtime/compile/src/expr. Docs/design/58-value-functions-and-drivers.md §58.2.

namespace xt::compile::expr {

/// The deepest syntax tree accepted (statements and expressions together); the evaluator's
/// recursion is bounded by it (14 §14.9 rule 2).
inline constexpr int kMaxTreeDepth = 200;
/// The most nested parentheses / statements the recursive descent follows.
inline constexpr int kMaxParseDepth = 64;
/// The most elements an array literal has.
inline constexpr int kMaxArrayLength = 4;

/// Builds `program` from `tokens` (from lexExpression). Names stay unresolved: Var and LocalCall
/// nodes hold the name's string index in `slot`. Returns false after a diagnostic.
bool parseTokens(const std::vector<Token>& tokens, Program& program, std::vector<Diagnostic>& diags);

/// Resolves every name of a parsed `program` to a global slot, a function local or a local function,
/// checks where strings may appear, and rejects recursion. Returns false after a diagnostic.
bool resolveProgram(Program& program, std::vector<Diagnostic>& diags);

/// True when `name` is a built-in name that cannot be declared or assigned (time, value, Math...).
bool isReservedName(const std::string& name);

}  // namespace xt::compile::expr
