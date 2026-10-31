#pragma once

#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xt/asset_resolver.h"
#include "xt/context.h"
#include "xt/types.h"

/// Bundle: the published form of a bundle, i.e. a directory holding `manifest.json`, one `.xtir`
/// per UI, deduplicated `assets/` and `thumbs/`. See Docs/design/22-bundle.md §22.3 and §22.5,
/// Docs/tasks/T-90.md.
///
/// A host only ever reads the manifest: it lists the UIs by stable id, their design size, the
/// features they require and their connection-point contract, so listing/binding never needs the
/// documents themselves. Loading a UI into a Context (loadUi/reloadUi) is T-93.
///
/// JSON never leaks into this header; it lives in bundle.cpp.

namespace xt {

/// T-263e: the stock `FileStatusFn` ("xt/asset_resolver.h") for `FileAssetResolver::
/// setFileStatusFn` -- xt_core asks the file system nothing itself. Reports the size in bytes and
/// the last-write time (an opaque tick count, only compared for equality) of the regular file at
/// `utf8Path`; false when there is no such file. The path is taken the way the resolver opens
/// its files: UTF-8, through the wide API on Windows; a path that is not valid UTF-8 is treated
/// as missing (T-261f: false). `Bundle::open` sets it on the resolver behind Bundle::assets(); a
/// host that builds a FileAssetResolver itself and calls revalidate() passes it too.
bool fileStatusUtf8(std::string_view utf8Path, uint64_t& size, uint64_t& modifiedTicks);

}  // namespace xt

