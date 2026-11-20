#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xt/compile/diagnostics.h"

/// Tokens of a timeline expression (Docs/design/58-value-functions-and-drivers.md §58.2): numbers,
/// strings, identifiers and JavaScript punctuators; `//` and `/* */` comments are skipped.

namespace xt::compile::expr {

/// Token kinds.
enum class TokKind : uint8_t { End, Number, String, Ident, Punct };

/// One token. `text` is the identifier, the punctuator or the decoded string.
struct Token {
    TokKind kind = TokKind::End;
    std::string text;
    double number = 0;
    int line = 1;
    int col = 1;
    bool newlineBefore = false;  ///< A line break separates it from the previous token (for ASI).
};

/// Splits `text` into tokens (the last is End). On an error it appends a diagnostic and returns
/// false.
bool lexExpression(std::string_view text, std::vector<Token>& out, std::vector<Diagnostic>& diags);

/// Appends an `expr.*` warning-level diagnostic (58 §58.4: expression problems are warnings).
void addExprDiagnostic(std::vector<Diagnostic>& diags, const char* code, std::string message, int line,
                       int col, std::string fix = {});

}  // namespace xt::compile::expr
