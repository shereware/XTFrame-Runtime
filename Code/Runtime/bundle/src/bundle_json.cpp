#include "bundle_json.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

#include "json_bounded.h"  // Code/Runtime/compile/src: T-262c, the one bounded parse (T-414)
#include "xt/bundle/bundle.h"
#include "xt/charconv.h"
#include "xt/compile/utf8_path.h"
#include "xt/num_guard.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>  // MoveFileExW (T-262b).
#endif

/// See bundle_json.h. Every accessor checks the JSON type before reading, so a hand-edited
/// manifest with a wrongly typed key degrades to the default instead of throwing.

namespace xt::bundle::detail {

using compile::pathFromUtf8;

json parseBounded(std::string_view text, int maxDepth, std::string& error) {
    return compile::detail::parseBounded(text, maxDepth, error);
}

namespace {

/// fopen wrapper: MSVC deprecates plain fopen under /W4 /WX. `path` is UTF-8 (T-237): Windows
/// opens it through the wide API, never the ANSI code page. T-500: openFileUtf8() in the header.
std::FILE* openFile(const std::string& path, const char* mode) {
    std::FILE* file = nullptr;
#if defined(_WIN32)
    const std::wstring wideMode(mode, mode + std::char_traits<char>::length(mode));
    if (_wfopen_s(&file, pathFromUtf8(path).c_str(), wideMode.c_str()) != 0) {
        file = nullptr;
    }
#elif defined(_MSC_VER)
    if (fopen_s(&file, path.c_str(), mode) != 0) {
        file = nullptr;
    }
#else
    file = std::fopen(path.c_str(), mode);
#endif
    return file;
}

/// A JSON number as a float. A double beyond the float range becomes +-infinity (T-258a: the plain
/// conversion would be undefined), which the callers' range checks then reject.
float jsonFloat(const json& v) {
    const double d = v.get<double>();
    if (std::fabs(d) <= static_cast<double>(std::numeric_limits<float>::max())) {
        return static_cast<float>(d);
    }
    return d > 0.0 ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity();
}

}  // namespace

std::FILE* openFileUtf8(const std::string& path, const char* mode) { return openFile(path, mode); }

namespace {

// T-240: setBundleFileReader().
BundleFileReader g_reader = nullptr;
void* g_readerUser = nullptr;

}  // namespace

bool readFile(const std::string& path, std::string& out) {
    if (g_reader != nullptr) {
        return g_reader(path, out, g_readerUser);
    }
    std::FILE* file = openFile(path, "rb");
    if (file == nullptr) {
        return false;
    }
    std::string text;
    char buffer[4096];
    size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        text.append(buffer, got);
    }
    const bool ok = std::ferror(file) == 0;
    std::fclose(file);
    if (!ok) {
        return false;
    }
    out = std::move(text);
    return true;
}

bool writeFile(const std::string& path, const std::string& text) {
    // T-262b (F17): written in full to `<path>.tmp` beside the target, then moved over it, so a
    // full disk or a killed process never leaves the target truncated. Any failure removes the
    // temporary file and leaves the target as it was.
    const std::string temp = path + ".tmp";
    std::FILE* file = openFile(temp, "wb");
    if (file == nullptr) {
        return false;
    }
    const size_t written = text.empty() ? 0 : std::fwrite(text.data(), 1, text.size(), file);
    bool ok = written == text.size() && std::ferror(file) == 0;
    ok = std::fclose(file) == 0 && ok;
#if defined(_WIN32)
    const std::wstring wideTemp = pathFromUtf8(temp).wstring();
    ok = ok && MoveFileExW(wideTemp.c_str(), pathFromUtf8(path).wstring().c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) {
        _wremove(wideTemp.c_str());
    }
#else
    ok = ok && std::rename(temp.c_str(), path.c_str()) == 0;
    if (!ok) {
        std::remove(temp.c_str());
    }
#endif
    return ok;
}

std::string joinPath(std::string_view dir, std::string_view name) {
    std::string path(dir);
    if (!path.empty() && path.back() != '/' && path.back() != '\\') {
        path += '/';
    }
    path.append(name);
    return path;
}

std::string getString(const json& obj, const char* key) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) {
        return {};
    }
    return it->get<std::string>();
}

bool getBool(const json& obj, const char* key, bool fallback) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_boolean()) {
        return fallback;
    }
    return it->get<bool>();
}

float getFloat(const json& obj, const char* key, float fallback) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_number()) {
        return fallback;
    }
    return jsonFloat(*it);
}

