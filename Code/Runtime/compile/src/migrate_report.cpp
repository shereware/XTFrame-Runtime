#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "migrate_v2.h"
#include "xt/compile/migrate.h"
#include "xt/compile/utf8_path.h"

/// T-589 (Docs/design/54-source-format-v2.md §54.10 "每次遷移產生報告"): the migration report
/// (JSON and the short summary) and writeMigratedFiles(). See xt/compile/migrate.h.

namespace xt::compile {

namespace {

namespace fs = std::filesystem;

const char* checkName(MigrateCheck check) {
    switch (check) {
        case MigrateCheck::Equal:
            return "equal";
        case MigrateCheck::Differ:
            return "differ";
        default:
            return "not-checked";
    }
}

std::string relativeTo(const std::string& path, const std::string& root) {
    if (root.empty()) {
        return path;
    }
    const fs::path rel = pathFromUtf8(path).lexically_relative(pathFromUtf8(root));
    return rel.empty() ? path : genericPathToUtf8(rel);
}

void appendList(std::string& out, const char* key, const std::vector<std::string>& items) {
    out += ", ";
    migrate::appendJsonString(out, key);
    out += ": [";
    for (size_t i = 0; i < items.size(); ++i) {
        out += i == 0 ? "" : ", ";
        migrate::appendJsonString(out, items[i]);
    }
    out += "]";
}

bool writeOnce(const fs::path& target, const std::string& text) {
    std::ofstream file(target, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.close();
    return file.good();
}

}  // namespace

void migrate::appendJsonString(std::string& out, std::string_view s) {
    out.push_back('"');
    for (char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (u < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", u);
            out += buf;
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
}

bool MigrateReport::ok() const {
    for (const MigrateFileReport& f : files) {
        if (f.status == "error" || f.layerTwo == MigrateCheck::Differ) {
            return false;
        }
    }
    return true;
}

std::string MigrateReport::json() const {
    std::string out = "{\n  \"root\": ";
    migrate::appendJsonString(out, root);
    out += ",\n  \"files\": [";
    for (size_t i = 0; i < files.size(); ++i) {
        const MigrateFileReport& f = files[i];
        out += i == 0 ? "\n    {" : ",\n    {";
        out += "\"path\": ";
        migrate::appendJsonString(out, relativeTo(f.path, root));
        out += ", \"kind\": ";
        migrate::appendJsonString(out, f.kind);
        out += ", \"status\": ";
        migrate::appendJsonString(out, f.status);
        if (!f.reason.empty()) {
            out += ", \"reason\": ";
            migrate::appendJsonString(out, f.reason);
        }
        out += ", \"rulesSplit\": " + std::to_string(f.rulesSplit) +
               ", \"anchors\": " + std::to_string(f.anchors) +
               ", \"triggersMoved\": " + std::to_string(f.triggersMoved) +
               ", \"portsMoved\": " + std::to_string(f.portsMoved) +
               ", \"inlineMoved\": " + std::to_string(f.inlineMoved);
        appendList(out, "idsAssigned", f.idsAssigned);
        out += ", \"layerOne\": ";
        migrate::appendJsonString(out, checkName(f.layerOne));
        out += ", \"layerOneWithoutSourceMap\": ";
        migrate::appendJsonString(out, checkName(f.layerOneWithoutSourceMap));
        out += ", \"layerTwo\": ";
        migrate::appendJsonString(out, checkName(f.layerTwo));
        if (!f.checkDetail.empty()) {
            out += ", \"checkDetail\": ";
            migrate::appendJsonString(out, f.checkDetail);
        }
        appendList(out, "notes", f.notes);
        appendList(out, "errors", f.errors);
        out += "}";
    }
    out += "\n  ]\n}\n";
    return out;
}

std::string MigrateReport::summary() const {
    std::string out;
    size_t migrated = 0;
    size_t skipped = 0;
    size_t errors = 0;
    size_t checked = 0;
    size_t layerOne = 0;
    size_t layerOneNoMap = 0;
    size_t layerTwoDiffer = 0;
    size_t notChecked = 0;
    size_t ids = 0;
    size_t split = 0;
    size_t anchors = 0;
    for (const MigrateFileReport& f : files) {
        const std::string rel = relativeTo(f.path, root);
        migrated += f.status == "migrated" ? 1 : 0;
        skipped += f.status == "skipped" ? 1 : 0;
        errors += f.status == "error" ? 1 : 0;
        ids += f.idsAssigned.size();
        split += f.rulesSplit;
        anchors += f.anchors;
        if (f.kind == "ui" && f.status != "skipped") {
            if (f.layerTwo == MigrateCheck::NotChecked) {
                ++notChecked;
            } else {
                ++checked;
                layerOne += f.layerOne == MigrateCheck::Equal ? 1 : 0;
                layerOneNoMap += f.layerOneWithoutSourceMap == MigrateCheck::Equal ? 1 : 0;
                layerTwoDiffer += f.layerTwo == MigrateCheck::Differ ? 1 : 0;
            }
        }
        for (const std::string& e : f.errors) {
            out += "error: " + rel + ": " + e + "\n";
        }
        if (f.layerTwo == MigrateCheck::Differ) {
            out += "layer two differs: " + rel + ": " + f.checkDetail + "\n";
        }
    }
    out += "migrate: " + std::to_string(files.size()) + " files (" + std::to_string(migrated) +
           " migrated, " + std::to_string(skipped) + " skipped, " + std::to_string(errors) +
           " with errors); " + std::to_string(split) + " rules split, " + std::to_string(anchors) +
           " anchors, " + std::to_string(ids) + " ids assigned\n";
    out += "migrate: self-check of " + std::to_string(checked) + " UI documents: layer two " +
           std::to_string(checked - layerTwoDiffer) + " equal, " + std::to_string(layerTwoDiffer) +
           " differ; layer one " + std::to_string(layerOne) + " equal (" + std::to_string(layerOneNoMap) +
           " without SRCM); " + std::to_string(notChecked) + " not checked\n";
    return out;
}

bool writeMigratedFiles(const std::vector<MigratedFile>& files, const MigrateReport& report,
                        const std::string& outDir, std::string& error) {
    for (const MigratedFile& f : files) {
        fs::path target = pathFromUtf8(f.path);
        if (!outDir.empty()) {
            target = pathFromUtf8(outDir) / pathFromUtf8(relativeTo(f.path, report.root));
        }
        std::error_code ec;
        if (target.has_parent_path()) {
            fs::create_directories(target.parent_path(), ec);
        }
        // One retry: an antivirus scanner may hold a file that was just written next to it.
        if (!writeOnce(target, f.text) && !writeOnce(target, f.text)) {
            error = "cannot write '" + pathToUtf8(target) + "'";
            return false;
        }
    }
    return true;
}

}  // namespace xt::compile
