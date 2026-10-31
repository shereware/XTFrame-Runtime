#include "xt/bundle/symbol_lint.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "symbol_internal.h"
#include "xt/bundle/symbol_library.h"
#include "xt/charconv.h"
#include "xt/compile/compile.h"
#include "xt/compile/utf8_path.h"
#include "xt/ir.h"
#include "xt/util/sha256.h"
#include "xt/util/text.h"

/// T-499 (Docs/design/49-symbol-sets.md 49.6), see the header. The scanners are plain character
/// loops, not std::regex: a symbol group is untrusted input (14 §14.9) and a backtracking regex over
/// a large file can exhaust the stack. Helpers live in `xt::bundle::detail::lint` so a unity build
/// of xt_bundle never sees two files' helpers under one name.

namespace xt::bundle {

namespace fs = std::filesystem;
using compile::genericPathToUtf8;
using compile::pathFromUtf8;
using compile::pathToUtf8;

namespace detail::lint {

constexpr float kMaxTimelineSeconds = 0.5f;  // contracts/motion-spec.yaml duration.slow upper bound.
constexpr int kMaxPreviewSize = 4096;
constexpr int kMaxPreviewFrames = 600;
constexpr size_t kMaxPreviewEvents = 256;
constexpr size_t kMaxPreviewLine = 2048;
constexpr size_t kMaxPreviewPorts = 256;

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isWord(char c) { return isAlpha(c) || isDigit(c) || c == '_'; }
bool isHex(char c) { return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

std::string stripComments(std::string_view css) {
    std::string out;
    out.reserve(css.size());
    size_t i = 0;
    while (i < css.size()) {
        if (css[i] == '/' && i + 1 < css.size() && css[i + 1] == '*') {
            const size_t end = css.find("*/", i + 2);
            if (end == std::string_view::npos) {
                out.append(css.substr(i));  // An unterminated comment is not one.
                break;
            }
            out += ' ';
            i = end + 2;
            continue;
        }
        out += css[i++];
    }
    return out;
}

bool hasHexColor(std::string_view value) {
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '#') {
            continue;
        }
        size_t end = i + 1;
        while (end < value.size() && isHex(value[end])) {
            ++end;
        }
        const size_t digits = end - i - 1;
        if (digits >= 3 && digits <= 8 && (end == value.size() || !isWord(value[end]))) {
            return true;
        }
    }
    return false;
}

bool hasColorFunction(std::string_view value) {
    for (const std::string_view name : {"rgba(", "rgb(", "hsla(", "hsl("}) {
        for (size_t at = value.find(name); at != std::string_view::npos; at = value.find(name, at + 1)) {
            if (at == 0 || !isWord(value[at - 1])) {
                return true;
            }
        }
    }
    return false;
}

bool hasNamedColor(std::string_view value) {
    static constexpr std::string_view kNames[] = {"black",  "silver",  "gray",  "white", "maroon", "red",
                                                  "purple", "fuchsia", "green", "lime",  "olive",  "yellow",
                                                  "navy",   "blue",    "teal",  "aqua",  "orange"};
    size_t start = 0;
    while (start <= value.size()) {
        size_t end = start;
        while (end < value.size() && !isSpace(value[end]) && value[end] != ',' && value[end] != '(' &&
               value[end] != ')') {
            ++end;
        }
        const std::string_view piece = value.substr(start, end - start);
        for (const std::string_view name : kNames) {
            if (piece == name) {
                return true;
            }
        }
        start = end + 1;
    }
    return false;
}

/// Every `name` of `text` of `[a-z0-9_-]` characters after `prefix` `var(` ... : the sorted set of
/// `--name` custom properties a file reads through `var(--name)`, without the dashes.
std::vector<std::string> varNames(const std::string& text) {
    std::set<std::string> names;
    for (size_t at = text.find("var("); at != std::string::npos; at = text.find("var(", at + 4)) {
        size_t i = at + 4;
        while (i < text.size() && isSpace(text[i])) {
            ++i;
        }
        if (text.compare(i, 2, "--") != 0) {
            continue;
        }
        i += 2;
        size_t end = i;
        while (end < text.size() && (isWord(text[end]) || text[end] == '-')) {
            ++end;
        }
        if (end > i) {
            names.insert(text.substr(i, end - i));
        }
    }
    return {names.begin(), names.end()};
}

/// The text between the double quotes of every `"..."` in `value`.
std::vector<std::string> quotedStrings(const std::string& value) {
    std::vector<std::string> out;
    size_t at = 0;
    while ((at = value.find('"', at)) != std::string::npos) {
        const size_t end = value.find('"', at + 1);
        if (end == std::string::npos) {
            break;
        }
        out.push_back(value.substr(at + 1, end - at - 1));
        at = end + 1;
    }
    return out;
}

/// The path of every `url(...)` in `value`.
std::vector<std::string> urlPaths(const std::string& value) {
    std::vector<std::string> out;
    size_t at = 0;
    while ((at = value.find("url(", at)) != std::string::npos) {
        const size_t close = value.find(')', at + 4);
        if (close == std::string::npos) {
            break;
        }
        std::string path = trimmed(value.substr(at + 4, close - at - 4));
        if (path.size() >= 2 && (path.front() == '"' || path.front() == '\'') &&
            path.back() == path.front()) {
            path = path.substr(1, path.size() - 2);
        }
        if (!path.empty()) {
            out.push_back(std::move(path));
        }
        at = close + 1;
    }
    return out;
}

void addUnique(std::vector<std::string>& list, const std::string& item) {
    if (std::find(list.begin(), list.end(), item) == list.end()) {
        list.push_back(item);
    }
}

/// The fonts and textures a symbol reading the tokens `names` (without `--`) needs: for every sheet
/// in order (the group's token file, then each theme file), the `@font-face src` of each family a
/// `--font-*` token names and the `url()` of each `--tex-*` token.
void dependencies(const std::vector<std::string>& names, const std::vector<TokenSheet>& sheets,
                  std::vector<std::string>& fonts, std::vector<std::string>& textures) {
    for (const TokenSheet& sheet : sheets) {
        for (const std::string& name : names) {
            const auto it = sheet.values.find("--" + name);
            const std::string value = it == sheet.values.end() ? std::string() : it->second;
            if (util::startsWith(name, "font-")) {
                for (const std::string& family : quotedStrings(value)) {
                    const auto face = sheet.faces.find(family);
                    if (face != sheet.faces.end()) {
                        for (const std::string& src : face->second) {
                            addUnique(fonts, src);
                        }
                    }
                }
            } else if (util::startsWith(name, "tex-")) {
                for (const std::string& path : urlPaths(value)) {
                    addUnique(textures, path);
                }
            }
        }
    }
}

std::string joinList(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& item : items) {
        out += (out.empty() ? "" : ", ") + item;
    }
    return out;
}

