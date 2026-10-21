#include "xt/bundle/pack.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "atlas_pack.h"
#include "bundle_json.h"
#include "font_subset.h"
#include "manifest_writer.h"
#include "symbol_contracts.h"
#include "texture_compress.h"
#include "xt/asset_resolver.h"
#include "xt/bundle/migrations.h"
#include "xt/bundle/ui_project.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/compile/parse_xt.h"
#include "xt/compile/parse_xts.h"
#include "xt/compile/utf8_path.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/flow/flow_graph.h"
#include "xt/flow/flow_ports.h"
#include "xt/ir.h"
#include "xt/ir_pod.h"
#include "xt/node_arena.h"
#include "xt/num_guard.h"
#include "xt/sw/sw_png.h"
#include "xt/sw/sw_renderer.h"
#include "xt/text_shaper.h"
#include "xt/util/hash.h"
#include "xt/util/time.h"
#include "xt/version.h"
#include "yaml_lite.h"
#ifdef XT_BUNDLE_HAS_TEXT
#include "xt/text/ft_text_shaper.h"
#endif

/// pack(): see xt/bundle/pack.h. Follows docs/design/22-bundle.md §22.3/§22.4 and
/// docs/tasks/T-92.md's algorithm step by step: compile each UI, rewrite its external assets to
/// content-hashed names under `assets/`, reflect its connection points, render its thumbnail, then
/// write `manifest.json`.

namespace xt::bundle {

using compile::genericPathToUtf8;
using compile::pathFromUtf8;
using compile::pathToUtf8;

namespace fs = std::filesystem;

namespace {

/// Asset kinds as CompileOptions::assetNameRewrite reports them (0 image, 1 font; 2 shader, a
/// `.xtfx` custom effect, T-203: copied like any file, listed as `kind: shader`).
constexpr uint8_t kAssetKindImage = 0;
constexpr uint8_t kAssetKindFont = 1;

using util::fnv1a64;
using util::hex16;
using util::isoUtcNow;

/// `path`'s extension, lowercased and including the dot; ".bin" when it has none.
std::string lowerExtension(std::string_view path) {
    std::string ext = pathToUtf8(pathFromUtf8(path).extension());
    if (ext.empty() || ext == ".") {
        return ".bin";
    }
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return ext;
}

/// The manifest's `assets[].kind` string for a CompiledAsset kind.
const char* assetKindName(uint8_t kind) {
    switch (kind) {
        case kAssetKindImage:
        case 3:  // T-199: AtlasImage is still an image to the manifest.
            return "image";
        case kAssetKindFont:
            return "font";
        default:
            return "shader";
    }
}

/// T-199: true for the bitmap formats the atlas / `--compress` paths decode (stb_image).
bool isRasterExtension(const std::string& ext) {
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".gif" ||
           ext == ".tga";
}

/// T-199: appends the code points of UTF-8 `text` to `out` (malformed bytes are skipped).
void appendCodePoints(std::string_view text, std::set<uint32_t>& out) {
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        uint32_t cp = 0;
        size_t extra = 0;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            extra = 3;
        } else {
            ++i;
            continue;
        }
        bool ok = true;
        for (size_t k = 1; k <= extra; ++k) {
            if (i + k >= text.size() || (static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        }
        if (ok) {
            out.insert(cp);
        }
        i += extra + 1;
    }
}

/// T-199: nearest-neighbour 2x upscale (the 2x page of an entry that has no `@2x` variant).
ImageData upscale2x(const ImageData& src) {
    ImageData dst;
    dst.width = src.width * 2;
    dst.height = src.height * 2;
    dst.rgba.resize(static_cast<size_t>(dst.width) * static_cast<size_t>(dst.height) * 4);
    for (int y = 0; y < dst.height; ++y) {
        for (int x = 0; x < dst.width; ++x) {
            const uint8_t* from =
                src.rgba.data() +
                (static_cast<size_t>(y / 2) * static_cast<size_t>(src.width) + static_cast<size_t>(x / 2)) *
                    4;
            uint8_t* to =
                dst.rgba.data() +
                (static_cast<size_t>(y) * static_cast<size_t>(dst.width) + static_cast<size_t>(x)) * 4;
            to[0] = from[0];
            to[1] = from[1];
            to[2] = from[2];
            to[3] = from[3];
        }
    }
    return dst;
}

/// T-199: copies `src` into `page` at (x, y) (both RGBA8; clipped to the page) and extrudes its
/// edge pixels `pad` pixels outwards, so bilinear sampling at a cell's border reads the same
/// clamped texels a stand-alone image would (the atlas draws pixel-identical to the file).
void blit(ImageData& page, const ImageData& src, int x, int y, int pad) {
    if (src.width <= 0 || src.height <= 0) {
        return;
    }
    for (int row = -pad; row < src.height + pad; ++row) {
        const int py = y + row;
        if (py < 0 || py >= page.height) {
            continue;
        }
        const int sy = std::clamp(row, 0, src.height - 1);
        for (int col = -pad; col < src.width + pad; ++col) {
            const int px = x + col;
            if (px < 0 || px >= page.width) {
                continue;
            }
            const int sx = std::clamp(col, 0, src.width - 1);
            const uint8_t* from =
                src.rgba.data() +
                (static_cast<size_t>(sy) * static_cast<size_t>(src.width) + static_cast<size_t>(sx)) * 4;
            uint8_t* to =
                page.rgba.data() +
                (static_cast<size_t>(py) * static_cast<size_t>(page.width) + static_cast<size_t>(px)) * 4;
            to[0] = from[0];
            to[1] = from[1];
            to[2] = from[2];
            to[3] = from[3];
        }
    }
}

/// PortType as §22.3's lowercase string.
const char* portTypeName(flow::PortType type) {
    switch (type) {
        case flow::PortType::Bool:
            return "bool";
        case flow::PortType::Number:
            return "number";
        case flow::PortType::String:
            return "string";
        case flow::PortType::Pulse:
        default:
            return "pulse";
    }
}

/// The text shaper pack() renders thumbnails with: FreeType when xt_text is in the build (fonts
/// come from the system font directories plus `shared.fontDirs`), otherwise a NullTextShaper,
/// which still lays text out as boxes.
class PackShaper {
public:
    /// Registers the system fonts first (so a UI that names no bundled family still gets glyphs,
    /// docs/tasks/T-97.md), then every `.ttf`/`.otf` below each of `project.shared.fontDirs`
    /// (recursively); a missing directory is skipped. Bundled fonts are registered last, but
    /// match() resolves an `@font-face` family name ahead of the fallback candidates anyway.
    explicit PackShaper(const ProjectFile& project) {
#ifdef XT_BUNDLE_HAS_TEXT
        m_shaper.addSystemFonts();
        for (const std::string& dir : project.shared.fontDirs) {
            const fs::path root = pathFromUtf8(project.rootDir) / pathFromUtf8(dir);
            std::error_code ec;
            if (!fs::is_directory(root, ec)) {
                continue;
            }
            for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
                if (ec) {
                    break;
                }
                if (!it->is_regular_file(ec)) {
                    continue;
                }
                const std::string ext = lowerExtension(pathToUtf8(it->path()));
                if (ext == ".ttf" || ext == ".otf") {
                    m_shaper.addFontFile(pathToUtf8(it->path()));
                }
            }
        }
#else
        (void)project;
#endif
    }

    /// The shaper handed to Context/SwRenderer; never null.
    TextShaper* get() { return &m_shaper; }

private:
#ifdef XT_BUNDLE_HAS_TEXT
    text::FtTextShaper m_shaper;
#else
    NullTextShaper m_shaper;
#endif
};

/// Reads a whole file as raw bytes. Returns false when it cannot be read.
bool readBytes(const fs::path& path, std::string& out) { return detail::readFile(pathToUtf8(path), out); }

/// Writes raw bytes, creating no directories. Returns false on failure.
bool writeBytes(const fs::path& path, const std::string& bytes) {
    return detail::writeFile(pathToUtf8(path), bytes);
}

/// T-273b: the manifest's `assets[].sources` entry for the reference `sourceName` whose file is
/// `source`, in the project at `rootDir`. A relative reference to a file inside the project as
/// written; a file outside the project by its file name alone (the content hash is the entry's
/// `file`); an absolute reference to a file inside the project relative to the project root.
std::string manifestSourceName(std::string_view sourceName, const fs::path& source,
                               const std::string& rootDir) {
    std::error_code ec;
    const fs::path file = fs::absolute(source, ec).lexically_normal();
    const fs::path root = fs::absolute(pathFromUtf8(rootDir), ec).lexically_normal();
    // T-273d: the one "inside the project" test (compile::pathInsideRoot()).
    if (!compile::pathInsideRoot(file, root)) {
        return pathToUtf8(source.filename());
    }
    // The elements of `file` below `root` (pathInsideRoot() skips the same empty / `.` ones).
    auto element = file.begin();
    for (const fs::path& rootElement : root) {
        if (!rootElement.empty() && rootElement != ".") {
            ++element;
        }
    }
    fs::path relative;
    for (; element != file.end(); ++element) {
        relative /= *element;
    }
    if (relative.empty()) {
        return pathToUtf8(source.filename());  // The root itself is no file of the project.
    }
    if (!compile::isMachinePath(sourceName)) {
        return std::string(sourceName);
    }
    return genericPathToUtf8(relative);
}

/// T-237: a PNG written through the UTF-8 file helpers (sw::writePng opens its path through the
/// ANSI code page).
bool writePngFile(const fs::path& path, const uint8_t* rgba, int width, int height, int strideBytes) {
    std::vector<uint8_t> png;
    return sw::encodePng(rgba, width, height, strideBytes, png) &&
           writeBytes(path, std::string(png.begin(), png.end()));
}

/// Box-filters `src` (RGBA8, `srcW` x `srcH`) into a `dstW` x `dstH` RGBA8 buffer, preserving the
/// aspect ratio and leaving the leftover margin fully transparent (§22.3's thumbnails).
std::vector<uint8_t> downscale(const std::vector<uint8_t>& src, int srcW, int srcH, int dstW, int dstH) {
    std::vector<uint8_t> dst(static_cast<size_t>(dstW) * static_cast<size_t>(dstH) * 4, 0);
    const float scale = std::min(static_cast<float>(dstW) / static_cast<float>(srcW),
                                 static_cast<float>(dstH) / static_cast<float>(srcH));
    const int fitW = std::max(1, std::min(dstW, static_cast<int>(static_cast<float>(srcW) * scale)));
    const int fitH = std::max(1, std::min(dstH, static_cast<int>(static_cast<float>(srcH) * scale)));
    const int offsetX = (dstW - fitW) / 2;
    const int offsetY = (dstH - fitH) / 2;

    for (int y = 0; y < fitH; ++y) {
        const int y0 = y * srcH / fitH;
        const int y1 = std::max(y0 + 1, (y + 1) * srcH / fitH);
        for (int x = 0; x < fitW; ++x) {
            const int x0 = x * srcW / fitW;
            const int x1 = std::max(x0 + 1, (x + 1) * srcW / fitW);
            uint32_t sum[4] = {0, 0, 0, 0};
            uint32_t count = 0;
            for (int sy = y0; sy < y1; ++sy) {
                for (int sx = x0; sx < x1; ++sx) {
                    const size_t index =
                        (static_cast<size_t>(sy) * static_cast<size_t>(srcW) + static_cast<size_t>(sx)) * 4;
                    for (int c = 0; c < 4; ++c) {
                        sum[c] += src[index + static_cast<size_t>(c)];
                    }
                    ++count;
                }
            }
            const size_t out = (static_cast<size_t>(offsetY + y) * static_cast<size_t>(dstW) +
                                static_cast<size_t>(offsetX + x)) *
                               4;
            for (int c = 0; c < 4; ++c) {
                dst[out + static_cast<size_t>(c)] =
                    static_cast<uint8_t>(sum[c] / std::max<uint32_t>(count, 1));
            }
        }
    }
    return dst;
}

/// pack()'s working state: one run over one ProjectFile.
class Packer {
public:
    /// Borrows `project`/`options` (and `uiProject`, the UI Project `project` was made from, or
    /// nullptr) for the run; all outlive it.
    Packer(const ProjectFile& project, const PackOptions& options, const UiProject* uiProject = nullptr)
        : m_project(project), m_options(options), m_ui(uiProject), m_shaper(project) {}

