#include "yaml_lite.h"

#include <cstdint>
#include <cstdlib>
#include <utility>
#include <vector>

#include "xt/charconv.h"

/// See yaml_lite.h. Two passes: the text is split into logical lines (comments and trailing
/// whitespace removed, blank lines dropped, indentation measured), then a recursive block parser
/// walks them by indentation and hands every inline value to a flow/scalar parser.

namespace xt::bundle::detail {

using nlohmann::json;

namespace {

/// One non-blank line after comment stripping.
struct Line {
    /// 1-based source line.
    int number = 0;
    /// Leading spaces.
    int indent = 0;
    /// The content after the indentation, without comment or trailing whitespace.
    std::string text;
};

bool setFailure(std::string* error, int line, const std::string& message) {
    if (error != nullptr) {
        *error = "line " + std::to_string(line) + ": " + message;
    }
    return false;
}

/// T-257: the deepest collection nesting either parser accepts ("nesting too deep" past it).
constexpr int kMaxNesting = 64;

/// Counts one level of nesting for the lifetime of the scope.
class NestingScope {
public:
    explicit NestingScope(int& depth) : m_depth(depth) { ++m_depth; }
    ~NestingScope() { --m_depth; }
    NestingScope(const NestingScope&) = delete;
    NestingScope& operator=(const NestingScope&) = delete;
    bool tooDeep() const { return m_depth > kMaxNesting; }

private:
    int& m_depth;
};

bool isSpace(char c) { return c == ' ' || c == '\t'; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && isSpace(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (isSpace(s.back()) || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

/// True when a quote at `i` opens a quoted scalar (it starts a token) rather than being an
/// apostrophe inside a plain scalar.
bool opensQuote(std::string_view s, size_t i) {
    if (s[i] != '"' && s[i] != '\'') {
        return false;
    }
    if (i == 0) {
        return true;
    }
    const char prev = s[i - 1];
    return prev == ' ' || prev == '[' || prev == '{' || prev == ',' || prev == ':';
}

/// Skips the quoted scalar starting at `i` (which opensQuote()); returns the index just past its
/// closing quote, or npos when it is unterminated.
size_t skipQuoted(std::string_view s, size_t i) {
    const char quote = s[i];
    for (size_t j = i + 1; j < s.size(); ++j) {
        if (quote == '"' && s[j] == '\\') {
            ++j;
            continue;
        }
        if (s[j] == quote) {
            if (quote == '\'' && j + 1 < s.size() && s[j + 1] == '\'') {
                ++j;
                continue;
            }
            return j + 1;
        }
    }
    return std::string_view::npos;
}

/// `s` without a `#` comment (one at the start or after whitespace, outside quotes).
std::string_view stripComment(std::string_view s) {
    for (size_t i = 0; i < s.size(); ++i) {
        if (opensQuote(s, i)) {
            const size_t end = skipQuoted(s, i);
            if (end == std::string_view::npos) {
                break;
            }
            i = end - 1;
            continue;
        }
        if (s[i] == '#' && (i == 0 || isSpace(s[i - 1]))) {
            return s.substr(0, i);
        }
    }
    return s;
}

/// The index of the `:` that ends a block mapping key in `s` (followed by a space or the end of
/// the line, outside quotes and flow brackets), or npos.
size_t findKeyColon(std::string_view s) {
    int depth = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (opensQuote(s, i)) {
            const size_t end = skipQuoted(s, i);
            if (end == std::string_view::npos) {
                return std::string_view::npos;
            }
            i = end - 1;
            continue;
        }
        const char c = s[i];
        if (c == '[' || c == '{') {
            ++depth;
        } else if (c == ']' || c == '}') {
            --depth;
        } else if (c == ':' && depth == 0 && (i + 1 == s.size() || s[i + 1] == ' ')) {
            return i;
        }
    }
    return std::string_view::npos;
}

bool isSeqItem(std::string_view text) {
    return text == "-" || (text.size() >= 2 && text[0] == '-' && text[1] == ' ');
}

/// A plain scalar's typed value (see yaml_lite.h).
json plainScalar(std::string_view raw) {
    const std::string_view s = trim(raw);
    if (s.empty() || s == "~" || s == "null") {
        return json(nullptr);
    }
    if (s == "true") {
        return json(true);
    }
    if (s == "false") {
        return json(false);
    }
    size_t i = 0;
    if (s[i] == '-' || s[i] == '+') {
        ++i;
    }
    size_t digits = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        ++i;
        ++digits;
    }
    bool isFloat = false;
    if (i < s.size() && s[i] == '.') {
        isFloat = true;
        ++i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            ++i;
            ++digits;
        }
    }
    if (digits > 0 && i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        size_t j = i + 1;
        if (j < s.size() && (s[j] == '-' || s[j] == '+')) {
            ++j;
        }
        const size_t expStart = j;
        while (j < s.size() && s[j] >= '0' && s[j] <= '9') {
            ++j;
        }
        if (j > expStart) {
            isFloat = true;
            i = j;
        }
    }
    const std::string text(s);
    if (digits > 0 && i == s.size()) {
        if (isFloat) {
            // T-268: through xt::fromChars, which takes no '+' (strtod did).
            const size_t first = text[0] == '+' ? 1 : 0;
            double value = 0;
            xt::fromChars(text.data() + first, text.data() + text.size(), value);
            return json(value);
        }
        return json(static_cast<int64_t>(std::strtoll(text.c_str(), nullptr, 10)));
    }
    return json(text);
}

/// Decodes the quoted scalar starting at `s[i]` into `out`; returns the index past it, or npos.
size_t parseQuoted(std::string_view s, size_t i, std::string& out) {
    const char quote = s[i];
    out.clear();
    for (size_t j = i + 1; j < s.size(); ++j) {
        const char c = s[j];
        if (quote == '"' && c == '\\' && j + 1 < s.size()) {
            const char e = s[++j];
            switch (e) {
                case 'n':
                    out += '\n';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'r':
                    out += '\r';
                    break;
                default:
                    out += e;  // \" \\ and anything else: the character itself.
                    break;
            }
            continue;
        }
        if (c == quote) {
            if (quote == '\'' && j + 1 < s.size() && s[j + 1] == '\'') {
                out += '\'';
                ++j;
                continue;
            }
            return j + 1;
        }
        out += c;
    }
    return std::string_view::npos;
}

/// The single-line inline value parser: flow collections, quoted and plain scalars.
class InlineParser {
public:
    InlineParser(std::string_view text, int line, std::string* error)
        : m_s(text), m_line(line), m_error(error) {}

