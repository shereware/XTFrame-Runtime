#include <cmath>

#include "xt/geom.h"

/// Trim path (§3.8): cuts an arc-length range out of raw verbs/points, treating every subpath as
/// part of one continuous path.
/// See docs/design/03-scene-and-properties.md §3.8

namespace xt {

namespace {

Vec2 lerp(Vec2 a, Vec2 b, float t) { return a + (b - a) * t; }

float lengthOf(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }

// One logical drawing edge (Line-shaped, control points unused when !isCubic) spanning
// [cumStart, cumStart+length] in the path's total arc length. `subpathIndex` distinguishes
// contiguous pieces of the same subpath from a jump across a Move.
struct Edge {
    bool isCubic;
    Vec2 p0, p1, p2, p3;  // for a line, only p0/p3 are used
    float length;
    float cumStart;
    size_t subpathIndex;
};

}  // namespace

void trimPath(std::span<const PathVerb> verbs, std::span<const float> points, float start, float end,
              float offset, std::vector<PathVerb>& outVerbs, std::vector<float>& outPoints) {
    // Inputs are clamped to [0, 1]; a reversed interval is swapped (matches lottie-web).
    start = start < 0.0f ? 0.0f : (start > 1.0f ? 1.0f : start);
    end = end < 0.0f ? 0.0f : (end > 1.0f ? 1.0f : end);
    if (end < start) {
        const float t = start;
        start = end;
        end = t;
    }
    if (start == end) {
        return;
    }
    if ((end - start) >= 1.0f) {
        outVerbs.assign(verbs.begin(), verbs.end());
        outPoints.assign(points.begin(), points.end());
        return;
    }

    // Build the continuous edge list across all subpaths.
    std::vector<Edge> edges;
    size_t pointCursor = 0;
    Vec2 cur{0, 0};
    Vec2 subpathStart{0, 0};
    size_t subpathIndex = static_cast<size_t>(-1);
    bool haveSubpath = false;
    float cumLen = 0.0f;

    for (PathVerb verb : verbs) {
        switch (verb) {
            case PathVerb::Move: {
                float x = points[pointCursor];
                float y = points[pointCursor + 1];
                pointCursor += 2;
                cur = Vec2{x, y};
                subpathStart = cur;
                ++subpathIndex;
                haveSubpath = true;
                break;
            }
            case PathVerb::Line: {
                float x = points[pointCursor];
                float y = points[pointCursor + 1];
                pointCursor += 2;
                Vec2 lineEnd = Vec2{x, y};
                float len = lengthOf(lineEnd - cur);
                edges.push_back(Edge{false, cur, {}, {}, lineEnd, len, cumLen, subpathIndex});
                cumLen += len;
                cur = lineEnd;
                break;
            }
            case PathVerb::Cubic: {
                Vec2 c1{points[pointCursor], points[pointCursor + 1]};
                Vec2 c2{points[pointCursor + 2], points[pointCursor + 3]};
                Vec2 p3{points[pointCursor + 4], points[pointCursor + 5]};
                pointCursor += 6;
                geom::CubicTable table = geom::buildCubicTable(cur, c1, c2, p3);
                edges.push_back(Edge{true, cur, c1, c2, p3, table.length, cumLen, subpathIndex});
                cumLen += table.length;
                cur = p3;
                break;
            }
            case PathVerb::Close: {
                if (haveSubpath) {
                    float len = lengthOf(subpathStart - cur);
                    edges.push_back(Edge{false, cur, {}, {}, subpathStart, len, cumLen, subpathIndex});
                    cumLen += len;
                    cur = subpathStart;
                }
                break;
            }
        }
    }

    float totalLen = cumLen;
    if (!(totalLen > 0.0f) || edges.empty()) {
        return;  // degenerate path: conservative empty output (uncovered by spec)
    }

    // Effective normalized interval, reduced into [0, 1).
    float a = start + offset;
    float len = end - start;  // 0 <= len < 1 here
    float a0 = a - std::floor(a);
    float b0 = a0 + len;

    struct Range {
        float s0, s1;  // arc-length units
    };
    std::vector<Range> ranges;
    if (b0 <= 1.0f) {
        ranges.push_back(Range{a0 * totalLen, b0 * totalLen});
    } else {
        ranges.push_back(Range{a0 * totalLen, totalLen});
        ranges.push_back(Range{0.0f, (b0 - 1.0f) * totalLen});
    }

    for (const Range& range : ranges) {
        bool started = false;
        size_t lastSubpath = static_cast<size_t>(-1);
        for (const Edge& edge : edges) {
            float edgeEnd = edge.cumStart + edge.length;
            if (edgeEnd <= range.s0 || edge.cumStart >= range.s1) {
                continue;
            }
            float localS = range.s0 > edge.cumStart ? (range.s0 - edge.cumStart) : 0.0f;
            float localE = range.s1 < edgeEnd ? (range.s1 - edge.cumStart) : edge.length;
            if (localE <= localS && edge.length > 0.0f) {
                continue;  // zero-length overlap, skip
            }
            float tA = edge.length > 1e-9f ? (localS / edge.length) : 0.0f;
            float tB = edge.length > 1e-9f ? (localE / edge.length) : 1.0f;

            bool newFragment = !started || edge.subpathIndex != lastSubpath;

            if (edge.isCubic) {
                geom::CubicTable table = geom::buildCubicTable(edge.p0, edge.p1, edge.p2, edge.p3);
                float realTA = geom::lengthFractionToT(table, tA);
                float realTB = geom::lengthFractionToT(table, tB);
                Vec2 sub[4];
                geom::subCubic(edge.p0, edge.p1, edge.p2, edge.p3, realTA, realTB, sub);
                if (newFragment) {
                    outVerbs.push_back(PathVerb::Move);
                    outPoints.push_back(sub[0].x);
                    outPoints.push_back(sub[0].y);
                }
                outVerbs.push_back(PathVerb::Cubic);
                outPoints.push_back(sub[1].x);
                outPoints.push_back(sub[1].y);
                outPoints.push_back(sub[2].x);
                outPoints.push_back(sub[2].y);
                outPoints.push_back(sub[3].x);
                outPoints.push_back(sub[3].y);
            } else {
                Vec2 pA = lerp(edge.p0, edge.p3, tA);
                Vec2 pB = lerp(edge.p0, edge.p3, tB);
                if (newFragment) {
                    outVerbs.push_back(PathVerb::Move);
                    outPoints.push_back(pA.x);
                    outPoints.push_back(pA.y);
                }
                outVerbs.push_back(PathVerb::Line);
                outPoints.push_back(pB.x);
                outPoints.push_back(pB.y);
            }
            started = true;
            lastSubpath = edge.subpathIndex;
        }
    }
}

}  // namespace xt