    /// Runs §22.3/§22.4's algorithm and returns the report.
    PackReport run();

private:
    /// Records an error diagnostic with no source position.
    void error(std::string file, std::string message) {
        m_report.diagnostics.push_back(compile::Diagnostic{compile::Diagnostic::Level::Error, std::move(file),
                                                           0, 0, std::move(message)});
        m_report.ok = false;
    }

    /// True when `id` was named by `--ui` (or when `--ui` was not used at all).
    bool wanted(const std::string& id) const {
        return m_options.onlyUis.empty() ||
               std::find(m_options.onlyUis.begin(), m_options.onlyUis.end(), id) != m_options.onlyUis.end();
    }

    /// T-204 (§22.2): the asset lookup order the compile uses -- the UI's own directory, then
    /// `shared.assetRoot`, then the project root. Without it an image under `shared/` was reported
    /// "not found" by the compile although rewriteAsset()'s own fallback then packed it.
    std::vector<std::string> searchDirsFor(const ProjectUi& ui) const {
        const fs::path root = pathFromUtf8(m_project.rootDir);
        std::vector<std::string> dirs;
        dirs.push_back(pathToUtf8((root / pathFromUtf8(ui.doc)).parent_path()));
        if (!m_project.shared.assetRoot.empty()) {
            dirs.push_back(pathToUtf8(root / pathFromUtf8(m_project.shared.assetRoot)));
        }
        dirs.push_back(pathToUtf8(root));
        return dirs;
    }
    /// Compiles, writes and describes one UI; false when it did not compile.
    bool packUi(const ProjectUi& ui, UiInfo& out);
    /// T-198 (30.4, 30.8): copies the standard symbol files `docPath`'s `<link rel="symbols">`
    /// names into the project's symbol directory when they are missing there (see
    /// PackOptions::standardSymbolDir). A project file of the same name always wins.
    void pullStandardSymbols(const fs::path& docPath);
    /// The CompileOptions::assetNameRewrite body: hash, write, dedup, and return the new name.
    std::string rewriteAsset(std::string_view sourceName, std::string_view resolvedPath, uint8_t kind,
                             std::vector<std::string>& uiAssets);
    /// Reflects ports/`requires` off `doc` and, when asked, renders the thumbnail.
    void describeUi(Context& ctx, AssetResolver& assets, Document& doc, const ProjectUi& ui, UiInfo& out);
    /// Deletes files under `assets/`, `ui/`, `thumbs/`, `themes/` and `strings/` the manifest no
    /// longer references.
    void prune();
    /// T-179 (§22.10, 12.7): copies the wanted `<root>/strings/<lang>.csv` tables to
    /// `<out>/strings/`, fills the manifest's `strings` and `fonts.fallback`, and lints the tables
    /// (`i18n`): keys a packed UI uses that a table lacks (or leaves empty), keys no UI uses (full
    /// runs only), repeated keys.
    void packStrings(bool incremental, const ManifestData& previous);
    /// T-342 (03 §3.13 lint): one warning per removed `editor-only` id an edge of the UI's flow
    /// still names (`#<id>.<port>`); the published UI has no such element.
    void warnEditorOnlyFlowReferences(const std::string& label, const flow::FlowGraph& graph,
                                      const std::vector<std::string>& strippedIds) {
        for (const std::string& id : strippedIds) {
            const std::string prefix = "#" + id + ".";
            for (const flow::EdgeDesc& edge : graph.edges) {
                if (edge.from.rfind(prefix, 0) == 0 || edge.to.rfind(prefix, 0) == 0) {
                    addWarning(label, "editor-only element '" + id + "' is referenced by the flow (" +
                                          edge.from + " -> " + edge.to + ")");
                    break;
                }
            }
        }
    }

    /// Records a warning diagnostic with no source position.
    void addWarning(std::string file, std::string message) {
        compile::Diagnostic diagnostic;
        diagnostic.level = compile::Diagnostic::Level::Warning;
        diagnostic.file = std::move(file);
        diagnostic.message = std::move(message);
        m_report.diagnostics.push_back(std::move(diagnostic));
    }
    /// T-170 (§22.10): the manifest's `tokens` (the shared stylesheets' `--name: value`
    /// declarations) and `themes[]` (`<root>/themes/*.xts`, copied to `<out>/themes/`).
    void packTokens();
    /// T-183 (§22.10, 26.3.3): `project`, `motionSpec` and the `manifest/assets.yaml` lint for a
    /// UI Project; a no-op without one.
    void packProjectExtras(bool incremental);
    /// T-193 (§22.10 `flows.global`, 27.4): validates `<root>/bundle.xtflow` (else
    /// `<root>/shared/bundle.xtflow`) -- FlowGraph::parse() plus every `ui#<id>` naming a UI of
    /// the project -- and copies it to `<out>/bundle.xtflow`; a no-op when neither exists.
    void packGlobalFlow();
    /// T-199 (26 §26.3.4): records how `doc` uses its image assets -- an asset is an atlas
    /// candidate when every use is an `<img>` (not a video/sequence/external), a
    /// `background-image` or a stretched `border-image`; an inline text image, a `repeat`/`round`
    /// border-image, a media element or a Lottie scene image keeps it out -- and, for
    /// `--font-subset`, the code points of the document's text.
    void collectImageUses(const Document& doc, const IrView& view, const std::string& uiId);
    /// T-199: packs the candidates into pages (`atlas/<name>.png|.dds`, manifest `atlas[]`),
    /// deletes their own files and fills `m_atlasPage` for the rewrite pass.
    void buildAtlases();
    /// T-199 (`--font-subset`): rewrites every bundled TrueType font as `fonts/<stem>.subset.ttf`
    /// and lints glyph coverage per language (`i18n`).
    void subsetFonts();
    /// T-199: pass 2 -- recompiles every packed UI with `m_rewritePass` so its `.xtir` names the
    /// atlas cells (`atlas:<name>#<asset>`) and subset fonts; rewrites `ui/<id>.xtir` and its hash.
    void rewriteUis();
    /// T-204: `--atlas` with `--compress` defers compression until the pages are packed (the
    /// candidates are collected from the plain `.png`/`.jpg` names); compressDeferred() then
    /// compresses the images that did not go into a page (pages compress as they are written).
    bool deferCompress() const { return m_atlas && m_compress != BcFormat::None; }
    void compressDeferred();
    /// T-199: the block format an image gets under `--compress` (the assets.yaml `compress:`
    /// override first, then alpha demotes bc1 to bc3); None keeps the file as it is.
    BcFormat compressFormatFor(const std::string& canonicalSource, const ImageData& decoded) const;
    /// T-200 (§22.10 `symbols[]`): the contract summary of every symbol of the project.
    void packSymbolContracts();
    /// T-204 (§22.10 `glyphs[device]`): packs every `glyphs/<device>/<key>.<image ext>` under
    /// `shared.assetRoot`, then the project root (the first of a device/key wins), as an ordinary
    /// asset and lists it in the manifest's `glyphs`. device is mouse|gamepad|touch|keyboard; any
    /// other directory is warned about and skipped.
    void packGlyphs();

    /// T-183: the theme / string-table directories, relative to the project root.
    /// T-407: the `.xtui`'s `spec.behavior`; 0 for a `bundle.xtproj` pack (no UI Project).
    uint32_t behavior() const { return m_ui != nullptr ? m_ui->spec.behavior : 0; }
    std::string themesDir() const { return m_ui != nullptr ? m_ui->shared.themes : std::string("themes"); }
    std::string stringsDir() const { return m_ui != nullptr ? m_ui->shared.strings : std::string("strings"); }
    /// T-183: `--lang`, else the UI Project's `publish.languages`.
    const std::vector<std::string>& languages() const {
        return (m_options.languages.empty() && m_ui != nullptr) ? m_ui->publish.languages
                                                                : m_options.languages;
    }

    const ProjectFile& m_project;
    const PackOptions& m_options;
    const UiProject* m_ui = nullptr;
    PackShaper m_shaper;
    PackReport m_report;
    ManifestData m_manifest;
    /// `assets/<hash16>.<ext>` -> index into m_manifest.assets.
    std::unordered_map<std::string, size_t> m_assetIndex;
    /// T-179: the `@key`s the UIs packed this run use.
    std::set<std::string> m_usedKeys;
    /// T-273d: the "video ... is outside the project" warnings already reported this run, so a
    /// path several UIs name is reported once.
    std::set<std::string> m_outsideVideoWarnings;
    /// T-183: canonicalAssetPath() of every source file an asset reference resolved to this run.
    std::vector<std::string> m_referenced;
    /// T-183: `assets/<hash16>.<ext>` -> canonicalAssetPath() of the first source file behind it.
    std::unordered_map<std::string, std::string> m_assetSourcePath;
    fs::path m_outDir;

