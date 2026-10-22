#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "bundle_json.h"
#include "color_math.h"
#include "design_internal.h"
#include "xt/bundle/design_color.h"
#include "xt/bundle/design_doc.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/dom.h"
#include "xt/compile/parse_xt.h"
#include "xt/compile/utf8_path.h"
#include "xt/util/text.h"

/// design.audit (Docs/design/35-design-system.md §35.7.1; Docs/tasks/T-309.md item 3): a text scan of
/// the style values of a UI Project. Declarations are read from `.xts` text, `<style>` blocks and
/// `style=` attributes without the compiler's value parser, so a value the compiler would reject is
/// still counted as written.
///
/// What counts:
///   - colours: `#` + 3/4/6/8 hex digits not followed by a name character, `rgb(`/`rgba(` with three
///     or four numeric components (percentages allowed); normalised to lowercase `#rrggbb[aa]`;
///   - lengths: a number written with `px` (neither preceded nor followed by a name character);
///   - fonts: the whole value of a `font-family` declaration that does not use var();
///   - the coverage numerator: each `var(--name` whose variable is a colour or length: its
///     category (tokens.yaml, else syncTokenMeta()'s guess from the name) is color, space, radius,
///     border, shadow or size, or a type variable named `font-size*`; a variable the project does not
///     declare counts too. The text inside var(...) is not scanned for literals.
/// Custom property declarations (`--name: value`) are definitions and are skipped.

