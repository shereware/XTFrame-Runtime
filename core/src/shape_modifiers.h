#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "xt/node.h"
#include "xt/types.h"

/// Runtime shape modifiers and parametric shapes (docs/design/31-vector-authoring.md §31.6.1,
/// T-225): pure functions over raw path verbs/points (the §3.4 layout). Every function appends its
/// output to caller-provided vectors (it never clears them) and allocates nothing else that
/// outlives the call. The Painter drives them (collect-apply-draw, painter.cpp).

namespace xt::modifiers {

/// One input path: raw verbs and their point operands.
struct PathSpan {
    std::span<const PathVerb> verbs;  ///< Path verbs.
    std::span<const float> points;    ///< Point operands (2 per Move/Line, 6 per Cubic).
};

/// Where one output path landed in the output vectors.
struct PathRange {
    uint32_t verbOffset = 0;   ///< First verb in the output verb vector.
    uint32_t verbCount = 0;    ///< Verb count.
    uint32_t pointOffset = 0;  ///< First float in the output point vector.
    uint32_t pointCount = 0;   ///< Float count.
};

/// Trim modes (ModifierNodeData::mode of a Trim).
inline constexpr uint8_t kTrimParallel = 0;  ///< Each path trimmed on its own (Lottie m = 1).
inline constexpr uint8_t kTrimSerial = 1;    ///< All paths as one length, in order (Lottie m = 2).

/// §31.6.1 trim: `start`/`end` are clamped to [0, 1], `offsetDeg` adds (offsetDeg mod 360) / 360,
/// a reversed interval is swapped; an interval past 1 is split into [s, 1] + [0, e - 1], the
/// second piece continuing the first without a break when the path is one closed contour. Arc
/// length per cubic from the 16-segment CubicTable, cuts by de Casteljau. kTrimParallel trims each
/// path over its own length (its contours chained into one length); kTrimSerial chains every path
/// in order. Appends one output path per input path (possibly empty) and one PathRange each.
void trimPaths(std::span<const PathSpan> paths, float start, float end, float offsetDeg, uint8_t mode,
               std::vector<PathVerb>& outVerbs, std::vector<float>& outPoints,
               std::vector<PathRange>& outRanges);

/// Most copies a Repeat modifier makes (§31.6.1 repeat).
inline constexpr uint32_t kMaxRepeatCopies = 1024;
/// Most points a parameter generates (T-258a): star / polygon `points`, zigzag `ridges` and wiggle
/// `detail` per segment, twist pieces per segment. A count from a file or a binding is clamped to
/// it before it sizes anything; a non-finite one leaves the path unmodified (a star draws nothing).
inline constexpr uint32_t kMaxGeneratedPoints = 1024;
/// Largest |angle| a Twist modifier turns, degrees (T-258a).
inline constexpr float kMaxTwistDegrees = 36000.0f;

/// One repeat copy (§31.6.1 repeat): its transform and opacity.
struct RepeatCopy {
    Mat2x3 m;            ///< T(t*p + a) * R(t*r) * S(s^t) * T(-a), t = offset + index.
    float alpha = 1;     ///< so + (eo - so) * index / n.
    uint32_t index = 0;  ///< Copy index (0 = the first copy).
};

/// §31.6.1 repeat: the copies of a Repeat modifier in copy order (copy 0 first); copies =
/// round(clamp(params[0], 0, 1024)); copies with opacity <= 0 are left out.
void repeatCopies(const ModifierNodeData& repeat, std::vector<RepeatCopy>& out);

/// True when the Repeat modifier draws copy 0 on top (`order="below"`, mode bit 0 == 0).
inline bool repeatCopyZeroOnTop(const ModifierNodeData& repeat) { return (repeat.mode & 1u) == 0; }

/// §31.6.1 round: every corner vertex (both handles zero, open end points excluded) is cut back by
/// min(radius, L / 2) along the arc of each adjacent segment and replaced by two vertices joined by
/// a cubic whose handles run along the cut tangents, 0.5523 x the cut distance long.
void roundCorners(std::span<const PathVerb> verbs, std::span<const float> points, float radius,
                  std::vector<PathVerb>& outVerbs, std::vector<float>& outPoints);

/// §31.6.1 pucker: per contour, C = mean of its vertices, a = clamp(amount / 100, -1, 1);
/// v' = lerp(v, C, a), handles (absolute) h' = lerp(h, C, -a).
void pucker(std::span<const PathVerb> verbs, std::span<const float> points, float amount,
            std::vector<PathVerb>& outVerbs, std::vector<float>& outPoints);

/// §31.6.1 zigzag: round(ridges) points per segment at arc fractions (k + 1) / (ridges + 1); every
/// point (original vertices included) moved +-size along its normal, alternating; open end points
/// do not wrap; a closed contour with an odd point count leaves its last point in place. Smooth
/// mode gives each point tangent handles of segment chord / (2 (ridges + 1)).
void zigzag(std::span<const PathVerb> verbs, std::span<const float> points, float size, float ridges,
            bool smooth, std::vector<PathVerb>& outVerbs, std::vector<float>& outPoints);

/// §31.6.1 twist: every segment split into max(1, ceil(|angle| / 15)) pieces (a line becomes a
/// cubic with 1/3 handles), then each vertex rotated about `center` by angle * (1 - r / R) (R =
/// farthest vertex distance) and its handles rotated with it.
void twist(std::span<const PathVerb> verbs, std::span<const float> points, float angleDeg, Vec2 center,
           std::vector<PathVerb>& outVerbs, std::vector<float>& outPoints);

/// Wiggle parameters (ModifierNodeData::params of a Wiggle).
struct WiggleParams {
    float size = 0;           ///< Displacement amplitude.
    float detail = 0;         ///< Points inserted per segment (rounded, >= 0).
    uint32_t seed = 0;        ///< Noise seed.
    float speed = 0;          ///< Noise cycles per second.
    float correlation = 0;    ///< 0 = independent points, 1 = every point moves together.
    float temporalPhase = 0;  ///< Degrees.
    float spatialPhase = 0;   ///< Degrees.
    bool smooth = false;      ///< mode 1: tangent handles of neighbour distance / 3.
};

/// §31.6.1 wiggle: each segment split into round(detail) + 1 equal-arc pieces (original vertices
/// kept); point k moved by size * (noise2(x, y), noise2(x + K, y)), x = u_k (1 - correlation) 4 +
/// spatialPhase / 360 + 7 * `ordinal` (u_k = the point's arc fraction along its contour, `ordinal`
/// = the contour's index across the modifier's scope), y = time * speed + temporalPhase / 360.
/// Pure: the same inputs give the same output. `ordinal` is advanced past this path's contours.
void wiggle(std::span<const PathVerb> verbs, std::span<const float> points, const WiggleParams& params,
            float time, uint32_t& ordinal, std::vector<PathVerb>& outVerbs, std::vector<float>& outPoints);

/// §31.6.1 parametric shapes: Rect [x, y, w, h, rx, ry] (0.5522847498 corner cubics, radii clamped
/// to half the size, none when either is <= 0), Ellipse [cx, cy, rx, ry] (4 cubics), Star [cx, cy,
/// points, r, r2, rotation, round, round2] (as parse_svg.cpp buildStar: first outer point up,
/// rotation clockwise degrees, rounded corners of 0.5523 cubics). Degenerate parameters append
/// nothing.
void buildShape(ShapeKind kind, const float* params, std::vector<PathVerb>& outVerbs,
                std::vector<float>& outPoints);

/// Local AABB of buildShape()'s output (without stroke), from the parameters alone. False when the
/// shape is degenerate (buildShape() would append nothing).
bool shapeBounds(ShapeKind kind, const float* params, Rect& out);

/// Number of on-curve points (Move/Line/Cubic end points) of a path.
uint32_t vertexCount(std::span<const PathVerb> verbs);

}  // namespace xt::modifiers
