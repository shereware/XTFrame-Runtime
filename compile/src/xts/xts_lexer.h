#pragma once

#include <cstdint>
#include <string>
#include <string_view>

/// Hand-written `.xts` tokenizer. See docs/design/05-style-and-css.md §5.3, docs/tasks/T-22.md.
///
/// `@` is not part of the symbol set literally listed in T-22 ("`{ } : ; , > . * ( ) !`"), but
/// at-rules (`@keyframes`, `@font-face`) require it; it is lexed as its own single-char token
/// (kAt) as the minimal necessary addition (see the T-22 report).
///
/// A "function" token (an ident directly followed by `(`) is not a distinct lexer token kind:
/// the lexer just emits Ident then LParen back to back with no whitespace consumed between them,
/// and the parser/value-parser tells the two cases apart by checking whether the next token is
/// LParen right after an Ident.

namespace xt::compile {

enum class TokenKind : uint8_t {
    End,
    Unknown,  // Lexer error (unterminated string/comment, stray character).
    Ident,
    Number,  // num + unit ("" = unitless, "%" = percent, else px/em/deg/s/ms/... ident).
    String,
    Hash,  // '#' + text = the hex digits that followed.
    LBrace,
    RBrace,
    Colon,
    Semicolon,
    Comma,
    Gt,
    Dot,
    Star,
    LParen,
    RParen,
    Bang,
    At,
    // T-173 (05.9.2): attribute selectors `[name]`, `[name=v]`, `[name^=v]`, `[name$=v]`,
    // `[name*=v]` (`*` is the existing Star).
    LBracket,
    RBracket,
    Equals,
    Caret,
    Dollar,
};

struct Token {
    TokenKind kind = TokenKind::End;
    std::string text;  // Ident name / string contents / hash hex digits / Unknown's message /
                       // Number's source digits including any sign (T-173's `an+b`).
    float num = 0;     // Number only.
    std::string unit;  // Number only.
    int line = 1;
    int col = 1;
    /// T-173: true when whitespace (or a comment) separated this token from the previous one; the
    /// selector grammar reads it as the descendant combinator.
    bool wsBefore = false;
};

/// Tokenizes `text` one token at a time via next(). Comments and whitespace are skipped
/// transparently. Line/col are 1-based and relative to the start of `text` (the caller adds its
/// own line offset for diagnostics).
class Lexer {
public:
    explicit Lexer(std::string_view text);

    Token next();

private:
    void skipWhitespaceAndComments();
    Token lexString(char quote);
    Token lexNumber();
    Token lexIdent();
    Token lexHash();
    char peek(size_t ahead = 0) const;
    char advance();
    bool atEnd() const;
    Token makeToken(TokenKind kind, int line, int col) const;

    std::string_view m_text;
    size_t m_pos = 0;
    int m_line = 1;
    int m_col = 1;
    bool m_wsBefore = false;  // T-173: whether next() skipped anything before the current token.
};

}  // namespace xt::compile