namespace xt::bundle::design {

namespace fs = std::filesystem;
using compile::genericPathToUtf8;
using compile::pathFromUtf8;
using compile::pathToUtf8;

namespace {

bool isHexDigit(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

using util::trim;

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

/// Runs of whitespace as one space.
std::string collapseSpace(std::string_view s) {
    std::string out;
    bool space = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            space = true;
            continue;
        }
        if (space && !out.empty()) {
            out += ' ';
        }
        space = false;
        out += c;
    }
    return out;
}

/// One declaration of a stylesheet: property, value and the `#id`s of the innermost enclosing rule
/// whose selector names one.
struct Declaration {
    std::string prop;
    std::string value;
    std::vector<std::string> ids;
};

void addDeclaration(std::string_view text, const std::vector<std::string>& stack,
                    std::vector<Declaration>& out) {
    const size_t colon = text.find(':');
    if (colon == std::string_view::npos || stack.empty()) {
        return;
    }
    Declaration d;
    d.prop = lower(trim(text.substr(0, colon)));
    d.value = trim(text.substr(colon + 1));
    if (d.prop.empty() || d.value.empty()) {
        return;
    }
    for (auto it = stack.rbegin(); it != stack.rend() && d.ids.empty(); ++it) {
        d.ids = detail::preludeIds(*it);
    }
    out.push_back(std::move(d));
}

/// Every declaration inside a rule block of `css` (comments skipped, quotes respected).
std::vector<Declaration> scanDeclarations(std::string_view css) {
    std::vector<Declaration> out;
    std::vector<std::string> stack;
    std::string current;
    size_t i = 0;
    while (i < css.size()) {
        const char c = css[i];
        if (c == '/' && i + 1 < css.size() && css[i + 1] == '*') {
            const size_t end = css.find("*/", i + 2);
            i = end == std::string_view::npos ? css.size() : end + 2;
            continue;
        }
        if (c == '"' || c == '\'') {
            size_t j = i + 1;
            while (j < css.size() && css[j] != c) {
                j += (css[j] == '\\' && j + 1 < css.size()) ? size_t{2} : size_t{1};
            }
            j = std::min(j + 1, css.size());
            current.append(css.substr(i, j - i));
            i = j;
            continue;
        }
        if (c == '{') {
            stack.push_back(current);
            current.clear();
        } else if (c == '}') {
            addDeclaration(current, stack, out);
            if (!stack.empty()) {
                stack.pop_back();
            }
            current.clear();
        } else if (c == ';') {
            addDeclaration(current, stack, out);
            current.clear();
        } else {
            current += c;
        }
        ++i;
    }
    return out;
}

/// The declarations of a `style=` attribute.
std::vector<Declaration> scanStyleAttribute(std::string_view style) {
    std::string wrapped = "x{";
    wrapped.append(style);
    wrapped += '}';
    std::vector<Declaration> out = scanDeclarations(wrapped);
    for (Declaration& d : out) {
        d.ids.clear();
    }
    return out;
}

/// A number at `text[i]` (optional sign, digits, optional fraction): its end, or `i` when none.
size_t numberEnd(std::string_view text, size_t i) {
    size_t j = i;
    if (j < text.size() && (text[j] == '-' || text[j] == '+')) {
        ++j;
    }
    const size_t digits = j;
    while (j < text.size() && text[j] >= '0' && text[j] <= '9') {
        ++j;
    }
    if (j < text.size() && text[j] == '.') {
        ++j;
        while (j < text.size() && text[j] >= '0' && text[j] <= '9') {
            ++j;
        }
    }
    if (j == digits || (j == digits + 1 && text[digits] == '.')) {
        return i;
    }
    return j;
}

/// `rgb(`/`rgba(` arguments as `#rrggbb[aa]`, or empty.
std::string rgbFunctionHex(std::string_view args) {
    std::vector<double> values;
    std::vector<bool> percent;
    size_t i = 0;
    while (i < args.size()) {
        const char c = args[i];
        if (c == ' ' || c == ',' || c == '/' || c == '\t' || c == '\n' || c == '\r') {
            ++i;
            continue;
        }
        const size_t end = numberEnd(args, i);
        if (end == i) {
            return std::string();
        }
        values.push_back(std::strtod(std::string(args.substr(i, end - i)).c_str(), nullptr));
        i = end;
        const bool isPercent = i < args.size() && args[i] == '%';
        percent.push_back(isPercent);
        if (isPercent) {
            ++i;
        }
    }
    if (values.size() != 3 && values.size() != 4) {
        return std::string();
    }
    double rgb[3];
    for (int k = 0; k < 3; ++k) {
        const auto idx = static_cast<size_t>(k);
        rgb[k] = percent[idx] ? values[idx] / 100.0 : values[idx] / 255.0;
    }
    std::string hex = bundle::detail::formatHex(rgb[0], rgb[1], rgb[2]);
    if (values.size() == 4) {
        double a = percent[3] ? values[3] / 100.0 : values[3];
        a = a < 0.0 ? 0.0 : (a > 1.0 ? 1.0 : a);
        const int alpha = static_cast<int>(std::lround(a * 255.0));
        if (alpha != 255) {
            char buffer[4];
            std::snprintf(buffer, sizeof(buffer), "%02x", alpha);
            hex += buffer;
        }
    }
    return hex;
}

/// `16.0` -> `16`, `-4` -> `-4`, `+2` -> `2`.
std::string normalizeNumber(std::string_view text) {
    const double v = std::strtod(std::string(text).c_str(), nullptr);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%g", v);
    return buffer;
}

struct Accumulator {
    int count = 0;
    std::set<std::string> where;
};

class Auditor {
public:
    Auditor(const UiProject& project, const DesignSystem& ds) : m_project(project), m_ds(ds) {
        DesignSystem synced = ds;
        syncTokenMeta(synced, project, nullptr);
        for (const TokenMeta& t : synced.tokens) {
            m_categories[t.base.id] = t.category;
        }
        m_root = pathFromUtf8(project.rootDir);
    }

