#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <json.hpp>
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
#include "xt/num_guard.h"
#include "xt/sw/sw_png.h"
#include "xt/util/time.h"
#include "yaml_lite.h"

/// The asset import pipeline, replacing, source folders, the generated `manifest/assets.yaml` and
/// promoting a reference (Docs/design/35-design-system.md §35.15, 40 §40.2.1, 26 §26.3.3;
/// Docs/tasks/T-312a.md items 2, 3, 4 and 6). See xt/bundle/asset_meta.h.

namespace xt::bundle::assets {

namespace fs = std::filesystem;
using compile::genericPathToUtf8;
using compile::pathFromUtf8;
using compile::pathToUtf8;
using nlohmann::json;

namespace {

/// How many names an import tries (`<name>`, `<name>-2`, ...) before giving up.
constexpr int kMaxNameTries = 1000;

using util::isoUtcNow;

bool fileExists(const std::string& path) {
    std::error_code ec;
    return fs::exists(pathFromUtf8(path), ec);
}

/// Content hash -> the first asset (in path order) with that content.
struct ContentIndex {
    std::map<std::string, std::string> byHash;
};

ContentIndex buildIndex(const UiProject& project) {
    ContentIndex index;
    for (const detail::ScannedAsset& asset : detail::scanAssets(project)) {
        std::string bytes;
        if (bundle::detail::readFile(detail::absolutePath(project, asset.rel), bytes)) {
            index.byHash.emplace(design::detail::contentHash(bytes), asset.rel);
        }
    }
    return index;
}

/// What importBytes() copies in.
struct Incoming {
    std::string fileName;
    std::string bytes;
    const ImportRequest* request = nullptr;
    std::string license;
    std::vector<design::Link> links;
};

/// `target` with `-<n>` inserted before its `@2x` suffix and extension (n >= 2).
std::string numbered(const std::string& target, int n) {
    if (n <= 1) {
        return target;
    }
    const size_t slash = target.rfind('/');
    const std::string dir = slash == std::string::npos ? std::string() : target.substr(0, slash + 1);
    const std::string ext = detail::lowerExt(target);
    std::string stem = detail::stemOf(target);
    std::string suffix;
    if (detail::has2xSuffix(stem)) {
        suffix = "@2x";
        stem.resize(stem.size() - 3);
    }
    return dir + stem + "-" + std::to_string(n) + suffix + ext;
}

Status importBytes(const UiProject& project, ContentIndex& index, const Incoming& in, ImportResult& result) {
    const ImportRequest& request = *in.request;
    result = ImportResult{};
    if (project.isUnnamed() || !isStatus(request.status)) {
        return Status::kInvalidArgument;
    }
    const std::string ext = detail::lowerExt(in.fileName);
    const std::string type = typeForExtension(ext);
    if (type.empty()) {
        return Status::kUnsupported;
    }
    const std::string target = importPathFor(project, type, request.collection, in.fileName);
    if (target.empty()) {
        return Status::kInvalidArgument;
    }
    const std::string hash = design::detail::contentHash(in.bytes);
    if (const auto found = index.byHash.find(hash); found != index.byHash.end()) {
        result.deduplicated = true;
        result.duplicateOf = found->second;
        result.relFile = found->second;
        result.warnings.push_back("'" + in.fileName + "' has the same content as '" + found->second +
                                  "'; nothing was copied");
        return ensureMeta(project, found->second, result.meta, nullptr);
    }
    std::string rel;
    for (int n = 1; n <= kMaxNameTries; ++n) {
        const std::string candidate = numbered(target, n);
        if (!fileExists(detail::absolutePath(project, candidate)) &&
            !fileExists(detail::absolutePath(project, candidate) + ".meta")) {
            rel = candidate;
            break;
        }
    }
    if (rel.empty()) {
        return Status::kIoError;
    }
    const std::string path = detail::absolutePath(project, rel);
    std::error_code ec;
    fs::create_directories(pathFromUtf8(path).parent_path(), ec);
    if (!bundle::detail::writeFile(path, in.bytes)) {
        return Status::kIoError;
    }
    AssetMeta meta = detail::freshMeta(project, rel, in.bytes);
    meta.id = design::detail::newGuid();
    meta.collection = request.collection;
    meta.title = request.title;
    meta.tags = request.tags;
    meta.status = request.status;
    meta.license = in.license;
    meta.links = in.links;
    meta.source = request.source;
    if (meta.source.kind.empty()) {
        meta.source.kind = "manual";
    }
    if (request.import.is_object()) {
        meta.import = request.import;
    }
    if (detail::has2xSuffix(detail::stemOf(rel))) {
        if (!meta.import.is_object()) {
            meta.import = json::object();
        }
        meta.import["scale2x"] = true;
    }
    const Status written = writeMeta(path, meta);
    if (written != Status::kOk) {
        return written;
    }
    index.byHash.emplace(hash, rel);
    result.relFile = rel;
    result.meta = std::move(meta);
    return Status::kOk;
}

Status importOne(const UiProject& project, ContentIndex& index, const ImportRequest& request,
                 ImportResult& result) {
    Incoming in;
    in.request = &request;
    in.fileName = detail::fileNameOf(request.sourcePath);
    if (!bundle::detail::readFile(request.sourcePath, in.bytes)) {
        result = ImportResult{};
        return Status::kNotFound;
    }
    return importBytes(project, index, in, result);
}

/// The scalar a manifest `extra` value (YAML scalar text) spells.
std::string extraScalar(const std::string& yamlText) {
    json root;
    if (bundle::detail::parseYamlLite("v: " + yamlText, root) && root.is_object()) {
        const auto it = root.find("v");
        if (it != root.end()) {
            return it->is_string() ? it->get<std::string>() : it->dump();
        }
    }
    return yamlText;
}

/// Seeds a just-created meta from the existing manifest entry of the same file.
void seedFromEntry(AssetMeta& meta, const AssetEntry& entry) {
    if (!entry.source.empty()) {
        meta.source.kind = entry.source;
    }
    if (isStatus(entry.status)) {
        meta.status = entry.status;
    }
    meta.license = entry.license;
    if (entry.measured.hasSlice) {
        if (!meta.import.is_object()) {
            meta.import = json::object();
        }
        const int* s = entry.measured.slice;
        meta.import["slice"] = json::array({s[0], s[1], s[2], s[3]});
    }
    for (const auto& [key, value] : entry.extra) {
        if (key == "compress") {
            if (!meta.import.is_object()) {
                meta.import = json::object();
            }
            meta.import["compress"] = extraScalar(value);
        }
    }
}

}  // namespace

// --- import, replace ---

Status importAsset(const UiProject& project, const ImportRequest& request, ImportResult& result) {
    if (project.isUnnamed()) {
        result = ImportResult{};
        return Status::kInvalidArgument;
    }
    ContentIndex index = buildIndex(project);
    return importOne(project, index, request, result);
}

Status importAssets(const UiProject& project, const std::vector<ImportRequest>& requests,
                    std::vector<ImportResult>& results) {
    results.clear();
    if (project.isUnnamed()) {
        return Status::kInvalidArgument;
    }
    ContentIndex index = buildIndex(project);
    for (const ImportRequest& request : requests) {
        ImportResult result;
        const Status status = importOne(project, index, request, result);
        if (status != Status::kOk) {
            return status;
        }
        results.push_back(std::move(result));
    }
    return Status::kOk;
}

Status replaceAsset(const UiProject& project, std::string_view relFile, std::string_view newSourcePath,
                    AssetMeta& meta) {
    if (project.isUnnamed()) {
        return Status::kInvalidArgument;
    }
    const std::string path = detail::absolutePath(project, relFile);
    std::error_code ec;
    if (!fs::is_regular_file(pathFromUtf8(path), ec)) {
        return Status::kNotFound;
    }
    if (detail::lowerExt(relFile) != detail::lowerExt(newSourcePath)) {
        return Status::kInvalidArgument;
    }
    std::string bytes;
    if (!bundle::detail::readFile(std::string(newSourcePath), bytes)) {
        return Status::kNotFound;
    }
    AssetMeta current;
    const Status ensured = ensureMeta(project, relFile, current, nullptr);
    if (ensured != Status::kOk) {
        return ensured;
    }
    if (!bundle::detail::writeFile(path, bytes)) {
        return Status::kIoError;
    }
    current.contentHash = design::detail::contentHash(bytes);
    detail::measureInto(bytes, detail::lowerExt(relFile), current.measured);
    current.source.at = isoUtcNow();
    current.status = "draft";
    const Status written = writeMeta(path, current);
    if (written != Status::kOk) {
        return written;
    }
    meta = std::move(current);
    return Status::kOk;
}

// --- source folders ---

Status syncSourceFolders(const UiProject& project, SyncReport& report) {
    report = SyncReport{};
    if (project.isUnnamed()) {
        return Status::kInvalidArgument;
    }
    ContentIndex index = buildIndex(project);
    for (const SourceFolder& folder : sourceFolders(project)) {
        if (importPathFor(project, "image", folder.collection, "x.png").empty()) {
            return Status::kInvalidArgument;
        }
        const fs::path dir = pathFromUtf8(folder.path);
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) {
            report.warnings.push_back("source folder '" + folder.path + "' does not exist");
            continue;
        }
        std::vector<fs::path> files;
        for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
            if (ec) {
                break;
            }
            if (it->is_regular_file(ec) &&
                !typeForExtension(detail::lowerExt(pathToUtf8(it->path().filename()))).empty()) {
                files.push_back(it->path());
            }
        }
        std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
            return genericPathToUtf8(a.filename()) < genericPathToUtf8(b.filename());
        });
        for (const fs::path& file : files) {
            const std::string source = genericPathToUtf8(file);
            const std::string name = pathToUtf8(file.filename());
            const std::string type = typeForExtension(detail::lowerExt(name));
            const std::string target = importPathFor(project, type, folder.collection, name);
            if (fileExists(detail::absolutePath(project, target))) {
                std::string bytes;
                if (!bundle::detail::readFile(source, bytes)) {
                    report.warnings.push_back("cannot read '" + source + "'");
                    continue;
                }
                AssetMeta meta;
                Status status = ensureMeta(project, target, meta, nullptr);
                if (status != Status::kOk) {
                    return status;
                }
                if (meta.contentHash == design::detail::contentHash(bytes)) {
                    report.unchanged.push_back(target);
                    continue;
                }
                status = replaceAsset(project, target, source, meta);
                if (status != Status::kOk) {
                    return status;
                }
                meta.status = "stale";
                status = writeMeta(detail::absolutePath(project, target), meta);
                if (status != Status::kOk) {
                    return status;
                }
                index.byHash[meta.contentHash] = target;
                report.replaced.push_back(target);
                continue;
            }
            ImportRequest request;
            request.sourcePath = source;
            request.collection = folder.collection;
            request.import = folder.import;
            request.source.kind = "import:folder";
            request.source.ref = source;
            ImportResult result;
            const Status status = importOne(project, index, request, result);
            if (status != Status::kOk) {
                return status;
            }
            if (result.deduplicated) {
                report.warnings.insert(report.warnings.end(), result.warnings.begin(), result.warnings.end());
            } else {
                report.imported.push_back(result.relFile);
            }
        }
        // Assets imported from this folder whose file is gone.
        const std::string prefix =
            folder.path + (folder.path.empty() || folder.path.back() == '/' ? "" : "/");
        for (const detail::ScannedAsset& asset : detail::scanAssets(project)) {
            if (collectionOf(project, asset.rel) != folder.collection) {
                continue;
            }
            AssetMeta meta;
            if (readMeta(detail::absolutePath(project, asset.rel), meta) != Status::kOk ||
                meta.source.kind != "import:folder" || meta.source.ref.rfind(prefix, 0) != 0) {
                continue;
            }
            if (!fileExists(meta.source.ref)) {
                report.missing.push_back(asset.rel);
            }
        }
    }
    for (std::vector<std::string>* list :
         {&report.imported, &report.replaced, &report.unchanged, &report.missing}) {
        std::sort(list->begin(), list->end());
        list->erase(std::unique(list->begin(), list->end()), list->end());
    }
    return Status::kOk;
}

