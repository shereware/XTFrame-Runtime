#pragma once

#include <cstdint>
#include <json.hpp>
#include <map>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "xt/compile/diagnostics.h"
#include "xt/compile/ir_writer.h"
#include "xt/compile/lottie_parser.h"
#include "xt/types.h"

/// Intermediate model for the Lottie parser: JSON is first parsed into this model (lottie_json.cpp),
/// then the model is mapped into a SceneBuilder/TimelineBuilder (lottie_build.cpp). Internal to
/// xt_compile; not installed. See docs/tasks/T-14.md "中間模型".

namespace xt::compile {

/// One sampled keyframe of an animated Lottie property. See docs/design/11-lottie-mapping.md §11.4.
template <class T>
struct LottieKeyframe {
    float frame = 0;
    T value{};
    bool hold = false;
    // Bezier easing control points (out-tangent ox/oy, in-tangent ix/iy), component 0 only.
    float ox = 0, oy = 0, ix = 0, iy = 0;
    // Spatial tangents; only meaningful when hasSpatial is true. The z component is only ever
    // non-zero when T is Vec3 (a 3D layer's position, docs/tasks/T-47b.md); it is always 0 for a
    // plain Vec2 property, since Lottie's `to`/`ti` arrays for those never carry a 3rd element.
    bool hasSpatial = false;
    Vec3 to{}, ti{};
};

/// A Lottie property object `{a, k}`: either a static value (`animated == false`) or a sequence of
/// keyframes (`animated == true`, `keys` non-empty).
template <class T>
struct AnimatedValue {
    bool animated = false;
    T value{};
    std::vector<LottieKeyframe<T>> keys;
};

/// One layer's transform group (`ks`). See docs/design/11-lottie-mapping.md §11.3.1.
struct LottieTransform {
    AnimatedValue<Vec2> anchor;
    AnimatedValue<Vec2> position;
    AnimatedValue<Vec2> scale;
    AnimatedValue<float> posX, posY;  // Used only when splitPosition is true (`p.s == true`).
    AnimatedValue<float> rotation;    // `r`, or `rz` when present (T-47b: `rz` is `r`'s 3D alias).
    AnimatedValue<float> opacity;
    AnimatedValue<float> skew;      // `sk`
    AnimatedValue<float> skewAxis;  // `sa`
    bool splitPosition = false;