    /// Parses the whole text as one value.
    bool parseAll(json& out) {
        if (!value(out, /*inFlow=*/false)) {
            return false;
        }
        skipSpaces();
        if (m_pos != m_s.size()) {
            return setFailure(m_error, m_line, "unexpected '" + std::string(m_s.substr(m_pos)) + "'");
        }
        return true;
    }

private:
    void skipSpaces() {
        while (m_pos < m_s.size() && isSpace(m_s[m_pos])) {
            ++m_pos;
        }
    }

    bool value(json& out, bool inFlow) {
        skipSpaces();
        if (m_pos >= m_s.size()) {
            out = json(nullptr);
            return true;
        }
        const char c = m_s[m_pos];
        if (c == '[') {
            return sequence(out);
        }
        if (c == '{') {
            return mapping(out);
        }
        if (c == '"' || c == '\'') {
            std::string text;
            const size_t end = parseQuoted(m_s, m_pos, text);
            if (end == std::string_view::npos) {
                return setFailure(m_error, m_line, "unterminated quoted scalar");
            }
            m_pos = end;
            out = json(text);
            return true;
        }
        if (c == '&' || c == '*' || c == '!' || c == '|' || c == '>') {
            return setFailure(m_error, m_line, std::string("unsupported YAML syntax '") + c + "'");
        }
        const size_t start = m_pos;
        if (inFlow) {
            while (m_pos < m_s.size() && m_s[m_pos] != ',' && m_s[m_pos] != ']' && m_s[m_pos] != '}') {
                ++m_pos;
            }
        } else {
            m_pos = m_s.size();
        }
        out = plainScalar(m_s.substr(start, m_pos - start));
        return true;
    }