    // T-199.
    BcFormat m_compress = BcFormat::None;
    bool m_atlas = false;
    bool m_atlasBundleScope = false;
    int m_atlasMaxSize = 4096;
    int m_atlasPadding = 2;
    bool m_atlasMipmaps = false;
    bool m_fontSubset = false;
    /// Pass 2 (rewriteUis): rewriteAsset() only maps names, writes nothing.
    bool m_rewritePass = false;
    std::string m_currentUi;
    /// `assets/<hash16>.<ext>` (the content name) -> the file actually written (`.dds` when
    /// compressed, else the same name).
    std::unordered_map<std::string, std::string> m_finalNameOf;
    /// Written file -> canonical path of its `@2x` source (only when one exists).
    std::unordered_map<std::string, std::string> m_variant2xSource;
    /// T-345: the same for the `@3x` source.
    std::unordered_map<std::string, std::string> m_variant3xSource;
    /// canonical source path -> `manifest/assets.yaml` `compress:` override.
    std::unordered_map<std::string, std::string> m_compressOverride;
    struct ImageUses {
        std::set<std::string> safe;
        std::set<std::string> unsafe;
    };
    /// UI id -> its image uses.
    std::map<std::string, ImageUses> m_imageUses;
    /// Group key (UI id, or "" for bundle scope) -> written file -> atlas page name.
    std::map<std::string, std::unordered_map<std::string, std::string>> m_atlasPage;
    /// Written font file -> `fonts/<stem>.subset.ttf`.
    std::unordered_map<std::string, std::string> m_fontSubsetOf;
    /// T-262a: files the atlas pages, the deferred compression and the font subsets replace;
    /// deleted only once rewriteUis() succeeded for every UI (never on a failed run, whose `.xtir`
    /// files still name them).
    std::vector<fs::path> m_pendingRemovals;
    /// T-204: the packed names of the manifest glyphs. A glyph is resolved by name at run time
    /// (an inline text image), so it never goes into an atlas page.
    std::set<std::string> m_glyphAssets;
    /// Code points of every document text and string table (for the subset).
    std::set<uint32_t> m_charset;
    /// Language -> code points of its string table ("" = the documents' own text).
    std::map<std::string, std::set<uint32_t>> m_langChars;
};

std::string Packer::rewriteAsset(std::string_view sourceName, std::string_view resolvedPath, uint8_t kind,
                                 std::vector<std::string>& uiAssets) {
    // §22.2's lookup order: the UI's own directory first (compileDocument already resolved that
    // into `resolvedPath`), then `shared.assetRoot`.
    fs::path source = pathFromUtf8(resolvedPath);
    if (resolvedPath.empty()) {
        std::error_code ec;
        const fs::path fallback = pathFromUtf8(m_project.rootDir) / pathFromUtf8(m_project.shared.assetRoot) /
                                  pathFromUtf8(sourceName);
        if (!fs::is_regular_file(fallback, ec)) {
            return std::string();
        }
        source = fallback;
    }

    std::string bytes;
    if (!readBytes(source, bytes)) {
        return std::string();
    }

    const std::string contentName = "assets/" + hex16(fnv1a64(bytes)) + lowerExtension(sourceName);
    if (m_rewritePass) {
        // T-199 pass 2: every file exists; only the names the `.xtir` carries change.
        const auto finalIt = m_finalNameOf.find(contentName);
        const std::string file = finalIt != m_finalNameOf.end() ? finalIt->second : contentName;
        if (kind == kAssetKindFont) {
            const auto subset = m_fontSubsetOf.find(file);
            if (subset != m_fontSubsetOf.end()) {
                if (std::find(uiAssets.begin(), uiAssets.end(), subset->second) == uiAssets.end()) {
                    uiAssets.push_back(subset->second);
                }
                return subset->second;
            }
        }
        if (std::find(uiAssets.begin(), uiAssets.end(), file) == uiAssets.end()) {
            uiAssets.push_back(file);
        }
        if (kind == kAssetKindImage) {
            const auto group = m_atlasPage.find(m_atlasBundleScope ? std::string() : m_currentUi);
            if (group != m_atlasPage.end()) {
                const auto page = group->second.find(file);
                if (page != group->second.end()) {
                    return std::string(kAtlasAssetScheme) + page->second + "#" + file;
                }
            }
        }
        return file;
    }

    const std::string canonical = canonicalAssetPath(pathToUtf8(source));
    // T-199 `--compress`: a bitmap becomes `assets/<hash16>.dds` (hash of the source bytes).
    std::string name = contentName;
    std::string written = bytes;
    std::string format;
    BcFormat bcFormat = BcFormat::None;
    if (kind == kAssetKindImage && m_compress != BcFormat::None && !deferCompress() &&
        isRasterExtension(lowerExtension(sourceName))) {
        ImageData decoded;
        if (sw::decodeImageBytes(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), decoded)) {
            bcFormat = compressFormatFor(canonical, decoded);
            if (bcFormat != BcFormat::None) {
                const std::vector<uint8_t> dds =
                    encodeDds(decoded.rgba.data(), decoded.width, decoded.height, bcFormat, m_atlasMipmaps);
                if (!dds.empty()) {
                    written.assign(dds.begin(), dds.end());
                    name = "assets/" + hex16(fnv1a64(bytes)) + ".dds";
                    format = bcFormatName(bcFormat);
                }
            }
        }
    }
    m_finalNameOf[contentName] = name;
    m_referenced.push_back(canonical);
    m_assetSourcePath.emplace(name, canonical);
    if (std::find(uiAssets.begin(), uiAssets.end(), name) == uiAssets.end()) {
        uiAssets.push_back(name);
    }

    // T-273b: no machine path in the published manifest.
    const std::string listedSource = manifestSourceName(sourceName, source, m_project.rootDir);
    const auto it = m_assetIndex.find(name);
    if (it != m_assetIndex.end()) {
        ManifestAsset& existing = m_manifest.assets[it->second];
        if (std::find(existing.sources.begin(), existing.sources.end(), listedSource) ==
            existing.sources.end()) {
            existing.sources.push_back(listedSource);
        }
        ++m_report.assetsDeduplicated;
        return name;
    }

    // T-258b: the run time does not decode a bitmap over sw::kMaxImageDimension per side or
    // sw::kMaxImagePixels in all (it draws nothing), so say so here, from the file header.
    auto warnIfUndecodable = [&](const std::string& imageBytes, const std::string& path) {
        if (kind != kAssetKindImage || !isRasterExtension(lowerExtension(path))) {
            return;
        }
        const ImageMeasure m = measureImage(imageBytes, lowerExtension(path));
        if (m.ok && (m.w > sw::kMaxImageDimension || m.h > sw::kMaxImageDimension ||
                     static_cast<int64_t>(m.w) * static_cast<int64_t>(m.h) > sw::kMaxImagePixels)) {
            addWarning(std::string(sourceName),
                       "image \"" + path + "\" is " + std::to_string(m.w) + "x" + std::to_string(m.h) +
                           "; larger than " + std::to_string(sw::kMaxImageDimension) + " px per side or " +
                           std::to_string(sw::kMaxImagePixels / (1024 * 1024)) +
                           "M pixels is not decoded at run time");
        }
    };
    warnIfUndecodable(bytes, std::string(sourceName));
    if (!writeBytes(m_outDir / pathFromUtf8(name), written)) {
        error(std::string(sourceName), "cannot write '" + pathToUtf8(m_outDir / pathFromUtf8(name)) + "'");
        return std::string();
    }
    if (!format.empty()) {
        ++m_report.assetsCompressed;
    }
    ManifestAsset asset;
    asset.file = name;
    asset.kind = assetKindName(kind);
    asset.bytes = static_cast<uint64_t>(written.size());
    asset.sources.push_back(listedSource);
    asset.format = format;

    // T-199 `@2x`: `<stem>@2x<ext>` next to the source is packed as the variant; T-345 (25 §25.4.2
    // item 4): `<stem>@3x<ext>` likewise.
    if (kind == kAssetKindImage) {
        auto packVariant = [&](const std::string& suffix, std::string& variantFile,
                               std::unordered_map<std::string, std::string>& sources, uint32_t& counter) {
            // T-261e: the name is UTF-8 -- a bare std::string here would go through the ANSI code
            // page (and throw on a name it cannot spell).
            const fs::path variantPath =
                source.parent_path() /
                pathFromUtf8(pathToUtf8(source.stem()) + suffix + pathToUtf8(source.extension()));
            std::error_code ec;
            std::string bytes2;
            if (!fs::is_regular_file(variantPath, ec) || !readBytes(variantPath, bytes2)) {
                return;
            }
            warnIfUndecodable(bytes2, pathToUtf8(variantPath.filename()));
            std::string written2 = bytes2;
            std::string name2 = "assets/" + hex16(fnv1a64(bytes)) + suffix + lowerExtension(sourceName);
            if (bcFormat != BcFormat::None) {
                ImageData decoded2;
                if (sw::decodeImageBytes(reinterpret_cast<const uint8_t*>(bytes2.data()), bytes2.size(),
                                         decoded2)) {
                    const std::vector<uint8_t> dds = encodeDds(decoded2.rgba.data(), decoded2.width,
                                                               decoded2.height, bcFormat, m_atlasMipmaps);
                    if (!dds.empty()) {
                        written2.assign(dds.begin(), dds.end());
                        name2 = "assets/" + hex16(fnv1a64(bytes)) + suffix + ".dds";
                    }
                }
            }
            if (writeBytes(m_outDir / pathFromUtf8(name2), written2)) {
                variantFile = name2;
                sources[name] = canonicalAssetPath(pathToUtf8(variantPath));
                ++counter;
            } else {
                addWarning(std::string(sourceName),
                           "cannot write the " + suffix + " variant '" + name2 + "'");
            }
        };
        packVariant("@2x", asset.variant2x, m_variant2xSource, m_report.variants2x);
        packVariant("@3x", asset.variant3x, m_variant3xSource, m_report.variants3x);
    }
    m_assetIndex.emplace(name, m_manifest.assets.size());
    m_manifest.assets.push_back(std::move(asset));
    ++m_report.assetsWritten;
    return name;
}

BcFormat Packer::compressFormatFor(const std::string& canonicalSource, const ImageData& decoded) const {
    BcFormat format = m_compress;
    const auto override = m_compressOverride.find(canonicalSource);
    if (override != m_compressOverride.end()) {
        BcFormat forced = BcFormat::None;
        if (parseBcFormat(override->second, forced)) {
            return forced;
        }
    }
    if (format == BcFormat::Bc1 && imageHasAlpha(decoded.rgba.data(), decoded.width, decoded.height)) {
        format = BcFormat::Bc3;  // 26.3.4: an icon with alpha never goes through BC1's 1-bit alpha.
    }
    return format;
}

void Packer::collectImageUses(const Document& doc, const IrView& view, const std::string& uiId) {
    ImageUses& uses = m_imageUses[uiId];
    std::span<const IrAsset> assets = view.assets().assets();
    auto nameOf = [&](AssetId id) -> std::string {
        if (id == kInvalidAsset || id >= assets.size() || assets[id].embedded != 0 ||
            assets[id].kind != kAssetKindImage) {
            return std::string();
        }
        const std::string name(view.string(assets[id].name));
        if (isExternalAssetName(name) || name.compare(0, 7, "assets/") != 0 ||
            !isRasterExtension(lowerExtension(name))) {
            return std::string();
        }
        return name;
    };
    auto mark = [&](AssetId id, bool safe) {
        const std::string name = nameOf(id);
        if (name.empty()) {
            return;
        }
        (safe ? uses.safe : uses.unsafe).insert(name);
    };
    const NodeArena& arena = doc.arena();
    for (NodeId node = 0; node < arena.capacity(); ++node) {
        if (!arena.alive(node)) {
            continue;
        }
        if (arena.kind(node) == NodeKind::Element) {
            const ElementData& data = arena.element(node);
            if (data.imageAsset != kInvalidAsset) {
                mark(data.imageAsset, data.externalNode == kInvalidNode);
            }
            if (data.style.backgroundImage != kInvalidAsset) {
                mark(data.style.backgroundImage, true);
            }
            if (data.style.borderImage != kInvalidAsset) {
                mark(data.style.borderImage, data.style.borderImageRepeat == 0);  // repeat tiles wrap.
            }
        } else if (arena.kind(node) == NodeKind::Text) {
            for (const TextInline& object : arena.text(node).inlines) {
                mark(object.asset, false);
            }
        }
    }
    for (uint32_t i = 0; i < view.sceneCount(); ++i) {
        for (const IrImageData& image : view.scene(i).images()) {
            mark(image.asset, false);
        }
    }
    if (m_fontSubset) {
        for (const IrDocText& text : view.doc().texts()) {
            appendCodePoints(view.string(text.text), m_charset);
            appendCodePoints(view.string(text.text), m_langChars[std::string()]);
        }
    }
}