Vec2 getVec2(const json& obj, const char* key, Vec2 fallback) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_array() || it->size() != 2 || !(*it)[0].is_number() ||
        !(*it)[1].is_number()) {
        return fallback;
    }
    return Vec2{jsonFloat((*it)[0]), jsonFloat((*it)[1])};
}

bool isValidUiSize(Vec2 size) {
    auto ok = [](float v) { return std::isfinite(v) && v >= 1.0f && v <= kMaxUiSize; };
    return ok(size.x) && ok(size.y);
}

Vec2 clampThumbnail(Vec2 size, Vec2 fallback) {
    return Vec2{clampFinite(size.x, 0.0f, kMaxThumbnailSize, fallback.x),
                clampFinite(size.y, 0.0f, kMaxThumbnailSize, fallback.y)};
}

std::vector<std::string> getStringArray(const json& obj, const char* key) {
    std::vector<std::string> out;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_array()) {
        return out;
    }
    for (const json& entry : *it) {
        if (entry.is_string()) {
            out.push_back(entry.get<std::string>());
        }
    }
    return out;
}

uint64_t getUint64(const json& obj, const char* key) {
    const auto it = obj.find(key);
    if (it == obj.end()) {
        return 0;
    }
    if (it->is_number_unsigned()) {
        return it->get<uint64_t>();
    }
    if (it->is_string()) {
        // Hashes are written as strings when they do not survive a double round-trip; base 0 so
        // both "123" and "0x7b" parse.
        const std::string text = it->get<std::string>();
        return std::strtoull(text.c_str(), nullptr, 0);
    }
    return 0;
}

std::vector<TokenDef> getTokens(const json& root) {
    std::vector<TokenDef> out;
    const auto it = root.find("tokens");
    if (it == root.end() || !it->is_object()) {
        return out;
    }
    for (auto entry = it->begin(); entry != it->end(); ++entry) {
        if (entry.value().is_string()) {
            out.push_back(TokenDef{entry.key(), entry.value().get<std::string>()});
        }
    }
    return out;
}

