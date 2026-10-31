#include "xt/bundle/ui_project.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <json.hpp>
#include <set>
#include <string>
#include <utility>

#include "bundle_json.h"
#include "xt/bundle/design_system.h"
#include "xt/bundle/migrations.h"
#include "xt/charconv.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/dom.h"
#include "xt/compile/parse_xt.h"
#include "xt/compile/utf8_path.h"

/// UiProject: reading and writing the UI Project root file `<name>.xtui` (T-214; the old
/// `ui-project.json` and `bundle.xtproj` are read only), and `xtframe-pack --upgrade`. See
/// Docs/design/30-ui-project.md §30.1/§30.2/§30.5, Docs/design/22-bundle.md §22.10/§22.11.1 and
/// Docs/tasks/T-183.md, Docs/tasks/T-214.md.
///
/// Parsing is non-throwing, as in project_file.cpp: json::parse's `allow_exceptions = false` plus
/// bundle_json.h's type-checked accessors. save() emits ordered_json, so its output is byte-stable.
/// Keys the reader does not know are kept (UiProject::extraJson/extraSpecJson) and written back
/// after the known ones, so a hand-added key survives an editor save.

namespace xt::bundle {

using detail::getBool;
using detail::getFloat;
using detail::getString;
using detail::getStringArray;
using detail::getVec2;
using detail::isValidId;
using detail::joinPath;
using detail::readUiScaleLimits;
using detail::writeUiScaleLimits;
using nlohmann::json;
using nlohmann::ordered_json;

namespace fs = std::filesystem;
using compile::genericPathToUtf8;
using compile::pathFromUtf8;
using compile::pathToUtf8;

namespace {

void setErrorUiProject(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

Result<UiProject> failUiProject(Status status, std::string* error, std::string message) {
    setErrorUiProject(error, std::move(message));
    return Result<UiProject>::error(status);
}

std::string trimTrailingSlashUiProject(std::string_view dir) {
    std::string out(dir);
    while (out.size() > 1 && (out.back() == '/' || out.back() == '\\')) {
        out.pop_back();
    }
    return out;
}

ordered_json writeVec2UiProject(Vec2 v) {
    ordered_json out = ordered_json::array();
    out.push_back(ordered_json());
    out.push_back(ordered_json());
    out[0] = detail::shortNumber(v.x);
    out[1] = detail::shortNumber(v.y);
    return out;
}

int getIntUiProject(const json& obj, const char* key, int fallback) {
    const auto it = obj.find(key);
    return (it != obj.end() && it->is_number_integer()) ? it->get<int>() : fallback;
}

/// T-258a: `publish.atlas` page size and padding limits (px); a `.xtui` value outside is clamped.
constexpr int kMinAtlasSize = 64;
constexpr int kMaxAtlasSize = 16384;
constexpr int kMaxAtlasPadding = 64;

/// getInt() clamped to [lo, hi] (0 <= lo <= hi), read at full width so a value past the int range
/// clamps too.
int getIntClamped(const json& obj, const char* key, int fallback, int lo, int hi) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_number_integer()) {
        return fallback;
    }
    if (it->is_number_unsigned()) {
        return static_cast<int>(
            std::clamp<uint64_t>(it->get<uint64_t>(), static_cast<uint64_t>(lo), static_cast<uint64_t>(hi)));
    }
    return static_cast<int>(std::clamp<int64_t>(it->get<int64_t>(), lo, hi));
}

/// `obj[key]` when it is an object, else an empty object.
const json& objectAt(const json& obj, const char* key) {
    static const json kEmpty = json::object();
    const auto it = obj.find(key);
    return (it != obj.end() && it->is_object()) ? *it : kEmpty;
}

/// Reads one `uis` entry; the rules are ProjectFile::load()'s.
bool readUi(const json& entry, std::set<std::string>& seen, ProjectUi& ui, std::string& message) {
    if (!entry.is_object()) {
        message = "'uis' contains a non-object entry";
        return false;
    }
    ui.id = getString(entry, "id");
    if (!isValidId(ui.id)) {
        message = "UI id '" + ui.id + "' is missing or not [a-z0-9-]+";
        return false;
    }
    if (!seen.insert(ui.id).second) {
        message = "duplicate UI id '" + ui.id + "'";
        return false;
    }
    ui.doc = getString(entry, "doc");
    if (ui.doc.empty()) {
        message = "UI '" + ui.id + "' has no 'doc'";
        return false;
    }
    ui.flow = getString(entry, "flow");
    ui.size = getVec2(entry, "size", Vec2{1280, 720});
    if (!detail::isValidUiSize(ui.size)) {  // T-258a
        message = "UI '" + ui.id + "' has an invalid 'size' (expected two numbers in 1..16384)";
        return false;
    }
    ui.tags = getStringArray(entry, "tags");
    ui.fit = getString(entry, "fit");
    if (ui.fit.empty()) {
        ui.fit = "native";
    }
    return true;
}

ordered_json writeUi(const ProjectUi& ui) {
    ordered_json entry;
    entry["id"] = ui.id;
    entry["doc"] = ui.doc;
    if (!ui.flow.empty()) {
        entry["flow"] = ui.flow;
    }
    entry["size"] = writeVec2UiProject(ui.size);
    if (!ui.tags.empty()) {
        entry["tags"] = ui.tags;
    }
    if (!ui.fit.empty() && ui.fit != "native") {
        entry["fit"] = ui.fit;
    }
    return entry;
}

/// A string-or-default reader for keys that may legitimately be "" (e.g. `shared.tokens`).
std::string getStringOr(const json& obj, const char* key, const std::string& fallback) {
    const auto it = obj.find(key);
    return (it != obj.end() && it->is_string()) ? it->get<std::string>() : fallback;
}

void readSpec(const json& obj, ProjectSpec& spec) {
    const auto platformsIt = obj.find("platforms");
    if (platformsIt != obj.end() && platformsIt->is_array()) {
        for (const json& entry : *platformsIt) {
            if (!entry.is_object()) {
                continue;
            }
            SpecPlatform platform;
            platform.name = getString(entry, "name");
            const auto resIt = entry.find("resolutions");
            if (resIt != entry.end() && resIt->is_array()) {
                for (const json& res : *resIt) {
                    if (res.is_array() && res.size() == 2 && res[0].is_number() && res[1].is_number()) {
                        platform.resolutions.push_back(Vec2{res[0].get<float>(), res[1].get<float>()});
                    }
                }
            }
            spec.platforms.push_back(std::move(platform));
        }
    }
    // T-407 (43 §43.3): a non-negative integer; missing or anything else reads as 0.
    spec.behavior = 0;
    if (const auto behavior = obj.find("behavior"); behavior != obj.end() && behavior->is_number_integer()) {
        const int64_t value = behavior->get<int64_t>();
        spec.behavior = value > 0 ? static_cast<uint32_t>(std::min<int64_t>(value, 0xFFFFFFFF)) : 0;
    }
    const json& safeArea = objectAt(obj, "safeArea");
    spec.safeAreaDefault = getFloat(safeArea, "default", spec.safeAreaDefault);
    spec.safeAreaConsole = getFloat(safeArea, "console", spec.safeAreaConsole);
    // T-210: a missing, non-positive or non-finite value is the default 1080.
    const float logicalHeight = getFloat(obj, "logicalHeight", 1080.0f);
    spec.logicalHeight = std::isfinite(logicalHeight) && logicalHeight > 0.0f ? logicalHeight : 1080.0f;
    spec.uiscaleRule = UiScaleRule::Height;  // T-241: missing or unknown = height.
    parseUiScaleRule(getString(obj, "uiscaleRule"), spec.uiscaleRule);
    readUiScaleLimits(obj, spec.uiscaleLimits);  // T-345
    // T-345 (25 §25.4.2): `responsive`, kept verbatim for the editor.
    spec.responsive = SpecResponsive{};
    if (const auto responsive = obj.find("responsive"); responsive != obj.end() && responsive->is_object()) {
        spec.responsive.present = true;
        const auto readValue = [&](const char* key, std::string& slot) {
            if (const auto value = responsive->find(key); value != responsive->end() && value->is_string()) {
                slot = value->get<std::string>();
            }
        };
        readValue("scale", spec.responsive.scale);
        readValue("position", spec.responsive.position);
        readValue("visibility", spec.responsive.visibility);
    }
    const json& input = objectAt(obj, "input");
    spec.inputMethods = getStringArray(input, "methods");
    spec.players = getIntUiProject(input, "players", spec.players);
    spec.languages = getStringArray(obj, "languages");
    const auto fallbackIt = obj.find("fontFallback");
    if (fallbackIt != obj.end() && fallbackIt->is_object()) {
        for (auto it = fallbackIt->begin(); it != fallbackIt->end(); ++it) {
            FontFallbackInfo info;
            info.lang = it.key();
            if (it->is_array()) {
                for (const json& family : *it) {
                    if (family.is_string()) {
                        info.families.push_back(family.get<std::string>());
                    }
                }
            }
            spec.fontFallback.push_back(std::move(info));
        }
    }
    const json& budget = objectAt(obj, "budget");
    spec.budgetFrameMs = getFloat(budget, "frameMs", 0.0f);
    spec.budgetEffectLayers = getIntUiProject(budget, "effectLayers", 0);
    spec.budgetFillRate = getFloat(budget, "fillRate", 0.0f);
    spec.budgetParticles = getIntUiProject(budget, "particles", 0);  // T-202
    // T-312a: a positive number; anything else reads as 0 (unset).
    if (const auto bytes = budget.find("assetBytes"); bytes != budget.end() && bytes->is_number()) {
        const double value = bytes->get<double>();
        spec.budgetAssetBytes =
            std::isfinite(value) && value > 0.0 ? static_cast<uint64_t>(std::min(value, 9.0e15)) : 0;
    }
    const json& engine = objectAt(obj, "engine");
    spec.engine = getString(engine, "name");
    spec.engineVersion = getString(engine, "version");
}

ordered_json writeSpec(const ProjectSpec& spec) {
    ordered_json out;
    if (spec.behavior != 0) {
        out["behavior"] = spec.behavior;  // T-407: written only when set.
    }
    ordered_json platforms = ordered_json::array();
    for (const SpecPlatform& platform : spec.platforms) {
        ordered_json entry;
        entry["name"] = platform.name;
        ordered_json resolutions = ordered_json::array();
        for (const Vec2& res : platform.resolutions) {
            resolutions.push_back(writeVec2UiProject(res));
        }
        entry["resolutions"] = std::move(resolutions);
        platforms.push_back(std::move(entry));
    }
    out["platforms"] = std::move(platforms);
    ordered_json safeArea;
    safeArea["default"] = detail::shortNumber(spec.safeAreaDefault);
    safeArea["console"] = detail::shortNumber(spec.safeAreaConsole);
    out["safeArea"] = std::move(safeArea);
    if (spec.logicalHeight != 1080.0f) {
        out["logicalHeight"] = detail::shortNumber(spec.logicalHeight);  // T-210: written only when set.
    }
    if (spec.uiscaleRule != UiScaleRule::Height) {
        out["uiscaleRule"] = uiScaleRuleName(spec.uiscaleRule);  // T-241: written only when set.
    }
    writeUiScaleLimits(out, spec.uiscaleLimits);  // T-345: each written only when set.
    const SpecResponsive& responsive = spec.responsive;
    if (responsive.present || responsive.scale != "auto" || responsive.position != "follow" ||
        responsive.visibility != "auto") {
        ordered_json r;
        r["scale"] = responsive.scale;
        r["position"] = responsive.position;
        r["visibility"] = responsive.visibility;
        out["responsive"] = std::move(r);
    }
    ordered_json input;
    input["methods"] = spec.inputMethods;
    input["players"] = spec.players;
    out["input"] = std::move(input);
    out["languages"] = spec.languages;
    ordered_json fallback = ordered_json::object();
    for (const FontFallbackInfo& info : spec.fontFallback) {
        fallback[info.lang] = info.families;
    }
    out["fontFallback"] = std::move(fallback);
    ordered_json budget;
    budget["frameMs"] = detail::shortNumber(spec.budgetFrameMs);
    budget["effectLayers"] = spec.budgetEffectLayers;
    budget["fillRate"] = detail::shortNumber(spec.budgetFillRate);
    if (spec.budgetParticles > 0) {
        budget["particles"] = spec.budgetParticles;  // T-202: written only when set.
    }
    if (spec.budgetAssetBytes > 0) {
        budget["assetBytes"] = spec.budgetAssetBytes;  // T-312a: written only when set.
    }
    out["budget"] = std::move(budget);
    ordered_json engine;
    engine["name"] = spec.engine;
    engine["version"] = spec.engineVersion;
    out["engine"] = std::move(engine);
    return out;
}

/// The keys of `obj` not in `known`, as a JSON object text; "" when there are none.
std::string extraKeys(const json& obj, std::initializer_list<const char*> known) {
    json extra = json::object();
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        bool isKnown = false;
        for (const char* key : known) {
            isKnown = isKnown || it.key() == key;
        }
        if (!isKnown) {
            extra[it.key()] = *it;
        }
    }
    return extra.empty() ? std::string() : extra.dump();
}

