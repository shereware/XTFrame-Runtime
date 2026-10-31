#include "xt/bundle/design_doc.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <json.hpp>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "bundle_json.h"
#include "design_internal.h"
#include "xt/bundle/asset_meta.h"
#include "xt/bundle/design_color.h"
#include "xt/bundle/symbol_library.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/dom.h"
#include "xt/compile/parse_xt.h"
#include "xt/compile/parse_xts.h"
#include "xt/compile/style_model.h"
#include "xt/compile/utf8_path.h"
#include "xt/util/text.h"
#include "yaml_lite.h"

/// The `DESIGN.md` generator and the remaining design lint (Docs/design/35-design-system.md §35.4,
/// §35.10, §35.16; Docs/tasks/T-309.md items 1 and 4). See xt/bundle/design_doc.h.
///
/// Layout of the document: the banner, `# <name> design system`, then each visible section as
/// `## <n> · <Title>` followed by its text entries (front matter `section`; a text under
/// `text/screens/` always goes to Screens), its data tables (`### ...` subsections with fixed
/// columns) and, outside the AI version, a `### Deprecated` subsection. A text entry's body keeps
/// its Markdown, with its first `#` heading becoming the entry's `###` heading (`(draft)` appended
/// for a draft) and every other heading a bold line, so the document never goes below `###` (the
/// §35.16 subset has headings 1-3 only). A text entry without a `section` (and not under
/// `text/screens/`) is not placed anywhere.
///
/// T-498c (Docs/design/49-symbol-sets.md §49.12 ruling): the implied default layer -- the preset
/// group at BuildOptions::standardSymbolDir -- is the inherited part of the project's variables.
/// Each Variables table is followed by `### Inherited from <group id>`: the group's tokens the
/// project does not declare, read-only, values from the group's `tokens.xts` and theme files (a
/// project theme of the same name wins), `From` and `Description` from an `inherited:` entry of
/// `tokens.yaml` when there is one, else the first standard symbol that reads the variable. The
/// contrast table and the type scale resolve an inherited variable the same way.

namespace xt::bundle::design {

namespace fs = std::filesystem;
using compile::genericPathToUtf8;
using compile::pathFromUtf8;
using compile::pathToUtf8;
using nlohmann::json;

namespace {

constexpr const char* kSectionTitles[] = {
    "Identity",   "Voice",   "Color",   "Type",      "Space and shape", "Motion", "Layout",
    "Components", "Imagery", "Screens", "Decisions", "Changelog",       "Assets"};
constexpr size_t kSectionCount = sizeof(kSectionTitles) / sizeof(kSectionTitles[0]);

enum SectionIndex : size_t {
    kIdentity,
    kVoice,
    kColor,
    kType,
    kSpace,
    kMotion,
    kLayout,
    kComponents,
    kImagery,
    kScreens,
    kDecisions,
    kChangelog,
    kAssets
};

using Lines = std::vector<std::string>;

std::string joinDesignDoc(const std::vector<std::string>& items, const char* separator) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i != 0) {
            out += separator;
        }
        out += items[i];
    }
    return out;
}