    void scanUis() {
        // Every stylesheet once, with the UIs that see it.
        std::map<std::string, std::set<std::string>> sheets;  // relative path -> UI ids
        std::map<std::string, std::set<std::string>> uiElements;
        for (const ProjectUi& ui : m_project.uis) {
            const fs::path docPath = (m_root / pathFromUtf8(ui.doc)).lexically_normal();
            std::string text;
            if (!bundle::detail::readFile(pathToUtf8(docPath), text)) {
                continue;
            }
            compile::DiagnosticSink sink;
            compile::DomTree dom;
            if (!compile::parseXt(text, pathToUtf8(docPath), sink, dom)) {
                continue;
            }
            const std::string docRel = relative(docPath);
            std::set<std::string>& elements = uiElements[ui.id];
            std::vector<std::pair<std::string, std::string>> styles;  // (element id, style)
            if (dom.body < dom.nodes.size()) {
                std::vector<uint32_t> stack{dom.body};
                while (!stack.empty()) {
                    const uint32_t index = stack.back();
                    stack.pop_back();
                    const compile::DomNode& n = dom.nodes[index];
                    const compile::DomAttr* id = n.attr("id");
                    if (id != nullptr && !id->value.empty()) {
                        elements.insert(id->value);
                    }
                    if (const compile::DomAttr* style = n.attr("style")) {
                        styles.emplace_back(id != nullptr ? id->value : std::string(), style->value);
                    }
                    for (auto it = n.children.rbegin(); it != n.children.rend(); ++it) {
                        if (*it < dom.nodes.size()) {
                            stack.push_back(*it);
                        }
                    }
                }
            }
            for (const auto& [id, style] : styles) {
                const std::vector<std::string> where{id.empty() ? docRel : ui.id + "#" + id};
                for (const Declaration& d : scanStyleAttribute(style)) {
                    declaration(d, where);
                }
            }
            for (const auto& [css, line] : dom.inlineStyles) {
                (void)line;
                for (const Declaration& d : scanDeclarations(css)) {
                    declaration(d, whereOf(d, {ui.id}, uiElements, docRel));
                }
            }
            const fs::path docDir = docPath.parent_path();
            for (const std::string& href : dom.linkHrefs) {
                const fs::path sheet = (docDir / pathFromUtf8(href)).lexically_normal();
                if (sheet.extension() == ".xts") {
                    sheets[relative(sheet)].insert(ui.id);
                }
            }
        }
        for (const std::string& style : m_project.shared.styles) {
            const fs::path sheet = (m_root / pathFromUtf8(style)).lexically_normal();
            std::set<std::string>& uis = sheets[relative(sheet)];
            for (const ProjectUi& ui : m_project.uis) {
                uis.insert(ui.id);
            }
        }
        for (const auto& [rel, uis] : sheets) {
            std::string css;
            if (!bundle::detail::readFile(pathToUtf8(m_root / pathFromUtf8(rel)), css)) {
                continue;
            }
            for (const Declaration& d : scanDeclarations(css)) {
                declaration(d, whereOf(d, uis, uiElements, rel));
            }
        }
    }

