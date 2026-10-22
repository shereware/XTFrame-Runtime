#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "css_number.h"       // Code/Runtime/core/src: the CSS number (T-272).
#include "xt/command_list.h"  // customParamSlots().
#include "xt/compile/diagnostics.h"
#include "xt/compile/parse_value.h"
#include "xt/compile/xtfx.h"
#include "xt/property.h"  // prop::kEffectParam*.

/// `.xtfx` header parser (Docs/design/18-effects.md §18.7, Docs/tasks/T-203.md item 1).

namespace xt::compile {

namespace {

std::string_view trimXtfxParse(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

/// Strips a trailing `// comment` (a `//` outside any quotes; header values have no strings).
std::string_view stripComment(std::string_view s) {
    size_t pos = s.find("//");
    return pos == std::string_view::npos ? s : s.substr(0, pos);
}

std::vector<std::string_view> splitWords(std::string_view s) {
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
            ++i;
        }
        size_t start = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t') {
            ++i;
        }
        if (i > start) {
            out.push_back(s.substr(start, i - start));
        }
    }
    return out;
}

bool parseFloatWord(std::string_view w, float& out) {
    // T-272: a CSS number (exponent included, no hexadecimal float / inf / nan), read as double as
    // before; beyond float it is not finite either.
    double v = 0;
    size_t length = 0;
    if (!css_number::read(w, v, length) || !(std::fabs(v) <= std::numeric_limits<float>::max())) {
        return false;
    }
    std::string_view rest = trimXtfxParse(w.substr(length));
    if (!rest.empty() && rest != "px") {
        return false;
    }
    out = static_cast<float>(v);
    return true;
}

int hexNibbleXtfxParse(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/// `#rgb`, `#rgba`, `#rrggbb`, `#rrggbbaa`.
bool parseHexColor(std::string_view w, float out[4]) {
    if (w.size() < 2 || w[0] != '#') {
        return false;
    }
    std::string_view h = w.substr(1);
    int v[8];
    for (size_t i = 0; i < h.size() && i < 8; ++i) {
        v[i] = hexNibbleXtfxParse(h[i]);
        if (v[i] < 0) {
            return false;
        }
    }
    auto byte = [](int hi, int lo) { return static_cast<float>(hi * 16 + lo) / 255.0f; };
    switch (h.size()) {
        case 3:
            out[0] = byte(v[0], v[0]);
            out[1] = byte(v[1], v[1]);
            out[2] = byte(v[2], v[2]);
            out[3] = 1.0f;
            return true;
        case 4:
            out[0] = byte(v[0], v[0]);
            out[1] = byte(v[1], v[1]);
            out[2] = byte(v[2], v[2]);
            out[3] = byte(v[3], v[3]);
            return true;
        case 6:
            out[0] = byte(v[0], v[1]);
            out[1] = byte(v[2], v[3]);
            out[2] = byte(v[4], v[5]);
            out[3] = 1.0f;
            return true;
        case 8:
            out[0] = byte(v[0], v[1]);
            out[1] = byte(v[2], v[3]);
            out[2] = byte(v[4], v[5]);
            out[3] = byte(v[6], v[7]);
            return true;
        default:
            return false;
    }
}

bool isIdentifier(std::string_view w) {
    if (w.empty() || !(std::isalpha(static_cast<unsigned char>(w[0])) || w[0] == '_')) {
        return false;
    }
    for (char c : w) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) {
            return false;
        }
    }
    return true;
}

bool failXtfxParse(XtfxError& err, std::string message, int line, int col = 1) {
    err.message = std::move(message);
    err.line = line;
    err.col = col;
    return false;
}

