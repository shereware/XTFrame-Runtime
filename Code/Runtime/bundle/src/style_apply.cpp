#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "bundle_json.h"
#include "style_internal.h"
#include "symbol_internal.h"
#include "xt/bundle/project_style.h"
#include "xt/compile/utf8_path.h"
#include "xt/util/sha256.h"

/// T-743 (Docs/design/56-symbol-identity-style-scope.md 56.2 items 1, 6; 47 rule 4): "apply style" --
/// the core tokens of a group replace the project's values; what is replaced, added and left
/// uncovered is reported, and the project's `style` record says where the style came from.

namespace xt::bundle {

namespace fs = std::filesystem;
using compile::genericPathToUtf8;
using compile::pathFromUtf8;
using compile::pathToUtf8;
using detail::json;
using detail::resolveInside;
using detail::trimmed;
namespace st = detail::style;

namespace {

/// The project files a style call writes, by absolute path, in first-touch order.
struct EditsApply {
    fs::path root;
    std::vector<fs::path> order;
    std::map<fs::path, FileChange> files;

    /// The current (edited) text of `file`.
    std::string& text(const fs::path& file) {
        auto it = files.find(file);
        if (it == files.end()) {
            FileChange change;
            change.path = detail::relativeText(root, file);
            change.before = st::readText(file, &change.existed);
            change.after = change.before;
            it = files.emplace(file, std::move(change)).first;
            order.push_back(file);
        }
        return it->second.after;
    }
    bool existed(const fs::path& file) {
        text(file);
        return files[file].existed;
    }
    std::vector<FileChange> changes() const {
        std::vector<FileChange> out;
        for (const fs::path& file : order) {
            const FileChange& change = files.at(file);
            if (change.existed ? change.before != change.after : !change.after.empty()) {
                out.push_back(change);
            }
        }
        return out;
    }
};

/// The families a font family list names (quoted or bare), in order.
std::vector<std::string> familiesOfApply(std::string_view value) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= value.size()) {
        size_t comma = value.find(',', start);
        if (comma == std::string_view::npos) {
            comma = value.size();
        }
        std::string item = trimmed(value.substr(start, comma - start));
        if (item.size() >= 2 && (item.front() == '"' || item.front() == '\'') &&
            item.back() == item.front()) {
            item = item.substr(1, item.size() - 2);
        }
        if (!item.empty()) {
            out.push_back(item);
        }
        start = comma + 1;
    }
    return out;
}

std::string fontFaceLineApply(const compile::FontFaceDef& face, const std::string& src) {
    std::string text = "@font-face { font-family: \"" + face.family + "\"; src: \"" + src +
                       "\"; font-weight: " + std::to_string(face.weight) + ";";
    if (face.italic) {
        text += " font-style: italic;";
    }
    return text + " }";
}

std::string faceKeyApply(const compile::FontFaceDef& face) {
    return face.family + "|" + std::to_string(face.weight) + (face.italic ? "|i" : "");
}

/// The token values of a group sheet file (a later declaration wins), with its `@font-face` blocks.
struct GroupSheetApply {
    bool ok = false;
    std::map<std::string, std::string> values;
    std::vector<compile::FontFaceDef> faces;
};

GroupSheetApply readGroupSheetApply(const fs::path& file) {
    GroupSheetApply sheet;
    std::string text;
    const std::string utf8 = genericPathToUtf8(file);
    if (!detail::readFile(utf8, text)) {
        return sheet;
    }
    compile::RuleSet rules;
    compile::DiagnosticSink sink;
    compile::parseXts(text, utf8, 1, sink, rules);
    sheet.ok = true;
    for (const compile::TokenDecl& token : rules.tokens) {
        sheet.values[token.name] = trimmed(token.value);
    }
    sheet.faces = rules.fontFaces;
    return sheet;
}

struct ApplyContextApply {
    const UiProject& project;
    const SymbolLibrary& library;
    const SymbolGroup& group;
    fs::path groupDir;
    StyleReport& report;
    EditsApply edits;
    std::map<std::string, std::string> implied;
    bool recorded = false;
    ProjectStyle record;
    /// The faces already in the project's tokens file and added this call.
    std::set<std::string> faces;
};