/// The `requires="..."` text the index entry's contract spells (`states: A, B; slots: x`).
std::string contractText(const SymbolEntry& entry) {
    std::string text;
    if (!entry.requiredStates.empty()) {
        text = "states: " + joinList(entry.requiredStates);
    }
    if (!entry.requiredSlots.empty()) {
        text += (text.empty() ? "" : "; ") + std::string("slots: ") + joinList(entry.requiredSlots);
    }
    return text;
}

/// The `requires` attribute value of the `<symbol name="<name>" ...>` start tag; empty when the
/// tag has none; `found` says whether the tag exists.
std::string requiresAttribute(const std::string& text, const std::string& name, bool& found) {
    const size_t tag = text.find("<symbol name=\"" + name + "\"");
    found = tag != std::string::npos;
    if (!found) {
        return std::string();
    }
    const size_t close = text.find('>', tag);
    const std::string head = text.substr(tag, close == std::string::npos ? std::string::npos : close - tag);
    const size_t attr = head.find("requires=\"");
    if (attr == std::string::npos) {
        return std::string();
    }
    const size_t end = head.find('"', attr + 10);
    return end == std::string::npos ? std::string() : head.substr(attr + 10, end - attr - 10);
}

std::string escapeAngle(std::string text) {
    std::string out;
    for (const char c : text) {
        out += c == '<' ? "&lt;" : c == '>' ? "&gt;" : std::string(1, c);
    }
    return out;
}

std::set<std::string> toSet(const std::vector<std::string>& items) { return {items.begin(), items.end()}; }

std::string describeSet(const std::set<std::string>& items) {
    std::string out;
    for (const std::string& item : items) {
        out += (out.empty() ? "" : ", ") + item;
    }
    return out.empty() ? "none" : out;
}

struct Lint {
    LintReport& report;
    const LintOptions& options;
    fs::path dir;

    void add(LintLevel level, const std::string& symbol, const char* code, std::string message) {
        report.issues.push_back(LintIssue{level, symbol, code, std::move(message)});
    }
    void error(const std::string& symbol, const char* code, std::string message) {
        add(LintLevel::Error, symbol, code, std::move(message));
    }
    void warn(const std::string& symbol, const char* code, std::string message) {
        add(LintLevel::Warning, symbol, code, std::move(message));
    }
    bool fileExists(const std::string& relative) const {
        fs::path path;
        std::error_code ec;
        return resolveInside(dir, relative, path) && fs::is_regular_file(path, ec);
    }
};

