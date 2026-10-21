#include "bind_emit.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
#include <utility>

#include "css_number.h"  // core/src: the CSS number (T-272).
#include "xt/compile/aliases.h"
#include "xt/compile/parse_xt.h"
#include "xt/compile/validate.h"
#include "xt/ir_pod.h"
#include "xt/timeline.h"
#include "xt/util/hex.h"
#include "xts/xts_props.h"

/// docs/design/27-interaction.md §27.3, 03 §3.12.3, 05 §5.9.1, 12 §12.7, docs/tasks/T-175.md.
///
/// Not covered by the design/task sheet (conservative choices made here):
///   - Port names follow the id rules (`[A-Za-z_][A-Za-z0-9_]*`); struct fields take the scalar
///     types only (no nested struct / list / enum); `enum(a|b)` and `enum(<a|b>)` both parse.
///   - Scopes: a port declared inside a symbol instance belongs to that instance (the innermost
///     instance root strictly above the declaring element); a port declared on the `<use>` itself
///     (its root) belongs to the enclosing scope. A plain name resolves in the element's own
///     scope, then outwards, then at document level; `<id>.<port>` names a port declared on
///     element `<id>` (path ids included) and `<instance id>.<port>` an exposed port of that
///     instance. `expose="false"` (or `expose:false`) on the declaring element or on the instance
///     root hides the port from the `<instance id>.<port>` form.
///   - A `bind:` source that is no declared port is an error (the contract of 05 §5.9.3); so is a
///     percentage in a comparison or a range() input without a `<port>_max` companion.
///   - `format(pattern, a, b, ...)` as the whole expression: `a` is the chain's input (`{0}`), the
///     rest are arguments `{1}`...; `select(<input>, k: v, ...)` / `step(<input>, t: v, ...)` take
///     their input as the first positional argument; after a `|` they take none.
///   - Host converters (13.8.1 registerConverter) are written `host.<name>(args)`; the compiler
///     cannot know them, so the `host.` prefix is what keeps an unknown name an error.
///   - `variant.<Prop>` must be on a symbol instance root; its values are not checked against the
///     symbol's `variants` list (the runtime only swaps the `v-<Prop>-*` class).
///   - T-177: inside a `<repeat>` template, `item(<field>)`, `index` and `count` name the repeat
///     entry (a head, a format()/select()/step() input, or a converter argument) before any port
///     of the same name; `item(<field>)` must name a field of the list's entries. `if` conditions
///     of triggers do not read them.
///   - T-380 (27 §27.3.4): `bind:<name>` on a symbol instance root targets the in-port `<name>`
///     declared directly in that instance's scope (exposed or not) before any property of that
///     name; a port of a nested instance is not reachable this way. An out / call / event port is
///     an error.

namespace xt::compile {

// Implemented in xts/xts_parser.cpp (T-172): a free-standing easing value.
bool parseEasingText(std::string_view text, Easing& out, std::vector<EasingPoint>* easingPoints = nullptr);

namespace {

std::string trimCopyBindEmit(std::string_view s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return std::string(s.substr(b, e - b));
}

bool isIdent(std::string_view s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) {
        return false;
    }
    for (char c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) {
            return false;
        }
    }
    return true;
}

/// Splits `text` at every top-level `sep` (outside quotes and (), {}, <>).
std::vector<std::string> splitTop(std::string_view text, char sep) {
    std::vector<std::string> out;
    int depth = 0;
    char quote = 0;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (quote != 0) {
            if (c == quote) {
                quote = 0;
            }
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '(' || c == '{' || c == '<') {
            ++depth;
        } else if (c == ')' || c == '}' || c == '>') {
            --depth;
        } else if (c == sep && depth == 0) {
            out.push_back(std::string(text.substr(start, i - start)));
            start = i + 1;
        }
    }
    out.push_back(std::string(text.substr(start)));
    return out;
}

/// Index of the first top-level `c` (outside quotes and brackets), or npos.
size_t findTop(std::string_view text, char c) {
    int depth = 0;
    char quote = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        char ch = text[i];
        if (quote != 0) {
            if (ch == quote) {
                quote = 0;
            }
            continue;
        }
        if (ch == '\'' || ch == '"') {
            quote = ch;
        } else if (ch == '(' || ch == '{' || ch == '<') {
            ++depth;
        } else if (ch == ')' || ch == '}' || ch == '>') {
            --depth;
        } else if (ch == c && depth == 0) {
            return i;
        }
    }
    return std::string_view::npos;
}

bool parseNumber(std::string_view text, float& out) {
    float v = 0;
    size_t length = 0;
    if (!css_number::read(text, v, length)) {  // T-272: CSS number, exponent included.
        return false;
    }
    std::string_view unit = text.substr(length);
    if (!unit.empty() && unit != "px") {
        return false;
    }
    out = v;
    return true;
}

/// `<n>`, `<n>s` or `<n>ms` -> seconds.
bool parseSeconds(std::string_view text, float& out) {
    float v = 0;
    size_t length = 0;
    if (!css_number::read(text, v, length)) {
        return false;
    }
    std::string_view unit = text.substr(length);
    if (unit.empty() || unit == "s") {
        out = v;
    } else if (unit == "ms") {
        out = v / 1000.0f;
    } else {
        return false;
    }
    return out >= 0.0f;
}

bool isQuoted(std::string_view s) {
    return s.size() >= 2 && (s.front() == '\'' || s.front() == '"') && s.back() == s.front();
}

// -- Port types -------------------------------------------------------------------------------

bool scalarType(std::string_view word, PortValueType& out) {
    struct Entry {
        const char* name;
        PortValueType type;
    };
    static constexpr Entry kScalars[] = {
        {"number", PortValueType::Number}, {"string", PortValueType::String},
        {"bool", PortValueType::Bool},     {"color", PortValueType::Color},
        {"vec2", PortValueType::Vec2},     {"vec3", PortValueType::Vec3},
        {"entity", PortValueType::Entity}, {"texture", PortValueType::Texture},
        {"event", PortValueType::Event},
    };
    for (const Entry& e : kScalars) {
        if (word == e.name) {
            out = e.type;
            return true;
        }
    }
    return false;
}

struct ParsedPortType {
    PortValueType type = PortValueType::Number;
    std::vector<std::pair<std::string, PortValueType>> fields;  // Enum values carry type 0.
    // T-381: List only: 0 for `list<struct{...}>`, else the primitive entry type (IrPort::entryType;
    // an Enum entry's value names are `fields`).
    uint8_t entryType = 0;
    // T-358: `= <literal>`. `defaultValue` is IrPortDefault::value except for String / Enum, whose
    // StringId the caller interns from `defaultText` (the unquoted string / the value name).
    bool hasDefault = false;
    uint8_t defaultValue[16] = {};
    std::string defaultText;
    // T-391 (27 §27.3.4): parallel to `fields` of a struct / list<struct>: a field of type List
    // has its entry type here (type List, entryType, fields, subs); other fields a default value.
    std::vector<ParsedPortType> subs;
};

bool parsePortType(std::string_view text, ParsedPortType& out, std::string& err);

/// `struct{a: number, b: string}` body (without the braces).
bool parseStructBody(std::string_view body, ParsedPortType& out, std::string& err) {
    for (const std::string& piece : splitTop(body, ',')) {
        std::string field = trimCopyBindEmit(piece);
        if (field.empty()) {
            continue;
        }
        size_t colon = field.find(':');
        std::string name =
            colon == std::string::npos ? std::string() : trimCopyBindEmit(field.substr(0, colon));
        std::string type =
            colon == std::string::npos ? std::string() : trimCopyBindEmit(field.substr(colon + 1));
        PortValueType t{};
        ParsedPortType sub;
        if (isIdent(name) && type.rfind("list", 0) == 0 && findTop(type, '=') == std::string::npos) {
            // T-391 (27 §27.3.4): a field may be a list<T> itself (T a struct or a base type).
            std::string subErr;
            if (!parsePortType(type, sub, subErr) || sub.type != PortValueType::List) {
                err = "invalid struct field \"" + field +
                      "\": " + (subErr.empty() ? std::string("expected list<...>") : subErr);
                return false;
            }
            t = PortValueType::List;
        } else if (!isIdent(name) || !scalarType(type, t) || t == PortValueType::Event) {
            err = "invalid struct field \"" + field + "\" (expected <name>: <scalar type> or list<...>)";
            return false;
        }
        for (const auto& f : out.fields) {
            if (f.first == name) {
                err = "struct field \"" + name + "\" declared twice";
                return false;
            }
        }
        out.fields.emplace_back(name, t);
        out.subs.push_back(std::move(sub));
    }
    if (out.fields.empty()) {
        err = "a struct needs at least one field";
        return false;
    }
    return true;
}

/// A whole CSS number (no unit), e.g. a port default.
bool parsePlainNumber(std::string_view text, float& out) {
    float v = 0;
    size_t length = 0;
    if (text.empty() || !css_number::read(text, v, length) || length != text.size() || !std::isfinite(v)) {
        return false;
    }
    out = v;
    return true;
}

using util::hexDigit;

/// T-358: `#rgb`, `#rgba`, `#rrggbb`, `#rrggbbaa`, `rgb(r, g, b)` or `rgba(r, g, b, a)` (0-255
/// channels, alpha 0-1) -> RGBA8 bytes.
bool parseDefaultColor(std::string_view text, uint8_t rgba[4]) {
    if (!text.empty() && text.front() == '#') {
        std::string_view hex = text.substr(1);
        for (char c : hex) {
            if (hexDigit(c) < 0) {
                return false;
            }
        }
        rgba[3] = 255;
        if (hex.size() == 3 || hex.size() == 4) {
            for (size_t i = 0; i < hex.size(); ++i) {
                rgba[i] = static_cast<uint8_t>(hexDigit(hex[i]) * 17);
            }
            return true;
        }
        if (hex.size() == 6 || hex.size() == 8) {
            for (size_t i = 0; i < hex.size() / 2; ++i) {
                rgba[i] = static_cast<uint8_t>(hexDigit(hex[2 * i]) * 16 + hexDigit(hex[2 * i + 1]));
            }
            return true;
        }
        return false;
    }
    const bool hasAlpha = text.rfind("rgba(", 0) == 0;
    const size_t open = hasAlpha ? 4 : text.rfind("rgb(", 0) == 0 ? 3 : std::string_view::npos;
    if (open == std::string_view::npos || text.back() != ')') {
        return false;
    }
    std::vector<std::string> parts = splitTop(text.substr(open + 1, text.size() - open - 2), ',');
    if (parts.size() != (hasAlpha ? 4u : 3u)) {
        return false;
    }
    rgba[3] = 255;
    for (size_t i = 0; i < parts.size(); ++i) {
        float v = 0;
        if (!parsePlainNumber(trimCopyBindEmit(parts[i]), v)) {
            return false;
        }
        const float scaled = i == 3 ? v * 255.0f : v;
        rgba[i] = static_cast<uint8_t>(std::lround(std::clamp(scaled, 0.0f, 255.0f)));
    }
    return true;
}

/// T-383: `text` split at its top-level commas, quotes (with backslash escapes) and (), [], {}
/// nesting respected. False on unbalanced brackets or an open quote.
bool splitJsonItems(std::string_view text, std::vector<std::string>& out) {
    int depth = 0;
    char quote = 0;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quote != 0) {
            if (c == '\\') {
                ++i;
            } else if (c == quote) {
                quote = 0;
            }
            continue;
        }
        if (c == '"') {
            quote = c;
        } else if (c == '[' || c == '{' || c == '(') {
            ++depth;
        } else if (c == ']' || c == '}' || c == ')') {
            if (--depth < 0) {
                return false;
            }
        } else if (c == ',' && depth == 0) {
            out.push_back(trimCopyBindEmit(text.substr(start, i - start)));
            start = i + 1;
        }
    }
    if (depth != 0 || quote != 0) {
        return false;
    }
    std::string last = trimCopyBindEmit(text.substr(start));
    if (!last.empty() || !out.empty()) {
        out.push_back(std::move(last));
    }
    return true;
}

