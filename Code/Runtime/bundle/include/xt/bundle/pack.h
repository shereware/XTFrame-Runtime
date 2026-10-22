#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "xt/bundle/gen_ports.h"
#include "xt/bundle/project_file.h"
#include "xt/bundle/ui_project.h"
#include "xt/compile/diagnostics.h"

/// pack(): turning a bundle's source form into its published form. See
/// Docs/design/22-bundle.md §22.3 (layout, deduplication, manifest) and §22.4 (`xtframe-pack`),
/// Docs/tasks/T-92.md.
///
/// Every UI listed in the `ProjectFile` is compiled with `xt_compile`, its external assets are
/// renamed to `assets/<hash16>.<ext>` and written once per distinct content, its connection points
/// are reflected with `xt_flow`, a thumbnail is rendered with `xt_sw`, and the result is described
/// by `manifest.json`. `xtframe-pack` is a thin CLI over this function; the editor's "publish"
/// command (T-94) calls it directly.

namespace xt::bundle {

/// What to produce, and where. See §22.4 for the matching CLI flags.
struct PackOptions {
    /// Output directory; created when missing, updated in place otherwise.
    std::string outDir;
    /// UI ids to re-pack (`--ui`); empty means every UI in the project.
    std::vector<std::string> onlyUis;
    /// Treat any compile warning as an error (`--strict`).
    bool strict = false;
    /// Render `thumbs/<id>.png` for each packed UI (`--no-thumbs` clears this).
    bool thumbnails = true;
    /// Force `publish.embedImages`/`publish.embedFonts` on (`--embed`).
    bool embed = false;
    /// Delete files under `assets/`, `ui/` and `thumbs/` the manifest no longer references
    /// (`--prune`); skipped when the run failed (T-262a).
    bool prune = false;
    /// T-183: let pack(const UiProject&) write filled-in `measured` fields back to
    /// `manifest/assets.yaml`; a read-only caller (xtframe-browse) clears this.
    bool writeAssetsManifest = true;
    /// T-179 (§22.10, 12.7 `--lang <list>`): the languages whose `strings/<lang>.csv` is packed;
    /// empty = every `strings/*.csv` of the project.
    std::vector<std::string> languages;
    /// T-198 (Docs/design/30-ui-project.md 30.4, 30.8): the standard symbol library directory
    /// (`<applicationDirPath>/symbols` for xtframe-pack). A UI's `<link rel="symbols" href>` that
    /// names a missing file directly inside the project's symbol directory (`shared.symbols`,
    /// "shared/symbols" for a legacy bundle) is filled from here when this directory has a file of
    /// that name; empty = off.
    std::string standardSymbolDir;
    /// T-199 (26 §26.3.4, 12.7 `--atlas`): pack the bundle's bitmap images into atlas pages
    /// (`atlas/<name>.png`, manifest `atlas[]`, `.xtir` names `atlas:<name>#<asset>`). Also on
    /// when the UI Project's `publish.atlas.enabled` is set. Needs a full run (no `--ui`).
    bool atlas = false;
    /// T-199: "ui" (one page set per UI) or "bundle" (one set for the whole bundle); empty = the
    /// UI Project's `publish.atlas.scope`, else "ui".
    std::string atlasScope;
    /// T-199 (12.7 `--compress`): "none" (default), "bc1", "bc3" or "bc7": bitmap assets and
    /// atlas pages are written as DDS in that block format (an image with alpha gets bc3 under
    /// bc1; `manifest/assets.yaml` `compress: <fmt|none>` overrides per asset).
    std::string compress = "none";
    /// T-199 (12.7 `--font-subset`): every bundled TrueType font is rewritten as
    /// `fonts/<stem>.subset.ttf` keeping only the glyphs the UIs' text and string tables use
    /// (plus ASCII); missing glyphs are reported as `i18n` warnings. Needs a full run.
    bool fontSubset = false;
    /// T-312a (35 §35.15, `--allow-placeholders`): a `manifest/assets.yaml` entry with status
    /// `placeholder` is an error (`asset.placeholderShipped`) unless this is set; then it is a
    /// warning.
    bool allowPlaceholders = false;
    /// T-314 (Docs/design/36-history.md §36.4): written to the manifest as `project.commit` /
    /// `project.checkpoint` when non-empty (only for a UI Project). Plain strings: the bundle layer
    /// does not know where they come from (the editor's history).
    std::string projectCommit;
    std::string projectCheckpoint;
};

/// What a pack() run did.
struct PackReport {
    /// Every UI's compile diagnostics; `file` is prefixed with "<ui id>: ".
    std::vector<compile::Diagnostic> diagnostics;
    /// The UI ids actually written out this run, in project order.
    std::vector<std::string> packedUis;
    /// Assets whose bytes were written to `assets/` this run.
    uint32_t assetsWritten = 0;
    /// Asset references that reused an already-written `assets/` file.
    uint32_t assetsDeduplicated = 0;
    /// Files deleted by `PackOptions::prune`.
    uint32_t assetsPruned = 0;
    /// True when every requested UI compiled and was written.
    bool ok = false;
    /// T-183: pack() filled missing `measured` fields and wrote `manifest/assets.yaml` back.
    bool assetsManifestUpdated = false;
    /// T-193 (`xtframe-pack --gen-ports`, 23.10 item 1): the declared-port table of every UI packed
    /// this run, in project order.
    std::vector<UiPortTable> portTables;
    /// T-193 (§22.10 `flows.global`): the global flow was found and written as `bundle.xtflow`.
    bool globalFlowWritten = false;
    /// T-198: standard symbol files copied into the project's symbol directory this run (paths
    /// relative to the project root, '/' separators).
    std::vector<std::string> standardSymbolsCopied;
    /// T-199: atlas pages written and entries placed; `atlasPixelsUsed` / `atlasPixelsTotal` =
    /// entry pixels over page pixels (the packing efficiency).
    uint32_t atlasPages = 0;
    uint32_t atlasEntries = 0;
    uint64_t atlasPixelsUsed = 0;
    uint64_t atlasPixelsTotal = 0;
    /// T-199: assets (and atlas pages) written as DDS.
    uint32_t assetsCompressed = 0;
    /// T-199: `@2x` variants found next to their source and packed.
    uint32_t variants2x = 0;
    /// T-345 (25 §25.4.2 item 4): `@3x` variants packed.
    uint32_t variants3x = 0;
    /// T-199: fonts rewritten as subsets.
    uint32_t fontsSubset = 0;
};

/// Packs `project` into `options.outDir` per §22.3/§22.4. With a non-empty `options.onlyUis` and an
/// existing manifest the run is incremental: only those UIs are recompiled and only their manifest
/// entries (plus any newly written assets) change. T-262a: an incremental run into a bundle whose
/// manifest has `atlas[]` or subset fonts is refused (nothing is written); a failed run never
/// prunes.
PackReport pack(const ProjectFile& project, const PackOptions& options);

/// T-183 (Docs/design/22-bundle.md §22.10, 12.7, 26.3.3): packs a UI Project. Same as
/// pack(project.toBundleProject(), options) plus: themes and string tables come from
/// `shared.themes`/`shared.strings`; an empty `options.languages` falls back to
/// `publish.languages`; the manifest gets `project {name, game, version}` and, when
/// `contracts/motion-spec.yaml` exists, `motionSpec` (that file as JSON; a malformed file is an
/// error); `manifest/assets.yaml`, when present, is validated (lint `assets`, checkAssets(); the
/// unreferenced check only on full, non-embedding runs), its missing `measured` fields are filled
/// and written back, and every packed asset whose source file it lists carries
/// `manifest {id, w, h, alpha, slice}` in the manifest's `assets[]` (T-312a: an entry with status
/// `placeholder` is the error `asset.placeholderShipped`, a warning under
/// PackOptions::allowPlaceholders, instead of checkAssets()'s status error). Nothing under `references/`,
/// `contracts/`, `shared.recipes` and no editor sidecar is ever copied. A `legacy` project (read
/// from `bundle.xtproj`) packs exactly like pack(project.toBundleProject(), options).
PackReport pack(const UiProject& project, const PackOptions& options);

/// T-200: a packed `.xtir`'s `uis[].fileHash` -- FNV-1a 64 over `ir` with the `META` chunk's
/// compile time taken as 0, so packing unchanged sources in different seconds gives the same
/// hash. Bytes that are not a valid IR are hashed as they are.
uint64_t irFileHash(std::span<const uint8_t> ir);

}  // namespace xt::bundle