    bool sequence(json& out) {
        const NestingScope nesting(m_depth);
        if (nesting.tooDeep()) {
            return setFailure(m_error, m_line, "nesting too deep");
        }
        out = json::array();
        ++m_pos;  // '['
        skipSpaces();
        if (m_pos < m_s.size() && m_s[m_pos] == ']') {
            ++m_pos;
            return true;
        }
        while (true) {
            json item;
            if (!value(item, /*inFlow=*/true)) {
                return false;
            }
            out.push_back(std::move(item));
            skipSpaces();
            if (m_pos >= m_s.size()) {
                return setFailure(m_error, m_line, "unterminated '['");
            }
            if (m_s[m_pos] == ',') {
                ++m_pos;
                continue;
            }
            if (m_s[m_pos] == ']') {
                ++m_pos;
                return true;
            }
            return setFailure(m_error, m_line, "expected ',' or ']'");
        }
    }

    bool mapping(json& out) {
        const NestingScope nesting(m_depth);
        if (nesting.tooDeep()) {
            return setFailure(m_error, m_line, "nesting too deep");
        }
        out = json::object();
        ++m_pos;  // '{'
        skipSpaces();
        if (m_pos < m_s.size() && m_s[m_pos] == '}') {
            ++m_pos;
            return true;
        }
        while (true) {
            skipSpaces();
            std::string key;
            if (m_pos < m_s.size() && (m_s[m_pos] == '"' || m_s[m_pos] == '\'')) {
                const size_t end = parseQuoted(m_s, m_pos, key);
                if (end == std::string_view::npos) {
                    return setFailure(m_error, m_line, "unterminated quoted key");
                }
                m_pos = end;
            } else {
                const size_t start = m_pos;
                while (m_pos < m_s.size() && m_s[m_pos] != ':' && m_s[m_pos] != ',' && m_s[m_pos] != '}') {
                    ++m_pos;
                }
                key = std::string(trim(m_s.substr(start, m_pos - start)));
            }
            skipSpaces();
            if (m_pos >= m_s.size() || m_s[m_pos] != ':') {
                return setFailure(m_error, m_line, "expected ':' after key '" + key + "'");
            }
            ++m_pos;
            json item;
            if (!value(item, /*inFlow=*/true)) {
                return false;
            }
            out[key] = std::move(item);
            skipSpaces();
            if (m_pos >= m_s.size()) {
                return setFailure(m_error, m_line, "unterminated '{'");
            }
            if (m_s[m_pos] == ',') {
                ++m_pos;
                continue;
            }
            if (m_s[m_pos] == '}') {
                ++m_pos;
                return true;
            }
            return setFailure(m_error, m_line, "expected ',' or '}'");
        }
    }

    std::string_view m_s;
    size_t m_pos = 0;
    int m_line = 0;
    std::string* m_error = nullptr;
    int m_depth = 0;  // open flow collections (T-257)
};

/// The indentation-driven block parser.
class BlockParser {
public:
    BlockParser(std::vector<Line>& lines, std::string* error) : m_lines(lines), m_error(error) {}