/// T-383 (27 §27.3.4): one entry of a list port's JSON-array default, of entry type `t` (0 =
/// struct, a JSON object); `enumValues` names an Enum entry's values.
bool listDefaultEntryOk(const std::string& item, uint8_t t,
                        const std::vector<std::pair<std::string, PortValueType>>& enumValues) {
    const bool quoted = item.size() >= 2 && item.front() == '"' && item.back() == '"';
    float f = 0;
    switch (static_cast<PortValueType>(t)) {
        case PortValueType::Number:
            return parsePlainNumber(item, f);
        case PortValueType::Bool:
            return item == "true" || item == "false" || item == "0" || item == "1";
        case PortValueType::String:
            return quoted || parsePlainNumber(item, f) || item == "true" || item == "false";
        case PortValueType::Color: {
            uint8_t rgba[4] = {};
            return quoted && parseDefaultColor(std::string_view(item).substr(1, item.size() - 2), rgba);
        }
        case PortValueType::Vec2:
        case PortValueType::Vec3: {
            std::vector<std::string> parts;
            if (item.size() < 2 || item.front() != '[' || item.back() != ']' ||
                !splitJsonItems(std::string_view(item).substr(1, item.size() - 2), parts) ||
                parts.size() != (static_cast<PortValueType>(t) == PortValueType::Vec2 ? 2u : 3u)) {
                return false;
            }
            for (const std::string& p : parts) {
                if (!parsePlainNumber(p, f)) {
                    return false;
                }
            }
            return true;
        }
        case PortValueType::Enum:
            if (quoted) {
                const std::string name = item.substr(1, item.size() - 2);
                for (const auto& v : enumValues) {
                    if (v.first == name) {
                        return true;
                    }
                }
                return false;
            }
            return parsePlainNumber(item, f) && f >= 0 && f == std::floor(f) &&
                   f < static_cast<float>(enumValues.size());
        default:  // A struct entry: a JSON object (fields by name; checked at load).
            return item.size() >= 2 && item.front() == '{' && item.back() == '}';
    }
}

/// T-358 (27 §27.3.1): the `= <literal>` default of a port of type `out.type`.
bool parsePortDefault(std::string_view literal, ParsedPortType& out, std::string& err) {
    std::string lit = trimCopyBindEmit(literal);
    out.hasDefault = true;
    auto bad = [&](const char* expected) {
        err = "default \"" + lit + "\" is not " + expected;
        return false;
    };
    auto putFloats = [&](const float* v, size_t n) { std::memcpy(out.defaultValue, v, n * sizeof(float)); };
    switch (out.type) {
        case PortValueType::Number: {
            float v = 0;
            if (!parsePlainNumber(lit, v)) {
                return bad("a number");
            }
            putFloats(&v, 1);
            return true;
        }
        case PortValueType::Bool:
            if (lit != "true" && lit != "false") {
                return bad("true or false");
            }
            out.defaultValue[0] = lit == "true" ? 1 : 0;
            return true;
        case PortValueType::Color:
            if (!parseDefaultColor(lit, out.defaultValue)) {
                return bad("a color (#rrggbb[aa] or rgb())");
            }
            return true;
        case PortValueType::String:
            if (!isQuoted(lit)) {
                return bad("a quoted string");
            }
            out.defaultText = lit.substr(1, lit.size() - 2);
            return true;
        case PortValueType::Vec2:
        case PortValueType::Vec3: {
            const size_t want = out.type == PortValueType::Vec2 ? 2 : 3;
            std::vector<std::string> words;
            size_t i = 0;
            while (i < lit.size()) {
                while (i < lit.size() && std::isspace(static_cast<unsigned char>(lit[i])) != 0) {
                    ++i;
                }
                size_t start = i;
                while (i < lit.size() && std::isspace(static_cast<unsigned char>(lit[i])) == 0) {
                    ++i;
                }
                if (i > start) {
                    words.push_back(lit.substr(start, i - start));
                }
            }
            float v[3] = {0, 0, 0};
            bool okNumbers = words.size() == want;
            for (size_t k = 0; okNumbers && k < want; ++k) {
                okNumbers = parsePlainNumber(words[k], v[k]);
            }
            if (!okNumbers) {
                return bad(want == 2 ? "two numbers" : "three numbers");
            }
            putFloats(v, want);
            return true;
        }
        case PortValueType::Enum:
            for (const auto& f : out.fields) {
                if (f.first == lit) {
                    out.defaultText = lit;
                    return true;
                }
            }
            return bad("one of the enum values");
        case PortValueType::List: {
            // T-383 (27 §27.3.4): a JSON array, as Document::setPortText() takes it; the text is
            // stored as is (IrPortDefault: StringId) and parsed at load.
            std::vector<std::string> items;
            if (lit.size() < 2 || lit.front() != '[' || lit.back() != ']' ||
                !splitJsonItems(std::string_view(lit).substr(1, lit.size() - 2), items)) {
                return bad("a JSON array ([...])");
            }
            for (const std::string& item : items) {
                if (!listDefaultEntryOk(item, out.entryType, out.fields)) {
                    err = "default \"" + lit + "\": entry " + item + " is not " +
                          (out.entryType == 0 ? "a JSON object" : "a value of the list's entry type");
                    return false;
                }
            }
            out.defaultText = lit;
            return true;
        }
        default:
            err = "default is not allowed for this port type";
            return false;
    }
}

bool parsePortType(std::string_view text, ParsedPortType& out, std::string& err) {
    std::string t = trimCopyBindEmit(text);
    // T-358: `<type> = <literal>`.
    if (size_t eq = findTop(t, '='); eq != std::string::npos) {
        std::string literal = trimCopyBindEmit(std::string_view(t).substr(eq + 1));
        if (!parsePortType(std::string_view(t).substr(0, eq), out, err)) {
            return false;
        }
        if (literal.empty()) {
            err = "default is empty";
            return false;
        }
        return parsePortDefault(literal, out, err);
    }
    if (t.empty()) {
        err = "unknown port type \"\"";
        return false;
    }
    if (scalarType(t, out.type)) {
        return true;
    }
    if (t.rfind("enum(", 0) == 0 && t.back() == ')') {
        std::string inner = trimCopyBindEmit(std::string_view(t).substr(5, t.size() - 6));
        if (inner.size() >= 2 && inner.front() == '<' && inner.back() == '>') {
            inner = inner.substr(1, inner.size() - 2);
        }
        out.type = PortValueType::Enum;
        for (const std::string& piece : splitTop(inner, '|')) {
            std::string v = trimCopyBindEmit(piece);
            if (!isIdent(v)) {
                err = "invalid enum value \"" + v + "\"";
                return false;
            }
            for (const auto& f : out.fields) {
                if (f.first == v) {
                    err = "enum value \"" + v + "\" listed twice";
                    return false;
                }
            }
            out.fields.emplace_back(v, static_cast<PortValueType>(0));
        }
        return true;
    }
    if (t.rfind("struct", 0) == 0) {
        std::string rest = trimCopyBindEmit(std::string_view(t).substr(6));
        if (rest.size() < 2 || rest.front() != '{' || rest.back() != '}') {
            err = "expected struct{<field>: <type>, ...}";
            return false;
        }
        out.type = PortValueType::Struct;
        return parseStructBody(std::string_view(rest).substr(1, rest.size() - 2), out, err);
    }
    if (t.rfind("list", 0) == 0) {
        std::string rest = trimCopyBindEmit(std::string_view(t).substr(4));
        if (rest.size() < 2 || rest.front() != '<' || rest.back() != '>') {
            err = "expected list<struct{...}> or list<number|string|bool|color|vec2|vec3|enum(...)>";
            return false;
        }
        std::string inner = trimCopyBindEmit(std::string_view(rest).substr(1, rest.size() - 2));
        ParsedPortType entry;
        // T-381 (27 §27.3.4): a struct or a primitive entry type (no default inside the brackets).
        if (findTop(inner, '=') != std::string::npos || !parsePortType(inner, entry, err)) {
            if (err.empty()) {
                err = "a list's entry type has no default";
            }
            return false;
        }
        const PortValueType e = entry.type;
        if (e != PortValueType::Struct && e != PortValueType::Number && e != PortValueType::String &&
            e != PortValueType::Bool && e != PortValueType::Color && e != PortValueType::Vec2 &&
            e != PortValueType::Vec3 && e != PortValueType::Enum) {
            err =
                "a list's entries must be a struct{...} or one of number, string, bool, color, vec2, "
                "vec3, enum(...), got \"" +
                inner + "\"";
            return false;
        }
        out.type = PortValueType::List;
        out.entryType = e == PortValueType::Struct ? 0 : static_cast<uint8_t>(e);
        out.fields = std::move(entry.fields);
        out.subs = std::move(entry.subs);  // T-391
        return true;
    }
    err = "unknown port type \"" + t + "\"";
    return false;
}

// -- Binding expressions ------------------------------------------------------------------------

struct Call {
    std::string name;
    std::vector<std::string> args;  // Raw argument text, trimmed.
    bool hasParens = false;
};

bool parseCallText(std::string_view text, Call& out, std::string& err) {
    std::string s = trimCopyBindEmit(text);
    size_t open = s.find('(');
    if (open == std::string::npos) {
        out.name = s;
        return !out.name.empty();
    }
    if (s.back() != ')') {
        err = "expected ')' at the end of \"" + s + "\"";
        return false;
    }
    out.name = trimCopyBindEmit(std::string_view(s).substr(0, open));
    out.hasParens = true;
    std::string inner = s.substr(open + 1, s.size() - open - 2);
    if (!trimCopyBindEmit(inner).empty()) {
        for (const std::string& a : splitTop(inner, ',')) {
            out.args.push_back(trimCopyBindEmit(a));
        }
    }
    return !out.name.empty();
}

/// One argument value, as it goes into IrConverterStep::args.
struct ArgVal {
    ConverterArg kind = ConverterArg::Number;
    float f = 0;
    uint32_t u = 0;
};

enum ArgAllow : uint32_t {
    kAllowNumber = 1,
    kAllowPercent = 2,
    kAllowPort = 4,
    kAllowString = 8,
    kAllowAny = 15,
};

void appendSteps(std::vector<IrConverterStep>& steps, ConverterOp op, const std::vector<ArgVal>& args) {
    size_t i = 0;
    bool first = true;
    do {
        IrConverterStep step{};
        step.op = static_cast<uint8_t>(first ? op : ConverterOp::Operands);
        step.argCount = first ? static_cast<uint8_t>(args.size()) : 0;
        for (uint32_t k = 0; k < 4 && i < args.size(); ++k, ++i) {
            const ArgVal& a = args[i];
            step.argKinds =
                static_cast<uint16_t>(step.argKinds | (static_cast<uint16_t>(a.kind) << (4u * k)));
            if (a.kind == ConverterArg::String || a.kind == ConverterArg::Port ||
                a.kind == ConverterArg::Item || a.kind == ConverterArg::OuterItem) {
                std::memcpy(&step.args[k], &a.u, sizeof(float));
            } else {
                step.args[k] = a.f;
            }
        }
        steps.push_back(step);
        first = false;
    } while (i < args.size());
}

/// T-201: `bind:translate-x|translate-y|scale-x|scale-y|rotate` -> the transform component PropId
/// (translate in px, scale as a factor, rotate in degrees).
bool transformComponent(std::string_view name, uint16_t& out) {
    struct Entry {
        const char* name;
        uint16_t prop;
    };
    static constexpr Entry kComponents[] = {
        {"translate-x", xt::prop::TranslateX}, {"translate-y", xt::prop::TranslateY},
        {"scale-x", xt::prop::ScaleX},         {"scale-y", xt::prop::ScaleY},
        {"rotate", xt::prop::Rotation},
    };
    for (const Entry& e : kComponents) {
        if (name == e.name) {
            out = e.prop;
            return true;
        }
    }
    return false;
}

class Binder {
public:
    Binder(const DomTree& dom, const std::vector<ExpandedSymbolInstance>& instances, const PortScopes& scopes,
           const CustomStateBuilder& states, IrWriter& writer, std::string_view fileName,
           DiagnosticSink& sink, const RepeatScopes* repeats)
        : m_dom(dom),
          m_scopes(scopes),
          m_repeats(repeats),
          m_states(states),
          m_writer(writer),
          m_fileName(fileName),
          m_sink(sink) {
        for (const ExpandedSymbolInstance& inst : instances) {
            m_instanceRoots.insert(inst.instanceNode);
        }
    }

    /// T-201: see compileConverterChain().
    bool chain(uint32_t element, std::string_view text, std::vector<IrConverterStep>& steps,
               std::string& err) {
        m_element = element;
        m_primary = -1;
        m_deps.clear();
        m_afterLayout = false;
        m_repeatItem = false;
        m_listValue = -1;
        m_needField = false;
        for (const std::string& segment : splitTop(text, '|')) {
            Call c;
            if (!parseCallText(segment, c, err) || !converter(c, steps, err)) {
                if (err.empty()) {
                    err = "invalid converter \"" + trimCopyBindEmit(segment) + "\"";
                }
                return false;
            }
            if (c.name == "tween" || c.name == "delay") {
                err = c.name + "() cannot be used in set()";
                return false;
            }
        }
        return checkNeedField(err);
    }

