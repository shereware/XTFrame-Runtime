#include "xt/bundle/project_edit.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <utility>

#include "bundle_json.h"
#include "bundle_paths.h"
#include "xt/bundle/asset_meta.h"
#include "xt/bundle/migrations.h"
#include "xt/compile/utf8_path.h"
#include "xt/flow/flow_graph.h"
#include "yaml_lite.h"

/// docs/tasks/T-371.md (docs/design/37-work-model.md §37.8.4): the UI Project writers shared by
/// the editor's panels and the Editor Service ops. See the header.

namespace xt::bundle {

namespace fs = std::filesystem;

namespace {

void setErrorProjectEdit(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

std::string slashed(std::string_view path) {
    std::string out(path);
    std::replace(out.begin(), out.end(), '\\', '/');
    return out;
}

/// `rel` under the project root, or an empty path when it leaves it (or is empty).
fs::path underRoot(const UiProject& project, std::string_view rel) {
    const std::string clean = slashed(rel);
    if (clean.empty() || !detail::isSafeBundlePath(clean)) {
        return fs::path();
    }
    return (compile::pathFromUtf8(project.rootDir) / compile::pathFromUtf8(clean)).lexically_normal();
}

/// Saves the project file; on failure `*error` explains.
Status saveProject(UiProject& project, std::string* error) {
    std::string why;
    const Status status = project.save(&why);
    if (status != Status::kOk) {
        setErrorProjectEdit(error, why.empty() ? "cannot write the project file" : why);
    }
    return status;
}

bool finite(float v) { return std::isfinite(v); }

/// `ui#<oldId>.…` with the id replaced; `endpoint` unchanged otherwise.
bool renameEndpoint(std::string& endpoint, std::string_view oldId, std::string_view newId) {
    const std::string prefix = "ui#" + std::string(oldId) + ".";
    if (endpoint.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    endpoint = "ui#" + std::string(newId) + "." + endpoint.substr(prefix.size());
    return true;
}

/// Where `key:` first appears in `text` at or after `from` as a key (not inside a longer word).
size_t findYamlKey(const std::string& text, const std::string& key, size_t from) {
    size_t pos = from;
    while ((pos = text.find(key, pos)) != std::string::npos) {
        const bool startOk = pos == 0 || !(std::isalnum(static_cast<unsigned char>(text[pos - 1])) ||
                                           text[pos - 1] == '_' || text[pos - 1] == '-');
        size_t end = pos + key.size();
        while (end < text.size() && text[end] == ' ') {
            ++end;
        }
        if (startOk && end < text.size() && text[end] == ':') {
            return pos;
        }
        pos += key.size();
    }
    return std::string::npos;
}

/// `value` with every mapping's keys in the order they appear in `text` (unknown ones last).
nlohmann::ordered_json inFileOrder(const nlohmann::json& value, const std::string& text, size_t from) {
    if (value.is_object()) {
        std::vector<std::pair<size_t, std::string>> keys;
        for (auto it = value.begin(); it != value.end(); ++it) {
            keys.emplace_back(findYamlKey(text, it.key(), from), it.key());
        }
        std::stable_sort(keys.begin(), keys.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        nlohmann::ordered_json out = nlohmann::ordered_json::object();
        for (const auto& [pos, key] : keys) {
            out[key] = inFileOrder(value.at(key), text, pos == std::string::npos ? from : pos);
        }
        return out;
    }
    if (value.is_array()) {
        nlohmann::ordered_json out = nlohmann::ordered_json::array();
        for (const nlohmann::json& item : value) {
            out.push_back(inFileOrder(item, text, from));
        }
        return out;
    }
    return nlohmann::ordered_json(value);
}

/// The scale value of `spec.responsive` reads as auto, none or clamp(<min>, <max>) with
/// 0 <= min <= max (the editor's responsive::parseScale() grammar).
bool validResponsiveScale(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    if (text.empty() || text == "auto" || text == "none") {
        return true;
    }
    constexpr std::string_view kClamp = "clamp(";
    if (text.size() <= kClamp.size() || text.compare(0, kClamp.size(), kClamp) != 0 || text.back() != ')') {
        return false;
    }
    const std::string inner = text.substr(kClamp.size(), text.size() - kClamp.size() - 1);
    const size_t comma = inner.find(',');
    if (comma == std::string::npos || inner.find(',', comma + 1) != std::string::npos) {
        return false;
    }
    const auto number = [](const std::string& s, double& out) {
        const char* begin = s.c_str();
        char* end = nullptr;
        out = std::strtod(begin, &end);
        while (end != nullptr && *end == ' ') {
            ++end;
        }
        return end != begin && end != nullptr && *end == '\0' && std::isfinite(out);
    };
    double lo = 0;
    double hi = 0;
    return number(inner.substr(0, comma), lo) && number(inner.substr(comma + 1), hi) && lo >= 0 && hi >= lo;
}

}  // namespace

const char* const kBlankUiDocument =
    "<xt>\n"
    "  <head>\n"
    "  </head>\n"
    "  <body>\n"
    "  </body>\n"
    "</xt>\n";

bool isValidUiId(std::string_view id) {
    return !id.empty() && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    });
}

std::string defaultUiDocPath(const UiProject& project, std::string_view uiId) {
    std::string prefix;
    bool first = true;
    for (const ProjectUi& ui : project.uis) {
        // `<prefix>/<id>/<file>.xt`: everything before the last two segments.
        std::string own;
        const std::string doc = slashed(ui.doc);
        const size_t last = doc.rfind('/');
        if (last != std::string::npos && last > 0) {
            const size_t second = doc.rfind('/', last - 1);
            if (second != std::string::npos) {
                own = doc.substr(0, second);
            }
        }
        if (first) {
            prefix = own;
            first = false;
        } else if (own != prefix) {
            prefix.clear();
            break;
        }
    }
    const std::string leaf = std::string(uiId) + "/" + std::string(uiId) + ".xt";
    return prefix.empty() ? leaf : prefix + "/" + leaf;
}

Status addProjectUi(UiProject& project, std::string_view id, std::string_view docRel, std::string_view text,
                    std::string* error) {
    if (project.isUnnamed()) {
        setErrorProjectEdit(error, "an unnamed UI Project has no UI list");
        return Status::kInvalidArgument;
    }
    if (!isValidUiId(id)) {
        setErrorProjectEdit(error, "a UI id takes lower-case letters, digits and '-' only");
        return Status::kInvalidArgument;
    }
    if (project.ui(id) != nullptr) {
        setErrorProjectEdit(error, "the project already has a UI '" + std::string(id) + "'");
        return Status::kInvalidArgument;
    }
    const fs::path target = underRoot(project, docRel);
    if (target.empty()) {
        setErrorProjectEdit(error, "the document path is empty or leaves the project");
        return Status::kInvalidArgument;
    }
    std::error_code ec;
    if (fs::exists(target, ec)) {
        setErrorProjectEdit(error, slashed(docRel) + " already exists");
        return Status::kInvalidArgument;
    }
    fs::create_directories(target.parent_path(), ec);
    if (!detail::writeFile(compile::pathToUtf8(target), std::string(text))) {
        setErrorProjectEdit(error, "cannot write " + slashed(docRel));
        return Status::kIoError;
    }
    ProjectUi ui;
    ui.id = std::string(id);
    ui.doc = slashed(docRel);
    project.uis.push_back(std::move(ui));
    const Status status = saveProject(project, error);
    if (status != Status::kOk) {
        project.uis.pop_back();
        fs::remove(target, ec);
    }
    return status;
}

Status renameProjectUi(UiProject& project, std::string_view oldId, std::string_view newId,
                       std::vector<std::string>* rewritten, std::vector<std::string>* notes,
                       std::string* error) {
    if (project.isUnnamed()) {
        setErrorProjectEdit(error, "an unnamed UI Project has no UI list");
        return Status::kInvalidArgument;
    }
    if (!isValidUiId(newId)) {
        setErrorProjectEdit(error, "a UI id takes lower-case letters, digits and '-' only");
        return Status::kInvalidArgument;
    }
    auto target = std::find_if(project.uis.begin(), project.uis.end(),
                               [&](const ProjectUi& ui) { return ui.id == oldId; });
    if (target == project.uis.end()) {
        setErrorProjectEdit(error, "no UI '" + std::string(oldId) + "' in the project");
        return Status::kInvalidArgument;
    }
    if (oldId == newId) {
        return Status::kOk;
    }
    if (project.ui(newId) != nullptr) {
        setErrorProjectEdit(error, "the project already has a UI '" + std::string(newId) + "'");
        return Status::kInvalidArgument;
    }
    target->id = std::string(newId);
    if (const Status status = saveProject(project, error); status != Status::kOk) {
        target->id = std::string(oldId);
        return status;
    }
    // What binds to the id: the global flow graph's `ui#<id>` endpoints and timeline `ui` params.
    const fs::path root = compile::pathFromUtf8(project.rootDir);
    std::error_code ec;
    for (const char* rel : {"bundle.xtflow", "shared/bundle.xtflow"}) {
        const fs::path file = root / rel;
        if (!fs::is_regular_file(file, ec)) {
            continue;
        }
        std::string text;
        flow::FlowGraph graph;
        std::vector<std::string> errors;
        if (!detail::readFile(compile::pathToUtf8(file), text) ||
            flow::FlowGraph::parse(text, graph, errors) != Status::kOk) {
            if (notes != nullptr) {
                notes->push_back(std::string(rel) + " does not parse; its references to '" +
                                 std::string(oldId) + "' were not rewritten");
            }
            break;
        }
        bool changed = false;
        for (flow::EdgeDesc& edge : graph.edges) {
            changed = renameEndpoint(edge.from, oldId, newId) || changed;
            changed = renameEndpoint(edge.to, oldId, newId) || changed;
        }
        for (flow::NodeDesc& node : graph.nodes) {
            auto ui = node.params.find("ui");
            if (node.type == "timeline" && ui != node.params.end() && ui->second.s == oldId) {
                ui->second.s = std::string(newId);
                changed = true;
            }
        }
        if (changed) {
            if (!detail::writeFile(compile::pathToUtf8(file), graph.toJson())) {
                if (notes != nullptr) {
                    notes->push_back(std::string("cannot write ") + rel);
                }
            } else if (rewritten != nullptr) {
                rewritten->push_back(rel);
            }
        }
        break;  // Only the file the runtime reads (the root one first).
    }
    return Status::kOk;
}

Status removeProjectUi(UiProject& project, std::string_view id, std::string* error) {
    if (project.isUnnamed()) {
        setErrorProjectEdit(error, "an unnamed UI Project has no UI list");
        return Status::kInvalidArgument;
    }
    auto it = std::find_if(project.uis.begin(), project.uis.end(),
                           [&](const ProjectUi& ui) { return ui.id == id; });
    if (it == project.uis.end()) {
        setErrorProjectEdit(error, "no UI '" + std::string(id) + "' in the project");
        return Status::kInvalidArgument;
    }
    if (project.uis.size() < 2) {
        setErrorProjectEdit(error, "a UI Project keeps at least one UI");
        return Status::kInvalidArgument;
    }
    const auto index = it - project.uis.begin();
    ProjectUi removed = std::move(*it);
    project.uis.erase(it);
    const Status status = saveProject(project, error);
    if (status != Status::kOk) {
        project.uis.insert(project.uis.begin() + index, std::move(removed));
    }
    return status;
}

ProjectSettings projectSettings(const UiProject& project) {
    return ProjectSettings{project.name, project.id, project.game, project.spec};
}

std::string validateProjectSettings(const ProjectSettings& settings) {
    if (!isValidUiId(settings.id)) {
        return "id: lower-case letters, digits and '-' only";
    }
    const ProjectSpec& s = settings.spec;
    if (s.behavior > kLatestBehavior) {  // T-407 (43 §43.3)
        return "spec.behavior: in [0, " + std::to_string(kLatestBehavior) + "]";
    }
    for (const SpecPlatform& platform : s.platforms) {
        for (const Vec2& r : platform.resolutions) {
            if (!finite(r.x) || !finite(r.y) || r.x < 1 || r.y < 1 || r.x > 16384 || r.y > 16384) {
                return "spec.platforms." + platform.name + ": resolutions are in [1, 16384]";
            }
        }
    }
    if (!finite(s.safeAreaDefault) || s.safeAreaDefault < 0 || s.safeAreaDefault > 0.5f) {
        return "spec.safeArea.default: a fraction in [0, 0.5]";
    }
    if (!finite(s.safeAreaConsole) || s.safeAreaConsole < 0 || s.safeAreaConsole > 0.5f) {
        return "spec.safeArea.console: a fraction in [0, 0.5]";
    }
    if (!finite(s.logicalHeight) || s.logicalHeight < 1 || s.logicalHeight > 16384) {
        return "spec.logicalHeight: in [1, 16384]";
    }
    const UiScaleLimits& limits = s.uiscaleLimits;
    if (limits.clamped) {
        if (!finite(limits.clampMin) || limits.clampMin < 0 || std::isnan(limits.clampMax) ||
            limits.clampMax < limits.clampMin) {
            return "spec.uiscaleClamp: min >= 0 and max >= min";
        }
    }
    if (!finite(limits.dpiFloor) || limits.dpiFloor < 0 || limits.dpiFloor > 16) {
        return "spec.uiscaleDpiFloor: in [0, 16]";
    }
    if (!validResponsiveScale(s.responsive.scale)) {
        return "spec.responsive.scale: auto, none or clamp(<min>, <max>)";
    }
    if (s.responsive.position != "follow" && s.responsive.position != "fixed") {
        return "spec.responsive.position: follow or fixed";
    }
    if (s.responsive.visibility != "auto" && s.responsive.visibility != "show" &&
        s.responsive.visibility != "hide") {
        return "spec.responsive.visibility: auto, show or hide";
    }
    for (const std::string& method : s.inputMethods) {
        if (method.empty()) {
            return "spec.input.methods: empty entry";
        }
    }
    if (s.players < 1 || s.players > 64) {
        return "spec.input.players: in [1, 64]";
    }
    for (const std::string& lang : s.languages) {
        if (lang.empty()) {
            return "spec.languages: empty language tag";
        }
    }
    if (!finite(s.budgetFrameMs) || s.budgetFrameMs < 0 || !finite(s.budgetFillRate) ||
        s.budgetFillRate < 0 || s.budgetEffectLayers < 0 || s.budgetParticles < 0) {
        return "spec.budget: finite numbers >= 0";
    }
    return std::string();
}

Status setProjectSettings(UiProject& project, const ProjectSettings& settings, std::string* error) {
    if (project.isUnnamed() || project.legacy) {
        setErrorProjectEdit(error, project.isUnnamed()
                                       ? "an unnamed UI Project has no settings"
                                       : "a project read from an old project file is read-only; "
                                         "save it as a UI Project first");
        return Status::kInvalidArgument;
    }
    if (const std::string problem = validateProjectSettings(settings); !problem.empty()) {
        setErrorProjectEdit(error, problem);
        return Status::kInvalidArgument;
    }
    ProjectSettings previous = projectSettings(project);
    project.name = settings.name;
    project.id = settings.id;
    project.game = settings.game;
    project.spec = settings.spec;
    const Status status = saveProject(project, error);
    if (status != Status::kOk) {
        project.name = std::move(previous.name);
        project.id = std::move(previous.id);
        project.game = std::move(previous.game);
        project.spec = std::move(previous.spec);
    }
    return status;
}

Status setAssetSourceFolders(UiProject& project, std::vector<AssetSourceFolder> folders, std::string* error) {
    if (project.isUnnamed() || project.legacy) {
        setErrorProjectEdit(error, "source folders need a UI Project (.xtui)");
        return Status::kInvalidArgument;
    }
    for (const AssetSourceFolder& folder : folders) {
        if (folder.path.empty() ||
            assets::importPathFor(project, "image", folder.collection, "x.png").empty()) {
            setErrorProjectEdit(error, "\"" + folder.collection + "\" is not a collection name");
            return Status::kInvalidArgument;
        }
    }
    std::vector<AssetSourceFolder> previous = std::move(project.assets.sourceFolders);
    project.assets.sourceFolders = std::move(folders);
    const Status status = saveProject(project, error);
    if (status != Status::kOk) {
        project.assets.sourceFolders = std::move(previous);
    }
    return status;
}

std::string agentsPath(const UiProject& project) {
    return project.isUnnamed() ? std::string() : detail::joinPath(slashed(project.rootDir), "AGENTS.md");
}

Status writeAgents(const UiProject& project, std::string_view text, std::string* error) {
    const std::string path = agentsPath(project);
    if (path.empty()) {
        setErrorProjectEdit(error, "an unnamed UI Project has no AGENTS.md");
        return Status::kInvalidArgument;
    }
    if (!detail::writeFile(path, std::string(text))) {
        setErrorProjectEdit(error, "cannot write AGENTS.md");
        return Status::kIoError;
    }
    return Status::kOk;
}

std::string conventionYamlText(std::string_view originalText, const nlohmann::json& doc) {
    const std::string text(originalText);
    // The file's leading comment block.
    std::string head;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t end = text.find('\n', pos);
        const std::string line =
            text.substr(pos, end == std::string::npos ? std::string::npos : end - pos + 1);
        const size_t first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos || line[first] != '#') {
            break;
        }
        head += line;
        if (end == std::string::npos) {
            head += '\n';
            break;
        }
        pos = end + 1;
    }
    try {
        nlohmann::ordered_json body = inFileOrder(doc, text, 0);
        if (body.is_object()) {
            // T-406 (43 §43.2): `version` first, always the latest; a newer one is never written over.
            int version = 1;
            if (!readFormatVersion(doc, version) || version > kDesignYamlFormatVersion) {
                return std::string();
            }
            nlohmann::ordered_json stamped = nlohmann::ordered_json::object();
            stamped["version"] = kDesignYamlFormatVersion;
            for (auto it = body.begin(); it != body.end(); ++it) {
                if (it.key() != "version") {
                    stamped[it.key()] = std::move(it.value());
                }
            }
            body = std::move(stamped);
        }
        return head + detail::emitYamlLite(body);
    } catch (const std::exception&) {
        return std::string();
    }
}

