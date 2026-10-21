#include "xt/bundle/asset_meta.h"

#include <algorithm>
#include <filesystem>
#include <json.hpp>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "asset_internal.h"
#include "bundle_json.h"
#include "design_internal.h"
#include "xt/bundle/migrations.h"
#include "xt/compile/utf8_path.h"
#include "xt/num_guard.h"
#include "yaml_lite.h"

/// Asset metas, types, collections and import paths (docs/design/35-design-system.md §35.15.1,
/// 40 §40.2.1; docs/tasks/T-312a.md items 1 and 3). See xt/bundle/asset_meta.h.
///
/// A meta is read with yaml_lite and written with emitYamlLite() from an ordered_json composed in
/// the fixed key order id, type, title, collection, status, source{kind, ref, at, <extra>},
/// approved{by, at}, license, content, tags, measured{w, h, alpha, slice}, import, links[{rel, to,
/// note}], note, <extra alphabetically>; `import` is written with its keys sorted.

namespace xt::bundle::assets {

namespace fs = std::filesystem;
using compile::genericPathToUtf8;
using compile::pathFromUtf8;
using compile::pathToUtf8;
using nlohmann::json;
using nlohmann::ordered_json;

namespace {

void setError(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

/// An unordered json as an ordered one (keys sorted, as json keeps them).
ordered_json ordered(const json& value) {
    if (value.is_object()) {
        ordered_json out = ordered_json::object();
        for (auto it = value.begin(); it != value.end(); ++it) {
            out[it.key()] = ordered(it.value());
        }
        return out;
    }
    if (value.is_array()) {
        ordered_json out = ordered_json::array();
        for (const json& item : value) {
            out.push_back(ordered(item));
        }
        return out;
    }
    if (value.is_string()) {
        return ordered_json(value.get<std::string>());
    }
    if (value.is_boolean()) {
        return ordered_json(value.get<bool>());
    }
    if (value.is_number_unsigned()) {
        return ordered_json(value.get<uint64_t>());
    }
    if (value.is_number_integer()) {
        return ordered_json(value.get<int64_t>());
    }
    if (value.is_number_float()) {
        return ordered_json(value.get<double>());
    }
    return ordered_json(nullptr);
}

void putExtra(json& extra, const std::string& key, const json& value) {
    if (!extra.is_object()) {
        extra = json::object();
    }
    extra[key] = value;
}

/// A scalar as text (strings verbatim, numbers / booleans dumped); false for null and containers.
bool scalarText(const json& value, std::string& out) {
    if (value.is_string()) {
        out = value.get<std::string>();
        return true;
    }
    if (value.is_number() || value.is_boolean()) {
        out = value.dump();
        return true;
    }
    return false;
}

/// Reads string key `key` of `obj` into `out`; a non-scalar value goes to `extra`.
void readString(const json& obj, const char* key, std::string& out, json& extra) {
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) {
        return;
    }
    if (!scalarText(*it, out)) {
        putExtra(extra, key, *it);
    }
}

int readInt(const json& value) { return value.is_number() ? toInt32Sat(value.get<double>()) : 0; }

bool readLinks(const json& value, std::vector<design::Link>& out) {
    if (!value.is_array()) {
        return false;
    }
    std::vector<design::Link> links;
    for (const json& item : value) {
        if (!item.is_object()) {
            return false;
        }
        design::Link link;
        json ignored;
        readString(item, "rel", link.rel, ignored);
        readString(item, "to", link.to, ignored);
        readString(item, "note", link.note, ignored);
        links.push_back(std::move(link));
    }
    out = std::move(links);
    return true;
}

bool isKnownKey(const std::string& key) {
    static constexpr const char* kKnown[] = {"version", "id",       "type",     "title",   "collection",
                                             "status",  "source",   "approved", "license", "content",
                                             "tags",    "measured", "import",   "links",   "note"};
    for (const char* known : kKnown) {
        if (key == known) {
            return true;
        }
    }
    return false;
}

std::string asciiLower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

std::string metaPathOf(std::string_view filePath) { return std::string(filePath) + ".meta"; }

/// Splits `rel` on '/', dropping a leading "./".
std::vector<std::string> segments(std::string_view rel) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= rel.size()) {
        const size_t end = std::min(rel.find_first_of("/\\", start), rel.size());
        std::string part(rel.substr(start, end - start));
        if (!part.empty() && part != ".") {
            out.push_back(std::move(part));
        }
        start = end + 1;
    }
    return out;
}

