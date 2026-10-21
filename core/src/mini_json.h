#pragma once

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

/// A small strict JSON reader shared inside xt_core: BindingRuntime::setText() (struct / list
/// ports, T-177) and Document::restoreState() (T-398, 23 §23.ad.1). Moved here from
/// binding_runtime.cpp unchanged. Internal to xt_core (not installed under core/include).

namespace xt::minijson {

// T-177: JsonField is defined after Json; std::vector allows the incomplete element, std::pair does
// not on libstdc++.
struct JsonField;

/// T-177: a JSON value, as setText() reads a struct / list port (`--port items='[{"name":"A"}]'`).
struct Json {
    enum class Kind : uint8_t { Null, Bool, Num, Str, Arr, Obj };
    Kind kind = Kind::Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> items;
    std::vector<JsonField> fields;
};

struct JsonField {
    std::string first;
    Json second;
};

/// A small strict JSON reader (no comments, `\uXXXX` escapes to UTF-8, depth-limited).
class JsonReader {
public:
    explicit JsonReader(std::string_view text) : m_t(text) {}
    bool parse(Json& out) {
        if (!value(out, 0)) {
            return false;
        }
        ws();
        return m_i == m_t.size();
    }

private:
    std::string_view m_t;
    size_t m_i = 0;

    void ws() {
        while (m_i < m_t.size() &&
               (m_t[m_i] == ' ' || m_t[m_i] == '\t' || m_t[m_i] == '\n' || m_t[m_i] == '\r')) {
            ++m_i;
        }
    }
    bool lit(std::string_view word) {
        if (m_t.substr(m_i, word.size()) != word) {
            return false;
        }
        m_i += word.size();
        return true;
    }
    bool string(std::string& out) {
        if (m_i >= m_t.size() || m_t[m_i] != '"') {
            return false;
        }
        ++m_i;
        while (m_i < m_t.size()) {
            char c = m_t[m_i++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (m_i >= m_t.size()) {
                return false;
            }
            char e = m_t[m_i++];
            switch (e) {
                case '"':
                case '\\':
                case '/':
                    out.push_back(e);
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'u': {
                    if (m_i + 4 > m_t.size()) {
                        return false;
                    }
                    uint32_t cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        char h = m_t[m_i++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') {
                            cp |= static_cast<uint32_t>(h - '0');
                        } else if (h >= 'a' && h <= 'f') {
                            cp |= static_cast<uint32_t>(h - 'a' + 10);
                        } else if (h >= 'A' && h <= 'F') {
                            cp |= static_cast<uint32_t>(h - 'A' + 10);
                        } else {
                            return false;
                        }
                    }
                    if (cp < 0x80) {
                        out.push_back(static_cast<char>(cp));
                    } else if (cp < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default:
                    return false;
            }
        }
        return false;
    }
    bool value(Json& out, int depth) {
        if (depth > 8) {
            return false;
        }
        ws();
        if (m_i >= m_t.size()) {
            return false;
        }
        const char c = m_t[m_i];
        if (c == '"') {
            out.kind = Json::Kind::Str;
            return string(out.s);
        }
        if (c == '[') {
            ++m_i;
            out.kind = Json::Kind::Arr;
            ws();
            if (m_i < m_t.size() && m_t[m_i] == ']') {
                ++m_i;
                return true;
            }
            for (;;) {
                Json item;
                if (!value(item, depth + 1)) {
                    return false;
                }
                out.items.push_back(std::move(item));
                ws();
                if (m_i < m_t.size() && m_t[m_i] == ',') {
                    ++m_i;
                    continue;
                }
                if (m_i < m_t.size() && m_t[m_i] == ']') {
                    ++m_i;
                    return true;
                }
                return false;
            }
        }
        if (c == '{') {
            ++m_i;
            out.kind = Json::Kind::Obj;
            ws();
            if (m_i < m_t.size() && m_t[m_i] == '}') {
                ++m_i;
                return true;
            }
            for (;;) {
                ws();
                std::string key;
                if (!string(key)) {
                    return false;
                }
                ws();
                if (m_i >= m_t.size() || m_t[m_i] != ':') {
                    return false;
                }
                ++m_i;
                Json item;
                if (!value(item, depth + 1)) {
                    return false;
                }
                out.fields.push_back(JsonField{std::move(key), std::move(item)});
                ws();
                if (m_i < m_t.size() && m_t[m_i] == ',') {
                    ++m_i;
                    continue;
                }
                if (m_i < m_t.size() && m_t[m_i] == '}') {
                    ++m_i;
                    return true;
                }
                return false;
            }
        }
        if (lit("true")) {
            out.kind = Json::Kind::Bool;
            out.b = true;
            return true;
        }
        if (lit("false")) {
            out.kind = Json::Kind::Bool;
            return true;
        }
        if (lit("null")) {
            return true;
        }
        const size_t start = m_i;
        while (m_i < m_t.size() &&
               (std::isdigit(static_cast<unsigned char>(m_t[m_i])) || m_t[m_i] == '-' || m_t[m_i] == '+' ||
                m_t[m_i] == '.' || m_t[m_i] == 'e' || m_t[m_i] == 'E')) {
            ++m_i;
        }
        if (m_i == start) {
            return false;
        }
        std::string num(m_t.substr(start, m_i - start));
        char* end = nullptr;
        out.n = std::strtod(num.c_str(), &end);
        out.kind = Json::Kind::Num;
        return end == num.c_str() + num.size();
    }
};

}  // namespace xt::minijson