void Packer::buildAtlases() {
    std::set<std::string> unsafeAll = m_glyphAssets;  // T-204
    for (const auto& [ui, uses] : m_imageUses) {
        unsafeAll.insert(uses.unsafe.begin(), uses.unsafe.end());
    }
    struct Group {
        std::string key;
        std::string prefix;
        std::vector<std::string> assets;
    };
    std::vector<Group> groups;
    if (m_atlasBundleScope) {
        Group group;
        std::set<std::string> all;
        for (const auto& [ui, uses] : m_imageUses) {
            all.insert(uses.safe.begin(), uses.safe.end());
        }
        for (const std::string& asset : all) {
            if (unsafeAll.count(asset) == 0) {
                group.assets.push_back(asset);
            }
        }
        groups.push_back(std::move(group));
    } else {
        for (const auto& [ui, uses] : m_imageUses) {
            Group group;
            group.key = ui;
            group.prefix = ui + "-";
            for (const std::string& asset : uses.safe) {
                if (unsafeAll.count(asset) == 0) {
                    group.assets.push_back(asset);
                }
            }
            groups.push_back(std::move(group));
        }
    }

    std::error_code ec;
    std::unordered_map<std::string, ImageData> decoded;
    std::unordered_map<std::string, ImageData> decoded2x;
    auto decodeFile = [&](const std::string& path, ImageData& out) {
        std::string bytes;
        return readBytes(pathFromUtf8(path), bytes) &&
               sw::decodeImageBytes(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), out);
    };
    int padding = std::max(0, m_atlasPadding);
    if (m_atlasMipmaps) {
        padding = std::max(1, padding) * 8;  // Three mip levels stay free of their neighbours.
    }
    bool createdDir = false;
    for (const Group& group : groups) {
        std::vector<AtlasInput> inputs;
        for (const std::string& asset : group.assets) {
            if (decoded.count(asset) == 0) {
                const auto source = m_assetSourcePath.find(asset);
                ImageData image;
                if (source == m_assetSourcePath.end() || !decodeFile(source->second, image)) {
                    addWarning(asset, "atlas: cannot decode the image; kept as a file");
                    continue;
                }
                const auto twoX = m_variant2xSource.find(asset);
                if (twoX != m_variant2xSource.end()) {
                    ImageData image2;
                    if (decodeFile(twoX->second, image2) && image2.width == image.width * 2 &&
                        image2.height == image.height * 2) {
                        decoded2x.emplace(asset, std::move(image2));
                    } else {
                        addWarning(
                            asset,
                            "atlas: the @2x variant is not exactly twice the size; upscaling the 1x image");
                    }
                }
                decoded.emplace(asset, std::move(image));
            }
            const ImageData& image = decoded[asset];
            inputs.push_back(AtlasInput{asset, image.width, image.height});
        }
        if (inputs.empty()) {
            continue;
        }
        std::vector<AtlasPage> pages;
        std::vector<std::string> tooLarge;
        packAtlas(inputs, m_atlasMaxSize, padding, pages, tooLarge);
        for (const std::string& asset : tooLarge) {
            addWarning(asset, "atlas: larger than maxSize; kept as a file");
        }
        for (size_t pageIndex = 0; pageIndex < pages.size(); ++pageIndex) {
            const AtlasPage& page = pages[pageIndex];
            const std::string name = group.prefix + std::to_string(pageIndex);
            ImageData rgba;
            rgba.width = page.width;
            rgba.height = page.height;
            rgba.rgba.assign(static_cast<size_t>(page.width) * static_cast<size_t>(page.height) * 4, 0);
            bool any2x = false;
            for (const AtlasPlacement& entry : page.entries) {
                blit(rgba, decoded[entry.id], entry.x, entry.y, padding);
                any2x = any2x || decoded2x.count(entry.id) != 0;
                m_report.atlasPixelsUsed +=
                    static_cast<uint64_t>(entry.width) * static_cast<uint64_t>(entry.height);
            }
            m_report.atlasPixelsTotal +=
                static_cast<uint64_t>(page.width) * static_cast<uint64_t>(page.height);
            ImageData rgba2x;
            if (any2x) {
                rgba2x.width = page.width * 2;
                rgba2x.height = page.height * 2;
                rgba2x.rgba.assign(static_cast<size_t>(rgba2x.width) * static_cast<size_t>(rgba2x.height) * 4,
                                   0);
                for (const AtlasPlacement& entry : page.entries) {
                    const auto hi = decoded2x.find(entry.id);
                    if (hi != decoded2x.end()) {
                        blit(rgba2x, hi->second, entry.x * 2, entry.y * 2, padding * 2);
                    } else {
                        blit(rgba2x, upscale2x(decoded[entry.id]), entry.x * 2, entry.y * 2, padding * 2);
                    }
                }
            }
            if (!createdDir) {
                fs::create_directories(m_outDir / "atlas", ec);
                createdDir = true;
            }
            ManifestAtlas manifestPage;
            manifestPage.name = name;
            manifestPage.w = page.width;
            manifestPage.h = page.height;
            auto writePage = [&](const ImageData& pixels, const std::string& stem, std::string& fileOut) {
                BcFormat format = m_compress;
                if (format == BcFormat::Bc1 &&
                    imageHasAlpha(pixels.rgba.data(), pixels.width, pixels.height)) {
                    format = BcFormat::Bc3;
                }
                if (format != BcFormat::None) {
                    const std::vector<uint8_t> dds =
                        encodeDds(pixels.rgba.data(), pixels.width, pixels.height, format, m_atlasMipmaps);
                    const std::string relative = "atlas/" + stem + ".dds";
                    if (dds.empty() ||
                        !writeBytes(m_outDir / pathFromUtf8(relative), std::string(dds.begin(), dds.end()))) {
                        error(m_project.id,
                              "cannot write '" + pathToUtf8(m_outDir / pathFromUtf8(relative)) + "'");
                        return;
                    }
                    manifestPage.format = bcFormatName(format);
                    fileOut = relative;
                    ++m_report.assetsCompressed;
                    return;
                }
                const std::string relative = "atlas/" + stem + ".png";
                if (!writePngFile(m_outDir / pathFromUtf8(relative), pixels.rgba.data(), pixels.width,
                                  pixels.height, pixels.width * 4)) {
                    error(m_project.id,
                          "cannot write '" + pathToUtf8(m_outDir / pathFromUtf8(relative)) + "'");
                    return;
                }
                fileOut = relative;
            };
            writePage(rgba, name, manifestPage.file);
            if (any2x) {
                writePage(rgba2x, name + "@2x", manifestPage.file2x);
            }
            if (manifestPage.file.empty()) {
                continue;
            }
            for (const AtlasPlacement& entry : page.entries) {
                manifestPage.entries.push_back(
                    ManifestAtlasEntry{entry.id, entry.x, entry.y, entry.width, entry.height});
                m_atlasPage[group.key][entry.id] = name;
                const auto row = m_assetIndex.find(entry.id);
                if (row != m_assetIndex.end()) {
                    std::vector<std::string>& pagesOf = m_manifest.assets[row->second].atlases;
                    if (std::find(pagesOf.begin(), pagesOf.end(), name) == pagesOf.end()) {
                        pagesOf.push_back(name);
                    }
                }
                ++m_report.atlasEntries;
            }
            m_manifest.atlases.push_back(std::move(manifestPage));
            ++m_report.atlasPages;
        }
    }
    // Every use of an atlased asset goes through its page(s): its own file (and variant) go, once
    // the rewrite pass has succeeded (T-262a).
    for (ManifestAsset& asset : m_manifest.assets) {
        if (asset.atlases.empty()) {
            continue;
        }
        m_pendingRemovals.push_back(m_outDir / pathFromUtf8(asset.file));
        if (!asset.variant2x.empty()) {
            m_pendingRemovals.push_back(m_outDir / pathFromUtf8(asset.variant2x));
        }
        if (!asset.variant3x.empty()) {
            m_pendingRemovals.push_back(m_outDir / pathFromUtf8(asset.variant3x));  // T-345
            asset.variant3x.clear();  // Atlas pages have no 3x file; the page's 2x serves.
        }
    }
}

void Packer::compressDeferred() {
    auto decodeFile = [&](const std::string& path, ImageData& out) {
        std::string bytes;
        return readBytes(pathFromUtf8(path), bytes) &&
               sw::decodeImageBytes(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), out);
    };
    for (size_t index = 0; index < m_manifest.assets.size(); ++index) {
        ManifestAsset& asset = m_manifest.assets[index];
        if (asset.kind != "image" || !asset.atlases.empty() || !asset.format.empty() ||
            !isRasterExtension(lowerExtension(asset.file))) {
            continue;
        }
        const auto source = m_assetSourcePath.find(asset.file);
        ImageData decoded;
        if (source == m_assetSourcePath.end() || !decodeFile(source->second, decoded)) {
            continue;  // Not decodable: kept as it is, as in the non-atlas path.
        }
        const std::string sourcePath = source->second;
        const BcFormat format = compressFormatFor(sourcePath, decoded);
        if (format == BcFormat::None) {
            continue;
        }
        // Same name the non-atlas path gives: `assets/<hash of the source bytes>.dds`.
        const std::string stem = "assets/" + pathToUtf8(pathFromUtf8(asset.file).stem());
        const std::string name = stem + ".dds";
        if (m_assetIndex.count(name) != 0) {
            continue;
        }
        const std::vector<uint8_t> dds =
            encodeDds(decoded.rgba.data(), decoded.width, decoded.height, format, m_atlasMipmaps);
        if (dds.empty() || !writeBytes(m_outDir / pathFromUtf8(name), std::string(dds.begin(), dds.end()))) {
            error(asset.file, "cannot write '" + pathToUtf8(m_outDir / pathFromUtf8(name)) + "'");
            continue;
        }
        const std::string old = asset.file;
        m_pendingRemovals.push_back(m_outDir / pathFromUtf8(old));  // T-262a: after rewriteUis().
        const auto twoX = m_variant2xSource.find(old);
        if (twoX != m_variant2xSource.end() && !asset.variant2x.empty()) {
            const std::string twoXPath = twoX->second;
            ImageData decoded2;
            if (decodeFile(twoXPath, decoded2)) {
                const std::vector<uint8_t> dds2 =
                    encodeDds(decoded2.rgba.data(), decoded2.width, decoded2.height, format, m_atlasMipmaps);
                const std::string name2 = stem + "@2x.dds";
                if (!dds2.empty() &&
                    writeBytes(m_outDir / pathFromUtf8(name2), std::string(dds2.begin(), dds2.end()))) {
                    m_pendingRemovals.push_back(m_outDir / pathFromUtf8(asset.variant2x));  // T-262a
                    asset.variant2x = name2;
                }
            }
            m_variant2xSource[name] = twoXPath;
        }
        // T-345: the `@3x` variant is compressed the same way.
        const auto threeX = m_variant3xSource.find(old);
        if (threeX != m_variant3xSource.end() && !asset.variant3x.empty()) {
            const std::string threeXPath = threeX->second;
            ImageData decoded3;
            if (decodeFile(threeXPath, decoded3)) {
                const std::vector<uint8_t> dds3 =
                    encodeDds(decoded3.rgba.data(), decoded3.width, decoded3.height, format, m_atlasMipmaps);
                const std::string name3 = stem + "@3x.dds";
                if (!dds3.empty() &&
                    writeBytes(m_outDir / pathFromUtf8(name3), std::string(dds3.begin(), dds3.end()))) {
                    m_pendingRemovals.push_back(m_outDir / pathFromUtf8(asset.variant3x));
                    asset.variant3x = name3;
                }
            }
            m_variant3xSource[name] = threeXPath;
        }
        asset.file = name;
        asset.bytes = static_cast<uint64_t>(dds.size());
        asset.format = bcFormatName(format);
        m_assetIndex.erase(old);
        m_assetIndex.emplace(name, index);
        m_assetSourcePath.emplace(name, sourcePath);
        for (auto& [content, written] : m_finalNameOf) {
            if (written == old) {
                written = name;
            }
        }
        for (UiInfo& ui : m_manifest.uis) {
            std::replace(ui.assets.begin(), ui.assets.end(), old, name);
        }
        for (GlyphInfo& glyph : m_manifest.glyphs) {  // T-204
            if (glyph.asset == old) {
                glyph.asset = name;
            }
        }
        ++m_report.assetsCompressed;
    }
}

