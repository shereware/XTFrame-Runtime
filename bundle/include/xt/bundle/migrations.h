#pragma once

#include <cstddef>
#include <json.hpp>
#include <string>

#include "xt/flow/flow_graph.h"
#include "xt/types.h"

/// Source-format versions and the "read old, write new" migration registry
/// (docs/design/43-compatibility-and-versions.md §43.2, docs/tasks/T-406.md).
///
/// Every source format carries a `version` (missing = 1). Opening a file reads its version, runs
/// the format's migration steps from that version up to the latest, and the writer always writes
/// the latest. A version newer than the latest is rejected ("a newer XTFrame is needed"), never
/// guessed at. Shared by the editor and `xtframe-pack` (R layer).
///
/// Every sequence is empty today (all formats are at version 1); the framework is exercised by
/// tests with a fake table (migrateWith()).

namespace xt::bundle {

/// The latest version of each source format (43 §43.2 table).
inline constexpr int kUiProjectFormatVersion = 1;                    ///< `<name>.xtui`, `ui-project.json`
inline constexpr int kFlowFormatVersion = flow::kFlowFormatVersion;  ///< `.xtflow`
inline constexpr int kEditorStateFormatVersion = 1;                  ///< `.xteditor.json`
inline constexpr int kMetaFormatVersion = 1;                         ///< asset and reference `.meta`
/// `design/*.yaml`, `contracts/*.yaml` and `conventions/*` (one sequence for the project's YAML
/// parts).
inline constexpr int kDesignYamlFormatVersion = 1;
/// `manifest/assets.yaml`: versioned and checked (newer refused) but generated from the metas, so
/// it has no migration sequence.
inline constexpr int kAssetsManifestVersion = 1;

/// The source formats with a migration sequence.
enum class SourceFormat { UiProject, Flow, EditorState, Meta, DesignYaml };

/// One migration step: rewrites `doc` from version v to v + 1 in place. False (with `error`) when
/// this document cannot be migrated.
using MigrationStep = bool (*)(nlohmann::json& doc, std::string& error);

/// A format's migration sequence: `steps[v]` migrates version v to v + 1 (index = starting
/// version; entries for versions that never existed are null). `latest` is the version written
/// today; `count` <= `latest`.
struct MigrationTable {
    int latest = 1;
    const MigrationStep* steps = nullptr;
    size_t count = 0;
};

/// The registered sequence of `format`.
const MigrationTable& migrationTable(SourceFormat format);

/// Reads the integer member `key` of `doc`: missing (or `doc` not an object) -> 1. False (with
/// `*error`) when the member is present but not a positive integer.
bool readFormatVersion(const nlohmann::json& doc, int& out, std::string* error = nullptr,
                       const char* key = "version");

/// Runs `table` on `doc` from version `from` up to `table.latest` and returns the version reached
/// (= `table.latest`; `from` itself when it already is the latest). Does not touch `doc`'s
/// `version` member (the writer stamps the latest). Errors: kUnsupported when `from` is newer than
/// `table.latest` ("... a newer XTFrame is needed") or when no step leads from `from` (an unknown
/// old version); kInvalidArgument when a step fails (its message in `*error`).
Result<int> migrateWith(const MigrationTable& table, nlohmann::json& doc, int from,
                        std::string* error = nullptr);

/// migrateWith() over the registered sequence of each format.
Result<int> migrateUiProject(nlohmann::json& doc, int from, std::string* error = nullptr);
Result<int> migrateFlow(nlohmann::json& doc, int from, std::string* error = nullptr);
Result<int> migrateEditorState(nlohmann::json& doc, int from, std::string* error = nullptr);
Result<int> migrateMeta(nlohmann::json& doc, int from, std::string* error = nullptr);
Result<int> migrateDesignYaml(nlohmann::json& doc, int from, std::string* error = nullptr);

/// The open path of every reader: readFormatVersion(doc, key), then the format's migrate<Format>().
/// kOk with `*fromVersion` (when not null) = the version the document had; otherwise the error of
/// the two (a malformed version is kInvalidArgument).
Status migrateToLatest(SourceFormat format, nlohmann::json& doc, int* fromVersion = nullptr,
                       std::string* error = nullptr, const char* key = "version");

}  // namespace xt::bundle