Status writeConventionYaml(const UiProject& project, std::string_view relFile, const nlohmann::json& doc,
                           std::string* error) {
    const fs::path file = project.isUnnamed() ? fs::path() : underRoot(project, relFile);
    if (file.empty()) {
        setErrorProjectEdit(error, "no UI Project, or the path leaves it");
        return Status::kInvalidArgument;
    }
    std::string original;
    detail::readFile(compile::pathToUtf8(file), original);
    const std::string out = conventionYamlText(original, doc);
    if (out.empty() && !doc.empty()) {
        setErrorProjectEdit(error, "cannot write the value as YAML");
        return Status::kInvalidArgument;
    }
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (!detail::writeFile(compile::pathToUtf8(file), out)) {
        setErrorProjectEdit(error, "cannot write " + slashed(relFile));
        return Status::kIoError;
    }
    return Status::kOk;
}

Status putMotionSpecEntry(const UiProject& project, std::string_view id, const nlohmann::json& value,
                          std::string* error) {
    if (project.isUnnamed()) {
        setErrorProjectEdit(error, "an unnamed UI Project has no motion spec");
        return Status::kInvalidArgument;
    }
    if (id.empty()) {
        setErrorProjectEdit(error, "empty motion spec key");
        return Status::kInvalidArgument;
    }
    const fs::path file = underRoot(project, kMotionSpecPath);
    std::string text;
    nlohmann::json doc = nlohmann::json::object();
    if (detail::readFile(compile::pathToUtf8(file), text)) {
        std::string why;
        bool parsed = false;
        try {
            parsed = detail::parseYamlLite(text, doc, &why);
        } catch (const std::exception& e) {
            why = e.what();
        }
        if (!parsed || !(doc.is_object() || doc.is_null())) {
            setErrorProjectEdit(error, std::string(kMotionSpecPath) + " does not read as a mapping: " + why);
            return Status::kInvalidArgument;
        }
        if (doc.is_null()) {
            doc = nlohmann::json::object();
        }
        // T-406 (43 §43.2): a motion spec from a newer XTFrame is not edited.
        std::string migrateError;
        if (const Status migrated = migrateToLatest(SourceFormat::DesignYaml, doc, nullptr, &migrateError);
            migrated != Status::kOk) {
            setErrorProjectEdit(error, std::string(kMotionSpecPath) + ": " + migrateError);
            return migrated;
        }
    }
    doc[std::string(id)] = value;
    return writeConventionYaml(project, kMotionSpecPath, doc, error);
}

