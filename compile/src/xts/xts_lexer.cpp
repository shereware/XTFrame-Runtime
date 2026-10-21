#include "xts_lexer.h"

#include <cctype>

#include "css_number.h"  // core/src: the CSS number shared with the runtime (T-272).

namespace xt::compile {

namespace {

bool isIdentStart(char c) { return c == '-' || c == '_' || std::isalpha(static_cast<unsigned char>(c)); }
bool isIdentCont(char c) { return c == '-' || c == '_' || std::isalnum(static_cast<unsigned char>(c)); }
bool isDigitCh(char c) { return std::isdigit(static_cast<unsigned char>(c)); }

}  // namespace

Lexer::Lexer(std::string_view text) : m_text(text) {}

bool Lexer::atEnd() const { return m_pos >= m_text.size(); }

char Lexer::peek(size_t ahead) const {
    size_t i = m_pos + ahead;
    return i < m_text.size() ? m_text[i] : '\0';
}

char Lexer::advance() {
    char c = m_text[m_pos++];
    if (c == '\n') {
        ++m_line;
        m_col = 1;
    } else {
        ++m_col;
    }
    return c;
}

Token Lexer::makeToken(TokenKind kind, int line, int col) const {
    Token t;
    t.kind = kind;
    t.line = line;
    t.col = col;
    t.wsBefore = m_wsBefore;
    return t;
}

void Lexer::skipWhitespaceAndComments() {
    for (;;) {
        while (!atEnd() && std::isspace(static_cast<unsigned char>(peek()))) {
            advance();
        }
        if (peek() == '/' && peek(1) == '*') {
            advance();
            advance();
            while (!atEnd() && !(peek() == '*' && peek(1) == '/')) {
                advance();
            }
            if (!atEnd()) {
                advance();
                advance();
            }
            continue;
        }
        break;
    }
}

Token Lexer::next() {
    const size_t before = m_pos;
    skipWhitespaceAndComments();
    m_wsBefore = m_pos != before;
    int line = m_line;
    int col = m_col;
    if (atEnd()) {
        return makeToken(TokenKind::End, line, col);
    }
    char c = peek();
    if (c == '"' || c == '\'') {
        return lexString(c);
    }
    if (isDigitCh(c) || (c == '.' && isDigitCh(peek(1))) ||
        ((c == '-' || c == '+') && (isDigitCh(peek(1)) || (peek(1) == '.' && isDigitCh(peek(2)))))) {
        return lexNumber();
    }
    if (isIdentStart(c)) {
        return lexIdent();
    }
    if (c == '#') {
        return lexHash();
    }
    switch (c) {
        case '{':
            advance();
            return makeToken(TokenKind::LBrace, line, col);
        case '}':
            advance();
            return makeToken(TokenKind::RBrace, line, col);
        case ':':
            advance();
            return makeToken(TokenKind::Colon, line, col);
        case ';':
            advance();
            return makeToken(TokenKind::Semicolon, line, col);
        case ',':
            advance();
            return makeToken(TokenKind::Comma, line, col);
        case '>':
            advance();
            return makeToken(TokenKind::Gt, line, col);
        case '.':
            advance();
            return makeToken(TokenKind::Dot, line, col);
        case '*':
            advance();
            return makeToken(TokenKind::Star, line, col);
        case '(':
            advance();
            return makeToken(TokenKind::LParen, line, col);
        case ')':
            advance();
            return makeToken(TokenKind::RParen, line, col);
        case '!':
            advance();
            return makeToken(TokenKind::Bang, line, col);
        case '@':
            advance();
            return makeToken(TokenKind::At, line, col);
        case '[':
            advance();
            return makeToken(TokenKind::LBracket, line, col);
        case ']':
            advance();
            return makeToken(TokenKind::RBracket, line, col);
        case '=':
            advance();
            return makeToken(TokenKind::Equals, line, col);
        case '^':
            advance();
            return makeToken(TokenKind::Caret, line, col);
        case '$':
            advance();
            return makeToken(TokenKind::Dollar, line, col);
        default:
            break;
    }
    advance();
    Token t = makeToken(TokenKind::Unknown, line, col);
    t.text = std::string("unexpected character '") + c + "'";
    return t;
}

Token Lexer::lexString(char quote) {
    int line = m_line;
    int col = m_col;
    advance();  // opening quote.
    std::string out;
    while (!atEnd() && peek() != quote) {
        char c = advance();
        if (c == '\\' && !atEnd() && (peek() == '"' || peek() == '\'' || peek() == '\\')) {
            out.push_back(advance());
        } else {
            out.push_back(c);
        }
    }
    if (atEnd()) {
        Token t = makeToken(TokenKind::Unknown, line, col);
        t.text = "unterminated string";
        return t;
    }
    advance();  // closing quote.
    Token t = makeToken(TokenKind::String, line, col);
    t.text = std::move(out);
    return t;
}

Token Lexer::lexNumber() {
    int line = m_line;
    int col = m_col;
    // T-272: CSS Syntax 3's number, exponent included (`1e3`, `1.5E-2`); an `e` with no digit after
    // it still starts the unit (`1em`, `2ex`).
    const size_t length = css_number::scan(m_text.substr(m_pos));
    std::string digits;
    for (size_t i = 0; i < length; ++i) {
        digits.push_back(advance());
    }
    std::string unit;
    if (peek() == '%') {
        unit.push_back(advance());
    } else if (isIdentStart(peek())) {
        while (isIdentCont(peek())) {
            unit.push_back(advance());
        }
    }
    float num = 0;
    if (!css_number::convert(digits, num)) {
        // Beyond float (`1e999`): a lexical error, so the declaration is an invalid value.
        Token t = makeToken(TokenKind::Unknown, line, col);
        t.text = "invalid number '" + digits + unit + "'";
        return t;
    }
    Token t = makeToken(TokenKind::Number, line, col);
    t.num = num;
    t.text = std::move(digits);
    t.unit = std::move(unit);
    return t;
}

Token Lexer::lexIdent() {
    int line = m_line;
    int col = m_col;
    std::string out;
    while (isIdentCont(peek())) {
        out.push_back(advance());
    }
    Token t = makeToken(TokenKind::Ident, line, col);
    t.text = std::move(out);
    return t;
}

Token Lexer::lexHash() {
    int line = m_line;
    int col = m_col;
    advance();  // '#'.
    std::string out;
    // T-201: a path id (`#Play/Box`, `#Item/3/Cell`, 12.7 / 27.3.4) keeps its '/' separators; a '/'
    // only joins when an id character follows it (a color never contains one).
    while (isIdentCont(peek()) || (peek() == '/' && !out.empty() && isIdentCont(peek(1)))) {
        out.push_back(advance());
    }
    // T-218 (31 §31.5.1): `#Icon/*` (every node of an inline <svg>) keeps its `/*` -- unless a `*/`
    // closes it before the next `{`, `;` or `}`, in which case it is an ordinary comment.
    if (!out.empty() && peek() == '/' && peek(1) == '*') {
        bool comment = false;
        for (size_t i = m_pos + 2; i < m_text.size(); ++i) {
            const char c = m_text[i];
            if (c == '{' || c == ';' || c == '}') {
                break;
            }
            if (c == '*' && i + 1 < m_text.size() && m_text[i + 1] == '/') {
                comment = true;
                break;
            }
        }
        if (!comment) {
            out.push_back(advance());
            out.push_back(advance());
        }
    }
    Token t = makeToken(TokenKind::Hash, line, col);
    t.text = std::move(out);
    return t;
}

}  // namespace xt::compile
