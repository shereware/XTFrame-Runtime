#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <json.hpp>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "bundle_json.h"
#include "design_internal.h"
#include "xt/bundle/design_system.h"
#include "xt/compile/utf8_path.h"
#include "yaml_lite.h"

/// design/ load and save, the variable metadata sync and the upgrade (docs/design/35-design-system.md
/// §35.2, §35.3, §35.3.4, §35.11; docs/tasks/T-305.md).
///
/// Every file is read with yaml_lite and written with emitYamlLite() from an ordered_json composed
/// in the fixed key order below, so a second save of the same data writes the same bytes. Keys a
/// reader does not know are kept in the entry's `extra` and written after the known ones,
/// alphabetically. Fixed key orders:
///   design.yaml    name, status, principles, sections, owners, languages, <extra>
///   palette.yaml   version, groups, swatches, contrast, <extra>
///     group        id, name, scale{base, steps, method}, status, source, approved, tags, links, note,
///                  swatches, <extra>
///     swatch       id, name, value, role, group (top-level list only), scale{base, step, method},
///                  status, source, approved, tags, links, note, <extra>; a swatch with nothing but an
///                  id is written as the bare id (the abbreviated group form)
///   tokens.yaml    version, tokens{--name (sorted by name): {name, category, description, from,
///                  status, source, approved, tags, links, note, <extra>}}, <extra>
///   type.yaml      roles[{id, name, family, weights, status, source, approved, tags, links, note,
///                  <extra>}], <extra>
///   text/*.md      front matter: id, title, name, section, status, source, approved, tags, applies-to,
///                  links, note, <extra>; then the body unchanged
///   *.meta         id (GUID), kind, title, name, status, source, approved, license, content, tags,
///                  annotations[{id, rect, note, links}], extracted[{value, share}], links, note,
///                  <extra>
///   source         kind, ref, at, <extra>;  approved  by, at;  link  rel, to, note
/// Empty strings and lists, a zero scale step and the Draft status are not written.

namespace xt::bundle::design {

using nlohmann::json;
using nlohmann::ordered_json;
namespace fs = std::filesystem;
using compile::genericPathToUtf8;
using compile::pathFromUtf8;
using compile::pathToUtf8;

namespace {

void setError(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

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

/// A float as the double of its shortest decimal form, so `0.12f` is written `0.12`.
ordered_json floatJson(float value) {
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    double d = static_cast<double>(value);
    std::from_chars(buffer, result.ptr, d);
    return ordered_json(d);
}

void putExtraKey(json& extra, const std::string& key, const json& value) {
    if (!extra.is_object()) {
        extra = json::object();
    }
    extra[key] = value;
}

bool extraHasDesignIo(const json& extra, const char* key) { return extra.is_object() && extra.contains(key); }

/// A scalar as text (`400` -> "400"); false for a collection or null.
bool scalarString(const json& v, std::string& out) {
    if (v.is_string()) {
        out = v.get<std::string>();
        return true;
    }
    if (v.is_number_integer() || v.is_number_unsigned() || v.is_number_float() || v.is_boolean()) {
        out = v.dump();
        return true;
    }
    return false;
}

/// `obj[key]` into `out` when it is a string; any other non-null value is kept raw in `extra`.
void readString(const json& obj, const char* key, std::string& out, json& extra) {
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) {
        return;
    }
    if (it->is_string()) {
        out = it->get<std::string>();
    } else {
        putExtraKey(extra, key, *it);
    }
}

/// `obj[key]` as a list of scalars (a lone scalar is a one-item list); anything else is kept raw.
void readStringList(const json& obj, const char* key, std::vector<std::string>& out, json& extra) {
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) {
        return;
    }
    std::string text;
    if (scalarString(*it, text)) {
        out.push_back(text);
        return;
    }
    if (!it->is_array()) {
        putExtraKey(extra, key, *it);
        return;
    }
    std::vector<std::string> items;
    for (const json& item : *it) {
        if (!scalarString(item, text)) {
            putExtraKey(extra, key, *it);
            return;
        }
        items.push_back(text);
    }
    out = std::move(items);
}

int readInt(const json& obj, const char* key, json& extra) {
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) {
        return 0;
    }
    if (it->is_number_integer() || it->is_number_unsigned()) {
        return static_cast<int>(it->get<int64_t>());
    }
    putExtraKey(extra, key, *it);
    return 0;
}

bool readLinkList(const json& value, std::vector<Link>& out) {
    if (!value.is_array()) {
        return false;
    }
    std::vector<Link> links;
    for (const json& item : value) {
        if (!item.is_object()) {
            return false;
        }
        Link link;
        json ignored;
        readString(item, "rel", link.rel, ignored);
        readString(item, "to", link.to, ignored);
        readString(item, "note", link.note, ignored);
        links.push_back(std::move(link));
    }
    out.insert(out.end(), links.begin(), links.end());
    return true;
}

bool knownKey(const std::string& key, std::initializer_list<const char*> known) {
    for (const char* k : known) {
        if (key == k) {
            return true;
        }
    }
    return false;
}

bool isCommonKey(const std::string& key) {
    return knownKey(key, {"id", "name", "status", "source", "approved", "tags", "links", "note"});
}

/// Reads the shared entry keys of `obj`; keys in neither the common set nor `specific` go to
/// `extra`. `id` is read by the caller.
void readCommon(const json& obj, Entry& e, std::initializer_list<const char*> specific) {
    readString(obj, "name", e.name, e.extra);
    if (const auto it = obj.find("status"); it != obj.end() && !it->is_null()) {
        const std::string text = it->is_string() ? it->get<std::string>() : std::string();
        if (text == "draft") {
            e.status = EntryStatus::Draft;
        } else if (text == "approved") {
            e.status = EntryStatus::Approved;
        } else if (text == "deprecated") {
            e.status = EntryStatus::Deprecated;
        } else {
            putExtraKey(e.extra, "status", *it);  // e.g. a decision's proposed / accepted / superseded
        }
    }
    if (const auto it = obj.find("source"); it != obj.end() && !it->is_null()) {
        if (it->is_object()) {
            json rest = json::object();
            for (auto s = it->begin(); s != it->end(); ++s) {
                if (s.key() == "kind" && s->is_string()) {
                    e.source.kind = s->get<std::string>();
                } else if (s.key() == "ref" && s->is_string()) {
                    e.source.ref = s->get<std::string>();
                } else if (s.key() == "at" && s->is_string()) {
                    e.source.at = s->get<std::string>();
                } else {
                    rest[s.key()] = *s;
                }
            }
            if (!rest.empty()) {
                putExtraKey(e.extra, "source", rest);
            }
        } else {
            putExtraKey(e.extra, "source", *it);
        }
    }
    if (const auto it = obj.find("approved"); it != obj.end() && !it->is_null()) {
        if (it->is_object()) {
            json ignored;
            readString(*it, "by", e.approved.by, ignored);
            readString(*it, "at", e.approved.at, ignored);
        } else {
            putExtraKey(e.extra, "approved", *it);
        }
    }
    readStringList(obj, "tags", e.tags, e.extra);
    if (const auto it = obj.find("links"); it != obj.end() && !it->is_null()) {
        if (!readLinkList(*it, e.links)) {
            putExtraKey(e.extra, "links", *it);
        }
    }
    readString(obj, "note", e.note, e.extra);
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (!isCommonKey(it.key()) && !knownKey(it.key(), specific)) {
            putExtraKey(e.extra, it.key(), it.value());
        }
    }
}