/// `param <name> <float|vec2|color> <default...> [<min>, <max>]`
bool parseParamLine(std::string_view rest, int line, XtfxHeader& out, XtfxError& err) {
    // Split off the optional `[min, max]`.
    std::string_view range;
    size_t lb = rest.find('[');
    if (lb != std::string_view::npos) {
        size_t rb = rest.find(']', lb);
        if (rb == std::string_view::npos) {
            return failXtfxParse(err, "param: unterminated range '['", line);
        }
        range = rest.substr(lb + 1, rb - lb - 1);
        if (!trimXtfxParse(rest.substr(rb + 1)).empty()) {
            return failXtfxParse(err, "param: unexpected text after the range", line);
        }
        rest = rest.substr(0, lb);
    }
    std::vector<std::string_view> words = splitWords(rest);
    if (words.size() < 3) {
        return failXtfxParse(err, "param: expected 'param <name> <float|vec2|color> <default>'", line);
    }
    XtfxParam p;
    p.name = std::string(words[0]);
    if (!isIdentifier(p.name)) {
        return failXtfxParse(err, "param: '" + p.name + "' is not an identifier", line);
    }
    for (const XtfxParam& existing : out.params) {
        if (existing.name == p.name) {
            return failXtfxParse(err, "param: duplicate parameter '" + p.name + "'", line);
        }
    }
    if (p.name == "xtTime" || p.name == "xtSample" || p.name == "xtMain" || p.name == "XtInput") {
        return failXtfxParse(err, "param: '" + p.name + "' is reserved", line);
    }
    std::string_view type = words[1];
    size_t needed = 0;
    if (type == "float") {
        p.type = 0;
        needed = 1;
    } else if (type == "vec2" || type == "float2") {
        p.type = 1;
        needed = 2;
    } else if (type == "color") {
        p.type = 2;
        needed = 1;
    } else {
        return failXtfxParse(err, "param: unknown type '" + std::string(type) + "' (float, vec2 or color)",
                             line);
    }
    if (words.size() != 2 + needed) {
        return failXtfxParse(err, "param '" + p.name + "': wrong number of default values", line);
    }
    if (p.type == 2) {
        if (!parseHexColor(words[2], p.def)) {
            return failXtfxParse(err, "param '" + p.name + "': color default must be a #hex color", line);
        }
    } else {
        for (size_t i = 0; i < needed; ++i) {
            if (!parseFloatWord(words[2 + i], p.def[i])) {
                return failXtfxParse(err, "param '" + p.name + "': default must be a number", line);
            }
        }
    }
    if (lb != std::string_view::npos) {
        if (p.type == 2) {
            return failXtfxParse(err, "param '" + p.name + "': a color takes no range", line);
        }
        size_t comma = range.find(',');
        if (comma == std::string_view::npos ||
            !parseFloatWord(trimXtfxParse(range.substr(0, comma)), p.min) ||
            !parseFloatWord(trimXtfxParse(range.substr(comma + 1)), p.max)) {
            return failXtfxParse(err, "param '" + p.name + "': range must be '[min, max]'", line);
        }
        if (!(p.min < p.max)) {
            return failXtfxParse(err, "param '" + p.name + "': range min must be below max", line);
        }
        p.hasRange = true;
    }
    out.params.push_back(std::move(p));
    return true;
}

bool parseFallback(std::string_view value, int line, XtfxHeader& out, XtfxError& err) {
    value = trimXtfxParse(value);
    if (value.empty()) {
        return failXtfxParse(err, "fallback: expected an effect function or 'none'", line);
    }
    if (value == "none") {
        out.fallback = EffectDeclEntry{};
        return true;
    }
    std::vector<EffectDeclEntry> entries;
    DiagnosticSink sink;
    if (parseEffectList(value, entries, sink) != Status::kOk || entries.size() != 1) {
        std::string why =
            sink.all().empty() ? std::string("not one built-in effect function") : sink.all().front().message;
        return failXtfxParse(err, "fallback: " + why, line);
    }
    if (entries.front().kind == EffectKind::Custom) {
        return failXtfxParse(err, "fallback: must be a built-in effect, not custom()", line);
    }
    out.fallback = std::move(entries.front());
    return true;
}

}  // namespace