/// 49 §49.3 item 2 for one font of the group: the project-relative target (an existing file of the
/// same content reused, other content -> `<stem>-<8 hex><ext>`); empty when it cannot be copied.
fs::path copyFontApply(ApplyContextApply& ctx, const std::string& groupRelative) {
    fs::path source;
    std::string bytes;
    if (!resolveInside(ctx.groupDir, groupRelative, source) ||
        !detail::readFile(genericPathToUtf8(source), bytes)) {
        return fs::path();
    }
    const std::string assetRoot =
        ctx.project.shared.assetRoot.empty() ? std::string("shared") : ctx.project.shared.assetRoot;
    fs::path target;
    if (!resolveInside(ctx.edits.root, assetRoot + "/fonts/" + detail::fileNameOf(groupRelative), target)) {
        return fs::path();
    }
    const std::string hash = util::sha256Hex(bytes);
    auto sameContent = [&](const fs::path& file) {
        bool existed = false;
        const std::string existing = st::readText(file, &existed);
        return existed ? std::optional<bool>(util::sha256Hex(existing) == hash) : std::optional<bool>();
    };
    std::optional<bool> same = sameContent(target);
    if (same.has_value() && !*same) {
        target = target.parent_path() / pathFromUtf8(pathToUtf8(target.stem()) + "-" + hash.substr(0, 8) +
                                                     pathToUtf8(target.extension()));
        same = sameContent(target);
    }
    if (!same.has_value()) {
        if (ctx.edits.files.count(target) == 0) {
            ctx.edits.text(target) = bytes;
            ctx.report.copied.push_back(groupRelative + " -> " +
                                        detail::relativeText(ctx.edits.root, target));
        }
    }
    return target;
}

/// Writes the core tokens of `values` (with the faces of `sheetFaces` their font tokens need) into
/// `file`: each declared one replaced, the missing ones appended in one section.
void writeCoreApply(ApplyContextApply& ctx, const fs::path& file, const std::string& theme,
                    const std::map<std::string, std::string>& values,
                    const std::vector<compile::FontFaceDef>& sheetFaces,
                    const std::map<std::string, std::string>* recordHashes,
                    std::map<std::string, std::string>& hashesOut) {
    std::string& text = ctx.edits.text(file);
    const bool existed = ctx.edits.existed(file);
    const std::map<std::string, std::string> current = st::tokenValues({file});
    std::vector<std::pair<std::string, std::string>> appended;
    std::vector<std::string> families;
    for (const CoreToken& core : coreTokens()) {
        const auto it = values.find(core.name);
        if (it == values.end()) {
            if (theme.empty() && std::find(ctx.report.missingCore.begin(), ctx.report.missingCore.end(),
                                           core.name) == ctx.report.missingCore.end()) {
                ctx.report.missingCore.push_back(core.name);
            }
            continue;
        }
        const std::string& after = it->second;
        hashesOut[core.name] = styleValueHash(after);
        if (core.type == CoreTokenType::Family) {
            for (const std::string& family : familiesOfApply(after)) {
                if (std::find(families.begin(), families.end(), family) == families.end()) {
                    families.push_back(family);
                }
            }
        }
        const auto have = current.find(core.name);
        StyleTokenChange change{theme, core.name, std::string(), after, false};
        if (have != current.end()) {
            if (have->second == after) {
                continue;
            }
            change.before = have->second;
            if (recordHashes != nullptr) {
                const auto rec = recordHashes->find(core.name);
                change.custom = rec == recordHashes->end() || rec->second != styleValueHash(have->second);
            } else if (theme.empty()) {
                const auto imp = ctx.implied.find(core.name);
                change.custom = imp == ctx.implied.end() || imp->second != have->second;
            } else {
                change.custom = true;
            }
            text = st::replaceTokenValue(text, core.name, after);
            ctx.report.overwritten.push_back(std::move(change));
        } else {
            if (theme.empty()) {
                const auto imp = ctx.implied.find(core.name);
                change.before = imp == ctx.implied.end() ? std::string() : imp->second;
            }
            appended.emplace_back(core.name, after);
            ctx.report.added.push_back(std::move(change));
        }
    }
    // The @font-face blocks of the families the core font tokens name, their files copied.
    std::vector<std::string> faceLines;
    for (const compile::FontFaceDef& face : sheetFaces) {
        if (std::find(families.begin(), families.end(), face.family) == families.end() ||
            ctx.faces.count(faceKeyApply(face)) != 0) {
            continue;
        }
        const fs::path target = copyFontApply(ctx, face.src);
        if (target.empty()) {
            continue;
        }
        faceLines.push_back(fontFaceLineApply(
            face, genericPathToUtf8(
                      target.lexically_normal().lexically_relative(file.parent_path().lexically_normal()))));
        ctx.faces.insert(faceKeyApply(face));
    }
    const std::string title = "style " + (ctx.group.name.empty() ? ctx.group.id : ctx.group.name) + " (" +
                              ctx.library.id + "/" + ctx.group.id + ")";
    const std::string created = theme.empty() ? "/* Design tokens of this UI Project (shared.tokens). */\n"
                                              : "/* Theme \"" + theme + "\": token overrides only. */\n";
    text = st::appendTokenSection(text, existed, created, title, faceLines, appended);
}