/// A table cell: `|` escaped, line breaks as spaces.
std::string cell(std::string_view text) {
    std::string out;
    for (char c : util::trim(text)) {
        if (c == '|') {
            out += "\\|";
        } else if (c == '\n' || c == '\r') {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

std::string code(std::string_view text) {
    if (text.empty()) {
        return std::string();
    }
    return "`" + std::string(text) + "`";
}

void table(Lines& out, const std::vector<std::string>& headers,
           const std::vector<std::vector<std::string>>& rows) {
    std::string line = "|";
    std::string rule = "|";
    for (const std::string& h : headers) {
        line += " " + cell(h) + " |";
        rule += " --- |";
    }
    out.push_back(line);
    out.push_back(rule);
    for (const std::vector<std::string>& row : rows) {
        std::string r = "|";
        for (size_t i = 0; i < headers.size(); ++i) {
            r += " " + (i < row.size() ? cell(row[i]) : std::string()) + " |";
        }
        out.push_back(r);
    }
}

void blankDesignDoc(Lines& out) {
    if (!out.empty() && !out.back().empty()) {
        out.emplace_back();
    }
}

void headingDesignDoc(Lines& out, const std::string& text) {
    blankDesignDoc(out);
    out.push_back("### " + text);
    out.emplace_back();
}

bool isDraft(const Entry& e) { return detail::statusText(e) == "draft"; }
bool isDeprecated(const Entry& e) {
    return e.status == EntryStatus::Deprecated || detail::statusText(e) == "deprecated";
}
std::string draftMark(const Entry& e) { return isDraft(e) ? " (draft)" : std::string(); }

std::string percentDesignDoc(double fraction) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%g %%", fraction * 100.0);
    return buffer;
}

std::string fixed2(double v) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2f", v);
    return buffer;
}

std::string byteSize(uint64_t bytes) {
    char buffer[48];
    if (bytes < 1024) {
        std::snprintf(buffer, sizeof(buffer), "%llu bytes", static_cast<unsigned long long>(bytes));
    } else if (bytes < 1024ull * 1024ull) {
        std::snprintf(buffer, sizeof(buffer), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    return buffer;
}

/// A JSON value as one line: strings bare, arrays `[a, b]`, objects `{k: v}`.
std::string inlineValue(const json& v) {
    if (v.is_string()) {
        return v.get<std::string>();
    }
    if (v.is_array()) {
        std::vector<std::string> items;
        for (const json& item : v) {
            items.push_back(inlineValue(item));
        }
        return "[" + joinDesignDoc(items, ", ") + "]";
    }
    if (v.is_object()) {
        std::vector<std::string> items;
        for (auto it = v.begin(); it != v.end(); ++it) {
            items.push_back(it.key() + ": " + inlineValue(it.value()));
        }
        return "{" + joinDesignDoc(items, ", ") + "}";
    }
    if (v.is_null()) {
        return "null";
    }
    return v.dump(-1, ' ', false, json::error_handler_t::replace);
}

void flatten(const json& v, const std::string& prefix, std::vector<std::vector<std::string>>& rows) {
    if (v.is_object() && !v.empty()) {
        for (auto it = v.begin(); it != v.end(); ++it) {
            flatten(it.value(), prefix.empty() ? it.key() : prefix + "." + it.key(), rows);
        }
        return;
    }
    rows.push_back({code(prefix), inlineValue(v)});
}

using detail::readTokenDecls;

/// Everything the generator and the lint read from the project besides the design directory.
struct ProjectData {
    fs::path root;
    /// shared.tokens: (name, value) in source order, and by name.
    std::vector<std::pair<std::string, std::string>> tokenOrder;
    std::map<std::string, std::string> tokenValues;
    /// shared.themes/*.xts by file stem: name -> value.
    std::vector<std::string> themeNames;
    std::vector<std::map<std::string, std::string>> themes;
    /// `ds` with syncTokenMeta() applied (every declared variable has metadata).
    DesignSystem synced;
    /// T-498c: the implied default layer (empty `id` without one) and, of its tokens, the ones
    /// `shared.tokens` does not declare: (name, value) in the group's order, and by name.
    detail::ImpliedLayer implied;
    std::vector<std::pair<std::string, std::string>> inheritedOrder;
    std::map<std::string, std::string> inheritedValues;
};

void loadProjectData(const UiProject& project, const DesignSystem& ds, const std::string& groupDir,
                     ProjectData& data) {
    data.root = pathFromUtf8(project.rootDir);
    if (!project.shared.tokens.empty()) {
        data.tokenOrder = readTokenDecls(pathToUtf8(data.root / pathFromUtf8(project.shared.tokens)),
                                         project.shared.tokens);
    }
    for (const auto& [name, value] : data.tokenOrder) {
        data.tokenValues[name] = value;
    }
    for (const auto& [stem, file] : detail::themeFiles(project)) {
        data.themeNames.push_back(stem);
        std::map<std::string, std::string> values;
        for (auto& [name, value] :
             readTokenDecls(pathToUtf8(file), genericPathToUtf8(file.lexically_relative(data.root)))) {
            values[name] = value;
        }
        data.themes.push_back(std::move(values));
    }
    data.synced = ds;
    syncTokenMeta(data.synced, project, nullptr);
    data.implied = detail::readImpliedLayer(groupDir);
    for (const auto& [name, value] : data.implied.tokens) {
        if (data.tokenValues.count(name) == 0 && data.inheritedValues.emplace(name, value).second) {
            data.inheritedOrder.emplace_back(name, value);
        }
    }
}

/// The value of an inherited variable `id` in theme `theme` of the implied layer; empty when the
/// layer has no such theme or the theme does not set it.
std::string impliedThemeValue(const detail::ImpliedLayer& implied, const std::string& theme,
                              const std::string& id) {
    for (const auto& [name, values] : implied.themes) {
        if (name != theme) {
            continue;
        }
        for (const auto& [token, value] : values) {
            if (token == id) {
                return value;
            }
        }
    }
    return std::string();
}

/// A colour reference (`token:--x`, `token:x`, `swatch:x`, `#hex`) as `#hex`, with `theme`
/// overrides (may be null) over the base variables (the project's, else the inherited ones);
/// follows `var(--x)` up to 8 levels. Empty when it does not resolve.
std::string resolveColor(const std::string& ref, const DesignSystem& ds, const ProjectData& data,
                         const std::map<std::string, std::string>* theme) {
    std::string current = util::trim(ref);
    for (int depth = 0; depth < 8; ++depth) {
        if (!current.empty() && current[0] == '#') {
            return normalizeHex(current);
        }
        std::string token;
        if (current.rfind("token:", 0) == 0) {
            token = detail::normalizeRef(current).substr(6);
        } else if (current.rfind("swatch:", 0) == 0) {
            const std::string id = current.substr(7);
            for (const Swatch& s : ds.swatches) {
                if (s.base.id == id) {
                    return normalizeHex(s.value);
                }
            }
            return std::string();
        } else if (current.rfind("var(", 0) == 0) {
            size_t b = 4;
            while (b < current.size() && current[b] == ' ') {
                ++b;
            }
            size_t e = b;
            while (e < current.size() && current[e] != ')' && current[e] != ',' && current[e] != ' ') {
                ++e;
            }
            token = current.substr(b, e - b);
            if (token.rfind("--", 0) == 0) {
                token = token.substr(2);
            }
        } else {
            return std::string();
        }
        if (theme != nullptr) {
            const auto found = theme->find(token);
            if (found != theme->end()) {
                current = found->second;
                continue;
            }
        }
        auto found = data.tokenValues.find(token);
        if (found == data.tokenValues.end()) {
            found = data.inheritedValues.find(token);  // T-498c: the implied default layer.
            if (found == data.inheritedValues.end()) {
                return std::string();
            }
        }
        current = found->second;
    }
    return std::string();
}

/// The token id of a colour reference (`token:--x` / `token:x` -> `x`), else empty.
std::string tokenIdOf(const std::string& ref) {
    if (ref.rfind("token:", 0) != 0) {
        return std::string();
    }
    return detail::normalizeRef(ref).substr(6);
}

struct ContrastPair {
    std::string fg;
    std::string bg;
    double min = 4.5;
};

std::vector<ContrastPair> contrastPairs(const DesignSystem& ds) {
    std::vector<ContrastPair> out;
    if (!ds.paletteExtra.is_object()) {
        return out;
    }
    const auto it = ds.paletteExtra.find("contrast");
    if (it == ds.paletteExtra.end() || !it->is_array()) {
        return out;
    }
    for (const json& item : *it) {
        if (!item.is_object()) {
            continue;
        }
        ContrastPair p;
        if (item.contains("fg") && item["fg"].is_string()) {
            p.fg = item["fg"].get<std::string>();
        }
        if (item.contains("bg") && item["bg"].is_string()) {
            p.bg = item["bg"].get<std::string>();
        }
        if (item.contains("min") && item["min"].is_number()) {
            p.min = item["min"].get<double>();
        }
        if (!p.fg.empty() && !p.bg.empty()) {
            out.push_back(p);
        }
    }
    return out;
}

/// A text entry as document lines (see the file comment).
Lines renderText(const TextEntry& t, bool forAi) {
    Lines out;
    const std::string title = t.title.empty() ? t.base.id : t.title;
    bool titled = false;
    bool fence = false;
    std::string body = t.body;
    size_t pos = 0;
    Lines bodyLines;
    while (pos <= body.size()) {
        const size_t nl = body.find('\n', pos);
        std::string line = body.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        bodyLines.push_back(line);
        if (nl == std::string::npos) {
            break;
        }
        pos = nl + 1;
    }
    for (std::string line : bodyLines) {
        const std::string trimmed = util::trim(line);
        if (trimmed.rfind("```", 0) == 0 || trimmed.rfind("~~~", 0) == 0) {
            fence = !fence;
            out.push_back(line);
            continue;
        }
        if (!fence && !line.empty() && line[0] == '#') {
            size_t level = 0;
            while (level < line.size() && line[level] == '#') {
                ++level;
            }
            if (level <= 6 && (level == line.size() || line[level] == ' ' || line[level] == '\t')) {
                std::string text = util::trim(line.substr(level));
                while (!text.empty() && text.back() == '#') {
                    text.pop_back();
                }
                text = util::trim(text);
                if (level == 1 && !titled) {
                    titled = true;
                    headingDesignDoc(out, text + draftMark(t.base));
                } else if (!text.empty()) {
                    blankDesignDoc(out);
                    out.push_back("**" + text + "**");
                    out.emplace_back();
                }
                continue;
            }
        }
        if (forAi && !fence) {
            // No images in the AI version.
            size_t at;
            while ((at = line.find("![")) != std::string::npos) {
                const size_t mid = line.find("](", at);
                const size_t end = mid == std::string::npos ? std::string::npos : line.find(')', mid);
                if (end == std::string::npos) {
                    break;
                }
                line.erase(at, end - at + 1);
            }
            if (util::trim(line).empty() && !trimmed.empty()) {
                continue;
            }
        }
        if (line.empty()) {
            blankDesignDoc(out);
        } else {
            out.push_back(line);
        }
    }
    if (fence) {
        out.push_back("```");
    }
    if (!titled) {
        Lines titledOut;
        headingDesignDoc(titledOut, title + draftMark(t.base));
        titledOut.insert(titledOut.end(), out.begin(), out.end());
        out = std::move(titledOut);
    }
    return out;
}

/// Section keys accepted in `design.yaml:sections`, BuildOptions::sections and front matter.
int sectionIndexOf(std::string_view key) {
    for (size_t i = 0; i < kSectionCount; ++i) {
        if (key == kDesignSectionKeys[i] || key == std::to_string(i + 1)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

class DocBuilder {
public:
    DocBuilder(const UiProject& project, const DesignSystem& ds, const Graph& graph,
               const BuildOptions& options)
        : m_project(project), m_ds(ds), m_graph(graph), m_options(options) {
        loadProjectData(project, ds, options.standardSymbolDir, m_data);
        m_texts = ds.texts;
        std::sort(m_texts.begin(), m_texts.end(),
                  [](const TextEntry& a, const TextEntry& b) { return a.path < b.path; });
        m_decisions = ds.decisions;
        std::sort(m_decisions.begin(), m_decisions.end(),
                  [](const TextEntry& a, const TextEntry& b) { return a.base.id < b.base.id; });
        // Variables in stylesheet order, then metadata-only ones in tokens.yaml order.
        std::set<std::string> seen;
        for (const auto& [name, value] : m_data.tokenOrder) {
            (void)value;
            for (const TokenMeta& t : m_data.synced.tokens) {
                if (t.base.id == name && seen.insert(name).second) {
                    m_tokens.push_back(&t);
                }
            }
        }
        for (const TokenMeta& t : m_data.synced.tokens) {
            if (t.inherited && m_data.tokenValues.count(t.base.id) == 0) {
                // T-498c: an `inherited:` entry describes a group variable; it is not a project one.
                m_inheritedMeta[t.base.id] = &t;
                continue;
            }
            if (seen.insert(t.base.id).second) {
                m_tokens.push_back(&t);
            }
        }
        // T-498c: the theme columns of the inherited tables -- the project's themes, then the
        // group's that the project does not have.
        m_inheritedThemes = m_data.themeNames;
        for (const auto& [name, values] : m_data.implied.themes) {
            (void)values;
            if (std::find(m_inheritedThemes.begin(), m_inheritedThemes.end(), name) ==
                m_inheritedThemes.end()) {
                m_inheritedThemes.push_back(name);
            }
        }
    }

    std::string build() {
        std::vector<bool> visible(kSectionCount, true);
        if (!m_ds.info.sections.empty()) {
            std::fill(visible.begin(), visible.end(), false);
            for (const std::string& key : m_ds.info.sections) {
                const int i = sectionIndexOf(util::trim(key));
                if (i >= 0) {
                    visible[static_cast<size_t>(i)] = true;
                }
            }
        }
        if (!m_options.sections.empty()) {
            std::vector<bool> wanted(kSectionCount, false);
            for (const std::string& key : m_options.sections) {
                const int i = sectionIndexOf(util::trim(key));
                if (i >= 0) {
                    wanted[static_cast<size_t>(i)] = true;
                }
            }
            for (size_t i = 0; i < kSectionCount; ++i) {
                visible[i] = visible[i] && wanted[i];
            }
        }
        const size_t shown = static_cast<size_t>(std::count(visible.begin(), visible.end(), true));

        std::string out(kDesignDocBanner);
        out += "\n";
        std::string name = m_ds.info.name;
        if (name.empty()) {
            name = m_project.name.empty() ? m_project.id : m_project.name;
        }
        out += "# " + name + " design system\n";
        if (m_options.forAi) {
            out += "\nAI edition: images and deprecated entries are left out; each section is cut to about " +
                   std::to_string(
                       shown == 0 ? 0 : std::max(1, m_options.maxTokensHint) / static_cast<int>(shown)) +
                   " tokens.\n";
        }
        for (size_t i = 0; i < kSectionCount; ++i) {
            if (!visible[i]) {
                continue;
            }
            Lines body = section(i);
            trimBlank(body);
            if (body.empty()) {
                body.push_back("Nothing recorded yet.");
            }
            if (!m_options.forAi && !m_deprecated[i].empty()) {
                headingDesignDoc(body, "Deprecated");
                body.insert(body.end(), m_deprecated[i].begin(), m_deprecated[i].end());
            }
            if (m_options.forAi) {
                const size_t budget =
                    static_cast<size_t>(std::max(1, m_options.maxTokensHint) / static_cast<int>(shown)) * 4;
                truncate(body, budget);
            }
            out += "\n## " + std::to_string(i + 1) + " · " + kSectionTitles[i] + "\n\n";
            for (const std::string& line : body) {
                out += line;
                out += "\n";
            }
        }
        return out;
    }

private:
    static void trimBlank(Lines& lines) {
        while (!lines.empty() && lines.back().empty()) {
            lines.pop_back();
        }
        size_t first = 0;
        while (first < lines.size() && lines[first].empty()) {
            ++first;
        }
        lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(first));
        // Collapse runs of blank lines.
        Lines out;
        for (const std::string& line : lines) {
            if (line.empty() && !out.empty() && out.back().empty()) {
                continue;
            }
            out.push_back(line);
        }
        lines = std::move(out);
    }

    static void truncate(Lines& lines, size_t budget) {
        size_t used = 0;
        size_t keep = 0;
        bool fence = false;
        bool fenceAtCut = false;
        for (; keep < lines.size(); ++keep) {
            if (used + lines[keep].size() + 1 > budget) {
                break;
            }
            used += lines[keep].size() + 1;
            const std::string t = util::trim(lines[keep]);
            if (t.rfind("```", 0) == 0 || t.rfind("~~~", 0) == 0) {
                fence = !fence;
            }
        }
        if (keep >= lines.size()) {
            return;
        }
        fenceAtCut = fence;
        size_t more = 0;
        for (size_t i = keep; i < lines.size(); ++i) {
            if (!util::trim(lines[i]).empty()) {
                ++more;
            }
        }
        lines.resize(keep);
        if (fenceAtCut) {
            lines.push_back("```");
        }
        trimBlank(lines);
        lines.emplace_back();
        lines.push_back("… (" + std::to_string(more) + " more)");
    }

    void deprecated(size_t section, const std::string& label, const std::string& ref) {
        m_deprecated[section].push_back("- " + label + " (" + code(ref) + ")");
    }

    /// Text entries placed in `section`.
    void texts(size_t section, Lines& out) {
        for (const TextEntry& t : m_texts) {
            const bool screen = t.path.rfind("text/screens/", 0) == 0;
            bool here = section == kScreens && screen;
            for (const std::string& key : t.sections) {
                if (sectionIndexOf(util::trim(key)) == static_cast<int>(section) &&
                    !(screen && section == kScreens)) {
                    here = true;
                }
            }
            if (!here) {
                continue;
            }
            if (isDeprecated(t.base)) {
                deprecated(section, t.title.empty() ? t.base.id : t.title,
                           entryRef(EntryKind::Text, t.base.id));
                continue;
            }
            const Lines lines = renderText(t, m_options.forAi);
            blankDesignDoc(out);
            out.insert(out.end(), lines.begin(), lines.end());
        }
    }

    /// The project's value of `id`, else the inherited one (T-498c), else empty.
    std::string tokenValue(const std::string& id) const {
        auto found = m_data.tokenValues.find(id);
        if (found == m_data.tokenValues.end()) {
            found = m_data.inheritedValues.find(id);
            if (found == m_data.inheritedValues.end()) {
                return std::string();
            }
        }
        return found->second;
    }

    /// T-498c (49 §49.12 ruling): `### Inherited from <group id>` -- the implied layer's variables
    /// of these categories (an `inherited:` entry's category, else the name's) that the project does
    /// not declare, in the group's order: value, each theme's value (the project's theme first,
    /// then the group's), `From` and `Description` from the entry, the description falling back to
    /// the first standard symbol that reads the variable. Nothing without an implied layer.
    void inherited(std::initializer_list<const char*> categories, Lines& out) {
        if (m_data.implied.id.empty()) {
            return;
        }
        std::vector<std::vector<std::string>> rows;
        for (const auto& [name, value] : m_data.inheritedOrder) {
            const auto metaIt = m_inheritedMeta.find(name);
            const TokenMeta* meta = metaIt == m_inheritedMeta.end() ? nullptr : metaIt->second;
            const std::string category = meta != nullptr && !meta->category.empty()
                                             ? meta->category
                                             : detail::guessTokenCategory(name);
            bool match = false;
            for (const char* c : categories) {
                match = match || category == c;
            }
            if (!match) {
                continue;
            }
            std::vector<std::string> row{code("--" + name), code(value)};
            for (const std::string& theme : m_inheritedThemes) {
                std::string themeValue;
                for (size_t t = 0; t < m_data.themeNames.size(); ++t) {
                    if (m_data.themeNames[t] == theme) {
                        const auto found = m_data.themes[t].find(name);
                        if (found != m_data.themes[t].end()) {
                            themeValue = found->second;
                        }
                    }
                }
                if (themeValue.empty()) {
                    themeValue = impliedThemeValue(m_data.implied, theme, name);
                }
                row.push_back(code(themeValue));
            }
            row.push_back(meta == nullptr || meta->from.empty() ? std::string() : code(meta->from));
            std::string description = meta == nullptr ? std::string() : meta->description;
            if (description.empty()) {
                for (const auto& [token, symbol] : m_data.implied.firstUser) {
                    if (token == name) {
                        description = "Used by " + code(symbol);
                        break;
                    }
                }
            }
            row.push_back(description);
            rows.push_back(std::move(row));
        }
        if (rows.empty()) {
            return;
        }
        headingDesignDoc(out, "Inherited from " + m_data.implied.id);
        std::vector<std::string> headers{"Variable", "Value"};
        headers.insert(headers.end(), m_inheritedThemes.begin(), m_inheritedThemes.end());
        headers.insert(headers.end(), {"From", "Description"});
        table(out, headers, rows);
    }

    size_t uiCount(const std::string& tokenId) const {
        return usage(m_graph, entryRef(EntryKind::Token, tokenId)).uis.size();
    }

    /// The variables of these categories (deprecated ones go to the section's Deprecated list).
    std::vector<const TokenMeta*> tokensOf(size_t section, std::initializer_list<const char*> categories) {
        std::vector<const TokenMeta*> out;
        for (const TokenMeta* t : m_tokens) {
            bool match = false;
            for (const char* c : categories) {
                if (t->category == c) {
                    match = true;
                }
            }
            if (!match) {
                continue;
            }
            if (isDeprecated(t->base)) {
                deprecated(section, "--" + t->base.id, entryRef(EntryKind::Token, t->base.id));
                continue;
            }
            out.push_back(t);
        }
        return out;
    }

    std::string tokenCell(const TokenMeta& t) const { return code("--" + t.base.id) + draftMark(t.base); }

    Lines section(size_t i) {
        Lines out;
        switch (i) {
            case kIdentity:
                identity(out);
                break;
            case kVoice:
                texts(kVoice, out);
                if (!m_project.spec.languages.empty()) {
                    blankDesignDoc(out);
                    out.push_back("Languages: " + joinDesignDoc(m_project.spec.languages, ", ") + ".");
                }
                break;
            case kColor:
                color(out);
                break;
            case kType:
                type(out);
                break;
            case kSpace:
                space(out);
                break;
            case kMotion:
                motion(out);
                break;
            case kLayout:
                layout(out);
                break;
            case kComponents:
                components(out);
                break;
            case kImagery:
                imagery(out);
                break;
            case kScreens:
                texts(kScreens, out);
                break;
            case kDecisions:
                decisions(out);
                break;
            case kChangelog:
                changelog(out);
                break;
            case kAssets:
                assets(out);
                break;
            default:
                break;
        }
        return out;
    }

    void identity(Lines& out) {
        const DesignInfo& info = m_ds.info;
        if (!m_project.game.empty()) {
            out.push_back("- Game: " + m_project.game);
        }
        out.push_back("- Project: " + (m_project.name.empty() ? m_project.id : m_project.name) +
                      (m_project.id.empty() ? std::string() : " (" + code(m_project.id) + ")"));
        if (!info.name.empty() || !info.status.empty()) {
            out.push_back("- Design system: " + (info.name.empty() ? std::string("(unnamed)") : info.name) +
                          (info.status.empty() ? std::string() : ", status " + info.status));
        }
        if (!info.owners.empty()) {
            out.push_back("- Owners: " + joinDesignDoc(info.owners, ", "));
        }
        if (!info.languages.empty()) {
            out.push_back("- Languages: " + joinDesignDoc(info.languages, ", "));
        }
        if (info.extra.is_object()) {
            const auto description = info.extra.find("description");
            if (description != info.extra.end() && description->is_string()) {
                blankDesignDoc(out);
                out.push_back(util::trim(description->get<std::string>()));
            }
        }
        if (!info.principles.empty()) {
            blankDesignDoc(out);
            out.push_back("Principles:");
            out.emplace_back();
            for (size_t k = 0; k < info.principles.size(); ++k) {
                out.push_back(std::to_string(k + 1) + ". " + info.principles[k]);
            }
        }
        texts(kIdentity, out);
    }

    void color(Lines& out) {
        texts(kColor, out);
        std::map<std::string, const Swatch*> byId;
        for (const Swatch& s : m_ds.swatches) {
            byId[s.base.id] = &s;
        }
        const auto swatchRow = [&](const Swatch& s) {
            size_t variables = 0;
            for (const std::string& e : usage(m_graph, entryRef(EntryKind::Swatch, s.base.id)).entries) {
                if (e.rfind("token:", 0) == 0) {
                    ++variables;
                }
            }
            const std::string label =
                (s.base.name.empty() ? code(s.base.id) : s.base.name + " (" + code(s.base.id) + ")") +
                draftMark(s.base);
            return std::vector<std::string>{label, code(s.value), s.role, std::to_string(variables)};
        };
        std::set<std::string> listed;
        for (const Group& g : m_ds.groups) {
            const std::string groupName =
                g.name.empty() ? (g.base.name.empty() ? g.base.id : g.base.name) : g.name;
            if (isDeprecated(g.base)) {
                deprecated(kColor, groupName, entryRef(EntryKind::Group, g.base.id));
                continue;
            }
            headingDesignDoc(out, groupName + draftMark(g.base));
            if (!g.base.note.empty()) {
                out.push_back(g.base.note);
                out.emplace_back();
            }
            if (!g.scale.base.empty()) {
                out.push_back("Scale: " + code(g.scale.base) + ", " + std::to_string(g.scale.steps) +
                              " steps" + (g.scale.method.empty() ? std::string() : ", " + g.scale.method) +
                              ".");
                out.emplace_back();
            }
            std::vector<std::vector<std::string>> rows;
            for (const std::string& id : g.swatches) {
                const auto found = byId.find(id);
                if (found == byId.end() || !listed.insert(id).second) {
                    continue;
                }
                const Swatch& s = *found->second;
                if (isDeprecated(s.base)) {
                    deprecated(kColor, s.base.name.empty() ? s.base.id : s.base.name,
                               entryRef(EntryKind::Swatch, s.base.id));
                    continue;
                }
                rows.push_back(swatchRow(s));
            }
            table(out, {"Swatch", "Value", "Role", "Variables"}, rows);
        }
        std::vector<std::vector<std::string>> loose;
        for (const Swatch& s : m_ds.swatches) {
            if (listed.count(s.base.id) != 0) {
                continue;
            }
            listed.insert(s.base.id);
            if (isDeprecated(s.base)) {
                deprecated(kColor, s.base.name.empty() ? s.base.id : s.base.name,
                           entryRef(EntryKind::Swatch, s.base.id));
                continue;
            }
            loose.push_back(swatchRow(s));
        }
        if (!loose.empty()) {
            headingDesignDoc(out, "Ungrouped swatches");
            table(out, {"Swatch", "Value", "Role", "Variables"}, loose);
        }

        const std::vector<const TokenMeta*> colors = tokensOf(kColor, {"color"});
        if (!colors.empty()) {
            headingDesignDoc(out, "Variables");
            std::vector<std::string> headers{"Variable", "Value"};
            headers.insert(headers.end(), m_data.themeNames.begin(), m_data.themeNames.end());
            headers.insert(headers.end(), {"From", "Description", "UIs"});
            std::vector<std::vector<std::string>> rows;
            for (const TokenMeta* t : colors) {
                std::vector<std::string> row{tokenCell(*t), code(tokenValue(t->base.id))};
                for (const auto& theme : m_data.themes) {
                    const auto found = theme.find(t->base.id);
                    row.push_back(found == theme.end() ? std::string() : code(found->second));
                }
                row.push_back(t->from.empty() ? std::string() : code(t->from));
                row.push_back(t->description);
                row.push_back(std::to_string(uiCount(t->base.id)));
                rows.push_back(std::move(row));
            }
            table(out, headers, rows);
        }
        inherited({"color"}, out);

        const std::vector<ContrastPair> pairs = contrastPairs(m_ds);
        if (!pairs.empty()) {
            headingDesignDoc(out, "Contrast");
            std::vector<std::vector<std::string>> rows;
            for (const ContrastPair& p : pairs) {
                const std::string fg = resolveColor(p.fg, m_ds, m_data, nullptr);
                const std::string bg = resolveColor(p.bg, m_ds, m_data, nullptr);
                std::string ratio;
                std::string result = "unresolved";
                if (!fg.empty() && !bg.empty()) {
                    const double r = contrastRatio(fg, bg);
                    ratio = fixed2(r);
                    result = r + 1e-9 >= p.min ? "pass" : "fail";
                }
                rows.push_back({code(p.fg), code(p.bg), ratio, fixed2(p.min), result});
            }
            table(out, {"Foreground", "Background", "Ratio", "Minimum", "Result"}, rows);
        }
    }

    void type(Lines& out) {
        texts(kType, out);
        std::vector<std::vector<std::string>> roles;
        for (const TypeRole& r : m_ds.typeRoles) {
            if (isDeprecated(r.base)) {
                deprecated(kType, r.base.name.empty() ? r.base.id : r.base.name,
                           entryRef(EntryKind::TypeRole, r.base.id));
                continue;
            }
            roles.push_back(
                {code(r.base.id) + draftMark(r.base), r.family, joinDesignDoc(r.weights, ", "), r.base.note});
        }
        if (!roles.empty()) {
            headingDesignDoc(out, "Roles");
            table(out, {"Role", "Family", "Weights", "Note"}, roles);
        }
        if (m_ds.typeExtra.is_object()) {
            const auto scale = m_ds.typeExtra.find("scale");
            if (scale != m_ds.typeExtra.end() && scale->is_object() && !scale->empty()) {
                headingDesignDoc(out, "Scale");
                std::vector<std::vector<std::string>> rows;
                for (auto it = scale->begin(); it != scale->end(); ++it) {
                    const std::string variable = it->is_string() ? it->get<std::string>() : inlineValue(*it);
                    const std::string id = variable.rfind("--", 0) == 0 ? variable.substr(2) : variable;
                    rows.push_back({code(it.key()), code(variable), code(tokenValue(id))});
                }
                table(out, {"Step", "Variable", "Value"}, rows);
            }
        }
        const std::vector<const TokenMeta*> vars = tokensOf(kType, {"type"});
        if (!vars.empty()) {
            headingDesignDoc(out, "Variables");
            std::vector<std::vector<std::string>> rows;
            for (const TokenMeta* t : vars) {
                rows.push_back({tokenCell(*t), code(tokenValue(t->base.id)), t->description,
                                std::to_string(uiCount(t->base.id))});
            }
            table(out, {"Variable", "Value", "Description", "UIs"}, rows);
        }
        inherited({"type"}, out);
        if (!m_project.spec.fontFallback.empty()) {
            headingDesignDoc(out, "Font fallback");
            std::vector<std::vector<std::string>> rows;
            for (const FontFallbackInfo& f : m_project.spec.fontFallback) {
                rows.push_back({code(f.lang), joinDesignDoc(f.families, ", ")});
            }
            table(out, {"Language", "Families"}, rows);
        }
        if (m_ds.typeExtra.is_object()) {
            const auto effects = m_ds.typeExtra.find("effects");
            if (effects != m_ds.typeExtra.end() && !effects->empty()) {
                headingDesignDoc(out, "Text effects");
                if (effects->is_object()) {
                    for (auto it = effects->begin(); it != effects->end(); ++it) {
                        out.push_back("- " + code(it.key()));
                    }
                } else if (effects->is_array()) {
                    for (const json& e : *effects) {
                        out.push_back("- " + code(inlineValue(e)));
                    }
                }
            }
        }
    }

    void space(Lines& out) {
        texts(kSpace, out);
        const std::vector<const TokenMeta*> vars =
            tokensOf(kSpace, {"space", "radius", "border", "shadow", "size", "other"});
        if (!vars.empty()) {
            headingDesignDoc(out, "Variables");
            std::vector<std::vector<std::string>> rows;
            for (const TokenMeta* t : vars) {
                rows.push_back({tokenCell(*t), t->category, code(tokenValue(t->base.id)), t->description,
                                std::to_string(uiCount(t->base.id))});
            }
            table(out, {"Variable", "Category", "Value", "Description", "UIs"}, rows);
        }
        inherited({"space", "radius", "border", "shadow", "size", "other"}, out);
    }

    void motion(Lines& out) {
        texts(kMotion, out);
        std::string text;
        if (bundle::detail::readFile(pathToUtf8(m_data.root / "contracts" / "motion-spec.yaml"), text)) {
            json spec;
            if (bundle::detail::parseYamlLite(text, spec, nullptr) && spec.is_object() && !spec.empty()) {
                headingDesignDoc(out, "Motion spec");
                std::vector<std::vector<std::string>> rows;
                flatten(spec, std::string(), rows);
                table(out, {"Key", "Value"}, rows);
            }
        }
        const std::vector<const TokenMeta*> vars = tokensOf(kMotion, {"motion"});
        if (!vars.empty()) {
            headingDesignDoc(out, "Variables");
            std::vector<std::vector<std::string>> rows;
            for (const TokenMeta* t : vars) {
                rows.push_back({tokenCell(*t), code(tokenValue(t->base.id)), t->description});
            }
            table(out, {"Variable", "Value", "Description"}, rows);
        }
        inherited({"motion"}, out);
        // Timeline names of conventions/*.yaml (a `timelines` key: a list, or a mapping by name).
        std::vector<std::vector<std::string>> rows;
        for (const fs::path& file : filesIn(m_data.root / "conventions", ".yaml", false)) {
            std::string yaml;
            json doc;
            if (!bundle::detail::readFile(pathToUtf8(file), yaml) ||
                !bundle::detail::parseYamlLite(yaml, doc, nullptr) || !doc.is_object()) {
                continue;
            }
            const auto timelines = doc.find("timelines");
            if (timelines == doc.end()) {
                continue;
            }
            std::vector<std::string> names;
            if (timelines->is_array()) {
                for (const json& n : *timelines) {
                    names.push_back(code(inlineValue(n)));
                }
            } else if (timelines->is_object()) {
                for (auto it = timelines->begin(); it != timelines->end(); ++it) {
                    names.push_back(code(it.key()));
                }
            }
            rows.push_back({code(relative(file)), joinDesignDoc(names, ", ")});
        }
        if (!rows.empty()) {
            headingDesignDoc(out, "Timeline conventions");
            table(out, {"File", "Timelines"}, rows);
        }
    }

    void layout(Lines& out) {
        texts(kLayout, out);
        const std::vector<fs::path> templates =
            filesIn(m_data.root / "conventions" / "layouts", ".xt", false);
        if (!templates.empty()) {
            headingDesignDoc(out, "Templates");
            std::vector<std::vector<std::string>> rows;
            for (const fs::path& file : templates) {
                std::string text;
                std::string title;
                if (bundle::detail::readFile(pathToUtf8(file), text)) {
                    compile::DiagnosticSink sink;
                    compile::DomTree dom;
                    if (compile::parseXt(text, relative(file), sink, dom)) {
                        title = dom.title;
                    }
                }
                rows.push_back({code(pathToUtf8(file.stem())), code(relative(file)), title});
            }
            table(out, {"Template", "File", "Title"}, rows);
        }
        headingDesignDoc(out, "Safe area");
        out.push_back("- Default: " + percentDesignDoc(m_project.spec.safeAreaDefault) + " of the viewport");
        out.push_back("- Console: " + percentDesignDoc(m_project.spec.safeAreaConsole) + " of the viewport");
    }

    void components(Lines& out) {
        texts(kComponents, out);
        // The standard library's index.json, by symbol name.
        std::vector<const json*> standard;
        std::map<std::string, const json*> byName;
        if (!m_options.standardSymbolDir.empty()) {
            std::string text;
            if (bundle::detail::readFile(bundle::detail::joinPath(m_options.standardSymbolDir, "index.json"),
                                         text)) {
                std::string error;
                m_index = bundle::detail::parseBounded(text, bundle::detail::kMaxJsonDepth, error);
            }
            if (m_index.is_object() && m_index.contains("symbols") && m_index["symbols"].is_array()) {
                for (const json& s : m_index["symbols"]) {
                    if (s.is_object() && s.contains("name") && s["name"].is_string()) {
                        standard.push_back(&s);
                        byName[s["name"].get<std::string>()] = &s;
                    }
                }
            }
        }
        const auto field = [](const json* s, const char* key) {
            if (s == nullptr || !s->contains(key) || !(*s)[key].is_string()) {
                return std::string();
            }
            return (*s)[key].get<std::string>();
        };
        const auto states = [](const json* s) {
            std::vector<std::string> list;
            if (s != nullptr && s->contains("requires") && (*s)["requires"].is_object()) {
                const json& r = (*s)["requires"];
                if (r.contains("states") && r["states"].is_array()) {
                    for (const json& st : r["states"]) {
                        list.push_back(inlineValue(st));
                    }
                }
            }
            return joinDesignDoc(list, ", ");
        };
        const auto ports = [](const json* s) {
            std::vector<std::string> list;
            if (s != nullptr && s->contains("ports") && (*s)["ports"].is_array()) {
                for (const json& p : (*s)["ports"]) {
                    if (!p.is_object() || !p.contains("name")) {
                        continue;
                    }
                    std::string text = inlineValue(p["name"]);
                    std::vector<std::string> parts;
                    if (p.contains("type")) {
                        parts.push_back(inlineValue(p["type"]));
                    }
                    if (p.contains("dir")) {
                        parts.push_back(inlineValue(p["dir"]));
                    }
                    if (!parts.empty()) {
                        text += " (" + joinDesignDoc(parts, ", ") + ")";
                    }
                    list.push_back(text);
                }
            }
            return joinDesignDoc(list, "; ");
        };

        std::vector<std::vector<std::string>> rows;
        if (!m_project.shared.symbols.empty()) {
            for (const fs::path& file :
                 filesIn(m_data.root / pathFromUtf8(m_project.shared.symbols), ".xt", true)) {
                std::string text;
                if (!bundle::detail::readFile(pathToUtf8(file), text)) {
                    continue;
                }
                compile::DiagnosticSink sink;
                compile::DomTree dom;
                if (!compile::parseXtSymbolLibrary(text, relative(file), sink, dom)) {
                    continue;
                }
                for (uint32_t index : dom.symbolDefs) {
                    if (index >= dom.nodes.size()) {
                        continue;
                    }
                    const compile::DomAttr* name = dom.nodes[index].attr("name");
                    if (name == nullptr) {
                        continue;
                    }
                    const auto found = byName.find(name->value);
                    const json* s = found == byName.end() ? nullptr : found->second;
                    rows.push_back({code(name->value), code(relative(file)), field(s, "category"),
                                    field(s, "description"), states(s), ports(s)});
                }
            }
        }
        if (!rows.empty()) {
            headingDesignDoc(out, "Project symbols");
            table(out, {"Symbol", "File", "Category", "Description", "States", "Ports"}, rows);
        }
        if (!standard.empty()) {
            headingDesignDoc(out, "Standard library");
            std::vector<std::vector<std::string>> stdRows;
            for (const json* s : standard) {
                stdRows.push_back({code(field(s, "name")), field(s, "category"), field(s, "description"),
                                   states(s), ports(s)});
            }
            table(out, {"Symbol", "Category", "Description", "States", "Ports"}, stdRows);
        }
    }

    void imagery(Lines& out) {
        texts(kImagery, out);
        std::vector<const ReferenceMeta*> featured;
        std::vector<const ReferenceMeta*> refs;
        for (const ReferenceMeta& r : m_ds.references) {
            refs.push_back(&r);
        }
        std::sort(refs.begin(), refs.end(),
                  [](const ReferenceMeta* a, const ReferenceMeta* b) { return a->base.id < b->base.id; });
        for (const ReferenceMeta* r : refs) {
            if (std::find(r->base.tags.begin(), r->base.tags.end(), "featured") == r->base.tags.end()) {
                continue;
            }
            const std::string title = r->title.empty() ? r->base.id : r->title;
            if (isDeprecated(r->base)) {
                deprecated(kImagery, title, entryRef(EntryKind::Reference, r->base.id));
                continue;
            }
            featured.push_back(r);
        }
        if (featured.empty()) {
            return;
        }
        headingDesignDoc(out, "Featured references");
        for (const ReferenceMeta* r : featured) {
            const std::string title = r->title.empty() ? r->base.id : r->title;
            if (m_options.forAi) {
                out.push_back("- " + title + draftMark(r->base) + " (" + code(r->path) + ")" +
                              (r->base.note.empty() ? std::string() : ": " + r->base.note));
                continue;
            }
            blankDesignDoc(out);
            out.push_back("![" + title + "](" + r->path + ")");
            out.emplace_back();
            out.push_back("**" + title + "**" + draftMark(r->base) +
                          (r->base.note.empty() ? std::string() : ": " + r->base.note));
            out.emplace_back();
        }
    }

    /// The one-sentence summary of a decision: its `note`, else the first paragraph under its
    /// "Decision" heading, else its first paragraph.
    static std::string decisionSummary(const TextEntry& d) {
        if (!d.base.note.empty()) {
            return d.base.note;
        }
        std::string first;
        std::string underDecision;
        bool inDecision = false;
        std::string paragraph;
        std::string body = d.body;
        size_t pos = 0;
        const auto flush = [&]() {
            if (paragraph.empty()) {
                return;
            }
            if (first.empty()) {
                first = paragraph;
            }
            if (inDecision && underDecision.empty()) {
                underDecision = paragraph;
            }
            paragraph.clear();
        };
        while (pos <= body.size()) {
            const size_t nl = body.find('\n', pos);
            const std::string line =
                util::trim(body.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
            if (!line.empty() && line[0] == '#') {
                flush();
                size_t level = 0;
                while (level < line.size() && line[level] == '#') {
                    ++level;
                }
                inDecision = detail::headingSlug(util::trim(line.substr(level))) == "decision";
            } else if (line.empty()) {
                flush();
            } else {
                paragraph += paragraph.empty() ? line : " " + line;
            }
            if (nl == std::string::npos) {
                break;
            }
            pos = nl + 1;
        }
        flush();
        std::string out = underDecision.empty() ? first : underDecision;
        if (out.size() > 200) {
            size_t cut = 200;
            while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
                --cut;
            }
            out = out.substr(0, cut) + "…";
        }
        return out;
    }

    void decisions(Lines& out) {
        texts(kDecisions, out);
        Lines current;
        Lines superseded;
        for (const TextEntry& d : m_decisions) {
            const std::string status = detail::statusText(d.base);
            const std::string title = d.title.empty() ? d.base.id : d.title;
            if (isDeprecated(d.base)) {
                deprecated(kDecisions, d.base.id + " · " + title, entryRef(EntryKind::Decision, d.base.id));
                continue;
            }
            std::string date;
            if (d.base.extra.is_object() && d.base.extra.contains("date") &&
                d.base.extra["date"].is_string()) {
                date = d.base.extra["date"].get<std::string>();
            } else if (!d.base.approved.at.empty()) {
                date = d.base.approved.at;
            } else {
                date = d.base.source.at;
            }
            const std::string summary = decisionSummary(d);
            std::string line = "- **" + d.base.id + " · " + title + "**";
            const bool draft = status == "draft" || status == "proposed";
            if (draft) {
                line += " (draft)";
            }
            std::vector<std::string> facts;
            if (!draft) {
                facts.push_back(status);
            }
            if (!date.empty()) {
                facts.push_back(date);
            }
            if (!facts.empty()) {
                line += " (" + joinDesignDoc(facts, ", ") + ")";
            }
            if (!summary.empty()) {
                line += ": " + summary;
            }
            (status == "superseded" ? superseded : current).push_back(line);
        }
        if (!current.empty()) {
            blankDesignDoc(out);
            out.insert(out.end(), current.begin(), current.end());
        }
        if (!superseded.empty()) {
            headingDesignDoc(out, "Superseded");
            out.insert(out.end(), superseded.begin(), superseded.end());
        }
    }

    void changelog(Lines& out) {
        const std::vector<fs::path> versions =
            filesIn(pathFromUtf8(m_ds.rootDir) / "versions", ".json", false);
        for (const fs::path& v : versions) {
            out.push_back("- " + code(pathToUtf8(v.stem())) + " (" +
                          code("versions/" + pathToUtf8(v.filename())) + ")");
        }
        std::error_code ec;
        const bool history = fs::is_directory(m_data.root / ".xtvc", ec);
        if (history) {
            blankDesignDoc(out);
            out.push_back("History: see the editor's History panel.");
        } else if (versions.empty()) {
            out.push_back("History is not enabled for this project.");
        }
    }

    /// T-312a (35 §35.15.1 item 5, §35.15.3): the collections and the cleanup numbers of
    /// assets::assetHygiene().
    void assets(Lines& out) {
        texts(kAssets, out);
        blankDesignDoc(out);
        std::string assetRoot =
            m_project.shared.assetRoot.empty() ? std::string("shared") : m_project.shared.assetRoot;
        assets::AssetHygiene h;
        if (assets::assetHygiene(m_project, &m_graph, h) != Status::kOk || h.totalAssets == 0) {
            out.push_back("No assets under " + code(assetRoot + "/") + ".");
            return;
        }
        out.push_back("- Assets: " + std::to_string(h.totalAssets) + " (" + byteSize(h.totalBytes) + ")");
        headingDesignDoc(out, "Collections");
        std::vector<std::vector<std::string>> rows;
        for (const assets::CollectionStat& c : h.collections) {
            rows.push_back({code(c.collection.empty() ? c.typeDir : c.typeDir + "/" + c.collection),
                            std::to_string(c.assets), byteSize(c.bytes)});
        }
        table(out, {"Collection", "Assets", "Size"}, rows);
        headingDesignDoc(out, "Cleanup");
        out.push_back("- Unused: " + std::to_string(h.unused));
        out.push_back("- Duplicates: " + std::to_string(h.duplicates));
        if (m_project.spec.budgetAssetBytes > 0) {
            out.push_back("- Oversized: " + std::to_string(h.oversize) + " (over " +
                          byteSize(m_project.spec.budgetAssetBytes) + ")");
        } else {
            out.push_back("- Oversized: not measured (spec.budget.assetBytes is not set)");
        }
        out.push_back("- Missing @2x: " + std::to_string(h.missing2x));
        out.push_back("- Bad nine-slice: " + std::to_string(h.badSlice));
        out.push_back("- Names not in kebab-case: " + std::to_string(h.badName));
        out.push_back("- Placeholders: " + std::to_string(h.placeholders));
        if (!m_project.uis.empty()) {
            headingDesignDoc(out, "Per UI");
            rows.clear();
            for (const ProjectUi& ui : m_project.uis) {
                const auto it = h.bytesPerUi.find(ui.id);
                rows.push_back({code(ui.id), byteSize(it == h.bytesPerUi.end() ? 0 : it->second)});
            }
            table(out, {"UI", "Asset size"}, rows);
        }
    }

    std::string relative(const fs::path& path) const {
        return genericPathToUtf8(path.lexically_normal().lexically_relative(m_data.root.lexically_normal()));
    }

    /// The files with `extension` in `dir` (recursively when asked), sorted; skips `.xtvc`, `.git`,
    /// `.xtcache` and the publish output directory.
    std::vector<fs::path> filesIn(const fs::path& dir, const char* extension, bool recursive) const {
        std::vector<fs::path> out;
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) {
            return out;
        }
        const auto consider = [&](const fs::directory_entry& e) {
            if (e.is_regular_file(ec) && e.path().extension() == extension) {
                out.push_back(e.path());
            }
        };
        if (recursive) {
            for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
                if (ec) {
                    break;
                }
                const std::string name = pathToUtf8(it->path().filename());
                if (it->is_directory(ec) &&
                    (name == ".xtvc" || name == ".git" || name == ".xtcache" ||
                     (!m_project.publish.outDir.empty() &&
                      it->path().lexically_normal() ==
                          (m_data.root / pathFromUtf8(m_project.publish.outDir)).lexically_normal()))) {
                    it.disable_recursion_pending();
                    continue;
                }
                consider(*it);
            }
        } else {
            for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
                if (ec) {
                    break;
                }
                consider(*it);
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    const UiProject& m_project;
    const DesignSystem& m_ds;
    const Graph& m_graph;
    const BuildOptions& m_options;
    ProjectData m_data;
    std::vector<TextEntry> m_texts;
    std::vector<TextEntry> m_decisions;
    std::vector<const TokenMeta*> m_tokens;
    /// T-498c: the `inherited:` entries of tokens.yaml by variable id, and the theme columns of the
    /// inherited tables.
    std::map<std::string, const TokenMeta*> m_inheritedMeta;
    std::vector<std::string> m_inheritedThemes;
    Lines m_deprecated[kSectionCount];
    json m_index;
};

void addIssueDesignDoc(std::vector<Issue>& out, const char* id, Severity severity, std::string entry,
                       std::string message) {
    out.push_back(Issue{id, severity, std::move(entry), std::move(message)});
}

std::vector<Issue> lintRules(const UiProject& project, const DesignSystem& ds, const Graph& graph,
                             const BuildOptions& options, bool withDocStale) {
    std::vector<Issue> out;
    ProjectData data;
    loadProjectData(project, ds, options.standardSymbolDir, data);

    // design.literalColorNotInPalette (info).
    bool palette = false;
    for (const Swatch& s : ds.swatches) {
        palette = palette || !normalizeHex(s.value).empty();
    }
    if (palette) {
        AuditReport report;
        if (audit(project, ds, "uis", report) == Status::kOk) {
            for (const AuditReport::Literal& l : report.colors) {
                if (!l.nearestSwatch.empty() && l.deltaE <= 2.0f) {
                    continue;
                }
                std::string entry;
                for (const std::string& w : l.where) {
                    if (w.find('#') != std::string::npos && w.find('/') == std::string::npos) {
                        entry = "layer:" + w;
                        break;
                    }
                }
                std::vector<std::string> where(
                    l.where.begin(),
                    l.where.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(3, l.where.size())));
                std::string message = l.value + " (" + std::to_string(l.count) + "x) is not in the palette";
                if (!l.nearestSwatch.empty()) {
                    message += "; nearest swatch:" + l.nearestSwatch + " at ΔE2000 " + fixed2(l.deltaE);
                }
                message += "; used at " + joinDesignDoc(where, ", ") + (l.where.size() > 3 ? ", …" : "");
                addIssueDesignDoc(out, "design.literalColorNotInPalette", Severity::Info, entry, message);
            }
        }
    }

    // design.contrastLow (warning): base, then every theme that overrides either side.
    for (const ContrastPair& p : contrastPairs(ds)) {
        const auto check = [&](const std::map<std::string, std::string>* theme,
                               const std::string& themeName) {
            const std::string fg = resolveColor(p.fg, ds, data, theme);
            const std::string bg = resolveColor(p.bg, ds, data, theme);
            if (fg.empty() || bg.empty()) {
                return;
            }
            const double ratio = contrastRatio(fg, bg);
            if (ratio + 1e-9 >= p.min) {
                return;
            }
            addIssueDesignDoc(out, "design.contrastLow", Severity::Warning, detail::normalizeRef(p.fg),
                              p.fg + " on " + p.bg +
                                  (themeName.empty() ? std::string() : " (theme " + themeName + ")") + ": " +
                                  fixed2(ratio) + ":1, below " + fixed2(p.min) + ":1");
        };
        check(nullptr, std::string());
        for (size_t t = 0; t < data.themes.size(); ++t) {
            const std::string fgId = tokenIdOf(p.fg);
            const std::string bgId = tokenIdOf(p.bg);
            if ((!fgId.empty() && data.themes[t].count(fgId) != 0) ||
                (!bgId.empty() && data.themes[t].count(bgId) != 0)) {
                check(&data.themes[t], data.themeNames[t]);
            }
        }
    }

    // token.sameAsDefault (warning, T-498b, 49 §49.3 ruling): a `shared.tokens` declaration that
    // repeats the value the preset group's `tokens.xts` gives the same name -- the implied default
    // layer already supplies it, so the project should delete it and follow the group.
    if (!options.standardSymbolDir.empty() && !project.shared.tokens.empty()) {
        SymbolGroup group;
        if (loadGroup(pathFromUtf8(options.standardSymbolDir), group) == Status::kOk &&
            !group.tokensFile.empty()) {
            const fs::path mine = pathFromUtf8(project.rootDir) / pathFromUtf8(project.shared.tokens);
            const fs::path theirs = pathFromUtf8(group.dir) / pathFromUtf8(group.tokensFile);
            for (const TokenDefault& token : sameAsDefaultTokens(mine, theirs)) {
                addIssueDesignDoc(
                    out, "token.sameAsDefault", Severity::Warning,
                    entryRef(EntryKind::Token, token.name.substr(2)),
                    token.name + " repeats the preset group's default; delete it to follow the group");
            }
        }
    }

    // design.draftShipped (warning): a declared variable whose metadata is a draft (or missing).
    for (const auto& [name, value] : data.tokenOrder) {
        (void)value;
        for (const TokenMeta& t : data.synced.tokens) {
            if (t.base.id == name && isDraft(t.base)) {
                addIssueDesignDoc(out, "design.draftShipped", Severity::Warning,
                                  entryRef(EntryKind::Token, name),
                                  "'--" + name + "' is a draft; approve it before publishing");
                break;
            }
        }
    }

    // asset.placeholderShipped (error, T-312a): an asset promoted from a reference and not replaced.
    assets::AssetHygiene hygiene;
    if (assets::assetHygiene(project, &graph, hygiene) == Status::kOk) {
        for (const std::string& file : hygiene.placeholderFiles) {
            addIssueDesignDoc(out, "asset.placeholderShipped", Severity::Error, file,
                              "'" + file + "' is a placeholder; replace it before publishing");
        }
    }

    // design.docStale (info).
    if (withDocStale) {
        BuildOptions full = options;
        full.forAi = false;
        full.sections.clear();
        std::string expected;
        std::string actual;
        if (buildDocument(project, ds, graph, full, expected, nullptr) == Status::kOk) {
            const std::string path = bundle::detail::joinPath(designDir(project), "DESIGN.md");
            if (!bundle::detail::readFile(path, actual)) {
                addIssueDesignDoc(out, "design.docStale", Severity::Info, std::string(),
                                  "DESIGN.md is missing; run xtframe-design build");
            } else if (actual != expected) {
                addIssueDesignDoc(out, "design.docStale", Severity::Info, std::string(),
                                  "DESIGN.md does not match its sources; run xtframe-design build");
            }
        }
    }
    return out;
}

}  // namespace

Status buildDocument(const UiProject& project, const DesignSystem& ds, const Graph& graph,
                     const BuildOptions& options, std::string& markdown, std::vector<Issue>* issues) {
    if (project.isUnnamed()) {
        return Status::kInvalidArgument;
    }
    DocBuilder builder(project, ds, graph, options);
    markdown = builder.build();
    if (issues != nullptr) {
        issues->clear();
        DesignSystem synced = ds;
        syncTokenMeta(synced, project, issues);
        const std::vector<Issue> checks = validate(ds, graph);
        issues->insert(issues->end(), checks.begin(), checks.end());
        const std::vector<Issue> rules = lintRules(project, ds, graph, options, false);
        issues->insert(issues->end(), rules.begin(), rules.end());
    }
    return Status::kOk;
}

Status writeDocument(const UiProject& project, const DesignSystem& ds, const Graph& graph,
                     const BuildOptions& options, std::vector<Issue>* issues, std::string* error) {
    BuildOptions full = options;
    full.forAi = false;
    full.sections.clear();
    std::string markdown;
    const Status built = buildDocument(project, ds, graph, full, markdown, issues);
    if (built != Status::kOk) {
        if (error != nullptr) {
            *error = "an unnamed UI Project has no design document";
        }
        return built;
    }
    const std::string path = bundle::detail::joinPath(designDir(project), "DESIGN.md");
    std::string existing;
    if (bundle::detail::readFile(path, existing) && existing == markdown) {
        return Status::kOk;
    }
    if (!bundle::detail::writeFile(path, markdown)) {
        if (error != nullptr) {
            *error = "cannot write '" + path + "'";
        }
        return Status::kIoError;
    }
    return Status::kOk;
}

std::vector<Issue> lintDocument(const UiProject& project, const DesignSystem& ds, const Graph& graph,
                                const BuildOptions& options) {
    if (project.isUnnamed()) {
        return {};
    }
    return lintRules(project, ds, graph, options, true);
}

namespace detail {

/// The `--name: value` declarations of a stylesheet: (name without `--`, value), first declaration's
/// order, last declaration's value.
std::vector<std::pair<std::string, std::string>> readTokenDecls(const std::string& path,
                                                                const std::string& label) {
    std::vector<std::pair<std::string, std::string>> out;
    std::string text;
    if (path.empty() || !bundle::detail::readFile(path, text)) {
        return out;
    }
    compile::DiagnosticSink sink;
    compile::RuleSet rules;
    compile::parseXts(text, label, 1, sink, rules);
    std::map<std::string, size_t> index;
    for (const compile::TokenDecl& t : rules.tokens) {
        if (t.name.size() < 3) {
            continue;
        }
        const std::string name = t.name.substr(2);
        const auto found = index.find(name);
        if (found == index.end()) {
            index[name] = out.size();
            out.emplace_back(name, util::trim(t.value));
        } else {
            out[found->second].second = util::trim(t.value);
        }
    }
    return out;
}

ImpliedLayer readImpliedLayer(const std::string& groupDir) {
    ImpliedLayer out;
    if (groupDir.empty()) {
        return out;
    }
    SymbolGroup group;
    if (loadGroup(pathFromUtf8(groupDir), group) != Status::kOk || group.tokensFile.empty()) {
        return out;
    }
    const fs::path root = pathFromUtf8(group.dir);
    out.tokens = readTokenDecls(pathToUtf8(root / pathFromUtf8(group.tokensFile)), group.tokensFile);
    if (out.tokens.empty()) {
        return out;
    }
    out.id = group.id;
    for (const std::string& file : group.themeFiles) {
        const std::string name = themeNameOf(file);
        if (name.empty()) {
            continue;
        }
        out.themes.emplace_back(name, readTokenDecls(pathToUtf8(root / pathFromUtf8(file)), file));
    }
    std::set<std::string> seen;
    for (const SymbolEntry& symbol : group.symbols) {
        for (const TokenDefault& token : symbol.tokens) {
            if (token.name.size() > 2 && seen.insert(token.name).second) {
                out.firstUser.emplace_back(token.name.substr(2), symbol.name);
            }
        }
    }
    return out;
}

}  // namespace detail

}  // namespace xt::bundle::design
