#include "xt/bundle/design_export.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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
#include "symbol_contracts.h"
#include "xt/bundle/design_color.h"
#include "xt/bundle/design_doc.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/parse_xts.h"
#include "xt/compile/style_model.h"
#include "xt/compile/utf8_path.h"

/// The Claude Design design-system exchange (docs/design/35-design-system.md §35.4, §35.7.1;
/// docs/tasks/T-316.md items 1 and 2). See xt/bundle/design_export.h.
///
/// The README is buildDocument()'s AI edition (no images, no Deprecated lists) of the exported
/// sections with an unlimited budget, then rewritten line by line: the banner and title replaced,
/// `## <n> · <Title>` headings shortened to `## <Title>`, ` (draft)` marks and the design system's
/// status removed, every table's `UIs` column dropped, empty sections ("Nothing recorded yet.")
/// left out. The import treats every file as untrusted input: size limits, type-checked JSON
/// access, names and values checked before they reach a file name or a stylesheet.

namespace xt::bundle::design {

namespace fs = std::filesystem;
using compile::genericPathToUtf8;
using compile::pathFromUtf8;
using compile::pathToUtf8;
using nlohmann::json;
using nlohmann::ordered_json;

namespace {

/// The largest exchange file the import reads.
constexpr uintmax_t kMaxImportFileBytes = 4u * 1024u * 1024u;
/// The most README sections, themes, variables per theme, groups and swatches the import takes.
constexpr size_t kMaxSections = 256;
constexpr size_t kMaxThemes = 64;
constexpr size_t kMaxTokensPerTheme = 4096;
constexpr size_t kMaxGroups = 256;
constexpr size_t kMaxSwatches = 4096;
/// The longest key / theme / swatch id, variable value and swatch text field the import takes.
constexpr size_t kMaxKeyBytes = 64;
constexpr size_t kMaxValueBytes = 256;
constexpr size_t kMaxTextFieldBytes = 1024;
/// The longest file-name slug of an imported README section.
constexpr size_t kMaxSlugBytes = 48;
/// buildDocument()'s AI edition is cut per section; this budget never cuts.
constexpr int kReadmeTokenBudget = 1 << 26;

/// The README sections, in document order (buildDocument() keys).
const std::vector<std::string> kReadmeSections = {"identity", "voice",  "color",      "type",
                                                  "space",    "motion", "components", "decisions"};

constexpr const char* kReadmeFile = "README.md";
constexpr const char* kTokensFile = "tokens.json";
constexpr const char* kPaletteFile = "palette.json";
constexpr const char* kComponentsFile = "components.md";

std::string trimDesignExport(std::string_view s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) {
        ++b;
    }
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) {
        --e;
    }
    return std::string(s.substr(b, e - b));
}

/// `text` split at '\n', each line without a trailing '\r'; a final newline gives no empty line.
std::vector<std::string> splitLines(std::string_view text) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t nl = text.find('\n', pos);
        std::string line(text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos));
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        out.push_back(std::move(line));
        if (nl == std::string_view::npos) {
            break;
        }
        pos = nl + 1;
    }
    return out;
}

std::string joinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) {
        out += line;
        out += '\n';
    }
    return out;
}

void trimBlankLines(std::vector<std::string>& lines) {
    while (!lines.empty() && trimDesignExport(lines.back()).empty()) {
        lines.pop_back();
    }
    size_t first = 0;
    while (first < lines.size() && trimDesignExport(lines[first]).empty()) {
        ++first;
    }
    lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(first));
}

void replaceAll(std::string& text, std::string_view from, std::string_view to) {
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

bool isFence(const std::string& line) {
    const std::string t = trimDesignExport(line);
    return t.rfind("```", 0) == 0 || t.rfind("~~~", 0) == 0;
}

/// The ATX heading level of `line` (1..6), 0 when it is not a heading.
int headingLevel(const std::string& line) {
    size_t level = 0;
    while (level < line.size() && line[level] == '#') {
        ++level;
    }
    if (level == 0 || level > 6) {
        return 0;
    }
    if (level < line.size() && line[level] != ' ' && line[level] != '\t') {
        return 0;
    }
    return static_cast<int>(level);
}

/// A heading line's text without its `#` marks.
std::string headingText(const std::string& line, int level) {
    std::string text = trimDesignExport(std::string_view(line).substr(static_cast<size_t>(level)));
    while (!text.empty() && text.back() == '#') {
        text.pop_back();
    }
    return trimDesignExport(text);
}

/// "accent-text" -> "accentText", "neutral-500" -> "neutral500".
std::string kebabToCamel(std::string_view s) {
    std::string out;
    bool upper = false;
    for (const char c : s) {
        if (c == '-' || c == '_') {
            upper = !out.empty();
            continue;
        }
        if (upper && c >= 'a' && c <= 'z') {
            out += static_cast<char>(c - 'a' + 'A');
        } else {
            out += c;
        }
        upper = false;
    }
    return out;
}

/// "accentText" -> "accent-text", "neutral500" -> "neutral-500"; already-kebab text is kept.
std::string camelToKebab(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool upper = c >= 'A' && c <= 'Z';
        const bool digit = c >= '0' && c <= '9';
        const bool prevLetter =
            i > 0 && ((s[i - 1] >= 'a' && s[i - 1] <= 'z') || (s[i - 1] >= 'A' && s[i - 1] <= 'Z'));
        if ((upper || (digit && prevLetter)) && !out.empty() && out.back() != '-') {
            out += '-';
        }
        if (upper) {
            out += static_cast<char>(c - 'A' + 'a');
        } else if (c == '_') {
            out += '-';
        } else {
            out += c;
        }
    }
    return out;
}

/// `12`, `12px`, `1.5` as a number; false for anything else.
bool parseNumber(const std::string& text, double& out) {
    std::string t = trimDesignExport(text);
    if (t.size() > 2 && t.compare(t.size() - 2, 2, "px") == 0) {
        t.resize(t.size() - 2);
    }
    if (t.empty() || t.size() > 32) {
        return false;
    }
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || *end != '\0' || !std::isfinite(v)) {
        return false;
    }
    out = v;
    return true;
}

ordered_json numberJson(double v) {
    if (std::floor(v) == v && std::fabs(v) < 1e15) {
        return ordered_json(static_cast<int64_t>(v));
    }
    return ordered_json(v);
}

/// A value as a number when it is one (`12px` -> 12), else the text.
ordered_json numberOrText(const std::string& value) {
    double v = 0.0;
    if (parseNumber(value, v)) {
        return numberJson(v);
    }
    return ordered_json(trimDesignExport(value));
}

/// The `--name: value` declarations of a stylesheet (name without `--`), first declaration's order,
/// last declaration's value. Empty when the file cannot be read.
std::vector<std::pair<std::string, std::string>> readDecls(const fs::path& file, const std::string& label) {
    std::vector<std::pair<std::string, std::string>> out;
    std::string text;
    if (!bundle::detail::readFile(pathToUtf8(file), text)) {
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
            out.emplace_back(name, trimDesignExport(t.value));
        } else {
            out[found->second].second = trimDesignExport(t.value);
        }
    }
    return out;
}