/// 56 §56.2 item 6 / 47 rule 4: the extension tokens project symbols read that the style lacks.
void collectUncoveredApply(ApplyContextApply& ctx, const std::map<std::string, std::string>& styleValues,
                           const fs::path& tokensFile) {
    const std::map<std::string, std::string> origins = st::sectionOrigins(st::readText(tokensFile));
    std::map<std::string, size_t> index;
    for (const st::ProjectSymbolFile& symbol : st::projectSymbolFiles(ctx.project)) {
        for (const std::string& name : st::varRefs(st::readText(symbol.path))) {
            if (findCoreToken(name) != nullptr || styleValues.count(name) != 0) {
                continue;
            }
            auto at = index.find(name);
            if (at == index.end()) {
                UncoveredToken token;
                token.name = name;
                const auto origin = origins.find(name);
                token.origin = origin != origins.end()        ? origin->second
                               : ctx.implied.count(name) != 0 ? std::string("implied")
                                                              : std::string();
                at = index.emplace(name, ctx.report.uncovered.size()).first;
                ctx.report.uncovered.push_back(std::move(token));
            }
            ctx.report.uncovered[at->second].usedBy.push_back(symbol.qualified);
        }
    }
    std::sort(ctx.report.uncovered.begin(), ctx.report.uncovered.end(),
              [](const UncoveredToken& a, const UncoveredToken& b) { return a.name < b.name; });
}

/// The `style` record in `<shared.symbols>/index.json`, every other key kept.
void writeRecordApply(ApplyContextApply& ctx, const std::map<std::string, std::string>& base,
                      const std::map<std::string, std::map<std::string, std::string>>& themes) {
    fs::path indexPath;
    if (!st::projectIndexPath(ctx.project, indexPath)) {
        return;
    }
    std::string& text = ctx.edits.text(indexPath);
    nlohmann::ordered_json root;
    std::string error;
    const json parsed = detail::parseBounded(text, detail::kMaxJsonDepth, error);
    if (!text.empty() && error.empty() && parsed.is_object()) {
        root = nlohmann::ordered_json::parse(text, nullptr, false);
    }
    if (!root.is_object()) {
        root = nlohmann::ordered_json::object();
        root["version"] = 1;
        root["symbols"] = nlohmann::ordered_json::array();
    }
    nlohmann::ordered_json style;
    style["library"] = ctx.library.id;
    style["group"] = ctx.group.id;
    style["name"] = ctx.group.name;
    style["set"] = ctx.group.source.set;
    style["setVersion"] = ctx.group.source.setVersion;
    style["theme"] = ctx.report.theme;
    style["tokens"] = nlohmann::ordered_json::object();
    for (const auto& [name, hash] : base) {
        style["tokens"][name] = hash;
    }
    nlohmann::ordered_json themeObj = nlohmann::ordered_json::object();
    for (const auto& [theme, hashes] : themes) {
        nlohmann::ordered_json t = nlohmann::ordered_json::object();
        for (const auto& [name, hash] : hashes) {
            t[name] = hash;
        }
        themeObj[theme] = std::move(t);
    }
    style["themes"] = std::move(themeObj);
    root["style"] = std::move(style);
    text = root.dump(2, ' ', false, nlohmann::ordered_json::error_handler_t::replace) + "\n";
}