    // --- T-47b: 3D layer fields (docs/design/19-3d-space.md §19.2-§19.3), only meaningful when
    // the owning LottieLayer::ddd is true. ---
    AnimatedValue<float> rx, ry;      // `rx`, `ry` -- rotation around the local X/Y axis, degrees.
    AnimatedValue<Vec3> orientation;  // `or`.
    AnimatedValue<float> anchorZ;     // `a`'s 3rd array component (z), read as a lone float.
    AnimatedValue<Vec3> position3;    // `p` re-read as Vec3; used when !splitPosition.
    AnimatedValue<float> posZ;        // `p.z`; used when splitPosition (`p.s == true`).
};

// -------------------------------------------------------------------------------------------
// JSON parsing helpers shared by lottie_json.cpp and lottie_shapes_json.cpp (T-15 reuses T-14's
// AnimatedValue parsing instead of duplicating it). See docs/tasks/T-15.md.
// -------------------------------------------------------------------------------------------

/// Reads a JSON number as float, explicit narrowing via double (nlohmann stores integers/floats
/// distinctly; going through double covers both without a second get<T>() attempt).
inline float jnum(const nlohmann::json& j, float def = 0.0f) {
    if (j.is_number()) {
        return static_cast<float>(j.get<double>());
    }
    return def;
}

inline float jfield(const nlohmann::json& j, const char* key, float def) {
    if (j.is_object() && j.contains(key)) {
        return jnum(j.at(key), def);
    }
    return def;
}

inline int ifield(const nlohmann::json& j, const char* key, int def) {
    if (j.is_object() && j.contains(key) && j.at(key).is_number()) {
        return static_cast<int>(j.at(key).get<double>());
    }
    return def;
}

inline std::string sfield(const nlohmann::json& j, const char* key, const std::string& def) {
    if (j.is_object() && j.contains(key) && j.at(key).is_string()) {
        return j.at(key).get<std::string>();
    }
    return def;
}

/// Boolean-ish field: accepts JSON true/false or a truthy/falsy number (Lottie mixes both).
inline bool bfield(const nlohmann::json& j, const char* key, bool def) {
    if (!j.is_object() || !j.contains(key)) {
        return def;
    }
    const nlohmann::json& v = j.at(key);
    if (v.is_boolean()) {
        return v.get<bool>();
    }
    if (v.is_number()) {
        return v.get<double>() != 0.0;
    }
    return def;
}

/// Reads array component `index` (0-based) as float; non-array or short array returns `def`.
inline float arrComp(const nlohmann::json& arr, size_t index, float def = 0.0f) {
    if (!arr.is_array() || index >= arr.size()) {
        return def;
    }
    return jnum(arr[index], def);
}

/// Reads a scalar/vector/color/path value node (Lottie's `k` for a==0, or a keyframe's `s`/`e`).
template <class T>
T readValue(const nlohmann::json& j);

template <>
inline float readValue<float>(const nlohmann::json& j) {
    if (j.is_array()) {
        return arrComp(j, 0);
    }
    return jnum(j);
}

template <>
inline Vec2 readValue<Vec2>(const nlohmann::json& j) {
    if (j.is_array()) {
        return Vec2{arrComp(j, 0), arrComp(j, 1)};
    }
    return Vec2{jnum(j), 0};
}

/// T-47b: a 3D layer's position/orientation array `[x, y, z]` (or `[x, y]` when the layer only has
/// two components, per docs/tasks/T-47b.md's "p/a 若只有兩個分量 → z = 0").
template <>
inline Vec3 readValue<Vec3>(const nlohmann::json& j) {
    if (j.is_array()) {
        return Vec3{arrComp(j, 0), arrComp(j, 1), arrComp(j, 2)};
    }
    return Vec3{jnum(j), 0, 0};
}

/// Color property values (`fl.c`, `st.c`): an array `[r,g,b]` or `[r,g,b,a]`. Values are 0..1 in
/// modern Lottie; older files use 0..255, detected when any component exceeds 1.
template <>
inline Color readValue<Color>(const nlohmann::json& j) {
    if (!j.is_array()) {
        return Color{0, 0, 0, 1};
    }
    float r = arrComp(j, 0);
    float g = arrComp(j, 1);
    float b = arrComp(j, 2);
    float a = arrComp(j, 3, 1.0f);
    if (r > 1.0f || g > 1.0f || b > 1.0f || a > 1.0f) {
        r /= 255.0f;
        g /= 255.0f;
        b /= 255.0f;
        a /= 255.0f;
    }
    return Color{r, g, b, a};
}

/// A flattened gradient stop array (`g.k`): a plain array of floats, used as-is both for a static
/// value and for one keyframe's `s`/`e`.
template <>
inline std::vector<float> readValue<std::vector<float>>(const nlohmann::json& j) {
    std::vector<float> out;
    if (j.is_array()) {
        out.reserve(j.size());
        for (const nlohmann::json& e : j) {
            out.push_back(jnum(e));
        }
    }
    return out;
}

/// Reads a keyframe's `i`/`o` easing control object (`{x:[...],y:[...]}` or `{x:number,y:number}`,
/// component 0 for arrays).
struct EasingXY {
    float x = 0, y = 0;
};

inline EasingXY readEasingXY(const nlohmann::json& io) {
    if (!io.is_object()) {
        return EasingXY{};
    }
    auto readComp = [](const nlohmann::json& v) -> float {
        if (v.is_array()) {
            return arrComp(v, 0);
        }
        return jnum(v);
    };
    EasingXY out;
    if (io.contains("x")) {
        out.x = readComp(io.at("x"));
    }
    if (io.contains("y")) {
        out.y = readComp(io.at("y"));
    }
    return out;
}

/// Parses a Lottie property object `{a, k}` into an AnimatedValue<T>. See
/// docs/design/11-lottie-mapping.md §11.4 and docs/tasks/T-14.md's keyframe field rules.
template <class T>
AnimatedValue<T> parseAnimated(const nlohmann::json& propObj) {
    AnimatedValue<T> out;
    if (!propObj.is_object() || !propObj.contains("k")) {
        return out;
    }
    int a = ifield(propObj, "a", 0);
    const nlohmann::json& k = propObj.at("k");
    if (a == 0) {
        out.animated = false;
        out.value = readValue<T>(k);
        return out;
    }
    if (!k.is_array()) {
        // Malformed (a==1 without a keyframe array): degrade to a static default.
        out.animated = false;
        return out;
    }
    out.animated = true;
    out.keys.reserve(k.size());
    for (const nlohmann::json& kf : k) {
        LottieKeyframe<T> key;
        key.frame = jfield(kf, "t", 0.0f);
        if (kf.contains("s")) {
            key.value = readValue<T>(kf.at("s"));
        } else if (kf.contains("e")) {
            key.value = readValue<T>(kf.at("e"));
        } else if (!out.keys.empty()) {
            key.value = out.keys.back().value;
        }
        key.hold = bfield(kf, "h", false);
        if (kf.contains("o")) {
            EasingXY oxy = readEasingXY(kf.at("o"));
            key.ox = oxy.x;
            key.oy = oxy.y;
        }
        if (kf.contains("i")) {
            EasingXY ixy = readEasingXY(kf.at("i"));
            key.ix = ixy.x;
            key.iy = ixy.y;
        }
        key.hasSpatial =
            kf.contains("ti") && kf.contains("to") && kf.at("ti").is_array() && kf.at("to").is_array();
        if (key.hasSpatial) {
            const nlohmann::json& to = kf.at("to");
            const nlohmann::json& ti = kf.at("ti");
            // Component 2 (z) is only present for a 3D layer's position (T-47b); arrComp defaults
            // to 0 for a plain 2-element `to`/`ti` array, so this is a no-op for existing Vec2
            // properties.
            key.to = Vec3{arrComp(to, 0), arrComp(to, 1), arrComp(to, 2)};
            key.ti = Vec3{arrComp(ti, 0), arrComp(ti, 1), arrComp(ti, 2)};
        }
        out.keys.push_back(key);
    }
    return out;
}

/// T-47b: parses a Lottie property object `{a, k}` into an AnimatedValue<float> holding just array
/// component `index` (0-based) of the underlying vector value -- used for the anchor point's z
/// component (docs/design/19-3d-space.md §19.3), which the general AnimatedValue<Vec2> parse for
/// `a` ignores. Mirrors parseAnimated<T>'s keyframe field handling (easing, hold), minus spatial
/// tangents (Lottie never carries `ti`/`to` on the anchor point).
inline AnimatedValue<float> parseAnimatedComponent(const nlohmann::json& propObj, size_t index) {
    AnimatedValue<float> out;
    if (!propObj.is_object() || !propObj.contains("k")) {
        return out;
    }
    int a = ifield(propObj, "a", 0);
    const nlohmann::json& k = propObj.at("k");
    if (a == 0) {
        out.animated = false;
        out.value = arrComp(k, index, 0.0f);
        return out;
    }
    if (!k.is_array()) {
        // Malformed (a==1 without a keyframe array): degrade to a static default.
        out.animated = false;
        return out;
    }
    out.animated = true;
    out.keys.reserve(k.size());
    for (const nlohmann::json& kf : k) {
        LottieKeyframe<float> key;
        key.frame = jfield(kf, "t", 0.0f);
        if (kf.contains("s")) {
            key.value = arrComp(kf.at("s"), index, 0.0f);
        } else if (kf.contains("e")) {
            key.value = arrComp(kf.at("e"), index, 0.0f);
        } else if (!out.keys.empty()) {
            key.value = out.keys.back().value;
        }
        key.hold = bfield(kf, "h", false);
        if (kf.contains("o")) {
            EasingXY oxy = readEasingXY(kf.at("o"));
            key.ox = oxy.x;
            key.oy = oxy.y;
        }
        if (kf.contains("i")) {
            EasingXY ixy = readEasingXY(kf.at("i"));
            key.ix = ixy.x;
            key.iy = ixy.y;
        }
        out.keys.push_back(key);
    }
    return out;
}

inline Color parseHexColor(const std::string& hex) {
    std::string h = hex;
    if (!h.empty() && h[0] == '#') {
        h = h.substr(1);
    }
    auto hexByte = [&h](size_t pos) -> float {
        if (pos + 2 > h.size()) {
            return 0.0f;
        }
        int v = 0;
        for (int i = 0; i < 2; ++i) {
            char c = h[pos + static_cast<size_t>(i)];
            v <<= 4;
            if (c >= '0' && c <= '9') {
                v |= (c - '0');
            } else if (c >= 'a' && c <= 'f') {
                v |= (c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                v |= (c - 'A' + 10);
            }
        }
        return static_cast<float>(v) / 255.0f;
    };
    return Color{hexByte(0), hexByte(2), hexByte(4), 1.0f};
}

}  // namespace xt::compile

#include "lottie_shapes.h"

namespace xt::compile {

/// One `masksProperties[]` entry (T-16). `mode` is the raw Lottie mode character: 'a' Add, 's'
/// Subtract, 'i' Intersect (supported), 'n' None, 'f' Lighten, 'd' Darken, 'l' Lighten (unsupported
/// -- see docs/tasks/T-16.md). `x` (mask expansion) is read but never applied (warned if non-zero).
struct LottieMask {
    int mode = 'n';
    bool inverted = false;
    AnimatedValue<LottiePath> pt;
    AnimatedValue<float> o;
    AnimatedValue<float> x;
};

/// One `ef[]` entry (T-55, docs/design/11-lottie-mapping.md / 18-effects.md §18.9). `ty` is the
/// effect group's own outer `ty` (29 Gaussian Blur, 25 Drop Shadow; anything else is unsupported
/// and carries no parsed fields -- only `ty` is meaningful, for the build-time warning). The
/// per-kind fields below are read from the effect group's `ef[]` sub-items by fixed index (0-based,
/// matching the Lottie schema for that effect type), each sub-item's `v` being an ordinary Lottie
/// property object (`{a, k}`).
struct LottieEffect {
    int ty = -1;