bool isValidLayoutTemplateName(std::string_view name) {
    if (name.empty() || !std::isalnum(static_cast<unsigned char>(name.front()))) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '_' || c == '-';
    });
}

Status saveLayoutTemplate(const UiProject& project, std::string_view name, std::string_view text,
                          bool replace, std::string* relPath, std::string* error) {
    if (project.isUnnamed()) {
        setErrorProjectEdit(error, "an unnamed UI Project has no layout templates");
        return Status::kInvalidArgument;
    }
    if (!isValidLayoutTemplateName(name)) {
        setErrorProjectEdit(error, "a layout template name takes letters, digits, ' ', '_' and '-'");
        return Status::kInvalidArgument;
    }
    if (text.empty()) {
        setErrorProjectEdit(error, "empty layout template");
        return Status::kInvalidArgument;
    }
    const std::string rel = std::string(kLayoutTemplateDir) + "/" + std::string(name) + ".xt";
    const fs::path file = underRoot(project, rel);
    std::error_code ec;
    if (!replace && fs::exists(file, ec)) {
        setErrorProjectEdit(error, rel + " already exists");
        return Status::kInvalidArgument;
    }
    fs::create_directories(file.parent_path(), ec);
    if (!detail::writeFile(compile::pathToUtf8(file), std::string(text))) {
        setErrorProjectEdit(error, "cannot write " + rel);
        return Status::kIoError;
    }
    if (relPath != nullptr) {
        *relPath = rel;
    }
    return Status::kOk;
}