void Packer::subsetFonts() {
    std::set<uint32_t> charset = m_charset;
    for (uint32_t cp = 0x20; cp <= 0x7E; ++cp) {
        charset.insert(cp);
    }
    const std::vector<uint32_t> codePoints(charset.begin(), charset.end());
    std::vector<FontFile> fonts;
    std::error_code ec;
    bool createdDir = false;
    std::set<std::string> usedNames;
    for (ManifestAsset& asset : m_manifest.assets) {
        if (asset.kind != "font") {
            continue;
        }
        std::string text;
        if (!readBytes(m_outDir / pathFromUtf8(asset.file), text)) {
            addWarning(asset.file, "font-subset: cannot read the font; kept as it is");
            continue;
        }
        FontFile font;
        const std::vector<uint8_t> bytes(text.begin(), text.end());
        if (!font.parse(bytes)) {
            addWarning(asset.file,
                       "font-subset: not a TrueType/OpenType font this subsetter reads; kept as it is");
            continue;
        }
        std::string stem = asset.sources.empty() ? pathToUtf8(pathFromUtf8(asset.file).stem())
                                                 : pathToUtf8(pathFromUtf8(asset.sources[0]).stem());
        if (stem.size() > 7 && stem.compare(stem.size() - 7, 7, ".subset") == 0) {
            stem.erase(stem.size() - 7);  // An already-subset source keeps one suffix.
        }
        std::string relative = "fonts/" + stem + ".subset.ttf";
        for (int n = 2; usedNames.count(relative) != 0; ++n) {
            relative = "fonts/" + stem + "-" + std::to_string(n) + ".subset.ttf";
        }
        usedNames.insert(relative);
        std::vector<uint8_t> out;
        if (!font.subset(codePoints, out)) {
            addWarning(asset.file, "font-subset: subsetting failed; kept as it is");
            continue;
        }
        if (!createdDir) {
            fs::create_directories(m_outDir / "fonts", ec);
            createdDir = true;
        }
        if (!writeBytes(m_outDir / pathFromUtf8(relative), std::string(out.begin(), out.end()))) {
            error(asset.file, "cannot write '" + pathToUtf8(m_outDir / pathFromUtf8(relative)) + "'");
            continue;
        }
        m_pendingRemovals.push_back(m_outDir / pathFromUtf8(asset.file));  // T-262a
        m_fontSubsetOf[asset.file] = relative;
        asset.file = relative;
        asset.bytes = static_cast<uint64_t>(out.size());
        ++m_report.fontsSubset;
        fonts.push_back(std::move(font));
    }
    if (fonts.empty()) {
        return;
    }
    // Coverage lint (12.7 `i18n`): every code point of every language must have a glyph in some
    // bundled font; at most 20 misses are reported per table.
    for (const auto& [lang, chars] : m_langChars) {
        int reported = 0;
        for (uint32_t cp : chars) {
            if (cp < 0x20 || cp == 0xFEFF) {
                continue;
            }
            bool covered = false;
            for (const FontFile& font : fonts) {
                covered = covered || font.hasGlyph(cp);
            }
            if (covered) {
                continue;
            }
            if (++reported > 20) {
                addWarning(lang.empty() ? "<text>" : "strings/" + lang + ".csv",
                           "i18n: more glyphs missing (not listed)");
                break;
            }
            char code[16];
            std::snprintf(code, sizeof(code), "U+%04X", static_cast<unsigned>(cp));
            addWarning(lang.empty() ? "<text>" : "strings/" + lang + ".csv",
                       std::string("i18n: ") + code + " has no glyph in any bundled font" +
                           (lang.empty() ? "" : " for " + lang));
        }
    }
}

void Packer::rewriteUis() {
    m_rewritePass = true;
    for (const ProjectUi& ui : m_project.uis) {
        if (std::find(m_report.packedUis.begin(), m_report.packedUis.end(), ui.id) ==
            m_report.packedUis.end()) {
            continue;
        }
        m_currentUi = ui.id;
        std::vector<std::string> uiAssets;
        compile::CompileOptions compileOptions;
        compileOptions.rootDir = m_project.rootDir;
        compileOptions.strict = m_options.strict;
        compileOptions.embedImages = m_project.publish.embedImages || m_options.embed;
        compileOptions.embedFonts = m_project.publish.embedFonts || m_options.embed;
        compileOptions.searchDirs = searchDirsFor(ui);                   // T-204
        compileOptions.standardSymbolDir = m_options.standardSymbolDir;  // T-235: `xt-standard:`
        compileOptions.publishAssetNames = true;                         // T-273b
        compileOptions.stripEditorOnly = true;                           // T-342 (22 §22.15)
        compileOptions.behavior = behavior();                            // T-407
        for (const std::string& style : m_project.shared.styles) {
            std::error_code ec;
            const fs::path absolute = fs::absolute(pathFromUtf8(m_project.rootDir) / pathFromUtf8(style), ec);
            compileOptions.impliedStylesheets.push_back(pathToUtf8(absolute.lexically_normal()));
        }
        compileOptions.assetNameRewrite = [this, &uiAssets](std::string_view sourceName,
                                                            std::string_view resolvedPath, uint8_t kind) {
            return rewriteAsset(sourceName, resolvedPath, kind, uiAssets);
        };
        const fs::path docPath = pathFromUtf8(m_project.rootDir) / pathFromUtf8(ui.doc);
        compile::CompileResult result = compile::compileDocument(pathToUtf8(docPath), compileOptions);
        if (!result.ok()) {
            error(ui.id, "atlas rewrite: the UI no longer compiles");
            continue;
        }
        const std::string irBytes(reinterpret_cast<const char*>(result.ir.data()), result.ir.size());
        const std::string irRelative = "ui/" + ui.id + ".xtir";
        if (!writeBytes(m_outDir / pathFromUtf8(irRelative), irBytes)) {
            error(ui.id, "cannot write '" + pathToUtf8(m_outDir / pathFromUtf8(irRelative)) + "'");
            continue;
        }
        for (UiInfo& info : m_manifest.uis) {
            if (info.id == ui.id) {
                info.fileHash = irFileHash(result.ir);  // T-200: without the META compile time.
                info.assets = uiAssets;
            }
        }
    }
    m_rewritePass = false;
    m_currentUi.clear();
}

void Packer::describeUi(Context& ctx, AssetResolver& assets, Document& doc, const ProjectUi& ui,
                        UiInfo& out) {
    // T-258a: the project readers already rejected sizes outside 1..16384; the casts only saturate.
    const int width = std::max(1, toInt32Sat(ui.size.x));
    const int height = std::max(1, toInt32Sat(ui.size.y));
    // T-408 (43 §43.6): the contract hash, taken before the first update so port defaults are
    // the declared ones.
    out.contract = contractHash(doc);
    ctx.setViewport(Rect{0, 0, static_cast<float>(width), static_cast<float>(height)}, 1.0f);
    ctx.update(0.0f);

    for (const flow::PortDesc& port : flow::connectionPoints(doc)) {
        PortInfo info;
        info.name = port.owner + "." + port.port;
        info.type = portTypeName(port.type);
        info.isInput = port.isInput;
        out.ports.push_back(std::move(info));
    }
    // T-193: the declared-port table, for `xtframe-pack --gen-ports`.
    m_report.portTables.push_back(UiPortTable{ui.id, doc.ports(), out.contract});

    const IrView& view = doc.ir();
    out.requiresEffects = !view.style().effects().empty() || !doc.arena().effects().empty();
    out.requiresText = !view.doc().texts().empty();
    const NodeArena& arena = doc.arena();
    for (NodeId node = 0; node < arena.capacity(); ++node) {
        if (!arena.alive(node)) {
            continue;
        }
        const NodeCommon& common = arena.common(node);
        if (common.is3D != 0 || common.isSpaceRoot != 0) {
            out.requiresSpace3d = true;
            break;
        }
    }

    if (!m_options.thumbnails) {
        return;
    }

    std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);
    sw::SwTarget target{pixels.data(), width, height, width * 4};
    CommandList list;
    ctx.render(list);
    sw::SwRenderer renderer;
    renderer.setTarget(target);
    renderer.clear(Color{0, 0, 0, 0});
    renderer.render(list, m_shaper.get(), &assets, &view, &ctx.perf());

    const int thumbW = std::max(1, toInt32Sat(m_project.publish.thumbnail.x));
    const int thumbH = std::max(1, toInt32Sat(m_project.publish.thumbnail.y));
    const std::vector<uint8_t> thumb = downscale(pixels, width, height, thumbW, thumbH);
    const std::string relative = "thumbs/" + ui.id + ".png";
    if (writePngFile(m_outDir / pathFromUtf8(relative), thumb.data(), thumbW, thumbH, thumbW * 4)) {
        out.thumb = relative;
    }
}

void Packer::pullStandardSymbols(const fs::path& docPath) {
    if (m_options.standardSymbolDir.empty()) {
        return;
    }
    std::string text;
    if (!readBytes(docPath, text)) {
        return;  // The compile reports the unreadable document.
    }
    compile::DiagnosticSink sink;
    compile::DomTree dom;
    if (!compile::parseXt(text, pathToUtf8(docPath), sink, dom)) {
        return;  // Likewise for a document that does not parse.
    }
    const std::string symbolsRel = m_ui != nullptr ? m_ui->shared.symbols : std::string("shared/symbols");
    if (symbolsRel.empty()) {
        return;
    }
    std::error_code ec;
    const fs::path root = fs::absolute(pathFromUtf8(m_project.rootDir), ec).lexically_normal();
    const fs::path symbolsDir = (root / pathFromUtf8(symbolsRel)).lexically_normal();
    for (const std::string& href : dom.symbolHrefs) {
        if (href.rfind("xt-standard:", 0) == 0) {
            continue;  // T-235: resolved by the compiler straight from the library, nothing to copy.
        }
        const fs::path target =
            fs::absolute(docPath.parent_path() / pathFromUtf8(href), ec).lexically_normal();
        if (fs::exists(target, ec) || target.parent_path() != symbolsDir) {
            continue;
        }
        // T-241: the library root first, then its `mobile/` subset (23 §23.11.2); T-382: `list/`.
        const fs::path standard =
            detail::findStandardSymbol(pathFromUtf8(m_options.standardSymbolDir), target.filename());
        std::string bytes;
        if (standard.empty() || !readBytes(standard, bytes)) {
            continue;  // Not a standard symbol: the compile reports the missing file.
        }
        fs::create_directories(symbolsDir, ec);
        if (!writeBytes(target, bytes)) {
            error(href, "cannot write '" + pathToUtf8(target) + "'");
            continue;
        }
        m_report.standardSymbolsCopied.push_back(genericPathToUtf8(target.lexically_relative(root)));
    }
}

