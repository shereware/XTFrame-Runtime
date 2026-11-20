#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "bundle_json.h"
#include "style_internal.h"
#include "symbol_internal.h"
#include "xt/bundle/project_style.h"
#include "xt/compile/utf8_path.h"

/// T-743 (Docs/design/56-symbol-identity-style-scope.md 56.2 item 2): "duplicate and detach" -- an
/// independent copy of a project symbol that reads tokens of its own, so the project's style can
/// change without it. Planned only; the caller writes the changes as one undo step.

namespace xt::bundle {

namespace fs = std::filesystem;
using compile::genericPathToUtf8;
using compile::pathFromUtf8;
using compile::pathToUtf8;
using detail::json;
using detail::resolveInside;
namespace st = detail::style;

namespace {

bool nameCharDetach(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) != 0 || c == '-' || c == '_' || u >= 0x80;
}

/// Every `var(--from` (followed by a non-name character) of `text` reading `var(--to` instead.
std::string renameVarRefDetach(std::string text, const std::string& from, const std::string& to) {
    for (size_t at = text.find("var("); at != std::string::npos; at = text.find("var(", at + 4)) {
        size_t i = at + 4;
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) {
            ++i;
        }
        if (text.compare(i, from.size(), from) == 0 &&
            (i + from.size() >= text.size() || !nameCharDetach(text[i + from.size()]))) {
            text.replace(i, from.size(), to);
        }
    }
    return text;
}

/// Replaces every `before` of `text` with `after`.
std::string replaceAllDetach(std::string text, const std::string& before, const std::string& after) {
    for (size_t at = text.find(before); at != std::string::npos; at = text.find(before, at + after.size())) {
        text.replace(at, before.size(), after);
    }
    return text;
}

/// The `<symbol ... name="<bare>"` of `text` renamed to `newName`; false when there is none.
bool renameDefinitionDetach(std::string& text, const std::string& bare, const std::string& newName) {
    for (size_t at = text.find("<symbol"); at != std::string::npos; at = text.find("<symbol", at + 7)) {
        const char next = at + 7 < text.size() ? text[at + 7] : '\0';
        if (next != ' ' && next != '\t' && next != '\n' && next != '\r') {
            continue;
        }
        const size_t close = text.find('>', at);
        const std::string needle = "name=\"" + bare + "\"";
        const size_t name = text.find(needle, at);
        if (name != std::string::npos && name < close) {
            text.replace(name, needle.size(), "name=\"" + newName + "\"");
            return true;
        }
    }
    return false;
}

/// For a file moving from `fromDir` to `toDir`: each relative path of `url(...)`, `src="..."` and
/// `href="..."` re-based on `toDir`.
std::string rebasePathsDetach(std::string text, const fs::path& fromDir, const fs::path& toDir) {
    auto rebase = [&](const std::string& path) -> std::string {
        if (path.empty() || path.find(':') != std::string::npos || path.front() == '/' ||
            path.front() == '#') {
            return path;
        }
        return genericPathToUtf8((fromDir / pathFromUtf8(path)).lexically_normal().lexically_relative(toDir));
    };
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        size_t open = std::string::npos;
        char quote = '\0';
        size_t close = std::string::npos;
        for (const char* key : {"url(", "src=\"", "href=\""}) {
            const size_t at = text.find(key, i);
            if (at != std::string::npos && (open == std::string::npos || at < open)) {
                open = at;
                quote = key[0] == 'u' ? ')' : '"';
                close = at + std::char_traits<char>::length(key);
            }
        }
        if (open == std::string::npos) {
            break;
        }
        size_t start = close;
        if (quote == ')') {
            while (start < text.size() && (text[start] == ' ' || text[start] == '"' || text[start] == '\'')) {
                ++start;
            }
        }
        size_t end = text.find(quote, start);
        if (end == std::string::npos) {
            break;
        }
        size_t pathEnd = end;
        if (quote == ')') {
            while (pathEnd > start &&
                   (text[pathEnd - 1] == ' ' || text[pathEnd - 1] == '"' || text[pathEnd - 1] == '\'')) {
                --pathEnd;
            }
        }
        out += text.substr(i, start - i);
        out += rebase(text.substr(start, pathEnd - start));
        i = pathEnd;
    }
    out += text.substr(i);
    return out;
}

