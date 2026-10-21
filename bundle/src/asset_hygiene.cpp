#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "asset_internal.h"
#include "bundle_json.h"
#include "design_internal.h"
#include "xt/bundle/asset_meta.h"
#include "xt/compile/utf8_path.h"

/// The asset cleanup numbers (docs/design/35-design-system.md §35.15.1 item 5; docs/tasks/T-312a.md
/// item 5). See xt/bundle/asset_meta.h. Nothing here writes a file.

namespace xt::bundle::assets {

namespace fs = std::filesystem;
using compile::genericPathToUtf8;
using compile::pathFromUtf8;
using compile::pathToUtf8;

namespace {

/// The `.xt` / `.xts` files under `dir` (recursively), sorted; skips `.xtvc`, `.git`, `.xtcache` and
/// the publish output directory (as the design document's scan, T-309).
std::vector<fs::path> sourceFilesUnder(const UiProject& project, const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return out;
    }
    const fs::path root = pathFromUtf8(project.rootDir);
    const fs::path outDir = project.publish.outDir.empty()
                                ? fs::path()
                                : (root / pathFromUtf8(project.publish.outDir)).lexically_normal();
    for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        const std::string name = pathToUtf8(it->path().filename());
        if (it->is_directory(ec)) {
            if (name == ".xtvc" || name == ".git" || name == ".xtcache" ||
                (!outDir.empty() && it->path().lexically_normal() == outDir)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        const std::string ext = detail::lowerExt(name);
        if (it->is_regular_file(ec) && (ext == ".xt" || ext == ".xts")) {
            out.push_back(it->path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string readAll(const std::vector<fs::path>& files) {
    std::string corpus;
    for (const fs::path& file : files) {
        std::string text;
        if (bundle::detail::readFile(pathToUtf8(file), text)) {
            corpus += text;
            corpus += '\n';
        }
    }
    return corpus;
}

bool isBitmap(std::string_view ext) { return ext == ".png" || ext == ".jpg" || ext == ".jpeg"; }

std::string parentOf(const std::string& rel) {
    const size_t slash = rel.rfind('/');
    return slash == std::string::npos ? std::string() : rel.substr(0, slash);
}

struct Info {
    const detail::ScannedAsset* scanned = nullptr;
    uint64_t bytes = 0;
    std::string hash;
    AssetMeta meta;
};

}  // namespace

Status assetHygiene(const UiProject& project, const design::Graph* graph, AssetHygiene& out) {
    out = AssetHygiene{};
    if (project.isUnnamed()) {
        return Status::kInvalidArgument;
    }
    const fs::path root = pathFromUtf8(project.rootDir);
    const std::vector<detail::ScannedAsset> scanned = detail::scanAssets(project);
    const std::string corpus = readAll(sourceFilesUnder(project, root));
    std::string graphText;
    if (graph != nullptr) {
        for (const std::string& node : graph->nodes) {
            graphText += node;
            graphText += '\n';
        }
        for (const design::Edge& edge : graph->edges) {
            graphText += edge.to;
            graphText += '\n';
        }
    }

    std::vector<Info> infos;
    std::set<std::string> with2x;  // 1x files that have an @2x variant
    for (const detail::ScannedAsset& asset : scanned) {
        if (asset.variant2x) {
            const std::string stem = detail::stemOf(asset.rel);
            const std::string parent = parentOf(asset.rel);
            const std::string name = detail::fileNameOf(asset.rel);
            with2x.insert((parent.empty() ? std::string() : parent + "/") + stem.substr(0, stem.size() - 3) +
                          name.substr(stem.size()));
            continue;
        }
        Info info;
        info.scanned = &asset;
        const std::string path = detail::absolutePath(project, asset.rel);
        std::string bytes;
        bundle::detail::readFile(path, bytes);
        info.bytes = bytes.size();
        info.hash = design::detail::contentHash(bytes);
        if (readMeta(path, info.meta) != Status::kOk) {
            info.meta = detail::freshMeta(project, asset.rel, bytes);
        }
        infos.push_back(std::move(info));
    }

    // Existing hand-written slices, for files whose meta has none (a project without metas).
    std::map<std::string, AssetMeasured> manifestSlices;
    {
        Result<AssetsManifest> manifest =
            AssetsManifest::load(detail::absolutePath(project, kAssetsManifestPath));
        if (manifest.ok()) {
            for (const AssetEntry& entry : manifest.value().assets) {
                if (entry.measured.hasSlice) {
                    manifestSlices[entry.file] = entry.measured;
                }
            }
        }
    }

    std::map<std::string, std::string> firstByHash;
    std::map<std::pair<std::string, std::string>, CollectionStat> collections;
    std::map<std::string, std::vector<const Info*>> bitmapsByDir;
    std::set<std::string> dirsWith2x;
    AssetsManifest sliceCheck;
    for (const Info& info : infos) {
        const std::string& rel = info.scanned->rel;
        const std::string name = detail::fileNameOf(rel);
        ++out.totalAssets;
        out.totalBytes += info.bytes;
        const std::string typeDir = typeDirectory(info.scanned->type);
        CollectionStat& stat = collections[{typeDir, collectionOf(project, rel)}];
        stat.typeDir = typeDir;
        stat.collection = collectionOf(project, rel);
        ++stat.assets;
        stat.bytes += info.bytes;

        const bool usedInGraph =
            !graphText.empty() &&
            (graphText.find(rel) != std::string::npos ||
             (!info.meta.id.empty() && graphText.find(info.meta.id) != std::string::npos));
        if (corpus.find(name) == std::string::npos && !usedInGraph) {
            out.unusedFiles.push_back(rel);
        }
        const auto [first, inserted] = firstByHash.emplace(info.hash, rel);
        if (!inserted) {
            out.duplicatePairs.emplace_back(first->second, rel);
        }
        if (project.spec.budgetAssetBytes > 0 && info.bytes > project.spec.budgetAssetBytes) {
            out.oversizeFiles.push_back(rel);
        }
        std::string stem = detail::stemOf(rel);
        if (detail::has2xSuffix(stem)) {
            stem.resize(stem.size() - 3);
        }
        if (stem != kebabAssetId(stem)) {
            ++out.badName;
        }
        if (info.meta.status == "placeholder") {
            out.placeholderFiles.push_back(rel);
        }
        if (isBitmap(info.scanned->ext)) {
            bitmapsByDir[parentOf(rel)].push_back(&info);
            if (with2x.count(rel) != 0) {
                dirsWith2x.insert(parentOf(rel));
            }
        }
        AssetEntry entry;
        entry.id = rel;
        entry.file = rel;
        int slice[4] = {0, 0, 0, 0};
        if (detail::sliceOf(info.meta, slice)) {
            entry.measured.hasSlice = true;
            std::copy(slice, slice + 4, entry.measured.slice);
        } else if (const auto it = manifestSlices.find(rel); it != manifestSlices.end()) {
            entry.measured.hasSlice = true;
            std::copy(it->second.slice, it->second.slice + 4, entry.measured.slice);
        }
        if (entry.measured.hasSlice) {
            sliceCheck.assets.push_back(std::move(entry));
        }
    }
    for (const auto& [dir, bitmaps] : bitmapsByDir) {
        if (dirsWith2x.count(dir) == 0) {
            continue;
        }
        for (const Info* info : bitmaps) {
            if (with2x.count(info->scanned->rel) == 0) {
                ++out.missing2x;
            }
        }
    }
    for (const AssetIssue& issue : checkAssets(sliceCheck, project.rootDir, nullptr, nullptr)) {
        if (issue.isError && issue.message.rfind("slice", 0) == 0) {
            ++out.badSlice;
        }
    }

    // Bytes per UI: the assets named by its document and the sources under its directory.
    for (const ProjectUi& ui : project.uis) {
        const fs::path doc = root / pathFromUtf8(ui.doc);
        std::vector<fs::path> files = sourceFilesUnder(project, doc.parent_path());
        if (std::find(files.begin(), files.end(), doc) == files.end()) {
            files.push_back(doc);
        }
        for (const std::string& style : project.shared.styles) {
            files.push_back(root / pathFromUtf8(style));
        }
        const std::string text = readAll(files);
        uint64_t total = 0;
        for (const Info& info : infos) {
            if (text.find(detail::fileNameOf(info.scanned->rel)) != std::string::npos) {
                total += info.bytes;
            }
        }
        out.bytesPerUi[ui.id] = total;
    }

    for (const auto& entry : collections) {
        out.collections.push_back(entry.second);
    }
    std::sort(out.unusedFiles.begin(), out.unusedFiles.end());
    std::sort(out.oversizeFiles.begin(), out.oversizeFiles.end());
    std::sort(out.placeholderFiles.begin(), out.placeholderFiles.end());
    std::sort(out.duplicatePairs.begin(), out.duplicatePairs.end());
    out.unused = static_cast<int>(out.unusedFiles.size());
    out.duplicates = static_cast<int>(out.duplicatePairs.size());
    out.oversize = static_cast<int>(out.oversizeFiles.size());
    out.placeholders = static_cast<int>(out.placeholderFiles.size());
    return Status::kOk;
}

}  // namespace xt::bundle::assets