    void scanSymbols() {
        if (m_project.shared.symbols.empty()) {
            return;
        }
        const fs::path dir = m_root / pathFromUtf8(m_project.shared.symbols);
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) {
            return;
        }
        std::vector<fs::path> files;
        for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
            if (ec) {
                break;
            }
            if (it->is_regular_file(ec) && it->path().extension() == ".xt") {
                files.push_back(it->path());
            }
        }
        std::sort(files.begin(), files.end());
        for (const fs::path& file : files) {
            std::string text;
            if (!bundle::detail::readFile(pathToUtf8(file), text)) {
                continue;
            }
            compile::DiagnosticSink sink;
            compile::DomTree dom;
            const std::string rel = relative(file);
            if (!compile::parseXtSymbolLibrary(text, rel, sink, dom)) {
                continue;
            }
            const std::vector<std::string> where{rel};
            for (const compile::DomNode& n : dom.nodes) {
                if (const compile::DomAttr* style = n.attr("style")) {
                    for (const Declaration& d : scanStyleAttribute(style->value)) {
                        declaration(d, where);
                    }
                }
            }
            for (const auto& [css, line] : dom.inlineStyles) {
                (void)line;
                for (const Declaration& d : scanDeclarations(css)) {
                    declaration(d, where);
                }
            }
        }
    }

    void finish(AuditReport& out) const {
        out = AuditReport();
        out.colors = literals(m_colors, true);
        out.sizes = literals(m_sizes, false);
        out.fonts = literals(m_fonts, false);
        const int total = m_tokenUses + m_literalValues;
        out.tokenCoverage = total == 0 ? 0.0f : static_cast<float>(m_tokenUses) / static_cast<float>(total);
        for (size_t i = 0; i < out.colors.size(); ++i) {
            for (size_t j = i + 1; j < out.colors.size(); ++j) {
                const std::string& a = out.colors[i].value;
                const std::string& b = out.colors[j].value;
                if (a.size() != b.size() || (a.size() == 9 && a.compare(7, 2, b, 7, 2) != 0)) {
                    continue;  // only colours of the same alpha are near duplicates
                }
                const float d = deltaE2000(a, b);
                if (d >= 0.0f && d < 3.0f) {
                    out.nearDuplicates.emplace_back(std::min(a, b), std::max(a, b));
                }
            }
        }
        std::sort(out.nearDuplicates.begin(), out.nearDuplicates.end());
    }