bool Packer::packUi(const ProjectUi& ui, UiInfo& out) {
    std::vector<std::string> uiAssets;

    compile::CompileOptions compileOptions;
    compileOptions.rootDir = m_project.rootDir;
    compileOptions.strict = m_options.strict;
    compileOptions.embedImages = m_project.publish.embedImages || m_options.embed;
    compileOptions.embedFonts = m_project.publish.embedFonts || m_options.embed;
    compileOptions.searchDirs = searchDirsFor(ui);                   // T-204
    compileOptions.standardSymbolDir = m_options.standardSymbolDir;  // T-235: `xt-standard:`
    // T-273b: no machine path (or `..` out of the project) in the bundle's `.xtir`.
    compileOptions.publishAssetNames = true;
    // T-342 (03 §3.13, 22 §22.15): `editor-only` subtrees are not published; the thumbnail below
    // renders the packed IR, so it never shows them either.
    compileOptions.stripEditorOnly = true;
    // T-407 (43 §43.3): every UI carries the project's `spec.behavior` in its META.
    compileOptions.behavior = behavior();
    for (const std::string& style : m_project.shared.styles) {
        // Absolute, because compileDocument() resolves a *relative* implied stylesheet against
        // `rootDir` again, which would double the prefix we just added.
        std::error_code ec;
        const fs::path absolute = fs::absolute(pathFromUtf8(m_project.rootDir) / pathFromUtf8(style), ec);
        compileOptions.impliedStylesheets.push_back(pathToUtf8(absolute.lexically_normal()));
    }
    compileOptions.assetNameRewrite = [this, &uiAssets](std::string_view sourceName,
                                                        std::string_view resolvedPath, uint8_t kind) {
        return rewriteAsset(sourceName, resolvedPath, kind, uiAssets);
    };

    const fs::path docPath = pathFromUtf8(m_project.rootDir) / pathFromUtf8(ui.doc);
    pullStandardSymbols(docPath);
    compile::CompileResult result = compile::compileDocument(pathToUtf8(docPath), compileOptions);
    for (compile::Diagnostic& diagnostic : result.diagnostics) {
        const std::string_view message = diagnostic.message;
        const std::string_view tail = compile::kVideoOutsideProjectTail;
        // T-273d: another UI of this run named the same outside video. Under --strict the warning
        // fails each UI that has it, so each keeps its own (it says why that UI failed).
        if (!m_options.strict && diagnostic.level == compile::Diagnostic::Level::Warning &&
            message.rfind("video \"", 0) == 0 && message.size() > tail.size() &&
            message.substr(message.size() - tail.size()) == tail &&
            !m_outsideVideoWarnings.insert(diagnostic.message).second) {
            continue;
        }
        diagnostic.file = ui.id + ": " + diagnostic.file;
        m_report.diagnostics.push_back(std::move(diagnostic));
    }
    if (!result.ok()) {
        return false;
    }

    // T-179: the keys this UI uses and its `@font-fallback` chains (the first UI to declare a
    // language's chain wins; chains are normally shared stylesheet rules anyway).
    {
        Result<IrView> view = IrView::open(IrBlob{result.ir.data(), result.ir.size()});
        if (view.ok()) {
            const LocalizationChunk& l10n = view.value().localization();
            for (uint32_t key : l10n.keys()) {
                m_usedKeys.emplace(view.value().string(key));
            }
            for (const IrFontFallback& fb : l10n.fallbacks()) {
                const std::string lang(view.value().string(fb.lang));
                bool known = false;
                for (const FontFallbackInfo& existing : m_manifest.fontFallbacks) {
                    known = known || existing.lang == lang;
                }
                if (known) {
                    continue;
                }
                FontFallbackInfo info;
                info.lang = lang;
                for (uint32_t i = 0; i < fb.familyCount; ++i) {
                    info.families.emplace_back(
                        view.value().string(l10n.fallbackFamilies()[fb.familyOffset + i]));
                }
                m_manifest.fontFallbacks.push_back(std::move(info));
            }
        }
    }

    // T-262a: a flow the UI names has to be readable and has to parse, as the global flow does
    // (packGlobalFlow); checked before anything of the UI is written.
    // T-359: a UI whose `.xtui` lists no flow uses `<doc dir>/<doc stem>.xtflow` when it exists.
    std::string flowFile = ui.flow;
    if (flowFile.empty()) {
        const fs::path docRelative = pathFromUtf8(ui.doc);
        const fs::path sibling =
            docRelative.parent_path() / pathFromUtf8(pathToUtf8(docRelative.stem()) + ".xtflow");
        std::error_code ec;
        if (fs::is_regular_file(pathFromUtf8(m_project.rootDir) / sibling, ec)) {
            flowFile = genericPathToUtf8(sibling);
            addWarning(ui.id,
                       "flow not listed in the .xtui; using the sibling " + pathToUtf8(sibling.filename()));
        }
    }
    std::string flowBytes;
    if (!flowFile.empty()) {
        const std::string label = ui.id + ": " + flowFile;
        const fs::path flowPath = pathFromUtf8(m_project.rootDir) / pathFromUtf8(flowFile);
        if (!readBytes(flowPath, flowBytes)) {
            error(label, "flow: cannot read '" + pathToUtf8(flowPath) + "'");
            return false;
        }
        flow::FlowGraph graph;
        std::vector<std::string> problems;
        if (flow::FlowGraph::parse(flowBytes, graph, problems) != Status::kOk) {
            for (const std::string& problem : problems) {
                error(label, "flow: " + problem);
            }
            return false;
        }
        warnEditorOnlyFlowReferences(label, graph, result.strippedIds);  // T-342
    }

    const std::string irBytes(reinterpret_cast<const char*>(result.ir.data()), result.ir.size());
    const std::string irRelative = "ui/" + ui.id + ".xtir";
    if (!writeBytes(m_outDir / pathFromUtf8(irRelative), irBytes)) {
        error(ui.id, "cannot write '" + pathToUtf8(m_outDir / pathFromUtf8(irRelative)) + "'");
        return false;
    }

    out.id = ui.id;
    out.name = ui.id;
    out.file = irRelative;
    out.size = ui.size;
    out.tags = ui.tags;
    out.fit = ui.fit;
    out.assets = uiAssets;
    out.fileHash = irFileHash(result.ir);  // T-200: the META compile time does not count.

    if (!flowFile.empty()) {
        const std::string flowRelative = "ui/" + ui.id + ".xtflow";
        if (!writeBytes(m_outDir / pathFromUtf8(flowRelative), flowBytes)) {
            error(ui.id, "cannot write '" + pathToUtf8(m_outDir / pathFromUtf8(flowRelative)) + "'");
            return false;
        }
        out.flow = flowRelative;
    }

    // The document is loaded from the packed bytes with an AssetResolver rooted at the output
    // directory, so reflection and the thumbnail see exactly what a host will see.
    FileAssetResolver assets(genericPathToUtf8(m_outDir), sw::decodeImageBytes);
    Context ctx(ContextConfig{m_shaper.get(), &assets});
    Result<DocumentHandle> loaded =
        ctx.loadDocument(IrBlob{result.ir.data(), result.ir.size()}, /*takeOwnership=*/false);
    if (!loaded.ok()) {
        error(ui.id, "cannot load the packed '" + irRelative + "'");
        return false;
    }
    Document* doc = ctx.document(loaded.value());
    if (doc == nullptr) {
        error(ui.id, "cannot load the packed '" + irRelative + "'");
        return false;
    }
    describeUi(ctx, assets, *doc, ui, out);
    collectImageUses(*doc, doc->ir(), ui.id);  // T-199
    return true;
}

