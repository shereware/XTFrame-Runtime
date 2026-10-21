#include "xt/bundle/migrations.h"

#include <string>
#include <utility>

/// The migration registry (docs/design/43-compatibility-and-versions.md §43.2, docs/tasks/T-406.md).
/// Adding version N + 1 of a format: bump its k<Format>FormatVersion, write the step
/// `bool <format>V<N>ToV<N+1>(json&, std::string&)` here and put it at index N of the format's step
/// array (one slot per version below the latest; slot 0 stays null).

namespace xt::bundle {

namespace {

using nlohmann::json;

void setErrorMigrations(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

// --- the sequences (all empty: every format is at version 1) ---

constexpr MigrationTable kUiProjectTable{kUiProjectFormatVersion, nullptr, 0};
constexpr MigrationTable kFlowTable{kFlowFormatVersion, nullptr, 0};
constexpr MigrationTable kEditorStateTable{kEditorStateFormatVersion, nullptr, 0};
constexpr MigrationTable kMetaTable{kMetaFormatVersion, nullptr, 0};
constexpr MigrationTable kDesignYamlTable{kDesignYamlFormatVersion, nullptr, 0};

}  // namespace

const MigrationTable& migrationTable(SourceFormat format) {
    switch (format) {
        case SourceFormat::UiProject:
            return kUiProjectTable;
        case SourceFormat::Flow:
            return kFlowTable;
        case SourceFormat::EditorState:
            return kEditorStateTable;
        case SourceFormat::Meta:
            return kMetaTable;
        case SourceFormat::DesignYaml:
            return kDesignYamlTable;
    }
    return kUiProjectTable;
}

bool readFormatVersion(const json& doc, int& out, std::string* error, const char* key) {
    out = 1;
    if (!doc.is_object()) {
        return true;
    }
    const auto it = doc.find(key);
    if (it == doc.end()) {
        return true;
    }
    if (it->is_number_unsigned()) {
        const auto value = it->get<unsigned long long>();
        if (value >= 1 && value <= 1000000) {
            out = static_cast<int>(value);
            return true;
        }
    } else if (it->is_number_integer()) {
        const auto value = it->get<long long>();
        if (value >= 1 && value <= 1000000) {
            out = static_cast<int>(value);
            return true;
        }
    }
    setErrorMigrations(error, std::string("'") + key + "' is not a positive integer: " + it->dump());
    return false;
}

Result<int> migrateWith(const MigrationTable& table, json& doc, int from, std::string* error) {
    if (from > table.latest) {
        setErrorMigrations(error, "format version " + std::to_string(from) +
                                      " is newer than this XTFrame supports (" +
                                      std::to_string(table.latest) + "); a newer XTFrame is needed");
        return Result<int>::error(Status::kUnsupported);
    }
    if (from < 0) {
        setErrorMigrations(error, "format version " + std::to_string(from) + " is not supported");
        return Result<int>::error(Status::kUnsupported);
    }
    int version = from;
    while (version < table.latest) {
        const size_t index = static_cast<size_t>(version);
        if (table.steps == nullptr || index >= table.count || table.steps[index] == nullptr) {
            setErrorMigrations(error, "format version " + std::to_string(version) +
                                          " is not supported (no migration from it)");
            return Result<int>::error(Status::kUnsupported);
        }
        std::string message;
        if (!table.steps[index](doc, message)) {
            setErrorMigrations(
                error, "migration from format version " + std::to_string(version) + " failed: " + message);
            return Result<int>::error(Status::kInvalidArgument);
        }
        ++version;
    }
    return Result<int>::ok(version);
}

Result<int> migrateUiProject(json& doc, int from, std::string* error) {
    return migrateWith(kUiProjectTable, doc, from, error);
}

Result<int> migrateFlow(json& doc, int from, std::string* error) {
    return migrateWith(kFlowTable, doc, from, error);
}

Result<int> migrateEditorState(json& doc, int from, std::string* error) {
    return migrateWith(kEditorStateTable, doc, from, error);
}

Result<int> migrateMeta(json& doc, int from, std::string* error) {
    return migrateWith(kMetaTable, doc, from, error);
}

Result<int> migrateDesignYaml(json& doc, int from, std::string* error) {
    return migrateWith(kDesignYamlTable, doc, from, error);
}

Status migrateToLatest(SourceFormat format, json& doc, int* fromVersion, std::string* error,
                       const char* key) {
    int from = 1;
    if (!readFormatVersion(doc, from, error, key)) {
        return Status::kInvalidArgument;
    }
    if (fromVersion != nullptr) {
        *fromVersion = from;
    }
    const Result<int> reached = migrateWith(migrationTable(format), doc, from, error);
    return reached.ok() ? Status::kOk : reached.status();
}

}  // namespace xt::bundle
