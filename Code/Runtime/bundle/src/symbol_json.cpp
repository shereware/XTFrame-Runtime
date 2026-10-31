#include "xt/bundle/symbol_json.h"

#include <set>
#include <string>
#include <vector>

#include "xt/compile/diagnostics.h"

/// T-502: the JSON shapes of xt/bundle/symbol_json.h. Plain data conversion; the readers that fill
/// the structs (symbol_library.cpp, symbol_set.cpp, symbol_lint.cpp) did every check already.

namespace xt::bundle {

namespace {

SymbolJson stringList(const std::vector<std::string>& values) {
    SymbolJson out = SymbolJson::array();
    for (const std::string& value : values) {
        out.push_back(value);
    }
    return out;
}

SymbolJson requiresJson(const SymbolEntry& entry) {
    return SymbolJson{{"states", stringList(entry.requiredStates)},
                      {"slots", stringList(entry.requiredSlots)}};
}

SymbolJson lintSummaryJson(const LintSummary& lint) {
    return SymbolJson{{"errors", lint.errors}, {"warnings", lint.warnings}};
}

SymbolJson problemsJson(const std::vector<compile::Diagnostic>& problems) {
    SymbolJson out = SymbolJson::array();
    for (const compile::Diagnostic& d : problems) {
        out.push_back(
            SymbolJson{{"level", d.level == compile::Diagnostic::Level::Error ? "error" : "warning"},
                       {"message", d.message}});
    }
    return out;
}

SymbolJson libraryRow(const SymbolLibrary& library) {
    SymbolJson groups = SymbolJson::array();
    for (const SymbolGroup& group : library.groups) {
        groups.push_back(group.id);
    }
    return SymbolJson{{"id", library.id},
                      {"name", library.name},
                      {"path", library.dir},
                      {"readOnly", library.readOnly},
                      {"scope", libraryScopeName(library.scope)},
                      {"defaultGroup", library.defaultGroup},
                      {"groups", std::move(groups)}};
}

}  // namespace

const char* libraryScopeName(LibraryScope scope) {
    switch (scope) {
        case LibraryScope::Preset:
            return "preset";
        case LibraryScope::User:
            return "user";
        case LibraryScope::Project:
            return "project";
        case LibraryScope::Extra:
            break;
    }
    return "extra";
}

const char* importActionName(ImportAction action) {
    switch (action) {
        case ImportAction::Added:
            return "added";
        case ImportAction::Updated:
            return "updated";
        case ImportAction::Unchanged:
            return "unchanged";
        case ImportAction::OlderSkipped:
            return "older-skipped";
        case ImportAction::OlderReplaced:
            return "older-replaced";
    }
    return "added";
}

SymbolJson librariesJson(const LibraryMounts& mounts) {
    SymbolJson out = SymbolJson::array();
    std::set<std::string> mounted;
    for (const SymbolLibrary& library : mounts.libraries) {
        SymbolJson row = libraryRow(library);
        row["enabled"] = true;
        out.push_back(std::move(row));
        mounted.insert(library.dir);
    }
    for (const MountEntry& entry : mounts.entries) {
        if (mounted.count(entry.path) != 0) {
            continue;
        }
        out.push_back(SymbolJson{{"id", ""},
                                 {"name", ""},
                                 {"path", entry.path},
                                 {"readOnly", false},
                                 {"scope", libraryScopeName(LibraryScope::Extra)},
                                 {"defaultGroup", ""},
                                 {"groups", SymbolJson::array()},
                                 {"enabled", entry.enabled}});
    }
    return out;
}

SymbolJson symbolEntryJson(const SymbolEntry& entry) {
    SymbolJson ports = SymbolJson::array();
    for (const SymbolPortInfo& port : entry.ports) {
        ports.push_back(SymbolJson{{"name", port.name}, {"type", port.type}, {"dir", port.dir}});
    }
    SymbolJson tokens = SymbolJson::array();
    for (const TokenDefault& token : entry.tokens) {
        tokens.push_back(SymbolJson{{"name", token.name}, {"default", token.value}});
    }
    return SymbolJson{{"name", entry.name},
                      {"file", entry.file},
                      {"thumbnail", entry.thumbnail},
                      {"description", entry.description},
                      {"category", entry.category},
                      {"requires", requiresJson(entry)},
                      {"ports", std::move(ports)},
                      {"timelines", stringList(entry.timelines)},
                      {"content", entry.content},
                      {"tokens", std::move(tokens)},
                      {"fonts", stringList(entry.fonts)},
                      {"textures", stringList(entry.textures)}};
}

SymbolJson symbolSummaryJson(const SymbolEntry& entry) {
    SymbolJson ports = SymbolJson::array();
    for (const SymbolPortInfo& port : entry.ports) {
        ports.push_back(port.name);
    }
    return SymbolJson{{"name", entry.name},           {"file", entry.file},
                      {"category", entry.category},   {"description", entry.description},
                      {"thumbnail", entry.thumbnail}, {"requires", requiresJson(entry)},
                      {"ports", std::move(ports)},    {"timelines", stringList(entry.timelines)}};
}

SymbolJson groupJson(const SymbolGroup& group) {
    SymbolJson symbols = SymbolJson::array();
    for (const SymbolEntry& entry : group.symbols) {
        symbols.push_back(symbolSummaryJson(entry));
    }
    return SymbolJson{{"id", group.id},
                      {"name", group.name},
                      {"description", group.description},
                      {"dir", group.dir},
                      {"source", SymbolJson{{"kind", group.source.kind},
                                            {"set", group.source.set},
                                            {"setVersion", group.source.setVersion}}},
                      {"tokens", group.tokensFile},
                      {"themes", stringList(group.themeFiles)},
                      {"thumbnail", group.thumbnail},
                      {"lint", lintSummaryJson(group.lint)},
                      {"problems", problemsJson(group.problems)},
                      {"symbols", std::move(symbols)}};
}

SymbolJson libraryJson(const SymbolLibrary& library) {
    SymbolJson out = libraryRow(library);
    SymbolJson groups = SymbolJson::array();
    for (const SymbolGroup& group : library.groups) {
        groups.push_back(groupJson(group));
    }
    out["groups"] = std::move(groups);
    return out;
}

SymbolJson lintReportJson(const LintReport& report) {
    SymbolJson issues = SymbolJson::array();
    for (const LintIssue& issue : report.issues) {
        issues.push_back(SymbolJson{{"level", issue.level == LintLevel::Error ? "error" : "warning"},
                                    {"symbol", issue.symbol},
                                    {"code", issue.code},
                                    {"message", issue.message}});
    }
    return SymbolJson{{"id", report.groupId},          {"dir", report.dir},
                      {"symbols", report.symbols},     {"errors", report.errors()},
                      {"warnings", report.warnings()}, {"issues", std::move(issues)}};
}

SymbolJson useReportJson(const UseReport& report) {
    return SymbolJson{
        {"status", statusName(report.status)},           {"written", stringList(report.written)},
        {"skipped", stringList(report.skipped)},         {"renamed", stringList(report.renamed)},
        {"tokensAdded", stringList(report.tokensAdded)}, {"notFound", stringList(report.notFound)}};
}

SymbolJson setInfoJson(const SetInfo& set) {
    return SymbolJson{{"id", set.id},
                      {"author", set.author},
                      {"name", set.name},
                      {"version", set.setVersion},
                      {"license", set.license},
                      {"fontsLicense", set.fontsLicense},
                      {"description", set.description},
                      {"homepage", set.homepage},
                      {"thumbnail", set.thumbnail},
                      {"groups", stringList(set.groups)},
                      {"requiresXtframe", set.requiresXtframe}};
}

SymbolJson importReportJson(const ImportReport& report) {
    SymbolJson groups = SymbolJson::array();
    size_t errors = 0;
    size_t warnings = 0;
    for (const ImportedGroup& group : report.groups) {
        errors += group.lint.errors();
        warnings += group.lint.warnings();
        groups.push_back(SymbolJson{{"sourceId", group.sourceId},
                                    {"targetId", group.targetId},
                                    {"action", importActionName(group.action)},
                                    {"trashed", group.trashed},
                                    {"lint", lintReportJson(group.lint)}});
    }
    return SymbolJson{
        {"status", statusName(report.status)}, {"error", report.error}, {"set", setInfoJson(report.set)},
        {"groups", std::move(groups)},         {"lintErrors", errors},  {"lintWarnings", warnings}};
}

}  // namespace xt::bundle