    // ty:29 Gaussian Blur.
    AnimatedValue<float> blurriness;  // ef[0].v ("Blurriness").
    int blurDimensions = 1;           // ef[1].v ("Blur Dimensions"): 1 both, 2 horizontal, 3 vertical.

    // ty:25 Drop Shadow.
    AnimatedValue<Color> shadowColor;      // ef[0].v ("Shadow Color").
    AnimatedValue<float> shadowOpacity;    // ef[1].v ("Opacity"), 0..255.
    AnimatedValue<float> shadowDirection;  // ef[2].v ("Direction"), degrees, 0 = up, clockwise.
    AnimatedValue<float> shadowDistance;   // ef[3].v ("Distance").
    AnimatedValue<float> shadowSoftness;   // ef[4].v ("Softness").
};

/// One Lottie layer. Fields not applicable to `type` are left at their defaults.
struct LottieLayer {
    int type = -1;   // 0 precomp, 1 solid, 2 image, 3 null, 4 shape.
    int index = -1;  // Lottie `ind`; referenced by other layers' `parent`.
    int parent = -1;
    std::string name;
    float ip = 0, op = 0, st = 0, sr = 1;
    bool hidden = false;
    int blendMode = 0;
    int matteType = 0;     // T-16: layer's own `tt` (1 Alpha, 2 AlphaInverted, 3 Luma, 4 LumaInverted).
    int matteParent = -1;  // T-16: `tp`, or -1 when absent (falls back to the preceding array entry).
    int td = 0;            // T-16: `td` (1 = this layer is usable as a track matte source).
    LottieTransform ks;