// --- the generated manifest ---

Status regenerateAssetsManifest(const UiProject& project, AssetsManifest& out, RegenerateReport* report,
                                std::string* error) {
    RegenerateReport local;
    if (project.isUnnamed()) {
        if (error != nullptr) {
            *error = "an unnamed UI Project has no asset manifest";
        }
        return Status::kInvalidArgument;
    }
    const std::string manifestPath = detail::absolutePath(project, kAssetsManifestPath);
    AssetsManifest existing;
    std::string existingText;
    const bool hadFile = bundle::detail::readFile(manifestPath, existingText);
    if (hadFile) {
        std::string message;
        if (!AssetsManifest::parse(existingText, existing, &message)) {
            if (error != nullptr) {
                *error = std::string(kAssetsManifestPath) + ": " + message;
            }
            return Status::kInvalidArgument;
        }
    }
    std::map<std::string, size_t> existingByFile;
    std::set<std::string> usedIds;
    for (size_t i = 0; i < existing.assets.size(); ++i) {
        existingByFile.emplace(existing.assets[i].file, i);
        usedIds.insert(existing.assets[i].id);
    }

    AssetsManifest manifest;
    std::set<size_t> matched;
    for (const detail::ScannedAsset& asset : detail::scanAssets(project)) {
        if (asset.variant2x) {
            continue;
        }
        AssetMeta meta;
        bool created = false;
        const Status status = ensureMeta(project, asset.rel, meta, &created);
        if (status != Status::kOk) {
            if (error != nullptr) {
                *error = "cannot read or write the meta of '" + asset.rel + "'";
            }
            return status;
        }
        const auto old = existingByFile.find(asset.rel);
        AssetEntry entry;
        if (old != existingByFile.end()) {
            matched.insert(old->second);
            entry = existing.assets[old->second];
            if (created) {
                seedFromEntry(meta, entry);
                const Status written = writeMeta(detail::absolutePath(project, asset.rel), meta);
                if (written != Status::kOk) {
                    return written;
                }
            }
        } else {
            std::string id = detail::stemOf(asset.rel);
            if (usedIds.count(id) != 0) {
                std::string collection = collectionOf(project, asset.rel);
                std::replace(collection.begin(), collection.end(), '/', '-');
                const std::string base =
                    (collection.empty() ? typeDirectory(asset.type) : collection) + "-" + id;
                id = base;
                for (int n = 2; usedIds.count(id) != 0 && n <= kMaxNameTries; ++n) {
                    id = base + "-" + std::to_string(n);
                }
            }
            usedIds.insert(id);
            entry.id = id;
        }
        if (created) {
            ++local.metasCreated;
        }
        entry.type = meta.type.empty() ? asset.type : meta.type;
        entry.file = asset.rel;
        entry.source = meta.source.kind.empty() ? std::string("manual") : meta.source.kind;
        entry.status = meta.status.empty() ? std::string("draft") : meta.status;
        entry.license = meta.license;
        entry.measured = AssetMeasured{};
        entry.measured.hasSize = meta.measured.hasSize;
        entry.measured.w = meta.measured.w;
        entry.measured.h = meta.measured.h;
        entry.measured.hasAlpha = meta.measured.hasAlpha;
        entry.measured.alpha = meta.measured.alpha;
        int slice[4] = {0, 0, 0, 0};
        if (detail::sliceOf(meta, slice)) {
            entry.measured.hasSlice = true;
            for (int i = 0; i < 4; ++i) {
                entry.measured.slice[i] = slice[i];
            }
        }
        if (meta.import.is_object()) {
            const auto compress = meta.import.find("compress");
            if (compress != meta.import.end() && compress->is_string()) {
                const std::string value = bundle::detail::yamlScalar(compress->get<std::string>());
                bool replaced = false;
                for (auto& [key, text] : entry.extra) {
                    if (key == "compress") {
                        text = value;
                        replaced = true;
                    }
                }
                if (!replaced) {
                    entry.extra.emplace_back("compress", value);
                }
            }
        }
        manifest.assets.push_back(std::move(entry));
    }
    for (size_t i = 0; i < existing.assets.size(); ++i) {
        if (matched.count(i) == 0) {
            manifest.assets.push_back(existing.assets[i]);
        }
    }

    const std::string text = manifest.toYaml();
    if (hadFile && existing.hasUnsavableContent) {
        local.warnings.push_back(std::string(kAssetsManifestPath) +
                                 " not regenerated: it has comments or structured values the writer cannot "
                                 "preserve");
    } else if ((hadFile && text != existingText) || (!hadFile && !manifest.assets.empty())) {
        std::error_code ec;
        fs::create_directories(pathFromUtf8(manifestPath).parent_path(), ec);
        if (manifest.save(manifestPath) != Status::kOk) {
            if (error != nullptr) {
                *error = "cannot write '" + manifestPath + "'";
            }
            return Status::kIoError;
        }
        local.written = true;
    }
    out = std::move(manifest);
    if (report != nullptr) {
        *report = std::move(local);
    }
    return Status::kOk;
}