/// True when `collection` is "" or a plain relative path (no empty / `.` / `..` segment, no '\\',
/// no ':', no leading '/').
bool isPlainCollection(std::string_view collection) {
    if (collection.empty()) {
        return true;
    }
    if (collection.find_first_of("\\:") != std::string_view::npos) {
        return false;
    }
    size_t start = 0;
    while (start <= collection.size()) {
        const size_t end = std::min(collection.find('/', start), collection.size());
        const std::string_view part = collection.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        start = end + 1;
    }
    return true;
}

}  // namespace

// --- internal helpers ---

namespace detail {

std::string assetRootOf(const UiProject& project) {
    std::string root = project.shared.assetRoot.empty() ? std::string("shared") : project.shared.assetRoot;
    std::replace(root.begin(), root.end(), '\\', '/');
    while (!root.empty() && root.back() == '/') {
        root.pop_back();
    }
    while (root.rfind("./", 0) == 0) {
        root.erase(0, 2);
    }
    return root.empty() ? std::string("shared") : root;
}

std::string absolutePath(const UiProject& project, std::string_view rel) {
    return genericPathToUtf8(pathFromUtf8(project.rootDir) / pathFromUtf8(rel));
}

std::string lowerExt(std::string_view path) {
    const std::string name = fileNameOf(path);
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) {
        return std::string();
    }
    return asciiLower(name.substr(dot));
}