void Packer::packTokens() {
    auto warn = [this](const std::string& file, const std::string& message) {
        compile::Diagnostic diagnostic;
        diagnostic.level = compile::Diagnostic::Level::Warning;
        diagnostic.file = file;
        diagnostic.message = message;
        m_report.diagnostics.push_back(std::move(diagnostic));
    };

    std::map<std::string, std::string> base;  // Later stylesheet wins, as in compileDocument().
    for (const std::string& style : m_project.shared.styles) {
        std::string text;
        if (!readBytes(pathFromUtf8(m_project.rootDir) / pathFromUtf8(style), text)) {
            continue;  // Every UI's compile already reports the unreadable stylesheet.
        }
        compile::DiagnosticSink sink;
        compile::RuleSet rules;
        compile::parseXts(text, style, 1, sink, rules);
        for (const compile::TokenDecl& token : rules.tokens) {
            base[token.name] = token.value;
        }
    }
    for (const auto& [name, value] : base) {
        m_manifest.tokens.push_back(TokenDef{name, value});
    }

    const fs::path themeDir = pathFromUtf8(m_project.rootDir) / themesDir();
    std::error_code ec;
    if (!fs::is_directory(themeDir, ec)) {
        return;
    }
    std::vector<fs::path> files;
    for (fs::directory_iterator it(themeDir, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        if (it->is_regular_file(ec) && lowerExtension(pathToUtf8(it->path().filename())) == ".xts") {
            files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        return;
    }
    fs::create_directories(m_outDir / "themes", ec);
    for (const fs::path& file : files) {
        const std::string relative = "themes/" + pathToUtf8(file.filename());
        std::string text;
        if (!readBytes(file, text)) {
            warn(relative, "cannot read theme file");
            continue;
        }
        compile::DiagnosticSink sink;
        compile::RuleSet rules;
        compile::parseXts(text, relative, 1, sink, rules);
        bool extra = !rules.keyframes.empty() || !rules.fontFaces.empty();
        for (const compile::StyleRule& rule : rules.rules) {
            extra = extra || !rule.decls.empty();
        }
        if (extra) {
            warn(relative, "a theme only sets tokens (--name: value); its other rules are ignored");
        }
        if (rules.tokens.empty()) {
            warn(relative, "theme declares no tokens");
        }
        if (!writeBytes(m_outDir / pathFromUtf8(relative), text)) {
            error(m_project.id, "cannot write '" + pathToUtf8(m_outDir / pathFromUtf8(relative)) + "'");
            continue;
        }
        m_manifest.themes.push_back(ThemeInfo{pathToUtf8(file.stem()), relative});
    }
}

void Packer::packStrings(bool incremental, const ManifestData& previous) {
    // An incremental run only saw the re-packed UIs' chains; keep the other languages' ones.
    if (incremental) {
        for (const FontFallbackInfo& old : previous.fontFallbacks) {
            bool known = false;
            for (const FontFallbackInfo& existing : m_manifest.fontFallbacks) {
                known = known || existing.lang == old.lang;
            }
            if (!known) {
                m_manifest.fontFallbacks.push_back(old);
            }
        }
    }

    const fs::path stringsRoot = pathFromUtf8(m_project.rootDir) / stringsDir();
    std::vector<fs::path> files;
    std::error_code ec;
    if (fs::is_directory(stringsRoot, ec)) {
        for (fs::directory_iterator it(stringsRoot, ec), end; it != end; it.increment(ec)) {
            if (ec) {
                break;
            }
            if (it->is_regular_file(ec) && lowerExtension(pathToUtf8(it->path().filename())) == ".csv") {
                files.push_back(it->path());
            }
        }
    }
    std::sort(files.begin(), files.end());
    for (const std::string& lang : languages()) {
        bool found = false;
        for (const fs::path& file : files) {
            found = found || pathToUtf8(file.stem()) == lang;
        }
        if (!found) {
            addWarning(stringsDir() + "/" + lang + ".csv", "i18n: --lang " + lang + " has no strings table");
        }
    }

    bool createdDir = false;
    for (const fs::path& file : files) {
        const std::string lang = pathToUtf8(file.stem());
        if (!languages().empty() &&
            std::find(languages().begin(), languages().end(), lang) == languages().end()) {
            continue;
        }
        const std::string relative = "strings/" + pathToUtf8(file.filename());
        std::string text;
        if (!readBytes(file, text)) {
            error(relative, "cannot read strings table");
            continue;
        }
        std::vector<StringsRow> rows;
        std::string message;
        if (!parseStringsCsv(text, rows, &message)) {
            error(relative, "malformed strings table: " + message);
            continue;
        }

        // Lint (12.7 `i18n`).
        std::map<std::string, std::string> table;
        for (const StringsRow& row : rows) {
            if (table.count(row.key) != 0) {
                addWarning(relative,
                           "i18n: line " + std::to_string(row.line) + ": key '" + row.key + "' repeated");
            }
            table[row.key] = row.text;
            if (m_fontSubset) {  // T-199: the subset covers every packed language.
                appendCodePoints(row.text, m_charset);
                appendCodePoints(row.text, m_langChars[lang]);
            }
        }
        for (const std::string& key : m_usedKeys) {
            auto it = table.find(key);
            if (it == table.end() || it->second.empty()) {
                addWarning(relative, "i18n: untranslated key '" + key + "'");
            }
        }
        if (!incremental) {
            for (const auto& [key, value] : table) {
                if (m_usedKeys.count(key) == 0) {
                    addWarning(relative, "i18n: unused key '" + key + "'");
                }
            }
        }

        if (!createdDir) {
            fs::create_directories(m_outDir / "strings", ec);
            createdDir = true;
        }
        if (!writeBytes(m_outDir / pathFromUtf8(relative), text)) {
            error(m_project.id, "cannot write '" + pathToUtf8(m_outDir / pathFromUtf8(relative)) + "'");
            continue;
        }
        m_manifest.strings.push_back(StringsInfo{lang, relative});
    }
}

void Packer::packProjectExtras(bool incremental) {
    if (m_ui == nullptr) {
        return;
    }
    m_manifest.hasProject = true;
    m_manifest.project.name = m_ui->name;
    m_manifest.project.game = m_ui->game;
    m_manifest.project.version = m_ui->version;
    m_manifest.project.commit = m_options.projectCommit;          // T-314
    m_manifest.project.checkpoint = m_options.projectCheckpoint;  // T-314
    // T-207 (§22.10 `spec`): the safe area of the .xtui; T-210 (25 §25.4.1 item 2): the
    // logical height is its spec.logicalHeight (default 1080).
    m_manifest.hasSpec = true;
    m_manifest.spec.safeAreaDefault = m_ui->spec.safeAreaDefault;
    m_manifest.spec.safeAreaConsole = m_ui->spec.safeAreaConsole;
    m_manifest.spec.logicalHeight = m_ui->spec.logicalHeight;
    m_manifest.spec.uiscaleRule = m_ui->spec.uiscaleRule;      // T-241
    m_manifest.spec.uiscaleLimits = m_ui->spec.uiscaleLimits;  // T-345
    m_manifest.spec.behavior = m_ui->spec.behavior;            // T-407

    const fs::path root = pathFromUtf8(m_project.rootDir);
    std::error_code ec;
    const fs::path motionPath = root / "contracts" / "motion-spec.yaml";
    if (fs::is_regular_file(motionPath, ec)) {
        std::string text;
        nlohmann::json motion;
        std::string message;
        if (!readBytes(motionPath, text)) {
            error("contracts/motion-spec.yaml", "spec: cannot read the motion spec");
        } else if (!detail::parseYamlLite(text, motion, &message)) {
            error("contracts/motion-spec.yaml", "spec: malformed motion spec: " + message);
        } else if (motion.is_object()) {
            // T-406 (43 §43.2): `version` missing = 1, newer refused; the manifest carries the
            // spec without it.
            if (migrateToLatest(SourceFormat::DesignYaml, motion, nullptr, &message) != Status::kOk) {
                error("contracts/motion-spec.yaml", "spec: " + message);
            } else {
                motion.erase("version");
                m_manifest.motionSpecJson = motion.dump();
            }
        } else if (!motion.is_null()) {
            error("contracts/motion-spec.yaml", "spec: the motion spec is not a mapping");
        }
    }

    const fs::path assetsPath = root / kAssetsManifestPath;
    if (!fs::is_regular_file(assetsPath, ec)) {
        return;
    }
    std::string message;
    Result<AssetsManifest> loaded = AssetsManifest::load(pathToUtf8(assetsPath), &message);
    if (!loaded.ok()) {
        error(kAssetsManifestPath, "assets: " + message);
        return;
    }
    AssetsManifest& assets = loaded.value();
    // Embedded assets never reach assetNameRewrite, so only a full, non-embedding run knows the
    // complete set of referenced files.
    const bool embeds = m_project.publish.embedImages || m_project.publish.embedFonts || m_options.embed;
    const bool checkUnreferenced = !incremental && !embeds;
    bool filled = false;
    // T-312a (35 §35.15): a placeholder asset stops the pack unless allowed; checkAssets()'s own
    // "status is not draft, ..." error for it is not repeated.
    std::set<std::string> placeholders;
    for (const AssetEntry& entry : assets.assets) {
        if (entry.status != "placeholder") {
            continue;
        }
        placeholders.insert(entry.id);
        const std::string text = "assets: " + entry.id + ": asset.placeholderShipped: '" + entry.file +
                                 "' is a placeholder" +
                                 (m_options.allowPlaceholders ? " (shipped: --allow-placeholders)"
                                                              : "; replace it or pass --allow-placeholders");
        if (m_options.allowPlaceholders) {
            addWarning(kAssetsManifestPath, text);
        } else {
            error(kAssetsManifestPath, text);
        }
    }
    for (const AssetIssue& issue :
         checkAssets(assets, m_project.rootDir, checkUnreferenced ? &m_referenced : nullptr, &filled)) {
        if (issue.isError && placeholders.count(issue.id) != 0 &&
            issue.message.rfind("status 'placeholder'", 0) == 0) {
            continue;
        }
        const std::string text =
            "assets: " + (issue.id.empty() ? std::string() : issue.id + ": ") + issue.message;
        if (issue.isError) {
            error(kAssetsManifestPath, text);
        } else {
            addWarning(kAssetsManifestPath, text);
        }
    }
    if (filled && m_options.writeAssetsManifest && assets.hasUnsavableContent) {
        // T-262b (F10): rewriting would drop what save() cannot reproduce; no in-place patching.
        addWarning(
            kAssetsManifestPath,
            "assets.yaml not updated: it has comments or structured values the writer cannot preserve");
    } else if (filled && m_options.writeAssetsManifest) {
        if (assets.save(pathToUtf8(assetsPath)) == Status::kOk) {
            m_report.assetsManifestUpdated = true;
        } else {
            error(kAssetsManifestPath, "assets: cannot write back the measured fields");
        }
    }

    // §22.10 `assets.manifest`: the 26.3.3 subset for every packed asset the manifest lists.
    std::unordered_map<std::string, const AssetEntry*> byPath;
    for (const AssetEntry& entry : assets.assets) {
        if (!entry.file.empty()) {
            byPath.emplace(canonicalAssetPath(pathToUtf8(root / pathFromUtf8(entry.file))), &entry);
        }
    }
    for (ManifestAsset& asset : m_manifest.assets) {
        const auto source = m_assetSourcePath.find(asset.file);
        if (source == m_assetSourcePath.end()) {
            continue;  // Kept from a previous (incremental) run: its recorded entry stays.
        }
        const auto entry = byPath.find(source->second);
        if (entry == byPath.end()) {
            asset.hasManifest = false;
            continue;
        }
        asset.hasManifest = true;
        asset.manifestId = entry->second->id;
        asset.measured = entry->second->measured;
    }
}

void Packer::packGlobalFlow() {
    const fs::path root = pathFromUtf8(m_project.rootDir);
    std::error_code ec;
    fs::path source = root / "bundle.xtflow";
    if (!fs::is_regular_file(source, ec)) {
        source = root / "shared" / "bundle.xtflow";
        if (!fs::is_regular_file(source, ec)) {
            return;
        }
    }
    const std::string label = genericPathToUtf8(fs::relative(source, root, ec));
    std::string text;
    if (!readBytes(source, text)) {
        error(label, "flow: cannot read the global flow");
        return;
    }
    flow::FlowGraph graph;
    std::vector<std::string> problems;
    if (flow::FlowGraph::parse(text, graph, problems) != Status::kOk) {
        for (const std::string& problem : problems) {
            error(label, "flow: " + problem);
        }
        return;
    }
    // Every `ui#<id>` has to name a UI of this project (the ports themselves are checked when a
    // host binds the flow: FlowRuntime::loadGlobal()).
    bool known = true;
    auto checkUi = [&](const std::string& endpoint) {
        if (endpoint.compare(0, 3, "ui#") != 0) {
            return;
        }
        const std::string uiId = endpoint.substr(
            3, endpoint.find('.') == std::string::npos ? std::string::npos : endpoint.find('.') - 3);
        if (m_project.ui(uiId) == nullptr) {
            error(label, "flow: unknown UI '" + uiId + "' in '" + endpoint + "'");
            known = false;
        }
    };
    for (const flow::EdgeDesc& edge : graph.edges) {
        checkUi(edge.from);
        checkUi(edge.to);
    }
    if (!known) {
        return;
    }
    const std::string relative = "bundle.xtflow";
    if (!writeBytes(m_outDir / pathFromUtf8(relative), text)) {
        error(label, "cannot write '" + pathToUtf8(m_outDir / pathFromUtf8(relative)) + "'");
        return;
    }
    m_manifest.globalFlow = relative;
    m_report.globalFlowWritten = true;
}

void Packer::prune() {
    std::unordered_set<std::string> referenced;
    for (const ManifestAsset& asset : m_manifest.assets) {
        referenced.insert(asset.file);
        if (!asset.variant2x.empty()) {
            referenced.insert(asset.variant2x);  // T-199
        }
        if (!asset.variant3x.empty()) {
            referenced.insert(asset.variant3x);  // T-345
        }
    }
    for (const ManifestAtlas& atlas : m_manifest.atlases) {  // T-199
        referenced.insert(atlas.file);
        if (!atlas.file2x.empty()) {
            referenced.insert(atlas.file2x);
        }
    }
    for (const UiInfo& ui : m_manifest.uis) {
        for (const std::string* name : {&ui.file, &ui.flow, &ui.thumb}) {
            if (!name->empty()) {
                referenced.insert(*name);
            }
        }
        referenced.insert(ui.assets.begin(), ui.assets.end());  // T-262a
    }

    for (const ThemeInfo& theme : m_manifest.themes) {
        referenced.insert(theme.file);
    }
    for (const StringsInfo& strings : m_manifest.strings) {
        referenced.insert(strings.file);
    }

    for (const char* folder : {"assets", "ui", "thumbs", "themes", "strings", "atlas", "fonts"}) {
        const fs::path dir = m_outDir / folder;
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) {
            continue;
        }
        std::vector<fs::path> doomed;
        for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
            if (ec) {
                break;
            }
            if (!it->is_regular_file(ec)) {
                continue;
            }
            const std::string relative = std::string(folder) + "/" + pathToUtf8(it->path().filename());
            if (referenced.find(relative) == referenced.end()) {
                doomed.push_back(it->path());
            }
        }
        for (const fs::path& path : doomed) {
            if (fs::remove(path, ec)) {
                ++m_report.assetsPruned;
            }
        }
    }
}

