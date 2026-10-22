#pragma once

#include <json.hpp>
#include <string>
#include <vector>

#include "lottie_model.h"
#include "xt/compile/diagnostics.h"
#include "xt/types.h"

/// Intermediate model for a Lottie shape layer's `shapes[]` tree (Shape layer, `ty: 4`). Parsed
/// from JSON by lottie_shapes_json.cpp, mapped to SceneBuilder/TimelineBuilder nodes by
/// lottie_shapes_build.cpp. See Docs/design/11-lottie-mapping.md §11.3.2-11.3.3, Docs/tasks/T-15.md.
///
/// This header is included both standalone (by lottie_geometry.h) and via lottie_model.h (which
/// includes it after defining AnimatedValue/LottieTransform, the types this file depends on); the
/// #include "lottie_model.h" below is a no-op in that case thanks to #pragma once.

namespace xt::compile {

/// One cubic-bezier path parsed from a Lottie `sh` shape's `ks` value (or a keyframe's `s[0]`).
/// `v[k]`/`in[k]`/`out[k]` mirror Lottie's `v`/`i`/`o` arrays; `in`/`out` are tangents relative to
/// `v[k]` (not absolute points). See Docs/design/11-lottie-mapping.md §11.3.2.
struct LottiePath {
    std::vector<Vec2> v;
    std::vector<Vec2> in;
    std::vector<Vec2> out;
    bool closed = false;
};

/// Reads a Lottie path value node: either the bare path object (`{i,o,v,c}`, static `a==0` case) or
/// an array wrapping exactly that object (a keyframe's `s`/`e`, `a==1` case; only element 0 is used
/// per Docs/design/11-lottie-mapping.md §11.4). Declared here (rather than lottie_model.h) because it
/// needs LottiePath complete; the primary `readValue<T>` template is declared earlier in
/// lottie_model.h, which this specialization is visible to since lottie_model.h includes this file
/// after that declaration.
template <>
inline LottiePath readValue<LottiePath>(const nlohmann::json& jIn) {
    const nlohmann::json* obj = &jIn;
    if (jIn.is_array()) {
        if (jIn.empty() || !jIn[0].is_object()) {
            return LottiePath{};
        }
        obj = &jIn[0];
    } else if (!jIn.is_object()) {
        return LottiePath{};
    }
    LottiePath path;
    path.closed = bfield(*obj, "c", false);
    if (obj->contains("v") && (*obj)["v"].is_array()) {
        for (const nlohmann::json& p : (*obj)["v"]) {
            path.v.push_back(Vec2{arrComp(p, 0), arrComp(p, 1)});
        }
    }
    if (obj->contains("i") && (*obj)["i"].is_array()) {
        for (const nlohmann::json& p : (*obj)["i"]) {
            path.in.push_back(Vec2{arrComp(p, 0), arrComp(p, 1)});
        }
    }
    if (obj->contains("o") && (*obj)["o"].is_array()) {
        for (const nlohmann::json& p : (*obj)["o"]) {
            path.out.push_back(Vec2{arrComp(p, 0), arrComp(p, 1)});
        }
    }
    // Malformed input (mismatched array lengths): pad the shorter arrays with zero tangents/points
    // rather than reading out of bounds; not covered by the spec, chosen as the conservative option.
    size_t n = path.v.size();
    path.in.resize(n, Vec2{0, 0});
    path.out.resize(n, Vec2{0, 0});
    return path;
}

/// One entry of a stroke's dash array (`st.d[]`). `n` selects which role this entry plays.
struct LottieDash {
    enum class Kind : uint8_t { Offset, Dash, Gap } kind = Kind::Dash;
    AnimatedValue<float> value;
};

/// A Lottie transform object (`tr`), used both for a shape group's own transform and for a
/// repeater's `tr` sub-object (which additionally carries `so`/`eo`).
struct LottieShapeTransform {
    LottieTransform base;
    // "so"/"eo" (repeater only); default 100 (fully opaque) when the repeater omits them.
    AnimatedValue<float> startOpacity;
    AnimatedValue<float> endOpacity;
};

/// One item of a Lottie shape group's `it[]` array (or a layer's top-level `shapes[]`).
/// Fields not applicable to `kind` are left at their defaults. See Docs/tasks/T-15.md "Shape 模型".
struct LottieShapeItem {
    enum class Kind : uint8_t {
        Group,
        Rect,
        Ellipse,
        Star,
        Path,
        Fill,
        Stroke,
        GradientFill,
        GradientStroke,
        Transform,
        Trim,
        RoundCorners,
        Repeater,
        // T-230 (31 §31.9): the remaining AE path operators, kept as items so both the `<anim>`
        // builder and the native import can turn them into modifier nodes / elements.
        PuckerBloat,
        ZigZag,
        Twist,
        OffsetPath,
        MergePaths,
        Unsupported
    };
    Kind kind = Kind::Unsupported;
    std::string name;
    bool hidden = false;

