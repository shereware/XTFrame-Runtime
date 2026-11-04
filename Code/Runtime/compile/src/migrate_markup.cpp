#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "migrate_v2.h"

/// T-589 (Docs/design/54-source-format-v2.md §54.10 steps 2, 4 and 6) on element markup: start
/// tag scanning, text edits, re-indentation, the editor's id rule and static simple-selector
/// matching. See migrate_v2.h.

namespace xt::compile::migrate {

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isDigit(char c) { return c >= '0' && c <= '9'; }

bool attrValueIs(const DomNode& node, std::string_view name, std::string_view value) {
    const DomAttr* a = node.attr(name);
    return a != nullptr && a->value == value;
}

/// Document::isValidName(): a letter, then letters, digits, `_` or `-`, not ending in `-`.
bool isValidName(std::string_view name) {
    if (name.empty() || !isAlpha(name[0]) || name.back() == '-') {
        return false;
    }
    return std::all_of(name.begin(), name.end(),
                       [](char c) { return isAlpha(c) || isDigit(c) || c == '_' || c == '-'; });
}

/// document_names.cpp numbered(): `<prefix>_<1 + the highest N of any used <prefix>_N>`.
std::string numbered(const std::set<std::string>& used, const std::string& prefix) {
    unsigned long best = 0;
    for (const std::string& id : used) {
        if (id.size() > prefix.size() + 1 && id.compare(0, prefix.size(), prefix) == 0 &&
            id[prefix.size()] == '_') {
            const std::string digits = id.substr(prefix.size() + 1);
            if (!digits.empty() && digits.size() < 9 && std::all_of(digits.begin(), digits.end(), isDigit)) {
                best = std::max(best, std::stoul(digits));
            }
        }
    }
    return prefix + "_" + std::to_string(best + 1);
}

/// document_names.cpp nameFromText(): first 24 code points, non-alphanumerics to `_` (runs
/// collapsed, trimmed), `Text_` before a leading digit; empty when nothing usable remains.
std::string nameFromText(std::string_view text) {
    std::string out;
    size_t points = 0;
    for (size_t i = 0; i < text.size() && points < 24; ++points) {
        const auto c = static_cast<unsigned char>(text[i]);
        const size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        const char mapped = (len == 1 && (isAlpha(text[i]) || isDigit(text[i]))) ? text[i] : '_';
        if (!(mapped == '_' && !out.empty() && out.back() == '_')) {
            out += mapped;
        }
        i += len;
    }
    while (!out.empty() && out.front() == '_') {
        out.erase(out.begin());
    }
    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }
    if (out.empty()) {
        return out;
    }
    if (!isAlpha(out[0])) {
        out = "Text_" + out;
    }
    return out;
}

/// The element's own `display` from its `style` attribute ("" when not written).
std::string inlineDisplay(const DomNode& node) {
    const DomAttr* style = node.attr("style");
    std::string display;
    if (style == nullptr) {
        return display;
    }
    for (const std::string& decl : splitDeclarations(style->value)) {
        const size_t colon = decl.find(':');
        if (colon != std::string::npos && decl.substr(0, colon).find("display") != std::string::npos) {
            std::string v = decl.substr(colon + 1);
            v.erase(0, v.find_first_not_of(" \t"));
            display = v;
        }
    }
    return display;
}

/// The role prefix Document::autoName() would use (its rule-derived roles read only the element's
/// own `style` attribute here).
std::string rolePrefix(const DomTree& tree, const DomNode& node, bool& text) {
    text = false;
    const std::string& tag = node.tag;
    if (tag == "use") {
        const DomAttr* symbol = node.attr("symbol");
        return symbol != nullptr && isValidName(symbol->value) ? symbol->value : std::string("Instance");
    }
    if (tag == "camera") {
        return "Camera";
    }
    if (tag == "anim" || tag == "video" || tag == "sequence") {
        return "Clip";
    }
    if (tag == "repeat") {
        return "Repeat";
    }
    if (tag == "particles") {
        return "Particles";
    }
    if (tag == "img") {
        const DomAttr* src = node.attr("src");
        return src != nullptr && src->value.rfind("engine://", 0) == 0 ? "External" : "Image";
    }
    if (tag == "svg") {
        return "Vector";
    }
    if (tag == "button") {
        return "Button";
    }
    if (tag == "input") {
        return "Input";
    }
    bool hasElements = false;
    for (uint32_t child : node.children) {
        hasElements = hasElements || !tree.nodes[child].tag.empty();
    }
    if (tag == "span" || tag == "p" || (!hasElements && node.hasText && !node.text.empty())) {
        text = true;
        return "Text";
    }
    if (hasElements) {
        const std::string display = inlineDisplay(node);
        if (display == "flex" || display == "grid" || display == "inline-flex") {
            return "Stack";
        }
    }
    return "Group";
}