namespace xt::bundle {

/// One connection point of a UI, reflected at pack time (§22.3 `ports`).
struct PortInfo {
    /// Port name, e.g. "#fire.Click"; stable like the element id it names.
    std::string name;
    /// Value type: "pulse", "bool", "number" or "string".
    std::string type;
    /// True for `ports.in` entries (host -> UI), false for `ports.out`.
    bool isInput = false;
    /// Extra string-valued parameters, e.g. {"prop": "RotationY"}.
    std::map<std::string, std::string> params;
};

/// One UI's manifest entry (§22.3 / §22.5).
struct UiInfo {
    /// Stable UI id.
    std::string id;
    /// Human-readable name.
    std::string name;
    /// The compiled document, relative to the bundle root, e.g. "ui/hud.xtir".
    std::string file;
    /// The flow graph, relative to the bundle root; empty when there is none.
    std::string flow;
    /// The thumbnail, relative to the bundle root; empty when there is none.
    std::string thumb;
    /// Design viewport.
    Vec2 size{0, 0};
    /// Free-form tags.
    std::vector<std::string> tags;
    /// `requires.effects`: the UI needs effect/shader support.
    bool requiresEffects = false;
    /// `requires.space3d`: the UI needs 3D support.
    bool requiresSpace3d = false;
    /// `requires.text`: the UI needs the text stack.
    bool requiresText = false;
    /// Asset names this UI references, as `assets/<hash>.<ext>`.
    std::vector<std::string> assets;
    /// Connection points; inputs and outputs in one list, distinguished by PortInfo::isInput.
    std::vector<PortInfo> ports;
    /// The manifest's recorded `.xtir` content hash; reload() compares it to spot rebuilt UIs.
    uint64_t fileHash = 0;
    /// How a host places the UI in a viewport whose size differs from `size` (§22.3 `fit`):
    /// "native", "stretch", "contain", "cover" or "center". A missing key reads as "native".
    std::string fit = "native";
    /// T-408 (Docs/design/43-compatibility-and-versions.md §43.6, 22 §22.3 `contract`): the UI's
    /// contract hash, 16 lowercase hex digits (bundle::contractHash()); empty for a bundle packed
    /// before it existed.
    std::string contract;
};

/// One theme of a bundle (Docs/design/22-bundle.md §22.10 `themes[]`, T-170): a `.xts` file of
/// `--name: value` token declarations that Context::setTheme() lays over every UI's own tokens.
struct ThemeInfo {
    /// Theme name, the host's setTheme() argument (the file's stem).
    std::string name;
    /// The theme file, relative to the bundle root, e.g. "themes/dark.xts".
    std::string file;
};

/// One language of a bundle (Docs/design/22-bundle.md §22.10 `strings[lang]`, T-179): the
/// `strings/<lang>.csv` table its `@key` text resolves through.
struct StringsInfo {
    /// BCP 47 language tag (the csv file's stem).
    std::string lang;
    /// The table, relative to the bundle root, e.g. "strings/zh-TW.csv".
    std::string file;
};

/// One `fonts.fallback[lang]` chain (§22.10, T-179): the `@font-fallback` families the bundle's
/// UIs declare for `lang`, most preferred first.
struct FontFallbackInfo {
    /// BCP 47 language tag.
    std::string lang;
    /// Family names.
    std::vector<std::string> families;
};

/// T-183 (Docs/design/22-bundle.md §22.10): the manifest's `project` object, written when the
/// bundle was packed from a UI Project (`<name>.xtui`).
struct ProjectInfo {
    /// `<name>.xtui:name`.
    std::string name;
    /// `<name>.xtui:game`.
    std::string game;
    /// `<name>.xtui:version` (the UI Project format version).
    int version = 0;
    /// T-314 (Docs/design/36-history.md §36.4, 22 §22.10): the history commit (40 hex) the bundle
    /// was published from (`project.commit`); empty = not written / not in the manifest.
    std::string commit;
    /// T-314: the checkpoint slug when that commit is exactly a checkpoint (`project.checkpoint`);
    /// empty = not written.
    std::string checkpoint;
};

/// T-241 (Docs/design/23-host-integration.md §23.11, §23.11.2): how uiscale follows the output
/// size (`spec.uiscaleRule`). Height (the default, also for a missing key): output height /
/// logicalHeight; Width: output width / logicalHeight; ShortEdge: the shorter output edge /
/// logicalHeight (a portrait phone scales by its width). T-345 (25 §25.4.2 item 1): Fit =
/// min(output width / design width, output height / logicalHeight), the whole design fits.
enum class UiScaleRule : uint8_t { Height = 0, Width = 1, ShortEdge = 2, Fit = 3 };

/// T-345: the design width `fit` divides by is logicalHeight x this (the spec names no design
/// width; 16:9 is the editor's default canvas aspect).
inline constexpr float kFitDesignAspect = 16.0f / 9.0f;

/// "height", "width", "shortEdge" or "fit".
const char* uiScaleRuleName(UiScaleRule rule);

/// Parses "height" / "width" / "shortEdge" / "fit" into `out`; false (leaving `out` alone)
/// otherwise.
bool parseUiScaleRule(std::string_view text, UiScaleRule& out);

/// The uiscale of an output `width` x `height` under `rule`; 1 when `logicalHeight` or the edge
/// the rule reads is not positive.
float uiScaleFor(UiScaleRule rule, float width, float height, float logicalHeight);

/// T-345 (25 §25.4.2 item 1): `spec.uiscaleClamp: [min, max]` and `spec.uiscaleDpiFloor`, the
/// values a host hands to Context::setUiScaleLimits().
struct UiScaleLimits {
    /// True when `spec.uiscaleClamp` was given (and valid); false = no clamp, written back absent.
    bool clamped = false;
    /// The clamp's lower bound (>= 0).
    float clampMin = 0.0f;
    /// The clamp's upper bound (>= clampMin); infinity = none (`null` in the JSON).
    float clampMax = std::numeric_limits<float>::infinity();
    /// `spec.uiscaleDpiFloor` (>= 0; 0 = none, not written back): uiscale >= dpiFloor x dpr.
    float dpiFloor = 0.0f;
};

/// T-207 (Docs/design/22-bundle.md §22.10 note, 17 §17.4.1 item 5): manifest `spec`, the
/// defaults a host passes to `Context::setDevice()`/`setUiScale()`. Written only for a bundle
/// packed from a UI Project `<name>.xtui` (from its `spec.safeArea`).
struct BundleSpec {
    /// `spec.safeArea.default`: safe-area inset per side, fraction of the viewport (desktop).
    float safeAreaDefault = 0.025f;
    /// `spec.safeArea.console`: the same on consoles (the engine's title-safe replaces it there).
    float safeAreaConsole = 0.05f;
    /// `spec.logicalHeight`: the logical height at which uiscale is 1 (25.4: 1080 = 1.0).
    float logicalHeight = 1080.0f;
    /// T-241: `spec.uiscaleRule` (missing = Height).
    UiScaleRule uiscaleRule = UiScaleRule::Height;
    /// T-345: `spec.uiscaleClamp` / `spec.uiscaleDpiFloor` (missing = no clamp, floor 0).
    UiScaleLimits uiscaleLimits;
    /// T-407 (43 §43.3): `spec.behavior`, the project's behavior version (every UI's IR META carries
    /// the same value); missing = 0, written only when not 0.
    uint32_t behavior = 0;
};

/// T-199 (Docs/design/22-bundle.md §22.10): one `assets[]` row a host may want to know about
/// (formats, variants, atlas membership, the manifest nine-slice).
struct AssetInfo {
    /// `assets/<hash>.<ext>` (the file that exists, e.g. `.dds` when compressed), or the
    /// original name of an atlased asset (whose own file is not written).
    std::string file;
    /// "image", "font" or "shader".
    std::string kind;
    /// Size of the written file in bytes (the original's size for an atlased asset).
    uint64_t bytes = 0;
    /// The source names that reached it.
    std::vector<std::string> sources;
    /// "bc1" / "bc3" / "bc7" for a DDS written by `xtframe-pack --compress`; empty otherwise.
    std::string format;
    /// The `@2x` variant's file, or empty.
    std::string variant2x;
    /// T-345: the `@3x` variant's file (manifest `variants.3x`), or empty.
    std::string variant3x;
    /// Atlas page names holding the asset; empty when it is not atlased.
    std::vector<std::string> atlases;
    /// `manifest.slice` (L, R, T, B) when the source's `manifest/assets.yaml` entry had one.
    bool hasSlice = false;
    int slice[4] = {0, 0, 0, 0};
};

/// T-199 (§22.10 `atlas[]`): one atlas page and its cells.
struct AtlasEntryInfo {
    std::string asset;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};
struct AtlasInfo {
    std::string name;
    /// `atlas/<name>.png` or `.dds`, relative to the bundle root.
    std::string file;
    /// The page's 2x variant, or empty.
    std::string file2x;
    /// "bc1" / "bc3" / "bc7" for a DDS page; empty for PNG.
    std::string format;
    int width = 0;
    int height = 0;
    std::vector<AtlasEntryInfo> entries;
};

/// T-204 (§22.10 `glyphs[device]`): one button glyph of the manifest, i.e. the project's
/// `glyphs/<device>/<key>.<ext>` packed as `asset`.
struct GlyphInfo {
    /// "mouse", "gamepad", "touch" or "keyboard".
    std::string device;
    /// The action / key name (the file's stem).
    std::string key;
    /// The packed asset, `assets/<hash16>.<ext>` (`.dds` under `--compress`).
    std::string asset;
};

/// T-200 (§22.10 `symbols[]`, 26 §26.2): one state group of a symbol (`state-groups="<name>: A|B"`).
struct SymbolStateGroup {
    /// The group name; empty when the attribute gives none.
    std::string name;
    /// The mutually exclusive states.
    std::vector<std::string> states;
};

/// T-200: one `port:<name>="<type> [in|out|call]"` declared inside a symbol definition.
struct SymbolPortInfo {
    std::string name;
    /// The type text as written (`number`, `enum(A|B)`, `list<struct{...}>`, ...).
    std::string type;
    /// `in`, `out` or `call`.
    std::string dir;
};

/// T-200 (§22.10 `symbols[]`): the contract summary of one `<symbol>` definition.
struct SymbolContract {
    /// `<symbol name>`.
    std::string name;
    /// The file defining it, relative to the project root ('/' separators).
    std::string source;
    /// `requires="states: ..."` and `requires="slots: ..."`, in written order.
    std::vector<std::string> requiredStates;
    std::vector<std::string> requiredSlots;
    /// `state-groups`, in written order.
    std::vector<SymbolStateGroup> stateGroups;
    /// The `<slot name>`s the definition has, in document order.
    std::vector<std::string> slotNames;  // Not `slots`: a Qt macro.
    /// The ports declared inside the definition, in document order.
    std::vector<SymbolPortInfo> ports;
};

/// T-200: the contracts of every `<symbol>` definition in one `.xt` source (`sourceName` becomes
/// each SymbolContract::source). Empty when the text does not parse.
std::vector<SymbolContract> readSymbolContracts(std::string_view xtSource, std::string_view sourceName);

/// T-200: a one-line summary, e.g. `states: Hover, Pressed; slots: label; group Level: Low|Ok;
/// ports: label string`; empty for a symbol without a contract, groups or ports.
std::string symbolContractSummary(const SymbolContract& contract);

namespace detail {
/// A parsed manifest.json, before it is committed to a Bundle (bundle.cpp).
struct BundleManifest;
}  // namespace detail

/// A published bundle directory, opened read-only.
class Bundle {
public:
    /// Constructs an empty bundle; open() is the only intended way to get a usable one.
    Bundle();
    /// Destroys the bundle and its asset resolver.
    ~Bundle();