    /// T-411: see compileActionValue().
    bool actionValue(uint32_t element, std::string_view text, ActionValue& out, std::string& err) {
        m_element = element;
        m_primary = -1;
        m_deps.clear();
        m_afterLayout = false;
        m_repeatItem = false;
        m_listValue = -1;
        m_needField = false;
        out = ActionValue{};
        const std::vector<std::string> segments = splitTop(text, '|');
        m_portCalls = true;
        bool ok = !segments.empty() && actionHead(trimCopyBindEmit(segments[0]), out, err);
        for (size_t i = 1; ok && i < segments.size(); ++i) {
            Call c;
            if (!parseCallText(segments[i], c, err) || !converter(c, out.steps, err)) {
                if (err.empty()) {
                    err = "invalid converter \"" + trimCopyBindEmit(segments[i]) + "\"";
                }
                ok = false;
            } else if (c.name == "tween" || c.name == "delay") {
                err = c.name + "() cannot be used in set()";
                ok = false;
            }
        }
        m_portCalls = false;
        return ok && checkNeedField(err);
    }

    bool run(BindingBuilder& out, const std::map<uint32_t, std::vector<SvgBindAttr>>* svgBinds) {
        for (uint32_t element = 0; element < m_dom.nodes.size(); ++element) {
            for (const DomAttr& attr : m_dom.nodes[element].attrs) {
                if (attr.name == "world-anchor") {
                    compileWorldAnchor(element, attr, out);
                    continue;
                }
                if (attr.name.size() <= 5 || attr.name.compare(0, 5, "bind:") != 0) {
                    continue;
                }
                compileBinding(element, attr, out);
            }
            // T-243 (32 §32.4): the element's inline <svg> content.
            if (svgBinds == nullptr) {
                continue;
            }
            auto it = svgBinds->find(element);
            if (it == svgBinds->end()) {
                continue;
            }
            for (const SvgBindAttr& sb : it->second) {
                DomAttr attr;
                attr.name = sb.name;
                attr.value = sb.value;
                attr.line = sb.line;
                if (sb.name == "world-anchor") {
                    compileWorldAnchor(element, attr, out, &sb);
                } else if (sb.name.size() > 5) {
                    compileBinding(element, attr, out, &sb);
                }
            }
        }
        return m_ok;
    }

private:
    const DomTree& m_dom;
    const PortScopes& m_scopes;
    const RepeatScopes* m_repeats;
    const CustomStateBuilder& m_states;
    IrWriter& m_writer;
    std::string m_fileName;
    DiagnosticSink& m_sink;
    std::set<uint32_t> m_instanceRoots;
    bool m_ok = true;

    // Per binding being compiled.
    uint32_t m_element = 0;
    int m_primary = -1;
    std::vector<uint32_t> m_deps;
    bool m_afterLayout = false;
    bool m_repeatItem = false;  // T-177: the chain starts at the repeat entry.
    // T-381: the chain's current value is a whole list port (only right after a list port head),
    // and the PORT index of that port; `m_needField`: an `item(<n>)` of a list<struct> is pending
    // an `item(<field>)`.
    int m_listValue = -1;
    bool m_needField = false;
    // T-411: compiling a trigger action value: a converter argument may be `port(<name>)`.
    bool m_portCalls = false;

    /// T-411: `port(<name>)` -> its PORT index (-1 = not that form; -2 = an error, `err` set).
    /// Call ports and event ports carry no value to read.
    int portCall(const std::string& text, std::string& err) const {
        Call c;
        std::string ignored;
        if (text.rfind("port", 0) != 0 || !parseCallText(text, c, ignored) || c.name != "port" ||
            !c.hasParens) {
            return -1;
        }
        if (c.args.size() != 1 || c.args[0].empty()) {
            err = "port() takes one port name";
            return -2;
        }
        const int port = m_scopes.resolve(c.args[0], m_element);
        if (port < 0) {
            err = "port(" + c.args[0] + "): no port \"" + c.args[0] + "\" is declared here";
            return -2;
        }
        const IrPort& decl = m_scopes.ports[static_cast<size_t>(port)];
        if (static_cast<PortDir>(decl.dir) == PortDir::Call ||
            static_cast<PortValueType>(decl.type) == PortValueType::Event) {
            err = "port(" + c.args[0] + "): a call or event port has no value to read";
            return -2;
        }
        return port;
    }

    /// T-411: the head of a trigger action value (compileActionValue()).
    bool actionHead(const std::string& text, ActionValue& out, std::string& err) {
        if (text.empty()) {
            err = "empty value";
            return false;
        }
        const int port = portCall(text, err);
        if (port == -2) {
            return false;
        }
        if (port >= 0) {
            out.head = ActionValue::Head::Port;
            out.port = port;
            m_primary = port;
            addDep(static_cast<uint32_t>(port));
            if (static_cast<PortValueType>(m_scopes.ports[static_cast<size_t>(port)].type) ==
                PortValueType::List) {
                m_listValue = port;  // `port(<list>) | item(<n>)`, as T-381.
            }
            return true;
        }
        if (text.rfind("outer.", 0) == 0) {
            err = text + " cannot be read by a trigger action (only item(), repeat.index and repeat.count)";
            return false;
        }
        if (text.rfind("item(", 0) == 0 || text.rfind("repeat.", 0) == 0) {
            ArgVal term;
            const int t = repeatTerm(text, term, err);
            if (t < 0) {
                return false;
            }
            if (t == 0) {
                err = text.rfind("item(", 0) == 0 ? text + " is only valid inside a <repeat> template"
                                                  : "unknown value \"" + text + "\"";
                return false;
            }
            out.head = ActionValue::Head::Item;
            repeatHead(term, out.steps);
            return true;
        }
        if (isQuoted(text)) {
            out.head = ActionValue::Head::String;
            out.text = text.substr(1, text.size() - 2);
            return true;
        }
        if (text == "true" || text == "false") {
            out.number = text == "true" ? 1.0f : 0.0f;
            return true;
        }
        if (parseNumber(text, out.number)) {
            return true;
        }
        err =
            "a value starts with port(<name>), item(<field>), item(), repeat.index, repeat.count, a number, "
            "true, false or a quoted string, got \"" +
            text + "\"";
        return false;
    }