/// One compound of a simple selector.
struct Compound {
    std::string tag;
    std::string id;
    std::vector<std::string> classes;
    bool child = false;  ///< Joined to the next (outer) compound by `>`.
};

bool parseSimpleSelector(std::string_view selector, std::vector<Compound>& out) {
    std::vector<Compound> compounds;
    size_t i = 0;
    bool pendingChild = false;
    while (i < selector.size()) {
        while (i < selector.size() && isSpace(selector[i])) {
            ++i;
        }
        if (i < selector.size() && selector[i] == '>') {
            pendingChild = true;
            ++i;
            continue;
        }
        if (i >= selector.size()) {
            break;
        }
        Compound c;
        while (i < selector.size() && !isSpace(selector[i]) && selector[i] != '>') {
            const char kind = selector[i];
            if (kind == ':' || kind == '[' || kind == '/' || kind == '(') {
                return false;
            }
            const bool marker = kind == '#' || kind == '.';
            size_t end = marker ? i + 1 : i;
            while (end < selector.size() &&
                   (isAlpha(selector[end]) || isDigit(selector[end]) || selector[end] == '-' ||
                    selector[end] == '_' || selector[end] == '*')) {
                ++end;
            }
            const std::string word(selector.substr(marker ? i + 1 : i, end - (marker ? i + 1 : i)));
            if (word.empty()) {
                return false;
            }
            if (kind == '#') {
                c.id = word;
            } else if (kind == '.') {
                c.classes.push_back(word);
            } else if (word != "*") {
                c.tag = word;
            }
            i = end;
        }
        if (!compounds.empty()) {
            compounds.back().child = pendingChild;
        }
        pendingChild = false;
        compounds.push_back(std::move(c));
    }
    if (compounds.empty()) {
        return false;
    }
    // Subject first; an outer compound's `child` says it must be the direct parent.
    std::reverse(compounds.begin(), compounds.end());
    out = std::move(compounds);
    return true;
}

bool hasClassWord(const DomNode& node, const std::string& cls) {
    const DomAttr* a = node.attr("class");
    if (a == nullptr) {
        return false;
    }
    size_t pos = 0;
    const std::string& list = a->value;
    while (pos < list.size()) {
        while (pos < list.size() && isSpace(list[pos])) {
            ++pos;
        }
        size_t end = pos;
        while (end < list.size() && !isSpace(list[end])) {
            ++end;
        }
        if (end > pos && list.compare(pos, end - pos, cls) == 0) {
            return true;
        }
        pos = end;
    }
    return false;
}

bool compoundMatches(const Compound& c, const DomNode& node) {
    if (!c.tag.empty() && c.tag != node.tag) {
        return false;
    }
    if (!c.id.empty() && !attrValueIs(node, "id", c.id)) {
        return false;
    }
    return std::all_of(c.classes.begin(), c.classes.end(),
                       [&](const std::string& cls) { return hasClassWord(node, cls); });
}

bool matchFrom(const DomTree& tree, const std::vector<Compound>& chain, size_t idx, uint32_t node) {
    if (!compoundMatches(chain[idx], tree.nodes[node])) {
        return false;
    }
    if (idx + 1 == chain.size()) {
        return true;
    }
    for (uint32_t p = tree.nodes[node].parent; p != 0xFFFFFFFFu; p = tree.nodes[p].parent) {
        if (matchFrom(tree, chain, idx + 1, p)) {
            return true;
        }
        if (chain[idx + 1].child) {
            return false;
        }
    }
    return false;
}

}  // namespace