    bool parseDocument(json& out) {
        if (m_lines.empty()) {
            out = json(nullptr);
            return true;
        }
        if (!node(m_lines[0].indent, out)) {
            return false;
        }
        if (m_i < m_lines.size()) {
            return setFailure(m_error, m_lines[m_i].number, "unexpected indentation");
        }
        return true;
    }

private:
    bool node(int indent, json& out) {
        if (isSeqItem(m_lines[m_i].text)) {
            return sequence(indent, out);
        }
        const Line& line = m_lines[m_i];
        if (findKeyColon(line.text) == std::string::npos) {
            // A lone scalar or flow collection (e.g. a document that is just `[a, b]`).
            ++m_i;
            InlineParser parser(line.text, line.number, m_error);
            return parser.parseAll(out);
        }
        return mapping(indent, out);
    }

    // T-257: every block recursion passes through mapping() or sequence(), so both count depth.
    bool mapping(int indent, json& out) {
        const NestingScope nesting(m_depth);
        if (nesting.tooDeep()) {
            return setFailure(m_error, m_lines[m_i].number, "nesting too deep");
        }
        out = json::object();
        while (m_i < m_lines.size()) {
            const Line line = m_lines[m_i];
            if (line.indent < indent) {
                break;
            }
            if (line.indent > indent) {
                return setFailure(m_error, line.number, "unexpected indentation");
            }
            if (isSeqItem(line.text)) {
                return setFailure(m_error, line.number, "expected 'key: value', found a list item");
            }
            const size_t colon = findKeyColon(line.text);
            if (colon == std::string::npos) {
                return setFailure(m_error, line.number, "expected 'key: value'");
            }
            const std::string_view rawKey = trim(std::string_view(line.text).substr(0, colon));
            std::string key;
            if (!rawKey.empty() && (rawKey[0] == '"' || rawKey[0] == '\'')) {
                if (parseQuoted(rawKey, 0, key) == std::string_view::npos) {
                    return setFailure(m_error, line.number, "unterminated quoted key");
                }
            } else {
                key = std::string(rawKey);
            }
            const std::string_view rest = trim(std::string_view(line.text).substr(colon + 1));
            ++m_i;
            json value(nullptr);
            if (rest.empty()) {
                if (m_i < m_lines.size() && m_lines[m_i].indent > indent) {
                    if (!node(m_lines[m_i].indent, value)) {
                        return false;
                    }
                } else if (m_i < m_lines.size() && m_lines[m_i].indent == indent &&
                           isSeqItem(m_lines[m_i].text)) {
                    if (!sequence(indent, value)) {
                        return false;
                    }
                }
            } else {
                InlineParser parser(rest, line.number, m_error);
                if (!parser.parseAll(value)) {
                    return false;
                }
            }
            if (out.contains(key)) {
                return setFailure(m_error, line.number, "duplicate key '" + key + "'");
            }
            out[key] = std::move(value);
        }
        return true;
    }

    bool sequence(int indent, json& out) {
        const NestingScope nesting(m_depth);
        if (nesting.tooDeep()) {
            return setFailure(m_error, m_lines[m_i].number, "nesting too deep");
        }
        out = json::array();
        while (m_i < m_lines.size()) {
            Line& line = m_lines[m_i];
            if (line.indent < indent || !isSeqItem(line.text)) {
                break;
            }
            if (line.indent > indent) {
                return setFailure(m_error, line.number, "unexpected indentation");
            }
            size_t offset = 1;
            while (offset < line.text.size() && line.text[offset] == ' ') {
                ++offset;
            }
            const std::string item = line.text.substr(offset);
            json value(nullptr);
            if (item.empty()) {
                ++m_i;
                if (m_i < m_lines.size() && m_lines[m_i].indent > indent) {
                    if (!node(m_lines[m_i].indent, value)) {
                        return false;
                    }
                }
            } else if (findKeyColon(item) != std::string::npos) {
                // `- key: value` opens a mapping whose keys sit at the item's column.
                line.indent = indent + static_cast<int>(offset);
                line.text = item;
                if (!mapping(line.indent, value)) {
                    return false;
                }
            } else {
                const int number = line.number;
                ++m_i;
                InlineParser parser(item, number, m_error);
                if (!parser.parseAll(value)) {
                    return false;
                }
            }
            out.push_back(std::move(value));
        }
        return true;
    }

