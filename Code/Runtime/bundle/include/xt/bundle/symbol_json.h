#pragma once

#include <json.hpp>
#include <string>

#include "xt/bundle/symbol_library.h"
#include "xt/bundle/symbol_lint.h"
#include "xt/bundle/symbol_set.h"

/// T-502 (Docs/design/49-symbol-sets.md 49.7): the JSON shapes of the symbol library model, as the
/// Editor Service's `symbols.*` operations and the `xtframe://symbols/...` MCP resources return
/// them. The same rows `xtframe-symbols --json` prints and the editor's Symbols tab shows, so an
/// agent reads one lint structure everywhere ("缺狀態" is `contract.state` in every reply). Only
/// data goes in: nothing here reads a file or throws.

namespace xt::bundle {

using SymbolJson = nlohmann::json;

/// The wire name of a mount scope: "preset" | "user" | "project" | "extra".
const char* libraryScopeName(LibraryScope scope);

/// The wire name of an import action: "added" | "updated" | "unchanged" | "older-skipped" |
/// "older-replaced" (the same words `xtframe-symbols import --json` prints).
const char* importActionName(ImportAction action);

/// `symbols.libraries`: one row per mounted library -- `{id, name, path, readOnly, scope,
/// defaultGroup, groups: [<group id>...], enabled: true}` -- followed by one row per
/// `mounts.entries` line whose directory is not mounted (disabled, or failed to load): `{id: "",
/// name: "", path, readOnly: false, scope: "extra", defaultGroup: "", groups: [], enabled}`.
SymbolJson librariesJson(const LibraryMounts& mounts);

/// `symbols.get`: the full `index.json` entry -- `{name, file, thumbnail, description, category,
/// requires: {states, slots}, ports: [{name, type, dir}], timelines, content, tokens: [{name,
/// default}], fonts, textures}`.
SymbolJson symbolEntryJson(const SymbolEntry& entry);

/// A symbol as `symbols.library` summarises it: `{name, file, category, description, thumbnail,
/// requires: {states, slots}, ports: [<name>...], timelines}`.
SymbolJson symbolSummaryJson(const SymbolEntry& entry);

/// One group as `symbols.library` lists it: `{id, name, description, dir, source: {kind, set,
/// setVersion}, tokens, themes, thumbnail, lint: {errors, warnings}, problems: [{level, message}],
/// symbols: [symbolSummaryJson...]}`. `lint` is the persisted mark (`library.json` `problems`),
/// `problems` what loadLibrary() found wrong with the group's files.
SymbolJson groupJson(const SymbolGroup& group);

/// One library with its groups: `{id, name, path, readOnly, scope, defaultGroup, groups:
/// [groupJson...]}`.
SymbolJson libraryJson(const SymbolLibrary& library);

/// A lint report (49.6): `{id, dir, symbols, errors, warnings, issues: [{level, symbol, code,
/// message}]}` -- the shape `xtframe-symbols lint --json` prints per group.
SymbolJson lintReportJson(const LintReport& report);

/// `symbols.use` / `useGroup` / `updateCopy`: `{status, written, skipped, renamed, tokensAdded,
/// notFound}` (`status` is the status name, "kOk" when everything went).
SymbolJson useReportJson(const UseReport& report);

/// `set.json` as data: `{id, author, name, version, license, fontsLicense, description, homepage,
/// thumbnail, groups, requiresXtframe}`.
SymbolJson setInfoJson(const SetInfo& set);

/// `symbols.import`: `{status, error, set: setInfoJson, groups: [{sourceId, targetId, action,
/// trashed, lint: lintReportJson}], lintErrors, lintWarnings}`.
SymbolJson importReportJson(const ImportReport& report);

}  // namespace xt::bundle
