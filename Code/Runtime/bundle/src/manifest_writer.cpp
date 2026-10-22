#include "manifest_writer.h"

#include <charconv>
#include <cstdlib>
#include <json.hpp>
#include <utility>

#include "bundle_json.h"
#include "xt/charconv.h"

/// See manifest_writer.h. The writer mirrors ProjectFile::save(): nlohmann's ordered_json so the
/// key order is §22.3's, dump(2) plus a trailing '\n', and detail::writeFile so no CRLF
/// translation happens. The reader only exists so an incremental run (`--ui`) can keep the
/// entries it is not re-packing.

namespace xt::bundle {

using detail::getBool;
using detail::getString;
using detail::getStringArray;
using detail::getUint64;
using detail::getVec2;
using detail::joinPath;
using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

namespace {

constexpr const char* kManifestFileNameManifestWriter = "manifest.json";

/// `[x, y]`, written as integers when the values are whole (a design size normally is).
ordered_json writeVec2ManifestWriter(Vec2 value) {
    ordered_json array = ordered_json::array();
    for (float component : {value.x, value.y}) {
        const auto rounded = static_cast<int64_t>(component);
        if (static_cast<float>(rounded) == component) {
            array.push_back(rounded);
        } else {
            array.push_back(component);
        }
    }
    return array;
}

/// One `ports.in`/`ports.out` array. `params` is optional and 0.3 never produces it (§22.3).
ordered_json writePorts(const std::vector<PortInfo>& ports, bool isInput) {
    ordered_json array = ordered_json::array();
    for (const PortInfo& port : ports) {
        if (port.isInput != isInput) {
            continue;
        }
        ordered_json entry;
        entry["name"] = port.name;
        entry["type"] = port.type;
        if (!port.params.empty()) {
            ordered_json params;
            for (const auto& [key, value] : port.params) {
                params[key] = value;
            }
            entry["params"] = std::move(params);
        }
        array.push_back(std::move(entry));
    }
    return array;
}

/// Reads one `ports.in`/`ports.out` array; the mirror of writePorts().
void readPortsManifestWriter(const json& portsObj, const char* key, bool isInput,
                             std::vector<PortInfo>& out) {
    const auto it = portsObj.find(key);
    if (it == portsObj.end() || !it->is_array()) {
        return;
    }
    for (const json& entry : *it) {
        if (!entry.is_object()) {
            continue;
        }
        PortInfo port;
        port.name = getString(entry, "name");
        port.type = getString(entry, "type");
        port.isInput = isInput;
        out.push_back(std::move(port));
    }
}

}  // namespace

Status writeManifest(std::string_view dir, const ManifestData& data) {
    ordered_json root;
    root["version"] = 1;

    ordered_json bundleObj;
    bundleObj["id"] = data.bundleId;
    bundleObj["name"] = data.bundleName;
    bundleObj["builtAt"] = data.builtAt;
    bundleObj["xtframe"] = data.xtframe;
    bundleObj["irMajor"] = data.irMajor;
    root["bundle"] = std::move(bundleObj);

    // T-183 (§22.10): only for bundles packed from a UI Project (.xtui).
    if (data.hasProject) {
        ordered_json projectObj;
        projectObj["name"] = data.project.name;
        projectObj["game"] = data.project.game;
        projectObj["version"] = data.project.version;
        // T-314 (36 §36.4): optional history provenance.
        if (!data.project.commit.empty()) {
            projectObj["commit"] = data.project.commit;
        }
        if (!data.project.checkpoint.empty()) {
            projectObj["checkpoint"] = data.project.checkpoint;
        }
        root["project"] = std::move(projectObj);
    }
    // T-207 (§22.10 `spec`, 17.4.1 item 5): the host's setDevice()/setUiScale() defaults.
    if (data.hasSpec) {
        ordered_json safeArea;
        safeArea["default"] = detail::shortNumber(data.spec.safeAreaDefault);
        safeArea["console"] = detail::shortNumber(data.spec.safeAreaConsole);
        ordered_json specObj;
        if (data.spec.behavior != 0) {
            specObj["behavior"] = data.spec.behavior;  // T-407: written only when set.
        }
        specObj["safeArea"] = std::move(safeArea);
        specObj["logicalHeight"] = detail::shortNumber(data.spec.logicalHeight);
        specObj["uiscaleRule"] = uiScaleRuleName(data.spec.uiscaleRule);  // T-241
        detail::writeUiScaleLimits(specObj, data.spec.uiscaleLimits);     // T-345
        root["spec"] = std::move(specObj);
    }

    ordered_json uiArray = ordered_json::array();
    for (const UiInfo& ui : data.uis) {
        ordered_json entry;
        entry["id"] = ui.id;
        entry["name"] = ui.name;
        entry["file"] = ui.file;
        entry["flow"] = ui.flow;
        entry["thumb"] = ui.thumb;
        entry["size"] = writeVec2ManifestWriter(ui.size);
        entry["tags"] = ui.tags;
        entry["fit"] = ui.fit.empty() ? std::string("native") : ui.fit;

        ordered_json requiresObj;
        requiresObj["effects"] = ui.requiresEffects;
        requiresObj["space3d"] = ui.requiresSpace3d;
        requiresObj["text"] = ui.requiresText;
        entry["requires"] = std::move(requiresObj);

        entry["assets"] = ui.assets;

        ordered_json portsObj;
        portsObj["in"] = writePorts(ui.ports, /*isInput=*/true);
        portsObj["out"] = writePorts(ui.ports, /*isInput=*/false);
        entry["ports"] = std::move(portsObj);

        entry["fileHash"] = ui.fileHash;
        if (!ui.contract.empty()) {
            entry["contract"] = ui.contract;  // T-408 (43 §43.6)
        }
        uiArray.push_back(std::move(entry));
    }
    root["uis"] = std::move(uiArray);

    ordered_json assetArray = ordered_json::array();
    for (const ManifestAsset& asset : data.assets) {
        ordered_json entry;
        entry["file"] = asset.file;
        entry["kind"] = asset.kind;
        entry["bytes"] = asset.bytes;
        entry["sources"] = asset.sources;
        if (asset.hasManifest) {
            ordered_json measured;
            measured["id"] = asset.manifestId;
            if (asset.measured.hasSize) {
                measured["w"] = asset.measured.w;
                measured["h"] = asset.measured.h;
            }
            if (asset.measured.hasAlpha) {
                measured["alpha"] = asset.measured.alpha;
            }
            if (asset.measured.hasSlice) {
                ordered_json slice = ordered_json::array();
                for (int margin : asset.measured.slice) {
                    slice.push_back(margin);
                }
                measured["slice"] = std::move(slice);
            }
            entry["manifest"] = std::move(measured);
        }
        // T-199 (§22.10): present only when set, so an unchanged bundle keeps its old bytes.
        if (!asset.format.empty()) {
            entry["format"] = asset.format;
        }
        if (!asset.variant2x.empty() || !asset.variant3x.empty()) {
            ordered_json variants;
            if (!asset.variant2x.empty()) {
                variants["2x"] = asset.variant2x;
            }
            if (!asset.variant3x.empty()) {
                variants["3x"] = asset.variant3x;  // T-345
            }
            entry["variants"] = std::move(variants);
        }
        if (!asset.atlases.empty()) {
            entry["atlas"] = asset.atlases;
        }
        assetArray.push_back(std::move(entry));
    }
    root["assets"] = std::move(assetArray);
    if (!data.atlases.empty()) {
        ordered_json atlasArray = ordered_json::array();
        for (const ManifestAtlas& atlas : data.atlases) {
            ordered_json entry;
            entry["name"] = atlas.name;
            entry["file"] = atlas.file;
            if (!atlas.file2x.empty()) {
                ordered_json variants;
                variants["2x"] = atlas.file2x;
                entry["variants"] = std::move(variants);
            }
            if (!atlas.format.empty()) {
                entry["format"] = atlas.format;
            }
            entry["w"] = atlas.w;
            entry["h"] = atlas.h;
            ordered_json entries = ordered_json::array();
            for (const ManifestAtlasEntry& e : atlas.entries) {
                ordered_json row;
                row["asset"] = e.asset;
                row["x"] = e.x;
                row["y"] = e.y;
                row["w"] = e.w;
                row["h"] = e.h;
                row["rotated"] = false;
                entries.push_back(std::move(row));
            }
            entry["entries"] = std::move(entries);
            atlasArray.push_back(std::move(entry));
        }
        root["atlas"] = std::move(atlasArray);
    }

    // T-170 (§22.10): only written when present, so a bundle without tokens keeps its old bytes.
    if (!data.tokens.empty()) {
        ordered_json tokens = ordered_json::object();
        for (const TokenDef& token : data.tokens) {
            tokens[token.name] = token.value;
        }
        root["tokens"] = std::move(tokens);
    }
    if (!data.themes.empty()) {
        ordered_json themes = ordered_json::array();
        for (const ThemeInfo& theme : data.themes) {
            ordered_json entry;
            entry["name"] = theme.name;
            entry["file"] = theme.file;
            themes.push_back(std::move(entry));
        }
        root["themes"] = std::move(themes);
    }

    // T-179 (§22.10): likewise only when present.
    if (!data.strings.empty()) {
        ordered_json strings = ordered_json::object();
        for (const StringsInfo& entry : data.strings) {
            strings[entry.lang] = entry.file;
        }
        root["strings"] = std::move(strings);
    }
    if (!data.fontFallbacks.empty()) {
        ordered_json fallback = ordered_json::object();
        for (const FontFallbackInfo& entry : data.fontFallbacks) {
            fallback[entry.lang] = entry.families;
        }
        ordered_json fonts;
        fonts["fallback"] = std::move(fallback);
        root["fonts"] = std::move(fonts);
    }

    // T-204 (§22.10 `glyphs[device]`): only when the project has button glyphs.
    if (!data.glyphs.empty()) {
        ordered_json glyphs = ordered_json::object();
        for (const GlyphInfo& glyph : data.glyphs) {
            glyphs[glyph.device][glyph.key] = glyph.asset;
        }
        root["glyphs"] = std::move(glyphs);
    }

    // T-200 (§22.10 `symbols[]`): the symbol contract summaries, only when there are symbols.
    if (!data.symbols.empty()) {
        ordered_json symbols = ordered_json::array();
        for (const SymbolContract& c : data.symbols) {
            ordered_json entry;
            entry["name"] = c.name;
            entry["source"] = c.source;
            ordered_json req;
            req["states"] = c.requiredStates;
            req["slots"] = c.requiredSlots;
            entry["requires"] = std::move(req);
            ordered_json groups = ordered_json::array();
            for (const SymbolStateGroup& g : c.stateGroups) {
                ordered_json group;
                group["name"] = g.name;
                group["states"] = g.states;
                groups.push_back(std::move(group));
            }
            entry["stateGroups"] = std::move(groups);
            entry["slots"] = c.slotNames;
            ordered_json ports = ordered_json::array();
            for (const SymbolPortInfo& p : c.ports) {
                ordered_json port;
                port["name"] = p.name;
                port["type"] = p.type;
                port["dir"] = p.dir;
                ports.push_back(std::move(port));
            }
            entry["ports"] = std::move(ports);
            symbols.push_back(std::move(entry));
        }
        root["symbols"] = std::move(symbols);
    }

    // T-193 (§22.10): only when the project has a global flow.
    if (!data.globalFlow.empty()) {
        ordered_json flows;
        flows["global"] = data.globalFlow;
        root["flows"] = std::move(flows);
    }

    if (!data.motionSpecJson.empty()) {
        const ordered_json motion =
            ordered_json::parse(data.motionSpecJson, nullptr, /*allow_exceptions=*/false);
        if (!motion.is_discarded() && motion.is_object()) {
            root["motionSpec"] = motion;
        }
    }

    // error_handler_t::replace keeps dump() from throwing on invalid UTF-8 in a caller's strings.
    std::string text = root.dump(2, ' ', false, json::error_handler_t::replace);
    text += '\n';

    if (!detail::writeFile(joinPath(dir, kManifestFileNameManifestWriter), text)) {
        return Status::kIoError;
    }
    return Status::kOk;
}

bool readManifest(std::string_view dir, ManifestData& out) {
    std::string text;
    if (!detail::readFile(joinPath(dir, kManifestFileNameManifestWriter), text)) {
        return false;
    }
    std::string depthError;  // T-262c
    const json root = detail::parseBounded(text, detail::kMaxJsonDepth, depthError);
    if (root.is_discarded() || !root.is_object()) {
        return false;
    }

    ManifestData data;
    const auto bundleIt = root.find("bundle");
    if (bundleIt != root.end() && bundleIt->is_object()) {
        data.bundleId = getString(*bundleIt, "id");
        data.bundleName = getString(*bundleIt, "name");
        data.builtAt = getString(*bundleIt, "builtAt");
        data.xtframe = getString(*bundleIt, "xtframe");
        const auto irMajorIt = bundleIt->find("irMajor");
        if (irMajorIt != bundleIt->end() && irMajorIt->is_number_integer()) {
            data.irMajor = irMajorIt->get<int>();
        }
    }

    const auto uisIt = root.find("uis");
    if (uisIt != root.end() && uisIt->is_array()) {
        for (const json& entry : *uisIt) {
            if (!entry.is_object()) {
                continue;
            }
            UiInfo ui;
            ui.id = getString(entry, "id");
            ui.name = getString(entry, "name");
            ui.file = getString(entry, "file");
            ui.flow = getString(entry, "flow");
            ui.thumb = getString(entry, "thumb");
            ui.size = getVec2(entry, "size", Vec2{0, 0});
            ui.tags = getStringArray(entry, "tags");
            ui.fit = getString(entry, "fit");
            if (ui.fit.empty()) {
                ui.fit = "native";
            }
            const auto requiresIt = entry.find("requires");
            if (requiresIt != entry.end() && requiresIt->is_object()) {
                ui.requiresEffects = getBool(*requiresIt, "effects", false);
                ui.requiresSpace3d = getBool(*requiresIt, "space3d", false);
                ui.requiresText = getBool(*requiresIt, "text", false);
            }
            ui.assets = getStringArray(entry, "assets");
            const auto portsIt = entry.find("ports");
            if (portsIt != entry.end() && portsIt->is_object()) {
                readPortsManifestWriter(*portsIt, "in", true, ui.ports);
                readPortsManifestWriter(*portsIt, "out", false, ui.ports);
            }
            ui.fileHash = getUint64(entry, "fileHash");
            ui.contract = getString(entry, "contract");  // T-408: a kept UI keeps its contract.
            data.uis.push_back(std::move(ui));
        }
    }

    const auto assetsIt = root.find("assets");
    if (assetsIt != root.end() && assetsIt->is_array()) {
        for (const json& entry : *assetsIt) {
            if (!entry.is_object()) {
                continue;
            }
            ManifestAsset asset;
            asset.file = getString(entry, "file");
            asset.kind = getString(entry, "kind");
            asset.bytes = getUint64(entry, "bytes");
            asset.sources = getStringArray(entry, "sources");
            const auto measuredIt = entry.find("manifest");
            if (measuredIt != entry.end() && measuredIt->is_object()) {
                const json& m = *measuredIt;
                asset.hasManifest = true;
                asset.manifestId = getString(m, "id");
                const auto w = m.find("w");
                const auto h = m.find("h");
                if (w != m.end() && h != m.end() && w->is_number_integer() && h->is_number_integer()) {
                    asset.measured.hasSize = true;
                    asset.measured.w = w->get<int>();
                    asset.measured.h = h->get<int>();
                }
                const auto alpha = m.find("alpha");
                if (alpha != m.end() && alpha->is_boolean()) {
                    asset.measured.hasAlpha = true;
                    asset.measured.alpha = alpha->get<bool>();
                }
                const auto slice = m.find("slice");
                if (slice != m.end() && slice->is_array() && slice->size() == 4) {
                    asset.measured.hasSlice = true;
                    for (size_t i = 0; i < 4; ++i) {
                        asset.measured.slice[i] =
                            (*slice)[i].is_number_integer() ? (*slice)[i].get<int>() : 0;
                    }
                }
            }
            asset.format = getString(entry, "format");  // T-199
            const auto variantsIt = entry.find("variants");
            if (variantsIt != entry.end() && variantsIt->is_object()) {
                asset.variant2x = getString(*variantsIt, "2x");
            }
            asset.atlases = getStringArray(entry, "atlas");
            if (!asset.file.empty()) {
                data.assets.push_back(std::move(asset));
            }
        }
    }
    for (detail::AtlasJson& page : detail::getAtlases(root)) {  // T-199
        ManifestAtlas atlas;
        atlas.name = std::move(page.name);
        atlas.file = std::move(page.file);
        atlas.file2x = std::move(page.file2x);
        atlas.format = std::move(page.format);
        atlas.w = page.w;
        atlas.h = page.h;
        for (detail::AtlasEntryJson& e : page.entries) {
            atlas.entries.push_back(ManifestAtlasEntry{std::move(e.asset), e.x, e.y, e.w, e.h});
        }
        data.atlases.push_back(std::move(atlas));
    }

    data.tokens = detail::getTokens(root);
    for (detail::ThemeEntry& theme : detail::getThemes(root)) {
        data.themes.push_back(ThemeInfo{std::move(theme.name), std::move(theme.file)});
    }
    for (auto& [lang, file] : detail::getStringsMap(root)) {
        data.strings.push_back(StringsInfo{lang, file});
    }
    for (auto& [lang, families] : detail::getFontFallbacks(root)) {
        data.fontFallbacks.push_back(FontFallbackInfo{lang, std::move(families)});
    }
    const auto projectIt = root.find("project");
    if (projectIt != root.end() && projectIt->is_object()) {
        data.hasProject = true;
        data.project.name = getString(*projectIt, "name");
        data.project.game = getString(*projectIt, "game");
        data.project.commit = getString(*projectIt, "commit");          // T-314
        data.project.checkpoint = getString(*projectIt, "checkpoint");  // T-314
        const auto versionIt = projectIt->find("version");
        if (versionIt != projectIt->end() && versionIt->is_number_integer()) {
            data.project.version = versionIt->get<int>();
        }
    }
    const auto flowsIt = root.find("flows");
    if (flowsIt != root.end() && flowsIt->is_object()) {
        data.globalFlow = getString(*flowsIt, "global");
    }
    data.symbols = detail::getSymbolContracts(root);  // T-200.
    data.glyphs = detail::getGlyphs(root);            // T-204.
    data.hasSpec = detail::getSpec(root, data.spec);  // T-207.
    const auto motionIt = root.find("motionSpec");
    if (motionIt != root.end() && motionIt->is_object()) {
        data.motionSpecJson = motionIt->dump();
    }
    out = std::move(data);
    return true;
}

}  // namespace xt::bundle
