#include "xt/style_template.h"

#include <cstring>
#include <string>
#include <vector>

#include "css_number.h"
#include "mini_css.h"
#include "style_decode.h"
#include "style_template_internal.h"
#include "xt/charconv.h"

/// T-477 (Docs/design/27-interaction.md §27.4.2 item 4): StyleTemplate compilation by probing --
/// the text is completed with known numbers and parsed by the same mini_css::parseDeclarations()
/// setStyle() uses; the declarations every probe agrees on, with each hole's number found
/// unchanged in its fields, are the template. ElementRef::setStyleTemplate() (element_ref.cpp)
/// reads the numbers back out of a completed text (readStyleTemplateNumbers()).

namespace xt {

namespace {

bool isSpaceChar(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
bool isDigitChar(char c) { return c >= '0' && c <= '9'; }
bool isLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

/// The character before a hole cannot continue a token into the number: start of text, a space,
/// `(`, `,`, `:` or `/` (not a digit, `.`, a sign, a letter, `_`, `#`, ...).
bool holeMayFollow(std::string_view before) {
    if (before.empty()) {
        return true;
    }
    const char c = before.back();
    return isSpaceChar(c) || c == '(' || c == ',' || c == ':' || c == '/';
}

/// The character after a hole cannot extend the number: end of text, a space, `)`, `,`, `;`, `!`,
/// `/`, `%`, or a unit letter that is not an exponent (`e` / `E` before a digit or a sign).
bool holeMayPrecede(std::string_view after) {
    if (after.empty()) {
        return true;
    }
    const char c = after.front();
    if (isSpaceChar(c) || c == ')' || c == ',' || c == ';' || c == '!' || c == '/' || c == '%') {
        return true;
    }
    if (!isLetter(c)) {
        return false;  // A digit, `.`, `_`, `-`, `(`, ...
    }
    if ((c == 'e' || c == 'E') && after.size() > 1) {
        const char n = after[1];
        if (isDigitChar(n) || n == '+' || n == '-') {
            return false;
        }
    }
    return true;
}

/// A float field of `v` (see StyleTemplate::Target), or nullptr for one it does not have.
const float* floatField(const Value& v, uint8_t field) {
    switch (v.type) {
        case ValueType::Float:
            return field == 0 ? &v.f : nullptr;
        case ValueType::Length:
            return field == 0 ? &v.len.value : nullptr;
        case ValueType::Vec2:
            return field == 0 ? &v.v2.x : field == 1 ? &v.v2.y : nullptr;
        case ValueType::Vec3:
            return field == 0 ? &v.v3.x : field == 1 ? &v.v3.y : field == 2 ? &v.v3.z : nullptr;
        case ValueType::Color:
            return field == 0   ? &v.c.r
                   : field == 1 ? &v.c.g
                   : field == 2 ? &v.c.b
                   : field == 3 ? &v.c.a
                                : nullptr;
        default:
            return nullptr;
    }
}

bool sameBits(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }

/// A declaration a template may hold: a plain value, nothing for setStyle() to intern or pool.
bool plainDecl(const mini_css::Decl& d) {
    namespace sp = style_decode::styleprop;
    if (d.varRef || !d.text.empty() || !d.effects.empty() || !d.effectStops.empty() ||
        !d.animations.empty() || d.prop == sp::Effect || d.prop == sp::BackgroundGradient ||
        d.prop == sp::ClipPath || d.prop == sp::Animation) {
        return false;
    }
    switch (d.value.type) {
        case ValueType::Float:
        case ValueType::Length:
        case ValueType::Vec2:
        case ValueType::Vec3:
        case ValueType::Color:
        case ValueType::Enum:
            return d.value.floats == nullptr && d.value.stops == nullptr;
        default:
            return false;
    }
}

/// The parts of two probe declarations that must agree whatever the hole numbers are.
bool sameShape(const mini_css::Decl& a, const mini_css::Decl& b) {
    if (a.prop != b.prop || a.important != b.important || a.value.type != b.value.type) {
        return false;
    }
    if (a.value.type == ValueType::Length && a.value.len.unit != b.value.len.unit) {
        return false;
    }
    if (a.value.type == ValueType::Enum && a.value.e != b.value.e) {
        return false;
    }
    return true;
}

/// Parses the template completed with `numbers` (one per hole).
Status probe(std::span<const std::string_view> literals, const float* numbers,
             std::vector<mini_css::Decl>& out) {
    std::string text;
    for (size_t i = 0; i < literals.size(); ++i) {
        text.append(literals[i]);
        if (i + 1 < literals.size()) {
            char buf[32];
            text.append(buf, xt::formatFloat(buf, sizeof(buf), static_cast<double>(numbers[i]), 9));
        }
    }
    out.clear();
    return mini_css::parseDeclarations(text, out);
}

/// True when `decls` has `base`'s shape and every field is either `base`'s constant or, where a
/// hole lands (`targets`), that hole's number in `numbers`.
bool matchesProbe(const std::vector<mini_css::Decl>& base, const std::vector<StyleTemplate::Target>& targets,
                  const std::vector<mini_css::Decl>& decls, const float* numbers) {
    if (decls.size() != base.size()) {
        return false;
    }
    for (size_t d = 0; d < base.size(); ++d) {
        if (!plainDecl(decls[d]) || !sameShape(base[d], decls[d])) {
            return false;
        }
        for (uint8_t f = 0; f < 4; ++f) {
            const float* want = floatField(base[d].value, f);
            const float* got = floatField(decls[d].value, f);
            if (want == nullptr) {
                break;
            }
            bool hole = false;
            for (const StyleTemplate::Target& t : targets) {
                if (t.decl == d && t.field == f) {
                    hole = true;
                    if (!sameBits(*got, numbers[t.hole])) {
                        return false;
                    }
                }
            }
            if (!hole && !sameBits(*want, *got)) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

bool compileStyleTemplate(std::span<const std::string_view> literals, StyleTemplate& out) {
    out = StyleTemplate{};
    if (literals.empty() || literals.size() - 1 > StyleTemplate::kMaxHoles) {
        return false;
    }
    const size_t holes = literals.size() - 1;
    for (size_t h = 0; h < holes; ++h) {
        if (!holeMayFollow(literals[h]) || !holeMayPrecede(literals[h + 1])) {
            return false;  // Also two holes in a row (an empty literal between them).
        }
    }
    if (holes > 0 && literals[0].empty()) {
        return false;  // A declaration starts with its property name, never a number.
    }
    // Two probes with distinct, exactly representable numbers per hole (never equal across the
    // probes, so a field that changes between them is a hole's).
    float a[StyleTemplate::kMaxHoles];
    float b[StyleTemplate::kMaxHoles];
    for (size_t h = 0; h < holes; ++h) {
        a[h] = 1234.5f + 16.0f * static_cast<float>(h);
        b[h] = 0.375f + 2.0f * static_cast<float>(h);
    }
    std::vector<mini_css::Decl> base;
    std::vector<mini_css::Decl> other;
    if (probe(literals, a, base) != Status::kOk || probe(literals, b, other) != Status::kOk || base.empty() ||
        base.size() != other.size() || base.size() > 0xFFFF) {
        return false;
    }
    std::vector<StyleTemplate::Target> targets;
    std::vector<uint8_t> bound(holes, 0);
    for (size_t d = 0; d < base.size(); ++d) {
        if (!plainDecl(base[d]) || !plainDecl(other[d]) || !sameShape(base[d], other[d])) {
            return false;
        }
        for (uint8_t f = 0; f < 4; ++f) {
            const float* fa = floatField(base[d].value, f);
            const float* fb = floatField(other[d].value, f);
            if (fa == nullptr) {
                break;
            }
            if (sameBits(*fa, *fb)) {
                continue;  // A constant of the text.
            }
            size_t hole = holes;
            for (size_t h = 0; h < holes; ++h) {
                if (sameBits(*fa, a[h]) && sameBits(*fb, b[h])) {
                    hole = h;
                    break;
                }
            }
            if (hole == holes) {
                return false;  // The number is converted (rgb() channels, rad, a composed matrix).
            }
            targets.push_back(StyleTemplate::Target{static_cast<uint16_t>(d), f, static_cast<uint8_t>(hole)});
            bound[hole] = 1;
        }
    }
    for (size_t h = 0; h < holes; ++h) {
        if (bound[h] == 0) {
            return false;  // A number that lands nowhere (or only through a conversion).
        }
    }
    if (!matchesProbe(base, targets, other, b)) {
        return false;
    }
    // Negative and zero numbers, one hole at a time: allowed only when the parse keeps its shape.
    std::vector<uint8_t> domain(holes, 0);
    std::vector<mini_css::Decl> variant;
    for (size_t h = 0; h < holes; ++h) {
        float numbers[StyleTemplate::kMaxHoles];
        std::memcpy(numbers, a, sizeof(float) * holes);
        numbers[h] = -a[h];
        if (probe(literals, numbers, variant) == Status::kOk &&
            matchesProbe(base, targets, variant, numbers)) {
            domain[h] |= StyleTemplate::kHoleNegative;
        }
        numbers[h] = 0.0f;
        if (probe(literals, numbers, variant) == Status::kOk &&
            matchesProbe(base, targets, variant, numbers)) {
            domain[h] |= StyleTemplate::kHoleZero;
        }
    }
    for (std::string_view literal : literals) {
        out.literals.emplace_back(literal);
    }
    out.decls.reserve(base.size());
    for (const mini_css::Decl& d : base) {
        out.decls.push_back(StyleTemplate::Decl{d.prop, d.important, d.value});
    }
    out.targets = std::move(targets);
    out.holeDomain = std::move(domain);
    return true;
}

bool readStyleTemplateNumbers(const StyleTemplate& t, std::string_view text, float* numbers) {
    if (!t.valid()) {
        return false;
    }
    const size_t holes = t.holeCount();
    size_t pos = 0;
    for (size_t h = 0; h <= holes; ++h) {
        const std::string& literal = t.literals[h];
        if (text.size() - pos < literal.size() || text.compare(pos, literal.size(), literal) != 0) {
            return false;
        }
        pos += literal.size();
        if (h == holes) {
            break;
        }
        // The number the tokenizer reads here: the longest CSS number (css_number::scan()).
        float v = 0;
        size_t length = 0;
        if (!css_number::read(text.substr(pos), v, length)) {
            return false;
        }
        const uint8_t domain = t.holeDomain[h];
        if ((v < 0.0f && (domain & StyleTemplate::kHoleNegative) == 0) ||
            (v == 0.0f && (domain & StyleTemplate::kHoleZero) == 0)) {
            return false;
        }
        numbers[h] = v;
        pos += length;
    }
    return pos == text.size();
}

void fillStyleTemplateValue(const StyleTemplate& t, size_t decl, const float* numbers, Value& out) {
    out = t.decls[decl].value;
    for (const StyleTemplate::Target& target : t.targets) {
        if (target.decl != decl) {
            continue;
        }
        float* field = const_cast<float*>(floatField(out, target.field));
        if (field != nullptr) {
            *field = numbers[target.hole];
        }
    }
}

}  // namespace xt
