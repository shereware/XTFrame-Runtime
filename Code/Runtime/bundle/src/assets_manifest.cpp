#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <json.hpp>
#include <set>
#include <string>
#include <system_error>
#include <utility>

#include "bundle_json.h"
#include "xt/bundle/migrations.h"
#include "xt/bundle/ui_project.h"
#include "xt/charconv.h"
#include "xt/compile/utf8_path.h"
#include "yaml_lite.h"

/// The asset manifest `manifest/assets.yaml` (Docs/design/26-layers-symbols-library.md §26.3.3,
/// Docs/design/12-compiler.md §12.7 lint category `assets`, Docs/tasks/T-183.md): reading,
/// writing, measuring PNG/JPEG/SVG files by their headers, and the validation rules.

namespace xt::bundle {

using compile::genericPathToUtf8;
using compile::pathFromUtf8;
using compile::pathToUtf8;

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

/// T-268: the number strtod(p, &end) read, through xt::fromChars so the process locale does not
/// matter: leading whitespace and one '+' are skipped as strtod skipped them. Returns the end of
/// the number, or `p` when there is none.
const char* readDouble(const char* p, double& value) {
    const char* q = p;
    while (*q == ' ' || (*q >= '\t' && *q <= '\r')) {
        ++q;
    }
    if (*q == '+' && q[1] != '-') {
        ++q;
    }
    const std::from_chars_result r = xt::fromChars(q, q + std::strlen(q), value);
    return r.ec == std::errc{} ? r.ptr : p;
}

uint32_t be32(std::string_view b, size_t at) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(b[at])) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(b[at + 1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(b[at + 2])) << 8) |
           static_cast<uint32_t>(static_cast<unsigned char>(b[at + 3]));
}

uint32_t be16(std::string_view b, size_t at) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(b[at])) << 8) |
           static_cast<uint32_t>(static_cast<unsigned char>(b[at + 1]));
}

ImageMeasure measurePng(std::string_view b) {
    ImageMeasure out;
    if (b.size() < 33 || b.substr(12, 4) != "IHDR") {
        return out;
    }
    out.w = static_cast<int>(be32(b, 16));
    out.h = static_cast<int>(be32(b, 20));
    const auto colorType = static_cast<unsigned char>(b[25]);
    out.alpha = colorType == 4 || colorType == 6;
    // A tRNS chunk (before IDAT) gives palette/grey/RGB images transparency too.
    size_t pos = 8;
    while (!out.alpha && pos + 8 <= b.size()) {
        const uint32_t length = be32(b, pos);
        const std::string_view type = b.substr(pos + 4, 4);
        if (type == "tRNS") {
            out.alpha = true;
        }
        if (type == "IDAT" || type == "IEND") {
            break;
        }
        pos += 12 + static_cast<size_t>(length);
    }
    out.ok = out.w > 0 && out.h > 0;
    return out;
}

ImageMeasure measureJpeg(std::string_view b) {
    ImageMeasure out;
    size_t pos = 2;
    while (pos + 4 <= b.size()) {
        if (static_cast<unsigned char>(b[pos]) != 0xFF) {
            return out;
        }
        while (pos < b.size() && static_cast<unsigned char>(b[pos]) == 0xFF) {
            ++pos;
        }
        if (pos >= b.size()) {
            return out;
        }
        const auto marker = static_cast<unsigned char>(b[pos]);
        ++pos;
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            continue;
        }
        if (marker == 0xD9 || marker == 0xDA || pos + 2 > b.size()) {
            return out;  // End of image / start of scan before any frame header.
        }
        const uint32_t length = be16(b, pos);
        const bool isSof =
            marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (isSof) {
            if (pos + 7 > b.size()) {
                return out;
            }
            out.h = static_cast<int>(be16(b, pos + 3));
            out.w = static_cast<int>(be16(b, pos + 5));
            out.ok = out.w > 0 && out.h > 0;
            return out;
        }
        pos += length;
    }
    return out;
}