std::vector<ThemeEntry> getThemes(const json& root) {
    std::vector<ThemeEntry> out;
    const auto it = root.find("themes");
    if (it == root.end() || !it->is_array()) {
        return out;
    }
    for (const json& entry : *it) {
        if (!entry.is_object()) {
            continue;
        }
        ThemeEntry theme{getString(entry, "name"), getString(entry, "file")};
        if (!theme.name.empty() && !theme.file.empty()) {
            out.push_back(std::move(theme));
        }
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> getStringsMap(const json& root) {
    std::vector<std::pair<std::string, std::string>> out;
    const auto it = root.find("strings");
    if (it == root.end() || !it->is_object()) {
        return out;
    }
    for (auto entry = it->begin(); entry != it->end(); ++entry) {
        if (!entry.key().empty() && entry.value().is_string() && !entry.value().get<std::string>().empty()) {
            out.emplace_back(entry.key(), entry.value().get<std::string>());
        }
    }
    return out;
}

bool getSpec(const json& root, BundleSpec& out) {
    const auto it = root.find("spec");
    if (it == root.end() || !it->is_object()) {
        return false;
    }
    const auto safeIt = it->find("safeArea");
    if (safeIt != it->end() && safeIt->is_object()) {
        out.safeAreaDefault = getFloat(*safeIt, "default", out.safeAreaDefault);
        out.safeAreaConsole = getFloat(*safeIt, "console", out.safeAreaConsole);
    }
    out.logicalHeight = getFloat(*it, "logicalHeight", out.logicalHeight);
    const auto ruleIt = it->find("uiscaleRule");  // T-241: missing or unknown = height.
    if (ruleIt != it->end() && ruleIt->is_string()) {
        parseUiScaleRule(ruleIt->get<std::string>(), out.uiscaleRule);
    }
    readUiScaleLimits(*it, out.uiscaleLimits);  // T-345
    // T-407: a non-negative integer; missing or anything else reads as 0.
    if (const auto behavior = it->find("behavior"); behavior != it->end() && behavior->is_number_unsigned()) {
        out.behavior = static_cast<uint32_t>(std::min<uint64_t>(behavior->get<uint64_t>(), 0xFFFFFFFFu));
    }
    return true;
}

void readUiScaleLimits(const json& spec, UiScaleLimits& out) {
    out = UiScaleLimits{};
    if (!spec.is_object()) {
        return;
    }
    const auto clampIt = spec.find("uiscaleClamp");
    if (clampIt != spec.end() && clampIt->is_array() && clampIt->size() == 2 && (*clampIt)[0].is_number()) {
        const float lo = (*clampIt)[0].get<float>();
        float hi = std::numeric_limits<float>::infinity();
        bool ok = std::isfinite(lo) && lo >= 0.0f;
        if ((*clampIt)[1].is_number()) {
            hi = (*clampIt)[1].get<float>();
            ok = ok && std::isfinite(hi) && hi >= lo;
        } else {
            ok = ok && (*clampIt)[1].is_null();
        }
        if (ok) {
            out.clamped = true;
            out.clampMin = lo;
            out.clampMax = hi;
        }
    }
    const float floor = getFloat(spec, "uiscaleDpiFloor", 0.0f);
    out.dpiFloor = std::isfinite(floor) && floor > 0.0f ? floor : 0.0f;
}

void writeUiScaleLimits(nlohmann::ordered_json& spec, const UiScaleLimits& limits) {
    if (limits.clamped) {
        nlohmann::ordered_json range = nlohmann::ordered_json::array();
        range.push_back(shortNumber(limits.clampMin));
        if (std::isfinite(limits.clampMax)) {
            range.push_back(shortNumber(limits.clampMax));
        } else {
            range.push_back(nullptr);
        }
        spec["uiscaleClamp"] = std::move(range);
    }
    if (limits.dpiFloor > 0.0f) {
        spec["uiscaleDpiFloor"] = shortNumber(limits.dpiFloor);
    }
}

std::vector<GlyphInfo> getGlyphs(const json& root) {
    std::vector<GlyphInfo> out;
    const auto it = root.find("glyphs");
    if (it == root.end() || !it->is_object()) {
        return out;
    }
    for (auto device = it->begin(); device != it->end(); ++device) {
        if (device.key().empty() || !device.value().is_object()) {
            continue;
        }
        for (auto key = device.value().begin(); key != device.value().end(); ++key) {
            if (!key.key().empty() && key.value().is_string() && !key.value().get<std::string>().empty()) {
                out.push_back(GlyphInfo{device.key(), key.key(), key.value().get<std::string>()});
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const GlyphInfo& a, const GlyphInfo& b) {
        return a.device != b.device ? a.device < b.device : a.key < b.key;
    });
    return out;
}

std::vector<std::pair<std::string, std::vector<std::string>>> getFontFallbacks(const json& root) {
    std::vector<std::pair<std::string, std::vector<std::string>>> out;
    const auto fonts = root.find("fonts");
    if (fonts == root.end() || !fonts->is_object()) {
        return out;
    }
    const auto it = fonts->find("fallback");
    if (it == fonts->end() || !it->is_object()) {
        return out;
    }
    for (auto entry = it->begin(); entry != it->end(); ++entry) {
        std::vector<std::string> families = getStringArray(*it, entry.key().c_str());
        // T-555 (T-527c): the empty key is the default chain and is kept.
        if (!families.empty()) {
            out.emplace_back(entry.key(), std::move(families));
        }
    }
    return out;
}

std::vector<SymbolContract> getSymbolContracts(const json& root) {
    std::vector<SymbolContract> out;
    const auto it = root.find("symbols");
    if (it == root.end() || !it->is_array()) {
        return out;
    }
    for (const json& entry : *it) {
        if (!entry.is_object()) {
            continue;
        }
        SymbolContract c;
        c.name = getString(entry, "name");
        if (c.name.empty()) {
            continue;
        }
        c.source = getString(entry, "source");
        const auto req = entry.find("requires");
        if (req != entry.end() && req->is_object()) {
            c.requiredStates = getStringArray(*req, "states");
            c.requiredSlots = getStringArray(*req, "slots");
        }
        const auto groups = entry.find("stateGroups");
        if (groups != entry.end() && groups->is_array()) {
            for (const json& g : *groups) {
                if (g.is_object()) {
                    c.stateGroups.push_back(
                        SymbolStateGroup{getString(g, "name"), getStringArray(g, "states")});
                }
            }
        }
        c.slotNames = getStringArray(entry, "slots");
        const auto ports = entry.find("ports");
        if (ports != entry.end() && ports->is_array()) {
            for (const json& p : *ports) {
                if (p.is_object() && !getString(p, "name").empty()) {
                    c.ports.push_back(
                        SymbolPortInfo{getString(p, "name"), getString(p, "type"), getString(p, "dir")});
                }
            }
        }
        out.push_back(std::move(c));
    }
    return out;
}

bool isValidId(std::string_view id) {
    if (id.empty()) {
        return false;
    }
    for (char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

std::vector<AtlasJson> getAtlases(const json& root) {
    std::vector<AtlasJson> out;
    const auto it = root.find("atlas");
    if (it == root.end() || !it->is_array()) {
        return out;
    }
    auto getInt = [](const json& obj, const char* key) {
        const auto v = obj.find(key);
        return (v != obj.end() && v->is_number()) ? static_cast<int>(v->get<double>()) : 0;
    };
    for (const json& entry : *it) {
        if (!entry.is_object()) {
            continue;
        }
        AtlasJson page;
        page.name = getString(entry, "name");
        page.file = getString(entry, "file");
        page.format = getString(entry, "format");
        const auto variants = entry.find("variants");
        if (variants != entry.end() && variants->is_object()) {
            page.file2x = getString(*variants, "2x");
        }
        page.w = getInt(entry, "w");
        page.h = getInt(entry, "h");
        const auto entries = entry.find("entries");
        if (entries != entry.end() && entries->is_array()) {
            for (const json& row : *entries) {
                if (!row.is_object()) {
                    continue;
                }
                AtlasEntryJson e;
                e.asset = getString(row, "asset");
                e.x = getInt(row, "x");
                e.y = getInt(row, "y");
                e.w = getInt(row, "w");
                e.h = getInt(row, "h");
                if (!e.asset.empty()) {
                    page.entries.push_back(std::move(e));
                }
            }
        }
        if (!page.file.empty()) {
            out.push_back(std::move(page));
        }
    }
    return out;
}

nlohmann::ordered_json shortNumber(float value) {
    const auto rounded = static_cast<int64_t>(value);
    if (static_cast<float>(rounded) == value) {
        return nlohmann::ordered_json(rounded);
    }
    char buffer[32] = {};
    const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer) - 1, value);
    double widened = 0;
    xt::fromChars(buffer, result.ptr, widened);
    return nlohmann::ordered_json(widened);
}

std::filesystem::path findStandardSymbol(const std::filesystem::path& library,
                                         const std::filesystem::path& fileName) {
    std::error_code ec;
    for (const std::filesystem::path& candidate :
         {library / fileName, library / "mobile" / fileName, library / "list" / fileName}) {
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }
    return std::filesystem::path();
}

}  // namespace xt::bundle::detail

namespace xt::bundle {

// T-241 (Docs/design/23-host-integration.md §23.11): `spec.uiscaleRule`.
const char* uiScaleRuleName(UiScaleRule rule) {
    switch (rule) {
        case UiScaleRule::Width:
            return "width";
        case UiScaleRule::ShortEdge:
            return "shortEdge";
        case UiScaleRule::Fit:  // T-345
            return "fit";
        case UiScaleRule::Height:
            break;
    }
    return "height";
}

bool parseUiScaleRule(std::string_view text, UiScaleRule& out) {
    if (text == "height") {
        out = UiScaleRule::Height;
    } else if (text == "width") {
        out = UiScaleRule::Width;
    } else if (text == "shortEdge") {
        out = UiScaleRule::ShortEdge;
    } else if (text == "fit") {  // T-345
        out = UiScaleRule::Fit;
    } else {
        return false;
    }
    return true;
}

float uiScaleFor(UiScaleRule rule, float width, float height, float logicalHeight) {
    if (rule == UiScaleRule::Fit) {
        // T-345 (25 §25.4.2 item 1): min(outW / designW, outH / designH).
        if (!(logicalHeight > 0.0f) || !(width > 0.0f) || !(height > 0.0f)) {
            return 1.0f;
        }
        const float byWidth = width / (logicalHeight * kFitDesignAspect);
        const float byHeight = height / logicalHeight;
        return byWidth < byHeight ? byWidth : byHeight;
    }
    const float edge = rule == UiScaleRule::Width       ? width
                       : rule == UiScaleRule::ShortEdge ? (width < height ? width : height)
                                                        : height;
    if (!(logicalHeight > 0.0f) || !(edge > 0.0f)) {
        return 1.0f;
    }
    return edge / logicalHeight;
}

void setBundleFileReader(BundleFileReader reader, void* user) {
    detail::g_reader = reader;
    detail::g_readerUser = user;
}

}  // namespace xt::bundle
