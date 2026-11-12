#include "stale_files.h"

#include <vector>

#include "xt/compile/utf8_path.h"

namespace xt::bundle::detail {

namespace fs = std::filesystem;

namespace {

std::string foldAscii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

/// True when `path` itself (not what it may point to) is a plain directory: false for a symbolic
/// link, a junction (MSVC reports it as its own file type) or anything missing.
bool isPlainDirectory(const fs::path& path) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    return !ec && status.type() == fs::file_type::directory;
}

/// True when `path` itself is a regular file (a link to one is not).
bool isPlainFile(const fs::path& path) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    return !ec && status.type() == fs::file_type::regular;
}

void removeOne(const fs::path& path, StaleRemoval& out) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    const uint64_t bytes = ec ? 0 : static_cast<uint64_t>(size);
    if (fs::remove(path, ec) && !ec) {
        ++out.files;
        out.bytes += bytes;
    }
}

}  // namespace

void KeptBundleFiles::add(std::string_view relative) {
    if (!relative.empty()) {
        m_names.insert(foldAscii(relative));
    }
}

void KeptBundleFiles::addManifest(const ManifestData& manifest) {
    for (const UiInfo& ui : manifest.uis) {
        add(ui.file);
        add(ui.flow);
        add(ui.thumb);
        for (const std::string& asset : ui.assets) {
            add(asset);
        }
    }
    for (const ManifestAsset& asset : manifest.assets) {
        add(asset.file);
        add(asset.variant2x);
        add(asset.variant3x);
    }
    for (const ManifestAtlas& atlas : manifest.atlases) {
        add(atlas.file);
        add(atlas.file2x);
    }
    for (const ThemeInfo& theme : manifest.themes) {
        add(theme.file);
    }
    for (const StringsInfo& strings : manifest.strings) {
        add(strings.file);
    }
    add(manifest.globalFlow);
    for (const GlyphInfo& glyph : manifest.glyphs) {
        add(glyph.asset);
    }
}

bool KeptBundleFiles::contains(std::string_view relative) const {
    return m_names.find(foldAscii(relative)) != m_names.end();
}

StaleRemoval removeStaleFiles(const fs::path& outDir, const KeptBundleFiles& keep,
                              const fs::path& unmanagedDir) {
    StaleRemoval removed;
    std::error_code dirEc;
    if (!fs::is_directory(outDir, dirEc)) {
        return removed;
    }
    for (const char* folder : kManagedBundleDirs) {
        const fs::path dir = outDir / folder;
        if (!isPlainDirectory(dir)) {
            continue;  // Missing, or a link / junction that may lead out of the bundle.
        }
        std::error_code ec;
        if (!unmanagedDir.empty() && fs::equivalent(dir, unmanagedDir, ec) && !ec) {
            continue;  // T-693 item 5: the encode cache was pointed here; not ours to clean.
        }
        std::vector<fs::path> doomed;
        ec.clear();
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const fs::path path = it->path();
            if (!isPlainFile(path)) {
                continue;  // Subdirectories, links and anything else stay.
            }
            const std::string relative = std::string(folder) + "/" + compile::pathToUtf8(path.filename());
            if (!keep.contains(relative)) {
                doomed.push_back(path);
            }
        }
        if (ec) {
            continue;  // A listing that broke off: remove nothing from this directory.
        }
        for (const fs::path& path : doomed) {
            removeOne(path, removed);
        }
    }
    for (const char* name : kKnownBundleRootFiles) {
        const fs::path path = outDir / name;
        if (isPlainFile(path) && !keep.contains(name)) {
            removeOne(path, removed);
        }
    }
    return removed;
}

}  // namespace xt::bundle::detail