/// An id: a string, or a scalar written as one.
std::string readId(const json& obj, const char* key = "id") {
    std::string out;
    const auto it = obj.find(key);
    if (it != obj.end()) {
        scalarString(*it, out);
    }
    return out;
}

// --- writing ---

void putStringDesignIo(ordered_json& o, const char* key, const std::string& value, const json& extra) {
    if (extraHasDesignIo(extra, key)) {
        o[key] = ordered(extra[key]);
    } else if (!value.empty()) {
        o[key] = value;
    }
}

ordered_json listJson(const std::vector<std::string>& items, bool numbersAsNumbers = false) {
    ordered_json out = ordered_json::array();
    for (const std::string& item : items) {
        bool digits = numbersAsNumbers && !item.empty() && item.size() < 10;
        for (char c : item) {
            digits = digits && c >= '0' && c <= '9';
        }
        int number = 0;
        if (digits && std::from_chars(item.data(), item.data() + item.size(), number).ec == std::errc()) {
            out.push_back(number);
        } else {
            out.push_back(item);
        }
    }
    return out;
}

void putList(ordered_json& o, const char* key, const std::vector<std::string>& items, const json& extra) {
    if (extraHasDesignIo(extra, key)) {
        o[key] = ordered(extra[key]);
    } else if (!items.empty()) {
        o[key] = listJson(items);
    }
}

ordered_json linksJson(const std::vector<Link>& links) {
    ordered_json out = ordered_json::array();
    for (const Link& link : links) {
        ordered_json l = ordered_json::object();
        l["rel"] = link.rel;
        l["to"] = link.to;
        if (!link.note.empty()) {
            l["note"] = link.note;
        }
        out.push_back(std::move(l));
    }
    return out;
}

void putLinks(ordered_json& o, const std::vector<Link>& links, const json& extra) {
    if (extraHasDesignIo(extra, "links")) {
        o["links"] = ordered(extra["links"]);
    } else if (!links.empty()) {
        o["links"] = linksJson(links);
    }
}

void putStatus(ordered_json& o, const Entry& e) {
    if (extraHasDesignIo(e.extra, "status")) {
        o["status"] = ordered(e.extra["status"]);
    } else if (e.status != EntryStatus::Draft) {
        o["status"] = detail::statusText(e);
    }
}

void putSource(ordered_json& o, const Entry& e) {
    const bool rawSource = extraHasDesignIo(e.extra, "source") && !e.extra["source"].is_object();
    if (rawSource) {
        o["source"] = ordered(e.extra["source"]);
        return;
    }
    ordered_json s = ordered_json::object();
    if (!e.source.kind.empty()) {
        s["kind"] = e.source.kind;
    }
    if (!e.source.ref.empty()) {
        s["ref"] = e.source.ref;
    }
    if (!e.source.at.empty()) {
        s["at"] = e.source.at;
    }
    if (extraHasDesignIo(e.extra, "source")) {
        for (auto it = e.extra["source"].begin(); it != e.extra["source"].end(); ++it) {
            if (!s.contains(it.key())) {
                s[it.key()] = ordered(it.value());
            }
        }
    }
    if (!s.empty()) {
        o["source"] = std::move(s);
    }
}

void putApproved(ordered_json& o, const Entry& e) {
    if (extraHasDesignIo(e.extra, "approved")) {
        o["approved"] = ordered(e.extra["approved"]);
        return;
    }
    if (e.approved.by.empty() && e.approved.at.empty()) {
        return;
    }
    ordered_json a = ordered_json::object();
    if (!e.approved.by.empty()) {
        a["by"] = e.approved.by;
    }
    if (!e.approved.at.empty()) {
        a["at"] = e.approved.at;
    }
    o["approved"] = std::move(a);
}

/// status, source, approved, tags (in that order).
void putMiddle(ordered_json& o, const Entry& e) {
    putStatus(o, e);
    putSource(o, e);
    putApproved(o, e);
    putList(o, "tags", e.tags, e.extra);
}

/// The unknown keys, alphabetically (json is a sorted map), skipping keys already written.
void putExtra(ordered_json& o, const json& extra) {
    if (!extra.is_object()) {
        return;
    }
    for (auto it = extra.begin(); it != extra.end(); ++it) {
        if (!o.contains(it.key()) && !knownKey(it.key(), {"status", "source", "approved", "links", "tags"})) {
            o[it.key()] = ordered(it.value());
        }
    }
}

bool entryIsBare(const Entry& e) {
    return e.name.empty() && e.status == EntryStatus::Draft && e.source.kind.empty() &&
           e.source.ref.empty() && e.source.at.empty() && e.approved.by.empty() && e.approved.at.empty() &&
           e.tags.empty() && e.links.empty() && e.note.empty() && (e.extra.is_null() || e.extra.empty());
}

// --- files ---

/// Reads a YAML file; a missing file is null. False (with `*error`) when it is malformed or not a
/// mapping.
bool readYamlFile(const fs::path& path, const std::string& label, json& out, std::string* error) {
    out = json(nullptr);
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return true;
    }
    std::string text;
    if (!bundle::detail::readFile(pathToUtf8(path), text)) {
        setError(error, label + ": cannot read");
        return false;
    }
    std::string message;
    if (!bundle::detail::parseYamlLite(text, out, &message)) {
        setError(error, label + ": " + message);
        return false;
    }
    if (!out.is_null() && !out.is_object()) {
        setError(error, label + ": not a mapping");
        return false;
    }
    return true;
}

bool writeText(const fs::path& path, const std::string& text, std::string* error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (!bundle::detail::writeFile(pathToUtf8(path), text)) {
        setError(error, "cannot write '" + genericPathToUtf8(path) + "'");
        return false;
    }
    return true;
}