/// The value of attribute `name` in the tag text `tag` (`<svg ...>`), or "" when absent.
std::string svgAttribute(std::string_view tag, std::string_view name) {
    size_t pos = 0;
    while (pos < tag.size()) {
        while (pos < tag.size() && !(std::isalpha(static_cast<unsigned char>(tag[pos])) != 0)) {
            ++pos;
        }
        const size_t nameStart = pos;
        while (pos < tag.size() && tag[pos] != '=' && tag[pos] != ' ' && tag[pos] != '>' &&
               tag[pos] != '\t' && tag[pos] != '\n' && tag[pos] != '\r') {
            ++pos;
        }
        const std::string_view attr = tag.substr(nameStart, pos - nameStart);
        while (pos < tag.size() &&
               (tag[pos] == ' ' || tag[pos] == '\t' || tag[pos] == '\n' || tag[pos] == '\r')) {
            ++pos;
        }
        if (pos >= tag.size() || tag[pos] != '=') {
            continue;
        }
        ++pos;
        while (pos < tag.size() &&
               (tag[pos] == ' ' || tag[pos] == '\t' || tag[pos] == '\n' || tag[pos] == '\r')) {
            ++pos;
        }
        if (pos >= tag.size() || (tag[pos] != '"' && tag[pos] != '\'')) {
            continue;
        }
        const char quote = tag[pos++];
        const size_t valueStart = pos;
        while (pos < tag.size() && tag[pos] != quote) {
            ++pos;
        }
        const std::string_view value = tag.substr(valueStart, pos - valueStart);
        ++pos;
        if (attr == name) {
            return std::string(value);
        }
    }
    return std::string();
}

/// A unitless or `px` length; -1 otherwise.
double svgLength(const std::string& text) {
    if (text.empty()) {
        return -1;
    }
    double value = 0;
    const char* end = readDouble(text.c_str(), value);
    std::string unit(end);
    while (!unit.empty() && unit.back() == ' ') {
        unit.pop_back();
    }
    if (end == text.c_str() || !(unit.empty() || unit == "px")) {
        return -1;
    }
    return value;
}

ImageMeasure measureSvg(std::string_view b) {
    ImageMeasure out;
    const size_t start = b.find("<svg");
    if (start == std::string_view::npos) {
        return out;
    }
    const size_t end = b.find('>', start);
    const std::string_view tag =
        b.substr(start + 4, end == std::string_view::npos ? std::string_view::npos : end - start - 4);
    double w = svgLength(svgAttribute(tag, "width"));
    double h = svgLength(svgAttribute(tag, "height"));
    if (w <= 0 || h <= 0) {
        std::string viewBox = svgAttribute(tag, "viewBox");
        std::replace(viewBox.begin(), viewBox.end(), ',', ' ');
        double values[4] = {0, 0, 0, 0};
        const char* cursor = viewBox.c_str();
        int count = 0;
        for (; count < 4; ++count) {
            const char* next = readDouble(cursor, values[count]);
            if (next == cursor) {
                break;
            }
            cursor = next;
        }
        if (count == 4) {
            w = w > 0 ? w : values[2];
            h = h > 0 ? h : values[3];
        }
    }
    if (w <= 0 || h <= 0) {
        return out;
    }
    out.w = static_cast<int>(w + 0.5);
    out.h = static_cast<int>(h + 0.5);
    out.alpha = true;
    out.ok = true;
    return out;
}

std::string lowerExt(std::string_view path) {
    std::string ext = pathToUtf8(pathFromUtf8(path).extension());
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return ext;
}

/// A JSON scalar as text: strings verbatim, other scalars dumped.
std::string scalarText(const json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_null()) {
        return std::string();
    }
    return value.dump();
}

bool readInt(const json& obj, const char* key, int& out) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_number()) {
        return false;
    }
    out = static_cast<int>(it->get<double>());
    return true;
}

/// The header line toYaml() writes (without its newline).
constexpr std::string_view kAssetsHeader =
    "# Asset manifest (docs/design/26-layers-symbols-library.md 26.3.3); xtframe-pack fills `measured`.";