void compileIssues(Lint& lint, const std::string& symbol, const compile::CompileResult& result) {
    for (const compile::Diagnostic& d : result.diagnostics) {
        std::string where = pathToUtf8(pathFromUtf8(d.file).filename());
        if (d.line > 0) {
            where += ":" + std::to_string(d.line);
        }
        const std::string text = where + ": " + d.message;
        const bool failed = !result.ok();
        if (d.message.find("requires state") != std::string::npos) {
            lint.error(symbol, "contract.state", text);
        } else if (d.message.find("requires slot") != std::string::npos) {
            lint.error(symbol, "contract.slot", text);
        } else if (d.level == compile::Diagnostic::Level::Error || failed) {
            lint.error(symbol, "compile.error", text);  // --strict: a warning fails the compile.
        } else {
            lint.warn(symbol, "compile.warning", text);
        }
    }
    if (!result.ok() && result.diagnostics.empty()) {
        lint.error(symbol, "compile.error", "the preview does not compile");
    }
}

bool instantiates(const compile::CompileResult& result, const std::string& name) {
    Result<IrView> view = IrView::open(IrBlob{result.ir.data(), result.ir.size()});
    if (!view.ok()) {
        return false;
    }
    for (const IrSymbolInstance& instance : view.value().symbols().instances()) {
        if (view.value().string(instance.symbol) == name) {
            return true;
        }
    }
    return false;
}

/// The strict compile of the symbol's preview (or a synthetic minimal one) and what it says.
void checkPreview(Lint& lint, const SymbolGroup& group, const SymbolEntry& entry) {
    const fs::path previews = lint.dir / "previews";
    const fs::path xt = previews / pathFromUtf8(entry.name + ".preview.xt");
    const fs::path json = previews / pathFromUtf8(entry.name + ".preview.json");
    std::error_code ec;
    const bool hasXt = fs::is_regular_file(xt, ec);
    const bool hasJson = fs::is_regular_file(json, ec);
    if (hasJson) {
        PreviewSpec spec;
        std::string error;
        if (const Status status = readPreviewSpec(json, spec, error); status != Status::kOk) {
            lint.error(entry.name, "preview.spec", "previews/" + entry.name + ".preview.json: " + error);
        }
        if (!hasXt) {
            lint.error(entry.name, "preview.spec",
                       "previews/" + entry.name + ".preview.json has no .preview.xt");
        }
    } else if (hasXt) {
        lint.warn(entry.name, "preview.json",
                  "previews/" + entry.name + ".preview.xt has no .preview.json: no thumbnail can be made");
    }

    compile::CompileOptions options;
    options.strict = true;
    options.searchDirs = {genericPathToUtf8(lint.dir)};
    compile::CompileResult result;
    if (hasXt) {
        options.rootDir = genericPathToUtf8(previews);
        result = compile::compileDocument(genericPathToUtf8(xt), options);
    } else {
        options.rootDir = genericPathToUtf8(lint.dir);
        options.xtDir = genericPathToUtf8(lint.dir);
        std::string doc = "<xt><head>";
        if (!group.tokensFile.empty()) {
            doc += "<link href=\"" + group.tokensFile + "\"/>";
        }
        doc += "<link rel=\"symbols\" href=\"" + entry.file + "\"/></head><body><use symbol=\"" + entry.name +
               "\" id=\"Preview\"/></body></xt>";
        const compile::SourceFile files[] = {{"xt-lint-preview.xt", doc}};
        result = compile::compileDocumentFromStrings(files, "xt-lint-preview.xt", options);
    }
    compileIssues(lint, entry.name, result);
    if (hasXt && result.ok() && !instantiates(result, entry.name)) {
        lint.error(entry.name, "preview.noInstance",
                   "previews/" + entry.name + ".preview.xt does not instantiate " + entry.name);
    }
}

void checkStyles(Lint& lint, const SymbolEntry& entry, const std::string& text) {
    for (const std::string& region : styleRegions(text, false)) {
        for (const std::string& found : findLiteralColors(region)) {
            lint.error(entry.name, "style.literalColor", found);
        }
    }
    for (const TimelineScan& timeline : scanTimelines(text)) {
        if (!isConventionalTimelineName(timeline.name)) {
            lint.warn(entry.name, "timeline.name",
                      "'" + timeline.name + "' is not a lower-case kebab-case name (28 1.3)");
        }
        if (timeline.name == "cooldown") {
            continue;  // A progress timeline the host seeks, not motion.
        }
        if (timeline.lastTime > kMaxTimelineSeconds) {
            lint.error(entry.name, "timeline.duration",
                       "'" + timeline.name + "' lasts " +
                           std::to_string(static_cast<int>(timeline.lastTime * 1000.0f + 0.5f)) +
                           " ms, the motion spec allows 500 ms");
        }
        for (const std::string& prop : timeline.props) {
            if (prop != "transform" && prop != "opacity" && prop != "text-reveal") {
                lint.error(entry.name, "timeline.property",
                           "'" + timeline.name + "' animates '" + prop +
                               "', the motion spec allows transform, opacity and text-reveal");
            }
        }
    }
}