    std::vector<Line>& m_lines;
    std::string* m_error = nullptr;
    size_t m_i = 0;
    int m_depth = 0;  // open block collections (T-257)
};

}  // namespace

bool parseYamlLite(std::string_view text, json& out, std::string* error) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }
    std::vector<Line> lines;
    int number = 0;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view raw = text.substr(start, end - start);
        ++number;
        start = end + 1;
        if (!raw.empty() && raw.back() == '\r') {
            raw.remove_suffix(1);
        }
        if (raw == "---" || raw == "...") {
            continue;
        }
        size_t indent = 0;
        while (indent < raw.size() && raw[indent] == ' ') {
            ++indent;
        }
        const std::string_view content = trim(stripComment(raw.substr(indent)));
        if (content.empty()) {
            continue;
        }
        if (indent < raw.size() && raw[indent] == '\t') {
            return setFailure(error, number, "tab in indentation");
        }
        lines.push_back(Line{number, static_cast<int>(indent), std::string(content)});
    }
    BlockParser parser(lines, error);
    json result;
    if (!parser.parseDocument(result)) {
        return false;
    }
    out = std::move(result);
    return true;
}

std::string yamlScalar(std::string_view text) {
    bool plain = !text.empty() && !isSpace(text.front()) && !isSpace(text.back());
    if (plain) {
        const std::string_view special = "-?:,[]{}#&*!|>'\"%@`";
        plain = special.find(text.front()) == std::string_view::npos;
    }
    if (plain) {
        for (size_t i = 0; i < text.size(); ++i) {
            const auto c = static_cast<unsigned char>(text[i]);
            if (c < 0x20 || (text[i] == ':' && (i + 1 == text.size() || text[i + 1] == ' ')) ||
                (text[i] == '#' && i > 0 && text[i - 1] == ' ')) {
                plain = false;
                break;
            }
        }
    }
    if (plain) {
        const json typed = plainScalar(text);
        plain = typed.is_string() && typed.get<std::string>() == text;
    }
    if (plain) {
        return std::string(text);
    }
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\r':
                out += "\\r";
                break;
            default:
                out += c;
                break;
        }
    }
    out += '"';
    return out;
}

// --- T-305: the block writer ---

namespace {

using nlohmann::ordered_json;

std::string doubleQuoted(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\r':
                out += "\\r";
                break;
            default:
                out += c;
                break;
        }
    }
    out += '"';
    return out;
}

bool isScalarValue(const ordered_json& v) { return !v.is_object() && !v.is_array(); }

bool allScalars(const ordered_json& array) {
    for (const ordered_json& item : array) {
        if (!isScalarValue(item)) {
            return false;
        }
    }
    return true;
}

std::string scalarText(const ordered_json& v, bool inFlow) {
    if (v.is_string()) {
        const std::string& s = v.get_ref<const std::string&>();
        std::string text = yamlScalar(s);
        if (inFlow && !text.empty() && text[0] != '"' && s.find_first_of(",[]{}") != std::string::npos) {
            text = doubleQuoted(s);
        }
        return text;
    }
    if (v.is_null()) {
        return "null";
    }
    if (v.is_boolean()) {
        return v.get<bool>() ? "true" : "false";
    }
    return v.dump();  // numbers: integers as is, floats in their shortest round-trip form
}

/// A mapping key: plain when it is made of ASCII letters, digits and `-_./` (so `--color-accent`
/// stays plain), quoted through yamlScalar() otherwise.
std::string keyText(const std::string& key) {
    bool plain = !key.empty() && key != "-";
    for (const char c : key) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '-' || c == '_' || c == '.' || c == '/';
        if (!ok) {
            plain = false;
            break;
        }
    }
    return plain ? key : yamlScalar(key);
}

std::string flowLine(const ordered_json& array) {
    std::string out = "[";
    bool first = true;
    for (const ordered_json& item : array) {
        if (!first) {
            out += ", ";
        }
        first = false;
        out += scalarText(item, /*inFlow=*/true);
    }
    out += "]";
    return out;
}

