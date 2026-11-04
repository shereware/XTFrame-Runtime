#include "ports_section.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "xt/compile/diagnostics.h"

/// T-587 (Docs/design/54-source-format-v2.md §54.4): the `<ports>` grammar. See ports_section.h.

namespace xt::compile {

namespace {

bool isPortsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

bool isPortsNameChar(char c, bool first) {
    const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    return first ? alpha : alpha || (c >= '0' && c <= '9');
}

std::string trimPorts(std::string_view s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && isPortsSpace(s[b])) {
        ++b;
    }
    while (e > b && isPortsSpace(s[e - 1])) {
        --e;
    }
    return std::string(s.substr(b, e - b));
}

/// `text` with every comment replaced by spaces (newlines kept, so offsets and lines stay put);
/// each comment that is alone on its line(s) goes to `standalone` as (first line index, text).
std::string blankPortsComments(std::string_view text, std::vector<std::pair<int, std::string>>& standalone) {
    std::string out(text);
    char quote = 0;
    int lineIndex = 0;
    size_t lineStart = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\n') {
            ++lineIndex;
            lineStart = i + 1;
            continue;
        }
        if (quote != 0) {
            if (c == '\\' && i + 1 < text.size()) {
                ++i;
            } else if (c == quote) {
                quote = 0;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            continue;
        }
        const bool line = c == '/' && i + 1 < text.size() && text[i + 1] == '/';
        const bool block = c == '/' && i + 1 < text.size() && text[i + 1] == '*';
        if (!line && !block) {
            continue;
        }
        size_t end = line ? text.find('\n', i) : text.find("*/", i + 2);
        end = end == std::string_view::npos ? text.size() : (line ? end : end + 2);
        bool alone = trimPorts(text.substr(lineStart, i - lineStart)).empty();
        const size_t after = text.find('\n', end);
        alone = alone &&
                trimPorts(text.substr(end, (after == std::string_view::npos ? text.size() : after) - end))
                    .empty();
        if (alone) {
            standalone.emplace_back(lineIndex, std::string(text.substr(i, end - i)));
        }
        for (size_t k = i; k < end; ++k) {
            if (out[k] != '\n') {
                out[k] = ' ';
            } else {
                ++lineIndex;
                lineStart = k + 1;
            }
        }
        i = end - 1;
    }
    return out;
}

/// Folds whitespace runs outside quotes to one space and trims.
std::string foldPortsValue(std::string_view s) {
    std::string out;
    char quote = 0;
    bool pending = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (quote == 0 && isPortsSpace(c)) {
            pending = !out.empty();
            continue;
        }
        if (pending) {
            out.push_back(' ');
            pending = false;
        }
        out.push_back(c);
        if (quote != 0) {
            if (c == '\\' && i + 1 < s.size()) {
                out.push_back(s[++i]);
            } else if (c == quote) {
                quote = 0;
            }
        } else if (c == '"' || c == '\'') {
            quote = c;
        }
    }
    return out;
}

/// Bracket depth change of `c` outside quotes (`<{([` open, `>})]` close).
int portsDepthDelta(char c) {
    if (c == '<' || c == '{' || c == '(' || c == '[') {
        return 1;
    }
    if (c == '>' || c == '}' || c == ')' || c == ']') {
        return -1;
    }
    return 0;
}

/// The position of the last top-level space of `s` (outside brackets and quotes), or npos; and,
/// through `eq`, the first top-level '=' (or npos).
size_t lastTopSpace(std::string_view s, size_t* eq) {
    size_t last = std::string_view::npos;
    int depth = 0;
    char quote = 0;
    if (eq != nullptr) {
        *eq = std::string_view::npos;
    }
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (quote != 0) {
            if (c == '\\') {
                ++i;
            } else if (c == quote) {
                quote = 0;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
        } else if (depth == 0 && c == ' ') {
            last = i;
        } else if (depth == 0 && c == '=' && eq != nullptr && *eq == std::string_view::npos) {
            *eq = i;
        }
        depth += portsDepthDelta(c);
        depth = depth < 0 ? 0 : depth;
    }
    return last;
}

}  // namespace