void lintSymbol(Lint& lint, const SymbolGroup& group, const SymbolEntry& entry,
                const std::vector<TokenSheet>& sheets, const json& rawEntry) {
    const std::string& name = entry.name;
    fs::path file;
    std::string text;
    if (!resolveInside(lint.dir, entry.file, file) || !readFile(genericPathToUtf8(file), text)) {
        lint.error(name, "symbol.fileMissing", "missing file " + entry.file);
        return;
    }

    bool tagFound = false;
    const std::string requiresText = requiresAttribute(text, name, tagFound);
    if (!tagFound) {
        lint.error(name, "symbol.name", entry.file + " has no <symbol name=\"" + name + "\"");
    } else if (requiresText != contractText(entry)) {
        lint.error(name, "index.contract",
                   "requires=\"" + requiresText + "\" but the index says \"" + contractText(entry) + "\"");
    }
    for (const SymbolPortInfo& port : entry.ports) {
        const std::string decl = "port:" + port.name + "=\"" + escapeAngle(port.type) + " " + port.dir + "\"";
        if (text.find(decl) == std::string::npos) {
            lint.error(name, "index.port", "no " + decl);
        }
    }
    std::set<std::string> declared;
    for (const TimelineScan& timeline : scanTimelines(text)) {
        declared.insert(timeline.name);
    }
    for (const std::string& timeline : entry.timelines) {
        if (declared.count(timeline) == 0) {
            lint.error(name, "index.timeline", "no @timeline " + timeline);
        }
    }

    // Tokens: the var(--name) set and the defaults of the token file.
    const std::vector<std::string> used = varNames(text);
    std::set<std::string> usedFull;
    const TokenSheet empty;
    const TokenSheet& base = sheets.empty() ? empty : sheets.front();
    for (const std::string& token : used) {
        usedFull.insert("--" + token);
        if (base.values.count("--" + token) == 0) {
            lint.error(name, "token.undeclared",
                       "var(--" + token + ") is not declared in " +
                           (group.tokensFile.empty() ? std::string("a token file") : group.tokensFile));
        }
    }
    std::set<std::string> listed;
    for (const TokenDefault& token : entry.tokens) {
        listed.insert(token.name);
    }
    if (listed != usedFull) {
        std::set<std::string> missing;
        std::set<std::string> extra;
        std::set_difference(usedFull.begin(), usedFull.end(), listed.begin(), listed.end(),
                            std::inserter(missing, missing.end()));
        std::set_difference(listed.begin(), listed.end(), usedFull.begin(), usedFull.end(),
                            std::inserter(extra, extra.end()));
        lint.error(name, "index.tokens", "missing " + describeSet(missing) + "; extra " + describeSet(extra));
    }
    for (const TokenDefault& token : entry.tokens) {
        const auto it = base.values.find(token.name);
        if (it != base.values.end() && it->second != token.value) {
            lint.error(name, "token.default",
                       "the default of " + token.name + " is '" + token.value +
                           "', the token file declares '" + it->second + "'");
        }
    }

    // Fonts and textures.
    std::vector<std::string> fonts;
    std::vector<std::string> textures;
    dependencies(used, sheets, fonts, textures);
    if (toSet(fonts) != toSet(entry.fonts)) {
        lint.error(
            name, "index.fonts",
            "lists " + describeSet(toSet(entry.fonts)) + ", the tokens name " + describeSet(toSet(fonts)));
    }
    if (toSet(textures) != toSet(entry.textures)) {
        lint.error(name, "index.textures",
                   "lists " + describeSet(toSet(entry.textures)) + ", the tokens name " +
                       describeSet(toSet(textures)));
    }
    std::set<std::string> assets = toSet(fonts);
    assets.insert(entry.fonts.begin(), entry.fonts.end());
    assets.insert(textures.begin(), textures.end());
    assets.insert(entry.textures.begin(), entry.textures.end());
    for (const std::string& asset : assets) {
        if (!lint.fileExists(asset)) {
            lint.error(name, "asset.missing", "missing " + asset);
        }
    }

    // Thumbnail and content hash.
    const std::string expectedThumbnail = "thumbnails/" + name + ".png";
    if (entry.thumbnail != expectedThumbnail) {
        lint.error(name, "index.thumbnail",
                   "thumbnail is '" + entry.thumbnail + "', not " + expectedThumbnail);
    }
    if (!lint.fileExists(entry.thumbnail.empty() ? expectedThumbnail : entry.thumbnail)) {
        lint.error(name, "thumbnail.missing",
                   "missing " + (entry.thumbnail.empty() ? expectedThumbnail : entry.thumbnail));
    }
    const std::string content = "sha256:" + util::sha256Hex(lfText(text));
    if (entry.content != content) {
        lint.error(
            name, "index.content",
            entry.content.empty() ? "the index has no content hash" : "the file's content hash differs");
    }
    if (!rawEntry.is_object() || !rawEntry.contains("previousContent") ||
        !rawEntry["previousContent"].is_array()) {
        lint.warn(name, "index.previousContent", "the index entry has no previousContent list");
    }

    if (lint.options.indexOnly) {
        return;
    }
    checkStyles(lint, entry, text);
    checkPreview(lint, group, entry);
}