/// Regular files under `dir` (recursively) whose name ends with `suffix`, sorted by generic path.
std::vector<fs::path> filesUnder(const fs::path& dir, std::string_view suffix) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return out;
    }
    for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        if (!it->is_regular_file(ec)) {
            continue;
        }
        const std::string name = pathToUtf8(it->path().filename());
        if (name.size() >= suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            out.push_back(it->path());
        }
    }
    std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) {
        return genericPathToUtf8(a) < genericPathToUtf8(b);
    });
    return out;
}

std::string relativeGeneric(const fs::path& path, const fs::path& base) {
    return genericPathToUtf8(path.lexically_normal().lexically_relative(base.lexically_normal()));
}

/// `name` without its last extension.
std::string stripExtension(const std::string& path) {
    const size_t slash = path.rfind('/');
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash) || dot == slash + 1) {
        return path;
    }
    return path.substr(0, dot);
}

// --- design.yaml ---

void readInfo(const json& root, DesignInfo& info) {
    if (!root.is_object()) {
        return;
    }
    readString(root, "name", info.name, info.extra);
    readString(root, "status", info.status, info.extra);
    readStringList(root, "principles", info.principles, info.extra);
    readStringList(root, "sections", info.sections, info.extra);
    readStringList(root, "owners", info.owners, info.extra);
    readStringList(root, "languages", info.languages, info.extra);
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (!knownKey(it.key(), {"name", "status", "principles", "sections", "owners", "languages"})) {
            putExtraKey(info.extra, it.key(), it.value());
        }
    }
}

ordered_json infoJson(const DesignInfo& info) {
    ordered_json o = ordered_json::object();
    putStringDesignIo(o, "name", info.name, info.extra);
    putStringDesignIo(o, "status", info.status, info.extra);
    putList(o, "principles", info.principles, info.extra);
    putList(o, "sections", info.sections, info.extra);
    putList(o, "owners", info.owners, info.extra);
    putList(o, "languages", info.languages, info.extra);
    if (info.extra.is_object()) {
        for (auto it = info.extra.begin(); it != info.extra.end(); ++it) {
            if (!o.contains(it.key())) {
                o[it.key()] = ordered(it.value());
            }
        }
    }
    return o;
}

// --- palette.yaml ---

Swatch readSwatch(const json& item, const std::string& groupId, bool topLevel) {
    Swatch s;
    s.base.kind = EntryKind::Swatch;
    s.group = groupId;
    if (!item.is_object()) {
        scalarString(item, s.base.id);
        return s;
    }
    s.base.id = readId(item);
    readString(item, "value", s.value, s.base.extra);
    readString(item, "role", s.role, s.base.extra);
    if (topLevel) {
        readString(item, "group", s.group, s.base.extra);
    }
    if (const auto it = item.find("scale"); it != item.end() && !it->is_null()) {
        if (it->is_object()) {
            json ignored;
            readString(*it, "base", s.scale.base, ignored);
            s.scale.step = readInt(*it, "step", ignored);
            readString(*it, "method", s.scale.method, ignored);
        } else {
            putExtraKey(s.base.extra, "scale", *it);
        }
    }
    if (topLevel) {
        readCommon(item, s.base, {"value", "role", "group", "scale"});
    } else {
        readCommon(item, s.base, {"value", "role", "scale"});
    }
    return s;
}

ordered_json swatchJson(const Swatch& s, bool topLevel) {
    if (s.value.empty() && s.role.empty() && s.scale.base.empty() && s.scale.step == 0 &&
        s.scale.method.empty() && (!topLevel || s.group.empty()) && entryIsBare(s.base)) {
        return ordered_json(s.base.id);
    }
    ordered_json o = ordered_json::object();
    o["id"] = s.base.id;
    putStringDesignIo(o, "name", s.base.name, s.base.extra);
    putStringDesignIo(o, "value", s.value, s.base.extra);
    putStringDesignIo(o, "role", s.role, s.base.extra);
    if (topLevel) {
        putStringDesignIo(o, "group", s.group, s.base.extra);
    }
    if (extraHasDesignIo(s.base.extra, "scale")) {
        o["scale"] = ordered(s.base.extra["scale"]);
    } else if (!s.scale.base.empty() || s.scale.step != 0 || !s.scale.method.empty()) {
        ordered_json scale = ordered_json::object();
        if (!s.scale.base.empty()) {
            scale["base"] = s.scale.base;
        }
        if (s.scale.step != 0) {
            scale["step"] = s.scale.step;
        }
        if (!s.scale.method.empty()) {
            scale["method"] = s.scale.method;
        }
        o["scale"] = std::move(scale);
    }
    putMiddle(o, s.base);
    putLinks(o, s.base.links, s.base.extra);
    putStringDesignIo(o, "note", s.base.note, s.base.extra);
    putExtra(o, s.base.extra);
    return o;
}

void readPalette(const json& root, DesignSystem& ds) {
    if (!root.is_object()) {
        return;
    }
    for (auto it = root.begin(); it != root.end(); ++it) {
        const std::string& key = it.key();
        if (key == "groups" && it->is_array()) {
            for (const json& item : *it) {
                if (!item.is_object()) {
                    continue;
                }
                Group g;
                g.base.kind = EntryKind::Group;
                g.base.id = readId(item);
                if (const auto sc = item.find("scale"); sc != item.end() && !sc->is_null()) {
                    if (sc->is_object()) {
                        json ignored;
                        readString(*sc, "base", g.scale.base, ignored);
                        g.scale.steps = readInt(*sc, "steps", ignored);
                        readString(*sc, "method", g.scale.method, ignored);
                    } else {
                        putExtraKey(g.base.extra, "scale", *sc);
                    }
                }
                if (const auto sw = item.find("swatches"); sw != item.end() && !sw->is_null()) {
                    if (sw->is_array()) {
                        for (const json& entry : *sw) {
                            Swatch s = readSwatch(entry, g.base.id, /*topLevel=*/false);
                            if (s.base.id.empty()) {
                                continue;
                            }
                            g.swatches.push_back(s.base.id);
                            ds.swatches.push_back(std::move(s));
                        }
                    } else {
                        putExtraKey(g.base.extra, "swatches", *sw);
                    }
                }
                readCommon(item, g.base, {"scale", "swatches"});
                g.name = g.base.name;
                ds.groups.push_back(std::move(g));
            }
        } else if (key == "swatches" && it->is_array()) {
            for (const json& entry : *it) {
                Swatch s = readSwatch(entry, std::string(), /*topLevel=*/true);
                if (s.base.id.empty()) {
                    continue;
                }
                // The abbreviated group form lists the id only; the full entry here fills it in.
                bool merged = false;
                for (Swatch& existing : ds.swatches) {
                    if (existing.base.id == s.base.id && existing.value.empty() &&
                        entryIsBare(existing.base)) {
                        const std::string group = existing.group;
                        existing = std::move(s);
                        if (existing.group.empty()) {
                            existing.group = group;
                        }
                        merged = true;
                        break;
                    }
                }
                if (!merged) {
                    ds.swatches.push_back(std::move(s));
                }
            }
        } else {
            putExtraKey(ds.paletteExtra, key, it.value());
        }
    }
}

