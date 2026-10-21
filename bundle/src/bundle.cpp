#include "xt/bundle/bundle.h"

#include <json.hpp>
#include <string>
#include <utility>

#include "bundle_json.h"
#include "bundle_paths.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/parse_xts.h"
#include "xt/compile/utf8_path.h"
#include "xt/ir_pod.h"

/// Bundle: reading a published bundle's `manifest.json`. See docs/design/22-bundle.md §22.3 and
/// §22.5, docs/tasks/T-90.md, docs/tasks/T-93.md (loadUi/reloadUi).
///
/// Parsing is non-throwing, as in project_file.cpp. A parsed manifest is only committed to the
/// Bundle once every check passed, so a failed reload() leaves the previous state intact.

namespace xt::bundle {

using detail::getBool;
using detail::getString;
using detail::getStringArray;
using detail::getUint64;
using detail::getVec2;
using detail::joinPath;
using nlohmann::json;

namespace {

constexpr const char* kManifestFileName = "manifest.json";

/// Sets `*error` when the caller asked for a message.
void setErrorBundle(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

}  // namespace

namespace detail {

/// The parsed manifest, before it is committed to a Bundle (Bundle::commit).
struct BundleManifest {
    std::string id;
    std::string name;
    std::vector<UiInfo> uis;
    std::vector<TokenDef> tokens;
    std::vector<ThemeInfo> themes;
    std::vector<StringsInfo> strings;
    std::vector<FontFallbackInfo> fontFallbacks;
    bool hasProject = false;
    ProjectInfo project;
    bool hasSpec = false;  // T-207 `spec`.
    BundleSpec spec;
    std::string motionSpec;
    std::string globalFlow;  // T-193 `flows.global`.
    // T-199.
    std::vector<AssetInfo> assetTable;
    std::vector<AtlasInfo> atlases;
    std::vector<SymbolContract> symbols;  // T-200 `symbols[]`.
    std::vector<GlyphInfo> glyphs;        // T-204 `glyphs[device]`.
};

}  // namespace detail

namespace {

using Manifest = detail::BundleManifest;

/// Reads one `ports.in`/`ports.out` array into `out`.
void readPorts(const json& portsObj, const char* key, bool isInput, std::vector<PortInfo>& out) {
    const auto it = portsObj.find(key);
    if (it == portsObj.end() || !it->is_array()) {
        return;
    }
    for (const json& entry : *it) {
        if (!entry.is_object()) {
            continue;
        }
        PortInfo port;
        port.name = getString(entry, "name");
        port.type = getString(entry, "type");
        port.isInput = isInput;
        const auto paramsIt = entry.find("params");
        if (paramsIt != entry.end() && paramsIt->is_object()) {
            for (auto param = paramsIt->begin(); param != paramsIt->end(); ++param) {
                // Params are host-binding hints; anything non-string is stringified so a number
                // like {"index": 2} still reaches the host.
                port.params[param.key()] =
                    param.value().is_string() ? param.value().get<std::string>() : param.value().dump();
            }
        }
        out.push_back(std::move(port));
    }
}

/// T-262b (F15): the first file path in `manifest` that would leave the bundle directory once
/// joined to it (detail::isSafeBundlePath()), or nullptr when every one stays inside. Empty
/// optional fields are skipped. `assets[].sources` are project paths, never opened from the bundle.
const std::string* firstUnsafePath(const Manifest& manifest) {
    const std::string* found = nullptr;
    auto check = [&found](const std::string& path) {
        if (found == nullptr && !path.empty() && !detail::isSafeBundlePath(path)) {
            found = &path;
        }
    };
    for (const UiInfo& ui : manifest.uis) {
        check(ui.file);
        check(ui.flow);
        check(ui.thumb);
        for (const std::string& asset : ui.assets) {
            check(asset);
        }
    }
    for (const ThemeInfo& theme : manifest.themes) {
        check(theme.file);
    }
    for (const StringsInfo& strings : manifest.strings) {
        check(strings.file);
    }
    check(manifest.globalFlow);
    for (const AssetInfo& asset : manifest.assetTable) {
        check(asset.file);
        check(asset.variant2x);
        check(asset.variant3x);  // T-345
    }
    for (const AtlasInfo& atlas : manifest.atlases) {
        check(atlas.file);
        check(atlas.file2x);
    }
    for (const GlyphInfo& glyph : manifest.glyphs) {
        check(glyph.asset);
    }
    return found;
}

/// Parses `<dir>/manifest.json` into `out`. See Bundle::open for the status mapping.
Status readManifestBundle(std::string_view dir, Manifest& out, std::string* error) {
    const std::string path = joinPath(dir, kManifestFileName);

    std::string text;
    if (!detail::readFile(path, text)) {
        setErrorBundle(error, "cannot read '" + path + "'");
        return Status::kNotFound;
    }

    std::string depthError;  // T-262c
    const json root = detail::parseBounded(text, detail::kMaxJsonDepth, depthError);
    if (!depthError.empty()) {
        setErrorBundle(error, "manifest " + depthError);
        return Status::kInvalidArgument;
    }
    if (root.is_discarded() || !root.is_object()) {
        setErrorBundle(error, "'" + path + "' is not a JSON object");
        return Status::kIoError;
    }

    const auto versionIt = root.find("version");
    const int version =
        (versionIt != root.end() && versionIt->is_number_integer()) ? versionIt->get<int>() : 0;
    if (version != 1) {
        setErrorBundle(error, "'" + path + "' has version " + std::to_string(version) + ", expected 1");
        return Status::kUnsupported;
    }

    const auto bundleIt = root.find("bundle");
    if (bundleIt == root.end() || !bundleIt->is_object()) {
        setErrorBundle(error, "'" + path + "' has no 'bundle' object");
        return Status::kInvalidArgument;
    }
    const auto irMajorIt = bundleIt->find("irMajor");
    const int irMajor =
        (irMajorIt != bundleIt->end() && irMajorIt->is_number_integer()) ? irMajorIt->get<int>() : 0;
    if (irMajor != static_cast<int>(kIrFormatMajor)) {
        setErrorBundle(error, "bundle IR major version " + std::to_string(irMajor) + " does not match " +
                                  std::to_string(static_cast<int>(kIrFormatMajor)));
        return Status::kUnsupported;
    }

    Manifest manifest;
    manifest.id = getString(*bundleIt, "id");
    manifest.name = getString(*bundleIt, "name");
    if (manifest.id.empty()) {
        setErrorBundle(error, "'" + path + "' has no 'bundle.id'");
        return Status::kInvalidArgument;
    }

    const auto uisIt = root.find("uis");
    if (uisIt == root.end() || !uisIt->is_array()) {
        setErrorBundle(error, "'" + path + "' has no 'uis' array");
        return Status::kInvalidArgument;
    }
    for (const json& entry : *uisIt) {
        if (!entry.is_object()) {
            continue;
        }
        UiInfo ui;
        ui.id = getString(entry, "id");
        if (ui.id.empty()) {
            setErrorBundle(error, "'" + path + "' has a UI without an 'id'");
            return Status::kInvalidArgument;
        }
        ui.name = getString(entry, "name");
        ui.file = getString(entry, "file");
        ui.flow = getString(entry, "flow");
        ui.thumb = getString(entry, "thumb");
        ui.size = getVec2(entry, "size", Vec2{0, 0});
        ui.tags = getStringArray(entry, "tags");
        ui.fit = getString(entry, "fit");
        if (ui.fit.empty()) {
            ui.fit = "native";
        }
        const auto requiresIt = entry.find("requires");
        if (requiresIt != entry.end() && requiresIt->is_object()) {
            ui.requiresEffects = getBool(*requiresIt, "effects", false);
            ui.requiresSpace3d = getBool(*requiresIt, "space3d", false);
            ui.requiresText = getBool(*requiresIt, "text", false);
        }
        ui.assets = getStringArray(entry, "assets");
        const auto portsIt = entry.find("ports");
        if (portsIt != entry.end() && portsIt->is_object()) {
            readPorts(*portsIt, "in", true, ui.ports);
            readPorts(*portsIt, "out", false, ui.ports);
        }
        ui.fileHash = getUint64(entry, "fileHash");
        manifest.uis.push_back(std::move(ui));
    }
    manifest.tokens = detail::getTokens(root);
    for (detail::ThemeEntry& theme : detail::getThemes(root)) {
        manifest.themes.push_back(ThemeInfo{std::move(theme.name), std::move(theme.file)});
    }
    for (auto& [lang, file] : detail::getStringsMap(root)) {
        manifest.strings.push_back(StringsInfo{lang, file});
    }
    for (auto& [lang, families] : detail::getFontFallbacks(root)) {
        manifest.fontFallbacks.push_back(FontFallbackInfo{lang, std::move(families)});
    }
    // T-183 (§22.10).
    const auto projectIt = root.find("project");
    if (projectIt != root.end() && projectIt->is_object()) {
        manifest.hasProject = true;
        manifest.project.name = getString(*projectIt, "name");
        manifest.project.game = getString(*projectIt, "game");
        manifest.project.commit = getString(*projectIt, "commit");          // T-314
        manifest.project.checkpoint = getString(*projectIt, "checkpoint");  // T-314
        const auto versionIt2 = projectIt->find("version");
        if (versionIt2 != projectIt->end() && versionIt2->is_number_integer()) {
            manifest.project.version = versionIt2->get<int>();
        }
    }
    const auto motionIt = root.find("motionSpec");
    if (motionIt != root.end() && motionIt->is_object()) {
        manifest.motionSpec = motionIt->dump();
    }
    const auto flowsIt = root.find("flows");
    if (flowsIt != root.end() && flowsIt->is_object()) {
        manifest.globalFlow = getString(*flowsIt, "global");
    }
    // T-199 (§22.10): the `assets[]` rows and `atlas[]` pages.
    const auto assetsIt = root.find("assets");
    if (assetsIt != root.end() && assetsIt->is_array()) {
        for (const json& entry : *assetsIt) {
            if (!entry.is_object()) {
                continue;
            }
            AssetInfo info;
            info.file = getString(entry, "file");
            if (info.file.empty()) {
                continue;
            }
            info.kind = getString(entry, "kind");
            info.bytes = getUint64(entry, "bytes");
            info.sources = getStringArray(entry, "sources");
            info.format = getString(entry, "format");
            const auto variantsIt = entry.find("variants");
            if (variantsIt != entry.end() && variantsIt->is_object()) {
                info.variant2x = getString(*variantsIt, "2x");
                info.variant3x = getString(*variantsIt, "3x");  // T-345
            }
            info.atlases = getStringArray(entry, "atlas");
            const auto measuredIt = entry.find("manifest");
            if (measuredIt != entry.end() && measuredIt->is_object()) {
                const auto slice = measuredIt->find("slice");
                if (slice != measuredIt->end() && slice->is_array() && slice->size() == 4) {
                    info.hasSlice = true;
                    for (size_t i = 0; i < 4; ++i) {
                        info.slice[i] =
                            (*slice)[i].is_number() ? static_cast<int>((*slice)[i].get<double>()) : 0;
                    }
                }
            }
            manifest.assetTable.push_back(std::move(info));
        }
    }
    for (detail::AtlasJson& page : detail::getAtlases(root)) {
        AtlasInfo atlas;
        atlas.name = std::move(page.name);
        atlas.file = std::move(page.file);
        atlas.file2x = std::move(page.file2x);
        atlas.format = std::move(page.format);
        atlas.width = page.w;
        atlas.height = page.h;
        for (detail::AtlasEntryJson& e : page.entries) {
            atlas.entries.push_back(AtlasEntryInfo{std::move(e.asset), e.x, e.y, e.w, e.h});
        }
        manifest.atlases.push_back(std::move(atlas));
    }
    manifest.symbols = detail::getSymbolContracts(root);      // T-200.
    manifest.glyphs = detail::getGlyphs(root);                // T-204.
    manifest.hasSpec = detail::getSpec(root, manifest.spec);  // T-207.
    if (const std::string* escaping = firstUnsafePath(manifest)) {
        setErrorBundle(error, "manifest path escapes the bundle: " + *escaping);
        return Status::kInvalidArgument;
    }
    out = std::move(manifest);
    return Status::kOk;
}

}  // namespace

Bundle::Bundle() = default;
Bundle::~Bundle() = default;

Result<std::unique_ptr<Bundle>> Bundle::open(std::string_view dir, ImageDecodeFn decode, std::string* error) {
    setErrorBundle(error, std::string());
    Manifest manifest;
    const Status status = readManifestBundle(dir, manifest, error);
    if (status != Status::kOk) {
        return Result<std::unique_ptr<Bundle>>::error(status);
    }

    auto bundle = std::make_unique<Bundle>();
    bundle->m_dir = std::string(dir);
    bundle->m_assets = std::make_unique<FileAssetResolver>(bundle->m_dir, decode);
    bundle->m_assets->setFileStatusFn(&fileStatusUtf8);  // T-263e: revalidate() works out of the box.
    bundle->commit(std::move(manifest));
    return Result<std::unique_ptr<Bundle>>::ok(std::move(bundle));
}

void Bundle::commit(detail::BundleManifest&& manifest) {
    m_id = std::move(manifest.id);
    m_name = std::move(manifest.name);
    m_uis = std::move(manifest.uis);
    m_tokens = std::move(manifest.tokens);
    m_themes = std::move(manifest.themes);
    m_strings = std::move(manifest.strings);
    m_fontFallbacks = std::move(manifest.fontFallbacks);
    m_hasProject = manifest.hasProject;
    m_project = std::move(manifest.project);
    m_hasSpec = manifest.hasSpec;
    m_spec = manifest.spec;
    m_motionSpec = std::move(manifest.motionSpec);
    m_globalFlow = std::move(manifest.globalFlow);
    m_assetTable = std::move(manifest.assetTable);
    m_atlases = std::move(manifest.atlases);
    m_symbols = std::move(manifest.symbols);
    m_glyphs = std::move(manifest.glyphs);
    if (m_assets == nullptr) {
        return;  // A default-constructed Bundle has no resolver to register with.
    }
    // T-199: manifest slices and `@2x` variants of plain assets, then every atlas cell (named as
    // the IR names it, `atlas:<page>#<asset>`, carrying the asset's slice too).
    for (const AssetInfo& info : m_assetTable) {
        if (!info.hasSlice && info.variant2x.empty() && info.variant3x.empty()) {
            continue;
        }
        AssetResolver::ImageMeta meta;
        meta.hasSlice = info.hasSlice;
        for (int i = 0; i < 4; ++i) {
            meta.slice[i] = static_cast<float>(info.slice[i]);
        }
        meta.variant2x = info.variant2x;
        meta.variant3x = info.variant3x;  // T-345
        m_assets->setImageMeta(info.file, std::move(meta));
    }
    for (const AtlasInfo& atlas : m_atlases) {
        for (const AtlasEntryInfo& entry : atlas.entries) {
            AssetResolver::ImageMeta meta;
            meta.inAtlas = true;
            meta.atlasFile = atlas.file;
            meta.atlasFile2x = atlas.file2x;
            meta.x = entry.x;
            meta.y = entry.y;
            meta.w = entry.w;
            meta.h = entry.h;
            meta.atlasW = atlas.width;
            meta.atlasH = atlas.height;
            if (atlas.width > 0 && atlas.height > 0) {
                meta.subUv = Rect{static_cast<float>(entry.x) / static_cast<float>(atlas.width),
                                  static_cast<float>(entry.y) / static_cast<float>(atlas.height),
                                  static_cast<float>(entry.w) / static_cast<float>(atlas.width),
                                  static_cast<float>(entry.h) / static_cast<float>(atlas.height)};
            }
            for (const AssetInfo& info : m_assetTable) {
                if (info.file == entry.asset && info.hasSlice) {
                    meta.hasSlice = true;
                    for (int i = 0; i < 4; ++i) {
                        meta.slice[i] = static_cast<float>(info.slice[i]);
                    }
                }
            }
            m_assets->setImageMeta(std::string(kAtlasAssetScheme) + atlas.name + "#" + entry.asset,
                                   std::move(meta));
        }
    }
}

std::string_view Bundle::id() const { return m_id; }
std::string_view Bundle::name() const { return m_name; }
std::string_view Bundle::dir() const { return m_dir; }

std::span<const UiInfo> Bundle::uis() const { return std::span<const UiInfo>(m_uis); }

std::span<const TokenDef> Bundle::tokens() const { return std::span<const TokenDef>(m_tokens); }

std::span<const ThemeInfo> Bundle::themes() const { return std::span<const ThemeInfo>(m_themes); }

std::span<const StringsInfo> Bundle::strings() const { return std::span<const StringsInfo>(m_strings); }

std::span<const FontFallbackInfo> Bundle::fontFallbacks() const {
    return std::span<const FontFallbackInfo>(m_fontFallbacks);
}

const ProjectInfo* Bundle::project() const { return m_hasProject ? &m_project : nullptr; }

const BundleSpec* Bundle::spec() const { return m_hasSpec ? &m_spec : nullptr; }

std::string_view Bundle::motionSpec() const { return m_motionSpec; }

std::string_view Bundle::globalFlow() const { return m_globalFlow; }

std::span<const SymbolContract> Bundle::symbolContracts() const { return m_symbols; }

const UiInfo* Bundle::ui(std::string_view uiId) const {
    for (const UiInfo& info : m_uis) {
        if (info.id == uiId) {
            return &info;
        }
    }
    return nullptr;
}

AssetResolver& Bundle::assets() { return *m_assets; }

std::span<const AssetInfo> Bundle::assetTable() const { return std::span<const AssetInfo>(m_assetTable); }

std::span<const AtlasInfo> Bundle::atlases() const { return std::span<const AtlasInfo>(m_atlases); }

std::span<const GlyphInfo> Bundle::glyphs() const { return std::span<const GlyphInfo>(m_glyphs); }

std::string_view Bundle::glyphAsset(std::string_view device, std::string_view key) const {
    for (const GlyphInfo& glyph : m_glyphs) {
        if (glyph.device == device && glyph.key == key) {
            return glyph.asset;
        }
    }
    return std::string_view();
}

BundleGlyphProvider::BundleGlyphProvider(const Bundle& bundle, IGlyphProvider* fallback)
    : m_bundle(bundle), m_fallback(fallback) {}

AssetRef BundleGlyphProvider::resolve(std::string_view action, InputKind device) {
    std::string_view asset;
    switch (device) {
        case InputKind::Mouse:
            asset = m_bundle.glyphAsset("mouse", action);
            if (asset.empty()) {
                asset = m_bundle.glyphAsset("keyboard", action);
            }
            break;
        case InputKind::Gamepad:
            asset = m_bundle.glyphAsset("gamepad", action);
            break;
        case InputKind::Touch:
            asset = m_bundle.glyphAsset("touch", action);
            break;
    }
    if (!asset.empty()) {
        return AssetRef{std::string(asset)};
    }
    return m_fallback != nullptr ? m_fallback->resolve(action, device) : AssetRef{};
}

std::vector<std::string> Bundle::reload() {
    Manifest manifest;
    if (readManifestBundle(m_dir, manifest, nullptr) != Status::kOk) {
        return {};
    }

    std::vector<std::string> changed;
    for (const UiInfo& fresh : manifest.uis) {
        const UiInfo* old = ui(fresh.id);
        if (old == nullptr || old->fileHash != fresh.fileHash) {
            changed.push_back(fresh.id);
        }
    }

    // T-262a: the previous build's image meta goes (a re-packed image has a new content name, an
    // atlas cell may have moved), and so do the decoded atlas pages, whose file names a re-pack
    // keeps; commit() then registers the new manifest's meta.
    if (m_assets != nullptr) {
        for (const AssetInfo& info : m_assetTable) {
            m_assets->clearImageMeta(info.file);
        }
        for (const std::vector<AtlasInfo>* pages : {&m_atlases, &manifest.atlases}) {
            for (const AtlasInfo& atlas : *pages) {
                for (const AtlasEntryInfo& entry : atlas.entries) {
                    m_assets->clearImageMeta(std::string(kAtlasAssetScheme) + atlas.name + "#" + entry.asset);
                }
                m_assets->forgetFile(atlas.file);
                if (!atlas.file2x.empty()) {
                    m_assets->forgetFile(atlas.file2x);
                }
            }
        }
    }
    commit(std::move(manifest));
    return changed;
}

namespace {

/// `<bundle dir>/<ui file>` for `uiId`, or an empty string when the bundle has no such UI (or its
/// entry carries no `file`).
std::string uiFilePath(const Bundle& bundle, std::string_view uiId) {
    const UiInfo* info = bundle.ui(uiId);
    if (info == nullptr || info->file.empty()) {
        return std::string();
    }
    return joinPath(bundle.dir(), info->file);
}

}  // namespace

Result<DocumentHandle> loadUi(Context& ctx, Bundle& bundle, std::string_view uiId) {
    const std::string path = uiFilePath(bundle, uiId);
    if (path.empty()) {
        return Result<DocumentHandle>::error(Status::kNotFound);
    }
    // Through detail::readFile so the T-240 file reader hook (APK assets) applies, as in reloadUi().
    std::string bytes;
    if (!detail::readFile(path, bytes)) {
        return Result<DocumentHandle>::error(Status::kIoError);
    }
    const IrBlob blob{reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()};
    return ctx.loadDocument(blob, /*takeOwnership=*/true);
}

std::vector<TokenDef> parseThemeTokens(std::string_view xtsText) {
    compile::DiagnosticSink sink;
    compile::RuleSet rules;
    compile::parseXts(xtsText, "<theme>", 1, sink, rules);
    std::vector<TokenDef> out;
    out.reserve(rules.tokens.size());
    for (compile::TokenDecl& token : rules.tokens) {
        out.push_back(TokenDef{std::move(token.name), std::move(token.value)});
    }
    return out;
}

Status loadThemes(Context& ctx, const Bundle& bundle) {
    for (const ThemeInfo& theme : bundle.themes()) {
        std::string text;
        if (!detail::readFile(joinPath(bundle.dir(), theme.file), text)) {
            return Status::kIoError;
        }
        const std::vector<TokenDef> tokens = parseThemeTokens(text);
        const Status status = ctx.defineTheme(theme.name, tokens);
        if (status != Status::kOk) {
            return status;
        }
    }
    return Status::kOk;
}

Result<DocumentHandle> reloadUi(Context& ctx, Bundle& bundle, DocumentHandle handle, std::string_view uiId) {
    const std::string path = uiFilePath(bundle, uiId);
    if (path.empty()) {
        return Result<DocumentHandle>::error(Status::kNotFound);
    }
    std::string bytes;
    if (!detail::readFile(path, bytes)) {
        return Result<DocumentHandle>::error(Status::kIoError);
    }
    const IrBlob blob{reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()};
    // takeOwnership: the new Document copies the blob, so `bytes` can die with this call.
    // T-263e: the replaced Document's IR is released (AssetResolver::releaseSource) by the Context
    // on its own resolver, which §22.5 requires to be bundle.assets(); the Bundle keeps no IrView
    // of its own to release.
    return ctx.reloadDocument(handle, blob, /*takeOwnership=*/true);
}

}  // namespace xt::bundle