    /// T-380: the PORT index of the port `name` declared directly in the scope of symbol instance
    /// root `element`, or -1 (not an instance root, or no such port).
    int instancePortOf(uint32_t element, std::string_view name) const {
        if (m_instanceRoots.count(element) == 0 || !isIdent(name)) {
            return -1;
        }
        for (uint32_t i = 0; i < m_scopes.ports.size(); ++i) {
            if (m_scopes.ports[i].scope == element && m_scopes.names[i] == name) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    void addDep(uint32_t port) {
        if (std::find(m_deps.begin(), m_deps.end(), port) == m_deps.end()) {
            m_deps.push_back(port);
        }
    }

    /// T-383 (27 §27.3.3): whether `name` is an in port declared in the scope seen from the
    /// current element (a declared port wins over a reserved name).
    bool declaredInPort(std::string_view name) const {
        const int port = m_scopes.resolve(name, m_element);
        return port >= 0 &&
               static_cast<PortDir>(m_scopes.ports[static_cast<size_t>(port)].dir) == PortDir::In;
    }

    /// T-177: inside a `<repeat>` template, `item(<field>)`, `index` or `count` -> `out` (kind
    /// Item / Index / Count). 0 = not a repeat term, 1 = one, -1 = an error (`err` set).
    /// T-383 (27 §27.3.3): a bare `index` / `count` naming a declared in port is the port (0);
    /// the explicit `repeat.index` / `repeat.count` / `repeat.item(...)` always name the repeat
    /// and are an error outside a template.
    int repeatTerm(const std::string& text, ArgVal& out, std::string& err) {
        if (text.rfind("outer.", 0) == 0) {
            return outerTerm(text, out, err);  // T-391
        }
        const bool explicitForm = text.rfind("repeat.", 0) == 0;
        const std::string raw = explicitForm ? text.substr(7) : text;
        const bool termWord = raw == "index" || raw == "count" || raw.rfind("item(", 0) == 0;
        if (explicitForm && !termWord) {
            return 0;  // `repeat.<other>`: a qualified name (an element id "repeat").
        }
        const uint32_t r = m_repeats != nullptr ? m_repeats->repeatAt(m_element) : RepeatScopes::kNoRepeat;
        if (r == RepeatScopes::kNoRepeat) {
            if (explicitForm) {
                err = text + " is only valid inside a <repeat> template";
                return -1;
            }
            return 0;
        }
        if (raw == "index" || raw == "count") {
            if (!explicitForm && declaredInPort(raw)) {
                return 0;
            }
            out.kind = raw == "index" ? ConverterArg::Index : ConverterArg::Count;
            return 1;
        }
        Call c;
        std::string ignored;
        if (raw.rfind("item", 0) != 0 || !parseCallText(raw, c, ignored) || c.name != "item" ||
            !c.hasParens) {
            return 0;
        }
        // T-381 (27 §27.3.4): `item()` is the whole entry of a list<number|string|...>; a
        // list<struct{...}> entry needs a field name. T-391: the list may be a field of the
        // enclosing repeat's entry.
        const bool resolved = m_repeats->resolved(r);
        const uint8_t entryType = resolved ? m_repeats->entryType[r] : uint8_t{0};
        if (c.args.empty()) {
            if (resolved && entryType == 0) {
                err = "item() needs a field name: the list entries are structs (item(<field>))";
                return -1;
            }
            out.kind = ConverterArg::Item;
            out.u = m_writer.intern("");
            return 1;
        }
        if (c.args.size() != 1) {
            err = "item() takes one field name";
            return -1;
        }
        const std::string field = isQuoted(c.args[0]) ? c.args[0].substr(1, c.args[0].size() - 2) : c.args[0];
        if (entryType != 0) {
            err = "item(" + field + "): the list entries are not structs; use item() for the whole entry";
            return -1;
        }
        const std::vector<std::string>& fields = m_repeats->fields[r];
        if (resolved && std::find(fields.begin(), fields.end(), field) == fields.end()) {
            err = "item(" + field + "): the list entries have no field \"" + field + "\"";
            return -1;
        }
        out.kind = ConverterArg::Item;
        out.u = m_writer.intern(field);
        return 1;
    }

    /// T-391 (27 §27.3.4): `outer.index` / `outer.item(<field>)` inside a nested repeat's template:
    /// the enclosing repeat's entry index / entry field (one level out). 0 = not an outer term
    /// (`outer.<other>`: a qualified name), 1 = one, -1 = an error (`err` set).
    int outerTerm(const std::string& text, ArgVal& out, std::string& err) {
        const std::string raw = text.substr(6);
        Call c;
        std::string ignored;
        const bool item =
            raw.rfind("item", 0) == 0 && parseCallText(raw, c, ignored) && c.name == "item" && c.hasParens;
        if (raw != "index" && !item) {
            return 0;
        }
        const uint32_t r = m_repeats != nullptr ? m_repeats->repeatAt(m_element) : RepeatScopes::kNoRepeat;
        const uint32_t parent = r != RepeatScopes::kNoRepeat && r < m_repeats->parent.size()
                                    ? m_repeats->parent[r]
                                    : RepeatScopes::kNoRepeat;
        if (parent == RepeatScopes::kNoRepeat) {
            err = text + " is only valid inside a <repeat> template nested in another <repeat> template";
            return -1;
        }
        if (!item) {
            out.kind = ConverterArg::OuterIndex;
            return 1;
        }
        if (c.args.size() != 1) {
            err = "outer.item() takes one field name";
            return -1;
        }
        const std::string field = isQuoted(c.args[0]) ? c.args[0].substr(1, c.args[0].size() - 2) : c.args[0];
        if (m_repeats->resolved(parent)) {
            const std::vector<std::string>& fields = m_repeats->fields[parent];
            if (m_repeats->entryType[parent] != 0 ||
                std::find(fields.begin(), fields.end(), field) == fields.end()) {
                err =
                    "outer.item(" + field + "): the enclosing list's entries have no field \"" + field + "\"";
                return -1;
            }
        }
        out.kind = ConverterArg::OuterItem;
        out.u = m_writer.intern(field);
        return 1;
    }

    /// T-177: a repeat term as the chain's head: the step that reads it from the entry.
    void repeatHead(const ArgVal& term, std::vector<IrConverterStep>& steps) {
        m_repeatItem = true;
        if (term.kind == ConverterArg::Item) {
            ArgVal field;
            field.kind = ConverterArg::String;
            field.u = term.u;
            appendSteps(steps, ConverterOp::Item, {field});
        } else if (term.kind == ConverterArg::OuterItem || term.kind == ConverterArg::OuterIndex) {
            // T-391: `item` / `index` with the outer term as its one argument yields that value.
            appendSteps(steps, term.kind == ConverterArg::OuterItem ? ConverterOp::Item : ConverterOp::Index,
                        {term});
        } else {
            appendSteps(steps, term.kind == ConverterArg::Index ? ConverterOp::Index : ConverterOp::Count,
                        {});
        }
    }

    /// Parses one argument. `err` is set on failure.
    bool arg(const std::string& raw, uint32_t allow, ArgVal& out, std::string& err) {
        if (raw.empty()) {
            err = "empty argument";
            return false;
        }
        if (m_portCalls && (allow & kAllowPort) != 0) {
            const int port = portCall(raw, err);  // T-411: `port(<name>)` in a trigger value.
            if (port == -2) {
                return false;
            }
            if (port >= 0) {
                out.kind = ConverterArg::Port;
                out.u = static_cast<uint32_t>(port);
                addDep(out.u);
                return true;
            }
        }
        if ((allow & (kAllowPort | kAllowString)) != 0) {
            const int term = repeatTerm(raw, out, err);
            if (term != 0) {
                return term > 0;
            }
        }
        if (isQuoted(raw)) {
            if ((allow & kAllowString) == 0) {
                err = "unexpected string " + raw;
                return false;
            }
            out.kind = ConverterArg::String;
            out.u = m_writer.intern(std::string_view(raw).substr(1, raw.size() - 2));
            return true;
        }
        if (raw.back() == '%') {
            float v = 0;
            if (parseNumber(std::string_view(raw).substr(0, raw.size() - 1), v)) {
                if ((allow & kAllowPercent) == 0) {
                    err = "a percentage is not allowed here (" + raw + ")";
                    return false;
                }
                out.kind = ConverterArg::Percent;
                out.f = v;
                return true;
            }
        }
        float v = 0;
        if (parseNumber(raw, v)) {
            if ((allow & kAllowNumber) == 0) {
                err = "a number is not allowed here (" + raw + ")";
                return false;
            }
            out.kind = ConverterArg::Number;
            out.f = v;
            return true;
        }
        if ((allow & kAllowPort) != 0) {
            int port = m_scopes.resolve(raw, m_element);
            if (port >= 0) {
                out.kind = ConverterArg::Port;
                out.u = static_cast<uint32_t>(port);
                addDep(out.u);
                return true;
            }
        }
        if ((allow & kAllowString) == 0) {
            err = "\"" + raw + "\" is neither a number nor a declared port";
            return false;
        }
        out.kind = ConverterArg::String;
        out.u = m_writer.intern(raw);
        return true;
    }

    /// A `%` argument of a comparison / range() input needs the input port's `_max` companion.
    bool checkPercent(const ArgVal& a, std::string& err) {
        if (a.kind != ConverterArg::Percent) {
            return true;
        }
        if (m_primary < 0 || m_scopes.maxCompanion(static_cast<uint32_t>(m_primary)) < 0) {
            err =
                "a percentage compares against the input port's \"<port>_max\" companion, and there is none";
            return false;
        }
        return true;
    }

    /// Named `key: value` argument split; `key` is empty for a positional argument.
    static void splitNamed(const std::string& raw, std::string& key, std::string& value) {
        size_t colon = findTop(raw, ':');
        if (colon == std::string::npos) {
            key.clear();
            value = raw;
            return;
        }
        key = trimCopyBindEmit(std::string_view(raw).substr(0, colon));
        value = trimCopyBindEmit(std::string_view(raw).substr(colon + 1));
        if (isQuoted(key)) {
            key = key.substr(1, key.size() - 2);
        }
    }

    /// select / step arguments (`named` = every argument after the positional input, if any) ->
    /// [default, k1, v1, ...].
    bool selectArgs(const Call& c, size_t firstNamed, bool numericKeys, std::vector<ArgVal>& out,
                    std::string& err) {
        ArgVal def;
        def.kind = ConverterArg::None;
        std::vector<ArgVal> pairs;
        for (size_t i = firstNamed; i < c.args.size(); ++i) {
            std::string key, value;
            splitNamed(c.args[i], key, value);
            if (key.empty()) {
                err = c.name + "() arguments must be <key>: <value>";
                return false;
            }
            ArgVal v;
            if (!arg(value, kAllowAny, v, err)) {
                return false;
            }
            if (key == "default") {
                def = v;
                continue;
            }
            ArgVal k;
            if (numericKeys) {
                if (!parseNumber(key, k.f)) {
                    err = "step() thresholds must be numbers, got \"" + key + "\"";
                    return false;
                }
                k.kind = ConverterArg::Number;
                if (!pairs.empty() && k.f < pairs[pairs.size() - 2].f) {
                    err = "step() thresholds must ascend";
                    return false;
                }
            } else {
                float n = 0;
                if (parseNumber(key, n)) {
                    k.kind = ConverterArg::Number;
                    k.f = n;
                } else {
                    k.kind = ConverterArg::String;
                    k.u = m_writer.intern(key);
                }
            }
            pairs.push_back(k);
            pairs.push_back(v);
        }
        if (pairs.empty()) {
            err = c.name + "() needs at least one <key>: <value> pair";
            return false;
        }
        out.push_back(def);
        out.insert(out.end(), pairs.begin(), pairs.end());
        return true;
    }

    /// T-381: an `item(<n>)` of a list<struct{...}> must be followed by `item(<field>)`.
    bool checkNeedField(std::string& err) {
        if (m_needField) {
            err = "item(<n>) of a list<struct{...}> port yields a struct; follow it with item(<field>)";
            return false;
        }
        return true;
    }

    /// One `| conv(args)` step.
    bool converter(const Call& c, std::vector<IrConverterStep>& steps, std::string& err) {
        // T-381: whether this step reads a whole list port / must be item(<field>).
        const int listValue = m_listValue;
        const bool needField = m_needField;
        m_listValue = -1;
        m_needField = false;
        float index = 0;
        if (c.name == "item" && c.args.size() == 1 && parseNumber(c.args[0], index)) {
            // T-381 (27 §27.3.4): `item(<n>)`, a constant entry index into the list port.
            if (index < 0 || index != std::floor(index) || index > 16777216.0f) {
                err = "item(<n>) needs a non-negative integer index, got \"" + c.args[0] + "\"";
                return false;
            }
            if (listValue < 0) {
                err = "item(<n>) reads an entry of a list port; write <list port> | item(<n>)";
                return false;
            }
            ArgVal a;
            a.kind = ConverterArg::Number;
            a.f = index;
            appendSteps(steps, ConverterOp::Item, {a});
            m_needField = m_scopes.ports[static_cast<size_t>(listValue)].entryType == 0;
            return true;
        }
        if (needField && c.name != "item") {
            return checkNeedField(err);
        }
        if (c.name == "item" && listValue >= 0) {
            err = "item(<field>) needs a struct; read a list entry first with item(<n>)";
            return false;
        }
        auto count = [&](size_t lo, size_t hi) {
            if (c.args.size() < lo || c.args.size() > hi) {
                err = c.name + "() takes " +
                      (lo == hi ? std::to_string(lo) : std::to_string(lo) + ".." + std::to_string(hi)) +
                      " argument(s)";
                return false;
            }
            return true;
        };
        auto numeric = [&](ConverterOp op, size_t lo, size_t hi, uint32_t allow, bool percentIsRelative) {
            if (!count(lo, hi)) {
                return false;
            }
            std::vector<ArgVal> args;
            for (size_t i = 0; i < c.args.size(); ++i) {
                ArgVal a;
                if (!arg(c.args[i], allow, a, err)) {
                    return false;
                }
                // range(): only its two inputs compare against `_max`; its outputs are lengths.
                const bool relative = percentIsRelative && !(op == ConverterOp::Range && i >= 2);
                if (relative && !checkPercent(a, err)) {
                    return false;
                }
                args.push_back(a);
            }
            appendSteps(steps, op, args);
            return true;
        };
        const std::string& n = c.name;
        const uint32_t numPort = kAllowNumber | kAllowPort;
        const uint32_t numPctPort = kAllowNumber | kAllowPercent | kAllowPort;
        if (n == "range") {
            return numeric(ConverterOp::Range, 4, 4, numPctPort, true);
        }
        if (n == "clamp") {
            return numeric(ConverterOp::Clamp, 2, 2, numPort, false);
        }
        if (n == "round") {
            return numeric(ConverterOp::Round, 0, 1, kAllowNumber, false);
        }
        if (n == "abs") {
            return numeric(ConverterOp::Abs, 0, 0, 0, false);
        }
        if (n == "scale") {
            return numeric(ConverterOp::Scale, 1, 1, numPort, false);
        }
        if (n == "offset") {
            return numeric(ConverterOp::Offset, 1, 1, numPort, false);
        }
        struct Cmp {
            const char* name;
            ConverterOp op;
            size_t argc;
        };
        static constexpr Cmp kCmp[] = {
            {"lt", ConverterOp::Lt, 1},           {"le", ConverterOp::Le, 1}, {"gt", ConverterOp::Gt, 1},
            {"ge", ConverterOp::Ge, 1},           {"eq", ConverterOp::Eq, 1}, {"ne", ConverterOp::Ne, 1},
            {"between", ConverterOp::Between, 2},
        };
        for (const Cmp& cmp : kCmp) {
            if (n == cmp.name) {
                // eq / ne may compare against a word (an enum value or a string).
                const uint32_t allow =
                    cmp.op == ConverterOp::Eq || cmp.op == ConverterOp::Ne ? kAllowAny : numPctPort;
                return numeric(cmp.op, cmp.argc, cmp.argc, allow, true);
            }
        }
        if (n == "not") {
            return numeric(ConverterOp::Not, 0, 0, 0, false);
        }
        if (n == "and" || n == "or") {
            return numeric(n == "and" ? ConverterOp::And : ConverterOp::Or, 1, 1, numPort, false);
        }
        if (n == "format" || n == "concat") {
            if (!count(1, 200)) {
                return false;
            }
            std::vector<ArgVal> args;
            for (size_t i = 0; i < c.args.size(); ++i) {
                ArgVal a;
                if (n == "format" && i == 0 && !isQuoted(c.args[0])) {
                    err = "format() needs a quoted pattern first";
                    return false;
                }
                if (!arg(c.args[i], kAllowAny, a, err)) {
                    return false;
                }
                args.push_back(a);
            }
            appendSteps(steps, n == "format" ? ConverterOp::Format : ConverterOp::Concat, args);
            return true;
        }
        if (n == "thousands" || n == "upper" || n == "lower" || n == "localize" || n == "count" ||
            n == "index") {
            if (!count(0, 0)) {
                return false;
            }
            ConverterOp op = n == "thousands"  ? ConverterOp::Thousands
                             : n == "upper"    ? ConverterOp::Upper
                             : n == "lower"    ? ConverterOp::Lower
                             : n == "localize" ? ConverterOp::Localize
                             : n == "count"    ? ConverterOp::Count
                                               : ConverterOp::Index;
            appendSteps(steps, op, {});
            return true;
        }
        if (n == "fixed") {
            return numeric(ConverterOp::Fixed, 1, 1, kAllowNumber, false);
        }
        if (n == "percent") {
            return numeric(ConverterOp::Percent, 0, 1, kAllowNumber, false);
        }
        if (n == "time") {
            if (!count(0, 1)) {
                return false;
            }
            std::vector<ArgVal> args;
            if (!c.args.empty()) {
                std::string pattern =
                    isQuoted(c.args[0]) ? c.args[0].substr(1, c.args[0].size() - 2) : c.args[0];
                for (char ch : pattern) {
                    if (ch != 'h' && ch != 'm' && ch != 's' && ch != ':' && ch != '.' && ch != ' ') {
                        err = "time() pattern may only use h, m, s, ':' and '.', got \"" + pattern + "\"";
                        return false;
                    }
                }
                ArgVal a;
                a.kind = ConverterArg::String;
                a.u = m_writer.intern(pattern);
                args.push_back(a);
            }
            appendSteps(steps, ConverterOp::Time, args);
            return true;
        }
        if (n == "select" || n == "step") {
            std::vector<ArgVal> args;
            if (!selectArgs(c, 0, n == "step", args, err)) {
                return false;
            }
            appendSteps(steps, n == "select" ? ConverterOp::Select : ConverterOp::Step, args);
            return true;
        }
        if (n == "lerpColor") {
            if (!count(2, 2)) {
                return false;
            }
            std::vector<ArgVal> args;
            for (const std::string& raw : c.args) {
                ArgVal a;
                if (!arg(raw, kAllowPort | kAllowString, a, err)) {
                    return false;
                }
                args.push_back(a);
            }
            appendSteps(steps, ConverterOp::LerpColor, args);
            return true;
        }
        if (n == "fromToken" || n == "item") {
            if (!count(1, 1)) {
                return false;
            }
            std::string raw = isQuoted(c.args[0]) ? c.args[0].substr(1, c.args[0].size() - 2) : c.args[0];
            if (n == "fromToken" && raw.rfind("--", 0) == 0) {
                raw = raw.substr(2);
            }
            if (raw.empty()) {
                err = n + "() needs a name";
                return false;
            }
            ArgVal a;
            a.kind = ConverterArg::String;
            a.u = m_writer.intern(raw);
            appendSteps(steps, n == "fromToken" ? ConverterOp::FromToken : ConverterOp::Item, {a});
            return true;
        }
        if (n == "glyph") {
            if (!count(0, 1)) {
                return false;
            }
            std::vector<ArgVal> args;
            if (!c.args.empty()) {
                ArgVal a;
                if (!arg(c.args[0], kAllowPort | kAllowString, a, err)) {
                    return false;
                }
                args.push_back(a);
            }
            appendSteps(steps, ConverterOp::Glyph, args);
            return true;
        }
        if (n == "tween") {
            if (!count(1, 2)) {
                return false;
            }
            ArgVal d;
            if (!parseSeconds(c.args[0], d.f)) {
                err = "tween() needs a duration (seconds or ms), got \"" + c.args[0] + "\"";
                return false;
            }
            // Arguments: [seconds, EasingKind, x1 | stepCount, y1 | jumpEnd, x2, y2] (linear default).
            Easing e = Easing::linear();
            if (c.args.size() == 2) {
                std::string easing =
                    isQuoted(c.args[1]) ? c.args[1].substr(1, c.args[1].size() - 2) : c.args[1];
                if (!parseEasingText(easing, e)) {
                    err = "tween(): unknown easing \"" + easing + "\"";
                    return false;
                }
            }
            auto num = [](float v) {
                ArgVal a;
                a.f = v;
                return a;
            };
            std::vector<ArgVal> args{d, num(static_cast<float>(e.kind))};
            if (e.kind == EasingKind::Steps) {
                args.push_back(num(static_cast<float>(e.stepCount)));
                args.push_back(num(static_cast<float>(e.stepsJumpEnd)));
                args.push_back(num(0));
                args.push_back(num(0));
            } else {
                args.push_back(num(e.x1));
                args.push_back(num(e.y1));
                args.push_back(num(e.x2));
                args.push_back(num(e.y2));
            }
            appendSteps(steps, ConverterOp::Tween, args);
            return true;
        }
        if (n == "delay") {
            if (!count(1, 1)) {
                return false;
            }
            ArgVal d;
            if (!parseSeconds(c.args[0], d.f)) {
                err = "delay() needs a duration (seconds or ms), got \"" + c.args[0] + "\"";
                return false;
            }
            appendSteps(steps, ConverterOp::Delay, {d});
            return true;
        }
        if (n == "x" || n == "y" || n == "z" || n == "w") {
            if (!count(0, 0)) {
                return false;
            }
            appendSteps(steps,
                        n == "x"   ? ConverterOp::X
                        : n == "y" ? ConverterOp::Y
                        : n == "z" ? ConverterOp::Z
                                   : ConverterOp::W,
                        {});
            return true;
        }
        if (n == "vec2" || n == "vec3") {
            // T-200: `vec2(y)` / `vec3(y, z)` put the input first; `vec2(a, b)` / `vec3(a, b, c)`
            // take every component from the arguments.
            const size_t components = n == "vec2" ? 2 : 3;
            if (!count(components - 1, components)) {
                return false;
            }
            std::vector<ArgVal> args;
            if (c.args.size() < components) {
                ArgVal input;
                input.kind = ConverterArg::None;
                args.push_back(input);
            }
            for (const std::string& raw : c.args) {
                ArgVal a;
                if (!arg(raw, numPort, a, err)) {
                    return false;
                }
                args.push_back(a);
            }
            appendSteps(steps, ConverterOp::Vec, args);
            return true;
        }
        if (n.rfind("host.", 0) == 0 && isIdent(std::string_view(n).substr(5))) {
            std::vector<ArgVal> args;
            ArgVal name;
            name.kind = ConverterArg::String;
            name.u = m_writer.intern(std::string_view(n).substr(5));
            args.push_back(name);
            for (const std::string& raw : c.args) {
                ArgVal a;
                if (!arg(raw, kAllowAny, a, err)) {
                    return false;
                }
                args.push_back(a);
            }
            appendSteps(steps, ConverterOp::Custom, args);
            return true;
        }
        // T-408 (43 §43.6): a registered converter alias compiles as its replacement.
        if (const NameAlias* alias = findNameAlias(AliasKind::Converter, n)) {
            m_sink.deprecated(m_fileName, deprecatedMessage(*alias));
            Call renamed = c;
            renamed.name = alias->replacement;
            return converter(renamed, steps, err);
        }
        err = "unknown converter \"" + n + "\"";
        return false;
    }

    /// `#Other.<prop>`: a step that replaces the value with another element's property.
    bool elemProp(const std::string& head, std::vector<IrConverterStep>& steps, std::string& err) {
        size_t dot = head.find('.', 1);
        std::string id = dot == std::string::npos ? std::string() : head.substr(1, dot - 1);
        std::string prop = dot == std::string::npos ? std::string() : head.substr(dot + 1);
        bool found = false;
        for (const DomNode& node : m_dom.nodes) {
            const DomAttr* a = node.attr("id");
            found = found || (a != nullptr && a->value == id);
        }
        if (id.empty() || prop.empty() || !found) {
            err = "\"" + head + "\" does not name an element (#<id>.<property>)";
            return false;
        }
        if (prop != "width" && prop != "height" && prop != "left" && prop != "top" && prop != "text" &&
            findProp(prop) == nullptr) {
            err = "\"" + head + "\": unknown property \"" + prop + "\"";
            return false;
        }
        ArgVal a, b;
        a.kind = b.kind = ConverterArg::String;
        a.u = m_writer.intern(id);
        b.u = m_writer.intern(prop);
        appendSteps(steps, ConverterOp::ElemProp, {a, b});
        m_afterLayout = true;
        return true;
    }

    /// The expression's head (before the first `|`).
    bool head(const std::string& text, std::vector<IrConverterStep>& steps, std::string& err) {
        if (text.empty()) {
            err = "empty expression";
            return false;
        }
        {
            ArgVal term;
            const int t = repeatTerm(text, term, err);
            if (t < 0) {
                return false;
            }
            if (t > 0) {
                repeatHead(term, steps);
                return true;
            }
        }
        if (text[0] == '#' && text.find('(') == std::string::npos) {
            // `#Other.width`, unless it is a qualified port (`#Hp.hp`).
            int port = m_scopes.resolve(text, m_element);
            if (port < 0) {
                return elemProp(text, steps, err);
            }
        }
        Call c;
        if (!parseCallText(text, c, err)) {
            if (err.empty()) {
                err = "invalid expression \"" + text + "\"";
            }
            return false;
        }
        if (!c.hasParens) {
            int port = m_scopes.resolve(c.name, m_element);
            if (port < 0) {
                err = "unknown port \"" + c.name + "\"";
                return false;
            }
            m_primary = port;
            addDep(static_cast<uint32_t>(port));
            if (static_cast<PortValueType>(m_scopes.ports[static_cast<size_t>(port)].type) ==
                PortValueType::List) {
                m_listValue = port;  // T-381: `<list port> | item(<n>)`.
            }
            return true;
        }
        if (c.name == "format") {
            if (c.args.size() < 2 || !isQuoted(c.args[0])) {
                err = "format() needs a quoted pattern and at least one value";
                return false;
            }
            // The first value is the chain's input ({0}); a constant one is loaded by concat().
            ArgVal first;
            if (!arg(c.args[1], kAllowAny, first, err)) {
                return false;
            }
            if (first.kind == ConverterArg::Port) {
                m_primary = static_cast<int>(first.u);
            } else if (first.kind == ConverterArg::Item || first.kind == ConverterArg::Index ||
                       first.kind == ConverterArg::Count || first.kind == ConverterArg::OuterItem ||
                       first.kind == ConverterArg::OuterIndex) {
                repeatHead(first, steps);
            } else {
                appendSteps(steps, ConverterOp::Concat, {first});
            }
            std::vector<ArgVal> args;
            ArgVal pattern;
            if (!arg(c.args[0], kAllowString, pattern, err)) {
                return false;
            }
            args.push_back(pattern);
            for (size_t i = 2; i < c.args.size(); ++i) {
                ArgVal a;
                if (!arg(c.args[i], kAllowAny, a, err)) {
                    return false;
                }
                args.push_back(a);
            }
            appendSteps(steps, ConverterOp::Format, args);
            return true;
        }
        if (c.name == "vec2" || c.name == "vec3") {
            // T-200: a vector composed of numbers and ports; the first port is the chain's input.
            const size_t components = c.name == "vec2" ? 2 : 3;
            if (c.args.size() != components) {
                err = c.name + "() takes " + std::to_string(components) + " argument(s)";
                return false;
            }
            std::vector<ArgVal> args;
            for (const std::string& raw : c.args) {
                ArgVal a;
                if (!arg(raw, kAllowNumber | kAllowPort, a, err)) {
                    return false;
                }
                if (a.kind == ConverterArg::Port && m_primary < 0) {
                    m_primary = static_cast<int>(a.u);
                }
                args.push_back(a);
            }
            appendSteps(steps, ConverterOp::Vec, args);
            return true;
        }
        if (c.name == "select" || c.name == "step") {
            if (c.args.empty()) {
                err = c.name + "() needs an input";
                return false;
            }
            std::string key, value;
            splitNamed(c.args[0], key, value);
            if (!key.empty()) {
                err = c.name + "(<input>, <key>: <value>, ...) needs its input first (or use <port> | " +
                      c.name + "(...))";
                return false;
            }
            ArgVal term;
            const int t = repeatTerm(value, term, err);
            if (t < 0) {
                return false;
            }
            if (t > 0) {
                repeatHead(term, steps);
            } else if (!value.empty() && value[0] == '#' && m_scopes.resolve(value, m_element) < 0) {
                if (!elemProp(value, steps, err)) {
                    return false;
                }
            } else {
                int port = m_scopes.resolve(value, m_element);
                if (port < 0) {
                    err = "unknown port \"" + value + "\"";
                    return false;
                }
                m_primary = port;
                addDep(static_cast<uint32_t>(port));
            }
            std::vector<ArgVal> args;
            if (!selectArgs(c, 1, c.name == "step", args, err)) {
                return false;
            }
            appendSteps(steps, c.name == "select" ? ConverterOp::Select : ConverterOp::Step, args);
            return true;
        }
        err =
            "an expression starts with a port, #<id>.<property>, format(), select(), step(), vec2() or "
            "vec3() (inside a <repeat> template also item(<field>), index or count), got \"" +
            c.name + "()\"";
        return false;
    }

    /// T-200 (13.8.3, 03 §3.12.2): `world-anchor="<port>"` -> a WorldAnchor binding whose only
    /// source is that vec2/vec3 port (no steps; evaluated again after every layout).
    void compileWorldAnchor(uint32_t element, const DomAttr& attr, BindingBuilder& out,
                            const SvgBindAttr* scene = nullptr) {
        const std::string name = trimCopyBindEmit(attr.value);
        const int port = m_scopes.resolve(name, element);
        auto fail = [&](const std::string& msg) {
            m_sink.error(m_fileName, "world-anchor: " + msg, attr.line, attr.col);
            m_ok = false;
        };
        if (port < 0) {
            fail("unknown port \"" + name + "\"");
            return;
        }
        const PortValueType type = static_cast<PortValueType>(m_scopes.ports[static_cast<size_t>(port)].type);
        if (type != PortValueType::Vec2 && type != PortValueType::Vec3) {
            fail("port \"" + name + "\" must be vec2 or vec3");
            return;
        }
        IrBinding b{};
        b.element = element;
        b.sceneNode = scene != nullptr ? scene->sceneNode : kBindElementSelf;  // T-243
        b.target = static_cast<uint8_t>(BindTarget::WorldAnchor);
        b.flags = kBindAfterLayout;
        b.targetName = kInvalidString;
        b.sourceOffset = static_cast<uint32_t>(out.sources.size());
        b.sourceCount = 1;
        b.stepOffset = static_cast<uint32_t>(out.steps.size());
        b.stepCount = 0;
        out.sources.push_back(static_cast<uint32_t>(port));
        out.bindings.push_back(b);
    }

    void compileBinding(uint32_t element, const DomAttr& attr, BindingBuilder& out,
                        const SvgBindAttr* scene = nullptr) {
        const std::string target = attr.name.substr(5);
        auto fail = [&](const std::string& msg) {
            m_sink.error(m_fileName, "bind:" + target + ": " + msg, attr.line, attr.col);
            m_ok = false;
        };
        IrBinding b{};
        b.element = element;
        b.targetName = kInvalidString;
        b.sceneNode = kBindElementSelf;
        if (scene != nullptr) {
            // T-243 (32 §32.4): on <svg> content only a property the scene node takes binds.
            const NodeKind kind = static_cast<NodeKind>(scene->kind);
            uint16_t prop = 0;
            if (!transformComponent(target, prop)) {
                const PropInfo* info = findProp(target);
                if (info == nullptr || info->kind == StyleValueKind::Shorthand ||
                    info->prop == kShorthandProp) {
                    fail("only a property binds on <svg> content (unknown property \"" + target + "\")");
                    return;
                }
                prop = info->prop;
            }
            prop = svgNodeProp(prop, kind, static_cast<ConstraintKind>(scene->constraintKind));
            if (!appliesToSvgNode(prop, kind)) {
                fail("\"" + target + "\" does not apply to this <svg> node");
                return;
            }
            b.sceneNode = scene->sceneNode;
            b.target = static_cast<uint8_t>(BindTarget::Prop);
            b.targetId = prop;
            b.targetName = m_writer.intern(target);
        } else if (const int port = instancePortOf(element, target); port >= 0) {
            // T-380 (27 §27.3.4): the instance's in-port wins over a property of the same name.
            const IrPort& p = m_scopes.ports[static_cast<uint32_t>(port)];
            if (static_cast<PortDir>(p.dir) != PortDir::In ||
                static_cast<PortValueType>(p.type) == PortValueType::Event) {
                m_sink.error(m_fileName,
                             "bind: cannot target an out port or an event port (bind:" + target + ")",
                             attr.line, attr.col);
                m_ok = false;
                return;
            }
            if (port > 0xFFFF) {
                fail("too many ports to target this one");
                return;
            }
            b.target = static_cast<uint8_t>(BindTarget::Port);
            b.targetId = static_cast<uint16_t>(port);
        } else if (target == "text") {
            b.target = static_cast<uint8_t>(BindTarget::Text);
        } else if (target == "src") {
            b.target = static_cast<uint8_t>(BindTarget::Src);
        } else if (target == "visible") {
            b.target = static_cast<uint8_t>(BindTarget::Visible);
        } else if (target == "disabled") {
            b.target = static_cast<uint8_t>(BindTarget::Disabled);  // T-385 (27 §27.3.2).
        } else if (target == "value") {
            // T-392 (27 §27.3.5): only an editable (text) input has a runtime value.
            const DomNode& node = m_dom.nodes[element];
            const DomAttr* type = node.attr("type");
            if (node.tag != "input" || (type != nullptr && !type->value.empty() && type->value != "text")) {
                fail("binds only on an editable <input> (type=\"text\" or no type)");
                return;
            }
            b.target = static_cast<uint8_t>(BindTarget::Value);
        } else if (target == "display") {
            b.target = static_cast<uint8_t>(BindTarget::Display);  // T-385 (27 §27.3.2).
        } else if (target == "checked") {
            // T-204 (05 §5.9.1): only a checkbox input has a checked state.
            const DomNode& node = m_dom.nodes[element];
            const DomAttr* type = node.attr("type");
            if (node.tag != "input" || type == nullptr || type->value != "checkbox") {
                fail("binds only on an <input type=\"checkbox\">");
                return;
            }
            b.target = static_cast<uint8_t>(BindTarget::Checked);
        } else if (target.rfind("state.", 0) == 0 && builtinStateAlias(target.substr(6)) != 0) {
            // T-392 (27 §27.3.5): `bind:state.Focused|Hover|Active|Checked` is the built-in state
            // (`bind:state.Disabled` was rewritten to `bind:disabled` already).
            b.target = static_cast<uint8_t>(BindTarget::BuiltinState);
            b.targetId = builtinStateAlias(target.substr(6));
        } else if (target.rfind("state.", 0) == 0) {
            std::string name = target.substr(6);
            StringId id = m_writer.intern(name);
            auto it = std::find(m_states.names.begin(), m_states.names.end(), id);
            if (!isIdent(name) || it == m_states.names.end()) {
                fail("invalid custom state \"" + name + "\"");
                return;
            }
            b.target = static_cast<uint8_t>(BindTarget::State);
            b.targetId = static_cast<uint16_t>(it - m_states.names.begin());
        } else if (target.rfind("class.", 0) == 0) {
            std::string name = target.substr(6);
            if (name.empty()) {
                fail("empty class name");
                return;
            }
            b.target = static_cast<uint8_t>(BindTarget::Class);
            b.targetName = m_writer.intern(name);
        } else if (target.rfind("variant.", 0) == 0) {
            std::string prop = target.substr(8);
            if (!isIdent(prop) || m_instanceRoots.count(element) == 0) {
                fail("variant.<Prop> binds only on a symbol instance (<use>)");
                return;
            }
            b.target = static_cast<uint8_t>(BindTarget::Variant);
            b.targetName = m_writer.intern("v-" + prop + "-");
        } else if (uint16_t component = 0; transformComponent(target, component)) {
            // T-201: a transform component (the PropId `transform` expands into); no relayout.
            b.target = static_cast<uint8_t>(BindTarget::Prop);
            b.targetId = component;
            b.targetName = m_writer.intern(target);
        } else {
            const PropInfo* info = findProp(target);
            if (info == nullptr || info->kind == StyleValueKind::Shorthand || info->prop == kShorthandProp) {
                fail("unknown or shorthand property \"" + target + "\"");
                return;
            }
            b.target = static_cast<uint8_t>(BindTarget::Prop);
            b.targetId = info->prop;
            b.targetName = m_writer.intern(target);
        }

        m_element = element;
        m_primary = -1;
        m_deps.clear();
        m_afterLayout = false;
        m_repeatItem = false;
        m_listValue = -1;
        m_needField = false;
        std::vector<IrConverterStep> steps;
        std::string err;
        std::vector<std::string> segments = splitTop(attr.value, '|');
        if (!head(trimCopyBindEmit(segments[0]), steps, err)) {
            fail(err);
            return;
        }
        for (size_t i = 1; i < segments.size(); ++i) {
            Call c;
            if (!parseCallText(segments[i], c, err) || !converter(c, steps, err)) {
                if (err.empty()) {
                    err = "invalid converter \"" + trimCopyBindEmit(segments[i]) + "\"";
                }
                fail(err);
                return;
            }
        }
        if (!checkNeedField(err)) {
            fail(err);
            return;
        }
        if (steps.size() > 0xFFFFu || m_deps.size() > 0xFFFFu) {
            fail("expression too long");
            return;
        }
        // Sources: the input port first, then every other port the chain reads.
        std::vector<uint32_t> sources;
        if (m_primary >= 0) {
            sources.push_back(static_cast<uint32_t>(m_primary));
        }
        for (uint32_t dep : m_deps) {
            if (static_cast<int>(dep) != m_primary) {
                sources.push_back(dep);
            }
        }
        b.flags = static_cast<uint8_t>((m_afterLayout ? kBindAfterLayout : 0) |
                                       (m_primary < 0 ? kBindNoPrimary : 0) |
                                       (m_repeatItem && m_primary < 0 ? kBindRepeatItem : 0));
        b.sourceOffset = static_cast<uint32_t>(out.sources.size());
        b.sourceCount = static_cast<uint16_t>(sources.size());
        b.stepOffset = static_cast<uint32_t>(out.steps.size());
        b.stepCount = static_cast<uint16_t>(steps.size());
        out.sources.insert(out.sources.end(), sources.begin(), sources.end());
        out.steps.insert(out.steps.end(), steps.begin(), steps.end());
        out.bindings.push_back(b);
    }
};

}  // namespace

bool compileConverterChain(const DomTree& dom, const PortScopes& scopes, IrWriter& writer, uint32_t element,
                           std::string_view converters, std::vector<IrConverterStep>& steps,
                           std::string& err) {
    static const std::vector<ExpandedSymbolInstance> kNoInstances;
    static const CustomStateBuilder kNoStates;
    DiagnosticSink sink;
    Binder binder(dom, kNoInstances, scopes, kNoStates, writer, "", sink, nullptr);
    return binder.chain(element, converters, steps, err);
}

bool compileActionValue(const DomTree& dom, const PortScopes& scopes, const RepeatScopes* repeats,
                        IrWriter& writer, uint32_t element, std::string_view text, ActionValue& out,
                        std::string& err) {
    static const std::vector<ExpandedSymbolInstance> kNoInstances;
    static const CustomStateBuilder kNoStates;
    DiagnosticSink sink;
    Binder binder(dom, kNoInstances, scopes, kNoStates, writer, "", sink, repeats);
    return binder.actionValue(element, text, out, err);
}

int PortScopes::resolve(std::string_view name, uint32_t element) const {
    if (!name.empty() && name[0] == '#') {
        name.remove_prefix(1);
    }
    if (name.find('.') != std::string_view::npos) {
        auto it = qualified.find(name);
        return it != qualified.end() ? static_cast<int>(it->second) : -1;
    }
    uint32_t scope = element < scopeOf.size() ? scopeOf[element] : kNoPortScope;
    for (;;) {
        for (uint32_t i = 0; i < ports.size(); ++i) {
            if (ports[i].scope == scope && names[i] == name) {
                return static_cast<int>(i);
            }
        }
        if (scope == kNoPortScope || scope >= scopeOf.size()) {
            return -1;
        }
        scope = scopeOf[scope];
    }
}

int PortScopes::maxCompanion(uint32_t port) const {
    if (port >= ports.size()) {
        return -1;
    }
    const std::string want = names[port] + "_max";
    for (uint32_t i = 0; i < ports.size(); ++i) {
        if (ports[i].scope == ports[port].scope && names[i] == want) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

namespace {

/// T-383 (27 §27.3.3): the reserved names that are ambiguous as a `bind:` source -- `index`,
/// `count`, `item` and `time` (the document clock). An in port with one of these names warns (the
/// port still wins). Converter names are reserved too but never ambiguous (a name after `|` is
/// always the converter), so they do not warn (T-385 ruling).
bool isReservedBindName(std::string_view name) {
    return name == "index" || name == "count" || name == "item" || name == "time";
}

/// T-383: the warning for in port `name` shadowing a reserved name.
std::string reservedNameWarning(const std::string& name) {
    std::string msg = "port name \"" + name + "\" shadows a reserved name; bindings read the port";
    if (name == "index") {
        return msg + "; write repeat.index for the repeat counter";
    }
    if (name == "count") {
        return msg + "; write repeat.count for the repeat entry count";
    }
    if (name == "item") {
        return msg + "; write repeat.item(...) for the repeat entry";
    }
    return msg + "; " + name + "(...) after a '|' is still the converter";
}

}  // namespace

namespace {

/// T-391 (27 §27.3.4): appends `type`'s fields to `out.fields` (contiguous; returns their offset),
/// then, for every field of type List, its own fields and an IrPortNested record (recursively).
uint32_t appendPortFields(const ParsedPortType& type, IrWriter& writer, PortBuilder& out) {
    const uint32_t offset = static_cast<uint32_t>(out.fields.size());
    for (const auto& [fname, ftype] : type.fields) {
        IrPortField f{};
        f.name = writer.intern(fname);
        f.type = static_cast<uint8_t>(ftype);
        out.fields.push_back(f);
    }
    for (size_t k = 0; k < type.fields.size() && k < type.subs.size(); ++k) {
        if (type.fields[k].second != PortValueType::List) {
            continue;
        }
        const ParsedPortType& sub = type.subs[k];
        IrPortNested n{};
        n.field = offset + static_cast<uint32_t>(k);
        n.entryType = sub.entryType;
        n.fieldCount = static_cast<uint32_t>(sub.fields.size());
        n.fieldOffset = appendPortFields(sub, writer, out);
        out.nested.push_back(n);
    }
    return offset;
}

}  // namespace

bool emitPorts(const DomTree& dom, const std::vector<ExpandedSymbolInstance>& instances, IrWriter& writer,
               std::string_view fileName, DiagnosticSink& sink, PortBuilder& out, PortScopes& scopes,
               const std::vector<std::pair<std::string, int>>* boneIds) {
    bool ok = true;
    const std::string file(fileName);
    std::set<uint32_t> roots;
    for (const ExpandedSymbolInstance& inst : instances) {
        roots.insert(inst.instanceNode);
    }
    scopes.scopeOf.assign(dom.nodes.size(), kNoPortScope);
    for (uint32_t i = 0; i < dom.nodes.size(); ++i) {
        uint32_t p = dom.nodes[i].parent;
        while (p != 0xFFFFFFFFu && p < dom.nodes.size()) {
            if (roots.count(p) != 0) {
                scopes.scopeOf[i] = p;
                break;
            }
            p = dom.nodes[p].parent;
        }
    }
    auto exposeOff = [&](uint32_t node) {
        const DomAttr* a = node < dom.nodes.size() ? dom.nodes[node].attr("expose") : nullptr;
        return a != nullptr && trimCopyBindEmit(a->value) == "false";
    };
    for (uint32_t element = 0; element < dom.nodes.size(); ++element) {
        const DomNode& node = dom.nodes[element];
        if (const DomAttr* expose = node.attr("expose")) {
            std::string v = trimCopyBindEmit(expose->value);
            if (v != "true" && v != "false") {
                sink.error(file, "expose must be true or false", expose->line, expose->col);
                ok = false;
            }
        }
        for (const DomAttr& attr : node.attrs) {
            if (attr.name.size() <= 5 || attr.name.compare(0, 5, "port:") != 0) {
                continue;
            }
            const std::string name = attr.name.substr(5);
            auto fail = [&](const std::string& msg) {
                sink.error(file, "port:" + name + ": " + msg, attr.line, attr.col);
                ok = false;
            };
            if (!isIdent(name)) {
                fail("invalid port name");
                continue;
            }
            // `<type> [in|out|call] [= <default>]`: the direction is the last top-level word (before
            // the default) when it is one.
            std::string text = trimCopyBindEmit(attr.value);
            std::string defaultPart;
            if (size_t eq = findTop(text, '='); eq != std::string::npos) {
                defaultPart = " " + text.substr(eq);
                text = trimCopyBindEmit(std::string_view(text).substr(0, eq));
            }
            PortDir dir = PortDir::In;
            size_t space = text.find_last_of(" \t");
            if (space != std::string::npos && findTop(text.substr(space), '(') == std::string::npos) {
                std::string last = trimCopyBindEmit(std::string_view(text).substr(space));
                if (last == "in" || last == "out" || last == "call") {
                    dir = last == "in" ? PortDir::In : last == "out" ? PortDir::Out : PortDir::Call;
                    text = trimCopyBindEmit(std::string_view(text).substr(0, space));
                }
            }
            ParsedPortType type;
            std::string err;
            if (!parsePortType(text + defaultPart, type, err)) {
                if (err.rfind("default", 0) == 0) {
                    sink.error(file, "port '" + name + "': " + err, attr.line, attr.col);  // T-358
                    ok = false;
                } else {
                    fail(err);
                }
                continue;
            }
            const uint32_t scope = scopes.scopeOf[element];
            bool dup = false;
            for (uint32_t i = 0; i < scopes.ports.size(); ++i) {
                dup = dup || (scopes.ports[i].scope == scope && scopes.names[i] == name);
            }
            if (dup) {
                fail("declared twice in the same scope");
                continue;
            }
            IrPort port{};
            port.name = writer.intern(name);
            port.type = static_cast<uint8_t>(type.type);
            port.dir = static_cast<uint8_t>(dir);
            const bool exposed = !exposeOff(element) && (scope == kNoPortScope || !exposeOff(scope));
            port.flags = exposed ? kPortExpose : 0;
            port.entryType = type.entryType;  // T-381
            port.element = element;
            port.scope = scope;
            port.fieldCount = static_cast<uint32_t>(type.fields.size());
            port.fieldOffset = appendPortFields(type, writer, out);  // T-391: nested lists too.
            const uint32_t index = static_cast<uint32_t>(scopes.ports.size());
            if (type.hasDefault) {  // T-358
                IrPortDefault d{};
                d.port = index;
                d.type = port.type;
                std::memcpy(d.value, type.defaultValue, sizeof(d.value));
                if (type.type == PortValueType::String || type.type == PortValueType::Enum ||
                    type.type == PortValueType::List) {  // T-383: a list's JSON text.
                    const uint32_t id = writer.intern(type.defaultText);
                    std::memcpy(d.value, &id, sizeof(id));
                }
                out.defaults.push_back(d);
            }
            scopes.ports.push_back(port);
            scopes.names.push_back(name);
            out.ports.push_back(port);
            if (dir == PortDir::In && isReservedBindName(name)) {
                sink.warn(file, reservedNameWarning(name), attr.line, attr.col);  // T-383
            }
            // Qualified names: `<element id>.<port>`, and `<instance id>.<port>` when exposed.
            if (const DomAttr* id = node.attr("id"); id != nullptr && !id->value.empty()) {
                scopes.qualified.emplace(id->value + "." + name, index);
            }
            if (scope != kNoPortScope && exposed) {
                if (const DomAttr* id = dom.nodes[scope].attr("id"); id != nullptr && !id->value.empty()) {
                    scopes.qualified.emplace(id->value + "." + name, index);
                }
            }
        }
    }
    declareSystemPorts(dom, writer, out, scopes);
    if (boneIds != nullptr) {
        declareBonePorts(*boneIds, writer, fileName, sink, out, scopes);  // T-243b
    }
    return ok;
}

void normalizeBindAliases(DomTree& dom, std::string_view fileName, DiagnosticSink& sink) {
    for (DomNode& node : dom.nodes) {
        for (DomAttr& attr : node.attrs) {
            // T-385 (27 §27.3.2): `bind:state.Disabled` is the built-in state, no custom one.
            if (const NameAlias* alias = findNameAlias(AliasKind::Binding, attr.name)) {
                sink.deprecated(std::string(fileName), deprecatedMessage(*alias), node.line, node.col);
                attr.name = alias->replacement;
            }
        }
    }
}

namespace {

/// T-385: `<use>` attributes that are no port and need no warning: the element attributes the
/// compiler and the run time read (plus `data-*` / `aria-*`). T-394: `draggable`, `drop-target`,
/// `drag-threshold`, `drag-autoscroll`, `drag-ghost` included.
bool isStandardUseAttr(std::string_view name) {
    static constexpr std::string_view kNames[] = {
        "symbol",
        "id",
        "class",
        "style",
        "state",
        "state-groups",
        "slot",
        "expose",
        "editor-only",
        "title",
        "alt",
        "placeholder",
        "disabled",
        "checked",
        "autofocus",
        "draggable",
        "drop-target",
        "focus-trap",
        "tabindex",
        "world-anchor",
        "sync",
        "layer",
        "sound",
        "haptic",
        // T-394 (08.15): the drag attributes are element attributes on <use> too.
        "drag-threshold",
        "drag-autoscroll",
        "drag-ghost",
    };
    for (std::string_view n : kNames) {
        if (n == name) {
            return true;
        }
    }
    return name.rfind("data-", 0) == 0 || name.rfind("aria-", 0) == 0;
}

/// T-390: ASCII case-insensitive equality (`<use>` attribute names vs. port names).
bool equalsIgnoreCaseBindEmit(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

/// T-385: parses `<use>` literal `text` for port `port` into `d` (IrPortDefault::value; String /
/// Enum / List / Struct as interned text). False with `err`.
bool parseUseLiteral(const IrPort& port, const PortBuilder& ports, IrWriter& writer, const std::string& text,
                     IrPortDefault& d, std::string& err) {
    d = IrPortDefault{};
    d.type = port.type;
    const PortValueType type = static_cast<PortValueType>(port.type);
    auto putText = [&](const std::string& value) {
        const uint32_t id = writer.intern(value);
        std::memcpy(d.value, &id, sizeof(id));
    };
    if (type == PortValueType::String) {
        putText(text);  // The attribute value as written, no quotes needed.
        return true;
    }
    if (type == PortValueType::Struct) {
        const std::string lit = trimCopyBindEmit(text);
        if (lit.size() < 2 || lit.front() != '{' || lit.back() != '}') {
            err = "\"" + lit + "\" is not a JSON object ({...})";
            return false;
        }
        putText(lit);
        return true;
    }
    ParsedPortType parsed;
    parsed.type = type;
    parsed.entryType = port.entryType;
    for (uint32_t k = 0; k < port.fieldCount && port.fieldOffset + k < ports.fields.size(); ++k) {
        const IrPortField& f = ports.fields[port.fieldOffset + k];
        parsed.fields.emplace_back(std::string(writer.stringAt(f.name)), static_cast<PortValueType>(f.type));
    }
    if (!parsePortDefault(text, parsed, err)) {
        if (err.rfind("default ", 0) == 0) {
            err = err.substr(8);
        }
        return false;
    }
    std::memcpy(d.value, parsed.defaultValue, sizeof(d.value));
    if (type == PortValueType::Enum || type == PortValueType::List) {
        putText(parsed.defaultText);
    }
    return true;
}

}  // namespace

bool applyUseLiterals(DomTree& dom, const std::vector<ExpandedSymbolInstance>& instances,
                      const PortScopes& scopes, IrWriter& writer, std::string_view fileName,
                      DiagnosticSink& sink, PortBuilder& out, bool strict) {
    bool ok = true;
    const std::string file(fileName);
    std::vector<std::string> ambiguousReported;  // T-390: one error per symbol.
    for (const ExpandedSymbolInstance& inst : instances) {
        if (inst.instanceNode >= dom.nodes.size()) {
            continue;
        }
        const std::string where = "<use symbol=\"" + inst.symbol + "\">";
        // T-390 (27.3.2, XF-065): the parser lower-cases plain attribute names, so a port name is
        // matched without regard to ASCII case; two ports differing only in case are ambiguous.
        std::vector<uint32_t> own;
        for (uint32_t i = 0; i < scopes.ports.size(); ++i) {
            if (scopes.ports[i].scope == inst.instanceNode) {
                own.push_back(i);
            }
        }
        bool ambiguous = false;
        for (size_t a = 0; a < own.size() && !ambiguous; ++a) {
            for (size_t b = a + 1; b < own.size() && !ambiguous; ++b) {
                if (scopes.names[own[a]] != scopes.names[own[b]] &&
                    equalsIgnoreCaseBindEmit(scopes.names[own[a]], scopes.names[own[b]])) {
                    ambiguous = true;
                    if (std::find(ambiguousReported.begin(), ambiguousReported.end(), inst.symbol) ==
                        ambiguousReported.end()) {
                        ambiguousReported.push_back(inst.symbol);
                        const DomNode& root = dom.nodes[inst.instanceNode];
                        sink.error(file,
                                   "ports \"" + scopes.names[own[a]] + "\" and \"" + scopes.names[own[b]] +
                                       "\" of symbol " + inst.symbol +
                                       " are ambiguous: <use> attribute names are case-insensitive",
                                   root.line, root.col);
                    }
                }
            }
        }
        if (ambiguous) {
            ok = false;
            continue;
        }
        std::vector<std::string> consumed;
        for (const DomAttr& attr : inst.useAttrs) {
            int found = -1;
            for (uint32_t i : own) {
                if (found < 0 && equalsIgnoreCaseBindEmit(scopes.names[i], attr.name)) {
                    found = static_cast<int>(i);
                }
            }
            if (found < 0) {
                if (strict && !isStandardUseAttr(attr.name)) {
                    sink.warn(file,
                              "unknown attribute \"" + attr.name + "\" on " + where + ": not a port of " +
                                  inst.symbol + " (attribute names are case-insensitive)",
                              attr.line, attr.col);
                }
                continue;
            }
            const std::string& portName = scopes.names[static_cast<uint32_t>(found)];  // As declared.
            const IrPort& port = scopes.ports[static_cast<uint32_t>(found)];
            if (static_cast<PortDir>(port.dir) != PortDir::In ||
                static_cast<PortValueType>(port.type) == PortValueType::Event) {
                sink.error(file,
                           "attribute \"" + attr.name + "\" on " + where +
                               ": a literal value cannot set an out port or an event port",
                           attr.line, attr.col);
                ok = false;
                continue;
            }
            if (dom.nodes[inst.instanceNode].attr("bind:" + portName) != nullptr ||
                dom.nodes[inst.instanceNode].attr("bind:" + attr.name) != nullptr) {
                sink.error(file,
                           "attribute \"" + attr.name + "\" on " + where +
                               ": a literal value and bind:" + portName + " cannot both be given",
                           attr.line, attr.col);
                ok = false;
                continue;
            }
            IrPortDefault d{};
            std::string err;
            const PortValueType type = static_cast<PortValueType>(port.type);
            if (type == PortValueType::Texture || type == PortValueType::Entity) {
                err = "a literal value is not allowed for this port type";
            } else if (parseUseLiteral(port, out, writer, attr.value, d, err)) {
                d.port = static_cast<uint32_t>(found);
                auto existing = std::find_if(out.defaults.begin(), out.defaults.end(),
                                             [&](const IrPortDefault& e) { return e.port == d.port; });
                if (existing != out.defaults.end()) {
                    *existing = d;  // The instance's value replaces the declared default.
                } else {
                    out.defaults.push_back(d);
                }
                consumed.push_back(attr.name);
                continue;
            }
            sink.error(file, "attribute \"" + attr.name + "\" on " + where + ": " + err, attr.line, attr.col);
            ok = false;
        }
        // A port value is not also an element attribute (`disabled="true"` on a port `disabled`).
        std::erase_if(dom.nodes[inst.instanceNode].attrs, [&](const DomAttr& a) {
            return std::find(consumed.begin(), consumed.end(), a.name) != consumed.end();
        });
    }
    return ok;
}

void declareBonePorts(const std::vector<std::pair<std::string, int>>& boneIds, IrWriter& writer,
                      std::string_view fileName, DiagnosticSink& sink, PortBuilder& out, PortScopes& scopes) {
    const std::string file(fileName);
    for (const auto& [id, line] : boneIds) {
        for (int k = 0; k < 2; ++k) {
            const std::string name = id + (k == 0 ? ".world" : ".angle");
            if (scopes.qualified.count(name) != 0) {
                sink.warn(file,
                          "bone port \"" + name + "\" is already declared (bone id \"" + id +
                              "\" used twice?), only the first is declared",
                          line, 0);
                continue;
            }
            IrPort port{};
            port.name = writer.intern(name);
            port.type = static_cast<uint8_t>(k == 0 ? PortValueType::Vec2 : PortValueType::Number);
            port.dir = static_cast<uint8_t>(PortDir::Out);
            port.flags = 0;
            port.element = 0;
            port.scope = kNoPortScope;
            port.fieldOffset = static_cast<uint32_t>(out.fields.size());
            port.fieldCount = 0;
            const uint32_t index = static_cast<uint32_t>(scopes.ports.size());
            scopes.ports.push_back(port);
            scopes.names.push_back(name);
            out.ports.push_back(port);
            scopes.qualified.emplace(name, index);
        }
    }
}

void declareSystemPorts(const DomTree& dom, IrWriter& writer, PortBuilder& out, PortScopes& scopes) {
    // T-200 (27.12, 23.10 item 4): the implicit document-level system ports, in a fixed order.
    struct SystemPort {
        const char* name;
        PortValueType type;
    };
    static constexpr SystemPort kPorts[] = {
        {"system.loading", PortValueType::Event},        {"system.paused", PortValueType::Event},
        {"system.resumed", PortValueType::Event},        {"system.disconnected", PortValueType::Event},
        {"system.language", PortValueType::String},      {"system.platform", PortValueType::String},
        {"system.inputDevice", PortValueType::String},   {"system.back", PortValueType::Event},  // T-240.
        {"system.reducedMotion", PortValueType::String},                                         // T-330.
    };
    // A reference is the name as a whole word in a `bind:` / `on:` value or in an `on:` event
    // (`on:port(system.paused)`).
    auto wordIn = [](std::string_view text, std::string_view name) {
        auto word = [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '.' || c == '#' ||
                   c == '/';
        };
        for (size_t pos = text.find(name); pos != std::string_view::npos; pos = text.find(name, pos + 1)) {
            const size_t end = pos + name.size();
            if ((pos == 0 || !word(text[pos - 1])) && (end == text.size() || !word(text[end]))) {
                return true;
            }
        }
        return false;
    };
    auto referenced = [&](std::string_view name) {
        for (const DomNode& node : dom.nodes) {
            for (const DomAttr& attr : node.attrs) {
                const bool on = attr.name.rfind("on:", 0) == 0;
                if ((on || attr.name.rfind("bind:", 0) == 0) &&
                    (wordIn(attr.value, name) || (on && wordIn(attr.name, name)))) {
                    return true;
                }
            }
        }
        return false;
    };
    for (const SystemPort& sp : kPorts) {
        if (scopes.qualified.count(sp.name) != 0 || !referenced(sp.name)) {
            continue;
        }
        IrPort port{};
        port.name = writer.intern(sp.name);
        port.type = static_cast<uint8_t>(sp.type);
        port.dir = static_cast<uint8_t>(PortDir::In);
        port.flags = 0;
        port.element = 0;
        port.scope = kNoPortScope;
        port.fieldOffset = static_cast<uint32_t>(out.fields.size());
        port.fieldCount = 0;
        const uint32_t index = static_cast<uint32_t>(scopes.ports.size());
        scopes.ports.push_back(port);
        scopes.names.emplace_back(sp.name);
        out.ports.push_back(port);
        scopes.qualified.emplace(sp.name, index);
    }
}

bool emitBindings(const DomTree& dom, const std::vector<ExpandedSymbolInstance>& instances,
                  const PortScopes& scopes, const CustomStateBuilder& states, IrWriter& writer,
                  std::string_view fileName, DiagnosticSink& sink, BindingBuilder& out,
                  const RepeatScopes* repeats, const std::map<uint32_t, std::vector<SvgBindAttr>>* svgBinds) {
    Binder binder(dom, instances, scopes, states, writer, fileName, sink, repeats);

    return binder.run(out, svgBinds);
}

// -- T-180: context-free validation (validate.h) ------------------------------------------------

namespace {

/// 27.3.3's converter catalog (the names Binder::converter()/head() accept), `host.<name>`
/// excluded (checked separately).
bool isCatalogConverter(std::string_view name) {
    static constexpr std::string_view kNames[] = {
        "range",    "clamp",   "round", "abs",     "scale", "offset", "lt",    "le",        "gt",
        "ge",       "eq",      "ne",    "between", "not",   "and",    "or",    "format",    "thousands",
        "fixed",    "percent", "time",  "upper",   "lower", "select", "step",  "lerpColor", "fromToken",
        "localize", "concat",  "glyph", "tween",   "delay", "count",  "index", "item",      "x",
        "y",        "z",       "w",     "vec2",    "vec3",
    };
    for (std::string_view n : kNames) {
        if (n == name) {
            return true;
        }
    }
    return false;
}

/// `name`, `a.b` or `#a.b` / `#a/b.c` (a port, a qualified port or an element property).
bool isReference(std::string_view text) {
    if (!text.empty() && text[0] == '#') {
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return false;
    }
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '.' || text[i] == '/') {
            std::string_view part = text.substr(start, i - start);
            if (!isIdent(part) && part.find('-') == std::string_view::npos) {
                return false;
            }
            if (part.empty()) {
                return false;
            }
            start = i + 1;
        }
    }
    return true;
}

}  // namespace

bool validateBindingTarget(std::string_view target, std::string& error) {
    std::string t = trimCopyBindEmit(target);
    // T-204: checked; T-385: disabled (`display` is also a property).
    if (t == "text" || t == "src" || t == "visible" || t == "checked" || t == "disabled" || t == "value") {
        return true;
    }
    auto suffix = [&](std::string_view prefix, std::string& rest) {
        if (t.rfind(prefix, 0) != 0) {
            return false;
        }
        rest = t.substr(prefix.size());
        return true;
    };
    std::string rest;
    if (suffix("state.", rest) || suffix("variant.", rest)) {
        if (!isIdent(rest)) {
            error = "invalid name \"" + rest + "\" in bind:" + t;
            return false;
        }
        return true;
    }
    if (suffix("class.", rest)) {
        if (rest.empty()) {
            error = "empty class name";
            return false;
        }
        return true;
    }
    const PropInfo* info = findProp(t);
    if (info == nullptr || info->kind == StyleValueKind::Shorthand || info->prop == kShorthandProp) {
        error = "unknown or shorthand property \"" + t + "\"";
        return false;
    }
    return true;
}

bool validateBindingTarget(std::string_view target, std::span<const std::string> instanceInPorts,
                           std::string& error) {
    const std::string t = trimCopyBindEmit(target);
    if (std::find(instanceInPorts.begin(), instanceInPorts.end(), t) != instanceInPorts.end()) {
        return true;  // T-380: the instance's in-port wins over a property of the same name.
    }
    return validateBindingTarget(target, error);
}

bool symbolInPorts(std::string_view xtText, std::string_view symbol, std::vector<std::string>& ports,
                   std::vector<std::string>* symbolHrefs) {
    ports.clear();
    DiagnosticSink sink;
    DomTree dom;
    if (!parseXtSymbolLibrary(xtText, "symbol", sink, dom)) {
        return false;
    }
    if (symbolHrefs != nullptr) {
        *symbolHrefs = dom.symbolHrefs;
    }
    for (uint32_t def : dom.symbolDefs) {
        if (def >= dom.nodes.size()) {
            continue;
        }
        const DomAttr* name = dom.nodes[def].attr("name");
        if (name == nullptr || name->value != symbol) {
            continue;
        }
        std::vector<uint32_t> stack{def};
        while (!stack.empty()) {
            const uint32_t idx = stack.back();
            stack.pop_back();
            if (idx >= dom.nodes.size()) {
                continue;
            }
            const DomNode& n = dom.nodes[idx];
            for (const DomAttr& a : n.attrs) {
                if (a.name.rfind("port:", 0) != 0) {
                    continue;
                }
                // `<type> [in|out|call] [= <default>]`, as emitPorts reads it.
                std::string text = trimCopyBindEmit(a.value);
                if (size_t eq = findTop(text, '='); eq != std::string::npos) {
                    text = trimCopyBindEmit(std::string_view(text).substr(0, eq));
                }
                bool in = true;
                size_t space = text.find_last_of(" \t");
                if (space != std::string::npos && findTop(text.substr(space), '(') == std::string::npos) {
                    const std::string last = trimCopyBindEmit(std::string_view(text).substr(space));
                    if (last == "in" || last == "out" || last == "call") {
                        in = last == "in";
                        text = trimCopyBindEmit(std::string_view(text).substr(0, space));
                    }
                }
                const bool event = text.rfind("event", 0) == 0 &&
                                   (text.size() == 5 || text[5] == '(' || text[5] == ' ' || text[5] == '\t');
                const std::string port = a.name.substr(5);
                if (in && !event && std::find(ports.begin(), ports.end(), port) == ports.end()) {
                    ports.push_back(port);
                }
            }
            for (uint32_t child : n.children) {
                if (child < dom.nodes.size() && dom.nodes[child].tag != "use") {
                    stack.push_back(child);  // A nested instance's ports are its own.
                }
            }
        }
        return true;
    }
    return false;
}

bool validateBindingExpression(std::string_view expression, std::string& error) {
    std::string text = trimCopyBindEmit(expression);
    if (text.empty()) {
        error = "empty expression";
        return false;
    }
    std::vector<std::string> segments = splitTop(text, '|');
    for (size_t i = 0; i < segments.size(); ++i) {
        std::string seg = trimCopyBindEmit(segments[i]);
        Call c;
        if (seg.empty() || !parseCallText(seg, c, error)) {
            if (error.empty()) {
                error = "invalid expression \"" + seg + "\"";
            }
            return false;
        }
        if (i == 0) {
            if (!c.hasParens) {
                if (!isReference(c.name)) {
                    error =
                        "an expression starts with a port, #<id>.<property>, format(), select() or step(), "
                        "got \"" +
                        c.name + "\"";
                    return false;
                }
                continue;
            }
            if (c.name == "vec2" || c.name == "vec3") {
                if (c.args.size() != (c.name == "vec2" ? 2u : 3u)) {
                    error = c.name + "() takes " + (c.name == "vec2" ? "2" : "3") + " argument(s)";
                    return false;
                }
                continue;
            }
            if (c.name != "format" && c.name != "select" && c.name != "step") {
                error =
                    "an expression starts with a port, #<id>.<property>, format(), select(), step(), vec2() "
                    "or vec3(), got \"" +
                    c.name + "()\"";
                return false;
            }
            if (c.name == "format" && (c.args.size() < 2 || !isQuoted(c.args[0]))) {
                error = "format() needs a quoted pattern and at least one value";
                return false;
            }
            if (c.name != "format" && c.args.empty()) {
                error = c.name + "() needs an input";
                return false;
            }
            continue;
        }
        const bool host = c.name.rfind("host.", 0) == 0 && isIdent(std::string_view(c.name).substr(5));
        if (!host && !isCatalogConverter(c.name)) {
            error = "unknown converter \"" + c.name + "\"";
            return false;
        }
    }
    return true;
}

bool validatePortName(std::string_view name, std::string& error) {
    if (!isIdent(name)) {
        error = "invalid port name \"" + std::string(name) + "\"";
        return false;
    }
    return true;
}

bool validatePortDeclaration(std::string_view declaration, std::string& error) {
    std::string text = trimCopyBindEmit(declaration);
    // T-358: the `= <default>` tail is split off first, then re-attached to the type.
    std::string defaultPart;
    if (size_t eq = findTop(text, '='); eq != std::string::npos) {
        defaultPart = " " + text.substr(eq);
        text = trimCopyBindEmit(std::string_view(text).substr(0, eq));
    }
    // The direction is the last top-level word when it is in/out/call (emitPorts' rule).
    std::string typeText = text;
    size_t space = text.find_last_of(" \t");
    if (space != std::string::npos && findTop(text.substr(space), '(') == std::string::npos) {
        std::string word = trimCopyBindEmit(std::string_view(text).substr(space + 1));
        if (word == "in" || word == "out" || word == "call") {
            typeText = trimCopyBindEmit(std::string_view(text).substr(0, space));
        }
    }
    ParsedPortType parsed;
    return parsePortType(typeText + defaultPart, parsed, error);
}

bool isKnownStyleProperty(std::string_view name) {
    if (findProp(name) != nullptr) {
        return true;
    }
    // Declarations the .xts parser accepts outside the property table (xts_parser.cpp parseDecls):
    // the transition longhands (T-322), triggers (`on-<event>`, `state-enter` / `state-exit`, T-174).
    static constexpr std::string_view kExtra[] = {
        "transition-property", "transition-duration", "transition-timing-function",
        "transition-delay",    "state-enter",         "state-exit"};
    for (std::string_view extra : kExtra) {
        if (name == extra) {
            return true;
        }
    }
    return name.size() > 3 && name.substr(0, 3) == "on-";
}

}  // namespace xt::compile