void applyImplApply(const LibraryMounts& mounts, const UiProject& project, std::string_view libraryId,
                    std::string_view groupId, std::string_view theme,
                    std::span<const std::string> impliedFiles, StyleReport& report) {
    auto fail = [&](Status status, std::string message) {
        report.status = status;
        report.error = std::move(message);
    };
    if (project.rootDir.empty() || project.isUnnamed()) {
        return fail(Status::kInvalidArgument, "no UI Project is open");
    }
    if (project.shared.tokens.empty()) {
        return fail(Status::kInvalidArgument, "the project names no shared.tokens file");
    }
    if (!detail::isGroupId(groupId)) {
        return fail(Status::kInvalidArgument, "not a group id: " + std::string(groupId));
    }
    const SymbolLibrary* library = nullptr;
    for (const SymbolLibrary& l : mounts.libraries) {
        if (l.id == libraryId) {
            library = &l;
            break;
        }
    }
    const SymbolGroup* group = library != nullptr ? detail::groupOf(*library, groupId) : nullptr;
    if (group == nullptr || group->dir.empty()) {
        return fail(Status::kNotFound, "no mounted library '" + std::string(libraryId) +
                                           "' with the group '" + std::string(groupId) + "'");
    }
    ApplyContextApply ctx{project, *library, *group, pathFromUtf8(group->dir), report, {}, {}, false, {}, {}};
    ctx.edits.root = st::projectRoot(project);
    report.library = library->id;
    report.group = group->id;
    report.groupName = group->name;
    report.setVersion = group->source.setVersion;
    report.theme = std::string(theme);
    fs::path groupTokens;
    if (group->tokensFile.empty() || !resolveInside(ctx.groupDir, group->tokensFile, groupTokens)) {
        return fail(Status::kNotFound, "the group '" + group->id + "' has no token file");
    }
    GroupSheetApply base = readGroupSheetApply(groupTokens);
    if (!base.ok) {
        return fail(Status::kNotFound, "cannot read the token file of the group '" + group->id + "'");
    }
    std::vector<std::pair<std::string, GroupSheetApply>> themeSheets;
    for (const std::string& themeFile : group->themeFiles) {
        fs::path path;
        if (!resolveInside(ctx.groupDir, themeFile, path)) {
            continue;
        }
        GroupSheetApply sheet = readGroupSheetApply(path);
        if (sheet.ok && !themeNameOf(themeFile).empty()) {
            themeSheets.emplace_back(themeNameOf(themeFile), std::move(sheet));
        }
    }
    if (!theme.empty()) {
        const auto found = std::find_if(themeSheets.begin(), themeSheets.end(),
                                        [&](const auto& t) { return t.first == theme; });
        if (found == themeSheets.end()) {
            return fail(Status::kNotFound,
                        "the group '" + group->id + "' has no theme '" + std::string(theme) + "'");
        }
        for (const auto& [name, value] : found->second.values) {
            base.values[name] = value;
        }
        themeSheets.clear();  // The theme is the base now; no theme file is written.
    }
    fs::path tokensFile;
    if (!resolveInside(ctx.edits.root, project.shared.tokens, tokensFile)) {
        return fail(Status::kInvalidArgument, "shared.tokens leaves the project");
    }
    std::vector<fs::path> implied;
    for (const std::string& f : impliedFiles) {
        implied.push_back(pathFromUtf8(f));
    }
    ctx.implied = st::tokenValues(implied);
    ctx.recorded = readProjectStyle(project, ctx.record);
    {
        std::string text;
        if (detail::readFile(genericPathToUtf8(tokensFile), text)) {
            compile::RuleSet rules;
            compile::DiagnosticSink sink;
            compile::parseXts(text, genericPathToUtf8(tokensFile), 1, sink, rules);
            for (const compile::FontFaceDef& face : rules.fontFaces) {
                ctx.faces.insert(faceKeyApply(face));
            }
        }
    }
    collectUncoveredApply(ctx, base.values, tokensFile);

    std::map<std::string, std::string> baseHashes;
    writeCoreApply(ctx, tokensFile, std::string(), base.values, base.faces,
                   ctx.recorded ? &ctx.record.tokens : nullptr, baseHashes);
    const std::string themesRel =
        project.shared.themes.empty() ? std::string("shared/themes") : project.shared.themes;
    std::map<std::string, std::map<std::string, std::string>> themeHashes;
    std::set<std::string> provided;
    for (const auto& [name, sheet] : themeSheets) {
        fs::path themeFile;
        if (!resolveInside(ctx.edits.root, themesRel + "/" + name + ".xts", themeFile)) {
            continue;
        }
        provided.insert(name);
        const std::map<std::string, std::string>* rec = nullptr;
        if (ctx.recorded) {
            const auto it = ctx.record.themes.find(name);
            rec = it != ctx.record.themes.end() ? &it->second : nullptr;
        }
        writeCoreApply(ctx, themeFile, name, sheet.values, {}, rec, themeHashes[name]);
    }
    // Project themes setting core tokens that the style has no theme for keep the old values.
    fs::path themesDir;
    if (theme.empty() && resolveInside(ctx.edits.root, themesRel, themesDir)) {
        std::error_code ec;
        std::vector<std::string> names;
        for (fs::directory_iterator it(themesDir, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec) && it->path().extension() == ".xts") {
                names.push_back(pathToUtf8(it->path().stem()));
            }
        }
        std::sort(names.begin(), names.end());
        for (const std::string& name : names) {
            if (provided.count(name) != 0) {
                continue;
            }
            for (const auto& [token, value] : st::tokenValues({themesDir / pathFromUtf8(name + ".xts")})) {
                if (findCoreToken(token) != nullptr) {
                    report.themesNotProvided.push_back(name);
                    break;
                }
            }
        }
    }
    writeRecordApply(ctx, baseHashes, themeHashes);
    report.changes = ctx.edits.changes();
}

}  // namespace