    // T-47b: `ddd == 1` (3D layer, docs/design/19-3d-space.md §19.3).
    bool ddd = false;
    // T-228: `ao == 1` (auto-orient along the position path) -> NodeCommon::autoOrient.
    bool autoOrient = false;

    // Camera (type == 13, T-47b). `cameraHasTarget` is whether `ks.a` was present in the JSON
    // (distinct from a present-but-zero point of interest): drives IrCameraData::useTarget.
    AnimatedValue<float> cameraZoom;            // `pe`.
    AnimatedValue<Vec3> cameraPointOfInterest;  // `ks.a` re-read as Vec3.
    bool cameraHasTarget = false;

    // Solid (type == 1).
    int solidW = 0, solidH = 0;
    Color solidColor{0, 0, 0, 1};

    // Precomp (type == 0) / Image (type == 2).
    std::string refId;
    float w = 0, h = 0;  // Precomp composition size.

    // Shape (type == 4).
    std::vector<LottieShapeItem> shapes;

    // masksProperties (T-16).
    std::vector<LottieMask> masks;

    // ef[] (T-55, 18.9). Order matches JSON array order (effect stack order).
    std::vector<LottieEffect> effects;
};

/// A Lottie `assets[]` entry: either a precomp (non-empty `layers`) or an image asset (T-16:
/// `isImage`, has no `layers` and a `p` field).
struct LottieAsset {
    std::string id;
    std::vector<LottieLayer> layers;