void emitMapping(std::string& out, const ordered_json& object, int indent);
void emitSequence(std::string& out, const ordered_json& array, int indent);

/// The rest of a `key:` (or `- key:`) line whose key sits at column `keyIndent`.
void emitAfterKey(std::string& out, const ordered_json& value, int keyIndent) {
    if (value.is_object()) {
        if (value.empty()) {
            out += " {}\n";
            return;
        }
        out += "\n";
        emitMapping(out, value, keyIndent + 2);
        return;
    }
    if (value.is_array()) {
        if (value.empty() || allScalars(value)) {
            out += " " + flowLine(value) + "\n";
            return;
        }
        out += "\n";
        emitSequence(out, value, keyIndent + 2);
        return;
    }
    out += " " + scalarText(value, /*inFlow=*/false) + "\n";
}

void emitMapping(std::string& out, const ordered_json& object, int indent) {
    for (auto it = object.begin(); it != object.end(); ++it) {
        out += std::string(static_cast<size_t>(indent), ' ') + keyText(it.key()) + ":";
        emitAfterKey(out, it.value(), indent);
    }
}

void emitSequence(std::string& out, const ordered_json& array, int indent) {
    const std::string pad(static_cast<size_t>(indent), ' ');
    for (const ordered_json& item : array) {
        if (item.is_object()) {
            if (item.empty()) {
                out += pad + "- {}\n";
                continue;
            }
            bool first = true;
            for (auto it = item.begin(); it != item.end(); ++it) {
                out += first ? pad + "- " : pad + "  ";
                first = false;
                out += keyText(it.key()) + ":";
                emitAfterKey(out, it.value(), indent + 2);
            }
        } else if (item.is_array()) {
            if (item.empty() || allScalars(item)) {
                out += pad + "- " + flowLine(item) + "\n";
            } else {
                out += pad + "-\n";
                emitSequence(out, item, indent + 2);
            }
        } else {
            out += pad + "- " + scalarText(item, /*inFlow=*/false) + "\n";
        }
    }
}

}  // namespace

ordered_json toOrdered(const json& value) {
    if (value.is_object()) {
        ordered_json out = ordered_json::object();
        for (auto it = value.begin(); it != value.end(); ++it) {
            out[it.key()] = toOrdered(it.value());
        }
        return out;
    }
    if (value.is_array()) {
        ordered_json out = ordered_json::array();
        for (const json& item : value) {
            out.push_back(toOrdered(item));
        }
        return out;
    }
    if (value.is_string()) {
        return ordered_json(value.get<std::string>());
    }
    if (value.is_boolean()) {
        return ordered_json(value.get<bool>());
    }
    if (value.is_number_unsigned()) {
        return ordered_json(value.get<uint64_t>());
    }
    if (value.is_number_integer()) {
        return ordered_json(value.get<int64_t>());
    }
    if (value.is_number_float()) {
        return ordered_json(value.get<double>());
    }
    return ordered_json(nullptr);
}

std::string emitYamlLite(const nlohmann::ordered_json& value, int indent) {
    std::string out;
    if (value.is_object()) {
        if (value.empty()) {
            return std::string(static_cast<size_t>(indent), ' ') + "{}\n";
        }
        emitMapping(out, value, indent);
    } else if (value.is_array()) {
        if (value.empty() || allScalars(value)) {
            return std::string(static_cast<size_t>(indent), ' ') + flowLine(value) + "\n";
        }
        emitSequence(out, value, indent);
    } else {
        out = std::string(static_cast<size_t>(indent), ' ') + scalarText(value, /*inFlow=*/false) + "\n";
    }
    return out;
}

std::string emitYamlLite(const nlohmann::json& value, int indent) {
    return emitYamlLite(toOrdered(value), indent);
}

}  // namespace xt::bundle::detail