StyleReport applyGroupStyle(const LibraryMounts& mounts, const UiProject& project, std::string_view libraryId,
                            std::string_view groupId, std::string_view theme, bool dryRun,
                            std::span<const std::string> impliedTokenFiles) {
    StyleReport report;
    const Status status = detail::catchAll([&]() {
        applyImplApply(mounts, project, libraryId, groupId, theme, impliedTokenFiles, report);
        return Status::kOk;
    });
    if (status != Status::kOk) {
        report = StyleReport();
        report.status = Status::kInternal;
        report.error = "unexpected failure while applying the style";
        return report;
    }
    if (report.status == Status::kOk && !dryRun) {
        report.status = writeFileChanges(project, report.changes);
        if (report.status != Status::kOk) {
            report.error = "cannot write the project's token files";
        }
    }
    return report;
}

Status writeFileChanges(const UiProject& project, const std::vector<FileChange>& changes) {
    const fs::path root = st::projectRoot(project);
    if (root.empty()) {
        return Status::kInvalidArgument;
    }
    for (const FileChange& change : changes) {
        fs::path file;
        if (!resolveInside(root, change.path, file)) {
            return Status::kIoError;
        }
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        if (!detail::writeFile(genericPathToUtf8(file), change.after)) {
            return Status::kIoError;
        }
    }
    return Status::kOk;
}

}  // namespace xt::bundle
