#include "expressions_section.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "xt/compile/diagnostics.h"

/// T-767 (Docs/design/58-value-functions-and-drivers.md §58.4): the `<expressions>` section's line
/// grammar. See expressions_section.h.

namespace xt::compile {

namespace {

bool isBlank(char c) { return c == ' ' || c == '\t' || c == '\r'; }

/// File positions of the section text's bytes.
class TextPositions {
public:
    TextPositions(std::string_view text, int line, int col) : m_line(line), m_col(col) {
        m_starts.push_back(0);
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\n') {
                m_starts.push_back(i + 1);
            }
        }
    }

    /// 1-based file line of byte `pos`.
    int lineOf(size_t pos) const { return m_line + static_cast<int>(index(pos)); }

    /// 1-based file column of byte `pos` (the first line starts at the section's column).
    int colOf(size_t pos) const {
        const size_t i = index(pos);
        const int base = i == 0 ? m_col : 1;
        return base + static_cast<int>(pos - m_starts[i]);
    }

private:
    std::vector<size_t> m_starts;
    int m_line;
    int m_col;

    size_t index(size_t pos) const {
        const auto it = std::upper_bound(m_starts.begin(), m_starts.end(), pos);
        return static_cast<size_t>(it - m_starts.begin()) - 1;
    }
};

std::string_view trimView(std::string_view s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && isBlank(s[b])) {
        ++b;
    }
    while (e > b && isBlank(s[e - 1])) {
        --e;
    }
    return s.substr(b, e - b);
}

/// The position of the `}` matching the `{` just before `pos`, skipping string literals, comments
/// and nested brackets; npos when the text ends first.
size_t matchingBrace(std::string_view text, size_t pos) {
    int depth = 1;
    char quote = 0;
    for (size_t i = pos; i < text.size(); ++i) {
        const char c = text[i];
        if (quote != 0) {
            if (c == '\\' && i + 1 < text.size()) {
                ++i;
            } else if (c == quote || c == '\n') {
                quote = 0;
            }
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            const size_t eol = text.find('\n', i);
            if (eol == std::string_view::npos) {
                return std::string_view::npos;
            }
            i = eol;
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            const size_t end = text.find("*/", i + 2);
            if (end == std::string_view::npos) {
                return std::string_view::npos;
            }
            i = end + 1;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}' && --depth == 0) {
            return i;
        }
    }
    return std::string_view::npos;
}

std::vector<std::string_view> splitWords(std::string_view s) {
    std::vector<std::string_view> words;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && isBlank(s[i])) {
            ++i;
        }
        const size_t b = i;
        while (i < s.size() && !isBlank(s[i])) {
            ++i;
        }
        if (i > b) {
            words.push_back(s.substr(b, i - b));
        }
    }
    return words;
}

/// Reads the header words (before the `=`) into `entry`; false with `message` when they are not
/// `<timeline> #<element> <row>[.component] [off]`.
bool readHeader(std::string_view header, ExpressionEntry& entry, std::string& message) {
    const std::vector<std::string_view> words = splitWords(header);
    if (words.size() < 3 || words.size() > 4) {
        message = "expected \"<timeline> #<element> <row> = <expression>\"";
        return false;
    }
    if (words[0].front() == '#') {
        message = "the entry starts with the timeline name, not \"" + std::string(words[0]) + "\"";
        return false;
    }
    if (words[1].size() < 2 || words[1].front() != '#') {
        message = "expected an element as #<id>, not \"" + std::string(words[1]) + "\"";
        return false;
    }
    if (words.size() == 4) {
        if (words[3] != "off") {
            message = "expected \"off\" or \"=\" after the row, not \"" + std::string(words[3]) + "\"";
            return false;
        }
        entry.off = true;
    }
    entry.timeline = std::string(words[0]);
    entry.element = std::string(words[1].substr(1));
    const std::string_view row = words[2];
    const size_t dot = row.rfind('.');
    // T-770 (28 §28.1.4): an effect-parameter row `effect.<fx>.<param>`; a fourth part is the
    // component of a vec2 parameter. Checked further by the lowering.
    if (row.substr(0, 7) == "effect." && std::count(row.begin(), row.end(), '.') < 3) {
        entry.property = std::string(row);
    } else if (dot != std::string_view::npos) {
        const std::string_view comp = row.substr(dot + 1);
        if (comp != "x" && comp != "y" && comp != "z") {
            message = "unknown component \"." + std::string(comp) + "\" (use .x, .y or .z)";
            return false;
        }
        entry.property = std::string(row.substr(0, dot));
        entry.component = std::string(comp);
    } else {
        entry.property = std::string(row);
    }
    if (entry.property.empty()) {
        message = "the row name is missing";
        return false;
    }
    return true;
}

}  // namespace