bool groupExists(const DesignSystem& ds, const std::string& id) {
    for (const Group& g : ds.groups) {
        if (g.base.id == id) {
            return true;
        }
    }
    return false;
}

ordered_json paletteJson(const DesignSystem& ds) {
    ordered_json o = ordered_json::object();
    if (extraHasDesignIo(ds.paletteExtra, "version")) {
        o["version"] = ordered(ds.paletteExtra["version"]);
    }
    if (!ds.groups.empty()) {
        ordered_json groups = ordered_json::array();
        for (const Group& g : ds.groups) {
            ordered_json go = ordered_json::object();
            go["id"] = g.base.id;
            putStringDesignIo(go, "name", g.name.empty() ? g.base.name : g.name, g.base.extra);
            if (extraHasDesignIo(g.base.extra, "scale")) {
                go["scale"] = ordered(g.base.extra["scale"]);
            } else if (!g.scale.base.empty() || g.scale.steps != 0 || !g.scale.method.empty()) {
                ordered_json scale = ordered_json::object();
                if (!g.scale.base.empty()) {
                    scale["base"] = g.scale.base;
                }
                if (g.scale.steps != 0) {
                    scale["steps"] = g.scale.steps;
                }
                if (!g.scale.method.empty()) {
                    scale["method"] = g.scale.method;
                }
                go["scale"] = std::move(scale);
            }
            putMiddle(go, g.base);
            putLinks(go, g.base.links, g.base.extra);
            putStringDesignIo(go, "note", g.base.note, g.base.extra);
            if (extraHasDesignIo(g.base.extra, "swatches")) {
                go["swatches"] = ordered(g.base.extra["swatches"]);
            } else {
                ordered_json list = ordered_json::array();
                std::set<std::string> written;
                for (const std::string& id : g.swatches) {
                    const Swatch* found = nullptr;
                    for (const Swatch& s : ds.swatches) {
                        if (s.base.id == id && s.group == g.base.id) {
                            found = &s;
                            break;
                        }
                    }
                    list.push_back(found != nullptr ? swatchJson(*found, false) : ordered_json(id));
                    written.insert(id);
                }
                for (const Swatch& s : ds.swatches) {
                    if (s.group == g.base.id && written.insert(s.base.id).second) {
                        list.push_back(swatchJson(s, false));
                    }
                }
                if (!list.empty()) {
                    go["swatches"] = std::move(list);
                }
            }
            putExtra(go, g.base.extra);
            groups.push_back(std::move(go));
        }
        o["groups"] = std::move(groups);
    }
    ordered_json loose = ordered_json::array();
    for (const Swatch& s : ds.swatches) {
        if (s.group.empty() || !groupExists(ds, s.group)) {
            loose.push_back(swatchJson(s, true));
        }
    }
    if (!loose.empty()) {
        o["swatches"] = std::move(loose);
    }
    if (extraHasDesignIo(ds.paletteExtra, "contrast")) {
        o["contrast"] = ordered(ds.paletteExtra["contrast"]);
    }
    if (ds.paletteExtra.is_object()) {
        for (auto it = ds.paletteExtra.begin(); it != ds.paletteExtra.end(); ++it) {
            if (!o.contains(it.key())) {
                o[it.key()] = ordered(it.value());
            }
        }
    }
    return o;
}

// --- tokens.yaml ---

void readTokens(const json& root, DesignSystem& ds) {
    if (!root.is_object()) {
        return;
    }
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (it.key() != "tokens" || !it->is_object()) {
            putExtraKey(ds.tokensExtra, it.key(), it.value());
            continue;
        }
        for (auto t = it->begin(); t != it->end(); ++t) {
            TokenMeta meta;
            meta.base.kind = EntryKind::Token;
            const std::string& key = t.key();
            meta.base.id = key.size() > 2 && key.compare(0, 2, "--") == 0 ? key.substr(2) : key;
            const json& obj = t.value();
            if (obj.is_object()) {
                readString(obj, "category", meta.category, meta.base.extra);
                readString(obj, "description", meta.description, meta.base.extra);
                readString(obj, "from", meta.from, meta.base.extra);
                readCommon(obj, meta.base, {"category", "description", "from"});
            } else if (!obj.is_null()) {
                putExtraKey(meta.base.extra, "value", obj);
            }
            ds.tokens.push_back(std::move(meta));
        }
    }
}

ordered_json tokenJson(const TokenMeta& t) {
    ordered_json o = ordered_json::object();
    putStringDesignIo(o, "name", t.base.name, t.base.extra);
    putStringDesignIo(o, "category", t.category, t.base.extra);
    putStringDesignIo(o, "description", t.description, t.base.extra);
    putStringDesignIo(o, "from", t.from, t.base.extra);
    putMiddle(o, t.base);
    putLinks(o, t.base.links, t.base.extra);
    putStringDesignIo(o, "note", t.base.note, t.base.extra);
    putExtra(o, t.base.extra);
    return o;
}

ordered_json tokensJson(const DesignSystem& ds) {
    ordered_json o = ordered_json::object();
    if (extraHasDesignIo(ds.tokensExtra, "version")) {
        o["version"] = ordered(ds.tokensExtra["version"]);
    }
    // Sorted by name: yaml_lite reads a mapping into a sorted json object, so this is the order a
    // load hands back, and a save after a load is byte-identical.
    std::vector<const TokenMeta*> sorted;
    for (const TokenMeta& t : ds.tokens) {
        sorted.push_back(&t);
    }
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const TokenMeta* x, const TokenMeta* y) { return x->base.id < y->base.id; });
    ordered_json tokens = ordered_json::object();
    for (const TokenMeta* t : sorted) {
        tokens["--" + t->base.id] = tokenJson(*t);
    }
    o["tokens"] = std::move(tokens);
    if (ds.tokensExtra.is_object()) {
        for (auto it = ds.tokensExtra.begin(); it != ds.tokensExtra.end(); ++it) {
            if (!o.contains(it.key())) {
                o[it.key()] = ordered(it.value());
            }
        }
    }
    return o;
}

