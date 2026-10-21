#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xt/compile/diagnostics.h"

/// `xt_compile`'s public compile entry points. See docs/design/12-compiler.md §12.1,
/// docs/tasks/T-16.md "compile.h", docs/tasks/T-23.md.

namespace xt::compile {

/// Options controlling a `compileDocument()`/`compileLottie()` run.
struct CompileOptions {
    /// Base directory external assets (e.g. image assets) resolve against.
    std::string rootDir;
    /// Directories a relative `<img src>`/`<anim src>`/`@font-face src` is tried against, in order,
    /// before the usual base directory (docs/design/12-compiler.md §12.1, docs/design/22-bundle.md
    /// §22.2 "UI dir -> shared.assetRoot -> bundle root", T-138). The first `dir / src` that exists
    /// wins; when none does (or this is empty) the path resolves exactly as it did without it (the
    /// `.xt` file's directory for `compileDocument()`, `rootDir` for `compileDocumentFromStrings()`).
    /// `<link href>` and `impliedStylesheets` ignore it.
    std::vector<std::string> searchDirs;
    /// true: embed image asset bytes into the ASST chunk.
    bool embedImages = false;
    /// true: embed `@font-face` asset bytes into the ASST chunk.
    bool embedFonts = false;
    /// Any warning is treated as a compile failure.
    bool strict = false;
    /// Emit a META chunk (source file name, compiler version, timestamp).
    bool emitMeta = true;
    /// Stylesheets parsed before the document's own `<link>`/`<style>`, in this order (a bundle's
    /// `shared.styles`, docs/design/22-bundle.md §22.2). Absolute paths, or relative to `rootDir`.
    /// A path that cannot be read is an error, same as an unreadable `<link href>`. T-368:
    /// `compileDocumentFromStrings()` takes the text of an in-memory file whose name is exactly the
    /// path instead of reading it (the editor's unsaved copy).
    std::vector<std::string> impliedStylesheets;
    /// T-235 (item 8): `compileDocumentFromStrings()` only -- the `.xt` file's directory, the base
    /// every relative path of the document (`<link href>`, `<link rel="symbols">`, `.xtanim.json`,
    /// assets after `searchDirs`) resolves against, as `compileDocument()` uses the file's own
    /// directory. Empty: `rootDir`. In-memory `files` still win over the disk.
    std::string xtDir;
    /// T-235 (30.4, 30.9): the standard symbol library directory a `<link rel="symbols"
    /// href="xt-standard:<Name>">` resolves against (`<standardSymbolDir>/<Name>.xt`). `<Name>` is
    /// a plain file stem (no `/`, `\`, `.` or `:`). Empty: such a link is an error ("no standard
    /// symbol library"). Other hrefs ignore it.
    std::string standardSymbolDir;
    /// Optional: maps every external (non-embedded) asset reference to the logical name written
    /// into ASST (docs/design/22-bundle.md §22.3). Called once per distinct asset with the name as
    /// written in the source, the resolved path on disk (empty when the file was not found), and
    /// the kind (0 image, 1 font). Returning an empty string keeps the source name. Embedded assets
    /// keep their source name and never reach this callback.
    std::function<std::string(std::string_view sourceName, std::string_view resolvedPath, uint8_t kind)>
        assetNameRewrite;
    /// T-273b: a published build (pack()). An asset whose source name lies outside the project
    /// (assetLeavesProject() against `rootDir`) is referenced by its ASST name everywhere the
    /// document names it (`<img src>`, `<sequence src>`, `<particles texture>`, an image-naming
    /// declaration), so no machine path or `..` escape reaches the `.xtir`: a non-embedded one by
    /// the name `assetNameRewrite` gave it, an embedded one by its file name (`<stem>-2<ext>`,
    /// `<stem>-3<ext>`... when another ASST entry already has that name). The reference text is
    /// interned as that name, so it shares the ASST entry's StringId. T-273c: an outside asset the
    /// rewrite could not find gets its file name the same way (the "not found" warning stays);
    /// an outside `<link href>` (SRCM), `<anim src>`, `<img src="*.svg">` (the attribute and the
    /// scene / timeline name) and frame-folder `<sequence src>` are written as their last path
    /// element. false (the default, editor preview and `xtframe-compile`): every name stays as
    /// written.
    bool publishAssetNames = false;
    /// T-342 (docs/design/03-scene-and-properties.md §3.13, 22-bundle.md §22.15): the publish view.
    /// true: every element carrying the `editor-only` attribute is removed with its subtree right
    /// after parsing (the document's and every symbol library's), before anything else is compiled,
    /// so neither DOCT nor ASST holds it and an asset only it names is never registered. A remaining
    /// reference to a removed id (`#<id>` in an attribute, a `<use>` of a removed symbol) is a
    /// warning. false (the default; the editor canvas, `xtframe-compile`): the attribute is kept
    /// like any other and the element is compiled as usual.
    bool stripEditorOnly = false;
    /// T-344 (docs/design/17-editor.md §17.31, 23-host-integration.md §23.aa.5): the reverse of
    /// `stripEditorOnly` -- the editor Demo space's scene. true: every element of the document's
    /// own tree that is neither inside an `editor-only` subtree nor an ancestor of one is removed
    /// with its subtree right after parsing, so only the `editor-only` subtrees (and the elements
    /// that place them) are compiled. Symbol definitions and symbol libraries are kept as they
    /// are. No reference warning is raised. Ignored when `stripEditorOnly` is also true.
    bool editorOnlyAsScene = false;
};

/// T-273d: the tail of the warning a published build (CompileOptions::publishAssetNames) gives for
/// a `<video src>` outside the project (assetLeavesProject()), once per distinct `src` of a
/// document: `video "<src as written>` followed by this text. The video is not packed and keeps
/// its `src` (and its `engine://video:<src>` key) as written. pack() matches it to warn once per
/// path across the UIs of one run.
inline constexpr std::string_view kVideoOutsideProjectTail =
    "\" is outside the project; a bundle does not carry video files, so the published UI names it by "
    "this path";

/// T-273b: true when the asset reference `src` is a machine path: it has a root name or a root
/// directory on this platform, or it reads as one on any platform (`<letter>:` at the start, or a
/// leading `//` or `\\`).
bool isMachinePath(std::string_view src);

/// T-273b: true when the asset reference `src`, written in a document in `xtDir`, lies outside
/// `rootDir` (empty: `xtDir`): isMachinePath(), or a relative path that escapes the root through
/// `..` (pathInsideRoot()). False for an empty `src` and for an `engine://` name.
bool assetLeavesProject(std::string_view src, std::string_view xtDir, std::string_view rootDir);

/// T-273c: the one definition of "inside the project" the compiler, the editor and the Editor
/// Service share. True when `path` is `root` or lies below it, element by element, both taken
/// lexically normal (a trailing separator, or a `.` element of `root`, does not count); on Windows
/// the comparison ignores case. Both are absolute, or both relative to the same base.
bool pathInsideRoot(const std::filesystem::path& path, const std::filesystem::path& root);

/// T-273c: the reference `ref`, written in a file in `baseDir`, as a lexically normal path: `ref`
/// itself when isMachinePath() (so `C:/x` is never taken as relative, on any platform), else
/// `baseDir / ref`.
std::filesystem::path referencePath(std::string_view ref, const std::filesystem::path& baseDir);

/// One ASST entry as the compiler produced it, reported back through `CompileResult::assets` so a
/// bundle writer knows which files to pack and under which name they are referenced.
struct CompiledAsset {
    /// Final logical name in ASST (after `CompileOptions::assetNameRewrite`).
    std::string name;
    /// The name as written in the source.
    std::string sourceName;
    /// Resolved path on disk; empty when the file was not found (or the source came from memory).
    std::string resolvedPath;
    /// 0 = Image, 1 = Font, 2 = Shader, 3 = AtlasImage (T-199: an image `assetNameRewrite`
    /// renamed to `atlas:<n>#<asset>`; a bundle's atlas page holds it).
    uint8_t kind = 0;
    /// True when the asset's bytes were embedded into the ASST chunk.
    bool embedded = false;
    /// True for an `engine://<name>` external texture (docs/tasks/T-150.md): the name is kept
    /// verbatim, nothing was looked up on disk, `resolvedPath` is empty and a bundle neither
    /// packs nor lists it -- the host binds it at runtime (AssetResolver::setExternalTexture).
    bool external = false;
};

/// Outcome of a compile call: the finished `.xtir` bytes plus any diagnostics raised along the way.
struct CompileResult {
    /// Empty means failure; see `diagnostics` for why.
    std::vector<uint8_t> ir;
    /// Warnings and errors collected during the compile, whether or not it ultimately succeeded.
    std::vector<Diagnostic> diagnostics;
    /// Every ASST entry, in AssetId order; filled whether or not the compile succeeded.
    std::vector<CompiledAsset> assets;
    /// T-342: with CompileOptions::stripEditorOnly, the `id` of every element that was removed
    /// (document order, the document's own tree only), so a caller can check its own references
    /// to them (a bundle's `.xtflow`). Empty otherwise.
    std::vector<std::string> strippedIds;
    /// True if compilation succeeded (`ir` is non-empty).
    bool ok() const { return !ir.empty(); }
};

/// Compiles a standalone Lottie (bodymovin) JSON file into an `.xtir` blob containing exactly one
/// Scene, one Timeline, an empty STYL, and a 3-element DOCT (`xt` > `body` > `anim`, the `anim`
/// referencing the parsed scene with `autoplay="true"` `loop="true"`). See docs/tasks/T-16.md
/// "compile.h" for the exact construction rules.
CompileResult compileLottie(std::string_view jsonPath, const CompileOptions& options);

/// Compiles a full `.xt` document (DomTree + stylesheets + `@keyframes` + Lottie `<anim>` scenes)
/// into a complete `.xtir` blob: `STYL` (rules/decls/transitions/animations/fontFamilies/
/// fontFaces), one `SCEN`+`TLIN` pair per distinct `<anim src>`, one `TLIN` per `@keyframes` block,
/// `DOCT`, and `ASST` (`img`/`@font-face` assets). Every relative path (`<link href>`, `<anim src>`,
/// `<img src>`, `@font-face src`) resolves against `xtPath`'s own directory, not `options.rootDir`
/// (see docs/tasks/T-23.md report); `<anim src>`/`<img src>`/`@font-face src` first try
/// `options.searchDirs` in order (T-138). See docs/design/12-compiler.md §12.2-§12.3 for the exact
/// pipeline.
CompileResult compileDocument(std::string_view xtPath, const CompileOptions& options);

/// One in-memory source file for `compileDocumentFromStrings()`.
struct SourceFile {
    std::string_view name;
    std::string_view text;
};

/// Same pipeline as `compileDocument()`, but every file is supplied in memory instead of read from
/// disk (docs/design/17-editor.md §17.7, docs/tasks/T-59.md): used by the editor's `PreviewSession`
/// to recompile a `Document`/`Stylesheet`s that may not correspond to any file on disk yet.
/// `mainName` must name one of `files`' entries (the `.xt` document itself) -- if it doesn't, this
/// reports an error and returns an empty result, same as a failed `compileDocument()` read.
///
/// Each `<link href>` is resolved, in order: an exact match against `files`' `name`s; failing that,
/// a match against `files`' `name`s with their directory stripped (so a `files` entry named
/// "app.xts" still resolves a `<link href="styles/app.xts">`); failing that, `options.rootDir /
/// href` read off disk. Every other relative path (`<anim src>`, `<img src>`, `@font-face src`)
/// resolves against `options.rootDir` directly (there being no real `.xt` file path to take a
/// directory from, unlike `compileDocument()`), after first trying `options.searchDirs` in order
/// (T-138).
///
/// The `SRCM` block's file name table lists the main file first, then every `<link href>` the
/// document actually has, in document order (its literal href text, not a resolved path) --
/// `compileDocument()` emits the very same table (driven by the same `<link>` list), so the two
/// agree byte-for-byte when `files` supplies exactly the main document plus its `<link>`ed files
/// (docs/design/17-editor.md §17.7, docs/tasks/T-59.md's test). Element/text source entries still
/// all point at the main file (fileId 0), same as `compileDocument()`.
CompileResult compileDocumentFromStrings(std::span<const SourceFile> files, std::string_view mainName,
                                         const CompileOptions& options);

}  // namespace xt::compile