// --- promoting a reference ---

Status promoteReference(const UiProject& project, std::string_view refIdIn,
                        const std::array<float, 4>* cropRect, std::string_view collection,
                        ImportResult& result) {
    result = ImportResult{};
    if (project.isUnnamed()) {
        return Status::kInvalidArgument;
    }
    std::string refId(refIdIn);
    if (refId.rfind("ref:", 0) == 0) {
        refId.erase(0, 4);
    }
    design::DesignSystem ds;
    if (design::load(project, ds) != Status::kOk) {
        return Status::kNotFound;
    }
    const design::ReferenceMeta* ref = nullptr;
    for (const design::ReferenceMeta& r : ds.references) {
        if (r.base.id == refId) {
            ref = &r;
        }
    }
    if (ref == nullptr) {
        return Status::kNotFound;
    }
    const std::string sourcePath = bundle::detail::joinPath(ds.rootDir, ref->path);
    Incoming in;
    if (!bundle::detail::readFile(sourcePath, in.bytes)) {
        return Status::kNotFound;
    }
    in.fileName = detail::fileNameOf(ref->path);
    if (typeForExtension(detail::lowerExt(in.fileName)).empty()) {
        return Status::kUnsupported;
    }
    std::vector<std::string> warnings;
    if (cropRect != nullptr) {
        const std::string_view b(in.bytes);
        const bool png = b.size() >= 8 && b.substr(0, 8) == std::string_view("\x89PNG\r\n\x1a\n", 8);
        const bool jpeg = b.size() >= 3 && static_cast<unsigned char>(b[0]) == 0xFF &&
                          static_cast<unsigned char>(b[1]) == 0xD8;
        ImageData image;
        if (!(png || jpeg)) {
            warnings.push_back("'" + in.fileName + "' is not a PNG or JPEG; copied whole, not cropped");
        } else if (!sw::decodeImageBytes(reinterpret_cast<const uint8_t*>(in.bytes.data()), in.bytes.size(),
                                         image) ||
                   image.width <= 0 || image.height <= 0) {
            warnings.push_back("'" + in.fileName + "' cannot be decoded; copied whole, not cropped");
        } else {
            const float x = clampFinite((*cropRect)[0], 0.0f, 1.0f, 0.0f);
            const float y = clampFinite((*cropRect)[1], 0.0f, 1.0f, 0.0f);
            const float w = clampFinite((*cropRect)[2], 0.0f, 1.0f, 0.0f);
            const float h = clampFinite((*cropRect)[3], 0.0f, 1.0f, 0.0f);
            const int x0 =
                std::clamp(toInt32Sat(std::floor(static_cast<double>(x) * image.width)), 0, image.width);
            const int y0 =
                std::clamp(toInt32Sat(std::floor(static_cast<double>(y) * image.height)), 0, image.height);
            const int x1 =
                std::clamp(toInt32Sat(std::ceil(static_cast<double>(x + w) * image.width)), 0, image.width);
            const int y1 =
                std::clamp(toInt32Sat(std::ceil(static_cast<double>(y + h) * image.height)), 0, image.height);
            if (x1 <= x0 || y1 <= y0) {
                return Status::kInvalidArgument;
            }
            const int cw = x1 - x0;
            const int ch = y1 - y0;
            std::vector<uint8_t> rgba(static_cast<size_t>(cw) * static_cast<size_t>(ch) * 4);
            for (int row = 0; row < ch; ++row) {
                const uint8_t* from =
                    image.rgba.data() + (static_cast<size_t>(y0 + row) * static_cast<size_t>(image.width) +
                                         static_cast<size_t>(x0)) *
                                            4;
                std::copy(from, from + static_cast<size_t>(cw) * 4,
                          rgba.data() + static_cast<size_t>(row) * static_cast<size_t>(cw) * 4);
            }
            std::vector<uint8_t> encoded;
            if (!sw::encodePng(rgba.data(), cw, ch, cw * 4, encoded)) {
                return Status::kIoError;
            }
            in.bytes.assign(reinterpret_cast<const char*>(encoded.data()), encoded.size());
            in.fileName = detail::stemOf(in.fileName) + ".png";
        }
    }
    ImportRequest request;
    request.collection = std::string(collection);
    request.title = ref->title;
    request.status = "placeholder";
    request.source.kind = "manual";
    in.request = &request;
    in.license = ref->license;
    in.links.push_back(design::Link{"derived-from", "ref:" + refId, std::string()});
    if (importPathFor(project, "image", collection, "x.png").empty()) {
        return Status::kInvalidArgument;
    }
    ContentIndex index = buildIndex(project);
    const Status status = importBytes(project, index, in, result);
    result.warnings.insert(result.warnings.begin(), warnings.begin(), warnings.end());
    return status;
}

}  // namespace xt::bundle::assets