private:
    std::string relative(const fs::path& path) const {
        return genericPathToUtf8(path.lexically_normal().lexically_relative(m_root.lexically_normal()));
    }

    /// `<ui>#<id>` for each UI that has an element the rule's selector names, else `file`.
    std::vector<std::string> whereOf(const Declaration& d, const std::set<std::string>& uis,
                                     const std::map<std::string, std::set<std::string>>& elements,
                                     const std::string& file) const {
        std::vector<std::string> out;
        for (const std::string& ui : uis) {
            const auto found = elements.find(ui);
            if (found == elements.end()) {
                continue;
            }
            for (const std::string& id : d.ids) {
                if (found->second.count(id) != 0) {
                    out.push_back(ui + "#" + id);
                }
            }
        }
        if (out.empty()) {
            out.push_back(file);
        }
        return out;
    }

    bool countsAsValue(const std::string& token) const {
        const auto found = m_categories.find(token);
        if (found == m_categories.end()) {
            return true;
        }
        const std::string& c = found->second;
        return c == "color" || c == "space" || c == "radius" || c == "border" || c == "shadow" ||
               c == "size" || (c == "type" && token.rfind("font-size", 0) == 0);
    }

    static void add(std::map<std::string, Accumulator>& map, const std::string& value,
                    const std::vector<std::string>& where) {
        Accumulator& a = map[value];
        ++a.count;
        a.where.insert(where.begin(), where.end());
    }

    void declaration(const Declaration& d, const std::vector<std::string>& where) {
        if (d.prop.rfind("--", 0) == 0) {
            return;
        }
        const std::string_view v = d.value;
        if (d.prop == "font-family") {
            if (v.find("var(") == std::string_view::npos) {
                add(m_fonts, collapseSpace(v), where);
            }
            return;
        }
        size_t i = 0;
        while (i < v.size()) {
            const char c = v[i];
            const bool boundary = i == 0 || !detail::isNameChar(v[i - 1]);
            if (c == 'v' && boundary && v.compare(i, 4, "var(") == 0) {
                size_t j = i + 4;
                while (j < v.size() && (v[j] == ' ' || v[j] == '\t')) {
                    ++j;
                }
                if (j + 1 < v.size() && v[j] == '-' && v[j + 1] == '-') {
                    size_t k = j + 2;
                    while (k < v.size() && detail::isNameChar(v[k])) {
                        ++k;
                    }
                    if (countsAsValue(std::string(v.substr(j + 2, k - j - 2)))) {
                        ++m_tokenUses;
                    }
                }
                // Skip to the matching ')'.
                int depth = 0;
                size_t k = i + 3;
                for (; k < v.size(); ++k) {
                    if (v[k] == '(') {
                        ++depth;
                    } else if (v[k] == ')' && --depth == 0) {
                        break;
                    }
                }
                i = k + 1;
                continue;
            }
            if (c == '#') {
                size_t j = i + 1;
                while (j < v.size() && isHexDigit(v[j])) {
                    ++j;
                }
                const size_t n = j - i - 1;
                if ((n == 3 || n == 4 || n == 6 || n == 8) && (j == v.size() || !detail::isNameChar(v[j]))) {
                    const std::string hex = normalizeHex(v.substr(i, j - i));
                    if (!hex.empty()) {
                        add(m_colors, hex, where);
                        ++m_literalValues;
                    }
                }
                i = std::max(j, i + 1);
                continue;
            }
            if ((c == 'r' || c == 'R') && boundary &&
                (lower(std::string(v.substr(i, 4))) == "rgb(" ||
                 lower(std::string(v.substr(i, 5))) == "rgba(")) {
                const size_t open = v.find('(', i);
                const size_t close = v.find(')', open);
                if (close != std::string_view::npos) {
                    const std::string hex = rgbFunctionHex(v.substr(open + 1, close - open - 1));
                    if (!hex.empty()) {
                        add(m_colors, hex, where);
                        ++m_literalValues;
                    }
                    i = close + 1;
                    continue;
                }
            }
            if (boundary && ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+')) {
                const size_t end = numberEnd(v, i);
                if (end > i) {
                    if (end + 2 <= v.size() && (v[end] == 'p' || v[end] == 'P') &&
                        (v[end + 1] == 'x' || v[end + 1] == 'X') &&
                        (end + 2 == v.size() || !detail::isNameChar(v[end + 2]))) {
                        add(m_sizes, normalizeNumber(v.substr(i, end - i)) + "px", where);
                        ++m_literalValues;
                    }
                    i = end;
                    while (i < v.size() && detail::isNameChar(v[i])) {
                        ++i;
                    }
                    continue;
                }
            }
            ++i;
        }
    }

    std::vector<AuditReport::Literal> literals(const std::map<std::string, Accumulator>& map,
                                               bool colors) const {
        std::vector<AuditReport::Literal> out;
        for (const auto& [value, acc] : map) {
            AuditReport::Literal l;
            l.value = value;
            l.count = acc.count;
            l.where.assign(acc.where.begin(), acc.where.end());
            if (colors) {
                float best = -1.0f;
                for (const Swatch& s : m_ds.swatches) {
                    const float d = deltaE2000(value, s.value);
                    if (d >= 0.0f && (best < 0.0f || d < best)) {
                        best = d;
                        l.nearestSwatch = s.base.id;
                    }
                }
                l.deltaE = best < 0.0f ? 0.0f : best;
            }
            out.push_back(std::move(l));
        }
        std::stable_sort(
            out.begin(), out.end(),
            [](const AuditReport::Literal& a, const AuditReport::Literal& b) { return a.count > b.count; });
        return out;
    }

    const UiProject& m_project;
    const DesignSystem& m_ds;
    fs::path m_root;
    std::map<std::string, std::string> m_categories;
    std::map<std::string, Accumulator> m_colors;
    std::map<std::string, Accumulator> m_sizes;
    std::map<std::string, Accumulator> m_fonts;
    int m_tokenUses = 0;
    int m_literalValues = 0;
};

}  // namespace

Status audit(const UiProject& project, const DesignSystem& ds, std::string_view scope, AuditReport& out) {
    if (project.isUnnamed() || (scope != "uis" && scope != "symbols" && scope != "all")) {
        return Status::kInvalidArgument;
    }
    Auditor auditor(project, ds);
    if (scope != "symbols") {
        auditor.scanUis();
    }
    if (scope != "uis") {
        auditor.scanSymbols();
    }
    auditor.finish(out);
    return Status::kOk;
}

}  // namespace xt::bundle::design