std::string fileNameOf(std::string_view path) {
    const size_t slash = path.find_last_of("/\\");
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

std::string stemOf(std::string_view path) {
    const std::string name = fileNameOf(path);
    const size_t dot = name.rfind('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

bool has2xSuffix(std::string_view stem) { return stem.size() > 3 && stem.substr(stem.size() - 3) == "@2x"; }

void measureInto(std::string_view bytes, std::string_view ext, AssetMeasured& m) {
    m.hasSize = false;
    m.w = 0;
    m.h = 0;
    m.hasAlpha = false;
    m.alpha = false;
    if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".svg") {
        return;
    }
    const ImageMeasure measured = measureImage(bytes, ext);
    if (!measured.ok) {
        return;
    }
    m.hasSize = true;
    m.w = measured.w;
    m.h = measured.h;
    m.hasAlpha = true;
    m.alpha = measured.alpha;
}

AssetMeta freshMeta(const UiProject& project, std::string_view rel, std::string_view bytes) {
    AssetMeta meta;
    const std::string ext = lowerExt(rel);
    meta.type = typeForExtension(ext);
    meta.collection = collectionOf(project, rel);
    meta.contentHash = design::detail::contentHash(bytes);
    meta.source.kind = "manual";
    meta.status = "draft";
    measureInto(bytes, ext, meta.measured);
    return meta;
}

bool sliceOf(const AssetMeta& meta, int slice[4]) {
    if (meta.import.is_object()) {
        const auto it = meta.import.find("slice");
        if (it != meta.import.end() && it->is_array() && it->size() == 4) {
            bool numbers = true;
            for (const json& n : *it) {
                numbers = numbers && n.is_number();
            }
            if (numbers) {
                for (size_t i = 0; i < 4; ++i) {
                    slice[i] = toInt32Sat((*it)[i].get<double>());
                }
                return true;
            }
        }
    }
    if (meta.measured.hasSlice) {
        for (int i = 0; i < 4; ++i) {
            slice[i] = meta.measured.slice[i];
        }
        return true;
    }
    return false;
}

std::vector<ScannedAsset> scanAssets(const UiProject& project) {
    struct Dir {
        const char* name;
        const char* exts[8];  // nullptr-terminated
    };
    static const Dir kDirs[] = {
        {"img", {".png", ".jpg", ".jpeg", ".svg"}},
        {"fonts", {".ttf", ".otf", ".ttc", ".woff", ".woff2"}},
        {"anim", {".json", ".lottie"}},
        {"video", {".mp4", ".webm", ".mov", ".ogv", ".mkv", ".avi", ".bik"}},
        {"fx", {".xtfx", ".xtfxp"}},
    };
    std::vector<ScannedAsset> out;
    if (project.rootDir.empty()) {
        return out;
    }
    const fs::path root = pathFromUtf8(project.rootDir);
    const std::string assetRoot = assetRootOf(project);
    std::error_code ec;
    for (const Dir& dir : kDirs) {
        const fs::path base = root / pathFromUtf8(assetRoot) / dir.name;
        if (!fs::is_directory(base, ec)) {
            continue;
        }
        for (fs::recursive_directory_iterator it(base, ec), end; it != end; it.increment(ec)) {
            if (ec) {
                break;
            }
            if (!it->is_regular_file(ec)) {
                continue;
            }
            const std::string ext = lowerExt(pathToUtf8(it->path().filename()));
            bool wanted = false;
            for (const char* const* e = dir.exts; *e != nullptr; ++e) {
                wanted = wanted || ext == *e;
            }
            if (!wanted) {
                continue;
            }
            ScannedAsset asset;
            asset.rel = genericPathToUtf8(it->path().lexically_relative(root));
            asset.ext = ext;
            asset.type = typeForExtension(ext);
            const std::string stem = stemOf(asset.rel);
            if (has2xSuffix(stem)) {
                const fs::path base1x =
                    it->path().parent_path() /
                    pathFromUtf8(stem.substr(0, stem.size() - 3) + pathToUtf8(it->path().extension()));
                asset.variant2x = fs::is_regular_file(base1x, ec);
            }
            out.push_back(std::move(asset));
        }
    }
    std::sort(out.begin(), out.end(),
              [](const ScannedAsset& a, const ScannedAsset& b) { return a.rel < b.rel; });
    return out;
}

}  // namespace detail

// --- statuses, types, names ---

bool isStatus(std::string_view status) {
    return std::find(kStatuses.begin(), kStatuses.end(), status) != kStatuses.end();
}

std::string typeForExtension(std::string_view extIn) {
    const std::string ext = asciiLower(extIn);
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp") return "image";
    if (ext == ".svg") return "vector";
    if (ext == ".ttf" || ext == ".otf" || ext == ".ttc" || ext == ".woff" || ext == ".woff2") return "font";
    if (ext == ".json" || ext == ".lottie") return "anim";
    if (ext == ".mp4" || ext == ".webm" || ext == ".mov" || ext == ".ogv" || ext == ".mkv" || ext == ".avi" ||
        ext == ".bik") {
        return "video";
    }
    if (ext == ".xtfx" || ext == ".xtfxp") return "effect";
    return std::string();
}

std::string typeDirectory(std::string_view type) {
    return type == "font"     ? "fonts"
           : type == "anim"   ? "anim"
           : type == "video"  ? "video"
           : type == "effect" ? "fx"
                              : "img";
}

std::string kebabAssetId(std::string_view stem) {
    std::string out;
    for (char c : stem) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte >= 0x80) {
            out += c;
        } else if ((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9')) {
            out += c;
        } else if (byte >= 'A' && byte <= 'Z') {
            out += static_cast<char>(byte - 'A' + 'a');
        } else if (!out.empty() && out.back() != '-') {
            out += '-';
        }
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    return out;
}

std::string collectionOf(const UiProject& project, std::string_view relFile) {
    const std::vector<std::string> root = segments(detail::assetRootOf(project));
    const std::vector<std::string> parts = segments(relFile);
    // <assetRoot...>/<type dir>/<collection...>/<file>
    if (parts.size() < root.size() + 2 || !std::equal(root.begin(), root.end(), parts.begin())) {
        return std::string();
    }
    std::string out;
    for (size_t i = root.size() + 1; i + 1 < parts.size(); ++i) {
        out += out.empty() ? parts[i] : "/" + parts[i];
    }
    return out;
}

std::string importPathFor(const UiProject& project, std::string_view type, std::string_view collection,
                          std::string_view fileName) {
    if (!isPlainCollection(collection)) {
        return std::string();
    }
    std::string stem = detail::stemOf(fileName);
    const bool is2x = detail::has2xSuffix(stem);
    if (is2x) {
        stem.resize(stem.size() - 3);
    }
    std::string name = kebabAssetId(stem);
    if (name.empty()) {
        name = "asset";
    }
    if (is2x) {
        name += "@2x";
    }
    std::string out = detail::assetRootOf(project) + "/" + typeDirectory(type) + "/";
    if (!collection.empty()) {
        out += std::string(collection) + "/";
    }
    return out + name + detail::lowerExt(fileName);
}

bool assetsUpgraded(const UiProject& project) {
    if (project.isUnnamed()) {
        return false;
    }
    std::error_code ec;
    if (fs::is_directory(pathFromUtf8(design::designDir(project)), ec)) {
        return true;
    }
    const fs::path base = pathFromUtf8(project.rootDir) / pathFromUtf8(detail::assetRootOf(project));
    if (!fs::is_directory(base, ec)) {
        return false;
    }
    for (fs::recursive_directory_iterator it(base, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        if (it->is_regular_file(ec) && detail::lowerExt(pathToUtf8(it->path().filename())) == ".meta") {
            return true;
        }
    }
    return false;
}

// --- meta text ---

bool parseMeta(std::string_view text, AssetMeta& out, std::string* error) {
    json root;
    std::string message;
    if (!bundle::detail::parseYamlLite(text, root, &message)) {
        setError(error, message);
        return false;
    }
    AssetMeta meta;
    if (root.is_null()) {
        out = std::move(meta);
        return true;
    }
    if (!root.is_object()) {
        setError(error, "not a mapping");
        return false;
    }
    // T-406 (43 §43.2): missing = 1; older versions are migrated, newer ones refused.
    if (migrateToLatest(SourceFormat::Meta, root, nullptr, &message) != Status::kOk) {
        setError(error, message);
        return false;
    }
    readString(root, "id", meta.id, meta.extra);
    readString(root, "type", meta.type, meta.extra);
    readString(root, "title", meta.title, meta.extra);
    readString(root, "collection", meta.collection, meta.extra);
    readString(root, "status", meta.status, meta.extra);
    if (meta.status.empty()) {
        meta.status = "draft";
    }
    readString(root, "license", meta.license, meta.extra);
    readString(root, "content", meta.contentHash, meta.extra);
    readString(root, "note", meta.note, meta.extra);
    if (const auto it = root.find("source"); it != root.end() && !it->is_null()) {
        if (it->is_object()) {
            json rest = json::object();
            for (auto s = it->begin(); s != it->end(); ++s) {
                std::string value;
                if (s.key() == "kind" && scalarText(*s, value)) {
                    meta.source.kind = value;
                } else if (s.key() == "ref" && scalarText(*s, value)) {
                    meta.source.ref = value;
                } else if (s.key() == "at" && scalarText(*s, value)) {
                    meta.source.at = value;
                } else {
                    rest[s.key()] = *s;
                }
            }
            if (!rest.empty()) {
                putExtra(meta.extra, "source", rest);
            }
        } else {
            putExtra(meta.extra, "source", *it);
        }
    }
    if (const auto it = root.find("approved"); it != root.end() && !it->is_null()) {
        if (it->is_object()) {
            json ignored;
            readString(*it, "by", meta.approved.by, ignored);
            readString(*it, "at", meta.approved.at, ignored);
        } else {
            putExtra(meta.extra, "approved", *it);
        }
    }
    if (const auto it = root.find("tags"); it != root.end() && !it->is_null()) {
        std::string one;
        if (scalarText(*it, one)) {
            meta.tags.push_back(one);
        } else if (it->is_array()) {
            for (const json& tag : *it) {
                std::string value;
                if (scalarText(tag, value)) {
                    meta.tags.push_back(value);
                }
            }
        } else {
            putExtra(meta.extra, "tags", *it);
        }
    }
    if (const auto it = root.find("measured"); it != root.end() && it->is_object()) {
        AssetMeasured& m = meta.measured;
        const auto w = it->find("w");
        const auto h = it->find("h");
        if (w != it->end() && h != it->end() && w->is_number() && h->is_number()) {
            m.hasSize = true;
            m.w = readInt(*w);
            m.h = readInt(*h);
        }
        if (const auto alpha = it->find("alpha"); alpha != it->end() && alpha->is_boolean()) {
            m.hasAlpha = true;
            m.alpha = alpha->get<bool>();
        }
        if (const auto slice = it->find("slice");
            slice != it->end() && slice->is_array() && slice->size() == 4) {
            bool numbers = true;
            for (const json& n : *slice) {
                numbers = numbers && n.is_number();
            }
            if (numbers) {
                m.hasSlice = true;
                for (size_t i = 0; i < 4; ++i) {
                    m.slice[i] = readInt((*slice)[i]);
                }
            }
        }
    } else if (it != root.end() && !it->is_null()) {
        putExtra(meta.extra, "measured", *it);
    }
    if (const auto it = root.find("import"); it != root.end() && !it->is_null()) {
        meta.import = *it;
    }
    if (const auto it = root.find("links"); it != root.end() && !it->is_null()) {
        if (!readLinks(*it, meta.links)) {
            putExtra(meta.extra, "links", *it);
        }
    }
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (!isKnownKey(it.key())) {
            putExtra(meta.extra, it.key(), it.value());
        }
    }
    out = std::move(meta);
    return true;
}

std::string metaText(const AssetMeta& meta) {
    const json& extra = meta.extra;
    const auto extraHas = [&extra](const char* key) { return extra.is_object() && extra.contains(key); };
    const auto putString = [&](ordered_json& o, const char* key, const std::string& value) {
        if (extraHas(key)) {
            o[key] = ordered(extra[key]);
        } else if (!value.empty()) {
            o[key] = value;
        }
    };
    ordered_json o = ordered_json::object();
    o["version"] = kMetaFormatVersion;  // T-406: always the latest.
    putString(o, "id", meta.id);
    putString(o, "type", meta.type);
    putString(o, "title", meta.title);
    putString(o, "collection", meta.collection);
    if (extraHas("status")) {
        o["status"] = ordered(extra["status"]);
    } else {
        o["status"] = meta.status.empty() ? std::string("draft") : meta.status;
    }
    if (extraHas("source") && !extra["source"].is_object()) {
        o["source"] = ordered(extra["source"]);
    } else {
        ordered_json s = ordered_json::object();
        if (!meta.source.kind.empty()) {
            s["kind"] = meta.source.kind;
        }
        if (!meta.source.ref.empty()) {
            s["ref"] = meta.source.ref;
        }
        if (!meta.source.at.empty()) {
            s["at"] = meta.source.at;
        }
        if (extraHas("source")) {
            for (auto it = extra["source"].begin(); it != extra["source"].end(); ++it) {
                if (!s.contains(it.key())) {
                    s[it.key()] = ordered(it.value());
                }
            }
        }
        if (!s.empty()) {
            o["source"] = std::move(s);
        }
    }
    if (extraHas("approved")) {
        o["approved"] = ordered(extra["approved"]);
    } else if (!meta.approved.by.empty() || !meta.approved.at.empty()) {
        ordered_json a = ordered_json::object();
        if (!meta.approved.by.empty()) {
            a["by"] = meta.approved.by;
        }
        if (!meta.approved.at.empty()) {
            a["at"] = meta.approved.at;
        }
        o["approved"] = std::move(a);
    }
    putString(o, "license", meta.license);
    putString(o, "content", meta.contentHash);
    if (extraHas("tags")) {
        o["tags"] = ordered(extra["tags"]);
    } else if (!meta.tags.empty()) {
        o["tags"] = meta.tags;
    }
    if (extraHas("measured")) {
        o["measured"] = ordered(extra["measured"]);
    } else {
        const AssetMeasured& m = meta.measured;
        ordered_json mo = ordered_json::object();
        if (m.hasSize) {
            mo["w"] = m.w;
            mo["h"] = m.h;
        }
        if (m.hasAlpha) {
            mo["alpha"] = m.alpha;
        }
        if (m.hasSlice) {
            mo["slice"] = ordered_json::array({m.slice[0], m.slice[1], m.slice[2], m.slice[3]});
        }
        if (!mo.empty()) {
            o["measured"] = std::move(mo);
        }
    }
    if (!meta.import.is_null() && !(meta.import.is_object() && meta.import.empty())) {
        o["import"] = ordered(meta.import);
    }
    if (extraHas("links")) {
        o["links"] = ordered(extra["links"]);
    } else if (!meta.links.empty()) {
        ordered_json list = ordered_json::array();
        for (const design::Link& link : meta.links) {
            ordered_json l = ordered_json::object();
            l["rel"] = link.rel;
            l["to"] = link.to;
            if (!link.note.empty()) {
                l["note"] = link.note;
            }
            list.push_back(std::move(l));
        }
        o["links"] = std::move(list);
    }
    putString(o, "note", meta.note);
    if (extra.is_object()) {
        for (auto it = extra.begin(); it != extra.end(); ++it) {
            if (!o.contains(it.key()) && !isKnownKey(it.key())) {
                o[it.key()] = ordered(it.value());
            }
        }
    }
    return bundle::detail::emitYamlLite(o);
}

Status readMeta(std::string_view filePath, AssetMeta& out, std::string* error) {
    const std::string path = metaPathOf(filePath);
    std::error_code ec;
    if (!fs::is_regular_file(pathFromUtf8(path), ec)) {
        setError(error, "no meta '" + path + "'");
        return Status::kNotFound;
    }
    std::string text;
    if (!bundle::detail::readFile(path, text)) {
        setError(error, "cannot read '" + path + "'");
        return Status::kNotFound;
    }
    std::string message;
    if (!parseMeta(text, out, &message)) {
        setError(error, path + ": " + message);
        return Status::kInvalidArgument;
    }
    return Status::kOk;
}

Status writeMeta(std::string_view filePath, const AssetMeta& meta, std::string* error) {
    const std::string path = metaPathOf(filePath);
    const std::string text = metaText(meta);
    std::string existing;
    if (bundle::detail::readFile(path, existing) && existing == text) {
        return Status::kOk;
    }
    std::error_code ec;
    fs::create_directories(pathFromUtf8(path).parent_path(), ec);
    if (!bundle::detail::writeFile(path, text)) {
        setError(error, "cannot write '" + path + "'");
        return Status::kIoError;
    }
    return Status::kOk;
}

// --- ensureMeta, source folders ---

Status ensureMeta(const UiProject& project, std::string_view relFile, AssetMeta& meta, bool* created) {
    if (created != nullptr) {
        *created = false;
    }
    if (project.isUnnamed()) {
        return Status::kInvalidArgument;
    }
    const std::string path = detail::absolutePath(project, relFile);
    std::error_code ec;
    std::string bytes;
    if (!fs::is_regular_file(pathFromUtf8(path), ec) || !bundle::detail::readFile(path, bytes)) {
        return Status::kNotFound;
    }
    AssetMeta existing;
    const Status read = readMeta(path, existing);
    if (read == Status::kOk) {
        const std::string hash = design::detail::contentHash(bytes);
        if (existing.contentHash != hash) {
            existing.contentHash = hash;
            detail::measureInto(bytes, detail::lowerExt(relFile), existing.measured);
            const Status written = writeMeta(path, existing);
            if (written != Status::kOk) {
                return written;
            }
        }
        meta = std::move(existing);
        return Status::kOk;
    }
    if (read != Status::kNotFound) {
        return read;
    }
    AssetMeta fresh = detail::freshMeta(project, relFile, bytes);
    fresh.id = design::detail::newGuid();
    const Status written = writeMeta(path, fresh);
    if (written != Status::kOk) {
        return written;
    }
    if (created != nullptr) {
        *created = true;
    }
    meta = std::move(fresh);
    return Status::kOk;
}

std::vector<SourceFolder> sourceFolders(const UiProject& project) {
    std::vector<SourceFolder> out;
    for (const AssetSourceFolder& folder : project.assets.sourceFolders) {
        SourceFolder parsed;
        parsed.path = folder.path;
        std::replace(parsed.path.begin(), parsed.path.end(), '\\', '/');
        parsed.collection = folder.collection;
        if (!folder.importJson.empty()) {
            json settings = json::parse(folder.importJson, nullptr, false);
            if (!settings.is_discarded()) {
                parsed.import = std::move(settings);
            }
        }
        out.push_back(std::move(parsed));
    }
    return out;
}

}  // namespace xt::bundle::assets