void lintGroupImpl(Lint& lint) {
    SymbolGroup group;
    const Status loaded = loadGroup(lint.dir, group);
    if (loaded == Status::kNotFound) {
        lint.error("", "group.notFound", "not a directory");
        return;
    }
    if (loaded != Status::kOk) {
        lint.error("", "group.id", "the directory name is not a group id ([a-z0-9-])");
        return;
    }
    lint.report.groupId = group.id;
    lint.report.symbols = group.symbols.size();
    for (const compile::Diagnostic& d : group.problems) {
        lint.add(d.level == compile::Diagnostic::Level::Error ? LintLevel::Error : LintLevel::Warning, "",
                 "group.problem", d.file + ": " + d.message);
    }

    // The token file and the theme files.
    std::vector<TokenSheet> sheets;
    if (!group.tokensFile.empty()) {
        fs::path path;
        TokenSheet sheet;
        if (resolveInside(lint.dir, group.tokensFile, path)) {
            sheet = readTokenSheet(path);
        }
        if (!sheet.ok) {
            lint.error("", "group.tokens", "cannot read " + group.tokensFile);
        }
        sheets.push_back(std::move(sheet));
    } else {
        lint.error("", "group.tokens", "the group names no token file");
        sheets.emplace_back();
    }
    for (const std::string& theme : group.themeFiles) {
        fs::path path;
        TokenSheet sheet;
        if (resolveInside(lint.dir, theme, path)) {
            sheet = readTokenSheet(path);
        }
        if (!sheet.ok) {
            lint.error("", "group.theme", "missing theme file " + theme);
            continue;
        }
        sheets.push_back(std::move(sheet));
    }
    if (group.thumbnail.empty()) {
        lint.warn("", "group.thumbnail", "group.json names no group thumbnail");
    } else if (!lint.fileExists(group.thumbnail)) {
        lint.error("", "thumbnail.missing", "missing " + group.thumbnail);
    }

    // The raw index entries (previousContent is not part of SymbolEntry).
    std::map<std::string, json> rawEntries;
    {
        std::string text;
        std::string depthError;
        if (readFile(genericPathToUtf8(lint.dir / "index.json"), text)) {
            const json root = parseBounded(text, kMaxJsonDepth, depthError);
            if (root.is_object() && root.contains("symbols") && root["symbols"].is_array()) {
                for (const json& item : root["symbols"]) {
                    const std::string name = getString(item, "name");
                    if (!name.empty()) {
                        rawEntries.emplace(name, item);
                    }
                }
            }
        }
    }

    std::set<std::string> listedFiles;
    for (const SymbolEntry& entry : group.symbols) {
        listedFiles.insert(entry.file);
        const auto raw = rawEntries.find(entry.name);
        lintSymbol(lint, group, entry, sheets, raw == rawEntries.end() ? json() : raw->second);
    }

    // Files of the group root, mobile/ and list/ that the index does not list.
    for (const char* sub : {"", "mobile", "list"}) {
        const fs::path folder = *sub == '\0' ? lint.dir : lint.dir / sub;
        std::error_code ec;
        std::vector<std::string> names;
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec) && it->path().extension() == ".xt") {
                names.push_back(pathToUtf8(it->path().filename()));
            }
        }
        std::sort(names.begin(), names.end());
        for (const std::string& file : names) {
            const std::string relative = *sub == '\0' ? file : std::string(sub) + "/" + file;
            if (listedFiles.count(relative) == 0) {
                lint.error(pathToUtf8(pathFromUtf8(file).stem()), "index.unlisted",
                           relative + " is not listed in index.json");
            }
        }
    }
}

}  // namespace detail::lint

size_t LintReport::errors() const {
    return static_cast<size_t>(std::count_if(issues.begin(), issues.end(), [](const LintIssue& issue) {
        return issue.level == LintLevel::Error;
    }));
}

size_t LintReport::warnings() const { return issues.size() - errors(); }

LintReport lintGroup(const fs::path& dir, const LintOptions& options) {
    LintReport report;
    report.dir = genericPathToUtf8(dir);
    while (report.dir.size() > 1 && report.dir.back() == '/') {
        report.dir.pop_back();
    }
    detail::lint::Lint lint{report, options, dir};
    const Status status = detail::catchAll([&]() {
        detail::lint::lintGroupImpl(lint);
        return Status::kOk;
    });
    if (status != Status::kOk) {
        lint.error("", "lint.internal", "lint stopped on an unexpected failure");
    }
    return report;
}