using Decls = std::vector<std::pair<std::string, std::string>>;
using DeclMap = std::map<std::string, std::string>;

DeclMap toMap(const Decls& decls) {
    DeclMap out;
    for (const auto& [name, value] : decls) {
        out[name] = value;
    }
    return out;
}

/// `value` with `var(--x)` followed (theme overrides first, then the base) up to 8 levels; the
/// original text when it does not resolve.
std::string resolveValue(const std::string& value, const DeclMap& base, const DeclMap* theme) {
    std::string current = trimDesignExport(value);
    for (int depth = 0; depth < 8; ++depth) {
        if (current.rfind("var(", 0) != 0) {
            return current;
        }
        size_t b = 4;
        while (b < current.size() && current[b] == ' ') {
            ++b;
        }
        size_t e = b;
        while (e < current.size() && current[e] != ')' && current[e] != ',' && current[e] != ' ') {
            ++e;
        }
        std::string name = current.substr(b, e - b);
        if (name.rfind("--", 0) == 0) {
            name = name.substr(2);
        }
        if (theme != nullptr) {
            const auto found = theme->find(name);
            if (found != theme->end()) {
                current = trimDesignExport(found->second);
                continue;
            }
        }
        const auto found = base.find(name);
        if (found == base.end()) {
            return trimDesignExport(value);
        }
        current = trimDesignExport(found->second);
    }
    return trimDesignExport(value);
}

/// The theme files of `project` (sorted by path): (stem, file).
std::vector<std::pair<std::string, fs::path>> themeFiles(const UiProject& project) {
    std::vector<std::pair<std::string, fs::path>> out;
    if (project.shared.themes.empty()) {
        return out;
    }
    const fs::path dir = pathFromUtf8(project.rootDir) / pathFromUtf8(project.shared.themes);
    std::error_code ec;
    std::vector<fs::path> files;
    if (fs::is_directory(dir, ec)) {
        for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
            if (ec) {
                break;
            }
            if (it->is_regular_file(ec) && it->path().extension() == ".xts") {
                files.push_back(it->path());
            }
        }
    }
    std::sort(files.begin(), files.end());
    for (const fs::path& file : files) {
        out.emplace_back(pathToUtf8(file.stem()), file);
    }
    return out;
}

fs::path tokensPath(const UiProject& project) {
    return pathFromUtf8(project.rootDir) / pathFromUtf8(project.shared.tokens);
}

bool isDeprecatedEntry(const Entry& e) {
    return e.status == EntryStatus::Deprecated || detail::statusText(e) == "deprecated";
}

// --- export: README.md ---

/// One Markdown table row's cells (`\|` kept as written).
std::vector<std::string> tableCells(const std::string& line) {
    std::vector<std::string> cells;
    std::string t = trimDesignExport(line);
    if (!t.empty() && t.front() == '|') {
        t.erase(0, 1);
    }
    if (!t.empty() && t.back() == '|' && !(t.size() >= 2 && t[t.size() - 2] == '\\')) {
        t.pop_back();
    }
    std::string current;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '\\' && i + 1 < t.size() && t[i + 1] == '|') {
            current += "\\|";
            ++i;
        } else if (t[i] == '|') {
            cells.push_back(trimDesignExport(current));
            current.clear();
        } else {
            current += t[i];
        }
    }
    cells.push_back(trimDesignExport(current));
    return cells;
}

std::string tableRow(const std::vector<std::string>& cells) {
    std::string out = "|";
    for (const std::string& c : cells) {
        out += " " + c + " |";
    }
    return out;
}

bool isTableRule(const std::string& line) {
    const std::string t = trimDesignExport(line);
    return t.rfind("| ---", 0) == 0 || t.rfind("|---", 0) == 0;
}

/// Every table of `lines` without its `UIs` column (an internal count).
void dropUiColumns(std::vector<std::string>& lines) {
    for (size_t i = 0; i + 1 < lines.size(); ++i) {
        if (trimDesignExport(lines[i]).rfind('|', 0) != 0 || !isTableRule(lines[i + 1])) {
            continue;
        }
        const std::vector<std::string> header = tableCells(lines[i]);
        const auto it = std::find(header.begin(), header.end(), "UIs");
        if (it == header.end()) {
            continue;
        }
        const size_t column = static_cast<size_t>(it - header.begin());
        for (size_t j = i; j < lines.size() && trimDesignExport(lines[j]).rfind('|', 0) == 0; ++j) {
            std::vector<std::string> cells = tableCells(lines[j]);
            if (column < cells.size()) {
                cells.erase(cells.begin() + static_cast<std::ptrdiff_t>(column));
            }
            lines[j] = tableRow(cells);
        }
    }
}

std::string designName(const UiProject& project, const DesignSystem& ds) {
    if (!ds.info.name.empty()) {
        return ds.info.name;
    }
    return project.name.empty() ? project.id : project.name;
}

std::string buildReadme(const UiProject& project, const DesignSystem& ds, const Graph& graph,
                        std::string_view symbolsDir, std::vector<std::string>& titles) {
    BuildOptions options;
    options.forAi = true;  // no images, no Deprecated lists
    options.maxTokensHint = kReadmeTokenBudget;
    options.sections = kReadmeSections;
    options.standardSymbolDir = std::string(symbolsDir);
    std::string markdown;
    buildDocument(project, ds, graph, options, markdown, nullptr);
    const std::string statusSuffix = ds.info.status.empty() ? std::string() : ", status " + ds.info.status;

    std::vector<std::string> out;
    out.push_back("# " + designName(project, ds) + " design system");
    out.emplace_back();
    out.push_back(
        "Built with XTFrame; colours and sizes below are the project's tokens. `tokens.json` holds the "
        "same values in the Claude Design token shape (`themes.base` is the base set; every other theme "
        "lists only the values it changes), `palette.json` the colour library and `components.md` the "
        "symbol library.");

    const std::vector<std::string> lines = splitLines(markdown);
    size_t i = 0;
    while (i < lines.size() && lines[i].rfind("## ", 0) != 0) {
        ++i;
    }
    while (i < lines.size()) {
        std::string title = headingText(lines[i], 2);
        const size_t dot = title.find("\xC2\xB7 ");  // "3 · Color"
        if (dot != std::string::npos) {
            title = trimDesignExport(title.substr(dot + 3));
        }
        std::vector<std::string> body;
        for (++i; i < lines.size() && lines[i].rfind("## ", 0) != 0; ++i) {
            std::string line = lines[i];
            replaceAll(line, " (draft)", "");
            if (!statusSuffix.empty() && line.rfind("- Design system: ", 0) == 0 &&
                line.size() >= statusSuffix.size() &&
                line.compare(line.size() - statusSuffix.size(), statusSuffix.size(), statusSuffix) == 0) {
                line.resize(line.size() - statusSuffix.size());
            }
            body.push_back(std::move(line));
        }
        trimBlankLines(body);
        if (body.empty() || (body.size() == 1 && body[0] == "Nothing recorded yet.")) {
            continue;
        }
        dropUiColumns(body);
        titles.push_back(title);
        out.emplace_back();
        out.push_back("## " + title);
        out.emplace_back();
        out.insert(out.end(), body.begin(), body.end());
    }
    return joinLines(out);
}

