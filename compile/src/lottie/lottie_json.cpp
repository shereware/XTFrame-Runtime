#include <json.hpp>
#include <string>

#include "lottie_model.h"
#include "lottie_shapes.h"

/// JSON -> LottieDoc. See docs/design/11-lottie-mapping.md §11.1-§11.3.1, docs/tasks/T-14.md "JSON
/// → 模型規則". Shape layer contents (`shapes[]`) are delegated to lottie_shapes_json.cpp (T-15);
/// the scalar/AnimatedValue JSON helpers (jnum/jfield/.../parseAnimated/parseHexColor) live in
/// lottie_model.h so lottie_shapes_json.cpp can reuse them without duplication.

namespace xt::compile {

namespace {

using nlohmann::json;

LottieTransform parseTransform(const json* ks) {
    LottieTransform t;
    t.anchor.value = Vec2{0, 0};
    t.position.value = Vec2{0, 0};
    t.scale.value = Vec2{100, 100};
    t.rotation.value = 0;
    t.opacity.value = 100;
    t.skew.value = 0;
    t.skewAxis.value = 0;
    if (ks == nullptr || !ks->is_object()) {
        return t;
    }
    if (ks->contains("a")) {
        t.anchor = parseAnimated<Vec2>(ks->at("a"));
        t.anchorZ = parseAnimatedComponent(ks->at("a"), 2);
    }
    if (ks->contains("r")) {
        t.rotation = parseAnimated<float>(ks->at("r"));
    }
    if (ks->contains("rz")) {
        // T-47b: `rz` is a 3D layer's alias for `r` (both are rotation around the local Z axis,
        // docs/design/19-3d-space.md §19.2); when both are present, `rz` wins.
        t.rotation = parseAnimated<float>(ks->at("rz"));
    }
    if (ks->contains("rx")) {
        t.rx = parseAnimated<float>(ks->at("rx"));
    }
    if (ks->contains("ry")) {
        t.ry = parseAnimated<float>(ks->at("ry"));
    }
    if (ks->contains("or")) {
        t.orientation = parseAnimated<Vec3>(ks->at("or"));
    }
    if (ks->contains("o")) {
        t.opacity = parseAnimated<float>(ks->at("o"));
    }
    if (ks->contains("sk")) {
        t.skew = parseAnimated<float>(ks->at("sk"));
    }
    if (ks->contains("sa")) {
        t.skewAxis = parseAnimated<float>(ks->at("sa"));
    }
    if (ks->contains("s")) {
        t.scale = parseAnimated<Vec2>(ks->at("s"));
    }
    if (ks->contains("p")) {
        const json& p = ks->at("p");
        if (bfield(p, "s", false)) {
            t.splitPosition = true;
            t.posX.value = 0;
            t.posY.value = 0;
            if (p.is_object() && p.contains("x")) {
                t.posX = parseAnimated<float>(p.at("x"));
            }
            if (p.is_object() && p.contains("y")) {
                t.posY = parseAnimated<float>(p.at("y"));
            }
            if (p.is_object() && p.contains("z")) {
                t.posZ = parseAnimated<float>(p.at("z"));
            }
        } else {
            t.position = parseAnimated<Vec2>(p);
            t.position3 = parseAnimated<Vec3>(p);
        }
    }
    return t;
}

void parseLayers(const json& arr, std::vector<LottieLayer>& out, const std::string& file,
                 DiagnosticSink& sink, ShapeWarnState& shapeWarnState) {
    if (!arr.is_array()) {
        return;
    }
    for (const json& lj : arr) {
        if (!lj.is_object()) {
            continue;
        }
        int ty = ifield(lj, "ty", -1);
        std::string name = sfield(lj, "nm", std::string());
        int ind = ifield(lj, "ind", -1);
        if (ty == 5) {
            sink.warn(file, "layer \"" + name + "\" (index " + std::to_string(ind) +
                                "): text layers are not supported; skipped");
            continue;
        }
        if (ty != 0 && ty != 1 && ty != 2 && ty != 3 && ty != 4 && ty != 13) {
            sink.warn(file, "layer \"" + name + "\" (index " + std::to_string(ind) +
                                "): unsupported layer type " + std::to_string(ty) + "; skipped");
            continue;
        }

        LottieLayer layer;
        layer.type = ty;
        layer.index = ind;
        layer.parent = ifield(lj, "parent", -1);
        layer.name = name;
        layer.ip = jfield(lj, "ip", 0.0f);
        layer.op = jfield(lj, "op", 0.0f);
        layer.st = jfield(lj, "st", 0.0f);
        layer.sr = jfield(lj, "sr", 1.0f);
        layer.hidden = bfield(lj, "hd", false);
        layer.blendMode = ifield(lj, "bm", 0);
        layer.matteType = ifield(lj, "tt", 0);
        layer.matteParent = ifield(lj, "tp", -1);
        layer.td = ifield(lj, "td", 0);

        layer.ddd = bfield(lj, "ddd", false);         // T-47b: 3D layers are now supported.
        layer.autoOrient = ifield(lj, "ao", 0) == 1;  // T-228.

        if (lj.contains("masksProperties") && lj.at("masksProperties").is_array()) {
            for (const json& mj : lj.at("masksProperties")) {
                if (!mj.is_object()) {
                    continue;
                }
                LottieMask mask;
                std::string modeStr = sfield(mj, "mode", "n");
                mask.mode = modeStr.empty() ? 'n' : static_cast<int>(modeStr[0]);
                mask.inverted = bfield(mj, "inv", false);
                if (mj.contains("pt")) {
                    mask.pt = parseAnimated<LottiePath>(mj.at("pt"));
                }
                mask.o.value = 100.0f;
                if (mj.contains("o")) {
                    mask.o = parseAnimated<float>(mj.at("o"));
                }
                if (mj.contains("x")) {
                    mask.x = parseAnimated<float>(mj.at("x"));
                }
                layer.masks.push_back(std::move(mask));
            }
        }

        if (lj.contains("ks") && lj.at("ks").is_object()) {
            const json& ks = lj.at("ks");
            layer.ks = parseTransform(&ks);
        } else {
            layer.ks = parseTransform(nullptr);
        }

        // ef[] (T-55, docs/design/18-effects.md §18.9): each entry is an effect group with its own
        // "ty" and a nested "ef[]" of sub-parameters, each `{ty, nm, v}` with `v` an ordinary
        // Lottie property object. Only ty:29 (Gaussian Blur) and ty:25 (Drop Shadow) have their
        // sub-parameters parsed here; other ty values are kept (ty only) so buildLayerEffects can
        // warn about them in layer/ef[] order.
        if (lj.contains("ef") && lj.at("ef").is_array()) {
            for (const json& ej : lj.at("ef")) {
                if (!ej.is_object()) {
                    continue;
                }
                LottieEffect eff;
                eff.ty = ifield(ej, "ty", -1);
                const json* subs = (ej.contains("ef") && ej.at("ef").is_array()) ? &ej.at("ef") : nullptr;
                auto subVal = [&](size_t idx) -> const json* {
                    if (subs == nullptr || idx >= subs->size()) {
                        return nullptr;
                    }
                    const json& item = (*subs)[idx];
                    return (item.is_object() && item.contains("v")) ? &item.at("v") : nullptr;
                };
                if (eff.ty == 29) {
                    eff.blurriness.value = 0.0f;
                    if (const json* v = subVal(0)) {
                        eff.blurriness = parseAnimated<float>(*v);
                    }
                    eff.blurDimensions = 1;
                    if (const json* v = subVal(1)) {
                        eff.blurDimensions = static_cast<int>(firstOrStatic(parseAnimated<float>(*v)));
                    }
                } else if (eff.ty == 25) {
                    eff.shadowColor.value = Color{0, 0, 0, 1};
                    if (const json* v = subVal(0)) {
                        eff.shadowColor = parseAnimated<Color>(*v);
                    }
                    eff.shadowOpacity.value = 255.0f;
                    if (const json* v = subVal(1)) {
                        eff.shadowOpacity = parseAnimated<float>(*v);
                    }
                    eff.shadowDirection.value = 0.0f;
                    if (const json* v = subVal(2)) {
                        eff.shadowDirection = parseAnimated<float>(*v);
                    }
                    eff.shadowDistance.value = 0.0f;
                    if (const json* v = subVal(3)) {
                        eff.shadowDistance = parseAnimated<float>(*v);
                    }
                    eff.shadowSoftness.value = 0.0f;
                    if (const json* v = subVal(4)) {
                        eff.shadowSoftness = parseAnimated<float>(*v);
                    }
                }
                layer.effects.push_back(std::move(eff));
            }
        }

        if (ty == 1) {
            layer.solidW = ifield(lj, "sw", 0);
            layer.solidH = ifield(lj, "sh", 0);
            std::string sc = sfield(lj, "sc", std::string());
            layer.solidColor = sc.empty() ? Color{0, 0, 0, 1} : parseHexColor(sc);
        } else if (ty == 0) {
            layer.refId = sfield(lj, "refId", std::string());
            layer.w = jfield(lj, "w", 0.0f);
            layer.h = jfield(lj, "h", 0.0f);
            if (lj.contains("tm")) {
                sink.warn(file, lottieLayerDesc(layer) + ": time remap (\"tm\") is not supported; ignored");
            }
        } else if (ty == 2) {
            layer.refId = sfield(lj, "refId", std::string());
        } else if (ty == 4) {
            if (lj.contains("shapes") && lj.at("shapes").is_array()) {
                layer.shapes = parseLottieShapeItems(lj.at("shapes"), file, sink, shapeWarnState);
            }
        } else if (ty == 13) {
            // T-47b: camera layer. `pe` (zoom) is top-level, not part of `ks`.
            if (lj.contains("pe")) {
                layer.cameraZoom = parseAnimated<float>(lj.at("pe"));
            }
            if (lj.contains("ks") && lj.at("ks").is_object() && lj.at("ks").contains("a")) {
                layer.cameraHasTarget = true;
                layer.cameraPointOfInterest = parseAnimated<Vec3>(lj.at("ks").at("a"));
            }
        }

        out.push_back(std::move(layer));
    }
}

void parseVersion(const std::string& v, int (&out)[3]) {
    out[0] = out[1] = out[2] = 0;
    size_t pos = 0;
    for (int i = 0; i < 3 && pos <= v.size(); ++i) {
        size_t dot = v.find('.', pos);
        std::string part = v.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);
        try {
            out[i] = std::stoi(part);
        } catch (...) {
            out[i] = 0;
        }
        if (dot == std::string::npos) {
            break;
        }
        pos = dot + 1;
    }
}

bool requireNumber(const json& root, const char* key, float& out, DiagnosticSink& sink,
                   const std::string& file) {
    if (!root.contains(key) || !root.at(key).is_number()) {
        sink.error(file, std::string("missing \"") + key + "\" field");
        return false;
    }
    out = static_cast<float>(root.at(key).get<double>());
    return true;
}

}  // namespace