/// Appends the keys of the JSON object text `extraText` to `out` (existing keys win).
void appendExtra(ordered_json& out, const std::string& extraText) {
    if (extraText.empty()) {
        return;
    }
    // T-262c: not parseBounded(): `extraText` is extraKeys()' dump of a project file that
    // load() already parsed with parseBounded(), so it is bounded by its source.
    const ordered_json extra = ordered_json::parse(extraText, nullptr, /*allow_exceptions=*/false);
    if (extra.is_discarded() || !extra.is_object()) {
        return;
    }
    for (auto it = extra.begin(); it != extra.end(); ++it) {
        if (!out.contains(it.key())) {
            out[it.key()] = *it;
        }
    }
}

constexpr std::string_view kMotionSpecYaml =
    "# Motion spec (docs/design/29-ai-endpoint.md 29.6): calibrated defaults, adjust per project.\n"
    "version: 1\n"
    "duration:  { instant: [0, 50], fast: [100, 200], normal: [200, 300], close: [200, 450], slow: [300, "
    "500], cinematic: [800, 2000] }\n"
    "easing:    { opacity: linear, move: ease-out, exit: ease-in, emphasis: back-out, spring: off }\n"
    "selection: snap            # snap (<=50ms, AAA) | soft (100-200ms)\n"
    "entrance:  { simultaneous_max: 3, stagger: off, stagger_ms: [40, 80], reveal_ms: [300, 400] }\n"
    "ambient:   { hud: 1, menu: 2, other: 1 }          # ambient loops per screen, at most\n"
    "particles: { hud: 0, menu: 1, results: 1 }\n"
    "celebration_per_screen: 1\n"
    "effects:   { backdrop_blur: { hud: 0, menu: 0, pause: 2, panel: 2 }, effect_layers_max: 24, "
    "fill_rate_max: 3.0 }\n"
    "overlay:   dim                # opening a panel dims, does not blur, by default\n"
    "safe_area: { default: 0.025, console: 0.05, applies_to: floating }   # edge-docked bars are fine\n"
    "text:      { min_px: 12, hud_default_px: 14, contrast_min: 4.5, font_families_max: 3 }\n"
    "animate_only: [transform, opacity]   # animating layout properties -> warning\n"
    "reduced_motion: crossfade\n";

