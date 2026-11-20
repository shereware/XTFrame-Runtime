#include "expr_lexer.h"

#include <cstring>

#include "expr.h"
#include "xt/charconv.h"

/// See expr_lexer.h. Docs/design/58-value-functions-and-drivers.md §58.2.

namespace xt::compile::expr {

void addExprDiagnostic(std::vector<Diagnostic>& diags, const char* code, std::string message, int line,
                       int col, std::string fix) {
    diags.push_back(Diagnostic{Diagnostic::Level::Warning, std::string(), line, col, std::move(message),
                               std::string(code), std::move(fix)});
}

namespace {

bool isIdentStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isIdentChar(char c) { return isIdentStart(c) || isDigit(c); }

// Longest first, so `===` wins over `==` and `=`.
constexpr const char* kPuncts[] = {"===", "!==", "**", "=>", "<=", ">=", "==", "!=", "&&", "||", "+=",
                                   "-=",  "*=",  "/=", "%=", "++", "--", "<<", ">>", "+",  "-",  "*",
                                   "/",   "%",   "(",  ")",  "[",  "]",  "{",  "}",  ",",  ";",  ".",
                                   "?",   ":",   "!",  "<",  ">",  "=",  "&",  "|",  "^",  "~"};

class Lexer {
public:
    Lexer(std::string_view text, std::vector<Token>& out, std::vector<Diagnostic>& diags)
        : m_text(text), m_out(out), m_diags(diags) {}

    bool run() {
        bool newline = false;
        while (true) {
            if (!skipSpace(newline)) {
                return false;
            }
            Token tok;
            tok.line = m_line;
            tok.col = col();
            tok.newlineBefore = newline;
            newline = false;
            if (m_pos >= m_text.size()) {
                tok.kind = TokKind::End;
                m_out.push_back(std::move(tok));
                return true;
            }
            const char c = m_text[m_pos];
            if (isDigit(c) || (c == '.' && m_pos + 1 < m_text.size() && isDigit(m_text[m_pos + 1]))) {
                if (!number(tok)) {
                    return false;
                }
            } else if (isIdentStart(c)) {
                const size_t start = m_pos;
                while (m_pos < m_text.size() && isIdentChar(m_text[m_pos])) {
                    ++m_pos;
                }
                tok.kind = TokKind::Ident;
                tok.text.assign(m_text.substr(start, m_pos - start));
            } else if (c == '"' || c == '\'') {
                if (!string(tok, c)) {
                    return false;
                }
            } else if (c == '`') {
                addExprDiagnostic(m_diags, "expr.unsupported", "template strings are not supported", tok.line,
                                  tok.col);
                return false;
            } else if (!punct(tok)) {
                std::string msg = "unexpected character '";
                if (static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7F) {
                    msg += c;
                } else {
                    msg += "?";
                }
                msg += "'";
                addExprDiagnostic(m_diags, "expr.syntax", std::move(msg), tok.line, tok.col);
                return false;
            }
            m_out.push_back(std::move(tok));
        }
    }

private:
    int col() const { return static_cast<int>(m_pos - m_lineStart) + 1; }

    bool skipSpace(bool& newline) {
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if (c == '\n') {
                newline = true;
                ++m_pos;
                ++m_line;
                m_lineStart = m_pos;
            } else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
                ++m_pos;
            } else if (c == '/' && m_pos + 1 < m_text.size() && m_text[m_pos + 1] == '/') {
                while (m_pos < m_text.size() && m_text[m_pos] != '\n') {
                    ++m_pos;
                }
            } else if (c == '/' && m_pos + 1 < m_text.size() && m_text[m_pos + 1] == '*') {
                const int line = m_line;
                const int column = col();
                m_pos += 2;
                bool closed = false;
                while (m_pos < m_text.size()) {
                    if (m_text[m_pos] == '*' && m_pos + 1 < m_text.size() && m_text[m_pos + 1] == '/') {
                        m_pos += 2;
                        closed = true;
                        break;
                    }
                    if (m_text[m_pos] == '\n') {
                        newline = true;
                        ++m_line;
                        m_lineStart = m_pos + 1;
                    }
                    ++m_pos;
                }
                if (!closed) {
                    addExprDiagnostic(m_diags, "expr.syntax", "unterminated comment", line, column);
                    return false;
                }
            } else {
                break;
            }
        }
        return true;
    }