// --- type.yaml ---

void readType(const json& root, DesignSystem& ds) {
    if (!root.is_object()) {
        return;
    }
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (it.key() != "roles" || !it->is_array()) {
            putExtraKey(ds.typeExtra, it.key(), it.value());
            continue;
        }
        for (const json& item : *it) {
            if (!item.is_object()) {
                continue;
            }
            TypeRole role;
            role.base.kind = EntryKind::TypeRole;
            role.base.id = readId(item);
            readString(item, "family", role.family, role.base.extra);
            readStringList(item, "weights", role.weights, role.base.extra);
            readCommon(item, role.base, {"family", "weights"});
            ds.typeRoles.push_back(std::move(role));
        }
    }
}

ordered_json typeJson(const DesignSystem& ds) {
    ordered_json o = ordered_json::object();
    if (!ds.typeRoles.empty()) {
        ordered_json roles = ordered_json::array();
        for (const TypeRole& r : ds.typeRoles) {
            ordered_json ro = ordered_json::object();
            ro["id"] = r.base.id;
            putStringDesignIo(ro, "name", r.base.name, r.base.extra);
            putStringDesignIo(ro, "family", r.family, r.base.extra);
            if (extraHasDesignIo(r.base.extra, "weights")) {
                ro["weights"] = ordered(r.base.extra["weights"]);
            } else if (!r.weights.empty()) {
                ro["weights"] = listJson(r.weights, /*numbersAsNumbers=*/true);
            }
            putMiddle(ro, r.base);
            putLinks(ro, r.base.links, r.base.extra);
            putStringDesignIo(ro, "note", r.base.note, r.base.extra);
            putExtra(ro, r.base.extra);
            roles.push_back(std::move(ro));
        }
        o["roles"] = std::move(roles);
    }
    if (ds.typeExtra.is_object()) {
        for (auto it = ds.typeExtra.begin(); it != ds.typeExtra.end(); ++it) {
            if (!o.contains(it.key())) {
                o[it.key()] = ordered(it.value());
            }
        }
    }
    return o;
}

// --- text/*.md ---

/// Splits a Markdown file into its front matter YAML and body. No front matter: empty YAML and the
/// whole text as body.
void splitFrontMatter(std::string_view text, std::string& yaml, std::string& body) {
    yaml.clear();
    std::string_view rest = text;
    if (rest.size() >= 3 && static_cast<unsigned char>(rest[0]) == 0xEF &&
        static_cast<unsigned char>(rest[1]) == 0xBB && static_cast<unsigned char>(rest[2]) == 0xBF) {
        rest.remove_prefix(3);
    }
    const auto firstLineEnd = rest.find('\n');
    std::string_view first =
        rest.substr(0, firstLineEnd == std::string_view::npos ? rest.size() : firstLineEnd);
    if (!first.empty() && first.back() == '\r') {
        first.remove_suffix(1);
    }
    if (first != "---" || firstLineEnd == std::string_view::npos) {
        body = std::string(text);
        return;
    }
    size_t pos = firstLineEnd + 1;
    while (pos <= rest.size()) {
        size_t end = rest.find('\n', pos);
        const bool last = end == std::string_view::npos;
        if (last) {
            end = rest.size();
        }
        std::string_view line = rest.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line == "---" || line == "...") {
            yaml = std::string(rest.substr(firstLineEnd + 1, pos - firstLineEnd - 1));
            body = last ? std::string() : std::string(rest.substr(end + 1));
            return;
        }
        if (last) {
            break;
        }
        pos = end + 1;
    }
    body = std::string(text);  // An unterminated front matter is not one.
}

bool readTextEntry(const fs::path& file, const fs::path& designRoot, EntryKind kind, TextEntry& out,
                   std::string* error) {
    const std::string relative = relativeGeneric(file, designRoot);
    std::string text;
    if (!bundle::detail::readFile(pathToUtf8(file), text)) {
        setError(error, relative + ": cannot read");
        return false;
    }
    std::string yaml;
    splitFrontMatter(text, yaml, out.body);
    json fm(nullptr);
    std::string message;
    if (!yaml.empty() && !bundle::detail::parseYamlLite(yaml, fm, &message)) {
        setError(error, relative + ": front matter " + message);
        return false;
    }
    if (!fm.is_null() && !fm.is_object()) {
        setError(error, relative + ": front matter is not a mapping");
        return false;
    }
    out.base.kind = kind;
    out.path = relative;
    // Default ids: text = the path under text/ without `.md`; decision = the file's leading number.
    std::string id = stripExtension(relative.substr(relative.find('/') + 1));
    if (kind == EntryKind::Decision) {
        const std::string fileName = pathToUtf8(file.filename());
        size_t digits = 0;
        while (digits < fileName.size() && fileName[digits] >= '0' && fileName[digits] <= '9') {
            ++digits;
        }
        id = digits > 0 ? fileName.substr(0, digits) : stripExtension(fileName);
    }
    if (fm.is_object()) {
        const auto idIt = fm.find("id");
        if (idIt != fm.end() && idIt->is_string() && !idIt->get<std::string>().empty()) {
            id = idIt->get<std::string>();
        }
        readString(fm, "title", out.title, out.base.extra);
        readStringList(fm, "section", out.sections, out.base.extra);
        readCommon(fm, out.base, {"title", "section", "applies-to"});
        if (const auto ap = fm.find("applies-to"); ap != fm.end() && !ap->is_null()) {
            std::vector<std::string> targets;
            json scratch;
            readStringList(fm, "applies-to", targets, scratch);
            if (!scratch.is_null()) {
                putExtraKey(out.base.extra, "applies-to", *ap);
            }
            for (const std::string& target : targets) {
                out.base.links.push_back(Link{"applies-to", target, std::string()});
            }
        }
    }
    out.base.id = id;
    return true;
}

std::string textFileContent(const TextEntry& t) {
    ordered_json o = ordered_json::object();
    o["id"] = t.base.id;
    putStringDesignIo(o, "title", t.title, t.base.extra);
    putStringDesignIo(o, "name", t.base.name, t.base.extra);
    if (extraHasDesignIo(t.base.extra, "section")) {
        o["section"] = ordered(t.base.extra["section"]);
    } else if (t.sections.size() == 1) {
        o["section"] = t.sections[0];
    } else if (!t.sections.empty()) {
        o["section"] = listJson(t.sections);
    }
    putMiddle(o, t.base);
    // `applies-to` is the short form of applies-to links without a note (§35.3.2).
    std::vector<std::string> appliesTo;
    std::vector<Link> links;
    for (const Link& link : t.base.links) {
        if (link.rel == "applies-to" && link.note.empty() && !extraHasDesignIo(t.base.extra, "applies-to")) {
            appliesTo.push_back(link.to);
        } else {
            links.push_back(link);
        }
    }
    if (extraHasDesignIo(t.base.extra, "applies-to")) {
        o["applies-to"] = ordered(t.base.extra["applies-to"]);
    } else if (!appliesTo.empty()) {
        o["applies-to"] = listJson(appliesTo);
    }
    putLinks(o, links, t.base.extra);
    putStringDesignIo(o, "note", t.base.note, t.base.extra);
    putExtra(o, t.base.extra);
    return "---\n" + bundle::detail::emitYamlLite(o) + "---\n" + t.body;
}