    /// Reads `<dir>/manifest.json`. Returns kNotFound when the file cannot be read, kIoError when
    /// it is not valid JSON, kUnsupported when `version` != 1 or `bundle.irMajor` !=
    /// kIrFormatMajor, and kInvalidArgument when `bundle.id` or `uis` is missing. `decode` is the
    /// image decoder handed to the FileAssetResolver behind assets(); nullptr means images cannot
    /// be decoded. `error`, when non-null, receives a human-readable message (it is cleared
    /// first); the IR mismatch message names both major versions.
    ///
    /// T-500 (§22.5 `.xtb`, the minimal form): `dir` may also name an `.xtb` file (a store-mode
    /// zip of the directory form, `xtframe-pack --zip`). It is unpacked into
    /// `<system temp>/xtframe-bundle/<content hash>/` (xt/bundle/zip_archive.h, with its entry
    /// checks and kMaxXtbBytes / kMaxXtbEntries) and read from there like a directory: not read
    /// in place, and not mapped (22 §22.2's mmap reading of a stored zip is not done yet). An
    /// unpacked copy whose `manifest.json` exists is reused; dir() then names that directory, and
    /// reload() re-reads the unpacked manifest, so a rewritten `.xtb` needs a new open(). A bad
    /// archive is kInvalidArgument with the entry or check in `error`.
    static Result<std::unique_ptr<Bundle>> open(std::string_view dir, ImageDecodeFn decode = nullptr,
                                                std::string* error = nullptr);