void parseExpressionsSection(std::string_view text, int line, int col, std::string_view fileName,
                             DiagnosticSink& sink, std::vector<ExpressionEntry>& out,
                             std::vector<std::string>* trailingComments) {
    const TextPositions positions(text, line, col);
    std::vector<std::string> comments;
    auto report = [&](size_t pos, const std::string& message) {
        sink.note(std::string(fileName), "<expressions>: " + message + "; the entry is ignored",
                  positions.lineOf(pos), positions.colOf(pos), "expr.entry");
    };
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos) {
            eol = text.size();
        }
        const std::string_view lineText = text.substr(pos, eol - pos);
        size_t first = 0;
        while (first < lineText.size() && isBlank(lineText[first])) {
            ++first;
        }
        const size_t start = pos + first;
        if (first == lineText.size()) {
            pos = eol + 1;
            continue;
        }
        if (lineText.substr(first, 2) == "//") {
            comments.emplace_back(trimView(lineText.substr(first)));
            pos = eol + 1;
            continue;
        }
        ExpressionEntry entry;
        entry.line = positions.lineOf(start);
        entry.col = positions.colOf(start);
        const size_t eq = lineText.find('=', first);
        std::string message;
        if (eq == std::string_view::npos) {
            report(start, "expected \"<timeline> #<element> <row> = <expression>\"");
            comments.clear();
            pos = eol + 1;
            continue;
        }
        if (!readHeader(lineText.substr(first, eq - first), entry, message)) {
            report(start, message);
            comments.clear();
            pos = eol + 1;
            continue;
        }
        size_t valueStart = eq + 1;
        while (valueStart < lineText.size() && isBlank(lineText[valueStart])) {
            ++valueStart;
        }
        size_t next = eol + 1;
        if (valueStart < lineText.size() && lineText[valueStart] == '{') {
            const size_t open = pos + valueStart;
            const size_t close = matchingBrace(text, open + 1);
            if (close == std::string_view::npos) {
                report(open, "the \"{\" of a multi-line expression is not closed");
                comments.clear();
                break;
            }
            size_t closeEol = text.find('\n', close);
            if (closeEol == std::string_view::npos) {
                closeEol = text.size();
            }
            const std::string_view after = trimView(text.substr(close + 1, closeEol - close - 1));
            next = closeEol + 1;
            if (!after.empty() && after.substr(0, 2) != "//") {
                report(close + 1, "unexpected text after the \"}\" of a multi-line expression");
                comments.clear();
                pos = next;
                continue;
            }
            entry.multiline = true;
            entry.text = std::string(text.substr(open + 1, close - open - 1));
            entry.textLine = positions.lineOf(open + 1);
            entry.textCol = positions.colOf(open + 1);
        } else {
            entry.text = std::string(trimView(lineText.substr(valueStart)));
            entry.textLine = positions.lineOf(pos + valueStart);
            entry.textCol = positions.colOf(pos + valueStart);
        }
        if (trimView(entry.text).find_first_not_of(" \t\r\n") == std::string_view::npos) {
            report(start, "the expression is empty");
            comments.clear();
            pos = next;
            continue;
        }
        entry.comments = std::move(comments);
        comments.clear();
        out.push_back(std::move(entry));
        pos = next;
    }
    if (trailingComments != nullptr) {
        *trailingComments = std::move(comments);
    }
}

uint32_t expressionSeed(std::string_view timeline, std::string_view elementPath, std::string_view property,
                        std::string_view component) {
    uint32_t h = 2166136261u;
    auto mix = [&h](std::string_view s) {
        for (char c : s) {
            h ^= static_cast<uint8_t>(c);
            h *= 16777619u;
        }
        h ^= 0u;  // The separator byte.
        h *= 16777619u;
    };
    mix(timeline);
    mix(elementPath);
    mix(property);
    mix(component);
    return h;
}

}  // namespace xt::compile