// --- export: tokens.json ---

/// `name` (`color-accent-text`) as a `themes` key (`accentText`).
std::string colorKey(const std::string& name) {
    return kebabToCamel(name.rfind("color-", 0) == 0 ? name.substr(6) : name);
}

ordered_json buildTokens(const UiProject& project, const DesignSystem& ds, ExportReport& report) {
    const Decls baseDecls =
        project.shared.tokens.empty() ? Decls() : readDecls(tokensPath(project), project.shared.tokens);
    const DeclMap base = toMap(baseDecls);
    DesignSystem synced = ds;
    syncTokenMeta(synced, project, nullptr);
    std::map<std::string, const TokenMeta*> meta;
    for (const TokenMeta& t : synced.tokens) {
        meta[t.base.id] = &t;
    }
    const auto categoryOf = [&](const std::string& name) {
        const auto found = meta.find(name);
        if (found == meta.end() || isDeprecatedEntry(found->second->base)) {
            return std::string();
        }
        return found->second->category;
    };
    const auto put = [&](ordered_json& object, const std::string& key, ordered_json value,
                         const std::string& what) {
        if (key.empty() || object.contains(key)) {
            report.notes.push_back(what + ": key '" + key + "' is empty or taken; not exported");
            return;
        }
        object[key] = std::move(value);
    };

    ordered_json root = ordered_json::object();
    root["name"] = designName(project, ds);
    root["version"] = 1;

    ordered_json themes = ordered_json::object();
    ordered_json baseTheme = ordered_json::object();
    for (const auto& [name, value] : baseDecls) {
        if (categoryOf(name) != "color") {
            continue;
        }
        put(baseTheme, colorKey(name), resolveValue(value, base, nullptr), "--" + name);
    }
    report.colorTokens = static_cast<int>(baseTheme.size());
    themes["base"] = std::move(baseTheme);
    report.themes.push_back("base");
    for (const auto& [stem, file] : themeFiles(project)) {
        if (stem == "base" || themes.contains(stem)) {
            report.notes.push_back("theme '" + stem + "': the name is taken; not exported");
            continue;
        }
        const Decls decls = readDecls(file, project.shared.themes + "/" + stem + ".xts");
        const DeclMap overrides = toMap(decls);
        ordered_json theme = ordered_json::object();
        for (const auto& [name, value] : decls) {
            if (categoryOf(name) != "color") {
                report.notes.push_back("theme '" + stem + "': --" + name +
                                       " is not a colour variable; not exported");
                continue;
            }
            put(theme, colorKey(name), resolveValue(value, base, &overrides),
                "theme '" + stem + "' --" + name);
        }
        themes[stem] = std::move(theme);
        report.themes.push_back(stem);
    }
    root["themes"] = std::move(themes);

    // font: the family, the type roles, then the type variables by step.
    ordered_json font = ordered_json::object();
    std::string family;
    if (const auto found = base.find("font-family"); found != base.end()) {
        family = resolveValue(found->second, base, nullptr);
    }
    for (const TypeRole& r : synced.typeRoles) {
        if (family.empty() && !r.family.empty() && !isDeprecatedEntry(r.base)) {
            family = r.family;
        }
    }
    font["family"] = family;
    const auto step = [&](const std::string& key) -> ordered_json& {
        if (!font.contains(key) || !font[key].is_object()) {
            font[key] = ordered_json::object();
        }
        return font[key];
    };
    for (const TypeRole& r : synced.typeRoles) {
        if (isDeprecatedEntry(r.base)) {
            continue;
        }
        const std::string key = kebabToCamel(r.base.id);
        if (key.empty() || key == "family") {
            report.notes.push_back("type role '" + r.base.id + "': not exported");
            continue;
        }
        ordered_json& role = step(key);
        if (!r.family.empty() && r.family != family) {
            role["family"] = r.family;
        }
        if (!r.weights.empty()) {
            role["weight"] = numberOrText(r.weights[0]);
            if (r.weights.size() > 1) {
                ordered_json weights = ordered_json::array();
                for (const std::string& w : r.weights) {
                    weights.push_back(numberOrText(w));
                }
                role["weights"] = std::move(weights);
            }
        }
    }
    struct TypeField {
        const char* prefix;
        const char* field;
    };
    static const TypeField kTypeFields[] = {{"font-size-", "size"},
                                            {"font-weight-", "weight"},
                                            {"font-line-height-", "lineHeight"},
                                            {"line-height-", "lineHeight"},
                                            {"font-letter-spacing-", "letterSpacing"},
                                            {"letter-spacing-", "letterSpacing"},
                                            {"font-family-", "family"}};
    std::vector<double> space;
    ordered_json radius = ordered_json::object();
    ordered_json shadow = ordered_json::object();
    for (const auto& [name, value] : baseDecls) {
        const std::string category = categoryOf(name);
        const std::string resolved = resolveValue(value, base, nullptr);
        if (category == "type") {
            if (name == "font-family") {
                continue;
            }
            bool placed = false;
            for (const TypeField& f : kTypeFields) {
                const std::string prefix = f.prefix;
                if (name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0) {
                    const std::string key = kebabToCamel(name.substr(prefix.size()));
                    if (key == "family") {
                        break;
                    }
                    ordered_json& entry = step(key);
                    if (!entry.contains(f.field)) {
                        entry[f.field] = std::string(f.field) == "family" ? ordered_json(resolved)
                                                                          : numberOrText(resolved);
                        placed = true;
                    }
                    break;
                }
            }
            if (!placed) {
                report.notes.push_back("--" + name + " (type): no place in tokens.json font; not exported");
            }
        } else if (category == "space") {
            double v = 0.0;
            if (parseNumber(resolved, v)) {
                space.push_back(v);
            } else {
                report.notes.push_back("--" + name + " (space): '" + resolved +
                                       "' is not a number; not exported");
            }
        } else if (category == "radius") {
            put(radius, kebabToCamel(name.rfind("radius-", 0) == 0 ? name.substr(7) : name),
                numberOrText(resolved), "--" + name);
        } else if (category == "shadow") {
            put(shadow, kebabToCamel(name.rfind("shadow-", 0) == 0 ? name.substr(7) : name),
                ordered_json(resolved), "--" + name);
        }
    }
    root["font"] = std::move(font);
    std::sort(space.begin(), space.end());
    space.erase(std::unique(space.begin(), space.end()), space.end());
    ordered_json spaceJson = ordered_json::array();
    for (double v : space) {
        spaceJson.push_back(numberJson(v));
    }
    root["space"] = std::move(spaceJson);
    root["radius"] = std::move(radius);
    root["shadow"] = std::move(shadow);

    Vec2 board{1280.0f, 720.0f};
    if (!project.spec.platforms.empty() && !project.spec.platforms[0].resolutions.empty()) {
        board = project.spec.platforms[0].resolutions[0];
    } else if (!project.uis.empty()) {
        board = project.uis[0].size;
    }
    const auto dimension = [](float v, double fallback) {
        return std::isfinite(v) && v >= 1.0f && v <= bundle::detail::kMaxUiSize
                   ? std::round(static_cast<double>(v))
                   : fallback;
    };
    root["board"] = ordered_json{{"width", numberJson(dimension(board.x, 1280.0))},
                                 {"height", numberJson(dimension(board.y, 720.0))}};
    return root;
}