// --- references/**/*.meta ---

bool readReference(const fs::path& metaFile, const fs::path& designRoot, ReferenceMeta& out,
                   std::string* error) {
    const std::string relativeMeta = relativeGeneric(metaFile, designRoot);
    json root;
    if (!readYamlFile(metaFile, relativeMeta, root, error)) {
        return false;
    }
    out.base.kind = EntryKind::Reference;
    out.path = relativeMeta.substr(0, relativeMeta.size() - 5);  // without ".meta"
    const std::string underRefs = out.path.substr(out.path.find('/') + 1);
    out.base.id = stripExtension(underRefs);
    if (!root.is_object()) {
        return true;
    }
    out.guid = readId(root);
    readString(root, "title", out.title, out.base.extra);
    readString(root, "license", out.license, out.base.extra);
    readString(root, "content", out.contentHash, out.base.extra);
    if (const auto it = root.find("annotations"); it != root.end() && !it->is_null()) {
        if (it->is_array()) {
            for (const json& item : *it) {
                if (!item.is_object()) {
                    continue;
                }
                ReferenceMeta::Annotation a;
                a.id = readId(item);
                if (const auto r = item.find("rect"); r != item.end() && r->is_array() && r->size() == 4) {
                    for (size_t i = 0; i < 4; ++i) {
                        a.rect[i] = (*r)[i].is_number() ? (*r)[i].get<float>() : 0.0f;
                    }
                }
                json ignored;
                readString(item, "note", a.note, ignored);
                if (const auto l = item.find("links"); l != item.end()) {
                    readLinkList(*l, a.links);
                }
                out.annotations.push_back(std::move(a));
            }
        } else {
            putExtraKey(out.base.extra, "annotations", *it);
        }
    }
    if (const auto it = root.find("extracted"); it != root.end() && !it->is_null()) {
        if (it->is_array()) {
            for (const json& item : *it) {
                if (!item.is_object()) {
                    continue;
                }
                ReferenceMeta::Extracted x;
                json ignored;
                readString(item, "value", x.value, ignored);
                if (const auto s = item.find("share"); s != item.end() && s->is_number()) {
                    x.share = s->get<float>();
                }
                out.extracted.push_back(std::move(x));
            }
        } else {
            putExtraKey(out.base.extra, "extracted", *it);
        }
    }
    readCommon(root, out.base, {"kind", "title", "license", "content", "annotations", "extracted"});
    return true;
}

std::string referenceMetaContent(const ReferenceMeta& r) {
    ordered_json o = ordered_json::object();
    if (!r.guid.empty()) {
        o["id"] = r.guid;
    }
    o["kind"] = "reference";
    putStringDesignIo(o, "title", r.title, r.base.extra);
    putStringDesignIo(o, "name", r.base.name, r.base.extra);
    putMiddle(o, r.base);
    putStringDesignIo(o, "license", r.license, r.base.extra);
    putStringDesignIo(o, "content", r.contentHash, r.base.extra);
    if (extraHasDesignIo(r.base.extra, "annotations")) {
        o["annotations"] = ordered(r.base.extra["annotations"]);
    } else if (!r.annotations.empty()) {
        ordered_json list = ordered_json::array();
        for (const ReferenceMeta::Annotation& a : r.annotations) {
            ordered_json ao = ordered_json::object();
            ao["id"] = a.id;
            ordered_json rect = ordered_json::array();
            for (float v : a.rect) {
                rect.push_back(floatJson(v));
            }
            ao["rect"] = std::move(rect);
            if (!a.note.empty()) {
                ao["note"] = a.note;
            }
            if (!a.links.empty()) {
                ao["links"] = linksJson(a.links);
            }
            list.push_back(std::move(ao));
        }
        o["annotations"] = std::move(list);
    }
    if (extraHasDesignIo(r.base.extra, "extracted")) {
        o["extracted"] = ordered(r.base.extra["extracted"]);
    } else if (!r.extracted.empty()) {
        ordered_json list = ordered_json::array();
        for (const ReferenceMeta::Extracted& x : r.extracted) {
            ordered_json xo = ordered_json::object();
            xo["value"] = x.value;
            xo["share"] = floatJson(x.share);
            list.push_back(std::move(xo));
        }
        o["extracted"] = std::move(list);
    }
    putLinks(o, r.base.links, r.base.extra);
    putStringDesignIo(o, "note", r.base.note, r.base.extra);
    putExtra(o, r.base.extra);
    return bundle::detail::emitYamlLite(o);
}

bool hasContent(const json& extra) { return extra.is_object() && !extra.empty(); }

/// §35.3.4: the category of a variable without metadata, from its name (without `--`). The first
/// rule that matches wins:
///   color   starts with `color` or `bg`, or contains `color`
///   type    starts with `font`, `text`, `line-height`, `letter-spacing` or `type`
///   radius  contains `radius`
///   shadow  contains `shadow` or starts with `elevation`
///   border  starts with `border`, `outline` or `stroke`, or ends with `-w` or `-width`
///   motion  starts with `motion`, `duration`, `ease`, `easing`, `delay` or `transition`
///   space   starts with `space`, `gap`, `pad`, `margin`, `inset` or `safe`
///   size    starts with `size`, `control`, `icon`, `width` or `height`, or ends with `-size`
///   other   everything else
std::string guessCategory(const std::string& name) {
    const auto starts = [&name](std::initializer_list<const char*> prefixes) {
        for (const char* p : prefixes) {
            if (name.rfind(p, 0) == 0) {
                return true;
            }
        }
        return false;
    };
    const auto ends = [&name](const char* suffix) {
        const std::string s(suffix);
        return name.size() >= s.size() && name.compare(name.size() - s.size(), s.size(), s) == 0;
    };
    const auto contains = [&name](const char* part) { return name.find(part) != std::string::npos; };
    if (starts({"color", "bg"}) || contains("color")) {
        return "color";
    }
    if (starts({"font", "text", "line-height", "letter-spacing", "type"})) {
        return "type";
    }
    if (contains("radius")) {
        return "radius";
    }
    if (contains("shadow") || starts({"elevation"})) {
        return "shadow";
    }
    if (starts({"border", "outline", "stroke"}) || ends("-w") || ends("-width")) {
        return "border";
    }
    if (starts({"motion", "duration", "ease", "easing", "delay", "transition"})) {
        return "motion";
    }
    if (starts({"space", "gap", "pad", "margin", "inset", "safe"})) {
        return "space";
    }
    if (starts({"size", "control", "icon", "width", "height"}) || ends("-size")) {
        return "size";
    }
    return "other";
}

