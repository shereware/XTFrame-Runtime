#include "xt/compile/diagnostic_report.h"

#include <fstream>
#include <json.hpp>

#include "xt/compile/utf8_path.h"
#include "xt/version.h"

/// T-576: the shared diagnostics file and line (Docs/design/24-design-import.md §24.15.2).

namespace xt::compile {

namespace {

using ReportJson = nlohmann::ordered_json;

ReportJson reportFieldsObject(const std::vector<DiagnosticField>& fields) {
    ReportJson object = ReportJson::object();
    for (const DiagnosticField& field : fields) {
        object[field.first] = field.second;
    }
    return object;
}

ReportJson reportItemJson(const ReportDiagnostic& item) {
    ReportJson row = ReportJson::object();
    row["code"] = item.code;
    row["severity"] = severityName(item.severity);
    row["message"] = item.message;
    row["element"] = item.element;
    row["source"] = item.source;
    row["line"] = item.line;
    row["col"] = item.col;
    row["ui"] = item.ui;
    row["detail"] = reportFieldsObject(item.detail);
    ReportJson fix = ReportJson::object();
    fix["kind"] = fixKindName(item.fix.kind);
    fix["applicability"] = fixApplicabilityName(item.fix.applicability);
    if (!item.fix.boardAttr.empty()) {
        fix["board"] = ReportJson{{"attr", item.fix.boardAttr}, {"value", item.fix.boardValue}};
    }
    if (!item.fix.editorOp.empty()) {
        fix["editor"] =
            ReportJson{{"op", item.fix.editorOp}, {"args", reportFieldsObject(item.fix.editorArgs)}};
    }
    row["fix"] = std::move(fix);
    row["accepted"] = item.accepted;
    return row;
}

}  // namespace

const char* severityName(Severity severity) {
    switch (severity) {
        case Severity::Error:
            return "error";
        case Severity::Warning:
            return "warning";
        case Severity::Info:
            return "info";
    }
    return "warning";
}

const char* fixKindName(FixKind kind) {
    switch (kind) {
        case FixKind::None:
            return "none";
        case FixKind::Board:
            return "board";
        case FixKind::Editor:
            return "editor";
    }
    return "none";
}

const char* fixApplicabilityName(FixApplicability applicability) {
    switch (applicability) {
        case FixApplicability::Hint:
            return "hint";
        case FixApplicability::Maybe:
            return "maybe";
        case FixApplicability::Machine:
            return "machine";
    }
    return "hint";
}

DiagnosticCounts countDiagnostics(const std::vector<ReportDiagnostic>& items) {
    DiagnosticCounts counts;
    for (const ReportDiagnostic& item : items) {
        if (item.accepted) {
            ++counts.accepted;
        } else if (item.severity == Severity::Error) {
            ++counts.errors;
        } else if (item.severity == Severity::Warning) {
            ++counts.warnings;
        } else {
            ++counts.infos;
        }
    }
    return counts;
}

std::string diagnosticsJson(const std::vector<ReportDiagnostic>& items, std::string_view tool,
                            std::string_view source) {
    ReportJson root = ReportJson::object();
    root["version"] = 1;
    root["tool"] = std::string(tool);
    root["source"] = std::string(source);
    ReportJson rows = ReportJson::array();
    for (const ReportDiagnostic& item : items) {
        rows.push_back(reportItemJson(item));
    }
    root["items"] = std::move(rows);
    // Board text is not guaranteed to be valid UTF-8; a bad byte becomes U+FFFD instead of throwing.
    return root.dump(2, ' ', false, ReportJson::error_handler_t::replace) + "\n";
}

std::string toolWithVersion(std::string_view tool) {
    return std::string(tool) + " " + std::to_string(XT_VERSION_MAJOR) + "." +
           std::to_string(XT_VERSION_MINOR) + "." + std::to_string(XT_VERSION_PATCH);
}

std::string formatDiagnostic(const ReportDiagnostic& item) {
    std::string line = std::string(severityName(item.severity)) + " " + item.code + " " +
                       (item.source.empty() ? std::string("<board>") : item.source) + ":" +
                       std::to_string(item.line) + ":" + std::to_string(item.col);
    if (!item.element.empty()) {
        line += " #" + item.element;
    }
    line += ": " + item.message;
    if (item.fix.kind == FixKind::None) {
        return line;
    }
    line += std::string("\n  fix (") + fixApplicabilityName(item.fix.applicability) + ", " +
            fixKindName(item.fix.kind) + "): ";
    if (item.fix.kind == FixKind::Board) {
        line += item.fix.boardAttr + "=\"" + item.fix.boardValue + "\"";
    } else {
        line += item.fix.editorOp;
        for (const DiagnosticField& arg : item.fix.editorArgs) {
            line += " " + arg.first + "=" + arg.second;
        }
    }
    return line;
}

ReportDiagnostic fromCompileDiagnostic(const Diagnostic& diagnostic) {
    ReportDiagnostic item;
    const bool error = diagnostic.level == Diagnostic::Level::Error;
    item.code = !diagnostic.code.empty() ? diagnostic.code : error ? "compile.error" : "compile.warning";
    item.severity = error ? Severity::Error : Severity::Warning;
    item.message = diagnostic.message;
    item.source = diagnostic.file;
    item.line = diagnostic.line;
    item.col = diagnostic.col;
    return item;
}

bool writeDiagnosticsFile(std::string_view path, std::string_view text) {
    std::ofstream file(pathFromUtf8(path), std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(file);
}

}  // namespace xt::compile