// --- export: palette.json ---

ordered_json swatchJsonDesignExport(const Swatch& s) {
    ordered_json o = ordered_json::object();
    o["id"] = s.base.id;
    o["name"] = s.base.name.empty() ? s.base.id : s.base.name;
    o["value"] = s.value;
    if (!s.role.empty()) {
        o["role"] = s.role;
    }
    if (!s.base.note.empty()) {
        o["note"] = s.base.note;
    }
    return o;
}

ordered_json buildPalette(const UiProject& project, const DesignSystem& ds, ExportReport& report) {
    std::map<std::string, const Swatch*> byId;
    for (const Swatch& s : ds.swatches) {
        byId.emplace(s.base.id, &s);
    }
    std::set<std::string> listed;
    ordered_json groups = ordered_json::array();
    for (const Group& g : ds.groups) {
        if (isDeprecatedEntry(g.base)) {
            continue;
        }
        ordered_json go = ordered_json::object();
        go["id"] = g.base.id;
        go["name"] = g.name.empty() ? (g.base.name.empty() ? g.base.id : g.base.name) : g.name;
        if (!g.base.note.empty()) {
            go["note"] = g.base.note;
        }
        ordered_json list = ordered_json::array();
        std::vector<std::string> ids = g.swatches;
        for (const Swatch& s : ds.swatches) {
            if (s.group == g.base.id) {
                ids.push_back(s.base.id);
            }
        }
        for (const std::string& id : ids) {
            const auto found = byId.find(id);
            if (found == byId.end() || isDeprecatedEntry(found->second->base) || !listed.insert(id).second) {
                continue;
            }
            list.push_back(swatchJsonDesignExport(*found->second));
        }
        go["swatches"] = std::move(list);
        groups.push_back(std::move(go));
    }
    ordered_json loose = ordered_json::array();
    for (const Swatch& s : ds.swatches) {
        if (isDeprecatedEntry(s.base) || !listed.insert(s.base.id).second) {
            continue;
        }
        loose.push_back(swatchJsonDesignExport(s));
    }
    report.swatches = 0;
    for (const auto& g : groups) {
        report.swatches += static_cast<int>(g["swatches"].size());
    }
    report.swatches += static_cast<int>(loose.size());
    ordered_json root = ordered_json::object();
    root["name"] = designName(project, ds);
    root["version"] = 1;
    root["groups"] = std::move(groups);
    root["swatches"] = std::move(loose);
    return root;
}

// --- export: components.md ---

std::string joinWith(const std::vector<std::string>& items, const char* separator) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        out += (i == 0 ? "" : separator) + items[i];
    }
    return out;
}

std::vector<std::string> jsonStrings(const json& object, const char* key) {
    std::vector<std::string> out;
    if (!object.is_object()) {
        return out;
    }
    const auto it = object.find(key);
    if (it == object.end() || !it->is_array()) {
        return out;
    }
    for (const json& v : *it) {
        if (v.is_string()) {
            out.push_back(v.get<std::string>());
        }
    }
    return out;
}

std::string jsonString(const json& object, const char* key) {
    if (!object.is_object()) {
        return std::string();
    }
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

/// One symbol of components.md.
struct SymbolSummary {
    std::string name;
    std::string source;
    std::string description;
    std::vector<std::string> states;
    std::vector<std::string> slots;
    std::vector<std::string> ports;
};

void appendUnique(std::vector<std::string>& list, const std::string& item) {
    if (!item.empty() && std::find(list.begin(), list.end(), item) == list.end()) {
        list.push_back(item);
    }
}

SymbolSummary fromIndex(const json& s) {
    SymbolSummary out;
    out.name = jsonString(s, "name");
    out.description = jsonString(s, "description");
    if (s.contains("requires") && s["requires"].is_object()) {
        out.states = jsonStrings(s["requires"], "states");
        out.slots = jsonStrings(s["requires"], "slots");
    }
    if (s.contains("ports") && s["ports"].is_array()) {
        for (const json& p : s["ports"]) {
            const std::string name = jsonString(p, "name");
            if (name.empty()) {
                continue;
            }
            std::vector<std::string> parts;
            for (const char* key : {"type", "dir"}) {
                const std::string v = jsonString(p, key);
                if (!v.empty()) {
                    parts.push_back(v);
                }
            }
            out.ports.push_back(parts.empty() ? name : name + " (" + joinWith(parts, ", ") + ")");
        }
    }
    return out;
}

std::string buildComponents(const UiProject& project, const DesignSystem& ds, std::string_view symbolsDir,
                            ExportReport& report) {
    std::vector<const json*> standard;
    std::map<std::string, const json*> byName;
    json index;
    if (!symbolsDir.empty()) {
        std::string text;
        if (bundle::detail::readFile(bundle::detail::joinPath(symbolsDir, "index.json"), text)) {
            std::string error;
            index = bundle::detail::parseBounded(text, bundle::detail::kMaxJsonDepth, error);
        }
        if (index.is_object() && index.contains("symbols") && index["symbols"].is_array()) {
            for (const json& s : index["symbols"]) {
                const std::string name = jsonString(s, "name");
                if (!name.empty()) {
                    standard.push_back(&s);
                    byName.emplace(name, &s);
                }
            }
        }
    }
    std::vector<SymbolSummary> project_symbols;
    std::set<std::string> seen;
    if (!project.shared.symbols.empty()) {
        for (const SymbolContract& c : collectSymbolContracts(project.rootDir, project.shared.symbols, {})) {
            if (!seen.insert(c.name).second) {
                continue;
            }
            SymbolSummary s;
            const auto found = byName.find(c.name);
            if (found != byName.end()) {
                s = fromIndex(*found->second);
            }
            s.name = c.name;
            s.source = c.source;
            for (const std::string& st : c.requiredStates) {
                appendUnique(s.states, st);
            }
            for (const SymbolStateGroup& g : c.stateGroups) {
                for (const std::string& st : g.states) {
                    appendUnique(s.states, st);
                }
            }
            for (const std::string& slot : c.requiredSlots) {
                appendUnique(s.slots, slot);
            }
            for (const std::string& slot : c.slotNames) {
                appendUnique(s.slots, slot);
            }
            if (!c.ports.empty()) {
                s.ports.clear();
                for (const SymbolPortInfo& p : c.ports) {
                    std::vector<std::string> parts;
                    if (!p.type.empty()) {
                        parts.push_back(p.type);
                    }
                    if (!p.dir.empty()) {
                        parts.push_back(p.dir);
                    }
                    s.ports.push_back(parts.empty() ? p.name : p.name + " (" + joinWith(parts, ", ") + ")");
                }
            }
            project_symbols.push_back(std::move(s));
        }
    }
    std::vector<SymbolSummary> library;
    for (const json* s : standard) {
        SymbolSummary summary = fromIndex(*s);
        if (seen.count(summary.name) == 0) {
            library.push_back(std::move(summary));
        }
    }

    std::vector<std::string> out;
    out.push_back("# " + designName(project, ds) + " components");
    out.emplace_back();
    out.push_back(
        "The symbol library of this XTFrame project: every symbol with its states, slots, ports and how to "
        "use it. Draw a board element that stands for one with the same structure and slot names.");
    const auto section = [&](const char* title, const std::vector<SymbolSummary>& list) {
        if (list.empty()) {
            return;
        }
        out.emplace_back();
        out.push_back(std::string("## ") + title);
        for (const SymbolSummary& s : list) {
            out.emplace_back();
            out.push_back("### " + s.name);
            out.emplace_back();
            if (!s.description.empty()) {
                out.push_back(trimDesignExport(s.description));
                out.emplace_back();
            }
            if (!s.source.empty()) {
                out.push_back("- File: `" + s.source + "`");
            }
            out.push_back("- States: " + (s.states.empty() ? std::string("none") : joinWith(s.states, ", ")));
            out.push_back("- Slots: " + (s.slots.empty() ? std::string("none") : joinWith(s.slots, ", ")));
            out.push_back("- Ports: " + (s.ports.empty() ? std::string("none") : joinWith(s.ports, "; ")));
        }
    };
    section("Project symbols", project_symbols);
    section("Standard library", library);
    if (project_symbols.empty() && library.empty()) {
        out.emplace_back();
        out.push_back("No symbols yet.");
    }
    report.symbols = static_cast<int>(project_symbols.size() + library.size());
    return joinLines(out);
}

std::string dumpJson(const ordered_json& j) {
    return j.dump(2, ' ', false, ordered_json::error_handler_t::replace) + "\n";
}

// --- import ---

/// A key, theme name or swatch id from an exchange file: an ASCII letter or digit first, then
/// letters, digits, '-' and '_', at most kMaxKeyBytes.
bool isSafeName(const std::string& name) {
    if (name.empty() || name.size() > kMaxKeyBytes) {
        return false;
    }
    for (size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!alnum && (i == 0 || (c != '-' && c != '_'))) {
            return false;
        }
    }
    return true;
}