    bool number(Token& tok) {
        const size_t start = m_pos;
        if (m_text[m_pos] == '0' && m_pos + 1 < m_text.size() &&
            (m_text[m_pos + 1] == 'x' || m_text[m_pos + 1] == 'X')) {
            addExprDiagnostic(m_diags, "expr.unsupported", "hexadecimal numbers are not supported", tok.line,
                              tok.col);
            return false;
        }
        while (m_pos < m_text.size() && isDigit(m_text[m_pos])) {
            ++m_pos;
        }
        if (m_pos < m_text.size() && m_text[m_pos] == '.') {
            ++m_pos;
            while (m_pos < m_text.size() && isDigit(m_text[m_pos])) {
                ++m_pos;
            }
        }
        if (m_pos < m_text.size() && (m_text[m_pos] == 'e' || m_text[m_pos] == 'E')) {
            size_t p = m_pos + 1;
            if (p < m_text.size() && (m_text[p] == '+' || m_text[p] == '-')) {
                ++p;
            }
            if (p < m_text.size() && isDigit(m_text[p])) {
                m_pos = p;
                while (m_pos < m_text.size() && isDigit(m_text[m_pos])) {
                    ++m_pos;
                }
            }
        }
        if (m_pos < m_text.size() && isIdentStart(m_text[m_pos])) {
            addExprDiagnostic(m_diags, "expr.syntax", "a number cannot be followed by a name", tok.line,
                              tok.col);
            return false;
        }
        // fromChars rejects a leading '.': feed it a copy with a leading zero.
        std::string digits = "0";
        digits.append(m_text.substr(start, m_pos - start));
        double v = 0;
        const auto res = fromChars(digits.data(), digits.data() + digits.size(), v);
        if (res.ec != std::errc() || res.ptr != digits.data() + digits.size()) {
            addExprDiagnostic(m_diags, "expr.syntax", "malformed number", tok.line, tok.col);
            return false;
        }
        tok.kind = TokKind::Number;
        tok.number = v;
        return true;
    }

    bool string(Token& tok, char quote) {
        ++m_pos;
        std::string s;
        while (true) {
            if (m_pos >= m_text.size() || m_text[m_pos] == '\n') {
                addExprDiagnostic(m_diags, "expr.syntax", "unterminated string", tok.line, tok.col);
                return false;
            }
            const char c = m_text[m_pos++];
            if (c == quote) {
                break;
            }
            if (c == '\\' && m_pos < m_text.size()) {
                const char e = m_text[m_pos++];
                switch (e) {
                    case 'n':
                        s += '\n';
                        break;
                    case 't':
                        s += '\t';
                        break;
                    default:
                        s += e;
                        break;
                }
                continue;
            }
            s += c;
        }
        tok.kind = TokKind::String;
        tok.text = std::move(s);
        return true;
    }

    bool punct(Token& tok) {
        for (const char* p : kPuncts) {
            const size_t n = std::strlen(p);
            if (m_text.substr(m_pos, n) == std::string_view(p, n)) {
                tok.kind = TokKind::Punct;
                tok.text.assign(p, n);
                m_pos += n;
                return true;
            }
        }
        return false;
    }

    std::string_view m_text;
    std::vector<Token>& m_out;
    std::vector<Diagnostic>& m_diags;
    size_t m_pos = 0;
    size_t m_lineStart = 0;
    int m_line = 1;
};

}  // namespace

bool lexExpression(std::string_view text, std::vector<Token>& out, std::vector<Diagnostic>& diags) {
    if (text.size() > kExprMaxTextBytes) {
        addExprDiagnostic(diags, "expr.limit", "the expression is longer than 64 KB", 1, 1);
        return false;
    }
    Lexer lexer(text, out, diags);
    return lexer.run();
}

}  // namespace xt::compile::expr