    /// T-500: the limits an `.xtb` is unpacked under (the sum of the entry sizes, the entry count).
    static constexpr uint64_t kMaxXtbBytes = 1024ull * 1024ull * 1024ull;
    static constexpr size_t kMaxXtbEntries = 65536;

    /// The bundle's stable id (`bundle.id`).
    std::string_view id() const;
    /// The bundle's human-readable name (`bundle.name`).
    std::string_view name() const;
    /// The directory passed to open(); for an `.xtb`, the directory it was unpacked into.
    std::string_view dir() const;

    /// The UIs, in manifest order.
    std::span<const UiInfo> uis() const;
    /// The UI with this id, or nullptr. Invalidated by reload().
    const UiInfo* ui(std::string_view id) const;

    /// T-170 (§22.10 `tokens`): the bundle's base design tokens (its shared stylesheets'), by name.
    std::span<const TokenDef> tokens() const;
    /// T-170 (§22.10 `themes[]`): the bundle's themes, in manifest order.
    std::span<const ThemeInfo> themes() const;
    /// T-179 (§22.10 `strings`): the bundle's languages, in manifest order.
    std::span<const StringsInfo> strings() const;
    /// T-179 (§22.10 `fonts.fallback`): the per-language fallback chains, in manifest order.
    /// T-555 (T-527c): a chain under the empty language key `""` is kept too; it is the default
    /// chain addBundleFonts() orders the bundle's fonts by.
    std::span<const FontFallbackInfo> fontFallbacks() const;

    /// T-183 (§22.10 `project`): the source UI Project's identity; nullptr when the manifest has
    /// none (packed from a `bundle.xtproj` or a lone `.xt`).
    const ProjectInfo* project() const;
    /// T-207 (§22.10 `spec`): the safe area and logical height of the source UI Project; nullptr
    /// when the manifest has none.
    const BundleSpec* spec() const;
    /// T-183 (§22.10 `motionSpec`): the motion spec summary (Docs/design/29-ai-endpoint.md §29.6)
    /// as compact JSON object text; empty when the manifest has none.
    std::string_view motionSpec() const;
    /// T-193 (§22.10 `flows.global`): the global flow graph (`bundle.xtflow`), relative to the
    /// bundle root; empty when the manifest has none. Load it with
    /// xt::flow::BundleContext::loadGlobalFlow().
    std::string_view globalFlow() const;
    /// T-200 (§22.10 `symbols[]`): the contract summary of every symbol of the source project (its
    /// symbol library, then the UIs' own definitions); empty when the manifest has none.
    std::span<const SymbolContract> symbolContracts() const;