bool scanStartTag(std::string_view text, size_t lt, std::vector<TagAttr>& attrs, size_t& nameEnd,
                  size_t& close) {
    attrs.clear();
    if (lt >= text.size() || text[lt] != '<') {
        return false;
    }
    size_t i = lt + 1;
    while (i < text.size() && !isSpace(text[i]) && text[i] != '>' && text[i] != '/') {
        ++i;
    }
    nameEnd = i;
    for (;;) {
        const size_t wsBegin = i;
        while (i < text.size() && isSpace(text[i])) {
            ++i;
        }
        if (i >= text.size()) {
            return false;
        }
        if (text[i] == '>' || (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '>')) {
            close = i;
            return true;
        }
        TagAttr attr;
        attr.begin = wsBegin;
        const size_t nameBegin = i;
        while (i < text.size() && !isSpace(text[i]) && text[i] != '=' && text[i] != '>' &&
               !(text[i] == '/' && i + 1 < text.size() && text[i + 1] == '>')) {
            ++i;
        }
        attr.name = std::string(text.substr(nameBegin, i - nameBegin));
        size_t j = i;
        while (j < text.size() && isSpace(text[j])) {
            ++j;
        }
        if (j < text.size() && text[j] == '=') {
            ++j;
            while (j < text.size() && isSpace(text[j])) {
                ++j;
            }
            if (j < text.size() && (text[j] == '"' || text[j] == '\'')) {
                const size_t closeQuote = text.find(text[j], j + 1);
                if (closeQuote == std::string_view::npos) {
                    return false;
                }
                i = closeQuote + 1;
            } else {
                while (j < text.size() && !isSpace(text[j]) && text[j] != '>') {
                    ++j;
                }
                i = j;
            }
        }
        attr.end = i;
        if (attr.name.empty()) {
            return false;
        }
        attrs.push_back(std::move(attr));
    }
}

std::string applyEdits(std::string_view text, size_t begin, size_t end, std::vector<TextEdit> edits) {
    // Insertions before a replacement at the same offset, otherwise the order given.
    std::stable_sort(edits.begin(), edits.end(), [](const TextEdit& a, const TextEdit& b) {
        return a.pos != b.pos ? a.pos < b.pos : (a.length == 0 && b.length != 0);
    });
    std::string out;
    size_t cursor = begin;
    for (const TextEdit& e : edits) {
        if (e.pos < cursor || e.pos > end || e.pos + e.length > end) {
            continue;
        }
        out.append(text.substr(cursor, e.pos - cursor));
        out += e.text;
        cursor = e.pos + e.length;
    }
    out.append(text.substr(cursor, end - cursor));
    return out;
}

std::string reindent(std::string_view text, int delta) {
    if (delta == 0) {
        return std::string(text);
    }
    std::string out;
    size_t start = 0;
    bool first = true;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        const std::string_view line =
            text.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
        if (first) {
            out.append(line);
            first = false;
        } else if (delta > 0) {
            out.append(line.empty() ? 0 : static_cast<size_t>(delta), ' ');
            out.append(line);
        } else {
            size_t drop = 0;
            while (drop < line.size() && drop < static_cast<size_t>(-delta) && line[drop] == ' ') {
                ++drop;
            }
            out.append(line.substr(drop));
        }
        if (nl == std::string_view::npos) {
            break;
        }
        out.push_back('\n');
        start = nl + 1;
    }
    return out;
}

int indentAt(std::string_view text, size_t pos) {
    size_t lineStart = pos;
    while (lineStart > 0 && text[lineStart - 1] != '\n') {
        --lineStart;
    }
    int col = 0;
    for (size_t i = lineStart; i < pos; ++i) {
        if (text[i] != ' ' && text[i] != '\t') {
            return col;  // Not at the start of its line: no indentation to speak of.
        }
        ++col;
    }
    return col;
}

std::string escapeAttr(std::string_view value) {
    std::string out;
    for (char c : value) {
        if (c == '&') {
            out += "&amp;";
        } else if (c == '"') {
            out += "&quot;";
        } else if (c == '<') {
            out += "&lt;";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string autoElementId(const DomTree& tree, uint32_t node, const std::set<std::string>& used) {
    const DomNode& n = tree.nodes[node];
    bool text = false;
    const std::string prefix = rolePrefix(tree, n, text);
    if (text) {
        const std::string base = nameFromText(n.text);
        if (!base.empty()) {
            if (used.count(base) == 0) {
                return base;
            }
            for (unsigned k = 2;; ++k) {
                const std::string candidate = base + "_" + std::to_string(k);
                if (used.count(candidate) == 0) {
                    return candidate;
                }
            }
        }
    }
    return numbered(used, prefix);
}

bool matchSimpleSelector(const DomTree& tree, uint32_t node, std::string_view selector, bool& ok) {
    std::vector<Compound> chain;
    ok = parseSimpleSelector(selector, chain);
    return ok && matchFrom(tree, chain, 0, node);
}

}  // namespace xt::compile::migrate
