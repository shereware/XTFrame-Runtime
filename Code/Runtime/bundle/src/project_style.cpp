#include "xt/bundle/project_style.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "bundle_json.h"
#include "style_internal.h"
#include "symbol_internal.h"
#include "xt/compile/utf8_path.h"
#include "xt/util/sha256.h"

/// T-743 (Docs/design/56-symbol-identity-style-scope.md 56.2, 47-symbol-visual-style.md "共同詞彙
/// 清單"): the core vocabulary, the project's style record, the implied layer of a project with
/// several groups (47 rule 5) and the token text helpers style_apply.cpp and symbol_detach.cpp use.

namespace xt::bundle {

namespace fs = std::filesystem;
using compile::genericPathToUtf8;
using compile::pathFromUtf8;
using compile::pathToUtf8;
using detail::json;
using detail::resolveInside;
using detail::trimmed;

namespace {

/// 47 "共同詞彙清單", in the chapter's order (Assets/Symbols/core-tokens.json is checked against it).
constexpr CoreToken kCoreTokens[] = {
    {"--color-bg", CoreTokenType::Color},
    {"--color-surface", CoreTokenType::Color},
    {"--color-surface-raised", CoreTokenType::Color},
    {"--color-surface-sunken", CoreTokenType::Color},
    {"--color-text", CoreTokenType::Color},
    {"--color-text-dim", CoreTokenType::Color},
    {"--color-text-on-accent", CoreTokenType::Color},
    {"--color-accent", CoreTokenType::Color},
    {"--color-accent-hover", CoreTokenType::Color},
    {"--color-accent-pressed", CoreTokenType::Color},
    {"--color-border", CoreTokenType::Color},
    {"--color-border-strong", CoreTokenType::Color},
    {"--color-focus", CoreTokenType::Color},
    {"--color-track", CoreTokenType::Color},
    {"--color-scrim", CoreTokenType::Color},
    {"--color-shadow", CoreTokenType::Color},
    {"--color-disabled-bg", CoreTokenType::Color},
    {"--color-disabled-text", CoreTokenType::Color},
    {"--color-success", CoreTokenType::Color},
    {"--color-warning", CoreTokenType::Color},
    {"--color-danger", CoreTokenType::Color},
    {"--color-info", CoreTokenType::Color},
    {"--font-family", CoreTokenType::Family},
    {"--font-body", CoreTokenType::Family},
    {"--font-display", CoreTokenType::Family},
    {"--font-size-xs", CoreTokenType::Length},
    {"--font-size-s", CoreTokenType::Length},
    {"--font-size-m", CoreTokenType::Length},
    {"--font-size-l", CoreTokenType::Length},
    {"--font-size-xl", CoreTokenType::Length},
    {"--letter-display", CoreTokenType::Length},
    {"--space-1", CoreTokenType::Length},
    {"--space-2", CoreTokenType::Length},
    {"--space-3", CoreTokenType::Length},
    {"--space-4", CoreTokenType::Length},
    {"--space-6", CoreTokenType::Length},
    {"--radius-s", CoreTokenType::Length},
    {"--radius-m", CoreTokenType::Length},
    {"--radius-l", CoreTokenType::Length},
    {"--radius-full", CoreTokenType::Length},
    {"--control-s", CoreTokenType::Length},
    {"--control-m", CoreTokenType::Length},
    {"--control-l", CoreTokenType::Length},
    {"--touch-min", CoreTokenType::Length},
    {"--touch-lg", CoreTokenType::Length},
    {"--border-w", CoreTokenType::Length},
    {"--focus-w", CoreTokenType::Length},
    {"--rule-thin", CoreTokenType::Length},
    {"--rule-thick", CoreTokenType::Length},
    {"--motion-instant", CoreTokenType::Time},
    {"--motion-fast", CoreTokenType::Time},
    {"--motion-normal", CoreTokenType::Time},
};

bool isSpaceCharStyle(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

bool isNameCharStyle(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) != 0 || c == '-' || c == '_' || u >= 0x80;
}

/// `text` is a number (`12`, `-0.5`, `.5`) followed by `unit` characters only; `*unit` gets them.
bool numberWithUnitStyle(std::string_view text, std::string* unit) {
    size_t i = 0;
    if (i < text.size() && (text[i] == '-' || text[i] == '+')) {
        ++i;
    }
    const size_t digits = i;
    bool dot = false;
    while (i < text.size() &&
           (std::isdigit(static_cast<unsigned char>(text[i])) != 0 || (text[i] == '.' && !dot))) {
        dot = dot || text[i] == '.';
        ++i;
    }
    if (i == digits || (i == digits + 1 && dot)) {
        return false;
    }
    for (size_t j = i; j < text.size(); ++j) {
        if (std::isalpha(static_cast<unsigned char>(text[j])) == 0 && text[j] != '%') {
            return false;
        }
    }
    if (unit != nullptr) {
        *unit = std::string(text.substr(i));
    }
    return true;
}

bool startsWithCiStyle(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) != prefix[i]) {
            return false;
        }
    }
    return true;
}