    /// The asset resolver rooted at dir(); names are the manifest's `assets/<hash>.<ext>`.
    /// T-199: open() registers every atlas cell and manifest nine-slice with it
    /// (AssetResolver::setImageMeta), so `atlas:<name>#<asset>` names resolve to their page.
    AssetResolver& assets();
    /// T-199 (§22.10): the manifest's `assets[]` rows.
    std::span<const AssetInfo> assetTable() const;
    /// T-199 (§22.10 `atlas[]`): the atlas pages, in manifest order; empty when none.
    std::span<const AtlasInfo> atlases() const;
    /// T-204 (§22.10 `glyphs[device]`): every button glyph, sorted by device then key.
    std::span<const GlyphInfo> glyphs() const;
    /// T-204: the packed asset of `glyphs[device][key]`; empty when the manifest has none.
    std::string_view glyphAsset(std::string_view device, std::string_view key) const;

    /// Re-reads the manifest and returns the ids of UIs whose fileHash changed plus UIs that are
    /// new, in manifest order. T-262a: the asset table, the atlases and the image meta registered
    /// with assets() are replaced as open() sets them, and the decoded atlas pages are forgotten. An
    /// unreadable, malformed or version-incompatible manifest leaves this Bundle untouched and returns an
    /// empty vector.
    ///
    /// Limitation (T-262b): an asset id resolved before reload() keeps its old decoded image, since
    /// the resolver caches by id. To see a new image the host reloads the UI (`reloadUi`) or its
    /// Document and resets the resolver's id cache. This note goes once the resolver keys by path
    /// (T-263b).
    std::vector<std::string> reload();

private:
    friend Result<uint32_t> addBundleFonts(const Bundle& bundle, std::string_view uiId, TextShaper& shaper,
                                           std::vector<std::string>* warnings);

    /// T-262a: takes over `manifest`'s content and registers the image meta of its assets and atlas
    /// cells with assets() (manifest slices, `@2x` variants, `atlas:<page>#<asset>` cells); shared
    /// by open() and reload().
    void commit(detail::BundleManifest&& manifest);

