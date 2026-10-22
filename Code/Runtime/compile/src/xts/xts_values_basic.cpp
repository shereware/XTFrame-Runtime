#include <string>
#include <vector>

#include "css_values.h"
#include "xt/compile/style_model.h"
#include "xts_lexer.h"
#include "xts_values_internal.h"

/// The bridge between the `.xts` lexer and core's value grammar (T-413; split from xts_values.cpp by
/// T-412, where this file held the token cursor, lengths, colours and easings that now live in
/// Code/Runtime/core/src/css_values.cpp).

namespace xt::compile::xts_values_detail {

// The lexer's TokenKind and css_values::TokKind list the same kinds in the same order (css_values
// adds Slash at the end, which the lexer never produces).
static_assert(static_cast<int>(TokenKind::End) == static_cast<int>(TokKind::End));
static_assert(static_cast<int>(TokenKind::Unknown) == static_cast<int>(TokKind::Unknown));
static_assert(static_cast<int>(TokenKind::Number) == static_cast<int>(TokKind::Number));
static_assert(static_cast<int>(TokenKind::Comma) == static_cast<int>(TokKind::Comma));
static_assert(static_cast<int>(TokenKind::LParen) == static_cast<int>(TokKind::LParen));
static_assert(static_cast<int>(TokenKind::Dollar) == static_cast<int>(TokKind::Dollar));
static_assert(static_cast<int>(TokKind::Slash) == static_cast<int>(TokenKind::Dollar) + 1);

namespace {

/// T-421 (05 §5.3, ruling 2026-10-21): units are ASCII case-insensitive, so a known unit in any
/// case reads as its lower-case spelling (a view of the literal); any other unit is kept as written.
std::string_view canonicalUnit(std::string_view unit) {
    static constexpr std::string_view kUnits[] = {
        "px",          "em",        "rem",     "vw",  "vh",  "vmin", "vmax", "safe-top", "safe-right",
        "safe-bottom", "safe-left", "uiscale", "deg", "rad", "turn", "s",    "ms",       "fr"};
    for (std::string_view known : kUnits) {
        if (css_values::iequals(unit, known)) {
            return known;
        }
    }
    return unit;
}

}  // namespace

std::vector<Tok> toValueTokens(const std::vector<Token>& toks) {
    std::vector<Tok> out;
    out.reserve(toks.size());
    for (const Token& t : toks) {
        Tok v;
        v.kind = static_cast<TokKind>(static_cast<uint8_t>(t.kind));
        v.text = t.text;
        v.num = t.num;
        v.unit = canonicalUnit(t.unit);
        out.push_back(v);
    }
    return out;
}

Token toLexerToken(const Tok& t) {
    Token out;
    out.kind = static_cast<TokenKind>(static_cast<uint8_t>(t.kind));
    out.text = std::string(t.text);
    out.num = t.num;
    out.unit = std::string(t.unit);
    return out;
}

void appendPropValues(const std::vector<css_values::PropValue>& values, int line,
                      std::vector<StyleDecl>& out) {
    for (const css_values::PropValue& v : values) {
        StyleDecl d;
        d.prop = v.prop;
        switch (v.value.type) {
            case ValueType::Length:
                d.value.kind = StyleValueKind::Length;
                d.value.len = v.value.len;
                break;
            case ValueType::Color:
                d.value.kind = StyleValueKind::Color;
                d.value.color = v.value.c;
                break;
            case ValueType::Enum:
                d.value.kind = StyleValueKind::Enum;
                d.value.e = v.value.e;
                break;
            default:
                d.value.kind = StyleValueKind::Float;
                d.value.f = v.value.f;
                break;
        }
        d.line = line;
        out.push_back(d);
    }
}

}  // namespace xt::compile::xts_values_detail