/// T-741's rule in symbol_implied.cpp: the id of the library a group directory sits in.
std::string libraryIdOfStyle(const fs::path& libraryDir) {
    std::string text;
    if (!detail::readFile(genericPathToUtf8(libraryDir / "library.json"), text)) {
        return std::string();
    }
    std::string error;
    const json library = detail::parseBounded(text, detail::kMaxJsonDepth, error);
    return error.empty() && library.is_object() ? detail::getString(library, "id") : std::string();
}

fs::path libraryDirOfGroupStyle(const std::string& groupDir) {
    fs::path dir = pathFromUtf8(groupDir).lexically_normal();
    if (!dir.has_filename()) {
        dir = dir.parent_path();
    }
    return pathToUtf8(dir.parent_path().filename()) == "groups" ? dir.parent_path().parent_path()
                                                                : fs::path();
}

}  // namespace

// --- the vocabulary ---------------------------------------------------------------------------

std::span<const CoreToken> coreTokens() { return kCoreTokens; }

const CoreToken* findCoreToken(std::string_view name) {
    for (const CoreToken& token : kCoreTokens) {
        if (name == token.name) {
            return &token;
        }
    }
    return nullptr;
}

const char* coreTokenTypeName(CoreTokenType type) {
    switch (type) {
        case CoreTokenType::Color:
            return "color";
        case CoreTokenType::Length:
            return "length";
        case CoreTokenType::Time:
            return "time";
        case CoreTokenType::Family:
            return "family";
    }
    return "length";
}