    std::string m_dir;
    std::string m_id;
    std::string m_name;
    std::vector<UiInfo> m_uis;
    std::vector<TokenDef> m_tokens;
    std::vector<ThemeInfo> m_themes;
    std::vector<StringsInfo> m_strings;
    std::vector<FontFallbackInfo> m_fontFallbacks;
    bool m_hasProject = false;
    ProjectInfo m_project;
    bool m_hasSpec = false;  // T-207.
    BundleSpec m_spec;
    std::string m_motionSpec;
    std::string m_globalFlow;
    // T-199.
    std::vector<AssetInfo> m_assetTable;
    std::vector<AtlasInfo> m_atlases;
    std::vector<SymbolContract> m_symbols;  // T-200.
    std::vector<GlyphInfo> m_glyphs;        // T-204.
    std::unique_ptr<FileAssetResolver> m_assets;
    /// T-555: the font assets addBundleFonts() registered, per shaper (dedup across loads and
    /// reload()). `fontCount` is the shaper's TextShaper::fonts() count right after the last
    /// registration: a shaper reporting fewer is a new one at a reused address and starts over.
    struct RegisteredFonts {
        const TextShaper* shaper = nullptr;
        size_t fontCount = 0;
        std::vector<std::string> files;
    };
    mutable std::vector<RegisteredFonts> m_registeredFonts;
};

/// T-204 (Docs/design/13-api.md §13.8.8, 22 §22.10): the default IGlyphProvider of a bundle. It
/// looks the action up in the manifest's `glyphs[device]` first -- InputKind::Mouse tries
/// "mouse", then "keyboard"; Gamepad "gamepad"; Touch "touch" -- and asks `fallback` (when given)
/// only when the manifest has no entry. The asset still has to be in the document's IR asset table
/// (the UI references the image, e.g. with an `<img>`) for Document to draw it. `bundle` (and
/// `fallback`) must outlive the provider; after Bundle::reload() it sees the new table.
class BundleGlyphProvider : public IGlyphProvider {
public:
    explicit BundleGlyphProvider(const Bundle& bundle, IGlyphProvider* fallback = nullptr);
    AssetRef resolve(std::string_view action, InputKind device) override;

private:
    const Bundle& m_bundle;
    IGlyphProvider* m_fallback = nullptr;
};

/// T-240 (Docs/design/23-host-integration.md §23.11.1): reads the whole file `path` into `out`;
/// false when it cannot be read. `user` is the pointer given to setBundleFileReader().
using BundleFileReader = bool (*)(const std::string& path, std::string& out, void* user);

/// T-240: routes every file xt_bundle reads at run time -- the manifest (Bundle::open/reload),
/// loadUi/reloadUi's `.xtir`, loadThemes' theme files, the strings tables -- through `reader`
/// instead of the file system, e.g. an Android host reading its APK's `assets/` through
/// SDL_RWops. Process-wide; set it before opening a bundle. nullptr (the default) restores plain
/// file reads. Bundle::assets() still reads the file system; such a host gives its Context a
/// resolver of its own (and copies the manifest's image metadata onto it).
void setBundleFileReader(BundleFileReader reader, void* user = nullptr);

/// Loads the UI `uiId` of `bundle` into `ctx` (`<bundle dir>/<UiInfo::file>`, owned copy).
/// Returns kNotFound when the bundle has no such UI (or its manifest entry has no `file`) and
/// kIoError when the `.xtir` cannot be read.
///
/// `ctx` must have been built with `bundle`'s own resolver -- `ContextConfig.assets =
/// &bundle.assets()` -- otherwise the UI's `assets/<hash>.<ext>` names resolve against something
/// else; this cannot be checked from here, so it is the host's responsibility.
///
/// T-408 (43 §43.6, 40 §40.6.1 contract lock): a non-empty `expectContract` (the `kContractHash` /
/// `Ports.ContractHash` of the `--gen-ports` file the host was compiled against) must equal the
/// manifest's `uis[].contract`; otherwise nothing is loaded and kIncompatible is returned (also
/// when the manifest has no contract). Empty: no check.
Result<DocumentHandle> loadUi(Context& ctx, Bundle& bundle, std::string_view uiId,
                              std::string_view expectContract = {});

/// Re-reads `uiId`'s `.xtir` and hands it to Context::reloadDocument(), so `handle` keeps its
/// identity while its content is replaced (hot reload, §22.5). Same error mapping as loadUi(),
/// plus Context::reloadDocument()'s kInvalidArgument for a handle that is not loaded. On any
/// failure the Document behind `handle` is left as it was.
///
/// As with Context::reloadDocument(), every `Document*`/`ElementRef` the host holds for `handle`
/// is invalidated by a successful call.
Result<DocumentHandle> reloadUi(Context& ctx, Bundle& bundle, DocumentHandle handle, std::string_view uiId);

/// T-170 (Docs/design/13-api.md §13.8.2, 22-bundle.md §22.10): reads every theme file the
/// manifest lists and registers it with `ctx.defineTheme(name, ...)` -- only its `--name: value`
/// declarations count -- so the host can then `ctx.setTheme(name)`. Returns kIoError when a theme
/// file cannot be read (the themes before it stay registered), else kOk. Calling it again after
/// reload() picks up changed themes without touching any loaded Document beyond re-resolving the
/// active theme's users.
Status loadThemes(Context& ctx, const Bundle& bundle);

/// T-555 (Docs/design/22-bundle.md §22.10.3, 50 §50.8 T-507): registers the `kind=font` assets
/// of UI `uiId`'s manifest entry (`uis[].assets`) with `shaper` through
/// TextShaper::addFontMemory(), reading them like every other bundle file (the T-240 reader hook;
/// an `.xtb` is read from its unpacked directory). Fonts whose family is in the default
/// `fonts.fallback` chain (language `""`, T-527c) are registered first, in chain order, then the
/// rest in manifest order -- addFontMemory() appends each font to the shaper's fallback list, so
/// the chain becomes its fallback order. An asset already registered with this shaper by an
/// earlier call is skipped (the bundle remembers it, also across reload()), so loading or
/// reloading a UI again registers nothing twice. Call it before the shaper's system fonts are
/// added so a bundled family wins over a system font of the same name; system fonts are not
/// touched. loadUi() and reloadUi() call it with their Context's shaper (Context::textShaper()).
/// Returns the number of fonts newly registered; kNotFound when the bundle has no such UI. A font
/// that cannot be read or that the shaper rejects is skipped, with one line appended to `warnings`
/// (when given); it is tried again by the next call.
Result<uint32_t> addBundleFonts(const Bundle& bundle, std::string_view uiId, TextShaper& shaper,
                                std::vector<std::string>* warnings = nullptr);

/// T-170: the `--name: value` declarations of one theme/token `.xts` text, in source order (a
/// repeated name keeps both; defineTheme() keeps the later). Everything else in the text is
/// ignored.
std::vector<TokenDef> parseThemeTokens(std::string_view xtsText);

/// One `key,text` row of a `strings/<lang>.csv` table (T-179).
struct StringsRow {
    /// The localization key, without `@`.
    std::string key;
    /// The UTF-8 text.
    std::string text;
    /// 1-based line the row starts on.
    int line = 0;
};

/// T-179 (§22.10): parses a `strings/<lang>.csv` table -- UTF-8 (a leading BOM is skipped), RFC
/// 4180 quoting (`"` around a field that holds `,`, `"` or a line break; `""` inside is one `"`),
/// CRLF or LF records, exactly two fields per record. A first record that reads exactly
/// `key,text` is a header and skipped, as are empty lines. Returns false (rows parsed so far are
/// kept in `out`) at the first malformed record; `error` then names its line.
bool parseStringsCsv(std::string_view text, std::vector<StringsRow>& out, std::string* error = nullptr);

/// T-179 (13.8.2 `ILocalizationProvider`): serves a bundle's `strings/<lang>.csv` tables. A key
/// missing from `lang`'s table (or empty there) is looked up again under each shorter BCP 47
/// prefix of `lang` (`zh-TW` -> `zh`); still missing = "" (the Document then shows the key).
class BundleLocalizationProvider : public ILocalizationProvider {
public:
    /// Reads every table `bundle.strings()` lists, replacing any table of the same language.
    /// kIoError when a file cannot be read, kInvalidArgument when one is malformed (`error` names
    /// the file and line); the tables before it stay loaded.
    Status load(const Bundle& bundle, std::string* error = nullptr);
    /// Reads one csv file as `lang`'s table (replacing it). Same errors as load().
    Status loadFile(std::string_view lang, std::string_view path, std::string* error = nullptr);
    /// Parses `csvText` as `lang`'s table (replacing it). kInvalidArgument when malformed.
    Status addCsv(std::string_view lang, std::string_view csvText, std::string* error = nullptr);
    /// Sets one entry of `lang`'s table.
    void set(std::string_view lang, std::string_view key, std::string_view text);
    /// The languages with a table, sorted.
    std::vector<std::string> languages() const;

    /// See the class comment.
    std::string_view resolve(std::string_view key, std::string_view lang) override;

private:
    std::map<std::string, std::map<std::string, std::string, std::less<>>, std::less<>> m_tables;
};

/// T-179 (§22.10, `xtframe-browse`): how much of one language the bundle's UIs are covered by.
struct LanguageCoverage {
    /// BCP 47 language tag.
    std::string lang;
    /// Distinct `@key`s the bundle's UIs use that this language's table translates (non-empty).
    uint32_t translated = 0;
    /// Distinct `@key`s the bundle's UIs use.
    uint32_t total = 0;
};

/// T-179: every `strings()` language's coverage, in manifest order; the keys come from each UI's
/// `.xtir` `STRS` table. A UI or table that cannot be read counts as having no keys/rows.
std::vector<LanguageCoverage> languageCoverage(const Bundle& bundle);

/// T-179: the distinct localization keys (without `@`) one compiled `.xtir` uses, sorted; empty
/// when it has none or cannot be opened.
std::vector<std::string> localizationKeys(IrBlob blob);

}  // namespace xt::bundle