Status setAssetSlice(const UiProject& project, std::string_view relFile, const std::array<int, 4>& slice,
                     std::string* error) {
    if (project.isUnnamed()) {
        setErrorProjectEdit(error, "an unnamed UI Project has no asset metas");
        return Status::kInvalidArgument;
    }
    if (std::any_of(slice.begin(), slice.end(), [](int v) { return v < 0; })) {
        setErrorProjectEdit(error, "nine-slice insets are >= 0");
        return Status::kInvalidArgument;
    }
    const fs::path file = underRoot(project, relFile);
    if (file.empty()) {
        setErrorProjectEdit(error, "the asset path is empty or leaves the project");
        return Status::kInvalidArgument;
    }
    assets::AssetMeta meta;
    if (const Status ensured = assets::ensureMeta(project, slashed(relFile), meta, nullptr);
        ensured != Status::kOk) {
        setErrorProjectEdit(error, "no asset " + slashed(relFile));
        return ensured;
    }
    if (!meta.import.is_object()) {
        meta.import = nlohmann::json::object();
    }
    if (std::all_of(slice.begin(), slice.end(), [](int v) { return v == 0; })) {
        meta.import.erase("slice");
    } else {
        meta.import["slice"] = nlohmann::json::array({slice[0], slice[1], slice[2], slice[3]});
    }
    if (meta.import.empty()) {
        meta.import = nullptr;
    }
    return assets::writeMeta(compile::pathToUtf8(file), meta, error);
}

}  // namespace xt::bundle