/// A value that can go into a declaration as written: not empty, at most kMaxValueBytes, no
/// control character, no `;`, braces, `<` or comment marks, balanced quotes.
bool isSafeValue(const std::string& value) {
    const std::string t = trimDesignExport(value);
    if (t.empty() || t.size() > kMaxValueBytes) {
        return false;
    }
    int doubles = 0;
    int singles = 0;
    for (const char c : t) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F || c == ';' || c == '{' || c == '}' || c == '<' || c == '\\') {
            return false;
        }
        doubles += c == '"' ? 1 : 0;
        singles += c == '\'' ? 1 : 0;
    }
    return doubles % 2 == 0 && singles % 2 == 0 && t.find("/*") == std::string::npos &&
           t.find("*/") == std::string::npos;
}

/// A text field (name, role, note) without line breaks, at most kMaxTextFieldBytes.
std::string textField(const json& object, const char* key) {
    std::string out = trimDesignExport(jsonString(object, key));
    for (char& c : out) {
        if (static_cast<unsigned char>(c) < 0x20) {
            c = ' ';
        }
    }
    if (out.size() > kMaxTextFieldBytes) {
        size_t cut = kMaxTextFieldBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        out.resize(cut);
    }
    return out;
}

/// Reads one exchange file. False with `report.error` when it exists but is too big or unreadable;
/// true with `exists = false` when it is missing.
bool readExchangeFile(const fs::path& dir, const char* name, std::string& out, bool& exists,
                      ImportReport& report) {
    const fs::path file = dir / name;
    std::error_code ec;
    exists = fs::is_regular_file(file, ec);
    if (!exists) {
        return true;
    }
    const uintmax_t size = fs::file_size(file, ec);
    if (ec || size > kMaxImportFileBytes) {
        report.error = std::string(name) + ": larger than " + std::to_string(kMaxImportFileBytes) + " bytes";
        return false;
    }
    if (!bundle::detail::readFile(pathToUtf8(file), out)) {
        report.error = std::string(name) + ": cannot read";
        return false;
    }
    if (out.size() >= 3 && out.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        out.erase(0, 3);
    }
    report.files.push_back(name);
    return true;
}

/// A README section title without a leading number ("3 · Color", "3. Color" -> "Color").
std::string withoutNumber(const std::string& title) {
    size_t i = 0;
    while (i < title.size() && title[i] >= '0' && title[i] <= '9') {
        ++i;
    }
    if (i == 0) {
        return title;
    }
    size_t j = i;
    while (j < title.size() && title[j] == ' ') {
        ++j;
    }
    if (title.compare(j, 2, "\xC2\xB7") == 0) {
        j += 2;
    } else if (j < title.size() &&
               (title[j] == '.' || title[j] == ')' || title[j] == '-' || title[j] == ':')) {
        ++j;
    } else if (j == i) {
        return title;  // "3D effects"
    }
    const std::string rest = trimDesignExport(title.substr(j));
    return rest.empty() ? title : rest;
}

/// The design document section a README title belongs to (identity when unknown).
std::string sectionFor(const std::string& title) {
    std::string key;
    for (const char c : title) {
        key += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    key = trimDesignExport(key);
    replaceAll(key, "&", "and");
    static const std::vector<std::pair<const char*, std::vector<const char*>>> kTable = {
        {"identity", {"identity", "overview", "about", "introduction", "brand", "principles"}},
        {"voice", {"voice", "tone", "voice and tone", "copy", "copywriting", "writing", "content"}},
        {"color", {"color", "colors", "colour", "colours", "palette", "color palette", "colour palette"}},
        {"type", {"type", "typography", "fonts", "font"}},
        {"space", {"space and shape", "space", "spacing", "shape", "radius", "elevation", "shadows"}},
        {"motion", {"motion", "animation", "animations"}},
        {"layout", {"layout", "layouts", "grid"}},
        {"components", {"components", "component library", "symbols"}},
        {"imagery", {"imagery", "iconography", "icons", "images", "illustration"}},
        {"screens", {"screens"}},
        {"decisions", {"decisions"}},
        {"changelog", {"changelog"}},
    };
    for (const auto& [section, names] : kTable) {
        for (const char* name : names) {
            if (key == name) {
                return section;
            }
        }
    }
    return "identity";
}

/// An ASCII file-name slug of an anchor (non-ASCII bytes dropped), at most kMaxSlugBytes.
std::string fileSlug(const std::string& anchor) {
    std::string out;
    for (const char c : anchor) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
            if (c == '-' && (out.empty() || out.back() == '-')) {
                continue;
            }
            out += c;
        }
    }
    if (out.size() > kMaxSlugBytes) {
        out.resize(kMaxSlugBytes);
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    return out;
}

struct MdSection {
    std::string title;
    std::string anchor;
    std::string body;
};