bool coreTokenValueFits(CoreTokenType type, std::string_view value) {
    const std::string v = trimmed(value);
    if (v.empty()) {
        return false;
    }
    if (v.find("var(") != std::string::npos || startsWithCiStyle(v, "calc(")) {
        return true;
    }
    std::string unit;
    switch (type) {
        case CoreTokenType::Color: {
            if (v[0] == '#') {
                const size_t n = v.size() - 1;
                return (n == 3 || n == 4 || n == 6 || n == 8) &&
                       std::all_of(v.begin() + 1, v.end(),
                                   [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
            }
            for (const char* fn : {"rgb(", "rgba(", "hsl(", "hsla(", "color-mix("}) {
                if (startsWithCiStyle(v, fn)) {
                    return true;
                }
            }
            return std::all_of(v.begin(), v.end(),
                               [](char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; });
        }
        case CoreTokenType::Length:
            // A number with a unit that is not a time, angle or frequency unit.
            return v == "0" || (numberWithUnitStyle(v, &unit) && !unit.empty() && unit != "ms" &&
                                unit != "s" && unit != "deg" && unit != "rad" && unit != "turn" &&
                                unit != "grad" && unit != "hz" && unit != "khz");
        case CoreTokenType::Time:
            return numberWithUnitStyle(v, &unit) && (unit == "ms" || unit == "s");
        case CoreTokenType::Family:
            return !numberWithUnitStyle(v, nullptr);
    }
    return false;
}

// --- the record -------------------------------------------------------------------------------

std::string styleValueHash(std::string_view value) { return "sha256:" + util::sha256Hex(trimmed(value)); }

bool readProjectStyle(const UiProject& project, ProjectStyle& out) {
    out = ProjectStyle();
    bool found = false;
    detail::catchAll([&]() {
        fs::path indexPath;
        if (project.rootDir.empty() || !detail::style::projectIndexPath(project, indexPath)) {
            return Status::kOk;
        }
        std::string text;
        if (!detail::readFile(genericPathToUtf8(indexPath), text)) {
            return Status::kOk;
        }
        std::string error;
        const json index = detail::parseBounded(text, detail::kMaxJsonDepth, error);
        if (!error.empty() || !index.is_object() || !index.contains("style") || !index["style"].is_object()) {
            return Status::kOk;
        }
        const json& style = index["style"];
        ProjectStyle s;
        s.library = detail::getString(style, "library");
        s.group = detail::getString(style, "group");
        s.name = detail::getString(style, "name");
        s.set = detail::getString(style, "set");
        s.setVersion = detail::getString(style, "setVersion");
        s.theme = detail::getString(style, "theme");
        if (s.library.empty() || s.group.empty()) {
            return Status::kOk;
        }
        auto hashes = [](const json& obj, std::map<std::string, std::string>& into) {
            if (!obj.is_object()) {
                return;
            }
            for (auto it = obj.begin(); it != obj.end(); ++it) {
                if (it.value().is_string()) {
                    into[it.key()] = it.value().get<std::string>();
                }
            }
        };
        if (style.contains("tokens")) {
            hashes(style["tokens"], s.tokens);
        }
        if (style.contains("themes") && style["themes"].is_object()) {
            for (auto it = style["themes"].begin(); it != style["themes"].end(); ++it) {
                hashes(it.value(), s.themes[it.key()]);
            }
        }
        out = std::move(s);
        found = true;
        return Status::kOk;
    });
    return found;
}

StyleStatus projectStyleStatus(const UiProject& project) {
    StyleStatus status;
    detail::catchAll([&]() {
        status.recorded = readProjectStyle(project, status.style);
        if (!status.recorded) {
            return Status::kOk;
        }
        const fs::path root = detail::style::projectRoot(project);
        auto compare = [&](const std::string& relative, const std::map<std::string, std::string>& hashes,
                           const std::string& prefix) {
            fs::path file;
            std::map<std::string, std::string> values;
            if (!relative.empty() && resolveInside(root, relative, file)) {
                values = detail::style::tokenValues({file});
            }
            for (const auto& [name, hash] : hashes) {
                const auto it = values.find(name);
                if (it == values.end() || styleValueHash(it->second) != hash) {
                    status.changed.push_back(prefix + name);
                }
            }
        };
        compare(project.shared.tokens, status.style.tokens, "");
        const std::string themes =
            project.shared.themes.empty() ? std::string("shared/themes") : project.shared.themes;
        for (const auto& [theme, hashes] : status.style.themes) {
            compare(themes + "/" + theme + ".xts", hashes, theme + ":");
        }
        return Status::kOk;
    });
    return status;
}

// --- the implied layer (47 rule 5) ------------------------------------------------------------

std::vector<std::string> libraryDirsOf(const LibraryMounts& mounts) {
    std::vector<std::string> out;
    for (const SymbolLibrary& library : mounts.libraries) {
        if (!library.dir.empty()) {
            out.push_back(library.dir);
        }
    }
    return out;
}

bool projectImpliedGroup(const std::string& rootDir, const std::vector<ProjectUi>& uis,
                         const std::string& standardSymbolDir, const std::string& symbolsDir,
                         std::span<const std::string> libraryDirs, SymbolGroup& out, std::string* dir) {
    bool result = false;
    std::string chosen;
    const Status status = detail::catchAll([&]() {
        std::vector<std::string> linked;
        for (const ProjectUi& ui : uis) {
            for (const std::string& label :
                 linkedSymbolGroups(rootDir, ui.doc, standardSymbolDir, symbolsDir)) {
                if (std::find(linked.begin(), linked.end(), label) == linked.end()) {
                    linked.push_back(label);
                }
            }
        }
        // Finds `<library>/<group>` (a bare group id: the standard group outside a library).
        auto resolve = [&](const std::string& libraryId, const std::string& groupId) {
            SymbolGroup standard;
            if (!standardSymbolDir.empty() &&
                loadGroup(pathFromUtf8(standardSymbolDir), standard) == Status::kOk &&
                standard.id == groupId) {
                const fs::path standardLibrary = libraryDirOfGroupStyle(standardSymbolDir);
                const std::string standardId =
                    standardLibrary.empty() ? std::string() : libraryIdOfStyle(standardLibrary);
                if (libraryId.empty() || libraryId == standardId) {
                    out = std::move(standard);
                    chosen = standardSymbolDir;
                    return true;
                }
            }
            if (libraryId.empty() || !detail::isGroupId(groupId)) {
                return false;
            }
            std::vector<fs::path> candidates;
            if (!standardSymbolDir.empty()) {
                const fs::path lib = libraryDirOfGroupStyle(standardSymbolDir);
                if (!lib.empty()) {
                    candidates.push_back(lib);
                }
            }
            if (!rootDir.empty()) {
                candidates.push_back(pathFromUtf8(rootDir) / "shared" / "symbol-library");
            }
            for (const std::string& d : libraryDirs) {
                candidates.push_back(pathFromUtf8(d));
            }
            for (const fs::path& lib : candidates) {
                if (libraryIdOfStyle(lib) != libraryId) {
                    continue;
                }
                SymbolGroup group;
                const fs::path groupDir = lib / "groups" / pathFromUtf8(groupId);
                if (loadGroup(groupDir, group) == Status::kOk) {
                    out = std::move(group);
                    chosen = genericPathToUtf8(groupDir);
                    return true;
                }
            }
            return false;
        };
        if (!linked.empty()) {
            UiProject probe;
            probe.rootDir = rootDir;
            probe.shared.symbols = symbolsDir.empty() ? std::string("shared/symbols") : symbolsDir;
            ProjectStyle style;
            if (readProjectStyle(probe, style) && resolve(style.library, style.group)) {
                result = true;
                return Status::kOk;
            }
            if (linked.size() >= 2) {
                const std::string& first = linked.front();
                const size_t slash = first.find('/');
                const bool found = slash == std::string::npos
                                       ? resolve(std::string(), first)
                                       : resolve(first.substr(0, slash), first.substr(slash + 1));
                if (found) {
                    result = true;
                    return Status::kOk;
                }
            }
        }
        if (detectImpliedGroup(rootDir, uis, standardSymbolDir, out, symbolsDir)) {
            chosen = standardSymbolDir;
            result = true;
        }
        return Status::kOk;
    });
    if (status != Status::kOk) {
        result = false;
    }
    if (dir != nullptr) {
        *dir = result ? chosen : std::string();
    }
    return result;
}

std::string projectImpliedTokenFile(const UiProject& project, const std::string& standardSymbolDir,
                                    std::span<const std::string> libraryDirs) {
    if (project.rootDir.empty()) {
        return std::string();
    }
    SymbolGroup group;
    std::string dir;
    if (!projectImpliedGroup(project.rootDir, project.uis, standardSymbolDir, project.shared.symbols,
                             libraryDirs, group, &dir) ||
        group.tokensFile.empty()) {
        return std::string();
    }
    return genericPathToUtf8(pathFromUtf8(dir) / pathFromUtf8(group.tokensFile));
}

// --- helpers (style_internal.h) ---------------------------------------------------------------

namespace detail::style {

std::string urlPath(std::string_view value) {
    const size_t at = value.find("url(");
    if (at == std::string_view::npos) {
        return std::string();
    }
    std::string_view rest = value.substr(at + 4);
    const size_t close = rest.find(')');
    if (close == std::string_view::npos) {
        return std::string();
    }
    rest = rest.substr(0, close);
    while (!rest.empty() && (rest.front() == ' ' || rest.front() == '"' || rest.front() == '\'')) {
        rest.remove_prefix(1);
    }
    while (!rest.empty() && (rest.back() == ' ' || rest.back() == '"' || rest.back() == '\'')) {
        rest.remove_suffix(1);
    }
    return std::string(rest);
}

std::vector<std::string> varRefs(std::string_view text) {
    std::vector<std::string> out;
    for (size_t at = text.find("var("); at != std::string_view::npos; at = text.find("var(", at + 4)) {
        size_t i = at + 4;
        while (i < text.size() && isSpaceCharStyle(text[i])) {
            ++i;
        }
        if (text.substr(i, 2) != "--") {
            continue;
        }
        size_t end = i + 2;
        while (end < text.size() && isNameCharStyle(text[end])) {
            ++end;
        }
        if (end > i + 2) {
            std::string name(text.substr(i, end - i));
            if (std::find(out.begin(), out.end(), name) == out.end()) {
                out.push_back(std::move(name));
            }
        }
    }
    return out;
}

std::string replaceTokenValue(std::string text, std::string_view name, std::string_view value, bool* found) {
    bool any = false;
    size_t i = 0;
    char prev = '{';  // The last significant character before the current position.
    while (i < text.size()) {
        const char c = text[i];
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            const size_t end = text.find("*/", i + 2);
            i = end == std::string::npos ? text.size() : end + 2;
            continue;
        }
        if (c == '"' || c == '\'') {
            const size_t end = text.find(c, i + 1);
            i = end == std::string::npos ? text.size() : end + 1;
            prev = c;
            continue;
        }
        if ((prev == '{' || prev == ';') && text.compare(i, name.size(), name) == 0 &&
            (i + name.size() >= text.size() || !isNameCharStyle(text[i + name.size()]))) {
            size_t j = i + name.size();
            while (j < text.size() && isSpaceCharStyle(text[j])) {
                ++j;
            }
            if (j < text.size() && text[j] == ':') {
                size_t v = j + 1;
                // The value runs to the `;` or `}` outside parentheses and strings.
                int depth = 0;
                size_t e = v;
                while (e < text.size()) {
                    const char d = text[e];
                    if (d == '"' || d == '\'') {
                        const size_t close = text.find(d, e + 1);
                        e = close == std::string::npos ? text.size() : close + 1;
                        continue;
                    }
                    if (d == '(') {
                        ++depth;
                    } else if (d == ')') {
                        depth = std::max(0, depth - 1);
                    } else if ((d == ';' || d == '}') && depth == 0) {
                        break;
                    }
                    ++e;
                }
                const std::string replacement = " " + std::string(value);
                text.replace(v, e - v, replacement);
                any = true;
                i = v + replacement.size();
                prev = ':';
                continue;
            }
        }
        if (!isSpaceCharStyle(c)) {
            prev = c;
        }
        ++i;
    }
    if (found != nullptr) {
        *found = any;
    }
    return text;
}

std::string appendTokenSection(std::string text, bool existed, const std::string& created,
                               const std::string& title, const std::vector<std::string>& faces,
                               const std::vector<std::pair<std::string, std::string>>& tokens) {
    if (faces.empty() && tokens.empty()) {
        return text;
    }
    if (!existed) {
        text = created;
    }
    if (!text.empty() && text.back() != '\n') {
        text += '\n';
    }
    text += "\n/* " + title + " */\n";
    for (const std::string& face : faces) {
        text += face + "\n";
    }
    if (!tokens.empty()) {
        text += ":root {\n";
        for (const auto& [name, value] : tokens) {
            text += "  " + name + ": " + value + ";\n";
        }
        text += "}\n";
    }
    return text;
}

std::map<std::string, std::string> sectionOrigins(std::string_view text) {
    std::map<std::string, std::string> out;
    std::string origin = "project";
    size_t i = 0;
    char prev = '{';
    while (i < text.size()) {
        const char c = text[i];
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            const size_t end = text.find("*/", i + 2);
            const std::string_view comment =
                text.substr(i + 2, (end == std::string_view::npos ? text.size() : end) - i - 2);
            const std::string body = trimmed(comment);
            if (body.rfind("added with ", 0) == 0) {
                const size_t from = body.rfind(" from ");
                if (from != std::string::npos) {
                    origin = trimmed(std::string_view(body).substr(from + 6));
                }
            }
            i = end == std::string_view::npos ? text.size() : end + 2;
            continue;
        }
        if ((prev == '{' || prev == ';') && text.substr(i, 2) == "--") {
            size_t e = i + 2;
            while (e < text.size() && isNameCharStyle(text[e])) {
                ++e;
            }
            out.emplace(std::string(text.substr(i, e - i)), origin);
            i = e;
            prev = '-';
            continue;
        }
        if (!isSpaceCharStyle(c)) {
            prev = c;
        }
        ++i;
    }
    return out;
}

fs::path projectRoot(const UiProject& project) {
    if (project.rootDir.empty()) {
        return fs::path();
    }
    std::error_code ec;
    return fs::absolute(pathFromUtf8(project.rootDir), ec).lexically_normal();
}

std::vector<ProjectSymbolFile> projectSymbolFiles(const UiProject& project) {
    std::vector<ProjectSymbolFile> flat;
    std::vector<ProjectSymbolFile> nested;
    fs::path symbolsDir;
    if (project.rootDir.empty() ||
        !resolveInside(projectRoot(project), projectSymbolsRel(project), symbolsDir)) {
        return flat;
    }
    std::error_code ec;
    for (fs::directory_iterator it(symbolsDir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        if (it->is_regular_file(ec) && p.extension() == ".xt" && isPlainSymbolName(pathToUtf8(p.stem()))) {
            flat.push_back(ProjectSymbolFile{pathToUtf8(p.stem()), p});
        } else if (it->is_directory(ec) && isGroupId(pathToUtf8(p.filename()))) {
            std::error_code inner;
            for (fs::directory_iterator jt(p, inner), jend; !inner && jt != jend; jt.increment(inner)) {
                const fs::path& q = jt->path();
                if (jt->is_regular_file(inner) && q.extension() == ".xt" &&
                    isPlainSymbolName(pathToUtf8(q.stem()))) {
                    nested.push_back(
                        ProjectSymbolFile{pathToUtf8(p.filename()) + "/" + pathToUtf8(q.stem()), q});
                }
            }
        }
    }
    auto byName = [](const ProjectSymbolFile& a, const ProjectSymbolFile& b) {
        return a.qualified < b.qualified;
    };
    std::sort(flat.begin(), flat.end(), byName);
    std::sort(nested.begin(), nested.end(), byName);
    flat.insert(flat.end(), nested.begin(), nested.end());
    return flat;
}

std::map<std::string, std::string> tokenValues(const std::vector<fs::path>& files) {
    std::map<std::string, std::string> out;
    for (const fs::path& file : files) {
        std::string text;
        const std::string utf8 = genericPathToUtf8(file);
        if (file.empty() || !readFile(utf8, text)) {
            continue;
        }
        compile::RuleSet rules;
        compile::DiagnosticSink sink;
        compile::parseXts(text, utf8, 1, sink, rules);
        for (const compile::TokenDecl& token : rules.tokens) {
            out[token.name] = trimmed(token.value);  // A later declaration wins, as in the compiler.
        }
    }
    return out;
}

std::string resolveVars(const std::string& value, const std::map<std::string, std::string>& values,
                        bool* ok) {
    std::string text = value;
    bool good = true;
    for (int step = 0; step < 64; ++step) {  // A cycle stops here and reads as unresolved.
        const size_t at = text.find("var(");
        if (at == std::string::npos) {
            break;
        }
        // The matching `)`.
        size_t close = at + 4;
        int nest = 1;
        while (close < text.size() && nest > 0) {
            if (text[close] == '(') {
                ++nest;
            } else if (text[close] == ')') {
                --nest;
            }
            if (nest > 0) {
                ++close;
            }
        }
        if (close >= text.size()) {
            good = false;
            break;
        }
        const std::string inner = text.substr(at + 4, close - at - 4);
        const size_t comma = inner.find(',');
        const std::string name = trimmed(std::string_view(inner).substr(0, comma));
        std::string replacement;
        const auto it = values.find(name);
        if (it != values.end()) {
            replacement = it->second;
        } else if (comma != std::string::npos) {
            replacement = trimmed(std::string_view(inner).substr(comma + 1));
        } else {
            good = false;
            break;
        }
        text.replace(at, close + 1 - at, replacement);
    }
    if (text.find("var(") != std::string::npos) {
        good = false;
    }
    if (ok != nullptr) {
        *ok = good;
    }
    return text;
}

bool projectIndexPath(const UiProject& project, fs::path& out) {
    return resolveInside(projectRoot(project), projectSymbolsRel(project) + "/index.json", out);
}

std::string readText(const fs::path& file, bool* existed) {
    std::string text;
    const bool ok = !file.empty() && readFile(genericPathToUtf8(file), text);
    if (existed != nullptr) {
        *existed = ok;
    }
    return ok ? text : std::string();
}

}  // namespace detail::style

}  // namespace xt::bundle