bool parseLottieJson(std::span<const uint8_t> jsonBytes, const std::string& file, LottieDoc& out,
                     DiagnosticSink& sink) {
    json root;
    try {
        root = json::parse(jsonBytes.begin(), jsonBytes.end());
    } catch (const std::exception& e) {
        sink.error(file, std::string("invalid JSON: ") + e.what());
        return false;
    }
    if (!root.is_object()) {
        sink.error(file, "top-level JSON value must be an object");
        return false;
    }

    if (!root.contains("v") || !root.at("v").is_string()) {
        sink.warn(file, "missing \"v\" field; assuming a compatible version");
    } else {
        std::string v = root.at("v").get<std::string>();
        parseVersion(v, out.version);
        if (out.version[0] < 5 || (out.version[0] == 5 && out.version[1] < 5)) {
            sink.warn(file, "lottie version " + v + " is older than 5.5; attempting to parse anyway");
        }
    }

    bool ok = true;
    ok = requireNumber(root, "fr", out.fr, sink, file) && ok;
    ok = requireNumber(root, "ip", out.ip, sink, file) && ok;
    ok = requireNumber(root, "op", out.op, sink, file) && ok;
    ok = requireNumber(root, "w", out.w, sink, file) && ok;
    ok = requireNumber(root, "h", out.h, sink, file) && ok;
    if (!ok) {
        return false;
    }

    ShapeWarnState shapeWarnState;

    if (root.contains("assets") && root.at("assets").is_array()) {
        for (const json& a : root.at("assets")) {
            if (!a.is_object() || !a.contains("id")) {
                continue;
            }
            LottieAsset asset;
            asset.id = sfield(a, "id", std::string());
            if (a.contains("layers")) {
                parseLayers(a.at("layers"), asset.layers, file, sink, shapeWarnState);
            } else if (a.contains("p")) {
                // T-16: image asset (has `p`, no `layers`).
                asset.isImage = true;
                asset.w = ifield(a, "w", 0);
                asset.h = ifield(a, "h", 0);
                asset.u = sfield(a, "u", std::string());
                asset.p = sfield(a, "p", std::string());
                asset.embed = bfield(a, "e", false);
            }
            std::string id = asset.id;
            out.assets[id] = std::move(asset);
        }
    }

    if (root.contains("markers") && root.at("markers").is_array()) {
        for (const json& m : root.at("markers")) {
            if (!m.is_object()) {
                continue;
            }
            LottieMarker marker;
            marker.name = sfield(m, "cm", std::string());
            marker.time = jfield(m, "tm", 0.0f);
            marker.duration = jfield(m, "dr", 0.0f);
            out.markers.push_back(std::move(marker));
        }
    }

    if (root.contains("layers")) {
        parseLayers(root.at("layers"), out.layers, file, sink, shapeWarnState);
    }

    // T-257: shape groups nested past kMaxShapeGroupDepth were reported and left unread.
    return !shapeWarnState.groupsTooDeep;
}

}  // namespace xt::compile