std::vector<LintReport> lintLibrary(const fs::path& dir, const LintOptions& options) {
    std::vector<LintReport> reports;
    const Status status = detail::catchAll([&]() {
        std::error_code ec;
        if (!fs::is_regular_file(dir / "library.json", ec)) {
            reports.push_back(lintGroup(dir, options));
            return Status::kOk;
        }
        SymbolLibrary library;
        if (const Status loaded = loadLibrary(dir, library); loaded != Status::kOk) {
            LintReport report;
            report.dir = genericPathToUtf8(dir);
            report.issues.push_back(LintIssue{LintLevel::Error, "", "library.invalid",
                                              "library.json cannot be read as a library"});
            reports.push_back(std::move(report));
            return Status::kOk;
        }
        for (const SymbolGroup& group : library.groups) {
            if (group.dir.empty()) {
                LintReport report;
                report.groupId = group.id;
                report.issues.push_back(LintIssue{LintLevel::Error, "", "group.notFound",
                                                  "listed in library.json but has no directory"});
                reports.push_back(std::move(report));
                continue;
            }
            reports.push_back(lintGroup(pathFromUtf8(group.dir), options));
        }
        return Status::kOk;
    });
    if (status != Status::kOk) {
        LintReport report;
        report.dir = genericPathToUtf8(dir);
        report.issues.push_back(
            LintIssue{LintLevel::Error, "", "lint.internal", "lint stopped on an unexpected failure"});
        reports.push_back(std::move(report));
    }
    return reports;
}

std::vector<std::string> findLiteralColors(std::string_view css) {
    using namespace detail::lint;
    const std::string clean = stripComments(css);
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < clean.size()) {
        const size_t colon = clean.find(':', pos);
        if (colon == std::string::npos) {
            break;
        }
        size_t nameEnd = colon;
        while (nameEnd > pos && isSpace(clean[nameEnd - 1])) {
            --nameEnd;
        }
        size_t nameStart = nameEnd;
        while (nameStart > pos && (isAlpha(clean[nameStart - 1]) || clean[nameStart - 1] == '-')) {
            --nameStart;
        }
        size_t valueStart = colon + 1;
        while (valueStart < clean.size() && isSpace(clean[valueStart])) {
            ++valueStart;
        }
        size_t valueEnd = valueStart;
        while (valueEnd < clean.size() && clean[valueEnd] != ';' && clean[valueEnd] != '{' &&
               clean[valueEnd] != '}') {
            ++valueEnd;
        }
        if (nameStart == nameEnd || valueEnd == valueStart) {
            pos = colon + 1;
            continue;
        }
        const std::string value =
            detail::trimmed(std::string_view(clean).substr(valueStart, valueEnd - valueStart));
        if (hasHexColor(value) || hasColorFunction(value) || hasNamedColor(value)) {
            out.push_back(clean.substr(nameStart, nameEnd - nameStart) + ": " + value);
        }
        pos = valueEnd;
    }
    return out;
}

std::vector<std::string> styleRegions(std::string_view text, bool isXts) {
    if (isXts) {
        return {std::string(text)};
    }
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t block = text.find("<style>", pos);
        const size_t attr = text.find("style=\"", pos);
        if (block == std::string_view::npos && attr == std::string_view::npos) {
            break;
        }
        if (block != std::string_view::npos && (attr == std::string_view::npos || block <= attr)) {
            const size_t close = text.find("</style>", block + 7);
            if (close == std::string_view::npos) {
                pos = block + 1;
                continue;
            }
            out.emplace_back(text.substr(block + 7, close - block - 7));
            pos = close + 8;
        } else {
            const size_t close = text.find('"', attr + 7);
            if (close == std::string_view::npos) {
                break;
            }
            out.emplace_back(text.substr(attr + 7, close - attr - 7));
            pos = close + 1;
        }
    }
    return out;
}