std::string lowerAsciiDetach(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

void detachImplDetach(const UiProject& project, std::string_view qualifiedIn, std::string_view newNameIn,
                      std::span<const std::string> impliedFiles, DetachReport& report) {
    auto fail = [&](Status status, std::string message) {
        report.status = status;
        report.error = std::move(message);
    };
    const std::string qualified(qualifiedIn);
    const std::string newName(newNameIn);
    std::string ns;
    std::string bare;
    if (project.rootDir.empty() || project.isUnnamed()) {
        return fail(Status::kInvalidArgument, "no UI Project is open");
    }
    if (!detail::splitQualifiedName(qualified, ns, bare)) {
        return fail(Status::kInvalidArgument, "not a project symbol name: " + qualified);
    }
    if (!detail::isPlainSymbolName(newName) || newName == bare) {
        return fail(Status::kInvalidArgument, "not a new plain symbol name: " + newName);
    }
    const fs::path root = st::projectRoot(project);
    const std::string symbolsRel = detail::projectSymbolsRel(project);
    fs::path symbolsDir;
    fs::path source;
    fs::path target;
    if (!resolveInside(root, symbolsRel, symbolsDir) ||
        !resolveInside(root, symbolsRel + "/" + qualified + ".xt", source) ||
        !resolveInside(root, symbolsRel + "/" + newName + ".xt", target)) {
        return fail(Status::kInvalidArgument, "shared.symbols leaves the project");
    }
    bool existed = false;
    const std::string original = st::readText(source, &existed);
    if (!existed) {
        return fail(Status::kNotFound, "no project symbol " + qualified);
    }
    std::error_code ec;
    if (fs::exists(target, ec)) {
        return fail(Status::kInvalidArgument, "the project already has a symbol " + newName);
    }
    std::string text = original;
    if (!renameDefinitionDetach(text, bare, newName)) {
        return fail(Status::kNotFound, symbolsRel + "/" + qualified + ".xt does not define " + bare);
    }
    text = replaceAllDetach(text, "symbol(" + bare + ")", "symbol(" + newName + ")");
    if (!ns.empty()) {
        // Bare names of a namespaced file looked in its namespace first; flat, they must say so.
        const fs::path nsDir = symbolsDir / pathFromUtf8(ns);
        for (fs::directory_iterator it(nsDir, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string sibling = pathToUtf8(it->path().stem());
            if (it->path().extension() != ".xt" || sibling == bare || !detail::isPlainSymbolName(sibling)) {
                continue;
            }
            text =
                replaceAllDetach(text, "symbol=\"" + sibling + "\"", "symbol=\"" + ns + "/" + sibling + "\"");
            text = replaceAllDetach(text, "symbol(" + sibling + ")", "symbol(" + ns + "/" + sibling + ")");
        }
        text = rebasePathsDetach(text, nsDir.lexically_normal(), symbolsDir.lexically_normal());
    }

    // The tokens: the project's values now (implied layer underneath), var() resolved.
    fs::path tokensFile;
    const bool haveTokens =
        !project.shared.tokens.empty() && resolveInside(root, project.shared.tokens, tokensFile);
    std::vector<fs::path> layers;
    for (const std::string& f : impliedFiles) {
        layers.push_back(pathFromUtf8(f));
    }
    const std::map<std::string, std::string> projectValues =
        haveTokens ? st::tokenValues({tokensFile}) : std::map<std::string, std::string>();
    if (haveTokens) {
        layers.push_back(tokensFile);
    }
    const std::map<std::string, std::string> values = st::tokenValues(layers);
    const std::string prefix = "--" + lowerAsciiDetach(newName) + "-";
    std::vector<std::pair<std::string, std::string>> appended;
    std::map<std::string, std::string> baseResolved;  // old name -> value
    for (const std::string& name : st::varRefs(original)) {
        const auto it = values.find(name);
        bool ok = it != values.end();
        const std::string value = ok ? st::resolveVars(it->second, values, &ok) : std::string();
        if (!ok || !haveTokens ||
            (value.find("url(") != std::string::npos && projectValues.count(name) == 0)) {
            report.unresolved.push_back(name);
            continue;
        }
        const std::string to = prefix + name.substr(2);
        text = renameVarRefDetach(text, name, to);
        report.tokens.push_back(DetachedToken{name, to, value});
        appended.emplace_back(to, value);
        baseResolved[name] = value;
    }
    const std::string title = "detached: " + newName + " (from " + qualified + ")";
    std::vector<FileChange> changes;
    report.name = newName;
    report.file = detail::relativeText(root, target);
    changes.push_back(FileChange{report.file, false, std::string(), text});
    if (haveTokens && !appended.empty()) {
        FileChange change;
        change.path = detail::relativeText(root, tokensFile);
        change.before = st::readText(tokensFile, &change.existed);
        change.after = st::appendTokenSection(change.before, change.existed,
                                              "/* Design tokens of this UI Project (shared.tokens). */\n",
                                              title, {}, appended);
        changes.push_back(std::move(change));
    }
    // Each theme that gives one of them another value gets the new token too.
    const std::string themesRel =
        project.shared.themes.empty() ? std::string("shared/themes") : project.shared.themes;
    fs::path themesDir;
    if (!report.tokens.empty() && resolveInside(root, themesRel, themesDir)) {
        std::vector<fs::path> themeFiles;
        for (fs::directory_iterator it(themesDir, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec) && it->path().extension() == ".xts") {
                themeFiles.push_back(it->path());
            }
        }
        std::sort(themeFiles.begin(), themeFiles.end());
        for (const fs::path& themeFile : themeFiles) {
            std::vector<fs::path> themeLayers = layers;
            themeLayers.push_back(themeFile);
            const std::map<std::string, std::string> themeValues = st::tokenValues(themeLayers);
            std::vector<std::pair<std::string, std::string>> themed;
            for (const DetachedToken& token : report.tokens) {
                bool ok = false;
                const auto it = themeValues.find(token.from);
                const std::string value =
                    it == themeValues.end() ? std::string() : st::resolveVars(it->second, themeValues, &ok);
                if (ok && value != baseResolved[token.from] && value.find("url(") == std::string::npos) {
                    themed.emplace_back(token.to, value);
                }
            }
            if (themed.empty()) {
                continue;
            }
            FileChange change;
            change.path = detail::relativeText(root, themeFile);
            change.before = st::readText(themeFile, &change.existed);
            change.after =
                st::appendTokenSection(change.before, change.existed, std::string(), title, {}, themed);
            changes.push_back(std::move(change));
        }
    }
    // index.json: the copy is the project's own (no `source`); `derivedFrom` says where it came from.
    fs::path indexPath;
    if (st::projectIndexPath(project, indexPath)) {
        FileChange change;
        change.path = detail::relativeText(root, indexPath);
        change.before = st::readText(indexPath, &change.existed);
        nlohmann::ordered_json index;
        std::string error;
        const json parsed = detail::parseBounded(change.before, detail::kMaxJsonDepth, error);
        if (change.existed && error.empty() && parsed.is_object()) {
            index = nlohmann::ordered_json::parse(change.before, nullptr, false);
        }
        if (!index.is_object()) {
            index = nlohmann::ordered_json::object();
            index["version"] = 1;
        }
        if (!index.contains("symbols") || !index["symbols"].is_array()) {
            index["symbols"] = nlohmann::ordered_json::array();
        }
        nlohmann::ordered_json derived = nlohmann::ordered_json::object();
        CopySource from;
        if (readCopySource(project, qualified, from)) {
            derived["library"] = from.library;
            derived["group"] = from.group;
            derived["name"] = bare;
            derived["setVersion"] = from.setVersion;
        } else {
            derived["project"] = qualified;
        }
        nlohmann::ordered_json entry;
        entry["name"] = newName;
        entry["qualified"] = newName;
        entry["file"] = report.file;
        entry["derivedFrom"] = std::move(derived);
        index["symbols"].push_back(std::move(entry));
        change.after = index.dump(2, ' ', false, nlohmann::ordered_json::error_handler_t::replace) + "\n";
        changes.push_back(std::move(change));
    }
    report.changes = std::move(changes);
}

}  // namespace

DetachReport duplicateDetached(const UiProject& project, std::string_view qualified, std::string_view newName,
                               std::span<const std::string> impliedTokenFiles) {
    DetachReport report;
    const Status status = detail::catchAll([&]() {
        detachImplDetach(project, qualified, newName, impliedTokenFiles, report);
        return Status::kOk;
    });
    if (status != Status::kOk) {
        report = DetachReport();
        report.status = Status::kInternal;
        report.error = "unexpected failure while detaching the symbol";
    }
    return report;
}

}  // namespace xt::bundle