bool parsePortsSection(std::string_view text, int line, int col, std::string_view fileName,
                       DiagnosticSink& sink, std::vector<PortsDecl>& out) {
    std::vector<std::pair<int, std::string>> comments;
    const std::string clean = blankPortsComments(text, comments);
    size_t nextComment = 0;
    std::vector<std::string> pendingComments;
    std::vector<std::string> values;
    bool ok = true;
    int depth = 0;
    int lineIndex = 0;
    size_t pos = 0;
    while (pos <= clean.size()) {
        size_t eol = clean.find('\n', pos);
        eol = eol == std::string::npos ? clean.size() : eol;
        const std::string_view row = std::string_view(clean).substr(pos, eol - pos);
        while (nextComment < comments.size() && comments[nextComment].first <= lineIndex) {
            pendingComments.push_back(comments[nextComment++].second);
        }
        size_t first = 0;
        while (first < row.size() && isPortsSpace(row[first])) {
            ++first;
        }
        size_t rest = first;
        bool starts = false;
        if (depth == 0 && first < row.size() && isPortsNameChar(row[first], true)) {
            size_t n = first + 1;
            while (n < row.size() && isPortsNameChar(row[n], false)) {
                ++n;
            }
            size_t colon = n;
            while (colon < row.size() && (row[colon] == ' ' || row[colon] == '\t')) {
                ++colon;
            }
            if (colon < row.size() && row[colon] == ':') {
                starts = true;
                PortsDecl decl;
                decl.name = std::string(row.substr(first, n - first));
                decl.line = line + lineIndex;
                decl.col = static_cast<int>(first) + (lineIndex == 0 ? col : 1);
                decl.comments = std::move(pendingComments);
                pendingComments.clear();
                out.push_back(std::move(decl));
                values.emplace_back();
                rest = colon + 1;
            }
        }
        if (!starts && first < row.size()) {
            if (out.empty() || depth < 0) {
                sink.error(std::string(fileName), "expected \"<name>: <type>\" in <ports>", line + lineIndex,
                           static_cast<int>(first) + (lineIndex == 0 ? col : 1));
                ok = false;
            }
            pendingComments.clear();  // A comment inside a declaration belongs to no declaration.
        }
        if (!values.empty() && rest < row.size()) {
            values.back() += ' ';
            values.back() += row.substr(rest);
        }
        char quote = 0;
        for (size_t i = rest; i < row.size(); ++i) {
            const char c = row[i];
            if (quote != 0) {
                quote = c == quote ? 0 : quote;
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else {
                depth += portsDepthDelta(c);
            }
        }
        depth = depth < 0 ? 0 : depth;
        ++lineIndex;
        pos = eol + 1;
    }
    for (size_t i = 0; i < out.size(); ++i) {
        std::string value = foldPortsValue(values[i]);
        const size_t space = lastTopSpace(value, nullptr);
        const std::string head =
            space == std::string::npos ? std::string() : trimPorts(value.substr(0, space));
        if (space != std::string::npos && value.substr(space + 1) == "hidden" && !head.empty() &&
            head.back() != '=') {
            out[i].hidden = true;
            value = head;
        } else if (value == "hidden") {
            out[i].hidden = true;
            value.clear();
        }
        out[i].value = std::move(value);
    }
    return ok;
}

void splitPortsValue(std::string_view value, std::string& type, std::string& dir, std::string& defaultText,
                     bool& hasDefault) {
    size_t eq = std::string_view::npos;
    lastTopSpace(value, &eq);
    hasDefault = eq != std::string_view::npos;
    defaultText = hasDefault ? trimPorts(value.substr(eq + 1)) : std::string();
    std::string head = trimPorts(hasDefault ? value.substr(0, eq) : value);
    dir.clear();
    const size_t space = lastTopSpace(head, nullptr);
    if (space != std::string::npos) {
        const std::string last = head.substr(space + 1);
        if (last == "in" || last == "out" || last == "call") {
            dir = last;
            head = trimPorts(std::string_view(head).substr(0, space));
        }
    }
    type = std::move(head);
}

}  // namespace xt::compile