std::vector<TimelineScan> scanTimelines(std::string_view text) {
    using namespace detail::lint;
    std::vector<TimelineScan> out;
    constexpr std::string_view kHead = "@timeline";
    size_t pos = 0;
    while ((pos = text.find(kHead, pos)) != std::string_view::npos) {
        size_t i = pos + kHead.size();
        const size_t spaces = i;
        while (i < text.size() && isSpace(text[i])) {
            ++i;
        }
        const size_t nameStart = i;
        while (i < text.size() && (isWord(text[i]) || text[i] == '-')) {
            ++i;
        }
        const size_t nameEnd = i;
        while (i < text.size() && isSpace(text[i])) {
            ++i;
        }
        if (spaces == nameStart || nameStart == nameEnd || i >= text.size() || text[i] != '{') {
            pos += kHead.size();
            continue;
        }
        TimelineScan info;
        info.name = std::string(text.substr(nameStart, nameEnd - nameStart));
        const size_t bodyStart = i + 1;
        size_t end = bodyStart;
        int depth = 1;
        while (end < text.size() && depth > 0) {
            depth += text[end] == '{' ? 1 : text[end] == '}' ? -1 : 0;
            ++end;
        }
        const std::string_view body = text.substr(bodyStart, end - bodyStart);
        // Key times: `<n>ms` / `<n>s` after the start of the body, whitespace, `{`, `}` or `;`.
        for (size_t k = 0; k < body.size(); ++k) {
            if (!isDigit(body[k]) || !(k == 0 || isSpace(body[k - 1]) || body[k - 1] == '{' ||
                                       body[k - 1] == '}' || body[k - 1] == ';')) {
                continue;
            }
            size_t numEnd = k;
            while (numEnd < body.size() && isDigit(body[numEnd])) {
                ++numEnd;
            }
            if (numEnd + 1 < body.size() && body[numEnd] == '.' && isDigit(body[numEnd + 1])) {
                ++numEnd;
                while (numEnd < body.size() && isDigit(body[numEnd])) {
                    ++numEnd;
                }
            }
            const bool ms = body.compare(numEnd, 2, "ms") == 0;
            const size_t unitEnd = numEnd + (ms ? 2 : 1);
            const bool unit = ms || (numEnd < body.size() && body[numEnd] == 's');
            if (unit && (unitEnd >= body.size() || !isWord(body[unitEnd]))) {
                float value = 0;
                const std::string number(body.substr(k, numEnd - k));
                if (fromChars(number.data(), number.data() + number.size(), value).ec == std::errc()) {
                    info.lastTime = std::max(info.lastTime, value * (ms ? 0.001f : 1.0f));
                }
                k = unitEnd - 1;
            } else {
                k = numEnd - 1;
            }
        }
        // Properties: `[a-z-]+ :` words other than `duration` and `marker`.
        std::set<std::string> props;
        for (size_t colon = body.find(':'); colon != std::string_view::npos;
             colon = body.find(':', colon + 1)) {
            size_t nameStop = colon;
            while (nameStop > 0 && isSpace(body[nameStop - 1])) {
                --nameStop;
            }
            size_t from = nameStop;
            while (from > 0 && ((body[from - 1] >= 'a' && body[from - 1] <= 'z') || body[from - 1] == '-')) {
                --from;
            }
            const std::string prop(body.substr(from, nameStop - from));
            if (!prop.empty() && prop != "duration" && prop != "marker") {
                props.insert(prop);
            }
        }
        info.props.assign(props.begin(), props.end());
        out.push_back(std::move(info));
        pos = end;
    }
    return out;
}

bool isConventionalTimelineName(std::string_view name) {
    if (name.empty() || name.front() < 'a' || name.front() > 'z' || name.back() == '-') {
        return false;
    }
    char previous = '\0';
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok || (c == '-' && previous == '-')) {
            return false;
        }
        previous = c;
    }
    return true;
}

// --- previews -----------------------------------------------------------------------------------

namespace {

Status previewError(std::string& error, std::string message) {
    error = std::move(message);
    return Status::kInvalidArgument;
}

bool smallInt(const nlohmann::ordered_json& value, int minimum, int maximum, int& out) {
    if (!value.is_number_integer()) {
        return false;
    }
    const int64_t number = value.is_number_unsigned()
                               ? static_cast<int64_t>(std::min<uint64_t>(value.get<uint64_t>(), INT64_MAX))
                               : value.get<int64_t>();
    if (number < minimum || number > maximum) {
        return false;
    }
    out = static_cast<int>(number);
    return true;
}

Status readPreviewSpecImpl(const fs::path& jsonFile, PreviewSpec& out, std::string& error) {
    using detail::lint::kMaxPreviewEvents;
    using detail::lint::kMaxPreviewFrames;
    using detail::lint::kMaxPreviewLine;
    using detail::lint::kMaxPreviewPorts;
    using detail::lint::kMaxPreviewSize;
    const std::string fileName = pathToUtf8(jsonFile.filename());
    constexpr std::string_view kSuffix = ".preview.json";
    if (fileName.size() <= kSuffix.size() ||
        fileName.compare(fileName.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
        return previewError(error, "the file name is not <Name>.preview.json");
    }
    out.name = fileName.substr(0, fileName.size() - kSuffix.size());
    if (out.name.find('.') != std::string::npos || out.name.find(':') != std::string::npos) {
        return previewError(error, "'" + out.name + "' is not a symbol name");
    }
    out.xtFile = "previews/" + out.name + ".preview.xt";
    out.jsonFile = "previews/" + fileName;
    std::string text;
    if (!detail::readFile(genericPathToUtf8(jsonFile), text)) {
        return Status::kNotFound;
    }
    std::string depthError;
    const detail::json checked = detail::parseBounded(text, detail::kMaxJsonDepth, depthError);
    if (!depthError.empty()) {
        return previewError(error, depthError);
    }
    if (checked.is_discarded() || !checked.is_object()) {
        return previewError(error, "not a JSON object");
    }
    const nlohmann::ordered_json root =
        nlohmann::ordered_json::parse(text, nullptr, false);  // Keeps the key order.
    if (!root.is_object()) {
        return previewError(error, "not a JSON object");
    }
    if (!root.contains("width") || !smallInt(root["width"], 1, kMaxPreviewSize, out.width)) {
        return previewError(error, "'width' must be an integer 1.." + std::to_string(kMaxPreviewSize));
    }
    if (!root.contains("height") || !smallInt(root["height"], 1, kMaxPreviewSize, out.height)) {
        return previewError(error, "'height' must be an integer 1.." + std::to_string(kMaxPreviewSize));
    }
    if (root.contains("frames") && !smallInt(root["frames"], 0, kMaxPreviewFrames, out.frames)) {
        return previewError(error, "'frames' must be an integer 0.." + std::to_string(kMaxPreviewFrames));
    }
    if (root.contains("ports")) {
        const nlohmann::ordered_json& ports = root["ports"];
        if (!ports.is_object() || ports.size() > kMaxPreviewPorts) {
            return previewError(error, "'ports' must be an object of at most " +
                                           std::to_string(kMaxPreviewPorts) + " entries");
        }
        for (const auto& [name, value] : ports.items()) {
            if (name.empty() || name.size() > 128 || name.find_first_of(" \t\r\n") != std::string::npos) {
                return previewError(error, "'ports' has a bad name '" + name + "'");
            }
            std::string valueText;
            if (value.is_string()) {
                valueText = value.get<std::string>();
            } else if (value.is_boolean() || value.is_number() || value.is_array() || value.is_object()) {
                valueText = value.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace);
            } else {
                return previewError(
                    error, "'ports." + name + "' must be a string, number, boolean, array or object");
            }
            if (valueText.size() > kMaxPreviewLine || valueText.find_first_of("\r\n") != std::string::npos) {
                return previewError(error, "'ports." + name + "' is too long or has a line break");
            }
            out.ports.emplace_back(name, std::move(valueText));
        }
    }
    if (root.contains("events")) {
        const nlohmann::ordered_json& events = root["events"];
        if (!events.is_array() || events.size() > kMaxPreviewEvents) {
            return previewError(error, "'events' must be an array of at most " +
                                           std::to_string(kMaxPreviewEvents) + " strings");
        }
        for (const nlohmann::ordered_json& line : events) {
            if (!line.is_string() || line.get<std::string>().size() > kMaxPreviewLine ||
                line.get<std::string>().find_first_of("\r\n") != std::string::npos) {
                return previewError(error, "'events' entries must be single-line strings");
            }
            out.events.push_back(line.get<std::string>());
        }
    }
    return Status::kOk;
}

}  // namespace