/// The `##` sections of a README (`#` when it has no `##` heading), outside fenced code.
std::vector<MdSection> readmeSections(const std::string& text) {
    const std::vector<std::string> lines = splitLines(text);
    int level = 1;
    bool fence = false;
    for (const std::string& line : lines) {
        if (isFence(line)) {
            fence = !fence;
        } else if (!fence && headingLevel(line) == 2) {
            level = 2;
            break;
        }
    }
    std::vector<MdSection> out;
    std::map<std::string, int> anchors;
    std::vector<std::string> body;
    bool open = false;
    const auto close = [&]() {
        if (open) {
            trimBlankLines(body);
            out.back().body = joinLines(body);
        }
        body.clear();
        open = false;
    };
    fence = false;
    for (const std::string& line : lines) {
        if (isFence(line)) {
            fence = !fence;
        }
        const int h = fence || isFence(line) ? 0 : headingLevel(line);
        if (h != 0 && h <= level) {
            close();
            if (h == level) {
                MdSection s;
                s.title = withoutNumber(headingText(line, h));
                std::string anchor = detail::headingSlug(s.title);
                const int seen = anchors[anchor]++;
                if (seen > 0) {
                    anchor += "-" + std::to_string(seen);
                }
                s.anchor = anchor;
                out.push_back(std::move(s));
                open = true;
            }
            continue;
        }
        if (open) {
            body.push_back(line);
        }
    }
    close();
    return out;
}

void eraseExtraKey(Entry& e, const char* key) {
    if (e.extra.is_object()) {
        e.extra.erase(key);
        if (e.extra.empty()) {
            e.extra = json();
        }
    }
}

bool fromThisImport(const Entry& e, const std::string& ref) {
    return e.source.kind == kClaudeDesignSourceKind && e.source.ref == ref;
}

/// Puts the entry back to draft (status and approval); true when anything changed.
bool backToDraft(Entry& e) {
    const bool changed =
        e.status != EntryStatus::Draft || !e.approved.by.empty() || !e.approved.at.empty() ||
        (e.extra.is_object() && (e.extra.contains("status") || e.extra.contains("approved")));
    e.status = EntryStatus::Draft;
    e.approved = Approval();
    eraseExtraKey(e, "status");
    eraseExtraKey(e, "approved");
    return changed;
}

Source importSource(const std::string& ref) {
    Source s;
    s.kind = std::string(kClaudeDesignSourceKind);
    s.ref = ref;
    return s;
}

void upsertText(DesignSystem& ds, const std::string& ref, std::string slug, const std::string& title,
                const std::string& section, const std::string& body, ImportReport& report) {
    for (TextEntry& t : ds.texts) {
        if (!fromThisImport(t.base, ref)) {
            continue;
        }
        bool changed = backToDraft(t.base);
        if (t.body != body || t.title != title || t.sections != std::vector<std::string>{section}) {
            t.body = body;
            t.title = title;
            t.sections = {section};
            eraseExtraKey(t.base, "section");
            eraseExtraKey(t.base, "title");
            changed = true;
        }
        report.textsUpdated += changed ? 1 : 0;
        return;
    }
    const auto taken = [&](const std::string& path, const std::string& id) {
        for (const std::vector<TextEntry>* list : {&ds.texts, &ds.decisions}) {
            for (const TextEntry& t : *list) {
                if (t.path == path || t.base.id == id) {
                    return true;
                }
            }
        }
        return false;
    };
    std::string id = "imported/" + slug;
    std::string path = "text/" + id + ".md";
    for (int n = 2; taken(path, id); ++n) {
        id = "imported/" + slug + "-" + std::to_string(n);
        path = "text/" + id + ".md";
    }
    TextEntry t;
    t.base.kind = EntryKind::Text;
    t.base.id = id;
    t.base.status = EntryStatus::Draft;
    t.base.source = importSource(ref);
    t.path = path;
    t.title = title;
    t.sections = {section};
    t.body = body;
    ds.texts.push_back(std::move(t));
    ++report.textsAdded;
}

/// Inserts `--name: value;` lines into the first `:root { }` block of `text` (a new block at the
/// end when there is none).
std::string insertDecls(const std::string& text,
                        const std::vector<std::pair<std::string, std::string>>& decls) {
    std::string lines;
    for (const auto& [name, value] : decls) {
        lines += "  " + name + ": " + trimDesignExport(value) + ";\n";
    }
    // Find `:root` and its block outside comments and strings.
    size_t rootAt = std::string::npos;
    size_t open = std::string::npos;
    size_t closeAt = std::string::npos;
    int depth = 0;
    char quote = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quote != 0) {
            if (c == quote) {
                quote = 0;
            }
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            const size_t end = text.find("*/", i + 2);
            if (end == std::string::npos) {
                break;
            }
            i = end + 1;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            continue;
        }
        if (open == std::string::npos) {
            if (rootAt == std::string::npos && depth == 0 && text.compare(i, 5, ":root") == 0) {
                rootAt = i;
            }
            if (c == '{') {
                if (rootAt != std::string::npos && depth == 0) {
                    open = i;
                }
                ++depth;
            } else if (c == '}') {
                depth = depth > 0 ? depth - 1 : 0;
            }
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            if (--depth == 0) {
                closeAt = i;
                break;
            }
        }
    }
    if (closeAt == std::string::npos) {
        std::string out = text;
        if (!out.empty() && out.back() != '\n') {
            out += '\n';
        }
        if (!out.empty()) {
            out += '\n';
        }
        return out + ":root {\n" + lines + "}\n";
    }
    std::string before = text.substr(0, closeAt);
    size_t last = before.size();
    while (last > 0 && (before[last - 1] == ' ' || before[last - 1] == '\t' || before[last - 1] == '\n' ||
                        before[last - 1] == '\r')) {
        --last;
    }
    std::string prefix = before.substr(0, last);
    if (!prefix.empty() && prefix.back() != ';' && prefix.back() != '{' && last > open + 1) {
        prefix += ';';
    }
    return prefix + "\n" + lines + text.substr(closeAt);
}

bool appendDecls(const fs::path& file, const std::vector<std::pair<std::string, std::string>>& decls,
                 ImportReport& report) {
    if (decls.empty()) {
        return true;
    }
    std::string text;
    std::error_code ec;
    if (fs::exists(file, ec) && !bundle::detail::readFile(pathToUtf8(file), text)) {
        report.error = "cannot read '" + genericPathToUtf8(file) + "'";
        return false;
    }
    fs::create_directories(file.parent_path(), ec);
    if (!bundle::detail::writeFile(pathToUtf8(file), insertDecls(text, decls))) {
        report.error = "cannot write '" + genericPathToUtf8(file) + "'";
        return false;
    }
    return true;
}

/// A JSON object's string values as (key, value) pairs in file order; other kinds are noted.
std::vector<std::pair<std::string, std::string>> stringMembers(const json& object, const std::string& what,
                                                               ImportReport& report) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t count = 0;
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (++count > kMaxTokensPerTheme) {
            report.notes.push_back(what + ": more than " + std::to_string(kMaxTokensPerTheme) +
                                   " values; the rest are not imported");
            break;
        }
        const std::string key = it.key();
        if (!isSafeName(key)) {
            report.notes.push_back(what + ": key '" + key.substr(0, kMaxKeyBytes) + "' is not a usable name");
            continue;
        }
        if (!it->is_string() || !isSafeValue(it->get<std::string>())) {
            report.notes.push_back(what + "." + key + ": not a usable value");
            continue;
        }
        out.emplace_back(key, trimDesignExport(it->get<std::string>()));
    }
    return out;
}