constexpr std::string_view kAgentsMd =
    "# AGENTS.md\n"
    "\n"
    "This directory is an XTFrame UI Project (docs/design/30-ui-project.md). Read this file first.\n"
    "Read design/DESIGN.md first when it exists: it is the project's design system (colours, type,\n"
    "rules, references).\n"
    "\n"
    "## Layout\n"
    "\n"
    "- `<name>.xtui`: project identity (name, game, version), `spec` (platforms and resolutions,\n"
    "  safe-area rules, input methods, languages, performance budget, target engine), the UI list\n"
    "  (`uis`), the shared resources (`shared`) and the publish settings (`publish`).\n"
    "- `ui/<name>/`: each UI's `.xt`, `.xts`, `.xtflow`, `.xtanim.json` and `.xteditor.json`.\n"
    "- `shared/`: symbols, styles, `tokens.xts`, themes, images, fonts, animations, video, effects,\n"
    "  string tables (`strings/<lang>.csv`) and motion recipes.\n"
    "- `conventions/`: layout templates, naming rules, state and timeline conventions.\n"
    "- `design/`: the design system: palette, variable notes, type, rules, decision records and\n"
    "  references (moodboards, screenshots, with licenses); `DESIGN.md` is generated from it.\n"
    "- `contracts/`: scenario contracts, tag dictionary references, `motion-spec.yaml`.\n"
    "- `manifest/`: `assets.yaml` (asset list, measured sizes, approval status, provenance).\n"
    "\n"
    "## Rules\n"
    "\n"
    "- Use design tokens (`var(--...)`) instead of literal colors, sizes and durations.\n"
    "- Stay inside `contracts/motion-spec.yaml`.\n"
    "- Follow the layouts and naming in `conventions/`.\n"
    "- Assets you create are `draft`; only a person marks them `approved`.\n"
    "- A UI root fills the viewport; anchor key elements to the safe area (`safe-*` units, 25 §25.4.1).\n"
    "  A fixed px root is lint `layout.fixedRoot`; the Editor Service's `repair.anchor` converts it.\n"
    "- `design/`, `contracts/` and `shared/recipes/` never ship in the bundle.\n";

}  // namespace