/// T-262b (F10): true when `text` has a YAML comment -- a `#` that starts a line or follows a space
/// or tab, outside quotes -- other than toYaml()'s own header on the first line.
bool hasComment(std::string_view text) {
    size_t lineStart = 0;
    bool firstLine = true;
    while (lineStart < text.size()) {
        size_t lineEnd = text.find('\n', lineStart);
        if (lineEnd == std::string_view::npos) {
            lineEnd = text.size();
        }
        std::string_view line = text.substr(lineStart, lineEnd - lineStart);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (!(firstLine && line == kAssetsHeader)) {
            char quote = 0;
            for (size_t i = 0; i < line.size(); ++i) {
                const char c = line[i];
                if (quote != 0) {
                    if (c == quote) {
                        quote = 0;
                    }
                } else if ((c == '"' || c == '\'') &&
                           (i == 0 ||
                            std::string_view(" \t[{,").find(line[i - 1]) != std::string_view::npos)) {
                    quote = c;  // Only a quote that opens a scalar (not `it's`).
                } else if (c == '#' && (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t')) {
                    return true;
                }
            }
        }
        firstLine = false;
        lineStart = lineEnd + 1;
    }
    return false;
}

}  // namespace

ImageMeasure measureImage(std::string_view bytes, std::string_view ext) {
    if (bytes.size() >= 8 && bytes.substr(0, 8) == std::string_view("\x89PNG\r\n\x1a\n", 8)) {
        return measurePng(bytes);
    }
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xD8) {
        return measureJpeg(bytes);
    }
    std::string lower(ext);
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    if (lower == "svg" || (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".svg") == 0)) {
        return measureSvg(bytes);
    }
    return ImageMeasure{};
}

bool AssetsManifest::parse(std::string_view text, AssetsManifest& out, std::string* error) {
    json root;
    if (!detail::parseYamlLite(text, root, error)) {
        return false;
    }
    AssetsManifest manifest;
    manifest.hasUnsavableContent = hasComment(text);  // T-262b (F10).
    if (root.is_null()) {
        out = std::move(manifest);
        return true;
    }
    if (!root.is_object()) {
        if (error != nullptr) {
            *error = "the top level is not a mapping";
        }
        return false;
    }
    // T-406 (43 §43.2): `version` missing = 1, newer refused (no migration sequence of its own: the
    // file is generated from the metas, 26 §26.3.3).
    int version = 1;
    if (!readFormatVersion(root, version, error)) {
        return false;
    }
    if (version > kAssetsManifestVersion) {
        if (error != nullptr) {
            *error = "assets.yaml format version " + std::to_string(version) +
                     " is newer than this XTFrame supports (" + std::to_string(kAssetsManifestVersion) +
                     "); a newer XTFrame is needed";
        }
        return false;
    }
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (it.key() != "assets" && it.key() != "version") {
            manifest.hasUnsavableContent = true;  // T-262b: toYaml() writes only these two.
        }
    }
    const auto assetsIt = root.find("assets");
    if (assetsIt != root.end() && !assetsIt->is_null()) {
        if (!assetsIt->is_array()) {
            if (error != nullptr) {
                *error = "'assets' is not a list";
            }
            return false;
        }
        size_t index = 0;
        for (const json& item : *assetsIt) {
            ++index;
            if (!item.is_object()) {
                if (error != nullptr) {
                    *error = "asset " + std::to_string(index) + " is not a mapping";
                }
                return false;
            }
            AssetEntry entry;
            for (auto it = item.begin(); it != item.end(); ++it) {
                const std::string& key = it.key();
                const json& value = *it;
                // T-262b (F10): toYaml() reproduces scalars and the known `measured` fields only.
                if (key == "measured") {
                    if (value.is_object()) {
                        for (auto field = value.begin(); field != value.end(); ++field) {
                            if (field.key() != "w" && field.key() != "h" && field.key() != "alpha" &&
                                field.key() != "slice") {
                                manifest.hasUnsavableContent = true;
                            }
                        }
                    } else if (!value.is_null()) {
                        manifest.hasUnsavableContent = true;
                    }
                } else if (!value.is_primitive()) {
                    manifest.hasUnsavableContent = true;
                }
                if (key == "id") {
                    entry.id = scalarText(value);
                } else if (key == "type") {
                    entry.type = scalarText(value);
                } else if (key == "file") {
                    entry.file = scalarText(value);
                } else if (key == "source") {
                    entry.source = scalarText(value);
                } else if (key == "status") {
                    entry.status = value.is_null() ? std::string("draft") : scalarText(value);
                } else if (key == "license") {
                    entry.license = scalarText(value);
                } else if (key == "measured") {
                    if (!value.is_object()) {
                        continue;
                    }
                    AssetMeasured& m = entry.measured;
                    const bool hasW = readInt(value, "w", m.w);
                    const bool hasH = readInt(value, "h", m.h);
                    m.hasSize = hasW && hasH;
                    const auto alphaIt = value.find("alpha");
                    if (alphaIt != value.end() && alphaIt->is_boolean()) {
                        m.hasAlpha = true;
                        m.alpha = alphaIt->get<bool>();
                    }
                    const auto sliceIt = value.find("slice");
                    if (sliceIt != value.end()) {
                        std::vector<double> numbers;
                        if (sliceIt->is_array()) {
                            for (const json& n : *sliceIt) {
                                if (n.is_number()) {
                                    numbers.push_back(n.get<double>());
                                }
                            }
                        } else if (sliceIt->is_string()) {
                            // §26.3.4 spells it `slice: L R T B`.
                            const std::string sliceText = sliceIt->get<std::string>();
                            const char* cursor = sliceText.c_str();
                            while (true) {
                                double n = 0;
                                const char* next = readDouble(cursor, n);
                                if (next == cursor) {
                                    break;
                                }
                                numbers.push_back(n);
                                cursor = next;
                            }
                        }
                        if (numbers.size() == 4) {
                            m.hasSlice = true;
                            for (int i = 0; i < 4; ++i) {
                                m.slice[i] = static_cast<int>(numbers[static_cast<size_t>(i)]);
                            }
                        }
                    }
                } else if (value.is_primitive()) {
                    entry.extra.emplace_back(
                        key, value.is_string() ? detail::yamlScalar(value.get<std::string>()) : value.dump());
                }
            }
            manifest.assets.push_back(std::move(entry));
        }
    }
    out = std::move(manifest);
    return true;
}

