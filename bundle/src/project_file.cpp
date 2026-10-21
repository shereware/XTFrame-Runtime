#include "xt/bundle/project_file.h"

#include <json.hpp>
#include <set>
#include <string>

#include "bundle_json.h"

/// ProjectFile: reading and writing `bundle.xtproj` v2. See docs/design/22-bundle.md §22.2 and
/// docs/tasks/T-90.md.
///
/// Parsing is non-throwing (json::parse's `allow_exceptions = false` plus bundle_json.h's
/// type-checked accessors), so no failure escapes as an exception. save() emits nlohmann's
/// ordered_json in §22.2's key order, which makes the output byte-stable across saves.

namespace xt::bundle {

using detail::getBool;
using detail::getFloat;
using detail::getString;
using detail::getStringArray;
using detail::getVec2;
using detail::isValidId;
using detail::joinPath;
using nlohmann::json;
using nlohmann::ordered_json;

namespace {

constexpr const char* kProjectFileName = "bundle.xtproj";

/// Sets `*error` when the caller asked for a message.
void setError(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

/// Fails `load` with `status`, recording `message`.
Result<ProjectFile> fail(Status status, std::string* error, std::string message) {
    setError(error, std::move(message));
    return Result<ProjectFile>::error(status);
}

/// Emits a number as an integer when it is integral, so sizes read back as "[1280, 720]" rather
/// than "[1280.0, 720.0]". Keeps the output stable and human-editable.
void writeNumber(ordered_json& slot, float value) {
    const auto rounded = static_cast<int64_t>(value);
    if (static_cast<float>(rounded) == value) {
        slot = rounded;
    } else {
        slot = value;
    }
}

/// Emits `[x, y]`.
ordered_json writeVec2(Vec2 v) {
    ordered_json out = ordered_json::array();
    out.push_back(ordered_json());
    out.push_back(ordered_json());
    writeNumber(out[0], v.x);
    writeNumber(out[1], v.y);
    return out;
}

/// Strips a trailing '/' or '\\' so rootDir never ends in a separator (joinPath adds one back).
std::string trimTrailingSlash(std::string_view dir) {
    std::string out(dir);
    while (out.size() > 1 && (out.back() == '/' || out.back() == '\\')) {
        out.pop_back();
    }
    if (out == "/" || out == "\\") {
        return out;
    }
    return out;
}

/// Lowercases ASCII letters and replaces every character outside `[a-z0-9-]` with '-', so any
/// file stem becomes a valid stable id (§22.2, implied bundles).
std::string sanitizeId(std::string_view stem) {
    std::string out;
    out.reserve(stem.size());
    for (char c : stem) {
        if (c >= 'A' && c <= 'Z') {
            out += static_cast<char>(c - 'A' + 'a');
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') {
            out += c;
        } else {
            out += '-';
        }
    }
    return out;
}

}  // namespace

Result<ProjectFile> ProjectFile::load(std::string_view dir, std::string* error) {
    setError(error, std::string());
    const std::string path = joinPath(dir, kProjectFileName);

    std::string text;
    if (!detail::readFile(path, text)) {
        return fail(Status::kNotFound, error, "cannot read '" + path + "'");
    }

    std::string depthError;  // T-262c
    const json root = detail::parseBounded(text, detail::kMaxJsonDepth, depthError);
    if (!depthError.empty()) {
        return fail(Status::kInvalidArgument, error, path + ": " + depthError);
    }
    if (root.is_discarded() || !root.is_object()) {
        return fail(Status::kIoError, error, "'" + path + "' is not a JSON object");
    }

    const auto versionIt = root.find("version");
    const int version =
        (versionIt != root.end() && versionIt->is_number_integer()) ? versionIt->get<int>() : 0;
    if (version != 2) {
        return fail(Status::kUnsupported, error,
                    "'" + path + "' has version " + std::to_string(version) + ", expected 2");
    }

    ProjectFile project;
    project.version = 2;
    project.rootDir = trimTrailingSlash(dir);
    project.name = getString(root, "name");
    project.id = getString(root, "id");
    if (project.id.empty()) {
        return fail(Status::kInvalidArgument, error, "'" + path + "' has no bundle 'id'");
    }
    if (!isValidId(project.id)) {
        return fail(Status::kInvalidArgument, error, "bundle id '" + project.id + "' is not [a-z0-9-]+");
    }

    const auto uisIt = root.find("uis");
    if (uisIt == root.end() || !uisIt->is_array()) {
        return fail(Status::kInvalidArgument, error, "'" + path + "' has no 'uis' array");
    }
    std::set<std::string> seen;
    for (const json& entry : *uisIt) {
        if (!entry.is_object()) {
            return fail(Status::kInvalidArgument, error, "'uis' contains a non-object entry");
        }
        ProjectUi ui;
        ui.id = getString(entry, "id");
        if (!isValidId(ui.id)) {
            return fail(Status::kInvalidArgument, error,
                        "UI id '" + ui.id + "' is missing or not [a-z0-9-]+");
        }
        if (!seen.insert(ui.id).second) {
            return fail(Status::kInvalidArgument, error, "duplicate UI id '" + ui.id + "'");
        }
        ui.doc = getString(entry, "doc");
        if (ui.doc.empty()) {
            return fail(Status::kInvalidArgument, error, "UI '" + ui.id + "' has no 'doc'");
        }
        ui.flow = getString(entry, "flow");
        ui.size = getVec2(entry, "size", Vec2{1280, 720});
        if (!detail::isValidUiSize(ui.size)) {  // T-258a
            return fail(Status::kInvalidArgument, error,
                        "UI '" + ui.id + "' has an invalid 'size' (expected two numbers in 1..16384)");
        }
        ui.tags = getStringArray(entry, "tags");
        ui.fit = getString(entry, "fit");
        if (ui.fit.empty()) {
            ui.fit = "native";
        }
        project.uis.push_back(std::move(ui));
    }

    const auto sharedIt = root.find("shared");
    if (sharedIt != root.end() && sharedIt->is_object()) {
        project.shared.assetRoot = getString(*sharedIt, "assetRoot");
        project.shared.fontDirs = getStringArray(*sharedIt, "fontDirs");
        project.shared.styles = getStringArray(*sharedIt, "styles");
        project.shared.templateDirs = getStringArray(*sharedIt, "templateDirs");
    } else {
        project.shared.assetRoot = "shared";
    }

    const auto previewIt = root.find("preview");
    if (previewIt != root.end() && previewIt->is_object()) {
        project.previewDpr = getFloat(*previewIt, "dpr", 1.0f);
    }

    const std::string target = getString(root, "target");
    if (!target.empty()) {
        project.target = target;
    }

    const auto publishIt = root.find("publish");
    if (publishIt != root.end() && publishIt->is_object()) {
        project.publish.embedImages = getBool(*publishIt, "embedImages", false);
        project.publish.embedFonts = getBool(*publishIt, "embedFonts", false);
        project.publish.thumbnail =
            detail::clampThumbnail(getVec2(*publishIt, "thumbnails", Vec2{320, 180}), Vec2{320, 180});
    }

    return Result<ProjectFile>::ok(std::move(project));
}

ProjectFile ProjectFile::implied(std::string_view xtPath) {
    std::string path(xtPath);
    for (char& c : path) {
        if (c == '\\') {
            c = '/';
        }
    }
    const size_t slash = path.find_last_of('/');
    const std::string fileName = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const std::string dir = (slash == std::string::npos) ? std::string() : path.substr(0, slash);

    const size_t dot = fileName.find_last_of('.');
    const std::string stem = (dot == std::string::npos) ? fileName : fileName.substr(0, dot);
    const std::string id = sanitizeId(stem);

    ProjectFile project;
    project.version = 2;
    project.name = id;
    project.id = id;
    project.rootDir = trimTrailingSlash(dir);
    project.shared.assetRoot.clear();

    ProjectUi ui;
    ui.id = id;
    ui.doc = fileName;
    project.uis.push_back(std::move(ui));
    return project;
}

Status ProjectFile::save(std::string* error) const {
    setError(error, std::string());
    if (rootDir.empty()) {
        setError(error, "cannot save a bundle.xtproj without a rootDir");
        return Status::kInvalidArgument;
    }

    ordered_json root;
    root["version"] = 2;
    root["name"] = name;
    root["id"] = id;

    ordered_json uiArray = ordered_json::array();
    for (const ProjectUi& ui : uis) {
        ordered_json entry;
        entry["id"] = ui.id;
        entry["doc"] = ui.doc;
        if (!ui.flow.empty()) {
            entry["flow"] = ui.flow;
        }
        entry["size"] = writeVec2(ui.size);
        if (!ui.tags.empty()) {
            entry["tags"] = ui.tags;
        }
        if (!ui.fit.empty() && ui.fit != "native") {
            entry["fit"] = ui.fit;
        }
        uiArray.push_back(std::move(entry));
    }
    root["uis"] = std::move(uiArray);

    ordered_json sharedObj;
    sharedObj["assetRoot"] = shared.assetRoot;
    sharedObj["fontDirs"] = shared.fontDirs;
    sharedObj["styles"] = shared.styles;
    sharedObj["templateDirs"] = shared.templateDirs;
    root["shared"] = std::move(sharedObj);

    ordered_json previewObj;
    writeNumber(previewObj["dpr"], previewDpr);
    root["preview"] = std::move(previewObj);

    root["target"] = target;

    ordered_json publishObj;
    publishObj["embedImages"] = publish.embedImages;
    publishObj["embedFonts"] = publish.embedFonts;
    publishObj["thumbnails"] = writeVec2(publish.thumbnail);
    root["publish"] = std::move(publishObj);

    // error_handler_t::replace keeps dump() from throwing on invalid UTF-8 in a caller's strings.
    std::string text = root.dump(2, ' ', false, json::error_handler_t::replace);
    text += '\n';

    const std::string path = joinPath(rootDir, kProjectFileName);
    if (!detail::writeFile(path, text)) {
        setError(error, "cannot write '" + path + "'");
        return Status::kIoError;
    }
    return Status::kOk;
}

const ProjectUi* ProjectFile::ui(std::string_view id_) const {
    for (const ProjectUi& entry : uis) {
        if (entry.id == id_) {
            return &entry;
        }
    }
    return nullptr;
}

}  // namespace xt::bundle
