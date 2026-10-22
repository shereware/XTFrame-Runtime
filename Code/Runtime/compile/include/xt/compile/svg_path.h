#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "xt/types.h"

/// SVG `<path d>` geometry: the `d` mini-language (M/L/H/V/C/S/Q/T/A/Z, absolute and relative) and
/// the elliptical-arc -> cubic conversion. See Docs/design/12-compiler.md §12.2.1,
/// Docs/tasks/T-110.md.

namespace xt::compile {

/// One generated path. `verbs`/`points` use the same encoding as `IrPathData`'s verb/point pools
/// (Docs/design/03-scene-and-properties.md §3.4): Move/Line consume 2 floats, Cubic 6, Close 0.
struct SvgPath {
    /// PathVerb values, one byte each.
    std::vector<uint8_t> verbs;
    /// Point operands, two floats per point.
    std::vector<float> points;

    /// Starts a new subpath at `p`.
    void moveTo(Vec2 p);
    /// Appends a line segment to `p`.
    void lineTo(Vec2 p);
    /// Appends a cubic segment with control points `c1`/`c2` ending at `p`.
    void cubicTo(Vec2 c1, Vec2 c2, Vec2 p);
    /// Closes the current subpath.
    void close();
    /// True when nothing has been appended yet.
    bool empty() const { return verbs.empty(); }
};

/// Appends the elliptical arc described by SVG's `A`/`a` end-point parameterization, split into
/// cubic segments of at most 90 degrees each (SVG 1.1 implementation notes F.6.5/F.6.6). A zero
/// radius, or `start == end`, degenerates to a straight line.
void appendSvgArc(SvgPath& path, Vec2 start, float rx, float ry, float xAxisRotationDeg, bool largeArc,
                  bool sweep, Vec2 end);

/// Parses an SVG `d` attribute into `out`. Returns false on a malformed command or argument list;
/// whatever was parsed before the failure is kept in `out`.
bool parseSvgPathData(std::string_view d, SvgPath& out);

}  // namespace xt::compile