    // Image asset (T-16).
    bool isImage = false;
    int w = 0, h = 0;
    std::string u;       // directory, concatenated verbatim with `p` for the external asset name.
    std::string p;       // filename, or a `data:...;base64,...` URI when embedded.
    bool embed = false;  // `e == 1`.
};

struct LottieMarker {
    std::string name;
    float time = 0;      // `tm`, in frames.
    float duration = 0;  // `dr`, in frames.
};

struct LottieDoc {
    int version[3] = {0, 0, 0};
    float fr = 0, ip = 0, op = 0, w = 0, h = 0;
    std::vector<LottieLayer> layers;
    std::map<std::string, LottieAsset> assets;
    std::vector<LottieMarker> markers;
};

/// Formats `layer "<name>" (index <ind>)` for diagnostic messages. See docs/tasks/T-14.md "診斷".
inline std::string lottieLayerDesc(const LottieLayer& layer) {
    return "layer \"" + layer.name + "\" (index " + std::to_string(layer.index) + ")";
}

/// Parses the Lottie JSON document into `out`. Returns false when the JSON is not valid JSON, the
/// top-level value is not an object, a required top-level field (`fr`/`ip`/`op`/`w`/`h`) is
/// missing/not a number, or shape groups nest deeper than kMaxShapeGroupDepth (T-257) -- in all
/// these cases an error has already been recorded in `sink`. May
/// throw nlohmann::json exceptions for other malformed input; the caller (parseLottie) catches
/// them.
bool parseLottieJson(std::span<const uint8_t> json, const std::string& file, LottieDoc& out,
                     DiagnosticSink& sink);

/// Maps `doc` into `scene`/`timeline` (not yet owned by an IrWriter). `writer` is only used to
/// intern strings. Returns false if any error was recorded into `sink` (in which case `scene` and
/// `timeline` are left partially built and must be discarded by the caller -- the caller only
/// commits them to the IrWriter on success).
bool buildLottieScene(const LottieDoc& doc, const LottieOptions& options, const std::string& file,
                      IrWriter& writer, DiagnosticSink& sink, SceneBuilder& scene, TimelineBuilder& timeline);

// -------------------------------------------------------------------------------------------
// Shared build-time helpers (lottie_build.cpp), reused by lottie_shapes_build.cpp instead of being
// duplicated (T-15). See docs/design/11-lottie-mapping.md §11.2, §11.4.
// -------------------------------------------------------------------------------------------

/// An affine mapping from a layer's own raw keyframe frame to the ROOT document's frame axis:
/// `apply(f) = offset + scale * f`. See docs/design/11-lottie-mapping.md §11.2.
struct TimeMap {
    float scale = 1.0f;
    float offset = 0.0f;
    float apply(float f) const { return offset + scale * f; }
};

/// T-258b (D6): the most layer instances one build creates (precomps expand once per use).
inline constexpr uint32_t kMaxLottieLayerInstances = 100000;
/// T-258b (D6): the most shape copies the compile-time repeater expansion makes in one build.
inline constexpr uint32_t kMaxLottieRepeaterCopies = 100000;

struct BuildCtx {
    const LottieDoc& doc;
    const LottieOptions& options;
    const std::string& file;
    IrWriter& writer;
    DiagnosticSink& sink;
    SceneBuilder& scene;
    TimelineBuilder& timeline;
    bool warnedPrecompClip = false;
    // T-16: Lottie asset id -> already-created AssetId, so the same image asset referenced by
    // multiple Image layers only creates one ASST entry.
    std::unordered_map<std::string, AssetId> imageAssetCache;
    // T-230 (31 §31.9): true when the shape builder feeds the native import (xt_import's
    // lottie_native.cpp) rather than an `<anim>`: rc/el/sr become parametric Shape nodes where the
    // SVG element draws the same path, a shape's fill and stroke share one node, and offset/merge
    // paths stay (disabled) Modifier nodes for the editor to bake. The `<anim>` path leaves it off.
    bool native = false;
    // T-230: per scene node, the element id the native import should give it (empty = none);
    // filled only when `native` is set.
    std::vector<std::string> nodeLabels{};
    // T-230: offset/merge paths warned once per `<anim>` build.
    bool warnedBakedModifier = false;
    // T-258b (D6): layer instances built and repeater copies made so far; past either limit the
    // build reports one error, sets `expansionExceeded` and builds nothing more.
    uint32_t layerInstances = 0;
    uint32_t repeaterCopies = 0;
    bool expansionExceeded = false;
};

/// Converts an already root-mapped frame (see TimeMap::apply) into seconds relative to the root's
/// `ip`. See docs/design/11-lottie-mapping.md §11.2.
float mapFrameToSec(const BuildCtx& ctx, TimeMap map, float rawFrame);

template <class T>
T firstOrStatic(const AnimatedValue<T>& av) {
    if (av.animated && !av.keys.empty()) {
        return av.keys.front().value;
    }
    return av.value;
}

/// Drops keyframes whose frame repeats the previous kept one (docs/tasks/T-14.md "關鍵幀時間必須
/// 嚴格遞增"), warning for each dropped one. The returned pointers stay valid as long as `av`.
template <class T>
std::vector<const LottieKeyframe<T>*> dedupKeyframes(const AnimatedValue<T>& av, BuildCtx& ctx,
                                                     const LottieLayer& layer, const char* propLabel) {
    std::vector<const LottieKeyframe<T>*> kept;
    kept.reserve(av.keys.size());
    for (const LottieKeyframe<T>& k : av.keys) {
        if (!kept.empty() && k.frame == kept.back()->frame) {
            ctx.sink.warn(ctx.file, lottieLayerDesc(layer) + ": duplicate keyframe at frame " +
                                        std::to_string(k.frame) + " for " + propLabel +
                                        "; discarding later one");
            continue;
        }
        kept.push_back(&k);
    }
    return kept;
}

/// Builds a Float Track from `av` if it is (still, after dedup) animated by more than one
/// keyframe; a single remaining keyframe is treated as a static value (no track). Returns true if
/// a track was emitted.
bool buildFloatTrack(BuildCtx& ctx, const AnimatedValue<float>& av, uint32_t target, PropId propId,
                     TimeMap map, const LottieLayer& layer, const char* propLabel, float scale = 1.0f);

/// Same as buildFloatTrack but for Vec2 properties; also carries spatial tangents (Lottie's
/// `ti`/`to`), scaled the same way as the value.
bool buildVec2Track(BuildCtx& ctx, const AnimatedValue<Vec2>& av, uint32_t target, PropId propId, TimeMap map,
                    const LottieLayer& layer, const char* propLabel, float scale = 1.0f);

/// Same as buildFloatTrack but for Color properties (T-15's Fill/Stroke color, T-55's Drop Shadow
/// color). Shared here (moved out of lottie_shapes_build.cpp's anonymous namespace, T-55) so
/// lottie_build.cpp can reuse it without duplicating the Color track-building logic.
bool buildColorTrack(BuildCtx& ctx, const AnimatedValue<Color>& av, uint32_t target, PropId propId,
                     TimeMap map, const LottieLayer& layer, const char* propLabel);

/// Builds `layer.shapes` (a Shape layer's `shapes[]` tree, T-15) as children of `contentGroupIdx`
/// (the layer's ContentGroup, see buildContentNode in lottie_build.cpp). `map` is this layer's own
/// frame mapping (same one passed to buildContentNode/buildTransformNode).
void buildShapeLayerContent(BuildCtx& ctx, const LottieLayer& layer, uint32_t contentGroupIdx, TimeMap map);

// -------------------------------------------------------------------------------------------
// T-16: masks, track matte, image assets (lottie_masks.cpp / lottie_assets.cpp). See
// docs/tasks/T-16.md.
// -------------------------------------------------------------------------------------------

/// Builds `layer.masksProperties` (T-16) as Mask nodes, inserted as the first children of
/// `contentGroupIdx` -- must be called before any of the layer's own content is appended, so the
/// Mask nodes precede it in `ctx.scene.nodes` (sibling/z order == array order, see
/// docs/design/03-scene-and-properties.md §3.7). `map` is this layer's own frame mapping.
void buildLayerMasks(BuildCtx& ctx, const LottieLayer& layer, uint32_t contentGroupIdx, TimeMap map);

/// Appends one Mask scene node (empty subtree; the caller fills it in) as the next child of
/// `parentIdx`, with `mode` = matteType mapped per docs/tasks/T-16.md (1 Alpha, 2 AlphaInverted,
/// 3 Luma, 4 LumaInverted). `matteType` must be in [1,4].
uint32_t appendTrackMatteMaskNode(BuildCtx& ctx, uint32_t parentIdx, int matteType, StringId name);

/// Resolves (creating and caching on first use) the AssetId for the Lottie image asset `assetId`
/// (a key into `ctx.doc.assets`). Returns kInvalidAsset and records an error into ctx.sink
/// (referencing `layer`) if `assetId` does not name an image asset.
AssetId resolveImageAsset(BuildCtx& ctx, const std::string& assetId, const LottieLayer& layer);

}  // namespace xt::compile