bool parseXtfxHeader(std::string_view text, XtfxHeader& out, std::string& body, int& bodyLine,
                     XtfxError& err) {
    out = XtfxHeader{};
    body.clear();
    bodyLine = 0;
    err = XtfxError{};
    // Skip a UTF-8 BOM.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }
    int line = 0;
    size_t pos = 0;
    bool sawName = false;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string_view raw =
            text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        ++line;
        size_t next = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
        std::string_view l = trimXtfxParse(stripComment(raw));
        if (l == "---") {
            if (next <= text.size()) {
                body.assign(text.substr(next));
            }
            bodyLine = line + 1;
            // T-203: every parameter needs a slot of the effect's EffectInstance.
            std::vector<uint8_t> types, slots(out.params.size());
            for (const XtfxParam& p : out.params) {
                types.push_back(p.type);
            }
            if (!customParamSlots(types.data(), types.size(), slots.data())) {
                return failXtfxParse(
                    err, "header: too many parameters (at most 8 float components and 2 colors)", line);
            }
            for (size_t i = 0; i < slots.size(); ++i) {
                out.params[i].slot = slots[i];
            }
            return true;
        }
        if (!l.empty()) {
            if (l.rfind("param ", 0) == 0 || l.rfind("param\t", 0) == 0) {
                if (!parseParamLine(l.substr(6), line, out, err)) {
                    return false;
                }
            } else {
                size_t colon = l.find(':');
                if (colon == std::string_view::npos) {
                    return failXtfxParse(err, "header: expected '<key>: <value>', 'param ...' or '---'",
                                         line);
                }
                std::string_view key = trimXtfxParse(l.substr(0, colon));
                std::string_view value = trimXtfxParse(l.substr(colon + 1));
                if (key == "name") {
                    if (sawName) {
                        return failXtfxParse(err, "header: duplicate 'name:'", line);
                    }
                    sawName = true;
                    out.name = std::string(value);
                    if (out.name.empty()) {
                        return failXtfxParse(err, "name: must not be empty", line);
                    }
                } else if (key == "fallback") {
                    if (!parseFallback(value, line, out, err)) {
                        return false;
                    }
                } else if (key == "expand") {
                    float px = 0;
                    if (!parseFloatWord(value, px) || px < 0) {
                        return failXtfxParse(err, "expand: expected a non-negative length in px", line);
                    }
                    out.expandPx = px;
                } else {
                    return failXtfxParse(err, "header: unknown key '" + std::string(key) + "'", line);
                }
            }
        }
        if (nl == std::string_view::npos) {
            break;
        }
        pos = next;
    }
    return failXtfxParse(err, "header: missing '---' line", line);
}

bool xtfxParamIndex(std::span<const XtfxParam> params, std::string_view name, uint32_t& paramIndex) {
    if (name == "enabled") {
        paramIndex = prop::kEffectParamEnabled;
        return true;
    }
    for (const XtfxParam& p : params) {
        if (p.name != name) {
            continue;
        }
        if (p.type == 0) {
            paramIndex = 1u + p.slot;
            return true;
        }
        if (p.type == 2) {
            paramIndex = prop::kEffectParamColor0 + p.slot;
            return true;
        }
        if (p.slot == 0) {
            paramIndex = prop::kEffectParamOffset;
            return true;
        }
        return false;
    }
    return false;
}

void applyXtfxDefaults(std::span<const XtfxParam> params, float outParams[8], float outColors[2][4]) {
    for (const XtfxParam& p : params) {
        if (p.type == 2) {
            for (int k = 0; k < 4; ++k) {
                outColors[p.slot][k] = p.def[k];
            }
        } else {
            outParams[p.slot] = p.def[0];
            if (p.type == 1) {
                outParams[p.slot + 1] = p.def[1];
            }
        }
    }
}

XtfxResult compileXtfx(std::string_view text) {
    XtfxResult r;
    int bodyLine = 0;
    if (!parseXtfxHeader(text, r.header, r.source, bodyLine, r.error)) {
        return r;
    }
    if (!translateHlslSubset(r.source, bodyLine, r.header.params, r.glsl, r.error)) {
        return r;
    }
    r.ok = true;
    return r;
}

}  // namespace xt::compile