std::string AssetsManifest::toYaml() const {
    std::string out =
        std::string(kAssetsHeader) + "\nversion: " + std::to_string(kAssetsManifestVersion) + "\n";
    if (assets.empty()) {
        out += "assets: []\n";
        return out;
    }
    out += "assets:\n";
    for (const AssetEntry& entry : assets) {
        out += "  - id: " + detail::yamlScalar(entry.id) + "\n";
        out += "    type: " + detail::yamlScalar(entry.type) + "\n";
        out += "    file: " + detail::yamlScalar(entry.file) + "\n";
        out += "    source: " + detail::yamlScalar(entry.source) + "\n";
        out += "    status: " + detail::yamlScalar(entry.status) + "\n";
        if (!entry.license.empty()) {
            out += "    license: " + detail::yamlScalar(entry.license) + "\n";
        }
        const AssetMeasured& m = entry.measured;
        if (m.hasSize || m.hasAlpha || m.hasSlice) {
            std::string fields;
            auto add = [&fields](const std::string& field) {
                fields += fields.empty() ? field : ", " + field;
            };
            if (m.hasSize) {
                add("w: " + std::to_string(m.w));
                add("h: " + std::to_string(m.h));
            }
            if (m.hasAlpha) {
                add(std::string("alpha: ") + (m.alpha ? "true" : "false"));
            }
            if (m.hasSlice) {
                add("slice: [" + std::to_string(m.slice[0]) + ", " + std::to_string(m.slice[1]) + ", " +
                    std::to_string(m.slice[2]) + ", " + std::to_string(m.slice[3]) + "]");
            }
            out += "    measured: { " + fields + " }\n";
        }
        for (const auto& [key, value] : entry.extra) {
            out += "    " + detail::yamlScalar(key) + ": " + value + "\n";
        }
    }
    return out;
}

Result<AssetsManifest> AssetsManifest::load(std::string_view path, std::string* error) {
    std::string text;
    if (!detail::readFile(std::string(path), text)) {
        if (error != nullptr) {
            *error = "cannot read '" + std::string(path) + "'";
        }
        return Result<AssetsManifest>::error(Status::kNotFound);
    }
    AssetsManifest manifest;
    std::string message;
    if (!parse(text, manifest, &message)) {
        if (error != nullptr) {
            *error = std::string(path) + ": " + message;
        }
        return Result<AssetsManifest>::error(Status::kInvalidArgument);
    }
    return Result<AssetsManifest>::ok(std::move(manifest));
}

Status AssetsManifest::save(std::string_view path) const {
    return detail::writeFile(std::string(path), toYaml()) ? Status::kOk : Status::kIoError;
}