namespace {

/// The old bundle source file (ProjectFile), read only.
constexpr const char* kBundleProjectFileName = "bundle.xtproj";

/// ASCII lower-case copy.
std::string lowerAsciiUiProject(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

/// True when `fileName` ends in ".xtui" (any case).
bool hasXtuiExtension(std::string_view fileName) {
    const std::string_view ext = kUiProjectExtension;
    return fileName.size() > ext.size() &&
           lowerAsciiUiProject(fileName.substr(fileName.size() - ext.size())) == ext;
}

/// The `*.xtui` regular files directly in `dir`, sorted by name.
std::vector<fs::path> xtuiFilesIn(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fileEc;
        if (it->is_regular_file(fileEc) && hasXtuiExtension(pathToUtf8(it->path().filename()))) {
            out.push_back(it->path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// True when `text` is well-formed UTF-8 (T-237: a name such as "測試專案" is a usable stem).
bool validUtf8(std::string_view text) {
    for (size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        const size_t length = lead < 0x80           ? 1
                              : (lead >> 5) == 0x6  ? 2
                              : (lead >> 4) == 0xE  ? 3
                              : (lead >> 3) == 0x1E ? 4
                                                    : 0;
        if (length == 0 || (length > 1 && lead < 0xC2) || i + length > text.size()) {
            return false;
        }
        for (size_t k = 1; k < length; ++k) {
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) {
                return false;
            }
        }
        i += length;
    }
    return true;
}

/// True when `name` can be used as a file name stem on every platform (T-214; T-237: UTF-8
/// letters are allowed, only characters no file system takes fall back to the id).
bool usableFileStem(std::string_view name) {
    if (name.empty() || name == "." || name == ".." || name.front() == ' ' || name.back() == ' ' ||
        name.back() == '.' || !validUtf8(name)) {
        return false;
    }
    for (const char c : name) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F ||
            std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos) {
            return false;
        }
    }
    // Windows device names, alone or before an extension ("CON", "com1.txt").
    std::string stem = lowerAsciiUiProject(name.substr(0, name.find('.')));
    while (!stem.empty() && stem.back() == ' ') {
        stem.pop_back();
    }
    for (const char* reserved : {"con", "prn", "aux", "nul"}) {
        if (stem == reserved) {
            return false;
        }
    }
    if (stem.size() == 4 && (stem.rfind("com", 0) == 0 || stem.rfind("lpt", 0) == 0) && stem[3] >= '1' &&
        stem[3] <= '9') {
        return false;
    }
    return true;
}

}  // namespace

std::string uiProjectFileName(std::string_view name, std::string_view id) {
    return std::string(usableFileStem(name) ? name : id) + kUiProjectExtension;
}

bool isUiProjectFilePath(std::string_view path) {
    const size_t slash = path.find_last_of("/\\");
    const std::string_view fileName = slash == std::string_view::npos ? path : path.substr(slash + 1);
    return hasXtuiExtension(fileName) || fileName == kLegacyUiProjectFileName ||
           fileName == kBundleProjectFileName;
}

std::string findUiProjectFile(std::string_view dir, LegacyKind* kind, std::string* error) {
    setErrorUiProject(error, std::string());
    const fs::path root = pathFromUtf8(dir);
    const std::vector<fs::path> xtui = xtuiFilesIn(root);
    if (xtui.size() > 1) {
        setErrorUiProject(error, "more than one .xtui in '" + std::string(dir) + "'");
        return std::string();
    }
    std::error_code ec;
    if (xtui.size() == 1) {
        if (kind != nullptr) {
            *kind = LegacyKind::kNone;
        }
        return joinPath(dir, pathToUtf8(xtui.front().filename()));
    }
    for (const auto& [name, legacyKind] : {std::pair{kLegacyUiProjectFileName, LegacyKind::kUiProjectJson},
                                           std::pair{kBundleProjectFileName, LegacyKind::kBundleXtproj}}) {
        const std::string path = joinPath(dir, name);
        if (fs::is_regular_file(pathFromUtf8(path), ec)) {
            if (kind != nullptr) {
                *kind = legacyKind;
            }
            return path;
        }
    }
    return std::string();
}

bool hasUiProjectFile(std::string_view dir) {
    std::error_code ec;
    return !xtuiFilesIn(pathFromUtf8(dir)).empty() ||
           fs::is_regular_file(pathFromUtf8(joinPath(dir, kLegacyUiProjectFileName)), ec);
}

std::string_view defaultMotionSpecYaml() { return kMotionSpecYaml; }

std::string_view agentsTemplate() { return kAgentsMd; }

Result<UiProject> UiProject::load(std::string_view input, std::string* error) {
    setErrorUiProject(error, std::string());
    std::error_code ec;
    const fs::path inputPath = pathFromUtf8(input);
    std::string dir;
    std::string path;
    LegacyKind kind = LegacyKind::kNone;
    if (fs::is_regular_file(inputPath, ec)) {
        // T-214: a root file given directly.
        const std::string fileName = pathToUtf8(inputPath.filename());
        if (hasXtuiExtension(fileName)) {
            kind = LegacyKind::kNone;
        } else if (fileName == kLegacyUiProjectFileName) {
            kind = LegacyKind::kUiProjectJson;
        } else if (fileName == kBundleProjectFileName) {
            kind = LegacyKind::kBundleXtproj;
        } else {
            return failUiProject(Status::kInvalidArgument, error,
                                 "'" + std::string(input) +
                                     "' is not a UI Project file (.xtui, ui-project.json or bundle.xtproj)");
        }
        dir = pathToUtf8(inputPath.parent_path());
        if (dir.empty()) {
            dir = ".";
        }
        path = std::string(input);
    } else {
        dir = std::string(input);
        std::string findError;
        path = findUiProjectFile(dir, &kind, &findError);
        if (!findError.empty()) {
            return failUiProject(Status::kInvalidArgument, error, findError);
        }
        if (path.empty()) {
            return failUiProject(
                Status::kNotFound, error,
                "no .xtui, ui-project.json or bundle.xtproj in '" + std::string(input) + "'");
        }
    }

    if (kind == LegacyKind::kBundleXtproj) {
        std::string legacyError;
        Result<ProjectFile> legacy = ProjectFile::load(dir, &legacyError);
        if (!legacy.ok()) {
            return failUiProject(legacy.status(), error, legacyError);
        }
        UiProject project = fromBundleProject(legacy.value());
        project.legacy = true;
        project.legacyKind = LegacyKind::kBundleXtproj;
        return Result<UiProject>::ok(std::move(project));
    }

    std::string text;
    if (!detail::readFile(path, text)) {
        return failUiProject(Status::kNotFound, error, "cannot read '" + path + "'");
    }

    std::string depthError;  // T-262c
    json root = detail::parseBounded(text, detail::kMaxJsonDepth, depthError);
    if (!depthError.empty()) {
        return failUiProject(Status::kInvalidArgument, error, path + ": " + depthError);
    }
    if (root.is_discarded() || !root.is_object()) {
        return failUiProject(Status::kIoError, error, "'" + path + "' is not a JSON object");
    }
    // T-406 (43 §43.2): missing = 1; older versions are migrated, newer ones refused.
    int fromVersion = kUiProjectFormatVersion;
    std::string migrateError;
    if (const Status migrated = migrateToLatest(SourceFormat::UiProject, root, &fromVersion, &migrateError);
        migrated != Status::kOk) {
        return failUiProject(migrated, error, "'" + path + "': " + migrateError);
    }

    UiProject project;
    project.version = kUiProjectFormatVersion;
    project.migratedFrom = fromVersion < kUiProjectFormatVersion ? fromVersion : 0;
    project.rootDir = trimTrailingSlashUiProject(dir);
    project.legacyKind = kind;
    project.name = getString(root, "name");
    project.id = getString(root, "id");
    if (project.id.empty()) {
        return failUiProject(Status::kInvalidArgument, error, "'" + path + "' has no 'id'");
    }
    if (!isValidId(project.id)) {
        return failUiProject(Status::kInvalidArgument, error,
                             "project id '" + project.id + "' is not [a-z0-9-]+");
    }
    project.game = getString(root, "game");

    const json& specObj = objectAt(root, "spec");
    readSpec(specObj, project.spec);
    project.extraSpecJson = extraKeys(
        specObj, {"behavior", "platforms", "safeArea", "logicalHeight", "uiscaleRule", "uiscaleClamp",
                  "uiscaleDpiFloor", "responsive", "input", "languages", "fontFallback", "budget", "engine"});

    const auto uisIt = root.find("uis");
    if (uisIt == root.end() || !uisIt->is_array()) {
        return failUiProject(Status::kInvalidArgument, error, "'" + path + "' has no 'uis' array");
    }
    std::set<std::string> seen;
    for (const json& entry : *uisIt) {
        ProjectUi ui;
        std::string message;
        if (!readUi(entry, seen, ui, message)) {
            return failUiProject(Status::kInvalidArgument, error, message);
        }
        project.uis.push_back(std::move(ui));
    }

    const json& shared = objectAt(root, "shared");
    project.shared.assetRoot = getStringOr(shared, "assetRoot", project.shared.assetRoot);
    project.shared.fontDirs = getStringArray(shared, "fontDirs");
    project.shared.styles = getStringArray(shared, "styles");
    project.shared.templateDirs = getStringArray(shared, "templateDirs");
    project.shared.symbols = getStringOr(shared, "symbols", project.shared.symbols);
    project.shared.tokens = getStringOr(shared, "tokens", project.shared.tokens);
    project.shared.themes = getStringOr(shared, "themes", project.shared.themes);
    project.shared.strings = getStringOr(shared, "strings", project.shared.strings);
    project.shared.recipes = getStringOr(shared, "recipes", project.shared.recipes);
    project.shared.design = getStringOr(shared, "design", project.shared.design);
    project.extraSharedJson = extraKeys(shared, {"assetRoot", "fontDirs", "styles", "templateDirs", "symbols",
                                                 "tokens", "themes", "strings", "recipes", "design"});

    project.previewDpr = getFloat(objectAt(root, "preview"), "dpr", 1.0f);
    project.previewScene = getString(objectAt(root, "preview"), "scene");  // T-344
    project.extraPreviewJson = extraKeys(objectAt(root, "preview"), {"dpr", "scene"});
    const std::string target = getString(root, "target");
    if (!target.empty()) {
        project.target = target;
    }

    const json& publish = objectAt(root, "publish");
    project.publish.outDir = getString(publish, "outDir");
    project.publish.languages = getStringArray(publish, "languages");
    project.publish.embedImages = getBool(publish, "embedImages", false);
    project.publish.embedFonts = getBool(publish, "embedFonts", false);
    project.publish.thumbnail =
        detail::clampThumbnail(getVec2(publish, "thumbnails", Vec2{320, 180}), Vec2{320, 180});  // T-258a
    const json& atlas = objectAt(publish, "atlas");
    project.publish.atlas.enabled = getBool(atlas, "enabled", false);
    project.publish.atlas.maxSize = getIntClamped(atlas, "maxSize", 4096, kMinAtlasSize, kMaxAtlasSize);
    project.publish.atlas.padding = getIntClamped(atlas, "padding", 2, 0, kMaxAtlasPadding);
    project.publish.atlas.mipmaps = getBool(atlas, "mipmaps", false);
    project.publish.atlas.scope = getString(atlas, "scope");  // T-199
    if (project.publish.atlas.scope != "bundle") {
        project.publish.atlas.scope = "ui";
    }
    // T-527 (22 §22.10.3): an unknown value is a load error, not silently "system".
    if (const auto fonts = publish.find("fonts"); fonts != publish.end()) {
        if (!fonts->is_string() || !parseFontsMode(fonts->get<std::string>(), project.publish.fonts)) {
            return failUiProject(Status::kInvalidArgument, error,
                                 "'" + path + "': publish.fonts must be \"system\" or \"bundled\"");
        }
    }
    // T-212: publish.targets[] -- entries without a kind or path are dropped. T-304: the retired
    // second kind older editors wrote reads as "dir" (same path); when an entry then matches an
    // earlier one (same kind and path), the one with the later lastPublished is kept, at the later
    // position.
    static constexpr const char* kRetiredKind = "ce-dir";
    if (const auto targets = publish.find("targets"); targets != publish.end() && targets->is_array()) {
        std::vector<PublishTarget>& loaded = project.publish.targets;
        for (const json& entry : *targets) {
            if (!entry.is_object()) {
                continue;
            }
            PublishTarget published;
            published.kind = getString(entry, "kind");
            published.path = getString(entry, "path");
            published.lastPublished = getString(entry, "lastPublished");
            if (published.kind.empty() || published.path.empty()) {
                continue;
            }
            if (published.kind == kRetiredKind) {
                published.kind = kPublishKindDir;
            }
            for (auto it = loaded.begin(); it != loaded.end(); ++it) {
                if (it->kind == published.kind && it->path == published.path) {
                    // ISO 8601 UTC timestamps order as strings; empty (never) sorts first.
                    if (it->lastPublished > published.lastPublished) {
                        published.lastPublished = it->lastPublished;
                    }
                    loaded.erase(it);
                    break;
                }
            }
            loaded.push_back(std::move(published));
        }
    }

    // T-312a: assets.sourceFolders[] -- entries without a path are dropped.
    const json& assets = objectAt(root, "assets");
    if (const auto folders = assets.find("sourceFolders"); folders != assets.end() && folders->is_array()) {
        for (const json& entry : *folders) {
            if (!entry.is_object()) {
                continue;
            }
            AssetSourceFolder folder;
            folder.path = getString(entry, "path");
            folder.collection = getString(entry, "collection");
            if (const auto settings = entry.find("import");
                settings != entry.end() && settings->is_object()) {
                folder.importJson = settings->dump();
            }
            if (!folder.path.empty()) {
                project.assets.sourceFolders.push_back(std::move(folder));
            }
        }
    }
    project.assets.extraJson = extraKeys(assets, {"sourceFolders"});
    project.extraPublishJson = extraKeys(publish, {"outDir", "languages", "embedImages", "embedFonts",
                                                   "thumbnails", "atlas", "fonts", "targets"});

    project.extraJson = extraKeys(root, {"version", "name", "id", "game", "spec", "uis", "shared", "preview",
                                         "target", "publish", "assets"});
    return Result<UiProject>::ok(std::move(project));
}

UiProject UiProject::fromBundleProject(const ProjectFile& source) {
    UiProject project;
    project.version = 1;
    project.name = source.name;
    project.id = source.id;
    project.uis = source.uis;
    project.shared.assetRoot = source.shared.assetRoot;
    project.shared.fontDirs = source.shared.fontDirs;
    project.shared.styles = source.shared.styles;
    project.shared.templateDirs = source.shared.templateDirs;
    // A bundle.xtproj project keeps its themes and string tables at the root (T-170/T-179) and
    // has no implied token stylesheet; keeping that makes an upgraded project pack identically.
    project.shared.themes = "themes";
    project.shared.strings = "strings";
    project.shared.tokens.clear();
    project.previewDpr = source.previewDpr;
    project.target = source.target;
    project.publish.embedImages = source.publish.embedImages;
    project.publish.embedFonts = source.publish.embedFonts;
    project.publish.thumbnail = source.publish.thumbnail;
    project.publish.fonts = source.publish.fonts;  // T-527
    project.rootDir = source.rootDir;
    return project;
}

UiProject UiProject::implied(std::string_view xtPath) {
    UiProject project = fromBundleProject(ProjectFile::implied(xtPath));
    project.shared.assetRoot.clear();
    project.legacy = true;
    return project;
}

UiProject UiProject::unnamedFor(std::string_view xtPath) {
    const ProjectFile implied = ProjectFile::implied(xtPath);
    UiProject project;
    project.id = implied.id;
    project.uis = implied.uis;
    project.rootDir = implied.rootDir;
    project.unnamed = true;
    return project;
}

std::string UiProject::fileName() const { return uiProjectFileName(name, id); }

std::string UiProject::path() const { return isUnnamed() ? std::string() : joinPath(rootDir, fileName()); }

const PublishTarget* UiProject::lastTarget() const {
    return publish.targets.empty() ? nullptr : &publish.targets.back();
}

Status UiProject::save(std::string* error) const {
    setErrorUiProject(error, std::string());
    if (rootDir.empty()) {
        setErrorUiProject(error, "cannot save a UI Project without a rootDir");
        return Status::kInvalidArgument;
    }
    if (unnamed) {
        setErrorUiProject(error, "an unnamed UI Project is saved with Save As UI Project");
        return Status::kInvalidArgument;
    }

    ordered_json root;
    root["version"] = kUiProjectFormatVersion;  // T-406: always the latest.
    root["name"] = name;
    root["id"] = id;
    root["game"] = game;
    ordered_json specObj = writeSpec(spec);
    appendExtra(specObj, extraSpecJson);
    root["spec"] = std::move(specObj);

    ordered_json uiArray = ordered_json::array();
    for (const ProjectUi& ui : uis) {
        uiArray.push_back(writeUi(ui));
    }
    root["uis"] = std::move(uiArray);

    ordered_json sharedObj;
    sharedObj["assetRoot"] = shared.assetRoot;
    sharedObj["fontDirs"] = shared.fontDirs;
    sharedObj["styles"] = shared.styles;
    sharedObj["templateDirs"] = shared.templateDirs;
    sharedObj["symbols"] = shared.symbols;
    sharedObj["tokens"] = shared.tokens;
    sharedObj["themes"] = shared.themes;
    sharedObj["strings"] = shared.strings;
    sharedObj["recipes"] = shared.recipes;
    sharedObj["design"] = shared.design;
    appendExtra(sharedObj, extraSharedJson);
    root["shared"] = std::move(sharedObj);

    ordered_json previewObj;
    previewObj["dpr"] = detail::shortNumber(previewDpr);
    if (!previewScene.empty()) {
        previewObj["scene"] = previewScene;  // T-344
    }
    appendExtra(previewObj, extraPreviewJson);
    root["preview"] = std::move(previewObj);
    root["target"] = target;

    ordered_json publishObj;
    publishObj["outDir"] = publish.outDir;
    publishObj["languages"] = publish.languages;
    publishObj["embedImages"] = publish.embedImages;
    publishObj["embedFonts"] = publish.embedFonts;
    publishObj["thumbnails"] = writeVec2UiProject(publish.thumbnail);
    ordered_json atlasObj;
    atlasObj["enabled"] = publish.atlas.enabled;
    atlasObj["maxSize"] = publish.atlas.maxSize;
    atlasObj["padding"] = publish.atlas.padding;
    atlasObj["mipmaps"] = publish.atlas.mipmaps;
    atlasObj["scope"] = publish.atlas.scope;
    publishObj["atlas"] = std::move(atlasObj);
    if (publish.fonts != FontsMode::kSystem) {
        publishObj["fonts"] = fontsModeName(publish.fonts);  // T-527: absent = "system".
    }
    if (!publish.targets.empty()) {
        ordered_json targetArray = ordered_json::array();
        for (const PublishTarget& published : publish.targets) {
            ordered_json entry;
            entry["kind"] = published.kind;
            entry["path"] = published.path;
            entry["lastPublished"] = published.lastPublished;
            targetArray.push_back(std::move(entry));
        }
        publishObj["targets"] = std::move(targetArray);
    }
    appendExtra(publishObj, extraPublishJson);
    root["publish"] = std::move(publishObj);
    // T-312a: `assets` only when it has content.
    ordered_json assetsObj = ordered_json::object();
    if (!assets.sourceFolders.empty()) {
        ordered_json folderArray = ordered_json::array();
        for (const AssetSourceFolder& folder : assets.sourceFolders) {
            ordered_json entry;
            entry["path"] = folder.path;
            entry["collection"] = folder.collection;
            if (!folder.importJson.empty()) {
                ordered_json settings = ordered_json::parse(folder.importJson, nullptr, false);
                if (settings.is_object()) {
                    entry["import"] = std::move(settings);
                }
            }
            folderArray.push_back(std::move(entry));
        }
        assetsObj["sourceFolders"] = std::move(folderArray);
    }
    appendExtra(assetsObj, assets.extraJson);
    if (!assetsObj.empty()) {
        root["assets"] = std::move(assetsObj);
    }
    appendExtra(root, extraJson);

    std::string text = root.dump(2, ' ', false, json::error_handler_t::replace);
    text += '\n';
    // T-214: one `.xtui` per directory. A case-only rename ("Foo" -> "foo") renames the existing
    // file first (case-insensitive file systems keep the old spelling otherwise); after the write
    // every other `.xtui` goes (the one of the project's old name). Legacy files stay.
    const std::string targetName = fileName();
    const std::string path = joinPath(rootDir, targetName);
    const std::vector<fs::path> existing = xtuiFilesIn(pathFromUtf8(rootDir));
    std::error_code ec;
    for (const fs::path& file : existing) {
        const std::string current = pathToUtf8(file.filename());
        if (current != targetName && lowerAsciiUiProject(current) == lowerAsciiUiProject(targetName)) {
            // In two steps: a rename that only changes the case is a no-op on some file systems
            // (exFAT keeps the old spelling).
            const fs::path via = pathFromUtf8(path + ".renaming");
            fs::rename(file, via, ec);
            if (ec) {
                continue;  // Left as it was; the write below goes into the old spelling.
            }
            fs::rename(via, pathFromUtf8(path), ec);
            if (ec) {
                // Put the file back under its old name rather than leave `<name>.renaming` beside
                // the file the write below would create.
                std::error_code back;
                fs::rename(via, file, back);
                if (back) {
                    setErrorUiProject(error, "cannot rename '" + pathToUtf8(via) + "' to '" + path + "'");
                    return Status::kIoError;
                }
            }
        }
    }
    if (!detail::writeFile(path, text)) {
        setErrorUiProject(error, "cannot write '" + path + "'");
        return Status::kIoError;
    }
    for (const fs::path& file : existing) {
        if (lowerAsciiUiProject(pathToUtf8(file.filename())) != lowerAsciiUiProject(targetName)) {
            fs::remove(file, ec);
        }
    }
    return Status::kOk;
}

ProjectFile UiProject::toBundleProject() const {
    ProjectFile project;
    project.version = 2;
    project.name = name;
    project.id = id;
    project.uis = uis;
    project.shared.assetRoot = shared.assetRoot;
    project.shared.fontDirs = shared.fontDirs;
    project.shared.styles = shared.styles;
    project.shared.templateDirs = shared.templateDirs;
    if (!shared.tokens.empty()) {
        std::error_code ec;
        const bool exists = fs::is_regular_file(pathFromUtf8(rootDir) / pathFromUtf8(shared.tokens), ec);
        bool listed = false;
        for (const std::string& style : shared.styles) {
            listed = listed || style == shared.tokens;
        }
        if (exists && !listed) {
            project.shared.styles.insert(project.shared.styles.begin(), shared.tokens);
        }
    }
    project.previewDpr = previewDpr;
    project.target = target;
    project.publish.embedImages = publish.embedImages;
    project.publish.embedFonts = publish.embedFonts;
    project.publish.thumbnail = publish.thumbnail;
    project.publish.fonts = publish.fonts;  // T-527
    project.rootDir = rootDir;
    return project;
}

const ProjectUi* UiProject::ui(std::string_view uiId) const {
    for (const ProjectUi& entry : uis) {
        if (entry.id == uiId) {
            return &entry;
        }
    }
    return nullptr;
}

Status upgradeToUiProject(std::string_view dir, UpgradeReport* report, std::string* error) {
    setErrorUiProject(error, std::string());
    if (hasUiProjectFile(dir)) {
        setErrorUiProject(error, "'" + std::string(dir) + "' already has a .xtui or ui-project.json");
        return Status::kInvalidArgument;
    }
    std::string loadError;
    Result<ProjectFile> legacy = ProjectFile::load(dir, &loadError);
    if (!legacy.ok()) {
        setErrorUiProject(error, loadError);
        return legacy.status();
    }

    UpgradeReport local;
    const fs::path root = pathFromUtf8(dir);
    std::error_code ec;
    for (const char* folder : {"conventions", "contracts", "manifest"}) {
        if (fs::is_directory(root / folder, ec)) {
            continue;
        }
        if (!fs::create_directories(root / folder, ec) || ec) {
            setErrorUiProject(error, "cannot create '" + genericPathToUtf8(root / folder) + "'");
            return Status::kIoError;
        }
        local.created.push_back(std::string(folder) + "/");
    }
    const std::pair<const char*, std::string_view> files[] = {
        {"AGENTS.md", kAgentsMd},
        {"contracts/motion-spec.yaml", kMotionSpecYaml},
    };
    for (const auto& [relative, content] : files) {
        const fs::path path = root / relative;
        if (fs::exists(path, ec)) {
            continue;
        }
        if (!detail::writeFile(pathToUtf8(path), std::string(content))) {
            setErrorUiProject(error, "cannot write '" + genericPathToUtf8(path) + "'");
            return Status::kIoError;
        }
        local.created.emplace_back(relative);
    }

    UiProject project = UiProject::fromBundleProject(legacy.value());
    const Status status = project.save(error);
    if (status != Status::kOk) {
        return status;
    }
    local.created.push_back(project.fileName());
    // T-305 (Docs/design/35-design-system.md §35.11): design/ replaces the top-level references/.
    design::DesignUpgradeReport designReport;
    if (const Status designStatus = design::upgradeDesign(project, &designReport, error);
        designStatus != Status::kOk) {
        return designStatus;
    }
    if (designReport.created) {
        local.created.push_back(project.shared.design + "/");
    }
    if (report != nullptr) {
        *report = std::move(local);
    }
    return Status::kOk;
}

// --- T-212 (Docs/design/22-bundle.md §22.11) ---

namespace {

/// True when `dir` does not exist or is an empty directory.
bool isFreshDir(const fs::path& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        return true;
    }
    return fs::is_directory(dir, ec) && fs::is_empty(dir, ec);
}

/// `path` lexically normal with '/' separators and no trailing '/'.
std::string normalPath(const fs::path& path) {
    std::string out = genericPathToUtf8(path.lexically_normal());
    while (out.size() > 1 && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

/// `path` relative to `base` when it lies inside it (never "..."-prefixed), else "".
std::string relativeInside(const fs::path& path, const fs::path& base) {
    const fs::path relative = path.lexically_normal().lexically_relative(base.lexically_normal());
    const std::string text = genericPathToUtf8(relative);
    if (text.empty() || text == "." || text.rfind("..", 0) == 0) {
        return std::string();
    }
    return text;
}

/// Copies `from` into `to` recursively; `skip` says which entries (relative paths) to leave out.
bool copyTree(const fs::path& from, const fs::path& to,
              const std::function<bool(const fs::path& entry, const std::string& relative)>& skip,
              std::string* error) {
    std::error_code ec;
    fs::create_directories(to, ec);
    if (ec) {
        setErrorUiProject(error, "cannot create '" + genericPathToUtf8(to) + "'");
        return false;
    }
    for (fs::recursive_directory_iterator it(from, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            setErrorUiProject(error, "cannot read '" + genericPathToUtf8(from) + "'");
            return false;
        }
        const std::string relative = relativeInside(it->path(), from);
        if (relative.empty()) {
            continue;
        }
        if (skip(it->path(), relative)) {
            if (it->is_directory(ec)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        const fs::path target = to / pathFromUtf8(relative);
        if (it->is_directory(ec)) {
            fs::create_directories(target, ec);
        } else {
            fs::create_directories(target.parent_path(), ec);
            fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing, ec);
        }
        if (ec) {
            setErrorUiProject(error, "cannot copy to '" + genericPathToUtf8(target) + "'");
            return false;
        }
    }
    return true;
}

/// Moves one file, falling back to copy + remove across volumes.
bool moveFile(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    fs::rename(from, to, ec);
    if (!ec) {
        return true;
    }
    ec.clear();
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        return false;
    }
    fs::remove(from, ec);
    return true;
}

}  // namespace

std::string slugifyId(std::string_view name) {
    std::string out;
    for (char c : name) {
        if (c >= 'A' && c <= 'Z') {
            out += static_cast<char>(c - 'A' + 'a');
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out += c;
        } else if (!out.empty() && out.back() != '-') {
            out += '-';
        }
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    return out;
}

std::string_view blankUiDocument() {
    return "<xt>\n"
           "  <head>\n"
           "  </head>\n"
           "  <body>\n"
           "  </body>\n"
           "</xt>\n";
}

Status pullStandardSymbols(const UiProject& project, std::string_view standardSymbolDir,
                           std::vector<std::string>* copied) {
    if (standardSymbolDir.empty() || project.shared.symbols.empty()) {
        return Status::kOk;
    }
    std::error_code ec;
    const fs::path root = fs::absolute(pathFromUtf8(project.rootDir), ec).lexically_normal();
    const fs::path symbolsDir = (root / pathFromUtf8(project.shared.symbols)).lexically_normal();
    const fs::path library = pathFromUtf8(standardSymbolDir);
    Status status = Status::kOk;
    for (const ProjectUi& ui : project.uis) {
        const fs::path docPath = (root / pathFromUtf8(ui.doc)).lexically_normal();
        std::string text;
        if (!detail::readFile(pathToUtf8(docPath), text)) {
            continue;  // Opening the UI reports the unreadable document.
        }
        compile::DiagnosticSink sink;
        compile::DomTree dom;
        if (!compile::parseXt(text, pathToUtf8(docPath), sink, dom)) {
            continue;
        }
        for (const std::string& href : dom.symbolHrefs) {
            if (href.rfind("xt-standard:", 0) == 0) {
                continue;
            }
            const fs::path target =
                fs::absolute(docPath.parent_path() / pathFromUtf8(href), ec).lexically_normal();
            if (fs::exists(target, ec) || target.parent_path() != symbolsDir) {
                continue;
            }
            const fs::path standard = detail::findStandardSymbol(library, target.filename());  // T-382
            std::string bytes;
            if (standard.empty() || !detail::readFile(pathToUtf8(standard), bytes)) {
                continue;  // Not a standard symbol: the compile reports the missing file.
            }
            fs::create_directories(symbolsDir, ec);
            if (!detail::writeFile(pathToUtf8(target), bytes)) {
                status = Status::kIoError;
                continue;
            }
            if (copied != nullptr) {
                copied->push_back(genericPathToUtf8(target.lexically_relative(root)));
            }
        }
    }
    return status;
}

Status createUiProject(std::string_view dir, std::string_view name, std::string_view id,
                       std::string_view templateDir, UiProject* out, std::string* error) {
    setErrorUiProject(error, std::string());
    if (!isValidId(id)) {
        setErrorUiProject(error, "project id '" + std::string(id) + "' is not [a-z0-9-]+");
        return Status::kInvalidArgument;
    }
    const fs::path root = pathFromUtf8(dir);
    if (dir.empty() || !isFreshDir(root)) {
        setErrorUiProject(error, "'" + genericPathToUtf8(root) + "' is not an empty directory");
        return Status::kInvalidArgument;
    }
    std::error_code ec;
    UiProject project;
    if (!templateDir.empty()) {
        const fs::path source = pathFromUtf8(templateDir);
        if (!hasUiProjectFile(templateDir)) {
            setErrorUiProject(error,
                              "template '" + genericPathToUtf8(source) + "' has no .xtui or ui-project.json");
            return Status::kInvalidArgument;
        }
        Result<UiProject> loaded = UiProject::load(templateDir, error);
        if (!loaded.ok()) {
            return loaded.status();
        }
        // T-214: the template's root files are not copied; save() below writes `<name>.xtui`.
        const auto skip = [](const fs::path&, const std::string& relative) {
            return relative == "out" || relative == "publish" ||
                   (relative.find('/') == std::string::npos && isUiProjectFilePath(relative));
        };
        if (!copyTree(source, root, skip, error)) {
            return Status::kIoError;
        }
        project = std::move(loaded.value());
        project.rootDir = normalPath(root);
        project.legacy = false;
    } else {
        fs::create_directories(root / "shared", ec);
        fs::create_directories(root / "main", ec);
        if (ec || !detail::writeFile(pathToUtf8(root / "main" / "main.xt"), std::string(blankUiDocument()))) {
            setErrorUiProject(error, "cannot write '" + genericPathToUtf8(root / "main" / "main.xt") + "'");
            return Status::kIoError;
        }
        project.rootDir = normalPath(root);
        ProjectUi ui;
        ui.id = "main";
        ui.doc = "main/main.xt";
        project.uis.push_back(std::move(ui));
    }
    project.name = std::string(name);
    project.id = std::string(id);
    project.legacy = false;
    project.legacyKind = LegacyKind::kNone;
    project.unnamed = false;
    project.spec.behavior = kLatestBehavior;  // T-407 (43 §43.3): a new project starts at the newest.
    if (const Status status = project.save(error); status != Status::kOk) {
        return status;
    }
    // T-305 (Docs/design/35-design-system.md §35.11): every new project has a design/ directory.
    if (const Status status = design::upgradeDesign(project, nullptr, error); status != Status::kOk) {
        return status;
    }
    if (out != nullptr) {
        *out = std::move(project);
    }
    return Status::kOk;
}

Status copyUiProject(const UiProject& project, std::string_view dir, UiProject* out, std::string* error) {
    setErrorUiProject(error, std::string());
    if (project.isUnnamed()) {
        setErrorUiProject(error, "an unnamed UI Project has no directory to copy");
        return Status::kInvalidArgument;
    }
    const fs::path from = pathFromUtf8(project.rootDir);
    const fs::path to = pathFromUtf8(dir);
    std::error_code ec;
    const fs::path fromAbs = fs::absolute(from, ec);
    const fs::path toAbs = fs::absolute(to, ec);
    if (dir.empty() || normalPath(fromAbs) == normalPath(toAbs) || !isFreshDir(to)) {
        setErrorUiProject(error, "'" + genericPathToUtf8(to) + "' is not an empty directory");
        return Status::kInvalidArgument;
    }
    const std::string nested = relativeInside(toAbs, fromAbs);
    const auto skip = [&nested](const fs::path&, const std::string& relative) {
        return !nested.empty() && (relative == nested || relative.rfind(nested + "/", 0) == 0);
    };
    if (!copyTree(fromAbs, toAbs, skip, error)) {
        return Status::kIoError;
    }
    if (out != nullptr) {
        *out = project;
        out->rootDir = normalPath(to);
    }
    return Status::kOk;
}

Status adoptAsUiProject(const UiProject& unnamed, const std::vector<std::string>& files, std::string_view dir,
                        UiProject* out, std::string* error) {
    setErrorUiProject(error, std::string());
    if (!unnamed.unnamed || unnamed.uis.empty() || files.empty()) {
        setErrorUiProject(error, "only an unnamed UI Project with a document can be adopted");
        return Status::kInvalidArgument;
    }
    const fs::path root = pathFromUtf8(dir);
    if (dir.empty() || !isFreshDir(root)) {
        setErrorUiProject(error, "'" + genericPathToUtf8(root) + "' is not an empty directory");
        return Status::kInvalidArgument;
    }
    const ProjectUi& source = unnamed.uis.front();
    const fs::path uiDir = root / pathFromUtf8(source.id);
    std::error_code ec;
    fs::create_directories(root / "shared", ec);
    fs::create_directories(uiDir, ec);
    if (ec) {
        setErrorUiProject(error, "cannot create '" + genericPathToUtf8(uiDir) + "'");
        return Status::kIoError;
    }
    const fs::path base = pathFromUtf8(unnamed.rootDir);
    std::string docName;
    for (size_t i = 0; i < files.size(); ++i) {
        const fs::path file = pathFromUtf8(files[i]);
        const std::string relative = relativeInside(file, base);
        if (i == 0) {
            docName = relative.empty() ? genericPathToUtf8(file.filename()) : relative;
        }
        if (relative.empty() || !fs::is_regular_file(file, ec)) {
            continue;
        }
        if (!moveFile(file, uiDir / pathFromUtf8(relative))) {
            setErrorUiProject(error, "cannot move '" + genericPathToUtf8(file) + "'");
            return Status::kIoError;
        }
    }
    UiProject project;
    project.rootDir = normalPath(root);
    project.id = unnamed.id;
    project.spec.behavior = kLatestBehavior;  // T-407: an adopted document starts a new project.
    project.name = pathToUtf8(pathFromUtf8(project.rootDir).filename());
    ProjectUi ui = source;
    ui.doc = source.id + "/" + docName;
    project.uis.push_back(std::move(ui));
    if (const Status status = project.save(error); status != Status::kOk) {
        return status;
    }
    if (out != nullptr) {
        *out = std::move(project);
    }
    return Status::kOk;
}

void recordPublish(UiProject& project, std::string_view kind, std::string_view path,
                   std::string_view timestamp) {
    std::error_code ec;
    const fs::path absolute = fs::absolute(pathFromUtf8(path), ec);
    std::string stored =
        project.rootDir.empty() ? std::string() : relativeInside(absolute, pathFromUtf8(project.rootDir));
    if (stored.empty()) {
        stored = normalPath(absolute);
    }
    std::vector<PublishTarget>& targets = project.publish.targets;
    PublishTarget entry;
    for (auto it = targets.begin(); it != targets.end(); ++it) {
        if (it->kind == kind && it->path == stored) {
            entry = *it;
            targets.erase(it);
            break;
        }
    }
    entry.kind = std::string(kind);
    entry.path = stored;
    entry.lastPublished = std::string(timestamp);
    targets.push_back(std::move(entry));
    if (kind == kPublishKindDir) {
        project.publish.outDir = stored;
    }
}

std::string resolveProjectPath(const UiProject& project, std::string_view path) {
    const fs::path stored = pathFromUtf8(path);
    if (stored.is_absolute() || project.rootDir.empty()) {
        return normalPath(stored);
    }
    return normalPath(pathFromUtf8(project.rootDir) / stored);
}

}  // namespace xt::bundle