PackReport Packer::run() {
    m_report.ok = true;
    if (m_options.outDir.empty()) {
        error(m_project.id, "pack: no output directory");
        return std::move(m_report);
    }
    for (const std::string& id : m_options.onlyUis) {
        if (m_project.ui(id) == nullptr) {
            error(m_project.id, "no UI with id '" + id + "'");
        }
    }
    if (!m_report.ok) {
        return std::move(m_report);
    }

    m_outDir = pathFromUtf8(m_options.outDir);
    std::error_code ec;
    fs::create_directories(m_outDir / "ui", ec);
    fs::create_directories(m_outDir / "assets", ec);
    if (m_options.thumbnails) {
        fs::create_directories(m_outDir / "thumbs", ec);
    }
    if (!fs::is_directory(m_outDir, ec)) {
        error(m_project.id, "cannot create '" + pathToUtf8(m_outDir) + "'");
        return std::move(m_report);
    }

    // T-199: the asset pipeline options (CLI first, then the UI Project's `publish.atlas`).
    if (!parseBcFormat(m_options.compress, m_compress)) {
        error(m_project.id,
              "pack: unknown --compress format '" + m_options.compress + "' (bc7|bc3|bc1|none)");
        return std::move(m_report);
    }
    m_atlas = m_options.atlas || (m_ui != nullptr && m_ui->publish.atlas.enabled);
    m_fontSubset = m_options.fontSubset;
    if (m_ui != nullptr) {
        m_atlasMaxSize = m_ui->publish.atlas.maxSize;
        m_atlasPadding = m_ui->publish.atlas.padding;
        m_atlasMipmaps = m_ui->publish.atlas.mipmaps;
    }
    const std::string scope = !m_options.atlasScope.empty() ? m_options.atlasScope
                              : m_ui != nullptr             ? m_ui->publish.atlas.scope
                                                            : std::string("ui");
    m_atlasBundleScope = scope == "bundle";
    if ((m_atlas || m_fontSubset) && !m_options.onlyUis.empty()) {
        error(m_project.id, "pack: --atlas / --font-subset need a full pack (no --ui)");
        return std::move(m_report);
    }

    // An incremental run (`--ui`) keeps the manifest's other UIs and its whole asset table; a full
    // run rebuilds both from scratch, so assets nothing references any more simply drop out.
    const bool incremental = !m_options.onlyUis.empty();
    ManifestData previous;
    const bool hadManifest = readManifest(genericPathToUtf8(m_outDir), previous);
    if (incremental && hadManifest) {
        // T-262a: the kept UIs name atlas cells and subset fonts this run cannot rebuild.
        bool pipelineBuilt = !previous.atlases.empty();
        for (const ManifestAsset& asset : previous.assets) {
            pipelineBuilt =
                pipelineBuilt || (asset.kind == "font" && asset.file.compare(0, 6, "fonts/") == 0);
        }
        if (pipelineBuilt) {
            error(m_project.id,
                  "incremental pack is not supported for a bundle built with --atlas or --font-subset; run a "
                  "full pack");
            return std::move(m_report);
        }
    }
    if (m_ui != nullptr && m_compress != BcFormat::None) {
        // Per-asset `compress:` overrides from manifest/assets.yaml (26.3.4 "quality threshold").
        const fs::path assetsPath = pathFromUtf8(m_project.rootDir) / kAssetsManifestPath;
        if (fs::is_regular_file(assetsPath, ec)) {
            Result<AssetsManifest> loaded = AssetsManifest::load(pathToUtf8(assetsPath));
            if (loaded.ok()) {
                for (const AssetEntry& entry : loaded.value().assets) {
                    for (const auto& [key, value] : entry.extra) {
                        if (key == "compress" && !entry.file.empty()) {
                            m_compressOverride[canonicalAssetPath(pathToUtf8(
                                pathFromUtf8(m_project.rootDir) / pathFromUtf8(entry.file)))] = value;
                        }
                    }
                }
            }
        }
    }

    if (incremental && hadManifest) {
        m_manifest.assets = previous.assets;
        for (size_t i = 0; i < m_manifest.assets.size(); ++i) {
            m_assetIndex.emplace(m_manifest.assets[i].file, i);
        }
    }

    std::vector<size_t> kept;  // T-262a: indices into m_manifest.uis of entries taken from `previous`.
    for (const ProjectUi& ui : m_project.uis) {
        const UiInfo* keep = nullptr;
        for (const UiInfo& old : previous.uis) {
            if (old.id == ui.id) {
                keep = &old;
                break;
            }
        }
        if (incremental && !wanted(ui.id)) {
            if (keep != nullptr) {
                kept.push_back(m_manifest.uis.size());
                m_manifest.uis.push_back(*keep);
            }
            continue;
        }
        UiInfo info;
        if (packUi(ui, info)) {
            m_manifest.uis.push_back(std::move(info));
            m_report.packedUis.push_back(ui.id);
        } else {
            // A UI that failed to compile is not written out; an entry a previous run left behind
            // stays valid (its files are still there), so it is kept rather than dropped.
            m_report.ok = false;
            if (keep != nullptr) {
                kept.push_back(m_manifest.uis.size());
                m_manifest.uis.push_back(*keep);
            }
        }
    }

    packGlyphs();  // T-204: before the assets.yaml lint, so glyph files count as referenced.
    // T-262a: a kept entry still names its files, so their rows come back from the previous
    // manifest (once per file name); otherwise `assets[]` misses files a listed UI uses.
    for (const size_t index : kept) {
        for (const std::string& file : m_manifest.uis[index].assets) {
            if (m_assetIndex.count(file) != 0) {
                continue;
            }
            for (const ManifestAsset& asset : previous.assets) {
                if (asset.file == file) {
                    m_assetIndex.emplace(file, m_manifest.assets.size());
                    m_manifest.assets.push_back(asset);
                    break;
                }
            }
        }
    }
    packTokens();
    packStrings(incremental, previous);
    packProjectExtras(incremental);
    packGlobalFlow();
    // T-199: atlas pages and font subsets change the names the `.xtir` files carry, so the UIs
    // are compiled a second time once the pages are known (pass 2, rewriteUis).
    if (m_report.ok && !incremental && (m_atlas || m_fontSubset)) {
        if (m_atlas) {
            buildAtlases();
            if (deferCompress()) {
                compressDeferred();  // T-204: after the pages, on what is left.
            }
        }
        if (m_fontSubset) {
            subsetFonts();
        }
        if (m_report.ok) {
            rewriteUis();
        }
        // T-262a: the replaced files go only when every `.xtir` names the pages and subsets.
        if (m_report.ok) {
            for (const fs::path& path : m_pendingRemovals) {
                fs::remove(path, ec);
            }
        }
        m_pendingRemovals.clear();
    }
    packSymbolContracts();
    m_manifest.bundleId = m_project.id;
    m_manifest.bundleName = m_project.name;
    m_manifest.builtAt = isoUtcNow();
    m_manifest.xtframe = versionString();
    m_manifest.irMajor = static_cast<int>(kIrFormatMajor);
    if (writeManifest(genericPathToUtf8(m_outDir), m_manifest) != Status::kOk) {
        error(m_project.id, "cannot write '" + pathToUtf8(m_outDir / "manifest.json") + "'");
        return std::move(m_report);
    }

    if (m_options.prune && m_report.ok) {  // T-262a: a failed run deletes nothing.
        prune();
    }
    return std::move(m_report);
}

}  // namespace

PackReport pack(const ProjectFile& project, const PackOptions& options) {
    Packer packer(project, options);
    return packer.run();
}

void Packer::packGlyphs() {
    const fs::path root = pathFromUtf8(m_project.rootDir);
    std::vector<fs::path> bases;
    if (!m_project.shared.assetRoot.empty()) {
        bases.push_back(root / pathFromUtf8(m_project.shared.assetRoot));
    }
    bases.push_back(root);
    std::error_code ec;
    std::set<std::pair<std::string, std::string>> seen;
    std::vector<std::string> unused;
    for (const fs::path& base : bases) {
        const fs::path glyphRoot = base / "glyphs";
        if (!fs::is_directory(glyphRoot, ec)) {
            continue;
        }
        std::vector<fs::path> devices;
        for (fs::directory_iterator it(glyphRoot, ec), end; it != end; it.increment(ec)) {
            if (ec) {
                break;
            }
            if (it->is_directory(ec)) {
                devices.push_back(it->path());
            }
        }
        std::sort(devices.begin(), devices.end());
        for (const fs::path& deviceDir : devices) {
            const std::string device = pathToUtf8(deviceDir.filename());
            if (device != "mouse" && device != "gamepad" && device != "touch" && device != "keyboard") {
                addWarning("glyphs/" + device,
                           "glyphs: unknown device '" + device + "' (mouse|gamepad|touch|keyboard); skipped");
                continue;
            }
            std::vector<fs::path> files;
            for (fs::directory_iterator it(deviceDir, ec), end; it != end; it.increment(ec)) {
                if (ec) {
                    break;
                }
                if (it->is_regular_file(ec) && isRasterExtension(lowerExtension(pathToUtf8(it->path())))) {
                    files.push_back(it->path());
                }
            }
            std::sort(files.begin(), files.end());
            for (const fs::path& file : files) {
                const std::string stem = pathToUtf8(file.stem());
                if (stem.size() > 3 && stem.compare(stem.size() - 3, 3, "@2x") == 0) {
                    continue;  // A `@2x` variant rides along with its 1x file (rewriteAsset).
                }
                if (!seen.insert({device, stem}).second) {
                    continue;
                }
                const std::string sourceName = "glyphs/" + device + "/" + pathToUtf8(file.filename());
                const std::string asset = rewriteAsset(sourceName, pathToUtf8(file), kAssetKindImage, unused);
                if (asset.empty()) {
                    addWarning(sourceName, "glyphs: cannot pack the glyph image");
                    continue;
                }
                m_glyphAssets.insert(asset);
                m_manifest.glyphs.push_back(GlyphInfo{device, stem, asset});
            }
        }
    }
    std::sort(m_manifest.glyphs.begin(), m_manifest.glyphs.end(), [](const GlyphInfo& a, const GlyphInfo& b) {
        return a.device != b.device ? a.device < b.device : a.key < b.key;
    });
}

void Packer::packSymbolContracts() {
    std::vector<std::string> docs;
    for (const ProjectUi& ui : m_project.uis) {
        docs.push_back(ui.doc);
    }
    m_manifest.symbols = collectSymbolContracts(
        m_project.rootDir, m_ui != nullptr ? m_ui->shared.symbols : "shared/symbols", docs);
}

uint64_t irFileHash(std::span<const uint8_t> ir) {
    std::string bytes(reinterpret_cast<const char*>(ir.data()), ir.size());
    Result<IrView> view = IrView::open(IrBlob{ir.data(), ir.size()});
    if (view.ok() && view.value().meta().present() && view.value().meta().meta() != nullptr) {
        // The compile time, and the META chunk header's payload CRC (which covers it).
        const IrMeta* meta = view.value().meta().meta();
        const uint8_t* at = reinterpret_cast<const uint8_t*>(&meta->compileTimeUnix);
        const uint8_t* crc = reinterpret_cast<const uint8_t*>(meta) - sizeof(uint32_t);
        if (crc >= ir.data() + sizeof(IrHeader) && at + sizeof(uint64_t) <= ir.data() + ir.size()) {
            std::fill_n(bytes.begin() + (at - ir.data()), sizeof(uint64_t), '\0');
            std::fill_n(bytes.begin() + (crc - ir.data()), sizeof(uint32_t), '\0');
        }
    }
    return fnv1a64(bytes);
}

PackReport pack(const UiProject& project, const PackOptions& options) {
    const ProjectFile bundleProject = project.toBundleProject();
    if (project.legacy) {
        return pack(bundleProject, options);
    }
    Packer packer(bundleProject, options, &project);
    return packer.run();
}

}  // namespace xt::bundle