constexpr std::string_view kPrinciplesTemplate =
    "---\n"
    "id: principles\n"
    "title: Principles\n"
    "section: identity\n"
    "---\n"
    "# Principles\n"
    "\n"
    "One heading per principle; each heading is a link target (`text:principles#<heading>`).\n"
    "\n"
    "## Readable at a glance\n"
    "\n"
    "The information a player needs right now reads in under a second at the smallest target resolution.\n";

constexpr std::string_view kVoiceTemplate =
    "---\n"
    "id: voice\n"
    "title: Voice\n"
    "section: voice\n"
    "---\n"
    "# Voice\n"
    "\n"
    "How the UI speaks: tone, capitalisation, numbers and localisation notes.\n"
    "\n"
    "## Tone\n"
    "\n"
    "## Capitalisation\n"
    "\n"
    "## Numbers\n";

constexpr std::string_view kDesignMdPlaceholder =
    "<!-- generated by xtframe-design; run xtframe-design build -->\n";

/// Moves one file, falling back to copy + remove across volumes.
bool moveOne(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    fs::rename(from, to, ec);
    if (!ec) {
        return true;
    }
    ec.clear();
    fs::copy_file(from, to, fs::copy_options::none, ec);
    if (ec) {
        return false;
    }
    fs::remove(from, ec);
    return true;
}

/// Removes the empty directories under `dir` (deepest first), then `dir` itself when empty.
void removeEmptyDirs(const fs::path& dir) {
    std::error_code ec;
    std::vector<fs::path> dirs;
    for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        if (it->is_directory(ec)) {
            dirs.push_back(it->path());
        }
    }
    std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) {
        return genericPathToUtf8(a).size() > genericPathToUtf8(b).size();
    });
    for (const fs::path& d : dirs) {
        if (fs::is_empty(d, ec)) {
            fs::remove(d, ec);
        }
    }
    if (fs::is_directory(dir, ec) && fs::is_empty(dir, ec)) {
        fs::remove(dir, ec);
    }
}

}  // namespace

Status load(const UiProject& project, DesignSystem& out, std::vector<Issue>* issues, std::string* error) {
    (void)issues;
    setError(error, std::string());
    const fs::path root = pathFromUtf8(designDir(project));
    std::error_code ec;
    if (project.rootDir.empty() || !fs::is_directory(root, ec)) {
        setError(error, "'" + genericPathToUtf8(root) + "' does not exist");
        return Status::kNotFound;
    }
    DesignSystem ds;
    ds.rootDir = genericPathToUtf8(root.lexically_normal());
    while (ds.rootDir.size() > 1 && ds.rootDir.back() == '/') {
        ds.rootDir.pop_back();
    }
    json doc;
    if (!readYamlFile(root / "design.yaml", "design.yaml", doc, error)) {
        return Status::kInvalidArgument;
    }
    readInfo(doc, ds.info);
    if (!readYamlFile(root / "palette.yaml", "palette.yaml", doc, error)) {
        return Status::kInvalidArgument;
    }
    readPalette(doc, ds);
    if (!readYamlFile(root / "tokens.yaml", "tokens.yaml", doc, error)) {
        return Status::kInvalidArgument;
    }
    readTokens(doc, ds);
    if (!readYamlFile(root / "type.yaml", "type.yaml", doc, error)) {
        return Status::kInvalidArgument;
    }
    readType(doc, ds);
    for (const fs::path& file : filesUnder(root / "text", ".md")) {
        const std::string relative = relativeGeneric(file, root);
        const bool decision = relative.rfind("text/decisions/", 0) == 0;
        TextEntry entry;
        if (!readTextEntry(file, root, decision ? EntryKind::Decision : EntryKind::Text, entry, error)) {
            return Status::kInvalidArgument;
        }
        (decision ? ds.decisions : ds.texts).push_back(std::move(entry));
    }
    for (const fs::path& file : filesUnder(root / "references", ".meta")) {
        ReferenceMeta ref;
        if (!readReference(file, root, ref, error)) {
            return Status::kInvalidArgument;
        }
        ds.references.push_back(std::move(ref));
    }
    out = std::move(ds);
    return Status::kOk;
}

Status save(const DesignSystem& ds, std::string* error) {
    setError(error, std::string());
    if (ds.rootDir.empty()) {
        setError(error, "cannot save a design system without a rootDir");
        return Status::kInvalidArgument;
    }
    const fs::path root = pathFromUtf8(ds.rootDir);
    const DesignInfo& info = ds.info;
    if (!info.name.empty() || !info.status.empty() || !info.principles.empty() || !info.sections.empty() ||
        !info.owners.empty() || !info.languages.empty() || hasContent(info.extra)) {
        if (!writeText(root / "design.yaml", bundle::detail::emitYamlLite(infoJson(info)), error)) {
            return Status::kIoError;
        }
    }
    if (!ds.groups.empty() || !ds.swatches.empty() || hasContent(ds.paletteExtra)) {
        if (!writeText(root / "palette.yaml", bundle::detail::emitYamlLite(paletteJson(ds)), error)) {
            return Status::kIoError;
        }
    }
    if (!ds.tokens.empty() || hasContent(ds.tokensExtra)) {
        if (!writeText(root / "tokens.yaml", bundle::detail::emitYamlLite(tokensJson(ds)), error)) {
            return Status::kIoError;
        }
    }
    if (!ds.typeRoles.empty() || hasContent(ds.typeExtra)) {
        if (!writeText(root / "type.yaml", bundle::detail::emitYamlLite(typeJson(ds)), error)) {
            return Status::kIoError;
        }
    }
    for (const std::vector<TextEntry>* list : {&ds.texts, &ds.decisions}) {
        for (const TextEntry& t : *list) {
            if (t.path.empty()) {
                setError(error, "text entry '" + t.base.id + "' has no path");
                return Status::kInvalidArgument;
            }
            if (!writeText(root / pathFromUtf8(t.path), textFileContent(t), error)) {
                return Status::kIoError;
            }
        }
    }
    for (const ReferenceMeta& r : ds.references) {
        if (r.path.empty()) {
            setError(error, "reference '" + r.base.id + "' has no path");
            return Status::kInvalidArgument;
        }
        if (!writeText(root / pathFromUtf8(r.path + ".meta"), referenceMetaContent(r), error)) {
            return Status::kIoError;
        }
    }
    return Status::kOk;
}