const AssetEntry* AssetsManifest::find(std::string_view assetId) const {
    for (const AssetEntry& entry : assets) {
        if (entry.id == assetId) {
            return &entry;
        }
    }
    return nullptr;
}

std::string canonicalAssetPath(std::string_view path) {
    std::error_code ec;
    std::string out = genericPathToUtf8(fs::absolute(pathFromUtf8(path), ec).lexically_normal());
#ifdef _WIN32
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
#endif
    return out;
}

std::vector<AssetIssue> checkAssets(AssetsManifest& manifest, std::string_view rootDir,
                                    const std::vector<std::string>* referenced, bool* filled) {
    std::vector<AssetIssue> issues;
    auto error = [&issues](const std::string& id, std::string message) {
        issues.push_back(AssetIssue{true, id, std::move(message)});
    };
    auto warning = [&issues](const std::string& id, std::string message) {
        issues.push_back(AssetIssue{false, id, std::move(message)});
    };
    std::set<std::string> referencedSet;
    if (referenced != nullptr) {
        referencedSet.insert(referenced->begin(), referenced->end());
    }
    std::set<std::string> seen;
    size_t index = 0;
    for (AssetEntry& entry : manifest.assets) {
        ++index;
        if (entry.id.empty()) {
            error("", "asset " + std::to_string(index) + " has no id");
        } else if (!seen.insert(entry.id).second) {
            error(entry.id, "duplicate id");
        }
        const std::string& id = entry.id;
        if (entry.status != "draft" && entry.status != "approved" && entry.status != "stale" &&
            entry.status != "pending") {
            error(id, "status '" + entry.status + "' is not draft, approved, stale or pending");
        }
        if ((entry.source.rfind("ai", 0) == 0 || entry.source.rfind("external", 0) == 0) &&
            entry.license.empty()) {
            error(id, "source '" + entry.source + "' needs a license");
        }
        if (entry.file.empty()) {
            error(id, "no file");
            continue;
        }
        const fs::path path = pathFromUtf8(rootDir) / pathFromUtf8(entry.file);
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) {
            error(id, "file '" + entry.file + "' not found");
            continue;
        }

        AssetMeasured& m = entry.measured;
        const std::string ext = lowerExt(entry.file);
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".svg") {
            std::string bytes;
            const ImageMeasure actual =
                detail::readFile(pathToUtf8(path), bytes) ? measureImage(bytes, ext) : ImageMeasure{};
            if (!actual.ok) {
                warning(id, "cannot read the size of '" + entry.file + "'");
            } else {
                if (!m.hasSize) {
                    m.hasSize = true;
                    m.w = actual.w;
                    m.h = actual.h;
                    if (filled != nullptr) {
                        *filled = true;
                    }
                } else if (m.w != actual.w || m.h != actual.h) {
                    error(id, "declared size " + std::to_string(m.w) + "x" + std::to_string(m.h) + " but '" +
                                  entry.file + "' is " + std::to_string(actual.w) + "x" +
                                  std::to_string(actual.h));
                }
                if (!m.hasAlpha) {
                    m.hasAlpha = true;
                    m.alpha = actual.alpha;
                    if (filled != nullptr) {
                        *filled = true;
                    }
                } else if (m.alpha && !actual.alpha) {
                    error(id, "declared transparent but '" + entry.file + "' has no alpha channel");
                }
            }
        }
        if (m.hasSlice) {
            const int* s = m.slice;
            if (s[0] < 0 || s[1] < 0 || s[2] < 0 || s[3] < 0) {
                error(id, "slice has a negative margin");
            } else if (!m.hasSize) {
                error(id, "slice needs the asset's size");
            } else if (s[0] + s[1] >= m.w || s[2] + s[3] >= m.h) {
                error(id, "slice [" + std::to_string(s[0]) + ", " + std::to_string(s[1]) + ", " +
                              std::to_string(s[2]) + ", " + std::to_string(s[3]) +
                              "] leaves no stretchable centre in " + std::to_string(m.w) + "x" +
                              std::to_string(m.h));
            }
        }
        if (referenced != nullptr && referencedSet.count(canonicalAssetPath(pathToUtf8(path))) == 0) {
            warning(id, "'" + entry.file + "' is not referenced by any packed UI");
        }
    }
    return issues;
}

}  // namespace xt::bundle