Status readPreviewSpec(const fs::path& jsonFile, PreviewSpec& out, std::string& error) {
    out = PreviewSpec();
    error.clear();
    const Status status = detail::catchAll([&]() { return readPreviewSpecImpl(jsonFile, out, error); });
    if (status == Status::kInternal) {
        out = PreviewSpec();
        error = "unexpected failure while reading the file";
    }
    if (status == Status::kInvalidArgument) {
        const PreviewSpec cleared;
        const std::string name = out.name;
        out = cleared;
        out.name = name;
    }
    return status;
}

std::vector<PreviewSpec> thumbnailSpecs(const fs::path& groupDir, const std::vector<std::string>& names,
                                        std::vector<compile::Diagnostic>* problems) {
    std::vector<PreviewSpec> specs;
    detail::catchAll([&]() {
        const fs::path previews = groupDir / "previews";
        std::error_code ec;
        std::vector<fs::path> files;
        for (fs::directory_iterator it(previews, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = pathToUtf8(it->path().filename());
            if (it->is_regular_file(ec) && name.size() > 13 &&
                name.compare(name.size() - 13, 13, ".preview.json") == 0) {
                files.push_back(it->path());
            }
        }
        std::sort(files.begin(), files.end());
        auto note = [&](const fs::path& file, std::string message) {
            if (problems != nullptr) {
                compile::Diagnostic d;
                d.level = compile::Diagnostic::Level::Error;
                d.file = pathToUtf8(file.filename());
                d.message = std::move(message);
                problems->push_back(std::move(d));
            }
        };
        for (const fs::path& file : files) {
            PreviewSpec spec;
            std::string error;
            if (readPreviewSpec(file, spec, error) != Status::kOk) {
                if (names.empty() || std::find(names.begin(), names.end(), spec.name) != names.end()) {
                    note(file, error);
                }
                continue;
            }
            if (!names.empty() && std::find(names.begin(), names.end(), spec.name) == names.end()) {
                continue;
            }
            if (!fs::is_regular_file(groupDir / pathFromUtf8(spec.xtFile), ec)) {
                note(file, "no " + spec.xtFile);
                continue;
            }
            specs.push_back(std::move(spec));
        }
        return Status::kOk;
    });
    return specs;
}

std::string previewEventScript(const PreviewSpec& spec) {
    std::string text;
    for (const auto& [name, value] : spec.ports) {
        text += "port " + name + " " + value + "\n";
    }
    for (const std::string& line : spec.events) {
        text += line + "\n";
    }
    return text;
}

}  // namespace xt::bundle