void importTokens(
    const UiProject& project, DesignSystem& ds, const json& tokens, ImportReport& report,
    std::vector<std::pair<fs::path, std::vector<std::pair<std::string, std::string>>>>& themeWrites) {
    const auto themesIt = tokens.is_object() ? tokens.find("themes") : tokens.end();
    if (!tokens.is_object() || themesIt == tokens.end() || !themesIt->is_object()) {
        report.notes.push_back("tokens.json: no themes object; no variables imported");
        return;
    }
    for (const char* key : {"font", "space", "radius", "shadow", "board"}) {
        if (tokens.contains(key)) {
            report.notes.push_back(std::string("tokens.json: '") + key +
                                   "' is not imported (the README keeps the values as text)");
        }
    }
    const json& themes = *themesIt;
    const DeclMap declared = project.shared.tokens.empty()
                                 ? DeclMap()
                                 : toMap(readDecls(tokensPath(project), project.shared.tokens));
    if (const auto base = themes.find("base"); base != themes.end() && base->is_object()) {
        for (const auto& [key, value] : stringMembers(*base, "tokens.json themes.base", report)) {
            const std::string id = "color-" + camelToKebab(key);
            const std::string ref = std::string(kTokensFile) + "#themes.base." + key;
            TokenMeta* meta = nullptr;
            for (TokenMeta& t : ds.tokens) {
                if (t.base.id == id) {
                    meta = &t;
                }
            }
            if (meta == nullptr) {
                TokenMeta t;
                t.base.kind = EntryKind::Token;
                t.base.id = id;
                t.base.status = EntryStatus::Draft;
                t.base.source = importSource(ref);
                t.category = "color";
                ds.tokens.push_back(std::move(t));
                ++report.tokensAdded;
            } else if (fromThisImport(meta->base, ref)) {
                report.tokensUpdated += backToDraft(meta->base) ? 1 : 0;
            } else {
                ++report.skipped;
            }
            const std::string name = "--" + id;
            const bool planned = std::any_of(report.baseValues.begin(), report.baseValues.end(),
                                             [&](const auto& v) { return v.first == name; });
            if (declared.count(id) == 0 && !planned) {
                report.baseValues.emplace_back(name, value);
            }
        }
    } else {
        report.notes.push_back("tokens.json: no themes.base; no base variables imported");
    }
    size_t themeCount = 0;
    for (auto it = themes.begin(); it != themes.end(); ++it) {
        const std::string name = it.key();
        if (name == "base") {
            continue;
        }
        if (++themeCount > kMaxThemes) {
            report.notes.push_back("tokens.json: more than " + std::to_string(kMaxThemes) +
                                   " themes; the rest are not imported");
            break;
        }
        if (!isSafeName(name) || !it->is_object()) {
            report.notes.push_back("tokens.json: theme '" + name.substr(0, kMaxKeyBytes) + "' is not usable");
            continue;
        }
        if (project.shared.themes.empty()) {
            report.notes.push_back("theme '" + name +
                                   "': the project has no shared.themes directory; not imported");
            continue;
        }
        const fs::path file =
            pathFromUtf8(project.rootDir) / pathFromUtf8(project.shared.themes) / pathFromUtf8(name + ".xts");
        const DeclMap existing = toMap(readDecls(file, project.shared.themes + "/" + name + ".xts"));
        std::vector<std::pair<std::string, std::string>> missing;
        for (const auto& [key, value] : stringMembers(*it, "tokens.json themes." + name, report)) {
            const std::string id = "color-" + camelToKebab(key);
            const bool listed = std::any_of(missing.begin(), missing.end(),
                                            [&](const auto& v) { return v.first == "--" + id; });
            if (existing.count(id) == 0 && !listed) {
                missing.emplace_back("--" + id, value);
            }
        }
        report.themeValuesWritten += static_cast<int>(missing.size());
        if (!missing.empty()) {
            themeWrites.emplace_back(file, std::move(missing));
        }
    }
}

void importSwatch(DesignSystem& ds, const json& s, const std::string& group, ImportReport& report) {
    const std::string id = jsonString(s, "id");
    if (!isSafeName(id)) {
        report.notes.push_back("palette.json: swatch '" + id.substr(0, kMaxKeyBytes) + "' has no usable id");
        return;
    }
    const std::string value = normalizeHex(trimDesignExport(jsonString(s, "value")));
    if (value.empty()) {
        report.notes.push_back("palette.json: swatch '" + id + "' has no hex value");
        return;
    }
    const std::string ref = std::string(kPaletteFile) + "#" + id;
    const std::string name = textField(s, "name");
    const std::string role = textField(s, "role");
    const std::string note = textField(s, "note");
    for (Swatch& existing : ds.swatches) {
        if (existing.base.id != id) {
            continue;
        }
        if (!fromThisImport(existing.base, ref)) {
            ++report.skipped;
            return;
        }
        bool changed = backToDraft(existing.base);
        if (existing.value != value || existing.base.name != name || existing.role != role ||
            existing.base.note != note || existing.group != group) {
            existing.value = value;
            existing.base.name = name;
            existing.role = role;
            existing.base.note = note;
            existing.group = group;
            changed = true;
        }
        report.swatchesUpdated += changed ? 1 : 0;
        return;
    }
    Swatch sw;
    sw.base.kind = EntryKind::Swatch;
    sw.base.id = id;
    sw.base.name = name;
    sw.base.note = note;
    sw.base.status = EntryStatus::Draft;
    sw.base.source = importSource(ref);
    sw.value = value;
    sw.role = role;
    sw.group = group;
    ds.swatches.push_back(std::move(sw));
    ++report.swatchesAdded;
    for (Group& g : ds.groups) {
        if (g.base.id == group && std::find(g.swatches.begin(), g.swatches.end(), id) == g.swatches.end()) {
            g.swatches.push_back(id);
        }
    }
}

void importPalette(DesignSystem& ds, const json& palette, ImportReport& report) {
    if (!palette.is_object()) {
        report.notes.push_back("palette.json: not an object; not imported");
        return;
    }
    size_t swatches = 0;
    const auto take = [&](const json& s, const std::string& group) {
        if (++swatches > kMaxSwatches) {
            if (swatches == kMaxSwatches + 1) {
                report.notes.push_back("palette.json: more than " + std::to_string(kMaxSwatches) +
                                       " swatches; the rest are not imported");
            }
            return;
        }
        if (s.is_object()) {
            importSwatch(ds, s, group, report);
        }
    };
    if (const auto groups = palette.find("groups"); groups != palette.end() && groups->is_array()) {
        size_t count = 0;
        for (const json& g : *groups) {
            if (++count > kMaxGroups) {
                report.notes.push_back("palette.json: more than " + std::to_string(kMaxGroups) +
                                       " groups; the rest are not imported");
                break;
            }
            const std::string id = jsonString(g, "id");
            if (!isSafeName(id)) {
                report.notes.push_back("palette.json: a group has no usable id; not imported");
                continue;
            }
            const bool exists = std::any_of(ds.groups.begin(), ds.groups.end(),
                                            [&](const Group& existing) { return existing.base.id == id; });
            if (!exists) {
                Group group;
                group.base.kind = EntryKind::Group;
                group.base.id = id;
                group.base.status = EntryStatus::Draft;
                group.base.source = importSource(std::string(kPaletteFile) + "#group:" + id);
                group.base.note = textField(g, "note");
                group.name = textField(g, "name");
                ds.groups.push_back(std::move(group));
                ++report.groupsAdded;
            }
            if (const auto list = g.find("swatches"); list != g.end() && list->is_array()) {
                for (const json& s : *list) {
                    take(s, id);
                }
            }
        }
    }
    if (const auto loose = palette.find("swatches"); loose != palette.end() && loose->is_array()) {
        for (const json& s : *loose) {
            take(s, std::string());
        }
    }
}

}  // namespace

