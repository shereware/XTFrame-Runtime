#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xt/types.h"

/// ProjectFile: the source form of a bundle, i.e. `bundle.xtproj` v2. See
/// Docs/design/22-bundle.md §22.2 and Docs/tasks/T-90.md.
///
/// A bundle's source form is a directory holding `bundle.xtproj` plus each UI's `.xt`/`.xts`/
/// `.xtflow` and the shared assets. Opening a lone `.xt` file yields an *implied* bundle with one
/// UI (see ProjectFile::implied), so the editor has exactly one project model either way.
///
/// Paths stored here use '/' separators and are relative to `rootDir` (the directory holding
/// `bundle.xtproj`). JSON never leaks into this header; it lives in project_file.cpp.

namespace xt::bundle {

/// One UI listed in `uis`. `id` is the *stable* id hosts bind to: renaming `doc` never changes it.
struct ProjectUi {
    /// Stable UI id; lowercase alphanumerics and '-'.
    std::string id;
    /// The UI's document, relative to the bundle root ('/' separated).
    std::string doc;
    /// The UI's optional flow graph, relative to the bundle root; empty if there is none.
    std::string flow;
    /// Design viewport, also the thumbnail/browser aspect ratio.
    Vec2 size{1280, 720};
    /// Free-form tags for browsing/filtering.
    std::vector<std::string> tags;
    /// Viewport fit mode carried into the manifest: "native", "stretch", "contain", "cover" or
    /// "center". A missing key reads as "native"; save() omits the key when it is "native".
    std::string fit = "native";
};

/// The `shared` object: assets and stylesheets every UI in the bundle sees.
struct ProjectShared {
    /// Directory searched after a UI's own directory when resolving assets; "" means the root.
    std::string assetRoot;
    /// Directories scanned for fonts.
    std::vector<std::string> fontDirs;
    /// Stylesheets implicitly loaded before every UI's own `<link>`s.
    std::vector<std::string> styles;
    /// Directories scanned for templates/effects.
    std::vector<std::string> templateDirs;
};

/// T-527 (Docs/design/22-bundle.md §22.10.3, 12 §12.7.1 `--fonts`): where a bundle's text gets its
/// fonts. kSystem (the default, T-97): the pack's thumbnail shaper also registers the system fonts
/// and nothing checks glyph coverage. kBundled: only the bundle's own fonts count -- every packed
/// UI's text must have a glyph in a font of that UI's bundle (an error otherwise), and an implied
/// symbol group's `@font-face` fonts are the last fallback.
enum class FontsMode : uint8_t { kSystem, kBundled };

/// T-527: "system" -> kSystem, "bundled" -> kBundled; false for any other text (`out` unchanged).
bool parseFontsMode(std::string_view text, FontsMode& out);

/// T-527: the `publish.fonts` / `--fonts` spelling of `mode`.
const char* fontsModeName(FontsMode mode);

/// The `publish` object: what `xtframe-pack` should produce.
struct ProjectPublish {
    /// Embed image assets into each `.xtir` instead of referencing `assets/`.
    bool embedImages = false;
    /// Embed font assets into each `.xtir` instead of referencing `assets/`.
    bool embedFonts = false;
    /// Thumbnail size rendered per UI.
    Vec2 thumbnail{320, 180};
    /// T-527: `publish.fonts` ("system" | "bundled"; absent = system). Written only when bundled.
    FontsMode fonts = FontsMode::kSystem;
};

/// A parsed `bundle.xtproj`. Field order matches §22.2's JSON, which save() reproduces verbatim.
struct ProjectFile {
    /// Format version; only 2 is supported.
    int version = 2;
    /// Human-readable bundle name.
    std::string name;
    /// Stable bundle id; must match `[a-z0-9-]+`.
    std::string id;
    /// The bundle's UIs, in `bundle.xtproj` order.
    std::vector<ProjectUi> uis;
    /// Shared assets/stylesheets (§22.2 `shared`).
    ProjectShared shared;
    /// Editor preview device pixel ratio (§22.2 `preview.dpr`).
    float previewDpr = 1.0f;
    /// "desktop" or "engine"; see §22.2 and 17.8.
    std::string target = "engine";
    /// Publish options (§22.2 `publish`).
    ProjectPublish publish;
    /// The directory `bundle.xtproj` was loaded from, exactly as the caller spelled it (no
    /// trailing '/'). Not stored in the JSON.
    std::string rootDir;

    /// Reads `<dir>/bundle.xtproj`. Returns kNotFound when the file cannot be read, kIoError when
    /// it is not valid JSON, kUnsupported when `version` != 2, and kInvalidArgument when `id` or
    /// `uis` is missing, `id` is not `[a-z0-9-]+`, a UI lacks `id`/`doc`, a UI id is malformed,
    /// two UIs share an id, or `publish.fonts` is neither "system" nor "bundled" (T-527). `error`, when
    /// non-null, receives a human-readable message for the editor's message panel (it is cleared first).
    static Result<ProjectFile> load(std::string_view dir, std::string* error = nullptr);

    /// The implied single-UI bundle for a lone `.xt` file (§22.2): `id` and `name` are the file's
    /// stem with every character outside `[a-z0-9-]` replaced by '-' (uppercase is lowercased),
    /// the one UI's `doc` is the file name, `rootDir` is the file's directory, and
    /// `shared.assetRoot` is empty (assets resolve against `rootDir`).
    static ProjectFile implied(std::string_view xtPath);

    /// Writes `<rootDir>/bundle.xtproj`: §22.2's key order, 2-space indent, UTF-8, LF endings,
    /// trailing newline. Returns kInvalidArgument when `rootDir` is empty, kIoError when the file
    /// cannot be written. Byte-stable: saving the same ProjectFile twice yields identical bytes.
    Status save(std::string* error = nullptr) const;

    /// The UI with this id, or nullptr. The pointer is invalidated by any change to `uis`.
    const ProjectUi* ui(std::string_view id) const;
};

}  // namespace xt::bundle