Status syncTokenMeta(DesignSystem& ds, const UiProject& project, std::vector<Issue>* issues) {
    const std::vector<std::string> names = detail::sharedTokenNames(project);
    std::set<std::string> inSheet;
    std::set<std::string> withMeta;
    for (const TokenMeta& t : ds.tokens) {
        withMeta.insert(t.base.id);
    }
    for (const std::string& name : names) {
        const std::string id = name.substr(2);
        inSheet.insert(id);
        if (withMeta.count(id) != 0) {
            continue;
        }
        TokenMeta meta;
        meta.base.kind = EntryKind::Token;
        meta.base.id = id;
        meta.base.status = EntryStatus::Draft;
        meta.category = guessCategory(id);
        ds.tokens.push_back(std::move(meta));
        withMeta.insert(id);
        if (issues != nullptr) {
            issues->push_back(Issue{"design.tokenNoMeta", Severity::Info, entryRef(EntryKind::Token, id),
                                    "'" + name + "' has no metadata in tokens.yaml; added as a draft"});
        }
    }
    for (const TokenMeta& t : ds.tokens) {
        if (inSheet.count(t.base.id) == 0 && issues != nullptr) {
            issues->push_back(Issue{
                "design.orphanMeta", Severity::Info, entryRef(EntryKind::Token, t.base.id),
                "tokens.yaml describes '--" + t.base.id + "', which " +
                    (project.shared.tokens.empty() ? std::string("no stylesheet") : project.shared.tokens) +
                    " does not declare"});
        }
    }
    return Status::kOk;
}

Status upgradeDesign(const UiProject& project, DesignUpgradeReport* report, std::string* error) {
    setError(error, std::string());
    if (project.isUnnamed()) {
        setError(error, "an unnamed UI Project has no design directory");
        return Status::kInvalidArgument;
    }
    DesignUpgradeReport local;
    const fs::path projectRoot = pathFromUtf8(project.rootDir);
    const fs::path root = pathFromUtf8(designDir(project));
    const std::string label = project.shared.design.empty() ? std::string("design") : project.shared.design;
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        fs::create_directories(root / "text", ec);
        fs::create_directories(root / "references", ec);
        if (ec) {
            setError(error, "cannot create '" + genericPathToUtf8(root) + "'");
            return Status::kIoError;
        }
        DesignInfo info;
        info.name = project.name.empty() ? project.id : project.name;
        info.status = "draft";
        if (!writeText(root / "design.yaml", bundle::detail::emitYamlLite(infoJson(info)), error) ||
            !writeText(root / "text" / "principles.md", std::string(kPrinciplesTemplate), error) ||
            !writeText(root / "text" / "voice.md", std::string(kVoiceTemplate), error) ||
            !writeText(root / "DESIGN.md", std::string(kDesignMdPlaceholder), error)) {
            return Status::kIoError;
        }
        local.created = true;
        local.notes.push_back("created " + label + "/");
    }

    // §35.11: the top-level references/ moves into design/references/ once.
    const fs::path oldRefs = projectRoot / "references";
    const fs::path newRefs = root / "references";
    if (fs::is_directory(oldRefs, ec) && !fs::equivalent(oldRefs, newRefs, ec)) {
        std::vector<fs::path> files;
        for (fs::recursive_directory_iterator it(oldRefs, ec), end; it != end; it.increment(ec)) {
            if (ec) {
                break;
            }
            if (it->is_regular_file(ec)) {
                files.push_back(it->path());
            }
        }
        std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
            return genericPathToUtf8(a) < genericPathToUtf8(b);
        });
        std::vector<fs::path> moved;
        for (const fs::path& file : files) {
            const std::string relative = relativeGeneric(file, oldRefs);
            if (pathToUtf8(file.filename()) == ".gitkeep") {
                fs::remove(file, ec);
                continue;
            }
            const fs::path target = newRefs / pathFromUtf8(relative);
            if (fs::exists(target, ec)) {
                local.notes.push_back("kept references/" + relative + ": " + label + "/references/" +
                                      relative + " already exists");
                continue;
            }
            if (!moveOne(file, target)) {
                setError(error, "cannot move '" + genericPathToUtf8(file) + "' to '" +
                                    genericPathToUtf8(target) + "'");
                return Status::kIoError;
            }
            local.notes.push_back("moved references/" + relative + " to " + label + "/references/" +
                                  relative);
            const std::string name = pathToUtf8(target.filename());
            if (name.size() < 5 || name.compare(name.size() - 5, 5, ".meta") != 0) {
                moved.push_back(target);
                ++local.referencesMoved;
            }
        }
        for (const fs::path& target : moved) {
            fs::path metaPath = target;
            metaPath += ".meta";
            if (fs::exists(metaPath, ec)) {
                continue;
            }
            std::string bytes;
            bundle::detail::readFile(pathToUtf8(target), bytes);
            ReferenceMeta meta;
            meta.base.kind = EntryKind::Reference;
            meta.guid = detail::newGuid();
            meta.path = relativeGeneric(target, root);
            meta.title = pathToUtf8(target.filename());
            meta.license = "unknown";
            meta.contentHash = detail::contentHash(bytes);
            if (!writeText(metaPath, referenceMetaContent(meta), error)) {
                return Status::kIoError;
            }
            local.notes.push_back("added " + label + "/" + meta.path + ".meta");
        }
        removeEmptyDirs(oldRefs);
    }

    DesignSystem ds;
    const Status loaded = load(project, ds, nullptr, error);
    if (loaded != Status::kOk) {
        return loaded;
    }
    const size_t before = ds.tokens.size();
    syncTokenMeta(ds, project, nullptr);
    local.tokensMetaAdded = static_cast<int>(ds.tokens.size() - before);
    if (local.tokensMetaAdded > 0) {
        if (!writeText(root / "tokens.yaml", bundle::detail::emitYamlLite(tokensJson(ds)), error)) {
            return Status::kIoError;
        }
        local.notes.push_back("added draft metadata for " + std::to_string(local.tokensMetaAdded) +
                              " variable(s) to " + label + "/tokens.yaml");
    }
    if (report != nullptr) {
        *report = std::move(local);
    }
    return Status::kOk;
}

}  // namespace xt::bundle::design