Status exportClaudeDesign(const UiProject& project, const DesignSystem& ds, const Graph& graph,
                          std::string_view outDir, ExportReport* report, std::string_view standardSymbolDir) {
    ExportReport local;
    ExportReport& r = report != nullptr ? *report : local;
    r = ExportReport();
    if (project.isUnnamed()) {
        r.error = "an unnamed UI Project has no design system";
        return Status::kInvalidArgument;
    }
    if (outDir.empty()) {
        r.error = "no output directory";
        return Status::kInvalidArgument;
    }
    const fs::path dir = pathFromUtf8(outDir);
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::pair<const char*, std::string> files[] = {
        {kReadmeFile, buildReadme(project, ds, graph, standardSymbolDir, r.sections)},
        {kTokensFile, dumpJson(buildTokens(project, ds, r))},
        {kPaletteFile, dumpJson(buildPalette(project, ds, r))},
        {kComponentsFile, buildComponents(project, ds, standardSymbolDir, r)},
    };
    for (const auto& [name, text] : files) {
        const std::string path = pathToUtf8(dir / name);
        std::string existing;
        if (!(bundle::detail::readFile(path, existing) && existing == text) &&
            !bundle::detail::writeFile(path, text)) {
            r.error = "cannot write '" + genericPathToUtf8(dir / name) + "'";
            return Status::kIoError;
        }
        r.files.push_back(name);
    }
    return Status::kOk;
}

Status importClaudeDesign(const UiProject& project, std::string_view dir, ImportReport* report,
                          const ImportOptions& options) {
    ImportReport local;
    ImportReport& r = report != nullptr ? *report : local;
    r = ImportReport();
    if (project.isUnnamed()) {
        r.error = "an unnamed UI Project has no design system";
        return Status::kInvalidArgument;
    }
    const fs::path root = pathFromUtf8(dir);
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(root, ec)) {
        r.error = "'" + std::string(dir) + "' is not a directory";
        return Status::kNotFound;
    }
    std::string readme;
    std::string tokensText;
    std::string paletteText;
    std::string components;
    bool hasReadme = false;
    bool hasTokens = false;
    bool hasPalette = false;
    bool hasComponents = false;
    if (!readExchangeFile(root, kReadmeFile, readme, hasReadme, r) ||
        !readExchangeFile(root, kTokensFile, tokensText, hasTokens, r) ||
        !readExchangeFile(root, kPaletteFile, paletteText, hasPalette, r) ||
        !readExchangeFile(root, kComponentsFile, components, hasComponents, r)) {
        return Status::kInvalidArgument;
    }
    if (!hasReadme) {
        r.error = "'" + genericPathToUtf8(root / kReadmeFile) + "' does not exist";
        return Status::kNotFound;
    }
    json tokens;
    json palette;
    std::string error;
    if (hasTokens) {
        tokens = bundle::detail::parseBounded(tokensText, bundle::detail::kMaxJsonDepth, error);
        if (tokens.is_discarded()) {
            r.error = std::string(kTokensFile) + ": " + (error.empty() ? "not JSON" : error);
            return Status::kInvalidArgument;
        }
    }
    if (hasPalette) {
        palette = bundle::detail::parseBounded(paletteText, bundle::detail::kMaxJsonDepth, error);
        if (palette.is_discarded()) {
            r.error = std::string(kPaletteFile) + ": " + (error.empty() ? "not JSON" : error);
            return Status::kInvalidArgument;
        }
    }

    DesignSystem ds;
    const Status loaded = load(project, ds, nullptr, &error);
    if (loaded == Status::kNotFound) {
        r.error =
            "the UI Project has no design directory (" + designDir(project) + "); run upgradeDesign first";
        return loaded;
    }
    if (loaded != Status::kOk) {
        r.error = error;
        return loaded;
    }

    // README.md sections -> text entries.
    std::vector<MdSection> sections = readmeSections(readme);
    if (sections.size() > kMaxSections) {
        r.notes.push_back("README.md: more than " + std::to_string(kMaxSections) +
                          " sections; the rest are not imported");
        sections.resize(kMaxSections);
    }
    int index = 0;
    for (const MdSection& s : sections) {
        ++index;
        if (trimDesignExport(s.body).empty()) {
            r.notes.push_back("README.md: section '" + s.title + "' is empty; not imported");
            continue;
        }
        std::string slug = fileSlug(s.anchor);
        if (slug.empty()) {
            slug = "section-" + std::to_string(index);
        }
        const std::string anchor = s.anchor.empty() ? slug : s.anchor;
        upsertText(ds, std::string(kReadmeFile) + "#" + anchor, slug, s.title, sectionFor(s.title),
                   "# " + s.title + "\n\n" + s.body, r);
    }

    // tokens.json -> variable metadata, values and theme files.
    std::vector<std::pair<fs::path, std::vector<std::pair<std::string, std::string>>>> themeWrites;
    if (hasTokens) {
        importTokens(project, ds, tokens, r, themeWrites);
    }
    // palette.json -> groups and swatches.
    if (hasPalette) {
        importPalette(ds, palette, r);
    }
    // components.md -> one text entry.
    if (hasComponents && !trimDesignExport(components).empty()) {
        std::string title = "Components";
        for (const std::string& line : splitLines(components)) {
            if (headingLevel(line) == 1) {
                title = headingText(line, 1);
                break;
            }
        }
        std::string body = components;
        replaceAll(body, "\r\n", "\n");
        upsertText(ds, kComponentsFile, "claude-design-components", title, "components", body, r);
    }

    // Write: variable values, theme files, then the design directory.
    r.valuesWritten = static_cast<int>(r.baseValues.size());
    if (options.writeBaseValues && !r.baseValues.empty()) {
        if (project.shared.tokens.empty()) {
            r.notes.push_back("the project has no shared.tokens stylesheet; " +
                              std::to_string(r.baseValues.size()) + " value(s) not written");
            r.valuesWritten = 0;
        } else if (!appendDecls(tokensPath(project), r.baseValues, r)) {
            return Status::kIoError;
        }
    }
    for (const auto& [file, decls] : themeWrites) {
        if (!appendDecls(file, decls, r)) {
            return Status::kIoError;
        }
    }
    if (save(ds, &error) != Status::kOk) {
        r.error = error;
        return Status::kIoError;
    }
    return Status::kOk;
}

}  // namespace xt::bundle::design
