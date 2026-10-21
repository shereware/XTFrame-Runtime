#include <json.hpp>
#include <string>

#include "lottie_model.h"
#include "lottie_shapes.h"

/// JSON -> LottieShapeItem tree. See docs/design/11-lottie-mapping.md §11.3.2-11.3.3,
/// docs/tasks/T-15.md "Shape 模型". Reuses lottie_model.h's AnimatedValue JSON helpers
/// (jfield/ifield/.../parseAnimated) rather than duplicating them.

namespace xt::compile {

namespace {

/// Parses a stroke / offset-path `ml` (miter limit). Exporters write it either as a plain number
/// (the common case) or as a `{a, k}` property; a missing or unreadable value keeps 4.
AnimatedValue<float> parseMiterLimit(const nlohmann::json& sj) {
    AnimatedValue<float> out;
    out.value = 4;
    if (!sj.contains("ml")) {
        return out;
    }
    const nlohmann::json& ml = sj.at("ml");
    if (ml.is_number()) {
        out.value = ml.get<float>();
        return out;
    }
    if (ml.is_object()) {
        return parseAnimated<float>(ml);
    }
    return out;
}

using nlohmann::json;

LottieShapeTransform parseShapeTransformObject(const json* tr) {
    LottieShapeTransform out;
    out.base.anchor.value = Vec2{0, 0};
    out.base.position.value = Vec2{0, 0};
    out.base.scale.value = Vec2{100, 100};
    out.base.rotation.value = 0;
    out.base.opacity.value = 100;
    out.base.skew.value = 0;
    out.base.skewAxis.value = 0;
    out.startOpacity.value = 100;
    out.endOpacity.value = 100;
    if (tr == nullptr || !tr->is_object()) {
        return out;
    }
    if (tr->contains("a")) {
        out.base.anchor = parseAnimated<Vec2>(tr->at("a"));
    }
    if (tr->contains("s")) {
        out.base.scale = parseAnimated<Vec2>(tr->at("s"));
    }
    if (tr->contains("r")) {
        out.base.rotation = parseAnimated<float>(tr->at("r"));
    }
    if (tr->contains("o")) {
        out.base.opacity = parseAnimated<float>(tr->at("o"));
    }
    if (tr->contains("sk")) {
        out.base.skew = parseAnimated<float>(tr->at("sk"));
    }
    if (tr->contains("sa")) {
        out.base.skewAxis = parseAnimated<float>(tr->at("sa"));
    }
    if (tr->contains("p")) {
        const json& p = tr->at("p");
        if (bfield(p, "s", false)) {
            out.base.splitPosition = true;
            if (p.is_object() && p.contains("x")) {
                out.base.posX = parseAnimated<float>(p.at("x"));
            }
            if (p.is_object() && p.contains("y")) {
                out.base.posY = parseAnimated<float>(p.at("y"));
            }
        } else {
            out.base.position = parseAnimated<Vec2>(p);
        }
    }
    if (tr->contains("so")) {
        out.startOpacity = parseAnimated<float>(tr->at("so"));
    }
    if (tr->contains("eo")) {
        out.endOpacity = parseAnimated<float>(tr->at("eo"));
    }
    return out;
}

/// The first key's (or the static) value, for `pt` written as a property object.
float firstOrStaticValue(const AnimatedValue<float>& av) {
    return av.animated && !av.keys.empty() ? av.keys.front().value : av.value;
}

void parseDashes(const json& arr, std::vector<LottieDash>& out) {
    if (!arr.is_array()) {
        return;
    }
    for (const json& dj : arr) {
        if (!dj.is_object()) {
            continue;
        }
        LottieDash dash;
        std::string n = sfield(dj, "n", "d");
        if (n == "o") {
            dash.kind = LottieDash::Kind::Offset;
        } else if (n == "g") {
            dash.kind = LottieDash::Kind::Gap;
        } else {
            dash.kind = LottieDash::Kind::Dash;
        }
        if (dj.contains("v")) {
            dash.value = parseAnimated<float>(dj.at("v"));
        }
        out.push_back(std::move(dash));
    }
}

std::vector<LottieShapeItem> parseItems(const json& arr, const std::string& file, DiagnosticSink& sink,
                                        ShapeWarnState& warnState, int depth);

/// `depth` is the group nesting of the list `sj` sits in (0 for a layer's `shapes`).
LottieShapeItem parseOneItem(const json& sj, const std::string& file, DiagnosticSink& sink,
                             ShapeWarnState& warnState, int depth, bool& skip) {
    LottieShapeItem item;
    skip = false;
    std::string ty = sfield(sj, "ty", std::string());
    item.name = sfield(sj, "nm", std::string());
    item.hidden = bfield(sj, "hd", false);
    if (item.hidden) {
        skip = true;
        return item;
    }

    if (ty == "gr") {
        item.kind = LottieShapeItem::Kind::Group;
        if (sj.contains("it") && sj.at("it").is_array()) {
            if (depth + 1 > kMaxShapeGroupDepth) {
                // T-257: never descend further; the import fails on this error.
                if (!warnState.groupsTooDeep) {
                    warnState.groupsTooDeep = true;
                    sink.error(file, "shape group nesting too deep (max " +
                                         std::to_string(kMaxShapeGroupDepth) + ")");
                }
            } else {
                item.items = parseItems(sj.at("it"), file, sink, warnState, depth + 1);
            }
        }
    } else if (ty == "rc") {
        item.kind = LottieShapeItem::Kind::Rect;
        if (sj.contains("p")) {
            item.position = parseAnimated<Vec2>(sj.at("p"));
        }
        if (sj.contains("s")) {
            item.size = parseAnimated<Vec2>(sj.at("s"));
        }
        if (sj.contains("r")) {
            item.radius = parseAnimated<float>(sj.at("r"));
        }
        item.direction = ifield(sj, "d", 1);
    } else if (ty == "el") {
        item.kind = LottieShapeItem::Kind::Ellipse;
        if (sj.contains("p")) {
            item.position = parseAnimated<Vec2>(sj.at("p"));
        }
        if (sj.contains("s")) {
            item.size = parseAnimated<Vec2>(sj.at("s"));
        }
        item.direction = ifield(sj, "d", 1);
    } else if (ty == "sr") {
        item.kind = LottieShapeItem::Kind::Star;
        if (sj.contains("p")) {
            item.position = parseAnimated<Vec2>(sj.at("p"));
        }
        if (sj.contains("pt")) {
            item.points = parseAnimated<float>(sj.at("pt"));
        }
        if (sj.contains("ir")) {
            item.innerRadius = parseAnimated<float>(sj.at("ir"));
        }
        if (sj.contains("or")) {
            item.outerRadius = parseAnimated<float>(sj.at("or"));
        }
        if (sj.contains("is")) {
            item.innerRoundness = parseAnimated<float>(sj.at("is"));
        }
        if (sj.contains("os")) {
            item.outerRoundness = parseAnimated<float>(sj.at("os"));
        }
        if (sj.contains("r")) {
            item.rotation = parseAnimated<float>(sj.at("r"));
        }
        item.starType = ifield(sj, "sy", 1);
        item.direction = ifield(sj, "d", 1);
    } else if (ty == "sh") {
        item.kind = LottieShapeItem::Kind::Path;
        if (sj.contains("ks")) {
            item.path = parseAnimated<LottiePath>(sj.at("ks"));
        }
        item.direction = ifield(sj, "d", 1);
    } else if (ty == "fl") {
        item.kind = LottieShapeItem::Kind::Fill;
        if (sj.contains("c")) {
            item.color = parseAnimated<Color>(sj.at("c"));
        }
        if (sj.contains("o")) {
            item.opacity = parseAnimated<float>(sj.at("o"));
        } else {
            item.opacity.value = 100;
        }
        item.fillRule = ifield(sj, "r", 1);
    } else if (ty == "st") {
        item.kind = LottieShapeItem::Kind::Stroke;
        if (sj.contains("c")) {
            item.color = parseAnimated<Color>(sj.at("c"));
        }
        if (sj.contains("o")) {
            item.opacity = parseAnimated<float>(sj.at("o"));
        } else {
            item.opacity.value = 100;
        }
        if (sj.contains("w")) {
            item.width = parseAnimated<float>(sj.at("w"));
        }
        item.lineCap = ifield(sj, "lc", 2);
        item.lineJoin = ifield(sj, "lj", 1);
        item.miterLimit = parseMiterLimit(sj);
        if (sj.contains("d")) {
            parseDashes(sj.at("d"), item.dashes);
        }
    } else if (ty == "gf" || ty == "gs") {
        item.kind = ty == "gf" ? LottieShapeItem::Kind::GradientFill : LottieShapeItem::Kind::GradientStroke;
        item.gradientKind = ifield(sj, "t", 1);
        if (sj.contains("s")) {
            item.gradientStart = parseAnimated<Vec2>(sj.at("s"));
        }
        if (sj.contains("e")) {
            item.gradientEnd = parseAnimated<Vec2>(sj.at("e"));
        }
        if (sj.contains("h")) {
            item.highlightLength = parseAnimated<float>(sj.at("h"));
        }
        if (sj.contains("a")) {
            item.highlightAngle = parseAnimated<float>(sj.at("a"));
        }
        if (sj.contains("o")) {
            item.opacity = parseAnimated<float>(sj.at("o"));
        } else {
            item.opacity.value = 100;
        }
        if (sj.contains("g") && sj.at("g").is_object()) {
            const json& g = sj.at("g");
            item.stopCount = ifield(g, "p", 0);
            if (g.contains("k")) {
                item.stops = parseAnimated<std::vector<float>>(g.at("k"));
            }
        }
        if (ty == "gs") {
            if (sj.contains("w")) {
                item.width = parseAnimated<float>(sj.at("w"));
            }
            item.lineCap = ifield(sj, "lc", 2);
            item.lineJoin = ifield(sj, "lj", 1);
            item.miterLimit = parseMiterLimit(sj);
            if (sj.contains("d")) {
                parseDashes(sj.at("d"), item.dashes);
            }
        }
    } else if (ty == "tr") {
        item.kind = LottieShapeItem::Kind::Transform;
        item.transform = parseShapeTransformObject(&sj);
    } else if (ty == "tm") {
        item.kind = LottieShapeItem::Kind::Trim;
        if (sj.contains("s")) {
            item.trimStart = parseAnimated<float>(sj.at("s"));
        }
        if (sj.contains("e")) {
            item.trimEnd = parseAnimated<float>(sj.at("e"));
        } else {
            item.trimEnd.value = 100;
        }
        if (sj.contains("o")) {
            item.trimOffset = parseAnimated<float>(sj.at("o"));
        }
        item.trimMode = ifield(sj, "m", 1) == 2 ? 2 : 1;
    } else if (ty == "rd") {
        item.kind = LottieShapeItem::Kind::RoundCorners;
        if (sj.contains("r")) {
            item.radius = parseAnimated<float>(sj.at("r"));
        }
    } else if (ty == "rp") {
        item.kind = LottieShapeItem::Kind::Repeater;
        if (sj.contains("c")) {
            item.copies = parseAnimated<float>(sj.at("c"));
        } else {
            item.copies.value = 3;
        }
        if (sj.contains("o")) {
            item.offset = parseAnimated<float>(sj.at("o"));
        }
        if (sj.contains("tr") && sj.at("tr").is_object()) {
            const json& tr = sj.at("tr");
            item.repeaterTransform = parseShapeTransformObject(&tr);
        } else {
            item.repeaterTransform = parseShapeTransformObject(nullptr);
        }
        item.repeaterComposite = ifield(sj, "m", 1) == 2 ? 2 : 1;
    } else if (ty == "pb") {
        item.kind = LottieShapeItem::Kind::PuckerBloat;
        if (sj.contains("a")) {
            item.amount = parseAnimated<float>(sj.at("a"));
        }
    } else if (ty == "zz") {
        item.kind = LottieShapeItem::Kind::ZigZag;
        if (sj.contains("s")) {
            item.zigzagSize = parseAnimated<float>(sj.at("s"));
        }
        if (sj.contains("r")) {
            item.zigzagRidges = parseAnimated<float>(sj.at("r"));
        }
        item.zigzagPointType = ifield(sj, "pt", 1);
        if (sj.contains("pt") && sj.at("pt").is_object()) {
            item.zigzagPointType = static_cast<int>(firstOrStaticValue(parseAnimated<float>(sj.at("pt"))));
        }
    } else if (ty == "tw") {
        item.kind = LottieShapeItem::Kind::Twist;
        if (sj.contains("a")) {
            item.amount = parseAnimated<float>(sj.at("a"));
        }
        if (sj.contains("c")) {
            item.twistCenter = parseAnimated<Vec2>(sj.at("c"));
        }
    } else if (ty == "op") {
        item.kind = LottieShapeItem::Kind::OffsetPath;
        if (sj.contains("a")) {
            item.amount = parseAnimated<float>(sj.at("a"));
        }
        item.offsetJoin = ifield(sj, "lj", 1);
        item.offsetMiter = parseMiterLimit(sj);
    } else if (ty == "mm") {
        item.kind = LottieShapeItem::Kind::MergePaths;
        item.mergeMode = ifield(sj, "mm", 1);
    } else {
        item.kind = LottieShapeItem::Kind::Unsupported;
        sink.warn(file, "shape \"" + item.name + "\": unsupported shape type \"" + ty + "\"; ignored");
    }
    return item;
}

std::vector<LottieShapeItem> parseItems(const json& arr, const std::string& file, DiagnosticSink& sink,
                                        ShapeWarnState& warnState, int depth) {
    std::vector<LottieShapeItem> out;
    if (!arr.is_array()) {
        return out;
    }
    out.reserve(arr.size());
    for (const json& sj : arr) {
        if (!sj.is_object()) {
            continue;
        }
        bool skip = false;
        LottieShapeItem item = parseOneItem(sj, file, sink, warnState, depth, skip);
        if (skip) {
            continue;
        }
        out.push_back(std::move(item));
    }
    return out;
}

}  // namespace

std::vector<LottieShapeItem> parseLottieShapeItems(const json& arr, const std::string& file,
                                                   DiagnosticSink& sink, ShapeWarnState& warnState) {
    return parseItems(arr, file, sink, warnState, 0);
}

}  // namespace xt::compile