    // Rect ("rc") / Ellipse ("el"): position + size. Rect also uses `radius` ("r").
    AnimatedValue<Vec2> position;  // "p"
    AnimatedValue<Vec2> size;      // "s"
    AnimatedValue<float> radius;   // Rect "r" (corner radius); RoundCorners "r"
    int direction = 1;             // "d": 3 = reversed winding, else forward.

    // Star ("sr").
    AnimatedValue<float> points;          // "pt"
    AnimatedValue<float> innerRadius;     // "ir"
    AnimatedValue<float> outerRadius;     // "or"
    AnimatedValue<float> innerRoundness;  // "is"
    AnimatedValue<float> outerRoundness;  // "os"
    AnimatedValue<float> rotation;        // "r" (Star only; Rect/RoundCorners use `radius` for "r")
    int starType = 1;                     // "sy": 1 star, 2 polygon.

    // Path ("sh").
    AnimatedValue<LottiePath> path;  // "ks"

    // Fill ("fl") / Stroke ("st") / GradientFill ("gf") / GradientStroke ("gs"): common paint.
    AnimatedValue<Color> color;    // "c" (Fill/Stroke solid color)
    AnimatedValue<float> opacity;  // "o", Lottie units 0..100
    int fillRule = 1;              // Fill "r": 1 NonZero, 2 EvenOdd.

    // Stroke / GradientStroke.
    AnimatedValue<float> width;       // "w"
    int lineCap = 2;                  // "lc": 1 Butt, 2 Round, 3 Square.
    int lineJoin = 1;                 // "lj": 1 Miter, 2 Round, 3 Bevel.
    AnimatedValue<float> miterLimit;  // "ml"
    std::vector<LottieDash> dashes;   // "d"

    // GradientFill / GradientStroke.
    int gradientKind = 1;                     // "t": 1 linear, 2 radial.
    AnimatedValue<Vec2> gradientStart;        // "s"
    AnimatedValue<Vec2> gradientEnd;          // "e"
    AnimatedValue<float> highlightLength;     // "h"
    AnimatedValue<float> highlightAngle;      // "a"
    int stopCount = 0;                        // "g.p": number of color stops.
    AnimatedValue<std::vector<float>> stops;  // "g.k": flattened color+alpha stop data.

    // Transform ("tr"): a group's own transform item.
    LottieShapeTransform transform;

    // Trim ("tm").
    AnimatedValue<float> trimStart;   // "s", Lottie units 0..100
    AnimatedValue<float> trimEnd;     // "e", Lottie units 0..100
    AnimatedValue<float> trimOffset;  // "o", Lottie units (percent of a full loop)
    int trimMode = 1;                 // "m": 1 simultaneous (parallel), 2 individually (serial).

    // Repeater ("rp").
    AnimatedValue<float> copies;             // "c"
    AnimatedValue<float> offset;             // "o"
    LottieShapeTransform repeaterTransform;  // "tr"
    int repeaterComposite = 1;               // "m": 1 above, 2 below.

    // T-230: PuckerBloat ("pb" a), ZigZag ("zz" s size, r ridges, pt 1 corner / 2 smooth),
    // Twist ("tw" a angle, c center), OffsetPath ("op" a amount, lj join, ml miter limit),
    // MergePaths ("mm" mm mode 1 merge .. 5 exclude intersections).
    AnimatedValue<float> amount;        // "pb" a, "op" a, "tw" a
    AnimatedValue<float> zigzagSize;    // "zz" s
    AnimatedValue<float> zigzagRidges;  // "zz" r
    int zigzagPointType = 1;            // "zz" pt
    AnimatedValue<Vec2> twistCenter;    // "tw" c
    AnimatedValue<float> offsetMiter;   // "op" ml
    int offsetJoin = 1;                 // "op" lj: 1 miter, 2 round, 3 bevel
    int mergeMode = 1;                  // "mm" mm

    // Group ("gr").
    std::vector<LottieShapeItem> items;  // "it"
};

/// Parses one `it`/`shapes` JSON array into a shape item list. `hd:true` items are skipped
/// entirely. An unrecognized `ty` becomes `Kind::Unsupported` and is warned about. T-230: `pb`/`zz`/
/// `tw`/`op`/`mm` are parsed into their own kinds; whether they are drawn is the builder's choice
/// (the `warned*` flags below are no longer set by the parser). T-257: groups nest at most
/// kMaxShapeGroupDepth deep; a deeper one is an error (reported once, `groupsTooDeep` set) and its
/// content is not read, and parseLottieJson() then fails.
constexpr int kMaxShapeGroupDepth = 64;

struct ShapeWarnState {
    bool warnedMergePaths = false;
    bool warnedPuckerBloat = false;
    bool warnedZigZag = false;
    bool warnedOffsetPath = false;
    bool groupsTooDeep = false;
};

std::vector<LottieShapeItem> parseLottieShapeItems(const nlohmann::json& arr, const std::string& file,
                                                   DiagnosticSink& sink, ShapeWarnState& warnState);

}  // namespace xt::compile
